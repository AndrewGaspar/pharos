#pragma once
#include <d3d11.h>

#include <functional>

#include "rig/pose.h"

namespace aco::rig {
void register_anchor(const char* name, Anchor reader);
bool armed();
void poll();
void abort_capture(const char* reason);
void scene_color(ID3D11DeviceContext* ctx);
void capture(ID3D11DeviceContext* ctx, const std::function<void()>& replay, const char* unavailable);

enum class Slot {
  Main,
  Screen,
  Instance,
  ViewProjection,
  ScreenToWorld,
  EyePosition,
  CloudProjection,
  RayTopLeft,
  RayRight,
  RayUp
};
struct View {
  Pose pose;
  float sx, sy;  // projection scales = reciprocal tangent of half the field of view
};
const View* replay_view();
void project(Slot kind, void* data, const View& view, float depth_a, float depth_b);
}  // namespace aco::rig
