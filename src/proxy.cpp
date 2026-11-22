#include "core/d3d11_title.h"
// d3d11.dll proxy. The game loads this from its own directory; we load the real d3d11.dll from
// the system directory (DXVK under Proton, Microsoft's on Windows) and forward to it, installing
// vtable hooks on the created device/contexts and on the DXGI factory/swapchain.
#ifdef AEGIS_ODYSSEY
#include "ports/common.h"
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <string>

#include "stereo/stereo.h"
#include "core/quiver.h"
#include "core/hook.h"
#include "core/d3d11_forward.h"
#include "core/log.h"
#include "core/lab.h"
#include "core/vtidx.h"
#include "core/config.h"
#include "core/content_loader.h"
#include "core/ctxhooks.h"
#include "core/latency.h"
#include "reflex/reflex.h"
#include "core/dump.h"
#include "fg/color.h"
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

// ---- DXGI hooks --------------------------------------------------------------------------------

// Time spent inside the real Present (presentation/back-pressure), reported every ~2 s.
double g_present_ms = 0;
int g_present_n = 0;

HRESULT STDMETHODCALLTYPE hk_GetBuffer(IDXGISwapChain* self, UINT index, REFIID riid, void** out);

// A second D3D11CreateDevice can be an adapter probe on another thread, even mid-game.
// Keep installing shader/context hooks at creation, but bind the renderer at a real Present.
using Microsoft::WRL::ComPtr;
struct ChainDevice {
  ID3D11Device* dev{};  // borrowed from the swapchain
  HWND window{};
  UINT vendor{};
};
constexpr GUID kChainDevice = {0xa161dfdf,0x621b,0x4439,{0xad,0xa1,0x60,0xb7,0xc4,0x38,0xbd,0x6f}};
constexpr GUID kDeviceVendor = {0x59397492,0x88f1,0x46e9,{0x8c,0xd7,0x09,0x10,0xd5,0xdb,0xea,0x9f}};
constexpr UINT kNvidia = 0x10de;
std::atomic<ID3D11Device*> g_present_device{};
std::atomic<HWND> g_present_window{};

UINT device_vendor(ID3D11Device* dev) {
  UINT vendor = 0, size = sizeof(vendor);
  if (SUCCEEDED(dev->GetPrivateData(kDeviceVendor, &size, &vendor))) return vendor;
  ComPtr<IDXGIDevice> dxgi;
  ComPtr<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC ad{};
  if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
      SUCCEEDED(adapter->GetDesc(&ad))) {
    vendor = ad.VendorId;
    dev->SetPrivateData(kDeviceVendor, sizeof(vendor), &vendor);
  }
  return vendor;
}

bool game_window(HWND window) {
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  return pid == GetCurrentProcessId() && GetAncestor(window, GA_ROOT) == window &&
         !(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW);
}

ChainDevice chain_device(IDXGISwapChain* sc) {
  ChainDevice info{};
  UINT size = sizeof(info);
  if (SUCCEEDED(sc->GetPrivateData(kChainDevice, &size, &info))) return info;
  DXGI_SWAP_CHAIN_DESC desc{};
  ComPtr<ID3D11Device> dev;
  if (FAILED(sc->GetDesc(&desc)) || FAILED(sc->GetDevice(IID_PPV_ARGS(&dev)))) return {};
  if (game_window(desc.OutputWindow)) {
    info.window = desc.OutputWindow;
    info.dev = dev.Get();
    info.vendor = device_vendor(dev.Get());
  }
  sc->SetPrivateData(kChainDevice, sizeof(info), &info);
  return info;
}

