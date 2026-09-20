/*
Copyright (C) 2003  T. Joseph Carter

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/
#undef WIN32_LEAN_AND_MEAN  //hush a warning, SDL.h redefines this
#include <SDL.h>
#include <stdio.h>

#include "quakedef.h"
#include "image.h"
#include "utf8lib.h"

#ifndef __IPHONEOS__
#ifdef MACOSX
#include <Carbon/Carbon.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDParameter.h>
#include <IOKit/hidsystem/event_status_driver.h>
#if (MAC_OS_X_VERSION_MIN_REQUIRED < 120000)
	#define IOMainPort IOMasterPort
#endif
static cvar_t apple_mouse_noaccel = {CF_CLIENT | CF_ARCHIVE, "apple_mouse_noaccel", "1", "disables mouse acceleration while DarkPlaces is active"};
#include "rt_metal.h"
#include "vid_metal.h"
#include "dpcmdtrace.h"
cvar_t rt_metal = {CF_CLIENT | CF_ARCHIVE, "rt_metal", "0", "composite in-process Metal ray-traced soft shadows over the 3D scene (shared IOSurface). 0 = off"};
// runtime tuning knobs for the Metal RT soft shadows (see rt_metal.m kernel)
cvar_t rt_metal_samples  = {CF_CLIENT | CF_ARCHIVE, "rt_metal_samples",  "8",    "RT soft-shadow rays per pixel (1-64); more = smoother penumbra, higher GPU cost"};
cvar_t rt_metal_scale    = {CF_CLIENT | CF_ARCHIVE, "rt_metal_scale",    "0.5",  "RT trace resolution as a fraction of the viewport (0.25-1). 1 = per-pixel tracing (the sharpest, most expensive look); 0.5 = quarter the rays for a large GPU saving with slightly softer shadow edges"};
cvar_t rt_metal_smoothnormals = {CF_CLIENT | CF_ARCHIVE, "rt_metal_smoothnormals", "1", "shade RT hits on entities (monsters, items) with interpolated vertex normals instead of flat per-triangle ones. Fixes low-poly models reading as faceted when a muzzle flash or explosion lights them; 0 restores the old flat look"};
cvar_t rt_metal_reproject_depth = {CF_CLIENT, "rt_metal_reproject_depth", "1", "make the RT reprojection TRANSLATION-aware as well as rotation-aware, using the per-pixel hit distance the trace now writes into the term buffer's alpha. Fixes the parallax fringing on entity silhouettes at close range (strafing past an Ogre at arm's length); 0 keeps the rotation-only remap, which is exact for turning but ignores the camera moving between the traced and shown frame. Console-only A/B switch"};
cvar_t rt_metal_reproject = {CF_CLIENT | CF_ARCHIVE, "rt_metal_reproject", "1", "reproject the RT lighting through the camera it was traced with, so it no longer trails the view by a frame on fast mouse turns (the drunken-lag fix). 0 restores the old screen-locked composite"};
cvar_t rt_metal_sameframe = {CF_CLIENT | CF_ARCHIVE, "rt_metal_sameframe", "1", "trace and composite the SAME frame instead of showing the previous frame's trace remapped through the current camera (the async mode, 0). Removes the one-frame fringing at close silhouettes and on monsters under wall lighting; measured cost 12-15% fps at settings that saturate the GPU, because the trace leaves the async pipeline and runs serially -- rt_metal_scale and rt_metal_fog_steps are the compensating levers. Reprojection is a provable no-op while this is on. Default 1 since Phase 8-6 QA (2026-08-09, Seb's verdict)"};
cvar_t rt_metal_lightcores = {CF_CLIENT | CF_ARCHIVE, "rt_metal_lightcores", "1", "lift torch/brazier flame models out of the RT shadow-caster set into a separate emissive instance: their light stops being blocked by their own flame mesh (torches self-shadowed to black) and the flames themselves render undarkened (bright yellow, as authored). 0 restores the old behaviour"};
cvar_t rt_metal_lavaemissive = {CF_CLIENT | CF_ARCHIVE, "rt_metal_lavaemissive", "1", "put lava sheets into the ray-traced world as an emissive surface: primary rays stop AT the lava (they used to pass through and print the sunken geometry's lighting onto the sheet) and its fullbright glow survives the RT composite undarkened. Shadow rays still pass through lava, like the torch light-cores. 0 restores the old leak"};
cvar_t rt_metal_lavalights = {CF_CLIENT | CF_ARCHIVE, "rt_metal_lavalights", "1", "lava lakes feed warm lights into the ray tracer (one per 256-unit patch of lava surface, gathered at map load): walls above lava glow orange with real shadows and the fog kernel picks the light up. The value scales the brightness; 0 disables"};
cvar_t rt_metal_skyopen = {CF_CLIENT | CF_ARCHIVE, "rt_metal_skyopen", "1", "treat sky brushes as OPEN SKY in the ray tracer instead of ordinary walls: primary rays pass through to the sky sentinel, so wall lighting stops multiplying the sky sphere by a brush's lighting term (e1m1's dark sky boxes), the fog kernel's sky handling and r_volumetric_skyfog actually engage, and god-ray beams genuinely cross sky windows. Shadow rays already ignored the sky instance's occlusion by design. 0 = sky brushes back in the caster set, the old behaviour exactly"};
cvar_t rt_metal_glowpass = {CF_CLIENT | CF_ARCHIVE, "rt_metal_glowpass", "1", "authored EMISSION survives wall lighting: on frames the RT composite multiplies (rt_metal_walllight > 0), glow/fullbright layers and r_redglow are withheld from the scene passes and re-added ADDITIVELY after the multiply, so a lamp face glows at its authored brightness however the lighting term shades it (a shadowed slot lamp used to render extinguished). Fog still darkens the re-added glow. 0 = the old behaviour exactly (emission multiplied by the lighting term)"};
cvar_t rt_metal_liquidemissive = {CF_CLIENT | CF_ARCHIVE, "rt_metal_liquidemissive", "1", "opaque water/slime join the ray-traced world as an EMISSIVE surface, like lava: primary rays stop AT the liquid (they used to pass through and print the pool floor's lighting through the sheet -- e1m1's machinery visible under the slime) and the sheet renders exactly as authored, the murky texture rt_metal 0 shows. Shadow rays still pass through (liquids never occlude), and the fog stops at the surface. Structurally inert when water renders transparent (r_wateralpha below 1 with r_wateralpha_force or map support) -- rt_metal_liquids owns that configuration. 0 = the old printthrough exactly"};
cvar_t rt_metal_softness = {CF_CLIENT | CF_ARCHIVE, "rt_metal_softness", "0.12", "RT penumbra softness: area-light radius as a fraction of light radius (bigger = softer edges)"};
cvar_t rt_metal_darkness = {CF_CLIENT | CF_ARCHIVE, "rt_metal_darkness", "0.40", "RT shadow darkness floor (0 = deepest shadows, 1 = no darkening). Over the lightmap it is the scene multiplier where fully shadowed; in wall-lighting mode (rt_metal_walllight > 0) it floors the dominant light's shadow term, 0 matching the old full-depth shadows"};
cvar_t rt_metal_culldist = {CF_CLIENT | CF_ARCHIVE, "rt_metal_culldist", "1000", "RT world-light view-relevance cull distance (units beyond a light's radius)"};
cvar_t rt_metal_history  = {CF_CLIENT | CF_ARCHIVE, "rt_metal_history",  "0.9",  "RT temporal accumulation weight 0..0.98 (0 = off; higher = smoother/less grain but slower to react to moving shadows)"};
cvar_t rt_metal_bluenoise= {CF_CLIENT | CF_ARCHIVE, "rt_metal_bluenoise","1",    "blue-noise jitter for the RT kernels' sampling and the fog light pick's random stream: the fog/shadow grain reads as featureless noise instead of the woven dither mesh. Since 2026-09-03 the table is SPATIOTEMPORAL (64x64x16, Wolfe et al. 2022): each texel's frame sequence is blue in time as well, so consecutive frames' errors cancel under the fog's temporal smoothing instead of piling up. 0 restores the classic interleaved-gradient dither exactly (one-off shader rebuild on change)"};
cvar_t rt_metal_color    = {CF_CLIENT | CF_ARCHIVE, "rt_metal_color",    "0.6",  "RT colour strength. Over the lightmap: dynamic-light colored brighten (0 = grey shadows only; higher = explosions/flashes flare the room in their real colour). In wall-lighting mode (rt_metal_walllight > 0): saturation of the RT lighting term (1 = neutral/old look, below greys the light, above exaggerates hue)"};
cvar_t rt_metal_walllight= {CF_CLIENT | CF_ARCHIVE, "rt_metal_walllight","0",    "RT FULL wall lighting (EXPERIMENTAL): 0 = RT shadows composited over the baked lightmap; >0 = render the world fullbright (albedo) and let RT compute ALL per-pixel lighting, replacing the lightmap (dynamic lights fold in automatically). 1 = normal brightness, higher = brighter. Looks great for world/exploration views. Emissive effects (explosions/flashes/flames) are self-lit and not RT-relit; the view weapon is lit from the same light list by rt_metal_viewmodel (on by default since 2026-08-21 -- it is masked out of the screen-space composite, so it needs its own term); water/slime CAN receive the term via rt_metal_liquids (with r_wateralpha_force 1 on stock maps)"};
cvar_t rt_metal_ambient  = {CF_CLIENT | CF_ARCHIVE, "rt_metal_ambient",  "0.15", "RT wall-lighting ambient fill: base light so surfaces facing away from every light don't go pure black (only used when rt_metal_walllight > 0)"};
// god rays: a second, smaller RT pass marching K jittered points along each pixel's view
// ray, shadow-testing the locally dominant light at each, consumed by the volumetric murk
cvar_t rt_metal_shafts           = {CF_CLIENT | CF_ARCHIVE, "rt_metal_shafts", "0", "RT god rays: volumetric light shafts traced by the Metal sidecar and drawn by the volumetric fog pass. Needs rt_metal 1 and r_volumetric 1"};
cvar_t rt_metal_shafts_samples   = {CF_CLIENT | CF_ARCHIVE, "rt_metal_shafts_samples",   "6",    "shaft samples along each view ray (1-16); more = smoother beams, higher GPU cost"};
cvar_t rt_metal_shafts_scale     = {CF_CLIENT | CF_ARCHIVE, "rt_metal_shafts_scale",     "0.5",  "shaft buffer resolution as a fraction of the RT viewport (0.125-1); beams are soft, so low is fine"};
cvar_t rt_metal_shafts_intensity = {CF_CLIENT | CF_ARCHIVE, "rt_metal_shafts_intensity", "0.5",  "strength of the shaft light added into the murk; 0 disables consumption without stopping the trace. 0.5 is the designed midpoint; the beam term is soft-limited so even high values cannot white the frame out"};
cvar_t rt_metal_shafts_history   = {CF_CLIENT | CF_ARCHIVE, "rt_metal_shafts_history",   "0.5",  "temporal smoothing of the shaft buffer 0-0.9 (0 = off; higher = calmer beams, slower to react)"};
cvar_t rt_metal_shafts_dist      = {CF_CLIENT | CF_ARCHIVE, "rt_metal_shafts_dist",      "2000", "maximum shaft distance in world units; also the ray length used against sky"};
cvar_t rt_metal_shafts_residual  = {CF_CLIENT | CF_ARCHIVE, "rt_metal_shafts_residual",  "0",    "weight of the UNSHADOWED non-dominant lights in the shaft term (0-1); 0 keeps only the occlusion-structured beams"};
cvar_t rt_metal_liquids          = {CF_CLIENT | CF_ARCHIVE, "rt_metal_liquids",          "0",    "strength of the RT lighting term applied to water/slime surfaces (0-1). They draw after the RT composite and otherwise never receive it -- bright and flat, worst in wall-lighting mode. 0 = off (previous look), 1 = the same lighting the walls got"};
cvar_t rt_metal_liquids_minlight = {CF_CLIENT | CF_ARCHIVE, "rt_metal_liquids_minlight", "5",    "a FLOOR under the RT lighting term that rt_metal_liquids multiplies into blended water and slime. The primary ray passes THROUGH the liquid surface, so that term is the lighting of whatever lies UNDER it, and the submerged geometry's SHADOWS get printed onto the water: on e1m1 the bases of the pillars read clearly through slime the eye should barely see into. A shadow is a DARK excursion, so clamping the term from below removes it while leaving it free to brighten, which is what the feature is for. THE USEFUL RANGE IS 1-5 AND NOT 0-1: under wall lighting the term is Lsum * rt_metal_walllight * 6 shouldered at rt_metal_lmax, so it runs to that knee's asymptote (lmax * 5/3, i.e. 4.17 at the default knee) -- a first ladder at 0.3-0.7 read as doing nothing at all. THE TRADE, since the default sits at the top of that range: above the asymptote the term is clamped everywhere, so no submerged shape survives, but it also stops VARYING and the feature degenerates to a uniform brightening. The printed geometry and the spatial variation are the same signal and no post-hoc filter of a wrong quantity can separate them -- the real fix is to give liquids their own term, the UNSHADOWED light sum at the surface, which the surface kernel already computes on both shading arms. Note that liquids are NOT kept out of the ray-tracing structure -- opaque ones are instance 5 there, and every shadow ray uses a mask that cannot see them -- so what forces a second term is the pass ordering, not the structure; CLAUDE.md carries the corrected fact. 0 = the unclamped term, byte for byte. Inert unless rt_metal_liquids is above 0, which is off by default"};
// full in-kernel fog lighting: the Metal sidecar owns the whole volumetric integral,
// evaluating density AND per-step lighting; supersedes the screen-space god rays
cvar_t rt_metal_fog           = {CF_CLIENT | CF_ARCHIVE, "rt_metal_fog", "0", "full RT fog lighting: the Metal kernel marches the volumetric fog itself, lighting every step from the map's lights with occlusion. Needs rt_metal 1 and r_volumetric 1. Supersedes rt_metal_shafts while on. The murk falls back to the GL march whenever the kernel output is unavailable"};
cvar_t rt_metal_fog_steps     = {CF_CLIENT | CF_ARCHIVE, "rt_metal_fog_steps",     "16",  "fog kernel march steps per pixel (2-64); the dominant cost together with _stride"};
cvar_t rt_metal_fog_scale     = {CF_CLIENT | CF_ARCHIVE, "rt_metal_fog_scale",     "0.5", "fog kernel buffer resolution as a fraction of the RT viewport (0.125-1)"};
cvar_t rt_metal_fog_intensity = {CF_CLIENT | CF_ARCHIVE, "rt_metal_fog_intensity", "0.55", "strength of the RT light added to the fog per step; the authored fog colours are the unlit base. 0.5 is the designed midpoint; the light is soft-limited so even the console maximum cannot white the frame out"};
cvar_t rt_metal_fog_stride    = {CF_CLIENT | CF_ARCHIVE, "rt_metal_fog_stride",    "2",   "shadow rays fire every Nth march step and hold between (1-8); higher = cheaper, softer light edges along the ray"};
cvar_t rt_metal_fog_history   = {CF_CLIENT | CF_ARCHIVE, "rt_metal_fog_history",   "0.5", "temporal smoothing of the fog kernel output 0-0.9"};
cvar_t rt_metal_fog_residual  = {CF_CLIENT | CF_ARCHIVE, "rt_metal_fog_residual", "0.1", "weight of the UNSHADOWED non-dominant lights in the fog light term (0-1). This is what lets faint warm lights (torches) tint the fog where a brighter light dominates the shadow ray; 0 = dominant light only (the old look). 0.25 until 2026-09-06 -- the fill washes the fog's per-light shadow structure out, and Seb's eye took 0.1 (\"sharp1\") over it"};
cvar_t rt_metal_fog_beams     = {CF_CLIENT | CF_ARCHIVE, "rt_metal_fog_beams",     "0.5", "strength of the DENSITY-INDEPENDENT god-ray term: beams glow even in clear air, on top of the density-coupled fog light. 0.5 = clearly visible god rays (the designed midpoint), 1 = thick bloom that saturates softly rather than whiting out; console values above 1 push into diminishing returns. 0 = beams only where fog is"};
// depth-aware (bilateral) magnification of the fog kernel's buffer in the murk composite
cvar_t rt_metal_fog_upsample  = {CF_CLIENT | CF_ARCHIVE, "rt_metal_fog_upsample",  "1",   "depth-aware upsample of the fog kernel's low-resolution buffer: at every silhouette the fog is rebuilt from the taps whose march ended on the same surface as the pixel, so the stepped fog edges the buffer's pitch used to leave (the 480-px staircase along walls against sky, pillars against far walls) resolve at full resolution. Interiors are untouched. 0 = the plain bilinear magnification, byte for byte"};
cvar_t rt_metal_lmax          = {CF_CLIENT | CF_ARCHIVE, "rt_metal_lmax", "2.5", "soft ceiling on the RT wall-lighting term (rt_metal_walllight > 0): above this knee the term rolls off smoothly (hue kept) and can never exceed knee x 5/3, so a wall beside its own light or a monster's lit ridge stops multiplying its texture detail far past white -- the bright single pixels seen at silhouettes and fixtures under HDR + MetalFX. Below the knee nothing changes. Measured on the M5 look: the frame's median term is 1-2, its top 5% above 3.2, its hottest texels 5-7. 0 = no ceiling (the old look exactly); 2-3 is the sensible range"};
// EVERY LIGHT CASTS A SHADOW (2026-08-17): stochastic light selection in all three
// RT kernels. Before it, only the locally loudest light was ever shadow-tested.
cvar_t rt_metal_lightsample   = {CF_CLIENT | CF_ARCHIVE, "rt_metal_lightsample", "1", "let every light cast shadows IN THE FOG, not just the brightest one at each point: each fog march step picks one light at random in proportion to the light it actually scatters there and shadow-tests that, weighted by the pick probability -- over a few frames every light, torches included, is correctly shadowed and the god-ray term gains real per-light structure. 0 = dominant light only with its unshadowed fill (rt_metal_fog_residual), which by construction lets torches warm fog through solid walls. 1 = fog and god rays only (the fill is ignored -- it would double-count). 2 = also the surface kernel's stochastic second light, the 2026-08-17 estimator that QA rejected as speckly under wall lighting; kept for A/B only"};
cvar_t rt_metal_lightsample_rays = {CF_CLIENT, "rt_metal_lightsample_rays", "1", "shadow rays spent on the stochastically chosen SECOND light in the surface kernel (1-16). The dominant light keeps its own rt_metal_samples rays and this is added on top, so 1 is about a 12%% ray-budget increase at the default 8. Higher = less noise in the extra shadows, at proportional cost. Inert unless rt_metal_lightsample is 2 (the surface arm; modes 0 and 1 never read it). Console-only"};
// THE VIEW WEAPON GETS ITS LIGHT BACK (2026-08-21). Wall lighting forces
// r_fullbright, which strips RENDER_LIGHT from EVERY entity -- and the gun is the
// only opaque thing that is then also masked out of the RT composite that gives
// everything else its lighting back. So it rendered at flat albedo, identical in
// every room, with the muzzle flash and the handlamp silently disconnected from
// it. These light it from the SAME light list the sidecar traces, so it belongs
// to the room rather than to a divorced lightmap scale.
cvar_t rt_metal_viewmodel = {CF_CLIENT | CF_ARCHIVE, "rt_metal_viewmodel", "1", "light the VIEW WEAPON from the RT light list while wall lighting is on (rt_metal_walllight > 0), instead of leaving it flat-albedo. It picks up each room's own brightness and colour, the dominant light's direction across the model, torch flicker, and every dynamic light -- so the muzzle flash, the handlamp and a passing rocket light your hands again. Strength scale: 1 = matched to the wall term, higher = brighter, 0 = off (the old flat gun, byte for byte). Inert at rt_metal_walllight 0, where the engine's own model lighting already works"};
cvar_t rt_metal_viewmodel_shadows = {CF_CLIENT | CF_ARCHIVE, "rt_metal_viewmodel_shadows", "4", "how many of the brightest lights get a real occlusion test when lighting the view weapon (0-16). This is what makes the gun go dark as you step behind a pillar instead of staying lit through it. Cost is this many CPU tracelines per frame REGARDLESS of how many lights the map has, so it does not scale with map size. 0 = no shadow tests (every light treated as visible)"};
cvar_t rt_metal_viewmodel_smooth = {CF_CLIENT, "rt_metal_viewmodel_smooth", "0.08", "seconds of smoothing on the view weapon's lighting (roughly the time to cover 63%% of a change). Without it the gun pops as the dominant light hands over between two lights while you walk, and as a shadow test flips. 0 = no smoothing, instant response. Console-only tuning knob"};
cvar_t rt_metal_fog_upsample_depth = {CF_CLIENT, "rt_metal_fog_upsample_depth", "0.1", "how far apart (as a fraction of the pixel's own distance) two depths may be before the fog upsample treats them as different surfaces (0.02-0.5 in practice; the floor is 0.001, which puts every pixel on the four-tap path and is the cost-measurement recipe). Lower = sharper edges, more surfaces split; higher = closer to plain bilinear. Console-only tuning knob"};
cvar_t rt_metal_fog_stepjitter = {CF_CLIENT | CF_ARCHIVE, "rt_metal_fog_stepjitter", "0", "spread the fog march's jitter ALONG each ray instead of using one offset for the whole ray (S2 of the fog-buffer plan). With one offset per texel, every march step sits on the same sawtooth phase and neighbouring texels' phases differ by the IGN lattice -- which IS the standing fog grid test/roll.py measures. A golden-ratio walk gives each step its own phase. MEASURED 2026-08-18 AND IT MAKES THE GRID WORSE: on the raw fog kernel (no scaler, no history) the Nyquist amplitude rises 0.0068 -> 0.0085 (+24%) and the high-pass energy +19%, because moving each step's sample position also moves where it reads the noise volume, and at the march's spacing that ADDS lattice structure rather than averaging it. Kept as an A/B switch, default 0 = the old march byte for byte. Do not enable"};
cvar_t rt_metal_fog_stride_adaptive = {CF_CLIENT | CF_ARCHIVE, "rt_metal_fog_stride_adaptive", "1", "space the fog kernel's shadow casts wider as the fog thickens (WARCHEST session 3, 2026-08-28): below 50%% transmittance the cast stride doubles, below 25%% it quadruples -- deep in a fog bank the light's per-step update rate is invisible but its shadow rays are not free. The march steps themselves are unchanged; only the cast schedule adapts. 0 = the fixed rt_metal_fog_stride schedule, byte for byte"};
cvar_t rt_metal_lightsample_clamp = {CF_CLIENT | CF_ARCHIVE, "rt_metal_lightsample_clamp", "0", "ceiling on the fog light pick's 1/p weight (the demo17 speckle fix, 2026-08-29): each fog march step samples ONE light and divides by its pick probability, so an unlucky pick of a dim light was an unbounded bright blotch -- the flickering speckle in thick fog. The clamp bounds that amplitude; the cost is energy loss from rarely-picked lights. MEASURED ON THE DEMO17 BED AND MOSTLY REJECTED as the fix there: 8 changed nothing (the blotch variance is the BULK of the pick distribution, not the tail this bounds) and 2 cut speckle events 66%% but ate 44%% of the fog's light -- the deepened parked stillness floor (0.9 under the pick, automatic) is what shipped instead. Kept as a bounded-amplitude lever for content with genuinely pathological picks. 0 = unclamped, the old estimator byte for byte. Console-only"};
cvar_t rt_metal_gi = {CF_CLIENT | CF_ARCHIVE, "rt_metal_gi", "1", "ONE-BOUNCE DIFFUSE GI in the RT wall-lighting term (GIARC, 2026-08-29): each pixel fires one cosine-weighted bounce ray from its surface hit, evaluates the map's real lights at the bounce point (one stochastic pick per frame, every light shadowed in expectation -- the fog lightsample machinery) and accumulates the result in a reprojected temporal history, so torch glow bleeds into shadowed corners and rooms read lit rather than painted. Needs rt_metal 1 and rt_metal_walllight > 0 (the over-lightmap arm's term is a shadow multiplier over baked light, which already carries what it carries -- bounce there would double-count). Default 1 since G3 (Seb's eye passed it in motion, 2026-08-29: 'gi_on is better than off'); menu row Bounce Lighting on the RT Shadows page"};
cvar_t rt_metal_gi_dist = {CF_CLIENT, "rt_metal_gi_dist", "512", "maximum bounce-ray length in world units for rt_metal_gi (64-8192). Longer rays gather light from further surfaces but the light evaluation at the bounce uses the PRIMARY pixel's tile-culled light list, so a light near a distant bounce point can be missed -- keeping this modest keeps that stated bias small. Console-only"};
cvar_t rt_metal_gi_albedo = {CF_CLIENT, "rt_metal_gi_albedo", "0.5", "constant reflectance of the bounce surface for rt_metal_gi (0-1). The RT geometry carries no surface colours, so this stands in for the average Quake wall -- in Quake's palette the LIGHT carries the hue, so a torch still bounces warm. The 1/pi of the diffuse BRDF is folded in here too; it is a calibration constant, not a material. Sampling real albedo is a later arc's question. Console-only"};
cvar_t rt_metal_gi_history = {CF_CLIENT, "rt_metal_gi_history", "0.9", "temporal accumulation weight of the GI colour history (0-0.98; 0 = off, one noisy sample per frame). GI is low-frequency light, so a deep history is affordable and is what makes one ray per pixel readable; reprojection is world-position-validated so disocclusions fall back to the current sample rather than ghosting. The stillness floor (0.75 while parked with no dynamic lights) applies as it does to the fog. Console-only"};
cvar_t rt_metal_gi_intensity = {CF_CLIENT, "rt_metal_gi_intensity", "1", "gain of the GI term added into the wall lighting (0-8). Applied inside the same walllight scale as the direct light and before the rt_metal_lmax shoulder, so bounce light rides the term's whole tone chain and cannot blow past the ceiling. Console-only"};
cvar_t rt_metal_gi_emissive = {CF_CLIENT, "rt_metal_gi_emissive", "0.65", "EMISSIVE BOUNCE (GIARC G3, 2026-08-29): a GI bounce ray that lands on a lava sheet or a torch/candle flame takes that surface's emission as its radiance, so lava throws orange onto ceilings and flames bounce their own glow. The value is the gain mapping emission colour onto the light scale (0-8); 0 keeps the G1 bounce (world+entities only) byte for byte. Honest overlap, stated: the merged lava lights and the real torch lights already deliver these emitters' DIRECT light, so this adds the area-source form those sparse points under-represent. Default 0.65 -- Seb's eye passed it in motion on e1m7 the day it landed ('emissive on is good') and 0.65 is his calibrated value; the Chthon-arena stills put gain 1 at 14.7% of the frame at mean 0.46, bounded by the lmax shoulder even at 4. Console-only"};
cvar_t rt_metal_gi_rate = {CF_CLIENT | CF_ARCHIVE, "rt_metal_gi_rate", "1", "GI SAMPLE RATE (GIARC G4-1, 2026-08-29): fire the bounce ray on a frame-rotating 1-in-N subset of pixels (16x16 tiles) and let the deep reprojected history fill the rest -- bounce light is low-frequency and can afford it. 1 = every pixel every frame (the G3 behaviour byte for byte), 2 = half the rays, 4 = a quarter. Default 1 -- Seb's eye judged rate 1 'a tiny bit better' and the look wins (2026-08-29); rate 2 recovers about half of the bounce's ~6 fps on Best/Better if you ever want the frames instead. The trade is temporal: bounce light lags a little more on turns, a freshly exposed silhouette can miss its bounce briefly -- recipes: exec gi_rate1/2/4.cfg. Archived so a tuned value survives the next launch"};
cvar_t rt_metal_gi_albedo_tex = {CF_CLIENT, "rt_metal_gi_albedo_tex", "1", "COLOURED BOUNCE (GIARC G4-2, 2026-08-29): blend the bounce surface's reflectance from the flat rt_metal_gi_albedo constant (0) to that surface's own mean texture colour (1), so a red brick corridor bleeds red light into its corners and a green slime hall bleeds green -- the room's own colour in its bounce, which is most of what reads as GI rather than a lift. The colour is each texture's load-time average (the same quantity the old bounce-grid used), so it follows replacement packs like QRP automatically; entity hits keep the constant (their gather carries no colour stream). Quake's walls average darker than the 0.5 constant, so 1 also dims the bounce a little -- rt_metal_gi_intensity buys it back if wanted. Default 1 since Seb's eye passed it in motion (2026-08-29, 'albedo on is ok'); 0 = the flat-constant bounce byte for byte. Console-only"};
cvar_t rt_metal_fog_liquidlight = {CF_CLIENT | CF_ARCHIVE, "rt_metal_fog_liquidlight", "1", "BEAUTY B2 (2026-09-16): LIGHT IN THE WATER. The fog kernel computes its in-scattered light and its god rays at every march step and then throws them away inside a liquid, at four sites, by design (the authored underwater look is the in-liquid colour and density alone). This is the fraction of those lit terms that survives under water: 1 = the full shafts and the lights' scatter through the murk while submerged, for the march that already runs; the in-liquid density and colour are untouched and the light ADDS to them. On by default since 2026-09-17, on Seb's eye; 0 = the old arithmetic exactly. Console-only; exec fog_liquidlight_on.cfg / _half.cfg / _off.cfg"};
cvar_t r_skylightning = {CF_CLIENT | CF_ARCHIVE, "r_skylightning", "1", "BEAUTY C1 (2026-09-17): LIGHTNING IN THE SKY, THUNDER THROUGH THE ROOM. On a map with open sky, at a random interval around r_skylightning_period, the sky light (the sun ray, rt_metal_sun's machinery) is driven for two or three frames with a cold blue-white light from a random high angle, so everything under open sky is lit hard with real ray-traced shadows for that instant, then a second, smaller flash a tenth of a second later; one to four seconds after (the distance), ambience/thunder1.wav rolls in from the flash's direction through the room reverb. This is the flash's strength (1 = a bright storm, the default since 2026-09-17 on Seb's eye; 0 = never). Needs rt_metal_walllight (the sun is a wall-lighting term) and a map with sky. Console-only; exec skylightning_on.cfg / _off.cfg"};
cvar_t r_skylightning_period = {CF_CLIENT | CF_ARCHIVE, "r_skylightning_period", "60", "r_skylightning: the MEAN seconds between flashes; each interval is random between a third and one and a half times this"};
cvar_t r_skylightning_hold = {CF_CLIENT | CF_ARCHIVE, "r_skylightning_hold", "0.05", "r_skylightning: how long the main flash holds, in seconds (0.05 = two or three frames; the second flash holds 0.035 s and a tenth of a second later)"};
cvar_t r_skylightning_fog = {CF_CLIENT | CF_ARCHIVE, "r_skylightning_fog", "0.5", "r_skylightning: how much of the flash reaches the FOG (1 = the air lights up as hard as the sky does, 0 = the old flash, surfaces only). The flash pushes one light up its own direction carrying the sun's colour as a fog weight -- the thunderbolt's own mechanism -- so the murk scatters it and the fog kernel's shadow ray keeps it out of sealed rooms. The GL murk march has no shadow rays, so on that tier it reaches indoor fog too. Calibrated on the e1m3 sky shaft at Seb's own config, where the fog over the shaft reads 19 of 255 unlit, 46 at 0.35, 58 at 0.5 and 84 at 1: the knee is the eye's, and a BRIGHT-gamma configuration with thick pale murk saturates well below 0.5, so this is the knob to turn before the flash itself"};
cvar_t rt_metal_shadowlights = {CF_CLIENT | CF_ARCHIVE, "rt_metal_shadowlights", "3", "THE ROUND SPOTLIGHTS (2026-09-19): how many of a pixel's BRIGHTEST lights get a shadow test (1-4). 1 = the dominant alone, which is every frame this renderer has drawn until now -- and the reason Seb reported \"some of the lights are not soft, and are cast from unseen sources, casting clean round lit areas in odd locations\": the other lights in range ride in the sum with NO occlusion at all, so a light in the next room, in a ceiling recess or behind a pillar paints its full disc straight through the geometry. 2-4 shadow-test that many, deterministically -- no stochastic pick, no noise, no history (which is what rt_metal_lightsample 2 does instead, and what his eye rejected in August for speckle). The runners-up spend rt_metal_shadowlights_rays each and skip contact hardening, being dimmer by construction. Wall-lighting arm only. Costs rays: see the 2026-09-19 record. exec spotfix_on.cfg / spotfix_off.cfg"};
cvar_t rt_metal_shadowlights_rays = {CF_CLIENT | CF_ARCHIVE, "rt_metal_shadowlights_rays", "2", "shadow rays spent on each light past the dominant under rt_metal_shadowlights (1-8). The dominant keeps rt_metal_samples and its contact hardening; these are cheaper because they are dimmer. Their penumbra uses the STATIC per-pixel dither and is never frame-rotated: the dominant's visibility is smoothed by the rt_metal_history EMA and these have none, so a rotating sample set would crawl"};
cvar_t rt_metal_contact = {CF_CLIENT | CF_ARCHIVE, "rt_metal_contact", "1", "BEAUTY B3 (2026-09-17): CONTACT-HARDENED SHADOWS. Today every shadow is the same softness (rt_metal_softness) whether the thing casting it stands on the surface or hangs far above it. With this on, one extra closest-hit ray per pixel toward the dominant light finds how far away the blocker is, and the penumbra narrows as the blocker approaches the receiver -- a foot sharp on the floor, a pillar's shadow crisp at its base and soft at its far end, PCSS's shape on the ray tracer's own disc. The fraction blends it in. On by default since 2026-09-17, on Seb's eye. NOT free: it changes the trace stage's per-pixel work (measured in CLAUDE.md's B3 record); 0 = the old bytes. Console-only; exec contact_on.cfg / contact_off.cfg"};
cvar_t rt_metal_gi_ao = {CF_CLIENT | CF_ARCHIVE, "rt_metal_gi_ao", "1", "BEAUTY B1 (2026-09-16): AMBIENT OCCLUSION FOR FREE from the bounce ray. The one-bounce GI ray (rt_metal_gi, on from Good up) is a closest-hit ray and already reports how far it went; a short one means the point is enclosed -- a corner, the foot of a wall, under a ledge. That distance, smoothed over rt_metal_gi_ao_dist and averaged through the GI's own history, darkens the FLAT AMBIENT FILL (rt_metal_ambient, 'light with no source, on purpose' -- this is its corrective) by this fraction; the direct light has its own shadows and is never touched. Costs two multiplies where the bounce already runs; nothing where it does not (Stock, Superfast, Fast). On by default since 2026-09-17, on Seb's eye; 0 = the old term byte for byte, 1 = the fill goes to nothing at contact. Console-only; exec gi_ao_on.cfg / gi_ao_off.cfg"};
cvar_t rt_metal_gi_ao_dist = {CF_CLIENT | CF_ARCHIVE, "rt_metal_gi_ao_dist", "64", "BEAUTY B1: the contact range of rt_metal_gi_ao in world units -- a bounce hit this far away occludes nothing, one at contact occludes fully, smoothly between. 32-128 is the sensible range"};
cvar_t rt_metal_gi_tiledilate = {CF_CLIENT | CF_ARCHIVE, "rt_metal_gi_tiledilate", "0", "BOUNCE LIGHT THAT WAS NOT ARRIVING (GIARC G4-4, 2026-08-31). The tile light cull bounds the AABB of the tile's PRIMARY HIT POINTS, but a bounce point lies up to rt_metal_gi_dist away from its hit, so a light that reaches the bounce and not the tile is culled and that sample contributes EXACTLY ZERO bounce light. This dilates the cull by that FRACTION of rt_metal_gi_dist and builds a SECOND, wider list that the bounce pick alone uses; the surface lighting keeps the tight list and is untouched by construction, and the whole arm is skipped unless rt_metal_gi and rt_metal_walllight are both live. IT WORKS, AND IT IS STILL THE WRONG TOOL -- MEASURED, AND THE RESULT IS NEGATIVE. Bias remaining over 385092 bounce samples on nine frames and four demos, by fraction: 0: 22.8%. 0.125: 10.1%. 0.25: 4.1%. 0.5: 0.3%. 1: 0.0%. But at the fraction that matches rt_metal_gi_fallback's quality (0.5) it is NEVER CHEAPER than it, on either map measured on a quiet machine at a 480x270 trace: on e1m3 the two TIE (+0.130 ms each, +26.5%, P(dilate cheaper) = 0.67, overlapping); on start the fallback WINS decisively (+0.150 ms against this feature's +0.260, P = 0.04). The reason is structural and is why no fraction rescues it: the dilated list is an ordered SUPERSET paid on EVERY bouncing pixel, while the fallback's full-list loop is paid only on the 20-40% that found nothing -- and the more compact the map, the more the dilation pulls in until its list approaches the full one anyway. The one point where it undercuts the fallback is 0.25 on e1m3 (+0.100 ms, P = 1.00) and that buys the saving with 4.1% residual bias rather than ~0. USE rt_metal_gi_fallback INSTEAD; this is retained as the measured A/B and as the tunable the fallback structurally cannot offer. 0 = the old bytes"};
cvar_t rt_metal_gi_fallback = {CF_CLIENT | CF_ARCHIVE, "rt_metal_gi_fallback", "0", "BOUNCE LIGHT THAT WAS NOT ARRIVING (GIARC G4-3, 2026-08-30). The bounce ray's light pick ranges over the PRIMARY pixel's tile-culled list, and measurement showed what that costs: on 20-40% of bounce hits NO light in that list reaches the bounce point, so the hit contributes EXACTLY ZERO bounce light -- which is why the bounce reads as a modest lift rather than filling a room. 1 re-runs the same pick over ALL staged lights, but only on those hits, so nothing that already works changes and the surface lighting is untouched. Unbiased by construction: the fallback fires exactly when the first pass found nothing, so its reservoir starts clean and its 1/p weight is the full list's. 0 = the old bytes"};
cvar_t rt_metal_refit = {CF_CLIENT | CF_ARCHIVE, "rt_metal_refit", "1", "REFIT the per-frame entity and light-core BLAS in place when their topology has not changed, instead of rebuilding from scratch (WARCHEST session 2, 2026-08-28). A refit updates the existing structure for moved vertices at a fraction of a build's GPU cost; the visible set changing (monsters culled in or out) or a periodic quality cadence forces a real rebuild. Invisible by design -- the traced picture is geometrically identical -- and measured on the KERNELMS trace stage. 0 = rebuild every frame, the old path byte for byte"};
cvar_t rt_metal_term_upsample = {CF_CLIENT | CF_ARCHIVE, "rt_metal_term_upsample", "1", "depth-aware upsample of the RT lighting term at the composite (WARCHEST session 1, 2026-08-28): the fog upsample's trick applied to the shadow/lighting buffer, so at every silhouette the multiply is rebuilt from the taps whose geometry matches the pixel's own scene depth instead of bleeding the background's lighting across the edge. Interiors are untouched (the hardware bilinear is taken where the four taps agree). This is what makes low Trace Resolution values usable: the term's edges stop stepping at the trace buffer's pitch. Needs the scene depth as a texture, which the offscreen scene path provides (any r_viewfbo, r_volumetric, r_edr or MetalFX-temporal configuration; the direct path falls back to plain bilinear). 0 = the plain magnification, byte for byte"};
cvar_t rt_metal_term_upsample_depth = {CF_CLIENT, "rt_metal_term_upsample_depth", "0.1", "how far apart (as a fraction of the pixel's own distance) two depths may be before the term upsample treats them as different surfaces. Lower = sharper shadow-edge splits; higher = closer to plain bilinear. Console-only tuning knob, the fog upsample's twin"};
cvar_t rt_metal_fog_filter = {CF_CLIENT | CF_ARCHIVE, "rt_metal_fog_filter", "0", "depth-aware spatial filter over the ray-traced fog buffer at the fog's own resolution, before it is magnified (S1 of the fog-buffer plan): 0 off, 1 a 3x3, 2 a 5x5, 4 the 5x5 twice with the second pass's taps two texels apart (a 13x13 footprint at two passes' cost, 2026-09-03: the fog's raw grain is three to four texels across and one 5x5 cannot average it -- that grain is the standing fizz). The fog's jitter puts its grain at the buffer's Nyquist frequency, which the 8/3 magnification then puts on screen as the standing mesh; a [1 2 1] filter has zero response at exactly that frequency and is the only stage that removes it at source. Depth affinity (rt_metal_fog_upsample_depth) keeps silhouettes; transmittance affinity keeps a fog bank's edge against clear air. ~0.05 ms. 0 is the old path byte for byte. Default off pending by-eye QA"};
cvar_t rt_metal_fog_clamp = {CF_CLIENT | CF_ARCHIVE, "rt_metal_fog_clamp", "0", "history clamp for the ray-traced fog buffer (BLUENOISE slice 2, 2026-09-03). Today the fog kernel blends each frame with last frame's FILTERED fog through a rotation-only reprojection and nothing checks what that read returned, which is why its temporal smoothing is capped at 0.9 and why the parked-camera floor stops there: deeper history without a check ghosts on every light flick, door and teleport. 1 clamps the reprojected history into the range of the current frame's 3x3 neighbourhood (min/max); 2 clips it to mean +/- rt_metal_fog_clamp_k sigma; 3 is the A/B with no clamp at all. Under any of them the history accumulates the RAW fog (the spatial filter then runs ONCE for display instead of compounding through the feedback every frame) and rt_metal_fog_history may go to 0.95. MEASURED IN MOTION 2026-09-03 (evening) AND NOT RECOMMENDED THERE: against the true fog on a strafe, today's 0.7 history errs 2.2%%, the clamp at 0.8 2.5%%, any 0.95 history 4-8%% -- a 2D history is content from where the fog WAS, and more of it trails more. Parked it is the smoothest arm there is. Console A/B for a parked camera; the despeckle package is rt_metal_lightsample_hybrid + rt_metal_fog_filter 4. 0 = the old path byte for byte"};
cvar_t rt_metal_fog_clamp_k = {CF_CLIENT, "rt_metal_fog_clamp_k", "1.5", "rt_metal_fog_clamp mode 2: how many neighbourhood sigmas of the current frame the reprojected fog history may sit from the neighbourhood mean before it is clipped. Smaller = less ghosting, less smoothing. Console-only"};
cvar_t rt_metal_fog_tonemapema = {CF_CLIENT | CF_ARCHIVE, "rt_metal_fog_tonemapema", "0", "blend the fog history in a compressed c/(1+luma) domain and invert on the way out (the Karis TAA weight; BLUENOISE slice 3, 2026-09-03), so one bright light pick weighs less than its linear worth in the average. Needs rt_metal_fog_clamp >= 1 (it is the history pass that blends). It biases the accumulated fog DOWN wherever the picks vary -- rt_metal_fog_intensity is the calibration if the fog dims. 0 = linear blending, the old bytes"};
cvar_t rt_metal_fog_reproject_depth = {CF_CLIENT | CF_ARCHIVE, "rt_metal_fog_reproject_depth", "0", "translation-aware fog history (2026-09-03, Seb's strafing smear): the fog kernel records where along each ray its in-scattered light came from (the extinction-weighted mean march distance -- about 100 units out at his density, against walls 300-1000 away) and the history pass puts that point through the FULL previous camera, eye included, reading last frame's fog bilinearly where it actually was. Where the previous frame's own scatter depth there disagrees by more than rt_metal_fog_reproject_tol the history is dropped (a doorway, a silhouette). Needs rt_metal_fog_clamp >= 1 (the history pass is what reads). 0 = the rotation-only remap, under which a 4-unit strafe step moves near fog 12 buffer texels a frame and any history at all trails it. MEASURED 2026-09-03 (evening): it makes the history WORSE (mean error against the true fog 4.4%% at 0.95 clamped vs 2.2%% for today's path) -- one displacement cannot register a volume whose structure sits deeper than its scatter centroid. Kept as the measured A/B; modes 2-5 are its debug probes"};
cvar_t rt_metal_liquids_own = {CF_CLIENT | CF_ARCHIVE, "rt_metal_liquids_own", "0", "SEPTEMBER2 C1 (2026-09-06): give alpha-blended water and slime their OWN light instead of the pool floor's. rt_metal_liquids multiplies the ray-traced lighting term into the water, but the primary ray passes THROUGH the surface, so that term is the lighting of whatever lies under it -- which prints the submerged geometry's shadows onto the water (e1m1's pillar bases through the slime). This blends (0-1) the sampled term toward the light sum AT THE SURFACE, evaluated on the CPU from the ray tracer's own light list at each batch's centre with rt_metal_liquids_own_shadows tracelines to its loudest lights -- one constant per batch, so a large lake reads flat: the cheap prototype the per-pixel second term (~0.3-0.6 ms) would replace if this look is worth it. Needs rt_metal_liquids > 0 and r_wateralpha_force 1. 0 = the sampled term byte for byte"};
cvar_t rt_metal_liquids_rt = {CF_CLIENT | CF_ARCHIVE, "rt_metal_liquids_rt", "0", "SEPTEMBER2 C2 (2026-09-09): alpha-blended water and slime take their OWN per-pixel light and a REFLECTION from the ray tracer. The blended liquids join the acceleration structure as a seventh instance that only this arm intersects (the primary ray still passes through to the pool floor, whose lighting the composite keeps); at every pixel where the liquid is in front of that floor the surface kernel lights the liquid point with the wall term's own chain (every in-range light, the dominant one shadow-tested by one ray) and reflects the view ray about the surface to shade one hit -- a torch flame, a lava sheet, a lit wall -- weighted by the water's Fresnel (faint looking down, strong across a lake). Both are written to a second buffer the liquid shader reads in place of the floor's term, so the pool floor stops printing through and the room shows in the water. Needs rt_metal_liquids > 0, rt_metal_walllight > 0 and water that actually renders blended (r_wateralpha_force 1 on stock maps). Deterministic per frame (no noise, no history). Supersedes rt_metal_liquids_own and the minlight floor while on. 0 = the old bytes"};
cvar_t rt_metal_liquids_reflect = {CF_CLIENT | CF_ARCHIVE, "rt_metal_liquids_reflect", "1", "SEPTEMBER2 C2: gain on the reflection rt_metal_liquids_rt adds (Schlick's Fresnel at F0 0.02, physical at 1; 0 = the own term only, no glint; 2 doubles the glint)"};
cvar_t rt_metal_liquids_own_shadows = {CF_CLIENT, "rt_metal_liquids_own_shadows", "2", "rt_metal_liquids_own: how many of a liquid batch's loudest lights are shadow-tested from its centre (0-16, one traceline each per frame per batch)"};
cvar_t rt_metal_sun = {CF_CLIENT | CF_ARCHIVE, "rt_metal_sun", "0", "THE SKY LIGHT (SEPTEMBER2 D, 2026-09-06): a directional sun for the ray tracer. Each surface pixel casts one ray toward the sun and is sunlit exactly where that ray leaves the level through open sky (the same open-sky structure the fog cap uses), so walls, floors and monsters take a real sun with real shadows -- wall-lighting mode only, since under the lightmap the map's own baked sun is already there and under wall lighting that lightmap is discarded, which is why outdoor Arcane Dimensions read flat until now. The sun comes from the map's ericw keys (_sunlight, _sunlight_color, _sunlight_mangle or _sun_mangle, _sunlight_penumbra: AD's start, sepulcher and e1m1 declare one; ad_tears and every id1 map do not) unless the rt_metal_sun_* cvars override them. This value is the multiplier on the map's strength (1 = as authored). One closest-hit ray per pixel. 0 = no sun, the old bytes"};
cvar_t rt_metal_sun_light = {CF_CLIENT | CF_ARCHIVE, "rt_metal_sun_light", "0", "rt_metal_sun: override the map's _sunlight strength (ericw units: 250 is a strong sun, 100 soft); 0 = the map's own, and no sun on a map without one"};
cvar_t rt_metal_sun_color = {CF_CLIENT | CF_ARCHIVE, "rt_metal_sun_color", "", "rt_metal_sun: override the map's _sunlight_color, three numbers QUOTED (0-1 or 0-255); empty = the map's own"};
cvar_t rt_metal_sun_mangle = {CF_CLIENT | CF_ARCHIVE, "rt_metal_sun_mangle", "", "rt_metal_sun: override the map's sun direction as \"yaw pitch\" QUOTED, ericw's convention (negative pitch shines down: \"300 -60\" is a sun high in the south-west); empty = the map's own, and no sun on a map without one"};
cvar_t rt_metal_sun_penumbra = {CF_CLIENT | CF_ARCHIVE, "rt_metal_sun_penumbra", "-1", "rt_metal_sun: the sun disc's angular radius in degrees (soft shadow edges); -1 = the map's _sunlight_penumbra (0 = hard)"};
cvar_t rt_metal_as_skipstatic = {CF_CLIENT | CF_ARCHIVE, "rt_metal_as_skipstatic", "1", "SEPTEMBER2 A3 (2026-09-06): reuse the ray tracer's entity and light-core acceleration structures when nothing they hold has moved. Every frame the two dynamic BLASes were refit and the TLAS rebuilt whatever happened (the 2026-08-30 AS-floor record: 0.17-0.36 ms, content-dependent); with this on each gather hashes the bytes it uploads and a slot whose structure was last built from exactly those bytes skips its refit, and the TLAS too when both do. Identical input, identical BVH: byte-exact. In play the saving is bounded by how often nothing animates -- every flame model lerps every frame, so a torch in view keeps the light-core structure moving; parked with no torches, in photo mode, or on a frozen bed the whole AS stage disappears. RT_METAL_KERNELMS's refit counter line reports the skipped share. 0 = refit every frame. Default 1 since 2026-09-08 (byte-exact, so his pass was the frame rate)"};
cvar_t rt_metal_pipeline = {CF_CLIENT | CF_ARCHIVE, "rt_metal_pipeline", "0", "same-frame RT pipelining (SEPTEMBER2 A2, 2026-09-06) -- BUILT AND MEASURED NIL, kept as the A/B. Under rt_metal_sameframe the composite hook commits the ray tracer's frame and waits for it while the raster encoded so far sits in the renderer's command buffer until the end of the frame. 1 commits the raster the moment the trace is committed so the two overlap on the GPU (they do: the sidecar's stages stretch 13-33%% beside it) -- and the frame is no shorter, +0.7-0.9%% on interleaved demo26 pairs at render scale 1, inside the pair noise: the GPU is saturated during the overlap, so concurrency reorders the work without shortening it. 2 also commits the RT composite and the murk composite as soon as they are encoded (the bubble after the wait) and read -1.5 to -5.7%% -- the extra buffers cost more than they fill. So the 12-15%% same-frame cost of 2026-08-08 is not commit ordering; the 8-1 record's reading stands. Picture byte-identical either way (parity bed). Metal only. 0 = today's frame order, the measured default"};
cvar_t rt_metal_fog_froxel = {CF_CLIENT | CF_ARCHIVE, "rt_metal_fog_froxel", "1", "the FROXEL fog volume (SEPTEMBER2 A1, 2026-09-06; DEFAULT 1 since 2026-09-07 on Seb's eye: \"froxel looks amazing, keep that\"): the ray-traced fog kernel marches FIXED view-aligned depth slices (exponential, dense near the eye, to r_volumetric_dist) and stores each cell's in-scattered light and density in a 3D volume instead of summing them per texel; the history is then blended PER CELL at the cell's own world position through the full previous camera, eye included, which is what a 2D fog history structurally cannot do (measured 2026-09-03: any 2D history under camera translation increases the error against the true fog). An integrate pass sums the accumulated cells into the same fog buffer as before, so the composite, the display filter and the upsample are untouched. What it buys is a deep history (rt_metal_fog_froxel_history, 0.9) with no smear on a strafe, so the light pick's variance averages over ~10 frames rather than 3 -- the route to fewer casts per ray at the same converged look. Cells beyond the wall are not marched (cheaper than today's 24 steps to every hit); cost is the volume's history reads and one small integrate pass. Supersedes rt_metal_fog_clamp / _reproject_depth while on. Two RGBA16F volume pairs: ~100 MB at a 480x270x24 buffer. 0 = the 2D-history fog kernel byte for byte (the pre-2026-09-07 default, exec froxel_off.cfg)"};
cvar_t rt_metal_fog_froxel_slices = {CF_CLIENT | CF_ARCHIVE, "rt_metal_fog_froxel_slices", "48", "rt_metal_fog_froxel: depth slices in the volume, which is also the march's step count under it (rt_metal_fog_steps is not read). MEASURED 2026-09-06 on demo23's strafe against the mean truth: 24 slices converge +1.4%% BRIGHTER than the true fog (cells coarser than the density's variation bias the extinction estimate), 48 land on it (-0.01%%) and beat every history arm ever measured on that bed; 16 is +3.7%%. Casts still fall every rt_metal_fog_stride cells. **AN M5 QUALITY TIER LEVER SINCE 2026-09-19**, and CF_ARCHIVE with it -- an unarchived lever resets on the next launch and the tier reads Custom every boot. 16/16/24/24/24/32/32 up the table; inert below Better, where rt_metal_fog_froxel is 0. 48 -> 32 measures x1.020, so the count is a convergence knob rather than a frame one"};
cvar_t rt_metal_fog_froxel_history = {CF_CLIENT, "rt_metal_fog_froxel_history", "0.9", "rt_metal_fog_froxel: the per-cell temporal blend (0 = no history, 0.9 = ~10 frames, 0.95 = ~20). Each cell reprojects its own world point, so this can sit far deeper than rt_metal_fog_history without smearing; a light flick or a door lags by about the same number of frames. Console-only"};
cvar_t rt_metal_fog_froxel_near = {CF_CLIENT, "rt_metal_fog_froxel_near", "6", "rt_metal_fog_froxel: how deep the FIRST cell is, in world units; the exponential slice spacing is derived from this, the slice count and the live r_volumetric_dist every frame, so the near fog keeps its resolution whatever the distance setting. Cells coarser than ~10 units bias the converged fog bright (measured 2026-09-06). Console-only"};
cvar_t rt_metal_fog_froxel_castphase = {CF_CLIENT, "rt_metal_fog_froxel_castphase", "0", "rt_metal_fog_froxel: 1 advances the shadow-cast schedule by one cell per frame (the first cell is always cast) so every cell is freshly cast within rt_metal_fog_stride frames. Built to average out the +2.5%% bright bias a stride of 6 shows under the froxel -- and MEASURED NOT TO (2026-09-06: +3.1%% with it, and noisier parked): the bias is a forward hold, every cell taking light from a cast point nearer the eye than itself, which no schedule phase touches; the fix is interpolating between casts, not built. Kept as the A/B. 0 = the fixed schedule, the measured default"};
cvar_t rt_metal_fog_froxel_curve = {CF_CLIENT, "rt_metal_fog_froxel_curve", "0", "rt_metal_fog_froxel: 0 = derive the exponential spacing from rt_metal_fog_froxel_near (the default); > 0 pins the exponent a directly, slice depth = dist * (e^(a u) - 1) / (e^a - 1) (2.5 with 48 slices over 1200 units is near 5.7 / far 68). Console-only"};
cvar_t rt_metal_fog_reproject_tol = {CF_CLIENT, "rt_metal_fog_reproject_tol", "0.3", "rt_metal_fog_reproject_depth: how far (as a fraction) the previous frame's scatter depth may disagree with this texel's reprojected distance before its history is rejected. Console-only"};
cvar_t rt_metal_lightsample_hybrid = {CF_CLIENT | CF_ARCHIVE, "rt_metal_lightsample_hybrid", "0", "the fog light pick's dark-spot fix (2026-09-03): with the single pick every fog cast is BINARY -- the one light it chose is either lit or blocked -- so a texel whose casts picked shadowed lights goes black for the frame and the spatial filter spreads it into a soft dark blob. 1 always shadow-tests the dominant light on its own ray and re-draws the pick over the REST, so a cast can lose at most the rest's share (two shadow rays a cast: the fog stage's cast cost roughly doubles); 2 alternates casts between the dominant and the pick at one ray a cast, holding each half's last visibility; 3 (2026-09-06) is the SINGLE-PASS form -- no second light loop: the one pick decides which half the cast serves, so the dominant is measured in proportion to its weight rather than every other cast (cheaper than 2 by the second loop; its held visibility lags where many lights are equal). Needs rt_metal_lightsample >= 1. 0 = the single pick, the old bytes"};
static void RT_Metal_Snapshot_f(cmd_state_t *cmd) { (void)cmd; RT_Metal_RequestDump(); }   // dump next RT frame (verification aid)
// Benchmark aid: the engine's own timedemo renders exactly one frame per demo
// packet, so every frame pays the full demo-parse + entity-update cost. Real
// play (and playdemo) renders many frames per packet, so timedemo systematically
// UNDERSTATES real-world fps. This logs presented-frame throughput to stderr so
// playdemo / live gameplay can be measured too — with RT on or off, unlike the
// rt_metal profiling report.
cvar_t m5_fpslog = {CF_CLIENT, "m5_fpslog", "0", "log presented frames-per-second to stderr every N frames (0 = off). Works during playdemo and live play, where timedemo's one-frame-per-packet accounting does not apply"};
static qbool vid_usingnoaccel;
static double originalMouseSpeed = -1.0;
static io_connect_t IN_GetIOHandle(void)
{
	io_connect_t iohandle = MACH_PORT_NULL;
	kern_return_t status;
	io_service_t iohidsystem = MACH_PORT_NULL;
	mach_port_t masterport;

	status = IOMainPort(MACH_PORT_NULL, &masterport);
	if(status != KERN_SUCCESS)
		return 0;

	iohidsystem = IORegistryEntryFromPath(masterport, kIOServicePlane ":/IOResources/IOHIDSystem");
	if(!iohidsystem)
		return 0;

	status = IOServiceOpen(iohidsystem, mach_task_self(), kIOHIDParamConnectType, &iohandle);
	IOObjectRelease(iohidsystem);

	return iohandle;
}
#endif
#endif


// Tell startup code that we have a client
int cl_available = true;

qbool vid_supportrefreshrate = false;

static qbool vid_usingmouse = false;
static qbool vid_usingmouse_relativeworks = false; // SDL2 workaround for unimplemented RelativeMouse mode
static qbool vid_usinghidecursor = false;
static qbool vid_hasfocus = false;
static qbool vid_wmborder_waiting, vid_wmborderless;
static SDL_Joystick *vid_sdljoystick = NULL;
static SDL_GameController *vid_sdlgamecontroller = NULL;
static cvar_t joy_sdl2_trigger_deadzone = {CF_ARCHIVE | CF_CLIENT, "joy_sdl2_trigger_deadzone", "0.5", "deadzone for triggers to be registered as key presses"};
// GAME_STEELSTORM specific
static cvar_t *steelstorm_showing_map = NULL; // detect but do not create the cvar
static cvar_t *steelstorm_showing_mousecursor = NULL; // detect but do not create the cvar

static SDL_GLContext context;
static SDL_Window *window;

// Input handling

#ifndef SDLK_PERCENT
#define SDLK_PERCENT '%'
#endif

static int MapKey( unsigned int sdlkey )
{
	switch(sdlkey)
	{
	// sdlkey can be Unicode codepoint for non-ascii keys, which are valid
	default:                      return sdlkey & SDLK_SCANCODE_MASK ? 0 : sdlkey;
//	case SDLK_UNKNOWN:            return K_UNKNOWN;
	case SDLK_RETURN:             return K_ENTER;
	case SDLK_ESCAPE:             return K_ESCAPE;
	case SDLK_BACKSPACE:          return K_BACKSPACE;
	case SDLK_TAB:                return K_TAB;
	case SDLK_SPACE:              return K_SPACE;
	case SDLK_EXCLAIM:            return '!';
	case SDLK_QUOTEDBL:           return '"';
	case SDLK_HASH:               return '#';
	case SDLK_PERCENT:            return '%';
	case SDLK_DOLLAR:             return '$';
	case SDLK_AMPERSAND:          return '&';
	case SDLK_QUOTE:              return '\'';
	case SDLK_LEFTPAREN:          return '(';
	case SDLK_RIGHTPAREN:         return ')';
	case SDLK_ASTERISK:           return '*';
	case SDLK_PLUS:               return '+';
	case SDLK_COMMA:              return ',';
	case SDLK_MINUS:              return '-';
	case SDLK_PERIOD:             return '.';
	case SDLK_SLASH:              return '/';
	case SDLK_0:                  return '0';
	case SDLK_1:                  return '1';
	case SDLK_2:                  return '2';
	case SDLK_3:                  return '3';
	case SDLK_4:                  return '4';
	case SDLK_5:                  return '5';
	case SDLK_6:                  return '6';
	case SDLK_7:                  return '7';
	case SDLK_8:                  return '8';
	case SDLK_9:                  return '9';
	case SDLK_COLON:              return ':';
	case SDLK_SEMICOLON:          return ';';
	case SDLK_LESS:               return '<';
	case SDLK_EQUALS:             return '=';
	case SDLK_GREATER:            return '>';
	case SDLK_QUESTION:           return '?';
	case SDLK_AT:                 return '@';
	case SDLK_LEFTBRACKET:        return '[';
	case SDLK_BACKSLASH:          return '\\';
	case SDLK_RIGHTBRACKET:       return ']';
	case SDLK_CARET:              return '^';
	case SDLK_UNDERSCORE:         return '_';
	case SDLK_BACKQUOTE:          return '`';
	case SDLK_a:                  return 'a';
	case SDLK_b:                  return 'b';
	case SDLK_c:                  return 'c';
	case SDLK_d:                  return 'd';
	case SDLK_e:                  return 'e';
	case SDLK_f:                  return 'f';
	case SDLK_g:                  return 'g';
	case SDLK_h:                  return 'h';
	case SDLK_i:                  return 'i';
	case SDLK_j:                  return 'j';
	case SDLK_k:                  return 'k';
	case SDLK_l:                  return 'l';
	case SDLK_m:                  return 'm';
	case SDLK_n:                  return 'n';
	case SDLK_o:                  return 'o';
	case SDLK_p:                  return 'p';
	case SDLK_q:                  return 'q';
	case SDLK_r:                  return 'r';
	case SDLK_s:                  return 's';
	case SDLK_t:                  return 't';
	case SDLK_u:                  return 'u';
	case SDLK_v:                  return 'v';
	case SDLK_w:                  return 'w';
	case SDLK_x:                  return 'x';
	case SDLK_y:                  return 'y';
	case SDLK_z:                  return 'z';
	case SDLK_CAPSLOCK:           return K_CAPSLOCK;
	case SDLK_F1:                 return K_F1;
	case SDLK_F2:                 return K_F2;
	case SDLK_F3:                 return K_F3;
	case SDLK_F4:                 return K_F4;
	case SDLK_F5:                 return K_F5;
	case SDLK_F6:                 return K_F6;
	case SDLK_F7:                 return K_F7;
	case SDLK_F8:                 return K_F8;
	case SDLK_F9:                 return K_F9;
	case SDLK_F10:                return K_F10;
	case SDLK_F11:                return K_F11;
	case SDLK_F12:                return K_F12;
	case SDLK_PRINTSCREEN:        return K_PRINTSCREEN;
	case SDLK_SCROLLLOCK:         return K_SCROLLOCK;
	case SDLK_PAUSE:              return K_PAUSE;
	case SDLK_INSERT:             return K_INS;
	case SDLK_HOME:               return K_HOME;
	case SDLK_PAGEUP:             return K_PGUP;
#ifdef __IPHONEOS__
	case SDLK_DELETE:             return K_BACKSPACE;
#else
	case SDLK_DELETE:             return K_DEL;
#endif
	case SDLK_END:                return K_END;
	case SDLK_PAGEDOWN:           return K_PGDN;
	case SDLK_RIGHT:              return K_RIGHTARROW;
	case SDLK_LEFT:               return K_LEFTARROW;
	case SDLK_DOWN:               return K_DOWNARROW;
	case SDLK_UP:                 return K_UPARROW;
	case SDLK_NUMLOCKCLEAR:       return K_NUMLOCK;
	case SDLK_KP_DIVIDE:          return K_KP_DIVIDE;
	case SDLK_KP_MULTIPLY:        return K_KP_MULTIPLY;
	case SDLK_KP_MINUS:           return K_KP_MINUS;
	case SDLK_KP_PLUS:            return K_KP_PLUS;
	case SDLK_KP_ENTER:           return K_KP_ENTER;
	case SDLK_KP_1:               return ((SDL_GetModState() & KMOD_NUM) ? K_KP_1 : K_END);
	case SDLK_KP_2:               return ((SDL_GetModState() & KMOD_NUM) ? K_KP_2 : K_DOWNARROW);
	case SDLK_KP_3:               return ((SDL_GetModState() & KMOD_NUM) ? K_KP_3 : K_PGDN);
	case SDLK_KP_4:               return ((SDL_GetModState() & KMOD_NUM) ? K_KP_4 : K_LEFTARROW);
	case SDLK_KP_5:               return K_KP_5;
	case SDLK_KP_6:               return ((SDL_GetModState() & KMOD_NUM) ? K_KP_6 : K_RIGHTARROW);
	case SDLK_KP_7:               return ((SDL_GetModState() & KMOD_NUM) ? K_KP_7 : K_HOME);
	case SDLK_KP_8:               return ((SDL_GetModState() & KMOD_NUM) ? K_KP_8 : K_UPARROW);
	case SDLK_KP_9:               return ((SDL_GetModState() & KMOD_NUM) ? K_KP_9 : K_PGUP);
	case SDLK_KP_0:               return ((SDL_GetModState() & KMOD_NUM) ? K_KP_0 : K_INS);
	case SDLK_KP_PERIOD:          return ((SDL_GetModState() & KMOD_NUM) ? K_KP_PERIOD : K_DEL);
//	case SDLK_APPLICATION:        return K_APPLICATION;
//	case SDLK_POWER:              return K_POWER;
	case SDLK_KP_EQUALS:          return K_KP_EQUALS;
//	case SDLK_F13:                return K_F13;
//	case SDLK_F14:                return K_F14;
//	case SDLK_F15:                return K_F15;
//	case SDLK_F16:                return K_F16;
//	case SDLK_F17:                return K_F17;
//	case SDLK_F18:                return K_F18;
//	case SDLK_F19:                return K_F19;
//	case SDLK_F20:                return K_F20;
//	case SDLK_F21:                return K_F21;
//	case SDLK_F22:                return K_F22;
//	case SDLK_F23:                return K_F23;
//	case SDLK_F24:                return K_F24;
//	case SDLK_EXECUTE:            return K_EXECUTE;
//	case SDLK_HELP:               return K_HELP;
//	case SDLK_MENU:               return K_MENU;
//	case SDLK_SELECT:             return K_SELECT;
//	case SDLK_STOP:               return K_STOP;
//	case SDLK_AGAIN:              return K_AGAIN;
//	case SDLK_UNDO:               return K_UNDO;
//	case SDLK_CUT:                return K_CUT;
//	case SDLK_COPY:               return K_COPY;
//	case SDLK_PASTE:              return K_PASTE;
//	case SDLK_FIND:               return K_FIND;
//	case SDLK_MUTE:               return K_MUTE;
//	case SDLK_VOLUMEUP:           return K_VOLUMEUP;
//	case SDLK_VOLUMEDOWN:         return K_VOLUMEDOWN;
//	case SDLK_KP_COMMA:           return K_KP_COMMA;
//	case SDLK_KP_EQUALSAS400:     return K_KP_EQUALSAS400;
//	case SDLK_ALTERASE:           return K_ALTERASE;
//	case SDLK_SYSREQ:             return K_SYSREQ;
//	case SDLK_CANCEL:             return K_CANCEL;
//	case SDLK_CLEAR:              return K_CLEAR;
//	case SDLK_PRIOR:              return K_PRIOR;
//	case SDLK_RETURN2:            return K_RETURN2;
//	case SDLK_SEPARATOR:          return K_SEPARATOR;
//	case SDLK_OUT:                return K_OUT;
//	case SDLK_OPER:               return K_OPER;
//	case SDLK_CLEARAGAIN:         return K_CLEARAGAIN;
//	case SDLK_CRSEL:              return K_CRSEL;
//	case SDLK_EXSEL:              return K_EXSEL;
//	case SDLK_KP_00:              return K_KP_00;
//	case SDLK_KP_000:             return K_KP_000;
//	case SDLK_THOUSANDSSEPARATOR: return K_THOUSANDSSEPARATOR;
//	case SDLK_DECIMALSEPARATOR:   return K_DECIMALSEPARATOR;
//	case SDLK_CURRENCYUNIT:       return K_CURRENCYUNIT;
//	case SDLK_CURRENCYSUBUNIT:    return K_CURRENCYSUBUNIT;
//	case SDLK_KP_LEFTPAREN:       return K_KP_LEFTPAREN;
//	case SDLK_KP_RIGHTPAREN:      return K_KP_RIGHTPAREN;
//	case SDLK_KP_LEFTBRACE:       return K_KP_LEFTBRACE;
//	case SDLK_KP_RIGHTBRACE:      return K_KP_RIGHTBRACE;
//	case SDLK_KP_TAB:             return K_KP_TAB;
//	case SDLK_KP_BACKSPACE:       return K_KP_BACKSPACE;
//	case SDLK_KP_A:               return K_KP_A;
//	case SDLK_KP_B:               return K_KP_B;
//	case SDLK_KP_C:               return K_KP_C;
//	case SDLK_KP_D:               return K_KP_D;
//	case SDLK_KP_E:               return K_KP_E;
//	case SDLK_KP_F:               return K_KP_F;
//	case SDLK_KP_XOR:             return K_KP_XOR;
//	case SDLK_KP_POWER:           return K_KP_POWER;
//	case SDLK_KP_PERCENT:         return K_KP_PERCENT;
//	case SDLK_KP_LESS:            return K_KP_LESS;
//	case SDLK_KP_GREATER:         return K_KP_GREATER;
//	case SDLK_KP_AMPERSAND:       return K_KP_AMPERSAND;
//	case SDLK_KP_DBLAMPERSAND:    return K_KP_DBLAMPERSAND;
//	case SDLK_KP_VERTICALBAR:     return K_KP_VERTICALBAR;
//	case SDLK_KP_DBLVERTICALBAR:  return K_KP_DBLVERTICALBAR;
//	case SDLK_KP_COLON:           return K_KP_COLON;
//	case SDLK_KP_HASH:            return K_KP_HASH;
//	case SDLK_KP_SPACE:           return K_KP_SPACE;
//	case SDLK_KP_AT:              return K_KP_AT;
//	case SDLK_KP_EXCLAM:          return K_KP_EXCLAM;
//	case SDLK_KP_MEMSTORE:        return K_KP_MEMSTORE;
//	case SDLK_KP_MEMRECALL:       return K_KP_MEMRECALL;
//	case SDLK_KP_MEMCLEAR:        return K_KP_MEMCLEAR;
//	case SDLK_KP_MEMADD:          return K_KP_MEMADD;
//	case SDLK_KP_MEMSUBTRACT:     return K_KP_MEMSUBTRACT;
//	case SDLK_KP_MEMMULTIPLY:     return K_KP_MEMMULTIPLY;
//	case SDLK_KP_MEMDIVIDE:       return K_KP_MEMDIVIDE;
//	case SDLK_KP_PLUSMINUS:       return K_KP_PLUSMINUS;
//	case SDLK_KP_CLEAR:           return K_KP_CLEAR;
//	case SDLK_KP_CLEARENTRY:      return K_KP_CLEARENTRY;
//	case SDLK_KP_BINARY:          return K_KP_BINARY;
//	case SDLK_KP_OCTAL:           return K_KP_OCTAL;
//	case SDLK_KP_DECIMAL:         return K_KP_DECIMAL;
//	case SDLK_KP_HEXADECIMAL:     return K_KP_HEXADECIMAL;
	case SDLK_LCTRL:              return K_CTRL;
	case SDLK_LSHIFT:             return K_SHIFT;
	case SDLK_LALT:               return K_ALT;
//	case SDLK_LGUI:               return K_LGUI;
	case SDLK_RCTRL:              return K_CTRL;
	case SDLK_RSHIFT:             return K_SHIFT;
	case SDLK_RALT:               return K_ALT;
//	case SDLK_RGUI:               return K_RGUI;
//	case SDLK_MODE:               return K_MODE;
//	case SDLK_AUDIONEXT:          return K_AUDIONEXT;
//	case SDLK_AUDIOPREV:          return K_AUDIOPREV;
//	case SDLK_AUDIOSTOP:          return K_AUDIOSTOP;
//	case SDLK_AUDIOPLAY:          return K_AUDIOPLAY;
//	case SDLK_AUDIOMUTE:          return K_AUDIOMUTE;
//	case SDLK_MEDIASELECT:        return K_MEDIASELECT;
//	case SDLK_WWW:                return K_WWW;
//	case SDLK_MAIL:               return K_MAIL;
//	case SDLK_CALCULATOR:         return K_CALCULATOR;
//	case SDLK_COMPUTER:           return K_COMPUTER;
//	case SDLK_AC_SEARCH:          return K_AC_SEARCH; // Android button
//	case SDLK_AC_HOME:            return K_AC_HOME; // Android button
	case SDLK_AC_BACK:            return K_ESCAPE; // Android button
//	case SDLK_AC_FORWARD:         return K_AC_FORWARD; // Android button
//	case SDLK_AC_STOP:            return K_AC_STOP; // Android button
//	case SDLK_AC_REFRESH:         return K_AC_REFRESH; // Android button
//	case SDLK_AC_BOOKMARKS:       return K_AC_BOOKMARKS; // Android button
//	case SDLK_BRIGHTNESSDOWN:     return K_BRIGHTNESSDOWN;
//	case SDLK_BRIGHTNESSUP:       return K_BRIGHTNESSUP;
//	case SDLK_DISPLAYSWITCH:      return K_DISPLAYSWITCH;
//	case SDLK_KBDILLUMTOGGLE:     return K_KBDILLUMTOGGLE;
//	case SDLK_KBDILLUMDOWN:       return K_KBDILLUMDOWN;
//	case SDLK_KBDILLUMUP:         return K_KBDILLUMUP;
//	case SDLK_EJECT:              return K_EJECT;
//	case SDLK_SLEEP:              return K_SLEEP;
	}
}

qbool VID_HasScreenKeyboardSupport(void)
{
	return SDL_HasScreenKeyboardSupport() != SDL_FALSE;
}

void VID_ShowKeyboard(qbool show)
{
	if (!SDL_HasScreenKeyboardSupport())
		return;

	if (show)
	{
		if (!SDL_IsTextInputActive())
			SDL_StartTextInput();
	}
	else
	{
		if (SDL_IsTextInputActive())
			SDL_StopTextInput();
	}
}

qbool VID_ShowingKeyboard(void)
{
	return SDL_IsTextInputActive() != 0;
}

static void VID_SetMouse(qbool relative, qbool hidecursor)
{
#ifndef DP_MOBILETOUCH
#ifdef MACOSX
	if(relative)
		if(vid_usingmouse && (vid_usingnoaccel != !!apple_mouse_noaccel.integer))
			VID_SetMouse(false, false); // ungrab first!
#endif
	if (vid_usingmouse != relative)
	{
		vid_usingmouse = relative;
		cl_ignoremousemoves = 2;
		vid_usingmouse_relativeworks = SDL_SetRelativeMouseMode(relative ? SDL_TRUE : SDL_FALSE) == 0;
//		Con_Printf("VID_SetMouse(%i, %i) relativeworks = %i\n", (int)relative, (int)hidecursor, (int)vid_usingmouse_relativeworks);
#ifdef MACOSX
		if(relative)
		{
			// Save the status of mouse acceleration
			originalMouseSpeed = -1.0; // in case of error
			if(apple_mouse_noaccel.integer)
			{
				io_connect_t mouseDev = IN_GetIOHandle();
				if(mouseDev != 0)
				{
					if(IOHIDGetAccelerationWithKey(mouseDev, CFSTR(kIOHIDMouseAccelerationType), &originalMouseSpeed) == kIOReturnSuccess)
					{
						Con_DPrintf("previous mouse acceleration: %f\n", originalMouseSpeed);
						if(IOHIDSetAccelerationWithKey(mouseDev, CFSTR(kIOHIDMouseAccelerationType), -1.0) != kIOReturnSuccess)
						{
							Con_Print("Could not disable mouse acceleration (failed at IOHIDSetAccelerationWithKey).\n");
							Cvar_SetValueQuick(&apple_mouse_noaccel, 0);
						}
					}
					else
					{
						Con_Print("Could not disable mouse acceleration (failed at IOHIDGetAccelerationWithKey).\n");
						Cvar_SetValueQuick(&apple_mouse_noaccel, 0);
					}
					IOServiceClose(mouseDev);
				}
				else
				{
					Con_Print("Could not disable mouse acceleration (failed at IO_GetIOHandle).\n");
					Cvar_SetValueQuick(&apple_mouse_noaccel, 0);
				}
			}

			vid_usingnoaccel = !!apple_mouse_noaccel.integer;
		}
		else
		{
			if(originalMouseSpeed != -1.0)
			{
				io_connect_t mouseDev = IN_GetIOHandle();
				if(mouseDev != 0)
				{
					Con_DPrintf("restoring mouse acceleration to: %f\n", originalMouseSpeed);
					if(IOHIDSetAccelerationWithKey(mouseDev, CFSTR(kIOHIDMouseAccelerationType), originalMouseSpeed) != kIOReturnSuccess)
						Con_Print("Could not re-enable mouse acceleration (failed at IOHIDSetAccelerationWithKey).\n");
					IOServiceClose(mouseDev);
				}
				else
					Con_Print("Could not re-enable mouse acceleration (failed at IO_GetIOHandle).\n");
			}
		}
#endif
	}
	if (vid_usinghidecursor != hidecursor)
	{
		vid_usinghidecursor = hidecursor;
		SDL_ShowCursor( hidecursor ? SDL_DISABLE : SDL_ENABLE);
	}
#endif
}

// multitouch[10][] represents the mouse pointer
// multitouch[][0]: finger active
// multitouch[][1]: Y
// multitouch[][2]: Y
// X and Y coordinates are 0-1.
#define MAXFINGERS 11
float multitouch[MAXFINGERS][3];

// this one stores how many areas this finger has touched
int multitouchs[MAXFINGERS];

// modified heavily by ELUAN
static qbool VID_TouchscreenArea(int corner, float px, float py, float pwidth, float pheight, const char *icon, float textheight, const char *text, float *resultmove, qbool *resultbutton, keynum_t key, const char *typedtext, float deadzone, float oversizepixels_x, float oversizepixels_y, qbool iamexclusive)
{
	int finger;
	float fx, fy, fwidth, fheight;
	float overfx, overfy, overfwidth, overfheight;
	float rel[3];
	float sqsum;
	qbool button = false;
	VectorClear(rel);
	if (pwidth > 0 && pheight > 0)
	{
		if (corner & 1) px += vid_conwidth.value;
		if (corner & 2) py += vid_conheight.value;
		if (corner & 4) px += vid_conwidth.value * 0.5f;
		if (corner & 8) py += vid_conheight.value * 0.5f;
		if (corner & 16) {px *= vid_conwidth.value * (1.0f / 640.0f);py *= vid_conheight.value * (1.0f / 480.0f);pwidth *= vid_conwidth.value * (1.0f / 640.0f);pheight *= vid_conheight.value * (1.0f / 480.0f);}
		fx = px / vid_conwidth.value;
		fy = py / vid_conheight.value;
		fwidth = pwidth / vid_conwidth.value;
		fheight = pheight / vid_conheight.value;

		// try to prevent oversizepixels_* from interfering with the iamexclusive cvar by not letting we start controlling from too far of the actual touch area (areas without resultbuttons should NEVER have the oversizepixels_* parameters set to anything other than 0)
		if (resultbutton)
			if (!(*resultbutton))
			{
				oversizepixels_x *= 0.2;
				oversizepixels_y *= 0.2;
			}

		oversizepixels_x /= vid_conwidth.value;
		oversizepixels_y /= vid_conheight.value;

		overfx = fx - oversizepixels_x;
		overfy = fy - oversizepixels_y;
		overfwidth = fwidth + 2*oversizepixels_x;
		overfheight = fheight + 2*oversizepixels_y;

		for (finger = 0;finger < MAXFINGERS;finger++)
		{
			if (multitouchs[finger] && iamexclusive) // for this to work correctly, you must call touch areas in order of highest to lowest priority
				continue;

			if (multitouch[finger][0] && multitouch[finger][1] >= overfx && multitouch[finger][2] >= overfy && multitouch[finger][1] < overfx + overfwidth && multitouch[finger][2] < overfy + overfheight)
			{
				multitouchs[finger]++;

				rel[0] = bound(-1, (multitouch[finger][1] - (fx + 0.5f * fwidth)) * (2.0f / fwidth), 1);
				rel[1] = bound(-1, (multitouch[finger][2] - (fy + 0.5f * fheight)) * (2.0f / fheight), 1);
				rel[2] = 0;

				sqsum = rel[0]*rel[0] + rel[1]*rel[1];
				// 2d deadzone
				if (sqsum < deadzone*deadzone)
				{
					rel[0] = 0;
					rel[1] = 0;
				}
				else if (sqsum > 1)
				{
					// ignore the third component
					Vector2Normalize2(rel, rel);
				}
				button = true;
				break;
			}
		}
		if (scr_numtouchscreenareas < 128)
		{
			scr_touchscreenareas[scr_numtouchscreenareas].pic = icon;
			scr_touchscreenareas[scr_numtouchscreenareas].text = text;
			scr_touchscreenareas[scr_numtouchscreenareas].textheight = textheight;
			scr_touchscreenareas[scr_numtouchscreenareas].rect[0] = px;
			scr_touchscreenareas[scr_numtouchscreenareas].rect[1] = py;
			scr_touchscreenareas[scr_numtouchscreenareas].rect[2] = pwidth;
			scr_touchscreenareas[scr_numtouchscreenareas].rect[3] = pheight;
			scr_touchscreenareas[scr_numtouchscreenareas].active = button;
			// the pics may have alpha too.
			scr_touchscreenareas[scr_numtouchscreenareas].activealpha = 1.f;
			scr_touchscreenareas[scr_numtouchscreenareas].inactivealpha = 0.95f;
			scr_numtouchscreenareas++;
		}
	}
	if (resultmove)
	{
		if (button)
			VectorCopy(rel, resultmove);
		else
			VectorClear(resultmove);
	}
	if (resultbutton)
	{
		if (*resultbutton != button)
		{
			if ((int)key > 0)
				Key_Event(key, 0, button);
			if (typedtext && typedtext[0] && !*resultbutton)
			{
				// FIXME: implement UTF8 support - nothing actually specifies a UTF8 string here yet, but should support it...
				int i;
				for (i = 0;typedtext[i];i++)
				{
					Key_Event(K_TEXT, typedtext[i], true);
					Key_Event(K_TEXT, typedtext[i], false);
				}
			}
		}
		*resultbutton = button;
	}
	return button;
}

// ELUAN:
// not reentrant, but we only need one mouse cursor anyway...
static void VID_TouchscreenCursor(float px, float py, float pwidth, float pheight, qbool *resultbutton, keynum_t key)
{
	int finger;
	float fx, fy, fwidth, fheight;
	qbool button = false;
	static int cursorfinger = -1;
	static int cursorfreemovement = false;
	static int canclick = false;
	static int clickxy[2];
	static int relclickxy[2];
	static double clickrealtime = 0;

	if (steelstorm_showing_mousecursor && steelstorm_showing_mousecursor->integer)
	if (pwidth > 0 && pheight > 0)
	{
		fx = px / vid_conwidth.value;
		fy = py / vid_conheight.value;
		fwidth = pwidth / vid_conwidth.value;
		fheight = pheight / vid_conheight.value;
		for (finger = 0;finger < MAXFINGERS;finger++)
		{
			if (multitouch[finger][0] && multitouch[finger][1] >= fx && multitouch[finger][2] >= fy && multitouch[finger][1] < fx + fwidth && multitouch[finger][2] < fy + fheight)
			{
				if (cursorfinger == -1)
				{
					clickxy[0] =  multitouch[finger][1] * vid_width.value - 0.5f * pwidth;
					clickxy[1] =  multitouch[finger][2] * vid_height.value - 0.5f * pheight;
					relclickxy[0] =  (multitouch[finger][1] - fx) * vid_width.value - 0.5f * pwidth;
					relclickxy[1] =  (multitouch[finger][2] - fy) * vid_height.value - 0.5f * pheight;
				}
				cursorfinger = finger;
				button = true;
				canclick = true;
				cursorfreemovement = false;
				break;
			}
		}
		if (scr_numtouchscreenareas < 128)
		{
			if (clickrealtime + 1 > host.realtime)
			{
				scr_touchscreenareas[scr_numtouchscreenareas].pic = "gfx/gui/touch_puck_cur_click.tga";
			}
			else if (button)
			{
				scr_touchscreenareas[scr_numtouchscreenareas].pic = "gfx/gui/touch_puck_cur_touch.tga";
			}
			else
			{
				switch ((int)host.realtime * 10 % 20)
				{
				case 0:
					scr_touchscreenareas[scr_numtouchscreenareas].pic = "gfx/gui/touch_puck_cur_touch.tga";
					break;
				default:
					scr_touchscreenareas[scr_numtouchscreenareas].pic = "gfx/gui/touch_puck_cur_idle.tga";
				}
			}
			scr_touchscreenareas[scr_numtouchscreenareas].text = "";
			scr_touchscreenareas[scr_numtouchscreenareas].textheight = 0;
			scr_touchscreenareas[scr_numtouchscreenareas].rect[0] = px;
			scr_touchscreenareas[scr_numtouchscreenareas].rect[1] = py;
			scr_touchscreenareas[scr_numtouchscreenareas].rect[2] = pwidth;
			scr_touchscreenareas[scr_numtouchscreenareas].rect[3] = pheight;
			scr_touchscreenareas[scr_numtouchscreenareas].active = button;
			scr_touchscreenareas[scr_numtouchscreenareas].activealpha = 1.0f;
			scr_touchscreenareas[scr_numtouchscreenareas].inactivealpha = 1.0f;
			scr_numtouchscreenareas++;
		}
	}

	if (cursorfinger != -1)
	{
		if (multitouch[cursorfinger][0])
		{
			if (multitouch[cursorfinger][1] * vid_width.value - 0.5f * pwidth < clickxy[0] - 1 ||
				multitouch[cursorfinger][1] * vid_width.value - 0.5f * pwidth > clickxy[0] + 1 ||
				multitouch[cursorfinger][2] * vid_height.value - 0.5f * pheight< clickxy[1] - 1 ||
				multitouch[cursorfinger][2] * vid_height.value - 0.5f * pheight> clickxy[1] + 1) // finger drifted more than the allowed amount
			{
				cursorfreemovement = true;
			}
			if (cursorfreemovement)
			{
				// in_windowmouse_x* is in screen resolution coordinates, not console resolution
				in_windowmouse_x = multitouch[cursorfinger][1] * vid_width.value - 0.5f * pwidth - relclickxy[0];
				in_windowmouse_y = multitouch[cursorfinger][2] * vid_height.value - 0.5f * pheight - relclickxy[1];
			}
		}
		else
		{
			cursorfinger = -1;
		}
	}

	if (resultbutton)
	{
		if (/**resultbutton != button && */(int)key > 0)
		{
			if (!button && !cursorfreemovement && canclick)
			{
				Key_Event(key, 0, true);
				canclick = false;
				clickrealtime = host.realtime;
			}

			// SS:BR can't qc can't cope with presses and releases on the same frame
			if (clickrealtime && clickrealtime + 0.1 < host.realtime)
			{
				Key_Event(key, 0, false);
				clickrealtime = 0;
			}
		}

		*resultbutton = button;
	}
}

