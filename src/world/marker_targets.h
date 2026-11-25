#pragma once
#include <cstddef>

namespace aco {
struct MarkerTarget {
  float x, y, z;
};
constexpr unsigned kMaxMarkerTargets = 192;
// Positions kept per frame; a marker pass keeps the kMaxMarkerTargets nearest.
constexpr unsigned kMaxMarkerSamples = 1024;
void marker_targets_frame(bool active);
size_t marker_targets_read(MarkerTarget (&out)[kMaxMarkerTargets]);
size_t marker_targets_read(MarkerTarget* out, size_t max);
}  // namespace aco
