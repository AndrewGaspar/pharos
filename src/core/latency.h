#pragma once

struct ID3D11DeviceContext;
struct IDXGISwapChain;

namespace aco {

// Input-to-frame latency instrumentation (enabled with latency_log=1 in pharos.ini).
//
// Writes two CSVs to the out dir, all times in ns of the QPC clock (under Wine this is
// CLOCK_MONOTONIC_RAW, so external input injectors on the Linux side can share the time base):
//
//   latency_taa.csv      one row per TAA pass recorded: CPU time of recording, GPU time at which
//                        the pass executed, and the camera rotation/position the frame used
//                        (PassParams.viewToWorld). A camera change marks the frame reacting to input.
//   latency_present.csv  one row per Present: CPU time of the call/return and the GPU time at
//                        which all work submitted before it had finished.
//
// GPU timestamps are mapped to the CPU clock by calibrating against an idle GPU when logging
// starts (and again when it stops, to report drift).
void latency_on_taa(ID3D11DeviceContext* ctx);
void latency_on_present_begin(IDXGISwapChain* sc);
void latency_on_present_end();

} // namespace aco
