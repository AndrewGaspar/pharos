#include "core/d3d11_title.h"
#include "core/config.h"
#include "core/function_key.h"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>

#include "core/lab.h"
#include "core/log.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace aco {

namespace {
Config g_config;  // effective (file + runtime toggles)
bool g_fg_blocked = false;  // stereo has (or may have) run this session: no Frame Generation
Config g_file;    // as last read from the file
std::mutex g_mutex;
bool g_loaded = false;
std::atomic<unsigned> g_generation{0};  // bumped by every reload that changed something

// pharos.ini; an install that still only has the pre-rename aco_dlss.ini keeps working.
std::wstring ini_path() {
  std::wstring p = module_dir() + L"\\" + title::ini;
  if (title::origins && GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
    std::wstring old = module_dir() + L"\\aco_dlss.ini";
    if (GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES) return old;
  }
  return p;
}

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
  c.fg_mode = read_str(L"mode", "off", L"fg");
  c.fg_mv_scale = read_float(L"mv_scale", 1.0f, L"fg");
  c.fg_pacing = read_float(L"pacing", 1.0f, L"fg") != 0.0f;
  c.fg_show = read_str(L"show", "both", L"fg");
  c.fg_debug_fill = read_float(L"debug_fill", 0.0f, L"fg") != 0.0f;
  c.fg_ngx_log = static_cast<int>(read_float(L"ngx_log", 0.0f, L"fg"));
  c.fg_bb_format_dxgi = read_float(L"bb_format_dxgi", 0.0f, L"fg") != 0.0f;
  c.fg_input = read_str(L"input", "swapchain", L"fg");
  c.fg_swap_outputs = read_float(L"swap_outputs", 0.0f, L"fg") != 0.0f;
  c.fg_hudless = read_float(L"hudless", 1.0f, L"fg") != 0.0f;
  c.fg_refresh_hz = read_float(L"refresh_hz", 0.0f, L"fg");
  c.fg_target_hz = read_float(L"target_hz", 0.0f, L"fg");
  {
    // count: presented frames per real frame (2x..6x) or auto. Older inis said count=1 for 2x
    // (DLSS-G's MultiFrameCount, generated frames per real frame): anything below 2 means 2x.
    std::string s = read_str(L"count", "2", L"fg");
    int n = s == "auto" ? 0 : atoi(s.c_str());
    c.fg_count = s == "auto" ? 0 : n < 2 ? 2 : n > 6 ? 6 : n;
  }
  c.fg_count_min = static_cast<int>(read_float(L"count_min", 2.0f, L"fg"));
  c.fg_count_max = static_cast<int>(read_float(L"count_max", 4.0f, L"fg"));
  c.fg_show_index = static_cast<int>(read_float(L"show_index", 1.0f, L"fg"));
  c.fg_raw_output = read_float(L"raw_output", 0.0f, L"fg") != 0.0f;
  c.fg_trace_cu = read_float(L"trace_cu", 0.0f, L"fg") != 0.0f;
  c.fg_hide_handle64 = read_float(L"hide_handle64", 0.0f, L"fg") != 0.0f;
  c.fg_present_thread = static_cast<int>(read_float(L"present_thread", 1.0f, L"fg"));
  c.fg_skip = read_str(L"skip", "", L"fg");
  c.fg_enabled = read_float(L"enabled", 1.0f, L"fg") != 0.0f;
  c.fg_toggle_key = read_str(L"toggle_key", "F9", L"fg");
  c.fg_frame_latency = static_cast<int>(read_float(L"frame_latency", 0.0f, L"fg"));
  c.fg_buffers = static_cast<int>(read_float(L"buffers", 0.0f, L"fg"));
  c.fg_sync = read_str(L"sync", "cpu", L"fg");
  c.fg_present_gate = read_float(L"present_gate", 1.0f, L"fg") != 0.0f;
  c.fg_verify = read_float(L"verify", 0.0f, L"fg") != 0.0f;
  c.fg_camera = read_str(L"camera", "engine", L"fg");
  c.fg_camera_unjitter = read_float(L"camera_unjitter", 1.0f, L"fg") != 0.0f;
  c.fg_mv_units = read_str(L"mv_units", "px", L"fg");
  c.fg_auto_cap = read_float(L"auto_cap", 1.0f, L"fg") != 0.0f;
  c.fg_auto_cap_lag = read_float(L"auto_cap_lag", 0.7f, L"fg");
  c.fg_auto_cap_lag_from = read_str(L"auto_cap_lag_from", "ready", L"fg");
  c.fg_auto_cap_min = read_float(L"auto_cap_min", 0.0f, L"fg");
  c.fg_auto_cap_max = read_float(L"auto_cap_max", 0.0f, L"fg");
  c.fg_sets = static_cast<int>(read_float(L"sets", 2.0f, L"fg"));
  c.fg_pace = read_str(L"pace", "smooth", L"fg");
  c.fg_pace_relax = read_float(L"pace_relax", 0.5f, L"fg");
  c.fg_done_fence = read_str(L"done_fence", "shared", L"fg");
  c.fg_ts_guard = read_float(L"ts_guard", 1.0f, L"fg") != 0.0f;
  c.fg_trace_presents = static_cast<int>(read_float(L"trace_presents", 1200.0f, L"fg"));
  c.fg_trace_shots = static_cast<int>(read_float(L"trace_shots", 120.0f, L"fg"));
  c.fg12_descriptor_heap = read_float(L"descriptor_heap", 1.0f, L"fg") != 0.0f;
  {
    auto parse = [](const std::string& extra, std::vector<std::pair<std::string, unsigned int>>& out) {
      size_t pos = 0;
      while (pos < extra.size()) {
        size_t end = extra.find(',', pos);
        if (end == std::string::npos) end = extra.size();
        std::string item = extra.substr(pos, end - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos)
          out.emplace_back(item.substr(0, colon), static_cast<unsigned int>(strtoul(item.c_str() + colon + 1, nullptr, 0)));
        pos = end + 1;
      }
    };
    parse(read_str(L"extra", "", L"fg"), c.fg_extra);
    parse(read_str(L"create_extra", "", L"fg"), c.fg_create_extra);
  }
  c.reflex = read_str(L"mode", "on", L"reflex");
  c.reflex_fps_limit = read_float(L"fps_limit", 0.0f, L"reflex");
  c.reflex_probe = read_float(L"probe", 0.0f, L"reflex") != 0.0f;
  c.reflex_limiter = read_str(L"limiter", "auto", L"reflex");
  c.world_lod_distance = read_float(L"lod_distance", 1.0f, L"world");
  c.world_lod_skip = read_str(L"lod_skip", "", L"world");
  c.world_lod_mode = read_str(L"lod_mode", "far", L"world");
  c.world_npc_density = read_float(L"npc_density", 1.0f, L"world");
  c.world_npc_distance = read_float(L"npc_distance", 1.0f, L"world");
  c.world_npc_spawn_ceiling = read_float(L"npc_spawn_ceiling", 800.0f, L"world");
  c.world_npc_mem_floor = read_float(L"npc_mem_floor", 3072.0f, L"world");
  c.world_npc_senu_max = read_float(L"npc_senu_max", 0.0f, L"world");
  c.world_npc_units = read_float(L"npc_units", 1.0f, L"world");
  c.world_npc_units_kill = read_float(L"npc_units_kill", 0.0f, L"world");
  c.world_npc_units_log = read_float(L"npc_units_log", 0.0f, L"world");
  c.world_npc_units_rural = read_float(L"npc_units_rural", 2.0f, L"world");
  c.world_npc_units_hostile = read_float(L"npc_units_hostile", 1.0f, L"world");
  c.world_npc_urban_threshold = read_float(L"npc_urban_threshold", 300.0f, L"world");
  c.world_npc_units_max = read_float(L"npc_units_max", 0.0f, L"world");
  c.world_npc_units_rate = read_float(L"npc_units_rate", 0.0f, L"world");
  c.world_tls_extend = read_float(L"tls_extend", 1.0f, L"world");
  c.reflex_markers = read_float(L"markers", 1.0f, L"reflex") != 0.0f;
  c.reflex_marker_set = read_str(L"marker_set", "minimal", L"reflex");
  c.toggle_key = read_str(L"toggle_key", "F10");
  c.reflex_optimize = read_float(L"optimize", 0.0f, L"reflex") != 0.0f;
  c.stereo_mode = read_str(L"mode", "off", L"stereo");
  c.stereo_output = read_str(L"output", "auto", L"stereo");
  // Stereo presents its own composition; Frame Generation (which presents generated frames from
  // its own back buffer) is off whenever stereo is configured.
  // Once set, for the whole session: Frame Generation can't start mid-session cleanly. mode=auto
  // blocks it only when the display is a stereo one when the game creates its swapchain
  // (config_block_fg, from stereo).
  if (c.stereo_mode == "sbs") g_fg_blocked = true;
  if (g_fg_blocked) c.fg_mode = "off";
  c.stereo_separation = read_float(L"separation", 32.5f, L"stereo");
  c.stereo_convergence = read_float(L"convergence", 4.0f, L"stereo");
  c.stereo_show = read_str(L"show", "sbs", L"stereo");
  c.stereo_diff_gain = read_float(L"diff_gain", 16.0f, L"stereo");
  c.stereo_swap = read_float(L"swap", 0.0f, L"stereo") != 0.0f;
  c.stereo_skip = read_str(L"skip", "", L"stereo");
  c.stereo_effects = read_str(L"effects", "ssr,fog,clouds,cloudmarch,shadow,flare", L"stereo");
  c.stereo_aim_eye = read_str(L"aim_eye", "left", L"stereo");
  c.stereo_markers = read_float(L"markers", 1, L"stereo") != 0;
  c.stereo_cursor = read_str(L"cursor", "game", L"stereo");
  c.cloth_mode = read_str(L"mode", "game", L"cloth");
  c.cloth_scale = read_float(L"scale", 1.0f, L"cloth");
  return c;
}

