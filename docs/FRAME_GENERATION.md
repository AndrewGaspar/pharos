# DLSS Frame Generation for a D3D11 game under Proton

Status (2026-11-16): **working through the D3D12 route (`[fg] mode=d3d12`)**: genuinely
interpolated frames in game. The Vulkan route below (`mode=on`) still never generates and is kept
for reference.

## D3D12 route (`src/fg/fg_d3d12.cpp`, `[fg] mode=d3d12`)

DLSS-G runs through NGX's **D3D12** API on a vkd3d-proton device of our own, next to the game's
DXVK D3D11 device:

1. **Device**: `LoadLibrary("d3d12.dll")` (the prefix's vkd3d-proton) and `D3D12CreateDevice` on the
   DXGI adapter of the game's D3D11 device. Created with `VKD3D_CONFIG=descriptor_heap` (appended
   before our device's creation, `[fg] descriptor_heap=1`): in vkd3d-proton 3.1's default
   descriptor-buffer mode, `NvAPI_D3D12_GetCudaIndependentDescriptorObject` fails (`E_INVALIDARG`,
   no image view) for two raw buffer UAVs DLSS-G creates (R32 typeless, 256 KB); heap mode resolves
   any heap slot through `vkGetDeviceCombinedImageSamplerIndexNVX`. (Generation worked without it
   in the synthetic test, but heap mode removes the errors.)
2. **Sharing**: D3D11 textures created with `D3D11_RESOURCE_MISC_SHARED_NTHANDLE | SHARED`, NT
   handle → `ID3D12Device::OpenSharedHandle`; one `ID3D11Fence` (`D3D11_FENCE_FLAG_SHARED`) opened as
   `ID3D12Fence`. Verified first with `tools/interop-test/interop_test.cpp` (D3D11 writes → D3D12
   copies → D3D11 reads back, all 32768 pixels correct).
3. **Per frame**: D3D11 copies backbuffer, depth (R32_TYPELESS → shared R32_FLOAT), motion vectors
   and the HUD-less DLSS output into the shared textures, `Signal(v)`, flush; the D3D12 queue
   `Wait(v)`, evaluates DLSS-G, `Signal(v+1)`. **The D3D11 queue never waits for D3D12**
   (`[fg] sync=cpu`, default): vkd3d-proton resolves a queue wait on a *shared* fence on the CPU in
   its submission thread (`d3d12_command_queue_wait_shared`, to avoid wait-before-signal), so a
   D3D11 GPU wait on the D3D12 signal (`sync=gpu`, the first version) put a CPU round trip into the
   D3D11 queue every frame and everything the game submitted after it waited: at 2560x1600 that
   halved the game's frame rate with the GPU 40–60 % busy. Instead frames alternate between two
   texture sets; the present thread waits on the CPU for the D3D12 fence before copying a generated
   frame out, and the game thread waits on the CPU only if the set it is about to overwrite (two
   frames old) is still being evaluated.
4. **NGX**: the core's `NVSDK_NGX_D3D12_Init` export takes *(app id, path, device, version,
   feature info)* — the version before the feature info, unlike the SDK header's inline overload
   (passing them in header order gives "installed NGX API is older than the one used by client").
5. **Presenting** (`[fg] present_thread=1`, default): the game renders into a *proxy backbuffer*
   (our `IDXGISwapChain::GetBuffer` hook returns our texture; the swapchain's own backbuffer is only
   written right before each real Present). A present thread presents the generated frame, waits
   half a (real) frame interval, then presents the real frame, while the game already renders the
   next frame. The game's blt-model swapchain has a single backbuffer, so without the proxy the
   game would overwrite it before the deferred real present. The D3D11 immediate context is shared
   by both threads → multithread protection (`ID3D10Multithread`). **DXVK's
   `SwapDeviceContextState` doesn't take that lock** (all other context methods do), so the DLSS-SR
   upscaler holds it explicitly around its job; without that the game crashed within seconds in
   DXVK's CS chunk emission (null chunk, two threads emitting). That lock is an unfair spinlock, so
   while the present thread copies + presents, the render thread waits at its next context hook
   (`core/ctxgate.h`, `[fg] present_gate`). Present jobs queue (at most two outstanding, one per
   texture set): the game doesn't wait for the previous frame's pacing, and a job superseded before
   it starts presents only its real frame (counted as "generated frames dropped"). `present_thread=0` presents both
   inline with the pacing wait on the render thread (which then settles at frame time = 2x work,
   halving the base rate); `=2` uses the proxy with inline presents (a bisecting aid).
