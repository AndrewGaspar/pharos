#include "stereo/record.h"

#include <windows.h>

#include <cstring>
#include <algorithm>
#include <map>
#include <string>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "core/hook.h"
#include "core/log.h"
#include "core/vtidx.h"

namespace aco {

std::atomic<bool> g_rec_on{false};
ID3D11DeviceContext* g_rec_imm = nullptr;
thread_local int g_rec_replaying = 0;

namespace {

using namespace vt;

uintptr_t g_lo = 0, g_hi = 0;

void init_range() {
  HMODULE m = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&init_range), &m);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(m);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<char*>(m) + dos->e_lfanew);
  g_lo = reinterpret_cast<uintptr_t>(m);
  g_hi = g_lo + nt->OptionalHeader.SizeOfImage;
}

// COM reference held by a recorded call.
template <typename T>
struct Ref {
  T* p = nullptr;
  Ref() = default;
  explicit Ref(T* x) : p(x) {
    if (p) p->AddRef();
  }
  Ref(const Ref& o) : p(o.p) {
    if (p) p->AddRef();
  }
  Ref& operator=(const Ref& o) {
    if (o.p) o.p->AddRef();
    if (p) p->Release();
    p = o.p;
    return *this;
  }
  ~Ref() {
    if (p) p->Release();
  }
};

// Array of COM references (null entries allowed).
template <typename T>
struct Refs {
  std::vector<T*> v;
  Refs() = default;
  Refs(T* const* a, UINT n) {
    if (!a) return;
    v.assign(a, a + n);
    for (T* p : v)
      if (p) p->AddRef();
  }
  Refs(const Refs& o) : v(o.v) {
    for (T* p : v)
      if (p) p->AddRef();
  }
  Refs& operator=(const Refs&) = delete;
  ~Refs() {
    for (T* p : v)
      if (p) p->Release();
  }
  T* const* data() const { return v.empty() ? nullptr : v.data(); }
};

template <typename T>
std::vector<T> copy_array(const T* a, UINT n) {
  return a ? std::vector<T>(a, a + n) : std::vector<T>();
}
template <typename T>
const T* ptr_or_null(const std::vector<T>& v) {
  return v.empty() ? nullptr : v.data();
}

}  // namespace

struct RecFrame {
  std::vector<RecFn> cmds;
  std::vector<uint8_t> kinds;  // RecKind per command
  size_t bytes = 0;
  const char* bad = nullptr;
  // Diagnostics: uploads by (kind, size).
  std::map<std::pair<int, size_t>, std::pair<size_t, size_t>> uploads;  // (kind, size) -> (count, bytes)
  double rec_ms = 0;
  std::map<std::pair<ID3D11Resource*, UINT>, int> big_discards;
  int big_rediscards = 0;  // large buffers discarded more than once (their earlier contents are not replayed)
};

namespace {

std::mutex g_mutex;
RecFrame* g_cur = nullptr;

// ---- format / layout helpers ------------------------------------------------------------------

// Bytes per block and block size (1 for uncompressed formats). 0 = unknown.
UINT format_block(DXGI_FORMAT f, UINT* bw) {
  *bw = 1;
  switch (f) {
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
      *bw = 4;
      return 8;
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
      *bw = 4;
      return 16;
    default: break;
  }
  if (f >= DXGI_FORMAT_R32G32B32A32_TYPELESS && f <= DXGI_FORMAT_R32G32B32A32_SINT) return 16;
  if (f >= DXGI_FORMAT_R32G32B32_TYPELESS && f <= DXGI_FORMAT_R32G32B32_SINT) return 12;
  if (f >= DXGI_FORMAT_R16G16B16A16_TYPELESS && f <= DXGI_FORMAT_X32_TYPELESS_G8X24_UINT) return 8;
  if (f >= DXGI_FORMAT_R10G10B10A2_TYPELESS && f <= DXGI_FORMAT_X24_TYPELESS_G8_UINT) return 4;
  if (f >= DXGI_FORMAT_R8G8_TYPELESS && f <= DXGI_FORMAT_R16_SINT) return 2;
  if (f >= DXGI_FORMAT_R8_TYPELESS && f <= DXGI_FORMAT_A8_UNORM) return 1;
  if (f >= DXGI_FORMAT_B8G8R8A8_UNORM && f <= DXGI_FORMAT_B8G8R8X8_UNORM_SRGB) return 4;
  if (f == DXGI_FORMAT_B5G6R5_UNORM || f == DXGI_FORMAT_B5G5R5A1_UNORM || f == DXGI_FORMAT_B4G4R4A4_UNORM) return 2;
  if (f == DXGI_FORMAT_R9G9B9E5_SHAREDEXP || f == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM) return 4;
  return 0;
}

// Dimensions of subresource `sub` (width, height, depth in texels) and its format. False for buffers.
bool sub_dims(ID3D11Resource* res, UINT sub, UINT* w, UINT* h, UINT* d, DXGI_FORMAT* fmt) {
  D3D11_RESOURCE_DIMENSION dim;
  res->GetType(&dim);
  switch (dim) {
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
      D3D11_TEXTURE1D_DESC t;
      static_cast<ID3D11Texture1D*>(res)->GetDesc(&t);
      UINT mip = sub % t.MipLevels;
      *w = (t.Width >> mip) ? (t.Width >> mip) : 1;
      *h = *d = 1;
      *fmt = t.Format;
      return true;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
      D3D11_TEXTURE2D_DESC t;
      static_cast<ID3D11Texture2D*>(res)->GetDesc(&t);
      UINT mip = sub % t.MipLevels;
      *w = (t.Width >> mip) ? (t.Width >> mip) : 1;
      *h = (t.Height >> mip) ? (t.Height >> mip) : 1;
      *d = 1;
      *fmt = t.Format;
      return true;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
      D3D11_TEXTURE3D_DESC t;
      static_cast<ID3D11Texture3D*>(res)->GetDesc(&t);
      UINT mip = sub % t.MipLevels;
      *w = (t.Width >> mip) ? (t.Width >> mip) : 1;
      *h = (t.Height >> mip) ? (t.Height >> mip) : 1;
      *d = (t.Depth >> mip) ? (t.Depth >> mip) : 1;
      *fmt = t.Format;
      return true;
    }
    default: return false;
  }
}

UINT buffer_width(ID3D11Resource* res) {
  D3D11_BUFFER_DESC b;
  static_cast<ID3D11Buffer*>(res)->GetDesc(&b);
  return b.ByteWidth;
}

