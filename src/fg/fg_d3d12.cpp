// DLSS Frame Generation through NGX's D3D12 API on a vkd3d-proton device of our own ([fg] mode=d3d12).
//
// The game renders with D3D11 (DXVK). Each frame we copy the DLSS-G inputs (backbuffer, depth, motion
// vectors) into textures shared between DXVK and vkd3-proton (NT handles), signal a shared fence,
// evaluate DLSS-G on our D3D12 queue (which waits on that fence), and have the D3D11 context wait for
// the D3D12 signal before it copies the generated frame into the swapchain. No CPU waits in the
// steady state. See docs/FRAME_GENERATION.md (D3D12 route) and tools/interop-test/.
#include "fg/fg_d3d12.h"

#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <nvsdk_ngx_params.h>

#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/config.h"
#include "core/ctxgate.h"
#include "core/log.h"
#include "dlss/ngx.h"
#include "dlss/upscaler.h"
#include "reflex/reflex.h"

namespace aco {

namespace {

constexpr unsigned long long kAppId = 0x41434f444c5353ull;  // "ACODLSS", as for DLSS-SR
// Multi Frame Generation: up to 5 generated frames per real frame (6x; DLSSG.MultiFrameCountMax is 5
// on an RTX 50 with nvngx_dlssg 310.9.16). Each generated index is its own submission (own fence
// value and timestamps), so the ring holds two frames' worth plus feature creation.
constexpr int kMaxGen = 5;
constexpr int kRing = 2 * kMaxGen + 4;
const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

int64_t now_us() {
  static LARGE_INTEGER f = [] {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return v;
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return c.QuadPart * 1000000 / f.QuadPart;
}

template <typename T>
void safe_release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

// The core export takes the version before the feature info (unlike the SDK's inline wrapper).
using PFN_Init = NVSDK_NGX_Result(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
                                            const NVSDK_NGX_FeatureCommonInfo*);
using PFN_GetParams = NVSDK_NGX_Result(__cdecl*)(NVSDK_NGX_Parameter**);
using PFN_Create = NVSDK_NGX_Result(__cdecl*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                                              NVSDK_NGX_Handle**);
using PFN_Eval = NVSDK_NGX_Result(__cdecl*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                            const NVSDK_NGX_Parameter*, void*);
using PFN_Release = NVSDK_NGX_Result(__cdecl*)(NVSDK_NGX_Handle*);
using PFN_D3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

// A texture created by D3D11 (DXVK) and opened in D3D12 (vkd3d-proton).
struct Shared {
  ID3D11Texture2D* t11 = nullptr;
  ID3D12Resource* t12 = nullptr;
  UINT w = 0, h = 0;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
  void release() {
    safe_release(t12);
    safe_release(t11);
    w = h = 0;
    fmt = DXGI_FORMAT_UNKNOWN;
  }
};

// One frame's DLSS-G inputs and outputs. Frames alternate between sets ([fg] sets=2, or 3), so the
// game can copy the next frame's inputs while the present thread still shows this one.
struct Set {
  Shared color, depth, mv, hudless;
  Shared out[kMaxGen];  // OutputInterpolated of generated index 1..n_gen
  ID3D11Texture2D* real = nullptr;  // D3D11-only copy of the real frame, presented after the generated ones
  UINT64 eval_v = 0;  // D3D12 fence value when the last evaluation using this set is done
  unsigned n_gen = 0;  // generated frames of that evaluation
  // Per generated index: completion value, timestamp ring slot and that slot's end timestamp before
  // the evaluation (the completion check).
  UINT64 gen_v[kMaxGen] = {};
  int gen_slot[kMaxGen] = {};
  UINT64 gen_ts_old[kMaxGen] = {};
};

struct State {

  ID3D11Device5* dev = nullptr;
  ID3D11DeviceContext4* imm = nullptr;
  ID3D11Fence* fence11 = nullptr;
  ID3D12Device* d12 = nullptr;
  ID3D12CommandQueue* q = nullptr;
  ID3D12Fence* fence12 = nullptr;
  UINT64 fence_value = 0;  // fence11/fence12: signaled by D3D11 only (inputs copied)
  // Signaled by D3D12 only (evaluation done). Two queues must never signal the same fence: with one
  // shared fence for both directions, D3D11's signal for the next frame's inputs (a higher value) could
  // land before D3D12's signal for this frame's evaluation, so "evaluation done" read as complete
  // early (the generated frame was presented before DLSS-G had written it: stale content from two
  // frames back), and the D3D12 signal then moved the timeline backwards.
  ID3D12Fence* done12 = nullptr;
  ID3D11Fence* done11 = nullptr;  // the same fence opened in D3D11 (sync=gpu / feature creation)
  UINT64 done_value = 0;
  // Same values as done12, but a plain (non-shared) D3D12 fence: [fg] done_fence=private makes the CPU
  // waits use it (diagnosing whether the shared fence completes before the GPU work it follows).
  ID3D12Fence* priv12 = nullptr;
  int eval_slot = -1;      // ring slot of the last submission
  UINT64 eval_ts_old = 0;  // that slot's end timestamp before the submission (changes once it has run)
  UINT64 last_v = 0;       // completion value of the last submission
  HANDLE fence_event = nullptr;
  ID3D12CommandAllocator* alloc[kRing] = {};
  UINT64 slot_value[kRing] = {};
  ID3D12GraphicsCommandList* cl = nullptr;
  int ring = 0;
  // GPU time of each evaluation: timestamps around it, resolved into a persistently mapped buffer.
  ID3D12QueryHeap* ts_heap = nullptr;
  ID3D12Resource* ts_buf = nullptr;
  const UINT64* ts_map = nullptr;
  bool ts_written[kRing] = {};
  double ts_freq = 0;

  PFN_GetParams alloc_params = nullptr;
  PFN_Create create = nullptr;
  PFN_Eval evaluate = nullptr;
  PFN_Release release = nullptr;
  ngx::Params create_params, eval_params;
  NVSDK_NGX_Handle* feature = nullptr;
  uint32_t feat_w = 0, feat_h = 0, feat_rw = 0, feat_rh = 0;
  DXGI_FORMAT feat_fmt = DXGI_FORMAT_UNKNOWN;
  unsigned feat_count = 0;
  unsigned caps_max_gen = 1;  // DLSSG.MultiFrameCountMax (generated frames per real frame)

  // Texture sets: cur (this frame), prev (the previous frame; [fg] sync=cpu), oldest ([fg] sets=3: the
  // present thread may run two frames behind before the game has to wait for it). swap_sets() rotates.
  Set sets[3];
  Set* cur = &sets[0];
  Set* prev = &sets[1];
  Set* oldest = &sets[2];
  Shared outreal;
  HANDLE game_event = nullptr;  // for CPU waits on the game thread (wait_d3d12 uses fence_event)
  uint32_t tex_w = 0, tex_h = 0;
  DXGI_FORMAT tex_fmt = DXGI_FORMAT_UNKNOWN;

  // [fg] verify=1: periodically read back (previous real, generated, current real) and log how the
  // generated frame differs from its neighbours; also written to out/fgverify/*.ppm.
  ID3D11Texture2D* verify_prev = nullptr;
  ID3D11Texture2D* stage[3] = {};
  int64_t verify_last = 0;
  int verify_pending = -1;  // presents until the staged copies are read
  int verify_n = 0;

  bool reset = true;
  uint64_t last_input_frame = 0;
  unsigned long long backbuffer_id = 0;
  int64_t last_real_present = 0;
  double frame_interval_us = 0;

  int64_t perf_last = 0;
  uint32_t perf_real = 0, perf_generated = 0, perf_fail = 0, perf_resets = 0, perf_cpu_waits = 0;
  double perf_eval_cpu_ms = 0, perf_gpu_ms = 0;
  uint32_t perf_gpu_n = 0;
} S;

// The display's refresh rate: [fg] refresh_hz, or (0) what the swapchain's mode / the display mode
// says (detect_refresh in fg12_present). 0 = unknown.
std::atomic<float> g_display_hz{0};
float display_hz() {
  float c = config().fg_refresh_hz;
  return c > 0 ? c : g_display_hz.load();
}

// sRGB views can't be UAVs; the shared copies use the UNORM sibling (CopyResource allows it).
DXGI_FORMAT storage_format(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    // Depth: a colour texture of the same size class (D3D11 CopyResource accepts the R32 family).
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    default: return f;
  }
}

bool make_shared(Shared& s, UINT w, UINT h, DXGI_FORMAT fmt, bool uav) {
  if (s.t11 && s.w == w && s.h == h && s.fmt == fmt) return true;
  s.release();
  D3D11_TEXTURE2D_DESC d{};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = fmt;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
  d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
  HRESULT hr = S.dev->CreateTexture2D(&d, nullptr, &s.t11);
  if (FAILED(hr)) {
    ACO_LOG("fg12: CreateTexture2D(shared %ux%u fmt %d) -> 0x%08lx", w, h, fmt, hr);
    return false;
  }
  IDXGIResource1* r1 = nullptr;
  HANDLE handle = nullptr;
  if (SUCCEEDED(s.t11->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&r1)))) {
    hr = r1->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle);
    r1->Release();
  }
  if (!handle || FAILED(hr)) {
    ACO_LOG("fg12: CreateSharedHandle -> 0x%08lx", hr);
    s.release();
    return false;
  }
  hr = S.d12->OpenSharedHandle(handle, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&s.t12));
  CloseHandle(handle);
  if (FAILED(hr)) {
    ACO_LOG("fg12: OpenSharedHandle(texture) -> 0x%08lx", hr);
    s.release();
    return false;
  }
  s.w = w;
  s.h = h;
  s.fmt = fmt;
  return true;
}

// The fence CPU waits use ([fg] done_fence).
ID3D12Fence* wait_fence() { return S.priv12 && config().fg_done_fence == "private" ? S.priv12 : S.done12; }

bool wait_on(ID3D12Fence* f, UINT64 v, HANDLE event, DWORD timeout_ms);
void wait_d3d12(UINT64 v) {
  if (!wait_on(wait_fence(), v, S.fence_event, 2000)) ACO_LOG("fg12: D3D12 fence wait for %llu timed out", (unsigned long long)v);
}

