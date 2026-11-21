// Inventory the native work that must be replayed for a second eye. Keep metadata
// with the list so a reset or a reused COM address cannot carry over an older frame.
#include "ports/bifrost_replay.h"

#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <vector>

#include "core/hook.h"
#include "core/log.h"
#include "ports/bifrost_present.h"
#include "ports/bifrost_scene.h"
#include "ports/bifrost_uploads.h"

namespace port {
extern const size_t barrier, dispatch, end_query, resolve_queries;
}
namespace bifrost::replay {
namespace {
using Microsoft::WRL::ComPtr;
constexpr GUID key{0x5dabd6a3, 0x5d01, 0x48aa, {0x99, 0xd7, 0x2c, 0xcb, 0x65, 0x97, 0xc6, 0x33}};
struct Edges {
  D3D12_RESOURCE_STATES first, last;
};
struct Recording final : IUnknown {
  std::atomic<ULONG> refs{1};
  unsigned draws{}, dispatches{}, queries{}, resolves{}, split{}, alias{};
  std::map<UINT64, UINT> cameras;
  std::map<std::pair<ID3D12Resource*, UINT>, Edges> transitions;
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
};
ComPtr<Recording> recording(ID3D12Object* list) {
  ComPtr<Recording> result;
  UINT bytes = sizeof(void*);
  list->GetPrivateData(key, &bytes, result.GetAddressOf());
  return result;
}
void WINAPI barriers(ID3D12GraphicsCommandList* list, UINT n, const D3D12_RESOURCE_BARRIER* data) {
  if (auto r = recording(list)) {
    for (UINT i = 0; i < n; ++i) {
      const auto& b = data[i];
      if (b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE) ++r->split;
      if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) ++r->alias;
      if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
      const auto& t = b.Transition;
      auto tag = std::make_pair(t.pResource, t.Subresource);
      auto [it, first] = r->transitions.try_emplace(tag, Edges{t.StateBefore, t.StateAfter});
      if (!first) it->second.last = t.StateAfter;
    }
  }
  aco::orig<decltype(barriers)>(list, port::barrier)(list, n, data);
}
void WINAPI dispatch(ID3D12GraphicsCommandList* list, UINT x, UINT y, UINT z) {
  if (auto r = recording(list)) ++r->dispatches;
  aco::orig<decltype(dispatch)>(list, port::dispatch)(list, x, y, z);
}
void WINAPI end_query(ID3D12GraphicsCommandList* list, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
  if (auto r = recording(list)) ++r->queries;
  aco::orig<decltype(end_query)>(list, port::end_query)(list, heap, type, index);
}
void WINAPI resolve(ID3D12GraphicsCommandList* list, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT start, UINT n,
                    ID3D12Resource* dest, UINT64 offset) {
  if (auto r = recording(list)) ++r->resolves;
  aco::orig<decltype(resolve)>(list, port::resolve_queries)(list, heap, type, start, n, dest, offset);
}
std::atomic<bool> capture{};
}  // namespace
bool tracking() {
  static bool on = option(L"stereo", L"trace", 0) != 0;
  return on;
}
void observe(ID3D12GraphicsCommandList* list) {
  if (!tracking()) return;
  aco::hook_vtable(list, port::barrier, reinterpret_cast<void*>(barriers));
  aco::hook_vtable(list, port::dispatch, reinterpret_cast<void*>(dispatch));
  aco::hook_vtable(list, port::end_query, reinterpret_cast<void*>(end_query));
  aco::hook_vtable(list, port::resolve_queries, reinterpret_cast<void*>(resolve));
}
void reset(ID3D12GraphicsCommandList* list) {
  if (!tracking()) return;
  auto* r = new Recording;
  list->SetPrivateDataInterface(key, r);
  r->Release();
}
void camera(ID3D12GraphicsCommandList* list, UINT64 va, UINT bytes) {
  if (auto r = recording(list)) r->cameras.try_emplace(va, bytes);
}
void draw(ID3D12GraphicsCommandList* list) {
  if (tracking()) {
    if (auto r = recording(list)) ++r->draws;
  }
}
void submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
  if (!capture.load(std::memory_order_relaxed)) return;
  unsigned draws = 0, dispatches = 0, queries = 0, resolves = 0, split = 0, alias = 0;
  std::map<UINT64, UINT> cameras;
  std::map<std::pair<ID3D12Resource*, UINT>, Edges> transitions;
  int temporal = -1;
  for (UINT i = 0; i < count; ++i) {
    auto r = recording(lists[i]);
    if (!r) continue;
    if (recorded_scene(lists[i]).info.serial) temporal = int(i);
    draws += r->draws;
    dispatches += r->dispatches;
    queries += r->queries;
    resolves += r->resolves;
    split += r->split;
    alias += r->alias;
    cameras.insert(r->cameras.begin(), r->cameras.end());
    for (const auto& [key, edges] : r->transitions) {
      auto [it, first] = transitions.try_emplace(key, edges);
      if (!first) it->second.last = edges.last;
    }
  }
  unsigned unequal = 0, perspective = 0;
  for (const auto& [k, e] : transitions) unequal += e.first != e.last;
  for (const auto& [va, bytes] : cameras) {
    if (bytes < 82 * 16) continue;
    auto c = read_constants(va, 82 * 16);
    if (c.size() != 82 * 4) continue;
    if (std::abs(c[29 * 4 + 2]) > .9f && std::abs(c[29 * 4 + 3]) < .001f && std::abs(c[26 * 4]) > .01f) ++perspective;
  }
  ACO_LOG(
      "Bifrost stereo: queue %p type %u, %u lists, TAA %d, draws %u dispatches %u, queries %u resolves %u, "
      "camera ranges %zu perspective %u, transition keys %zu different end states %u split %u alias %u",
      queue, UINT(queue->GetDesc().Type), count, temporal, draws, dispatches, queries, resolves, cameras.size(),
      perspective, transitions.size(), unequal, split, alias);
}
void present() {
  if (!tracking()) return;
  static const auto path = aco::out_dir() + L"\\capture-stereo";
  static unsigned frames = 0;
  bool on = capture.exchange(false);
  if (on) ACO_LOG("Bifrost stereo: end frame");
  if (++frames % 30 == 0 && DeleteFileW(path.c_str())) {
    ACO_LOG("Bifrost stereo: begin frame");
    capture = true;
  }
}
}  // namespace bifrost::replay
