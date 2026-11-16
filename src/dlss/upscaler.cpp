#include "dlss/upscaler.h"

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <shlobj.h>

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
#include "core/config.h"
#include "core/ctxhooks.h"
#include "core/log.h"
#include "dlss/kernels.h"
#include "dlss/ngx.h"
#include "trace/trace.h"

namespace aco {

namespace {

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
constexpr UINT kTaaColor = 86, kTaaMotion = 87, kTaaDepth = 88, kTaaResponsive = 89;

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
  ID3D11Resource* responsive = nullptr;
  uint32_t render_w = 0, render_h = 0;
  float jitter_x = 0, jitter_y = 0;
  uint64_t frame = 0;
  ~Job() {
    safe_release(color);
    safe_release(depth);
    safe_release(responsive);
  }
};

// Attached to the game's command list (SetPrivateDataInterface) when we split it: the parts that
// precede it, each followed by a DLSS job. Released together with the command list.
struct Chain final : IUnknown {
  std::atomic<ULONG> refs{1};
  std::vector<std::pair<ID3D11CommandList*, std::shared_ptr<Job>>> parts;
  ~Chain() {
    for (auto& p : parts) p.first->Release();
  }
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
  std::unordered_map<ID3D11DeviceContext*, std::vector<std::pair<ID3D11CommandList*, std::shared_ptr<Job>>>> pending;

  // Immediate-context-only state (render thread).
  ID3D11Texture2D* out = nullptr;
  ID3D11UnorderedAccessView* out_uav = nullptr;
  ID3D11ShaderResourceView* out_srv = nullptr;
  uint32_t out_w = 0, out_h = 0;
  std::unordered_map<ID3D11Resource*, ID3D11ShaderResourceView*> srv_cache;
  ngx::Params create_params, eval_params;
  NVSDK_NGX_Handle* feature = nullptr;
  uint32_t feat_rw = 0, feat_rh = 0, feat_dw = 0, feat_dh = 0;
  std::string feat_preset;
  bool ngx_tried = false, ngx_ok = false;
  bool reset_next = true;
  uint64_t last_job_frame = 0;
  int eval_failures = 0;
  LARGE_INTEGER last_eval_time{};

  GpuTimer gpu_timer;
  LARGE_INTEGER perf_last{};
  uint64_t perf_frames = 0;
  double perf_max_ms = 0;
  LARGE_INTEGER frame_last{};

  std::atomic<uint32_t> display_w{0}, display_h{0};
  std::atomic<uint64_t> frame{0};
  std::atomic<uint64_t> taa_replaced_frame{0};
  // per perf-window counters
  std::atomic<uint32_t> n_taa_calls{0}, n_bail{0}, n_taa{0}, n_jobs{0}, n_dlss{0}, n_comp{0}, n_swap{0};
  std::mutex jit_mutex;
  std::set<std::pair<float, float>> jitters;
  float render_scale_hint = 1.0f;  // for sampler LOD bias (known before the first frame)
} S;

bool ensure_kernels() {
  std::lock_guard lock(S.mutex);
  if (!S.kernels_tried) {
    S.kernels_tried = true;
    S.kernels_ok = kernels::create(S.dev, S.k);
  }
  return S.kernels_ok;
}

// Called with S.mutex held.
bool ensure_mv(uint32_t w, uint32_t h) {
  if (S.mv && S.mv_w == w && S.mv_h == h) return true;
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
    ACO_LOG("upscaler: failed to create %ux%u motion vector texture", w, h);
    return false;
  }
  S.mv_w = w;
  S.mv_h = h;
  ACO_LOG("upscaler: motion vectors %ux%u", w, h);
  return true;
}

// Render thread.
bool ensure_output(uint32_t w, uint32_t h) {
  if (S.out && S.out_w == w && S.out_h == h) return true;
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
    ACO_LOG("upscaler: failed to create %ux%u output texture", w, h);
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
  S.dev->CreateShaderResourceView(r, pd, &v);
  r->AddRef();  // keep the key alive while cached
  S.srv_cache[r] = v;
  return v;
}

