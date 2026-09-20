#!/bin/sh
# ---------------------------------------------------------------------------
# QuakeM5 ghost bed driver -- dumps the frames test/ghost.py reads.
#
#   test/ghostbed.sh <outdir> <arm> "<cfg lines>" [frames]
#
#     <outdir>    where the dumps go: <outdir>/<arm>.f<N>, <arm>.log, <arm>.manifest
#     <arm>       a short name (bil, spa, nopart, m3p3 ...)
#     <cfg lines> newline-separated console lines for this arm, e.g.
#                 "r_metalfx 2
#                  r_metalfx_reactive 3"
#     [frames]    RT_METAL_DUMPFRAMES list, default 417,418,419,420,421
#
#   env  GHOST_DEMO=ghostbed.dem   the demo in Seb's m5/ userdir (static camera,
#                                  rocket fired twice on e1m3; test/README.md)
#        GHOST_VIEWSCALE=0.667     the pinned render scale (what he plays)
#        GHOST_BIN=<path>          run THIS binary instead of ./darkplaces-sdl --
#                                  a control build. It is copied into the repo
#                                  root as ./darkplaces-sdl.head and run from
#                                  there with -quake: the engine finds game
#                                  CONTENT relative to its cwd (m5/ is not in
#                                  git, so a worktree has no progs.dat, no
#                                  QRP, no AMI), and it picks its GAMEMODE from
#                                  the executable's NAME by strstr ("dp-head"
#                                  boots Arcane Dimensions -- "head" contains
#                                  "ad"). FS_StripExtension turns
#                                  darkplaces-sdl.head into darkplaces-sdl and
#                                  -quake forces GAME_NORMAL regardless.
#        GHOST_ENV="A=1 B=2"       extra environment for the run (METAL_FRAMEMS=3 ...)
#        GHOST_CONFIG=<cfg>        the config copied into the sandbox
#                                  (default: Seb's live m5/config.cfg)
#
# THE PREAMBLE IS A PIN, NOT A SUGGESTION. Every arm gets the same window
# geometry (borderless 1920x1080 -- a titled window is clamped under the menu
# bar and dumps 1920x959 that match nothing), vsync off, no fps cap, the
# net-time sync pinned (cl.time drift wobbles every lerp consumer), and
# r_viewscale at Seb's value. The arm's own lines come AFTER, so an arm can
# override a pin deliberately and the log shows it did.
#
# THE MANIFEST is the proof the bed ran what it claims: the gamedirs line
# (m5 must be mounted -- a control binary run from the wrong cwd boots a
# different-looking world and renders happily), the autoexec exec line, the
# demo line, the Video Mode line (the 1920x1017 class announces itself
# nowhere else), and the binary's version string. Read it before the dumps.
#
# Same-binary floor first: run the SAME arm twice (two names) and compare with
#   python3 test/ghost.py --floor <outdir>/m3p3 <outdir>/m3p3b
# before quoting any number from the bed -- demo dumps on the Metal path are a
# METRIC bed (a few tenths of a percent, localised), and an odd boot is a
# >50% global wash that is rerun once (the demo11 rule).
# ---------------------------------------------------------------------------
set -eu
cd "$(dirname "$0")/.." || exit 1
ROOT="$(pwd)"
OUT="${1:-}"; ARM="${2:-}"; LINES="${3:-}"; FRAMES="${4:-417,418,419,420,421}"
if [ -z "$OUT" ] || [ -z "$ARM" ]; then
	echo "usage: test/ghostbed.sh <outdir> <arm> \"<cfg lines>\" [frames]" >&2
	exit 2
fi
DEMO="${GHOST_DEMO:-ghostbed.dem}"
VS="${GHOST_VIEWSCALE:-0.667}"
USERDIR_REAL="$HOME/Library/Application Support/darkplaces"
CONFIG="${GHOST_CONFIG:-$USERDIR_REAL/m5/config.cfg}"
mkdir -p "$OUT"

