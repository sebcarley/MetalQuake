#!/usr/bin/env python3
"""test/bluenoise-gen.py -- generate rt_bluenoise.h, the committed 64x64x8
blue-noise jitter table for the RT kernels.

Run BY HAND, once; the committed header is the artefact. Deliberately pure
Python (no numpy -- the tree adds no dependencies) with its own xorshift32
PRNG and fixed literal seeds, so the output is bit-reproducible on any
machine forever: checked-in bytes never drift under generator refactors,
which is what every pixel-proof baseline in this tree wants.

TWO GENERATORS, one shape. Both write the same header, the same symbol
(rt_bluenoise64[8][4096]) and the same slice-major layout the kernels index
(bn[slice*4096 + y*64 + x]), so switching between them is a data-only commit
with zero engine change.

  --stbn (the default since 2026-09-03, BLUENOISE slice 1): ONE 3D
  void-and-cluster over 64x64xT (T = 8 or 16), toroidal in x, y AND t, with
  the SPATIOTEMPORAL energy of Wolfe, Morrical, Gruen and Villemin,
  "Spatiotemporal Blue Noise Masks" (EGSR 2022): two Gaussians that never
  cross --
      same slice   (dt == 0):        exp(-(dx^2+dy^2) / (2 sigma_s^2))
      same texel   (dx == dy == 0):  exp(-dt^2 / (2 sigma_t^2))
      otherwise:                     0
  so every slice is blue in SPACE and every texel's T-frame sequence is blue
  in TIME, and consecutive frames' errors cancel under a temporal filter
  instead of piling up -- the property the old table lacked by construction
  (independent slices are blue in space and WHITE in time, which is exactly
  what an EMA averages worst). NOT an isotropic 3D Gaussian: that is the
  paper's "3D blue noise" straw man, and the first cut here built exactly it
  -- test/bluenoise-check.py read 45% of each slice's power below radius 4
  (the old table: 0%), because a 3D-blue volume's SLICES are not 2D-blue.
  The time axis wraps because the kernels cycle slices with `frame & (T-1)`.
  MEASURED HERE (bluenoise-check.py, EMA sims): a cycle of T slices caps
  what a temporal filter can average -- at history 0.9 the old 8-slice table
  reads WORSE than an i.i.d. stream (0.258 vs 0.164 on the jitter
  integrand), because an EMA over a period-8 sequence cannot see more than
  8 values. --slices 16 halves that ceiling for 64 KB more table.

  --legacy: the 2026-08-13 generator, 8 INDEPENDENT 2D fields (own seed each),
  Ulichney void-and-cluster, sigma 1.9, toroidal -- reproduces the header
  that shipped from 25030d8a to 5978a586 byte for byte. Kept for A/B.

Algorithm (both): Ulichney void-and-cluster. On a torus the zero-field energy
is a constant minus the ones-field energy, so phase 3 (cluster-of-zeros) is
identical to continuing phase 2's largest-void insertion -- one loop fills
ranks initial..N-1. Ranks map to 8-bit values by an exact integer shift, so
every value appears equally often over the whole table (legacy: per slice).

Usage:
  python3 test/bluenoise-gen.py > rt_bluenoise.h                (STBN, ~4 min)
  python3 test/bluenoise-gen.py --legacy > rt_bluenoise.h       (~5 min)
  python3 test/bluenoise-gen.py --slices 16 > rt_bluenoise.h      (~6 min)
  python3 test/bluenoise-gen.py --sigma-t 1.9 --seed 0x5EB1234 > rt_bluenoise.h
Validate BEFORE any boot with test/bluenoise-check.py (spectra, temporal
discrepancy, EMA convergence against the previous table).
"""
import math, sys

