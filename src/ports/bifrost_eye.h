#pragma once
#include <d3d12.h>
#include <wrl/client.h>

namespace bifrost::eye {
bool enabled();
bool alternating();
Microsoft::WRL::ComPtr<ID3DBlob> camera_root(const D3D12_ROOT_SIGNATURE_DESC& desc, UINT& index);
void tag_root(ID3D12RootSignature* root, UINT index);
UINT root_index(ID3D12RootSignature* root);
void allocator(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator);
D3D12_GPU_VIRTUAL_ADDRESS bind(ID3D12GraphicsCommandList* list, UINT64 original, UINT bytes);
void prepare(UINT count, ID3D12CommandList* const* lists, const float* camera, int eye);
}  // namespace bifrost::eye
