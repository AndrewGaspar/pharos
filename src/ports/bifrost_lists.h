#pragma once
#include <d3d12.h>
namespace bifrost::dual {
ID3D12GraphicsCommandList* list(ID3D12GraphicsCommandList* source);
ID3D12GraphicsCommandList* list(ID3D12CommandList* source);
void make_list(ID3D12GraphicsCommandList* original, ID3D12Device* dev, UINT node, D3D12_COMMAND_LIST_TYPE type,
               ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial);
void reset_list(ID3D12GraphicsCommandList* original, ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial);
void reset_allocator(ID3D12CommandAllocator* allocator);
void install(ID3D12GraphicsCommandList* list);
void resource_barriers(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers);
}  // namespace bifrost::dual
