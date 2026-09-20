#!/usr/bin/env python3
"""ghost.py -- the temporal scaler's GHOST FRACTION, split by WHAT is moving.

The question this answers: on a frame where something moved, how much of the
PREVIOUS frame is still lingering in a temporally-upscaled arm -- and is it the
thing that moved, the light it threw on still geometry, or the particles it
left behind?  It is the instrument the MetalFX-temporal arc used to find that
the measured "rocket ghost" was never the rocket (126 px of model against
10,400 px of change, 78% of it the smoke trail), and it lived only as a
heredoc until 2026-08-19.  This is its committed form.

THE FRACTION.  Per pixel, with the bilinear arm (no scaler, no history) as the
truth:  d = bil[N-1] - bil[N]  (how the truth changed)  and  e = arm[N] - bil[N]
(how the arm deviates from the truth).  A history-carrying arm that lingers
satisfies  arm[N] ~= bil[N] + a * (bil[N-1] - bil[N]),  so the least-squares
a = sum(e*d) / sum(d*d) over the selected pixels is literally the fraction of
the previous frame still present: 0 = none, 1 = the whole previous frame.
Pixels with |d| < T are excluded (T = 6 of 255 by default -- below that the
8-bit dump is the quantiser, not the scene).

THE CONTROL.  The SPATIAL arm (r_metalfx 1) upscales but keeps no history, so
a sound reading of this metric must put it at ~0.  It is printed per class
beside every arm, and the script REFUSES (exit 2) if it exceeds |0.03| on a
class with enough pixels to mean anything -- when a metric's own control reads
impossible, the metric is broken for that subject (the viewmodel lesson: a
subject that does not change on screen starves the denominator).

THE SPLIT.  An ORACLE boot -- the bilinear arm with cl_particles 0, which dumps
the same frames -- marks where particles are.  Three classes:
    now    particle pixels in frame N          (|bil[N] - nopart[N]| > T)
    was    particle pixels in N-1 but not N   (the trailing edge, where a puff
                                               WAS and the history still has it)
    other  everything else                    (the light on geometry, the
                                               moving object itself)
Both oracle frames come from ONE boot, so the classes share a timeline.  The
old two-way split counted the trailing edge under "other"; this one does not.
Classes are evaluated over the whole frame minus the HUD rows, and over the
historic crop 820 440 1140 760 for continuity with the recorded 0.338 / 0.283.

THE CRAWL ROW, and why it exists.  a ~= 0 cannot tell "history correctly kept"
from "history thrown away": a reactive mask that over-marks (too high a gain,
or a trail stamp that marks geometry whose history was right) scores as
success on a while re-introducing unconverged twinkle where it fired.  So each
class also prints the frame-to-frame change of the HIGH-PASSED image (the
5x5-box residual, the crawl) for truth / arm / control.  Over-masking shows as
the arm's crawl rising ABOVE the truth's; an arm that merely stops lingering
sits at or below it.

THRESHOLD SWEEP.  The a rows are repeated at T = 4 / 6 / 10: a row that moves
with T is a threshold artefact, not a finding.

FLOOR MODE.  --floor A B compares two boots of ONE arm (% px differing, mean
and max channel delta, and the 16x16 blocks touched) so the bed's own noise
is on the same printout as the number it qualifies.  Demo dumps on the Metal
path are a METRIC bed, not a byte bed: majority-state boots agree to a few
tenths of a percent, localised; an odd boot is a >50% global wash and is
rerun once (the demo11 rule).

FORMAT.  RT_METAL_DUMP raw: [int32 w][int32 h] then w*h*4 RGBA8, bottom-up
(rt_metal.m, "The dump file").  Arms are given as <prefix>, frames appended
as <prefix>.f<N>.  Every arm must share the dump geometry; a mismatch is a
refusal (the titled-window clamp trap), not a warning.  Note the dump is an
8-bit clamp of whatever the scene buffer held -- under r_edr a saturated flare
is 255 in every arm and ghost inside it is invisible to this script.

USAGE
  python3 test/ghost.py --truth DIR/bil --control DIR/spa --oracle DIR/nopart \\
      --frames 417-421 m0=DIR/m0 m3p0=DIR/m3p0 m3p3=DIR/m3p3 [--tsv out.tsv]
  python3 test/ghost.py --floor DIR/m3p3 DIR/m3p3b --frames 417-421

Exit status: 0 fine; 2 the control refused or geometry mismatched; 1 usage.
"""
import sys, struct, argparse
from array import array


# --- loading ---------------------------------------------------------------

