#pragma once
#include <d3d11.h>
#include "core/shaders.h"

namespace aco {
void markers_init(ID3D11Device* dev);
bool markers_begin(ID3D11DeviceContext* ctx, ShaderClass shader);
void markers_end(ID3D11DeviceContext* ctx);
}  // namespace aco
