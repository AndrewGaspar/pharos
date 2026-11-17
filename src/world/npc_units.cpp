// [world] npc_units: extra MetaAI units (experiment). See npc_units.h.
#include "world/npc_units.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "core/config.h"
#include "core/log.h"

namespace aco {

namespace {

// v1.62 RVAs (docs/ENGINE_NOTES.md, "What the MetaAI save stores about units").
constexpr uintptr_t kGameGlobalRva = 0x4c27f18;   // [rva] -> game; +0xc0 = World
constexpr uintptr_t kPlayerRootRva = 0x4c23c40;   // [[rva]] = player entity, position at +0x50
constexpr uintptr_t kPlayerRoot2Rva = 0x4c5d788;  // [[rva] + 0x78] = player entity (second root, world/teleport.cpp)
constexpr uintptr_t kEntityVtRva = 0x365e460;     // Entity vtable
constexpr uintptr_t kMetaAIOff = 0x1688;          // World + 0x1688 = MetaAI manager
constexpr uintptr_t kMgrVtRva = 0x377e7e8;        // MetaAI manager vtable (89 slots)
constexpr int kMgrVtSlots = 89;
constexpr int kUpdateSlot = 47;                   // +0x178: per-frame update (RVA 0x1daf5f0)
constexpr uintptr_t kUpdateRva = 0x1daf5f0;
constexpr uintptr_t kObjListOff = 0x3b8;          // manager: all virtual objectives {ptr, u32 n}
constexpr uintptr_t kSpawnerVtRva = 0x377d218;    // MetaAIVirtualObjectiveSpawner
constexpr uintptr_t kUnitVtRva = 0x377eab8;       // MetaAI virtual unit (human)
constexpr uintptr_t kNullLinkRva = 0x4871468;     // the engine's empty object link
constexpr uintptr_t kLinkTableRva = 0x3cc79d0;    // link factory context for 0x88b920
// Engine functions used by the game's own "instantiate unit of archetype A at P in spawner S"
// (RVA 0x1dab8a0, the dead-body restore at load), minus its two kill calls.
constexpr uintptr_t kSlotParamRva = 0x1d80300;    // u32 (spawner, archetype entry)
constexpr uintptr_t kMakeLinkRva = 0x88b920;      // link (ctx, 0, object, hash)
constexpr uintptr_t kReleaseLinkRva = 0x88cfd0;   // release (link)
constexpr uintptr_t kEntryAddRva = 0x1d0cee0;     // (archetype entry, unit, 0)
constexpr uintptr_t kArrayAddRva = 0x136f630;     // (spawner + 0xa0, unit, 0)
constexpr uintptr_t kRegisterRva = 0x1df55c0;     // (unit + 0x30 object, manager)
constexpr uint32_t kOwnerLinkHash = 0xcf078f5e;

// Units within this distance of the player that the MetaAI has not activated yet (activation is
// ~120 m for most) get their extras, so the new units go through the normal activation -> spawn
// path like the others.
constexpr float kRingOuter = 400.0f;
constexpr unsigned kMaxExtrasPerWorld = 60000;  // hard ceiling; [world] npc_units_max lowers it
std::atomic<unsigned> g_max_extras{kMaxExtrasPerWorld};
constexpr unsigned kMaxCreatePerStep = 96;
std::atomic<unsigned> g_rate{kMaxCreatePerStep};  // [world] npc_units_rate (diagnostics): units per 100 ms step
constexpr uint32_t kMaxUnitsPerSpawner = 16000;  // see augment_spawner

uintptr_t g_base = 0;
std::atomic<int> g_k{1};
void* g_vt_copy[kMgrVtSlots + 1];  // [0] = RTTI locator, then the slots
using UpdateFn = uintptr_t (*)(void*, uintptr_t, uintptr_t, uintptr_t);
UpdateFn g_orig_update = nullptr;

// Sim-thread state (only touched from the hooked update).
uintptr_t g_cur_mgr = 0;
std::unordered_set<uintptr_t> g_done;
std::vector<uintptr_t> g_created;  // extras of the current manager
std::atomic<bool> g_log_units{false};  // [world] npc_units_log (diagnostics): log each extra and its original
unsigned g_extras = 0, g_failed = 0;
uint32_t g_cursor = 0;
ULONGLONG g_last_step = 0;
std::atomic<unsigned> g_stat_extras{0}, g_stat_spawners{0}, g_list_n{0}, g_stat_failed{0};
std::atomic<uintptr_t> g_last_unit{0};
std::atomic<uintptr_t> g_stat_mgr{0};  // manager the sim thread is stepping (diagnostics)  // a recently created extra (diagnostics: activation state)

template <class T> T rd(uintptr_t a) { T v; memcpy(&v, reinterpret_cast<void*>(a), sizeof(T)); return v; }

bool readable(uintptr_t p, size_t n) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!p || !VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  return p + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}
uintptr_t load(uintptr_t a) { return readable(a, 8) ? rd<uintptr_t>(a) : 0; }
// [p, p + n) committed and readable, possibly over several regions (big engine arrays can straddle
// two reservations of its allocator; readable() alone then rejects them).
bool readable_span(uintptr_t p, size_t n) {
  const uintptr_t end = p + n;
  while (p < end) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || !VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    p = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
  }
  return true;
}

