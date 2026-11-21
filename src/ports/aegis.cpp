#include <wrl/client.h>

#include <sstream>
#include <type_traits>

#include "core/d3d11_forward.h"
#include "core/hook.h"
#include "core/vtidx.h"
#include "dlss/ngx.h"
#include "ports/aegis_sr.h"
#include "ports/common.h"

using namespace aco;
using namespace aco::vt;
namespace {
using Microsoft::WRL::ComPtr;
struct Pending {
  ID3D11DeviceContext* ctx{};
  port::Entry* entry{};
};
thread_local Pending pixel, compute;
std::atomic<bool> ngx_checked{false}, ngx_ready{false};

void describe(std::ostringstream& msg, const char* kind, UINT slot, ID3D11View* view) {
  if (!view) return;
  ComPtr<ID3D11Resource> res;
  view->GetResource(&res);
  ComPtr<ID3D11Texture2D> texture;
  if (FAILED(res.As(&texture))) return;
  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);
  msg << " " << kind << slot << "=" << desc.Width << "x" << desc.Height << "/fmt" << int(desc.Format);
}

void inspect(ID3D11DeviceContext* ctx, bool cs) {
  Pending& pending = cs ? compute : pixel;
  if (ctx != pending.ctx || !pending.entry) return;
  auto* entry = pending.entry;
  pending = {};
  ComPtr<ID3D11DeviceChild> shader;
  if (cs) {
    ComPtr<ID3D11ComputeShader> current;
    ctx->CSGetShader(&current, nullptr, nullptr);
    shader = current;
  } else {
    ComPtr<ID3D11PixelShader> current;
    ctx->PSGetShader(&current, nullptr, nullptr);
    shader = current;
  }
  if (port::binding(shader.Get()) != entry) return;
  if (!cs && ngx_ready.load()) aegis::capture_draw(ctx, *entry);
  if (entry->sampled.exchange(true)) return;
  std::ostringstream msg;
  ID3D11ShaderResourceView* textures[7]{};
  if (cs)
    ctx->CSGetShaderResources(85, 7, textures);
  else
    ctx->PSGetShaderResources(85, 7, textures);
  for (UINT i = 0; i < 7; ++i) {
    describe(msg, "t", 85 + i, textures[i]);
    if (textures[i]) textures[i]->Release();
  }
  if (cs) {
    ID3D11UnorderedAccessView* outputs[8]{};
    ctx->CSGetUnorderedAccessViews(0, 8, outputs);
    for (UINT i = 0; i < 8; ++i) {
      describe(msg, "u", i, outputs[i]);
      if (outputs[i]) outputs[i]->Release();
    }
  } else {
    ID3D11RenderTargetView* outputs[8]{};
    ctx->OMGetRenderTargets(8, outputs, nullptr);
    for (UINT i = 0; i < 8; ++i) {
      describe(msg, "rt", i, outputs[i]);
      if (outputs[i]) outputs[i]->Release();
    }
  }
  ACO_LOG("submitted %s%s", entry->role.c_str(), msg.str().c_str());
}

template <const size_t& slot, bool cs, typename... Args>
void WINAPI submit(ID3D11DeviceContext* ctx, Args... args) {
  inspect(ctx, cs);
  orig<decltype(submit<slot, cs, Args...>)>(ctx, slot)(ctx, args...);
}

template <typename Shader, const size_t& slot>
HRESULT WINAPI create_shader(ID3D11Device* device, const void* bytes, SIZE_T size, ID3D11ClassLinkage* linkage,
                             Shader** shader) {
  auto hr = orig<decltype(create_shader<Shader, slot>)>(device, slot)(device, bytes, size, linkage, shader);
  if (SUCCEEDED(hr) && shader && *shader) port::tag(*shader, port::lookup(bytes, size));
  return hr;
}

template <typename Shader, const size_t& slot>
void WINAPI set_shader(ID3D11DeviceContext* ctx, Shader* shader, ID3D11ClassInstance* const* ci, UINT count) {
  orig<decltype(set_shader<Shader, slot>)>(ctx, slot)(ctx, shader, ci, count);
  auto* entry = port::binding(shader);
  auto& pending = std::is_same_v<Shader, ID3D11ComputeShader> ? compute : pixel;
  pending = entry && (!entry->sampled.load(std::memory_order_relaxed) || aegis::capturing()) ? Pending{ctx, entry}
                                                                                             : Pending{};
  if (!entry || entry->bound.exchange(true, std::memory_order_relaxed)) return;
  port::bound(*entry);
  if (!ngx_checked.exchange(true)) {
    ID3D11Device* device = nullptr;
    ctx->GetDevice(&device);
    ngx_ready.store(ngx::init(device, out_dir()) && ngx::dlss_available());
    device->Release();
  }
}

