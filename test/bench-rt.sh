#!/bin/sh
# test/bench-rt.sh -- the interleaved A/B bench harness (METAL.md Phase 8-1b).
#
# VERIFY THE GEOMETRY OF EVERY RUN BEFORE COMPARING THEM. On this machine
# `vid_desktopfullscreen 1` intermittently comes up at **1920x1017** instead of
# 1920x1080 -- 1080 minus the 63-pixel menu bar, the window-clamp trap CLAUDE.md
# already records for screenshots, reaching the fullscreen path too. It is not
# arm-dependent and it does not announce itself in the fps line; it changes the
# pixel count by 6%, the RT trace buffer with it (641x361 vs 641x340), and it
# poisons any A/B whose two arms happen to land on different desktop modes.
# Measured 2026-08-18: a fullscreen ladder read "temporal 60% FASTER than doing
# no upscaling at all", which is impossible, and the whole result was two arms
# rendering different numbers of pixels. The `Video Mode:` line in each run's
# log states what was actually used -- read it, or compare windowed with the
# viewport confirmed, before believing any ratio. Since 2026-08-19 the engine
# also appends `| mode <kind> WxH` (read at the END of the run, from vid.mode)
# to every benchmark.log result line, so results.log below carries each arm's
# geometry beside its fps and the check no longer depends on keeping the log.
#
# The Phase 8 flip session's demo11 bench method did not survive anywhere on
# disk -- no script, no cfg, and benchmark.log's commandline field recorded
# neither the backend nor the knobs, so the headline numbers cannot be
# reproduced from any committed artefact. This harness is that method, written
# down: sandboxed userdir, Seb's demo and config copied in, the determinism
# pins applied through autoexec.cfg (which loads AFTER config.cfg, so the pins
# win), the backend passed on the COMMAND LINE so the log's commandline field
# finally records it, and the arms interleaved A B A B ... with a discarded
# warm-up first, per the standing bench doctrine (CLAUDE.md: never quote a
# single run; interleave; check uptime first).
#
# Usage:
#   sh test/bench-rt.sh                     # 3 rounds, windowed, metal vs metal (null A/B)
#   BENCH_B='rt_metal_sameframe 1' sh test/bench-rt.sh
#   BENCH_B_ENV='RT_METAL_SYNC=1' sh test/bench-rt.sh          # env-var arm
#   BENCH_A_RENDERER=gl BENCH_B_RENDERER=metal sh test/bench-rt.sh
#   BENCH_FULLSCREEN=1 BENCH_ROUNDS=4 sh test/bench-rt.sh      # the honest block
#
# Environment:
#   BENCH_DEMO        demo file to copy in (default: Seb's m5/demo11.dem)
#   BENCH_CONFIG      config.cfg to copy in (default: Seb's live m5/config.cfg;
#                     the word `none` runs at engine defaults)
#   BENCH_A, BENCH_B  extra console lines per arm, newline-separated (autoexec)
#   BENCH_A_ENV, BENCH_B_ENV   environment assignments per arm (VAR=value ...)
#   BENCH_RENDERER    vid_renderer for both arms (default metal);
#                     BENCH_A_RENDERER / BENCH_B_RENDERER override per arm
#   BENCH_FULLSCREEN  1 = 1920x1080 desktop fullscreen (takes the display);
#                     default windowed-borderless (the load-noise caveat applies
#                     doubly there -- fullscreen is the quotable block)
#   BENCH_ROUNDS      interleaved rounds (default 3 -> warmup + A B A B A B)
#   BENCH_DUMPFRAMES  arm RT_METAL_DUMP at these playback frames per run
#                     (e.g. "1450,2800"); dumps land beside the results
#   BENCH_OUT         results dir (default: a fresh mktemp -d, printed)
#
# The one-second min/avg/max on each line is the engine's own timedemo
# accounting (cl_demo.c); the summary quotes total fps and the 1-sec minimum,
# which is what the Phase 8 flip was judged on. Numbers taken under load are
# not quotable as absolutes -- the harness prints uptime before and after so
# the log carries the conditions.

set -eu
cd "$(dirname "$0")/.."

USERDIR_REAL="$HOME/Library/Application Support/darkplaces"
DEMO="${BENCH_DEMO:-$USERDIR_REAL/m5/demo11.dem}"
CONFIG="${BENCH_CONFIG:-$USERDIR_REAL/m5/config.cfg}"
ROUNDS="${BENCH_ROUNDS:-3}"
OUT="${BENCH_OUT:-$(mktemp -d /tmp/bench-rt.XXXXXX)}"
mkdir -p "$OUT"
RENDERER="${BENCH_RENDERER:-metal}"
A_RENDERER="${BENCH_A_RENDERER:-$RENDERER}"
B_RENDERER="${BENCH_B_RENDERER:-$RENDERER}"

[ -x ./darkplaces-sdl ] || { echo "bench-rt: ./darkplaces-sdl missing -- run from the repo root after make sdl-release" >&2; exit 1; }
[ -f "$DEMO" ] || { echo "bench-rt: demo not found: $DEMO" >&2; exit 1; }
DEMOBASE="$(basename "$DEMO")"

