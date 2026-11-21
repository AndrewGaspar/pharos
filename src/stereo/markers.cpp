#include "stereo/markers.h"

#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <wrl/client.h>
#include "core/config.h"
#include "core/log.h"
#include "core/lab.h"
#include "dlss/kernels.h"
#include "stereo/camera.h"
#include "stereo/marker_history.h"

namespace aco {
namespace {
using Microsoft::WRL::ComPtr;
enum Layout { Sprite, Plain, Rich, Level, Logo, PlainLogo, LayoutCount };
ComPtr<ID3D11GeometryShader> shaders[LayoutCount];
ComPtr<ID3D11Buffer> anchors, empty_anchors;
ComPtr<ID3D11ShaderResourceView> anchor_view;
std::atomic<bool> initialized{false}, probe_requested{false};
ComPtr<ID3D11Buffer> probe_buffer;
constexpr UINT kAnchorCount = 1536;  // six vertices per icon
const char source[] = R"(
cbuffer Targets : register(b12) { uint numTargets; float4 targets[192]; }
cbuffer Camera : register(b13) { float4 zRow; float4 wRow; float4 scale; }
Texture2D<float4> History : register(t125);
Buffer<float4> Anchors : register(t126);
Texture2D<float> Depth : register(t127);
struct Vertex {
  float4 p : SV_Position;
  float4 uv : TEXCOORD0;
#ifdef RICH
  float4 local : TEXCOORD1;
  float4 color : COLOR0;
  float4 a : TEXCOORD2; float4 b : TEXCOORD3; float4 c : TEXCOORD4; float4 d : TEXCOORD5;
#else
#ifndef PLAIN
  float4 color : COLOR0;
#endif
  float4 a : TEXCOORD1; float4 b : TEXCOORD2; float4 c : TEXCOORD3; float4 d : TEXCOORD4;
#endif
  uint instance : InstanceID;
};
#ifdef LOGO
struct Recorded { Vertex vertex; float4 anchor : TEXCOORD6; };
#define OUTPUT Recorded
#else
#define OUTPUT Vertex
#endif

float surfaceInvZ(float2 ndc) {
  uint2 size, tiles;
  Depth.GetDimensions(size.x,size.y);
  History.GetDimensions(tiles.x,tiles.y);
  float2 pixel = clamp((ndc*float2(0.5,-0.5)+0.5)*size,0,float2(size)-1);
  uint2 p = uint2(pixel);
  float z = Depth.Load(int3(p,0));
  if (all(tiles > 0)) {
    // Extend the near tile by one pixel across its boundary. Blend adjacent
    // tiles over the next two pixels as the camera moves off a silhouette.
    float2 tile = pixel*0.25-0.5;
    int2 base = int2(floor(tile));
    float2 f = smoothstep(0.25,0.75,frac(tile));
    float4 h;
    h.x = History.Load(int3(clamp(base,0,int2(tiles)-1),0)).x;
    h.y = History.Load(int3(clamp(base+int2(1,0),0,int2(tiles)-1),0)).x;
    h.z = History.Load(int3(clamp(base+int2(0,1),0,int2(tiles)-1),0)).x;
    h.w = History.Load(int3(clamp(base+1,0,int2(tiles)-1),0)).x;
    float filtered = lerp(lerp(h.x,h.y,f.x),lerp(h.z,h.w,f.x),f.y);
    z = max(z,max(filtered,History.Load(int3(p/4,0)).x));
  }
  float4 clip = float4(ndc,z,1);
  return max(0,-dot(wRow,clip)/dot(zRow,clip));
}
[maxvertexcount(3)]
void main(triangle Vertex vs[3], inout TriangleStream<OUTPUT> outStream) {
  float2 a = vs[0].p.xy/vs[0].p.w, b = vs[1].p.xy/vs[1].p.w, c = vs[2].p.xy/vs[2].p.w;
  float2 centre = (min(a,min(b,c)) + max(a,max(b,c)))*0.5;
  float nativeInvZ = 3.0/(vs[0].p.w+vs[1].p.w+vs[2].p.w);
  // The engine compresses even a distant billboard into a roughly 10 m HUD rig.
  // Match its cyclopean bearing to the original world target before testing visibility.
  float2 mono = centre;
  mono.x -= scale.x*(scale.z-nativeInvZ);
  float targetInvZ=0, bestError=1e20;
  bool reticle = scale.w == 2;
  [loop] for (uint t=0; t<numTargets && !reticle; ++t) {
    float4 target=targets[t];
    float2 direction=target.xy;
    if (scale.w == 0) {
      float edge=max(abs(direction.x),abs(direction.y));
      if (edge<0.95) continue;
      direction/=edge;
      // Edge widgets are inset from the actual boundary.
      direction*=max(abs(mono.x),abs(mono.y));
    }
    float2 diff=mono-direction;
    diff.y -= clamp(diff.y,min(0,target.w),max(0,target.w));
    float error=dot(diff,diff);
    if (error<bestError && error<0.045*0.045) { bestError=error; targetInvZ=target.z; }
  }
  float2 ray = mono;
  ray.x += scale.y*(scale.z-targetInvZ);
  float surface = surfaceInvZ(ray);
  if (reticle) {
    uint2 sceneSize, tiles;
    Depth.GetDimensions(sceneSize.x,sceneSize.y);
    History.GetDimensions(tiles.x,tiles.y);
    // Both eyes aim along one ray. The selected Predator eye supplies its native
    // centre pixel, including when the rim of the bow crosses the other eye's view.
    float d = all(tiles>0) ? History.Load(int3((sceneSize/2)/4,0)).z : Depth.Load(int3(sceneSize/2,0));
    float4 clip = float4(0,0,d,1);
    surface = max(0,-dot(wRow,clip)/dot(zRow,clip));
    targetInvZ = 0;
  }
  float invZ = surface > targetInvZ ? surface*1.005 : targetInvZ;
#ifdef RICH
#ifndef LEVEL
  // Use the projected local origin of the whole text mesh so every glyph chooses
  // the same preceding icon, even where the lettering crosses a depth edge.
  float2 u = vs[1].local.xy-vs[0].local.xy, v = vs[2].local.xy-vs[0].local.xy;
  float2 q = -vs[0].local.xy;
  float det = u.x*v.y-u.y*v.x;
  if (abs(det)>1e-10) centre = a+((q.x*v.y-q.y*v.x)*(b-a)+(u.x*q.y-u.y*q.x)*(c-a))/det;
#endif
  uint count;
  Anchors.GetDimensions(count);
  float best = 1e20;
  float2 label = centre;
  label.x -= scale.x*(scale.z-nativeInvZ);
  [loop] for (uint j=0;j<count;j+=3) {
    float4 anchor = Anchors[j];
    if (anchor.w <= 0) break;
    float2 delta = label-anchor.xy;
    float wDifference = (nativeInvZ*anchor.w-1);
    float dist = dot(delta,delta) + min(0.0025,wDifference*wDifference*0.0004);
    if (dist < best) { best=dist; invZ=anchor.z; }
  }
#endif
  [unroll] for (uint i=0;i<3;i++) {
    Vertex v=vs[i];
    if (!reticle || all(abs(mono) < 0.12)) v.p.x += scale.x*(1-v.p.w*scale.z) + scale.y*v.p.w*(scale.z-invZ);
#ifdef LOGO
    Recorded rec;
    rec.vertex=v;
    // The label reader consumes the first record of each triangle. Keep the
    // actual target and hit in the second record for the lab's depth probe.
    rec.anchor = i==1 ? float4(targetInvZ,surface,nativeInvZ,1) : float4(mono,invZ,1/nativeInvZ);
    outStream.Append(rec);
#else
    outStream.Append(v);
#endif
  }
}
)";

D3D11_TEXTURE2D_DESC texture_at(ID3D11DeviceContext* ctx, UINT slot) {
  D3D11_TEXTURE2D_DESC desc{};
  ComPtr<ID3D11ShaderResourceView> srv;
  ctx->PSGetShaderResources(slot, 1, &srv);
  if (!srv) return desc;
  ComPtr<ID3D11Resource> resource;
  ComPtr<ID3D11Texture2D> texture;
  srv->GetResource(&resource);
  if (SUCCEEDED(resource.As(&texture))) texture->GetDesc(&desc);
  return desc;
}

bool page(const D3D11_TEXTURE2D_DESC& d, UINT w, UINT h, DXGI_FORMAT fmt) {
  return d.Width == w && d.Height == h && d.Format == fmt;
}
bool stream_available(ID3D11DeviceContext* ctx) {
  ID3D11Buffer* bound[4]{};
  ctx->SOGetTargets(4, bound);
  bool free = true;
  for (auto* b : bound)
    if (b) {
      free = false;
      b->Release();
    }
  return free;
}
}  // namespace

