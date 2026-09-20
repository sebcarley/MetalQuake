#!/bin/sh
# test/rollbed.sh -- the frozen bed test/roll.py's numbers are taken on.
#
# A FROZEN e1m3 scene with the murk winds pinned to zero, host_framerate
# pinning host.realtime to frame count, and ten CONSECUTIVE screenshots. With
# the scene frozen and the winds zeroed, the ONLY thing that changes between
# consecutive frames is the RT kernels' jitter phase and the temporal EMAs that
# ride it -- which is what makes the RT_METAL_FRAMEPIN=1 arm come out
# byte-identical frame to frame, and what makes every difference the base arm
# shows attributable to the jitter and to nothing else.
#
# Design notes, each of which cost something to learn:
#
#  * host_framerate 0.05 makes every `defer` step of 0.05 exactly one frame,
#    which is what "consecutive" means here. Without it the shots land at
#    whatever wall-clock spacing the machine happens to give.
#  * sv_freezenonclients BEFORE the map load pins sv.time, so lightstyles do
#    not tick between shots (the parity harness's rule, same reason).
#  * The murk winds are pinned QUOTED -- the console keeps only the first token
#    of an unquoted multi-token cvar value and would silently set "0".
#  * NEUTRAL GAMMA IS A MEASUREMENT CONDITION. At Seb's archived gamma a fog
#    crop sits in the bottom few levels of an 8-bit screenshot and the
#    high-passed signal IS the quantiser (measured: hp energy 0.28 of 255,
#    every arm reading INCOHERENT). 6-5's look A/B re-took its numbers the same
#    way for the same reason.
#  * rt_metal_bluenoise IS PINNED, because it defaults to 1 while Seb archives
#    0: a bed that writes its own autoexec and does not copy his config silently
#    measures the blue-noise arm instead of his. It announced itself as an exact
#    r = 1.0000 at one frame gap and nowhere else -- blue-noise slices cycle
#    with period 8.
#  * The screenshot console command captures the PREVIOUS frame, and its own
#    "Wrote ..." line lands in the notify area -- so keep any crop away from the
#    top-left text, exactly as the one-shot-per-boot rule warns.
#
# THIS IS A METRIC BED, NOT A BYTE BED, and the distinction cost a wrong verdict.
# Two boots of the SAME binary produce ten DIFFERENT screenshots -- measured,
# mean channel delta 0.999 with max 211 -- because the sidecar's temporal chains
# seed from the pre-map composite count and the load time varies per boot
# (RT_Metal_ResetTemporal is only called at timedemo start, and this is not a
# timedemo). So an md5 comparison across boots reads "all ten differ" for an
# unchanged binary, and reading that as a byte-gate failure is exactly the trap
# CLAUDE.md already records for the demo5 gate. Use roll.py / weave.py /
# sharpness.py numbers from this bed and test/parity-4a.sh for byte gates --
# its frozen vantages have two-boot controls that really do come out at zero.
#
# Usage: sh test/rollbed.sh <outdir> <tag> [extra-cfg-lines] [extra-env]
#   sh test/rollbed.sh /tmp/roll base
#   sh test/rollbed.sh /tmp/roll pin "" "RT_METAL_FRAMEPIN=1"
# then
#   python3 test/roll.py /tmp/roll/base.s02.tga /tmp/roll/base.s03.tga \
#       --crop 700 280 320 220
set -e
OUT="$1"; TAG="$2"; EXTRA="${3:-}"; EXTRAENV="${4:-}"
[ -n "$OUT" ] && [ -n "$TAG" ] || { echo "usage: rollbed.sh <outdir> <tag> [cfg] [env]" >&2; exit 1; }
[ -x ./darkplaces-sdl ] || { echo "rollbed: run from the repo root" >&2; exit 1; }
mkdir -p "$OUT"
SB=$(mktemp -d); mkdir -p "$SB/m5"
cat > "$SB/m5/autoexec.cfg" <<CFG
vid_borderless 1
vid_fullscreen 0
vid_width 1280
vid_height 720
vid_vsync 0
cl_maxfps 0
cl_maxidlefps 0
scr_screenshot_jpeg 0
scr_screenshot_png 0
scr_conalpha 0
con_notifytime 0
developer 0
sv_freezenonclients 1
r_volumetric 1
r_volumetric_wind "0 0 0"
r_volumetric_groundwind "0 0 0"
r_volumetric_density 0.22
r_volumetric_ground 1
r_volumetric_groundheight 48
r_volumetric_height 256
r_volumetric_corner 5
r_volumetric_scale 0.75
r_volumetric_steps 32
rt_metal 1
rt_metal_fog 1
rt_metal_walllight 0.8
rt_metal_scale 0.5
rt_metal_history 0.25
rt_metal_bluenoise 0
rt_metal_fog_history 0.7
r_viewscale 0.667
r_metalfx 1
v_gamma 1
v_contrast 1
r_brightness 0.5
r_edr 0
$EXTRA
defer 2 "map e1m3"
defer 12 "host_framerate 0.05"
CFG
i=0; t=1300
while [ $i -lt 10 ]; do
	echo "defer $(echo "$t" | awk '{printf "%.2f", $1/100}') \"screenshot\"" >> "$SB/m5/autoexec.cfg"
	t=$((t+5)); i=$((i+1))
done
echo 'defer 14.0 "quit"' >> "$SB/m5/autoexec.cfg"
env $EXTRAENV ./darkplaces-sdl -userdir "$SB" -nosound -nostartdemos > "$OUT/$TAG.log" 2>&1 || true
grep -q "using gamedirs" "$OUT/$TAG.log" || echo "rollbed: WARN no gamedirs line -- did the bed boot?" >&2
n=0
for f in "$SB"/m5/screenshots/*.tga; do
	[ -f "$f" ] || continue
	cp "$f" "$OUT/${TAG}.s$(printf '%02d' $n).tga"; n=$((n+1))
done
echo "  $TAG shots: $n"
[ "$n" -ge 4 ] || echo "rollbed: WARN only $n shots -- see $OUT/$TAG.log" >&2
rm -rf "$SB"
