#include "core/ctxhooks.h"

#include <windows.h>
#include <d3d11_1.h>

#include <atomic>
#include <cstdio>
#include <mutex>

#include "core/cbshadow.h"
#include "core/ctxgate.h"
#include "core/hook.h"
#include "core/log.h"
#include "core/memtool.h"
#include "world/world.h"
#include "core/shaders.h"
#include "core/vtidx.h"
#include "dlss/upscaler.h"
#include "stereo/camera.h"
#include "stereo/cursor.h"
#include "stereo/record.h"
#include "stereo/temporal.h"
#include "stereo/stereo.h"
#include "trace/trace.h"

namespace aco {

std::atomic<int> g_ctx_gate{0};
thread_local int g_ctx_gate_exempt = 0;
ID3D11DeviceContext* g_ctx_gate_imm = nullptr;

void ctx_gate_wait() {
  LARGE_INTEGER f, c0, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c0);
  const LONGLONG deadline = f.QuadPart / 50;  // 20 ms: never stall the game for long
  while (g_ctx_gate.load(std::memory_order_acquire) > 0) {
    SwitchToThread();
    QueryPerformanceCounter(&c);
    if (c.QuadPart - c0.QuadPart > deadline) return;
  }
}

namespace {

using namespace vt;

ID3D11DeviceContext* g_immediate = nullptr;

// Per-context class of the bound pixel shader. Contexts are few (immediate + ~9 deferred), so a
// small linear table keeps lookups lock-free.
struct CtxState {
  std::atomic<ID3D11DeviceContext*> ctx{nullptr};
  std::atomic<ShaderClass> ps{ShaderClass::None};
  std::atomic<ShaderClass> cs{ShaderClass::None};
  // The bound shader when it is a CamEffect (stereo camera copies), else null.
  std::atomic<ID3D11DeviceChild*> ps_fx{nullptr};
  std::atomic<ID3D11DeviceChild*> cs_fx{nullptr};
};
constexpr size_t kMaxContexts = 64;
CtxState g_ctx[kMaxContexts];
std::mutex g_ctx_mutex;

CtxState* state_for(ID3D11DeviceContext* ctx) {
  for (auto& s : g_ctx) {
    ID3D11DeviceContext* c = s.ctx.load(std::memory_order_acquire);
    if (c == ctx) return &s;
    if (!c) break;
  }
  std::lock_guard lock(g_ctx_mutex);
  for (auto& s : g_ctx) {
    ID3D11DeviceContext* c = s.ctx.load();
    if (c == ctx) return &s;
    if (!c) {
      s.ps.store(ShaderClass::None);
      s.cs.store(ShaderClass::None);
      s.ctx.store(ctx, std::memory_order_release);
      return &s;
    }
  }
  return nullptr;
}

inline ShaderClass ps_class(ID3D11DeviceContext* ctx) {
  CtxState* s = state_for(ctx);
  return s ? s->ps.load(std::memory_order_relaxed) : ShaderClass::None;
}

#define ORIG(T, slot) orig<T>(self, slot)

// ---- context hooks ---------------------------------------------------------------------------

void STDMETHODCALLTYPE hk_PSSetShader(ID3D11DeviceContext* self, ID3D11PixelShader* ps, ID3D11ClassInstance* const* ci, UINT n) {
  if (ACO_REC_WANT(self)) rec_ps_set_shader(ps, ci, n);
  ctx_gate(self);
  if (CtxState* s = state_for(self)) {
    ShaderClass c = shader_class_fast(ps);
    s->ps.store(c, std::memory_order_relaxed);
    s->ps_fx.store(c == ShaderClass::CamEffect ? ps : nullptr, std::memory_order_relaxed);
  }
  ORIG(decltype(hk_PSSetShader), Context_PSSetShader)(self, ps, ci, n);
}

void STDMETHODCALLTYPE hk_CSSetShader(ID3D11DeviceContext* self, ID3D11ComputeShader* cs, ID3D11ClassInstance* const* ci,
                                      UINT n) {
  if (ACO_REC_WANT(self)) rec_cs_set_shader(cs, ci, n);
  ctx_gate(self);
  if (CtxState* s = state_for(self)) {
    ShaderClass c = shader_class_fast(cs);
    s->cs.store(c, std::memory_order_relaxed);
    s->cs_fx.store(c == ShaderClass::CamEffect ? cs : nullptr, std::memory_order_relaxed);
  }
  ORIG(decltype(hk_CSSetShader), Context_CSSetShader)(self, cs, ci, n);
}

// A draw/dispatch with a CamEffect shader bound: its camera copies per eye around the call.
template <typename F>
inline bool cam_effect_call(ID3D11DeviceContext* ctx, bool compute, F&& issue) {
  if (!g_cam_on.load(std::memory_order_relaxed)) return false;
  CtxState* s = state_for(ctx);
  ID3D11DeviceChild* sh = s ? (compute ? s->cs_fx : s->ps_fx).load(std::memory_order_relaxed) : nullptr;
  if (!sh) return false;
  CamFx fx;
  cam_effect_begin(ctx, compute ? kCS : kPS, sh, &fx);
  issue();
  cam_effect_end(ctx, &fx);
  return true;
}

inline bool skinning_cs(ID3D11DeviceContext* ctx) {
  CtxState* s = state_for(ctx);
  return s && s->cs.load(std::memory_order_relaxed) == ShaderClass::Skinning;
}

inline bool culling_cs(ID3D11DeviceContext* ctx) {
  CtxState* s = state_for(ctx);
  return s && s->cs.load(std::memory_order_relaxed) == ShaderClass::Culling;
}

void STDMETHODCALLTYPE hk_ClearState(ID3D11DeviceContext* self) {
  if (ACO_REC_WANT(self)) rec_clear_state();
  ctx_gate(self);
  if (CtxState* s = state_for(self)) {
    s->ps.store(ShaderClass::None, std::memory_order_relaxed);
    s->cs.store(ShaderClass::None, std::memory_order_relaxed);
    s->ps_fx.store(nullptr, std::memory_order_relaxed);
    s->cs_fx.store(nullptr, std::memory_order_relaxed);
  }
  ORIG(decltype(hk_ClearState), Context_ClearState)(self);
}

// skipdraw experiment (trace/skipdraw.txt): drop draws matching a pixel-shader hash prefix.
inline bool skip_this_draw(ID3D11DeviceContext* self, UINT count) {
  if (!skipdraw_active()) return false;
  ID3D11PixelShader* ps = nullptr;
  self->PSGetShader(&ps, nullptr, nullptr);
  bool hit = ps && skipdraw_match(shader_hash(ps), count);
  if (ps) ps->Release();
  return hit;
}

// Common draw path: trace, then let the upscaler intercept classified passes.
template <typename F>
inline void draw_common(ID3D11DeviceContext* self, const char* op, const char* args, F&& issue) {
  if (skipdraw_range_hit(self)) return;  // range-form experiment (counts every draw)
  ctx_gate(self);
  tmp_on_draw(self);
  trace_draw(self, op, args);
  ShaderClass c = ps_class(self);
  if (c == ShaderClass::CamEffect && cam_effect_call(self, false, issue)) return;
  if (c != ShaderClass::None && upscaler_on_draw(self, c, issue)) return;
  issue();
}

void STDMETHODCALLTYPE hk_Draw(ID3D11DeviceContext* self, UINT a, UINT b) {
  if (skip_this_draw(self, a)) return;
  if (ACO_REC_WANT(self)) rec_draw(a, b);
  geo_count(a, false);
  char args[64] = "";
  if (trace_capturing()) snprintf(args, sizeof(args), "%u,%u", a, b);
  draw_common(self, "Draw", args, [&] { ORIG(decltype(hk_Draw), Context_Draw)(self, a, b); });
}
void STDMETHODCALLTYPE hk_DrawIndexed(ID3D11DeviceContext* self, UINT a, UINT b, INT c) {
  if (skip_this_draw(self, a)) return;
  if (ACO_REC_WANT(self)) rec_draw_indexed(a, b, c);
  geo_count(a, false);
  char args[64] = "";
  if (trace_capturing()) snprintf(args, sizeof(args), "%u,%u,%d", a, b, c);
  draw_common(self, "DrawIndexed", args, [&] { ORIG(decltype(hk_DrawIndexed), Context_DrawIndexed)(self, a, b, c); });
}
void STDMETHODCALLTYPE hk_DrawInstanced(ID3D11DeviceContext* self, UINT a, UINT b, UINT c, UINT d) {
  if (skip_this_draw(self, b)) return;
  if (ACO_REC_WANT(self)) rec_draw_instanced(a, b, c, d);
  geo_count(uint64_t(a) * b, false);
  char args[64] = "";
  if (trace_capturing()) snprintf(args, sizeof(args), "%u,%u,%u,%u", a, b, c, d);
  draw_common(self, "DrawInstanced", args,
              [&] { ORIG(decltype(hk_DrawInstanced), Context_DrawInstanced)(self, a, b, c, d); });
}
void STDMETHODCALLTYPE hk_DrawIndexedInstanced(ID3D11DeviceContext* self, UINT a, UINT b, UINT c, INT d, UINT e) {
  if (skip_this_draw(self, b)) return;
  if (ACO_REC_WANT(self)) rec_draw_indexed_instanced(a, b, c, d, e);
  geo_count(uint64_t(a) * b, false);
  char args[80] = "";
  if (trace_capturing()) snprintf(args, sizeof(args), "%u,%u,%u,%d,%u", a, b, c, d, e);
  draw_common(self, "DrawIndexedInstanced", args,
              [&] { ORIG(decltype(hk_DrawIndexedInstanced), Context_DrawIndexedInstanced)(self, a, b, c, d, e); });
}
void STDMETHODCALLTYPE hk_DrawIndexedInstancedIndirect(ID3D11DeviceContext* self, ID3D11Buffer* buf, UINT off) {
  if (skip_this_draw(self, off)) return;
  CursorDraw cursor = cursor_prepare(self, buf, off);
  if (cursor == CursorDraw::Hide) return;
  if (ACO_REC_WANT(self)) rec_draw_indexed_instanced_indirect(buf, off);
  geo_count(0, true);
  char args[64] = "";
  if (trace_capturing()) snprintf(args, sizeof(args), "\"%p\",%u", static_cast<void*>(buf), off);
  draw_common(self, "DrawIndexedInstancedIndirect", args, [&] {
    ORIG(decltype(hk_DrawIndexedInstancedIndirect), Context_DrawIndexedInstancedIndirect)(self, buf, off);
  });
  if (cursor == CursorDraw::Capture) cursor_restore(self);
}
void STDMETHODCALLTYPE hk_DrawInstancedIndirect(ID3D11DeviceContext* self, ID3D11Buffer* buf, UINT off) {
  if (skip_this_draw(self, off)) return;
  if (ACO_REC_WANT(self)) rec_draw_instanced_indirect(buf, off);
  char args[64] = "";
  if (trace_capturing()) snprintf(args, sizeof(args), "\"%p\",%u", static_cast<void*>(buf), off);
  draw_common(self, "DrawInstancedIndirect", args,
              [&] { ORIG(decltype(hk_DrawInstancedIndirect), Context_DrawInstancedIndirect)(self, buf, off); });
}
void STDMETHODCALLTYPE hk_Dispatch(ID3D11DeviceContext* self, UINT x, UINT y, UINT z) {
  if (ACO_REC_WANT(self)) rec_dispatch(x, y, z);
  tmp_on_dispatch(self);
  ctx_gate(self);
  if (trace_capturing()) {
    char args[64];
    snprintf(args, sizeof(args), "%u,%u,%u", x, y, z);
    trace_dispatch(self, "Dispatch", args);
  } else {
    trace_dispatch(self, "Dispatch", "");
  }
  if (skinning_cs(self)) skin_count(uint64_t(x) * y * z);
  if (culling_cs(self)) {
    world_on_culling_dispatch(self, [&] { ORIG(decltype(hk_Dispatch), Context_Dispatch)(self, x, y, z); });
    return;
  }
  if (cam_effect_call(self, true, [&] { ORIG(decltype(hk_Dispatch), Context_Dispatch)(self, x, y, z); })) return;
  ORIG(decltype(hk_Dispatch), Context_Dispatch)(self, x, y, z);
}
void STDMETHODCALLTYPE hk_DispatchIndirect(ID3D11DeviceContext* self, ID3D11Buffer* buf, UINT off) {
  if (ACO_REC_WANT(self)) rec_dispatch_indirect(buf, off);
  tmp_on_dispatch(self);
  ctx_gate(self);
  char args[64] = "";
  if (trace_capturing()) snprintf(args, sizeof(args), "\"%p\",%u", static_cast<void*>(buf), off);
  trace_dispatch(self, "DispatchIndirect", args);
  if (skinning_cs(self)) skin_count(0);
  if (culling_cs(self)) {
    world_on_culling_dispatch(self, [&] { ORIG(decltype(hk_DispatchIndirect), Context_DispatchIndirect)(self, buf, off); });
    return;
  }
  if (cam_effect_call(self, true, [&] { ORIG(decltype(hk_DispatchIndirect), Context_DispatchIndirect)(self, buf, off); }))
    return;
  ORIG(decltype(hk_DispatchIndirect), Context_DispatchIndirect)(self, buf, off);
}
void STDMETHODCALLTYPE hk_CopyResource(ID3D11DeviceContext* self, ID3D11Resource* dst, ID3D11Resource* src) {
  if (ACO_REC_WANT(self)) rec_copy_resource(dst, src);
  tmp_on_copy(self, dst, src);
  ctx_gate(self);
  trace_copy_resource(self, dst, src);
  ORIG(decltype(hk_CopyResource), Context_CopyResource)(self, dst, src);
}
void STDMETHODCALLTYPE hk_CopySubresourceRegion(ID3D11DeviceContext* self, ID3D11Resource* dst, UINT dsub, UINT x, UINT y,
                                                UINT z, ID3D11Resource* src, UINT ssub, const D3D11_BOX* box) {
  if (ACO_REC_WANT(self)) rec_copy_subresource(dst, dsub, x, y, z, src, ssub, box);
  tmp_on_copy(self, dst, src);
  ctx_gate(self);
  trace_copy_subresource(self, dst, dsub, x, y, z, src, ssub, box);
  ORIG(decltype(hk_CopySubresourceRegion), Context_CopySubresourceRegion)(self, dst, dsub, x, y, z, src, ssub, box);
}
void STDMETHODCALLTYPE hk_ClearRenderTargetView(ID3D11DeviceContext* self, ID3D11RenderTargetView* v, const FLOAT c[4]) {
  if (ACO_REC_WANT(self)) rec_clear_rtv(v, c);
  tmp_on_clear(self, v);
  ctx_gate(self);
  trace_clear_rtv(self, v, c);
  ORIG(decltype(hk_ClearRenderTargetView), Context_ClearRenderTargetView)(self, v, c);
}
void STDMETHODCALLTYPE hk_ClearDepthStencilView(ID3D11DeviceContext* self, ID3D11DepthStencilView* v, UINT f, FLOAT d,
                                                UINT8 st) {
  if (ACO_REC_WANT(self)) rec_clear_dsv(v, f, d, st);
  tmp_on_clear(self, v);
  ctx_gate(self);
  trace_clear_dsv(self, v, f, d, st);
  ORIG(decltype(hk_ClearDepthStencilView), Context_ClearDepthStencilView)(self, v, f, d, st);
}
void STDMETHODCALLTYPE hk_ClearUAVFloat(ID3D11DeviceContext* self, ID3D11UnorderedAccessView* v, const FLOAT c[4]) {
  if (ACO_REC_WANT(self)) rec_clear_uav_float(v, c);
  tmp_on_clear(self, v);
  ctx_gate(self);
  trace_clear_uav(self, v, true);
  ORIG(decltype(hk_ClearUAVFloat), Context_ClearUnorderedAccessViewFloat)(self, v, c);
}
void STDMETHODCALLTYPE hk_ClearUAVUint(ID3D11DeviceContext* self, ID3D11UnorderedAccessView* v, const UINT c[4]) {
  if (ACO_REC_WANT(self)) rec_clear_uav_uint(v, c);
  tmp_on_clear(self, v);
  ctx_gate(self);
  trace_clear_uav(self, v, false);
  ORIG(decltype(hk_ClearUAVUint), Context_ClearUnorderedAccessViewUint)(self, v, c);
}
HRESULT STDMETHODCALLTYPE hk_FinishCommandList(ID3D11DeviceContext* self, BOOL restore, ID3D11CommandList** out) {
  HRESULT hr = ORIG(decltype(hk_FinishCommandList), Context_FinishCommandList)(self, restore, out);
  skipdraw_reset_ctx(self);
  if (!restore)
    if (CtxState* s = state_for(self)) {
      s->ps.store(ShaderClass::None, std::memory_order_relaxed);
      s->cs.store(ShaderClass::None, std::memory_order_relaxed);
      s->ps_fx.store(nullptr, std::memory_order_relaxed);
      s->cs_fx.store(nullptr, std::memory_order_relaxed);
    }
  if (SUCCEEDED(hr) && out && *out) {
    cam_finish_command_list(self, *out);
    tmp_on_finish_command_list(self, *out);
    trace_finish_command_list(self, *out);
    upscaler_on_finish_command_list(self, *out);
  }
  return hr;
}
void STDMETHODCALLTYPE hk_ExecuteCommandList(ID3D11DeviceContext* self, ID3D11CommandList* cl, BOOL restore) {
  if (ACO_REC_WANT(self)) rec_execute(cl, restore);
  if (g_cam_on.load(std::memory_order_relaxed)) cam_before_execute(self, cl, stereo_eye());
  tmp_on_execute(self, cl);
  ctx_gate(self);
  trace_execute_command_list(self, cl);
  if (!upscaler_on_execute_command_list(self, cl, restore))
    ORIG(decltype(hk_ExecuteCommandList), Context_ExecuteCommandList)(self, cl, restore);
  if (!restore)
    if (CtxState* s = state_for(self)) {
      s->ps.store(ShaderClass::None, std::memory_order_relaxed);
      s->cs.store(ShaderClass::None, std::memory_order_relaxed);
      s->ps_fx.store(nullptr, std::memory_order_relaxed);
      s->cs_fx.store(nullptr, std::memory_order_relaxed);
    }
}

// ---- device hooks ----------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE hk_CreateVertexShader(ID3D11Device* self, const void* bc, SIZE_T n, ID3D11ClassLinkage* l,
                                                ID3D11VertexShader** out) {
  HRESULT hr = ORIG(decltype(hk_CreateVertexShader), Device_CreateVertexShader)(self, bc, n, l, out);
  if (SUCCEEDED(hr) && out) shaders_register(*out, ShaderKind::VS, bc, n);
  return hr;
}
HRESULT STDMETHODCALLTYPE hk_CreatePixelShader(ID3D11Device* self, const void* bc, SIZE_T n, ID3D11ClassLinkage* l,
                                               ID3D11PixelShader** out) {
  HRESULT hr = ORIG(decltype(hk_CreatePixelShader), Device_CreatePixelShader)(self, bc, n, l, out);
  if (SUCCEEDED(hr) && out) shaders_register(*out, ShaderKind::PS, bc, n);
  return hr;
}
HRESULT STDMETHODCALLTYPE hk_CreateComputeShader(ID3D11Device* self, const void* bc, SIZE_T n, ID3D11ClassLinkage* l,
                                                 ID3D11ComputeShader** out) {
  HRESULT hr = ORIG(decltype(hk_CreateComputeShader), Device_CreateComputeShader)(self, bc, n, l, out);
  if (SUCCEEDED(hr) && out) shaders_register(*out, ShaderKind::CS, bc, n);
  return hr;
}
HRESULT STDMETHODCALLTYPE hk_CreateDeferredContext(ID3D11Device* self, UINT flags, ID3D11DeviceContext** out) {
  HRESULT hr = ORIG(decltype(hk_CreateDeferredContext), Device_CreateDeferredContext)(self, flags, out);
  if (SUCCEEDED(hr) && out && *out) {
    static std::atomic<int> n{0};
    ACO_LOG("CreateDeferredContext #%d -> %p", ++n, static_cast<void*>(*out));
    ctxhooks_install_context(*out);
  }
  return hr;
}
HRESULT STDMETHODCALLTYPE hk_CreateSamplerState(ID3D11Device* self, const D3D11_SAMPLER_DESC* desc,
                                                ID3D11SamplerState** out) {
  if (desc) {
    D3D11_SAMPLER_DESC d = *desc;
    upscaler_adjust_sampler(&d);
    return ORIG(decltype(hk_CreateSamplerState), Device_CreateSamplerState)(self, &d, out);
  }
  return ORIG(decltype(hk_CreateSamplerState), Device_CreateSamplerState)(self, desc, out);
}

} // namespace

