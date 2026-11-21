#pragma once
#include <d3d12.h>
#include <string>

namespace bifrost {
UINT option(const wchar_t* section, const wchar_t* key, UINT fallback);
std::wstring option_text(const wchar_t* section, const wchar_t* key, const wchar_t* fallback);
void observe_queue(ID3D12CommandQueue* queue);
void observe_factory(IUnknown* adapter);
void record_composite(ID3D12GraphicsCommandList* list, UINT64 scene, UINT64 target);
void record_frame_size(ID3D12GraphicsCommandList* list, UINT width, UINT height);
void clear_frame_size(ID3D12GraphicsCommandList* list);
}  // namespace bifrost