void VID_BuildJoyState(vid_joystate_t *joystate)
{
	VID_Shared_BuildJoyState_Begin(joystate);

	if (vid_sdljoystick)
	{
		SDL_Joystick *joy = vid_sdljoystick;
		int j;

		if (vid_sdlgamecontroller)
		{
			for (j = 0; j <= SDL_CONTROLLER_AXIS_MAX; ++j)
			{
				joystate->axis[j] = SDL_GameControllerGetAxis(vid_sdlgamecontroller, (SDL_GameControllerAxis)j) * (1.0f / 32767.0f);
			}
			for (j = 0; j < SDL_CONTROLLER_BUTTON_MAX; ++j)
				joystate->button[j] = SDL_GameControllerGetButton(vid_sdlgamecontroller, (SDL_GameControllerButton)j);
			// emulate joy buttons for trigger "axes"
			joystate->button[SDL_CONTROLLER_BUTTON_MAX] = VID_JoyState_GetAxis(joystate, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1, joy_sdl2_trigger_deadzone.value) > 0.0f;
			joystate->button[SDL_CONTROLLER_BUTTON_MAX+1] = VID_JoyState_GetAxis(joystate, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1, joy_sdl2_trigger_deadzone.value) > 0.0f;
		}
		else

		{
			int numaxes;
			int numbuttons;
			numaxes = SDL_JoystickNumAxes(joy);
			for (j = 0;j < numaxes;j++)
				joystate->axis[j] = SDL_JoystickGetAxis(joy, j) * (1.0f / 32767.0f);
			numbuttons = SDL_JoystickNumButtons(joy);
			for (j = 0;j < numbuttons;j++)
				joystate->button[j] = SDL_JoystickGetButton(joy, j);
		}
	}

	VID_Shared_BuildJoyState_Finish(joystate);
}

