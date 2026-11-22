#include "core/d3d11_title.h"
#include "dlss/upscaler.h"
#include "stereo/stereo.h"
#include "stereo/cursor.h"
#include "core/ctxgate.h"

#include <windows.h>
#include <d3d11_1.h>
#include <d3d10.h>
#include <dxgi.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/cbshadow.h"
#include "core/latency.h"
#include "core/config.h"
#include "core/ctxhooks.h"
#include "core/log.h"
#include "dlss/kernels.h"
#include "dlss/ngx.h"
#include "dlss/sr_settings.h"
#include "trace/trace.h"
#include "trace/bloom_probe.h"

namespace aco {

namespace {

using Microsoft::WRL::ComPtr;

// TemporalAA_cbuffer layout (from the TAA shader's reflection data).
struct TaaConstants {
  uint32_t isolation_vars[4];
  float full_res_size[2];
  float full_res_size_inv[2];
  float low_res_size[2];
  float low_res_size_inv[2];
  float sub_pixel_jitter[2];
  float pad0[2];
  float noise_offset_scale[3];
  float rejection_avoidance_max;
  float rest[12];
};
static_assert(sizeof(TaaConstants) == 128);

// TAA resolve bindings (PS slots).
constexpr UINT kTaaColor = title::texture0;

template <typename T>
void safe_release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

ID3D11Resource* resource_of(ID3D11View* v) {
  ID3D11Resource* r = nullptr;
  if (v) v->GetResource(&r);
  return r;  // AddRef'd
}

struct Job {
  ID3D11Resource* color = nullptr;     // tonemapped LDR, render res
  ID3D11Resource* depth = nullptr;     // raw reversed-Z device depth (R32F copy), render res
  ComPtr<ID3D11Resource> taa_output;
  ComPtr<ID3D11Texture2D> mv;
  ComPtr<ID3D11ShaderResourceView> mv_srv;
  uint32_t render_w = 0, render_h = 0;
  float jitter_x = 0, jitter_y = 0;
  uint64_t frame = 0;
  uint64_t resize_generation = 0;
  FgCamera cam;
  ~Job() {
    safe_release(color);
    safe_release(depth);
  }
};

// A split preserves all commands preceding the intercepted pass. For a deferred
// composite we record both bindings, then submit just one draw on the immediate
// context, after the result for this frame and this stereo eye is known.
struct Part {
  ComPtr<ID3D11CommandList> list;
  std::shared_ptr<Job> job;
  ComPtr<ID3D11CommandList> upscale_draw;
  ComPtr<ID3D11Resource> scene;
  ComPtr<ID3D11ShaderResourceView> upscaled;
};

struct Chain final : IUnknown {
  std::atomic<ULONG> refs{1};
  std::vector<Part> parts;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == __uuidof(IUnknown)) {
      *out = this;
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = --refs;
    if (r == 0) delete this;
    return r;
  }
};

// {6b1e0f6a-6f0e-4a63-9a0e-acd1550c0de5}
constexpr GUID kChainGuid = {0x6b1e0f6a, 0x6f0e, 0x4a63, {0x9a, 0x0e, 0xac, 0xd1, 0x55, 0x0c, 0x0d, 0xe5}};

// GPU timing of our work on the immediate context (ring of timestamp query sets, read back
// without stalling a few frames later).
struct GpuTimer {
  static constexpr int kRing = 8;
  ID3D11Query* disjoint[kRing] = {};
  ID3D11Query* begin[kRing] = {};
  ID3D11Query* end[kRing] = {};
  bool pending[kRing] = {};
  int next = 0;
  double total_ms = 0;
  int samples = 0;

