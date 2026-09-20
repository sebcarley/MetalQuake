#!/bin/sh
# M5 smoke tests — headless end-to-end checks of the engine + m5 QuakeC.
#
# Boots the real engine (windowed, no sound), drives it with deferred console
# commands, and asserts on the console log and on stills.
# Run from anywhere:  sh tests/smoke.sh
# In Xcode: Cmd+U on the QuakeM5 scheme (tests/QuakeM5Tests.m wraps this script
# and matches checks BY THEIR PRINTED NAME — a check's string is an API; rename
# one and its Xcode test silently stops asserting it).
#
# RATIONALISED 2026-08-21: boots that shared a compatible configuration were
# merged, because the F3/F4 lesson cuts that way — a validation boot is only as
# good as the configuration it boots, and the all-live boots are the ones that
# caught the shipped defects the narrow ones missed. The run LETTERS are kept
# (CLAUDE.md's records cite them); a letter whose boot was absorbed says so at
# its old position. The one new bed is run Z: Seb's demo16 (e3m3, real play)
# played back through the whole live stack on the default renderer — the
# closest thing the suite has to an hour of his session in twenty seconds.
#
# The suite, by run:
#   builds  make sdl-release + fteqcc
#   A   horde mode (dm4): director, wave 1, spawn placement, notarget brawl
#   B   gore / Doom shotgun / impulses / photo mode / noclip-fly (absorbed I2)
#   D   the GL+sidecar bridge, all-live (absorbed run C): fog kernel stack,
#       view-model depth mask at non-unit gamma, term dump, open-sky instance
#   E   the M5 thunderbolt, including the master gate (absorbed run F via a
#       marker-scoped absent check)
#   G   menu row self-checks, idle sway, maplights statistic
#   H   Scrag venom + its master gate (two boots — the QC print is first-event
#       per map, so a mid-boot toggle cannot test the gate)
#   H2  ball lightning, the ninth weapon + its master gate (two boots, same
#       first-event reasoning as H); the knot and the effectinfo handles
#   I   the mission packs + Arcane Dimensions (skip when not installed)
#   J   the big Metal round trip: probes, permutations, counters
#   K   gl escape hatch persists + the analytic gamma curve (absorbed D2)
#   K2  the default boot lands on Metal + MetalFX probes + EDR report
#       (absorbed J3)
#   Z   demo16: the all-live playback on the default renderer (NEW)
#   Q   ONE Metal-API-validation boot with everything live (absorbed J4, J5
#       and the constant-struct ceiling bed): temporal + reactive + fog
#       kernel + filter modes + liquids sampler + analytic gamma + a spatial
#       re-key
#   J6  the frame-pool leak rate net (heap sampling, 25 s)
#   J2  a 2D frame reaches the drawable (VID_METAL_PROBE)
#   RT  the cross-backend term-buffer identity (two boots by necessity)
#   L   the emissive liquid instance (e1m1 slime)
#   P   blue-noise jitter + the IGN arm (one boot, marker-scoped)
#   N   the SDF bolt reaches the screen
#   O   e1m7: the lava crust flows (absorbed D3) + the heat haze marches
#   M   the KH swirl stills (one frozen boot, four shots, cleared notify)
#   T   the torch: envelope/backstop/colour + the lamp lights the room
#       (one frozen boot, two shots)
# (The number of checks is NOT recorded here on purpose: it was hand-maintained,
# it rotted, and by 2026-08-05 this header said 30, CLAUDE.md said 15 and
# METAL.md said 59 while the real figure was 63. The verdict line below counts
# them, so the only quotable total is the one a run just printed.)
cd "$(dirname "$0")/.." || exit 1

fail=0
npass=0
nfail=0
pass() { npass=$((npass + 1)); printf 'PASS: %s\n' "$1"; }
failt() { nfail=$((nfail + 1)); printf 'FAIL: %s\n' "$1"; fail=1; }
check()  { if grep -qE "$2" "$3"; then pass "$1"; else failt "$1"; fi; }
absent() { if grep -qE "$2" "$3"; then failt "$1"; else pass "$1"; fi; }

LOG_A=$(mktemp); LOG_B=$(mktemp)

# Run the engine against a throwaway user directory. Without this the tests use the
# real ~/Library/Application Support/darkplaces, and the quit-time config save
# persists test cvars into it — run C sets v_gamma, which is CF_ARCHIVE, so running
# the suite silently changed the player's saved gamma. Game data still resolves from
# basedir, so id1/ and m5/ are unaffected.
SANDBOX=$(mktemp -d)
# DP_EXTRA lets a caller add cvars/arguments to every run, e.g.
#   DP_EXTRA="+r_volumetric 1" sh tests/smoke.sh
#
# +vid_renderer gl is PINNED (Phase 8): runs A-I were all authored against the
# GL renderpath -- C and D specifically test the GL+sidecar IOSurface bridge --
# and when vid_renderer's default flipped to metal, an inherited default would
# have silently moved every one of them onto the other backend. A pin cannot
# move under a bed; a default can. The Metal-path runs (J, J2, J3, J4, RT, K)
# set their renderer explicitly and are unaffected.
DP="./darkplaces-sdl -userdir $SANDBOX +vid_renderer gl $DP_EXTRA"

trap 'rm -f "$LOG_A" "$LOG_B" m5/smoketest_a.cfg m5/smoketest_b.cfg m5/smoketest_d.cfg m5/smoketest_e.cfg m5/smoketest_g.cfg m5/smoketest_h.cfg m5/smoketest_h2.cfg m5/smoketest_i.cfg m5/smoketest_j.cfg m5/smoketest_j2.cfg m5/smoketest_j6.cfg m5/smoketest_k.cfg m5/smoketest_k2.cfg m5/smoketest_n.cfg m5/smoketest_q.cfg m5/maps/e1m3.pts; rm -rf "$SANDBOX"' EXIT

# --- 1: builds -------------------------------------------------------------
if make sdl-release -j8 >"$LOG_A" 2>&1; then pass "engine builds (make sdl-release)"; else failt "engine builds (make sdl-release)"; fi
if [ -x tools/fteqcc/fteqcc ]; then
	if sh qc/build.sh >"$LOG_A" 2>&1; then pass "m5 QuakeC compiles"; else failt "m5 QuakeC compiles"; fi
else
	echo "SKIP: m5 QuakeC compile (tools/fteqcc missing; using existing m5/progs.dat)"
fi
[ -f m5/progs.dat ] || { failt "m5/progs.dat exists"; echo "cannot continue"; exit 1; }

# The M5 Quality tier arms in test/perf/levers.tsv must BE the shipped tiers
# (menu.c's m5_quality_levers[]). They have silently stopped being so twice --
# four omitted levers on every arm, and a blanket "rate 1 ships" rewrite that
# turned the two HALF-RATE GI control arms into duplicates of their full-rate
# parents -- and neither failure is visible in a results.tsv: the sweep runs
# happily and reports numbers for an arm that is not the thing it is named
# after. A static check, so it costs no boot.
if python3 test/perf/check-tiers.py >/dev/null 2>&1; then pass "perf tier arms mirror the shipped table"; else failt "perf tier arms mirror the shipped table"; fi

# The release ships a config.cfg as a NEW machine's defaults, and one that
# matches no tier makes Options -> M5 Quality read "Custom" on first launch --
# silently, and it nearly shipped that way. make-release.sh now refuses such a
# config; this covers the CHECKER itself (parse the table, resolve archived
# cvars that are absent because they sit at their compiled default, compare
# within slop) by round-tripping a synthesised config for every tier.
if python3 test/perf/check-tiers.py --selftest >/dev/null 2>&1; then pass "tier detection round-trips every tier"; else failt "tier detection round-trips every tier"; fi

# The documents carry a large TRANSCRIPTION surface -- SETTINGS.md quotes ~170
# cvar defaults by hand, CLAUDE.md quotes the XCTest and vantage counts, and
# three files carry the tier table -- and every one of those is a fact about the
# source copied into prose. It rots: the 2026-08-18 audit corrected two counts
# and by 2026-08-30 one had drifted again (46 against 58), rt_metal_gi_albedo_tex
# still read "Default 0" a day after it shipped at 1, five "all four tiers"
# phrases survived into a six-tier table, and a blockquote WARNING about stale
# values was itself stale in four of its nine. docsync checks everything with a
# machine-readable shape and generates what it can. Static, so it costs no boot.
if python3 test/docsync.py >/dev/null 2>&1; then pass "docs match the tree (docsync)"; else failt "docs match the tree (docsync)"; fi

# --- run A: horde mode -----------------------------------------------------
cat > m5/smoketest_a.cfg <<'EOF'
sv_cheats 1
# early, and with god on: M5Horde_Think returns before the hidden-player block
# if the player is dead, so a wave that got a kill in first would make the
# notarget check below flake rather than fail honestly
defer 3 "god"
defer 5 "notarget"
defer 22 "m5_horde 0"
defer 22 "m5_horde_director 0"
defer 23 quit
EOF
# SEPTEMBER2 H: the director is on for this boot (its wave-1 line is the check)
# and switched off again before the quit -- it is CF_ARCHIVE and this sandbox is
# shared, so run H's absent-check below depends on that line.
$DP -window -nosound +developer 1 +m5_horde 1 +m5_horde_director 1 +map dm4 +exec smoketest_a.cfg >"$LOG_A" 2>&1

check  "m5 gamedir auto-mounts"            "using gamedirs.*id1.*m5"   "$LOG_A"
check  "horde director starts"             "M5 HORDE MODE"             "$LOG_A"
check  "horde wave 1 announces"            "=== Wave 1 ==="            "$LOG_A"
check  "horde director shapes wave 1"      "M5 horde: director wave 1 budget [0-9]+% score" "$LOG_A"
absent "no bad horde spawn placements"     "badly placed|walkmonster in wall" "$LOG_A"
# notarget reaches horde monsters at all. They are woken by M5H_Aggro calling
# FoundTarget() directly, which never passes FindTarget's FL_NOTARGET test, so
# without the hidden-player handling this line never appears.
check  "horde notarget engages the brawl"  "player hidden - monsters will brawl" "$LOG_A"

# --- run B: gore / shotgun / impulses / photo mode / noclip-fly --------------
# (dm4 has no map monsters. Absorbed run I2, 2026-08-21: the noclip-fly
# sequence rides the tail of the same boot -- same map, same cheats, and the
# probe markers scope its measurement to its own window.)
cat > m5/smoketest_b.cfg <<'EOF'
sv_cheats 1
defer 2 "m5_gore 2"
defer 3 "cl_particles_quality"
defer 4 "m5_gore 0"
defer 5 "cl_particles_quality"
defer 6 "m5_shotgun 1"
defer 6.2 "m5_shotgun_casing 1"
// the flash is DEFAULT 1 since 2026-09-16; stated anyway (the run depends on it), and
// never toggled back to 0 here -- an archived 0 would follow every later run round the
// shared sandbox
defer 6.3 "m5_muzzleflash 1"
defer 7 "impulse 2"
defer 8 "+attack"
defer 9 "-attack"
defer 10 "impulse 222"
defer 11 "impulse 224"
defer 12 "photomode"
defer 14 "photomode"
defer 15 "m5_shotgun 0"
defer 15.1 "m5_shotgun_casing 0"
defer 15.2 "god"
defer 15.3 "m5_explosion_sprite 0"
defer 15.4 "impulse 9"
defer 15.5 "impulse 7"
defer 15.6 "+attack"
defer 15.7 "-attack"
defer 16 "noclip"
defer 17 "+lookup"
defer 17.3 "-lookup"
defer 19 "echo NOCLIPFLY-BEFORE"
defer 19 "r_volumetric_probe"
defer 19.2 "+forward"
defer 20.2 "-forward"
defer 20.5 "echo NOCLIPFLY-AFTER"
defer 20.5 "r_volumetric_probe"
defer 21 "m5_explosion_sprite 1"
// BEAUTY A2: the atlas re-generates live, DOWN to 64 and back up to 256. The
// order matters since 256 became the DEFAULT (2026-09-17): the 2048 line now
// prints at boot too, so the 512 line is what proves a live re-generation, and
// ending at 256 leaves the shared sandbox at the default (CF_ARCHIVE writes only
// a value that differs, so nothing is archived for the runs after this one).
defer 21.1 "cl_particles_texsize 64"
defer 21.2 "cl_particles_texsize 256"
defer 21.5 quit
EOF
$DP -window -nosound +developer 1 +map dm4 +exec smoketest_b.cfg >"$LOG_B" 2>&1

check "gore preset 2 applies quality 3"    "quality.* is \"3\""        "$LOG_B"
check "gore preset 0 restores quality 1"   "quality.* is \"1\""        "$LOG_B"
check "Doom shotgun QuakeC branch fires"   "M5 boomstick fired"        "$LOG_B"
# SEPTEMBER S5 step 2: the sprite's off arm is a QuakeC branch with no other
# textual evidence. A rocket is fired at 15.6 with the cvar at 0 and god on;
# the print is first-event per map. The cvar is CF_ARCHIVE and run B shares
# the sandbox userdir, so it is restored before quit.
check "explosion sprite: off branch taken"  "M5 explosion sprite off"   "$LOG_B"
# SEPTEMBER S6: a lockstep pair. The positive proves the engine's own font was
# generated and pins the cell INDEX; the absent proves the external-particlefont
# fallback did not fire silently. Neither is vacuous without the other.
check  "shell casing: procedural cell generated"   "M5 shell casing: procedural cell 34"  "$LOG_B"
check  "particle cells: dust 35 and ring 36 generated" "M5 particle cells: dust 35, ring 36" "$LOG_B"
# BEAUTY A2/A3 (2026-09-13): the flash and spark cells, and the drawn flash
# itself armed on the shotgun shot at 8 s (first-event; it prints only when the
# view weapon's mesh gave a muzzle and the burst was spawned).
check  "particle cells: flash 37 and spark 38 generated" "M5 particle cells: .*flash 37, spark 38" "$LOG_B"
# BEAUTY A2 (2026-09-16): the ember and droplet cells, and the atlas re-generating
# live at 256-pixel cells (the 2048 font line prints only from the 256 generation,
# so it is the liveness of the runtime size and of the between-frames re-apply).
check  "particle cells: ember 39 and droplet 40 generated" "M5 particle cells: .*ember 39, droplet 40" "$LOG_B"
check  "particle font: 256-pixel cells are the shipped atlas" "M5 particle font: 2048x2048" "$LOG_B"
# The live-regeneration half of that pair: 512x512 can only come from the
# mid-boot drop to 64-pixel cells, which is what makes the size a RUNTIME
# value rather than a boot-time one.
check  "particle font: the cell size re-applies between frames" "M5 particle font: 512x512" "$LOG_B"
check  "muzzle flash: armed on the shotgun"        "M5 muzzle flash: armed"  "$LOG_B"
absent "shell casing: no silent fallback to the blob" "falling back to the round particle" "$LOG_B"
check "impulse 222 grants pentagram"       "Pentagram of Protection"   "$LOG_B"
check "impulse 224 grants ring of shadows" "Ring of Shadows"           "$LOG_B"
check "photo mode enters"                  "photo mode ON"             "$LOG_B"
check "photo mode exits"                   "photo mode OFF"            "$LOG_B"
# m5_noclipfly (was run I2): the +lookup hold pitches the view near the clamp;
# with the fly the following second of +forward must RAISE the eye by hundreds
# of units (the wish basis carries the full pitch and maxspeed normalises the
# 3D magnitude); the classic yaw-basis noclip moves it by exactly zero.
# r_volumetric_probe with no arguments is the eye oracle. Floor 150: 45 degrees
# of 320 u/s is already ~226 and the measured near-vertical flight reads ~318.
zdelta=$(awk '/NOCLIPFLY-BEFORE/{grab=1} /NOCLIPFLY-AFTER/{grab=2} /volumetric probe at/{if(grab==1)z0=$6; if(grab==2)z1=$6}END{if(z0!=""&&z1!="")printf "%d", z1-z0; else print "NA"}' "$LOG_B")
if [ "$zdelta" != "NA" ] && [ "$zdelta" -ge 150 ] 2>/dev/null; then
	pass "m5_noclipfly: pitched +forward climbs ($zdelta units)"
