#!/usr/bin/env python3
"""
make_v_flamer.py -- builds progs/v_flamer.mdl, the flamethrower's view model
(FLAMETHROWER.md), from nothing but this script: no editor, no borrowed asset.

Seb (2026-10-03): "go with option 3, make an original one". It is ours, so it
ships in the public download -- unlike a mission pack's model. The release
script and qc/build.sh both run this; the .mdl itself is never committed (the
public source snapshot refuses any .mdl, and a generated file needs no copy).

What it reads at build time, and why it copies neither:
  - the Quake PALETTE (gfx/palette.lmp) from the player's own id1 pak -- the
    skin is palette indices, and the palette is id's data, not ours to carry;
  - the 162 vertex NORMALS from this tree's mathlib.c (m_bytenormals), the
    table an MDL's lightnormalindex points into.

The gun, in view-model space (x forward, y LEFT, z up, the eye at the origin;
the thunderbolt sits in x 2..28, y -6..6, z -15..-9 and this sits where it
does): a steel barrel under a perforated heat shroud, a flared nozzle with a
brass pilot-light housing and a small FULLBRIGHT pilot flame beneath it, a red
fuel canister on the right with a hazard band and domed ends, and a ribbed
black hose from the canister into the barrel. Five frames, named as the
thunderbolt's (shot1..shot5): frame 0 at rest, 1-4 the firing loop -- a small
recoil shudder and the pilot flame flaring.

Usage: python3 qc/make_v_flamer.py [out.mdl]   (default m5/progs/v_flamer.mdl)
"""
import math, os, random, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# ---------------------------------------------------------------- inputs ----
def load_palette():
    for name in ('PAK0.PAK', 'pak0.pak', 'Pak0.pak'):
        p = os.path.join(ROOT, 'id1', name)
        if os.path.exists(p):
            d = open(p, 'rb').read()
            _, off, ln = struct.unpack('<4sii', d[:12])
            for i in range(ln // 64):
                n, o, l = struct.unpack('<56sii', d[off + i*64:off + i*64 + 64])
                if n.split(b'\0')[0] == b'gfx/palette.lmp':
                    pal = d[o:o + 768]
                    return [tuple(pal[i*3:i*3 + 3]) for i in range(256)]
    loose = os.path.join(ROOT, 'id1', 'gfx', 'palette.lmp')
    if os.path.exists(loose):
        pal = open(loose, 'rb').read()
        return [tuple(pal[i*3:i*3 + 3]) for i in range(256)]
    sys.exit('make_v_flamer: no id1 pak0 to read the Quake palette from')

def load_normals():
    src = open(os.path.join(ROOT, 'mathlib.c')).read()
    i = src.index('m_bytenormals[NUMVERTEXNORMALS][3]')
    body = src[i:src.index('};', i)]
    nums = [float(x) for x in re.findall(r'-?\d+\.\d+', body)]
    return [tuple(nums[k:k + 3]) for k in range(0, len(nums), 3)]

PAL = load_palette()
NORMS = load_normals()

# ------------------------------------------------------------------ skin ----
SW, SH = 256, 256
rng = random.Random(1996)
skin_rgb = [[(255, 0, 255)] * SW for _ in range(SH)]
fullbright = [[False] * SW for _ in range(SH)]

def clamp(v): return max(0, min(255, int(v)))

def paint(x0, y0, w, h, fn, fb=False):
    for y in range(y0, y0 + h):
        for x in range(x0, x0 + w):
            u = (x - x0 + 0.5) / w
            v = (y - y0 + 0.5) / h
            r, g, b = fn(u, v, x, y)
            skin_rgb[y][x] = (clamp(r), clamp(g), clamp(b))
            fullbright[y][x] = fb

# SMOOTH grime: value noise, a few octaves, in skin pixels. Low-frequency soot
# blotches and streaks survive the palette as shading; per-texel noise turns
# into coloured speckle (measured on the first stills), so that stays small.
_lat = {}
def _vn(ix, iy, oc):
    k = (ix, iy, oc)
    if k not in _lat:
        _lat[k] = random.Random(hash(k) & 0xffffffff).random()
    return _lat[k]
def vnoise(x, y, oc=0):
    ix, iy = math.floor(x), math.floor(y)
    fx, fy = x - ix, y - iy
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy)
    a = _vn(ix, iy, oc); b = _vn(ix + 1, iy, oc)
    c = _vn(ix, iy + 1, oc); d = _vn(ix + 1, iy + 1, oc)
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy
def fbm(x, y):
    return 0.5 * vnoise(x / 24, y / 24, 1) + 0.3 * vnoise(x / 10, y / 10, 2) + 0.2 * vnoise(x / 4, y / 4, 3)
