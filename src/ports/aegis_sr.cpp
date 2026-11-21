// Single reset-frame SR experiment. GPU copies are recorded at the game's draw.
// Only evaluate once those copies have executed, on private D3D11 context state.
#include "ports/aegis_sr.h"

#include <d3d10.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <vector>

#include "core/log.h"
#include "dlss/ngx.h"
#include "ports/common.h"

using Microsoft::WRL::ComPtr;
using namespace aco;
namespace aegis {
namespace {
enum class State { idle, recording, pending, immediate, done };
std::atomic<State> state{State::idle};
std::atomic<uint64_t> display_size{};
const GUID raw_depth_tag = {0xbfcfa64c, 0x62f8, 0x42d1, {0x93, 0xcf, 0xa0, 0x1b, 0x1f, 0x7b, 0x9e, 0x36}};
const GUID capture_tag = {0x533b9fa1, 0x72bf, 0x4630, {0x82, 0x45, 0xc7, 0x89, 0x24, 0x43, 0xf8, 0xce}};
struct Snapshot {
  ComPtr<ID3D11Texture2D> color, depth;
  bool immediate{};
  UINT width{}, height{};
} shot;

template <typename V>
ComPtr<ID3D11Texture2D> texture(V* view) {
  ComPtr<ID3D11Texture2D> tex;
  if (view) {
    ComPtr<ID3D11Resource> res;
    view->GetResource(&res);
    res.As(&tex);
  }
  return tex;
}

void pair_depth(ID3D11DeviceContext* ctx) {
  ComPtr<ID3D11RenderTargetView> views[2];
  ID3D11RenderTargetView* bound[2]{};
  ctx->OMGetRenderTargets(2, bound, nullptr);
  for (UINT i = 0; i < 2; ++i) views[i].Attach(bound[i]);
  auto linear = texture(views[0].Get()), raw = texture(views[1].Get());
  if (linear && raw && raw.Get() != linear.Get()) linear->SetPrivateDataInterface(raw_depth_tag, raw.Get());
}

bool record(ID3D11DeviceContext* ctx) {
  ComPtr<ID3D11ShaderResourceView> source, linear_view;
  ctx->PSGetShaderResources(85, 1, &source);
  ctx->PSGetShaderResources(87, 1, &linear_view);
  auto color = texture(source.Get()), linear = texture(linear_view.Get());
  ComPtr<ID3D11Texture2D> depth;
  UINT len = sizeof(ID3D11Texture2D*);
  if (!color || !linear || FAILED(linear->GetPrivateData(raw_depth_tag, &len, depth.GetAddressOf()))) return false;
  D3D11_TEXTURE2D_DESC cd, dd;
  color->GetDesc(&cd);
  depth->GetDesc(&dd);
  if (cd.Format != DXGI_FORMAT_R8G8B8A8_UNORM || dd.Format != DXGI_FORMAT_R32_FLOAT || cd.Width != dd.Width ||
      cd.Height != dd.Height || cd.SampleDesc.Count != 1 || dd.SampleDesc.Count != 1 || cd.ArraySize != 1 ||
      dd.ArraySize != 1)
    return false;
  cd.BindFlags = dd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  cd.MiscFlags = dd.MiscFlags = 0;
  ComPtr<ID3D11Device> dev;
  ctx->GetDevice(&dev);
  Snapshot next;
  if (FAILED(dev->CreateTexture2D(&cd, nullptr, &next.color)) ||
      FAILED(dev->CreateTexture2D(&dd, nullptr, &next.depth)))
    return false;
  next.width = cd.Width;
  next.height = cd.Height;
  ctx->CopyResource(next.color.Get(), color.Get());
  ctx->CopyResource(next.depth.Get(), depth.Get());
  next.immediate = ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
  if (!next.immediate) {
    const BOOL mark = TRUE;
    if (FAILED(ctx->SetPrivateData(capture_tag, sizeof(mark), &mark))) return false;
  }
  shot = std::move(next);
  return true;
}

bool write_image(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* image, const wchar_t* name) {
  D3D11_TEXTURE2D_DESC d;
  image->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  d.BindFlags = d.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging))) return false;
  ctx->CopyResource(staging.Get(), image);
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
  FILE* f = _wfopen((out_dir() + L"\\" + name).c_str(), L"wb");
  const bool depth = d.Format == DXGI_FORMAT_R32_FLOAT;
  bool ok = f && fprintf(f, depth ? "Pf\n%u %u\n-1.0\n" : "P6\n%u %u\n255\n", d.Width, d.Height) > 0;
  std::vector<unsigned char> row(3 * d.Width);
  for (UINT y = 0; ok && y < d.Height; ++y) {
    auto* pixels = static_cast<unsigned char*>(m.pData) + y * m.RowPitch;
    if (depth) {
      // Portable floatmaps store scanlines bottom-up. The negative scale marks little-endian samples.
      pixels = static_cast<unsigned char*>(m.pData) + size_t(d.Height - 1 - y) * m.RowPitch;
      ok = fwrite(pixels, sizeof(float), d.Width, f) == d.Width;
    } else {
      for (UINT x = 0; x < d.Width; ++x) memcpy(row.data() + 3 * x, pixels + 4 * x, 3);
      ok = fwrite(row.data(), 1, row.size(), f) == row.size();
    }
  }
  if (f && fclose(f)) ok = false;
  ctx->Unmap(staging.Get(), 0);
  return ok;
}

