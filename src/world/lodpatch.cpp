#include "world/lodpatch.h"

#include <cmath>
#include <cstring>

namespace aco {

namespace {

uint32_t rd32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}
void wr32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

void md5_block(uint32_t st[4], const uint8_t* blk) {
  static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                            14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                            4,  11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
  static uint32_t K[64];
  static bool init = [] {
    for (int i = 0; i < 64; ++i) K[i] = static_cast<uint32_t>(std::fabs(std::sin(double(i + 1))) * 4294967296.0);
    return true;
  }();
  (void)init;
  uint32_t X[16];
  memcpy(X, blk, 64);
  uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
  for (int i = 0; i < 64; ++i) {
    uint32_t f;
    int g;
    if (i < 16) { f = (b & c) | (~b & d); g = i; }
    else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
    else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
    else { f = c ^ (b | ~d); g = (7 * i) % 16; }
    uint32_t t = d;
    d = c;
    c = b;
    b = b + rol(a + f + K[i] + X[g], S[i]);
    a = t;
  }
  st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

// SM4/5 token helpers.
constexpr uint32_t kOpMul = 0x38, kOpGe = 0x1d, kOpCustomData = 0x35;
constexpr uint32_t kTypeTemp = 0, kTypeImm32 = 4, kTypeCB = 8;

struct Operand {
  uint32_t type = 0, ncomp = 0, selmode = 0, sel = 0, dim = 0;
  uint32_t idx[3] = {};
  bool ok = true;
};

// Decodes the operands of a plain ALU instruction at tok[i] (length ln). Relative indexing is
// reported as !ok (never needed for the instructions we match).
std::vector<Operand> operands(const std::vector<uint32_t>& tok, size_t i, size_t ln) {
  std::vector<Operand> out;
  size_t j = i + 1;
  if (tok[i] & 0x80000000u) {
    while (j < i + ln && (tok[j] & 0x80000000u)) ++j;
    ++j;
  }
  while (j < i + ln) {
    uint32_t t = tok[j++];
    Operand o;
    o.ncomp = t & 3;
    o.selmode = (t >> 2) & 3;
    o.sel = (t >> 4) & 0xff;
    o.type = (t >> 12) & 0xff;
    o.dim = (t >> 20) & 3;
    if (t & 0x80000000u) ++j;  // extended operand token (modifiers)
    for (uint32_t d = 0; d < o.dim && d < 3; ++d) {
      uint32_t rep = (t >> (22 + 3 * d)) & 7;
      if (rep != 0 || j >= tok.size()) { o.ok = false; out.push_back(o); return out; }
      o.idx[d] = tok[j++];
    }
    if (o.type == kTypeImm32) j += (o.ncomp == 2) ? 4 : 1;
    out.push_back(o);
  }
  return out;
}

} // namespace

void dxbc_checksum(const uint8_t* data, size_t size, uint8_t out[16]) {
  const uint8_t* body = data + 20;
  size_t n = size - 20;
  uint32_t bits = static_cast<uint32_t>(n * 8);
  uint32_t st[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
  size_t full = n - (n % 64);
  for (size_t o = 0; o < full; o += 64) md5_block(st, body + o);
  size_t left = n - full;
  uint8_t blk[64];
  if (left >= 56) {
    memset(blk, 0, 64);
    memcpy(blk, body + full, left);
    blk[left] = 0x80;
    md5_block(st, blk);
    memset(blk, 0, 64);
    wr32(blk, bits);
    wr32(blk + 60, (bits >> 2) | 1);
    md5_block(st, blk);
  } else {
    memset(blk, 0, 64);
    wr32(blk, bits);
    memcpy(blk + 4, body + full, left);
    blk[4 + left] = 0x80;
    wr32(blk + 60, (bits >> 2) | 1);
    md5_block(st, blk);
  }
  memcpy(out, st, 16);
}

bool lod_patch_far(const std::vector<uint8_t>& in, float k, std::vector<uint8_t>& out, std::string* why) {
  auto fail = [&](const char* s) {
    if (why) *why = s;
    return false;
  };
  if (in.size() < 32 || memcmp(in.data(), "DXBC", 4) != 0) return fail("not DXBC");
  uint32_t nchunks = rd32(&in[28]);
  if (32 + 4 * size_t(nchunks) > in.size()) return fail("bad header");
  size_t shex = 0;
  for (uint32_t c = 0; c < nchunks; ++c) {
    uint32_t o = rd32(&in[32 + 4 * c]);
    if (o + 8 > in.size()) return fail("bad chunk");
    if (!memcmp(&in[o], "SHEX", 4) || !memcmp(&in[o], "SHDR", 4)) shex = o;
  }
  if (!shex) return fail("no SHEX");
  uint32_t size = rd32(&in[shex + 4]);
  if (shex + 8 + size_t(size) > in.size() || size % 4) return fail("bad SHEX size");
  std::vector<uint32_t> tok(size / 4);
  memcpy(tok.data(), &in[shex + 8], size);
  if (tok.size() < 2 || tok[1] != tok.size()) return fail("SHEX length mismatch");

  // Instruction starts.
  std::vector<size_t> at;
  for (size_t i = 2; i < tok.size();) {
    uint32_t op = tok[i] & 0x7ff;
    size_t ln = (op == kOpCustomData) ? tok[i + 1] : (tok[i] >> 24) & 0x7f;
    if (!ln || i + ln > tok.size()) return fail("bad instruction length");
    at.push_back(i);
    i += ln;
  }
  auto len_of = [&](size_t n) { return (n + 1 < at.size() ? at[n + 1] : tok.size()) - at[n]; };

  for (size_t n = 0; n < at.size(); ++n) {
    size_t i = at[n];
    if ((tok[i] & 0x7ff) != kOpMul) continue;
    auto ops = operands(tok, i, len_of(n));
    if (ops.size() != 3 || !ops[2].ok) continue;
    const Operand& s2 = ops[2];
    // cb8[35].w (GeometricObjectLODDistanceScale), selected as a single component.
    if (s2.type != kTypeCB || s2.dim != 2 || s2.idx[0] != 8 || s2.idx[1] != 35 || s2.selmode != 2 || (s2.sel & 3) != 3)
      continue;
    // The first `ge` after it compares the distance with the range's max: R.z.
    for (size_t m = n + 1; m < at.size() && m < n + 16; ++m) {
      if ((tok[at[m]] & 0x7ff) != kOpGe) continue;
      auto g = operands(tok, at[m], len_of(m));
      if (g.size() != 3 || !g[2].ok || g[2].type != kTypeTemp || g[2].selmode != 2 || (g[2].sel & 3) != 2)
        return fail("ge after the LOD distance does not read a temp .z");
      uint32_t r = g[2].idx[0];
      uint32_t kbits;
      memcpy(&kbits, &k, 4);
      // mul rR.z, rR.z, l(k)
      const uint32_t ins[7] = {0x07000038u, 0x00100042u, r, 0x0010002Au, r, 0x00004001u, kbits};
      size_t pos = i + len_of(n);
      tok.insert(tok.begin() + pos, ins, ins + 7);
      tok[1] = static_cast<uint32_t>(tok.size());

      size_t newsize = tok.size() * 4, delta = newsize - size;
      out.clear();
      out.reserve(in.size() + delta);
      out.insert(out.end(), in.begin(), in.begin() + shex + 4);
      out.resize(shex + 8);
      wr32(&out[shex + 4], static_cast<uint32_t>(newsize));
      out.resize(shex + 8 + newsize);
      memcpy(&out[shex + 8], tok.data(), newsize);
      out.insert(out.end(), in.begin() + shex + 8 + size, in.end());
      for (uint32_t c = 0; c < nchunks; ++c) {
        uint32_t o = rd32(&out[32 + 4 * c]);
        if (o > shex) wr32(&out[32 + 4 * c], static_cast<uint32_t>(o + delta));
      }
      wr32(&out[24], static_cast<uint32_t>(out.size()));
      dxbc_checksum(out.data(), out.size(), &out[4]);
      return true;
    }
    return fail("no ge after the LOD distance");
  }
  return fail("no LOD distance (mul by cb8[35].w)");
}

} // namespace aco
