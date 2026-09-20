#!/usr/bin/env python3
# test/flicker.py -- consecutive-frame flicker metric over RT_METAL_DUMP raw
# frames ([int32 w][int32 h][RGBA8 bottom-up]; RGBA, not BGRA -- the documented
# reader trap). Born 2026-08-29 diagnosing Seb's demo17 fog speckle; the bed it
# established is repeatable verbatim and is the acceptance bed for any fog
# variance work (the FOGLIGHT residual escalations, GIARC G2):
#
#   BENCH_DEMO="$HOME/Library/Application Support/darkplaces/m5/demo17.dem" \
#   BENCH_ROUNDS=1 BENCH_B='<candidate cvar line>' \
#   BENCH_DUMPFRAMES="2600,2601,2602,2603,2604,2605,2606,2607,2608,2609" \
#   BENCH_OUT=<dir> sh test/bench-rt.sh
#   python3 test/flicker.py seq <dir>/dump_A_r1 2600 2601 ... 2609
#
# The crop is centred slightly above frame centre (the crosshair region; on
# demo17 f2600 that is the fog bank in front of the ogre room's columns).
# Recorded 2026-08-29 baselines on that bed, mean flicker / % of crop >2:
#   Seb's config as-is            0.80 / 2.3%   (the reported speckle)
#   rt_metal_gi 0                 0.84 / 2.4%   (GI exonerated, as he observed)
#   rt_metal_fog_stride_adaptive 0  0.79 / 2.1% (a shaving, not the cause)
#   rt_metal_fog_history 0.9      0.37 / 0.55%  (free config mitigation)
#   rt_metal_fog_stride 1         0.52 / 0.6%   (3x casts, ~3x fog ray cost)
#   rt_metal_lightsample 0        0.08 / 0.05%  (THE MECHANISM: the fog pick)
#
# 'fogseq' (BLUENOISE slice 0, 2026-09-03) reads the FOG-BUFFER dumps that
# RT_METAL_FOGDUMP=1 writes beside each frame ("<dump>.fN.fog": [int32 w]
# [int32 h][RGBA16F bottom-up], RGB = in-scattered light, A = transmittance)
# -- the fog BEFORE the magnification, the composite and MetalFX, so the
# number is the kernel's own and the scaler cannot confound it. Values are
# float, so the metric is RELATIVE: mean |delta luma| over the crop's mean
# luma, the max relative delta, and the share of texels moving more than 5%
# of the crop mean. 'fogpng' exports one (gain-scaled) for eyeballing. The
# crop is the same crosshair region, in fog-buffer texels.
# 'png' mode exports a frame (or its crosshair crop, optionally gain-scaled)
# for eyeballing; the amplified consecutive-frame difference image is what
# localised the speckle to the fog bank in one look.
import struct, sys, zlib, os

def load(path):
    with open(path, 'rb') as f:
        w, h = struct.unpack('<ii', f.read(8))
        data = f.read(w * h * 4)
    return w, h, data

def crop_delta(a, b, w, h, cx, cy, cw, ch):
    # bottom-up rows; cy given from TOP of image for sanity -> convert
    tot = 0; n = 0; mx = 0; changed = 0
    for yy in range(cy, cy + ch):
        row = (h - 1 - yy) * w
        for xx in range(cx, cx + cw):
            i = (row + xx) * 4
            d = max(abs(a[i]-b[i]), abs(a[i+1]-b[i+1]), abs(a[i+2]-b[i+2]))
            tot += d; n += 1
            if d > mx: mx = d
            if d > 2: changed += 1
    return tot / n, mx, 100.0 * changed / n

def load_fog(path):
    # [int32 w][int32 h][RGBA16F bottom-up]; returns (w, h, list of floats, 4 per texel)
    with open(path, 'rb') as f:
        w, h = struct.unpack('<ii', f.read(8))
        vals = list(struct.unpack('<%de' % (w * h * 4), f.read(w * h * 8)))
    return w, h, vals

def fog_luma(v, i):
    return 0.299 * v[i] + 0.587 * v[i + 1] + 0.114 * v[i + 2]

