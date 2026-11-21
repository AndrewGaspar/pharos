#include "ports/bifrost_uploads.h"
// Follow the game's CPU descriptor copies and bound GPU tables to the inputs of
// each pass.
#include <wrl/client.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_set>

#include "core/hook.h"
#include "ports/bifrost_present.h"
#include "ports/bifrost_resources.h"
#include "ports/bifrost_scene.h"

using aco::orig;
using Microsoft::WRL::ComPtr;
namespace port {
extern const size_t topology, viewports, scissors;
extern const size_t create_heap, make_srv, make_uav, make_cbv, make_rtv, copy_ranges, copy_simple, set_heaps,
    set_gtable, set_groot, set_targets, draw, draw_indexed, indirect, clear_state;
}  // namespace port
namespace bifrost {
namespace {
const GUID layout_tag{0xab064932, 0x44f0, 0x4321, {0xb2, 0x88, 0x11, 0x54, 0x26, 0xd8, 0x78, 0x90}};
const GUID list_tag{0xb71ab932, 0x7635, 0x4a01, {0xb1, 0x88, 0x11, 0x54, 0x26, 0xd8, 0x78, 0x91}};
const GUID pass_tag{0xb4102301, 0x98f0, 0x4391, {0xb2, 0x88, 0x11, 0x54, 0x26, 0xd8, 0x78, 0x92}};

struct Slot {
  UINT root{}, offset{}, count{};
};
struct Table {
  Slot cbv, srv;
};
struct Layout {
  Table pass, temporal, composite;
};
struct Pass {
  Table camera, resources;
  port::Entry* entry{};
};
struct Heap {
  UINT64 gpu{}, cpu{}, length{};
  UINT stride{};
};
struct Bindings {
  Pass pass;
  Heap heap;
  ID3D12DescriptorHeap* sampler{};
  UINT64 roots[16]{};
  ID3D12RootSignature* signature{};
  DrawState draw;
};
struct View {
  // Identity only. The metadata survives descriptor copies without owning a
  // game resource.
  UINT64 id{}, width{};
  UINT height{};
  DXGI_FORMAT format{};
  char kind{};
};
std::map<UINT64, View> descriptors;
std::mutex lock;
std::unordered_set<port::Entry*> sampled;

template <class Obj, class T>
T get(Obj* o, REFGUID key) {
  T result{};
  UINT size = sizeof(T);
  if (!o || FAILED(o->GetPrivateData(key, &size, &result)) || size != sizeof(T)) return {};
  return result;
}
void save(ID3D12GraphicsCommandList* list, const Bindings& state) {
  list->SetPrivateData(list_tag, sizeof(state), &state);
}
template <class Desc>
Layout ranges(const Desc& desc) {
  Layout l{};
  for (UINT i = 0; i < desc.NumParameters && i < 16; ++i) {
    const auto& p = desc.pParameters[i];
    if (p.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ||
        (p.ShaderVisibility != D3D12_SHADER_VISIBILITY_ALL && p.ShaderVisibility != D3D12_SHADER_VISIBILITY_PIXEL))
      continue;
    UINT offset = 0;
    for (UINT j = 0; j < p.DescriptorTable.NumDescriptorRanges; ++j) {
      const auto& r = p.DescriptorTable.pDescriptorRanges[j];
      if (r.OffsetInDescriptorsFromTableStart != D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)
        offset = r.OffsetInDescriptorsFromTableStart;
      Table* t = r.RegisterSpace == 1   ? &l.pass
                 : r.RegisterSpace == 2 ? &l.temporal
                 : r.RegisterSpace == 4 ? &l.composite
                                        : nullptr;
      if (t && r.BaseShaderRegister == 0) {
        if (r.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV) t->srv = {i, offset, r.NumDescriptors};
        if (r.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) t->cbv = {i, offset, r.NumDescriptors};
      }
      offset += r.NumDescriptors;
    }
  }
  return l;
}
View describe(ID3D12Resource* res, DXGI_FORMAT format, char kind) {
  if (!res) return {};
  auto d = res->GetDesc();
  return {reinterpret_cast<UINT64>(res), d.Width, d.Height, format == DXGI_FORMAT_UNKNOWN ? d.Format : format, kind};
}
void write_descriptor(D3D12_CPU_DESCRIPTOR_HANDLE dest, const View& v) {
  std::lock_guard guard(lock);
  if (v.kind)
    descriptors[dest.ptr] = v;
  else
    descriptors.erase(dest.ptr);
}
void WINAPI srv(ID3D12Device* dev, ID3D12Resource* res, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                D3D12_CPU_DESCRIPTOR_HANDLE dest) {
  orig<decltype(srv)>(dev, port::make_srv)(dev, res, desc, dest);
  write_descriptor(dest, describe(res, desc ? desc->Format : DXGI_FORMAT_UNKNOWN, 't'));
}
void WINAPI rtv(ID3D12Device* dev, ID3D12Resource* res, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                D3D12_CPU_DESCRIPTOR_HANDLE dest) {
  orig<decltype(rtv)>(dev, port::make_rtv)(dev, res, desc, dest);
  write_descriptor(dest, describe(res, desc ? desc->Format : DXGI_FORMAT_UNKNOWN, 'r'));
}
void WINAPI cbv(ID3D12Device* dev, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
  orig<decltype(cbv)>(dev, port::make_cbv)(dev, desc, dest);
  write_descriptor(dest, desc ? View{desc->BufferLocation, desc->SizeInBytes, 1, DXGI_FORMAT_UNKNOWN, 'b'} : View{});
}
void WINAPI uav(ID3D12Device* dev, ID3D12Resource* res, ID3D12Resource* counter,
                const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
  orig<decltype(uav)>(dev, port::make_uav)(dev, res, counter, desc, dest);
  write_descriptor(dest, describe(res, desc ? desc->Format : DXGI_FORMAT_UNKNOWN, 'u'));
}
bool tracked(D3D12_DESCRIPTOR_HEAP_TYPE t) {
  return t == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || t == D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
}
HRESULT WINAPI create_heap(ID3D12Device* dev, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID iid, void** out) {
  auto hr = orig<decltype(create_heap)>(dev, port::create_heap)(dev, desc, iid, out);
  ComPtr<ID3D12DescriptorHeap> heap;
  if (SUCCEEDED(hr) && desc && tracked(desc->Type) && out && *out &&
      SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&heap)))) {
    auto start = heap->GetCPUDescriptorHandleForHeapStart().ptr;
    auto bytes = UINT64(desc->NumDescriptors) * dev->GetDescriptorHandleIncrementSize(desc->Type);
    std::lock_guard guard(lock);
    descriptors.erase(descriptors.lower_bound(start), descriptors.lower_bound(start + bytes));
  }
  return hr;
}
void copy(ID3D12Device* dev, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* dst, const UINT* dl, UINT ns,
          const D3D12_CPU_DESCRIPTOR_HANDLE* src, const UINT* sl, D3D12_DESCRIPTOR_HEAP_TYPE type) {
  if (!tracked(type)) return;
  UINT stride = dev->GetDescriptorHandleIncrementSize(type);
  std::lock_guard guard(lock);
  UINT d = 0, s = 0, di = 0, si = 0;
  while (d < nd && s < ns) {
    if (di == (dl ? dl[d] : 1)) {
      ++d;
      di = 0;
      continue;
    }
    if (si == (sl ? sl[s] : 1)) {
      ++s;
      si = 0;
      continue;
    }
    auto dest = dst[d].ptr + UINT64(di++) * stride;
    auto it = descriptors.find(src[s].ptr + UINT64(si++) * stride);
    if (it != descriptors.end())
      descriptors[dest] = it->second;
    else
      descriptors.erase(dest);
  }
}
void WINAPI copy_ranges(ID3D12Device* dev, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* dst, const UINT* dl, UINT ns,
                        const D3D12_CPU_DESCRIPTOR_HANDLE* src, const UINT* sl, D3D12_DESCRIPTOR_HEAP_TYPE type) {
  orig<decltype(copy_ranges)>(dev, port::copy_ranges)(dev, nd, dst, dl, ns, src, sl, type);
  copy(dev, nd, dst, dl, ns, src, sl, type);
}
void WINAPI simple(ID3D12Device* dev, UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src,
                   D3D12_DESCRIPTOR_HEAP_TYPE type) {
  orig<decltype(simple)>(dev, port::copy_simple)(dev, count, dst, src, type);
  copy(dev, 1, &dst, &count, 1, &src, &count, type);
}
void WINAPI set_heaps(ID3D12GraphicsCommandList* list, UINT count, ID3D12DescriptorHeap* const* heaps) {
  orig<decltype(set_heaps)>(list, port::set_heaps)(list, count, heaps);
  auto st = get<ID3D12GraphicsCommandList, Bindings>(list, list_tag);
  Heap h{};
  ID3D12DescriptorHeap* sampler = nullptr;
  for (UINT i = 0; i < count; ++i) {
    auto d = heaps[i]->GetDesc();
    if (d.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) sampler = heaps[i];
    if (d.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) continue;
    ComPtr<ID3D12Device> dev;
    if (FAILED(heaps[i]->GetDevice(IID_PPV_ARGS(&dev)))) continue;
    UINT stride = dev->GetDescriptorHandleIncrementSize(d.Type);
    h = {heaps[i]->GetGPUDescriptorHandleForHeapStart().ptr, heaps[i]->GetCPUDescriptorHandleForHeapStart().ptr,
         UINT64(stride) * d.NumDescriptors, stride};
  }
  if (st.heap.gpu != h.gpu || st.sampler != sampler) std::fill(std::begin(st.roots), std::end(st.roots), 0);
  st.sampler = sampler;
  st.heap = h;
  save(list, st);
}
void WINAPI table(ID3D12GraphicsCommandList* list, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) {
  orig<decltype(table)>(list, port::set_gtable)(list, i, h);
  if (i >= 16) return;
  auto st = get<ID3D12GraphicsCommandList, Bindings>(list, list_tag);
  st.roots[i] = h.ptr;
  save(list, st);
}
void WINAPI root(ID3D12GraphicsCommandList* list, ID3D12RootSignature* sig) {
  orig<decltype(root)>(list, port::set_groot)(list, sig);
  auto st = get<ID3D12GraphicsCommandList, Bindings>(list, list_tag);
  if (st.signature != sig) {
    std::fill(std::begin(st.roots), std::end(st.roots), 0);
    st.signature = sig;
    st.draw.signature = sig;
    save(list, st);
  }
}
void WINAPI targets(ID3D12GraphicsCommandList* list, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* handles,
                    BOOL contiguous, const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
  orig<decltype(targets)>(list, port::set_targets)(list, n, handles, contiguous, depth);
  auto st = get<ID3D12GraphicsCommandList, Bindings>(list, list_tag);
  ComPtr<ID3D12Device> dev;
  UINT stride = 0;
  if (contiguous && n > 1 && SUCCEEDED(list->GetDevice(IID_PPV_ARGS(&dev))))
    stride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  st.draw.num_targets = std::min(n, 8u);
  st.draw.depth = depth ? *depth : D3D12_CPU_DESCRIPTOR_HANDLE{};
  for (UINT i = 0; i < 8; ++i)
    st.draw.targets[i].ptr = i >= n ? 0 : contiguous ? handles[0].ptr + i * stride : handles[i].ptr;
  save(list, st);
}
UINT64 address(const Bindings& st, Slot slot, UINT reg) {
  if (reg >= slot.count) return 0;
  auto offset = st.roots[slot.root] - st.heap.gpu;
  if (offset >= st.heap.length) return 0;
  offset += (UINT64(slot.offset) + reg) * st.heap.stride;
  return offset < st.heap.length ? st.heap.cpu + offset : 0;
}
void WINAPI topology(ID3D12GraphicsCommandList* list, D3D12_PRIMITIVE_TOPOLOGY t) {
  orig<decltype(topology)>(list, port::topology)(list, t);
  auto st = get<ID3D12GraphicsCommandList, Bindings>(list, list_tag);
  st.draw.topology = t;
  save(list, st);
}
void WINAPI viewports(ID3D12GraphicsCommandList* list, UINT n, const D3D12_VIEWPORT* v) {
  orig<decltype(viewports)>(list, port::viewports)(list, n, v);
  auto st = get<ID3D12GraphicsCommandList, Bindings>(list, list_tag);
  st.draw.viewport_count = std::min(n, UINT(std::size(st.draw.viewports)));
  std::copy_n(v, st.draw.viewport_count, st.draw.viewports);
  save(list, st);
}
void WINAPI scissors(ID3D12GraphicsCommandList* list, UINT n, const D3D12_RECT* v) {
  orig<decltype(scissors)>(list, port::scissors)(list, n, v);
  auto st = get<ID3D12GraphicsCommandList, Bindings>(list, list_tag);
  st.draw.scissor_count = std::min(n, UINT(std::size(st.draw.scissors)));
  std::copy_n(v, st.draw.scissor_count, st.draw.scissors);
  save(list, st);
}
void inspect(ID3D12GraphicsCommandList* list) {
  auto st = get<ID3D12GraphicsCommandList, Bindings>(list, list_tag);
  auto e = st.pass.entry;
  if (!e) return;
  std::unique_lock guard(lock);
  if (e->role == "PS/temporalaa.hlsl/PS_TemporalAA" &&
      (e->detail.starts_with("TemporalAATilesColorBoxYCoCgClampNoUpsample,") ||
       e->detail.starts_with("TemporalAATilesColorBoxYCoCgClamp,"))) {
    auto read = [&](Slot slot) {
      auto i = descriptors.find(address(st, slot, 0));
      return i != descriptors.end() && i->second.kind == 'b' ? read_constants(i->second.id, UINT(i->second.width))
                                                             : std::vector<float>{};
    };
    auto camera = read(st.pass.camera.cbv), temporal = read(st.pass.resources.cbv);
    const auto scene = descriptors.find(address(st, st.pass.resources.srv, 0));
    if (scene != descriptors.end() && scene->second.kind == 't' && camera.size() >= 82 * 4 &&
        temporal.size() >= 7 * 4) {
      FrameInfo info{};
      info.width = UINT(temporal[8]);
      info.height = UINT(temporal[9]);
      if (info.width && info.width <= scene->second.width && info.height && info.height <= scene->second.height) {
        info.jitter[0] = temporal[12];
        info.jitter[1] = temporal[13];
        memcpy(info.camera.view_to_world, camera.data() + 6 * 4, sizeof(info.camera.view_to_world));
        memcpy(info.camera.clip_to_view, camera.data() + 69 * 4, sizeof(info.camera.clip_to_view));
        memcpy(info.camera.prev_view_proj, camera.data() + 38 * 4, sizeof(info.camera.prev_view_proj));
        info.camera.valid = true;
        guard.unlock();
        record_frame_size(list, info.width, info.height);
        record_scene(list, st.draw, info);
        guard.lock();
      }
    }
    static ULONGLONG next = 0;
    static bool done = false;
    const auto now = GetTickCount64();
    if (!done && now >= next) {
      next = now + 500;
      static const auto trigger = aco::out_dir() + L"\\capture-camera";
      if (GetFileAttributesW(trigger.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (!camera.empty() && !temporal.empty()) {
          auto file = _wfopen((aco::out_dir() + L"\\bifrost-camera.tsv").c_str(), L"w");
          if (file) {
            auto dump = [&](const char* name, const std::vector<float>& v) {
              for (size_t i = 0; i + 3 < v.size(); i += 4)
                fprintf(file, "%s\t%zu\t%.9g\t%.9g\t%.9g\t%.9g\n", name, i / 4, v[i], v[i + 1], v[i + 2], v[i + 3]);
            };
            dump("camera", camera);
            dump("temporal", temporal);
            done = fclose(file) == 0;
            ACO_LOG(
                "Bifrost: read camera %zu and temporal %zu floats from the "
                "active GPU upload ranges",
                camera.size(), temporal.size());
          }
        }
      }
    }
  }
  if (e->role == "PS/uicompositing.hlsl/PS_ShadePixel" && e->detail.starts_with("CompositeTextureInvSrcAlpha,")) {
    auto scene = descriptors.find(address(st, st.pass.resources.srv, 1)),
         out = descriptors.find(st.draw.targets[0].ptr);
    if (scene != descriptors.end() && out != descriptors.end())
      record_composite(list, scene->second.id, out->second.id);
  }
  if (sampled.count(e)) return;
  std::ostringstream s;
  auto append = [&](UINT64 addr, const char* name, UINT slot, char kind) {
    auto it = descriptors.find(addr);
    if (it == descriptors.end() || it->second.kind != kind) return false;
    auto& v = it->second;
    s << " " << name << slot << "=" << v.width << "x" << v.height << "/fmt" << int(v.format) << "#" << std::hex << v.id
      << std::dec;
    return true;
  };
  append(address(st, st.pass.camera.cbv, 0), "camera-b", 0, 'b');
  append(address(st, st.pass.resources.cbv, 0), "b", 0, 'b');
  UINT textures = 0;
  for (UINT i = 0; i < 8; ++i) textures += append(address(st, st.pass.resources.srv, i), "t", i, 't');
  for (UINT i = 0; i < 8; ++i) append(st.draw.targets[i].ptr, "rt", i, 'r');
  if (textures) {
    sampled.insert(e);
    ACO_LOG("submitted %s %s%s", e->role.c_str(), e->detail.c_str(), s.str().c_str());
  }
}
template <const size_t& slot, typename... Args>
void WINAPI submit(ID3D12GraphicsCommandList* list, Args... args) {
  inspect(list);
  orig<decltype(submit<slot, Args...>)>(list, slot)(list, args...);
}
void WINAPI clear(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso) {
  orig<decltype(clear)>(list, port::clear_state)(list, pso);
  reset_inputs(list, pso);
}
}  // namespace
void read_layout(ID3D12RootSignature* root, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& d) {
  Layout l{};
  if (d.Version == D3D_ROOT_SIGNATURE_VERSION_1_0)
    l = ranges(d.Desc_1_0);
  else if (d.Version == D3D_ROOT_SIGNATURE_VERSION_1_1)
    l = ranges(d.Desc_1_1);
  root->SetPrivateData(layout_tag, sizeof(l), &l);
}
void tag_inputs(ID3D12PipelineState* pso, ID3D12RootSignature* root, port::Entry& e) {
  bool taa = e.role == "PS/temporalaa.hlsl/PS_TemporalAA";
  if (!taa && !e.role.starts_with("PS/convertlinearz.") && !e.role.starts_with("PS/uicompositing.")) return;
  auto l = get<ID3D12RootSignature, Layout>(root, layout_tag);
  Pass pass{l.pass, taa ? l.temporal : l.composite, &e};
  pso->SetPrivateData(pass_tag, sizeof(pass), &pass);
}
void bind_inputs(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso) {
  auto st = get<ID3D12GraphicsCommandList, Bindings>(list, list_tag);
  st.draw.pso = pso;
  st.pass = get<ID3D12PipelineState, Pass>(pso, pass_tag);
  save(list, st);
}
void reset_inputs(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso) {
  Bindings st{};
  st.draw.pso = pso;
  st.pass = get<ID3D12PipelineState, Pass>(pso, pass_tag);
  save(list, st);
}
void observe_descriptors(ID3D12Device* dev) {
  observe_uploads(dev);
#define HOOK(slot, fn) aco::hook_vtable(dev, port::slot, reinterpret_cast<void*>(fn))
  HOOK(make_cbv, cbv);
  HOOK(make_srv, srv);
  HOOK(make_uav, uav);
  HOOK(make_rtv, rtv);
  HOOK(copy_ranges, copy_ranges);
  HOOK(copy_simple, simple);
  HOOK(create_heap, create_heap);
#undef HOOK
}
void observe_commands(ID3D12GraphicsCommandList* list) {
#define HOOK(slot, ...) aco::hook_vtable(list, port::slot, reinterpret_cast<void*>(__VA_ARGS__))
  HOOK(topology, topology);
  HOOK(viewports, viewports);
  HOOK(scissors, scissors);
  HOOK(set_heaps, set_heaps);
  HOOK(set_gtable, table);
  HOOK(set_groot, root);
  HOOK(set_targets, targets);
  HOOK(clear_state, clear);
  HOOK(draw, submit<port::draw, UINT, UINT, UINT, UINT>);
  HOOK(draw_indexed, submit<port::draw_indexed, UINT, UINT, UINT, INT, UINT>);
  HOOK(indirect,
       submit<port::indirect, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64>);
#undef HOOK
}
}  // namespace bifrost
