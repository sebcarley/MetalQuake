#!/usr/bin/env python3
"""perf-pairs-clean.py -- perf-pairs.py plus the CONTAMINATION FILTER.

    python3 test/perf/perf-pairs-clean.py <outdir> [--bar 5]

BEAUTYBENCH (2026-09-17) recorded that under PERF_ROTATE=2 a pair whose
reference deviates badly from the block's median reference is poisoned -- there
it was a 2-5 minute idle gap worth +22%, and an arm that only REMOVES work read
11% SLOWER. perf-pairs.py has no such filter and would have published that
0.887. This drops any pair whose reference sits more than --bar percent from
that BED's median reference, and says which it dropped and why, so a
contaminated block degrades into fewer honest pairs rather than confident
nonsense. Rows flagged '!' (wrong geometry) are dropped first, as ever.
"""
import sys, os, csv, statistics as st

def bed_ref_medians(d):
    rows=list(csv.DictReader(open(os.path.join(d,'results.tsv')),delimiter='\t'))
    by={}
    for r in rows:
        if r['arm'].startswith('!') or not r['round'].startswith('p'): continue
        by.setdefault((r['bed'],r['round']),[]).append(r)
    out={}
    for (bed,tag),rs in by.items():
        if len(rs)==2: out.setdefault(bed,[]).append(float(rs[0]['fps']))
    return {b:st.median(v) for b,v in out.items()}

def main(d, bar=5.0, baseline=None):
    rows = list(csv.DictReader(open(os.path.join(d,'results.tsv')), delimiter='\t'))
    flagged = [r for r in rows if r['arm'].startswith('!')]
    rows = [r for r in rows if not r['arm'].startswith('!') and r['round'].startswith('p')]
    if flagged: print('dropped %d flagged (wrong geometry) rows' % len(flagged))
    pairs = {}
    for r in rows: pairs.setdefault((r['bed'], r['round']), []).append(r)
    pairs = {k:v for k,v in pairs.items() if len(v)==2}
    # the per-bed median reference is the yardstick
    refs_by_bed = {}
    for (bed,tag), rs in pairs.items(): refs_by_bed.setdefault(bed,[]).append(float(rs[0]['fps']))
    med = {b: st.median(v) for b,v in refs_by_bed.items()}
    if baseline:
        bmed = bed_ref_medians(baseline)
        for b in med:
            if b in bmed: med[b] = bmed[b]
        print('yardstick: per-bed reference medians from %s (a clean block)' % baseline)
    keep, drop = {}, []
    for (bed,tag), rs in sorted(pairs.items()):
        ref, cand = rs
        rf, cf = float(ref['fps']), float(cand['fps'])
        dev = 100.0*(rf - med[bed])/med[bed]
        if abs(dev) > bar:
            drop.append((cand['arm'], bed, tag, rf, med[bed], dev)); continue
        keep.setdefault(cand['arm'], []).append((bed, cf/rf, cf, int(cand['min1s'])))
    if drop:
        print('\nDROPPED %d contaminated pair(s) -- reference off its bed median by >%.0f%%:' % (len(drop), bar))
        for a,b,t,rf,m,dev in drop: print('  %-20s %-8s %s  ref %.1f vs bed median %.1f  (%+.1f%%)' % (a,b,t,rf,m,dev))
    print('\n%-20s %8s  %-28s %s' % ('candidate','median','per-bed ratios','fps (1s-min)'))
    for a in sorted(keep):
        v = keep[a]
        rs = sorted(x[1] for x in v)
        print('%-20s  x%.3f  %-28s %s' % (a, st.median(rs),
            ' '.join('%s:x%.2f'%(b,r) for b,r,_,_ in v),
            ' '.join('%.0f(%d)'%(f,m) for _,_,f,m in v)))
    print('\nper-bed median reference: ' + '  '.join('%s %.1f'%(b,m) for b,m in sorted(med.items())))

if __name__ == '__main__':
    bar = 5.0
    if '--bar' in sys.argv: bar = float(sys.argv[sys.argv.index('--bar')+1])
    base = sys.argv[sys.argv.index('--baseline')+1] if '--baseline' in sys.argv else None
    main(sys.argv[1], bar, base)
