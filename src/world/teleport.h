#pragma once

namespace aco {

// Programmatic player placement (test tooling; see tools/teleport, tools/where, tools/camera).
//
// <out>\teleport.txt  "x y z [hold=s] [drop=m]"  moves the player there: one write of the player
//                     entity's position, `drop` m (default 1) above z, then a check that it took
//                     and a watch of `hold` s (default 4); if the player drops through ground that
//                     isn't streamed in yet it is put back.
//                     Result: <out>\teleport.done ("ok x y z yaw ..." / "fail <reason> ...").
// <out>\where         writes <out>\where.txt: "x y z yaw camera cx cy cz cyaw cpitch" (player
//                     position and heading, render camera position, heading and pitch; degrees,
//                     Z up, yaw 0 = +X, 90 = +Y) and deletes the trigger.
// Checked from Present.
//
// In the Quiver lab the same is served through the lab channel (teleport_register): `quiver lab
// where`, `quiver lab teleport <i> <location | x y z [yaw [pitch]]>` (the player only; quiver lab
// turns the camera to the reply's "want" from the host); and the lab state is kept (menu | in-world)
// for `quiver lab wait
// <i> in-world`.
bool camera_at_player_head();
void teleport_on_present();
void teleport_register();

} // namespace aco
