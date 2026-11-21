#include "world/teleport.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/lab.h"
#include "rig/capture.h"
#include "core/log.h"

namespace aco {

namespace {

// Player Entity (AnvilNext): vtable at +0, world matrix (4 float4 rows) at +0x20: row 0 = right,
// row 1 = forward, row 2 = up (Z-up world), row 3 = position (+0x50). The entity's position is
// authoritative: the character controller and every derived copy follow it.
constexpr uintptr_t kEntityVtRva = 0x365e460;
constexpr uintptr_t kForward = 0x30, kUp = 0x40, kPos = 0x50;  // row 0 (right) at +0x20

// Static roots that lead to the player entity: entity = *(*(base + rva) + off). Both survive
// restarts and the entity being recreated (respawn); the first valid one is used.
struct Chain {
  uintptr_t rva, off;
};
constexpr Chain kChains[] = {{0x4c23c40, 0x0}, {0x4c5d788, 0x78}};

uintptr_t image_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

bool readable(uintptr_t p, size_t n) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!p || !VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
  return p + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

uintptr_t read_ptr(uintptr_t p) { return readable(p, 8) ? *reinterpret_cast<uintptr_t*>(p) : 0; }

bool valid_entity(uintptr_t e, bool upright = true) {
  if (!readable(e, kPos + 16) || read_ptr(e) != image_base() + kEntityVtRva) return false;
  const float* up = reinterpret_cast<const float*>(e + kUp);
  const float* pos = reinterpret_cast<const float*>(e + kPos);
  for (int i = 0; i < 3; ++i)
    if (!std::isfinite(pos[i]) || std::fabs(pos[i]) > 1e5f) return false;
  if (upright) return std::fabs(up[2] - 1.0f) < 0.05f && pos[3] == 1.0f;
  const float norm2 = up[0] * up[0] + up[1] * up[1] + up[2] * up[2];
  return std::fabs(norm2 - 1) < 0.1f && pos[3] == 1.0f;
}

uintptr_t find_player(bool upright = true) {
  static uintptr_t last = 1;
  uintptr_t found = 0;
  std::string diag;
  for (const Chain& c : kChains) {
    uintptr_t root = read_ptr(image_base() + c.rva);
    uintptr_t e = root ? read_ptr(root + c.off) : 0;
    bool ok = valid_entity(e, upright);
    if (ok && !found) found = e;
    char b[96];
    snprintf(b, sizeof(b), " [%#llx+%#llx]=%p%s", (unsigned long long)c.rva, (unsigned long long)c.off,
             reinterpret_cast<void*>(e), ok ? (e == found ? "*" : "!") : "x");
    diag += b;
  }
  if (found != last) ACO_LOG("teleport: player entity %p; chains%s", reinterpret_cast<void*>(found), diag.c_str());
  last = found;
  return found;
}

// Render camera (what PassParams.viewToWorld is built from): global pointer at rva 0x49e86b8
// (also 0x49f39d8); vtable rva 0x36afd88; position at +0x130, forward (unit) at +0x140.
constexpr uintptr_t kCameraPtrRva = 0x49e86b8, kCameraVtRva = 0x36afd88, kCamPos = 0x130, kCamFwd = 0x140;

// x y z yaw pitch (degrees; yaw 0 = +X, 90 = +Y).
bool read_camera(float out[5]) {
  uintptr_t c = read_ptr(image_base() + kCameraPtrRva);
  if (!readable(c, kCamFwd + 12) || read_ptr(c) != image_base() + kCameraVtRva) return false;
  const float* p = reinterpret_cast<const float*>(c + kCamPos);
  const float* f = reinterpret_cast<const float*>(c + kCamFwd);
  out[0] = p[0], out[1] = p[1], out[2] = p[2];
  out[3] = std::atan2(f[1], f[0]) * 57.29578f;
  out[4] = std::atan2(f[2], std::hypot(f[0], f[1])) * 57.29578f;
  return std::isfinite(out[3]) && std::isfinite(out[4]);
}

float* pos_of(uintptr_t e) { return reinterpret_cast<float*>(e + kPos); }

float yaw_of(uintptr_t e) {
  const float* f = reinterpret_cast<const float*>(e + kForward);
  return std::atan2(f[1], f[0]) * 57.29578f;
}

double now_s() {
  LARGE_INTEGER c, f;
  QueryPerformanceCounter(&c);
  QueryPerformanceFrequency(&f);
  return double(c.QuadPart) / double(f.QuadPart);
}

void write_file(const wchar_t* name, const char* text) {
  std::wstring path = out_dir() + L"\\" + name, tmp = path + L".tmp";
  FILE* f = _wfopen(tmp.c_str(), L"w");
  if (!f) return;
  fputs(text, f);
  fclose(f);
  MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// The write happens on the render thread while the simulation thread updates the entity, so a
// single write can be lost; rewriting it every frame instead fights the character controller
// (it derives motion from the displacement and the player gets flung or respawned), and once the
// player is airborne the physics body owns the position (entity writes no longer take). So: write
// once, check a few frames later, and only rewrite if it didn't take or the player fell through.
enum class Phase { Idle, Place, Verify, Watch };
struct {
  Phase phase = Phase::Idle;
  float target[3] = {};
  double watch = 4, t_phase = 0, t_start = 0, t_log = 0;
  float drop = 1.0f;  // placed this far above the requested point (see teleport_on_present)
  int frames = 0, places = 0;
  unsigned poll = 0;
  bool from_file = true;  // requested through <out>\teleport.txt (else through the lab channel)
} S;

// The last finished teleport, for the lab verb (which waits for `g_done` to move).
std::mutex g_result_mu;
std::string g_result;  // JSON object
std::atomic<unsigned> g_done{0};

void finish(const char* status, uintptr_t e) {
  char buf[256];
  float p[3] = {};
  if (e) memcpy(p, pos_of(e), 12);
  float yaw = e ? yaw_of(e) : 0.0f;
  double elapsed = now_s() - S.t_start;
  snprintf(buf, sizeof(buf), "%s %.3f %.3f %.3f %.1f writes=%d elapsed=%.2f\n", status, p[0], p[1], p[2], yaw,
           S.places, elapsed);
  if (S.from_file) write_file(L"teleport.done", buf);
  ACO_LOG("teleport: %s", buf);
  char js[320];
  snprintf(js, sizeof(js), "{\"status\":\"%s\",\"player\":[%.3f,%.3f,%.3f],\"yaw\":%.1f,\"writes\":%d,\"elapsed\":%.2f}",
           status, p[0], p[1], p[2], yaw, S.places, elapsed);
  {
    std::lock_guard lk(g_result_mu);
    g_result = js;
  }
  g_done.fetch_add(1);
  S.phase = Phase::Idle;
}

// Starts a teleport (render thread). Returns false (and finishes with a failure) if it can't start.
bool start(const float target[3], double hold, float drop, bool from_file) {
  S = decltype(S){};
  S.t_start = now_s();
  S.from_file = from_file;
  if (hold >= 0) S.watch = hold;
  if (drop >= 0) S.drop = drop;
  memcpy(S.target, target, sizeof(S.target));
  uintptr_t e = find_player();
  if (!e) {
    finish("fail no-player", 0);
    return false;
  }
  S.phase = Phase::Place;
  return true;
}

// "x y z yaw camera cx cy cz cyaw cpitch" (as <out>\where.txt) and the same as JSON.
void where_now(std::string* text, std::string* json) {
  uintptr_t e = find_player();
  float cam[5];
  bool have_cam = read_camera(cam);
  char buf[160], js[320];
  if (e) {
    const float* p = pos_of(e);
    if (have_cam)
      snprintf(buf, sizeof(buf), "%.3f %.3f %.3f %.1f camera %.3f %.3f %.3f %.2f %.2f\n", p[0], p[1], p[2], yaw_of(e),
               cam[0], cam[1], cam[2], cam[3], cam[4]);
    else
      snprintf(buf, sizeof(buf), "%.3f %.3f %.3f %.1f\n", p[0], p[1], p[2], yaw_of(e));
    snprintf(js, sizeof(js), "{\"player\":[%.3f,%.3f,%.3f],\"yaw\":%.1f", p[0], p[1], p[2], yaw_of(e));
  } else {
    snprintf(buf, sizeof(buf), "none\n");
    snprintf(js, sizeof(js), "{\"player\":null");
  }
  std::string j = js;
  if (have_cam) {
    snprintf(js, sizeof(js), ",\"camera\":{\"pos\":[%.3f,%.3f,%.3f],\"yaw\":%.2f,\"pitch\":%.2f}", cam[0], cam[1],
             cam[2], cam[3], cam[4]);
    j += js;
  }
  j += "}";
  if (text) *text = buf;
  if (json) *json = j;
}

void poll_triggers() {
  std::wstring where = out_dir() + L"\\where";
  if (GetFileAttributesW(where.c_str()) != INVALID_FILE_ATTRIBUTES) {
    DeleteFileW(where.c_str());
    std::string text;
    where_now(&text, nullptr);
    write_file(L"where.txt", text.c_str());
    ACO_LOG("teleport: where -> %s", text.c_str());
  }

  std::wstring path = out_dir() + L"\\teleport.txt";
  FILE* f = _wfopen(path.c_str(), L"r");
  if (!f) return;
  char line[256] = {};
  if (!fgets(line, sizeof(line), f)) line[0] = 0;
  fclose(f);
  DeleteFileW(path.c_str());
  DeleteFileW((out_dir() + L"\\teleport.done").c_str());
  ACO_LOG("teleport: request %s", line);

  float target[3] = {};
  double hold = -1;
  float drop = -1;
  int n = 0;
  for (char* tok = strtok(line, " \t\r\n"); tok; tok = strtok(nullptr, " \t\r\n")) {
    if (!strncmp(tok, "hold=", 5)) hold = atof(tok + 5);
    else if (!strncmp(tok, "drop=", 5)) drop = static_cast<float>(atof(tok + 5));
    else if (n < 3) target[n++] = static_cast<float>(atof(tok));
  }
  if (n < 3) {
    S = decltype(S){};
    S.t_start = now_s();
    return finish("fail bad-request", 0);
  }
  start(target, hold, drop, true);
}

// Readiness for the lab (`quiver lab wait <i> in-world`), checked every 30 presents:
//   menu      not in a world: no player entity, or one near z = 0 (the title and main menu keep a
//             placeholder entity at (0, 1, 0.9); while a world loads the player waits in the Animus
//             void near (-100, 33.5, 0.2));
//   in-world  the player is in a world (two checks 30 presents apart agree).
// Only written on a change.
void world_state() {
  static unsigned n = 0;
  static std::string cur, pending;
  if (++n % 30) return;
  uintptr_t e = find_player();
  std::string st = !e ? "menu" : std::fabs(pos_of(e)[2]) < 5.0f ? "menu" : "in-world";
  if (st == cur) {
    pending.clear();
    return;
  }
  if (st == "in-world" && pending != st) {  // confirm at the next check
    pending = st;
    return;
  }
  pending.clear();
  cur = st;
  std::vector<std::string> kv;
  if (e) {
    char b[96];
    const float* p = pos_of(e);
    snprintf(b, sizeof(b), "player=%.1f %.1f %.1f", p[0], p[1], p[2]);
    kv.emplace_back(b);
  }
  char f[48];
  snprintf(f, sizeof(f), "present=%llu", (unsigned long long)present_count());
  kv.emplace_back(f);
  lab_state(st, kv);
}

bool parse_float(const std::string& s, float& out) {
  char* end = nullptr;
  out = strtof(s.c_str(), &end);
  return end && end != s.c_str() && *end == 0 && std::isfinite(out);
}

// `quiver lab teleport <i> <location | x y z [yaw [pitch]]> [hold=S] [drop=M]` (Quiver strips
// locations.txt's "# comments"). Moves the player only: the camera can't be turned from inside the
// game (SendInput mouse motion never reaches the game's raw-input mouse-look under Wine; XTEST
// motion from the host does), so yaw/pitch are returned as "want", and quiver lab teleport turns
// the camera there with mouse-look in a closed loop on `where` (as quiver lab turn).
std::string verb_teleport(const std::vector<std::string>& a, bool& ok) {
  float v[5];
  int n = 0;
  double hold = -1;
  float drop = -1;
  for (const auto& t : a) {
    if (t.rfind("hold=", 0) == 0) hold = atof(t.c_str() + 5);
    else if (t.rfind("drop=", 0) == 0) drop = static_cast<float>(atof(t.c_str() + 5));
    else if (n < 5 && parse_float(t, v[n])) n++;
    else return lab_error(ok, "teleport: x y z [yaw [pitch]] [hold=S] [drop=M]; not a number: " + t);
  }
  if (n < 3) return lab_error(ok, "teleport: x y z [yaw [pitch]] [hold=S] [drop=M]");
  unsigned seq = g_done.load();
  bool started = false;
  if (!on_present([&] {
        if (S.phase != Phase::Idle) return;
        started = true;
        start(v, hold, drop, false);
      }))
    return lab_error(ok, "teleport: game not presenting");
  if (!started) return lab_error(ok, "teleport: another teleport is running");
  for (int i = 0; g_done.load() == seq; ++i) {
    if (i > 1800) return lab_error(ok, "teleport: no result in 90 s");
    Sleep(50);
  }
  std::string r;
  {
    std::lock_guard lk(g_result_mu);
    r = g_result;
  }
  if (r.find("\"status\":\"ok\"") == std::string::npos) {
    ok = false;
    return json_str(r);
  }
  if (n >= 4) {
    char w[96];
    snprintf(w, sizeof(w), ",\"want\":{\"yaw\":%.2f%s", v[3], n >= 5 ? ",\"pitch\":" : "");
    r = r.substr(0, r.size() - 1) + w;
    if (n >= 5) {
      snprintf(w, sizeof(w), "%.2f", v[4]);
      r += w;
    }
    r += "}}";
  }
  return r;
}

} // namespace

bool camera_at_player_head() {
  float cam[5];
  if (!read_camera(cam)) return false;
  for (const Chain& chain : kChains) {
    uintptr_t root = read_ptr(image_base() + chain.rva);
    uintptr_t e = root ? read_ptr(root + chain.off) : 0;
    if (!valid_entity(e)) continue;
    const float* p = pos_of(e);
    float dx = cam[0]-p[0], dy = cam[1]-p[1], dz = cam[2]-p[2];
    return dx*dx + dy*dy < 0.6f*0.6f && dz > 0.5f && dz < 2.2f;
  }
  return false;
}

void teleport_register() {
  rig::register_anchor("face", [](rig::Pose& out) {
    uintptr_t entity = find_player(false);
    if (!entity || !readable(entity + 0xd8, 10)) return false;
    uintptr_t components = read_ptr(entity + 0xd8);
    auto count = *reinterpret_cast<const uint16_t*>(entity + 0xe0);
    if (count > 512 || !readable(components, count * sizeof(uintptr_t))) return false;
    for (unsigned i = 0; i < count; ++i) {
      auto component = read_ptr(components + sizeof(uintptr_t) * i);
      if (read_ptr(component) != image_base() + 0x35f5508 || read_ptr(component + 0x10) != entity) continue;
      auto look = read_ptr(component + 0xb0);
      auto runtime = read_ptr(component + 0x4c0);
      if (read_ptr(look) != image_base() + 0x35f4228 || read_ptr(runtime + 0x1a8) != component ||
          read_ptr(runtime + 0x1b0) != look || !readable(runtime + 0x1e8, 10) ||
          *reinterpret_cast<const uint16_t*>(runtime + 0x1f0) != 9)
        return false;
      auto chain = read_ptr(runtime + 0x1e8);
      if (!readable(chain, 9 * 32)) return false;
      // LookAtData's evaluated world-space chain ends in head and two eyes.
      // The eye joints can turn independently. Use the head's orientation for the mount.
      rig::Quat head;
      rig::Vec a, b;
      memcpy(&head, reinterpret_cast<void*>(chain + 13 * 16), sizeof(head));
      memcpy(&a, reinterpret_cast<void*>(chain + 14 * 16), sizeof(a));
      memcpy(&b, reinterpret_cast<void*>(chain + 16 * 16), sizeof(b));
      float ipd2 = (a - b).dot(a - b);
      if (!head.unit() || !(ipd2 > 0.04f * 0.04f && ipd2 < 0.09f * 0.09f)) return false;
      out = {(a + b) * 0.5f, head * rig::Quat{{0, 0.70710678f, -0.70710678f}, 0}};
      return true;
    }
    return false;
  });
  if (!lab_active()) return;
  // `quiver lab where <i>`: player position and heading, render camera (Z up, yaw 0 = +X, 90 = +Y).
  lab_verb("where", [](const std::vector<std::string>&, bool& ok) -> std::string {
    std::string j;
    if (!on_present([&] { where_now(nullptr, &j); })) return lab_error(ok, "where: game not presenting");
    return j;
  });
  lab_verb("teleport", verb_teleport);
}

void teleport_on_present() {
  if (lab_active()) world_state();
  if (S.phase == Phase::Idle) {
    if (++S.poll % 8) return;
    poll_triggers();
    if (S.phase == Phase::Idle) return;
  }
  uintptr_t e = find_player();
  if (!e) return finish("fail lost-player", 0);
  float* p = pos_of(e);
  float dz = p[2] - S.target[2];
  float dxy = std::hypot(p[0] - S.target[0], p[1] - S.target[1]);
  double now = now_s();
  if (S.phase != Phase::Place && now - S.t_log >= 0.25) {
    ACO_LOG("teleport: t=%.2f pos %.2f %.2f %.2f (dxy %.2f dz %.2f)", now - S.t_start, p[0], p[1], p[2], dxy, dz);
    S.t_log = now;
  }
  switch (S.phase) {
    case Phase::Place:
      if (++S.places > 8) return finish("fail unstable", e);
      // A short fall makes the character controller move the player itself; placed exactly on the
      // ground the model stays culled (only its shadow draws) until the player moves.
      p[0] = S.target[0], p[1] = S.target[1], p[2] = S.target[2] + S.drop;
      S.phase = Phase::Verify;
      S.frames = 0;
      break;
    case Phase::Verify:
      if (++S.frames < 6) break;
      if (dxy > 3.0f || std::fabs(dz - S.drop) > 3.0f) {
        ACO_LOG("teleport: write didn't take (dxy %.1f dz %.1f); again", dxy, dz);
        S.phase = Phase::Place;
      } else {
        S.phase = Phase::Watch;
        S.t_phase = now;
      }
      break;
    case Phase::Watch:
      if (dz < -4.0f || dxy > 10.0f) {
        // Fell through ground that wasn't streamed in yet (or got pushed away): put it back.
        ACO_LOG("teleport: drifted (dxy %.1f dz %.1f); placing again", dxy, dz);
        S.phase = Phase::Place;
      } else if (now - S.t_phase >= S.watch) {
        finish("ok", e);
      }
      break;
    default:
      break;
  }
}

} // namespace aco
