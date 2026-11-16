#pragma once

namespace aco {

// Debug/RE helper. When <out>\memtool.txt exists it is executed (one command per line) and
// deleted; results go to aco_dlss.log. Addresses are hex; "rva" values are relative to the game's
// image base. Checked from Present every ~64 frames.
//   vt <rva> [bytes]        find heap objects whose first qword is base+rva (a vtable); dump them
//   vwf <rva> <off> <float> write a float at +off (hex) in every such object (vwi: int)
//   rf <addr> <count>       read 32-bit values (shown as int/float)
//   wf <addr> <float>       write a float
//   wi <addr> <int>         write a 32-bit int
void memtool_poll();

// Geometry statistics (draws / indices per frame), logged every ~2 s as "geo:" lines; used to
// see the effect of LOD/draw-distance changes quantitatively.
void geo_count(unsigned long long indices, bool indirect);

} // namespace aco
