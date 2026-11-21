#pragma once
#include <d3d12.h>
#include <wrl/client.h>

#include "dlss/upscaler.h"

namespace bifrost {
struct DrawState {
  ID3D12PipelineState* pso{};
  ID3D12RootSignature* signature{};
  D3D12_CPU_DESCRIPTOR_HANDLE targets[8]{}, depth{};
  UINT num_targets{};
  D3D_PRIMITIVE_TOPOLOGY topology{};
  UINT viewport_count{}, scissor_count{};
  D3D12_VIEWPORT
  viewports[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
  D3D12_RECT
  scissors[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
};
struct FrameInfo {
  UINT64 serial{}, native{};  // Resource identity, only compared while processing the submission.
  int eye{};
  UINT width{}, height{};
  float jitter[2]{};
  aco::FgCamera camera{};
};
struct Scene {
  FrameInfo info;
  Microsoft::WRL::ComPtr<ID3D12Resource> color, motion, depth, output;
};
bool record_scene(ID3D12GraphicsCommandList* list, const DrawState& state, const FrameInfo& info, ID3D12Resource* native);
Scene recorded_scene(ID3D12Object* object);
void forward_scene(ID3D12Object* dst, const Scene& scene);
void clear_scene(ID3D12GraphicsCommandList* list);
void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after);
HRESULT make_texture(ID3D12Device* dev, UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                     D3D12_RESOURCE_STATES initial, ID3D12Resource** out);
}  // namespace bifrost
