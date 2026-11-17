#include "core/tlsext.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "core/config.h"
#include "core/iat.h"
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
uintptr_t g_img = 0, g_img_size = 0;    // the game image (only its TlsAlloc calls get virtual indices)
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
  uintptr_t ra = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  if (!g_active.load(std::memory_order_relaxed) || ra - g_img >= g_img_size || native_free() > kReserve) {
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
  HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
  if (!k32) return false;
  g_img = reinterpret_cast<uintptr_t>(base);
  g_img_size = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew)
                   ->OptionalHeader.SizeOfImage;
  const struct { uintptr_t rva; const char* name; void* hook; void** orig; } want[] = {
      {kSlotAlloc, "TlsAlloc", reinterpret_cast<void*>(&hk_alloc), reinterpret_cast<void**>(&g_alloc)},
      {kSlotGet, "TlsGetValue", reinterpret_cast<void*>(&hk_get), reinterpret_cast<void**>(&g_get)},
      {kSlotSet, "TlsSetValue", reinterpret_cast<void*>(&hk_set), reinterpret_cast<void**>(&g_set)},
      {kSlotFree, "TlsFree", reinterpret_cast<void*>(&hk_free), reinterpret_cast<void**>(&g_free)}};
  // The game must be calling kernel32's exports through its private import table (filled by the
  // VMProtect loader; patching that table does not stick: it was found reverted after the world load).
  for (const auto& w : want) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(base + w.rva, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    void* cur = *reinterpret_cast<void* volatile*>(base + w.rva);
    void* a = reinterpret_cast<void*>(GetProcAddress(k32, w.name));
    if (!cur || cur != a) {
      ACO_LOG("tls: game import slot rva %llx is %p, not kernel32!%s (%p); tls_extend disabled",
              (unsigned long long)w.rva, cur, w.name, a);
      return false;
    }
  }
  // Wine's kernel32 exports are stubs that jump through kernel32's own import table (to
  // kernelbase): hook those entries. Every caller of kernel32's Tls* passes through us; only the
  // game's own TlsAlloc calls ever receive virtual indices.
  g_slot = TlsAlloc();
  if (g_slot == TLS_OUT_OF_INDEXES) return false;
  for (const auto& w : want)
    if (!iat_hook(k32, nullptr, w.name, w.hook, w.orig)) {
      ACO_LOG("tls: kernel32 does not import %s; tls_extend disabled", w.name);
      return false;  // hooks installed so far only pass through
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
