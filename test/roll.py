#!/usr/bin/env python3
"""test/roll.py -- the pattern-COHERENCE instrument (the MetalFX-temporal arc).

weave.py measures how LOUD the fog/shadow grain is. It cannot see whether that
grain is a coherent lattice that TRANSLATES every frame or featureless twinkle
of the same energy -- its own docstring says so, and that blindness is why two
sampling rounds (rt_metal_bluenoise, rt_metal_lightsample) were accepted by
every number in the tree and then rejected by Seb's eye in motion. This is the
missing discriminator.

METHOD. Take two CONSECUTIVE frames, high-pass each (subtract a 5x5 box mean,
which removes scene content and leaves the Nyquist-band grain), then compute
the normalised cross-correlation of the two high-passed crops over every
integer lag within +/-MAXLAG pixels. Read the map:

  peak at (0,0), r ~= 1    the pattern is STATIC and unchanging
  peak at (0,0), r < 1     the grid is STATIC but its CONTENTS re-randomise;
                           1-r is the fraction of grain replaced per frame
  peak OFF (0,0)           the pattern TRANSLATES, and the peak's lag IS the
                           per-frame step in pixels
  no peak anywhere         incoherent grain, or no grain at all

WHAT IT MEASURED AT HEAD c4565362 -- AND IT CORRECTED THE DIAGNOSIS. The
MetalFX-temporal plan was written on the model that the golden-ratio phase add
TRANSLATES the IGN lattice, so "the roll" would show as an off-origin peak.
On the frozen bed below, at Seb's own settings, it does not:

    arm                     zero-lag r      peak       hp energy
    IGN, phase advancing    0.865 / 0.903   at (0,0)   2.259
    IGN, RT_METAL_FRAMEPIN  1.0000          at (0,0)   3.082

The peak never leaves the origin, and r is FLAT at 0.86-0.92 for frame gaps of
1, 2, 3, 4, 5, 6, 7, 8 and 9 -- no decay, so the changing part is per-frame
INDEPENDENT rather than a pattern moving or evolving. So the artefact on a
parked camera is a STATIC screen-locked grid (the fog buffer's 8/3
magnification, fixed in screen space) whose CONTENTS twinkle by ~10-14% every
frame. What the eye reads as rolling is that twinkle on a fixed grid, not a
lattice sliding across the screen.

Two consequences worth carrying. (1) The acceptance test for a fix is
"zero-lag r rises towards 1 AND hp energy falls", not "the off-origin peak
collapses" -- there is no off-origin peak to collapse. (2) The phase advance
is already doing useful work: pinning it RAISES the standing lattice amplitude
by 27% (2.259 -> 3.082), because the fog EMA averaging successive phases is
what keeps it down. Anything that freezes the phase to stop the twinkle would
trade it for a stronger static grid.

THE NULL CONTROL IS MANDATORY, and here it is also the proof the bed is sound.
RT_METAL_FRAMEPIN=1 pins the kernels' jitter phase (rt_metal.m, s_framePin ->
cam.frame = 0) while the frame counter still advances. On a frozen scene with
the murk winds pinned it makes consecutive frames BYTE-IDENTICAL (r = 1.0000
to four decimals, hp energy equal to the last digit) -- which says the jitter
phase is the only thing changing frame to frame, so every difference the base
arm shows is attributable to it and to nothing else.

BED. test/rollbed.sh drives the frozen bed this docstring quotes. For frames
from a demo instead, RT_METAL_DUMPFRAMES at consecutive numbers:
  RT_METAL_DUMP=<path> RT_METAL_DUMPFRAMES=900,901,902,903 ./darkplaces-sdl \
     -benchmark demo14.dem ...
(<=16 frames per run; the counter counts timedemo playback frames only, and a
timedemo renders exactly one frame per demo packet, so consecutive dump frames
are consecutive RENDERED frames). Raw dumps and .tga screenshots both read.

MEASURE WHERE THE FRAME HAS RANGE. At Seb's archived gamma (v_gamma 0.5 plus
r_brightness) a fog crop sits in the bottom few levels and the high-passed
signal is the 8-bit quantisation floor -- measured, hp energy 0.28 of 255, and
every arm read INCOHERENT because there was nothing above the quantiser. The
bed script pins a neutral gamma for exactly the reason 6-5's look A/B re-took
its numbers that way. It is a measurement condition, not a look claim.

A PERIODIC PATTERN ALIASES. IGN's harmonics land at periods of 2.5-3 texels,
so a lattice correlates with itself at every multiple of its period and the
map carries several near-equal ridges -- the reported lag is the translation
only MODULO that period, and the top-lags line is there so the ridge structure
is visible rather than hidden behind one number. What is unambiguous is
whether the ORIGIN wins.

AND CHECK WHICH JITTER ARM THE BED BOOTED. rt_metal_bluenoise defaults to 1
and Seb archives 0; a bed that writes its own autoexec without copying his
config gets the blue-noise arm, whose slices cycle with period 8 -- which
showed up here as an exact r = 1.0000 at one particular frame gap and nowhere
else. The bed script pins the cvar explicitly for that reason.

CAMERA MOTION IS A CONFOUND. On a moving camera the SCENE translates between
frames, so everything decorrelates and the numbers shrink towards nothing
(measured on demo13's fog: hp energy 0.69, zero-lag r -0.02 base against +0.05
pinned -- the differential still has the right sign, but the magnitude is
gone). Use the frozen bed for the clean reading and compare any moving bed
against its own FRAMEPIN control, never against the frozen one.

Usage:
  python3 test/roll.py <f1> <f2> [f3 ...]            consecutive frames
  python3 test/roll.py <f1> <f2> --crop X Y W H      explicit crop
  python3 test/roll.py <f1> <f2> --maxlag 12
Pairs are formed from adjacent arguments; per-pair lines then a summary.
"""
import sys, os, math, operator

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from weave import load_any