// Runs `record` on our D3D12 queue after the D3D11 work submitted so far (`after_d3d11`; later
// submissions of the same frame follow on the same queue anyway), then makes the D3D11 context wait
// for it. `cpu_wait` blocks until the GPU is done (feature creation).
template <typename F>
bool submit(F&& record, bool cpu_wait, bool after_d3d11 = true) {
  int slot = S.ring;
  S.ring = (S.ring + 1) % kRing;
  if (wait_fence()->GetCompletedValue() < S.slot_value[slot]) {
    ++S.perf_cpu_waits;
    wait_d3d12(S.slot_value[slot]);
  }
  if (S.ts_written[slot] && S.ts_map) {
    UINT64 t0 = S.ts_map[slot * 2], t1 = S.ts_map[slot * 2 + 1];
    if (t1 > t0 && S.ts_freq > 0) {
      S.perf_gpu_ms += (t1 - t0) * 1000.0 / S.ts_freq;
      ++S.perf_gpu_n;
    }
    S.ts_written[slot] = false;
  }
  S.alloc[slot]->Reset();
  S.cl->Reset(S.alloc[slot], nullptr);
  if (S.ts_heap) S.cl->EndQuery(S.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
  bool ok = record(S.cl);
  if (S.ts_heap) {
    S.cl->EndQuery(S.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
    S.cl->ResolveQueryData(S.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, S.ts_buf, slot * 2 * sizeof(UINT64));
    S.ts_written[slot] = true;
  }
  if (FAILED(S.cl->Close())) return false;
  S.eval_slot = S.ts_written[slot] ? slot : -1;
  S.eval_ts_old = S.ts_map ? S.ts_map[slot * 2 + 1] : 0;

  // D3D11 -> D3D12: everything the context has recorded so far (our input copies) first.
  if (after_d3d11) {
    UINT64 v_in = ++S.fence_value;
    S.imm->Signal(S.fence11, v_in);
    S.imm->Flush();
    S.q->Wait(S.fence12, v_in);
  }
  ID3D12CommandList* lists[] = {S.cl};
  S.q->ExecuteCommandLists(1, lists);
  UINT64 v_out = ++S.done_value;
  S.q->Signal(S.done12, v_out);
  if (S.priv12) S.q->Signal(S.priv12, v_out);
  S.slot_value[slot] = v_out;
  S.last_v = v_out;
  // D3D12 -> D3D11. sync=gpu: later context work waits on the GPU (vkd3d-proton resolves the D3D12
  // side's wait on the shared fence on the CPU, so this stalls the D3D11 queue on a CPU round trip).
  // sync=cpu: whoever copies the output out first waits on the CPU (wait_eval).
  if ((cpu_wait || config().fg_sync == "gpu") && S.done11) S.imm->Wait(S.done11, v_out);
  if (cpu_wait) wait_d3d12(v_out);
  return ok;
}

// CPU wait for a D3D12 fence value with a caller-owned event; false on timeout.
// The event alone is never trusted: a wait that timed out leaves its SetEventOnCompletion registered,
// and when that fires later it would wake the *next* wait early - from then on every wait returned one
// evaluation too soon and generated frames showed the texture's previous contents (seen on the user's
// machine after a period of heavy GPU contention). So: clear stale signals first, and only return true
// once the fence value itself has been reached.
bool wait_on(ID3D12Fence* f, UINT64 v, HANDLE event, DWORD timeout_ms) {
  if (!v || f->GetCompletedValue() >= v) return true;
  int64_t deadline = now_us() + int64_t(timeout_ms) * 1000;
  ResetEvent(event);
  f->SetEventOnCompletion(v, event);
  for (;;) {
    if (f->GetCompletedValue() >= v) return true;
    int64_t left = deadline - now_us();
    if (left <= 0) return false;
    WaitForSingleObject(event, static_cast<DWORD>((left + 999) / 1000));
  }
}

bool wait_eval(UINT64 v, HANDLE event, DWORD timeout_ms) { return wait_on(wait_fence(), v, event, timeout_ms); }

void swap_sets() {
  if (config().fg_sets >= 3) {
    // (cur, prev, oldest) -> (oldest, cur, prev): the oldest set becomes current.
    Set* c = S.cur;
    S.cur = S.oldest;
    S.oldest = S.prev;
    S.prev = c;
    return;
  }
  std::swap(S.cur, S.prev);
}

bool init(ID3D11Device* dev) {
  HRESULT hr = dev->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&S.dev));
  ID3D11DeviceContext* ctx = nullptr;
  dev->GetImmediateContext(&ctx);
  if (FAILED(hr) || !ctx || FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&S.imm)))) {
    ACO_LOG("fg12: no ID3D11Device5/ID3D11DeviceContext4");
    safe_release(ctx);
    return false;
  }
  ctx->Release();

  // vkd3d-proton's descriptor-heap mode: in its default descriptor-buffer mode two of DLSS-G's raw
  // buffer UAVs have no CUDA descriptor (NvAPI_D3D12_GetCudaIndependentDescriptorObject fails).
  // vkd3d reads VKD3D_CONFIG when it creates its instance, i.e. at our D3D12CreateDevice.
  if (config().fg12_descriptor_heap) {
    wchar_t cur[512] = {};
    DWORD n = GetEnvironmentVariableW(L"VKD3D_CONFIG", cur, 512);
    std::wstring v = n ? std::wstring(cur) + L",descriptor_heap" : L"descriptor_heap";
    SetEnvironmentVariableW(L"VKD3D_CONFIG", v.c_str());
  }
  HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
  auto create_device = d3d12 ? reinterpret_cast<PFN_D3D12CreateDevice>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
  if (!create_device) {
    ACO_LOG("fg12: d3d12.dll / D3D12CreateDevice unavailable");
    return false;
  }
  IDXGIDevice* dxgi_dev = nullptr;
  IDXGIAdapter* adapter = nullptr;
  if (SUCCEEDED(S.dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_dev)))) {
    dxgi_dev->GetAdapter(&adapter);
    dxgi_dev->Release();
  }
  int64_t t0 = now_us();
  hr = create_device(adapter, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&S.d12));
  safe_release(adapter);
  ACO_LOG("fg12: D3D12CreateDevice -> 0x%08lx (%.0f ms)", hr, (now_us() - t0) / 1000.0);
  if (FAILED(hr)) return false;

  D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
  qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
  if (FAILED(S.d12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&S.q)))) return false;
  for (auto& a : S.alloc)
    if (FAILED(S.d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                             reinterpret_cast<void**>(&a))))
      return false;
  if (FAILED(S.d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, S.alloc[0], nullptr,
                                      __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&S.cl))))
    return false;
  S.cl->Close();

  // The shared fence: created by D3D11, opened by D3D12.
  HANDLE fh = nullptr;
  hr = S.dev->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&S.fence11));
  if (SUCCEEDED(hr)) hr = S.fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fh);
  if (SUCCEEDED(hr)) hr = S.d12->OpenSharedHandle(fh, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&S.fence12));
  if (fh) CloseHandle(fh);
  if (FAILED(hr)) {
    ACO_LOG("fg12: shared fence -> 0x%08lx", hr);
    return false;
  }
  // The completion fence: created by D3D12, also opened in D3D11 (only needed for GPU-side waits).
  hr = S.d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&S.done12));
  if (FAILED(hr)) {
    ACO_LOG("fg12: completion fence -> 0x%08lx", hr);
    return false;
  }
  fh = nullptr;
  if (SUCCEEDED(S.d12->CreateSharedHandle(S.done12, nullptr, GENERIC_ALL, nullptr, &fh)) && fh) {
    if (FAILED(S.dev->OpenSharedFence(fh, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&S.done11)))) S.done11 = nullptr;
    CloseHandle(fh);
  }
  ACO_LOG("fg12: fences: inputs (D3D11 -> D3D12) shared, completion (D3D12) %s in D3D11",
          S.done11 ? "also opened" : "NOT opened (sync=gpu unavailable)");
  if (FAILED(S.d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&S.priv12))))
    S.priv12 = nullptr;
  S.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  S.game_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  D3D12_QUERY_HEAP_DESC qh{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kRing * 2, 0};
  UINT64 freq = 0;
  if (SUCCEEDED(S.d12->CreateQueryHeap(&qh, __uuidof(ID3D12QueryHeap), reinterpret_cast<void**>(&S.ts_heap))) &&
      SUCCEEDED(S.q->GetTimestampFrequency(&freq))) {
    S.ts_freq = double(freq);
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = kRing * 2 * sizeof(UINT64);
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* m = nullptr;
    if (SUCCEEDED(S.d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 __uuidof(ID3D12Resource), reinterpret_cast<void**>(&S.ts_buf))) &&
        SUCCEEDED(S.ts_buf->Map(0, nullptr, &m)))
      S.ts_map = static_cast<const UINT64*>(m);
  }
  if (!S.ts_map) safe_release(S.ts_heap);

  HMODULE ngx = ngx::core_module();
  if (!ngx) return false;
  auto init_fn = reinterpret_cast<PFN_Init>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init"));
  auto caps_fn = reinterpret_cast<PFN_GetParams>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
  S.alloc_params = reinterpret_cast<PFN_GetParams>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_AllocateParameters"));
  S.create = reinterpret_cast<PFN_Create>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature"));
  S.evaluate = reinterpret_cast<PFN_Eval>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature"));
  S.release = reinterpret_cast<PFN_Release>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_ReleaseFeature"));
  if (!init_fn || !caps_fn || !S.alloc_params || !S.create || !S.evaluate || !S.release) {
    ACO_LOG("fg12: NGX D3D12 entry points missing");
    return false;
  }
  std::wstring dir = module_dir();
  const wchar_t* paths[] = {dir.c_str()};
  NVSDK_NGX_FeatureCommonInfo common{};
  common.PathListInfo.Path = paths;
  common.PathListInfo.Length = 1;
  t0 = now_us();
  NVSDK_NGX_Result r = init_fn(kAppId, dir.c_str(), S.d12, NVSDK_NGX_Version_API, &common);
  ACO_LOG("fg12: NVSDK_NGX_D3D12_Init -> %s (%.1f s)", ngx::result_str(r), (now_us() - t0) / 1e6);
  if (NVSDK_NGX_FAILED(r)) return false;
  NVSDK_NGX_Parameter* caps_raw = nullptr;
  caps_fn(&caps_raw);
  ngx::Params caps(caps_raw);
  int avail = -1;
  if (caps) caps.get(NVSDK_NGX_Parameter_FrameGeneration_Available, &avail);
  unsigned max_gen = 0;
  if (caps) caps.get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &max_gen);
  S.caps_max_gen = std::max(1u, std::min<unsigned>(max_gen, kMaxGen));
  ACO_LOG("fg12: FrameGeneration available=%d, MultiFrameCountMax=%u (up to %ux)", avail, max_gen, S.caps_max_gen + 1);
  if (avail != 1) return false;
  NVSDK_NGX_Parameter* p = nullptr;
  S.alloc_params(&p);
  S.create_params = ngx::Params(p);
  p = nullptr;
  S.alloc_params(&p);
  S.eval_params = ngx::Params(p);
  return S.create_params && S.eval_params;
}

bool ensure_feature(uint32_t w, uint32_t h, uint32_t rw, uint32_t rh, DXGI_FORMAT fmt, unsigned count) {
  if (S.feature && S.feat_w == w && S.feat_h == h && S.feat_rw == rw && S.feat_rh == rh && S.feat_fmt == fmt &&
      S.feat_count == count)
    return true;
  if (S.feature) {
    wait_d3d12(S.done_value);
    S.release(S.feature);
    S.feature = nullptr;
  }
  ngx::Params& p = S.create_params;
  p.reset();
  p.set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
  p.set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
  p.set(NVSDK_NGX_Parameter_Width, w);
  p.set(NVSDK_NGX_Parameter_Height, h);
  p.set(NVSDK_NGX_DLSSG_Parameter_Width, w);
  p.set(NVSDK_NGX_DLSSG_Parameter_Height, h);
  p.set(NVSDK_NGX_DLSSG_Parameter_BackbufferFormat, static_cast<unsigned int>(fmt));
  p.set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, count);
  p.set(NVSDK_NGX_DLSSG_Parameter_InternalWidth, rw);
  p.set(NVSDK_NGX_DLSSG_Parameter_InternalHeight, rh);
  p.set(NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 0u);
  for (const auto& kv : config().fg_create_extra) p.set(kv.first.c_str(), kv.second);
  NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
  int64_t t0 = now_us();
  submit([&](ID3D12GraphicsCommandList* cl) {
    r = S.create(cl, NVSDK_NGX_Feature_FrameGeneration, p.get(), &S.feature);
    return !NVSDK_NGX_FAILED(r);
  }, true);
  ACO_LOG("fg12: CreateFeature(FrameGeneration %ux%u, render %ux%u, format %d, count %u) -> %s (%.0f ms)", w, h, rw,
          rh, fmt, count, ngx::result_str(r), (now_us() - t0) / 1000.0);
  if (NVSDK_NGX_FAILED(r)) {
    S.feature = nullptr;
    return false;
  }
  S.feat_w = w;
  S.feat_h = h;
  S.feat_rw = rw;
  S.feat_rh = rh;
  S.feat_fmt = fmt;
  S.feat_count = count;
  S.reset = true;
  return true;
}

// Backbuffer-sized textures of every set: the real-frame copy, the colour input and `n_out` generated
// outputs (more are created when the multiplier grows, the surplus released when it shrinks). The
// game thread waits for the sets' evaluations (join + set fences) before calling this with a new
// n_out, and outputs above the new count are no longer read by any queued job.
bool ensure_textures(const D3D11_TEXTURE2D_DESC& bb, unsigned n_out) {
  DXGI_FORMAT f = storage_format(bb.Format);
  static unsigned have_out = 0;
  bool same = S.tex_w == bb.Width && S.tex_h == bb.Height && S.tex_fmt == f;
  if (same && have_out == n_out) return true;
  if (!same) {
    for (Set& s : S.sets) safe_release(s.real);
    S.tex_w = S.tex_h = 0;
    have_out = 0;
  }
  D3D11_TEXTURE2D_DESC d{};
  d.Width = bb.Width;
  d.Height = bb.Height;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = bb.Format;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  for (Set& s : S.sets) {
    if (!s.real && FAILED(S.dev->CreateTexture2D(&d, nullptr, &s.real))) return false;
    if (!make_shared(s.color, bb.Width, bb.Height, f, false)) return false;
    for (unsigned k = 0; k < unsigned(kMaxGen); ++k) {
      if (k >= n_out) s.out[k].release();
      else if (!make_shared(s.out[k], bb.Width, bb.Height, f, true)) return false;
    }
  }
  if (!make_shared(S.outreal, bb.Width, bb.Height, f, true)) return false;
  S.tex_w = bb.Width;
  S.tex_h = bb.Height;
  S.tex_fmt = f;
  have_out = n_out;
  ACO_LOG("fg12: shared textures %ux%u format %d (backbuffer format %d), %u generated output(s) per set", bb.Width,
          bb.Height, f, bb.Format, n_out);
  return true;
}

