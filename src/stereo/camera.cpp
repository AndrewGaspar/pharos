#include "stereo/camera.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "stereo/record.h"

namespace aco {

std::atomic<bool> g_cam_on{false};

namespace {

// ---- layouts (float4 registers, from the shaders' reflection data) ------------------------------

constexpr UINT kPassParamsBytes = 2048;  // pool buffer size of PassParams (cb1)
constexpr UINT kInstanceBytes = 512;     // pool buffer size of InstanceParams (cb3)
constexpr UINT kPassSlot = 1, kInstanceSlot = 3;

// PassParams
constexpr int kEyePos = 0, kEyePosPrev = 1, kViewToWorld = 4, kViewToWorldPrev = 8, kWorldToView = 12,
              kWorldToViewPrev = 16, kProj = 20, kViewProj = 24, kVNTP = 28, kViewProjPrev = 32, kVNTPPrev = 36,
              kViewTrans = 40, kViewTransPrev = 41, kFrustum = 46, kClipToView = 57, kClipToWorld = 61;
// InstanceParams
constexpr int kWorld = 0, kWVP = 8, kWVPPrev = 12;

struct F4 {
  float x, y, z, w;
};

inline float dot3(const F4& r, const float d[3]) { return r.x * d[0] + r.y * d[1] + r.z * d[2]; }
inline void add_row(F4& a, const F4& b, float k) {
  a.x += k * b.x;
  a.y += k * b.y;
  a.z += k * b.z;
  a.w += k * b.w;
}

// The view a PassParams describes, if it is a candidate main view: perspective, not square (cube
// faces and shadow views are square or orthographic).
bool is_main_view(const F4* c) {
  const F4& r3 = c[kProj + 3];
  if (std::fabs(r3.z) < 0.5f || std::fabs(r3.w) > 1e-4f || std::fabs(r3.x) > 1e-4f || std::fabs(r3.y) > 1e-4f)
    return false;
  float m00 = c[kProj].x, m11 = c[kProj + 1].y;
  if (std::fabs(m00) < 1e-4f) return false;
  float a = std::fabs(m11 / m00);
  return std::fabs(a - 1.0f) > 0.05f;
}

// Per-eye parameters of one view.
struct Eye {
  float d[3];   // world offset of the eye (this frame)
  float dp[3];  // same with the previous frame's camera
  float k;      // NDC shift (convergence)
};

void eye_params(const F4* c, float sep_m, float conv_m, int eye, Eye* e) {
  float m00 = c[kProj].x;
  float dv = (eye == 0 ? -sep_m : sep_m) * (m00 < 0 ? -1.0f : 1.0f);  // view-space x of the eye
  for (int i = 0; i < 3; ++i) {
    // Camera right vector = column 0 of viewToWorld (rows are output components).
    e->d[i] = dv * (&c[kViewToWorld + i].x)[0];
    e->dp[i] = dv * (&c[kViewToWorldPrev + i].x)[0];
  }
  e->k = conv_m > 0 ? m00 * dv / conv_m : 0.0f;
}

// PassParams of a main view -> the eye's.
void transform_pass(F4* c, const Eye& e, float dv) {
  for (int i = 0; i < 3; ++i) {
    (&c[kEyePos].x)[i] += e.d[i];
    (&c[kEyePosPrev].x)[i] += e.dp[i];
    c[kViewToWorld + i].w += e.d[i];
    c[kViewToWorldPrev + i].w += e.dp[i];
    (&c[kViewTrans].x)[i] += e.d[i];
    (&c[kViewTransPrev].x)[i] += e.dp[i];
  }
  c[kWorldToView].w -= dv;
  c[kWorldToViewPrev].w -= dv;
  add_row(c[kProj], c[kProj + 3], e.k);
  for (int r = 0; r < 4; ++r) {
    c[kViewProj + r].w -= dot3(c[kViewProj + r], e.d);
    c[kViewProjPrev + r].w -= dot3(c[kViewProjPrev + r], e.dp);
  }
  add_row(c[kViewProj], c[kViewProj + 3], e.k);
  add_row(c[kViewProjPrev], c[kViewProjPrev + 3], e.k);
  add_row(c[kVNTP], c[kVNTP + 3], e.k);
  add_row(c[kVNTPPrev], c[kVNTPPrev + 3], e.k);
  for (int r = 0; r < 4; ++r) c[kFrustum + r].w -= dot3(c[kFrustum + r], e.d);
  for (int r = 0; r < 4; ++r) {
    c[kClipToView + r].w -= e.k * c[kClipToView + r].x;
    c[kClipToWorld + r].w -= e.k * c[kClipToWorld + r].x;
  }
  for (int i = 0; i < 3; ++i) add_row(c[kClipToWorld + i], c[kClipToWorld + 3], e.d[i]);
}

// ---- pool --------------------------------------------------------------------------------------

constexpr UINT kPoolBytes = 32u << 20;
constexpr UINT kChunkBytes = 256u << 10;
// The last 4 MB hold immediate-context slots (2 KB granularity), so an immediate-context bind
// that outlives a command list's execution is never overwritten by that list's upload.
constexpr UINT kImmBytes = 4u << 20;
constexpr UINT kRingBytes = kPoolBytes - kImmBytes;

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_imm = nullptr;
ID3D11Buffer* g_pool = nullptr;
std::atomic<UINT> g_ring{0};
UINT g_imm_ring = 0;  // render thread only
bool g_pool_failed = false;

struct Chunk {
  UINT base = 0;  // offset in the pool
  UINT used = 0;
  std::vector<uint8_t> eye[2];
};

std::shared_ptr<Chunk> new_chunk() {
  auto c = std::make_shared<Chunk>();
  c->base = g_ring.fetch_add(kChunkBytes) % kRingBytes;
  c->eye[0].resize(kChunkBytes);
  c->eye[1].resize(kChunkBytes);
  return c;
}

// Attached to a command list (private data), released with it.
// {9c4f2d8e-1b7a-4e3c-8f5d-2a6b0c9e7d14}
const GUID kCamGuid = {0x9c4f2d8e, 0x1b7a, 0x4e3c, {0x8f, 0x5d, 0x2a, 0x6b, 0x0c, 0x9e, 0x7d, 0x14}};
struct ClChunks final : IUnknown {
  std::atomic<ULONG> refs{1};
  std::vector<std::shared_ptr<Chunk>> chunks;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == __uuidof(IUnknown)) {
      *out = this;
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = --refs;
    if (!r) delete this;
    return r;
  }
};

// ---- per-context state -------------------------------------------------------------------------

struct MapOpen {
  void* real = nullptr;
  UINT size = 0;
};

struct MainView {
  bool valid = false;
  F4 vp[4], vpp[4];  // mono viewProj / previous
  Eye eye[2];
};

struct CamCtx {
  ID3D11DeviceContext* ctx = nullptr;
  std::unordered_map<ID3D11Resource*, std::vector<uint8_t>> shadow;  // last written contents
  std::unordered_map<ID3D11Resource*, MapOpen> open;
  std::vector<std::shared_ptr<Chunk>> chunks;  // since the last FinishCommandList
  MainView main;                               // the main view bound at cb1 (latest)
};

constexpr size_t kMaxCtx = 64;
CamCtx g_ctx[kMaxCtx];
std::atomic<size_t> g_nctx{0};
std::mutex g_ctx_mutex;

CamCtx* ctx_state(ID3D11DeviceContext* ctx) {
  size_t n = g_nctx.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i)
    if (g_ctx[i].ctx == ctx) return &g_ctx[i];
  std::lock_guard lock(g_ctx_mutex);
  n = g_nctx.load();
  for (size_t i = 0; i < n; ++i)
    if (g_ctx[i].ctx == ctx) return &g_ctx[i];
  if (n >= kMaxCtx) return nullptr;
  g_ctx[n].ctx = ctx;
  g_nctx.store(n + 1, std::memory_order_release);
  return &g_ctx[n];
}

