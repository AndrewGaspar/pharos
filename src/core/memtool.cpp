#include "core/memtool.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"

namespace aco {

namespace {

uintptr_t image_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

bool readable(const void* p, size_t n) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  return reinterpret_cast<uintptr_t>(p) + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

void dump_object(uintptr_t a, size_t bytes) {
  std::string line;
  char buf[64];
  for (size_t off = 0; off < bytes; off += 4) {
    uint32_t u;
    memcpy(&u, reinterpret_cast<void*>(a + off), 4);
    float f;
    memcpy(&f, &u, 4);
    if (u == 0) snprintf(buf, sizeof(buf), " +%zx:0", off);
    else if (u < 0x10000) snprintf(buf, sizeof(buf), " +%zx:i%u", off, u);
    else if (f > -1e7f && f < 1e7f && (f > 1e-6f || f < -1e-6f)) snprintf(buf, sizeof(buf), " +%zx:%g", off, f);
    else snprintf(buf, sizeof(buf), " +%zx:%08x", off, u);
    line += buf;
  }
  ACO_LOG("memtool:   %p%s", reinterpret_cast<void*>(a), line.c_str());
}

// Calls fn(object) for every heap object whose first qword is vt.
template <typename F>
int for_each_instance(uintptr_t vt, F fn) {
  uintptr_t addr = 0x10000;
  int found = 0;
  MEMORY_BASIC_INFORMATION mbi{};
  while (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) {
    uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = start + mbi.RegionSize;
    if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) &&
        !(mbi.Protect & PAGE_GUARD)) {
      for (uintptr_t p = start; p + 8 <= end; p += 8) {
        if (*reinterpret_cast<uintptr_t*>(p) != vt) continue;
        // Skip our own copies of the needle (e.g. this function's locals / parsed command).
        if (p >= reinterpret_cast<uintptr_t>(&mbi) - 0x10000 && p < reinterpret_cast<uintptr_t>(&mbi) + 0x10000) continue;
        ++found;
        fn(p);
      }
    }
    if (end <= addr) break;
    addr = end;
    if (addr >= 0x7fffffff0000ull) break;
  }
  return found;
}

void scan_vtable(uintptr_t vt, size_t bytes) {
  int shown = 0;
  int found = for_each_instance(vt, [&](uintptr_t p) {
    if (shown++ < 64 && readable(reinterpret_cast<void*>(p), bytes)) dump_object(p, bytes);
  });
  ACO_LOG("memtool: vtable %p: %d instances", reinterpret_cast<void*>(vt), found);
}

// ---- find: every qword equal to a value in writable memory (heap and the image's data) ----
void find_value(uint64_t value, int max) {
  uintptr_t addr = 0x10000;
  int found = 0;
  MEMORY_BASIC_INFORMATION mbi{};
  while (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) {
    uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = start + mbi.RegionSize;
    if (mbi.State == MEM_COMMIT && (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_IMAGE) &&
        (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_READONLY)) && !(mbi.Protect & PAGE_GUARD)) {
      for (uintptr_t p = start; p + 8 <= end; p += 8) {
        if (*reinterpret_cast<uint64_t*>(p) != value) continue;
        if (found++ < max) {
          uintptr_t b = image_base();
          if (mbi.Type == MEM_IMAGE && p >= b && p < b + 0x10000000) ACO_LOG("memtool:   %p (rva %llx)", reinterpret_cast<void*>(p), static_cast<unsigned long long>(p - b));
          else ACO_LOG("memtool:   %p", reinterpret_cast<void*>(p));
        }
      }
    }
    if (end <= addr) break;
    addr = end;
    if (addr >= 0x7fffffff0000ull) break;
  }
  ACO_LOG("memtool: find %llx: %d hits", static_cast<unsigned long long>(value), found);
}

void write_bytes(uintptr_t a, const void* v, size_t n) {
  DWORD old = 0;
  bool prot = VirtualProtect(reinterpret_cast<void*>(a), n, PAGE_READWRITE, &old);
  memcpy(reinterpret_cast<void*>(a), v, n);
  if (prot) VirtualProtect(reinterpret_cast<void*>(a), n, old, &old);
}

