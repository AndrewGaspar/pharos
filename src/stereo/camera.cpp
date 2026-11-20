#include "stereo/camera.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/config.h"
#include "core/hook.h"
#include "core/log.h"
#include "core/vtidx.h"
#include "stereo/record.h"
#include "stereo/stereo.h"

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

// ---- passes drawn for the main view without its projection ------------------------------------
// Fullscreen passes (sky/cloud composite, ...) bind a PassParams with a flat projection but the
// main camera's matrices, and rebuild view directions from them. Left alone, both eyes rebuild the
// same directions: the sky lands at screen depth. Such a PassParams (non-perspective projection,
// camera equal to the latest main view's) gets the eye's camera; its projection stays.
struct MainView {
  bool valid = false;
  F4 vp[4], vpp[4];  // mono viewProj / previous
  Eye eye[2];
  // For recognising copies of this camera in effect constant buffers (mono; p = previous frame):
  float pos[3], posp[3];      // camera position
  float c0[3], c1[3];         // right / m00, up / m11 (screen-to-world columns)
  float c0p[3], c1p[3];
  float right[3], look[3];    // unit vectors
  float m00 = 0;
};

void fill_main(MainView& mv, const F4* c) {
  float m00 = c[kProj].x, m11 = c[kProj + 1].y;
  mv.m00 = m00;
  for (int i = 0; i < 3; ++i) {
    const F4& v = c[kViewToWorld + i];
    const F4& w = c[kViewToWorldPrev + i];
    mv.pos[i] = v.w;
    mv.posp[i] = w.w;
    mv.c0[i] = v.x / m00;
    mv.c1[i] = v.y / m11;
    mv.c0p[i] = w.x / m00;
    mv.c1p[i] = w.y / m11;
    mv.right[i] = v.x;
    mv.look[i] = -v.z;  // the view looks down -z
  }
}

std::mutex g_main_mutex;
F4 g_main_v2w[3];        // latest main view's viewToWorld rows (mono)
float g_main_m00 = 0;    // and its projection's m00
bool g_main_valid = false;
MainView g_last_main;    // and the rest of it (effect cameras, any context)

void note_main(const F4* c, const MainView& mv) {
  std::lock_guard lock(g_main_mutex);
  memcpy(g_main_v2w, c + kViewToWorld, sizeof(g_main_v2w));
  g_main_m00 = c[kProj].x;
  g_main_valid = true;
  g_last_main = mv;
}

