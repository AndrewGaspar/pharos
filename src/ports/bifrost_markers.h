#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <functional>

namespace bifrost {
enum class MarkerKind { None, World, Reticle, MapCursor, BowDot };
struct MarkerDepth {
  Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
  UINT width{}, height{}, pitch{}, slice_bytes{};
};
MarkerDepth marker_depth();
// Lab captures retire on the stereo presenter's existing per-slot fence.
struct MarkerCapture {
  Microsoft::WRL::ComPtr<ID3D12Resource> source, readback;
  void record(ID3D12GraphicsCommandList* list, ID3D12CommandQueue* queue);
  void finish();
};
void capture_marker_depth(ID3D12GraphicsCommandList* list, ID3D12Device* dev, ID3D12Resource* depth, bool right_eye);
MarkerKind prepare_marker_pipeline(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                                   ID3D12PipelineState* original);
bool draw_marker(ID3D12GraphicsCommandList* list, ID3D12PipelineState* original, UINT camera_slot,
                 UINT64 camera_address, MarkerKind kind, const std::function<void()>& draw);
}  // namespace bifrost