def fog_crop_delta(a, b, w, h, cx, cy, cw, ch):
    # relative flicker of the in-scattered light plus the transmittance's own
    # mean |delta| (A is 0..1 already, so it is reported absolute)
    tot = 0.0; lum = 0.0; ta = 0.0; n = 0; mx = 0.0; big = 0
    for yy in range(cy, cy + ch):
        row = (h - 1 - yy) * w
        for xx in range(cx, cx + cw):
            i = (row + xx) * 4
            la = fog_luma(a, i); lb = fog_luma(b, i)
            d = abs(la - lb)
            tot += d; lum += la; n += 1
            if d > mx: mx = d
            ta += abs(a[i + 3] - b[i + 3])
    lum /= max(n, 1)
    rel = (tot / n) / lum if lum > 0 else 0.0
    if lum > 0:
        big = 0
        for yy in range(cy, cy + ch):
            row = (h - 1 - yy) * w
            for xx in range(cx, cx + cw):
                i = (row + xx) * 4
                if abs(fog_luma(a, i) - fog_luma(b, i)) > 0.05 * lum: big += 1
    return lum, tot / n, rel, (mx / lum if lum > 0 else 0.0), 100.0 * big / n, ta / n

def burst_bands(seqs, n):
    # seqs: list of per-texel luma sequences of length n (a burst of CONSECUTIVE
    # frames). Per texel remove the mean, DFT, and split the power into the
    # VISIBLE band (periods n .. 2.67, bins 1..n/2-1) and the NYQUIST bin
    # (period 2, bin n/2). Returns (rms_visible, rms_nyquist) over all texels.
    #
    # WHY: a consecutive-frame delta is dominated by the period-2 alternation,
    # which a blue-in-time sequence carries MORE of by design while it takes
    # error OUT of the low temporal frequencies -- and at 80-120 fps a period-2
    # alternation at sub-1% contrast is below flicker fusion. The eye sees the
    # low band. Measured 2026-09-03: the STBN table read slightly WORSE than
    # the old one on the consecutive delta and far better on this split.
    import cmath, math
    tw = [cmath.exp(-2j * math.pi * k / n) for k in range(n)]
    vis = 0.0; nyq = 0.0; m = 0
    half = n // 2
    for seq in seqs:
        mean = sum(seq) / n
        c = [x - mean for x in seq]
        for k in range(1, half + 1):
            acc = 0j
            for t in range(n):
                acc += c[t] * tw[(k * t) % n]
            p = (abs(acc) ** 2) / (n * n)
            if k == half: nyq += p
            else: vis += p
        m += 1
    return math.sqrt(2.0 * vis / m), math.sqrt(nyq / m)

def fog_burst(paths):
    n = len(paths)
    vals = [load_fog(p) for p in paths]
    w, h = vals[0][0], vals[0][1]
    cw, ch = w // 4, h // 4
    cx, cy = (w - cw) // 2, (h - ch) // 2 - h // 16
    seqs = []; lum = 0.0
    for yy in range(cy, cy + ch):
        row = (h - 1 - yy) * w
        for xx in range(cx, cx + cw):
            i = (row + xx) * 4
            seqs.append([fog_luma(v[2], i) for v in vals])
            lum += seqs[-1][0]
    lum /= len(seqs)
    rv, rn = burst_bands(seqs, n)
    return lum, rv / lum, rn / lum

def frame_burst(paths):
    n = len(paths)
    vals = [load(p) for p in paths]
    w, h = vals[0][0], vals[0][1]
    cw, ch = w // 4, h // 4
    cx, cy = (w - cw) // 2, (h - ch) // 2 - h // 16
    seqs = []
    for yy in range(cy, cy + ch, 2):          # every other pixel: the burst is 4x the work of a pair
        row = (h - 1 - yy) * w
        for xx in range(cx, cx + cw, 2):
            i = (row + xx) * 4
            seqs.append([0.299 * v[2][i] + 0.587 * v[2][i + 1] + 0.114 * v[2][i + 2] for v in vals])
    return burst_bands(seqs, n)