bool last_main(MainView* out) {
  std::lock_guard lock(g_main_mutex);
  if (!g_last_main.valid) return false;
  *out = g_last_main;
  return true;
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
UINT g_imm_ring = 0;
std::mutex g_imm_ring_mutex;

// A slot of the immediate context's ring.
UINT imm_alloc(UINT bytes) {
  std::lock_guard lock(g_imm_ring_mutex);
  if (g_imm_ring + bytes > kImmBytes) g_imm_ring = 0;
  UINT base = kRingBytes + g_imm_ring;
  g_imm_ring += bytes;
  return base;
}
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

// Last written contents of a shadowed constant buffer. `valid` is false once the buffer was
// written some other way (WRITE_NO_OVERWRITE, a partial UpdateSubresource): then the shadow no
// longer holds what the GPU will read.
struct Shadow {
  std::vector<uint8_t> data;
  bool valid = false;
};

struct CamCtx {
  ID3D11DeviceContext* ctx = nullptr;
  // The immediate context's maps can come from several game threads: every access to this state
  // holds the lock (uncontended for deferred contexts).
  std::mutex mutex;
  std::unordered_map<ID3D11Resource*, Shadow> shadow;
  std::unordered_map<ID3D11Resource*, MapOpen> open;
  std::vector<std::shared_ptr<Chunk>> chunks;  // since the last FinishCommandList
  MainView main;                               // the main view bound at cb1 (latest)
  MainView last_main;                          // the latest main view of this command list
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

// Sizes of the effect constant buffers (learned at their draws/dispatches: a buffer bound there
// without a shadow adds its size, and its later writes are shadowed).
constexpr size_t kMaxFxSizes = 8;
std::atomic<UINT> g_fx_sizes[kMaxFxSizes];
std::atomic<size_t> g_fx_nsizes{0};
std::mutex g_fx_sizes_mutex;

bool fx_size(UINT bw) {
  size_t n = g_fx_nsizes.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i)
    if (g_fx_sizes[i].load(std::memory_order_relaxed) == bw) return true;
  return false;
}

void learn_fx_size(UINT bw) {
  if (bw == kPassParamsBytes || bw == kInstanceBytes || bw > 65536 || fx_size(bw)) return;
  std::lock_guard lock(g_fx_sizes_mutex);
  size_t n = g_fx_nsizes.load();
  if (fx_size(bw) || n >= kMaxFxSizes) return;
  g_fx_sizes[n].store(bw, std::memory_order_relaxed);
  g_fx_nsizes.store(n + 1, std::memory_order_release);
  ACO_LOG("stereo: shadowing %u-byte constant buffers (effect cameras)", bw);
}

// Effect buffers are often written on one context and bound on another (the fog's on another
// deferred context): their latest write from any context, as a fallback.
std::mutex g_fx_last_mutex;
std::unordered_map<ID3D11Resource*, Shadow> g_fx_last;

void note_fx_write(ID3D11Resource* res, const std::vector<uint8_t>& data, bool valid) {
  if (data.size() == kPassParamsBytes || data.size() == kInstanceBytes) return;
  std::lock_guard lock(g_fx_last_mutex);
  Shadow& s = g_fx_last[res];
  if (valid) s.data = data;
  s.valid = valid;
}

bool shadow_size(ID3D11Resource* res, UINT* size) {
  D3D11_RESOURCE_DIMENSION dim;
  res->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
  D3D11_BUFFER_DESC d;
  static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
  if (!(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return false;
  if (d.ByteWidth != kPassParamsBytes && d.ByteWidth != kInstanceBytes && !fx_size(d.ByteWidth)) return false;
  *size = d.ByteWidth;
  return true;
}

// ---- statistics --------------------------------------------------------------------------------

std::atomic<uint64_t> st_pass_main{0}, st_pass_screen{0}, st_pass_other{0}, st_inst_main{0}, st_inst_other{0}, st_nodata{0},
    st_uploads{0}, st_upload_bytes{0}, st_fx_draws{0}, st_fx_moved{0}, st_fx_nodata{0}, st_fx_nomatch{0}, st_fx_cross{0};

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

// ---- effect cameras ----------------------------------------------------------------------------
// Effects with constant buffers of their own carry copies of the main camera, outside PassParams
// (names from the shaders' reflection data, values checked on captures at the Alexandria harbour
// and Giza). Left alone, both eyes use the left eye's copy: the right eye's reflections, fog and
// clouds are drawn from the wrong place (the sky's clouds land near screen depth).
//
// The shaders that read them are recognised when the game creates them (cbuffer names in the
// bytecode; offsets of the fields from the reflection data). At each of their draws/dispatches the
// buffer bound at that slot is copied into the pool with the fields moved to each eye (each field
// is first checked against the main view's camera: a copy of something else is left alone), the
// slot is pointed at the copy for that one call, and then given back.

enum FxKind : uint8_t {
  kFxVP,         // world -> clip (rows): viewProj with its own depth row
  kFxVPPrev,     //   the previous frame's
  kFxSS2WS,      // (x z, y z, z, 1) -> world (rows), x/y = NDC: columns right/m00, up/m11, look, position
  kFxSS2WSPrev,  //   the previous frame's
  kFxPos,        // camera position
  kFxP2W,        // cloud composite: (cs.x, cs.y, 1) -> view direction (rows, .x .y .w), cs ~ NDC
  kFxRayTL,      // cloud ray march: ray = PlaneTopLeft + u PlaneRight - v PlaneUp
  kFxRayR,
};

enum FxGroup : uint8_t { kGrSSR, kGrFog, kGrClouds, kGrCloudMarch, kGrShadow, kGrFlare, kGrCount };
const char* const kGroupNames[kGrCount] = {"ssr", "fog", "clouds", "cloudmarch", "shadow", "flare"};

struct FxFieldName {
  const char* name;
  FxKind kind;
};
struct FxCbuffer {
  const char* cbuffer;
  FxGroup group;
  FxFieldName fields[4];
};
const FxCbuffer kFxCbuffers[] = {
    {"NewSSLRShared_cbuffer", kGrSSR, {{"worldToProj", kFxVP}, {"prevWorldToProj", kFxVPPrev}}},
    {"VolumetricFogAllParamsCustomLayout_cbuffer", kGrFog, {{"SSToWS", kFxSS2WS}, {"EyePositionWS", kFxPos}}},
    {"CloudsCompositeViewCache_cbuffer", kGrClouds, {{"Proj2World", kFxP2W}}},
    {"CloudsRayMarch_cbuffer", kGrCloudMarch, {{"PlaneTopLeft", kFxRayTL}, {"PlaneRight", kFxRayR}}},
    {"ShadowMaskStabilization_cbuffer", kGrShadow,
     {{"LastSSToWS", kFxSS2WSPrev}, {"SSToWS", kFxSS2WS}}},
    {"SunFlareOcclusion_cbuffer", kGrFlare, {{"ViewProj", kFxVP}}},
};

struct FxField {
  FxKind kind;
  UINT offset;  // bytes in the cbuffer
};
struct FxSlot {
  UINT slot;
  FxGroup group;
  std::vector<FxField> fields;
};
struct FxShader {
  uint64_t hash = 0;
  std::vector<FxSlot> slots;
};

std::shared_mutex g_fx_mutex;
std::unordered_map<ID3D11DeviceChild*, FxShader> g_fx;

// ---- DXBC reflection (RDEF chunk) ----
struct Rdef {
  const uint8_t* p = nullptr;
  size_t n = 0;
  bool sm5 = false;
  bool ok(size_t off, size_t len) const { return off <= n && len <= n - off; }
  uint32_t u32(size_t off) const {
    uint32_t v = 0;
    if (ok(off, 4)) memcpy(&v, p + off, 4);
    return v;
  }
  uint16_t u16(size_t off) const {
    uint16_t v = 0;
    if (ok(off, 2)) memcpy(&v, p + off, 2);
    return v;
  }
  bool name_is(uint32_t off, const char* s) const {
    size_t l = strlen(s);
    return ok(off, l + 1) && memcmp(p + off, s, l + 1) == 0;
  }
};

bool find_rdef(const uint8_t* bc, size_t n, Rdef* r) {
  if (n < 32 || memcmp(bc, "DXBC", 4) != 0) return false;
  uint32_t count;
  memcpy(&count, bc + 28, 4);
  if (count > 64 || 32 + size_t(count) * 4 > n) return false;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t off;
    memcpy(&off, bc + 32 + i * 4, 4);
    if (off + 8ull > n) continue;
    if (memcmp(bc + off, "RDEF", 4) != 0) continue;
    uint32_t len;
    memcpy(&len, bc + off + 4, 4);
    if (off + 8ull + len > n) return false;
    r->p = bc + off + 8;
    r->n = len;
    r->sm5 = r->ok(0, 28) && r->p[13] >= 5;  // major version
    return true;
  }
  return false;
}

// Offset of the first member called `name` under a type (structs and arrays of them: element 0).
bool find_member(const Rdef& r, uint32_t type_off, const char* name, UINT base, UINT* out, int depth) {
  if (depth > 6 || !r.ok(type_off, 12)) return false;
  uint16_t cls = r.u16(type_off), members = r.u16(type_off + 10);
  uint32_t moff = r.u32(type_off + 12);
  if (cls != 5 /* D3D_SVC_STRUCT */ || !members) return false;
  for (uint16_t m = 0; m < members; ++m) {
    uint32_t md = moff + m * 12u;
    if (!r.ok(md, 12)) return false;
    uint32_t mname = r.u32(md), mtype = r.u32(md + 4), mo = r.u32(md + 8);
    if (r.name_is(mname, name)) {
      *out = base + mo;
      return true;
    }
    if (find_member(r, mtype, name, base + mo, out, depth + 1)) return true;
  }
  return false;
}

// Offset of variable `name` in cbuffer `cb` (top level or a member of a struct variable).
bool find_var(const Rdef& r, uint32_t cb, const char* name, UINT* out) {
  uint32_t nvars = r.u32(cb + 4), voff = r.u32(cb + 8);
  size_t vsz = r.sm5 ? 40 : 24;
  if (nvars > 4096) return false;
  for (uint32_t v = 0; v < nvars; ++v) {
    size_t vd = voff + v * vsz;
    if (!r.ok(vd, vsz)) return false;
    uint32_t vname = r.u32(vd), vstart = r.u32(vd + 4), vtype = r.u32(vd + 16);
    if (r.name_is(vname, name)) {
      *out = vstart;
      return true;
    }
    if (find_member(r, vtype, name, vstart, out, 0)) return true;
  }
  return false;
}

bool parse_fx(const uint8_t* bc, size_t n, FxShader* fx) {
  Rdef r;
  if (!find_rdef(bc, n, &r) || !r.ok(0, 28)) return false;
  uint32_t ncb = r.u32(0), cboff = r.u32(4), nbind = r.u32(8), bindoff = r.u32(12);
  if (ncb > 64 || nbind > 512) return false;
  for (const FxCbuffer& want : kFxCbuffers) {
    // Bind point of the cbuffer.
    int slot = -1;
    for (uint32_t b = 0; b < nbind; ++b) {
      size_t bd = bindoff + b * 32u;
      if (!r.ok(bd, 32)) return false;
      if (r.u32(bd + 4) == 0 /* D3D_SIT_CBUFFER */ && r.name_is(r.u32(bd), want.cbuffer)) {
        slot = int(r.u32(bd + 20));
        break;
      }
    }
    if (slot < 0 || slot >= 14) continue;
    for (uint32_t c = 0; c < ncb; ++c) {
      size_t cd = cboff + c * 24u;
      if (!r.ok(cd, 24)) return false;
      if (!r.name_is(r.u32(cd), want.cbuffer)) continue;
      FxSlot fs{UINT(slot), want.group, {}};
      for (const FxFieldName& f : want.fields) {
        if (!f.name) break;
        UINT off;
        if (find_var(r, uint32_t(cd), f.name, &off) && off % 16 == 0) fs.fields.push_back({f.kind, off});
      }
      if (!fs.fields.empty()) fx->slots.push_back(std::move(fs));
      break;
    }
  }
  return !fx->slots.empty();
}

// ---- per-eye versions of the fields ----

inline float dot3f(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline bool near3(const float* a, const float* b, float tol) {
  return std::fabs(a[0] - b[0]) <= tol && std::fabs(a[1] - b[1]) <= tol && std::fabs(a[2] - b[2]) <= tol;
}
inline float len3(const float* a) { return std::sqrt(dot3f(a, a)); }
inline bool near_rel(const float* a, const float* b, float rel) { return near3(a, b, rel * (len3(b) + 1e-6f)); }

// NDC x of a view direction (mono main view; m00's sign included).
inline float ndc_x(const MainView& mv, const float* dir) {
  float z = dot3f(dir, mv.look);
  return std::fabs(z) < 1e-9f ? 0.0f : mv.m00 * dot3f(dir, mv.right) / z;
}

bool match_vp(const F4* m, const F4* vp) {
  for (int r : {0, 1, 3})
    if (!near_rel(&m[r].x, &vp[r].x, 2e-3f) || std::fabs(m[r].w - vp[r].w) > 2e-3f * (std::fabs(vp[r].w) + 1.0f))
      return false;
  return true;
}

// Columns 0 and 1 of a screen-to-world matrix.
bool match_ss2ws(const F4* m, const float* c0, const float* c1) {
  float a[3] = {m[0].x, m[1].x, m[2].x}, b[3] = {m[0].y, m[1].y, m[2].y};
  return near_rel(a, c0, 2e-3f) && near_rel(b, c1, 2e-3f);
}

const char* kind_name(FxKind k) {
  switch (k) {
    case kFxVP: return "vp";
    case kFxVPPrev: return "vp-prev";
    case kFxSS2WS: return "ss2ws";
    case kFxSS2WSPrev: return "ss2ws-prev";
    case kFxPos: return "pos";
    case kFxP2W: return "proj2world";
    case kFxRayTL: return "ray";
    case kFxRayR: return "ray-right";
  }
  return "?";
}

void log_field(const char* what, uint64_t hash, UINT slot, const FxField& f, const F4* m, int nreg, const MainView& mv) {
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) >= 40) return;
  char buf[1024];
  int o = snprintf(buf, sizeof(buf), "stereo: effect %s %s cb%u+%u (shader %016llx):", what, kind_name(f.kind), slot, f.offset,
                   static_cast<unsigned long long>(hash));
  for (int i = 0; i < nreg && o < int(sizeof(buf)) - 64; ++i)
    o += snprintf(buf + o, sizeof(buf) - o, " [%g %g %g %g]", m[i].x, m[i].y, m[i].z, m[i].w);
  snprintf(buf + o, sizeof(buf) - o, " | main pos %g %g %g right %g %g %g look %g %g %g m00 %g", mv.pos[0], mv.pos[1], mv.pos[2],
           mv.right[0], mv.right[1], mv.right[2], mv.look[0], mv.look[1], mv.look[2], mv.m00);
  ACO_LOG("%s", buf);
}

// Moves the fields of one constant buffer (l = left eye's copy, r = right's; both start as the
// game's data, `bytes` long). Returns the number of fields moved.
int move_fields(const FxShader& fs, const FxSlot& slot, const MainView& mv, uint8_t* l, uint8_t* r, UINT bytes) {
  int moved = 0;
  uint8_t* out[2] = {l, r};
  // The ray march's plane pair is handled together.
  int ray_tl = -1, ray_r = -1;
  for (const FxField& f : slot.fields) {
    if (f.kind == kFxRayTL) ray_tl = int(f.offset);
    if (f.kind == kFxRayR) ray_r = int(f.offset);
  }
  for (const FxField& f : slot.fields) {
    UINT need = f.kind == kFxPos || f.kind == kFxRayTL || f.kind == kFxRayR ? 16 : 64;
    if (f.offset + need > bytes) continue;
    const F4* m = reinterpret_cast<const F4*>(l + f.offset);
    bool ok = false;
    switch (f.kind) {
      case kFxVP:
      case kFxVPPrev: {
        bool cur = match_vp(m, mv.vp), prev = match_vp(m, mv.vpp);
        if (!cur && !prev) break;
        bool use_prev = prev && (f.kind == kFxVPPrev || !cur);
        F4 mm[4];
        memcpy(mm, m, sizeof(mm));
        for (int e = 0; e < 2; ++e) {
          F4* d = reinterpret_cast<F4*>(out[e] + f.offset);
          memcpy(d, mm, sizeof(mm));
          const float* dd = use_prev ? mv.eye[e].dp : mv.eye[e].d;
          for (int i = 0; i < 4; ++i) d[i].w -= dot3(d[i], dd);
          add_row(d[0], d[3], mv.eye[e].k);
        }
        ok = true;
        break;
      }
      case kFxSS2WS:
      case kFxSS2WSPrev: {
        bool cur = match_ss2ws(m, mv.c0, mv.c1), prev = match_ss2ws(m, mv.c0p, mv.c1p);
        if (!cur && !prev) break;
        bool use_prev = prev && (f.kind == kFxSS2WSPrev || !cur);
        F4 mm[4];
        memcpy(mm, m, sizeof(mm));
        bool affine = std::fabs(mm[3].x) + std::fabs(mm[3].y) + std::fabs(mm[3].z) < 1e-6f && std::fabs(mm[3].w - 1.0f) < 1e-6f;
        for (int e = 0; e < 2; ++e) {
          F4* d = reinterpret_cast<F4*>(out[e] + f.offset);
          memcpy(d, mm, sizeof(mm));
          const float* dd = use_prev ? mv.eye[e].dp : mv.eye[e].d;
          float k = mv.eye[e].k;
          // The eye's NDC x is the centre's + k: (x z) -> ((x - k) z), i.e. column 2 -= k column 0.
          for (int i = 0; i < 4; ++i) d[i].z -= k * d[i].x;
          // Then the eye's position.
          if (affine) {
            for (int i = 0; i < 3; ++i) d[i].w += dd[i];
          } else {
            for (int i = 0; i < 3; ++i) add_row(d[i], d[3], dd[i]);
          }
        }
        ok = true;
        break;
      }
      case kFxPos: {
        bool cur = near3(&m->x, mv.pos, 0.01f), prev = near3(&m->x, mv.posp, 0.01f);
        if (!cur && !prev) break;
        for (int e = 0; e < 2; ++e) {
          float* d = reinterpret_cast<float*>(out[e] + f.offset);
          const float* dd = cur ? mv.eye[e].d : mv.eye[e].dp;
          for (int i = 0; i < 3; ++i) d[i] += dd[i];
        }
        ok = true;
        break;
      }
      case kFxP2W: {
        // dir(cs) = rows . (cs.x, cs.y, 1) (x, y, w components), divided by row 3's. The eye's
        // pixel at cs sees the centre's direction at cs - kcs: column w -= kcs column x, with kcs
        // the eye's NDC shift in cs units (measured on the matrix itself).
        auto dir = [&](float x, float* o) {
          float w = m[3].x * x + m[3].w;
          for (int i = 0; i < 3; ++i) o[i] = ((&m[i].x)[0] * x + (&m[i].x)[3]) / (std::fabs(w) > 1e-9f ? w : 1.0f);
        };
        float a[3], b[3];
        dir(-0.5f, a);
        dir(0.5f, b);
        float slope = ndc_x(mv, b) - ndc_x(mv, a);  // NDC per cs unit
        // Its columns x/y must be the main view's right/up (a camera-aligned matrix).
        float cx[3] = {m[0].x, m[1].x, m[2].x};
        float c = dot3f(cx, mv.right) / (len3(cx) + 1e-12f);
        if (std::fabs(c) < 0.999f || std::fabs(slope) < 1e-3f) break;
        for (int e = 0; e < 2; ++e) {
          F4* d = reinterpret_cast<F4*>(out[e] + f.offset);
          float kcs = mv.eye[e].k / slope;
          for (int i = 0; i < 4; ++i) d[i].w -= kcs * d[i].x;
        }
        static std::atomic<int> once{0};
        if (once.fetch_add(1) < 2)
          ACO_LOG("stereo: effect proj2world: %.4f NDC per composite unit, eye shifts %.5f %.5f", slope,
                  mv.eye[0].k / slope, mv.eye[1].k / slope);
        ok = true;
        break;
      }
      case kFxRayTL: {
        if (ray_r < 0 || UINT(ray_r) + 16 > bytes) break;
        // Copies: l is also the left eye's output, written below.
        float tl[3] = {m->x, m->y, m->z}, pr[3];
        memcpy(pr, l + ray_r, sizeof(pr));
        // Only the main view's camera: PlaneRight along its right vector.
        float c = dot3f(pr, mv.right) / (len3(pr) + 1e-12f);
        if (std::fabs(c) < 0.999f) break;
        float a[3], b[3];
        for (int i = 0; i < 3; ++i) {
          a[i] = tl[i] + 0.25f * pr[i];
          b[i] = tl[i] + 0.75f * pr[i];
        }
        float slope = 2.0f * (ndc_x(mv, b) - ndc_x(mv, a));  // NDC per unit of u
        // And spanning the screen (u 0..1 = NDC -1..1): the cloud view cache's ring cameras can
        // line up with the right vector too.
        float mid[3];
        for (int i = 0; i < 3; ++i) mid[i] = tl[i] + 0.5f * pr[i];
        if (std::fabs(std::fabs(slope) - 2.0f) > 0.2f || std::fabs(ndc_x(mv, mid)) > 0.2f) break;
        for (int e = 0; e < 2; ++e) {
          float* d = reinterpret_cast<float*>(out[e] + f.offset);
          float du = mv.eye[e].k / slope;
          for (int i = 0; i < 3; ++i) d[i] = tl[i] - du * pr[i];
        }
        static std::atomic<int> once{0};
        if (once.fetch_add(1) < 2) ACO_LOG("stereo: effect ray march: %.4f NDC per unit of u", slope);
        ok = true;
        break;
      }
      case kFxRayR: continue;  // with kFxRayTL
    }
    if (ok) {
      moved++;
      static std::atomic<int> logged_ok[8];
      if (logged_ok[f.kind].fetch_add(1) < 1) log_field("moved", fs.hash, slot.slot, f, m, need / 16, mv);
    } else {
      st_fx_nomatch.fetch_add(1, std::memory_order_relaxed);
      log_field("not the main camera", fs.hash, slot.slot, f, m, need / 16, mv);
    }
  }
  return moved;
}

using SetCb1 = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);

