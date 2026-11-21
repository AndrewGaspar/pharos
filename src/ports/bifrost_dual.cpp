// The right eye uses its own mutable textures and buffers. Preserve placed-resource
// aliasing by using the same offsets on a second heap. Static assets remain shared.
#include "ports/bifrost_dual.h"
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>

#include "core/hook.h"
#include "ports/bifrost_present.h"
#include "ports/common.h"

namespace bifrost::dual {
namespace {
using Microsoft::WRL::ComPtr;
constexpr GUID tag(BYTE n) { return {0xdc31d461, 0x2704, 0x49d6, {0x9d, 0x41, 0xaa, 0x74, 0xec, 0x1b, 0xe7, n}}; }
constexpr GUID twin_tag = tag(1), addresses_tag = tag(2);
thread_local unsigned inside{};
std::mutex heaps;
template <class T>
T* twin(T* p) {
  if (!p) return nullptr;
  T* right{};
  UINT bytes = sizeof(right);
  if (FAILED(p->GetPrivateData(twin_tag, &bytes, &right)) || !right) return nullptr;
  // The caller owns the source for the duration of its D3D12 call.
  right->Release();
  return right;
}
struct Span {
  UINT64 bytes, base;
};
using Map = std::map<UINT64, Span>;
Map cpu, gpu;
using Aliases = std::multimap<UINT64, Span>;
Aliases virtual_addresses;
UINT64 largest_buffer{};
std::mutex addresses;
struct Lifetime final : IUnknown {
  std::atomic<ULONG> refs{1};
  UINT64 cpu{}, gpu{};
  Aliases::iterator buffer = virtual_addresses.end();
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (id != __uuidof(IUnknown)) return E_NOINTERFACE;
    *out = static_cast<IUnknown*>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    auto n = --refs;
    if (!n) {
      std::lock_guard guard(addresses);
      if (cpu) dual::cpu.erase(cpu);
      if (gpu) dual::gpu.erase(gpu);
      if (buffer != virtual_addresses.end()) virtual_addresses.erase(buffer);
      delete this;
    }
    return n;
  }
};
UINT64 translated(Map& map, UINT64 a) {
  if (!a) return a;
  std::lock_guard guard(addresses);
  auto i = map.upper_bound(a);
  if (i == map.begin()) return a;
  --i;
  auto delta = a - i->first;
  return delta < i->second.bytes ? i->second.base + delta : a;
}
void record(ID3D12Resource* resource, ID3D12Resource* right) {
  if (FAILED(resource->SetPrivateDataInterface(twin_tag, right))) return;
  auto desc = resource->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return;
  UINT64 va = resource->GetGPUVirtualAddress(), other = right->GetGPUVirtualAddress();
  if (!va || !other) return;
  auto* token = new Lifetime;
  if (SUCCEEDED(resource->SetPrivateDataInterface(addresses_tag, token))) {
    std::lock_guard guard(addresses);
    token->buffer = virtual_addresses.emplace(va, Span{desc.Width, other});
    largest_buffer = std::max(largest_buffer, desc.Width);
  }
  token->Release();
}
bool writable(const D3D12_RESOURCE_DESC& d) {
  return (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) != 0;
}
std::atomic<bool> failed{};
}  // namespace
void fail(const char* action, HRESULT hr) {
  if (!failed.exchange(true)) ACO_LOG("Bifrost: second-eye %s failed (0x%08lx)", action, hr);
}
bool healthy() { return !failed.load(std::memory_order_relaxed); }
bool enabled() {
  static const bool active = option_text(L"stereo", L"mode", L"off") == L"sbs" || option(L"stereo", L"dual", 0);
  return active;
}
Internal::Internal() { ++inside; }
Internal::~Internal() { --inside; }
bool game_call() { return !inside && enabled() && healthy(); }
ID3D12Resource* other(ID3D12Resource* r) {
  auto* next = twin(r);
  return next ? next : r;
}
ID3D12DescriptorHeap* other(ID3D12DescriptorHeap* h) {
  auto* next = twin(h);
  return next ? next : h;
}
D3D12_CPU_DESCRIPTOR_HANDLE other(D3D12_CPU_DESCRIPTOR_HANDLE h) { return {SIZE_T(translated(cpu, h.ptr))}; }
D3D12_GPU_DESCRIPTOR_HANDLE other(D3D12_GPU_DESCRIPTOR_HANDLE h) { return {translated(gpu, h.ptr)}; }
UINT64 address(UINT64 va) {
  if (!va) return 0;
  // Placed buffers can overlap. A smaller alias can end before the requested
  // offset even when an older, larger resource still covers it.
  std::lock_guard guard(addresses);
  auto i = virtual_addresses.upper_bound(va);
  while (i != virtual_addresses.begin()) {
    --i;
    auto offset = va - i->first;
    if (offset >= largest_buffer) break;
    if (offset < i->second.bytes) return i->second.base + offset;
  }
  return va;
}
void committed(ID3D12Device* dev, const D3D12_HEAP_PROPERTIES& hp, D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC& d,
               D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, void* object) {
  if (!game_call() || !object || !writable(d) || hp.Type == D3D12_HEAP_TYPE_UPLOAD ||
      hp.Type == D3D12_HEAP_TYPE_READBACK)
    return;
  ComPtr<ID3D12Resource> original, right;
  if (FAILED(static_cast<IUnknown*>(object)->QueryInterface(IID_PPV_ARGS(&original)))) return;
  Internal scope;
  auto hr = dev->CreateCommittedResource(&hp, flags, &d, state, clear, IID_PPV_ARGS(&right));
  if (SUCCEEDED(hr))
    record(original.Get(), right.Get());
  else
    fail("committed resource", hr);
}
void placed(ID3D12Device* dev, ID3D12Heap* heap, UINT64 off, const D3D12_RESOURCE_DESC& d, D3D12_RESOURCE_STATES state,
            const D3D12_CLEAR_VALUE* clear, void* object) {
  if (!game_call() || !object || !writable(d)) return;
  ComPtr<ID3D12Resource> original, right;
  if (FAILED(static_cast<IUnknown*>(object)->QueryInterface(IID_PPV_ARGS(&original)))) return;
  Internal scope;
  ComPtr<ID3D12Heap> paired;
  {
    std::lock_guard guard(heaps);
    paired = twin(heap);
    if (!paired) {
      auto h = heap->GetDesc();
      if (h.Properties.Type == D3D12_HEAP_TYPE_UPLOAD || h.Properties.Type == D3D12_HEAP_TYPE_READBACK) return;
      auto hr = dev->CreateHeap(&h, IID_PPV_ARGS(&paired));
      if (FAILED(hr)) {
        fail("placed heap", hr);
        return;
      }
      heap->SetPrivateDataInterface(twin_tag, paired.Get());
    }
  }
  auto hr = dev->CreatePlacedResource(paired.Get(), off, &d, state, clear, IID_PPV_ARGS(&right));
  if (SUCCEEDED(hr))
    record(original.Get(), right.Get());
  else
    fail("placed resource", hr);
}
void descriptors(ID3D12Device* dev, ID3D12DescriptorHeap* heap, const D3D12_DESCRIPTOR_HEAP_DESC& d) {
  if (!game_call() || d.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) return;
  Internal scope;
  ComPtr<ID3D12DescriptorHeap> right;
  auto hr = dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(&right));
  if (FAILED(hr)) {
    fail("descriptor heap", hr);
    return;
  }
  if (FAILED(heap->SetPrivateDataInterface(twin_tag, right.Get()))) return;
  auto* token = new Lifetime;
  if (SUCCEEDED(heap->SetPrivateDataInterface(addresses_tag, token))) {
    auto size = UINT64(d.NumDescriptors) * dev->GetDescriptorHandleIncrementSize(d.Type);
    std::lock_guard guard(addresses);
    token->cpu = heap->GetCPUDescriptorHandleForHeapStart().ptr;
    cpu[token->cpu] = {size, right->GetCPUDescriptorHandleForHeapStart().ptr};
    if (d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
      token->gpu = heap->GetGPUDescriptorHandleForHeapStart().ptr;
      gpu[token->gpu] = {size, right->GetGPUDescriptorHandleForHeapStart().ptr};
    }
  }
  token->Release();
}
void backbuffers(IDXGISwapChain* chain, ID3D12CommandQueue* queue) {
  if (!game_call()) return;
  DXGI_SWAP_CHAIN_DESC d{};
  ComPtr<ID3D12Device> dev;
  if (FAILED(chain->GetDesc(&d)) || FAILED(queue->GetDevice(IID_PPV_ARGS(&dev)))) return;
  for (UINT i = 0; i < d.BufferCount; ++i) {
    ComPtr<ID3D12Resource> bb;
    if (FAILED(chain->GetBuffer(i, IID_PPV_ARGS(&bb))) || twin(bb.Get())) continue;
    auto desc = bb->GetDesc();
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    hp.CreationNodeMask = hp.VisibleNodeMask = 1;
    Internal scope;
    ComPtr<ID3D12Resource> right;
    auto hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PRESENT, nullptr,
                                           IID_PPV_ARGS(&right));
    if (FAILED(hr)) {
      fail("backbuffer", hr);
      return;
    }
    record(bb.Get(), right.Get());
  }
}
}  // namespace bifrost::dual
