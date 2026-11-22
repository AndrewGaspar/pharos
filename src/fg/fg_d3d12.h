#pragma once
#include <cstdint>

#include "fg/fg.h"

struct _GUID;

namespace aco {

// [fg] mode=d3d12: DLSS Frame Generation through NGX D3D12 on a vkd3d-proton device of our own,
// fed through textures shared with DXVK. Same contract as fg_present; with the proxy backbuffer
// active it always presents (also when not generating).
bool fg12_present(IDXGISwapChain* sc, UINT sync, UINT flags, PFN_RealPresent real, HRESULT* hr, uint64_t frames);

// IDXGISwapChain::GetBuffer hook: with [fg] mode=d3d12 and present_thread=1 (decided at the first
// call) the game renders into a backbuffer of ours instead of the swapchain's.
using PFN_GetBuffer = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, const _GUID&, void**);
// Frame Generation's proxy back buffer + present thread is in use (decided at the first GetBuffer).
bool fg12_proxy_on(IDXGISwapChain* sc);
void fg12_copy_proxy(IDXGISwapChain* sc);
HRESULT fg12_get_buffer(IDXGISwapChain* sc, UINT index, const _GUID& riid, void** out, PFN_GetBuffer real, bool eligible);
void fg12_before_resize(IDXGISwapChain* sc);
void fg12_after_resize(IDXGISwapChain* sc);
// [fg] auto_cap: the real-frame-rate cap Frame Generation currently wants (0 = none); enforced by the
// [reflex] limiter.
float fg12_auto_cap();
// Frame Generation presented generated frames for the last game frame (mode=d3d12, toggled on, ready).
bool fg12_generating();

} // namespace aco
