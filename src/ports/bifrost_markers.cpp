#include "ports/bifrost_markers.h"

#include <cmath>
#include <cstring>
#include <mutex>
#include <d3dcompiler.h>
#include "core/hook.h"
#include "core/log.h"
#include "ports/bifrost_dual.h"
#include "ports/bifrost_eye.h"
#include "ports/bifrost_scene.h"
#include "ports/bifrost_present.h"
#include "world/marker_targets.h"

namespace port {
extern const size_t graphics_pso, set_pso;
}
namespace bifrost {
namespace {
using Microsoft::WRL::ComPtr;
constexpr GUID alternate{0xfd93fe33, 0x73c6, 0x4236, {0x8d, 0xb2, 0x21, 0xab, 0x09, 0x67, 0x11, 0xaa}};
constexpr GUID depth_owner{0xfd93fe33, 0x73c6, 0x4236, {0x8d, 0xb2, 0x21, 0xab, 0x09, 0x67, 0x11, 0xab}};
constexpr GUID stamp_owner{0xfd93fe33, 0x73c6, 0x4236, {0x8d, 0xb2, 0x21, 0xab, 0x09, 0x67, 0x11, 0xac}};
std::mutex depth_lock;
MarkerDepth latest;
ComPtr<ID3D12Resource> stamps;
ID3D12Device* depth_device{};
unsigned next_slice{};
constexpr unsigned history = 3;
constexpr char shader[] = R"(
ByteAddressBuffer depth : register(t0,space80);
cbuffer Marker : register(b0,space80) {
  float4 clipZ, clipW, scale;
  uint4 size; uint count; float rayScale; float4 targets[192];
}
struct Vertex {
  float4 p : SV_Position;
  float4 uv : TEXCOORD0;
#ifndef PLAIN
  float4 color : COLOR0;
#endif
  float4 a : TEXCOORD1; float4 b : TEXCOORD2; float4 c : TEXCOORD3; float4 d : TEXCOORD4;
  uint instance : InstanceID;
};
float invDepth(float2 ndc) {
  int2 pixel=int2((ndc*float2(.5,-.5)+.5)*size.xy);
  float z=0;
  uint newest=depth.Load(0);
  // Reversed Z. Keep an obstruction across small silhouette movements and
  // across the three most recent rendered frames. Both eyes use the same depth image.
  [unroll] for (int y=-1;y<=1;y++)
    [unroll] for (int x=-1;x<=1;x++) {
      int2 p=clamp(pixel+int2(x,y),0,int2(size.xy)-1);
      uint address=512+p.y*size.z+4*p.x;
      [unroll] for (uint age=0;age<3;age++) {
        uint slice=(newest+3-age)%3;
        float weight=age==0 ? 1 : age==1 ? .85 : .45;
        z=max(z,weight*asfloat(depth.Load(address+slice*size.w)));
      }
    }
  float4 clip=float4(ndc,z,1);
  return max(0,-dot(clipW,clip)/dot(clipZ,clip));
}
[maxvertexcount(3)]
void main(triangle Vertex v[3], inout TriangleStream<Vertex> stream) {
  float2 a=v[0].p.xy/v[0].p.w, b=v[1].p.xy/v[1].p.w, c=v[2].p.xy/v[2].p.w;
  float2 mono=(min(a,min(b,c))+max(a,max(b,c)))*.5;
  float nativeInv=3/(v[0].p.w+v[1].p.w+v[2].p.w);
  mono.x-=scale.x*(scale.z-nativeInv);
  bool dotOnly=scale.w==4, aim=scale.w==2 || dotOnly, cursor=scale.w==3;
  float best=.045*.045, target=0;
  [loop] for (uint t=0;t<count;t++) {
    float4 p=targets[t];
    float2 delta=mono-p.xy;
    delta.y-=clamp(delta.y,min(0,p.w),max(0,p.w));
    float e=dot(delta,delta);
    if (e<best) { target=p.z; best=e; }
  }
  // If the native billboard used the scene camera, its clip W is already a
  // world distance. Flat UI quads have an arbitrary W and use the surface.
  if (!aim && !cursor && target==0 && scale.x!=0) target=max(0,nativeInv);
  bool active=(!aim && !cursor) || all(abs(mono)<(dotOnly ? .012 : .12));
  if (scale.w!=0 && active) {
    float2 ray=aim ? float2(0,0) : mono+float2(rayScale*(scale.z-target),0);
    float surface=invDepth(ray);
    float chosen=surface>target ? surface*1.005 : target;
    [unroll] for (uint i=0;i<3;i++)
      v[i].p.x+=scale.x*(1-v[i].p.w*scale.z)+scale.y*v[i].p.w*(scale.z-chosen);
  }
  [unroll] for (uint j=0;j<3;j++) stream.Append(v[j]);
})";
ComPtr<ID3DBlob> geometry(bool plain) {
  HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
  auto compile = dll ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(dll, "D3DCompile")) : nullptr;
  D3D_SHADER_MACRO defs[] = {{"PLAIN", "1"}, {nullptr, nullptr}};
  ComPtr<ID3DBlob> code, error;
  if (!compile || FAILED(compile(shader, sizeof(shader) - 1, "bifrost_marker", plain ? defs : nullptr, nullptr, "main",
                                 "gs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error)))
    ACO_LOG("Bifrost: marker shader %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "unavailable");
  if (dll) FreeLibrary(dll);
  return code;
}
UINT64 checksum(const D3D12_SHADER_BYTECODE& code) {
  UINT64 id{};
  if (code.pShaderBytecode && code.BytecodeLength >= 20)
    memcpy(&id, static_cast<const BYTE*>(code.pShaderBytecode) + 4, sizeof(id));
  return id;
}
}  // namespace
MarkerDepth marker_depth() {
  std::lock_guard guard(depth_lock);
  return latest;
}
void MarkerCapture::record(ID3D12GraphicsCommandList* list, ID3D12CommandQueue* queue) {
  static const bool enabled = GetEnvironmentVariableW(L"QUIVER_LAB", nullptr, 0) && option(L"stereo", L"trace_hud", 0);
  if (!enabled) return;
  static ULONGLONG next{};
  const auto now = GetTickCount64();
  if (now < next) return;
  next = now + 250;
  static const auto flag = aco::out_dir() + L"\\capture-marker";
  if (!DeleteFileW(flag.c_str())) return;
  auto depth = marker_depth();
  auto scene = recorded_scene(queue);
  if (!depth.buffer || !scene.info.camera.valid) return;
  auto desc = depth.buffer->GetDesc();
  ComPtr<ID3D12Device> dev;
  D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
  hp.CreationNodeMask = hp.VisibleNodeMask = 1;
  if (FAILED(list->GetDevice(IID_PPV_ARGS(&dev))) ||
      FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&readback))))
    return;
  auto f = _wfopen((aco::out_dir() + L"\\marker-camera.tsv").c_str(), L"w");
  if (f) {
    fprintf(f, "depth\t%u\t%u\t%u\t%u\n", depth.width, depth.height, depth.pitch, depth.slice_bytes);
    for (const auto& row : scene.info.world_to_clip)
      fprintf(f, "vp\t%.9g\t%.9g\t%.9g\t%.9g\n", row[0], row[1], row[2], row[3]);
    for (const auto& row : scene.info.camera.clip_to_view)
      fprintf(f, "inv\t%.9g\t%.9g\t%.9g\t%.9g\n", row[0], row[1], row[2], row[3]);
    aco::MarkerTarget points[aco::kMaxMarkerTargets];
    auto count = aco::marker_targets_read(points);
    for (size_t i = 0; i < count; ++i) fprintf(f, "target\t%.9g\t%.9g\t%.9g\n", points[i].x, points[i].y, points[i].z);
    fclose(f);
  }
  source = depth.buffer;
  transition(list, source.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(readback.Get(), source.Get());
  transition(list, source.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}
void MarkerCapture::finish() {
  if (!readback) return;
  const SIZE_T bytes = SIZE_T(readback->GetDesc().Width);
  D3D12_RANGE range{0, bytes}, written{};
  void* mapped{};
  if (SUCCEEDED(readback->Map(0, &range, &mapped))) {
    if (auto f = _wfopen((aco::out_dir() + L"\\marker-depth.bin").c_str(), L"wb")) {
      fwrite(mapped, 1, bytes, f);
      fclose(f);
    }
    readback->Unmap(0, &written);
  }
  readback.Reset();
  source.Reset();
}
void capture_marker_depth(ID3D12GraphicsCommandList* list, ID3D12Device* dev, ID3D12Resource* depth, bool right_eye) {
  const auto d = depth->GetDesc();
  std::lock_guard guard(depth_lock);
  bool resized = depth_device != dev || latest.width != d.Width || latest.height != d.Height;
  if (resized) {
    MarkerDepth next;
    next.width = UINT(d.Width);
    next.height = d.Height;
    next.pitch = (next.width * 4 + 255u) & ~255u;
    next.slice_bytes = (next.pitch * next.height + 511u) & ~511u;
    D3D12_RESOURCE_DESC b{};
    b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    b.Width = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT + UINT64(next.slice_bytes) * history;
    b.Height = b.DepthOrArraySize = b.MipLevels = b.SampleDesc.Count = 1;
    b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    hp.CreationNodeMask = hp.VisibleNodeMask = 1;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &b,
                                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                            IID_PPV_ARGS(&next.buffer))))
      return;
    ComPtr<ID3D12Resource> next_stamps;
    b.Width = history * sizeof(UINT);
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&next_stamps))))
      return;
    UINT* values{};
    D3D12_RANGE empty{};
    if (FAILED(next_stamps->Map(0, &empty, reinterpret_cast<void**>(&values)))) return;
    for (UINT i = 0; i < history; ++i) values[i] = i;
    next_stamps->Unmap(0, nullptr);
    if (FAILED(next.buffer->SetPrivateDataInterface(stamp_owner, next_stamps.Get()))) return;
    stamps = std::move(next_stamps);
    latest = std::move(next);
    depth_device = dev;
    next_slice = 0;
  }
  // Keep the destination alive with the native capture texture through GPU retirement.
  depth->SetPrivateDataInterface(depth_owner, latest.buffer.Get());
  auto* buffer = latest.buffer.Get();
  transition(list, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list, buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
  src.pResource = depth;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.pResource = buffer;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_FLOAT, latest.width, latest.height, 1, latest.pitch};
  static bool previous_right;
  const bool refill = resized || previous_right != right_eye;
  previous_right = right_eye;
  unsigned newest = 0;
  for (unsigned i = 0; i < (refill ? history : 1); ++i) {
    newest = next_slice;
    dst.PlacedFootprint.Offset = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT + UINT64(next_slice) * latest.slice_bytes;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    next_slice = (next_slice + 1) % history;
  }
  list->CopyBufferRegion(buffer, 0, stamps.Get(), newest * sizeof(UINT), sizeof(UINT));
  transition(list, buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  transition(list, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}
MarkerKind prepare_marker_pipeline(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d,
                                   ID3D12PipelineState* original) {
  if (!dual::enabled() || d.NumRenderTargets != 1 || !d.BlendState.RenderTarget[0].BlendEnable || d.GS.BytecodeLength ||
      d.PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE ||
      eye::root_index(d.pRootSignature) > eye::max_marker_camera_index)
    return MarkerKind::None;
  const auto vs = checksum(d.VS), ps = checksum(d.PS);
  const bool aim = vs == 0x77d0bb4c42ba8e4f && ps == 0x9499690ed3b7eee7;
  if (!aim && !((vs == 0x94a6d4c33f888201 || vs == 0x0cf8ad1c2ce429f3) && ps == 0xb97c1ae1272f1974))
    return MarkerKind::None;
  static const auto plain = geometry(true), sprite = geometry(false);
  const auto& gs = aim ? plain : sprite;
  if (!gs) return MarkerKind::None;
  auto desc = d;
  desc.GS = {gs->GetBufferPointer(), gs->GetBufferSize()};
  desc.CachedPSO = {};
  ComPtr<ID3D12PipelineState> alt;
  auto create = aco::orig<HRESULT WINAPI(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**)>(
      dev, port::graphics_pso);
  auto hr = create(dev, &desc, IID_PPV_ARGS(&alt));
  if (FAILED(hr)) {
    ACO_LOG("Bifrost: marker pipeline %016llx result %08lx", ps, hr);
    return MarkerKind::None;
  }
  if (FAILED(original->SetPrivateDataInterface(alternate, alt.Get()))) return MarkerKind::None;
  return aim ? MarkerKind::Reticle : MarkerKind::World;
}
bool draw_marker(ID3D12GraphicsCommandList* list, ID3D12PipelineState* original, UINT slot, UINT64 camera,
                 MarkerKind kind, const std::function<void()>& draw) {
  if (slot > eye::max_marker_camera_index || !camera) return false;
  auto depth = marker_depth();
  if (!depth.buffer) return false;
  dual::Internal internal;
  auto cbv = eye::marker_buffer(list, camera, kind, depth);
  if (!cbv) return false;
  ComPtr<ID3D12PipelineState> alt;
  UINT bytes = sizeof(void*);
  if (FAILED(original->GetPrivateData(alternate, &bytes, alt.GetAddressOf()))) return false;
  auto set = aco::orig<void WINAPI(ID3D12GraphicsCommandList*, ID3D12PipelineState*)>(list, port::set_pso);
  list->SetGraphicsRootConstantBufferView(slot + 1, cbv);
  list->SetGraphicsRootShaderResourceView(slot + 2, depth.buffer->GetGPUVirtualAddress());
  set(list, alt.Get());
  draw();
  set(list, original);
  return true;
}
}  // namespace bifrost
