#include "core/dump.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "core/log.h"

namespace aco {

void dump_poll() {
  static unsigned n = 0;
  if (++n % 64) return;
  std::wstring trigger = out_dir() + L"\\dump-image";
  if (GetFileAttributesW(trigger.c_str()) == INVALID_FILE_ATTRIBUTES) return;
  DeleteFileW(trigger.c_str());
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
  size_t size = nt->OptionalHeader.SizeOfImage;
  std::wstring path = out_dir() + L"\\ACOrigins.image";
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  std::vector<uint8_t> zeros(1 << 16);
  size_t off = 0, readable = 0;
  while (off < size) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(base + off, &mbi, sizeof(mbi))) break;
    size_t len = std::min<size_t>(reinterpret_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize - (base + off), size - off);
    bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    DWORD w = 0;
    for (size_t done = 0; done < len;) {
      DWORD chunk = static_cast<DWORD>(std::min<size_t>(len - done, zeros.size()));
      WriteFile(f, ok ? base + off + done : zeros.data(), chunk, &w, nullptr);
      done += chunk;
    }
    if (ok) readable += len;
    off += len;
  }
  CloseHandle(f);
  ACO_LOG("dump: wrote %zu bytes (%zu readable) of the image at %p to ACOrigins.image", off, readable,
          static_cast<void*>(base));
}

} // namespace aco
