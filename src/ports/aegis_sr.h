#pragma once
#include <d3d11.h>

namespace port {
struct Entry;
}
namespace aegis {
bool capturing();
void capture_draw(ID3D11DeviceContext* ctx, const port::Entry& entry);
void finished_list(ID3D11DeviceContext* ctx, ID3D11CommandList* list);
void after_execute(ID3D11DeviceContext* ctx, ID3D11CommandList* list);
}  // namespace aegis
