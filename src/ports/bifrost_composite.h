#pragma once
#include <d3d12.h>

#include <functional>

namespace bifrost {
void remember_root(ID3D12RootSignature* root, const void* bytes, SIZE_T size);
void prepare_composite(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                       ID3D12PipelineState* original);
bool composite(ID3D12GraphicsCommandList* list, ID3D12Resource* native, ID3D12PipelineState* original,
               ID3D12RootSignature* root, const UINT64* tables, UINT camera_index, UINT64 camera_address,
               const std::function<void()>& draw);
}  // namespace bifrost