// Object behind a link record {object, flags/refcount, id}: the object when the flag bit is set.
uintptr_t link_object(uintptr_t link) {
  if (!readable(link, 16)) return 0;
  uint64_t o = rd<uint64_t>(link), f = rd<uint64_t>(link + 8);
  return static_cast<int64_t>(f << 32) >> 63 & o;
}

struct Params {           // creation parameters, as 0x1dab8a0 builds them on its stack (0x60 bytes)
  float pos[4];
  float dir[4];           // (1, 0, 0, 0)
  uint32_t slot, pad0;    // 0x1d80300(spawner, entry)
  uintptr_t link28;       // empty link
  uintptr_t owner;        // link to the spawner (hash 0xcf078f5e)
  uint64_t z38, z40;
  uint64_t one48;         // 1 (u16 in the game)
  uintptr_t link50;       // empty link
  uint64_t z58;
};
static_assert(sizeof(Params) == 0x60, "params layout");

template <class F> F fn(uintptr_t rva) { return reinterpret_cast<F>(g_base + rva); }
template <class F> F vfn(uintptr_t obj, uintptr_t off) { return reinterpret_cast<F>(rd<uintptr_t>(rd<uintptr_t>(obj) + off)); }

// One extra unit of the archetype entry `entry` in spawner `sp`, at `pos`. Returns the unit or 0.
uintptr_t add_unit(uintptr_t mgr, uintptr_t sp, uintptr_t entry, const float pos[4]) {
  uintptr_t arche = link_object(rd<uintptr_t>(entry));
  if (!arche || !readable(arche, 8)) return 0;
  const uintptr_t null_link = g_base + kNullLinkRva;
  Params p{};
  memcpy(p.pos, pos, 16);
  p.dir[0] = 1.0f;
  p.one48 = 1;
  p.link28 = p.link50 = null_link;
  p.slot = fn<uint32_t (*)(uintptr_t, uintptr_t)>(kSlotParamRva)(sp, entry);
  p.owner = fn<uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uint32_t)>(kMakeLinkRva)(g_base + kLinkTableRva, 0, sp, kOwnerLinkHash);
  auto release = fn<void (*)(uintptr_t)>(kReleaseLinkRva);
  uintptr_t created = vfn<uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, Params*, uint8_t)>(arche, 0xa8)(arche, mgr, arche, &p, 0);
  uintptr_t unit = 0;
  if (created && !(rd<uint8_t>(created + 0x5a) & 4)) {
    unit = created;
    fn<void (*)(uintptr_t, uintptr_t, uintptr_t)>(kEntryAddRva)(entry, created, 0);
    fn<void (*)(uintptr_t, uintptr_t, uintptr_t)>(kArrayAddRva)(sp + 0xa0, unit, 0);
    vfn<void (*)(uintptr_t, uintptr_t)>(unit, 0x228)(unit, sp);
    vfn<void (*)(uintptr_t, uintptr_t, uintptr_t)>(sp, 0x240)(sp, mgr, unit);
    if (uintptr_t o = rd<uintptr_t>(unit + 0x30)) fn<void (*)(uintptr_t, uintptr_t)>(kRegisterRva)(o, mgr);
  }
  release(p.link50);
  release(p.owner);
  release(p.link28);
  return unit;
}

