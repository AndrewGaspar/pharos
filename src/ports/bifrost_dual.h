#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace bifrost::dual {
bool enabled();
bool healthy();
void fail(const char* action, HRESULT result);
// Native calls made by the compositor or NGX must not acquire a second copy.
struct Internal {
  Internal();
  ~Internal();
  Internal(const Internal&) = delete;
  Internal& operator=(const Internal&) = delete;
};
bool game_call();
void committed(ID3D12Device* dev, const D3D12_HEAP_PROPERTIES& hp, D3D12_HEAP_FLAGS flags,
               const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
               void* original);
void placed(ID3D12Device* dev, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC& desc,
            D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, void* original);
void backbuffers(IDXGISwapChain* chain, ID3D12CommandQueue* queue);
void descriptors(ID3D12Device* dev, ID3D12DescriptorHeap* heap, const D3D12_DESCRIPTOR_HEAP_DESC& desc);
ID3D12Resource* other(ID3D12Resource* resource);
ID3D12DescriptorHeap* other(ID3D12DescriptorHeap* heap);
D3D12_CPU_DESCRIPTOR_HANDLE other(D3D12_CPU_DESCRIPTOR_HANDLE handle);
D3D12_GPU_DESCRIPTOR_HANDLE other(D3D12_GPU_DESCRIPTOR_HANDLE handle);
UINT64 address(UINT64 va);
}  // namespace bifrost::dual