void get_cb(ID3D11DeviceContext* ctx, CamStage st, UINT slot, ID3D11Buffer** b, UINT* first, UINT* num) {
  auto* c1 = static_cast<ID3D11DeviceContext1*>(ctx);
  if (st == kCS)
    c1->CSGetConstantBuffers1(slot, 1, b, first, num);
  else
    c1->PSGetConstantBuffers1(slot, 1, b, first, num);
}

// Binds without our hooks (no camera filtering, not recorded).
void set_cb(ID3D11DeviceContext* ctx, CamStage st, UINT slot, ID3D11Buffer* b, UINT first, UINT num) {
  auto set = reinterpret_cast<SetCb1>(
      original(ctx, st == kCS ? vt::Context1_CSSetConstantBuffers1 : vt::Context1_PSSetConstantBuffers1));
  set(static_cast<ID3D11DeviceContext1*>(ctx), slot, 1, &b, &first, &num);
}

bool group_on(FxGroup g) {
  const std::string& s = config().stereo_effects;
  const char* n = kGroupNames[g];
  size_t l = strlen(n);
  for (size_t p = s.find(n); p != std::string::npos; p = s.find(n, p + 1))
    if ((p == 0 || !isalnum(static_cast<unsigned char>(s[p - 1]))) &&
        (p + l == s.size() || !isalnum(static_cast<unsigned char>(s[p + l]))))
      return true;
  return false;
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
  if (!g_cam_on.load(std::memory_order_relaxed) || !m || !m->pData) return;
  if (type != D3D11_MAP_WRITE_DISCARD && type != D3D11_MAP_WRITE_NO_OVERWRITE) return;
  UINT size;
  if (!shadow_size(res, &size)) return;
  CamCtx* cs = ctx_state(ctx);
  if (!cs) return;
  std::lock_guard lock(cs->mutex);
  if (type == D3D11_MAP_WRITE_NO_OVERWRITE) {
    // Written in place: the shadow goes stale.
    auto it = cs->shadow.find(res);
    if (it != cs->shadow.end()) it->second.valid = false;
    if (size != kPassParamsBytes && size != kInstanceBytes) note_fx_write(res, {}, false);
    return;
  }
  auto& s = cs->shadow[res];
  s.data.resize(size);
  s.valid = false;  // until Unmap
  cs->open[res] = MapOpen{m->pData, size};
  m->pData = s.data.data();
}

