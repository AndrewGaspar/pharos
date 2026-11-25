#ifdef PORT_NAME
#define QUIVER_MOD_VERSION PORT_NAME
static constexpr auto mod_id = PORT_NAME;
#else
#define QUIVER_MOD_VERSION "pharos"
static constexpr auto mod_id = "aco-dlss";
#endif
#define QUIVER_MOD_IMPLEMENTATION
#include "core/quiver_mod.h"

#include "core/quiver.h"

#include <windows.h>

#include <mutex>
#include <string>
#include <thread>

#include "core/log.h"

namespace aco {
namespace {
quiver_mod* g_q = nullptr;
std::string g_layout;  // last layout said

// Touch: subscribers, the latest viewport and context.
std::mutex g_touch_mu;
TouchViewFn g_view_fns[8];
int g_view_n = 0;
TouchActionFn g_action_fns[8];
int g_action_n = 0;
TouchView g_view;
bool g_have_view = false;
std::string g_touch_ctx = "\x01";  // nothing said yet

void on_viewport(void*, const quiver_viewport* v) {
  TouchView t;
  t.orientation = v->orientation ? v->orientation : "";
  t.hand = v->hand ? v->hand : "";
  t.screen_w = v->screen_w;
  t.screen_h = v->screen_h;
  t.scale = v->scale;
  const quiver_rect* rs[2] = {&v->safe, &v->video};
  float* ds[2] = {t.safe, t.video};
  for (int i = 0; i < 2; i++) {
    ds[i][0] = rs[i]->x;
    ds[i][1] = rs[i]->y;
    ds[i][2] = rs[i]->w;
    ds[i][3] = rs[i]->h;
  }
  t.json = v->json ? v->json : "null";
  TouchViewFn fns[8];
  int n;
  {
    std::lock_guard<std::mutex> lk(g_touch_mu);
    g_view = t;
    g_have_view = true;
    n = g_view_n;
    for (int i = 0; i < n; i++) fns[i] = g_view_fns[i];
  }
  ACO_LOG("quiver touch: viewport %s %.0fx%.0f (video %.0f,%.0f %.0fx%.0f)",
          t.orientation.empty() ? "(none)" : t.orientation.c_str(), t.screen_w, t.screen_h,
          t.video[0], t.video[1], t.video[2], t.video[3]);
  for (int i = 0; i < n; i++) fns[i](t);
}

void on_action(void*, const char* id, quiver_touch_phase phase, float value, float x, float y) {
  TouchActionFn fns[8];
  int n;
  {
    std::lock_guard<std::mutex> lk(g_touch_mu);
    n = g_action_n;
    for (int i = 0; i < n; i++) fns[i] = g_action_fns[i];
  }
  for (int i = 0; i < n; i++)
    if (fns[i](id, (int)phase, value, x, y)) return;
  ACO_LOG("quiver touch: action %s (phase %d) not handled", id, (int)phase);
}
}  // namespace

void quiver_init() {
  if (g_q) return;
  g_q = quiver_open(mod_id, nullptr, 0);
  if (!g_q) return;
  quiver_on_viewport_cb(g_q, on_viewport, nullptr);
  quiver_on_touch_action_cb(g_q, on_action, nullptr);
  std::thread([] {
    for (int i = 0; i < 120; i++) {
      if (quiver_connected(g_q)) {
        ACO_LOG("quiver SDK: connected (%.1f s after open)", i * 0.5);
        return;
      }
      Sleep(500);
    }
    ACO_LOG("quiver SDK: not connected 60 s after open (not under Quiver, or no channel offered)");
  }).detach();
}

void quiver_stereo_layout(const char* layout) {
  if (g_layout == layout) return;
  g_layout = layout;
  if (g_q) quiver_set_stereo_layout(g_q, layout);
  ACO_LOG("quiver SDK: stereo layout %s", layout);
}

void quiver_touch_on_viewport(TouchViewFn fn) {
  TouchView t;
  bool have;
  {
    std::lock_guard<std::mutex> lk(g_touch_mu);
    if (g_view_n >= 8) return;
    g_view_fns[g_view_n++] = fn;
    have = g_have_view;
    t = g_view;
  }
  if (have) fn(t);
}

bool quiver_touch_view(TouchView* out) {
  std::lock_guard<std::mutex> lk(g_touch_mu);
  if (g_have_view && out) *out = g_view;
  return g_have_view;
}

void quiver_touch_on_action(TouchActionFn fn) {
  std::lock_guard<std::mutex> lk(g_touch_mu);
  if (g_action_n < 8) g_action_fns[g_action_n++] = fn;
}

void quiver_touch_layout(const char* json) {
  if (g_q && !quiver_touch_layout(g_q, json)) ACO_LOG("quiver touch: layout refused by the SDK");
}

void quiver_touch_context(const char* names) {
  {
    std::lock_guard<std::mutex> lk(g_touch_mu);
    if (g_touch_ctx == names) return;
    g_touch_ctx = names;
  }
  if (g_q) quiver_touch_context(g_q, names);
  ACO_LOG("quiver touch: context %s", names[0] ? names : "(default)");
}

} // namespace aco
