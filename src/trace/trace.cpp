#include "trace/trace.h"

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/cbshadow.h"
#include "core/hook.h"
#include "core/log.h"
#include "core/shaders.h"
#include "core/vtidx.h"

namespace aco {

namespace {

constexpr int kCaptureFrames = 3;  // default; the trigger file may contain another count
int g_capture_frames = kCaptureFrames;

std::atomic<int> g_capture_left{0};  // frames left to capture (0 = idle)
std::atomic<uint64_t> g_frame{0};
ID3D11DeviceContext* g_immediate = nullptr;

std::mutex g_cap_mutex;
std::vector<std::string> g_events;                                          // immediate stream
std::unordered_map<ID3D11DeviceContext*, std::vector<std::string>> g_deferred;  // per deferred ctx
std::unordered_map<ID3D11CommandList*, std::vector<std::string>> g_cmdlists;
std::set<ID3D11DeviceChild*> g_used_shaders;
std::wstring g_cap_dir;
int g_cap_index = 0;
std::vector<std::string> g_cb_watch;  // shader hash prefixes whose constant buffer contents we dump
bool g_preroll = false;               // shadowing enabled, capture starts next frame

bool cb_watched(ID3D11DeviceChild* sh) {
  if (!sh || g_cb_watch.empty()) return false;
  std::string h = hex64(shader_hash(sh));
  for (auto& w : g_cb_watch)
    if (h.compare(0, w.size(), w) == 0) return true;
  return false;
}

void load_cb_watch() {
  g_cb_watch.clear();
  if (FILE* f = _wfopen((out_dir() + L"\\cbwatch.txt").c_str(), L"r")) {
    char line[128];
    while (fgets(line, sizeof(line), f)) {
      std::string w(line);
      while (!w.empty() && (w.back() == '\n' || w.back() == '\r' || w.back() == ' ')) w.pop_back();
      if (!w.empty()) g_cb_watch.push_back(w);
    }
    fclose(f);
  }
}

bool capturing() { return g_capture_left.load(std::memory_order_relaxed) > 0; }

// skipdraw experiment (see trace.h): spec guarded by its own mutex, polled every 30 frames.
std::atomic<bool> g_skip_on{false};
std::atomic<long> g_skip_lo{-1}, g_skip_hi{-1};   // range form (@lo-hi); lo < 0 = shader form
std::atomic<long> g_skip_ctx{-1};                 // per-context form (@ctx:...); -1 = all contexts
std::atomic<unsigned long long> g_skip_n{0};      // draws since Present (all-context range form)
struct SkipCtx {
  std::atomic<ID3D11DeviceContext*> ctx{nullptr};
  std::atomic<unsigned long long> n{0};
};
SkipCtx g_skip_ctxs[64];  // first-seen order = the @ctx: ids
std::mutex g_skip_mutex;

int skip_ctx_id(ID3D11DeviceContext* ctx) {
  for (int i = 0; i < 64; ++i) {
    ID3D11DeviceContext* c = g_skip_ctxs[i].ctx.load(std::memory_order_acquire);
    if (c == ctx) return i;
    if (!c) break;
  }
  std::lock_guard lock(g_skip_mutex);
  for (int i = 0; i < 64; ++i) {
    ID3D11DeviceContext* c = g_skip_ctxs[i].ctx.load();
    if (c == ctx) return i;
    if (!c) {
      g_skip_ctxs[i].ctx.store(ctx, std::memory_order_release);
      return i;
    }
  }
  return -1;
}
std::string g_skip_prefix;   // pixel-shader hash prefix (hex); empty = off
long g_skip_count = -1;      // vertex/index/instance count to match, -1 = any
std::string g_skip_raw;      // last file content seen (log on change only)

void poll_skipdraw() {
  std::string raw;
  if (FILE* f = _wfopen((out_dir() + L"\\skipdraw.txt").c_str(), L"r")) {
    char line[128] = "";
    if (fgets(line, sizeof(line), f)) raw = line;
    fclose(f);
  }
  while (!raw.empty() && (raw.back() == '\n' || raw.back() == '\r' || raw.back() == ' ')) raw.pop_back();
  std::lock_guard lock(g_skip_mutex);
  if (raw == g_skip_raw) return;
  g_skip_raw = raw;
  g_skip_prefix.clear();
  g_skip_count = -1;
  g_skip_lo.store(-1, std::memory_order_relaxed);
  g_skip_hi.store(-1, std::memory_order_relaxed);
  g_skip_ctx.store(-1, std::memory_order_relaxed);
  if (!raw.empty() && raw[0] == '@') {
    long a = -1, lo = -1, hi = -1;
    if (sscanf(raw.c_str(), "@%ld:%ld-%ld", &a, &lo, &hi) == 3 && a >= 0 && lo >= 0 && hi > lo) {
      g_skip_ctx.store(a, std::memory_order_relaxed);
      g_skip_lo.store(lo, std::memory_order_relaxed);
      g_skip_hi.store(hi, std::memory_order_relaxed);
      g_skip_prefix = "@";  // marks active (never matches a hex hash)
    } else if (sscanf(raw.c_str(), "@%ld:*", &a) == 1 && a >= 0) {
      g_skip_ctx.store(a, std::memory_order_relaxed);
      g_skip_lo.store(0, std::memory_order_relaxed);
      g_skip_hi.store(1L << 40, std::memory_order_relaxed);
      g_skip_prefix = "@";
    } else if (sscanf(raw.c_str(), "@%ld-%ld", &lo, &hi) == 2 && lo >= 0 && hi > lo) {
      g_skip_lo.store(lo, std::memory_order_relaxed);
      g_skip_hi.store(hi, std::memory_order_relaxed);
      g_skip_prefix = "@";
    }
  } else if (!raw.empty()) {
    char pfx[64] = "";
    long n = -1;
    if (sscanf(raw.c_str(), "%63s %ld", pfx, &n) >= 1) {
      g_skip_prefix = pfx;
      g_skip_count = n;
    }
  }
  g_skip_on.store(!g_skip_prefix.empty(), std::memory_order_relaxed);
  ACO_LOG("skipdraw: %s", g_skip_prefix.empty() ? "off" : raw.c_str());
}

void appendf(std::string& s, const char* fmt, ...) __attribute__((format(gnu_printf, 2, 3)));
void appendf(std::string& s, const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0) s.append(buf, std::min<size_t>(n, sizeof(buf) - 1));
}

std::string resource_name(ID3D11Resource* r) {
  char name[256];
  UINT size = sizeof(name) - 1;
  if (SUCCEEDED(r->GetPrivateData(WKPDID_D3DDebugObjectName, &size, name))) {
    name[size] = 0;
    for (char* c = name; *c; ++c)
      if (*c == '"' || *c == '\\' || *c < 32) *c = '_';
    return name;
  }
  return {};
}

void describe_resource(std::string& s, ID3D11Resource* r) {
  D3D11_RESOURCE_DIMENSION dim;
  r->GetType(&dim);
  appendf(s, "\"r\":\"%p\"", static_cast<void*>(r));
  if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
    D3D11_TEXTURE2D_DESC d;
    static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
    appendf(s, ",\"t\":\"2d\",\"w\":%u,\"h\":%u,\"arr\":%u,\"mips\":%u,\"fmt\":%d,\"ms\":%u,\"bind\":%u,\"use\":%d",
            d.Width, d.Height, d.ArraySize, d.MipLevels, d.Format, d.SampleDesc.Count, d.BindFlags, d.Usage);
  } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
    D3D11_TEXTURE3D_DESC d;
    static_cast<ID3D11Texture3D*>(r)->GetDesc(&d);
    appendf(s, ",\"t\":\"3d\",\"w\":%u,\"h\":%u,\"d\":%u,\"mips\":%u,\"fmt\":%d,\"bind\":%u", d.Width, d.Height,
            d.Depth, d.MipLevels, d.Format, d.BindFlags);
  } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
    D3D11_BUFFER_DESC d;
    static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
    appendf(s, ",\"t\":\"buf\",\"size\":%u,\"stride\":%u,\"bind\":%u,\"misc\":%u,\"use\":%d", d.ByteWidth,
            d.StructureByteStride, d.BindFlags, d.MiscFlags, d.Usage);
  } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
    D3D11_TEXTURE1D_DESC d;
    static_cast<ID3D11Texture1D*>(r)->GetDesc(&d);
    appendf(s, ",\"t\":\"1d\",\"w\":%u,\"fmt\":%d", d.Width, d.Format);
  }
  std::string n = resource_name(r);
  if (!n.empty()) appendf(s, ",\"name\":\"%s\"", n.c_str());
}

