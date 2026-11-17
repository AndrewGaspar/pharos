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

namespace aco {

namespace {

constexpr unsigned long long kAppId = 0x41434f444c5353ull;  // "ACODLSS", as for DLSS-SR
constexpr int kRing = 3;
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
  int eval_slot = -1;      // ring slot of the last evaluation
  UINT64 eval_ts_old = 0;  // that slot's end timestamp before the evaluation (changes once it has run)
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

  Shared color, depth, mv, hudless, out, outreal, scratch;
  ID3D11Texture2D* real = nullptr;  // D3D11-only copy of the real frame for presenting after the generated one
  UINT64 eval_v = 0;  // D3D12 fence value when the last evaluation using this set is done
  // [fg] sync=cpu: the other texture set (frames alternate), swapped in by swap_sets().
  Shared alt_color, alt_depth, alt_mv, alt_hudless, alt_out;
  ID3D11Texture2D* alt_real = nullptr;
  UINT64 alt_eval_v = 0;
  HANDLE game_event = nullptr;  // for CPU waits on the game thread (wait_d3d12 uses fence_event)
  uint32_t tex_w = 0, tex_h = 0;
  DXGI_FORMAT tex_fmt = DXGI_FORMAT_UNKNOWN;

  // [fg] verify=1: periodically read back (previous real, generated, current real) and log how the
  // generated frame differs from its neighbours; also written to out/fgverify/*.ppm.
  ID3D11Texture2D* prev = nullptr;
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

void wait_d3d12(UINT64 v) {
  ID3D12Fence* f = wait_fence();
  if (f->GetCompletedValue() >= v) return;
  f->SetEventOnCompletion(v, S.fence_event);
  WaitForSingleObject(S.fence_event, 2000);
}

// Runs `record` on our D3D12 queue after the D3D11 work submitted so far, then makes the D3D11
// context wait for it. `cpu_wait` blocks until the GPU is done (feature creation).
template <typename F>
bool submit(F&& record, bool cpu_wait) {
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
  UINT64 v_in = ++S.fence_value;
  S.imm->Signal(S.fence11, v_in);
  S.imm->Flush();
  S.q->Wait(S.fence12, v_in);
  ID3D12CommandList* lists[] = {S.cl};
  S.q->ExecuteCommandLists(1, lists);
  UINT64 v_out = ++S.done_value;
  S.q->Signal(S.done12, v_out);
  if (S.priv12) S.q->Signal(S.priv12, v_out);
  S.slot_value[slot] = v_out;
  S.eval_v = v_out;
  // D3D12 -> D3D11. sync=gpu: later context work waits on the GPU (vkd3d-proton resolves the D3D12
  // side's wait on the shared fence on the CPU, so this stalls the D3D11 queue on a CPU round trip).
  // sync=cpu: whoever copies the output out first waits on the CPU (wait_eval).
  if ((cpu_wait || config().fg_sync == "gpu") && S.done11) S.imm->Wait(S.done11, v_out);
  if (cpu_wait) wait_d3d12(v_out);
  return ok;
}

// CPU wait for a D3D12 fence value with a caller-owned event; false on timeout.
bool wait_eval(UINT64 v, HANDLE event, DWORD timeout_ms) {
  ID3D12Fence* f = wait_fence();
  if (!v || f->GetCompletedValue() >= v) return true;
  f->SetEventOnCompletion(v, event);
  return WaitForSingleObject(event, timeout_ms) == WAIT_OBJECT_0 || f->GetCompletedValue() >= v;
}

void swap_sets() {
  std::swap(S.color, S.alt_color);
  std::swap(S.depth, S.alt_depth);
  std::swap(S.mv, S.alt_mv);
  std::swap(S.hudless, S.alt_hudless);
  std::swap(S.out, S.alt_out);
  std::swap(S.real, S.alt_real);
  std::swap(S.eval_v, S.alt_eval_v);
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
  ACO_LOG("fg12: FrameGeneration available=%d", avail);
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

bool ensure_textures(const D3D11_TEXTURE2D_DESC& bb) {
  DXGI_FORMAT f = storage_format(bb.Format);
  if (S.real && S.alt_real && S.tex_w == bb.Width && S.tex_h == bb.Height && S.tex_fmt == f) return true;
  safe_release(S.real);
  safe_release(S.alt_real);
  S.tex_w = S.tex_h = 0;
  D3D11_TEXTURE2D_DESC d{};
  d.Width = bb.Width;
  d.Height = bb.Height;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = bb.Format;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(S.dev->CreateTexture2D(&d, nullptr, &S.real)) || FAILED(S.dev->CreateTexture2D(&d, nullptr, &S.alt_real)))
    return false;
  if (!make_shared(S.color, bb.Width, bb.Height, f, false) || !make_shared(S.out, bb.Width, bb.Height, f, true) ||
      !make_shared(S.alt_color, bb.Width, bb.Height, f, false) || !make_shared(S.alt_out, bb.Width, bb.Height, f, true) ||
      !make_shared(S.outreal, bb.Width, bb.Height, f, true) || !make_shared(S.scratch, bb.Width, bb.Height, f, true))
    return false;
  S.tex_w = bb.Width;
  S.tex_h = bb.Height;
  S.tex_fmt = f;
  ACO_LOG("fg12: shared textures %ux%u format %d (backbuffer format %d)", bb.Width, bb.Height, f, bb.Format);
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

bool generate(const FgInputs& in, ID3D11Texture2D* bb) {
  const Config& cfg = config();
  S.imm->CopyResource(S.color.t11, bb);
  if (!copy_in(S.depth, in.depth) || !copy_in(S.mv, in.mv)) { gen_fail("inputs"); return false; }
  bool hudless = cfg.fg_hudless && in.hudless;
  if (hudless) {
    D3D11_TEXTURE2D_DESC hd;
    in.hudless->GetDesc(&hd);
    // Only a HUD-less image that matches the backbuffer (size and format) is meaningful to DLSS-G.
    hudless = hd.Width == S.tex_w && hd.Height == S.tex_h && storage_format(hd.Format) == S.tex_fmt &&
              copy_in(S.hudless, in.hudless);
  }
  const unsigned count = static_cast<unsigned>(cfg.fg_count < 1 ? 1 : cfg.fg_count);
  const unsigned show_index = static_cast<unsigned>(cfg.fg_show_index);
  if (!ensure_feature(S.tex_w, S.tex_h, in.render_w, in.render_h, S.tex_fmt, count)) { gen_fail("feature"); return false; }

  ngx::Params& p = S.eval_params;
  p.reset();
  p.set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, S.color.t12);
  p.set(NVSDK_NGX_DLSSG_Parameter_Depth, S.depth.t12);
  p.set(NVSDK_NGX_DLSSG_Parameter_MVecs, S.mv.t12);
  if (hudless) {
    p.set(NVSDK_NGX_DLSSG_Parameter_HUDLess, S.hudless.t12);
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
  if (cfg.fg_refresh_hz > 0) p.set("DLSSG.RefreshRate", cfg.fg_refresh_hz);
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
  if (cfg.fg_skip == "d3d12") return true;
  const bool skip_eval = cfg.fg_skip == "eval";
  bool ok = submit([&](ID3D12GraphicsCommandList* cl) {
    if (skip_eval) {
      r = NVSDK_NGX_Result_Success;
      return true;
    }
    for (unsigned i = 1; i <= count; ++i) {
      p.set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, count);
      p.set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, i);
      p.set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolated, i == show_index ? S.out.t12 : S.scratch.t12);
      r = S.evaluate(cl, S.feature, p.get(), nullptr);
      if (NVSDK_NGX_FAILED(r)) {
        static int n = 0;
        if (n++ < 5) ACO_LOG("fg12: Evaluate(index %u of %u) -> %s", i, count, ngx::result_str(r));
        return false;
      }
    }
    return true;
  }, false);
  S.perf_eval_cpu_ms += (now_us() - t0) / 1000.0;
  static int logged = 0;
  if (logged < 3 || (!ok && logged < 20)) {
    ++logged;
    ACO_LOG("fg12: Evaluate -> %s (reset=%d, render %ux%u, jitter %.3f,%.3f, hudless %d)", ngx::result_str(r), S.reset,
            in.render_w, in.render_h, in.jitter_x, in.jitter_y, hudless);
  }
  S.reset = false;
  return ok;
}

