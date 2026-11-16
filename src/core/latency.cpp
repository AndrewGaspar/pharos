#include "core/latency.h"

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "core/config.h"
#include "core/log.h"

namespace aco {

namespace {

int64_t now_ns() {
  static const int64_t freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  // Wine: 10 MHz; avoid overflow by splitting.
  return (c.QuadPart / freq) * 1000000000LL + (c.QuadPart % freq) * 1000000000LL / freq;
}

enum Kind { kTaa, kPresent };

struct Pending {
  ID3D11Query* q = nullptr;
  Kind kind = kTaa;
  uint64_t frame = 0;
  int64_t cpu_ns = 0, cpu_ret_ns = 0;
  ID3D11Buffer* cam_buf = nullptr;  // staging copy of PassParams.viewToWorld, read once GPU-done
  uint32_t age = 0;
};

struct State {
  std::atomic<bool> active{false};
  std::mutex mutex;  // pending, pool
  std::deque<Pending> pending;
  std::vector<ID3D11Query*> pool;
  std::vector<ID3D11Buffer*> cam_pool;
  ID3D11Device* dev = nullptr;  // not owned; valid while the swapchain lives
  ID3D11DeviceContext* imm = nullptr;
  uint64_t freq = 0;
  int64_t offset_ns = 0;  // cpu_ns = gpu_ns - offset_ns
  int64_t calib_err_ns = 0;
  uint64_t frame = 0;
  FILE* taa = nullptr;
  FILE* present = nullptr;
  uint64_t dropped = 0;
} S;

ID3D11Query* get_query() {
  {
    std::lock_guard lock(S.mutex);
    if (!S.pool.empty()) {
      ID3D11Query* q = S.pool.back();
      S.pool.pop_back();
      return q;
    }
  }
  D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP, 0};
  ID3D11Query* q = nullptr;
  S.dev->CreateQuery(&d, &q);
  return q;
}

constexpr UINT kCamBytes = 48;  // 3 float4 rows

ID3D11Buffer* get_cam_buf() {
  {
    std::lock_guard lock(S.mutex);
    if (!S.cam_pool.empty()) {
      ID3D11Buffer* b = S.cam_pool.back();
      S.cam_pool.pop_back();
      return b;
    }
  }
  D3D11_BUFFER_DESC d{};
  d.ByteWidth = kCamBytes;
  d.Usage = D3D11_USAGE_STAGING;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Buffer* b = nullptr;
  S.dev->CreateBuffer(&d, nullptr, &b);
  return b;
}

int64_t gpu_ticks_to_ns(uint64_t t) {
  return static_cast<int64_t>((t / S.freq) * 1000000000ULL + (t % S.freq) * 1000000000ULL / S.freq);
}

template <typename T>
bool spin_get(ID3D11Query* q, T* out, UINT flags, int64_t timeout_ns = 200000000) {
  int64_t start = now_ns();
  while (S.imm->GetData(q, out, sizeof(T), flags) != S_OK) {
    if (now_ns() - start > timeout_ns) return false;
  }
  return true;
}

// Drains the GPU, then timestamps an otherwise empty submission. The GPU executes it somewhere in
// [t0, t1]; keep the tightest of several tries.
bool calibrate(int64_t* offset, int64_t* err) {
  D3D11_QUERY_DESC ed{D3D11_QUERY_EVENT, 0};
  D3D11_QUERY_DESC td{D3D11_QUERY_TIMESTAMP, 0};
  ID3D11Query* ev = nullptr;
  ID3D11Query* ts = nullptr;
  S.dev->CreateQuery(&ed, &ev);
  S.dev->CreateQuery(&td, &ts);
  bool ok = false;
  int64_t best = INT64_MAX;
  for (int i = 0; i < 9 && ev && ts; ++i) {
    S.imm->End(ev);
    BOOL done = FALSE;
    if (!spin_get(ev, &done, 0)) continue;
    S.imm->End(ts);
    int64_t t0 = now_ns();
    S.imm->Flush();
    uint64_t ticks = 0;
    if (!spin_get(ts, &ticks, D3D11_ASYNC_GETDATA_DONOTFLUSH)) continue;
    int64_t t1 = now_ns();
    if (t1 - t0 < best) {
      best = t1 - t0;
      *offset = gpu_ticks_to_ns(ticks) - (t0 + t1) / 2;
      *err = (t1 - t0) / 2;
      ok = true;
    }
  }
  if (ev) ev->Release();
  if (ts) ts->Release();
  return ok;
}

FILE* open_csv(const wchar_t* name, const char* header) {
  std::wstring p = out_dir() + L"\\" + name;
  FILE* f = _wfopen(p.c_str(), L"w");
  if (f) fputs(header, f);
  return f;
}

bool start(IDXGISwapChain* sc) {
  ID3D11Device* dev = nullptr;
  if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev)))) return false;
  S.dev = dev;
  dev->Release();  // the swapchain keeps it alive
  if (S.imm) S.imm->Release();
  dev->GetImmediateContext(&S.imm);

  D3D11_QUERY_DESC dd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
  ID3D11Query* dj = nullptr;
  S.dev->CreateQuery(&dd, &dj);
  D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dis{};
  if (dj) {
    S.imm->Begin(dj);
    S.imm->End(dj);
    spin_get(dj, &dis, 0);
    dj->Release();
  }
  S.freq = dis.Frequency;
  if (!S.freq) {
    ACO_LOG("latency: no timestamp frequency");
    return false;
  }
  if (!calibrate(&S.offset_ns, &S.calib_err_ns)) {
    ACO_LOG("latency: calibration failed");
    return false;
  }
  S.taa = open_csv(L"latency_taa.csv",
                   "cpu_record_ns,gpu_ns,have_cam,r00,r01,r02,tx,r10,r11,r12,ty,r20,r21,r22,tz\n");
  S.present = open_csv(L"latency_present.csv", "frame,cpu_present_ns,cpu_return_ns,gpu_ns\n");
  ACO_LOG("latency: logging on (timestamp freq %llu Hz, gpu->cpu offset %lld ns +- %lld ns)",
          (unsigned long long)S.freq, (long long)S.offset_ns, (long long)S.calib_err_ns);
  return S.taa && S.present;
}

