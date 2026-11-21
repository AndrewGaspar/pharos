#include "ports/common.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

extern "C" IMAGE_DOS_HEADER __ImageBase;
namespace port {
namespace fs = std::filesystem;
using Key = std::array<unsigned char, 16>;

HMODULE system_library(const wchar_t* dll) {
  wchar_t dir[MAX_PATH];
  UINT n = GetSystemDirectoryW(dir, MAX_PATH);
  if (!n || n >= MAX_PATH) return nullptr;
  HMODULE mod = LoadLibraryW((fs::path(dir) / dll).c_str());
  return mod == reinterpret_cast<HMODULE>(&__ImageBase) ? nullptr : mod;
}

bool active() {
  static const bool matches = [] {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
    auto exe = fs::path(path).filename().wstring();
    return !_wcsicmp(exe.c_str(), PORT_EXE L".exe") || !_wcsicmp(exe.c_str(), PORT_EXE L"_plus.exe");
  }();
  return matches;
}

Entry* lookup(const void* bytecode, size_t size) {
  if (!bytecode || size < 32 || memcmp(bytecode, "DXBC", 4)) return nullptr;
  static auto entries = [] {
    std::map<Key, Entry> table;
    wchar_t file[MAX_PATH];
    if (!GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), file, MAX_PATH)) return table;
    std::ifstream in(fs::path(file).parent_path() / L"shaders.tsv");
    for (std::string line; std::getline(in, line);) {
      size_t end = line.find('\t', 33);
      if (line.size() < 34 || line[32] != '\t' || end == std::string::npos) continue;
      Key key{};
      size_t i = 0;
      for (; i < key.size(); ++i) {
        const char* digits = "0123456789abcdef";
        const char* hi = strchr(digits, line[i * 2]);
        const char* lo = strchr(digits, line[i * 2 + 1]);
        if (!hi || !lo) break;
        key[i] = (hi - digits) * 16 + (lo - digits);
      }
      if (i != key.size()) continue;
      auto [it, inserted] = table.try_emplace(key);
      if (inserted) {
        it->second.role = line.substr(33, end - 33);
        it->second.detail = line.substr(end + 1);
      }
    }
    ACO_LOG("%s: %zu distinct shader signatures loaded", PORT_NAME, table.size());
    return table;
  }();
  Key key;
  memcpy(key.data(), static_cast<const unsigned char*>(bytecode) + 4, key.size());
  auto it = entries.find(key);
  return it == entries.end() ? nullptr : &it->second;
}

void created(Entry& e) {
  if (!e.created.exchange(true, std::memory_order_relaxed)) ACO_LOG("created %s %s", e.role.c_str(), e.detail.c_str());
}
void bound(Entry& e) { ACO_LOG("bound %s %s", e.role.c_str(), e.detail.c_str()); }

}  // namespace port