void ctxhooks_install_context(ID3D11DeviceContext* ctx) {
  cbshadow_install(ctx);
  rec_install_cb_hooks(ctx);
  hook_vtable(ctx, Context_PSSetShader, reinterpret_cast<void*>(hk_PSSetShader));
  hook_vtable(ctx, Context_CSSetShader, reinterpret_cast<void*>(hk_CSSetShader));
  hook_vtable(ctx, Context_ClearState, reinterpret_cast<void*>(hk_ClearState));
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

void ctxhooks_install_device(ID3D11Device* dev) {
  hook_vtable(dev, Device_CreateVertexShader, reinterpret_cast<void*>(hk_CreateVertexShader));
  hook_vtable(dev, Device_CreatePixelShader, reinterpret_cast<void*>(hk_CreatePixelShader));
  hook_vtable(dev, Device_CreateComputeShader, reinterpret_cast<void*>(hk_CreateComputeShader));
  hook_vtable(dev, Device_CreateDeferredContext, reinterpret_cast<void*>(hk_CreateDeferredContext));
  hook_vtable(dev, Device_CreateSamplerState, reinterpret_cast<void*>(hk_CreateSamplerState));
  ID3D11DeviceContext* imm = nullptr;
  dev->GetImmediateContext(&imm);
  g_immediate = imm;
  trace_set_immediate(imm);
  ctxhooks_install_context(imm);
  stereo_init(dev, imm);
  upscaler_init(dev, imm);
  imm->Release();
}

ID3D11DeviceContext* immediate_context() { return g_immediate; }

HRESULT orig_FinishCommandList(ID3D11DeviceContext* ctx, BOOL restore, ID3D11CommandList** out) {
  return orig<decltype(hk_FinishCommandList)>(ctx, Context_FinishCommandList)(ctx, restore, out);
}
void orig_ExecuteCommandList(ID3D11DeviceContext* ctx, ID3D11CommandList* cl, BOOL restore) {
  orig<decltype(hk_ExecuteCommandList)>(ctx, Context_ExecuteCommandList)(ctx, cl, restore);
}

} // namespace aco