void flush_pending(bool all);

void stop() {
  S.active.store(false);
  flush_pending(true);
  int64_t off = 0, err = 0;
  if (calibrate(&off, &err))
    ACO_LOG("latency: logging off; calibration drift %lld ns (+- %lld ns), %llu dropped", (long long)(off - S.offset_ns),
            (long long)err, (unsigned long long)S.dropped);
  if (S.taa) fclose(S.taa);
  if (S.present) fclose(S.present);
  S.taa = S.present = nullptr;
}

// Resolves finished timestamps (any order) and writes their rows.
void flush_pending(bool all) {
  std::lock_guard lock(S.mutex);
  for (auto it = S.pending.begin(); it != S.pending.end();) {
    uint64_t ticks = 0;
    HRESULT hr = S.imm->GetData(it->q, &ticks, sizeof(ticks), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    bool ready = hr == S_OK;
    if (!ready && !all && ++it->age < 1000) {
      ++it;
      continue;
    }
    if (ready) {
      int64_t gpu = gpu_ticks_to_ns(ticks) - S.offset_ns;
      if (it->kind == kPresent && S.present) {
        fprintf(S.present, "%llu,%lld,%lld,%lld\n", (unsigned long long)it->frame, (long long)it->cpu_ns,
                (long long)it->cpu_ret_ns, (long long)gpu);
      } else if (it->kind == kTaa && S.taa) {
        // The copy precedes the timestamp in the same GPU stream, so it is complete too.
        float cam[12] = {};
        bool have_cam = false;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (it->cam_buf) {
          // DXVK may not have retired the copy yet even though the timestamp after it has
          // landed (it tracks resources per submission): retry on a later frame.
          HRESULT mr = S.imm->Map(it->cam_buf, 0, D3D11_MAP_READ, all ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
          if (mr == DXGI_ERROR_WAS_STILL_DRAWING && ++it->age < 1000) {
            ++it;
            continue;
          }
          if (SUCCEEDED(mr)) {
            memcpy(cam, m.pData, sizeof(cam));
            S.imm->Unmap(it->cam_buf, 0);
            have_cam = true;
          }
        }
        fprintf(S.taa, "%lld,%lld,%d", (long long)it->cpu_ns, (long long)gpu, have_cam);
        for (float v : cam) fprintf(S.taa, ",%.7g", v);
        fputc('\n', S.taa);
      }
    } else {
      ++S.dropped;  // never executed (discarded command list) or still running at stop
    }
    S.pool.push_back(it->q);
    if (it->cam_buf) S.cam_pool.push_back(it->cam_buf);
    it = S.pending.erase(it);
  }
}

} // namespace

void latency_on_taa(ID3D11DeviceContext* ctx) {
  if (!S.active.load(std::memory_order_relaxed)) return;
  Pending p;
  p.kind = kTaa;
  p.cpu_ns = now_ns();

  // Camera of this frame: PassParams.viewToWorld (float4 rows c4..c6 of the TAA pass's cb1). Copied
  // GPU-side in the game's command stream, so no CPU shadowing of the (pooled, 2 KB) cbuffer.
  ID3D11DeviceContext1* c1 = nullptr;
  if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1)))) {
    ID3D11Buffer* cb = nullptr;
    UINT first = 0, num = 0;
    c1->PSGetConstantBuffers1(1, 1, &cb, &first, &num);
    if (cb) {
      p.cam_buf = get_cam_buf();
      if (p.cam_buf) {
        D3D11_BOX box{first * 16 + 4 * 16, 0, 0, first * 16 + 4 * 16 + kCamBytes, 1, 1};
        ctx->CopySubresourceRegion(p.cam_buf, 0, 0, 0, 0, cb, 0, &box);
      }
      cb->Release();
    }
    c1->Release();
  }

  p.q = get_query();
  if (!p.q) return;
  ctx->End(p.q);
  std::lock_guard lock(S.mutex);
  S.pending.push_back(p);
}

void latency_on_present_begin(IDXGISwapChain* sc) {
  bool want = config().latency_log;
  bool active = S.active.load();
  if (want && !active) {
    if (!start(sc)) return;
    S.active.store(true);
  } else if (!want && active) {
    stop();
    return;
  }
  if (!S.active.load()) return;

  Pending p;
  p.kind = kPresent;
  p.frame = ++S.frame;
  p.cpu_ns = now_ns();
  p.q = get_query();
  if (p.q) {
    S.imm->End(p.q);
    std::lock_guard lock(S.mutex);
    S.pending.push_back(p);
  }
  flush_pending(false);
}

void latency_on_present_end() {
  if (!S.active.load()) return;
  int64_t t = now_ns();
  std::lock_guard lock(S.mutex);
  for (auto it = S.pending.rbegin(); it != S.pending.rend(); ++it) {
    if (it->kind == kPresent) {
      if (!it->cpu_ret_ns) it->cpu_ret_ns = t;
      break;
    }
  }
}

} // namespace aco
