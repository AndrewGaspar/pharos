#include "core/cbshadow.h"

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/hook.h"
#include "core/log.h"
#include "core/vtidx.h"

namespace aco {

namespace {

using namespace vt;

struct Key {
  ID3D11DeviceContext* ctx;
  ID3D11Resource* res;
  bool operator==(const Key& o) const { return ctx == o.ctx && res == o.res; }
};
struct KeyHash {
  size_t operator()(const Key& k) const {
    return std::hash<void*>()(k.ctx) * 31 + std::hash<void*>()(k.res);
  }
};

struct Shadow {
  std::vector<uint8_t> data;
  void* mapped = nullptr;  // live Map pointer, valid until Unmap
};

std::atomic<bool> g_all{false};
std::atomic<size_t> g_watch_count{0};
std::mutex g_mutex;
std::unordered_map<Key, Shadow, KeyHash> g_shadows;
std::unordered_set<ID3D11Resource*> g_watched;
std::unordered_map<ID3D11Resource*, ID3D11DeviceContext*> g_last_writer;

// Returns the buffer's size if it should be shadowed, else 0.
uint32_t shadow_size(ID3D11Resource* res) {
  if (!g_all.load(std::memory_order_relaxed) && g_watch_count.load(std::memory_order_relaxed) == 0) return 0;
  D3D11_RESOURCE_DIMENSION dim;
  res->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return 0;
  D3D11_BUFFER_DESC d;
  static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
  if (!(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) || d.ByteWidth > kMaxShadowBytes) return 0;
  if (!g_all.load(std::memory_order_relaxed)) {
    std::lock_guard lock(g_mutex);
    if (!g_watched.count(res)) return 0;
  }
  return d.ByteWidth;
}

HRESULT STDMETHODCALLTYPE hk_Map(ID3D11DeviceContext* self, ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags,
                                 D3D11_MAPPED_SUBRESOURCE* out) {
  HRESULT hr = orig<decltype(hk_Map)>(self, Context_Map)(self, res, sub, type, flags, out);
  if (SUCCEEDED(hr) && out && (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE_NO_OVERWRITE)) {
    if (uint32_t size = shadow_size(res)) {
      std::lock_guard lock(g_mutex);
      auto& s = g_shadows[{self, res}];
      s.data.resize(size);
      s.mapped = out->pData;
    }
  }
  return hr;
}

void STDMETHODCALLTYPE hk_Unmap(ID3D11DeviceContext* self, ID3D11Resource* res, UINT sub) {
  {
    std::lock_guard lock(g_mutex);
    auto it = g_shadows.find({self, res});
    if (it != g_shadows.end() && it->second.mapped) {
      memcpy(it->second.data.data(), it->second.mapped, it->second.data.size());
      it->second.mapped = nullptr;
      g_last_writer[res] = self;
    }
  }
  orig<decltype(hk_Unmap)>(self, Context_Unmap)(self, res, sub);
}

void STDMETHODCALLTYPE hk_UpdateSubresource(ID3D11DeviceContext* self, ID3D11Resource* res, UINT sub, const D3D11_BOX* box,
                                            const void* data, UINT rowpitch, UINT depthpitch) {
  if (data) {
    if (uint32_t size = shadow_size(res)) {
      std::lock_guard lock(g_mutex);
      auto& s = g_shadows[{self, res}];
      s.data.resize(size);
      uint32_t lo = box ? box->left : 0, hi = box ? box->right : size;
      if (hi > size) hi = size;
      if (lo < hi) memcpy(s.data.data() + lo, data, hi - lo);
      g_last_writer[res] = self;
    }
  }
  orig<decltype(hk_UpdateSubresource)>(self, Context_UpdateSubresource)(self, res, sub, box, data, rowpitch, depthpitch);
}

} // namespace

void cbshadow_install(ID3D11DeviceContext* ctx) {
  hook_vtable(ctx, Context_Map, reinterpret_cast<void*>(hk_Map));
  hook_vtable(ctx, Context_Unmap, reinterpret_cast<void*>(hk_Unmap));
  hook_vtable(ctx, Context_UpdateSubresource, reinterpret_cast<void*>(hk_UpdateSubresource));
}

void cbshadow_set_all(bool enabled) { g_all.store(enabled); }

void cbshadow_watch(ID3D11Buffer* buf) {
  std::lock_guard lock(g_mutex);
  if (g_watched.insert(buf).second) g_watch_count.store(g_watched.size());
}

bool cbshadow_read(ID3D11DeviceContext* ctx, ID3D11Buffer* buf, uint32_t offset, uint32_t size, void* out) {
  std::lock_guard lock(g_mutex);
  auto it = g_shadows.find({ctx, buf});
  if (it == g_shadows.end()) {
    // Written on another context (typically the immediate one) and consumed here.
    auto lw = g_last_writer.find(buf);
    if (lw != g_last_writer.end()) it = g_shadows.find({lw->second, buf});
  }
  if (it == g_shadows.end() || offset + size > it->second.data.size()) return false;
  memcpy(out, it->second.data.data() + offset, size);
  return true;
}

} // namespace aco
