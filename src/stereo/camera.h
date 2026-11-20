#pragma once
#include <d3d11_1.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace aco {

// Per-eye camera constants for stereo (stereo.cpp).
//
// The camera reaches the GPU through constant buffers that the game writes on deferred contexts
// and bakes into command lists, which both eyes execute. So while stereo runs, every bind of a
// camera buffer (PassParams at cb1 of a perspective, non-square view; InstanceParams at cb3 whose
// worldViewProj is that view's) is redirected to a slot in a buffer of ours. Each slot holds two
// versions, computed at bind time: the left eye's and the right eye's. Before a command list runs
// (or an immediate-context bind takes effect), the slots it uses get the version of the eye being
// drawn: the game's own submission draws the left eye, the replay the right.
//
// Data: Map(WRITE_DISCARD) of the buffer sizes involved is shadowed in cached memory (the game
// writes into the shadow, Unmap copies it to the real mapping), so the bind can read it cheaply.

extern std::atomic<bool> g_cam_on;  // redirect binds (set at Present while stereo runs)

// cbshadow's Map/Unmap hooks, any context. cam_map may replace m->pData.
void cam_map(ID3D11DeviceContext* ctx, ID3D11Resource* res, D3D11_MAP type, D3D11_MAPPED_SUBRESOURCE* m);
void cam_unmap(ID3D11DeviceContext* ctx, ID3D11Resource* res);
void cam_update(ID3D11DeviceContext* ctx, ID3D11Resource* res, const D3D11_BOX* box, const void* data);

// Bind filter, any context: if slot start+i of `stage` should be redirected, fills buf/first/num
// for it and returns a bitmask of redirected entries (bit i). Called by the *SetConstantBuffers(1)
// hooks before forwarding the game's call; they then rebind the redirected slots.
enum CamStage { kVS, kHS, kDS, kGS, kPS, kCS };
unsigned cam_filter(ID3D11DeviceContext* ctx, CamStage stage, UINT start, UINT n, ID3D11Buffer* const* bufs,
                    const UINT* first, ID3D11Buffer** out_buf, UINT* out_first, UINT* out_num);

// Effects with their own copies of the camera (SSR, volumetric fog, clouds, shadow mask
// stabilisation, sun flare): recognised at shader creation by their cbuffers' names.
bool cam_effect_bytecode(const void* bc, size_t n);  // classification (core/shaders)
void cam_register_effect(ID3D11DeviceChild* sh, uint64_t hash, const void* bc, size_t n);
// Around one draw/dispatch of such a shader (bound at `stage`, PS or CS): points its constant
// buffer slots at per-eye copies with the camera fields moved, then gives the game's back.
struct CamFx {
  static constexpr int kMax = 4;
  int n = 0;
  struct Saved {
    int st;
    UINT slot;
    ID3D11Buffer* buf;
    UINT first, num;
  } s[kMax];
};
void cam_effect_begin(ID3D11DeviceContext* ctx, CamStage stage, ID3D11DeviceChild* shader, CamFx* fx);
void cam_effect_end(ID3D11DeviceContext* ctx, CamFx* fx);

void cam_finish_command_list(ID3D11DeviceContext* ctx, ID3D11CommandList* cl);
void cam_before_execute(ID3D11DeviceContext* imm, ID3D11CommandList* cl, int eye);

void cam_init(ID3D11Device* dev, ID3D11DeviceContext* imm);
void cam_on_present(bool stereo_on);  // per-frame: eye parameters, statistics

} // namespace aco
