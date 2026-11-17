#pragma once

namespace aco {

// [world] tls_extend (default 1): more TLS indices than Windows/Wine's 1088 per process for the game.
//
// Every engine reader/writer lock (ctor RVA 0x60ebc0: SRWLOCK + reader count + owner + a TlsAlloc'd
// index for the per-thread read recursion count) takes one TLS index, and each Kino animation instance
// (one per animated character, created by RVA 0x71a6d0 -> 0x71a520 -> 0x708c10 -> 0x7483e0) owns one.
// Past ~1000 characters TlsAlloc fails and returns TLS_OUT_OF_INDEXES; the lock keeps -1, its read
// unlock (RVA 0x617c20) then never releases the SRW lock, and the first writer waits forever: the
// "~1000 spawned characters" freeze (docs/ENGINE_NOTES.md, "Spawned-character limit").
//
// The game calls TlsAlloc/TlsFree/TlsGetValue/TlsSetValue through its private import table (RVA
// 0x353a688..0x353a6a0, filled by the VMProtect loader). We swap those four pointers (data writes;
// no code is modified) for wrappers that pass everything through to kernel32 until fewer than
// kReserve native indices are left, then hand out "virtual" indices (>= 0x10000) whose per-thread
// values live in our own table. Native indices take one compare on the way to kernel32.
void tlsext_poll();

// Free native TLS indices (PEB bitmaps; -1 if unknown) and virtual indices handed out so far.
int tlsext_native_free();
unsigned tlsext_virtual_in_use();
bool tlsext_active();

} // namespace aco
