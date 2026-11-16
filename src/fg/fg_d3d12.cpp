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

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/config.h"
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
  bool tried = false, ok = false;
  ID3D11Device5* dev = nullptr;
  ID3D11DeviceContext4* imm = nullptr;
  ID3D11Fence* fence11 = nullptr;
  ID3D12Device* d12 = nullptr;
  ID3D12CommandQueue* q = nullptr;
  ID3D12Fence* fence12 = nullptr;
  UINT64 fence_value = 0;
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

void wait_d3d12(UINT64 v) {
  if (S.fence12->GetCompletedValue() >= v) return;
  S.fence12->SetEventOnCompletion(v, S.fence_event);
  WaitForSingleObject(S.fence_event, 2000);
}

// Runs `record` on our D3D12 queue after the D3D11 work submitted so far, then makes the D3D11
// context wait for it. `cpu_wait` blocks until the GPU is done (feature creation).
template <typename F>
bool submit(F&& record, bool cpu_wait) {
  int slot = S.ring;
  S.ring = (S.ring + 1) % kRing;
  if (S.fence12->GetCompletedValue() < S.slot_value[slot]) {
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

  // D3D11 -> D3D12: everything the context has recorded so far (our input copies) first.
  UINT64 v_in = ++S.fence_value;
  S.imm->Signal(S.fence11, v_in);
  S.imm->Flush();
  S.q->Wait(S.fence12, v_in);
  ID3D12CommandList* lists[] = {S.cl};
  S.q->ExecuteCommandLists(1, lists);
  UINT64 v_out = ++S.fence_value;
  S.q->Signal(S.fence12, v_out);
  S.slot_value[slot] = v_out;
  // D3D12 -> D3D11: later context work (copying the output out) waits for the evaluation.
  S.imm->Wait(S.fence11, v_out);
  if (cpu_wait) wait_d3d12(v_out);
  return ok;
}

bool init(IDXGISwapChain* sc) {
  ID3D11Device* dev = nullptr;
  if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev)))) return false;
  HRESULT hr = dev->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&S.dev));
  ID3D11DeviceContext* ctx = nullptr;
  dev->GetImmediateContext(&ctx);
  dev->Release();
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
  S.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

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
    wait_d3d12(S.fence_value);
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
  if (S.real && S.tex_w == bb.Width && S.tex_h == bb.Height && S.tex_fmt == f) return true;
  safe_release(S.real);
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
  if (FAILED(S.dev->CreateTexture2D(&d, nullptr, &S.real))) return false;
  if (!make_shared(S.color, bb.Width, bb.Height, f, false) || !make_shared(S.out, bb.Width, bb.Height, f, true) ||
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
  for (const char* m : {NVSDK_NGX_DLSSG_Parameter_CameraViewToClip, NVSDK_NGX_DLSSG_Parameter_ClipToCameraView,
                        NVSDK_NGX_DLSSG_Parameter_ClipToLensClip, NVSDK_NGX_DLSSG_Parameter_ClipToPrevClip,
                        NVSDK_NGX_DLSSG_Parameter_PrevClipToClip})
    p.set(m, const_cast<void*>(static_cast<const void*>(kIdentity)));
  // Our motion vectors are in UV units (1 = full screen) = DLSS-G's normalized space.
  p.set(NVSDK_NGX_DLSSG_Parameter_MvecScaleX, cfg.fg_mv_scale);
  p.set(NVSDK_NGX_DLSSG_Parameter_MvecScaleY, cfg.fg_mv_scale);
  p.set(NVSDK_NGX_DLSSG_Parameter_JitterOffsetX, in.jitter_x);
  p.set(NVSDK_NGX_DLSSG_Parameter_JitterOffsetY, in.jitter_y);
  // A valid (if synthetic) camera frame: an all-zero basis is "not a valid frame" for DLSS-G.
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraPosX, 0.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraPosY, 0.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraPosZ, 0.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraUpX, 0.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraUpY, 1.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraUpZ, 0.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraRightX, 1.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraRightY, 0.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraRightZ, 0.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraFwdX, 0.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraFwdY, 0.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraFwdZ, 1.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetX, 0.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetY, 0.0f);
  // Invalid-MV marker: not 0 (that would mark every static pixel invalid); Streamline uses FLT_MIN.
  p.set(NVSDK_NGX_DLSSG_Parameter_MvecInvalidValue, 1.17549435e-38f);
  p.set(NVSDK_NGX_DLSSG_Parameter_LinearizedDepth_Scale, 1.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_LinearizedDepth_NearFarPartition, 600.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_AutomodeOverrideReset, 0u);
  p.set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, ++S.backbuffer_id);
  if (cfg.fg_refresh_hz > 0) p.set("DLSSG.RefreshRate", cfg.fg_refresh_hz);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraNear, 0.1f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraFar, 100000.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraFOV, 1.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraAspectRatio, float(S.tex_w) / float(S.tex_h));
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
  bool ok = submit([&](ID3D12GraphicsCommandList* cl) {
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
void fill(ID3D11Texture2D* dst, bool generated_slot) {
  const std::string& show = config().fg_show;
  if (show == "compare" || show == "compare_gen") {
    // Debug: left half the real frame, right half OutputReal / the generated frame.
    D3D11_BOX left{0, 0, 0, S.tex_w / 2, S.tex_h, 1};
    S.imm->CopySubresourceRegion(dst, 0, 0, 0, 0, S.real, 0, &left);
    S.imm->CopySubresourceRegion(dst, 0, S.tex_w / 2, 0, 0, show == "compare_gen" ? S.out.t11 : S.outreal.t11, 0, &left);
    return;
  }
  ID3D11Texture2D* src = generated_slot ? (show == "real" ? S.real : show == "outreal" ? S.outreal.t11 : S.out.t11)
                                        : (show == "generated" ? S.out.t11 : show == "outreal" ? S.outreal.t11 : S.real);
  S.imm->CopyResource(dst, src);
}

} // namespace

bool fg12_present(IDXGISwapChain* sc, UINT sync, UINT flags, PFN_RealPresent real, HRESULT* hr) {
  const Config& cfg = config();
  if (!S.tried) {
    S.tried = true;
    S.ok = init(sc);
    ACO_LOG("fg12: %s", S.ok ? "ready" : "unavailable, presenting normally");
  }
  if (!S.ok) return false;

  FgInputs in;
  if (!upscaler_fg_inputs(&in)) {  // no DLSS this frame (menus, loading): present normally
    S.reset = true;
    static int64_t last = 0;
    if (now_us() - last > 5000000) {
      last = now_us();
      ACO_LOG("fg12: no DLSS inputs this frame (menus/loading?) - presenting normally");
    }
    return false;
  }
  if (in.frame != S.last_input_frame + 1) S.reset = true;
  S.last_input_frame = in.frame;

  ID3D11Texture2D* bb = nullptr;
  bool done = false;
  if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) {
    D3D11_TEXTURE2D_DESC bd;
    bb->GetDesc(&bd);
    if (ensure_textures(bd)) {
      bool verify = cfg.fg_verify && verify_textures();
      if (verify) {
        if (S.verify_pending > 0 && --S.verify_pending == 0) verify_read();
        S.imm->CopyResource(S.prev, S.real);  // S.real still holds the previous real frame
      }
      S.imm->CopyResource(S.real, bb);
      if (generate(in, bb)) {
        if (verify && S.verify_pending <= 0 && now_us() - S.verify_last > 2000000) {
          S.verify_last = now_us();
          S.imm->CopyResource(S.stage[0], S.prev);
          S.imm->CopyResource(S.stage[1], S.out.t11);
          S.imm->CopyResource(S.stage[2], S.real);
          S.verify_pending = 4;
        }
        // The generated frame first (it lies between the previous real frame and this one) ...
        fill(bb, true);
        *hr = real(sc, sync, flags);
        int64_t t_gen = now_us();
        ++S.perf_generated;
        // ... then the real frame, half a frame interval later.
        if (cfg.fg_pacing && S.frame_interval_us > 0) wait_until(t_gen + static_cast<int64_t>(S.frame_interval_us / 2));
        ID3D11Texture2D* bb2 = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb2)))) {
          fill(bb2, false);
          bb2->Release();
        }
        *hr = real(sc, sync, flags);
        done = true;
      } else {
        ++S.perf_fail;
      }
    }
    bb->Release();
  }
  safe_release(in.mv);
  safe_release(in.depth);
  safe_release(in.hudless);
  if (!done) return false;

  int64_t now = now_us();
  if (S.last_real_present) {
    double dt = double(now - S.last_real_present);
    S.frame_interval_us = S.frame_interval_us > 0 ? S.frame_interval_us * 0.9 + dt * 0.1 : dt;
  }
  S.last_real_present = now;
  ++S.perf_real;
  perf_report();
  return true;
}

} // namespace aco
