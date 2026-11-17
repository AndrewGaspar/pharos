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

DLSS-G on D3D12 costs ~1.55 ms GPU per evaluation at 768x480 (2x). (The option then called `count=1`, one
generated frame per real frame, is now `count=2`, the multiplier; see "Multi Frame Generation" for 3x-6x.)

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
pharos.log — a generated/real `Present` above ~1 ms means the display path throttles presents;
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

### Stale generated frames after GPU contention: the event latch (2026-11-17)

On the user's machine a session that started under GPU contention showed stale generated frames
(58/118 inverted shift ratios with `pace=smooth`, 108/118 with `pace=ready`), in runs, showing
gen(N-2) (the texture set's previous output), and stayed that way after the contention ended. A
fresh session was clean. Theory (9e2c3d8): a present-thread wait for DLSS-G that times out
(100 ms) leaves its `SetEventOnCompletion` registered. When it fires, it sets the auto-reset event,
and the *next* wait returns at once although the fence is below its value. From then on every wait
returns one evaluation early (each registration fires during the following wait): the thread is
latched one evaluation behind until two signals happen to coalesce.

**Reproduction (t2, 2560x1600, `tools/profiles/fg-minimal.ini`, t1 as the load at PixelDensity
2.0 uncapped, total VRAM ≤ 6.4 GB).** Debug builds of each version, made with
`tools/fg-latch-dbg.py` (scratch copies only), count fence-event waits, timeouts and wakes with the
fence still below the value, and add `[fg] dbg_present_wait_ms` (live) to force present-wait
timeouts. `tools/fg-contend` traces before, during and after a 60 s contention period;
`tools/fg-latch-force` forces timeouts (3 ms wait for 5 s), restores 100 ms and traces twice.

| build | trigger | event waits that woke with the fence below the value (per 2 s), afterwards | completion check (a9fc0a2+) | traces after: inverted / n |
|---|---|---|---|---|
| v6 cfc92d7 | **natural**: one 100 ms timeout during a teleport load hitch (`1 timed out`, then `2`, `127`, then ~200 of ~200 waits) | ~200 of ~200, permanently | – | 62 / 112 (median ratio −0.93) |
| v6 (earlier session, uninstrumented) | load of a second game while FG ran, VRAM 7.3 GB | – | – | 95/111 before contention, 40/110 during, 82/112 and 92/112 after |
| v6 (instrumented, other sessions) | 60 s natural contention (t2 at 62 real fps) | 0 (no timeouts) | – | 1/118, 0/118, 0/118, 0/118 |
| a9fc0a2, ts_guard=0 | 60 s natural contention | 0 (no timeouts) | 0 | 0/118, 1/118, –, 0/118 |
| a9fc0a2, ts_guard=0 | forced (7 timeouts) | 173 of 173, 175 of 175 | **173 of 173** "fence done before DLSS-G's end timestamp" | **113 / 113** (median −1.53) |
| a9fc0a2, ts_guard=1 | still latched (same session) | 172 of 172, 191 of 191 | 191 of 191, guard timeouts 0 | 1 / 118 |
| HEAD 9e2c3d8, ts_guard=0 | 60 s natural contention | 0 | 0 | 0, 1, 0, 0 / 118 |
| HEAD, ts_guard=0 | forced (323 timeouts) | 1 stale wake caught (`wait_on` re-checks and keeps waiting) | 0 | 0 / 118, 0 / 118 |
| HEAD, ts_guard=1 | forced (283 timeouts) | 1 caught | 0 | 0 / 118, 0 / 118 |

Conclusions:
- The theory holds. A single present-wait timeout is enough, and the latch outlives its cause
  indefinitely. Natural 60 s contention here (DLSS-G 7 ms, 62 real fps) never timed out; load
  hitches (teleport/streaming, another game loading with VRAM near full) did. That matches the
  user's session that *started* under contention.
- The a9fc0a2 counter is misnamed for this case: the fence was *not* done. The event woke early,
  and the end-timestamp check sees the unfinished evaluation. Both the timestamp guard and
  `wait_on` fix the symptom; `wait_on` fixes the cause (the fence's own value is the only thing
  trusted). The timestamp check stays useful as a cheap independent tripwire (0 on HEAD).
- Under forced timeouts a9 hardly times out at all (7 vs ~300 on HEAD), because latched waits
  return immediately. That is the latch hiding its own cause.
- Caveat: these are debug builds (counters + the wait knob); the shipped HEAD differs only by those.
- Debug-build counter line: `perf: DEBUG wait_eval: N event waits, T timed out (...), E woke with the
  fence below the value (present thread; game thread G)`; game-thread waits never woke early in
  any run.
- `tools/fg-trace` occasionally reported "no trace written" although the mod wrote it: `tail | grep -q`
  under `pipefail` fails when grep exits first (SIGPIPE). Fixed.

### Game cadence under Frame Generation: headroom cap, Reflex watchdog (2026-11-17)

User report: with FG on (VRR active, ~71 real fps, DLSS-G ~5.1 ms) camera turning felt hitchy, with
FG off it felt great. Their traces: generated frames correct, presents evenly spaced, but the
*game's* frame times uneven (arrival interval 14 ± 3–6 ms, p90 22, max 44 vs 9.7 ± 1.0 with FG off),
the game waiting for the present thread on ~every frame (~5 ms), and G/R presented +24/+31 ms after
the game's Present. Reflex had shut itself off 2 s after arming.

**Why the cadence breaks.** Without headroom the GPU queue fills: DLSS-G runs on our second
(vkd3d-proton) device, which the game's Reflex doesn't account for, so even with Reflex on the
GPU is oversubscribed. DLSS-G for frame N then finishes ~1.7 frames after the game's Present, the
present thread falls behind, and the game blocks in `join` (before its Reflex sleep) at irregular
times. The simulation's frame times follow those stalls; evenly spaced presents of unevenly timed
content look like hitching. Reflex alone doesn't fix it (measured below); a cap with headroom does.

**Reflex "sleep did not return within 500 ms; Reflex disabled for this session"** came from
hitches, not driver hangs. In the t2 log the first sleeps coincided with FG's D3D12/NGX init
(D3D12CreateDevice 709 ms, a 582 ms frame): init starts at frame 300, Reflex arms at frame 240.
That matches the user's "2 s after arming". The watchdog now only skips sleeps until the overrun
sleep returns, then resumes; a sleep stuck > 3 s switches the sleep mode off, starts a new
sleeper and re-arms (up to 3 times, then off for the session). The perf line reports
`overruns`/`hangs`.

**A second bug: FG re-enabled after a gap ran at 2 fps for ~40 s.** The real-frame interval EMA
spanned the frames without FG (F9/ini toggle, menus, loading). After 30 s off the EMA jumped to
~3 s, the real present waited half of that, and the game (blocked in `join`) ran at ~2 fps while
the EMA decayed by 10 % per frame. The F9 hotkey path reset it, the ini path and menus/loading
screens did not. Now any frame without FG breaks the chain, hitch intervals count at most 2.5x,
and the half-interval wait is capped at 25 ms. Re-enabling is now immediate (66 fps in the first
2 s window).

Measurements: t2, 2560x1600, DLSS at the user's PixelDensity 0.6, Ultra High, Siwa, only this game
on the GPU, constant-rate spin (`tools/fg-series`). Uncapped FG ~73–81 real fps, DLSS-G 5.1 ms,
close to the user's.

| configuration | real fps | game arrival interval mean ± sd (p90 / max) ms | game waited for present thread / 2 s | G / R presented after game's Present ms | position residual (VRR) mean / p90 px |
|---|---|---|---|---|---|
| **user-like**: Reflex off, no cap | 78 | 12.9 ± 3.3 (17.8 / 39.5) | 124–136 (4.6 ms avg) | 22.6 / 29.2 | 6.7 / 16.4 (p99 59) |
| Reflex on, no cap | 81 | 13.0 ± 3.5 (18.8 / 25.9) | 106 (3.6 ms) | 20.9 / 27.4 | 8.4 / 18.0 |
| `[fg] sets=3` (Reflex on, no cap) | 76 | 13.0 ± 6.0 (23.4 / 37.5) | 0, but 56 generated frames dropped | 20.0 / 22.0 | 26.4 / 59.0 |
| Reflex on, fps_limit=76 (too little headroom) | 73 | 13.4 ± 1.4 | 78 (3.3 ms) | 13.1 / 19.8 | 2.0 / 3.9 |
| **Reflex on, fps_limit=66, limiter=driver** | 66 | 15.2 ± **0.79** (15.8 / 19.7) | **0** | **9.6 / 17.2** | **2.3 / 4.7** |
| Reflex on, fps_limit=66, limiter=cpu | 66 | 15.2 ± 0.86 | 0 | 9.9 / 17.5 | 2.5 / 5.3 |
| Reflex on, fps_limit=60 | 60 | 16.7 ± 0.85 | 0 | 9.2 / 17.6 | 2.8 / 6.0 |
| **`auto_cap=1`, auto_cap_lag=0.7** (new default) | 63–64 | 15.6 ± **0.86**, 16.0 ± 0.87 | 0 | 9.5 / 17.3 | 2.3 / 4.8, 3.2 / 8.1 |
| auto_cap, lag 0.85 (Reflex on / off) | 70 | 14.5 ± 1.19 / 14.3 ± 1.38 | 0 | 10.2 / 17.4, 11.0 / 18.2 | 3.9 / 5.3, 3.0 / 6.1 |
| FG off (Reflex on) | 107 | 9.3 ± 1.15 | – | – | 4.2 / 9.7 |

(The residual is display-time judder in px of image motion during a ~5000 px/s spin; with the
cap the FG-on image is as smooth as, or smoother than, FG off at 107 fps.)

Latency (`tools/latency-bench`, 40 mouse-look injections, fresh launch each, same settings). "Frame
done" is to the end of the real frame's GPU work; what reaches the screen also includes the R/G
present delay above:

| | real fps | input → Present called, median | input → frame done, median (p10–p90) | + R present after Present | ≈ input → real frame presented |
|---|---|---|---|---|---|
| user-like (Reflex off, no cap) | 77.8 | 47.5 | 62.2 (58.0–69.4) | +28.9 | ~76 |
| Reflex on, no cap | 77.5 | 44.5 | 59.1 (52.9–66.2) | +27.5 | ~72 |
| Reflex on, fps_limit=66 (driver) | 65.8 | 53.5 | 59.6 (53.8–64.6) | +17.3 | ~71 |
| Reflex off, fps_limit=66 (cpu limiter) | 66.1 | 54.7 | 60.8 (52.9–65.3) | +17.5 | ~72 |
| auto_cap (defaults) | 62.4 | 56.0 | 61.8 (55.6–68.3) | +17.8 | ~74 |
| FG off (Reflex on) | 107.8 | 32.0 | 40.4 (37.7–44.1) | – | ~40 |

So the cap trades ~12 real fps (78 → 66) for an even game cadence, no blocking, and about the
same or slightly lower latency to the screen (the queue it removes is worth more than the longer
frame).

**Recommended defaults** (now in `dist/pharos.ini`): `[reflex] mode=on`, `fps_limit=0`,
`limiter=driver` (it falls back to the CPU limiter while Reflex isn't sleeping); `[fg] auto_cap=1`,
`auto_cap_lag=0.7`, `pace=smooth`, `sets=2`. A fixed `[reflex] fps_limit` (~85–90 % of the uncapped
FG real rate; e.g. 64–66 at the user's ~72–78) overrides the automatic cap and is the steadiest
option (no sawtooth when the automatic cap probes for more). `sets=3` is worse (stale queued
jobs get their generated frame dropped) and stays off.

How `auto_cap` works: every second it compares when DLSS-G finished (after the game's Present) with
the frame interval. Above `auto_cap_lag` (0.7 frames), or any `join` wait, the cap drops 4 %. Below
0.55 it rises 1 %, or 4 % when below 0.35. It starts 2 s after FG starts, at 90 % of the rate then,
and is enforced through the Reflex limiter. Expect a small sawtooth of a few fps as it probes for
the knee; `auto_cap_min/max` bound it.

### Multi Frame Generation (2026-11-17)

`[fg] count` = frames presented per real frame: 2 (2x, the default) .. 6 (6x), or `auto`. Older
inis said `count=1` for 2x (DLSS-G's own MultiFrameCount); anything below 2 still means 2x.

**How raw NGX does it** (no Streamline). The FG programming guide (310.7, "Evaluating the Feature" and
"Presenting Multiple Generated Frames") and the header agree, and the synthetic test confirms it:

- One `EvaluateFeature` per generated frame. `DLSSG.MultiFrameCount` = generated frames per real
  frame (1 = 2x, 3 = 4x, 5 = 6x), `DLSSG.MultiFrameIndex` = 1..MultiFrameCount, each call with its
  own `OutputInterpolated`; all other parameters (inputs, camera, Reset, BackbufferFrameID) are the
  same for the calls of one real frame. Index i is generated at t = i / (MultiFrameCount + 1)
  between the previous and the current real frame. (The "last index is the real frame" reading of
  the Vulkan-route reverse engineering below is wrong: every index is a generated frame.)
- `DLSSG.MultiFrameCountMax` (capability parameter) is **5** with the SDK's nvngx_dlssg 310.9.16 on
  the RTX 5070 Laptop: 6x needs no newer DLL. `MultiFrameCount=6` fails every Evaluate with
  `0xbad00005` (invalid parameter). The driver profile can lower the maximum ("DRS
  MULTI_FRAME_COUNT_LIMIT clamping max frame count" in the DLL).
- The feature is created with a MultiFrameCount, but **an evaluation may use any count up to it,
  changing per frame** without re-creating the feature or a Reset: created with 5, evaluated with 2
  and 3 alternately, every index comes out where a feature created with that count puts it, and the
  GPU cost is that of the smaller count. So the dynamic multiplier is free; the mod creates the
  feature (and the texture sets' outputs) for the largest count it may use.
- Separate command lists per index work (the state lives in the feature): the mod submits each index
  on its own, with its own completion fence value and timestamps, so the present thread can show
  index 1 while the others are still being generated (what the guide recommends).
- **Dynamic MFG is not in the DLL.** `DLSSG.DynamicMaxFrameCount`, `DLSSG.Dynamic.ReflexDriven`,
  `DLSSG.TargetFrameRate`, `DLSSG.RefreshRate`, `DLSSG.StreamlineMode`, VSync and FlipMetering are
  read from the evaluate parameters into one block (nvngx_dlssg.dll `0x1800635xx`), and only used for
  the telemetry record and the debug indicator text (`StreamlineMode` 2 = "Auto", 3 = "Dyn" / "Dyn
  DRV" when ReflexDriven; `- %ux/%ux` = DynamicMaxFrameCount + 1; `- %.0fHz` = TargetFrameRate). They
  are not capability parameters and `GetCurrentSettings` doesn't report them. The per-frame choice is
  the integrator's (Streamline's pacer or the driver); the DLL only consumes MultiFrameCount. So the
  mod has its own controller (below). `GetCurrentSettings` only reports `DLSSG.MustCallEval=1`.

**GPU cost** (synthetic test, `dlssg_d3d12_test.exe <dir> <count> s 24 40 2560 2560 2560 1600`,
2560x1600, one command list per index, GPU otherwise nearly idle):

| multiplier | 2x | 3x | 4x | 5x | 6x |
|---|---|---|---|---|---|
| generated frames | 1 | 2 | 3 | 4 | 5 |
| DLSS-G GPU time per real frame | 2.47 ms | 4.11 ms | 5.81 ms | 7.50 ms | 9.11 ms |

The first generated frame costs ~2.5 ms, every further one ~1.6 ms (optical flow and the network's
shared work are done once per real frame; at 512x256: 0.9 and 0.45 ms). In the synthetic
scene the measured positions lag t by up to ~0.1 (the 2x frame sits at t ≈ 0.39, as before), but
every index is written, in order, and identical to the same t of another count (index 3 of 5 =
index 1 of 1 = index 2 of 3 at t = 0.5).

**Design** (src/fg/fg_d3d12.cpp):

- *Texture sets* (`Set`): per set the colour/depth/MV/HUD-less inputs, the real-frame copy and one
  `OutputInterpolated` per generated index (count 2: as before, one; the `scratch` output is gone).
  Per index the completion value, timestamp ring slot and its old end timestamp (ts_guard check).
- *Evaluation*: one submission per index (`submit(..., after_d3d11 = i == 1)`: only the first waits
  for the D3D11 inputs fence), ring of `2 x 5 + 4` command allocators / timestamp pairs.
- *Present thread*: a job holds n generated frames; each is waited for (fence + timestamp check),
  paced and presented, then the real frame. Step = real-frame interval / multiplier (2x: half an
  interval, as before; the interval counts at most 50 ms). `pace=smooth` generalised: generated 1 is
  held until one step after the previous real present minus `pace_relax` of how early it is, every
  later present (generated 2.., then the real one) one step after the previous one's Present call.
  `ready` / `arrival` / `content` likewise (content: generated k at arrive + L − dt·(m − k)/m). A job
  superseded before it starts presents only its real frame, as before; a generated frame that isn't
  ready within 100 ms drops it and the rest of its job's generated frames.
- *Trace*: presents.csv gains `gi,gn` (generated index, multiplier); `tools/fg-pacing.py` reports
  the shift ratio per index (expected gi/gn), ordering (G1..Gk of one real frame in order), and
  works unchanged on old traces (identical 2x output).
- *Refresh target*: `[fg] refresh_hz=0` (new default) detects the display refresh (swapchain
  fullscreen mode, else `EnumDisplaySettings` of the swapchain's output; logged as `fg12: display
  refresh`), passed to DLSS-G as `DLSSG.RefreshRate`; `[fg] target_hz` (0 = that refresh) is the
  presents/s to aim for.
- *auto_cap* generalised: cap = min(headroom cap, target / multiplier). The headroom cap is the
  feedback described above (DLSS-G's first generated frame done within `auto_cap_lag` frames, no
  joins); it now rises only while it is the binding cap, otherwise it climbed unchecked while the
  refresh cap held the rate. With VRR at 165 Hz: 82.5 real fps at 2x, 55 at 3x, 41.25 at 4x.
- *count=auto* (dynamic): every second, in `[count_min, count_max]` (default 2..4): one step up when
  the headroom cap stays more than 10 % below target / multiplier for 2 s (within 10 % the lower
  multiplier at the headroom cap is the better trade); one step down when the frame's own work
  (frame interval minus the time the limiter / Reflex slept, `reflex_wait_us()`) leaves 15 % headroom
  for target / (multiplier − 1) and DLSS-G's lag scaled to the shorter interval stays below
  auto_cap_lag − 0.15, for 3 s. A step down undone within 6 s blocks further steps down for 10 s,
  doubling to 60 s. Windows with a hitch (real fps below 60 % of the cap: loading, a trace being
  written, a config change) never change the multiplier. Without auto_cap (or with an explicit `[reflex] fps_limit`) the multiplier is
  floor(target / real fps + 0.1). The log line `fg12: auto cap ... fps, Nx (...)` reports every
  change.
- *Perf line*: `generated` counts generated presents; `GPU x ms/frame` is per real frame (all its
  generated frames), followed by the time per generated frame and the effective multiplier.
  `fg12 DLSS-G lag split` decomposes the auto-cap lag: game Present → DLSS-G starts on the GPU, its
  GPU duration, GPU end → present thread awake (timestamps mapped with `GetClockCalibration`).

**Measurements** (t2, 2560x1600, DLSS at PixelDensity 0.6 (render 1536x960), Ultra High, Reflex on,
only this game on the GPU, constant-rate spin `tools/fg-series`; gamescope headless at 165 Hz
(detected: `display refresh 165.00 Hz (swapchain mode, fullscreen)`); Siwa unless noted. "cap" =
how the real rate was limited: auto = `auto_cap` (headroom + refresh target), a number = `[reflex]
fps_limit`. Shift ratio: median (p10–p90) per generated index, expected k/N.)

| config | real fps | presents/s | present interval sd | game arrival interval | shift ratio per index | outside [0,1] / out of order | VRR position residual mean / p90 px |
|---|---|---|---|---|---|---|---|
| 2x, auto (63 cap) | 57.9 | 116 | 0.46 ms | 17.3 ± 1.28 | 0.500 (0.489–0.527) | 1 / 0 of 118 | 4.15 / 7.64 |
| 2x, 55 | 54.9 | 110 | 0.41 | 18.2 ± 0.93 | 0.500 (0.481–0.544) | 0 / 0 | 2.88 / 6.71 |
| 2x, 41.25 | 41.2 | 82 | 0.39 | 24.3 ± 0.92 | 0.500 (0.475–0.526) | 0 / 0 | 3.95 / 8.72 |
| **3x, auto** (refresh cap 55) | 54.8 | **164.4** | 0.35 | 18.2 ± 1.07 | 0.333 (0.329–0.345), 0.666 (0.656–0.676) | 2 / 0 of 156 | **1.94 / 3.65** |
| 3x, 55 | 54.8 | 164.5 | 0.31 | 18.2 ± 0.99 | 0.333 (0.327–0.339), 0.666 (0.661–0.673) | 0 / 0 | 1.71 / 3.45 |
| 3x, 41.25 | 41.2 | 124 | 0.34 | 24.3 ± 0.95 | 0.333 (0.319–0.347), 0.666 (0.655–0.682) | 0 / 0 | 2.50 / 6.01 |
| **4x, auto** (refresh cap 41.25) | 41.2 | **165.1** | 0.35 | 24.3 ± 1.20 | 0.250, 0.501, 0.750 (p10–p90 ±0.015) | 0 / 0 of 174 | 3.24 / 6.99 |
| 4x, 41.25 | 41.0 | 164.0 | 0.28 | 24.4 ± 0.95 | 0.250, 0.500, 0.750 | 1 / 0 | 2.04 / 4.63 |
| **count=auto** | 54.8 (3x) | 164.6 | 0.31 | 18.2 ± 0.84 | 0.333, 0.666 | 0 / 0 | 1.79 / 3.79 |
| Alexandria (DT, npc_density=3, npc_distance=2): 2x, auto | 42.8 | 86 | 1.79 | 23.4 ± 1.80 | 0.500 (0.477–0.534) | 1 / 0 | 4.24 / 8.36 |
| Alexandria: count=auto (two traces) | 54.8 (3x) | 164.5 / 164.7 | 0.42 / 0.39 | 18.3 ± 1.20 | 0.333, 0.666 | 0 / 0 | 2.26 / 5.00, 2.92 / 6.28 |

Every generated index sits at k/N with the same tight spread as 2x, nothing out of order, the
completion check never fired (0 of all), no generated frames dropped and the game never waited for
the present thread in the steady state. Present intervals are as even as at 2x (sd 0.3–0.35 ms; the
transitions into a real frame are ±0.03 ms, the ones after it carry pace_relax). The position residual
is a judder measure in px of image motion: at the same base rate 3x is below 2x (1.7 vs 2.9 px at 55,
2.5 vs 4.0 at 41) - each present moves the image by a smaller step and the cadence stays even.

5x and 6x (gamescope `-r 480`, `scripts/aco up t2 --refresh 480`; detected 480 Hz, target 480
presents/s): every index exact (5x: 0.200 / 0.400 / 0.601 / 0.800; 6x: 0.167 / 0.333 / 0.500 / 0.666 /
0.833, p10–p90 within ±0.02), 0 outside, 0 out of order; the base is GPU-limited at 38–45 real fps
there (226–263 presents/s; GPU 8.3–8.6 ms per real frame at 5x, 10.1–13.2 at 6x), so 6x needs a
faster GPU or a lower resolution to reach 480.

GPU cost in game (perf line, per real frame; overlaps the game's own GPU work): 2x 2.6–2.9 ms, 3x
4.8–5.3 ms, 4x 6.3 ms, 5x 8.3–8.6 ms, 6x 10.1–13.2 ms; 2.1–2.6 ms per generated frame.

**Latency** (`tools/latency-bench`, 40 mouse-look injections, fresh launch each, same session):

| config | real fps | presents/s | input → Present called | input → frame done (p10–p90) | generated / real presented after the game's Present | ≈ input → first generated / real frame presented |
|---|---|---|---|---|---|---|
| 2x, auto cap | 63.7 | 127 | 53.2 | 60.6 (55.0–68.5) | +10.1 / +17.9 | ~63 / ~71 |
| 2x, 55 | 54.8 | 110 | 64.0 | 69.9 (63.9–79.3) | +10.0 / +19.1 | ~74 / ~83 |
| 3x, 55 | 54.8 | 165 | 66.6 | 73.9 (66.7–79.5) | +11.1 / +23.2 | ~78 / ~90 |
| 4x, 41.25 | 41.1 | 165 | 83.9 | 91.1 (82.2–102.2) | +10.4 / +28.5 | ~94 / ~112 |
| count=auto (3x at 54.4) | 54.4 | 163 | 67.0 | 74.1 (68.4–81.9) | +10.3 / +22.5 | ~77 / ~90 |

Latency is set by the base rate (the game's own pipeline is ~3 frames): at the same 55 fps base, 3x
costs ~4 ms more to the first generated frame and ~7 ms more to the real one (held (m − 1)/m instead of
1/2 of an interval) over 2x, but shows 165 instead of 110 frames per second. 4x at 41 fps adds ~20 ms
over 3x at 55 because the base is slower.

F9 (`tools/fg-toggle-test t2 6 fg.count=3`): six toggles, no hang or exception, 3x back after every
re-enable (141–154 presents/s while running with the character, verify: the 2/3 frame strictly between
its neighbours in 95 % of moving samples). Count changes in the live ini work (the outputs and the
feature are re-created: one ~1 s hitch); count=auto never re-creates (made for count_max).

**Two fixes found on the way:**
- *The present thread could stay behind after a hitch* (seen at 4x after a live count change: 1.2 s
  stall, then G presented 60–100 ms after the game's Present for seconds, the game blocked in join
  and running at ~28 fps). The real-frame interval EMA came from the game's arrivals, and while the
  game waits for the present thread its frames are exactly as long as the present thread's jobs, so
  the pacing step never shrank: a neutral loop with no way back. The game's join waits are now taken
  out of the interval; afterwards 4x ran clean from a fresh launch and after count changes.
- *fg-pacing.py at 3x+*: one real frame moves the image 100–200 px in the spin, too much for one
  phase correlation of the band: the direct R→R′ shift sometimes failed (the "inversions" in the
  first 4x runs were that, every consecutive step was consistent). MFG runs use the sum of the
  consecutive shifts; 2x output is unchanged.

**auto_cap in partly CPU-bound scenes** (question from the NPC runs: ~55 real fps at 2x in story
Alexandria with the GPU ~64 % busy). The lag split (`fg12 DLSS-G lag split`) shows where the "DLSS-G
done" time goes: DLSS-G *starts* on the GPU 6–9 ms after the game's Present (the game's own frame
still finishing on the GPU, plus our copies), runs 2.6–2.8 ms, and the present thread is awake
0.3–0.8 ms after the GPU end (1–4 ms more only while the pipeline is overloaded). So the 0.7-frame
target effectively keeps the game's GPU frame plus DLSS-G under ~70 % of the frame interval: the cap
follows GPU latency, not GPU queue depth, and that is why it stops at ~64 % busy. In Alexandria (DT,
npc_density 3) measuring from DLSS-G's GPU end (`[fg] auto_cap_lag_from=gpu`, new) let the 2x cap
climb from 48 to 58–62 fps, but the result was worse: VRR residual 8.2 / 14.2 px and 6 ratio
outliers against 4.2 / 8.4 and 1 (arrival sd 1.17 vs 1.80 ms). The default stays `ready`, 0.7. For a
CPU-bound base the answer is MFG: count=auto went to 3x at 55 fps there (165 presents/s, residual
2.3–2.9 px, 0 outliers).

**Recommended for the user's 165 Hz VRR panel**: `count=auto` (count_min 2, count_max 4) with the
defaults (`auto_cap=1`, `auto_cap_lag=0.7`, `refresh_hz=0`, `target_hz=0`, `pace=smooth`), or a
fixed `count=3`. In both the cap is at most 165 / N, so presents stay at or below the refresh: 2x
whenever the game holds 82.5 fps with headroom, 3x at 55 when it holds that (Siwa and dense
Alexandria here), 4x at 41.25 below that. For a fixed base, `[reflex] fps_limit=55` with `count=3`
was the steadiest single setting measured (sd 0.31 ms, residual 1.7 px). 5x/6x only make sense on
360–500 Hz displays (set `count=auto` with `count_max=6`, or the count), and need a GPU that can
afford 8–10+ ms of DLSS-G per real frame at that resolution.

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

## Debugging aids (pharos.ini `[fg]`)

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
