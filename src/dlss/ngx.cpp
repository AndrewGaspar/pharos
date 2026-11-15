#include "dlss/ngx.h"

#include <windows.h>

#include <cstdio>

#include "core/log.h"

namespace aco::ngx {

namespace {

// MSVC vtable slots of NVSDK_NGX_Parameter (see header comment).
enum Slot : size_t {
  kSetVoidPtr = 0,
  kSetD3D12Res = 1,
  kSetD3D11Res = 2,
  kSetI = 3,
  kSetUI = 4,
  kSetD = 5,
  kSetF = 6,
  kSetULL = 7,
  kGetVoidPtr = 8,
  kGetD3D12Res = 9,
  kGetD3D11Res = 10,
  kGetI = 11,
  kGetUI = 12,
  kGetD = 13,
  kGetF = 14,
  kGetULL = 15,
  kReset = 16,
};

template <typename Fn>
Fn* slot(NVSDK_NGX_Parameter* p, size_t i) {
  return reinterpret_cast<Fn*>((*reinterpret_cast<void***>(p))[i]);
}

using PFN_Init_Ext = NVSDK_NGX_Result(__cdecl*)(unsigned long long, const wchar_t*, ID3D11Device*, NVSDK_NGX_Version,
                                                const NVSDK_NGX_Parameter*);
using PFN_Shutdown1 = NVSDK_NGX_Result(__cdecl*)(ID3D11Device*);
using PFN_GetParams = NVSDK_NGX_Result(__cdecl*)(NVSDK_NGX_Parameter**);
using PFN_DestroyParams = NVSDK_NGX_Result(__cdecl*)(NVSDK_NGX_Parameter*);
using PFN_CreateFeature = NVSDK_NGX_Result(__cdecl*)(ID3D11DeviceContext*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                                                     NVSDK_NGX_Handle**);
using PFN_EvaluateFeature = NVSDK_NGX_Result(__cdecl*)(ID3D11DeviceContext*, const NVSDK_NGX_Handle*,
                                                       const NVSDK_NGX_Parameter*, void*);
using PFN_ReleaseFeature = NVSDK_NGX_Result(__cdecl*)(NVSDK_NGX_Handle*);

struct Api {
  HMODULE dll = nullptr;
  PFN_Init_Ext init_ext = nullptr;
  PFN_Shutdown1 shutdown1 = nullptr;
  PFN_GetParams get_capability_params = nullptr;
  PFN_GetParams allocate_params = nullptr;
  PFN_DestroyParams destroy_params = nullptr;
  PFN_CreateFeature create_feature = nullptr;
  PFN_EvaluateFeature evaluate_feature = nullptr;
  PFN_ReleaseFeature release_feature = nullptr;
} g;

bool g_initialized = false;
bool g_dlss_available = false;

// The NGX core lives in the driver store; its directory is published in the registry. Under
// Proton the DLLs are copied into system32 instead.
HMODULE load_core() {
  wchar_t path[MAX_PATH];
  DWORD size = sizeof(path);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath", RRF_RT_REG_SZ,
                   nullptr, path, &size) == ERROR_SUCCESS) {
    std::wstring p = std::wstring(path) + L"\\_nvngx.dll";
    if (HMODULE m = LoadLibraryW(p.c_str())) {
      ACO_LOG("ngx: loaded %ls (registry NGXCore)", p.c_str());
      return m;
    }
  }
  for (const wchar_t* name : {L"_nvngx.dll", L"nvngx.dll"}) {
    if (HMODULE m = LoadLibraryW(name)) {
      GetModuleFileNameW(m, path, MAX_PATH);
      ACO_LOG("ngx: loaded %ls", path);
      return m;
    }
  }
  return nullptr;
}

template <typename T>
bool resolve(T& out, const char* name) {
  out = reinterpret_cast<T>(GetProcAddress(g.dll, name));
  if (!out) ACO_LOG("ngx: missing export %s", name);
  return out != nullptr;
}

} // namespace

