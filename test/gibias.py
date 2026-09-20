#!/usr/bin/env python3
"""gibias.py -- read the RT_METAL_GIBIAS=1 classification out of a term dump.

The debug arm paints its classification into the RT term buffer, which
RT_METAL_TERMDUMP=1 writes beside every RT_METAL_DUMP frame:
    [i32 w][i32 h][RGBA16F, kernel row order]

Mode 1 (the tile-list bias, GIARC G4 item 4):
    R = 1 on a VALID bounce sample (the denominator)
    G = 1 when the brightest REACHING light was OUTSIDE the list the pick used
    B = the relative shortfall (allbr - tilebr) / allbr -- what turns a count
        into a magnitude: a bias that only misses lights no brighter than the
        ones it did see does not matter.

Prints the bias fraction and the shortfall distribution over valid samples.
Pure python (no numpy on this machine), 'e' is the half-float struct code.
"""
import struct, sys

def read_term(path):
    d = open(path, 'rb').read()
    w, h = struct.unpack_from('<ii', d, 0)
    n = w * h
    px = struct.unpack_from('<%de' % (n * 4), d, 8)
    return w, h, px

def main(paths):
    print('%-34s %8s %8s %7s   %s' % ('dump', 'valid', 'outside', 'bias%', 'shortfall p50/p90/mean'))
    for p in paths:
        try:
            w, h, px = read_term(p)
        except Exception as e:
            print('%-34s  READ FAILED: %s' % (p.split('/')[-1], e)); continue
        valid = 0; outside = 0; sf = []
        for i in range(w * h):
            r = px[i*4]; g = px[i*4+1]; b = px[i*4+2]
            if r <= 0.5: continue
            valid += 1
            if g > 0.5:
                outside += 1
            sf.append(b)
        if not valid:
            print('%-34s %8d  (no valid bounce samples -- wrong bed or GI off)' % (p.split('/')[-1], 0)); continue
        sf.sort()
        p50 = sf[len(sf)//2]; p90 = sf[int(len(sf)*0.9)]; mean = sum(sf)/len(sf)
        print('%-34s %8d %8d %6.1f%%   %.3f / %.3f / %.3f'
              % (p.split('/')[-1], valid, outside, 100.0*outside/valid, p50, p90, mean))

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(2)
    main(sys.argv[1:])
