# Aegis and Bifrost

Aegis is the Odyssey D3D11 port. Bifrost is the Valhalla D3D12 port. Both currently run as development builds in disposable Quiver instances. Pharos remains the Origins release.

## Build and run

Use the Pharos build prerequisites and pass the directory containing the installed game's shader bank. The packager reads that bank and writes a signature table beside the proxy. Regenerate the table after a game update. The package contains no game shader bytecode.

```sh
scripts/package-port odyssey "/path/to/Assassin's Creed Odyssey"
scripts/package-port valhalla "/path/to/Assassin's Creed Valhalla"

quiver lab edit aegis dll.d3d11=n,b env.PROTON_ENABLE_NVAPI=1 env.DXVK_ENABLE_NVAPI=1
quiver lab edit bifrost dll.d3d12=n,b env.PROTON_ENABLE_NVAPI=1
quiver lab up aegis
quiver lab launch aegis --mod build/ports/aegis/mod --wait frame
quiver lab up bifrost
quiver lab launch bifrost --mod build/ports/bifrost/mod --wait frame
```

These commands assume Odyssey and Valhalla instances named `aegis` and `bifrost`. Each run writes `aegis.log` or `bifrost.log` in the Quiver run directory. A created shader records what the game compiled. A bound shader records a pass selected by the game. Aegis also records the texture sizes and formats at the draw or dispatch.

## Aegis • Odyssey

Odyssey's D3D11 shaders retain resource and constant-buffer reflection. The temporal pass in the tested build uses t85 for color, t86 for packed motion, t87 for linear depth, t88 for the responsive mask, and t89–90 for history. The preceding depth conversion pass binds two R32F targets. The driver reports SR available on Odyssey's own device.

Enable `env.AEGIS_SR_CAPTURE=1` in the lab manifest before launch to test one offscreen SR frame. Set Odyssey's resolution modifier below 100%, enter the benchmark from the main menu, and create an empty `capture-sr` file in the current run directory (the first line of `quiver lab logs aegis`). Aegis follows the captured draw's command list through execution and evaluates NGX on private context state. It saves the engine input (`aegis-input.ppm`), the DLSS reconstruction (`aegis-sr.ppm`) and device depth (`aegis-depth.pfm`). Relaunch for a fresh capture.

On the RTX 5070 Laptop, the 66.7% benchmark produced a pre-HUD 854×480 input and a 1280×720 DLSS output. The paired depth image aligned with the market stalls and pedestrians, with zero at the sky and larger values on foreground geometry. This one-frame diagnostic resets history and uses centered jitter and zero motion. Continuous SR needs the game's per-frame jitter, decoded motion, history resets, and a replacement of the scene input at the UI compositor. Odyssey's pooled temporal constants read as zero in the GPU readback experiment.

The signature table includes the reflected offsets for the current projection and view-to-world matrices (b2), previous transform (b4), and render size and jitter (b6). These are the starting points for stereo camera offsets and motion reconstruction. Validate projection, sky motion, UI composition, and per-eye histories during gameplay before using Pharos's stereo and frame-generation paths.

## Bifrost • Valhalla

Valhalla ships named shader permutations in an SBIN bank with stripped DXBC payloads. Bifrost matches the bytecode passed to D3D12 pipeline creation and records the root signature's register spaces, ranges, visibility and table offsets. The tested bank produced 956 matching entries and 586 distinct bytecode signatures.

The signature table also decodes shader model 5.1 register declarations. In the ordinary tiled resolve, the shader declares t0–t5 in space 2 and b0 in spaces 1 and 2 (73 and 7 sixteen-byte vectors). The sky variant omits t1 and t2. The UI composite uses t0 and t1 in space 4. These differences narrow the set of descriptors to track when identifying motion and depth.

The game bound tiled temporal resolve variants for ordinary geometry, sky and dither, plus linear-depth and UI passes. NVIDIA NGX reported both Super Resolution and Frame Generation available on the game's D3D12 device. The next step is to resolve the descriptor tables to the actual color, depth, motion and history resources at draw time. Stereo needs a per-eye camera and resource lifetime through command-list submission. Frame generation also needs HUD separation, queue synchronization and presentation pacing.
