#!/usr/bin/env python3
"""rimcount.py -- the silhouette-outlier metric for the RT lighting term (2026-08-16).

Seb reported bright single pixels along monster and fixture silhouettes under
wall lighting. Whole-frame means cannot see a one-texel rim, so this counts the
thing itself, on the two artefacts the sidecar can dump:

  rimcount.py term  <dump>.term [K] [absmin]
      The SHOWN RT term buffer (RT_METAL_TERMDUMP=1 writes it beside every
      RT_METAL_DUMP frame: [i32 w][i32 h][RGBA16F, kernel row order]). Counts
      texels whose luminance exceeds the max of their 8-ring by ratio > K
      (default 2.0) AND exceeds absmin (default 0.5) -- "outliers" -- and splits
      them into EDGE (a hit-distance discontinuity of > 5% against any neighbour,
      or a sky texel beside a hit; alpha is the primary hit distance, 0 = sky) and
      INTERIOR. Also prints p50 / p99 / max of the term luminance over hit
      texels -- the LMAX-knee measurement CLAUDE.md's "no ceiling" fact asks
      for -- and the top outliers with their neighbourhoods.

  rimcount.py frame <dump>[.fN] x,y,w,h[;x,y,w,h ...] [X]
      The final frame ([i32 w][i32 h][RGBA8 bottom-up]). Counts pixels whose
      luminance exceeds the max of their 3x3 ring by more than X (default 40 of
      255) inside each crop rectangle (top-down coordinates) -- "fireflies".

Fail-first: HEAD's numbers on the bed frames are the bar; a fix must cut EDGE
outliers with the INTERIOR count and the below-knee histogram unmoved. Pure
python on purpose (no numpy on this machine); a 640x360 term takes ~2 s.
Dumps are RGBA, not BGRA (verified numerically 2026-08-16).
"""
import struct, sys

def load_term(path):
    d = open(path, 'rb').read()
    w, h = struct.unpack('<ii', d[:8])
    n = w * h
    vals = struct.unpack('<%de' % (n * 4), d[8:8 + n * 8])
    lum = [0.0] * n
    alp = [0.0] * n
    for i in range(n):
        r, g, b, a = vals[i*4], vals[i*4+1], vals[i*4+2], vals[i*4+3]
        lum[i] = 0.299 * r + 0.587 * g + 0.114 * b
        alp[i] = a
    return w, h, lum, alp, vals

def load_frame(path):
    d = open(path, 'rb').read()
    w, h = struct.unpack('<ii', d[:8])
    px = d[8:8 + w * h * 4]
    return w, h, px

def term_stats(path, K=2.0, absmin=0.5, topn=8):
    w, h, lum, alp, vals = load_term(path)
    hit = [l for i, l in enumerate(lum) if alp[i] > 0.0]
    hs = sorted(hit)
    def pct(p):
        return hs[min(len(hs) - 1, int(p * len(hs)))] if hs else 0.0
    edge = 0; interior = 0; outs = []
    for y in range(1, h - 1):
        for x in range(1, w - 1):
            i = y * w + x
            l = lum[i]
            if l <= absmin:
                continue
            ring = 0.0; a0 = alp[i]; isedge = False
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if dx == 0 and dy == 0:
                        continue
                    j = i + dy * w + dx
                    if lum[j] > ring:
                        ring = lum[j]
                    a1 = alp[j]
                    if (a0 == 0.0) != (a1 == 0.0):
                        isedge = True
                    elif a0 > 0.0 and abs(a0 - a1) > 0.05 * max(a0, a1):
                        isedge = True
            if l > K * ring:
                if isedge: edge += 1
                else: interior += 1
                outs.append((l, x, y, ring, a0))
    outs.sort(reverse=True)
    print('%s: %dx%d  hit texels %d  term luminance p50 %.3f p99 %.3f max %.3f' % (path.split('/')[-1], w, h, len(hs), pct(0.5), pct(0.99), hs[-1] if hs else 0.0))
    print('  outliers (lum > %.1fx 8-ring max, > %.2f): EDGE %d  INTERIOR %d' % (K, absmin, edge, interior))
    for (l, x, y, ring, a0) in outs[:topn]:
        print('    (%4d,%4d) lum %.3f ring-max %.3f dist %.1f' % (x, y, l, ring, a0))
    return edge, interior

def frame_fireflies(path, crops, X=40):
    w, h, px = load_frame(path)
    def lum_at(x, y):   # top-down coords into a bottom-up buffer
        o = ((h - 1 - y) * w + x) * 4
        return (299 * px[o] + 587 * px[o+1] + 114 * px[o+2]) // 1000
    total = 0
    for (cx, cy, cw, ch) in crops:
        cnt = 0
        for y in range(max(1, cy), min(h - 1, cy + ch)):
            for x in range(max(1, cx), min(w - 1, cx + cw)):
                l = lum_at(x, y)
                if l < X:
                    continue
                ring = 0
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        if dx == 0 and dy == 0:
                            continue
                        v = lum_at(x + dx, y + dy)
                        if v > ring:
                            ring = v
                if l > ring + X:
                    cnt += 1
        print('%s: crop (%d,%d %dx%d) fireflies (lum > 3x3-ring max + %d): %d' % (path.split('/')[-1], cx, cy, cw, ch, X, cnt))
        total += cnt
    return total

if __name__ == '__main__':
    a = sys.argv[1:]
    if not a:
        print(__doc__); sys.exit(1)
    if a[0] == 'term':
        K = float(a[2]) if len(a) > 2 else 2.0
        absmin = float(a[3]) if len(a) > 3 else 0.5
        term_stats(a[1], K, absmin)
    elif a[0] == 'frame':
        crops = [tuple(int(v) for v in c.split(',')) for c in a[2].split(';')]
        X = int(a[3]) if len(a) > 3 else 40
        frame_fireflies(a[1], crops, X)
    else:
        print(__doc__); sys.exit(1)
