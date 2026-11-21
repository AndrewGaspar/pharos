#pragma once
#include <windows.h>

#include <atomic>

#include "core/log.h"

namespace port {
struct Entry {
  std::string role, detail;
  std::atomic<bool> created{false}, bound{false}, sampled{false};
};
HMODULE system_library(const wchar_t* dll);
bool active();
Entry* lookup(const void* bytecode, size_t size);
void created(Entry& entry);
void bound(Entry& entry);
const GUID shader_tag = {0xe01488ba, 0x817d, 0x402a, {0x91, 0xb1, 0xd0, 0x13, 0x58, 0x41, 0x60, 0x29}};

template <class Object>
void tag(Object* object, Entry* entry) {
  if (object && entry) {
    object->SetPrivateData(shader_tag, sizeof(entry), &entry);
    created(*entry);
  }
}

template <class Object>
Entry* binding(Object* object) {
  Entry* entry = nullptr;
  UINT size = sizeof(entry);
  if (!object || FAILED(object->GetPrivateData(shader_tag, &size, &entry))) return nullptr;
  return entry;
}
}  // namespace port
