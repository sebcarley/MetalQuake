#!/bin/sh
# ---------------------------------------------------------------------------
# QuakeM5 command-digest gate driver (METAL.md Phase 1).
#
#   test/cmdtrace.sh baseline   build the trace binary, capture the golden digest
#   test/cmdtrace.sh check      rebuild, re-capture, and diff against the baseline
#
# WHAT IT PROVES. Every backend call the renderer makes is hashed into a
# per-frame digest. Two builds that issue an IDENTICAL call stream produce an
# identical digest, which is a behavioural proof that does not depend on the
# driver, the GPU or the screenshot noise floor. It exists because CLAUDE.md
# records the demo5 byte gate going blind twice; its own lesson is "when the
# gate is blind, find a counter".
#
# WORKFLOW around a refactor that should change nothing (Phase 3's 167-site
# uniform indirection is the first customer):
#
#     test/cmdtrace.sh baseline    # BEFORE the change
#     ...make the change, then...
#     test/cmdtrace.sh check       # must report PASS
#
# THE BED IS A FROZEN SCENE, NOT A DEMO, and that was measured rather than
# assumed: two boots of one binary agreed on 0 of 6124 demo5 frames, because the
# demo's own simulation diverges and drags visibility and draw counts with it.
# A frozen scene instead yields ONE constant frame state, so "every tail frame
# is identical" is a self-check on the instrument before any comparison is made.
#
# LEVELS. 1 = structure (call identity, order, counts) and is the only level
# proven stable across processes -- use it for gating. 2 adds integer arguments
# and 3 adds float bits; both are still run-varying (see METAL.md) and are
# diagnostic aids, not gates.
# ---------------------------------------------------------------------------
set -eu
cd "$(dirname "$0")/.." || exit 1
ROOT="$(pwd)"

MODE="${1:-}"
LEVEL="${LEVEL:-1}"
MAP="${MAP:-e1m3}"
ORIGIN="${ORIGIN:-544 288 88}"
TAIL="${TAIL:-60}"
EXE="darkplaces-sdl-cmdtrace"
OBJDIR="build-obj/release/$EXE"
OUT="$ROOT/test/results"
BASE="$ROOT/test/baseline/cmdtrace-L$LEVEL.txt"

case "$MODE" in
	baseline|check) ;;
	*) echo "usage: test/cmdtrace.sh baseline|check    (env: LEVEL MAP ORIGIN TAIL)"; exit 2 ;;
esac

mkdir -p "$OUT" "$ROOT/test/baseline"

# --- build -----------------------------------------------------------------
# Two traps, both real, both turned into guardrails here:
#  (a) object dirs are keyed on build TYPE, never on CFLAGS, so a flag change
#      would silently relink stale untraced objects and the digest would be a
#      lie. Hence the unconditional rm -rf.
#  (b) every SDL make target writes darkplaces-sdl and release STRIPS it, so a
#      trace build would clobber the shipping binary indistinguishably. Hence
#      EXE_UNIXSDL.
echo "cmdtrace: building $EXE (this removes $OBJDIR first, by design)"
rm -rf "$OBJDIR"
make sdl-release -j8 EXE_UNIXSDL="$EXE" CFLAGS_EXTRA=-DDP_CMDTRACE >"$OUT/cmdtrace-build.log" 2>&1 || {
	echo "cmdtrace: BUILD FAILED, see $OUT/cmdtrace-build.log"; exit 1; }

# --- run -------------------------------------------------------------------
SANDBOX=$(mktemp -d)
trap 'rm -rf "$SANDBOX"' EXIT
mkdir -p "$SANDBOX/m5"
cat > "$SANDBOX/m5/cmdtrace.cfg" <<EOF
defer 2 "sv_cheats 1"
defer 3 "noclip"
defer 4 "prvm_edictset server 1 origin \\"$ORIGIN\\""
defer 5 "sv_freezenonclients 1"
defer 6 "cl_particles 0"
defer 12 quit
EOF

