// Exercise the actual scRGB/PQ conversion command lists and the packed RGB10 intermediate.
// x86_64-w64-mingw32-g++ -std=c++20 -O2 -Isrc -static tools/interop-test/fg_hdr_test.cpp \
//   src/fg/color.cpp src/dlss/kernels.cpp src/core/{hook,vtidx,log}.cpp -ld3d11 -ldxguid -o fg_hdr_test.exe
#include "fg/color.h"
#include "core/log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <vector>

using namespace aco;
using Microsoft::WRL::ComPtr;

// No context hooks are installed by this executable.
namespace aco {
HRESULT orig_FinishCommandList(ID3D11DeviceContext* ctx, BOOL restore, ID3D11CommandList** out) {
  return ctx->FinishCommandList(restore, out);
}
}  // namespace aco

static void check(HRESULT hr, const char* what) {
  if (FAILED(hr)) {
    std::fprintf(stderr, "%s failed: %#lx\n", what, hr);
    std::exit(1);
  }
}
static double half(unsigned short h) {
  const int e = (h >> 10) & 31, m = h & 1023;
  return (h & 32768 ? -1 : 1) * std::ldexp(e ? 1024 + m : m, (e ? e : 1) - 25);
}
static double pq_code(double linear) {
  double n = std::pow(std::max(0., linear * .008), 2610. / 16384.);
  return std::pow((3424. / 4096. + 2413. / 128. * n) / (1. + 2392. / 128. * n), 2523. / 32.) * 1023.;
}

int main() {
  log_init(L"fg_hdr_test.log");
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> imm;
  check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr,
                          &imm),
        "D3D11CreateDevice");
  constexpr UINT w = 19, h = 13;
  // Black, SDR white, 640 nits, saturated red, a wide-gamut cyan, and 10,000 nits.
  using Pixel = std::array<unsigned short, 4>;
  constexpr Pixel samples[] = {
      {0, 0, 0, 0x3c00},      {0x3c00, 0x3c00, 0x3c00, 0x3c00}, {0x4800, 0x4800, 0x4800, 0x3c00},
      {0x4400, 0, 0, 0x3c00}, {0xb400, 0x3c00, 0x3c00, 0x3c00}, {0x57d0, 0x57d0, 0x57d0, 0x3c00}};
  std::vector<Pixel> pixels(w * h);
  for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = samples[i % std::size(samples)];

  auto texture = [&](DXGI_FORMAT fmt, UINT bind, const void* init = nullptr) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = fmt;
    d.BindFlags = bind;
    d.Usage = bind ? D3D11_USAGE_DEFAULT : D3D11_USAGE_STAGING;
    d.CPUAccessFlags = bind ? 0 : D3D11_CPU_ACCESS_READ;
    D3D11_SUBRESOURCE_DATA data{init, w * sizeof(Pixel), 0};
    ComPtr<ID3D11Texture2D> t;
    check(dev->CreateTexture2D(&d, init ? &data : nullptr, &t), "texture");
    return t;
  };
  auto original = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, pixels.data());
  auto packed = texture(DXGI_FORMAT_R10G10B10A2_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
  auto converted = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_UNORDERED_ACCESS);
  auto stage_pq = texture(DXGI_FORMAT_R10G10B10A2_UNORM, 0);
  auto stage_rgb = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, 0);
  fg_color::Converter converter;
  ComPtr<ID3D11CommandList> encode, decode;
  if (!converter.record(dev.Get(), original.Get(), packed.Get(), true, &encode) ||
      !converter.record(dev.Get(), packed.Get(), converted.Get(), false, &decode)) {
    std::fprintf(stderr, "Failed recording HDR pass. See fg_hdr_test.log\n");
    return 1;
  }
  imm->ExecuteCommandList(encode.Get(), TRUE);
  imm->CopyResource(stage_pq.Get(), packed.Get());
  imm->ExecuteCommandList(decode.Get(), TRUE);
  imm->CopyResource(stage_rgb.Get(), converted.Get());
  D3D11_MAPPED_SUBRESOURCE p{}, r{};
  check(imm->Map(stage_pq.Get(), 0, D3D11_MAP_READ, 0, &p), "map pq");
  check(imm->Map(stage_rgb.Get(), 0, D3D11_MAP_READ, 0, &r), "map scRGB");
  unsigned failures = 0;
  for (unsigned y = 0; y < h; ++y)
    for (unsigned x = 0; x < w; ++x) {
      auto code = reinterpret_cast<const unsigned*>(static_cast<const BYTE*>(p.pData) + y * p.RowPitch)[x];
      const Pixel& out = reinterpret_cast<const Pixel*>(static_cast<const BYTE*>(r.pData) + y * r.RowPitch)[x];
      const auto& in = pixels[y * w + x];
      double rr = half(in[0]), gg = half(in[1]), bb = half(in[2]);
      const double expected[] = {pq_code(.627404 * rr + .329283 * gg + .043313 * bb),
                                 pq_code(.069097 * rr + .919540 * gg + .011362 * bb),
                                 pq_code(.016391 * rr + .088013 * gg + .895595 * bb)};
      for (int c = 0; c < 3; ++c) {
        unsigned actual = (code >> (10 * c)) & 1023;
        if (std::abs(actual - expected[c]) > 1 ||
            std::abs(half(out[c]) - half(in[c])) > .015 * (1 + std::abs(half(in[c])))) {
          if (failures++ < 6)
            std::printf("pixel %u,%u ch %d code %u expected %.2f; scRGB %.4f expected %.4f\n", x, y, c, actual,
                        expected[c], half(out[c]), half(in[c]));
        }
      }
    }
  imm->Unmap(stage_rgb.Get(), 0);
  imm->Unmap(stage_pq.Get(), 0);
  std::printf("scRGB -> PQ RGB10 -> scRGB: %u pixels, %u mismatched channels\n", w * h, failures);
  return failures ? 1 : 0;
}
