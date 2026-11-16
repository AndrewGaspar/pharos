#include "world/world.h"

#include <windows.h>
#include <d3d11_1.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "core/cbshadow.h"
#include "core/config.h"
#include "core/log.h"

namespace aco {

namespace {

// MiscCullingConsts layout (from the shaders' reflection data).
constexpr UINT kSlot = 8;
constexpr UINT kSize = 620;
constexpr UINT kMinTriangleArea = 552, kFOVDistanceFactor = 564, kLODDistanceBias = 568, kLODDistanceScale = 572;
constexpr UINT kOwnBytes = 640;  // our copy: kSize rounded up to 16

std::mutex g_mutex;
std::unordered_map<ID3D11DeviceContext*, ID3D11Buffer*> g_own;  // one dynamic CB per context
std::atomic<uint32_t> g_patched{0}, g_unshadowed{0}, g_dispatches{0};
std::atomic<float> g_seen_scale{0}, g_seen_bias{0}, g_seen_fov{0}, g_seen_tri{0};

ID3D11Buffer* own_buffer(ID3D11DeviceContext* ctx) {
  std::lock_guard lock(g_mutex);
  auto it = g_own.find(ctx);
  if (it != g_own.end()) return it->second;
  ID3D11Device* dev = nullptr;
  ctx->GetDevice(&dev);
  D3D11_BUFFER_DESC d{};
  d.ByteWidth = kOwnBytes;
  d.Usage = D3D11_USAGE_DYNAMIC;
  d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  ID3D11Buffer* b = nullptr;
  if (dev) dev->CreateBuffer(&d, nullptr, &b);
  if (dev) dev->Release();
  g_own[ctx] = b;
  return b;
}

float f_at(const uint8_t* p, UINT off) {
  float v;
  memcpy(&v, p + off, 4);
  return v;
}
void f_set(uint8_t* p, UINT off, float v) { memcpy(p + off, &v, 4); }

} // namespace

void world_on_culling_dispatch(ID3D11DeviceContext* ctx, const std::function<void()>& dispatch) {
  const Config& cfg = config();
  g_dispatches.fetch_add(1, std::memory_order_relaxed);
  bool active = cfg.world_lod_distance != 1.0f || cfg.world_min_triangle_area != 1.0f;

  ID3D11DeviceContext1* c1 = nullptr;
  ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1));
  ID3D11Buffer* cb = nullptr;
  UINT first = 0, num = 0;
  if (c1) c1->CSGetConstantBuffers1(kSlot, 1, &cb, &first, &num);
  if (!cb) {
    if (c1) c1->Release();
    dispatch();
    return;
  }
  D3D11_BUFFER_DESC bd{};
  cb->GetDesc(&bd);
  cbshadow_watch_size(bd.ByteWidth);  // shadowed from the next upload on
  uint8_t data[kOwnBytes] = {};
  bool have = cbshadow_read(ctx, cb, first * 16, kSize, data);
  if (have) {
    g_seen_scale.store(f_at(data, kLODDistanceScale), std::memory_order_relaxed);
    g_seen_bias.store(f_at(data, kLODDistanceBias), std::memory_order_relaxed);
    g_seen_fov.store(f_at(data, kFOVDistanceFactor), std::memory_order_relaxed);
    g_seen_tri.store(f_at(data, kMinTriangleArea), std::memory_order_relaxed);
  } else {
    g_unshadowed.fetch_add(1, std::memory_order_relaxed);
  }
  ID3D11Buffer* own = (active && have) ? own_buffer(ctx) : nullptr;
  if (!own) {
    cb->Release();
    if (c1) c1->Release();
    dispatch();
    return;
  }
  if (cfg.world_lod_distance > 0.0f)
    f_set(data, kLODDistanceScale, f_at(data, kLODDistanceScale) / cfg.world_lod_distance);
  f_set(data, kMinTriangleArea, f_at(data, kMinTriangleArea) * cfg.world_min_triangle_area);

  D3D11_MAPPED_SUBRESOURCE m{};
  if (SUCCEEDED(ctx->Map(own, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
    memcpy(m.pData, data, kOwnBytes);
    ctx->Unmap(own, 0);
    ctx->CSSetConstantBuffers(kSlot, 1, &own);
    dispatch();
    c1->CSSetConstantBuffers1(kSlot, 1, &cb, &first, &num);
    g_patched.fetch_add(1, std::memory_order_relaxed);
  } else {
    dispatch();
  }
  cb->Release();
  c1->Release();
}

void world_on_present() {
  static ULONGLONG last = 0;
  ULONGLONG now = GetTickCount64();
  if (now - last < 2000) return;
  if (last) {
    const Config& cfg = config();
    ACO_LOG("world: %u culling dispatches, %u patched, %u unshadowed | game LODDistanceScale=%g bias=%g "
            "FOVDistanceFactor=%g MinTriangleArea=%g | lod_distance=%g min_triangle_area=%g",
            g_dispatches.exchange(0), g_patched.exchange(0), g_unshadowed.exchange(0), g_seen_scale.load(),
            g_seen_bias.load(), g_seen_fov.load(), g_seen_tri.load(), cfg.world_lod_distance,
            cfg.world_min_triangle_area);
  }
  last = now;
}

} // namespace aco