// A shared copy of a game texture (same size, colour-compatible format).
bool copy_in(Shared& s, ID3D11Resource* src) {
  ID3D11Texture2D* t = nullptr;
  if (!src || FAILED(src->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t)))) return false;
  D3D11_TEXTURE2D_DESC d;
  t->GetDesc(&d);
  t->Release();
  DXGI_FORMAT f = storage_format(d.Format);
  if (f != s.fmt || d.Width != s.w || d.Height != s.h) {
    if (f == DXGI_FORMAT_R24G8_TYPELESS || f == DXGI_FORMAT_D24_UNORM_S8_UINT || f == DXGI_FORMAT_R32G8X24_TYPELESS ||
        d.SampleDesc.Count != 1) {
      static bool logged = false;
      if (!logged) ACO_LOG("fg12: input format %d (samples %u) can't be copied into a shared texture", d.Format, d.SampleDesc.Count);
      logged = true;
      return false;
    }
    if (!make_shared(s, d.Width, d.Height, f, false)) return false;
    ACO_LOG("fg12: shared input %ux%u format %d (source format %d)", d.Width, d.Height, f, d.Format);
  }
  S.imm->CopyResource(s.t11, src);
  return true;
}

// ---- Camera for DLSS-G ([fg] camera=engine) ---------------------------------------------------
// The engine's matrices are column-vector (v' = M v); DLSS-G wants row-major for post-multiplication
// (v' = v M), i.e. the transpose. DLSS-G wants them without TAA jitter.
struct M4 {
  double m[4][4] = {};
  static M4 id() {
    M4 r;
    for (int i = 0; i < 4; ++i) r.m[i][i] = 1;
    return r;
  }
};
M4 mul(const M4& a, const M4& b) {
  M4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
  return r;
}
bool inverse(const M4& a, M4& out) {
  double t[4][8];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 8; ++j) t[i][j] = j < 4 ? a.m[i][j] : (j - 4 == i ? 1 : 0);
  for (int c = 0; c < 4; ++c) {
    int p = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(t[r][c]) > std::fabs(t[p][c])) p = r;
    if (std::fabs(t[p][c]) < 1e-30) return false;
    if (p != c)
      for (int j = 0; j < 8; ++j) std::swap(t[p][j], t[c][j]);
    double d = t[c][c];
    for (int j = 0; j < 8; ++j) t[c][j] /= d;
    for (int r = 0; r < 4; ++r)
      if (r != c) {
        double f = t[r][c];
        for (int j = 0; j < 8; ++j) t[r][j] -= f * t[c][j];
      }
  }
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) out.m[i][j] = t[i][j + 4];
  return true;
}
void store_t(const M4& a, float out[16]) {  // transpose into DLSS-G's row-vector layout
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) out[i * 4 + j] = float(a.m[j][i]);
}

struct CamOut {
  bool ok = false;
  float view_to_clip[16], clip_to_view[16], clip_to_prev[16], prev_to_clip[16];
  float pos[3], up[3], right[3], fwd[3];
  float near_z = 0.1f, far_z = 1e5f, fov = 1.0f, aspect = 1.6f;
  double jit_ndc[2] = {};  // jitter found in the projection (NDC units)
};

CamOut camera_from(const FgCamera& c, const Config& cfg) {
  CamOut o;
  if (!c.valid) return o;
  M4 c2v, v2w = M4::id(), pvp;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      c2v.m[i][j] = c.clip_to_view[i][j];
      pvp.m[i][j] = c.prev_view_proj[i][j];
      if (i < 3) v2w.m[i][j] = c.view_to_world[i][j];
    }
  M4 proj;
  if (!inverse(c2v, proj)) return o;
  // View space looks down -z (w = -z): a TAA jitter in the projection shows up as proj[0][2],
  // proj[1][2] (x_ndc = ... - proj[0][2]). Remove it from the current-frame matrices.
  M4 proj_u = proj, c2v_u = c2v, jcur = M4::id();
  o.jit_ndc[0] = -proj.m[0][2];
  o.jit_ndc[1] = -proj.m[1][2];
  if (cfg.fg_camera_unjitter) {
    proj_u.m[0][2] = proj_u.m[1][2] = 0;
    if (!inverse(proj_u, c2v_u)) return o;
    // jittered clip = J * unjittered clip (x_j = x_u + jit * w)
    jcur.m[0][3] = o.jit_ndc[0];
    jcur.m[1][3] = o.jit_ndc[1];
  }
  M4 c2p = mul(pvp, mul(v2w, mul(c2v, jcur)));  // unjittered current clip -> previous clip
  M4 p2c;
  if (!inverse(c2p, p2c)) return o;
  store_t(proj_u, o.view_to_clip);
  store_t(c2v_u, o.clip_to_view);
  store_t(c2p, o.clip_to_prev);
  store_t(p2c, o.prev_to_clip);
  auto norm = [](float v[3]) {
    float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0)
      for (int i = 0; i < 3; ++i) v[i] /= l;
  };
  for (int i = 0; i < 3; ++i) {
    o.right[i] = float(v2w.m[i][0]);
    o.up[i] = float(v2w.m[i][1]);
    o.fwd[i] = float(-v2w.m[i][2]);
    o.pos[i] = float(v2w.m[i][3]);
  }
  norm(o.right);
  norm(o.up);
  norm(o.fwd);
  // Near/far from the unjittered projection: view z where NDC depth is 1 (near, reversed Z) and 0.
  auto view_z = [&](double d) {
    double w = c2v_u.m[3][2] * d + c2v_u.m[3][3];
    double z = c2v_u.m[2][2] * d + c2v_u.m[2][3];
    return std::fabs(w) > 1e-30 ? -z / w : 1e30;
  };
  o.near_z = float(std::min(view_z(1.0), view_z(0.0)));
  double f = std::max(view_z(1.0), view_z(0.0));
  o.far_z = float(std::isfinite(f) && f < 1e7 ? f : 1e7);
  if (proj_u.m[1][1] != 0 && proj_u.m[0][0] != 0) {
    o.fov = float(2.0 * std::atan(1.0 / std::fabs(proj_u.m[1][1])));
    o.aspect = float(std::fabs(proj_u.m[1][1] / proj_u.m[0][0]));
  }
  o.ok = std::isfinite(o.near_z) && o.near_z > 0;
  return o;
}

void gen_fail(const char* why) {
  static int n = 0;
  if (n++ < 10) ACO_LOG("fg12: generate failed: %s", why);
}

// Evaluates DLSS-G for the current set: n_gen generated frames (MultiFrameCount = n_gen, index 1..n_gen,
// each its own submission so the present thread can show index 1 while the others are still being
// generated) on a feature created for feat_gen (>= n_gen; the count may change per frame without
// re-creating the feature, checked with tools/interop-test/dlssg_d3d12_test.cpp).
bool generate(const FgInputs& in, ID3D11Texture2D* bb, unsigned n_gen, unsigned feat_gen) {
  const Config& cfg = config();
  Set& st = *S.cur;
  st.n_gen = 0;
  S.imm->CopyResource(st.color.t11, bb);
  if (!copy_in(st.depth, in.depth) || !copy_in(st.mv, in.mv)) { gen_fail("inputs"); return false; }
  bool hudless = cfg.fg_hudless && in.hudless;
  if (hudless) {
    D3D11_TEXTURE2D_DESC hd;
    in.hudless->GetDesc(&hd);
    // Only a HUD-less image that matches the backbuffer (size and format) is meaningful to DLSS-G.
    hudless = hd.Width == S.tex_w && hd.Height == S.tex_h && storage_format(hd.Format) == S.tex_fmt &&
              copy_in(st.hudless, in.hudless);
  }
  if (!ensure_feature(S.tex_w, S.tex_h, in.render_w, in.render_h, S.tex_fmt, feat_gen)) { gen_fail("feature"); return false; }

  ngx::Params& p = S.eval_params;
  p.reset();
  p.set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, st.color.t12);
  p.set(NVSDK_NGX_DLSSG_Parameter_Depth, st.depth.t12);
  p.set(NVSDK_NGX_DLSSG_Parameter_MVecs, st.mv.t12);
  if (hudless) {
    p.set(NVSDK_NGX_DLSSG_Parameter_HUDLess, st.hudless.t12);
    p.set(NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectWidth, S.tex_w);
    p.set(NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectHeight, S.tex_h);
  }
  if (cfg.fg_show == "outreal" || cfg.fg_show == "compare") {
    p.set(NVSDK_NGX_DLSSG_Parameter_OutputReal, S.outreal.t12);
    p.set(NVSDK_NGX_DLSSG_Parameter_OutputRealSubrectWidth, S.tex_w);
    p.set(NVSDK_NGX_DLSSG_Parameter_OutputRealSubrectHeight, S.tex_h);
  }
  // Camera: the engine's matrices ([fg] camera=engine) or identity + a synthetic basis.
  static CamOut cam;
  cam = cfg.fg_camera == "engine" ? camera_from(in.cam, cfg) : CamOut{};
  if (cam.ok) {
    p.set(NVSDK_NGX_DLSSG_Parameter_CameraViewToClip, static_cast<void*>(cam.view_to_clip));
    p.set(NVSDK_NGX_DLSSG_Parameter_ClipToCameraView, static_cast<void*>(cam.clip_to_view));
    p.set(NVSDK_NGX_DLSSG_Parameter_ClipToPrevClip, static_cast<void*>(cam.clip_to_prev));
    p.set(NVSDK_NGX_DLSSG_Parameter_PrevClipToClip, static_cast<void*>(cam.prev_to_clip));
    p.set(NVSDK_NGX_DLSSG_Parameter_ClipToLensClip, const_cast<void*>(static_cast<const void*>(kIdentity)));
  } else {
    for (const char* m : {NVSDK_NGX_DLSSG_Parameter_CameraViewToClip, NVSDK_NGX_DLSSG_Parameter_ClipToCameraView,
                          NVSDK_NGX_DLSSG_Parameter_ClipToLensClip, NVSDK_NGX_DLSSG_Parameter_ClipToPrevClip,
                          NVSDK_NGX_DLSSG_Parameter_PrevClipToClip})
      p.set(m, const_cast<void*>(static_cast<const void*>(kIdentity)));
    // A valid (if synthetic) camera frame: an all-zero basis is "not a valid frame" for DLSS-G.
    cam.pos[0] = cam.pos[1] = cam.pos[2] = 0;
    cam.up[0] = 0, cam.up[1] = 1, cam.up[2] = 0;
    cam.right[0] = 1, cam.right[1] = 0, cam.right[2] = 0;
    cam.fwd[0] = 0, cam.fwd[1] = 0, cam.fwd[2] = 1;
    cam.near_z = 0.1f, cam.far_z = 100000.0f, cam.fov = 1.0f, cam.aspect = float(S.tex_w) / float(S.tex_h);
  }
  {
    static int64_t last = 0;
    if (cfg.fg_camera == "engine" && now_us() - last > 10000000) {
      last = now_us();
      ACO_LOG("fg12: camera %s: pos %.2f %.2f %.2f fwd %.3f %.3f %.3f near %.3f far %.0f fov %.1f deg aspect %.3f; "
              "projection jitter %.3f,%.3f px vs engine jitter %.3f,%.3f px; clip->prev diag %.4f %.4f t %.5f %.5f",
              cam.ok ? "engine" : (in.cam.valid ? "INVALID" : "not read yet"), cam.pos[0], cam.pos[1], cam.pos[2],
              cam.fwd[0], cam.fwd[1], cam.fwd[2], cam.near_z, cam.far_z, cam.fov * 57.2958f, cam.aspect,
              cam.jit_ndc[0] * in.render_w / 2, -cam.jit_ndc[1] * in.render_h / 2, in.jitter_x, in.jitter_y,
              cam.clip_to_prev[0], cam.clip_to_prev[5], cam.clip_to_prev[12], cam.clip_to_prev[13]);
    }
  }
  // Our motion vectors are in UV units (1 = full screen); [fg] mv_units=px scales them to render pixels.
  const bool mv_px = cfg.fg_mv_units == "px";
  p.set(NVSDK_NGX_DLSSG_Parameter_MvecScaleX, cfg.fg_mv_scale * (mv_px ? float(in.render_w) : 1.0f));
  p.set(NVSDK_NGX_DLSSG_Parameter_MvecScaleY, cfg.fg_mv_scale * (mv_px ? float(in.render_h) : 1.0f));
  p.set(NVSDK_NGX_DLSSG_Parameter_JitterOffsetX, in.jitter_x);
  p.set(NVSDK_NGX_DLSSG_Parameter_JitterOffsetY, in.jitter_y);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraPosX, cam.pos[0]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraPosY, cam.pos[1]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraPosZ, cam.pos[2]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraUpX, cam.up[0]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraUpY, cam.up[1]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraUpZ, cam.up[2]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraRightX, cam.right[0]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraRightY, cam.right[1]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraRightZ, cam.right[2]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraFwdX, cam.fwd[0]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraFwdY, cam.fwd[1]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraFwdZ, cam.fwd[2]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetX, 0.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetY, 0.0f);
  // Invalid-MV marker: not 0 (that would mark every static pixel invalid); Streamline uses FLT_MIN.
  p.set(NVSDK_NGX_DLSSG_Parameter_MvecInvalidValue, 1.17549435e-38f);
  p.set(NVSDK_NGX_DLSSG_Parameter_LinearizedDepth_Scale, 1.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_LinearizedDepth_NearFarPartition, 600.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_AutomodeOverrideReset, 0u);
  p.set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, ++S.backbuffer_id);
  if (float hz = display_hz(); hz > 0) p.set("DLSSG.RefreshRate", hz);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraNear, cam.near_z);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraFar, cam.far_z);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraFOV, cam.fov);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraAspectRatio, cam.aspect);
  p.set(NVSDK_NGX_DLSSG_Parameter_ColorBuffersHDR, 0u);
  p.set(NVSDK_NGX_DLSSG_Parameter_DepthInverted, 1u);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraMotionIncluded, 1u);
  p.set(NVSDK_NGX_DLSSG_Parameter_Reset, S.reset ? 1u : 0u);
  if (S.reset) ++S.perf_resets;
  p.set(NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames, 0u);
  p.set(NVSDK_NGX_DLSSG_Parameter_OrthoProjection, 0u);
  p.set(NVSDK_NGX_DLSSG_Parameter_MvecDilated, 0u);
  p.set(NVSDK_NGX_DLSSG_Parameter_MenuDetectionEnabled, 0u);
  p.set(NVSDK_NGX_DLSSG_Parameter_MinRelativeLinearDepthObjectSeparation, 40.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectWidth, in.render_w);
  p.set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectHeight, in.render_h);
  p.set(NVSDK_NGX_DLSSG_Parameter_DepthSubrectWidth, in.render_w);
  p.set(NVSDK_NGX_DLSSG_Parameter_DepthSubrectHeight, in.render_h);
  p.set(NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectWidth, S.tex_w);
  p.set(NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectHeight, S.tex_h);
  p.set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectWidth, S.tex_w);
  p.set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectHeight, S.tex_h);
  for (const auto& kv : cfg.fg_extra) p.set(kv.first.c_str(), kv.second);

  NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
  int64_t t0 = now_us();
  // [fg] skip (bisecting): d3d12 = no D3D12 submission/fences at all, eval = submit without DLSS-G.
  if (cfg.fg_skip == "d3d12") {
    st.n_gen = n_gen;
    return true;
  }
  const bool skip_eval = cfg.fg_skip == "eval";
  bool ok = true;
  for (unsigned i = 1; ok && i <= n_gen; ++i) {
    ok = submit([&](ID3D12GraphicsCommandList* cl) {
      if (skip_eval) {
        r = NVSDK_NGX_Result_Success;
        return true;
      }
      p.set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, n_gen);
      p.set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, i);
      p.set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolated, st.out[i - 1].t12);
      r = S.evaluate(cl, S.feature, p.get(), nullptr);
      if (NVSDK_NGX_FAILED(r)) {
        static int n = 0;
        if (n++ < 5) ACO_LOG("fg12: Evaluate(index %u of %u) -> %s", i, n_gen, ngx::result_str(r));
        return false;
      }
      return true;
    }, false, i == 1);
    st.gen_v[i - 1] = S.last_v;
    st.gen_slot[i - 1] = S.eval_slot;
    st.gen_ts_old[i - 1] = S.eval_ts_old;
    if (ok) st.n_gen = i;
  }
  st.eval_v = S.last_v;
  S.perf_eval_cpu_ms += (now_us() - t0) / 1000.0;
  static int logged = 0;
  if (logged < 3 || (!ok && logged < 20)) {
    ++logged;
    ACO_LOG("fg12: Evaluate x%u -> %s (reset=%d, render %ux%u, jitter %.3f,%.3f, hudless %d)", n_gen, ngx::result_str(r),
            S.reset, in.render_w, in.render_h, in.jitter_x, in.jitter_y, hudless);
  }
  S.reset = false;
  return ok;
}