// Render camera (RVA 0x49e86b8 -> +0x130 position, +0x140 unit forward), read once per step.
constexpr uintptr_t kCameraRva = 0x49e86b8;
constexpr float kViewRange = 110.0f;  // beyond this a new character is small and spawns at the rim
float g_cam[6];
bool g_cam_ok = false;

void read_camera() {
  uintptr_t c = load(g_base + kCameraRva);
  g_cam_ok = c && readable(c + 0x130, 0x20);
  if (!g_cam_ok) return;
  memcpy(g_cam, reinterpret_cast<void*>(c + 0x130), 12);
  memcpy(g_cam + 3, reinterpret_cast<void*>(c + 0x140), 12);
}

// Within kViewRange of the camera and inside a ~70 degree half-angle cone around its forward axis.
bool in_view(float x, float y, float z) {
  if (!g_cam_ok) return true;  // unknown: be conservative
  float dx = x - g_cam[0], dy = y - g_cam[1], dz = z - g_cam[2];
  float d = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (d > kViewRange) return false;
  if (d < 1.0f) return true;
  return (dx * g_cam[3] + dy * g_cam[4] + dz * g_cam[5]) / d > 0.34f;
}

// The player's position from either static root (validated like world/teleport.cpp: after some
// fast travels the first root does not lead to the entity), else the render camera's.
std::atomic<unsigned> g_skip_player{0}, g_skip_list{0}, g_cam_pos{0};
bool player_xy(float& px, float& py) {
  for (uintptr_t e : {load(load(g_base + kPlayerRootRva)), load(load(g_base + kPlayerRoot2Rva) + 0x78)}) {
    if (!readable(e, 0x60) || rd<uintptr_t>(e) != g_base + kEntityVtRva) continue;
    float x = rd<float>(e + 0x50), y = rd<float>(e + 0x54);
    if (!std::isfinite(x) || !std::isfinite(y) || std::fabs(x) > 1e5f || std::fabs(y) > 1e5f) continue;
    px = x, py = y;
    return true;
  }
  if (g_cam_ok) {
    px = g_cam[0], py = g_cam[1];
    ++g_cam_pos;
    return true;
  }
  return false;
}

