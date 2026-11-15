#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;

namespace aco {

// Frame tracer. Creating the file <out>\capture arms a capture of the next kCaptureFrames frames
// into <out>\cap-<n>\events.jsonl (+ shaders\*.dxbc/.asm for every referenced shader).
void trace_install_device(ID3D11Device* dev);
void trace_install_context(ID3D11DeviceContext* ctx);
void trace_on_present(IDXGISwapChain* sc);

} // namespace aco
