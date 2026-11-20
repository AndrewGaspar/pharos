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
constexpr UINT kMaxScanBytes = 65536;  // constant buffers shadowed while stereo runs (one bind's maximum)

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

// ---- passes drawn for the main view without its projection ------------------------------------
// Fullscreen passes (sky/cloud composite, ...) bind a PassParams with a flat projection but the
// main camera's matrices, and rebuild view directions from them. Left alone, both eyes rebuild the
// same directions: the sky lands at screen depth. Such a PassParams (non-perspective projection,
// camera equal to the latest main view's) gets the eye's camera; its projection stays.
std::mutex g_main_mutex;
F4 g_main_v2w[3];        // latest main view's viewToWorld rows (mono)
float g_main_m00 = 0;    // and its projection's m00
bool g_main_valid = false;

void note_main(const F4* c) {
  std::lock_guard lock(g_main_mutex);
  memcpy(g_main_v2w, c + kViewToWorld, sizeof(g_main_v2w));
  g_main_m00 = c[kProj].x;
  g_main_valid = true;
}

bool matches_main(const F4* c, float* m00) {
  std::lock_guard lock(g_main_mutex);
  if (!g_main_valid) return false;
  for (int r = 0; r < 3; ++r) {
    const float* a = &c[kViewToWorld + r].x;
    const float* b = &g_main_v2w[r].x;
    for (int i = 0; i < 3; ++i)
      if (std::fabs(a[i] - b[i]) > 1e-3f) return false;
    if (std::fabs(a[3] - b[3]) > 0.01f) return false;  // position, metres
  }
  *m00 = g_main_m00;
  return true;
}

// A matrix whose last row is (0, 0, 0, 1) isn't a perspective one.
inline bool perspective_rows(const F4* m) {
  return std::fabs(m[3].x) + std::fabs(m[3].y) + std::fabs(m[3].z) > 0.5f;
}

void transform_screen_pass(F4* c, const Eye& e, float dv) {
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
  if (perspective_rows(c + kViewProj)) {
    for (int r = 0; r < 4; ++r) c[kViewProj + r].w -= dot3(c[kViewProj + r], e.d);
    add_row(c[kViewProj], c[kViewProj + 3], e.k);
  }
  if (perspective_rows(c + kViewProjPrev)) {
    for (int r = 0; r < 4; ++r) c[kViewProjPrev + r].w -= dot3(c[kViewProjPrev + r], e.dp);
    add_row(c[kViewProjPrev], c[kViewProjPrev + 3], e.k);
  }
  if (perspective_rows(c + kVNTP)) add_row(c[kVNTP], c[kVNTP + 3], e.k);
  if (perspective_rows(c + kVNTPPrev)) add_row(c[kVNTPPrev], c[kVNTPPrev + 3], e.k);
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
  // For finding copies of this camera in other constant buffers (mono, this frame / previous):
  float pos[3], posp[3];      // camera position
  float c0[3], c1[3], f[3];   // right / m00, up / m11, forward (the view's z axis)
  float c0p[3], c1p[3], fp[3];
};

struct CamCtx {
  ID3D11DeviceContext* ctx = nullptr;
  // The immediate context is used from several game threads (maps from task threads): every
  // access to this state holds the lock (uncontended for deferred contexts).
  std::mutex mutex;
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
  if (d.ByteWidth > kMaxScanBytes) return false;
  *size = d.ByteWidth;
  return true;
}

// ---- copies of the camera in other constant buffers ----------------------------------------------
// Effects with their own constant buffers carry copies of the main camera (measured, cap at the
// Alexandria harbour): SSR's worldToProj/prevWorldToProj and the sun flare's ViewProj (= viewProj
// with a linear depth row), the fog's and shadow stabilisation's SSToWS/LastSSToWS (columns
// right/m00, up/m11, -forward, camera position), the cloud composite's Proj2World (same columns,
// -forward in .w: the sky's directions), and plain camera positions (fog EyePositionWS). Left
// alone, both eyes use the same copy: reflections stick to the water's surface, the clouds land
// near screen depth. Each copy found in a bound buffer is replaced with the eye's version.

