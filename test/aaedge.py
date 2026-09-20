#!/usr/bin/env python3
# test/aaedge.py -- the offline antialiasing bed. SMAA.md slice 0c.
#
# Pure stdlib, no numpy (this machine has none -- the bluenoise-check.py
# precedent). Four things, and the order matters:
#
#   1. GROUND TRUTH. synth() builds an edge whose every pixel carries its EXACT
#      analytic area coverage, integrated in closed form, so the right answer is
#      known rather than assumed. synth(hard=True) is the same edge with no
#      antialiasing at all -- the thing a filter has to fix.
#   2. THE ESTIMATOR. edge_positions() recovers the edge's sub-pixel position per
#      column by threshold crossing. This is the STABLE estimator: a max-gradient
#      tracker latches onto rivets and reads pure noise on real frames.
#   3. THE METRIC. staircase() = mean |2nd difference| of that position. A clean
#      ramp is 0; a staircase of step length L reads about 2/L.
#   4. THE FILTERS. fxaa() is shader_msl.h's dp_fxaa transcribed tap for tap
#      (bilinear sampling included, because the taps land between texels and that
#      is half of what the filter does). mlaa() is the analytic morphological
#      pass SMAA.md slice 1 specifies, and its offline number is the gate the MSL
#      arm has to reproduce.
#
# THE INSTRUMENT EXISTS BECAUSE A SKETCH MEASURED AS DOING NOTHING. A first MLAA
# cut that blended SYMMETRICALLY across the edge changed the staircase by zero --
# a symmetric transfer cannot move an edge's position, which the ground-truth bed
# reported in seconds and reading the code would not have. Validate here before
# writing a line of MSL.
#
# Recorded baselines (this file, 2026-09-18; reproduce with `ladder`), staircase
# by the integrating estimator on a 96x40 synthetic edge:
#
#   slope    truth    no AA    fxaa8    fxaa16   mlaa
#   1:2      0.013    0.895    0.547    0.543    0.013
#   1:3      0.000    0.670    0.145    0.143    0.005
#   1:4      0.000    0.511    0.144    0.145    0.005
#   1:6      0.000    0.340    0.230    0.124    0.008
#   1:8      0.000    0.255    0.171    0.096    0.008
#   1:12     0.000    0.170    0.121    0.147    0.005
#   1:20     0.000    0.085    0.061    0.104    0.001
#   1:40     0.000    0.043    0.030    0.052    0.002
#
# The 1:6 row reproduces SMAA.md's own ground-truth table (0.335 / 0.232 /
# 0.120) to within 0.005 on all three columns, which is the cross-check that
# this instrument is the one that wrote the plan. Read the shape, not just the
# numbers: FXAA leaves a third to a half of the staircase at every slope
# because it corrects at most half a pixel near a step's ENDS, while MLAA is at
# the estimator's own floor everywhere, because it blends every pixel of a step
# by that pixel's own coverage.
#
# Usage:
#   python3 test/aaedge.py selftest            # the synthetic ladder above
#   python3 test/aaedge.py ladder 6 3 12       # arbitrary slopes
#   python3 test/aaedge.py real <dump> x y w h [thresh]   # an RT_METAL_DUMP frame
#   python3 test/aaedge.py realfilter <dump> x y w h thresh mlaa|fxaa8|fxaa16
#   python3 test/aaedge.py pgm <dump> x y w h out.pgm [mlaa|fxaa8|fxaa16]
import struct, sys, os

LUMA = (0.299, 0.587, 0.114)

# ---------------------------------------------------------------------------
# images: flat list of floats, 3 per pixel, ROW 0 IS THE TOP.
# ---------------------------------------------------------------------------

def make(w, h, v=0.0):
    return [v] * (w * h * 3)

def px(img, w, x, y):
    i = (y * w + x) * 3
    return img[i], img[i + 1], img[i + 2]

def luma_of(img, w, x, y):
    i = (y * w + x) * 3
    return img[i] * LUMA[0] + img[i + 1] * LUMA[1] + img[i + 2] * LUMA[2]

# ---------------------------------------------------------------------------
# 1. ground truth
# ---------------------------------------------------------------------------

