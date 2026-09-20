#!/bin/sh
# METAL.md Phase 4a/4b/4c acceptance: the world, its entities and its effects,
# GL vs Metal, frozen scene. Phase 5 added the RT vantages and Phase 6 the murk.
#
# THE BED WAS BLIND TO THE FORK'S OWN SIGNATURE PASS until 6-3b, and it is worth
# stating plainly at the top because it is the shape 5-6 found too: a bed made of
# feature vantages tests the features somebody thought to name. Twenty-nine
# vantages across five phases and not one of them turned r_volumetric on. Note
# the mechanism, because an earlier note in this tree got it wrong: only the rt_*
# arm ever emitted `r_volumetric 0`. The other vantages were murk-free because
# every boot takes a fresh userdir and the cvar DEFAULTS to 0 -- which is the
# weaker of the two, since a default can move under a bed and a pin cannot.
#
# The bed is the sv_freezenonclients recipe -- it stops sv.time itself, so
# cl.time converges and texture animation, waterscroll and lightstyles all
# freeze; this is what makes a MAP bed pinnable at all (the Phase 3 HUD bed
# failure). Camera via the committed cmdtrace pattern: noclip + edictset.
# Same-binary two-boot controls run per backend per vantage, FIRST.
# r_vis_trace must never be enabled here (wall-clock-hysteretic visibility).
#
# THE BED WAS DARK FROM THE F7 MERGE (2026-08-12) TO 2026-08-16, and the way it
# went dark is the shape to watch for. Nobody re-ran the sweep after the merge;
# every r_volumetric-0 vantage then failed its cross at ~90% of pixels with clean
# controls (spawn 4.01, rt_wall 3.69), and the murk vantages -- which cross
# clean -- said nothing. The cause was a BAKE-TIMING ASYMMETRY: the heat
# shimmer resolved the baked world field lazily inside the frame on Metal only
# (vid.m5postfx), and an in-frame bake shifts the whole scene ~1 px for the
# boot ON EITHER BACKEND (measured on GL too, 94.6% at mean 3.97 when GL's own
# bake moves). The murk vantages were clean only because BOTH backends baked
# in-frame there. R_Volumetric_ResolveField() (gl_rmain.c) now bakes between
# frames for every consumer on both backends. Two consequences for this file:
# (1) after ANY merge, run the full sweep -- a feature branch's own gates
# cannot see a cross-backend asymmetry it introduces; (2) a feature that is
# METAL-ONLY BY CAPABILITY (vid.blendequationmax, vid.m5postfx) must be PINNED
# OFF on the vantages where it moves pixels, so the bed compares like with
# like -- beam/beam_lit pin the SDF bolt, rt_lava/lavaboil pin the shimmer;
# their Metal-side liveness is smoke's (runs N and O). The shimmer stays LIVE
# on every e1m3 vantage on purpose: it moves no pixel there (no lava), so it
# keeps the bed sensitive to exactly the bake-timing class that broke it.
#
# PARITY_VANTAGES selects what runs (2026-08-21 rationalisation):
#   (unset)                    -> the CORE set: one vantage per subsystem, the
#                                 everyday gate (~a quarter of the full sweep)
#   PARITY_VANTAGES=full       -> every vantage. REQUIRED after any merge (the
#                                 standing rule above) and before any claim
#                                 about a subsystem the core set does not carry
#   PARITY_VANTAGES="flipped"  -> exactly the named vantages
# spawn and hall emit the 4a bed byte for byte -- they are the regression
# vantages and their cfg text must never change.
#
# READING A DIRTY CONTROL, and this is the part that was being done by feel
# until the 4d-5 sweep. A dirty control invalidates the cross number under it,
# but it does NOT by itself mean the renderer moved -- and the counters already
# captured here tell you which it is, for free:
#
#   dirty control + dump-1 counters IDENTICAL between the two boots
#       -> the same geometry was drawn twice and came out different.
#          That is a real rendering difference. Investigate it.
#   dirty control + dump-1 counters DIFFERING between the two boots
#       -> the two boots drew different visible sets, so the camera or the
#          screenshot landed at a different moment. A BED artefact.
#
# Measured on the 4d-5 sweep: sky's odd Metal boot reported draws 77 /
# vertices 3229 / batches 22 against the good boot's 53 / 2962 / 20 -- a
# different camera -- and the good boot matched GL at 0.0001. viewmodel showed
# the same shape. Both re-ran clean. A second tell for that case: the CROSS
# statistics come out exactly equal to the dirty control's, because the odd
# boot is the outlier in both comparisons.
#
# A CLEAN CONTROL IS NOT PROOF THAT THE RUN IS REPRESENTATIVE. Both boots of one
# backend can go odd TOGETHER -- measured once on rt_lava, where the control was
# 0.0000 and the cross was 4.6855 against a normal 0.0010, and which has not
# reproduced since. The counters rule above cannot see that case either, because
# the geometry really was the same. The discriminator that does work: re-run the
# vantage and compare each backend against ITSELF across the two runs. The stable
# side is right and the moving side is the outlier.
#
# CAMERA DELIVERY ON THE FROZEN BED, measured 2026-08-07 and sharper than the
# per-vantage notes below. ORIGIN applies reliably (the eye lands where asked,
# confirmed by r_volumetric_probe on six boots). PITCH applies, divided -- the
# player-angles convention, hence the ask-3x rule. YAW DOES NOT APPLY AT ALL:
# four boots at the same origin requesting yaw 0, 90, 180 and 270 came out
# BYTE-IDENTICAL to each other. The client owns its own yaw and the server's
# entity angles do not overwrite it, so a vantage's yaw request is decoration --
# what it actually gets is the map's spawn yaw. That does not invalidate any
# vantage (both backends get the same camera, which is all parity needs), but it
# does mean a frozen vantage cannot be AIMED by yaw: to frame something specific,
# move the ORIGIN until the default heading shows it.
#
# NEVER take more than one screenshot per boot to test for animation. The
# screenshot's own "Wrote <name>.tga" console line lands in the notify area, so
# shot N+1 captures shot N's message and the frames differ in the top-left for a
# reason that has nothing to do with the scene. That artefact was briefly
# mistaken for e1m7 failing to freeze; one shot per boot shows 16.00, 16.05 and
# 16.15 seconds byte-identical.
#
# Two vantages never produce a zero control, by construction rather than by
# accident, and this is not a defect to chase:
#   water -- the only UNFROZEN vantage (see the long note below). Its GL
#     control runs ~0.03 and its cross ~0.15, both inside the world gate.
#   (flash used to be a third; 4d-5 measured the cause and fixed it -- see
#     that vantage's own note.)
set -e
cd "$(dirname "$0")/.."
OUT="$1"; [ -n "$OUT" ] || OUT=/tmp
mkdir -p "$OUT"
CMP="test/tgacmp.py"
# Every vantage, for PARITY_VANTAGES=full and the after-merge rule.
FULL_VANTAGES="spawn hall flipped ents_spawn ents_hall ogre viewmodel colormap diffuse sky water points beam beam_lit flash sprite fog fbo1 fbo2 fbo3 bloom fxaa tint texfilter texfiltermag rt_spawn rt_wall rt_ogre rt_lava rt_liquid rt_sun murk murk_water murk_kernel murk_shafts murk_sky murk_liquid lavaboil"
# The core set: one vantage per subsystem, chosen so every ported ARM the fork
# leans on daily is exercised once -- world (spawn + the hall outlier camera),
# entities (ents_spawn), the weapon in hand (viewmodel), the offscreen frame
# tail (fbo2) and the bloom chain's unique blend (bloom), the RT composite at
# defaults and at Seb's walllight (rt_spawn, rt_wall), and the murk's two
# arms (murk_kernel byte-identity, murk_shafts the GL-march tier). Everything
# else -- the permutation isolates, the effects layer, the e1m7 pair, the
# texfilter controls -- lives in the full sweep.
CORE_VANTAGES="spawn hall ents_spawn viewmodel fbo2 bloom rt_spawn rt_wall murk_kernel murk_shafts murk_liquid"
case "${PARITY_VANTAGES:-}" in
	"")   VANTAGES="$CORE_VANTAGES" ;;
	full) VANTAGES="$FULL_VANTAGES" ;;
	*)    VANTAGES="$PARITY_VANTAGES" ;;
esac