inline bool near3(const float* a, const float* b, float tol) {
  return std::fabs(a[0] - b[0]) < tol && std::fabs(a[1] - b[1]) < tol && std::fabs(a[2] - b[2]) < tol;
}

// Screen-to-world matrix at c: columns 0 and 1 = c0/c1, a column in {2,3} = +-forward. Returns
// that column (2 or 3), or -1; *sign = the forward column's sign. With -forward (the view ray of
// NDC x is x*c0 - forward) the eye's NDC shift k moves it by -k*c0; with +forward (the ray is
// negated, so x runs the other way: measured on the cloud composite) by +k*c0.
int match_ss2ws(const F4* c, const float* c0, const float* c1, const float* f, float* sign) {
  float m0[3] = {c[0].x, c[1].x, c[2].x}, m1[3] = {c[0].y, c[1].y, c[2].y};
  if (!near3(m0, c0, 2e-3f) || !near3(m1, c1, 2e-3f)) return -1;
  for (int col = 2; col < 4; ++col) {
    float v[3] = {(&c[0].x)[col], (&c[1].x)[col], (&c[2].x)[col]};
    float nf[3] = {-f[0], -f[1], -f[2]};
    if (near3(v, f, 2e-3f)) {
      *sign = 1.0f;
      return col;
    }
    if (near3(v, nf, 2e-3f)) {
      *sign = -1.0f;
      return col;
    }
  }
  return -1;
}

// viewProj-like matrix at c: rows 0, 1 and 3 equal vp's (row 2 may hold another depth mapping).
bool match_vp(const F4* c, const F4* vp) {
  return near3(&c[0].x, &vp[0].x, 2e-3f) && near3(&c[1].x, &vp[1].x, 2e-3f) && near3(&c[3].x, &vp[3].x, 2e-3f) &&
         std::fabs(c[3].w - vp[3].w) < 0.05f;
}

// Finds camera copies in src (nreg float4 registers) and writes the eyes' versions into out[e].
// Returns the number of copies found.
int scan_other(const F4* src, UINT nreg, const MainView& mv, F4* out0, F4* out1, unsigned kinds) {
  int found = 0;
  F4* out[2] = {out0, out1};
  for (UINT r = 0; r < nreg;) {
    if (r + 4 <= nreg) {
      bool cur = (kinds & 1) && match_vp(src + r, mv.vp), prev = (kinds & 1) && !cur && match_vp(src + r, mv.vpp);
      if (cur || prev) {
        for (int e = 0; e < 2; ++e) {
          F4* m = out[e] + r;
          const float* d = cur ? mv.eye[e].d : mv.eye[e].dp;
          for (int i = 0; i < 4; ++i) m[i].w -= dot3(m[i], d);
          add_row(m[0], m[3], mv.eye[e].k);
        }
        found++;
        r += 4;
        continue;
      }
      float fs = 0;
      int fc = (kinds & 2) ? match_ss2ws(src + r, mv.c0, mv.c1, mv.f, &fs) : -1;
      bool sprev = false;
      if (fc < 0 && (kinds & 2)) {
        fc = match_ss2ws(src + r, mv.c0p, mv.c1p, mv.fp, &fs);
        sprev = fc >= 0;
      }
      if (fc >= 0) {
        const float* pos = sprev ? mv.posp : mv.pos;
        float t[3] = {src[r].w, src[r + 1].w, src[r + 2].w};
        bool translated = fc == 2 && near3(t, pos, 0.02f);
        for (int e = 0; e < 2; ++e) {
          F4* m = out[e] + r;
          const float* d = sprev ? mv.eye[e].dp : mv.eye[e].d;
          for (int i = 0; i < 3; ++i) {
            (&m[i].x)[fc] += fs * mv.eye[e].k * m[i].x;
            if (translated) m[i].w += d[i];
          }
        }
        found++;
        r += 4;
        continue;
      }
    }
    bool pc = (kinds & 4) && near3(&src[r].x, mv.pos, 0.01f), pp = (kinds & 4) && !pc && near3(&src[r].x, mv.posp, 0.01f);
    if (pc || pp) {
      for (int e = 0; e < 2; ++e) {
        const float* d = pc ? mv.eye[e].d : mv.eye[e].dp;
        for (int i = 0; i < 3; ++i) (&out[e][r].x)[i] += d[i];
      }
      found++;
    }
    ++r;
  }
  return found;
}

