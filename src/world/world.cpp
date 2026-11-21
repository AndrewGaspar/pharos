#include "world/world.h"
#include "rig/capture.h"

#include <windows.h>
#include <d3d11_1.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/cbshadow.h"
#include "core/config.h"
#include "core/log.h"
#include "core/shaders.h"
#include "world/lodpatch.h"

namespace aco {

namespace {

// MiscCullingConsts layout (from the shaders' reflection data).
constexpr UINT kSlot = 8;
constexpr UINT kSize = 620;
constexpr UINT kCullingFlags = 556, kVPosToUV = 592;
constexpr UINT kMinTriangleArea = 552, kFOVDistanceFactor = 564, kLODDistanceBias = 568, kLODDistanceScale = 572;
constexpr UINT kOwnBytes = 640;  // our copy: kSize rounded up to 16

std::mutex g_mutex;
std::unordered_map<ID3D11DeviceContext*, ID3D11Buffer*> g_own;  // one dynamic CB per context
std::atomic<uint32_t> g_patched{0}, g_unshadowed{0}, g_dispatches{0};
std::atomic<float> g_seen_scale{0}, g_seen_bias{0}, g_seen_fov{0}, g_seen_tri{0};

// Diagnostics: dispatches per culling shader (hash), last CullingFlags / VPosToUV seen per shader.
struct PerShader {
  uint32_t dispatches = 0, skipped = 0, flags = 0;
  float vpos[4] = {};
};
std::mutex g_stats_mutex;
std::unordered_map<uint64_t, PerShader> g_stats;

bool skip_hash(const std::string& list, uint64_t hash) {
  if (list.empty() || !hash) return false;
  std::string h = hex64(hash);
  size_t pos = 0;
  while (pos <= list.size()) {
    size_t end = list.find(',', pos);
    if (end == std::string::npos) end = list.size();
    std::string item = list.substr(pos, end - pos);
    if (!item.empty() && h.compare(0, item.size(), item) == 0) return true;
    pos = end + 1;
  }
  return false;
}

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

// mode=far: per (game shader, k) a patched copy that scales only the far end of every LOD range.
std::mutex g_variant_mutex;
std::unordered_map<ID3D11ComputeShader*, std::pair<uint32_t, ID3D11ComputeShader*>> g_variants;  // k bits, variant
std::unordered_map<ID3D11ComputeShader*, bool> g_unpatchable;
std::vector<ID3D11ComputeShader*> g_retired;

ID3D11ComputeShader* far_variant(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs, float k) {
  uint32_t kb;
  memcpy(&kb, &k, 4);
  std::lock_guard lock(g_variant_mutex);
  if (g_unpatchable.count(cs)) return nullptr;
  auto it = g_variants.find(cs);
  if (it != g_variants.end() && it->second.first == kb) return it->second.second;
  const ShaderInfo* info = shader_info(cs);
  std::vector<uint8_t> out;
  std::string why;
  if (!info || !lod_patch_far(info->bytecode, k, out, &why)) {
    ACO_LOG("world: culling shader %s not patched (%s)", info ? hex64(info->hash).c_str() : "?",
            info ? why.c_str() : "unknown shader");
    g_unpatchable[cs] = true;
    return nullptr;
  }
  ID3D11Device* dev = nullptr;
  ctx->GetDevice(&dev);
  ID3D11ComputeShader* v = nullptr;
  HRESULT hr = dev ? dev->CreateComputeShader(out.data(), out.size(), nullptr, &v) : E_FAIL;
  if (dev) dev->Release();
  if (FAILED(hr) || !v) {
    ACO_LOG("world: CreateComputeShader for the patched %s failed (0x%08lx)", hex64(info->hash).c_str(), hr);
    g_unpatchable[cs] = true;
    return nullptr;
  }
  if (it != g_variants.end()) {
    // The previous k's variant is kept alive: command lists recorded with it may not have run yet.
    g_retired.push_back(it->second.second);
    it->second = {kb, v};
  } else {
    g_variants[cs] = {kb, v};
  }
  if (const ShaderInfo* vi = shader_info(v)) {
    std::wstring dir = out_dir() + L"\\lodpatch";
    CreateDirectoryW(dir.c_str(), nullptr);
    shader_dump(*vi, dir);
  }
  ACO_LOG("world: culling shader %s: far-range variant for lod_distance=%g", hex64(info->hash).c_str(), k);
  return v;
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
  bool active = cfg.world_lod_distance != 1.0f && cfg.world_lod_distance > 0.0f;
  // far: patched shaders extend only the far end of LOD ranges (k > 1). k < 1 (shorter draw distance)
  // keeps the old behaviour: every range scaled through the cbuffer.
  bool far_mode = cfg.world_lod_mode != "all" && cfg.world_lod_distance > 1.0f;
  uint64_t hash = 0;
  ID3D11ComputeShader* cs = nullptr;
  ctx->CSGetShader(&cs, nullptr, nullptr);
  if (cs) hash = shader_hash(cs);
  bool skip = skip_hash(cfg.world_lod_skip, hash);
  if (skip) active = false;

  ID3D11DeviceContext1* c1 = nullptr;
  ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1));
  ID3D11Buffer* cb = nullptr;
  UINT first = 0, num = 0;
  if (c1) c1->CSGetConstantBuffers1(kSlot, 1, &cb, &first, &num);
  if (!cb) {
    if (c1) c1->Release();
    if (cs) cs->Release();
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
    std::lock_guard lock(g_stats_mutex);
    PerShader& ps = g_stats[hash];
    ps.dispatches++;
    if (skip) ps.skipped++;
    memcpy(&ps.flags, data + kCullingFlags, 4);
    memcpy(ps.vpos, data + kVPosToUV, 16);
  } else {
    g_unshadowed.fetch_add(1, std::memory_order_relaxed);
  }
  // The display camera's visibility and hierarchical depth buffer cannot cull
  // the other directions of a sensor rig. Leave square shadow-map jobs alone.
  bool rig_views = rig::armed() && have && f_at(data, kVPosToUV + 8) != f_at(data, kVPosToUV + 12);
  if (rig_views) {
    uint32_t flags;
    memcpy(&flags, data + kCullingFlags, 4);
    flags &= ~(0x10u | 4u);
    memcpy(data + kCullingFlags, &flags, 4);
    memset(data + 544, 0, 8);  // FrustumPlaneCount and AntiFrustumPlaneCount
  }
  if (active && far_mode && !rig_views) {
    cb->Release();
    if (c1) c1->Release();
    ID3D11ComputeShader* v = cs ? far_variant(ctx, cs, cfg.world_lod_distance) : nullptr;
    if (v) {
      ctx->CSSetShader(v, nullptr, 0);
      dispatch();
      ctx->CSSetShader(cs, nullptr, 0);
      g_patched.fetch_add(1, std::memory_order_relaxed);
    } else {
      dispatch();
    }
    if (cs) cs->Release();
    return;
  }
  ID3D11Buffer* own = ((active || rig_views) && have) ? own_buffer(ctx) : nullptr;
  if (!own) {
    cb->Release();
    if (c1) c1->Release();
    if (cs) cs->Release();
    dispatch();
    return;
  }
  if (cfg.world_lod_distance > 0.0f)
    f_set(data, kLODDistanceScale, f_at(data, kLODDistanceScale) / cfg.world_lod_distance);

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
  if (cs) cs->Release();
}