else
	failt "m5_noclipfly: pitched +forward climbs (got: $zdelta)"
fi

# --- run D: the GL+sidecar bridge, all-live (absorbed run C, 2026-08-21) -----
# One boot carries what used to be two: run C's view-model depth mask at a
# non-unit gamma plus the term dump, and run D's full RT+fog stack. The merge
# is deliberate, not merely cheaper -- v_gamma 0.8125 forces the offscreen
# post-process path, which is exactly the configuration the mask was once
# silently disabled in, and asserting the fog stack UNDER that gamma is the
# stronger bed (the F3/F4 lesson: a boot is only as good as the configuration
# it boots).
#
# The composite must STILL depth-mask the view model on the offscreen path; it
# only can because it runs inside R_RenderView while the scene's own depth
# buffer is bound. If someone moves it back to the end of the frame, the mask
# checks fail.
#
# RT_METAL_TERMDUMP=1 (2026-08-16): the on-demand rt_snapshot must write the
# frame AND the shown term buffer beside it, and the term reader must parse it
# -- the silhouette-outlier instrument (test/rimcount.py) is only an instrument
# while both halves keep working.
LOG_D=$(mktemp)
DUMP_C=$(mktemp)
cat > m5/smoketest_d.cfg <<'EOF'
v_gamma 0.8125
defer 6 rt_snapshot
defer 8 quit
EOF
RT_METAL_DUMP="$DUMP_C" RT_METAL_TERMDUMP=1 RT_METAL_FOGDUMP=1 $DP -window -nosound +developer 1 +r_volumetric 1 +rt_metal 1 +rt_metal_fog 1 +r_bloom 1 +r_bloom_m5 1 +map e1m3 +exec smoketest_d.cfg >"$LOG_D" 2>&1
if grep -q "RT_Metal: device" "$LOG_D"; then
	check  "RT composite runs"                        "RT_Metal: composite viewmodel mask" "$LOG_D"
	check  "RT term dump: rt_snapshot writes the term beside the frame" "RT_Metal: dumped [0-9]*x[0-9]* term" "$LOG_D"
	if [ -f "$DUMP_C.term" ] && python3 test/rimcount.py term "$DUMP_C.term" 2>/dev/null | grep -q "term luminance p50"; then
		pass "RT term dump: rimcount.py parses the dump (metric live)"
	else
		failt "RT term dump: rimcount.py parses the dump (metric live)"
	fi
	# RT_METAL_FOGDUMP=1 (BLUENOISE slice 0, 2026-09-03): the fog-buffer twin of
	# the term dump -- the SHOWN fog surface (RGBA16F, A = transmittance) beside
	# the frame, and test/flicker.py's float reader must parse it. The fog
	# kernel is live on this boot (rt_metal_fog 1 above), so a missing line
	# means the dump arm regressed, not that there was no fog to dump.
	check  "RT fog dump: rt_snapshot writes the fog buffer beside the frame" "RT_Metal: dumped [0-9]*x[0-9]* fog" "$LOG_D"
	if [ -f "$DUMP_C.fog" ] && python3 -c "
import sys; sys.path.insert(0, 'test'); import flicker
w, h, v = flicker.load_fog(sys.argv[1]); assert w > 0 and h > 0 and len(v) == w * h * 4
print('fog dump %dx%d parses' % (w, h))" "$DUMP_C.fog" 2>/dev/null | grep -q "parses"; then
		pass "RT fog dump: flicker.py parses the dump (metric live)"
	else
		failt "RT fog dump: flicker.py parses the dump (metric live)"
	fi
	check  "RT view-model mask active despite gamma"  "viewmodel mask ACTIVE"              "$LOG_D"
	absent "RT mask not silently disabled"            "viewmodel mask DISABLED"            "$LOG_D"
	# e1m3 has sky (the shaft's sky4 brushes), so the OPEN SKY instance must
	# report a real build here -- a silent degenerate stand-in on a map WITH sky
	# means the gather or the cvar default regressed (rt_metal_skyopen, 2026-08-09)
	check  "RT open-sky instance built"               "RT_Metal: built sky acceleration structure" "$LOG_D"
	check  "fog kernel: noise volume uploaded"  "RT_Metal: fog noise"       "$LOG_D"
	check  "fog kernel: world field uploaded"   "RT_Metal: fog field"       "$LOG_D"
	check  "fog kernel: output buffer created"  "RT_Metal: fog buffer"      "$LOG_D"
	check  "fog kernel: RT composite mask up"   "composite viewmodel mask ACTIVE" "$LOG_D"
	absent "fog kernel: no kernel compile fail" "fog kernel compile failed" "$LOG_D"
	# the kernel must actually take the fog over, not quietly leave the GL march
	# to do it -- which was unobservable until this line existed
	check  "fog kernel: reports itself ACTIVE"  "RT fog kernel ACTIVE"      "$LOG_D"
	# THE COMPOSITE SHADER ITSELF COMPILES on the GL bridge with the kernel-fog
	# parm in. "ACTIVE" above is printed by the C side whether or not the GLSL
	# built; the depth-aware upsample (rt_metal_fog_upsample) added textureSize /
	# texelFetch on the rectangle sampler to that branch, and a driver that
	# rejected either would drop the whole murk pass silently behind this line.
	check  "fog kernel: the GL composite permutation compiles (upsample arm)" "volumetricfog compiled" "$LOG_D"
	# BEAUTY A1 (2026-09-16): the GLSL twin of run Q's compile check -- the
	# bloom chain's arm is a peer of MODE_POSTPROCESS and both backends ask for it
	check  "bloom: the M5 chain's GLSL arm compiles on GL" "bloomblur compiled" "$LOG_D"
	# FOG LIGHTING (F5): the ambient irradiance grid bakes, prints its ms (the
	# brief's bake rule), stays inside the 600 ms budget, and crosses to the
	# sidecar. r_volumetric_ambient defaults on, so run D exercises it for free.
	check  "fog lighting: irradiance grid crosses to the kernel" "RT_Metal: fog irradiance" "$LOG_D"
	irrms=$(grep -aoE "volumetric irradiance: .* baked in [0-9]+ ms" "$LOG_D" | grep -oE "[0-9]+ ms" | grep -oE "[0-9]+" | head -1)
	if [ -n "$irrms" ] && [ "$irrms" -le 600 ] 2>/dev/null; then
		pass "fog lighting: irradiance bake prints and fits the budget (${irrms} ms)"
	else
		failt "fog lighting: irradiance bake prints and fits the budget (got: ${irrms:-no line})"
	fi
	# FOG-BAKE V2 (F4): the per-map noise bakes, prints its ms, fits the budget.
	# r_volumetric_noise2 defaults on, so run D exercises it for free; forcing
	# it to 0 removes the line, which is the verified fail direction.
	n2ms=$(grep -aoE "volumetric noise v2: .* baked in [0-9]+ ms" "$LOG_D" | grep -oE "[0-9]+ ms" | grep -oE "[0-9]+" | head -1)
	if [ -n "$n2ms" ] && [ "$n2ms" -le 600 ] 2>/dev/null; then
		pass "fog bake v2: per-map noise bakes and fits the budget (${n2ms} ms)"
	else
		failt "fog bake v2: per-map noise bakes and fits the budget (got: ${n2ms:-no line})"
	fi
else
	echo "SKIP: fog kernel checks (no Metal ray-tracing device available)"
fi
rm -f "$LOG_D" "$DUMP_C" "$DUMP_C.term" m5/smoketest_d.cfg

# (run D3, the lava crust, was absorbed into run O -- same map, one boot.)
# (run D2, the analytic gamma curve, was absorbed into run K -- same
#  no-map GL boot shape, and K's config-save wait gives it its time for free.)

# --- run E: m5_burn, the lightning gun sets things alight --------------------
# The lightning gun is a straight beam with no auto-aim, so the player is
# noclipped next to e1m3's ogre and sweeps the beam while firing -- standing at
# spawn and holding attack just cooks a wall, which is how this first read as
# broken. Asserts on the QuakeC dprint, the same shape as the shotgun check.
LOG_E=$(mktemp)
cat > m5/smoketest_e.cfg <<'EOF'
sv_cheats 1
m5_burn 1
defer 3 "god"
defer 4 "noclip"
defer 5 "impulse 9"
defer 6 "impulse 8"
defer 7 "prvm_edictset server 1 origin \"-154 -1102 90\""
defer 8 "+attack"
defer 8 "+left"
defer 15 "-attack"
defer 15 "-left"
defer 16 "r_lightningbeam_m5_test"
defer 17 "r_lightningbeam_m5 0"
defer 17.5 "echo M5-GATE-OFF-MARK"
defer 18 "+attack"
defer 18 "+left"
defer 22 "-attack"
defer 22 "-left"
defer 23 quit
EOF
# developer 2 so the bolt emits its rate-limited shape report; run E already
# noclips next to the ogre and sweeps, which is exactly the connect case
$DP -window -nosound +developer 2 +map e1m3 +exec smoketest_e.cfg >"$LOG_E" 2>&1
check "m5_burn: lightning ignites its target" "M5 burn: ignited" "$LOG_E"
# node count must be (1 << levels) + 1 or slightly under after compaction; a
# corrupt path array reported 49 nodes for levels 4 and drew uninitialised stack
check "M5 bolt: builds with a sane node count" "M5 bolt: len [0-9]+ levels [345] nodes ([0-9]|[12][0-9]|3[0-3]) " "$LOG_E"
check "M5 bolt: knows when it is connecting" "M5 bolt:.*CONNECTED" "$LOG_E"
check "M5 bolt: flicker floor holds" "M5 bolt envelope:.*floor holds" "$LOG_E"
# the bolt's shape RNG must actually differ between re-rolls. It did not for the
# whole of this feature's first life - the seed's low word was constant and it is
# the only word the generator's output depends on - and nothing on screen said so.
check "M5 bolt: seed is decorrelated" "M5 bolt seed:.*decorrelated" "$LOG_E"
# The arc must walk the gun's three electrodes, not sit on one. Silent when it
# regresses: a pinned arc looks exactly like one that has not moved yet.
check "M5 bolt: the arc walks all three electrodes" "M5 bolt muzzle:.*walks all three" "$LOG_E"
absent "M5 bolt: no beam list overflow" "beam list overflow" "$LOG_E"
# F1: every free tip must actually grow a fray. The tip count and the frizzle
# line count are separate numbers on purpose - a tip list that came out empty
# and a fray that produced nothing look identical from the outside otherwise.
check "M5 bolt: the tips fray" "M5 frizzle: on tips [1-9][0-9]* lines [0-9]+ frizzlines [1-9]" "$LOG_E"
# THE MASTER GATE (absorbed run F, 2026-08-21): r_lightningbeam_m5 0 must fall
# all the way back to the stock beam -- the M5 path must not RUN, not merely
# look the same. The same boot flips the gate off, drops a marker, and fires
# for four more seconds; the shape report is rate-limited to once per SECOND
# (not first-event), so a gate that failed would print several times in that
# window. The check is scoped to the log AFTER the marker, and the marker's own
# presence is asserted first -- a boot that quit before the gate-off window
# must fail rather than pass vacuously.
check "M5 bolt: the gate-off window ran" "M5-GATE-OFF-MARK" "$LOG_E"
if sed -n '/M5-GATE-OFF-MARK/,$p' "$LOG_E" | grep -qE "M5 bolt: len"; then
	failt "M5 bolt: master gate disables the whole path"
else
	pass "M5 bolt: master gate disables the whole path"
fi
rm -f "$LOG_E" m5/smoketest_e.cfg

# --- run G: every menu page draws the number of rows it claims --------------
# Each Options-family page carries a self-check comparing the rows it actually
# drew against its *_ITEMS define. A mismatch gives the cursor phantom slots
# below the last row (upstream shipped that bug on two pages). The check is a
# Con_DPrintf, so it needs developer 1 and, crucially, a frame drawn with the
# page open -- hence one defer per page rather than a single command.
# Verified to have teeth: bumping OPTIONS_LIGHTNING_ITEMS by one makes this fail.
LOG_G=$(mktemp)
cat > m5/smoketest_g.cfg <<'EOF'
defer 0.5 "v_idlesway_test"
defer 1 "menu_options"
defer 2 "menu_options_effects"
defer 3 "menu_options_lightning"
defer 4 "menu_options_graphics"
defer 5 "menu_options_colorcontrol"
defer 6 "menu_options_volumetric"
defer 7 "menu_options_rtshadows"
defer 8 "menu_options_m5mods"
defer 8.5 "menu_video"
defer 9 quit
EOF
$DP -window -nosound +developer 1 +map dm4 +exec smoketest_g.cfg >"$LOG_G" 2>&1
absent "menu: every page draws the rows it declares" "rows \([0-9]+\) !=" "$LOG_G"
absent "menu: every page command is registered"      "Unknown command \"menu_" "$LOG_G"
# The idle sway must ease IN slowly (so it never surprises you) and OUT fast (so
# it never fights your input), and its envelope must stay small enough to read as
# a drift rather than a wobble.
check  "sway: envelope holds"                        "M5 sway:.*envelope holds" "$LOG_G"
# The per-map light statistic the pack level-correction reads. On a stock id1
# map nothing may be rescaled from 0-255: id1 has no "_color" light at all, and
# that is the proof the colour-unit fix cannot touch stock Quake.
check  "maplights: statistic prints"                 "M5 maplights: [0-9]+ lights" "$LOG_G"
check  "maplights: nothing rescaled on id1"          "M5 maplights:.*, 0 rescaled" "$LOG_G"
rm -f "$LOG_G" m5/smoketest_g.cfg

# --- run H: the Scrag's acid venom -----------------------------------------
# m5_horde_species 4 makes every spawn a scrag, so the test does not depend on
# the wave roll producing one -- an earlier version of this check waited for a
# random scrag and flaked. Scrags unlock at wave 2, hence the one skip. The
# dprint fires where the light is attached to the projectile, proving the
# QuakeC half; the engine half is stock PFLAGS_FULLDYNAMIC handling.
LOG_H=$(mktemp)
cat > m5/smoketest_h.cfg <<'EOF'
sv_cheats 1
defer 2 "god"
defer 3 "impulse 210"
defer 32 quit
EOF
$DP -window -nosound +developer 1 +m5_venom 1 +m5_horde 1 +m5_horde_species 4 +map dm4 +exec smoketest_h.cfg >"$LOG_H" 2>&1
check  "M5 venom: spit carries an acid light" "M5 venom: acid light" "$LOG_H"
absent "horde director: silent at 0"           "M5 horde: director" "$LOG_H"
$DP -window -nosound +developer 1 +m5_venom 0 +m5_horde 1 +m5_horde_species 4 +map dm4 +exec smoketest_h.cfg >"$LOG_H" 2>&1
absent "M5 venom: master gate disables it"    "M5 venom: acid light" "$LOG_H"
rm -f "$LOG_H" m5/smoketest_h.cfg

