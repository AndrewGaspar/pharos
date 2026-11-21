#pragma once
#include <d3d12.h>

#include "ports/common.h"

namespace bifrost {
void observe_descriptors(ID3D12Device* dev);
void observe_commands(ID3D12GraphicsCommandList* list);
void reset_inputs(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso);
void bind_inputs(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso);
void read_layout(ID3D12RootSignature* root, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc);
void tag_inputs(ID3D12PipelineState* pso, ID3D12RootSignature* root, port::Entry& entry);
void tag_hud(ID3D12Device* dev, ID3D12PipelineState* pso, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);
void hud_capture_present();
}  // namespace bifrost
