#pragma once

namespace aco {

// Debug/RE helper. When <out>\memtool.txt exists it is executed (one command per line) and
// deleted; results go to aco_dlss.log. Addresses are hex; "rva" values are relative to the game's
// image base. Checked from Present every ~64 frames.
//   vt <rva> [bytes]        find heap objects whose first qword is base+rva (a vtable); dump them
//   vwf <rva> <off> <float> write a float at +off (hex) in every such object (vwi: int)
//   rf <addr> <count>       read 32-bit values (shown as int/float)
//   wf <addr> <float>       write a float
//   wi <addr> <int>         write a 32-bit int (wq <addr> <hex>: qword, wb: byte)
//   rq <addr> <count>       read qwords (image pointers shown as RVAs)
//   find <value> [max]      every aligned qword == value in writable/readonly data (pointer search)
// (To see which code touches an address, use hardware watchpoints from the host: tools/npc-re/gw.sh.)
// An address written "r<hex>" is relative to the image base.
void memtool_poll();

// Geometry statistics (draws / indices per frame), logged every ~2 s as "geo:" lines; used to
// see the effect of LOD/draw-distance changes quantitatively.
void geo_count(unsigned long long indices, bool indirect);
// Character skinning compute dispatches (thread groups), reported with the geo stats: a proxy for
// the number of animated characters being rendered.
void skin_count(unsigned long long groups);

} // namespace aco
