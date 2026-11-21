#include "stereo/markers.h"

#include <atomic>
#include <string>
#include <wrl/client.h>
#include "core/config.h"
#include "core/log.h"
#include "dlss/kernels.h"
#include "stereo/camera.h"

namespace aco {
namespace {
using Microsoft::WRL::ComPtr;
ComPtr<ID3D11GeometryShader> shaders[2];
std::atomic<bool> initialized{false};
const char source[] = R"(
cbuffer Camera : register(b13) { float4 zRow; float4 wRow; float3 scale; }
Texture2D<float> Depth : register(t127);
struct Vertex {
  float4 p : SV_Position; float4 uv : TEXCOORD0;
#ifdef HAS_COLOR
  float4 color : COLOR0;
#endif
  float4 a : TEXCOORD1; float4 b : TEXCOORD2; float4 c : TEXCOORD3;
  float4 d : TEXCOORD4; uint instance : InstanceID;
};
float sampleZ(float2 ndc, uint2 size) {
  int2 pixel = clamp((ndc * float2(0.5,-0.5) + 0.5) * size, 0, float2(size)-1);
  return Depth.Load(int3(pixel,0));
}
[maxvertexcount(3)]
void main(triangle Vertex vs[3], inout TriangleStream<Vertex> outStream) {
  float2 a = vs[0].p.xy/vs[0].p.w, b = vs[1].p.xy/vs[1].p.w, c = vs[2].p.xy/vs[2].p.w;
  float2 lo = min(a,min(b,c)), hi = max(a,max(b,c)), centre = (lo+hi)*0.5;
  uint2 size; Depth.GetDimensions(size.x,size.y);
  // Both triangles of a sprite sample the same rectangle. Reversed Z: largest is nearest.
  float depth = max(sampleZ(centre,size),max(max(sampleZ(lo,size),sampleZ(hi,size)),
                    max(sampleZ(float2(lo.x,hi.y),size),sampleZ(float2(hi.x,lo.y),size))));
  float4 clip = float4(centre,depth,1);
  float invZ = max(0,-dot(wRow,clip)/dot(zRow,clip));
  [unroll] for (uint i=0;i<3;i++) {
    Vertex v=vs[i];
    // Replace the disparity of the game's proxy quad with that of the visible surface.
    v.p.x += scale.x * (1-v.p.w*scale.z) + scale.y * v.p.w * (scale.z-invZ);
    outStream.Append(v);
  }
}
)";

// r30: the brackets following the edge indicator and distance share the ordinary
// sprite VS. The brackets use the 64x64 BC3 page, other HUD decorations do not.
bool bracket_page(ID3D11DeviceContext* ctx) {
  ComPtr<ID3D11ShaderResourceView> srv;
  ctx->PSGetShaderResources(70, 1, &srv);
  if (!srv) return false;
  ComPtr<ID3D11Resource> resource;
  srv->GetResource(&resource);
  ComPtr<ID3D11Texture2D> texture;
  if (FAILED(resource.As(&texture))) return false;
  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);
  return desc.Width == 64 && desc.Height == 64 && desc.Format == DXGI_FORMAT_BC3_UNORM;
}
}  // namespace

void markers_init(ID3D11Device* dev) {
  for (int i = 0; i < 2; ++i) {
    std::string hlsl = i ? source : std::string("#define HAS_COLOR\n") + source;
    ComPtr<ID3DBlob> code;
    code.Attach(kernels::compile_code(hlsl.data(), hlsl.size(), "pharos_marker.hlsl", "gs_5_0"));
    shaders[i].Reset();
    if (code) dev->CreateGeometryShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shaders[i]);
  }
  initialized.store(true, std::memory_order_release);
}

bool markers_begin(ID3D11DeviceContext* ctx, ShaderClass shader) {
  if (!g_cam_on.load(std::memory_order_relaxed) || !config().stereo_markers) return false;
  bool plain = shader == ShaderClass::UiPlain;
  if (!initialized.load(std::memory_order_acquire) || !shaders[plain]) return false;
  ComPtr<ID3D11VertexShader> vs;
  ctx->VSGetShader(&vs, nullptr, nullptr);
  const auto hash = shader_hash(vs.Get());
  // Off-screen indicators and their distance labels use separate vertex layouts
  // in the flat HUD pass. Other sprites sharing their pixel shaders stay in place.
  const bool edge = hash == 0x8495d24fffb60308ULL || hash == 0xd8d8858e469845bcULL;
  if (plain ? hash != 0xcab2d4126b8ca082ULL
            : !edge && hash != 0x12b61097a3a0b0e7ULL && hash != 0xb1bcca4dff8812c8ULL) return false;
  ComPtr<ID3D11GeometryShader> gs;
  ComPtr<ID3D11ShaderResourceView> old, depth;
  ComPtr<ID3D11Buffer> cb;
  ctx->GSGetShader(&gs, nullptr, nullptr);
  ctx->GSGetShaderResources(127, 1, &old);
  ctx->GSGetConstantBuffers(13, 1, &cb);
  ctx->PSGetShaderResources(28, 1, &depth);
  if (gs || old || cb || !depth) return false;
  const bool bracket = hash == 0x12b61097a3a0b0e7ULL && bracket_page(ctx);
  if (!cam_bind_marker(ctx, edge, bracket)) return false;
  static std::atomic<bool> reported[2]{};
  if (!reported[edge].load(std::memory_order_relaxed) && !reported[edge].exchange(true))
    ACO_LOG("stereo: sampling scene depth for %s markers", edge ? "screen-edge" : "world");
  ctx->GSSetShader(shaders[plain].Get(), nullptr, 0);
  ctx->GSSetShaderResources(127, 1, depth.GetAddressOf());
  return true;
}

void markers_end(ID3D11DeviceContext* ctx) {
  ID3D11ShaderResourceView* srv = nullptr;
  ID3D11Buffer* cb = nullptr;
  ctx->GSSetShader(nullptr, nullptr, 0);
  ctx->GSSetShaderResources(127, 1, &srv);
  ctx->GSSetConstantBuffers(13, 1, &cb);
}
}  // namespace aco