// "r<hex>" = image base + rva, otherwise an absolute hex address.
unsigned long long parse_addr(const char* s) {
  if (s[0] == 'r') return image_base() + strtoull(s + 1, nullptr, 16);
  return strtoull(s, nullptr, 16);
}

// ---- vec3 scanner (Cheat-Engine-style first scan / refine) ----
struct Cand {
  uintptr_t a;
  float v[3];
};
std::vector<Cand> g_cands;

bool near3(const float* v, const float* p, float r) {
  float dx = v[0] - p[0], dy = v[1] - p[1], dz = v[2] - p[2];
  return dx * dx + dy * dy + dz * dz <= r * r;
}

template <typename F>
void for_each_rw_region(F fn) {
  uintptr_t addr = 0x10000;
  MEMORY_BASIC_INFORMATION mbi{};
  while (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) {
    uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = start + mbi.RegionSize;
    if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) &&
        !(mbi.Protect & PAGE_GUARD))
      fn(start, end);
    if (end <= addr) break;
    addr = end;
    if (addr >= 0x7fffffff0000ull) break;
  }
}

void log_cands(size_t n) {
  for (size_t i = 0; i < g_cands.size() && i < n; ++i) {
    const Cand& c = g_cands[i];
    float v[3] = {};
    SIZE_T got = 0;
    ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(c.a), v, 12, &got);
    ACO_LOG("memtool:   %p  %.3f %.3f %.3f", reinterpret_cast<void*>(c.a), v[0], v[1], v[2]);
  }
}

