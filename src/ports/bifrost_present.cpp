#include "ports/bifrost_present.h"

#include <dxgi1_4.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <array>
#include <thread>
#include <mutex>

#include "core/hook.h"
#include "core/vtidx.h"
#include "dlss/ngx.h"
#include "fg/camera.h"
#include "ports/bifrost_scene.h"
#include "ports/bifrost_verify.h"
#include "ports/common.h"

using Microsoft::WRL::ComPtr;
using namespace aco;
extern "C" IMAGE_DOS_HEADER __ImageBase;
namespace port {
extern const size_t execute;
}
namespace bifrost {
namespace {
const GUID session_tag{0x4266010b, 0xa117, 0x43a1, {0xb9, 0x41, 0xad, 0x64, 0x23, 0x04, 0x31, 0xc9}};
const GUID frame_tag{0x4266010b, 0xa117, 0x43a1, {0xb9, 0x41, 0xad, 0x64, 0x23, 0x04, 0x31, 0xca}};
const GUID composite_tag{0x4266010b, 0xa117, 0x43a1, {0xb9, 0x41, 0xad, 0x64, 0x23, 0x04, 0x31, 0xcb}};
struct Composite {
  UINT64 scene, target;
};
std::mutex handoff;
const std::wstring& ini_file() {
  static const auto ini = [] {
    wchar_t file[MAX_PATH]{};
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), file, MAX_PATH);
    std::wstring path(file);
    return path.substr(0, path.find_last_of(L"\\/") + 1) + L"bifrost.ini";
  }();
  return ini;
}
struct Extent {
  UINT width{}, height{};
};
struct FeatureDesc {
  UINT width{}, height{}, render_width{}, render_height{};
  DXGI_FORMAT format{};
  bool operator==(const FeatureDesc&) const = default;
};
using CreateFeature = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                                           NVSDK_NGX_Handle**);
using Parameters = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter**);
using Destroy = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter*);
using ReleaseFeature = NVSDK_NGX_Result (*)(NVSDK_NGX_Handle*);
template <class Fn>
Fn ngx_proc(const char* name) {
  const auto dll = ngx::core_module();
  return dll ? reinterpret_cast<Fn>(GetProcAddress(dll, name)) : nullptr;
}