MAXLAG = 8
BOX = 5           # high-pass box radius*2+1


def highpass(lum, x0, y0, cw, ch):
    """Crop, then subtract a BOXxBOX box mean. Separable, O(n). The box is
    taken from the FULL image where it reaches outside the crop, so the crop
    edges are not high-passed against their own clamped selves."""
    r = BOX // 2
    h = len(lum)
    w = len(lum[0])
    # horizontal pass over the rows the crop needs, plus the vertical halo
    ytop = max(0, y0 - r)
    ybot = min(h, y0 + ch + r)
    hor = []
    for y in range(ytop, ybot):
        row = lum[y]
        acc = []
        for x in range(x0, x0 + cw):
            a = max(0, x - r)
            b = min(w, x + r + 1)
            acc.append(sum(row[a:b]) / float(b - a))
        hor.append(acc)
    out = []
    for y in range(y0, y0 + ch):
        a = max(ytop, y - r)
        b = min(ybot, y + r + 1)
        n = float(b - a)
        cols = hor[a - ytop:b - ytop]
        mean = [sum(c[i] for c in cols) / n for i in range(cw)]
        out.append([lum[y][x0 + i] - mean[i] for i in range(cw)])
    return out


def prefix2(sq):
    """2D prefix sums of a list of rows, size (h+1) x (w+1)."""
    h = len(sq)
    w = len(sq[0])
    p = [[0.0] * (w + 1) for _ in range(h + 1)]
    for y in range(h):
        rowp = p[y]
        rown = p[y + 1]
        s = 0.0
        r = sq[y]
        for x in range(w):
            s += r[x]
            rown[x + 1] = rowp[x + 1] + s
    return p


def rectsum(p, x0, y0, x1, y1):
    """sum over [x0,x1) x [y0,y1)"""
    return p[y1][x1] - p[y1][x0] - p[y0][x1] + p[y0][x0]


def correlate(a, b, maxlag):
    """NCC of a against b over integer lags. Positive dx means b's copy of the
    pattern sits dx pixels to the RIGHT of a's, i.e. the pattern moved +dx.
    Denominators are exact over each lag's own overlap via prefix sums."""
    h = len(a)
    w = len(a[0])
    pa = prefix2([[v * v for v in r] for r in a])
    pb = prefix2([[v * v for v in r] for r in b])
    best = None
    grid = {}
    for dy in range(-maxlag, maxlag + 1):
        ay0 = max(0, -dy); ay1 = min(h, h - dy)
        for dx in range(-maxlag, maxlag + 1):
            ax0 = max(0, -dx); ax1 = min(w, w - dx)
            num = 0.0
            for y in range(ay0, ay1):
                ra = a[y]
                rb = b[y + dy]
                num += sum(map(operator.mul, ra[ax0:ax1], rb[ax0 + dx:ax1 + dx]))
            ea = rectsum(pa, ax0, ay0, ax1, ay1)
            eb = rectsum(pb, ax0 + dx, ay0 + dy, ax1 + dx, ay1 + dy)
            den = math.sqrt(ea * eb) if ea > 0 and eb > 0 else 0.0
            r = num / den if den > 0 else 0.0
            grid[(dx, dy)] = r
            if best is None or r > best[0]:
                best = (r, dx, dy)
    return grid, best


