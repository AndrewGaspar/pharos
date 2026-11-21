#include "fg/camera.h"

#include <algorithm>
#include <cmath>
#include <nvsdk_ngx_defs_dlssg.h>
#include "dlss/ngx.h"
#include "dlss/upscaler.h"

namespace aco {
namespace {
// ---- Camera for DLSS-G ([fg] camera=engine) ---------------------------------------------------
// The engine's matrices are column-vector (v' = M v); DLSS-G wants row-major for post-multiplication
// (v' = v M), i.e. the transpose. DLSS-G wants them without TAA jitter.
struct M4 {
  double m[4][4] = {};
  static M4 id() {
    M4 r;
    for (int i = 0; i < 4; ++i) r.m[i][i] = 1;
    return r;
  }
};
M4 mul(const M4& a, const M4& b) {
  M4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
  return r;
}
bool inverse(const M4& a, M4& out) {
  double t[4][8];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 8; ++j) t[i][j] = j < 4 ? a.m[i][j] : (j - 4 == i ? 1 : 0);
  for (int c = 0; c < 4; ++c) {
    int p = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(t[r][c]) > std::fabs(t[p][c])) p = r;
    if (std::fabs(t[p][c]) < 1e-30) return false;
    if (p != c)
      for (int j = 0; j < 8; ++j) std::swap(t[p][j], t[c][j]);
    double d = t[c][c];
    for (int j = 0; j < 8; ++j) t[c][j] /= d;
    for (int r = 0; r < 4; ++r)
      if (r != c) {
        double f = t[r][c];
        for (int j = 0; j < 8; ++j) t[r][j] -= f * t[c][j];
      }
  }
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) out.m[i][j] = t[i][j + 4];
  return true;
}
void store_t(const M4& a, float out[16]) {  // transpose into DLSS-G's row-vector layout
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) out[i * 4 + j] = float(a.m[j][i]);
}

}  // namespace
CamOut camera_from(const FgCamera& c, bool unjitter) {
  CamOut o;
  if (!c.valid) return o;
  M4 c2v, v2w = M4::id(), pvp;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      c2v.m[i][j] = c.clip_to_view[i][j];
      pvp.m[i][j] = c.prev_view_proj[i][j];
      if (i < 3) v2w.m[i][j] = c.view_to_world[i][j];
    }
  M4 proj;
  if (!inverse(c2v, proj)) return o;
  // View space looks down -z (w = -z): a TAA jitter in the projection shows up as proj[0][2],
  // proj[1][2] (x_ndc = ... - proj[0][2]). Remove it from the current-frame matrices.
  M4 proj_u = proj, c2v_u = c2v, jcur = M4::id();
  o.jit_ndc[0] = -proj.m[0][2];
  o.jit_ndc[1] = -proj.m[1][2];
  if (unjitter) {
    proj_u.m[0][2] = proj_u.m[1][2] = 0;
    if (!inverse(proj_u, c2v_u)) return o;
    // jittered clip = J * unjittered clip (x_j = x_u + jit * w)
    jcur.m[0][3] = o.jit_ndc[0];
    jcur.m[1][3] = o.jit_ndc[1];
  }
  M4 c2p = mul(pvp, mul(v2w, mul(c2v, jcur)));  // unjittered current clip -> previous clip
  M4 p2c;
  if (!inverse(c2p, p2c)) return o;
  store_t(proj_u, o.view_to_clip);
  store_t(c2v_u, o.clip_to_view);
  store_t(c2p, o.clip_to_prev);
  store_t(p2c, o.prev_to_clip);
  auto norm = [](float v[3]) {
    float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0)
      for (int i = 0; i < 3; ++i) v[i] /= l;
  };
  for (int i = 0; i < 3; ++i) {
    o.right[i] = float(v2w.m[i][0]);
    o.up[i] = float(v2w.m[i][1]);
    o.fwd[i] = float(-v2w.m[i][2]);
    o.pos[i] = float(v2w.m[i][3]);
  }
  norm(o.right);
  norm(o.up);
  norm(o.fwd);
  // Near/far from the unjittered projection: view z where NDC depth is 1 (near, reversed Z) and 0.
  auto view_z = [&](double d) {
    double w = c2v_u.m[3][2] * d + c2v_u.m[3][3];
    double z = c2v_u.m[2][2] * d + c2v_u.m[2][3];
    return std::fabs(w) > 1e-30 ? -z / w : 1e30;
  };
  o.near_z = float(std::min(view_z(1.0), view_z(0.0)));
  double f = std::max(view_z(1.0), view_z(0.0));
  o.far_z = float(std::isfinite(f) && f < 1e7 ? f : 1e7);
  if (proj_u.m[1][1] != 0 && proj_u.m[0][0] != 0) {
    o.fov = float(2.0 * std::atan(1.0 / std::fabs(proj_u.m[1][1])));
    o.aspect = float(std::fabs(proj_u.m[1][1] / proj_u.m[0][0]));
  }
  o.ok = std::isfinite(o.near_z) && o.near_z > 0;
  return o;
}

void set_camera_basis(ngx::Params& p, const CamOut& cam) {
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraPosX, cam.pos[0]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraPosY, cam.pos[1]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraPosZ, cam.pos[2]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraUpX, cam.up[0]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraUpY, cam.up[1]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraUpZ, cam.up[2]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraRightX, cam.right[0]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraRightY, cam.right[1]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraRightZ, cam.right[2]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraFwdX, cam.fwd[0]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraFwdY, cam.fwd[1]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraFwdZ, cam.fwd[2]);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetX, 0.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetY, 0.0f);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraNear, cam.near_z);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraFar, cam.far_z);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraFOV, cam.fov);
  p.set(NVSDK_NGX_DLSSG_Parameter_CameraAspectRatio, cam.aspect);
}
}  // namespace aco
