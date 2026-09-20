#!/usr/bin/env python3
"""Absolute look metrics for one or two DarkPlaces TGA screenshots.

tgacmp.py answers "how far apart are these two frames"; this answers "what does
each frame actually look like", which is what METAL.md's Phase 5 acceptance row
asks for -- **frame mean within 1%** is a ratio of two absolutes, and a delta
metric cannot express it.

Reports per frame: per-channel means, mean luminance, p99 luminance, and the
flat-white percentage (all of R, G and B >= 250 -- the canary the RT
wall-lighting rounds use, because a composite that reports active without
drawing renders raw albedo and blinds the frame).

With two frames it adds the signed per-channel difference and the frame-mean
difference as a PERCENTAGE of the first frame, which is the acceptance number.

Exit status: 0 if the frame-mean difference is within --tol per cent (default
1.0), 1 otherwise. With one frame it always exits 0 -- there is nothing to gate.
"""
import sys, argparse
from tgacmp import load

def stats(path):
    w, h, px, rows = load(path)
    n = w * h
    sr = sg = sb = 0
    white = 0
    lum = []
    for row in rows:
        for x in range(0, w * px, px):
            b, g, r = row[x], row[x + 1], row[x + 2]
            sr += r; sg += g; sb += b
            if r >= 250 and g >= 250 and b >= 250:
                white += 1
            # Rec.601 luma, integer-weighted to keep this readable in a log
            lum.append((r * 299 + g * 587 + b * 114) // 1000)
    lum.sort()
    return {
        'w': w, 'h': h, 'n': n,
        'r': sr / n, 'g': sg / n, 'b': sb / n,
        'mean': (sr + sg + sb) / (3.0 * n),
        'lum': sum(lum) / float(n),
        'p99': lum[int(0.99 * (n - 1))],
        'white': 100.0 * white / n,
    }

def show(tag, s):
    print('  %-6s  R %6.2f  G %6.2f  B %6.2f | mean %6.2f  luma %6.2f  p99 %3d  flat-white %.3f%%'
          % (tag, s['r'], s['g'], s['b'], s['mean'], s['lum'], s['p99'], s['white']))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a'); ap.add_argument('b', nargs='?')
    ap.add_argument('--tol', type=float, default=1.0,
                    help='frame-mean tolerance in per cent (METAL.md Phase 5: 1%%)')
    ap.add_argument('--labels', default='A,B')
    args = ap.parse_args()
    la, lb = (args.labels.split(',') + ['B'])[:2]

    sa = stats(args.a)
    show(la, sa)
    if not args.b:
        return 0
    sb = stats(args.b)
    show(lb, sb)

    # per cent OF THE FIRST FRAME. Guard the zero case rather than dividing by
    # it: a black frame is a real outcome here (a refused path, a dead bed) and
    # it should read as a loud failure, not a ZeroDivisionError.
    if sa['mean'] <= 0.0001:
        print('  delta   %s frame mean is ~0 -- nothing to take a ratio of' % la)
        return 1
    dm = 100.0 * (sb['mean'] - sa['mean']) / sa['mean']
    print('  delta   R %+6.2f  G %+6.2f  B %+6.2f | frame mean %+.3f%%  flat-white %+.3f pp'
          % (sb['r'] - sa['r'], sb['g'] - sa['g'], sb['b'] - sa['b'], dm, sb['white'] - sa['white']))
    ok = abs(dm) <= args.tol
    print('  gate    frame mean within %.1f%%%s : %s' % (args.tol, '', 'PASS' if ok else 'FAIL'))
    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main())
