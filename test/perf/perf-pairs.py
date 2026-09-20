#!/usr/bin/env python3
"""perf-pairs.py -- read a PERF_ROTATE=2 (PAIRED) block as ratios.

    python3 test/perf/perf-pairs.py test/perf/out-good11 [more dirs...]

In the paired form every candidate run is immediately preceded by the block's
reference arm on the SAME bed, and both carry the same round tag (p0, p1, ...).
perf-report.py knows nothing about that pairing; it medians arms across rounds,
which is the wrong reading here -- a pair is the instrument. This prints, per
candidate row, the candidate against its own paired reference (avg fps and the
1-second minimum), then the median ratio per candidate across every bed it ran
on, and the reference's own spread per bed (the drift witness: the reference
ran once per candidate on each bed, so its min..max on a bed is how much the
machine moved while that bed's pairs were taken). Rows flagged '!' (wrong
geometry) are dropped and counted, as perf-report.py does.
"""
import sys, os, csv, statistics as st

def load(d):
    rows = []
    with open(os.path.join(d, 'results.tsv')) as f:
        for r in csv.DictReader(f, delimiter='\t'):
            rows.append(r)
    return rows

def main(dirs):
    rows = [r for d in dirs for r in load(d)]
    flagged = [r for r in rows if r['arm'].startswith('!')]
    rows = [r for r in rows if not r['arm'].startswith('!') and r['round'].startswith('p')]
    if flagged: print('dropped %d flagged (wrong geometry) rows' % len(flagged))
    # pair = (bed, round tag); the reference is whichever arm appears FIRST in that pair
    pairs = {}
    for r in rows:
        pairs.setdefault((r['bed'], r['round']), []).append(r)
    ratios, refs = {}, {}
    print('%-24s %-7s %5s  %7s %7s  %6s   %5s %5s' % ('candidate', 'bed', 'pair', 'ref', 'cand', 'ratio', 'refmin', 'min'))
    for (bed, tag), rs in sorted(pairs.items(), key=lambda kv: (kv[0][0], int(kv[0][1][1:]))):
        if len(rs) != 2:
            print('  %s %s: %d rows, not a pair -- skipped' % (bed, tag, len(rs))); continue
        ref, cand = rs
        rf, cf = float(ref['fps']), float(cand['fps'])
        ratios.setdefault(cand['arm'], {}).setdefault(bed, []).append(cf / rf)
        refs.setdefault(bed, []).append(rf)
        print('%-24s %-7s %5s  %7.1f %7.1f  x%.3f   %5s %5s' % (cand['arm'], bed, tag, rf, cf, cf / rf, ref['min1s'], cand['min1s']))
    print('\nper candidate, median ratio over beds (n beds):')
    for arm, byb in sorted(ratios.items()):
        meds = [st.median(v) for v in byb.values()]
        print('  %-24s x%.3f  range %.3f-%.3f  (%d)' % (arm, st.median(meds), min(meds), max(meds), len(meds)))
    print('\nreference spread per bed (the drift witness):')
    for bed, v in sorted(refs.items()):
        print('  %-7s %6.1f .. %6.1f  (%.1f%%, n=%d)' % (bed, min(v), max(v), 100 * (max(v) / min(v) - 1), len(v)))

if __name__ == '__main__':
    main(sys.argv[1:] or ['test/perf/out'])