bool shadow_size(ID3D11Resource* res, UINT* size) {
  D3D11_RESOURCE_DIMENSION dim;
  res->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
  D3D11_BUFFER_DESC d;
  static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
  if (!(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return false;
  if (d.ByteWidth != kPassParamsBytes && d.ByteWidth != kInstanceBytes) return false;
  *size = d.ByteWidth;
  return true;
}

// ---- statistics --------------------------------------------------------------------------------

std::atomic<uint64_t> st_pass_main{0}, st_pass_other{0}, st_inst_main{0}, st_inst_other{0}, st_nodata{0},
    st_uploads{0}, st_upload_bytes{0};

// Copies the slot data into a chunk of this context (new chunk if full). Returns the pool offset.
UINT put_slot(CamCtx* cs, const void* l, const void* r, UINT size) {
  if (cs->chunks.empty() || cs->chunks.back()->used + size > kChunkBytes) cs->chunks.push_back(new_chunk());
  Chunk& ch = *cs->chunks.back();
  memcpy(ch.eye[0].data() + ch.used, l, size);
  memcpy(ch.eye[1].data() + ch.used, r, size);
  UINT off = ch.base + ch.used;
  ch.used += (size + 255) & ~255u;  // constant buffer offsets: multiples of 16 constants
  return off;
}

void upload_chunk(ID3D11DeviceContext* imm, const Chunk& ch, int eye) {
  if (!ch.used) return;
  D3D11_BOX b{ch.base, 0, 0, ch.base + ch.used, 1, 1};
  imm->UpdateSubresource(g_pool, 0, &b, ch.eye[eye].data(), 0, 0);
  st_uploads.fetch_add(1, std::memory_order_relaxed);
  st_upload_bytes.fetch_add(ch.used, std::memory_order_relaxed);
}

}  // namespace

void cam_init(ID3D11Device* dev, ID3D11DeviceContext* imm) {
  if (g_dev == dev) return;
  g_dev = dev;
  g_imm = imm;
  if (g_pool) g_pool->Release();
  g_pool = nullptr;
  D3D11_BUFFER_DESC d{};
  d.ByteWidth = kPoolBytes;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  HRESULT hr = dev->CreateBuffer(&d, nullptr, &g_pool);
  g_pool_failed = FAILED(hr);
  ACO_LOG("stereo: camera constant pool %u MB: 0x%08lx", kPoolBytes >> 20, hr);
}

void cam_map(ID3D11DeviceContext* ctx, ID3D11Resource* res, D3D11_MAP type, D3D11_MAPPED_SUBRESOURCE* m) {
  if (!g_cam_on.load(std::memory_order_relaxed) || type != D3D11_MAP_WRITE_DISCARD || !m || !m->pData) return;
  UINT size;
  if (!shadow_size(res, &size)) return;
  CamCtx* cs = ctx_state(ctx);
  if (!cs) return;
  auto& s = cs->shadow[res];
  s.resize(size);
  cs->open[res] = MapOpen{m->pData, size};
  m->pData = s.data();
}

void cam_unmap(ID3D11DeviceContext* ctx, ID3D11Resource* res) {
  CamCtx* cs = ctx_state(ctx);
  if (!cs) return;
  auto it = cs->open.find(res);
  if (it == cs->open.end()) return;
  memcpy(it->second.real, cs->shadow[res].data(), it->second.size);
  cs->open.erase(it);
}

unsigned cam_filter(ID3D11DeviceContext* ctx, CamStage stage, UINT start, UINT n, ID3D11Buffer* const* bufs,
                    const UINT* first, ID3D11Buffer** out_buf, UINT* out_first, UINT* out_num) {
  (void)stage;
  if (!g_cam_on.load(std::memory_order_relaxed) || !bufs || !g_pool || g_pool_failed) return 0;
  if (start > kInstanceSlot || start + n <= kPassSlot) return 0;
  CamCtx* cs = nullptr;
  unsigned mask = 0;
  const Config& cfg = config();
  for (UINT i = 0; i < n; ++i) {
    UINT slot = start + i;
    if (slot != kPassSlot && slot != kInstanceSlot) continue;
    ID3D11Buffer* b = bufs[i];
    if (!b) continue;
    UINT size;
    if (!shadow_size(b, &size)) continue;
    if ((slot == kPassSlot) != (size == kPassParamsBytes)) continue;
    if (!cs && !(cs = ctx_state(ctx))) return mask;
    auto it = cs->shadow.find(b);
    if (it == cs->shadow.end()) {
      st_nodata.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    UINT off = first ? first[i] * 16 : 0;
    if (off + size > it->second.size()) continue;
    const F4* src = reinterpret_cast<const F4*>(it->second.data() + off);
    alignas(16) F4 l[kPassParamsBytes / 16], r[kPassParamsBytes / 16];
    UINT nreg = size / 16;
    if (slot == kPassSlot) {
      if (!is_main_view(src)) {
        cs->main.valid = false;
        st_pass_other.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      float sep = cfg.stereo_separation * 0.001f;
      MainView& mv = cs->main;
      mv.valid = true;
      memcpy(mv.vp, src + kViewProj, sizeof(mv.vp));
      memcpy(mv.vpp, src + kViewProjPrev, sizeof(mv.vpp));
      for (int e = 0; e < 2; ++e) eye_params(src, sep, cfg.stereo_convergence, e, &mv.eye[e]);
      float m00 = src[kProj].x;
      float dv0 = -sep * (m00 < 0 ? -1.0f : 1.0f), dv1 = -dv0;
      memcpy(l, src, size);
      memcpy(r, src, size);
      transform_pass(l, mv.eye[0], dv0);
      transform_pass(r, mv.eye[1], dv1);
      st_pass_main.fetch_add(1, std::memory_order_relaxed);
    } else {
      // InstanceParams: worldViewProj of the main view? (== viewProj x world)
      const MainView& mv = cs->main;
      if (!mv.valid) {
        st_inst_other.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      const F4* w = src + kWorld;
      const F4* wvp = src + kWVP;
      float err = 0, mag = 1e-6f;
      for (int rr = 0; rr < 4; ++rr)
        for (int cc = 0; cc < 4; ++cc) {
          float v = 0;
          for (int j = 0; j < 4; ++j) v += (&mv.vp[rr].x)[j] * (&w[j].x)[cc];
          float g = (&wvp[rr].x)[cc];
          err = std::fmax(err, std::fabs(v - g));
          mag = std::fmax(mag, std::fabs(g));
        }
      if (err > 1e-3f * mag) {
        st_inst_other.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 6) {
          const F4* pp = nullptr;
          (void)pp;
          ACO_LOG("stereo: InstanceParams not matched (err %g mag %g) world [%g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g] "
                  "wvp [%g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g] vp0 [%g %g %g %g] vp3 [%g %g %g %g] c16 [%g %g %g %g] c17 [%g %g %g %g] c18 [%g %g %g %g]",
                  err, mag, w[0].x, w[0].y, w[0].z, w[0].w, w[1].x, w[1].y, w[1].z, w[1].w, w[2].x, w[2].y, w[2].z, w[2].w,
                  w[3].x, w[3].y, w[3].z, w[3].w, wvp[0].x, wvp[0].y, wvp[0].z, wvp[0].w, wvp[1].x, wvp[1].y, wvp[1].z,
                  wvp[1].w, wvp[2].x, wvp[2].y, wvp[2].z, wvp[2].w, wvp[3].x, wvp[3].y, wvp[3].z, wvp[3].w, mv.vp[0].x,
                  mv.vp[0].y, mv.vp[0].z, mv.vp[0].w, mv.vp[3].x, mv.vp[3].y, mv.vp[3].z, mv.vp[3].w, src[16].x, src[16].y,
                  src[16].z, src[16].w, src[17].x, src[17].y, src[17].z, src[17].w, src[18].x, src[18].y, src[18].z, src[18].w);
        }
        continue;
      }
      memcpy(l, src, size);
      memcpy(r, src, size);
      for (int e = 0; e < 2; ++e) {
        F4* c = e ? r : l;
        const Eye& ey = mv.eye[e];
        for (int rr = 0; rr < 4; ++rr) {
          c[kWVP + rr].w -= dot3(mv.vp[rr], ey.d);
          c[kWVPPrev + rr].w -= dot3(mv.vpp[rr], ey.dp);
        }
        add_row(c[kWVP], c[kWVP + 3], ey.k);
        add_row(c[kWVPPrev], c[kWVPPrev + 3], ey.k);
      }
      st_inst_main.fetch_add(1, std::memory_order_relaxed);
    }
    (void)nreg;
    UINT poff;
    if (ctx == g_imm) {
      // Immediate context: the bind takes effect now; upload the left eye's version right away
      // and have the replay upload the right eye's before its copy of this bind.
      auto ch = std::make_shared<Chunk>();
      UINT bytes = 256 * ((size + 255) / 256);
      if (g_imm_ring + bytes > kImmBytes) g_imm_ring = 0;
      ch->base = kRingBytes + g_imm_ring;
      g_imm_ring += bytes;
      ch->eye[0].resize(bytes);
      ch->eye[1].resize(bytes);
      memcpy(ch->eye[0].data(), l, size);
      memcpy(ch->eye[1].data(), r, size);
      ch->used = UINT(ch->eye[0].size());
      upload_chunk(g_imm, *ch, 0);
      if (g_rec_on.load(std::memory_order_relaxed))
        rec_push([ch](ID3D11DeviceContext* c) { upload_chunk(c, *ch, 1); }, RecKind::Upload);
      poff = ch->base;
    } else {
      poff = put_slot(cs, l, r, size);
    }
    out_buf[i] = g_pool;
    out_first[i] = poff / 16;
    out_num[i] = ((size + 255) / 256) * 16;
    mask |= 1u << i;
  }
  return mask;
}

void cam_finish_command_list(ID3D11DeviceContext* ctx, ID3D11CommandList* cl) {
  CamCtx* cs = ctx_state(ctx);
  if (!cs || cs->chunks.empty()) return;
  auto* h = new ClChunks;
  h->chunks = std::move(cs->chunks);
  cs->chunks.clear();
  cs->main.valid = false;
  if (cl) cl->SetPrivateDataInterface(kCamGuid, h);
  h->Release();
}

void cam_before_execute(ID3D11DeviceContext* imm, ID3D11CommandList* cl, int eye) {
  if (!cl) return;
  IUnknown* u = nullptr;
  UINT size = sizeof(u);
  if (FAILED(cl->GetPrivateData(kCamGuid, &size, &u)) || !u) return;
  auto* h = static_cast<ClChunks*>(u);
  for (auto& ch : h->chunks) upload_chunk(imm, *ch, eye);
  u->Release();
}

void cam_on_present(bool stereo_on) {
  g_cam_on.store(stereo_on && g_pool && !g_pool_failed, std::memory_order_relaxed);
  static uint64_t frames = 0;
  if (!stereo_on) return;
  if (++frames % 300 == 0 || frames == 60) {
    ACO_LOG("stereo: camera binds/frame: PassParams main %.0f other %.0f, InstanceParams main %.0f other %.0f, no data %.0f; uploads %.0f (%.2f MB)",
            st_pass_main.exchange(0) / 300.0, st_pass_other.exchange(0) / 300.0, st_inst_main.exchange(0) / 300.0,
            st_inst_other.exchange(0) / 300.0, st_nodata.exchange(0) / 300.0, st_uploads.exchange(0) / 300.0,
            st_upload_bytes.exchange(0) / 300.0 / 1048576.0);
  }
}

} // namespace aco
