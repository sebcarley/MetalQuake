#!/usr/bin/env python3
"""check-tiers.py -- assert test/perf/levers.tsv's tier_* arms mirror menu.c's
shipped m5_quality_levers[] table, exactly and in full.

WHY THIS EXISTS. The tier arms are meant to BE the shipped tiers, and they have
silently stopped being so twice:

  * COVERAGE -- every tier arm omitted four shipped levers (rt_metal_lightsample,
    rt_metal_fog_beams, rt_metal_shafts_samples, rt_metal_shafts_scale). The arms
    were accidentally right only because those four happened to equal their tier
    values in the config the bench copies in; the moment a default moves or a
    config archives something else, an arm silently stops being its tier.
  * VALUE -- 2026-08-29's "rate 1 ships" edit rewrote `rt_metal_gi_rate 2` to 1
    across every phase-T row and caught the two HALF-RATE CONTROL arms with it,
    making tier_best_r2 and tier_good_gi_r2 byte-identical to their full-rate
    parents while still described as half rate. Re-running "does half-rate GI
    clear Good's target?" from that file would have measured full rate and
    reported no cost.

Neither is visible in a results.tsv: the sweep runs happily and reports numbers
for an arm that is not the thing it is named after. This is the net.

Usage:  python3 test/perf/check-tiers.py        # exit 0 = clean, 1 = divergence
Deliberate exceptions are declared in EXPECT_DIFFERS below, each with a reason.
"""
import re, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MENU = os.path.join(ROOT, 'menu.c')
LEVERS = os.path.join(ROOT, 'test', 'perf', 'levers.tsv')
TIERS = ['Stock', 'Superfast', 'Fast', 'Good', 'Better', 'Best', 'Ultimate']
# Index by NAME, never by a bare integer: inserting Superfast at the bottom on
# 2026-08-30 shifted every one of these by one, and a literal would have
# silently re-pointed each control arm at the tier below the one it names.
T = {n: i for i, n in enumerate(TIERS)}

