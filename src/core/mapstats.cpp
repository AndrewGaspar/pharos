#include "core/mapstats.h"

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

#include "core/log.h"

namespace aco {

namespace {

std::atomic<int> g_left{0};
std::mutex g_mutex;
// key: (bind flags << 32) | byte width; value: (map count, update count)
std::map<uint64_t, std::pair<uint32_t, uint32_t>> g_frame;
unsigned g_poll = 0;
long long g_last_ns = 0;

long long now_ns() {
  LARGE_INTEGER c, f;
  QueryPerformanceCounter(&c);
  QueryPerformanceFrequency(&f);
  return c.QuadPart / f.QuadPart * 1000000000LL + c.QuadPart % f.QuadPart * 1000000000LL / f.QuadPart;
}

} // namespace

void mapstats_note(ID3D11Resource* res, bool update_subresource) {
  if (g_left.load(std::memory_order_relaxed) <= 0 || !res) return;
  D3D11_RESOURCE_DIMENSION dim;
  res->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return;
  D3D11_BUFFER_DESC d;
  static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
  if (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) return;
  uint64_t key = (uint64_t(d.BindFlags) << 32) | d.ByteWidth;
  std::lock_guard lock(g_mutex);
  auto& v = g_frame[key];
  if (update_subresource) ++v.second;
  else ++v.first;
}

void mapstats_on_present() {
  int left = g_left.load();
  if (left <= 0) {
    if (++g_poll % 32) return;
    std::wstring trigger = out_dir() + L"\\mapstats";
    if (GetFileAttributesW(trigger.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    int n = 32;
    if (FILE* f = _wfopen(trigger.c_str(), L"r")) {
      if (fscanf(f, "%d", &n) != 1) n = 32;
      fclose(f);
    }
    DeleteFileW(trigger.c_str());
    std::lock_guard lock(g_mutex);
    g_frame.clear();
    g_last_ns = now_ns();
    g_left.store(n < 1 ? 1 : (n > 1000 ? 1000 : n));
    ACO_LOG("mapstats: %d frames", g_left.load());
    return;
  }
  long long t = now_ns();
  std::string line;
  {
    std::lock_guard lock(g_mutex);
    char buf[96];
    for (auto& [key, v] : g_frame) {
      snprintf(buf, sizeof(buf), " b%llx/%llu:%u+%u", (unsigned long long)(key >> 32),
               (unsigned long long)(key & 0xffffffff), v.first, v.second);
      line += buf;
    }
    g_frame.clear();
  }
  ACO_LOG("mapstats: dt %.2f ms%s", (t - g_last_ns) / 1e6, line.c_str());
  g_last_ns = t;
  g_left.fetch_sub(1);
}

} // namespace aco
