#pragma once
#include <atomic>
#include <cstddef>

namespace aco {

// Vtable slot hooking for COM objects.
//
// Different concrete classes implementing the same interface (e.g. DXVK's immediate vs deferred
// context) have different vtables, so the "original" function must be looked up per vtable.
// Originals are stored per (slot index, vtable) in a small lock-free table; lookup is a short
// linear scan on the hot path.

constexpr size_t kMaxSlots = 256;
constexpr size_t kMaxVtblsPerSlot = 8;

struct SlotEntry {
  std::atomic<void**> vtbl{nullptr};
  void* orig = nullptr;
};
extern SlotEntry g_slots[kMaxSlots][kMaxVtblsPerSlot];

// Patch slot `index` in the vtable of `obj` to `fn` (no-op if already patched).
void hook_vtable(void* obj, size_t index, void* fn);

// Original implementation of slot `index` for `obj`'s vtable.
inline void* original(void* obj, size_t index) {
  void** vtbl = *reinterpret_cast<void***>(obj);
  for (auto& e : g_slots[index]) {
    void** v = e.vtbl.load(std::memory_order_acquire);
    if (v == vtbl) return e.orig;
    if (!v) break;
  }
  return vtbl[index];  // not hooked on this vtable: the slot still holds the original
}

template <typename Fn>
inline Fn* orig(void* obj, size_t index) {
  return reinterpret_cast<Fn*>(original(obj, index));
}

} // namespace aco
