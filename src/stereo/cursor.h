#pragma once
#include <d3d11.h>

struct IDXGISwapChain;

namespace aco {

// Map/menu cursor at scene depth ([stereo] cursor=depth, stereo only).
//
// The game's white arrow is one DrawIndexedInstancedIndirect into the UI texture the
// UICompositing pass samples (t86): VS 12b61097a3a0b0e7 / PS 068233d2886bf2c8 (DXBC checksum
// hashes, ACOrigins v1.62; found live by dropping draws - see docs/STEREO.md). While active,
// that draw is dropped at record time (absent from both the game's submission and the stereo
// replay) and our own ring cursor is drawn into each eye texture before composition, offset
// per eye by the disparity of the scene depth under the OS cursor (the arrow follows
// GetCursorPos 1:1). For a controller, stream output captures the native dot position. If the hashes never match (other game build), nothing is hidden and no
// substitute is drawn: the game cursor stays.

void cursor_note_flat_menu();
void cursor_set_active(bool on);  // at Present, like g_cam_on
bool cursor_ready(ID3D11Device* dev);
bool cursor_needs_depth();
// Only the indexed indirect UI batch contains the cursor. The caller asks only when the bound pixel
// shader is classed UiSprite (the cursor's) or UiMenu (Odyssey's menu background).
enum class CursorDraw { Game, Hide, Capture };
CursorDraw cursor_prepare(ID3D11DeviceContext* ctx, ID3D11Buffer* args, UINT off);
void cursor_save_left(ID3D11DeviceContext* imm);
void cursor_restore(ID3D11DeviceContext* ctx);
// Per frame, before drawing: read the OS cursor, sample last frame's depth, issue this frame's.
void cursor_begin_frame(ID3D11DeviceContext* imm, IDXGISwapChain* sc, unsigned w, unsigned h);
bool cursor_visible();  // the game cursor was hidden and the pointer is in the window
// Draw into both eye textures (caller has swapped in our device context state).
void cursor_draw(ID3D11Device* dev, ID3D11DeviceContext* imm, ID3D11UnorderedAccessView* eye_uav[2],
                 unsigned w, unsigned h);

} // namespace aco
