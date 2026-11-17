# jobwhy.py <job addr hex>... : why is a job not finished? Walk its unfinished prerequisites
# (Anvil array at +0xc0 holding the prerequisites' +0x60; a prerequisite counts while it still has
# pending prerequisites itself, or has been started but not finished), depth first, printing each with
# jobq.py's fields. Leaves (pending 0) are where the graph is stuck. ACO_INST picks the instance.
import sys
import jobq as J  # prints the jobq summary first

seen = set()


def why(p, depth):
    if p in seen or depth > 25: return
    seen.add(p)
    J.job(p, '  ' * depth + 'why')
    arr, n = J.L.q(p + 0xc0), (J.L.d(p + 0xc8) or 0) >> 17
    for i in range(min(n, 64)):
        q0 = J.L.q(arr + 8 * i)
        if not q0: continue
        c = q0 - 0x60
        # descend into prerequisites that are not done: pending > 0, or state != 0, or armed (+0x60 set:
        # completion not yet notified to dependents)
        if J.D(c + 0x58) > 0 or J.D(c + 0x4c) not in (0, -1) or J.b1(c + 0x60) == 1:
            why(c, depth + 1)


print('---- why')
for a in sys.argv[1:]:
    why(int(a, 16), 0)
