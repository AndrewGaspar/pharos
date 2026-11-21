#include "stereo/stereo.h"
#include "rig/capture.h"

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi.h>

#include <atomic>
#include <cstring>
#include <string>

#include "core/config.h"
#include "core/hyprland.h"
#include "core/quiver.h"
#include "fg/fg_d3d12.h"
#include "core/log.h"
#include "dlss/kernels.h"
#include "dlss/upscaler.h"
#include "trace/bloom_probe.h"
#include "stereo/camera.h"
#include "stereo/cursor.h"
#include "stereo/markers.h"
#include "stereo/marker_history.h"
#include "stereo/record.h"
#include "stereo/temporal.h"

namespace aco {

namespace {

// Composition: side by side (each eye squeezed to half width), one eye, or |L - R| amplified.
// u1 collects difference statistics (sum of |L-R| over RGB in 1/255 units, pixels that differ,
// largest difference) when stats != 0.
const char kComposeHlsl[] = R"(
cbuffer P : register(b0) { uint W; uint H; uint Mode; uint Stats; float Gain; uint Swap; uint2 pad; }
Texture2D<float4> L : register(t0);
Texture2D<float4> R : register(t1);
RWTexture2D<float4> O : register(u0);
RWByteAddressBuffer S : register(u1);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= W || id.y >= H) return;
    float4 l = L.Load(int3(id, 0)), r = R.Load(int3(id, 0));
    if (Stats != 0) {
        uint3 d = (uint3)round(abs(l.rgb - r.rgb) * 255.0);
        uint s = d.x + d.y + d.z;
        if (s != 0) {
            S.InterlockedAdd(0, s);
            S.InterlockedAdd(4, 1);
            S.InterlockedMax(8, max(d.x, max(d.y, d.z)));
        }
    }
    float4 c;
    if (Mode == 0) {
        uint half_w = W / 2;
        bool right = id.x >= half_w;
        uint x = (right ? id.x - half_w : id.x) * 2;
        bool use_r = right != (Swap != 0);
        float4 a = use_r ? R.Load(int3(x, id.y, 0)) : L.Load(int3(x, id.y, 0));
        float4 b = use_r ? R.Load(int3(min(x + 1, W - 1), id.y, 0)) : L.Load(int3(min(x + 1, W - 1), id.y, 0));
        c = (a + b) * 0.5;
    } else if (Mode == 1) {
        c = l;
    } else if (Mode == 2) {
        c = r;
    } else {
        c = float4(saturate(abs(l.rgb - r.rgb) * Gain), 1.0);
    }
    O[id] = c;
}
)";

struct ComposeParams {
  UINT w, h, mode, stats;
  float gain;
  UINT swap;
  UINT pad[2];
};

template <typename T>
void release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

struct State {
  ID3D11Device* dev = nullptr;
  ID3D11Device1* dev1 = nullptr;
  ID3D11DeviceContext* imm = nullptr;
  ID3D11DeviceContext1* imm1 = nullptr;
  ID3DDeviceContextState* own_state = nullptr;
  ID3D11ComputeShader* compose = nullptr;
  ID3D11Buffer* cb = nullptr;
  bool failed = false;

  // Per back-buffer size.
  UINT w = 0, h = 0;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
  ID3D11Texture2D* eye[2] = {};
  ID3D11ShaderResourceView* eye_srv[2] = {};
  ID3D11UnorderedAccessView* eye_uav[2] = {};  // cursor drawing
  ID3D11Texture2D* out = nullptr;
  ID3D11UnorderedAccessView* out_uav = nullptr;

  // Difference statistics: a UAV buffer and a staging ring, read a few frames later.
  ID3D11Buffer* stats = nullptr;
  ID3D11UnorderedAccessView* stats_uav = nullptr;
  ID3D11Buffer* stats_read[4] = {};
  uint64_t stats_frame[4] = {};
  int stats_slot = 0;

