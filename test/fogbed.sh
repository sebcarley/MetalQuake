#!/bin/sh
# test/fogbed.sh -- the demo fog-speckle bed (BLUENOISE slice 0, 2026-09-03).
#
# ONE arm, ONE playback: Seb's config and demo copied into a sandbox userdir,
# the bench-rt.sh pins in autoexec.cfg (which quake.rc execs AFTER config.cfg,
# so the pins and the arm's own lines beat anything the config archived), the
# frame dumps AND the fog-buffer dumps (RT_METAL_FOGDUMP=1) at the requested
# playback frames, then test/flicker.py on both. The fog dump is the kernel's
# own surface, before the magnification, the composite and MetalFX; the frame
# is what the eye sees. Read both: the scaler can hide or sharpen what the
# kernel does, and only the pair says which.
#
# The arm's lines are cvars in the copied config's own units; the demo and the
# frames decide which bed:
#   demo17 f2600-2609  e1m3, PARKED in front of the ogre room's columns (the
#                      2026-08-29 speckle bed; the stillness floor engages)
#   demo22 f3300-6400  e1m2, WALKING forward through the lit hall (Seb,
#                      2026-09-03: "especially bad walking forward in lit
#                      foggy areas") -- take bursts inside it
# A moving bed's consecutive-frame delta carries the SCENE's motion too, so
# read a moving arm against its own frozen control (RT_METAL_FRAMEPIN=1 plus
# rt_metal_lightsample 0 -- every stochastic source pinned, the residue is
# the motion) and quote the excess, never the raw number (roll.py's rule).
#
# Usage: sh test/fogbed.sh <outdir> <tag> <demo> <frames-csv> [cfg-lines] [env]
#   sh test/fogbed.sh /tmp/fb asis "$HOME/Library/Application Support/darkplaces/m5/demo17.dem" \
#      2600,2601,2602,2603,2604,2605,2606,2607,2608,2609
#   sh test/fogbed.sh /tmp/fb pin ... 2600,... "" "RT_METAL_FRAMEPIN=1"
#   sh test/fogbed.sh /tmp/fb ls0 ... 2600,... "rt_metal_lightsample 0"
# FOGBED_CONFIG overrides the config (default Seb's live m5/config.cfg; `none`
# = engine defaults). SNAPSHOT HIS CONFIG FIRST when a session's arms are
# judged against each other: the live file is rewritten every time he quits,
# and on 2026-09-03 an afternoon's 'as-is' arms silently inherited the clamp
# and history he had archived from the morning's recipes -- two arms came out
# byte-identical to the baseline because the baseline already carried them.
# State every cvar an arm depends on IN the arm, never by omission; FOGBED_FULLSCREEN=1 takes the display at 1920x1080;
# FOGBED_CAMTRACK=1 adds one camera line per playback frame to the log;
# FOGBED_BIN=<path> runs another binary (a control), still from the repo root.
set -eu
cd "$(dirname "$0")/.."
OUT="$1"; TAG="$2"; DEMO="$3"; FRAMES="$4"; EXTRA="${5:-}"; EXTRAENV="${6:-}"
USERDIR_REAL="$HOME/Library/Application Support/darkplaces"
CONFIG="${FOGBED_CONFIG:-$USERDIR_REAL/m5/config.cfg}"
BIN="${FOGBED_BIN:-./darkplaces-sdl}"   # a control binary must still be run FROM the repo root (the content is not in git)
[ -x "$BIN" ] || { echo "fogbed: $BIN missing -- run from the repo root after make sdl-release" >&2; exit 1; }
[ -f "$DEMO" ] || { echo "fogbed: demo not found: $DEMO" >&2; exit 1; }
mkdir -p "$OUT"
DEMOBASE="$(basename "$DEMO")"
SB=$(mktemp -d)
mkdir -p "$SB/m5"
cp "$DEMO" "$SB/m5/$DEMOBASE"
if [ "$CONFIG" != "none" ] && [ -f "$CONFIG" ]; then cp "$CONFIG" "$SB/m5/config.cfg"; fi
{
	echo 'vid_borderless 1'
	echo 'cl_nettimesyncfactor 1'
	echo 'cl_nettimesyncboundmode 1'
	echo 'vid_vsync 0'
	echo 'cl_maxfps 0'
	echo 'cl_maxidlefps 0'
	if [ "${FOGBED_FULLSCREEN:-0}" = "1" ]; then
		echo 'vid_width 1920'; echo 'vid_height 1080'; echo 'vid_fullscreen 1'; echo 'vid_desktopfullscreen 1'
	else
		echo 'vid_fullscreen 0'
	fi
	if [ -n "$EXTRA" ]; then printf '%s\n' "$EXTRA"; fi
} > "$SB/m5/autoexec.cfg"
CT=""
[ "${FOGBED_CAMTRACK:-0}" = "1" ] && CT="RT_METAL_CAMTRACK=1"
# WATCHDOG (2026-09-03): an engine that crashes under the dump path prints its
# crash banner and then sits at 0% CPU for ever (the crash handler waits), and
# a chain of arms behind it never runs. Cap a playback at FOGBED_TIMEOUT
# seconds (default 400 -- the longest bed here is ~90 s) and kill it hard.
env RT_METAL_PROFILE=1 RT_METAL_DUMP="$OUT/$TAG" RT_METAL_DUMPFRAMES="$FRAMES" RT_METAL_FOGDUMP=1 $CT $EXTRAENV \
	"$BIN" -userdir "$SB" -nosound -benchmark "$DEMOBASE" +vid_renderer metal > "$OUT/$TAG.log" 2>&1 &
EPID=$!
T=0; LIM="${FOGBED_TIMEOUT:-400}"
while kill -0 "$EPID" 2>/dev/null; do
	sleep 2; T=$((T+2))
	if [ "$T" -ge "$LIM" ]; then echo "fogbed: $TAG: TIMED OUT after ${LIM}s -- killed (see $OUT/$TAG.log)" >&2; kill -9 "$EPID" 2>/dev/null; break; fi
done
wait "$EPID" 2>/dev/null || true
grep -q "Engine Crash" "$OUT/$TAG.log" && echo "fogbed: $TAG: ENGINE CRASHED -- see $OUT/$TAG.log" >&2
LINE=$(tail -1 "$SB/m5/benchmark.log" 2>/dev/null || true)
[ -n "$LINE" ] || { echo "fogbed: $TAG: NO BENCHMARK LINE -- see $OUT/$TAG.log" >&2; head -5 "$OUT/$TAG.log" >&2 || true; rm -rf "$SB"; exit 1; }
printf '%s %s\n' "$TAG" "$LINE" >> "$OUT/results.log"
grep -q "using gamedirs.*m5" "$OUT/$TAG.log" || echo "fogbed: WARN no 'using gamedirs' line" >&2
grep -q "execing config.cfg" "$OUT/$TAG.log" || echo "fogbed: WARN config.cfg not exec'd" >&2
rm -rf "$SB"
FR=$(printf '%s' "$FRAMES" | tr ',' ' ')
echo "== $TAG  ($(printf '%s' "$LINE" | sed 's/.*seconds \([0-9.]*\) fps.*/\1 fps/'); $(grep -c 'dumped .* fog' "$OUT/$TAG.log") fog dumps)"
python3 test/flicker.py seq "$OUT/$TAG" $FR
python3 test/flicker.py fogseq "$OUT/$TAG" $FR
