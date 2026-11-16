#include "world/teleport.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/log.h"

namespace aco {

namespace {

// Player Entity (AnvilNext): vtable at +0, world matrix (4 float4 rows) at +0x20: row 0 = right,
// row 1 = forward, row 2 = up (Z-up world), row 3 = position (+0x50). The entity's position is
// authoritative: the character controller and every derived copy follow it.
constexpr uintptr_t kEntityVtRva = 0x365e460;
constexpr uintptr_t kRight = 0x20, kForward = 0x30, kUp = 0x40, kPos = 0x50;

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

bool valid_entity(uintptr_t e) {
  if (!readable(e, kPos + 16) || read_ptr(e) != image_base() + kEntityVtRva) return false;
  const float* up = reinterpret_cast<const float*>(e + kUp);
  const float* pos = reinterpret_cast<const float*>(e + kPos);
  for (int i = 0; i < 3; ++i)
    if (!std::isfinite(pos[i]) || std::fabs(pos[i]) > 1e5f) return false;
  return std::fabs(up[2] - 1.0f) < 0.05f && pos[3] == 1.0f;  // upright, homogeneous w = 1
}

uintptr_t find_player() {
  static uintptr_t last = 1;
  uintptr_t found = 0;
  std::string diag;
  for (const Chain& c : kChains) {
    uintptr_t root = read_ptr(image_base() + c.rva);
    uintptr_t e = root ? read_ptr(root + c.off) : 0;
    bool ok = valid_entity(e);
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

float* pos_of(uintptr_t e) { return reinterpret_cast<float*>(e + kPos); }

float yaw_of(uintptr_t e) {
  const float* f = reinterpret_cast<const float*>(e + kForward);
  return std::atan2(f[1], f[0]) * 57.29578f;
}

void set_yaw(uintptr_t e, float deg) {
  float r = deg / 57.29578f, c = std::cos(r), s = std::sin(r);
  float right[4] = {s, -c, 0, 0}, fwd[4] = {c, s, 0, 0}, up[4] = {0, 0, 1, 0};
  memcpy(reinterpret_cast<void*>(e + kRight), right, 16);
  memcpy(reinterpret_cast<void*>(e + kForward), fwd, 16);
  memcpy(reinterpret_cast<void*>(e + kUp), up, 16);
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
// (it derives motion from the displacement and the player gets flung or respawned). So: write
// once, check a few frames later, and only rewrite if it didn't take or the player fell through.
enum class Phase { Idle, Place, Verify, Watch };
struct {
  Phase phase = Phase::Idle;
  float target[3] = {};
  float yaw = 0;
  bool has_yaw = false;
  double watch = 4, t_phase = 0, t_start = 0, t_log = 0;
  int frames = 0, places = 0;
  unsigned poll = 0;
} S;

void finish(const char* status, uintptr_t e) {
  char buf[256];
  float p[3] = {};
  if (e) memcpy(p, pos_of(e), 12);
  snprintf(buf, sizeof(buf), "%s %.3f %.3f %.3f %.1f writes=%d elapsed=%.2f\n", status, p[0], p[1], p[2],
           e ? yaw_of(e) : 0.0f, S.places, now_s() - S.t_start);
  write_file(L"teleport.done", buf);
  ACO_LOG("teleport: %s", buf);
  S.phase = Phase::Idle;
}

void poll_triggers() {
  std::wstring where = out_dir() + L"\\where";
  if (GetFileAttributesW(where.c_str()) != INVALID_FILE_ATTRIBUTES) {
    DeleteFileW(where.c_str());
    uintptr_t e = find_player();
    char buf[128];
    if (e) {
      const float* p = pos_of(e);
      snprintf(buf, sizeof(buf), "%.3f %.3f %.3f %.1f\n", p[0], p[1], p[2], yaw_of(e));
    } else {
      snprintf(buf, sizeof(buf), "none\n");
    }
    write_file(L"where.txt", buf);
    ACO_LOG("teleport: where -> %s", buf);
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

  S = decltype(S){};
  S.t_start = now_s();
  int n = 0;
  for (char* tok = strtok(line, " \t\r\n"); tok; tok = strtok(nullptr, " \t\r\n")) {
    if (!strncmp(tok, "hold=", 5)) S.watch = atof(tok + 5);
    else if (n < 3) S.target[n++] = static_cast<float>(atof(tok));
    else if (n == 3) S.yaw = static_cast<float>(atof(tok)), S.has_yaw = true, ++n;
  }
  if (n < 3) return finish("fail bad-request", 0);
  uintptr_t e = find_player();
  if (!e) return finish("fail no-player", 0);
  S.phase = Phase::Place;
}

} // namespace

void teleport_on_present() {
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
      memcpy(p, S.target, 12);
      if (S.has_yaw) set_yaw(e, S.yaw);
      S.phase = Phase::Verify;
      S.frames = 0;
      break;
    case Phase::Verify:
      if (++S.frames < 6) break;
      if (dxy > 3.0f || dz > 3.0f || dz < -3.0f) {
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