  // Full side by side (output = sbs): the swapchain is twice the game's width; the game draws one
  // eye into `proxy` (what GetBuffer returns), and each eye is copied 1:1 into its half.
  IDXGISwapChain* wide_sc = nullptr;
  UINT game_w = 0, game_h = 0;
  ID3D11Texture2D* proxy = nullptr;
  std::string tag;  // Hyprland window tag applied
  PFN_GetBuffer real_get_buffer = nullptr;

  bool active = false;
  unsigned hold_frames = 0;  // remaining presents for which the last stereo pair is fresh
  uint64_t frames = 0, replays = 0;
  unsigned held = 0, mono = 0;
  size_t cmds_sum = 0, bytes_sum = 0;
  double replay_ms = 0;
  const char* last_bad = nullptr;
} S;

std::atomic<int> g_eye{0};

bool ensure_device_objects() {
  if (S.compose) return true;
  if (S.failed) return false;
  markers_init(S.dev);
  S.compose = kernels::compile(S.dev, kComposeHlsl, sizeof(kComposeHlsl) - 1, "aco_stereo_compose.hlsl");
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = sizeof(ComposeParams);
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  S.dev->CreateBuffer(&bd, nullptr, &S.cb);
  D3D11_BUFFER_DESC sd{};
  sd.ByteWidth = 16;
  sd.Usage = D3D11_USAGE_DEFAULT;
  sd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  sd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  S.dev->CreateBuffer(&sd, nullptr, &S.stats);
  if (S.stats) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = 4;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    S.dev->CreateUnorderedAccessView(S.stats, &ud, &S.stats_uav);
  }
  D3D11_BUFFER_DESC rd{};
  rd.ByteWidth = 16;
  rd.Usage = D3D11_USAGE_STAGING;
  rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  for (auto& b : S.stats_read) S.dev->CreateBuffer(&rd, nullptr, &b);
  if (!S.compose || !S.cb || !S.stats_uav || !S.own_state) {
    ACO_LOG("stereo: device objects failed (compose=%p cb=%p stats=%p state=%p); stereo off",
            static_cast<void*>(S.compose), static_cast<void*>(S.cb), static_cast<void*>(S.stats_uav),
            static_cast<void*>(S.own_state));
    S.failed = true;
    return false;
  }
  return true;
}

bool ensure_targets(ID3D11Texture2D* bb) {
  D3D11_TEXTURE2D_DESC d;
  bb->GetDesc(&d);
  if (S.out && d.Width == S.w && d.Height == S.h && d.Format == S.fmt) return true;
  S.hold_frames = 0;
  for (int i = 0; i < 2; ++i) {
    release(S.eye_uav[i]);
    release(S.eye_srv[i]);
    release(S.eye[i]);
  }
  release(S.out_uav);
  release(S.out);
  S.w = d.Width;
  S.h = d.Height;
  S.fmt = d.Format;
  D3D11_TEXTURE2D_DESC t{};
  t.Width = d.Width;
  t.Height = d.Height;
  t.MipLevels = 1;
  t.ArraySize = 1;
  t.Format = d.Format;
  t.SampleDesc = {1, 0};
  t.Usage = D3D11_USAGE_DEFAULT;
  t.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  for (int i = 0; i < 2; ++i) {
    if (FAILED(S.dev->CreateTexture2D(&t, nullptr, &S.eye[i]))) return false;
    if (FAILED(S.dev->CreateShaderResourceView(S.eye[i], nullptr, &S.eye_srv[i]))) return false;
    S.dev->CreateUnorderedAccessView(S.eye[i], nullptr, &S.eye_uav[i]);  // cursor only; may stay null
  }
  t.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  if (FAILED(S.dev->CreateTexture2D(&t, nullptr, &S.out)) ||
      FAILED(S.dev->CreateUnorderedAccessView(S.out, nullptr, &S.out_uav))) {
    ACO_LOG("stereo: no UAV for back-buffer format %d", d.Format);
    release(S.out);
    return false;
  }
  ACO_LOG("stereo: eye textures %ux%u fmt %d", d.Width, d.Height, d.Format);
  return true;
}

