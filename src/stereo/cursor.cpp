#include "stereo/cursor.h"

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <wrl/client.h>

#include "core/config.h"
#include "core/d3d11_title.h"
#include "core/log.h"
#include "core/shaders.h"
#include "dlss/kernels.h"
#include "dlss/upscaler.h"
#include "stereo/camera.h"
#include "stereo/system_cursor.h"

namespace aco {

namespace {

// The interactive sprites use different shaders in each game.
constexpr unsigned long long kCursorVS = title::origins ? 0x12b61097a3a0b0e7ULL : 0x76b57ce1598793e5ULL;
constexpr unsigned long long kCursorPS = title::origins ? 0x068233d2886bf2c8ULL : 0x97091288f72ad57cULL;

const char kCursorHlsl[] = R"(
cbuffer P : register(b0) { float2 Centre; float Radius; float Pad; uint2 Origin; uint2 Size; }
RWTexture2D<float4> O : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    uint2 px = Origin + id;
    if (px.x >= Size.x || px.y >= Size.y) return;
    float d = length(float2(px) + 0.5 - Centre);
    if (d > Radius + 3.0 && d > 4.5) return;
    float4 c = O[px];
    float aBlack = (1.0 - smoothstep(Radius + 1.6, Radius + 2.6, d)) * smoothstep(Radius - 3.4, Radius - 2.4, d);
    float aWhite = (1.0 - smoothstep(Radius - 0.4, Radius + 0.6, d)) * smoothstep(Radius - 2.2, Radius - 1.2, d);
    float aDotB = 1.0 - smoothstep(2.4, 3.4, d);
    float aDotW = 1.0 - smoothstep(1.2, 2.2, d);
    c.rgb = lerp(c.rgb, float3(0, 0, 0), saturate(aBlack) * 0.92);
    c.rgb = lerp(c.rgb, float3(1, 1, 1), saturate(aWhite));
    c.rgb = lerp(c.rgb, float3(0, 0, 0), saturate(aDotB) * 0.92);
    c.rgb = lerp(c.rgb, float3(1, 1, 1), saturate(aDotW));
    c.a = 1.0;
    O[px] = c;
}
)";

struct CursorParams {
  float centre[2];
  float radius;
  float pad;  // HLSL pads Origin to a 16-byte boundary
  unsigned origin[2];
  unsigned size[2];
};

template <typename T>
void release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

std::atomic<bool> g_flat_menu{false};
std::atomic<bool> g_on{false};
std::atomic<bool> g_hidden{false};   // a cursor draw was dropped since the last begin_frame
std::atomic<bool> g_logged_hide{false};

bool pair_bound(ID3D11DeviceContext* ctx) {
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ctx->VSGetShader(&vs, nullptr, nullptr);
  ctx->PSGetShader(&ps, nullptr, nullptr);
  const auto vertex = vs ? shader_hash(vs) : 0;
  const auto pixel = ps ? shader_hash(ps) : 0;
  if constexpr (!title::origins) {
    // Odyssey's main-menu background. Its buttons and pointer share the HUD plane.
    if (vertex == 0x275b56c336f2b40bULL && pixel == 0xafc3de8b425b3010ULL) cursor_note_flat_menu();
  }
  bool hit = vertex == kCursorVS && pixel == kCursorPS;
  if (vs) vs->Release();
  if (ps) ps->Release();
  return hit;
}

using Microsoft::WRL::ComPtr;
enum class Batch { Other, Mouse, Pad, PadDot };
std::atomic<bool> g_pad{false};
std::mutex g_atlas_mutex;
ComPtr<ID3D11ShaderResourceView> g_pad_atlas;

