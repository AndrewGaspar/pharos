#include "core/memtool.h"

#include <windows.h>

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

void run(const char* line) {
  char cmd[16] = {};
  unsigned long long a = 0;
  char arg[64] = {}, arg2[64] = {};
  int n = sscanf(line, "%15s %llx %63s %63s", cmd, &a, arg, arg2);
  if (n < 2) return;
  ACO_LOG("memtool: %s", line);
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
  } else if (!strcmp(cmd, "rf") && readable(reinterpret_cast<void*>(a), 4)) {
    dump_object(a, n >= 3 ? strtoul(arg, nullptr, 0) * 4 : 16);
  } else if (!strcmp(cmd, "wf") && n >= 3 && readable(reinterpret_cast<void*>(a), 4)) {
    float v = strtof(arg, nullptr);
    memcpy(reinterpret_cast<void*>(a), &v, 4);
  } else if (!strcmp(cmd, "wi") && n >= 3 && readable(reinterpret_cast<void*>(a), 4)) {
    int32_t v = strtol(arg, nullptr, 0);
    memcpy(reinterpret_cast<void*>(a), &v, 4);
  }
}

std::atomic<uint64_t> g_draws{0}, g_indices{0}, g_indirect{0};
// Draw statistics are a debugging aid (shared atomics on every draw): on only while <out>\geo exists.
std::atomic<bool> g_geo_on{false};

} // namespace

void geo_count(unsigned long long indices, bool indirect) {
  if (!g_geo_on.load(std::memory_order_relaxed)) return;
  g_draws.fetch_add(1, std::memory_order_relaxed);
  if (indirect) g_indirect.fetch_add(1, std::memory_order_relaxed);
  else g_indices.fetch_add(indices, std::memory_order_relaxed);
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
      ACO_LOG("geo: %.0f draws/frame, %.2fM indices/frame, %.0f indirect/frame", double(g_draws.exchange(0)) / frames,
              double(g_indices.exchange(0)) / frames / 1e6, double(g_indirect.exchange(0)) / frames);
    else
      g_draws = g_indices = g_indirect = 0;
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