writecfg() { # backend vantage tag
	# The Metal boot's restart + shader-compile stall can compress the wall
	# clock so that several defers fire in ONE resumed cbuf burst -- and a
	# screenshot in the same burst as a state change captures the PREVIOUS
	# frame (screenshots always do). Vantages whose state lands late (fog)
	# push the shot/quit times out instead of trusting the spacing.
	SHOT_T=12; QUIT_T=14
	GAMEARGS=""
	case "$2" in
	fog) SHOT_T=16; QUIT_T=18 ;;
	# rt_sun (SEPTEMBER2 D, 2026-09-06): the harness's first ARCANE DIMENSIONS
	# vantage -- AD's start hub under its own authored sun. AD loads slowest of
	# all (a 400 ms field bake, a 400k-triangle BLAS), so it takes the lava
	# vantage's allowance and a margin; the gamedir rides the boot line.
	rt_sun) SHOT_T=40; QUIT_T=42; GAMEARGS="-game m5 -game ad" ;;
	# The RT vantages push out for the same reason and one more of their own:
	# the composite is ASYNC and shows the PREVIOUS frame's trace, so the
	# camera must have been settled for at least a frame before the shot.
	rt_*) SHOT_T=16; QUIT_T=18 ;;
	# The two kernel-driven murk vantages ride the SAME async trace, so they
	# inherit the RT settling allowance rather than the murk's own 12 s. The
	# GL-march murk vantages do not: nothing about them is async.
	#
	# murk_kernel SHOWS THE DOCUMENTED CAMERA-DELIVERY FLAKE, roughly one boot
	# in six, and it is NOT a settling problem -- which is worth writing down
	# because the obvious fix was tried and is wrong. The symptom is a dirty
	# CONTROL (max 164-219) while that vantage's own CROSS stays byte-identical
	# at 0.0000 / max 0, on either backend. Raising this pin to 22 did not help:
	# the flake simply moved from the Metal control to the GL one at the same
	# rate, so it was reverted rather than left as an unjustified number.
	#
	# The counters settle it, exactly as this file's header prescribes: the two
	# boots reported draws 89 / vertices 1173 / batches 18 against 110 / 1435 /
	# 19, so they DREW DIFFERENT VISIBLE SETS -- a bed artefact, the camera or
	# the moment, not the renderer. Both boots took kernel-ACTIVE. Same class as
	# the sky and viewmodel outliers of the 4d-5 sweep. Re-run; the cross has
	# been byte-identical on every run measured.
	murk_kernel|murk_shafts|murk_sky) SHOT_T=16; QUIT_T=18 ;;
	# murk_liquid marches on the GL/MSL surface arm -- nothing async -- but it
	# loads e1m2, which bakes a 49x54x20 field, so it keeps the murk allowance.
	murk_liquid) SHOT_T=16; QUIT_T=18 ;;
	esac
	case "$2" in
	# rt_lava needs LONGER STILL since 5-5-3, and the reason is worth stating
	# because it looked exactly like a renderer defect for an hour. Restoring
	# the liquid supercontents (m5_liquidflags) brought rt_metal_lavaemissive
	# and rt_metal_lavalights back to life -- a second static TLAS instance and
	# 8 more lights, on a path that had never executed on the Metal renderpath
	# at all -- so the async trace takes longer to settle and a shot at 16 s
	# landed while the composite was still showing an older one. The symptom
	# was a REPRODUCIBLE dirty Metal control at mean 3.6 / max 221, whole-frame
	# and uniform, with dump-1 counters IDENTICAL between the two boots, which
	# by the header's rule reads as a real rendering difference. It was not.
	# What settled it: m5_liquidflags 0 restored a clean control and the exact
	# recorded 0.0010 / max 1, so the lava path was implicated rather than the
	# port; then SHOT_T 26 with lava LIVE reproduced 0.0010 / max 1 too. Note
	# the counters rule cannot see this class -- the geometry really is the
	# same, only the trace's age differs.
	# 34, not 26, since the 5-6 look A/B: that run drives the trace at Seb's
	# rt_metal_scale 0.75 and samples 6 rather than the vantage's 0.5/default,
	# and at 26 the Metal control went dirty again in the same signature (1.2487
	# / max 157, cross exactly equal to it). Settle time here scales with how
	# much work the trace is actually doing, so the pin has to clear the
	# heaviest configuration the vantage is ever run at, not the default one.
	rt_lava) SHOT_T=34; QUIT_T=36 ;;
	# e1m7 without the RT trace: no async settling to wait for, but the map is
	# the slowest to load in the bed, so it keeps a margin over the default.
	lavaboil) SHOT_T=16; QUIT_T=18 ;;
	esac
	{
		echo 'scr_screenshot_jpeg 0'
		echo 'scr_screenshot_png 0'
		echo 'showfps 0'; echo 'showtime 0'; echo 'showdate 0'; echo 'showbrand 0'
		echo 'cl_nettimesyncfactor 1'
		echo 'cl_nettimesyncboundmode 1'
		echo 'sv_random_seed 1'
		# PINS, not defaults (a default can move under a bed; a pin cannot):
		# every recorded baseline was captured at async RT and vsync off.
		# Phase 8-6 flipped both DEFAULTS on after QA; the pins keep all
		# thirty-five vantages' recorded numbers valid, and same-frame keeps
		# its own coverage via PARITY_EXTRA='rt_metal_sameframe 1' (8-2).
		echo 'rt_metal_sameframe 0'
		echo 'vid_vsync 0'
		# rt_metal_lightsample 0, PINNED (2026-08-28, the FOGLIGHT default flip).
		# The fog-only stochastic pick defaults 1 now and moves ~95% of a murk
		# frame; every recorded murk baseline was captured at mode 0. The 8-6
		# vsync/sameframe shape verbatim: pin the old value here, and the mode-1
		# coverage is PARITY_EXTRA='rt_metal_lightsample 1' (measured 2026-08-28:
		# murk_kernel crosses 16 px @ max 1 at mode 1 -- as tight as mode 0).
		echo 'rt_metal_lightsample 0'
		# rt_metal_term_upsample 0, PINNED (2026-08-28, WARCHEST session 1's
		# follow-up catch). The term upsample engages wherever the scene
		# renders offscreen with a sampleable depth -- and at PLAIN DEFAULTS
		# that is Metal only, because the M5 bolt's asker
		# (vid.blendequationmax) forces the offscreen path there and cannot
		# on GL: the documented Metal-only-by-capability class, which moved
		# the rt vantages' Metal frames by the feature's own edge sharpening
		# (rt_ogre 2960 px @ max 122) while GL stayed byte-identical. Pin the
		# old value, keep every recorded baseline; feature-on coverage is
		# PARITY_EXTRA='rt_metal_term_upsample 1' with 'r_viewfbo 2' so both
		# backends engage it together (measured 2026-08-28: crosses 7-27 px).
		echo 'rt_metal_term_upsample 0'
		# rt_metal_fog_stride_adaptive 0, PINNED (2026-08-28, WARCHEST session
		# 3): the adaptive cast schedule changes deep-fog pixels on BOTH
		# backends identically (one shared sidecar), so this pin preserves the
		# recorded murk baselines rather than guarding an asymmetry; coverage
		# is PARITY_EXTRA='rt_metal_fog_stride_adaptive 1'.
		echo 'rt_metal_fog_stride_adaptive 0'
		# rt_metal_gi 0, PINNED from the day the cvar exists (GIARC G1,
		# 2026-08-29 — the term-upsample lesson applied in advance: a feature
		# left to its DEFAULT can engage asymmetrically under a bed the day
		# some capability asker moves, and a pin cannot). G3 (same day)
		# flipped the DEFAULT to 1 on Seb's eye pass — which is exactly the
		# case this pin pre-paid: every recorded rt/murk baseline was captured
		# at gi 0 and stays valid. Feature-on coverage is
		# PARITY_EXTRA='rt_metal_gi 1' (both backends move together — one
		# shared sidecar; measured at G1: crosses 0.0005–0.0009 / max 1), and
		# the emissive bounce adds 'rt_metal_gi_emissive 1' on the e1m7
		# vantages (its instances are degenerate on e1m3).
		echo 'rt_metal_gi 0'
		# rt_metal_sun 0, PINNED from the day the cvar exists (SEPTEMBER2 D,
		# 2026-09-06), the gi pin's shape: no id1 map declares a sun and the
		# default is 0, so this is a no-op today and a guard against the
		# default ever moving. rt_sun's own block overrides it.
		echo 'rt_metal_sun 0'
		# r_volumetric_liquidfade 0, PINNED FROM THE DAY THE CVAR EXISTS
		# (LIQUIDFOG, 2026-08-30 -- the term-upsample lesson applied in advance:
		# a feature left to its DEFAULT can start engaging under a bed the day
		# some predicate moves, and a pin cannot). It is a STATIC PARM, so at 0
		# the shader text is not merely branched past but absent, and every
		# recorded baseline here stays valid byte for byte. Feature-on coverage
		# is PARITY_EXTRA='r_volumetric_liquidfade 1' on murk_liquid, where the
		# two arms are measured to move IDENTICALLY -- 115515 px, mean 2.1781
		# on each backend (2026-08-31, at Seb's own murk values) -- which is the
		# whole proof that the GLSL and MSL splices of DPD_LIQUID_FADE have not
		# drifted. There is no shared sidecar under the surface shader doing the
		# work for both, so that agreement is earned rather than structural.
		echo 'r_volumetric_liquidfade 0'
		# rt_metal_liquids_minlight 0, PINNED the day its DEFAULT moved off 0
		# (2026-08-31). Unlike the pins above it this one is NOT structural --
		# it is a plain uniform, and it only reaches a pixel where
		# rt_metal_liquids is above 0, which is exactly ONE vantage (rt_liquid,
		# at Seb's 0.45). Its recorded cross of 0.0015 / max 1 was captured with
		# the term unclamped, so without this the default flip would silently
		# re-baseline that vantage rather than being measured against it.
		# Feature-on coverage is PARITY_EXTRA='rt_metal_liquids_minlight 5'.
		echo 'rt_metal_liquids_minlight 0'
		# rt_metal_lightcap 0, PINNED the day its DEFAULT moved off 0 (2026-09-02,
		# SEPTEMBER S2). A per-light knee at the RT export: at 0.75 every e1m3
		# light above `light 192` is trimmed a little (200 -> 0.77, 300 -> 0.93,
		# 400-500 more), so every recorded rt_* baseline -- all captured with no
		# cap -- would silently re-baseline. Pin the old value; feature-on
		# coverage is PARITY_EXTRA='rt_metal_lightcap 0.75' (one shared sidecar,
		# so both backends move together).
		echo 'rt_metal_lightcap 0'
		# rt_metal_shadowlights 1, PINNED the day its DEFAULT moved to 3 (2026-09-19,
		# THE ROUND SPOTLIGHTS). Past 1 the kernel shadow-tests the next brightest
		# lights as well as the dominant, which removes light that every recorded rt_*
		# baseline was captured WITH -- e1m3's rt_wall camera moves 8.9%% of its term at
		# 3. Wall-lighting arm only, so the walllight-0 vantages are inert either way.
		# Feature-on coverage is PARITY_EXTRA='rt_metal_shadowlights 3' (one shared
		# sidecar, so both backends move together).
		echo 'rt_metal_shadowlights 1'
		# rt_metal_fog_froxel 0, PINNED the day its DEFAULT moved to 1 (2026-09-07,
		# Seb's eye): the froxel is a different fog integral (48 fixed cells, a
		# per-cell history), so murk_kernel / murk_shafts would re-baseline.
		# Feature-on coverage is PARITY_EXTRA='rt_metal_fog_froxel 1'.
		echo 'rt_metal_fog_froxel 0'
		# r_volumetric_liquidfloor 0, PINNED the day it shipped at 1 (2026-09-07): it moves
		# the air murk over and beside every liquid, so the murk baselines (e1m3's moat is
		# in frame) would re-baseline. Coverage is PARITY_EXTRA='r_volumetric_liquidfloor 1'.
		echo 'r_volumetric_liquidfloor 0'
		# r_watersurface 0, PINNED the day it shipped at 1 (2026-09-13, Seb's eye): on the
		# liquid vantages it draws the pool interior (the below-surface PVS merge) and
		# refracts it, so rt_liquid and murk_liquid would re-baseline. Feature-on
		# coverage is PARITY_EXTRA='r_watersurface 1' (measured 2026-09-13: both arms move
		# together to the pixel, 38.10% at 4.0191 on each backend at murk_liquid).
		echo 'r_watersurface 0'
		# rt_metal_fog_residual 0.25, PINNED the day its DEFAULT moved to 0.1 (2026-09-06
		# evening, Seb's "sharp1" verdict): the fill is a term in the fog kernel's light
		# sum, so every recorded murk_kernel / murk_shafts baseline was captured at 0.25.
		# The lightcap shape: pin the old value, cover the new one via PARITY_EXTRA.
		echo 'rt_metal_fog_residual 0.25'
		# the ambient grid's 2026-08-12 calibration, PINNED the day its defaults
		# moved (2026-09-19: mean dilate, gain 1, 64-unit cells). Every recorded
		# murk baseline was captured with the brightest-neighbour fill at gain
		# 1.5 on 128-unit cells. Coverage of the new bake is
		# PARITY_EXTRA='r_volumetric_ambientdilate 1' (+ gain/irrcell as wanted).
		echo 'r_volumetric_ambientdilate 0'
		echo 'r_volumetric_ambientgain 1.5'
		echo 'r_volumetric_irrcell 128'
		# rt_metal_bluenoise 0, PINNED (2026-09-03, BLUENOISE slice 1). Every
		# frozen vantage renders at history 0, and at history 0 the blue arm
		# reads slice 0 of whatever table is compiled in -- so REGENERATING the
		# table (which the STBN session did, replacing 8 independent slices
		# with a 64x64x16 spatiotemporal volume, and which any future
		# regeneration will do again) silently moves every recorded rt and
		# murk cross on both backends at once, the shared-sidecar signature.
		# The IGN arm is shader TEXT and can never be regenerated under the
		# bed, so pinning it is what makes the crosses stable; blue-arm
		# coverage is PARITY_EXTRA='rt_metal_bluenoise 1'. Crosses recorded
		# before this date were taken on the OLD table's slice 0 and are
		# re-recorded at the pin in the 2026-09-03 CLAUDE.md session record.
		echo 'rt_metal_bluenoise 0'
		# r_metalfx 0, PINNED. MetalFX is Metal-only by capability and moves every
		# pixel; every recorded baseline was captured with it off. Until 2026-08-18
		# the bed relied on the cvar's DEFAULT for that, and this file's own header
		# says why that is the weaker guard: a default can move under a bed and a
		# pin cannot. If the default ever flips, an unpinned bed silently starts
		# comparing a GL bilinear frame against a Metal-upscaled one and reports it
		# as a renderer divergence. Temporal is a THREE-state cvar now (0/1/2), so
		# the pin says 0, not 'off'.
		echo 'r_metalfx 0'
		# The weapon feel, PINNED at 0 the day four of its cvars went to DEFAULT 1
		# (2026-09-16, Seb: "feel_on is good. make that default"). QuakeC, not the
		# renderer, but three vantages fire a weapon (flash, beam, beam_lit) and
		# the kick moves the view, so every recorded baseline -- all taken with the
		# stock guns -- would otherwise re-baseline. At 0 each hook writes id's own
		# values, so the pin is the old behaviour by construction. The grenade
		# bounce defaults 0 and fires nothing a vantage can see.
		echo 'm5_kick 0'
		echo 'm5_nailtracer 0'
		echo 'm5_nailbarrels 0'
		echo 'm5_axesparks 0'
		# The drawn muzzle flash, PINNED at 0 the day it went to DEFAULT 1
		# (2026-09-16). It is a particle over the view weapon, so the preamble's
		# cl_particles 0 already keeps it out of every frame; the pin makes that
		# a statement rather than a consequence of another pin.
		echo 'm5_muzzleflash 0'
		# THE BEAUTY PARTICLE PACK, PINNED at 0 the day it went to DEFAULT ON
		# (2026-09-17, Seb's eye on the whole round). The preamble's
		# cl_particles 0 below keeps most of it out of every frame, but the
		# `points` vantage turns particles back ON to draw the pointfile's
		# billboards, and TWO of these are not particle-gated at all:
		# cl_particles_soft and cl_particles_refract are STATIC PARMS on
		# MODE_GENERIC, so a non-zero default compiles a different shader into
		# every 2D and particle batch on every vantage. cl_particles_texsize
		# additionally moves the atlas AND the rand() stream (the 256
		# generation draws more numbers -- see BEAUTY A2), so a bed that let it
		# drift could not compare two binaries at all.
		echo 'cl_particles_texsize 64'
		echo 'cl_particles_blood_droplet 0'
		echo 'cl_particles_soft 0'
		echo 'cl_particles_refract 0'
		echo 'cl_particles_scorchglow 0'
		# THE BEAUTY LIGHT PACK, PINNED at 0 the day it went to DEFAULT ON
		# (2026-09-17). rt_metal_gi_ao is already inert under the rt_metal_gi 0
		# pin above -- it darkens the ambient fill only where the bounce ray
		# runs -- and is pinned anyway so the bed STATES it rather than
		# inheriting it from a neighbouring pin. The other two are not covered
		# by anything: rt_metal_fog_liquidlight changes the fog kernel's
		# in-liquid arithmetic and would re-baseline `murk_water`, and
		# rt_metal_contact changes the trace kernel's penumbra wherever a
		# blocker stands near its receiver, which is every rt_* vantage.
		echo 'rt_metal_gi_ao 0'
		echo 'rt_metal_fog_liquidlight 0'
		echo 'rt_metal_contact 0'
		# THE BEAUTY ATMOSPHERE PACK, PINNED at 0 the day it went to DEFAULT ON
		# (2026-09-17). r_skylightning is the one pin in this whole preamble
		# that defends DETERMINISM rather than a baseline: its schedule is a
		# random interval on cl.time, so a flash can land inside a capture on
		# one boot and not the next, and it drives the sun AND pushes a light
		# -- a two-boot control could not come back clean. m5_torch_embers
		# spawns particles and fires a traceline per ember from a camera the
		# bed has frozen; r_caustics moves every world surface that lies under
		# a liquid, which is 38% of the `murk_liquid` frame by its own
		# isolation. Feature coverage for all three is PARITY_EXTRA.
		echo 'r_skylightning 0'
		echo 'm5_torch_embers 0'
		echo 'r_caustics 0'
		echo 'r_drawentities 0'
		echo 'r_drawviewmodel 0'
		echo 'r_drawdecals 0'
		echo 'cl_particles 0'
		# NOT r_q1bsp_lightmap_updates_enabled 0: q1bsp lightmaps are BUILT by
		# that very update path at load, so pinning it from boot leaves them
		# black -- measured, mean luminance 9.5 -> 1.1. With sv.time frozen the
		# lightstyles are constant, so there is nothing mid-run to pin anyway.
		echo 'viewsize 100'
		echo 'crosshair 0'
		# freeze BEFORE the map loads: sv.time then pins at its spawn value on
		# every boot, so the lightstyle phase -- floor(cl.time*10), quantised --
		# is DETERMINISTIC instead of load-duration-dependent. The deferred
		# freeze-after-load variant left one boot per four a lightstyle tick
		# adrift, and the two dirty controls it produced had byte-identical
		# statistics: the same two style frames differencing, wherever they land.
		#
		# EXCEPTION (4c, measured 2026-08-06): the pre-load freeze SUPPRESSES
		# the r_wateralpha/r_wateralpha_force path outright -- water renders
		# opaque with the cvars provably live, draw counters unchanged;
		# mechanism unidentified (late cvar-setting does not rescue it). A
		# deferred freeze preserves the alpha but pins cl.time at a
		# load-duration-dependent value, and Metal's slower restart pins one
		# lightstyle tick apart from GL -- a whole-frame +-1 wash that fails
		# the cross gate as a bed artefact. The water vantage therefore runs
		# UNFROZEN with the frame made time-immune instead: r_fullbright 1
		# (no lightstyles), r_waterscroll 0 (no scroll phase), entities and
		# particles already off. Water resolves to mode 11 under fullbright;
		# the blend path through the transparent queue is what is under test.
		case "$2" in
		water) ;;
		*) echo 'sv_freezenonclients 1' ;;
		esac
		# Per-vantage bed extras. spawn and hall emit NOTHING here, so their
		# cfg text -- the 4a acceptance bed -- stays byte-identical.
		#
		# Frozen-bed expectation for the entity vantages: monsters hold their
		# spawn poses and may hover un-dropped (droptofloor thinks never fire
		# under the pre-load freeze) -- identically on both backends, so the
		# bed is valid. r_cullentities_trace must be 0 on every entity
		# vantage: entity visibility otherwise depends on host.realtime.
		# developer 1 surfaces the per-permutation Metal compile lines for
		# the log greps. Notify text never expires on a frozen clock and
		# con_notifytime 0 does NOT hide it (measured); the sv_cheats/noclip
		# lines therefore sit in every frame -- identically on both backends,
		# so they cancel in the diff, exactly as on the 4a vantages.
		case "$2" in
		flipped)
			# v_flipped on BOTH backends: a pure world handedness check
			# (entities stay off, so it is valid independent of the entity
			# shader arm). A winding or cull-shadow error mirrors or
			# inside-outs the whole frame -- far outside the world gate.
			echo 'v_flipped 1' ;;
		ents_spawn|ents_hall|ogre)
			echo 'r_drawentities 1'
			echo 'r_cullentities_trace 0'
			echo 'developer 1'
			echo 'con_notifytime 0' ;;
		viewmodel)
			# The weapon in hand: SHORTDEPTHRANGE -> GL_DepthRange(0,0.0625),
			# the viewport-z path on Metal. The sway/bob pins are
			# belt-and-braces (a frozen clock stops both anyway).
			#
			# r_drawentities MUST be on (2026-08-21): the preamble's
			# r_drawentities 0 hides the viewmodel too (cl.viewent rides the
			# same entity list), so from 4b to 2026-08-21 this vantage
			# compared two weaponless frames -- byte-clean, and VACUOUS. It
			# passed every sweep while proving nothing about the thing it is
			# named for. Fixed and measured (2026-08-21): with entities on,
			# hiding the weapon again (PARITY_EXTRA='r_drawviewmodel 0')
			# moves 5149 px on this camera, so the weapon is demonstrably in
			# frame; both two-boot controls read 0 of 307200 px and the cross
			# 0.0005 / max 1. Monsters enter the frame too (frozen spawn
			# poses, deterministic -- the ents vantages' own bed rules),
			# which is fine: the cross compares like with like.
			echo 'r_drawentities 1'
			echo 'r_cullentities_trace 0'
			echo 'r_drawviewmodel 1'
			echo 'v_idlesway 0'
			echo 'cl_bob 0'
			echo 'cl_bobmodel 0' ;;
		colormap)
			# Third-person player model with explicit team colours:
			# USECOLORMAPPING (pants/shirt) on the one model that has them.
			echo 'chase_active 1'
			echo 'color 4 12'
			echo 'r_drawentities 1'
			echo 'r_cullentities_trace 0'
			echo 'developer 1'
			echo 'con_notifytime 0' ;;
		diffuse)
			# r_fullbright_directed forces RENDER_LIGHT|RENDER_DYNAMICMODELLIGHT
			# onto every model entity INCLUDING the world, so the whole frame
			# becomes mode 11 with USEDIFFUSE -- the one stock-cvar bed that
			# exercises the directional arm. Direction rides cl.viewangles,
			# pinned by fixangle. SYNTHETIC LOOK: valid as a parity instrument,
			# not as a look reference.
			echo 'r_fullbright 1'
			echo 'r_fullbright_directed 1'
			echo 'r_drawentities 1'
			echo 'r_cullentities_trace 0'
			echo 'developer 1'
			echo 'con_notifytime 0' ;;
		water)
			# Alpha-blended water through the transparent queue. UNFROZEN --
			# see the note above; fullbright + scroll-off make the frame
			# time-immune instead.
			echo 'r_fullbright 1'
			echo 'r_waterscroll 0'
			echo 'r_wateralpha_force 1'
			echo 'r_wateralpha 0.8' ;;
		points)
			# pointfile: CL_ReadPointFile_f spawns static billboard particles
			# (no velocity, no jitter, no rand consumed) + three additive axis
			# beams from maps/e1m3.pts, which shot() writes transiently. The
			# richest deterministic particle scene available frozen.
			echo 'cl_particles 1' ;;
		beam)
			# The permanent thunderbolt: one lightning shot, endtime frozen.
			# r_shadow_realtime_dlight 0 isolates BEAM GEOMETRY from the
			# muzzle-flash rtlight (LIGHTSOURCE is 4c-2's item, not 4c-1's).
			# r_lightningbeam_m5_sdf 0 since 2026-08-16: F1's capsule-SDF bolt
			# is METAL-ONLY BY CAPABILITY (vid.blendequationmax -- GL never asks,
			# by design), so at the default GL draws the additive ribbon and
			# Metal the SDF bolt and the cross reads 2.81 / max 255 / 3.79% >8
			# with clean controls. Pinned off, both draw the ribbon and the
			# vantage returns to its recorded 0.0006 / max 1. The SDF bolt's
			# Metal-side liveness is smoke run N's.
			echo 'cl_particles 1'
			echo 'r_shadow_realtime_dlight 0'
			echo 'r_lightningbeam_m5_sdf 0'
			echo 'developer 1'
			echo 'con_notifytime 0' ;;
		beam_lit)
			# The beam vantage WITHOUT the dlight pin: the bolt plus its
			# permanent muzzle-flash rtlight -- LIGHTSOURCE additive passes
			# (mode 12) from a second light source, over the beam.
			# dlight SHADOWS are pinned off on the light vantages: GL runs
			# ~25 draws of stencil shadow-volume work per light that Metal
			# structurally lacks (measured: pixel-invisible here, but it
			# blunts the counter instrument; divergence recorded in METAL.md).
			# r_lightningbeam_m5_sdf 0: see beam.
			echo 'cl_particles 1'
			echo 'r_shadow_realtime_dlight_shadows 0'
			echo 'r_lightningbeam_m5_sdf 0'
			echo 'developer 1'
			echo 'con_notifytime 0' ;;
		flash)
			# One shotgun shot from the spawn camera: the muzzle-flash
			# rtlight never decays on the frozen clock, so every frame after
			# carries mode-12 additive passes over the lit world, plus the gun
			# in hand. Shadow pin: see beam_lit.
			#
			# PARTICLES OFF since 4d-5, and measured rather than assumed. The
			# shot's impact puffs are boot-unstable on BOTH backends -- with
			# cl_particles 1 the GL two-boot control alone came out at mean
			# 0.0006 / max 23 / 41 px and the Metal one at 0.1031 / max 255 /
			# 317 px, which by this harness's own protocol makes the cross
			# number unreadable. All four boots carried IDENTICAL dump-1
			# counters (draws 62, vertices 1507, lights 1, batches 18), so it
			# was never a camera or visibility artefact; the differing pixels
			# sit in one centre-frame cluster. Turning particles off makes the
			# GL control 0.0000 / max 0 and the cross 0.0008 / max 2 -- so the
			# muzzle flash, the rtlight pass and the viewmodel are byte-clean
			# and the puffs were the whole of it. Nothing is lost: `points` is
			# the particle vantage, and it is deterministic by construction
			# (pointfile spawns static billboards that consume no randomness).
			# Same class as the axis beams `points` had to bury.
			echo 'r_drawviewmodel 1'
			echo 'v_idlesway 0'
			echo 'cl_bob 0'
			echo 'cl_bobmodel 0'
			echo 'cl_particles 0'
			echo 'r_shadow_realtime_dlight_shadows 0'
			echo 'developer 1'
			echo 'con_notifytime 0' ;;
		sprite)
			# e4m7's light_globe (s_light.spr) -- the only static sprite in
			# id1. Sprites resolve to mode 11 via the null-lightmap route and
			# draw from the transparent queue.
			echo 'r_drawentities 1'
			echo 'r_cullentities_trace 0'
			echo 'developer 1'
			echo 'con_notifytime 0' ;;
		fog)
			# Classic fog via the `fog` console command (applies instantly on
			# the frozen clock -- measured: 98.7% of the frame moves). The
			# whole world takes USEFOGINSIDE/OUTSIDE permutations; entities
			# on so mode 11 fogs too.
			echo 'r_drawentities 1'
			echo 'r_cullentities_trace 0'
			echo 'developer 1'
			echo 'con_notifytime 0' ;;
		# METAL.md Phase 4d -- the three fbo* vantages are the OFFSCREEN SCENE
		# PATH. Every vantage above runs with skipblend TRUE:
		# R_BlendView_IsTrivial (gl_rmain.c) returns true at fresh-userdir
		# defaults, so the 3D scene renders straight to fbo 0, R_BlendView
		# never runs, and the one render target the frame allocates
		# (rt_screen) is never rendered INTO. Before 4d, therefore, neither
		# R_BlendView nor an offscreen scene target had ever executed on Metal
		# on any bed. Any nonzero r_viewfbo flips it: scene -> rt_screen ->
		# one POSTPROCESS blit to fbo 0.
		#
		# Set PRE-MAP, deliberately: r_viewfbo and r_hdr_shoulder are read per
		# frame, so there is no need to defer -- which side-steps the
		# burst-compression trap documented at SHOT_T above entirely.
		#
		# MEASURED 2026-08-06, and it CORRECTS the 4a/4b record: the hall
		# vantage's mean 0.0009 / max 45 / 15 px is NOT a Metal divergence.
		# At the same camera, three of the four frames are BYTE-IDENTICAL --
		# metal-direct, metal-offscreen (fbo1) and gl-offscreen (fbo1) all
		# agree at 0 px -- and gl DIRECT is the sole outlier, differing from
		# all three by exactly those 66 px / max 45. Structural reason:
		# Metal has no default framebuffer. Metal_Backend_SetRenderTarget's
		# fbo==0 arm binds mb_screentex/mb_screendepth, textures the backend
		# creates itself, so Metal's "direct" path IS an offscreen render;
		# GL's genuine window framebuffer, whose depth/stencil comes from the
		# SDL context request rather than from the engine, is the only surface
		# in the set the engine did not choose the attachments for. The 66 px
		# are short horizontal runs of covered-vs-uncovered at particular
		# geometry (black vs a dim wall texel), absent at the spawn camera --
		# so an edge/depth resolution difference, not a systematic shift. The
		# precise GL-side cause is NOT isolated and does not need to be: it is
		# a GL-vs-GL difference that no Metal gate depends on. Note the
		# consequence for instrument choice -- for this camera fbo1 compares
		# like with like and hall does not.
		fbo1)
			# 8-bit offscreen: the minimal case, so a failure is unambiguous.
			echo 'r_viewfbo 1'
			echo 'developer 1'
			echo 'con_notifytime 0' ;;
		fbo2)
			# 16F scene buffer AND the HDR shoulder actually exercised. The C
			# side forces the shoulder uniform to 0 below r_viewfbo 2, so this
			# is the only configuration in which it can move a pixel at all.
			#
			# The scene-brightness value is 6 and that number is MEASURED, not
			# picked. The shoulder only acts where a channel exceeds the knee,
			# and this camera's scene peaks around 0.6 even at brightness 2 --
			# so the obvious-looking pairing (brightness 2, the knee 0.75 that
			# the cvar's own help recommends) is a NO-OP: shoulder 0.75 against
			# shoulder 0 came back byte-identical, 0 of 307200 px, and the bed
			# would have shipped claiming to exercise something it never ran.
			# (The few pixels above 191 in that frame are 2D drawn AFTER the
			# blit, which the shoulder cannot touch by construction.) Lowering
			# the knee instead would work -- 0.30 moves 2998 px, 0.05 moves 36%
			# of the frame -- but pushing the SCENE over 1.0 is the honest
			# shape, because rolling off genuine above-1.0 highlights is what
			# the shoulder is for. At brightness 6 the knee 0.75 moves 15699 px
			# (5.1% of the frame, max 93).
			# SYNTHETIC LOOK, like the diffuse vantage: valid as a parity
			# instrument, not as a look reference.
			echo 'r_viewfbo 2'
			echo 'r_hdr_shoulder 0.75'
			echo 'r_hdr_scenebrightness 6'
			echo 'developer 1'
			echo 'con_notifytime 0' ;;
		fbo3)
			# 32F: format coverage.
			echo 'r_viewfbo 3'
			echo 'developer 1'
			echo 'con_notifytime 0' ;;
		bloom)
			# The bloom chain: R_Bloom_MakeTexture's downsample + exponent +
			# two blur directions, ping-ponging over pooled render targets
			# through MODE_GENERIC, then the USEBLOOM composite in
			# MODE_POSTPROCESS. Bloom also forces the offscreen path on its
			# own (r_fb.bloomwidth makes R_BlendView_IsTrivial false), so no
			# r_viewfbo is needed here.
			#
			# Entities ON at the spawn camera deliberately: the torch flame is
			# the one bright source in that view, and bloom with nothing to
			# bite on would be a vantage that passes while proving nothing.
			# This is the only 4d item Seb can reach without a console --
			# r_bloom has a menu checkbox and six sliders, and the "Better"
			# and "Best" lighting presets switch it on.
			#
			# Three knobs off their defaults, each for a measured reason, and
			# SYNTHETIC like the diffuse and fbo2 vantages -- a parity
			# instrument, not a look reference:
			#   _colorsubtract 0 (default 0.1) -- e1m3's spawn is dim (mean
			#     luminance 9.6/255), so at the default nearly the whole bloom
			#     texture subtracts to zero: measured 1337 px changed against
			#     bloom off, which is too near nothing to discriminate a
			#     correct composite from a broken one.
			#   _brighten 4 (default 1) -- same reason, from the other end.
			#   _colorexponent 2 (default 1) -- this one is structural, not
			#     cosmetic. R_Bloom_MakeTexture's darkening loop is
			#     `for (x = 1; x < min(exponent, 32);)`, so at the default it
			#     NEVER ITERATES: the squaring pass, its GL_Clear and its
			#     GL_BlendFunc(GL_SRC_COLOR, GL_ZERO) are dead. GL_SRC_COLOR
			#     is used nowhere else in the engine, so without this the
			#     blend factor and that pipeline key would ship unexercised.
			echo 'r_bloom 1'
			echo 'r_bloom_colorsubtract 0'
			echo 'r_bloom_brighten 4'
			echo 'r_bloom_colorexponent 2'
			# BEAUTY A1 (2026-09-16): the modern chain (r_bloom_m5) replaces all of
			# the above when on -- no darkening loop, so the _colorexponent pin is
			# INERT there and the three synthetic knobs are ignored. PINNED at 0 so
			# this vantage stays the 2001 chain's byte gate whatever the default does;
			# the new chain's own coverage is PARITY_EXTRA='r_bloom_m5 1'.
			echo 'r_bloom_m5 0'
			echo 'r_drawentities 1'
			echo 'r_cullentities_trace 0'
			echo 'developer 1'
			echo 'con_notifytime 0' ;;
		fxaa)
			# The two SPATIAL postprocess static parms, kept apart from the
			# colour ones (tint) so a failure localises to one or the other.
			# Both force the offscreen path on their own.
			#
			# FXAA is ~45 lines of dense arithmetic and the easiest thing in
			# this phase to get subtly wrong; its errors cluster on edges,
			# which is exactly what tgacmp's 16x16 block map shows.
			# r_colorfringe grows with distance from the screen centre, so it
			# is strongest in the corners and near-zero at the middle -- the
			# two have complementary footprints and do not mask each other.
			#
			# Both are static parms, so neither shows up in a permutation
			# number or in GL's shader name, and a vantage that silently
			# failed to enable them would PASS while proving nothing. Measured
			# in isolation at this camera against the same frame with both
			# off: FXAA alone moves 81667 px (26.6% of the frame, max 35),
			# colour fringe alone 158559 px (51.6%, max 67). The hall view is
			# dense world brushwork, which is where those edges come from --
			# r_drawentities is on but this camera happens to render NO
			# entities, so nothing here depends on model silhouettes.
			echo 'r_fxaa 1'
			echo 'r_colorfringe 0.02'
			echo 'r_drawentities 1'
			echo 'r_cullentities_trace 0'
			echo 'developer 1'
			echo 'con_notifytime 0' ;;
		tint)
			# The three COLOUR-space postprocess bits in one frame, in the
			# order the shader applies them: viewtint, then saturation, then
			# the gamma ramps.
			#
			# v_cshift sets CSHIFT_VCSHIFT, which V_CalcViewBlend re-applies
			# from a file static EVERY frame with no fade -- so it is a
			# PERMANENT viewblend, unlike bf, whose alphafade would make the
			# frame depend on when it was captured. That is what finally makes
			# USEVIEWTINT a measured bit rather than a compiled-but-never-run
			# one.
			#
			# v_gamma 0.7 is also the only boot in the sweep carrying
			# non-trivial gamma tables, which is what 4d-2's GENERIC defect
			# needed to become live -- see that commit for why no other
			# vantage could ever have caught it.
			#
			# viewtint, saturation and gammaramps are permutation bits, so
			# GL names them in its compile line ("postprocess viewtint
			# saturation gammaramps") and they cannot silently fail to
			# engage. SATURATION_REDCOMPENSATE is a static parm and can, so
			# it was isolated the same way the fxaa vantage's pair was:
			# against the identical frame at saturation 0.6 with
			# redcompensate 0, turning it on moves 125336 px (40.8% of the
			# frame, max 9).
			echo 'v_cshift 0.8 0.1 0.1 40'
			echo 'r_glsl_saturation 0.6'
			echo 'r_glsl_saturation_redcompensate 1'
			echo 'v_gamma 0.7'
			echo 'r_drawentities 1'
			echo 'r_cullentities_trace 0'
			echo 'developer 1'
			echo 'con_notifytime 0' ;;
		# METAL.md Phase 6 -- THE TEXTURE-FILTER VANTAGES. Both emit exactly ONE
		# line and nothing else, which is the point: `hall` shares this camera and
		# emits nothing at all, so hall's own frame IS the isolation baseline for
		# both of these, byte for byte, with no PARITY_EXTRA and no second bed.
		# (That is also why neither takes the developer/notify/counter treatment
		# the vantages above use -- developer 1 puts extra notify text in the
		# frame, which would break the shared baseline for a diagnostic these two
		# do not need. Their controls measure 0.0000 on both backends.)
		#
		# The line is emitted PRE-MAP, so it is set before any texture is created
		# and the world loads through GL_SetupTextureParameters with the mode
		# already in force -- the creation path, rather than GL_TextureMode_f's
		# re-walk of live textures. Both arrive at the same answer; the creation
		# path is the one Metal has an analogue for.
		texfilter)
			# THE FORCED MODE. `gl_texturemode <mode> force` is the only stock
			# control that OVERRIDES a texture's own TEXF_FORCENEAREST /
			# TEXF_FORCELINEAR flag: GL gates both force arms on !gl_filter_force
			# and falls through to gl_filter_min/_mag.
			#
			# ISOLATED BEFORE IT WAS BELIEVED (the 4d rule), and the isolation is
			# what actually diagnosed the defect rather than merely licensing the
			# number. Against the identical hall frame with the mode unset, this
			# one line moves 145217 px on GL -- 47.3% of the frame, mean 2.1884,
			# max 126 -- and 0 OF 307200 PX ON METAL. Not a small divergence: a
			# stock control that one backend could not see at all. The cross said
			# far less on its own (10.84% > 8, world gate FAIL), because a large
			# cross number looks the same whatever causes it.
			#
			# GL_NEAREST rather than a mipmapped mode deliberately -- its
			# minification enum carries NO mip term, so this exercises the
			# mip-disable arm (NotMipmapped on a TEXF_MIPMAP texture) and the
			# force arm in one frame.
			echo 'gl_texturemode GL_NEAREST force' ;;
		texfiltermag)
			# THE MAGNIFICATION SLIP, which needs its own vantage because it is
			# reachable WITHOUT `force` and texfilter therefore cannot localise it.
			# modes[4] is {"GL_NEAREST_MIPMAP_LINEAR", GL_NEAREST_MIPMAP_LINEAR,
			# GL_NEAREST} (gl_textures.c): minification and MAGNIFICATION are
			# independent enums and GL magnifies NEAREST here, while the Metal
			# sampler key consulted only gl_filter_min and so magnified LINEAR.
			#
			# Unforced, so every TEXF_FORCE* texture takes its own GL arm exactly
			# as before and the difference is confined to the plain world
			# textures -- the narrower signal, and the reason this is a second
			# vantage rather than a second line in the first. Isolated the same
			# way against the same hall baseline: 115541 px on GL (37.6%, mean
			# 0.5524, max 126), again 0 of 307200 px on Metal.
			echo 'gl_texturemode GL_NEAREST_MIPMAP_LINEAR' ;;
		# --- METAL.md Phase 5: the RT vantages -------------------------------
		# The first vantages in this bed that turn rt_metal on at all; the other
		# twenty-three are RT-free by DEFAULT rather than by assertion (every
		# boot takes a fresh userdir, and rt_metal defaults 0).
		#
		# Three pins are shared and each is load-bearing. rt_metal_history 0 is
		# the whole determinism story: all three kernels read cam.frame only
		# under `cam.history > 0`, so with history off the jitter falls back to
		# pixel-derived noise that cannot vary per frame -- measured in slice
		# 5-1 at 0 px differing across two boots, against 25261 px at history
		# 0.5 unpinned. rt_metal_reproject 1 since slice 5-4 -- the Metal
		# composite reprojects now, so the bed runs the shipped default on both
		# sides. Note what that CANNOT prove on its own: with a static camera the
		# current and shown bases are equal, so the remap is an identity and
		# enabling it changes 0 of 307200 px on BOTH backends (measured). The
		# instrument that does prove it is RT_METAL_REPROJ_TEST=1, which pretends
		# the shown frame was traced from 2 degrees and 8 units away -- see the
		# 5-4 record. r_volumetric 0 is now a SEPARATION rather than a refusal:
		# 6-2b put the murk's GL march on Metal, so these vantages could carry
		# it -- but the murk is a screen-wide pass over the whole frame, and one
		# on top of the RT term would blend the two ports' errors into a single
		# number that could not attribute either. The murk has its own vantages
		# below.
		rt_spawn|rt_wall|rt_ogre|rt_lava|rt_liquid|rt_sun)
			echo 'rt_metal 1'
			echo 'rt_metal_history 0'
			echo 'rt_metal_reproject 1'
			echo 'r_volumetric 0'
			echo 'developer 1'
			echo 'con_notifytime 0'
			case "$2" in
			rt_spawn)
				# RT over the baked lightmap (walllight 0 = the modulate arm),
				# traced at FULL resolution. NOTE (corrected at Phase 8-2): the
				# old claim here that full res means TEXF_FORCENEAREST was
				# false for as long as it stood -- the vantages pin
				# rt_metal_reproject 1 below, and reprojection samples
				# fractionally, so it forces LINEAR even at full res. The
				# NEAREST adoption arm's first real coverage is this vantage
				# with PARITY_EXTRA='rt_metal_sameframe 1', which clamps the
				# reprojection identity and restores the NEAREST fetch -- a
				# deliberately DIFFERENT (but cross-backend-agreeing) image.
				echo 'rt_metal_walllight 0'
				echo 'rt_metal_scale 1' ;;
			rt_sun)
				# THE SKY LIGHT (SEPTEMBER2 D): AD's start hub at walllight 0.8
				# with rt_metal_sun 1 -- the map's own sun (_sunlight 250,
				# _sun_mangle "300 -70"), one closest-hit ray per pixel toward it
				# through the open-sky instance. Its disc jitter is the kernels'
				# static per-pixel stream at history 0, so the frozen bed holds.
				# This vantage RECORDS WITH THE SUN ON: the preamble pins
				# rt_metal_sun 0 for every other vantage, and this line wins here.
				# RECORDED STATE (2026-09-06): the CROSS is TWO-STATE on Metal --
				# 889 px / max 22 (world PASS) or 83% at mean 3.3 -- and it is NOT
				# the sun: at rt_metal 0 the backends still differ by 82%, both
				# controls byte-clean. A pre-existing raster divergence on AD's
				# content (the water precedent: recorded, not a gate). The sun's
				# own isolation is the proof: 39.2% @ 4.42 GL, 39.8% @ 4.50 Metal.
				echo 'rt_metal_walllight 0.8'
				echo 'rt_metal_scale 0.5'
				echo 'rt_metal_sun 1'
				# AD's start has lava: the heat shimmer is Metal-only by capability
				# (the e1m7 vantages' pin, for the same reason)
				echo 'r_lavashimmer 0' ;;
			rt_wall)
				# THE CANARY. walllight > 0 makes cl_screen.c force
				# r_fullbright, so the world is drawn as raw albedo and the RT
				# composite supplies ALL of its lighting: a composite that
				# reports active without drawing renders a blinding white
				# frame. That failure is reachable by NONE of the other
				# vantages, and it is Seb's own configuration (0.8). The view
				# model is drawn here too, because the collapsed depth range
				# that masks it is the other thing only this path exercises.
				echo 'rt_metal_walllight 0.8'
				echo 'rt_metal_scale 0.5'
				echo 'r_drawviewmodel 1' ;;
			rt_ogre)
				# Entities in the RT: the dynamic BLAS, smooth normals and the
				# light-core instance split all reach the term here.
				echo 'rt_metal_walllight 0.8'
				echo 'rt_metal_scale 0.5'
				echo 'r_drawentities 1'
				echo 'r_cullentities_trace 0' ;;
			rt_lava)
				# e1m7, the only lava in the bed: the static lava TLAS instance
				# (rt_metal_lavaemissive) and the merged lava lights
				# (rt_metal_lavalights) are RT paths e1m3 cannot reach at all.
				# r_lavaboil 0 because the boil warps texcoords from cl.time,
				# which keeps running under sv_freezenonclients; USELAVA is
				# GL32-gated in any case, so Metal's lava is static and the
				# unpinned boil would be a pure GL-side animation in the diff.
				#
				# Normal value 0.0010 / max 1, confirmed four independent ways
				# (twice isolated, once in a six-vantage sweep, and against a
				# hand-written probe that agrees with the isolated parity run
				# BYTE-FOR-BYTE).
				#
				# ONE CORRELATED OUTLIER PAIR HAS BEEN SEEN, and it is worth
				# knowing about because it defeats the usual control: in one
				# 27-vantage sweep this vantage came out at 4.6855 / max 221
				# with BOTH controls clean at zero -- because both GL boots went
				# odd TOGETHER, so they agreed with each other. Diagnosis, which
				# is the reusable part: compare each backend against ITSELF
				# across two runs. The Metal frames were byte-identical (0 of
				# 307200 px) and the GL pair was the mover, so nothing about the
				# Metal port was implicated. It has not reproduced since.
				#
				# A cl.time-phase mechanism was hypothesised for it and is
				# DISPROVEN, recorded here so nobody re-derives it: screenshots
				# taken at 16.00, 16.05 and 16.15 seconds are byte-identical, so
				# the bed is frozen with respect to shot timing, and e1m7's 5 Hz
				# animated-texture frame and 10 Hz lightstyle tick are not
				# reached. Treat a big number here as a suspected outlier and
				# re-run before believing it.
				#
				# r_lavashimmer 0 since 2026-08-16: F7's heat shimmer is
				# METAL-ONLY BY CAPABILITY (vid.m5postfx) and this camera looks
				# across the lava, so at the default the whole Metal frame is
				# displaced by the haze and the cross reads 32.1 / 98.5% with
				# clean controls. Pinned off it returns to its recorded value.
				# The shimmer's Metal-side liveness is smoke run O's.
				echo 'rt_metal_walllight 0.8'
				echo 'rt_metal_scale 0.5'
				echo 'r_lavaboil 0'
				echo 'r_lavashimmer 0'
				echo 'r_lavaglow 1' ;;
			rt_liquid)
				# rt_metal_liquids: the RT term multiplied into alpha-blended
				# water in the TRANSPARENT pass -- the one surface class the
				# composite itself never reaches. Seb's own 0.45.
				#
				# THIS VANTAGE COULD NOT EXIST UNTIL 5-5-3, and the reason is
				# worth keeping. Three sessions recorded that "the pre-load
				# freeze suppresses the r_wateralpha path"; it does not. The
				# QRP replacement pack supplies an image for *04water1, so
				# Mod_LoadTextureFromQ3Shader succeeded and model_brush.c
				# skipped the whole Q1 liquid classification -- the texture
				# arrived as a plain opaque wall with no MATERIALFLAG_WATERALPHA
				# for any r_wateralpha cvar to act on. Frozen and unfrozen
				# printed IDENTICAL flags; the freeze was never involved.
				# m5_liquidflags (default 1) is the fix, so this vantage runs
				# FROZEN like every other one and inherits its zero controls.
				#
				# r_waterscroll 0 pins the surface phase; the walllight arm is
				# Seb's, so the term the water multiplies is the whole scene
				# lighting rather than a modulate over a lightmap.
				echo 'rt_metal_walllight 0.8'
				echo 'rt_metal_scale 0.5'
				echo 'rt_metal_liquids 0.45'
				echo 'r_wateralpha 0.8'
				echo 'r_wateralpha_force 1'
				echo 'r_waterscroll 0' ;;
			esac ;;
		# --- METAL.md Phase 6-4: the LAVA BOIL -------------------------------
		# e1m7, the only lava in the bed, with no RT and no murk so the SURFACE
		# arm is the only thing under test. `rt_lava` cannot serve: it pins
		# r_lavaboil 0, and it did so partly because "USELAVA is GL32-gated in
		# any case, so Metal's lava is static" -- a pin whose justification 6-4
		# removes.
		#
		# IT IS A FULL GATE, and that was measured rather than assumed -- the
		# first version of this note hedged that the boil's cl.time phase would
		# make the controls informational only, on the strength of the rt_lava
		# arm's claim that cl.time "keeps running under sv_freezenonclients".
		# Both controls come out at 0 of 307200 px, so whatever that arm was
		# describing, this camera does not suffer it and the vantage carries a
		# real cross (0.0008 / max 1). Do not weaken it back.
		#
		# ISOLATION, per the 4d rule and on EACH backend separately: r_lavaboil
		# 0.6 against 0 at this camera moves 1619 px at mean 0.0034 / max 24 on
		# GL and **exactly the same 1619 px, mean and max on Metal**, with the
		# block map confined to the lava sheet's own silhouette and every other
		# block pure clean. Agreeing on the magnitude of the feature is the
		# claim worth having; agreeing on the frame alone is what two no-ops do.
		#
		# The exactness claim for the rest of 6-4 is made elsewhere, because it
		# needs content this map does not have: on lava-free e1m3, USELAVA
		# compiled IN is byte-identical to it compiled OUT (spawn, hall and ogre,
		# both backends, 0 px), since LavaParams is zeroed for every non-lava
		# batch.
		lavaboil)
			# r_lavashimmer 0: see rt_lava -- e1m7, the haze is Metal-only.
			echo 'r_lavaboil 0.6'
			echo 'r_lavaglow 1'
			echo 'r_lavashimmer 0'
			echo 'developer 1'
			echo 'con_notifytime 0' ;;
		# --- METAL.md Phase 6: the MURK vantages -----------------------------
		# The fork's signature pass, and until now the bed had no vantage that
		# turned it on at all. That was not a pin -- correcting the note this
		# file used to carry: `r_volumetric 0` is emitted ONLY in the rt_* arm
		# above, and the other twenty-four vantages are murk-free because every
		# boot takes a fresh userdir and the cvar defaults to 0. Same blindness,
		# different mechanism, and the mechanism matters: a DEFAULT can move
		# under a bed, a pin cannot.
		#
		# DETERMINISM. The murk's density model reads cl.time through exactly
		# two routes and no others -- `windoffset = VolumetricWind *
		# VolumetricNoise.w` and its ground twin (shader_density.h references
		# neither uniform anywhere else; the two offsets are built in each
		# march's main()). Pinning BOTH winds to zero therefore makes the whole
		# model time-independent, which is what lets these vantages inherit the
		# frozen bed's zero controls instead of needing a clock pin of their
		# own. QUOTED, both of them: the console keeps only the FIRST token of
		# an unquoted multi-token cvar value and would silently set "0".
		#
		# The look knobs are pinned EXPLICITLY rather than left at their
		# defaults, because a default that drifts would move the bed silently.
		# The values are Seb's own, so the frames these vantages compare are
		# the frames he will actually be looking at.
		murk|murk_water|murk_kernel|murk_shafts|murk_sky|murk_liquid)
			echo 'r_volumetric 1'
			echo 'r_volumetric_wind "0 0 0"'
			echo 'r_volumetric_groundwind "0 0 0"'
			echo 'r_volumetric_density 0.22'
			echo 'r_volumetric_ground 1'
			echo 'r_volumetric_groundheight 48'
			echo 'r_volumetric_height 256'
			echo 'r_volumetric_corner 5'
			echo 'r_volumetric_scale 0.75'
			echo 'r_volumetric_steps 32'
			echo 'developer 1'
			echo 'con_notifytime 0'
			case "$2" in
			murk)
				# The GL march on both backends: the arm 6-2b ported, brought
				# into the harness from the local bed it was measured on. The
				# map's own spawn point, which is torch-lit and has the corner
				# term and the ground layer both live.
				;;
			murk_liquid)
				# LIQUIDFOG's bed: BLENDED liquid seen THROUGH thick murk, and
				# the one vantage in the sweep where a liquid surface is a
				# large share of the frame rather than a strip on the horizon.
				#
				# WHY IT EXISTS. The murk composites inside R_RenderScene,
				# BEFORE R_MeshQueue_RenderTransparent, and transparent
				# surfaces write no depth -- so the moment r_wateralpha_force
				# makes water blended it leaves the pass that was fogging it
				# and reads at full brightness through fog thick enough to
				# hide the wall behind it. r_volumetric_liquidfade is the
				# per-fragment hook that fixes it, and this is the frame that
				# can see it.
				#
				# WHY e1m2 AND NOT e1m3. Measured 2026-08-30, and it is the
				# whole reason this vantage is not simply rt_liquid with the
				# murk turned on: at rt_liquid's camera the moat is a small
				# part of the frame. Liquid coverage measured by an
				# r_wateralpha 0.7-vs-0.35 A/B, and the murk's own response
				# inside that mask against outside it:
				#
				#   e1m3 rt_liquid camera        water a few % of the frame
				#   e1m2  900 500 240   16.5%   fog delta IN 0.39 / OUT 4.04
				#   e1m2  800 500 210   22.7%   fog delta IN 0.71 / OUT 7.70
				#   e1m2  900 400 200   29.8%   fog delta IN 0.55 / OUT 4.00
				#
				# 900 400 200 it is: 80218 px of 307200 are blended water, and
				# the murk moves them SEVEN TIMES LESS than it moves the rest
				# of the frame. The residual 0.55 is not the murk reaching the
				# water -- it is the 30% of each pixel that sees the opaque,
				# correctly-fogged pool floor through it.
				#
				# e1m2's liquid is WATER throughout (105 water leaves, no slime
				# and no lava anywhere on the map -- checked in the BSP), so
				# this vantage says nothing about the slime and lava tints; the
				# density model's liquid arm is murk_water's subject.
				#
				# The camera looks -Y because e1m2's info_player_start carries
				# angle 270 and YAW IS NOT DELIVERED on the frozen bed (four
				# boots at one origin requesting 0/90/180/270 came out
				# byte-identical -- the documented camera-delivery rule, and it
				# reproduces here). Pitch IS delivered, divided by three, hence
				# the 45 for 15 degrees down. The origin is what aims this
				# vantage; do not try to turn it.
				#
				# Two-boot controls measured byte-clean at zero on BOTH
				# backends, cross 0.0001 / max 1 / 133 px.
				echo 'r_volumetric_watermist 0.1'
				echo 'r_volumetric_mistheight 10'
				echo 'r_volumetric_waterdensity 0.6'
				echo 'r_wateralpha 0.7'
				echo 'r_wateralpha_force 1'
				echo 'r_waterscroll 0' ;;
			murk_water)
				# The liquid half of the density model, which the spawn camera
				# never reaches: the field's G channel (a SIGNED distance,
				# whose trilinear zero crossing reconstructs the waterline),
				# the surface-mist band that rides it, and the inliquid mix
				# that swaps density and tint wholesale. r_wateralpha_force so
				# the moat is genuinely transparent and the in-pool murk is
				# visible from the bank -- the configuration the feature was
				# designed for.
				echo 'r_volumetric_watermist 0.1'
				echo 'r_volumetric_mistheight 10'
				echo 'r_volumetric_waterdensity 0.6'
				echo 'r_wateralpha 0.8'
				echo 'r_wateralpha_force 1'
				echo 'r_waterscroll 0' ;;
			murk_kernel|murk_sky)
				# THE FOG KERNEL (rt_metal_fog): the murk pass degenerates to a
				# masked composite of the sidecar's own marched output, and the
				# GL march below it becomes the per-frame fallback. This is the
				# vantage 6-3b exists for.
				#
				# rt_metal_fog_history 0 is as load-bearing here as
				# rt_metal_history 0 is on the RT vantages, and for a DIFFERENT
				# reason: the fog EMA accumulates over however many frames have
				# been composited since the map loaded, which is a
				# load-duration-dependent count and therefore backend-dependent.
				# At 0 there is no accumulation to be adrift. It also switches
				# off the stillness denoise, which floors the EMA at 0.75 only
				# while history > 0.
				#
				# The ogre camera: torch-lit, with geometry close enough that
				# both the fog light term and the beams have real structure.
				#
				# murk_sky shares this whole cvar block and swaps only the
				# CAMERA (the sky vantage's, below e1m3's sky shaft, looking
				# up): the first vantage anywhere to frame MURK + SKY together
				# -- the coverage hole the 2026-08-09 e1m1 triage found. Both
				# marches fog sky to ~99% extinction at ordinary densities and
				# no bed had ever looked.
				echo 'rt_metal 1'
				echo 'rt_metal_history 0'
				echo 'rt_metal_reproject 1'
				echo 'rt_metal_fog 1'
				echo 'rt_metal_fog_history 0'
				echo 'rt_metal_fog_intensity 0.5'
				echo 'rt_metal_fog_beams 0.5'
				echo 'rt_metal_fog_scale 0.5'
				echo 'rt_metal_fog_steps 24' ;;
			murk_shafts)
				# THE SCREEN-SPACE GOD RAYS (rt_metal_shafts), the cheaper tier
				# the fog kernel supersedes. Same camera as murk_kernel on
				# purpose, so the two are directly comparable: they differ in
				# exactly which of the two mutually exclusive static parms is
				# compiled in (gl_rmain.c's enable conditions -- shafts require
				# !rt_metal_fog, kernel fog requires it -- so USEVOLUMETRICSHAFTS
				# and USEVOLUMETRICKERNELFOG can never both be set).
				#
				# _intensity 0.5 rather than Seb's archived 0.05: that 0.05 was
				# a workaround for the pre-shoulder blowout and the 2026-08-01
				# beam shoulder retired it. A vantage wants the term clearly
				# above the noise floor, not at the value that made a defect
				# tolerable.
				echo 'rt_metal 1'
				echo 'rt_metal_history 0'
				echo 'rt_metal_reproject 1'
				echo 'rt_metal_fog 0'
				echo 'rt_metal_shafts 1'
				echo 'rt_metal_shafts_history 0'
				echo 'rt_metal_shafts_intensity 0.5'
				echo 'rt_metal_shafts_samples 8'
				echo 'rt_metal_shafts_scale 0.75' ;;
			esac
			# Clear the notify area a second before the shot (2026-08-21, the
			# swirl-still discipline arriving here at last). The bake-duration
			# notify lines ("baked in NN ms") carry per-boot digits, and
			# con_notifytime 0 does NOT expire notify on a frozen clock (the
			# documented fact) -- so every murk vantage used to wear a per-boot
			# text stripe in rows 0-24, its controls never read whole-frame
			# zero, and every recorded reading had to exclude those rows by
			# hand. With the clear, the whole-frame number IS the reading.
			# (clear empties the rendered console only; the captured log the
			# greps read is untouched.)
			case "$2" in
			murk_kernel|murk_shafts|murk_sky|murk_liquid) echo 'defer 15 "clear"' ;;
			*) echo 'defer 11 "clear"' ;;
			esac ;;
		esac
		if [ "$1" = metal ]; then
			echo 'defer 1 "vid_renderer metal; vid_restart"'
		else
			echo 'defer 1 "vid_renderer gl; vid_restart"'
		fi
		case "$2" in
		sprite)  echo 'defer 3 "map e4m7"' ;;
		rt_lava|lavaboil) echo 'defer 3 "map e1m7"' ;;
		rt_sun)  echo 'defer 3 "map start"' ;;   # AD's own start (the ad-first search order shadows id1's)
		# e1m2 is the only map in the bed with a liquid body big enough to fill
		# a real share of the frame -- see the murk_liquid block above.
		murk_liquid) echo 'defer 3 "map e1m2"' ;;
		*)       echo 'defer 3 "map e1m3"' ;;
		esac
		echo 'defer 5 "sv_cheats 1"'
		echo 'defer 6 "noclip"'
		case "$2" in
		spawn|ents_spawn|viewmodel|colormap|points|flash|bloom) ;;   # the map's own spawn point, default angles
		rt_spawn|rt_lava|murk|lavaboil) ;;   # ditto: e1m3's and e1m7's own spawn points
		rt_wall|rt_ogre|murk_kernel|murk_shafts)
			# the ogre camera, shared with the vantages below -- torch-lit, with
			# geometry close enough that the RT term has real structure in it
			echo 'defer 7 "prvm_edictset server 1 origin \"-154 -1102 90\""'
			echo 'defer 7.5 "prvm_edictset server 1 angles \"5 0 0\""'
			echo 'defer 7.6 "prvm_edictset server 1 fixangle 1"' ;;
		hall|flipped|ents_hall|diffuse|fbo1|fbo2|fbo3|fxaa|tint|texfilter|texfiltermag)
			echo 'defer 7 "prvm_edictset server 1 origin \"544 288 88\""'
			echo 'defer 7.5 "prvm_edictset server 1 angles \"10 135 0\""'
			echo 'defer 7.6 "prvm_edictset server 1 fixangle 1"' ;;
		ogre|beam|beam_lit|fog)
			# The smoke tests' documented ogre-adjacent spot; yaw 0 frames the
			# ogre close on the left with three zombies centre. NOTE: only the
			# FIRST angles+fixangle of a boot applies on the frozen bed --
			# re-sets within one boot are not delivered, so one camera per boot.
			# NOTE also: requested PITCH arrives divided (the player-angles
			# convention) -- ask for 3x the pitch you want; yaw is verbatim.
			echo 'defer 7 "prvm_edictset server 1 origin \"-154 -1102 90\""'
			echo 'defer 7.5 "prvm_edictset server 1 angles \"5 0 0\""'
			echo 'defer 7.6 "prvm_edictset server 1 fixangle 1"' ;;
		sky|murk_sky)
			# Below e1m3's one sky shaft, looking up: the sphere fills the top
			# half of the frame, over the mask + scissor machinery.
			echo 'defer 7 "prvm_edictset server 1 origin \"1348 628 780\""'
			echo 'defer 7.5 "prvm_edictset server 1 angles \"-85 90 0\""'
			echo 'defer 7.6 "prvm_edictset server 1 fixangle 1"' ;;
		murk_liquid)
			# e1m2, low over the great moat looking down its length. The pitch
			# is asked as 45 and arrives as 15 (the documented divide-by-three);
			# the yaw is e1m2's own spawn heading and cannot be changed here.
			echo 'defer 7 "prvm_edictset server 1 origin \"900 400 200\""'
			echo 'defer 7.5 "prvm_edictset server 1 angles \"45 270 0\""'
			echo 'defer 7.6 "prvm_edictset server 1 fixangle 1"' ;;
		water|rt_liquid|murk_water)
			# Just above the moat surface (z -368, probe-confirmed water at
			# this column), shallow look north across confirmed water.
			# NOTE the yaw request is decoration on the frozen bed (see the
			# camera-delivery note in the header): rt_liquid gets e1m3's spawn
			# heading from this origin, and what makes it a valid instrument is
			# the measured liquids-on-vs-off isolation, not the requested angle.
			echo 'defer 7 "prvm_edictset server 1 origin \"-1000 -600 -340\""'
			echo 'defer 7.5 "prvm_edictset server 1 angles \"30 90 0\""'
			echo 'defer 7.6 "prvm_edictset server 1 fixangle 1"' ;;
		esac
		case "$2" in
		points)
			echo 'defer 8 "pointfile"' ;;
		beam|beam_lit)
			# give/impulse must precede the one +attack of the boot (impulses
			# are blocked once attack_finished is set against frozen sv.time);
			# the beam's endtime rides frozen cl.time, so the bolt persists.
			echo 'defer 8 "give 8"'
			echo 'defer 8.2 "give c 100"'
			echo 'defer 8.5 "impulse 8"'
			echo 'defer 9 "+attack"'
			echo 'defer 9.5 "-attack"' ;;
		flash)
			echo 'defer 9 "+attack"'
			echo 'defer 9.5 "-attack"' ;;
		fog)
			echo 'defer 8 "fog 0.05 0.3 0.3 0.3"' ;;
		esac
		echo "defer $SHOT_T \"screenshot $3.tga\""
		case "$2" in
		ents_spawn|ents_hall|ogre|viewmodel|colormap|diffuse|sky|water|points|beam|beam_lit|flash|sprite|fog|fbo1|fbo2|fbo3|bloom|fxaa|tint|rt_spawn|rt_wall|rt_ogre|rt_lava|rt_liquid|rt_sun|murk|murk_water|murk_kernel|murk_shafts|murk_sky|murk_liquid|lavaboil)
			# Counter capture AFTER the screenshot: one clean dump (the five
			# headline counters), then r_speeds 1 for a frame so
			# entities_surfaces/_triangles populate, then a second dump. Only
			# those two lines are read from dump 2 -- the overlay's own 2D
			# draws contaminate draws* there.
			echo "defer $SHOT_T.4 \"r_speeds_dump\""
			echo "defer $SHOT_T.6 \"r_speeds 1\""
			echo "defer $((SHOT_T+1)).2 \"r_speeds_dump\""
			;;
		esac
		# PARITY_EXTRA: newline-separated cvar lines appended to EVERY vantage,
		# for isolating one knob against an otherwise identical frame (the 4d
		# rule -- a feature bed must be shown to move pixels before its parity
		# number means anything). Emits NOTHING when unset, so the spawn and
		# hall cfg text stays byte-identical to the 4a acceptance bed.
		# (an `[ ] && printf` one-liner would abort the whole script under set -e
		# whenever the variable is unset -- the common case)
		if [ -n "${PARITY_EXTRA:-}" ]; then printf '%s\n' "$PARITY_EXTRA"; fi
		echo "defer $QUIT_T quit"
	} > m5/parity_run.cfg
}

