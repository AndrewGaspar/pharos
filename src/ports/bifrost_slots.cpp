#define CINTERFACE
#include <d3d12.h>

#include <cstddef>
namespace port {
extern const size_t create_root = offsetof(ID3D12DeviceVtbl, CreateRootSignature) / sizeof(void*);
extern const size_t graphics_pso = offsetof(ID3D12DeviceVtbl, CreateGraphicsPipelineState) / sizeof(void*);
extern const size_t compute_pso = offsetof(ID3D12DeviceVtbl, CreateComputePipelineState) / sizeof(void*);
extern const size_t create_list = offsetof(ID3D12DeviceVtbl, CreateCommandList) / sizeof(void*);
extern const size_t set_pso = offsetof(ID3D12GraphicsCommandListVtbl, SetPipelineState) / sizeof(void*);
extern const size_t reset_list = offsetof(ID3D12GraphicsCommandListVtbl, Reset) / sizeof(void*);
}  // namespace port
