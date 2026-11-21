#include "core/cbshadow.h"
#include "rig/capture.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/hook.h"
#include "stereo/camera.h"
#include "stereo/record.h"
#include "core/log.h"
#include "core/mapstats.h"
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
// Watched buffers: tiny lock-free array checked before doing any work in Map (hot path).
constexpr size_t kMaxWatched = 32;
std::atomic<ID3D11Resource*> g_watch_list[kMaxWatched];
std::atomic<size_t> g_watch_count{0};

bool is_watched(ID3D11Resource* r) {
  size_t n = g_watch_count.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i)
    if (g_watch_list[i].load(std::memory_order_relaxed) == r) return true;
  return false;
}
// Watched buffer sizes: engines often rotate a cbuffer through a pool of identical buffers, so
// watching individual buffers misses most writes; watching by ByteWidth catches the whole pool.
constexpr size_t kMaxWatchedSizes = 4;
std::atomic<uint32_t> g_watch_sizes[kMaxWatchedSizes];
std::atomic<size_t> g_watch_size_count{0};

bool is_watched_size(uint32_t bw) {
  size_t n = g_watch_size_count.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i)
    if (g_watch_sizes[i].load(std::memory_order_relaxed) == bw) return true;
  return false;
}

// Pointer-only sizes (cbshadow_watch_size_ptr): remember where the last Map(WRITE_DISCARD) of each
// (context, buffer) put the data, no copy. Mapped dynamic buffers in DXVK stay mapped, so the data
// can be read at record time; copying every Map instead (reads from write-combined memory) costs
// far too much for a size as common as 2 KB.
std::atomic<uint32_t> g_ptr_sizes[kMaxWatchedSizes];
std::atomic<size_t> g_ptr_size_count{0};
bool is_ptr_size(uint32_t bw) {
  size_t n = g_ptr_size_count.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i)
    if (g_ptr_sizes[i].load(std::memory_order_relaxed) == bw) return true;
  return false;
}

std::mutex g_mutex;
std::unordered_map<Key, Shadow, KeyHash> g_shadows;
std::unordered_map<Key, const void*, KeyHash> g_ptrs;
std::unordered_map<ID3D11Resource*, ID3D11DeviceContext*> g_last_writer;

// Returns the buffer's size if it should be shadowed, else 0.
uint32_t shadow_size(ID3D11Resource* res) {
  bool by_res = g_all.load(std::memory_order_relaxed) || is_watched(res);
  if (!by_res && g_watch_size_count.load(std::memory_order_relaxed) == 0) return 0;
  D3D11_RESOURCE_DIMENSION dim;
  res->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return 0;
  D3D11_BUFFER_DESC d;
  static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
  if (!(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) || d.ByteWidth > kMaxShadowBytes) return 0;
  if (!by_res && !is_watched_size(d.ByteWidth)) return 0;
  return d.ByteWidth;
}

// A streaming upload can span Presents. Map a separate discard buffer before
// handing the game's caller its cached shadow. Commit to the real resource on
// the game's Unmap, keeping earlier GPU commands replayable during the upload.
ID3D11Buffer* discard_scratch(ID3D11DeviceContext* ctx, UINT bytes) {
  using Microsoft::WRL::ComPtr;
  thread_local ComPtr<ID3D11Buffer> scratch;
  thread_local ID3D11DeviceContext* owner = nullptr;
  thread_local UINT capacity = 0;
  if (owner != ctx || capacity < bytes || !scratch) {
    ComPtr<ID3D11Device> device;
    ctx->GetDevice(&device);
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = std::max(bytes, 65536u);
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> buffer;
    if (FAILED(device->CreateBuffer(&d, nullptr, &buffer))) return nullptr;
    scratch = std::move(buffer);
    owner = ctx;
    capacity = d.ByteWidth;
  }
  return scratch.Get();
}

void STDMETHODCALLTYPE hk_Unmap(ID3D11DeviceContext*, ID3D11Resource*, UINT);
HRESULT STDMETHODCALLTYPE hk_Map(ID3D11DeviceContext* self, ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags,
                                 D3D11_MAPPED_SUBRESOURCE* out) {
  auto map = orig<decltype(hk_Map)>(self, Context_Map);
  auto unmap = orig<decltype(hk_Unmap)>(self, Context_Unmap);
  if (g_rec_replaying && self == g_rec_imm && rec_detached_type(res, sub)) return map(self, res, sub, type, flags, out);
  bool record = ACO_REC_WANT(self);
  bool detached = false;
  UINT width = 0;
  ID3D11Resource* target = res;
  if (record && rig::armed() && out && res && sub == 0 && !flags &&
      (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE_NO_OVERWRITE)) {
    D3D11_RESOURCE_DIMENSION dim;
    res->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
      D3D11_BUFFER_DESC d;
      static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
      if (d.Usage == D3D11_USAGE_DYNAMIC && (d.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE)) {
        width = d.ByteWidth;
        // NO_OVERWRITE can seed a fresh shadow from the original mapping.
        // Closing it immediately preserves the original contents.
        if (type == D3D11_MAP_WRITE_DISCARD) {
          if (auto* scratch = discard_scratch(self, width)) target = scratch;
          detached = target != res;
        } else
          detached = true;
      }
    }
  }
  HRESULT hr = map(self, target, sub, type, flags, out);
  if (FAILED(hr) && target != res) {
    target = res;
    detached = false;
    hr = map(self, res, sub, type, flags, out);
  }
  if (SUCCEEDED(hr) && out && record) {
    bool copied = rec_map(res, sub, type, flags, out, detached);
    if (detached && copied) {
      unmap(self, target, sub);
      if (type == D3D11_MAP_WRITE_NO_OVERWRITE) rec_unmapped();
      out->RowPitch = out->DepthPitch = width;
    } else if (target != res) {
      unmap(self, target, sub);
      hr = map(self, res, sub, type, flags, out);
    }
  }
  if (SUCCEEDED(hr) && out) cam_map(self, res, type, out);
  if (SUCCEEDED(hr) && out && (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE_NO_OVERWRITE)) {
    mapstats_note(res, false);
    if (uint32_t size = shadow_size(res)) {
      std::lock_guard lock(g_mutex);
      auto& s = g_shadows[{self, res}];
      s.data.resize(size);
      s.mapped = out->pData;
    } else if (type == D3D11_MAP_WRITE_DISCARD && g_ptr_size_count.load(std::memory_order_relaxed)) {
      D3D11_RESOURCE_DIMENSION dim;
      res->GetType(&dim);
      if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d;
        static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
        if ((d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && is_ptr_size(d.ByteWidth)) {
          std::lock_guard lock(g_mutex);
          g_ptrs[{self, res}] = out->pData;
          g_last_writer[res] = self;
        }
      }
    }
  }
  return hr;
}