// Extras for the units of spawner `sp` that are within the ring, not activated yet and not handled
// before. Units are handled one by one (big spawners have 60+ units spread over a district, some of
// them always active near the player). Returns the number of units created.
unsigned augment_spawner(uintptr_t mgr, uintptr_t sp, int k, float px, float py, unsigned budget) {
  // Spawners grow by k-1 units per original, and a spawner can be handled over several steps
  // (step budget, units deferred while in view), so this must hold k x the biggest spawner.
  constexpr uint32_t kMaxUnits = 16384;
  uintptr_t arr = rd<uintptr_t>(sp + 0xa0);
  uint32_t n = rd<uint32_t>(sp + 0xa8) >> 17;
  uintptr_t ents = rd<uintptr_t>(sp + 0xc2);
  uint32_t ne = rd<uint32_t>(sp + 0xca) >> 17;
  if (!n || n > kMaxUnits || !ne || ne > 256 || !arr || !ents) return 0;
  static uintptr_t units[kMaxUnits];  // sim thread only
  memcpy(units, reinterpret_cast<void*>(arr), n * 8);  // the array grows while we add
  unsigned made = 0;
  for (uint32_t i = 0; i < n && made < budget && g_extras < g_max_extras.load(); ++i) {
    uintptr_t u = units[i];
    if (!u || rd<uintptr_t>(u) != g_base + kUnitVtRva) continue;
    float d = std::hypot(rd<float>(u + 0x10) - px, rd<float>(u + 0x14) - py);
    if (d > kRingOuter) continue;
    // Activated units get extras too: the MetaAI activates the new unit on its own. So that no
    // character appears in view, an activated unit near the camera and in front of it is left for
    // later (the player turns or moves on); everything else is handled at once, so after a load or
    // a fast travel the whole 400 m area gets its extras within seconds.
    if (rd<uintptr_t>(u + 0x1b8) && in_view(rd<float>(u + 0x10), rd<float>(u + 0x14), rd<float>(u + 0x18))) continue;
    if (!g_done.insert(u).second) continue;   // handled before, or one of our extras
    if ((rd<uint8_t>(u + 0x5a) & 6) || (rd<uint8_t>(u + 0x90) & 0x40)) continue;  // dead / group / scripted
    uintptr_t link = rd<uintptr_t>(u + 0x38), entry = 0;
    for (uint32_t e = 0; e < ne; ++e) {
      uintptr_t c = rd<uintptr_t>(ents + 8 * e);
      if (c && rd<uintptr_t>(c) == link) { entry = c; break; }
    }
    if (!entry) continue;
    float pos[4];
    memcpy(pos, reinterpret_cast<void*>(u + 0x10), 16);
    for (int j = 1; j < k; ++j) {
      // Sanity limit only: the freezes once blamed on big spawners (and on ~7500 extras per world)
      // were TLS index exhaustion (core/tlsext.h); with [world] tls_extend k=10 ran clean with
      // spawners far above 256 units and 12.5k extras.
      if ((rd<uint32_t>(sp + 0xa8) >> 17) >= kMaxUnitsPerSpawner) break;
      float a = 2.39996f * (i * 7 + j);  // spread around the original, 1.0-2.0 m away
      float r = 1.0f + 0.25f * (j % 5);
      float q[4] = {pos[0] + r * std::cos(a), pos[1] + r * std::sin(a), pos[2], pos[3]};
      if (uintptr_t nu = add_unit(mgr, sp, entry, q)) {
        ++g_extras;
        ++made;
        g_last_unit = nu;
        g_created.push_back(nu);
        if (g_log_units) ACO_LOG("npc_units: extra %p of %p (spawner %p)", reinterpret_cast<void*>(nu), reinterpret_cast<void*>(u), reinterpret_cast<void*>(sp));
        g_done.insert(nu);
      } else {
        ++g_failed;
      }
    }
  }
  return made;
}

// Units of areas the world streams out are destroyed with their spawners, so the handled / extra
// sets would keep stale pointers (and a reused address would make a new unit look handled). Once a
// minute keep only units that still sit in a spawner, and count the live extras.
ULONGLONG g_last_prune = 0;
void prune(uintptr_t mgr) {
  uintptr_t list = rd<uintptr_t>(mgr + kObjListOff);
  uint32_t n = rd<uint32_t>(mgr + kObjListOff + 8);
  if (!n || n > 400000 || !readable_span(list, size_t(n) * 8)) return;
  std::unordered_set<uintptr_t> live;
  live.reserve(g_done.size() + 1024);
  const uintptr_t spvt = g_base + kSpawnerVtRva;
  for (uint32_t i = 0; i < n; ++i) {
    uintptr_t sp = rd<uintptr_t>(list + 8 * i);
    if (!sp || rd<uintptr_t>(sp) != spvt) continue;
    uintptr_t arr = rd<uintptr_t>(sp + 0xa0);
    uint32_t nu = rd<uint32_t>(sp + 0xa8) >> 17;
    if (!arr || nu > 16384) continue;
    for (uint32_t j = 0; j < nu; ++j) live.insert(rd<uintptr_t>(arr + 8 * j));
  }
  for (auto it = g_done.begin(); it != g_done.end();) it = live.count(*it) ? std::next(it) : g_done.erase(it);
  std::vector<uintptr_t> keep;
  keep.reserve(g_created.size());
  for (uintptr_t u : g_created) if (live.count(u)) keep.push_back(u);
  g_created.swap(keep);
  g_extras = static_cast<unsigned>(g_created.size());
}

