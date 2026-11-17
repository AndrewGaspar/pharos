#include "core/tlsext.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "core/config.h"
#include "core/log.h"

namespace aco {
namespace {

// v1.62: the game's private import table entries (call qword ptr [rip+x] at 2000+ sites).
constexpr uintptr_t kSlotAlloc = 0x353a688, kSlotGet = 0x353a690, kSlotSet = 0x353a698, kSlotFree = 0x353a6a0;
constexpr DWORD kVirtBase = 0x10000;   // virtual indices: kVirtBase + n (native ones are < 1088)
constexpr DWORD kMaxVirt = 0x10000;
constexpr int kReserve = 64;           // native indices left for everything else in the process
constexpr DWORD kChunk = 256;

using AllocFn = DWORD(WINAPI*)();
using FreeFn = BOOL(WINAPI*)(DWORD);
using GetFn = LPVOID(WINAPI*)(DWORD);
using SetFn = BOOL(WINAPI*)(DWORD, LPVOID);
AllocFn g_alloc;
FreeFn g_free;
GetFn g_get;
SetFn g_set;

DWORD g_slot = TLS_OUT_OF_INDEXES;     // our native slot -> per-thread Block
struct Entry { void* v; uint32_t gen; uint32_t pad; };
struct Block { Entry* chunks[kMaxVirt / kChunk]; };

std::atomic<uint32_t> g_gen[kMaxVirt];
SRWLOCK g_lock = SRWLOCK_INIT;         // virtual index allocation only (plain SRW, no TLS)
DWORD g_free_list[kMaxVirt];
DWORD g_free_n = 0, g_next = 0;
std::atomic<unsigned> g_in_use{0}, g_handed{0};
std::atomic<bool> g_active{false};

int native_free() {
  // PEB (x64): TlsBitmapBits at +0x80 (64 bits), TlsExpansionBitmapBits at +0x240 (1024 bits).
  auto* peb = reinterpret_cast<const uint8_t*>(__readgsqword(0x60));
  if (!peb) return -1;
  int used = 0;
  for (int i = 0; i < 2; ++i) used += __builtin_popcount(reinterpret_cast<const volatile uint32_t*>(peb + 0x80)[i]);
  for (int i = 0; i < 32; ++i) used += __builtin_popcount(reinterpret_cast<const volatile uint32_t*>(peb + 0x240)[i]);
  return 64 + 1024 - used;
}

Entry* entry(DWORD v, bool create) {
  auto* b = static_cast<Block*>(g_get(g_slot));
  if (!b) {
    if (!create) return nullptr;
    b = static_cast<Block*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Block)));
    if (!b || !g_set(g_slot, b)) return nullptr;
  }
  Entry*& c = b->chunks[v / kChunk];
  if (!c) {
    if (!create) return nullptr;
    c = static_cast<Entry*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Entry) * kChunk));
    if (!c) return nullptr;
  }
  return &c[v % kChunk];
}

DWORD WINAPI hk_alloc() {
  if (native_free() > kReserve) {
    DWORD r = g_alloc();
    if (r != TLS_OUT_OF_INDEXES) return r;
  }
  AcquireSRWLockExclusive(&g_lock);
  DWORD v = TLS_OUT_OF_INDEXES;
  if (g_free_n) v = g_free_list[--g_free_n];
  else if (g_next < kMaxVirt) v = g_next++;
  if (v != TLS_OUT_OF_INDEXES) g_gen[v].fetch_add(1);  // new slot reads 0 in every thread
  ReleaseSRWLockExclusive(&g_lock);
  if (v == TLS_OUT_OF_INDEXES) return g_alloc();
  g_in_use.fetch_add(1);
  if (g_handed.fetch_add(1) == 0) ACO_LOG("tls: native TLS indices exhausted (%d free); handing out virtual ones", native_free());
  return kVirtBase + v;
}

BOOL WINAPI hk_free(DWORD i) {
  if (i < kVirtBase) return g_free(i);
  DWORD v = i - kVirtBase;
  if (v >= kMaxVirt) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  g_gen[v].fetch_add(1);
  AcquireSRWLockExclusive(&g_lock);
  g_free_list[g_free_n++] = v;
  ReleaseSRWLockExclusive(&g_lock);
  g_in_use.fetch_sub(1);
  return TRUE;
}

