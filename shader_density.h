/*
QuakeM5 -- constants and shaping shared by the two murk implementations
(METAL.md Phase 2).

WHY THIS EXISTS. The volumetric murk is evaluated twice, in two languages: the
GL march in shader_glsl.h (MODE_VOLUMETRICFOG) and the Metal fog kernel in
rt_metal.m (kFogSrc). CLAUDE.md records the pair as a deliberate lockstep and
warns that changing one and not the other makes the kernel-vs-GL tone match
drift silently. Several of the numbers in that pair were not merely duplicated
but written DIFFERENTLY on each side -- named #defines in the kernel
(LAVAGLOW_H, LAVAGLOW_REF, RT_BEAM_REF) against bare literals in the GL march
(40.0, 256.0, 640.0) -- so the two sites were not textually comparable even in
principle, and no reviewer could diff them.

This file is the single definition of those numbers and of the one shaping
function they feed. Change a value here and BOTH shaders change together.

WHAT IS AND IS NOT SHARED. The constants and the max-channel Reinhard shoulder
are shared, because they are pure arithmetic whose spelling is identical in GLSL
and MSL -- `max`, `+`, `/` and vector `.r/.g/.b` all mean the same thing in both,
so the shoulder needs no type names and no per-language macros at all.

The 95-line density model itself IS shared now, as DPD_DENSITY_MODEL: the GL
march (shader_glsl.h MODE_VOLUMETRICFOG) and the MSL march (shader_msl.h) both
splice this one text, and the name-mapping layer this paragraph used to call
"a separate piece of work" turned out to be twenty local aliases in the MSL
fragment's prologue (`float4 VolumetricParams = u.VolumetricParams;` and its
peers), which fold away entirely. rt_metal.m's kFogSrc is the ONE remaining
copy -- `grep -c DPD_DENSITY_MODEL rt_metal.m` is 0 -- and it is a copy on
purpose: it packs the same numbers into named struct fields on an ABI that is
versioned by a _Static_assert, and reaching it from here would mean either
mapping those names or renaming its ABI.

DPD_LIQUID_FADE (below) is the second shared text, and it is deliberately NOT
the model: it is the GPU twin of the CPU hook R_Volumetric_TransmittanceToPoint,
which is itself documented as a cheap approximation rather than a third
lockstep copy. Both SURFACE arms splice it, which is the lockstep that matters
for it -- there is no shared sidecar underneath the surface shader doing the
work for both backends, so a drift between the two would be a real
GL-vs-Metal divergence rather than a tone mismatch.

Also deliberately NOT shared, and still divergent on purpose (documented in
CLAUDE.md): the march step ceilings (GL 128 vs kernel 64) and the march length
source (raster depth vs RT hit).
*/

#ifndef SHADER_DENSITY_H
#define SHADER_DENSITY_H

// The C-side values. Kept as text rather than numbers because both consumers
// splice them into shader source; a single spelling here is the whole point.
#define DPD_BEAM_REF_STR     "640.0"   // beam path-integral reference, world units
#define DPD_LAVAGLOW_H_STR    "40.0"   // in-air lava glow falloff height, world units
#define DPD_LAVAGLOW_REF_STR "256.0"   // lava glow path-integral reference, world units
#define DPD_LMAX_STR           "3.0"   // fog light max-channel shoulder knee (kernel only)
#define DPD_SWIRL_ADVECT_STR  "0.35"   // swirl-phase advection as a fraction of the wind

