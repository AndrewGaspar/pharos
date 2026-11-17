#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace aco {

// DXBC container checksum (the MD5 variant D3D uses; bytes 4..20 of the container).
void dxbc_checksum(const uint8_t* data, size_t size, uint8_t out[16]);

// Patches an instance-culling compute shader (MiscCullingConsts) so that only the far end of every
// LOD range moves out by `k`: right after the LOD distance is computed
//   d' = (dist * scale * FOVDistanceFactor + GeometricObjectLODDistanceBias) * GeometricObjectLODDistanceScale
// (`mul X, X, cb8[35].w`), the instruction `mul R.z, R.z, l(k)` is inserted, where R holds the
// entry's range (R.y = min, R.z = max, R.w = fade-in width; the first `ge _, d', R.z` after the
// distance identifies it). Returns false (with a reason) if the shader does not have that shape.
bool lod_patch_far(const std::vector<uint8_t>& in, float k, std::vector<uint8_t>& out, std::string* why);

} // namespace aco