// r25: the pad cursor ends with the mouse batch followed by the ring atlas at t70.
// Earlier draws use this same atlas at t71, then the smaller centre-dot mask.
Batch classify(ID3D11DeviceContext* ctx, ID3D11Buffer* args, UINT off) {
  struct Sequence {
    ID3D11DeviceContext* ctx{};
    ID3D11Buffer* args{};
    UINT off{};
    Batch kind{};
  };
  static thread_local Sequence prev;
  ComPtr<ID3D11ShaderResourceView> pages[2];
  ID3D11ShaderResourceView* raw[2]{};
  ctx->PSGetShaderResources(title::origins ? 70 : 69, 2, raw);
  D3D11_TEXTURE2D_DESC d[2]{};
  for (int i = 0; i < 2; ++i) {
    pages[i].Attach(raw[i]);
    if (!pages[i]) return Batch::Other;
    ComPtr<ID3D11Resource> r;
    pages[i]->GetResource(&r);
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(r.As(&t))) return Batch::Other;
    t->GetDesc(&d[i]);
  }
  auto page = [&](int i, UINT size, DXGI_FORMAT fmt) {
    return d[i].Width == size && d[i].Height == size && d[i].Format == fmt;
  };
  if constexpr (!title::origins) {
    // In Odyssey's pad map pass the first sprite is the animated pointer.
    // The 256x256 BC4 sprite in the world pass is a map location instead.
    return off == 0 && page(0, 4, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) &&
           page(1, 256, DXGI_FORMAT_BC7_UNORM) ? Batch::PadDot : Batch::Other;
  }
  bool next = prev.ctx == ctx && prev.args == args && off == prev.off + 20;
  Batch result = Batch::Other;
  if (page(0, 64, DXGI_FORMAT_BC3_UNORM) && page(1, 512, DXGI_FORMAT_BC4_UNORM)) result = Batch::Mouse;
  else {
    std::lock_guard lock(g_atlas_mutex);
    if (next && prev.kind == Batch::Mouse && page(0, 256, DXGI_FORMAT_BC4_UNORM) && page(1, 512, DXGI_FORMAT_BC4_UNORM)) {
      g_pad_atlas = pages[0];
      g_pad.store(true);
      result = Batch::Pad;
    } else if (page(0, 16, DXGI_FORMAT_BC1_UNORM)) {
      if (pages[1].Get() == g_pad_atlas.Get()) result = Batch::Pad;
      else if (next && prev.kind == Batch::Pad && page(1, 64, DXGI_FORMAT_BC4_UNORM)) result = Batch::PadDot;
    }
  }
  prev = {ctx, args, off, result};
  return result;
}

const char kPadHlsl[] = R"(
struct Centre { float2 ndc : TEXCOORD0; };
[maxvertexcount(1)]
void main(triangle float4 p[3] : SV_Position, inout PointStream<Centre> points) {
  float2 a=p[0].xy/p[0].w, b=p[1].xy/p[1].w, c=p[2].xy/p[2].w;
  Centre v;
  v.ndc=(min(a,min(b,c))+max(a,max(b,c)))*0.5;
  points.Append(v);
}
)";
void note_hidden() {
  g_hidden.store(true, std::memory_order_relaxed);
  if (!g_logged_hide.exchange(true, std::memory_order_relaxed)) {
    ACO_LOG("stereo: game cursor draw hidden (VS %016llx PS %016llx)", kCursorVS, kCursorPS);
  }
}

struct State {
  ID3D11ComputeShader* cs = nullptr;
  ID3D11Buffer* cb = nullptr;
  ID3D11GeometryShader* pad_gs = nullptr;
  ID3D11Buffer* pad_point = nullptr;
  ID3D11Buffer* pad_left = nullptr;
  ID3D11Buffer* pad_stage[3] = {};
  unsigned pad_slot = 0;
  bool pad_valid = false;
  unsigned pad_samples = 0;
  float pad_xy[2] = {};
  bool failed = false;
  HWND hwnd = nullptr;
  // Depth readback ring: issue into slot s, read slot s^1 (one frame old).
  ID3D11Texture2D* stage[2] = {};
  DXGI_FORMAT stage_fmt[2] = {};
  int slot = 0;
  bool pending[2] = {};
  bool pend_ok[2] = {};  // pend_c2v valid
  float pend_ndc[2][2] = {};
  float pend_c2v[2][4][4] = {};
  // Per-frame results.
  bool visible = false;
  float mono[2] = {};        // cursor centre, eye-texture pixels
  float centre[2][2] = {};   // per eye
  float inv_z = 0.0f;        // smoothed 1/Z at the cursor (0 = at infinity / unknown)
  float last_z = 0.0f;
  float dbg_m00 = 0.0f, dbg_d = -1.0f;
  int dbg_fg = -1;           // last upscaler_fg_inputs result in begin_frame
  bool dbg_logged_draw = false;
  unsigned long long frames = 0;
} S;

