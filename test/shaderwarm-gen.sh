#!/bin/sh
# test/shaderwarm-gen.sh -- regenerate m5/shaderwarm.txt, the SHIPPED shader warm list
# (r_shaderwarm): the permutations real play compiles, so a fresh install compiles them at
# its first 3D frame instead of one at a time mid-play. Runs the demos below as timedemos --
# muted, windowed, background app, one engine at a time -- at three configurations: Seb's
# own config (the look as played), the public download's config (the Best row and the look
# list, from release/make-public-config.py) and the Stock overlay (m5_cheap 1), each boot
# with M5_SHADERWARM_OUT set, and unions what they compiled. Re-run when a shader mode is
# added or renumbered, or when a default changes which permutations play reaches.
#
#   sh test/shaderwarm-gen.sh            # -> m5/shaderwarm.txt (git add -f it: /m5/* is ignored)
#   WARM_DEMOS="demo1 demo22" sh ...     # a subset
set -eu
cd "$(dirname "$0")/.."
DEMOS="${WARM_DEMOS:-demo1 demo7 demo22 demo26 demo34 demo49}"
USERDIR_REAL="$HOME/Library/Application Support/darkplaces"
OUT=$(mktemp -d /tmp/shaderwarm.XXXXXX)
python3 release/make-public-config.py "$USERDIR_REAL/m5/config.cfg" "$OUT/public.cfg" > /dev/null
n=0
for arm in seb public stock; do
	for demo in $DEMOS; do
		SB=$(mktemp -d /tmp/shaderwarm-boot.XXXXXX); mkdir -p "$SB/m5"
		if [ "$demo" = demo1 ]; then cp id1/demo1.dem "$SB/m5/" 2>/dev/null || true; else cp "$USERDIR_REAL/m5/$demo.dem" "$SB/m5/"; fi
		case $arm in
			seb)    cp "$USERDIR_REAL/m5/config.cfg" "$SB/m5/config.cfg"; EXTRA="";;
			public) cp "$OUT/public.cfg" "$SB/m5/config.cfg"; EXTRA="";;
			stock)  cp "$OUT/public.cfg" "$SB/m5/config.cfg"; EXTRA="+m5_cheap 1";;
		esac
		printf '%s\n' 'vid_fullscreen 0' 'vid_vsync 0' 'cl_maxfps 0' 'cl_maxidlefps 0' 'r_shaderwarm 0' > "$SB/m5/autoexec.cfg"
		n=$((n + 1))
		env SDL_MAC_BACKGROUND_APP=1 M5_SHADERWARM_OUT="$OUT/$arm-$demo.txt" ./darkplaces-sdl -userdir "$SB" -nosound -window \
			+vid_renderer metal +vid_width 1280 +vid_height 720 $EXTRA -benchmark "$demo.dem" > "$OUT/$arm-$demo.log" 2>&1 < /dev/null || true
		printf '  %-6s %-7s %s\n' "$arm" "$demo" "$(wc -l < "$OUT/$arm-$demo.txt" 2>/dev/null || echo 0) lines"
		rm -rf "$SB"
	done
done
python3 - "$OUT" <<'PY'
import glob, os, re, sys
seen = {}
for f in sorted(glob.glob(os.path.join(sys.argv[1], '*.txt'))):
    if f.endswith('public.cfg'): continue
    for line in open(f):
        m = re.match(r'\s*(\d+)\s+([0-9a-fA-F]+)\s*(//\s*(.*))?', line)
        if m and (int(m.group(1)), int(m.group(2), 16)) not in seen:
            seen[(int(m.group(1)), int(m.group(2), 16))] = (m.group(4) or '').strip()
with open('m5/shaderwarm.txt', 'w') as o:
    o.write('// m5/shaderwarm.txt -- the shipped shader warm list (r_shaderwarm): every permutation the\n')
    o.write('// demos in test/shaderwarm-gen.sh compiled at the played, the public and the Stock configurations.\n')
    o.write('// One per line: <mode> <permutation hex>; the rest of a line is a comment. Regenerate with that script.\n')
    for (mode, perm) in sorted(seen):
        o.write('%u %x  // %s\n' % (mode, perm, seen[(mode, perm)]))
print('m5/shaderwarm.txt: %d permutations from %d boots' % (len(seen), len(glob.glob(os.path.join(sys.argv[1], '*-*.txt')))))
PY
rm -rf "$OUT"
