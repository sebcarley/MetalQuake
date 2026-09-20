#!/usr/bin/env python3
"""Compare two DarkPlaces TGA screenshots (uncompressed truecolour only).

The cross-backend parity comparator for the Metal arc (METAL.md). Committed to
test/ rather than living in a session scratchpad, per the lesson Phase 3's
record spells out: a parity gate that dies with the session is not an
instrument.

Reports both gate families:
  - the hard 2D gate  (max delta <= 2, >= 99.9% of pixels within 1)
  - the 3D world gate (mean <= 0.3, p99.9 <= 4, < 0.5% of pixels > 8)
plus a 16x16 block map so a mismatch says WHERE -- a vertical flip and a wrong
colour want opposite fixes, and a whole-frame number cannot tell them apart.

Exit status: 0 if the gate named by --gate passes, 1 otherwise.
"""
import sys, struct, argparse

def load(path):
    d = open(path, 'rb').read()
    idlen, imgtype = d[0], d[2]
    w, h = struct.unpack('<HH', d[12:16])
    bpp, desc = d[16], d[17]
    if imgtype != 2:
        raise SystemExit('%s: only uncompressed truecolour supported (type %d)' % (path, imgtype))
    off = 18 + idlen
    px = bpp // 8
    raw = d[off:off + w * h * px]
    rows = [raw[y * w * px:(y + 1) * w * px] for y in range(h)]
    if not (desc & 0x20):
        rows = rows[::-1]          # TGA default is bottom-up; normalise to top-down
    return w, h, px, rows

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a'); ap.add_argument('b')
    ap.add_argument('--gate', choices=['2d', 'world'], default='world',
                    help='which acceptance family decides the exit status')
    args = ap.parse_args()

    wa, ha, pa, ra = load(args.a)
    wb, hb, pb, rb = load(args.b)
    if (wa, ha) != (wb, hb):
        raise SystemExit('size mismatch: %dx%d vs %dx%d' % (wa, ha, wb, hb))
    n = wa * ha
    maxd = over1 = over2 = over8 = diff = 0
    totald = 0
    deltas = []
    BL = 16
    blocks = [[0] * BL for _ in range(BL)]
    for y in range(ha):
        A, B = ra[y], rb[y]
        by = y * BL // ha
        for x in range(wa):
            ia, ib = x * pa, x * pb
            d = 0
            for c in range(3):
                cd = abs(A[ia + c] - B[ib + c])
                totald += cd
                if cd > d:
                    d = cd
            deltas.append(d)
            if d:
                diff += 1
                blocks[by][x * BL // wa] += 1
            if d > maxd: maxd = d
            if d > 1: over1 += 1
            if d > 2: over2 += 1
            if d > 8: over8 += 1
    deltas.sort()
    p999 = deltas[min(n - 1, int(n * 0.999))]
    mean = totald / (3.0 * n)

    print('%s vs %s  (%dx%d, %d px)' % (args.a.split('/')[-1], args.b.split('/')[-1], wa, ha, n))
    print('  mean channel delta : %.4f' % mean)
    print('  max delta          : %d' % maxd)
    print('  p99.9 delta        : %d' % p999)
    print('  pixels differing   : %d (%.3f%%)' % (diff, 100.0 * diff / n))
    print('  delta > 1          : %d (%.4f%%)' % (over1, 100.0 * over1 / n))
    print('  delta > 8          : %d (%.4f%%)' % (over8, 100.0 * over8 / n))
    ok2d = maxd <= 2 and (n - over1) >= 0.999 * n
    okworld = mean <= 0.3 and p999 <= 4 and over8 < 0.005 * n
    print('  2D gate    (max<=2, >=99.9%% within 1)          : %s' % ('PASS' if ok2d else 'FAIL'))
    print('  world gate (mean<=0.3, p99.9<=4, <0.5%%>8)      : %s' % ('PASS' if okworld else 'FAIL'))
    if diff:
        print('  block map (per-16th shading 0-9, . = clean):')
        cells = n / (BL * BL)
        for row in blocks:
            print('    ' + ''.join('.' if v == 0 else ('%d' % min(9, int(9.0 * v / cells))) for v in row))
    sys.exit(0 if (ok2d if args.gate == '2d' else okworld) else 1)

if __name__ == '__main__':
    main()