void markers_init(ID3D11Device* dev) {
  const char* defines[] = {"",
                           "#define PLAIN\n",
                           "#define RICH\n",
                           "#define RICH\n#define LEVEL\n",
                           "#define LOGO\n",
                           "#define LOGO\n#define PLAIN\n"};
  for (int i = 0; i < LayoutCount; ++i) {
    std::string hlsl = std::string(defines[i]) + source;
    ComPtr<ID3DBlob> code;
    code.Attach(kernels::compile_code(hlsl.data(), hlsl.size(), "pharos_marker.hlsl", "gs_5_0"));
    if (!code) continue;
    if (i == Logo || i == PlainLogo) {
      const D3D11_SO_DECLARATION_ENTRY entry{0, "TEXCOORD", 6, 0, 4, 0};
      const UINT stride = 16;
      dev->CreateGeometryShaderWithStreamOutput(code->GetBufferPointer(), code->GetBufferSize(), &entry, 1, &stride, 1,
                                                0, nullptr, &shaders[i]);
    } else {
      dev->CreateGeometryShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shaders[i]);
    }
  }
  D3D11_BUFFER_DESC d{};
  d.ByteWidth = kAnchorCount * 16;
  d.BindFlags = D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_SHADER_RESOURCE;
  if (SUCCEEDED(dev->CreateBuffer(&d, nullptr, &anchors))) {
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    view.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    view.Buffer.NumElements = kAnchorCount;
    dev->CreateShaderResourceView(anchors.Get(), &view, &anchor_view);
    const float zeros[kAnchorCount * 4]{};
    D3D11_SUBRESOURCE_DATA data{zeros, 0, 0};
    d.BindFlags = 0;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    dev->CreateBuffer(&d, &data, &empty_anchors);
  }
  // Capture one labeled logo batch on demand in Quiver's disposable lab.
  if (lab_active() && anchors) {
    d.BindFlags = 0;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(dev->CreateBuffer(&d, nullptr, &probe_buffer))) {
      lab_verb("marker", [dev](const std::vector<std::string>&, bool& ok) {
        static std::mutex probe_lock;
        std::lock_guard guard(probe_lock);
        probe_requested.store(true);
        if (!wait_presents(6, 10000)) {
          probe_requested.store(false);
          return lab_error(ok, "no rendered frame");
        }
        if (probe_requested.exchange(false)) return lab_error(ok, "no labeled world icon");
        std::string result;
        if (!on_present([&] {
              ComPtr<ID3D11DeviceContext> ctx;
              dev->GetImmediateContext(&ctx);
              D3D11_MAPPED_SUBRESOURCE m{};
              if (FAILED(ctx->Map(probe_buffer.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
              float* v = static_cast<float*>(m.pData);
              result = "[";
              for (UINT i = 0; i < kAnchorCount && v[4 * i + 3] > 0; i += 3) {
                if (i && i + 3 < kAnchorCount && std::abs(v[4 * i] - v[4 * (i - 3)]) < .0001f &&
                    std::abs(v[4 * i + 1] - v[4 * (i - 3) + 1]) < .0001f)
                  continue;
                char buf[240];
                snprintf(buf, sizeof(buf),
                         "%s{\"xy\":[%.6f,%.6f],\"chosen_inv_z\":%.6f,\"native_w\":%.6f,\"target_inv_z\":%.6f,"
                         "\"surface_inv_z\":%.6f}",
                         i ? "," : "", v[4 * i], v[4 * i + 1], v[4 * i + 2], v[4 * i + 3], v[4 * i + 4], v[4 * i + 5]);
                result += buf;
              }
              result += "]";
              ctx->Unmap(probe_buffer.Get(), 0);
            }) ||
            result.empty())
          return lab_error(ok, "marker readback unavailable");
        return result;
      });
    }
  }
  initialized.store(true, std::memory_order_release);
}

bool panel_begin(ID3D11DeviceContext* ctx, ShaderClass shader, UINT* caption_offset) {
  if (!g_cam_on.load(std::memory_order_relaxed)) return false;
  ComPtr<ID3D11VertexShader> vs;
  ctx->VSGetShader(&vs, nullptr, nullptr);
  const bool intro = shader == ShaderClass::UiTourIntro;
  const auto hash = shader_hash(vs.Get());
  if (shader == ShaderClass::UiSprite && hash == 0x12b61097a3a0b0e7ULL) {
    return page(texture_at(ctx, 70), 1920, 1080, DXGI_FORMAT_BC1_UNORM) &&
           page(texture_at(ctx, 71), 128, 128, DXGI_FORMAT_BC1_UNORM) && cam_begin_panel(ctx, true, caption_offset);
  }
  if (hash != (intro ? 0x1664ad3d166a9464ULL : 0xcab2d4126b8ca082ULL)) return false;
  const auto art = texture_at(ctx, 70);
  if (intro) {
    return page(art, 64, 64, DXGI_FORMAT_BC5_UNORM) && page(texture_at(ctx, 71), 64, 32, DXGI_FORMAT_BC4_UNORM) &&
           cam_begin_panel(ctx, false, caption_offset);
  }
  if (art.Usage != D3D11_USAGE_IMMUTABLE || art.MipLevels != 1) return false;
  const bool photo = page(art, 1920, 1080, DXGI_FORMAT_BC1_UNORM);
  const bool menu = page(art, 2048, 1024, DXGI_FORMAT_BC1_UNORM) || page(art, 1920, 1080, DXGI_FORMAT_BC3_UNORM);
  return (photo || menu) && !texture_at(ctx, 71).Width && cam_begin_panel(ctx, menu, caption_offset);
}

MarkerDraw markers_begin(ID3D11DeviceContext* ctx, ShaderClass shader) {
  if (!g_cam_on.load(std::memory_order_relaxed) || !config().stereo_markers ||
      !initialized.load(std::memory_order_acquire))
    return MarkerDraw::Game;
  const bool plain = shader == ShaderClass::UiPlain, rich = shader == ShaderClass::UiRichText;
  ComPtr<ID3D11VertexShader> vs;
  ctx->VSGetShader(&vs, nullptr, nullptr);
  const auto hash = shader_hash(vs.Get());
  const bool edge = hash == 0x8495d24fffb60308ULL || hash == 0xd8d8858e469845bcULL;
  const bool sprite = hash == 0x12b61097a3a0b0e7ULL;
  if (plain  ? hash != 0xcab2d4126b8ca082ULL
      : rich ? (hash != 0x6d0fb2ea896797e3ULL && hash != 0xf4b81a7a619bd7b3ULL)
             : !edge && !sprite && hash != 0xb1bcca4dff8812c8ULL)
    return MarkerDraw::Game;
  Layout layout = plain ? Plain : rich ? Rich : Sprite;
  if (hash == 0xf4b81a7a619bd7b3ULL) layout = Level;
  if (!shaders[layout]) return MarkerDraw::Game;
  ComPtr<ID3D11GeometryShader> gs;
  ComPtr<ID3D11ShaderResourceView> depth;
  ComPtr<ID3D11Buffer> cb;
  ctx->GSGetShader(&gs, nullptr, nullptr);
  ctx->GSGetConstantBuffers(12, 1, &cb);
  if (cb) return MarkerDraw::Game;
  ctx->GSGetConstantBuffers(13, 1, &cb);
  ctx->PSGetShaderResources(28, 1, &depth);
  if (gs || cb || !depth) return MarkerDraw::Game;
  ID3D11ShaderResourceView* old[3]{};
  ctx->GSGetShaderResources(125, 3, old);
  bool occupied = false;
  for (auto* v : old)
    if (v) {
      occupied = true;
      v->Release();
    }
  if (occupied) return MarkerDraw::Game;
  const auto atlas = texture_at(ctx, 70);
  const bool bracket = sprite && page(atlas, 64, 64, DXGI_FORMAT_BC3_UNORM);
  const bool reticle =
      plain && (page(atlas, 32, 32, DXGI_FORMAT_BC3_UNORM) || page(atlas, 128, 128, DXGI_FORMAT_BC4_UNORM));
  // Level numerals (f4b81a7a) follow a plain 128x128 shield batch.
  const bool shield = plain && page(atlas, 128, 128, DXGI_FORMAT_BC3_UNORM);
  const bool label_background = shield || bracket || (sprite && page(atlas, 16, 16, DXGI_FORMAT_BC3_UNORM_SRGB));
  // After the first guard, the level font leaves its 512px atlas mask bound
  // through the next shield. Both bindings occur in the same command list.
  const auto mask = label_background ? texture_at(ctx, 71) : D3D11_TEXTURE2D_DESC{};
  const bool logo_mask = page(mask, 128, 128, DXGI_FORMAT_BC1_UNORM) ||
                         (shield && page(mask, 512, 512, DXGI_FORMAT_BC1_UNORM));
  const bool logo = label_background && logo_mask && shaders[plain ? PlainLogo : Logo] && empty_anchors &&
                    anchor_view && stream_available(ctx);
  bool record = false;
  MarkerRole role = MarkerRole::Ordinary;
  if (edge) role = MarkerRole::Edge;
  if (bracket) role = MarkerRole::Bracket;
  if (logo) role = MarkerRole::Logo;
  if (rich) role = MarkerRole::Label;
  if (reticle) role = MarkerRole::Reticle;
  if (!cam_bind_marker(ctx, role, &record)) return MarkerDraw::Game;
  if (record) {
    layout = plain ? PlainLogo : Logo;
    ctx->CopyResource(anchors.Get(), empty_anchors.Get());
    UINT start = 0;
    ctx->SOSetTargets(1, anchors.GetAddressOf(), &start);
  }
  if (rich && probe_requested.exchange(false)) ctx->CopyResource(probe_buffer.Get(), anchors.Get());
  auto history = marker_history_sample(depth.Get());
  ID3D11ShaderResourceView* views[] = {history.Get(), rich ? anchor_view.Get() : nullptr, depth.Get()};
  ctx->GSSetShader(shaders[layout].Get(), nullptr, 0);
  ctx->GSSetShaderResources(125, 3, views);
  return record ? MarkerDraw::RecordLogo : MarkerDraw::Shift;
}

void markers_end(ID3D11DeviceContext* ctx, MarkerDraw draw) {
  if (draw == MarkerDraw::Game) return;
  if (draw == MarkerDraw::RecordLogo) ctx->SOSetTargets(0, nullptr, nullptr);
  ID3D11ShaderResourceView* views[3]{};
  ID3D11Buffer* cb[2]{};
  ctx->GSSetShader(nullptr, nullptr, 0);
  ctx->GSSetShaderResources(125, 3, views);
  ctx->GSSetConstantBuffers(12, 2, cb);
}
}  // namespace aco