bool run_scan(const char* line) {
  char cmd[16] = {};
  float a[4] = {};
  int n = sscanf(line, "%15s %f %f %f %f", cmd, &a[0], &a[1], &a[2], &a[3]);
  if (n < 1) return false;
  if (!strcmp(cmd, "fs") && n == 5) {  // fs x y z r: every 4-aligned float3 within r of (x,y,z)
    ACO_LOG("memtool: %s", line);
    g_cands.clear();
    size_t total = 0;
    // Copy through ReadProcessMemory: regions can be freed by game threads mid-scan, and a
    // failed copy is just skipped instead of faulting.
    static std::vector<uint8_t> buf(1 << 20);
    size_t regions = 0, bytes = 0;
    for_each_rw_region([&](uintptr_t start, uintptr_t end) {
      ++regions;
      for (uintptr_t c = start; c < end; c += buf.size() - 8) {
        size_t len = std::min<size_t>(buf.size(), end - c);
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(c), buf.data(), len, &got) || got < 12) continue;
        bytes += got;
        for (size_t o = 0; o + 12 <= got; o += 4) {
          const float* v = reinterpret_cast<const float*>(buf.data() + o);
          if (!near3(v, a, a[3])) continue;
          // Skip our own scan buffer.
          if (c + o >= reinterpret_cast<uintptr_t>(buf.data()) && c + o < reinterpret_cast<uintptr_t>(buf.data()) + buf.size()) continue;
          if (++total <= 500000) g_cands.push_back({c + o, {v[0], v[1], v[2]}});
        }
        if (len < buf.size()) break;
      }
    });
    ACO_LOG("memtool: fs: scanned %zu regions, %zu MB", regions, bytes >> 20);
    ACO_LOG("memtool: fs: %zu matches (%zu kept)", total, g_cands.size());
    log_cands(32);
    return true;
  }
  if ((!strcmp(cmd, "fr") && n == 5) || !strcmp(cmd, "fc") || !strcmp(cmd, "fu") || (!strcmp(cmd, "fd") && n == 5)) {
    // fr x y z r: keep those now within r of (x,y,z); fc/fu: keep changed/unchanged since last
    // scan; fd dx dy dz tol: keep those that moved by (dx,dy,dz) +- tol.
    ACO_LOG("memtool: %s", line);
    std::vector<Cand> keep;
    for (Cand c : g_cands) {
      float v[3];
      SIZE_T got = 0;
      if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(c.a), v, 12, &got) || got != 12) continue;
      bool changed = memcmp(v, c.v, 12) != 0;
      bool ok = cmd[1] == 'r' ? near3(v, a, a[3]) : cmd[1] == 'c' ? changed : cmd[1] == 'u' ? !changed : false;
      if (cmd[1] == 'd') {
        float d[3] = {v[0] - c.v[0], v[1] - c.v[1], v[2] - c.v[2]};
        ok = near3(d, a, a[3]);
      }
      if (!ok) continue;
      memcpy(c.v, v, 12);
      keep.push_back(c);
    }
    g_cands.swap(keep);
    ACO_LOG("memtool: %s: %zu left", cmd, g_cands.size());
    log_cands(32);
    return true;
  }
  if ((!strcmp(cmd, "f1") || !strcmp(cmd, "f1a")) && n == 3) {
    // f1 v tol: every 4-aligned float within tol of v (f1a: add to the current candidates).
    ACO_LOG("memtool: %s", line);
    if (!strcmp(cmd, "f1")) g_cands.clear();
    static std::vector<uint8_t> buf(1 << 20);
    size_t total = 0;
    for_each_rw_region([&](uintptr_t start, uintptr_t end) {
      for (uintptr_t c = start; c < end; c += buf.size()) {
        size_t len = std::min<size_t>(buf.size(), end - c);
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(c), buf.data(), len, &got) || got < 4) continue;
        if (c <= reinterpret_cast<uintptr_t>(buf.data()) + buf.size() && reinterpret_cast<uintptr_t>(buf.data()) < c + got) continue;
        for (size_t o = 0; o + 4 <= got; o += 4) {
          float v;
          memcpy(&v, buf.data() + o, 4);
          if (!(std::fabs(v - a[0]) <= a[1])) continue;
          if (++total, g_cands.size() < 2000000) g_cands.push_back({c + o, {v, 0, 0}});
        }
      }
    });
    ACO_LOG("memtool: %s: %zu matches, %zu candidates", cmd, total, g_cands.size());
    return true;
  }
  if ((!strcmp(cmd, "f1r") || !strcmp(cmd, "f1d")) && n == 3) {
    // f1r v tol: keep floats now within tol of v; f1d d tol: keep those whose |change| is d +- tol.
    ACO_LOG("memtool: %s", line);
    std::vector<Cand> keep;
    for (Cand c : g_cands) {
      float v;
      SIZE_T got = 0;
      if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(c.a), &v, 4, &got) || got != 4) continue;
      bool ok = cmd[2] == 'r' ? std::fabs(v - a[0]) <= a[1] : std::fabs(std::fabs(v - c.v[0]) - a[0]) <= a[1];
      if (!ok) continue;
      c.v[0] = v;
      keep.push_back(c);
    }
    g_cands.swap(keep);
    ACO_LOG("memtool: %s: %zu left", cmd, g_cands.size());
    for (size_t i = 0; i < g_cands.size() && i < 64; ++i)
      ACO_LOG("memtool:   %p  %g", reinterpret_cast<void*>(g_cands[i].a), g_cands[i].v[0]);
    return true;
  }
  if (!strcmp(cmd, "fl")) {
    ACO_LOG("memtool: fl: %zu candidates", g_cands.size());
    log_cands(n >= 2 ? size_t(a[0]) : 256);
    return true;
  }
  return false;
}

// fp <value> [range]: qwords holding a pointer into [value - range, value] (default range 0), in
// private RW memory and in the image. Image hits are shown as rva.
void find_pointers(uintptr_t value, uintptr_t range) {
  static std::vector<uint8_t> buf(1 << 20);
  uintptr_t base = image_base();
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
  uintptr_t isize = nt->OptionalHeader.SizeOfImage;
  int hits = 0;
  auto scan = [&](uintptr_t start, uintptr_t end) {
    for (uintptr_t c = start; c < end; c += buf.size()) {
      size_t len = std::min<size_t>(buf.size(), end - c);
      SIZE_T got = 0;
      if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(c), buf.data(), len, &got)) {
        // Partially unreadable (image sections): fall back to pages.
        for (uintptr_t pg = c; pg < c + len; pg += 0x1000) {
          SIZE_T g2 = 0;
          if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(pg), buf.data() + (pg - c), 0x1000, &g2)) memset(buf.data() + (pg - c), 0, 0x1000);
        }
        got = len;
      }
      if (c <= reinterpret_cast<uintptr_t>(buf.data()) + buf.size() && reinterpret_cast<uintptr_t>(buf.data()) < c + got) continue;
      for (size_t o = 0; o + 8 <= got; o += 8) {
        uintptr_t v;
        memcpy(&v, buf.data() + o, 8);
        if (v > value || v + range < value) continue;
        uintptr_t at = c + o;
        if (++hits <= 200) {
          if (at >= base && at < base + isize)
            ACO_LOG("memtool:   image rva %#llx -> +%#llx", (unsigned long long)(at - base), (unsigned long long)(value - v));
          else
            ACO_LOG("memtool:   %p -> +%#llx", reinterpret_cast<void*>(at), (unsigned long long)(value - v));
        }
      }
    }
  };
  scan(base, base + isize);
  for_each_rw_region(scan);
  ACO_LOG("memtool: fp: %d hits", hits);
}