void perf_report() {
  int64_t now = now_us();
  if (!S.perf_last) S.perf_last = now;
  double secs = (now - S.perf_last) / 1e6;
  if (secs < 2.0) return;
  // GPU: per real frame (all of its generated frames) and per generated frame (one evaluation).
  ACO_LOG("perf: fg real %.1f fps + generated %.1f fps = %.1f presents/s (fail %u, resets %u, cpu waits %u), eval CPU "
          "%.2f ms GPU %.3f ms/frame (%.3f ms per generated frame, x%.2f) [d3d12]",
          S.perf_real / secs, S.perf_generated / secs, (S.perf_real + S.perf_generated) / secs, S.perf_fail,
          S.perf_resets, S.perf_cpu_waits, S.perf_real ? S.perf_eval_cpu_ms / S.perf_real : 0.0,
          S.perf_real ? S.perf_gpu_ms / S.perf_real : 0.0, S.perf_gpu_n ? S.perf_gpu_ms / S.perf_gpu_n : 0.0,
          S.perf_real ? 1.0 + double(S.perf_generated) / S.perf_real : 0.0);
  S.perf_last = now;
  S.perf_real = S.perf_generated = S.perf_fail = S.perf_resets = S.perf_cpu_waits = 0;
  S.perf_eval_cpu_ms = S.perf_gpu_ms = 0;
  S.perf_gpu_n = 0;
}

// A GPU timestamp of our D3D12 queue in now_us() time (ID3D12CommandQueue::GetClockCalibration,
// refreshed every second). Present thread only.
bool gpu_to_us(UINT64 ts, int64_t* out) {
  static UINT64 cal_gpu = 0, cal_cpu = 0;
  static int64_t cal_at = 0;
  static LARGE_INTEGER qf = [] {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return v;
  }();
  if (!S.q || S.ts_freq <= 0 || !ts) return false;
  int64_t now = now_us();
  if (!cal_at || now - cal_at > 1000000) {
    UINT64 g = 0, c = 0;
    if (FAILED(S.q->GetClockCalibration(&g, &c)) || !g || !c) return false;
    cal_gpu = g, cal_cpu = c, cal_at = now;
  }
  double cpu_us = double(cal_cpu) * 1e6 / double(qf.QuadPart);
  *out = int64_t(cpu_us + (double(int64_t(ts - cal_gpu)) * 1e6 / S.ts_freq));
  return true;
}

void wait_until(int64_t t_us) {
  for (;;) {
    int64_t left = t_us - now_us();
    if (left <= 0) return;
    if (left > 2000) Sleep(static_cast<DWORD>((left - 1500) / 1000));
    else YieldProcessor();
  }
}

bool verify_textures() {
  if (S.verify_prev && S.stage[0]) return true;
  D3D11_TEXTURE2D_DESC d{};
  S.cur->real->GetDesc(&d);
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (!S.verify_prev && FAILED(S.dev->CreateTexture2D(&d, nullptr, &S.verify_prev))) return false;
  d.Format = S.tex_fmt;
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  for (auto& s : S.stage)
    if (!s && FAILED(S.dev->CreateTexture2D(&d, nullptr, &s))) return false;
  return true;
}

void verify_read() {
  std::vector<std::vector<uint8_t>> img(3);
  for (int i = 0; i < 3; ++i) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(S.imm->Map(S.stage[i], 0, D3D11_MAP_READ, 0, &m))) return;
    img[i].resize(size_t(S.tex_w) * S.tex_h * 3);
    for (uint32_t y = 0; y < S.tex_h; ++y) {
      const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
      for (uint32_t x = 0; x < S.tex_w; ++x) {
        uint8_t* o = &img[i][(size_t(y) * S.tex_w + x) * 3];
        bool bgra = S.tex_fmt == DXGI_FORMAT_B8G8R8A8_UNORM;
        o[0] = row[x * 4 + (bgra ? 2 : 0)];
        o[1] = row[x * 4 + 1];
        o[2] = row[x * 4 + (bgra ? 0 : 2)];
      }
    }
    S.imm->Unmap(S.stage[i], 0);
  }
  // Mean absolute difference (0..255) of each pair, and the share of pixels where the generated
  // frame is between its neighbours (per channel, with some slack) while they differ.
  auto mad = [&](int a, int b) {
    double s = 0;
    for (size_t k = 0; k < img[a].size(); ++k) s += std::abs(int(img[a][k]) - int(img[b][k]));
    return s / img[a].size();
  };
  size_t moving = 0, between = 0;
  for (size_t k = 0; k < img[0].size(); ++k) {
    int p = img[0][k], g = img[1][k], c = img[2][k];
    if (std::abs(p - c) < 24) continue;
    ++moving;
    if (g >= std::min(p, c) - 4 && g <= std::max(p, c) + 4 && std::abs(g - p) > 2 && std::abs(g - c) > 2) ++between;
  }
  ACO_LOG("fg12 verify #%d: |gen-prev| %.2f  |gen-cur| %.2f  |prev-cur| %.2f  (moving %.1f%% of samples, generated "
          "strictly between neighbours in %.1f%% of those)",
          S.verify_n, mad(1, 0), mad(1, 2), mad(0, 2), 100.0 * moving / img[0].size(),
          moving ? 100.0 * between / moving : 0.0);
  std::wstring dir = out_dir() + L"\\fgverify";
  CreateDirectoryW(dir.c_str(), nullptr);
  const char* names[3] = {"prev", "gen", "cur"};
  for (int i = 0; i < 3; ++i) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\%02d_%hs.ppm", dir.c_str(), S.verify_n % 10, names[i]);
    if (FILE* f = _wfopen(path, L"wb")) {
      fprintf(f, "P6\n%u %u\n255\n", S.tex_w, S.tex_h);
      fwrite(img[i].data(), 1, img[i].size(), f);
      fclose(f);
    }
  }
  ++S.verify_n;
}

// What goes into the backbuffer for the generated (first) or real (second) present.
void fill(ID3D11Texture2D* dst, bool generated_slot, const std::string& show, ID3D11Texture2D* gen,
          ID3D11Texture2D* real) {
  if (show == "compare" || show == "compare_gen") {
    // Debug: left half the real frame, right half OutputReal / the generated frame.
    D3D11_BOX left{0, 0, 0, S.tex_w / 2, S.tex_h, 1};
    S.imm->CopySubresourceRegion(dst, 0, 0, 0, 0, real, 0, &left);
    S.imm->CopySubresourceRegion(dst, 0, S.tex_w / 2, 0, 0, show == "compare_gen" ? gen : S.outreal.t11, 0, &left);
    return;
  }
  ID3D11Texture2D* src = generated_slot ? (show == "real" ? real : show == "outreal" ? S.outreal.t11 : gen)
                                        : (show == "generated" ? gen : show == "outreal" ? S.outreal.t11 : real);
  S.imm->CopyResource(dst, src);
}

// ---- Present trace (`touch <out>/fgtrace-start`) -------------------------------------------------------
// For frame-pacing analysis: the next [fg] trace_presents presents get timestamps (game Present
// arrival, our Present call and its return, QPC us) and the first [fg] trace_shots of them a copy
// of a horizontal band of exactly what was presented, in present order. Written to
// <out>/fgtrace/{presents.csv,bands.bin} once complete; tools/fg-pacing analyses them.
struct TraceEv {
  int64_t arrive, call, ret;
  uint64_t frame;
  char kind;  // N normal (FG off / not ready), G generated, R real
  int32_t half_us, interval_us, shot;
  int32_t gi, gn;  // generated frame index (1..gn-1; 0 for R/N) and the job's multiplier
};
struct Tracer {
  std::mutex m;
  std::atomic<bool> active{false};
  bool complete = false;
  std::vector<TraceEv> ev;
  size_t max_ev = 0;
  std::vector<ID3D11Texture2D*> stage;
  size_t shots = 0;
  UINT w = 0, h = 0, y0 = 0;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
  int64_t last_check = 0;
} T;

