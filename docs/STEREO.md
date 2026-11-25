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

### 2b. Effects with their own camera copies (`camera.cpp`, "effect cameras")

Some effects don't read the camera from `PassParams` but from a copy in a constant buffer of their
own. Left alone, both eyes use the left eye's copy. Found in capture cap-001 (Alexandria harbour)
from the shaders' reflection data and CB dumps:

| Effect (`[stereo] effects` name) | cbuffer (slot) | Fields | Form |
|---|---|---|---|
| SSR (`ssr`) | `NewSSLRShared` (PS cb7, 512 B) | `worldToProj`, `prevWorldToProj` | viewProj, linear-depth row 2 |
| Volumetric fog (`fog`), 16 CS | `VolumetricFogAllParamsCustomLayout` (CS cb5, 4 KB) | `SSToWS`, `EyePositionWS` | (x z, y z, z, 1) → world; position |
| Near clouds' ray march (`cloudmarch`) | `CloudsRayMarch` (CS cb6, 8 KB) | `Camera[0].PlaneTopLeft`, `PlaneRight` | ray = TL + u R − v U |
| Cloud composite (`clouds`) | `CloudsCompositeViewCache` (CS cb6, 16 KB) | `Proj2World` | (cs.x, cs.y, 1) → sky direction |
| Shadow mask stabilisation (`shadow`) | `ShadowMaskStabilization` (CS cb6, 256 B) | `SSToWS`, `LastSSToWS` | as above |
| Sun flare occlusion (`flare`) | `SunFlareOcclusion` (CS cb6) | `ViewProj` | viewProj |

The clouds are drawn in three steps (cap-001): the near clouds are ray-marched into a 742x464
texture in screen space (`CloudsRayMarch`), blended with the previous frame's (`CombineCloudsTexture`),
and the composite (`CloudsCompositeViewCache`) maps each pixel to a sky direction to read the far
clouds from a 1280x1024 view cache and puts the near ones over them. The sky pass reads the result.
With both copies mono, the clouds came out at mixed depths in front of the mountains.

**How:** shaders whose reflection data names one of these cbuffers are classified `CamEffect` when
the game creates them, and the fields' offsets are read from the reflection data (RDEF). At each
draw/dispatch of such a shader, the buffer bound at that slot (read back with
`*GetConstantBuffers1`) is copied into the camera pool with the fields moved to each eye, the slot
is pointed at the copy for that one call and then given back. Deferred contexts use the per-command-
list slots of §2; the immediate context uploads the eye being drawn (the replay runs the hook again
for the right eye). The data comes from the cached shadow of the buffer's last `Map(WRITE_DISCARD)`
on that context. If that context didn't write it, the latest write from any context is used. Live,
the fog's buffer is always written on another deferred context than the one that dispatches (16 of
the 26 buffers moved per frame). The buffer sizes involved are learned at the effects' draws (live:
256 B, 4 KB, 8 KB, 16 KB, plus the 512 B already shadowed for `InstanceParams`), so only maps of those
sizes are shadowed. A shadow is dropped once the buffer is written another way (`WRITE_NO_OVERWRITE`,
a partial `UpdateSubresource`).

**No D3D call while one of our locks is held.** The per-context state has a lock, because the
immediate context's maps come from several game threads. The uploads and binds that follow from it
happen after the lock is released. (A build that uploaded under the lock hung before its first
Present once, live. It wasn't seen again after the change, but the host was heavily loaded then, so
that's suggestive, not proof.)

Each field is first checked against the main view (the latest main `PassParams` of the command
list, else of any context). A field that isn't a copy of the main camera is left alone. Live, that's 2 per frame: the cloud
view cache's ring cameras, whose `PlaneRight` can line up with the main view's right vector. So the
ray-march planes must also span the screen (2 NDC per unit of u, centred). Per eye:

- **viewProj copies:** the same as `viewProj` in §2: translation by the eye offset, then `x += k w`.
- **screen-to-world (`SSToWS`):** the input x is NDC, and the eye's NDC is the centre's + k, so
  column 2 −= k × column 0. Then the position column += the eye offset.
- **directions (`Proj2World`, the ray-march planes):** the eye's pixel sees the centre's direction
  at NDC − k. The shift in the matrix's own units is measured on the matrix (the NDC of two
  directions it produces), and the constant column (or `PlaneTopLeft`) moves by it.
