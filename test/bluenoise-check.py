#!/usr/bin/env python3
"""test/bluenoise-check.py -- validate a blue-noise jitter table OFFLINE, before
any boot (BLUENOISE slice 1, 2026-09-03; the FOGLIGHT pickstream precedent:
simulate before code). Reads the C header(s) and reports, for each:

  spatial   per-slice radially-binned power spectrum (fraction of non-DC
            power below radius 4 / 8 / 16 of 32): blue noise has next to
            nothing in the low bands
  temporal  per-texel T-frame sequence (T = 8 or 16), toroidal: mean |consecutive
            difference| (i.i.d. uniform = 1/3; blue-in-time is HIGHER) and the
            share of temporal power at the lowest non-DC frequency (period 8)
            (period T) against the top one (period 2): white ~ equal, blue ~ top-heavy
  ema       the number that decides the session -- a synthetic integrand of
            the jitter (a zero-mean sawtooth phase error, sin 2 pi u) run
            through the fog EMA at history 0.7 and 0.9, cycling the table's
            slices exactly as the kernels do (frame & 7): the settled std over
            texels and frames, against an i.i.d. hash stream. Lower is better.
            NOTE a T-slice cycle bounds what an EMA can average: a period-T
            sequence cannot read below its own T-value mean spread, which is
            why the old 8-slice table reads WORSE than i.i.d. at history 0.9.
  pick      the same EMA on the fog light PICK: a 60/25/10/5 light split with
            the 25% light shadowed, eight stratified casts per frame
            (rt_selseq, base from the table's pick tap exactly as rt_selrand's
            blue arm reads it), estimate = sum(c) * vis(pick). Lower is better.
  coupling  Pearson r between the jitter tap bn[f][y][x] and the pick tap
            bn[(f+T/2)&(T-1)][(y+29)&63][(x+11)&63] over all texels and frames, and
            the pick tap's own spatial neighbour delta -- the two streams must
            not move together (the coupling rt_metal.m's comment block warns
            about).

Usage: python3 test/bluenoise-check.py rt_bluenoise.h [candidate.h ...]
Pure stdlib; ~20 s per table.
"""
import math, re, sys, cmath

N = 64; CELLS = N * N; SLICES = 8

def load_table(path):
    t = open(path).read()
    m = re.search(r'rt_bluenoise64\[\d+\]\[\d+\]\s*=\s*\{(.*)\};', t, re.S)
    body = m.group(1)
    slices = []
    for blk in re.findall(r'\{([^{}]*)\}', body):
        vals = [int(v) for v in re.findall(r'\d+', blk)]
        assert len(vals) == CELLS, len(vals)
        slices.append(vals)
    assert len(slices) in (8, 16), len(slices)
    return slices

def u(slices, s, x, y):
    # the kernels' read: slice (frame & (T-1)), texel wrapped at 64
    return (slices[s & (len(slices) - 1)][(y & 63) * N + (x & 63)] + 0.5) / 256.0

# ---------------------------------------------------------------- spectrum
def spectrum(vals):
    # separable 2D DFT of the centred slice; returns power by radius band
    tw = [cmath.exp(-2j * math.pi * k / N) for k in range(N)]
    v = [[(vals[y * N + x] + 0.5) / 256.0 - 0.5 for x in range(N)] for y in range(N)]
    rows = []
    for y in range(N):
        r = []
        for kx in range(N):
            acc = 0j
            for x in range(N):
                acc += v[y][x] * tw[(kx * x) % N]
            r.append(acc)
        rows.append(r)
    bands = {4: 0.0, 8: 0.0, 16: 0.0, 33: 0.0}
    total = 0.0
    for kx in range(N):
        for ky in range(N):
            acc = 0j
            for y in range(N):
                acc += rows[y][kx] * tw[(ky * y) % N]
            if kx == 0 and ky == 0:
                continue
            fx = kx if kx < N // 2 else kx - N
            fy = ky if ky < N // 2 else ky - N
            r = math.hypot(fx, fy)
            p = abs(acc) ** 2
            total += p
            for b in (4, 8, 16, 33):
                if r < b:
                    bands[b] += p; break
    return {b: bands[b] / total for b in bands}

