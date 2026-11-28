#!/usr/bin/env python3
# take23 item 8: clocks, load and frames of one vm-boot session, from thermal.txt, serial.txt and perf.txt.
#   t8.py SESSION_DIR [--rounds]      (times printed are UTC; thermal.txt's are local = UTC+1)
import re, sys, collections, statistics as st
D = sys.argv[1]; show_rounds = '--rounds' in sys.argv
def hms(s): h, m, x = s.split(':'); return int(h) * 3600 + int(m) * 60 + int(x)
# ---- thermal.txt: one sample every ~1.25 s
T = []
for l in open(D + '/thermal.txt', errors='replace'):
    if l.startswith('#') or ' f0 ' not in l: continue
    w = l.split(); d = {'t': hms(w[0]) - 3600}
    def after(k, n=1): i = w.index(k); return w[i + 1:i + 1 + n]
    try:
        for k in ('c0', 'c1', 'gpu', 'board', 'bat'): d[k] = int(after(k)[0]) / 10
        d['f0'] = int(after('f0')[0].split('/')[0]) / 1e6; d['f6'] = int(after('f6')[0].split('/')[0]) / 1e6
        g = after('g', 2); d['g'] = int(g[0]) / 1e6; d['gbusy'] = int(g[1])
        b = after('batt', 2); d['ia'] = int(b[0]) / 1e6; d['v'] = int(b[1]) / 1e6
        d['usb'] = after('usb')[0]; hw = after('hw', 2); d['h0'] = int(hw[0]) / 1e6; d['h6'] = int(hw[1]) / 1e6
        lm = after('lmh', 2); d['lmh'] = lm
        d['cool'] = ' '.join(w[w.index('cool') + 1:])
    except (ValueError, IndexError): continue
    T.append(d)
# ---- serial.txt: 10 s rounds
R = []; cur = None
for l in open(D + '/serial.txt', errors='replace'):
    m = re.search(r'oneios-clock: (\d\d:\d\d:\d\d) UTC, up (\d+) s, round (\d+)', l)
    if m: cur = {'t': hms(m.group(1)), 'n': int(m.group(3)), 'procs': {}}; R.append(cur); continue
    if cur is None: continue
    if 'oneios-cpu:' in l:
        m = re.search(r'\(all (\d+)%\)', l)
        if m: cur['all'] = int(m.group(1))
        for p, v in re.findall(r'(\S+)\[\d+\] (\d+)%', l): cur['procs'][p] = int(v)
    if 'oneios-cpu-sys:' in l: cur['sys'] = sum(int(v) for _, v in re.findall(r'(\S+)\[\d+\] (\d+)%', l))
    m = re.search(r'display-pace: 10 s: swaps \+(\d+), ([\d.]+) a second while moving; apart by <1 refresh \+(\d+), 1 \+(\d+), 2 \(one skipped\) \+(\d+), more \+(\d+), pauses \+(\d+)', l)
    if m: cur['sw'] = int(m.group(1)); cur['fps'] = float(m.group(2)); cur['gaps'] = [int(m.group(i)) for i in (3, 4, 5, 6, 7)]
    m = re.search(r'gpu-report: 10 s: submits \+(\d+).*frames \+(\d+) mean (\d+) us.*<1/<2/<4/<8/<16/<32 ms \+(\d+)/\+(\d+)/\+(\d+)/\+(\d+)/\+(\d+)/\+(\d+) longer \+(\d+)', l)
    if m: cur['sub'] = int(m.group(1)); cur['gfr'] = int(m.group(2)); cur['gus'] = int(m.group(3)); cur['gh'] = [int(m.group(i)) for i in range(4, 11)]
    m = re.search(r'gpu-waits: 10 s: waits \+(\d+).*looks \+(\d+)', l)
    if m: cur['waits'] = int(m.group(1)); cur['looks'] = int(m.group(2))
    m = re.search(r'oneios-memory: 10 s: available (\d+) MiB', l)
    if m: cur['avail'] = int(m.group(1))
# a round's line is printed at its END: it covers [t-10, t]
for r in R:
    s = [x for x in T if r['t'] - 10 <= x['t'] < r['t']]
    r['T'] = s
    for k in ('f0', 'f6', 'g', 'gbusy', 'c0', 'c1'): r[k] = st.mean(x[k] for x in s) if s else float('nan')