def soot(x, y, amount):
    """multiplier: 1 clean, down to 1 - amount in the dirtiest blotches"""
    g = fbm(x, y)
    return 1.0 - amount * max(0.0, (g - 0.35) / 0.65) ** 1.4
def streak(x, y, amount):
    """oily drips running along the length (v): narrow in u, long in v"""
    g = vnoise(x / 3.0, y / 40.0, 4)
    return 1.0 - amount * max(0.0, (g - 0.55) / 0.45)

def grime(k=10):
    # kept small: palette quantisation turns large noise into coloured speckle
    return rng.uniform(-k, k) * 0.45

def roundlight(u):
    # a cylinder's sheen around its circumference: u runs once round, the
    # light from above-left (u ~ 0.15), a soft highlight and a dark underside
    # u = 0 and 1 are the UNDERSIDE (the wrap seam is put where it cannot be
    # seen), so u = 0.5 is the top; the light comes from above
    a = u * 2 * math.pi
    return 0.55 + 0.45 * math.cos(a - math.pi + 0.5)

# regions: (x0, y0, w, h)
R_BARREL = (0, 0, 128, 32)
R_SHROUD = (0, 32, 128, 64)
R_TANK = (0, 96, 128, 64)
R_NOZZLE = (128, 0, 64, 32)
R_CAP = (128, 32, 64, 48)
R_HOSE = (128, 80, 64, 16)
R_BRASS = (192, 0, 64, 32)
R_FLAME = (192, 32, 64, 32)
R_BORE = (192, 64, 64, 16)
R_SHROUDEND = (128, 96, 64, 48)
R_LIP = (192, 80, 64, 16)

def barrel(u, v, x, y):
    s = roundlight(u)
    base = (14 + 28 * s) * soot(x, y, 0.7) * streak(x, y, 0.5)
    return (base * 1.02 + grime(4), base * 0.97 + grime(4), base * 0.9 + grime(4))

def shroud(u, v, x, y):
    s = roundlight(u)
    base = (16 + 30 * s) * soot(x, y, 0.75) * streak(x, y, 0.45)
    col = [base * 1.03, base * 0.97, base * 0.88]
    # rows of round vent holes with HARD edges: inside is black, and a one-
    # texel bevel round the rim -- lit on its far (lower) side, shadowed on its
    # near (upper) side -- reads as a hole punched through sheet metal
    cu = (u * 10) % 1.0
    cv = (v * 5) % 1.0
    if 0.12 < v < 0.88:
        dx, dy = (cu - 0.5) * 1.4, (cv - 0.5)
        d = math.hypot(dx, dy)
        if d < 0.22:
            col = [5, 4, 4]
        elif d < 0.27:
            col = [c * (1.7 if dy > 0 else 0.45) for c in col]
    # rolled rims at both ends
    if v < 0.08 or v > 0.92:
        col = [c * 1.3 + 12 for c in col]
    # heat stain towards the muzzle end
    heat = max(0.0, v - 0.6) / 0.4
    col[0] += 30 * heat; col[1] += 8 * heat; col[2] -= 10 * heat
    return [c + grime(7) for c in col]

def tank(u, v, x, y):
    s = roundlight(u)
    col = [40 + 32 * s, 9 + 8 * s, 7 + 5 * s]          # oxblood, weathered
    g = soot(x, y, 0.65) * streak(x, y, 0.4)
    col = [c * g for c in col]
    if 0.42 < v < 0.58:                                 # hazard band, yellow/black chevrons
        stripe = ((u * 16 + v * 6) % 1.0) < 0.5
        col = [210 * s + 30, 170 * s + 25, 20] if stripe else [18, 16, 14]
    if v < 0.06 or v > 0.94:                            # seams at the ends
        col = [c * 0.55 for c in col]
    if rng.random() < 0.02:                             # chipped paint
        col = [c * 0.6 + 20 for c in col]
    return [c + grime(8) for c in col]

