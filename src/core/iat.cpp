#include "core/iat.h"

#include <cstdint>
#include <cstring>

#include "core/log.h"

namespace aco {

bool iat_hook(HMODULE module, const char* dll, const char* func, void* hook, void** orig) {
  auto* base = reinterpret_cast<uint8_t*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return false;
  for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
    if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0) continue;
    if (!imp->OriginalFirstThunk) continue;
    auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
    auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (strcmp(reinterpret_cast<const char*>(by_name->Name), func) != 0) continue;
      DWORD old = 0;
      if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
      void* prev = reinterpret_cast<void*>(slots->u1.Function);
      __atomic_store_n(orig, prev, __ATOMIC_SEQ_CST);
      __atomic_store_n(&slots->u1.Function, reinterpret_cast<ULONG_PTR>(hook), __ATOMIC_SEQ_CST);
      VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
      ACO_LOG("iat: hooked %s!%s (was %p)", dll, func, prev);
      return true;
    }
  }
  return false;
}

} // namespace aco