// Multi-level reverse pointer scan. pp <value> <range>: level 1 = all qwords v with
// value - range <= v <= value; pn <range>: next level, targets = previous level's hit addresses.
// Hits inside the image (static roots) are logged with their chain.
struct PHit {
  uintptr_t at;      // where the pointer is stored
  uint32_t parent;   // index into previous level
  uint32_t off;      // target - pointer value
};
std::vector<std::vector<PHit>> g_levels;

void pointer_level(const std::vector<uintptr_t>& targets_in, uintptr_t range) {
  std::vector<std::pair<uintptr_t, uint32_t>> t;  // sorted targets with their indices
  for (uint32_t i = 0; i < targets_in.size(); ++i) t.push_back({targets_in[i], i});
  std::sort(t.begin(), t.end());
  if (t.empty()) return;
  uintptr_t lo = t.front().first - range, hi = t.back().first;
  static std::vector<uint8_t> buf(1 << 20);
  uintptr_t base = image_base();
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
  uintptr_t isize = nt->OptionalHeader.SizeOfImage;
  std::vector<PHit> hits;
  size_t image_hits = 0;
  auto scan = [&](uintptr_t start, uintptr_t end) {
    for (uintptr_t c = start; c < end; c += buf.size()) {
      size_t len = std::min<size_t>(buf.size(), end - c);
      SIZE_T got = 0;
      if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(c), buf.data(), len, &got)) {
        for (uintptr_t pg = c; pg < c + len; pg += 0x1000) {
          SIZE_T g2 = 0;
          if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(pg), buf.data() + (pg - c), 0x1000, &g2))
            memset(buf.data() + (pg - c), 0, 0x1000);
        }
        got = len;
      }
      if (c <= reinterpret_cast<uintptr_t>(buf.data()) + buf.size() && reinterpret_cast<uintptr_t>(buf.data()) < c + got) continue;
      for (size_t o = 0; o + 8 <= got; o += 8) {
        uintptr_t v;
        memcpy(&v, buf.data() + o, 8);
        if (v < lo || v > hi) continue;
        auto it = std::lower_bound(t.begin(), t.end(), std::make_pair(v, uint32_t(0)));  // nearest target >= v
        if (it == t.end() || it->first > v + range) continue;
        uintptr_t at = c + o;
        if (at == it->first) continue;
        if (hits.size() < 300000) hits.push_back({at, it->second, uint32_t(it->first - v)});
        if (at >= base && at < base + isize) ++image_hits;
      }
    }
  };
  scan(base, base + isize);
  for_each_rw_region(scan);
  g_levels.push_back(hits);
  ACO_LOG("memtool: level %zu: %zu hits (%zu in image)", g_levels.size(), hits.size(), image_hits);
  int shown = 0;
  for (const PHit& h : hits) {
    if (h.at < base || h.at >= base + isize || shown++ >= 100) continue;
    char b[64];
    snprintf(b, sizeof(b), "rva %#llx", (unsigned long long)(h.at - base));
    std::string chain = b;
    const PHit* cur = &h;
    for (size_t lvl = g_levels.size() - 1;; --lvl) {
      snprintf(b, sizeof(b), " -> [+%#x]", cur->off);
      chain += b;
      if (lvl == 0) break;
      cur = &g_levels[lvl - 1][cur->parent];
    }
    ACO_LOG("memtool:   %s", chain.c_str());
  }
}