# arm -> (tier index, {cvar: overridden value}, reason). An arm listed here is a
# deliberate control: it is the tier EXCEPT for the named cvars.
EXPECT_DIFFERS = {
    'tier_best':        (T['Best'], {}, 'the shipped Best'),
    # --- TARGET-120 block (phase Z, 2026-09-18) -------------------------------
    # Seb: "120 sounds fine for best". Best measures 111/112 on demo46/demo26
    # (2026-09-17 night) and the whole BEAUTY round is only 3.1%, so the 7.5%
    # has to come from the main stack. These price the candidates; none is
    # wired until he picks a look.
    'tier_superfast_spatial': (T['Superfast'], {'r_metalfx': 1, 'r_metalfx_reactive': 0},
                               'Superfast at the PRE-2026-09-18 SPATIAL scaler -- the revert control'),
    'tier_best_ts375':       (T['Best'], {'rt_metal_scale': 0.375},
                               'Best at the PRE-WIRING trace 0.375 -- the revert control for 2026-09-18 option A'),
    # 2026-09-18: Best took trace 0.3125 (TARGET-110 option A), so the whole phase-Z
    # block -- which measured AGAINST the 0.375 Best -- declares 0.375 as the state it
    # was taken in, and tier_best_z_ts3125 is now the shipped value rather than a candidate.
    'tier_best_z_ref':       (T['Best'], {'rt_metal_scale': 0.375}, 'the block reference -- the shipped Best, phase Z'),
    'tier_best_z_fogbuf':    (T['Best'], {'rt_metal_scale': 0.375, 'rt_metal_fog_scale': 0.3125},
                               "Best at Better's fog buffer -- the biggest single candidate"),
    'tier_best_z_steps16':   (T['Best'], {'rt_metal_scale': 0.375, 'rt_metal_fog_steps': 16},
                               'Best at 16 fog march steps -- gives back the 09-06 sharpness'),
    'tier_best_z_ts3125':    (T['Best'], {'rt_metal_scale': 0.3125},
                               'Best tracing at 0.3125'),
    'tier_best_z_samples3':  (T['Best'], {'rt_metal_scale': 0.375, 'rt_metal_samples': 3},
                               'Best at 3 shadow rays -- measured nil in 09-05, confirm rather than assume'),
    'tier_best_z_beautyoff': (T['Best'], {'rt_metal_scale': 0.375, 'cl_particles_texsize': 64, 'cl_particles_blood_droplet': 0,
                               'cl_particles_soft': 0, 'cl_particles_refract': 0, 'cl_particles_scorchglow': 0,
                               'rt_metal_gi_ao': 0, 'rt_metal_fog_liquidlight': 0, 'rt_metal_contact': 0,
                               'm5_torch_embers': 0, 'r_skylightning': 0, 'r_caustics': 0},
                               'Best with the whole BEAUTY round reverted (+3.1%, not enough alone)'),
    'tier_best_z_combo':     (T['Best'], {'rt_metal_scale': 0.375, 'rt_metal_fog_scale': 0.3125, 'rt_metal_samples': 3},
                               'THE CANDIDATE: fog buffer + shadow rays, keeping the 24 steps and the pick'),
    'tier_best_z_combo2':    (T['Best'], {'rt_metal_scale': 0.375, 'rt_metal_fog_scale': 0.3125, 'rt_metal_fog_steps': 16,
                               'rt_metal_samples': 3},
                               'the fallback: buffer AND steps'),
    # 2026-09-18: Seb re-cut the target to 110 AND added "while keeping the sharp
    # fog" -- which rules out the four arms above that move rt_metal_fog_scale or
    # _steps, the two levers the 09-06 "even sharper fog definition" verdict bought.
    # These three are the fog-preserving candidates: the fog buffer, the 24 march
    # steps, the light pick, the hybrid, the residual and the filter are untouched
    # in every one of them.
    'tier_best_z_girate2':   (T['Best'], {'rt_metal_scale': 0.375, 'rt_metal_gi_rate': 2},
                               'bounce lighting at half rate -- his own 08-29 verdict was "almost impossible to tell"'),
    'tier_best_z_spatial':   (T['Best'], {'rt_metal_scale': 0.375, 'r_metalfx': 1, 'r_metalfx_reactive': 0},
                               'the temporal scaler down to spatial -- the biggest fog-preserving lever, and a look he liked'),
    'tier_best_z_sharp':     (T['Best'], {'rt_metal_scale': 0.3125, 'rt_metal_gi_rate': 2,
                               'rt_metal_contact': 0, 'r_caustics': 0},
                               'THE FOG-PRESERVING PACKAGE: trace 0.3125 + half-rate bounce + the two dearest BEAUTY levers'),
    # 2026-08-31: r_volumetric_liquidfade's default went 0 -> 0.5, and its static
    # parm compiles into EVERY surface permutation on every install -- so it costs
    # on every tier whether or not water is on screen. It is not a tier lever, so
    # this is the only arm that can price the flip.
    'tier_best_nolf':   (T['Best'], {'r_volumetric_liquidfade': 0},
                          "Best MINUS the 2026-08-31 liquid-fade default flip"),
    'tier_ultimate':    (T['Ultimate'], {}, 'the shipped Ultimate'),
    # 2026-09-05: Ultimate's shipped trace IS 0.375 now (the term was 960x540,
    # four times Best's, for a raster only 2.25x sharper), so the arm that
    # bracketed that question becomes the REVERT control -- the tier_best_ts5
    # and tier_superfast_prev shape.
    'tier_ultimate_ts5': (T['Ultimate'], {'rt_metal_scale': 0.5},
                          'Ultimate at the PRE-WIRING trace 0.5 -- the revert control'),
    # ...and the two single-lever reverts for the same block's fog-step change.
    'tier_best_steps16': (T['Best'], {'rt_metal_fog_steps': 16},
                          'Best at the 2026-09-05 16 fog march steps -- the revert control'),
    'tier_ultimate_steps16': (T['Ultimate'], {'rt_metal_fog_steps': 16},
                          'Ultimate at the 2026-09-05 16 fog march steps -- the revert control'),
    'tier_better':      (T['Better'], {}, 'the shipped Better'),
    'tier_good':        (T['Good'], {}, 'the shipped Good'),
    'tier_stock':       (T['Stock'], {}, 'the shipped Stock -- 1996 GLQuake through Metal'),
    'tier_superfast':   (T['Superfast'], {}, 'the shipped Superfast'),
    # 2026-08-30: Superfast's shipped render scale IS 0.375 now (at Fast's 0.5 it
    # came out only 7-12% above Fast, which is not a tier). This becomes the
    # REVERT control -- Superfast at the pre-wiring 0.5 -- which is what a later
    # block needs to judge the change.
    'tier_superfast_prev': (T['Superfast'], {'rt_metal_fog_froxel': 0,  # historical: the 2D fog history until 2026-09-07
                         'r_viewscale': 0.5},
                          'Superfast at the PRE-WIRING render scale 0.5 -- the revert control'),
    'tier_fast':        (T['Fast'], {}, 'the shipped Fast'),
    'tier_best_r2':     (T['Best'], {'rt_metal_gi_rate': 2},
                         'Best with the GI bounce at HALF rate (G4-1 control)'),
    'tier_best_r4':     (T['Best'], {'rt_metal_gi_rate': 4},
                         'Best with the GI bounce at QUARTER rate (G4-1 control)'),
    'tier_best_nogi':   (T['Best'], {'rt_metal_gi': 0},
                         "Best's levers with bounce lighting off -- isolates GI's cost"),
    'tier_good_gi':     (T['Good'], {'rt_metal_gi': 1},
                         'Good WITH bounce lighting -- the arm rejected 2026-08-29'),
    'tier_good_gi_r2':  (T['Good'], {'rt_metal_gi': 1, 'rt_metal_gi_rate': 2},
                         'Good with bounce lighting at HALF rate'),
    'tier_best_prev':   (T['Best'], {'rt_metal_fog_froxel': 0,  # historical: the 2D fog history until 2026-09-07
                         'r_metalfx': 1, 'r_metalfx_reactive': 0,
                             'rt_metal_lightsample': 1,   # 2026-09-06: historical -- the pick was on
                             'rt_metal_fog_filter': 0, 'rt_metal_gi': 0, 'rt_metal_scale': 0.5,
                             'rt_metal_fog_residual': 0.25,   # historical: the fill until 2026-09-06
                             # 2026-09-04: Best's fog moved (stride 2, buffer 0.5, history
                             # 0.6, the hybrid pick); this historical arm keeps the old fog
                             'rt_metal_fog_stride': 3, 'rt_metal_fog_scale': 0.375,
                             'rt_metal_fog_history': 0.7, 'rt_metal_lightsample_hybrid': 0,
                             # 2026-09-05: Best's steps went 24 -> 16; a HISTORICAL arm
                             # declares the old value rather than following the table
                             'rt_metal_fog_steps': 24},
                         'the pre-2026-08-20 Best (spatial, no mask, no filter, no GI)'),
    # 2026-08-29 tier-targets candidates -- quality steps inside the tier system,
    # keeping the look Seb has settled (temporal + bounce light on Better).
    # 2026-08-29: Better's shipped trace scale IS 0.375 now, so the old ts375
    # candidate would be a duplicate of tier_better. It becomes the REVERT
    # control instead -- Better at the pre-wiring 0.5 -- which is what the
    # fullscreen block needs to judge the change.
    'tier_better_prev':  (T['Better'], {'rt_metal_fog_froxel': 0,  # historical: the 2D fog history until 2026-09-07
                         'rt_metal_scale': 0.5,
                              # 2026-09-05: Better's fog buffer moved 0.375 -> 0.3125; a
                              # HISTORICAL arm declares its own era's value
                              'rt_metal_fog_scale': 0.375},
                          "Better at the PRE-WIRING trace 0.5 -- the revert control"),
    'tier_better_fog3':  (T['Better'], {'rt_metal_fog_scale': 0.3333},
                          "Better with Fast's fog buffer"),
    'tier_better_lean':  (T['Better'], {'rt_metal_samples': 3, 'rt_metal_fog_stride': 4},
                          "Better with Good's shadow settings"),
    # 2026-09-05, the ladder block: the Best->Better rung measured only x1.04 once
    # Best took 16 fog steps, against x1.35 for every step below it. Four tiers
    # allocate the IDENTICAL 480x270 fog buffer and the fog kernel is ~36% of the
    # frame, which is why the upper ladder is flat -- so the rung is evened by
    # giving Better a genuinely smaller fog buffer, the way every working step
    # below it already differs.
    # ...and now that BOTH shipped, the three candidates become the three REVERT
    # controls -- both cells, the trace alone, the buffer alone.
    'tier_better_prev0905': (T['Better'], {'rt_metal_fog_froxel': 0,  # historical: the 2D fog history until 2026-09-07
                         'rt_metal_fog_scale': 0.375, 'rt_metal_scale': 0.375},
                          'Better at the PRE-WIRING fog buffer AND trace -- the full revert'),
    'tier_better_ts375': (T['Better'], {'rt_metal_scale': 0.375},
                          'Better with ONLY the trace given back to 0.375'),
    'tier_better_fogbuf375': (T['Better'], {'rt_metal_fog_scale': 0.375},
                          'Better with ONLY the fog buffer given back to 480x270'),
    # 2026-09-06 evening: Better and Best took the pick BACK (Seb's eye on the fog:
    # "lacking the detail and clumpiness" -- the pick IS that structure), Better's
    # history and steps went to the values he judged, and the top three march 24
    # steps again. The afternoon's arms invert into the revert controls.
    'tier_better_ls0':   (T['Better'], {'rt_metal_lightsample': 0},
                          'Better with ONLY the fog light pick off -- the revert control'),
    'tier_better_prev0906': (T['Better'], {'rt_metal_fog_froxel': 0,  # historical: the 2D fog history until 2026-09-07
                         'rt_metal_lightsample': 0, 'rt_metal_fog_history': 0.8,
                              'rt_metal_fog_steps': 16},
                          'Better as it shipped on the 2026-09-06 afternoon -- the full revert'),
    # 2026-09-01: Best's shipped trace IS 0.375 now, so the arm that bracketed
    # that step becomes the REVERT control -- Best at the pre-wiring 0.5.
    'tier_best_ts5':     (T['Best'], {'rt_metal_scale': 0.5},
                          'Best at the PRE-WIRING trace 0.5 -- the revert control'),
    # 2026-09-04: Best's fog took Seb's ladder verdict (buffer 0.5, casts every 2nd
    # step, history 0.6, the hybrid pick). This is Best with ONLY the fog reverted
    # -- the arm that prices that verdict on its own.
    'tier_best_fogprev': (T['Best'], {'rt_metal_scale': 0.375, 'rt_metal_fog_froxel': 0,  # historical: the 2D fog history until 2026-09-07
                         'rt_metal_fog_stride': 3, 'rt_metal_lightsample': 1,   # 09-06: historical, pick was on
                              'rt_metal_fog_history': 0.7, 'rt_metal_lightsample_hybrid': 0,
                              'rt_metal_fog_steps': 24},   # historical: see tier_best_prev
                          "Best at the PRE-2026-09-04 fog -- the revert control"),
    # ...and the two SINGLE-LEVER give-backs the 2026-09-04 block exists to rank:
    # the buffer alone (x1.78 texels) and the stride alone (x1.5 shadow casts).
    # 2026-09-04 evening: the bench gave the buffer back (58% of the fog verdict's
    # cost, on the one lever Seb was least sure of), so the arm that bracketed that
    # step becomes the revert control -- the tier_best_ts5 shape.
    'tier_best_fogbuf05': (T['Best'], {'rt_metal_fog_scale': 0.5},
                          "Best at the PRE-give-back fog buffer 0.5 -- the revert control"),
    # 2026-09-06: Best took stride 3 and the pick off on Seb's word (+9.4% and
    # +13.6% measured 09-05), so the stride arm inverts into the revert control
    # and the pick gets one of its own.
    'tier_best_stride2': (T['Best'], {'rt_metal_fog_stride': 2},
                          "Best at the PRE-WIRING stride 2 -- the revert control"),
    # 2026-09-06 evening, "sharp1": the residual fill 0.25 -> 0.1 (a new lever, identical
    # on every tier) and the 3x3 filter on the top three; the pair reverts here.
    'tier_better_sharp0': (T['Better'], {'rt_metal_fog_residual': 0.25, 'rt_metal_fog_filter': 2},
                          'Better at the PRE-sharp1 fog (fill 0.25, the 5x5) -- the revert control'),
    'tier_best_sharp0':  (T['Best'], {'rt_metal_fog_residual': 0.25, 'rt_metal_fog_filter': 2},
                          'Best at the PRE-sharp1 fog (fill 0.25, the 5x5) -- the revert control'),
    'tier_best_prev0906': (T['Best'], {'rt_metal_scale': 0.375, 'rt_metal_fog_froxel': 0,  # historical: the 2D fog history until 2026-09-07
                         'rt_metal_lightsample': 0, 'rt_metal_fog_steps': 16,
                              'rt_metal_fog_residual': 0.25, 'rt_metal_fog_filter': 2},
                          'Best as it shipped on the 2026-09-06 afternoon -- the full revert'),
    'tier_best_ls0':     (T['Best'], {'rt_metal_lightsample': 0},
                          "Best with the fog light pick OFF (the 2026-09-06 afternoon cell) -- the revert control"),
    # SEPTEMBER2 A5 (2026-09-08): the froxel priced on each tier that carries it (the 2026-09-07
    # default flip owed a soaked block), and the pick's cheaper forms under the froxel.
    'tier_best_froxel0':     (T['Best'], {'rt_metal_fog_froxel': 0},
                              'Best at the PRE-2026-09-07 2D fog history -- prices the froxel default'),
    'tier_better_froxel0':   (T['Better'], {'rt_metal_fog_froxel': 0},
                              'Better at the PRE-2026-09-07 2D fog history -- prices the froxel default'),
    'tier_good_froxel0':     (T['Good'], {'rt_metal_fog_froxel': 0},
                              'Good at the PRE-2026-09-07 2D fog history -- prices the froxel default'),
    'tier_ultimate_froxel0': (T['Ultimate'], {'rt_metal_fog_froxel': 0},
                              'Ultimate at the PRE-2026-09-07 2D fog history -- prices the froxel default'),
    'tier_best_hybrid2':     (T['Best'], {'rt_metal_lightsample_hybrid': 2},
                              'Best at the PRE-A5 alternating hybrid 2 -- the revert of the single-pass pick (wired 2026-09-13)'),
    'tier_best_stride3':     (T['Best'], {'rt_metal_fog_stride': 3},
                              'Best at the PRE-A5 cast stride 3 -- the revert (wired 4 on 2026-09-13)'),
    'tier_best_stride6':     (T['Best'], {'rt_metal_fog_stride': 6},
                              'Best casting every 6th froxel cell (A5 ceiling; +2.5% forward-hold bias measured)'),
    'tier_ultimate_hybrid2': (T['Ultimate'], {'rt_metal_lightsample_hybrid': 2},
                              'Ultimate at the PRE-A5 alternating hybrid 2 -- the revert of the single-pass pick (wired 2026-09-13)'),
    'tier_ultimate_stride2': (T['Ultimate'], {'rt_metal_fog_stride': 2},
                              'Ultimate at the PRE-A5 cast stride 2 -- the revert (wired 4 on 2026-09-13)'),
    'tier_ultimate_ls0':     (T['Ultimate'], {'rt_metal_lightsample': 0},
                              'Ultimate with the fog light pick OFF (A5 ceiling for the pick)'),
    'tier_best_0821':    (T['Best'], {'rt_metal_fog_froxel': 0,  # historical: the 2D fog history until 2026-09-07
                         'rt_metal_gi': 0, 'rt_metal_lightsample': 0,
                              'rt_metal_fog_residual': 0.25,   # historical: the fill until 2026-09-06
                              'rt_metal_fog_beams': 0.1, 'rt_metal_scale': 0.5,
                              # 2026-09-04: and the fog as it was on 08-21 (see tier_best_prev)
                              'rt_metal_fog_stride': 3, 'rt_metal_fog_scale': 0.375,
                              'rt_metal_fog_history': 0.7, 'rt_metal_lightsample_hybrid': 0,
                              'rt_metal_fog_steps': 24},   # historical: see tier_best_prev
                          "the time machine: Best's levers with every post-08-21 "
                          'per-frame change reverted -- the two that are levers '
                          'here, the four that are not in ALLOWED_EXTRA'),
    # 2026-09-06: the Better->Good rung (x1.159) was structural -- Good allocated
    # more of BOTH buffers than Better and traced LARGER than Best (640x360 against
    # 480x270). Measured paired on two beds: Good x1.34 of Best as shipped, with
    # Best's trace x1.47, with Better's 400x225 fog buffer x1.48, with both x1.64.
    # Seb wired both, so the three candidates become the three REVERT controls --
    # the Better precedent of 2026-09-05, cell for cell.
    'tier_good_prev0906': (T['Good'], {'rt_metal_fog_froxel': 0,  # historical: the 2D fog history until 2026-09-07
                         'rt_metal_scale': 0.5, 'rt_metal_fog_scale': 0.375},
                          'Good at the PRE-WIRING trace AND fog buffer -- the full revert'),
    'tier_good_ts5':     (T['Good'], {'rt_metal_scale': 0.5},
                          'Good with ONLY the trace given back to 0.5 (640x360)'),
    'tier_good_fogbuf375': (T['Good'], {'rt_metal_fog_scale': 0.375},
                          'Good with ONLY the fog buffer given back to 480x270'),
    'tier_ultimate_samples4': (T['Ultimate'], {'rt_metal_samples': 4},
                          'Ultimate closer: shadow samples 6->4'),
    'tier_ultimate_stride3': (T['Ultimate'], {'rt_metal_fog_stride': 3},
                          'Ultimate closer: fog shadow stride 2->3'),
    # 2026-09-13: SEPTEMBER2 A5 wired on Seb's eye ("exec a5_both.cfg looks great in
    # practice -- so let's use that"): hybrid 3 + stride 4 on every pick-running tier.
    # The pre-wiring cells become the revert controls, the tier_best_stride2 shape.
    'tier_best_a5off':     (T['Best'], {'rt_metal_lightsample_hybrid': 2, 'rt_metal_fog_stride': 3},
                          'Best at the PRE-A5 pick (hybrid 2, stride 3) -- the revert control'),
    'tier_better_a5off':   (T['Better'], {'rt_metal_lightsample_hybrid': 2, 'rt_metal_fog_stride': 3},
                          'Better at the PRE-A5 pick (hybrid 2, stride 3) -- the revert control'),
    'tier_ultimate_a5off': (T['Ultimate'], {'rt_metal_lightsample_hybrid': 2, 'rt_metal_fog_stride': 2},
                          'Ultimate at the PRE-A5 pick (hybrid 2, stride 2) -- the revert control'),
    # --- phase AA, the 2026-09-19 AA retier's live controls (bed demo48) ---------
    'tier_ultimate_now':      (T['Ultimate'], {},
                              'the shipped Ultimate on the phase-AA bed -- the paired reference'),
    'tier_ultimate_prev0919': (T['Ultimate'], {'rt_metal_shadowlights': 1, 'rt_metal_scale': 0.375, 'r_viewscale': 1,
                              'rt_metal_samples': 6, 'rt_metal_fog_scale': 0.25, 'r_smaa': 0,
                              'rt_metal_fog_froxel_slices': 48},
                              'Ultimate as it shipped up to 2026-09-18 -- the full revert control'),
    'tier_best_prev0919':     (T['Best'], {'rt_metal_shadowlights': 1, 'rt_metal_scale': 0.3125, 'r_viewscale': 0.667,
                              'rt_metal_samples': 4, 'rt_metal_fog_scale': 0.375, 'r_smaa': 0,
                              'rt_metal_fog_froxel_slices': 48},
                              'Best as it shipped up to 2026-09-18 -- the tier Seb actually played'),
    'tier_ultimate_smaa0':    (T['Ultimate'], {'r_smaa': 0},
                              'the new Ultimate with MLAA off -- prices the AA pass at the new geometry'),
    'tier_ultimate_fxaa1':    (T['Ultimate'], {'r_fxaa': 1},
                              "the new Ultimate with Seb's archived r_fxaa 1 back -- prices the pin"),
    'tier_ultimate_bare':     (T['Ultimate'], {'cl_particles_texsize': 64, 'cl_particles_blood_droplet': 0,
                              'cl_particles_soft': 0, 'cl_particles_refract': 0, 'cl_particles_scorchglow': 0,
                              'rt_metal_gi_ao': 0, 'rt_metal_fog_liquidlight': 0, 'rt_metal_contact': 0,
                              'm5_torch_embers': 0, 'r_skylightning': 0, 'r_caustics': 0},
                              "the new Ultimate with rich.cfg's eleven BEAUTY extras off -- the one deviation"),
}