void cam_unmap(ID3D11DeviceContext* ctx, ID3D11Resource* res) {
  if (!g_nctx.load(std::memory_order_relaxed)) return;
  CamCtx* cs = ctx_state(ctx);
  if (!cs) return;
  std::lock_guard lock(cs->mutex);
  auto it = cs->open.find(res);
  if (it == cs->open.end()) return;
  Shadow& s = cs->shadow[res];
  memcpy(it->second.real, s.data.data(), it->second.size);
  s.valid = true;
  cs->open.erase(it);
  note_fx_write(res, s.data, true);
}

void cam_update(ID3D11DeviceContext* ctx, ID3D11Resource* res, const D3D11_BOX* box, const void* data) {
  if (!g_cam_on.load(std::memory_order_relaxed) || !data) return;
  UINT size;
  if (!shadow_size(res, &size)) return;
  CamCtx* cs = ctx_state(ctx);
  if (!cs) return;
  std::lock_guard lock(cs->mutex);
  if (!box || (box->left == 0 && box->right >= size)) {
    auto& s = cs->shadow[res];
    s.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    s.valid = true;
    note_fx_write(res, s.data, true);
  } else {
    auto it = cs->shadow.find(res);
    if (it != cs->shadow.end()) it->second.valid = false;
    if (size != kPassParamsBytes && size != kInstanceBytes) note_fx_write(res, {}, false);
  }
}

