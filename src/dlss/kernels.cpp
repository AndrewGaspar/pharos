#include "dlss/kernels.h"

#include <windows.h>
#include <d3dcompiler.h>

#include "core/log.h"

namespace aco::kernels {

namespace {

// Reconstructs per-pixel motion exactly like the engine's TAA resolve (TemporalAA PS):
//  * pixels at/after MaxViewDepth (sky, far) use camera-only reprojection from the screen position
//  * everything else decodes the engine's motion buffer, which stores (currentUV - previousUV)
//    packed into RGBA8 as two 12-bit values with a quartic curve: m = sign(v) * 0.2 * v^4.
// Unlike the TAA we don't dilate by closest depth: DLSS does its own dilation.
const char kMotionHlsl[] = R"(
cbuffer PassParams     : register(b1) { float4 pp[85]; }
cbuffer InstanceParams : register(b3) { float4 ip[20]; }
cbuffer TemporalAA     : register(b5) { float4 taa[8]; }

Texture2D<float4> Motion   : register(t0);
Texture2D<float>  LinearZ  : register(t1);
RWTexture2D<float2> OutMV  : register(u0);

float2 decode_motion(float3 m)
{
    float g = m.y * 15.9375;                       // 255/16
    float x = m.x * 4080.0 + floor(g);
    float y = frac(g) * 4096.0 + m.z * 255.0;
    float2 v = clamp(float2(x, y) * (2.0 / 4095.0) - 1.0, -1.0, 1.0);
    float2 v2 = v * v;
    return sign(v) * 0.2 * v2 * v2;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    float2 size = taa[2].xy;        // LowResSize
    float2 inv  = taa[2].zw;        // LowResSizeInv
    if (id.x >= (uint)size.x || id.y >= (uint)size.y) return;

    float2 uv = (float2(id) + 0.5) * inv;
    float z = LinearZ.Load(int3(id, 0));
    float far_threshold = (taa[7].x - taa[7].y) / (taa[7].z - taa[7].w);   // MaxViewDepthParams

    float2 motion;
    if (z >= far_threshold)
    {
        float4 clip = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), pp[44].y, 1.0);
        float4 v = float4(dot(clip, pp[57]), dot(clip, pp[58]), dot(clip, pp[59]), dot(clip, pp[60]));
        float4 view = float4(v.xyz / v.w, 1.0);                                   // RevProjParams.ClipXYZToViewPos
        float4 world = float4(dot(view, pp[4]), dot(view, pp[5]), dot(view, pp[6]), 1.0);   // viewToWorld
        float3 prev = float3(dot(world, ip[12]), dot(world, ip[13]), dot(world, ip[15]));   // worldViewProjPrevFrame
        float2 prev_uv = prev.xy / prev.z * float2(0.5, -0.5) + 0.5;
        motion = uv - prev_uv;
    }
    else
    {
        motion = decode_motion(Motion.Load(int3(id, 0)).xyz);
    }
    OutMV[id] = -motion;
}
)";

const char kBlitHlsl[] = R"(
cbuffer BlitParams : register(b0)
{
    float2 dst_size_inv;
    float2 src_size;
    uint   mode;
    float  scale;
    float2 pad;
};
Texture2D<float4> Src : register(t0);
SamplerState LinearClamp : register(s0);
RWTexture2D<float4> Out : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    float2 uv = (float2(id) + 0.5) * dst_size_inv;
    float4 c = Src.SampleLevel(LinearClamp, uv, 0);
    if (mode == 1)      // motion vectors (UV units) -> pixels, visualised around grey
        c = float4(saturate(0.5 + c.xy * src_size * scale), 0.5, 1.0);
    else if (mode == 2) // reversed-Z device depth
        c = float4(pow(saturate(c.xxx), scale), 1.0);
    Out[id] = c;
}
)";

using PFN_D3DCompile = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const D3D_SHADER_MACRO*, ID3DInclude*,
                                        const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);

}  // namespace

ID3D11ComputeShader* compile(ID3D11Device* dev, const char* src, size_t len, const char* name) {
  static PFN_D3DCompile d3d_compile = [] {
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    return m ? reinterpret_cast<PFN_D3DCompile>(GetProcAddress(m, "D3DCompile")) : nullptr;
  }();
  if (!d3d_compile) {
    ACO_LOG("kernels: d3dcompiler_47.dll not available");
    return nullptr;
  }
  ID3DBlob* code = nullptr;
  ID3DBlob* errors = nullptr;
  HRESULT hr = d3d_compile(src, len, name, nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                           &code, &errors);
  if (errors) {
    ACO_LOG("kernels: %s: %.*s", name, static_cast<int>(errors->GetBufferSize()),
            static_cast<const char*>(errors->GetBufferPointer()));
    errors->Release();
  }
  if (FAILED(hr) || !code) return nullptr;
  ID3D11ComputeShader* cs = nullptr;
  hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs);
  code->Release();
  if (FAILED(hr)) ACO_LOG("kernels: CreateComputeShader(%s) failed 0x%08lx", name, hr);
  return cs;
}


bool create(ID3D11Device* dev, Kernels& k) {
  k.motion = compile(dev, kMotionHlsl, sizeof(kMotionHlsl) - 1, "aco_motion.hlsl");
  k.blit = compile(dev, kBlitHlsl, sizeof(kBlitHlsl) - 1, "aco_blit.hlsl");
  D3D11_SAMPLER_DESC sd = {};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  dev->CreateSamplerState(&sd, &k.linear_clamp);
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = sizeof(BlitParams);
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  dev->CreateBuffer(&bd, nullptr, &k.blit_cb);
  bool ok = k.motion && k.blit && k.linear_clamp && k.blit_cb;
  ACO_LOG("kernels: %s", ok ? "ready" : "FAILED");
  return ok;
}

} // namespace aco::kernels
