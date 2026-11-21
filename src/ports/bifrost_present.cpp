#include "ports/bifrost_present.h"

#include <dxgi1_4.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <array>
#include <algorithm>
#include <thread>
#include <mutex>

#include "core/hook.h"
#include "core/vtidx.h"
#include "dlss/ngx.h"
#include "fg/camera.h"
#include "ports/bifrost_scene.h"
#include "ports/bifrost_replay.h"
#include "ports/bifrost_eye.h"
#include "ports/bifrost_dual.h"
#include "ports/bifrost_lists.h"
#include <vector>
#include "ports/bifrost_stereo.h"
#include "ports/bifrost_sr.h"
#include "ports/bifrost_verify.h"
#include "ports/common.h"

using Microsoft::WRL::ComPtr;
using namespace aco;
extern "C" IMAGE_DOS_HEADER __ImageBase;
namespace port {
extern const size_t execute, resize1;
}
namespace bifrost {
namespace {
const GUID session_tag{0x4266010b, 0xa117, 0x43a1, {0xb9, 0x41, 0xad, 0x64, 0x23, 0x04, 0x31, 0xc9}};
const GUID eye_tag{0x276e3ecd, 0x1f80, 0x4042, {0xb4, 0x73, 0xda, 0xf3, 0x0c, 0x40, 0xdf, 0x22}};
const GUID frame_tag{0x4266010b, 0xa117, 0x43a1, {0xb9, 0x41, 0xad, 0x64, 0x23, 0x04, 0x31, 0xca}};
const GUID composite_tag{0x4266010b, 0xa117, 0x43a1, {0xb9, 0x41, 0xad, 0x64, 0x23, 0x04, 0x31, 0xcb}};
const GUID pair_tag{0x4266010b, 0xa117, 0x43a1, {0xb9, 0x41, 0xad, 0x64, 0x23, 0x04, 0x31, 0xcc}};
struct Pair {
  UINT64 left_serial{}, right_scene{}, right_target{};
};
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
// Preserve submission ordering when an engine batch is split around TAA.
std::recursive_mutex submissions;
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
    fg_enabled = !eye::enabled() && option(L"fg", L"enabled", 1);
    hudless = option(L"fg", L"hudless", 1) && !option(L"upscaler", L"compare", 0);
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
  template <class Resize>
  HRESULT resize(IDXGISwapChain* chain, Resize&& resize_buffers) {
    std::lock_guard guard(lock);
    if (!drain() || !stereo.drain()) return DXGI_ERROR_DEVICE_RESET;
    HRESULT hr = resize_buffers();
    if (SUCCEEDED(hr)) {
      DXGI_SWAP_CHAIN_DESC d{};
      ComPtr<ID3D12Device> dev;
      if (option(L"upscaler", L"enabled", 1) && SUCCEEDED(chain->GetDesc(&d)) &&
          SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&dev))))
        display_size(dev.Get(), d.BufferDesc.Width, d.BufferDesc.Height);
      reset_history = true;
      stereo.invalidate();
      dual::backbuffers(chain, queue.Get());
      refresh_checked = false;
      period = {};
      next_flip = {};
    }
    return hr;
  }
  // Fullscreen flip-model chains expose the current physical buffer to D3D12.
  // Queue both copies around the extra flip so the second present carries the untouched real frame.
  template <class Flip>
  HRESULT present(IDXGISwapChain* chain, UINT flags, Flip&& flip) {
    std::lock_guard guard(lock);
    if (flags & (DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_WAIT | DXGI_PRESENT_RESTART)) return flip();
    replay::present();
    finish_sr_frame(queue.Get());
    if (!fg_enabled && !eye::alternating() && !dual::enabled()) return flip();
    Scene scene;
    Pair pair{};
    {
      std::lock_guard handoff_guard(handoff);
      if (dual::enabled()) {
        UINT n = sizeof(pair);
        queue->GetPrivateData(pair_tag, &n, &pair);
      }
      scene = recorded_scene(queue.Get());
    }
    if (dual::enabled()) {
      bool fresh = dual::healthy() && pair.left_serial && pair.left_serial == scene.info.serial && scene.info.serial != seen;
      stereo.present(chain, queue.Get(), -1, fresh ? pair.right_target : 0);
      seen = scene.info.serial;
      return flip();
    }
    if (eye::alternating()) {
      bool fresh = scene.info.serial && scene.info.serial != seen;
      stereo.present(chain, queue.Get(), fresh ? scene.info.eye : -1);
      seen = scene.info.serial;
      if (fresh) {
        int next = 1 - scene.info.eye;
        queue->SetPrivateData(eye_tag, sizeof(next), &next);
      }
      return flip();
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
    const bool use_hudless = hudless && scene.output && scene.output->GetDesc().Format == bd.Format;
    if (use_hudless) {
      p.set(NVSDK_NGX_DLSSG_Parameter_HUDLess, scene.output.Get());
      p.set(NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectWidth, wanted.width);
      p.set(NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectHeight, wanted.height);
    }
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
    p.set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, generated_count);
    const bool insert = !reset_history;
    bool flipped = false;
    for (unsigned i = 1; i <= generated_count; ++i) {
      p.set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, i);
      auto result = evaluate(list.Get(), feature, p.get(), nullptr);
      if (i == 1 && (frame_id <= 3 || frame_id % 300 == 0))
        ACO_LOG(
            "Bifrost: FG evaluate #%u %s (%ux), jitter %.3f %.3f, camera %.2f %.2f %.2f, real %u generated %u, "
            "HUD-less %d",
            frame_id, ngx::result_str(result), generated_count + 1, scene.info.jitter[0], scene.info.jitter[1],
            camera.pos[0], camera.pos[1], camera.pos[2], real_presents, generated_presents, use_hudless);
      if (NVSDK_NGX_FAILED(result)) {
        list->Close();
        reset_history = true;
        if (!flipped) return flip();
        if (!begin()) return DXGI_ERROR_DEVICE_RESET;
        break;
      }
      bool sample = check && i == (generated_count + 1) / 2;
      if (sample) verifier.finish(list.Get(), generated.Get(), real_frame.Get());
      if (insert) copy_to_buffer(bb.Get(), generated.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      if (!submit()) return DXGI_ERROR_DEVICE_RESET;
      slots[current].scene = scene;
      if (sample && wait_for(submitted)) verifier.report();
      reset_history = false;
      if (insert) {
        pace(chain);
        auto extra = flip();
        if (extra != S_OK) {
          reset_history = true;
          return extra;
        }
        ++generated_presents;
        flipped = true;
        bb.Reset();
        if (FAILED(sc->GetBuffer(sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb)))) return DXGI_ERROR_DEVICE_RESET;
      }
      if ((insert || i < generated_count) && !begin()) return DXGI_ERROR_DEVICE_RESET;
    }
    if (flipped) {
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
    p.set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, generated_count);
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
    auto caps = ngx_proc<Parameters>("NVSDK_NGX_D3D12_GetCapabilityParameters");
    NVSDK_NGX_Parameter* limits{};
    UINT maximum = 1;
    if (caps && NVSDK_NGX_SUCCEED(caps(&limits)) && limits) {
      ngx::Params(limits).get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &maximum);
      destroy(limits);
    }
    UINT requested = GetPrivateProfileIntW(L"fg", L"count", 2, ini_file().c_str());
    generated_count = std::clamp(requested, 2u, std::clamp(maximum, 1u, 5u) + 1) - 1;
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
  StereoOutput stereo;
  VerifyFrames verifier;
  UINT generated_count = 1;
  bool fg_enabled{}, verify{}, hudless{};
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
  auto original = orig<decltype(execute)>(q, port::execute);
  replay::submitted(q, count, lists);
  BOOL attached = FALSE;
  UINT bytes = sizeof(attached);
  q->GetPrivateData(session_tag, &bytes, &attached);
  std::vector<ID3D12CommandList*> combined, right;
  std::vector<int> which;
  if (dual::game_call()) {
    combined.reserve(2 * count);
    right.reserve(count);
    which.reserve(2 * count);
    for (UINT i = 0; i < count; ++i) {
      combined.push_back(lists[i]);
      which.push_back(0);
      if (auto* other = dual::list(lists[i])) {
        combined.push_back(other);
        right.push_back(other);
        which.push_back(1);
      }
    }
  }
  if (!attached) {
    if (!right.empty())
      original(q, UINT(combined.size()), combined.data());
    else
      original(q, count, lists);
    return;
  }
  std::lock_guard submit_guard(submissions);
  int eye_index = 0;
  if (eye::enabled()) {
    UINT length = sizeof(eye_index);
    if (!dual::enabled()) q->GetPrivateData(eye_tag, &length, &eye_index);
    Scene camera;
    for (UINT i = 0; i < count && !camera.info.serial; ++i) camera = recorded_scene(lists[i]);
    auto* view = camera.info.camera.valid ? camera.info.camera.view_to_world[0] : nullptr;
    eye::refresh();
    eye::prepare(count, lists, view, eye_index);
    if (!right.empty()) eye::prepare(UINT(right.size()), right.data(), view, 1);
  }
  if (!right.empty()) {
    count = UINT(combined.size());
    lists = combined.data();
  }
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
  Pair pair{};
  bool paired = !right.empty();
  UINT pair_bytes = sizeof(pair);
  if (paired) q->GetPrivateData(pair_tag, &pair_bytes, &pair);
  UINT start = 0;
  for (UINT i = 0; i < count; ++i) {
    // A complete pair requires both TAA draws and the matching right-eye composite.
    // These may arrive in separate ExecuteCommandLists calls before the flip.
    if (paired && which[i] && pair.right_scene) {
      Composite ui{};
      UINT n = sizeof(ui);
      if (SUCCEEDED(lists[i]->GetPrivateData(composite_tag, &n, &ui)) && ui.scene == pair.right_scene)
        pair.right_target = ui.target;
    }
    Extent size{};
    UINT bytes = sizeof(size);
    if (FAILED(lists[i]->GetPrivateData(frame_tag, &bytes, &size)) || !size.width) continue;
    auto scene = recorded_scene(lists[i]);
    scene.info.eye = paired ? which[i] : eye_index;
    if (paired) {
      if (!which[i])
        pair = {scene.info.serial, 0, 0};
      else if (i && !which[i-1] && lists[i] == dual::list(lists[i-1]))
        pair.right_scene = scene.info.native;
    }
    original(q, i + 1 - start, lists + start);
    start = i + 1;
    if (scene.output && !upscale(q, scene)) scene.output.Reset();
    std::lock_guard guard(handoff);
    if (right.empty() || !which[i]) forward_scene(q, scene);
  }
  if (start < count) original(q, count - start, lists + start);
  if (paired) {
    std::lock_guard guard(handoff);
    q->SetPrivateData(pair_tag, sizeof(pair), &pair);
  }
}
thread_local bool in_resize = false;
template <class Fn>
HRESULT on_resize(IDXGISwapChain* chain, Fn&& fn) {
  ComPtr<IUnknown> owner;
  UINT bytes = sizeof(void*);
  if (in_resize || FAILED(chain->GetPrivateData(session_tag, &bytes, owner.GetAddressOf())) || !owner) return fn();
  in_resize = true;
  auto hr = static_cast<Session*>(owner.Get())->resize(chain, fn);
  in_resize = false;
  return hr;
}
HRESULT WINAPI resize(IDXGISwapChain* chain, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags) {
  return on_resize(chain, [&] {
    return orig<decltype(resize)>(chain, vt::SwapChain_ResizeBuffers)(chain, count, w, h, format, flags);
  });
}
HRESULT WINAPI resize1(IDXGISwapChain3* chain, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags,
                       const UINT* masks, IUnknown* const* queues) {
  return on_resize(chain, [&] {
    return orig<decltype(resize1)>(chain, port::resize1)(chain, count, w, h, format, flags, masks, queues);
  });
}
void attach(IUnknown* creator, IDXGISwapChain* chain) {
  ComPtr<ID3D12CommandQueue> queue;
  if (!creator || !chain || FAILED(creator->QueryInterface(IID_PPV_ARGS(&queue))) ||
      queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
    return;
  DXGI_SWAP_CHAIN_DESC desc{};
  if (option(L"upscaler", L"enabled", 1) && SUCCEEDED(chain->GetDesc(&desc)))
    attach_sr(queue.Get(), desc.BufferDesc.Width, desc.BufferDesc.Height);
  dual::backbuffers(chain, queue.Get());
  auto* session = new Session(queue.Get());
  const auto hr = chain->SetPrivateDataInterface(session_tag, session);
  session->Release();
  if (FAILED(hr)) return;
  const BOOL attached = TRUE;
  queue->SetPrivateData(session_tag, sizeof(attached), &attached);
  hook_vtable(queue.Get(), port::execute, reinterpret_cast<void*>(execute));
  hook_vtable(chain, vt::SwapChain_Present, reinterpret_cast<void*>(present));
  hook_vtable(chain, vt::SwapChain_ResizeBuffers, reinterpret_cast<void*>(resize));
  ComPtr<IDXGISwapChain3> sc3;
  if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
    hook_vtable(sc3.Get(), vt::SwapChain_ResizeBuffers, reinterpret_cast<void*>(resize));
    hook_vtable(sc3.Get(), port::resize1, reinterpret_cast<void*>(resize1));
  }
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
UINT option(const wchar_t* section, const wchar_t* key, UINT fallback) {
  return GetPrivateProfileIntW(section, key, fallback, ini_file().c_str());
}
std::wstring option_text(const wchar_t* section, const wchar_t* key, const wchar_t* fallback) {
  wchar_t text[96]{};
  GetPrivateProfileStringW(section, key, fallback, text, std::size(text), ini_file().c_str());
  return text;
}
void observe_queue(ID3D12CommandQueue* queue) { hook_vtable(queue, port::execute, reinterpret_cast<void*>(execute)); }
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
