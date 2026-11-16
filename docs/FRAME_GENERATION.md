# DLSS Frame Generation for a D3D11 game under Proton

Status: **blocked on the Vulkan route; recommend the D3D12 (vkd3d-proton) route next.**
Everything up to the generated image works: DLSS-G is created and evaluated on DXVK's VkDevice every
frame (~2.1–2.7 ms GPU at 768x480), reports interpolation enabled, copies the real frame to
`OutputReal`, and the mod presents generated + real frames. But DLSS-G's *generation kernels* never
write their output — not into DXVK's images, and not into an image created directly with Vulkan
either (see "Root-cause analysis"). On this stack (Windows `nvngx_dlssg.dll` → the driver's Wine NGX
core → winevulkan) the kernel path doesn't produce frames.

## Root-cause analysis (2026-11-16)

Black-box tests (all on t3, `[fg]` debug knobs below):

| Test | Result |
|---|---|
| `OutputReal` provided | written; **byte-identical to the input frame** (left/right same-frame compare, RMSE 0, also while the camera moves) |
| `OutputInterpolated` (DXVK texture, magenta at creation) | never written |
| swap which texture is passed as OutputReal / OutputInterpolated | the OutputReal one is written, the OutputInterpolated one never — parameter, not texture |
| `OutputInterpolated` = image created with plain Vulkan (cleared magenta per frame, read back) | never written |
| `OutputDisableInterpolation` | 0x01 on the reset frame, then 0x00 (interpolation enabled) |
| NVX image-view handles of our views | valid (storage handles for UAV-capable images, sampled for all) |
| GPU time of our DLSS-G submission | 2.1 ms (count=1) / 2.7 ms (count=2): DLSS-G does run work |
| Reflex on/off, HUD-less colour, camera basis, `MvecInvalidValue`, DLSSG.Width/Height, BackbufferFrameID, RefreshRate, `StreamlineMode` (eval and create), `AsyncCreateEnabled=0`, swapchain vs copy as input, dev DLL + indicator | no change |

Reverse engineering `nvngx_dlssg.dll` 310.9.16 (dev build; Ghidra headless scripts in the session
scratchpad, RTTI names from the binary):

- Feature creation builds two endpoints: `EndpointAlgorithm` (generation) and
  `EndpointPassthrough`; a per-evaluation check picks one. Passthrough copies the backbuffer into
  **both** outputs — since `OutputInterpolated` stays untouched we are on the `EndpointAlgorithm`
  path.
- `EndpointCoreInputs::ComputeAndValidateTimeFactor` defines the call sequence: the **last
  `MultiFrameIndex` of a `MultiFrameCount` sequence is the real frame** (unless an internal flag
  that nothing in this build sets), earlier indices are generated at `t = index / (count + 1)`.
  So `count=1` (the SDK's documented "2x") only ever makes the real-frame call. The mod now uses
  `count=2` (index 1 generated, index 2 real) — accepted by the DLL on this GPU (multi-frame
  capable), GPU time rises accordingly, but the output is still not written.
- In `EndpointAlgorithm::Evaluate` the fallback that copies the backbuffer into
  `OutputInterpolated` is disabled right before the generation step, which then writes
  `OutputInterpolated` from the network's compute kernels (cubins launched with
  `vkCmdCuLaunchKernelNVX`, resources via `VK_NVX_image_view_handle`). Those writes are what never
  land. Copies (vkCmdCopyImage) and buffer fills from the same evaluation do land.

Kernel trace ([fg] trace_cu=1, hooks nvngx_dlssg.dll's own GetProcAddress, which it uses to
resolve Vulkan from vulkan-1.dll): all CUDA modules/functions are created successfully
(`Kernel_BlendCandidatesFused`, `Kernel_DL1Net_*`, `Kernel_EstimateIntermMvecsScatter`, …) and ~110
kernels are launched per evaluation (`vkCmdCuLaunchKernelNVX`, parameters passed as a
CU_LAUNCH_PARAM_BUFFER_POINTER "extras" block, the same form DXVK uses for DLSS-SR). So the kernels
run; their image accesses go nowhere.

The snippet uses **64-bit image view handles** ("Enabling texmode_raw" →
`vkGetImageViewHandle64NVX`/`vkGetImageViewAddressNVX`). Through winevulkan these return the same
small values as the 32-bit `vkGetImageViewHandleNVX` (e.g. 0x3a for both), which looks wrong for a
64-bit handle. Hiding the 64-bit entry points from the snippet ([fg] hide_handle64=1) doesn't make
it fall back: evaluation then fails with `PlatformError` ("vkGetImageViewHandle64NVX() function not
available") — raw texture mode is mandatory. **Prime suspect: 64-bit image view handles obtained
through winevulkan are not valid for kernels launched through it** (DXVK's working DLSS-SR path uses
32-bit handles). (Correction: an earlier commit message claimed hide_handle64 produced generated
frames — those screenshots were real frames presented after the failed evaluation; `tools/fg-shots`
now refuses to run when Frame Generation isn't presenting.)

Conclusion: the Windows DLSS-G snippet's *Vulkan* kernel path does not work through the driver's
Wine NGX core + winevulkan (whereas DLSS-SR's kernels work through DXVK's own NVX interop, and
DLSS-G is known to work under Proton through **D3D12/vkd3d-proton**, and natively on Linux).
Not checked: whether winevulkan's `vkCmdCuLaunchKernelNVX` thunk handles the launch form NGX uses
(e.g. `pExtras` buffer-pointer parameters) — a winevulkan trace (`WINEDEBUG=+vulkan`) of one
evaluation would settle it.

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

## Next steps (recommended)

1. **Pin down the 64-bit handle problem** (launch marshalling looks fine, see above): compare
   `vkGetImageViewHandle64NVX` through winevulkan with a native Linux Vulkan call on the same kind
   of image (a tiny native test program), and read winevulkan's thunk for it in the Proton source.
   If the thunk is at fault, the fix belongs in Wine/Proton (or our hook could call the native
   driver directly).
2. Otherwise switch to the **D3D12 route**: a vkd3d-proton D3D12 device in-process, DLSS-G via NGX
   D3D12 (the path Proton officially supports), frames shared from DXVK with
   `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` textures + a shared fence, presenting through DXVK's
   swapchain as now. Everything else built here (inputs, present takeover, pacing, Reflex
   handling, debug knobs) carries over.

## Open issues (from the first prototype)

1. **Generated image never written** (current blocker, see root-cause analysis). DLSS-G writes the output (verified with a
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
