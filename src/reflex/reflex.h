#pragma once

struct ID3D11Device;
struct IDXGISwapChain;

namespace aco {

// NVIDIA Reflex for a game that has no Reflex support, through NVAPI (dxvk-nvapi -> DXVK's
// D3D11 low-latency device -> VK_NV_low_latency2).
void reflex_on_device(ID3D11Device* dev);   // the game's (newest) device
void reflex_on_present_begin(IDXGISwapChain* sc);
void reflex_on_present_end();

} // namespace aco
