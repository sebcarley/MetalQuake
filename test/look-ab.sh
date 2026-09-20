#!/bin/sh
# METAL.md Phase 5-6 acceptance: the look A/B against the configuration Seb
# actually plays. GL vs Metal, his cvar values, across the RT vantages.
#
#   sh test/look-ab.sh /tmp/look
#
# Every phase up to Phase 5 was judged against a GL run with rt_metal 0
# r_volumetric 0 r_bloom 0 -- NOT his config -- because the Metal path could not
# run rt_metal until then. 5-6 was the milestone that changed that, and 6-5 is
# the one that completes it: the volumetrics were the last part of his picture
# this script had to exclude, and Phase 6 ported them, so what is compared below
# is now his WHOLE frame rather than everything-Phase-5-owns.
#
# The acceptance row is different in kind from the per-vantage parity gate:
#
#   parity gate (tgacmp)      : do the two backends agree pixel for pixel
#   acceptance  (lookmetrics) : frame mean within 1%, diffs confined
#
# Both are reported per vantage. The parity gate is the strict one and is the
# one that has teeth; the look gate is the one that answers "would he notice".
#
# ---------------------------------------------------------------------------
# WHAT IS INJECTED, and it is his live config verbatim -- re-snapshotted
# 2026-08-09 (samples 6->4, both trace scales down a step, fog steps 24->12 +
# stride 3, volumetric steps 32->16, anisotropy back at default) -- previously
# captured 2026-08-07
# from ~/Library/Application Support/darkplaces/m5/config.cfg. Re-snapshot it
# before quoting any number from this script: CLAUDE.md records two separate
# investigations misled by a stale copy of this table.
#
# WHAT IS DELIBERATELY NOT INJECTED, with the reason, because a look A/B that
# quietly drops half the configuration is worse than none:
#
#   THE VOLUMETRICS ARE INJECTED AS OF 6-5 -- this block used to exclude
#       r_volumetric / rt_metal_fog because MODE_VOLUMETRICFOG sentinelled
#       MAGENTA on Metal, which would have compared a fogged GL frame against a
#       magenta Metal one and reported a vast difference meaning nothing. Phase
#       6 ported the murk (6-2), the fog kernel and the god rays (6-3), so the
#       scoped limit this script carried is gone and his whole picture is now in
#       the comparison. His volumetric colours are per-channel cvars and every
#       one of them is injected; the master r_volumetric 1 and rt_metal_fog 1
#       come with them.
#
#   rt_metal_shafts -- NOT injected, and NOT an omission: it is ABSENT from his
#       config, so it sits at its default of 0. His saved rt_metal_shafts_*
#       values (intensity 0.05, samples 8, scale 0.75, history 0.55) are
#       therefore inert TWICE OVER -- the master is off, and rt_metal_fog 1
#       would supersede the shafts tier even if it were on (gl_rmain.c enables
#       the shafts parm only under !rt_metal_fog). Injecting them would suggest
#       this measurement covers a path his configuration never takes. The shafts
#       arm has its own strict vantage in the parity bed (murk_shafts).
#
#   rt_metal_history 0.2, rt_metal_fog_history 0.5 (default), and the shaft twin
#       All three are pinned to 0, one reason for all of them. The bed's
#       determinism IS history 0 -- slice 5-1 measured 0 px differing across two
#       boots at history 0 against 25261 px at 0.5, because all three kernels
#       read cam.frame only under cam.history > 0; and the fog EMA additionally
#       accumulates over however many frames have been composited since the map
#       loaded, which is a load-duration-dependent and therefore
#       BACKEND-dependent count. A look number taken at his values would carry
#       that as noise and could not be told from a real difference. The temporal
#       blend is not a look knob in a frozen scene anyway: with a static camera
#       it converges to the same image.
#
#   r_volumetric_wind / _groundwind -- pinned to "0 0 0" (QUOTED; the console
#       keeps only the first token otherwise), and they are absent from his
#       config so his are the nonzero defaults. Same class as the histories:
#       cl.time reaches the density model through these two offsets and nowhere
#       else, so at his values the murk drifts and the bed cannot produce a
#       clean control. Pinning them fixes the noise PHASE without changing the
#       murk's character, which is what a still frame is measuring.
#
#   the m5_* gameplay mods
#       Server-side behaviour on a frozen server. They cannot move a pixel here
#       and would only add ways for the bed to differ.
#
#   vid_width/height 1920x1080
#       The bed renders 640x480 and the comparator's gates are ratios, so the
#       parity verdict is resolution-independent. Running his resolution would
#       cost 9x the pixels for no extra discrimination.
#
#   gl_texturecompression -- injected as 0, HIS IS 1, and this is the one
#       substitution that changes the answer rather than protecting it. Metal
#       does not implement texture compression at all: metal_textures.m has no
#       compression path, so the cvar is simply inert there. Measured, alone, on
#       rt_wall -- GL with it on against GL with it off moves 97.03% of pixels
#       (mean 1.88, max 62); Metal with it on against Metal with it off is
#       BYTE-IDENTICAL, 0 of 307200 px. Left at 1 it is the whole cross-backend
#       difference and it swamps everything Phase 5 owns. Equalised at 0, what
#       remains is a comparison of the two RENDERERS rather than of two texture
#       pipelines, which is what this milestone is for. The divergence itself is
#       recorded in METAL.md and is Phase 8's if it is ever worth doing: on
#       unified memory the VRAM saving is far less compelling than it was on
#       discrete, and Metal's uncompressed textures are the BETTER image.
#
#   developer 0 / con_notify 0 -- NOT his config either, and they are here
#       because of a second consequence of his gamma. The RT vantages set
#       developer 1 so the parity harness can grep compile lines out of the log,
#       and that output lands in the NOTIFY AREA, which never expires on a
#       frozen clock. White text stays white through any gamma ramp, so once the
#       scene is crushed to the bottom four levels the notify text is the
#       BRIGHTEST thing in the frame and it dominates the statistics: measured
#       here as two-boot controls dirty at max 210 and 226, with the block map
#       confined to the top two rows and the entire 3D scene clean. The look A/B
#       does not need the compile lines, so it turns the text off instead.
#
# TWO CVARS ARE INJECTED THAT ARE **NOT** IN HIS CONFIG, and this is deliberate:
# r_wateralpha_force 1 and r_wateralpha 0.8. Both sit at their defaults in his
# saved config (1 and 0), which means his water is OPAQUE and his saved
# rt_metal_liquids 0.45 is structurally unreachable -- it needs
# WATERALPHA|BLENDED, and opaque water is neither. He has said he wants
# transparent water, so the A/B is run at the configuration he is moving to;
# without them the liquids half of this measurement would be vacuous.
# ---------------------------------------------------------------------------
set -e
cd "$(dirname "$0")/.."
OUT="${1:-/tmp/look-ab}"
mkdir -p "$OUT"