LPVOID WINAPI hk_get(DWORD i) {
  if (i < kVirtBase) return g_get(i);  // tail call to kernel32
  DWORD v = i - kVirtBase;
  if (v >= kMaxVirt) { SetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
  Entry* e = entry(v, false);
  SetLastError(ERROR_SUCCESS);
  return e && e->gen == g_gen[v].load(std::memory_order_relaxed) ? e->v : nullptr;
}

BOOL WINAPI hk_set(DWORD i, LPVOID val) {
  if (i < kVirtBase) return g_set(i, val);
  DWORD v = i - kVirtBase;
  if (v >= kMaxVirt) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  Entry* e = entry(v, true);
  if (!e) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  e->v = val;
  e->gen = g_gen[v].load(std::memory_order_relaxed);
  return TRUE;
}

bool install() {
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  HMODULE k32 = GetModuleHandleW(L"kernel32.dll"), kb = GetModuleHandleW(L"kernelbase.dll");
  const struct { uintptr_t rva; const char* name; } want[] = {
      {kSlotAlloc, "TlsAlloc"}, {kSlotGet, "TlsGetValue"}, {kSlotSet, "TlsSetValue"}, {kSlotFree, "TlsFree"}};
  void* cur[4];
  for (int i = 0; i < 4; ++i) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(base + want[i].rva, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    cur[i] = *reinterpret_cast<void* volatile*>(base + want[i].rva);
    void* a = k32 ? reinterpret_cast<void*>(GetProcAddress(k32, want[i].name)) : nullptr;
    void* b = kb ? reinterpret_cast<void*>(GetProcAddress(kb, want[i].name)) : nullptr;
    if (!cur[i] || (cur[i] != a && cur[i] != b)) {
      ACO_LOG("tls: import slot rva %llx is %p, not %s (%p / %p); tls_extend disabled",
              (unsigned long long)want[i].rva, cur[i], want[i].name, a, b);
      return false;
    }
  }
  g_alloc = reinterpret_cast<AllocFn>(cur[0]);
  g_get = reinterpret_cast<GetFn>(cur[1]);
  g_set = reinterpret_cast<SetFn>(cur[2]);
  g_free = reinterpret_cast<FreeFn>(cur[3]);
  g_slot = g_alloc();
  if (g_slot == TLS_OUT_OF_INDEXES) return false;
  void* hooks[4] = {reinterpret_cast<void*>(&hk_alloc), reinterpret_cast<void*>(&hk_get),
                    reinterpret_cast<void*>(&hk_set), reinterpret_cast<void*>(&hk_free)};
  for (int i = 0; i < 4; ++i) {
    void** p = reinterpret_cast<void**>(base + want[i].rva);
    DWORD old = 0;
    if (!VirtualProtect(p, sizeof(void*), PAGE_READWRITE, &old)) return false;
    __atomic_store_n(p, hooks[i], __ATOMIC_SEQ_CST);
    VirtualProtect(p, sizeof(void*), old, &old);
  }
  return true;
}

} // namespace

int tlsext_native_free() { return native_free(); }
unsigned tlsext_virtual_in_use() { return g_in_use.load(); }
bool tlsext_active() { return g_active.load(); }

void tlsext_poll() {
  static int state = 0;  // 0 unchecked, 1 installed, 2 off/unsupported
  static ULONGLONG last = 0;
  ULONGLONG now = GetTickCount64();
  if (state == 0) {
    if (config().world_tls_extend == 0.0f) { state = 2; return; }
    state = install() ? 1 : 2;
    g_active = state == 1;
    ACO_LOG("tls: %s (%d native indices free)", state == 1 ? "game TlsAlloc/TlsGetValue/TlsSetValue/TlsFree hooked (v1.62)"
                                                         : "not installed", native_free());
    last = now;
  }
  if (state != 1 || now - last < 10000) return;
  last = now;
  if (g_handed.load() || native_free() < 256) ACO_LOG("tls: %d native free, %u virtual indices in use", native_free(), g_in_use.load());
}

} // namespace aco