def cap(u, v, x, y):
    # a domed end: radial, a bolt circle, the red paint going dark at the rim
    du, dv = u - 0.5, v - 0.5
    r = math.hypot(du, dv) * 2
    s = 0.55 + 0.45 * max(0.0, 1 - r)
    col = [c * soot(x, y, 0.6) for c in (36 + 30 * s, 8 + 7 * s, 6 + 5 * s)]
    ang = math.atan2(dv, du)
    if 0.62 < r < 0.78 and (ang * 8 / (2 * math.pi)) % 1.0 < 0.3:
        col = [c * 0.55 for c in col]                   # bolt heads, shadowed
    if r < 0.18:
        col = [60, 58, 56]                              # filler plug
    return [c + grime(6) for c in col]

def nozzle(u, v, x, y):
    s = roundlight(u)
    base = (18 + 32 * s) * soot(x, y, 0.65)
    # gunmetal at the barrel end going to scorched bronze, then soot at the tip
    t = v
    col = [base * (0.92 + 0.35 * t), base * (0.92 - 0.05 * t), base * (1.0 - 0.55 * t)]
    if t > 0.7:
        col = [c * max(0.12, 1.0 - (t - 0.7) * 2.8) + 4 for c in col]
    return [c + grime(5) for c in col]

def hose(u, v, x, y):
    s = roundlight(u)
    rib = 0.75 + 0.25 * math.cos(v * 2 * math.pi * 8)
    b = (14 + 34 * s) * rib
    return (b + grime(3), b + grime(3), b * 1.05 + grime(3))

def brass(u, v, x, y):
    s = roundlight(u)
    return (120 + 100 * s + grime(6), 90 + 70 * s + grime(6), 30 + 25 * s + grime(4))

def flame(u, v, x, y):
    # the pilot flame: white-yellow at the base of the teardrop, orange at the tip
    return (255, 250 - 90 * v, 200 - 180 * v)

def bore(u, v, x, y):
    return (6, 5, 5)

def lip(u, v, x, y):
    # the rolled lips at the shroud's ends: a bright worn edge on a dark band
    s = roundlight(u)
    b = (20 + 42 * s) * (1.5 if 0.35 < v < 0.65 else 0.8) * soot(x, y, 0.55)
    return (b * 0.95 + grime(3), b * 0.93 + grime(3), b * 0.9 + grime(3))

def shroudend(u, v, x, y):
    du, dv = u - 0.5, v - 0.5
    r = math.hypot(du, dv) * 2
    b = (32 + 36 * (1 - r)) * soot(x, y, 0.5)
    return (b * 0.95 + grime(5), b * 0.92 + grime(5), b * 0.88 + grime(5))

paint(*R_BARREL, barrel)
paint(*R_SHROUD, shroud)
paint(*R_TANK, tank)
paint(*R_NOZZLE, nozzle)
paint(*R_CAP, cap)
paint(*R_HOSE, hose)
paint(*R_BRASS, brass)
paint(*R_FLAME, flame, fb=True)
paint(*R_BORE, bore)
paint(*R_SHROUDEND, shroudend)
paint(*R_LIP, lip)

def nearest(rgb, lo, hi):
    best, bi = 1e18, lo
    for i in range(lo, hi):
        p = PAL[i]
        # "redmean" distance: luma-only weights let dark neutral greys land on
        # the palette's BLUE-greys (the shroud read blue on the first stills)
        rm = (p[0] + rgb[0]) / 2
        d = (2 + rm/256) * (p[0]-rgb[0])**2 + 4 * (p[1]-rgb[1])**2 + (2 + (255-rm)/256) * (p[2]-rgb[2])**2
        if d < best:
            best, bi = d, i
    return bi

cache = {}
skin = bytearray(SW * SH)
for y in range(SH):
    for x in range(SW):
        key = (skin_rgb[y][x], fullbright[y][x])
        if key not in cache:
            # 224-255 are the palette's fullbrights: only the pilot flame may use them
            cache[key] = nearest(key[0], 224, 255) if key[1] else nearest(key[0], 0, 224)
        skin[y * SW + x] = cache[key]

# -------------------------------------------------------------- geometry ----
verts = []   # [pos(x,y,z), normal, (s,t)]
tris = []

def add_vert(p, n, st):
    verts.append([list(p), n, st])
    return len(verts) - 1

def norm(v):
    l = math.sqrt(sum(c*c for c in v)) or 1.0
    return [c / l for c in v]