N = 64                  # tile edge
CELLS = N * N
SLICES = 8
SIGMA = 1.9             # spatial sigma (both generators)
SIGMA_T = 1.9           # temporal sigma (STBN); the paper's default for both axes
RADIUS = 8              # spatial kernel truncation: exp(-64/7.22) = 1.4e-4, below rank resolution
INITIAL = CELLS // 10   # legacy: starting point count per slice
SEEDS = [0x1D872B41, 0x5CA1AB1E, 0x0BADCAFE, 0x7355608D,
         0x2E8BFA92, 0x66C209C4, 0x4C377A1E, 0x1BEF726D,
         0x3A7F9C21, 0x59D0E4B7, 0x0C1F8E3D, 0x71A2B5C9,   # slices 8-15 (--legacy --slices 16, 2026-09-03)
         0x2F6D1A84, 0x6E0B7C53, 0x4B9E2F17, 0x1CD3A6F2]
STBN_SEED = 0x57B10903  # 2026-09-03

def xorshift32(state):
    state ^= (state << 13) & 0xFFFFFFFF
    state ^= state >> 17
    state ^= (state << 5) & 0xFFFFFFFF
    return state

# ---------------------------------------------------------------- legacy 2D
kern = [0.0] * CELLS
for dy in range(N):
    wy = min(dy, N - dy)
    for dx in range(N):
        wx = min(dx, N - dx)
        kern[dy * N + dx] = math.exp(-(wx * wx + wy * wy) / (2.0 * SIGMA * SIGMA))

def splat(E, cx, cy, sign):
    # E += sign * kernel centred at (cx, cy), toroidal
    for y in range(N):
        row = ((y - cy) % N) * N
        erow = y * N
        for x in range(N):
            E[erow + x] += sign * kern[row + ((x - cx) % N)]

def argbest(E, mask, want, best_of):
    # index of min (want=0) or max (want=1) energy among cells where
    # mask[i] == best_of; ties broken by lowest index (deterministic)
    best_i = -1
    best_e = None
    for i in range(CELLS):
        if mask[i] != best_of:
            continue
        e = E[i]
        if best_i < 0 or (e > best_e if want else e < best_e):
            best_i, best_e = i, e
    return best_i

