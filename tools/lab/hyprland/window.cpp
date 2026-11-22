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
  HWND splash = CreateWindowW(wc.lpszClassName, L"", WS_POPUP | WS_VISIBLE,
                              20, 20, 400, 225, nullptr, nullptr, wc.hInstance, nullptr);
  const auto splash_xid = reinterpret_cast<uintptr_t>(GetPropW(splash, L"__wine_x11_whole_window"));
  if (!splash_xid) return 5;
  std::ofstream("splash_xid") << splash_xid;
  const struct { const char* stage; bool forced, expected; } splash_cases[] = {
      {"splash_unmapped", false, false}, {"splash_mono", false, false}, {"splash_mono", true, true},
      {"splash_no_rule", false, false}, {"splash_existing_tag", false, false},
      {"splash_existing_rule", false, true}, {"splash_stereo", false, true}};
  for (const auto& test : splash_cases) {
    std::ofstream("stage") << test.stage;
    bool added = false;
    bool ready = aco::hypr_prepare_splash_stereo(splash, test.forced, &added);
    printf("%s: forced=%d packed=%d (expected %d)\n", test.stage, test.forced, ready, test.expected);
    if (ready != test.expected || GetWindowTextLengthW(splash) != 0 || aco::hypr_display_stereo() != 0) return 6;
    if (added != (strcmp(test.stage, "splash_stereo") == 0)) return 7;
    if (added) aco::hypr_clear_splash_stereo(splash);
  }
  DestroyWindow(splash);
  DestroyWindow(hwnd);
}