bool ensure_objects(ID3D11Device* dev) {
  if (S.failed || !dev) return false;
  if (S.cs) return true;
  S.cs = kernels::compile(dev, kCursorHlsl, sizeof(kCursorHlsl) - 1, "aco_cursor.hlsl");
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = sizeof(CursorParams);
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  dev->CreateBuffer(&bd, nullptr, &S.cb);
  ComPtr<ID3DBlob> code;
  code.Attach(kernels::compile_code(kPadHlsl, sizeof(kPadHlsl)-1, "pharos_pad.hlsl", "gs_5_0"));
  const D3D11_SO_DECLARATION_ENTRY entry{0, "TEXCOORD", 0, 0, 2, 0};
  const UINT stride = 8;
  if (code) dev->CreateGeometryShaderWithStreamOutput(code->GetBufferPointer(), code->GetBufferSize(),
      &entry, 1, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &S.pad_gs);
  bd = {};
  bd.ByteWidth = 16;  // one point for each triangle in the centre-dot quad
  bd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
  dev->CreateBuffer(&bd, nullptr, &S.pad_point);
  if constexpr (!title::origins) {
    bd.BindFlags = 0;
    dev->CreateBuffer(&bd, nullptr, &S.pad_left);
    bd.ByteWidth *= 2;
  }
  bd.Usage = D3D11_USAGE_STAGING;
  bd.BindFlags = 0;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  for (auto& stage : S.pad_stage) dev->CreateBuffer(&bd, nullptr, &stage);
  if ((!title::origins && !S.pad_left) || !S.cs || !S.cb || !S.pad_point || !S.pad_gs || !S.pad_stage[0] || !S.pad_stage[1] || !S.pad_stage[2]) {
    ACO_LOG("stereo: cursor objects failed (cs=%p cb=%p); cursor=depth off", static_cast<void*>(S.cs),
            static_cast<void*>(S.cb));
    S.failed = true;
    return false;
  }
  return true;
}

bool cursor_pos(IDXGISwapChain* sc, unsigned w, unsigned h, float out[2]) {
  if (!S.hwnd && sc) {
    DXGI_SWAP_CHAIN_DESC d{};
    if (SUCCEEDED(sc->GetDesc(&d))) S.hwnd = d.OutputWindow;
  }
  if (!S.hwnd) return false;
  POINT p{};
  if (!GetCursorPos(&p) || !ScreenToClient(S.hwnd, &p)) return false;
  RECT r{};
  if (!GetClientRect(S.hwnd, &r) || r.right <= 0 || r.bottom <= 0) return false;
  if (p.x < 0 || p.y < 0 || p.x >= r.right || p.y >= r.bottom) return false;
  out[0] = float(p.x) * float(w) / float(r.right);
  out[1] = float(p.y) * float(h) / float(r.bottom);
  return true;
}

// View-space distance (m) of device depth d at NDC (x, y), via the frame's ClipXYZToViewPos.
// Returns 0 for infinity/invalid (reversed-Z: d==0 is the infinite far plane).
float view_z(const float m[4][4], float ndc_x, float ndc_y, float d) {
  if (!(d > 0.0f) || d > 1.0f) return 0.0f;
  float v[4] = {};
  const float clip[4] = {ndc_x, ndc_y, d, 1.0f};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) v[i] += m[i][j] * clip[j];
  if (v[3] == 0.0f) return 0.0f;
  float z = -(v[2] / v[3]);  // view forward is -z
  return (std::isfinite(z) && z > 0.05f) ? z : 0.0f;
}