bool same(const Config& a, const Config& b) {
  return a.enabled == b.enabled && a.backend == b.backend && a.preset == b.preset && a.debug == b.debug &&
         a.jitter_x == b.jitter_x && a.jitter_y == b.jitter_y && a.mv_x == b.mv_x && a.mv_y == b.mv_y &&
         a.render_scale == b.render_scale && a.mip_bias == b.mip_bias && a.mip_bias_offset == b.mip_bias_offset &&
         a.latency_log == b.latency_log && a.fg_mode == b.fg_mode && a.fg_mv_scale == b.fg_mv_scale && a.fg_pacing == b.fg_pacing && a.fg_show == b.fg_show && a.fg_debug_fill == b.fg_debug_fill && a.fg_ngx_log == b.fg_ngx_log && a.fg_bb_format_dxgi == b.fg_bb_format_dxgi && a.fg_extra == b.fg_extra && a.fg_input == b.fg_input && a.fg_swap_outputs == b.fg_swap_outputs && a.fg_hudless == b.fg_hudless && a.fg_refresh_hz == b.fg_refresh_hz && a.fg_target_hz == b.fg_target_hz && a.fg_count == b.fg_count && a.fg_count_min == b.fg_count_min && a.fg_count_max == b.fg_count_max && a.fg_show_index == b.fg_show_index && a.fg_raw_output == b.fg_raw_output && a.fg_trace_cu == b.fg_trace_cu && a.fg_hide_handle64 == b.fg_hide_handle64 && a.fg12_descriptor_heap == b.fg12_descriptor_heap && a.fg_verify == b.fg_verify && a.fg_camera == b.fg_camera && a.fg_pace == b.fg_pace && a.fg_sets == b.fg_sets && a.fg_auto_cap == b.fg_auto_cap && a.fg_auto_cap_lag == b.fg_auto_cap_lag && a.fg_auto_cap_lag_from == b.fg_auto_cap_lag_from && a.fg_auto_cap_min == b.fg_auto_cap_min && a.fg_auto_cap_max == b.fg_auto_cap_max && a.fg_pace_relax == b.fg_pace_relax && a.fg_done_fence == b.fg_done_fence && a.fg_ts_guard == b.fg_ts_guard && a.fg_camera_unjitter == b.fg_camera_unjitter && a.fg_mv_units == b.fg_mv_units && a.fg_trace_presents == b.fg_trace_presents && a.fg_trace_shots == b.fg_trace_shots && a.fg_present_gate == b.fg_present_gate && a.fg_sync == b.fg_sync && a.fg_frame_latency == b.fg_frame_latency && a.fg_skip == b.fg_skip && a.fg_enabled == b.fg_enabled && a.fg_toggle_key == b.fg_toggle_key && a.fg_buffers == b.fg_buffers && a.fg_present_thread == b.fg_present_thread && a.fg_create_extra == b.fg_create_extra && a.reflex == b.reflex && a.reflex_fps_limit == b.reflex_fps_limit && a.reflex_limiter == b.reflex_limiter &&
         a.reflex_probe == b.reflex_probe && a.reflex_markers == b.reflex_markers &&
         a.reflex_marker_set == b.reflex_marker_set && a.reflex_optimize == b.reflex_optimize &&
         a.toggle_key == b.toggle_key && a.cloth_mode == b.cloth_mode && a.cloth_scale == b.cloth_scale &&
         a.stereo_mode == b.stereo_mode && a.stereo_output == b.stereo_output && a.stereo_separation == b.stereo_separation &&
         a.stereo_convergence == b.stereo_convergence && a.stereo_show == b.stereo_show &&
         a.stereo_diff_gain == b.stereo_diff_gain && a.stereo_swap == b.stereo_swap && a.stereo_skip == b.stereo_skip && a.stereo_effects == b.stereo_effects && a.stereo_cursor == b.stereo_cursor &&
         a.stereo_aim_eye == b.stereo_aim_eye && a.stereo_markers == b.stereo_markers &&
         a.world_lod_distance == b.world_lod_distance && a.world_lod_skip == b.world_lod_skip && a.world_lod_mode == b.world_lod_mode && a.world_npc_density == b.world_npc_density &&
         a.world_npc_distance == b.world_npc_distance && a.world_npc_spawn_ceiling == b.world_npc_spawn_ceiling && a.world_npc_units == b.world_npc_units &&
         a.world_npc_mem_floor == b.world_npc_mem_floor && a.world_npc_senu_max == b.world_npc_senu_max &&
         a.world_npc_units_kill == b.world_npc_units_kill &&
         a.world_npc_units_log == b.world_npc_units_log && a.world_npc_units_rural == b.world_npc_units_rural &&
         a.world_npc_units_hostile == b.world_npc_units_hostile && a.world_npc_urban_threshold == b.world_npc_urban_threshold && a.world_npc_units_max == b.world_npc_units_max &&
         a.world_npc_units_rate == b.world_npc_units_rate && a.world_tls_extend == b.world_tls_extend;
}
} // namespace

