#pragma once
#include <windows.h>

namespace aco {

// Replaces the import address table entry of `module` for dll!func with `hook`. The previous
// target is stored to *orig BEFORE the entry is switched (other threads may call the hook at
// once). Returns false if the import wasn't found.
//
// AC Origins doesn't call user32 through its own IAT (Denuvo/VMProtect resolve imports privately),
// but Wine's user32 forwards to win32u through *its* IAT, so hooking user32's imports catches every
// caller in the process.
bool iat_hook(HMODULE module, const char* dll, const char* func, void* hook, void** orig);

} // namespace aco
