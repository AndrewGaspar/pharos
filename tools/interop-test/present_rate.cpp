// How many presents per second the display path sustains for a trivial frame (one clear), and how
// long each IDXGISwapChain::Present call takes. Same swapchain shape as AC Origins (blt model,
// DISCARD, 3 buffers, sync 0).
//   wine present_rate.exe [width height seconds [flip]]
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cstdio>
#include <cstdlib>

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

int main(int argc, char** argv) {
  UINT w = argc > 2 ? atoi(argv[1]) : 2560, h = argc > 2 ? atoi(argv[2]) : 1600;
  double secs = argc > 3 ? atof(argv[3]) : 5.0;
  bool flip = argc > 4 && atoi(argv[4]) != 0;
  WNDCLASSW wc{};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"present_rate";
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"present_rate", WS_POPUP | WS_VISIBLE, 0, 0, w, h, nullptr, nullptr,
                              wc.hInstance, nullptr);
  DXGI_SWAP_CHAIN_DESC d{};
  d.BufferDesc.Width = w;
  d.BufferDesc.Height = h;
  d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  d.BufferCount = 3;
  d.OutputWindow = hwnd;
  d.Windowed = TRUE;
  d.SwapEffect = flip ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;
  IDXGISwapChain* sc = nullptr;
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                             &d, &sc, &dev, nullptr, &ctx);
  if (FAILED(hr)) return printf("D3D11CreateDeviceAndSwapChain -> 0x%08lx\n", hr), 1;
  ID3D11Texture2D* bb = nullptr;
  sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb));
  ID3D11RenderTargetView* rtv = nullptr;
  dev->CreateRenderTargetView(bb, nullptr, &rtv);
  LARGE_INTEGER f, t0, a, b;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t0);
  double in_present = 0, max_present = 0;
  int n = 0;
  for (;;) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    float c[4] = {float(n & 255) / 255.0f, 0.2f, 0.4f, 1.0f};
    ctx->ClearRenderTargetView(rtv, c);
    QueryPerformanceCounter(&a);
    sc->Present(0, 0);
    QueryPerformanceCounter(&b);
    double ms = 1000.0 * double(b.QuadPart - a.QuadPart) / double(f.QuadPart);
    in_present += ms;
    if (ms > max_present) max_present = ms;
    ++n;
    if (double(b.QuadPart - t0.QuadPart) / double(f.QuadPart) >= secs) break;
  }
  double el = double(b.QuadPart - t0.QuadPart) / double(f.QuadPart);
  printf("%ux%u %s: %d presents in %.2f s = %.1f presents/s; Present() avg %.2f ms, max %.2f ms\n", w, h,
         flip ? "flip-discard" : "discard (blt)", n, el, n / el, in_present / n, max_present);
  return 0;
}