// View descriptions: resource + view format + subresource.
void describe_view(std::string& s, ID3D11View* v, int vfmt, int mip, int slice) {
  ID3D11Resource* r = nullptr;
  v->GetResource(&r);
  s += "{";
  describe_resource(s, r);
  appendf(s, ",\"vfmt\":%d,\"mip\":%d,\"slice\":%d}", vfmt, mip, slice);
  r->Release();
}

void describe_srv(std::string& s, ID3D11ShaderResourceView* v) {
  D3D11_SHADER_RESOURCE_VIEW_DESC d;
  v->GetDesc(&d);
  int mip = d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D ? (int)d.Texture2D.MostDetailedMip : -1;
  describe_view(s, v, d.Format, mip, -1);
}
void describe_uav(std::string& s, ID3D11UnorderedAccessView* v) {
  D3D11_UNORDERED_ACCESS_VIEW_DESC d;
  v->GetDesc(&d);
  int mip = d.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D ? (int)d.Texture2D.MipSlice : -1;
  describe_view(s, v, d.Format, mip, -1);
}
void describe_rtv(std::string& s, ID3D11RenderTargetView* v) {
  D3D11_RENDER_TARGET_VIEW_DESC d;
  v->GetDesc(&d);
  int mip = -1, slice = -1;
  if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) mip = d.Texture2D.MipSlice;
  if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) {
    mip = d.Texture2DArray.MipSlice;
    slice = d.Texture2DArray.FirstArraySlice;
  }
  describe_view(s, v, d.Format, mip, slice);
}
void describe_dsv(std::string& s, ID3D11DepthStencilView* v) {
  D3D11_DEPTH_STENCIL_VIEW_DESC d;
  v->GetDesc(&d);
  int mip = d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D ? (int)d.Texture2D.MipSlice : -1;
  describe_view(s, v, d.Format, mip, -1);
}

