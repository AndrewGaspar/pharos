#pragma once
#include <d3d11.h>

#include <functional>

#include "core/shaders.h"

struct IDXGISwapChain;

namespace aco {

// Replaces the engine's TAA resolve with DLSS Super Resolution.
//
// Frame flow (AnvilNext 2.0, Origins): ... tonemap (LDR RGBA8, render res) -> TemporalAA resolve
// (render res) -> UI composite (display res, samples the TAA output with UVs).
//
//  * TemporalAA draw (recorded on a deferred context): skipped. In its place we record our motion
//    vector kernel (using the TAA's own constant buffers) and a copy of the input colour into the
//    TAA output, then split the deferred command list at that point.
//  * When the split list is executed on the immediate context, DLSS runs between the two halves.
//  * UI composite draw: the TAA output SRV is swapped for the display-resolution DLSS output.
void upscaler_init(ID3D11Device* dev, ID3D11DeviceContext* immediate);
void upscaler_on_present(IDXGISwapChain* sc);

// Called for draws whose pixel shader is one of the classified engine passes. `draw` issues the
// original draw call. Returns true if the draw was handled (the caller must not draw again).
bool upscaler_on_draw(ID3D11DeviceContext* ctx, ShaderClass cls, const std::function<void()>& draw);

// Command list bookkeeping for split deferred lists.
void upscaler_on_finish_command_list(ID3D11DeviceContext* ctx, ID3D11CommandList* cl);
// Returns true if it executed the list itself (with DLSS spliced in).
bool upscaler_on_execute_command_list(ID3D11DeviceContext* ctx, ID3D11CommandList* cl, BOOL restore);

// Negative texture LOD bias for material samplers when rendering below display resolution.
void upscaler_adjust_sampler(D3D11_SAMPLER_DESC* desc);

// Called from DllMain: applies render_scale from aco_dlss.ini to the game's ACO.ini.
void upscaler_preinit();

} // namespace aco