bool render_chain(const ChainDevice& info, bool presenting = false) {
  if (!info.dev) return false;
  auto* active = g_present_device.load(std::memory_order_acquire);
  if (!active && presenting) {
    if (!ctxhooks_engine_renderer(info.dev)) return false;
    static std::once_flag bind;
    std::call_once(bind, [&] {
      ComPtr<ID3D11DeviceContext> imm;
      info.dev->GetImmediateContext(&imm);
      info.dev->AddRef();  // the renderer's device-bound caches last for the game session
      trace_set_immediate(imm.Get());
      stereo_init(info.dev, imm.Get());
      if (info.vendor == kNvidia) {
        upscaler_init(info.dev, imm.Get());
        reflex_on_device(info.dev);
      } else {
        ACO_LOG("upscaler: presenting adapter vendor=%04x, using game TAA", info.vendor);
      }
      g_present_window = info.window;
      g_present_device.store(info.dev, std::memory_order_release);
      ACO_LOG("renderer: bound presenting device %p vendor=%04x window=%p", static_cast<void*>(info.dev),
              info.vendor, static_cast<void*>(info.window));
    });
    active = g_present_device.load(std::memory_order_acquire);
  }
  if (!active) return true;
  if (active != info.dev) return false;
  HWND window = g_present_window.load();
  if (window == info.window) return true;
  // Fullscreen transitions can replace the window while keeping the renderer alive.
  if (IsWindow(window)) return false;
  return !presenting || g_present_window.compare_exchange_strong(window, info.window) || window == info.window;
}

ChainDevice creating_chain(IUnknown* device, HWND window) {
  ComPtr<ID3D11Device> dev;
  if (!device || !game_window(window) || FAILED(device->QueryInterface(IID_PPV_ARGS(&dev))) ||
      !ctxhooks_engine_renderer(dev.Get()))
    return {};
  return {dev.Get(), window, device_vendor(dev.Get())};
}

HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* self, UINT sync, UINT flags) {
  auto real_present = orig<decltype(hk_Present)>(self, SwapChain_Present);
  if (flags & DXGI_PRESENT_TEST) return real_present(self, sync, flags);
  const auto info = chain_device(self);
  if (!render_chain(info, true)) {
    fg12_copy_proxy(self);
    return real_present(self, sync, flags);
  }
  stereo_on_present(self, orig<decltype(hk_GetBuffer)>(self, SwapChain_GetBuffer));
  trace_on_present(self);
  upscaler_on_present(self);
  latency_on_present_begin(self);
  reflex_on_present_begin(self);
  dump_poll();
  fg_on_present(self);
  if constexpr (title::origins) {
    memtool_poll();
    tlsext_poll();
    world_on_present();
    npc_on_present();
    npc_units_on_present();
    framedump_on_present(self);
    peek_on_present();
    cloth_on_present();
    teleport_on_present();
  }
  mapstats_on_present();
  lab_on_present();
  LARGE_INTEGER a, b, f;
  QueryPerformanceCounter(&a);
  HRESULT hr = S_OK;
  if (info.vendor != kNvidia || stereo_active() || stereo_wide(self) || !fg_present(self, sync, flags, real_present, &hr))
    hr = real_present(self, sync, flags);
  QueryPerformanceCounter(&b);
  latency_on_present_end();
  reflex_on_present_end();
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
  auto real = orig<decltype(hk_Present1)>(self, SwapChain1_Present1);
  if (flags & DXGI_PRESENT_TEST) return real(self, sync, flags, p);
  fg12_copy_proxy(self);
  if (!render_chain(chain_device(self), true)) return real(self, sync, flags, p);
  trace_on_present(self);
  upscaler_on_present(self);
  latency_on_present_begin(self);
  reflex_on_present_begin(self);
  HRESULT hr = real(self, sync, flags, p);
  latency_on_present_end();
  reflex_on_present_end();
  return hr;
}
HRESULT STDMETHODCALLTYPE hk_GetBuffer(IDXGISwapChain* self, UINT index, REFIID riid, void** out) {
  auto real = orig<decltype(hk_GetBuffer)>(self, SwapChain_GetBuffer);
  const auto info = chain_device(self);
  bool eligible = render_chain(info);
  if (eligible && stereo_get_buffer(self, index, riid, out, real)) return S_OK;
  return fg12_get_buffer(self, index, riid, out, real, eligible && info.vendor == kNvidia);
}
HRESULT STDMETHODCALLTYPE hk_GetDesc(IDXGISwapChain* self, DXGI_SWAP_CHAIN_DESC* d);
HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain* self, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
  auto real = orig<decltype(hk_ResizeBuffers)>(self, SwapChain_ResizeBuffers);
  auto info = chain_device(self);
  if (!render_chain(info)) {
    HRESULT hr = real(self, n, w, h, f, fl);
    if (SUCCEEDED(hr)) fg12_after_resize(self);
    return hr;
  }
  if (info.vendor == kNvidia && n && config().fg_mode == "d3d12" && config().fg_buffers > 0 &&
      n < UINT(config().fg_buffers)) n = config().fg_buffers;
  ACO_LOG("ResizeBuffers %ux%u fmt=%d n=%u", w, h, f, n);
  reflex_on_resize();
  fg12_before_resize(self);
  if (stereo_wide(self)) {
    UINT gw = w, gh = h;
    if (!gw || !gh) {  // window size
      DXGI_SWAP_CHAIN_DESC d;
      orig<decltype(hk_GetDesc)>(self, SwapChain_GetDesc)(self, &d);
      RECT r{};
      GetClientRect(d.OutputWindow, &r);
      if (!gw) gw = UINT(r.right - r.left);
      if (!gh) gh = UINT(r.bottom - r.top);
    }
    stereo_swapchain_resized(self, gw, gh);
    w = gw * 2;
    h = gh;
  }
  HRESULT hr = real(self, n, w, h, f, fl);
  if (SUCCEEDED(hr)) {
    fg12_after_resize(self);
    if (g_present_device.load(std::memory_order_acquire) == info.dev) upscaler_on_resize(self);
  }
  return hr;
}
HRESULT STDMETHODCALLTYPE hk_GetDesc(IDXGISwapChain* self, DXGI_SWAP_CHAIN_DESC* d) {
  HRESULT hr = orig<decltype(hk_GetDesc)>(self, SwapChain_GetDesc)(self, d);
  if (SUCCEEDED(hr) && d) stereo_adjust_desc(self, &d->BufferDesc.Width);
  return hr;
}
HRESULT STDMETHODCALLTYPE hk_GetDesc1(IDXGISwapChain1* self, DXGI_SWAP_CHAIN_DESC1* d) {
  HRESULT hr = orig<decltype(hk_GetDesc1)>(self, SwapChain1_GetDesc1)(self, d);
  if (SUCCEEDED(hr) && d) stereo_adjust_desc(self, &d->Width);
  return hr;
}