// Game thread: start a trace when <out>/fgtrace appears; write it out once complete.
void trace_poll(ID3D11Texture2D* bb);

// Before a Present: claims an event slot and copies the band of `bb` (what is about to be presented).
int trace_begin(char kind, uint64_t frame, int64_t arrive, int64_t half_us, ID3D11Texture2D* bb,
                ID3D11DeviceContext* ctx, int gi = 0, int gn = 1) {
  if (!T.active.load(std::memory_order_relaxed)) return -1;
  std::lock_guard<std::mutex> lk(T.m);
  if (T.complete || T.ev.size() >= T.max_ev) return -1;
  TraceEv e{arrive, now_us(), 0, frame, kind, int32_t(half_us), int32_t(S.frame_interval_us), -1, gi, gn};
  if (T.shots < T.stage.size() && bb) {
    D3D11_BOX box{0, T.y0, 0, T.w, T.y0 + T.h, 1};
    ctx->CopySubresourceRegion(T.stage[T.shots], 0, 0, 0, 0, bb, 0, &box);
    e.shot = int32_t(T.shots++);
    e.call = now_us();
  }
  T.ev.push_back(e);
  return int(T.ev.size() - 1);
}

void trace_end(int idx) {
  if (idx < 0) return;
  std::lock_guard<std::mutex> lk(T.m);
  T.ev[size_t(idx)].ret = now_us();
  if (T.ev.size() >= T.max_ev) T.complete = true;
}

// ---- Present thread and proxy backbuffer ([fg] present_thread=1) ---------------------------------
// The game renders into a backbuffer of ours (GetBuffer returns it); the swapchain's own backbuffer
// is only written right before each real Present, here or on the present thread. That lets the
// present thread show the real frame half a frame after the generated one while the game already
// renders the next frame, without the game overwriting the swapchain image in between. The D3D11
// immediate context is shared by both threads, so it runs with multithread protection.
HANDLE g_exc_file = INVALID_HANDLE_VALUE;  // see exception_logger
DWORD g_present_tid = 0;

struct Presenter {
  int proxy = -1;  // decided at the first GetBuffer: 0 off, 1 on
  PFN_GetBuffer real_get_buffer = nullptr;
  ID3D11Texture2D* proxy_bb = nullptr;
  ID3D11DeviceContext* ctx = nullptr;  // immediate context, for plain presents before FG is ready
  std::mutex m;
  std::condition_variable cv;
  bool job = false, busy = false;
  IDXGISwapChain* sc = nullptr;
  PFN_RealPresent present = nullptr;
  UINT sync = 0, flags = 0;
  int64_t half_interval_us = 0;
  std::string show;
  bool gate = true;  // [fg] present_gate
  std::atomic<HRESULT> hr{S_OK};
  std::atomic<int> init_state{0};  // 0 not started, 1 running, 2 ready, 3 failed
  int64_t arrive_us = 0;  // when the game's Present reached us
  UINT64 wait_value = 0;  // the generated frame is ready when the D3D12 fence reaches this
  ID3D11Texture2D* gen_tex = nullptr;   // this job's textures (one of the two sets)
  ID3D11Texture2D* real_tex = nullptr;
  std::atomic<uint32_t> gen_dropped{0};  // generated frame not ready in time: real frame only
  // The fence said "done" but the evaluation's end timestamp hadn't been written yet (the fence
  // completed before the GPU work it follows); and how often [fg] ts_guard then timed out.
  std::atomic<uint32_t> gen_early{0}, gen_early_timeout{0}, gen_checked{0};
  double perf_join_ms = 0;
  uint32_t perf_join_n = 0;
  // Present-thread timing: when the generated / real present calls were made after the game's
  // Present arrived (extra display latency of the real frame).
  std::atomic<int64_t> delay_gen_us{0}, delay_real_us{0};
  std::atomic<uint32_t> delay_n{0};
  // Per-job CPU time of the present thread's steps: pick-up after enqueue, copy into the swapchain
  // backbuffer (includes waiting for the D3D11 context lock), the real IDXGISwapChain::Present call,
  // the pacing wait - for the generated (g) and real (r) present.
  std::atomic<int64_t> seg_us[6] = {};  // pickup, fill_g, present_g, wait, fill_r, present_r
  int64_t last_real_call = 0;  // present thread: when the last real Present was called
  std::atomic<int64_t> ready_lag_sum{0};  // [fg] auto_cap inputs
  std::atomic<uint32_t> ready_lag_n{0};
  // Where that lag comes from (first generated frame, GPU timestamps mapped to QPC): game's Present ->
  // DLSS-G starts on the GPU (game GPU work, input copies, vkd3d's CPU-side resolution of the shared
  // fence wait, GPU scheduling) | its GPU duration | GPU end -> present thread awake (fence event).
  std::atomic<int64_t> lag_start_sum{0}, lag_gpu_sum{0}, lag_wake_sum{0};
  std::atomic<uint32_t> lag_split_n{0};
  std::atomic<int64_t> gpu_lag_sum{0};  // game Present -> DLSS-G GPU end (auto cap with [fg] auto_cap_lag_from=gpu)
  std::atomic<uint32_t> gpu_lag_n{0};
  uint32_t cap_join_n = 0;
} P;

ID3D11Texture2D* real_backbuffer(IDXGISwapChain* sc) {
  ID3D11Texture2D* bb = nullptr;
  HRESULT hr = P.real_get_buffer ? P.real_get_buffer(sc, 0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))
                                 : sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb));
  return SUCCEEDED(hr) ? bb : nullptr;
}

// One frame's presents (its generated frames in order, then the real one) for the present thread.
struct Job {
  IDXGISwapChain* sc = nullptr;
  PFN_RealPresent present = nullptr;
  UINT sync = 0, flags = 0;
  // Pacing step: consecutive presents this far apart (real-frame interval / multiplier; with 2x half
  // an interval). 0 = no pacing.
  int64_t half_us = 0;
  int64_t arrive_us = 0;    // when the game's Present reached us
  unsigned n_gen = 0;       // generated frames (multiplier - 1)
  // Per generated index: the frame is ready when the D3D12 fence reaches wait_value; its texture;
  // ring slot of its evaluation (timestamp check, -1 = none) and that slot's end timestamp before it.
  UINT64 wait_value[kMaxGen] = {};
  ID3D11Texture2D* gen[kMaxGen] = {};
  int ts_slot[kMaxGen] = {};
  UINT64 ts_old[kMaxGen] = {};
  ID3D11Texture2D* real = nullptr;
  std::string show;
  bool gate = true;
  bool skip_gen = false;    // superseded before it started: present only the real frame
  uint64_t frame = 0;       // BackbufferFrameID of this job's evaluation (trace)
  int pace = 0;             // [fg] pace: 0 ready, 1 smooth, 2 arrival, 3 content
  double relax = 0.5;       // [fg] pace_relax
  int64_t dt_us = 0;        // game's frame interval before this frame (pace=content)
};
std::deque<Job> g_jobs;  // guarded by P.m
int g_active = 0;        // jobs being processed (0/1), guarded by P.m

// Waits until generated frame k of the job is done on the GPU (fence, then the end-timestamp check).
bool job_gen_ready(const Job& j, unsigned k, HANDLE event) {
  if (!wait_eval(j.wait_value[k], event, 100)) return false;
  if (j.ts_slot[k] >= 0 && S.ts_map) {
    // The evaluation's command list ends by resolving its end timestamp into this slot: until the
    // value changes, the GPU hasn't finished it, whatever the fence says.
    const volatile UINT64* end = &S.ts_map[j.ts_slot[k] * 2 + 1];
    ++P.gen_checked;
    if (*end == j.ts_old[k]) {
      ++P.gen_early;
      if (config().fg_ts_guard) {
        int64_t deadline = now_us() + 50000;
        while (*end == j.ts_old[k] && now_us() < deadline) Sleep(0);
        if (*end == j.ts_old[k]) ++P.gen_early_timeout;
      }
    }
  }
  return true;
}

void present_loop() {
  g_present_tid = GetCurrentThreadId();
  g_ctx_gate_exempt = 1;
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  int64_t lag_us = 0, lag_c = 0;  // pace=arrival / pace=content lags
  for (;;) {
    std::unique_lock<std::mutex> lk(P.m);
    P.cv.wait(lk, [] { return !g_jobs.empty(); });
    Job j = g_jobs.front();
    g_jobs.pop_front();
    g_active = 1;
    lk.unlock();

    HRESULT hr = S_OK;
    const unsigned n = j.skip_gen ? 0 : j.n_gen;
    const int64_t mult = int64_t(j.n_gen) + 1;
    const int64_t step = j.half_us;
    if (j.skip_gen) P.gen_dropped += j.n_gen;
    // Timeline of this job: pickup .. first generated present call (t_first), copies / Presents of the
    // generated frames, waits between them, the real frame's copy and Present.
    int64_t t_first = 0, t_gen_ret = 0, prev_call = 0, prev_ret = 0;
    int64_t copy_g = 0, pres_g = 0, real_target = 0;
    unsigned shown = 0;
    int64_t dt = 0;
    for (unsigned k = 0; k < n; ++k) {
      if (!job_gen_ready(j, k, event)) {
        P.gen_dropped += n - k;  // the rest would only be later
        break;
      }
      const int64_t t_ready = now_us();
      if (k == 0) {  // auto cap: how long after the game's Present DLSS-G was done
        P.ready_lag_sum += t_ready - j.arrive_us;
        ++P.ready_lag_n;
        int64_t g0 = 0, g1 = 0;
        if (j.ts_slot[0] >= 0 && gpu_to_us(S.ts_map[j.ts_slot[0] * 2], &g0) &&
            gpu_to_us(S.ts_map[j.ts_slot[0] * 2 + 1], &g1) && g1 >= g0 && g1 <= t_ready + 2000) {
          P.lag_start_sum += g0 - j.arrive_us;
          P.lag_gpu_sum += g1 - g0;
          P.lag_wake_sum += t_ready - g1;
          ++P.lag_split_n;
          P.gpu_lag_sum += g1 - j.arrive_us;
          ++P.gpu_lag_n;
        }
      }
      // Pacing ([fg] pace). The first generated frame - ready: as soon as DLSS-G is done. smooth: not
      // sooner than one step after the previous real present, minus a share ([fg] pace_relax) of how
      // early DLSS-G finished relative to that, so a delay never sticks. arrival: at a fixed lag after
      // the game's Present (lag = recent maximum of DLSS-G ready - arrival), i.e. locked to the game's
      // own frame cadence. content: every present on the game's own timeline, a fixed lag L after it:
      // the real frame at arrive(N) + L, generated frame k (content k/mult of the way from N-1 to N) at
      // arrive(N) + L - dt (mult - k) / mult, with dt the *actual* interval since the previous game
      // frame. Later generated frames: one step after the previous present's call (ready: its return).
      if (step > 0) {
        int64_t target = 0;
        if (j.pace == 3) {
          if (k == 0) {
            dt = j.dt_us > 0 ? j.dt_us : mult * step;
            dt = dt < 3000 ? 3000 : dt > 50000 ? 50000 : dt;
            int64_t need = (t_ready - j.arrive_us) + dt * (mult - 1) / mult;  // smallest L that has it ready in time
            lag_c = need > lag_c ? need : lag_c - (lag_c - need) / 64;
            real_target = j.arrive_us + lag_c;
          }
          target = real_target - dt * (mult - 1 - int64_t(k)) / mult;
          if (target > t_ready && target - t_ready < 100000) wait_until(target);
        } else {
          if (k == 0) {
            int64_t ready_lag = t_ready - j.arrive_us;
            lag_us = ready_lag > lag_us ? ready_lag : lag_us - (lag_us - ready_lag) / 64;
            if (j.pace == 1 && P.last_real_call)
              target = P.last_real_call + step - int64_t(j.relax * double(P.last_real_call + step - t_ready));
            else if (j.pace == 2)
              target = j.arrive_us + lag_us;
          } else {
            target = (j.pace ? prev_call : prev_ret) + step;
          }
          if (target > t_ready && target - t_ready < 2 * step) wait_until(target);
        }
      }
      ID3D11Texture2D* bb = real_backbuffer(j.sc);
      if (!bb) break;
      int64_t c0 = now_us();
      if (j.gate) g_ctx_gate.fetch_add(1);
      fill(bb, true, j.show, j.gen[k], j.real);
      int ti = trace_begin('G', j.frame, j.arrive_us, step, bb, S.imm, int(k) + 1, int(mult));
      bb->Release();
      int64_t c1 = now_us();
      hr = j.present(j.sc, j.sync, j.flags);
      trace_end(ti);
      if (j.gate) g_ctx_gate.fetch_sub(1);
      prev_ret = now_us();
      prev_call = c1;
      if (!shown) t_first = c0, t_gen_ret = prev_ret;
      copy_g += c1 - c0;
      pres_g += prev_ret - c1;
      ++shown;
    }
    const int64_t t_wait = now_us();
    // The real frame one step after the last generated one: measured from when its Present was called
    // (pace=smooth/arrival) - a Present that blocks must not push the real frame later (pace=ready,
    // the original: from when it returned).
    if (shown && step > 0) {
      if (j.pace == 3 && real_target > 0) {
        if (real_target - now_us() < 100000) wait_until(real_target);
      } else {
        wait_until((j.pace ? prev_call : prev_ret) + step);
      }
    }
    int64_t t_real = now_us(), t4 = t_real;
    if (j.show == "skip_present2") {
      // bisecting: no real present
    } else if (ID3D11Texture2D* bb = real_backbuffer(j.sc)) {
      if (j.gate) g_ctx_gate.fetch_add(1);
      fill(bb, false, j.show, j.gen[0], j.real);
      int ti = trace_begin('R', j.frame, j.arrive_us, step, bb, S.imm, 0, int(mult));
      bb->Release();
      t4 = now_us();
      P.last_real_call = t4;
      HRESULT h2 = j.present(j.sc, j.sync, j.flags);
      trace_end(ti);
      if (j.gate) g_ctx_gate.fetch_sub(1);
      if (SUCCEEDED(hr)) hr = h2;
    }
    int64_t t5 = now_us();
    if (!shown) t_first = t_gen_ret = t_wait;
    // Waits between presents: from the first generated present's return to the real present, minus
    // the later generated presents' own copy/Present time.
    int64_t between = t_real - t_gen_ret;
    if (shown > 1) between -= (copy_g + pres_g) * int64_t(shown - 1) / int64_t(shown);
    P.seg_us[0] += t_first - j.arrive_us;
    P.seg_us[1] += copy_g;
    P.seg_us[2] += pres_g;
    P.seg_us[3] += between;
    P.seg_us[4] += t4 - t_real;
    P.seg_us[5] += t5 - t4;
    P.delay_gen_us += t_gen_ret - j.arrive_us;
    P.delay_real_us += t_real - j.arrive_us;
    ++P.delay_n;
    P.hr = hr;
    lk.lock();
    g_active = 0;
    P.cv.notify_all();
  }
}