  void init(ID3D11Device* dev) {
    D3D11_QUERY_DESC d = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC t = {D3D11_QUERY_TIMESTAMP, 0};
    for (int i = 0; i < kRing; ++i) {
      dev->CreateQuery(&d, &disjoint[i]);
      dev->CreateQuery(&t, &begin[i]);
      dev->CreateQuery(&t, &end[i]);
    }
  }
  void collect(ID3D11DeviceContext* c) {
    for (int i = 0; i < kRing; ++i) {
      if (!pending[i]) continue;
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
      UINT64 b, e;
      if (c->GetData(disjoint[i], &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
      if (c->GetData(begin[i], &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
      if (c->GetData(end[i], &e, sizeof(e), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
      pending[i] = false;
      if (!dj.Disjoint && dj.Frequency) {
        total_ms += 1000.0 * double(e - b) / double(dj.Frequency);
        ++samples;
      }
    }
  }
  int start(ID3D11DeviceContext* c) {
    if (!disjoint[0]) return -1;
    collect(c);
    int i = next;
    if (pending[i]) return -1;  // ring full; skip timing this frame
    next = (next + 1) % kRing;
    c->Begin(disjoint[i]);
    c->End(begin[i]);
    return i;
  }
  void stop(ID3D11DeviceContext* c, int i) {
    if (i < 0) return;
    c->End(end[i]);
    c->End(disjoint[i]);
    pending[i] = true;
  }
};

struct State {
  ID3D11Device* dev = nullptr;
  std::atomic<ID3D11Device*> ready_device{};
  ID3D11Device1* dev1 = nullptr;
  ID3D11DeviceContext* imm = nullptr;
  ID3D11DeviceContext1* imm1 = nullptr;
  ID3DDeviceContextState* own_state = nullptr;

  std::mutex mutex;  // guards everything below that's touched from recording threads
  bool kernels_ok = false, kernels_tried = false;
  kernels::Kernels k;

  // Motion vectors (render res): written in the game's command stream, read by DLSS.
  ID3D11Texture2D* mv = nullptr;
  ID3D11UnorderedAccessView* mv_uav = nullptr;
  ID3D11ShaderResourceView* mv_srv = nullptr;
  uint32_t mv_w = 0, mv_h = 0;

  std::unordered_map<ID3D11Resource*, ID3D11Resource*> lin_to_raw;  // linear Z -> raw depth copy
  std::unordered_set<ID3D11Resource*> taa_outputs;                   // TAA RT0 resources (rotating)
  std::unordered_map<ID3D11DeviceContext*, std::vector<Part>> pending;

  // Immediate-context-only state (render thread).
  ID3D11Texture2D* out = nullptr;
  ID3D11UnorderedAccessView* out_uav = nullptr;
  ID3D11ShaderResourceView* out_srv = nullptr;
  uint32_t out_w = 0, out_h = 0;
  std::unordered_map<ID3D11Resource*, ID3D11ShaderResourceView*> srv_cache;
  ngx::Params create_params, eval_params;
  // One DLSS feature (history) per eye; [1] is only used by stereo's second pass.
  struct Feature {
    NVSDK_NGX_Handle* handle = nullptr;
    uint32_t rw = 0, rh = 0, dw = 0, dh = 0;
    std::string preset;
    bool reset_next = true;
    uint64_t last_job_frame = 0;
    LARGE_INTEGER last_eval_time{};
  } feat[2];
  bool ngx_tried = false;
  std::atomic<bool> ngx_unavailable{false};
  // Last job executed (for Frame Generation).
  ID3D11Resource* fg_depth = nullptr;
  uint32_t fg_rw = 0, fg_rh = 0;
  float fg_jx = 0, fg_jy = 0;
  uint64_t fg_frame = 0;
  FgCamera fg_cam;
  ComPtr<ID3D11Texture2D> fg_mv;
  ComPtr<ID3D11Resource> valid_scene;
  bool output_composited = false;
  int valid_eye = 0;
  const char* output_backend = nullptr;
  const char* presented_backend = "game-taa";
  uint64_t last_swap_frame = UINT64_MAX;
  std::atomic<bool> logged_fallback{false};

  GpuTimer gpu_timer;
  LARGE_INTEGER perf_last{};
  uint64_t perf_frames = 0;
  double perf_max_ms = 0;
  LARGE_INTEGER frame_last{};

  std::atomic<uint64_t> display_size{0};  // a consistent width/height pair across resize threads
  std::atomic<bool> resized{false};
  std::atomic<uint64_t> resize_generation{0};
  std::atomic<uint64_t> frame{0};
  // per perf-window counters
  std::atomic<uint32_t> n_taa_calls{0}, n_bail{0}, n_taa{0}, n_jobs{0}, n_dlss{0}, n_comp{0}, n_swap{0};
  std::mutex jit_mutex;
  std::set<std::pair<float, float>> jitters;
  float render_scale_hint = 1.0f;  // for sampler LOD bias (known before the first frame)
} S;

void note_fallback(const char* reason) {
  if (!S.logged_fallback.exchange(true))
    ACO_LOG("upscaler: no upscale output (%s); using the game's TAA", reason);
}

bool ensure_kernels() {
  std::lock_guard lock(S.mutex);
  if (!S.kernels_tried) {
    S.kernels_tried = true;
    S.kernels_ok = kernels::create(S.dev, S.k);
    if (!S.kernels_ok) note_fallback("shader initialization failed");
  }
  return S.kernels_ok;
}

// Called with S.mutex held.
bool ensure_mv(uint32_t w, uint32_t h) {
  if (S.mv_srv && S.mv_w == w && S.mv_h == h) return true;
  safe_release(S.mv_uav);
  safe_release(S.mv_srv);
  safe_release(S.mv);
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = DXGI_FORMAT_R16G16_FLOAT;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  if (FAILED(S.dev->CreateTexture2D(&d, nullptr, &S.mv)) ||
      FAILED(S.dev->CreateUnorderedAccessView(S.mv, nullptr, &S.mv_uav)) ||
      FAILED(S.dev->CreateShaderResourceView(S.mv, nullptr, &S.mv_srv))) {
    note_fallback("motion vector texture allocation failed");
    return false;
  }
  S.mv_w = w;
  S.mv_h = h;
  ACO_LOG("upscaler: motion vectors %ux%u", w, h);
  return true;
}

// Render thread.
bool ensure_output(uint32_t w, uint32_t h) {
  std::lock_guard lock(S.mutex);
  if (S.out_srv && S.out_w == w && S.out_h == h) return true;
  safe_release(S.out_uav);
  safe_release(S.out_srv);
  safe_release(S.out);
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  if (FAILED(S.dev->CreateTexture2D(&d, nullptr, &S.out)) ||
      FAILED(S.dev->CreateUnorderedAccessView(S.out, nullptr, &S.out_uav)) ||
      FAILED(S.dev->CreateShaderResourceView(S.out, nullptr, &S.out_srv))) {
    note_fallback("output texture allocation failed");
    return false;
  }
  S.out_w = w;
  S.out_h = h;
  ACO_LOG("upscaler: output %ux%u", w, h);
  return true;
}

ID3D11ShaderResourceView* srv_for(ID3D11Resource* r, DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN) {
  auto it = S.srv_cache.find(r);
  if (it != S.srv_cache.end()) return it->second;
  D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
  D3D11_SHADER_RESOURCE_VIEW_DESC* pd = nullptr;
  if (fmt != DXGI_FORMAT_UNKNOWN) {
    d.Format = fmt;
    d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    d.Texture2D.MipLevels = 1;
    pd = &d;
  }
  ID3D11ShaderResourceView* v = nullptr;
  if (!r || FAILED(S.dev->CreateShaderResourceView(r, pd, &v))) return nullptr;
  r->AddRef();  // keep the key alive while cached
  S.srv_cache[r] = v;
  return v;
}

bool blit(ID3D11ShaderResourceView* src, uint32_t src_w, uint32_t src_h, kernels::BlitMode mode, float scale) {
  ID3D11DeviceContext* c = S.imm;
  D3D11_MAPPED_SUBRESOURCE m;
  if (!src || FAILED(c->Map(S.k.blit_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
  kernels::BlitParams p = {};
  p.dst_size_inv[0] = 1.0f / S.out_w;
  p.dst_size_inv[1] = 1.0f / S.out_h;
  p.src_size[0] = static_cast<float>(src_w);
  p.src_size[1] = static_cast<float>(src_h);
  p.mode = mode;
  p.scale = scale;
  memcpy(m.pData, &p, sizeof(p));
  c->Unmap(S.k.blit_cb, 0);
  c->CSSetShader(S.k.blit, nullptr, 0);
  c->CSSetConstantBuffers(0, 1, &S.k.blit_cb);
  c->CSSetShaderResources(0, 1, &src);
  c->CSSetSamplers(0, 1, &S.k.linear_clamp);
  c->CSSetUnorderedAccessViews(0, 1, &S.out_uav, nullptr);
  c->Dispatch((S.out_w + 7) / 8, (S.out_h + 7) / 8, 1);
  ID3D11ShaderResourceView* null_srv = nullptr;
  ID3D11UnorderedAccessView* null_uav = nullptr;
  c->CSSetShaderResources(0, 1, &null_srv);
  c->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
  return true;
}

bool ensure_feature(int eye, uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh) {
  const Config& cfg = config();
  auto& F = S.feat[eye];
  // A feature rejected at these dimensions and preset will also be rejected
  // on the next frame. Retry when its creation parameters change.
  if (F.rw == rw && F.rh == rh && F.dw == dw && F.dh == dh && F.preset == cfg.preset)
    return F.handle != nullptr;
  if (F.handle) {
    ngx::release_feature(F.handle);
    F.handle = nullptr;
  }
  F.rw = rw;
  F.rh = rh;
  F.dw = dw;
  F.dh = dh;
  F.preset = cfg.preset;
  F.reset_next = true;
  ngx::Params& p = S.create_params;
  if (!p) p = ngx::alloc_params();
  if (!p) return false;
  p.reset();
  int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
  p.set(NVSDK_NGX_Parameter_Width, rw);
  p.set(NVSDK_NGX_Parameter_Height, rh);
  p.set(NVSDK_NGX_Parameter_OutWidth, dw);
  p.set(NVSDK_NGX_Parameter_OutHeight, dh);
  p.set(NVSDK_NGX_Parameter_PerfQualityValue, ngx::perf_quality_for(static_cast<float>(rw) / dw));
  p.set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
  p.set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
  ngx::set_sr_preset(p, ngx::preset_value(cfg.preset));
  F.handle = ngx::create_feature(S.imm, NVSDK_NGX_Feature_SuperSampling, p);
  ACO_LOG("upscaler: DLSS feature (eye %d) %ux%u -> %ux%u preset=%s flags=0x%x: %s", eye, rw, rh, dw, dh,
          cfg.preset.c_str(), flags, F.handle ? "ok" : "FAILED");
  return F.handle != nullptr;
}

bool evaluate_dlss(const Job& job, uint64_t frame) {
  if (!S.ngx_tried) {
    S.ngx_tried = true;
    S.ngx_unavailable = !ngx::init(S.dev, out_dir()) || !ngx::dlss_available();
  }
  if (S.ngx_unavailable || !job.depth) return false;
  int eye = stereo_eye();
  auto& F = S.feat[eye];
  if (!ensure_feature(eye, job.render_w, job.render_h, S.out_w, S.out_h)) return false;
  const Config& cfg = config();
  ngx::Params& p = S.eval_params;
  if (!p) p = ngx::alloc_params();
  if (!p) return false;

  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  float dt_ms = F.last_eval_time.QuadPart ? 1000.0f * (now.QuadPart - F.last_eval_time.QuadPart) / freq.QuadPart : 16.6f;
  F.last_eval_time = now;
  bool reset = F.reset_next || frame > F.last_job_frame + 1;

  p.set(NVSDK_NGX_Parameter_Color, job.color);
  p.set(NVSDK_NGX_Parameter_Output, static_cast<ID3D11Resource*>(S.out));
  p.set(NVSDK_NGX_Parameter_Depth, job.depth);
  p.set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D11Resource*>(job.mv.Get()));
  p.set(NVSDK_NGX_Parameter_Jitter_Offset_X, job.jitter_x * cfg.jitter_x);
  p.set(NVSDK_NGX_Parameter_Jitter_Offset_Y, job.jitter_y * cfg.jitter_y);
  p.set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
  p.set(NVSDK_NGX_Parameter_Reset, reset ? 1 : 0);
  p.set(NVSDK_NGX_Parameter_MV_Scale_X, job.render_w * cfg.mv_x);
  p.set(NVSDK_NGX_Parameter_MV_Scale_Y, job.render_h * cfg.mv_y);
  p.set(NVSDK_NGX_Parameter_TransparencyMask, static_cast<ID3D11Resource*>(nullptr));
  p.set(NVSDK_NGX_Parameter_ExposureTexture, static_cast<ID3D11Resource*>(nullptr));
  p.set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, static_cast<ID3D11Resource*>(nullptr));
  p.set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
  p.set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
  p.set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, 0u);
  p.set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 0u);
  p.set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0u);
  p.set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, 0u);
  p.set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0u);
  p.set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0u);
  p.set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0u);
  p.set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0u);
  p.set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, job.render_w);
  p.set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, job.render_h);
  p.set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, dt_ms);

  NVSDK_NGX_Result r = ngx::evaluate(S.imm, F.handle, p);
  if (NVSDK_NGX_FAILED(r)) {
    F.reset_next = true;
    static std::once_flag first_eval_failure;
    std::call_once(first_eval_failure, [&] { ACO_LOG("upscaler: DLSS evaluate failed: %s", ngx::result_str(r)); });
    return false;
  }
  F.reset_next = false;
  F.last_job_frame = frame;
  static bool logged = false;
  if (!logged) {
    logged = true;
    ACO_LOG("upscaler: first DLSS evaluate ok (render %ux%u, jitter %g,%g)", job.render_w, job.render_h, job.jitter_x,
            job.jitter_y);
  }
  return true;
}