bool evaluate(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
  auto display = display_size.load();
  UINT w = UINT(display >> 32), h = UINT(display);
  if (w < shot.width || h < shot.height) return false;
  D3D11_TEXTURE2D_DESC d{};
  d.Width = w;
  d.Height = h;
  d.ArraySize = d.MipLevels = d.SampleDesc.Count = 1;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11Texture2D> output, motion;
  if (FAILED(dev->CreateTexture2D(&d, nullptr, &output))) return false;
  d.Width = shot.width;
  d.Height = shot.height;
  d.Format = DXGI_FORMAT_R16G16_FLOAT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  // A reset frame has no prior sample to reproject. The continuous path needs the
  // game's decoded per-pixel motion and camera reprojection for sky pixels.
  std::vector<uint32_t> zero(size_t(d.Width) * d.Height);
  D3D11_SUBRESOURCE_DATA data{zero.data(), d.Width * 4, 0};
  if (FAILED(dev->CreateTexture2D(&d, &data, &motion))) return false;
  auto p = ngx::alloc_params();
  if (!p) return false;
  p.set(NVSDK_NGX_Parameter_Width, d.Width);
  p.set(NVSDK_NGX_Parameter_Height, d.Height);
  p.set(NVSDK_NGX_Parameter_OutWidth, w);
  p.set(NVSDK_NGX_Parameter_OutHeight, h);
  p.set(NVSDK_NGX_Parameter_PerfQualityValue,
        int(d.Width == w ? NVSDK_NGX_PerfQuality_Value_DLAA : NVSDK_NGX_PerfQuality_Value_MaxQuality));
  p.set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
        int(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted));
  auto* handle = ngx::create_feature(ctx, NVSDK_NGX_Feature_SuperSampling, p);
  bool ok = false;
  if (handle) {
    p.reset();
    p.set(NVSDK_NGX_Parameter_Color, shot.color.Get());
    p.set(NVSDK_NGX_Parameter_Depth, shot.depth.Get());
    p.set(NVSDK_NGX_Parameter_MotionVectors, motion.Get());
    p.set(NVSDK_NGX_Parameter_Output, output.Get());
    p.set(NVSDK_NGX_Parameter_Reset, 1);
    p.set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.f);
    p.set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.f);
    p.set(NVSDK_NGX_Parameter_MV_Scale_X, float(d.Width));
    p.set(NVSDK_NGX_Parameter_MV_Scale_Y, float(d.Height));
    p.set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.f);
    p.set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.f);
    p.set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, d.Width);
    p.set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, d.Height);
    auto r = ngx::evaluate(ctx, handle, p);
    ACO_LOG("Aegis: reset-frame SR %ux%u -> %ux%u centered jitter: %s", d.Width, d.Height, w, h, ngx::result_str(r));
    ok = NVSDK_NGX_SUCCEED(r) && write_image(dev, ctx, output.Get(), L"aegis-sr.ppm") &&
         write_image(dev, ctx, shot.color.Get(), L"aegis-input.ppm") &&
         write_image(dev, ctx, shot.depth.Get(), L"aegis-depth.pfm");
    ngx::release_feature(handle);
  }
  ngx::destroy_params(p);
  return ok;
}
}  // namespace

