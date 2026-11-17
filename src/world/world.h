#pragma once
#include <functional>

struct ID3D11DeviceContext;

namespace aco {

// Draw distance / LOD (AC Origins v1.62).
//
// Geometry LOD selection and distance culling run on the GPU, in the instance-culling compute
// shaders (12 permutations, classified by "MiscCullingConsts_cbuffer"). Per instance they compute
//   d' = (distance * objectScale * FOVDistanceFactor + GeometricObjectLODDistanceBias)
//        * GeometricObjectLODDistanceScale
// and pick the LOD whose [min, max) range contains d'; beyond the last range the instance is
// culled. MiscCullingConsts (b8) also holds MinTriangleArea, the small-object cull threshold.
//
// [world] lod_distance=k (lod_mode=far, default) extends only the far end of every LOD range:
// the 7 LOD permutations are replaced, around each dispatch, by copies with `max *= k` inserted
// (world/lodpatch.h). Scaling the whole distance (lod_mode=all, the old behaviour, still used for
// k < 1) also moves the near end of far-only representations (merged proxies, lower LODs), which
// then vanish before their near replacement has been streamed in (docs/ENGINE_NOTES.md).
// (MinTriangleArea is 0 in this game, i.e. unused.) lod_mode=all applies per dispatch: the game's
// b8 contents (read through the CB shadow) are copied into our own constant buffer with the scale
// changed, bound for that dispatch only, and the game's binding is restored.
void world_on_culling_dispatch(ID3D11DeviceContext* ctx, const std::function<void()>& dispatch);
void world_on_present();

} // namespace aco
