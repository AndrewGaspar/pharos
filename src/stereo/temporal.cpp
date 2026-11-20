#include "stereo/temporal.h"

#include <windows.h>

#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/log.h"
#include "stereo/record.h"

namespace aco {

std::atomic<bool> g_tmp_learning{false};

namespace {

// The twin copies go through our own context hooks (which report accesses while learning and take
// g_mutex): they run without the lock and are not reported.
thread_local int g_internal = 0;

constexpr int kLearnFrames = 4;  // frames observed per learning round

struct Access {
  ID3D11Resource* res;
  bool write;
};
using AccessList = std::vector<Access>;

std::mutex g_mutex;
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_imm = nullptr;
std::unordered_map<ID3D11DeviceContext*, AccessList> g_ctx_lists;  // recording, per deferred context
std::unordered_map<ID3D11CommandList*, AccessList> g_cl_lists;      // finished, not yet executed
AccessList g_frame;                                                 // this frame, execution order

// Learning state.
int g_learn_frame = 0;  // 0 = not learning; 1..kLearnFrames
std::unordered_set<ID3D11Resource*> g_written_prev;
std::unordered_set<ID3D11Resource*> g_found;

// Per-eye twins.
struct Twin {
  ID3D11Resource* res = nullptr;  // the game's (we hold a reference)
  ID3D11Resource* eye[2] = {};    // saved state of each eye
  UINT64 bytes = 0;
};
std::vector<Twin> g_twins;
bool g_right_seeded = false;
bool g_learned = false;  // a first learning round was started
size_t g_learned_size = 0;  // frame size when the last round started
int g_changed_frames = 0;
uint64_t g_frames = 0;

void add(AccessList& l, ID3D11Resource* r, bool w) {
  if (r) l.push_back({r, w});
}

void add_view(AccessList& l, ID3D11View* v, bool w) {
  if (!v) return;
  ID3D11Resource* r = nullptr;
  v->GetResource(&r);
  if (r) {
    l.push_back({r, w});
    r->Release();  // identity only; the game keeps it alive for the frame
  }
}

AccessList& list_for(ID3D11DeviceContext* ctx) {
  if (ctx == g_imm) return g_frame;
  return g_ctx_lists[ctx];
}

template <typename T, typename Get>
void add_views(AccessList& l, UINT n, bool w, Get get) {
  T* v[128] = {};
  get(v);
  for (UINT i = 0; i < n; ++i)
    if (v[i]) {
      add_view(l, v[i], w);
      v[i]->Release();
    }
}

// Per-eye state only for resources laid out like the screen (render resolution and its fractions,
// with the screen's aspect ratio), small buffers and 1x1 values. Square textures, arrays and volumes
// with square slices hold world-space data (GI volume, cached shadow cascades, wind/cloud maps): the
// same for both eyes, so they are shared.
float g_aspect = 16.0f / 10.0f;
bool screen_like(UINT w, UINT h) {
  if (w <= 2 && h <= 2) return true;
  float a = float(w) / float(h);
  return std::fabs(a / g_aspect - 1.0f) < 0.06f;
}

bool usable(ID3D11Resource* r, UINT64* bytes) {
  D3D11_RESOURCE_DIMENSION dim;
  r->GetType(&dim);
  switch (dim) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: {
      D3D11_BUFFER_DESC d;
      static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
      *bytes = d.ByteWidth;
      return d.Usage == D3D11_USAGE_DEFAULT && !(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && d.ByteWidth <= (1u << 20);
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
      D3D11_TEXTURE2D_DESC d;
      static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
      *bytes = UINT64(d.Width) * d.Height * d.ArraySize * 8;  // estimate
      return d.Usage == D3D11_USAGE_DEFAULT && d.SampleDesc.Count == 1 && d.ArraySize == 1 && screen_like(d.Width, d.Height);
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
      D3D11_TEXTURE3D_DESC d;
      static_cast<ID3D11Texture3D*>(r)->GetDesc(&d);
      *bytes = UINT64(d.Width) * d.Height * d.Depth * 8;
      return d.Usage == D3D11_USAGE_DEFAULT && screen_like(d.Width, d.Height);  // froxel grids, not world volumes
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
      D3D11_TEXTURE1D_DESC d;
      static_cast<ID3D11Texture1D*>(r)->GetDesc(&d);
      *bytes = UINT64(d.Width) * d.ArraySize * 8;
      return d.Usage == D3D11_USAGE_DEFAULT;
    }
    default: return false;
  }
}

// A resource like `r` for holding a copy of it (same description, no CPU access or sharing).
ID3D11Resource* make_twin(ID3D11Resource* r) {
  constexpr UINT kDropMisc = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
                             D3D11_RESOURCE_MISC_GDI_COMPATIBLE | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
  D3D11_RESOURCE_DIMENSION dim;
  r->GetType(&dim);
  HRESULT hr = E_FAIL;
  switch (dim) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: {
      D3D11_BUFFER_DESC d;
      static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
      d.CPUAccessFlags = 0;
      d.MiscFlags &= ~kDropMisc;
      ID3D11Buffer* b = nullptr;
      hr = g_dev->CreateBuffer(&d, nullptr, &b);
      if (SUCCEEDED(hr)) return b;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
      D3D11_TEXTURE2D_DESC d;
      static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
      d.CPUAccessFlags = 0;
      d.MiscFlags &= ~kDropMisc;
      ID3D11Texture2D* t = nullptr;
      hr = g_dev->CreateTexture2D(&d, nullptr, &t);
      if (SUCCEEDED(hr)) return t;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
      D3D11_TEXTURE3D_DESC d;
      static_cast<ID3D11Texture3D*>(r)->GetDesc(&d);
      d.CPUAccessFlags = 0;
      d.MiscFlags &= ~kDropMisc;
      ID3D11Texture3D* t = nullptr;
      hr = g_dev->CreateTexture3D(&d, nullptr, &t);
      if (SUCCEEDED(hr)) return t;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
      D3D11_TEXTURE1D_DESC d;
      static_cast<ID3D11Texture1D*>(r)->GetDesc(&d);
      d.CPUAccessFlags = 0;
      d.MiscFlags &= ~kDropMisc;
      ID3D11Texture1D* t = nullptr;
      hr = g_dev->CreateTexture1D(&d, nullptr, &t);
      if (SUCCEEDED(hr)) return t;
      break;
    }
    default: break;
  }
  ACO_LOG("stereo: twin for %p failed 0x%08lx", static_cast<void*>(r), hr);
  return nullptr;
}

void drop_twins() {
  for (auto& t : g_twins) {
    for (auto*& e : t.eye)
      if (e) e->Release();
    t.res->Release();
  }
  g_twins.clear();
  g_right_seeded = false;
}

}  // namespace