void install_swapchain(IDXGISwapChain* sc) {
  fg_color::watch(sc);
  DXGI_SWAP_CHAIN_DESC d;
  sc->GetDesc(&d);
  ACO_LOG("swapchain %p: %ux%u fmt=%d buffers=%u swap=%d windowed=%d", static_cast<void*>(sc), d.BufferDesc.Width,
          d.BufferDesc.Height, d.BufferDesc.Format, d.BufferCount, d.SwapEffect, d.Windowed);
  hook_vtable(sc, SwapChain_Present, reinterpret_cast<void*>(hk_Present));
  hook_vtable(sc, SwapChain_ResizeBuffers, reinterpret_cast<void*>(hk_ResizeBuffers));
  hook_vtable(sc, SwapChain_GetBuffer, reinterpret_cast<void*>(hk_GetBuffer));
  hook_vtable(sc, SwapChain_GetDesc, reinterpret_cast<void*>(hk_GetDesc));
  IDXGISwapChain1* sc1 = nullptr;
  if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1)))) {
    hook_vtable(sc1, SwapChain1_Present1, reinterpret_cast<void*>(hk_Present1));
    hook_vtable(sc1, SwapChain1_GetDesc1, reinterpret_cast<void*>(hk_GetDesc1));
    sc1->Release();
  }
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChain(IDXGIFactory* self, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
  const auto info = creating_chain(dev, desc ? desc->OutputWindow : nullptr);
  const bool eligible = render_chain(info);
  if (eligible && info.vendor == kNvidia && config().fg_mode == "d3d12" && config().fg_buffers > 0 && desc->BufferCount < UINT(config().fg_buffers)) {
    ACO_LOG("fg: swapchain buffer count %u -> %d", desc->BufferCount, config().fg_buffers);
    desc->BufferCount = config().fg_buffers;
  }
  UINT scale = eligible ? stereo_swapchain_scale(info.window) : 1, gw = 0, gh = 0;
  if (scale > 1) {
    gw = desc->BufferDesc.Width;
    gh = desc->BufferDesc.Height;
    if (!gw || !gh) {
      RECT r{};
      GetClientRect(desc->OutputWindow, &r);
      if (!gw) gw = UINT(r.right - r.left);
      if (!gh) gh = UINT(r.bottom - r.top);
    }
    desc->BufferDesc.Width = gw * scale;
    desc->BufferDesc.Height = gh;
  }
  HRESULT hr = orig<decltype(hk_CreateSwapChain)>(self, Factory_CreateSwapChain)(self, dev, desc, out);
  if (scale > 1) desc->BufferDesc.Width = gw;
  if (SUCCEEDED(hr) && out && *out) {
    install_swapchain(*out);
    if (scale > 1) stereo_swapchain_created(*out, gw, gh);
  }
  return hr;
}
HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** out) {
  UINT scale = desc && render_chain(creating_chain(dev, hwnd)) ? stereo_swapchain_scale(hwnd) : 1, gw = 0, gh = 0;
  DXGI_SWAP_CHAIN_DESC1 d1{};
  if (scale > 1) {
    d1 = *desc;
    gw = d1.Width;
    gh = d1.Height;
    if (!gw || !gh) {
      RECT r{};
      GetClientRect(hwnd, &r);
      if (!gw) gw = UINT(r.right - r.left);
      if (!gh) gh = UINT(r.bottom - r.top);
    }
    d1.Width = gw * scale;
    d1.Height = gh;
    desc = &d1;
  }
  HRESULT hr = orig<decltype(hk_CreateSwapChainForHwnd)>(self, Factory2_CreateSwapChainForHwnd)(self, dev, hwnd, desc, fs, o, out);
  if (SUCCEEDED(hr) && out && *out) {
    install_swapchain(*out);
    if (scale > 1) stereo_swapchain_created(*out, gw, gh);
  }
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
    DXGI_ADAPTER_DESC ad{};
    if (SUCCEEDED(adapter->GetDesc(&ad))) {
      dev->SetPrivateData(kDeviceVendor, sizeof(ad.VendorId), &ad.VendorId);
      ACO_LOG("adapter: %ls vendor=%04x device=%04x", ad.Description, ad.VendorId, ad.DeviceId);
    }
  } else {
    ACO_LOG("failed to reach DXGI factory from device");
  }
  if (factory) factory->Release();
  if (adapter) adapter->Release();
  if (dxgi) dxgi->Release();
}