void read_pending(ID3D11DeviceContext* imm) {
  int s = S.slot ^ 1;
  if (!S.pending[s] || !S.stage[s]) return;
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(imm->Map(S.stage[s], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) return;
  float d = *static_cast<const float*>(m.pData);
  S.dbg_d = d;
  imm->Unmap(S.stage[s], 0);
  S.pending[s] = false;
  float z = S.pend_ok[s] ? view_z(S.pend_c2v[s], S.pend_ndc[s][0], S.pend_ndc[s][1], d) : 0.0f;
  S.last_z = z;
  float target = z > 0.0f ? 1.0f / z : 0.0f;
  S.inv_z += (target - S.inv_z) * 0.35f;  // smooth in disparity space
}

void issue_depth_sample(ID3D11DeviceContext* imm, ID3D11Device* dev, unsigned w, unsigned h) {
  FgInputs in{};
  if (!upscaler_fg_inputs(&in) || !in.depth || !in.render_w || !in.render_h) return;
  ID3D11Texture2D* tex = nullptr;
  if (FAILED(in.depth->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
    in.depth->Release();
    if (in.mv) in.mv->Release();
    if (in.hudless) in.hudless->Release();
    return;
  }
  D3D11_TEXTURE2D_DESC d;
  tex->GetDesc(&d);
  int s = S.slot;
  if (!S.stage[s] || S.stage_fmt[s] != d.Format) {
    release(S.stage[s]);
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = sd.Height = 1;
    sd.MipLevels = sd.ArraySize = 1;
    sd.Format = d.Format;
    sd.SampleDesc = {1, 0};
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    dev->CreateTexture2D(&sd, nullptr, &S.stage[s]);
    S.stage_fmt[s] = d.Format;
    S.pending[s] = false;
  }
  if (S.stage[s]) {
    unsigned x = unsigned(S.mono[0] / float(w) * float(in.render_w));
    unsigned y = unsigned(S.mono[1] / float(h) * float(in.render_h));
    if (x >= in.render_w) x = in.render_w - 1;
    if (y >= in.render_h) y = in.render_h - 1;
    D3D11_BOX box{x, y, 0, x + 1, y + 1, 1};
    imm->CopySubresourceRegion(S.stage[s], 0, 0, 0, 0, tex, 0, &box);
    S.pend_ndc[s][0] = 2.0f * (float(x) + 0.5f) / float(in.render_w) - 1.0f;
    S.pend_ndc[s][1] = 1.0f - 2.0f * (float(y) + 0.5f) / float(in.render_h);
    float m00tmp = 0.0f;
    S.pend_ok[s] = cam_cursor_view(&m00tmp, S.pend_c2v[s]);
    S.pending[s] = true;
    S.slot ^= 1;
  }
  tex->Release();
  in.depth->Release();
  if (in.mv) in.mv->Release();
  if (in.hudless) in.hudless->Release();
}

} // namespace

void cursor_note_flat_menu() { g_flat_menu.store(true, std::memory_order_relaxed); }

void cursor_set_active(bool on) {
  if (g_on.exchange(on, std::memory_order_acq_rel) != on && on)
    ACO_LOG("stereo: cursor=depth on (game arrow hidden, substitute at scene depth)");
  if (!on) {
    if constexpr (!title::origins) stereo_system_cursor(nullptr);
    g_flat_menu.store(false);
    S.visible = false;
    g_hidden.store(false, std::memory_order_relaxed);
    S.pending[0] = S.pending[1] = false;
    S.inv_z = 0;
  }
}

CursorDraw cursor_prepare(ID3D11DeviceContext* ctx, ID3D11Buffer* args, UINT off) {
  if (!g_on.load(std::memory_order_acquire) || !pair_bound(ctx)) return CursorDraw::Game;
  Batch batch = classify(ctx, args, off);
  if (batch == Batch::Mouse) {
    note_hidden();
    return CursorDraw::Hide;
  }
  if (batch == Batch::Pad) return CursorDraw::Hide;
  if (batch != Batch::PadDot) return CursorDraw::Game;
  ComPtr<ID3D11GeometryShader> gs;
  ctx->GSGetShader(&gs, nullptr, nullptr);
  if (gs) return CursorDraw::Game;
  ID3D11Buffer* bound[4]{};
  ctx->SOGetTargets(4, bound);
  bool occupied = false;
  for (auto* buf : bound) if (buf) { occupied = true; buf->Release(); }
  if (occupied) return CursorDraw::Game;
  if constexpr (!title::origins) {
    g_pad.store(true);
    note_hidden();
  }
  UINT offset = 0;
  ctx->GSSetShader(S.pad_gs, nullptr, 0);
  ctx->SOSetTargets(1, &S.pad_point, &offset);
  return CursorDraw::Capture;
}

void cursor_restore(ID3D11DeviceContext* ctx) {
  ctx->SOSetTargets(0, nullptr, nullptr);
  ctx->GSSetShader(nullptr, nullptr, 0);
}

void cursor_save_left(ID3D11DeviceContext* imm) {
  if constexpr (!title::origins) {
    if (g_pad.load() && S.pad_left) imm->CopyResource(S.pad_left, S.pad_point);
  }
}

bool pad_position(ID3D11DeviceContext* imm, unsigned w, unsigned h, float out[2]) {
  unsigned slot = S.pad_slot;
  if constexpr (title::origins) {
    imm->CopyResource(S.pad_stage[slot], S.pad_point);
  } else {
    imm->CopySubresourceRegion(S.pad_stage[slot], 0, 0, 0, 0, S.pad_left, 0, nullptr);
    imm->CopySubresourceRegion(S.pad_stage[slot], 0, 16, 0, 0, S.pad_point, 0, nullptr);
  }
  S.pad_slot = (slot+1)%3;
  if (S.pad_samples < 2) { ++S.pad_samples; return false; }
  D3D11_MAPPED_SUBRESOURCE m{};
  if (SUCCEEDED(imm->Map(S.pad_stage[S.pad_slot], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
    const float* xy = static_cast<const float*>(m.pData);
    // The map projects the native ring once per eye. Sample depth at its
    // binocular midpoint before applying the terrain's own disparity.
    float x = title::origins ? xy[0] : (xy[0] + xy[4]) * 0.5f;
    float y = title::origins ? xy[1] : (xy[1] + xy[5]) * 0.5f;
    S.pad_xy[0] = (x + 1)*0.5f;
    S.pad_xy[1] = (1 - y)*0.5f;
    S.pad_valid = S.pad_xy[0] >= 0 && S.pad_xy[0] <= 1 && S.pad_xy[1] >= 0 && S.pad_xy[1] <= 1;
    imm->Unmap(S.pad_stage[S.pad_slot], 0);
  }
  out[0] = S.pad_xy[0]*w;
  out[1] = S.pad_xy[1]*h;
  return S.pad_valid;
}

void cursor_begin_frame(ID3D11DeviceContext* imm, IDXGISwapChain* sc, unsigned w, unsigned h) {
  ++S.frames;
  bool flat_menu = g_flat_menu.exchange(false);
  bool hidden = g_hidden.exchange(false, std::memory_order_relaxed);
  float pos[2] = {};
  bool pad = g_pad.exchange(false);
  bool mouse = false;
  if constexpr (!title::origins) {
    bool inside = !pad && cursor_pos(sc, w, h, pos);
    mouse = stereo_system_cursor(inside ? S.hwnd : nullptr);
  }
  if (!pad) {
    S.pad_samples = 0;
    S.pad_valid = false;
  }
  S.visible = mouse || (hidden && (pad ? pad_position(imm, w, h, pos) : cursor_pos(sc, w, h, pos)));
  if (!S.visible) return;
  if (flat_menu) {
    for (auto& centre : S.centre) { centre[0] = pos[0]; centre[1] = pos[1]; }
    S.pending[0] = S.pending[1] = false;
    if (S.frames % 300 == 0) ACO_LOG("stereo: menu cursor at (%.0f, %.0f), screen depth", pos[0], pos[1]);
    return;
  }
  S.mono[0] = pos[0];
  S.mono[1] = pos[1];

  const Config& cfg = config();
  ID3D11Device* dev = nullptr;
  imm->GetDevice(&dev);
  read_pending(imm);
  if (dev) issue_depth_sample(imm, dev, w, h);

  // Per-eye NDC shift of a point at the cursor's scene depth, the same model as
  // camera.cpp eye_params: dx = m00 * dv * (1/convergence - 1/Z), dv = -/+ separation.
  // (The FG camera copy can't be used in stereo: its PassParams reads hit our redirected
  // pool buffers. The camera system's own last main view is the source instead.)
  float m00 = 0.0f, c2v[4][4];
  S.dbg_fg = cam_cursor_view(&m00, c2v) ? 1 : 0;
  float sep = cfg.stereo_separation * 0.001f;
  float inv_conv = cfg.stereo_convergence > 0.0f ? 1.0f / cfg.stereo_convergence : 0.0f;
  for (int e = 0; e < 2; ++e) {
    float dv = (e == 0 ? -sep : sep) * (m00 < 0 ? -1.0f : 1.0f);
    float dx = m00 * dv * (inv_conv - S.inv_z) * (float(w) * 0.5f);
    S.centre[e][0] = S.mono[0] + dx;
    S.centre[e][1] = S.mono[1];
  }
  if (dev) dev->Release();
  S.dbg_m00 = m00;
  if (S.frames % 300 == 0)
    ACO_LOG("stereo: cursor at (%.0f, %.0f), scene %.1f m, eye centres (%.1f, %.1f) [fg=%d m00=%.3f d=%.6f]",
            S.mono[0], S.mono[1], S.last_z, S.centre[0][0], S.centre[1][0], S.dbg_fg, S.dbg_m00, S.dbg_d);
}

bool cursor_ready(ID3D11Device* dev) { return ensure_objects(dev); }

bool cursor_visible() { return S.visible; }

void cursor_draw(ID3D11Device* dev, ID3D11DeviceContext* imm, ID3D11UnorderedAccessView* eye_uav[2],
                 unsigned w, unsigned h) {
  if (!S.visible || !ensure_objects(dev)) return;
  if (!S.dbg_logged_draw) {
    S.dbg_logged_draw = true;
    ACO_LOG("stereo: cursor draw (cs=%p cb=%p uav=%p/%p)", static_cast<void*>(S.cs), static_cast<void*>(S.cb),
            static_cast<void*>(eye_uav[0]), static_cast<void*>(eye_uav[1]));
  }
  float radius = 11.0f * float(h) / 720.0f;
  if (radius < 6.0f) radius = 6.0f;
  for (int e = 0; e < 2; ++e) {
    if (!eye_uav[e]) continue;
    CursorParams p{};
    p.centre[0] = S.centre[e][0];
    p.centre[1] = S.centre[e][1];
    p.radius = radius;
    p.size[0] = w;
    p.size[1] = h;
    int ox = int(S.centre[e][0] - radius - 4.0f), oy = int(S.centre[e][1] - radius - 4.0f);
    p.origin[0] = unsigned(ox < 0 ? 0 : ox);
    p.origin[1] = unsigned(oy < 0 ? 0 : oy);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(imm->Map(S.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, &p, sizeof(p));
    imm->Unmap(S.cb, 0);
    imm->CSSetShader(S.cs, nullptr, 0);
    imm->CSSetConstantBuffers(0, 1, &S.cb);
    imm->CSSetUnorderedAccessViews(0, 1, &eye_uav[e], nullptr);
    unsigned tw = unsigned(radius * 2.0f) + 9;
    imm->Dispatch((tw + 7) / 8, (tw + 7) / 8, 1);
    ID3D11UnorderedAccessView* no = nullptr;
    imm->CSSetUnorderedAccessViews(0, 1, &no, nullptr);
  }
}

} // namespace aco

