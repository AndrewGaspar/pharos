#pragma once

namespace aco {

// Debug: when <out>\dump-image exists, writes the game's in-memory executable image (decrypted by
// Denuvo/VMProtect at runtime; file offset = RVA, unreadable pages zero-filled) to
// <out>\ACOrigins.image and deletes the trigger. Checked from Present every ~64 frames.
void dump_poll();

} // namespace aco