unsigned cam_filter(ID3D11DeviceContext* ctx, CamStage stage, UINT start, UINT n, ID3D11Buffer* const* bufs,
                    const UINT* first, ID3D11Buffer** out_buf, UINT* out_first, UINT* out_num) {
  (void)stage;
  if (!g_cam_on.load(std::memory_order_relaxed) || !bufs || !g_pool || g_pool_failed) return 0;
  if (start > kInstanceSlot || start + n <= kPassSlot) return 0;
  CamCtx* cs = ctx_state(ctx);
  if (!cs) return 0;
  // No D3D call while the lock is held (a game thread holding the device's lock may be waiting
  // for it in Map): the immediate context's uploads happen after the loop.
  std::unique_lock cs_lock(cs->mutex);
  std::shared_ptr<Chunk> imm_up[16];
  int n_up = 0;
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
    if (slot == kInstanceSlot && size != kInstanceBytes) continue;
    auto it = cs->shadow.find(b);
    if (it == cs->shadow.end()) {
      st_nodata.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    UINT off = first ? first[i] * 16 : 0;
    if (off + size > it->second.data.size()) continue;
    const F4* src = reinterpret_cast<const F4*>(it->second.data.data() + off);
    alignas(16) F4 l[kPassParamsBytes / 16], r[kPassParamsBytes / 16];
    UINT nreg = size / 16;
    if (slot == kPassSlot) {
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
      MainView& mv = cs->main;
      mv.valid = true;
      memcpy(mv.vp, src + kViewProj, sizeof(mv.vp));
      memcpy(mv.vpp, src + kViewProjPrev, sizeof(mv.vpp));
      for (int e = 0; e < 2; ++e) eye_params(src, sep, cfg.stereo_convergence, e, &mv.eye[e]);
      fill_main(mv, src);
      cs->last_main = mv;
      note_main(src, mv);
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
      ch->base = imm_alloc(bytes);
      ch->eye[0].resize(bytes);
      ch->eye[1].resize(bytes);
      memcpy(ch->eye[0].data(), l, size);
      memcpy(ch->eye[1].data(), r, size);
      ch->used = UINT(ch->eye[0].size());
      poff = ch->base;
      imm_up[n_up++] = std::move(ch);
    } else {
      poff = put_slot(cs, l, r, size);
    }
    out_buf[i] = g_pool;
    out_first[i] = poff / 16;
    out_num[i] = ((size + 255) / 256) * 16;
    mask |= 1u << i;
  }
  cs_lock.unlock();
  for (int u = 0; u < n_up; ++u) {
    std::shared_ptr<Chunk> ch = imm_up[u];
    upload_chunk(g_imm, *ch, stereo_eye());
    if (g_rec_on.load(std::memory_order_relaxed))
      rec_push([ch](ID3D11DeviceContext* c) { upload_chunk(c, *ch, 1); }, RecKind::Upload);
  }
  return mask;
}

