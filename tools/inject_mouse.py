#!/usr/bin/env python3
"""Injects horizontal relative mouse motion (mouse-look) through XTEST and logs when each event
was sent, in CLOCK_MONOTONIC_RAW ns (Wine's QPC time base). Runs inside the instance container.

usage: inject_mouse.py <out.csv> <count> <dx> [min_gap_s max_gap_s]
"""
import ctypes
import random
import sys
import time

out, count, dx = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
gap_lo, gap_hi = (float(sys.argv[4]), float(sys.argv[5])) if len(sys.argv) > 5 else (0.4, 0.7)

x11 = ctypes.CDLL("libX11.so.6")
xtst = ctypes.CDLL("libXtst.so.6")
x11.XOpenDisplay.restype = ctypes.c_void_p
x11.XFlush.argtypes = [ctypes.c_void_p]
xtst.XTestFakeRelativeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
dpy = x11.XOpenDisplay(None)
if not dpy:
    sys.exit("cannot open display")

raw = lambda: time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
with open(out, "w") as f:
    f.write("t_send_ns,t_flushed_ns,dx\n")
    for i in range(count):
        # Random gaps so injections don't phase-lock with the frame cadence.
        time.sleep(random.uniform(gap_lo, gap_hi))
        d = dx if i % 2 == 0 else -dx
        t0 = raw()
        xtst.XTestFakeRelativeMotionEvent(dpy, d, 0, 0)
        x11.XFlush(dpy)
        t1 = raw()
        f.write(f"{t0},{t1},{d}\n")
        f.flush()