// clear every touch screen area, except the one with button[skip]
#define Vid_ClearAllTouchscreenAreas(skip) \
	if (skip != 0) \
		VID_TouchscreenCursor(0, 0, 0, 0, &buttons[0], K_MOUSE1); \
	if (skip != 1) \
		VID_TouchscreenArea( 0,   0,   0,   0,   0, NULL                         , 0.0f, NULL, move, &buttons[1], K_MOUSE4, NULL, 0, 0, 0, false); \
	if (skip != 2) \
		VID_TouchscreenArea( 0,   0,   0,   0,   0, NULL                         , 0.0f, NULL, aim,  &buttons[2], K_MOUSE5, NULL, 0, 0, 0, false); \
	if (skip != 3) \
		VID_TouchscreenArea( 0,   0,   0,   0,   0, NULL                         , 0.0f, NULL, NULL, &buttons[3], K_SHIFT, NULL, 0, 0, 0, false); \
	if (skip != 4) \
		VID_TouchscreenArea( 0,   0,   0,   0,   0, NULL                         , 0.0f, NULL, NULL, &buttons[4], K_MOUSE2, NULL, 0, 0, 0, false); \
	if (skip != 9) \
		VID_TouchscreenArea( 0,   0,   0,   0,   0, NULL                         , 0.0f, NULL, NULL, &buttons[9], K_MOUSE3, NULL, 0, 0, 0, false); \
	if (skip != 10) \
		VID_TouchscreenArea( 0,   0,   0,   0,   0, NULL                         , 0.0f, NULL, NULL, &buttons[10], (keynum_t)'m', NULL, 0, 0, 0, false); \
	if (skip != 11) \
		VID_TouchscreenArea( 0,   0,   0,   0,   0, NULL                         , 0.0f, NULL, NULL, &buttons[11], (keynum_t)'b', NULL, 0, 0, 0, false); \
	if (skip != 12) \
		VID_TouchscreenArea( 0,   0,   0,   0,   0, NULL                         , 0.0f, NULL, NULL, &buttons[12], (keynum_t)'q', NULL, 0, 0, 0, false); \
	if (skip != 13) \
		VID_TouchscreenArea( 0,   0,   0,   0,   0, NULL                         , 0.0f, NULL, NULL, &buttons[13], (keynum_t)'`', NULL, 0, 0, 0, false); \
	if (skip != 14) \
		VID_TouchscreenArea( 0,   0,   0,   0,   0, NULL                         , 0.0f, NULL, NULL, &buttons[14], K_ESCAPE, NULL, 0, 0, 0, false); \
	if (skip != 15) \
		VID_TouchscreenArea( 0,  0,  0,  0,  0, NULL                         , 0.0f, NULL, NULL, &buttons[15], K_SPACE, NULL, 0, 0, 0, false); \

