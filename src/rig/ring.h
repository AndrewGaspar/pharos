#pragma once
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace aco::rig {
// One producer and one reader share a bounded byte stream. Cursor publication is
// interprocess atomic. Wine maps the same file pages visible to a Linux reader.
class RingWriter {
 public:
  static std::unique_ptr<RingWriter> open(const std::wstring& path) {
    HANDLE file =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    bool valid = GetFileSizeEx(file, &size) && size.QuadPart > kHeader && size.QuadPart <= 64 * 1024 * 1024 + kHeader;
    HANDLE map = valid ? CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, 0, nullptr) : nullptr;
    CloseHandle(file);
    if (!map) return {};
    auto view = static_cast<uint8_t*>(MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
    CloseHandle(map);
    if (!view) return {};
    auto ring = std::unique_ptr<RingWriter>(new RingWriter(view, size.QuadPart - kHeader));
    uint64_t capacity;
    memcpy(&capacity, view + 8, 8);
    if (memcmp(view, "PHARRNG1", 8) || capacity != ring->capacity_ || ring->load(16) || ring->load(24) ||
        ring->cancelled())
      return {};
    return ring;
  }
  ~RingWriter() { UnmapViewOfFile(view_); }
  bool cancelled() const { return load(32) != 0; }
  bool write(const void* data, size_t bytes) {
    using Clock = std::chrono::steady_clock;
    auto from = static_cast<const uint8_t*>(data);
    auto deadline = Clock::now() + std::chrono::seconds(30);
    while (bytes) {
      uint64_t read = load(24);
      if (cancelled() || read > written_ || written_ - read > capacity_) return false;
      size_t space = capacity_ - (written_ - read);
      if (!space) {
        if (Clock::now() >= deadline) return false;
        Sleep(1);
        continue;
      }
      size_t offset = written_ % capacity_;
      size_t n = std::min({bytes, space, capacity_ - offset});
      memcpy(view_ + kHeader + offset, from, n);
      written_ += n;
      InterlockedExchange64(word(16), written_);
      from += n;
      bytes -= n;
      deadline = Clock::now() + std::chrono::seconds(30);
    }
    return true;
  }

 private:
  static constexpr int kHeader = 64;
  RingWriter(uint8_t* view, size_t capacity) : view_(view), capacity_(capacity) {}
  volatile LONG64* word(size_t offset) const { return reinterpret_cast<volatile LONG64*>(view_ + offset); }
  uint64_t load(size_t offset) const { return InterlockedCompareExchange64(word(offset), 0, 0); }
  uint8_t* view_;
  size_t capacity_;
  uint64_t written_ = 0;
};
}  // namespace aco::rig