shot() { # backend vantage tag
	SB=$(mktemp -d)
	writecfg "$1" "$2" "$3"
	if [ "$2" = points ]; then
		# The pointfile the points vantage reads: eight static billboards in
		# the spawn view (spawn faces yaw 270). The three +-4096 axis beams
		# spawn at the LAST point, and their rendered brightness proved
		# boot-unstable (both controls dirty, mean 5-7) -- so the last point
		# is sacrificial, buried at z -3000: the beams are depth-tested and
		# the floor occludes all three, leaving only the stable billboards.
		# Transient, like parity_run.cfg -- m5/ is gitignored.
		mkdir -p m5/maps
		cat > m5/maps/e1m3.pts <<'PTSEOF'
-736 -1742 120
-696 -1792 100
-776 -1792 140
-736 -1842 80
-656 -1892 130
-816 -1892 110
-736 -1892 160
-736 -1692 90
-736 -1500 -3000
PTSEOF
	fi
	# rt_sun needs the AD gamedir; without it `map start` would load id1's start
	# and the vantage would pass as a different picture -- skip loudly instead
	if [ "$2" = rt_sun ] && [ ! -d ad ]; then echo "SKIP: rt_sun (ad gamedir not installed)"; rm -f m5/parity_run.cfg; rm -rf "$SB"; return; fi
	./darkplaces-sdl -userdir "$SB" $GAMEARGS -window -nosound +exec parity_run.cfg > "$OUT/$3.log" 2>&1 || true
	find "$SB" -name "$3.tga" -exec cp {} "$OUT/" \; 2>/dev/null
	rm -f m5/parity_run.cfg m5/maps/e1m3.pts; rm -rf "$SB"
}