/////////////////////
// Movement handling
////

static void IN_Move_TouchScreen_SteelStorm(void)
{
	// ELUAN
	int i, numfingers;
	float xscale, yscale;
	float move[3], aim[3];
	static qbool oldbuttons[128];
	static qbool buttons[128];
	keydest_t keydest = (key_consoleactive & KEY_CONSOLEACTIVE_USER) ? key_console : key_dest;
	memcpy(oldbuttons, buttons, sizeof(oldbuttons));
	memset(multitouchs, 0, sizeof(multitouchs));

	for (i = 0, numfingers = 0; i < MAXFINGERS - 1; i++)
		if (multitouch[i][0])
			numfingers++;

	/*
	Enable this to use a mouse as a touch device (it may conflict with the iamexclusive parameter if a finger is also reported as a mouse at the same location
	if (numfingers == 1)
	{
		multitouch[MAXFINGERS-1][0] = SDL_GetMouseState(&x, &y) ? 11 : 0;
		multitouch[MAXFINGERS-1][1] = (float)x / vid.width;
		multitouch[MAXFINGERS-1][2] = (float)y / vid.height;
	}
	else
	{
		// disable it so it doesn't get stuck, because SDL seems to stop updating it if there are more than 1 finger on screen
		multitouch[MAXFINGERS-1][0] = 0;
	}*/

	// TODO: make touchscreen areas controlled by a config file or the VMs. THIS IS A MESS!
	// TODO: can't just clear buttons[] when entering a new keydest, some keys would remain pressed
	// SS:BR menuqc has many peculiarities, including that it can't accept more than one command per frame and pressing and releasing on the same frame

	// Tuned for the SGS3, use it's value as a base. CLEAN THIS.
	xscale = vid_touchscreen_density.value / 2.0f;
	yscale = vid_touchscreen_density.value / 2.0f;
	switch(keydest)
	{
	case key_console:
		Vid_ClearAllTouchscreenAreas(14);
		VID_TouchscreenArea( 0,   0, 160,  64,  64, "gfx/gui/touch_menu_button.tga"         , 0.0f, NULL, NULL, &buttons[14], K_ESCAPE, NULL, 0, 0, 0, false);
		break;
	case key_game:
		if (steelstorm_showing_map && steelstorm_showing_map->integer) // FIXME: another hack to be removed when touchscreen areas go to QC
		{
			VID_TouchscreenArea( 0,   0,   0, vid_conwidth.value, vid_conheight.value, NULL                         , 0.0f, NULL, NULL, &buttons[10], (keynum_t)'m', NULL, 0, 0, 0, false);
			Vid_ClearAllTouchscreenAreas(10);
		}
		else if (steelstorm_showing_mousecursor && steelstorm_showing_mousecursor->integer)
		{
			// in_windowmouse_x* is in screen resolution coordinates, not console resolution
			VID_TouchscreenCursor((float)in_windowmouse_x/vid_width.value*vid_conwidth.value, (float)in_windowmouse_y/vid_height.value*vid_conheight.value, 192*xscale, 192*yscale, &buttons[0], K_MOUSE1);
			Vid_ClearAllTouchscreenAreas(0);
		}
		else
		{
			VID_TouchscreenCursor(0, 0, 0, 0, &buttons[0], K_MOUSE1);

			VID_TouchscreenArea( 2,16*xscale,-240*yscale, 224*xscale, 224*yscale, "gfx/gui/touch_l_thumb_dpad.tga", 0.0f, NULL, move, &buttons[1], (keynum_t)0, NULL, 0.15, 112*xscale, 112*yscale, false);

			VID_TouchscreenArea( 3,-240*xscale,-160*yscale, 224*xscale, 128*yscale, "gfx/gui/touch_r_thumb_turn_n_shoot.tga"    , 0.0f, NULL, NULL,  0, (keynum_t)0, NULL, 0, 56*xscale, 0, false);
			VID_TouchscreenArea( 3,-240*xscale,-256*yscale, 224*xscale, 224*yscale, NULL    , 0.0f, NULL, aim,  &buttons[2], (keynum_t)0, NULL, 0.2, 56*xscale, 0, false);

			VID_TouchscreenArea( 2, (vid_conwidth.value / 2) - 128,-80,  256,  80, NULL, 0.0f, NULL, NULL, &buttons[3], K_SHIFT, NULL, 0, 0, 0, true);

			VID_TouchscreenArea( 3,-240*xscale,-256*yscale, 224*xscale,  64*yscale, "gfx/gui/touch_secondary_slide.tga", 0.0f, NULL, NULL, &buttons[4], K_MOUSE2, NULL, 0, 56*xscale, 0, false);
			VID_TouchscreenArea( 3,-240*xscale,-256*yscale, 224*xscale,  160*yscale, NULL , 0.0f, NULL, NULL, &buttons[9], K_MOUSE3, NULL, 0.2, 56*xscale, 0, false);

			VID_TouchscreenArea( 1,-100,   0, 100, 100, NULL                         , 0.0f, NULL, NULL, &buttons[10], (keynum_t)'m', NULL, 0, 0, 0, true);
			VID_TouchscreenArea( 1,-100, 120, 100, 100, NULL                         , 0.0f, NULL, NULL, &buttons[11], (keynum_t)'b', NULL, 0, 0, 0, true);
			VID_TouchscreenArea( 0,   0,   0,  64,  64, NULL                         , 0.0f, NULL, NULL, &buttons[12], (keynum_t)'q', NULL, 0, 0, 0, true);
			if (developer.integer)
				VID_TouchscreenArea( 0,   0,  96,  64,  64, NULL                         , 0.0f, NULL, NULL, &buttons[13], (keynum_t)'`', NULL, 0, 0, 0, true);
			else
				VID_TouchscreenArea( 0,   0,   0,   0,   0, NULL                         , 0.0f, NULL, NULL, &buttons[13], (keynum_t)'`', NULL, 0, 0, 0, false);
			VID_TouchscreenArea( 0,   0, 160,  64,  64, "gfx/gui/touch_menu_button.tga"         , 0.0f, NULL, NULL, &buttons[14], K_ESCAPE, NULL, 0, 0, 0, true);
			switch(cl.activeweapon)
			{
			case 14:
				VID_TouchscreenArea( 2,  16*xscale,-320*yscale, 224*xscale, 64*yscale, "gfx/gui/touch_booster.tga" , 0.0f, NULL, NULL, &buttons[15], K_SPACE, NULL, 0, 0, 0, true);
				break;
			case 12:
				VID_TouchscreenArea( 2,  16*xscale,-320*yscale, 224*xscale, 64*yscale, "gfx/gui/touch_shockwave.tga" , 0.0f, NULL, NULL, &buttons[15], K_SPACE, NULL, 0, 0, 0, true);
				break;
			default:
				VID_TouchscreenArea( 0,  0,  0,  0,  0, NULL , 0.0f, NULL, NULL, &buttons[15], K_SPACE, NULL, 0, 0, 0, false);
			}
		}
		break;
	default:
		if (!steelstorm_showing_mousecursor || !steelstorm_showing_mousecursor->integer)
		{
			Vid_ClearAllTouchscreenAreas(14);
			// this way we can skip cutscenes
			VID_TouchscreenArea( 0,   0,   0, vid_conwidth.value, vid_conheight.value, NULL                         , 0.0f, NULL, NULL, &buttons[14], K_ESCAPE, NULL, 0, 0, 0, false);
		}
		else
		{
			// in_windowmouse_x* is in screen resolution coordinates, not console resolution
			VID_TouchscreenCursor((float)in_windowmouse_x/vid_width.value*vid_conwidth.value, (float)in_windowmouse_y/vid_height.value*vid_conheight.value, 192*xscale, 192*yscale, &buttons[0], K_MOUSE1);
			Vid_ClearAllTouchscreenAreas(0);
		}
		break;
	}

	if (VID_ShowingKeyboard() && (float)in_windowmouse_y > vid_height.value / 2 - 10)
		in_windowmouse_y = 128;

	cl.cmd.forwardmove -= move[1] * cl_forwardspeed.value;
	cl.cmd.sidemove += move[0] * cl_sidespeed.value;
	cl.viewangles[0] += aim[1] * cl_pitchspeed.value * cl.realframetime;
	cl.viewangles[1] -= aim[0] * cl_yawspeed.value * cl.realframetime;
}

