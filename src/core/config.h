#pragma once
#include <string>
#include <utility>
#include <vector>

namespace aco {

// aco_dlss.ini, next to the DLL. Re-read periodically so most settings can be tuned live.
//
// [upscaler]
// enabled=1             ; 0 = leave the game's TAA alone
// backend=dlss          ; dlss | bilinear (no temporal AA; for A/B and debugging)
// preset=M              ; DLSS render preset letter (J,K,L,M), or "default"
// debug=none            ; none | motion | depth  (replaces the output with a visualisation)
// jitter_x=1            ; multipliers applied to the engine's SubPixelJitter before DLSS
// jitter_y=1
// mv_x=1                ; multipliers applied to motion vectors (sign conventions)
// mv_y=1
// render_scale=0        ; >0: written to ACO.ini PixelDensity at startup (needs a game restart)
// mip_bias=auto         ; auto = log2(render/display) + mip_bias_offset, or a fixed number
// mip_bias_offset=0
// latency_log=0        ; 1 = write latency_taa.csv / latency_present.csv (see core/latency.h)
//
// [reflex]
// mode=on               ; off | on | boost (on + keep GPU clocks high)
// fps_limit=0           ; >0: Reflex frame rate cap (e.g. 160 just below a 165 Hz refresh)
struct Config {
  bool enabled = true;
  std::string backend = "dlss";
  std::string preset = "M";
  std::string debug = "none";
  float jitter_x = 1.0f, jitter_y = 1.0f;
  float mv_x = 1.0f, mv_y = 1.0f;
  float render_scale = 0.0f;
  std::string mip_bias = "auto";
  float mip_bias_offset = 0.0f;
  bool latency_log = false;
  std::string fg_mode = "off";  // [fg] mode: off | on (Vulkan route, broken) | d3d12 (DLSS Frame Generation)
  float fg_mv_scale = 1.0f;     // [fg] mv_scale: motion vector normalisation (UV units -> 1)
  std::string fg_show = "both";  // [fg] show: both | generated | real (debug: present only one kind)
  bool fg_pacing = true;
  bool fg_debug_fill = false;
  int fg_ngx_log = 0;
  bool fg_bb_format_dxgi = false;  // [fg] bb_format_dxgi
  std::string fg_input = "swapchain";  // [fg] input: swapchain | copy
  bool fg_swap_outputs = false;         // [fg] swap_outputs (debug)
  bool fg_hudless = true;
  float fg_refresh_hz = 165.0f;
  int fg_count = 1;       // [fg] count: DLSS-G MultiFrameCount (generated frames evaluated per real frame)
  int fg_show_index = 1;
  bool fg_raw_output = false;  // [fg] raw_output (debug)
  bool fg_trace_cu = false;
  int fg_present_thread = 1;  // [fg] present_thread (mode=d3d12, at swapchain creation): 0 inline, 1 proxy+thread, 2 proxy+inline
  bool fg_enabled = true;          // [fg] enabled: initial state of the F9 toggle (mode=d3d12)
  std::string fg_toggle_key = "F9";  // [fg] toggle_key: F1..F24 or none (Frame Generation on/off live)
  std::string fg_skip;  // [fg] skip (bisecting): eval | d3d12 | present2
  int fg_frame_latency = 0;  // [fg] frame_latency: DXGI maximum frame latency while FG runs (0 = the game's)
  int fg_buffers = 0;        // [fg] buffers: minimum swapchain buffer count (mode=d3d12, at creation/resize)
  std::string fg_sync = "cpu";  // [fg] sync: how D3D11 waits for DLSS-G output: cpu | gpu
  bool fg_present_gate = true;  // [fg] present_gate: render thread yields the context to the present thread
  std::string fg_camera = "engine";  // [fg] camera: engine (the game's matrices) | identity
  bool fg_camera_unjitter = true;     // [fg] camera_unjitter: strip TAA jitter from the projection
  std::string fg_mv_units = "px";    // [fg] mv_units: px (MvecScale = mv_scale x render size) | uv
  std::string fg_pace = "smooth";  // [fg] pace: ready | smooth | arrival (when the generated frame is presented)
  float fg_pace_relax = 0.5f;     // [fg] pace_relax (pace=smooth)
  int fg_trace_presents = 1200;  // [fg] trace_presents: presents timed per `touch <out>/fgtrace-start`
  int fg_trace_shots = 120;      // [fg] trace_shots: of those, how many get an image band captured
  bool fg_verify = false;  // [fg] verify: log/dump (previous real, generated, current real) every 2 s
  bool fg12_descriptor_heap = true;  // [fg] descriptor_heap (mode=d3d12): VKD3D_CONFIG=descriptor_heap for our device
  bool fg_hide_handle64 = false;  // [fg] hide_handle64 (experiment): evaluation then fails, DLSS-G needs them    // [fg] trace_cu (debug): log NGX CUDA-kernel creation/launches  // [fg] show_index: which generated index to present          // [fg] refresh_hz (DLSSG.RefreshRate)               // [fg] hudless: pass the DLSS output as HUD-less colour
  std::vector<std::pair<std::string, unsigned int>> fg_create_extra;  // [fg] create_extra=DLSSG.Name:value,...
  std::vector<std::pair<std::string, unsigned int>> fg_extra;  // [fg] extra=DLSSG.Name:value,...           // [fg] ngx_log: 1 = NGX log lines into aco_dlss.log, 2 = verbose   // [fg] debug_fill: pre-fill the output with magenta        // [fg] pacing: present the real frame half a frame after the generated one
  std::string toggle_key = "F10";  // [upscaler] toggle_key: F1..F24 or none (DLSS <-> game TAA)
  std::string reflex = "on";     // [reflex] mode
  float reflex_fps_limit = 0.0f;  // [reflex] fps_limit
  bool reflex_markers = true;     // [reflex] markers
  std::string reflex_marker_set = "minimal";  // [reflex] marker_set: minimal | full (experiments)
  bool reflex_optimize = false;   // [reflex] optimize: bUseMarkersToOptimize
  bool reflex_probe = false;
  float world_npc_density = 1.0f;         // [world] npc_density
  float world_npc_distance = 1.0f;        // [world] npc_distance
  float world_npc_units = 1.0f;           // [world] npc_units (experiment)
  float world_npc_units_log = 0.0f;       // [world] npc_units_log (diagnostics)
  float world_npc_units_kill = 0.0f;      // [world] npc_units_kill (test: raise by one to kill an extra unit)
  float world_lod_distance = 1.0f;        // [world] lod_distance
  std::string world_lod_mode = "far";      // [world] lod_mode: far (patched culling shaders, LOD range max only) | all (old: scale every range)
  std::string world_lod_skip;             // [world] lod_skip (diagnostics): culling shader hash prefixes left unpatched
  std::string cloth_mode = "game";  // [cloth] mode: game | render | scale (see cloth/cloth.h)
  float cloth_scale = 1.0f;         // [cloth] scale (mode=scale, diagnostics)
};

const Config& config();
// Re-reads the file; returns true if anything changed.
bool config_reload();
std::wstring module_dir();
// Runtime toggle (hotkey) between DLSS and the game's TAA; reverts when the file changes.
void config_toggle_upscaler();
int config_toggle_vk();  // virtual-key code of [upscaler] toggle_key, 0 = none
int config_key_vk(const std::string& key);  // "F1".."F24" -> virtual-key code, else 0

} // namespace aco
