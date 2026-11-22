#pragma once

struct ID3D11Device;
struct IDXGISwapChain;

namespace aco {

// NVIDIA Reflex for a game that has no Reflex support, through NVAPI (dxvk-nvapi -> DXVK's
// D3D11 low-latency device -> VK_NV_low_latency2).
void reflex_on_device(ID3D11Device* dev);   // the presenting NVIDIA device
void reflex_on_present_begin(IDXGISwapChain* sc);
void reflex_on_present_end();
void reflex_on_resize();  // swapchain resized: pause Reflex until stable again
// Total time the render thread has waited in the frame limiter and the Reflex sleep (us, monotonic):
// the slack the frame-rate cap leaves (Frame Generation's dynamic multiplier uses it).
unsigned long long reflex_wait_us();

} // namespace aco
