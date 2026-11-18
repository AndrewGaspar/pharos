#include "world/npc.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <cstring>

#include "core/config.h"
#include "core/log.h"
#include "core/tlsext.h"

namespace aco {

namespace {

// v1.62 layout. Game global -> World -> spawning world component -> SpawningManager -> SpawnSettings.
constexpr uintptr_t kGameGlobalRva = 0x4c27f18;      // [rva] -> game object; +0xc0 = World
constexpr uintptr_t kWorldOff = 0xc0;
constexpr uintptr_t kSpawnCompOff = 0x13c0;          // World + 0x13c0 -> component; +0x38 = SpawningManager
constexpr uintptr_t kSpawnMgrOff = 0x38;
constexpr uintptr_t kSettingsOff = 0x10;             // SpawningManager + 0x10 -> SpawnSettings
constexpr uintptr_t kSpawnMgrVtRva = 0x363beb0;
constexpr uintptr_t kSpawnSettingsVtRva = 0x363cc80;
constexpr uintptr_t kMaxSpawnedOff = 0x38;           // uint32, 200 in the shipped data
constexpr uintptr_t kCapMultiplierRva = 0x12559c0; // float (SpawnSettings*): quality multiplier of the cap
// The three readers of maxSpawned (each: call cap multiplier 0x12559c0; mov eax/ecx, [reg+0x38]).
constexpr uintptr_t kReaderRvas[] = {0x1255a9e, 0x1268f21, 0x1259092};

bool readable(const void* p, size_t n) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
    return false;
  return reinterpret_cast<uintptr_t>(p) + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

uintptr_t load(uintptr_t addr) {
  if (!addr || !readable(reinterpret_cast<void*>(addr), 8)) return 0;
  uintptr_t v;
  memcpy(&v, reinterpret_cast<void*>(addr), 8);
  return v;
}

enum class State { Unchecked, Ok, Unsupported };
State g_state = State::Unchecked;
uintptr_t g_base = 0;

bool verify_code() {
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
  if (nt->OptionalHeader.SizeOfImage <= kGameGlobalRva + 8) return false;
  // Each reader: 8b 4x 38 (mov e?x, [r?? + 0x38]), right after the multiplier call.
  for (uintptr_t rva : kReaderRvas) {
    const uint8_t* p = base + rva;
    if (!readable(p, 4)) return false;
    bool ok = (p[0] == 0x8b && p[2] == 0x38) || (p[0] == 0x41 && p[1] == 0x8b && p[3] == 0x38);
    if (!ok) return false;
  }
  g_base = reinterpret_cast<uintptr_t>(base);
  return true;
}

uintptr_t g_mgr = 0;

// Live spawned-entity count, as the game computes it for the cap check (sum over the manager's
// pool list at +0x58: {entries, count << 17}, 0x28-byte entries with the count at +0x10).
uint32_t spawned_count(uintptr_t mgr) {
  uintptr_t list = load(mgr + 0x58);
  if (!list || !readable(reinterpret_cast<void*>(list), 12)) return 0;
  uintptr_t arr = load(list);
  uint32_t n = *reinterpret_cast<uint32_t*>(list + 8) >> 17;
  if (!arr || n > 64 || !readable(reinterpret_cast<void*>(arr), n * 0x28)) return 0;
  uint32_t total = 0;
  for (uint32_t i = 0; i < n; ++i) total += *reinterpret_cast<uint32_t*>(arr + i * 0x28 + 0x10);
  return total;
}

// Returns the live SpawnSettings object, or 0 (no world loaded / unexpected layout).
uintptr_t find_settings() {
  uintptr_t game = load(g_base + kGameGlobalRva);
  uintptr_t world = load(game + kWorldOff);
  uintptr_t comp = load(world + kSpawnCompOff);
  uintptr_t mgr = load(comp + kSpawnMgrOff);
  if (!mgr || load(mgr) != g_base + kSpawnMgrVtRva) return 0;
  uintptr_t settings = load(mgr + kSettingsOff);
  if (!settings || load(settings) != g_base + kSpawnSettingsVtRva) return 0;
  if (!readable(reinterpret_cast<void*>(settings + kMaxSpawnedOff), 4)) return 0;
  g_mgr = mgr;
  return settings;
}

// The SpawningManager switches between SpawnSettings objects (e.g. 200 and 300 in Alexandria), so
// originals are kept per object: re-reading "the original" from an object we already patched
// compounded the factor (seen: 300 -> 900 -> 9000).
struct Patched {
  uint32_t max_orig = 0, max_written = 0;  // maxSpawned before us / what we wrote (0 = nothing)
  float mul_orig = 1.0f, mul_written = 0;  // +0x70 likewise
};
std::unordered_map<uintptr_t, Patched> g_patched;
uintptr_t g_settings = 0;     // object we last looked at
float g_applied = 1.0f;

uint32_t& max_spawned(uintptr_t s) { return *reinterpret_cast<uint32_t*>(s + kMaxSpawnedOff); }

// ---- npc_distance ----------------------------------------------------------------------------
// SpawnSettings+0x70: spawn-distance multiplier (1.0). The MetaAI scales every objective's
// activation range by it; the spawning source scales a candidate's spawn distance by it only when
// the candidate is inside the camera frustum. Most ambient spawn templates are SphereStrategyParams
// data objects (spawn / despawn distance at +0x2c / +0x30, 80 / 82 m), streamed with the world, so
// a background thread finds them by vtable every few seconds and scales those two fields.
constexpr uintptr_t kDistanceMulOff = 0x70;
constexpr uintptr_t kSphereVtRva = 0x37a48f8;
constexpr uintptr_t kSphereSpawnOff = 0x2c, kSphereDespawnOff = 0x30, kSphereKindOff = 0x28;

uintptr_t g_dist_settings = 0;
float g_dist_original = 0;      // SpawnSettings+0x70 before we touched it
std::atomic<float> g_dist_k{1.0f};
HANDLE g_scan_thread = nullptr;

struct Sphere { float spawn, despawn, k; };       // originals + factor applied
std::unordered_map<uintptr_t, Sphere> g_spheres;  // scanner thread only

bool rpm(uintptr_t a, void* out, size_t n) {
  SIZE_T got = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), out, n, &got) && got == n;
}
bool wpm(uintptr_t a, const void* in, size_t n) {
  SIZE_T put = 0;
  return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), in, n, &put) && put == n;
}

