#include "core/peek.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"

namespace aco {

namespace {

struct Probe {
  std::string name, type;
  uint64_t rva = 0;
  std::vector<uint64_t> offsets;  // pointer chain: each but the last is dereferenced
};

std::vector<Probe> g_probes;
int g_left = 0;
unsigned g_poll = 0;
long long g_last = 0;

bool readable(const void* p, size_t n) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
  return static_cast<const char*>(p) + n <= static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
}

std::string read_probe(const Probe& p) {
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  uint8_t* addr = base + p.rva;
  for (uint64_t off : p.offsets) {
    if (!readable(addr, 8)) return "?";
    uint64_t next;
    memcpy(&next, addr, 8);
    addr = reinterpret_cast<uint8_t*>(next) + off;
  }
  if (!readable(addr, 8)) return "?";
  char buf[64];
  if (p.type == "f32") {
    float v;
    memcpy(&v, addr, 4);
    snprintf(buf, sizeof(buf), "%.6g", v);
  } else if (p.type == "u32") {
    uint32_t v;
    memcpy(&v, addr, 4);
    snprintf(buf, sizeof(buf), "%u", v);
  } else if (p.type == "i32") {
    int32_t v;
    memcpy(&v, addr, 4);
    snprintf(buf, sizeof(buf), "%d", v);
  } else {
    uint64_t v;
    memcpy(&v, addr, 8);
    snprintf(buf, sizeof(buf), "%#llx", (unsigned long long)v);
  }
  return buf;
}

long long now_us() {
  LARGE_INTEGER c, f;
  QueryPerformanceCounter(&c);
  QueryPerformanceFrequency(&f);
  return c.QuadPart * 1000000 / f.QuadPart;
}

} // namespace

void peek_on_present() {
  if (g_left <= 0) {
    if (++g_poll % 32) return;
    std::wstring trigger = out_dir() + L"\\peek";
    FILE* f = _wfopen(trigger.c_str(), L"r");
    if (!f) return;
    g_probes.clear();
    int frames = 16;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
      if (sscanf(line, "frames=%d", &frames) == 1) continue;
      char name[64], type[16];
      char* tok = strtok(line, " \t\r\n");
      if (!tok || tok[0] == '#') continue;
      snprintf(name, sizeof(name), "%s", tok);
      std::vector<std::string> rest;
      while ((tok = strtok(nullptr, " \t\r\n"))) rest.emplace_back(tok);
      if (rest.size() < 2) continue;
      Probe p;
      p.name = name;
      snprintf(type, sizeof(type), "%s", rest.back().c_str());
      p.type = type;
      p.rva = strtoull(rest[0].c_str(), nullptr, 16);
      for (size_t i = 1; i + 1 < rest.size(); ++i) p.offsets.push_back(strtoull(rest[i].c_str(), nullptr, 16));
      g_probes.push_back(p);
    }
    fclose(f);
    DeleteFileW(trigger.c_str());
    g_left = frames;
    g_last = now_us();
    ACO_LOG("peek: %zu probes for %d frames", g_probes.size(), frames);
    return;
  }
  long long t = now_us();
  std::string s;
  for (auto& p : g_probes) s += " " + p.name + "=" + read_probe(p);
  ACO_LOG("peek: dt %.2f ms%s", (t - g_last) / 1000.0, s.c_str());
  g_last = t;
  --g_left;
}

} // namespace aco