- **positions:** + the eye offset.

**Measured** (t1, 32.5 mm, 4 m convergence, render 1484x928; expected disparity at infinity +31.2 px).
Disparities are taken from one hsbs frame (both eyes at once, ×2 for the half-width squeeze).
Captures a few seconds apart can't be compared: the clouds, the time of day and the camera move.
Only patches at 0 px with all effects off (the volumetric clouds) are counted:

| `[stereo] effects` | Giza, volumetric clouds (median) |
|---|---|
| none (the bug: clouds at screen depth, in front of the mountains) | 0.0 to +0.2 px |
| `clouds` (composite only) | +0.6 px: these clouds are all near-layer |
| `cloudmarch` | +30.8 px |
| all | +29.4 px |

- **Composite on far clouds: not verified.** No far clouds were visible during the runs, and the
  composite alone doesn't move the near layer. Its matrix maps 1.00 NDC per unit, as expected.
- **2D cloud layers already correct:** the sky pass's own layers (Alexandria at dusk) were already
  at +31.5 px with effects off, because they take the camera from `PassParams`.
- **SSR, Alexandria quay, water region:** with `ssr`, 45 patches match between the eyes, at +28.4
  to +29.0 px (reflected buildings behind the surface). Without it, 24 match, spread up to +34 px
  (beyond infinity: inconsistent eyes). Visibly, the right eye's reflections were smeared, and are
  now as sharp as the left's.
- **Fog:** no colour artifacts or ghosts (all effects on, Giza and Alexandria).
- **Zero separation, all effects on:** L/R difference 0.24/255 mean, the same as before.
- **Frame rate (Giza, uncapped, GPU to itself):** 52.7 and 52.8 fps with all effects, against 53.0
  and 52.6 with none. Cost: ~26 buffer copies of ≤16 KB per frame.

Not moved (deliberately): the temporal reprojection matrices (`SSToLastFrameSS` of the fog and of
`CombineCloudsTexture`). They map an eye's screen to its own previous screen. That's the same
matrix for both eyes when the camera is still, and only second-order different while it moves.
Also not moved: the fog's light-culling view matrices (`VolumetricFogLightCullMask`), the culling
cbuffers (HZB), GI and the water's particle projection (VS stage).

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
`"stereo": "sbs"`. At swapchain creation Pharos briefly marks its window title and checks which
mapped client reports that marker. It uses that client's monitor, including when another output
has focus. A headless game with no matching desktop window selects mono and can use frame generation.

If Quiver stays open across a compositor restart, a new game can inherit the retired
`HYPRLAND_INSTANCE_SIGNATURE`. Pharos first tries that instance. If the connection fails,
it checks the instance locks in `$XDG_RUNTIME_DIR/hypr` for the game's `WAYLAND_DISPLAY`
and connects there before choosing a swapchain. A successful fallback writes
`hyprland: recovered compositor` in `pharos.log`. If a previous launch already selected the
frame-generation back buffer, a fresh game launch is required to switch to stereo.

Frame generation uses its own back buffer. An explicit `[stereo] mode=sbs`, or auto mode that
detects a stereo output at swapchain creation, disables frame generation for the session.

## Performance (measured, t1, Discovery Tour courtyard, GPU to itself)

| render → output | mono | stereo | GPU busy (stereo) |
|---|---|---|---|
| 1484x928 → 2560x1600 | 97 fps | 50.6 fps (twins on) | ~60% |
| 960x600 → 2560x1600 (≈ DLSS Performance) | 100 fps | 60 fps | 82% |

**The replay's CPU cost is small:** ~1 ms on the render thread, ~0.7k replayed calls. The second
eye's cost is GPU work plus DXVK's CS thread executing the lists again. At 1080p output, DLSS costs
about half of the 2560x1600 numbers per eye, so 1080p60 at DLSS Performance holds where mono runs
~100 fps. In dense crowds (CPU-bound mono ~55 fps) it won't.

## Predator Bow and tracked markers