// One pass over committed private read-write memory: (re)scale every SphereStrategyParams found.
// All accesses go through Read/WriteProcessMemory, so memory the game frees or decommits meanwhile
// just fails the call instead of faulting.
void scan_spheres(float k) {
  const uint64_t vt = g_base + kSphereVtRva;
  std::vector<uint8_t> buf(4 << 20);
  uintptr_t addr = 0x10000;
  MEMORY_BASIC_INFORMATION mbi{};
  unsigned found = 0, written = 0;
  while (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) {
    uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = start + mbi.RegionSize;
    if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
      for (uintptr_t c = start; c < end; c += buf.size()) {
        size_t n = std::min<uintptr_t>(buf.size(), end - c);
        if (!rpm(c, buf.data(), n)) continue;
        for (size_t o = 0; o + 0x38 <= n; o += 8) {
          uint64_t q;
          memcpy(&q, buf.data() + o, 8);
          if (q != vt) continue;
          uint32_t kind;
          float sp, de;
          memcpy(&kind, buf.data() + o + kSphereKindOff, 4);
          memcpy(&sp, buf.data() + o + kSphereSpawnOff, 4);
          memcpy(&de, buf.data() + o + kSphereDespawnOff, 4);
          if (kind != 4 || !(sp > 0 && sp < 20000) || !(de > 0 && de < 20000)) continue;
          ++found;
          uintptr_t obj = c + o;
          auto it = g_spheres.find(obj);
          // A tracked object still holding what we wrote keeps its originals; anything else is a
          // new (or reloaded) object whose current values are the originals.
          if (it == g_spheres.end() || sp != it->second.spawn * it->second.k || de != it->second.despawn * it->second.k)
            it = g_spheres.insert_or_assign(obj, Sphere{sp, de, 1.0f}).first;
          Sphere& s = it->second;
          if (s.k == k) continue;
          float v[2] = {s.spawn * k, s.despawn * k};
          uint64_t check = 0;
          if (rpm(obj, &check, 8) && check == vt && wpm(obj + kSphereSpawnOff, v, 8)) {
            s.k = k;
            ++written;
          }
        }
      }
    }
    if (end <= addr) break;
    addr = end;
    if (addr >= 0x7fffffff0000ull) break;
  }
  if (g_spheres.size() > 200000) g_spheres.clear();  // stale entries from streamed-out data
  static unsigned last_found = 0;
  if (written || found != last_found) ACO_LOG("npc: spawn templates: %u found, %u rescaled (x%g)", found, written, k);
  last_found = found;
}