def make_slice(seed):
    rng = seed
    ones = [0] * CELLS
    E = [0.0] * CELLS
    placed = 0
    while placed < INITIAL:                       # random initial pattern
        rng = xorshift32(rng)
        i = rng % CELLS
        if not ones[i]:
            ones[i] = 1
            splat(E, i % N, i // N, +1.0)
            placed += 1
    while True:                                    # prototype: swap until stable
        c = argbest(E, ones, 1, 1)                 # tightest cluster
        ones[c] = 0; splat(E, c % N, c // N, -1.0)
        v = argbest(E, ones, 0, 0)                 # largest void
        ones[v] = 1; splat(E, v % N, v // N, +1.0)
        if v == c:
            break
    rank = [0] * CELLS
    work = ones[:]                                 # phase 1: rank the prototype downward
    Ew = E[:]
    count = INITIAL
    while count > 0:
        c = argbest(Ew, work, 1, 1)
        work[c] = 0; splat(Ew, c % N, c // N, -1.0)
        count -= 1
        rank[c] = count
    work = ones[:]                                 # phases 2+3: fill upward (torus: one loop)
    Ew = E[:]
    count = INITIAL
    while count < CELLS:
        v = argbest(Ew, work, 0, 0)
        work[v] = 1; splat(Ew, v % N, v // N, +1.0)
        rank[v] = count
        count += 1
    return [r >> 4 for r in rank]                  # 4096 ranks -> 0..255, exactly 16 each

# ------------------------------------------------------------------ STBN 3D
# The volume is flat, index = t*CELLS + y*N + x (slice-major: the kernels'
# own layout, so the output needs no re-ordering). Two energy views ride
# beside the true energy E: Ez holds E on ZERO cells and +inf on ones, Eo
# holds E on ONE cells and -inf on zeros -- so "largest void" is min(Ez) and
# "tightest cluster" is max(Eo), both C-speed built-ins over 32768 floats
# instead of a Python loop with a mask test (the 2D generator's argbest),
# which is what makes a 32768-cell volume tractable without numpy.
INF = float('inf')

def stbn_kernel(sigma_s, sigma_t, slices):
    # (dt, dy, dx, weight): the STBN energy is TWO Gaussians that never
    # cross -- the 2D spatial one within a slice (dt = 0, truncated at
    # RADIUS) and the 1D temporal one down a texel's own time column
    # (dx = dy = 0, every temporal offset kept, toroidal in t). A point in a
    # different slice AND a different texel contributes nothing.
    ks = []
    for dy in range(-RADIUS, RADIUS + 1):
        for dx in range(-RADIUS, RADIUS + 1):
            ks.append((0, dy, dx, math.exp(-(dx * dx + dy * dy) / (2.0 * sigma_s * sigma_s))))
    for t in range(1, slices):
        wt = min(t, slices - t)
        ks.append((t, 0, 0, math.exp(-(wt * wt) / (2.0 * sigma_t * sigma_t))))
    return ks

def stbn_splat(E, Eview, view_mask, mask, ci, sign, ks, slices):
    # E += sign*kernel about cell ci; Eview mirrors E on cells whose mask
    # equals view_mask (the others keep their sentinel)
    ct, rem = divmod(ci, CELLS)
    cy, cx = divmod(rem, N)
    for (t, dy, dx, w) in ks:
        tt = (ct + t) % slices
        yy = (cy + dy) % N
        xx = (cx + dx) % N
        i = tt * CELLS + yy * N + xx
        e = E[i] + sign * w
        E[i] = e
        if mask[i] == view_mask:
            Eview[i] = e

def stbn_splat2(E, Ez, Eo, mask, ci, sign, ks, slices):
    # phase 0 keeps BOTH views current
    ct, rem = divmod(ci, CELLS)
    cy, cx = divmod(rem, N)
    for (t, dy, dx, w) in ks:
        tt = (ct + t) % slices
        yy = (cy + dy) % N
        xx = (cx + dx) % N
        i = tt * CELLS + yy * N + xx
        e = E[i] + sign * w
        E[i] = e
        if mask[i]:
            Eo[i] = e
        else:
            Ez[i] = e

def make_stbn(seed, sigma_s, sigma_t, slices, log=sys.stderr):
    VOL = CELLS * slices
    ks = stbn_kernel(sigma_s, sigma_t, slices)
    rng = seed
    mask = [0] * VOL
    E = [0.0] * VOL
    Ez = [0.0] * VOL           # zero-cell view (+inf on ones)
    Eo = [-INF] * VOL          # one-cell view (-inf on zeros)
    initial = VOL // 10
    placed = 0
    while placed < initial:                        # random initial pattern
        rng = xorshift32(rng)
        i = rng % VOL
        if not mask[i]:
            mask[i] = 1
            Ez[i] = INF; Eo[i] = E[i]
            stbn_splat2(E, Ez, Eo, mask, i, +1.0, ks, slices)
            placed += 1
    print("// stbn: initial pattern placed", file=log)
    swaps = 0
    while True:                                    # prototype: swap until stable
        c = Eo.index(max(Eo))                      # tightest cluster among ones
        mask[c] = 0; Eo[c] = -INF; Ez[c] = E[c]
        stbn_splat2(E, Ez, Eo, mask, c, -1.0, ks, slices)
        v = Ez.index(min(Ez))                      # largest void among zeros
        mask[v] = 1; Ez[v] = INF; Eo[v] = E[v]
        stbn_splat2(E, Ez, Eo, mask, v, +1.0, ks, slices)
        swaps += 1
        if v == c:
            break
    print("// stbn: prototype stable after %d swaps" % swaps, file=log)
    rank = [0] * VOL
    # phase 1: rank the prototype downward (remove the tightest cluster)
    work = mask[:]
    Ew = E[:]
    Ev = [Ew[i] if work[i] else -INF for i in range(VOL)]
    count = initial
    while count > 0:
        c = Ev.index(max(Ev))
        work[c] = 0; Ev[c] = -INF
        stbn_splat(Ew, Ev, 1, work, c, -1.0, ks, slices)
        count -= 1
        rank[c] = count
    print("// stbn: phase 1 done", file=log)
    # phases 2+3: fill upward (largest void), torus -> one loop
    work = mask[:]
    Ew = E[:]
    Ev = [INF if work[i] else Ew[i] for i in range(VOL)]
    count = initial
    while count < VOL:
        v = Ev.index(min(Ev))
        work[v] = 1; Ev[v] = INF
        stbn_splat(Ew, Ev, 0, work, v, +1.0, ks, slices)
        rank[v] = count
        count += 1
        if count % 4096 == 0:
            print("// stbn: filled %d / %d" % (count, VOL), file=log)
    shift = int(round(math.log2(VOL // 256)))     # VOL ranks -> 0..255, exactly VOL/256 each
    vals = [r >> shift for r in rank]
    return [vals[t * CELLS:(t + 1) * CELLS] for t in range(slices)]

# ------------------------------------------------------------------ output
def write_header(out, slices, note):
    SLICES = len(slices)
    out.write("// rt_bluenoise.h -- 64x64x%d blue-noise jitter table for the RT kernels.\n" % SLICES)
    out.write("// GENERATED by test/bluenoise-gen.py %s.\n" % note)
    out.write("// Regenerate ONLY by rerunning that script; hand edits invalidate every\n")
    out.write("// pixel baseline downstream, and so does regenerating it: the frozen\n")
    out.write("// beds render slice 0 (test/parity-4a.sh pins rt_metal_bluenoise 0 for\n")
    out.write("// exactly that reason -- coverage of the blue arm is PARITY_EXTRA).\n")
    for s in range(SLICES):
        vals = slices[s]
        mean = sum(vals) / CELLS
        sd = math.sqrt(sum((v - mean) ** 2 for v in vals) / CELLS)
        out.write("// slice %d: mean %.3f sd %.3f\n" % (s, mean, sd))
    out.write("\nstatic const unsigned char rt_bluenoise64[%d][%d] = {\n" % (SLICES, CELLS))
    for s in range(SLICES):
        out.write("{\n")
        vals = slices[s]
        for row in range(0, CELLS, 16):
            out.write("".join("%3d," % v for v in vals[row:row + 16]) + "\n")
        out.write("},\n")
    out.write("};\n")

def main(argv):
    legacy = "--legacy" in argv
    sigma_t = SIGMA_T
    seed = STBN_SEED
    if "--sigma-t" in argv: sigma_t = float(argv[argv.index("--sigma-t") + 1])
    if "--seed" in argv: seed = int(argv[argv.index("--seed") + 1], 0)
    nslices = int(argv[argv.index("--slices") + 1]) if "--slices" in argv else SLICES
    assert nslices in (8, 16), "the kernels index slices with frame & (T-1): T must be a power of two the engine knows"
    if legacy:
        slices = []
        for s in range(nslices):
            slices.append(make_slice(SEEDS[s]))
            print("// slice %d done" % s, file=sys.stderr)
        note = ("(legacy: %d independent 2D void-and-cluster fields, sigma %.1f,\n"
                "// toroidal, xorshift32 seeds %s)" % (nslices, SIGMA, ", ".join("0x%08X" % s for s in SEEDS[:nslices])))
    else:
        slices = make_stbn(seed, SIGMA, sigma_t, nslices)
        note = ("(SPATIOTEMPORAL blue noise, Wolfe et al. EGSR 2022: one 3D\n"
                "// void-and-cluster over 64x64x%d, toroidal in x, y and t, spatial sigma %.1f\n"
                "// within a slice + temporal sigma %.1f down each texel's column, no cross\n"
                "// terms, xorshift32 seed 0x%08X)" % (nslices, SIGMA, sigma_t, seed))
    write_header(sys.stdout, slices, note)

if __name__ == "__main__":
    main(sys.argv[1:])