void Params::set(const char* n, void* v) { slot<void(NVSDK_NGX_Parameter*, const char*, void*)>(p_, kSetVoidPtr)(p_, n, v); }
void Params::set(const char* n, ID3D11Resource* v) {
  slot<void(NVSDK_NGX_Parameter*, const char*, ID3D11Resource*)>(p_, kSetD3D11Res)(p_, n, v);
}
void Params::set(const char* n, int v) { slot<void(NVSDK_NGX_Parameter*, const char*, int)>(p_, kSetI)(p_, n, v); }
void Params::set(const char* n, unsigned int v) {
  slot<void(NVSDK_NGX_Parameter*, const char*, unsigned int)>(p_, kSetUI)(p_, n, v);
}
void Params::set(const char* n, float v) { slot<void(NVSDK_NGX_Parameter*, const char*, float)>(p_, kSetF)(p_, n, v); }
void Params::set(const char* n, unsigned long long v) {
  slot<void(NVSDK_NGX_Parameter*, const char*, unsigned long long)>(p_, kSetULL)(p_, n, v);
}
bool Params::get(const char* n, int* v) const {
  return slot<NVSDK_NGX_Result(NVSDK_NGX_Parameter*, const char*, int*)>(p_, kGetI)(p_, n, v) == NVSDK_NGX_Result_Success;
}
bool Params::get(const char* n, unsigned int* v) const {
  return slot<NVSDK_NGX_Result(NVSDK_NGX_Parameter*, const char*, unsigned int*)>(p_, kGetUI)(p_, n, v) ==
         NVSDK_NGX_Result_Success;
}
bool Params::get(const char* n, float* v) const {
  return slot<NVSDK_NGX_Result(NVSDK_NGX_Parameter*, const char*, float*)>(p_, kGetF)(p_, n, v) ==
         NVSDK_NGX_Result_Success;
}
bool Params::get(const char* n, void** v) const {
  return slot<NVSDK_NGX_Result(NVSDK_NGX_Parameter*, const char*, void**)>(p_, kGetVoidPtr)(p_, n, v) ==
         NVSDK_NGX_Result_Success;
}
void Params::reset() { slot<void(NVSDK_NGX_Parameter*)>(p_, kReset)(p_); }

const char* result_str(NVSDK_NGX_Result r) {
  static thread_local char buf[32];
  switch (r) {
    case NVSDK_NGX_Result_Success: return "Success";
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported: return "FeatureNotSupported";
    case NVSDK_NGX_Result_FAIL_PlatformError: return "PlatformError";
    case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists: return "FeatureAlreadyExists";
    case NVSDK_NGX_Result_FAIL_FeatureNotFound: return "FeatureNotFound";
    case NVSDK_NGX_Result_FAIL_InvalidParameter: return "InvalidParameter";
    case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall: return "ScratchBufferTooSmall";
    case NVSDK_NGX_Result_FAIL_NotInitialized: return "NotInitialized";
    case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat: return "UnsupportedInputFormat";
    case NVSDK_NGX_Result_FAIL_RWFlagMissing: return "RWFlagMissing";
    case NVSDK_NGX_Result_FAIL_MissingInput: return "MissingInput";
    case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "UnableToInitializeFeature";
    case NVSDK_NGX_Result_FAIL_OutOfDate: return "OutOfDate";
    case NVSDK_NGX_Result_FAIL_OutOfGPUMemory: return "OutOfGPUMemory";
    case NVSDK_NGX_Result_FAIL_UnsupportedFormat: return "UnsupportedFormat";
    case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath: return "UnableToWriteToAppDataPath";
    case NVSDK_NGX_Result_FAIL_UnsupportedParameter: return "UnsupportedParameter";
    case NVSDK_NGX_Result_FAIL_Denied: return "Denied";
    case NVSDK_NGX_Result_FAIL_NotImplemented: return "NotImplemented";
    default: snprintf(buf, sizeof(buf), "0x%08x", static_cast<unsigned>(r)); return buf;
  }
}

