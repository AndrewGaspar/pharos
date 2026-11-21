#include "stereo/marker_history.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <cmath>
#include "stereo/camera.h"
#include "dlss/kernels.h"

namespace aco {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
std::mutex mutex;
ComPtr<ID3D11ShaderResourceView> pending, published, history;
ComPtr<ID3D11Texture2D> previous, output;
ComPtr<ID3D11UnorderedAccessView> uav;
ComPtr<ID3D11ComputeShader> filter;
ComPtr<ID3D11Buffer> constants;
UINT width, height;
ComPtr<ID3D11ShaderResourceView> source_view, filtered_source;
MarkerCamera last_camera{};
Clock::time_point updated;

const char code[] = R"(
Texture2D<float> scene : register(t0);
Texture2D<float4> old : register(t1);
RWTexture2D<float4> result : register(u0);
cbuffer Timing : register(b0) { float dt; uint reset; }
[numthreads(8,8,1)]
void main(uint2 p : SV_DispatchThreadID) {
  uint2 size, tiles; scene.GetDimensions(size.x,size.y); result.GetDimensions(tiles.x,tiles.y);
  if (any(p >= tiles)) return;
  float z = 0;
  [unroll] for(uint y=0;y<4;++y) [unroll] for(uint x=0;x<4;++x)
    z = max(z, scene.Load(int3(min(4*p+uint2(x,y),size-1),0)));
  float2 h = old.Load(int3(p,0)).xy;
  uint2 ray = all(p == (size/2)/4) ? size/2 : min(4*p+2,size-1);
  float raw = scene.Load(int3(ray,0));
  // A newly visible occluder wins immediately. Hold a near sample for 120 ms,
  // then release towards the new disparity with a 120 ms time constant.
  float hold = max(0,h.y-dt);
  if (reset || z >= h.x) result[p] = float4(z,0.12,raw,0);
  else result[p] = float4(lerp(z,h.x,exp(-max(0,dt-h.y)/0.12)),hold,raw,0);
}
)";

bool prepare(ID3D11Device* dev, UINT w, UINT h) {
  if (!filter) filter.Attach(kernels::compile(dev, code, sizeof(code) - 1, "pharos_marker_history.hlsl"));
  if (!constants) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = 16;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev->CreateBuffer(&d, nullptr, &constants);
  }
  if (!filter || !constants) return false;
  if (w == width && h == height && uav && history) return true;
  width = height = 0;
  D3D11_TEXTURE2D_DESC d{};
  d.Width = (w + 3) / 4;
  d.Height = (h + 3) / 4;
  d.ArraySize = d.MipLevels = d.SampleDesc.Count = 1;
  d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(dev->CreateTexture2D(&d, nullptr, previous.ReleaseAndGetAddressOf())) ||
      FAILED(dev->CreateShaderResourceView(previous.Get(), nullptr, history.ReleaseAndGetAddressOf())))
    return false;
  d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  if (FAILED(dev->CreateTexture2D(&d, nullptr, output.ReleaseAndGetAddressOf())) ||
      FAILED(dev->CreateUnorderedAccessView(output.Get(), nullptr, uav.ReleaseAndGetAddressOf())))
    return false;
  width = w;
  height = h;
  return true;
}
}  // namespace

ComPtr<ID3D11ShaderResourceView> marker_history_sample(ID3D11ShaderResourceView* depth) {
  std::lock_guard lock(mutex);
  pending = depth;
  return source_view.Get() == depth ? published : nullptr;
}

void marker_history_reset() {
  std::lock_guard lock(mutex);
  pending.Reset();
  published.Reset();
  source_view.Reset();
  filtered_source.Reset();
}

void marker_history_filter(ID3D11DeviceContext1* ctx, ID3DDeviceContextState* private_state) {
  ComPtr<ID3D11ShaderResourceView> depth;
  filtered_source.Reset();
  bool reset;
  {
    std::lock_guard lock(mutex);
    depth.Swap(pending);
    reset = !published || depth.Get() != source_view.Get();
    if (reset) {
      published.Reset();
      source_view = nullptr;
    }
    if (!depth) return;
  }
  ComPtr<ID3D11Resource> r;
  ComPtr<ID3D11Texture2D> tex;
  depth->GetResource(&r);
  if (FAILED(r.As(&tex))) return;
  D3D11_TEXTURE2D_DESC d;
  tex->GetDesc(&d);
  ComPtr<ID3D11Device> dev;
  ctx->GetDevice(&dev);
  // The immediate thread owns GPU resources. Worker threads only acquire the published SRV.
  if (!prepare(dev.Get(), d.Width, d.Height)) return;
  auto camera = cam_marker_view();
  float motion = 0, facing = 0;
  for (int i = 0; i < 3; ++i) {
    const float d = camera.pos[i] - last_camera.pos[i];
    motion += d * d;
    facing += camera.look[i] * last_camera.look[i];
  }
  reset |= motion > 4 || facing < .99f || std::fabs(camera.m00 - last_camera.m00) > .1f;
  last_camera = camera;
  const auto now = Clock::now();
  float dt = std::chrono::duration<float>(now - updated).count();
  struct {
    float dt;
    unsigned reset, pad[2];
  } params{std::clamp(dt, 0.f, 0.25f), reset || dt > 0.35f, {}};
  updated = now;
  ComPtr<ID3DDeviceContextState> saved;
  ctx->SwapDeviceContextState(private_state, &saved);
  ctx->UpdateSubresource(constants.Get(), 0, nullptr, &params, 0, 0);
  ID3D11ShaderResourceView* views[] = {depth.Get(), history.Get()};
  ctx->CSSetShaderResources(0, 2, views);
  ctx->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
  ctx->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
  ctx->CSSetShader(filter.Get(), nullptr, 0);
  ctx->Dispatch((d.Width + 31) / 32, (d.Height + 31) / 32, 1);
  ID3D11ShaderResourceView* empty[2]{};
  ID3D11UnorderedAccessView* none = nullptr;
  ctx->CSSetShaderResources(0, 2, empty);
  ctx->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
  ctx->SwapDeviceContextState(saved.Get(), nullptr);
  filtered_source = depth;
}

void marker_history_publish(ID3D11DeviceContext* ctx) {
  if (!filtered_source) return;
  // Both command-list executions must finish reading the old map before it is replaced.
  ctx->CopyResource(previous.Get(), output.Get());
  std::lock_guard lock(mutex);
  published = history;
  source_view.Swap(filtered_source);
  filtered_source.Reset();
}
}  // namespace aco
