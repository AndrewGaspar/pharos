#include "rig/capture.h"
#include "rig/ring.h"

#include <wrl/client.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/d3d11_title.h"
#include "core/lab.h"
#include "core/log.h"

namespace aco::rig {
namespace {
using Microsoft::WRL::ComPtr;
using File = std::unique_ptr<FILE, decltype(&fclose)>;
using Clock = std::chrono::steady_clock;
struct Ray {
  uint32_t view;
  float u, v;
};
struct Pixel {
  uint32_t index;
  float u, v;
};
struct Camera {
  View local;
  std::vector<Pixel> pixels;
};
struct Session {
  std::wstring dir;
  Anchor read = nullptr;
  unsigned hz = 0, count = 0, stride = 0, shutter = 0, paced = 0, tick = 0, warm = 8;
  Pose mount{};
  Quat previous{};
  Vec previous_position{};
  float turn_limit = 0, speed_limit = 0;
  std::vector<Camera> cameras;
  std::vector<float> luminance;
  File output{nullptr, fclose};
  std::unique_ptr<RingWriter> ring;
  bool write(const void* data, size_t bytes) {
    return ring ? ring->write(data, bytes) : fwrite(data, 1, bytes, output.get()) == bytes;
  }
  Clock::time_point start;
};
Session run;
std::map<std::string, Anchor> anchors;
std::atomic<bool> requested{false};
thread_local const View* current_view = nullptr;
std::mutex color_lock;
ComPtr<ID3D11Texture2D> color, staging;
ComPtr<ID3D11RenderTargetView> clear_target;
bool finite_pose(const Pose& p) { return p.q.unit() && std::isfinite(p.p.dot(p.p)); }
bool read_pose(FILE* f, Pose& p) {
  return fscanf(f, "%f%f%f%f%f%f%f", &p.p.x, &p.p.y, &p.p.z, &p.q.v.x, &p.q.v.y, &p.q.v.z, &p.q.w) == 7 &&
         finite_pose(p);
}
void finish(const char* status) {
  requested.store(false);
  if (run.output && fclose(run.output.release()) != 0) status = "output close failed";
  std::wstring tmp = run.dir + L"\\status.tmp";
  if (File f{_wfopen(tmp.c_str(), L"w"), fclose}) fprintf(f.get(), "%s\n", status);
  MoveFileExW(tmp.c_str(), (run.dir + L"\\status").c_str(), MOVEFILE_REPLACE_EXISTING);
  ACO_LOG("rig: %s at tick %u", status, run.tick);
  std::lock_guard lock(color_lock);
  color.Reset();
  staging.Reset();
  clear_target.Reset();
  run = Session{};
}
bool configure() {
  File f{_wfopen((run.dir + L"\\config").c_str(), L"r"), fclose};
  char name[64]{}, magic[32]{};
  unsigned views = 0, pixels = 0;
  if (!f || fscanf(f.get(), "%31s%63s%u%u%u%u%u%u%u", magic, name, &run.hz, &run.count, &run.stride, &run.shutter,
                   &run.paced, &views, &pixels) != 9)
    return false;
  const std::string protocol(magic);
  bool bounded = protocol == "PHAROS_RIG_2" || protocol == "PHAROS_RIG_RING_2";
  bool streamed = protocol == "PHAROS_RIG_RING_1" || protocol == "PHAROS_RIG_RING_2";
  if (!bounded && !streamed && protocol != "PHAROS_RIG_1") return false;
  auto anchor = anchors.find(name);
  if (anchor == anchors.end() || !run.hz || run.hz > 2000 || !run.count || !run.stride || !run.shutter ||
      run.shutter > run.stride || !views || views > 32 || !pixels || pixels > 8000000 ||
      !read_pose(f.get(), run.mount))
    return false;
  run.read = anchor->second;
  run.cameras.resize(views);
  for (auto& camera : run.cameras) {
    auto& v = camera.local;
    if (!read_pose(f.get(), v.pose) || fscanf(f.get(), "%f%f", &v.sx, &v.sy) != 2 ||
        !(v.sx > 0 && v.sx < 100 && v.sy > 0 && v.sy < 100))
      return false;
  }
  int limit = fscanf(f.get(), "%f", &run.turn_limit);
  if (limit == 0 || !(run.turn_limit >= 0 && run.turn_limit <= 36000)) return false;
  if (bounded && (fscanf(f.get(), "%f", &run.speed_limit) != 1 || !std::isfinite(run.speed_limit) ||
                  run.speed_limit < 0))
    return false;
  File rays{_wfopen((run.dir + L"\\rays").c_str(), L"rb"), fclose};
  run.luminance.resize(pixels);
  for (unsigned i = 0; i < pixels; ++i) {
    Ray r;
    if (!rays || fread(&r, sizeof(r), 1, rays.get()) != 1 || r.view >= views ||
        !(r.u >= 0 && r.u <= 1 && r.v >= 0 && r.v <= 1))
      return false;
    run.cameras[r.view].pixels.push_back({i, r.u, r.v});
  }
  if (fgetc(rays.get()) != EOF) return false;
  if (streamed) {
    run.ring = RingWriter::open(run.dir + L"\\capture.ring");
    return run.ring && run.write("PHARRIG1", 8);
  }
  // CREATE_NEW reserves a session directory against accidental reuse.
  HANDLE handle = CreateFileW((run.dir + L"\\capture.bin").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_NEW, 0, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_BINARY | _O_WRONLY);
  if (fd < 0) {
    CloseHandle(handle);
    return false;
  }
  run.output.reset(_fdopen(fd, "wb"));
  if (!run.output) _close(fd);
  return run.output && fwrite("PHARRIG1", 1, 8, run.output.get()) == 8;
}
float half(uint16_t h) {
  static const auto lookup = [] {
    std::vector<float> a(65536);
    for (unsigned i = 0; i < 32768; ++i) {
      int exponent = i >> 10;
      a[i] = exponent == 31
                 ? 0.f
                 : std::ldexp(float((i & 1023) + (exponent ? 1024 : 0)), exponent ? exponent - 25 : -24);
    }
    return a;
  }();
  return lookup[h];
}
bool sample(ID3D11DeviceContext* ctx, const Camera& camera, ID3D11Texture2D* tex, ID3D11Texture2D* cpu) {
  D3D11_TEXTURE2D_DESC d{};
  tex->GetDesc(&d);
  ctx->CopyResource(cpu, tex);
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(ctx->Map(cpu, 0, D3D11_MAP_READ, 0, &m))) return false;
  auto data = static_cast<const uint8_t*>(m.pData);
  bool copied = *reinterpret_cast<const uint16_t*>(data) != 0xfbff;  // sentinel cleared before replay
  auto lum = [&](unsigned x, unsigned y) {
    auto p = reinterpret_cast<const uint16_t*>(data + y * m.RowPitch) + 4 * x;
    return .2126f * half(p[0]) + .7152f * half(p[1]) + .0722f * half(p[2]);
  };
  if (copied)
    for (const auto& pix : camera.pixels) {
      float x = std::clamp(pix.u * d.Width - .5f, 0.f, float(d.Width - 1));
      float y = std::clamp(pix.v * d.Height - .5f, 0.f, float(d.Height - 1));
      auto ix = unsigned(x), iy = unsigned(y);
      auto jx = std::min(ix + 1, d.Width - 1), jy = std::min(iy + 1, d.Height - 1);
      float a = x - ix, b = y - iy;
      run.luminance[pix.index] =
          (1 - b) * ((1 - a) * lum(ix, iy) + a * lum(jx, iy)) + b * ((1 - a) * lum(ix, jy) + a * lum(jx, jy));
    }
  ctx->Unmap(cpu, 0);
  return copied;
}
}  // namespace
void register_anchor(const char* name, Anchor reader) { anchors[name] = reader; }
bool armed() { return requested.load(std::memory_order_acquire); }
const View* replay_view() { return current_view; }
void abort_capture(const char* reason) {
  if (armed()) finish(reason);
}
void poll() {
  static unsigned counter = 0;
  if (armed() || !lab_active() || ++counter % 8) return;
  auto path = out_dir() + L"\\rig.request";
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
  File f{_wfopen(path.c_str(), L"r"), fclose};
  if (!f) return;
  char id[65]{};
  bool valid = fscanf(f.get(), "%64[a-zA-Z0-9_-]", id) == 1;
  f.reset();
  DeleteFileW(path.c_str());
  if (!valid) return;
  std::string name(id);
  run.dir = out_dir() + L"\\" + std::wstring(name.begin(), name.end());
  if (GetFileAttributesW((run.dir + L"\\status").c_str()) != INVALID_FILE_ATTRIBUTES) {
    ACO_LOG("rig: session %s has already finished", id);
    run = Session{};
    return;
  }
  if (!configure()) {
    finish("invalid configuration, existing capture, or unsupported anchor");
    return;
  }
  requested.store(true, std::memory_order_release);
  ACO_LOG("rig: armed %s, %zu views, %u ticks at %u Hz", id, run.cameras.size(), run.count, run.hz);
}
void scene_color(ID3D11DeviceContext* ctx) {
  ComPtr<ID3D11ShaderResourceView> srv;
  ctx->PSGetShaderResources(title::texture0, 1, &srv);
  if (!srv) return;
  ComPtr<ID3D11Resource> src;
  srv->GetResource(&src);
  ComPtr<ID3D11Texture2D> texture;
  if (FAILED(src.As(&texture))) return;
  D3D11_TEXTURE2D_DESC d{};
  texture->GetDesc(&d);
  if (d.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || d.SampleDesc.Count != 1 || d.MipLevels != 1 ||
      d.ArraySize != 1)
    return;
  std::lock_guard lock(color_lock);
  if (!armed()) return;
  if (!color) {
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    d.MiscFlags = 0;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &color))) return;
    if (FAILED(dev->CreateRenderTargetView(color.Get(), nullptr, &clear_target))) {
      color.Reset();
      return;
    }
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging))) {
      color.Reset();
      clear_target.Reset();
      return;
    }
  }
  D3D11_TEXTURE2D_DESC stored{};
  color->GetDesc(&stored);
  if (stored.Width == d.Width && stored.Height == d.Height) ctx->CopyResource(color.Get(), texture.Get());
}
void capture(ID3D11DeviceContext* ctx, const std::function<void()>& replay, const char* unavailable) {
  if (run.ring && run.ring->cancelled()) {
    finish("stream reader disconnected");
    return;
  }
  if (run.warm) {
    --run.warm;
    return;
  }
  Pose anchor;
  if (!run.read(anchor)) {
    finish("face anchor unavailable");
    return;
  }
  if (run.tick && run.turn_limit)
    anchor.q = limit_turn(run.previous, anchor.q, run.turn_limit * 0.01745329252f / run.hz);
  run.previous = anchor.q;
  auto body = anchor * run.mount;
  if (run.tick && run.speed_limit > 0)
    body.p = limit_translation(run.previous_position, body.p, run.speed_limit / run.hz);
  run.previous_position = body.p;
  if (!run.tick) run.start = Clock::now();
  bool image = run.tick % run.stride < run.shutter;
  if (image) {
    if (unavailable) {
      finish(unavailable);
      return;
    }
    ComPtr<ID3D11Texture2D> tex, cpu;
    ComPtr<ID3D11RenderTargetView> target;
    {
      std::lock_guard lock(color_lock);
      tex = color;
      cpu = staging;
      target = clear_target;
    }
    if (!tex || !cpu || !target) {
      finish("HDR scene unavailable");
      return;
    }
    for (const auto& cam : run.cameras) {
      View v = cam.local;
      v.pose = body * v.pose;
      const float clear[4] = {-65504, -65504, -65504, -65504};
      ctx->ClearRenderTargetView(target.Get(), clear);
      current_view = &v;
      replay();
      current_view = nullptr;
      if (!sample(ctx, cam, tex.Get(), cpu.Get())) {
        finish("HDR pass missing from replay");
        return;
      }
    }
  }
  static_assert(sizeof(body) == 28);
  if (!run.write(&run.tick, 4) || !run.write(&body, sizeof(body)) ||
      (image && !run.write(run.luminance.data(), 4 * run.luminance.size())) ||
      (run.output && fflush(run.output.get()) != 0)) {
    finish("capture write failed");
    return;
  }
  if (++run.tick == run.count) {
    finish("ok");
    return;
  }
  if (run.paced) {
    auto until = run.start + std::chrono::microseconds(1000000LL * run.tick / run.hz);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
    if (ms > 0) Sleep(static_cast<DWORD>(ms));
  }
}
}  // namespace aco::rig
