#pragma once
#include <d3d12.h>

namespace bifrost {
UINT option(const wchar_t* section, const wchar_t* key, UINT fallback);
void observe_factory(IUnknown* adapter);
void record_composite(ID3D12GraphicsCommandList* list, UINT64 scene, UINT64 target);
void record_frame_size(ID3D12GraphicsCommandList* list, UINT width, UINT height);
void clear_frame_size(ID3D12GraphicsCommandList* list);
}  // namespace bifrost
