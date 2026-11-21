#include <d3d12.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <wrl/client.h>

#include <sstream>
#include <vector>

#include "core/hook.h"
#include "dlss/ngx.h"
#include "ports/bifrost_present.h"
#include "ports/bifrost_composite.h"
#include "ports/bifrost_resources.h"
#include "ports/bifrost_replay.h"
#include "ports/bifrost_eye.h"
#include "ports/bifrost_dual.h"
#include "ports/bifrost_lists.h"
#include "ports/common.h"

using namespace aco;
using Microsoft::WRL::ComPtr;
namespace port {
extern const size_t create_queue;
extern const size_t create_root, graphics_pso, compute_pso, create_list, set_pso, reset_list;
}  // namespace port
namespace {
HMODULE system_d3d12() {
  static HMODULE mod = port::system_library(L"d3d12.dll");
  return mod;
}
template <typename Fn>
Fn real(const char* name) {
  auto mod = system_d3d12();
  return mod ? reinterpret_cast<Fn>(GetProcAddress(mod, name)) : nullptr;
}

void check_features(ID3D12Device* dev) {
  bifrost::dual::Internal internal;
  HMODULE core = ngx::core_module();
  if (!core) {
    ACO_LOG("Bifrost: NGX driver unavailable");
    return;
  }
  using Init = NVSDK_NGX_Result (*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
                                    const NVSDK_NGX_FeatureCommonInfo*);
  using Caps = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter**);
  using Destroy = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter*);
  auto init = reinterpret_cast<Init>(GetProcAddress(core, "NVSDK_NGX_D3D12_Init"));
  auto caps = reinterpret_cast<Caps>(GetProcAddress(core, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
  auto destroy = reinterpret_cast<Destroy>(GetProcAddress(core, "NVSDK_NGX_D3D12_DestroyParameters"));
  if (!init || !caps || !destroy) {
    ACO_LOG("Bifrost: D3D12 NGX API unavailable");
    return;
  }
  auto result = init(0x424946524f5354ull, out_dir().c_str(), dev, NVSDK_NGX_Version_API, nullptr);
  if (NVSDK_NGX_FAILED(result)) {
    ACO_LOG("Bifrost: NGX init %s", ngx::result_str(result));
    return;
  }
  NVSDK_NGX_Parameter* params = nullptr;
  result = caps(&params);
  if (NVSDK_NGX_FAILED(result) || !params) {
    ACO_LOG("Bifrost: NGX capabilities %s", ngx::result_str(result));
    return;
  }
  int sr = -1, fg = -1;
  ngx::Params p(params);
  p.get(NVSDK_NGX_Parameter_SuperSampling_Available, &sr);
  p.get(NVSDK_NGX_Parameter_FrameGeneration_Available, &fg);
  ACO_LOG("Bifrost: D3D12 NGX capabilities SR=%d FG=%d", sr, fg);
  destroy(params);
}

const GUID root_layout_tag = {0xaaf1f067, 0xa3c3, 0x4b85, {0x86, 0x1f, 0x59, 0x5e, 0x3f, 0xf0, 0x76, 0x39}};

// Both root signature versions expose the same register/space/table-offset
// fields.
template <typename Desc>
std::string layout(const Desc& desc) {
  std::ostringstream s;
  for (UINT i = 0; i < desc.NumParameters; ++i) {
    auto& p = desc.pParameters[i];
    s << " root" << i << "(vis" << p.ShaderVisibility << ")=";
    if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
      for (UINT j = 0; j < p.DescriptorTable.NumDescriptorRanges; ++j) {
        const auto& r = p.DescriptorTable.pDescriptorRanges[j];
        s << (j ? "," : "") << "tubs"[r.RangeType] << r.BaseShaderRegister << "/" << r.RegisterSpace << "+"
          << r.NumDescriptors << "@";
        if (r.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)
          s << "append";
        else
          s << r.OffsetInDescriptorsFromTableStart;
      }
    } else if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
      s << "constants:b" << p.Constants.ShaderRegister << "/" << p.Constants.RegisterSpace << "+"
        << p.Constants.Num32BitValues;
    } else {
      s << (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV   ? 'b'
            : p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV ? 't'
                                                               : 'u')
        << p.Descriptor.ShaderRegister << "/" << p.Descriptor.RegisterSpace;
    }
  }
  return s.str();
}

