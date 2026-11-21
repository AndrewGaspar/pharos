#pragma once
#include <d3d12.h>

#include <vector>
namespace bifrost {
void observe_uploads(ID3D12Device* device);
std::vector<float> read_constants(D3D12_GPU_VIRTUAL_ADDRESS address, UINT bytes);
}  // namespace bifrost
