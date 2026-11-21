#include "core/content_loader.h"

#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>

#include "core/log.h"

namespace aco {
namespace {

using Byte = uint8_t;

// The game's arrays may start at a four-byte boundary, even on x64.
#pragma pack(push, 1)
template <typename T>
struct GameArray {
  T* data;
  uint32_t counts;
  uint32_t size() const { return counts >> 17; }
};
#pragma pack(pop)
using Batch = GameArray<const Byte*>;

Byte* image;
constexpr uintptr_t kContentManager = 0x4c23e48;
constexpr uintptr_t kFrameClock = 0x48b4a10;
constexpr uintptr_t kShutdown = 0x4c23eb0;
constexpr uintptr_t kBatchCaller = 0x135b6a5;
constexpr uintptr_t kEmptyPrepare = 0x25121c0;

template <typename T>
T load(const Byte* p) {
  return __atomic_load_n(reinterpret_cast<const T*>(p), __ATOMIC_ACQUIRE);
}

// The launch-screen request in 0x22b00c0 supplies an empty include list and excludes
// pack 30. The content worker owns these four request lists until its completion.
bool title_batch(const Byte* owner) {
  const Byte* mgr = load<const Byte*>(image + kContentManager);
  if (!mgr || owner != mgr + 0x188 || load<uint32_t>(mgr + 0x194) != 1 ||
      load<const Byte*>(mgr + 0x88) != image + 0x3654490)
    return false;
  const auto* request = reinterpret_cast<const GameArray<uint32_t>*>(mgr + 0x1a4);
  // Each inline request has four more bytes of embedded storage after its array header.
  const auto& except = *reinterpret_cast<const GameArray<uint32_t>*>(mgr + 0x1b4);
  return request->size() == 0 && except.size() == 1 && except.data && *except.data == 30 &&
         load<uint32_t>(mgr + 0x1cc) >> 17 == 0 && load<uint32_t>(mgr + 0x1dc) >> 17 == 0;
}

enum class Stage { acquired, prepared, published };
struct Publication {
  const Batch* batch;
  const Byte* last_pending;
};
thread_local Publication publication;

const Byte* resolved_asset(const Byte* entry) {
  const Byte* handle = load<const Byte*>(entry + 0x10);
  if (!handle || !(load<uint32_t>(handle + 8) & 0x80000000u)) return nullptr;
  return load<const Byte*>(handle);
}

bool skipped_duplicate(const Byte* entry, const Batch& registered) {
  // For kind zero the loader requests only the first registration of a key. An actual
  // request retains its handle at +0x30, even when the asset has not arrived yet.
  const Byte* handle = load<const Byte*>(entry + 0x10);
  if (load<uint32_t>(entry + 0x18) != 0 || !handle || load<const Byte*>(entry + 0x30) == handle)
    return false;
  const uint32_t key = load<uint32_t>(entry + 0x28);
  for (uint32_t i = 0; i < registered.size(); ++i) {
    const Byte* first = registered.data[i];
    if (load<uint32_t>(first + 0x28) == key)
      return first != entry && load<Byte>(first + 0x2d) == 1 && resolved_asset(first);
  }
  return false;
}

bool completed(const Batch& batch, const Batch& registered, Stage stage) {
  for (uint32_t i = 0; i < batch.size(); ++i) {
    const Byte* entry = batch.data[i];
    const bool marked = load<Byte>(entry + 0x2d) != 0;
    if (stage == Stage::published && marked) continue;
    const Byte* asset = resolved_asset(entry);
    if (!asset) {
      if (!marked && skipped_duplicate(entry, registered)) continue;
      return false;
    }
    if (stage == Stage::published) return false;
    if (stage == Stage::prepared && !marked) {
      const Byte* table = load<const Byte*>(asset);
      if (load<const Byte*>(table + 0xa8) != image + kEmptyPrepare) return false;
    }
  }
  return true;
}

template <Stage stage>
void wait_for_batch(const Batch* batch, const Byte* owner, const Byte* caller) {
  auto frame = [] {
    return load<uint32_t>(load<const Byte*>(image + kFrameClock) + 0x2c);
  };
  const uint32_t deadline = frame() + 3;
  bool eligible = (stage == Stage::published || caller == image + kBatchCaller) && title_batch(owner);
  const Byte* witness = nullptr;
  if constexpr (stage == Stage::prepared) {
    // The callback walks in array order. A new mark on the last unmarked entry proves
    // it has passed every earlier duplicate too. A missing final handle gives no witness.
    publication = {};
    if (eligible) {
      for (uint32_t i = batch->size(); i > 0; --i) {
        const Byte* entry = batch->data[i - 1];
        if (!load<Byte>(entry + 0x2d)) {
          if (resolved_asset(entry)) publication = {batch, entry};
          break;
        }
      }
    }
  }
  if constexpr (stage == Stage::published) {
    witness = publication.last_pending;
    eligible = eligible && publication.batch == batch && witness;
    publication = {};
  }
  do {
    reinterpret_cast<void (*)(uint32_t)>(image + 0x616f00)(1);
    if (frame() >= deadline || load<Byte>(image + kShutdown)) return;
    if (!eligible || (witness && !load<Byte>(witness + 0x2d))) continue;
    if (!completed(*batch, *reinterpret_cast<const Batch*>(owner), stage)) continue;
    if constexpr (stage == Stage::published) return;
    // Both native streaming-queue drains and every native preparation callback still
    // execute. Recheck the queue before shortening either of the two following delays.
    if (!reinterpret_cast<bool (*)()>(image + 0x8a41d0)()) return;
  } while (true);
}

// Three inlined waits from ACOrigins.exe v1.62. Validate the entire original block,
// including clock, sleep, deadline, and shutdown flag references, before changing any.
struct WaitSite {
  uint32_t rva;
  void (*wait)(const Batch*, const Byte*, const Byte*);
  Byte original[44];
};
const WaitSite sites[] = {
    {0x135c133, wait_for_batch<Stage::acquired>,
     {0x48,0x8b,0x05,0xd6,0x88,0x55,0x03,0x8b,0x58,0x2c,0x83,0xc3,0x03,0xb9,0x01,0,0,0,0xe8,0xb6,0xad,0x2b,
      0xff,0x48,0x8b,0x05,0xbf,0x88,0x55,0x03,0x39,0x58,0x2c,0x73,0x09,0x40,0x38,0x3d,0x53,0x7d,0x8c,0x03,0x74,0xe1}},
    {0x135c1e3, wait_for_batch<Stage::prepared>,
     {0x48,0x8b,0x05,0x26,0x88,0x55,0x03,0x8b,0x58,0x2c,0x83,0xc3,0x03,0xb9,0x01,0,0,0,0xe8,0x06,0xad,0x2b,
      0xff,0x48,0x8b,0x05,0x0f,0x88,0x55,0x03,0x39,0x58,0x2c,0x73,0x09,0x80,0x3d,0xa3,0x7c,0x8c,0x03,0x00,0x74,0xe1}},
    {0x135b6f4, wait_for_batch<Stage::published>,
     {0x48,0x8b,0x05,0x15,0x93,0x55,0x03,0x8b,0x78,0x2c,0x83,0xc7,0x03,0xb9,0x01,0,0,0,0xe8,0xf5,0xb7,0x2b,
      0xff,0x48,0x8b,0x05,0xfe,0x92,0x55,0x03,0x39,0x78,0x2c,0x73,0x09,0x44,0x38,0x35,0x92,0x87,0x8c,0x03,0x74,0xe1}},
};

bool matches(uintptr_t rva, const Byte* bytes, size_t n) {
  const Byte* p = image + rva;
  MEMORY_BASIC_INFORMATION region{};
  return VirtualQuery(p, &region, sizeof(region)) && region.AllocationBase == image && region.State == MEM_COMMIT &&
         !(region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && p + n <= static_cast<Byte*>(region.BaseAddress) + region.RegionSize &&
         memcmp(p, bytes, n) == 0;
}

bool install() {
  image = reinterpret_cast<Byte*>(GetModuleHandleW(nullptr));
  const Byte empty_method[] = {0xc2, 0, 0};
  const Byte publish[] = {0xff,0x90,0xb0,0,0,0,0xc6,0x47,0x2d,0x01};
  const Byte batch_call[] = {0x48,0x8d,0x55,0xa7,0x49,0x8b,0xcf,0xe8,0xcb,0x09,0,0};
  const Byte inner_args[] = {0x4c,0x8b,0xe2,0xc1,0xe8,0x11,0x4c,0x8b,0xf1};
  const Byte registration[] = {
      0x39,0x7b,0x18,0x75,0x3b,0x45,0x8b,0x46,0x08,0x49,0x8b,0x06,0x44,0x8b,0x4b,0x28,
      0x41,0xc1,0xe8,0x11,0x41,0xc1,0xe0,0x03,0x4c,0x03,0xc0,0x49,0x3b,0xc0,0x74,0x12,
      0x48,0x8b,0x10,0x44,0x39,0x4a,0x28,0x74,0x1f,0x48,0x83,0xc0,0x08,0x49,0x3b,0xc0,
      0x75,0xee,0x45,0x33,0xc0,0x48,0x8b,0xd3,0x49,0x8b,0xce,0xe8,0x2d,0x35,0x01,0,
      0x48,0x8b,0xcb,0xe8,0xb5,0xfe,0xff,0xff};
  const Byte retained_handle[] = {0x48,0x8d,0x73,0x30,0x48,0x8d,0x44,0x24,0x50,0x48,0x3b,0xf0,0x74,0x0e,
                                  0x48,0x8b,0x0e,0xe8,0xf0,0x7b,0x4d,0xff,0x48,0x8b,0xcd,0x48,0x89,0x3e};
  const Byte stream_busy[] = {0x48,0x8b,0x05,0x49,0xd1,0xfc,0x03,0x8b,0x88,0xd0,0x03,0,0,0x85,0xc9,0x0f,0x95,0xc0,0xc3};
  const Byte title_request[] = {0x48,0x8b,0x0d,0xc4,0x3a,0x97,0x02,0x4c,0x8d,0x44,0x24,0x40,
                                0x48,0x8d,0x54,0x24,0x50,0xe8,0x4d,0x12,0x0b,0xff};
  if (!matches(kEmptyPrepare, empty_method, sizeof(empty_method)) ||
      !matches(0x13679f1, publish, sizeof(publish)) || !matches(0x135b699, batch_call, sizeof(batch_call)) ||
      !matches(0x135c096, inner_args, sizeof(inner_args)) || !matches(0x8a41d0, stream_busy, sizeof(stream_busy)) ||
      !matches(0x135c0c3, registration, sizeof(registration)) || !matches(0x135c00a, retained_handle, sizeof(retained_handle)) ||
      !matches(0x22b037d, title_request, sizeof(title_request)))
    return false;
  for (const auto& site : sites)
    if (!matches(site.rva, site.original, sizeof(site.original))) return false;

  // Acquire all required pages first. Install during initial device creation, before
  // the interactive title screen can submit the first content job.
  Byte* pages[] = {image + 0x135b000, image + 0x135c000};
  DWORD old[2]{};
  for (size_t i = 0; i < 2; ++i) {
    if (!VirtualProtect(pages[i], 0x1000, PAGE_EXECUTE_READWRITE, &old[i])) {
      if (i) { DWORD ignored; VirtualProtect(pages[0], 0x1000, old[0], &ignored); }
      return false;
    }
  }
  for (const auto& site : sites) {
    // The original calls to Sleep already have Win64 shadow space and aligned RSP.
    // Inner routine: RCX=batch (R12), RDX=owner (R14), R8=original caller at [RSP+38h].
    // Outer routine: RCX=batch (&[RBP-59h]), RDX=owner (R15), R8=0.
    static constexpr Byte inner[] = {0x4c,0x89,0xe1,0x4c,0x89,0xf2,0x4c,0x8b,0x44,0x24,0x38};
    static constexpr Byte outer[] = {0x48,0x8d,0x4d,0xa7,0x4c,0x89,0xfa,0x45,0x31,0xc0};
    const bool nested = site.wait != wait_for_batch<Stage::published>;
    const size_t n = nested ? sizeof(inner) : sizeof(outer);
    std::array<Byte, sizeof(site.original)> code;
    code.fill(0x90);
    memcpy(code.data(), nested ? inner : outer, n);
    code[n] = 0x48; code[n + 1] = 0xb8;  // mov rax, wait
    memcpy(code.data() + n + 2, &site.wait, sizeof(site.wait));
    code[n + 10] = 0xff; code[n + 11] = 0xd0;  // call rax
    memcpy(image + site.rva, code.data(), code.size());
    FlushInstructionCache(GetCurrentProcess(), image + site.rva, code.size());
  }
  for (size_t i = 0; i < 2; ++i) {
    DWORD ignored;
    VirtualProtect(pages[i], 0x1000, old[i], &ignored);
  }
  return true;
}
} // namespace

void install_content_loader_fix() {
  static const bool installed = [] {
    bool ok = install();
    ACO_LOG("content loader: startup batch readiness waits %s", ok ? "enabled" : "unavailable on this build");
    return ok;
  }();
  (void)installed;
}
} // namespace aco
