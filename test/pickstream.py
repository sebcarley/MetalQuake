#!/usr/bin/env python3
# pickstream.py -- decorrelation probe for the stochastic light-pick stream
# (rt_metal_lightsample). Mirrors the RT_BLUENOISE 0 arm of rt_selrand in
# rt_metal.m EXACTLY (a deliberate mini-lockstep: change one, change both) and
# prints the mean absolute difference between adjacent pixels' pick uniforms.
# For an i.i.d. uniform stream that statistic is 1/3; the pre-2026-08-28 IGN
# arm measured 0.0144 (neighbours pick the SAME light, so the estimator's
# error arrives as moving blotches -- the shape Seb's eye rejected on
# 2026-08-17). The probe validated itself by reproducing that recorded figure
# before the hash replacement was written. Pure stdlib on purpose (no numpy
# on this machine); the statistic is precision-insensitive.
#
# Usage: python3 test/pickstream.py    (no engine boot -- the stream is a
# pure function of pixel coordinates)
#
# The BLUE arm's pick tap (rt_selrand under RT_BLUENOISE 1: slice (frame + T/2)
# & (T-1), tap (x+11, y+29) of the committed table) is probed by
# test/bluenoise-check.py, which reads rt_bluenoise.h directly and reports the
# jitter/pick coupling and the tap's own spatial neighbour delta (2026-09-03:
# r = +0.005, delta 0.383 -- blue over the screen, decoupled from the jitter).

W, H = 640, 360   # trace-buffer geometry at 1280x720 * 0.5; statistic is size-independent
M32 = 0xFFFFFFFF

def fract(a):
    return a - int(a) if a >= 0 else a - int(a) + (1 if a != int(a) else 0)

def ign(x, y, k):
    # the retired arm, kept as the probe's own control
    inner = fract(0.11348 * (x + 7 * k) + 0.00937 * (y + 3 * k))
    return fract(52.9829189 * inner)

def pcg(v):
    s = (v * 747796405 + 2891336453) & M32
    w = (((s >> ((s >> 28) + 4)) ^ s) * 277803737) & M32
    return ((w >> 22) ^ w) & M32

def hash_arm(x, y, k, frame):
    h = pcg(x + pcg(y + pcg(k + pcg(frame))))
    return ((h >> 8) + 0.5) * (1.0 / 16777216.0)

def selseq(x, y, k, frame):
    # the MARCH kernels' stratified variant (rt_selseq): hashed base at k=0,
    # golden-ratio drift per cast -- one pixel's casts sweep the light CDF
    # quasi-evenly, which is what halved the fog EMA's speckle (2026-08-28)
    return (hash_arm(x, y, 0, frame) + k * 0.6180339887) % 1.0

def stat(f):
    # mean |horizontal neighbour difference|
    total = n = 0
    for y in range(H):
        prev = f(0, y)
        for x in range(1, W):
            v = f(x, y)
            total += abs(v - prev)
            prev = v
            n += 1
    return total / n

def stat_v(f):
    total = n = 0
    for x in range(0, W, 4):          # every 4th column is ample
        prev = f(x, 0)
        for y in range(1, H):
            v = f(x, y)
            total += abs(v - prev)
            prev = v
            n += 1
    return total / n

print("i.i.d. reference:            0.3333")
print(f"IGN arm (retired):           {stat(lambda x, y: ign(x, y, 0)):.4f}   (recorded 2026-08-17: 0.0144)")
hx = stat(lambda x, y: hash_arm(x, y, 0, 0))
print(f"hash arm (shipped):          {hx:.4f}")
print(f"hash arm, vertical:          {stat_v(lambda x, y: hash_arm(x, y, 0, 0)):.4f}")
# across k: adjacent march steps of one pixel must not pick together
tot = 0
for y in range(0, H, 4):
    for x in range(W):
        tot += abs(hash_arm(x, y, 1, 0) - hash_arm(x, y, 0, 0))
print(f"hash arm, adjacent k:        {tot / ((H // 4) * W):.4f}")
# selseq: spatial decorrelation must survive (the base is the hash), and the
# per-pixel cast sequence must be the golden-ratio lattice (adjacent-k
# fract-distance exactly min(phi-1, 2-phi) = 0.382)
sx = stat(lambda x, y: selseq(x, y, 0, 0))
tot = 0
for y in range(0, H, 4):
    for x in range(W):
        d = abs(selseq(x, y, 1, 0) - selseq(x, y, 0, 0))
        tot += min(d, 1.0 - d)
sk = tot / ((H // 4) * W)
print(f"selseq, horizontal:          {sx:.4f}")
print(f"selseq, adjacent-k lattice:  {sk:.4f}   (golden-ratio target 0.3820)")
ok = abs(hx - 1.0 / 3.0) < 0.01 and abs(sx - 1.0 / 3.0) < 0.01 and abs(sk - 0.3820) < 0.001
print("PASS" if ok else "FAIL")
