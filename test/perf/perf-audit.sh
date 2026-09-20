#!/bin/sh
# test/perf/perf-audit.sh -- the "what actually runs where" probe (plan area 2).
#
# Boots the shipped binary at Seb's config on two maps and harvests the
# arm-liveness lines into audit.txt, because the true answer to "is the fog
# Metal?" is a runtime fact, not a grep of the source: the kernel falls back
# to the GL march SILENTLY by design, and a parity bed measures agreement, not
# liveness. The report embeds the result.
#
# What each line proves:
#   "Metal renderer up"            the renderpath itself
#   "RT fog kernel ACTIVE"         the fog integral runs in the Metal compute
#                                  kernel (take the LAST state -- the first
#                                  frames legitimately print the fallback)
#   "falling back to the GL march" if this is the LAST state, the murk's fog
#                                  lighting is NOT running in the kernel
#   "compiled" lines               which shader modes were built this boot; on
#                                  the Metal path these are MSL compiles under
#                                  shared naming
#   "sentinel" / "will not compile" a mode fell to the magenta sentinel --
#                                  an unported shape got reached
set -eu
cd "$(dirname "$0")/../.."

OUT="${PERF_OUT:-test/perf/out}"; mkdir -p "$OUT"
AUD="$OUT/audit.txt"
USERDIR_REAL="$HOME/Library/Application Support/darkplaces"

boot() { # $1 map  $2 extra +cmds
	SB=$(mktemp -d "${TMPDIR:-/tmp}/audit.XXXXXX"); mkdir -p "$SB/m5"
	cp "$USERDIR_REAL/m5/config.cfg" "$SB/m5/config.cfg"
	LOG=$(mktemp)
	# shellcheck disable=SC2086
	./darkplaces-sdl -userdir "$SB" -window -nosound +developer 1 $2 +map "$1" +defer 8 quit >"$LOG" 2>&1 || true
	echo "=== map $1 $2 ==="
	grep -m1 "Metal renderer up\|Metal video:" "$LOG" || echo "NO METAL RENDERER LINE"
	grep "RT fog kernel" "$LOG" | tail -1 || echo "no fog kernel state line (murk off?)"
	grep -m1 "RT_Metal: device" "$LOG" || echo "NO SIDECAR DEVICE"
	grep -cE "compiled \(" "$LOG" | sed 's/^/shader modes compiled: /'
	grep -iE "sentinel|will not compile" "$LOG" | head -5 || true
	grep -i "falling back" "$LOG" | sort | uniq -c || true
	grep -E "M5 lava: heat haze marching|volumetric irradiance|volumetric field:" "$LOG" || true
	rm -f "$LOG"; rm -rf "$SB"
	echo
}

{
	echo "perf-audit $(date '+%Y-%m-%d %H:%M')  binary: $(strings darkplaces-sdl | grep -om1 'pre-m5-[a-z]*-[0-9]*-g[0-9a-f]*')"
	echo
	boot e1m3 "+r_volumetric 1 +rt_metal 1 +rt_metal_fog 1"
	boot e1m7 "+r_volumetric 1 +rt_metal 1 +rt_metal_fog 1 +r_lavashimmer 4"
	echo "=== static census: renderpath-gated code that could still reach the Metal path ==="
	echo "(equality tests on renderpath -- each should name what it LACKS, the 6-4 rule)"
	grep -n "renderpath == RENDERPATH_GL" gl_rmain.c r_shadow.c gl_backend.c 2>/dev/null | head -12 || true
} > "$AUD"
cat "$AUD"
echo "wrote $AUD"