def energy(a):
    n = len(a) * len(a[0])
    return math.sqrt(sum(v * v for r in a for v in r) / n)


def analyse(fa, fb, crop, maxlag):
    wa, ha, la = load_any(fa)
    wb, hb, lb = load_any(fb)
    if (wa, ha) != (wb, hb):
        raise SystemExit('roll: %s is %dx%d but %s is %dx%d' % (fa, wa, ha, fb, wb, hb))
    if crop:
        x0, y0, cw, ch = crop
    else:
        cw = min(192, wa // 2); ch = min(192, ha // 2)
        x0 = (wa - cw) // 2; y0 = (ha - ch) // 2
    if x0 < 0 or y0 < 0 or x0 + cw > wa or y0 + ch > ha:
        raise SystemExit('roll: crop %dx%d+%d+%d exceeds %dx%d' % (cw, ch, x0, y0, wa, ha))
    a = highpass(la, x0, y0, cw, ch)
    b = highpass(lb, x0, y0, cw, ch)
    grid, best = correlate(a, b, maxlag)
    r0 = grid[(0, 0)]
    peak, pdx, pdy = best
    # strongest lag that is NOT the origin -- the roll candidate
    off = max(((v, k) for k, v in grid.items() if k != (0, 0)))
    top = sorted(grid.items(), key=lambda kv: -kv[1])[:3]
    return dict(crop=(x0, y0, cw, ch), ea=energy(a), eb=energy(b),
                r0=r0, peak=peak, pdx=pdx, pdy=pdy,
                offr=off[0], offdx=off[1][0], offdy=off[1][1], top=top, grid=grid)


def verdict(d):
    if d['peak'] < 0.10:
        return 'INCOHERENT (no pattern survives one frame -- twinkle or clean)'
    if (d['pdx'], d['pdy']) == (0, 0):
        return 'STATIC (pattern does not translate)'
    return 'ROLLING by (%+d,%+d) px/frame' % (d['pdx'], d['pdy'])


def main(argv):
    crop = None
    maxlag = MAXLAG
    files = []
    i = 0
    while i < len(argv):
        if argv[i] == '--crop':
            crop = tuple(int(v) for v in argv[i + 1:i + 5]); i += 5
        elif argv[i] == '--maxlag':
            maxlag = int(argv[i + 1]); i += 2
        else:
            files.append(argv[i]); i += 1
    if len(files) < 2:
        raise SystemExit(__doc__)
    rows = []
    for k in range(len(files) - 1):
        d = analyse(files[k], files[k + 1], crop, maxlag)
        rows.append(d)
        x0, y0, cw, ch = d['crop']
        print('%s -> %s  crop %dx%d+%d+%d' %
              (os.path.basename(files[k]), os.path.basename(files[k + 1]), cw, ch, x0, y0))
        print('  hp energy  %.4f / %.4f      zero-lag r %.4f' % (d['ea'], d['eb'], d['r0']))
        print('  peak       r %.4f at (%+d,%+d)   best off-origin r %.4f at (%+d,%+d)  |lag| %.2f px' %
              (d['peak'], d['pdx'], d['pdy'], d['offr'], d['offdx'], d['offdy'],
               math.hypot(d['offdx'], d['offdy'])))
        print('  top lags   %s' % '  '.join('(%+d,%+d) %.3f' % (k[0], k[1], v) for k, v in d['top']))
        print('  verdict    %s' % verdict(d))
    if len(rows) > 1:
        def med(vals):
            s = sorted(vals); n = len(s)
            return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])
        print('summary over %d pairs: median peak r %.4f, median zero-lag r %.4f, median hp energy %.4f'
              % (len(rows), med([r['peak'] for r in rows]), med([r['r0'] for r in rows]),
                 med([r['ea'] for r in rows])))
        lags = [(r['pdx'], r['pdy']) for r in rows]
        print('  peak lags: %s' % ' '.join('(%+d,%+d)' % l for l in lags))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
