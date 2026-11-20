#pragma once
#include <string>

namespace aco {

// Directory for logs, captures and debug triggers (framedump, memtool, ...): ACO_DLSS_OUT (a Windows
// path) if set, else the Quiver lab run folder (QUIVER_LAB_RUN_DIR, a fresh folder per launch), else
// the directory containing our DLL (the game directory).
const std::wstring& out_dir();
void log_init();
void logf(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));

} // namespace aco

#define ACO_LOG(...) ::aco::logf(__VA_ARGS__)
