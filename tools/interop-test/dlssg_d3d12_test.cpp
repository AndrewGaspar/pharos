// Standalone check that DLSS Frame Generation produces genuinely interpolated frames through NGX D3D12
// on vkd3d-proton under Proton. Two synthetic real frames: a white square on grey that moves +DX
// pixels; motion vectors describe the move. After evaluating frame A then frame B, the generated frame
// should show the square halfway (if it is untouched/copied, generation doesn't work).
//   WINEDLLOVERRIDES="d3d11,dxgi,d3d12,d3d12core,nvapi64=n" wine dlssg_d3d12_test.exe <dir with nvngx_dlssg.dll>
//     [count] [opts r/h/u/s] [frames] [step px] [mvscale_x [mvscale_y]] [width height]
// count = DLSSG.MultiFrameCount (generated frames per real frame: 1 = 2x, 3 = 4x, 5 = 6x). Every
// generated index gets its own output; the test reports where each one put the square (expected at
// t = index / (count + 1) between the last two real frames) and each index's GPU time.
// opts: r OutputReal, h HUDLess, u log UAVs, s one command list + fence signal per index (as the mod).
// Env DLSSG_CREATE="k=v,..." / DLSSG_EVAL="k=v,..." add unsigned-int parameters (v with '.' = float).
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <nvsdk_ngx_params.h>

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define CHECK(x)                                                                                     \
  do {                                                                                               \
    HRESULT hr_ = (x);                                                                               \
    if (FAILED(hr_)) {                                                                               \
      printf("FAIL %s -> 0x%08lx (line %d)\n", #x, static_cast<unsigned long>(hr_), __LINE__);         \
      return 1;                                                                                      \
    }                                                                                                \
  } while (0)

// NVSDK_NGX_Parameter vtable (MSVC layout, overload groups reversed), see src/dlss/ngx.h.
enum Slot { kSetVoidPtr = 0, kSetD3D12Res = 1, kSetI = 3, kSetUI = 4, kSetF = 6, kSetULL = 7, kGetVoidPtr = 8, kGetI = 11,
            kGetUI = 12, kGetF = 14, kReset = 16 };
template <typename F>
F* slot(NVSDK_NGX_Parameter* p, int i) {
  return reinterpret_cast<F*>((*reinterpret_cast<void***>(p))[i]);
}
struct P {
  NVSDK_NGX_Parameter* p;
  void ui(const char* n, unsigned v) { slot<void(NVSDK_NGX_Parameter*, const char*, unsigned)>(p, kSetUI)(p, n, v); }
  void f(const char* n, float v) { slot<void(NVSDK_NGX_Parameter*, const char*, float)>(p, kSetF)(p, n, v); }
  void ull(const char* n, unsigned long long v) {
    slot<void(NVSDK_NGX_Parameter*, const char*, unsigned long long)>(p, kSetULL)(p, n, v);
  }
  void ptr(const char* n, void* v) { slot<void(NVSDK_NGX_Parameter*, const char*, void*)>(p, kSetVoidPtr)(p, n, v); }
  void res(const char* n, ID3D12Resource* v) {
    slot<void(NVSDK_NGX_Parameter*, const char*, ID3D12Resource*)>(p, kSetD3D12Res)(p, n, v);
  }
  int geti(const char* n) {
    int v = -1;
    slot<NVSDK_NGX_Result(NVSDK_NGX_Parameter*, const char*, int*)>(p, kGetI)(p, n, &v);
    return v;
  }
  // "name=value" or "name=<unset>".
  std::string show(const char* n) {
    unsigned u = 0;
    float fl = 0;
    void* v = nullptr;
    char b[160];
    if (slot<NVSDK_NGX_Result(NVSDK_NGX_Parameter*, const char*, unsigned*)>(p, kGetUI)(p, n, &u) == NVSDK_NGX_Result_Success)
      snprintf(b, sizeof b, "%s=%u", n, u);
    else if (slot<NVSDK_NGX_Result(NVSDK_NGX_Parameter*, const char*, float*)>(p, kGetF)(p, n, &fl) == NVSDK_NGX_Result_Success)
      snprintf(b, sizeof b, "%s=%g", n, fl);
    else if (slot<NVSDK_NGX_Result(NVSDK_NGX_Parameter*, const char*, void**)>(p, kGetVoidPtr)(p, n, &v) == NVSDK_NGX_Result_Success)
      snprintf(b, sizeof b, "%s=ptr %p", n, v);
    else
      snprintf(b, sizeof b, "%s=<unset>", n);
    return b;
  }
  // DLSSG_CREATE / DLSSG_EVAL: "k=v,k=v" (v containing '.' is a float).
  void extra(const char* env) {
    const char* e = getenv(env);
    if (!e) return;
    std::string all = e;
    size_t i = 0;
    while (i < all.size()) {
      size_t j = all.find(',', i);
      std::string kv = all.substr(i, j == std::string::npos ? std::string::npos : j - i);
      i = j == std::string::npos ? all.size() : j + 1;
      size_t eq = kv.find('=');
      if (eq == std::string::npos) continue;
      std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
      if (v.find('.') != std::string::npos) f(k.c_str(), float(atof(v.c_str())));
      else ui(k.c_str(), unsigned(strtoul(v.c_str(), nullptr, 0)));
    }
  }
};

using PFN_Init = NVSDK_NGX_Result(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
                                            const NVSDK_NGX_FeatureCommonInfo*);  // core export order
using PFN_GetParams = NVSDK_NGX_Result(__cdecl*)(NVSDK_NGX_Parameter**);
using PFN_Create = NVSDK_NGX_Result(__cdecl*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                                              NVSDK_NGX_Handle**);
using PFN_Eval = NVSDK_NGX_Result(__cdecl*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                            const NVSDK_NGX_Parameter*, void*);

// Debug: log every UAV NGX creates (resource, dimension, format, destination descriptor), to map a
// failing NvAPI_D3D12_GetCudaIndependentDescriptorObject descriptor back to a resource.
using PFN_CreateUAV = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, ID3D12Resource*,
                                               const D3D12_UNORDERED_ACCESS_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
static PFN_CreateUAV g_orig_uav;
static bool g_log_uav = false;
static void STDMETHODCALLTYPE hk_uav(ID3D12Device* d, ID3D12Resource* r, ID3D12Resource* cnt,
                                     const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
  if (g_log_uav) {
    D3D12_RESOURCE_DESC rd{};
    if (r) rd = r->GetDesc();
    printf("  UAV desc=%llx res=%p dim=%d %llux%u fmt=%d flags=0x%x view_dim=%d view_fmt=%d\n",
           (unsigned long long)h.ptr, (void*)r, r ? rd.Dimension : -1, (unsigned long long)rd.Width, rd.Height,
           rd.Format, rd.Flags, desc ? desc->ViewDimension : -1, desc ? desc->Format : -1);
  }
  g_orig_uav(d, r, cnt, desc, h);
}

UINT W = 512, H = 256;
constexpr UINT SQ = 48, X0 = 100, Y0 = 100;

struct Ctx {
  ID3D12Device* dev;
  ID3D12CommandQueue* q;
  ID3D12CommandAllocator* alloc;
  ID3D12GraphicsCommandList* cl;
  ID3D12Fence* fence;
  UINT64 fv = 0;
  void run() {
    cl->Close();
    ID3D12CommandList* l[] = {cl};
    q->ExecuteCommandLists(1, l);
    q->Signal(fence, ++fv);
    while (fence->GetCompletedValue() < fv) Sleep(1);
    alloc->Reset();
    cl->Reset(alloc, nullptr);
  }
};

ID3D12Resource* tex(Ctx& c, DXGI_FORMAT fmt, D3D12_RESOURCE_STATES st, bool uav) {
  D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = W;
  d.Height = H;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = fmt;
  d.SampleDesc.Count = 1;
  d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
  ID3D12Resource* r = nullptr;
  c.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, __uuidof(ID3D12Resource),
                                 reinterpret_cast<void**>(&r));
  return r;
}

