#pragma once
#include <algorithm>
#include <cmath>

namespace aco::rig {
struct Vec {
  float x, y, z;
  Vec operator+(Vec b) const { return {x + b.x, y + b.y, z + b.z}; }
  Vec operator-(Vec b) const { return {x - b.x, y - b.y, z - b.z}; }
  Vec operator*(float s) const { return {x * s, y * s, z * s}; }
  float dot(Vec b) const { return x * b.x + y * b.y + z * b.z; }
  Vec cross(Vec b) const { return {y * b.z - z * b.y, z * b.x - x * b.z, x * b.y - y * b.x}; }
};
struct Quat {
  Vec v;
  float w;
  Vec rotate(Vec a) const {
    Vec t = v.cross(a) * 2;
    return a + t * w + v.cross(t);
  }
  Quat operator*(Quat b) const { return {b.v * w + v * b.w + v.cross(b.v), w * b.w - v.dot(b.v)}; }
  bool unit() const { return std::fabs(v.dot(v) + w * w - 1) < 0.002f; }
};
// Shortest-arc rate limiting leaves the animated eye midpoint untouched.
inline Quat limit_turn(Quat prev, Quat target, float radians) {
  auto normalize = [](Quat q) {
    float inv = 1 / std::sqrt(q.v.dot(q.v) + q.w * q.w);
    return Quat{q.v * inv, q.w * inv};
  };
  prev = normalize(prev);
  target = normalize(target);
  float dot = prev.v.dot(target.v) + prev.w * target.w;
  if (dot < 0) {
    target = {target.v * (-1), -target.w};
    dot = -dot;
  }
  float half = std::acos(std::clamp(dot, 0.f, 1.f));
  if (half * 2 <= radians || half < 1e-7f) return target;
  float denom = std::sin(half);
  float a = std::sin(half - radians * .5f) / denom, b = std::sin(radians * .5f) / denom;
  return normalize({prev.v * a + target.v * b, prev.w * a + target.w * b});
}
struct Pose {
  Vec p;
  Quat q;  // Hamilton xyzw. Maps sensor coordinates (+right,+down,+forward) to world metres.
  Pose operator*(Pose b) const { return {p + q.rotate(b.p), q * b.q}; }
};
using Anchor = bool (*)(Pose&);
}  // namespace aco::rig
