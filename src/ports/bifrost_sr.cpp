#include "ports/bifrost_dual.h"
#include "ports/bifrost_sr.h"

#include <nvsdk_ngx_defs.h>
#include <atomic>
#include <chrono>
#include <mutex>

#include "core/hook.h"
#include "core/log.h"
#include "dlss/ngx.h"
#include "ports/bifrost_scene.h"
#include "ports/bifrost_eye.h"

using Microsoft::WRL::ComPtr;
using namespace aco;
namespace port {
extern const size_t execute;
}
namespace bifrost {
namespace {
constexpr GUID key(unsigned n) {
  return {0xa61c4df1, 0x4690, 0x4206, {0x83, 0x61, 0x36, 0x30, 0x4e, 0xc8, 0x10, BYTE(n)}};
}
constexpr GUID output_key = key(1), display_buffer_key = key(2), queue_key = key(3), display_key = key(4),
               right_key = key(5), enabled_key = key(6);
constexpr auto read_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
struct Size {
  UINT width{}, height{};
  bool operator==(const Size&) const = default;
};
std::mutex surfaces;
template <class T>
ComPtr<T> associated(ID3D12Object* obj, REFGUID id) {
  ComPtr<T> out;
  UINT n = sizeof(void*);
  obj->GetPrivateData(id, &n, out.GetAddressOf());
  return out;
}
HRESULT make_buffer(ID3D12Device* dev, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, ID3D12Resource** out,
                    UINT64 bytes = 16) {
  D3D12_HEAP_PROPERTIES hp{type};
  hp.CreationNodeMask = hp.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = bytes;
  d.Height = 1;
  d.MipLevels = d.DepthOrArraySize = d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(out));
}
template <class Fn>
Fn proc(const char* name) {
  auto h = ngx::core_module();
  return h ? reinterpret_cast<Fn>(GetProcAddress(h, name)) : nullptr;
}
class Upscaler final : public IUnknown {
 public:
  explicit Upscaler(ID3D12CommandQueue* q) : queue(q) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (id != __uuidof(IUnknown)) return E_NOINTERFACE;
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
  bool eval(const Scene& scene) {
    dual::Internal internal;
    if (!scene.output || !scene.color || !scene.motion || !scene.depth) return false;
    std::lock_guard guard(lock);
    if (!open()) return false;
    auto output = scene.output->GetDesc();
    Size render{scene.info.width, scene.info.height}, target{UINT(output.Width), output.Height};
    if (!feature || render != input_size || target != output_size) {
      if (!create_feature(render, target)) return false;
    }
    auto* slot = begin();
    if (!slot) return false;
    auto* cl = slot->list.Get();
    if (active) set_valid(cl, active.Get(), false);
    transition(cl, scene.output.Get(), read_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ngx::Params p(raw);
    p.reset();
    p.set(NVSDK_NGX_Parameter_Color, scene.color.Get());
    p.set(NVSDK_NGX_Parameter_Depth, scene.depth.Get());
    p.set(NVSDK_NGX_Parameter_MotionVectors, scene.motion.Get());
    p.set(NVSDK_NGX_Parameter_Output, scene.output.Get());
    p.set(NVSDK_NGX_Parameter_MV_Scale_X, float(render.width));
    p.set(NVSDK_NGX_Parameter_MV_Scale_Y, float(render.height));
    p.set(NVSDK_NGX_Parameter_Jitter_Offset_X, scene.info.jitter[0]);
    p.set(NVSDK_NGX_Parameter_Jitter_Offset_Y, scene.info.jitter[1]);
    p.set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, render.width);
    p.set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, render.height);
    p.set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.f);
    p.set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.f);
    const auto now = std::chrono::steady_clock::now();
    float ms = std::chrono::duration<float, std::milli>(now - previous).count();
    bool reset = reset_next || ms > 250.f;
    p.set(NVSDK_NGX_Parameter_Reset, int(reset));
    p.set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, reset ? 16.667f : ms);
    previous = now;
    const auto r = evaluate(cl, feature, p.get(), nullptr);
    reset_next = NVSDK_NGX_FAILED(r);
    auto next = display_buffer(scene.output.Get());
    if (next && !reset_next) {
      D3D12_RESOURCE_DESC d = scene.output->GetDesc();
      ComPtr<ID3D12Device> dev;
      scene.output->GetDevice(IID_PPV_ARGS(&dev));
      D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
      src.pResource = scene.output.Get();
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.pResource = next.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dev->GetCopyableFootprints(&d, 0, 1, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &dst.PlacedFootprint, nullptr,
                                 nullptr, nullptr);
      transition(cl, scene.output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      transition(cl, next.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      cl->CopyBufferRegion(next.Get(), 0, values.Get(), 8, 8);
      transition(cl, next.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      transition(cl, scene.output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, read_state);
    } else {
      transition(cl, scene.output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read_state);
      if (next) set_valid(cl, next.Get(), false);
    }
    slot->scene = scene;
    slot->previous_display = active;
    const bool submitted = submit(*slot);
    if (submitted) active = next;
    if (++evaluations <= 3 || evaluations % 300 == 0)
      ACO_LOG("Bifrost: SR eval #%u %ux%u -> %ux%u %s, jitter %.3f %.3f reset %d", evaluations, render.width,
              render.height, target.width, target.height, ngx::result_str(r), scene.info.jitter[0],
              scene.info.jitter[1], reset);
    return submitted && next && !reset_next;
  }
  void invalidate() {
    std::lock_guard guard(lock);
    reset_next = true;
  }
  void finish() {
    std::lock_guard guard(lock);
    dual::Internal internal;
    if (!active) return;
    auto* slot = begin();
    if (!slot) return;
    set_valid(slot->list.Get(), active.Get(), false);
    slot->previous_display = active;
    if (submit(*slot)) active.Reset();
  }

 private:
  struct Slot {
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12CommandAllocator> allocator;
    UINT64 done{};
    Scene scene;
    ComPtr<ID3D12Resource> previous_display;
  };
  Slot slots[8];
  unsigned pos{};
  bool wait(UINT64 v) {
    if (!v) return true;
    const auto done = fence->GetCompletedValue();
    return done != UINT64_MAX && (done >= v || SUCCEEDED(fence->SetEventOnCompletion(v, nullptr)));
  }
  ~Upscaler() {
    wait(last);
    if (feature) release(feature);
    if (raw) destroy(raw);
  }
  Slot* begin() {
    auto& s = slots[++pos % std::size(slots)];
    if (!wait(s.done)) return nullptr;
    s.scene = {};
    s.previous_display.Reset();
    if (FAILED(s.allocator->Reset()) || FAILED(s.list->Reset(s.allocator.Get(), nullptr))) return nullptr;
    return &s;
  }
  bool submit(Slot& s) {
    if (FAILED(s.list->Close())) return false;
    ID3D12CommandList* lists[]{s.list.Get()};
    orig<void WINAPI(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*)>(queue, port::execute)(queue, 1, lists);
    if (FAILED(queue->Signal(fence.Get(), last + 1))) return false;
    s.done = ++last;
    return true;
  }
  void set_valid(ID3D12GraphicsCommandList* list, ID3D12Resource* pred, bool enabled) {
    transition(list, pred, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(pred, 0, values.Get(), enabled ? 8 : 0, 8);
    transition(list, pred, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }
  bool open() {
    if (raw) return true;
    ComPtr<ID3D12Device> dev;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&dev)))) return false;
    auto alloc = proc<NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter**)>("NVSDK_NGX_D3D12_AllocateParameters");
    create = proc<decltype(create)>("NVSDK_NGX_D3D12_CreateFeature");
    evaluate = proc<decltype(evaluate)>("NVSDK_NGX_D3D12_EvaluateFeature");
    release = proc<decltype(release)>("NVSDK_NGX_D3D12_ReleaseFeature");
    destroy = proc<decltype(destroy)>("NVSDK_NGX_D3D12_DestroyParameters");
    if (!create || !evaluate || !release || !destroy || !alloc) return false;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) ||
        FAILED(make_buffer(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                           values.ReleaseAndGetAddressOf())))
      return false;
    UINT64* data{};
    D3D12_RANGE empty{};
    if (FAILED(values->Map(0, &empty, reinterpret_cast<void**>(&data)))) return false;
    data[0] = 0;
    data[1] = 1;
    values->Unmap(0, nullptr);
    for (auto& s : slots) {
      if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator))) ||
          FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&s.list))) ||
          FAILED(s.list->Close()))
        return false;
    }
    return NVSDK_NGX_SUCCEED(alloc(&raw)) && raw;
  }
  bool create_feature(Size render, Size out) {
    if (!wait(last)) return false;
    if (feature) {
      release(feature);
      feature = nullptr;
    }
    auto* slot = begin();
    if (!slot) return false;
    ngx::Params p(raw);
    p.reset();
    p.set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    p.set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    p.set(NVSDK_NGX_Parameter_Width, render.width);
    p.set(NVSDK_NGX_Parameter_Height, render.height);
    p.set(NVSDK_NGX_Parameter_OutWidth, out.width);
    p.set(NVSDK_NGX_Parameter_OutHeight, out.height);
    float ratio = float(render.width) / out.width;
    int quality = ratio >= .99f   ? NVSDK_NGX_PerfQuality_Value_DLAA
                  : ratio >= .66f ? NVSDK_NGX_PerfQuality_Value_MaxQuality
                  : ratio >= .57f ? NVSDK_NGX_PerfQuality_Value_Balanced
                  : ratio >= .49f ? NVSDK_NGX_PerfQuality_Value_MaxPerf
                                  : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    p.set(NVSDK_NGX_Parameter_PerfQualityValue, quality);
    p.set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
          int(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted));
    for (const char* name :
         {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance})
      p.set(name, 13u);
    auto r = create(slot->list.Get(), NVSDK_NGX_Feature_SuperSampling, p.get(), &feature);
    ACO_LOG("Bifrost: SR create %ux%u -> %ux%u: %s", render.width, render.height, out.width, out.height,
            ngx::result_str(r));
    if (NVSDK_NGX_FAILED(r)) {
      slot->list->Close();
      return false;
    }
    if (!submit(*slot)) return false;
    input_size = render;
    output_size = out;
    reset_next = true;
    return true;
  }
  ID3D12CommandQueue* queue;
  std::mutex lock;
  std::atomic<ULONG> refs{1};
  Size input_size{}, output_size{};
  NVSDK_NGX_Result (*create)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**){};
  NVSDK_NGX_Result (*evaluate)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
                               void*){};
  NVSDK_NGX_Result (*release)(NVSDK_NGX_Handle*){};
  NVSDK_NGX_Result (*destroy)(NVSDK_NGX_Parameter*){};
  NVSDK_NGX_Parameter* raw{};
  NVSDK_NGX_Handle* feature{};
  ComPtr<ID3D12Fence> fence;
  UINT64 last{};
  ComPtr<ID3D12Resource> values, active;
  bool reset_next = true;
  std::chrono::steady_clock::time_point previous{};
  unsigned evaluations{};
};
}  // namespace
void display_size(ID3D12Device* dev, UINT width, UINT height) {
  Size s{width, height};
  dev->SetPrivateData(display_key, sizeof(s), &s);
}
void enable_sr(ID3D12CommandQueue* q, bool enabled) {
  BOOL value = enabled;
  q->SetPrivateData(enabled_key, sizeof(value), &value);
  ComPtr<ID3D12Device> dev;
  if (SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&dev)))) dev->SetPrivateData(enabled_key, sizeof(value), &value);
  for (const auto& tag : {queue_key, right_key}) {
    auto sr = associated<IUnknown>(q, tag);
    if (sr) static_cast<Upscaler*>(sr.Get())->invalidate();
  }
}
void attach_sr(ID3D12CommandQueue* q, UINT width, UINT height) {
  ComPtr<ID3D12Device> dev;
  if (FAILED(q->GetDevice(IID_PPV_ARGS(&dev)))) return;
  display_size(dev.Get(), width, height);
  for (int i = 0; i < ((eye::alternating() || dual::enabled()) ? 2 : 1); ++i) {
    auto* sr = new Upscaler(q);
    q->SetPrivateDataInterface(i ? right_key : queue_key, sr);
    sr->Release();
  }
}
void prepare_output(ID3D12Resource* native, Scene& scene) {
  if (!native) return;
  ComPtr<ID3D12Device> dev;
  if (FAILED(native->GetDevice(IID_PPV_ARGS(&dev)))) return;
  BOOL enabled = FALSE;
  UINT n = sizeof(enabled);
  if (FAILED(dev->GetPrivateData(enabled_key, &n, &enabled)) || !enabled) return;
  Size size{};
  n = sizeof(size);
  if (FAILED(dev->GetPrivateData(display_key, &n, &size)) || !size.width || !size.height) return;
  std::lock_guard guard(surfaces);
  auto output = output_for(native);
  if (output) {
    auto d = output->GetDesc();
    if (d.Width == size.width && d.Height == size.height) {
      scene.output = output;
      return;
    }
  }
  if (FAILED(make_texture(dev.Get(), size.width, size.height, DXGI_FORMAT_R8G8B8A8_UNORM,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, read_state, output.ReleaseAndGetAddressOf())))
    return;
  const UINT64 pitch = (UINT64(size.width) * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                       ~UINT64(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
  const UINT64 bytes = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT + pitch * size.height;
  ComPtr<ID3D12Resource> buffer;
  if (FAILED(make_buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                         buffer.GetAddressOf(), bytes)))
    return;
  if (SUCCEEDED(output->SetPrivateDataInterface(display_buffer_key, buffer.Get())) &&
      SUCCEEDED(native->SetPrivateDataInterface(output_key, output.Get())))
    scene.output = output;
}
ComPtr<ID3D12Resource> output_for(ID3D12Resource* native) { return associated<ID3D12Resource>(native, output_key); }
ComPtr<ID3D12Resource> display_buffer(ID3D12Resource* output) {
  return associated<ID3D12Resource>(output, display_buffer_key);
}
bool upscale(ID3D12CommandQueue* queue, const Scene& scene) {
  BOOL enabled = FALSE;
  UINT n = sizeof(enabled);
  if (FAILED(queue->GetPrivateData(enabled_key, &n, &enabled)) || !enabled) return false;
  auto sr =
      associated<IUnknown>(queue, (eye::alternating() || dual::enabled()) && scene.info.eye ? right_key : queue_key);
  return sr && static_cast<Upscaler*>(sr.Get())->eval(scene);
}
void finish_sr_frame(ID3D12CommandQueue* queue) {
  auto sr = associated<IUnknown>(queue, queue_key);
  if (sr) static_cast<Upscaler*>(sr.Get())->finish();
  if (eye::alternating() || dual::enabled()) {
    auto right = associated<IUnknown>(queue, right_key);
    if (right) static_cast<Upscaler*>(right.Get())->finish();
  }
}
}  // namespace bifrost