HRESULT WINAPI finish(ID3D11DeviceContext* ctx, BOOL restore, ID3D11CommandList** out) {
  auto hr = orig<decltype(finish)>(ctx, Context_FinishCommandList)(ctx, restore, out);
  if (SUCCEEDED(hr) && out && *out) aegis::finished_list(ctx, *out);
  return hr;
}

void WINAPI execute(ID3D11DeviceContext* ctx, ID3D11CommandList* list, BOOL restore) {
  orig<decltype(execute)>(ctx, Context_ExecuteCommandList)(ctx, list, restore);
  aegis::after_execute(ctx, list);
}
void observe_context(ID3D11DeviceContext* ctx) {
  hook_vtable(ctx, Context_PSSetShader, reinterpret_cast<void*>(set_shader<ID3D11PixelShader, Context_PSSetShader>));
  hook_vtable(ctx, Context_CSSetShader, reinterpret_cast<void*>(set_shader<ID3D11ComputeShader, Context_CSSetShader>));
  hook_vtable(ctx, Context_Draw, reinterpret_cast<void*>(submit<Context_Draw, false, UINT, UINT>));
  hook_vtable(ctx, Context_DrawIndexed, reinterpret_cast<void*>(submit<Context_DrawIndexed, false, UINT, UINT, INT>));
  hook_vtable(ctx, Context_DrawInstanced,
              reinterpret_cast<void*>(submit<Context_DrawInstanced, false, UINT, UINT, UINT, UINT>));
  hook_vtable(ctx, Context_DrawIndexedInstanced,
              reinterpret_cast<void*>(submit<Context_DrawIndexedInstanced, false, UINT, UINT, UINT, INT, UINT>));
  hook_vtable(ctx, Context_DrawIndexedInstancedIndirect,
              reinterpret_cast<void*>(submit<Context_DrawIndexedInstancedIndirect, false, ID3D11Buffer*, UINT>));
  hook_vtable(ctx, Context_DrawInstancedIndirect,
              reinterpret_cast<void*>(submit<Context_DrawInstancedIndirect, false, ID3D11Buffer*, UINT>));
  hook_vtable(ctx, Context_Dispatch, reinterpret_cast<void*>(submit<Context_Dispatch, true, UINT, UINT, UINT>));
  hook_vtable(ctx, Context_DispatchIndirect,
              reinterpret_cast<void*>(submit<Context_DispatchIndirect, true, ID3D11Buffer*, UINT>));
}

HRESULT WINAPI deferred(ID3D11Device* dev, UINT flags, ID3D11DeviceContext** out) {
  auto hr = orig<decltype(deferred)>(dev, Device_CreateDeferredContext)(dev, flags, out);
  if (SUCCEEDED(hr) && out && *out) {
    observe_context(*out);
    hook_vtable(*out, Context_FinishCommandList, reinterpret_cast<void*>(finish));
  }
  return hr;
}
}  // namespace

namespace aco {
HMODULE d3d11_system_module() {
  static HMODULE mod = port::system_library(L"d3d11.dll");
  return mod;
}
void d3d11_device_created(ID3D11Device* dev, const char* via) {
  if (!port::active()) return;
  log_init(L"aegis.log");
  ACO_LOG("Aegis: %s, D3D11 feature level 0x%x", via, dev->GetFeatureLevel());
  hook_vtable(dev, Device_CreatePixelShader,
              reinterpret_cast<void*>(create_shader<ID3D11PixelShader, Device_CreatePixelShader>));
  hook_vtable(dev, Device_CreateComputeShader,
              reinterpret_cast<void*>(create_shader<ID3D11ComputeShader, Device_CreateComputeShader>));
  hook_vtable(dev, Device_CreateDeferredContext, reinterpret_cast<void*>(deferred));
  ID3D11DeviceContext* ctx = nullptr;
  dev->GetImmediateContext(&ctx);
  observe_context(ctx);
  hook_vtable(ctx, Context_ExecuteCommandList, reinterpret_cast<void*>(execute));
  ctx->Release();
}
void d3d11_swapchain_created(IDXGISwapChain*) {}
}  // namespace aco
