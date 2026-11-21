#pragma once
#include <d3d12.h>
#include <wrl/client.h>

namespace bifrost {
// Three consecutive inputs to the physical presentation queue in an SDR verification run.
class VerifyFrames {
 public:
  bool begin(ID3D12GraphicsCommandList* list, ID3D12Resource* previous);
  void finish(ID3D12GraphicsCommandList* list, ID3D12Resource* generated, ID3D12Resource* current);
  void report();

 private:
  void copy(ID3D12GraphicsCommandList* list, ID3D12Resource* image, D3D12_RESOURCE_STATES state, unsigned index);
  Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 stride{};
};
}  // namespace bifrost