void tmp_init(ID3D11Device* dev, ID3D11DeviceContext* imm) {
  std::lock_guard lock(g_mutex);
  if (g_dev == dev) return;
  drop_twins();
  g_dev = dev;
  g_imm = imm;
}

void tmp_on_draw(ID3D11DeviceContext* ctx) {
  if (!g_tmp_learning.load(std::memory_order_relaxed)) return;
  if (g_rec_replaying || g_internal) return;  // the replay repeats what was already seen; our own copies
  std::lock_guard lock(g_mutex);
  AccessList& l = list_for(ctx);
  add_views<ID3D11ShaderResourceView>(l, 128, false, [&](ID3D11ShaderResourceView** v) { ctx->PSGetShaderResources(0, 128, v); });
  ID3D11RenderTargetView* rtv[8] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11UnorderedAccessView* uav[8] = {};
  ctx->OMGetRenderTargetsAndUnorderedAccessViews(8, rtv, &dsv, 0, 8, uav);
  for (auto* v : rtv)
    if (v) {
      add_view(l, v, true);
      v->Release();
    }
  if (dsv) {
    add_view(l, dsv, true);
    dsv->Release();
  }
  for (auto* v : uav)
    if (v) {
      add_view(l, v, true);
      v->Release();
    }
}

void tmp_on_dispatch(ID3D11DeviceContext* ctx) {
  if (!g_tmp_learning.load(std::memory_order_relaxed)) return;
  if (g_rec_replaying || g_internal) return;  // the replay repeats what was already seen; our own copies
  std::lock_guard lock(g_mutex);
  AccessList& l = list_for(ctx);
  add_views<ID3D11ShaderResourceView>(l, 128, false, [&](ID3D11ShaderResourceView** v) { ctx->CSGetShaderResources(0, 128, v); });
  add_views<ID3D11UnorderedAccessView>(l, 8, true, [&](ID3D11UnorderedAccessView** v) { ctx->CSGetUnorderedAccessViews(0, 8, v); });
}

