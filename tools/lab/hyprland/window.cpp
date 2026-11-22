// A real Wine X11 window observed by the test compositor in check.py.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <fstream>

#include "core/hyprland.h"

int main() {
  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"PharosWindowFixture";
  if (!RegisterClassW(&wc)) return 1;
  HWND hwnd = CreateWindowW(wc.lpszClassName, L"Origins window", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd) return 2;
  const auto xid = reinterpret_cast<uintptr_t>(GetPropW(hwnd, L"__wine_x11_whole_window"));
  if (!xid) return 3;
  std::ofstream("xid") << xid;
  const struct { const char* stage; int expected; } cases[] = {
      {"headless", 0}, {"desktop", 1}, {"moved", 0}, {"other_pid", 0},
      {"duplicate_title", 0}, {"return", 1}, {"unmapped", 0}, {"removed", 0}};
  for (const auto& test : cases) {
    std::ofstream("stage") << test.stage;
    const int state = aco::hypr_display_stereo_now(hwnd);
    wchar_t caption[64]{};
    GetWindowTextW(hwnd, caption, 64);
    printf("%s: stereo=%d (expected %d), caption=%ls\n", test.stage, state, test.expected, caption);
    if (state != test.expected || wcscmp(caption, L"Origins window")) return 4;
  }
  DestroyWindow(hwnd);
}
