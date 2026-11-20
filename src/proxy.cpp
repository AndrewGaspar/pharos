// d3d11.dll proxy. The game loads this from its own directory; we load the real d3d11.dll from
// the system directory (DXVK under Proton, Microsoft's on Windows) and forward to it, installing
// vtable hooks on the created device/contexts and on the DXGI factory/swapchain.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <atomic>
#include <string>

#include "stereo/stereo.h"
#include "core/hook.h"
#include "core/log.h"
#include "core/vtidx.h"
#include "core/config.h"
#include "core/ctxhooks.h"
#include "core/latency.h"
#include "reflex/reflex.h"
#include "core/dump.h"
#include "fg/fg.h"
#include "fg/fg_d3d12.h"
#include "fg/vk_device_ext.h"
#include "core/memtool.h"
#include "core/tlsext.h"
#include "world/npc.h"
#include "world/npc_units.h"
#include "world/teleport.h"
#include "world/world.h"
#include "core/framedump.h"
#include "core/mapstats.h"
#include "core/peek.h"
#include "cloth/cloth.h"
#include "dlss/upscaler.h"
#include "trace/trace.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

using namespace aco;
using namespace aco::vt;

namespace {

HMODULE g_real = nullptr;

HMODULE real_d3d11() {
  if (g_real) return g_real;
  wchar_t path[MAX_PATH];
  UINT n = GetSystemDirectoryW(path, MAX_PATH);
  std::wstring p(path, n);
  p += L"\\d3d11.dll";
  g_real = LoadLibraryW(p.c_str());
  if (g_real == reinterpret_cast<HMODULE>(&__ImageBase)) {
    ACO_LOG("FATAL: system d3d11 resolved to ourselves");
    g_real = nullptr;
  }
  ACO_LOG("real d3d11: %ls -> %p", p.c_str(), static_cast<void*>(g_real));
  if (g_real && config().fg_mode == "on") fg_hook_dxvk_device_creation(nullptr, g_real);
  return g_real;
}

template <typename T>
T real_proc(const char* name) {
  HMODULE m = real_d3d11();
  return m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
}

// ---- DXGI hooks --------------------------------------------------------------------------------

// Time spent inside the real Present (presentation/back-pressure), reported every ~2 s.
double g_present_ms = 0;
int g_present_n = 0;

HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* self, UINT sync, UINT flags) {
  if (!(flags & DXGI_PRESENT_TEST)) {
    stereo_on_present(self);
    trace_on_present(self);
    upscaler_on_present(self);
    latency_on_present_begin(self);
    reflex_on_present_begin(self);
    dump_poll();
    fg_on_present(self);
    memtool_poll();
    tlsext_poll();
    world_on_present();
    npc_on_present();
    npc_units_on_present();
    framedump_on_present(self);
    mapstats_on_present();
    peek_on_present();
    cloth_on_present();
    teleport_on_present();
  }
  LARGE_INTEGER a, b, f;
  QueryPerformanceCounter(&a);
  auto real_present = orig<decltype(hk_Present)>(self, SwapChain_Present);
  HRESULT hr = S_OK;
  if ((flags & DXGI_PRESENT_TEST) || stereo_active() || !fg_present(self, sync, flags, real_present, &hr))
    hr = real_present(self, sync, flags);
  QueryPerformanceCounter(&b);
  if (!(flags & DXGI_PRESENT_TEST)) {
    latency_on_present_end();
    reflex_on_present_end();
  }
  QueryPerformanceFrequency(&f);
  g_present_ms += 1000.0 * double(b.QuadPart - a.QuadPart) / double(f.QuadPart);
  if (++g_present_n == 128) {
    ACO_LOG("perf: Present() avg %.2f ms (sync=%u)", g_present_ms / g_present_n, sync);
    g_present_ms = 0;
    g_present_n = 0;
  }
  return hr;
}
HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
  if (!(flags & DXGI_PRESENT_TEST)) {
    trace_on_present(self);
    upscaler_on_present(self);
    latency_on_present_begin(self);
    reflex_on_present_begin(self);
  }
  HRESULT hr = orig<decltype(hk_Present1)>(self, SwapChain1_Present1)(self, sync, flags, p);
  if (!(flags & DXGI_PRESENT_TEST)) {
    latency_on_present_end();
    reflex_on_present_end();
  }
  return hr;
}
HRESULT STDMETHODCALLTYPE hk_GetBuffer(IDXGISwapChain* self, UINT index, REFIID riid, void** out) {
  return fg12_get_buffer(self, index, riid, out, orig<decltype(hk_GetBuffer)>(self, SwapChain_GetBuffer));
}
HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain* self, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
  if (n && config().fg_mode == "d3d12" && config().fg_buffers > 0 && n < UINT(config().fg_buffers)) n = config().fg_buffers;
  ACO_LOG("ResizeBuffers %ux%u fmt=%d n=%u", w, h, f, n);
  reflex_on_resize();
  fg12_before_resize();
  return orig<decltype(hk_ResizeBuffers)>(self, SwapChain_ResizeBuffers)(self, n, w, h, f, fl);
}