# 2026-09-19, THE AA RETIER: every column of m5_quality_levers[] moved (the RT term
# is pixel-exact with the raster on every tier, the raster is 0.375 everywhere, and
# r_smaa / r_fxaa / r_fxaa_post / rt_metal_fog_froxel_slices became levers). The arms
# listed here were all written against the PRE-retier table, so comparing them to the
# live one would report ~60 divergences that say nothing: an arm named "Best at the
# PRE-WIRING trace 0.5" cannot be re-derived from a Best that no longer has a trace
# scale of its own. THEY ARE KEPT, NOT DELETED -- each is the only record of the
# configuration its block measured, and several are cited by figures in SETTINGS.md
# and CLAUDE.md. They are simply not checked against a table they predate.
#
# RE-RUNNING ANY OF THEM IS STILL VALID (their console lines are complete and pinned);
# what is NOT valid is reading one as "tier X plus a lever" any more. The live controls
# for the new table are the phase-AA arms at the foot of levers.tsv.
FROZEN = {
    'tier_best_0821', 'tier_best_a5off', 'tier_best_fogbuf05', 'tier_best_fogprev',
    'tier_best_froxel0', 'tier_best_hybrid2', 'tier_best_ls0', 'tier_best_nogi',
    'tier_best_nolf', 'tier_best_prev', 'tier_best_prev0906', 'tier_best_r2', 'tier_best_r4',
    'tier_best_sharp0', 'tier_best_steps16', 'tier_best_stride2', 'tier_best_stride3',
    'tier_best_stride6', 'tier_best_ts375', 'tier_best_ts5', 'tier_best_z_beautyoff',
    'tier_best_z_combo', 'tier_best_z_combo2', 'tier_best_z_fogbuf', 'tier_best_z_girate2',
    'tier_best_z_ref', 'tier_best_z_samples3', 'tier_best_z_sharp', 'tier_best_z_spatial',
    'tier_best_z_steps16', 'tier_best_z_ts3125', 'tier_better_a5off', 'tier_better_fog3',
    'tier_better_fogbuf375', 'tier_better_froxel0', 'tier_better_lean', 'tier_better_ls0',
    'tier_better_prev', 'tier_better_prev0905', 'tier_better_prev0906', 'tier_better_sharp0',
    'tier_better_ts375', 'tier_good_fogbuf375', 'tier_good_froxel0', 'tier_good_gi',
    'tier_good_gi_r2', 'tier_good_prev0906', 'tier_good_ts5', 'tier_superfast_prev',
    'tier_superfast_spatial', 'tier_ultimate_a5off', 'tier_ultimate_froxel0',
    'tier_ultimate_hybrid2', 'tier_ultimate_ls0', 'tier_ultimate_samples4',
    'tier_ultimate_steps16', 'tier_ultimate_stride2', 'tier_ultimate_stride3',
    'tier_ultimate_ts5'
}