void run_job(const Job& job) {
  S.valid_scene = nullptr;
  S.output_backend = nullptr;
  S.output_composited = false;
  if (S.resized.load() || job.resize_generation != S.resize_generation.load()) return;
  uint64_t frame = S.frame.load();
  // The cursor uses engine depth even if NGX fails after the TAA was recorded.
  if (job.depth) job.depth->AddRef();
  safe_release(S.fg_depth);
  S.fg_depth = job.depth;
  S.fg_rw = job.render_w;
  S.fg_rh = job.render_h;
  S.fg_frame = frame;
  const Config& cfg = config();
  if (!cfg.enabled || !job.mv) return;
  uint64_t display = S.display_size.load();
  uint32_t dw = static_cast<uint32_t>(display >> 32), dh = static_cast<uint32_t>(display);
  static int diag = 0;
  if (diag++ < 4)
    ACO_LOG("upscaler: job frame=%llu render=%ux%u display=%ux%u jitter=(%g,%g) depth=%p", (unsigned long long)job.frame,
            job.render_w, job.render_h, dw, dh, job.jitter_x, job.jitter_y, static_cast<void*>(job.depth));
  if (!dw || !dh) {
    note_fallback("swapchain size unavailable");
    return;
  }
  if (!ensure_output(dw, dh)) return;
  // DXVK's SwapDeviceContextState doesn't take the context's multithread lock (other methods do), and
  // with Frame Generation's present thread ([fg] present_thread=1) another thread uses the immediate
  // context: hold the lock for the whole job. A no-op unless multithread protection is on.
  ID3D10Multithread* mt = nullptr;
  if (FAILED(S.imm->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(&mt)))) mt = nullptr;
  if (mt) mt->Enter();
  ++g_ctx_gate_exempt;  // holds the context lock: must not wait at the FG present gate
  // Run on a private context state so the game's bindings are untouched.
  ID3DDeviceContextState* prev = nullptr;
  S.imm1->SwapDeviceContextState(S.own_state, &prev);

  S.n_jobs++;
  int timer = S.gpu_timer.start(S.imm);
  if (cfg.debug == "motion") {
    if (blit(job.mv_srv.Get(), job.render_w, job.render_h, kernels::kBlitMotion, 0.05f))
      S.output_backend = "motion";
  } else if (cfg.debug == "depth") {
    if (blit(srv_for(job.depth, DXGI_FORMAT_R32_FLOAT), job.render_w, job.render_h, kernels::kBlitDepth, 0.25f))
      S.output_backend = "depth";
  } else if (cfg.backend == "bilinear") {
    if (blit(srv_for(job.color), job.render_w, job.render_h, kernels::kBlitColor, 1.0f))
      S.output_backend = "bilinear";
  } else if (cfg.backend == "dlss" && evaluate_dlss(job, frame)) {
    S.output_backend = "dlss";
    S.n_dlss++;
    std::lock_guard lock(S.jit_mutex);
    S.jitters.emplace(job.jitter_x, job.jitter_y);
  }
  if (S.output_backend) {
    S.valid_scene = job.taa_output;
    S.valid_eye = stereo_eye();
    S.fg_mv = job.mv;
    S.fg_jx = job.jitter_x * cfg.jitter_x;
    S.fg_jy = job.jitter_y * cfg.jitter_y;
    S.fg_cam = job.cam;
    if (bloom_probe_wants(frame))
      bloom_probe_sample(S.dev, S.imm, frame, S.valid_eye, srv_for(job.color), S.out_srv);
  } else {
    note_fallback(S.ngx_unavailable ? "NGX initialization or capability check failed" : "upscaler job failed");
  }
  S.gpu_timer.stop(S.imm, timer);

  S.imm1->SwapDeviceContextState(prev, nullptr);
  if (prev) prev->Release();
  --g_ctx_gate_exempt;
  if (mt) {
    mt->Leave();
    mt->Release();
  }
}

