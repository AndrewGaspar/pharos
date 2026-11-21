#include "ports/bifrost_eye.h"

#include <atomic>
#include <cmath>
#include <cwchar>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "core/hook.h"
#include "ports/bifrost_camera.h"
#include "ports/bifrost_present.h"
#include "ports/bifrost_uploads.h"
#include "ports/bifrost_resources.h"
#include "ports/common.h"
#include "ports/bifrost_dual.h"
#include "ports/bifrost_lists.h"
#include "ports/bifrost_markers.h"
#include "ports/bifrost_scene.h"
#include "world/marker_targets.h"

namespace port {
extern const size_t reset_allocator;
}
namespace bifrost::eye {
namespace {
using Microsoft::WRL::ComPtr;
constexpr GUID key(BYTE n) { return {0xca84218f, 0x1f50, 0x4e11, {0x80, 0xab, 0x50, 0xbb, 0xbb, 0x15, 0xf6, n}}; }
constexpr GUID root_tag = key(1), arena_tag = key(2), notes_tag = key(3);
float metres = .0325f, convergence = 4;
std::atomic<int> aim_bias{1}; // +1 anchors the left eye, -1 the right.
std::atomic<bool> markers_enabled{true}, cursor_enabled{true};
float number(const std::wstring& text, float fallback) {
  wchar_t* end{};
  float value = std::wcstof(text.c_str(), &end);
  return end != text.c_str() && !*end && std::isfinite(value) && value >= 0 ? value : fallback;
}
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
  std::map<ID3D12Resource*, ComPtr<ID3D12Resource>> depth_buffers;
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
  struct Marker {
    Range constants, camera;
    UINT dimensions[4];
    MarkerKind kind;
  };
  std::map<std::tuple<UINT64, ID3D12Resource*, MarkerKind>, Marker> markers;
};
HRESULT WINAPI reset(ID3D12CommandAllocator* a) {
  auto hr = aco::orig<decltype(reset)>(a, port::reset_allocator)(a);
  if (SUCCEEDED(hr) && dual::enabled()) dual::reset_allocator(a);
  if (SUCCEEDED(hr))
    if (auto pool = associated<Arena>(a, arena_tag)) {
      pool->cursor = 0;
      pool->depth_buffers.clear();
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
  return dual::enabled() || alternating() || eye == 1 || eye == 2;
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
  if (dual::enabled() && index <= max_marker_camera_index) {
    D3D12_ROOT_PARAMETER marker{};
    marker.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    marker.Descriptor = {0, 80};
    marker.ShaderVisibility = D3D12_SHADER_VISIBILITY_GEOMETRY;
    params.push_back(marker);
    marker.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params.push_back(marker);
    desc.Flags &= ~D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
  }
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
bool right_depth_eye() { return aim_bias.load(std::memory_order_relaxed) < 0; }
void refresh() {
  // Queue submissions are serialized. Read both eye settings together so they
  // take effect for the same simulation step.
  static ULONGLONG next{};
  auto now = GetTickCount64();
  if (now < next) return;
  next = now + 1000;
  auto distance = option_text(L"stereo", L"separation", L"");
  if (distance.empty()) distance = option_text(L"stereo", L"separation_mm", L"32.5");
  metres = .001f * number(distance, 32.5f);
  convergence = number(option_text(L"stereo", L"convergence", L"4"), 4);
  auto aim = option_text(L"stereo", L"aim_eye", L"left");
  aim_bias = aim == L"right" ? -1 : aim == L"center" ? 0 : 1;
  markers_enabled = option(L"stereo", L"markers", 1) != 0;
  cursor_enabled = option_text(L"stereo", L"cursor", L"depth") == L"depth";
}
struct Vec4 {
  float x, y, z, w;
};
struct MarkerBlock {
  float clip_z[4], clip_w[4];
  Vec4 scale;
  UINT dimensions[4], count;
  float ray_scale;
  UINT padding[2];
  Vec4 targets[aco::kMaxMarkerTargets];
};
UINT64 marker_buffer(ID3D12GraphicsCommandList* list, UINT64 camera, MarkerKind kind, const MarkerDepth& depth) {
  if (!camera || !(kind == MarkerKind::MapCursor ? cursor_enabled : markers_enabled).load(std::memory_order_relaxed) ||
      !depth.buffer)
    return 0;
  auto notes = associated<Notes>(list, notes_tag);
  if (!notes) return 0;
  const auto key = std::make_tuple(camera, depth.buffer.Get(), kind);
  if (auto it = notes->markers.find(key); it != notes->markers.end()) return it->second.constants.shadow;
  Range view{};
  for (const auto& [_, r] : notes->ranges)
    if (r.shadow == camera) {
      view = r;
      break;
    }
  if (!view.original) return 0;
  ComPtr<ID3D12Device> dev;
  if (FAILED(list->GetDevice(IID_PPV_ARGS(&dev)))) return 0;
  auto mem = notes->arena->reserve(dev.Get(), 0, (sizeof(MarkerBlock) + 255u) & ~255u);
  if (!mem.shadow) return 0;
  memset(mem.mapped, 0, sizeof(MarkerBlock));
  notes->arena->depth_buffers.try_emplace(depth.buffer.Get(), depth.buffer);
  notes->markers.emplace(key,
                         Notes::Marker{mem, view, {depth.width, depth.height, depth.pitch, depth.slice_bytes}, kind});
  return mem.shadow;
}
void fill_markers(const Notes::Marker& marker, const FrameInfo& frame, const float* view, float separation, float ray_offset, bool first_person) {
  MarkerBlock block{};
  const auto& inv = frame.camera.clip_to_view;
  if (frame.camera.valid && std::abs(inv[0][0]) > .001f) {
    memcpy(block.clip_z, inv[2], 16);
    memcpy(block.clip_w, inv[3], 16);
    memcpy(block.dimensions, marker.dimensions, sizeof(block.dimensions));
    const float scale = separation / std::abs(inv[0][0]);
    block.ray_scale = ray_offset / std::abs(inv[0][0]);
    block.scale = {0, scale, convergence > 0 ? 1 / convergence : 0, float(marker.kind)};
    if (marker.kind == MarkerKind::BowDot && !first_person && !bow_bars_present()) block.scale.w = 0;
    if (view) {
      auto original = read_constants(marker.camera.original, marker.camera.bytes);
      if (scene_camera_matches(original.data(), UINT(original.size() * sizeof(float)), view))
        block.scale.x = separation * original[26 * 4];
    }
    if (marker.kind == MarkerKind::World) {
      aco::MarkerTarget points[aco::kMaxMarkerTargets];
      size_t n = aco::marker_targets_read(points);
      const auto& vp = frame.world_to_clip;
      for (size_t i = 0; i < n; ++i) {
        const auto& p = points[i];
        auto project = [&](unsigned row) {
          return vp[row][0] * p.x + vp[row][1] * p.y + vp[row][2] * p.z + vp[row][3];
        };
        const float w = project(3);
        if (!(w > .05f)) continue;
        const float y = project(1) / w, raised = w + 2 * vp[3][2];
        const float lift = raised > .05f ? (project(1) + 2 * vp[1][2]) / raised - y : 0;
        block.targets[block.count++] = {project(0) / w, y, 1 / w, lift};
      }
    }
  }
  memcpy(marker.constants.mapped, &block, sizeof(block));
}
void prepare(UINT count, ID3D12CommandList* const* lists, const FrameInfo* frame, int eye) {
  if (!enabled()) return;
  static FrameInfo latest;
  if (frame) latest = *frame;
  const float* camera = frame ? frame->camera.view_to_world[0] : nullptr;
  static const int single = option(L"stereo", L"preview_eye", 2) == 1 ? 0 : 1;
  int which = alternating() || dual::enabled() ? eye : single;
  // The first-person Needler camera has an 18-degree vertical FOV and
  // a 0.10 m near plane. The raven uses a 30-degree FOV and 0.80 m.
  const auto& inv = latest.camera.clip_to_view;
  const bool first_person = dual::enabled() && bow_dot_present() && latest.camera.valid &&
                            inv[1][1] > .05f && inv[1][1] < .21f && std::abs(inv[3][2] - 10.f) < .5f;
  const int bias = first_person ? aim_bias.load(std::memory_order_relaxed) : 0;
  const float spacing = dual::healthy() ? metres : 0;
  const float separation = spacing * ((which == 0 ? -1 : 1) + bias);
  const float ray_offset = spacing * ((right_depth_eye() ? 1 : -1) + bias);
  unsigned moved = 0;
  std::map<UINT64, Range> ranges;
  for (UINT i = 0; i < count; ++i) {
    if (auto notes = associated<Notes>(lists[i], notes_tag)) {
      for (const auto& [_, r] : notes->ranges) ranges.try_emplace(r.shadow, r);
      for (const auto& [_, m] : notes->markers) fill_markers(m, latest, camera, separation, ray_offset, first_person);
    }
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
      ACO_LOG("Bifrost: eye camera #%u moved %u of %zu camera ranges by %.3f m", seq, moved, ranges.size(),
              separation);
  }
}
}  // namespace bifrost::eye
