# DLSS Frame Generation for a D3D11 game under Proton

Status: **pipeline working end to end, image not yet correct.** DLSS-G is created and evaluated on
DXVK's VkDevice every frame, its GPU work runs (it writes its "disable interpolation" flag = 0 and
writes the output image), and the mod presents generated + real frames (e.g. 43.5 + 43.5 fps,
pacing off). But the generated image DLSS-G writes is **all zeros**. See "Open issues".

## Why it is not straightforward

DLSS Frame Generation (DLSS-G, NGX feature `NVSDK_NGX_Feature_FrameGeneration`, `nvngx_dlssg.dll`)
has **no D3D11 interface**: the SDK (`third_party/DLSS-310.9.16`, `nvsdk_ngx_helpers_dlssg_*.h`)
only provides D3D12 and Vulkan create/evaluate helpers. NVIDIA's official integration path is
Streamline (D3D12/Vulkan), which also owns the swapchain and frame pacing.

## Routes considered

| Route | Verdict |
|---|---|
| **(b) NGX Vulkan on DXVK's own VkDevice** | **Chosen. Works.** Under Proton the game's D3D11 already runs on Vulkan (DXVK). DXVK exposes its VkInstance/VkPhysicalDevice/VkDevice/VkQueue and the VkImage behind every D3D11 texture (`IDXGIVkInteropDevice`/`IDXGIVkInteropSurface`). We record NGX's DLSS-G evaluation into our own command buffer and submit it on DXVK's queue (`FlushRenderingCommands` + `LockSubmissionQueue`), so no cross-device resource sharing or cross-API synchronisation is needed. |
| (a) vkd3d-proton D3D12 device + shared resources | Not needed. Would require D3D11<->D3D12 shared textures across two VkDevices (external memory + shared fences under Wine), a second swapchain on the game window, and Streamline-like pacing. Strictly more moving parts than (b). |
| NGX D3D11 `CreateFeature(FrameGeneration)` | Not offered by the SDK; not pursued. |

## What the Proton/driver stack provides (verified)

- The driver's Wine NGX core (`/usr/lib/nvidia/wine/_nvngx.dll`, NGXCore registry path) exports the
  full Vulkan NGX API (`NVSDK_NGX_VULKAN_Init`, `CreateFeature1`, `EvaluateFeature`, ...).
- `NVSDK_NGX_VULKAN_Init` on DXVK's device: **Success**; `FrameGeneration.Available = 1`
  (min driver 520). The first init after a driver update takes ~30 s (cache build), later ~0.5 s.
- `NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(FrameGeneration)` returns
  `NotImplemented` in this build. DXVK's device already enables `VK_NVX_binary_import`,
  `VK_NVX_image_view_handle`, `VK_NV_low_latency2`, external memory/semaphores and timeline
  semaphores, but **not `VK_KHR_push_descriptor`**, which NGX's Vulkan kernels need (see below).
  `CreateFeature1`/`EvaluateFeature` return Success either way — without the extension the
  evaluation just does nothing.
- `nvngx_dlssg.dll` from the SDK is loaded from the game directory (passed via
  `NVSDK_NGX_FeatureCommonInfo::PathListInfo`); `scripts/package` ships it.
- DXVK keeps images in `VK_IMAGE_LAYOUT_GENERAL` (it enables `VK_KHR_unified_image_layouts`), so
  NGX can use them without layout transitions; full memory barriers before/after our work order it
  against DXVK's submissions on the same queue.

## Inputs (src/fg/fg.cpp, inputs from src/dlss/upscaler.cpp)

| DLSS-G input | Source |
|---|---|
| Backbuffer | copy of the game's swapchain image at Present (final frame incl. UI) |
| Depth | the engine's raw reversed-Z depth copy (R32F, render res) — `DepthInverted=1` |
| Motion vectors | our DLSS-SR motion vectors (RG16F, render res, UV units, prev−cur), `MvecScale=1`, `CameraMotionIncluded=1` |
| Jitter | the engine's sub-pixel jitter (pixels) |
| Camera matrices | **identity for now** (see open issues) |
| HUD-less colour / UI | not yet provided |
| Output | our own display-size texture (`OutputInterpolated`) |

## Present model