/*
The shader-source prelude. Both languages take it verbatim: GLSL accepts the
"f" suffix from #version 120 on and MSL requires nothing special, so one text
serves both. Consumers splice this ahead of their own source.

DPD_SHOULDER is the max-channel Reinhard used in three places (the GL beam term,
the GL lava glow, the kernel beam term). Max-channel rather than a sum norm is
load bearing -- calibrated 2026-08-01 on the demo6 bloom bed -- because a sum
norm crushes ordinary white lights, and the max-channel form keeps a hot
highlight's hue instead of drifting it white. Every channel stays below 1 by
construction, so no knob value can flat-white the RGBA8 composite.

DPD_CURL (F3, the KH swirl) is the analytic divergence-free swirl field the
murk's noise LOOKUPS are displaced by. Each octave is an ABC-flow
(Arnold-Beltrami-Childress) trigonometric velocity spelled as one vector
sin plus one vector cos of rotated swizzles -- sin((q).zxy) + cos((q).yzx)
-- so component i never reads coordinate i and the divergence is
identically zero by construction: Bridson's curl-noise property with no
finite differences, no hash and no texture fetch. Two octaves at 1x/2.7x
with amplitudes 1/0.5; component range about +/-3, RMS about 1.25, so the
amplitude knob reads roughly in world units. TWO octaves and the
swizzle-pair form are a measured cost decision, not taste: the first cut
was three octaves of six independently-phased scalar sin/cos each (18
unfusable transcendentals per call), and at the GL march's 128-step
full-res stress config it HALVED the frame rate (34 vs 70 fps on the
lakeside bed); this form is 12 lane-evaluations the compiler can fuse to
sincos pairs, and each scalar of q feeds exactly one sin and one cos. The
argument is still expanded several times -- bind it to a local first,
never pass an expression with a side effect (the mathlib Vector* lesson,
same shape).

DPD_SWIRL_ADVECT is the fraction of the wind the swirl PHASE rides. It is
deliberately not 1.0: with the phase advecting at exactly the wind speed the
whole lookup becomes a rigid translation of a static (curlier) pattern --
provably pure translation again, which is precisely what the feature exists
to break. At 0.35 the curl field sweeps THROUGH the advecting noise and the
banks shear and roll. With the winds pinned "0 0 0" both offsets are zero and
the swirl is a static function of position, so the frozen murk beds stay
byte-deterministic with no new pins.
*/
#define DPD_SHADER_PRELUDE \
	"#define DPD_BEAM_REF "     DPD_BEAM_REF_STR     "\n" \
	"#define DPD_LAVAGLOW_H "   DPD_LAVAGLOW_H_STR   "\n" \
	"#define DPD_LAVAGLOW_REF " DPD_LAVAGLOW_REF_STR "\n" \
	"#define DPD_LMAX "         DPD_LMAX_STR         "\n" \
	"#define DPD_SWIRL_ADVECT " DPD_SWIRL_ADVECT_STR "\n" \
	"#define DPD_SHOULDER(v) ((v) / (1.0 + max((v).r, max((v).g, (v).b))))\n" \
	"#define DPD_CURL_OCT(q) (sin((q).zxy) + cos((q).yzx))\n" \
	"#define DPD_CURL(q) (DPD_CURL_OCT(q) + 0.5 * DPD_CURL_OCT(2.7 * (q) + vec3(1.7, 4.3, 2.9)))\n"