// Waits (deadline 500 ms) until at most `outstanding` jobs are queued or running.
void join(size_t outstanding = 0) {
  std::unique_lock<std::mutex> lk(P.m);
  auto ok = [outstanding] { return g_jobs.size() + size_t(g_active) <= outstanding; };
  if (ok()) return;
  int64_t t0 = now_us();
  P.cv.wait_for(lk, std::chrono::milliseconds(500), ok);
  P.perf_join_ms += (now_us() - t0) / 1000.0;
  ++P.perf_join_n;
  ++P.cap_join_n;
}

// ---- Frame-rate cap and multiplier ([fg] auto_cap, count, target_hz) ------------------------------
// auto_cap: a real-frame-rate cap with headroom, found by feedback. Without headroom the GPU queue
// fills (DLSS-G runs on a second device the game's Reflex doesn't account for), DLSS-G finishes ~2
// frames after the game's Present, the present thread falls behind and the game blocks on it at
// irregular times: uneven game frame times = uneven motion, and ~10 ms more latency. With headroom
// DLSS-G is done well within a frame. Every second: if DLSS-G (the first generated frame) finished
// later than auto_cap_lag of a frame interval after the game's Present on average, or the game had to
// wait for the present thread, the headroom cap drops 4 %; if it finished within auto_cap_lag - 0.15
// and the game never waited, it rises 1 % (4 % when far below the target). Starts 2 s after FG
// starts, at 90 % of the rate then.
//
// Refresh target (Multi Frame Generation): presents = real fps x multiplier should not exceed the
// display's refresh ([fg] target_hz, default the detected refresh), so the cap is also at most
// target / multiplier (e.g. 165 Hz: 82.5 real fps at 2x, 55 at 3x, 41.25 at 4x). With VRR that gives
// one present per refresh at an even cadence. The headroom cap only rises while it is the binding
// one (otherwise it would climb unchecked while the refresh cap holds the rate).
//
// count=auto (dynamic multiplier): the multiplier is the smallest in [count_min, count_max] at which
// the refresh target fits under the headroom cap. One step up when the headroom cap stays below
// target / multiplier (2 s); one step down when the frame's own work (frame interval minus the time
// the limiter / Reflex slept) and DLSS-G's lag predict headroom for target / (multiplier - 1)
// (3 s, 15 % margin). A step down that has to be undone within 6 s blocks further steps down for
// 10 s, doubling up to 60 s.
std::atomic<float> g_auto_cap{0};
std::atomic<int> g_mult{2};

// Presents per second to aim for: [fg] target_hz, else the display's refresh.
float target_hz() {
  const Config& cfg = config();
  return cfg.fg_target_hz > 0 ? cfg.fg_target_hz : display_hz();
}

// Multiplier range: fixed count = [count, count]; auto = [count_min, count_max]; both within what
// DLSS-G supports (MultiFrameCountMax + 1).
void mult_range(int& lo, int& hi) {
  const Config& cfg = config();
  int caps = int(S.caps_max_gen) + 1;
  if (cfg.fg_count > 0) {
    lo = hi = std::max(2, std::min(cfg.fg_count, caps));
    return;
  }
  hi = std::max(2, std::min(cfg.fg_count_max, caps));
  lo = std::max(2, std::min(cfg.fg_count_min, hi));
}

struct Pacer {
  int64_t last = 0, since = 0;
  uint32_t frames = 0;
  uint64_t wait0 = 0;    // reflex_wait_us() at the last update
  float head = 0;        // headroom cap
  int mult = 0;
  int up_n = 0, down_n = 0;
  int64_t last_down = 0, block_until = 0, backoff = 10000000;
} PC;

void pacing_update(bool fg_active) {
  const Config& cfg = config();
  int64_t now = now_us();
  ++PC.frames;
  int lo, hi;
  mult_range(lo, hi);
  if (PC.mult < lo || PC.mult > hi) PC.mult = cfg.fg_count > 0 ? hi : lo;
  g_mult = PC.mult;
  if (!fg_active) {
    g_auto_cap = 0;
    PC.last = PC.since = 0;
    PC.head = 0;
    PC.up_n = PC.down_n = 0;
    return;
  }
  if (!PC.since) PC.since = now;
  if (now - PC.since < 2000000) return;  // let FG settle (and loading end) first
  if (!PC.last) {
    PC.last = now;
    PC.frames = 0;
    PC.wait0 = reflex_wait_us();
    P.ready_lag_sum = 0;
    P.ready_lag_n = 0;
    P.gpu_lag_sum = 0;
    P.gpu_lag_n = 0;
    P.cap_join_n = 0;
    return;
  }
  if (now - PC.last < 1000000) return;
  double secs = (now - PC.last) / 1e6;
  double fps = PC.frames / secs;
  uint32_t n = P.ready_lag_n.exchange(0);
  double lag_ready = n ? double(P.ready_lag_sum.exchange(0)) / n : 0;
  uint32_t gn = P.gpu_lag_n.exchange(0);
  double lag_gpu = gn ? double(P.gpu_lag_sum.exchange(0)) / gn : 0;
  // [fg] auto_cap_lag_from: ready = when the present thread had the first generated frame (includes
  // waiting for the previous job and the fence wake-up), gpu = when DLSS-G finished on the GPU.
  double lag = cfg.fg_auto_cap_lag_from == "gpu" && gn ? lag_gpu : lag_ready;
  uint32_t waits = P.cap_join_n;
  P.cap_join_n = 0;
  uint64_t w = reflex_wait_us();
  double slack = PC.frames ? double(w - PC.wait0) / PC.frames : 0;  // limiter / Reflex sleep per frame, us
  PC.wait0 = w;
  PC.frames = 0;
  PC.last = now;
  double interval = fps > 1 ? 1e6 / fps : 0;
  double ratio = interval > 0 ? lag / interval : 0;
  const float target = target_hz();
  const float lim = cfg.reflex_fps_limit;  // an explicit limit overrides the automatic cap
  const bool overloaded = ratio > cfg.fg_auto_cap_lag || waits > fps * 0.02;
  const int old_mult = PC.mult;
  float cap = 0;
  if (cfg.fg_auto_cap && lim <= 0) {
    auto refresh_cap = [&](int m) { return target > 0 ? target / float(m) : 1e9f; };
    float cur_cap = g_auto_cap.load();
    float r = refresh_cap(PC.mult);
    if (PC.head <= 0) {
      PC.head = static_cast<float>((cur_cap > 0 ? std::min<double>(cur_cap, fps) : fps) * 0.9);
    } else if (overloaded) {
      PC.head = static_cast<float>(std::min<double>(cur_cap > 0 ? cur_cap : PC.head, fps) * 0.96);
    } else if (PC.head <= r * 1.001f && waits == 0) {  // only while the headroom cap binds
      if (ratio < cfg.fg_auto_cap_lag - 0.35) PC.head *= 1.04f;
      else if (ratio < cfg.fg_auto_cap_lag - 0.15) PC.head *= 1.01f;
    }
    // (30 fps as before, lower only where the refresh target at the highest multiplier needs it)
    float flo = cfg.fg_auto_cap_min > 0 ? cfg.fg_auto_cap_min : std::min(30.0f, target > 0 ? target / float(hi) * 0.75f : 30.0f);
    float fhi = cfg.fg_auto_cap_max > 0 ? cfg.fg_auto_cap_max : 240.0f;
    PC.head = std::max(flo, std::min(fhi, PC.head));
    if (lo < hi && target > 0) {
      // Up: the headroom cap can't hold target / multiplier.
      if (PC.head < r * 0.97f && PC.mult < hi) {
        if (++PC.up_n >= 2) {
          if (now - PC.last_down < 6000000) {  // undoing a step down: back off
            PC.block_until = now + PC.backoff;
            PC.backoff = std::min<int64_t>(PC.backoff * 2, 60000000);
          }
          ++PC.mult;
        }
      } else {
        PC.up_n = 0;
      }
      // Down: the frame's own work and DLSS-G's lag leave headroom for target / (multiplier - 1).
      bool down = false;
      if (PC.mult == old_mult && PC.mult > lo && !overloaded && now >= PC.block_until && interval > 0) {
        double work = interval - slack;
        double need = target / double(PC.mult - 1);
        double ratio_next = ratio * PC.mult / (PC.mult - 1);
        down = work > 0 && 1e6 / work * 0.85 >= need && ratio_next < cfg.fg_auto_cap_lag - 0.15;
      }
      PC.down_n = down ? PC.down_n + 1 : 0;
      if (PC.down_n >= 3) {
        --PC.mult;
        PC.last_down = now;
        PC.head = std::max(PC.head, refresh_cap(PC.mult));
      }
      if (PC.mult != old_mult) PC.up_n = PC.down_n = 0;
      if (PC.last_down && now - PC.last_down > 20000000) PC.backoff = 10000000;  // a step down that held
    }
    cap = std::min(PC.head, refresh_cap(PC.mult));
  } else if (lo < hi && target > 0) {
    // Dynamic multiplier without the automatic cap: from the real rate (an explicit fps_limit, or
    // what the game reaches), presents at or just below the target.
    double base = lim > 0 ? lim : fps;
    int want = base > 0 ? int(std::floor(target / base + 0.1)) : lo;
    want = std::max(lo, std::min(hi, want));
    PC.down_n = want != PC.mult ? PC.down_n + 1 : 0;
    if (PC.down_n >= 3) PC.mult = want, PC.down_n = 0;
  }
  if (PC.mult < old_mult && !(cfg.fg_auto_cap && lim <= 0)) PC.last_down = now;
  g_mult = PC.mult;
  static float logged = 0;
  static int logged_mult = 0;
  if (std::fabs(cap - logged) > 3.0f || PC.mult != logged_mult) {
    ACO_LOG("fg12: auto cap %.1f fps, %dx (real %.1f fps, DLSS-G done %.1f ms = %.2f frames after the game's Present, game "
            "waited %u times, limiter/Reflex sleep %.1f ms/frame, headroom cap %.1f, target %.0f Hz; lag ready %.1f / GPU end %.1f ms)",
            cap, PC.mult, fps, lag / 1000.0, ratio, waits, slack / 1000.0, PC.head, target, lag_ready / 1000.0, lag_gpu / 1000.0);
    logged = cap;
    logged_mult = PC.mult;
  }
  g_auto_cap = cap;
}