def load_raw(path):
    """(w, h, lum, rgba): lum is a flat top-down array('f') of (r+g+b)/3;
    rgba is the raw top-down byte string (for the floor mode)."""
    try:
        d = open(path, 'rb').read()
    except OSError as e:
        raise SystemExit('%s: %s' % (path, e))
    if len(d) < 8:
        raise SystemExit('%s: not a dump (too short)' % path)
    w, h = struct.unpack('<ii', d[:8])
    px = d[8:]
    if w <= 0 or h <= 0 or len(px) < w * h * 4:
        raise SystemExit('%s: header says %dx%d but %d bytes follow' % (path, w, h, len(px)))
    lum = array('f')
    rows = []
    for y in range(h - 1, -1, -1):                 # bottom-up -> top-down
        row = px[y * w * 4:(y + 1) * w * 4]
        rows.append(row)
        lum.extend([(a + b + c) / 3.0 for a, b, c in zip(row[0::4], row[1::4], row[2::4])])
    return w, h, lum, b''.join(rows)


def parse_frames(s):
    out = []
    for part in s.split(','):
        part = part.strip()
        if '-' in part:
            a, b = part.split('-', 1)
            out.extend(range(int(a), int(b) + 1))
        elif part:
            out.append(int(part))
    if len(out) < 2:
        raise SystemExit('--frames needs at least two consecutive frames (got %s)' % s)
    return out


class Arm:
    def __init__(self, name, prefix):
        self.name = name
        self.prefix = prefix
        self.frames = {}        # frame -> (lum, rgba)
        self.geom = None

    def get(self, frame):
        if frame not in self.frames:
            w, h, lum, rgba = load_raw('%s.f%d' % (self.prefix, frame))
            if self.geom is None:
                self.geom = (w, h)
            elif self.geom != (w, h):
                raise SystemExit('%s: frame %d is %dx%d, earlier frames %dx%d' % (self.prefix, frame, w, h, self.geom[0], self.geom[1]))
            self.frames[frame] = (lum, rgba)
        return self.frames[frame]


# --- the high-pass (5x5 box residual) for the crawl row --------------------

def box5(lum, w, h):
    """Flat top-down 5x5 box mean (clamped at the borders), via row then
    column prefix sums.  Pure Python, ~2-3 s at 1080p; cached per arm/frame."""
    # horizontal pass
    hsum = [0.0] * (w * h)
    for y in range(h):
        base = y * w
        row = lum[base:base + w]
        pre = [0.0] * (w + 1)
        acc = 0.0
        for x in range(w):
            acc += row[x]
            pre[x + 1] = acc
        for x in range(w):
            x0 = x - 2 if x >= 2 else 0
            x1 = x + 3 if x + 3 <= w else w
            hsum[base + x] = (pre[x1] - pre[x0]) / (x1 - x0)
    # vertical pass
    out = [0.0] * (w * h)
    for x in range(w):
        pre = [0.0] * (h + 1)
        acc = 0.0
        for y in range(h):
            acc += hsum[y * w + x]
            pre[y + 1] = acc
        for y in range(h):
            y0 = y - 2 if y >= 2 else 0
            y1 = y + 3 if y + 3 <= h else h
            out[y * w + x] = (pre[y1] - pre[y0]) / (y1 - y0)
    return out


_hp_cache = {}

def highpass(arm, frame, w, h):
    key = (arm.name, frame)
    if key not in _hp_cache:
        lum = arm.get(frame)[0]
        b = box5(lum, w, h)
        _hp_cache[key] = [l - m for l, m in zip(lum, b)]
    return _hp_cache[key]


# --- the fraction ------------------------------------------------------------

MIN_SIGNAL = 50     # a fraction fitted on fewer pixels than this is printed as n/a

def fraction(truth_prev, truth_cur, arm_cur, sel, T):
    """Least-squares a over the pixels in sel (an iterable of flat indices)
    where |d| >= T.  Returns (a, n_selected, n_signal, rms_residual)."""
    num = 0.0
    den = 0.0
    n = 0
    nsel = 0
    res = 0.0
    for i in sel:
        nsel += 1
        d = truth_prev[i] - truth_cur[i]
        if d < T and d > -T:
            continue
        e = arm_cur[i] - truth_cur[i]
        num += e * d
        den += d * d
        n += 1
    a = num / den if den > 0.0 else 0.0
    if n:
        for i in sel:
            d = truth_prev[i] - truth_cur[i]
            if d < T and d > -T:
                continue
            e = arm_cur[i] - truth_cur[i]
            r = e - a * d
            res += r * r
        res = (res / n) ** 0.5
    return a, nsel, n, res