# cvars a tier arm may carry that are NOT menu levers (bench pins).
ALLOWED_EXTRA = {'r_viewfbo',
                 # tier_best_0821's reverts -- none of these is a menu lever;
                 # they are the defaults that landed AFTER the 08-21 bench.
                 'rt_metal_term_upsample',
                 'rt_metal_viewmodel', 'rt_metal_refit',
                 'rt_metal_fog_stride_adaptive'}


def parse_menu():
    src = open(MENU, encoding='utf-8', errors='replace').read()
    i = src.index('m5_quality_levers[] =')
    body = src[i:src.index('\n};', i)]
    table, skipped = {}, []
    for m in re.finditer(
            r'\{\s*&(\w+)\s*,\s*\{([^}]*)\}\s*\}', body):
        name, vals = m.group(1), m.group(2)
        nums = re.findall(r'-?\d+(?:\.\d+)?', vals)
        if len(nums) != len(TIERS):
            # A row that does not parse as one value per tier would be SILENTLY absent
            # from every comparison below -- the same shape as the coverage
            # failure this checker exists to catch. Say so loudly instead.
            skipped.append('%s (%d values, expected %d)' % (name, len(nums), len(TIERS)))
            continue
        table[name] = [float(x) for x in nums]
    if skipped:
        print('check-tiers: WARNING -- unparsed lever rows in menu.c: %s'
              % ', '.join(skipped))
    return table


