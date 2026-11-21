#pragma once
#include <windows.h>

namespace aco {
// Use a transparent cursor on the game's client area while the compositor
// draws the visible Windows pointer into its eye images. Pass null to restore.
bool stereo_system_cursor(HWND window);
}  // namespace aco
