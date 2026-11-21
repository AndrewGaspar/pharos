# Animated sensor rigs

An external recorder can attach a rigid body to an animated character, render calibrated camera rays and stream the body's world pose on a virtual clock. Origins v1.62 supplies the `face` anchor in Discovery Tour. Its origin is the midpoint of the animated eyes, with orientation from the evaluated head joint. The basis is right, down, forward. Translation uses world metres. The original head motion is retained, including movement during locomotion.

## Capture contract

Start a Quiver lab with a fixed virtual clock, the D3D11 renderer and frame generation off. Choose the rate so each camera period is an integral number of game presents. The client checks that the clock is engaged, has the requested rate, and stays on the exact tick grid with unchanged creep, escape and overrun counters. Capture samples one pose from the game per present. Extra renders for other views consume wall time without advancing the game.

Within the active run directory, create a fresh session subdirectory with an ASCII alphanumeric, hyphen or underscore name. Write `config` and `rays`, then atomically publish a `rig.request` file containing the session name. Run one capture at a time. The text configuration is whitespace separated.

```text
PHAROS_RIG_1 anchor hz ticks stride shutter_samples paced view_count pixel_count
anchor_from_body_tx ty tz qx qy qz qw
body_from_view_tx ty tz qx qy qz qw sx sy
... one transform and projection per view ...
optional_max_turn_degrees_per_second
```

The optional final angular speed limit defaults to zero. A positive value limits head rotation along the shortest arc in virtual time. Translation and the calibrated geometry are preserved. Quaternions are Hamilton XYZW. Each view looks down its local positive Z with Y down. The horizontal and vertical projection scales are the reciprocals of the tangents of the half fields of view. The binary ray table has one little-endian `uint32 view_index, float32 u, float32 v` per output pixel, in client-defined order. UVs cover the rendered view from top left (0, 0) to bottom right (1, 1). A client can concatenate multiple lenses and output shapes in one table.

After eight warm-up presents, Pharos writes `capture.bin`. The eight-byte header is `PHARRIG1`. Each tick contains its zero-based uint32 index and seven float32 values for `world_from_body` in translation/XYZW order. When `tick % stride < shutter_samples`, the pose is followed by one float32 relative linear luminance per output pixel. All data is little-endian. A reader can tail the file and wait for complete records. Use `(tick + 0.5) / hz` as the midpoint time of a tick. Set `paced` to 1 to aim for wall-clock real time. A slow readback or writer slows the simulation and preserves the sampling sequence.

The session ends with an atomically published `status` containing `ok` or an error. A missing anchor, missing HDR pass, unreplayable image tick or failed write invalidates the capture. A process exit leaves an incomplete session. Keep only sessions with a successful status, the expected sequence and length, and valid external clock telemetry.

## Rendering and adding a title

Origins samples the floating-point scene input to its tone mapper, before the display curve and HUD. Pharos bilinearly samples each view and converts linear RGB with weights 0.2126, 0.7152 and 0.0722. The external device implementation supplies pixel-ray geometry, shutter integration, radiometric response and inertial processing. Relative engine light units require calibration for comparison to a physical sensor.

To add a game, register its named animated joint provider with `rig::register_anchor`, map its view/projection and per-instance transforms into the D3D11 replay, and supply the pre-tone-map scene-color input. A provider must return false when the rig is unavailable. The capture request and external rig geometry are independent of these title details. The renderer can draw commands submitted by the game. CPU visibility, level streaming, view-dependent shadows and temporal screen-space effects can limit peripheral coverage.