for v in $VANTAGES; do
	echo "############ vantage: $v"
	shot gl    "$v" "w_${v}_gl_a"
	shot gl    "$v" "w_${v}_gl_b"
	shot metal "$v" "w_${v}_mt_a"
	shot metal "$v" "w_${v}_mt_b"
	if [ ! -f "$OUT/w_${v}_gl_a.tga" ] || [ ! -f "$OUT/w_${v}_mt_a.tga" ]; then
		echo "  (missing capture -- see $OUT/w_${v}_*.log)"; continue
	fi
	echo "--- CONTROL GL (two boots, must be clean)"
	python3 "$CMP" "$OUT/w_${v}_gl_a.tga" "$OUT/w_${v}_gl_b.tga" | sed 's/^/  /' || true
	echo "--- CONTROL METAL (two boots, must be clean)"
	python3 "$CMP" "$OUT/w_${v}_mt_a.tga" "$OUT/w_${v}_mt_b.tga" | sed 's/^/  /' || true
	echo "--- CROSS-BACKEND (the acceptance)"
	python3 "$CMP" --gate world "$OUT/w_${v}_gl_a.tga" "$OUT/w_${v}_mt_a.tga" | sed 's/^/  /' || true
	case "$v" in
	murk|murk_water|murk_kernel|murk_shafts|murk_sky|murk_liquid)
		# WHICH ARM RAN, per backend, printed rather than assumed. The murk has
		# three of them and they are not distinguishable from a parity number:
		# the kernel-fog composite falls back to the GL march SILENTLY whenever
		# the sidecar's output is unavailable (first frames, compile failure, RT
		# off), by design so fog never pops out -- and a murk_kernel vantage
		# that fell back on BOTH backends would agree perfectly while proving
		# nothing at all. That is the 4d no-op-bed trap wearing a fallback for a
		# disguise, and the engine already prints the answer (gl_rmain.c's
		# lastfogpath line exists for exactly this reason). The compile line
		# names the permutation, so it is the second, independent tell.
		echo "--- MURK PATH (which arm rendered -- a fallback here voids the number above)"
		for side in gl mt; do
			printf '  [%s] ' "$side"
			# The LAST state change BEFORE the screenshot, not merely one that
			# occurred somewhere in the boot. That distinction is not academic:
			# gl_rmain.c prints this line only on CHANGE, and the kernel is
			# legitimately unavailable for the first frames after a map load, so
			# every kernel boot logs "falling back" and then "ACTIVE". A plain
			# grep for either string matches both and answers the wrong
			# question. Measured on this bed at HEAD: the GL log's last state
			# before the shot is ACTIVE and Metal's is fallback -- which IS the
			# 6-3b gap, and a grep that could not order them would have hidden
			# it behind a magenta cross number.
			awk -v tag="Wrote w_${v}_${side}_a.tga" '
				index($0, tag) { stop = 1 }
				!stop && /RT fog kernel ACTIVE/ { s = "kernel-ACTIVE" }
				!stop && /falling back to the GL march/ { s = "kernel-FELLBACK" }
				END { printf "%s ", (s == "" ? "no-kernel-line" : s) }' "$OUT/w_${v}_${side}_a.log"
			grep -ao 'volumetricfog[a-z0-9 ]* compiled' "$OUT/w_${v}_${side}_a.log" | sort -u | tr '\n' ';'
			echo
		done ;;
	esac
	case "$v" in
	ents_spawn|ents_hall|ogre|viewmodel|colormap|diffuse|sky|water|points|beam|beam_lit|flash|sprite|fog|fbo1|fbo2|fbo3|bloom|fxaa|tint|rt_spawn|rt_wall|rt_ogre|rt_lava|rt_liquid|rt_sun|murk|murk_water|murk_kernel|murk_shafts|murk_sky|murk_liquid|lavaboil)
		# Dump 1 (clean frame): the headline counters. Dump 2 (r_speeds
		# overlay frame): ONLY entities_surfaces/_triangles -- the overlay
		# draws its own text quads, whose count varies with the digits
		# printed, so its draws* lines are not parity material.
		#
		# 4d added the bloom and render-target counters: the offscreen path
		# and the bloom chain are exactly what this phase turns on, and the
		# dump only prints a counter at all when it is nonzero -- so their
		# PRESENCE is itself the assertion that the path ran. r_stat names
		# are anchored (^  NAME  +), so `bloom` matches only the bare counter.
		echo "--- COUNTERS gl_a vs mt_a (dump 1 headline; dump 2 entity lines only)"
		for side in gl mt; do
			echo "  [$side]"
			awk '/r_speeds_dump: last completed frame/{n++} n==1' "$OUT/w_${v}_${side}_a.log" | grep -aE '^  (entities|particles|lights|lights_lighttriangles|draws|draws_vertices|draws_elements|batch_batches|bloom|bloom_copypixels|bloom_drawpixels|rendertargets_used|rendertargets_pixels) +[0-9]+' | sed 's/^/  /'
			awk '/r_speeds_dump: last completed frame/{n++} n==2' "$OUT/w_${v}_${side}_a.log" | grep -aE '^  (entities_surfaces|entities_triangles) +[0-9]+' | sed 's/^/  /'
		done ;;
	esac
done
