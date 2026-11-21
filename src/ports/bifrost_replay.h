#pragma once
#include <d3d12.h>

namespace bifrost::replay {
bool tracking();
void reset(ID3D12GraphicsCommandList* list);
void observe(ID3D12GraphicsCommandList* list);
void camera(ID3D12GraphicsCommandList* list, UINT64 address, UINT bytes);
void draw(ID3D12GraphicsCommandList* list);
void submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
void present();
}  // namespace bifrost::replay
