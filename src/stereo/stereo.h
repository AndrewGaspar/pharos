#pragma once

#include <windows.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;

namespace aco {

// Stereoscopic 3D ([stereo] mode = sbs): both eyes from the same game frame.
//
// The game's frame is submitted once as usual (left eye) while the immediate-context recorder
// (stereo/record.h) keeps every call; at Present the left image is copied out, the frame is
// submitted again (right eye), and the two are composed side by side. See docs/STEREO.md.
void stereo_init(ID3D11Device* dev, ID3D11DeviceContext* imm);
using PFN_GetBuffer = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, REFIID, void**);
// Before anything else touches the back buffer; real_get_buffer = the swapchain's own GetBuffer.
void stereo_on_present(IDXGISwapChain* sc, PFN_GetBuffer real_get_buffer);
bool stereo_active();                 // side by side is running (Frame Generation stays off)
bool stereo_wide(IDXGISwapChain* sc);  // sc is a 2x-wide (output = sbs) swapchain
int stereo_eye();                     // 0 while the game's own submission runs, 1 during the replay

// Swapchain layout (proxy.cpp's DXGI hooks).
UINT stereo_swapchain_scale();  // width factor for a swapchain being created: 2 for output = sbs
void stereo_swapchain_created(IDXGISwapChain* sc, UINT game_w, UINT game_h);  // after a 2x creation
void stereo_swapchain_resized(IDXGISwapChain* sc, UINT game_w, UINT game_h);
void stereo_adjust_desc(IDXGISwapChain* sc, UINT* width);  // GetDesc(1): the game sees its width
// GetBuffer(0) of a 2x swapchain returns the game's one-eye back buffer; false = not ours.
bool stereo_get_buffer(IDXGISwapChain* sc, UINT index, REFIID riid, void** out, PFN_GetBuffer real);

} // namespace aco