void step(uintptr_t mgr) {
  int k = g_k.load();
  if (k <= 1) return;
  ULONGLONG now = GetTickCount64();
  if (now - g_last_step < 100) return;
  g_last_step = now;
  if (mgr != g_cur_mgr) {  // new world / manager
    g_cur_mgr = mgr;
    g_stat_mgr = mgr;
    g_stat_extras = g_stat_failed = g_stat_spawners = 0;
    g_list_n = 0;
    g_done.clear();
    g_created.clear();
    g_extras = g_failed = 0;
    g_cursor = 0;
  }
  read_camera();
  float px, py;
  if (!player_xy(px, py)) { ++g_skip_player; return; }
  // The manager's list of all virtual objectives ({ptr} at +0x3b8, plain u32 count at +0x3c0),
  // walked in slices so one update never reads more than a few thousand objects.
  uintptr_t list = rd<uintptr_t>(mgr + kObjListOff);
  uint32_t n = rd<uint32_t>(mgr + kObjListOff + 8);
  if (!n || n > 400000 || !readable_span(list, size_t(n) * 8)) { ++g_skip_list; g_list_n = n; return; }
  g_list_n = n;
  if (g_cursor >= n) g_cursor = 0;
  uint32_t end = std::min<uint32_t>(n, g_cursor + 6000);
  unsigned made = 0;
  const uintptr_t spvt = g_base + kSpawnerVtRva;
  uint32_t i = g_cursor;
  for (; i < end && made < g_rate.load() && g_extras < g_max_extras.load(); ++i) {
    uintptr_t sp = rd<uintptr_t>(list + 8 * i);
    if (!sp || rd<uintptr_t>(sp) != spvt) continue;
    made += augment_spawner(mgr, sp, k, px, py, g_rate.load() - made);
  }
  g_cursor = i;  // resume where this slice stopped
  if (now - g_last_prune > 60000) {
    g_last_prune = now;
    prune(mgr);
  }
  g_stat_extras = g_extras;
  g_stat_failed = g_failed;
  g_stat_spawners = static_cast<unsigned>(g_done.size());
}

// Test hook ([world] npc_units_kill = n, raised by one): kill the extra unit nearest to the player
// with the engine's unit kill (unit vtable +0x1c0, as 0x1dab8a0 calls it), to check what a dead
// extra unit leaves in a save. Sim thread.
std::atomic<int> g_kill_req{0};
int g_kill_done = 0;

void kill_test(uintptr_t mgr) {
  int want = g_kill_req.load();
  if (want <= g_kill_done) return;
  g_kill_done = want;
  uintptr_t pl = load(load(g_base + kPlayerRootRva));
  if (!readable(pl + 0x50, 12)) return;
  float px = rd<float>(pl + 0x50), py = rd<float>(pl + 0x54);
  uintptr_t best = 0;
  float bd = 1e9f;
  for (uintptr_t u : g_created) {
    if (!readable(u, 0x1c0) || rd<uintptr_t>(u) != g_base + kUnitVtRva || (rd<uint8_t>(u + 0x5a) & 2)) continue;
    float d = std::hypot(rd<float>(u + 0x10) - px, rd<float>(u + 0x14) - py);
    if (d < bd) { bd = d; best = u; }
  }
  if (!best) { ACO_LOG("npc_units: kill test: no live extra unit"); return; }
  vfn<void (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t)>(best, 0x1c0)(best, mgr, 0, 0, 0x14);
  ACO_LOG("npc_units: kill test: killed extra unit %p at %.1f m (%.1f %.1f %.1f), dead flag %d", reinterpret_cast<void*>(best), bd,
          rd<float>(best + 0x10), rd<float>(best + 0x14), rd<float>(best + 0x18), (rd<uint8_t>(best + 0x5a) & 2) != 0);
}