ComPtr<ID3D11CommandList> finish_part(ID3D11DeviceContext* ctx) {
  ComPtr<ID3D11CommandList> part;
  HRESULT hr = orig_FinishCommandList(ctx, TRUE, part.GetAddressOf());
  if (FAILED(hr)) ACO_LOG("upscaler: FinishCommandList split failed 0x%08lx", hr);
  return part;
}

bool split_here(ID3D11DeviceContext* ctx, std::shared_ptr<Job> job = {}) {
  Part part;
  part.list = finish_part(ctx);
  if (!part.list) return false;
  part.job = std::move(job);
  std::lock_guard lock(S.mutex);
  S.pending[ctx].push_back(std::move(part));
  return true;
}

bool wants_upscale() {
  const Config& cfg = config();
  return cfg.enabled && (!S.ngx_unavailable.load() || cfg.backend == "bilinear" || cfg.debug != "none");
}

bool on_linear_z(ID3D11DeviceContext* ctx) {
  ID3D11RenderTargetView* rtv[2] = {};
  ctx->OMGetRenderTargets(2, rtv, nullptr);
  if (rtv[0] && rtv[1]) {
    ID3D11Resource* lin = resource_of(rtv[0]);
    ID3D11Resource* raw = resource_of(rtv[1]);
    {
      std::lock_guard lock(S.mutex);
      auto [it, inserted] = S.lin_to_raw.emplace(lin, raw);
      if (inserted) {
        ACO_LOG("upscaler: linear Z %p <-> raw depth %p", static_cast<void*>(lin), static_cast<void*>(raw));
        lin->AddRef();
        raw->AddRef();
      } else if (it->second != raw) {
        it->second->Release();
        raw->AddRef();
        it->second = raw;
      }
    }
    lin->Release();
    raw->Release();
  }
  safe_release(rtv[0]);
  safe_release(rtv[1]);
  return false;  // the engine still needs its linear Z
}

