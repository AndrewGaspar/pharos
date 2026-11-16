#pragma once
#include "fg/fg.h"

namespace aco {

// [fg] mode=d3d12: DLSS Frame Generation through NGX D3D12 on a vkd3d-proton device of our own,
// fed through textures shared with DXVK. Same contract as fg_present.
bool fg12_present(IDXGISwapChain* sc, UINT sync, UINT flags, PFN_RealPresent real, HRESULT* hr);

} // namespace aco