VANTAGES="${LOOK_VANTAGES:-rt_spawn rt_wall rt_ogre rt_liquid rt_lava}"

SEB_LOOK='rt_metal_walllight 0.8
rt_metal_samples 4
rt_metal_scale 0.5
rt_metal_softness 0.38
rt_metal_darkness 0.1
rt_metal_color 1.4
rt_metal_ambient 0.1
rt_metal_liquids 0.45
rt_metal_lightcores 1
rt_metal_lavaemissive 1
rt_metal_lavalights 1
r_wateralpha_force 1
r_wateralpha 0.8
r_brightness 0.4
v_gamma 0.5
v_contrast 0.625
r_redglow 0.5
gl_texturecompression 0
r_coronas 1
r_lerpsprites 1
fov 100
viewsize 120
crosshair 5
developer 0
con_notify 0
r_volumetric 1
r_volumetric_density 0.22
r_volumetric_ground 1
r_volumetric_scale 0.5
r_volumetric_steps 16
r_volumetric_height 256
r_volumetric_corner 5
r_volumetric_watermist 0.1
r_volumetric_mistheight 10
r_volumetric_groundheight 48
r_volumetric_waterdensity 0.6
r_volumetric_color_red 0
r_volumetric_color_green 0.08
r_volumetric_color_blue 0.1
r_volumetric_groundcolor_red 0.02
r_volumetric_groundcolor_green 0.08
r_volumetric_groundcolor_blue 0.1
r_volumetric_watercolor_red 0.05
r_volumetric_watercolor_green 0.08
r_volumetric_watercolor_blue 0.08
r_volumetric_wind "0 0 0"
r_volumetric_groundwind "0 0 0"
rt_metal_fog 1
rt_metal_fog_intensity 0.5
rt_metal_fog_beams 0.5
rt_metal_fog_scale 0.375
rt_metal_fog_steps 12
rt_metal_fog_stride 3
rt_metal_fog_history 0'

