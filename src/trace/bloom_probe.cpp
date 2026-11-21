#include "trace/bloom_probe.h"

#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>

#include "core/lab.h"
#include "core/log.h"
#include "dlss/kernels.h"
#include "stereo/temporal.h"

namespace aco {
namespace {
using Microsoft::WRL::ComPtr;
constexpr UINT width = 64, height = 36;
constexpr unsigned ring_size = 8;
// At 18,444 bytes per record this caps a lab run below 512 MiB.
constexpr unsigned max_records = 28000;
const char shader[] = R"(
Texture2D<float4> scene : register(t0);
Texture2D<float4> scaled : register(t1);
RWTexture2D<float4> samples : register(u0);
[numthreads(8,8,1)]
void main(uint2 p : SV_DispatchThreadID) {
  uint2 size, a, b;
  samples.GetDimensions(size.x,size.y); size.x /= 2;
  if(any(p >= size)) return;
  scene.GetDimensions(a.x,a.y); scaled.GetDimensions(b.x,b.y);
  float2 uv = (p+0.5)/size;
  samples[p] = scene.Load(int3(uint2(uv*a),0));
  samples[p+uint2(size.x,0)] = scaled.Load(int3(uint2(uv*b),0));
}
)";
struct Sample {
  ComPtr<ID3D11Texture2D> staging;
  uint64_t frame = 0;
  uint32_t kind = 0;
  bool pending = false;
};
struct Probe {
  ComPtr<ID3D11ComputeShader> cs;
  ComPtr<ID3D11Texture2D> output;
  ComPtr<ID3D11UnorderedAccessView> uav;
  Sample ring[ring_size];
  unsigned slot = 0;
  std::unique_ptr<FILE, decltype(&fclose)> file{nullptr, fclose};
  unsigned written = 0;
} probe;
std::atomic<bool> enabled{false};
std::mutex probe_mutex;

bool prepare(ID3D11Device* dev) {
  probe.cs.Attach(kernels::compile(dev, shader, sizeof(shader) - 1, "bloom_probe.hlsl"));
  D3D11_TEXTURE2D_DESC d{};
  d.Width = 2 * width;
  d.Height = height;
  d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  if (!probe.cs || FAILED(dev->CreateTexture2D(&d, nullptr, &probe.output)) ||
      FAILED(dev->CreateUnorderedAccessView(probe.output.Get(), nullptr, &probe.uav)))
    return false;
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  for (auto& s : probe.ring)
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &s.staging))) return false;
  if (!probe.file) probe.file.reset(_wfopen((out_dir() + L"\\bloom-pairs.bin").c_str(), L"wb"));
  return probe.file != nullptr;
}
}  // namespace

void bloom_probe_init() {
  // Keep the run file but discard GPU resources when the rendering device changes.
  std::lock_guard lock(probe_mutex);
  Probe fresh;
  fresh.file = std::move(probe.file);
  fresh.written = probe.written;
  probe = std::move(fresh);
  wchar_t flag[2]{};
  enabled = probe.written < max_records && lab_active() &&
            GetEnvironmentVariableW(L"PHAROS_BLOOM_PROBE", flag, 2) == 1 && flag[0] == L'1';
}
bool bloom_probe_wants(uint64_t frame) { return enabled.load(std::memory_order_relaxed) && frame % 4 == 0; }

void bloom_probe_sample(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint64_t frame, unsigned kind,
                        ID3D11ShaderResourceView* input, ID3D11ShaderResourceView* output) {
  if (!input || !output) return;
  // Upscale jobs can arrive on game threads. Never wait for another sampler
  // while holding the immediate-context lock.
  std::unique_lock lock(probe_mutex, std::try_to_lock);
  if (!lock.owns_lock() || !enabled.load(std::memory_order_relaxed)) return;
  TemporalPresentScope internal;
  if (!probe.cs && !prepare(dev)) {
    enabled = false;
    ACO_LOG("bloom probe: could not create capture resources");
    return;
  }
  Sample& s = probe.ring[probe.slot];
  if (s.pending) {
    D3D11_MAPPED_SUBRESOURCE m{};
    HRESULT hr = ctx->Map(s.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
    if (FAILED(hr)) {
      ACO_LOG("bloom probe: readback failed 0x%08lx", hr);
      enabled = false;
      return;
    }
    bool ok = fwrite(&s.frame, sizeof(s.frame), 1, probe.file.get()) == 1 &&
              fwrite(&s.kind, sizeof(s.kind), 1, probe.file.get()) == 1;
    for (UINT y = 0; y < height && ok; ++y)
      ok = fwrite(static_cast<const char*>(m.pData) + y * m.RowPitch, 2 * width * 4, 1, probe.file.get()) == 1;
    ctx->Unmap(s.staging.Get(), 0);
    if (!ok) {
      enabled = false;
      ACO_LOG("bloom probe: write failed");
      return;
    }
    if (++probe.written == max_records) {
      probe.file.reset();
      enabled = false;
      ACO_LOG("bloom probe: capture limit reached");
      return;
    }
  }

  ctx->CSSetShader(probe.cs.Get(), nullptr, 0);
  ID3D11ShaderResourceView* views[] = {input, output};
  ctx->CSSetShaderResources(0, 2, views);
  ctx->CSSetUnorderedAccessViews(0, 1, probe.uav.GetAddressOf(), nullptr);
  ctx->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
  ID3D11ShaderResourceView* empty[2]{};
  ID3D11UnorderedAccessView* none = nullptr;
  ctx->CSSetShaderResources(0, 2, empty);
  ctx->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
  ctx->CopyResource(s.staging.Get(), probe.output.Get());
  s.frame = frame;
  s.kind = kind;
  s.pending = true;
  probe.slot = (probe.slot + 1) % ring_size;
}
}  // namespace aco
