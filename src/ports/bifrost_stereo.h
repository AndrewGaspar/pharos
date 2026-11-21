#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>

namespace bifrost {
class StereoOutput {
 public:
  ~StereoOutput();
  bool drain();
  void invalidate() { valid = 0; }
  // eye==-1 duplicates the live flat image and drops the previous perspective pair.
  bool present(IDXGISwapChain* chain, ID3D12CommandQueue* queue, int eye, UINT64 synchronized_target = 0);

 private:
  template <class T>
  using Ptr = Microsoft::WRL::ComPtr<T>;
  struct Slot {
    Ptr<ID3D12CommandAllocator> allocator;
    Ptr<ID3D12GraphicsCommandList> list;
    UINT64 done{};
  };
  std::array<Slot, 4> slots;
  Ptr<ID3D12RootSignature> root;
  Ptr<ID3D12PipelineState> pso;
  Ptr<ID3D12Fence> fence;
  Ptr<ID3D12DescriptorHeap> srv, rtv;
  Ptr<ID3D12Resource> views[2];
  UINT width{}, height{}, stride{}, slot{};
  DXGI_FORMAT format{};
  UINT64 last{};
  unsigned valid{}, pairs{};
  UINT show{}, swap{};
  float difference_gain = 16;
  ULONGLONG next_settings{};
  bool open(ID3D12Device* dev, const D3D12_RESOURCE_DESC& desc);
};
}  // namespace bifrost
