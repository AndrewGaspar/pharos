// D3D11 (DXVK) <-> D3D12 (vkd3d-proton) shared resource round trip, the building block of the D3D12
// Frame Generation route. Run in the instance prefix:
//   WINEDLLOVERRIDES="d3d11,dxgi,d3d12,d3d12core=n" wine interop_test.exe
// D3D11 writes a pattern into shared texture A and signals a shared fence; D3D12 waits, copies A
// into shared texture B and signals; D3D11 waits, reads B back and checks every pixel.
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

#define CHECK(x)                                                                         \
  do {                                                                                   \
    HRESULT hr_ = (x);                                                                   \
    if (FAILED(hr_)) {                                                                   \
      printf("FAIL %s -> 0x%08lx (line %d)\n", #x, static_cast<unsigned long>(hr_), __LINE__); \
      return 1;                                                                          \
    }                                                                                    \
  } while (0)

int main() {
  const UINT W = 256, H = 128;
  ID3D11Device* d11 = nullptr;
  ID3D11DeviceContext* c11 = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &d11, nullptr, &c11));
  ID3D11Device5* d11_5 = nullptr;
  ID3D11DeviceContext4* c11_4 = nullptr;
  CHECK(d11->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&d11_5)));
  CHECK(c11->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&c11_4)));

  IDXGIDevice* dxgi_dev = nullptr;
  IDXGIAdapter* adapter = nullptr;
  CHECK(d11->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_dev)));
  CHECK(dxgi_dev->GetAdapter(&adapter));
  DXGI_ADAPTER_DESC ad{};
  adapter->GetDesc(&ad);
  printf("D3D11 adapter: %ls (LUID %08lx:%08lx)\n", ad.Description, ad.AdapterLuid.HighPart, ad.AdapterLuid.LowPart);

  // Shared textures A, B and a shared fence on the D3D11 side.
  D3D11_TEXTURE2D_DESC td{};
  td.Width = W;
  td.Height = H;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
  std::vector<uint32_t> pattern(W * H);
  for (UINT y = 0; y < H; ++y)
    for (UINT x = 0; x < W; ++x) pattern[y * W + x] = 0xff000000u | (y << 8) | x | ((x ^ y) << 16);
  D3D11_SUBRESOURCE_DATA init{pattern.data(), W * 4, 0};
  ID3D11Texture2D *a11 = nullptr, *b11 = nullptr;
  CHECK(d11->CreateTexture2D(&td, &init, &a11));
  CHECK(d11->CreateTexture2D(&td, nullptr, &b11));
  HANDLE ha = nullptr, hb = nullptr, hf = nullptr;
  IDXGIResource1* r1 = nullptr;
  CHECK(a11->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&r1)));
  CHECK(r1->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &ha));
  r1->Release();
  CHECK(b11->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&r1)));
  CHECK(r1->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &hb));
  r1->Release();
  ID3D11Fence* f11 = nullptr;
  CHECK(d11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&f11)));
  CHECK(f11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &hf));
  printf("D3D11: shared handles A=%p B=%p fence=%p\n", ha, hb, hf);

  // D3D12 device on the same adapter.
  ID3D12Device* d12 = nullptr;
  CHECK(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&d12)));
  LUID l12 = d12->GetAdapterLuid();
  printf("D3D12 device created (LUID %08lx:%08lx)\n", l12.HighPart, l12.LowPart);
  ID3D12Resource *a12 = nullptr, *b12 = nullptr;
  ID3D12Fence* f12 = nullptr;
  CHECK(d12->OpenSharedHandle(ha, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&a12)));
  CHECK(d12->OpenSharedHandle(hb, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&b12)));
  CHECK(d12->OpenSharedHandle(hf, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&f12)));
  D3D12_RESOURCE_DESC rd = a12->GetDesc();
  printf("D3D12: opened A (%llux%u fmt %d flags 0x%x), B, fence\n", (unsigned long long)rd.Width, rd.Height, rd.Format,
         rd.Flags);

  ID3D12CommandQueue* q = nullptr;
  D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(d12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&q)));
  ID3D12CommandAllocator* alloc = nullptr;
  CHECK(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                    reinterpret_cast<void**>(&alloc)));
  ID3D12GraphicsCommandList* cl = nullptr;
  CHECK(d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, __uuidof(ID3D12GraphicsCommandList),
                               reinterpret_cast<void**>(&cl)));

  // D3D11: touch A on the GPU (overwrite the top-left 16x16 with a known value), signal 1.
  std::vector<uint32_t> patch(16 * 16, 0xff123456u);
  D3D11_BOX box{0, 0, 0, 16, 16, 1};
  c11->UpdateSubresource(a11, 0, &box, patch.data(), 16 * 4, 0);
  for (UINT y = 0; y < 16; ++y)
    for (UINT x = 0; x < 16; ++x) pattern[y * W + x] = 0xff123456u;
  CHECK(c11_4->Signal(f11, 1));
  c11->Flush();

  // D3D12: wait for 1, copy A -> B, signal 2.
  CHECK(q->Wait(f12, 1));
  D3D12_RESOURCE_BARRIER bar[2] = {};
  // Shared resources start in COMMON; COPY_SOURCE/DEST transitions and back.
  for (int i = 0; i < 2; ++i) {
    bar[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar[i].Transition.pResource = i ? b12 : a12;
    bar[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    bar[i].Transition.StateAfter = i ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE;
  }
  cl->ResourceBarrier(2, bar);
  cl->CopyResource(b12, a12);
  for (auto& b : bar) std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
  cl->ResourceBarrier(2, bar);
  CHECK(cl->Close());
  ID3D12CommandList* lists[] = {cl};
  q->ExecuteCommandLists(1, lists);
  CHECK(q->Signal(f12, 2));

  // D3D11: wait for 2, read B back.
  CHECK(c11_4->Wait(f11, 2));
  D3D11_TEXTURE2D_DESC sd = td;
  sd.Usage = D3D11_USAGE_STAGING;
  sd.BindFlags = 0;
  sd.MiscFlags = 0;
  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* staging = nullptr;
  CHECK(d11->CreateTexture2D(&sd, nullptr, &staging));
  c11->CopyResource(staging, b11);
  D3D11_MAPPED_SUBRESOURCE m{};
  CHECK(c11->Map(staging, 0, D3D11_MAP_READ, 0, &m));
  UINT bad = 0, first_bad = UINT32_MAX;
  for (UINT y = 0; y < H; ++y)
    for (UINT x = 0; x < W; ++x) {
      uint32_t v = static_cast<const uint32_t*>(m.pData)[y * (m.RowPitch / 4) + x];
      if (v != pattern[y * W + x]) {
        if (first_bad == UINT32_MAX) first_bad = y * W + x;
        ++bad;
      }
    }
  uint32_t px00 = static_cast<const uint32_t*>(m.pData)[0], px_far = static_cast<const uint32_t*>(m.pData)[(H - 1) * (m.RowPitch / 4) + W - 1];
  c11->Unmap(staging, 0);
  printf("round trip: %u of %u pixels wrong (first bad %u); B[0]=0x%08x (want 0xff123456), B[last]=0x%08x (want 0x%08x)\n",
         bad, W * H, first_bad == UINT32_MAX ? 0 : first_bad, px00, px_far, pattern[(H - 1) * W + W - 1]);
  printf("fence values: d3d11 %llu, d3d12 %llu\n", (unsigned long long)f11->GetCompletedValue(),
         (unsigned long long)f12->GetCompletedValue());
  printf(bad == 0 ? "RESULT: PASS\n" : "RESULT: FAIL\n");
  return bad == 0 ? 0 : 2;
}
