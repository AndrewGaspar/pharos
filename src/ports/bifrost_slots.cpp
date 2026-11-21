#define CINTERFACE
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstddef>
namespace port {
extern const size_t close_list = offsetof(ID3D12GraphicsCommandListVtbl, Close) / sizeof(void*);
extern const size_t copy_buffer = offsetof(ID3D12GraphicsCommandListVtbl, CopyBufferRegion) / sizeof(void*);
extern const size_t copy_texture = offsetof(ID3D12GraphicsCommandListVtbl, CopyTextureRegion) / sizeof(void*);
extern const size_t copy_resource = offsetof(ID3D12GraphicsCommandListVtbl, CopyResource) / sizeof(void*);
extern const size_t resolve_sub = offsetof(ID3D12GraphicsCommandListVtbl, ResolveSubresource) / sizeof(void*);
extern const size_t clear_rtv = offsetof(ID3D12GraphicsCommandListVtbl, ClearRenderTargetView) / sizeof(void*);
extern const size_t clear_dsv = offsetof(ID3D12GraphicsCommandListVtbl, ClearDepthStencilView) / sizeof(void*);
extern const size_t clear_uav_float =
    offsetof(ID3D12GraphicsCommandListVtbl, ClearUnorderedAccessViewFloat) / sizeof(void*);
extern const size_t clear_uav_uint =
    offsetof(ID3D12GraphicsCommandListVtbl, ClearUnorderedAccessViewUint) / sizeof(void*);
extern const size_t discard = offsetof(ID3D12GraphicsCommandListVtbl, DiscardResource) / sizeof(void*);
extern const size_t blend_factor = offsetof(ID3D12GraphicsCommandListVtbl, OMSetBlendFactor) / sizeof(void*);
extern const size_t stencil_ref = offsetof(ID3D12GraphicsCommandListVtbl, OMSetStencilRef) / sizeof(void*);
extern const size_t execute_bundle = offsetof(ID3D12GraphicsCommandListVtbl, ExecuteBundle) / sizeof(void*);
extern const size_t set_index = offsetof(ID3D12GraphicsCommandListVtbl, IASetIndexBuffer) / sizeof(void*);
extern const size_t set_vertices = offsetof(ID3D12GraphicsCommandListVtbl, IASetVertexBuffers) / sizeof(void*);
extern const size_t set_stream = offsetof(ID3D12GraphicsCommandListVtbl, SOSetTargets) / sizeof(void*);
extern const size_t predication = offsetof(ID3D12GraphicsCommandListVtbl, SetPredication) / sizeof(void*);
extern const size_t compute_const =
    offsetof(ID3D12GraphicsCommandListVtbl, SetComputeRoot32BitConstant) / sizeof(void*);
extern const size_t compute_consts =
    offsetof(ID3D12GraphicsCommandListVtbl, SetComputeRoot32BitConstants) / sizeof(void*);
extern const size_t compute_cbv =
    offsetof(ID3D12GraphicsCommandListVtbl, SetComputeRootConstantBufferView) / sizeof(void*);
extern const size_t compute_srv =
    offsetof(ID3D12GraphicsCommandListVtbl, SetComputeRootShaderResourceView) / sizeof(void*);
extern const size_t compute_uav =
    offsetof(ID3D12GraphicsCommandListVtbl, SetComputeRootUnorderedAccessView) / sizeof(void*);
extern const size_t graphics_const =
    offsetof(ID3D12GraphicsCommandListVtbl, SetGraphicsRoot32BitConstant) / sizeof(void*);
extern const size_t graphics_consts =
    offsetof(ID3D12GraphicsCommandListVtbl, SetGraphicsRoot32BitConstants) / sizeof(void*);
extern const size_t graphics_cbv =
    offsetof(ID3D12GraphicsCommandListVtbl, SetGraphicsRootConstantBufferView) / sizeof(void*);
extern const size_t graphics_srv =
    offsetof(ID3D12GraphicsCommandListVtbl, SetGraphicsRootShaderResourceView) / sizeof(void*);
extern const size_t graphics_uav =
    offsetof(ID3D12GraphicsCommandListVtbl, SetGraphicsRootUnorderedAccessView) / sizeof(void*);
extern const size_t create_queue = offsetof(ID3D12DeviceVtbl, CreateCommandQueue) / sizeof(void*);
extern const size_t reset_allocator = offsetof(ID3D12CommandAllocatorVtbl, Reset) / sizeof(void*);
extern const size_t barrier = offsetof(ID3D12GraphicsCommandListVtbl, ResourceBarrier) / sizeof(void*);
extern const size_t dispatch = offsetof(ID3D12GraphicsCommandListVtbl, Dispatch) / sizeof(void*);
extern const size_t end_query = offsetof(ID3D12GraphicsCommandListVtbl, EndQuery) / sizeof(void*);
extern const size_t resolve_queries = offsetof(ID3D12GraphicsCommandListVtbl, ResolveQueryData) / sizeof(void*);
extern const size_t resize1 = offsetof(IDXGISwapChain3Vtbl, ResizeBuffers1) / sizeof(void*);
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
extern const size_t set_ctable = offsetof(ID3D12GraphicsCommandListVtbl, SetComputeRootDescriptorTable) / sizeof(void*);
extern const size_t set_croot = offsetof(ID3D12GraphicsCommandListVtbl, SetComputeRootSignature) / sizeof(void*);
extern const size_t set_groot = offsetof(ID3D12GraphicsCommandListVtbl, SetGraphicsRootSignature) / sizeof(void*);
extern const size_t set_gtable =
    offsetof(ID3D12GraphicsCommandListVtbl, SetGraphicsRootDescriptorTable) / sizeof(void*);
extern const size_t set_heaps = offsetof(ID3D12GraphicsCommandListVtbl, SetDescriptorHeaps) / sizeof(void*);
extern const size_t copy_simple = offsetof(ID3D12DeviceVtbl, CopyDescriptorsSimple) / sizeof(void*);
extern const size_t copy_ranges = offsetof(ID3D12DeviceVtbl, CopyDescriptors) / sizeof(void*);
extern const size_t make_rtv = offsetof(ID3D12DeviceVtbl, CreateRenderTargetView) / sizeof(void*);
extern const size_t make_uav = offsetof(ID3D12DeviceVtbl, CreateUnorderedAccessView) / sizeof(void*);
extern const size_t make_srv = offsetof(ID3D12DeviceVtbl, CreateShaderResourceView) / sizeof(void*);
extern const size_t make_dsv = offsetof(ID3D12DeviceVtbl, CreateDepthStencilView) / sizeof(void*);
extern const size_t make_cbv = offsetof(ID3D12DeviceVtbl, CreateConstantBufferView) / sizeof(void*);
extern const size_t create_heap = offsetof(ID3D12DeviceVtbl, CreateDescriptorHeap) / sizeof(void*);
}  // namespace port