bool capturing() {
  static const bool enabled = [] {
    wchar_t value[2]{};
    return GetEnvironmentVariableW(L"QUIVER_LAB", nullptr, 0) &&
           GetEnvironmentVariableW(L"AEGIS_SR_CAPTURE", value, 2) == 1 && value[0] == L'1';
  }();
  return enabled && state.load() == State::idle;
}

void capture_draw(ID3D11DeviceContext* ctx, const port::Entry& entry) {
  if (!capturing()) return;
  if (entry.role == "PS/ui-composite") {
    ComPtr<ID3D11RenderTargetView> rtv;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (auto target = texture(rtv.Get())) {
      D3D11_TEXTURE2D_DESC d;
      target->GetDesc(&d);
      display_size.store((uint64_t(d.Width) << 32) | d.Height);
    }
  }
  if (entry.role == "PS/linear-depth") pair_depth(ctx);
  // A permutation that omits depth may still have a stale view bound in t87.
  if (entry.role != "PS/temporal-resolve" || entry.detail.find("t85=TemporalAA_Color ") == std::string::npos ||
      entry.detail.find("t87=TemporalAA_Depth ") == std::string::npos)
    return;
  static const auto trigger = out_dir() + L"\\capture-sr";
  // Only poll from a temporal pass, with a bounded frequency.
  static std::atomic<ULONGLONG> next_check{0};
  auto now = GetTickCount64(), next = next_check.load();
  if (now < next || !next_check.compare_exchange_strong(next, now + 250)) return;
  if (GetFileAttributesW(trigger.c_str()) == INVALID_FILE_ATTRIBUTES) return;
  State expected = State::idle;
  if (!state.compare_exchange_strong(expected, State::recording)) return;
  bool ok = record(ctx);
  if (ok) ACO_LOG("Aegis: captured temporal inputs %ux%u", shot.width, shot.height);
  auto submitted = shot.immediate ? State::immediate : State::pending;
  state.store(ok ? submitted : State::idle);
}

void finished_list(ID3D11DeviceContext* ctx, ID3D11CommandList* list) {
  if (state.load() != State::pending) return;
  BOOL mark = FALSE;
  UINT size = sizeof(mark);
  if (SUCCEEDED(ctx->GetPrivateData(capture_tag, &size, &mark)) && mark) {
    list->SetPrivateData(capture_tag, sizeof(mark), &mark);
    ctx->SetPrivateData(capture_tag, 0, nullptr);
  }
}

void after_execute(ID3D11DeviceContext* ctx, ID3D11CommandList* list) {
  auto expected = state.load();
  if (expected == State::pending) {
    BOOL mark = FALSE;
    UINT size = sizeof(mark);
    if (!list || FAILED(list->GetPrivateData(capture_tag, &size, &mark)) || !mark) return;
  } else if (expected != State::immediate) {
    return;
  }
  if (!state.compare_exchange_strong(expected, State::recording)) return;
  ComPtr<ID3D11Device> dev;
  ctx->GetDevice(&dev);
  ComPtr<ID3D11Device1> dev1;
  ComPtr<ID3D11DeviceContext1> ctx1;
  ComPtr<ID3DDeviceContextState> own, prior;
  auto fl = dev->GetFeatureLevel();
  bool ok =
      SUCCEEDED(dev.As(&dev1)) && SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))) &&
      SUCCEEDED(dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &own));
  if (!ok) ACO_LOG("Aegis: private device context state unavailable");
  if (ok) {
    ComPtr<ID3D10Multithread> mt;
    ctx->QueryInterface(IID_PPV_ARGS(&mt));
    if (mt) mt->Enter();
    ctx1->SwapDeviceContextState(own.Get(), &prior);
    ok = evaluate(dev.Get(), ctx);
    ctx1->SwapDeviceContextState(prior.Get(), nullptr);
    if (mt) mt->Leave();
  }
  ACO_LOG("Aegis: offscreen SR capture %s", ok ? "written" : "failed");
  shot = {};
  state.store(State::done);
}
}  // namespace aegis
