#include "core/framedump.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "core/log.h"

namespace aco {

namespace {

struct Frame {
  ID3D11Texture2D* tex = nullptr;
  long long t_ns = 0;
};

std::vector<Frame> g_frames;
int g_want = 0;
unsigned g_poll = 0;
D3D11_TEXTURE2D_DESC g_desc{};

long long now_ns() {
  LARGE_INTEGER c, f;
  QueryPerformanceCounter(&c);
  QueryPerformanceFrequency(&f);
  return c.QuadPart / f.QuadPart * 1000000000LL + c.QuadPart % f.QuadPart * 1000000000LL / f.QuadPart;
}

void write_all(ID3D11DeviceContext* ctx) {
  std::wstring dir = out_dir() + L"\\frames";
  CreateDirectoryW(dir.c_str(), nullptr);
  bool bgr = g_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || g_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  FILE* times = _wfopen((dir + L"\\times.txt").c_str(), L"w");
  std::vector<unsigned char> rgb(size_t(g_desc.Width) * g_desc.Height * 3);
  for (size_t i = 0; i < g_frames.size(); ++i) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_frames[i].tex, 0, D3D11_MAP_READ, 0, &m))) continue;
    for (UINT y = 0; y < g_desc.Height; ++y) {
      auto* src = static_cast<const unsigned char*>(m.pData) + size_t(y) * m.RowPitch;
      unsigned char* dst = rgb.data() + size_t(y) * g_desc.Width * 3;
      for (UINT x = 0; x < g_desc.Width; ++x) {
        dst[x * 3 + 0] = src[x * 4 + (bgr ? 2 : 0)];
        dst[x * 3 + 1] = src[x * 4 + 1];
        dst[x * 3 + 2] = src[x * 4 + (bgr ? 0 : 2)];
      }
    }
    ctx->Unmap(g_frames[i].tex, 0);
    wchar_t name[32];
    swprintf(name, 32, L"\\%04zu.ppm", i);
    if (FILE* f = _wfopen((dir + name).c_str(), L"wb")) {
      fprintf(f, "P6\n%u %u\n255\n", g_desc.Width, g_desc.Height);
      fwrite(rgb.data(), 1, rgb.size(), f);
      fclose(f);
    }
    if (times) fprintf(times, "%zu %lld\n", i, g_frames[i].t_ns);
  }
  if (times) fclose(times);
  for (auto& fr : g_frames) fr.tex->Release();
  ACO_LOG("framedump: wrote %zu frames (%ux%u fmt %d)", g_frames.size(), g_desc.Width, g_desc.Height, g_desc.Format);
  g_frames.clear();
  g_want = 0;
}

} // namespace

void framedump_on_present(IDXGISwapChain* sc) {
  if (!g_want) {
    if (++g_poll % 32) return;
    std::wstring trigger = out_dir() + L"\\framedump";
    if (GetFileAttributesW(trigger.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    int n = 60;
    if (FILE* f = _wfopen(trigger.c_str(), L"r")) {
      if (fscanf(f, "%d", &n) != 1) n = 60;
      fclose(f);
    }
    DeleteFileW(trigger.c_str());
    g_want = n < 1 ? 1 : (n > 600 ? 600 : n);
    ACO_LOG("framedump: capturing %d frames", g_want);
  }
  ID3D11Texture2D* bb = nullptr;
  if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return;
  ID3D11Device* dev = nullptr;
  bb->GetDevice(&dev);
  ID3D11DeviceContext* ctx = nullptr;
  dev->GetImmediateContext(&ctx);
  D3D11_TEXTURE2D_DESC d{};
  bb->GetDesc(&d);
  g_desc = d;
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  d.MiscFlags = 0;
  d.SampleDesc = {1, 0};
  d.MipLevels = 1;
  d.ArraySize = 1;
  Frame fr;
  if (SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &fr.tex))) {
    ctx->CopyResource(fr.tex, bb);
    fr.t_ns = now_ns();
    g_frames.push_back(fr);
  }
  if (int(g_frames.size()) >= g_want) write_all(ctx);
  ctx->Release();
  dev->Release();
  bb->Release();
}

} // namespace aco