# --- run H2: ball lightning, the ninth weapon (BALLLIGHTNING.md slice 1) -----
# A pack of soldiers (species 1) so there is something to arc at; give 8 hands
# over the thunderbolt, which is what grants the ball gun (M5Ball_Sync), and
# impulse 202 selects it. The two dprints are first-event per map: "launched"
# proves the dispatch chain (W_Attack -> W_FireBall) and "arc" proves a ball's
# think found a target it could see and zapped it. The control boots the same
# bed at cvar 0: impulse 202 must refuse (the off line) and neither event line
# may appear -- at 0 the weapon does not exist.
# Its OWN throwaway userdir (run L's shape): the shared sandbox carries every
# archived cvar earlier runs quit with -- run E leaves r_lightningbeam_m5 0
# behind, which gates the knot -- so the arm states the bolt master itself and
# boots clean. The reach is set absurdly wide (the flush-test rule) so the arc
# check depends on a soldier being in SIGHT, not on where the wave placed him.
SANDBOX_H2=$(mktemp -d)
LOG_H2=$(mktemp)
DP_H2="./darkplaces-sdl -userdir $SANDBOX_H2 +vid_renderer gl $DP_EXTRA"
cat > m5/smoketest_h2.cfg <<'EOF'
sv_cheats 1
defer 2 "god"
defer 2 "give 8"
defer 2 "give c 200"
defer 3 "impulse 210"
defer 5 "impulse 202"
defer 5.5 "impulse 223"
defer 6 "+attack"
defer 14 "-attack"
defer 16 quit
EOF
$DP_H2 -window -nosound +developer 1 +r_lightningbeam_m5 1 +m5_stock 0 +m5_balllightning 1 +m5_balllightning_radius 600 +m5_horde 1 +m5_horde_species 1 +map dm4 +exec smoketest_h2.cfg >"$LOG_H2" 2>&1
check  "M5 ball: a ball launches"                 "M5 ball: launched" "$LOG_H2"
check  "M5 ball: a ball arcs at a target"         "M5 ball: arc" "$LOG_H2"
# THE POWERUP GLOW (2026-09-19). impulse 223 is the quad cheat; the QuakeC's
# first-event line proves the PFLAGS_FULLDYNAMIC light replaced vanilla's white
# EF_DIMLIGHT rather than merely being registered.
check  "M5 powerups: the quad glows rather than spotlights" "M5 powerup glow: armed at radius 400" "$LOG_H2"
# Slice 3: the effectinfo handles resolve (0 = the block is missing or the file
# stopped parsing above it -- rule 1 of the effectinfo header) and the CLIENT
# draws the plasma knot at the ball (a Con_DPrintf, first-event per process).
check  "M5 ball: glow and burst effects resolve"   "M5 ball: effects glow [1-9][0-9]* burst [1-9][0-9]*" "$LOG_H2"
check  "M5 ball: the plasma knot draws"            "M5 ball: plasma knot active" "$LOG_H2"
$DP_H2 -window -nosound +developer 1 +r_lightningbeam_m5 1 +m5_stock 0 +m5_balllightning 0 +m5_horde 1 +m5_horde_species 1 +map dm4 +exec smoketest_h2.cfg >"$LOG_H2" 2>&1
check  "M5 ball: master gate refuses impulse 202" "ball lightning is off" "$LOG_H2"
absent "M5 ball: master gate spawns nothing"      "M5 ball: (launched|arc)" "$LOG_H2"
rm -f "$LOG_H2" m5/smoketest_h2.cfg; rm -rf "$SANDBOX_H2"

# THE DOORWAY (2026-09-19). The ball is a POINT entity now, so Q1BSP hands it the
# point clip hull instead of rounding its 20-unit box up to the 32x32x56 PLAYER
# hull -- which is what made it foul on arches and door heads. The bed is e1m1
# and NOT run H2's dm4, because the map has to CONTAIN a doorway for the check to
# have teeth: measured over five maps, dm4 moves 191 -> 214 units and e1m3
# 321 -> 338 (the ball merely stopping AT a wall rather than 20 units short of
# it), while e1m1's spawn fires straight through the start room's doorway and
# reads 303 with the box against 1109 with the point. A four-digit distance is
# therefore the assertion, and the old ball could not have produced one.
SANDBOX_H3=$(mktemp -d)
LOG_H3=$(mktemp)
cat > m5/smoketest_h3.cfg <<'EOF'
sv_cheats 1
defer 1.5 "god"
defer 1.5 "give 8"
defer 1.5 "give c 200"
defer 2.0 "impulse 202"
defer 2.5 "+attack"
defer 2.6 "-attack"
defer 7 quit
EOF
./darkplaces-sdl -userdir "$SANDBOX_H3" +vid_renderer gl $DP_EXTRA -window -nosound \
   +developer 1 +m5_balllightning 1 +r_lightningbeam_m5 1 +m5_stock 0 \
   +map e1m1 +exec smoketest_h3.cfg >"$LOG_H3" 2>&1
check  "M5 ball: it clears the doorway"  "M5 ball: earthed [0-9][0-9][0-9][0-9]+\\." "$LOG_H3"
rm -f "$LOG_H3" m5/smoketest_h3.cfg; rm -rf "$SANDBOX_H3"


# --- run G: weapon feel (SEPTEMBER2 Part G) ---------------------------------
# Its own throwaway userdir (run H2's shape): all four cvars are CF_ARCHIVE and
# a shared sandbox would carry them into every later run. Each feature has one
# first-event dprint per map. The nailgun and the grenade fire from the dm4
# spawn (the grenade lands and bounces inside two seconds); the axe sparks need
# a WALL within 64 units, so the player walks forward swinging until one stops
# him. The control boots the same script with every cvar at 0 and asserts no
# line appears -- at 0 each hook is a cvar read and nothing else.
SANDBOX_G=$(mktemp -d)
LOG_G=$(mktemp)
DP_G="./darkplaces-sdl -userdir $SANDBOX_G +vid_renderer gl $DP_EXTRA"
cat > m5/smoketest_g.cfg <<'EOF'
sv_cheats 1
defer 2 "god"
defer 2.5 "impulse 9"
defer 3 "impulse 4"
defer 3.5 "+attack"
defer 4 "-attack"
defer 4.5 "impulse 6"
defer 5 "+attack"
defer 5.2 "-attack"
defer 7.5 "impulse 1"
defer 8 "+forward"
defer 8.2 "+attack"
defer 12 "-attack"
defer 12 "-forward"
defer 13 quit
EOF
$DP_G -window -nosound +developer 1 +m5_stock 0 +m5_kick 1 +m5_grenadebounce 1 +m5_nailtracer 1 +m5_nailbarrels 1 +m5_axesparks 1 +map dm4 +exec smoketest_g.cfg >"$LOG_G" 2>&1
check  "M5 feel: weapon kick fires"        "M5 feel: weapon kick"    "$LOG_G"
check  "M5 feel: nail tracer set"          "M5 feel: nail tracer"    "$LOG_G"
check  "M5 feel: nails from the barrels"    "M5 feel: nails from the barrels" "$LOG_G"
check  "M5 feel: grenade bounce sounds"    "M5 feel: grenade bounce" "$LOG_G"
check  "M5 feel: axe sparks on a wall"     "M5 feel: axe sparks"     "$LOG_G"
$DP_G -window -nosound +developer 1 +m5_stock 0 +m5_kick 0 +m5_grenadebounce 0 +m5_nailtracer 0 +m5_nailbarrels 0 +m5_axesparks 0 +map dm4 +exec smoketest_g.cfg >"$LOG_G" 2>&1
absent "M5 feel: every cvar at 0 is silent"  "M5 feel:"                "$LOG_G"
rm -f "$LOG_G" m5/smoketest_g.cfg; rm -rf "$SANDBOX_G"

# --- run I: the mission packs ----------------------------------------------
# Each pack mounts as "m5 <pack>", so the search order is pack > m5 > id1: the
# pack's own progs.dat and models win while m5's replacement content still
# fills in underneath. The gamemode must switch too -- STAT_ACTIVEWEAPON is
# decoded differently for hipnotic and rogue, so the data without the mode
# gives a garbage HUD. Each check skips if the gamedir is not installed, since
# the packs are the player's own Steam data and are not in the repo.
LOG_I=$(mktemp)
cat > m5/smoketest_i.cfg <<'EOF'
defer 5 quit
EOF
pack_check() {   # $1 gamedir  $2 map  $3 expected game name
	if [ ! -d "$1" ]; then echo "SKIP: mission pack $1 (not installed)"; return; fi
	$DP -window -nosound -game m5 -game "$1" +developer 1 +map "$2" +exec smoketest_i.cfg >"$LOG_I" 2>&1
	check  "pack $1: mounts as $3 over m5" "Game is .*$3.*using gamedirs.*id1.*m5.*$1" "$LOG_I"
	check  "pack $1: $2 loads its own progs" "program loaded" "$LOG_I"
	absent "pack $1: $2 loads without error" "Host_Error|couldn't load map" "$LOG_I"
	# the per-pack look file carries everything that differs for this pack
	check  "pack $1: look file execs" "execing m5pack_$1.cfg" "$LOG_I"
}
pack_check hipnotic hip1m1 Hipnotic
pack_check rogue    r1m1   Rogue
pack_check dopa     e5m1   "Dimension of the Past"
pack_check mg1      mge1m1 "Dimension of the Machine"
# AD is a total conversion, not a mission pack: its own progs.dat replaces the
# M5 QuakeC lane wholesale (by the same search-order design). "start" is AD's
# own hub map, which shadows id1's under the ad-first search order.
pack_check ad       start  "Arcane Dimensions"
# SEPTEMBER2 D (2026-09-06): the sky light reads AD's own sun keys (start declares
# _sunlight 250 / _sun_mangle "300 -70"); the setter's change-only line is the
# evidence that the entity lump parsed and the cvar path armed it. Skips with AD.
if [ -d ad ]; then
	$DP -window -nosound -game m5 -game ad +developer 1 +rt_metal 1 +rt_metal_walllight 0.8 +rt_metal_sun 1 +map start +exec smoketest_i.cfg >"$LOG_I" 2>&1
	check  "rt: the sky light reads AD's sun keys on start" "RT sky light ON .toward -0.1[0-9] 0.[23][0-9] 0.9[0-9]" "$LOG_I"
else
	echo "SKIP: sky light on AD (ad not installed)"
fi
# AD's dev helpers must be OFF BY DEFAULT (2026-08-21): ad/quake.rc ships
# temp1 3072 (bit 2048 = helpers off) because this engine's shared config
# archives developer 1, which is AD's other gate for its translucent arrows
# and marker diamonds. AD's own worldspawn prints the effective flag word;
# assert bit 2048 is in it. LOG_I still holds the ad boot's log here. The
# trim lives in gitignored content, so a fresh AD reinstall loses it -- this
# check is what says so.
if [ -d ad ]; then
	adflags=$(grep -aoE "TEMP1 \+ Worldspawn \([0-9]+\)" "$LOG_I" | grep -oE "\([0-9]+\)" | tr -d "()" | head -1)
	if [ -n "$adflags" ] && [ $((adflags & 2048)) -ne 0 ] 2>/dev/null; then
		pass "pack ad: dev helpers off by default (temp1 bit 2048, flags $adflags)"
	else
		failt "pack ad: dev helpers off by default (temp1 bit 2048, got: ${adflags:-no flag line})"
	fi
fi
rm -f "$LOG_I" m5/smoketest_i.cfg

# (run I2, noclip-fly, was absorbed into run B -- same map, same cheats.)

# --- run J: the Metal renderer (METAL.md Phase 4a) --------------------------
# macOS only. The gl -> metal -> gl round trip with a real MAP LOADED while on
# Metal -- Phase 4a opened the last door, so "map refuses" flipped to "map
# renders". The r_speeds_dump draws line only prints when a frame actually
# rendered with the map up, which makes it the cheapest honest "the 3D pipeline
# ran on Metal" assertion a grep can make.
if [ "$(uname -s)" = "Darwin" ]; then
	LOG_J=$(mktemp); SANDBOX_J=$(mktemp -d)
	cat > m5/smoketest_j.cfg <<'EOF'
