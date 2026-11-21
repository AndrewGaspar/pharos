#include "stereo/system_cursor.h"
#include <array>
#include <atomic>
#include <mutex>

namespace aco {
namespace {
constexpr wchar_t state_key[] = L"AnvilStereo.Cursor";
struct WindowCursor {
  WNDPROC previous{};
  HCURSOR saved{};
};
std::atomic<HWND> wanted{};
std::mutex installation;
UINT refresh_message() {
  static const UINT msg = RegisterWindowMessageW(L"AnvilStereo.RefreshClientCursor");
  return msg;
}
HCURSOR transparent() {
  static const auto cursor = [] {
    std::array<BYTE, 128> mask{}, color{};
    mask.fill(0xff);
    return CreateCursor(nullptr, 0, 0, 32, 32, mask.data(), color.data());
  }();
  return cursor;
}
void update(HWND window, WindowCursor& cursor, bool client) {
  if (wanted.load() == window && client && GetForegroundWindow() == window) {
    HCURSOR current = GetCursor();
    if (current && current != transparent()) cursor.saved = SetCursor(transparent());
  } else if (GetCursor() == transparent() && cursor.saved) {
    SetCursor(cursor.saved);
    cursor.saved = nullptr;
  }
}
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
  auto* cursor = static_cast<WindowCursor*>(GetPropW(window, state_key));
  if (!cursor) return DefWindowProcW(window, message, w, l);
  if (message == refresh_message()) {
    POINT at{};
    GetCursorPos(&at);
    const auto target = WindowFromPoint(at);
    update(window, *cursor, target == window || IsChild(window, target));
    return 0;
  }
  auto previous = cursor->previous;
  if (message == WM_NCDESTROY) {
    update(window, *cursor, false);
    HWND old = window;
    wanted.compare_exchange_strong(old, nullptr);
    RemovePropW(window, state_key);
    delete cursor;
    return CallWindowProcW(previous, window, message, w, l);
  }
  auto result = CallWindowProcW(previous, window, message, w, l);
  // The game can destroy its window inside its own message handler.
  if ((message == WM_SETCURSOR || message == WM_KILLFOCUS) && GetPropW(window, state_key) == cursor)
    update(window, *cursor, message == WM_SETCURSOR && LOWORD(l) == HTCLIENT);
  return result;
}
bool install(HWND window) {
  if (GetPropW(window, state_key)) return true;
  std::lock_guard guard(installation);
  if (GetPropW(window, state_key)) return true;
  auto* state = new WindowCursor;
  state->previous = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC));
  if (!state->previous || !SetPropW(window, state_key, state)) {
    delete state;
    return false;
  }
  auto original = SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(procedure));
  if (!original) {
    RemovePropW(window, state_key);
    delete state;
    return false;
  }
  return true;
}
}  // namespace
bool stereo_system_cursor(HWND window) {
  CURSORINFO cursor{sizeof(cursor)};
  if (!window || GetForegroundWindow() != window || !GetCursorInfo(&cursor) ||
      !(cursor.flags & CURSOR_SHOWING) || !cursor.hCursor || !transparent() || !install(window)) window = nullptr;
  HWND old = wanted.exchange(window);
  if (old && old != window) PostMessageW(old, refresh_message(), 0, 0);
  if (window && cursor.hCursor != transparent()) PostMessageW(window, refresh_message(), 0, 0);
  return window != nullptr;
}
}  // namespace aco
