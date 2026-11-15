#include "core/shaders.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "core/log.h"

namespace aco {

namespace {
std::shared_mutex g_mutex;
std::unordered_map<ID3D11DeviceChild*, ShaderInfo> g_shaders;

using PFN_Disassemble = HRESULT(WINAPI*)(const void*, SIZE_T, UINT, const char*, ID3DBlob**);
PFN_Disassemble disassembler() {
  static PFN_Disassemble fn = [] {
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    return m ? reinterpret_cast<PFN_Disassemble>(GetProcAddress(m, "D3DDisassemble")) : nullptr;
  }();
  return fn;
}

const char* kind_ext(ShaderKind k) {
  switch (k) {
    case ShaderKind::VS: return "vs";
    case ShaderKind::PS: return "ps";
    case ShaderKind::CS: return "cs";
  }
  return "xx";
}
} // namespace

std::string hex64(uint64_t v) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

void shaders_register(ID3D11DeviceChild* shader, ShaderKind kind, const void* bytecode, size_t size) {
  if (!shader || !bytecode || size < 20) return;
  ShaderInfo info;
  info.kind = kind;
  // DXBC header: "DXBC" + 16-byte checksum.
  memcpy(&info.hash, static_cast<const uint8_t*>(bytecode) + 4, sizeof(info.hash));
  info.bytecode.assign(static_cast<const uint8_t*>(bytecode), static_cast<const uint8_t*>(bytecode) + size);
  std::unique_lock lock(g_mutex);
  g_shaders[shader] = std::move(info);
}

uint64_t shader_hash(ID3D11DeviceChild* shader) {
  if (!shader) return 0;
  std::shared_lock lock(g_mutex);
  auto it = g_shaders.find(shader);
  return it == g_shaders.end() ? 0 : it->second.hash;
}

const ShaderInfo* shader_info(ID3D11DeviceChild* shader) {
  std::shared_lock lock(g_mutex);
  auto it = g_shaders.find(shader);
  return it == g_shaders.end() ? nullptr : &it->second;  // entries are never erased
}

void shader_dump(const ShaderInfo& info, const std::wstring& dir) {
  std::string base = hex64(info.hash) + "." + kind_ext(info.kind);
  std::wstring wbase(base.begin(), base.end());
  std::wstring bin = dir + L"\\" + wbase + L".dxbc";
  if (GetFileAttributesW(bin.c_str()) != INVALID_FILE_ATTRIBUTES) return;
  if (FILE* f = _wfopen(bin.c_str(), L"wb")) {
    fwrite(info.bytecode.data(), 1, info.bytecode.size(), f);
    fclose(f);
  }
  if (auto dis = disassembler()) {
    ID3DBlob* blob = nullptr;
    if (SUCCEEDED(dis(info.bytecode.data(), info.bytecode.size(), 0, nullptr, &blob)) && blob) {
      std::wstring asmp = dir + L"\\" + wbase + L".asm";
      if (FILE* f = _wfopen(asmp.c_str(), L"wb")) {
        fwrite(blob->GetBufferPointer(), 1, blob->GetBufferSize() - 1, f);
        fclose(f);
      }
      blob->Release();
    }
  }
}

} // namespace aco
