#include "ports/bifrost_camera.h"

#include <cmath>
#include <initializer_list>

namespace bifrost {
bool translate_camera(float* buffer, unsigned bytes, const float* scene_camera, float dv, float convergence) {
  if (!scene_camera || bytes < 82 * 16 || !std::isfinite(dv)) return false;
  auto c = reinterpret_cast<float (*)[4]>(buffer);
  auto world = reinterpret_cast<const float (*)[4]>(scene_camera);
  // The flat UI has its own camera, and the shadow cameras can also be perspective.
  // Match the temporal pass by position and basis before editing any matrix.
  for (int i = 0; i < 3; ++i) {
    if (!(std::abs(c[6 + i][3] - world[i][3]) < .05f)) return false;
    for (int j = 0; j < 3; ++j)
      if (!(std::abs(c[6 + i][j] - world[i][j]) < .002f)) return false;
  }
  if (!(std::abs(c[29][2] + 1) < .001f && std::abs(c[29][3]) < .001f && c[26][0] > .01f)) return false;
  float d[3], prev[3];
  float k = convergence > 0 ? dv * c[26][0] / convergence : 0;
  for (int i = 0; i < 3; ++i) {
    d[i] = dv * c[6 + i][0];
    prev[i] = dv * c[10 + i][0];
    c[2][i] += d[i];
    c[3][i] += prev[i];
    c[6 + i][3] += d[i];
    c[10 + i][3] += prev[i];
    c[50][i] += d[i];
    c[51][i] += prev[i];
    c[56][i] += d[i];
  }
  c[18][3] -= dv;
  auto row_add = [&](int row, int src, float alpha) {
    for (int j = 0; j < 4; ++j) c[row][j] += alpha * c[src][j];
  };
  auto translate = [&](int row, const float* delta) {
    for (int j = 0; j < 3; ++j) c[row][3] -= c[row][j] * delta[j];
  };
  for (int i = 0; i < 4; ++i) {
    translate(30 + i, d);
    translate(38 + i, prev);
    translate(58 + i, d);
  }
  for (int base : {26, 30, 34, 38, 42}) row_add(base, base + 3, k);
  // Inverses of the off-axis projection, world VP and rotation-only VP.
  for (int base : {46, 69, 73, 77}) {
    for (int i = 0; i < 4; ++i) c[base + i][3] -= k * c[base + i][0];
  }
  for (int i = 0; i < 3; ++i) {
    row_add(46 + i, 49, prev[i]);
    row_add(73 + i, 76, d[i]);
  }
  return true;
}
}  // namespace bifrost
