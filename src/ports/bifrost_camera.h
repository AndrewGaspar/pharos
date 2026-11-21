#pragma once

namespace bifrost {
// Move the active Valhalla view by metres along the camera right axis.
// The buffer is b0/space1, with four floats per register.
bool translate_camera(float* registers, unsigned bytes, const float* scene_camera, float eye_metres,
                      float convergence_metres);
}  // namespace bifrost