def tube(path, radii, region, sides=8, ucycles=1.0, cap_start=None, cap_end=None, part='static'):
    """Rings along a polyline `path` (list of points), radius per node; the
    skin region is wrapped once round (u) and stretched along (v)."""
    x0, y0, w, h = region
    rings = []
    n = len(path)
    # a frame along the path: tangent, and a stable side vector
    for i, p in enumerate(path):
        a = path[max(0, i-1)]; b = path[min(n-1, i+1)]
        t = norm([b[k]-a[k] for k in range(3)])
        up = [0, 0, 1] if abs(t[2]) < 0.9 else [0, 1, 0]
        side = norm([t[1]*up[2]-t[2]*up[1], t[2]*up[0]-t[0]*up[2], t[0]*up[1]-t[1]*up[0]])
        upv = norm([side[1]*t[2]-side[2]*t[1], side[2]*t[0]-side[0]*t[2], side[0]*t[1]-side[1]*t[0]])
        ring = []
        for k in range(sides + 1):
            # start (and wrap) at the UNDERSIDE, so the seam is out of sight
            a_ = math.pi + 2 * math.pi * k / sides
            dirv = [math.cos(a_)*upv[j] + math.sin(a_)*side[j] for j in range(3)]
            pos = [p[j] + radii[i]*dirv[j] for j in range(3)]
            # inset a texel from every region edge: bilinear filtering would
            # otherwise pull the neighbouring region in along the seam
            s = x0 + 1 + (k / sides) * (w - 3) * ucycles
            tt = y0 + 1 + (i / (n - 1)) * (h - 3)
            ring.append(add_vert(pos, dirv, (s, tt)))
            verts[-1].append(part)
        rings.append(ring)
    for i in range(n - 1):
        for k in range(sides):
            a, b = rings[i][k], rings[i][k+1]
            c, d = rings[i+1][k], rings[i+1][k+1]
            tris.append((a, c, b)); tris.append((b, c, d))
    for cap_spec, idx, sign in ((cap_start, 0, -1), (cap_end, n - 1, 1)):
        if not cap_spec:
            continue
        cregion, bulge = cap_spec
        cx0, cy0, cw, ch = cregion
        p = path[idx]
        a = path[max(0, idx-1)] if sign > 0 else path[0]
        b = path[idx] if sign > 0 else path[min(n-1, 1)]
        t = norm([b[k]-a[k] for k in range(3)])
        tip = [p[j] + sign * t[j] * bulge for j in range(3)]
        centre = add_vert(tip, [sign*t[j] for j in range(3)], (cx0 + cw/2, cy0 + ch/2))
        verts[-1].append(part)
        rim = []
        for k in range(sides + 1):
            a_ = 2 * math.pi * k / sides
            q = verts[rings[idx][k]][0]
            nv = norm([q[j] - p[j] + sign*t[j]*radii[idx]*0.6 for j in range(3)])
            s = cx0 + cw/2 + math.cos(a_) * (cw/2 - 2)
            tt = cy0 + ch/2 + math.sin(a_) * (ch/2 - 2)
            rim.append(add_vert(q, nv, (s, tt)))
            verts[-1].append(part)
        for k in range(sides):
            if sign > 0:
                tris.append((centre, rim[k], rim[k+1]))
            else:
                tris.append((centre, rim[k+1], rim[k]))

ZB = -11.8     # the barrel's centre line, below the eye
# the barrel and nozzle (the muzzle is where m5flame.qc starts the stream)
tube([(7, 0, ZB), (27, 0, ZB)], [1.5, 1.5], R_BARREL, sides=10)
tube([(27, 0, ZB), (29.6, 0, ZB), (30.4, 0, ZB), (31.2, 0, ZB)], [1.25, 1.15, 1.45, 1.75], R_NOZZLE, sides=10,
     cap_end=(R_BORE, -0.4))
# the perforated heat shroud
tube([(10, 0, ZB), (23, 0, ZB)], [2.5, 2.5], R_SHROUD, sides=16,
     cap_start=(R_SHROUDEND, 0.0), cap_end=(R_SHROUDEND, 0.0))
# raised lips at both ends of the shroud: a hard step in the silhouette
for lx in (10.0, 22.4):
    tube([(lx - 0.05, 0, ZB), (lx + 0.65, 0, ZB)], [2.78, 2.78], R_LIP, sides=16,
         cap_start=(R_SHROUDEND, 0.0), cap_end=(R_SHROUDEND, 0.0))