`stereo.aim_eye` selects the eye that looks down the Predator Bow sight. The selected eye takes
the native first-person camera position. The other eye moves by the full eye separation.
`left` is the default. `right` mirrors the arrangement and `center` keeps the native sight line
between the eyes. The activation check uses both the narrow perspective and the camera's proximity
to the player's head. In the Alexandria story save the vertical projection scale changed from
2.40 to 6.19. At two headings the sight camera was 0.18 m to Bayek's right and 1.85 m above the
player origin. This measures the camera rig. The model's eye dominance cannot be inferred from it.
Lab run r29 showed the dot inside the selected aperture for each setting at 32.5 mm separation
and 4 m convergence.

Pharos samples the centre depth while the Predator Bow HUD draws in the sight eye and projects
that point into the other eye. If the bowstring or another surface blocks the target there, only
the sight eye sees the dot. The left eye renders first. For right-eye aim the left view uses the
previous right-eye sample and checks its projection against the current scene within two render
pixels. The first frame of right-eye aim uses a single dot until that depth is available.
The bow and Senu reticles
sample the centre ray independently of tagged world icons. The ordinary bow draws its
spread marks with two separate 32×16 sprites. Each uses the same ray as the center dot,
preserving the spread the game displays as the bow is drawn.

`stereo.markers` reads the world positions returned by the game's position, entity, custom
marker and dynamic location icon controllers on Origins 1.62. Sprite positions are compressed
by the UI. At the Sphinx a 53 m tour label had a native billboard depth of 9.27 m. The marker
shader associates the sprite with a projected world target and limits its depth to the first
visible obstruction. It places an obstructed marker just in front of that surface. Screen-edge
arrows sample the clamped screen position. An unrecognised target uses the visible surface.
Transparent objects without depth writes cannot be identified from this buffer.
If the controllers report more than the shader's 192 positions, each marker pass keeps the
nearest 192 (established in code; the Odyssey port exceeded the limit in Athens, see PORTS.md). The centered
compass distance uses the native HUD plane. At 1280×720 in Alexandria, its left-to-right
displacement matched the compass icon and city name at −4 packed pixels.

A quarter-resolution history holds newly visible foreground depth for 120 ms and eases back
as an obstruction clears. Stereo replay uses the same history for both eyes. The icon batch
records its resolved depths during rasterization. Senu's name and distance labels reuse their
icon's depth. Level numerals use the corresponding shield sprite batch.

The Discovery Tour photo viewer draws its image at zero disparity, with the title, credit and
body text on a nearer plane. The animated panel on the tour introduction also covers the
viewport edge in each eye. The live landscape behind that panel retains its stereo camera.
Main-menu and Tours controls and their mouse or left-stick cursor share the screen plane.
The map continues to use terrain depth.

## Interrupted right-eye replay

Senu's flight can leave a mapped resource open across Present while the game streams scenery.
Pharos skips the unsafe replay. For up to three consecutive missed presents it displays the last
complete stereo pair. A longer interruption shows the live scene in both halves until replay
resumes. The frame statistics record held pairs and mono fallbacks separately. During one
300-present flight interval in r31, 287 replays completed and 13 presents held a stereo pair.

## Not done / known issues

- **Intermittent purple-white flash.** In an Alexandria capture from r36, Senu and the HUD
  remained visible while the landscape in the left eye washed out. The right eye retained
  its normal colors. After the v18 change to stereo history tracking, r41 and r42 showed
  normal brightness in both eyes on the same transition. In r42, 24 movement captures
  with DLSS and 24 with game TAA covered walking and flight. The reported flash did not
  recur. In r36 the left eye's mean blue level exceeded the right by about 123/255.
  In the clear-air control shot of the same rooftops, corresponding patches had nearly
  identical contrast. During the wash, contrast in 48 matched distant patches fell to a
  median 17% of the right eye's. This makes depth-dependent lighting or fog worth checking.
  The saved r36 images only show the final output, so the responsible pass is still unknown.

  To capture a recurrence in a Quiver lab, set `env.PHAROS_BLOOM_PROBE=1` with
  `quiver lab edit` before launch. Every fourth engine frame, the probe samples a
  64×36 grid per eye from the tonemapped input, the upscaler output, and the finished
  stereo images after HUD composition. Native TAA supplies the finished images only.
  Readback skips busy staging slots. The run file stops below 512 MiB.
  `tools/lab/bloom-report RUN_DIR --from-frame N --sheet events.jpg` ranks inter-eye
  differences, changes between stages, brightening within each eye, and the fraction
  of the scene with the pale violet colour seen in r36. Blank samples
  are labelled in the contact sheet. A camera cut can also rank highly.
  Clear the launch override with `quiver lab edit origins env.PHAROS_BLOOM_PROBE=`.

  In r44, 2,678 matched v20 upscaler pairs covered movement and camera switches in
  Alexandria, Memphis and the Black Desert. The strongest inter-eye differences came
  from foreground geometry. In r45, an instrumented pre-v18-history-fix renderer
  revisited the r36 rooftops and Senu hover. Its largest channel-mean difference
  between eyes over 1,497 matched pairs was 3/255. In r46, selecting the debug depth
  display left the colour input intact and turned the output and both finished eyes
  grayscale, confirming the probe distinguishes a change introduced at the resolve.
  In r47, Discovery Tour ran with a real-time clock, DLSS Balanced at 1114×626 to
  1920×1080, and the active profile's LOD and population settings. Over 1,305 final
  stereo pairs from Alexandria and Giza, the largest mean channel difference between
  eyes at any sampled stage was 2.6/255. The strongest pale violet match was cloud
  cover visible in both game inputs. At that frame DLSS changed each eye's mean RGB
  by less than one level per channel. The original flash did not recur.