# ---------------------------------------------------------------- temporal
def temporal(slices):
    T = len(slices)
    diff = 0.0; n = 0
    plow = 0.0; ptop = 0.0
    tw = [cmath.exp(-2j * math.pi * k / T) for k in range(T)]
    for i in range(CELLS):
        seq = [(slices[s][i] + 0.5) / 256.0 for s in range(T)]
        for s in range(T):
            diff += abs(seq[s] - seq[(s + 1) % T]); n += 1
        c = [x - 0.5 for x in seq]
        f1 = abs(sum(c[t] * tw[t] for t in range(T))) ** 2                       # period T (lowest)
        f4 = abs(sum(c[t] * tw[((T // 2) * t) % T] for t in range(T))) ** 2      # period 2 (Nyquist)
        plow += f1; ptop += f4
    return diff / n, plow / (plow + ptop)

# ---------------------------------------------------------------- hash i.i.d.
M32 = 0xFFFFFFFF
def pcg(v):
    s = (v * 747796405 + 2891336453) & M32
    w = (((s >> ((s >> 28) + 4)) ^ s) * 277803737) & M32
    return ((w >> 22) ^ w) & M32
def hash_u(x, y, k, frame):
    h = pcg(x + pcg(y + pcg(k + pcg(frame))))
    return ((h >> 8) + 0.5) * (1.0 / 16777216.0)

# ---------------------------------------------------------------- EMA sims
FRAMES = 96          # 6-12 full cycles; the first 32 frames are the transient
def ema_std(sample, h):
    # sample(x, y, frame) -> per-frame estimate; std of the settled EMA over
    # a 64x64 texel field and frames 24..63
    vals = []
    for y in range(N):
        for x in range(N):
            e = 0.0
            for f in range(FRAMES):
                s = sample(x, y, f)
                e = h * e + (1.0 - h) * s if f > 0 else s
                if f >= 32:
                    vals.append(e)
    m = sum(vals) / len(vals)
    return math.sqrt(sum((v - m) ** 2 for v in vals) / len(vals))

def jitter_sim(slices, h):
    if slices is None:
        return ema_std(lambda x, y, f: math.sin(2 * math.pi * hash_u(x, y, 0, f)), h)
    return ema_std(lambda x, y, f: math.sin(2 * math.pi * u(slices, f, x, y)), h)

LIGHTS = [(0.60, 1.0), (0.25, 0.0), (0.10, 1.0), (0.05, 1.0)]   # (contribution, visibility)
CDF = []
_a = 0.0
for _c, _v in LIGHTS:
    _a += _c; CDF.append(_a)
PHI = 0.6180339887
def pick_estimate(base):
    # eight casts at step indices 0,3,6,...: rt_selseq(k = 1 + i) = fract(base + k*phi)
    tot = 0.0
    for m in range(8):
        k = 1 + 3 * m
        uu = (base + k * PHI) % 1.0
        for j, c in enumerate(CDF):
            if uu <= c:
                tot += LIGHTS[j][1]; break
    return tot / 8.0
def pick_base_table(slices, x, y, f):
    # rt_selrand's blue arm at k = 0: slice (f + T/2) & (T-1), tap (x+11, y+29)
    return u(slices, f + len(slices) // 2, x + 11, y + 29)
def pick_sim(slices, h):
    if slices is None:
        return ema_std(lambda x, y, f: pick_estimate(hash_u(x, y, 0, f)), h)
    return ema_std(lambda x, y, f: pick_estimate(pick_base_table(slices, x, y, f)), h)

# ---------------------------------------------------------------- coupling
def coupling(slices):
    xs = []; ys = []
    for f in range(len(slices)):
        for y in range(N):
            for x in range(N):
                xs.append(u(slices, f, x, y)); ys.append(pick_base_table(slices, x, y, f))
    n = len(xs); mx = sum(xs) / n; my = sum(ys) / n
    sxy = sum((a - mx) * (b - my) for a, b in zip(xs, ys))
    sxx = sum((a - mx) ** 2 for a in xs); syy = sum((b - my) ** 2 for b in ys)
    r = sxy / math.sqrt(sxx * syy)
    # pick tap spatial neighbour delta (pickstream's statistic)
    d = 0.0; n2 = 0
    for f in range(len(slices)):
        for y in range(N):
            for x in range(N - 1):
                d += abs(pick_base_table(slices, x, y, f) - pick_base_table(slices, x + 1, y, f)); n2 += 1
    return r, d / n2

def report(name, slices):
    print('== %s' % name)
    if slices is not None:
        bands = [spectrum(s) for s in slices]
        for b in (4, 8, 16):
            vals = [bd[b] for bd in bands]
            print('  spatial power below r=%2d: mean %.4f  (slices %s)' % (b, sum(vals) / len(vals), ' '.join('%.3f' % v for v in vals)))
        td, tl = temporal(slices)
        print('  temporal |consecutive diff| %.4f  (i.i.d. 0.3333; higher = bluer in time)' % td)
        print('  temporal power share at period %d vs period 2: %.3f  (white 0.5; lower = bluer in time)' % (len(slices), tl))
        r, nd = coupling(slices)
        print('  jitter/pick coupling r = %+.4f ; pick tap spatial neighbour delta %.4f' % (r, nd))
    for h in (0.7, 0.9):
        print('  EMA h=%.1f  jitter-integrand std %.4f   pick-estimate std %.4f' % (h, jitter_sim(slices, h), pick_sim(slices, h)))

if __name__ == '__main__':
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    report('i.i.d. PCG hash (the IGN-arm pick stream / a white reference)', None)
    for p in sys.argv[1:]:
        report(p, load_table(p))
