#!/bin/sh
# test/perf/perf-sweep.sh -- the lever sweep driver (the 2026-08-12 perf plan).
#
# Runs the arms of levers.tsv against Seb's live config as baseline, one bed at
# a time, ROUND-ROBIN INTERLEAVED (warmup discarded, then R rounds of
# baseline + every arm in turn) so machine drift lands on all arms alike --
# the standing bench doctrine: never quote a single run, interleave, check
# uptime first. Results accumulate as TSV for perf-report.py.
#
# Usage:
#   sh test/perf/perf-sweep.sh A                # phase A screen (all A rows)
#   sh test/perf/perf-sweep.sh B 'fog|rtscale'  # phase B rows matching regex
#   PERF_KERNELMS=1 sh test/perf/perf-sweep.sh B fogstride
#   PERF_BEDS=demo11 sh test/perf/perf-sweep.sh B .   # override every row's bed
#
# Environment:
#   PERF_OUT        results dir (default test/perf/out -- gitignored)
#   PERF_ROUNDS     interleaved rounds (default 3)
#   PERF_FULLSCREEN 1 = 1920x1080 fullscreen (THE quotable block; default 1 --
#                   this suite exists to be run on the quiet 240 Hz display)
#   PERF_KERNELMS   1 = RT_METAL_KERNELMS=1 per run; stage ms columns captured
#   PERF_BEDS       space list overriding the tsv bed column (e.g. "demo11 demo14")
#   PERF_CONFIG     config to copy as baseline (default Seb's live config.cfg;
#                   'none' = engine defaults -- NOT the plan's shape, but useful)
#   PERF_ORDER      fixed (default, the historical behaviour) | alternate.
#                   ALTERNATE REVERSES THE ARM ORDER ON EVEN ROUNDS, which is
#                   the defence against a MONOTONIC within-round drift: with a
#                   fixed order, a machine that slows through each round taxes
#                   the late arms every single round and no median can cancel
#                   it. The 2026-08-29 evening suite's whole demo11 block was
#                   DISCARDED for exactly that (values fell monotonically
#                   through each round; the tell was an impossible ordering --
#                   GI rate 2 reading slower than rate 1). Under alternate,
#                   every arm gets an early slot and a late slot, so the ramp
#                   cancels in the per-arm median instead of biasing it.
#   PERF_ROTATE     1 = rotate the bed on EVERY run; the first matching arm is
#                   the reference and is run on every bed at both ends of the
#                   block. For feature-cost ladders; ignores ROUNDS/ORDER/TAILBASE.
#                   2 = PAIRED: the reference runs on the candidate's bed right
#                   before it (twice the runs, drift-immune per pair).
#   PERF_TAILBASE   1 = run the baseline arm a SECOND time at the end of each
#                   round (recorded as arm 'baseline_tail'). This MEASURES the
#                   within-round drift rather than assuming it: baseline and
#                   baseline_tail are the same arm at the two ends of the same
#                   round, so their gap IS the ramp, and a block whose gap
#                   exceeds its own arm deltas is not quotable. Default 0.
#
# Every run gets a fresh sandbox userdir (archived-cvar persistence trap), the
# demo copied in, the arm's lines in autoexec.cfg (loads AFTER config.cfg, so
# the overrides win), and -benchmark's parseable result line harvested from
# <sandbox>/m5/benchmark.log. -nosound throughout, per the brief.

set -eu
cd "$(dirname "$0")/../.."

PHASE="${1:?usage: perf-sweep.sh A|B [arm-filter-regex]}"
FILTER="${2:-.}"
LEVERS="test/perf/levers.tsv"
OUT="${PERF_OUT:-test/perf/out}"
ROUNDS="${PERF_ROUNDS:-3}"
FS="${PERF_FULLSCREEN:-1}"
ORDER="${PERF_ORDER:-fixed}"
TAILBASE="${PERF_TAILBASE:-0}"
USERDIR_REAL="$HOME/Library/Application Support/darkplaces"
CONFIG="${PERF_CONFIG:-$USERDIR_REAL/m5/config.cfg}"
RESULTS="$OUT/results.tsv"
mkdir -p "$OUT"