void world_on_present() {
  static ULONGLONG last = 0;
  ULONGLONG now = GetTickCount64();
  if (now - last < 2000) return;
  if (last) {
    const Config& cfg = config();
    ACO_LOG("world: %u culling dispatches, %u patched, %u unshadowed | game LODDistanceScale=%g bias=%g "
            "FOVDistanceFactor=%g MinTriangleArea=%g | lod_distance=%g lod_mode=%s",
            g_dispatches.exchange(0), g_patched.exchange(0), g_unshadowed.exchange(0), g_seen_scale.load(),
            g_seen_bias.load(), g_seen_fov.load(), g_seen_tri.load(), cfg.world_lod_distance, cfg.world_lod_mode.c_str());
  }
  static int n = 0;
  if (last && ++n % 15 == 0) {  // per-shader breakdown every ~30 s
    std::lock_guard lock(g_stats_mutex);
    for (auto& [h, ps] : g_stats)
      ACO_LOG("world:   cs %s: %u dispatches (%u lod_skip) flags=0x%x VPosToUV=(%g %g %g %g)", hex64(h).c_str(),
              ps.dispatches, ps.skipped, ps.flags, ps.vpos[0], ps.vpos[1], ps.vpos[2], ps.vpos[3]);
    g_stats.clear();
  }
  last = now;
}

} // namespace aco
