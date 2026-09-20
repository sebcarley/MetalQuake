#!/usr/bin/env python3
"""test/weave.py -- the dither-weave metric (the 2026-08-13 blue-noise arc).

Measures the Nyquist-band checkerboard energy of a crop: mean |response| to
the 2x2 kernel [[+1,-1],[-1,+1]] over luminance, normalised by the crop's
mean luminance so exposure shifts cannot masquerade as weave. This is the
kh-swirl session's instrument, recreated and committed -- the ~2px IGN weave
lands at Nyquist, which separates it from real cloud/fog detail. The
kh-swirl warning stands: a naive HF metric ROSE with resolution and was
discarded; always evaluate at OUTPUT resolution on a FIXED crop, and only
compare crops of identical geometry.

WHAT THIS METRIC CAN AND CANNOT SEE (measured 2026-08-13, the blue-noise
arc): it measures banding/grain AMPLITUDE and is valid for step-count and
scale comparisons (the fail-first grid: IGN weave rose 0.0209 -> 0.0269 as
fog steps fell 24 -> 8). It CANNOT distinguish a woven lattice from
featureless grain of equal energy -- blue-noise-vs-IGN read identical here
at every tier while 4x-magnified stills showed the lattice plainly gone,
because blue noise removes the COHERENCE, not the energy. Autocorrelation
and band-profiled variants were tried and defeated by bilinear-magnification
smear and camera motion; a pattern-coherence instrument does not exist in
this tree yet. For coherence questions, magnified stills and the eye are
the instrument of record.

Usage:
  python3 test/weave.py <file.tga> [x y w h]        one file, optional crop
  python3 test/weave.py <a.tga> <b.tga> [x y w h]   two files: b as % of a

Also accepts the RT_METAL_DUMP raw format ([int32 w][int32 h][RGBA8
bottom-up], .fNNN suffix) so it reads both instruments' output.
"""
import struct, sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tgacmp


def load_any(path):
    # returns (w, h, rows) where rows are top-down luminance lists
    with open(path, 'rb') as f:
        head = f.read(8)
    if path.endswith('.tga') or (len(head) >= 3 and head[2] == 2):
        w, h, px, rows = tgacmp.load(path)
        lum = [[(r[x * px] + r[x * px + 1] + r[x * px + 2]) / 3.0 for x in range(w)] for r in rows]
        return w, h, lum
    d = open(path, 'rb').read()
    w, h = struct.unpack('<ii', d[:8])
    px = d[8:]
    lum = []
    for y in range(h - 1, -1, -1):          # bottom-up -> top-down
        row = px[y * w * 4:(y + 1) * w * 4]
        lum.append([(row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2]) / 3.0 for x in range(w)])
    return w, h, lum


def weave(path, crop=None):
    w, h, lum = load_any(path)
    x0, y0, cw, ch = crop if crop else (0, 0, w, h)
    if x0 + cw > w or y0 + ch > h:
        raise SystemExit('%s: crop %dx%d+%d+%d exceeds %dx%d' % (path, cw, ch, x0, y0, w, h))
    acc = 0.0
    n = 0
    mean = 0.0
    for y in range(y0, y0 + ch - 1):
        r0, r1 = lum[y], lum[y + 1]
        for x in range(x0, x0 + cw - 1):
            acc += abs(r0[x] - r0[x + 1] - r1[x] + r1[x + 1])
            mean += r0[x]
            n += 1
    mean /= max(n, 1)
    # normalise by mean luminance: a darker exposure of the same weave scores
    # the same. Floor the denominator -- a near-black crop has no weave to
    # measure and must refuse rather than divide by nothing (the 5-6
    # near-black-denominator hazard).
    if mean < 2.0:
        return None, mean
    return acc / n / mean, mean


def main():
    args = [a for a in sys.argv[1:]]
    files = [a for a in args if not a.lstrip('-').isdigit()]
    nums = [int(a) for a in args if a.lstrip('-').isdigit()]
    crop = tuple(nums) if len(nums) == 4 else None
    if not files or len(files) > 2:
        raise SystemExit(__doc__)
    wa, ma = weave(files[0], crop)
    if wa is None:
        raise SystemExit('%s: crop mean %.2f is near-black; metric refused' % (files[0], ma))
    print('%s: weave %.5f (crop mean %.1f)' % (files[0], wa, ma))
    if len(files) == 2:
        wb, mb = weave(files[1], crop)
        if wb is None:
            raise SystemExit('%s: crop mean %.2f is near-black; metric refused' % (files[1], mb))
        print('%s: weave %.5f (crop mean %.1f)' % (files[1], wb, mb))
        print('b vs a: %.1f%%' % (100.0 * wb / wa))


if __name__ == '__main__':
    main()