// Cost accounting (sim thread writes, render thread reads/resets for the 10 s log line).
std::atomic<uint64_t> g_t_orig{0}, g_t_ours{0}, g_t_ours_max{0}, g_calls{0};

uintptr_t update_hook(void* self, uintptr_t a, uintptr_t b, uintptr_t c) {
  LARGE_INTEGER t0, t1, t2;
  QueryPerformanceCounter(&t0);
  uintptr_t r = g_orig_update(self, a, b, c);
  QueryPerformanceCounter(&t1);
  step(reinterpret_cast<uintptr_t>(self));
  if (reinterpret_cast<uintptr_t>(self) == g_cur_mgr) kill_test(g_cur_mgr);
  QueryPerformanceCounter(&t2);
  uint64_t ours = t2.QuadPart - t1.QuadPart;
  g_t_orig += t1.QuadPart - t0.QuadPart;
  g_t_ours += ours;
  if (ours > g_t_ours_max.load()) g_t_ours_max = ours;
  ++g_calls;
  return r;
}

// Diagnostics for the freezes seen with many extra units (every thread idle, no crash dump): log
// first-chance exceptions (code, faulting RVA, thread, a few return addresses from the stack) so a
// fault swallowed by the engine's job system becomes visible. Rate-limited; never handles anything.
LONG CALLBACK log_exception(EXCEPTION_POINTERS* ep) {
  static std::atomic<int> logged{0};
  DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code == 0x406D1388 || code == 0x40010006 || code == 0x4001000A || code == DBG_PRINTEXCEPTION_C ||
      code == 0xE06D7363 /* C++ throw */ || code == 0x80000003 || code == 0x8001010D)
    return EXCEPTION_CONTINUE_SEARCH;
  if (logged.fetch_add(1) >= 40) return EXCEPTION_CONTINUE_SEARCH;
  auto rel = [](uintptr_t v) -> uintptr_t { return v >= g_base && v < g_base + 0x5000000 ? v - g_base : v; };
  uintptr_t pc = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
  uintptr_t av = ep->ExceptionRecord->NumberParameters >= 2 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
  char ra[160] = {0};
  int n = 0;
  uintptr_t sp = ep->ContextRecord->Rsp;
  for (int i = 0; i < 256 && n < 8; ++i) {
    uintptr_t v;
    if (!readable(sp + 8 * i, 8)) break;
    memcpy(&v, reinterpret_cast<void*>(sp + 8 * i), 8);
    if (v >= g_base + 0x1000 && v < g_base + 0x3500000)
      n += 0, snprintf(ra + strlen(ra), sizeof(ra) - strlen(ra), " %llx", (unsigned long long)(v - g_base)), ++n;
  }
  ACO_LOG("npc_units: exception %08lx at %s%llx (address %llx) thread %lu, stack:%s", code,
          pc >= g_base && pc < g_base + 0x5000000 ? "rva " : "", (unsigned long long)rel(pc), (unsigned long long)av,
          GetCurrentThreadId(), ra);
  return EXCEPTION_CONTINUE_SEARCH;
}

bool verify() {
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  g_base = reinterpret_cast<uintptr_t>(base);
  const uintptr_t vt = g_base + kMgrVtRva;
  if (!readable(vt - 8, (kMgrVtSlots + 1) * 8)) return false;
  if (rd<uintptr_t>(vt + 8 * kUpdateSlot) != g_base + kUpdateRva) return false;
  // 0x1dab8a0's archetype lookup (mov r11d, [rcx+0xca]; mov r10, [rcx+0xc2]) as a layout check.
  static const uint8_t sig[] = {0x44, 0x8b, 0x99, 0xca, 0x00, 0x00, 0x00};
  if (!readable(g_base + 0x1dab8ac, sizeof(sig)) || memcmp(base + 0x1dab8ac, sig, sizeof(sig)) != 0) return false;
  memcpy(g_vt_copy, reinterpret_cast<void*>(vt - 8), sizeof(g_vt_copy));
  g_orig_update = reinterpret_cast<UpdateFn>(g_vt_copy[1 + kUpdateSlot]);
  g_vt_copy[1 + kUpdateSlot] = reinterpret_cast<void*>(&update_hook);
  return true;
}

} // namespace

