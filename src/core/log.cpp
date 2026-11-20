#include "core/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace aco {

namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
std::wstring g_out;
} // namespace

const std::wstring& out_dir() {
  if (g_out.empty()) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ACO_DLSS_OUT", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) n = GetEnvironmentVariableW(L"QUIVER_LAB_RUN_DIR", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
      g_out = buf;
    } else {
      GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), buf, MAX_PATH);
      g_out = buf;
      g_out.resize(g_out.find_last_of(L"\\/"));
    }
    CreateDirectoryW(g_out.c_str(), nullptr);
  }
  return g_out;
}

void log_init() {
  std::lock_guard lock(g_mutex);
  if (g_file) return;
  std::wstring path = out_dir() + L"\\pharos.log";
  g_file = _wfopen(path.c_str(), L"w");
  // In the Quiver lab the log is in this run's folder; %QUIVER_LAB%\pharos.log is a hard link to it,
  // so the instance manifest's log_file (lab:pharos.log) always names the current run's log (for
  // `quiver lab set --wait`, which can't name a run folder).
  wchar_t lab[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"QUIVER_LAB", lab, MAX_PATH);
  if (g_file && n > 0 && n < MAX_PATH) {
    std::wstring link = std::wstring(lab, n) + L"\\pharos.log";
    if (link != path) {
      DeleteFileW(link.c_str());
      CreateHardLinkW(link.c_str(), path.c_str(), nullptr);
    }
  }
}

void logf(const char* fmt, ...) {
  std::lock_guard lock(g_mutex);
  if (!g_file) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  fprintf(g_file, "%02d:%02d:%02d.%03d [%5lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
          GetCurrentThreadId());
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_file, fmt, ap);
  va_end(ap);
  fputc('\n', g_file);
  fflush(g_file);
}

} // namespace aco
