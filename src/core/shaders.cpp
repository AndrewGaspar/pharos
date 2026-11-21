#include "core/shaders.h"
#include "core/d3d11_title.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "core/log.h"
#include "stereo/camera.h"

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

// Open-addressing hash set of classified shader objects: lock-free probes on the hot path
// (PSSetShader), inserts serialised by g_mutex. Origins has ~60 TAA permutations alone.
constexpr size_t kInterestingCap = 4096;  // power of two
std::atomic<ID3D11DeviceChild*> g_interesting[kInterestingCap];
std::atomic<ShaderClass> g_interesting_cls[kInterestingCap];
std::atomic<size_t> g_interesting_n{0};

inline size_t slot_of(ID3D11DeviceChild* p) {
  return ((reinterpret_cast<uintptr_t>(p) >> 4) * 0x9E3779B97F4A7C15ull >> 20) & (kInterestingCap - 1);
}

bool contains(const std::vector<uint8_t>& bc, const char* needle) {
  size_t n = strlen(needle);
  if (bc.size() < n) return false;
  return std::search(bc.begin(), bc.end(), needle, needle + n) != bc.end();
}

ShaderClass classify(ShaderKind kind, const std::vector<uint8_t>& bc) {
  if (kind == ShaderKind::CS)
    return contains(bc, "MiscCullingConsts_cbuffer")                ? ShaderClass::Culling
           : contains(bc, "SkinningPassCompute_cbuffer")            ? ShaderClass::Skinning
           : cam_effect_bytecode(bc.data(), bc.size())              ? ShaderClass::CamEffect
                                                                    : ShaderClass::None;
  if (kind != ShaderKind::PS) return ShaderClass::None;
  if (contains(bc, "TemporalAA_ColorAccumulation")) {
    // Odyssey splits geometry, sky and dither into three stencil-masked draws.
    // Reconstruct the full color image once, from the geometry permutation.
    if (title::origins || (contains(bc, "TemporalAA_Depth") && contains(bc, "TemporalAA_Motion") &&
                           !contains(bc, "TemporalAA_DitherBlendColor")))
      return ShaderClass::TemporalAA;
    return ShaderClass::None;
  }
  if (contains(bc, "ConvertLinearZParams_DepthBufferSampler")) return ShaderClass::LinearZ;
  if (contains(bc, "UICompositingParameters_cbuffer")) return ShaderClass::UIComposite;
  if (cam_effect_bytecode(bc.data(), bc.size())) return ShaderClass::CamEffect;
  return ShaderClass::None;
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
  info.cls = classify(kind, info.bytecode);
  if (kind == ShaderKind::PS) {
    if (info.hash == 0x068233d2886bf2c8ULL || info.hash == 0xaa384ce949239abeULL) info.cls = ShaderClass::UiSprite;
    if (info.hash == 0x49121071b3b9679aULL) info.cls = ShaderClass::UiRichText;
    if (info.hash == 0x9b14ee478f4adcadULL) info.cls = ShaderClass::UiTourIntro;
    if (info.hash == 0xd0e87cc04dcdc27fULL) info.cls = ShaderClass::UiPlain;
  }
  if (info.cls == ShaderClass::CamEffect) cam_register_effect(shader, info.hash, bytecode, size);
  std::unique_lock lock(g_mutex);
  if (info.cls != ShaderClass::None) {
    // Serialised by g_mutex; readers are lock-free and only see published entries.
    if (g_interesting_n.load() < kInterestingCap / 2) {
      size_t i = slot_of(shader);
      while (true) {
        ID3D11DeviceChild* k = g_interesting[i].load(std::memory_order_relaxed);
        if (!k || k == shader) break;
        i = (i + 1) & (kInterestingCap - 1);
      }
      g_interesting_cls[i].store(info.cls, std::memory_order_relaxed);
      g_interesting[i].store(shader, std::memory_order_release);
      g_interesting_n.fetch_add(1);
      ACO_LOG("shader %s classified as %s", hex64(info.hash).c_str(), shader_class_name(info.cls));
    } else {
      ACO_LOG("shader %s: classification table full", hex64(info.hash).c_str());
    }
  }
  g_shaders[shader] = std::move(info);
}

ShaderClass shader_class_fast(ID3D11DeviceChild* shader) {
  if (!shader) return ShaderClass::None;
  for (size_t i = slot_of(shader);; i = (i + 1) & (kInterestingCap - 1)) {
    ID3D11DeviceChild* k = g_interesting[i].load(std::memory_order_acquire);
    if (k == shader) return g_interesting_cls[i].load(std::memory_order_relaxed);
    if (!k) return ShaderClass::None;
  }
}

const char* shader_class_name(ShaderClass c) {
  switch (c) {
    case ShaderClass::None: return "none";
    case ShaderClass::UiSprite: return "UI sprite or text";
    case ShaderClass::UiRichText: return "UI rich text";
    case ShaderClass::UiTourIntro: return "UI animated panel";
    case ShaderClass::UiPlain: return "UI sprite";
    case ShaderClass::TemporalAA: return "TemporalAA";
    case ShaderClass::LinearZ: return "LinearZ";
    case ShaderClass::UIComposite: return "UIComposite";
    case ShaderClass::Culling: return "Culling";
    case ShaderClass::Skinning: return "Skinning";
    case ShaderClass::CamEffect: return "CamEffect";
  }
  return "?";
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
