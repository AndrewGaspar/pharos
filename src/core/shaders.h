#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct ID3D11DeviceChild;

namespace aco {

enum class ShaderKind : uint8_t { VS, PS, CS };

// Engine passes we care about, recognised by resource/cbuffer names in the shader's reflection
// data (robust across permutations and patches, unlike bytecode hashes).
enum class ShaderClass : uint8_t {
  None,
  TemporalAA,   // TAA resolve ("TemporalAA_ColorAccumulation")
  LinearZ,      // depth linearisation; RT0 = linear Z, RT1 = raw device depth copy
  UIComposite,  // final scene + UI composite ("UICompositingParameters_cbuffer")
  Culling,      // GPU instance culling / LOD selection CS ("MiscCullingConsts_cbuffer" at b8)
  Skinning,     // character skinning CS ("SkinningPassCompute_cbuffer"); counted for NPC load stats
  CamEffect,    // PS/CS with its own copy of the camera (SSR, fog, clouds...): per-eye in stereo
};

struct ShaderInfo {
  uint64_t hash = 0;  // first 8 bytes of the DXBC container checksum
  ShaderKind kind = ShaderKind::PS;
  ShaderClass cls = ShaderClass::None;
  std::vector<uint8_t> bytecode;
};

// Registry of every shader the game creates, keyed by the D3D object pointer.
void shaders_register(ID3D11DeviceChild* shader, ShaderKind kind, const void* bytecode, size_t size);
// Returns 0 if unknown.
uint64_t shader_hash(ID3D11DeviceChild* shader);
const ShaderInfo* shader_info(ID3D11DeviceChild* shader);
// Writes <dir>\<hash>.<vs|ps|cs>.dxbc and .asm (via d3dcompiler_47's D3DDisassemble).
void shader_dump(const ShaderInfo& info, const std::wstring& dir);

std::string hex64(uint64_t v);

// Lock-free check used on hot paths (PSSetShader): class of a shader object, None for anything
// that isn't one of the few interesting shaders.
ShaderClass shader_class_fast(ID3D11DeviceChild* shader);
const char* shader_class_name(ShaderClass c);

} // namespace aco