// ---- statistics --------------------------------------------------------------------------------

std::atomic<uint64_t> st_other_cb{0}, st_pass_main{0}, st_pass_screen{0}, st_pass_other{0}, st_inst_main{0}, st_inst_other{0}, st_nodata{0},
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
  g_pool_failed = false;
}

namespace {
void create_pool() {
  ID3D11Device* dev = g_dev;
  D3D11_BUFFER_DESC d{};
  d.ByteWidth = kPoolBytes;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  HRESULT hr = dev->CreateBuffer(&d, nullptr, &g_pool);
  g_pool_failed = FAILED(hr);
  ACO_LOG("stereo: camera constant pool %u MB: 0x%08lx", kPoolBytes >> 20, hr);
}
}  // namespace

void cam_map(ID3D11DeviceContext* ctx, ID3D11Resource* res, D3D11_MAP type, D3D11_MAPPED_SUBRESOURCE* m) {
  if (!g_cam_on.load(std::memory_order_relaxed) || type != D3D11_MAP_WRITE_DISCARD || !m || !m->pData) return;
  UINT size;
  if (!shadow_size(res, &size)) return;
  CamCtx* cs = ctx_state(ctx);
  if (!cs) return;
  std::lock_guard lock(cs->mutex);
  auto& s = cs->shadow[res];
  s.resize(size);
  cs->open[res] = MapOpen{m->pData, size};
  m->pData = s.data();
}

void cam_unmap(ID3D11DeviceContext* ctx, ID3D11Resource* res) {
  CamCtx* cs = ctx_state(ctx);
  if (!cs) return;
  std::lock_guard lock(cs->mutex);
  auto it = cs->open.find(res);
  if (it == cs->open.end()) return;
  memcpy(it->second.real, cs->shadow[res].data(), it->second.size);
  cs->open.erase(it);
}

