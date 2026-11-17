#pragma once

namespace aco {

// Debug/RE helper. When <out>\memtool.txt exists it is executed (one command per line) and
// deleted; results go to pharos.log. Addresses are hex; "rva" values are relative to the game's
// image base. Checked from Present every ~64 frames.
//   vt <rva> [bytes]        find heap objects whose first qword is base+rva (a vtable); dump them
//   vwf <rva> <off> <float> write a float at +off (hex) in every such object (vwi: int)
//   rf <addr> <count>       read 32-bit values (shown as int/float)
//   wf <addr> <float>       write a float
//   wi <addr> <int>         write a 32-bit int
//   rq <addr> <count>       read qwords (pointers into the image are marked)
//   fo <addr>               list qwords pointing into the image in the 0x800 bytes before addr
//   fp <value> [range]      find qwords pointing into [value - range, value] (heap and image)
//   pp <value> <range>      reverse pointer scan level 1 (qwords in [value - range, value])
//   pn <range>              next level (targets = previous level's hits); static (image) roots
//                           are logged as chains "rva R -> [+off] -> ..." (offsets outermost first)
//   fs x y z r             scan private RW memory for float3 within r of (x,y,z) (decimal)
//   fr x y z r | fc | fu    refine: within r of a new point / changed / unchanged since last scan
//   fd dx dy dz tol         refine: moved by (dx,dy,dz) +- tol since last scan
//   fl [n]                  list candidates
//   wq <addr> <hex> / wb <addr> <hex>   write a qword / byte
//   find <value> [max]      every aligned qword == value in writable/readonly data
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
