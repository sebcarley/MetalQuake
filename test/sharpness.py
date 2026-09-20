#!/usr/bin/env python3
"""Acutance proxy for DarkPlaces TGA screenshots (uncompressed truecolour).

Written for METAL.md Phase 8-5's MetalFX acceptance -- the claim "sharper than
bilinear at equal cost" needs a number, and until this file the tree had no
sharpness instrument at all (lookmetrics is means/luma/p99, tgacmp is per-pixel
deltas; neither can rank two upscales of the same frame).

Two standard proxies over Rec.601 luma, both higher-is-sharper:
  - grad : mean central-difference gradient magnitude (|dx| + |dy|)
  - lap  : mean absolute 4-neighbour Laplacian

Both are RELATIVE instruments: they rank renderings of the SAME scene from the
SAME camera. Comparing across scenes or cameras is meaningless -- a busier
frame scores higher with no sharpness change. And more is not always better in
the absolute: an oversharpened ringing image out-scores the native frame too,
so the honest reading for an upscaler is "closer to native than the bilinear
arm is", not "biggest number wins".

Usage:
  python3 test/sharpness.py native.tga bilinear.tga metalfx.tga [--crop x,y,w,h]

Prints one line per file plus each file's two metrics as a percentage of the
FIRST file's (the reference -- pass native first). --crop restricts to a
rectangle (origin top-left of the on-screen image; rows are un-flipped from
the TGA's bottom-up order first), which is how to aim it at detail and keep
the HUD -- identical in every arm and native-res under MetalFX by design --
from diluting the comparison.
"""
import sys, argparse
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from tgacmp import load


def luma_rows(w, h, bpp, tgarows):
    # tgacmp's load() hands back rows already normalised TOP-DOWN, one bytes
    # object per row, BGR(A) order -- so --crop coordinates read like the
    # on-screen image with no flipping here
    rows = []
    for tr in tgarows:
        row = []
        for x in range(w):
            o = x * bpp
            b, g, r = tr[o], tr[o + 1], tr[o + 2]
            row.append((299 * r + 587 * g + 114 * b) // 1000)
        rows.append(row)
    return rows


def metrics(rows, cx, cy, cw, ch):
    gsum = 0
    lsum = 0
    n = 0
    for y in range(max(cy, 1), min(cy + ch, len(rows) - 1)):
        up, row, dn = rows[y - 1], rows[y], rows[y + 1]
        for x in range(max(cx, 1), min(cx + cw, len(row) - 1)):
            c = row[x]
            gsum += abs(row[x + 1] - row[x - 1]) + abs(dn[x] - up[x])
            lsum += abs(row[x - 1] + row[x + 1] + up[x] + dn[x] - 4 * c)
            n += 1
    if not n:
        raise SystemExit('sharpness: empty crop')
    return gsum / n, lsum / n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='+')
    ap.add_argument('--crop', help='x,y,w,h in top-down image coordinates')
    args = ap.parse_args()

    ref = None
    for path in args.files:
        w, h, bpp, tgarows = load(path)
        cx, cy, cw, ch = 0, 0, w, h
        if args.crop:
            cx, cy, cw, ch = (int(v) for v in args.crop.split(','))
        rows = luma_rows(w, h, bpp, tgarows)
        g, l = metrics(rows, cx, cy, cw, ch)
        if ref is None:
            ref = (g, l)
            print('%-28s grad %7.4f  lap %7.4f  (reference)' % (path.rsplit('/', 1)[-1], g, l))
        else:
            print('%-28s grad %7.4f  lap %7.4f  (%.1f%% / %.1f%% of reference)'
                  % (path.rsplit('/', 1)[-1], g, l, 100.0 * g / ref[0], 100.0 * l / ref[1]))


if __name__ == '__main__':
    main()