# ---- perf.txt: thread tables (cumulative run ns) and kgsl's time in each clock
P = []; cur = None; kg = {}
sect = None
for l in open(D + '/perf.txt', errors='replace'):
    m = re.match(r'(\d\d:\d\d:\d\d) threads', l)
    if m: cur = {'t': hms(m.group(1)) - 3600, 'th': {}}; P.append(cur); continue
    m = re.match(r'thread (\d+) cpus (\S+) .* ticks \d+ sched (\d+) (\d+) (\d+) comm (.*)', l)
    if m and cur is not None: cur['th'][(m.group(1), m.group(6).strip())] = (int(m.group(3)), int(m.group(4)), int(m.group(5)), m.group(2)); continue
    m = re.match(r'kgsl (start|end) trans_stat', l)
    if m: sect = m.group(1); kg[sect] = {}; continue
    m = re.match(r'[ *]+(\d+):((?:\s+\d+)+)\s*$', l)
    if m and sect: kg[sect][int(m.group(1)) // 1000000] = int(m.group(2).split()[-1])
print('== session: thermal samples %d (%.2f s apart), rounds %d, thread tables %d' % (len(T), (T[-1]['t'] - T[0]['t']) / (len(T) - 1), len(R), len(P)))
for u in sorted(set(x['usb'] for x in T)):
    s_ = [x for x in T if x['usb'] == u]
    print('usb online %s: %d samples; battery %.2f W out on average (+ = out of it), hottest CPU sensor mean %.1f C' % (u, len(s_), st.mean(-x['ia'] * x['v'] for x in s_), st.mean(max(x['c0'], x['c1']) for x in s_)))
print('usb online values:', collections.Counter(x['usb'] for x in T), '| lmh:', collections.Counter(tuple(x['lmh']) for x in T).most_common(2), '| cooling states seen:', collections.Counter(x['cool'] for x in T).most_common(3))
for k, n in (('c0', 'CPU sensor 0 C'), ('c1', 'CPU sensor 1 C'), ('gpu', 'GPU C'), ('board', 'board C'), ('bat', 'battery C'), ('f0', 'policy0 GHz'), ('f6', 'policy6 GHz'), ('g', 'GPU MHz'), ('gbusy', 'GPU busy %'), ('ia', 'battery A (+ = into it)')):
    v = sorted(x[k] for x in T); print('  %-24s min %.2f mean %.2f median %.2f p90 %.2f max %.2f' % (n, v[0], st.mean(v), v[len(v) // 2], v[int(len(v) * .9)], v[-1]))
def classify(r):
    if 'all' not in r: return None
    if r.get('sw', 0) <= 130 and r['all'] <= 40: return 'idle (still screen)'
    if r['all'] >= 300: return 'heavy (>=300 % CPU)'
    return 'use'
cls = collections.defaultdict(list)
for r in R:
    c = classify(r)
    if c and r['T']: cls[c].append(r)
print('\n== rounds by class: clocks against load (guest CPU is of 600 %; fps = swaps a second while moving)')
print('  class                 rounds  CPU%%  fps    f0 GHz  f6 GHz  GPU MHz busy%%  c0 C')
for c, rs in cls.items():
    print('  %-20s  %5d  %4.0f  %5.1f  %5.2f   %5.2f   %5.0f   %4.1f  %5.1f' % (c, len(rs), st.mean(r['all'] for r in rs), st.mean(r.get('fps', 0) for r in rs if r.get('fps')) if any(r.get('fps') for r in rs) else 0,
          st.mean(r['f0'] for r in rs), st.mean(r['f6'] for r in rs), st.mean(r['g'] for r in rs), st.mean(r['gbusy'] for r in rs), st.mean(r['c0'] for r in rs)))
# residency of the sampled clocks, per class
def resid(key, steps):
    print('\n== %s: share of samples at each clock, per class' % key)
    for c, rs in cls.items():
        s = [x[key] for r in rs for x in r['T']]; n = len(s)
        print('  %-20s' % c, ' '.join('%s %2.0f%%' % (lab, 100 * sum(1 for v in s if lo <= v < hi) / n) for lab, lo, hi in steps))
resid('f0', [('<1.0', 0, 1.0), ('1.0-1.6', 1.0, 1.6), ('1.6-2.3', 1.6, 2.3), ('2.3-3.0', 2.3, 3.0), ('3.0-3.53', 3.0, 9)])
resid('f6', [('<1.1', 0, 1.1), ('1.1-2.0', 1.1, 2.0), ('2.0-3.0', 2.0, 3.0), ('3.0-4.0', 3.0, 4.0), ('4.0-4.47', 4.0, 9)])
resid('g', [('160', 0, 200), ('222', 200, 300), ('342-443', 300, 500), ('525-607', 500, 650), ('660-900', 650, 950), ('967-1200', 950, 9999)])
# ---- the host's view: each vCPU thread's run time between thread tables, against the clock sampled in the same span
print('\n== host: vCPU threads between thread tables (run %% of wall; clock = mean of the samples in the span; need = clock x busiest/0.80)')
print('  span(UTC)          v0  v1  v2  v3 | v4  v5 | v_gpu blk  | f0   need0 | f6   need6 | guest all%  fps')
waste = []
for a, b in zip(P, P[1:]):
    dt = b['t'] - a['t']
    if dt <= 0: continue
    def run(name, idx=None):
        tot = 0
        for (tid, comm), v in b['th'].items():
            if comm.startswith(name) and (tid, comm) in a['th']: tot += v[0] - a['th'][(tid, comm)][0]
        return 100 * tot / 1e9 / dt
    v = [run('crosvm_vcpu%d' % i) for i in range(6)]
    s = [x for x in T if a['t'] <= x['t'] < b['t']]
    if not s: continue
    f0 = st.mean(x['f0'] for x in s); f6 = st.mean(x['f6'] for x in s)
    rs = [r for r in R if a['t'] < r['t'] <= b['t'] and 'all' in r]
    ga = st.mean(r['all'] for r in rs) if rs else 0; fps = st.mean(r['fps'] for r in rs if r.get('fps')) if any(r.get('fps') for r in rs) else 0
    n0 = f0 * max(v[:4]) / 80; n6 = f6 * max(v[4:]) / 80
    waste.append((dt, f0, n0, f6, n6))
    def f(t): return '%02d:%02d:%02d' % (t // 3600, t // 60 % 60, t % 60)
    print('  %s-%s %3.0f %3.0f %3.0f %3.0f | %3.0f %3.0f | %3.0f  %3.0f  | %.2f %.2f  | %.2f %.2f  | %4.0f      %5.1f' % (f(a['t']), f(b['t'])[3:], *v, run('v_gpu'), run('virtio_blk'), f0, n0, f6, n6, ga, fps))
tot = sum(w[0] for w in waste)
print('  time-weighted: policy0 ran at %.2f GHz, the busiest core needed %.2f; policy6 ran at %.2f, needed %.2f (minute means: bursts inside a minute need more)' % (
    sum(w[0] * w[1] for w in waste) / tot, sum(w[0] * w[2] for w in waste) / tot, sum(w[0] * w[3] for w in waste) / tot, sum(w[0] * w[4] for w in waste) / tot))
# ---- GPU: kgsl's own time in each clock over the session, and the frames' GPU time
if 'start' in kg and 'end' in kg:
    d = {k: kg['end'][k] - kg['start'].get(k, 0) for k in kg['end']}; tot = sum(d.values()) or 1
    print('\n== GPU clock residency (kgsl trans_stat, end minus start, %d s): ' % (tot // 1000) + ' '.join('%d MHz %.1f%%' % (k, 100 * d[k] / tot) for k in sorted(d) if d[k]))
gf = sum(r.get('gfr', 0) for r in R); gh = [sum(r['gh'][i] for r in R if 'gh' in r) for i in range(7)]
print('== GPU time of a frame (guest submit to host done): frames %d, <1/<2/<4/<8/<16/<32/longer ms: %s = over the 8.33 ms budget %.1f%%' % (gf, '/'.join(map(str, gh)), 100 * sum(gh[4:]) / max(1, sum(gh))))
mv = [r for r in R if r.get('fps') and r.get('sw', 0) > 200]
g = [sum(r['gaps'][i] for r in mv) for i in range(5)]
print('== swaps while moving (rounds with >200 swaps: %d): apart by <1/1/2/more refreshes: %s; on time (<=1) %.1f%%' % (len(mv), '/'.join(map(str, g[:4])), 100 * (g[0] + g[1]) / max(1, sum(g[:4]))))
lk = [r for r in R if 'looks' in r]
print('== GPU fence polling: looks a round mean %.0f (max %d), waits mean %.0f; looks per wait %.1f' % (st.mean(r['looks'] for r in lk), max(r['looks'] for r in lk), st.mean(r['waits'] for r in lk), sum(r['looks'] for r in lk) / max(1, sum(r['waits'] for r in lk))))
top = collections.Counter(); n = 0
for r in R:
    if 'all' in r:
        n += 1
        for p, v in r['procs'].items(): top[p] += v
print('== guest CPU by process, mean %% over %d rounds (of 600): ' % n + ', '.join('%s %.1f' % (p, v / n) for p, v in top.most_common(10)) + '; all %.0f' % st.mean(r['all'] for r in R if 'all' in r))
if show_rounds:
    print('\n== rounds: n UTC all% sys% fps swaps f0 f6 gMHz gbusy c0 looks top')
    for r in R:
        if 'all' in r: print(' %3d %02d:%02d:%02d %4d %3d %5.1f %4d %.2f %.2f %4.0f %3.0f %5.1f %6d %s' % (r['n'], r['t'] // 3600, r['t'] // 60 % 60, r['t'] % 60, r['all'], r.get('sys', 0), r.get('fps', 0), r.get('sw', 0), r['f0'], r['f6'], r['g'], r['gbusy'], r['c0'], r.get('looks', 0), ' '.join('%s:%d' % kv for kv in sorted(r['procs'].items(), key=lambda kv: -kv[1])[:3])))
