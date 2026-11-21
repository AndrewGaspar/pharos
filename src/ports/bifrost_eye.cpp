#include "ports/bifrost_eye.h"

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "core/hook.h"
#include "ports/bifrost_camera.h"
#include "ports/bifrost_present.h"
#include "ports/bifrost_uploads.h"
#include "ports/common.h"

namespace port {
extern const size_t reset_allocator;
}
namespace bifrost::eye {
namespace {
using Microsoft::WRL::ComPtr;
constexpr GUID key(BYTE n) { return {0xca84218f, 0x1f50, 0x4e11, {0x80, 0xab, 0x50, 0xbb, 0xbb, 0x15, 0xf6, n}}; }
constexpr GUID root_tag = key(1), arena_tag = key(2), notes_tag = key(3);
constexpr UINT page_size = 256 * 1024;
struct Owner : IUnknown {
  std::atomic<ULONG> refs{1};
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
    if (!n) delete this;
    return n;
  }
  virtual ~Owner() = default;
};
template <class T>
ComPtr<T> associated(ID3D12Object* obj, REFGUID id) {
  ComPtr<T> result;
  UINT n = sizeof(void*);
  if (obj) obj->GetPrivateData(id, &n, result.GetAddressOf());
  return result;
}
struct Range {
  UINT bytes;
  UINT64 original, shadow;
  BYTE* mapped;
};
struct Arena final : Owner {
  struct Page {
    ComPtr<ID3D12Resource> buffer;
    BYTE* mapped{};
  };
  std::vector<Page> pages;
  unsigned cursor{};
  Range reserve(ID3D12Device* dev, UINT64 address, UINT bytes) {
    if (!bytes || bytes > 65536 || bytes % 256) return {};
    if (cursor % page_size + bytes > page_size) cursor = (cursor + page_size - 1) & ~(page_size - 1);
    unsigned i = cursor / page_size, offset = cursor % page_size;
    if (i >= pages.size()) {
      D3D12_RESOURCE_DESC d{};
      d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      d.Width = page_size;
      d.Height = 1;
      d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
      d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      D3D12_HEAP_PROPERTIES h{D3D12_HEAP_TYPE_UPLOAD};
      h.CreationNodeMask = h.VisibleNodeMask = 1;
      Page p;
      if (FAILED(dev->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(&p.buffer))))
        return {};
      D3D12_RANGE empty{};
      if (FAILED(p.buffer->Map(0, &empty, reinterpret_cast<void**>(&p.mapped)))) return {};
      pages.push_back(std::move(p));
    }
    auto& p = pages[i];
    Range r{bytes, address, p.buffer->GetGPUVirtualAddress() + offset, p.mapped + offset};
    cursor += bytes;
    return r;
  }
};
struct Notes final : Owner {
  ComPtr<Arena> arena;
  std::map<UINT64, Range> ranges;
};
HRESULT WINAPI reset(ID3D12CommandAllocator* a) {
  auto hr = aco::orig<decltype(reset)>(a, port::reset_allocator)(a);
  if (SUCCEEDED(hr))
    if (auto pool = associated<Arena>(a, arena_tag)) {
      pool->cursor = 0;
    }
  return hr;
}
}  // namespace
bool alternating() {
  static const bool on = option(L"stereo", L"preview_pair", 0) != 0;
  return on;
}
bool enabled() {
  static const auto eye = option(L"stereo", L"preview_eye", 0);
  return alternating() || eye == 1 || eye == 2;
}
ComPtr<ID3DBlob> camera_root(const D3D12_ROOT_SIGNATURE_DESC& input, UINT& index) {
  if (!enabled() || input.NumParameters > 16) return {};
  // Keep the original heap offsets. The camera descriptor moves into a root CBV,
  // so neither eye has to modify a descriptor or an upload belonging to the game.
  std::vector<D3D12_ROOT_PARAMETER> params(input.pParameters, input.pParameters + input.NumParameters);
  std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
  D3D12_ROOT_PARAMETER camera{};
  camera.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  camera.Descriptor = {0, 1};
  bool found = false;
  for (auto& p : params) {
    if (p.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) continue;
    auto& table = p.DescriptorTable;
    for (UINT i = 0; i < table.NumDescriptorRanges; ++i) {
      const auto& r = table.pDescriptorRanges[i];
      if (r.RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_CBV || r.RegisterSpace != 1 || r.BaseShaderRegister != 0) continue;
      if (found || r.NumDescriptors != 1) return {};
      ranges.assign(table.pDescriptorRanges, table.pDescriptorRanges + table.NumDescriptorRanges);
      ranges[i].RegisterSpace = 79;
      table.pDescriptorRanges = ranges.data();
      camera.ShaderVisibility = p.ShaderVisibility;
      found = true;
    }
  }
  if (!found) return {};
  index = UINT(params.size());
  params.push_back(camera);
  auto desc = input;
  desc.pParameters = params.data();
  desc.NumParameters = UINT(params.size());
  static auto fn = reinterpret_cast<decltype(&D3D12SerializeRootSignature)>(
      GetProcAddress(port::system_library(L"d3d12.dll"), "D3D12SerializeRootSignature"));
  ComPtr<ID3DBlob> code, errors;
  if (!fn || FAILED(fn(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &code, &errors))) return {};
  return code;
}
void tag_root(ID3D12RootSignature* root, UINT index) { root->SetPrivateData(root_tag, sizeof(index), &index); }
UINT root_index(ID3D12RootSignature* root) {
  UINT index{}, bytes = sizeof(index);
  return root && SUCCEEDED(root->GetPrivateData(root_tag, &bytes, &index)) ? index : UINT_MAX;
}
void allocator(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* a) {
  if (!enabled() || !a) return;
  auto arena = associated<Arena>(a, arena_tag);
  if (!arena) {
    arena.Attach(new Arena);
    a->SetPrivateDataInterface(arena_tag, arena.Get());
    aco::hook_vtable(a, port::reset_allocator, reinterpret_cast<void*>(reset));
  }
  auto* notes = new Notes;
  notes->arena = arena;
  list->SetPrivateDataInterface(notes_tag, notes);
  notes->Release();
}
UINT64 bind(ID3D12GraphicsCommandList* list, UINT64 address, UINT bytes) {
  if (!address || bytes < 82 * 16) return address;
  auto notes = associated<Notes>(list, notes_tag);
  if (!notes) return address;
  if (auto i = notes->ranges.find(address); i != notes->ranges.end() && i->second.bytes == bytes)
    return i->second.shadow;
  auto c = read_constants(address, bytes);
  if (c.empty()) return address;
  ComPtr<ID3D12Device> dev;
  if (FAILED(list->GetDevice(IID_PPV_ARGS(&dev)))) return address;
  auto r = notes->arena->reserve(dev.Get(), address, bytes);
  if (!r.shadow) return address;
  memcpy(r.mapped, c.data(), bytes);
  notes->ranges[address] = r;
  return r.shadow;
}
void prepare(UINT count, ID3D12CommandList* const* lists, const float* camera, int eye) {
  if (!enabled()) return;
  static const float metres = option(L"stereo", L"separation_mm", 33) * .001f;
  static const int single = option(L"stereo", L"preview_eye", 2) == 1 ? 0 : 1;
  const float separation = (alternating() ? eye : single) == 0 ? -metres : metres;
  static const float convergence = float(option(L"stereo", L"convergence", 4));
  unsigned moved = 0;
  std::map<UINT64, Range> ranges;
  for (UINT i = 0; i < count; ++i) {
    if (auto notes = associated<Notes>(lists[i], notes_tag))
      for (const auto& [_, r] : notes->ranges) ranges.try_emplace(r.shadow, r);
  }
  for (const auto& [_, r] : ranges) {
    auto c = read_constants(r.original, r.bytes);
    if (c.empty()) continue;
    moved += translate_camera(c.data(), r.bytes, camera, separation, convergence);
    memcpy(r.mapped, c.data(), r.bytes);
  }
  if (camera) {
    static std::atomic<unsigned> frames{};
    auto seq = ++frames;
    if (seq <= 3 || seq % 300 == 0)
      ACO_LOG("Bifrost: eye preview #%u moved %u of %zu camera ranges by %.3f m", seq, moved, ranges.size(),
              separation);
  }
}
}  // namespace bifrost::eye
