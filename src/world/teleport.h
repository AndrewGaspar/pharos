#pragma once

namespace aco {

// Programmatic player placement (test tooling; see tools/teleport, tools/where).
//
// <out>\teleport.txt  "x y z [yaw_deg] [hold=s]"  moves the player there (one write of the entity
//                     transform), checks that it took, then watches it for `hold` seconds; if the
//                     player drops through ground that isn't streamed in yet it is put back.
//                     Result: <out>\teleport.done ("ok x y z yaw ..." / "fail <reason> ...").
// <out>\where         writes <out>\where.txt: "x y z yaw_deg" of the player (and deletes the
//                     trigger).
// Checked from Present.
void teleport_on_present();

} // namespace aco