# the fuel canister, on the RIGHT (y is LEFT in model space): it was a pair,
# one either side, until Seb's second look
TZ = -14.4
for TY in (-4.6,):	# the right-hand one only (Seb, 2026-10-03: "lose the left hand canister")
    tube([(6, TY, TZ), (20, TY, TZ)], [2.3, 2.3], R_TANK, sides=12,
         cap_start=(R_CAP, 1.2), cap_end=(R_CAP, 1.5))
    # a hose from each canister's front dome, under and into the barrel
    sg = 1 if TY > 0 else -1
    hp = [(21.6, TY, TZ), (23.4, TY - sg * 0.6, TZ - 0.3), (25.0, TY - sg * 2.6, TZ + 0.8), (26.0, sg * 1.1, ZB - 1.0), (26.2, 0, ZB - 0.9)]
    tube(hp, [0.5] * len(hp), R_HOSE, sides=6)
# the pilot-light housing, under the nozzle
tube([(27.6, 0, ZB - 2.0), (30.2, 0, ZB - 2.0)], [0.55, 0.55], R_BRASS, sides=6,
     cap_end=(R_BRASS, 0.2))
# the pilot flame: a small teardrop at the housing's mouth, FULLBRIGHT, its own
# part so the frames can flare it
fx = 30.6
tube([(fx, 0, ZB - 2.0), (fx + 0.5, 0, ZB - 2.0), (fx + 1.4, 0, ZB - 1.9), (fx + 2.4, 0, ZB - 1.7)],
     [0.05, 0.42, 0.32, 0.02], R_FLAME, sides=6, part='flame')

# ---------------------------------------------------------------- frames ----
def frame_positions(f):
    """f = 0 rest; 1..4 the firing loop: a small recoil shudder and the pilot
    flame flaring (scaled about its root)."""
    out = []
    kick = [0.0, -0.45, -0.75, -0.35, -0.6][f]
    jit = [(0, 0), (0.05, -0.04), (-0.04, 0.05), (0.03, 0.03), (-0.05, -0.03)][f]
    flare = [1.0, 1.7, 2.1, 1.5, 1.9][f]
    root = (fx, 0, ZB - 2.0)
    for v in verts:
        p = list(v[0])
        if v[3] == 'flame':
            p = [root[j] + (p[j] - root[j]) * (flare if j == 0 else 1 + (flare - 1) * 0.5) for j in range(3)]
        p[0] += kick
        p[1] += jit[0]
        p[2] += jit[1]
        out.append(p)
    return out

FRAMES = [frame_positions(f) for f in range(5)]
allp = [p for fr in FRAMES for p in fr]
mins = [min(p[k] for p in allp) for k in range(3)]
maxs = [max(p[k] for p in allp) for k in range(3)]
scale = [(maxs[k] - mins[k]) / 255.0 or 1.0 for k in range(3)]

def nearest_normal(n):
    best, bi = -2, 0
    for i, q in enumerate(NORMS):
        d = n[0]*q[0] + n[1]*q[1] + n[2]*q[2]
        if d > best:
            best, bi = d, i
    return bi

def trivert(p, n):
    return bytes([max(0, min(255, int(round((p[k] - mins[k]) / scale[k])))) for k in range(3)] + [nearest_normal(n)])

# ----------------------------------------------------------------- write ----
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'm5', 'progs', 'v_flamer.mdl')
os.makedirs(os.path.dirname(out), exist_ok=True)
radius = max(math.sqrt(sum(c*c for c in p)) for p in allp)
b = bytearray()
b += struct.pack('<4si', b'IDPO', 6)
b += struct.pack('<3f', *scale) + struct.pack('<3f', *mins)
b += struct.pack('<f', radius) + struct.pack('<3f', 0.0, 0.0, -54.0)
b += struct.pack('<8i', 1, SW, SH, len(verts), len(tris), len(FRAMES), 0, 0)
b += struct.pack('<f', 2.0)
b += struct.pack('<i', 0) + bytes(skin)
for v in verts:
    s, t = v[2]
    b += struct.pack('<3i', 0, int(round(s)), int(round(t)))
for t in tris:
    b += struct.pack('<4i', 1, t[0], t[1], t[2])
for f, fr in enumerate(FRAMES):
    lo = trivert([min(p[k] for p in fr) for k in range(3)], (0, 0, 1))
    hi = trivert([max(p[k] for p in fr) for k in range(3)], (0, 0, 1))
    b += struct.pack('<i', 0) + lo + hi + ('shot%d' % (f + 1)).encode().ljust(16, b'\0')
    for p, v in zip(fr, verts):
        b += trivert(p, v[1])
open(out, 'wb').write(bytes(b))
print('make_v_flamer: %s  %d verts, %d tris, %d frames, skin %dx%d' % (out, len(verts), len(tris), len(FRAMES), SW, SH))