sv_freezenonclients 1
v_gamma 0.7
r_bloom 1
r_bloom_colorexponent 2
rt_metal 1
rt_metal_history 0
rt_metal_fog 1
r_volumetric 1
defer 1 "vid_renderer metal; vid_restart"
defer 3 "map e1m3"
defer 5 "r_metal_drawprobe"
defer 5.2 "r_metal_textureprobe"
defer 5.4 "r_metal_readbackprobe"
defer 6 "r_speeds_dump"
defer 6.5 "+attack"
defer 6.8 "-attack"
defer 7.5 "r_speeds_dump"
defer 7.7 "r_lavaboil 0"
defer 8 "vid_renderer gl; vid_restart"
defer 10 quit
EOF
	./darkplaces-sdl -userdir "$SANDBOX_J" -window -nosound +developer 1 +exec smoketest_j.cfg >"$LOG_J" 2>&1
	check  "metal: the preview brings up a Metal device"   "Metal video:"                    "$LOG_J"
	check  "metal: map loads and the 3D pipeline runs"     "^  draws  *[1-9]"                "$LOG_J"
	# 2026-08-16: NO MSL PERMUTATION MAY FAIL TO COMPILE, in either static-parm
	# set this run boots. A failed surface permutation on Metal is a BLACK
	# FRAME with one console line -- and the sweep's rt_lava vantage found one
	# shipped since the F3 merge: WaterParams declared inside USELAVA's uniform
	# block while its body sits under USEWATERSWIRL, so r_lavaboil 0 with the
	# swirl at its default failed EVERY surface mode. The `r_lavaboil 0` defer
	# above flips the static parm mid-run (R_GLSL_Restart_f recompiles every
	# permutation), so this one absent-check covers the default set AND the
	# no-boil set. Verified to FAIL on the pre-fix binary.
	absent "metal: no MSL permutation fails to compile"   "MSL compile failed"              "$LOG_J"
	# METAL.md Phase 4b: entities. The entities counter is bumped before the
	# renderpath switch and r_drawentities defaults 1 here, so a non-zero count
	# proves entity draws flowed on Metal; the compile line (developer 1, mode
	# 11 = SHADERMODE_LIGHTDIRECTION) is the tripwire for an accidentally
	# re-sentineled arm; the absent-check watches the one silent draw-skip the
	# backend has (client arrays >4KB never happen when the ring upload runs).
	check  "metal: entities render on the metal path"      "^  entities  *[1-9]"             "$LOG_J"
	check  "metal: LIGHTDIRECTION compiles for entities"   "compiled mode 11 permutation"    "$LOG_J"
	# METAL.md Phase 4c: run J is now FROZEN (pre-load, like the parity bed),
	# so the one shotgun shot's muzzle-flash rtlight never decays and the
	# second dump deterministically shows it -- unfrozen, the flash dies in
	# ~50ms and the deferred dump would race it. The lights counter proves
	# the mode-12 additive pass ran on Metal; the compile line is the
	# re-sentinel tripwire (12 = SHADERMODE_LIGHTSOURCE).
	check  "metal: the rtlight pass runs (dlights live)"   "^  lights  *[1-9]"               "$LOG_J"
	check  "metal: LIGHTSOURCE compiles for dlights"       "compiled mode 12 permutation"    "$LOG_J"
	absent "metal: no draws skipped for client arrays"     "too large to inline"             "$LOG_J"
	# METAL.md Phase 4d: the texture-index lockstep rule. An MSL arm must
	# declare its [[texture(n)]] slots densely, in the order gl_rmain.c's
	# sampler walk visits them; a violation binds where the shader does not
	# sample and the sample returns zero, with nothing in any log. Metal now
	# asserts it for every texture in every permutation it compiles.
	#
	# The absent-check would be VACUOUS on its own, so the pair matters: the
	# check below pins the permutation that actually carried the defect --
	# mode 0 (GENERIC), 0x204 = VIEWTINT|GAMMARAMPS with NO DIFFUSE, which is
	# R_SetupShader_Generic_NoTexture(true, true) drawing the loading-screen
	# progress bar. It only compiles when the gamma tables are non-trivial,
	# which is why this run pins v_gamma 0.7 (a fresh userdir is v_gamma 1 and
	# would never reach it -- exactly why no parity vantage ever caught this).
	# Verified to FAIL when broken: before the fix this run printed the
	# mismatch, naming Texture_GammaRamps at [[texture(1)]] against unit 0.
	check  "metal: the loading-bar permutation compiles"   "compiled mode 0 permutation 204" "$LOG_J"
	# METAL.md Phase 4d: the bloom chain and its composite. r_bloom forces the
	# offscreen path on its own, and r_bloom_colorexponent 2 matters -- at the
	# default 1 the darkening loop `for (x = 1; x < min(exponent, 32);)` never
	# iterates, so its squaring pass and its GL_SRC_COLOR blend (used nowhere
	# else in the engine) would go unexercised. The bloom counter proves the
	# chain ran; mode 1 = SHADERMODE_POSTPROCESS and 0x1200 = BLOOM|GAMMARAMPS
	# is the THREE-texture permutation, where the ramps must sit at index 2
	# behind First and Second -- the case the computed indices exist for, and
	# the one a literal would get wrong. The lockstep absent-check above is
	# what asserts they are right.
	check  "metal: the bloom chain runs"                   "^  bloom  *[1-9]"                "$LOG_J"
	# METAL.md Phase 5 slice 1: the RT sidecar now comes up on the RENDERER's
	# MTLDevice on the Metal path, where for Phases 0-4d it was never started at
	# all (RT_Metal_Init is called only from VID_InitModeGL). Sharing the device
	# is the whole phase -- two MTLDevice objects for one GPU cannot share
	# resources, which is the only reason the IOSurface/CGL bridge exists. The
	# log line names the owner precisely so this cannot pass by accident.
	check  "metal: RT sidecar shares the renderer's device" "device .* SHARED with the renderer" "$LOG_J"
	# Slice 3: the composite draws, so the paired check INVERTS -- it was an
	# absent ("RT does not claim to relight yet") for exactly as long as the
	# refusal stood. The string is the same on both backends by construction:
	# cl_screen.c's backend draw prints the line the GL bridge prints, so one
	# grep covers both paths. This is also the safety invariant in its positive
	# form, since the line is emitted only AFTER the draw and immediately after
	# RT_Metal_MarkComposited, which is the sole thing that makes
	# RT_Metal_Active() true on this path -- and cl_screen.c forces r_fullbright
	# on that, so a claim without a draw would render raw albedo, blinding.
	check  "metal: the RT composite draws"                  "composite viewmodel mask ACTIVE"    "$LOG_J"
	absent "metal: the slice-3 refusal is gone"             "composite is not ported yet"        "$LOG_J"
	# THE FOG KERNEL OWNS THE MURK ON METAL SINCE 6-3b, where it used to announce
	# a Phase 6 wait. This check is the positive twin of run D's GL one (above),
	# and it is the cheapest end-to-end proof the slice has: the line is printed
	# only when R_Volumetric_RenderFog actually received a fog texture, which
	# requires the encode gate, the surface allocation, the pointer-identity
	# adoption and the accessor's shared arm all to have connected. Run J already
	# sets rt_metal_fog 1 + r_volumetric 1, so it needs no new configuration.
	#
	# The `absent` is the other half and is the one that would catch a REVERT:
	# the kernel falls back to the GL march silently by design, so without it a
	# regression that stopped the kernels encoding would leave the murk looking
	# plausible and this file quiet.
	check  "metal: the fog kernel owns the murk"            "RT fog kernel ACTIVE"               "$LOG_J"
	absent "metal: the Phase 6 kernel wait is retired"      "kernels stay on the GL bridge"      "$LOG_J"
	# the MSL twin of run D's compile check: mode 17 is MODE_VOLUMETRICFOG, and
	# the depth-aware upsample's helpers and uniforms live in its kernel-fog arm
	check  "metal: the murk's MSL permutation compiles (upsample arm)" "compiled mode 17 permutation" "$LOG_J"
	check  "metal: bloom's 3-texture postprocess compiles" "compiled mode 1 permutation 1200" "$LOG_J"
	absent "metal: texture index lockstep holds"           "LOCKSTEP BROKEN"                 "$LOG_J"
	check  "metal: returns to GL on vid_restart"           "GL_VENDOR"                        "$LOG_J"
	absent "metal: no crash across the round trip"         "Engine Crash|Segmentation|Engine Error" "$LOG_J"
	# METAL.md 5-6: RT_Metal_InitWithDevice keeps the device it already holds and
	# discards the offered one, which is only safe because VID_Shutdown brings the
	# sidecar down between renderpaths. This run is exactly that round trip, so it
	# is the one place that invariant is exercised -- and the warning firing would
	# mean the sidecar is serving a stale MTLDevice into live renderer encoders,
	# with no other symptom. The check is the regression net for the guard.
	absent "metal: sidecar never re-inits on a stale device" "WARNING: re-init on a"          "$LOG_J"
	# METAL.md Phase 3 slice 3: the drawing backend, proven by readback rather
	# than by an absence of errors -- a pipeline that silently failed to build
	# and one that drew correctly produce identical logs. Both halves are
	# verified to FAIL when deliberately broken (a blend that never reaches the
	# pipeline key moves the first; a restarted pass that clears instead of
	# loading moves the second, and each leaves the other green).
	check  "metal: the drawing backend starts"             "Metal backend started"            "$LOG_J"
	check  "metal: draw probe is byte-exact vs the model"  "drawprobe: 0 of [0-9]+ bytes off by more than 1 \(max delta 0\) -- PASS" "$LOG_J"
	check  "metal: draw probe survives encoder restarts"   "drawprobe: encoder-restart pass is byte-identical -- PASS" "$LOG_J"
	# METAL.md Phase 6 slice 6-1: 3D textures, the murk's hard prerequisite.
	# Two checks, because they fail on different things and each was verified to
	# FAIL on its own. Collapsing z, or a wrong bytesPerImage or bytesPerRow,
	# moves the UPLOAD line; a NEAREST sampler or a transposed sample coordinate
	# leaves every uploaded byte perfect and moves only the SAMPLING line
	# (measured 15/255 and 210/255 against a 2/255 tolerance). A readback alone
	# would have passed both of the latter two, which is why there are two.
	check  "metal: texture probe is byte-exact (2D)"       "textureprobe: handle [0-9]+, 0 of [0-9]+ bytes differ -- PASS" "$LOG_J"
	check  "metal: 3D texture uploads every slice"         "textureprobe: 3D upload [0-9x]+, 0 of [0-9]+ bytes differ -- PASS" "$LOG_J"
	check  "metal: 3D sampling has correct axes + filter"  "textureprobe: 3D sampling [0-9]+ points, max deviation [0-2]/255 .* -- PASS" "$LOG_J"
	# METAL.md Phase 7-2: the readback's stride and format conversion. Every
	# texture in the tree is BGRA8 today, so the 16F and 32F arms are dead code
	# until Phase 7 moves a format -- and they would go live underneath the one
	# instrument nobody can double-check, the screenshot. The fixture is 7x5 and
	# the extents are the point: CLAUDE.md's 6-1 lesson is that a square probe
	# cannot see a width/height transposition, because bytesPerRow and its
	# transpose are the same number. All three breaks were verified: a wrong
	# bytes-per-pixel moves only the format that has it (63 of 140 bytes, max
	# 238), a transposed stride moves ALL FOUR formats (112 of 140 each), and a
	# missing channel swap moves only the format that needed it (70 of 140).
	check  "metal: readback stride + conversion, 4 formats" "readbackprobe: 0 bytes off across four formats \(max delta 0\) -- PASS" "$LOG_J"
	rm -f "$LOG_J" m5/smoketest_j.cfg; rm -rf "$SANDBOX_J"

	# (run J3, the EDR report, was absorbed into run K2 -- the default boot is
	#  already on Metal at r_edr 0, which is exactly the configuration J3 ran.)

	# (runs J4 and J5 -- the liquids-sampler and MetalFX-spatial validation
	#  boots -- were absorbed into run Q, 2026-08-21. The all-live validation
	#  boot is the STRONGER shape by this suite's own history: J4's class of
	#  defect was caught by narrow beds, but the F4 fog-filter layout bug and
	#  the F3 constant-struct overflow were both caught only by boots with
	#  everything live at once. Q now carries the liquids sampler, the spatial
	#  re-key and the worst-case constant struct alongside its own temporal
	#  subjects; each has a positive liveness twin so the absent checks cannot
	#  go vacuous.)


	# --- run Q: the MetalFX TEMPORAL scaler, under API validation -------------
	# r_metalfx 2 shipped with no coverage at all, and it is the arm that most
	# needs it: the temporal scaler takes THREE extra input textures (colour,
	# depth, motion) on the frame's own command buffer, so it has strictly more
	# encoder-lifecycle surface than the spatial one J5 covers.
	#
	# THE ABSENT CHECKS ARE THE POINT HERE, because every way this feature fails
	# is SILENT AND FASTER: the caller's fallback renders a correct, unscaled
	# frame, so a total failure looks like a speed-up and shows nothing on
	# screen. The two warnings below were added for exactly that reason; this
	# run is what listens to them.
	#
	# r_viewscale 0.667 because temporal at 1:1 is a different key, and
	# r_viewfbo 2 because the float scene buffer is the configuration Seb plays
	# (r_edr forces it) and the one the arm was measured in.
	#
	# 2026-08-19: the REACTIVE MASK joins the bed -- r_metalfx_reactive 3 (the
	# light footprint) plus the particle stamp that rides it -- because the
	# mask had no coverage at all and every way it fails is the same silent
	# fallback. Particles come from `pointfile` (the parity `points` vantage's
	# eight static billboards in the spawn view): ordinary CL_NewParticle
	# particles, deterministic, no cheats, and they print a free vacuous-bed
	# guard ("N points read (N particles spawned)"). A rocket would prove
	# nothing about whether particles DREW, only that the stamp said so.
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
	#
	# AND THE FOG KERNEL IS ON (2026-08-19): this run set rt_metal_fog_filter 1
	# from the day it existed but never turned rt_metal / rt_metal_fog on, so
	# the filter kernel never DISPATCHED here and a 48-byte constant block
	# bound to a 64-byte struct (the 5x5 mode read past the bind and never
	# ran) sailed through validation for a day -- caught only by an all-live
	# boot. Now the kernel is live and all three filter modes are toggled
	# under validation. A validation boot is only as good as the configuration
	# it boots.
	LOG_Q=$(mktemp); SANDBOX_Q=$(mktemp -d)
	cat > m5/smoketest_q.cfg <<'EOF'
r_metalfx 2
r_viewscale 0.667
r_viewfbo 2
r_volumetric 1
rt_metal 1
rt_metal_fog 1
rt_metal_fog_filter 1
r_metalfx_reactive 3
rt_metal_liquids 0.45
rt_metal_as_skipstatic 0
r_wateralpha 0.8
r_wateralpha_force 1
cl_particles_soft 24
cl_particles_refract 12
r_caustics 0.6
rt_metal_gi_ao 1
rt_metal_fog_liquidlight 1
m5_torch_embers 4
sv_cheats 1
r_volumetric_liquidfade 1
r_watersurface 1
r_fxaa_post 1
v_gamma 0.5
r_gamma_analytic 1
// BEAUTY A1 (2026-09-16): the modern bloom chain, live on the float buffer
// (r_viewfbo 2 above) under validation -- its own render targets, its own mode
r_bloom 1
r_bloom_m5 1
defer 1 "vid_renderer metal; vid_restart"
defer 4 "map e1m3"
defer 6 "pointfile"
defer 7 "r_speeds_dump"
defer 7.3 "+attack"
defer 7.6 "-attack"
// BEAUTY A5: a rocket at the wall so the explosion's refract ring draws under validation --
// the trigger held 0.8 s, because a +attack and -attack landing in one compressed burst
// fire nothing (the first cut failed exactly that way in the suite and passed standalone)
defer 9.3 "impulse 9"
defer 9.45 "impulse 7"
defer 9.6 "+attack"
defer 10.4 "-attack"
defer 8 "r_metalfx_entities 0"
defer 9 "r_metalfx_viewmodel 0"
defer 9.5 "rt_metal_fog_filter 2"
defer 10 "rt_metal_fog_filter 3"
defer 10.3 "r_metalfx_reactive_trail 1"
defer 10.6 "r_metalfx_reactive_particles 0; rt_metal_fog_filter 0"
defer 10.8 "rt_metal_lightsample 1"
defer 10.85 "cl_particles_lighting 1"
defer 10.9 "m5_dust 64"