/*
THE DENSITY MODEL ITSELF, shared by the two MARCHES.

This is the per-step evaluation the murk runs at every march sample: the two 3D
fetches, the height falloff, the noise patch threshold, the corner term, the
ground-fog layer, the surface mist, the liquid override and the lava-glow path
integral. It reads the uniforms, `p`, `dt` and `transmittance`; it writes
`density`, `tint` and `ground`, and accumulates `lavasum`.

WHY IT CAN BE SHARED NOW, when the file header above says it cannot. That header
is still right about the MARCH-versus-KERNEL barrier: rt_metal.m's kFogSrc packs
these sixteen uniforms into named struct fields (cam.grounddensity) where the
marches read vec4 lanes (VolumetricGround.x), so those two cannot share text
without a name-mapping layer. But the two MARCHES -- the GL one in shader_glsl.h
and the MSL one Phase 6-2b adds -- read the IDENTICAL uniform names, because the
Metal uniform seam is name-based reflection (metal_backend.m) and feeds an MSL
arm from the very same R_Shader_Uniform* calls. So the barrier is between the
march and the kernel, not between the two marches, and this text crosses the one
seam it can.

The text is deliberately GLSL-flavoured rather than a neutral dialect, because
GLSL is the reference implementation and a neutral dialect would make the GL
side unreadable to buy nothing. MSL aliases the handful of spellings that differ
(vec2/3/4 -> float2/3/4, and dp_texture3D to its own sampler pair) in its own
prelude; every other token here -- mix, smoothstep, exp, max, the ternary, and
the .rgb/.xyz swizzles -- means the same thing in both languages.

C STRING CONCATENATION IS WHAT MAKES THIS PROVABLE. R_ShaderStrCat joins the
builtinshaderstrings array with no separator, so collapsing 73 array elements
into one concatenated literal cannot change a byte of the assembled source. The
dump filename carries a CRC of it (combined_crc57874.glsl), so the move
announces itself if it is not exact.

The kernel remains the documented second copy. Change this model and rt_metal.m
kFogSrc still has to change with it -- that lockstep is unaffected by this file.
*/
#define DPD_DENSITY_MODEL \
"		// KH SWIRL (F3, r_volumetric_swirl): displace the noise LOOKUP by the\n" \
"		// analytic curl field -- never the world-field fetch, so geometry (floors,\n" \
"		// waterlines, corners) stays honest. The phase rides windoffset at\n" \
"		// DPD_SWIRL_ADVECT (see shader_density.h), so the pinned-wind beds stay\n" \
"		// deterministic and the curl sweeps THROUGH the advecting noise rather\n" \
"		// than translating with it. VolumetricSwirl: x = amplitude (wu),\n" \
"		// y = spatial scale, z = KH interface boost. x == 0 is value-exact the\n" \
"		// old lookup.\n" \
"		vec3 nwoff = p + windoffset;\n" \
"		if (VolumetricSwirl.x > 0.0)\n" \
"		{\n" \
"			vec3 swq = (p + windoffset * DPD_SWIRL_ADVECT) * VolumetricSwirl.y;\n" \
"			nwoff += VolumetricSwirl.x * DPD_CURL(swq);\n" \
"		}\n" \
"		vec3 npos = nwoff * VolumetricNoise.x;\n" \
"		vec3 n = dp_texture3D(Texture_VolumeNoise, npos).rgb;\n" \
"		vec4 world = dp_texture3D(Texture_VolumeField, (p - VolumetricFieldOrigin) * VolumetricFieldParams.xyz);\n" \
"\n" \
"		// Height above the fog bed. With the baked field the bed IS the floor beneath\n" \
"		// this sample, so the murk settles into each room and stays put when the player\n" \
"		// jumps; with it off the bed is the old plane a fixed distance under the eye.\n" \
"		float abovefloor = world.r * VolumetricFieldParams.w - VolumetricLiquid.z;\n" \
"		// LIQUID FLOOR (r_volumetric_liquidfloor, 2026-09-07). Over a pool the\n" \
"		// column's floor is the pool BED, so the air murk read hundreds of units\n" \
"		// up and vanished over every lake while it hung over the bank beside it:\n" \
"		// a hard step along the waterline (measured on e1m4's lake). A liquid\n" \
"		// surface is a floor for the air above it, and the field's signed\n" \
"		// distance IS the height above it there. Uniform branch, so 0 is the old\n" \
"		// expression textually. LOCKSTEP with rt_metal.m kFogSrc and gl_rmain.c\n" \
"		// R_Volumetric_FieldDensity (and the fade's midpoint tap below).\n" \
"		float surfdist = (world.g - 0.5) * 2.0 * VolumetricMist.z;\n" \
"		if (VolumetricLiquidFloor > 0.0)\n" \
"			abovefloor = min(abovefloor, mix(abovefloor, max(-surfdist, 0.0), VolumetricLiquidFloor));\n" \
"		float abovecam = p.z - VolumetricParams.z;\n" \
"		// The second noise field undulates the bed, which is what turns a flat slab\n" \
"		// into layered banks with real vertical structure.\n" \
"		float above = mix(abovecam, abovefloor, VolumetricLiquid.w) + (0.5 - n.g) * VolumetricParams.y * 0.6;\n" \
"		float height = exp(-max(above, 0.0) / VolumetricParams.y);\n" \
"		// Thresholding leaves clear air between banks instead of hazing everything.\n" \
"		float patch = smoothstep(VolumetricNoise.y, 1.0, n.r);\n" \
"		float density = VolumetricParams.x * height * patch;\n" \
"\n" \
"		// CORNERS. Squared, so the term separates a real corner from a flat wall\n" \
"		// instead of just fattening the murk near all geometry -- the difference\n" \
"		// between fog that gathers where it should and generic cloudiness.\n" \
"		density *= 1.0 + VolumetricMist.w * world.a * world.a;\n" \
"		// GROUND FOG (dry ice): a second shallow layer hugging each room's floor.\n" \
"		// Its billow has its OWN noise scale and drift; the top swell rides the\n" \
"		// AIR fetch's third seed (n.b, 625-unit period), so the billow patches\n" \
"		// and the swell move at different velocities and cloud tops morph rather\n" \
"		// than slide. Corner-immune (added after the corner multiply), swallowed\n" \
"		// inside liquids by the mix below. VolumetricGround.x is zeroed at upload\n" \
"		// when there is no baked field or _floor is off (abovefloor would be\n" \
"		// garbage). LOCKSTEP with rt_metal.m kFogSrc -- change one, change both.\n" \
"		float ground = 0.0;\n" \
"		if (VolumetricGround.x > 0.0)\n" \
"		{\n" \
"			// the swell rides the AIR fetch's third seed -- literally the same\n" \
"			// texel, so it costs nothing extra here. VolumetricGround.x is a\n" \
"			// runtime uniform (not a static parm), so at ground 0 this branch\n" \
"			// is skipped coherently at runtime rather than compiled away.\n" \
"			float nb = n.b;\n" \
"			// KH SWIRL on the ground billow: same curl on the ground layer's OWN\n" \
"			// wind phase (the two layers already advect independently, so shear\n" \
"			// layering comes free), with the amplitude boosted in a Gaussian band\n" \
"			// centred one scale-height above the layer base -- the mist/air\n" \
"			// interface, where KH billows live. The boost is gated on floormode\n" \
"			// (VolumetricLiquid.w) because abovefloor is garbage at _floor 0.\n" \
"			vec3 gwoff = p + groundwindoffset;\n" \
"			if (VolumetricSwirl.x > 0.0)\n" \
"			{\n" \
"				vec3 gswq = (p + groundwindoffset * DPD_SWIRL_ADVECT) * VolumetricSwirl.y;\n" \
"				float kh = (abovefloor - VolumetricGround2.y - VolumetricGround.y) / max(VolumetricGround.y, 1.0);\n" \
"				gwoff += (VolumetricSwirl.x * (1.0 + VolumetricSwirl.z * VolumetricLiquid.w * exp(-kh * kh))) * DPD_CURL(gswq);\n" \
"			}\n" \
"			vec2 gn = dp_texture3D(Texture_VolumeNoise, gwoff * VolumetricGround.z).rg;\n" \
"			float gabove = abovefloor - VolumetricGround2.y + (0.5 - nb) * VolumetricGround2.x;\n" \
"			float gfall = exp(-max(gabove, 0.0) / VolumetricGround.y);\n" \
"			float gpatch = smoothstep(VolumetricGround.w, 1.0, gn.r) * (0.75 + 0.5 * gn.g);\n" \
"			ground = VolumetricGround.x * gfall * gpatch;\n" \
"			density += ground;\n" \
"		}\n" \
"\n" \
"		// Mist lying on the liquid surface. The field's signed distance is in world\n" \
"		// units, so this band tracks the waterline exactly, over any shape of pool,\n" \
"		// and decays to nothing well before the next dry cell.\n" \
"		// (surfdist is declared beside abovefloor above: the liquid floor reads it)\n" \
"		// The field's B channel holds the NEAREST span's kind, so this is 1 over\n" \
"		// lava and 0 over water and slime. Hoisted out of the lava-glow term\n" \
"		// below, which already computed exactly this: the mist cut is then two\n" \
"		// more instructions rather than a second smoothstep.\n" \
"		float lavakind = smoothstep(0.75, 1.0, world.b);\n" \
"		// MIST OVER LAVA (r_volumetric_mistlavacut). Cold mist lying on a lake of\n" \
"		// molten rock is the one liquid where the surface band reads wrong; the\n" \
"		// proximity gate is the mist term's own exp, so B alone is enough here.\n" \
"		density += VolumetricMist.x * exp(-max(-surfdist, 0.0) / VolumetricMist.y) * (1.0 - VolumetricMistLavaCut * lavakind);\n" \
"		// LAVA GLOW (r_volumetric_lavaglow, VolumetricGround2.z): warm emission\n" \
"		// the AIR picks up near lava, keyed on the field's kind channel (B holds\n" \
"		// the NEAREST span's kind even in dry cells) and the signed distance. A\n" \
"		// path integral shouldered after the march like the beams; 40/256 are\n" \
"		// the kernel's LAVAGLOW_H/LAVAGLOW_REF lockstep constants (rt_metal.m\n" \
"		// kFogSrc prelude) -- change one, change both.\n" \
"\n" \
"		vec3 tint = VolumetricColor;\n" \
"\n" \
"		// Inside a liquid the murk is thick, uniform and takes that liquid's own\n" \
"		// colour: no height falloff, no wind-blown banks, because water is not air.\n" \
"		// The field stores a SIGNED DISTANCE, so this boundary reconstructs the\n" \
"		// waterline the player can see rather than snapping to a 64-unit cell edge.\n" \
"		float inliquid = smoothstep(0.49, 0.51, world.g) * VolumetricLiquid.y;\n" \
"		vec3 liquidtint = world.b < 0.25 ? VolumetricWaterColor\n" \
"		                : (world.b < 0.75 ? VolumetricSlimeColor : VolumetricLavaColor);\n" \
"		// PER-LIQUID DENSITY: the same kind selection the tint does two lines\n" \
"		// down, so a lava lake need not carry a density tuned for swimming in\n" \
"		// water. VolumetricLiquidDens = (water, slime, lava); all three are\n" \
"		// equal unless r_volumetric_slimedensity/_lavadensity are set off -1.\n" \
"		float liqdens = world.b < 0.25 ? VolumetricLiquidDens.x\n" \
"		              : (world.b < 0.75 ? VolumetricLiquidDens.y : VolumetricLiquidDens.z);\n" \
"		density = mix(density, liqdens, inliquid);\n" \
"		tint = mix(tint, liquidtint, inliquid);\n" \
"		// the ground layer's own colour, weighted by its share of the total\n" \
"		// density; (1 - inliquid) swallows it inside liquids like the density\n" \
"		// term. Divide guard: everything here can be exactly 0.\n" \
"		tint = mix(tint, VolumetricGroundColor, (1.0 - inliquid) * ground / max(density, 1e-4));\n" \
"		// AMBIENT IRRADIANCE (r_volumetric_ambient): the murk's authored colour\n" \
"		// is scaled by the level's own static lighting, so dark rooms give dark\n" \
"		// fog and the ground mist stops glowing on its own. Air + ground tint\n" \
"		// only -- max(inliquid, ...) keeps the authored liquid colours\n" \
"		// untouched, matching the kernel's in-liquid light suppression. The\n" \
"		// *2.0 undoes the grid's 128-equals-lit storage; the min() caps the\n" \
"		// multiplier at 1 so ambient can only DARKEN the authored look, never\n" \
"		// overbrighten it (light ADDING is the light loop's and the kernel's\n" \
"		// job). VolumetricIrr.z = 0 is the old bytes exactly. Deliberately NOT\n" \
"		// applied to the lava glow below: that term is EMISSION.\n" \
"		vec3 irr = dp_texture3D(Texture_VolumeIrr, (p - VolumetricIrrOrigin) * VolumetricIrrInvSize).rgb;\n" \
"		tint *= mix(min(vec3(VolumetricIrr.y) + VolumetricIrr.x * (irr * 2.0), vec3(1.0)), vec3(1.0), max(inliquid, 1.0 - VolumetricIrr.z));\n" \
"		lavasum += transmittance * lavakind * exp(-max(-surfdist, 0.0) / DPD_LAVAGLOW_H) * (1.0 - inliquid) * dt * (1.0 / DPD_LAVAGLOW_REF);\n"