void cam_finish_command_list(ID3D11DeviceContext* ctx, ID3D11CommandList* cl) {
  if (!g_nctx.load(std::memory_order_relaxed)) return;
  CamCtx* cs = ctx_state(ctx);
  if (!cs) return;
  std::lock_guard lock(cs->mutex);
  cs->main.valid = false;
  cs->last_main.valid = false;
  if (cs->chunks.empty()) return;
  auto* h = new ClChunks;
  h->chunks = std::move(cs->chunks);
  cs->chunks.clear();
  if (cl) cl->SetPrivateDataInterface(kCamGuid, h);
  h->Release();
}

bool cam_effect_bytecode(const void* bc, size_t n) {
  Rdef r;
  if (!find_rdef(static_cast<const uint8_t*>(bc), n, &r)) return false;
  for (const FxCbuffer& c : kFxCbuffers) {
    size_t l = strlen(c.cbuffer);
    for (size_t i = 0; i + l <= r.n; ++i)
      if (r.p[i] == c.cbuffer[0] && memcmp(r.p + i, c.cbuffer, l) == 0) return true;
  }
  return false;
}

void cam_register_effect(ID3D11DeviceChild* sh, uint64_t hash, const void* bc, size_t n) {
  FxShader fx;
  fx.hash = hash;
  if (!parse_fx(static_cast<const uint8_t*>(bc), n, &fx)) return;
  char desc[256];
  int o = 0;
  for (const FxSlot& sl : fx.slots)
    for (const FxField& f : sl.fields)
      if (o < int(sizeof(desc)) - 40) o += snprintf(desc + o, sizeof(desc) - o, " %s@cb%u+%u", kind_name(f.kind), sl.slot, f.offset);
  ACO_LOG("stereo: effect shader %016llx:%s", static_cast<unsigned long long>(hash), desc);
  std::unique_lock lock(g_fx_mutex);
  g_fx[sh] = std::move(fx);
}

