#pragma once
#include <d3d11.h>
#include "core/shaders.h"

namespace aco {
void markers_init(ID3D11Device* dev);
bool panel_begin(ID3D11DeviceContext* ctx, ShaderClass shader, UINT* caption_offset);
enum class MarkerDraw { Game, Shift, RecordLogo };
MarkerDraw markers_begin(ID3D11DeviceContext* ctx, ShaderClass shader);
void markers_end(ID3D11DeviceContext* ctx, MarkerDraw draw);
}  // namespace aco
