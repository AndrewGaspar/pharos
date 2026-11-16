#include "core/config.h"

#include <windows.h>

#include <cstdlib>
#include <mutex>

#include "core/log.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace aco {

namespace {
Config g_config;
std::mutex g_mutex;
bool g_loaded = false;

std::wstring ini_path() { return module_dir() + L"\\aco_dlss.ini"; }

std::string read_str(const wchar_t* key, const char* def) {
  wchar_t buf[256];
  std::wstring wdef(def, def + strlen(def));
  GetPrivateProfileStringW(L"upscaler", key, wdef.c_str(), buf, 256, ini_path().c_str());
  std::string s;
  for (wchar_t* c = buf; *c; ++c) s.push_back(static_cast<char>(*c));
  // strip inline comments and whitespace
  if (auto p = s.find(';'); p != std::string::npos) s.resize(p);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  return s;
}

float read_float(const wchar_t* key, float def) {
  std::string s = read_str(key, "");
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
  return c;
}

bool same(const Config& a, const Config& b) {
  return a.enabled == b.enabled && a.backend == b.backend && a.preset == b.preset && a.debug == b.debug &&
         a.jitter_x == b.jitter_x && a.jitter_y == b.jitter_y && a.mv_x == b.mv_x && a.mv_y == b.mv_y &&
         a.render_scale == b.render_scale && a.mip_bias == b.mip_bias && a.mip_bias_offset == b.mip_bias_offset &&
         a.latency_log == b.latency_log;
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
  bool changed = !g_loaded || !same(c, g_config);
  if (changed) {
    g_config = c;
    ACO_LOG("config: enabled=%d backend=%s preset=%s debug=%s jitter=(%g,%g) mv=(%g,%g) render_scale=%g mip_bias=%s%+g latency_log=%d",
            c.enabled, c.backend.c_str(), c.preset.c_str(), c.debug.c_str(), c.jitter_x, c.jitter_y, c.mv_x, c.mv_y,
            c.render_scale, c.mip_bias.c_str(), c.mip_bias_offset, c.latency_log);
  }
  g_loaded = true;
  return changed;
}

} // namespace aco