defer 11 "r_metalfx 1"
defer 11.15 "cl_particles_lighting 2"
# the static/dynamic split (2026-09-19). Toggled INSIDE the window where mode 2
# is live and the dust is spawning, because the change-only line it arms is
# printed from CL_ParticleLight and a lit particle has to be drawn to reach it.
# Restored to 1 in the same boot: run Q shares the suite's sandbox userdir, and
# an archived 0.3 would dim every later run's particles without a word.
defer 11.2 "cl_particles_lighting_static 0.3"
defer 11.3 "cl_particles_lighting_static 1"
defer 11.45 "cl_particles_lighting 0"
defer 11.5 "rt_metal_lightsample 2"
defer 12 "rt_metal_lightsample 0"
defer 12.2 "rt_metal_walllight 0.8; rt_metal_gi 1"
defer 12.25 "r_skylightning 1; r_skylightning_period 2"
defer 12.28 "rt_metal_gi_tiledilate 0.5"
defer 12.33 "rt_metal_gi_tiledilate 0"
defer 12.4 "rt_metal_gi_emissive 1"
defer 12.45 "rt_metal_gi_fallback 1"
defer 12.58 "rt_metal_gi_fallback 0"
defer 12.5 "r_metalfx 2"
defer 12.55 "rt_metal_gi_rate 2"
defer 12.6 "rt_metal_gi_emissive 0"
defer 12.65 "rt_metal_gi_albedo_tex 1"
defer 12.7 "rt_metal_gi_rate 1"
defer 12.75 "rt_metal_gi_albedo_tex 0"
defer 12.8 "rt_metal_gi 0"
defer 12.82 "rt_metal_shadowlights 1"
defer 12.84 "m5_dust 0"
defer 12.86 "r_wateralpha_force 0"
defer 12.93 "r_wateralpha_force 1"
defer 12.96 "r_volumetric_liquidfade 0"
defer 13.0 "rt_metal_fog_clamp 1"
defer 13.05 "rt_metal_fog_clamp 2; rt_metal_fog_tonemapema 1"
defer 13.1 "rt_metal_fog_clamp 3; rt_metal_fog_filter 0"
defer 13.15 "rt_metal_fog_clamp 1; rt_metal_fog_tonemapema 0; rt_metal_fog_reproject_depth 1"
# 60 ms apart, not 10: each mode's line is change-only and needs a FRAME of its
# own, and at 10 ms two defers land in one cbuf burst under load (mode 2's line
# went missing twice running on 2026-09-06 with a film playing in the browser)
defer 13.18 "rt_metal_lightsample_hybrid 1"
defer 13.24 "rt_metal_lightsample_hybrid 2"
defer 13.30 "rt_metal_lightsample_hybrid 3; rt_metal_fog_filter 4"
defer 13.33 "rt_metal_fog_clamp 0; rt_metal_fog_reproject_depth 0; rt_metal_lightsample_hybrid 0"
# SEPTEMBER2 A1 + A2 (2026-09-06): the froxel fog volume is a second fog PSO
# plus two 3D volume pairs and an integrate pass, compiled lazily on the first
# enable -- exactly the declared-but-unbound / constant-struct class this run
# exists for -- and the pipelining kick splits the renderer's command buffer
# mid-frame under validation. Both toggled both ways; their change-only lines
# are the liveness evidence (a reordering shows in no counter and no pixel).
defer 13.35 "rt_metal_fog_froxel 1; rt_metal_pipeline 1; rt_metal_as_skipstatic 1; rt_metal_liquids_own 1"
defer 13.6 "rt_metal_fog_froxel 0; rt_metal_pipeline 0; rt_metal_as_skipstatic 0; rt_metal_liquids_own 0"
# STOCK MODE, toggled live: it compiles no permutation and appears in no shader
# name -- it SUBTRACTS features -- so its change-only console line is the only
# textual evidence a test can hold on to. Toggled BOTH ways, because the off
# line is what proves nothing is left latched.
#
# LAST, and with SECONDS around it rather than the 0.05 this run uses between
# its fog toggles: m5_stock moves FIVE static parms (fxaa, colorfringe, lava,
# waterswirl, redglow, liquidfade), so each toggle is a full shader rebuild, and
# under METAL_DEVICE_WRAPPER_TYPE=1 that stall compresses every following defer
# into one burst -- which ate six change-only fog-mode lines when this sat at
# 11.2 (the documented burst trap, 2026-09-06). Anything that rebuilds shaders
# goes at the END of a timing-tight run, never in the middle of one.
defer 13.4 "m5_stock 1"
defer 15.0 "m5_stock 0"
defer 16.5 quit
EOF
	METAL_DEVICE_WRAPPER_TYPE=1 ./darkplaces-sdl -userdir "$SANDBOX_Q" -window -nosound +developer 1 +exec smoketest_q.cfg >"$LOG_Q" 2>&1
	check  "metal: temporal bed actually ran"             "using gamedirs.*id1 m5"          "$LOG_Q"
	check  "metal: temporal bed reached a frame"          "^  draws  +[1-9]"                "$LOG_Q"
	check  "metal: temporal validation is really on"      "Metal API Validation Enabled"    "$LOG_Q"
	# BEAUTY A4 (2026-09-16): soft particles compile a depth unit into EVERY GENERIC
	# permutation; this boot draws the HUD, the console and the pointfile's
	# particles with the parm live under validation (the declared-but-unbound
	# class), and the first-event line says the particle pass engaged the depth.
	check  "soft particles: armed under validation"       "soft particles armed"            "$LOG_Q"
	# BEAUTY A5: the refracting shockwave -- the parm compiles the frame-copy unit
	# into every GENERIC permutation, and the explosion's refract ring at the rocket
	# fired below is what arms it (first-event).
	check  "particle refraction: armed under validation"  "particle refraction armed"       "$LOG_Q"
	# BEAUTY B1: AO from the bounce ray, toggled while GI is live under validation
	# (the kernel branch and the GI history's alpha both compile in every build;
	# the line is the change-only liveness of the setter).
	check  "rt: GI ambient occlusion armed under validation" "RT GI ambient occlusion armed"  "$LOG_Q"
	# BEAUTY B2: the fog kernel's lit terms inside a liquid, armed while the kernel is live
	check  "rt: fog light in the water armed under validation" "RT fog: light in the water armed" "$LOG_Q"
	# BEAUTY C1: a sky-lightning flash under validation on e1m3's sky (period 2 s from 12.25, the quit at 13.5+)
	check  "rt: sky lightning flashed under validation"     "sky lightning: flash"            "$LOG_Q"
	# BEAUTY C2: caustics -- the world's opaque batches arm the per-pixel test under
	# both surface parms (the fade's field, the boil's noise) under validation
	check  "caustics: armed under validation"             "caustics armed"                  "$LOG_Q"
	# BEAUTY B4: embers off e1m3's spawn torches (first-event; dm4's spawn sees no torch,
	# measured -- run B read 0 standalone, e1m3's spawn read 1 on its own bed)
	check  "torch embers: emitter live"                   "M5 torch embers: live"            "$LOG_Q"
	check  "metal: the fog kernel is live under temporal" "RT fog kernel ACTIVE"            "$LOG_Q"
	check  "metal: the temporal scaler actually engaged"  "MetalFX: temporal scaler [0-9]+x[0-9]+" "$LOG_Q"
	check  "metal: the motion-vector shader compiles"     "motionfill compiled"             "$LOG_Q"
	# BEAUTY A1 (2026-09-16): MODE_BLOOMBLUR is mode 23; the chain's change-only
	# line is the C side's evidence that the passes ran, the compile line the
	# shader's. Both under validation, so a pooled target reopened additively
	# or a mis-indexed sampler aborts here rather than rendering plausibly.
	check  "metal: the M5 bloom chain's shader compiles"  "compiled mode 23 permutation 0" "$LOG_Q"
	check  "bloom: the M5 chain arms (levels and size)"    "M5 bloom: chain armed"           "$LOG_Q"
	check  "metal: the per-entity motion shader compiles" "motionvector compiled"           "$LOG_Q"
	check  "metal: the temporal bed has particles"        "[1-9][0-9]* points read \([1-9][0-9]* particles spawned\)" "$LOG_Q"
	check  "metal: the reactive mask reaches the scaler"  "MetalFX: reactive mask [0-9]+x[0-9]+ engaged" "$LOG_Q"
	check  "metal: the particle stamp shader compiles"    "reactivestamp compiled"          "$LOG_Q"
	check  "metal: the particle stamp actually engaged"   "MetalFX: reactive stamp engaged" "$LOG_Q"
	# FOGLIGHT (2026-08-28): both lightsample modes dispatch the fog/shaft (and
	# at mode 2 the trace) kernels with the stochastic pick live, under
	# validation -- a short bind or bad index in the pick path aborts the boot.
	# The mode line is change-only in RT_Metal_SetLightSample and is the
	# feature's ONLY console evidence, so these are its liveness twins.
	check  "rt: lightsample fog-only mode engaged"        "RT lightsample mode 1"           "$LOG_Q"
	# BLUENOISE slice 2/3 (2026-09-03): the fog history pass. All three clamp
	# modes and the tonemapped blend dispatch rt_fogtemporal under validation
	# (a short bind or a bad texture index aborts the boot), with the display
	# filter on AND off (mode 3 runs with rt_metal_fog_filter 0, which is the
	# blit-copy arm). The mode line is change-only in RT_Metal_SetFogClamp and
	# is the feature's only console evidence.
	check  "rt: fog clamp mode 1 (min/max) engaged"       "RT fog clamp mode 1 \\(tonemap 0\\)" "$LOG_Q"
	check  "rt: fog clamp mode 2 + tonemapped EMA engaged" "RT fog clamp mode 2 \\(tonemap 1\\)" "$LOG_Q"
	check  "rt: fog clamp mode 3 (decoupled EMA) engaged"  "RT fog clamp mode 3 \\(tonemap 1\\)" "$LOG_Q"
	check  "rt: fog clamp returned to 0"                   "RT fog clamp mode 0 \\(tonemap 0\\)" "$LOG_Q"
	absent "rt: the fog temporal kernel compiled"          "fog temporal kernel compile failed" "$LOG_Q"
	# 2026-09-03 afternoon: the translation-aware history read (the scatter-depth
	# textures bound at 3/4 of the temporal pass, sampled bilinearly) and both
	# hybrid pick modes dispatch under validation; each has a change-only line.
	check  "rt: fog history reprojection went translation-aware" "RT fog history reprojection: translation-aware" "$LOG_Q"
	check  "rt: fog history reprojection returned to rotation-only" "RT fog history reprojection: rotation-only" "$LOG_Q"
	check  "rt: fog light pick hybrid mode 1 (two rays) engaged"  "RT fog light pick: dominant \\+ pick" "$LOG_Q"
	check  "rt: fog light pick hybrid mode 2 (alternating) engaged" "RT fog light pick: dominant / pick alternating" "$LOG_Q"
	check  "rt: fog light pick hybrid mode 3 (single-pass) engaged" "RT fog light pick: single-pass dominant / pick" "$LOG_Q"
	# SEPTEMBER S7 (2026-09-02): lit particles. The lit branch sits under
	# PBLEND_ALPHA in the draw callback and its mode line is the feature's only
	# console evidence (change-only, per the console-ink rule). This bed has
	# the pointfile's eight alpha billboards in the spawn view, so both modes
	# actually light something here -- mode 2 also bakes the irradiance grid
	# on this boot, so a bad sampler index would abort under validation.
	check  "particles: lighting mode 1 (engine LightPoint) lit a particle"  "M5 particle lighting: mode 1 live" "$LOG_Q"
	check  "particles: lighting mode 2 (irradiance grid) lit a particle"    "M5 particle lighting: mode 2 live" "$LOG_Q"
	# ...and the dust emitter spawned motes into the same lit bed (first-event line).
	check  "particles: the dust emitter spawned motes"                      "M5 dust: emitter live" "$LOG_Q"
	# ...and cl_particles_lighting_static reached the lighting. The suffix is the
	# feature's ONLY observable -- it scales a term inside a per-particle colour,
	# which no console line and no counter can otherwise see.
	check  "particles: the static-light scale is live"                     "static light scaled" "$LOG_Q"
# THE ROUND SPOTLIGHTS. The setter's line is change-only and the ONLY textual
# evidence the arm exists -- no permutation number carries it and no counter
# sees it. The boot arms at the default 3 and the defer above takes it to 1, so
# both directions are asserted: a default flip that made either unobservable is
# the documented way this class of check goes vacuous.
check  "RT: the extra lights are shadow-tested"   "RT shadow lights: 3 per pixel" "$LOG_Q"
check  "RT: the extra lights can be switched off" "RT shadow lights: the dominant alone" "$LOG_Q"
	# 2026-08-30: rt_metal_liquids multiplies the RT term into BLENDED water, and
	# on a stock id1 map nothing renders blended without r_wateralpha_force -- so
	# the feature can be set, tuned and archived while being structurally unable
	# to move a pixel, which is exactly what happened: Seb ran 0.45 for months
	# and it had never once fired. The bed drops r_wateralpha_force mid-boot to
	# put the configuration into that state and asserts the report appears.
	check  "rt: an inert rt_metal_liquids says so"        "rt_metal_liquids .* is INERT here" "$LOG_Q"
	# ...and EXACTLY ONCE. The report sits in a per-frame path, so the whole
	# guard against it is that it is change-only; if that broke it would print
	# every frame, which is the 92 GB console-ink hazard this tree has already
	# paid for once. A count check is the only thing that can see the difference.
	if [ "$(grep -c "is INERT here" "$LOG_Q")" = "1" ]; then pass "rt: the inert report is change-only"; else failt "rt: the inert report is change-only"; fi
	check  "rt: lightsample full mode engaged"            "RT lightsample mode 2"           "$LOG_Q"
	# LIQUIDFOG (2026-08-30): r_volumetric_liquidfade is live from boot HERE and
	# nowhere else in the suite, which makes this the one bed where BOTH parms
	# that share the surface DP_TEX chain are compiled at once -- USERTLIQUIDS
	# and USEVOLUMETRICLIQUIDFADE. That is the load-bearing part: leaving
	# DP_TEX_RTTERM un-rebased would give Texture_VolumeField and Texture_RTTerm
	# the SAME MSL index, i.e. a texture3d bound where the shader declares
	# texture2d, which is a validation abort here and a silent wrong sample
	# without validation (the class gl_rmain.c's RTLiquid block records three
	# shipped instances of). A static parm appears in NO permutation number and
	# NO shader name, so the armed line is the feature's only console evidence
	# and this is what proves the arm compiled and dispatched.
	check  "murk: the liquid fade is armed under validation" "murk liquid fade armed"       "$LOG_Q"
	check  "stock: 1996 mode arms under validation"       "M5 stock mode ON"                "$LOG_Q"
	check  "stock: and disarms again"                    "M5 stock mode off"               "$LOG_Q"
	# ...and its twin: dropping the fade with blended water still on screen puts
	# the configuration into the BROKEN state, which the engine must say out
	# loud. Force Water Alpha used to produce exactly that picture in silence.
	check  "murk: blended liquid with no fade says so"    "murk cannot fog it"              "$LOG_Q"
	# EXACTLY ONCE, the console-ink rule: both halves sit in a per-frame path
	# and change-only is their whole guard.
	if [ "$(grep -c "murk cannot fog it" "$LOG_Q")" = "1" ]; then pass "murk: the liquid-fade report is change-only"; else failt "murk: the liquid-fade report is change-only"; fi
	# WARCHEST session 1 (2026-08-28): the term upsample samples the scene depth
	# in the composite -- a new binding pattern, so its liveness belongs in the
	# validation boot (r_viewfbo 2 makes the depth sampleable here). The armed
	# line is change-only in RT_SceneComposite.
	check  "rt: term upsample armed under validation"     "RT term upsample armed"          "$LOG_Q"
	# WARCHEST session 2: the refit engages on this boot (entities present,
	# rt_metal_refit defaults 1) and the whole boot runs under validation.
	check  "rt: dynamic BLAS refit engages under validation" "RT refit active"                 "$LOG_Q"
	check  "rt: adaptive fog stride engages under validation" "RT fog adaptive stride active"   "$LOG_Q"
	# GIARC G1 (2026-08-29): the one-bounce GI dispatches its branch here with
	# walllight live (the F3/F4 lesson: the toggle sets BOTH the cvar and the
	# arm it feeds — gi without walllight never executes the bounce), the fog
	# kernel live, under validation. The armed line is change-only in
	# RT_Metal_SetGI and is the feature's only console evidence.
	check  "rt: one-bounce GI armed under validation"     "RT GI armed"                     "$LOG_Q"
	# GIARC G3 (2026-08-29): the emissive bounce widens the ray mask to the
	# core/lava instances (degenerate on e1m3 -- the mask and branch still
	# dispatch under validation; e1m7 coverage is the session validation boot).
	# The mode suffix rides the same change-only line.
	check  "rt: emissive bounce armed under validation"   "RT GI armed .one-bounce.emissive." "$LOG_Q"
	# GIARC G4-1 (2026-08-29): the rate rotation dispatches under validation;
	# the rate suffix rides the same change-only line.
	check  "rt: GI rate rotation armed under validation"  "RT GI armed .one-bounce.*rate 1/2" "$LOG_Q"
	# GIARC G4-2 (2026-08-29): the coloured bounce reads the per-tri albedo
	# buffer (a NEW binding, buffer 9) under validation; the +albedo suffix
	# rides the same change-only line.
	check  "rt: coloured bounce armed under validation"   "RT GI armed .one-bounce.*albedo" "$LOG_Q"
	# G4-3: the full-light-list fallback dispatches a SECOND pick loop over all
	# staged lights on bounce hits no tile light reaches. Under validation because
	# it indexes tgl outside the tile list, which is exactly the shape a bad bound
	# would abort on.
	check  "rt: bounce light fallback armed under validation" "RT GI armed .one-bounce.*fallback" "$LOG_Q"
	check  "rt: froxel fog kernels compile under validation"  "RT froxel fog kernels compiled" "$LOG_Q"
	check  "rt: froxel fog volume allocated"                  "RT froxel fog volume [0-9]+x[0-9]+x48" "$LOG_Q"
	check  "rt: pipelining kick armed"                        "RT pipelining: raster committed ahead of the trace" "$LOG_Q"
	check  "rt: AS skip-static armed under validation"        "RT AS skip-static ON" "$LOG_Q"
	# GIARC G4-4: the dilated bounce cull is a uniform and a threadgroup array --
	# it appears in NO permutation number and NO shader name, so the change-only
	# suffix is its only console evidence. Toggled at 12.28/12.33, clear of run
	# Q's rt_metal_gi 0 at 12.8: a toggle that races the switch-off of the thing
	# GATING the print can never be observed (the G4-3 collision, already paid).
	check  "rt: dilated bounce cull armed under validation" "RT GI armed .one-bounce.*dilate" "$LOG_Q"
	# was run J5: the r_metalfx 1 toggle at defer 11 mints the SPATIAL scaler
	# under validation (already on the 16F input -- r_viewfbo 2 from boot), and
	# the return to 2 re-mints temporal; both keyed paths run in one boot
	check  "metal: the spatial scaler engages under validation" "MetalFX: spatial scaler [0-9]+x[0-9]+" "$LOG_Q"
	check  "metal: the spatial scaler keys on the 16F input"    "MetalFX: spatial scaler .*RGBA16F" "$LOG_Q"
	# was run J4: rt_metal_liquids 0.45 + wateralpha_force put the liquids
	# sampler in every surface permutation (USERTLIQUIDS is a static parm; at 0
	# it is not compiled in and the validation proves nothing). The liveness
	# twin: blended water must actually take the term, which the murk's own
	# compile line plus the frame counters carry -- and the whole boot aborts
	# under validation if the sampler binding is wrong, which is the subject.
	# was the constant-struct ceiling bed: volumetrics + fog kernel + liquids +
	# analytic gamma at a non-trivial v_gamma is the LARGEST permutation the
	# tree can compile, so mode 17 compiling here plus the absent overflow line
	# is the F3 net. Drop any one of those cvars and the bed silently stops
	# being a worst case.
	check  "metal: the worst-case murk permutation compiles"    "Metal_Backend: compiled mode 17" "$LOG_Q"
	absent "metal: constant struct fits the staged block"       "byte constant struct, over the" "$LOG_Q"
	# WATERSURFACE (2026-09-12): the murk's constant struct reached 128 reflected
	# names once r_watersurface added three members, and the Metal uniform table
	# dropped the murk's TEXTURES past its cap with a bare break -- Seb's Cmd+R
	# crash-looped at the first murk draw. The table says so out loud now; this
	# boot carries r_watersurface 1 so the struct is at its fattest here.
	absent "metal: uniform table is not full"            "uniform table FULL"             "$LOG_Q"
	absent "metal: no name reflected at two locations"   "reflects '.*' at two locations" "$LOG_Q"
	absent "metal: the reactive mask is not refused"      "MetalFX: reactive mask refused"  "$LOG_Q"
	absent "metal: the particle stamp is not refused"     "reactive stamp shader unavailable" "$LOG_Q"
	absent "metal: the fog filter kernel compiles"        "fog filter kernel compile failed" "$LOG_Q"
	absent "metal: no MSL failure on the temporal path"   "MSL compile failed"              "$LOG_Q"
	# 2026-09-18: the post-upscale FXAA pass. Its failure mode is SILENT in the
	# picture -- a refused encode or an uncompiled mode still renders a correct
	# frame that is merely not antialiased -- so the armed line is the only
	# evidence it ran, and the absent-check above is what caught the mode
	# failing to compile at all the first time (PixelSize was not declared for it).
	check  "metal: FXAA runs at native resolution after the upscale" "FXAA post-upscale armed" "$LOG_Q"
	absent "metal: the temporal upscale is not refused"   "MetalFX: temporal encode refused" "$LOG_Q"
	absent "metal: no scaler refusal slipped through"     "MetalFX: encode refused|exceeds the pooled render-target usage|exceeds the screen texture" "$LOG_Q"
	absent "metal: the temporal upscale is not skipped"   "temporal upscale skipped"        "$LOG_Q"
	absent "metal: temporal has the scene depth it needs" "temporal wants to run but the scene depth" "$LOG_Q"
	# 2026-09-13: the shotgun fires at 7.3 s so the RTLIGHT pass (mode 12, the
	# muzzle flash's dlight) draws UNDER VALIDATION with every static parm this
	# cfg compiles in (the water, the fade, the RT liquid term, the volumes). No
	# validation bed had ever fired a weapon: the attract demo's first flash at
	# plain defaults aborted on a stale texture type at the water's depth unit,
	# and the pass had been drawing with another program's uniform bytes on
	# Metal for weeks (flash / beam_lit 2.1 / 5.1 against GL). The positive line
	# proves the pass ran; the absent check below is what catches the abort.
	check  "metal: the rtlight pass draws under validation"  "lightsource.*compiled" "$LOG_Q"
	absent "metal: no validation failure under temporal"  "failed assertion|missing Sampler binding|missing texture binding|incorrect type of texture|Engine Crash|Segmentation" "$LOG_Q"
	rm -f "$LOG_Q" m5/smoketest_q.cfg m5/maps/e1m3.pts; rm -rf "$SANDBOX_Q"

	# --- run J6: the frame autorelease pool drains (the 33 GB leak's net) ---
	# The backend's per-frame Metal objects are AUTORELEASED, and an SDL C
	# main loop drains no pool -- before the mb_framepool fix every encoder,
	# descriptor and command buffer lived until quit. Invisible to every
	# other bed: in release the growth hides in footprint noise, and under
	# validation a short boot leaks tens of MB with no failed assertion. A
	# leak needs a RATE instrument: this counts LIVE MTLDebugRenderCommandEncoder
	# objects mid-run via heap(1). Measured discrimination: the unfixed
	# binary reads 3089 at t+25s and +121/sec; the fixed one reads 1. An
	# empty reading FAILS -- a bed that did not run must never pass (the
	# vacuous-bed rule).
	LOG_J=$(mktemp); SANDBOX_J=$(mktemp -d)
	cat > m5/smoketest_j6.cfg <<'EOF'
