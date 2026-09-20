#!/usr/bin/env python3
"""
fogmatch.py -- does a fogged surface BELONG in the frame it is sitting in?

WHY THIS EXISTS, and it is the reason LIQUIDFOG shipped broken once.

Every other instrument in test/ measures AGREEMENT. tgacmp.py compares GL
against Metal; the byte gate compares before against after; look-ab.sh injects
Seb's config and still only asks whether the two backends match. Nothing asked
whether a pixel BELONGS. So when r_volumetric_liquidfade faded blended water
toward the wrong colour -- the authored r_volumetric_color_*, the murk's UNLIT
base, while the murk on screen is that base plus everything rt_metal_fog adds --
both backends agreed perfectly, every gate was green, and the first person to
see it was Seb: "from a distance it looks black through the bluegreen fog".

The measurement is the question his eye asked. Take the far edge of the liquid,
take the fog immediately beyond that edge, and compare them. A surface that has
been correctly faded into the murk is within a few levels of the murk it is
fading into. A surface faded toward a reconstructed colour is not.

  python3 test/fogmatch.py --mask A.tga B.tga [--absent X.tga] frame1.tga [...]

  --mask A B   two frames differing ONLY in r_wateralpha (0.7 vs 0.35, say).
               The pixels that move ARE the alpha-blended liquid; nothing else
               in the frame can respond to that cvar. Same camera as the frames
               being measured, or the mask is meaningless.
  --absent X   the same camera with the liquid rendered essentially INVISIBLE
               (r_wateralpha 0.02). This is the UNBIASED gate -- see below.
  --band N     rows of liquid to call "far" (default 24), measured DOWN from the
               liquid's top edge in each column.
  --gap N      rows to skip above the edge before sampling fog (default 2), so
               the waterline's own filtering does not pollute the reference.
  --hud N      rows to exclude at the bottom (default 60): the status bar is
               drawn over the liquid and is not part of it.

TWO NUMBERS, AND THEY ANSWER DIFFERENT QUESTIONS. Read both.

`delta`/`ratio` compare the far liquid against the fog BEYOND its far edge.
That is the question Seb's eye asked and the number that moves when the defect
is present -- but it is DEPTH-BIASED and does not go to zero on a correct
renderer: the wall above a waterline is further from the eye than the water's
far edge, so it is legitimately more fogged. Treat it as an impression, not a
gate. (2026-08-31: 33.9 with no fade, 34.4 with the colour-lerp fade -- i.e.
that fade bought nothing -- and 27.8 with the alpha fade.)

`absent` is the gate. It compares the far liquid against THE SAME PIXELS with
the liquid all but removed, so it is free of the depth bias and it asks the
only question the fade itself is responsible for: at full extinction, does the
surface vanish into exactly what the murk painted behind it? A correct fade
drives this to zero as strength rises; a fade that converges to a colour of its
own cannot, however high you push it. Anything left in `delta` once `absent` is
near zero belongs to what is BEHIND the liquid -- on a vanilla-vis map with
r_wateralpha_force that is largely the culled pool interior, which r_novis 1
will show you and which no shader arm can fix.
"""
import sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def read_tga(path):
    """Uncompressed and RLE 24/32-bit TGA -> (w, h, rows top-to-bottom, RGBA)."""
    import struct
    d = open(path, 'rb').read()
    idlen, cmaptype, imgtype = d[0], d[1], d[2]
    w, h = struct.unpack('<HH', d[12:16])
    bpp, desc = d[16], d[17]
    off = 18 + idlen
    if cmaptype:
        cmlen, cmbpp = struct.unpack('<H', d[5:7])[0], d[7]
        off += cmlen * (cmbpp // 8)
    npx, nch = w * h, bpp // 8
    if imgtype in (2, 3):
        raw = d[off:off + npx * nch]
    elif imgtype in (10, 11):
        out = bytearray()
        i = off
        while len(out) < npx * nch:
            p = d[i]; i += 1; n = (p & 0x7f) + 1
            if p & 0x80:
                out += d[i:i + nch] * n; i += nch
            else:
                out += d[i:i + n * nch]; i += n * nch
        raw = bytes(out[:npx * nch])
    else:
        raise SystemExit('%s: unsupported TGA type %d' % (path, imgtype))
    rows = []
    for y in range(h):
        s = y * w * nch
        rows.append(raw[s:s + w * nch])
    if not (desc & 0x20):
        rows.reverse()
    return w, h, rows, nch


def liquid_mask(a, b, thresh=6):
    """Pixels that moved between two r_wateralpha arms == the blended liquid."""
    w, h, A, nch = read_tga(a)
    _, _, B, _ = read_tga(b)
    m = []
    for y in range(h):
        ra, rb = A[y], B[y]
        row = bytearray(w)
        for x in range(w):
            i = x * nch
            if (abs(ra[i] - rb[i]) + abs(ra[i+1] - rb[i+1]) + abs(ra[i+2] - rb[i+2])) > thresh:
                row[x] = 1
        m.append(row)
    return w, h, m


def measure(path, w, h, mask, band, gap, hud, absent=None):
    fw, fh, F, nch = read_tga(path)
    if (fw, fh) != (w, h):
        raise SystemExit('%s is %dx%d, mask is %dx%d' % (path, fw, fh, w, h))
    A = None
    if absent is not None:
        aw, ah, A, anch = read_tga(absent)
        if (aw, ah) != (w, h):
            raise SystemExit('--absent frame is %dx%d, mask is %dx%d' % (aw, ah, w, h))
    lim = h - hud
    lsum = [0, 0, 0]; ln = 0
    fsum = [0, 0, 0]; fn = 0
    adiff = 0.0
    for x in range(w):
        # the liquid's top edge in this column: the far end of the surface
        top = -1
        for y in range(lim):
            if mask[y][x]:
                top = y
                break
        if top < 0:
            continue
        for y in range(top, min(top + band, lim)):
            if not mask[y][x]:
                continue
            i = x * nch; r = F[y]
            lsum[0] += r[i]; lsum[1] += r[i+1]; lsum[2] += r[i+2]; ln += 1
            if A is not None:
                ra = A[y]
                adiff += abs(r[i] - ra[i]) + abs(r[i+1] - ra[i+1]) + abs(r[i+2] - ra[i+2])
        # the fog just BEYOND that edge -- same column, above the waterline,
        # and never itself liquid (a pool can have liquid above it in frame)
        for y in range(max(0, top - gap - band), max(0, top - gap)):
            if mask[y][x]:
                continue
            i = x * nch; r = F[y]
            fsum[0] += r[i]; fsum[1] += r[i+1]; fsum[2] += r[i+2]; fn += 1
    if not ln or not fn:
        print('%-22s (no liquid edge found -- is the mask from this camera?)' % os.path.basename(path))
        return None
    L = [s / float(ln) for s in lsum]
    G = [s / float(fn) for s in fsum]
    delta = sum(abs(L[k] - G[k]) for k in range(3)) / 3.0
    lum = lambda c: 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]
    ratio = lum(L) / max(lum(G), 1e-6)
    tail = ''
    if A is not None:
        tail = '   absent %6.2f' % (adiff / (3.0 * ln))
    print('%-22s far liquid %6.2f %6.2f %6.2f | fog beyond %6.2f %6.2f %6.2f'
          '  ->  delta %6.2f   ratio %5.2f%s  (%d px)'
          % (os.path.basename(path), L[0], L[1], L[2], G[0], G[1], G[2],
             delta, ratio, tail, ln))
    return delta, ratio


if __name__ == '__main__':
    a = sys.argv[1:]
    band, gap, hud, thresh = 24, 2, 60, 6
    for opt, cast in (('--band', int), ('--gap', int), ('--hud', int), ('--thresh', int)):
        if opt in a:
            i = a.index(opt)
            v = cast(a[i + 1]); del a[i:i + 2]
            if opt == '--band': band = v
            elif opt == '--gap': gap = v
            elif opt == '--hud': hud = v
            else: thresh = v
    absent = None
    if '--absent' in a:
        i = a.index('--absent'); absent = a[i + 1]; del a[i:i + 2]
    if '--mask' not in a:
        raise SystemExit(__doc__)
    i = a.index('--mask')
    ma, mb = a[i + 1], a[i + 2]
    del a[i:i + 3]
    w, h, mask = liquid_mask(ma, mb, thresh)
    npx = sum(sum(r) for r in mask)
    print('mask: %d px of %d (%.2f%%) from %s vs %s'
          % (npx, w * h, 100.0 * npx / (w * h), os.path.basename(ma), os.path.basename(mb)))
    for p in a:
        measure(p, w, h, mask, band, gap, hud, absent)