def _int_clamp01(g0, g1, span):
    """Integral over a unit-parameterised interval of clamp(linear, 0, 1),
    where the linear function runs from g0 to g1 across an interval of width
    `span`. Exact: split at the two clamp breakpoints, trapezoid each monotone
    piece (a linear function's integral IS its trapezoid)."""
    if span <= 0.0:
        return 0.0
    if abs(g1 - g0) < 1e-12:
        return min(1.0, max(0.0, g0)) * span
    # parametrise u in [0,1] across the interval; g(u) = g0 + (g1-g0)*u
    pts = [0.0, 1.0]
    for target in (0.0, 1.0):
        u = (target - g0) / (g1 - g0)
        if 0.0 < u < 1.0:
            pts.append(u)
    pts.sort()
    total = 0.0
    for a, b in zip(pts[:-1], pts[1:]):
        ga = min(1.0, max(0.0, g0 + (g1 - g0) * a))
        gb = min(1.0, max(0.0, g0 + (g1 - g0) * b))
        total += 0.5 * (ga + gb) * (b - a)
    return total * span

def synth(w, h, slope, y0=None, lo=0.12, hi=0.78, hard=False):
    """A straight edge at y = y0 + slope*x. Pixels BELOW the line (larger y)
    take `hi`, above take `lo`. Without `hard`, every partially covered pixel
    carries its exact area coverage -- so the staircase metric's answer on this
    image is 0 by construction and anything above that is the estimator's own
    floor."""
    if y0 is None:
        y0 = h * 0.5 - slope * w * 0.5
    img = make(w, h)
    for y in range(h):
        for x in range(w):
            if hard:
                # one sample at the pixel centre: the aliased edge
                cov = 1.0 if (y + 0.5) > (y0 + slope * (x + 0.5)) else 0.0
            else:
                # coverage of "below the line" = integral over the pixel's x span
                # of clamp((y+1) - line(t), 0, 1)
                g0 = (y + 1.0) - (y0 + slope * x)
                g1 = (y + 1.0) - (y0 + slope * (x + 1.0))
                cov = _int_clamp01(g0, g1, 1.0)
            v = lo + (hi - lo) * cov
            i = (y * w + x) * 3
            img[i] = img[i + 1] = img[i + 2] = v
    return img

# ---------------------------------------------------------------------------
# 2/3. the estimator and the metric
# ---------------------------------------------------------------------------

def edge_positions_thresh(img, w, h, thresh, x0=0, x1=None, y0=0, y1=None):
    """Sub-pixel y of the luma crossing `thresh`, per column, by linear
    interpolation between the bracketing rows. This is the estimator SMAA.md's
    recorded real-frame numbers were taken with, so it is what a comparison
    against them must use -- but note its floor: on a pixel-wide coverage ramp
    it carries a systematic +-0.083 px nonlinearity (the samples are box
    integrals and this interpolates them as point samples), which reads as a
    staircase of 0.066 on a PERFECT synthetic edge. Use edge_positions_sum for
    the synthetic bed, where the profile really is a coverage ramp.
    Columns with no crossing come back None, so a crop that half-misses the
    edge cannot quietly contribute zeros."""
    if x1 is None: x1 = w
    if y1 is None: y1 = h
    out = []
    for x in range(x0, x1):
        prev = luma_of(img, w, x, y0)
        found = None
        for y in range(y0 + 1, y1):
            cur = luma_of(img, w, x, y)
            if (prev - thresh) * (cur - thresh) <= 0.0 and prev != cur:
                found = (y - 1) + (thresh - prev) / (cur - prev) + 0.5
                break
            prev = cur
        out.append(found)
    return out

def edge_positions_sum(img, w, h, x0=0, x1=None, y0=0, y1=None, pad=3):
    """Sub-pixel y of the edge, per column, by INTEGRATING the profile rather
    than crossing it: with coverage c(y) = clamp(y+1 - yedge, 0, 1), the sum of
    c over the window is exactly (y1 - yedge), so yedge = y1 - sum. Exact on an
    analytic coverage edge -- ground truth reads 0.000 -- and unbiased under any
    filter that preserves the profile's mean, which a blend does. The window's
    first and last `pad` rows give the per-column lo/hi, so texture that varies
    along the edge does not bias it."""
    if x1 is None: x1 = w
    if y1 is None: y1 = h
    out = []
    for x in range(x0, x1):
        lo = sum(luma_of(img, w, x, y) for y in range(y0, y0 + pad)) / pad
        hi = sum(luma_of(img, w, x, y) for y in range(y1 - pad, y1)) / pad
        if abs(hi - lo) < 1e-6:
            out.append(None); continue
        s = 0.0
        for y in range(y0, y1):
            s += (luma_of(img, w, x, y) - lo) / (hi - lo)
        out.append(y1 - s)
    return out