def frame_darkspots(path, box=12, thresh=0.15):
    # the same local-contrast tail on the COMPOSITED frame's crosshair crop (8-bit
    # luma): what the eye is offered after the upsample, the composite, the
    # scaler and the gamma. box 12 px ~= 3 fog texels at his 4x magnification.
    w, h, data = load(path)
    cw, ch = w // 4, h // 4
    cx, cy = (w - cw) // 2, (h - ch) // 2 - h // 16
    x0, x1 = max(0, cx - box), min(w, cx + cw + box)
    y0, y1 = max(0, cy - box), min(h, cy + ch + box)
    W = x1 - x0; H = y1 - y0
    def L(xx, yy):
        i = ((h - 1 - yy) * w + xx) * 4
        return 0.299 * data[i] + 0.587 * data[i + 1] + 0.114 * data[i + 2] + 0.5
    lum = [[L(x0 + i, y0 + j) for i in range(W)] for j in range(H)]
    S = [[0.0] * (W + 1) for _ in range(H + 1)]
    for j in range(H):
        acc = 0.0
        for i in range(W):
            acc += lum[j][i]
            S[j + 1][i + 1] = S[j][i + 1] + acc
    cs = []
    for yy in range(cy, cy + ch, 2):
        for xx in range(cx, cx + cw, 2):
            ax = max(x0, xx - box) - x0; bx = min(x1, xx + box + 1) - x0
            ay = max(y0, yy - box) - y0; by = min(y1, yy + box + 1) - y0
            m = (S[by][bx] - S[ay][bx] - S[by][ax] + S[ay][ax]) / ((bx - ax) * (by - ay))
            cs.append(lum[yy - y0][xx - x0] / m - 1.0)
    cs.sort(); n = len(cs)
    tail = [c for c in cs if c < -thresh]
    return cs[n // 20], 100.0 * len(tail) / n, (sum(tail) / len(tail) if tail else 0.0), cs[n // 2]

def fog_darkspots(path, box=4, thresh=0.15, outpng=None, gain=3.0):
    # THE DARK-SPOT INSTRUMENT (2026-09-03, Seb: "prominent soft dark spots that
    # mainly resolve when we stand still"). Per texel of the fog buffer's
    # crosshair crop, the LOCAL contrast against a (2*box+1)^2 box mean of the
    # same frame, c = v/mean - 1. A stochastic-pick zero that the 5x5 filter has
    # spread into a soft blob is a NEGATIVE excursion a few texels wide; the
    # visible-band RMS averages it away, this reads the tail: the 5th percentile
    # of c, the share of texels below -thresh, and the mean depth of that tail.
    # A depth edge is a genuine local contrast too, so read an arm against the
    # frozen-pick control (rt_metal_lightsample 0) on the same frames and quote
    # the excess. outpng writes the contrast map: mid-grey 0, darker = negative,
    # gain levels per unit contrast / 128.
    w, h, v = load_fog(path)
    cw, ch = w // 4, h // 4
    cx, cy = (w - cw) // 2, (h - ch) // 2 - h // 16
    def L(xx, yy):
        return fog_luma(v, ((h - 1 - yy) * w + xx) * 4)
    # integral image of luma over the crop plus halo
    x0, x1 = max(0, cx - box), min(w, cx + cw + box)
    y0, y1 = max(0, cy - box), min(h, cy + ch + box)
    W = x1 - x0; H = y1 - y0
    lum = [[L(x0 + i, y0 + j) for i in range(W)] for j in range(H)]
    S = [[0.0] * (W + 1) for _ in range(H + 1)]
    for j in range(H):
        acc = 0.0
        for i in range(W):
            acc += lum[j][i]
            S[j + 1][i + 1] = S[j][i + 1] + acc
    cs = []
    rows = []
    for yy in range(cy, cy + ch):
        line = bytearray()
        for xx in range(cx, cx + cw):
            ax = max(x0, xx - box) - x0; bx = min(x1, xx + box + 1) - x0
            ay = max(y0, yy - box) - y0; by = min(y1, yy + box + 1) - y0
            m = (S[by][bx] - S[ay][bx] - S[by][ax] + S[ay][ax]) / ((bx - ax) * (by - ay))
            c = (lum[yy - y0][xx - x0] / m - 1.0) if m > 1e-6 else 0.0
            cs.append(c)
            g = int(min(255, max(0, 128 + c * gain * 128)))
            line += bytes((g, g, g))
        rows.append(bytes(line))
    cs.sort()
    n = len(cs)
    p5 = cs[n // 20]
    tail = [c for c in cs if c < -thresh]
    if outpng:
        raw = b''.join(b'\x00' + r for r in rows)
        def chunk(t, d):
            c = t + d
            return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
        png = (b'\x89PNG\r\n\x1a\n'
               + chunk(b'IHDR', struct.pack('>IIBBBBB', cw, ch, 8, 2, 0, 0, 0))
               + chunk(b'IDAT', zlib.compress(raw))
               + chunk(b'IEND', b''))
        open(outpng, 'wb').write(png)
    return p5, 100.0 * len(tail) / n, (sum(tail) / len(tail) if tail else 0.0), cs[n // 2]

def fog_topng(path, out, gain=1.0):
    w, h, v = load_fog(path)
    rows = []
    for yy in range(h):
        srow = (h - 1 - yy) * w
        line = bytearray()
        for xx in range(w):
            i = (srow + xx) * 4
            line += bytes((int(min(255, max(0, v[i] * gain * 255))),
                           int(min(255, max(0, v[i + 1] * gain * 255))),
                           int(min(255, max(0, v[i + 2] * gain * 255)))))
        rows.append(bytes(line))
    raw = b''.join(b'\x00' + r for r in rows)
    def chunk(t, d):
        c = t + d
        return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw))
           + chunk(b'IEND', b''))
    open(out, 'wb').write(png)

