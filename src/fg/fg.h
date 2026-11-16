#pragma once
#include <windows.h>

struct IDXGISwapChain;

namespace aco {

// DLSS Frame Generation through NGX's Vulkan API on DXVK's own VkDevice (D3D11 has no DLSS-G
// path). See docs/FRAME_GENERATION.md. Config: [fg] mode=off|probe|on.

using PFN_RealPresent = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

void fg_preinit();                       // DllMain: NGX logging environment (before NGX loads)
void fg_on_present(IDXGISwapChain* sc);  // called before Present
// Takes over the game's Present when Frame Generation is active: presents the generated frame,
// then the real one. Returns false if it didn't (caller presents normally).
bool fg_present(IDXGISwapChain* sc, UINT sync, UINT flags, PFN_RealPresent real, HRESULT* hr);

} // namespace aco