// Owned by the swapchain. Holding its queue and device does not keep the
// swapchain alive.
class Session final : public IUnknown {
 public:
  explicit Session(ID3D12CommandQueue* q) : queue(q) {
    verify = GetPrivateProfileIntW(L"fg", L"verify", 0, ini_file().c_str()) != 0;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
    *out = static_cast<IUnknown*>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto n = --refs;
    if (!n) delete this;
    return n;
  }
  // Fullscreen flip-model chains expose the current physical buffer to D3D12.
  // Queue both copies around the extra flip so the second present carries the untouched real frame.
  template <class Flip>
  HRESULT present(IDXGISwapChain* chain, UINT flags, Flip&& flip) {
    std::lock_guard guard(lock);
    if (flags & (DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_WAIT | DXGI_PRESENT_RESTART)) return flip();
    Scene scene;
    {
      std::lock_guard handoff_guard(handoff);
      scene = recorded_scene(queue.Get());
    }
    if (!scene.info.serial || scene.info.serial == seen || !scene.motion || !scene.depth) {
      reset_history = true;
      return flip();
    }
    seen = scene.info.serial;
    ComPtr<IDXGISwapChain3> sc;
    ComPtr<ID3D12Resource> bb;
    if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&sc))) ||
        FAILED(sc->GetBuffer(sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb))))
      return flip();
    auto bd = bb->GetDesc();
    FeatureDesc wanted{UINT(bd.Width), bd.Height, scene.info.width, scene.info.height, bd.Format};
    if (wanted != last) prepare(wanted);
    if (!feature || !real_frame || !generated || !begin()) return flip();
    auto camera = camera_from(scene.info.camera, true);
    if (!camera.ok) {
      list->Close();
      reset_history = true;
      return flip();
    }
    const bool check = verify && !reset_history && (frame_id % 60 == 0) && verifier.begin(list.Get(), real_frame.Get());
    transition(list.Get(), bb.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(list.Get(), real_frame.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(real_frame.Get(), bb.Get());
    transition(list.Get(), real_frame.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list.Get(), bb.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    ngx::Params p(raw_params);
    p.reset();
    p.set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, real_frame.Get());
    p.set(NVSDK_NGX_DLSSG_Parameter_Depth, scene.depth.Get());
    p.set(NVSDK_NGX_DLSSG_Parameter_MVecs, scene.motion.Get());
    p.set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolated, generated.Get());
    p.set(NVSDK_NGX_DLSSG_Parameter_ClipToCameraView, static_cast<void*>(camera.clip_to_view));
    p.set(NVSDK_NGX_DLSSG_Parameter_CameraViewToClip, static_cast<void*>(camera.view_to_clip));
    p.set(NVSDK_NGX_DLSSG_Parameter_ClipToPrevClip, static_cast<void*>(camera.clip_to_prev));
    p.set(NVSDK_NGX_DLSSG_Parameter_PrevClipToClip, static_cast<void*>(camera.prev_to_clip));
    static float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    p.set(NVSDK_NGX_DLSSG_Parameter_ClipToLensClip, static_cast<void*>(identity));
    set_camera_basis(p, camera);
    p.set(NVSDK_NGX_DLSSG_Parameter_MvecScaleX, float(wanted.render_width));
    p.set(NVSDK_NGX_DLSSG_Parameter_MvecScaleY, float(wanted.render_height));
    p.set(NVSDK_NGX_DLSSG_Parameter_JitterOffsetX, scene.info.jitter[0]);
    p.set(NVSDK_NGX_DLSSG_Parameter_JitterOffsetY, scene.info.jitter[1]);
    p.set(NVSDK_NGX_DLSSG_Parameter_MvecInvalidValue, 1.17549435e-38f);
    p.set(NVSDK_NGX_DLSSG_Parameter_LinearizedDepth_Scale, 1.0f);
    p.set(NVSDK_NGX_DLSSG_Parameter_LinearizedDepth_NearFarPartition, 600.0f);
    p.set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, ++frame_id);
    p.set(NVSDK_NGX_DLSSG_Parameter_ColorBuffersHDR, 0u);
    p.set(NVSDK_NGX_DLSSG_Parameter_DepthInverted, 1u);
    p.set(NVSDK_NGX_DLSSG_Parameter_CameraMotionIncluded, 1u);
    p.set(NVSDK_NGX_DLSSG_Parameter_Reset, unsigned(reset_history));
    p.set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectWidth, wanted.render_width);
    p.set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectHeight, wanted.render_height);
    p.set(NVSDK_NGX_DLSSG_Parameter_DepthSubrectWidth, wanted.render_width);
    p.set(NVSDK_NGX_DLSSG_Parameter_DepthSubrectHeight, wanted.render_height);
    p.set(NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectWidth, wanted.width);
    p.set(NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectHeight, wanted.height);
    p.set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectWidth, wanted.width);
    p.set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectHeight, wanted.height);
    p.set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, 1u);
    p.set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
    auto r = evaluate(list.Get(), feature, p.get(), nullptr);
    if (frame_id <= 3 || frame_id % 300 == 0)
      ACO_LOG("Bifrost: FG evaluate #%u %s, jitter %.3f %.3f, camera %.2f %.2f %.2f, real %u generated %u", frame_id,
              ngx::result_str(r), scene.info.jitter[0], scene.info.jitter[1], camera.pos[0], camera.pos[1],
              camera.pos[2], real_presents, generated_presents);
    if (NVSDK_NGX_FAILED(r)) {
      list->Close();
      reset_history = true;
      return flip();
    }
    if (check) verifier.finish(list.Get(), generated.Get(), real_frame.Get());
    bool insert = !reset_history;
    reset_history = false;
    if (insert) copy_to_buffer(bb.Get(), generated.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!submit()) return flip();
    slots[current].scene = scene;
    if (check && wait_for(submitted)) verifier.report();
    if (insert) {
      pace(chain);
      auto extra = flip();
      if (extra != S_OK) {
        reset_history = true;
        return extra;
      }
      ++generated_presents;
      bb.Reset();
      if (FAILED(sc->GetBuffer(sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb))) || !begin())
        return DXGI_ERROR_DEVICE_RESET;

      copy_to_buffer(bb.Get(), real_frame.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      if (!submit()) return DXGI_ERROR_DEVICE_RESET;
    }
    pace(chain);
    ++real_presents;
    return flip();
  }

 private:
  using Clock = std::chrono::steady_clock;
  void pace(IDXGISwapChain* chain) {
    if (!refresh_checked) {
      refresh_checked = true;
      UINT hz = GetPrivateProfileIntW(L"fg", L"present_hz", 0, ini_file().c_str());
      if (!hz) {
        ComPtr<IDXGIOutput> output;
        DXGI_OUTPUT_DESC output_desc{};
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (SUCCEEDED(chain->GetContainingOutput(&output)) && SUCCEEDED(output->GetDesc(&output_desc)) &&
            EnumDisplaySettingsW(output_desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode))
          hz = mode.dmDisplayFrequency;
      }
      if (hz > 1 && hz <= 1000) period = std::chrono::nanoseconds(1000000000ull / hz);
      ACO_LOG("Bifrost: display cadence %u Hz", hz);
    }
    if (!period.count()) return;
    auto now = Clock::now();
    if (next_flip > now) std::this_thread::sleep_until(next_flip);
    now = Clock::now();
    next_flip = (now > next_flip + period ? now : next_flip) + period;
  }
  void prepare(const FeatureDesc& wanted) {
    last = wanted;
    if (!open_commands()) return;
    if (!drain()) return;
    reset_history = true;
    real_frame.Reset();
    generated.Reset();
    if (feature) {
      release(feature);
      feature = nullptr;
    }
    ComPtr<ID3D12Device> dev;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&dev))) ||
        FAILED(make_texture(dev.Get(), wanted.width, wanted.height, wanted.format, D3D12_RESOURCE_FLAG_NONE,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, real_frame.GetAddressOf())) ||
        FAILED(make_texture(dev.Get(), wanted.width, wanted.height, wanted.format,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            generated.GetAddressOf())))
      return;
    if (!begin()) return;
    ngx::Params p(raw_params);
    p.reset();
    p.set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    p.set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    p.set(NVSDK_NGX_Parameter_Width, wanted.width);
    p.set(NVSDK_NGX_Parameter_Height, wanted.height);
    p.set(NVSDK_NGX_DLSSG_Parameter_Width, wanted.width);
    p.set(NVSDK_NGX_DLSSG_Parameter_Height, wanted.height);
    p.set(NVSDK_NGX_DLSSG_Parameter_InternalWidth, wanted.render_width);
    p.set(NVSDK_NGX_DLSSG_Parameter_InternalHeight, wanted.render_height);
    p.set(NVSDK_NGX_DLSSG_Parameter_BackbufferFormat, UINT(wanted.format));
    p.set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, 1u);
    p.set(NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 0u);
    const auto result = create(list.Get(), NVSDK_NGX_Feature_FrameGeneration, p.get(), &feature);
    const auto closed = list->Close();
    if (SUCCEEDED(closed) && NVSDK_NGX_SUCCEED(result)) {
      ID3D12CommandList* commands[]{list.Get()};
      queue->ExecuteCommandLists(1, commands);
      signal();
    }
    ACO_LOG(
        "Bifrost: native FG create %ux%u -> %ux%u format %u on the "
        "presenting graphics queue: %s, list 0x%08lx",
        wanted.render_width, wanted.render_height, wanted.width, wanted.height, UINT(wanted.format),
        ngx::result_str(result), closed);
  }

 private:
  ~Session() {
    drain();
    if (feature) release(feature);
    if (raw_params) destroy(raw_params);
  }
  bool open_commands() {
    if (raw_params) return true;
    evaluate = ngx_proc<decltype(evaluate)>("NVSDK_NGX_D3D12_EvaluateFeature");
    create = ngx_proc<CreateFeature>("NVSDK_NGX_D3D12_CreateFeature");
    release = ngx_proc<ReleaseFeature>("NVSDK_NGX_D3D12_ReleaseFeature");
    destroy = ngx_proc<Destroy>("NVSDK_NGX_D3D12_DestroyParameters");
    const auto alloc = ngx_proc<Parameters>("NVSDK_NGX_D3D12_AllocateParameters");
    if (!alloc || !create || !release || !destroy || !evaluate) return false;
    ComPtr<ID3D12Device> dev;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&dev))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
      return false;
    for (auto& slot : slots) {
      if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))) ||
          FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&slot.commands))) ||
          FAILED(slot.commands->Close()))
        return false;
    }
    return NVSDK_NGX_SUCCEED(alloc(&raw_params)) && raw_params;
  }
  bool begin() {
    current = (current + 1) % slots.size();
    auto& slot = slots[current];
    if (!wait_for(slot.fence_value)) return false;
    slot.scene = {};
    list = slot.commands;
    return SUCCEEDED(slot.allocator->Reset()) && SUCCEEDED(list->Reset(slot.allocator.Get(), nullptr));
  }
  bool submit() {
    if (FAILED(list->Close())) return false;
    ID3D12CommandList* batch[] = {list.Get()};
    queue->ExecuteCommandLists(1, batch);
    return signal();
  }
  void copy_to_buffer(ID3D12Resource* bb, ID3D12Resource* src, D3D12_RESOURCE_STATES state) {
    transition(list.Get(), src, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(list.Get(), bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(bb, src);
    transition(list.Get(), bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    transition(list.Get(), src, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
  }
  bool wait_for(UINT64 value) {
    if (!value) return true;
    auto done = fence->GetCompletedValue();
    return done != UINT64_MAX && (done >= value || SUCCEEDED(fence->SetEventOnCompletion(value, nullptr)));
  }
  bool drain() { return wait_for(submitted); }
  bool signal() {
    if (FAILED(queue->Signal(fence.Get(), submitted + 1))) {
      ACO_LOG("Bifrost: failed to signal the presentation queue fence");
      return false;
    }
    slots[current].fence_value = ++submitted;
    return true;
  }
  NVSDK_NGX_Result (*evaluate)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
                               void*){};
  VerifyFrames verifier;
  bool verify{};
  bool refresh_checked{};
  std::chrono::nanoseconds period{};
  Clock::time_point next_flip{};
  struct Slot {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    Scene scene;
    UINT64 fence_value{};
  };
  std::array<Slot, 8> slots;
  size_t current{};
  ComPtr<ID3D12Resource> real_frame, generated;
  bool reset_history = true;
  UINT frame_id{}, real_presents{}, generated_presents{};
  UINT64 seen{};
  std::atomic<ULONG> refs{1};
  std::mutex lock;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  UINT64 submitted{};
  NVSDK_NGX_Handle* feature{};
  NVSDK_NGX_Parameter* raw_params{};
  Destroy destroy{};
  CreateFeature create{};
  ReleaseFeature release{};
  FeatureDesc last{};
};
thread_local bool in_present = false;
template <class Flip>
HRESULT on_present(IDXGISwapChain* chain, UINT flags, Flip&& flip) {
  // A DXGI implementation may implement Present via Present1 (or vice versa).

  ComPtr<IUnknown> session;
  UINT n = sizeof(void*);
  if (in_present || FAILED(chain->GetPrivateData(session_tag, &n, session.GetAddressOf())) || !session) return flip();
  in_present = true;
  HRESULT hr = static_cast<Session*>(session.Get())->present(chain, flags, flip);
  in_present = false;
  return hr;
}
HRESULT WINAPI present(IDXGISwapChain* chain, UINT interval, UINT flags) {
  return on_present(chain, flags,
                    [&] { return orig<decltype(present)>(chain, vt::SwapChain_Present)(chain, interval, flags); });
}
HRESULT WINAPI present1(IDXGISwapChain1* chain, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
  if (params && (params->DirtyRectsCount || params->pScrollRect))
    return orig<decltype(present1)>(chain, vt::SwapChain1_Present1)(chain, interval, flags, params);
  return on_present(chain, flags, [&] {
    return orig<decltype(present1)>(chain, vt::SwapChain1_Present1)(chain, interval, flags, params);
  });
}
void WINAPI execute(ID3D12CommandQueue* q, UINT count, ID3D12CommandList* const* lists) {
  static std::atomic<unsigned> trace_count{};
  if (trace_count.load(std::memory_order_relaxed) < 24) {
    for (UINT i = 0; i < count; ++i) {
      Composite ui{};
      UINT bytes = sizeof(ui);
      bool composite = SUCCEEDED(lists[i]->GetPrivateData(composite_tag, &bytes, &ui));
      auto scene = recorded_scene(lists[i]);
      if ((composite || scene.info.serial) && trace_count.fetch_add(1) < 24)
        ACO_LOG("Bifrost: queue %p list %p at %u/%u TAA #%llu, composite scene %llx target %llx", q, lists[i], i, count,
                scene.info.serial, ui.scene, ui.target);
    }
  }
  orig<decltype(execute)>(q, port::execute)(q, count, lists);
  for (UINT i = count; i > 0; --i) {
    Extent size{};
    UINT bytes = sizeof(size);
    if (SUCCEEDED(lists[i - 1]->GetPrivateData(frame_tag, &bytes, &size)) && size.width) {
      const auto scene = recorded_scene(lists[i - 1]);
      std::lock_guard guard(handoff);
      forward_scene(q, scene);
      q->SetPrivateData(frame_tag, sizeof(size), &size);
      break;
    }
  }
}
void attach(IUnknown* creator, IDXGISwapChain* chain) {
  ComPtr<ID3D12CommandQueue> queue;
  if (!creator || !chain || FAILED(creator->QueryInterface(IID_PPV_ARGS(&queue))) ||
      queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
    return;
  if (!GetPrivateProfileIntW(L"fg", L"enabled", 1, ini_file().c_str())) return;
  auto* session = new Session(queue.Get());
  const auto hr = chain->SetPrivateDataInterface(session_tag, session);
  session->Release();
  if (FAILED(hr)) return;
  hook_vtable(queue.Get(), port::execute, reinterpret_cast<void*>(execute));
  hook_vtable(chain, vt::SwapChain_Present, reinterpret_cast<void*>(present));
  ComPtr<IDXGISwapChain1> sc1;
  if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&sc1)))) {
    hook_vtable(sc1.Get(), vt::SwapChain_Present, reinterpret_cast<void*>(present));
    hook_vtable(sc1.Get(), vt::SwapChain1_Present1, reinterpret_cast<void*>(present1));
  }
  ACO_LOG("Bifrost: attached to the D3D12 swapchain and its graphics queue");
}
HRESULT WINAPI swap(IDXGIFactory* f, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
  auto hr = orig<decltype(swap)>(f, vt::Factory_CreateSwapChain)(f, device, desc, out);
  if (SUCCEEDED(hr) && out) attach(device, *out);
  return hr;
}
HRESULT WINAPI swap_hwnd(IDXGIFactory2* f, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                         const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* restrict_to, IDXGISwapChain1** out) {
  auto hr =
      orig<decltype(swap_hwnd)>(f, vt::Factory2_CreateSwapChainForHwnd)(f, device, hwnd, desc, fs, restrict_to, out);
  if (SUCCEEDED(hr) && out) attach(device, *out);
  return hr;
}
}  // namespace
void observe_factory(IUnknown* object) {
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory> factory;
  if (object && SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&adapter)))) adapter->GetParent(IID_PPV_ARGS(&factory));
  if (!factory) {
    const auto dll = GetModuleHandleW(L"dxgi.dll");
    auto make =
        dll ? reinterpret_cast<decltype(&CreateDXGIFactory1)>(GetProcAddress(dll, "CreateDXGIFactory1")) : nullptr;
    if (make) make(IID_PPV_ARGS(&factory));
  }
  if (!factory) return;
  hook_vtable(factory.Get(), vt::Factory_CreateSwapChain, reinterpret_cast<void*>(swap));
  ComPtr<IDXGIFactory2> factory2;
  if (SUCCEEDED(factory.As(&factory2))) {
    hook_vtable(factory2.Get(), vt::Factory_CreateSwapChain, reinterpret_cast<void*>(swap));
    hook_vtable(factory2.Get(), vt::Factory2_CreateSwapChainForHwnd, reinterpret_cast<void*>(swap_hwnd));
  }
}
void record_composite(ID3D12GraphicsCommandList* list, UINT64 scene, UINT64 target) {
  Composite ui{scene, target};
  list->SetPrivateData(composite_tag, sizeof(ui), &ui);
}
void record_frame_size(ID3D12GraphicsCommandList* list, UINT width, UINT height) {
  Extent size{width, height};
  list->SetPrivateData(frame_tag, sizeof(size), &size);
}
void clear_frame_size(ID3D12GraphicsCommandList* list) {
  list->SetPrivateData(composite_tag, 0, nullptr);
  list->SetPrivateData(frame_tag, 0, nullptr);
  clear_scene(list);
}
}  // namespace bifrost
