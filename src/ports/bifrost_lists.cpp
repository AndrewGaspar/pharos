#include "ports/bifrost_lists.h"
#include "ports/bifrost_dual.h"
#include "ports/bifrost_eye.h"
#include "ports/bifrost_resources.h"
#include "ports/bifrost_replay.h"
#include "ports/bifrost_present.h"
#include "core/hook.h"
#include "core/log.h"

#include <mutex>
#include <vector>
namespace port {
extern const size_t create_list, close_list, barrier, dispatch, copy_buffer, copy_texture, copy_resource, resolve_sub,
    clear_rtv, clear_dsv, clear_uav_float, clear_uav_uint, discard, blend_factor, stencil_ref, execute_bundle,
    set_index, set_vertices, set_stream, predication, compute_const, graphics_const, compute_consts, graphics_consts,
    compute_cbv, graphics_cbv, compute_srv, graphics_srv, compute_uav, graphics_uav;
}
namespace bifrost::dual {
namespace {
using Microsoft::WRL::ComPtr;
constexpr GUID tag{0x643316f1, 0xeed9, 0x4be4, {0xb2, 0x49, 0xc0, 0x28, 0x65, 0xd9, 0xf0, 0xf8}};
template <class T>
T* paired(ID3D12Object* source) {
  T* result{};
  UINT bytes = sizeof(result);
  if (!source || FAILED(source->GetPrivateData(tag, &bytes, &result)) || !result) return nullptr;
  result->Release();
  return result;
}
ComPtr<ID3D12CommandAllocator> pair_allocator(ID3D12Device* dev, ID3D12CommandAllocator* original,
                                              D3D12_COMMAND_LIST_TYPE t) {
  static std::mutex lock;
  std::lock_guard guard(lock);
  ComPtr<ID3D12CommandAllocator> result = paired<ID3D12CommandAllocator>(original);
  if (!result && SUCCEEDED(dev->CreateCommandAllocator(t, IID_PPV_ARGS(&result))))
    original->SetPrivateDataInterface(tag, result.Get());
  return result;
}
HRESULT WINAPI close_list(ID3D12GraphicsCommandList* self) {
  auto original = aco::orig<decltype(close_list)>(self, port::close_list);
  auto hr = original(self);
  if (SUCCEEDED(hr))
    if (auto* right = list(self)) {
      auto result = original(right);
      if (FAILED(result)) fail("command list close", result);
    }
  return hr;
}
template <const size_t& Slot, class... Args>
void WINAPI same(ID3D12GraphicsCommandList* self, Args... args) {
  auto fn = aco::orig<void WINAPI(ID3D12GraphicsCommandList*, Args...)>(self, Slot);
  fn(self, args...);
  if (auto* r = list(self)) fn(r, args...);
}
template <const size_t& Slot>
void WINAPI root_va(ID3D12GraphicsCommandList* self, UINT index, UINT64 va) {
  auto fn = aco::orig<decltype(root_va<Slot>)>(self, Slot);
  fn(self, index, va);
  if (auto* r = list(self)) fn(r, index, address(va));
}
void WINAPI copy_buffer(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, UINT64 pos, ID3D12Resource* src, UINT64 off,
                        UINT64 n) {
  auto fn = aco::orig<decltype(copy_buffer)>(cl, port::copy_buffer);
  fn(cl, dst, pos, src, off, n);
  if (auto* r = list(cl))
    if (other(dst) != dst) fn(r, other(dst), pos, other(src), off, n);
}
void WINAPI copy_texture(ID3D12GraphicsCommandList* cl, const D3D12_TEXTURE_COPY_LOCATION* dest, UINT x, UINT y, UINT z,
                         const D3D12_TEXTURE_COPY_LOCATION* source, const D3D12_BOX* box) {
  auto fn = aco::orig<decltype(copy_texture)>(cl, port::copy_texture);
  fn(cl, dest, x, y, z, source, box);
  if (auto* r = list(cl)) {
    auto d = *dest, s = *source;
    d.pResource = other(d.pResource);
    s.pResource = other(s.pResource);
    if (d.pResource != dest->pResource) fn(r, &d, x, y, z, &s, box);
  }
}
void WINAPI copy_resource(ID3D12GraphicsCommandList* cl, ID3D12Resource* dest, ID3D12Resource* src) {
  auto fn = aco::orig<decltype(copy_resource)>(cl, port::copy_resource);
  fn(cl, dest, src);
  if (auto* r = list(cl))
    if (other(dest) != dest) fn(r, other(dest), other(src));
}
void WINAPI resolve_sub(ID3D12GraphicsCommandList* cl, ID3D12Resource* dest, UINT di, ID3D12Resource* src, UINT si,
                        DXGI_FORMAT f) {
  auto fn = aco::orig<decltype(resolve_sub)>(cl, port::resolve_sub);
  fn(cl, dest, di, src, si, f);
  if (auto* r = list(cl))
    if (other(dest) != dest) fn(r, other(dest), di, other(src), si, f);
}
void WINAPI clear_rtv(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE h, const FLOAT* value, UINT count,
                      const D3D12_RECT* rect) {
  auto fn = aco::orig<decltype(clear_rtv)>(cl, port::clear_rtv);
  fn(cl, h, value, count, rect);
  if (auto* r = list(cl))
    if (other(h).ptr != h.ptr) fn(r, other(h), value, count, rect);
}
void WINAPI clear_dsv(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE h, D3D12_CLEAR_FLAGS flags,
                      FLOAT depth, UINT8 stencil, UINT count, const D3D12_RECT* rect) {
  auto fn = aco::orig<decltype(clear_dsv)>(cl, port::clear_dsv);
  fn(cl, h, flags, depth, stencil, count, rect);
  if (auto* r = list(cl))
    if (other(h).ptr != h.ptr) fn(r, other(h), flags, depth, stencil, count, rect);
}
template <const size_t& Slot, class V>
void WINAPI clear_uav(ID3D12GraphicsCommandList* cl, D3D12_GPU_DESCRIPTOR_HANDLE gpu, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                      ID3D12Resource* res, const V* values, UINT n, const D3D12_RECT* rect) {
  auto fn = aco::orig<decltype(clear_uav<Slot, V>)>(cl, Slot);
  fn(cl, gpu, cpu, res, values, n, rect);
  if (auto* r = list(cl))
    if (other(res) != res) fn(r, other(gpu), other(cpu), other(res), values, n, rect);
}
void WINAPI discard(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, const D3D12_DISCARD_REGION* region) {
  auto fn = aco::orig<decltype(discard)>(cl, port::discard);
  fn(cl, res, region);
  if (auto* r = list(cl))
    if (other(res) != res) fn(r, other(res), region);
}
void WINAPI set_index(ID3D12GraphicsCommandList* cl, const D3D12_INDEX_BUFFER_VIEW* vb) {
  auto fn = aco::orig<decltype(set_index)>(cl, port::set_index);
  fn(cl, vb);
  if (auto* r = list(cl)) {
    D3D12_INDEX_BUFFER_VIEW v{};
    if (vb) {
      v = *vb;
      v.BufferLocation = address(v.BufferLocation);
    }
    fn(r, vb ? &v : nullptr);
  }
}
void WINAPI set_vertices(ID3D12GraphicsCommandList* cl, UINT first, UINT n, const D3D12_VERTEX_BUFFER_VIEW* vb) {
  auto fn = aco::orig<decltype(set_vertices)>(cl, port::set_vertices);
  fn(cl, first, n, vb);
  if (auto* r = list(cl)) {
    if (!vb) return fn(r, first, n, nullptr);
    std::vector<D3D12_VERTEX_BUFFER_VIEW> v(vb, vb + n);
    for (auto& b : v) b.BufferLocation = address(b.BufferLocation);
    fn(r, first, n, v.data());
  }
}
void WINAPI set_stream(ID3D12GraphicsCommandList* cl, UINT first, UINT n, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* vb) {
  auto fn = aco::orig<decltype(set_stream)>(cl, port::set_stream);
  fn(cl, first, n, vb);
  if (auto* r = list(cl)) {
    if (!vb) return fn(r, first, n, nullptr);
    std::vector<D3D12_STREAM_OUTPUT_BUFFER_VIEW> v(vb, vb + n);
    for (auto& b : v) {
      const auto output = address(b.BufferLocation), counter = address(b.BufferFilledSizeLocation);
      if ((b.BufferLocation && output == b.BufferLocation) ||
          (b.BufferFilledSizeLocation && counter == b.BufferFilledSizeLocation)) {
        fail("shared stream-output destination", E_NOTIMPL);
        return;
      }
      b.BufferLocation = output;
      b.BufferFilledSizeLocation = counter;
    }
    fn(r, first, n, v.data());
  }
}
void WINAPI predication(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, UINT64 off, D3D12_PREDICATION_OP op) {
  auto fn = aco::orig<decltype(predication)>(cl, port::predication);
  fn(cl, res, off, op);
  if (auto* r = list(cl)) fn(r, res, off, op);
}
void WINAPI bundle(ID3D12GraphicsCommandList* cl, ID3D12GraphicsCommandList* secondary) {
  auto fn = aco::orig<decltype(bundle)>(cl, port::execute_bundle);
  fn(cl, secondary);
  if (auto* r = list(cl))
    if (auto* b = list(secondary)) fn(r, b);
}
void WINAPI barriers(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_RESOURCE_BARRIER* b) {
  aco::orig<decltype(barriers)>(cl, port::barrier)(cl, n, b);
  resource_barriers(cl, n, b);
}
}  // namespace
ID3D12GraphicsCommandList* list(ID3D12GraphicsCommandList* cl) {
  return game_call() ? paired<ID3D12GraphicsCommandList>(cl) : nullptr;
}
ID3D12GraphicsCommandList* list(ID3D12CommandList* cl) {
  return game_call() ? paired<ID3D12GraphicsCommandList>(cl) : nullptr;
}
void resource_barriers(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_RESOURCE_BARRIER* b) {
  auto* r = list(cl);
  if (!r) return;
  std::vector<D3D12_RESOURCE_BARRIER> output;
  output.reserve(n);
  for (UINT i = 0; i < n; ++i) {
    auto x = b[i];
    if (x.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
      x.Transition.pResource = other(x.Transition.pResource);
      if (x.Transition.pResource == b[i].Transition.pResource) continue;
    } else if (x.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
      x.UAV.pResource = other(x.UAV.pResource);
      if (x.UAV.pResource && x.UAV.pResource == b[i].UAV.pResource) continue;
    } else if (x.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
      auto& a = x.Aliasing;
      a.pResourceBefore = other(a.pResourceBefore);
      a.pResourceAfter = other(a.pResourceAfter);
      if (a.pResourceBefore == b[i].Aliasing.pResourceBefore) a.pResourceBefore = nullptr;
      if (a.pResourceAfter == b[i].Aliasing.pResourceAfter) a.pResourceAfter = nullptr;
    }
    output.push_back(x);
  }
  if (!output.empty()) aco::orig<decltype(barriers)>(r, port::barrier)(r, UINT(output.size()), output.data());
}
void reset_allocator(ID3D12CommandAllocator* a) {
  if (auto* r = paired<ID3D12CommandAllocator>(a)) {
    auto hr = r->Reset();
    if (FAILED(hr)) fail("command allocator reset", hr);
  }
}
void make_list(ID3D12GraphicsCommandList* original, ID3D12Device* dev, UINT node, D3D12_COMMAND_LIST_TYPE type,
               ID3D12CommandAllocator* alloc, ID3D12PipelineState* initial) {
  if (!game_call()) return;
  auto a = pair_allocator(dev, alloc, type);
  if (!a) {
    fail("command allocator creation", E_FAIL);
    return;
  }
  Internal scope;
  ComPtr<ID3D12GraphicsCommandList> r;
  auto make = aco::orig<HRESULT WINAPI(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
                                       ID3D12PipelineState*, REFIID, void**)>(dev, port::create_list);
  auto hr = make(dev, node, type, a.Get(), initial, IID_PPV_ARGS(&r));
  if (FAILED(hr)) {
    fail("command list creation", hr);
    return;
  }
  original->SetPrivateDataInterface(tag, r.Get());
  eye::allocator(r.Get(), a.Get());
  reset_inputs(r.Get(), initial);
  replay::reset(r.Get());
}
void reset_list(ID3D12GraphicsCommandList* cl, ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial) {
  auto* r = list(cl);
  if (!r) return;
  ComPtr<ID3D12Device> dev;
  if (FAILED(cl->GetDevice(IID_PPV_ARGS(&dev)))) return;
  auto a = pair_allocator(dev.Get(), allocator, cl->GetType());
  auto hr = a ? r->Reset(a.Get(), initial) : E_FAIL;
  if (FAILED(hr)) fail("command list reset", hr);
}
void install(ID3D12GraphicsCommandList* cl) {
  if (!enabled()) return;
#define H(slot, ...) aco::hook_vtable(cl, port::slot, reinterpret_cast<void*>(__VA_ARGS__))
  H(close_list, close_list);
  H(copy_buffer, copy_buffer);
  H(copy_texture, copy_texture);
  H(copy_resource, copy_resource);
  H(resolve_sub, resolve_sub);
  H(clear_rtv, clear_rtv);
  H(clear_dsv, clear_dsv);
  H(discard, discard);
  H(clear_uav_float, clear_uav<port::clear_uav_float, FLOAT>);
  H(clear_uav_uint, clear_uav<port::clear_uav_uint, UINT>);
  H(stencil_ref, same<port::stencil_ref, UINT>);
  H(blend_factor, same<port::blend_factor, const FLOAT*>);
  H(execute_bundle, bundle);
  H(set_index, set_index);
  H(set_vertices, set_vertices);
  H(set_stream, set_stream);
  H(predication, predication);
  H(compute_const, same<port::compute_const, UINT, UINT, UINT>);
  H(graphics_const, same<port::graphics_const, UINT, UINT, UINT>);
  H(compute_consts, same<port::compute_consts, UINT, UINT, const void*, UINT>);
  H(graphics_consts, same<port::graphics_consts, UINT, UINT, const void*, UINT>);
  H(compute_cbv, root_va<port::compute_cbv>);
  H(graphics_cbv, root_va<port::graphics_cbv>);
  H(compute_srv, root_va<port::compute_srv>);
  H(graphics_srv, root_va<port::graphics_srv>);
  H(compute_uav, root_va<port::compute_uav>);
  H(graphics_uav, root_va<port::graphics_uav>);
  if (!replay::tracking()) {
    H(barrier, barriers);
    H(dispatch, same<port::dispatch, UINT, UINT, UINT>);
  }
#undef H
}
}  // namespace bifrost::dual
