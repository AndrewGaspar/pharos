#include "world/npc.h"

#include <windows.h>

#include <cmath>
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

} // namespace

void npc_on_present() {
  static ULONGLONG last = 0;
  ULONGLONG now = GetTickCount64();
  if (now - last < 1000) return;
  last = now;

  const Config& cfg = config();
  float k = cfg.world_npc_density;
  if (g_state == State::Unchecked) {
    if (k == 1.0f && !g_written) return;  // don't even look unless asked to
    g_state = verify_code() ? State::Ok : State::Unsupported;
    ACO_LOG("npc: spawn budget code %s", g_state == State::Ok ? "found (v1.62 layout)" : "not found; npc_density disabled");
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
  if (k < 0.1f) k = 0.1f;
  if (k > 20.0f) k = 20.0f;
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
  static ULONGLONG last_stat = 0;
  if (now - last_stat >= 10000) {
    last_stat = now;
    ACO_LOG("npc: %u spawned, maxSpawned %u", spawned_count(g_mgr), max_spawned(s));
  }
}

} // namespace aco