void read_stats() {
  for (int i = 0; i < 4; ++i) {
    if (!S.stats_frame[i] || S.frames < S.stats_frame[i] + 3) continue;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(S.imm->Map(S.stats_read[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
      const UINT* v = static_cast<const UINT*>(m.pData);
      double px = double(S.w) * S.h;
      ACO_LOG("stereo: L/R difference (frame %llu): mean %.4f/255 per channel, %.3f%% of pixels differ, max %u/255",
              (unsigned long long)S.stats_frame[i], v[0] / (px * 3.0), 100.0 * v[1] / px, v[2]);
      S.imm->Unmap(S.stats_read[i], 0);
      S.stats_frame[i] = 0;
    }
  }
}

void compose(ID3D11Texture2D* bb, bool stats) {
  const Config& cfg = config();
  ComposeParams p{};
  p.w = S.w;
  p.h = S.h;
  p.mode = cfg.stereo_show == "left" ? 1 : cfg.stereo_show == "right" ? 2 : cfg.stereo_show == "diff" ? 3 : 0;
  p.stats = stats ? 1 : 0;
  p.gain = cfg.stereo_diff_gain;
  p.swap = cfg.stereo_swap ? 1 : 0;

  ID3DDeviceContextState* prev = nullptr;
  S.imm1->SwapDeviceContextState(S.own_state, &prev);
  D3D11_MAPPED_SUBRESOURCE m{};
  if (SUCCEEDED(S.imm->Map(S.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
    memcpy(m.pData, &p, sizeof(p));
    S.imm->Unmap(S.cb, 0);
  }
  if (stats) {
    const UINT zero[4] = {};
    S.imm->ClearUnorderedAccessViewUint(S.stats_uav, zero);
  }
  S.imm->CSSetShader(S.compose, nullptr, 0);
  S.imm->CSSetConstantBuffers(0, 1, &S.cb);
  S.imm->CSSetShaderResources(0, 2, S.eye_srv);
  ID3D11UnorderedAccessView* uavs[2] = {S.out_uav, S.stats_uav};
  S.imm->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
  S.imm->Dispatch((S.w + 7) / 8, (S.h + 7) / 8, 1);
  ID3D11UnorderedAccessView* no_uav[2] = {};
  ID3D11ShaderResourceView* no_srv[2] = {};
  S.imm->CSSetUnorderedAccessViews(0, 2, no_uav, nullptr);
  S.imm->CSSetShaderResources(0, 2, no_srv);
  S.imm->CopyResource(bb, S.out);
  if (stats) {
    S.imm->CopyResource(S.stats_read[S.stats_slot], S.stats);
    S.stats_frame[S.stats_slot] = S.frames;
    S.stats_slot = (S.stats_slot + 1) % 4;
  }
  S.imm1->SwapDeviceContextState(prev, nullptr);
  if (prev) prev->Release();
}

double now_ms() {
  LARGE_INTEGER c, f;
  QueryPerformanceCounter(&c);
  QueryPerformanceFrequency(&f);
  return 1000.0 * double(c.QuadPart) / double(f.QuadPart);
}

}  // namespace

void stereo_init(ID3D11Device* dev, ID3D11DeviceContext* imm) {
  if (S.dev == dev) return;
  // The game creates probe devices first; bind to the newest (as the upscaler does).
  release(S.own_state);
  release(S.dev1);
  release(S.imm1);
  S.dev = dev;
  S.imm = imm;
  dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&S.dev1));
  imm->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&S.imm1));
  D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
  if (S.dev1)
    S.dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &S.own_state);
  rec_install(imm);
  cam_init(dev, imm);
  tmp_init(dev, imm);
}

bool stereo_active() { return S.active; }
bool stereo_wide(IDXGISwapChain* sc) { return sc && sc == S.wide_sc; }
int stereo_eye() { return g_eye.load(std::memory_order_relaxed); }

// ---- swapchain layout --------------------------------------------------------------------------