# the default estimator is the exact one; the crossing form is named explicitly
def edge_positions(img, w, h, thresh=None, x0=0, x1=None, y0=0, y1=None):
    if thresh is None:
        return edge_positions_sum(img, w, h, x0, x1, y0, y1)
    return edge_positions_thresh(img, w, h, thresh, x0, x1, y0, y1)

def staircase(pos):
    """mean |2nd difference| of the recovered edge position. A clean ramp is 0.
    A staircase whose steps are L columns long reads about 2/L."""
    vals = [p for p in pos if p is not None]
    if len(vals) != len(pos) or len(vals) < 3:
        # a gap makes the 2nd difference meaningless across it
        vals = [p for p in pos if p is not None]
        if len(vals) < 3:
            return float('nan')
    s = 0.0
    n = 0
    for i in range(1, len(vals) - 1):
        s += abs(vals[i + 1] - 2.0 * vals[i] + vals[i - 1])
        n += 1
    return s / n if n else float('nan')

# ---------------------------------------------------------------------------
# 4a. dp_fxaa, transcribed from shader_msl.h tap for tap
# ---------------------------------------------------------------------------

def sample(img, w, h, u, v):
    """Bilinear, clamp-to-edge, texel (i,j) centred at ((i+.5)/w,(j+.5)/h) --
    the sampler the shader is given."""
    fx = u * w - 0.5
    fy = v * h - 0.5
    x0 = int(fx // 1); y0 = int(fy // 1)
    tx = fx - x0; ty = fy - y0
    def at(x, y):
        x = 0 if x < 0 else (w - 1 if x >= w else x)
        y = 0 if y < 0 else (h - 1 if y >= h else y)
        i = (y * w + x) * 3
        return img[i], img[i + 1], img[i + 2]
    a = at(x0, y0); b = at(x0 + 1, y0); c = at(x0, y0 + 1); d = at(x0 + 1, y0 + 1)
    out = []
    for k in range(3):
        top = a[k] + (b[k] - a[k]) * tx
        bot = c[k] + (d[k] - c[k]) * tx
        out.append(top + (bot - top) * ty)
    return out

def _dot3(v):
    return v[0] * LUMA[0] + v[1] * LUMA[1] + v[2] * LUMA[2]

def fxaa(img, w, h, maxspan=8.0):
    """shader_msl.h's dp_fxaa, LOCKSTEP. Note what it is: a <=0.5 px correction
    applied near a step's ENDS. On a 6-px step four of the six pixels get
    nothing, which is why SMAA.md calls it structurally exhausted."""
    out = list(img)
    psx = 1.0 / w; psy = 1.0 / h
    mulreduct = 1.0 / maxspan
    minreduct = 1.0 / 128.0
    for y in range(h):
        for x in range(w):
            tcx = (x + 0.5) * psx; tcy = (y + 0.5) * psy
            NW = sample(img, w, h, tcx - psx, tcy - psy)
            NE = sample(img, w, h, tcx + psx, tcy - psy)
            SW = sample(img, w, h, tcx - psx, tcy + psy)
            SE = sample(img, w, h, tcx + psx, tcy + psy)
            M  = sample(img, w, h, tcx, tcy)
            lNW = _dot3(NW); lNE = _dot3(NE); lSW = _dot3(SW); lSE = _dot3(SE); lM = _dot3(M)
            lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)))
            lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)))
            dirx = -((lNW + lNE) - (lSW + lSE))
            diry = ((lNW + lSW) - (lNE + lSE))
            rcpval = 1.0 / (min(abs(dirx), abs(diry)) + max((lNW + lNE + lSW + lSE) * (0.25 * mulreduct), minreduct))
            dx = min(maxspan, max(-maxspan, dirx * rcpval)) * psx
            dy = min(maxspan, max(-maxspan, diry * rcpval)) * psy
            s1 = sample(img, w, h, tcx + dx * (1.0/3.0 - 0.5), tcy + dy * (1.0/3.0 - 0.5))
            s2 = sample(img, w, h, tcx + dx * (2.0/3.0 - 0.5), tcy + dy * (2.0/3.0 - 0.5))
            rA = [0.5 * (s1[k] + s2[k]) for k in range(3)]
            s0 = sample(img, w, h, tcx + dx * (0.0/3.0 - 0.5), tcy + dy * (0.0/3.0 - 0.5))
            s3 = sample(img, w, h, tcx + dx * (3.0/3.0 - 0.5), tcy + dy * (3.0/3.0 - 0.5))
            rB = [rA[k] * 0.5 + 0.25 * (s0[k] + s3[k]) for k in range(3)]
            lB = _dot3(rB)
            res = rA if (lB < lMin or lB > lMax) else rB
            i = (y * w + x) * 3
            out[i] = res[0]; out[i + 1] = res[1]; out[i + 2] = res[2]
    return out

