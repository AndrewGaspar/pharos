# Stereoscopic 3D (`[stereo]`, experimental)

Both eyes are drawn from the same simulated frame and shown side by side. The aim is XREAL glasses in
3D mode under HypXRland, at 1080p60 per eye with DLSS Performance. Code: `src/stereo/`. The idea
comes from torchbearer (Alan Wake Remastered), which renders twice by calling the engine's frame
function again. Origins can't be done that way. Its engine has separate simulation and render threads
and nine recording threads, and Denuvo/VMProtect rule out patching its code. So Pharos does it at the
Direct3D 11 level.

"live" = observed in the lab (t1, 2560x1600, Discovery Tour), "measured" = numbers from logs or
captures.

## How it works

### 1. Record the frame, submit it twice (`record.cpp`)

The render thread issues each frame on the immediate context (live, capture cap-001):

- about 53 `ExecuteCommandList`s, holding about 14,500 indirect draws recorded on deferred contexts;
- about 120 draws, dispatches and copies of its own.

While stereo runs, every immediate-context call the game makes between two Presents is recorded,
with its arguments, COM references and uploaded data:

- state setters, draws, dispatches, copies and clears;
- `Map`/`Unmap` uploads and `UpdateSubresource`;
- `ExecuteCommandList`.

At Present the frame's image (the left eye) is copied out, the recording is replayed (the right eye)
and the two images are composed. Command lists are re-executable in D3D11, and the data mapped into
them is baked in, so replaying a list costs DXVK's CS thread the same work again but no game CPU.

The replay leaves some things out or reproduces them differently:

- **Queries and readbacks are skipped.** Map READ, copies into staging and Begin/End are left out;
  the game keeps the left eye's results.
- **Calls from the mod itself aren't recorded.** These are recognised by the return address being in
  our DLL.
- **Uploads into resources the GPU never writes are skipped.** That means texture/mesh streaming
  (no render-target, depth or UAV binding, and not a constant buffer). Re-running them is pointless,
  and wrong when the source ring has been refilled since. That refill was the green/magenta texture
  corruption seen while bisecting, which turned out to be a broader problem with copies (next item).
- **Copies whose source the CPU writes (dynamic or staging) read a GPU snapshot.** The source region
  is copied into an arena of ours when the game issues the copy, and the replay copies from the
  arena. Bisected live: replaying immediate-context copies alone corrupted textures (green/magenta
  surfaces), because the game had refilled their upload ring by the time of the replay.
- **Map data goes through a cached shadow.** Reading back what the game wrote into a mapped dynamic
  buffer means reading write-combined memory, measured at ~20 MB/s. Even 0.3 MB a frame cost ~20 ms,
  and 31 MB at the title screen cost seconds. So while recording, `Map` hands the game a cached
  shadow, and `Unmap` copies it into the real mapping. A WRITE_NO_OVERWRITE map copies, and records,
  only the bytes that changed.

**Check: with zero separation the replayed eye must match the original.** It does exactly
(0 of 4 M pixels differ) at the title and in the world, once the per-eye state below is in place.
With only DLSS shared the mean difference was 0.2/255. With everything per eye it's 0–0.5/255: fine
noise over textured surfaces, likely the two DLSS instances (live).

### 2. Per-eye camera (`camera.cpp`)