HRESULT WINAPI root_signature(ID3D12Device* dev, UINT node, const void* bytes, SIZE_T size, REFIID iid, void** out) {
  auto hr = orig<decltype(root_signature)>(dev, port::create_root)(dev, node, bytes, size, iid, out);
  if (FAILED(hr) || !out || !*out) return hr;
  static auto decode =
      real<decltype(&D3D12CreateVersionedRootSignatureDeserializer)>("D3D12CreateVersionedRootSignatureDeserializer");
  ComPtr<ID3D12VersionedRootSignatureDeserializer> reader;
  ComPtr<ID3D12RootSignature> root;
  if (!decode || FAILED(decode(bytes, size, IID_PPV_ARGS(&reader))) ||
      FAILED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&root))))
    return hr;
  auto* desc = reader->GetUnconvertedRootSignatureDesc();
  ComPtr<ID3DBlob> eye_signature;
  if (bifrost::eye::enabled()) {
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* legacy{};
    UINT root_index{};
    if (SUCCEEDED(reader->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_0, &legacy)))
      eye_signature = bifrost::eye::camera_root(legacy->Desc_1_0, root_index);
    if (eye_signature) {
      void* replacement = nullptr;
      auto native = orig<decltype(root_signature)>(dev, port::create_root);
      if (SUCCEEDED(native(dev, node, eye_signature->GetBufferPointer(), eye_signature->GetBufferSize(), iid,
                           &replacement))) {
        static_cast<IUnknown*>(*out)->Release();
        *out = replacement;
        root.Reset();
        if (FAILED(static_cast<IUnknown*>(replacement)->QueryInterface(IID_PPV_ARGS(&root)))) return hr;
        bifrost::eye::tag_root(root.Get(), root_index);
        bytes = eye_signature->GetBufferPointer();
        size = eye_signature->GetBufferSize();
      }
    }
  }
  bifrost::remember_root(root.Get(), bytes, size);
  bifrost::read_layout(root.Get(), *desc);
  std::string text;
  if (desc->Version == D3D_ROOT_SIGNATURE_VERSION_1_0)
    text = layout(desc->Desc_1_0);
  else if (desc->Version == D3D_ROOT_SIGNATURE_VERSION_1_1)
    text = layout(desc->Desc_1_1);
  if (!text.empty()) root->SetPrivateData(root_layout_tag, UINT(text.size() + 1), text.c_str());
  return hr;
}

void tag_pipeline(IUnknown* object, const D3D12_SHADER_BYTECODE& code, ID3D12RootSignature* root) {
  auto* entry = port::lookup(code.pShaderBytecode, code.BytecodeLength);
  if (!entry) return;
  ComPtr<ID3D12PipelineState> pso;
  if (FAILED(object->QueryInterface(IID_PPV_ARGS(&pso)))) return;
  port::tag(pso.Get(), entry);
  bifrost::tag_inputs(pso.Get(), root, *entry);
  if (root && !entry->sampled.exchange(true)) {
    UINT size = 0;
    root->GetPrivateData(root_layout_tag, &size, nullptr);
    std::vector<char> text(size);
    if (size && SUCCEEDED(root->GetPrivateData(root_layout_tag, &size, text.data())))
      ACO_LOG("layout %s %s%s", entry->role.c_str(), entry->detail.c_str(), text.data());
  }
}
HRESULT WINAPI graphics(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID iid, void** out) {
  auto hr = orig<decltype(graphics)>(dev, port::graphics_pso)(dev, desc, iid, out);
  if (SUCCEEDED(hr) && out && *out && desc) tag_pipeline(static_cast<IUnknown*>(*out), desc->PS, desc->pRootSignature);
  if (SUCCEEDED(hr) && out && *out && desc) {
    ComPtr<ID3D12PipelineState> pso;
    if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&pso)))) {
      bifrost::tag_hud(dev, pso.Get(), *desc);
      auto entry = port::binding(pso.Get());
      if (entry && entry->role == "PS/uicompositing.hlsl/PS_ShadePixel" &&
          entry->detail.starts_with("CompositeTextureInvSrcAlpha,"))
        bifrost::prepare_composite(dev, *desc, pso.Get());
    }
  }
  return hr;
}
HRESULT WINAPI compute(ID3D12Device* dev, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID iid, void** out) {
  auto hr = orig<decltype(compute)>(dev, port::compute_pso)(dev, desc, iid, out);
  if (SUCCEEDED(hr) && out && *out && desc) tag_pipeline(static_cast<IUnknown*>(*out), desc->CS, desc->pRootSignature);
  return hr;
}
void observe(ID3D12PipelineState* pso) {
  auto* entry = port::binding(pso);
  if (!entry || entry->bound.exchange(true, std::memory_order_relaxed)) return;
  port::bound(*entry);
  static std::atomic<bool> checked{false};
  if (!checked.exchange(true)) {
    ComPtr<ID3D12Device> dev;
    if (SUCCEEDED(pso->GetDevice(IID_PPV_ARGS(&dev)))) check_features(dev.Get());
  }
}
void WINAPI bind(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso) {
  orig<decltype(bind)>(list, port::set_pso)(list, pso);
  bifrost::bind_inputs(list, pso);
  if (auto* r = bifrost::dual::list(list)) bind(r, pso);
  observe(pso);
}
HRESULT WINAPI reset(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator, ID3D12PipelineState* pso) {
  auto hr = orig<decltype(reset)>(list, port::reset_list)(list, allocator, pso);
  if (SUCCEEDED(hr)) {
    bifrost::dual::reset_list(list, allocator, pso);
    bifrost::eye::allocator(list, allocator);
    bifrost::replay::reset(list);
    bifrost::clear_frame_size(list);
    bifrost::reset_inputs(list, pso);
    observe(pso);
  }
  return hr;
}
HRESULT WINAPI create_queue(ID3D12Device* dev, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID iid, void** out) {
  auto hr = orig<decltype(create_queue)>(dev, port::create_queue)(dev, desc, iid, out);
  ComPtr<ID3D12CommandQueue> q;
  if (SUCCEEDED(hr) && out && *out && SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&q))))
    bifrost::observe_queue(q.Get());
  return hr;
}
HRESULT WINAPI command_list(ID3D12Device* dev, UINT node, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* a,
                            ID3D12PipelineState* pso, REFIID iid, void** out) {
  auto hr = orig<decltype(command_list)>(dev, port::create_list)(dev, node, type, a, pso, iid, out);
  if (SUCCEEDED(hr) && out && *out) {
    ComPtr<ID3D12GraphicsCommandList> list;
    if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&list)))) {
      hook_vtable(list.Get(), port::set_pso, reinterpret_cast<void*>(bind));
      hook_vtable(list.Get(), port::reset_list, reinterpret_cast<void*>(reset));
      bifrost::observe_commands(list.Get());
      bifrost::dual::make_list(list.Get(), dev, node, type, a, pso);
      bifrost::eye::allocator(list.Get(), a);
      bifrost::replay::reset(list.Get());
      bifrost::reset_inputs(list.Get(), pso);
      observe(pso);
    }
  }
  return hr;
}
}  // namespace

