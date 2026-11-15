#pragma once
#include <d3d11.h>

namespace aco::kernels {

// Compute shaders, compiled at runtime from embedded HLSL with d3dcompiler_47.dll.
struct Kernels {
  ID3D11ComputeShader* motion = nullptr;  // engine motion buffer + camera reprojection -> DLSS MVs
  ID3D11ComputeShader* blit = nullptr;    // bilinear upscale / debug visualisation
  ID3D11SamplerState* linear_clamp = nullptr;
  ID3D11Buffer* blit_cb = nullptr;        // BlitParams, dynamic
};

bool create(ID3D11Device* dev, Kernels& k);

// Register layout of the motion kernel. It is dispatched *inside the game's command stream* with
// the TAA pass's own constant buffers rebound to the same slots, so it reads exactly the camera
// and TAA constants the engine's TAA would have used.
//   b1 = PassParams (camera matrices), b3 = InstanceParams (prev view-projection for the pass),
//   b5 = TemporalAA constants, t0 = engine motion buffer (RGBA8 packed), t1 = linear Z,
//   u0 = output RG16F, UV-space motion pointing from current to previous position.
constexpr UINT kMotionCbSlots[3] = {1, 3, 5};

enum BlitMode : UINT {
  kBlitColor = 0,     // bilinear upscale of src
  kBlitMotion = 1,    // visualise motion vectors
  kBlitDepth = 2,     // visualise device depth
};

struct BlitParams {
  float dst_size_inv[2];
  float src_size[2];
  UINT mode;
  float scale;
  float pad[2];
};

} // namespace aco::kernels
