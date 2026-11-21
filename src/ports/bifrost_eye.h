#pragma once
#include <d3d12.h>
#include <wrl/client.h>

namespace bifrost {
struct FrameInfo;
struct MarkerDepth;
enum class MarkerKind;
}  // namespace bifrost
namespace bifrost::eye {
bool enabled();
bool alternating();
// The command-list tracker stores 16 native root bindings.
constexpr UINT max_marker_camera_index = 13;
Microsoft::WRL::ComPtr<ID3DBlob> camera_root(const D3D12_ROOT_SIGNATURE_DESC& desc, UINT& index);
void tag_root(ID3D12RootSignature* root, UINT index);
UINT root_index(ID3D12RootSignature* root);
void allocator(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator);
D3D12_GPU_VIRTUAL_ADDRESS bind(ID3D12GraphicsCommandList* list, UINT64 original, UINT bytes);
void refresh();
UINT64 marker_buffer(ID3D12GraphicsCommandList* list, UINT64 eye_camera, MarkerKind kind, const MarkerDepth& depth);
void prepare(UINT count, ID3D12CommandList* const* lists, const FrameInfo* frame, int eye);
}  // namespace bifrost::eye
