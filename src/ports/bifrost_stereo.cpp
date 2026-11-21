#include "ports/bifrost_dual.h"
#include "ports/bifrost_stereo.h"
#include "ports/bifrost_wide.h"
#include "ports/bifrost_present.h"
#include <d3dcompiler.h>
#include <cmath>
#include <cwchar>
#include "ports/bifrost_scene.h"
#include "ports/common.h"
#include "core/hook.h"

namespace port {
extern const size_t execute, create_root, graphics_pso;
}
namespace bifrost {
namespace {
using Microsoft::WRL::ComPtr;
constexpr char hlsl[] = R"(
Texture2D<float4> leftEye:register(t0);
Texture2D<float4> rightEye:register(t1);
SamplerState linearClamp:register(s0);
cbuffer Display:register(b0) { uint show; uint swap; float differenceGain; }
struct Coord {float4 position:SV_Position; float2 uv:TEXCOORD0;};
Coord vs(uint id:SV_VertexID) {
 Coord o; o.uv=float2((id<<1)&2,id&2);o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;
}
float4 ps(Coord v):SV_Target{
 if (show == 1) return leftEye.SampleLevel(linearClamp,v.uv,0);
 if (show == 2) return rightEye.SampleLevel(linearClamp,v.uv,0);
 if (show == 3) {
   float3 l=leftEye.SampleLevel(linearClamp,v.uv,0).rgb;
   float3 r=rightEye.SampleLevel(linearClamp,v.uv,0).rgb;
   return float4(saturate(abs(l-r)*differenceGain),1);
 }
 float2 uv=float2(frac(v.uv.x*2),v.uv.y);
 bool left=(v.uv.x<.5) != (swap!=0);
 return left ? leftEye.SampleLevel(linearClamp,uv,0) : rightEye.SampleLevel(linearClamp,uv,0);
}
)";
ComPtr<ID3DBlob> compile(const char* fn, const char* target) {
  static const auto dll = LoadLibraryW(L"d3dcompiler_47.dll");
  static const auto build = reinterpret_cast<decltype(&D3DCompile)>(dll ? GetProcAddress(dll, "D3DCompile") : nullptr);
  ComPtr<ID3DBlob> blob, errors;
  if (!build || FAILED(build(hlsl, sizeof(hlsl) - 1, "bifrost_stereo", nullptr, nullptr, fn, target,
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors)))
    return {};
  return blob;
}
bool wait(ID3D12Fence* fence, UINT64 v) {
  if (!v) return true;
  auto done = fence->GetCompletedValue();
  return done != UINT64_MAX && (done >= v || SUCCEEDED(fence->SetEventOnCompletion(v, nullptr)));
}
}  // namespace
StereoOutput::~StereoOutput() { drain(); }
bool StereoOutput::drain() { return wait(fence.Get(), last); }
bool StereoOutput::open(ID3D12Device* dev, const D3D12_RESOURCE_DESC& d) {
  if (!drain()) return false;
  valid = 0;
  if (!fence) {
    Ptr<ID3D12Fence> ready;
    std::array<Slot, 4> commands;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ready)))) return false;
    for (auto& s : commands) {
      if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator))) ||
          FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&s.list))) ||
          FAILED(s.list->Close()))
        return false;
    }
    fence = std::move(ready);
    slots = std::move(commands);
  }
  if (!root) {
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {1, &range};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {0, 0, 3};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxAnisotropy = 1;
    D3D12_ROOT_SIGNATURE_DESC signature{2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    static auto encode = reinterpret_cast<decltype(&D3D12SerializeRootSignature)>(
        GetProcAddress(port::system_library(L"d3d12.dll"), "D3D12SerializeRootSignature"));
    ComPtr<ID3DBlob> code;
    if (!encode || FAILED(encode(&signature, D3D_ROOT_SIGNATURE_VERSION_1_0, &code, nullptr))) return false;
    auto make =
        aco::orig<HRESULT WINAPI(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**)>(dev, port::create_root);
    if (FAILED(make(dev, 0, code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&root)))) return false;
  }
  D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  if (!srv && FAILED(dev->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srv)))) return false;
  heap = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, UINT(slots.size()), D3D12_DESCRIPTOR_HEAP_FLAG_NONE};
  if (!rtv && FAILED(dev->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv)))) return false;
  stride = dev->GetDescriptorHandleIncrementSize(heap.Type);
  auto handle = srv->GetCPUDescriptorHandleForHeapStart();
  for (auto& view : views) {
    if (FAILED(make_texture(dev, UINT(d.Width), d.Height, d.Format, D3D12_RESOURCE_FLAG_NONE,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, view.ReleaseAndGetAddressOf())))
      return false;
    dev->CreateShaderResourceView(view.Get(), nullptr, handle);
    handle.ptr += dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }
  static const auto vs = compile("vs", "vs_5_0"), ps = compile("ps", "ps_5_0");
  if (!vs || !ps) return false;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root.Get();
  desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = d.Format;
  desc.SampleDesc.Count = 1;
  desc.SampleMask = ~0u;
  auto make = aco::orig<HRESULT WINAPI(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**)>(
      dev, port::graphics_pso);
  if (FAILED(make(dev, &desc, IID_PPV_ARGS(pso.ReleaseAndGetAddressOf())))) return false;
  width = UINT(d.Width);
  height = d.Height;
  format = d.Format;
  ACO_LOG("Bifrost: opened two %ux%u native eye images", width, height);
  return true;
}
bool StereoOutput::present(IDXGISwapChain* chain, ID3D12CommandQueue* queue, int eye, UINT64 synchronized_target) {
  dual::Internal internal;
  Ptr<IDXGISwapChain3> sc;
  Ptr<ID3D12Resource> bb;
  if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&sc))) ||
      FAILED(sc->GetBuffer(sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb))))
    return false;
  auto d = bb->GetDesc();
  Ptr<ID3D12Device> dev;
  if (FAILED(queue->GetDevice(IID_PPV_ARGS(&dev)))) return false;
  if ((width != d.Width || height != d.Height || format != d.Format) && !open(dev.Get(), d)) return false;
  slot = (slot + 1) % slots.size();
  auto& s = slots[slot];
  if (!wait(fence.Get(), s.done) || FAILED(s.allocator->Reset()) || FAILED(s.list->Reset(s.allocator.Get(), pso.Get())))
    return false;
  auto* cl = s.list.Get();
  auto handle = rtv->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += slot * stride;
  auto target = wide::physical(chain, sc->GetCurrentBackBufferIndex());
  if (!target) { cl->Close(); return false; }
  const UINT packed_width = UINT(target->GetDesc().Width);
  const bool full_width = target.Get() != bb.Get();
  dev->CreateRenderTargetView(target.Get(), nullptr, handle);
  transition(cl, bb.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto* right = synchronized_target ? dual::other(bb.Get()) : bb.Get();
  const bool both = right != bb.Get() && reinterpret_cast<UINT64>(right) == synchronized_target;
  if (both) transition(cl, right, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  if (eye < 0) valid = 0;
  auto copy = [&](unsigned i) {
    transition(cl, views[i].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(views[i].Get(), both && i ? right : bb.Get());
    transition(cl, views[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  };
  for (int i = 0; i < 2; ++i)
    if (i == eye || !(valid & (1u << i))) copy(i);
  if (both) {
    transition(cl, right, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    valid = 3;
  }
  if (eye >= 0) valid |= 1u << eye;
  if (full_width) transition(cl, bb.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
  transition(cl, target.Get(), full_width ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_COPY_SOURCE,
             D3D12_RESOURCE_STATE_RENDER_TARGET);
  cl->SetGraphicsRootSignature(root.Get());
  auto* heap = srv.Get();
  cl->SetDescriptorHeaps(1, &heap);
  cl->SetGraphicsRootDescriptorTable(0, srv->GetGPUDescriptorHandleForHeapStart());
  auto now = GetTickCount64();
  if (now >= next_settings) {
    auto mode = option_text(L"stereo", L"show", L"sbs");
    show = mode == L"left" ? 1u : mode == L"right" ? 2u : mode == L"diff" ? 3u : 0u;
    swap = option(L"stereo", L"swap", 0) != 0;
    auto gain = option_text(L"stereo", L"diff_gain", L"16");
    wchar_t* end{};
    float value = std::wcstof(gain.c_str(), &end);
    difference_gain = end != gain.c_str() && !*end && std::isfinite(value) && value >= 0 ? value : 16;
    next_settings = now + 1000;
  }
  struct { UINT show, swap; float gain; } display{show, swap, difference_gain};
  cl->SetGraphicsRoot32BitConstants(1, 3, &display, 0);
  cl->OMSetRenderTargets(1, &handle, FALSE, nullptr);
  D3D12_VIEWPORT vp{0, 0, float(packed_width), float(height), 0, 1};
  D3D12_RECT rect{0, 0, LONG(packed_width), LONG(height)};
  cl->RSSetViewports(1, &vp);
  cl->RSSetScissorRects(1, &rect);
  cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cl->DrawInstanced(3, 1, 0, 0);
  transition(cl, target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
  if (FAILED(cl->Close())) return false;
  ID3D12CommandList* batch[]{cl};
  aco::orig<void WINAPI(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*)>(queue, port::execute)(queue, 1, batch);
  if (FAILED(queue->Signal(fence.Get(), last + 1))) return false;
  s.done = ++last;
  if (last == 1) ACO_LOG("Bifrost: %ux%u eyes packed into %ux%u swapchain", width, height, packed_width, height);
  if (valid == 3 && ++pairs % 300 == 0)
    ACO_LOG("Bifrost: packed %s stereo frame #%u", both ? "synchronized" : "alternating", pairs);
  return true;
}
}  // namespace bifrost
