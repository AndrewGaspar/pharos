#include "ports/bifrost_wide.h"
#include <mutex>
#include "core/hook.h"
#include "core/vtidx.h"
#include "ports/bifrost_dual.h"
#include "ports/bifrost_present.h"
#include "ports/bifrost_scene.h"

namespace bifrost::wide {
namespace {
using Microsoft::WRL::ComPtr;
constexpr GUID layout{0x407f95bf, 0x5085, 0x41bf, {0x8c, 0xc1, 0xe7, 0x24, 0x4d, 0xaa, 0x1b, 0x99}};
constexpr GUID surface{0x407f95bf, 0x5085, 0x41bf, {0x8c, 0xc1, 0xe7, 0x24, 0x4d, 0xaa, 0x1b, 0x9a}};
HWND window_for(IDXGISwapChain* chain) {
  HWND window{};
  UINT n = sizeof(window);
  chain->GetPrivateData(layout, &n, &window);
  return window;
}
HRESULT WINAPI get_buffer(IDXGISwapChain* chain, UINT index, REFIID iid, void** object) {
  auto get = aco::orig<decltype(get_buffer)>(chain, aco::vt::SwapChain_GetBuffer);
  if (!window_for(chain) || !object) return get(chain, index, iid, object);
  *object = nullptr;
  ComPtr<ID3D12Resource> output, game;
  auto hr = get(chain, index, IID_PPV_ARGS(&output));
  if (FAILED(hr)) return hr;
  // Keep each logical buffer with exactly one physical flip buffer. Releasing
  // the old backbuffers on resize also releases the game's eye surfaces.
  static std::mutex lock;
  std::lock_guard guard(lock);
  UINT n = sizeof(void*);
  output->GetPrivateData(surface, &n, game.GetAddressOf());
  if (!game) {
    ComPtr<ID3D12Device> dev;
    if (FAILED(hr = output->GetDevice(IID_PPV_ARGS(&dev)))) return hr;
    auto d = output->GetDesc();
    d.Width /= 2;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    hp.CreationNodeMask = hp.VisibleNodeMask = 1;
    dual::Internal internal;
    if (FAILED(hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PRESENT,
                                                 nullptr, IID_PPV_ARGS(&game)))) return hr;
    if (FAILED(hr = output->SetPrivateDataInterface(surface, game.Get()))) return hr;
  }
  return game->QueryInterface(iid, object);
}
thread_local bool describing{};
template<class Chain, class Desc, class Fn>
HRESULT description(Chain* chain, Desc* desc, Fn fn, UINT* w) {
  if (describing || !w || !window_for(chain)) return fn(chain, desc);
  describing = true;
  auto hr = fn(chain, desc);
  describing = false;
  if (SUCCEEDED(hr)) *w /= 2;
  return hr;
}
HRESULT WINAPI get_desc(IDXGISwapChain* chain, DXGI_SWAP_CHAIN_DESC* d) {
  return description(chain, d, aco::orig<decltype(get_desc)>(chain, aco::vt::SwapChain_GetDesc),
                     d ? &d->BufferDesc.Width : nullptr);
}
HRESULT WINAPI get_desc1(IDXGISwapChain1* chain, DXGI_SWAP_CHAIN_DESC1* d) {
  return description(chain, d, aco::orig<decltype(get_desc1)>(chain, aco::vt::SwapChain1_GetDesc1), d ? &d->Width : nullptr);
}
}  // namespace
bool requested() {
  static const bool enabled = dual::enabled() && option_text(L"stereo", L"output", L"auto") == L"sbs";
  return enabled;
}
UINT width(HWND window, UINT game_width) {
  if (!game_width) {
    RECT bounds{};
    if (!GetClientRect(window, &bounds) || bounds.right <= bounds.left) return 0;
    game_width = UINT(bounds.right - bounds.left);
  }
  if (game_width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION / 2) return 0;
  return game_width * 2;
}
UINT resize_width(IDXGISwapChain* chain, UINT w) {
  auto hwnd = window_for(chain);
  return hwnd ? width(hwnd, w) : w;
}
void attach(IDXGISwapChain* chain, HWND window) {
  if (!window || FAILED(chain->SetPrivateData(layout, sizeof(window), &window))) return;
  aco::hook_vtable(chain, aco::vt::SwapChain_GetBuffer, reinterpret_cast<void*>(get_buffer));
  aco::hook_vtable(chain, aco::vt::SwapChain_GetDesc, reinterpret_cast<void*>(get_desc));
  ComPtr<IDXGISwapChain1> sc;
  if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&sc)))) {
    aco::hook_vtable(sc.Get(), aco::vt::SwapChain_GetBuffer, reinterpret_cast<void*>(get_buffer));
    aco::hook_vtable(sc.Get(), aco::vt::SwapChain_GetDesc, reinterpret_cast<void*>(get_desc));
    aco::hook_vtable(sc.Get(), aco::vt::SwapChain1_GetDesc1, reinterpret_cast<void*>(get_desc1));
  }
}
ComPtr<ID3D12Resource> physical(IDXGISwapChain* chain, UINT index) {
  ComPtr<ID3D12Resource> out;
  aco::orig<decltype(get_buffer)>(chain, aco::vt::SwapChain_GetBuffer)(chain, index, IID_PPV_ARGS(&out));
  return out;
}
}  // namespace bifrost::wide
