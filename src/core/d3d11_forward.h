#pragma once
#include <d3d11.h>

// Implemented by the game-specific frontend.
namespace aco {
HMODULE d3d11_system_module();
void d3d11_device_created(ID3D11Device* device, const char* entry);
bool d3d11_upscale_device(ID3D11Device* device);  // NVIDIA, and the presenting device once selected
void d3d11_swapchain_created(IDXGISwapChain* swapchain);
}  // namespace aco
