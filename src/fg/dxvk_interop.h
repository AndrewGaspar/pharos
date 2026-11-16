#pragma once
// DXVK's Vulkan interop interfaces (subset of dxvk/src/dxgi/dxgi_interfaces.h, zlib license).
// Every D3D11 device/texture created by DXVK implements these; they expose the underlying
// VkDevice/VkQueue/VkImage so we can run Vulkan work (NGX Frame Generation) on the same device.
#include <d3d11.h>
#include <vulkan/vulkan.h>

struct IDXGIVkInteropDevice;

MIDL_INTERFACE("5546cf8c-77e7-4341-b05d-8d4d5000e77d")
IDXGIVkInteropSurface : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetDevice(IDXGIVkInteropDevice** ppDevice) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(VkImage* pHandle, VkImageLayout* pLayout,
                                                       VkImageCreateInfo* pInfo) = 0;
};

MIDL_INTERFACE("e2ef5fa5-dc21-4af7-90c4-f67ef6a09323")
IDXGIVkInteropDevice : public IUnknown {
  virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance* pInstance, VkPhysicalDevice* pPhysDev,
                                                  VkDevice* pDevice) = 0;
  virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue* pQueue, uint32_t* pQueueFamilyIndex) = 0;
  virtual void STDMETHODCALLTYPE TransitionSurfaceLayout(IDXGIVkInteropSurface* pSurface,
                                                         const VkImageSubresourceRange* pSubresources,
                                                         VkImageLayout OldLayout, VkImageLayout NewLayout) = 0;
  virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
  virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
  virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
};

__CRT_UUID_DECL(IDXGIVkInteropDevice, 0xe2ef5fa5, 0xdc21, 0x4af7, 0x90, 0xc4, 0xf6, 0x7e, 0xf6, 0xa0, 0x93, 0x23);
__CRT_UUID_DECL(IDXGIVkInteropSurface, 0x5546cf8c, 0x77e7, 0x4341, 0xb0, 0x5d, 0x8d, 0x4d, 0x50, 0x00, 0xe7, 0x7d);