void tmp_on_copy(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src) {
  if (!g_tmp_learning.load(std::memory_order_relaxed)) return;
  if (g_rec_replaying || g_internal) return;  // the replay repeats what was already seen; our own copies
  std::lock_guard lock(g_mutex);
  AccessList& l = list_for(ctx);
  add(l, src, false);
  add(l, dst, true);
}

void tmp_on_clear(ID3D11DeviceContext* ctx, ID3D11View* v) {
  if (!g_tmp_learning.load(std::memory_order_relaxed)) return;
  if (g_rec_replaying || g_internal) return;  // the replay repeats what was already seen; our own copies
  std::lock_guard lock(g_mutex);
  add_view(list_for(ctx), v, true);
}

void tmp_on_finish_command_list(ID3D11DeviceContext* ctx, ID3D11CommandList* cl) {
  std::lock_guard lock(g_mutex);
  auto it = g_ctx_lists.find(ctx);
  if (it == g_ctx_lists.end()) return;
  if (cl && g_learn_frame) g_cl_lists[cl] = std::move(it->second);
  g_ctx_lists.erase(it);
}

void tmp_on_execute(ID3D11DeviceContext* imm, ID3D11CommandList* cl) {
  (void)imm;
  std::lock_guard lock(g_mutex);
  auto it = g_cl_lists.find(cl);
  if (it == g_cl_lists.end()) return;
  g_frame.insert(g_frame.end(), it->second.begin(), it->second.end());
  g_cl_lists.erase(it);
}

void tmp_relearn() {
  std::lock_guard lock(g_mutex);
  if (g_learn_frame) return;
  g_learn_frame = 1;
  g_written_prev.clear();
  g_found.clear();
  g_frame.clear();
  g_ctx_lists.clear();
  g_cl_lists.clear();
  g_tmp_learning.store(true);
}