echo "############ look A/B: GL vs Metal at Seb's config (WHOLE picture, 6-5)"
echo "# injected:"
printf '%s\n' "$SEB_LOOK" | sed 's/^/#   /'
echo "# NOT injected: rt_metal_shafts (ABSENT from his config, so default 0 -- and"
echo "#   rt_metal_fog 1 supersedes that tier anyway); rt_metal_history 0.2 and both"
echo "#   kernel histories (the bed's determinism IS history 0); gl_texturecompression"
echo "#   equalised at 0 (unported on Metal, and it swamps everything else);"
echo "#   r_metalfx (Metal-only by construction: injecting it would compare a GL"
echo "#   bilinear frame against an upscaled Metal one, which is not a look A/B."
echo "#   NOTE the old reason given here -- that it is inert for him because"
echo "#   r_viewscale is absent from his config -- is STALE: his config carries"
echo "#   r_viewscale 0.667, so it is live at 0.667, and r_metalfx 2 (temporal)"
echo "#   is live at ANY scale including 1. The exclusion stands on the"
echo "#   cross-backend argument alone."
echo "#   See this script's header for the full reasoning on each."
echo

PARITY_VANTAGES="$VANTAGES" PARITY_EXTRA="$SEB_LOOK" sh test/parity-4a.sh "$OUT" > "$OUT/parity.log" 2>&1 || true

FAILED=0
for v in $VANTAGES; do
	echo "============ $v"
	if [ ! -f "$OUT/w_${v}_gl_a.tga" ] || [ ! -f "$OUT/w_${v}_mt_a.tga" ]; then
		echo "  (missing capture -- see $OUT/w_${v}_*.log)"; FAILED=1; continue
	fi
	echo "--- CONTROLS (must be clean before anything below is read)"
	printf '  gl    '; python3 test/tgacmp.py "$OUT/w_${v}_gl_a.tga" "$OUT/w_${v}_gl_b.tga" \
		| grep -aE 'mean channel|max delta' | sed 's/.*: *//' | tr '\n' '/'; echo
	printf '  metal '; python3 test/tgacmp.py "$OUT/w_${v}_mt_a.tga" "$OUT/w_${v}_mt_b.tga" \
		| grep -aE 'mean channel|max delta' | sed 's/.*: *//' | tr '\n' '/'; echo
	echo "--- PARITY GATE (pixels)"
	python3 test/tgacmp.py --gate world "$OUT/w_${v}_gl_a.tga" "$OUT/w_${v}_mt_a.tga" | sed 's/^/  /' || FAILED=1
	echo "--- LOOK ACCEPTANCE (frame mean within 1%)"
	python3 test/lookmetrics.py "$OUT/w_${v}_gl_a.tga" "$OUT/w_${v}_mt_a.tga" --labels gl,metal || FAILED=1
	echo
done
echo "############ look A/B done (FAILED=$FAILED)"
exit $FAILED