# ---------------------------------------------------------------------------
# 4b. analytic MLAA -- SMAA.md slice 1
# ---------------------------------------------------------------------------
#
# Three passes, exactly as the MSL arm will have them.
#
# PASS 1, edges. Per pixel, two booleans: is there a luma discontinuity across
# its LEFT boundary (a vertical edge line) and across its TOP boundary (a
# horizontal one). Edge lines live on pixel BOUNDARIES, not on pixels.
#
# PASS 2, weights. For a pixel with a horizontal edge line above it, walk that
# line left and right to its ends and read what the edge does there. The
# revectorised geometry is then a straight segment per half-run, running from
# +-0.5 at the run's own end to 0 at its midpoint -- which for a monotone
# staircase composes into exactly the single sloped line through the run, and
# for a one-off notch decays instead of smearing a genuine feature. Each
# pixel's weight is that segment's SIGNED area over the pixel's own column,
# and the sign says which of the two pixels straddling the line gets it.
# THE ASYMMETRY IS THE WHOLE MECHANISM: exactly one side is blended.
#
# PASS 3, blend. Each pixel mixes toward its four neighbours by the four
# weights (its own top/left, and the bottom/right it gathers from below/right).

def _detect(img, w, h, thresh, adapt=2.0):
    """left-boundary and top-boundary edge flags, with SMAA's LOCAL CONTRAST
    ADAPTATION -- LOCKSTEP the MODE_SMAAEDGES arm in shader_msl.h.

    The adaptation is what keeps a morphological pass off ordinary texture: an
    edge survives only if its own luma step is at least 1/adapt of the biggest
    step in its neighbourhood, so the strong silhouette in a busy region wins
    and the texture around it does not also register. The two double-steps
    (leftleft, toptop) are what let a boundary see the contrast on BOTH sides of
    itself. It changes nothing on an isolated synthetic edge, which is why the
    acceptance ladder is unmoved by it."""
    el = [False] * (w * h)   # vertical edge line on this pixel's LEFT
    et = [False] * (w * h)   # horizontal edge line on this pixel's TOP
    lum = [0.0] * (w * h)
    for y in range(h):
        for x in range(w):
            lum[y * w + x] = luma_of(img, w, x, y)
    def L(x, y):
        x = 0 if x < 0 else (w - 1 if x >= w else x)
        y = 0 if y < 0 else (h - 1 if y >= h else y)
        return lum[y * w + x]
    for y in range(h):
        for x in range(w):
            i = y * w + x
            c = L(x, y)
            dl = abs(c - L(x - 1, y))
            dt = abs(c - L(x, y - 1))
            e = [x > 0 and dl > thresh, y > 0 and dt > thresh]
            md = max(dl, dt, abs(c - L(x + 1, y)), abs(c - L(x, y + 1)),
                     abs(L(x - 1, y) - L(x - 2, y)), abs(L(x, y - 1) - L(x, y - 2)))
            if md > adapt * dl:
                e[0] = False
            if md > adapt * dt:
                e[1] = False
            el[i] = e[0]
            et[i] = e[1]
    return el, et

