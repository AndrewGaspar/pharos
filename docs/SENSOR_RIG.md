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

For animations that snap the character between locations, select `PHAROS_RIG_2` (file) or `PHAROS_RIG_RING_2` (live). Supply two final values, the angular speed limit in degrees per second followed by the linear speed limit in metres per second. Zero disables the corresponding limit. On each tick the mounted body centre moves toward its target by at most the linear limit divided by the tick rate. Each camera and the reported body pose use the same resulting rigid transform. The binary output layout is the same for both protocol versions.

After eight warm-up presents, Pharos writes `capture.bin`. The eight-byte header is `PHARRIG1`. Each tick contains its zero-based uint32 index and seven float32 values for `world_from_body` in translation/XYZW order. When `tick % stride < shutter_samples`, the pose is followed by one float32 relative linear luminance per output pixel. All data is little-endian. A reader can tail the file and wait for complete records. Use `(tick + 0.5) / hz` as the midpoint time of a tick. Set `paced` to 1 to aim for wall-clock real time. A slow readback or writer slows the simulation and preserves the sampling sequence.

During rig capture the D3D11 recorder keeps in-progress dynamic buffer uploads in a CPU shadow. If an upload spans a Present, each sensor view can replay the last committed GPU contents. The game’s Unmap commits the new contents. A physical mapping that prevents replay can still invalidate an exposure.

The session ends with an atomically published `status` containing `ok` or an error. A missing anchor, missing HDR pass, unreplayable image tick or failed write invalidates the capture. A process exit leaves an incomplete session. Keep only sessions with a successful status, the expected sequence and length, and valid external clock telemetry.

## Bounded live transport

A client can select `PHAROS_RIG_RING_1` as the configuration magic. Before publishing the request, it creates and maps `capture.ring` in the session directory. The layout starts with the eight bytes `PHARRNG1`, then four aligned little-endian uint64 fields at offsets 8, 16, 24 and 32. They carry capacity in bytes, total bytes published, total bytes consumed and a reader cancellation flag. Initially both cursors and the flag are zero. The data region starts at byte 64 and occupies exactly the specified capacity (up to 64 MiB). The byte stream has the same header and tick layout as file capture.

One producer and one reader share the mapping. Each cursor is a monotonically increasing byte count. Its offset in the data region is the count modulo capacity. Use interprocess acquire loads and release stores. Pharos publishes only after copying, and the reader advances its cursor only after consuming the bytes. When the ring fills, the game waits for space. The producer rejects a disconnected reader or a full ring with no progress for 30 seconds. A reader sets the cancellation flag before detaching. Successful EOF requires both a drained ring and a successful session status. The client can release the mapping and remove its backing file after capture. Computation and disk use in downstream consumers depend on what they choose to retain.

## Rendering and adding a title

Origins samples the floating-point scene input to its tone mapper, before the display curve and HUD. Pharos bilinearly samples each view and converts linear RGB with weights 0.2126, 0.7152 and 0.0722. The external device implementation supplies pixel-ray geometry, shutter integration, radiometric response and inertial processing. Relative engine light units require calibration for comparison to a physical sensor.

To add a game, register its named animated joint provider with `rig::register_anchor`, map its view/projection and per-instance transforms into the D3D11 replay, and supply the pre-tone-map scene-color input. A provider must return false when the rig is unavailable. The capture request and external rig geometry are independent of these title details. The renderer can draw commands submitted by the game. CPU visibility, level streaming, view-dependent shadows and temporal screen-space effects can limit peripheral coverage.
