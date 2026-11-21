#include "ports/bifrost_uploads.h"

#include <wrl/client.h>

#include <atomic>
#include <map>
#include <mutex>

#include "core/hook.h"
#include "ports/bifrost_dual.h"
namespace port {
extern const size_t committed, placed;
}
namespace bifrost {
namespace {
const GUID lifetime_tag{0xb42406fe, 0xea61, 0x44b9, {0x80, 0x11, 0x89, 0x11, 0x2d, 0xca, 0x6a, 0xba}};
struct Upload;
std::mutex guard;
std::multimap<UINT64, Upload*> buffers;
struct Upload final : IUnknown {
  std::atomic<ULONG> refs{1};
  const UINT64 start, size;
  ID3D12Resource* const owner;
  Upload(UINT64 base, UINT64 bytes, ID3D12Resource* resource) : start(base), size(bytes), owner(resource) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
    *out = static_cast<IUnknown*>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    auto n = --refs;
    if (n == 0) {
      std::lock_guard lock(guard);
      auto [first, end] = buffers.equal_range(start);
      for (auto i = first; i != end; ++i)
        if (i->second == this) {
          buffers.erase(i);
          break;
        }
      delete this;
    }
    return n;
  }
};
void remember(const D3D12_RESOURCE_DESC* desc, const D3D12_HEAP_PROPERTIES& properties, void* object) {
  if (!object || !desc || desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
      (properties.Type != D3D12_HEAP_TYPE_UPLOAD &&
       properties.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE &&
       properties.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_WRITE_BACK))
    return;
  Microsoft::WRL::ComPtr<ID3D12Resource> res;
  if (FAILED(static_cast<IUnknown*>(object)->QueryInterface(IID_PPV_ARGS(&res)))) return;
  const auto va = res->GetGPUVirtualAddress();
  if (!va) return;
  auto* token = new Upload(va, desc->Width, res.Get());
  if (SUCCEEDED(res->SetPrivateDataInterface(lifetime_tag, token))) {
    std::lock_guard lock(guard);
    buffers.emplace(va, token);
  }
  token->Release();
}
HRESULT WINAPI committed(ID3D12Device* dev, const D3D12_HEAP_PROPERTIES* props, D3D12_HEAP_FLAGS flags,
                         const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
                         REFIID iid, void** out) {
  auto hr = aco::orig<decltype(committed)>(dev, port::committed)(dev, props, flags, desc, state, clear, iid, out);
  if (SUCCEEDED(hr) && out && props && desc) {
    remember(desc, *props, *out);
    dual::committed(dev, *props, flags, *desc, state, clear, *out);
  }
  return hr;
}
HRESULT WINAPI placed(ID3D12Device* dev, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC* desc,
                      D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID iid, void** out) {
  auto hr = aco::orig<decltype(placed)>(dev, port::placed)(dev, heap, offset, desc, state, clear, iid, out);
  if (SUCCEEDED(hr) && out && heap && desc) dual::placed(dev, heap, offset, *desc, state, clear, *out);
  if (SUCCEEDED(hr) && out && heap && desc && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    remember(desc, heap->GetDesc().Properties, *out);
  return hr;
}
}  // namespace
void observe_uploads(ID3D12Device* dev) {
  aco::hook_vtable(dev, port::committed, reinterpret_cast<void*>(committed));
  aco::hook_vtable(dev, port::placed, reinterpret_cast<void*>(placed));
}
std::vector<float> read_constants(D3D12_GPU_VIRTUAL_ADDRESS address, UINT bytes) {
  if (!address || !bytes || bytes > 65536 || bytes % sizeof(float)) return {};
  std::lock_guard lock(guard);
  auto it = buffers.upper_bound(address);
  if (it == buffers.begin()) return {};
  const auto* b = std::prev(it)->second;
  const UINT64 offset = address - b->start;
  if (offset > b->size || bytes > b->size - offset) return {};
  D3D12_RANGE read{SIZE_T(offset), SIZE_T(offset + bytes)}, written{};
  void* mapped = nullptr;
  if (FAILED(b->owner->Map(0, &read, &mapped))) return {};
  std::vector<float> values(bytes / sizeof(float));
  memcpy(values.data(), static_cast<char*>(mapped) + offset, bytes);
  b->owner->Unmap(0, &written);
  return values;
}
}  // namespace bifrost