- **Full resolution needs DXVK to honour the larger-swapchain switch** (above).
- **Occlusion and frustum culling use the left eye's camera.** HZB culling and the per-instance
  culling constants aren't redirected, so objects at the right edge of the right eye could be culled
  early. Not seen yet.
- **General HUD and menu layout** retains the game's stereo UI projection. The cursor and tracked markers have their own depth controls.
- **Map/menu cursor at screen depth: fixed by `[stereo] cursor=depth`** (`stereo/cursor.cpp`;
  measured live in the lab, runs r9-r24). The game's white arrow is drawn into the backbuffer
  (present in framedump PPMs, not a compositor cursor) by one DrawIndexedInstancedIndirect in
  the late UI pass into the display-resolution target the UICompositing pass samples as t86:
  vertex shader `12b61097a3a0b0e7`, pixel shader `068233d2886bf2c8`. It is recognised by that
  shader pair plus its sprite pages (t70 64x64 fmt 77, t71 512x512 fmt 80; sibling draws with
  the same shaders, and the single gameplay submission of it, bind other sizes there). While
  `cursor=depth`, that draw is dropped at record time and a ring is drawn into each eye texture
  at the OS cursor position (the arrow follows `GetCursorPos` 1:1), offset per eye by the
  disparity of the scene depth under it: last frame's DLSS depth sampled at the pointer,
  converted with the main view's ClipXYZToViewPos, smoothed in disparity space. Measured: on
  the Giza map, scene 833 m, ring disparity 10-12 px at 1280 wide vs 10 px template-matched
  terrain disparity. The main menu used the 10 m backdrop in v16. In gameplay the pair's only draw binds other pages, so
  nothing is hidden or drawn there; in mono the feature is inert. Still open: the arrow's
  hotspot vs the ring centre (ring is centred on the pointer position; the arrow's tip measured
  within ~7 px of it), and checking other menu screens (inventory) bind the same pages.
  The left-stick cursor uses separate ring and dot batches. In r25, suppressing map draws at
  offsets 460 and 420 removed the ring and dot respectively. The matching menu draws had
  different offsets. The runtime identifies their sprite pages from the cursor batch sequence
  and takes the centre from the dot vertices using D3D11 stream output. In r27, stick moves
  put the menu cursor at (801,440) and (523,380) at 1280×720. Both eyes drew the substitute
  at 10 m, with 8.5 pixels of disparity. Switching back to the mouse restored pointer tracking.
- **Learning hitch:** ~300 ms, once per scene change.
- **Effect cameras not covered** (§2b): the temporal reprojection matrices, the fog's light-culling
  view matrices, the shadow reprojection (`ReprojectConsts.ScreenToWorldToShadowViewProjection`, VS)
  and other VS-stage copies (`WaterCompositingCommonConsts.waterParticleViewProj`). Only PS and CS
  shaders are tracked for effect cameras.
- **The cloud composite's far-cloud correction is unmeasured** (no far clouds were in view).
- **Not yet checked on the glasses under HypXRland.**