def parse_levers():
    arms = {}
    for line in open(LEVERS, encoding='utf-8'):
        if line.startswith('#'):
            continue
        p = line.rstrip('\n').split('\t')
        if len(p) < 4 or not p[0].startswith('tier_'):
            continue
        cv = {}
        for chunk in p[3].split(';'):
            chunk = chunk.strip()
            if not chunk:
                continue
            bits = chunk.split()
            if len(bits) == 2:
                cv[bits[0]] = float(bits[1])
        arms[p[0]] = cv
    return arms


def markdown():
    """Emit SETTINGS.md's player-facing lever grid straight from menu.c.

    The grid had silently lost four shipped rows (lightsample, fog_beams, gi,
    gi_rate) by being hand-maintained; regenerate it instead of editing it."""
    table = parse_menu()
    src = open(MENU, encoding='utf-8', errors='replace').read()
    i = src.index('m5_quality_levers[] =')
    body = src[i:src.index('\n};', i)]
    order = [m.group(1) for m in re.finditer(r'\{\s*&(\w+)\s*,\s*\{([^}]*)\}\s*\}', body)
             if m.group(1) in table]

    def fmt(v):
        return str(int(round(v))) if abs(v - round(v)) < 1e-9 else ('%.3f' % v).rstrip('0')

    print('| Setting | %s |' % ' | '.join(TIERS))
    print('|' + '---|' * (len(TIERS) + 1))
    for k in order:
        cells = [fmt(v) for v in table[k]]
        # bold a lever that actually differentiates, so the grid shows at a
        # glance which rows carry a tier's identity and which merely pin
        if len(set(cells)) > 1:
            cells = ['**%s**' % c for c in cells]
        print('| `%s` | %s |' % (k, ' | '.join(cells)))
    return 0


