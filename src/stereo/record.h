#pragma once
#include <d3d11_1.h>

#include <atomic>
#include <cstdint>
#include <functional>

namespace aco {

// Recorder for the immediate context: every call the game makes on it between two Presents is
// kept (with its arguments, COM references and any CPU data it uploads), so the frame can be
// submitted a second time with other camera constants (stereo/stereo.cpp).
//
// What is recorded: state setters, draws, dispatches, copies, clears, uploads (Map/Unmap with
// WRITE_DISCARD / WRITE_NO_OVERWRITE, UpdateSubresource) and ExecuteCommandList. What is not:
// queries (Begin/End/GetData: the second pass keeps the first pass's results), readbacks (Map
// READ, copies into staging), Get* calls, Flush. Calls made by this mod itself (return address in
// our module) and calls made while a replay runs are not recorded.
//
// Hooks that other modules already own (draws, dispatches, copies, clears, ExecuteCommandList,
// Map/Unmap/UpdateSubresource, PSSetShader, CSSetShader, ClearState) call the rec_* functions
// below; the rest are installed by rec_install.

extern std::atomic<bool> g_rec_on;
extern ID3D11DeviceContext* g_rec_imm;
extern thread_local int g_rec_replaying;
bool rec_from_us(void* ret);

// True if a call on ctx from the caller at `ret` is to be recorded.
inline bool rec_want(ID3D11DeviceContext* ctx, void* ret) {
  return g_rec_on.load(std::memory_order_relaxed) && ctx == g_rec_imm && !g_rec_replaying && !rec_from_us(ret);
}
#define ACO_REC_WANT(ctx) ::aco::rec_want((ctx), __builtin_return_address(0))

using RecFn = std::function<void(ID3D11DeviceContext*)>;
enum class RecKind : uint8_t { State, Draw, Dispatch, Copy, Clear, Upload, Execute };
void rec_push(RecFn f, RecKind k = RecKind::State);
void rec_unsupported(const char* what);  // the frame can't be replayed

// Uploads.
// rec_map may replace m->pData with a shadow; rec_unmap must then run for every Unmap on the
// immediate context (also when not recording) so the shadow reaches the real mapping.
bool rec_map(ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* m, bool detached);
// A detached upload lives in the CPU shadow until the game commits it. Set
// begin_commit at game Unmap and call rec_unmapped after the native Unmap.
D3D11_MAP rec_detached_type(ID3D11Resource* res, UINT sub, bool begin_commit = false);
bool rec_unmap(ID3D11Resource* res, UINT sub, bool notify, void* destination);
void rec_unmapped();
void rec_update(ID3D11Resource* res, UINT sub, const D3D11_BOX* box, const void* data, UINT rowpitch, UINT depthpitch,
                UINT copy_flags, bool is1);

// Already-hooked slots.
void rec_draw(UINT a, UINT b);
void rec_draw_indexed(UINT a, UINT b, INT c);
void rec_draw_instanced(UINT a, UINT b, UINT c, UINT d);
void rec_draw_indexed_instanced(UINT a, UINT b, UINT c, INT d, UINT e);
void rec_draw_indexed_instanced_indirect(ID3D11Buffer* buf, UINT off);
void rec_draw_instanced_indirect(ID3D11Buffer* buf, UINT off);
void rec_dispatch(UINT x, UINT y, UINT z);
void rec_dispatch_indirect(ID3D11Buffer* buf, UINT off);
void rec_copy_resource(ID3D11Resource* dst, ID3D11Resource* src);
void rec_copy_subresource(ID3D11Resource* dst, UINT dsub, UINT x, UINT y, UINT z, ID3D11Resource* src, UINT ssub,
                          const D3D11_BOX* box);
void rec_clear_rtv(ID3D11RenderTargetView* v, const FLOAT c[4]);
void rec_clear_dsv(ID3D11DepthStencilView* v, UINT f, FLOAT d, UINT8 s);
void rec_clear_uav_float(ID3D11UnorderedAccessView* v, const FLOAT c[4]);
void rec_clear_uav_uint(ID3D11UnorderedAccessView* v, const UINT c[4]);
void rec_execute(ID3D11CommandList* cl, BOOL restore);
void rec_ps_set_shader(ID3D11PixelShader* s, ID3D11ClassInstance* const* ci, UINT n);
void rec_cs_set_shader(ID3D11ComputeShader* s, ID3D11ClassInstance* const* ci, UINT n);
void rec_clear_state();

// Installs the recorder's own hooks on the immediate context (vtable of the immediate context's
// class only, so deferred contexts are unaffected).
void rec_install(ID3D11DeviceContext* imm);
// The *SetConstantBuffers(1) hooks (camera redirection, stereo/camera.h) on any context.
void rec_install_cb_hooks(ID3D11DeviceContext* ctx);

// Frame control (render thread, from Present).
struct RecFrame;
void rec_start();                 // begin recording a frame (drops anything recorded so far)
RecFrame* rec_stop(bool wait_for_uploads);  // stop and hand over the frame (nullptr if not recording)
bool rec_frame_ok(const RecFrame* f, const char** why);
size_t rec_frame_size(const RecFrame* f);
size_t rec_frame_bytes(const RecFrame* f);
void rec_replay(const RecFrame* f, ID3D11DeviceContext* ctx, unsigned skip_mask = 0);  // skip: 1 << RecKind
void rec_free(RecFrame* f);
void rec_log_uploads(const RecFrame* f);  // diagnostics

} // namespace aco
