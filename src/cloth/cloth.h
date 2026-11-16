#pragma once

namespace aco {

// Cloth simulation rate (AC Origins v1.62).
//
// The engine steps each MotionCloth with a fixed timestep picked by cloth LOD (code at RVA
// 0x1536a80, called from the UpdateMotionComponentsSim job):
//   step = {1/30, 1/20, 1/15, 1/10}[lod] * timescale * 0.5      (table at RVA 0x368c4d0)
//   -> 60 / 40 / 30 / 20 Hz, at most 3 substeps per frame, and the rendered cloth is NOT
//   interpolated between steps. Above 60 fps even the nearest cloth visibly updates at a lower
//   rate than the rest of the frame (every other frame at 120 fps), NPC cloth at 30-20 Hz.
//
// [cloth] mode=render rewrites the table every frame to a step just below the current frame
// time, so every cloth (all LODs) advances at least once per rendered frame. mode=scale
// multiplies the original intervals by `scale` (diagnostics; e.g. scale=20 makes cloth visibly
// step at a few Hz). mode=game leaves the table alone (restoring it if it was patched).
// The table is only touched after verifying it holds the expected v1.62 values.
void cloth_on_present();

} // namespace aco