static void IN_Move_TouchScreen_Quake(void)
{
	int x, y;
	float move[3], aim[3], click[3];
	static qbool oldbuttons[128];
	static qbool buttons[128];
	keydest_t keydest = (key_consoleactive & KEY_CONSOLEACTIVE_USER) ? key_console : key_dest;
	memcpy(oldbuttons, buttons, sizeof(oldbuttons));
	memset(multitouchs, 0, sizeof(multitouchs));

	// simple quake controls
	multitouch[MAXFINGERS-1][0] = SDL_GetMouseState(&x, &y);
	multitouch[MAXFINGERS-1][1] = x * 32768 / vid.mode.width;
	multitouch[MAXFINGERS-1][2] = y * 32768 / vid.mode.height;

	// top of screen is toggleconsole and K_ESCAPE
	switch(keydest)
	{
	case key_console:
		VID_TouchscreenArea( 0,   0,   0,  64,  64, NULL                         , 0.0f, NULL, NULL, &buttons[13], (keynum_t)'`', NULL, 0, 0, 0, true);
		VID_TouchscreenArea( 0,  64,   0,  64,  64, "gfx/touch_menu.tga"         , 0.0f, NULL, NULL, &buttons[14], K_ESCAPE, NULL, 0, 0, 0, true);
		if (!VID_ShowingKeyboard())
		{
			// user entered a command, close the console now
			Con_ToggleConsole_f(cmd_local);
		}
		VID_TouchscreenArea( 0,   0,   0,   0,   0, NULL                         , 0.0f, NULL, NULL, &buttons[15], (keynum_t)0, NULL, 0, 0, 0, true);
		VID_TouchscreenArea( 0,   0,   0,   0,   0, NULL                         , 0.0f, NULL, move, &buttons[0], K_MOUSE4, NULL, 0, 0, 0, true);
		VID_TouchscreenArea( 0,   0,   0,   0,   0, NULL                         , 0.0f, NULL, aim,  &buttons[1], K_MOUSE5, NULL, 0, 0, 0, true);
		VID_TouchscreenArea( 0,   0,   0,   0,   0, NULL                         , 0.0f, NULL, click,&buttons[2], K_MOUSE1, NULL, 0, 0, 0, true);
		VID_TouchscreenArea( 0,   0,   0,   0,   0, NULL                         , 0.0f, NULL, NULL, &buttons[3], K_SPACE, NULL, 0, 0, 0, true);
		VID_TouchscreenArea( 0,   0,   0,   0,   0, NULL                         , 0.0f, NULL, NULL, &buttons[4], K_MOUSE2, NULL, 0, 0, 0, true);
		break;
	case key_game:
		VID_TouchscreenArea( 0,   0,   0,  64,  64, NULL                         , 0.0f, NULL, NULL, &buttons[13], (keynum_t)'`', NULL, 0, 0, 0, true);
		VID_TouchscreenArea( 0,  64,   0,  64,  64, "gfx/touch_menu.tga"         , 0.0f, NULL, NULL, &buttons[14], K_ESCAPE, NULL, 0, 0, 0, true);
		VID_TouchscreenArea( 2,   0,-128, 128, 128, "gfx/touch_movebutton.tga"   , 0.0f, NULL, move, &buttons[0], K_MOUSE4, NULL, 0, 0, 0, true);
		VID_TouchscreenArea( 3,-128,-128, 128, 128, "gfx/touch_aimbutton.tga"    , 0.0f, NULL, aim,  &buttons[1], K_MOUSE5, NULL, 0, 0, 0, true);
		VID_TouchscreenArea( 2,   0,-160,  64,  32, "gfx/touch_jumpbutton.tga"   , 0.0f, NULL, NULL, &buttons[3], K_SPACE, NULL, 0, 0, 0, true);
		VID_TouchscreenArea( 3,-128,-160,  64,  32, "gfx/touch_attackbutton.tga" , 0.0f, NULL, NULL, &buttons[2], K_MOUSE1, NULL, 0, 0, 0, true);
		VID_TouchscreenArea( 3, -64,-160,  64,  32, "gfx/touch_attack2button.tga", 0.0f, NULL, NULL, &buttons[4], K_MOUSE2, NULL, 0, 0, 0, true);
		buttons[15] = false;
		break;
	default:
		VID_TouchscreenArea( 0,   0,   0,  64,  64, NULL                         , 0.0f, NULL, NULL, &buttons[13], (keynum_t)'`', NULL, 0, 0, 0, true);
		VID_TouchscreenArea( 0,  64,   0,  64,  64, "gfx/touch_menu.tga"         , 0.0f, NULL, NULL, &buttons[14], K_ESCAPE, NULL, 0, 0, 0, true);
		// in menus, an icon in the corner activates keyboard
		VID_TouchscreenArea( 2,   0, -32,  32,  32, "gfx/touch_keyboard.tga"     , 0.0f, NULL, NULL, &buttons[15], (keynum_t)0, NULL, 0, 0, 0, true);
		if (buttons[15])
			VID_ShowKeyboard(true);
		VID_TouchscreenArea( 0,   0,   0,   0,   0, NULL                         , 0.0f, NULL, move, &buttons[0], K_MOUSE4, NULL, 0, 0, 0, true);
		VID_TouchscreenArea( 0,   0,   0,   0,   0, NULL                         , 0.0f, NULL, aim,  &buttons[1], K_MOUSE5, NULL, 0, 0, 0, true);
		VID_TouchscreenArea(16, -320,-480,640, 960, NULL                         , 0.0f, NULL, click,&buttons[2], K_MOUSE1, NULL, 0, 0, 0, true);
		VID_TouchscreenArea( 0,   0,   0,   0,   0, NULL                         , 0.0f, NULL, NULL, &buttons[3], K_SPACE, NULL, 0, 0, 0, true);
		VID_TouchscreenArea( 0,   0,   0,   0,   0, NULL                         , 0.0f, NULL, NULL, &buttons[4], K_MOUSE2, NULL, 0, 0, 0, true);
		if (buttons[2])
		{
			in_windowmouse_x = x;
			in_windowmouse_y = y;
		}
		break;
	}

	cl.cmd.forwardmove -= move[1] * cl_forwardspeed.value;
	cl.cmd.sidemove += move[0] * cl_sidespeed.value;
	cl.viewangles[0] += aim[1] * cl_pitchspeed.value * cl.realframetime;
	cl.viewangles[1] -= aim[0] * cl_yawspeed.value * cl.realframetime;
}