DWORD WINAPI scan_thread(void*) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
  float applied = 1.0f;
  for (;;) {
    float k = g_dist_k.load();
    if (k != 1.0f || applied != 1.0f) {  // keep scanning while active, plus one pass to restore
      scan_spheres(k);
      applied = k;
    }
    Sleep(k != 1.0f ? 10000 : 2000);
  }
}

void apply_distance(uintptr_t s, Patched& p, float k) {
  float* mul = reinterpret_cast<float*>(s + kDistanceMulOff);
  if (p.mul_written != 0 && *mul != p.mul_written) {  // rewritten by the game (data reload)
    p.mul_orig = *mul;
    p.mul_written = 0;
  }
  g_dist_settings = s;
  g_dist_original = p.mul_orig;
  float want = p.mul_orig * k;
  if (*mul != want) *mul = want;
  p.mul_written = want == p.mul_orig ? 0 : want;
  if (k != g_dist_k.load()) {
    ACO_LOG("npc: npc_distance=%g -> spawn distance multiplier %g (game %g)", k, want, g_dist_original);
    g_dist_k = k;
  }
  if (k != 1.0f && !g_scan_thread) g_scan_thread = CreateThread(nullptr, 0, scan_thread, nullptr, 0, nullptr);
}

// TLS slot use of the process (PEB TlsBitmapBits at +0x80: 64 slots, TlsExpansionBitmapBits at
// +0x240: 1024). The engine's RW locks each take a TlsAlloc slot; when all 1088 are used the next
// lock gets index -1 and deadlocks its first writer: the freeze with ~1000+ spawned characters.
unsigned tls_used() {
  auto* peb = reinterpret_cast<const uint8_t*>(__readgsqword(0x60));
  unsigned used = 0;
  for (int i = 0; i < 2; ++i) used += __builtin_popcount(*reinterpret_cast<const uint32_t*>(peb + 0x80 + 4 * i));
  for (int i = 0; i < 32; ++i) used += __builtin_popcount(*reinterpret_cast<const uint32_t*>(peb + 0x240 + 4 * i));
  return used;
}
constexpr unsigned kTlsSlots = 64 + 1024;

// Free host memory (MB). Wine fills ullAvailPhys from the host's /proc/meminfo.
uint64_t avail_mb() {
  MEMORYSTATUSEX m{};
  m.dwLength = sizeof(m);
  return GlobalMemoryStatusEx(&m) ? m.ullAvailPhys >> 20 : 0;
}

// The game's working set (MB), through kernel32!K32GetProcessMemoryInfo (no psapi import).
uint64_t working_set_mb() {
  struct Pmc { DWORD cb, faults; SIZE_T peak_ws, ws, a, b, c, d, pagefile, peak_pagefile; } pmc{};
  using Fn = BOOL(WINAPI*)(HANDLE, Pmc*, DWORD);
  static Fn fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo"));
  pmc.cb = sizeof(pmc);
  return fn && fn(GetCurrentProcess(), &pmc, sizeof(pmc)) ? pmc.ws >> 20 : 0;
}