void note_shader(std::string& s, const char* key, ID3D11DeviceChild* sh) {
  if (!sh) return;
  uint64_t h = shader_hash(sh);
  appendf(s, ",\"%s\":\"%s\"", key, hex64(h).c_str());
  g_used_shaders.insert(sh);  // caller holds g_cap_mutex
}

template <typename T, typename Fn>
void srv_list(std::string& s, const char* key, Fn getter, UINT count) {
  std::vector<T*> v(count, nullptr);
  getter(0, count, v.data());
  bool first = true;
  for (UINT i = 0; i < count; ++i) {
    if (!v[i]) continue;
    if (first) appendf(s, ",\"%s\":{", key);
    else s += ",";
    appendf(s, "\"%u\":", i);
    first = false;
    if constexpr (std::is_same_v<T, ID3D11ShaderResourceView>) describe_srv(s, v[i]);
    else describe_uav(s, v[i]);
    v[i]->Release();
  }
  if (!first) s += "}";
}

void cb_list(std::string& s, const char* key, ID3D11DeviceContext* ctx, bool compute, bool pixel, bool data = false) {
  ID3D11DeviceContext1* c1 = nullptr;
  ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1));
  ID3D11Buffer* b[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
  UINT first[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {}, num[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
  constexpr UINT n = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
  if (c1) {
    if (compute) c1->CSGetConstantBuffers1(0, n, b, first, num);
    else if (pixel) c1->PSGetConstantBuffers1(0, n, b, first, num);
    else c1->VSGetConstantBuffers1(0, n, b, first, num);
    c1->Release();
  } else {
    if (compute) ctx->CSGetConstantBuffers(0, n, b);
    else if (pixel) ctx->PSGetConstantBuffers(0, n, b);
    else ctx->VSGetConstantBuffers(0, n, b);
  }
  bool any = false;
  std::string dump;
  for (UINT i = 0; i < n; ++i) {
    if (!b[i]) continue;
    D3D11_BUFFER_DESC d;
    b[i]->GetDesc(&d);
    appendf(s, "%s\"%u\":[\"%p\",%u,%u,%u]", any ? "," : (std::string(",\"") + key + "\":{").c_str(), i,
            static_cast<void*>(b[i]), d.ByteWidth, first[i] * 16, num[i] * 16);
    any = true;
    if (data) {
      uint32_t off = first[i] * 16, size = num[i] ? num[i] * 16 : d.ByteWidth;
      if (off + size > d.ByteWidth) size = d.ByteWidth > off ? d.ByteWidth - off : 0;
      std::vector<float> f(size / 4);
      if (size && cbshadow_read(ctx, b[i], off, size, f.data())) {
        appendf(dump, "%s\"%u\":[", dump.empty() ? "" : ",", i);
        for (size_t k = 0; k < f.size(); ++k) {
          if (k) dump += ",";
          if (std::isfinite(f[k])) appendf(dump, "%.9g", f[k]);
          else dump += "null";
        }
        dump += "]";
      }
    }
    b[i]->Release();
  }
  if (any) s += "}";
  if (!dump.empty()) s += std::string(",\"") + key + "d\":{" + dump + "}";
}

std::vector<std::string>& stream_for(ID3D11DeviceContext* ctx) {
  return ctx == g_immediate ? g_events : g_deferred[ctx];
}

void record(ID3D11DeviceContext* ctx, std::string&& line) {
  std::lock_guard lock(g_cap_mutex);
  stream_for(ctx).push_back(std::move(line));
}

void record_draw(ID3D11DeviceContext* ctx, const char* op, const char* args) {
  std::string s;
  appendf(s, "{\"op\":\"%s\",\"args\":[%s]", op, args);
  std::lock_guard lock(g_cap_mutex);
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ctx->VSGetShader(&vs, nullptr, nullptr);
  ctx->PSGetShader(&ps, nullptr, nullptr);
  note_shader(s, "vs", vs);
  note_shader(s, "ps", ps);
  ID3D11RenderTargetView* rtv[8] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  ctx->OMGetRenderTargets(8, rtv, &dsv);
  bool any = false;
  for (int i = 0; i < 8; ++i) {
    if (!rtv[i]) continue;
    appendf(s, "%s\"%d\":", any ? "," : ",\"rt\":{", i);
    describe_rtv(s, rtv[i]);
    rtv[i]->Release();
    any = true;
  }
  if (any) s += "}";
  if (dsv) {
    s += ",\"ds\":";
    describe_dsv(s, dsv);
    dsv->Release();
  }
  srv_list<ID3D11ShaderResourceView>(s, "srv", [&](UINT a, UINT b, ID3D11ShaderResourceView** v) { ctx->PSGetShaderResources(a, b, v); }, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
  bool watched = cb_watched(ps);
  cb_list(s, "pscb", ctx, false, true, watched);
  cb_list(s, "vscb", ctx, false, false, watched);
  D3D11_VIEWPORT vp[1];
  UINT nvp = 1;
  ctx->RSGetViewports(&nvp, vp);
  if (nvp) appendf(s, ",\"vp\":[%g,%g,%g,%g]", vp[0].TopLeftX, vp[0].TopLeftY, vp[0].Width, vp[0].Height);
  s += "}";
  if (vs) vs->Release();
  if (ps) ps->Release();
  stream_for(ctx).push_back(std::move(s));
}

void record_dispatch(ID3D11DeviceContext* ctx, const char* op, const char* args) {
  std::string s;
  appendf(s, "{\"op\":\"%s\",\"args\":[%s]", op, args);
  std::lock_guard lock(g_cap_mutex);
  ID3D11ComputeShader* cs = nullptr;
  ctx->CSGetShader(&cs, nullptr, nullptr);
  note_shader(s, "cs", cs);
  srv_list<ID3D11ShaderResourceView>(s, "srv", [&](UINT a, UINT b, ID3D11ShaderResourceView** v) { ctx->CSGetShaderResources(a, b, v); }, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
  srv_list<ID3D11UnorderedAccessView>(s, "uav", [&](UINT a, UINT b, ID3D11UnorderedAccessView** v) { ctx->CSGetUnorderedAccessViews(a, b, v); }, 8);
  cb_list(s, "cscb", ctx, true, false, cb_watched(cs));
  s += "}";
  if (cs) cs->Release();
  stream_for(ctx).push_back(std::move(s));
}

void finish_capture() {
  std::lock_guard lock(g_cap_mutex);
  std::wstring dir = g_cap_dir;
  std::wstring sdir = dir + L"\\shaders";
  CreateDirectoryW(sdir.c_str(), nullptr);
  if (FILE* f = _wfopen((dir + L"\\events.jsonl").c_str(), L"wb")) {
    for (auto& e : g_events) {
      fwrite(e.data(), 1, e.size(), f);
      fputc('\n', f);
    }
    fclose(f);
  }
  for (auto* sh : g_used_shaders)
    if (auto* info = shader_info(sh)) shader_dump(*info, sdir);
  ACO_LOG("capture %d written: %zu events, %zu shaders", g_cap_index, g_events.size(), g_used_shaders.size());
  g_events.clear();
  g_used_shaders.clear();
}

} // namespace

bool trace_capturing() { return capturing(); }

bool skipdraw_active() { return g_skip_on.load(std::memory_order_relaxed); }

void skipdraw_reset_ctx(ID3D11DeviceContext* ctx) {
  int id = skip_ctx_id(ctx);
  if (id >= 0) g_skip_ctxs[id].n.store(0, std::memory_order_relaxed);
}

bool skipdraw_range_hit(ID3D11DeviceContext* ctx) {
  long lo = g_skip_lo.load(std::memory_order_relaxed);
  if (lo < 0) return false;
  long want = g_skip_ctx.load(std::memory_order_relaxed);
  unsigned long long n;
  if (want >= 0) {
    int id = skip_ctx_id(ctx);
    if (id < 0) return false;
    n = g_skip_ctxs[id].n.fetch_add(1, std::memory_order_relaxed);
    if (id != want) return false;
  } else {
    n = g_skip_n.fetch_add(1, std::memory_order_relaxed);
  }
  return n >= static_cast<unsigned long long>(lo) &&
         n < static_cast<unsigned long long>(g_skip_hi.load(std::memory_order_relaxed));
}

bool skipdraw_match(unsigned long long ps_hash, unsigned count) {
  std::lock_guard lock(g_skip_mutex);
  if (g_skip_prefix.empty()) return false;
  if (g_skip_count >= 0 && count != static_cast<unsigned>(g_skip_count)) return false;
  std::string h = hex64(ps_hash);
  return h.compare(0, g_skip_prefix.size(), g_skip_prefix) == 0;
}
void trace_set_immediate(ID3D11DeviceContext* ctx) { g_immediate = ctx; }

void trace_draw(ID3D11DeviceContext* ctx, const char* op, const char* args) {
  if (capturing()) record_draw(ctx, op, args);
}

void trace_dispatch(ID3D11DeviceContext* ctx, const char* op, const char* args) {
  if (capturing()) record_dispatch(ctx, op, args);
}

void trace_copy_resource(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src) {
  if (!capturing()) return;
  std::string s = "{\"op\":\"CopyResource\",\"dst\":{";
  describe_resource(s, dst);
  s += "},\"src\":{";
  describe_resource(s, src);
  s += "}}";
  record(ctx, std::move(s));
}

void trace_copy_subresource(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT dsub, UINT x, UINT y, UINT z,
                            ID3D11Resource* src, UINT ssub, const D3D11_BOX* box) {
  if (!capturing()) return;
  std::string s;
  appendf(s, "{\"op\":\"CopySubresourceRegion\",\"dsub\":%u,\"ssub\":%u,\"at\":[%u,%u,%u],\"dst\":{", dsub, ssub, x, y, z);
  describe_resource(s, dst);
  s += "},\"src\":{";
  describe_resource(s, src);
  s += "}";
  if (box) appendf(s, ",\"box\":[%u,%u,%u,%u]", box->left, box->top, box->right, box->bottom);
  s += "}";
  record(ctx, std::move(s));
}

void trace_clear_rtv(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* v, const FLOAT c[4]) {
  if (!capturing() || !v) return;
  std::string s;
  appendf(s, "{\"op\":\"ClearRTV\",\"color\":[%g,%g,%g,%g],\"view\":", c[0], c[1], c[2], c[3]);
  describe_rtv(s, v);
  s += "}";
  record(ctx, std::move(s));
}

void trace_clear_dsv(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* v, UINT f, FLOAT d, UINT8 st) {
  if (!capturing() || !v) return;
  std::string s;
  appendf(s, "{\"op\":\"ClearDSV\",\"flags\":%u,\"depth\":%g,\"stencil\":%u,\"view\":", f, d, st);
  describe_dsv(s, v);
  s += "}";
  record(ctx, std::move(s));
}

void trace_clear_uav(ID3D11DeviceContext* ctx, ID3D11UnorderedAccessView* v, bool is_float) {
  if (!capturing() || !v) return;
  std::string s = is_float ? "{\"op\":\"ClearUAVFloat\",\"view\":" : "{\"op\":\"ClearUAVUint\",\"view\":";
  describe_uav(s, v);
  s += "}";
  record(ctx, std::move(s));
}

void trace_note(ID3D11DeviceContext* ctx, const std::string& json_fields) {
  if (!capturing()) return;
  record(ctx, "{\"op\":\"Note\"," + json_fields + "}");
}

void trace_finish_command_list(ID3D11DeviceContext* ctx, ID3D11CommandList* cl) {
  std::lock_guard lock(g_cap_mutex);
  auto it = g_deferred.find(ctx);
  if (it != g_deferred.end()) {
    if (!it->second.empty()) {
      auto& dst = g_cmdlists[cl];
      dst.insert(dst.end(), std::make_move_iterator(it->second.begin()), std::make_move_iterator(it->second.end()));
    }
    g_deferred.erase(it);
  }
}

void trace_execute_command_list(ID3D11DeviceContext* ctx, ID3D11CommandList* cl) {
  std::lock_guard lock(g_cap_mutex);
  auto it = g_cmdlists.find(cl);
  if (capturing()) {
    std::string s;
    appendf(s, "{\"op\":\"ExecuteCommandList\",\"cl\":\"%p\",\"n\":%zu}", static_cast<void*>(cl),
            it == g_cmdlists.end() ? (size_t)0 : it->second.size());
    auto& out = stream_for(ctx);
    out.push_back(std::move(s));
    if (it != g_cmdlists.end())
      for (auto& e : it->second) out.push_back("{\"in_cl\":1," + e.substr(1));
  }
  if (it != g_cmdlists.end()) g_cmdlists.erase(it);
}

void trace_on_present(IDXGISwapChain* sc) {
  g_skip_n.store(0, std::memory_order_relaxed);
  if (g_immediate) skipdraw_reset_ctx(g_immediate);  // deferred ctxs reset at FinishCommandList
  uint64_t frame = g_frame.fetch_add(1) + 1;
  if (g_preroll) {
    g_preroll = false;
    g_capture_left.store(g_capture_frames);
    ACO_LOG("capture %d started at frame %llu", g_cap_index, (unsigned long long)frame);
    return;
  }
  int left = g_capture_left.load();
  if (left > 0) {
    {
      std::lock_guard lock(g_cap_mutex);
      std::string s;
      appendf(s, "{\"op\":\"Present\",\"frame\":%llu}", (unsigned long long)frame);
      g_events.push_back(std::move(s));
    }
    if (left == 1) {
      finish_capture();
      cbshadow_set_all(false);
    }
    g_capture_left.store(left - 1);
    return;
  }
  // Poll for the trigger file every 30 frames.
  if (frame % 30 == 0) {
    poll_skipdraw();
    std::wstring trig = out_dir() + L"\\capture";
    if (GetFileAttributesW(trig.c_str()) != INVALID_FILE_ATTRIBUTES) {
      g_capture_frames = kCaptureFrames;
      if (FILE* f = _wfopen(trig.c_str(), L"r")) {
        int n = 0;
        if (fscanf(f, "%d", &n) == 1 && n > 0 && n <= 64) g_capture_frames = n;
        fclose(f);
      }
      DeleteFileW(trig.c_str());
      wchar_t name[64];
      swprintf(name, 64, L"\\cap-%03d", ++g_cap_index);
      g_cap_dir = out_dir() + name;
      CreateDirectoryW(g_cap_dir.c_str(), nullptr);
      {
        std::lock_guard lock(g_cap_mutex);
        g_events.clear();
        g_deferred.clear();
        g_cmdlists.clear();
        g_used_shaders.clear();
      }
      load_cb_watch();
      ACO_LOG("capture %d armed at frame %llu (%zu cb watch entries)", g_cap_index, (unsigned long long)frame, g_cb_watch.size());
      // One frame of constant-buffer shadowing before recording, so draws early in the captured
      // frames can see buffers written in the previous frame.
      cbshadow_set_all(true);
      g_preroll = true;
    }
  }
}

} // namespace aco