void perf_report() {
  int64_t now = now_us();
  if (!S.perf_last) S.perf_last = now;
  double secs = (now - S.perf_last) / 1e6;
  if (secs < 2.0) return;
  ACO_LOG("perf: fg real %.1f fps + generated %.1f fps = %.1f presents/s (fail %u, resets %u, cpu waits %u), eval CPU "
          "%.2f ms GPU %.3f ms/frame [d3d12]",
          S.perf_real / secs, S.perf_generated / secs, (S.perf_real + S.perf_generated) / secs, S.perf_fail,
          S.perf_resets, S.perf_cpu_waits, S.perf_real ? S.perf_eval_cpu_ms / S.perf_real : 0.0,
          S.perf_gpu_n ? S.perf_gpu_ms / S.perf_gpu_n : 0.0);
  S.perf_last = now;
  S.perf_real = S.perf_generated = S.perf_fail = S.perf_resets = S.perf_cpu_waits = 0;
  S.perf_eval_cpu_ms = S.perf_gpu_ms = 0;
  S.perf_gpu_n = 0;
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
  if (S.prev && S.stage[0]) return true;
  D3D11_TEXTURE2D_DESC d{};
  S.real->GetDesc(&d);
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (!S.prev && FAILED(S.dev->CreateTexture2D(&d, nullptr, &S.prev))) return false;
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
                ID3D11DeviceContext* ctx) {
  if (!T.active.load(std::memory_order_relaxed)) return -1;
  std::lock_guard<std::mutex> lk(T.m);
  if (T.complete || T.ev.size() >= T.max_ev) return -1;
  TraceEv e{arrive, now_us(), 0, frame, kind, int32_t(half_us), int32_t(S.frame_interval_us), -1};
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
} P;