UINT stereo_swapchain_scale() {
  const Config& cfg = config();
  if (cfg.stereo_mode == "off") return 1;
  // mode=auto: decide now (before Frame Generation sets up its back buffer at the first GetBuffer)
  // whether this session may show stereo.
  if (cfg.stereo_mode == "auto") {
    int ds = hypr_display_stereo_now();
    ACO_LOG("stereo: mode=auto, display at swapchain creation: %s", ds == 1 ? "stereo" : ds == 0 ? "mono" : "unknown");
    if (ds != 1) return 1;
    config_block_fg();
  }
  // auto = hsbs under DXVK: a 2x swapchain needs Wine to allow a Vulkan swapchain larger than the
  // window AND the D3D layer to create it at the buffer size; DXVK follows the surface's
  // currentExtent (the window) and would scale the 2x buffer down to the window (a filtered half
  // side by side). See docs/STEREO.md.
  return cfg.stereo_output == "sbs" ? 2 : 1;
}

void stereo_swapchain_created(IDXGISwapChain* sc, UINT game_w, UINT game_h) {
  release(S.proxy);
  S.wide_sc = sc;
  S.game_w = game_w;
  S.game_h = game_h;
  ACO_LOG("stereo: full side-by-side swapchain %ux%u (game sees %ux%u)", game_w * 2, game_h, game_w, game_h);
}

void stereo_swapchain_resized(IDXGISwapChain* sc, UINT game_w, UINT game_h) {
  if (sc != S.wide_sc) return;
  release(S.proxy);
  S.game_w = game_w;
  S.game_h = game_h;
  ACO_LOG("stereo: swapchain resized, game sees %ux%u", game_w, game_h);
}

void stereo_adjust_desc(IDXGISwapChain* sc, UINT* width) {
  if (sc == S.wide_sc && width && *width) *width /= 2;
}

