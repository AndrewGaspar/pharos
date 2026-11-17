#!/usr/bin/env python3
"""Debug patch for the FG "stale generated frame" latch investigation (scratch copies only, never commit
the result; build like scripts/package):
- [fg] dbg_present_wait_ms (live, default 100): the present thread's wait for DLSS-G; small values
  force timeouts, the trigger of the stale-event latch (tools/fg-latch-force).
- counters of fence-event waits, timeouts, and wakes with the fence still below the waited value,
  logged next to the present-thread perf line ("perf: DEBUG wait_eval ...").

usage: fg-latch-dbg.py <src/fg/fg_d3d12.cpp> v6|a9|head
  v6 = cfc92d7 (event-trusting wait_eval on done12), a9 = a9fc0a2 (same, on wait_fence()),
  head = 9e2c3d8 (wait_on: resets the event, re-checks the value; a stale wake is counted, not returned)
"""
import sys

p, kind = sys.argv[1], sys.argv[2]
s = open(p).read()


def rep(a, b):
    global s
    assert a in s, a[:80]
    s = s.replace(a, b, 1)


COUNTERS = """// DEBUG (latch investigation): event waits, timeouts, wakes with the fence still below the value.
std::atomic<uint32_t> g_dbg_waits{0}, g_dbg_timeouts{0}, g_dbg_early{0}, g_dbg_timeout_late_ok{0}, g_dbg_early_game{0};
"""
OLD_BODY = """  ++g_dbg_waits;
  DWORD w = WaitForSingleObject(event, timeout_ms);
  UINT64 c = F->GetCompletedValue();
  if (w != WAIT_OBJECT_0) { ++g_dbg_timeouts; if (c >= v) ++g_dbg_timeout_late_ok; }
  else if (c < v) { if (event == S.game_event) ++g_dbg_early_game; else ++g_dbg_early; }
  return w == WAIT_OBJECT_0 || c >= v;
}"""

if kind == "v6":
    rep("""bool wait_eval(UINT64 v, HANDLE event, DWORD timeout_ms) {
  if (!v || S.done12->GetCompletedValue() >= v) return true;
  S.done12->SetEventOnCompletion(v, event);
  return WaitForSingleObject(event, timeout_ms) == WAIT_OBJECT_0 || S.done12->GetCompletedValue() >= v;
}""", COUNTERS + """bool wait_eval(UINT64 v, HANDLE event, DWORD timeout_ms) {
  if (!v || S.done12->GetCompletedValue() >= v) return true;
  S.done12->SetEventOnCompletion(v, event);
""" + OLD_BODY.replace("F->", "S.done12->"))
elif kind == "a9":
    rep("""bool wait_eval(UINT64 v, HANDLE event, DWORD timeout_ms) {
  ID3D12Fence* f = wait_fence();
  if (!v || f->GetCompletedValue() >= v) return true;
  f->SetEventOnCompletion(v, event);
  return WaitForSingleObject(event, timeout_ms) == WAIT_OBJECT_0 || f->GetCompletedValue() >= v;
}""", COUNTERS + """bool wait_eval(UINT64 v, HANDLE event, DWORD timeout_ms) {
  ID3D12Fence* f = wait_fence();
  if (!v || f->GetCompletedValue() >= v) return true;
  f->SetEventOnCompletion(v, event);
""" + OLD_BODY.replace("F->", "f->"))
elif kind == "head":
    rep("""    if (left <= 0) return false;
    WaitForSingleObject(event, static_cast<DWORD>((left + 999) / 1000));
  }
}""", """    if (left <= 0) { ++g_dbg_timeouts; return false; }
    if (WaitForSingleObject(event, static_cast<DWORD>((left + 999) / 1000)) == WAIT_OBJECT_0 &&
        f->GetCompletedValue() < v) {  // woken by a stale signal: the old code would have returned here
      if (event == S.game_event) ++g_dbg_early_game; else ++g_dbg_early;
    }
  }
}""")
    rep("""  ResetEvent(event);
  f->SetEventOnCompletion(v, event);
""", """  ResetEvent(event);
  f->SetEventOnCompletion(v, event);
  ++g_dbg_waits;
""")
    rep("bool wait_on(ID3D12Fence* f, UINT64 v, HANDLE event, DWORD timeout_ms) {\n  if (!v",
        COUNTERS + "bool wait_on(ID3D12Fence* f, UINT64 v, HANDLE event, DWORD timeout_ms) {\n  if (!v")
else:
    sys.exit("kind: v6|a9|head")

rep("""void present_loop() {""", """DWORD dbg_present_wait_ms() {
  static DWORD v = 100;
  static int64_t last = 0;
  int64_t t = now_us();
  if (t - last > 500000) {
    last = t;
    v = GetPrivateProfileIntW(L"fg", L"dbg_present_wait_ms", 100, (module_dir() + L"\\\\aco_dlss.ini").c_str());
  }
  return v;
}

void present_loop() {""")
rep("wait_eval(j.wait_value, event, 100)", "wait_eval(j.wait_value, event, dbg_present_wait_ms())")
rep("""          n ? dr / 1000.0 / n : 0.0, P.gen_dropped.exchange(0));""",
    """          n ? dr / 1000.0 / n : 0.0, P.gen_dropped.exchange(0));
  ACO_LOG("perf: DEBUG wait_eval: %u event waits, %u timed out (%u with the value reached by then), %u woke with the fence "
          "below the value (present thread; game thread %u) [dbg_present_wait_ms=%lu]", g_dbg_waits.exchange(0),
          g_dbg_timeouts.exchange(0), g_dbg_timeout_late_ok.exchange(0), g_dbg_early.exchange(0),
          g_dbg_early_game.exchange(0), static_cast<unsigned long>(dbg_present_wait_ms()));""")
open(p, "w").write(s)
print("patched", p, kind)
