#include "core/d3d11_forward.h"

#include "core/log.h"

using namespace aco;

template <typename Fn>
static Fn d3d11_proc(const char* name) {
  HMODULE mod = d3d11_system_module();
  return mod ? reinterpret_cast<Fn>(GetProcAddress(mod, name)) : nullptr;
}

extern "C" {
HRESULT WINAPI aco_D3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
                                     const D3D_FEATURE_LEVEL* levels, UINT nlevels, UINT sdk, ID3D11Device** dev,
                                     D3D_FEATURE_LEVEL* fl, ID3D11DeviceContext** ctx) {
  auto fn = d3d11_proc<PFN_D3D11_CREATE_DEVICE>("D3D11CreateDevice");
  if (!fn) return E_FAIL;
  ACO_LOG("D3D11CreateDevice(type=%d flags=%x)", type, flags);
  HRESULT hr = fn(adapter, type, sw, flags, levels, nlevels, sdk, dev, fl, ctx);
  if (SUCCEEDED(hr) && dev && *dev) d3d11_device_created(*dev, "D3D11CreateDevice");
  return hr;
}

HRESULT WINAPI aco_D3D11CreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
                                                 const D3D_FEATURE_LEVEL* levels, UINT nlevels, UINT sdk,
                                                 const DXGI_SWAP_CHAIN_DESC* scd, IDXGISwapChain** sc,
                                                 ID3D11Device** dev, D3D_FEATURE_LEVEL* fl, ID3D11DeviceContext** ctx) {
  auto fn = d3d11_proc<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>("D3D11CreateDeviceAndSwapChain");
  if (!fn) return E_FAIL;
  HRESULT hr = fn(adapter, type, sw, flags, levels, nlevels, sdk, scd, sc, dev, fl, ctx);
  if (SUCCEEDED(hr) && dev && *dev) d3d11_device_created(*dev, "D3D11CreateDeviceAndSwapChain");
  if (SUCCEEDED(hr) && sc && *sc) d3d11_swapchain_created(*sc);
  return hr;
}

// Rarely used entry points: forward untouched.
HRESULT WINAPI aco_D3D11CoreCreateDevice(void* a, void* b, UINT c, const D3D_FEATURE_LEVEL* d, UINT e, void** f) {
  using Fn = HRESULT(WINAPI*)(void*, void*, UINT, const D3D_FEATURE_LEVEL*, UINT, void**);
  auto fn = d3d11_proc<Fn>("D3D11CoreCreateDevice");
  return fn ? fn(a, b, c, d, e, f) : E_NOTIMPL;
}
HRESULT WINAPI aco_D3D11On12CreateDevice(IUnknown* a, UINT b, const D3D_FEATURE_LEVEL* c, UINT d, IUnknown* const* e,
                                         UINT f, UINT g, ID3D11Device** h, ID3D11DeviceContext** i,
                                         D3D_FEATURE_LEVEL* j) {
  using Fn = HRESULT(WINAPI*)(IUnknown*, UINT, const D3D_FEATURE_LEVEL*, UINT, IUnknown* const*, UINT, UINT,
                              ID3D11Device**, ID3D11DeviceContext**, D3D_FEATURE_LEVEL*);
  auto fn = d3d11_proc<Fn>("D3D11On12CreateDevice");
  return fn ? fn(a, b, c, d, e, f, g, h, i, j) : E_NOTIMPL;
}
}