void IN_Move( void )
{
	static int old_x = 0, old_y = 0;
	static int stuck = 0;
	static keydest_t oldkeydest;
	static qbool oldshowkeyboard;
	int x, y;
	vid_joystate_t joystate;
	keydest_t keydest = (key_consoleactive & KEY_CONSOLEACTIVE_USER) ? key_console : key_dest;

	scr_numtouchscreenareas = 0;

	// Only apply the new keyboard state if the input changes.
	if (keydest != oldkeydest || !!vid_touchscreen_showkeyboard.integer != oldshowkeyboard)
	{
		switch(keydest)
		{
			case key_console: VID_ShowKeyboard(true);break;
			case key_message: VID_ShowKeyboard(true);break;
			default: VID_ShowKeyboard(!!vid_touchscreen_showkeyboard.integer); break;
		}
	}
	oldkeydest = keydest;
	oldshowkeyboard = !!vid_touchscreen_showkeyboard.integer;

	if (vid_touchscreen.integer)
	{
		switch(gamemode)
		{
		case GAME_STEELSTORM:
			IN_Move_TouchScreen_SteelStorm();
			break;
		default:
			IN_Move_TouchScreen_Quake();
			break;
		}
	}
	else
	{
		if (vid_usingmouse)
		{
			if (vid_stick_mouse.integer || !vid_usingmouse_relativeworks)
			{
				// have the mouse stuck in the middle, example use: prevent expose effect of beryl during the game when not using
				// window grabbing. --blub
				int win_half_width = vid.mode.width>>1;
				int win_half_height = vid.mode.height>>1;
	
				// we need 2 frames to initialize the center position
				if(!stuck)
				{
					SDL_WarpMouseInWindow(window, win_half_width, win_half_height);
					SDL_GetMouseState(&x, &y);
					SDL_GetRelativeMouseState(&x, &y);
					++stuck;
				} else {
					SDL_GetRelativeMouseState(&x, &y);
					in_mouse_x = x + old_x;
					in_mouse_y = y + old_y;
					SDL_GetMouseState(&x, &y);
					old_x = x - win_half_width;
					old_y = y - win_half_height;
					SDL_WarpMouseInWindow(window, win_half_width, win_half_height);
				}
			} else {
				SDL_GetRelativeMouseState( &x, &y );
				in_mouse_x = x;
				in_mouse_y = y;
			}
		}

		SDL_GetMouseState(&x, &y);
		in_windowmouse_x = x;
		in_windowmouse_y = y;
	}

	//Con_Printf("Mouse position: in_mouse %f %f in_windowmouse %f %f\n", in_mouse_x, in_mouse_y, in_windowmouse_x, in_windowmouse_y);

	VID_BuildJoyState(&joystate);
	VID_ApplyJoyState(&joystate);
}

/////////////////////
// Message Handling
////

static keynum_t buttonremap[] =
{
	K_MOUSE1,
	K_MOUSE3,
	K_MOUSE2,
	K_MOUSE4,
	K_MOUSE5,
	K_MOUSE6,
	K_MOUSE7,
	K_MOUSE8,
	K_MOUSE9,
	K_MOUSE10,
	K_MOUSE11,
	K_MOUSE12,
	K_MOUSE13,
	K_MOUSE14,
	K_MOUSE15,
	K_MOUSE16,
};

//#define DEBUGSDLEVENTS
void Sys_SDL_HandleEvents(void)
{
	int keycode;
	int i;
	const char *chp;
	qbool isdown;
	Uchar unicode;
	SDL_Event event;

	VID_EnableJoystick(true);

	while( SDL_PollEvent( &event ) )
		loop_start:
		switch( event.type ) {
			case SDL_QUIT:
#ifdef DEBUGSDLEVENTS
				Con_DPrintf("SDL_Event: SDL_QUIT\n");
#endif
				host.state = host_shutdown;
				break;
			case SDL_KEYDOWN:
			case SDL_KEYUP:
#ifdef DEBUGSDLEVENTS
				if (event.type == SDL_KEYDOWN)
					Con_DPrintf("SDL_Event: SDL_KEYDOWN %i\n", event.key.keysym.sym);
				else
					Con_DPrintf("SDL_Event: SDL_KEYUP %i\n", event.key.keysym.sym);
#endif
				keycode = MapKey(event.key.keysym.sym);
				isdown = (event.key.state == SDL_PRESSED);
				unicode = 0;
				if(isdown)
				{
					if(SDL_PollEvent(&event))
					{
						if(event.type == SDL_TEXTINPUT)
						{
							// combine key code from SDL_KEYDOWN event and character
							// from SDL_TEXTINPUT event in a single Key_Event call
#ifdef DEBUGSDLEVENTS
							Con_DPrintf("SDL_Event: SDL_TEXTINPUT - text: %s\n", event.text.text);
#endif
							unicode = u8_getchar_utf8_enabled(event.text.text + (int)u8_bytelen(event.text.text, 0), NULL);
						}
						else
						{
							if (!VID_JoyBlockEmulatedKeys(keycode))
								Key_Event(keycode, 0, isdown);
							goto loop_start;
						}
					}
				}
				if (!VID_JoyBlockEmulatedKeys(keycode))
					Key_Event(keycode, unicode, isdown);
				break;
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP:
#ifdef DEBUGSDLEVENTS
				if (event.type == SDL_MOUSEBUTTONDOWN)
					Con_DPrintf("SDL_Event: SDL_MOUSEBUTTONDOWN\n");
				else
					Con_DPrintf("SDL_Event: SDL_MOUSEBUTTONUP\n");
#endif
				if (!vid_touchscreen.integer)
				if (event.button.button > 0 && event.button.button <= ARRAY_SIZE(buttonremap))
					Key_Event( buttonremap[event.button.button - 1], 0, event.button.state == SDL_PRESSED );
				break;
			case SDL_MOUSEWHEEL:
				// TODO support wheel x direction.
				i = event.wheel.y;
				while (i > 0) {
					--i;
					Key_Event( K_MWHEELUP, 0, true );
					Key_Event( K_MWHEELUP, 0, false );
				}
				while (i < 0) {
					++i;
					Key_Event( K_MWHEELDOWN, 0, true );
					Key_Event( K_MWHEELDOWN, 0, false );
				}
				break;
			case SDL_JOYBUTTONDOWN:
			case SDL_JOYBUTTONUP:
			case SDL_JOYAXISMOTION:
			case SDL_JOYBALLMOTION:
			case SDL_JOYHATMOTION:
#ifdef DEBUGSDLEVENTS
				Con_DPrintf("SDL_Event: SDL_JOY*\n");
#endif
				break;
			case SDL_WINDOWEVENT:
#ifdef DEBUGSDLEVENTS
				Con_DPrintf("SDL_Event: SDL_WINDOWEVENT %i\n", (int)event.window.event);
#endif
				//if (event.window.windowID == window) // how to compare?
				{
					switch(event.window.event)
					{
					case SDL_WINDOWEVENT_SHOWN:
						vid_hidden = false;
						break;
					case  SDL_WINDOWEVENT_HIDDEN:
						vid_hidden = true;
						break;
					case SDL_WINDOWEVENT_EXPOSED:
#ifdef DEBUGSDLEVENTS
						Con_DPrintf("SDL_Event: SDL_WINDOWEVENT_EXPOSED\n");
#endif
						break;
					case SDL_WINDOWEVENT_MOVED:
						vid.xPos = event.window.data1;
						vid.yPos = event.window.data2;
						// Update vid.displayindex (current monitor) as it may have changed
						// SDL_GetWindowDisplayIndex() doesn't work if the window manager moves the fullscreen window, but this works:
						for (i = 0; i < vid_info_displaycount.integer; ++i)
						{
							SDL_Rect displaybounds;
							if (SDL_GetDisplayBounds(i, &displaybounds) < 0)
							{
								Con_Printf(CON_ERROR "Error getting bounds of display %i: \"%s\"\n", i, SDL_GetError());
								return;
							}
							if (vid.xPos >= displaybounds.x && vid.xPos < displaybounds.x + displaybounds.w)
							if (vid.yPos >= displaybounds.y && vid.yPos < displaybounds.y + displaybounds.h)
							{
								vid.mode.display = i;
								break;
							}
						}
						// when the window manager adds/removes the border it's likely to move the SDL window
						// we'll need to correct that to (re)align the xhair with the monitor
						if (vid_wmborder_waiting)
						{
							SDL_GetWindowBordersSize(window, &i, NULL, NULL, NULL);
							if (!i != vid_wmborderless) // border state changed
							{
								SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED_DISPLAY(vid.mode.display), SDL_WINDOWPOS_CENTERED_DISPLAY(vid.mode.display));
								SDL_GetWindowPosition(window, &vid.xPos, &vid.yPos);
								vid_wmborder_waiting = false;
							}
						}
						break;
					case SDL_WINDOWEVENT_RESIZED: // external events only
						if(vid_resizable.integer < 2)
						{
							//vid.width = event.window.data1;
							//vid.height = event.window.data2;
							// get the real framebuffer size in case the platform's screen coordinates are DPI scaled
							SDL_GL_GetDrawableSize(window, &vid.mode.width, &vid.mode.height);
						}
						break;
					case SDL_WINDOWEVENT_SIZE_CHANGED: // internal and external events
						break;
					case SDL_WINDOWEVENT_MINIMIZED:
						break;
					case SDL_WINDOWEVENT_MAXIMIZED:
						break;
					case SDL_WINDOWEVENT_RESTORED:
						break;
					case SDL_WINDOWEVENT_ENTER:
						break;
					case SDL_WINDOWEVENT_LEAVE:
						break;
					case SDL_WINDOWEVENT_FOCUS_GAINED:
						vid_hasfocus = true;
						break;
					case SDL_WINDOWEVENT_FOCUS_LOST:
						vid_hasfocus = false;
						break;
					case SDL_WINDOWEVENT_CLOSE:
						host.state = host_shutdown;
						break;
					case SDL_WINDOWEVENT_TAKE_FOCUS:
						break;
					case SDL_WINDOWEVENT_HIT_TEST:
						break;
					case SDL_WINDOWEVENT_ICCPROF_CHANGED:
						break;
					case SDL_WINDOWEVENT_DISPLAY_CHANGED:
						// this event can't be relied on in fullscreen, see SDL_WINDOWEVENT_MOVED above
						vid.mode.display = event.window.data1;
						break;
					}
				}
				break;
			case SDL_DISPLAYEVENT: // Display hotplugging
				switch (event.display.event)
				{
					case SDL_DISPLAYEVENT_CONNECTED:
						Con_Printf(CON_WARN "Display %i connected: %s\n", event.display.display, SDL_GetDisplayName(event.display.display));
#ifdef __linux__
						Con_Print(CON_WARN "A vid_restart may be necessary!\n");
#endif
						Cvar_SetValueQuick(&vid_info_displaycount, SDL_GetNumVideoDisplays());
						// Ideally we'd call VID_ApplyDisplayMode() to try to switch to the preferred display here,
						// but we may need a vid_restart first, see comments in VID_ApplyDisplayMode().
						break;
					case SDL_DISPLAYEVENT_DISCONNECTED:
						Con_Printf(CON_WARN "Display %i disconnected.\n", event.display.display);
#ifdef __linux__
						Con_Print(CON_WARN "A vid_restart may be necessary!\n");
#endif
						Cvar_SetValueQuick(&vid_info_displaycount, SDL_GetNumVideoDisplays());
						break;
					case SDL_DISPLAYEVENT_ORIENTATION:
						break;
				}
				break;
			case SDL_TEXTEDITING:
#ifdef DEBUGSDLEVENTS
				Con_DPrintf("SDL_Event: SDL_TEXTEDITING - composition = %s, cursor = %d, selection lenght = %d\n", event.edit.text, event.edit.start, event.edit.length);
#endif
				// FIXME!  this is where composition gets supported
				break;
			case SDL_TEXTINPUT:
#ifdef DEBUGSDLEVENTS
				Con_DPrintf("SDL_Event: SDL_TEXTINPUT - text: %s\n", event.text.text);
#endif
				// convert utf8 string to char
				// NOTE: this code is supposed to run even if utf8enable is 0
				chp = event.text.text;
				while (*chp != 0)
				{
					// input the chars one by one (there can be multiple chars when e.g. using an "input method")
					unicode = u8_getchar_utf8_enabled(chp, &chp);
					Key_Event(K_TEXT, unicode, true);
					Key_Event(K_TEXT, unicode, false);
				}
				break;
			case SDL_MOUSEMOTION:
				break;
			case SDL_FINGERDOWN:
#ifdef DEBUGSDLEVENTS
				Con_DPrintf("SDL_FINGERDOWN for finger %i\n", (int)event.tfinger.fingerId);
#endif
				for (i = 0;i < MAXFINGERS-1;i++)
				{
					if (!multitouch[i][0])
					{
						multitouch[i][0] = event.tfinger.fingerId + 1;
						multitouch[i][1] = event.tfinger.x;
						multitouch[i][2] = event.tfinger.y;
						// TODO: use event.tfinger.pressure?
						break;
					}
				}
				if (i == MAXFINGERS-1)
					Con_DPrintf("Too many fingers at once!\n");
				break;
			case SDL_FINGERUP:
#ifdef DEBUGSDLEVENTS
				Con_DPrintf("SDL_FINGERUP for finger %i\n", (int)event.tfinger.fingerId);
#endif
				for (i = 0;i < MAXFINGERS-1;i++)
				{
					if (multitouch[i][0] == event.tfinger.fingerId + 1)
					{
						multitouch[i][0] = 0;
						break;
					}
				}
				if (i == MAXFINGERS-1)
					Con_DPrintf("No SDL_FINGERDOWN event matches this SDL_FINGERMOTION event\n");
				break;
			case SDL_FINGERMOTION:
#ifdef DEBUGSDLEVENTS
				Con_DPrintf("SDL_FINGERMOTION for finger %i\n", (int)event.tfinger.fingerId);
#endif
				for (i = 0;i < MAXFINGERS-1;i++)
				{
					if (multitouch[i][0] == event.tfinger.fingerId + 1)
					{
						multitouch[i][1] = event.tfinger.x;
						multitouch[i][2] = event.tfinger.y;
						break;
					}
				}
				if (i == MAXFINGERS-1)
					Con_DPrintf("No SDL_FINGERDOWN event matches this SDL_FINGERMOTION event\n");
				break;
			default:
#ifdef DEBUGSDLEVENTS
				Con_DPrintf("Received unrecognized SDL_Event type 0x%x\n", event.type);
#endif
				break;
		}

	vid_activewindow = !vid_hidden && vid_hasfocus;

	if (!vid_activewindow || key_consoleactive || scr_loading)
		VID_SetMouse(false, false);
	else if (key_dest == key_menu || key_dest == key_menu_grabbed)
		VID_SetMouse(vid_mouse.integer && !in_client_mouse && !vid_touchscreen.integer, !vid_touchscreen.integer);
	else
		VID_SetMouse(vid_mouse.integer && !cl.csqc_wantsmousemove && cl_prydoncursor.integer <= 0 && (!cls.demoplayback || cl_demo_mousegrab.integer) && !vid_touchscreen.integer, !vid_touchscreen.integer);
}

/////////////////
// Video system
////

void *GL_GetProcAddress(const char *name)
{
	void *p = NULL;
	p = SDL_GL_GetProcAddress(name);
	return p;
}

qbool GL_ExtensionSupported(const char *name)
{
	return SDL_GL_ExtensionSupported(name);
}

