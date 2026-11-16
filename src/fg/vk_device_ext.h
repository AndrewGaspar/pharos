#pragma once
#include <windows.h>

namespace aco {

// Hooks DXVK's GetProcAddress imports so its VkDevice is created with the extensions NGX Frame
// Generation needs (VK_KHR_push_descriptor). Must run before the game creates its first device.
void fg_hook_dxvk_device_creation(HMODULE dxvk_dxgi, HMODULE dxvk_d3d11);

} // namespace aco