bool on_taa(ID3D11DeviceContext* ctx) {
  const Config& cfg = config();
  uint64_t generation = S.resize_generation.load();
  S.n_taa_calls++;
  const bool upscale = wants_upscale() && ensure_kernels();
  if (!upscale && !cursor_needs_depth()) return false;

  ID3D11ShaderResourceView* srv[4] = {};
  ctx->PSGetShaderResources(kTaaColor, 4, srv);
  ID3D11RenderTargetView* rtv[1] = {};
  ctx->OMGetRenderTargets(1, rtv, nullptr);
  constexpr UINT ncb = title::temporal_slot + 1;
  ID3D11Buffer* cb[ncb] = {};
  UINT first[ncb] = {}, num[ncb] = {};
  ID3D11DeviceContext1* c1 = nullptr;
  ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1));
  if (c1) c1->PSGetConstantBuffers1(0, ncb, cb, first, num);

  auto cleanup = [&] {
    for (auto*& s : srv) safe_release(s);
    safe_release(rtv[0]);
    for (auto*& b : cb) safe_release(b);
    safe_release(c1);
  };

  TaaConstants tc;
  if (cb[title::temporal_slot]) {
    // The engine rotates this cbuffer through a pool of identical buffers: shadow them all by size.
    // Until the first write after this is seen, the frame falls back to the game's TAA.
    D3D11_BUFFER_DESC bd{};
    cb[title::temporal_slot]->GetDesc(&bd);
    cbshadow_watch_size(bd.ByteWidth);
  }
  bool have_tc = cb[title::temporal_slot] && cbshadow_read(ctx, cb[title::temporal_slot], first[title::temporal_slot] * 16, sizeof(tc), &tc);
  // Camera for Frame Generation ([fg] camera=engine): PassParams (cb1) and InstanceParams (cb3) are
  // pooled too; shadow them by size as well (until the first write after this, no camera).
  FgCamera cam;
  if (cfg.fg_mode == "d3d12" && cfg.fg_camera == "engine" && cb[title::pass_slot] && cb[title::instance_slot]) {
    D3D11_BUFFER_DESC b1{}, b3{};
    cb[title::pass_slot]->GetDesc(&b1);
    cb[title::instance_slot]->GetDesc(&b3);
    cbshadow_watch_size_ptr(b1.ByteWidth);
    cbshadow_watch_size_ptr(b3.ByteWidth);
    cam.valid = cbshadow_read(ctx, cb[title::pass_slot], first[title::pass_slot] * 16 + title::view_to_world * 16, sizeof(cam.view_to_world), cam.view_to_world) &&
                cbshadow_read(ctx, cb[title::pass_slot], first[title::pass_slot] * 16 + title::clip_to_view * 16, sizeof(cam.clip_to_view), cam.clip_to_view) &&
                cbshadow_read(ctx, cb[title::instance_slot], first[title::instance_slot] * 16 + 12 * 16, sizeof(cam.prev_view_proj), cam.prev_view_proj);
  }
  static std::atomic<int> diag{0};
  if (diag++ < 6)
    ACO_LOG("upscaler: TAA ctx=%p imm=%d have_tc=%d color=%p motion=%p depth=%p rt0=%p cb1=%p cb3=%p", static_cast<void*>(ctx),
            ctx == S.imm, have_tc, static_cast<void*>(srv[0]), static_cast<void*>(srv[1]), static_cast<void*>(srv[2]),
            static_cast<void*>(rtv[0]), static_cast<void*>(cb[title::pass_slot]), static_cast<void*>(cb[title::instance_slot]));
  if (!have_tc || !srv[0] || !srv[1] || !srv[2] || !rtv[0] || !cb[title::pass_slot] || !cb[title::instance_slot]) {
    if (S.n_bail++ < 40) {
      D3D11_BUFFER_DESC bd{};
      if (cb[title::temporal_slot]) cb[title::temporal_slot]->GetDesc(&bd);
      ACO_LOG("upscaler: TAA bail have_tc=%d srv=%d%d%d rtv=%d cb1=%d cb3=%d cb5=%p bw=%u usage=%u cpu=%x first=%u num=%u",
              have_tc, !!srv[0], !!srv[1], !!srv[2], !!rtv[0], !!cb[title::pass_slot], !!cb[title::instance_slot], static_cast<void*>(cb[title::temporal_slot]), bd.ByteWidth,
              bd.Usage, bd.CPUAccessFlags, first[title::temporal_slot], num[title::temporal_slot]);
    }
    cleanup();
    return false;
  }
  if (!(tc.low_res_size[0] >= 1 && tc.low_res_size[0] <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        tc.low_res_size[1] >= 1 && tc.low_res_size[1] <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        std::isfinite(tc.sub_pixel_jitter[0]) && std::isfinite(tc.sub_pixel_jitter[1]))) {
    cleanup();
    return false;
  }
  uint32_t rw = static_cast<uint32_t>(tc.low_res_size[0]), rh = static_cast<uint32_t>(tc.low_res_size[1]);

  ID3D11Resource* color = resource_of(srv[0]);
  ID3D11Resource* lin = resource_of(srv[2]);
  ID3D11Resource* taa_out = resource_of(rtv[0]);
  ID3D11Resource* raw = nullptr;
  auto job = std::make_shared<Job>();
  ComPtr<ID3D11UnorderedAccessView> mv_uav;
  {
    std::lock_guard lock(S.mutex);
    if (upscale && ensure_mv(rw, rh)) {
      mv_uav = S.mv_uav;
      job->mv = S.mv;
      job->mv_srv = S.mv_srv;
      if (S.taa_outputs.insert(taa_out).second) taa_out->AddRef();
    }
    auto it = S.lin_to_raw.find(lin);
    if (it != S.lin_to_raw.end()) {
      raw = it->second;
      raw->AddRef();
    }
  }

  // Record motion reconstruction only for upscale jobs. A cursor-only job
  // publishes the actual engine depth at execution time in each eye.
  if (mv_uav) {
    ID3D11ComputeShader* old_cs = nullptr;
    ctx->CSGetShader(&old_cs, nullptr, nullptr);
    ID3D11ShaderResourceView* old_srv[2] = {};
    ctx->CSGetShaderResources(0, 2, old_srv);
    ID3D11UnorderedAccessView* old_uav = nullptr;
    ctx->CSGetUnorderedAccessViews(0, 1, &old_uav);
    ID3D11Buffer* old_cb[3] = {};
    UINT old_first[3] = {}, old_num[3] = {};
    for (int i = 0; i < 3; ++i)
      c1->CSGetConstantBuffers1(kernels::kMotionCbSlots[i], 1, &old_cb[i], &old_first[i], &old_num[i]);

    ctx->CSSetShader(S.k.motion, nullptr, 0);
    ID3D11ShaderResourceView* in[2] = {srv[1], srv[2]};
    ctx->CSSetShaderResources(0, 2, in);
    ctx->CSSetUnorderedAccessViews(0, 1, mv_uav.GetAddressOf(), nullptr);
    for (int i = 0; i < 3; ++i) {
      UINT slot = kernels::kMotionCbSlots[i];
      constexpr UINT source_slots[] = {title::pass_slot, title::instance_slot, title::temporal_slot};
      UINT source = source_slots[i];
      c1->CSSetConstantBuffers1(slot, 1, &cb[source], &first[source], &num[source]);
    }
    ctx->Dispatch((rw + 7) / 8, (rh + 7) / 8, 1);

    ctx->CSSetShader(old_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 2, old_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &old_uav, nullptr);
    for (int i = 0; i < 3; ++i)
      c1->CSSetConstantBuffers1(kernels::kMotionCbSlots[i], 1, &old_cb[i], &old_first[i], &old_num[i]);
    safe_release(old_cs);
    for (auto*& s : old_srv) safe_release(s);
    safe_release(old_uav);
    for (auto*& b : old_cb) safe_release(b);
  }

  // Preserve the engine resolve and temporal history. The composite can be
  // recorded on a different worker before this job ever reaches NGX.
  job->color = color;
  job->depth = raw;
  job->taa_output.Attach(taa_out);
  job->render_w = rw;
  job->render_h = rh;
  job->jitter_x = tc.sub_pixel_jitter[0];
  job->jitter_y = tc.sub_pixel_jitter[1];
  job->frame = S.frame.load();
  job->resize_generation = generation;
  job->cam = cam;
  if (job->mv) S.n_taa++;
  lin->Release();
  cleanup();

  if (ctx == S.imm) {
    run_job(*job);
  } else {
    split_here(ctx, std::move(job));
  }
  return false;  // the caller issues the game's original TAA draw
}