# --- preflight: the machine must be quiet and the binary current -------------
echo "== preflight =="
uptime
LOAD=$(uptime | sed 's/.*load averages*: *//' | awk '{print int($1)}')
[ "$LOAD" -ge 3 ] && echo "WARNING: load >= 3 -- absolutes will not be quotable (doctrine: ratios only)"
[ -x ./darkplaces-sdl ] || { echo "no ./darkplaces-sdl -- build first"; exit 1; }
# a stale binary silently poisons every number (the documented trap)
make sdl-release -j8 >/dev/null 2>&1 || { echo "build failed"; exit 1; }
system_profiler SPDisplaysDataType 2>/dev/null | grep -E "Resolution|UI Looks" | head -4 || true
echo "phase $PHASE, filter '$FILTER', rounds $ROUNDS, fullscreen $FS, kernelms ${PERF_KERNELMS:-0}, order $ORDER, tailbase $TAILBASE"

# vidmode is the run's `Video Mode:` line (2026-08-19): `vid_desktopfullscreen 1`
# intermittently comes up at 1920x1017 -- 1080 minus the menu bar -- which is
# not arm-dependent, does not show in the fps line, changes the pixel count by
# 6% and the RT trace buffer with it, and read as "temporal 60% FASTER than no
# upscaling" on 2026-08-18. The log was deleted per run before this, so nobody
# could check after the fact. Now every row carries its geometry, and a
# fullscreen run that did not land on the requested mode is flagged with a
# leading '!' on the arm name (perf-report.py drops those rows and says how
# many). A results.tsv from before this column has the old 14-field header;
# start a fresh PERF_OUT rather than mixing.
[ -f "$RESULTS" ] || printf 'ts\tphase\tbed\tarm\tround\tframes\tseconds\tfps\tmin1s\tavg1s\tmax1s\ttrace_ms\tfog_ms\tshaft_ms\tvidmode\n' > "$RESULTS"

# --- collect matching rows ----------------------------------------------------
ARMFILE=$(mktemp)
grep -v '^#' "$LEVERS" | awk -F'\t' -v p="$PHASE" 'NF >= 3 && $2 == p' | grep -E "$FILTER" > "$ARMFILE" || true
NARMS=$(wc -l < "$ARMFILE" | tr -d ' ')
[ "$NARMS" -gt 0 ] || { echo "no arms match phase $PHASE filter '$FILTER'"; exit 1; }

BEDS_OVERRIDE="${PERF_BEDS:-}"
BEDS=$( [ -n "$BEDS_OVERRIDE" ] && echo "$BEDS_OVERRIDE" || cut -f3 "$ARMFILE" | sort -u )
echo "$NARMS arms over beds: $(echo $BEDS | tr '\n' ' ')"

