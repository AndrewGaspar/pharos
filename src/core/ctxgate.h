#pragma once
#include <atomic>

struct ID3D11DeviceContext;

namespace aco {

// Fair access to the D3D11 immediate context for Frame Generation's present thread.
//
// With multithread protection DXVK guards the immediate context with an unfair spinlock. The
// game's render thread takes and releases it for every call while it records a frame, so another
// thread waiting for it (our present thread, for its copy + Present) can starve for most of the
// frame. The present thread raises the gate while it needs the context; the context hooks (which
// the render thread passes through constantly) then wait before their next call, with a deadline.
extern std::atomic<int> g_ctx_gate;      // > 0: a presenting thread wants the immediate context
extern thread_local int g_ctx_gate_exempt;  // > 0: this thread must not wait (it is the presenter,
                                            // or it holds the context lock itself)
extern ID3D11DeviceContext* g_ctx_gate_imm;  // the immediate context (set when the gate is used)

void ctx_gate_wait();

inline void ctx_gate(ID3D11DeviceContext* ctx) {
  if (g_ctx_gate.load(std::memory_order_acquire) > 0 && ctx == g_ctx_gate_imm && g_ctx_gate_exempt == 0)
    ctx_gate_wait();
}

} // namespace aco
