#pragma once
#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace aco::fg_color {

void watch(IDXGISwapChain* chain);
DXGI_COLOR_SPACE_TYPE space(IDXGISwapChain* chain, DXGI_FORMAT format);
bool supported(DXGI_COLOR_SPACE_TYPE space, DXGI_FORMAT format);
bool is_hdr(DXGI_COLOR_SPACE_TYPE space);
bool needs_pq(DXGI_COLOR_SPACE_TYPE space, DXGI_FORMAT format);

// Record the conversion once per texture pair. Execute with RestoreContextState=TRUE
// on the immediate context so the present thread never disturbs the game's bindings.
class Converter {
 public:
  bool compiler_failed() const { return failed; }
  void retry_shader() { failed = false; }
  bool record(ID3D11Device* dev, ID3D11Texture2D* src, ID3D11Texture2D* dst, bool encode,
              ID3D11CommandList** result);
 private:
  bool failed = false;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> recording;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> to_pq, from_pq;
};

} // namespace aco::fg_color