std::wstring module_dir() {
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), buf, MAX_PATH);
  std::wstring s(buf);
  return s.substr(0, s.find_last_of(L"\\/"));
}

void config_block_fg() {
  std::lock_guard lock(g_mutex);
  if (g_fg_blocked) return;
  g_fg_blocked = true;
  g_config.fg_mode = g_file.fg_mode = "off";
  ACO_LOG("config: Frame Generation off for this session (stereo)");
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
    g_generation.fetch_add(1);
    g_file = c;
    g_config = c;
    ACO_LOG("config: enabled=%d backend=%s preset=%s debug=%s jitter=(%g,%g) mv=(%g,%g) render_scale=%g mip_bias=%s%+g latency_log=%d reflex=%s fps_limit=%g",
            c.enabled, c.backend.c_str(), c.preset.c_str(), c.debug.c_str(), c.jitter_x, c.jitter_y, c.mv_x, c.mv_y,
            c.render_scale, c.mip_bias.c_str(), c.mip_bias_offset, c.latency_log, c.reflex.c_str(), c.reflex_fps_limit);
  }
  g_loaded = true;
  return changed;
}

namespace {
std::string trim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
  size_t i = s.find_first_not_of(" \t");
  return i == std::string::npos ? "" : s.substr(i);
}

// Reads (and with `value`, replaces) section.key in pharos.ini, editing only that line's value, like
// `quiver lab set`. Returns false if the key isn't in the file.
bool ini_line(const std::string& section, const std::string& key, const std::string* value, std::string& old) {
  std::wstring path = ini_path();
  std::ifstream in{std::filesystem::path(path), std::ios::binary};
  if (!in) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  in.close();
  std::string text = ss.str(), out, cur;
  bool found = false;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = nl == std::string::npos ? text.size() : nl + 1;
    std::string t = trim(line);
    if (!t.empty() && t[0] == '[' && t.back() == ']') {
      cur = t.substr(1, t.size() - 2);
    } else if (!found && cur == section && t.find('=') != std::string::npos && trim(t.substr(0, t.find('='))) == key) {
      found = true;
      old = trim(t.substr(t.find('=') + 1));
      if (value) line = key + "=" + *value + (line.size() && line.back() == '\r' ? "\r" : "");
    }
    out += line;
    if (nl != std::string::npos) out += '\n';
  }
  if (!found || !value || *value == old) return found;
  std::wstring tmp = path + L".tmp";
  {
    std::ofstream f(std::filesystem::path(tmp), std::ios::binary | std::ios::trunc);
    f << out;
  }
  return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
}  // namespace

