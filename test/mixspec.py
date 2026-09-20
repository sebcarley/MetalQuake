#!/usr/bin/env python3
# test/mixspec.py -- the sound bed's reader (SEPTEMBER2 Part B, 2026-09-06).
#
# Reads the WAV that SND_DUMPMIX=<path> writes from a -simsound timedemo (the
# whole stereo mix, the device format) and answers the three questions the
# brief set: is the off switch byte-identical (cmp/md5 does that); is the DRY
# level at each loud onset unchanged with the effect on ('compare' reports the
# per-second RMS of both files and the level at the loudest onsets); and does
# the on arm's tail decay the way the room says ('tail' fits dB/s after an
# onset) with its top end where the cvar puts it ('bands' splits each second
# into <300 Hz / 300-3000 / >3 kHz shares with a radix-2 FFT). Pure Python
# (this machine has no numpy): 4096-sample windows, one per half second.
#
#   python3 test/mixspec.py stats a.wav
#   python3 test/mixspec.py compare a.wav b.wav        # a = off/control, b = on
#   python3 test/mixspec.py tail a.wav [t_onset_s]     # decay after the loudest onset (or at t)
#   python3 test/mixspec.py difftail dry.wav on.wav [t] # the WET ALONE: decay of (on - dry) after the dry's loudest onset --
#                                                      # the room's own tail, which the game's next sound cannot pollute
import sys, struct, math, cmath

