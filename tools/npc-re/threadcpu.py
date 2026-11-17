# threadcpu.py <secs> : CPU use of the game's threads over <secs>, grouped by thread name (percent
# of one core), from /proc/<pid>/task/*/stat; plus GPU utilization samples from nvidia-smi and the
# game's resident memory. Host-side, no memory reads.
import os, re, subprocess, sys, time, collections

pid = int(subprocess.run([os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gamepid.sh'), os.environ['ACO_INST']],
                         capture_output=True, text=True, check=True).stdout)
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 30
hz = os.sysconf('SC_CLK_TCK')


def snap():
    out = {}
    for t in os.listdir('/proc/%d/task' % pid):
        try:
            s = open('/proc/%d/task/%s/stat' % (pid, t)).read()
        except OSError:
            continue
        name = s[s.index('(') + 1:s.rindex(')')]
        f = s[s.rindex(')') + 2:].split()
        out[t] = (name, int(f[11]) + int(f[12]))
    return out


a = snap()
gpu = []
t0 = time.time()
while time.time() - t0 < secs:
    r = subprocess.run(['nvidia-smi', '--query-gpu=utilization.gpu,memory.used', '--format=csv,noheader,nounits'], capture_output=True, text=True)
    try: gpu.append(tuple(int(x) for x in r.stdout.strip().split(',')))
    except ValueError: pass
    time.sleep(2)
b = snap()
el = time.time() - t0
grp = collections.Counter()
for t, (name, ticks) in b.items():
    if t in a:
        grp[re.sub(r'\d+$', '#', name)] += (ticks - a[t][1]) / hz / el * 100
rss = int(re.search(r'VmRSS:\s+(\d+)', open('/proc/%d/status' % pid).read())[1]) // 1024
print('cpu %% of a core: ' + ', '.join('%s %.0f' % (n, v) for n, v in grp.most_common(8)) + ' | total %.0f' % sum(grp.values()))
if gpu:
    print('gpu util %.0f%% (max %d), vram %d MiB | game RSS %d MiB' % (sum(g[0] for g in gpu) / len(gpu), max(g[0] for g in gpu), max(g[1] for g in gpu), rss))
