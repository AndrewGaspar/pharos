#include <cstring>

#include "core/d3d11_title.h"
#include "rig/capture.h"

namespace aco::rig {
namespace {
struct Matrix {
  float m[4][4];
};
Matrix product(const Matrix& a, const Matrix& b) {
  Matrix r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
  return r;
}
void put(float (*c)[4], int reg, const Matrix& m) { std::memcpy(c + reg, &m, sizeof(m)); }
Matrix get(float (*c)[4], int reg) {
  Matrix m;
  std::memcpy(&m, c + reg, sizeof(m));
  return m;
}
}  // namespace
// All engine matrices here use column vectors and rows in registers.
// Keep gameplay projection separate from the reverse-Z depth reconstruction.
void project(Slot kind, void* data, const View& view, float depth_a, float depth_b) {
  auto c = static_cast<float (*)[4]>(data);
  Vec right = view.pose.q.rotate({1, 0, 0}), up = view.pose.q.rotate({0, -1, 0}),
      back = view.pose.q.rotate({0, 0, -1});
  auto p = view.pose.p;
  Matrix v{{{right.x, right.y, right.z, -right.dot(p)},
            {up.x, up.y, up.z, -up.dot(p)},
            {back.x, back.y, back.z, -back.dot(p)},
            {0, 0, 0, 1}}};
  Matrix inv{
      {{right.x, up.x, back.x, p.x}, {right.y, up.y, back.y, p.y}, {right.z, up.z, back.z, p.z}, {0, 0, 0, 1}}};
  Matrix proj{{{view.sx, 0, 0, 0}, {0, view.sy, 0, 0}, {0, 0, depth_a, depth_b}, {0, 0, -1, 0}}};
  Matrix vp = product(proj, v);
  auto forward = back * (-1), dx = right * (1 / view.sx), dy = up * (1 / view.sy);
  switch (kind) {
    case Slot::ViewProjection:
      put(c, 0, vp);
      return;
    case Slot::ScreenToWorld:
    case Slot::CloudProjection: {
      float x[3], y[3], z[3], pos[3];
      std::memcpy(x, &dx, 12);
      std::memcpy(y, &dy, 12);
      std::memcpy(z, &forward, 12);
      std::memcpy(pos, &p, 12);
      for (int i = 0; i < 3; ++i) {
        c[i][0] = x[i];
        c[i][1] = y[i];
        if (kind == Slot::CloudProjection) {
          c[i][2] = 0;
          c[i][3] = z[i];
        } else {
          c[i][2] = z[i];
          c[i][3] = pos[i];
        }
      }
      return;
    }
    case Slot::EyePosition:
      std::memcpy(c, &p, 12);
      return;
    case Slot::RayTopLeft: {
      auto tl = forward - dx + dy;
      std::memcpy(c, &tl, 12);
      return;
    }
    case Slot::RayRight: {
      auto r = dx * 2;
      std::memcpy(c, &r, 12);
      return;
    }
    case Slot::RayUp: {
      auto u = dy * 2;
      std::memcpy(c, &u, 12);
      return;
    }
    default:
      break;
  }
  if (kind == Slot::Instance) {
    put(c, 8, product(vp, get(c, 0)));
    put(c, 12, product(vp, get(c, 4)));
    return;
  }
  Matrix ip{
      {{1 / view.sx, 0, 0, 0}, {0, 1 / view.sy, 0, 0}, {0, 0, 0, -1}, {0, 0, 1 / depth_b, depth_a / depth_b}}};
  put(c, title::clip_to_view, ip);
  put(c, title::clip_to_world, product(inv, ip));
  for (int delta : {0, 1}) {
    std::memcpy(c[title::eye + delta], &p, sizeof(p));
    auto forward = back * (-1);
    std::memcpy(c[title::eye + 2 + delta], &forward, sizeof(forward));
    std::memcpy(c[title::view_translation + delta], &p, sizeof(p));
    put(c, title::view_to_world + 4 * delta, inv);
    put(c, title::world_to_view + 4 * delta, v);
  }
  if (kind == Slot::Screen && std::abs(c[title::view_proj + 3][3] - 1) < 1e-6f &&
      std::abs(c[title::view_proj + 3][0]) + std::abs(c[title::view_proj + 3][1]) +
              std::abs(c[title::view_proj + 3][2]) <
          .01f)
    return;
  if (kind == Slot::Main) put(c, title::projection, proj);
  put(c, title::view_proj, vp);
  put(c, title::view_proj + 8, vp);
  for (int i = 0; i < 3; ++i) v.m[i][3] = 0;
  auto vp0 = product(proj, v);
  put(c, title::view_proj + 4, vp0);
  put(c, title::view_proj + 12, vp0);
  for (int i = 0; i < 4; ++i) {
    float sign = (i % 2) ? -1.f : 1.f;
    float scale = -1 / std::sqrt(1 + (i < 2 ? view.sx * view.sx : view.sy * view.sy));
    for (int j = 0; j < 4; ++j) c[title::frustum + i][j] = scale * (vp.m[3][j] + sign * vp.m[i / 2][j]);
  }
}
}  // namespace aco::rig
