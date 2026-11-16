#pragma once

namespace aco {

// Debug: when <out>\peek exists, logs memory values of the game image for N consecutive frames.
// File format (one per line):  frames=<n>   and   <name> <rva-hex> [<offset-hex> ...] <type>
// e.g. "dt 48b4a10 18 f32" reads *(base+0x48b4a10) as a pointer, then the float at +0x18.
// type: f32 | u32 | i32 | u64 | ptr.
void peek_on_present();

} // namespace aco