def distinct(table):
    """Assert the tiers are PAIRWISE DISTINCT.

    M5_DetectQuality returns the FIRST tier every lever matches, so two tiers
    with identical values would make the later one unreachable — the menu would
    show the earlier name and a player could never land on the other. Nothing
    else checks this, and it becomes easy to trip the moment tiers are inserted
    or a lever is flattened across the table."""
    bad = 0
    for i in range(len(TIERS)):
        for j in range(i + 1, len(TIERS)):
            if all(abs(v[i] - v[j]) <= 0.001 for v in table.values()):
                print('  COLLISION: %s and %s have identical lever sets — '
                      'the detector can never return %s'
                      % (TIERS[i], TIERS[j], TIERS[j]))
                bad += 1
    return bad


def cvar_defaults(table):
    """The COMPILED default of each lever, read from its cvar_t declaration.

    Needed because Cvar_WriteVariables omits an archived cvar that sits at its
    default, so an absent lever in a config is the default, not a mismatch."""
    out = {}
    src = open(os.path.join(ROOT, 'vid_sdl.c'), encoding='utf-8', errors='replace').read()
    src += open(os.path.join(ROOT, 'gl_rmain.c'), encoding='utf-8', errors='replace').read()
    # 2026-09-06: the Stock tier's two levers live elsewhere (m5_stock beside
    # m5_packlight in r_shadow.c so the dedicated server links; cl_particles_quake
    # is upstream's, in cl_particles.c) -- without these two the reader called every
    # real config "off by 2 levers" and could not name a tier.
    src += open(os.path.join(ROOT, 'r_shadow.c'), encoding='utf-8', errors='replace').read()
    src += open(os.path.join(ROOT, 'cl_particles.c'), encoding='utf-8', errors='replace').read()
    for name in table:
        m = re.search(r'cvar_t\s+%s\s*=\s*\{[^}]*?"%s"\s*,\s*"([-\d.]+)"' % (name, name), src)
        if m:
            out[name] = float(m.group(1))
    return out