void blit(ID3D11ShaderResourceView* src, uint32_t src_w, uint32_t src_h, kernels::BlitMode mode, float scale) {
  ID3D11DeviceContext* c = S.imm;
  D3D11_MAPPED_SUBRESOURCE m;
  if (SUCCEEDED(c->Map(S.k.blit_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
    kernels::BlitParams p = {};
    p.dst_size_inv[0] = 1.0f / S.out_w;
    p.dst_size_inv[1] = 1.0f / S.out_h;
    p.src_size[0] = static_cast<float>(src_w);
    p.src_size[1] = static_cast<float>(src_h);
    p.mode = mode;
    p.scale = scale;
    memcpy(m.pData, &p, sizeof(p));
    c->Unmap(S.k.blit_cb, 0);
  }
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
}

unsigned preset_value(const std::string& p) {
  if (p.size() == 1 && p[0] >= 'A' && p[0] <= 'Z') return static_cast<unsigned>(p[0] - 'A' + 1);
  return NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
}

int perf_quality_for(float ratio) {
  if (ratio >= 0.99f) return NVSDK_NGX_PerfQuality_Value_DLAA;
  if (ratio >= 0.66f) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
  if (ratio >= 0.57f) return NVSDK_NGX_PerfQuality_Value_Balanced;
  if (ratio >= 0.49f) return NVSDK_NGX_PerfQuality_Value_MaxPerf;
  return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

bool ensure_feature(uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh) {
  const Config& cfg = config();
  if (S.feature && S.feat_rw == rw && S.feat_rh == rh && S.feat_dw == dw && S.feat_dh == dh &&
      S.feat_preset == cfg.preset)
    return true;
  if (S.feature) {
    ngx::release_feature(S.feature);
    S.feature = nullptr;
  }
  ngx::Params& p = S.create_params;
  if (!p) p = ngx::alloc_params();
  if (!p) return false;
  p.reset();
  int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
  p.set(NVSDK_NGX_Parameter_Width, rw);
  p.set(NVSDK_NGX_Parameter_Height, rh);
  p.set(NVSDK_NGX_Parameter_OutWidth, dw);
  p.set(NVSDK_NGX_Parameter_OutHeight, dh);
  p.set(NVSDK_NGX_Parameter_PerfQualityValue, perf_quality_for(static_cast<float>(rw) / dw));
  p.set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
  p.set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
  unsigned preset = preset_value(cfg.preset);
  for (const char* key :
       {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
    p.set(key, preset);
  S.feature = ngx::create_feature(S.imm, NVSDK_NGX_Feature_SuperSampling, p);
  ACO_LOG("upscaler: DLSS feature %ux%u -> %ux%u preset=%s flags=0x%x: %s", rw, rh, dw, dh, cfg.preset.c_str(), flags,
          S.feature ? "ok" : "FAILED");
  S.feat_rw = rw;
  S.feat_rh = rh;
  S.feat_dw = dw;
  S.feat_dh = dh;
  S.feat_preset = cfg.preset;
  S.reset_next = true;
  return S.feature != nullptr;
}

bool evaluate_dlss(const Job& job) {
  if (!S.ngx_tried) {
    S.ngx_tried = true;
    S.ngx_ok = ngx::init(S.dev, out_dir()) && ngx::dlss_available();
  }
  if (!S.ngx_ok || !job.depth) return false;
  if (!ensure_feature(job.render_w, job.render_h, S.out_w, S.out_h)) return false;
  const Config& cfg = config();
  ngx::Params& p = S.eval_params;
  if (!p) p = ngx::alloc_params();
  if (!p) return false;

  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  float dt_ms = S.last_eval_time.QuadPart ? 1000.0f * (now.QuadPart - S.last_eval_time.QuadPart) / freq.QuadPart : 16.6f;
  S.last_eval_time = now;
  bool reset = S.reset_next || job.frame > S.last_job_frame + 2;
  S.reset_next = false;
  S.last_job_frame = job.frame;

  p.set(NVSDK_NGX_Parameter_Color, job.color);
  p.set(NVSDK_NGX_Parameter_Output, static_cast<ID3D11Resource*>(S.out));
  p.set(NVSDK_NGX_Parameter_Depth, job.depth);
  p.set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D11Resource*>(S.mv));
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

  NVSDK_NGX_Result r = ngx::evaluate(S.imm, S.feature, p);
  if (NVSDK_NGX_FAILED(r)) {
    if (S.eval_failures++ < 10) ACO_LOG("upscaler: DLSS evaluate failed: %s", ngx::result_str(r));
    return false;
  }
  static bool logged = false;
  if (!logged) {
    logged = true;
    ACO_LOG("upscaler: first DLSS evaluate ok (render %ux%u, jitter %g,%g)", job.render_w, job.render_h, job.jitter_x,
            job.jitter_y);
  }
  return true;
}

void run_job(const Job& job) {
  uint32_t dw = S.display_w.load(), dh = S.display_h.load();
  static int diag = 0;
  if (diag++ < 4)
    ACO_LOG("upscaler: job frame=%llu render=%ux%u display=%ux%u jitter=(%g,%g) depth=%p", (unsigned long long)job.frame,
            job.render_w, job.render_h, dw, dh, job.jitter_x, job.jitter_y, static_cast<void*>(job.depth));
  if (!dw || !dh) return;
  if (!ensure_output(dw, dh)) return;
  // Run on a private context state so the game's bindings are untouched.
  ID3DDeviceContextState* prev = nullptr;
  S.imm1->SwapDeviceContextState(S.own_state, &prev);

  const Config& cfg = config();
  bool done = false;
  S.n_jobs++;
  int timer = S.gpu_timer.start(S.imm);
  if (cfg.debug == "motion") {
    blit(S.mv_srv, job.render_w, job.render_h, kernels::kBlitMotion, 0.05f);
    done = true;
  } else if (cfg.debug == "depth" && job.depth) {
    blit(srv_for(job.depth, DXGI_FORMAT_R32_FLOAT), job.render_w, job.render_h, kernels::kBlitDepth, 0.25f);
    done = true;
  } else if (cfg.backend == "dlss") {
    done = evaluate_dlss(job);
    if (done) S.n_dlss++;
  }
  if (!done) blit(srv_for(job.color), job.render_w, job.render_h, kernels::kBlitColor, 1.0f);
  S.gpu_timer.stop(S.imm, timer);

  S.imm1->SwapDeviceContextState(prev, nullptr);
  if (prev) prev->Release();
}

void split_here(ID3D11DeviceContext* ctx, std::shared_ptr<Job> job) {
  ID3D11CommandList* part = nullptr;
  HRESULT hr = orig_FinishCommandList(ctx, TRUE, &part);
  if (FAILED(hr) || !part) {
    ACO_LOG("upscaler: FinishCommandList split failed 0x%08lx", hr);
    return;
  }
  std::lock_guard lock(S.mutex);
  S.pending[ctx].emplace_back(part, std::move(job));
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
  S.n_taa_calls++;
  if (!cfg.enabled || !ensure_kernels()) return false;

  ID3D11ShaderResourceView* srv[4] = {};
  ctx->PSGetShaderResources(kTaaColor, 4, srv);
  ID3D11RenderTargetView* rtv[1] = {};
  ctx->OMGetRenderTargets(1, rtv, nullptr);
  ID3D11Buffer* cb[6] = {};
  UINT first[6] = {}, num[6] = {};
  ID3D11DeviceContext1* c1 = nullptr;
  ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1));
  c1->PSGetConstantBuffers1(0, 6, cb, first, num);

  auto cleanup = [&] {
    for (auto*& s : srv) safe_release(s);
    safe_release(rtv[0]);
    for (auto*& b : cb) safe_release(b);
    safe_release(c1);
  };

  TaaConstants tc;
  if (cb[5]) {
    // The engine rotates this cbuffer through a pool of identical buffers: shadow them all by size.
    // Until the first write after this is seen, the frame falls back to the game's TAA.
    D3D11_BUFFER_DESC bd{};
    cb[5]->GetDesc(&bd);
    cbshadow_watch_size(bd.ByteWidth);
  }
  bool have_tc = cb[5] && cbshadow_read(ctx, cb[5], first[5] * 16, sizeof(tc), &tc);
  static std::atomic<int> diag{0};
  if (diag++ < 6)
    ACO_LOG("upscaler: TAA ctx=%p imm=%d have_tc=%d color=%p motion=%p depth=%p rt0=%p cb1=%p cb3=%p", static_cast<void*>(ctx),
            ctx == S.imm, have_tc, static_cast<void*>(srv[0]), static_cast<void*>(srv[1]), static_cast<void*>(srv[2]),
            static_cast<void*>(rtv[0]), static_cast<void*>(cb[1]), static_cast<void*>(cb[3]));
  if (!have_tc || !srv[0] || !srv[1] || !srv[2] || !rtv[0] || !cb[1] || !cb[3]) {
    if (S.n_bail++ < 40) {
      D3D11_BUFFER_DESC bd{};
      if (cb[5]) cb[5]->GetDesc(&bd);
      ACO_LOG("upscaler: TAA bail have_tc=%d srv=%d%d%d rtv=%d cb1=%d cb3=%d cb5=%p bw=%u usage=%u cpu=%x first=%u num=%u",
              have_tc, !!srv[0], !!srv[1], !!srv[2], !!rtv[0], !!cb[1], !!cb[3], static_cast<void*>(cb[5]), bd.ByteWidth,
              bd.Usage, bd.CPUAccessFlags, first[5], num[5]);
    }
    cleanup();
    return false;
  }
  uint32_t rw = static_cast<uint32_t>(tc.low_res_size[0]), rh = static_cast<uint32_t>(tc.low_res_size[1]);

  ID3D11Resource* color = resource_of(srv[0]);
  ID3D11Resource* lin = resource_of(srv[2]);
  ID3D11Resource* taa_out = resource_of(rtv[0]);
  ID3D11Resource* raw = nullptr;
  {
    std::lock_guard lock(S.mutex);
    if (!ensure_mv(rw, rh)) {
      color->Release();
      lin->Release();
      taa_out->Release();
      cleanup();
      return false;
    }
    auto it = S.lin_to_raw.find(lin);
    if (it != S.lin_to_raw.end()) {
      raw = it->second;
      raw->AddRef();
    }
    if (S.taa_outputs.insert(taa_out).second) taa_out->AddRef();
  }

  // 1) Motion vectors, recorded in the game's stream with the TAA pass's own constant buffers.
  {
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
    ctx->CSSetUnorderedAccessViews(0, 1, &S.mv_uav, nullptr);
    for (int i = 0; i < 3; ++i) {
      UINT slot = kernels::kMotionCbSlots[i];
      c1->CSSetConstantBuffers1(slot, 1, &cb[slot], &first[slot], &num[slot]);
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

  // 2) Keep engine consumers of the TAA output (screenshots, next-frame effects) fed with the
  //    un-antialiased colour, since the TAA resolve no longer writes it.
  ctx->CopyResource(taa_out, color);

  // 3) DLSS runs between the two halves of this command list.
  auto job = std::make_shared<Job>();
  job->color = color;  // ownership transferred
  job->depth = raw;
  job->responsive = resource_of(srv[3]);
  job->render_w = rw;
  job->render_h = rh;
  job->jitter_x = tc.sub_pixel_jitter[0];
  job->jitter_y = tc.sub_pixel_jitter[1];
  job->frame = S.frame.load();
  S.taa_replaced_frame.store(job->frame);
  S.n_taa++;
  {
    std::lock_guard jl(S.jit_mutex);
    S.jitters.emplace(job->jitter_x, job->jitter_y);
  }
  lin->Release();
  taa_out->Release();
  cleanup();

  if (ctx == S.imm) {
    run_job(*job);
  } else {
    split_here(ctx, std::move(job));
  }
  return true;
}

bool on_composite(ID3D11DeviceContext* ctx, const std::function<void()>& draw) {
  const Config& cfg = config();
  static std::atomic<int> diag{0};
  if (diag++ < 8)
    ACO_LOG("upscaler: composite ctx=%p imm=%d out_srv=%p replaced_frame=%llu frame=%llu", static_cast<void*>(ctx),
            ctx == S.imm, static_cast<void*>(S.out_srv), (unsigned long long)S.taa_replaced_frame.load(),
            (unsigned long long)S.frame.load());
  S.n_comp++;
  // Only swap if the TAA of this frame was actually replaced (else the TAA output is valid).
  if (!cfg.enabled || !S.out_srv || S.taa_replaced_frame.load() + 1 < S.frame.load()) return false;
  ID3D11ShaderResourceView* srv[2] = {};
  ctx->PSGetShaderResources(86, 2, srv);
  int slot = -1;
  {
    std::lock_guard lock(S.mutex);
    for (int i = 0; i < 2 && slot < 0; ++i) {
      if (!srv[i]) continue;
      ID3D11Resource* r = resource_of(srv[i]);
      if (S.taa_outputs.count(r)) slot = i;
      r->Release();
    }
  }
  if (diag < 10) {
    ID3D11Resource* r0 = srv[0] ? resource_of(srv[0]) : nullptr;
    ID3D11Resource* r1 = srv[1] ? resource_of(srv[1]) : nullptr;
    ACO_LOG("upscaler: composite t86=%p t87=%p -> slot %d (%zu known TAA outputs)", static_cast<void*>(r0),
            static_cast<void*>(r1), slot, S.taa_outputs.size());
    if (r0) r0->Release();
    if (r1) r1->Release();
  }
  if (slot >= 0) {
    S.n_swap++;
    ctx->PSSetShaderResources(86 + slot, 1, &S.out_srv);
    draw();
    ctx->PSSetShaderResources(86 + slot, 1, &srv[slot]);
  }
  for (auto*& s : srv) safe_release(s);
  return slot >= 0;
}

// Documents\Assassin's Creed Origins\ACO.ini
std::wstring game_ini_path() {
  wchar_t* docs = nullptr;
  std::wstring p;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
    p = std::wstring(docs) + L"\\Assassin's Creed Origins\\ACO.ini";
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
  // The game creates probe devices before the real one; always bind to the newest. Lazily created
  // resources belong to whichever device was current when first needed (the real one).
  if (S.dev == dev) return;
  if (S.own_state) S.own_state->Release();
  S.own_state = nullptr;
  if (S.dev1) S.dev1->Release();
  if (S.imm1) S.imm1->Release();
  S.dev1 = nullptr;
  S.imm1 = nullptr;
  S.dev = dev;
  S.imm = immediate;
  dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&S.dev1));
  immediate->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&S.imm1));
  D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
  HRESULT hr = S.dev1 ? S.dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr,
                                                         &S.own_state)
                      : E_NOINTERFACE;
  S.gpu_timer = GpuTimer();
  S.gpu_timer.init(dev);
  ACO_LOG("upscaler: init (device context state 0x%08lx)", hr);
}