defer 3 "map e1m3"
defer 40 quit
EOF
	METAL_DEVICE_WRAPPER_TYPE=1 ./darkplaces-sdl -userdir "$SANDBOX_J" -window -nosound +exec smoketest_j6.cfg >"$LOG_J" 2>&1 &
	J6PID=$!
	sleep 25
	J6COUNT=$(heap "$J6PID" 2>/dev/null | awk '$4=="MTLDebugRenderCommandEncoder"{print $1; exit}')
	kill "$J6PID" 2>/dev/null; wait "$J6PID" 2>/dev/null
	if [ -z "$J6COUNT" ]; then
		failt "metal: frame pool drains (no heap reading - the bed did not run)"
	elif [ "$J6COUNT" -le 100 ]; then
		pass  "metal: frame pool drains (live debug encoders: $J6COUNT)"
	else
		failt "metal: frame pool drains (live debug encoders: $J6COUNT - the 33 GB leak class is back)"
	fi
	rm -f "$LOG_J" m5/smoketest_j6.cfg; rm -rf "$SANDBOX_J"

	# METAL.md Phase 3 slice 4: a real 2D frame reaches the drawable. This check
	# is not synthetic -- its failure mode was observed for real during the
	# slice, when one un-folded switch in R_DrawModelTextureSurfaceList left the
	# whole console black while every other instrument stayed green, and this
	# probe was the only thing that said so. It reads back the DRAWABLE, so it
	# also covers the v-flip present pass, which a screen-texture readback (and
	# therefore the entire parity comparison) cannot see.
	LOG_J=$(mktemp); SANDBOX_J=$(mktemp -d)
	cat > m5/smoketest_j2.cfg <<'EOF'
defer 1 "vid_renderer metal; vid_restart"
defer 3 "disconnect"
defer 4 "menu_main"
defer 9 quit
EOF
	VID_METAL_PROBE=1 ./darkplaces-sdl -userdir "$SANDBOX_J" -window -nosound +exec smoketest_j2.cfg >"$LOG_J" 2>&1
	check  "metal: a 2D frame reaches the drawable"        "VID_METAL_PROBE present .* -> A FRAME REACHED THE DRAWABLE" "$LOG_J"
	absent "metal: the frame is not flat"                  "FLAT \(present pass drew nothing\)" "$LOG_J"
	absent "metal: no crash carrying a frame"              "Engine Crash|Segmentation|Engine Error" "$LOG_J"
	rm -f m5/smoketest_j2.cfg
	rm -f "$LOG_J" m5/smoketest_j.cfg; rm -rf "$SANDBOX_J"

	# METAL.md Phase 5 slice 2: THE CROSS-BACKEND TERM-BUFFER IDENTITY. The RT
	# sidecar runs the same kernel source on the same MTLDevice with the same
	# uploaded inputs on both paths; the only difference is how the output
	# texture is allocated (IOSurface-backed on GL, plain on Metal). So the
	# traced term buffer must come out BYTE-IDENTICAL, and that single fact
	# proves the whole compute half ported -- before any pixel is composited.
	#
	# rt_metal_history 0 is what makes it reproducible at all, and not by luck:
	# all three kernels read cam.frame only under `cam.history > 0.0f`, so with
	# history off the jitter falls back to pixel-derived noise that cannot vary
	# per frame. (Measured in 5-1: history 0.5 unpinned gives two DIFFERENT
	# hashes, which is how this hash was shown to have teeth.)
	#
	# The zero-texel count is part of the assertion, not decoration: an all-zero
	# buffer hashes perfectly consistently and would otherwise be a very
	# convincing pass.
	RTP_GL=$(mktemp); RTP_MT=$(mktemp)
	for rtb in gl metal; do
		SANDBOX_R=$(mktemp -d)
		cat > m5/smoketest_rt.cfg <<EOF
sv_freezenonclients 1
sv_random_seed 1
cl_nettimesyncfactor 1
cl_nettimesyncboundmode 1
r_drawentities 0
cl_particles 0
viewsize 100
crosshair 0
r_volumetric 0
rt_metal 1
rt_metal_history 0
rt_metal_walllight 0
defer 1 "vid_renderer $rtb; vid_restart"
defer 4 "map e1m3"
defer 6 "sv_cheats 1"
defer 7 "noclip"
defer 12 "rt_metal_termprobe"
defer 14 quit
EOF
		if [ "$rtb" = gl ]; then RTOUT="$RTP_GL"; else RTOUT="$RTP_MT"; fi
		./darkplaces-sdl -userdir "$SANDBOX_R" -window -nosound +exec smoketest_rt.cfg >"$RTOUT" 2>&1
		rm -f m5/smoketest_rt.cfg; rm -rf "$SANDBOX_R"
	done
	RTH_GL=$(grep -a "rt_metal_termprobe: slot" "$RTP_GL" | tail -1 | sed 's/.*hash \([0-9a-f]*\).*/\1/')
	RTH_MT=$(grep -a "rt_metal_termprobe: slot" "$RTP_MT" | tail -1 | sed 's/.*hash \([0-9a-f]*\).*/\1/')
	RTZ_GL=$(grep -a "rt_metal_termprobe: slot" "$RTP_GL" | tail -1 | sed 's/.*zerotexels \([0-9]*\) of.*/\1/')
	if [ -z "$RTH_GL" ] || [ -z "$RTH_MT" ]; then
		failt "metal: RT term buffer is byte-identical across backends (probe produced no hash)"
	elif [ "$RTZ_GL" != "0" ]; then
		failt "metal: RT term buffer is byte-identical across backends (GL buffer is all zeros - vacuous)"
	elif [ "$RTH_GL" = "$RTH_MT" ]; then
		pass  "metal: RT term buffer is byte-identical across backends"
	else
		failt "metal: RT term buffer is byte-identical across backends (gl $RTH_GL vs metal $RTH_MT)"
	fi
	rm -f "$RTP_GL" "$RTP_MT"

	# THE ESCAPE HATCH, INVERTED AT PHASE 8. Run K used to assert vid_renderer
	# was NOT archived -- the Phase 0 safety, when the Metal path drew nothing
	# and a saved "metal" was a blank violet window. With metal the DEFAULT, the
	# hazard runs the other way: "vid_renderer gl" MUST persist, because a
	# player who cannot save their way onto GL has no escape hatch at all
	# (Cvar_WriteVariables only writes archived cvars that differ from the
	# default, which is exactly why the old non-archived shape was safe then and
	# is a trap now). Same two vacuousness traps as before: config.cfg is only
	# written after ~6s of uptime, and only a NON-DEFAULT value is written -- so
	# the run must be long enough AND must be on gl at quit. A missing
	# config.cfg fails rather than passes.
	# ...and the ANALYTIC GAMMA curve rides the same boot (was run D2, absorbed
	# 2026-08-21 -- the config-save wait gives it its runtime for free).
	# r_gamma_analytic_test measures palette.c's 256-entry LUT against
	# vid_shared.c's analytic form at 256 points. EXPECT AGREEMENT, NOT
	# EQUALITY: the LUT quantises input and output to 8 bits, so half a level
	# is the floor, not a defect (7-1 measured 0.498-0.500 across every
	# configuration). Run at a NON-TRIVIAL gamma on purpose -- at defaults the
	# ramp texture is never built and the comparison is 0 against 0, a check
	# that passes by being vacuous.
	LOG_K=$(mktemp); SANDBOX_K=$(mktemp -d)
	cat > m5/smoketest_k.cfg <<'EOF'
defer 1 "r_gamma_analytic_test"
defer 2 "v_color_enable 1"
defer 2.2 "v_color_grey_g 0.8"
defer 2.4 "v_color_white_b 3"
defer 3 "r_gamma_analytic_test"
defer 7 quit
EOF
	./darkplaces-sdl -userdir "$SANDBOX_K" -window -nosound +vid_renderer gl +r_brightness 0.4 +v_gamma 0.5 +v_contrast 0.625 +exec smoketest_k.cfg >"$LOG_K" 2>&1
	check "metal: quitting on gl still saves a config" "Saving config" "$LOG_K"
	check  "gamma: analytic curve matches the LUT"      "gamma analytic: MATCHES the LUT"   "$LOG_K"
	absent "gamma: analytic curve never diverges"       "DIVERGES from the LUT"             "$LOG_K"
	absent "gamma: analytic stays available"            "NOT AVAILABLE"                     "$LOG_K"
	if [ ! -f "$SANDBOX_K/m5/config.cfg" ]; then
		failt "metal: vid_renderer gl persists (no config.cfg written - run too short?)"
	elif grep -q '"vid_renderer" "gl"' "$SANDBOX_K/m5/config.cfg"; then
		pass  "metal: vid_renderer gl persists (the escape hatch saves)"
	else
		failt "metal: vid_renderer gl persists (the escape hatch saves)"
	fi
	rm -f "$LOG_K" m5/smoketest_k.cfg; rm -rf "$SANDBOX_K"

	# THE DEFAULT IS METAL (Phase 8, Seb's call), proven by a boot that sets
	# nothing: no +vid_renderer, no config (fresh userdir), no vid_restart. The
	# "Metal backend started" line only prints when the Metal renderpath
	# actually came up, and the draws counter proves it rendered rather than
	# fell back -- VID_InitMode falls back to GL silently-but-loggedly when
	# Metal init fails, and that fallback PASSING this bed would be the exact
	# false negative the bed exists to refuse.
	LOG_K=$(mktemp); SANDBOX_K=$(mktemp -d)
	cat > m5/smoketest_k2.cfg <<'EOF'
defer 3 "map e1m3"
defer 6 "r_speeds_dump"
defer 7 "r_edr_report"
defer 9 quit
EOF
	./darkplaces-sdl -userdir "$SANDBOX_K" -window -nosound +exec smoketest_k2.cfg >"$LOG_K" 2>&1
	check  "metal: the DEFAULT boot lands on Metal"      "Metal backend started"    "$LOG_K"
	check  "metal: the default boot renders a frame"     "^  draws  +[1-9]"         "$LOG_K"
	absent "metal: the default boot did not fall back"   "falling back to GL"       "$LOG_K"
	# METAL.md Phase 8-4: the MetalFX probe must RUN and say what it found --
	# the pattern matches both "spatial scaler available" and "spatial scaling
	# not supported", because the check's subject is the probe reporting, not
	# this machine's answer. A refactor that silently stops calling
	# MetalFX_Start is what this catches.
	check  "metal: the MetalFX probe reported (8-4)"     "MetalFX: spatial scal"    "$LOG_K"
	# ...and the TEMPORAL half of the same probe, which is a separate
	# descriptor round trip in MetalFX_Start. Matches both wordings, so a
	# machine without temporal support still passes -- the subject is the
	# probe running, not this device's answer.
	check  "metal: the temporal probe reported too"     "MetalFX: temporal scal"   "$LOG_K"
	# was run J3: the EDR report, on the default Metal boot at r_edr 0 --
	# DELIBERATELY 0: the ask engages EDR on the real display and visibly dims
	# the desktop, which a test suite has no business doing. What it proves is
	# that the AppKit path resolves the WINDOW's own screen (not mainScreen,
	# which follows the key window and answers a different question plausibly)
	# and that the refusal is NAMED rather than silent.
	check  "metal: EDR report names why it is not asking" "r_edr_report: r_edr is 0"        "$LOG_K"
	check  "metal: EDR report resolves OUR screen"        "screen [0-9]+ <- ours"           "$LOG_K"
	check  "metal: EDR report reads a headroom pair"      "current [0-9.]+  potential [0-9.]+" "$LOG_K"
	absent "metal: EDR never asks at r_edr 0"             "ask=YES"                         "$LOG_K"
	rm -f "$LOG_K" m5/smoketest_k2.cfg; rm -rf "$SANDBOX_K"

	echo
	# --- run R: analytic MLAA at native resolution (SMAA.md, 2026-09-18) ------
	# ITS OWN BOOT, AND THAT IS THE FINDING RATHER THAN A PREFERENCE. These
	# checks first rode run Q, and enabling r_smaa COMPILES THREE SHADER MODES --
	# under METAL_DEVICE_WRAPPER_TYPE=1 that stall compresses every following
	# defer into one cbuf burst, which is the documented trap run Q's own
	# m5_stock comment states in as many words ("anything that rebuilds shaders
	# goes at the END of a timing-tight run"). At 13.7 it ate SIX unrelated
	# pre-existing checks; moved past m5_stock to 16.5 it ate its own five and
	# the quit with them. Run Q is at its limit; a rebuild belongs in a boot with
	# slack, and this one has seconds between every toggle.
	#
	# Validation is on because the blend arm declares a SECOND sampler and the
	# two intermediates are pooled targets -- the declared-but-unbound class.
	# r_fxaa_post 1 so the first toggle also exercises the supersede path.
	# ITS OWN USERDIR: this boot archives r_metalfx, r_viewfbo and r_viewscale,
	# and the shared sandbox is saved on every quit -- the documented
	# archived-cvar persistence trap that has bitten runs L, G and H2 before.
	SANDBOX_R=$(mktemp -d); mkdir -p "$SANDBOX_R/m5"
	cat > "$SANDBOX_R/m5/smoketest_r.cfg" <<'CFGEOF'
