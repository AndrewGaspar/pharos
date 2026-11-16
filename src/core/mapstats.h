#pragma once

struct ID3D11Resource;

namespace aco {

// Debug: when <out>\mapstats exists (content: frame count, default 32), logs for that many
// consecutive frames a histogram of CPU uploads (Map for writing / UpdateSubresource) to
// non-constant buffers, keyed by bind flags and size, plus the frame interval. Work that runs
// only every Nth frame (e.g. a CPU cloth simulation uploading vertices) shows up as a periodic key.
void mapstats_note(ID3D11Resource* res, bool update_subresource);
void mapstats_on_present();

} // namespace aco
