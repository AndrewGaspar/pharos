#include "core/hook.h"

#include <windows.h>

#include <mutex>

#include "core/log.h"

namespace aco {

SlotEntry g_slots[kMaxSlots][kMaxVtblsPerSlot];

namespace {
std::mutex g_mutex;
} // namespace

void hook_vtable(void* obj, size_t index, void* fn) {
  if (index >= kMaxSlots) {
    ACO_LOG("hook_vtable: slot %zu out of range", index);
    return;
  }
  std::lock_guard lock(g_mutex);
  void** vtbl = *reinterpret_cast<void***>(obj);
  for (auto& e : g_slots[index]) {
    void** v = e.vtbl.load(std::memory_order_relaxed);
    if (v == vtbl) return;  // already hooked
    if (v) continue;
    void* prev = vtbl[index];
    if (prev == fn) return;
    e.orig = prev;
    e.vtbl.store(vtbl, std::memory_order_release);
    DWORD old;
    VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    vtbl[index] = fn;
    VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
    return;
  }
  ACO_LOG("hook_vtable: too many vtables for slot %zu", index);
}

} // namespace aco