bool is_staging(ID3D11Resource* res) {
  D3D11_RESOURCE_DIMENSION dim;
  res->GetType(&dim);
  D3D11_USAGE u = D3D11_USAGE_DEFAULT;
  switch (dim) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: {
      D3D11_BUFFER_DESC d;
      static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
      u = d.Usage;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
      D3D11_TEXTURE1D_DESC d;
      static_cast<ID3D11Texture1D*>(res)->GetDesc(&d);
      u = d.Usage;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
      D3D11_TEXTURE2D_DESC d;
      static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
      u = d.Usage;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
      D3D11_TEXTURE3D_DESC d;
      static_cast<ID3D11Texture3D*>(res)->GetDesc(&d);
      u = d.Usage;
      break;
    }
    default: break;
  }
  return u == D3D11_USAGE_STAGING;
}

// True if the GPU never writes `res` (no render-target, depth, UAV or stream-out binding) and it is
// not a constant buffer: its contents only change through uploads (texture streaming, mesh data).
// Re-running such an upload in the replay is pointless, and wrong when the source has been reused
// since (streaming copies from an upload ring that the game refilled later in the frame: the
// replay wrote other data into the textures).
bool upload_only(ID3D11Resource* res) {
  D3D11_RESOURCE_DIMENSION dim;
  res->GetType(&dim);
  UINT bind = 0;
  switch (dim) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: {
      D3D11_BUFFER_DESC d;
      static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
      if (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) return false;
      bind = d.BindFlags;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
      D3D11_TEXTURE1D_DESC d;
      static_cast<ID3D11Texture1D*>(res)->GetDesc(&d);
      bind = d.BindFlags;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
      D3D11_TEXTURE2D_DESC d;
      static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
      bind = d.BindFlags;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
      D3D11_TEXTURE3D_DESC d;
      static_cast<ID3D11Texture3D*>(res)->GetDesc(&d);
      bind = d.BindFlags;
      break;
    }
    default: return false;
  }
  return !(bind & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS |
                   D3D11_BIND_STREAM_OUTPUT));
}

// ---- recorder's own hooks ---------------------------------------------------------------------

#define ORIGF(name) orig<decltype(hk_##name)>(self, Context_##name)
#define ORIGF1(name) orig<decltype(hk_##name)>(self, Context1_##name)