// Distance from the render camera to the player entity (-1 if unknown). On foot the camera is 2-5 m
// from the player; with Senu (the eagle) it is 20+ m above and drifts hundreds of metres away, and
// the spawning follows the camera (docs/ENGINE_NOTES.md, "Senu").
constexpr uintptr_t kCameraRva = 0x49e86b8;      // [rva] -> render camera, position at +0x130
constexpr uintptr_t kPlayerRootRva = 0x4c23c40;  // [[rva]] -> player entity, position at +0x50
constexpr uintptr_t kEntityVtRva = 0x365e460;
float camera_player_distance() {
  uintptr_t c = load(g_base + kCameraRva);
  uintptr_t e = load(load(g_base + kPlayerRootRva));
  if (!c || !e || !readable(reinterpret_cast<void*>(c + 0x130), 12) || !readable(reinterpret_cast<void*>(e), 0x60) ||
      load(e) != g_base + kEntityVtRva)
    return -1;
  const float* cp = reinterpret_cast<const float*>(c + 0x130);
  const float* pp = reinterpret_cast<const float*>(e + 0x50);
  float dx = cp[0] - pp[0], dy = cp[1] - pp[1], dz = cp[2] - pp[2];
  float d = std::sqrt(dx * dx + dy * dy + dz * dz);
  return std::isfinite(d) ? d : -1;
}
unsigned g_tls_min_free = kTlsSlots;

} // namespace

