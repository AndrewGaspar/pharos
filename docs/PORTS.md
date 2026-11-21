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

These commands assume Odyssey and Valhalla instances named `aegis` and `bifrost`. Each run writes `aegis.log` or `bifrost.log` in the Quiver run directory. Use separate launches for frame generation and stereo, as with Pharos.

## Aegis • Odyssey

The main Aegis build runs the Pharos D3D11 rendering path with Odyssey's texture slots, constant-buffer ranges, projection layout, config file and game settings path. Its default `aegis.ini` enables DLSS SR and 2× FG. To run stereo, set `[stereo] mode=sbs` before launch. The compositor and per-eye temporal histories use the same scene twice per simulation step. Origins-specific world hooks are excluded from the Aegis binary.

Odyssey's temporal pass binds color at t85, packed motion at t86, linear depth at t87, the responsive mask at t88, and history at t89–90. The depth converter's second R32F target carries device depth. The CPU upload mirror reads the original b6 jitter during command recording. The motion shader uses the current camera in b2 and previous object projection in b4. The full-frame DLSS evaluation runs once at the geometry draw and the game still writes its own geometry, sky and dither histories. The UI compositor then samples the DLSS reconstruction underneath the HUD.

On the RTX 5070 Laptop, the 66.7% benchmark rendered at 854×480 and presented at 1280×720. Run r10 saw eight jitter positions and one SR evaluation and compositor substitution per rendered frame. The engine camera moved through the benchmark and 2× FG sustained about 30 rendered plus 30 generated frames per second. In verification sample 45, moving pixels differed by 14.16/255 between the real neighbors and the generated image differed from each by 9.71 and 9.70. The 4× benchmark in r13 sustained 15 rendered plus 45 generated frames per second at 60 Hz. Its moving-scene verification sample 75 measured differences of 22.03 and 21.79/255 from the neighboring real images. Stereo run r11 reconstructed each eye independently. On the market approach, 275 of 300 presents used a fresh stereo pair and 25 held the preceding pair. The foreground floor and distant hill differed in horizontal disparity. In r12, preserving the flat HUD camera reduced the measured binocular offset of the main-menu controls and the benchmark hint text to zero pixels. Game-specific world markers, aiming and cursor depth remain to be validated.

For the original single-frame diagnostic, package `odyssey-probe` and launch with `build/ports/aegis-probe/mod` and lab environment `AEGIS_SR_CAPTURE=1`. Touch `capture-sr` in the current run directory during the benchmark to save the input, the centered-jitter reset frame, and the paired raw device depth. The main renderer uses the game's per-frame jitter and decoded motion.

## Bifrost • Valhalla

Bifrost matches named permutations in the SBIN bank against D3D12 PSOs. It decodes SM5.1 register declarations and root-signature ranges and follows CPU descriptor writes and copies into the active graphics descriptor tables. In the tested bank, 956 matching entries reduced to 586 distinct shader signatures.

In the tiled TAA geometry draw, space 2 holds six inputs at t0–t5 (RGBA8, RGBA8, R32F, R8, RGBA8, R8) and three render targets (RGBA8, RGBA8, R8). The resource bound as t2 was the linear-depth pass's RT0. That pass also writes an R32F device-depth copy to RT1. The geometry draw's RT0 reappeared at t1/space4 in the UI composite. These identifications come from matching the resource objects at actual draws in run r6.

The active camera CBV is b0/space1, and the temporal CBV is b0/space2. Touch `capture-camera` in a Bifrost run directory to write their current contents to `bifrost-camera.tsv`. Run r7 found current/previous eye positions at float4 rows 2/3, current camera-to-world rows 6–9, projection 26–29, view-projection 30–33, previous view-projection 38–41 and inverse projection 69–72. Temporal rows 1/2 held display/render sizes and their reciprocals. Row 3.xy held jitter in pixels. The sampled jitter (−0.125, −0.388889) agreed with the projection's offsets at 1280×720.

NGX reported SR and FG available on Valhalla's D3D12 device. The native command-list work still needs motion reconstruction, feature evaluation, frame presentation and independent per-eye command streams and histories.
