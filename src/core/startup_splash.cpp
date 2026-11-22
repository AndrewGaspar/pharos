#include "core/startup_splash.h"

#include <windows.h>

#include <cstdint>
#include <thread>

#include "core/config.h"
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
    // Origins also creates a visible 0x0 HWND with the splash class and the game's title.
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
  HDC dc = nullptr;
  HBITMAP bitmap = nullptr;
  HGDIOBJ previous = nullptr;
  SIZE extent{};

  void reset() {
    if (previous && previous != HGDI_ERROR) SelectObject(dc, previous);
    if (bitmap) DeleteObject(bitmap);
    if (dc) DeleteDC(dc);
    dc = nullptr;
    bitmap = nullptr;
    previous = nullptr;
    extent = {};
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
};

void show_stereo(HINSTANCE module, bool forced, bool swapped) {
  if (!forced && !hypr_available()) return;
  auto resource = FindResourceW(module, MAKEINTRESOURCEW(IDB_ORIGINS_SPLASH_STEREO), RT_BITMAP);
  if (!resource) {
    ACO_LOG("startup splash: stereo artwork resource unavailable (%lu)", GetLastError());
    return;
  }
  auto* image = static_cast<const BITMAPINFO*>(LockResource(LoadResource(module, resource)));
  if (!image || image->bmiHeader.biSize != sizeof(BITMAPINFOHEADER) || image->bmiHeader.biBitCount != 24 ||
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
    // size preserve the geometry of Origins' own layered window.
    if (!UpdateLayeredWindow(window, nullptr, nullptr, nullptr, surface.dc, nullptr, 0, nullptr, ULW_OPAQUE)) {
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

void configure_startup_splash(HINSTANCE module) {
  const auto& cfg = config();
  if (cfg.stereo_mode != "sbs" && cfg.stereo_mode != "auto") return;
  std::thread(show_stereo, module, cfg.stereo_mode == "sbs", cfg.stereo_swap).detach();
}

} // namespace aco