void install_swapchain(IDXGISwapChain* sc) {
  DXGI_SWAP_CHAIN_DESC d;
  sc->GetDesc(&d);
  ACO_LOG("swapchain %p: %ux%u fmt=%d buffers=%u swap=%d windowed=%d", static_cast<void*>(sc), d.BufferDesc.Width,
          d.BufferDesc.Height, d.BufferDesc.Format, d.BufferCount, d.SwapEffect, d.Windowed);
  hook_vtable(sc, SwapChain_Present, reinterpret_cast<void*>(hk_Present));
  hook_vtable(sc, SwapChain_ResizeBuffers, reinterpret_cast<void*>(hk_ResizeBuffers));
  hook_vtable(sc, SwapChain_GetBuffer, reinterpret_cast<void*>(hk_GetBuffer));
  IDXGISwapChain1* sc1 = nullptr;
  if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1)))) {
    hook_vtable(sc1, SwapChain1_Present1, reinterpret_cast<void*>(hk_Present1));
    sc1->Release();
  }
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChain(IDXGIFactory* self, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
  if (desc && config().fg_mode == "d3d12" && config().fg_buffers > 0 && desc->BufferCount < UINT(config().fg_buffers)) {
    ACO_LOG("fg: swapchain buffer count %u -> %d", desc->BufferCount, config().fg_buffers);
    desc->BufferCount = config().fg_buffers;
  }
  HRESULT hr = orig<decltype(hk_CreateSwapChain)>(self, Factory_CreateSwapChain)(self, dev, desc, out);
  if (SUCCEEDED(hr) && out && *out) install_swapchain(*out);
  return hr;
}
HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** out) {
  HRESULT hr = orig<decltype(hk_CreateSwapChainForHwnd)>(self, Factory2_CreateSwapChainForHwnd)(self, dev, hwnd, desc, fs, o, out);
  if (SUCCEEDED(hr) && out && *out) install_swapchain(*out);
  return hr;
}

void install_factory_hooks(ID3D11Device* dev) {
  IDXGIDevice* dxgi = nullptr;
  IDXGIAdapter* adapter = nullptr;
  IDXGIFactory* factory = nullptr;
  if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))) &&
      SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
      SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory)))) {
    hook_vtable(factory, Factory_CreateSwapChain, reinterpret_cast<void*>(hk_CreateSwapChain));
    IDXGIFactory2* f2 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&f2)))) {
      hook_vtable(f2, Factory2_CreateSwapChainForHwnd, reinterpret_cast<void*>(hk_CreateSwapChainForHwnd));
      f2->Release();
    }
    DXGI_ADAPTER_DESC ad;
    adapter->GetDesc(&ad);
    ACO_LOG("adapter: %ls vendor=%04x device=%04x", ad.Description, ad.VendorId, ad.DeviceId);
  } else {
    ACO_LOG("failed to reach DXGI factory from device");
  }
  if (factory) factory->Release();
  if (adapter) adapter->Release();
  if (dxgi) dxgi->Release();
}