#define REC_STAGE(S, Stage)                                                                                       \
  void STDMETHODCALLTYPE hk_##S##SetConstantBuffers(ID3D11DeviceContext* self, UINT s, UINT n,                       \
                                                    ID3D11Buffer* const* b) {                                       \
    if (ACO_REC_WANT(self)) {                                                                                       \
      Refs<ID3D11Buffer> r(b, n);                                                                                   \
      rec_push([s, n, r](ID3D11DeviceContext* c) { c->S##SetConstantBuffers(s, n, r.data()); });                   \
    }                                                                                                               \
    ORIGF(S##SetConstantBuffers)(self, s, n, b);                                                                    \
  }                                                                                                                 \
  void STDMETHODCALLTYPE hk_##S##SetShaderResources(ID3D11DeviceContext* self, UINT s, UINT n,                       \
                                                    ID3D11ShaderResourceView* const* v) {                           \
    if (ACO_REC_WANT(self)) {                                                                                       \
      Refs<ID3D11ShaderResourceView> r(v, n);                                                                       \
      rec_push([s, n, r](ID3D11DeviceContext* c) { c->S##SetShaderResources(s, n, r.data()); });                   \
    }                                                                                                               \
    ORIGF(S##SetShaderResources)(self, s, n, v);                                                                    \
  }                                                                                                                 \
  void STDMETHODCALLTYPE hk_##S##SetSamplers(ID3D11DeviceContext* self, UINT s, UINT n,                              \
                                             ID3D11SamplerState* const* v) {                                        \
    if (ACO_REC_WANT(self)) {                                                                                       \
      Refs<ID3D11SamplerState> r(v, n);                                                                             \
      rec_push([s, n, r](ID3D11DeviceContext* c) { c->S##SetSamplers(s, n, r.data()); });                          \
    }                                                                                                               \
    ORIGF(S##SetSamplers)(self, s, n, v);                                                                           \
  }                                                                                                                 \
  void STDMETHODCALLTYPE hk_##S##SetConstantBuffers1(ID3D11DeviceContext1* self, UINT s, UINT n,                     \
                                                     ID3D11Buffer* const* b, const UINT* f, const UINT* k) {        \
    if (ACO_REC_WANT(self)) {                                                                                       \
      Refs<ID3D11Buffer> r(b, n);                                                                                   \
      auto fv = copy_array(f, n), kv = copy_array(k, n);                                                            \
      rec_push([s, n, r, fv, kv](ID3D11DeviceContext* c) {                                                          \
        static_cast<ID3D11DeviceContext1*>(c)->S##SetConstantBuffers1(s, n, r.data(), ptr_or_null(fv),             \
                                                                      ptr_or_null(kv));                             \
      });                                                                                                           \
    }                                                                                                               \
    ORIGF1(S##SetConstantBuffers1)(self, s, n, b, f, k);                                                            \
  }

REC_STAGE(VS, Vertex)
REC_STAGE(PS, Pixel)
REC_STAGE(GS, Geometry)
REC_STAGE(HS, Hull)
REC_STAGE(DS, Domain)
REC_STAGE(CS, Compute)

#define REC_SHADER(S, T)                                                                                            \
  void STDMETHODCALLTYPE hk_##S##SetShader(ID3D11DeviceContext* self, T* sh, ID3D11ClassInstance* const* ci,         \
                                           UINT n) {                                                                \
    if (ACO_REC_WANT(self)) {                                                                                       \
      if (n) rec_unsupported(#S "SetShader with class instances");                                                  \
      Ref<T> r(sh);                                                                                                 \
      rec_push([r](ID3D11DeviceContext* c) { c->S##SetShader(r.p, nullptr, 0); });                                 \
    }                                                                                                               \
    ORIGF(S##SetShader)(self, sh, ci, n);                                                                           \
  }

REC_SHADER(VS, ID3D11VertexShader)
REC_SHADER(GS, ID3D11GeometryShader)
REC_SHADER(HS, ID3D11HullShader)
REC_SHADER(DS, ID3D11DomainShader)

void STDMETHODCALLTYPE hk_IASetInputLayout(ID3D11DeviceContext* self, ID3D11InputLayout* l) {
  if (ACO_REC_WANT(self)) {
    Ref<ID3D11InputLayout> r(l);
    rec_push([r](ID3D11DeviceContext* c) { c->IASetInputLayout(r.p); });
  }
  ORIGF(IASetInputLayout)(self, l);
}
void STDMETHODCALLTYPE hk_IASetVertexBuffers(ID3D11DeviceContext* self, UINT s, UINT n, ID3D11Buffer* const* b,
                                             const UINT* strides, const UINT* offsets) {
  if (ACO_REC_WANT(self)) {
    Refs<ID3D11Buffer> r(b, n);
    auto sv = copy_array(strides, n), ov = copy_array(offsets, n);
    rec_push([s, n, r, sv, ov](ID3D11DeviceContext* c) {
      c->IASetVertexBuffers(s, n, r.data(), ptr_or_null(sv), ptr_or_null(ov));
    });
  }
  ORIGF(IASetVertexBuffers)(self, s, n, b, strides, offsets);
}
void STDMETHODCALLTYPE hk_IASetIndexBuffer(ID3D11DeviceContext* self, ID3D11Buffer* b, DXGI_FORMAT f, UINT off) {
  if (ACO_REC_WANT(self)) {
    Ref<ID3D11Buffer> r(b);
    rec_push([r, f, off](ID3D11DeviceContext* c) { c->IASetIndexBuffer(r.p, f, off); });
  }
  ORIGF(IASetIndexBuffer)(self, b, f, off);
}
void STDMETHODCALLTYPE hk_IASetPrimitiveTopology(ID3D11DeviceContext* self, D3D11_PRIMITIVE_TOPOLOGY t) {
  if (ACO_REC_WANT(self)) rec_push([t](ID3D11DeviceContext* c) { c->IASetPrimitiveTopology(t); });
  ORIGF(IASetPrimitiveTopology)(self, t);
}
void STDMETHODCALLTYPE hk_SetPredication(ID3D11DeviceContext* self, ID3D11Predicate* p, BOOL v) {
  if (ACO_REC_WANT(self)) {
    Ref<ID3D11Predicate> r(p);
    rec_push([r, v](ID3D11DeviceContext* c) { c->SetPredication(r.p, v); });
  }
  ORIGF(SetPredication)(self, p, v);
}
void STDMETHODCALLTYPE hk_OMSetRenderTargets(ID3D11DeviceContext* self, UINT n, ID3D11RenderTargetView* const* rtv,
                                             ID3D11DepthStencilView* dsv) {
  if (ACO_REC_WANT(self)) {
    Refs<ID3D11RenderTargetView> r(rtv, n);
    Ref<ID3D11DepthStencilView> d(dsv);
    rec_push([n, r, d](ID3D11DeviceContext* c) { c->OMSetRenderTargets(n, r.data(), d.p); });
  }
  ORIGF(OMSetRenderTargets)(self, n, rtv, dsv);
}
void STDMETHODCALLTYPE hk_OMSetRenderTargetsAndUnorderedAccessViews(ID3D11DeviceContext* self, UINT nr,
                                                                    ID3D11RenderTargetView* const* rtv,
                                                                    ID3D11DepthStencilView* dsv, UINT us, UINT nu,
                                                                    ID3D11UnorderedAccessView* const* uav,
                                                                    const UINT* counts) {
  if (ACO_REC_WANT(self)) {
    bool keep_rt = nr == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL;
    bool keep_uav = nu == D3D11_KEEP_UNORDERED_ACCESS_VIEWS;
    Refs<ID3D11RenderTargetView> r(keep_rt ? nullptr : rtv, keep_rt ? 0 : nr);
    Ref<ID3D11DepthStencilView> d(keep_rt ? nullptr : dsv);
    Refs<ID3D11UnorderedAccessView> u(keep_uav ? nullptr : uav, keep_uav ? 0 : nu);
    auto cv = copy_array(keep_uav ? nullptr : counts, keep_uav ? 0 : nu);
    rec_push([nr, r, d, us, nu, u, cv](ID3D11DeviceContext* c) {
      c->OMSetRenderTargetsAndUnorderedAccessViews(nr, r.data(), d.p, us, nu, u.data(), ptr_or_null(cv));
    });
  }
  ORIGF(OMSetRenderTargetsAndUnorderedAccessViews)(self, nr, rtv, dsv, us, nu, uav, counts);
}
void STDMETHODCALLTYPE hk_OMSetBlendState(ID3D11DeviceContext* self, ID3D11BlendState* s, const FLOAT f[4], UINT mask) {
  if (ACO_REC_WANT(self)) {
    Ref<ID3D11BlendState> r(s);
    bool hf = f != nullptr;
    float fv[4] = {1, 1, 1, 1};
    if (hf) memcpy(fv, f, sizeof(fv));
    rec_push([r, hf, fv, mask](ID3D11DeviceContext* c) { c->OMSetBlendState(r.p, hf ? fv : nullptr, mask); });
  }
  ORIGF(OMSetBlendState)(self, s, f, mask);
}
void STDMETHODCALLTYPE hk_OMSetDepthStencilState(ID3D11DeviceContext* self, ID3D11DepthStencilState* s, UINT ref) {
  if (ACO_REC_WANT(self)) {
    Ref<ID3D11DepthStencilState> r(s);
    rec_push([r, ref](ID3D11DeviceContext* c) { c->OMSetDepthStencilState(r.p, ref); });
  }
  ORIGF(OMSetDepthStencilState)(self, s, ref);
}
void STDMETHODCALLTYPE hk_SOSetTargets(ID3D11DeviceContext* self, UINT n, ID3D11Buffer* const* b, const UINT* off) {
  if (ACO_REC_WANT(self)) {
    Refs<ID3D11Buffer> r(b, n);
    auto ov = copy_array(off, n);
    rec_push([n, r, ov](ID3D11DeviceContext* c) { c->SOSetTargets(n, r.data(), ptr_or_null(ov)); });
  }
  ORIGF(SOSetTargets)(self, n, b, off);
}
void STDMETHODCALLTYPE hk_RSSetState(ID3D11DeviceContext* self, ID3D11RasterizerState* s) {
  if (ACO_REC_WANT(self)) {
    Ref<ID3D11RasterizerState> r(s);
    rec_push([r](ID3D11DeviceContext* c) { c->RSSetState(r.p); });
  }
  ORIGF(RSSetState)(self, s);
}
void STDMETHODCALLTYPE hk_RSSetViewports(ID3D11DeviceContext* self, UINT n, const D3D11_VIEWPORT* v) {
  if (ACO_REC_WANT(self)) {
    auto vv = copy_array(v, n);
    rec_push([n, vv](ID3D11DeviceContext* c) { c->RSSetViewports(n, ptr_or_null(vv)); });
  }
  ORIGF(RSSetViewports)(self, n, v);
}
void STDMETHODCALLTYPE hk_RSSetScissorRects(ID3D11DeviceContext* self, UINT n, const D3D11_RECT* r) {
  if (ACO_REC_WANT(self)) {
    auto rv = copy_array(r, n);
    rec_push([n, rv](ID3D11DeviceContext* c) { c->RSSetScissorRects(n, ptr_or_null(rv)); });
  }
  ORIGF(RSSetScissorRects)(self, n, r);
}
void STDMETHODCALLTYPE hk_CopyStructureCount(ID3D11DeviceContext* self, ID3D11Buffer* dst, UINT off,
                                             ID3D11UnorderedAccessView* src) {
  if (ACO_REC_WANT(self)) {
    Ref<ID3D11Buffer> d(dst);
    Ref<ID3D11UnorderedAccessView> s(src);
    if (!is_staging(dst)) rec_push([d, off, s](ID3D11DeviceContext* c) { c->CopyStructureCount(d.p, off, s.p); });
  }
  ORIGF(CopyStructureCount)(self, dst, off, src);
}
void STDMETHODCALLTYPE hk_GenerateMips(ID3D11DeviceContext* self, ID3D11ShaderResourceView* v) {
  if (ACO_REC_WANT(self)) {
    Ref<ID3D11ShaderResourceView> r(v);
    rec_push([r](ID3D11DeviceContext* c) { c->GenerateMips(r.p); });
  }
  ORIGF(GenerateMips)(self, v);
}
void STDMETHODCALLTYPE hk_SetResourceMinLOD(ID3D11DeviceContext* self, ID3D11Resource* res, FLOAT lod) {
  if (ACO_REC_WANT(self)) {
    Ref<ID3D11Resource> r(res);
    rec_push([r, lod](ID3D11DeviceContext* c) { c->SetResourceMinLOD(r.p, lod); });
  }
  ORIGF(SetResourceMinLOD)(self, res, lod);
}
void STDMETHODCALLTYPE hk_ResolveSubresource(ID3D11DeviceContext* self, ID3D11Resource* dst, UINT ds, ID3D11Resource* src,
                                             UINT ss, DXGI_FORMAT f) {
  if (ACO_REC_WANT(self)) {
    Ref<ID3D11Resource> d(dst), s(src);
    rec_push([d, ds, s, ss, f](ID3D11DeviceContext* c) { c->ResolveSubresource(d.p, ds, s.p, ss, f); });
  }
  ORIGF(ResolveSubresource)(self, dst, ds, src, ss, f);
}
void STDMETHODCALLTYPE hk_CSSetUnorderedAccessViews(ID3D11DeviceContext* self, UINT s, UINT n,
                                                    ID3D11UnorderedAccessView* const* v, const UINT* counts) {
  if (ACO_REC_WANT(self)) {
    Refs<ID3D11UnorderedAccessView> r(v, n);
    auto cv = copy_array(counts, n);
    rec_push([s, n, r, cv](ID3D11DeviceContext* c) { c->CSSetUnorderedAccessViews(s, n, r.data(), ptr_or_null(cv)); });
  }
  ORIGF(CSSetUnorderedAccessViews)(self, s, n, v, counts);
}

// ID3D11DeviceContext1
void STDMETHODCALLTYPE hk_CopySubresourceRegion1(ID3D11DeviceContext1* self, ID3D11Resource* dst, UINT dsub, UINT x,
                                                 UINT y, UINT z, ID3D11Resource* src, UINT ssub, const D3D11_BOX* box,
                                                 UINT flags) {
  if (ACO_REC_WANT(self)) rec_copy_subresource(dst, dsub, x, y, z, src, ssub, box);  // copy flags dropped in the replay
  ORIGF1(CopySubresourceRegion1)(self, dst, dsub, x, y, z, src, ssub, box, flags);
}
void STDMETHODCALLTYPE hk_UpdateSubresource1(ID3D11DeviceContext1* self, ID3D11Resource* dst, UINT sub,
                                             const D3D11_BOX* box, const void* data, UINT rp, UINT dp, UINT flags) {
  if (ACO_REC_WANT(self)) rec_update(dst, sub, box, data, rp, dp, flags, true);
  ORIGF1(UpdateSubresource1)(self, dst, sub, box, data, rp, dp, flags);
}
void STDMETHODCALLTYPE hk_DiscardResource(ID3D11DeviceContext1* self, ID3D11Resource* res) {
  if (ACO_REC_WANT(self)) {
    Ref<ID3D11Resource> r(res);
    rec_push([r](ID3D11DeviceContext* c) { static_cast<ID3D11DeviceContext1*>(c)->DiscardResource(r.p); });
  }
  ORIGF1(DiscardResource)(self, res);
}
void STDMETHODCALLTYPE hk_DiscardView(ID3D11DeviceContext1* self, ID3D11View* v) {
  if (ACO_REC_WANT(self)) {
    Ref<ID3D11View> r(v);
    rec_push([r](ID3D11DeviceContext* c) { static_cast<ID3D11DeviceContext1*>(c)->DiscardView(r.p); });
  }
  ORIGF1(DiscardView)(self, v);
}
void STDMETHODCALLTYPE hk_DiscardView1(ID3D11DeviceContext1* self, ID3D11View* v, const D3D11_RECT* r, UINT n) {
  if (ACO_REC_WANT(self)) {
    Ref<ID3D11View> rv(v);
    auto rects = copy_array(r, n);
    rec_push([rv, rects, n](ID3D11DeviceContext* c) {
      static_cast<ID3D11DeviceContext1*>(c)->DiscardView1(rv.p, ptr_or_null(rects), n);
    });
  }
  ORIGF1(DiscardView1)(self, v, r, n);
}
void STDMETHODCALLTYPE hk_ClearView(ID3D11DeviceContext1* self, ID3D11View* v, const FLOAT col[4], const D3D11_RECT* r,
                                    UINT n) {
  if (ACO_REC_WANT(self)) {
    Ref<ID3D11View> rv(v);
    float cv[4];
    memcpy(cv, col, sizeof(cv));
    auto rects = copy_array(r, n);
    rec_push([rv, cv, rects, n](ID3D11DeviceContext* c) {
      static_cast<ID3D11DeviceContext1*>(c)->ClearView(rv.p, cv, ptr_or_null(rects), n);
    });
  }
  ORIGF1(ClearView)(self, v, col, r, n);
}
void STDMETHODCALLTYPE hk_SwapDeviceContextState(ID3D11DeviceContext1* self, ID3DDeviceContextState* s,
                                                 ID3DDeviceContextState** prev) {
  if (ACO_REC_WANT(self)) rec_unsupported("SwapDeviceContextState");
  ORIGF1(SwapDeviceContextState)(self, s, prev);
}

}  // namespace

// ---- public -------------------------------------------------------------------------------------

bool rec_from_us(void* ret) {
  auto a = reinterpret_cast<uintptr_t>(ret);
  return a >= g_lo && a < g_hi;
}

void rec_push(RecFn f, RecKind k) {
  std::lock_guard lock(g_mutex);
  if (!g_cur) return;
  g_cur->cmds.push_back(std::move(f));
  g_cur->kinds.push_back(uint8_t(k));
}

void rec_unsupported(const char* what) {
  std::lock_guard lock(g_mutex);
  if (g_cur && !g_cur->bad) g_cur->bad = what;
}

namespace {

// Uploads through Map. Reading back what the game wrote into mapped memory means reading
// write-combined (uncached) memory, ~20 MB/s: even 0.3 MB a frame cost ~20 ms. So while recording,
// the game writes into a cached shadow instead (Map returns the shadow's pointer) and Unmap copies
// it into the real mapping: one fast write. The shadow is kept per (resource, subresource) along
// with a mirror of what the real buffer holds, so a WRITE_NO_OVERWRITE map copies (and records)
// only the bytes that changed.
struct MapShadow {
  Ref<ID3D11Resource> res;
  std::vector<uint8_t> s;  // what the game writes into
  std::vector<uint8_t> m;  // mirror of the real mapping's contents
  uint64_t epoch = 0;
  uint64_t last_frame = 0;
};
std::map<std::pair<ID3D11Resource*, UINT>, MapShadow> g_shadows;
uint64_t g_epoch = 0;   // bumped when recording starts after a gap (mirrors may be stale)
uint64_t g_frame_no = 0;
uint64_t g_stopped_at = ~0ull;

struct OpenMap {
  D3D11_MAP type;
  UINT flags;
  void* real;
  UINT rp, dp;
  size_t size;
};
std::map<std::pair<ID3D11Resource*, UINT>, OpenMap> g_open;

size_t map_size(ID3D11Resource* res, UINT sub, const D3D11_MAPPED_SUBRESOURCE& m) {
  UINT w, h, d;
  DXGI_FORMAT fmt;
  if (!sub_dims(res, sub, &w, &h, &d, &fmt)) return buffer_width(res);
  UINT bw;
  if (!format_block(fmt, &bw)) return 0;
  UINT rows = (h + bw - 1) / bw;
  return d > 1 ? size_t(m.DepthPitch) * d : size_t(m.RowPitch) * rows;
}

}  // namespace

void rec_map(ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* m) {
  if (type != D3D11_MAP_WRITE_DISCARD && type != D3D11_MAP_WRITE_NO_OVERWRITE) return;  // readbacks, staging writes
  if (!m || !m->pData) return;
  size_t size = map_size(res, sub, *m);
  if (!size) {
    rec_unsupported("Map of a texture with an unknown format");
    return;
  }
  std::lock_guard lock(g_mutex);
  if (!g_cur) return;
  MapShadow& sh = g_shadows[{res, sub}];
  bool fresh = sh.s.size() != size || sh.epoch != g_epoch || sh.res.p != res;
  if (fresh) {
    sh.res = Ref<ID3D11Resource>(res);
    sh.s.assign(size, 0);
    sh.m.assign(size, 0);
    sh.epoch = g_epoch;
    // The game may rely on what it wrote earlier (NO_OVERWRITE appends): start from the real
    // contents (one slow read, once per buffer).
    if (type == D3D11_MAP_WRITE_NO_OVERWRITE) {
      memcpy(sh.s.data(), m->pData, size);
      sh.m = sh.s;
    }
  }
  sh.last_frame = g_frame_no;
  g_open[{res, sub}] = OpenMap{type, flags, m->pData, m->RowPitch, m->DepthPitch, size};
  m->pData = sh.s.data();
}

void rec_unmap(ID3D11Resource* res, UINT sub) {
  std::lock_guard lock(g_mutex);
  auto it = g_open.find({res, sub});
  if (it == g_open.end()) return;
  OpenMap o = it->second;
  g_open.erase(it);
  auto sit = g_shadows.find({res, sub});
  if (sit == g_shadows.end()) return;
  MapShadow& sh = sit->second;
  size_t lo = 0, hi = o.size;
  if (o.type == D3D11_MAP_WRITE_NO_OVERWRITE) {
    // Changed range against the mirror.
    const uint8_t* a = sh.s.data();
    const uint8_t* b = sh.m.data();
    while (lo < hi && a[lo] == b[lo]) ++lo;
    while (hi > lo && a[hi - 1] == b[hi - 1]) --hi;
  }
  if (hi > lo) {
    memcpy(static_cast<uint8_t*>(o.real) + lo, sh.s.data() + lo, hi - lo);
    memcpy(sh.m.data() + lo, sh.s.data() + lo, hi - lo);
  }
  if (!g_cur) return;
  auto blob = std::make_shared<std::vector<uint8_t>>(sh.s.begin() + lo, sh.s.begin() + hi);
  g_cur->bytes += hi - lo;
  {
    auto& u = g_cur->uploads[{o.type == D3D11_MAP_WRITE_DISCARD ? 0 : 1, o.size}];
    u.first++;
    u.second += hi - lo;
  }
  Ref<ID3D11Resource> r(res);
  D3D11_MAP type = o.type;
  UINT flags = o.flags & ~D3D11_MAP_FLAG_DO_NOT_WAIT;
  size_t off = lo;
  g_cur->kinds.push_back(uint8_t(RecKind::Upload));
  g_cur->cmds.push_back([r, sub, type, flags, blob, off](ID3D11DeviceContext* c) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(c->Map(r.p, sub, type, flags, &m)) || !m.pData) return;
    if (!blob->empty()) memcpy(static_cast<uint8_t*>(m.pData) + off, blob->data(), blob->size());
    c->Unmap(r.p, sub);
  });
}

namespace {

// ---- snapshots of CPU-written copy sources ----------------------------------------------------
// A copy from a dynamic or staging resource reads data the CPU wrote; by the time of the replay the
// game may have refilled it (texture/mesh streaming rings, staging pools). The source region is
// copied on the GPU into one of our buffers/textures when the game issues the copy, and the replay
// copies from there.

bool cpu_written(ID3D11Resource* res) {
  D3D11_RESOURCE_DIMENSION dim;
  res->GetType(&dim);
  D3D11_USAGE u = D3D11_USAGE_DEFAULT;
  switch (dim) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: {
      D3D11_BUFFER_DESC d;
      static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
      u = d.Usage;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
      D3D11_TEXTURE2D_DESC d;
      static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
      u = d.Usage;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
      D3D11_TEXTURE3D_DESC d;
      static_cast<ID3D11Texture3D*>(res)->GetDesc(&d);
      u = d.Usage;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
      D3D11_TEXTURE1D_DESC d;
      static_cast<ID3D11Texture1D*>(res)->GetDesc(&d);
      u = d.Usage;
      break;
    }
    default: break;
  }
  return u == D3D11_USAGE_DYNAMIC || u == D3D11_USAGE_STAGING;
}

struct Arena {
  Ref<ID3D11Buffer> buf;
  UINT size = 0, used = 0;
};
std::vector<Arena> g_arenas;  // reset every frame (GPU-ordered reuse is safe)
std::mutex g_snap_mutex;

// Allocates `n` bytes in an arena buffer; returns it and the offset.
ID3D11Buffer* arena_alloc(ID3D11Device* dev, UINT n, UINT* off) {
  n = (n + 15) & ~15u;
  for (auto& a : g_arenas)
    if (a.size - a.used >= n) {
      *off = a.used;
      a.used += n;
      return a.buf.p;
    }
  D3D11_BUFFER_DESC d{};
  d.ByteWidth = n > (8u << 20) ? n : (8u << 20);
  d.Usage = D3D11_USAGE_DEFAULT;
  ID3D11Buffer* b = nullptr;
  if (FAILED(dev->CreateBuffer(&d, nullptr, &b))) return nullptr;
  Arena a;
  a.buf = Ref<ID3D11Buffer>(b);
  b->Release();
  a.size = d.ByteWidth;
  a.used = n;
  g_arenas.push_back(a);
  *off = 0;
  return b;
}

struct TexSnap {
  Ref<ID3D11Resource> tex;
  bool used = false;
};
std::vector<TexSnap> g_texsnaps;

// A DEFAULT-usage texture like `src` (any free one from the pool).
ID3D11Resource* tex_snap(ID3D11Device* dev, ID3D11Resource* src) {
  D3D11_RESOURCE_DIMENSION dim;
  src->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return nullptr;  // 1D/3D sources: not seen
  D3D11_TEXTURE2D_DESC d;
  static_cast<ID3D11Texture2D*>(src)->GetDesc(&d);
  for (auto& t : g_texsnaps) {
    if (t.used) continue;
    D3D11_TEXTURE2D_DESC e;
    static_cast<ID3D11Texture2D*>(t.tex.p)->GetDesc(&e);
    if (e.Width == d.Width && e.Height == d.Height && e.MipLevels == d.MipLevels && e.ArraySize == d.ArraySize &&
        e.Format == d.Format) {
      t.used = true;
      return t.tex.p;
    }
  }
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = 0;
  d.CPUAccessFlags = 0;
  d.MiscFlags = 0;
  ID3D11Texture2D* t = nullptr;
  if (FAILED(dev->CreateTexture2D(&d, nullptr, &t))) return nullptr;
  TexSnap ts;
  ts.tex = Ref<ID3D11Resource>(t);
  t->Release();
  ts.used = true;
  g_texsnaps.push_back(ts);
  return t;
}

// Replaces (src, ssub, box) with a snapshot of that region. Returns false if not possible.
bool snapshot_source(ID3D11Resource** src, UINT* ssub, D3D11_BOX* box, bool* has_box) {
  ID3D11Device* dev = nullptr;
  (*src)->GetDevice(&dev);
  if (!dev) return false;
  dev->Release();  // the device outlives this call
  D3D11_RESOURCE_DIMENSION dim;
  (*src)->GetType(&dim);
  if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
    UINT lo = *has_box ? box->left : 0, hi = *has_box ? box->right : buffer_width(*src);
    if (hi <= lo) return false;
    UINT off = 0;
    ID3D11Buffer* a = arena_alloc(dev, hi - lo, &off);
    if (!a) return false;
    D3D11_BOX b{lo, 0, 0, hi, 1, 1};
    g_rec_imm->CopySubresourceRegion(a, 0, off, 0, 0, *src, 0, &b);
    *src = a;
    *ssub = 0;
    *box = D3D11_BOX{off, 0, 0, off + (hi - lo), 1, 1};
    *has_box = true;
    return true;
  }
  ID3D11Resource* t = tex_snap(dev, *src);
  if (!t) return false;
  g_rec_imm->CopySubresourceRegion(t, *ssub, 0, 0, 0, *src, *ssub, *has_box ? box : nullptr);
  if (*has_box) {
    // The snapshot holds the region at the origin.
    *box = D3D11_BOX{0, 0, 0, box->right - box->left, box->bottom - box->top, box->back - box->front};
  }
  *src = t;
  return true;
}

}  // namespace

void rec_update(ID3D11Resource* res, UINT sub, const D3D11_BOX* box, const void* data, UINT rp, UINT dp, UINT copy_flags,
                bool is1) {
  if (!data || upload_only(res)) return;
  size_t size = 0;
  UINT w, h, d;
  DXGI_FORMAT fmt;
  if (!sub_dims(res, sub, &w, &h, &d, &fmt)) {
    UINT bw = buffer_width(res);
    size = box ? (box->right > box->left ? box->right - box->left : 0) : bw;
  } else {
    if (box) {
      w = box->right - box->left;
      h = box->bottom - box->top;
      d = box->back - box->front;
    }
    UINT bwid;
    UINT bpb = format_block(fmt, &bwid);
    if (!bpb) {
      rec_unsupported("UpdateSubresource of a texture with an unknown format");
      return;
    }
    UINT rows = (h + bwid - 1) / bwid;
    size_t row_bytes = size_t((w + bwid - 1) / bwid) * bpb;
    if (!w || !rows || !d) return;
    size = size_t(d - 1) * dp + size_t(rows - 1) * rp + row_bytes;
  }
  auto blob = std::make_shared<std::vector<uint8_t>>(size);
  if (size) memcpy(blob->data(), data, size);
  Ref<ID3D11Resource> r(res);
  bool hb = box != nullptr;
  D3D11_BOX b = hb ? *box : D3D11_BOX{};
  std::lock_guard lock(g_mutex);
  if (!g_cur) return;
  g_cur->bytes += size;
  {
    auto& u = g_cur->uploads[{2, size}];
    u.first++;
    u.second += size;
  }
  g_cur->kinds.push_back(uint8_t(RecKind::Upload));
  g_cur->cmds.push_back([r, sub, hb, b, blob, rp, dp, copy_flags, is1](ID3D11DeviceContext* c) {
    if (is1)
      static_cast<ID3D11DeviceContext1*>(c)->UpdateSubresource1(r.p, sub, hb ? &b : nullptr, blob->data(), rp, dp,
                                                                copy_flags);
    else
      c->UpdateSubresource(r.p, sub, hb ? &b : nullptr, blob->data(), rp, dp);
  });
}

void rec_draw(UINT a, UINT b) {
  rec_push([a, b](ID3D11DeviceContext* c) { c->Draw(a, b); }, RecKind::Draw);
}
void rec_draw_indexed(UINT a, UINT b, INT x) {
  rec_push([a, b, x](ID3D11DeviceContext* c) { c->DrawIndexed(a, b, x); }, RecKind::Draw);
}
void rec_draw_instanced(UINT a, UINT b, UINT x, UINT y) {
  rec_push([a, b, x, y](ID3D11DeviceContext* c) { c->DrawInstanced(a, b, x, y); }, RecKind::Draw);
}
void rec_draw_indexed_instanced(UINT a, UINT b, UINT x, INT y, UINT z) {
  rec_push([a, b, x, y, z](ID3D11DeviceContext* c) { c->DrawIndexedInstanced(a, b, x, y, z); }, RecKind::Draw);
}
void rec_draw_indexed_instanced_indirect(ID3D11Buffer* buf, UINT off) {
  Ref<ID3D11Buffer> r(buf);
  rec_push([r, off](ID3D11DeviceContext* c) { c->DrawIndexedInstancedIndirect(r.p, off); }, RecKind::Draw);
}
void rec_draw_instanced_indirect(ID3D11Buffer* buf, UINT off) {
  Ref<ID3D11Buffer> r(buf);
  rec_push([r, off](ID3D11DeviceContext* c) { c->DrawInstancedIndirect(r.p, off); }, RecKind::Draw);
}
void rec_dispatch(UINT x, UINT y, UINT z) {
  rec_push([x, y, z](ID3D11DeviceContext* c) { c->Dispatch(x, y, z); }, RecKind::Dispatch);
}
void rec_dispatch_indirect(ID3D11Buffer* buf, UINT off) {
  Ref<ID3D11Buffer> r(buf);
  rec_push([r, off](ID3D11DeviceContext* c) { c->DispatchIndirect(r.p, off); }, RecKind::Dispatch);
}
void rec_copy_resource(ID3D11Resource* dst, ID3D11Resource* src) {
  if (is_staging(dst) || upload_only(dst)) return;  // readback / pure upload: nothing to redo
  if (cpu_written(src)) {
    std::lock_guard lock(g_snap_mutex);
    D3D11_RESOURCE_DIMENSION dim;
    src->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
      ID3D11Resource* s2 = src;
      UINT ss = 0;
      D3D11_BOX b{};
      bool hb = false;
      if (snapshot_source(&s2, &ss, &b, &hb)) {
        Ref<ID3D11Resource> d(dst), sr(s2);
        rec_push([d, sr, b](ID3D11DeviceContext* c) { c->CopySubresourceRegion(d.p, 0, 0, 0, 0, sr.p, 0, &b); },
                 RecKind::Copy);
        return;
      }
    } else {
      ID3D11Device* dev = nullptr;
      src->GetDevice(&dev);
      ID3D11Resource* t = dev ? tex_snap(dev, src) : nullptr;
      if (dev) dev->Release();
      if (t) {
        g_rec_imm->CopyResource(t, src);
        Ref<ID3D11Resource> d(dst), sr(t);
        rec_push([d, sr](ID3D11DeviceContext* c) { c->CopyResource(d.p, sr.p); }, RecKind::Copy);
        return;
      }
    }
    rec_unsupported("copy from a CPU-written resource that could not be snapshotted");
  }
  Ref<ID3D11Resource> d(dst), s(src);
  rec_push([d, s](ID3D11DeviceContext* c) { c->CopyResource(d.p, s.p); }, RecKind::Copy);
}
void rec_copy_subresource(ID3D11Resource* dst, UINT dsub, UINT x, UINT y, UINT z, ID3D11Resource* src, UINT ssub,
                          const D3D11_BOX* box) {
  if (is_staging(dst) || upload_only(dst)) return;
  bool hb = box != nullptr;
  D3D11_BOX b = hb ? *box : D3D11_BOX{};
  ID3D11Resource* s2 = src;
  if (cpu_written(src)) {
    std::lock_guard lock(g_snap_mutex);
    if (!snapshot_source(&s2, &ssub, &b, &hb)) rec_unsupported("copy from a CPU-written resource that could not be snapshotted");
  }
  Ref<ID3D11Resource> d(dst), s(s2);
  rec_push([=](ID3D11DeviceContext* c) { c->CopySubresourceRegion(d.p, dsub, x, y, z, s.p, ssub, hb ? &b : nullptr); },
           RecKind::Copy);
}
void rec_clear_rtv(ID3D11RenderTargetView* v, const FLOAT col[4]) {
  Ref<ID3D11RenderTargetView> r(v);
  float cv[4];
  memcpy(cv, col, sizeof(cv));
  rec_push([r, cv](ID3D11DeviceContext* c) { c->ClearRenderTargetView(r.p, cv); }, RecKind::Clear);
}
void rec_clear_dsv(ID3D11DepthStencilView* v, UINT f, FLOAT d, UINT8 s) {
  Ref<ID3D11DepthStencilView> r(v);
  rec_push([r, f, d, s](ID3D11DeviceContext* c) { c->ClearDepthStencilView(r.p, f, d, s); }, RecKind::Clear);
}
void rec_clear_uav_float(ID3D11UnorderedAccessView* v, const FLOAT col[4]) {
  Ref<ID3D11UnorderedAccessView> r(v);
  float cv[4];
  memcpy(cv, col, sizeof(cv));
  rec_push([r, cv](ID3D11DeviceContext* c) { c->ClearUnorderedAccessViewFloat(r.p, cv); }, RecKind::Clear);
}
void rec_clear_uav_uint(ID3D11UnorderedAccessView* v, const UINT col[4]) {
  Ref<ID3D11UnorderedAccessView> r(v);
  UINT cv[4];
  memcpy(cv, col, sizeof(cv));
  rec_push([r, cv](ID3D11DeviceContext* c) { c->ClearUnorderedAccessViewUint(r.p, cv); }, RecKind::Clear);
}
void rec_execute(ID3D11CommandList* cl, BOOL restore) {
  Ref<ID3D11CommandList> r(cl);
  rec_push([r, restore](ID3D11DeviceContext* c) { c->ExecuteCommandList(r.p, restore); }, RecKind::Execute);
}
void rec_ps_set_shader(ID3D11PixelShader* s, ID3D11ClassInstance* const*, UINT n) {
  if (n) rec_unsupported("PSSetShader with class instances");
  Ref<ID3D11PixelShader> r(s);
  rec_push([r](ID3D11DeviceContext* c) { c->PSSetShader(r.p, nullptr, 0); });
}
void rec_cs_set_shader(ID3D11ComputeShader* s, ID3D11ClassInstance* const*, UINT n) {
  if (n) rec_unsupported("CSSetShader with class instances");
  Ref<ID3D11ComputeShader> r(s);
  rec_push([r](ID3D11DeviceContext* c) { c->CSSetShader(r.p, nullptr, 0); });
}
void rec_clear_state() {
  rec_push([](ID3D11DeviceContext* c) { c->ClearState(); });
}

void rec_install(ID3D11DeviceContext* imm) {
  if (!g_lo) init_range();
  g_rec_imm = imm;
#define H(name) hook_vtable(imm, Context_##name, reinterpret_cast<void*>(hk_##name))
#define H1(name) hook_vtable(imm, Context1_##name, reinterpret_cast<void*>(hk_##name))
#define HSTAGE(S)          \
  H(S##SetConstantBuffers); \
  H(S##SetShaderResources); \
  H(S##SetSamplers);        \
  H1(S##SetConstantBuffers1)
  HSTAGE(VS);
  HSTAGE(PS);
  HSTAGE(GS);
  HSTAGE(HS);
  HSTAGE(DS);
  HSTAGE(CS);
  H(VSSetShader);
  H(GSSetShader);
  H(HSSetShader);
  H(DSSetShader);
  H(IASetInputLayout);
  H(IASetVertexBuffers);
  H(IASetIndexBuffer);
  H(IASetPrimitiveTopology);
  H(SetPredication);
  H(OMSetRenderTargets);
  H(OMSetRenderTargetsAndUnorderedAccessViews);
  H(OMSetBlendState);
  H(OMSetDepthStencilState);
  H(SOSetTargets);
  H(RSSetState);
  H(RSSetViewports);
  H(RSSetScissorRects);
  H(CopyStructureCount);
  H(GenerateMips);
  H(SetResourceMinLOD);
  H(ResolveSubresource);
  H(CSSetUnorderedAccessViews);
  H1(CopySubresourceRegion1);
  H1(UpdateSubresource1);
  H1(DiscardResource);
  H1(DiscardView);
  H1(DiscardView1);
  H1(ClearView);
  H1(SwapDeviceContextState);
#undef HSTAGE
#undef H1
#undef H
  ACO_LOG("stereo: immediate-context recorder installed on %p", static_cast<void*>(imm));
}

void rec_start() {
  std::lock_guard lock(g_mutex);
  delete g_cur;
  g_cur = new RecFrame;
  g_cur->cmds.reserve(4096);
  if (g_stopped_at != g_frame_no) ++g_epoch;  // recording resumes after a gap: mirrors may be stale
  for (auto& a : g_arenas) a.used = 0;
  for (auto& t : g_texsnaps) t.used = false;
  ++g_frame_no;
  // Drop shadows of buffers not mapped for a while (releases the resources).
  if (g_frame_no % 256 == 0)
    for (auto it = g_shadows.begin(); it != g_shadows.end();)
      it = g_frame_no - it->second.last_frame > 256 ? g_shadows.erase(it) : std::next(it);
  g_rec_on.store(true, std::memory_order_release);
}

RecFrame* rec_stop() {
  std::lock_guard lock(g_mutex);
  g_rec_on.store(false, std::memory_order_release);
  RecFrame* f = g_cur;
  g_cur = nullptr;
  if (f) g_stopped_at = g_frame_no;
  if (f && !g_open.empty() && !f->bad) f->bad = "Map still open at Present";
  return f;
}

bool rec_frame_ok(const RecFrame* f, const char** why) {
  if (!f) {
    *why = "no frame";
    return false;
  }
  *why = f->bad;
  return !f->bad;
}
size_t rec_frame_size(const RecFrame* f) { return f ? f->cmds.size() : 0; }
size_t rec_frame_bytes(const RecFrame* f) { return f ? f->bytes : 0; }

void rec_replay(const RecFrame* f, ID3D11DeviceContext* ctx, unsigned skip_mask) {
  if (!f) return;
  ++g_rec_replaying;
  for (size_t i = 0; i < f->cmds.size(); ++i)
    if (!(skip_mask & (1u << f->kinds[i]))) f->cmds[i](ctx);
  --g_rec_replaying;
}

void rec_free(RecFrame* f) { delete f; }

void rec_log_uploads(const RecFrame* f) {
  if (!f) return;
  std::vector<std::pair<size_t, std::string>> v;
  for (auto& [k, c] : f->uploads) {
    char b[128];
    snprintf(b, sizeof(b), "%s %zu B x%zu", k.first == 0 ? "discard" : k.first == 1 ? "nooverwrite" : k.first == 2 ? "update" : k.first == 3 ? "big-discard" : "big-nooverwrite", k.second,
             c.first);
    v.emplace_back(c.second, b);
  }
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
  std::string s;
  for (size_t i = 0; i < v.size() && i < 12; ++i) s += (i ? ", " : "") + v[i].second;
  ACO_LOG("stereo: frame uploads %.2f MB copied in %zu calls, %d large buffers discarded twice: %s", f->bytes / 1048576.0,
          f->cmds.size(), f->big_rediscards, s.c_str());
}

}  // namespace aco
