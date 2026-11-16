#pragma once

struct IDXGISwapChain;

namespace aco {

// Debug: when <out>\framedump exists (optional content: frame count, default 60), copies the
// backbuffer of that many consecutive Presents into staging textures, then writes them as
// <out>\frames\NNNN.ppm (+ times.txt with QPC ns per frame) and deletes the trigger.
void framedump_on_present(IDXGISwapChain* sc);

} // namespace aco