// fo <addr>: likely object starts before addr (qwords pointing into the image), for class lookup.
void find_object(uintptr_t a) {
  uintptr_t base = image_base();
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
  uintptr_t size = nt->OptionalHeader.SizeOfImage;
  for (uintptr_t p = a & ~uintptr_t(7); p + 0x800 > a && p > 0x10000; p -= 8) {
    if (!readable(reinterpret_cast<void*>(p), 8)) break;
    uintptr_t v = *reinterpret_cast<uintptr_t*>(p);
    if (v >= base && v < base + size) ACO_LOG("memtool:   -%#llx: rva %#llx", (unsigned long long)(a - p), (unsigned long long)(v - base));
  }
}

void run(const char* line) {
  if (run_scan(line)) return;
  char cmd[16] = {};
  unsigned long long a = 0;
  char a0[64] = {}, arg[64] = {}, arg2[64] = {};
  int n = sscanf(line, "%15s %63s %63s %63s", cmd, a0, arg, arg2);
  if (n < 2) return;
  ACO_LOG("memtool: %s", line);
  if (!strcmp(cmd, "find")) {  // find <hex value | r<rva>> [max]
    find_value(parse_addr(a0), n >= 3 ? atoi(arg) : 32);
    return;
  }
  // vt/vwf/vwi take a vtable RVA; everything else an address.
  a = (!strcmp(cmd, "vt") || !strcmp(cmd, "vwf") || !strcmp(cmd, "vwi")) ? strtoull(a0, nullptr, 16) : parse_addr(a0);
  if (!strcmp(cmd, "rq") && readable(reinterpret_cast<void*>(a), 8)) {  // rq <addr> <count>: qwords
    int cnt = n >= 3 ? atoi(arg) : 8;
    for (int i = 0; i < cnt && readable(reinterpret_cast<void*>(a + 8 * i), 8); ++i) {
      uint64_t v;
      memcpy(&v, reinterpret_cast<void*>(a + 8 * i), 8);
      uintptr_t b = image_base();
      if (v >= b && v < b + 0x10000000) ACO_LOG("memtool:   +%x: %016llx (rva %llx)", i * 8, static_cast<unsigned long long>(v), static_cast<unsigned long long>(v - b));
      else ACO_LOG("memtool:   +%x: %016llx", i * 8, static_cast<unsigned long long>(v));
    }
    return;
  }
  if (!strcmp(cmd, "wq") && n >= 3 && readable(reinterpret_cast<void*>(a), 8)) {
    uint64_t v = strtoull(arg, nullptr, 16);
    write_bytes(a, &v, 8);
    return;
  }
  if (!strcmp(cmd, "wb") && n >= 3 && readable(reinterpret_cast<void*>(a), 1)) {
    uint8_t v = static_cast<uint8_t>(strtoul(arg, nullptr, 16));
    write_bytes(a, &v, 1);
    return;
  }
  if (!strcmp(cmd, "vwf") && n >= 4) {  // vwf <vtable rva> <offset> <float>: write in every instance
    unsigned long off = strtoul(arg, nullptr, 16);
    float v = strtof(arg2, nullptr);
    int found = for_each_instance(image_base() + a, [&](uintptr_t p) { memcpy(reinterpret_cast<void*>(p + off), &v, 4); });
    ACO_LOG("memtool: wrote %g at +%lx in %d instances", v, off, found);
  } else if (!strcmp(cmd, "vwi") && n >= 4) {
    unsigned long off = strtoul(arg, nullptr, 16);
    int32_t v = strtol(arg2, nullptr, 0);
    int found = for_each_instance(image_base() + a, [&](uintptr_t p) { memcpy(reinterpret_cast<void*>(p + off), &v, 4); });
    ACO_LOG("memtool: wrote %d at +%lx in %d instances", v, off, found);
  } else if (!strcmp(cmd, "vt")) {
    scan_vtable(image_base() + a, n >= 3 ? strtoul(arg, nullptr, 0) : 0x40);
  } else if (!strcmp(cmd, "pp")) {
    g_levels.clear();
    pointer_level({a}, n >= 3 ? strtoull(arg, nullptr, 16) : 0);
  } else if (!strcmp(cmd, "pn") && !g_levels.empty()) {  // pn <range>
    std::vector<uintptr_t> targets;
    for (const PHit& h : g_levels.back()) targets.push_back(h.at);
    pointer_level(targets, a);
  } else if (!strcmp(cmd, "fp")) {
    find_pointers(a, n >= 3 ? strtoull(arg, nullptr, 16) : 0);
  } else if (!strcmp(cmd, "fo")) {
    find_object(a);
  } else if (!strcmp(cmd, "rq") && readable(reinterpret_cast<void*>(a), 8)) {  // rq <addr> <count>: qwords
    unsigned cnt = n >= 3 ? strtoul(arg, nullptr, 0) : 8;
    for (unsigned i = 0; i < cnt && readable(reinterpret_cast<void*>(a + 8 * i), 8); ++i) {
      uint64_t v = *reinterpret_cast<uint64_t*>(a + 8 * i);
      uintptr_t b = image_base();
      ACO_LOG("memtool:   +%#x: %#llx%s", i * 8, (unsigned long long)v, v >= b && v < b + 0x10000000 ? " (image)" : "");
    }
  } else if (!strcmp(cmd, "rf") && readable(reinterpret_cast<void*>(a), 4)) {
    dump_object(a, n >= 3 ? strtoul(arg, nullptr, 0) * 4 : 16);
  } else if (!strcmp(cmd, "wf") && n >= 3 && readable(reinterpret_cast<void*>(a), 4)) {
    float v = strtof(arg, nullptr);
    write_bytes(a, &v, 4);
  } else if (!strcmp(cmd, "wi") && n >= 3 && readable(reinterpret_cast<void*>(a), 4)) {
    int32_t v = strtol(arg, nullptr, 0);
    write_bytes(a, &v, 4);
  }
}