void npc_on_present() {
  static ULONGLONG last = 0;
  ULONGLONG now = GetTickCount64();
  if (now - last < 1000) return;
  last = now;

  const Config& cfg = config();
  float k = cfg.world_npc_density;
  float kd = cfg.world_npc_distance;
  if (g_state == State::Unchecked) {
    if (k == 1.0f && kd == 1.0f) return;  // don't even look unless asked to
    g_state = verify_code() ? State::Ok : State::Unsupported;
    ACO_LOG("npc: spawn budget code %s", g_state == State::Ok ? "found (v1.62 layout)" : "not found; npc settings disabled");
  }
  if (g_state != State::Ok) return;

  uintptr_t s = find_settings();
  if (!s) return;  // menus / loading
  auto it = g_patched.find(s);
  if (it == g_patched.end()) {  // first sight of this object (a world reload brings new ones)
    if (g_patched.size() > 64) g_patched.clear();
    Patched p;
    p.max_orig = max_spawned(s);
    p.mul_orig = *reinterpret_cast<float*>(s + kDistanceMulOff);
    it = g_patched.emplace(s, p).first;
    ACO_LOG("npc: SpawnSettings %p maxSpawned=%u", reinterpret_cast<void*>(s), p.max_orig);
  }
  Patched& p = it->second;
  g_settings = s;
  uint32_t cur = max_spawned(s);
  if (p.max_written && cur != p.max_written) {  // data was rewritten under us: take it as the new original
    p.max_orig = cur;
    p.max_written = 0;
  }
  k = std::clamp(k, 0.1f, 100.0f);
  uint32_t want = static_cast<uint32_t>(std::lround(p.max_orig * static_cast<double>(k)));
  // The game froze (every thread waiting) once ~1000 or more characters were spawned at once: each
  // animated character's Kino instance takes a TLS index and the process runs out of them (see
  // core/tlsext.h). Without [world] tls_extend keep the effective cap (maxSpawned x the quality
  // multiplier, RVA 0x12559c0, 0.75-1.2 by Environment quality) at [world] npc_spawn_ceiling
  // (default 800) or below.
  float mult = reinterpret_cast<float (*)(uintptr_t)>(g_base + kCapMultiplierRva)(s);
  if (!(mult > 0.1f && mult < 4.0f)) mult = 1.2f;  // unexpected: assume the largest shipped value
  if (!tlsext_active()) {
    uint32_t ceiling = static_cast<uint32_t>(std::clamp(cfg.world_npc_spawn_ceiling, 100.0f, 5000.0f) / mult);
    if (want > ceiling && want > p.max_orig) want = std::max(ceiling, p.max_orig);
  }
  // Memory: the game's memory grows with the most characters ever spawned at once (~0.9 MB each at
  // 2560x1600; despawning does not give it back) and stalls for seconds when the host runs short
  // (a Senu flight over Alexandria at k=8 hitched for 25 s while the host reported memory
  // pressure). Below [world] npc_mem_floor MB free, hold the cap near the most characters spawned
  // so far: refilling up to that reuses the game's pooled memory; much beyond it would not. The
  // cap is that peak / 0.8: the game shortens its spawn distance from 0.8 x cap (x0.9) and again
  // from 0.9 x cap (x0.8), and that pressure shrinks the crowd well below the cap (a cap at the
  // count: 1140 -> 562; at peak / 0.9: 1240 -> 800). At most ~12% above the peak.
  // Released once 1 GB more is free again.
  static uint32_t hold = 0, peak = 0;
  static ULONGLONG last_mem = 0;
  uint32_t spawned_now = g_mgr ? spawned_count(g_mgr) : 0;
  peak = std::max(peak, spawned_now);
  if (cfg.world_npc_mem_floor > 0 && want > p.max_orig && now - last_mem >= 1000) {
    last_mem = now;
    uint64_t a = avail_mb();
    if (a && a < cfg.world_npc_mem_floor) {
      if (!hold) {
        hold = std::max(p.max_orig, static_cast<uint32_t>(std::ceil(peak / 0.8 / mult)));
        ACO_LOG("npc: host memory low (%llu MB free < npc_mem_floor %g): cap held at %u (%u spawned, peak %u)",
                (unsigned long long)a, cfg.world_npc_mem_floor, static_cast<unsigned>(hold * mult), spawned_now, peak);
      }
    } else if (hold && a > cfg.world_npc_mem_floor + 1024) {
      ACO_LOG("npc: host memory back (%llu MB free): cap released", (unsigned long long)a);
      hold = 0;
    }
  } else if (cfg.world_npc_mem_floor <= 0) {
    hold = 0;
  }
  if (hold && want > hold) want = hold;
  // Senu: with the camera away from the player the spawning follows the camera over the whole city
  // (2000 -> 3100 spawned at k=8 over Alexandria, 30-38 fps). [world] npc_senu_max limits the
  // characters spawned then; the game evicts the farthest first, so the crowd under the bird stays.
  static bool senu = false;
  if (cfg.world_npc_senu_max > 0 && want > p.max_orig) {
    float d = camera_player_distance();
    bool away = d > 18.0f;
    if (away != senu) {
      senu = away;
      ACO_LOG("npc: camera %s the player (%.0f m): %s", away ? "away from" : "back at", d,
              away ? "npc_senu_max applies" : "npc_senu_max lifted");
    }
    // / 0.8: the budget pressure starts at 0.8 x cap, so the crowd settles near the number asked
    // for (a cap of exactly 1500 gave 700-1424 over a lap of the centre)
    uint32_t lim = std::max(p.max_orig, static_cast<uint32_t>(cfg.world_npc_senu_max / 0.8f / mult));
    if (senu && want > lim) want = lim;
  } else {
    senu = false;
  }
  if (k == 1.0f) want = p.max_orig;
  if (cur != want) max_spawned(s) = want;
  p.max_written = want == p.max_orig ? 0 : want;
  if (k != g_applied) {
    ACO_LOG("npc: npc_density=%g -> maxSpawned %u (game %u)", k, want, p.max_orig);
    g_applied = k;
  }
  apply_distance(s, p, std::clamp(kd, 0.25f, 8.0f));
  static ULONGLONG last_stat = 0;
  if (now - last_stat >= 10000) {
    last_stat = now;
    unsigned tls_free = kTlsSlots - tls_used();
    g_tls_min_free = std::min(g_tls_min_free, tls_free);
    ACO_LOG("npc: %u spawned, maxSpawned %u (x%.3g quality = cap %.0f), TLS slots free %u (min %u), virtual TLS %u, "
            "game %llu MB, host free %llu MB",
            spawned_count(g_mgr), max_spawned(s), mult, max_spawned(s) * mult, tls_free, g_tls_min_free, tlsext_virtual_in_use(),
            (unsigned long long)working_set_mb(), (unsigned long long)avail_mb());
  }
}

} // namespace aco
