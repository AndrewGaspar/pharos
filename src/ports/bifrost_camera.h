#pragma once

namespace bifrost {
bool scene_camera_matches(const float* registers, unsigned bytes, const float* scene_camera);
// Move the active Valhalla view by metres along the camera right axis.
// The buffer is b0/space1, with four floats per register.
bool translate_camera(float* registers, unsigned bytes, const float* scene_camera, float eye_metres,
                      float convergence_metres);
}  // namespace bifrost