std::atomic<uint64_t> g_draws{0}, g_indices{0}, g_indirect{0}, g_skin{0}, g_skin_groups{0};
// Draw statistics are a debugging aid (shared atomics on every draw): on only while <out>\geo exists.
std::atomic<bool> g_geo_on{false};

} // namespace

void geo_count(unsigned long long indices, bool indirect) {
  if (!g_geo_on.load(std::memory_order_relaxed)) return;
  g_draws.fetch_add(1, std::memory_order_relaxed);
  if (indirect) g_indirect.fetch_add(1, std::memory_order_relaxed);
  else g_indices.fetch_add(indices, std::memory_order_relaxed);
}

void skin_count(unsigned long long groups) {
  if (!g_geo_on.load(std::memory_order_relaxed)) return;
  g_skin.fetch_add(1, std::memory_order_relaxed);
  g_skin_groups.fetch_add(groups, std::memory_order_relaxed);
}

void memtool_poll() {
  static unsigned frames = 0;
  static uint64_t last_ms = 0;
  ++frames;
  uint64_t now = GetTickCount64();
  if (now - last_ms >= 2000) {
    g_geo_on.store(GetFileAttributesW((out_dir() + L"\\geo").c_str()) != INVALID_FILE_ATTRIBUTES,
                   std::memory_order_relaxed);
    if (last_ms && g_geo_on.load())
      ACO_LOG("geo: %.0f draws/frame, %.2fM indices/frame, %.0f indirect/frame, skinning %.1f dispatches %.0f groups/frame",
              double(g_draws.exchange(0)) / frames, double(g_indices.exchange(0)) / frames / 1e6,
              double(g_indirect.exchange(0)) / frames, double(g_skin.exchange(0)) / frames,
              double(g_skin_groups.exchange(0)) / frames);
    else
      g_draws = g_indices = g_indirect = g_skin = g_skin_groups = 0;
    last_ms = now;
    frames = 0;
  }
  if (frames % 64) return;
  std::wstring path = out_dir() + L"\\memtool.txt";
  FILE* f = _wfopen(path.c_str(), L"r");
  if (!f) return;
  std::vector<std::string> lines;
  char buf[256];
  while (fgets(buf, sizeof(buf), f)) lines.emplace_back(buf);
  fclose(f);
  DeleteFileW(path.c_str());
  ACO_LOG("memtool: image base %p", reinterpret_cast<void*>(image_base()));
  for (auto& l : lines) run(l.c_str());
  ACO_LOG("memtool: done");
}

} // namespace aco