When FG is active the mod takes over the game's `Present`: copy the real frame aside, evaluate
DLSS-G (interpolating between the previous and the current real frame), copy the generated frame
into the backbuffer and present it, wait half the measured real-frame interval, then present the
real frame (`[fg] pacing=1`). This adds ~half a real frame of latency, which is inherent to
interpolation (Reflex is on to compensate).

## DXVK's device needs `VK_KHR_push_descriptor`

NGX's Vulkan "cubin" kernels bind resources with push descriptors. DXVK doesn't enable
`VK_KHR_push_descriptor`, so nvngx_dlssg logged `Failed to load VK device function:
vkCmdPushDescriptorSetKHR` and every evaluate silently did nothing while returning Success.
`src/fg/vk_device_ext.cpp` fixes this from inside our DLL: DXVK (dxgi.dll / d3d11.dll) resolves
`vkGetInstanceProcAddr` from winevulkan with `GetProcAddress`; we hook that import (IAT), wrap
`vkCreateDevice` and append the extension. Verified: all three of the game's devices get it and
DLSS-G's GPU work then executes.

## Debugging aids (aco_dlss.ini `[fg]`)

- `ngx_log=1|2`: sets `__NGX_LOG_LEVEL`, `__NGX_ENABLE_OVERRIDE_LOG_PATH`,
  `__NGX_LOG_PATH_OVERRIDE` (env vars the driver's Wine NGX core reads) at DLL load; NGX then
  writes `nvngx.log`, `nvngx_dlssg_*.log` etc. into the out dir. (The `LoggingInfo` callback in
  `NVSDK_NGX_FeatureCommonInfo` is ignored by this NGX build.)
- `show=generated|real|both`: present only one kind of frame, so screenshots capture it.
- `debug_fill=1`: create the output texture magenta. Don't use a D3D11 UAV clear for this: DXVK
  defers clears until the image's next D3D11 use (our copy *after* NGX), which overwrites NGX's
  output and looks like "NGX never writes".
- OutputDisableInterpolation is read back (host-visible VkBuffer per ring slot) and counted in the
  perf line (`dlssg-disabled`).
- `bb_format_dxgi=1`: pass the DXGI format number at create. Result: NGX then doesn't write the
  output at all, so on Vulkan the **VkFormat** numbering is the right one.

## Open issues

1. **Generated image is all zeros** (current blocker). DLSS-G writes the output (verified with a
   magenta-initialised output texture → black) and reports interpolation enabled. Next checks:
   pass `OutputReal` to see whether NGX reads the backbuffer at all (if OutputReal is also black,
   input reads are failing — e.g. image-view handle/descriptor type of DXVK images via
   `VK_NVX_image_view_handle`); verify our submission really follows DXVK's copy of the
   backbuffer (FlushRenderingCommands semantics); provide real camera matrices; compare with how
   Streamline drives DLSS-G (param set incl. undocumented `DLSSG.MustCallEval`,
   `DLSSG.UseReflexMatrices`, `DLSSG.StreamlineMode`, `DLSSG.FlipMetering`).
2. **Reflex + two Presents per game frame → GPU hang** (`VK_ERROR_DEVICE_LOST` within ~20 s with
   Reflex on). DXVK's Reflex tracker maps one app frame to one present; with the generated present
   in between it keeps remapping and flipping in/out of markerless mode. Needs marker handling per
   present (or Reflex markerless) while FG is active. Until then FG requires `[reflex] mode=off`.
3. **Pacing on the game's render thread halves the base frame rate** (`pacing=1`: the wait for
   the mid-point is measured as part of the frame interval → converges to 2× frame time). Needs a
   dedicated present thread (with DXVK's immediate context made thread-safe via
   `ID3D10Multithread`), as Streamline does. `pacing=0` presents back to back (no throughput
   loss, but uneven frame spacing).
4. Camera matrices are identity; HUD-less colour/UI not provided (UI will be interpolated).
5. CPU cost of recording the evaluation: ~0.4–4 ms per frame on the render thread.
6. First NGX Vulkan init after a driver change takes ~30 s on the render thread → do it async.

## Lifetime rule learned the hard way

DXVK only tracks its own GPU work. Releasing a D3D11 texture right after submitting our Vulkan
work that uses it lets DXVK destroy the VkImage while our command buffer is in flight →
`VK_ERROR_DEVICE_LOST` a few seconds later (seen when the engine recreated render targets on a
level load). Every submission now holds references to its textures until its fence signals, and
image views are cached per D3D11 texture (holding a reference) and pruned when unused.
