#pragma once

#include <string>

namespace aco {

// Quiver mod SDK (core/quiver_mod.h, protocol 3.2). Pharos still takes its options from
// pharos.ini; the SDK channel tells Quiver how the presented frames are packed (stereo), so its
// HUD and Sight panel are drawn once per eye, and carries the phone's touch controls. Inert when
// not running under Quiver.
void quiver_init();
// "sbs" | "hsbs" | "mono": what the presented image is (not whether stereo rendering is on).
void quiver_stereo_layout(const char* layout);

// ---- Touch (Quiver design/TOUCH-SDK.md; a phone streaming the game) ----

// The phone's viewport, copied out of the SDK's (which is valid during its callback only).
// Points; rects are {x, y, w, h}. orientation "" = no touch client.
struct TouchView {
  std::string orientation, hand;
  float screen_w = 0, screen_h = 0, scale = 0;
  float safe[4] = {}, video[4] = {};
  std::string json;  // the whole viewport
};
// Subscribers run on the SDK thread, on every viewport (connect, rotation, hand change, leave);
// one registered after a viewport arrived is called once with it. At most 8.
using TouchViewFn = void (*)(const TouchView& v);
void quiver_touch_on_viewport(TouchViewFn fn);
bool quiver_touch_view(TouchView* out);  // the latest viewport; false = none yet

// Action handlers run on the SDK thread, in registration order, until one returns true.
// phase: 1 press, 2 release, 3 value. value -1..1; x, y in 0..1 of the video, < 0 = no point.
using TouchActionFn = bool (*)(const char* id, int phase, float value, float x, float y);
void quiver_touch_on_action(TouchActionFn fn);  // at most 8

void quiver_touch_layout(const char* json);    // replace the layout (copied; resent on reconnect)
void quiver_touch_context(const char* names);  // "explore", "map,menu", ... ("" = default); deduplicated

} // namespace aco