BIN="./darkplaces-sdl"
CLEANUP_BIN=""
if [ -n "${GHOST_BIN:-}" ]; then
	cp "$GHOST_BIN" "$ROOT/darkplaces-sdl.head"
	chmod +x "$ROOT/darkplaces-sdl.head"
	BIN="./darkplaces-sdl.head"
	CLEANUP_BIN="$ROOT/darkplaces-sdl.head"
fi
[ -x "$BIN" ] || { echo "ghostbed: no $BIN -- build first" >&2; exit 1; }
[ -f "$USERDIR_REAL/m5/$DEMO" ] || { echo "ghostbed: $USERDIR_REAL/m5/$DEMO missing (re-record per test/README.md)" >&2; exit 1; }

SB=$(mktemp -d "${TMPDIR:-/tmp}/ghostbed.XXXXXX")
mkdir -p "$SB/m5"
cp "$USERDIR_REAL/m5/$DEMO" "$SB/m5/"
[ "$CONFIG" != "none" ] && cp "$CONFIG" "$SB/m5/config.cfg"
{
	echo 'vid_borderless 1'
	echo 'vid_fullscreen 0'
	echo 'vid_width 1920'
	echo 'vid_height 1080'
	echo 'vid_vsync 0'
	echo 'cl_maxfps 0'
	echo 'cl_nettimesyncfactor 1'
	echo 'cl_nettimesyncboundmode 1'
	echo "r_viewscale $VS"
	printf '%s\n' "$LINES"
} > "$SB/m5/autoexec.cfg"

LOG="$OUT/$ARM.log"
# shellcheck disable=SC2086
env ${GHOST_ENV:-} RT_METAL_DUMP="$OUT/$ARM" RT_METAL_DUMPFRAMES="$FRAMES" \
	"$BIN" -quake -userdir "$SB" -nosound -benchmark "$DEMO" > "$LOG" 2>&1 < /dev/null || true

# the manifest: positive proof of what ran
{
	echo "arm $ARM"
	echo "binary $BIN $(strings "$BIN" 2>/dev/null | grep -m1 -E '^[a-z0-9-]+-[0-9]+-g[0-9a-f]{8} ' || echo '?')"
	echo "frames $FRAMES"
	echo "viewscale $VS"
	grep -m1 "using gamedirs" "$LOG" || echo "MISSING: using gamedirs"
	grep -m1 "execing autoexec.cfg" "$LOG" || echo "MISSING: execing autoexec.cfg"
	grep -m1 "Playing demo" "$LOG" || echo "MISSING: Playing demo"
	grep -m1 "Video Mode:" "$LOG" || echo "MISSING: Video Mode"
	grep -h "| result" "$SB/m5/benchmark.log" 2>/dev/null | tail -1 || echo "MISSING: benchmark result"
	printf 'lines:\n%s\n' "$LINES"
} > "$OUT/$ARM.manifest"

# a SINGLE requested frame is written to the bare path (rt_metal.m's dump
# naming only suffixes .f<N> when more than one frame is listed); rename it so
# every dump from this bed carries its frame number
case "$FRAMES" in
	*,*) ;;
	*) [ -f "$OUT/$ARM" ] && mv "$OUT/$ARM" "$OUT/$ARM.f$FRAMES" ;;
esac
n=$(ls "$OUT/$ARM".f* 2>/dev/null | wc -l | tr -d ' ')
echo "ghostbed: $ARM -> $n dump(s) in $OUT ($(grep -m1 'Video Mode:' "$LOG" | sed 's/.*Video Mode: //' || echo 'no video mode line'))"
grep -q "using gamedirs.*m5" "$LOG" || echo "ghostbed: WARNING -- m5 not mounted in $ARM (see $LOG)"

rm -rf "$SB"
[ -n "$CLEANUP_BIN" ] && rm -f "$CLEANUP_BIN"
exit 0