bool init(ID3D11Device* dev, const std::wstring& data_path) {
  if (g_initialized) return true;
  g.dll = load_core();
  if (!g.dll) {
    ACO_LOG("ngx: NGX core not found (no NVIDIA driver / Proton without PROTON_ENABLE_NVAPI?)");
    return false;
  }
  bool ok = resolve(g.init_ext, "NVSDK_NGX_D3D11_Init_Ext") && resolve(g.shutdown1, "NVSDK_NGX_D3D11_Shutdown1") &&
            resolve(g.get_capability_params, "NVSDK_NGX_D3D11_GetCapabilityParameters") &&
            resolve(g.allocate_params, "NVSDK_NGX_D3D11_AllocateParameters") &&
            resolve(g.destroy_params, "NVSDK_NGX_D3D11_DestroyParameters") &&
            resolve(g.create_feature, "NVSDK_NGX_D3D11_CreateFeature") &&
            resolve(g.evaluate_feature, "NVSDK_NGX_D3D11_EvaluateFeature") &&
            resolve(g.release_feature, "NVSDK_NGX_D3D11_ReleaseFeature");
  if (!ok) return false;

  // Arbitrary application id; DLSS-SR does not require an NVIDIA-issued one.
  constexpr unsigned long long kAppId = 0x41434f444c5353ull;  // "ACODLSS"
  NVSDK_NGX_Result r = g.init_ext(kAppId, data_path.c_str(), dev, NVSDK_NGX_Version_API, nullptr);
  ACO_LOG("ngx: D3D11_Init_Ext -> %s", result_str(r));
  if (NVSDK_NGX_FAILED(r)) return false;
  g_initialized = true;

  NVSDK_NGX_Parameter* caps = nullptr;
  r = g.get_capability_params(&caps);
  if (NVSDK_NGX_FAILED(r) || !caps) {
    ACO_LOG("ngx: GetCapabilityParameters -> %s", result_str(r));
    return true;
  }
  Params c(caps);
  int available = 0, needs_update = 0;
  unsigned int min_major = 0, min_minor = 0;
  int init_result = 0;
  c.get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
  c.get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_update);
  c.get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &min_major);
  c.get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &min_minor);
  c.get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init_result);
  ACO_LOG("ngx: DLSS-SR available=%d needs_driver_update=%d (min %u.%u) feature_init_result=%s", available,
          needs_update, min_major, min_minor, result_str(static_cast<NVSDK_NGX_Result>(init_result)));
  g_dlss_available = available != 0;
  g.destroy_params(caps);
  return true;
}

void shutdown(ID3D11Device* dev) {
  if (g_initialized && g.shutdown1) g.shutdown1(dev);
  g_initialized = false;
}

bool initialized() { return g_initialized; }
bool dlss_available() { return g_dlss_available; }

Params alloc_params() {
  NVSDK_NGX_Parameter* p = nullptr;
  if (g_initialized) g.allocate_params(&p);
  return Params(p);
}

void destroy_params(Params& p) {
  if (p) g.destroy_params(p.get());
  p = Params();
}

NVSDK_NGX_Handle* create_feature(ID3D11DeviceContext* ctx, NVSDK_NGX_Feature feature, Params& p) {
  NVSDK_NGX_Handle* h = nullptr;
  NVSDK_NGX_Result r = g.create_feature(ctx, feature, p.get(), &h);
  ACO_LOG("ngx: CreateFeature(%d) -> %s", feature, result_str(r));
  return NVSDK_NGX_SUCCEED(r) ? h : nullptr;
}

NVSDK_NGX_Result evaluate(ID3D11DeviceContext* ctx, NVSDK_NGX_Handle* h, Params& p) {
  return g.evaluate_feature(ctx, h, p.get(), nullptr);
}

void release_feature(NVSDK_NGX_Handle* h) {
  if (h && g.release_feature) g.release_feature(h);
}

} // namespace aco::ngx