void STDMETHODCALLTYPE hk_Unmap(ID3D11DeviceContext* self, ID3D11Resource* res, UINT sub) {
  auto unmap = orig<decltype(hk_Unmap)>(self, Context_Unmap);
  auto pending = self == g_rec_imm ? rec_detached_type(res, sub, !g_rec_replaying) : D3D11_MAP(0);
  if (pending && g_rec_replaying) {
    unmap(self, res, sub);
    return;
  }
  cam_unmap(self, res);
  D3D11_MAPPED_SUBRESOURCE dst{};
  if (pending) {
    auto map = orig<decltype(hk_Map)>(self, Context_Map);
    if (FAILED(map(self, res, sub, pending, 0, &dst))) ACO_LOG("rig: could not commit dynamic buffer upload");
  }
  const bool notify = self == g_rec_imm && rec_unmap(res, sub, rig::armed(), dst.pData);
  if (shadow_size(res)) {
    std::lock_guard lock(g_mutex);
    auto it = g_shadows.find({self, res});
    if (it != g_shadows.end() && it->second.mapped) {
      memcpy(it->second.data.data(), it->second.mapped, it->second.data.size());
      it->second.mapped = nullptr;
      g_last_writer[res] = self;
    }
  }
  if (!pending || dst.pData) unmap(self, res, sub);
  if (notify) rec_unmapped();
}

void STDMETHODCALLTYPE hk_UpdateSubresource(ID3D11DeviceContext* self, ID3D11Resource* res, UINT sub, const D3D11_BOX* box,
                                            const void* data, UINT rowpitch, UINT depthpitch) {
  if (g_rec_replaying && self == g_rec_imm && rec_detached_type(res, sub)) {
    orig<decltype(hk_UpdateSubresource)>(self, Context_UpdateSubresource)(self, res, sub, box, data, rowpitch,
                                                                          depthpitch);
    return;
  }
  if (ACO_REC_WANT(self)) rec_update(res, sub, box, data, rowpitch, depthpitch, 0, false);
  cam_update(self, res, box, data);
  if (data) {
    mapstats_note(res, true);
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
  if (is_watched(buf)) return;
  std::lock_guard lock(g_mutex);
  if (is_watched(buf)) return;
  size_t n = g_watch_count.load();
  if (n >= kMaxWatched) return;
  g_watch_list[n].store(buf, std::memory_order_relaxed);
  g_watch_count.store(n + 1, std::memory_order_release);
}

void cbshadow_watch_size(uint32_t byte_width) {
  if (is_watched_size(byte_width)) return;
  std::lock_guard lock(g_mutex);
  if (is_watched_size(byte_width)) return;
  size_t n = g_watch_size_count.load();
  if (n >= kMaxWatchedSizes) return;
  g_watch_sizes[n].store(byte_width, std::memory_order_relaxed);
  g_watch_size_count.store(n + 1, std::memory_order_release);
  ACO_LOG("cbshadow: shadowing all %u-byte constant buffers", byte_width);
}

void cbshadow_watch_size_ptr(uint32_t byte_width) {
  if (is_ptr_size(byte_width)) return;
  std::lock_guard lock(g_mutex);
  if (is_ptr_size(byte_width)) return;
  size_t n = g_ptr_size_count.load();
  if (n >= kMaxWatchedSizes) return;
  g_ptr_sizes[n].store(byte_width, std::memory_order_relaxed);
  g_ptr_size_count.store(n + 1, std::memory_order_release);
  ACO_LOG("cbshadow: tracking the mapped data of all %u-byte constant buffers", byte_width);
}

bool cbshadow_read(ID3D11DeviceContext* ctx, ID3D11Buffer* buf, uint32_t offset, uint32_t size, void* out) {
  std::lock_guard lock(g_mutex);
  {
    auto p = g_ptrs.find({ctx, buf});
    if (p == g_ptrs.end()) {
      auto lw = g_last_writer.find(buf);
      if (lw != g_last_writer.end()) p = g_ptrs.find({lw->second, buf});
    }
    if (p != g_ptrs.end()) {
      D3D11_BUFFER_DESC d;
      buf->GetDesc(&d);
      if (offset + size > d.ByteWidth) return false;
      memcpy(out, static_cast<const uint8_t*>(p->second) + offset, size);
      return true;
    }
  }
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
