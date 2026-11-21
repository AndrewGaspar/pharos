#pragma once
#include <d3d11.h>

#include <atomic>

namespace aco {

// Per-eye copies of the game's temporal state for stereo (stereo.cpp).
//
// Some GPU resources carry information from one frame to the next (TAA-style histories, SSAO and
// volumetric accumulation, exposure, the GI volume): the frame reads them before writing them. With
// both eyes drawn from the same frame, each eye must find its own previous frame there, not the
// other eye's. Which resources these are is learned at run time: for a few frames every draw,
// dispatch, copy and clear on every context reports what it reads and writes; a resource whose first
// access in a frame is a read, and that the previous frame or this one wrote, is temporal state.
// Each gets a twin per eye, swapped in with GPU copies around the right eye's pass.

extern std::atomic<bool> g_tmp_learning;

// Access reports (any context; cheap no-ops unless learning).
void tmp_on_draw(ID3D11DeviceContext* ctx);
void tmp_on_dispatch(ID3D11DeviceContext* ctx);
void tmp_on_copy(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src);
void tmp_on_clear(ID3D11DeviceContext* ctx, ID3D11View* v);
void tmp_on_finish_command_list(ID3D11DeviceContext* ctx, ID3D11CommandList* cl);
void tmp_on_execute(ID3D11DeviceContext* imm, ID3D11CommandList* cl);

// Ignore the compositor's copies and draws when identifying game-owned histories.
struct TemporalPresentScope {
  TemporalPresentScope();
  ~TemporalPresentScope();
  TemporalPresentScope(const TemporalPresentScope&) = delete;
  TemporalPresentScope& operator=(const TemporalPresentScope&) = delete;
};

// Render thread, from stereo's Present.
void tmp_init(ID3D11Device* dev, ID3D11DeviceContext* imm);
// Learning bookkeeping, every frame; frame_size = immediate-context calls recorded this frame.
void tmp_frame_end(bool stereo_on, ID3D11Resource* backbuffer, size_t frame_size);
void tmp_before_right(ID3D11DeviceContext* imm);  // left eye's state out, right eye's in
void tmp_after_right(ID3D11DeviceContext* imm);   // right eye's state out, left eye's in
void tmp_relearn();

} // namespace aco
