#pragma once
#include <cstddef>

namespace aco {
struct MarkerTarget {
  float x, y, z;
};
constexpr unsigned kMaxMarkerTargets = 192;
void marker_targets_frame(bool active);
size_t marker_targets_read(MarkerTarget (&out)[kMaxMarkerTargets]);
}  // namespace aco
