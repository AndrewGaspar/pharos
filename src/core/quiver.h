#pragma once

namespace aco {

// Quiver mod SDK (third_party/quiver_mod.h, protocol 3). Pharos still takes its options from
// pharos.ini; the SDK channel tells Quiver how the presented frames are packed (stereo), so its
// HUD and Sight panel are drawn once per eye. Inert when not running under Quiver.
void quiver_init();
// "sbs" | "hsbs" | "mono": what the presented image is (not whether stereo rendering is on).
void quiver_stereo_layout(const char* layout);

} // namespace aco