void tmp_frame_end(bool stereo_on, ID3D11Resource* backbuffer, size_t frame_size) {
  std::lock_guard lock(g_mutex);
  if (backbuffer) {
    D3D11_TEXTURE2D_DESC d;
    static_cast<ID3D11Texture2D*>(backbuffer)->GetDesc(&d);
    if (d.Height) g_aspect = float(d.Width) / float(d.Height);
  }
  ++g_frames;
  if (!stereo_on) {
    if (!g_twins.empty()) drop_twins();
    g_learned = false;
    g_learn_frame = 0;
    g_tmp_learning.store(false);
    g_frame.clear();
    return;
  }
  if (!g_learn_frame) {
    // Learn at start, and again when the frame's shape changes for good (title screen -> game
    // world, another area): measured by the number of immediate-context calls recorded.
    bool changed = false;
    if (g_learned && g_learned_size) {
      double dev = std::fabs(double(frame_size) - double(g_learned_size)) / double(g_learned_size);
      g_changed_frames = dev > 0.25 ? g_changed_frames + 1 : 0;
      changed = g_changed_frames >= 90;
    }
    if ((!g_learned && g_frames > 2) || changed) {
      g_learned = true;
      g_learned_size = frame_size;
      g_changed_frames = 0;
      g_learn_frame = 1;  // start learning
      g_tmp_learning.store(true);
    }
    g_frame.clear();
    return;
  }
  // A learning frame just ended: first access per resource.
  static LARGE_INTEGER t_last{};
  LARGE_INTEGER t_now, fq;
  QueryPerformanceCounter(&t_now);
  QueryPerformanceFrequency(&fq);
  if (t_last.QuadPart)
    ACO_LOG("stereo: learning frame %d: %zu accesses, %.1f ms", g_learn_frame, g_frame.size(),
            1000.0 * double(t_now.QuadPart - t_last.QuadPart) / double(fq.QuadPart));
  t_last = t_now;
  std::unordered_map<ID3D11Resource*, bool> first_write;
  std::unordered_set<ID3D11Resource*> written;
  for (const Access& a : g_frame) {
    first_write.emplace(a.res, a.write);
    if (a.write) written.insert(a.res);
  }
  if (g_learn_frame > 1)
    for (auto& [r, w] : first_write)
      if (!w && r != backbuffer && (written.count(r) || g_written_prev.count(r))) g_found.insert(r);
  g_written_prev = std::move(written);
  g_frame.clear();
  if (++g_learn_frame <= kLearnFrames) return;

  // Done: (re)build the twins.
  g_learn_frame = 0;
  g_tmp_learning.store(false);
  g_cl_lists.clear();
  g_ctx_lists.clear();
  drop_twins();
  UINT64 total = 0;
  for (ID3D11Resource* r : g_found) {
    UINT64 bytes = 0;
    if (!usable(r, &bytes)) {
      D3D11_RESOURCE_DIMENSION dim;
      r->GetType(&dim);
      if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
        D3D11_TEXTURE3D_DESC d;
        static_cast<ID3D11Texture3D*>(r)->GetDesc(&d);
        ACO_LOG("stereo:   shared: tex3d %ux%ux%u fmt %u", d.Width, d.Height, d.Depth, d.Format);
      } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d;
        static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
        ACO_LOG("stereo:   shared: tex2d %ux%u x%u fmt %u", d.Width, d.Height, d.ArraySize, d.Format);
      } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d;
        static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
        ACO_LOG("stereo:   shared: buffer %u B", d.ByteWidth);
      }
      continue;
    }
    Twin t;
    t.res = r;
    r->AddRef();
    t.eye[0] = make_twin(r);
    t.eye[1] = make_twin(r);
    t.bytes = bytes;
    if (!t.eye[0] || !t.eye[1]) {
      for (auto*& e : t.eye)
        if (e) e->Release();
      r->Release();
      continue;
    }
    total += bytes;
    g_twins.push_back(t);
  }
  ACO_LOG("stereo: %zu temporal resources found, %zu with per-eye copies (~%.1f MB each eye)", g_found.size(),
          g_twins.size(), total / 1048576.0);
  for (auto& t : g_twins) {
    D3D11_RESOURCE_DIMENSION dim;
    t.res->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
      D3D11_TEXTURE2D_DESC d;
      static_cast<ID3D11Texture2D*>(t.res)->GetDesc(&d);
      ACO_LOG("stereo:   tex2d %ux%u x%u fmt %u bind 0x%x", d.Width, d.Height, d.ArraySize, d.Format, d.BindFlags);
    } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
      D3D11_BUFFER_DESC d;
      static_cast<ID3D11Buffer*>(t.res)->GetDesc(&d);
      ACO_LOG("stereo:   buffer %u B bind 0x%x misc 0x%x", d.ByteWidth, d.BindFlags, d.MiscFlags);
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
      D3D11_TEXTURE3D_DESC d;
      static_cast<ID3D11Texture3D*>(t.res)->GetDesc(&d);
      ACO_LOG("stereo:   tex3d %ux%ux%u fmt %u bind 0x%x", d.Width, d.Height, d.Depth, d.Format, d.BindFlags);
    }
  }
  g_found.clear();
}

void tmp_before_right(ID3D11DeviceContext* imm) {
  std::vector<Twin> twins;
  bool seeded;
  {
    std::lock_guard lock(g_mutex);
    twins = g_twins;  // render thread: the twins are only rebuilt from tmp_frame_end on this thread
    seeded = g_right_seeded;
    g_right_seeded = !g_twins.empty();
  }
  ++g_internal;
  for (auto& t : twins) {
    imm->CopyResource(t.eye[0], t.res);
    // First time: the right eye starts from the left eye's state.
    if (seeded) imm->CopyResource(t.res, t.eye[1]);
  }
  --g_internal;
}

void tmp_after_right(ID3D11DeviceContext* imm) {
  std::vector<Twin> twins;
  {
    std::lock_guard lock(g_mutex);
    twins = g_twins;
  }
  ++g_internal;
  for (auto& t : twins) {
    imm->CopyResource(t.eye[1], t.res);
    imm->CopyResource(t.res, t.eye[0]);
  }
  --g_internal;
}

} // namespace aco