/// Applies display settings immediately (no vid_restart required).
static void VID_ApplyDisplayMode(const viddef_mode_t *mode)
{
	uint32_t fullscreenwanted;
	int displaywanted = bound(0, mode->display, vid_info_displaycount.integer - 1);
	SDL_DisplayMode modefinal;

	if (mode->fullscreen)
		fullscreenwanted = mode->desktopfullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_FULLSCREEN;
	else
		fullscreenwanted = 0;

	// moving to another display or switching to windowed
	if (vid.mode.display != displaywanted // SDL seems unable to move any fullscreen window to another display
	|| !fullscreenwanted)
	{
		if (SDL_SetWindowFullscreen(window, 0) < 0)
		{
			Con_Printf(CON_ERROR "ERROR: can't deactivate fullscreen on display %i because %s\n", vid.mode.display, SDL_GetError());
			return;
		}
		vid.mode.desktopfullscreen = vid.mode.fullscreen = false;
		Con_DPrintf("Fullscreen deactivated on display %i\n", vid.mode.display);
	}

	// switching to windowed
	if (!fullscreenwanted)
	{
		int toppx;

		SDL_SetWindowSize(window, vid.mode.width = mode->width, vid.mode.height = mode->height);
		// resizable and borderless set here cos a separate callback would fail if the cvar is changed when the window is fullscreen
		SDL_SetWindowResizable(window, vid_resizable.integer ? SDL_TRUE : SDL_FALSE);
		SDL_SetWindowBordered(window, (SDL_bool)!vid_borderless.integer);
		SDL_GetWindowBordersSize(window, &toppx, NULL, NULL, NULL);
		vid_wmborderless = !toppx;
		if (vid_borderless.integer != vid_wmborderless) // this is not the state we're looking for
			vid_wmborder_waiting = true;
	}

	// moving to another display or switching to windowed
	if (vid.mode.display != displaywanted || !fullscreenwanted)
	{
//		SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED_DISPLAY(displaywanted), SDL_WINDOWPOS_CENTERED_DISPLAY(displaywanted));
//		SDL_GetWindowPosition(window, &vid.xPos, &vid.yPos);

		/* bones_was_here BUG: after SDL_DISPLAYEVENT hotplug events, on Xorg + NVIDIA,
		 * SDL_WINDOWPOS_CENTERED_DISPLAY(displaywanted) may place the window somewhere completely invisible.
		 * WORKAROUND: manual positioning seems safer: although SDL_GetDisplayBounds() may return outdated values,
		 * SDL_SetWindowPosition() always placed the window somewhere fully visible, even if it wasn't correct,
		 * when tested with SDL 2.26.5.
		 */
		SDL_Rect displaybounds;
		if (SDL_GetDisplayBounds(displaywanted, &displaybounds) < 0)
		{
			Con_Printf(CON_ERROR "Error getting bounds of display %i: \"%s\"\n", displaywanted, SDL_GetError());
			return;
		}
		vid.xPos = displaybounds.x + 0.5 * (displaybounds.w - vid.mode.width);
		vid.yPos = displaybounds.y + 0.5 * (displaybounds.h - vid.mode.height);
		SDL_SetWindowPosition(window, vid.xPos, vid.yPos);

		vid.mode.display = displaywanted;
	}

	// switching to a fullscreen mode
	if (fullscreenwanted)
	{
		if (fullscreenwanted == SDL_WINDOW_FULLSCREEN)
		{
			// determine if a modeset is needed and if the requested resolution is supported
			SDL_DisplayMode modewanted, modecurrent;

			modewanted.w = mode->width;
			modewanted.h = mode->height;
			modewanted.format = mode->bitsperpixel == 16 ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_RGB888;
			modewanted.refresh_rate = mode->refreshrate;
			if (!SDL_GetClosestDisplayMode(displaywanted, &modewanted, &modefinal))
			{
				// SDL_GetError() returns a random unrelated error if this fails (in 2.26.5)
				Con_Printf(CON_ERROR "Error getting closest mode to %ix%i@%ihz for display %i\n", modewanted.w, modewanted.h, modewanted.refresh_rate, vid.mode.display);
				return;
			}
			if (SDL_GetCurrentDisplayMode(displaywanted, &modecurrent) < 0)
			{
				Con_Printf(CON_ERROR "Error getting current mode of display %i: \"%s\"\n", vid.mode.display, SDL_GetError());
				return;
			}
			if (memcmp(&modecurrent, &modefinal, sizeof(modecurrent)) != 0)
			{
				if (mode->width != modefinal.w || mode->height != modefinal.h)
				{
					Con_Printf(CON_WARN "Display %i doesn't support resolution %ix%i\n", vid.mode.display, modewanted.w, modewanted.h);
					return;
				}
				if (SDL_SetWindowDisplayMode(window, &modefinal) < 0)
				{
					Con_Printf(CON_ERROR "Error setting mode %ix%i@%ihz for display %i: \"%s\"\n", modefinal.w, modefinal.h, modefinal.refresh_rate, vid.mode.display, SDL_GetError());
					return;
				}
				// HACK to work around SDL BUG when switching from a lower to a higher res:
				// the display res gets increased but the window size isn't increased
				// (unless we do this first; switching to windowed mode first also works).
				SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
			}
		}

		if (SDL_SetWindowFullscreen(window, fullscreenwanted) < 0)
		{
			Con_Printf(CON_ERROR "ERROR: can't activate fullscreen on display %i because %s\n", vid.mode.display, SDL_GetError());
			return;
		}
		// get the real framebuffer size in case the platform's screen coordinates are DPI scaled
		SDL_GL_GetDrawableSize(window, &vid.mode.width, &vid.mode.height);
		vid.mode.fullscreen = true;
		vid.mode.desktopfullscreen = fullscreenwanted == SDL_WINDOW_FULLSCREEN_DESKTOP;
		Con_DPrintf("Fullscreen activated on display %i\n", vid.mode.display);
	}

	if (!fullscreenwanted || fullscreenwanted == SDL_WINDOW_FULLSCREEN_DESKTOP)
		SDL_GetDesktopDisplayMode(displaywanted, &modefinal);
	else { /* modefinal was set by SDL_GetClosestDisplayMode */ }
	vid.mode.bitsperpixel = SDL_BITSPERPIXEL(modefinal.format);
	vid.mode.refreshrate  = mode->refreshrate && mode->fullscreen && !mode->desktopfullscreen ? modefinal.refresh_rate : 0;
	vid.stencil           = mode->bitsperpixel > 16;
}

static void VID_ApplyDisplayMode_c(cvar_t *var)
{
	viddef_mode_t mode;

	if (!window)
		return;

	// Menu designs aren't suitable for instant hardware modesetting
	// they make players scroll through a list, setting the cvars at each step.
	if (key_dest == key_menu && !key_consoleactive // in menu, console closed
	&& vid_fullscreen.integer && !vid_desktopfullscreen.integer) // modesetting enabled
		return;

	Con_DPrintf("%s: applying %s \"%s\"\n", __func__, var->name, var->string);

	mode.display           = vid_display.integer;
	mode.fullscreen        = vid_fullscreen.integer;
	mode.desktopfullscreen = vid_desktopfullscreen.integer;
	mode.width             = vid_width.integer;
	mode.height            = vid_height.integer;
	mode.bitsperpixel      = vid_bitsperpixel.integer;
	mode.refreshrate       = max(0, vid_refreshrate.integer);
	VID_ApplyDisplayMode(&mode);
}

static void VID_SetVsync_c(cvar_t *var)
{
	int vsyncwanted = cls.timedemo ? 0 : vid_vsync.integer;

#ifdef MACOSX
	// METAL.md Phase 8-2 rider. CAMetalLayer owns pacing on the Metal path;
	// SDL_GL_SetSwapInterval needs the GL context this path never creates, so
	// before this arm the `!context` early-out below swallowed EVERY runtime
	// vsync change on the shipped default renderer -- the menu row, the
	// console, and (the one with teeth) the timedemo force-off: cl_demo.c
	// re-invokes this callback at timedemo start and finish exactly so
	// benchmarks never run synced, and with vid_vsync archived on, every
	// Metal benchmark would have silently capped at the display refresh.
	if (vid.renderpath == RENDERPATH_METAL)
	{
		VID_Metal_SetVsync(vsyncwanted != 0);
		return;
	}
#endif
	if (!context)
		return;
/*
Can't check first: on Wayland SDL_GL_GetSwapInterval() may initially return 0 when vsync is on.
On Xorg it returns the correct value.
	if (SDL_GL_GetSwapInterval() == vsyncwanted)
		return;
*/

	// __EMSCRIPTEN__ SDL_GL_SetSwapInterval() calls emscripten_set_main_loop_timing()
	if (SDL_GL_SetSwapInterval(vsyncwanted) >= 0)
		Con_DPrintf("Vsync %s\n", vsyncwanted ? "activated" : "deactivated");
	else
		Con_Printf(CON_ERROR "ERROR: can't %s vsync because %s\n", vsyncwanted ? "activate" : "deactivate", SDL_GetError());
}

static void VID_SetHints_c(cvar_t *var)
{
	SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH,     vid_mouse_clickthrough.integer     ? "1" : "0");
	SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, vid_minimize_on_focus_loss.integer ? "1" : "0");
}

void VID_Init (void)
{
	SDL_version version;

#ifndef __IPHONEOS__
#ifdef MACOSX
	Cvar_RegisterVariable(&apple_mouse_noaccel);
	Cvar_RegisterVariable(&rt_metal);
	Cvar_RegisterVariable(&rt_metal_samples);
	Cvar_RegisterVariable(&rt_metal_scale);
	Cvar_RegisterVariable(&rt_metal_smoothnormals);
	Cvar_RegisterVariable(&rt_metal_reproject);
	Cvar_RegisterVariable(&rt_metal_reproject_depth);
	Cvar_RegisterVariable(&rt_metal_sameframe);
	Cvar_RegisterVariable(&rt_metal_lightcores);
	Cvar_RegisterVariable(&rt_metal_lavaemissive);
	Cvar_RegisterVariable(&rt_metal_lavalights);
	Cvar_RegisterVariable(&rt_metal_skyopen);
	Cvar_RegisterVariable(&rt_metal_glowpass);
	Cvar_RegisterVariable(&rt_metal_liquidemissive);
	Cvar_RegisterVariable(&rt_metal_softness);
	Cvar_RegisterVariable(&rt_metal_darkness);
	Cvar_RegisterVariable(&rt_metal_culldist);
	Cvar_RegisterVariable(&rt_metal_history);
	Cvar_RegisterVariable(&rt_metal_bluenoise);
	Cvar_RegisterVariable(&rt_metal_color);
	Cvar_RegisterVariable(&rt_metal_walllight);
	Cvar_RegisterVariable(&rt_metal_ambient);
	Cvar_RegisterVariable(&rt_metal_shafts);
	Cvar_RegisterVariable(&rt_metal_shafts_samples);
	Cvar_RegisterVariable(&rt_metal_shafts_scale);
	Cvar_RegisterVariable(&rt_metal_shafts_intensity);
	Cvar_RegisterVariable(&rt_metal_shafts_history);
	Cvar_RegisterVariable(&rt_metal_shafts_dist);
	Cvar_RegisterVariable(&rt_metal_shafts_residual);
	Cvar_RegisterVariable(&rt_metal_liquids);
	Cvar_RegisterVariable(&rt_metal_liquids_minlight);
	Cvar_RegisterVariable(&rt_metal_fog);
	Cvar_RegisterVariable(&rt_metal_fog_steps);
	Cvar_RegisterVariable(&rt_metal_fog_scale);
	Cvar_RegisterVariable(&rt_metal_fog_intensity);
	Cvar_RegisterVariable(&rt_metal_fog_stride);
	Cvar_RegisterVariable(&rt_metal_fog_history);
	Cvar_RegisterVariable(&rt_metal_fog_residual);
	Cvar_RegisterVariable(&rt_metal_fog_beams);
	Cvar_RegisterVariable(&rt_metal_fog_upsample);
	Cvar_RegisterVariable(&rt_metal_term_upsample);
	Cvar_RegisterVariable(&rt_metal_refit);
	Cvar_RegisterVariable(&rt_metal_fog_stride_adaptive);
	Cvar_RegisterVariable(&rt_metal_term_upsample_depth);
	Cvar_RegisterVariable(&rt_metal_fog_upsample_depth);
	Cvar_RegisterVariable(&rt_metal_fog_stepjitter);
	Cvar_RegisterVariable(&rt_metal_fog_filter);
	Cvar_RegisterVariable(&rt_metal_fog_clamp);
	Cvar_RegisterVariable(&rt_metal_fog_clamp_k);
	Cvar_RegisterVariable(&rt_metal_fog_tonemapema);
	Cvar_RegisterVariable(&rt_metal_fog_reproject_depth);
	Cvar_RegisterVariable(&rt_metal_fog_reproject_tol);
	Cvar_RegisterVariable(&rt_metal_liquids_own);
	Cvar_RegisterVariable(&rt_metal_liquids_rt);
	Cvar_RegisterVariable(&rt_metal_liquids_reflect);
	Cvar_RegisterVariable(&rt_metal_liquids_own_shadows);
	Cvar_RegisterVariable(&rt_metal_sun);
	Cvar_RegisterVariable(&rt_metal_sun_light);
	Cvar_RegisterVariable(&rt_metal_sun_color);
	Cvar_RegisterVariable(&rt_metal_sun_mangle);
	Cvar_RegisterVariable(&rt_metal_sun_penumbra);
	Cvar_RegisterVariable(&rt_metal_as_skipstatic);
	Cvar_RegisterVariable(&rt_metal_pipeline);
	Cvar_RegisterVariable(&rt_metal_fog_froxel);
	Cvar_RegisterVariable(&rt_metal_fog_froxel_slices);
	Cvar_RegisterVariable(&rt_metal_fog_froxel_history);
	Cvar_RegisterVariable(&rt_metal_fog_froxel_curve);
	Cvar_RegisterVariable(&rt_metal_fog_froxel_near);
	Cvar_RegisterVariable(&rt_metal_fog_froxel_castphase);
	Cvar_RegisterVariable(&rt_metal_lightsample_hybrid);
	Cvar_RegisterVariable(&rt_metal_lmax);
	Cvar_RegisterVariable(&rt_metal_lightsample);
	Cvar_RegisterVariable(&rt_metal_lightsample_rays);
	Cvar_RegisterVariable(&rt_metal_lightsample_clamp);
	Cvar_RegisterVariable(&rt_metal_gi);
	Cvar_RegisterVariable(&rt_metal_gi_dist);
	Cvar_RegisterVariable(&rt_metal_gi_albedo);
	Cvar_RegisterVariable(&rt_metal_gi_history);
	Cvar_RegisterVariable(&rt_metal_gi_intensity);
	Cvar_RegisterVariable(&rt_metal_gi_emissive);
	Cvar_RegisterVariable(&rt_metal_gi_rate);
	Cvar_RegisterVariable(&rt_metal_gi_albedo_tex);
	Cvar_RegisterVariable(&rt_metal_gi_fallback);
	Cvar_RegisterVariable(&rt_metal_gi_tiledilate);
	Cvar_RegisterVariable(&rt_metal_gi_ao);
	Cvar_RegisterVariable(&rt_metal_contact);
	Cvar_RegisterVariable(&rt_metal_shadowlights);
	Cvar_RegisterVariable(&rt_metal_shadowlights_rays);
	Cvar_RegisterVariable(&r_skylightning);
	Cvar_RegisterVariable(&r_skylightning_period);
	Cvar_RegisterVariable(&r_skylightning_hold);
	Cvar_RegisterVariable(&r_skylightning_fog);
	Cvar_RegisterVariable(&rt_metal_fog_liquidlight);
	Cvar_RegisterVariable(&rt_metal_gi_ao_dist);
	Cvar_RegisterVariable(&rt_metal_viewmodel);
	Cvar_RegisterVariable(&rt_metal_viewmodel_shadows);
	Cvar_RegisterVariable(&rt_metal_viewmodel_smooth);
	Cvar_RegisterVariable(&m5_fpslog);
	Cmd_AddCommand(CF_CLIENT, "rt_snapshot", RT_Metal_Snapshot_f, "dump the next RT frame to the RT_METAL_DUMP path (verification aid)");
	// METAL.md Phase 7-4: the Metal video layer's own knobs, registered from the
	// same client-only, macOS-only place as the sidecar's above.
	VID_Metal_RegisterCvars();
#endif
#endif
#ifdef DP_MOBILETOUCH
	Cvar_SetValueQuick(&vid_touchscreen, 1);
#endif
	Cvar_RegisterVariable(&joy_sdl2_trigger_deadzone);

	Cvar_RegisterCallback(&vid_display,                VID_ApplyDisplayMode_c);
	Cvar_RegisterCallback(&vid_fullscreen,             VID_ApplyDisplayMode_c);
	Cvar_RegisterCallback(&vid_desktopfullscreen,      VID_ApplyDisplayMode_c);
	Cvar_RegisterCallback(&vid_width,                  VID_ApplyDisplayMode_c);
	Cvar_RegisterCallback(&vid_height,                 VID_ApplyDisplayMode_c);
	Cvar_RegisterCallback(&vid_refreshrate,            VID_ApplyDisplayMode_c);
	Cvar_RegisterCallback(&vid_resizable,              VID_ApplyDisplayMode_c);
	Cvar_RegisterCallback(&vid_borderless,             VID_ApplyDisplayMode_c);
	Cvar_RegisterCallback(&vid_vsync,                  VID_SetVsync_c);
	Cvar_RegisterCallback(&vid_mouse_clickthrough,     VID_SetHints_c);
	Cvar_RegisterCallback(&vid_minimize_on_focus_loss, VID_SetHints_c);

	// DPI scaling prevents use of the native resolution, causing blurry rendering
	// and/or mouse cursor problems and/or incorrect render area, so we need to opt-out.
	// Must be set before first SDL_INIT_VIDEO. Documented in SDL_hints.h.
#ifdef WIN32
	// make SDL coordinates == hardware pixels
	SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "0");
	// use best available awareness mode
	SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif

	if (SDL_Init(SDL_INIT_VIDEO) < 0)
		Sys_Error ("Failed to init SDL video subsystem: %s", SDL_GetError());
	if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) < 0)
		Con_Printf(CON_ERROR "Failed to init SDL joystick subsystem: %s\n", SDL_GetError());

	SDL_GetVersion(&version);
	Con_Printf("Linked against SDL version %d.%d.%d\n"
	           "Using SDL library version %d.%d.%d\n",
	           SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL,
	           version.major, version.minor, version.patch);
}

static int vid_sdljoystickindex = -1;
void VID_EnableJoystick(qbool enable)
{
	int index = joy_enable.integer > 0 ? joy_index.integer : -1;
	int numsdljoysticks;
	qbool success = false;
	int sharedcount = 0;
	int sdlindex = -1;
	sharedcount = VID_Shared_SetJoystick(index);
	if (index >= 0 && index < sharedcount)
		success = true;
	sdlindex = index - sharedcount;

	numsdljoysticks = SDL_NumJoysticks();
	if (sdlindex < 0 || sdlindex >= numsdljoysticks)
		sdlindex = -1;

	// update cvar containing count of XInput joysticks + SDL joysticks
	if (joy_detected.integer != sharedcount + numsdljoysticks)
		Cvar_SetValueQuick(&joy_detected, sharedcount + numsdljoysticks);

	if (vid_sdljoystickindex != sdlindex)
	{
		vid_sdljoystickindex = sdlindex;
		// close SDL joystick if active
		if (vid_sdljoystick)
		{
			SDL_JoystickClose(vid_sdljoystick);
			vid_sdljoystick = NULL;
		}
		if (vid_sdlgamecontroller)
		{
			SDL_GameControllerClose(vid_sdlgamecontroller);
			vid_sdlgamecontroller = NULL;
		}
		if (sdlindex >= 0)
		{
			vid_sdljoystick = SDL_JoystickOpen(sdlindex);
			if (vid_sdljoystick)
			{
				const char *joystickname = SDL_JoystickName(vid_sdljoystick);
				if (SDL_IsGameController(vid_sdljoystickindex))
				{
					vid_sdlgamecontroller = SDL_GameControllerOpen(vid_sdljoystickindex);
					Con_DPrintf("Using SDL GameController mappings for Joystick %i\n", index);
				}
				Con_Printf("Joystick %i opened (SDL_Joystick %i is \"%s\" with %i axes, %i buttons, %i balls)\n", index, sdlindex, joystickname, (int)SDL_JoystickNumAxes(vid_sdljoystick), (int)SDL_JoystickNumButtons(vid_sdljoystick), (int)SDL_JoystickNumBalls(vid_sdljoystick));
			}
			else
			{
				Con_Printf(CON_ERROR "Joystick %i failed (SDL_JoystickOpen(%i) returned: %s)\n", index, sdlindex, SDL_GetError());
				sdlindex = -1;
			}
		}
	}

	if (sdlindex >= 0)
		success = true;

	if (joy_active.integer != (success ? 1 : 0))
		Cvar_SetValueQuick(&joy_active, success ? 1 : 0);
}

#ifdef WIN32
static void AdjustWindowBounds(viddef_mode_t *mode, RECT *rect)
{
	int workWidth;
	int workHeight;
	int titleBarPixels = 2;
	int screenHeight;
	RECT workArea;
	LONG width = mode->width; // vid_width
	LONG height = mode->height; // vid_height

	// adjust width and height for the space occupied by window decorators (title bar, borders)
	rect->top = 0;
	rect->left = 0;
	rect->right = width;
	rect->bottom = height;
	AdjustWindowRectEx(rect, WS_CAPTION|WS_THICKFRAME, false, 0);

	SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);
	workWidth = workArea.right - workArea.left;
	workHeight = workArea.bottom - workArea.top;

	// SDL forces the window height to be <= screen height - 27px (on Win8.1 - probably intended for the title bar) 
	// If the task bar is docked to the the left screen border and we move the window to negative y,
	// there would be some part of the regular desktop visible on the bottom of the screen.
	screenHeight = GetSystemMetrics(SM_CYSCREEN);
	if (screenHeight == workHeight)
		titleBarPixels = -rect->top;

	//Con_Printf("window mode: %dx%d, workArea: %d/%d-%d/%d (%dx%d), title: %d\n", width, height, workArea.left, workArea.top, workArea.right, workArea.bottom, workArea.right - workArea.left, workArea.bottom - workArea.top, titleBarPixels);

	// if height and width matches the physical or previously adjusted screen height and width, adjust it to available desktop area
	if ((width == GetSystemMetrics(SM_CXSCREEN) || width == workWidth) && (height == screenHeight || height == workHeight - titleBarPixels))
	{
		rect->left = workArea.left;
		mode->width = workWidth;
		rect->top = workArea.top + titleBarPixels;
		mode->height = workHeight - titleBarPixels;
	}
	else 
	{
		rect->left = workArea.left + max(0, (workWidth - width) / 2);
		rect->top = workArea.top + max(0, (workHeight - height) / 2);
	}
}
#endif