namespace {
// Immediate context: a slot of the immediate ring holding this eye's version, uploaded now.
UINT imm_slot(const uint8_t* data, UINT bytes) {
  UINT base = imm_alloc(bytes);
  D3D11_BOX bx{base, 0, 0, base + bytes, 1, 1};
  g_imm->UpdateSubresource(g_pool, 0, &bx, data, 0, 0);
  st_uploads.fetch_add(1, std::memory_order_relaxed);
  st_upload_bytes.fetch_add(bytes, std::memory_order_relaxed);
  return base;
}
}  // namespace

void cam_effect_begin(ID3D11DeviceContext* ctx, CamStage st, ID3D11DeviceChild* sh, CamFx* fx) {
  fx->n = 0;
  if (!g_cam_on.load(std::memory_order_relaxed) || !g_pool || g_pool_failed || !sh) return;
  // No D3D call while one of our locks is held (see cam_filter): the shader's description is
  // copied out, the bindings read first, the per-eye copies made under the context's lock, and
  // the uploads and binds done after.
  FxShader fs;
  {
    std::shared_lock fx_lock(g_fx_mutex);
    auto fit = g_fx.find(sh);
    if (fit == g_fx.end()) return;
    fs = fit->second;
  }
  st_fx_draws.fetch_add(1, std::memory_order_relaxed);
  CamCtx* cs = ctx_state(ctx);
  if (!cs) return;
  struct Bound {
    const FxSlot* sl;
    ID3D11Buffer* b;
    UINT first, num, width;
  } bound[CamFx::kMax];
  int nb = 0;
  for (const FxSlot& sl : fs.slots) {
    if (nb >= CamFx::kMax) break;
    if (!group_on(sl.group)) continue;
    ID3D11Buffer* b = nullptr;
    UINT first = 0, num = 0;
    get_cb(ctx, st, sl.slot, &b, &first, &num);
    if (!b) continue;
    D3D11_BUFFER_DESC d;
    b->GetDesc(&d);
    if (b == g_pool || first * 16 >= d.ByteWidth || !num) {
      b->Release();
      continue;
    }
    bound[nb++] = {&sl, b, first, num, d.ByteWidth};
  }
  if (!nb) return;
  thread_local std::vector<uint8_t> lb[CamFx::kMax], rb[CamFx::kMax];
  UINT poff[CamFx::kMax], pbytes[CamFx::kMax];
  bool use[CamFx::kMax] = {};
  bool imm = ctx == g_imm;
  {
    std::lock_guard lock(cs->mutex);
    MainView mv;
    bool have = cs->last_main.valid;
    if (have) mv = cs->last_main;
    if (!have) have = last_main(&mv);  // g_main_mutex: no D3D calls under it either
    for (int i = 0; have && i < nb; ++i) {
      const Bound& bd = bound[i];
      UINT off = bd.first * 16;
      UINT bytes = std::min<UINT>(bd.num * 16, bd.width - off);
      const Shadow* sh = nullptr;
      auto it = cs->shadow.find(bd.b);
      if (it != cs->shadow.end() && it->second.valid) sh = &it->second;
      // Written on another context: its latest write (copied, under the store's lock).
      thread_local Shadow other;
      bool cross = false;
      if (!sh) {
        std::lock_guard l2(g_fx_last_mutex);
        auto g = g_fx_last.find(bd.b);
        if (g != g_fx_last.end() && g->second.valid) {
          other.data = g->second.data;
          other.valid = true;
          sh = &other;
          cross = true;
        }
      }
      if (cross) st_fx_cross.fetch_add(1, std::memory_order_relaxed);
      if (!sh || sh->data.size() < off + bytes) {
        learn_fx_size(bd.width);
        st_fx_nodata.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 30)
          ACO_LOG("stereo: effect no data: shader %016llx cb%u (%u bytes, constants %u+%u) on %s: %s",
                  static_cast<unsigned long long>(fs.hash), bd.sl->slot, bd.width, bd.first, bd.num,
                  imm ? "immediate" : "deferred",
                  !sh ? (it == cs->shadow.end() ? "not written" : "stale") : "short");
        continue;
      }
      pbytes[i] = (bytes + 255) & ~255u;
      lb[i].assign(pbytes[i], 0);
      rb[i].assign(pbytes[i], 0);
      memcpy(lb[i].data(), sh->data.data() + off, bytes);
      memcpy(rb[i].data(), sh->data.data() + off, bytes);
      if (!move_fields(fs, *bd.sl, mv, lb[i].data(), rb[i].data(), bytes)) continue;
      if (!imm) poff[i] = put_slot(cs, lb[i].data(), rb[i].data(), pbytes[i]);
      use[i] = true;
    }
  }
  for (int i = 0; i < nb; ++i) {
    const Bound& bd = bound[i];
    if (!use[i]) {
      bd.b->Release();
      continue;
    }
    if (imm) poff[i] = imm_slot(stereo_eye() ? rb[i].data() : lb[i].data(), pbytes[i]);
    set_cb(ctx, st, bd.sl->slot, g_pool, poff[i] / 16, pbytes[i] / 16);
    fx->s[fx->n++] = {st, bd.sl->slot, bd.b, bd.first, bd.num};  // keeps the reference until cam_effect_end
    st_fx_moved.fetch_add(1, std::memory_order_relaxed);
  }

}