unsigned cam_filter(ID3D11DeviceContext* ctx, CamStage stage, UINT start, UINT n, ID3D11Buffer* const* bufs,
                    const UINT* first, ID3D11Buffer** out_buf, UINT* out_first, UINT* out_num) {
  (void)stage;
  if (!g_cam_on.load(std::memory_order_relaxed) || !bufs || !g_pool || g_pool_failed) return 0;
  if (start + n <= kPassSlot) return 0;
  CamCtx* cs = ctx_state(ctx);
  if (!cs) return 0;
  std::lock_guard cs_lock(cs->mutex);
  unsigned mask = 0;
  const Config& cfg = config();
  for (UINT i = 0; i < n; ++i) {
    UINT slot = start + i;
    // cb0 (FrameParams) and cb2 (MaterialParams) are bound for every draw and hold no camera.
    if (slot == 0 || slot == 2) continue;
    ID3D11Buffer* b = bufs[i];
    if (!b) continue;
    UINT size;
    if (!shadow_size(b, &size)) continue;
    bool other = slot != kPassSlot && slot != kInstanceSlot;
    if (!other && (slot == kPassSlot) != (size == kPassParamsBytes)) continue;
    if (other && !cs && !(cs = ctx_state(ctx))) return mask;
    if (other && !cs->main.valid) continue;
    if (!cs && !(cs = ctx_state(ctx))) return mask;
    auto it = cs->shadow.find(b);
    if (it == cs->shadow.end()) {
      st_nodata.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    UINT off = first ? first[i] * 16 : 0;
    if (off + size > it->second.size()) continue;
    const F4* src = reinterpret_cast<const F4*>(it->second.data() + off);
    thread_local std::vector<F4> lbuf, rbuf;
    UINT nreg = size / 16;
    if (lbuf.size() < nreg) {
      lbuf.resize(nreg);
      rbuf.resize(nreg);
    }
    F4* l = lbuf.data();
    F4* r = rbuf.data();
    if (other) {
      memcpy(l, src, size);
      memcpy(r, src, size);
      unsigned kinds = (cfg.stereo_copies.find("vp") != std::string::npos ? 1u : 0u) |
                       (cfg.stereo_copies.find("ss2ws") != std::string::npos ? 2u : 0u) |
                       (cfg.stereo_copies.find("pos") != std::string::npos ? 4u : 0u);
      if (!kinds || !scan_other(src, nreg, cs->main, l, r, kinds)) continue;
      st_other_cb.fetch_add(1, std::memory_order_relaxed);
    } else if (slot == kPassSlot) {
      float sep = cfg.stereo_separation * 0.001f;
      if (!is_main_view(src)) {
        cs->main.valid = false;
        float m00 = 0;
        if (perspective_rows(src + kProj) || !matches_main(src, &m00)) {
          st_pass_other.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        // A screen pass of the main view: eye parameters from the main projection.
        F4 tmp[kPassParamsBytes / 16];
        memcpy(tmp, src, size);
        tmp[kProj].x = m00;
        Eye ey[2];
        for (int e = 0; e < 2; ++e) eye_params(tmp, sep, cfg.stereo_convergence, e, &ey[e]);
        float dv0 = -sep * (m00 < 0 ? -1.0f : 1.0f);
        memcpy(l, src, size);
        memcpy(r, src, size);
        transform_screen_pass(l, ey[0], dv0);
        transform_screen_pass(r, ey[1], -dv0);
        st_pass_screen.fetch_add(1, std::memory_order_relaxed);
      } else {
      note_main(src);
      MainView& mv = cs->main;
      mv.valid = true;
      {
        float m00 = src[kProj].x, m11 = src[kProj + 1].y;
        for (int i = 0; i < 3; ++i) {
          const F4& v = src[kViewToWorld + i];
          const F4& w = src[kViewToWorldPrev + i];
          mv.pos[i] = v.w;
          mv.posp[i] = w.w;
          mv.c0[i] = v.x / m00;
          mv.c1[i] = v.y / m11;
          mv.f[i] = v.z;
          mv.c0p[i] = w.x / m00;
          mv.c1p[i] = w.y / m11;
          mv.fp[i] = w.z;
        }
      }
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
      }
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
  if (!cs) return;
  std::lock_guard lock(cs->mutex);
  if (cs->chunks.empty()) return;
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
  if (stereo_on && !g_pool && !g_pool_failed && g_dev) create_pool();  // only once stereo runs
  g_cam_on.store(stereo_on && g_pool && !g_pool_failed, std::memory_order_relaxed);
  static uint64_t frames = 0;
  if (!stereo_on) return;
  if (++frames % 300 == 0 || frames == 60) {
    ACO_LOG("stereo: camera binds/frame: PassParams main %.0f screen %.0f other %.0f, InstanceParams main %.0f other %.0f, no data %.0f, other buffers with camera copies %.0f; uploads %.0f (%.2f MB)",
            st_pass_main.exchange(0) / 300.0, st_pass_screen.exchange(0) / 300.0, st_pass_other.exchange(0) / 300.0, st_inst_main.exchange(0) / 300.0,
            st_inst_other.exchange(0) / 300.0, st_nodata.exchange(0) / 300.0, st_other_cb.exchange(0) / 300.0, st_uploads.exchange(0) / 300.0,
            st_upload_bytes.exchange(0) / 300.0 / 1048576.0);
  }
}

} // namespace aco