def read_wav(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'RIFF' and d[8:12] == b'WAVE', 'not a WAV'
    pos = 12; fmt = None; data = None
    while pos + 8 <= len(d):
        cid = d[pos:pos+4]; n = struct.unpack('<I', d[pos+4:pos+8])[0]
        body = d[pos+8:pos+8+n]
        if cid == b'fmt ': fmt = struct.unpack('<HHIIHH', body[:16])
        elif cid == b'data': data = body
        pos += 8 + n + (n & 1)
    tag, ch, rate, _, blk, bits = fmt
    n = len(data) // blk
    if tag == 3 and bits == 32:
        vals = struct.unpack('<%df' % (n * ch), data[:n * ch * 4])
    elif bits == 16:
        vals = [v / 32768.0 for v in struct.unpack('<%dh' % (n * ch), data[:n * ch * 2])]
    else:
        raise SystemExit('unsupported WAV format %r' % (fmt,))
    return rate, ch, n, vals

def mono(vals, ch, n):
    if ch == 1: return list(vals)
    return [(vals[i*ch] + vals[i*ch+1]) * 0.5 for i in range(n)]

def rms(seq):
    return math.sqrt(sum(x*x for x in seq) / max(1, len(seq)))

def fft(x):
    n = len(x)
    if n == 1: return x
    e = fft(x[0::2]); o = fft(x[1::2])
    out = [0j] * n
    for k in range(n // 2):
        t = cmath.exp(-2j * math.pi * k / n) * o[k]
        out[k] = e[k] + t; out[k + n//2] = e[k] - t
    return out

def bands(seg, rate):
    n = 4096
    w = [seg[i] * (0.5 - 0.5 * math.cos(2 * math.pi * i / n)) for i in range(n)]
    sp = fft([complex(v) for v in w])
    lo = mid = hi = 0.0
    for k in range(1, n // 2):
        f = k * rate / n; p = abs(sp[k]) ** 2
        if f < 300: lo += p
        elif f < 3000: mid += p
        else: hi += p
    tot = lo + mid + hi + 1e-20
    return lo / tot, mid / tot, hi / tot

def per_second(m, rate):
    out = []
    for s in range(len(m) // rate):
        seg = m[s*rate:(s+1)*rate]
        r = rms(seg)
        b = bands(seg[:4096], rate) if len(seg) >= 4096 and r > 1e-4 else (0, 0, 0)
        out.append((s, r, b))
    return out

def onsets(m, rate, count=5):
    # the loudest 50 ms windows, at least 1 s apart
    win = rate // 20
    e = [(rms(m[i:i+win]), i / rate) for i in range(0, len(m) - win, win)]
    e.sort(reverse=True)
    picked = []
    for r, t in e:
        if all(abs(t - p[1]) > 1.0 for p in picked): picked.append((r, t))
        if len(picked) >= count: break
    return sorted(picked, key=lambda p: p[1])

def db(x): return 20 * math.log10(max(x, 1e-9))

mode = sys.argv[1]
if mode == 'stats':
    rate, ch, n, v = read_wav(sys.argv[2]); m = mono(v, ch, n)
    print(f'{sys.argv[2]}: {rate} Hz, {ch} ch, {n/rate:.1f} s, overall rms {db(rms(m)):.1f} dBFS')
    for s, r, b in per_second(m, rate):
        print(f'  t={s:3d}s rms {db(r):6.1f} dB  bands lo {100*b[0]:4.1f}% mid {100*b[1]:4.1f}% hi {100*b[2]:4.1f}%')
elif mode == 'compare':
    ra, ca, na, va = read_wav(sys.argv[2]); rb, cb, nb, vb = read_wav(sys.argv[3])
    assert ra == rb and ca == cb, 'format differs'
    ma = mono(va, ca, na); mb = mono(vb, cb, nb)
    n = min(len(ma), len(mb))
    print(f'lengths {na} / {nb} frames ({"equal" if na == nb else "DIFFER"}); overall rms {db(rms(ma[:n])):.1f} vs {db(rms(mb[:n])):.1f} dBFS')
    diff = [mb[i] - ma[i] for i in range(n)]
    print(f'difference signal rms {db(rms(diff)):.1f} dBFS ({100*rms(diff)/max(rms(ma[:n]),1e-9):.1f}% of a)')
    print('loudest onsets in a (50 ms): dry level a vs b')
    for r, t in onsets(ma, ra):
        i = int(t * ra); w = ra // 20
        print(f'  t={t:6.2f}s  a {db(rms(ma[i:i+w])):6.1f} dB  b {db(rms(mb[i:i+w])):6.1f} dB  ({db(rms(mb[i:i+w])) - db(rms(ma[i:i+w])):+.2f} dB)')
    pa = per_second(ma, ra); pb = per_second(mb, rb)
    hia = [b[2] for _, r, b in pa if r > 1e-3]; hib = [b[2] for _, r, b in pb if r > 1e-3]
    if hia and hib: print(f'mean high-band share (>3 kHz): a {100*sum(hia)/len(hia):.2f}%  b {100*sum(hib)/len(hib):.2f}%')
elif mode in ('tail', 'difftail'):
    rate, ch, n, v = read_wav(sys.argv[2]); m = mono(v, ch, n)
    argi = 3
    if mode == 'difftail':
        rb, cb, nb, vb = read_wav(sys.argv[3]); mb = mono(vb, cb, nb); argi = 4
        nn = min(len(m), len(mb)); dry = m
        m = [mb[i] - dry[i] for i in range(nn)]
        # onsets are the DRY's (the difference has no onset of its own); level relative to the dry onset
        t0 = float(sys.argv[argi]) if len(sys.argv) > argi else onsets(dry, rate, 1)[0][1]
        i0 = int(t0 * rate); w = rate // 50
        print(f'wet-alone tail after the dry onset at {t0:.2f}s (dry onset {db(rms(dry[i0:i0+w])):.1f} dBFS; 20 ms windows, dBFS):')
        pts = []
        for k in range(0, 60):
            seg = m[i0 + k*w:i0 + (k+1)*w]
            if len(seg) < w: break
            pts.append((k * 0.02, db(rms(seg))))
        for t, l in pts[::5]: print(f'  +{t:.2f}s {l:6.1f} dBFS')
        sel = [(t, l) for t, l in pts if 0.2 <= t <= 0.8]
        if len(sel) > 2:
            xm = sum(t for t, _ in sel) / len(sel); ym = sum(l for _, l in sel) / len(sel)
            slope = sum((t-xm)*(l-ym) for t, l in sel) / max(1e-9, sum((t-xm)**2 for t, _ in sel))
            print(f'  slope 200-800 ms: {slope:.0f} dB/s  -> RT60 ~ {60/max(1e-3,-slope):.2f} s; wet peak {max(l for _, l in pts):.1f} dBFS vs dry onset {db(rms(dry[i0:i0+w])):.1f}')
        sys.exit(0)
    if len(sys.argv) > argi: t0 = float(sys.argv[argi])
    else: t0 = onsets(m, rate, 1)[0][1]
    i0 = int(t0 * rate); w = rate // 50   # 20 ms windows
    print(f'tail after onset at {t0:.2f}s (20 ms windows, dB rel. onset):')
    ref = rms(m[i0:i0+w]); pts = []
    for k in range(0, 40):
        seg = m[i0 + k*w:i0 + (k+1)*w]
        if len(seg) < w: break
        pts.append((k * 0.02, db(rms(seg)) - db(ref)))
    for t, l in pts[::4]: print(f'  +{t:.2f}s {l:6.1f} dB')
    # slope over the 100-500 ms window: dB per second, and the RT60 it implies
    sel = [(t, l) for t, l in pts if 0.1 <= t <= 0.5]
    if len(sel) > 2:
        xm = sum(t for t, _ in sel) / len(sel); ym = sum(l for _, l in sel) / len(sel)
        slope = sum((t-xm)*(l-ym) for t, l in sel) / max(1e-9, sum((t-xm)**2 for t, _ in sel))
        print(f'  slope 100-500 ms: {slope:.0f} dB/s  -> RT60 ~ {60/max(1e-3,-slope):.2f} s')
