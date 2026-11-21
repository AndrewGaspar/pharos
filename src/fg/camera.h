#pragma once

namespace aco {
struct FgCamera;
namespace ngx {
class Params;
}
struct CamOut {
  bool ok = false;
  float view_to_clip[16], clip_to_view[16], clip_to_prev[16], prev_to_clip[16];
  float pos[3], up[3], right[3], fwd[3];
  float near_z = 0.1f, far_z = 1e5f, fov = 1.0f, aspect = 1.6f;
  double jit_ndc[2] = {};  // jitter found in the projection (NDC units)
};

CamOut camera_from(const FgCamera& c, bool unjitter);
void set_camera_basis(ngx::Params& p, const CamOut& cam);
}  // namespace aco
