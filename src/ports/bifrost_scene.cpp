#include "ports/bifrost_scene.h"

#include <algorithm>
#include <atomic>
#include <d3dcompiler.h>

#include "core/hook.h"
#include "core/log.h"

using Microsoft::WRL::ComPtr;
namespace port {
extern const size_t draw, set_pso, set_targets, topology, viewports, scissors;
}
namespace bifrost {
namespace {
// Sample the native temporal draw's descriptors in place. Both branches and the
// inverse of ConvertLinearZ are derived from the game's SM5.1 pixel shaders.
const char shader[] = R"(
cbuffer Camera : register(b0, space1) { float4 camera[82]; }
cbuffer Temporal : register(b0, space2) { float4 taa[7]; }
Texture2D<float4> color : register(t0, space2);
Texture2D<float4> packed : register(t1, space2);
Texture2D<float> linearZ : register(t2, space2);
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(float2((id << 1) & 2, id & 2) * float2(2,-2) + float2(-1,1), 0, 1);
}
struct Outputs { float4 color : SV_Target0; float2 motion : SV_Target1; float depth : SV_Target2; };
Outputs ps(float4 pos : SV_Position) {
  Outputs o;
  int3 p = int3(int2(pos.xy), 0);
  o.color = color.Load(p);
  float z = linearZ.Load(p);
  float4 c = camera[81];
  o.depth = saturate((c.y - z*c.x)/(z*c.z - c.w));
  float4 farParams = taa[6];
  if (z >= (farParams.x-farParams.y)/(farParams.z-farParams.w)) {
    float2 uv = pos.xy * taa[2].zw;
    float4 clip = float4(uv*float2(2,-2)+float2(-1,1),camera[54].y,1);
    float4 view = float4(dot(clip,camera[69]),dot(clip,camera[70]),dot(clip,camera[71]),dot(clip,camera[72]));
    view /= view.w;
    float4 world = float4(dot(view,camera[6]),dot(view,camera[7]),dot(view,camera[8]),1);
    float3 prev = float3(dot(world,camera[38]),dot(world,camera[39]),dot(world,camera[41]));
    o.motion = prev.xy / prev.z * float2(.5,-.5) + .5 - uv;
  } else {
    float3 m = packed.Load(p).rgb;
    float g = m.y * 15.9375;
    float2 raw = float2(m.x*4080+floor(g),frac(g)*4096+m.z*255);
    float2 v = clamp(raw*(2.0/4095.0)-1,-1,1);
    float2 v2=v*v;
    o.motion = -sign(v)*0.2*v2*v2;
  }
  return o;
})";
constexpr GUID tag(unsigned n) {
  return {0x1ad03511, 0xb710, 0x45b0, {0xb1, 0x42, 0x56, 0x39, 0x19, 0xd0, 0x31, BYTE(n)}};
}
constexpr GUID info_tag = tag(1), color_tag = tag(2), motion_tag = tag(3), depth_tag = tag(4), heap_tag = tag(5),
               pso_tag = tag(6), signature_tag = tag(7);
