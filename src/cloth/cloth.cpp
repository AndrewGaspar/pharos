#include "cloth/cloth.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "core/config.h"
#include "core/log.h"

namespace aco {

namespace {

constexpr uintptr_t kTableRva = 0x368c4d0;      // float[4] cloth step intervals per LOD
constexpr uintptr_t kTableUseRva = 0x1536b55;   // movaps xmm0, [rip + table]
constexpr float kOriginal[4] = {1.0f / 30.0f, 0.05f, 1.0f / 15.0f, 0.1f};
constexpr float kEngineFactor = 0.5f;            // step = table[lod] * timescale * 0.5

enum class State { Unchecked, Ok, Unsupported };
State g_state = State::Unchecked;
float* g_table = nullptr;
bool g_patched = false;
double g_ema_dt = 0;
LARGE_INTEGER g_last{};
std::string g_applied_mode;
float g_applied_scale = 0;

bool verify() {
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
  if (nt->OptionalHeader.SizeOfImage <= kTableRva + 16) return false;
  // The table must hold the expected values, and the cloth step code must reference it.
  float t[4];
  memcpy(t, base + kTableRva, sizeof(t));
  for (int i = 0; i < 4; ++i)
    if (t[i] != kOriginal[i]) return false;
  const uint8_t* insn = base + kTableUseRva;  // 0f 28 05 <disp32>
  if (insn[0] != 0x0f || insn[1] != 0x28 || insn[2] != 0x05) return false;
  int32_t disp;
  memcpy(&disp, insn + 3, 4);
  if (reinterpret_cast<uintptr_t>(insn + 7 + disp) != reinterpret_cast<uintptr_t>(base + kTableRva)) return false;
  g_table = reinterpret_cast<float*>(base + kTableRva);
  return true;
}

void write_table(const float v[4]) {
  DWORD old = 0;
  if (!VirtualProtect(g_table, 16, PAGE_READWRITE, &old)) return;
  for (int i = 0; i < 4; ++i) __atomic_store_n(reinterpret_cast<uint32_t*>(&g_table[i]),
                                               *reinterpret_cast<const uint32_t*>(&v[i]), __ATOMIC_RELAXED);
  VirtualProtect(g_table, 16, old, &old);
}

} // namespace

void cloth_on_present() {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  if (g_last.QuadPart) {
    double dt = double(now.QuadPart - g_last.QuadPart) / double(freq.QuadPart);
    if (dt > 0 && dt < 0.25) g_ema_dt = g_ema_dt > 0 ? g_ema_dt * 0.9 + dt * 0.1 : dt;
  }
  g_last = now;

  const Config& cfg = config();
  if (g_state == State::Unchecked) {
    if (cfg.cloth_mode == "game") return;  // don't even look unless asked to
    g_state = verify() ? State::Ok : State::Unsupported;
    ACO_LOG("cloth: step table %s", g_state == State::Ok ? "found (v1.62 layout)" : "not found; cloth patch disabled");
  }
  if (g_state != State::Ok) return;

  if (cfg.cloth_mode != g_applied_mode || cfg.cloth_scale != g_applied_scale) {
    ACO_LOG("cloth: mode=%s scale=%g", cfg.cloth_mode.c_str(), cfg.cloth_scale);
    g_applied_mode = cfg.cloth_mode;
    g_applied_scale = cfg.cloth_scale;
  }
  if (cfg.cloth_mode == "render" && g_ema_dt > 0) {
    // A step a bit shorter than the frame time guarantees >= 1 step per rendered frame (the
    // engine allows up to 3 substeps). Never step less often than the game itself would (low
    // frame rates keep the native intervals), and never above 240 Hz.
    float step = static_cast<float>(std::max(g_ema_dt * 0.8, 1.0 / 240.0));
    float v[4];
    for (int i = 0; i < 4; ++i) v[i] = std::min(kOriginal[i], step / kEngineFactor);
    write_table(v);
    g_patched = true;
  } else if (cfg.cloth_mode == "scale" && cfg.cloth_scale > 0) {
    float v[4];
    for (int i = 0; i < 4; ++i) v[i] = kOriginal[i] * cfg.cloth_scale;
    write_table(v);
    g_patched = true;
  } else if (g_patched) {
    write_table(kOriginal);
    g_patched = false;
  }
}

} // namespace aco
