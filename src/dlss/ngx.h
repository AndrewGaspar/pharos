#pragma once
#include <d3d11.h>

#include <nvsdk_ngx_defs.h>

#include <string>

namespace aco::ngx {

// Thin dynamic binding to the NVIDIA driver's NGX core (_nvngx.dll). We don't link NVIDIA's static
// loader library (MSVC-only); the driver DLL exports the D3D11 API directly.
//
// NVSDK_NGX_Parameter is a C++ interface built by MSVC. Its Set/Get methods are overloaded
// virtuals, which MSVC lays out per overload group in *reverse* declaration order. GCC uses
// declaration order, so we must not call through a GCC-compiled declaration of that class; Params
// calls the vtable slots explicitly instead.
class Params {
 public:
  explicit Params(NVSDK_NGX_Parameter* p = nullptr) : p_(p) {}
  NVSDK_NGX_Parameter* get() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }

  void set(const char* name, void* v);
  void set(const char* name, ID3D11Resource* v);
  void set(const char* name, int v);
  void set(const char* name, unsigned int v);
  void set(const char* name, float v);
  void set(const char* name, unsigned long long v);
  bool get(const char* name, int* v) const;
  bool get(const char* name, unsigned int* v) const;
  bool get(const char* name, float* v) const;
  bool get(const char* name, void** v) const;
  void reset();

 private:
  NVSDK_NGX_Parameter* p_;
};

bool init(ID3D11Device* dev, const std::wstring& data_path);
// The driver's NGX core (_nvngx.dll), loaded on first use (also by the Vulkan Frame Generation path).
HMODULE core_module();
void shutdown(ID3D11Device* dev);
bool initialized();
bool dlss_available();

Params alloc_params();
void destroy_params(Params& p);
NVSDK_NGX_Handle* create_feature(ID3D11DeviceContext* ctx, NVSDK_NGX_Feature feature, Params& p);
NVSDK_NGX_Result evaluate(ID3D11DeviceContext* ctx, NVSDK_NGX_Handle* h, Params& p);
void release_feature(NVSDK_NGX_Handle* h);

const char* result_str(NVSDK_NGX_Result r);

} // namespace aco::ngx