r_viewfbo 2
r_viewscale 0.667
r_metalfx 2
r_metalfx_reactive 3
r_fxaa_post 1
r_fxaa 0
rt_metal 1
r_volumetric 1
defer 3 "map e1m3"
defer 6 "r_smaa 1"
defer 9 "r_smaa_debug 1"
defer 11 "r_smaa_debug 2"
defer 13 "r_smaa_debug 0"
defer 15 "r_smaa_search 32"
defer 17 "r_smaa 0"
defer 19 "quit"
CFGEOF
	LOG_R=$(mktemp)
	METAL_DEVICE_WRAPPER_TYPE=1 ./darkplaces-sdl -userdir "$SANDBOX_R" -window -nosound +developer 1 +vid_renderer metal +exec smoketest_r.cfg >"$LOG_R" 2>&1
	check  "smaa: validation is really on"                "Metal API Validation Enabled"    "$LOG_R"
	# per MODE, because a mode that fails to build still DRAWS -- the permutation
	# fallback renders a correct frame that is merely not antialiased, which is
	# the invisible-failure class this feature is full of. The texture COUNTS are
	# in the pattern on purpose: the blend arm declares a second sampler, and
	# "2 textures" is what says the sampler walk gave it one rather than leaving
	# it on whatever the last shader bound.
	check  "smaa: the edge pass compiles"                 "smaaedges compiled \\(1 textures\\)"   "$LOG_R"
	check  "smaa: the weight pass compiles"               "smaaweights compiled \\(1 textures\\)" "$LOG_R"
	check  "smaa: the blend pass compiles with 2 samplers" "smaablend compiled \\(2 textures\\)"  "$LOG_R"
	check  "smaa: the pass arms under validation"         "SMAA armed \\([0-9]+x[0-9]+, native, search 16\\)" "$LOG_R"
	check  "smaa: the search reach reaches the pass"      "SMAA armed \\([0-9]+x[0-9]+, native, search 32\\)" "$LOG_R"
	check  "smaa: the debug view arms"                    "SMAA armed .*DEBUG VIEW"         "$LOG_R"
	check  "smaa: it supersedes the FXAA post pass"       "r_smaa supersedes r_fxaa_post"   "$LOG_R"
	absent "smaa: no MSL arm failed to compile"           "MSL compile failed"              "$LOG_R"
	absent "smaa: no intermediate target was refused"     "r_smaa: no intermediate render target" "$LOG_R"
	rm -f "$LOG_R"; rm -rf "$SANDBOX_R"

	# --- run T: the liquid's own light (SEPTEMBER2 C1, 2026-09-06) -------------
	# Its line fires only when a BLENDED liquid batch is drawn with the RT term
	# live, and run Q's spawn camera on e1m3 draws none -- a check there was
	# vacuous (measured: it failed while every neighbour passed). Nor does e1m1's
	# spawn (measured too, twice); the camera is demo19's at its f4900 -- the slime
	# hall, eye 1080 2209 -337 looking 28.6 degrees down at yaw 79 -- set under
	# noclip (origin sticks only while noclip is on: the documented headless rule).
	cat > m5/smoketest_t.cfg <<'CFGEOF'
rt_metal 1
rt_metal_walllight 0.8
r_wateralpha 0.7
r_wateralpha_force 1
rt_metal_liquids 0.45
rt_metal_liquids_own 1
rt_metal_liquids_rt 1
r_watersurface 1
defer 3 "map e1m1"
defer 5 "sv_cheats 1"
defer 5.5 "noclip"
defer 6 "prvm_edictset server 1 origin \"1080 2209 -359\""
defer 6.1 "prvm_edictset server 1 v_angle \"28.6 79.2 0\""
defer 6.2 "prvm_edictset server 1 fixangle 1"
defer 11 "quit"
CFGEOF
	LOG_T=$(mktemp)
	$DP -window -nosound +developer 1 +vid_renderer metal +exec smoketest_t.cfg >"$LOG_T" 2>&1
	check  "rt: liquid own light armed on e1m1's slime"       "RT liquid own light armed" "$LOG_T"   # the camera above is the bed; a spawn boot never draws a blended liquid
	# SEPTEMBER2 C2 (2026-09-09): the per-pixel pair on the same bed -- the setter's
	# change-only line, the blended-liquid structure, and the consumer's own line,
	# which fires only once a blended batch is drawn with the pair LIVE (traced).
	check  "rt: liquid pair switched on (C2)"                 "RT liquid pair ON" "$LOG_T"
	check  "rt: blended liquid acceleration structure built"  "built BLENDED liquid acceleration structure" "$LOG_T"
	check  "rt: liquid pair armed at the slime (own term + reflection)" "RT liquid pair armed" "$LOG_T"
	# WATERSURFACE (2026-09-12): the screen-space refraction on the same bed -- its
	# static parm appears in no permutation number and no shader name, so the
	# first-event line is its only console evidence; and this boot compiles it
	# beside USERTLIQUIDS on Metal, which is the texture-index re-basing case the
	# Metal API validation of run Q cannot see from e1m3's spawn (no liquid there).
	check  "water: surface refraction armed on e1m1's slime" "water surface armed" "$LOG_T"
	rm -f "$LOG_T" m5/smoketest_t.cfg

	# --- run S: SOUND, silently (SEPTEMBER2 Part B, 2026-09-06) ---------------
	# -simsound mixes with no output device (nothing is heard); the mix dump is
	# the instrument (SND_DUMPMIX) and the reverb's room report is its console
	# evidence. Every other run here is -nosound, so without this boot the
	# mixer's new arms would have no coverage at all.
	SNDWAV=$(mktemp).wav
	cat > m5/smoketest_s.cfg <<'CFGEOF'
snd_reverb 1
snd_reverb_report 1
snd_occlusion_lowpass 1
snd_airabsorb 1
defer 3 "map dm4"
defer 8 "snd_reverb 0"
defer 9 "quit"
CFGEOF
	LOG_S=$(mktemp)
	SND_DUMPMIX="$SNDWAV" $DP -window -simsound +developer 1 +exec smoketest_s.cfg >"$LOG_S" 2>&1
	check  "snd: the mix dump opens under -simsound"     "snd: dumping the mix to" "$LOG_S"
	check  "snd: the room reverb reads the level"        "reverb room: mean free path [0-9]+ units" "$LOG_S"
	check  "snd: the mix dump closes with bytes in it"   "snd: mix dump closed, [1-9][0-9]+ bytes" "$LOG_S"
	check  "snd: air absorption armed on a far sound (B3)" "snd: air absorption armed" "$LOG_S"   # first-event, developer 1; dm4's ambients sit well past 300 units of the spawn
	rm -f "$LOG_S" "$SNDWAV" m5/smoketest_s.cfg

	# --- run Z: demo16, the all-live playback (NEW, 2026-08-21) --------------
	# Seb's own recording (e3m3, real play, ~70 seconds of fighting) played
	# back with the WHOLE live stack on the DEFAULT renderer -- temporal
	# scaler, reactive mask, RT wall lighting, the weapon's own RT light, fog
	# kernel, filter, analytic gamma. This is the one bed in the suite that
	# looks like an hour of his session rather than a synthetic vantage: every
	# frame of the demo drives the full pipeline, and -benchmark quits by
	# itself with a parseable result line.
	#
	# The demo lives in his real userdir (it is not repo content), so the run
	# SKIPS cleanly when absent -- but on this machine it is the standing net
	# for "the live configuration survives real play".
	DEMO16="$HOME/Library/Application Support/darkplaces/m5/demo16.dem"
	if [ -f "$DEMO16" ]; then
		LOG_Z=$(mktemp); SANDBOX_Z=$(mktemp -d)
		mkdir -p "$SANDBOX_Z/m5"
		cp "$DEMO16" "$SANDBOX_Z/m5/"
		cat > "$SANDBOX_Z/m5/autoexec.cfg" <<'EOF'
vid_vsync 0
developer 1
r_metalfx 2
r_viewscale 0.667
r_viewfbo 2
r_metalfx_reactive 3
r_volumetric 1
rt_metal 1
rt_metal_walllight 0.8
rt_metal_fog 1
rt_metal_fog_filter 2
r_gamma_analytic 1
v_gamma 0.5
EOF
		./darkplaces-sdl -userdir "$SANDBOX_Z" -window -nosound -benchmark demo16.dem >"$LOG_Z" 2>&1
		if grep -q "RT_Metal: device" "$LOG_Z"; then
			if grep -qE "result [0-9]+ frames" "$SANDBOX_Z/m5/benchmark.log" 2>/dev/null; then
				pass "demo16: the playback completes with a result line"
			else
				failt "demo16: the playback completes with a result line"
			fi
			check  "demo16: the RT composite is live"           "composite viewmodel mask ACTIVE" "$LOG_Z"
			check  "demo16: the fog kernel owns the murk"       "RT fog kernel ACTIVE"            "$LOG_Z"
			# the weapon's RT-matched light (2026-08-21): its first-event
			# liveness line. The feature falls back to the flat gun SILENTLY
			# on any of its predicates, so this is the one place a regression
			# that stopped it engaging would say so.
			check  "demo16: the weapon takes its RT light"      "RT viewmodel light active"       "$LOG_Z"
			check  "demo16: the temporal scaler engaged"        "MetalFX: temporal scaler [0-9]+x[0-9]+" "$LOG_Z"
			check  "demo16: the reactive mask engaged"          "MetalFX: reactive mask [0-9]+x[0-9]+ engaged" "$LOG_Z"
			absent "demo16: no MSL failure on the live config"  "MSL compile failed"              "$LOG_Z"
			absent "demo16: the stack survives real play"       "Host_Error|Engine Crash|Segmentation" "$LOG_Z"
		else
			echo "SKIP: demo16 playback (no Metal ray-tracing device available)"
		fi
		rm -f "$LOG_Z"; rm -rf "$SANDBOX_Z"
	else
		echo "SKIP: demo16 playback (no demo16.dem in the userdir)"
	fi

	echo
	# --- run L: the emissive liquid instance goes live (rt_metal_liquidemissive)
	# Its OWN throwaway userdir: the cvar is archived, so setting anything in the
	# shared sandbox would silently ride into every later suite run's config (the
	# persistence trap run C's v_gamma note documents). e1m1's slime renders
	# opaque at fresh-userdir defaults and the cvar defaults 1, so the gather
	# must produce a real BLAS -- a silent degenerate here means the gather, the
	# opacity predicate or the cvar plumbing regressed (2026-08-09, QA round 2).
	SANDBOX_L=$(mktemp -d)
	LOG_L=$(mktemp)
	./darkplaces-sdl -userdir "$SANDBOX_L" -window -nosound +developer 1 +rt_metal 1 +map e1m1 +defer 6 quit >"$LOG_L" 2>&1
	if grep -q "RT_Metal: device" "$LOG_L"; then
		check  "metal: emissive liquid instance built (e1m1 slime)" "RT_Metal: built liquid acceleration structure" "$LOG_L"
	else
		echo "SKIP: emissive liquid instance (no Metal ray-tracing device available)"
	fi
	rm -f "$LOG_L"; rm -rf "$SANDBOX_L"

	# --- run P: blue-noise kernel jitter (rt_metal_bluenoise, the weave fix) ---
	# Its OWN throwaway userdir (archived cvar, the run-L persistence trap). Two
	# boots: the default (1) must show the table resident AND the fog kernel
	# still ACTIVE (a broken splice falls back SILENTLY by design -- liveness is
	# the check, per the perf-audit doctrine); the off arm (0) must ALSO reach
	# ACTIVE, proving the IGN preprocessor arm still compiles -- the arm the
	# byte-exact off-switch promise lives on.
	# ONE boot since 2026-08-21: the toggle to 0 mid-run triggers the one-off
	# kernel rebuild with the IGN arm, so both preprocessor sets compile in a
	# single boot. The gate-off half is marker-scoped, and the marker's own
	# presence is asserted so a boot that quit early fails rather than passes.
	SANDBOX_P=$(mktemp -d)
	LOG_P=$(mktemp)
	./darkplaces-sdl -userdir "$SANDBOX_P" -window -nosound +developer 1 +rt_metal 1 +r_volumetric 1 +rt_metal_fog 1 +map e1m3 \
		+defer 6 "echo BLUENOISE-OFF-MARK" +defer 6.2 "rt_metal_bluenoise 0" +defer 10 quit >"$LOG_P" 2>&1
	if grep -q "RT_Metal: device" "$LOG_P"; then
		check  "metal: blue-noise jitter table resident" "RT_Metal: blue-noise jitter table resident" "$LOG_P"
		check  "metal: fog kernel ACTIVE with blue noise" "RT fog kernel ACTIVE" "$LOG_P"
		absent "metal: no kernel compile failure with blue noise" "kernel compile failed" "$LOG_P"
		check  "metal: the bluenoise-0 window ran" "BLUENOISE-OFF-MARK" "$LOG_P"
		if sed -n '/BLUENOISE-OFF-MARK/,$p' "$LOG_P" | grep -qE "kernel compile failed"; then
			failt "metal: the IGN arm recompiles cleanly at bluenoise 0"
		else
			pass "metal: the IGN arm recompiles cleanly at bluenoise 0"
		fi
	else
		echo "SKIP: blue-noise jitter (no Metal ray-tracing device available)"
	fi
	rm -f "$LOG_P"; rm -rf "$SANDBOX_P"

	# --- run N: F1's capsule-SDF bolt reaches the screen on Metal --------------
	# Its OWN throwaway userdir for the same reason run L has one: both cvars are
	# archived and would otherwise ride into every later run's config.
	#
	# Three separate claims, because two of them have already been true while the
	# bolt was invisible. The MODE has to compile (a permutation-0 failure
	# degrades silently to program 0). The PASS has to find geometry and draw it
	# -- the stream is filled during the relink and drained during the render, and
	# a frame-stamp mismatch once emptied it with the shader compiled and the
	# emitter provably forking into it. And the magenta sentinel must never be
	# what compiled: it is a peer arm in the GLSL chain, reachable only if the
	# capability gate is wrong.
	SANDBOX_N=$(mktemp -d)
	LOG_N=$(mktemp)
	cat > m5/smoketest_n.cfg <<'EOF'
