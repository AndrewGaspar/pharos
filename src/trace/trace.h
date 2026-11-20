#pragma once
#include <d3d11.h>

#include <string>

struct IDXGISwapChain;

namespace aco {

// Frame tracer (recording API; hooks live in core/ctxhooks). Creating the file <out>\capture arms a
// capture of the next frames into <out>\cap-<n>\events.jsonl (+ shaders\*.dxbc/.asm for every
// referenced shader). <out>\cbwatch.txt lists shader-hash prefixes whose constant buffer contents
// are dumped with each draw.
// Experiment aid: while <out>\skipdraw.txt exists, matching draws are dropped in core/ctxhooks.
// Forms: "<ps-hash-prefix> [count]" (pixel-shader hash prefix, optional vertex/index/instance
// count), "@<lo>-<hi>" (per-frame draw index range, all contexts), or "@<ctx>:<lo>-<hi>" /
// "@<ctx>:*" (per-context draw indices from Present; ctx ids are assigned in first-seen order).
// For bisecting a frame to one draw. Re-read with the capture trigger poll; delete to stop.
bool skipdraw_active();
bool skipdraw_match(unsigned long long ps_hash, unsigned count);
bool skipdraw_range_hit(ID3D11DeviceContext* ctx);  // once per draw: counts it, true = drop
void skipdraw_reset_ctx(ID3D11DeviceContext* ctx);  // per-context counter restart (command list end)
bool trace_capturing();
void trace_set_immediate(ID3D11DeviceContext* ctx);
void trace_draw(ID3D11DeviceContext* ctx, const char* op, const char* args);
void trace_dispatch(ID3D11DeviceContext* ctx, const char* op, const char* args);
void trace_copy_resource(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src);
void trace_copy_subresource(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT dsub, UINT x, UINT y, UINT z,
                            ID3D11Resource* src, UINT ssub, const D3D11_BOX* box);
void trace_clear_rtv(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* v, const FLOAT c[4]);
void trace_clear_dsv(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* v, UINT f, FLOAT d, UINT8 st);
void trace_clear_uav(ID3D11DeviceContext* ctx, ID3D11UnorderedAccessView* v, bool is_float);
void trace_note(ID3D11DeviceContext* ctx, const std::string& json_fields);  // e.g. "\"what\":\"dlss\""
void trace_finish_command_list(ID3D11DeviceContext* ctx, ID3D11CommandList* cl);
void trace_execute_command_list(ID3D11DeviceContext* ctx, ID3D11CommandList* cl);
void trace_on_present(IDXGISwapChain* sc);

} // namespace aco