# +vid_renderer gl is PINNED, not inherited: this instrument hashes the GL
# backend call stream by definition, and once vid_renderer defaults to metal a
# bootful of Metal calls would be a different digest measuring a different
# renderer. A pin cannot move under the bed; a default can (the 6-3b lesson).
# m5_torch_embers 0 is pinned on a MEASUREMENT rather than an expectation, and
# the measurement is worth keeping. With the emitter at its new default 3 this
# digest reads 1653 and with it at 0 it reads 1647, three runs each way, stable.
# The embers are long dead by the frozen tail -- but R_DrawParticles gates its
# whole walk on `if (!cl.num_particles) return;`, and that count does not fall
# back to zero once the pool has been populated, so the particle pass goes on
# issuing its half-dozen setup calls every frame for the rest of the map. The
# bed's own `cl_particles 0` at 6 s cannot undo it: that gate is in
# CL_NewParticle (spawning) and not in the walk. Pre-existing engine behaviour
# that the embers merely trigger first on this bed.
#
# m5_dust 0 is pinned for EXACTLY that mechanism, one default flip later: the
# ambient dust defaults to 512 as of 2026-09-19, its emitter populates the
# particle pool within the first second, and cl.num_particles never falls back
# to zero -- so without this pin the particle pass's setup calls join the frozen
# tail's stream for the rest of the map and the digest moves for a reason that
# has nothing to do with whatever is being gated.
#
# r_skylightning 0 and r_caustics 0 are pinned for the same reason one level
# along (2026-09-17, the day both went to DEFAULT ON). The storm pushes a scene
# LIGHT on a random schedule, which is both a call-stream change and a
# nondeterminism this instrument cannot tolerate; the caustics ride
# USEVOLUMETRICLIQUIDFADE, which the pin above already keeps out of the shader,
# so that one is a statement rather than a load-bearing pin -- but a statement
# is what stops the next reader assuming the liquid-fade pin will stay.
#
# The BEAUTY A4/A5 pins, added the day both went to DEFAULT ON (2026-09-17):
# cl_particles_soft and cl_particles_refract are STATIC PARMS on MODE_GENERIC,
# so a non-zero default compiles a depth unit (and a frame-copy unit) into every
# generic permutation and feeds them per batch -- the r_volumetric_liquidfade
# shape exactly, which moved this digest 1645 -> 1942 when it flipped. Pinned, so
# the recorded 1645 stays a measurement rather than being re-baselined.
#
# The swirl pins (F3): with either cvar on, USEWATERSWIRL compiles WaterParams
# into every surface permutation and its per-batch Uniform4f moves the digest;
# pinned off, the parm is out and the call stream is the recorded baseline.
# r_volumetric_liquidfade is the SAME SHAPE and is pinned for the same reason
# (LIQUIDFOG, 2026-08-31): SHADERSTATICPARM_VOLUMETRICLIQUIDFADE is gated on
# that cvar ALONE -- deliberately, because the murk's own gate moves with the
# map and following it would rebuild every shader on a map change -- so a
# non-zero value compiles the fade into every surface permutation and
# R_Volumetric_LiquidFadeUniforms then issues SEVEN uniform calls per surface
# batch, on liquid and non-liquid batches alike. It was pinned the day its
# default became 0.5; before that the bed was riding the default, which is
# exactly the "a default can move under a bed; a pin cannot" trap one line up.
DP_CMDTRACE="$LEVEL" DP_CMDTRACE_OUT=cmdtrace.txt \
"$ROOT/$EXE" -userdir "$SANDBOX" -window -nosound +vid_renderer gl \
	+vid_width 1280 +vid_height 720 +vid_borderless 1 \
	+cl_nettimesyncfactor 1 +cl_nettimesyncboundmode 1 +vid_vsync 0 \
	+r_waterswirl 0 +r_teleportswirl 0 +r_volumetric_liquidfade 0 +r_watersurface 0 \
	+cl_particles_soft 0 +cl_particles_refract 0 +r_caustics 0 +r_skylightning 0 \
	+m5_torch_embers 0 +m5_dust 0 \
	+map "$MAP" +exec cmdtrace.cfg >"$OUT/cmdtrace-run.log" 2>&1

# the banner is the anti-stale-binary check: a build that should be tracing but
# silently is not would otherwise look like a clean PASS
grep -q "DP_CMDTRACE: command-digest build ACTIVE" "$OUT/cmdtrace-run.log" || {
	echo "cmdtrace: FAIL - the binary is not a trace build (stale objects?)"; exit 1; }
[ -f "$SANDBOX/m5/cmdtrace.txt" ] || { echo "cmdtrace: FAIL - no digest written"; exit 1; }

# --- reduce ----------------------------------------------------------------
# The tail of a frozen scene should be ONE repeated state. Anything else means
# the scene is not actually frozen and no comparison from it can be trusted.
DIGEST="$OUT/cmdtrace-L$LEVEL.txt"
tail -"$TAIL" "$SANDBOX/m5/cmdtrace.txt" | grep -v '^#' | awk '{print $2, $3}' | sort -u > "$DIGEST"
STATES=$(wc -l < "$DIGEST" | tr -d ' ')
if [ "$STATES" -ne 1 ]; then
	echo "cmdtrace: WARNING - the frozen tail has $STATES distinct states, expected 1:"
	sed 's/^/    /' "$DIGEST"
	[ "$LEVEL" = "1" ] && { echo "cmdtrace: FAIL - level 1 must be constant on a frozen scene"; exit 1; }
fi

# --- baseline / check ------------------------------------------------------
if [ "$MODE" = "baseline" ]; then
	cp "$DIGEST" "$BASE"
	echo "cmdtrace: baseline captured (level $LEVEL, $MAP): $(cat "$BASE")"
	exit 0
fi

[ -f "$BASE" ] || { echo "cmdtrace: FAIL - no baseline; run 'test/cmdtrace.sh baseline' first"; exit 1; }
if cmp -s "$DIGEST" "$BASE"; then
	echo "cmdtrace: PASS - identical backend call stream ($(cat "$BASE"))"
	exit 0
fi
echo "cmdtrace: FAIL - the backend call stream CHANGED"
echo "    baseline: $(cat "$BASE")"
echo "    now:      $(cat "$DIGEST")"
echo "  (first field is the call count: a difference there is calls added or removed)"
exit 1