def check_config(path):
    """Report which tier a config.cfg detects as, the way M5_DetectQuality does.

    THE RELEASE SHIPS A CONFIG AS A NEW MACHINE'S DEFAULTS, and a config that
    matches no tier makes the M5 Quality row read "Custom" on a fresh install --
    a poor first impression, and silent. This is that check, executable rather
    than a comment, so it cannot rot the way the settings tables did.
    """
    table = parse_menu()
    if not table:
        print('check-tiers: cannot parse menu.c'); return 2
    cfg = {}
    try:
        fh = open(path, encoding='utf-8', errors='replace')
    except OSError as e:
        print('check-tiers: cannot read %s (%s)' % (path, e)); return 2
    with fh:
        for line in fh:
            m = re.match(r'^"([^"]+)"\s+"([^"]+)"', line)
            if m:
                try:
                    cfg[m.group(1)] = float(m.group(2))
                except ValueError:
                    pass
    # An ARCHIVED cvar sitting at its default is omitted from config.cfg
    # entirely (Cvar_WriteVariables skips it), so an absent lever is not a
    # mismatch -- it is the compiled default, which we read from the engine.
    defaults = cvar_defaults(table)
    unknown = [k for k in table if k not in cfg and k not in defaults]
    best, bestmiss = None, None
    for ti, tname in enumerate(TIERS):
        miss = []
        for k, v in sorted(table.items()):
            have = cfg.get(k, defaults.get(k))
            if have is None or abs(have - v[ti]) > 0.001:
                miss.append((k, have, v[ti]))
        if not miss:
            print('check-tiers: %s detects as %s' % (path, tname)); return 0
        if bestmiss is None or len(miss) < len(bestmiss):
            best, bestmiss = tname, miss
    print('check-tiers: %s matches NO tier -- the M5 Quality row would read "Custom"' % path)
    if unknown:
        print('  (could not resolve a default for: %s)' % ', '.join(unknown))
    print('  closest is %s, off by %d lever(s):' % (best, len(bestmiss)))
    for k, have, want in bestmiss[:8]:
        print('    %-28s config %s, %s wants %g'
              % (k, ('absent' if have is None else '%g' % have), best, want))
    return 1