// Execution is ordered on the presenting immediate context. Never consume an
// earlier frame, the other eye, a different engine target or a resized output.
bool use_upscaled_draw(ID3D11Resource* scene, ID3D11ShaderResourceView* upscaled) {
  if (S.resized.load() || !config().enabled || !S.valid_scene || S.valid_scene.Get() != scene || S.fg_frame != S.frame.load() ||
      S.valid_eye != stereo_eye() || upscaled != S.out_srv) {
    S.output_composited = false;
    S.last_swap_frame = UINT64_MAX;
    return false;
  }
  S.output_composited = true;
  S.last_swap_frame = S.fg_frame;
  S.presented_backend = S.output_backend;
  S.n_swap++;
  return true;
}

bool on_composite(ID3D11DeviceContext* ctx, const std::function<void()>& draw) {
  S.n_comp++;
  if (!wants_upscale()) return false;
  // Only the scene slot (t87 in Origins) is replaceable. The UI can reuse a
  // resource from the same pool at 100% render scale.
  constexpr UINT slot = title::texture0 + 1;
  ComPtr<ID3D11ShaderResourceView> native;
  ctx->PSGetShaderResources(slot, 1, native.GetAddressOf());
  if (!native) return false;
  Part part;
  native->GetResource(part.scene.GetAddressOf());
  {
    std::lock_guard lock(S.mutex);
    if (!S.out_srv || !S.taa_outputs.count(part.scene.Get())) return false;
    part.upscaled = S.out_srv;
  }
  if (ctx == S.imm) {
    if (!use_upscaled_draw(part.scene.Get(), part.upscaled.Get())) return false;
    ctx->PSSetShaderResources(slot, 1, part.upscaled.GetAddressOf());
    draw();
    ctx->PSSetShaderResources(slot, 1, native.GetAddressOf());
    return true;
  }

  if (!split_here(ctx)) return false;  // unconditional commands before the draw
  ctx->PSSetShaderResources(slot, 1, part.upscaled.GetAddressOf());
  draw();
  ctx->PSSetShaderResources(slot, 1, native.GetAddressOf());
  part.upscale_draw = finish_part(ctx);
  if (!part.upscale_draw) return false;
  draw();
  part.list = finish_part(ctx);
  if (part.list) {
    std::lock_guard lock(S.mutex);
    S.pending[ctx].push_back(std::move(part));
  }
  return true;
}

