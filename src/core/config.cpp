#include "core/config.h"

#include <windows.h>

#include <cstdlib>
#include <mutex>

#include "core/log.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace aco {

namespace {
Config g_config;  // effective (file + runtime toggles)
Config g_file;    // as last read from the file
std::mutex g_mutex;
bool g_loaded = false;

std::wstring ini_path() { return module_dir() + L"\\aco_dlss.ini"; }

std::string read_str(const wchar_t* key, const char* def, const wchar_t* section = L"upscaler") {
  wchar_t buf[256];
  std::wstring wdef(def, def + strlen(def));
  GetPrivateProfileStringW(section, key, wdef.c_str(), buf, 256, ini_path().c_str());
  std::string s;
  for (wchar_t* c = buf; *c; ++c) s.push_back(static_cast<char>(*c));
  // strip inline comments and whitespace
  if (auto p = s.find(';'); p != std::string::npos) s.resize(p);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  return s;
}

float read_float(const wchar_t* key, float def, const wchar_t* section = L"upscaler") {
  std::string s = read_str(key, "", section);
  return s.empty() ? def : static_cast<float>(atof(s.c_str()));
}

Config load() {
  Config c;
  c.enabled = read_float(L"enabled", 1.0f) != 0.0f;
  c.backend = read_str(L"backend", "dlss");
  c.preset = read_str(L"preset", "M");
  c.debug = read_str(L"debug", "none");
  c.jitter_x = read_float(L"jitter_x", 1.0f);
  c.jitter_y = read_float(L"jitter_y", 1.0f);
  c.mv_x = read_float(L"mv_x", 1.0f);
  c.mv_y = read_float(L"mv_y", 1.0f);
  c.render_scale = read_float(L"render_scale", 0.0f);
  c.mip_bias = read_str(L"mip_bias", "auto");
  c.mip_bias_offset = read_float(L"mip_bias_offset", 0.0f);
  c.latency_log = read_float(L"latency_log", 0.0f) != 0.0f;
  c.reflex = read_str(L"mode", "on", L"reflex");
  c.reflex_fps_limit = read_float(L"fps_limit", 0.0f, L"reflex");
  c.reflex_probe = read_float(L"probe", 0.0f, L"reflex") != 0.0f;
  c.world_lod_distance = read_float(L"lod_distance", 1.0f, L"world");
  c.reflex_markers = read_float(L"markers", 1.0f, L"reflex") != 0.0f;
  c.reflex_marker_set = read_str(L"marker_set", "minimal", L"reflex");
  c.toggle_key = read_str(L"toggle_key", "F10");
  c.reflex_optimize = read_float(L"optimize", 0.0f, L"reflex") != 0.0f;
  c.cloth_mode = read_str(L"mode", "game", L"cloth");
  c.cloth_scale = read_float(L"scale", 1.0f, L"cloth");
  return c;
}

bool same(const Config& a, const Config& b) {
  return a.enabled == b.enabled && a.backend == b.backend && a.preset == b.preset && a.debug == b.debug &&
         a.jitter_x == b.jitter_x && a.jitter_y == b.jitter_y && a.mv_x == b.mv_x && a.mv_y == b.mv_y &&
         a.render_scale == b.render_scale && a.mip_bias == b.mip_bias && a.mip_bias_offset == b.mip_bias_offset &&
         a.latency_log == b.latency_log && a.reflex == b.reflex && a.reflex_fps_limit == b.reflex_fps_limit &&
         a.reflex_probe == b.reflex_probe && a.reflex_markers == b.reflex_markers &&
         a.reflex_marker_set == b.reflex_marker_set && a.reflex_optimize == b.reflex_optimize &&
         a.toggle_key == b.toggle_key && a.cloth_mode == b.cloth_mode && a.cloth_scale == b.cloth_scale &&
         a.world_lod_distance == b.world_lod_distance;
}
} // namespace

std::wstring module_dir() {
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), buf, MAX_PATH);
  std::wstring s(buf);
  return s.substr(0, s.find_last_of(L"\\/"));
}

const Config& config() {
  if (!g_loaded) config_reload();
  return g_config;
}

bool config_reload() {
  Config c = load();
  std::lock_guard lock(g_mutex);
  // Compare against the file's previous contents, so runtime toggles survive until the file changes.
  bool changed = !g_loaded || !same(c, g_file);
  if (changed) {
    g_file = c;
    g_config = c;
    ACO_LOG("config: enabled=%d backend=%s preset=%s debug=%s jitter=(%g,%g) mv=(%g,%g) render_scale=%g mip_bias=%s%+g latency_log=%d reflex=%s fps_limit=%g",
            c.enabled, c.backend.c_str(), c.preset.c_str(), c.debug.c_str(), c.jitter_x, c.jitter_y, c.mv_x, c.mv_y,
            c.render_scale, c.mip_bias.c_str(), c.mip_bias_offset, c.latency_log, c.reflex.c_str(), c.reflex_fps_limit);
  }
  g_loaded = true;
  return changed;
}

void config_toggle_upscaler() {
  std::lock_guard lock(g_mutex);
  g_config.enabled = !g_config.enabled;
  ACO_LOG("config: hotkey -> %s", g_config.enabled ? "DLSS" : "game TAA");
}

int config_toggle_vk() {
  const std::string& k = config().toggle_key;
  if (k.size() >= 2 && (k[0] == 'F' || k[0] == 'f')) {
    int n = atoi(k.c_str() + 1);
    if (n >= 1 && n <= 24) return VK_F1 + n - 1;
  }
  return 0;  // "none" or unknown: no hotkey
}

} // namespace aco