def crawl(hp_prev, hp_cur, sel):
    """Mean |hp[N] - hp[N-1]| over sel -- the frame-to-frame high-frequency
    change.  Returns (mean, n)."""
    acc = 0.0
    n = 0
    for i in sel:
        acc += abs(hp_cur[i] - hp_prev[i])
        n += 1
    return (acc / n if n else 0.0), n


# --- regions and classes -----------------------------------------------------

def region_indices(w, h, top, bottom, crop):
    """Flat indices of the region: the crop if given, else the whole frame
    minus `top` rows at the top and `bottom` rows at the bottom (the HUD)."""
    if crop:
        x0, y0, x1, y1 = crop
        x0 = max(0, x0); y0 = max(0, y0); x1 = min(w, x1); y1 = min(h, y1)
        return [y * w + x for y in range(y0, y1) for x in range(x0, x1)]
    return [y * w + x for y in range(top, h - bottom) for x in range(w)]


def classes(region, truth_prev, truth_cur, ora_prev, ora_cur, T):
    """Split region into now / was / other by the oracle.  With no previous
    oracle frame (ora_prev None) the trailing edge cannot be told from the
    light on geometry, and the split is the old two-way now / rest."""
    now = []
    was = []
    other = []
    for i in region:
        if abs(truth_cur[i] - ora_cur[i]) > T:
            now.append(i)
            continue
        if ora_prev is not None and abs(truth_prev[i] - ora_prev[i]) > T:
            was.append(i)
        else:
            other.append(i)
    if ora_prev is None:
        return {'now': now, 'rest': other}
    return {'now': now, 'was': was, 'other': other}


# --- floor mode ----------------------------------------------------------------

def floor_compare(pa, pb, frames):
    A = Arm('A', pa)
    B = Arm('B', pb)
    print('FLOOR: two boots of one arm -- %s vs %s' % (pa, pb))
    print('%-6s %10s %8s %6s %8s  %s' % ('frame', 'px differ', '%px', 'max', 'mean', 'blocks16 touched / bbox'))
    worst = 0.0
    for f in frames:
        la, ra = A.get(f)
        lb, rb = B.get(f)
        if A.geom != B.geom:
            raise SystemExit('geometry mismatch: %s %s vs %s %s' % (pa, A.geom, pb, B.geom))
        w, h = A.geom
        n = w * h
        ndiff = 0
        total = 0
        mx = 0
        bx0 = w; by0 = h; bx1 = -1; by1 = -1
        blocks = set()
        # channel deltas from the raw bytes; pixel differs if any channel does
        for y in range(h):
            base = y * w * 4
            rowa = ra[base:base + w * 4]
            rowb = rb[base:base + w * 4]
            if rowa == rowb:
                continue
            for x in range(w):
                o = x * 4
                d0 = abs(rowa[o] - rowb[o]); d1 = abs(rowa[o + 1] - rowb[o + 1]); d2 = abs(rowa[o + 2] - rowb[o + 2])
                if d0 or d1 or d2:
                    ndiff += 1
                    total += d0 + d1 + d2
                    m = max(d0, d1, d2)
                    if m > mx:
                        mx = m
                    blocks.add((x >> 4, y >> 4))
                    if x < bx0: bx0 = x
                    if x > bx1: bx1 = x
                    if y < by0: by0 = y
                    if y > by1: by1 = y
        pct = 100.0 * ndiff / n
        worst = max(worst, pct)
        mean = total / (3.0 * n)
        bbox = ('%d,%d-%d,%d' % (bx0, by0, bx1, by1)) if ndiff else '-'
        print('%-6d %10d %7.3f%% %6d %8.4f  %d / %s' % (f, ndiff, pct, mx, mean, len(blocks), bbox))
    if worst == 0.0:
        print('floor: BYTE-IDENTICAL on every frame -- a byte bed for this arm class')
    elif worst <= 0.7:
        print('floor: <= 0.7% px, localised -- a METRIC bed; quote numbers against this floor')
    elif worst > 50.0:
        print('floor: >50% of the frame -- the odd-boot class; rerun once before concluding anything')
    else:
        print('floor: %.2f%% px -- wider than the demo11 rule allows; characterise before quoting' % worst)
    return 0


