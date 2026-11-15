#include "trace/trace.h"

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/hook.h"
#include "core/log.h"
#include "core/shaders.h"
#include "core/vtidx.h"

namespace aco {

namespace {

constexpr int kCaptureFrames = 3;

std::atomic<int> g_capture_left{0};  // frames left to capture (0 = idle)
std::atomic<uint64_t> g_frame{0};
std::atomic<uint64_t> g_draws_imm{0}, g_draws_def{0}, g_dispatch_imm{0}, g_dispatch_def{0};
ID3D11DeviceContext* g_immediate = nullptr;

std::mutex g_cap_mutex;
std::vector<std::string> g_events;                                          // immediate stream
std::unordered_map<ID3D11DeviceContext*, std::vector<std::string>> g_deferred;  // per deferred ctx
std::unordered_map<ID3D11CommandList*, std::vector<std::string>> g_cmdlists;
std::set<ID3D11DeviceChild*> g_used_shaders;
std::wstring g_cap_dir;
int g_cap_index = 0;

bool capturing() { return g_capture_left.load(std::memory_order_relaxed) > 0; }

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

void cb_list(std::string& s, const char* key, ID3D11DeviceContext* ctx, bool compute, bool pixel) {
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
  for (UINT i = 0; i < n; ++i) {
    if (!b[i]) continue;
    D3D11_BUFFER_DESC d;
    b[i]->GetDesc(&d);
    appendf(s, "%s\"%u\":[\"%p\",%u,%u,%u]", any ? "," : (std::string(",\"") + key + "\":{").c_str(), i,
            static_cast<void*>(b[i]), d.ByteWidth, first[i] * 16, num[i] * 16);
    any = true;
    b[i]->Release();
  }
  if (any) s += "}";
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
  srv_list<ID3D11ShaderResourceView>(s, "srv", [&](UINT a, UINT b, ID3D11ShaderResourceView** v) { ctx->PSGetShaderResources(a, b, v); }, 32);
  cb_list(s, "pscb", ctx, false, true);
  cb_list(s, "vscb", ctx, false, false);
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
  srv_list<ID3D11ShaderResourceView>(s, "srv", [&](UINT a, UINT b, ID3D11ShaderResourceView** v) { ctx->CSGetShaderResources(a, b, v); }, 32);
  srv_list<ID3D11UnorderedAccessView>(s, "uav", [&](UINT a, UINT b, ID3D11UnorderedAccessView** v) { ctx->CSGetUnorderedAccessViews(a, b, v); }, 8);
  cb_list(s, "cscb", ctx, true, false);
  s += "}";
  if (cs) cs->Release();
  stream_for(ctx).push_back(std::move(s));
}

// ---- hooks -------------------------------------------------------------------------------------

using namespace vt;

#define ORIG(T, slot) orig<T>(self, slot)

void STDMETHODCALLTYPE hk_Draw(ID3D11DeviceContext* self, UINT a, UINT b) {
  (self == g_immediate ? g_draws_imm : g_draws_def).fetch_add(1, std::memory_order_relaxed);
  if (capturing()) {
    char args[64];
    snprintf(args, sizeof(args), "%u,%u", a, b);
    record_draw(self, "Draw", args);
  }
  ORIG(decltype(hk_Draw), Context_Draw)(self, a, b);
}
void STDMETHODCALLTYPE hk_DrawIndexed(ID3D11DeviceContext* self, UINT a, UINT b, INT c) {
  (self == g_immediate ? g_draws_imm : g_draws_def).fetch_add(1, std::memory_order_relaxed);
  if (capturing()) {
    char args[64];
    snprintf(args, sizeof(args), "%u,%u,%d", a, b, c);
    record_draw(self, "DrawIndexed", args);
  }
  ORIG(decltype(hk_DrawIndexed), Context_DrawIndexed)(self, a, b, c);
}
void STDMETHODCALLTYPE hk_DrawInstanced(ID3D11DeviceContext* self, UINT a, UINT b, UINT c, UINT d) {
  (self == g_immediate ? g_draws_imm : g_draws_def).fetch_add(1, std::memory_order_relaxed);
  if (capturing()) {
    char args[64];
    snprintf(args, sizeof(args), "%u,%u,%u,%u", a, b, c, d);
    record_draw(self, "DrawInstanced", args);
  }
  ORIG(decltype(hk_DrawInstanced), Context_DrawInstanced)(self, a, b, c, d);
}
void STDMETHODCALLTYPE hk_DrawIndexedInstanced(ID3D11DeviceContext* self, UINT a, UINT b, UINT c, INT d, UINT e) {
  (self == g_immediate ? g_draws_imm : g_draws_def).fetch_add(1, std::memory_order_relaxed);
  if (capturing()) {
    char args[80];
    snprintf(args, sizeof(args), "%u,%u,%u,%d,%u", a, b, c, d, e);
    record_draw(self, "DrawIndexedInstanced", args);
  }
  ORIG(decltype(hk_DrawIndexedInstanced), Context_DrawIndexedInstanced)(self, a, b, c, d, e);
}
void STDMETHODCALLTYPE hk_DrawIndexedInstancedIndirect(ID3D11DeviceContext* self, ID3D11Buffer* buf, UINT off) {
  (self == g_immediate ? g_draws_imm : g_draws_def).fetch_add(1, std::memory_order_relaxed);
  if (capturing()) {
    char args[64];
    snprintf(args, sizeof(args), "\"%p\",%u", static_cast<void*>(buf), off);
    record_draw(self, "DrawIndexedInstancedIndirect", args);
  }
  ORIG(decltype(hk_DrawIndexedInstancedIndirect), Context_DrawIndexedInstancedIndirect)(self, buf, off);
}
void STDMETHODCALLTYPE hk_DrawInstancedIndirect(ID3D11DeviceContext* self, ID3D11Buffer* buf, UINT off) {
  (self == g_immediate ? g_draws_imm : g_draws_def).fetch_add(1, std::memory_order_relaxed);
  if (capturing()) {
    char args[64];
    snprintf(args, sizeof(args), "\"%p\",%u", static_cast<void*>(buf), off);
    record_draw(self, "DrawInstancedIndirect", args);
  }
  ORIG(decltype(hk_DrawInstancedIndirect), Context_DrawInstancedIndirect)(self, buf, off);
}
void STDMETHODCALLTYPE hk_Dispatch(ID3D11DeviceContext* self, UINT x, UINT y, UINT z) {
  (self == g_immediate ? g_dispatch_imm : g_dispatch_def).fetch_add(1, std::memory_order_relaxed);
  if (capturing()) {
    char args[64];
    snprintf(args, sizeof(args), "%u,%u,%u", x, y, z);
    record_dispatch(self, "Dispatch", args);
  }
  ORIG(decltype(hk_Dispatch), Context_Dispatch)(self, x, y, z);
}
void STDMETHODCALLTYPE hk_DispatchIndirect(ID3D11DeviceContext* self, ID3D11Buffer* buf, UINT off) {
  (self == g_immediate ? g_dispatch_imm : g_dispatch_def).fetch_add(1, std::memory_order_relaxed);
  if (capturing()) {
    char args[64];
    snprintf(args, sizeof(args), "\"%p\",%u", static_cast<void*>(buf), off);
    record_dispatch(self, "DispatchIndirect", args);
  }
  ORIG(decltype(hk_DispatchIndirect), Context_DispatchIndirect)(self, buf, off);
}
void STDMETHODCALLTYPE hk_CopyResource(ID3D11DeviceContext* self, ID3D11Resource* dst, ID3D11Resource* src) {
  if (capturing()) {
    std::string s = "{\"op\":\"CopyResource\",\"dst\":{";
    describe_resource(s, dst);
    s += "},\"src\":{";
    describe_resource(s, src);
    s += "}}";
    record(self, std::move(s));
  }
  ORIG(decltype(hk_CopyResource), Context_CopyResource)(self, dst, src);
}
void STDMETHODCALLTYPE hk_CopySubresourceRegion(ID3D11DeviceContext* self, ID3D11Resource* dst, UINT dsub, UINT x,
                                                UINT y, UINT z, ID3D11Resource* src, UINT ssub, const D3D11_BOX* box) {
  if (capturing()) {
    std::string s;
    appendf(s, "{\"op\":\"CopySubresourceRegion\",\"dsub\":%u,\"ssub\":%u,\"at\":[%u,%u,%u],\"dst\":{", dsub, ssub, x, y, z);
    describe_resource(s, dst);
    s += "},\"src\":{";
    describe_resource(s, src);
    s += "}";
    if (box) appendf(s, ",\"box\":[%u,%u,%u,%u]", box->left, box->top, box->right, box->bottom);
    s += "}";
    record(self, std::move(s));
  }
  ORIG(decltype(hk_CopySubresourceRegion), Context_CopySubresourceRegion)(self, dst, dsub, x, y, z, src, ssub, box);
}
void STDMETHODCALLTYPE hk_ClearRenderTargetView(ID3D11DeviceContext* self, ID3D11RenderTargetView* v, const FLOAT c[4]) {
  if (capturing() && v) {
    std::string s;
    appendf(s, "{\"op\":\"ClearRTV\",\"color\":[%g,%g,%g,%g],\"view\":", c[0], c[1], c[2], c[3]);
    describe_rtv(s, v);
    s += "}";
    record(self, std::move(s));
  }
  ORIG(decltype(hk_ClearRenderTargetView), Context_ClearRenderTargetView)(self, v, c);
}
void STDMETHODCALLTYPE hk_ClearDepthStencilView(ID3D11DeviceContext* self, ID3D11DepthStencilView* v, UINT f, FLOAT d, UINT8 st) {
  if (capturing() && v) {
    std::string s;
    appendf(s, "{\"op\":\"ClearDSV\",\"flags\":%u,\"depth\":%g,\"stencil\":%u,\"view\":", f, d, st);
    describe_dsv(s, v);
    s += "}";
    record(self, std::move(s));
  }
  ORIG(decltype(hk_ClearDepthStencilView), Context_ClearDepthStencilView)(self, v, f, d, st);
}
void STDMETHODCALLTYPE hk_ClearUAVFloat(ID3D11DeviceContext* self, ID3D11UnorderedAccessView* v, const FLOAT c[4]) {
  if (capturing() && v) {
    std::string s = "{\"op\":\"ClearUAVFloat\",\"view\":";
    describe_uav(s, v);
    s += "}";
    record(self, std::move(s));
  }
  ORIG(decltype(hk_ClearUAVFloat), Context_ClearUnorderedAccessViewFloat)(self, v, c);
}
void STDMETHODCALLTYPE hk_ClearUAVUint(ID3D11DeviceContext* self, ID3D11UnorderedAccessView* v, const UINT c[4]) {
  if (capturing() && v) {
    std::string s = "{\"op\":\"ClearUAVUint\",\"view\":";
    describe_uav(s, v);
    s += "}";
    record(self, std::move(s));
  }
  ORIG(decltype(hk_ClearUAVUint), Context_ClearUnorderedAccessViewUint)(self, v, c);
}
HRESULT STDMETHODCALLTYPE hk_FinishCommandList(ID3D11DeviceContext* self, BOOL restore, ID3D11CommandList** out) {
  HRESULT hr = ORIG(decltype(hk_FinishCommandList), Context_FinishCommandList)(self, restore, out);
  if (SUCCEEDED(hr) && out && *out) {
    std::lock_guard lock(g_cap_mutex);
    auto it = g_deferred.find(self);
    if (it != g_deferred.end()) {
      if (!it->second.empty()) g_cmdlists[*out] = std::move(it->second);
      g_deferred.erase(it);
    }
  }
  return hr;
}
void STDMETHODCALLTYPE hk_ExecuteCommandList(ID3D11DeviceContext* self, ID3D11CommandList* cl, BOOL restore) {
  {
    std::lock_guard lock(g_cap_mutex);
    auto it = g_cmdlists.find(cl);
    if (capturing()) {
      std::string s;
      appendf(s, "{\"op\":\"ExecuteCommandList\",\"cl\":\"%p\",\"n\":%zu}", static_cast<void*>(cl),
              it == g_cmdlists.end() ? (size_t)0 : it->second.size());
      auto& out = stream_for(self);
      out.push_back(std::move(s));
      if (it != g_cmdlists.end())
        for (auto& e : it->second) out.push_back("{\"in_cl\":1," + e.substr(1));
    }
    if (it != g_cmdlists.end()) g_cmdlists.erase(it);
  }
  ORIG(decltype(hk_ExecuteCommandList), Context_ExecuteCommandList)(self, cl, restore);
}

HRESULT STDMETHODCALLTYPE hk_CreateVertexShader(ID3D11Device* self, const void* bc, SIZE_T n, ID3D11ClassLinkage* l, ID3D11VertexShader** out) {
  HRESULT hr = ORIG(decltype(hk_CreateVertexShader), Device_CreateVertexShader)(self, bc, n, l, out);
  if (SUCCEEDED(hr) && out) shaders_register(*out, ShaderKind::VS, bc, n);
  return hr;
}
HRESULT STDMETHODCALLTYPE hk_CreatePixelShader(ID3D11Device* self, const void* bc, SIZE_T n, ID3D11ClassLinkage* l, ID3D11PixelShader** out) {
  HRESULT hr = ORIG(decltype(hk_CreatePixelShader), Device_CreatePixelShader)(self, bc, n, l, out);
  if (SUCCEEDED(hr) && out) shaders_register(*out, ShaderKind::PS, bc, n);
  return hr;
}
HRESULT STDMETHODCALLTYPE hk_CreateComputeShader(ID3D11Device* self, const void* bc, SIZE_T n, ID3D11ClassLinkage* l, ID3D11ComputeShader** out) {
  HRESULT hr = ORIG(decltype(hk_CreateComputeShader), Device_CreateComputeShader)(self, bc, n, l, out);
  if (SUCCEEDED(hr) && out) shaders_register(*out, ShaderKind::CS, bc, n);
  return hr;
}
HRESULT STDMETHODCALLTYPE hk_CreateDeferredContext(ID3D11Device* self, UINT flags, ID3D11DeviceContext** out) {
  HRESULT hr = ORIG(decltype(hk_CreateDeferredContext), Device_CreateDeferredContext)(self, flags, out);
  if (SUCCEEDED(hr) && out && *out) {
    static std::atomic<int> n{0};
    ACO_LOG("CreateDeferredContext #%d -> %p", ++n, static_cast<void*>(*out));
    trace_install_context(*out);
  }
  return hr;
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

void trace_install_context(ID3D11DeviceContext* ctx) {
  hook_vtable(ctx, Context_Draw, reinterpret_cast<void*>(hk_Draw));
  hook_vtable(ctx, Context_DrawIndexed, reinterpret_cast<void*>(hk_DrawIndexed));
  hook_vtable(ctx, Context_DrawInstanced, reinterpret_cast<void*>(hk_DrawInstanced));
  hook_vtable(ctx, Context_DrawIndexedInstanced, reinterpret_cast<void*>(hk_DrawIndexedInstanced));
  hook_vtable(ctx, Context_DrawIndexedInstancedIndirect, reinterpret_cast<void*>(hk_DrawIndexedInstancedIndirect));
  hook_vtable(ctx, Context_DrawInstancedIndirect, reinterpret_cast<void*>(hk_DrawInstancedIndirect));
  hook_vtable(ctx, Context_Dispatch, reinterpret_cast<void*>(hk_Dispatch));
  hook_vtable(ctx, Context_DispatchIndirect, reinterpret_cast<void*>(hk_DispatchIndirect));
  hook_vtable(ctx, Context_CopyResource, reinterpret_cast<void*>(hk_CopyResource));
  hook_vtable(ctx, Context_CopySubresourceRegion, reinterpret_cast<void*>(hk_CopySubresourceRegion));
  hook_vtable(ctx, Context_ClearRenderTargetView, reinterpret_cast<void*>(hk_ClearRenderTargetView));
  hook_vtable(ctx, Context_ClearDepthStencilView, reinterpret_cast<void*>(hk_ClearDepthStencilView));
  hook_vtable(ctx, Context_ClearUnorderedAccessViewFloat, reinterpret_cast<void*>(hk_ClearUAVFloat));
  hook_vtable(ctx, Context_ClearUnorderedAccessViewUint, reinterpret_cast<void*>(hk_ClearUAVUint));
  hook_vtable(ctx, Context_FinishCommandList, reinterpret_cast<void*>(hk_FinishCommandList));
  hook_vtable(ctx, Context_ExecuteCommandList, reinterpret_cast<void*>(hk_ExecuteCommandList));
}

void trace_install_device(ID3D11Device* dev) {
  hook_vtable(dev, Device_CreateVertexShader, reinterpret_cast<void*>(hk_CreateVertexShader));
  hook_vtable(dev, Device_CreatePixelShader, reinterpret_cast<void*>(hk_CreatePixelShader));
  hook_vtable(dev, Device_CreateComputeShader, reinterpret_cast<void*>(hk_CreateComputeShader));
  hook_vtable(dev, Device_CreateDeferredContext, reinterpret_cast<void*>(hk_CreateDeferredContext));
  ID3D11DeviceContext* imm = nullptr;
  dev->GetImmediateContext(&imm);
  g_immediate = imm;
  trace_install_context(imm);
  imm->Release();
}

void trace_on_present(IDXGISwapChain* sc) {
  uint64_t frame = g_frame.fetch_add(1) + 1;
  if (frame == 300 || frame == 3000) {
    ACO_LOG("frame %llu: draws imm=%llu def=%llu dispatch imm=%llu def=%llu", (unsigned long long)frame,
            (unsigned long long)g_draws_imm.load(), (unsigned long long)g_draws_def.load(),
            (unsigned long long)g_dispatch_imm.load(), (unsigned long long)g_dispatch_def.load());
  }
  int left = g_capture_left.load();
  if (left > 0) {
    {
      std::lock_guard lock(g_cap_mutex);
      std::string s;
      appendf(s, "{\"op\":\"Present\",\"frame\":%llu}", (unsigned long long)frame);
      g_events.push_back(std::move(s));
    }
    if (left == 1) finish_capture();
    g_capture_left.store(left - 1);
    return;
  }
  // Poll for the trigger file every 30 frames.
  if (frame % 30 == 0) {
    std::wstring trig = out_dir() + L"\\capture";
    if (GetFileAttributesW(trig.c_str()) != INVALID_FILE_ATTRIBUTES) {
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
      ACO_LOG("capture %d armed at frame %llu", g_cap_index, (unsigned long long)frame);
      g_capture_left.store(kCaptureFrames);
    }
  }
}

} // namespace aco