def _run_ends(flag, w, h, x, y, horizontal, maxsearch):
    """Walk the edge line through (x,y) to both ends. Returns
    (start, endexclusive, o1, o2) in the line's own axis, where o1/o2 are the
    revectorised line's offsets at the two ends: -0.5 when the edge steps to
    the LOWER index side (up, for a horizontal line), +0.5 for the higher, and
    0.0 when the search found no crossing (an uncapped run: correct to leave
    alone rather than invent a slope for)."""
    if horizontal:
        idx = lambda t: y * w + t
        above = lambda t: (y - 1) * w + t if y > 0 else None
        below = lambda t: (y + 1) * w + t if y + 1 < h else None
        lo_lim, hi_lim = 0, w
    else:
        idx = lambda t: t * w + x
        above = lambda t: t * w + (x - 1) if x > 0 else None
        below = lambda t: t * w + (x + 1) if x + 1 < w else None
        lo_lim, hi_lim = 0, h
    t0 = x if horizontal else y
    a = t0
    while a - 1 >= lo_lim and flag[idx(a - 1)] and (t0 - a) < maxsearch:
        a -= 1
    b = t0
    while b + 1 < hi_lim and flag[idx(b + 1)] and (b - t0) < maxsearch:
        b += 1
    def crossing(t, step):
        # at the far side of the run's end, which way did the edge line go?
        u = t + step
        if u < lo_lim or u >= hi_lim:
            return 0.0
        ia = above(u); ib = below(u)
        up = ia is not None and flag[ia]
        dn = ib is not None and flag[ib]
        if up and not dn:
            return -0.5
        if dn and not up:
            return +0.5
        return 0.0
    o1 = crossing(a, -1) if (a - lo_lim) > 0 and (t0 - a) < maxsearch else 0.0
    o2 = crossing(b, +1) if (hi_lim - 1 - b) > 0 and (b - t0) < maxsearch else 0.0
    return a, b + 1, o1, o2

def _wedge_area(a, b, o1, o2, t):
    """Signed area, over the unit cell [t, t+1], of the revectorised offset
    function: a wedge from o1 at `a` to 0 at the midpoint, and 0 at the
    midpoint to o2 at `b`. Integrated exactly (the function is piecewise
    linear, so a trapezoid per piece is not an approximation)."""
    m = 0.5 * (a + b)
    half = m - a
    if half <= 0.0:
        return 0.0
    def off(u):
        if u <= m:
            return o1 * (m - u) / half
        return o2 * (u - m) / half
    lo = float(t); hi = float(t + 1)
    pts = [lo, hi]
    if lo < m < hi:
        pts.insert(1, m)
    total = 0.0
    for p, q in zip(pts[:-1], pts[1:]):
        total += 0.5 * (off(p) + off(q)) * (q - p)
    return total

def mlaa(img, w, h, thresh=0.05, maxsearch=24, adapt=2.0):
    el, et = _detect(img, w, h, thresh, adapt)
    wt = [0.0] * (w * h)   # blend toward the pixel ABOVE
    wl = [0.0] * (w * h)   # blend toward the pixel to the LEFT
    wb = [0.0] * (w * h)   # blend toward the pixel BELOW
    wr = [0.0] * (w * h)   # blend toward the pixel to the RIGHT
    for y in range(h):
        for x in range(w):
            i = y * w + x
            if et[i]:
                a, b, o1, o2 = _run_ends(et, w, h, x, y, True, maxsearch)
                area = _wedge_area(a, b, o1, o2, x)
                if area < 0.0:
                    # the true edge sits ABOVE the line here, so the pixel above
                    # is partly covered by THIS pixel's colour
                    wb[i - w] += -area
                elif area > 0.0:
                    wt[i] += area
            if el[i]:
                a, b, o1, o2 = _run_ends(el, w, h, x, y, False, maxsearch)
                area = _wedge_area(a, b, o1, o2, y)
                if area < 0.0:
                    wr[i - 1] += -area
                elif area > 0.0:
                    wl[i] += area
    out = list(img)
    for y in range(h):
        for x in range(w):
            i = y * w + x
            a_t = wt[i] if y > 0 else 0.0
            a_b = wb[i] if y + 1 < h else 0.0
            a_l = wl[i] if x > 0 else 0.0
            a_r = wr[i] if x + 1 < w else 0.0
            s = a_t + a_b + a_l + a_r
            if s <= 0.0:
                continue
            if s > 1.0:
                k = 1.0 / s
                a_t *= k; a_b *= k; a_l *= k; a_r *= k; s = 1.0
            c = px(img, w, x, y)
            nt = px(img, w, x, y - 1) if y > 0 else c
            nb = px(img, w, x, y + 1) if y + 1 < h else c
            nl = px(img, w, x - 1, y) if x > 0 else c
            nr = px(img, w, x + 1, y) if x + 1 < w else c
            j = i * 3
            for k in range(3):
                out[j + k] = c[k] * (1.0 - s) + nt[k] * a_t + nb[k] * a_b + nl[k] * a_l + nr[k] * a_r
    return out

# ---------------------------------------------------------------------------
# RT_METAL_DUMP frames: [int32 w][int32 h][RGBA8 bottom-up]; RGBA, not BGRA
# ---------------------------------------------------------------------------