void upscaler_on_present(IDXGISwapChain* sc) {
  uint64_t f = S.frame.fetch_add(1) + 1;
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
    const Config& cfg = config();
    double gpu = S.gpu_timer.samples ? S.gpu_timer.total_ms / S.gpu_timer.samples : 0.0;
    ACO_LOG("perf: %.1f fps (%.2f ms avg, %.2f ms worst) | upscale gpu %.3f ms [%s%s] render %ux%u -> %ux%u",
            S.perf_frames / elapsed, 1000.0 * elapsed / S.perf_frames, S.perf_max_ms, gpu,
            cfg.enabled ? cfg.backend.c_str() : "game-taa", cfg.debug == "none" ? "" : (" debug=" + cfg.debug).c_str(),
            S.mv_w, S.mv_h, S.out_w, S.out_h);
    {
      std::lock_guard jl(S.jit_mutex);
      ACO_LOG("perf: taa_calls=%u bails=%u taa=%u jobs=%u dlss=%u composite=%u swapped=%u distinct_jitters=%zu", S.n_taa_calls.exchange(0), S.n_bail.load(), S.n_taa.exchange(0),
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
    DXGI_SWAP_CHAIN_DESC d;
    if (SUCCEEDED(sc->GetDesc(&d))) {
      S.display_w.store(d.BufferDesc.Width);
      S.display_h.store(d.BufferDesc.Height);
    }
    config_reload();
  }
}

bool upscaler_on_draw(ID3D11DeviceContext* ctx, ShaderClass cls, const std::function<void()>& draw) {
  if (!S.dev || !S.own_state) return false;
  switch (cls) {
    case ShaderClass::LinearZ: return on_linear_z(ctx);
    case ShaderClass::TemporalAA: return on_taa(ctx);
    case ShaderClass::UIComposite: return on_composite(ctx, draw);
    default: return false;
  }
}

void upscaler_on_finish_command_list(ID3D11DeviceContext* ctx, ID3D11CommandList* cl) {
  std::vector<std::pair<ID3D11CommandList*, std::shared_ptr<Job>>> parts;
  {
    std::lock_guard lock(S.mutex);
    auto it = S.pending.find(ctx);
    if (it == S.pending.end() || it->second.empty()) return;
    parts = std::move(it->second);
    S.pending.erase(it);
  }
  if (!cl) {
    for (auto& p : parts) p.first->Release();
    return;
  }
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
  for (auto& [part, job] : chain->parts) {
    orig_ExecuteCommandList(ctx, part, restore);
    trace_note(ctx, "\"what\":\"dlss\"");
    run_job(*job);
  }
  orig_ExecuteCommandList(ctx, cl, restore);
  u->Release();
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
