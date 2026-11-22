#include "core/startup_splash.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <thread>
#include <vector>

#include "core/hyprland.h"
#include "core/log.h"
#include "core/startup_splash_resource.h"

namespace aco {
namespace {

struct StartupWindows {
  HWND artwork = nullptr;
  SIZE size{};
  bool main_visible = false;
};

StartupWindows visible_game_windows() {
  StartupWindows result;
  EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
    auto& result = *reinterpret_cast<StartupWindows*>(param);
    DWORD pid{};
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd)) return TRUE;
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) return TRUE;
    const SIZE size{rect.right - rect.left, rect.bottom - rect.top};
    // Anvil can also create a visible 0x0 HWND with the splash class and the game's title.
    if (size.cx < 2 || size.cy < 1) return TRUE;
    wchar_t name[64]{};
    if (!GetClassNameW(hwnd, name, 64)) return TRUE;
    if (!wcscmp(name, L"ScimitarSplashScreenWindow") && (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED)) {
      result.artwork = hwnd;
      result.size = size;
      return FALSE;
    }
    if (!wcscmp(name, L"ScimitarEngineWindowClass")) result.main_visible = true;
    return TRUE;
  }, reinterpret_cast<LPARAM>(&result));
  return result;
}

// Resample each eye independently. Windows layered surfaces use premultiplied BGRA,
// while the artwork stores straight BGRA, including a soft, translucent window border.
void pack_translucent_sbs(const BYTE* pixels, int eye_width, int height, SIZE size, bool swapped, BYTE* dst) {
  auto coordinate = [](int index, int source_size, int dest_size) {
    const auto value = (std::int64_t(index) * 2 + 1) * source_size * 128 / dest_size - 128;
    return static_cast<int>(std::clamp<std::int64_t>(value, 0, std::int64_t(source_size - 1) * 256));
  };
  const size_t pitch = size_t(eye_width) * 2 * 4;
  for (int y = 0; y < size.cy; ++y) {
    const int sy = coordinate(y, height, size.cy);
    const BYTE* top = pixels + size_t(height - 1 - sy / 256) * pitch; // bottom-up resource
    const BYTE* bottom = sy / 256 + 1 < height ? top - pitch : top;
    const int fy = sy % 256;
    for (int eye = 0; eye < 2; ++eye) {
      const int out_width = eye == 0 ? size.cx / 2 : size.cx - size.cx / 2;
      const int origin = (swapped ? 1 - eye : eye) * eye_width;
      for (int x = 0; x < out_width; ++x, dst += 4) {
        const int sx = coordinate(x, eye_width, out_width);
        const int left = (origin + sx / 256) * 4;
        const int right = sx / 256 + 1 < eye_width ? left + 4 : left;
        const int fx = sx % 256;
        const BYTE* samples[] = {top + left, top + right, bottom + left, bottom + right};
        const int weights[] = {(256 - fx) * (256 - fy), fx * (256 - fy), (256 - fx) * fy, fx * fy};
        // Multiply by alpha before filtering. Invisible RGB never bleeds into a visible edge.
        int alpha = 0;
        std::int64_t rgb[3]{};
        for (int i = 0; i < 4; ++i) {
          const int coverage = weights[i] * samples[i][3];
          alpha += coverage;
          for (int c = 0; c < 3; ++c) rgb[c] += std::int64_t(coverage) * samples[i][c];
        }
        constexpr int rgb_scale = 256 * 256 * 255;
        for (int c = 0; c < 3; ++c) dst[c] = static_cast<BYTE>((rgb[c] + rgb_scale / 2) / rgb_scale);
        dst[3] = static_cast<BYTE>((alpha + 32768) / 65536);
      }
    }
  }
}

// The stock window is one eye wide. Half-SBS then uses two source texels per displayed
// pixel. Wine's StretchDIBits can drop the odd columns even with HALFTONE selected.
void pack_half_sbs(const BYTE* pixels, int image_width, SIZE size, bool swapped, BYTE* dst) {
  const int eye_width = image_width / 2;
  const int row_bytes = (3 * image_width + 3) & ~3;
  for (int y = 0; y < size.cy; ++y) {
    const BYTE* row = pixels + size_t(size.cy - 1 - y) * row_bytes;  // bottom-up BMP
    for (int eye = 0; eye < 2; ++eye) {
      const BYTE* src = row + (swapped ? 1 - eye : eye) * eye_width * 3;
      for (int x = 0; x < size.cx / 2; ++x, src += 6, dst += 4) {
        for (int c = 0; c < 3; ++c) dst[c] = static_cast<BYTE>((src[c] + src[c+3] + 1) / 2);
        dst[3] = 255;
      }
    }
  }
}

