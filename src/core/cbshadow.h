#pragma once
#include <cstddef>
#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11Buffer;

namespace aco {

// CPU-side shadow copies of constant buffer contents, per (context, buffer).
//
// Constant buffers are written through Map/Unmap or UpdateSubresource, often on deferred contexts,
// so the GPU contents a given draw will see aren't readable at record time. We mirror the writes
// so a hooked draw can read "what this context last wrote into buffer X".
//
// Shadowing applies to constant buffers up to kMaxShadowBytes that are either globally enabled
// (cbshadow_set_all, used while tracing) or watched, individually (cbshadow_watch) or by size
// (cbshadow_watch_size).
constexpr uint32_t kMaxShadowBytes = 8192;

void cbshadow_install(ID3D11DeviceContext* ctx);  // hooks Map/Unmap/UpdateSubresource
void cbshadow_set_all(bool enabled);
void cbshadow_watch(ID3D11Buffer* buf);
// Shadows every constant buffer of this ByteWidth (for cbuffers rotated through a buffer pool).
void cbshadow_watch_size(uint32_t byte_width);
// Copies [offset, offset+size) of the shadow into out. Returns false if no shadow exists.
bool cbshadow_read(ID3D11DeviceContext* ctx, ID3D11Buffer* buf, uint32_t offset, uint32_t size, void* out);

} // namespace aco
