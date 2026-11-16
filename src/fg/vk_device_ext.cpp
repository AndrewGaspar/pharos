// Adds Vulkan device extensions that NGX's Frame Generation kernels need to DXVK's VkDevice.
//
// DXVK creates its device with the extensions *it* uses. NGX's Vulkan "cubin" path binds its
// kernels' resources with push descriptors (vkCmdPushDescriptorSetKHR, VK_KHR_push_descriptor);
// without the extension NGX's evaluate silently records nothing (logged by nvngx_dlssg as
// "Failed to load VK device function: vkCmdPushDescriptorSetKHR").
//
// DXVK loads winevulkan with LoadLibrary and resolves vkGetInstanceProcAddr with GetProcAddress
// (dxvk/src/vulkan/vulkan_loader.cpp); everything else comes from vkGetInstanceProcAddr. Hooking
// GetProcAddress in DXVK's import tables (dxgi.dll, d3d11.dll) lets us wrap vkCreateDevice and append
// the extension before the device exists.
#include "fg/vk_device_ext.h"

#include <windows.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstring>
#include <vector>

#include "core/iat.h"
#include "core/log.h"

namespace aco {

namespace {

using PFN_GetProcAddress = FARPROC(WINAPI*)(HMODULE, LPCSTR);
PFN_GetProcAddress g_orig_gpa_dxgi = nullptr;
PFN_GetProcAddress g_orig_gpa_d3d11 = nullptr;
PFN_vkGetInstanceProcAddr g_real_gipa = nullptr;
PFN_vkCreateDevice g_real_create_device = nullptr;
VkInstance g_instance = VK_NULL_HANDLE;

const char* const kWanted[] = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};

VkResult VKAPI_CALL hk_vkCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
                                      const VkAllocationCallbacks* alloc, VkDevice* out) {
  auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
      g_real_gipa(g_instance, "vkEnumerateDeviceExtensionProperties"));
  std::vector<VkExtensionProperties> avail;
  if (enumerate) {
    uint32_t n = 0;
    enumerate(pd, nullptr, &n, nullptr);
    avail.resize(n);
    enumerate(pd, nullptr, &n, avail.data());
  }
  std::vector<const char*> exts(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
  for (const char* want : kWanted) {
    bool enabled = false, supported = false;
    for (const char* e : exts) enabled |= strcmp(e, want) == 0;
    for (const auto& a : avail) supported |= strcmp(a.extensionName, want) == 0;
    if (!enabled && supported) exts.push_back(want);
    ACO_LOG("fg: vkCreateDevice: %s %s", want, enabled ? "already enabled" : supported ? "added" : "NOT SUPPORTED");
  }
  VkDeviceCreateInfo copy = *ci;
  copy.enabledExtensionCount = static_cast<uint32_t>(exts.size());
  copy.ppEnabledExtensionNames = exts.data();
  return g_real_create_device(pd, &copy, alloc, out);
}

PFN_vkVoidFunction VKAPI_CALL hk_vkGetInstanceProcAddr(VkInstance instance, const char* name) {
  if (instance) g_instance = instance;
  if (name && strcmp(name, "vkCreateDevice") == 0) {
    g_real_create_device = reinterpret_cast<PFN_vkCreateDevice>(g_real_gipa(instance, name));
    return g_real_create_device ? reinterpret_cast<PFN_vkVoidFunction>(hk_vkCreateDevice) : nullptr;
  }
  if (name && strcmp(name, "vkGetInstanceProcAddr") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(hk_vkGetInstanceProcAddr);
  return g_real_gipa(instance, name);
}

FARPROC wrap(FARPROC real, LPCSTR name) {
  if (!real || IS_INTRESOURCE(name) || strcmp(name, "vkGetInstanceProcAddr") != 0) return real;
  g_real_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(real);
  return reinterpret_cast<FARPROC>(hk_vkGetInstanceProcAddr);
}
FARPROC WINAPI hk_gpa_dxgi(HMODULE m, LPCSTR name) { return wrap(g_orig_gpa_dxgi(m, name), name); }
FARPROC WINAPI hk_gpa_d3d11(HMODULE m, LPCSTR name) { return wrap(g_orig_gpa_d3d11(m, name), name); }

} // namespace

void fg_hook_dxvk_device_creation(HMODULE dxvk_dxgi, HMODULE dxvk_d3d11) {
  if (dxvk_dxgi && !g_orig_gpa_dxgi)
    iat_hook(dxvk_dxgi, "KERNEL32.dll", "GetProcAddress", reinterpret_cast<void*>(hk_gpa_dxgi),
             reinterpret_cast<void**>(&g_orig_gpa_dxgi));
  if (dxvk_d3d11 && !g_orig_gpa_d3d11)
    iat_hook(dxvk_d3d11, "KERNEL32.dll", "GetProcAddress", reinterpret_cast<void*>(hk_gpa_d3d11),
             reinterpret_cast<void**>(&g_orig_gpa_d3d11));
}

} // namespace aco