void cam_effect_end(ID3D11DeviceContext* ctx, CamFx* fx) {
  for (int i = 0; i < fx->n; ++i) {
    auto& e = fx->s[i];
    set_cb(ctx, CamStage(e.st), e.slot, e.buf, e.first, e.num);
    e.buf->Release();
  }
  fx->n = 0;
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
  if (g_imm)
    if (CamCtx* cs = ctx_state(g_imm)) {
      std::lock_guard lock(cs->mutex);
      cs->last_main.valid = false;
    }
  if (++frames % 300 == 0 || frames == 60) {
    ACO_LOG("stereo: effect cameras/frame: draws %.1f, buffers moved %.1f (written on another context %.1f), no data %.1f, fields not the main camera %.1f",
            st_fx_draws.exchange(0) / 300.0, st_fx_moved.exchange(0) / 300.0, st_fx_cross.exchange(0) / 300.0,
            st_fx_nodata.exchange(0) / 300.0, st_fx_nomatch.exchange(0) / 300.0);
    ACO_LOG("stereo: camera binds/frame: PassParams main %.0f screen %.0f other %.0f, InstanceParams main %.0f other %.0f, no data %.0f; uploads %.0f (%.2f MB)",
            st_pass_main.exchange(0) / 300.0, st_pass_screen.exchange(0) / 300.0, st_pass_other.exchange(0) / 300.0, st_inst_main.exchange(0) / 300.0,
            st_inst_other.exchange(0) / 300.0, st_nodata.exchange(0) / 300.0, st_uploads.exchange(0) / 300.0,
            st_upload_bytes.exchange(0) / 300.0 / 1048576.0);
  }
}

} // namespace aco
