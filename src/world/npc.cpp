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

uintptr_t g_settings = 0;     // object we last patched
uint32_t g_original = 0;      // its maxSpawned before we touched it
uint32_t g_written = 0;       // what we wrote (0 = nothing)
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

void apply_distance(uintptr_t s, float k) {
  float* mul = reinterpret_cast<float*>(s + kDistanceMulOff);
  if (s != g_dist_settings) {
    g_dist_settings = s;
    g_dist_original = *mul;
  } else if (*mul != g_dist_original * g_dist_k.load()) {
    g_dist_original = *mul;  // rewritten by the game (data reload)
  }
  float want = g_dist_original * k;
  if (*mul != want) *mul = want;
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
  if (s != g_settings) {  // first sight, or the world (and its data) was reloaded
    g_settings = s;
    g_original = max_spawned(s);
    g_written = 0;
    ACO_LOG("npc: SpawnSettings %p maxSpawned=%u", reinterpret_cast<void*>(s), g_original);
  }
  uint32_t cur = max_spawned(s);
  if (g_written && cur != g_written) {  // data was rewritten under us: take it as the new original
    g_original = cur;
    g_written = 0;
  }
  k = std::clamp(k, 0.1f, 20.0f);
  uint32_t want = static_cast<uint32_t>(std::lround(g_original * static_cast<double>(k)));
  if (k == 1.0f) want = g_original;
  if (cur != want) {
    max_spawned(s) = want;
    g_written = want == g_original ? 0 : want;
  }
  if (k != g_applied) {
    ACO_LOG("npc: npc_density=%g -> maxSpawned %u (game %u)", k, want, g_original);
    g_applied = k;
  }
  apply_distance(s, std::clamp(kd, 0.25f, 8.0f));
  static ULONGLONG last_stat = 0;
  if (now - last_stat >= 10000) {
    last_stat = now;
    ACO_LOG("npc: %u spawned, maxSpawned %u", spawned_count(g_mgr), max_spawned(s));
  }
}

} // namespace aco
