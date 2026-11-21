#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>

namespace aco {
// The returned conservative depth map is shared by both eyes for the entire frame.
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> marker_history_sample(ID3D11ShaderResourceView* depth);
void marker_history_filter(ID3D11DeviceContext1* ctx, ID3DDeviceContextState* private_state);
void marker_history_publish(ID3D11DeviceContext* ctx);
void marker_history_reset();
}  // namespace aco