// Keep one surface at the native window size. Xwayland can take several polls to publish the
// HWND and confirm the stereo rule. Resample once, *before* asking it to split the window.
struct SplashSurface {
  struct ClearRun { size_t first, count; };
  HDC dc = nullptr;
  HBITMAP bitmap = nullptr;
  HGDIOBJ previous = nullptr;
  SIZE extent{};
  std::uint32_t* argb = nullptr;
  std::vector<ClearRun> clear_runs;

  void reset() {
    if (previous && previous != HGDI_ERROR) SelectObject(dc, previous);
    if (bitmap) DeleteObject(bitmap);
    if (dc) DeleteDC(dc);
    dc = nullptr;
    bitmap = nullptr;
    previous = nullptr;
    extent = {};
    argb = nullptr;
    clear_runs.clear();
  }
  ~SplashSurface() { reset(); }

  bool prepare(SIZE size, const BITMAPINFO* artwork, bool swapped) {
    if (size.cx == extent.cx && size.cy == extent.cy) return true;
    reset();
    dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    BITMAPINFO buffer{};
    buffer.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    buffer.bmiHeader.biWidth = size.cx;
    buffer.bmiHeader.biHeight = -size.cy;
    buffer.bmiHeader.biPlanes = 1;
    buffer.bmiHeader.biBitCount = 32;
    buffer.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    bitmap = CreateDIBSection(dc, &buffer, DIB_RGB_COLORS, &bits, nullptr, 0);
    previous = bitmap ? SelectObject(dc, bitmap) : nullptr;
    if (!previous || previous == HGDI_ERROR) return false;
    const auto& header = artwork->bmiHeader;
    const auto* pixels = reinterpret_cast<const BYTE*>(artwork) + header.biSize;
    if (header.biBitCount == 32) {
      pack_translucent_sbs(pixels, header.biWidth / 2, header.biHeight, size, swapped, static_cast<BYTE*>(bits));
      if (GetModuleHandleW(L"winex11.drv")) {
        argb = static_cast<std::uint32_t*>(bits);
        const size_t count = size_t(size.cx) * size.cy;
        for (size_t i = 0; i < count;) {
          if (argb[i]) { ++i; continue; }
          const size_t first = i;
          while (i < count && argb[i] == 0) ++i;
          clear_runs.push_back({first, i - first});
        }
      }
      extent = size;
      return true;
    }
    if (size.cx == header.biWidth / 2 && size.cx % 2 == 0 && size.cy == header.biHeight) {
      pack_half_sbs(pixels, header.biWidth, size, swapped, static_cast<BYTE*>(bits));
      extent = size;
      return true;
    }
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    const int eye_width = header.biWidth / 2;
    const int middle = size.cx / 2;
    for (int eye = 0; eye < 2; ++eye) {
      const int src_x = (swapped ? 1 - eye : eye) * eye_width;
      if (StretchDIBits(dc, eye ? middle : 0, 0, eye ? size.cx - middle : middle, size.cy,
                        src_x, 0, eye_width, header.biHeight, pixels, artwork, DIB_RGB_COLORS, SRCCOPY) <= 0)
        return false;
    }
    GdiFlush();
    // GDI expands our 24-bit art into BGRX with undefined alpha. Wine can forward those
    // bytes into the compositor's ARGB surface even for ULW_OPAQUE, so make it fully opaque.
    auto* rgba = static_cast<std::uint32_t*>(bits);
    const auto count = static_cast<size_t>(size.cx) * size.cy;
    for (size_t i = 0; i < count; ++i) rgba[i] |= 0xff000000u;
    extent = size;
    return true;
  }

