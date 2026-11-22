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
//  * TemporalAA draw: record motion vectors and preserve the native TAA resolve and history.
//  * When the split list executes on the immediate context, evaluate the upscaler.
//  * UI composite: submit the draw with a matching successful output, otherwise use native TAA.
void upscaler_init(ID3D11Device* dev, ID3D11DeviceContext* immediate);
void upscaler_on_resize(IDXGISwapChain* sc);  // after a successful resize of the rendering chain
void upscaler_on_present(IDXGISwapChain* sc);
uint64_t upscaler_frame();  // identifier used by TAA jobs for the pending Present

// Called for draws whose pixel shader is one of the classified engine passes. `draw` issues the
// original draw call. Returns true if the draw was handled (the caller must not draw again).
bool upscaler_on_draw(ID3D11DeviceContext* ctx, ShaderClass cls, const std::function<void()>& draw);

// Command list bookkeeping for split deferred lists.
void upscaler_on_finish_command_list(ID3D11DeviceContext* ctx, ID3D11CommandList* cl);
// Returns true if it executed the list itself (with DLSS spliced in).
bool upscaler_on_execute_command_list(ID3D11DeviceContext* ctx, ID3D11CommandList* cl, BOOL restore);

// Negative texture LOD bias for material samplers when rendering below display resolution.
void upscaler_adjust_sampler(D3D11_SAMPLER_DESC* desc);

// Inputs for DLSS Frame Generation, from the last DLSS job executed on the immediate context
// (i.e. the frame about to be presented). Pointers are AddRef'd; the caller releases them.
// The engine's camera for the frame (column-vector convention, rows as the shaders use them), read
// from the TAA pass's constant buffers at record time: PassParams.viewToWorld (cb1[4..6]),
// PassParams.RevProjParams.ClipXYZToViewPos (cb1[57..60]), InstanceParams.worldViewProjPrevFrame
// (cb3[12..15]).
struct FgCamera {
  bool valid = false;
  float view_to_world[3][4];
  float clip_to_view[4][4];
  float prev_view_proj[4][4];
};

struct FgInputs {
  ID3D11Texture2D* mv = nullptr;       // RG16F, render res, UV units, prev - cur
  ID3D11Resource* depth = nullptr;     // R32F raw reversed-Z device depth, render res
  ID3D11Texture2D* hudless = nullptr;  // DLSS output (scene without UI, before the composite), display res
  uint32_t render_w = 0, render_h = 0;
  float jitter_x = 0, jitter_y = 0;    // pixels
  uint64_t frame = 0;                  // upscaler frame counter at execution
  FgCamera cam;
};
bool upscaler_fg_inputs(FgInputs* out);
// Engine depth (AddRef'd) before Present advances the frame, including native TAA.
ID3D11Texture2D* upscaler_cursor_depth(uint32_t& render_w, uint32_t& render_h);

// Called from DllMain: applies render_scale from pharos.ini to the game's ACO.ini.
void upscaler_preinit();

} // namespace aco