void enqueue(IDXGISwapChain* sc, PFN_RealPresent present, UINT sync, UINT flags, int64_t step, const std::string& show,
             int64_t arrive) {
  static std::thread thread(present_loop);
  const Set& st = *S.cur;
  Job j;
  j.sc = sc;
  j.present = present;
  j.sync = sync;
  j.flags = flags;
  j.half_us = step;
  j.arrive_us = arrive;
  j.n_gen = st.n_gen;
  for (unsigned k = 0; k < st.n_gen; ++k) {
    j.wait_value[k] = config().fg_sync == "gpu" ? 0 : st.gen_v[k];
    j.gen[k] = st.out[k].t11;
    j.ts_slot[k] = st.gen_slot[k];
    j.ts_old[k] = st.gen_ts_old[k];
  }
  j.real = st.real;
  j.show = config().fg_skip == "present2" ? std::string("skip_present2") : show;
  j.gate = config().fg_present_gate;
  j.frame = S.backbuffer_id;
  const std::string& pm = config().fg_pace;
  j.pace = pm == "smooth" ? 1 : pm == "arrival" ? 2 : pm == "content" ? 3 : 0;
  static int64_t prev_arrive = 0;
  j.dt_us = prev_arrive ? arrive - prev_arrive : 0;
  prev_arrive = arrive;
  j.relax = config().fg_pace_relax;
  std::lock_guard<std::mutex> lk(P.m);
  // A job that hasn't started yet is late: its generated frames would only delay the real ones.
  for (Job& q : g_jobs) {
    q.skip_gen = true;
    q.half_us = 0;
  }
  g_jobs.push_back(j);
  P.cv.notify_all();
}

// Debug: first-chance fatal-looking exceptions to out/fg12_exceptions.txt (lock-free writes), as
// the game's own crash handler exits silently.
// (declared above)

LONG CALLBACK exception_logger(EXCEPTION_POINTERS* ep) {
  DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW && code != 0xE06D7363u &&
      code != 0xC0000409u && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
      code != 0x40000015u)
    return EXCEPTION_CONTINUE_SEARCH;
  static LONG n = 0;
  if (InterlockedIncrement(&n) > 50 || g_exc_file == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;
  void* addr = ep->ExceptionRecord->ExceptionAddress;
  HMODULE mod = nullptr;
  char name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCSTR>(addr), &mod))
    GetModuleFileNameA(mod, name, MAX_PATH);
  char line[768];
  int len = snprintf(line, sizeof(line), "%lld tid %lu%s code 0x%08lx at %p (%s+0x%llx) info %llx %llx\n",
                     static_cast<long long>(now_us()), GetCurrentThreadId(),
                     GetCurrentThreadId() == g_present_tid ? " [present thread]" : "", code, addr, name,
                     static_cast<unsigned long long>(static_cast<char*>(addr) - reinterpret_cast<char*>(mod)),
                     static_cast<unsigned long long>(ep->ExceptionRecord->NumberParameters > 0 ? ep->ExceptionRecord->ExceptionInformation[0] : 0),
                     static_cast<unsigned long long>(ep->ExceptionRecord->NumberParameters > 1 ? ep->ExceptionRecord->ExceptionInformation[1] : 0));
  DWORD w;
  if (len > 0) WriteFile(g_exc_file, line, static_cast<DWORD>(len), &w, nullptr);
  return EXCEPTION_CONTINUE_SEARCH;
}

void enable_multithread(ID3D11Device* dev) {
  g_exc_file = CreateFileW((out_dir() + L"\\fg12_exceptions.txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  AddVectoredExceptionHandler(1, exception_logger);
  ID3D11DeviceContext* ctx = nullptr;
  dev->GetImmediateContext(&ctx);
  bool ok = false;
  ID3D11Multithread* mt = nullptr;
  ID3D10Multithread* mt10 = nullptr;
  if (ctx && SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&mt)))) {
    mt->SetMultithreadProtected(TRUE);
    ok = mt->GetMultithreadProtected();
    mt->Release();
  } else if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(&mt10)))) {
    mt10->SetMultithreadProtected(TRUE);
    ok = mt10->GetMultithreadProtected();
    mt10->Release();
  }
  P.ctx = ctx;  // keeps the reference
  g_ctx_gate_imm = ctx;
  ACO_LOG("fg12: D3D11 multithread protection %s", ok ? "on" : "UNAVAILABLE");
}

void perf_join_report() {
  static int64_t last = 0;
  int64_t now = now_us();
  if (!last) last = now;
  if (now - last < 2000000) return;
  uint32_t n = P.delay_n.exchange(0);
  int64_t dg = P.delay_gen_us.exchange(0), dr = P.delay_real_us.exchange(0);
  ACO_LOG("perf: fg12 present thread: game waited for it %u times (%.2f ms avg); after the game's Present: generated "
          "presented +%.2f ms, real +%.2f ms; generated frames dropped %u",
          P.perf_join_n, P.perf_join_n ? P.perf_join_ms / P.perf_join_n : 0.0, n ? dg / 1000.0 / n : 0.0,
          n ? dr / 1000.0 / n : 0.0, P.gen_dropped.exchange(0));
  ACO_LOG("perf: fg12 completion check: %u of %u generated frames were about to be presented before DLSS-G's end "
          "timestamp (guard timeouts %u) [done_fence=%s ts_guard=%d]",
          P.gen_early.exchange(0), P.gen_checked.exchange(0), P.gen_early_timeout.exchange(0),
          S.priv12 && config().fg_done_fence == "private" ? "private" : "shared", config().fg_ts_guard ? 1 : 0);
  double s[6];
  for (int i = 0; i < 6; ++i) s[i] = n ? P.seg_us[i].exchange(0) / 1000.0 / n : 0.0;
  ACO_LOG("perf: fg12 present thread steps (ms/job): pickup %.2f | copy %.2f, Present %.2f (generated) | wait %.2f | "
          "copy %.2f, Present %.2f (real)", s[0], s[1], s[2], s[3], s[4], s[5]);
  uint32_t ln = P.lag_split_n.exchange(0);
  double l0 = P.lag_start_sum.exchange(0), l1 = P.lag_gpu_sum.exchange(0), l2 = P.lag_wake_sum.exchange(0);
  if (ln)
    ACO_LOG("perf: fg12 DLSS-G lag split (first generated frame, ms after the game's Present): GPU start +%.2f, GPU "
            "duration %.2f, present thread awake %.2f after the GPU end (%u frames, multiplier %dx)",
            l0 / 1000.0 / ln, l1 / 1000.0 / ln, l2 / 1000.0 / ln, ln, g_mult.load());
  P.perf_join_ms = 0;
  P.perf_join_n = 0;
  last = now;
}

void trace_poll(ID3D11Texture2D* bb) {
  const Config& cfg = config();
  int64_t now = now_us();
  if (!T.active) {
    if (now - T.last_check < 250000) return;
    T.last_check = now;
    std::wstring trigger = out_dir() + L"\\fgtrace-start";
    if (GetFileAttributesW(trigger.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileW(trigger.c_str());
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    std::lock_guard<std::mutex> lk(T.m);
    T.w = d.Width;
    T.h = std::min<UINT>(d.Height / 16, 128);
    T.y0 = d.Height * 9 / 32;  // above the player character in third person, below most of the sky
    T.fmt = d.Format;
    d.Height = T.h;
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    for (int i = 0; i < cfg.fg_trace_shots; ++i) {
      ID3D11Texture2D* t = nullptr;
      if (FAILED(S.dev ? S.dev->CreateTexture2D(&d, nullptr, &t) : E_FAIL)) break;
      T.stage.push_back(t);
    }
    T.ev.clear();
    T.ev.reserve(size_t(std::max(cfg.fg_trace_presents, 1)));
    T.max_ev = size_t(std::max(cfg.fg_trace_presents, 1));
    T.shots = 0;
    T.complete = false;
    T.active = true;
    ACO_LOG("fg12: trace started (%zu presents, %zu band shots %ux%u at y=%u)", T.max_ev, T.stage.size(), T.w, T.h, T.y0);
    return;
  }
  {
    std::lock_guard<std::mutex> lk(T.m);
    if (!T.complete) return;
  }
  join(0);
  std::wstring dir = out_dir() + L"\\fgtrace";
  CreateDirectoryW(dir.c_str(), nullptr);
  if (FILE* f = _wfopen((dir + L"\\presents.csv").c_str(), L"w")) {
    fprintf(f, "kind,frame,arrive_us,call_us,ret_us,half_us,interval_us,shot,gi,gn\n");
    for (const TraceEv& e : T.ev)
      fprintf(f, "%c,%llu,%lld,%lld,%lld,%d,%d,%d,%d,%d\n", e.kind, static_cast<unsigned long long>(e.frame),
              static_cast<long long>(e.arrive), static_cast<long long>(e.call), static_cast<long long>(e.ret), e.half_us,
              e.interval_us, e.shot, e.gi, e.gn);
    fclose(f);
  }
  if (FILE* f = _wfopen((dir + L"\\bands.bin").c_str(), L"wb")) {
    // Header: magic, width, height, count, bgra flag; then count * width * height * 4 bytes.
    bool bgra = T.fmt == DXGI_FORMAT_B8G8R8A8_UNORM || T.fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                T.fmt == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    uint32_t hdr[5] = {0x42474641u, T.w, T.h, uint32_t(T.shots), bgra ? 1u : 0u};
    fwrite(hdr, sizeof(hdr), 1, f);
    std::vector<uint8_t> row(size_t(T.w) * 4);
    for (size_t i = 0; i < T.shots; ++i) {
      D3D11_MAPPED_SUBRESOURCE m{};
      if (FAILED(S.imm->Map(T.stage[i], 0, D3D11_MAP_READ, 0, &m))) {
        std::fill(row.begin(), row.end(), 0);
        for (UINT y = 0; y < T.h; ++y) fwrite(row.data(), 1, row.size(), f);
        continue;
      }
      for (UINT y = 0; y < T.h; ++y) fwrite(static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, 1, row.size(), f);
      S.imm->Unmap(T.stage[i], 0);
    }
    fclose(f);
  }
  ACO_LOG("fg12: trace written (%zu presents, %zu shots) to out/fgtrace", T.ev.size(), T.shots);
  std::lock_guard<std::mutex> lk(T.m);
  for (auto*& t : T.stage) safe_release(t);
  T.stage.clear();
  T.active = false;
  T.complete = false;
}

} // namespace

HRESULT fg12_get_buffer(IDXGISwapChain* sc, UINT index, const _GUID& riid, void** out, PFN_GetBuffer real) {
  P.real_get_buffer = real;
  if (P.proxy < 0) {
    P.proxy = config().fg_mode == "d3d12" && config().fg_present_thread ? 1 : 0;
    if (P.proxy) {
      ID3D11Device* dev = nullptr;
      if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev)))) {
        enable_multithread(dev);
        dev->Release();
      }
      ACO_LOG("fg12: proxy backbuffer + present thread enabled");
    }
  }
  {
    // Diagnostics: DXVK makes each Present wait until the present this many presents back is done.
    static int64_t last = 0;
    if (now_us() - last > 10000000) {
      last = now_us();
      IDXGIDevice1* d1 = nullptr;
      UINT lat = 0;
      if (SUCCEEDED(sc->GetDevice(__uuidof(IDXGIDevice1), reinterpret_cast<void**>(&d1)))) {
        d1->GetMaximumFrameLatency(&lat);
        d1->Release();
      }
      int want = config().fg_frame_latency;
      if (want > 0 && config().fg_mode == "d3d12" && lat != UINT(want) &&
          SUCCEEDED(sc->GetDevice(__uuidof(IDXGIDevice1), reinterpret_cast<void**>(&d1)))) {
        d1->SetMaximumFrameLatency(want);
        d1->Release();
        ACO_LOG("fg12: maximum frame latency %u -> %d", lat, want);
      }
      ACO_LOG("fg12: game's maximum frame latency %u", lat);
    }
  }
  if (!P.proxy) return real(sc, index, riid, out);
  ID3D11Texture2D* swap_bb = nullptr;
  HRESULT hr = real(sc, 0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&swap_bb));
  if (FAILED(hr)) return hr;
  D3D11_TEXTURE2D_DESC d;
  swap_bb->GetDesc(&d);
  bool same = false;
  if (P.proxy_bb) {
    D3D11_TEXTURE2D_DESC pd;
    P.proxy_bb->GetDesc(&pd);
    same = pd.Width == d.Width && pd.Height == d.Height && pd.Format == d.Format;
  }
  if (!same) {
    safe_release(P.proxy_bb);
    ID3D11Device* dev = nullptr;
    swap_bb->GetDevice(&dev);
    d.MiscFlags = 0;
    d.CPUAccessFlags = 0;
    d.Usage = D3D11_USAGE_DEFAULT;
    hr = dev->CreateTexture2D(&d, nullptr, &P.proxy_bb);
    dev->Release();
    ACO_LOG("fg12: proxy backbuffer %ux%u format %d bind 0x%x -> 0x%08lx", d.Width, d.Height, d.Format, d.BindFlags, hr);
    if (FAILED(hr)) {
      P.proxy_bb = nullptr;
      swap_bb->Release();
      return E_FAIL;
    }
  }
  swap_bb->Release();
  return P.proxy_bb->QueryInterface(riid, out);
}