void config_register_lab() {
  if (!lab_active()) return;
  // `quiver lab call <i> cfg section.key [value]`: read, or set and wait for the reload (pharos.ini
  // is re-read every 64 presents). The same file `quiver lab set <i> pharos:[section]key=value`
  // edits.
  lab_verb("cfg", [](const std::vector<std::string>& a, bool& ok) -> std::string {
    if (a.empty() || a[0].find('.') == std::string::npos) return lab_error(ok, "usage: cfg section.key [value]");
    std::string sec = a[0].substr(0, a[0].find('.')), key = a[0].substr(a[0].find('.') + 1), old;
    if (a.size() < 2) {
      if (!ini_line(sec, key, nullptr, old)) return lab_error(ok, "no " + a[0] + " in pharos.ini");
      return "{\"key\":" + json_str(a[0]) + ",\"value\":" + json_str(old) + "}";
    }
    unsigned gen = g_generation.load();
    if (!ini_line(sec, key, &a[1], old)) return lab_error(ok, "no " + a[0] + " in pharos.ini");
    bool reloaded = false;
    if (old != a[1])
      for (int i = 0; i < 200 && !(reloaded = g_generation.load() != gen); ++i) Sleep(50);
    return "{\"key\":" + json_str(a[0]) + ",\"value\":" + json_str(a[1]) + ",\"was\":" + json_str(old) +
           ",\"reloaded\":" + (reloaded ? "true" : "false") + "}";
  });
}

void config_toggle_upscaler() {
  std::lock_guard lock(g_mutex);
  g_config.enabled = !g_config.enabled;
  ACO_LOG("config: hotkey -> %s", g_config.enabled ? "DLSS" : "game TAA");
}

int config_toggle_vk() { return config_key_vk(config().toggle_key); }

int config_key_vk(const std::string& k) { return function_key(std::string_view(k)); }

} // namespace aco