static qbool VID_InitModeGL(const viddef_mode_t *mode)
{
	int windowflags = SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL;
	int i;
	// SDL usually knows best
	const char *drivername = NULL;

	// video display selection (multi-monitor)
	Cvar_SetValueQuick(&vid_info_displaycount, SDL_GetNumVideoDisplays());
	vid.mode.display = bound(0, mode->display, vid_info_displaycount.integer - 1);
	vid.xPos = SDL_WINDOWPOS_CENTERED_DISPLAY(vid.mode.display);
	vid.yPos = SDL_WINDOWPOS_CENTERED_DISPLAY(vid.mode.display);
	vid_wmborder_waiting = vid_wmborderless = false;

	if(vid_resizable.integer)
		windowflags |= SDL_WINDOW_RESIZABLE;

#ifndef USE_GLES2
// COMMANDLINEOPTION: SDL GL: -gl_driver <drivername> selects a GL driver library, default is whatever SDL recommends, useful only for 3dfxogl.dll/3dfxvgl.dll or fxmesa or similar, if you don't know what this is for, you don't need it
	i = Sys_CheckParm("-gl_driver");
	if (i && i < sys.argc - 1)
		drivername = sys.argv[i + 1];
	if (SDL_GL_LoadLibrary(drivername) < 0)
	{
		Con_Printf(CON_ERROR "Unable to load GL driver \"%s\": %s\n", drivername, SDL_GetError());
		return false;
	}
#endif

#ifdef DP_MOBILETOUCH
	// mobile platforms are always fullscreen, we'll get the resolution after opening the window
	mode->fullscreen = true;
	// hide the menu with SDL_WINDOW_BORDERLESS
	windowflags |= SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS;
#endif

	// SDL_CreateWindow() supports only width and height modesetting,
	// so initially we use desktopfullscreen and perform a modeset later if necessary,
	// this way we do only one modeset to apply the full config.
	if (mode->fullscreen)
	{
		windowflags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
		vid.mode.fullscreen = vid.mode.desktopfullscreen = true;
	}
	else
	{
		if (vid_borderless.integer)
			windowflags |= SDL_WINDOW_BORDERLESS;
		else
			vid_wmborder_waiting = true; // waiting for border to be added
#ifdef WIN32
		if (!vid_ignore_taskbar.integer)
		{
			RECT rect;
			AdjustWindowBounds((viddef_mode_t *)mode, &rect);
			vid.xPos = rect.left;
			vid.xPos = rect.top;
			vid_wmborder_waiting = false;
		}
#endif
		vid.mode.fullscreen = vid.mode.desktopfullscreen = false;
	}

	VID_SetHints_c(NULL);

	SDL_GL_SetAttribute (SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute (SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute (SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute (SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute (SDL_GL_ALPHA_SIZE, 8);
	SDL_GL_SetAttribute (SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute (SDL_GL_STENCIL_SIZE, 8);
	if (mode->stereobuffer)
	{
		SDL_GL_SetAttribute (SDL_GL_STEREO, 1);
		vid.mode.stereobuffer = true;
	}
	if (mode->samples > 1)
	{
		SDL_GL_SetAttribute (SDL_GL_MULTISAMPLEBUFFERS, 1);
		SDL_GL_SetAttribute (SDL_GL_MULTISAMPLESAMPLES, mode->samples);
	}

#ifdef USE_GLES2
	SDL_GL_SetAttribute (SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute (SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute (SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
	SDL_GL_SetAttribute (SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
	/* Requesting a Core profile and 3.2 minimum is mandatory on macOS and older Mesa drivers.
	 * It works fine on other drivers too except NVIDIA, see HACK below.
	 */
#endif

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, (gl_debug.integer > 0 ? SDL_GL_CONTEXT_DEBUG_FLAG : 0));

	window = SDL_CreateWindow(gamename, vid.xPos, vid.yPos, mode->width, mode->height, windowflags);
	if (window == NULL)
	{
		Con_Printf(CON_ERROR "Failed to set video mode to %ix%i: %s\n", mode->width, mode->height, SDL_GetError());
		VID_Shutdown();
		return false;
	}

	context = SDL_GL_CreateContext(window);
	if (context == NULL)
		Sys_Error("Failed to initialize OpenGL context: %s\n", SDL_GetError());

	GL_InitFunctions();

#ifdef MACOSX
	// SPIKE: bring up the in-process Metal RT sidecar against this live GL context.
	RT_Metal_Init();
#endif

#if !defined(USE_GLES2) && !defined(MACOSX)
	// NVIDIA hates the Core profile and limits the version to the minimum we specified.
	// HACK: to detect NVIDIA we first need a context, fortunately replacing it takes a few milliseconds
	gl_vendor = (const char *)qglGetString(GL_VENDOR);
	if (strncmp(gl_vendor, "NVIDIA", 6) == 0)
	{
		Con_DPrint("The Way It's Meant To Be Played: replacing OpenGL Core profile with Compatibility profile...\n");
		SDL_GL_DeleteContext(context);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
		context = SDL_GL_CreateContext(window);
		if (context == NULL)
			Sys_Error("Failed to initialize OpenGL context: %s\n", SDL_GetError());
	}
#endif

	// apply vid_vsync
	Cvar_Callback(&vid_vsync);

	vid_hidden = false;
	vid_activewindow = true;
	vid_hasfocus = true;
	vid_usingmouse = false;
	vid_usinghidecursor = false;

	// clear to black (loading plaque will be seen over this)
	GL_Clear(GL_COLOR_BUFFER_BIT, NULL, 1.0f, 0);
	VID_Finish(); // checks vid_hidden

	GL_Setup();

	// VorteX: set other info
	Cvar_SetQuick(&gl_info_vendor, gl_vendor);
	Cvar_SetQuick(&gl_info_renderer, gl_renderer);
	Cvar_SetQuick(&gl_info_version, gl_version);
	Cvar_SetQuick(&gl_info_driver, drivername ? drivername : "");

	for (i = 0; i < vid_info_displaycount.integer; ++i)
		Con_Printf("Display %i: %s\n", i, SDL_GetDisplayName(i));

	// Perform any hardware modesetting and update vid.mode
	// if modesetting fails desktopfullscreen continues to be used (see above).
	VID_ApplyDisplayMode(mode);

	return true;
}

#ifdef MACOSX
// METAL.md Phase 0: the native Metal window. Mirrors VID_InitModeGL's window
// management with the GL context creation replaced by the CAMetalLayer bring-up
// in vid_metal.m. No GL function pointers are ever loaded on this path -- every
// renderer touchpoint must go through a RENDERPATH_METAL arm.
static qbool VID_InitModeMetal(const viddef_mode_t *mode)
{
	int windowflags = SDL_WINDOW_SHOWN | SDL_WINDOW_METAL;
	int i;

	// video display selection (multi-monitor) -- as VID_InitModeGL
	Cvar_SetValueQuick(&vid_info_displaycount, SDL_GetNumVideoDisplays());
	vid.mode.display = bound(0, mode->display, vid_info_displaycount.integer - 1);
	vid.xPos = SDL_WINDOWPOS_CENTERED_DISPLAY(vid.mode.display);
	vid.yPos = SDL_WINDOWPOS_CENTERED_DISPLAY(vid.mode.display);
	vid_wmborder_waiting = vid_wmborderless = false;

	if (vid_resizable.integer)
		windowflags |= SDL_WINDOW_RESIZABLE;

	if (mode->fullscreen)
	{
		windowflags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
		vid.mode.fullscreen = vid.mode.desktopfullscreen = true;
	}
	else
	{
		if (vid_borderless.integer)
			windowflags |= SDL_WINDOW_BORDERLESS;
		else
			vid_wmborder_waiting = true; // waiting for border to be added
		vid.mode.fullscreen = vid.mode.desktopfullscreen = false;
	}

	VID_SetHints_c(NULL);

	window = SDL_CreateWindow(gamename, vid.xPos, vid.yPos, mode->width, mode->height, windowflags);
	if (window == NULL)
	{
		// NOT VID_Shutdown() here, deliberately (Phase 8, when metal became the
		// default): that ends with SDL_QuitSubSystem(SDL_INIT_VIDEO), and this
		// function's caller falls back to VID_InitModeGL on false -- which
		// would then run against a torn-down video subsystem. Unreachable in
		// practice while nobody defaulted to metal; a plausible first-boot path
		// now. Return false with the subsystem intact, exactly as the
		// VID_Metal_Init failure below does, and let the GL path have its turn.
		Con_Printf(CON_ERROR "Failed to set video mode to %ix%i: %s\n", mode->width, mode->height, SDL_GetError());
		return false;
	}

	if (!VID_Metal_Init(window, vid_vsync.integer != 0))
	{
		// no Metal device / view: tear the window down and let VID_InitMode
		// fall back to the GL path
		SDL_DestroyWindow(window);
		window = NULL;
		return false;
	}

	vid.renderpath = RENDERPATH_METAL;

	// METAL.md Phase 5: bring the RT sidecar up on the RENDERER'S device. Until
	// now VID_InitModeMetal never started the sidecar at all -- RT_Metal_Init is
	// called only from VID_InitModeGL (see above) -- which is why rt_metal did
	// nothing on the Metal path for Phases 0-4d.
	//
	// The DEVICE is shared and that is the whole point: two MTLDevice objects
	// for one GPU cannot share resources, which is the only reason the
	// IOSurface/CGL bridge exists. The QUEUE is deliberately NOT shared (NULL
	// here means "make your own"). Decided with Seb, and it is a departure from
	// METAL.md's wording, which says device AND queue: the backend commits one
	// command buffer per frame at end-of-frame, so putting the trace on that
	// queue risks serialising it behind an open render command buffer and
	// erasing exactly the CPU/GPU overlap the 2-slot async design exists to
	// buy. That regression would be invisible to every pixel gate in this
	// phase -- same frame, slower. Revisit only with RT_METAL_KERNELMS=1
	// per-stage numbers showing it is free.
	RT_Metal_InitWithDevice(VID_Metal_GetDevice(), NULL);

	// capabilities normally filled in by GL_Setup(). vid.support stays zeroed:
	// every vid.support.* consumer is a GL extension test and must read false
	// here (occlusion queries, s3tc, ...). Sizes are the M5's Metal limits.
	vid.maxtexturesize_2d = 16384;
	vid.maxtexturesize_3d = 2048;
	vid.maxtexturesize_cubemap = 16384;
	vid.max_anisotropy = 16;
	vid.maxdrawbuffers = 4; // engine maximum; Metal supports 8
	vid.allowalphatocoverage = false;
	// MTLBlendOperationMax is core Metal, no extension and no query. The GL path
	// leaves this false: GL_MAX is core there too, but nothing on that side asks
	// for it, and a capability nobody has exercised is not one to advertise.
	vid.blendequationmax = true;
	vid.m5postfx = true;   // the MSL carries the shimmer body; see vid.h
	vid.sRGBcapable2D = false;
	vid.sRGBcapable3D = false;
	vid.forcetextype = 0;

	Cvar_SetQuick(&gl_info_vendor, "Apple");
	Cvar_SetQuick(&gl_info_renderer, "Metal");
	Cvar_SetQuick(&gl_info_version, "");
	Cvar_SetQuick(&gl_info_driver, "vid_metal");

	vid_hidden = false;
	vid_activewindow = true;
	vid_hasfocus = true;
	vid_usingmouse = false;
	vid_usinghidecursor = false;

	// first present (the Phase 0 marker clear; the loading plaque cannot draw yet)
	VID_Metal_Finish();

	for (i = 0; i < vid_info_displaycount.integer; ++i)
		Con_Printf("Display %i: %s\n", i, SDL_GetDisplayName(i));

	// Perform any hardware modesetting and update vid.mode
	VID_ApplyDisplayMode(mode);

	return true;
}
#endif

qbool VID_InitMode(const viddef_mode_t *mode)
{
	// GAME_STEELSTORM specific
	steelstorm_showing_map = Cvar_FindVar(&cvars_all, "steelstorm_showing_map", ~0);
	steelstorm_showing_mousecursor = Cvar_FindVar(&cvars_all, "steelstorm_showing_mousecursor", ~0);

	if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_VIDEO) < 0)
		Sys_Error ("Failed to init SDL video subsystem: %s", SDL_GetError());

	Cvar_SetValueQuick(&vid_touchscreen_supportshowkeyboard, SDL_HasScreenKeyboardSupport() ? 1 : 0);

	// METAL.md: backend selection, read exactly once per video restart
	if (!strcasecmp(vid_renderer.string, "metal"))
	{
#ifdef MACOSX
		if (VID_InitModeMetal(mode))
			return true;
		Con_Printf(CON_WARN "vid_renderer metal: Metal video failed, falling back to GL\n");
#else
		Con_Printf(CON_WARN "vid_renderer metal is macOS-only, using GL\n");
#endif
	}
	else if (strcasecmp(vid_renderer.string, "gl"))
		Con_Printf(CON_WARN "vid_renderer \"%s\" unknown (gl or metal), using GL\n", vid_renderer.string);
	return VID_InitModeGL(mode);
}

void VID_Shutdown (void)
{
	VID_EnableJoystick(false);
	VID_SetMouse(false, false);

#ifdef MACOSX
	if (vid.renderpath == RENDERPATH_METAL)
	{
		// The sidecar DOES exist on this path since Phase 5 slice 1, and it holds
		// the renderer's own MTLDevice -- so it has to come down here too, and
		// FIRST. Two reasons, and the second is the one with teeth. It publishes
		// its term buffer into the renderer's texture table (slice 3), so those
		// handles must be released while that table is still alive; and
		// RT_Metal_InitWithDevice early-outs on `if (s_dev) return`, so a sidecar
		// left standing across a vid_restart would keep serving the OLD device --
		// which was harmless while the composite refused, and is a cross-device
		// resource the moment its textures are bound into renderer encoders.
		// This also un-breaks metal -> gl: the sidecar now re-initialises for real
		// on the next VID_InitModeGL instead of silently staying dead.
		// No GL context exists here, and RT_Metal_Shutdown handles that: s_glctx
		// is never set on the shared path, so it takes the rt_abandon_gl() branch,
		// where every GL delete is guarded by a name that was never created.
		RT_Metal_Shutdown();
		VID_Metal_Shutdown();
	}
	else
	{
		// SPIKE: release the Metal sidecar while its GL context is still current.
		RT_Metal_Shutdown();
	}
#endif
	if (context)
	{
		SDL_GL_DeleteContext(context);
		context = NULL;
	}
	SDL_DestroyWindow(window);
	window = NULL;

	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void VID_Finish (void)
{
	VID_UpdateGamma();

	if (!vid_hidden)
	{
		switch(vid.renderpath)
		{
#ifdef MACOSX
		case RENDERPATH_METAL:
			// METAL.md Phase 8-1a: the dump instrument's Metal arm, BEFORE the
			// present — the readback inside commits and waits the frame's command
			// buffer, so mb_screentex holds the completed frame (scene + HUD), and
			// VID_Metal_Finish's EndFrame opens a fresh buffer for the present
			// pass, which it already tolerates. Same counting contract as the GL
			// arm below; sized from vid.mode.*, which is what sized the screen
			// texture (and is immune to the WM window clamp that bites the GL
			// arm's SDL_GL_GetDrawableSize). Dumps clamp an EDR frame to 8-bit —
			// identically for both ends of an A/B — and say so once, here, where
			// vid.edr_active is in scope.
			{
				static int dumparmed = -1;
				if (dumparmed < 0)
					dumparmed = getenv("RT_METAL_DUMP") != NULL;
				if (dumparmed && vid.edr_active)
				{
					static int edrnoted;
					if (!edrnoted)
					{
						edrnoted = 1;
						fprintf(stderr, "RT_Metal: dump note: EDR is active; dumps clamp to 8-bit\n");
					}
				}
				RT_Metal_DumpFrameMetal(vid.mode.width, vid.mode.height, cls.timedemo ? 1 : 0);
			}
			// METAL.md Phase 0: clear-to-marker + present lives in vid_metal.m
			VID_Metal_Finish();
			break;
#else
		case RENDERPATH_METAL:
			break;
#endif
		case RENDERPATH_GL32:
		case RENDERPATH_GLES2:
			CHECKGLERROR
			if (r_speeds.integer == 2 || gl_finish.integer)
				GL_Finish();
#ifdef MACOSX
			// SPIKE: the RT trace + composite happens mid-frame (SCR_DrawScreen,
			// after the 3D view, before the HUD). Here we only capture the final
			// frame for verification when RT_METAL_DUMP is set. Not gated on
			// rt_metal: RT-off configurations are legitimate A/B ends too. Only
			// timedemo frames advance the dump counter (run-stable numbering).
			{
				int rtw = 0, rth = 0;
				SDL_GL_GetDrawableSize(window, &rtw, &rth);
				RT_Metal_DumpFrame(rtw, rth, cls.timedemo ? 1 : 0);
			}
#endif
			// METAL.md Phase 1: close the command digest for this frame, right
			// beside the byte gate's own dump so the two instruments see the
			// same moment. Compiles to nothing without -DDP_CMDTRACE.
			//
			// Emitted on EVERY presented frame, not only timedemo frames. That
			// differs from RT_Metal_DumpFrame deliberately, and the measurement
			// is why: a demo's call stream is NOT reproducible run to run (two
			// boots of one binary agreed on 0 of 6124 frames), because the
			// demo's own simulation diverges and drags visibility, draw counts
			// and GL object numbering with it. The bed that does reproduce is a
			// FROZEN scene, where the stream should be constant frame to frame --
			// which makes "the tail frames all agree" a self-check on the
			// instrument, before any cross-run claim is made.
			{
				static int dpcmd_framenum;
				DPCMD_FrameEnd(++dpcmd_framenum);
			}
			SDL_GL_SwapWindow(window);
			break;
		}
	}

	// presented-frame throughput (see m5_fpslog). Measured across the swap, so it
	// counts exactly the frames the user actually sees.
	if (m5_fpslog.integer > 0)
	{
		static double t0;
		static int n, armed;
		double now = Sys_DirtyTime();
		if (!armed) { armed = 1; t0 = now; n = 0; }
		if (++n >= m5_fpslog.integer)
		{
			double dt = now - t0;
			if (dt > 0)
				fprintf(stderr, "M5_FPS: %d frames in %.3f s = %.1f fps (%.3f ms/frame)\n",
						n, dt, n / dt, dt * 1000.0 / n);
			t0 = now;
			n = 0;
		}
	}
}

vid_mode_t VID_GetDesktopMode(void)
{
	SDL_DisplayMode mode;
	int bpp;
	Uint32 rmask, gmask, bmask, amask;
	vid_mode_t desktop_mode;

	SDL_GetDesktopDisplayMode(vid.mode.display, &mode);
	SDL_PixelFormatEnumToMasks(mode.format, &bpp, &rmask, &gmask, &bmask, &amask);
	desktop_mode.width = mode.w;
	desktop_mode.height = mode.h;
	desktop_mode.bpp = bpp;
	desktop_mode.refreshrate = mode.refresh_rate;
	desktop_mode.pixelheight_num = 1;
	desktop_mode.pixelheight_denom = 1; // SDL does not provide this
	return desktop_mode;
}

size_t VID_ListModes(vid_mode_t *modes, size_t maxcount)
{
	size_t k = 0;
	int modenum;
	int nummodes = SDL_GetNumDisplayModes(vid.mode.display);
	SDL_DisplayMode mode;
	for (modenum = 0;modenum < nummodes;modenum++)
	{
		if (k >= maxcount)
			break;
		if (SDL_GetDisplayMode(vid.mode.display, modenum, &mode))
			continue;
		modes[k].width = mode.w;
		modes[k].height = mode.h;
		modes[k].bpp = SDL_BITSPERPIXEL(mode.format);
		modes[k].refreshrate = mode.refresh_rate;
		modes[k].pixelheight_num = 1;
		modes[k].pixelheight_denom = 1; // SDL does not provide this
		Con_DPrintf("Display %i mode %i: %ix%i %ibpp %ihz\n", vid.mode.display, modenum, modes[k].width, modes[k].height, modes[k].bpp, modes[k].refreshrate);
		k++;
	}
	return k;
}