std::atomic<bool> g_device_installed{false};

void on_device(ID3D11Device* dev, const char* via) {
  ACO_LOG("%s -> device %p, feature level %x", via, static_cast<void*>(dev), dev->GetFeatureLevel());
  if (g_device_installed.exchange(true)) {
    ACO_LOG("additional device created; hooks are per-class so it is covered");
  }
  ctxhooks_install_device(dev);
  install_factory_hooks(dev);
  reflex_on_device(dev);
}

} // namespace

extern "C" {

HRESULT WINAPI aco_D3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
                                                         const D3D_FEATURE_LEVEL* levels, UINT nlevels, UINT sdk,
                                                         ID3D11Device** dev, D3D_FEATURE_LEVEL* fl, ID3D11DeviceContext** ctx) {
  auto fn = real_proc<PFN_D3D11_CREATE_DEVICE>("D3D11CreateDevice");
  if (!fn) return E_FAIL;
  ACO_LOG("D3D11CreateDevice(type=%d flags=%x)", type, flags);
  HRESULT hr = fn(adapter, type, sw, flags, levels, nlevels, sdk, dev, fl, ctx);
  if (SUCCEEDED(hr) && dev && *dev) on_device(*dev, "D3D11CreateDevice");
  return hr;
}

HRESULT WINAPI aco_D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT nlevels,
    UINT sdk, const DXGI_SWAP_CHAIN_DESC* scd, IDXGISwapChain** sc, ID3D11Device** dev, D3D_FEATURE_LEVEL* fl,
    ID3D11DeviceContext** ctx) {
  auto fn = real_proc<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>("D3D11CreateDeviceAndSwapChain");
  if (!fn) return E_FAIL;
  HRESULT hr = fn(adapter, type, sw, flags, levels, nlevels, sdk, scd, sc, dev, fl, ctx);
  if (SUCCEEDED(hr) && dev && *dev) on_device(*dev, "D3D11CreateDeviceAndSwapChain");
  if (SUCCEEDED(hr) && sc && *sc) install_swapchain(*sc);
  return hr;
}

// Rarely used entry points: forward untouched.
HRESULT WINAPI aco_D3D11CoreCreateDevice(void* a, void* b, UINT c, const D3D_FEATURE_LEVEL* d, UINT e, void** f) {
  using Fn = HRESULT(WINAPI*)(void*, void*, UINT, const D3D_FEATURE_LEVEL*, UINT, void**);
  auto fn = real_proc<Fn>("D3D11CoreCreateDevice");
  return fn ? fn(a, b, c, d, e, f) : E_NOTIMPL;
}
HRESULT WINAPI aco_D3D11On12CreateDevice(IUnknown* a, UINT b, const D3D_FEATURE_LEVEL* c, UINT d, IUnknown* const* e,
                                                             UINT f, UINT g, ID3D11Device** h, ID3D11DeviceContext** i, D3D_FEATURE_LEVEL* j) {
  using Fn = HRESULT(WINAPI*)(IUnknown*, UINT, const D3D_FEATURE_LEVEL*, UINT, IUnknown* const*, UINT, UINT, ID3D11Device**,
                              ID3D11DeviceContext**, D3D_FEATURE_LEVEL*);
  auto fn = real_proc<Fn>("D3D11On12CreateDevice");
  return fn ? fn(a, b, c, d, e, f, g, h, i, j) : E_NOTIMPL;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(inst);
    log_init();
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    ACO_LOG("Pharos loaded into %ls", exe);
    config_reload();
    upscaler_preinit();
    fg_preinit();
  }
  return TRUE;
}

} // extern "C"