bool stereo_get_buffer(IDXGISwapChain* sc, UINT index, REFIID riid, void** out, PFN_GetBuffer real) {
  if (sc != S.wide_sc || index != 0) return false;
  if (!S.proxy) {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(real(sc, 0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return false;
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    ID3D11Device* dev = nullptr;
    bb->GetDevice(&dev);
    bb->Release();
    d.Width = S.game_w ? S.game_w : d.Width / 2;
    d.Height = S.game_h ? S.game_h : d.Height;
    d.BindFlags |= D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = 0;
    d.CPUAccessFlags = 0;
    d.Usage = D3D11_USAGE_DEFAULT;
    HRESULT hr = dev ? dev->CreateTexture2D(&d, nullptr, &S.proxy) : E_FAIL;
    if (dev) dev->Release();
    ACO_LOG("stereo: game back buffer %ux%u fmt %d: 0x%08lx", d.Width, d.Height, d.Format, hr);
    if (FAILED(hr)) return false;
  }
  return SUCCEEDED(S.proxy->QueryInterface(riid, out));
}

namespace {

void set_tag(const char* tag) {
  // Quiver's HUD/Sight: the packing of what is presented.
  // With [stereo] mode=off Pharos says nothing (Quiver's own sight.stereo setting applies).
  if (config().stereo_mode != "off")
    quiver_stereo_layout(!strcmp(tag, "stereo-sbs") ? "sbs" : !strcmp(tag, "stereo-hsbs") ? "hsbs" : "mono");
  if (S.tag == tag) return;
  S.tag = tag;
  hypr_set_window_tag(tag);
}

// output = sbs: each eye into its half of the real back buffer (or the game's image into both).
void compose_wide(IDXGISwapChain* sc, ID3D11Texture2D* left, ID3D11Texture2D* right) {
  ID3D11Texture2D* bb = nullptr;
  if (FAILED(S.real_get_buffer(sc, 0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return;
  D3D11_BOX box{0, 0, 0, S.game_w, S.game_h, 1};
  S.imm->CopySubresourceRegion(bb, 0, 0, 0, 0, left, 0, &box);
  S.imm->CopySubresourceRegion(bb, 0, S.game_w, 0, 0, right, 0, &box);
  bb->Release();
}

}  // namespace

void stereo_on_present(IDXGISwapChain* sc, PFN_GetBuffer real_get_buffer) {
  const Config& cfg = config();
  rig::poll();
  S.real_get_buffer = real_get_buffer;
  if (cfg.stereo_mode != "off") hypr_start();
  bool display_stereo = hypr_display_stereo() == 1;
  bool stereo_display = cfg.stereo_mode == "sbs" || (cfg.stereo_mode == "auto" && display_stereo);
  bool want = (rig::armed() || stereo_display) && S.dev && S.imm1 && !S.failed && !fg12_proxy_on();
  bool wide = sc == S.wide_sc;
  RecFrame* f = rec_stop(rig::armed());
  TemporalPresentScope compositor;
  cam_on_present(want);
  if (!want) {
    if (rig::armed()) rig::abort_capture("D3D11 replay backend unavailable");
    marker_history_reset();
    markers_reset_aim_depth();
    cursor_set_active(false);
    tmp_frame_end(false, nullptr, 0);
    rec_free(f);
    if (S.active) ACO_LOG("stereo: off");
    S.active = false;
    S.hold_frames = 0;
    if (wide && S.proxy) compose_wide(sc, S.proxy, S.proxy);  // 2D: the same image in both halves
    set_tag(wide ? "stereo-sbs" : "");
    return;
  }
  if (!S.active)
    ACO_LOG("stereo: on (separation %.1f mm, convergence %.2f m, output %s)", cfg.stereo_separation,
            cfg.stereo_convergence, wide ? "sbs" : "hsbs");
  S.active = true;
  set_tag(wide ? "stereo-sbs" : "stereo-hsbs");
  ++S.frames;
  read_stats();

  const char* why = nullptr;
  ID3D11Texture2D* bb = nullptr;  // the image the game drew
  bool got = false;
  if (wide) {
    if (S.proxy) {
      bb = S.proxy;
      bb->AddRef();
      got = true;
    }
  } else {
    got = SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)));
  }
  bool ready = got && ensure_device_objects() && ensure_targets(bb);
  bool replayable = rec_frame_ok(f, &why) && ready;
  if (rig::armed()) {
    bool saved = false;
    rig::capture(
        S.imm,
        [&] {
          if (!saved) {
            S.imm->CopyResource(S.eye[0], bb);
            tmp_rig_history(S.imm, true);
            saved = true;
          } else
            tmp_rig_history(S.imm, false);
          g_eye.store(1, std::memory_order_relaxed);
          rec_replay(f, S.imm);
          g_eye.store(0, std::memory_order_relaxed);
        },
        replayable ? nullptr : (why ? why : "stereo resources unavailable"));
    if (saved) {
      tmp_rig_history(S.imm, false);
      S.imm->CopyResource(bb, S.eye[0]);
    }
    if (!stereo_display) {
      if (wide && bb) compose_wide(sc, bb, bb);
      tmp_frame_end(true, bb, rec_frame_size(f));
      if (bb) bb->Release();
      rec_free(f);
      rec_start();
      return;
    }
  }
  if (replayable) {
    S.hold_frames = 3;
    cursor_set_active(cfg.stereo_cursor == "depth" && S.eye_uav[0] && S.eye_uav[1] && cursor_ready(S.dev));
    double t0 = now_ms();
    S.imm->CopyResource(S.eye[0], bb);
    const bool right_reference = cam_right_aim();
    markers_save_aim_depth(S.imm, 0);
    if (!right_reference) marker_history_filter(S.imm1, S.own_state);
    cursor_save_left(S.imm);
    tmp_before_right(S.imm);
    double t_copy = now_ms();
    g_eye.store(1, std::memory_order_relaxed);
    unsigned skip = 0;
    const std::string& k = cfg.stereo_skip;
    if (k.find("draw") != std::string::npos) skip |= 1u << unsigned(RecKind::Draw);
    if (k.find("dispatch") != std::string::npos) skip |= 1u << unsigned(RecKind::Dispatch);
    if (k.find("copy") != std::string::npos) skip |= 1u << unsigned(RecKind::Copy);
    if (k.find("clear") != std::string::npos) skip |= 1u << unsigned(RecKind::Clear);
    if (k.find("upload") != std::string::npos) skip |= 1u << unsigned(RecKind::Upload);
    if (k.find("execute") != std::string::npos) skip |= 1u << unsigned(RecKind::Execute);
    rec_replay(f, S.imm, skip);
    g_eye.store(0, std::memory_order_relaxed);
    S.imm->CopyResource(S.eye[1], bb);
    markers_save_aim_depth(S.imm, 1);
    if (right_reference) marker_history_filter(S.imm1, S.own_state);
    tmp_after_right(S.imm);
    marker_history_publish(S.imm);
    if (cfg.stereo_cursor == "depth") {
      cursor_begin_frame(S.imm, sc, S.w, S.h);
      if (cursor_visible()) {
        ID3DDeviceContextState* prev = nullptr;
        S.imm1->SwapDeviceContextState(S.own_state, &prev);
        cursor_draw(S.dev, S.imm, S.eye_uav, S.w, S.h);
        S.imm1->SwapDeviceContextState(prev, nullptr);
        if (prev) prev->Release();
      }
    }
    double t_replay = now_ms();
    if (wide) {
      bool sw = cfg.stereo_swap;
      compose_wide(sc, S.eye[sw ? 1 : 0], S.eye[sw ? 0 : 1]);
    } else {
      compose(bb, S.frames % 30 == 0);
    }
    S.replay_ms += now_ms() - t0;
    if (S.frames <= 3 || S.frames % 300 == 0)
      ACO_LOG("stereo: frame %llu: copy %.2f ms, replay %.2f ms, compose %.2f ms (%zu calls)", (unsigned long long)S.frames,
              t_copy - t0, t_replay - t_copy, now_ms() - t_replay, rec_frame_size(f));
    ++S.replays;
    S.cmds_sum += rec_frame_size(f);
    S.bytes_sum += rec_frame_bytes(f);
  } else {
    marker_history_reset();
    markers_reset_aim_depth();
    cursor_set_active(false);
    if (why && why != S.last_bad) {
      ACO_LOG("stereo: frame not replayed: %s", why);
      S.last_bad = why;
    }
    // Streaming can leave a Map open at Present. Keep a short coherent pair through
    // that gap. Longer interruptions get live mono in both halves.
    if (ready) {
      if (S.hold_frames) {
        --S.hold_frames;
        ++S.held;
      } else {
        ++S.mono;
        S.imm->CopyResource(S.eye[0], bb);
        S.imm->CopyResource(S.eye[1], bb);
      }
      if (wide) compose_wide(sc, S.eye[cfg.stereo_swap], S.eye[!cfg.stereo_swap]);
      else compose(bb, false);
    } else if (wide && S.proxy) compose_wide(sc, S.proxy, S.proxy);
  }
  const uint64_t frame = upscaler_frame();
  if (ready && bloom_probe_wants(frame)) {
    ID3DDeviceContextState* prev = nullptr;
    S.imm1->SwapDeviceContextState(S.own_state, &prev);
    bloom_probe_sample(S.dev, S.imm, frame, kBloomFinishedEyes, S.eye_srv[0], S.eye_srv[1]);
    S.imm1->SwapDeviceContextState(prev, nullptr);
    if (prev) prev->Release();
  }
  tmp_frame_end(true, bb, rec_frame_size(f));
  if (bb) bb->Release();
  if (S.frames <= 3 || S.frames % 300 == 0) rec_log_uploads(f);
  rec_free(f);
  if (S.frames % 300 == 0) {
    const double replays = S.replays ? double(S.replays) : 1.0;
    ACO_LOG("stereo: %llu/300 frames replayed (%u held pairs, %u mono), %.0f calls and %.2f MB uploads per frame, replay %.2f ms CPU",
            (unsigned long long)S.replays, S.held, S.mono, double(S.cmds_sum) / replays, double(S.bytes_sum) / replays / 1048576.0,
            S.replay_ms / replays);
    S.held = S.mono = 0;
    S.replays = 0;
    S.cmds_sum = S.bytes_sum = 0;
    S.replay_ms = 0;
  }
  rec_start();
}

}  // namespace aco
