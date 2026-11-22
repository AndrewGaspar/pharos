#include "core/startup_splash.h"

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>

#include "core/log.h"

namespace aco {
namespace {

bool intro_logo_hidden(const wchar_t* executable) {
  std::wstring path(executable);
  const auto slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return false;
  const wchar_t* name = path.c_str() + slash + 1;
  if (_wcsicmp(name, L"ACOrigins.exe") && _wcsicmp(name, L"ACOrigins_plus.exe")) return false;
  path.resize(slash + 1);
  path += L"videos\\";
  // This stock Origins movie is present with both intro settings. It also distinguishes a
  // missing logo from a bad install path, an inaccessible directory or a missing videos tree.
  DWORD attr = GetFileAttributesW((path + L"ACE_Loading.bk2").c_str());
  if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
  return GetFileAttributesW((path + L"UbisoftLogo.bk2").c_str()) == INVALID_FILE_ATTRIBUTES &&
         GetLastError() == ERROR_FILE_NOT_FOUND;
}

bool skip_bitmap() {
  auto* image = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  auto* site = image + 0x2c3a425;
  // v1.62 calls FindResourceA(NULL, 1337, "PNG") here. Follow its existing null-resource
  // branch before the WIC decode or splash HWND creation. The startup thread exits normally.
  constexpr uint8_t request[] = {
      0x33,0xff, 0x4c,0x8d,0x05,0xda,0x8c,0xc4,0x00, 0xba,0x39,0x05,0x00,0x00,
      0x48,0x89,0x7d,0xdf, 0x33,0xc9, 0xff,0x15,0x59,0x00,0x90,0x00, 0x44,0x8d,0x67,0x01,
      0x48,0x8b,0xd8, 0x48,0x85,0xc0, 0x0f,0x84,0x85,0x00,0x00,0x00};
  MEMORY_BASIC_INFORMATION region{};
  if (!VirtualQuery(site, &region, sizeof(region)) || region.AllocationBase != image ||
      region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
      site + sizeof(request) > static_cast<uint8_t*>(region.BaseAddress) + region.RegionSize ||
      std::memcmp(site, request, sizeof(request)) != 0)
    return false;

  // A single opcode changes TEST RAX,RAX to XOR RAX,RAX, setting ZF for the original JE.
  // Other threads can see either complete instruction.
  auto* opcode = site + 0x22;
  static_assert(request[0x22] == 0x85);
  DWORD old = 0;
  if (!VirtualProtect(opcode, 1, PAGE_EXECUTE_READWRITE, &old)) return false;
  *opcode = 0x31;
  DWORD unused = 0;
  VirtualProtect(opcode, 1, old, &unused);
  FlushInstructionCache(GetCurrentProcess(), opcode, 1);
  return true;
}
} // namespace

void configure_startup_splash(const wchar_t* executable) {
  if (intro_logo_hidden(executable))
    ACO_LOG("startup splash: %s", skip_bitmap() ? "skipped with intro videos" : "unavailable on this build");
}

} // namespace aco
