#include "world/marker_targets.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include "core/log.h"

namespace aco {
namespace {
// UIIconsController::GetWorldPosition. The custom marker
// resolves its map position, entity/location controllers resolve the entity
// transform, and the position controller returns the authored point.
using GetPosition = float* (*)(void*, float*);
struct Controller {
  uintptr_t table, getter;
  GetPosition original = nullptr;
};
#if defined(BIFROST_VALHALLA)
// Valhalla uses the same controller type IDs as Odyssey. Its position
// getter returns a float4 from +0x200; all four slots are checked at runtime.
Controller controllers[] = {
    {0x4936520, 0x3176360}, {0x4936638, 0x31764d0}, {0x4936750, 0x3176320}, {0x4936868, 0x3176360}};
constexpr unsigned kGetPosition = 28;
#elif defined(AEGIS_ODYSSEY)
// Odyssey's Position controller returns its authored float4 at +0x200.
Controller controllers[] = {
    {0x444ef08, 0x2f83c90}, {0x444f010, 0x2f83d90}, {0x444f118, 0x2f83c00}, {0x444f220, 0x2f83c90}};
constexpr unsigned kGetPosition = 28;
#else
Controller controllers[] = {
    {0x3822b00, 0x22f6340}, {0x3822bf0, 0xa266d0}, {0x3822ce0, 0x22f6300}, {0x3822dd0, 0x22f6340}};
constexpr unsigned kGetPosition = 26;
#endif

struct Sample {
  MarkerTarget pos;
  unsigned frame;
};
std::unordered_map<void*, Sample> samples;
std::mutex lock;
std::atomic<bool> enabled{false};
unsigned frame;
MarkerTarget latest[kMaxMarkerSamples];
size_t count;

template <int I>
float* observe(void* self, float* out) {
  float* value = controllers[I].original(self, out);
  if (enabled.load(std::memory_order_relaxed) && value && std::isfinite(value[0]) && std::isfinite(value[1]) &&
      std::isfinite(value[2]) && (value[0] || value[1] || value[2])) {
    std::lock_guard guard(lock);
    samples[self] = {{value[0], value[1], value[2]}, frame};
  }
  return value;
}

bool install() {
  auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const GetPosition observers[] = {observe<0>, observe<1>, observe<2>, observe<3>};
  for (const Controller& c : controllers) {
    if (c.table + (kGetPosition + 1) * sizeof(void*) > pe->OptionalHeader.SizeOfImage) return false;
    auto* vt = reinterpret_cast<GetPosition*>(base + c.table);
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(vt + kGetPosition, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
      return false;
    if (vt[kGetPosition] != reinterpret_cast<GetPosition>(base + c.getter)) return false;
  }
  bool installed = false;
  for (size_t i = 0; i < std::size(observers); ++i) {
    auto& c = controllers[i];
    auto* slot = reinterpret_cast<GetPosition*>(base + c.table) + kGetPosition;
    DWORD old;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) continue;
    c.original = *slot;
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot), reinterpret_cast<void*>(observers[i]));
    VirtualProtect(slot, sizeof(*slot), old, &old);
    installed = true;
  }
  return installed;
}
}  // namespace

void marker_targets_frame(bool active) {
  static bool installed = false, attempted = false;
  if (active && !attempted) {
    attempted = true;
    installed = install();
    ACO_LOG("stereo: game world marker positions %s", installed ? "available" : "unavailable on this build");
  }
  enabled.store(active && installed, std::memory_order_relaxed);
  std::lock_guard guard(lock);
  ++frame;
  count = 0;
  for (auto it = samples.begin(); it != samples.end();) {
    if (!active || frame - it->second.frame > 8) {
      it = samples.erase(it);
      continue;
    }
    if (count < kMaxMarkerSamples) latest[count++] = it->second.pos;
    ++it;
  }
}

size_t marker_targets_read(MarkerTarget* out, size_t max) {
  std::lock_guard guard(lock);
  const size_t n = count < max ? count : max;
  memcpy(out, latest, n * sizeof(*out));
  return n;
}

size_t marker_targets_read(MarkerTarget (&out)[kMaxMarkerTargets]) {
  return marker_targets_read(out, kMaxMarkerTargets);
}
}  // namespace aco
