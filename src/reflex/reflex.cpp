#include "reflex/reflex.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <map>
#include <mutex>
#include <string>

#include "core/config.h"
#include "core/iat.h"
#include "core/log.h"
#include "fg/fg_d3d12.h"
#include "reflex/nvapi_min.h"

namespace aco {

namespace {

using PFN_QueryInterface = void*(__cdecl*)(nv::NvU32);
using PFN_Initialize = nv::Status(__cdecl*)();
using PFN_SetSleepMode = nv::Status(__cdecl*)(IUnknown*, nv::SetSleepModeParams*);
using PFN_Sleep = nv::Status(__cdecl*)(IUnknown*);
using PFN_GetSleepStatus = nv::Status(__cdecl*)(IUnknown*, nv::GetSleepStatusParams*);
using PFN_SetLatencyMarker = nv::Status(__cdecl*)(IUnknown*, nv::LatencyMarkerParams*);

struct Sleeper;
struct State {
  std::mutex mutex;
  bool loaded = false, available = false;
  PFN_SetSleepMode set_sleep_mode = nullptr;
  PFN_Sleep sleep = nullptr;
  PFN_GetSleepStatus get_sleep_status = nullptr;
  PFN_SetLatencyMarker set_marker = nullptr;
  uint64_t frame_id = 1;     // app frame ID for markers (one per Present)
  bool frame_begun = false;  // SIMULATION_START sent for frame_id
  ID3D11Device* dev = nullptr;  // the newest device the game created (owned ref)
  ID3D11Device* applied_dev = nullptr;
  std::string applied_mode;
  float applied_limit = -1.0f;
  bool applied_optimize = false;
  std::string applied_limiter;
  int64_t cap_next = 0;  // [reflex] limiter=cpu: when the next frame may start
  bool sleeping = false;
  // Robustness: Reflex is only armed after kArmFrames stable presents (not during startup /
  // device probing / swapchain changes), and the sleep runs on a sleeper thread with a watchdog:
  // DXVK waits on the driver's semaphore with no timeout, so a confused frame mapping could hang the
  // game (seen once at startup). A sleep that overruns the watchdog is usually just a hitch (e.g.
  // Frame Generation's D3D12/NGX initialisation, ~0.7 s, or loading), so the render thread carries
  // on without sleeping until the sleeper returns, then Reflex resumes. Only a sleep that hasn't
  // returned after kHangMs counts as a hang: the sleep mode is switched off (which can release the
  // wait), a fresh sleeper is started and Reflex is re-armed; after kMaxHangs it stays off.
  IDXGISwapChain* swapchain = nullptr;
  uint32_t stable_frames = 0;
  bool stalled = false;  // given up for the session
  HANDLE sleep_req = nullptr, sleep_done = nullptr;
  std::atomic<bool> sleeper_busy{false};
  std::atomic<int64_t> sleep_started{0};
  struct Sleeper* sleeper = nullptr;  // when the sleeper's current sleep began
  int64_t overrun_since = 0;              // render thread stopped sleeping (watchdog) at
  uint32_t overrun_skips = 0, overruns = 0, hangs = 0;
  // perf
  double sleep_ms_sum = 0;
  uint32_t sleeps = 0;
  int64_t last_report = 0;
} S;

int64_t now_us() {
  static const int64_t freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return c.QuadPart / freq * 1000000 + c.QuadPart % freq * 1000000 / freq;
}

bool load() {
  if (S.loaded) return S.available;
  S.loaded = true;
  HMODULE m = LoadLibraryW(L"nvapi64.dll");
  auto qi = m ? reinterpret_cast<PFN_QueryInterface>(GetProcAddress(m, "nvapi_QueryInterface")) : nullptr;
  if (!qi) {
    ACO_LOG("reflex: nvapi64.dll not available (Proton: PROTON_ENABLE_NVAPI=1)");
    return false;
  }
  auto init = reinterpret_cast<PFN_Initialize>(qi(nv::kId_Initialize));
  S.set_sleep_mode = reinterpret_cast<PFN_SetSleepMode>(qi(nv::kId_D3D_SetSleepMode));
  S.sleep = reinterpret_cast<PFN_Sleep>(qi(nv::kId_D3D_Sleep));
  S.get_sleep_status = reinterpret_cast<PFN_GetSleepStatus>(qi(nv::kId_D3D_GetSleepStatus));
  S.set_marker = reinterpret_cast<PFN_SetLatencyMarker>(qi(nv::kId_D3D_SetLatencyMarker));
  nv::Status st = init ? init() : -1;
  S.available = st == 0 && S.set_sleep_mode && S.sleep;
  ACO_LOG("reflex: NvAPI_Initialize -> %d; SetSleepMode=%p Sleep=%p", st, reinterpret_cast<void*>(S.set_sleep_mode),
          reinterpret_cast<void*>(S.sleep));
  return S.available;
}

constexpr uint32_t kArmFrames = 240;

constexpr int64_t kHangMs = 3000;
constexpr uint32_t kMaxHangs = 3;

// One sleeper thread at a time; after a hang a new one (with new events) replaces it and the old one
// is left to finish (or stay blocked) on its own.
struct Sleeper {
  HANDLE req, done;
  std::atomic<bool>* busy;
};
DWORD WINAPI sleeper_main(void* arg) {
  Sleeper* sl = static_cast<Sleeper*>(arg);
  for (;;) {
    WaitForSingleObject(sl->req, INFINITE);
    S.sleep(S.dev);
    sl->busy->store(false);
    SetEvent(sl->done);
    if (sl->req != S.sleep_req) return 0;  // replaced after a hang: retire
  }
}

void start_sleeper() {
  auto* sl = new Sleeper{CreateEventW(nullptr, FALSE, FALSE, nullptr), CreateEventW(nullptr, FALSE, FALSE, nullptr),
                         new std::atomic<bool>(false)};
  S.sleep_req = sl->req;
  S.sleep_done = sl->done;
  S.sleeper = sl;
  HANDLE t = CreateThread(nullptr, 0, sleeper_main, sl, 0, nullptr);
  if (t) CloseHandle(t);
}

enum class SleepResult { ok, overrun, still_busy };

// Runs NvAPI_D3D_Sleep on the sleeper thread and waits up to timeout_ms for it.
SleepResult sleep_with_watchdog(DWORD timeout_ms) {
  if (!S.sleeper) start_sleeper();
  if (S.sleeper->busy->load()) return SleepResult::still_busy;  // the previous sleep hasn't returned
  S.sleeper->busy->store(true);
  S.sleep_started.store(now_us());
  ResetEvent(S.sleep_done);  // a sleep that overran signalled it after we stopped waiting: stale
  SetEvent(S.sleep_req);
  return WaitForSingleObject(S.sleep_done, timeout_ms) == WAIT_OBJECT_0 ? SleepResult::ok : SleepResult::overrun;
}

// fps_limit, or Frame Generation's automatic cap when no explicit limit is set.
float effective_limit() {
  float l = config().reflex_fps_limit;
  return l > 0 ? l : fg12_auto_cap();
}

// (Re)applies the sleep mode when the config or the device changed.
void apply_mode() {
  const Config& cfg = config();
  const float limit = effective_limit();
  if (S.stalled) return;
  if (S.applied_dev == S.dev && S.applied_mode == cfg.reflex && std::fabs(S.applied_limit - limit) < 0.5f &&
      S.applied_optimize == cfg.reflex_optimize && S.applied_limiter == cfg.reflex_limiter)
    return;
  S.applied_limiter = cfg.reflex_limiter;
  S.applied_optimize = cfg.reflex_optimize;
  S.applied_dev = S.dev;
  S.applied_mode = cfg.reflex;
  S.applied_limit = limit;
  nv::SetSleepModeParams p{};
  p.version = nv::kSetSleepModeVer;
  p.bLowLatencyMode = cfg.reflex == "on" || cfg.reflex == "boost";
  p.bLowLatencyBoost = cfg.reflex == "boost";
  p.minimumIntervalUs = limit > 0 && cfg.reflex_limiter != "cpu" ? static_cast<nv::NvU32>(1e6 / limit) : 0;
  p.bUseMarkersToOptimize = cfg.reflex_optimize;
  nv::Status st = S.set_sleep_mode(S.dev, &p);
  S.sleeping = st == 0 && p.bLowLatencyMode;
  S.frame_begun = false;
  static std::string last_logged;
  char key[96];
  snprintf(key, sizeof(key), "%d%d%d%d%d", p.bLowLatencyMode, p.bLowLatencyBoost, p.minimumIntervalUs > 0,
           p.bUseMarkersToOptimize, st);
  if (key == last_logged && cfg.reflex_fps_limit <= 0) return;  // only the automatic cap moved
  last_logged = key;
  ACO_LOG("reflex: SetSleepMode(low_latency=%d boost=%d min_interval=%uus optimize=%d) -> %d", p.bLowLatencyMode,
          p.bLowLatencyBoost, p.minimumIntervalUs, p.bUseMarkersToOptimize, st);
  if (S.get_sleep_status) {
    nv::GetSleepStatusParams gs{};
    gs.version = nv::kGetSleepStatusVer;
    nv::Status gst = S.get_sleep_status(S.dev, &gs);
    ACO_LOG("reflex: GetSleepStatus -> %d (low_latency=%d)", gst, gs.bLowLatencyMode);
  }
}

// ---- input probe (config [reflex] probe=1): which threads sample input, and when ---------------
// Hooks user32's calls into win32u, which every user32 caller in the process goes through.

struct ThreadStats {
  uint32_t peek = 0, peek_msgs = 0, wm_input = 0, bursts = 0, raw_data = 0, raw_buffer = 0, async = 0, kbd = 0;
  bool last_peek_empty = true;
  int64_t last_burst_us = 0, last_input_us = 0;
};

std::mutex g_probe_mutex;
std::map<DWORD, ThreadStats> g_threads;
int64_t g_since_input_sum = 0;
uint32_t g_since_input_n = 0, g_probe_frames = 0;
int64_t g_probe_last = 0;

using PFN_NtUserPeekMessage = BOOL(WINAPI*)(MSG*, HWND, UINT, UINT, UINT);
using PFN_NtUserGetRawInputData = UINT(WINAPI*)(HRAWINPUT, UINT, void*, UINT*, UINT);
using PFN_NtUserGetRawInputBuffer = UINT(WINAPI*)(RAWINPUT*, UINT*, UINT);
using PFN_NtUserGetAsyncKeyState = SHORT(WINAPI*)(INT);
using PFN_NtUserGetKeyboardState = BOOL(WINAPI*)(BYTE*);
PFN_NtUserPeekMessage o_peek;
PFN_NtUserGetRawInputData o_raw_data;
PFN_NtUserGetRawInputBuffer o_raw_buffer;
PFN_NtUserGetAsyncKeyState o_async;
PFN_NtUserGetKeyboardState o_kbd;

ThreadStats& tstats() { return g_threads[GetCurrentThreadId()]; }

BOOL WINAPI hk_peek(MSG* msg, HWND wnd, UINT lo, UINT hi, UINT flags) {
  BOOL r = o_peek(msg, wnd, lo, hi, flags);
  std::lock_guard lock(g_probe_mutex);
  auto& t = tstats();
  ++t.peek;
  if (r) ++t.peek_msgs;
  if (r && msg && msg->message == WM_INPUT) ++t.wm_input, t.last_input_us = now_us();
  if (t.last_peek_empty) ++t.bursts, t.last_burst_us = now_us();
  t.last_peek_empty = !r;
  return r;
}
UINT WINAPI hk_raw_data(HRAWINPUT h, UINT cmd, void* data, UINT* size, UINT hdr) {
  { std::lock_guard lock(g_probe_mutex); auto& t = tstats(); ++t.raw_data; t.last_input_us = now_us(); }
  return o_raw_data(h, cmd, data, size, hdr);
}
UINT WINAPI hk_raw_buffer(RAWINPUT* data, UINT* size, UINT hdr) {
  { std::lock_guard lock(g_probe_mutex); auto& t = tstats(); ++t.raw_buffer; t.last_input_us = now_us(); }
  return o_raw_buffer(data, size, hdr);
}
SHORT WINAPI hk_async(INT k) {
  { std::lock_guard lock(g_probe_mutex); ++tstats().async; }
  return o_async(k);
}
BOOL WINAPI hk_kbd(BYTE* k) {
  { std::lock_guard lock(g_probe_mutex); ++tstats().kbd; }
  return o_kbd(k);
}

template <typename T>
void probe_hook(const char* fn, T hk, T* orig) {
  if (!iat_hook(GetModuleHandleW(L"user32.dll"), "win32u.dll", fn, reinterpret_cast<void*>(hk),
                reinterpret_cast<void**>(orig)))
    ACO_LOG("reflex-probe: user32 does not import win32u!%s", fn);
}

void probe_install() {
  static bool done = false;
  if (done) return;
  done = true;
  probe_hook("NtUserPeekMessage", hk_peek, &o_peek);
  probe_hook("NtUserGetRawInputData", hk_raw_data, &o_raw_data);
  probe_hook("NtUserGetRawInputBuffer", hk_raw_buffer, &o_raw_buffer);
  probe_hook("NtUserGetAsyncKeyState", hk_async, &o_async);
  probe_hook("NtUserGetKeyboardState", hk_kbd, &o_kbd);
}

void probe_on_present() {
  std::lock_guard lock(g_probe_mutex);
  int64_t now = now_us();
  ++g_probe_frames;
  // Time since the latest input read on any thread, sampled at Present.
  int64_t latest = 0;
  for (auto& [tid, t] : g_threads) latest = std::max(latest, t.last_input_us);
  if (latest) g_since_input_sum += now - latest, ++g_since_input_n;
  if (now - g_probe_last < 2000000) return;
  double frames = g_probe_frames;
  if (g_probe_last) {
    ACO_LOG("reflex-probe: present tid %lu, %.0f frames, latest input read -> present avg %.2f ms",
            GetCurrentThreadId(), frames, g_since_input_n ? g_since_input_sum / 1000.0 / g_since_input_n : -1.0);
    for (auto& [tid, t] : g_threads) {
      if (!(t.peek | t.raw_data | t.raw_buffer | t.async | t.kbd)) continue;
      ACO_LOG("reflex-probe:   tid %5lu peek %6.2f/f msgs %5.2f/f WM_INPUT %5.2f/f bursts %4.2f/f rawdata %5.2f/f "
              "rawbuf %4.2f/f async %5.2f/f kbd %4.2f/f",
              tid, t.peek / frames, t.peek_msgs / frames, t.wm_input / frames, t.bursts / frames, t.raw_data / frames,
              t.raw_buffer / frames, t.async / frames, t.kbd / frames);
    }
  }
  for (auto& [tid, t] : g_threads) {
    ThreadStats fresh;
    fresh.last_peek_empty = t.last_peek_empty;
    fresh.last_burst_us = t.last_burst_us;
    fresh.last_input_us = t.last_input_us;
    t = fresh;
  }
  g_probe_last = now;
  g_probe_frames = 0;
  g_since_input_sum = 0;
  g_since_input_n = 0;
}

void marker(nv::LatencyMarker type) {
  if (!S.set_marker || !config().reflex_markers) return;
  nv::LatencyMarkerParams p{};
  p.version = nv::kLatencyMarkerVer;
  p.frameID = S.frame_id;
  p.markerType = type;
  S.set_marker(S.dev, &p);
}

} // namespace

void reflex_on_device(ID3D11Device* dev) {
  std::lock_guard lock(S.mutex);
  // The game creates probe devices before the real one; follow the newest.
  dev->AddRef();
  if (S.dev) S.dev->Release();
  S.dev = dev;
}

void reflex_on_resize() {
  std::lock_guard lock(S.mutex);
  S.stable_frames = 0;  // re-arm once the resized swapchain is stable
}

void reflex_on_present_begin(IDXGISwapChain* sc) {
  std::lock_guard lock(S.mutex);
  if (sc != S.swapchain) {  // new swapchain: re-arm after it has been stable for a while
    S.swapchain = sc;
    S.stable_frames = 0;
  }
  if (S.stable_frames < kArmFrames) {
    ++S.stable_frames;
    return;
  }
  if (config().reflex_probe) {
    probe_install();
    probe_on_present();
  }
  if (!S.dev || !load()) return;
  apply_mode();
  if (!S.sleeping) return;
  if (!S.frame_begun) {  // first frame after enabling: no sleep happened yet
    marker(nv::SIMULATION_START);
    S.frame_begun = true;
  }
  if (config().reflex_marker_set != "minimal") {
    marker(nv::SIMULATION_END);
    marker(nv::RENDERSUBMIT_START);
    marker(nv::RENDERSUBMIT_END);
  }
  marker(nv::PRESENT_START);
}

// Reflex sleep at the start of the render thread's next frame: the driver (VK_NV_low_latency2 via
// DXVK) delays us just enough that the frame's GPU work arrives as the GPU frees up, which drains
// the render queue. Frames are delimited for the driver with markers, one app frame per Present
// (SIMULATION_START right after the sleep ... PRESENT_END). Without markers DXVK has to make the
// sleep wait for its CS thread to finish the previous frame, which costs throughput when CPU-bound.
// [reflex] limiter=cpu: our own frame cap on the render thread (independent of the driver's Reflex
// limiter, and of whether Reflex is on). Frame starts are kept on a fixed grid of 1/fps_limit; if the
// game falls behind by more than a frame the grid restarts from now (no catch-up bursts).
std::atomic<uint64_t> g_wait_us{0};  // reflex_wait_us()

void cpu_limit() {
  const Config& cfg = config();
  const float limit = effective_limit();
  // limiter=driver needs Reflex sleeping; without it (mode=off, or not armed yet) we enforce the cap.
  if ((cfg.reflex_limiter != "cpu" && S.sleeping) || limit <= 0) {
    S.cap_next = 0;
    return;
  }
  int64_t interval = static_cast<int64_t>(1e6 / limit);
  int64_t now = now_us();
  if (!S.cap_next || now - S.cap_next > interval) S.cap_next = now;
  for (;;) {
    int64_t left = S.cap_next - now_us();
    if (left <= 0) break;
    if (left > 1500) Sleep(static_cast<DWORD>((left - 1000) / 1000));
    else YieldProcessor();
  }
  S.cap_next += interval;
  g_wait_us += uint64_t(now_us() - now);
}

void reflex_on_present_end() {
  std::lock_guard lock(S.mutex);
  cpu_limit();
  if (!S.available || !S.sleeping || S.stalled || S.stable_frames < kArmFrames) return;
  if (S.frame_begun) marker(nv::PRESENT_END);
  ++S.frame_id;
  int64_t t0 = now_us();
  // Generous: a legitimate sleep is at most about one frame (or one fps_limit interval).
  float limit = effective_limit();
  DWORD timeout = 500 + (limit > 0 ? static_cast<DWORD>(2000.0f / limit) : 0);
  SleepResult r = sleep_with_watchdog(timeout);
  if (r != SleepResult::ok) {
    if (r == SleepResult::overrun) {
      S.overrun_since = t0;
      S.overrun_skips = 0;
      ++S.overruns;
      ACO_LOG("reflex: sleep did not return within %lu ms (hitch?); not sleeping until it does", timeout);
    } else {
      ++S.overrun_skips;
      int64_t waited = (now_us() - S.sleep_started.load()) / 1000;
      if (waited > kHangMs) {
        // A real hang: switch the sleep mode off (may release the driver's wait), replace the sleeper
        // and re-arm (apply_mode re-applies the mode on the next present) - or give up.
        ++S.hangs;
        nv::SetSleepModeParams off{};
        off.version = nv::kSetSleepModeVer;
        nv::Status st = S.set_sleep_mode(S.dev, &off);
        S.sleeping = false;
        S.applied_dev = nullptr;
        S.stalled = S.hangs >= kMaxHangs;
        start_sleeper();
        ACO_LOG("reflex: sleep hung for %lld ms (hang %u of %u; SetSleepMode off -> %d); %s", static_cast<long long>(waited),
                S.hangs, kMaxHangs, st, S.stalled ? "Reflex disabled for this session" : "re-arming with a new sleeper");
      }
    }
    marker(nv::SIMULATION_START);  // the frame goes on without a sleep
    S.frame_begun = true;
    return;
  }
  if (S.overrun_since) {
    ACO_LOG("reflex: sleeping again (the overrun sleep returned; %u frames went without a sleep, %.0f ms)",
            S.overrun_skips, (t0 - S.overrun_since) / 1000.0);
    S.overrun_since = 0;
  }
  int64_t t1 = now_us();
  marker(nv::SIMULATION_START);
  S.frame_begun = true;
  S.sleep_ms_sum += (t1 - t0) / 1000.0;
  g_wait_us += uint64_t(t1 - t0);
  ++S.sleeps;
  if (t1 - S.last_report >= 2000000) {
    if (S.last_report)
      ACO_LOG("perf: reflex sleep avg %.2f ms over %u frames (overruns %u, hangs %u)", S.sleep_ms_sum / S.sleeps,
              S.sleeps, S.overruns, S.hangs);
    S.last_report = t1;
    S.sleep_ms_sum = 0;
    S.sleeps = 0;
  }
}


unsigned long long reflex_wait_us() { return g_wait_us.load(); }

} // namespace aco