def selftest():
    """Synthesise a config for each tier and assert --config names it back.

    This covers the whole path the release build depends on -- parse menu.c,
    resolve archived cvars that are ABSENT because they sit at their compiled
    default, compare within slop -- without needing anybody's real config. It
    also catches the default-resolution regex silently failing, which would make
    every config look like a mismatch.
    """
    table = parse_menu()
    if not table:
        print('check-tiers: selftest cannot parse menu.c'); return 1
    import tempfile
    bad = 0
    for ti, tname in enumerate(TIERS):
        # OMIT any lever whose tier value equals its compiled default, exactly as
        # Cvar_WriteVariables does. Writing all 22 explicitly made this test
        # vacuous: it never exercised default resolution, which is the fragile
        # half, and a deliberately broken resolver still passed it.
        defs = cvar_defaults(table)
        omitted = 0
        with tempfile.NamedTemporaryFile('w', suffix='.cfg', delete=False) as fh:
            for k, v in sorted(table.items()):
                if k in defs and abs(defs[k] - v[ti]) <= 0.001:
                    omitted += 1
                    continue
                fh.write('"%s" "%g"\n' % (k, v[ti]))
            path = fh.name
        import io as _io, contextlib
        buf = _io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = check_config(path)
        os.unlink(path)
        got = buf.getvalue().strip()
        if omitted == 0:
            # A DEAD DEFAULT RESOLVER OMITS NOTHING, which writes all 22 levers
            # explicitly and makes this test pass while covering nothing -- the
            # fixture degrading into a tautology. Every tier legitimately has
            # levers sitting at their compiled default, so zero is the signature
            # of the resolver failing, not of a config that happens to be
            # explicit. Measured when this assertion was added: 7 to 12 omitted
            # per tier.
            print('  selftest %-10s FAILED: nothing omitted -- the default '
                  'resolver is dead and this test would be vacuous' % tname)
            bad += 1
        elif rc == 0 and got.endswith('detects as ' + tname):
            print('  selftest %-10s OK (%d lever(s) omitted as default)' % (tname, omitted))
        else:
            print('  selftest %-10s FAILED: %s' % (tname, got.splitlines()[0] if got else 'no output'))
            bad += 1
    print('check-tiers: selftest %s' % ('PASS' if not bad else 'FAIL (%d)' % bad))
    return 1 if bad else 0


def main():
    if '--markdown' in sys.argv:
        return markdown()
    if '--selftest' in sys.argv:
        return selftest()
    if '--config' in sys.argv:
        return check_config(sys.argv[sys.argv.index('--config') + 1])
    table, arms = parse_menu(), parse_levers()
    if not table:
        print('check-tiers: FAILED to parse m5_quality_levers[] from menu.c')
        return 1
    print('check-tiers: menu.c table has %d levers, %d tiers; levers.tsv has %d tier arms'
          % (len(table), len(TIERS), len(arms)))
    bad = distinct(table)
    nfrozen = 0
    for arm in sorted(arms):
        if arm in FROZEN:
            nfrozen += 1
            continue
        if arm not in EXPECT_DIFFERS:
            print('  %-18s UNDECLARED -- add it to EXPECT_DIFFERS with a reason' % arm)
            bad += 1
            continue
        tier, override, reason = EXPECT_DIFFERS[arm]
        want = {k: v[tier] for k, v in table.items()}
        want.update(override)
        got = arms[arm]
        missing = [k for k in want if k not in got]
        wrong = [(k, got[k], want[k]) for k in want
                 if k in got and abs(got[k] - want[k]) > 0.001]
        extra = [k for k in got if k not in want and k not in ALLOWED_EXTRA]
        if missing or wrong or extra:
            bad += 1
            print('  %-18s DIVERGES (%s)' % (arm, reason))
            for k in missing:
                print('      missing lever %-26s (tier %s wants %g)'
                      % (k, TIERS[tier], want[k]))
            for k, g, w in wrong:
                print('      wrong value   %-26s arm %g, tier %s wants %g'
                      % (k, g, TIERS[tier], w))
            for k in extra:
                print('      not a lever   %-26s (arm sets %g)' % (k, got[k]))
        else:
            print('  %-18s OK (%s = %s)' % (arm, TIERS[tier], reason))
    if nfrozen:
        print('  (%d arm(s) FROZEN as pre-2026-09-19 records -- see FROZEN above)' % nfrozen)
    if bad:
        print('check-tiers: FAIL -- %d arm(s) diverge from the shipped table' % bad)
        return 1
    print('check-tiers: PASS -- every live tier arm mirrors menu.c')
    return 0


if __name__ == '__main__':
    sys.exit(main())
