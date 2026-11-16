#pragma once
#include <string>

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
};

const Config& config();
// Re-reads the file; returns true if anything changed.
bool config_reload();
std::wstring module_dir();

} // namespace aco