std::atomic<bool> g_device_installed{false};

void on_device(ID3D11Device* dev, const char* via) {
#ifdef AEGIS_ODYSSEY
  if (!port::active()) return;
#endif
  ACO_LOG("%s -> device %p, feature level %x", via, static_cast<void*>(dev), dev->GetFeatureLevel());
  if (g_device_installed.exchange(true)) {
    ACO_LOG("additional device created; hooks are per-class so it is covered");
  }
  if constexpr (title::origins) install_content_loader_fix();
  ctxhooks_install_device(dev);
  install_factory_hooks(dev);
}

} // namespace

namespace aco {
HMODULE d3d11_system_module() { return real_d3d11(); }
bool d3d11_upscale_device(ID3D11Device* dev) {
  auto* active = g_present_device.load(std::memory_order_acquire);
  return (!active || active == dev) && device_vendor(dev) == kNvidia;
}
void d3d11_device_created(ID3D11Device* dev, const char* via) { on_device(dev, via); }
void d3d11_swapchain_created(IDXGISwapChain* sc) { install_swapchain(sc); }
} // namespace aco

extern "C" {

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(inst);
#ifdef AEGIS_ODYSSEY
    if (!port::active()) return TRUE;
#endif
    log_init(title::log);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    ACO_LOG("%s loaded into %ls", title::name, exe);
    config_reload();
    lab_init();
    config_register_lab();
    if constexpr (title::origins) teleport_register();
    quiver_init();
    upscaler_preinit();
    fg_preinit();
  }
  return TRUE;
}

} // extern "C"