def topng(path, out, cx=None, cy=None, cw=None, ch=None, scale=1):
    w, h, data = load(path)
    if cx is None: cx, cy, cw, ch = 0, 0, w, h
    rows = []
    for yy in range(cy, cy + ch):
        srow = (h - 1 - yy) * w
        line = bytearray()
        for xx in range(cx, cx + cw):
            i = (srow + xx) * 4
            px = bytes((min(255, data[i]*scale), min(255, data[i+1]*scale), min(255, data[i+2]*scale)))
            line += px
        rows.append(bytes(line))
    raw = b''.join(b'\x00' + r for r in rows)
    def chunk(t, d):
        c = t + d
        return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', cw, ch, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw))
           + chunk(b'IEND', b''))
    with open(out, 'wb') as f:
        f.write(png)

if __name__ == '__main__':
    mode = sys.argv[1]
    if mode == 'seq':
        # spk.py seq <prefix> f1 f2 ... : consecutive deltas in the crosshair crop
        pre = sys.argv[2]
        frames = sys.argv[3:]
        w, h, _ = load(pre + '.f' + frames[0])
        # crosshair crop: centred, slightly above centre (the corridor view),
        # 25% of width x 25% of height
        cw, ch = w // 4, h // 4
        cx, cy = (w - cw) // 2, (h - ch) // 2 - h // 16
        print(f'frame {w}x{h}, crop {cw}x{ch} at {cx},{cy}')
        prev = None
        for fr in frames:
            _, _, d = load(pre + '.f' + fr)
            if prev is not None:
                m, mx, pct = crop_delta(prev[1], d, w, h, cx, cy, cw, ch)
                print(f'  f{prev[0]} -> f{fr}: mean {m:.3f}  max {mx}  >2: {pct:.1f}%')
            prev = (fr, d)
    elif mode == 'fogseq':
        # flicker.py fogseq <prefix> f1 f2 ... : consecutive RELATIVE deltas of
        # the fog buffer's crosshair crop (reads <prefix>.f<N>.fog)
        pre = sys.argv[2]
        frames = sys.argv[3:]
        w, h, _ = load_fog(pre + '.f' + frames[0] + '.fog')
        cw, ch = w // 4, h // 4
        cx, cy = (w - cw) // 2, (h - ch) // 2 - h // 16
        print(f'fog {w}x{h}, crop {cw}x{ch} at {cx},{cy}')
        prev = None
        rels = []
        for fr in frames:
            _, _, d = load_fog(pre + '.f' + fr + '.fog')
            if prev is not None:
                lum, m, rel, mxr, pct, ta = fog_crop_delta(prev[1], d, w, h, cx, cy, cw, ch)
                rels.append(rel)
                print(f'  f{prev[0]} -> f{fr}: luma {lum:.4f}  |d| {m:.5f}  rel {100*rel:.2f}%  max {100*mxr:.0f}%  >5%: {pct:.1f}%  |dT| {ta:.4f}')
            prev = (fr, d)
        if rels:
            rels.sort()
            print(f'  median rel flicker {100*rels[len(rels)//2]:.2f}%  mean {100*sum(rels)/len(rels):.2f}%')
    elif mode == 'burst':
        # flicker.py burst <prefix> f1 f2 ... fN : the temporal-band split of a burst
        # of CONSECUTIVE frames, both the frame crop (8-bit luma levels) and the fog
        # buffer crop (relative to its mean luma): RMS in the visible band (periods
        # N..2.67) and at Nyquist (period 2, invisible at play frame rates)
        pre = sys.argv[2]; frames = sys.argv[3:]
        fv, fn = frame_burst([pre + '.f' + f for f in frames])
        lum, gv, gn = fog_burst([pre + '.f' + f + '.fog' for f in frames])
        print(f'burst f{frames[0]}-{frames[-1]} ({len(frames)} frames): frame visible-band rms {fv:.3f} levels, nyquist {fn:.3f} | fog visible {100*gv:.2f}%  nyquist {100*gn:.2f}%  (crop luma {lum:.3f})')
    elif mode == 'darkspots':
        # flicker.py darkspots <prefix> f1 f2 ... [--png <outprefix>] : the fog
        # buffer's dark-tail statistics per frame (median over the frames last)
        args = sys.argv[2:]
        outp = None
        if '--png' in args:
            i = args.index('--png'); outp = args[i + 1]; args = args[:i] + args[i + 2:]
        onframe = '--frame' in args
        if onframe: args.remove('--frame')
        pre = args[0]; frames = args[1:]
        p5s = []; shares = []; depths = []
        for f in frames:
            if onframe:
                p5, share, depth, med = frame_darkspots(pre + '.f' + f)
            else:
                p5, share, depth, med = fog_darkspots(pre + '.f' + f + '.fog', outpng=(outp + '.f' + f + '.png') if outp else None)
            p5s.append(p5); shares.append(share); depths.append(depth)
            print(f'  f{f}: local-contrast p5 {100*p5:+.1f}%  below -15%: {share:.2f}% of crop  tail mean {100*depth:+.1f}%  median {100*med:+.2f}%')
        p5s.sort(); shares.sort(); depths.sort()
        k = len(frames) // 2
        print(f'  median over {len(frames)} frames: p5 {100*p5s[k]:+.1f}%  share below -15% {shares[k]:.2f}%  tail mean {100*depths[k]:+.1f}%')
    elif mode == 'fogpng':
        # flicker.py fogpng <in.fog> <out.png> [gain]
        fog_topng(sys.argv[2], sys.argv[3], float(sys.argv[4]) if len(sys.argv) > 4 else 1.0)
    elif mode == 'diff':
        # flicker.py diff <frameA> <frameB> <out.png> [gain] : amplified
        # per-pixel max-channel difference image -- the localiser.
        gain = int(sys.argv[5]) if len(sys.argv) > 5 else 8
        w, h, a = load(sys.argv[2]); _, _, b = load(sys.argv[3])
        rows = []
        for yy in range(h):
            srow = (h - 1 - yy) * w; line = bytearray()
            for xx in range(w):
                i = (srow + xx) * 4
                d = min(255, gain * max(abs(a[i]-b[i]), abs(a[i+1]-b[i+1]), abs(a[i+2]-b[i+2])))
                line += bytes((d, d, d))
            rows.append(bytes(line))
        raw = b''.join(b'\x00' + r for r in rows)
        def chunk(t, d):
            c = t + d
            return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
        png = (b'\x89PNG\r\n\x1a\n'
               + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
               + chunk(b'IDAT', zlib.compress(raw))
               + chunk(b'IEND', b''))
        open(sys.argv[4], 'wb').write(png)
    elif mode == 'png':
        # spk.py png <in> <out> [crop] [scale]
        crop = None
        if len(sys.argv) > 4 and sys.argv[4] == 'crop':
            w, h, _ = load(sys.argv[2])
            cw, ch = w // 4, h // 4
            cx, cy = (w - cw) // 2, (h - ch) // 2 - h // 16
            sc = int(sys.argv[5]) if len(sys.argv) > 5 else 1
            topng(sys.argv[2], sys.argv[3], cx, cy, cw, ch, sc)
        else:
            topng(sys.argv[2], sys.argv[3])
