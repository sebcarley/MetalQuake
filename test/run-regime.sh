#!/bin/sh
# ---------------------------------------------------------------------------
# DarkPlaces RT-work test regime driver (read-only correctness + perf nets).
#
#   test/run-regime.sh baseline   capture golden trace dumps into test/baseline/
#   test/run-regime.sh check      re-run and diff against the baseline (PASS/FAIL)
#   test/run-regime.sh perf       run the windowed -benchmark demos (needs a GL display)
#
# The correctness net (trace parity) runs on the headless dedicated server, so
# it needs no display and does not disturb the desktop. It fires a deterministic,
# seeded set of collision traces through the same ray-cast core that server
# physics and player movement use; a byte-identical dump before vs after a change
# proves the change is gameplay-safe. The reported traces/sec is the ray-cast
# micro-benchmark for the BIH/triangle optimizations (#2/#3).
#
# Perf net: the stock demos are replayed with -benchmark (already srand(0)
# deterministic) writing id1/benchmark.log.
# ---------------------------------------------------------------------------
set -eu
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
UD="$ROOT/test/runtime"          # isolated userdir so runs never touch ~/Library
SV="$ROOT/darkplaces-dedicated"
CL="$ROOT/darkplaces-sdl"
MAPS="${MAPS:-start e1m1 e1m2 e1m3}"
COUNT="${COUNT:-20000}"
SEED="${SEED:-1}"
DEMOS="${DEMOS:-demo1 demo2 demo3}"

mkdir -p "$ROOT/test/baseline" "$ROOT/test/results" "$UD/id1"

# run the headless dedicated server, wait for the dump to finish, then stop it
run_dump() { # map outfile
	map="$1"; out="$2"; target="$UD/id1/$out"
	rm -f "$target"
	"$SV" -userdir "$UD" +map "$map" +collision_dumptraces "$COUNT" "$SEED" "$out" >/tmp/dp-regime.log 2>&1 &
	pid=$!
	i=0
	while [ $i -lt 120 ]; do
		if [ -f "$target" ] && grep -q "# hash" "$target" 2>/dev/null; then break; fi
		kill -0 $pid 2>/dev/null || break
		sleep 0.5; i=$((i+1))
	done
	kill -9 $pid 2>/dev/null || true
	wait $pid 2>/dev/null || true
	[ -f "$target" ] || { echo "  ERROR: no dump produced for $map (see /tmp/dp-regime.log)"; return 1; }
}

cmd_traces() { # mode: baseline|check
	mode="$1"; fail=0
	[ -x "$SV" ] || { echo "missing $SV — run 'make sv-release' first"; exit 1; }
	for m in $MAPS; do
		out="tracedump-$m.txt"
		printf '%-8s ' "$m"
		run_dump "$m" "$out" || { fail=1; continue; }
		res="$UD/id1/$out"
		hash=$(sed -n 's/^# hash \([0-9a-f]*\).*/\1/p' "$res")
		cp "$res" "$ROOT/test/results/$out"
		if [ "$mode" = baseline ]; then
			cp "$res" "$ROOT/test/baseline/$out"
			echo "baseline captured  hash $hash"
		else
			base="$ROOT/test/baseline/$out"
			if [ ! -f "$base" ]; then echo "NO BASELINE (run 'baseline' first)"; fail=1
			elif diff -q "$base" "$res" >/dev/null 2>&1; then echo "PASS  hash $hash (bit-identical)"
			else echo "FAIL  trace results changed vs baseline — gameplay-affecting!"; fail=1
				diff "$base" "$res" | head -6 | sed 's/^/    /'
			fi
		fi
	done
	return $fail
}

cmd_perf() {
	[ -x "$CL" ] || { echo "missing $CL — run 'make sdl-release' first"; exit 1; }
	echo "Running windowed -benchmark demos (this takes over the display briefly)..."
	for d in $DEMOS; do
		"$CL" -userdir "$UD" -benchmark "$d.dem" +exec "$ROOT/test/bench.cfg" || true
		echo "--- $d ---"; tail -3 "$UD/id1/benchmark.log" 2>/dev/null || true
	done
}

case "${1:-check}" in
	baseline) echo "== capturing trace-parity baselines =="; cmd_traces baseline ;;
	check)    echo "== trace-parity check vs baseline =="; cmd_traces check ;;
	perf)     cmd_perf ;;
	*) echo "usage: $0 [baseline|check|perf]"; exit 2 ;;
esac
