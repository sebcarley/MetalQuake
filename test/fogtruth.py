#!/usr/bin/env python3
# test/fogtruth.py -- a fog arm against the MEAN-OF-REALISATIONS truth
# (SEPTEMBER2 A1, 2026-09-06; the 2026-09-03 evening's method, committed).
#
# A single 1-spp fog frame is a SKEWED truth (CLAUDE.md, 2026-09-03): the light
# pick makes each cast lit or black, so a raw frame sits below its mean more
# often than above it, and any smoother arm compared against one raw frame
# reads "brighter than truth" at the median. The truth is the MEAN of N
# independent realisations of the same playback frame -- N runs of
# test/fogbed.sh with RT_METAL_FRAMEOFFSET=<distinct> at rt_metal_fog_history
# 0.01 (NOT 0: the kernels ignore the frame counter at history 0 by design,
# so eight offsets at 0 give eight identical frames) and every history pass
# off. An arm's error is then mean |arm - truth| over the truth's luma, on the
# crosshair crop flicker.py uses and over the whole buffer; the signed
# per-texel relative error's median and tails say whether an arm is biased or
# merely noisy. Recorded references on demo23's fast strafe (f2340-2347, his
# config, 2026-09-03): a raw 1-spp frame 1.72%, his 0.7 rotation-only history
# 2.24%, the clamp at 0.8 2.48%, any 2D history at 0.95 4.4-8.4%.
#
# Usage: python3 test/fogtruth.py <truthdir> <N> <armprefix> f1 f2 ...
#   reads <truthdir>/r<1..N>.f<F>.fog and <armprefix>.f<F>.fog (fogbed's names)
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from flicker import load_fog, fog_luma

def crop_rect(w, h):
    cw, ch = w // 4, h // 4
    return (w - cw) // 2, (h - ch) // 2 - h // 16, cw, ch

def mean_truth(paths):
    w = h = None; acc = None
    for p in paths:
        pw, ph, v = load_fog(p)
        if acc is None:
            w, h = pw, ph; acc = [0.0] * len(v)
        assert (pw, ph) == (w, h), 'truth realisations differ in size'
        for i, x in enumerate(v): acc[i] += x
    n = float(len(paths))
    return w, h, [x / n for x in acc]

def compare(truth, arm, w, h, cx, cy, cw, ch):
    # returns (crop luma, crop mean|err|/luma, whole mean|err|/luma, median signed rel err, p5, p95, mean |dT| crop)
    def region(x0, y0, rw, rh):
        tot = 0.0; lum = 0.0; n = 0; rels = []; dT = 0.0
        for yy in range(y0, y0 + rh):
            row = (h - 1 - yy) * w
            for xx in range(x0, x0 + rw):
                i = (row + xx) * 4
                lt = fog_luma(truth, i); la = fog_luma(arm, i)
                tot += abs(la - lt); lum += lt; n += 1
                dT += abs(arm[i + 3] - truth[i + 3])
                if lt > 1e-4: rels.append((la - lt) / lt)
        lum /= max(n, 1)
        rels.sort()
        med = rels[len(rels) // 2] if rels else 0.0
        p5 = rels[len(rels) // 20] if rels else 0.0
        p95 = rels[(len(rels) * 19) // 20] if rels else 0.0
        return lum, (tot / n) / lum if lum > 0 else 0.0, med, p5, p95, dT / n
    cl, crel, cmed, cp5, cp95, cdT = region(cx, cy, cw, ch)
    _, wrel, _, _, _, _ = region(0, 0, w, h)
    return cl, crel, wrel, cmed, cp5, cp95, cdT

if __name__ == '__main__':
    if len(sys.argv) < 5:
        print(__doc__ or 'usage: fogtruth.py <truthdir> <N> <armprefix> f1 f2 ...'); sys.exit(2)
    tdir = sys.argv[1]; N = int(sys.argv[2]); arm = sys.argv[3]; frames = sys.argv[4:]
    crels = []; wrels = []; meds = []
    for fr in frames:
        w, h, truth = mean_truth([os.path.join(tdir, 'r%d.f%s.fog' % (n, fr)) for n in range(1, N + 1)])
        aw, ah, av = load_fog(arm + '.f' + fr + '.fog')
        assert (aw, ah) == (w, h), 'arm and truth differ in size'
        cx, cy, cw, ch = crop_rect(w, h)
        cl, crel, wrel, med, p5, p95, dT = compare(truth, av, w, h, cx, cy, cw, ch)
        crels.append(crel); wrels.append(wrel); meds.append(med)
        print(f'  f{fr}: truth luma {cl:.4f}  err crop {100*crel:.2f}%  whole {100*wrel:.2f}%  signed median {100*med:+.2f}%  p5 {100*p5:+.1f}%  p95 {100*p95:+.1f}%  |dT| {dT:.4f}')
    n = len(frames)
    print(f'  MEAN over {n} frames: err crop {100*sum(crels)/n:.2f}%  whole {100*sum(wrels)/n:.2f}%  signed median {100*sum(meds)/n:+.2f}%')