ID3D11Texture2D* real_backbuffer(IDXGISwapChain* sc) {
  ID3D11Texture2D* bb = nullptr;
  HRESULT hr = P.real_get_buffer ? P.real_get_buffer(sc, 0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))
                                 : sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb));
  return SUCCEEDED(hr) ? bb : nullptr;
}

// One frame's presents (generated, then real) for the present thread.
struct Job {
  IDXGISwapChain* sc = nullptr;
  PFN_RealPresent present = nullptr;
  UINT sync = 0, flags = 0;
  int64_t half_us = 0;      // pacing: real frame this long after the generated one
  int64_t arrive_us = 0;    // when the game's Present reached us
  UINT64 wait_value = 0;    // the generated frame is ready when the D3D12 fence reaches this
  ID3D11Texture2D* gen = nullptr;
  ID3D11Texture2D* real = nullptr;
  std::string show;
  bool gate = true;
  bool skip_gen = false;    // superseded before it started: present only the real frame
  uint64_t frame = 0;       // BackbufferFrameID of this job's evaluation (trace)
  int pace = 0;             // [fg] pace: 0 ready, 1 smooth, 2 arrival
  double relax = 0.5;       // [fg] pace_relax
  int ts_slot = -1;         // ring slot of this job's evaluation (timestamp check), -1 = none
  UINT64 ts_old = 0;        // that slot's end timestamp before the evaluation
};
std::deque<Job> g_jobs;  // guarded by P.m
int g_active = 0;        // jobs being processed (0/1), guarded by P.m