6. **Init** (D3D12 device + NGX, ~1 s) runs on its own thread; presents stay normal until ready.

Verification (`[fg] verify=1`: every 2 s reads back previous real, generated and current real
frame, logs their differences and writes `out/fgverify/*.ppm`). Running in Discovery Tour:
`|gen-prev| 10.5, |gen-cur| 10.5, |prev-cur| 15.0` (mean abs, 0..255) — the generated frame differs
from both neighbours symmetrically and is not a blend (that would give 7.5 + 7.5 = 15); the images
show the scene (background props, the character's arm) between the two real positions, sharp, no
double images. Synthetic check (`tools/interop-test/dlssg_d3d12_test.cpp`): a square moving 16 px
per frame is generated at x = 226 between real 220 and 236 (count 1 and 2).

DLSS-G on D3D12 costs ~1.55 ms GPU per evaluation at 768x480 (count=1). `count=1` (one generated
frame per real frame) generates on D3D12; the "last index is the real-frame call" reading from the
Vulkan-route reverse engineering below doesn't hold here.

### D3D12 route measurements (2026-11-16)

`tools/gpu-exclusive t3 tools/latency-bench t3 <label> upscaler.render_scale=0.5 ...` — DLSS
Performance (preset M), Reflex on, t3 at 768x480 ultra-high, Discovery Tour, 40 mouse-look
injections each, fresh launch per configuration:

| | FG off | FG on (`mode=d3d12`, count 1, present thread) |
|---|---|---|
| real (game) frames/s | 139.0 | 110.1 |
| presented frames/s | 139 | ~220 (110 real + 110 generated) |
| input → frame done (GPU), median / p10 / p90 ms | 26.8 / 23.5 / 30.1 | 35.3 / 29.2 / 54.9 |
| generated frame presented after the game's Present | – | +1.3–2.1 ms |
| real frame presented after the game's Present | – | +5.6–6.9 ms (half-interval pacing) |
| DLSS-G GPU time per frame | – | 1.55 ms |

So the base rate drops by ~21 % (the DLSS-G evaluation plus copies on a GPU-bound frame), it is no
longer halved; output is ~1.6x FG-off. Input latency to the real frame grows by roughly
8.5 ms (frame done) + 6 ms (pacing delay) ≈ 15 ms at the median, with a long p90 tail (55 ms vs
30 ms) — the present thread sometimes blocks the game (6–12 joins per 2 s at ~5–9 ms). At the
target 2560x1600 the DLSS-G cost per frame is higher in absolute terms but smaller relative to the
frame; not measured yet (instances run at 768x480).

### At 2560x1600 (2026-11-16, after the `sync=cpu` fix)

t1, 2560x1600, DLSS Performance (render 1280x800), Reflex on, Ultra High, gpu-exclusive,
latency-bench (40 mouse-look injections); FG-off runs before and after the FG-on run:

| | FG off (2 runs) | FG on (`mode=d3d12`, defaults) | FG on, first version (`sync=gpu`, coordinator's run) |
|---|---|---|---|
| game (real) frames/s | 110.2 / 115.3 | 78.5 | 58 |
| presented frames/s | 110 / 115 | 159.5 (79.7 + 79.7) | 117 |
| input → frame done, median (p10–p90) | 35.6 / 34.5 ms (32–45) | 54.7 ms (47–63) | 85.1 ms |
| generated / real frame presented after the game's Present | – | +21.3 / +27.6 ms | +18.6 / +27.1 ms |
| DLSS-G GPU time (wall, overlapping the game's work) | – | ~5 ms | 2.7 ms |
| generated frames dropped | – | 0 | – |

How the collapse was found (`tools/fg-diag`, per-step timing of the present thread, `[fg] skip`):
the generated frame's `Present` blocked ~8 ms and the GPU was 40–60 % busy; `skip=eval` (fence
round trip, no DLSS-G) was just as slow, `skip=d3d12` (no fences; copies + two presents) ran at
115 fps with the GPU at 95 %. Presentation itself is not capped: `present_rate.exe` (a trivial
frame, same swapchain shape) presents ~10000/s at 2560x1600 through DXVK + headless gamescope.
Reflex off, `pacing=0`, `present_thread=0`, a higher frame latency or more swapchain buffers did
not help; removing the D3D11 GPU wait did.

Latency: the base rate drops ~30 % (DLSS-G at 2560x1600 plus copies on a GPU-bound frame) and the
real frame reaches the screen later — the generated frame needs the game's frame finished on the
GPU plus DLSS-G (~19 ms after the game's Present), the real frame follows half a frame later.
Next steps to reduce it: Reflex/latency markers that describe the FG presents (DXVK logs frame-ID
mismatches, so its low-latency pacing doesn't see our presents), presenting the generated frame as
soon as DLSS-G finishes instead of polling from the present thread, fewer full-resolution copies
(the real-frame copy could reuse the shared input texture).

Verification at 2560x1600 (verify=1, running): `|gen-prev| 9.5, |gen-cur| 10.4, |prev-cur| 18.8`,
generated strictly between its neighbours in 95 % of moving samples; images coherent.

Checking on the user's display path (Hyprland 165 Hz, MangoHud, not gamescope): run
`tools/interop-test/present_rate.exe 2560 1600 5` with the same Proton to get the raw present
ceiling, then play with `[fg] mode=d3d12` and read `perf: fg12 present thread steps` in
aco_dlss.log — a generated/real `Present` above ~1 ms means the display path throttles presents;
MangoHud counts both presents (expect ~2x the game rate up to the refresh rate).

### Live toggle (F9, `[fg] toggle_key`, initial state `[fg] enabled`)

With `mode=d3d12` the D3D12 device, NGX and the present thread stay alive; the hotkey (edge-detected
like F10) flips Frame Generation. Off: no evaluation, no generated present, no pacing — each frame
is copied from the proxy backbuffer and presented on the game thread as with `mode=off`. On again:
the first evaluation passes Reset=1. Reflex frame IDs are per game Present, so they stay monotonic
across toggles. 2560x1600, t1, DLSS Performance, Reflex on, latency-bench:

| | `mode=off` | `mode=d3d12`, FG on | `mode=d3d12`, FG toggled off (`enabled=0`) |
|---|---|---|---|
| game fps | 120.5 | 80.1 (75.8 + 75.8 presents/s) | 117.8 |
| input → frame done, median (p10–p90) | 35.6 ms (32.4–39.6) | 55.4 ms (47.4–63.9) | 36.1 ms (33.6–40.1) |

`tools/fg-toggle-test t1 6`: six F9 presses while running: no hang or crash, no exceptions,
generated frames back after every re-enable (~80 + 80 presents/s, verify: generated strictly
between its neighbours), each re-enable logged with one reset.

Still open on this route: latency (above); HUD-less/UI handling (the HUD-less input is the DLSS
output, the UI is interpolated with the scene).

### Frame pacing and generated-frame timing (2026-11-17)

User report: spinning the camera with FG on looked jumpy at 2560x1600 / 165 Hz (Hyprland), while
MangoHud's frametime graph was flat.

**Measurement** (`tools/fg-trace <inst> <label>`): `tools/spin.py` turns the camera at a constant
rate (XTEST mouse-look, ~5000 px/s on screen, ~60 px per real frame), the mod's present trace
(`touch <out>/fgtrace-start`, `[fg] trace_presents` / `trace_shots`) records the QPC time of every
Present call/return plus the game's Present arrival, and copies a 2560x100 band of exactly what was
presented (in present order) for the first 120 presents. `tools/fg-pacing.py` then measures:

- the horizontal image shift between consecutive presented frames (phase correlation on two
  sub-bands left/right of the player character); for every (real N-1, generated N, real N) the
  *shift ratio* d(R<sub>N-1</sub>,G<sub>N</sub>) / d(R<sub>N-1</sub>,R<sub>N</sub>), 0.5 = midpoint;
- present-call intervals by transition (G→R, R→G), Present durations, arrival intervals;
- *position residual*: the cumulative image position against present time (a constant-rate spin is
  a straight line) minus a local linear fit, in px: what the eye sees as judder on a VRR display.
  Also simulated for a fixed 165 Hz display (`mailbox` = newest frame per vblank, as a
  compositor does with vsync off; `fifo` = every frame in order).

All runs: t2, 2560x1600, DLSS Performance, Ultra High, Discovery Tour Siwa, only this game on the GPU.

| configuration | shift ratio median (p10–p90), outside [0,1] | present-interval sd; G→R / R→G mean (p10–p90) ms | position residual, mean / p90 px (VRR) | real fps |
|---|---|---|---|---|
| **before**: one shared fence, `mv_units=uv`, identity camera, `pace=ready` | **−0.69** (−2.24–0.51), **58 of 107** | 2.46; 7.13 (5.57–11.0) / 4.63 (0.45–6.38) | **40.0 / 74.1** | 85 |
| separate completion fence | 0.497 (0.477–0.518), 2 of 114 | 2.6; 8.02 / 3.73 | 10.4 / 19.7 | 85 |
| + `mv_units=px` | 0.501 (0.486–0.516), 0 of 117 | 2.6; 8.04 / 3.75 | 11.8 / 22.1 | 85 |
| + `camera=engine` (`pace=ready`) | 0.500 (0.488–0.518), 0 of 118 | 2.17; 7.03 / 4.83 | 9.8 / 21.2 | 85 |
| + `pace=smooth` (**new defaults**) | 0.500 (0.491–0.522), 1 of 118 | **0.46**; 5.89 (5.46–6.32) / 5.81 (5.17–6.47) | **6.7 / 12.5** | 85.5 |
| `pace=arrival` (alternative) | 0.499 (0.481–0.505), 0 of 118 | 2.35; 6.49 / 5.29 | 9.7 / 21.0 | 85 |
| FG off (`enabled=0`), for reference | – | 1.14; N→N 8.42 | 4.2 / 8.1 | 119 |

(The `camera=engine` row and `pace=arrival` were measured before the real-present timing change
below; the final `pace=smooth` row includes it.)

**What was wrong**

1. **Generated frames shown before DLSS-G had written them (the jumpiness).** The D3D11 queue
   (signal "inputs copied", value 2N+1) and the D3D12 queue (signal "evaluation N done", 2N) used
   the *same* shared fence. Whenever the game's next frame reached its signal before DLSS-G
   finished, the fence jumped past 2N, the present thread's "evaluation done" check passed
   immediately and it copied the output texture of the set while DLSS-G was still about to write
   it: the frame presented as "generated" was the generated frame from two frames earlier. In the
   trace these show up as the image jumping *back* by ~1.5–2 real frames (+110 px, then −170 px)
   at every generated present — about half of all generated frames during a spin. (The D3D12
   signal then also moved the timeline backwards, which Vulkan timeline semaphores don't allow; the
   two GPU hangs seen during this work under heavy VRAM pressure may be related.) The present
   *times* were fine, so MangoHud's graph was flat. Fix: a separate completion fence created by
   D3D12 (`done12`; also opened in D3D11 as `done11` for `sync=gpu` and feature creation). The sync
   model is otherwise unchanged (`sync=cpu`: D3D11 never waits for D3D12).
2. **Motion vector units.** At the NGX level DLSS-G expects `MvecScale` to convert the vectors to
   *pixels of the motion-vector texture* (the FG programming guide's "Motion Vectors" section; the
   struct comment "normalize to [-1,1]" is Streamline's convention). With our UV-unit vectors and
   scale 1, DLSS-G saw motion of ~0.05 px, i.e. none, and interpolated from optical flow alone. The
   synthetic test shows it: a square moving 64 px per frame comes out at the right place with
   `MvecScale = width` (275.5 between 252 and 316) and is lost entirely with scale 1, 0 or negative
   (`dlssg_d3d12_test.exe <dir> 1 "" 4 64 <scale>`); at 16–48 px optical flow alone copes. In game
   (≤ 60 px per real frame) the spread of the ratio shrinks (sd 0.072 → 0.025, outliers 2 → 0).
   `[fg] mv_units=px` (default) passes `mv_scale x render size`.
3. **Present timing.** The real frame waited half an interval from when the generated `Present`
   *returned*; under headless gamescope that call blocks up to ~5–6 ms (p90 4.7 ms), so G→R
   stretched to 10–12 ms and the next generated frame followed the real one almost at once (R→G
   p10 0.4–0.9 ms). `pace=smooth` (default) times the real frame from the generated Present's
   *call*, and holds the generated frame until half an interval after the previous real present,
   minus `pace_relax` (0.5) of how early DLSS-G finished (so a delay never sticks). Present
   intervals go from sd 2.3–2.5 ms to 0.46 ms; the latency cost is ~+0.6 ms on the generated present
   (19.0 vs 18.4 ms after the game's Present) and none on the real one (24.9 vs 25.4 ms).
   `pace=arrival` (fixed lag after the game's Present) is kept as an option; it was worse.

**Camera** (`[fg] camera=engine`, default): projection, view and previous view-projection come from
the TAA pass's `PassParams` (`viewToWorld` c4–c6, `RevProjParams.ClipXYZToViewPos` c57–c60) and
`InstanceParams.worldViewProjPrevFrame` (c12–c15); passed transposed (DLSS-G: row-major,
post-multiplied), basis from `viewToWorld`, near 0.1, far ~59900 (infinite reversed Z, from the
projection), vertical FOV 45.2°, aspect 1.6. `ClipXYZToViewPos` carries no TAA jitter (checked:
0.000 px in the projection while the engine jitter is ±0.5 px), `camera_unjitter=1` strips it if it
ever does. Reading those constant buffers by copying every Map of their pooled sizes (2048 / 512
bytes) dropped the game to 15 fps (reads from write-combined memory); `cbshadow_watch_size_ptr`
remembers only the mapped pointer (DXVK keeps dynamic buffers mapped) and reads 208 bytes per
frame: no measurable cost. Measured effect on the shift ratio: none (0.500 either way); kept because
it is what DLSS-G is specified to get.

**Latency / fps** (latency-bench, 40 mouse-look injections, t2, same session conditions):

| | FG off | FG on, old settings (fence fix only) | FG on, new defaults |
|---|---|---|---|
| game (real) fps | 125.9 | 87.1 (174 presents/s) | 86.2 (172 presents/s) |
| input → frame done, median (p10–p90) | 32.6 ms (29.6–37.3) | 51.3 ms (46.5–58.6) | 51.7 ms (48.4–58.2) |

The changes cost no fps or latency. The "old settings" column already includes the fence fix, and
the traces taken before the fix also ran at 85 real fps. The earlier 76–80 real fps / ~55 ms
(measured on t1, above) against 86 fps / ~51.5 ms here is variance between instances and sessions,
not an effect of these changes. Re-validate after the fence change: the F9 toggle, resize / alt-tab
(feature re-creation waits on the completion fence) and `sync=gpu` (needs the completion fence
opened in D3D11; logged as `fg12: fences: ...`).

**Fixed-refresh display (the user's Hyprland 165 Hz without VRR).** The trace's display models show
what an uneven present cadence does on a fixed refresh: with the old pacing (`pace=ready`) ~10 %
of vblanks repeat a frame and ~10 % of frames are never shown in the mailbox model at 170
presents/s; with `pace=smooth` 3 % / 6.5 % (most of what remains is simply 171 presents/s > 165 Hz).
At the user's ~152 presents/s on 165 Hz *without* VRR, at least 13 vblanks per second must repeat a
frame even with perfect pacing — a 13 Hz beat, visible in a fast pan — and any uneven gen/real
spacing turns into dropped frames on top. Options:

- **VRR** is the real fix. Hyprland `misc:vrr = 3` enables VRR only for fullscreen windows whose
  content type is "game"/"video"; Proton games under Xwayland usually don't set a content type,
  so check `hyprctl monitors | grep -i vrr` *while the game is fullscreen*. If it says `false`, try
  `misc:vrr = 2` (all fullscreen windows) and make sure the game runs fullscreen (WindowMode).
- Without VRR, the output rate should divide the refresh: 165 presents/s needs 82.5 real fps
  (`[reflex] fps_limit=82.5`) — above the user's ~76, so not reachable at these settings; a 120 Hz
  mode with `[reflex] fps_limit=60` (120 presents/s, one per vblank) is even but costs latency.
- A "pace to vblank" mode (present on a measured vblank grid) would need display timing that
  DXGI under Wine/DXVK doesn't expose; not attempted.

The NVRM kernel messages seen during this work (Xid 31 MMU faults in ACOrigins at 16:17:03 and
16:43:54, `intermapRegisterDmaMapping`/`gpu_vaspace.c` assertion failures at 16:29:50 and
16:45:15 local time) all happened while four game instances shared the 8 GB GPU (7.3–7.7 GB in
use); they faulted the offending process' GPU context only.

## Vulkan route (`mode=on`, not working)

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
available") — raw texture mode is mandatory. A native Linux Vulkan test (no Wine) shows the 64-bit
handle *also* equals the 32-bit one there (0x9/0x9 storage, 0x8/0x8 sampled), so identical values are
normal and winevulkan is not truncating them. The handle values are therefore not obviously wrong;
why the kernels' image accesses are lost remains open (candidates: how NGX's Wine core maps the
cubin launch / descriptor residency for images it didn't create itself, or something DLSS-SR's
DXVK-driven launches do that NGX's own Vulkan backend doesn't). (Correction: an earlier commit message claimed hide_handle64 produced generated
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
| Motion vectors | our DLSS-SR motion vectors (RG16F, render res, UV units, prev−cur), `MvecScale` = render width/height (`[fg] mv_units=px`; DLSS-G wants pixels, see "Frame pacing"), `CameraMotionIncluded=1` |
| Jitter | the engine's sub-pixel jitter (pixels) |
| Camera matrices | the engine's (`[fg] camera=engine`, see "Frame pacing and generated-frame timing"); identity with `camera=identity` |
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
- `touch <out>/fgtrace-start` (mode=d3d12): per-present timestamps + image bands into `<out>/fgtrace/`;
  `tools/fg-trace` drives a constant-rate camera spin around it and runs `tools/fg-pacing.py`.
- `debug_fill=1`: create the output texture magenta. Don't use a D3D11 UAV clear for this: DXVK
  defers clears until the image's next D3D11 use (our copy *after* NGX), which overwrites NGX's
  output and looks like "NGX never writes".
- OutputDisableInterpolation is read back (host-visible VkBuffer per ring slot) and counted in the
  perf line (`dlssg-disabled`).
- `bb_format_dxgi=1`: pass the DXGI format number at create. Result: NGX then doesn't write the
  output at all, so on Vulkan the **VkFormat** numbering is the right one.

## Next steps (recommended)

1. (Optional, uncertain payoff) keep digging into the Vulkan route: handle values and launch form
   look normal, so the next step would be a minimal standalone Windows Vulkan program under the same
   Proton that runs DLSS-G through NGX on its own images — if that fails too, the Vulkan route is
   broken in the driver's Wine NGX core and only NVIDIA can fix it.
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