sv_cheats 1
r_lightningbeam_m5_sdf 1
r_lightningbeam_m5_seed 12345
defer 3 "god"
defer 4 "noclip"
defer 5 "impulse 9"
defer 6 "impulse 8"
defer 7 "prvm_edictset server 1 origin \"-154 -1102 90\""
defer 8 "+attack"
defer 10 "r_lightningbeam_m5_fizz 0.35"
defer 13 quit
EOF
	./darkplaces-sdl -userdir "$SANDBOX_N" -window -nosound +developer 2 +map e1m3 +exec smoketest_n.cfg >"$LOG_N" 2>&1
	check  "m5bolt: the SDF shader mode compiles"   "m5bolt compiled"                  "$LOG_N"
	check  "m5bolt: the pass draws real geometry"   "M5 bolt SDF: segs [1-9][0-9]* verts [1-9]" "$LOG_N"
	absent "m5bolt: the SDF shader never refuses"   "will not compile"                 "$LOG_N"
	# THE FIZZ (S4, 2026-09-01). Its arm lives inside a mode that appears in no
	# permutation number and no shader name, so the change-only console line is
	# its only textual evidence. Toggled at 10, two seconds clear of the quit.
	#
	# The pattern pins the TOGGLED amplitude, not just "armed": the default is
	# 0.666 since Seb's QA, so the boot already prints an armed line and a bare
	# "armed" would pass with the toggle doing nothing -- the LIQUIDFOG
	# Outcome III shape, where a default flip quietly makes a change-only
	# assertion unobservable.
	check  "m5bolt: the fizz reaches the shader"    "m5bolt fizz armed .amplitude 0.35" "$LOG_N"
	# EFFECTINFO (S5). The parser's failure mode is the reason this check exists:
	# a wrong-argument-count line ends in `break;` out of the LINE loop, so it
	# prints one message and every block below it silently never exists. An
	# `absent` on that message is therefore a whole-file syntax gate. Verified to
	# FAIL: appending `size 1 2 3` to m5/effectinfo.txt produces two of them.
	absent "effectinfo: m5/effectinfo.txt parses clean" "error while parsing" "$LOG_N"
	rm -f "$LOG_N" m5/smoketest_n.cfg; rm -rf "$SANDBOX_N"
	# --- run O: the heat haze marches (r_lavashimmer, F7) -------------------
	# Its OWN throwaway userdir for the same archived-cvar reason as run L, and
	# it must boot on the DEFAULT renderer because the shimmer is gated on
	# vid.m5postfx -- runs A-I pin +vid_renderer gl, where the static parm is
	# never enabled and this could not fire. e1m7 is the suite's lava map.
	#
	# The line is emitted once, from the postprocess uniform fill, only when the
	# baked field resolved AND the scene depth really published -- which is the
	# whole chain the reverted first attempt at this effect got wrong silently
	# (it ran with a garbage world position and a mask that was always zero, and
	# nothing said so). It also reports the EFFECTIVE reach after the field's
	# signed-distance cap, so a bake that shrank it is visible rather than mute.
	# (absorbed run D3, 2026-08-21: the lava-flow uniform fill is backend-
	#  agnostic C, so it rides this e1m7 boot instead of its own GL one. Both
	#  prints are first-event, so each asserts its term genuinely reached a
	#  lava batch -- r_lavaflow 0, or a sheet that never classified as lava,
	#  make the flow line silent.)
	SANDBOX_O=$(mktemp -d)
	LOG_O=$(mktemp)
	./darkplaces-sdl -userdir "$SANDBOX_O" -window -nosound +developer 1 +r_lavashimmer 4 +r_lavaboil 0.6 +r_lavaflow 1 +map e1m7 +defer 6 quit >"$LOG_O" 2>&1
	check  "m5 lava: the heat haze marches the view ray" "M5 lava: heat haze marching" "$LOG_O"
	check  "m5 lava: the crust flows"                    "M5 lava: crust flowing"      "$LOG_O"
	rm -f "$LOG_O"; rm -rf "$SANDBOX_O"
else
	echo "SKIP: metal renderer preview (macOS only)"
fi

echo
# --- run M: the KH swirl (F3) ------------------------------------------------
# Two still pairs on the frozen byte-deterministic murk bed (winds pinned "0 0 0"
# QUOTED, harness preamble, clear on its OWN defer a second before the shot --
# the screenshot captures the PREVIOUS frame, so a same-defer clear leaves the
# per-boot bake-ms notify digits in frame). The control pair of this exact bed
# measured 0 of 307200 px, so "the stills differ" cannot be boot noise:
#   fog pair    r_volumetric_swirl 0 vs 6      -- the curl displaces the murk
#   water pair  r_waterswirl 0 vs 0.5, murk OFF -- the surface arm alone
# The teleport classification line rides the first boot's log for free (e1m3
# carries one *teleport texture; developer 1 is on).
# ONE boot since 2026-08-21 (was four): both cvars are live uniforms, so the
# swirl arms toggle mid-boot on the frozen bed and the four stills come out of
# one map load. The notify discipline is what keeps the assertion honest: each
# shot's "Wrote" line lands in the NOTIFY area of the next frame, so every shot
# gets its own `clear` a second beforehand -- without that, the pairs would
# differ on console text alone and "the stills differ" would pass with the
# swirl dead. Within one boot the comparison is STRONGER than the old
# cross-boot one: the same bake, the same realisation, zero boot noise.
SANDBOX_M=$(mktemp -d)
LOG_M=$(mktemp)
mkdir -p "$SANDBOX_M/id1"
cat > "$SANDBOX_M/id1/smoketest_m.cfg" <<'EOF'
scr_screenshot_jpeg 0
showfps 0
showtime 0
showdate 0
showbrand 0
cl_nettimesyncfactor 1
cl_nettimesyncboundmode 1
sv_random_seed 1
rt_metal_sameframe 0
vid_vsync 0
r_drawentities 0
r_drawviewmodel 0
r_drawdecals 0
cl_particles 0
viewsize 100
crosshair 0
r_volumetric 1
r_volumetric_wind "0 0 0"
r_volumetric_groundwind "0 0 0"
r_volumetric_swirl 0
rt_metal 0
r_waterscroll 0
r_wateralpha 0.8
r_wateralpha_force 1
r_teleportswirl 0
r_waterswirl 0
defer 5 "clear"
defer 6 "screenshot m_fog0.tga"
defer 6.5 "r_volumetric_swirl 6"
defer 7.5 "clear"
defer 8.5 "screenshot m_fog6.tga"
defer 9 "r_volumetric 0"
defer 9.2 "prvm_edictset server 1 origin \"-1000 -600 -340\""
defer 9.4 "prvm_edictset server 1 angles \"30 90 0\""
defer 9.5 "prvm_edictset server 1 fixangle 1"
defer 11.5 "clear"
defer 12.5 "screenshot m_wat0.tga"
defer 13 "r_waterswirl 0.5"
defer 14 "clear"
defer 15 "screenshot m_wat1.tga"
defer 16.5 quit
EOF
./darkplaces-sdl -userdir "$SANDBOX_M" -window -nosound +developer 1 +sv_freezenonclients 1 +exec smoketest_m.cfg +map e1m3 >>"$LOG_M" 2>&1
check "kh-swirl: teleport texture classified" "M5 teleportswirl: [0-9]+ teleport" "$LOG_M"
if [ -f "$SANDBOX_M/m5/m_fog0.tga" ] && [ -f "$SANDBOX_M/m5/m_fog6.tga" ]; then
	if cmp -s "$SANDBOX_M/m5/m_fog0.tga" "$SANDBOX_M/m5/m_fog6.tga"; then
		failt "kh-swirl: fog swirl moves the murk (stills differ)"
	else
		pass "kh-swirl: fog swirl moves the murk (stills differ)"
	fi
else
	failt "kh-swirl: fog swirl moves the murk (stills missing)"
fi
if [ -f "$SANDBOX_M/m5/m_wat0.tga" ] && [ -f "$SANDBOX_M/m5/m_wat1.tga" ]; then
	if cmp -s "$SANDBOX_M/m5/m_wat0.tga" "$SANDBOX_M/m5/m_wat1.tga"; then
		failt "kh-swirl: water swirl moves the moat (stills differ)"
	else
		pass "kh-swirl: water swirl moves the moat (stills differ)"
	fi
else
	failt "kh-swirl: water swirl moves the moat (stills missing)"
fi

# --- the TELEPORTER starfield (2026-09-01) --------------------------------
# A SECOND boot, and on start.bsp rather than e1m3, for one blunt reason: this
# check needs a teleporter IN FRAME, and e1m3's two teleport faces are not
# reachable from run M's camera -- which is locked to yaw 90 for the whole boot,
# because only the FIRST angles of a boot applies on a frozen bed. start.bsp's
# NORMAL skill hall puts a 48x96 curtain dead centre from (544 1284 48) using
# the map's own spawn heading, so it needs no angles command at all. The camera
# is verified by eye, not inferred: an earlier attempt at an e1m3 camera drew a
# brick wall while the console line still fired, because that line proves a
# teleport batch was SUBMITTED, not that it is in view.
#
# Two checks, deliberately different in kind. The console line is the feature's
# only textual evidence (the arm lives inside a static parm, which appears in no
# permutation number and no shader name); the still pair is the pixels.
cat > "$SANDBOX_M/id1/smoketest_m2.cfg" <<'EOF'
scr_screenshot_jpeg 0
showfps 0
showtime 0
showdate 0
showbrand 0
con_notifytime 0
sv_cheats 1
r_volumetric 0
rt_metal 0
r_drawviewmodel 0
r_waterscroll 0
r_teleportswirl 1
r_teleportswirl_churn 1
defer 3.0 "noclip"
defer 4.0 "prvm_edictset server 1 origin \"544 1284 48\""
defer 6.0 "clear"
defer 7.0 "screenshot m_tel1.tga"
defer 8.0 "r_teleportswirl_churn 8"
defer 10.0 "clear"
defer 11.0 "screenshot m_tel8.tga"
defer 12.5 quit
EOF
./darkplaces-sdl -userdir "$SANDBOX_M" -window -nosound +developer 1 +sv_freezenonclients 1 +exec smoketest_m2.cfg +map start >>"$LOG_M" 2>&1
check  "teleport swirl: the shipped pivot reaches the shader" "teleport swirl: pivot none" "$LOG_M"
if [ -f "$SANDBOX_M/m5/m_tel1.tga" ] && [ -f "$SANDBOX_M/m5/m_tel8.tga" ]; then
	if cmp -s "$SANDBOX_M/m5/m_tel1.tga" "$SANDBOX_M/m5/m_tel8.tga"; then
		failt "teleport swirl: churn rate moves the starfield (stills differ)"
	else
		pass "teleport swirl: churn rate moves the starfield (stills differ)"
	fi
else
	failt "teleport swirl: churn rate moves the starfield (stills missing)"
fi

# (The CONSTANT-STRUCT CEILING bed -- the F3 launch-crash net -- was absorbed
#  into run Q, 2026-08-21: Q's validation boot now carries the full worst-case
#  static-parm stack (volumetrics + fog kernel + liquids + analytic gamma at a
#  non-trivial v_gamma), which is both the ceiling bed AND the abort net in
#  one, since validation traps a short bind at the first draw.)
rm -f "$LOG_M"; rm -rf "$SANDBOX_M"

echo
# --- run P: the bad torch (F6) ----------------------------------------------
# The flicker floor is a SAFETY property -- a lamp that reaches zero is a strobe,
# and 3-30 Hz is the band this must never build -- so it is asserted rather than
# assumed, the shape r_lightningbeam_m5_test established. m5_torch_test sweeps
# 300 s at 1 kHz with the depth forced past its cvar bound.
#
# THE FLOOR CHECK ALONE WOULD BE A TAUTOLOGY and was one twice during development:
# at the worst depth a cvar can reach the envelope bottoms at 0.066, comfortably
# above the 0.040 floor, so the clamp never fires and the assertion would pass
# with the clamp deleted. Hence two independent checks -- the signal's own
# minimum, and the backstop verified directly against an impossible value.
SANDBOX_P=$(mktemp -d)
LOG_P=$(mktemp)
./darkplaces-sdl -userdir "$SANDBOX_P" -window -nosound +developer 1 \
	+m5_torch_test +defer 3 quit >"$LOG_P" 2>&1
check "torch: flicker floor holds"          "M5 torch envelope:.*floor holds"   "$LOG_P"
check "torch: the backstop is live"         "M5 torch backstop:.*backstop live" "$LOG_P"
check "torch: flicker noise is not frozen"  "M5 torch noise:.*decorrelated"     "$LOG_P"
# The lamp must REDDEN as it dips, which is what sells a cooling filament over a
# dimmer knob: green must fall between the healthy and the brownout colour.
# field 6 of "M5 torch colour: 2850K (1.000 0.675 0.389)" and field 5 of
# "dipping to 1900K (1.000 0.517 0.000)" -- the leading "(" rides the red channel,
# so the two indices differ by one and getting it wrong yields a non-numeric "(1.000"
tg=$(grep -aoE "M5 torch colour: [0-9]+K \([0-9.]+ [0-9.]+ [0-9.]+\)" "$LOG_P" | head -1 | awk '{print $6}')
td=$(grep -aoE "dipping to [0-9]+K \([0-9.]+ [0-9.]+ [0-9.]+\)" "$LOG_P" | head -1 | awk '{print $5}')
if [ -n "$tg" ] && [ -n "$td" ] && awk "BEGIN{exit !($td < $tg)}"; then
	pass "torch: colour reddens as it dips (green $tg -> $td)"
else
	failt "torch: colour reddens as it dips (green ${tg:-?} -> ${td:-?})"
fi
rm -f "$LOG_P"; rm -rf "$SANDBOX_P"

# And the lamp must actually light something. Frozen bed, ONE boot since
# 2026-08-21 (m5_torch is a live per-frame cvar, so the two states share one
# map load -- and the within-boot pair is the stronger comparison: same bake,
# same realisation, zero boot noise). Same notify discipline as run M: a
# `clear` on its own defer before each shot, or the pair would differ on the
# first shot's "Wrote" line alone and pass with the lamp dead. Own userdir:
# m5_torch is archived.
if [ "$(uname)" = "Darwin" ]; then
	SANDBOX_P2=$(mktemp -d); LOG_P2=$(mktemp)
	mkdir -p "$SANDBOX_P2/id1"
	cat > "$SANDBOX_P2/id1/smoketest_p.cfg" <<'EOF'
scr_screenshot_jpeg 0
showfps 0
showtime 0
showdate 0
showbrand 0
cl_nettimesyncfactor 1
cl_nettimesyncboundmode 1
sv_random_seed 1
r_drawviewmodel 0
cl_particles 0
crosshair 0
viewsize 100
gl_polyblend 0
rt_metal 1
rt_metal_history 0
rt_metal_sameframe 0
vid_vsync 0
m5_torch 0
rt_metal_walllight 0.8
m5_torch_sway 0
m5_torch_flicker 0
defer 7 "prvm_edictset server 1 origin \"544 288 88\""
defer 10 "clear"
defer 11 "screenshot p_off.tga"
defer 11.5 "m5_torch 1"
defer 13 "clear"
defer 14 "screenshot p_on.tga"
defer 15 "m5_torch_softness 0.35"
defer 16.5 "clear"
defer 17.5 "screenshot p_soft035.tga"
defer 18.5 "m5_torch_softness 1.0"
defer 20 "clear"
defer 21 "screenshot p_soft100.tga"
defer 23 quit
EOF
	./darkplaces-sdl -userdir "$SANDBOX_P2" -window -nosound +developer 1 \
		+sv_freezenonclients 1 +exec smoketest_p.cfg +map e1m3 >>"$LOG_P2" 2>&1
	if grep -q "RT_Metal: device" "$LOG_P2" && [ -f "$SANDBOX_P2/m5/p_off.tga" ] && [ -f "$SANDBOX_P2/m5/p_on.tga" ]; then
		if cmp -s "$SANDBOX_P2/m5/p_off.tga" "$SANDBOX_P2/m5/p_on.tga"; then
			failt "torch: the lamp lights the room (stills differ)"
		else
			pass "torch: the lamp lights the room (stills differ)"
		fi
	else
		echo "SKIP: torch lighting still (no Metal ray-tracing device available)"
	fi
	# THE CONE'S SHOULDER (2026-09-01). m5_torch_softness is the fraction of the
	# way from the cone's outer edge to its axis at which it reaches full
	# brightness. At the historic 0.35 that leaves 80.3% of a 28-degree cone as a
	# FLAT PLATEAU, which is what reads on a wall as a stencilled disc rather than
	# a beam; the knob is what removes it. Same within-boot pair and the same
	# notify discipline as the on/off shots above -- the cvar is live per frame,
	# and sway and flicker are pinned off so the only thing moving is the cone.
	if grep -q "RT_Metal: device" "$LOG_P2" && [ -f "$SANDBOX_P2/m5/p_soft035.tga" ] && [ -f "$SANDBOX_P2/m5/p_soft100.tga" ]; then
		if cmp -s "$SANDBOX_P2/m5/p_soft035.tga" "$SANDBOX_P2/m5/p_soft100.tga"; then
			failt "torch: cone softness reaches the kernel (stills differ)"
		else
			pass "torch: cone softness reaches the kernel (stills differ)"
		fi
	else
		echo "SKIP: torch cone softness still (no Metal ray-tracing device available)"
	fi
	rm -f "$LOG_P2"; rm -rf "$SANDBOX_P2"
fi

ntotal=$((npass + nfail))
if [ "$fail" -eq 0 ]; then
	echo "SMOKE TESTS: all passed ($npass checks)"
else
	echo "SMOKE TESTS: FAILURES (see above) -- $nfail of $ntotal failed"
fi
exit "$fail"