// Documents\Assassin's Creed Origins\ACO.ini
std::wstring game_ini_path() {
  wchar_t* docs = nullptr;
  std::wstring p;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
    p = std::wstring(docs) + title::game_ini;
    CoTaskMemFree(docs);
  }
  return p;
}

} // namespace

void upscaler_preinit() {
  const Config& cfg = config();
  std::wstring ini = game_ini_path();
  if (ini.empty()) return;
  if (cfg.enabled && cfg.render_scale > 0.0f) {
    wchar_t v[32];
    swprintf(v, 32, L"%.5f", cfg.render_scale);
    WritePrivateProfileStringW(L"Graphics", L"PixelDensity", v, ini.c_str());
    ACO_LOG("upscaler: set ACO.ini PixelDensity=%ls", v);
  }
  wchar_t buf[32];
  GetPrivateProfileStringW(L"Graphics", L"PixelDensity", L"1.0", buf, 32, ini.c_str());
  S.render_scale_hint = static_cast<float>(_wtof(buf));
  ACO_LOG("upscaler: render scale (PixelDensity) %g", S.render_scale_hint);
}

void upscaler_init(ID3D11Device* dev, ID3D11DeviceContext* immediate) {
  bloom_probe_init();
  S.dev = dev;
  S.imm = immediate;
  dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&S.dev1));
  immediate->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&S.imm1));
  D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
  HRESULT hr = S.dev1 ? S.dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr,
                                                         &S.own_state)
                      : E_NOINTERFACE;
  S.gpu_timer.init(dev);
  if (S.own_state && S.imm1) S.ready_device.store(dev, std::memory_order_release);
  else note_fallback("private D3D11 context state unavailable");
  ACO_LOG("upscaler: init presenting device %p (device context state 0x%08lx)", static_cast<void*>(dev), hr);
}

uint64_t upscaler_frame() { return S.frame.load(std::memory_order_relaxed); }

static void read_display_size(IDXGISwapChain* sc) {
  DXGI_SWAP_CHAIN_DESC d{};
  if (SUCCEEDED(sc->GetDesc(&d))) {
    // In full-width SBS, the hooked description already reports a single eye.
    S.display_size.store((uint64_t(d.BufferDesc.Width) << 32) | d.BufferDesc.Height);
  }
}

void upscaler_on_resize(IDXGISwapChain* sc) {
  S.resized.store(true);
  S.resize_generation.fetch_add(1);
  read_display_size(sc);
}

void upscaler_on_present(IDXGISwapChain* sc) {
  uint64_t f = S.frame.fetch_add(1) + 1;
  static IDXGISwapChain* previous = nullptr;  // identity only; DXGI owns the chain
  if (!previous) read_display_size(sc);
  else if (sc != previous) upscaler_on_resize(sc);
  previous = sc;
  if (S.resized.exchange(false)) {
    // Retire the previous resolution's cached engine textures on the immediate
    // thread. Command lists and jobs hold their own references until execution.
    S.valid_scene = nullptr;
    S.last_swap_frame = UINT64_MAX;
    safe_release(S.fg_depth);
    S.fg_mv.Reset();
    for (auto& feature : S.feat) {
      feature.reset_next = true;
      if (!feature.handle) feature.rw = 0;  // a fresh swapchain may permit feature creation
    }
    for (auto& [resource, view] : S.srv_cache) {
      resource->Release();
      safe_release(view);
    }
    S.srv_cache.clear();
    std::lock_guard lock(S.mutex);
    for (auto& [lin, raw] : S.lin_to_raw) {
      lin->Release();
      raw->Release();
    }
    S.lin_to_raw.clear();
    for (auto* target : S.taa_outputs) target->Release();
    S.taa_outputs.clear();
  }
  // Hotkey: DLSS <-> the game's own TAA, for A/B comparisons in play (edge-triggered).
  static bool key_was_down = false;
  if (int vk = config_toggle_vk()) {
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    if (down && !key_was_down) config_toggle_upscaler();
    key_was_down = down;
  }
  // Perf report every ~2 s: CPU frame pacing and GPU time spent in our upscaling work.
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  if (S.frame_last.QuadPart) {
    double ms = 1000.0 * double(now.QuadPart - S.frame_last.QuadPart) / double(freq.QuadPart);
    if (ms > S.perf_max_ms) S.perf_max_ms = ms;
  }
  S.frame_last = now;
  ++S.perf_frames;
  if (!S.perf_last.QuadPart) S.perf_last = now;
  double elapsed = double(now.QuadPart - S.perf_last.QuadPart) / double(freq.QuadPart);
  if (elapsed >= 2.0) {
    double gpu = S.gpu_timer.samples ? S.gpu_timer.total_ms / S.gpu_timer.samples : 0.0;
    ACO_LOG("perf: %.1f fps (%.2f ms avg, %.2f ms worst) | upscale gpu %.3f ms [%s] render %ux%u -> %ux%u",
            S.perf_frames / elapsed, 1000.0 * elapsed / S.perf_frames, S.perf_max_ms, gpu,
            S.last_swap_frame == f - 1 ? S.presented_backend : "game-taa",
            S.mv_w, S.mv_h, S.out_w, S.out_h);
    {
      std::lock_guard jl(S.jit_mutex);
      ACO_LOG("perf: taa_calls=%u bails=%u taa=%u jobs=%u dlss=%u game_composite=%u swapped=%u distinct_jitters=%zu", S.n_taa_calls.exchange(0), S.n_bail.load(), S.n_taa.exchange(0),
              S.n_jobs.exchange(0), S.n_dlss.exchange(0), S.n_comp.exchange(0), S.n_swap.exchange(0),
              S.jitters.size());
      S.jitters.clear();
    }
    S.perf_last = now;
    S.perf_frames = 0;
    S.perf_max_ms = 0;
    S.gpu_timer.total_ms = 0;
    S.gpu_timer.samples = 0;
  }
  if (f == 1 || f % 64 == 0) {
    read_display_size(sc);
    config_reload();
  }
}

