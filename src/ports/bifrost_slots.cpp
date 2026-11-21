#define CINTERFACE
#include <d3d12.h>

#include <cstddef>
namespace port {
extern const size_t topology = offsetof(ID3D12GraphicsCommandListVtbl, IASetPrimitiveTopology) / sizeof(void*);
extern const size_t viewports = offsetof(ID3D12GraphicsCommandListVtbl, RSSetViewports) / sizeof(void*);
extern const size_t scissors = offsetof(ID3D12GraphicsCommandListVtbl, RSSetScissorRects) / sizeof(void*);
extern const size_t execute = offsetof(ID3D12CommandQueueVtbl, ExecuteCommandLists) / sizeof(void*);
extern const size_t committed = offsetof(ID3D12DeviceVtbl, CreateCommittedResource) / sizeof(void*);
extern const size_t placed = offsetof(ID3D12DeviceVtbl, CreatePlacedResource) / sizeof(void*);
extern const size_t create_root = offsetof(ID3D12DeviceVtbl, CreateRootSignature) / sizeof(void*);
extern const size_t graphics_pso = offsetof(ID3D12DeviceVtbl, CreateGraphicsPipelineState) / sizeof(void*);
extern const size_t compute_pso = offsetof(ID3D12DeviceVtbl, CreateComputePipelineState) / sizeof(void*);
extern const size_t create_list = offsetof(ID3D12DeviceVtbl, CreateCommandList) / sizeof(void*);
extern const size_t set_pso = offsetof(ID3D12GraphicsCommandListVtbl, SetPipelineState) / sizeof(void*);
extern const size_t reset_list = offsetof(ID3D12GraphicsCommandListVtbl, Reset) / sizeof(void*);
extern const size_t clear_state = offsetof(ID3D12GraphicsCommandListVtbl, ClearState) / sizeof(void*);
extern const size_t indirect = offsetof(ID3D12GraphicsCommandListVtbl, ExecuteIndirect) / sizeof(void*);
extern const size_t draw_indexed = offsetof(ID3D12GraphicsCommandListVtbl, DrawIndexedInstanced) / sizeof(void*);
extern const size_t draw = offsetof(ID3D12GraphicsCommandListVtbl, DrawInstanced) / sizeof(void*);
extern const size_t set_targets = offsetof(ID3D12GraphicsCommandListVtbl, OMSetRenderTargets) / sizeof(void*);
extern const size_t set_groot = offsetof(ID3D12GraphicsCommandListVtbl, SetGraphicsRootSignature) / sizeof(void*);
extern const size_t set_gtable =
    offsetof(ID3D12GraphicsCommandListVtbl, SetGraphicsRootDescriptorTable) / sizeof(void*);
extern const size_t set_heaps = offsetof(ID3D12GraphicsCommandListVtbl, SetDescriptorHeaps) / sizeof(void*);
extern const size_t copy_simple = offsetof(ID3D12DeviceVtbl, CopyDescriptorsSimple) / sizeof(void*);
extern const size_t copy_ranges = offsetof(ID3D12DeviceVtbl, CopyDescriptors) / sizeof(void*);
extern const size_t make_rtv = offsetof(ID3D12DeviceVtbl, CreateRenderTargetView) / sizeof(void*);
extern const size_t make_uav = offsetof(ID3D12DeviceVtbl, CreateUnorderedAccessView) / sizeof(void*);
extern const size_t make_srv = offsetof(ID3D12DeviceVtbl, CreateShaderResourceView) / sizeof(void*);
extern const size_t make_cbv = offsetof(ID3D12DeviceVtbl, CreateConstantBufferView) / sizeof(void*);
extern const size_t create_heap = offsetof(ID3D12DeviceVtbl, CreateDescriptorHeap) / sizeof(void*);
}  // namespace port