/*
DPD_LIQUID_FADE -- the murk, applied per fragment to a BLENDED liquid surface.

WHY IT IS NOT THE MARCH. The volumetric murk is a screen-space pass composited
inside R_RenderScene, BEFORE R_MeshQueue_RenderTransparent, and transparent
surfaces write no depth -- so the moment r_wateralpha_force makes water render
blended it leaves the pass that WAS fogging it correctly and reads at full
brightness through fog thick enough to hide the wall behind it. Moving the murk
after the transparent queue is the obvious fix and is wrong: the murk reads
scene depth, so a water pixel would then be fogged at the depth of the wall
BEHIND it, and near water in a deep room would come out heavily fogged. The
ordering is deliberate and the tree's own comment says so.

The hook that works is the one classic fog has always used for exactly this
class: evaluate the attenuation inside the surface's OWN shader, off the
fragment's own eye vector, touching no depth buffer. One line in the epilogue.

IT MODULATES ALPHA, AND NEVER A COLOUR. THIS IS THE WHOLE DESIGN, AND THE FIRST
CUT GOT IT WRONG IN A WAY WORTH RECORDING (2026-08-30, rejected 2026-08-31).

That cut lerped the surface toward a RECONSTRUCTED fog colour -- the authored
r_volumetric_color_*, shaded toward the ground layer's. It looked right on the
bed it was measured on and wrong on the configuration Seb plays, and the reason
is that the authored colours are only the murk's UNLIT BASE. What is actually
on screen is that base darkened by the ambient irradiance grid and then ADDED
to, per march step, by rt_metal_fog's in-scattered light (which carries the
winning light's own hue), by the density-independent beam term, and by the lava
glow. vid_sdl.c says it in as many words: "the authored fog colours are the
unlit base". So as transmittance fell, the water converged to a flat dark
constant while the fog around it stayed lit and varying -- a uniform patch with
a hard edge, which is exactly how it was reported: "from a distance it looks
black through the bluegreen fog", and worse the further away.

Scaling ALPHA cannot make that mistake, because it never forms an opinion about
the fog's colour. As the surface fades out, what shows through is the murk's OWN
composited pixel -- the real lit fog, with the beams, the irradiance, the
stochastic light's hue and any map _fog override already in it. Correct by
construction rather than by reconstruction, and it deletes five tint uniforms.

The arithmetic is exact at both ends. The murk has already painted
S_full + backdrop*T_full at this pixel; writing a for r_wateralpha and T for the
transmittance eye->surface, the blend gives W*a*T + (1 - a*T)*(murk pixel):
  T -> 1   the surface at its authored opacity, unchanged
  T -> 0   exactly the murk's own pixel, whatever colour the murk made it
Between those it is a good approximation rather than an identity, which is the
same honesty the CPU hook carries.

WHAT IT EVALUATES, and its ancestry. This is the GPU twin of
R_Volumetric_TransmittanceToPoint / R_Volumetric_FieldDensity (gl_rmain.c) --
the CPU hook that fades PARTICLES for the same structural reason -- and it is a
LOCKSTEP PAIR WITH THAT C CODE, not with the march. Keep the two in step: same
clamps, same terms, same 0.01 optical-depth convention (the march accumulates
exp(-density * dt * 0.01), so tau over a segment is 0.01 * mean density *
length).

ONE FIELD FETCH, TAKEN AT THE MIDPOINT, and the midpoint is load bearing. The
obvious one-tap position is the fragment itself, and it is the one position that
cannot work: a liquid fragment sits ON the waterline, the field's G channel is a
signed distance whose 0.5 IS the surface, and the density evaluator's first
branch returns the in-liquid density wholesale for sdf > 0. Trilinear
interpolation straddles that boundary, so roughly half the fragments of any
water surface would take the liquid override and half the air branch -- a hard,
noisy, half-and-half over-fog painted precisely on the water. The midpoint of
eye and fragment is in the medium the ray actually crosses, it is second-order
accurate where the endpoint rule is first-order, and it costs the same one
fetch. (The lava shimmer already dodges the same boundary, by offsetting its
probe 8 units towards the camera.)

It ignores the noise `patch` terms for the same reason the CPU hook does -- they
break the murk into banks and only the covered fraction really attenuates -- so
it OVER-estimates, and r_volumetric_liquidfade is the calibration, exactly as
r_volumetric_particles is for the CPU hook.

WHAT IT NEEDS from the arm that splices it: Texture_VolumeField and the
LiquidFade* uniforms. It writes `lfT` alone; the caller applies it to alpha,
because that one line is the only thing the two languages spell differently.

UNIFORM PACKING -- mirrored EXACTLY in gl_rmain.c's R_SetupShader_Surface:
  LiquidFade            x strength (0 = the exact no-op), y max distance,
                        z field sdf range, w flooroffset
  LiquidFadeAir         x density, y height (>=1), z corner, w field max height
  LiquidFadeGround      x ground density * floormode, y ground height (>=1),
                        z ground offset, w surface mist * watermode
  LiquidFadeLiquid      x in-liquid density, y watermode, z mist height (>=1), w liquid floor (r_volumetric_liquidfloor)
  LiquidFadeOrigin      the eye, WORLD space
  LiquidFadeFieldOrigin / LiquidFadeFieldScale   world -> field texcoord
There is deliberately NO tint uniform of any kind. If one ever reappears here,
the defect above has been reintroduced.
*/
#define DPD_LIQUID_FADE \
"	// eye -> fragment, both in world space (M5WorldPosition is Attrib_Position\n" \
"	// through ModelToWorld, so this is right on brush-model liquids too).\n" \
"	vec3  lfseg  = M5WorldPosition - LiquidFadeOrigin;\n" \
"	float lfdist = min(length(lfseg), LiquidFade.y);\n" \
"	// THE MIDPOINT, never the fragment -- see the block comment above.\n" \
"	vec3  lfp    = LiquidFadeOrigin + lfseg * 0.5;\n" \
"	vec4  lff    = dp_texture3D(Texture_VolumeField, (lfp - LiquidFadeFieldOrigin) * LiquidFadeFieldScale);\n" \
"	float lfabove = lff.r * LiquidFadeAir.w - LiquidFade.w;\n" \
"	float lfsdf   = (lff.g - 0.5) * 2.0 * LiquidFade.z;\n" \
"	// LIQUID FLOOR, LOCKSTEP with DPD_DENSITY_MODEL: the surface is the air's floor\n" \
"	if (LiquidFadeLiquid.w > 0.0)\n" \
"		lfabove = min(lfabove, mix(lfabove, max(-lfsdf, 0.0), LiquidFadeLiquid.w));\n" \
"	float lfdens;\n" \
"	if (lfsdf > 0.0 && LiquidFadeLiquid.y > 0.0)\n" \
"	{\n" \
"		// inside a liquid the murk is thick and uniform -- the density model's\n" \
"		// own in-liquid branch, and the CPU hook's early return. Only the\n" \
"		// DENSITY is taken from it; the liquid's colour is the murk's business\n" \
"		// and is already in the pixel this surface is fading into.\n" \
"		lfdens = LiquidFadeLiquid.x;\n" \
"	}\n" \
"	else\n" \
"	{\n" \
"		lfdens  = LiquidFadeAir.x * exp(-max(lfabove, 0.0) / LiquidFadeAir.y);\n" \
"		lfdens *= 1.0 + LiquidFadeAir.z * lff.a * lff.a;\n" \
"		lfdens += LiquidFadeGround.x * exp(-max(lfabove - LiquidFadeGround.z, 0.0) / LiquidFadeGround.y);\n" \
"		lfdens += LiquidFadeGround.w * exp(-max(-lfsdf, 0.0) / LiquidFadeLiquid.z);\n" \
"	}\n" \
"	// the march's own extinction convention: 0.01 * density * length\n" \
"	float lfT = exp(-lfdens * lfdist * 0.01 * LiquidFade.x);\n"

/*
NOTE, 2026-08-31: DPD_RTLIQUID_BLUR lived here and was REMOVED after measurement.

The defect it targeted is real -- rt_metal_liquids multiplies the RT lighting
term into blended water, liquids are deliberately absent from the ray-tracing
structure so that term is the lighting of whatever lies UNDER the surface, and
the submerged geometry's SHADOWS get printed onto the water (on e1m1 the pillar
bases read clearly through the slime). But a low-pass is the wrong instrument
for it: the printed feature is a shadow about 85 viewport pixels wide, not
high-frequency detail, so a 13-tap disc moved the band's contrast from 13.49 to
13.40 / 13.24 / 12.82 levels at radii of 6 / 12 / 24 px -- against a target of
8.4, which is what the same frame reads with the term switched off entirely.
A radius large enough to matter would need far more taps than the effect is
worth, and a sparse wide disc reproduces the band as ghosts instead of removing
it. Kept as a note rather than deleted silently, because "blur it" is the
obvious first idea and it does not work.

What DID work is one clamp, and it is in the surface arms rather than here
because it is a single expression in each language: the shadow is a DARK
excursion, so flooring the term removes it while leaving the term's brightening
free. See rt_metal_liquids_minlight.
*/

#endif // SHADER_DENSITY_H
