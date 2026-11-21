#include "ports/bifrost_composite.h"

#include <wrl/client.h>
#include <d3dcompiler.h>
#include <mutex>
#include <vector>

#include "core/hook.h"
#include "ports/common.h"
#include "ports/bifrost_sr.h"
#include "ports/bifrost_eye.h"
#include "ports/bifrost_present.h"

using namespace aco;
using Microsoft::WRL::ComPtr;
namespace port {
extern const size_t create_root, graphics_pso, set_pso, set_groot, set_gtable;
}
namespace bifrost {
namespace {
constexpr GUID key(unsigned n) {
  return {0x3db7a31e, 0x3149, 0x4600, {0x9a, 0xa5, 0xcf, 0x1d, 0xba, 0x91, BYTE(n >> 8), BYTE(n)}};
}
constexpr auto root_bytes = key(1), alternate_pso = key(2), alternate_root = key(3), table_count = key(4);
template <class Obj, class T>
bool read(Obj* obj, REFGUID id, T& v) {
  UINT n = sizeof(v);
  return SUCCEEDED(obj->GetPrivateData(id, &n, &v)) && n == sizeof(v);
}
template <class T>
ComPtr<T> associated(ID3D12Object* o, REFGUID id) {
  ComPtr<T> out;
  UINT n = sizeof(void*);
  o->GetPrivateData(id, &n, out.GetAddressOf());
  return out;
}
ComPtr<ID3DBlob> pixel_shader() {
  // Keep the shipped CompositeTextureInvSrcAlpha math. A root buffer lets the
  // reconstructed scene coexist with Valhalla's fully allocated descriptor heap.
  static constexpr char hlsl[] = R"(
Texture2D<float4> ui : register(t0,space4);
Texture2D<float4> nativeScene : register(t1,space4);
SamplerState filterScene : register(s2,space98);
cbuffer Display : register(b0,space4) { float4 adjustment; }
ByteAddressBuffer reconstructed : register(t0,space77);
cbuffer Footprint : register(b0,space77) { uint width; uint height; uint pitch; uint compare; }
float4 texel(int2 xy) {
  uint2 p=clamp(xy,0,int2(width,height)-1);
  uint v=reconstructed.Load(512+p.y*pitch+p.x*4);
  return float4(v&255,(v>>8)&255,(v>>16)&255,v>>24)/255.;
}
float4 main(float4 pos:SV_Position,float2 uv:TEXCOORD0):SV_Target {
  float4 overlay=ui.Sample(filterScene,uv);
  float4 scene;
  if(reconstructed.Load(0)!=0) {
    float2 xy=uv*float2(width,height)-.5;
    int2 p=int2(floor(xy));float2 f=frac(xy);
    scene=lerp(lerp(texel(p),texel(p+int2(1,0)),f.x),
               lerp(texel(p+int2(0,1)),texel(p+1),f.x),f.y);
    if (compare && uv.x < .5) {
      float4 native=nativeScene.SampleLevel(filterScene,uv,0);
      scene=uv.x < .25 ? native : float4(saturate(4*abs(scene.rgb-native.rgb)),1);
    }
  } else scene=nativeScene.SampleLevel(filterScene,uv,0);
  float4 result=overlay+(1-overlay.a)*scene;
  result.rgb=pow(abs((result.rgb-.5)*adjustment.y+.5),adjustment.x);
  return result;
})";
  auto dll = LoadLibraryW(L"d3dcompiler_47.dll");
  auto fn = dll ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(dll, "D3DCompile")) : nullptr;
  ComPtr<ID3DBlob> code, errors;
  if (!fn || FAILED(fn(hlsl, sizeof(hlsl) - 1, "bifrost_ui", nullptr, nullptr, "main", "ps_5_1",
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
    ACO_LOG("Bifrost: UI compile: %s",
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compiler unavailable");
  if (dll) FreeLibrary(dll);
  return code;
}
ComPtr<ID3D12RootSignature> alternate(ID3D12Device* dev, ID3D12RootSignature* root, UINT& n) {
  UINT bytes = 0;
  root->GetPrivateData(root_bytes, &bytes, nullptr);
  if (!bytes) return {};
  std::vector<BYTE> serialized(bytes);
  if (FAILED(root->GetPrivateData(root_bytes, &bytes, serialized.data()))) return {};
  static HMODULE dll = port::system_library(L"d3d12.dll");
  static auto decode = reinterpret_cast<decltype(&D3D12CreateVersionedRootSignatureDeserializer)>(
      GetProcAddress(dll, "D3D12CreateVersionedRootSignatureDeserializer"));
  static auto encode =
      reinterpret_cast<decltype(&D3D12SerializeRootSignature)>(GetProcAddress(dll, "D3D12SerializeRootSignature"));
  ComPtr<ID3D12VersionedRootSignatureDeserializer> reader;
  if (!decode || !encode || FAILED(decode(serialized.data(), bytes, IID_PPV_ARGS(&reader)))) return {};
  const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* version{};
  if (FAILED(reader->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_0, &version))) return {};
  auto d = version->Desc_1_0;
  n = d.NumParameters;
  if (n > 16) return {};
  std::vector<D3D12_ROOT_PARAMETER> params(d.pParameters, d.pParameters + n);
  UINT camera = eye::enabled() ? eye::root_index(root) : UINT_MAX;
  for (UINT i = 0; i < n; ++i) {
    if (i == camera && params[i].ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV) continue;
    if (params[i].ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) return {};
  }
  D3D12_ROOT_PARAMETER image{}, dimensions{};
  image.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  image.Descriptor = {0, 77};
  image.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  dimensions.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  dimensions.Constants = {0, 77, 4};
  dimensions.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params.push_back(image);
  params.push_back(dimensions);
  d.NumParameters = UINT(params.size());
  d.pParameters = params.data();
  ComPtr<ID3DBlob> code, errors;
  if (FAILED(encode(&d, D3D_ROOT_SIGNATURE_VERSION_1_0, &code, &errors))) {
    ACO_LOG("Bifrost: composite signature: %s",
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "serialization failed");
    return {};
  }
  ComPtr<ID3D12RootSignature> alt;
  orig<HRESULT WINAPI(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**)>(dev, port::create_root)(
      dev, 0, code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&alt));
  return alt;
}
}  // namespace
void remember_root(ID3D12RootSignature* root, const void* bytes, SIZE_T size) {
  if (size <= UINT_MAX) root->SetPrivateData(root_bytes, UINT(size), bytes);
}
void prepare_composite(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                       ID3D12PipelineState* original) {
  if (!desc.pRootSignature) return;
  UINT n{};
  auto root = alternate(dev, desc.pRootSignature, n);
  if (!root) return;
  auto d = desc;
  static const auto ps = pixel_shader();
  if (!ps) return;
  d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  d.pRootSignature = root.Get();
  d.CachedPSO = {};
  ComPtr<ID3D12PipelineState> pso;
  auto hr = orig<HRESULT WINAPI(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**)>(
      dev, port::graphics_pso)(dev, &d, IID_PPV_ARGS(&pso));
  if (FAILED(hr)) {
    ACO_LOG("Bifrost: alternate UI pipeline 0x%08lx", hr);
    return;
  }
  original->SetPrivateDataInterface(alternate_root, root.Get());
  original->SetPrivateDataInterface(alternate_pso, pso.Get());
  original->SetPrivateData(table_count, sizeof(n), &n);
  ACO_LOG("Bifrost: native UI pipeline accepts alternate scene in root %u", n);
}
bool composite(ID3D12GraphicsCommandList* list, ID3D12Resource* native, ID3D12PipelineState* pso,
               ID3D12RootSignature* root, const UINT64* tables, UINT camera_index, UINT64 camera_address,
               const std::function<void()>& draw) {
  if (!native || !pso || !root) return false;
  auto output = output_for(native);
  if (!output) {
    static std::atomic<unsigned> misses{};
    auto i = ++misses;
    if (i <= 4 || i == 300) ACO_LOG("Bifrost: UI input %p has no TAA output association", native);
    return false;
  }
  auto buffer = display_buffer(output.Get());
  UINT n{};
  if (!buffer || !read(pso, table_count, n)) return false;
  auto alt = associated<ID3D12PipelineState>(pso, alternate_pso);
  auto signature = associated<ID3D12RootSignature>(pso, alternate_root);
  if (!alt || !signature) return false;
  auto set_root = orig<void WINAPI(ID3D12GraphicsCommandList*, ID3D12RootSignature*)>(list, port::set_groot);
  auto set_pso = orig<void WINAPI(ID3D12GraphicsCommandList*, ID3D12PipelineState*)>(list, port::set_pso);
  auto set_table =
      orig<void WINAPI(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE)>(list, port::set_gtable);
  auto bind = [&](ID3D12PipelineState* p, ID3D12RootSignature* r) {
    set_root(list, r);
    set_pso(list, p);
    for (UINT i = 0; i < n; ++i)
      if (i == camera_index)
        list->SetGraphicsRootConstantBufferView(i, camera_address);
      else if (tables[i])
        set_table(list, i, {tables[i]});
  };
  auto d = output->GetDesc();
  UINT pitch =
      (UINT(d.Width) * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~UINT(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
  static const auto compare = option(L"upscaler", L"compare", 0);
  UINT dims[] = {UINT(d.Width), d.Height, pitch, compare};
  bind(alt.Get(), signature.Get());
  list->SetGraphicsRootShaderResourceView(n, buffer->GetGPUVirtualAddress());
  list->SetGraphicsRoot32BitConstants(n + 1, 4, dims, 0);
  draw();
  bind(pso, root);
  static std::atomic<unsigned> uses{};
  auto seq = ++uses;
  if (seq <= 2 || seq % 300 == 0) ACO_LOG("Bifrost: recorded SR/native selection for UI composite #%u", seq);
  return true;
}
}  // namespace bifrost