void present_loop() {
  g_present_tid = GetCurrentThreadId();
  g_ctx_gate_exempt = 1;
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  for (;;) {
    std::unique_lock<std::mutex> lk(P.m);
    P.cv.wait(lk, [] { return !g_jobs.empty(); });
    Job j = g_jobs.front();
    g_jobs.pop_front();
    g_active = 1;
    lk.unlock();

    HRESULT hr = S_OK;
    int64_t t0 = now_us(), t1 = t0, t2 = t0;
    bool gen_ready = !j.skip_gen && wait_eval(j.wait_value, event, 100);
    if (!gen_ready) ++P.gen_dropped;
    if (gen_ready && j.ts_slot >= 0 && S.ts_map) {
      // The evaluation's command list ends by resolving its end timestamp into this slot: until the
      // value changes, the GPU hasn't finished it, whatever the fence says.
      const volatile UINT64* end = &S.ts_map[j.ts_slot * 2 + 1];
      ++P.gen_checked;
      if (*end == j.ts_old) {
        ++P.gen_early;
        if (config().fg_ts_guard) {
          int64_t deadline = now_us() + 50000;
          while (*end == j.ts_old && now_us() < deadline) Sleep(0);
          if (*end == j.ts_old) ++P.gen_early_timeout;
        }
      }
    }
    t0 = now_us();
    // Pacing of the generated present ([fg] pace). ready: as soon as DLSS-G is done. smooth: not
    // sooner than half an interval after the previous real present, minus a share ([fg] pace_relax)
    // of how early DLSS-G finished relative to that, so a delay never sticks. arrival: at a fixed
    // lag after the game's Present (lag = recent maximum of DLSS-G ready - arrival), i.e. locked to
    // the game's own frame cadence.
    if (gen_ready && j.half_us > 0) {
      static int64_t lag_us = 0;
      int64_t ready_lag = t0 - j.arrive_us;
      lag_us = ready_lag > lag_us ? ready_lag : lag_us - (lag_us - ready_lag) / 64;
      int64_t target = 0;
      if (j.pace == 1 && P.last_real_call)
        target = P.last_real_call + j.half_us - int64_t(j.relax * double(P.last_real_call + j.half_us - t0));
      else if (j.pace == 2)
        target = j.arrive_us + lag_us;
      if (target > t0 && target - t0 < 2 * j.half_us) wait_until(target);
    }
    t0 = t1 = t2 = now_us();
    if (gen_ready) {
      if (ID3D11Texture2D* bb = real_backbuffer(j.sc)) {
        if (j.gate) g_ctx_gate.fetch_add(1);
        fill(bb, true, j.show, j.gen, j.real);
        int ti = trace_begin('G', j.frame, j.arrive_us, j.half_us, bb, S.imm);
        bb->Release();
        t1 = now_us();
        hr = j.present(j.sc, j.sync, j.flags);
        trace_end(ti);
        if (j.gate) g_ctx_gate.fetch_sub(1);
      }
    }
    int64_t t_gen = t2 = now_us();
    // The real frame half an interval after the generated one: measured from when the generated
    // Present was called (pace=smooth/arrival) - a Present that blocks must not push the real frame
    // later (pace=ready, the original: from when it returned).
    if (gen_ready && j.half_us > 0) wait_until((j.pace ? t1 : t_gen) + j.half_us);
    int64_t t_real = now_us(), t4 = t_real;
    if (j.show == "skip_present2") {
      // bisecting: no second present
    } else if (ID3D11Texture2D* bb = real_backbuffer(j.sc)) {
      if (j.gate) g_ctx_gate.fetch_add(1);
      fill(bb, false, j.show, j.gen, j.real);
      int ti = trace_begin('R', j.frame, j.arrive_us, j.half_us, bb, S.imm);
      bb->Release();
      t4 = now_us();
      P.last_real_call = t4;
      HRESULT h2 = j.present(j.sc, j.sync, j.flags);
      trace_end(ti);
      if (j.gate) g_ctx_gate.fetch_sub(1);
      if (SUCCEEDED(hr)) hr = h2;
    }
    int64_t t5 = now_us();
    P.seg_us[0] += t0 - j.arrive_us;
    P.seg_us[1] += t1 - t0;
    P.seg_us[2] += t2 - t1;
    P.seg_us[3] += t_real - t2;
    P.seg_us[4] += t4 - t_real;
    P.seg_us[5] += t5 - t4;
    P.delay_gen_us += t_gen - j.arrive_us;
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
}

void enqueue(IDXGISwapChain* sc, PFN_RealPresent present, UINT sync, UINT flags, int64_t half, const std::string& show,
             int64_t arrive) {
  static std::thread thread(present_loop);
  Job j;
  j.sc = sc;
  j.present = present;
  j.sync = sync;
  j.flags = flags;
  j.half_us = half;
  j.arrive_us = arrive;
  j.wait_value = config().fg_sync == "gpu" ? 0 : S.eval_v;
  j.gen = S.out.t11;
  j.real = S.real;
  j.ts_slot = S.eval_slot;
  j.ts_old = S.eval_ts_old;
  j.show = config().fg_skip == "present2" ? std::string("skip_present2") : show;
  j.gate = config().fg_present_gate;
  j.frame = S.backbuffer_id;
  j.pace = config().fg_pace == "smooth" ? 1 : config().fg_pace == "arrival" ? 2 : 0;
  j.relax = config().fg_pace_relax;
  std::lock_guard<std::mutex> lk(P.m);
  // A job that hasn't started yet is late: its generated frame would only delay the real ones.
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
  ACO_LOG("perf: fg12 completion check: %u of %u generated frames had the fence done before DLSS-G's end "
          "timestamp (guard timeouts %u) [done_fence=%s ts_guard=%d]",
          P.gen_early.exchange(0), P.gen_checked.exchange(0), P.gen_early_timeout.exchange(0),
          S.priv12 && config().fg_done_fence == "private" ? "private" : "shared", config().fg_ts_guard ? 1 : 0);
  double s[6];
  for (int i = 0; i < 6; ++i) s[i] = n ? P.seg_us[i].exchange(0) / 1000.0 / n : 0.0;
  ACO_LOG("perf: fg12 present thread steps (ms/job): pickup %.2f | copy %.2f, Present %.2f (generated) | wait %.2f | "
          "copy %.2f, Present %.2f (real)", s[0], s[1], s[2], s[3], s[4], s[5]);
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
    fprintf(f, "kind,frame,arrive_us,call_us,ret_us,half_us,interval_us,shot\n");
    for (const TraceEv& e : T.ev)
      fprintf(f, "%c,%llu,%lld,%lld,%lld,%d,%d,%d\n", e.kind, static_cast<unsigned long long>(e.frame),
              static_cast<long long>(e.arrive), static_cast<long long>(e.call), static_cast<long long>(e.ret), e.half_us,
              e.interval_us, e.shot);
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
  if (proxy) join(cfg.fg_sync == "gpu" ? 0 : 1);

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
    if (in.frame != S.last_input_frame + 1) S.reset = true;
    S.last_input_frame = in.frame;
    D3D11_TEXTURE2D_DESC bd;
    bb->GetDesc(&bd);
    if (ensure_textures(bd)) {
      bool verify = cfg.fg_verify && verify_textures();
      if (verify && S.verify_pending > 0 && --S.verify_pending == 0) verify_read();
      if (cfg.fg_sync != "gpu") {
        // Frames alternate between two texture sets; the one we're about to overwrite was last read
        // and written by the evaluation two frames ago (normally long done).
        swap_sets();
        if (wait_fence()->GetCompletedValue() < S.eval_v) {
          ++S.perf_cpu_waits;
          wait_eval(S.eval_v, S.game_event, 200);
        }
      }
      ID3D11Texture2D* prev_real = cfg.fg_sync != "gpu" ? S.alt_real : nullptr;  // previous real frame
      S.imm->CopyResource(S.real, bb);
      if (generate(in, bb)) {
        if (verify && S.verify_pending <= 0 && now_us() - S.verify_last > 2000000) {
          S.verify_last = now_us();
          S.imm->CopyResource(S.stage[0], prev_real ? prev_real : S.real);
          wait_eval(S.eval_v, S.game_event, 200);
          S.imm->CopyResource(S.stage[1], S.out.t11);
          S.imm->CopyResource(S.stage[2], S.real);
          S.verify_pending = 4;
        }
        int64_t half = cfg.fg_pacing && S.frame_interval_us > 0 ? static_cast<int64_t>(S.frame_interval_us / 2) : 0;
        if (proxy && config().fg_present_thread == 1) {
          // Both presents (generated now, real half a frame later) on the present thread.
          enqueue(sc, real, sync, flags, half, cfg.fg_show, arrive);
          *hr = P.hr;
        } else {
          if (proxy) join(0);
          ID3D11Texture2D* dst = proxy ? real_backbuffer(sc) : bb;
          if (dst) {
            if (cfg.fg_sync != "gpu") wait_eval(S.eval_v, S.game_event, 200);
            fill(dst, true, cfg.fg_show, S.out.t11, S.real);
            *hr = real(sc, sync, flags);
            if (half > 0) wait_until(now_us() + half);
            fill(dst, false, cfg.fg_show, S.out.t11, S.real);
            *hr = real(sc, sync, flags);
            if (proxy) dst->Release();
          }
        }
        ++S.perf_generated;
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
  if (!have_in) return true;

  // Real-frame interval from the game's arrivals here (with the present thread, not inflated by our
  // own pacing wait).
  if (S.last_real_present) {
    double dt = double(arrive - S.last_real_present);
    S.frame_interval_us = S.frame_interval_us > 0 ? S.frame_interval_us * 0.9 + dt * 0.1 : dt;
  }
  S.last_real_present = arrive;
  ++S.perf_real;
  perf_report();
  if (proxy) perf_join_report();
  return true;
}

} // namespace aco
