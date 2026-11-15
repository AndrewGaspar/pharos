#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct ID3D11DeviceChild;

namespace aco {

enum class ShaderKind : uint8_t { VS, PS, CS };

struct ShaderInfo {
  uint64_t hash = 0;  // first 8 bytes of the DXBC container checksum
  ShaderKind kind = ShaderKind::PS;
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

} // namespace aco
