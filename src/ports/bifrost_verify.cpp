#include "ports/bifrost_verify.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "ports/bifrost_scene.h"

using Microsoft::WRL::ComPtr;
namespace bifrost {
bool VerifyFrames::begin(ID3D12GraphicsCommandList* list, ID3D12Resource* previous) {
  const auto d = previous->GetDesc();
  if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return false;
  if (!buffer || footprint.Footprint.Width != d.Width || footprint.Footprint.Height != d.Height ||
      footprint.Footprint.Format != d.Format) {
    ComPtr<ID3D12Device> dev;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&dev)))) return false;
    UINT64 size{};
    dev->GetCopyableFootprints(&d, 0, 1, 0, &footprint, nullptr, nullptr, &size);
    constexpr UINT64 alignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    stride = (size + alignment - 1) & ~(alignment - 1);
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 3 * stride;
    bd.Height = bd.MipLevels = bd.DepthOrArraySize = bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(buffer.ReleaseAndGetAddressOf()))))
      return false;
  }
  copy(list, previous, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 0);
  return true;
}
void VerifyFrames::copy(ID3D12GraphicsCommandList* list, ID3D12Resource* image, D3D12_RESOURCE_STATES state,
                        unsigned index) {
  transition(list, image, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
  dst.pResource = buffer.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = footprint;
  dst.PlacedFootprint.Offset = index * stride;
  src.pResource = image;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  transition(list, image, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
}
void VerifyFrames::finish(ID3D12GraphicsCommandList* list, ID3D12Resource* generated, ID3D12Resource* current) {
  copy(list, generated, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 1);
  copy(list, current, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 2);
}
void VerifyFrames::report() {
  BYTE* data{};
  D3D12_RANGE range{0, SIZE_T(3 * stride)}, written{};
  if (FAILED(buffer->Map(0, &range, reinterpret_cast<void**>(&data)))) return;
  unsigned samples = 0, moving = 0, between = 0;
  double gp = 0, gc = 0, pc = 0;
  for (UINT y = 0; y < footprint.Footprint.Height; y += 4)
    for (UINT x = 0; x < footprint.Footprint.Width; x += 4)
      for (unsigned c = 0; c < 3; ++c) {
        auto* p = data + y * footprint.Footprint.RowPitch + x * 4 + c;
        int a = *p, g = p[stride], b = p[2 * stride];
        ++samples;
        gp += std::abs(a - g);
        gc += std::abs(b - g);
        pc += std::abs(a - b);
        if (std::abs(a - b) > 8) {
          ++moving;
          between += g > std::min(a, b) && g < std::max(a, b);
        }
      }
  buffer->Unmap(0, &written);
  ACO_LOG("Bifrost: FG pixels |gen-prev| %.2f |gen-cur| %.2f |prev-cur| %.2f, moving %.1f%%, strictly between %.1f%%",
          gp / samples, gc / samples, pc / samples, 100.0 * moving / samples, moving ? 100.0 * between / moving : 0.0);
}
}  // namespace bifrost
