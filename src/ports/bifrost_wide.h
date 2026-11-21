#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace bifrost::wide {
bool requested();
UINT width(HWND window, UINT game_width);
void attach(IDXGISwapChain* chain, HWND window);
UINT resize_width(IDXGISwapChain* chain, UINT width);
Microsoft::WRL::ComPtr<ID3D12Resource> physical(IDXGISwapChain* chain, UINT index);
}  // namespace bifrost::wide