echo "bench-rt: binary $(strings ./darkplaces-sdl | grep -m1 'Clang' || true)"
echo "bench-rt: demo $DEMOBASE  rounds $ROUNDS  A=$A_RENDERER  B=$B_RENDERER  out $OUT"
echo "bench-rt: machine before: $(uptime)"

# one run: $1 arm-name, $2 renderer, $3 extra-cfg-lines, $4 extra-env, $5 tag
run_one() {
	SB=$(mktemp -d)
	mkdir -p "$SB/m5"
	cp "$DEMO" "$SB/m5/$DEMOBASE"
	if [ "$CONFIG" != "none" ] && [ -f "$CONFIG" ]; then
		cp "$CONFIG" "$SB/m5/config.cfg"
	fi
	# The pins ride autoexec.cfg, which quake.rc execs AFTER config.cfg, so
	# they beat anything the copied config archived (the rt-suite.py pattern).
	# vid_borderless 1 is load-bearing for any dump geometry (the WM clamp
	# trap); the nettimesync pair pins cl.time to frame count.
	{
		echo 'vid_borderless 1'
		echo 'cl_nettimesyncfactor 1'
		echo 'cl_nettimesyncboundmode 1'
		echo 'vid_vsync 0'
		echo 'cl_maxfps 0'
		echo 'cl_maxidlefps 0'
		if [ "${BENCH_FULLSCREEN:-0}" = "1" ]; then
			echo 'vid_width 1920'
			echo 'vid_height 1080'
			echo 'vid_fullscreen 1'
			echo 'vid_desktopfullscreen 1'
		else
			echo 'vid_fullscreen 0'
		fi
		if [ -n "$3" ]; then printf '%s\n' "$3"; fi
	} > "$SB/m5/autoexec.cfg"

	DUMPENV=""
	if [ -n "${BENCH_DUMPFRAMES:-}" ]; then
		DUMPENV="RT_METAL_DUMP=$OUT/dump_$5 RT_METAL_DUMPFRAMES=$BENCH_DUMPFRAMES"
	fi
	# +vid_renderer on the COMMAND LINE: read once at VID_InitMode, and -- the
	# point -- recorded verbatim in benchmark.log's commandline field, so the
	# backend of every line is recoverable from the log alone.
	# RT_METAL_PROFILE=1: the 120-frame profile line is gated off in ordinary
	# play (the console-volume incident); a bench run is a verification run and
	# the per-run logs keep the trace cpu/gpu numbers.
	env RT_METAL_PROFILE=1 $4 $DUMPENV ./darkplaces-sdl -userdir "$SB" -nosound \
		-benchmark "$DEMOBASE" +vid_renderer "$2" \
		> "$OUT/$5.log" 2>&1 || true
	LINE=$(tail -1 "$SB/m5/benchmark.log" 2>/dev/null || true)
	if [ -z "$LINE" ]; then
		echo "bench-rt: $5: NO BENCHMARK LINE -- see $OUT/$5.log (first lines follow)" >&2
		head -5 "$OUT/$5.log" >&2 || true
		rm -rf "$SB"
		return 1
	fi
	printf '%s\n' "$LINE" >> "$OUT/results.log"
	# result <frames> frames <secs> seconds <fps> fps, one-second fps min/avg/max: <mn> <av> <mx>
	FPS=$(printf '%s' "$LINE"  | sed 's/.*seconds \([0-9.]*\) fps.*/\1/')
	MINS=$(printf '%s' "$LINE" | sed 's/.*min\/avg\/max: \([0-9.]*\) \([0-9.]*\) \([0-9.]*\).*/\1 \2 \3/')
	echo "  $5 ($2): $FPS fps   1-sec min/avg/max: $MINS"
	rm -rf "$SB"
}

: > "$OUT/results.log"
echo "bench-rt: warm-up (discarded)"
run_one A "$A_RENDERER" "${BENCH_A:-}" "${BENCH_A_ENV:-}" warmup || exit 1

i=1
while [ "$i" -le "$ROUNDS" ]; do
	run_one A "$A_RENDERER" "${BENCH_A:-}" "${BENCH_A_ENV:-}" "A_r$i" || exit 1
	run_one B "$B_RENDERER" "${BENCH_B:-}" "${BENCH_B_ENV:-}" "B_r$i" || exit 1
	i=$((i + 1))
done

echo "bench-rt: machine after: $(uptime)"
echo "bench-rt: summary (excluding warm-up; results.log line 1 is the warm-up)"
for arm in A B; do
	if [ "$arm" = A ]; then OFF=0; else OFF=1; fi
	FPSLIST=""
	i=1
	while [ "$i" -le "$ROUNDS" ]; do
		L=$(sed -n "$((2 * i + OFF))p" "$OUT/results.log")
		F=$(printf '%s' "$L" | sed 's/.*seconds \([0-9.]*\) fps.*/\1/')
		FPSLIST="$FPSLIST $F"
		i=$((i + 1))
	done
	echo "  arm $arm fps:$FPSLIST"
done
echo "bench-rt: raw lines in $OUT/results.log; per-run logs beside it"