void npc_units_on_present() {
  static ULONGLONG last = 0;
  ULONGLONG now = GetTickCount64();
  if (now - last < 1000) return;
  last = now;
  static int state = 0;  // 0 unchecked, 1 ok, 2 unsupported
  int k = static_cast<int>(std::lround(config().world_npc_units));
  if (k < 1) k = 1;
  if (k > 16) k = 16;
  if (state == 0) {
    if (k == 1) return;
    state = verify() ? 1 : 2;
    if (state == 1) AddVectoredExceptionHandler(1, log_exception);
    ACO_LOG("npc_units: %s", state == 1 ? "MetaAI layout found (v1.62)" : "MetaAI layout not found; npc_units disabled");
  }
  if (state != 1) return;
  if (k != g_k.load()) {
    ACO_LOG("npc_units: k=%d (new spawners get %d extra unit(s) per unit; units already added stay)", k, k - 1);
    g_k = k;
  }
  if (k == 1) return;  // the hook stays installed but idle
  g_kill_req = static_cast<int>(config().world_npc_units_kill);
  g_log_units = config().world_npc_units_log != 0.0f;
  { float r = config().world_npc_units_rate; g_rate = r >= 1 ? std::min<unsigned>(static_cast<unsigned>(r), kMaxCreatePerStep) : kMaxCreatePerStep; }
  { float mx = config().world_npc_units_max; g_max_extras = mx > 0 ? std::min<unsigned>(static_cast<unsigned>(mx), kMaxExtrasPerWorld) : kMaxExtrasPerWorld; }
  uintptr_t world = load(load(g_base + kGameGlobalRva) + 0xc0);
  uintptr_t mgr = load(world + kMetaAIOff);
  if (!mgr) return;
  uintptr_t vp = load(mgr);
  if (vp == g_base + kMgrVtRva) {
    // Swap the manager's vtable pointer to our copy (data write; no code is modified).
    *reinterpret_cast<volatile uintptr_t*>(mgr) = reinterpret_cast<uintptr_t>(&g_vt_copy[1]);
    ACO_LOG("npc_units: MetaAI manager %p hooked", reinterpret_cast<void*>(mgr));
  }
  static ULONGLONG last_stat = 0;
  if (now - last_stat >= 10000) {
    last_stat = now;
    uintptr_t lu = g_last_unit.load();
    ACO_LOG("npc_units: %u live extra units (%u units handled, %u failed), objective list %u, last extra %p (activation %p)",
            g_stat_extras.load(), g_stat_spawners.load(), g_stat_failed.load(), g_list_n.load(),
            reinterpret_cast<void*>(lu), reinterpret_cast<void*>(lu && readable(lu + 0x1b8, 8) ? rd<uintptr_t>(lu + 0x1b8) : 0));
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    uint64_t calls = g_calls.exchange(0), to = g_t_orig.exchange(0), tu = g_t_ours.exchange(0), tm = g_t_ours_max.exchange(0);
    if (uintptr_t sm = g_stat_mgr.load(); sm && sm != mgr)
      ACO_LOG("npc_units: stepping manager %p, but the world's is %p", reinterpret_cast<void*>(sm), reinterpret_cast<void*>(mgr));
    unsigned sp = g_skip_player.exchange(0), sl = g_skip_list.exchange(0), cp = g_cam_pos.exchange(0);
    if (sp || sl || cp)
      ACO_LOG("npc_units: steps skipped in 10 s: %u no player position, %u objective list unreadable; %u used the camera position", sp, sl, cp);
    if (calls)
      ACO_LOG("npc_units: cost: %llu MetaAI updates in 10 s, game update %.3f ms avg, ours %.3f ms avg / %.3f ms max",
              (unsigned long long)calls, 1e3 * to / f.QuadPart / calls, 1e3 * tu / f.QuadPart / calls, 1e3 * tm / f.QuadPart);
  }
}

} // namespace aco