def load_dump(path, x0, y0, cw, ch):
    """Crop (x0,y0 from the TOP) out of a raw dump into our top-down float image."""
    with open(path, 'rb') as f:
        w, h = struct.unpack('<ii', f.read(8))
        data = f.read(w * h * 4)
    img = make(cw, ch)
    for y in range(ch):
        srow = (h - 1 - (y0 + y)) * w
        for x in range(cw):
            si = (srow + x0 + x) * 4
            di = (y * cw + x) * 3
            img[di] = data[si] / 255.0
            img[di + 1] = data[si + 1] / 255.0
            img[di + 2] = data[si + 2] / 255.0
    return img

def save_pgm(img, w, h, path):
    rows = bytearray()
    for y in range(h):
        for x in range(w):
            v = luma_of(img, w, x, y)
            rows.append(max(0, min(255, int(v * 255.0 + 0.5))))
    with open(path, 'wb') as f:
        f.write(b'P5\n%d %d\n255\n' % (w, h))
        f.write(bytes(rows))

# ---------------------------------------------------------------------------

def _report(name, img, w, h, thresh):
    pos = edge_positions(img, w, h, thresh)
    return name, staircase(pos)

def cmd_ladder(slopes):
    W, H = 96, 40
    print("%-10s %8s %8s %8s %8s %8s" % ("slope", "truth", "noAA", "fxaa8", "fxaa16", "mlaa"))
    for s in slopes:
        slope = 1.0 / s
        truth = synth(W, H, slope)
        hard = synth(W, H, slope, hard=True)
        row = [staircase(edge_positions(truth, W, H)),
               staircase(edge_positions(hard, W, H)),
               staircase(edge_positions(fxaa(hard, W, H, 8.0), W, H)),
               staircase(edge_positions(fxaa(hard, W, H, 16.0), W, H)),
               staircase(edge_positions(mlaa(hard, W, H), W, H))]
        print("1:%-8g %8.3f %8.3f %8.3f %8.3f %8.3f" % tuple([s] + row))

def cmd_selftest():
    cmd_ladder([6, 3])
    W, H = 96, 40
    ok = True
    for s in (6, 3):
        hard = synth(W, H, 1.0 / s, hard=True)
        v = staircase(edge_positions(mlaa(hard, W, H), W, H))
        good = v <= 0.02
        ok = ok and good
        print("acceptance 1:%d  mlaa staircase %.4f  %s (gate <= 0.02)" % (s, v, "PASS" if good else "FAIL"))
    print("SELFTEST", "PASS" if ok else "FAIL")
    return 0 if ok else 1

def main(argv):
    if len(argv) < 2:
        print(__doc__ or "see the header"); return 2
    cmd = argv[1]
    if cmd == 'selftest':
        return cmd_selftest()
    if cmd == 'ladder':
        cmd_ladder([float(a) for a in argv[2:]] or [6, 3]); return 0
    if cmd in ('real', 'realfilter', 'pgm'):
        path = argv[2]; x0, y0, cw, ch = (int(a) for a in argv[3:7])
        img = load_dump(path, x0, y0, cw, ch)
        if cmd == 'real':
            thr = float(argv[7]) if len(argv) > 7 else 0.45
            pos = edge_positions_thresh(img, cw, ch, thr)
            # the CROSSING estimator on a real frame, per SMAA.md: the
            # integrating one needs a per-column lo/hi read off the window's
            # ends, and a textured band (rivets, studs) makes that nonsense --
            # it reads tens where the answer is fractions. The synthetic bed is
            # where the exact estimator belongs.
            print("staircase %.4f  (%d/%d columns crossed)"
                  % (staircase(pos), sum(1 for p in pos if p is not None), cw))
            print("edge y:", " ".join("%.1f" % p if p is not None else "--" for p in pos))
            return 0
        filt = argv[8] if cmd == 'realfilter' else (argv[8] if len(argv) > 8 else None)
        thr = float(argv[7]) if len(argv) > 7 else 0.45
        if filt == 'mlaa':   img2 = mlaa(img, cw, ch)
        elif filt == 'fxaa8':  img2 = fxaa(img, cw, ch, 8.0)
        elif filt == 'fxaa16': img2 = fxaa(img, cw, ch, 16.0)
        else: img2 = img
        if cmd == 'pgm':
            save_pgm(img2, cw, ch, argv[7]); return 0
        print("%-8s staircase %.4f" % (filt or 'none', staircase(edge_positions_thresh(img2, cw, ch, thr))))
        return 0
    print("unknown command", cmd); return 2

if __name__ == '__main__':
    sys.exit(main(sys.argv))