ID3D12Resource* buffer(Ctx& c, UINT64 size, D3D12_HEAP_TYPE heap) {
  D3D12_HEAP_PROPERTIES hp{heap};
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = size;
  d.Height = 1;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* r = nullptr;
  c.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                 heap == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                                : D3D12_RESOURCE_STATE_COPY_DEST,
                                 nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&r));
  return r;
}

void barrier(Ctx& c, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
  D3D12_RESOURCE_BARRIER br{};
  br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  br.Transition.pResource = r;
  br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  br.Transition.StateBefore = a;
  br.Transition.StateAfter = b;
  c.cl->ResourceBarrier(1, &br);
}

// Uploads W*H texels of `bpp` bytes (rows tightly packed in `data`) into texture `t` (state `st`).
void upload(Ctx& c, ID3D12Resource* t, const void* data, UINT bpp, D3D12_RESOURCE_STATES st) {
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  UINT64 total = 0;
  D3D12_RESOURCE_DESC d = t->GetDesc();
  c.dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
  ID3D12Resource* up = buffer(c, total, D3D12_HEAP_TYPE_UPLOAD);
  uint8_t* m = nullptr;
  up->Map(0, nullptr, reinterpret_cast<void**>(&m));
  for (UINT y = 0; y < H; ++y) memcpy(m + y * fp.Footprint.RowPitch, static_cast<const uint8_t*>(data) + y * W * bpp, W * bpp);
  up->Unmap(0, nullptr);
  barrier(c, t, st, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION dst{t, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  dst.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION src{up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  src.PlacedFootprint = fp;
  c.cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  barrier(c, t, D3D12_RESOURCE_STATE_COPY_DEST, st);
  c.run();
  up->Release();
}

std::vector<uint32_t> readback(Ctx& c, ID3D12Resource* t, D3D12_RESOURCE_STATES st) {
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  UINT64 total = 0;
  D3D12_RESOURCE_DESC d = t->GetDesc();
  c.dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
  ID3D12Resource* rb = buffer(c, total, D3D12_HEAP_TYPE_READBACK);
  barrier(c, t, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  dst.PlacedFootprint = fp;
  D3D12_TEXTURE_COPY_LOCATION src{t, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  src.SubresourceIndex = 0;
  c.cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  barrier(c, t, D3D12_RESOURCE_STATE_COPY_SOURCE, st);
  c.run();
  std::vector<uint32_t> out(W * H);
  uint8_t* m = nullptr;
  rb->Map(0, nullptr, reinterpret_cast<void**>(&m));
  for (UINT y = 0; y < H; ++y) memcpy(&out[y * W], m + y * fp.Footprint.RowPitch, W * 4);
  rb->Unmap(0, nullptr);
  rb->Release();
  return out;
}

// Horizontal centre of the bright square in row Y0 + SQ/2, or -1.
// Brightness-weighted centroid over the square's rows (above the background's 0x50), pixel centres,
// so a blended or ghosted square is measured where its mass is; `extent` gets the first/last x
// brighter than 180 on the middle row.
double square_x(const std::vector<uint32_t>& img, int* extent = nullptr) {
  double sum = 0, n = 0;
  for (UINT y = Y0; y < Y0 + SQ; ++y)
    for (UINT x = 0; x < W; ++x) {
      int g = (img[y * W + x] >> 8) & 0xff;
      if (g > 0x60) sum += (x + 0.5) * (g - 0x60), n += g - 0x60;
    }
  if (extent) {
    extent[0] = extent[1] = -1;
    for (UINT x = 0; x < W; ++x)
      if (((img[(Y0 + SQ / 2) * W + x] >> 8) & 0xff) > 180) {
        if (extent[0] < 0) extent[0] = int(x);
        extent[1] = int(x);
      }
  }
  return n ? sum / n : -1;
}

int main(int argc, char** argv) {
  // Under `proton run` stdout is lost: DLSSG_OUT=<file> sends it there.
  if (const char* o = getenv("DLSSG_OUT")) freopen(o, "w", stdout);
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::wstring dir = L"Z:\\out";
  if (argc > 1) {
    std::string a = argv[1];
    dir.assign(a.begin(), a.end());
  }
  unsigned count = argc > 2 ? atoi(argv[2]) : 1;
  if (count < 1) count = 1;
  // Optional resources: "r" = OutputReal, "h" = HUDLess; "s" = one command list per index.
  std::string opts = argc > 3 ? argv[3] : "";
  bool want_real = opts.find('r') != std::string::npos, want_hudless = opts.find('h') != std::string::npos;
  bool split = opts.find('s') != std::string::npos;
  // e<k>: evaluate with MultiFrameCount k (created with count); d: alternate k and count per frame
  // (changing the multiplier without re-creating the feature; the last frame uses k).
  unsigned ec = count;
  if (size_t e = opts.find('e'); e != std::string::npos && e + 1 < opts.size()) ec = unsigned(opts[e + 1] - '0');
  if (ec < 1 || ec > count) ec = count;
  const bool dyn = opts.find('d') != std::string::npos;
  const int frames = argc > 4 ? atoi(argv[4]) : 8;
  const UINT STEP = argc > 5 ? atoi(argv[5]) : 16;
  // MvecScale passed to DLSS-G (the vectors are in UV units): 1 = "normalised" reading, W = pixel reading.
  const float MVSCALE_X = argc > 6 ? float(atof(argv[6])) : 1.0f;
  const float MVSCALE_Y = argc > 7 ? float(atof(argv[7])) : MVSCALE_X;
  if (argc > 9) W = atoi(argv[8]), H = atoi(argv[9]);

  IDXGIFactory4* fac = nullptr;
  CHECK(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&fac)));
  IDXGIAdapter1* ad = nullptr;
  for (UINT i = 0; fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 d;
    ad->GetDesc1(&d);
    if (d.VendorId == 0x10de) break;
    ad->Release();
    ad = nullptr;
  }
  if (!ad) return printf("no NVIDIA adapter\n"), 1;
  Ctx c{};
  CHECK(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&c.dev)));
  D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(c.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&c.q)));
  CHECK(c.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                      reinterpret_cast<void**>(&c.alloc)));
  CHECK(c.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.alloc, nullptr,
                                 __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&c.cl)));
  CHECK(c.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&c.fence)));

  // Timestamps around every Evaluate (GPU cost per generated index).
  ID3D12QueryHeap* ts_heap = nullptr;
  D3D12_QUERY_HEAP_DESC qh{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 * count, 0};
  CHECK(c.dev->CreateQueryHeap(&qh, __uuidof(ID3D12QueryHeap), reinterpret_cast<void**>(&ts_heap)));
  ID3D12Resource* ts_buf = buffer(c, 2 * count * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK);
  UINT64 ts_freq = 1;
  c.q->GetTimestampFrequency(&ts_freq);

  {
    void** vt = *reinterpret_cast<void***>(c.dev);
    DWORD old;
    VirtualProtect(&vt[19], sizeof(void*), PAGE_READWRITE, &old);
    g_orig_uav = reinterpret_cast<PFN_CreateUAV>(vt[19]);
    vt[19] = reinterpret_cast<void*>(&hk_uav);
    VirtualProtect(&vt[19], sizeof(void*), old, &old);
    g_log_uav = opts.find('u') != std::string::npos;
  }
  wchar_t ngxpath[MAX_PATH];
  DWORD sz = sizeof(ngxpath);
  HMODULE ngx = nullptr;
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath", RRF_RT_REG_SZ,
                   nullptr, ngxpath, &sz) == ERROR_SUCCESS)
    ngx = LoadLibraryW((std::wstring(ngxpath) + L"\\_nvngx.dll").c_str());
  if (!ngx) return printf("no _nvngx.dll\n"), 1;
  auto init = reinterpret_cast<PFN_Init>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init"));
  auto caps = reinterpret_cast<PFN_GetParams>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
  auto alloc = reinterpret_cast<PFN_GetParams>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_AllocateParameters"));
  auto create = reinterpret_cast<PFN_Create>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature"));
  auto eval = reinterpret_cast<PFN_Eval>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature"));
  const wchar_t* paths[] = {dir.c_str()};
  NVSDK_NGX_FeatureCommonInfo common{};
  common.PathListInfo.Path = paths;
  common.PathListInfo.Length = 1;
  NVSDK_NGX_Result r = init(0x41434f444c5353ull, dir.c_str(), c.dev, NVSDK_NGX_Version_API, &common);
  printf("NGX D3D12 init -> 0x%x\n", r);
  if (NVSDK_NGX_FAILED(r)) return 1;
  NVSDK_NGX_Parameter* cp = nullptr;
  caps(&cp);
  P capsp{cp};
  printf("FrameGeneration.Available = %d\n", capsp.geti(NVSDK_NGX_Parameter_FrameGeneration_Available));
  for (const char* n : {NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, "DLSSG.DynamicMaxFrameCount", "DLSSG.ReflexWarp.Available",
                        "DLSSG.Dynamic.ReflexDriven", "DLSSG.TargetFrameRate", "DLSSG.RefreshRate",
                        "DLSSG.GetCurrentSettingsCallback", "DLSSG.EstimateVRAMCallback", "DLSSG.ModelVersion"})
    printf("caps: %s\n", capsp.show(n).c_str());

  // Inputs.
  auto rt = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  ID3D12Resource* color[2] = {tex(c, DXGI_FORMAT_R8G8B8A8_UNORM, rt, false), tex(c, DXGI_FORMAT_R8G8B8A8_UNORM, rt, false)};
  ID3D12Resource* depth = tex(c, DXGI_FORMAT_R32_FLOAT, rt, false);
  ID3D12Resource* mv = tex(c, DXGI_FORMAT_R16G16_FLOAT, rt, false);
  std::vector<ID3D12Resource*> out(count + 1, nullptr);  // out[idx], idx 1..count
  for (unsigned i = 1; i <= count; ++i) out[i] = tex(c, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  ID3D12Resource* outreal = tex(c, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  // Frame f: textured background (static), white square at x = X0 + f*STEP moving right.
  auto sq_x = [&](int f) { return X0 + f * STEP; };
  std::vector<uint32_t> img(W * H);
  auto draw = [&](int f) {
    for (UINT y = 0; y < H; ++y)
      for (UINT x = 0; x < W; ++x) {
        bool in = y >= Y0 && y < Y0 + SQ && x >= sq_x(f) && x < sq_x(f) + SQ;
        uint32_t g = ((x / 8 + y / 8) & 1) ? 0x50 : 0x30;
        img[y * W + x] = in ? 0xffffffffu : (0xff000000u | g << 16 | g << 8 | g);
      }
  };
  std::vector<float> dep(W * H, 0.5f);
  upload(c, depth, dep.data(), 4, rt);
  auto half = [](float v) {
    uint32_t b;
    memcpy(&b, &v, 4);
    uint32_t s = (b >> 16) & 0x8000, e = ((b >> 23) & 0xff) - 127 + 15, m = (b >> 13) & 0x3ff;
    return static_cast<uint16_t>(v == 0 ? 0 : (s | (e << 10) | m));
  };
  // Motion vectors of frame f (prev - cur, normalised UV): -STEP/W inside the square, 0 elsewhere.
  std::vector<uint32_t> mvs(W * H);
  auto draw_mv = [&](int f) {
    for (UINT y = 0; y < H; ++y)
      for (UINT x = 0; x < W; ++x) {
        bool in = f > 0 && y >= Y0 && y < Y0 + SQ && x >= sq_x(f) && x < sq_x(f) + SQ;
        mvs[y * W + x] = in ? half(-float(STEP) / W) : 0;
      }
  };
  std::vector<uint32_t> magenta(W * H, 0xffff00ffu);
  for (unsigned i = 1; i <= count; ++i) upload(c, out[i], magenta.data(), 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  NVSDK_NGX_Parameter* raw = nullptr;
  alloc(&raw);
  P p{raw};
  p.ui(NVSDK_NGX_Parameter_CreationNodeMask, 1);
  p.ui(NVSDK_NGX_Parameter_VisibilityNodeMask, 1);
  p.ui(NVSDK_NGX_Parameter_Width, W);
  p.ui(NVSDK_NGX_Parameter_Height, H);
  p.ui(NVSDK_NGX_DLSSG_Parameter_Width, W);
  p.ui(NVSDK_NGX_DLSSG_Parameter_Height, H);
  p.ui(NVSDK_NGX_DLSSG_Parameter_BackbufferFormat, DXGI_FORMAT_R8G8B8A8_UNORM);
  p.ui(NVSDK_NGX_DLSSG_Parameter_InternalWidth, W);
  p.ui(NVSDK_NGX_DLSSG_Parameter_InternalHeight, H);
  p.ui(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, count);
  p.extra("DLSSG_CREATE");
  NVSDK_NGX_Handle* h = nullptr;
  r = create(c.cl, NVSDK_NGX_Feature_FrameGeneration, raw, &h);
  c.run();
  printf("CreateFeature(FrameGeneration %ux%u, count %u) -> 0x%x\n", W, H, count, r);
  if (NVSDK_NGX_FAILED(r)) return 1;
  // GetCurrentSettings (what the feature reports about itself after creation).
  {
    void* cb = nullptr;
    slot<NVSDK_NGX_Result(NVSDK_NGX_Parameter*, const char*, void**)>(cp, kGetVoidPtr)(cp, "DLSSG.GetCurrentSettingsCallback", &cb);
    if (cb) {
      NVSDK_NGX_Parameter* sp = nullptr;
      alloc(&sp);
      NVSDK_NGX_Result sr = reinterpret_cast<PFN_NVSDK_NGX_DLSSG_GetCurrentSettingsCallback>(cb)(h, sp);
      P spp{sp};
      printf("GetCurrentSettings -> 0x%x\n", sr);
      for (const char* n : {NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, NVSDK_NGX_DLSSG_Parameter_MultiFrameCount,
                            "DLSSG.DynamicMaxFrameCount", "DLSSG.Dynamic.ReflexDriven", "DLSSG.TargetFrameRate",
                            "DLSSG.RefreshRate", "DLSSG.MustCallEval", "DLSSG.ReflexWarp.Available", "DLSSG.ModelVersion",
                            "DLSSG.NvAppOvrAppliedVal.MultiFrameCount", "DLSSG.MinWidthOrHeight", "DLSSG.EstimatedVRAM"})
        printf("  settings: %s\n", spp.show(n).c_str());
    }
  }

  static float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  std::vector<double> gpu_ms(count + 1, 0.0);
  std::vector<int> gpu_n(count + 1, 0);
  double wall_ms = 0;  // first Evaluate's start to the last one's end (frames evaluated with ec)
  int wall_n = 0;
  for (int f = 0; f < frames; ++f) {
    const unsigned fc = f == frames - 1 ? ec : dyn && (f & 1) ? count : ec;  // this frame's MultiFrameCount
    draw(f);
    upload(c, color[f & 1], img.data(), 4, rt);
    draw_mv(f);
    upload(c, mv, mvs.data(), 4, rt);
    for (unsigned idx = 1; idx <= fc; ++idx) {
      slot<void(NVSDK_NGX_Parameter*)>(raw, kReset)(raw);
      p.res(NVSDK_NGX_DLSSG_Parameter_Backbuffer, color[f & 1]);
      p.res(NVSDK_NGX_DLSSG_Parameter_Depth, depth);
      p.res(NVSDK_NGX_DLSSG_Parameter_MVecs, mv);
      p.res(NVSDK_NGX_DLSSG_Parameter_OutputInterpolated, out[idx]);
      if (want_real) p.res(NVSDK_NGX_DLSSG_Parameter_OutputReal, outreal);
      if (want_hudless) p.res(NVSDK_NGX_DLSSG_Parameter_HUDLess, color[f & 1]);
      p.ui(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, fc);
      p.ui(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, idx);
      for (const char* m : {NVSDK_NGX_DLSSG_Parameter_CameraViewToClip, NVSDK_NGX_DLSSG_Parameter_ClipToCameraView,
                            NVSDK_NGX_DLSSG_Parameter_ClipToLensClip, NVSDK_NGX_DLSSG_Parameter_ClipToPrevClip,
                            NVSDK_NGX_DLSSG_Parameter_PrevClipToClip})
        p.ptr(m, ident);
      p.f(NVSDK_NGX_DLSSG_Parameter_MvecScaleX, MVSCALE_X);
      p.f(NVSDK_NGX_DLSSG_Parameter_MvecScaleY, MVSCALE_Y);
      p.f(NVSDK_NGX_DLSSG_Parameter_CameraUpY, 1.0f);
      p.f(NVSDK_NGX_DLSSG_Parameter_CameraRightX, 1.0f);
      p.f(NVSDK_NGX_DLSSG_Parameter_CameraFwdZ, 1.0f);
      p.f(NVSDK_NGX_DLSSG_Parameter_CameraNear, 0.1f);
      p.f(NVSDK_NGX_DLSSG_Parameter_CameraFar, 1000.0f);
      p.f(NVSDK_NGX_DLSSG_Parameter_CameraFOV, 1.0f);
      p.f(NVSDK_NGX_DLSSG_Parameter_CameraAspectRatio, float(W) / H);
      p.f(NVSDK_NGX_DLSSG_Parameter_MvecInvalidValue, 1.17549435e-38f);
      p.ui(NVSDK_NGX_DLSSG_Parameter_CameraMotionIncluded, 1);
      p.ui(NVSDK_NGX_DLSSG_Parameter_Reset, f == 0 ? 1 : 0);
      p.ui(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectWidth, W);
      p.ui(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectHeight, H);
      p.ui(NVSDK_NGX_DLSSG_Parameter_DepthSubrectWidth, W);
      p.ui(NVSDK_NGX_DLSSG_Parameter_DepthSubrectHeight, H);
      p.ull(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, f + 1);
      p.extra("DLSSG_EVAL");
      c.cl->EndQuery(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, 2 * (idx - 1));
      r = eval(c.cl, h, raw, nullptr);
      c.cl->EndQuery(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, 2 * (idx - 1) + 1);
      if (NVSDK_NGX_FAILED(r)) printf("Evaluate(frame %d, index %u) -> 0x%x\n", f, idx, r);
      if (f == frames - 1 && idx == fc) {
        printf("after the last Evaluate:");
        for (const char* n : {NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, "DLSSG.DynamicMaxFrameCount",
                              "DLSSG.OutputDisableInterpolation"})
          printf(" %s", p.show(n).c_str());
        printf("\n");
      }
      if (idx == fc) c.cl->ResolveQueryData(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2 * fc, ts_buf, 0);
      if (split || idx == fc) c.run();
    }
    if (f >= 2) {
      UINT64* m = nullptr;
      ts_buf->Map(0, nullptr, reinterpret_cast<void**>(&m));
      for (unsigned i = 1; i <= fc; ++i) {
        gpu_ms[i] += double(m[2 * i - 1] - m[2 * i - 2]) * 1000.0 / double(ts_freq);
        ++gpu_n[i];
      }
      if (fc == ec) wall_ms += double(m[2 * fc - 1] - m[0]) * 1000.0 / double(ts_freq), ++wall_n;
      ts_buf->Unmap(0, nullptr);
    }
  }
  double prev_x = sq_x(frames - 2) + SQ / 2.0, cur_x = sq_x(frames - 1) + SQ / 2.0;
  printf("previous real square centre %.1f, current real %.1f (%u px/frame, %ux%u, %s)\n", prev_x, cur_x, STEP, W, H,
         split ? "one command list per index" : "one command list");
  bool all_ok = true;
  double total = 0, last_t = 0;
  for (unsigned idx = 1; idx <= ec; ++idx) {
    std::vector<uint32_t> o = readback(c, out[idx], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    UINT magenta_px = 0;
    for (uint32_t px : o) magenta_px += px == 0xffff00ffu;
    int ext[2];
    double sx = square_x(o, ext);
    double t = (sx - prev_x) / (cur_x - prev_x), want = double(idx) / (ec + 1);
    double ms = gpu_n[idx] ? gpu_ms[idx] / gpu_n[idx] : 0;
    total += ms;
    // Written, between the real frames and after the previous index. (Absolute positions in this
    // synthetic scene lag t by up to ~0.1: count 1 puts the square at t ~ 0.39.)
    bool ok = magenta_px == 0 && sx > 0 && t > last_t && t < 1;
    last_t = t;
    all_ok = all_ok && ok;
    printf("index %u/%u: square x = %.1f -> t = %.3f (expected %.3f; bright %d..%d) untouched %u px, GPU %.3f ms %s\n", idx,
           ec, sx, t, want, ext[0], ext[1], magenta_px, ms, ok ? "ok" : "WRONG");
  }
  printf("GPU total %.3f ms (sum of per-index times), %.3f ms first start to last end, per real frame for %u generated frames\n",
         total, wall_n ? wall_ms / wall_n : 0.0, ec);
  if (want_real) printf("OutputReal square centre x = %.1f\n", square_x(readback(c, outreal, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)));
  printf(all_ok ? "RESULT: INTERPOLATED (every index written, in order between the real frames)\n" : "RESULT: NOT INTERPOLATED\n");
  return all_ok ? 0 : 2;
}
