#pragma once

#include <d3d11.h>

#include <cstdint>

namespace aco {
// Called on private immediate-context state. Kinds 0 and 1 sample the
// upscaler input and output for one eye. Kind 2 samples the two finished eyes.
constexpr unsigned kBloomFinishedEyes = 2;
void bloom_probe_init();
bool bloom_probe_wants(uint64_t frame);
void bloom_probe_sample(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint64_t frame, unsigned kind,
                        ID3D11ShaderResourceView* input, ID3D11ShaderResourceView* output);
}  // namespace aco
