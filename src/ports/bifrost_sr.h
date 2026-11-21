#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <string>

namespace bifrost {
struct Scene;
void attach_sr(ID3D12CommandQueue* queue, UINT width, UINT height);
void sr_preset(const std::wstring& value);
void enable_sr(ID3D12CommandQueue* queue, bool enabled);
void display_size(ID3D12Device* device, UINT width, UINT height);
void prepare_output(ID3D12Resource* native, Scene& scene);
Microsoft::WRL::ComPtr<ID3D12Resource> output_for(ID3D12Resource* native);
Microsoft::WRL::ComPtr<ID3D12Resource> display_buffer(ID3D12Resource* output);
bool upscale(ID3D12CommandQueue* queue, const Scene& scene);
void finish_sr_frame(ID3D12CommandQueue* queue);
}  // namespace bifrost
