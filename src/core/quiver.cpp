#define QUIVER_MOD_VERSION "pharos"
#define QUIVER_MOD_IMPLEMENTATION
#include "core/quiver_mod.h"

#include "core/quiver.h"

#include <windows.h>

#include <string>
#include <thread>

#include "core/log.h"

namespace aco {
namespace {
quiver_mod* g_q = nullptr;
std::string g_layout;  // last layout said
}  // namespace

void quiver_init() {
  if (g_q) return;
  g_q = quiver_open("aco-dlss", nullptr, 0);
  if (!g_q) return;
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

} // namespace aco