extern "C" HRESULT WINAPI bifrost_create(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID iid, void** out) {
  static auto fn = real<decltype(&D3D12CreateDevice)>("D3D12CreateDevice");
  if (!fn) return E_NOINTERFACE;
  auto hr = fn(adapter, level, iid, out);
  if (port::active() && SUCCEEDED(hr) && out && *out) {
    ComPtr<ID3D12Device> dev;
    if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&dev)))) {
      log_init(L"bifrost.log");
      bifrost::observe_factory(adapter);
      bifrost::observe_descriptors(dev.Get());
      if (bifrost::dual::enabled()) hook_vtable(dev.Get(), port::create_queue, reinterpret_cast<void*>(create_queue));
      ACO_LOG("Bifrost: D3D12 device, requested feature level 0x%x", level);
      hook_vtable(dev.Get(), port::create_root, reinterpret_cast<void*>(root_signature));
      hook_vtable(dev.Get(), port::graphics_pso, reinterpret_cast<void*>(graphics));
      hook_vtable(dev.Get(), port::compute_pso, reinterpret_cast<void*>(compute));
      hook_vtable(dev.Get(), port::create_list, reinterpret_cast<void*>(command_list));
    }
  }
  return hr;
}

#define FORWARD(name, signature, args)              \
  extern "C" HRESULT WINAPI port_##name signature { \
    static auto fn = real<decltype(&name)>(#name);  \
    return fn ? fn args : E_NOINTERFACE;            \
  }
FORWARD(D3D12CreateRootSignatureDeserializer, (const void* p, SIZE_T n, REFIID i, void** o), (p, n, i, o))
FORWARD(D3D12CreateVersionedRootSignatureDeserializer, (const void* p, SIZE_T n, REFIID i, void** o), (p, n, i, o))
FORWARD(D3D12EnableExperimentalFeatures, (UINT n, const IID* i, void* c, UINT* s), (n, i, c, s))
FORWARD(D3D12GetInterface, (REFCLSID c, REFIID i, void** o), (c, i, o))
FORWARD(D3D12SerializeRootSignature,
        (const D3D12_ROOT_SIGNATURE_DESC* d, D3D_ROOT_SIGNATURE_VERSION v, ID3DBlob** b, ID3DBlob** e), (d, v, b, e))
FORWARD(D3D12SerializeVersionedRootSignature,
        (const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* d, ID3DBlob** b, ID3DBlob** e), (d, b, e))
FORWARD(D3D12GetDebugInterface, (REFIID i, void** o), (i, o))
