#pragma once
#include <d3d11.h>

namespace aco {

// Owns every ID3D11Device / ID3D11DeviceContext vtable hook and dispatches to the tracer and the
// upscaler. Per-context we track the class of the bound pixel shader (via PSSetShader) so the
// draw hooks stay cheap for the ~20k uninteresting draws per frame.
void ctxhooks_install_device(ID3D11Device* dev);
void ctxhooks_install_context(ID3D11DeviceContext* ctx);
bool ctxhooks_engine_renderer(ID3D11Device* dev);

// Original (unhooked) implementations, for modules that must bypass their own hooks.
HRESULT orig_FinishCommandList(ID3D11DeviceContext* ctx, BOOL restore, ID3D11CommandList** out);
void orig_ExecuteCommandList(ID3D11DeviceContext* ctx, ID3D11CommandList* cl, BOOL restore);

} // namespace aco