# --- one benchmark run --------------------------------------------------------
# $1 bed  $2 arm-name  $3 cvar-lines(;-sep)  $4 round-tag
run_one() {
	bed=$1; arm=$2; lines=$3; tag=$4
	SB=$(mktemp -d "${TMPDIR:-/tmp}/perf.XXXXXX"); mkdir -p "$SB/m5"
	cp "$USERDIR_REAL/m5/$bed.dem" "$SB/m5/"
	[ "$CONFIG" != "none" ] && cp "$CONFIG" "$SB/m5/config.cfg"
	{
		echo 'vid_vsync 0'          # benches must never present-throttle
		if [ "$FS" = "1" ]; then
			# PINNED, not inherited (2026-08-19): this block used to set the
			# size but not the MODE, so the suite ran whatever
			# vid_desktopfullscreen the copied config archived -- Seb's 0
			# (exclusive modeset) -- while bench-rt.sh pins desktop fullscreen,
			# and nobody could say which mode out-tiers2 used. Exclusive
			# 1920x1080 is what he plays and is immune to the 1017 class; the
			# geometry column below is what proves each run landed on it.
			echo 'vid_desktopfullscreen 0'
			echo 'vid_fullscreen 1'; echo 'vid_width 1920'; echo 'vid_height 1080'
		else
			echo 'vid_fullscreen 0'; echo 'vid_borderless 1'
		fi
		echo "$lines" | tr ';' '\n'
	} > "$SB/m5/autoexec.cfg"
	ENVV=""
	[ "${PERF_KERNELMS:-0}" = "1" ] && ENVV="RT_METAL_KERNELMS=1"
	LOG=$(mktemp)
	# stdin from /dev/null, and not as tidiness: run_one is called from inside a
	# `while read` loop, and a child that reads stdin EATS THE REST OF THE ARM
	# LIST -- the shakedown run lost two of its three arms to exactly this.
	# shellcheck disable=SC2086
	env $ENVV ./darkplaces-sdl -userdir "$SB" -nosound -benchmark "$bed.dem" >"$LOG" 2>&1 < /dev/null || true
	RES=$(grep -h "| result" "$SB/m5/benchmark.log" 2>/dev/null | tail -1 || true)
	if [ -z "$RES" ]; then
		echo "  $arm [$tag]: NO RESULT (crash or wrong demo?) -- log kept: $LOG"
		rm -rf "$SB"; return 0
	fi
	frames=$(echo "$RES"  | sed -n 's/.*result \([0-9]*\) frames.*/\1/p')
	seconds=$(echo "$RES" | sed -n 's/.*frames \([0-9.]*\) seconds.*/\1/p')
	fps=$(echo "$RES"     | sed -n 's/.*seconds \([0-9.]*\) fps.*/\1/p')
	mam=$(echo "$RES"     | sed -n 's/.*min\/avg\/max: \([0-9]*\) \([0-9]*\) \([0-9]*\).*/\1 \2 \3/p')
	# stage ms: average every RT_Metal-kern line the run printed
	tms=""; fms=""; sms=""
	if [ "${PERF_KERNELMS:-0}" = "1" ]; then
		set -- $(grep "RT_Metal-kern:" "$LOG" | sed -n 's/.*trace \([0-9.]*\) fog \([0-9.]*\) shaft \([0-9.]*\) ms.*/\1 \2 \3/p' \
			| awk '{t+=$1;f+=$2;s+=$3;n++} END{if(n)printf "%.3f %.3f %.3f",t/n,f/n,s/n}')
		tms=${1:-}; fms=${2:-}; sms=${3:-}
	fi
	# the geometry witness: the last Video Mode line the run printed. Under
	# FS=1 anything but the requested 1920x1080 flags the row ('!' on the arm),
	# is warned about here, and is dropped by perf-report.py -- recorded rather
	# than refused, because results.tsv is append-only and a flagged row is
	# itself evidence of the trap firing.
	vm=$(sed -n 's/^.*Video Mode: \(.*\) on display.*$/\1/p' "$LOG" | tail -1)
	armrec="$arm"
	if [ "$FS" = "1" ]; then
		case "$vm" in
			*" 1920x1080 "*) ;;
			*) echo "  WARNING: $arm [$tag] ran at '${vm:-?}' -- the 1920x1017 class; row flagged with '!'"; armrec="!$arm" ;;
		esac
	fi
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$(date +%H:%M:%S)" "$PHASE" "$bed" "$armrec" "$tag" \
		"$frames" "$seconds" "$fps" ${mam:-"- - -"} "${tms:--}" "${fms:--}" "${sms:--}" "${vm:--}" >> "$RESULTS"
	echo "  $arm [$tag]: $fps fps (1s min/avg/max $mam)${tms:+  stages t=$tms f=$fms s=$sms}  [$vm]"
	rm -f "$LOG"; rm -rf "$SB"
}