bool upscaler_on_draw(ID3D11DeviceContext* ctx, ShaderClass cls, const std::function<void()>& draw) {
  if (cls != ShaderClass::LinearZ && cls != ShaderClass::TemporalAA && cls != ShaderClass::UIComposite) return false;
  auto* active = S.ready_device.load(std::memory_order_acquire);
  if (!active || S.resized.load()) return false;
  if (ctx != S.imm) {
    ComPtr<ID3D11Device> owner;
    ctx->GetDevice(&owner);
    if (owner.Get() != active) return false;
  }
  switch (cls) {
    case ShaderClass::LinearZ: return on_linear_z(ctx);
    case ShaderClass::TemporalAA: latency_on_taa(ctx); return on_taa(ctx);
    case ShaderClass::UIComposite: return on_composite(ctx, draw);
    default: return false;
  }
}

void upscaler_on_finish_command_list(ID3D11DeviceContext* ctx, ID3D11CommandList* cl) {
  std::vector<Part> parts;
  {
    std::lock_guard lock(S.mutex);
    auto it = S.pending.find(ctx);
    if (it == S.pending.end() || it->second.empty()) return;
    parts = std::move(it->second);
    S.pending.erase(it);
  }
  if (!cl) return;
  auto* chain = new Chain;
  chain->parts = std::move(parts);
  cl->SetPrivateDataInterface(kChainGuid, chain);
  chain->Release();
}

bool upscaler_on_execute_command_list(ID3D11DeviceContext* ctx, ID3D11CommandList* cl, BOOL restore) {
  if (!cl) return false;
  IUnknown* u = nullptr;
  UINT size = sizeof(u);
  if (FAILED(cl->GetPrivateData(kChainGuid, &size, &u)) || !u) return false;
  auto* chain = static_cast<Chain*>(u);
  for (const Part& part : chain->parts) {
    bool use_upscaled = part.upscale_draw && use_upscaled_draw(part.scene.Get(), part.upscaled.Get());
    orig_ExecuteCommandList(ctx, use_upscaled ? part.upscale_draw.Get() : part.list.Get(), restore);
    if (part.job) {
      trace_note(ctx, "\"what\":\"dlss\"");
      run_job(*part.job);
    }
  }
  orig_ExecuteCommandList(ctx, cl, restore);
  u->Release();
  return true;
}

ID3D11Texture2D* upscaler_cursor_depth(uint32_t& render_w, uint32_t& render_h) {
  if (S.resized.load() || !S.fg_depth || S.fg_frame != S.frame.load()) return nullptr;
  render_w = S.fg_rw;
  render_h = S.fg_rh;
  ID3D11Texture2D* depth = nullptr;
  S.fg_depth->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&depth));
  return depth;
}

bool upscaler_fg_inputs(FgInputs* o) {
  // Present advanced the frame counter. The *latest* contents of S.out must
  // have reached the composite, even if several jobs ran in this game frame.
  if (S.resized.load() || !S.output_composited || !S.valid_scene || !S.fg_depth || S.fg_frame + 1 != S.frame.load())
    return false;
  o->mv = S.fg_mv.Get();
  o->mv->AddRef();
  o->depth = S.fg_depth;
  o->depth->AddRef();
  o->hudless = S.out;
  o->hudless->AddRef();
  o->render_w = S.fg_rw;
  o->render_h = S.fg_rh;
  o->jitter_x = S.fg_jx;
  o->jitter_y = S.fg_jy;
  o->frame = S.fg_frame;
  o->cam = S.fg_cam;
  return true;
}

void upscaler_adjust_sampler(D3D11_SAMPLER_DESC* d) {
  const Config& cfg = config();
  if (!cfg.enabled || S.render_scale_hint >= 0.999f) return;
  bool aniso = d->Filter == D3D11_FILTER_ANISOTROPIC;
  bool mip_linear_wrap = (d->Filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR || d->Filter == D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR ||
                          d->Filter == D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR) &&
                         d->AddressU == D3D11_TEXTURE_ADDRESS_WRAP;
  if (!aniso && !mip_linear_wrap) return;
  float bias = cfg.mip_bias == "auto" ? std::log2(S.render_scale_hint) + cfg.mip_bias_offset
                                      : static_cast<float>(atof(cfg.mip_bias.c_str()));
  d->MipLODBias += bias;
  static std::atomic<int> n{0};
  if (n++ < 5) ACO_LOG("upscaler: sampler LOD bias %+.2f (filter %d)", bias, d->Filter);
}

} // namespace aco