  bool publish(HWND window, bool translucent) {
    if (!translucent)
      return UpdateLayeredWindow(window, nullptr, nullptr, nullptr, dc, nullptr, 0, nullptr, ULW_OPAQUE);
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    auto update = [&] { return UpdateLayeredWindow(window, nullptr, nullptr, nullptr, dc, nullptr, 0, &blend, ULW_ALPHA); };
    bool primed = false;
    if (!clear_runs.empty()) {
      // Wine's X11 driver shapes the window before uploading its new pixels. Previously
      // opaque artwork would survive in the compositor's backing pixmap under our new
      // transparent margins. Include them in one upload before publishing exact alpha.
      for (const auto& run : clear_runs) std::fill_n(argb + run.first, run.count, 0x01000000u);
      primed = update();
      for (const auto& run : clear_runs) std::fill_n(argb + run.first, run.count, 0u);
      if (!primed) return false;
    }
    // Once the first upload succeeds, the HWND contains the stereo pair, even if it
    // closes during the second upload. Keep the matching compositor tag in that case.
    return update() || primed;
  }
};

void show_stereo(HINSTANCE module, bool forced, bool swapped) {
  if (!forced && !hypr_available()) return;
  auto resource = FindResourceW(module, MAKEINTRESOURCEW(IDB_STARTUP_SPLASH_STEREO), RT_BITMAP);
  if (!resource) {
    ACO_LOG("startup splash: stereo artwork resource unavailable (%lu)", GetLastError());
    return;
  }
  auto* image = static_cast<const BITMAPINFO*>(LockResource(LoadResource(module, resource)));
  if (!image || image->bmiHeader.biSize != sizeof(BITMAPINFOHEADER) ||
      (image->bmiHeader.biBitCount != 24 && image->bmiHeader.biBitCount != 32) ||
      image->bmiHeader.biCompression != BI_RGB || image->bmiHeader.biClrUsed || image->bmiHeader.biHeight < 1 ||
      image->bmiHeader.biWidth < 2 || image->bmiHeader.biWidth % 2) {
    ACO_LOG("startup splash: invalid bitmap resource");
    return;
  }
  ACO_LOG("startup splash: looking for the artwork window (%s)", forced ? "forced stereo" : "auto stereo");

  // The splash is optional in the engine. Stop looking once the main window is up, even
  // if a later game build registers a different splash class. This worker never gates startup.
  const ULONGLONG deadline = GetTickCount64() + 120000;
  SplashSurface surface;
  HWND seen = nullptr;
  SIZE drawn{};
  ULONGLONG first_visible = 0;
  ULONGLONG main_since = 0;
  bool retain_native = false;
  for (; GetTickCount64() < deadline; Sleep(seen ? 100 : 30)) {
    const auto windows = visible_game_windows();
    HWND window = windows.artwork;
    if (!window) {
      seen = nullptr;
      if (!windows.main_visible) main_since = 0;
      else if (!main_since) main_since = GetTickCount64();
      else if (GetTickCount64() - main_since > 300) return;
      continue;
    }
    main_since = 0;
    if (seen != window) {
      seen = window;
      first_visible = GetTickCount64();
      retain_native = false;
      drawn = {};
      ACO_LOG("startup splash: native artwork window %p (%ldx%ld)", window, windows.size.cx, windows.size.cy);
    }
    const SIZE size = windows.size;
    if (retain_native || (size.cx == drawn.cx && size.cy == drawn.cy)) continue;
    if (!surface.prepare(size, image, swapped)) {
      ACO_LOG("startup splash: could not prepare the stereo artwork");
      retain_native = true;
      continue;
    }
    bool added_tag = false;
    if (!hypr_prepare_splash_stereo(window, forced, &added_tag)) {
      // Xwayland can publish its window a moment after the Win32 show. On a confirmed
      // mono or missing output, leave the native art up instead of polling until handoff.
      if (GetTickCount64() - first_visible > 1500) {
        ACO_LOG("startup splash: retaining native artwork for this display");
        retain_native = true;
      }
      continue;
    }
    // Publish immediately after the compositor acknowledges the tag. Null position and
    // size preserve the geometry of the game's own layered window.
    if (!surface.publish(window, image->bmiHeader.biBitCount == 32)) {
      if (added_tag) hypr_clear_splash_stereo(window);
      if (GetTickCount64() - first_visible > 1500) {
        ACO_LOG("startup splash: layered window update failed");
        retain_native = true;
      }
      continue;
    }
    drawn = size;
    ACO_LOG("startup splash: painted stereo artwork %ldx%ld, half side by side%s",
            size.cx, size.cy, swapped ? ", eyes swapped" : "");
  }
}
} // namespace

void start_stereo_splash(HINSTANCE module, bool forced, bool swapped) {
  std::thread(show_stereo, module, forced, swapped).detach();
}

} // namespace aco