void fg12_before_resize() {
  if (P.proxy != 1) return;
  join();
  safe_release(P.proxy_bb);  // the game has released its references (DXGI requires it for the real ones)
}

bool fg12_proxy_active() { return P.proxy == 1; }

float fg12_auto_cap() { return g_auto_cap.load(); }

// The display's refresh rate for [fg] refresh_hz=0 / target_hz=0: the swapchain's fullscreen mode, else
// the current display mode of the output the swapchain is on (under Wine: the X/Xwayland mode).
// Re-checked every 10 s (mode switches).
static void detect_refresh(IDXGISwapChain* sc) {
  static int64_t last = 0;
  int64_t now = now_us();
  if (last && now - last < 10000000) return;
  last = now;
  float sc_hz = 0, mode_hz = 0;
  DXGI_SWAP_CHAIN_DESC d{};
  BOOL fullscreen = FALSE;
  if (SUCCEEDED(sc->GetDesc(&d)) && d.BufferDesc.RefreshRate.Denominator)
    sc_hz = float(d.BufferDesc.RefreshRate.Numerator) / float(d.BufferDesc.RefreshRate.Denominator);
  sc->GetFullscreenState(&fullscreen, nullptr);
  IDXGIOutput* out = nullptr;
  if (SUCCEEDED(sc->GetContainingOutput(&out)) && out) {
    DXGI_OUTPUT_DESC od{};
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (SUCCEEDED(out->GetDesc(&od)) && GetMonitorInfoW(od.Monitor, &mi) &&
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
      mode_hz = float(dm.dmDisplayFrequency);
    out->Release();
  }
  if (mode_hz <= 0) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
      mode_hz = float(dm.dmDisplayFrequency);
  }
  float hz = fullscreen && sc_hz > 1 ? sc_hz : mode_hz > 0 ? mode_hz : sc_hz > 1 ? sc_hz : 0;
  if (std::fabs(hz - g_display_hz.load()) > 0.5f) {
    ACO_LOG("fg12: display refresh %.2f Hz (swapchain mode %.2f Hz, %s; display mode %.0f Hz); [fg] refresh_hz=%.0f "
            "target_hz=%.0f -> target %.0f presents/s", hz, sc_hz, fullscreen ? "fullscreen" : "windowed", mode_hz,
            config().fg_refresh_hz, config().fg_target_hz, config().fg_target_hz > 0 ? config().fg_target_hz :
            config().fg_refresh_hz > 0 ? config().fg_refresh_hz : hz);
    g_display_hz = hz;
  }
}

// Runtime Frame Generation on/off ([fg] toggle_key, default F9; initial state [fg] enabled). The
// D3D12 device, NGX and the present thread stay alive while off; frames then go out like mode=off.
bool g_fg_on = true;
void update_toggle() {
  const Config& cfg = config();
  static bool init = false, last_cfg = true, key_was_down = false;
  if (!init || cfg.fg_enabled != last_cfg) {  // start-up, or [fg] enabled changed in the live ini
    if (init) ACO_LOG("fg12: [fg] enabled=%d in the ini -> Frame Generation %s", cfg.fg_enabled, cfg.fg_enabled ? "on" : "off");
    init = true;
    last_cfg = cfg.fg_enabled;
    g_fg_on = cfg.fg_enabled;
    S.reset = true;
  }
  if (int vk = config_key_vk(cfg.fg_toggle_key)) {
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    if (down && !key_was_down) {
      g_fg_on = !g_fg_on;
      S.reset = true;  // first evaluation after re-enabling: no stale history
      S.last_real_present = 0;
      ACO_LOG("fg12: hotkey %s -> Frame Generation %s", cfg.fg_toggle_key.c_str(), g_fg_on ? "on" : "off");
    }
    key_was_down = down;
  }
}

bool fg12_present(IDXGISwapChain* sc, UINT sync, UINT flags, PFN_RealPresent real, HRESULT* hr, uint64_t frames) {
  const Config& cfg = config();
  const bool proxy = P.proxy == 1;
  int64_t arrive = now_us();
  // The previous frame's presents may still be running (they use the other texture set); the one
  // before must be done before we reuse its set.
  if (proxy) join(cfg.fg_sync == "gpu" ? 0 : cfg.fg_sets >= 3 ? 2 : 1);

  // NGX/D3D12 initialisation runs on its own thread; presents stay normal until it is ready.
  update_toggle();
  // NGX/D3D12 are initialised whenever mode=d3d12 (also while toggled off, so F9 is instant).
  const bool want_init = cfg.fg_mode == "d3d12" && frames >= 300;
  const bool want = want_init && g_fg_on;
  if (!want) S.reset = true;
  if (want_init && P.init_state == 0) {
    ID3D11Device* dev = nullptr;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev)))) {
      P.init_state = 1;
      std::thread([dev] {
        int64_t t0 = now_us();
        bool ok = init(dev);
        dev->Release();
        ACO_LOG("fg12: %s (init thread, %.2f s)", ok ? "ready" : "unavailable, presenting normally", (now_us() - t0) / 1e6);
        P.init_state = ok ? 2 : 3;
      }).detach();
    }
  }
  bool done = false;
  FgInputs in{};
  bool have_in = false;
  if (want && P.init_state == 2) {
    have_in = upscaler_fg_inputs(&in);
    if (!have_in) {  // no DLSS this frame (menus, loading): present normally
      S.reset = true;
      static int64_t last = 0;
      if (now_us() - last > 5000000) {
        last = now_us();
        ACO_LOG("fg12: no DLSS inputs this frame (menus/loading?) - presenting normally");
      }
    }
  }
  ID3D11Texture2D* bb = proxy ? P.proxy_bb : real_backbuffer(sc);
  if (proxy && bb) bb->AddRef();
  if (proxy && bb) trace_poll(bb);
  if (have_in && bb) {
    detect_refresh(sc);
    if (in.frame != S.last_input_frame + 1) S.reset = true;
    S.last_input_frame = in.frame;
    D3D11_TEXTURE2D_DESC bd;
    bb->GetDesc(&bd);
    // Multiplier of this frame (fixed [fg] count, or the dynamic choice) and the generated frames the
    // feature and texture sets are made for (count_max with count=auto, so the multiplier can change
    // per frame without re-creating anything).
    int lo, hi;
    mult_range(lo, hi);
    const unsigned n_gen = unsigned(std::max(lo, std::min(hi, g_mult.load())) - 1);
    const unsigned feat_gen = unsigned(hi - 1);
    {
      // Textures about to be re-created or released (new size / format / count): nothing queued or
      // on the GPU may still use them.
      static unsigned last_feat_gen = 0;
      if (last_feat_gen != feat_gen || S.tex_w != bd.Width || S.tex_h != bd.Height) {
        if (proxy) join(0);
        if (S.done12) wait_d3d12(S.done_value);
        last_feat_gen = feat_gen;
      }
    }
    if (ensure_textures(bd, feat_gen)) {
      bool verify = cfg.fg_verify && verify_textures();
      if (verify && S.verify_pending > 0 && --S.verify_pending == 0) verify_read();
      if (cfg.fg_sync != "gpu") {
        // Frames alternate between two texture sets; the one we're about to overwrite was last read
        // and written by the evaluation two frames ago (normally long done).
        swap_sets();
        if (wait_fence()->GetCompletedValue() < S.cur->eval_v) {
          ++S.perf_cpu_waits;
          wait_eval(S.cur->eval_v, S.game_event, 200);
        }
      }
      Set& st = *S.cur;
      ID3D11Texture2D* prev_real = cfg.fg_sync != "gpu" ? S.prev->real : nullptr;  // previous real frame
      S.imm->CopyResource(st.real, bb);
      if (generate(in, bb, n_gen, feat_gen)) {
        if (verify && S.verify_pending <= 0 && now_us() - S.verify_last > 2000000 && st.n_gen) {
          // The generated frame closest to the midpoint.
          const Shared& g = st.out[st.n_gen / 2];
          S.verify_last = now_us();
          S.imm->CopyResource(S.stage[0], prev_real ? prev_real : st.real);
          wait_eval(st.eval_v, S.game_event, 200);
          S.imm->CopyResource(S.stage[1], g.t11);
          S.imm->CopyResource(S.stage[2], st.real);
          S.verify_pending = 4;
        }
        // Pacing step: presents one interval / multiplier apart (2x: half an interval). The interval
        // counts at most 50 ms (never hold a real frame more than 25 ms at 2x).
        const int64_t mult = int64_t(st.n_gen) + 1;
        int64_t step = cfg.fg_pacing && S.frame_interval_us > 0
                           ? static_cast<int64_t>(std::min(S.frame_interval_us, 50000.0) / double(mult))
                           : 0;
        if (proxy && config().fg_present_thread == 1) {
          // All presents (generated now, then one step apart, real last) on the present thread.
          enqueue(sc, real, sync, flags, step, cfg.fg_show, arrive);
          *hr = P.hr;
        } else {
          if (proxy) join(0);
          ID3D11Texture2D* dst = proxy ? real_backbuffer(sc) : bb;
          if (dst) {
            for (unsigned k = 0; k < st.n_gen; ++k) {
              if (cfg.fg_sync != "gpu") wait_eval(st.gen_v[k], S.game_event, 200);
              if (k && step > 0) wait_until(now_us() + step);
              fill(dst, true, cfg.fg_show, st.out[k].t11, st.real);
              *hr = real(sc, sync, flags);
            }
            if (step > 0) wait_until(now_us() + step);
            fill(dst, false, cfg.fg_show, st.n_gen ? st.out[0].t11 : st.real, st.real);
            *hr = real(sc, sync, flags);
            if (proxy) dst->Release();
          }
        }
        S.perf_generated += st.n_gen;
        done = true;
      } else {
        ++S.perf_fail;
      }
    }
  }
  if (have_in) {
    safe_release(in.mv);
    safe_release(in.depth);
    safe_release(in.hudless);
  }
  if (!done && proxy && bb) {
    // A normal present of the game's frame (after any queued FG presents).
    join(0);
    int ti = -1;
    if (ID3D11Texture2D* swap_bb = real_backbuffer(sc)) {
      P.ctx->CopyResource(swap_bb, bb);
      ti = trace_begin('N', frames, arrive, 0, swap_bb, P.ctx);
      swap_bb->Release();
    }
    *hr = real(sc, sync, flags);
    trace_end(ti);
    done = true;
  }
  safe_release(bb);
  if (!done) return false;
  if (!have_in) {
    // No Frame Generation for this frame (toggled off, menus, loading): the next FG frame's interval
    // must not span the gap. (It did: after e.g. 30 s with FG off the EMA jumped to ~3 s, the real
    // present then waited half of that and the game, blocked in join, ran at 2 fps for half a minute
    // while the EMA decayed - also after every menu or loading screen.)
    S.last_real_present = 0;
    pacing_update(false);
    return true;
  }

  // Real-frame interval from the game's arrivals here (with the present thread, not inflated by our
  // own pacing wait). A hitch counts at most 2.5x the current interval.
  if (S.last_real_present) {
    double dt = double(arrive - S.last_real_present);
    if (S.frame_interval_us > 0 && dt > 2.5 * S.frame_interval_us) dt = 2.5 * S.frame_interval_us;
    S.frame_interval_us = S.frame_interval_us > 0 ? S.frame_interval_us * 0.9 + dt * 0.1 : std::min(dt, 50000.0);
  }
  S.last_real_present = arrive;
  pacing_update(true);
  ++S.perf_real;
  perf_report();
  if (proxy) perf_join_report();
  return true;
}

} // namespace aco
