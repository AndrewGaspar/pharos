#include "fg/color.h"
#include <string>

#include "core/ctxhooks.h"
#include "core/hook.h"
#include "core/vtidx.h"
#include "dlss/kernels.h"

namespace aco::fg_color {
using Microsoft::WRL::ComPtr;
namespace {
// DXGI exposes no swapchain color-space getter. Private data follows the chain's
// COM identity and lifetime, including QueryInterface and multiple live chains.
constexpr GUID color_key{0x114f87ad,0xab87,0x4ecd,{0x9f,0x18,0x16,0xd0,0x82,0x05,0x68,0xf3}};

HRESULT STDMETHODCALLTYPE set_space(IDXGISwapChain3* chain, DXGI_COLOR_SPACE_TYPE value) {
  auto hr = orig<decltype(set_space)>(chain, vt::SwapChain3_SetColorSpace1)(chain, value);
  if (SUCCEEDED(hr)) chain->SetPrivateData(color_key, sizeof(value), &value);
  return hr;
}

// ST 2084 absolute luminance, BT.709 <-> BT.2020 with D65 white.
// scRGB 1.0 represents 80 cd/m². Clip only to the representable BT.2020/PQ
// gamut after converting primaries; wide-gamut scRGB can have negative channels.
constexpr char shader[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
static const float m1 = 2610./16384., m2 = 2523./32.;
static const float c1 = 3424./4096., c2 = 2413./128., c3 = 2392./128.;
[numthreads(8,8,1)]
void main(uint2 xy : SV_DispatchThreadID) {
    uint w,h; src.GetDimensions(w,h);
    if (xy.x >= w || xy.y >= h) return;
    float4 v = src.Load(int3(xy,0));
#ifdef ENCODE
    float3 light = mul(float3x3(.627404, .329283, .043313,
                                 .069097, .919540, .011362,
                                 .016391, .088013, .895595), v.rgb);
    float3 n = pow(saturate(light * (80./10000.)), m1);
    dst[xy] = float4(pow((c1+c2*n)/(1.+c3*n), m2), v.a);
#else
    float3 n = pow(saturate(v.rgb), 1./m2);
    float3 light = pow(max(n-c1,0.)/(c2-c3*n), 1./m1) * (10000./80.);
    dst[xy] = float4(mul(float3x3(1.660491,-.587641,-.072850,
                                -.124550,1.132900,-.008349,
                                -.018151,-.100579,1.118730), light), v.a);
#endif
}
)";
} // namespace

void watch(IDXGISwapChain* chain) {
  ComPtr<IDXGISwapChain3> sc3;
  if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&sc3))))
    hook_vtable(sc3.Get(), vt::SwapChain3_SetColorSpace1, reinterpret_cast<void*>(set_space));
}

DXGI_COLOR_SPACE_TYPE space(IDXGISwapChain* chain, DXGI_FORMAT format) {
  DXGI_COLOR_SPACE_TYPE cs;
  UINT bytes = sizeof(cs);
  if (SUCCEEDED(chain->GetPrivateData(color_key, &bytes, &cs)) && bytes == sizeof(cs)) return cs;
  // DXGI defaults to linear scRGB for FP16, sRGB for integer swapchains (including RGB10).
  return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                                 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
}

bool is_hdr(DXGI_COLOR_SPACE_TYPE cs) {
  return cs == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 || cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
         cs == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020;
}

bool needs_pq(DXGI_COLOR_SPACE_TYPE cs, DXGI_FORMAT format) {
  return format == DXGI_FORMAT_R16G16B16A16_FLOAT && cs == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
}

bool supported(DXGI_COLOR_SPACE_TYPE cs, DXGI_FORMAT format) {
  return !is_hdr(cs) || needs_pq(cs, format) ||
         (cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 && format == DXGI_FORMAT_R10G10B10A2_UNORM);
}

bool Converter::record(ID3D11Device* dev, ID3D11Texture2D* src, ID3D11Texture2D* dst, bool encode,
                       ID3D11CommandList** result) {
  if (failed) return false;
  if (!recording && FAILED(dev->CreateDeferredContext(0, &recording))) return false;
  auto& cs = encode ? to_pq : from_pq;
  if (!cs) {
    std::string code = encode ? "#define ENCODE\n" : "";
    code += shader;
    cs.Attach(kernels::compile(dev, code.data(), code.size(), encode ? "scRGB_to_HDR10" : "HDR10_to_scRGB"));
    if (!cs) {
      failed = true;
      return false;
    }
  }
  ComPtr<ID3D11ShaderResourceView> srv;
  ComPtr<ID3D11UnorderedAccessView> uav;
  if (FAILED(dev->CreateShaderResourceView(src, nullptr, &srv)) ||
      FAILED(dev->CreateUnorderedAccessView(dst, nullptr, &uav))) return false;
  D3D11_TEXTURE2D_DESC d;
  dst->GetDesc(&d);
  recording->CSSetShader(cs.Get(), nullptr, 0);
  recording->CSSetShaderResources(0, 1, srv.GetAddressOf());
  recording->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
  recording->Dispatch((d.Width + 7) / 8, (d.Height + 7) / 8, 1);
  return SUCCEEDED(orig_FinishCommandList(recording.Get(), FALSE, result));
}
} // namespace aco::fg_color