constexpr DXGI_FORMAT formats[]{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT};
template <class T>
ComPtr<T> associated(ID3D12Object* o, REFGUID id) {
  ComPtr<T> value;
  UINT size = sizeof(void*);
  o->GetPrivateData(id, &size, value.GetAddressOf());
  return value;
}
ComPtr<ID3DBlob> compile(const char* entry, const char* profile) {
  const auto dll = LoadLibraryW(L"d3dcompiler_47.dll");
  const auto fn = dll ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(dll, "D3DCompile")) : nullptr;
  ComPtr<ID3DBlob> code, errors;
  if (!fn || FAILED(fn(shader, sizeof(shader) - 1, "bifrost_scene", nullptr, nullptr, entry, profile,
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
    ACO_LOG("Bifrost: compiling %s failed: %s", profile,
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no compiler");
  }
  if (dll) FreeLibrary(dll);
  return code;
}
ComPtr<ID3D12PipelineState> pipeline(ID3D12Device* dev, ID3D12RootSignature* sig, ID3D12GraphicsCommandList* list) {
  auto existing = associated<ID3D12PipelineState>(list, pso_tag);
  ID3D12RootSignature* previous{};
  UINT size = sizeof(previous);
  if (existing && SUCCEEDED(existing->GetPrivateData(signature_tag, &size, &previous)) && previous == sig)
    return existing;
  static auto vs = compile("vs", "vs_5_1"), ps = compile("ps", "ps_5_1");
  if (!vs || !ps) return {};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
  d.pRootSignature = sig;
  d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  d.SampleDesc.Count = 1;
  d.SampleMask = ~0u;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  d.RasterizerState.DepthClipEnable = TRUE;
  d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  d.NumRenderTargets = 3;
  std::copy(std::begin(formats), std::end(formats), d.RTVFormats);
  ComPtr<ID3D12PipelineState> pso;
  auto hr = dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
  if (FAILED(hr)) {
    static std::atomic<bool> logged{};
    if (!logged.exchange(true)) ACO_LOG("Bifrost: scene extraction pipeline 0x%08lx", hr);
    return {};
  }
  pso->SetPrivateData(signature_tag, sizeof(sig), &sig);
  list->SetPrivateDataInterface(pso_tag, pso.Get());
  return pso;
}
bool targets(ID3D12Device* dev, ID3D12GraphicsCommandList* list, Scene& scene) {
  scene.color = associated<ID3D12Resource>(list, color_tag);
  scene.motion = associated<ID3D12Resource>(list, motion_tag);
  scene.depth = associated<ID3D12Resource>(list, depth_tag);
  if (scene.color && scene.motion && scene.depth) {
    auto d = scene.color->GetDesc();
    if (d.Width == scene.info.width && d.Height == scene.info.height) return true;
  }
  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3};
  if (FAILED(dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)))) return false;
  auto handle = heap->GetCPUDescriptorHandleForHeapStart();
  size_t i = 0;
  for (auto* res : {std::addressof(scene.color), std::addressof(scene.motion), std::addressof(scene.depth)}) {
    if (FAILED(make_texture(dev, scene.info.width, scene.info.height, formats[i++],
                            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            res->ReleaseAndGetAddressOf())))
      return false;
    dev->CreateRenderTargetView(res->Get(), nullptr, handle);
    handle.ptr += dev->GetDescriptorHandleIncrementSize(desc.Type);
  }
  return SUCCEEDED(scene.color->SetPrivateDataInterface(heap_tag, heap.Get()));
}
}  // namespace

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  list->ResourceBarrier(1, &b);
}
HRESULT make_texture(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                     D3D12_RESOURCE_STATES initial, ID3D12Resource** out) {
  D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
  hp.CreationNodeMask = hp.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = w;
  d.Height = h;
  d.MipLevels = d.DepthOrArraySize = d.SampleDesc.Count = 1;
  d.Format = format;
  d.Flags = flags;
  return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, initial, nullptr, IID_PPV_ARGS(out));
}
void forward_scene(ID3D12Object* dst, const Scene& s) {
  dst->SetPrivateDataInterface(color_tag, s.color.Get());
  dst->SetPrivateDataInterface(motion_tag, s.motion.Get());
  dst->SetPrivateDataInterface(depth_tag, s.depth.Get());
  dst->SetPrivateData(info_tag, sizeof(s.info), &s.info);
}
Scene recorded_scene(ID3D12Object* o) {
  Scene s{};
  UINT n = sizeof(s.info);
  if (FAILED(o->GetPrivateData(info_tag, &n, &s.info)) || !s.info.width) return {};
  s.color = associated<ID3D12Resource>(o, color_tag);
  s.motion = associated<ID3D12Resource>(o, motion_tag);
  s.depth = associated<ID3D12Resource>(o, depth_tag);
  return s;
}
void clear_scene(ID3D12GraphicsCommandList* list) { list->SetPrivateData(info_tag, 0, nullptr); }
bool record_scene(ID3D12GraphicsCommandList* list, const DrawState& state, const FrameInfo& info) {
  if (!state.topology || !state.signature || !state.pso || !state.viewport_count || !state.scissor_count) return false;
  ComPtr<ID3D12Device> dev;
  if (FAILED(list->GetDevice(IID_PPV_ARGS(&dev)))) return false;
  const auto pso = pipeline(dev.Get(), state.signature, list);
  static std::atomic<UINT64> sequence{};
  Scene s{info};
  s.info.serial = ++sequence;
  if (!pso || !targets(dev.Get(), list, s)) return false;
  const auto heap = associated<ID3D12DescriptorHeap>(s.color.Get(), heap_tag);
  if (!heap) return false;
  for (auto* res : {s.color.Get(), s.motion.Get(), s.depth.Get()})
    transition(list, res, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto bind_pso = aco::orig<void WINAPI(ID3D12GraphicsCommandList*, ID3D12PipelineState*)>(list, port::set_pso);
  auto bind_topology =
      aco::orig<void WINAPI(ID3D12GraphicsCommandList*, D3D12_PRIMITIVE_TOPOLOGY)>(list, port::topology);
  auto bind_targets = aco::orig<void WINAPI(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE*)>(list, port::set_targets);
  auto viewport =
      aco::orig<void WINAPI(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*)>(list, port::viewports);
  auto scissor = aco::orig<void WINAPI(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*)>(list, port::scissors);
  auto handle = heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_VIEWPORT vp{0, 0, float(info.width), float(info.height), 0, 1};
  D3D12_RECT rect{0, 0, LONG(info.width), LONG(info.height)};
  bind_pso(list, pso.Get());
  bind_targets(list, 3, &handle, TRUE, nullptr);
  bind_topology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  viewport(list, 1, &vp);
  scissor(list, 1, &rect);
  aco::orig<void WINAPI(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT)>(list, port::draw)(list, 3, 1, 0, 0);
  viewport(list, state.viewport_count, state.viewports);
  scissor(list, state.scissor_count, state.scissors);
  bind_topology(list, state.topology);
  bind_targets(list, state.num_targets, state.targets, FALSE, state.depth.ptr ? &state.depth : nullptr);
  bind_pso(list, state.pso);
  for (auto* res : {s.color.Get(), s.motion.Get(), s.depth.Get()})
    transition(list, res, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  forward_scene(list, s);
  static std::atomic<bool> logged{};
  if (!logged.exchange(true))
    ACO_LOG(
        "Bifrost: captured pre-TAA color, decoded UV motion and reversed "
        "device depth at %ux%u",
        info.width, info.height);
  return true;
}
}  // namespace bifrost
