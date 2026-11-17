#include "world/npc.h"

#include <windows.h>

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
constexpr uint32_t kMaxSafeSpawned = 800;          // see npc_on_present
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
  // With [world] npc_units the game froze (every thread waiting) once ~1000-1100 characters were
  // spawned at once: each animated character's Kino instance takes a TLS index and the process runs
  // out of them (see core/tlsext.h). Without [world] tls_extend keep the budget below that whatever
  // npc_density says (the quality multiplier is at most 1.2, so 800 raw -> at most 960 spawned).
  if (!tlsext_active() && want > kMaxSafeSpawned && want > p.max_orig) want = std::max(kMaxSafeSpawned, p.max_orig);
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
    ACO_LOG("npc: %u spawned, maxSpawned %u", spawned_count(g_mgr), max_spawned(s));
  }
}

} // namespace aco