# --- main ------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('arms', nargs='*', help='name=<prefix> ... the arms under test')
    ap.add_argument('--truth', help='<prefix> of the bilinear arm (r_metalfx 0)')
    ap.add_argument('--control', help='<prefix> of the spatial arm (r_metalfx 1), the no-history control')
    ap.add_argument('--oracle', help='<prefix> of the cl_particles 0 bilinear boot')
    ap.add_argument('--frames', default='417-421', help='e.g. 417-421 or 417,418,419')
    ap.add_argument('--thresh', type=float, default=6.0, help='|d| threshold, of 255 (default 6)')
    ap.add_argument('--sweep', default='4,6,10', help='thresholds for the sensitivity rows')
    ap.add_argument('--crop', nargs=4, type=int, default=[820, 440, 1140, 760], metavar=('X0', 'Y0', 'X1', 'Y1'))
    ap.add_argument('--no-crop', action='store_true', help='skip the crop rows')
    ap.add_argument('--top', type=int, default=24, help='rows excluded at the top (notify text)')
    ap.add_argument('--bottom', type=int, default=120, help='rows excluded at the bottom (the HUD)')
    ap.add_argument('--no-crawl', action='store_true', help='skip the crawl rows (faster)')
    ap.add_argument('--tsv', help='write a machine-readable table here')
    ap.add_argument('--floor', nargs=2, metavar=('A', 'B'), help='compare two boots of one arm and exit')
    ap.add_argument('--control-limit', type=float, default=0.03, help='|a| above which the control refuses')
    args = ap.parse_args()

    frames = parse_frames(args.frames)
    if args.floor:
        return floor_compare(args.floor[0], args.floor[1], frames)
    if not args.truth or not args.arms:
        ap.error('--truth and at least one name=<prefix> arm are required (or --floor A B)')

    truth = Arm('truth', args.truth)
    control = Arm('control', args.control) if args.control else None
    oracle = Arm('oracle', args.oracle) if args.oracle else None
    arms = []
    for spec in args.arms:
        if '=' not in spec:
            ap.error('arm "%s" must be name=<prefix>' % spec)
        n, p = spec.split('=', 1)
        arms.append(Arm(n, p))

    sweep = [float(t) for t in args.sweep.split(',')]
    if args.thresh not in sweep:
        sweep = [args.thresh] + sweep

    # geometry from the truth's first frame; every arm is checked against it
    w, h = None, None
    truth.get(frames[0])
    w, h = truth.geom
    for a in [control, oracle] + arms:
        if a is None:
            continue
        # the oracle may lack the first frame (see the WARNING below); its last
        # frame is always needed, so check geometry on that one
        a.get(frames[-1] if a is oracle else frames[0])
        if a.geom != (w, h):
            raise SystemExit('geometry mismatch: %s is %dx%d, truth is %dx%d' % (a.prefix, a.geom[0], a.geom[1], w, h))

    print('ghost.py: %dx%d dumps, frames %s, T=%g (sweep %s), crop %s, rows %d..%d' % (
        w, h, ','.join(str(f) for f in frames), args.thresh, ','.join('%g' % t for t in sweep),
        'off' if args.no_crop else ' '.join(str(c) for c in args.crop), args.top, h - args.bottom))
    print('  a = fraction of the previous frame lingering (0 = none); control must read ~0;')
    print('  crawl = mean |hp[N]-hp[N-1]| over the class (high-passed, 5x5 box), truth / arm / control.')
    print('  The dump is an 8-bit clamp: ghost inside saturated (255) pixels is invisible here.')

    tsv = open(args.tsv, 'w') if args.tsv else None
    if tsv:
        tsv.write('region\tclass\tframe\tT\tarm\ta\tn_sel\tn_sig\trms\tcrawl_truth\tcrawl_arm\tcrawl_control\n')

    regions = [('frame', None)]
    if not args.no_crop:
        regions.append(('crop', tuple(args.crop)))

    refused = False
    full_region_idx = {}
    for rname, crop in regions:
        full_region_idx[rname] = region_indices(w, h, args.top, args.bottom, crop)

    for fi in range(1, len(frames)):
        fprev, fcur = frames[fi - 1], frames[fi]
        if fcur != fprev + 1:
            print('WARNING: frames %d -> %d are not consecutive; the fraction assumes one frame of history' % (fprev, fcur))
        tprev = truth.get(fprev)[0]
        tcur = truth.get(fcur)[0]
        oprev = ocur = None
        if oracle:
            ocur = oracle.get(fcur)[0]
            try:
                oprev = oracle.get(fprev)[0]
            except SystemExit:
                # an oracle with only the current frame (the 2026-08-19 heredoc
                # bed dumped f419 alone): the "was" class cannot be formed, so
                # it is empty and the trailing edge stays inside "other" -- the
                # old two-way split, stated rather than silently produced
                print('WARNING: oracle frame %d missing -- no "was" class for f%d (two-way split now / rest)' % (fprev, fcur))
                oprev = None
        cprev = control.get(fprev)[0] if control else None
        ccur = control.get(fcur)[0] if control else None
        for rname, crop in regions:
            region = full_region_idx[rname]
            if oracle:
                cls = classes(region, tprev, tcur, oprev, ocur, args.thresh)
                # the oracle's own credentials: how many of the CHANGED pixels are particles
                chg = 0; chg_part = 0
                for i in region:
                    d = tprev[i] - tcur[i]
                    if d >= args.thresh or d <= -args.thresh:
                        chg += 1
                        if abs(tcur[i] - ocur[i]) > args.thresh:
                            chg_part += 1
                share = (100.0 * chg_part / chg) if chg else 0.0
                print('\n[%s] f%d: changed px %d, of which particle-now %d (%.0f%%); classes %s' % (
                    rname, fcur, chg, chg_part, share, ' / '.join('%s %d' % (k, len(v)) for k, v in cls.items())))
            else:
                cls = {'all': region}
                print('\n[%s] f%d: %d px (no oracle -- no split)' % (rname, fcur, len(region)))
            for cname, sel in cls.items():
                if not sel:
                    print('  %-5s (empty)' % cname)
                    continue
                # crawl of the truth and control, once per class
                crow = ''
                ct = ca = cc = None
                if not args.no_crawl:
                    hp_tp = highpass(truth, fprev, w, h); hp_tc = highpass(truth, fcur, w, h)
                    ct, _ = crawl(hp_tp, hp_tc, sel)
                    if control:
                        hp_cp = highpass(control, fprev, w, h); hp_cc = highpass(control, fcur, w, h)
                        cc, _ = crawl(hp_cp, hp_cc, sel)
                for T in sweep:
                    line = '  %-5s T=%-4g' % (cname, T)
                    # control first
                    if control:
                        a_c, nsel, nsig, rms_c = fraction(tprev, tcur, ccur, sel, T)
                        line += (' control %+.3f' % a_c) if nsig >= MIN_SIGNAL else ' control   n/a '
                        if T == args.thresh and nsig >= 500 and abs(a_c) > args.control_limit:
                            refused = True
                            line += ' <-- REFUSED (control above |%.2f| on %d px)' % (args.control_limit, nsig)
                    else:
                        nsel = len(sel); nsig = -1
                    for arm in arms:
                        acur = arm.get(fcur)[0]
                        a, nsel, nsig, rms = fraction(tprev, tcur, acur, sel, T)
                        line += ('  %s %+.3f' % (arm.name, a)) if nsig >= MIN_SIGNAL else ('  %s   n/a ' % arm.name)
                        if tsv:
                            cr_a = None
                            if not args.no_crawl and T == args.thresh:
                                hp_ap = highpass(arm, fprev, w, h); hp_ac = highpass(arm, fcur, w, h)
                                cr_a, _ = crawl(hp_ap, hp_ac, sel)
                            tsv.write('%s\t%s\t%d\t%g\t%s\t%.4f\t%d\t%d\t%.3f\t%s\t%s\t%s\n' % (
                                rname, cname, fcur, T, arm.name, a, nsel, nsig, rms,
                                '%.4f' % ct if ct is not None else '-', '%.4f' % cr_a if cr_a is not None else '-',
                                '%.4f' % cc if cc is not None else '-'))
                    line += '   [n_sel %d, n_sig %d]' % (nsel, nsig)
                    print(line)
                if not args.no_crawl:
                    crow = '  %-5s crawl   truth %.4f' % (cname, ct)
                    if control:
                        crow += '  control %.4f' % cc
                    for arm in arms:
                        hp_ap = highpass(arm, fprev, w, h); hp_ac = highpass(arm, fcur, w, h)
                        ca, _ = crawl(hp_ap, hp_ac, sel)
                        flag = ' ^' if ct and ca > ct * 1.05 else ''
                        crow += '  %s %.4f%s' % (arm.name, ca, flag)
                    print(crow + '    (^ = above the truth: over-masking or twinkle re-introduced)')
    if tsv:
        tsv.close()
        print('\nTSV: %s' % args.tsv)
    if refused:
        print('\nREFUSED: the no-history control read outside |%.2f| on a class with signal -- the metric '
              'is not trustworthy for that subject on this bed; do not quote it.' % args.control_limit)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