# --- PERF_ROTATE: the bed changes on EVERY run --------------------------------
# Seb, 2026-09-04: "run each demo in turn, not 4-5 runs of the same demo". It is
# also the better instrument for a FEATURE-COST ladder: the FIRST matching arm is
# the reference and is run once per bed at BOTH ends of the block, so every
# candidate has a same-bed reference bracketing it and the two reference passes
# are that bed's drift witness. Candidates then take beds round-robin, so no two
# consecutive runs share a map and no arm sits permanently in a taxed slot.
# Read each candidate as a RATIO against its own bed's reference -- absolutes
# across beds are different maps and are not comparable.
if [ "${PERF_ROTATE:-0}" = "1" ] || [ "${PERF_ROTATE:-0}" = "2" ]; then
	NBEDS=$(echo $BEDS | wc -w | tr -d ' ')
	REFARM=$(head -1 "$ARMFILE" | cut -f1)
	REFLINES=$(head -1 "$ARMFILE" | cut -f4)
	CAND=$(mktemp); tail -n +2 "$ARMFILE" > "$CAND"
	echo "== rotate: reference '$REFARM' on $NBEDS beds, then $(wc -l < "$CAND" | tr -d ' ') candidates round-robin, then the reference again =="
	# PERF_ROTATE=2 is the PAIRED form: the reference runs on the candidate's own
	# bed IMMEDIATELY before it, so each pair spans two runs and a drift that
	# moved the block 29% end to end (2026-09-04, post-reboot, no soak) taxes a
	# pair by a percent or two. Twice the runs; the only drift-immune shape.
	PAIRED=0; [ "$PERF_ROTATE" = "2" ] && PAIRED=1
	if [ "$PAIRED" = "0" ]; then for bed in $BEDS; do run_one "$bed" "$REFARM" "$REFLINES" ref1; done; fi
	i=0
	# not a pipeline: a `while read` on the right of a pipe runs in a subshell
	# and $i would not survive it, so every candidate would land on bed 1.
	# The read variables are NOT arm/lines: run_one assigns those as shell
	# globals, so the paired form's first call (the reference) clobbered the
	# candidate's before its second call could use them, and out-feat2 ran the
	# reference twenty-two times. (It measured the paired noise floor: ~1%.)
	while IFS="$(printf '\t')" read -r carm cphase cabed clines cvw cnote; do
		cbed=$(echo $BEDS | awk -v k=$((i % NBEDS)) '{print $(k + 1)}')
		[ "$PAIRED" = "1" ] && run_one "$cbed" "$REFARM" "$REFLINES" "p$i"
		run_one "$cbed" "$carm" "$clines" "$([ "$PAIRED" = "1" ] && echo "p$i" || echo c)"
		i=$((i + 1))
	done < "$CAND"
	if [ "$PAIRED" = "0" ]; then for bed in $BEDS; do run_one "$bed" "$REFARM" "$REFLINES" ref2; done; fi
	rm -f "$CAND" "$ARMFILE"
	echo "== done =="; uptime
	echo "results: $RESULTS"
	exit 0
fi

# --- the interleaved sweep, one bed at a time ---------------------------------
for bed in $BEDS; do
	[ -f "$USERDIR_REAL/m5/$bed.dem" ] || { echo "SKIP bed $bed: no such demo"; continue; }
	BEDARMS=$(mktemp)
	if [ -n "$BEDS_OVERRIDE" ]; then cp "$ARMFILE" "$BEDARMS"; else awk -F'\t' -v b="$bed" '$3 == b' "$ARMFILE" > "$BEDARMS"; fi
	N=$(wc -l < "$BEDARMS" | tr -d ' '); [ "$N" -gt 0 ] || { rm -f "$BEDARMS"; continue; }
	echo "== bed $bed: baseline + $N arms x $ROUNDS rounds =="
	run_one "$bed" baseline "" warmup   # discarded by the report (round=warmup)
	r=1
	while [ "$r" -le "$ROUNDS" ]; do
		run_one "$bed" baseline "" "$r"
		# PERF_ORDER=alternate reverses the arm order on even rounds so a
		# monotonic within-round drift taxes each arm in one round and spares
		# it in the next, instead of always taxing the same tail (see the
		# header). 'fixed' reproduces the historical order exactly.
		ORDERED="$BEDARMS"
		if [ "$ORDER" = "alternate" ] && [ $((r % 2)) -eq 0 ]; then
			ORDERED=$(mktemp)
			awk '{ l[NR] = $0 } END { for (i = NR; i >= 1; i--) print l[i] }' "$BEDARMS" > "$ORDERED"
		fi
		while IFS="$(printf '\t')" read -r arm phase abed lines vw note; do
			run_one "$bed" "$arm" "$lines" "$r"
		done < "$ORDERED"
		[ "$ORDERED" = "$BEDARMS" ] || rm -f "$ORDERED"
		# the drift witness: the same baseline at the other end of the round.
		# An explicit `if`, not `[ ... ] && run_one`: this is the LAST command
		# of the loop body and a false test there is a non-zero status under
		# `set -e`.
		if [ "$TAILBASE" = "1" ]; then run_one "$bed" baseline_tail "" "$r"; fi
		r=$((r + 1))
	done
	rm -f "$BEDARMS"
done
rm -f "$ARMFILE"
echo "== done =="
uptime
echo "results: $RESULTS  (render with: python3 test/perf/perf-report.py $OUT)"