The camera reaches the GPU through constant buffers (names from the shaders' reflection data):

- **`PassParams` (cb1, 2 KB pool buffers)** carries the eye position, view, projection, view-projection
  and their previous-frame versions, `viewTranslation` (the engine renders camera-relative: 79 of 100
  vertex shaders compute `(pos - viewTranslation) x viewNoTranslationProj`), the frustum planes and
  the reverse projections.
- **`InstanceParams` (cb3, 512 B)** carries a per-draw `worldViewProj`, used by 9 of 100 vertex
  shaders.

These buffers are written on deferred contexts and baked into command lists that both eyes execute.
So while stereo runs:

1. **Each bind is redirected.** On any context, every bind of a `PassParams` that describes a
   perspective, non-square view (cube faces and shadow views are square or orthographic), and of an
   `InstanceParams` whose `worldViewProj` equals that view's `viewProj x world`, goes to a slot in a
   32 MB pool buffer of ours.
2. **Each slot holds two versions, computed at bind time.** Each eye's camera is moved along the
   camera's right vector by +/- separation. Convergence is an NDC shift, `x += k w` with
   `k = m00 d / Zc`, giving zero disparity at Zc.
3. **Uploads pick the eye.** Before a command list executes, its slots get the version of the eye
   being drawn: the left eye for the game's own submission, the right eye for the replay.

The data comes from the same kind of cached shadow (`Map(WRITE_DISCARD)` of those buffer sizes, any
context).

Measured in the world: per frame, ~480 main-view `PassParams` and ~3,800 `InstanceParams` binds are
redirected, and ~5 MB of slots are uploaded per eye.

Check, measured on screenshot pairs:

- **Title:** the 3D logo comes out of the screen (−19 px) and the background is behind it (+10 px).
- **World:** far walls show +20 to +24 px, against +22 px predicted for 30 m at 4 m convergence,
  2560 px wide.
- **Anaglyphs** of the Alexandria courtyard and Giza show consistent geometry, shadows and lighting.

### 3. Per-eye temporal state (`temporal.cpp`, DLSS in `upscaler.cpp`)

**DLSS** has one NGX feature, and so one history, per eye.

**The game's own temporal state is learned at run time.** For 4 frames, every draw, dispatch, copy
and clear on every context reports what it reads (PS/CS SRVs, copy sources) and writes (RTVs, DSV,
UAVs, copy destinations, clears). The lists are kept in execution order: per command list, merged
when the immediate context executes it. A resource whose first access in a frame is a read, and that
this frame or the previous one wrote, carries state across frames. That covers ping-pong pairs.

Live, in the world: 39 such resources. Each gets a handling:

- **Per-eye twins, swapped with GPU copies around the right eye's pass.** These are the ones laid out
  like the screen (render resolution and its fractions with the screen's aspect ratio, froxel
  volumes), plus small buffers and 1x1 values. 31 resources:
  - render-resolution RGBA8/RGBA16F/R32F/R8 histories;
  - 742x464 and 371x232 SSAO/volumetric buffers;
  - 93x58x252 froxel volumes;
  - exposure (1x1);
  - small buffers.
- **Shared between the eyes:** world-space data, i.e. the 128x128x300 GI volume, cached shadow
  cascades (512x512x3) and square maps.

**Cost: about 3 ms of GPU copies a frame at 1484x928.** The swaps take 4 copies per resource per
frame.

**Relearning:** at start, and again when the size of the frame changes by more than 25% for 90
frames (title → world, other areas). A learning round costs one ~300 ms hitch.

Measured at zero separation in the world: the L/R difference went from 0.2/255 mean (DLSS shared)
and visible lighting patches that differed between the eyes, to 0.5/255 of uniform noise with no
structure.

### 4. Output (`stereo.cpp`, `core/hyprland.cpp`)

- **`hsbs` (and `auto`):** each eye squeezed to half the window's width in the window's own
  swapchain.
- **`sbs`:** the swapchain is created twice the game's width. `GetDesc`/`GetDesc1` report the game's
  width, and `GetBuffer(0)` returns a one-eye back buffer of ours. Each eye is then copied 1:1 into
  its half. Live: the game kept rendering 2560x1600, and the swapchain was 5120x1600.

Under DXVK, `sbs` can't reach full resolution in a 1920 window yet:

- DXVK sizes its Vulkan swapchain from the surface's `currentExtent` whenever the surface defines one
  (`Presenter::pickImageExtent`, DXVK 3.1.1).
- The user's GE-Proton11-12-hypxr (`WINE_VULKAN_ALLOW_LARGER_SWAPCHAIN=1`) only raises
  `maxImageExtent`. `currentExtent` stays the window, which suits vkd3d-proton, whose separate patch
  creates the swapchain at the buffer size (confirmed by the torchbearer session).
- So under D3D11, `sbs` is scaled down to the window. It's a filtered half side by side until DXVK
  gets the same change, which is why `auto` = `hsbs`.

**The window is tagged for HypXRland.** The tag is `stereo-hsbs` or `stereo-sbs` (the user's
`hyprland-xr.lua` maps `stereo-<layout>`). It's set over Hyprland's IPC socket from inside Wine,
ported from torchbearer. `mode = auto` turns stereo on while HypXRland reports the game's monitor as
`"stereo": "sbs"`.

**Frame Generation is off for the session once `[stereo] mode` isn't `off`.** It presents generated
frames from its own back buffer.

## Performance (measured, t1, Discovery Tour courtyard, GPU to itself)

| render → output | mono | stereo | GPU busy (stereo) |
|---|---|---|---|
| 1484x928 → 2560x1600 | 97 fps | 50.6 fps (twins on) | ~60% |
| 960x600 → 2560x1600 (≈ DLSS Performance) | 100 fps | 60 fps | 82% |

**The replay's CPU cost is small:** ~1 ms on the render thread, ~0.7k replayed calls. The second
eye's cost is GPU work plus DXVK's CS thread executing the lists again. At 1080p output, DLSS costs
about half of the 2560x1600 numbers per eye, so 1080p60 at DLSS Performance holds where mono runs
~100 fps. In dense crowds (CPU-bound mono ~55 fps) it won't.

## Not done / known issues

- **Full resolution needs DXVK to honour the larger-swapchain switch** (above).
- **Occlusion and frustum culling use the left eye's camera.** HZB culling and the per-instance
  culling constants aren't redirected, so objects at the right edge of the right eye could be culled
  early. Not seen yet.
- **The HUD and menus sit at screen depth in both eyes.** That's the same as torchbearer.
- **Quiver's HUD isn't told the layout.** Pharos doesn't use the Quiver SDK
  (`quiver_set_stereo_layout`).
- **Learning hitch:** ~300 ms, once per scene change.
- **Not yet checked on the glasses under HypXRland.**
