/*
	rt_metal.m — in-process Metal RT sidecar (macOS only)

	GPU ray tracing of the DarkPlaces world, composited into the engine's live GL
	frame through shared IOSurfaces. Three compute kernels:
	  - rt_trace : per-pixel primary ray + tiled light cull + disc-jittered soft
	    shadow of the dominant light, temporally accumulated (world-reprojected
	    history). Entity hits can shade with interpolated vertex normals.
	  - rt_fog   : the WHOLE volumetric fog integral (density model in lockstep
	    with the GL murk in shader_glsl.h) with per-step lighting and god rays.
	  - rt_shaft : the older screen-space god rays; superseded while rt_fog runs,
	    kept as the cheaper tier for fog-kernel-off configurations.

	ASYNC pipeline: each frame encodes AS builds + kernels into one command buffer
	for slot s_par and commits WITHOUT waiting; the composite draws the OTHER
	slot's finished frame (RT_METAL_SYNC=1 restores same-frame order for A/Bs) and
	reprojects it through the camera it was traced with (rt_metal_reproject), so
	the one-frame latency never reads as lag. RT_Metal_Composite runs mid-frame
	from R_RenderView; every GL state touched is saved and restored so the
	engine's gl_backend cache stays coherent. See metal/async-plan.md and the
	measurement aids at the dump/KERNELMS mentions in CLAUDE.md.
*/

#ifdef __APPLE__

#define GL_SILENCE_DEPRECATION   // OpenGL/CGL are deprecated on macOS but fully functional (the engine relies on them)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <OpenGL/gl3.h>
#import <OpenGL/OpenGL.h>        // CGL
#import <OpenGL/CGLIOSurface.h>  // CGLTexImageIOSurface2D
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>   // clock_gettime_nsec_np — CPU wall timing for the profile report

#include "rt_metal.h"
#include "shader_density.h"   // METAL.md Phase 2: fog constants shared with the GL murk march
#include "rt_bluenoise.h"     // the 64x64x8 blue-noise jitter table (the weave fix; regenerate ONLY via test/bluenoise-gen.py)
#include "metal_textures.h"   // METAL.md Phase 5: publishing the term buffer to the renderer
#include "metal_backend.h"    // METAL.md Phase 8-1a: the dump's Metal arm reads fbo 0 by name
#include "r_textures.h"       // TEXF_* -- the sampler choice for the published term. Pulls only
                              // qtypes.h/qdefs.h, so this file stays free of the engine proper.

// THE SPOT CONE, spliced into ALL THREE kernels (F6, the handlamp). Defined once
// here and injected by the three newLibraryWithSource calls, exactly as
// DPD_SHADER_PRELUDE is -- the alternative was a fourth hand-kept lockstep copy,
// and this file already carries one of those in the fog density model.
//
// `sd` is the light's unit throw direction and `ldir` the unit direction FROM the
// light TO the point being lit; both are the light's own slots 8..11.
//
// A ZERO DIRECTION MEANS OMNI AND RETURNS EXACTLY 1.0f. That is the no-op proof
// for this commit and it is the same shape the fog weight uses: every producer
// but the handlamp writes zeros (R_RTLight_Update memsets the rtlight), so every
// other light multiplies by an exact 1.0f and the frame is bit-identical to the
// pre-cone build. Verified as such, not assumed.
//
// The inner edge sits 35% of the way from the outer cosine to the axis, so the
// beam has a soft shoulder rather than a stencilled rim -- a hard edge reads as
// a projected circle, which is what a torch must not look like.
// SELF-CONTAINED ON PURPOSE: this is spliced AHEAD of each kernel source, which
// is where its own #include sits, so it must bring its own. Duplicate includes
// are idempotent and a repeated `using namespace` is legal; without them the
// helper sees no dot/mix/smoothstep and ALL THREE kernels fail to compile and
// silently fall back, which is exactly what the first cut of this commit did.
static const char *kConeSrc =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"static inline float rt_cone(float3 sd, float scos, float sinner, float fil, float3 ldir) {\n"
"    if (dot(sd, sd) < 1e-8f) return 1.0f;\n"
"    // The shoulder runs from the outer cosine to sinner. A caller that never\n"
"    // set it -- which is the memset zero every producer but the handlamp\n"
"    // leaves -- gets the historic fixed 35% of the way to the axis, so this is\n"
"    // bit-identical to the pre-2026-09-01 kernel for them. That fixed 35% left\n"
"    // the inner 65% of the cone a FLAT PLATEAU at full brightness, which is\n"
"    // what read on a wall as a stencilled disc rather than a beam.\n"
"    float si = (sinner > scos) ? sinner : mix(scos, 1.0f, 0.35f);\n"
"    float d = dot(ldir, sd);\n"
"    float cone = smoothstep(scos, si, d);\n"
"    // FILAMENT (m5_torch_filament, slot 13, 2026-09-06): a cheap reflector\n"
"    // images its bulb as a hot spot about a third of the beam across, with\n"
"    // a dim ring where the reflector's dead zone falls. The core is a second\n"
"    // smoothstep from the core edge to the axis, added as up to +1x (twice\n"
"    // the skirt at fil 1); the ring is a Gaussian dip just outside the core\n"
"    // edge, 30% deep at fil 1. Both widths scale with the cone so a spot and\n"
"    // a lantern keep the same proportions. At fil 0 both factors are EXACTLY\n"
"    // 1.0f and the product is the bare cone bit for bit -- the off switch.\n"
"    float ccos = mix(scos, 1.0f, 0.88f);\n"
"    float core = smoothstep(ccos, mix(ccos, 1.0f, 0.7f), d);\n"
"    float rc = ccos - 0.2f * (1.0f - scos);\n"
"    float rw = 0.06f * (1.0f - scos) + 1e-6f;\n"
"    float ring = exp(-((d - rc) * (d - rc)) / (rw * rw));\n"
"    return cone * (1.0f + fil * core) * (1.0f - 0.3f * fil * ring);\n"
"}\n";

// Blue-noise jitter helper (rt_metal_bluenoise, the weave fix). Spliced AFTER
// kConeSrc (which brings metal_stdlib) and only into kernels compiled with
// RT_BLUENOISE 1, so the RT_BLUENOISE 0 sources stay byte-identical to the
// classic IGN build. The table is 8 independent 64x64 void-and-cluster slices;
// cycling WHOLE slices per frame (frame & 7) keeps each frame's spatial
// spectrum perfectly blue -- a golden-ratio scalar add (the IGN scheme) would
// split a blue-noise field at the fract() wrap into a half-blue/half-white
// spectrum, re-importing exactly the broadband energy this removes. The
// history gate mirrors the IGN arm: history off = slice 0 forever, so stills
// stay static and shimmer-free. +0.5/256 centres the 8-bit rank in its bin.
static const char *kJitterSrc =
"#if RT_BLUENOISE\n"
"static inline float rt_bnjitter(uint2 gid, uint frame, float history, device const uchar *bn) {\n"
"    uint slice = (history > 0.0f) ? (frame & (RT_BN_SLICES - 1u)) : 0u;\n"
"    return (float(bn[slice * 4096u + ((gid.y & 63u) * 64u) + (gid.x & 63u)]) + 0.5f) * (1.0f / 256.0f);\n"
"}\n"
"#endif\n"
// A SECOND, DECORRELATED stream, for the stochastic light PICK
// (rt_metal_lightsample). It must not share the jitter's value: the pick and the
// shadow disc's rotation would then move together and beat against each other in
// the accumulation. Blue-noise arm: a different slice (half the cycle away) and a
// shifted tap, so the pick is still blue over the screen -- and, since the
// table became spatiotemporal (2026-09-03), blue in TIME as well; the coupling
// with the jitter tap is measured by test/bluenoise-check.py (r = +0.007).
// RT_BN_SLICES is the table's slice count, injected with the other preamble
// defines: 8 or 16, the kernels never assume which. Non-blue arm: a PCG integer hash
// (2026-08-28) -- the original IGN arm here was spatially CORRELATED
// (neighbouring pixels picked the SAME light: mean neighbour delta 0.014-0.029
// against 0.333 for i.i.d., test/pickstream.py mirrors this function and is the
// probe), so the estimator's error arrived as moving BLOTCHES rather than fine
// grain, which is the shape Seb's eye rejected on 2026-08-17. The hash reads
// 0.334 on x, y and adjacent k. Same history gate as ever -- history 0 means
// frame-invariant, which is what keeps the frozen parity bed deterministic.
// k decorrelates per march step / per cast (0 in the surface kernel).
// LOCKSTEP: test/pickstream.py replicates rt_pickhash + this arm verbatim.
// RT_SELSEQ is the MARCH kernels' variant (fog and shaft): the same hashed
// base, but the CAST index becomes a golden-ratio drift instead of entering
// the hash, so one pixel's casts sweep the light CDF quasi-evenly rather than
// drawing independently. A pixel's fog is the SUM over its ~8 casts, and
// stratifying that sum is where the variance lives: simulated at history 0.7
// with a 60/25/10/5 light split, the EMA's std HALVES (0.068 -> 0.034,
// 2026-08-28 — the "teeny bit of speckle" verdict's fix). The surface kernel
// keeps fully-hashed RT_SELRAND for both its calls, because its k=1 call is
// the secondary disc ROTATION and a drift would correlate it with the pick —
// the exact coupling this comment block has always warned about.
"#if RT_BLUENOISE\n"
"#define RT_SELRAND(g, f, k, h) rt_selrand((g), (f), (k), (h), bn)\n"
"#define RT_SELSEQ(g, f, k, h) rt_selseq((g), (f), (k), (h), bn)\n"
"static inline float rt_selrand(uint2 gid, uint frame, uint k, float history, device const uchar *bn) {\n"
"    uint slice = (history > 0.0f) ? ((frame + (RT_BN_SLICES / 2u) + k) & (RT_BN_SLICES - 1u)) : (k & (RT_BN_SLICES - 1u));\n"
"    uint x = (gid.x + 23u * k + 11u) & 63u;\n"
"    uint y = (gid.y + 17u * k + 29u) & 63u;\n"
"    return (float(bn[slice * 4096u + y * 64u + x]) + 0.5f) * (1.0f / 256.0f);\n"
"}\n"
"static inline float rt_selseq(uint2 gid, uint frame, uint k, float history, device const uchar *bn) {\n"
"    return fract(rt_selrand(gid, frame, 0u, history, bn) + float(k) * 0.6180339887f);\n"
"}\n"
"#else\n"
"#define RT_SELRAND(g, f, k, h) rt_selrand((g), (f), (k), (h))\n"
"#define RT_SELSEQ(g, f, k, h) rt_selseq((g), (f), (k), (h))\n"
"static inline uint rt_pickhash(uint v) {\n"
"    uint s = v * 747796405u + 2891336453u;\n"
"    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;\n"
"    return (w >> 22u) ^ w;\n"
"}\n"
"static inline float rt_selrand(uint2 gid, uint frame, uint k, float history) {\n"
"    uint f = (history > 0.0f) ? frame : 0u;\n"
"    uint h = rt_pickhash(gid.x + rt_pickhash(gid.y + rt_pickhash(k + rt_pickhash(f))));\n"
"    return (float(h >> 8u) + 0.5f) * (1.0f / 16777216.0f);\n"
"}\n"
"static inline float rt_selseq(uint2 gid, uint frame, uint k, float history) {\n"
"    return fract(rt_selrand(gid, frame, 0u, history) + float(k) * 0.6180339887f);\n"
"}\n"
"#endif\n";

// Ray tracer with soft shadows from the map's REAL lights. One primary ray per
// pixel finds the world hit (position + face normal). Each pixel then picks the
// most significant real light reaching it (from the lights buffer) and casts N
// soft shadow rays to equal-area Vogel-spiral samples across that light's disc,
// averaged into a soft visibility fraction. Per-pixel spiral rotation uses the
// blue-noise table (rt_metal_bluenoise; the IGN dither under RT_BLUENOISE 0 —
// which is NOT blue-noise-like, its lattice is the documented weave). The
// output is a grey multiplier that darkens the scene only where the dominant
// real light is occluded (added on top of the engine's lightmapped render).
// A frame counter is plumbed through the ABI for later temporal jitter.
// The Cam struct layout is tight (packed_float3 = 12 bytes) to match the C side.
static const char *kTraceSrc =
"#include <metal_stdlib>\n"
"#include <metal_raytracing>\n"
"using namespace metal;\n"
"using namespace raytracing;\n"
"struct Cam {\n"
"    packed_float3 origin;\n"
"    packed_float3 forward;\n"
"    packed_float3 right;\n"
"    packed_float3 up;\n"
"    float tanx;\n"
"    float tany;\n"
"    uint w;\n"
"    uint h;\n"
"    uint frame;\n"
"    uint numLights;\n"
"    uint samples;\n"      // rt_metal_samples: shadow rays per pixel
"    float softness;\n"    // rt_metal_softness: area radius = clamp(lightradius*softness, 8, 40)
"    float darkness;\n"    // rt_metal_darkness: scene multiplier where fully shadowed
"    packed_float3 pOrigin;\n"   // previous-frame camera (for temporal reprojection)
"    packed_float3 pForward;\n"
"    packed_float3 pRight;\n"
"    packed_float3 pUp;\n"
"    float pTanx;\n"
"    float pTany;\n"
"    float history;\n"    // rt_metal_history: temporal EMA weight (0 = temporal off)
"    uint hasPrev;\n"     // 1 if the previous-frame history is valid (not first frame / after resize)
"    uint numDynamic;\n"  // lights[0..numDynamic) are DYNAMIC (prepended): only these get colored brighten
"    float colorstr;\n"   // rt_metal_color: strength of the dynamic-light colored brighten (0 = grey/off)
"    float walllight;\n"  // rt_metal_walllight: >0 = full RT lighting mode (scene is fullbright albedo), this = intensity
"    float ambient;\n"    // rt_metal_ambient: ambient fill for wall-lighting mode
"    uint entnorms;\n"    // 1 = enorms holds per-vertex entity normals (rt_metal_smoothnormals)
"    float lmax;\n"       // rt_metal_lmax: wall-lighting term soft ceiling knee (0 = no ceiling, the old bytes)
"    uint lsample;\n"     // rt_metal_lightsample: 1 = stochastic SECONDARY light (every light shadowed in expectation)
"    uint lsrays;\n"      // rt_metal_lightsample_rays: shadow rays spent on that secondary
"    uint gi;\n"          // rt_metal_gi: one-bounce diffuse GI (GIARC G1; walllight arm only)
"    float gidist;\n"     // rt_metal_gi_dist: bounce ray max length, wu
"    float gialbedo;\n"   // rt_metal_gi_albedo: constant bounce reflectance (the BLAS is geometry-only)
"    float gihistory;\n"  // rt_metal_gi_history: GI colour EMA weight (stillness-floored C-side)
"    float giintensity;\n"// rt_metal_gi_intensity: gain into the term, inside the walllight*6 scale
"    float giemissive;\n" // rt_metal_gi_emissive: emissive-instance bounce gain (0 = mask 0x3u, the G1 bytes; appended G3 2026-08-29)
"    uint girate;\n"      // rt_metal_gi_rate: bounce fires on a 1-in-N frame-rotating pixel subset (1 = every pixel, the old bytes; appended G4-1 2026-08-29)
"    float gialbtex;\n"   // rt_metal_gi_albedo_tex: 0 = the constant gialbedo (the old bytes), 1 = the bounce surface's own mean colour (appended G4-2 2026-08-29)
"    uint gbias;\n"       // RT_METAL_GIBIAS: MEASURE-ONLY (GIARC G4). 0 = the shipped kernel, every branch untaken; 1 = paint the tile-list bias; 2 = paint the bounce outcome
"    uint gifallback;\n"   // rt_metal_gi_fallback: when NO tile light reaches a bounce point, pick over ALL staged lights instead of contributing nothing (GIARC G4-3; 0 = the old bytes)
"    float gitiledilate;\n"// rt_metal_gi_tiledilate: dilate the tile cull by this FRACTION of gidist for the BOUNCE pick only, so a light that reaches the bounce point but not the tile is no longer culled (GIARC G4-4; 0 = the old bytes)
"    packed_float3 sundir;\n" // SEPTEMBER2 D (2026-09-06): unit vector TOWARD the sun (from the map's _sunlight_mangle or rt_metal_sun_mangle)
"    packed_float3 suncol;\n" // the sun's colour in Lsum units (_sunlight/256 x _sunlight_color x rt_metal_sun)
"    float sunpen;\n"         // tan(penumbra): the disc the sun ray is jittered inside
"    uint sun;\n"             // 0 = no sun ray, the old bytes
"    uint liqrt;\n"           // SEPTEMBER2 C2 (2026-09-09): 1 = intersect the blended-liquid instance, write the liquid pair (0 = the old bytes)
"    float liqreflect;\n"     // rt_metal_liquids_reflect: gain on the Fresnel-weighted reflection (0 = own term only)
"    float giao;\n"           // BEAUTY B1 (2026-09-16): ambient occlusion from the bounce ray, applied to the FLAT AMBIENT FILL only (0 = the old bytes)
"    float giaodist;\n"       // rt_metal_gi_ao_dist: a bounce hit nearer than this occludes, smoothly to 0 at contact
"    float contact;\n"        // BEAUTY B3 (2026-09-17): contact-hardened shadows -- the dominant light's penumbra disc scaled by the blocker's distance (0 = the old bytes)
"    uint shadowlights;\n"    // rt_metal_shadowlights (2026-09-19): how many of a pixel's brightest lights get a shadow test (1 = the dominant alone, the old bytes)
"    uint shadowlightrays;\n" // rt_metal_shadowlights_rays: rays spent on each runner-up
"};\n"
"kernel void rt_trace(texture2d<float, access::write> outtex [[texture(0)]],\n"
"                     texture2d<float, access::read> histIn [[texture(1)]],\n"    // prev-frame history (world pos + vis)
"                     texture2d<float, access::write> histOut [[texture(2)]],\n"  // this-frame history
"                     texture2d<float, access::read> secIn [[texture(3)]],\n"     // prev-frame secondary estimate
"                     texture2d<float, access::write> secOut [[texture(4)]],\n"   // this-frame secondary estimate
"                     texture2d<float, access::read> giIn [[texture(5)]],\n"      // prev-frame GI colour EMA (rt_metal_gi)
"                     texture2d<float, access::write> giOut [[texture(6)]],\n"    // this-frame GI colour EMA
"                     texture2d<float, access::write> liqOut [[texture(7)]],\n"   // SEPTEMBER2 C2: the liquid pair, DOUBLE width (left = own term, right = reflection); a bound stand-in when the arm is off (never written then)
"                     instance_acceleration_structure accel [[buffer(0)]],\n"     // TLAS: world + entities + light-cores
"                     device const packed_float3 *verts [[buffer(1)]],\n"         // world verts
"                     device const uint *idx [[buffer(2)]],\n"                    // world indices
"                     constant Cam &cam [[buffer(3)]],\n"
"                     device const float *lights [[buffer(4)]],\n"
"                     device const packed_float3 *everts [[buffer(5)]],\n"        // entity verts (world space)
"                     device const uint *eidx [[buffer(6)]],\n"                   // entity indices
"                     device const packed_float3 *enorms [[buffer(7)]],\n"        // entity vertex normals (world space; may be zeros)
"#if RT_BLUENOISE\n"
"                     device const uchar *bn [[buffer(8)]],\n"
"#endif\n"
// walb sits OUTSIDE the RT_BLUENOISE conditional above, and the first cut of
// G4-2 put it inside by accident: at rt_metal_bluenoise 0 the declaration
// vanished while the coloured-bounce use remained, and the IGN-arm RECOMPILE
// failed (the first compile runs at the default bluenoise 1, so nothing
// visible broke -- the failed recompile kept the previous PSO). Smoke's
// bluenoise-toggle absent-check is what caught it.
"                     device const uchar4 *walb [[buffer(9)]],\n"   // per-world-triangle RGBA8 mean colour (rt_metal_gi_albedo_tex; grey degenerate when unsupplied)
"                     device const uint *lqidx [[buffer(10)]],\n"    // SEPTEMBER2 C2: the BLENDED liquid instance's indices into the world verts (degenerate when none); always bound
"                     uint2 gid [[thread_position_in_grid]],\n"
"                     uint tid [[thread_index_in_threadgroup]],\n"
"                     uint2 tgs [[threads_per_threadgroup]],\n"
"                     uint sgi [[simdgroup_index_in_threadgroup]],\n"
"                     uint lane [[thread_index_in_simdgroup]])\n"
"{\n"
"    // cache the lights in fast threadgroup memory (loaded cooperatively once\n"
"    // per tile) so the light loops don't hammer device memory\n"
"    // TILED LIGHT CULL: after the primary rays land, the tile computes the\n"
"    // AABB of its hit points and culls the light list against it ONCE, so the\n"
"    // per-pixel loop only sees lights whose sphere can reach this tile (the\n"
"    // measured hot spot was every pixel iterating every light every frame).\n"
"    // Survivors are compacted IN ORIGINAL ORDER via the bitmask, so per-pixel\n"
"    // results (dominant-light choice, Lsum accumulation order) are bit-exact\n"
"    // vs the untiled loop: any light passing the per-pixel range test has its\n"
"    // hit point inside its sphere, hence intersects the tile AABB.\n"
"    threadgroup float tgl[256 * RTL];\n"
"    threadgroup atomic_uint tflags[8];\n"     // 256-bit survivor mask
"    threadgroup uint tlist[256];\n"           // ordered surviving light indices
"    threadgroup uint tcount;\n"
// GIARC G4-4: the DILATED tile list, for the GI bounce pick only. The cull above
// bounds the tile's PRIMARY HIT POINTS, but a bounce point lies up to gidist away
// from its hit, so a light that reaches the bounce and not the tile is culled and
// the sample contributes EXACTLY ZERO -- measured at 22.8% of 385092 bounce
// samples over nine frames and four demos (RT_METAL_GIBIAS=1, test/gibias.py).
// Dilating by the FULL gidist bounds every possible bounce point, so at fraction
// 1 the candidate set is conservative again and the bias is 0 by construction;
// below that it is a trade, measured 10.1 / 4.1 / 0.3% at 0.125 / 0.25 / 0.5.
//
// It is a SECOND list and not a widening of the first, deliberately. The surface
// loop is the trace stage's per-pixel hot spot and the cull exists to keep it
// short; handing it a longer list would tax every pixel on every tier, including
// the three that ship with GI off. Two lists cost 292 bytes of threadgroup memory
// (uchar indices -- nl is capped at 256) and leave the surface byte-exact BY
// CONSTRUCTION rather than by an argument about conservativeness.
"    threadgroup atomic_uint gflags[8];\n"    // 256-bit dilated survivor mask
"    threadgroup uchar gtlist[256];\n"        // dilated survivors, same in-order compaction
"    threadgroup uint gtcount;\n"
"    float gdil = cam.gidist * cam.gitiledilate;\n"
"    bool usedil = (gdil > 0.0f) && (cam.gi != 0u) && (cam.walllight > 0.0f);\n"  // ...and only where the bounce can actually fire: these are
                                                     // the two UNIFORM terms gating the only consumer (gifire is
                                                     // per-pixel and must NOT join them). Nothing else reads the
                                                     // dilated list, so the three tiers shipping GI off pay nothing.
                                                     // Measured before this term existed: gi 0 + dilate 1 cost +0.03 ms
                                                     // over gi 0 alone, i.e. the clear, the second sphere test and the
                                                     // second compaction all ran for a list no one consumed.
"    threadgroup float3 rmin[8], rmax[8];\n"   // per-simdgroup AABB partials
"    uint nl = min(cam.numLights, 256u);\n"
"    uint tgcount = tgs.x * tgs.y;\n"
"    for (uint k = tid; k < nl * RTL; k += tgcount) tgl[k] = lights[k];\n"
"    for (uint k = tid; k < 8u; k += tgcount) atomic_store_explicit(&tflags[k], 0u, memory_order_relaxed);\n"
"    if (usedil) for (uint k = tid; k < 8u; k += tgcount) atomic_store_explicit(&gflags[k], 0u, memory_order_relaxed);\n"
"    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"    // NOTE: no early return before the barriers below — every launched thread\n"
"    // participates (dispatchThreads never launches out-of-bounds threads).\n"
"    bool inbounds = (gid.x < cam.w && gid.y < cam.h);\n"
"    float sx = (2.0f * (float(gid.x) + 0.5f) / float(cam.w) - 1.0f) * cam.tanx;\n"
"    float sy = (2.0f * (float(gid.y) + 0.5f) / float(cam.h) - 1.0f) * cam.tany;\n"  // GL window coords: row 0 = bottom
"    float3 ro = float3(cam.origin);\n"
"    float3 rd = normalize(float3(cam.forward) + sx * float3(cam.right) + sy * float3(cam.up));\n"
"    ray r;\n"
"    r.origin = ro;\n"
"    r.direction = rd;\n"
"    r.min_distance = 0.0f;\n"
"    r.max_distance = 1.0e9f;\n"
"    intersector<triangle_data, instancing> isect;\n"
"    isect.set_triangle_cull_mode(triangle_cull_mode::none);\n"
"    // sees world + entities + light-cores + lava + sky + liquids (LOCKSTEP: instance masks in rt_build_dynamic_as)\n"
"    intersection_result<triangle_data, instancing> res = isect.intersect(r, accel, 0x3Fu);\n"
"    bool hitw = inbounds && (res.type == intersection_type::triangle);\n"
"    // OPEN SKY (instance 4, rt_metal_skyopen): a hit on a sky brush IS a miss.\n"
"    // The miss path below then writes the sky sentinel and term 1.0, which is\n"
"    // what lets the fog kernel's sky handling, r_volumetric_skyfog and the\n"
"    // shaft kernel's beams-cross-sky design fire on map sky at all -- and\n"
"    // stops wall lighting multiplying the sky sphere by a brush's term.\n"
"    if (hitw && res.instance_id == 4u) hitw = false;\n"
"    float3 hit = hitw ? (ro + rd * res.distance) : float3(0.0f);\n"
// SEPTEMBER2 C2 (2026-09-09): the BLENDED liquid this ray passed through. Instance 6
// (mask 0x40) is NEVER in the primary mask -- the composite must keep multiplying
// the pool floor (the 2026-08-31 fact) -- so the same ray is intersected against it
// alone. Where that hit lies in front of the primary hit, or the ray missed
// everything, this is a liquid pixel: it gets the surface's OWN light and one
// reflection, written to the liquid pair below. Off, no branch is taken.
"    bool liqhit = false; float3 lqp = float3(0.0f); float3 lqn = float3(0.0f, 0.0f, 1.0f);\n"
"    if (cam.liqrt != 0u && inbounds) {\n"
"        intersection_result<triangle_data, instancing> lres = isect.intersect(r, accel, 0x40u);\n"
"        if (lres.type == intersection_type::triangle && (!hitw || lres.distance < res.distance)) {\n"
"            uint lt = lres.primitive_id;\n"
"            float3 la = float3(verts[lqidx[lt * 3u + 0u]]);\n"
"            float3 lb = float3(verts[lqidx[lt * 3u + 1u]]);\n"
"            float3 lc3 = float3(verts[lqidx[lt * 3u + 2u]]);\n"
"            lqn = normalize(cross(lb - la, lc3 - la));\n"
"            if (dot(lqn, rd) > 0.0f) lqn = -lqn;\n"                 // toward the viewer, as the surface does
"            lqp = ro + rd * lres.distance;\n"
"            liqhit = true;\n"
"        }\n"
"    }\n"
"    // --- tile AABB of hit points: simdgroup reduce, then cross-simd reduce ---\n"
"    float3 pmin = hitw ? hit : float3( 1.0e30f);\n"
"    float3 pmax = hitw ? hit : float3(-1.0e30f);\n"
"    if (liqhit) { pmin = min(pmin, lqp); pmax = max(pmax, lqp); }\n"   // C2: the liquid point joins the tile AABB -- a SUPERSET list, so the surface term is unchanged (the cull's own bit-exactness argument) and the 08-31 tile hazard cannot starve the water
"    pmin = simd_min(pmin);\n"
"    pmax = simd_max(pmax);\n"
"    if (lane == 0u) { rmin[sgi] = pmin; rmax[sgi] = pmax; }\n"
"    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"    if (tid == 0u) {\n"
"        uint nsg = (tgcount + 31u) / 32u;\n"
"        float3 bmin = rmin[0]; float3 bmax = rmax[0];\n"
"        for (uint s = 1u; s < nsg; ++s) { bmin = min(bmin, rmin[s]); bmax = max(bmax, rmax[s]); }\n"
"        rmin[0] = bmin; rmax[0] = bmax;\n"
"    }\n"
"    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"    float3 bmin = rmin[0]; float3 bmax = rmax[0];\n"
"    // --- cooperative sphere-vs-AABB cull (skipped for all-sky tiles) ---\n"
"    if (bmin.x <= bmax.x) {\n"
"        for (uint li = tid; li < nl; li += tgcount) {\n"
"            float3 lp = float3(tgl[li*RTL+0u], tgl[li*RTL+1u], tgl[li*RTL+2u]);\n"
"            float lr = tgl[li*RTL+3u];\n"
"            float3 v = clamp(lp, bmin, bmax) - lp;\n"     // closest AABB point to centre
"            if (dot(v, v) < lr * lr)\n"
"                atomic_fetch_or_explicit(&tflags[li >> 5u], 1u << (li & 31u), memory_order_relaxed);\n"
// ...and the same test against the AABB dilated by gidist, for the bounce pick.
// This half is one extra clamp+dot per light per TILE (cooperative). The cost
// that matters is NOT here: the pick loop below iterates the dilated list PER
// PIXEL, and gtlist is an ordered SUPERSET of tlist, so the dilation buys its
// coverage by lengthening a per-pixel loop. Measured on a frozen e1m3 tile at a
// 480x270 trace: +0.21 ms (+41%) at dilation 1, +0.09 ms (+16%) at 0.25.
"            if (usedil) {\n"
"                float3 gv = clamp(lp, bmin - gdil, bmax + gdil) - lp;\n"
"                if (dot(gv, gv) < lr * lr)\n"
"                    atomic_fetch_or_explicit(&gflags[li >> 5u], 1u << (li & 31u), memory_order_relaxed);\n"
"            }\n"
"        }\n"
"    }\n"
"    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"    if (tid == 0u) {\n"
"#if RT_NOTILE\n"                                          // verification: bypass the cull (RT_METAL_NOTILE=1)
"        for (uint li = 0u; li < nl; ++li) tlist[li] = li;\n"
"        tcount = nl;\n"
"#else\n"
"        uint n = 0u;\n"                                   // in-order compaction keeps results bit-exact
"        for (uint li = 0u; li < nl; ++li)\n"
"            if (atomic_load_explicit(&tflags[li >> 5u], memory_order_relaxed) & (1u << (li & 31u))) tlist[n++] = li;\n"
"        tcount = n;\n"
"#endif\n"
"    }\n"
"    if (usedil && tid == 0u) {\n"
"#if RT_NOTILE\n"
"        for (uint li = 0u; li < nl; ++li) gtlist[li] = uchar(li);\n"
"        gtcount = nl;\n"
"#else\n"
"        uint gn = 0u;\n"                        // same in-order compaction, so the pick stays deterministic
"        for (uint li = 0u; li < nl; ++li)\n"
"            if (atomic_load_explicit(&gflags[li >> 5u], memory_order_relaxed) & (1u << (li & 31u))) gtlist[gn++] = uchar(li);\n"
"        gtcount = gn;\n"
"#endif\n"
"    }\n"
"    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"    if (!inbounds) return;\n"                             // barriers all passed; per-pixel work from here
// SEPTEMBER2 C2: THE LIQUID PAIR. Left texel = the surface's OWN wall-lighting term at
// the liquid point (LOCKSTEP with the wall term's chain below: Lsum over the tile
// list with the liquid normal, the dominant light shadow-tested by ONE any-hit ray
// at its centre, ambient + walllight*6*(Lsum - dom), the saturation knob, the lmax
// shoulder). Right texel = the reflection: the view ray mirrored about the liquid
// normal, one closest hit at 0x1Fu (world, entities, cores, lava, sky; never
// liquids); a core or lava hit takes its emission colour (the GI G3 constants), a
// world or entity hit its albedo (walb / the GI constant) times the same
// dominant-shadowed sum over the FULL staged list (a reflected hit is far from
// this tile), sky nothing; weighted by Schlick's Fresnel at F0 0.02 times
// rt_metal_liquids_reflect. Deterministic per frame -- no pick, no history --
// which is Seb's choice (2026-09-08) and what lets it ship without a new
// reprojection. Wall-lighting arm only: over the lightmap the term is a shadow
// multiplier and the water keeps term 1.
"    if (cam.liqrt != 0u) {\n"
"        float4 lown = float4(1.0f, 1.0f, 1.0f, 0.0f);\n"
"        float4 lrefl = float4(0.0f);\n"
"        if (liqhit && cam.walllight > 0.0f) {\n"
"            float3 Ll = float3(0.0f); float lbw = 0.0f; float3 lblp = float3(0.0f); float3 lblc = float3(0.0f); float lbg = 0.0f;\n"
"            for (uint ii = 0u; ii < tcount; ++ii) {\n"
"                uint li = tlist[ii];\n"
"                float3 lpos = float3(tgl[li*RTL+0u], tgl[li*RTL+1u], tgl[li*RTL+2u]);\n"
"                float lr = tgl[li*RTL+3u];\n"
"                float3 tl = lpos - lqp; float d2 = dot(tl, tl);\n"
"                if (d2 >= lr*lr) continue;\n"
"                float dl = sqrt(d2);\n"
"                float ndl = dot(lqn, tl / dl); if (ndl <= 0.0f) continue;\n"
"                float fall = 1.0f - dl / lr; fall *= fall;\n"
"                fall *= rt_cone(float3(tgl[li*RTL+8u], tgl[li*RTL+9u], tgl[li*RTL+10u]), tgl[li*RTL+11u], tgl[li*RTL+12u], tgl[li*RTL+13u], -tl / dl);\n"
"                float3 lcol = float3(tgl[li*RTL+4u], tgl[li*RTL+5u], tgl[li*RTL+6u]);\n"
"                Ll += lcol * (ndl * fall);\n"
"                float br = (lcol.r + lcol.g + lcol.b) * ndl * fall;\n"
"                if (br > lbw) { lbw = br; lblp = lpos; lblc = lcol; lbg = ndl * fall; }\n"
"            }\n"
"            if (cam.sun != 0u) {\n"                                   // the sky light on water: LOCKSTEP with the surface's cast (no disc jitter -- deterministic)
"                float3 sd = normalize(float3(cam.sundir)); float ndls = dot(lqn, sd);\n"
"                if (ndls > 0.0f) {\n"
"                    ray sunr; sunr.origin = lqp + lqn * 0.75f; sunr.direction = sd; sunr.min_distance = 0.0f; sunr.max_distance = 1.0e6f;\n"
"                    intersector<triangle_data, instancing> sci; sci.set_triangle_cull_mode(triangle_cull_mode::none);\n"
"                    intersection_result<triangle_data, instancing> sres = sci.intersect(sunr, accel, 0x13u);\n"
"                    if (sres.type == intersection_type::triangle && sres.instance_id == 4u) Ll += float3(cam.suncol) * ndls;\n"
"                }\n"
"            }\n"
"            float lvis = 1.0f;\n"
"            if (lbw > 0.0f) {\n"
"                float3 d = lblp - lqp; float sd = length(d); d /= sd;\n"
"                ray sr; sr.origin = lqp + lqn * 0.75f; sr.direction = d; sr.min_distance = 0.0f; sr.max_distance = max(sd - 1.0f, 0.0f);\n"
"                intersector<triangle_data, instancing> si; si.set_triangle_cull_mode(triangle_cull_mode::none); si.accept_any_intersection(true);\n"
"                if (si.intersect(sr, accel, 0x3u).type != intersection_type::none) lvis = 0.0f;\n"
"            }\n"
"            float3 ldom = lblc * lbg * (1.0f - cam.darkness) * (1.0f - lvis);\n"
"            float3 lcol2 = max(float3(cam.ambient) + (cam.walllight * 6.0f) * (Ll - ldom), float3(0.0f));\n"
"            if (cam.colorstr != 1.0f) { float lum = dot(lcol2, float3(0.299f, 0.587f, 0.114f)); lcol2 = max(mix(float3(lum), lcol2, cam.colorstr), float3(0.0f)); }\n"
"            if (cam.lmax > 0.0f) { float lm = max(lcol2.r, max(lcol2.g, lcol2.b)); if (lm > cam.lmax) { float e = lm - cam.lmax; float k = cam.lmax * (2.0f / 3.0f); lcol2 *= (cam.lmax + e * k / (k + e)) / lm; } }\n"
"            lown = float4(lcol2, 1.0f);\n"
"            if (cam.liqreflect > 0.0f) {\n"
"                float3 rdir = normalize(rd - 2.0f * dot(lqn, rd) * lqn);\n"
"                ray rr; rr.origin = lqp + lqn * 0.75f; rr.direction = rdir; rr.min_distance = 0.0f; rr.max_distance = 1.0e6f;\n"
"                intersector<triangle_data, instancing> ri; ri.set_triangle_cull_mode(triangle_cull_mode::none);\n"
"                intersection_result<triangle_data, instancing> rres = ri.intersect(rr, accel, 0x1Fu);\n"
"                float3 rad = float3(0.0f);\n"
"                if (rres.type == intersection_type::triangle && rres.instance_id >= 2u && rres.instance_id != 4u) {\n"
"                    rad = (rres.instance_id == 3u) ? float3(1.0f, 0.4f, 0.1f) : float3(1.0f, 0.5f, 0.1f);\n"   // LOCKSTEP the GI bounce's emissive constants
"                } else if (rres.type == intersection_type::triangle && rres.instance_id < 2u) {\n"
"                    device const packed_float3 *rvp = (rres.instance_id == 0u) ? verts : everts;\n"
"                    device const uint *rip = (rres.instance_id == 0u) ? idx : eidx;\n"
"                    uint rt = rres.primitive_id;\n"
"                    float3 ra = float3(rvp[rip[rt * 3u + 0u]]); float3 rb = float3(rvp[rip[rt * 3u + 1u]]); float3 rc = float3(rvp[rip[rt * 3u + 2u]]);\n"
"                    float3 rn = normalize(cross(rb - ra, rc - ra)); if (dot(rn, rdir) > 0.0f) rn = -rn;\n"
"                    float3 rp = rr.origin + rdir * rres.distance;\n"
"                    float3 Lr = float3(0.0f); float rbw = 0.0f; float3 rblp = float3(0.0f); float3 rblc = float3(0.0f); float rbg = 0.0f;\n"
"                    for (uint li = 0u; li < nl; ++li) {\n"
"                        float3 lpos = float3(tgl[li*RTL+0u], tgl[li*RTL+1u], tgl[li*RTL+2u]);\n"
"                        float lr = tgl[li*RTL+3u];\n"
"                        float3 tl = lpos - rp; float d2 = dot(tl, tl);\n"
"                        if (d2 >= lr*lr) continue;\n"
"                        float dl = sqrt(d2);\n"
"                        float ndl = dot(rn, tl / dl); if (ndl <= 0.0f) continue;\n"
"                        float fall = 1.0f - dl / lr; fall *= fall;\n"
"                        fall *= rt_cone(float3(tgl[li*RTL+8u], tgl[li*RTL+9u], tgl[li*RTL+10u]), tgl[li*RTL+11u], tgl[li*RTL+12u], tgl[li*RTL+13u], -tl / dl);\n"
"                        float3 lcol = float3(tgl[li*RTL+4u], tgl[li*RTL+5u], tgl[li*RTL+6u]);\n"
"                        Lr += lcol * (ndl * fall);\n"
"                        float br = (lcol.r + lcol.g + lcol.b) * ndl * fall;\n"
"                        if (br > rbw) { rbw = br; rblp = lpos; rblc = lcol; rbg = ndl * fall; }\n"
"                    }\n"
"                    float rvis = 1.0f;\n"
"                    if (rbw > 0.0f) {\n"
"                        float3 d = rblp - rp; float sd = length(d); d /= sd;\n"
"                        ray sr; sr.origin = rp + rn * 0.75f; sr.direction = d; sr.min_distance = 0.0f; sr.max_distance = max(sd - 1.0f, 0.0f);\n"
"                        intersector<triangle_data, instancing> si; si.set_triangle_cull_mode(triangle_cull_mode::none); si.accept_any_intersection(true);\n"
"                        if (si.intersect(sr, accel, 0x3u).type != intersection_type::none) rvis = 0.0f;\n"
"                    }\n"
"                    float3 rdom = rblc * rbg * (1.0f - cam.darkness) * (1.0f - rvis);\n"
"                    float3 rterm = max(float3(cam.ambient) + (cam.walllight * 6.0f) * (Lr - rdom), float3(0.0f));\n"
"                    if (cam.lmax > 0.0f) { float lm = max(rterm.r, max(rterm.g, rterm.b)); if (lm > cam.lmax) { float e = lm - cam.lmax; float k = cam.lmax * (2.0f / 3.0f); rterm *= (cam.lmax + e * k / (k + e)) / lm; } }\n"
"                    float3 ralb = float3(cam.gialbedo);\n"
"                    if (rres.instance_id == 0u) { uchar4 wa = walb[rt]; ralb = float3(wa.x, wa.y, wa.z) * (1.0f / 255.0f); }\n"
"                    rad = ralb * rterm;\n"
"                }\n"
"                float cosT = max(dot(-rd, lqn), 0.0f); float omc = 1.0f - cosT; float fres = 0.02f + 0.98f * omc * omc * omc * omc * omc;\n"
"                lrefl = float4(rad * (fres * cam.liqreflect), 1.0f);\n"
"            }\n"
"        }\n"
"        liqOut.write(lown, gid);\n"
"        liqOut.write(lrefl, uint2(gid.x + cam.w, gid.y));\n"
"    }\n"
"    // EMISSIVE hit (instance 2 = flame light-core, instance 3 = lava sheet,\n"
"    // instance 5 = opaque liquid, rt_metal_liquidemissive -- the >= 2u is\n"
"    // DELIBERATE for it: a liquid's authored murky texture IS its look, Q1\n"
"    // liquids carry no lightmaps, so term 1.0 renders the sheet exactly as\n"
"    // rt_metal 0 does and the pool floor stops printing through):\n"
"    // term 1.0 is right in BOTH modes -- over-lightmap: no darkening of the\n"
"    // self-lit skin; walllight: scene * 1 = the authored albedo, whose\n"
"    // fullbright-palette pixels ARE the glow (every *lava1 texel is\n"
"    // fullbright, so the sheet's authored appearance IS its emission).\n"
"    // History keeps the REAL hit so the fog and shaft marches stop at the\n"
"    // flame/sheet; va = 1 keeps the EMA clean. Must sit BEFORE the vertex\n"
"    // fetch below: neither instance has entries in the entity buffers.\n"
"    if (hitw && res.instance_id >= 2u) {\n"
"        histOut.write(float4(hit, 1.0f), gid);\n"
"        secOut.write(float4(0.0f), gid);\n"
"        giOut.write(float4(0.0f), gid);\n"   // GI history is written on EVERY path: a reprojection onto this pixel must never read stale colour behind a valid hist position
"        outtex.write(float4(cam.gbias != 0u ? float3(0.0f) : float3(1.0f), res.distance), gid);\n"
"        return;\n"
"    }\n"
"    float3 col;\n"
"    float3 gdbg = float3(0.0f);\n"   // RT_METAL_GIBIAS classification; shares col's scope because the GI block is inside if (hitw)

"    if (hitw) {\n"
"        // instance 0 = static world, instance 1 = dynamic entities; both are in\n"
"        // world space (identity instance transform) so the normal needs no transform\n"
"        device const packed_float3 *vp = (res.instance_id == 0u) ? verts : everts;\n"
"        device const uint *ip = (res.instance_id == 0u) ? idx : eidx;\n"
"        uint t = res.primitive_id;\n"
"        float3 a = float3(vp[ip[t * 3u + 0u]]);\n"
"        float3 b = float3(vp[ip[t * 3u + 1u]]);\n"
"        float3 c = float3(vp[ip[t * 3u + 2u]]);\n"
"        float3 ng = normalize(cross(b - a, c - a));\n"
"        if (dot(ng, rd) > 0.0f) ng = -ng;\n"                  // orient toward the viewer
"        float3 n = ng;\n"
"        // SMOOTH SHADING (rt_metal_smoothnormals): entity hits interpolate the\n"
"        // model's vertex normals via barycentrics. Low-poly monsters otherwise\n"
"        // read as flat-shaded the instant a close muzzle flash dominates them\n"
"        // (per-facet N.L deltas explode). Shading only: the shadow-ray origin\n"
"        // keeps the GEOMETRIC normal offset (self-intersection safety), and\n"
"        // zero-length normals (models without them) fall back per pixel.\n"
"        if (res.instance_id == 1u && cam.entnorms != 0u) {\n"
"            float2 bw = res.triangle_barycentric_coord;\n"
"            float3 ns = float3(enorms[ip[t*3u+0u]]) * (1.0f - bw.x - bw.y)\n"
"                      + float3(enorms[ip[t*3u+1u]]) * bw.x\n"
"                      + float3(enorms[ip[t*3u+2u]]) * bw.y;\n"
"            float l2 = dot(ns, ns);\n"
"            if (l2 > 1.0e-8f) {\n"
"                n = ns * rsqrt(l2);\n"
"                if (dot(n, rd) > 0.0f) n = -n;\n"
"            }\n"
"        }\n"
"        // Pick the most significant REAL light reaching this point (its shadow\n"
"        // dominates locally); shadow only from it. Also remember its colour, its\n"
"        // geometric weight (ndl*falloff) and whether it is a DYNAMIC light (index\n"
"        // < numDynamic) so the output can brighten the scene toward its colour.\n"
"        float bestw = 0.0f; float3 blp = float3(0.0f, 0.0f, 0.0f); float brad = 1.0f;\n"
"        float3 blc = float3(0.0f); float bg = 0.0f; bool bdyn = false; uint bidx = 0xffffffffu;\n"
"        float3 Lsum = float3(0.0f);\n"
// EVERY LIGHT CASTS A SHADOW (rt_metal_lightsample). The dominant light chosen
// above is the only one this kernel has ever shadow-tested, so a torch beside a
// brighter light casts none at all: it loses the vote everywhere (colour sum
// ~1.65 against ~3 for a plain light) and its whole contribution rides in Lsum
// unshadowed. This picks ONE more light per pixel, in the same pass, with
// probability proportional to its own contribution, and divides by that
// probability -- so the estimator of sum(c_i*(1-v_i)) over the non-dominant
// lights is unbiased and, in expectation, every light shadows.
"        float js = (cam.lsample != 0u) ? RT_SELRAND(gid, cam.frame, 0u, cam.history) : 0.0f;\n"
// THE ROUND SPOTLIGHTS (rt_metal_shadowlights, 2026-09-19). Seb: "some of the
// lights are not soft, and are cast from unseen sources, casting clean round lit
// areas in odd locations". That is this kernel's oldest structural
// approximation showing: the dominant light above is the ONLY one a pixel has
// ever shadow-tested, and every other light in the tile rides in Lsum with NO
// occlusion at all -- so a light in the next room, inside a ceiling recess or
// behind a pillar paints its full (1-d/r)^2 disc straight through the geometry.
// Measured on his own demo49 f5000 (e2m1): the term buffer carries perfect
// circles spilling across a wall, a ledge and a floor uninterrupted, and the
// full stochastic estimator (rt_metal_lightsample 2) removes them -- confirming
// the light is unshadowed rather than mis-shaded. That estimator is the one his
// eye rejected in August for speckle, so this is the DETERMINISTIC form: track
// the brightest few lights rather than one and shadow-test each. No pick, no
// variance, no history -- and at 1 the array below is never written and the
// apply loop never runs, so it is the old bytes by construction.
"        float rupw[4] = {0.0f, 0.0f, 0.0f, 0.0f};\n"
"        float3 ruplp[4]; float3 ruplc[4]; float rupg[4]; float ruprad[4]; uint rupidx[4];\n"
"        for (uint j = 0u; j < 4u; ++j) { ruplp[j] = float3(0.0f); ruplc[j] = float3(0.0f); rupg[j] = 0.0f; ruprad[j] = 1.0f; rupidx[j] = 0xffffffffu; }\n"
"        float W2 = 0.0f; uint sidx = 0xffffffffu; bool shave = false;\n"
"        float3 slp = float3(0.0f); float srad = 1.0f; float3 slc = float3(0.0f); float sg = 0.0f; float sw = 0.0f;\n"                        // full per-pixel light sum (wall-lighting mode)
"        for (uint ii = 0u; ii < tcount; ++ii) {\n"            // tile survivors only (ordered)
"            uint li = tlist[ii];\n"
"            float3 lp = float3(tgl[li*RTL+0u], tgl[li*RTL+1u], tgl[li*RTL+2u]);\n"
"            float lr = tgl[li*RTL+3u];\n"
"            float3 tl = lp - hit; float d2 = dot(tl, tl);\n"
"            if (d2 >= lr*lr) continue;\n"                     // squared-distance cull (no sqrt for far lights)
"            float dl = sqrt(d2);\n"
"            float ndl = dot(n, tl / dl); if (ndl <= 0.0f) continue;\n"
"            float fall = 1.0f - dl / lr; fall *= fall;\n"     // distance falloff ("fall": float3 a above is a vertex)
// SPOT CONE -- LOCKSTEP with the shaft and fog kernels below; the helper itself
// is kConeSrc, spliced into all three. Folded into the falloff so it scales the
// accumulated light AND the dominant-light vote together: a surface outside the
// beam must not win the vote and then be shadow-rayed as if it were lit.
"            fall *= rt_cone(float3(tgl[li*RTL+8u], tgl[li*RTL+9u], tgl[li*RTL+10u]), tgl[li*RTL+11u], tgl[li*RTL+12u], tgl[li*RTL+13u], -tl / dl);\n"
"            float3 lcol = float3(tgl[li*RTL+4u], tgl[li*RTL+5u], tgl[li*RTL+6u]);\n"
"            Lsum += lcol * (ndl * fall);\n"                   // accumulate this light's diffuse contribution
"            float br = (lcol.r + lcol.g + lcol.b) * ndl * fall;\n"
"            if (br > bestw) { bestw = br; blp = lp; brad = lr; blc = lcol; bg = ndl * fall; bdyn = (li < cam.numDynamic); bidx = ii; }\n"
// Descending insertion into the top-4 (the dominant is entry 0 of it too;
// the apply loop skips whichever entry IS the dominant, so the runners-up
// are the next brightest). Bounded and branch-uniform: four compares.
"            if (cam.shadowlights > 1u) {\n"
"                for (uint j = 0u; j < 4u; ++j) {\n"
"                    if (br > rupw[j]) {\n"
"                        for (uint k = 3u; k > j; --k) { rupw[k] = rupw[k-1u]; ruplp[k] = ruplp[k-1u]; ruplc[k] = ruplc[k-1u]; rupg[k] = rupg[k-1u]; ruprad[k] = ruprad[k-1u]; rupidx[k] = rupidx[k-1u]; }\n"
"                        rupw[j] = br; ruplp[j] = lp; ruplc[j] = lcol; rupg[j] = ndl * fall; ruprad[j] = lr; rupidx[j] = ii;\n"
"                        break;\n"
"                    }\n"
"                }\n"
"            }\n"
// ONE-PASS WEIGHTED PICK FROM A SINGLE UNIFORM, exact rather than approximate:
// the surviving uniform is RESCALED back onto [0,1) after each item, so the
// selection probability really is br/W at every step and the final pick is
// exactly proportional to weight. (A textbook reservoir wants an independent
// uniform per item, which a kernel with one blue-noise tap per pixel has not
// got; hashing per light would throw away the blue-noise distribution.)
"            if (cam.lsample != 0u && br > 0.0f) {\n"
"                W2 += br;\n"
"                float pick = js * W2;\n"
"                if (pick <= br) { js = pick / br; sidx = ii; slp = lp; srad = lr; slc = lcol; sg = ndl * fall; sw = br; shave = true; }\n"
"                else { float rem = W2 - br; js = (rem > 0.0f) ? ((pick - br) / rem) : 0.0f; }\n"
"            }\n"
"        }\n"
// THE SKY LIGHT (SEPTEMBER2 D, 2026-09-06). A directional sun at infinity: one
// CLOSEST-hit ray from the surface toward the sun, jittered inside the sun's
// disc (the map's _sunlight_penumbra), and the surface is sunlit exactly when
// that ray leaves the level through OPEN SKY -- instance 4, the same open-sky
// structure the fog cap and the sky-as-miss rule key on. Mask 0x13u: world and
// entities occlude, sky terminates, and the emitters and liquids (cores, lava,
// blended water) neither block nor count. Closest-hit, not any-hit, because a
// wall short of the sky must win. Added to Lsum like any light, so it takes the
// walllight*6 chain, the saturation knob and the lmax shoulder with the rest;
// it never enters the dominant vote (its shadow is this ray). Walllight arm
// only: under the lightmap the map's baked sun is already there, and under
// wall lighting that lightmap is discarded -- which is exactly why an outdoor
// AD map read flat under rt_metal_walllight until now.
"        if (cam.sun != 0u && cam.walllight > 0.0f) {\n"
"            float3 sd = normalize(float3(cam.sundir));\n"
"            float ndls = dot(n, sd);\n"
"            if (ndls > 0.0f) {\n"
"                float3 up0s = (fabs(sd.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);\n"
"                float3 Ts = normalize(cross(up0s, sd)); float3 Bs = cross(sd, Ts);\n"
"                float u1 = RT_SELRAND(gid, cam.frame, 5u, cam.history);\n"   // its own slots: the pick (0), the secondary disc (1) and the bounce (2-4) keep theirs
"                float u2 = RT_SELRAND(gid, cam.frame, 6u, cam.history);\n"
"                float rr = sqrt(u1) * cam.sunpen; float th = 6.2831853f * u2;\n"
"                float3 dsun = normalize(sd + (cos(th) * rr) * Ts + (sin(th) * rr) * Bs);\n"
"                ray sunr; sunr.origin = hit + ng * 0.75f; sunr.direction = dsun; sunr.min_distance = 0.0f; sunr.max_distance = 1.0e6f;\n"
"                intersector<triangle_data, instancing> sci;\n"
"                sci.set_triangle_cull_mode(triangle_cull_mode::none);\n"
"                intersection_result<triangle_data, instancing> sres = sci.intersect(sunr, accel, 0x13u);\n"
"                if (sres.type == intersection_type::triangle && sres.instance_id == 4u) Lsum += float3(cam.suncol) * ndls;\n"
"            }\n"
"        }\n"
"        float vis = 1.0f;\n"                                  // soft visibility of the dominant light
"        if (bestw > 0.0f) {\n"
"            float3 lv = blp - hit; float lc_d = length(lv); float3 lc = lv / lc_d;\n"
"            uint N = clamp(cam.samples, 1u, 64u);\n"          // rt_metal_samples
"            const float GA = 2.39996323f;\n"
"            float R = clamp(brad * cam.softness, 8.0f, 40.0f);\n"  // penumbra width (rt_metal_softness)
// CONTACT-HARDENED SHADOWS (BEAUTY B3, measure-first). Every shadow ray in the
// kernel is any-hit, so no blocker distance exists; this casts ONE closest-hit
// probe at the light's centre and, where it is blocked, scales the disc the N
// any-hit samples cover by (d_receiver - d_blocker) / d_receiver -- PCSS's
// shape: a blocker at the receiver (a foot on the floor, a ledge's underside)
// sharpens the shadow to its edge, a blocker near the light keeps the full
// penumbra. An unblocked probe leaves R as it was (the receiver sits in the lit
// half of the penumbra; the samples find the edge). cam.contact blends the
// scaling in; at 0 the branch is untaken and the loop below is the old text.
"            if (cam.contact > 0.0f) {\n"
"                ray cr; cr.origin = hit + ng * 0.75f; cr.direction = lc; cr.min_distance = 0.0f; cr.max_distance = max(lc_d - 1.0f, 0.0f);\n"
"                intersector<triangle_data, instancing> ci; ci.set_triangle_cull_mode(triangle_cull_mode::none);\n"
"                intersection_result<triangle_data, instancing> cres = ci.intersect(cr, accel, 0x3u);\n"
"                if (cres.type == intersection_type::triangle) {\n"
"                    float ck = clamp((lc_d - cres.distance) / max(lc_d, 1.0f), 0.02f, 1.0f);\n"
"                    R = max(R * mix(1.0f, ck, cam.contact), 0.5f);\n"
"                }\n"
"            }\n"
"            float3 up0 = (fabs(lc.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);\n"
"            float3 T = normalize(cross(up0, lc));\n"
"            float3 B = cross(lc, T);\n"
"#if RT_BLUENOISE\n"
"            float jf = rt_bnjitter(gid, cam.frame, cam.history, bn);\n"
"#else\n"
"            float ign = fract(52.9829189f * fract(0.06711056f * float(gid.x) + 0.00583715f * float(gid.y)));\n"
"            // when temporal is on, rotate the sample set each frame (golden ratio) so\n"
"            // accumulation averages many distinct sets; when off, keep the static\n"
"            // per-pixel dither (shimmer-free stills, like M5).\n"
"            float jf = (cam.history > 0.0f) ? fract(ign + float(cam.frame) * 0.61803399f) : ign;\n"
"#endif\n"
"            float rot = 6.2831853f * jf;\n"
"            intersector<triangle_data, instancing> si;\n"
"            si.set_triangle_cull_mode(triangle_cull_mode::none);\n"
"            si.accept_any_intersection(true);\n"
"            float hitsN = 0.0f;\n"
"            for (uint i = 0u; i < N; ++i) {\n"
"                float rr = sqrt((float(i) + 0.5f) / float(N)) * R;\n"
"                float th = float(i) * GA + rot;\n"
"                float3 p = blp + (cos(th) * rr) * T + (sin(th) * rr) * B;\n"
"                float3 d = p - hit; float sd = length(d); d /= sd;\n"
"                ray sr;\n"
"                sr.origin = hit + ng * 0.75f;\n"
"                sr.direction = d;\n"
"                sr.min_distance = 0.0f;\n"
"                sr.max_distance = max(sd - 1.0f, 0.0f);\n"   // clamped like the fog/shaft kernels (a light within a unit of the surface made the interval negative)
"                if (si.intersect(sr, accel, 0x3u).type != intersection_type::none) hitsN += 1.0f;\n"
"            }\n"
"            vis = 1.0f - hitsN / float(N);\n"
"        }\n"
// THE SECONDARY LIGHT'S SHADOW. sec is the picked light's OCCLUDED contribution
// divided by its selection probability (sw/W2), which is what makes the estimate
// unbiased. Skipped when the pick landed on the dominant, which the block above
// has already tested. The rays reuse the dominant's Vogel-spiral shape (the
// picked light's own disc radius and tangent frame) so the penumbra reads the
// same; a separate rotation keeps the two sets from lining up.
"        float3 sec = float3(0.0f);\n"
// Gated on the wall-lighting arm: it is the only consumer. The over-lightmap arm
// below renders a grey shadow multiplier from the dominant light's visibility
// alone, and has nowhere to put a second light's colour -- firing rays for it
// there would be wasted work (and would perturb a bed that pins walllight 0).
"        if (cam.lsample != 0u && cam.walllight > 0.0f && shave && sw > 0.0f && sidx != bidx) {\n"
"            float3 lv = slp - hit; float lc_d = length(lv); float3 lc = lv / lc_d;\n"
"            uint N2 = clamp(cam.lsrays, 1u, 16u);\n"
"            float R2 = clamp(srad * cam.softness, 8.0f, 40.0f);\n"
"            float3 up2 = (fabs(lc.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);\n"
"            float3 T2 = normalize(cross(up2, lc));\n"
"            float3 B2 = cross(lc, T2);\n"
"            float rot2 = 6.2831853f * RT_SELRAND(gid, cam.frame, 1u, cam.history);\n"
"            intersector<triangle_data, instancing> si2;\n"
"            si2.set_triangle_cull_mode(triangle_cull_mode::none);\n"
"            si2.accept_any_intersection(true);\n"
"            float hits2 = 0.0f;\n"
"            for (uint i = 0u; i < N2; ++i) {\n"
"                float rr = sqrt((float(i) + 0.5f) / float(N2)) * R2;\n"
"                float th = float(i) * 2.39996323f + rot2;\n"
"                float3 pp = slp + (cos(th) * rr) * T2 + (sin(th) * rr) * B2;\n"
"                float3 d = pp - hit; float sd = length(d); d /= sd;\n"
"                ray sr;\n"
"                sr.origin = hit + ng * 0.75f;\n"
"                sr.direction = d;\n"
"                sr.min_distance = 0.0f;\n"
"                sr.max_distance = max(sd - 1.0f, 0.0f);\n"
"                if (si2.intersect(sr, accel, 0x3u).type != intersection_type::none) hits2 += 1.0f;\n"
"            }\n"
"            sec = slc * (sg * (hits2 / float(N2)) * (W2 / sw));\n"
"        }\n"
// THE RUNNERS-UP (rt_metal_shadowlights). One shadow test for each of the next
// brightest lights after the dominant, and their occluded contribution joins
// dom. Three deliberate differences from the dominant's test, each a cost
// choice stated rather than hidden: the rotation is the STATIC per-pixel
// dither, never frame-rotated (the dominant's visibility is smoothed by the
// history EMA and these have no EMA, so a rotating set would crawl); the ray
// count is its own small cvar rather than rt_metal_samples; and contact
// hardening is not applied (one more closest-hit probe each, for a light that
// is by construction dimmer than the one that already has it). Wall-lighting
// arm only -- the over-lightmap arm renders a grey multiplier from the
// dominant's visibility alone and has nowhere to put a second light's colour,
// exactly as the secondary estimate above is gated.
"        float3 dom2 = float3(0.0f);\n"
"        if (cam.shadowlights > 1u && cam.walllight > 0.0f) {\n"
"            uint want = min(cam.shadowlights - 1u, 3u);\n"
"            uint N3 = clamp(cam.shadowlightrays, 1u, 8u);\n"
"            float ign3 = fract(52.9829189f * fract(0.06711056f * float(gid.x) + 0.00583715f * float(gid.y)));\n"
"            float rot3 = 6.2831853f * ign3;\n"
"            intersector<triangle_data, instancing> si3;\n"
"            si3.set_triangle_cull_mode(triangle_cull_mode::none);\n"
"            si3.accept_any_intersection(true);\n"
"            uint got = 0u;\n"
"            for (uint j = 0u; j < 4u && got < want; ++j) {\n"
"                if (rupw[j] <= 0.0f) break;\n"
"                if (rupidx[j] == bidx) continue;\n"
"                got++;\n"
"                float3 lv3 = ruplp[j] - hit; float d3 = length(lv3); float3 lc3 = lv3 / d3;\n"
"                float R3 = clamp(ruprad[j] * cam.softness, 8.0f, 40.0f);\n"
"                float3 up3 = (fabs(lc3.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);\n"
"                float3 T3 = normalize(cross(up3, lc3)); float3 B3 = cross(lc3, T3);\n"
"                float hits3 = 0.0f;\n"
"                for (uint i = 0u; i < N3; ++i) {\n"
"                    float rr = sqrt((float(i) + 0.5f) / float(N3)) * R3;\n"
"                    float th = float(i) * 2.39996323f + rot3;\n"
"                    float3 pp = ruplp[j] + (cos(th) * rr) * T3 + (sin(th) * rr) * B3;\n"
"                    float3 d = pp - hit; float sd = length(d); d /= sd;\n"
"                    ray sr; sr.origin = hit + ng * 0.75f; sr.direction = d; sr.min_distance = 0.0f; sr.max_distance = max(sd - 1.0f, 0.0f);\n"
"                    if (si3.intersect(sr, accel, 0x3u).type != intersection_type::none) hits3 += 1.0f;\n"
"                }\n"
"                dom2 += ruplc[j] * (rupg[j] * (hits3 / float(N3)) * (1.0f - cam.darkness));\n"
"            }\n"
"        }\n"
// ONE-BOUNCE DIFFUSE GI (rt_metal_gi, GIARC G1, 2026-08-29). One cosine-
// hemisphere ray from the primary hit; at the bounce hit, direct light is
// evaluated with the FOGLIGHT machinery -- one stochastic pick over the PRIMARY
// pixel's tile list (falloff at the bounce position), one shadow ray, 1/p
// weight. The tile list belongs to the primary pixel: a light reaching the
// bounce but outside this tile's cull is missed -- a stated bias (GIARC.md);
// the world-space light grid is the structural fix and waits on measurement.
// Cosine sampling makes the estimator of the bounce irradiance just the direct
// light at the sampled point (the cos cancels the pdf); the 1/pi and the mean
// surface albedo fold into cam.gialbedo, which the cvar help says out loud.
// Units: the result is in raw Lsum units, composed inside the walllight*6
// scale below, so it rides the term's whole tone chain (saturation, lmax
// shoulder, EDR) for free. WALLLIGHT ARM ONLY, like the lightsample secondary
// and for the same reason: the over-lightmap arm's output is a shadow
// MULTIPLIER over baked light -- adding bounce light there double-counts
// whatever the lightmap carries and brightens multiplicatively; one consumer,
// one bed. Ray mask: 0x3u (world + entities, the shadow-ray class) in G1's
// bytes; with rt_metal_gi_emissive > 0 (G3, 2026-08-29) it widens to 0xFu so
// the bounce also STOPS at the emissive core (0x4) and lava (0x8) instances
// and takes their emission directly -- lava throws orange onto ceilings,
// torch flames bounce their own glow. Sky (0x10) stays out (a miss
// contributes nothing; sky GI is a different feature) and liquids (0x20)
// stay out (bounce rays pass through water to the lit pool floor, the
// shadow-ray convention -- a water sheet's "emission" is authored murk, not
// light). The mask is gated on the GAIN so emissive 0 is the G1 bounce byte
// for byte: with 0xFu a ray that used to reach the wall behind a flame would
// stop at the flame and, at gain 0, contribute zero where it used to
// contribute the wall's lit radiance. RT_SELRAND slots k=2,3 (hemisphere)
// and k=4 (pick) -- the pick and the direction must not share a value, the
// original decorrelation rule.
// GI RATE CONTROL (rt_metal_gi_rate, GIARC G4-1, 2026-08-29). GI is
// low-frequency light carried by a deep reprojected EMA -- exactly the signal
// that does not need a fresh sample at every pixel every frame. At rate N the
// bounce ray fires on a frame-rotating 1-in-N subset of pixels ((x + 2y +
// frame) % N -- uniform spatial coverage, every pixel sampled every N frames)
// and every skipped pixel carries its REPROJECTED HISTORY FORWARD UNCHANGED
// at the accumulation site below. That last clause is the whole correctness
// of the feature: mixing a zero "sample" into a skipped pixel's EMA would
// drain the accumulator to black at the skip rate. Rate 1 short-circuits
// before the modulo, the old bytes exactly. The rotation deliberately uses
// cam.frame UNGATED by cam.history (unlike RT_SELRAND): gated, a frozen bed
// would freeze the phase too and 1-1/N of all tiles would simply never
// sample.
//
// THE ROTATION IS PER 16x16 TILE, NOT PER PIXEL, AND THAT IS MEASURED, NOT
// STYLE (2026-08-29 fullscreen bench, out-girate): a per-pixel checkerboard
// saved 0-2 fps where GI's whole cost is 6-7, because every SIMD group still
// contained firing lanes and the warp paid the bounce's full latency -- the
// classic divergence miss. 16x16 is exactly the dispatch's
// threadsPerThreadgroup, so a skipped tile retires its whole threadgroup and
// the saving is real (re-measured after the change, same bench).
"        float3 gib = float3(0.0f);\n"
"        float gob = 0.0f;\n"   // BEAUTY B1: this sample's occlusion (1 = the bounce ray stopped at contact), 0 when no ray fired
"        bool gifire = (cam.girate <= 1u) || ((((gid.x >> 4u) + 2u * (gid.y >> 4u) + cam.frame) % cam.girate) == 0u);\n"
"        if (cam.gi != 0u && cam.walllight > 0.0f && gifire) {\n"
"            float gu1 = RT_SELRAND(gid, cam.frame, 2u, cam.history);\n"
"            float gu2 = RT_SELRAND(gid, cam.frame, 3u, cam.history);\n"
"            float3 gup = (fabs(n.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);\n"
"            float3 gT = normalize(cross(gup, n));\n"
"            float3 gB = cross(n, gT);\n"
"            float grr = sqrt(gu1);\n"
"            float gph = 6.2831853f * gu2;\n"
"            float3 gd = normalize(gT * (grr * cos(gph)) + gB * (grr * sin(gph)) + n * sqrt(max(1.0f - gu1, 0.0f)));\n"
// The hemisphere is about the SMOOTH normal; where it disagrees with the
// geometric one a sampled direction can point into the surface -- skip it
// (contributes 0; a slight grazing-angle darkening, cheaper and safer than a
// guaranteed self-hit through the 0.75 offset).
"            if (dot(gd, ng) > 0.0f) {\n"
"                ray gr;\n"
"                gr.origin = hit + ng * 0.75f;\n"
"                gr.direction = gd;\n"
"                gr.min_distance = 0.0f;\n"
"                gr.max_distance = cam.gidist;\n"
"                intersector<triangle_data, instancing> gsect;\n"
"                gsect.set_triangle_cull_mode(triangle_cull_mode::none);\n"
"                uint gmask = (cam.giemissive > 0.0f) ? 0xFu : 0x3u;\n"
"                if (cam.gbias == 2u) gmask |= 0x10u;\n"   // MEASURE-ONLY: sky (instance 4) joins the mask so a sky escape is distinguishable from running out of gidist

"                intersection_result<triangle_data, instancing> gres = gsect.intersect(gr, accel, gmask);\n"
// AMBIENT OCCLUSION FROM THE SAME RAY (BEAUTY B1): the closest-hit bounce ray
// already reports its distance; a short one means the point is enclosed. The
// sample is stored in the GI history's spare alpha, rides its reprojection and
// its EMA exactly as the colour does, and is applied to the flat ambient fill
// ONLY (rt_metal_ambient is 'light with no source, on purpose', and this is its
// corrective) -- never to the direct term, which has its own shadows. Gated on
// the cvar so at 0 the alpha stays the 0 it always was.
"                if (cam.giao > 0.0f && gres.type == intersection_type::triangle && gres.instance_id != 4u)\n"
"                    gob = 1.0f - smoothstep(0.0f, cam.giaodist, gres.distance);\n"
// EMISSIVE BOUNCE HIT (rt_metal_gi_emissive, G3): a bounce ray stopping at a
// core (instance 2) or lava (instance 3) takes that surface's EMISSION as its
// radiance -- no light pick, no shadow ray, no triangle fetch. The colours are
// the ones the direct path already calibrated: the merged lava lights' warm
// (1, 0.4, 0.1) (cl_screen.c, the lava light grid -- LOCKSTEP) and the torch
// heuristic's (1, 0.5, 0.1) (r_shadow.c's classname override -- LOCKSTEP).
// gialbedo is deliberately NOT applied: albedo is the reflectance of a BOUNCE
// surface, and an emitter emits rather than reflects -- the gain cvar alone
// maps emission colour units onto Lsum units. Honest overlap, stated: the
// primary surface already receives lava/torch light DIRECTLY through the
// merged lava lights and the real torch lights in Lsum, so an emissive hit
// partly re-delivers what those sparse point lights approximate; the gain is
// the calibration for how much the area-source form adds on top, judged on
// the e1m7 stills and by Seb's eye in motion.
// GIARC G4 item 3's PREMISE, MEASURE-ONLY (RT_METAL_GIBIAS=2). R = this
// pixel fired a bounce ray (the denominator); G = it hit nothing at all;
// B = it escaped through SKY specifically. B/R is the fraction of the
// scene's hemispheres that can see sky, which is the number that decides
// whether a sky term varies spatially or is a flat lift wearing a cvar.
"                if (cam.gbias == 2u) {\n"
"                    bool miss = (gres.type != intersection_type::triangle);\n"
"                    bool sky  = (!miss && gres.instance_id == 4u);\n"
"                    gdbg = float3(1.0f, miss ? 1.0f : 0.0f, sky ? 1.0f : 0.0f);\n"
"                }\n"
"                if (gres.type == intersection_type::triangle && gres.instance_id == 4u) {\n"
"                    gib = float3(0.0f);\n"   // MEASURE-ONLY mode 2: sky is in the mask but contributes nothing, so the arm cannot be mistaken for the feature
"                } else if (gres.type == intersection_type::triangle && gres.instance_id >= 2u) {\n"
"                    float3 gle = (gres.instance_id == 3u) ? float3(1.0f, 0.4f, 0.1f) : float3(1.0f, 0.5f, 0.1f);\n"
"                    gib = gle * cam.giemissive;\n"
"                } else if (gres.type == intersection_type::triangle) {\n"
"                    device const packed_float3 *gvp = (gres.instance_id == 0u) ? verts : everts;\n"
"                    device const uint *gip = (gres.instance_id == 0u) ? idx : eidx;\n"
"                    uint gt = gres.primitive_id;\n"
"                    float3 gva = float3(gvp[gip[gt * 3u + 0u]]);\n"
"                    float3 gvb = float3(gvp[gip[gt * 3u + 1u]]);\n"
"                    float3 gvc = float3(gvp[gip[gt * 3u + 2u]]);\n"
"                    float3 gn = normalize(cross(gvb - gva, gvc - gva));\n"
"                    if (dot(gn, gd) > 0.0f) gn = -gn;\n"          // orient toward the incoming ray
"                    float3 gp = gr.origin + gd * gres.distance;\n"
"                    float gjs = RT_SELRAND(gid, cam.frame, 4u, cam.history);\n"
"                    float GW = 0.0f; float gpw = 0.0f; float ggw = 0.0f; bool ghave = false;\n"
"                    float3 glp = float3(0.0f); float3 glc = float3(0.0f);\n"
// THE TILE-LIST FALLBACK (rt_metal_gi_fallback, GIARC G4-3, 2026-08-30).
// The pick below ranges over the PRIMARY pixel's tile survivors, and G4's own
// measurement showed what that costs: on 20-40% of bounce hits NO light in that
// list reaches the bounce point at all, so `ghave` stays false and the hit
// contributes EXACTLY ZERO bounce light -- measured over nine frames on four
// beds, with the shortfall channel reading p50 0.9-1.0, i.e. in most of those
// cases the best in-tile light delivers nothing. That is why the bounce reads as
// a modest lift rather than filling a room.
//
// Pass 1 re-runs the IDENTICAL reservoir over all nl staged lights, and only
// when pass 0 found nothing. That is safe and unbiased by construction: `ghave`
// is false exactly when no light passed the `br > 0` test, which means GW is
// still 0 and gjs is untouched, so the second pass starts from a pristine
// reservoir over the full list and its 1/p weight is the full list's. It is also
// targeted -- it fires only on hits that currently contribute zero, so nothing
// that already works changes, and the surface pick above is untouched.
//
// The loop body is written ONCE, on purpose: the reservoir arithmetic is subtle
// (the surviving uniform is rescaled onto [0,1) after each candidate, which is
// what makes the single-uniform scheme exact rather than approximate) and a
// hand-copied second version is a lockstep pair waiting to drift.
"                    for (uint gpass = 0u; gpass < 2u; ++gpass) {\n"
"                        if (gpass == 1u && (ghave || cam.gifallback == 0u)) break;\n"
"                    uint gn_lights = (gpass == 0u) ? (usedil ? gtcount : tcount) : nl;\n"
"                    for (uint ii = 0u; ii < gn_lights; ++ii) {\n"
"                        uint li = (gpass == 0u) ? (usedil ? uint(gtlist[ii]) : tlist[ii]) : ii;\n"
"                        float3 lp = float3(tgl[li*RTL+0u], tgl[li*RTL+1u], tgl[li*RTL+2u]);\n"
"                        float lr = tgl[li*RTL+3u];\n"
"                        float3 tl = lp - gp; float d2 = dot(tl, tl);\n"
"                        if (d2 >= lr*lr) continue;\n"
"                        float dl = sqrt(d2);\n"
"                        float ndl = dot(gn, tl / dl); if (ndl <= 0.0f) continue;\n"
"                        float fall = 1.0f - dl / lr; fall *= fall;\n"
"                        fall *= rt_cone(float3(tgl[li*RTL+8u], tgl[li*RTL+9u], tgl[li*RTL+10u]), tgl[li*RTL+11u], tgl[li*RTL+12u], tgl[li*RTL+13u], -tl / dl);\n"
"                        float3 lcol = float3(tgl[li*RTL+4u], tgl[li*RTL+5u], tgl[li*RTL+6u]);\n"
"                        float br = (lcol.r + lcol.g + lcol.b) * ndl * fall;\n"
"                        if (br <= 0.0f) continue;\n"
// The one-pass weighted pick, the surface pick's exact single-uniform scheme.
"                        GW += br;\n"
"                        float pick = gjs * GW;\n"
"                        if (pick <= br) { gjs = pick / br; glp = lp; glc = lcol; ggw = ndl * fall; gpw = br; ghave = true; }\n"
"                        else { float rem = GW - br; gjs = (rem > 0.0f) ? ((pick - br) / rem) : 0.0f; }\n"
"                    }\n"
"                    }\n"
// GIARC G4 item 4, MEASURE-ONLY (RT_METAL_GIBIAS=1). The pick above ranged
// over the PRIMARY pixel's tile survivors; this ranges over ALL nl lights
// staged in tgl -- already in threadgroup memory, loaded BEFORE the cull,
// so the measurement needs no new binding. Tile membership is tested by
// scanning tlist rather than tflags, so the answer is right under
// RT_NOTILE too. R = a valid bounce sample (the denominator); G = its
// brightest REACHING light was outside the tile list; B = the relative
// shortfall, which is what turns a count into a magnitude -- a bias that
// only misses lights no brighter than the ones it did see does not matter.
"                    if (cam.gbias == 1u) {\n"
"                        float allbr = 0.0f; float tilebr = 0.0f; uint allli = 0u; bool allhave = false;\n"
"                        for (uint ai = 0u; ai < nl; ++ai) {\n"
"                            float3 lp = float3(tgl[ai*RTL+0u], tgl[ai*RTL+1u], tgl[ai*RTL+2u]);\n"
"                            float lr = tgl[ai*RTL+3u];\n"
"                            float3 tl = lp - gp; float d2 = dot(tl, tl);\n"
"                            if (d2 >= lr*lr) continue;\n"
"                            float dl = sqrt(d2);\n"
"                            float ndl = dot(gn, tl / dl); if (ndl <= 0.0f) continue;\n"
"                            float fall = 1.0f - dl / lr; fall *= fall;\n"
"                            fall *= rt_cone(float3(tgl[ai*RTL+8u], tgl[ai*RTL+9u], tgl[ai*RTL+10u]), tgl[ai*RTL+11u], tgl[ai*RTL+12u], tgl[ai*RTL+13u], -tl / dl);\n"
"                            float3 lcol = float3(tgl[ai*RTL+4u], tgl[ai*RTL+5u], tgl[ai*RTL+6u]);\n"
"                            float br = (lcol.r + lcol.g + lcol.b) * ndl * fall;\n"
"                            if (br <= 0.0f) continue;\n"
"                            if (!allhave || br > allbr) { allbr = br; allli = ai; allhave = true; }\n"
"                            bool intile = false;\n"
"                            uint pc = usedil ? gtcount : tcount;\n"
"                            for (uint ti = 0u; ti < pc; ++ti) { uint pl = usedil ? uint(gtlist[ti]) : tlist[ti]; if (pl == ai) { intile = true; break; } }\n"
"                            if (intile && br > tilebr) tilebr = br;\n"
"                        }\n"
"                        if (allhave) {\n"
"                            bool bestintile = false;\n"
"                            uint pc2 = usedil ? gtcount : tcount;\n"
"                            for (uint ti = 0u; ti < pc2; ++ti) { uint pl = usedil ? uint(gtlist[ti]) : tlist[ti]; if (pl == allli) { bestintile = true; break; } }\n"
"                            gdbg = float3(1.0f, bestintile ? 0.0f : 1.0f, clamp((allbr - tilebr) / allbr, 0.0f, 1.0f));\n"
"                        }\n"
"                    }\n"
"                    if (ghave && gpw > 0.0f) {\n"
"                        float3 d = glp - gp; float sd = length(d); d /= sd;\n"
"                        ray sr;\n"
"                        sr.origin = gp + gn * 0.75f;\n"
"                        sr.direction = d;\n"
"                        sr.min_distance = 0.0f;\n"
"                        sr.max_distance = max(sd - 1.0f, 0.0f);\n"   // the house interval clamp
"                        intersector<triangle_data, instancing> gsi;\n"
"                        gsi.set_triangle_cull_mode(triangle_cull_mode::none);\n"
"                        gsi.accept_any_intersection(true);\n"
"                        if (gsi.intersect(sr, accel, 0x3u).type == intersection_type::none) {\n"
// COLOURED BOUNCE (rt_metal_gi_albedo_tex, G4-2): at a WORLD bounce hit the
// reflectance blends from the constant to that triangle's own mean texture
// colour, so a red brick corridor bleeds red light into its corners instead
// of grey-warm. The blend REPLACES the constant rather than multiplying it
// (avgcolor already IS the albedo; stacking it on gialbedo would fold the
// albedo twice and dim everything). Entity hits (instance 1) keep the
// constant -- the entity gather is per-frame churn and carries no colour
// stream; stated in the cvar help. 0 = the old bytes (branch untaken).
"                            float3 grho = float3(cam.gialbedo);\n"
"                            if (cam.gialbtex > 0.0f && gres.instance_id == 0u) {\n"
"                                uchar4 wa = walb[gt];\n"
"                                grho = mix(grho, float3(wa.x, wa.y, wa.z) * (1.0f / 255.0f), cam.gialbtex);\n"
"                            }\n"
"                            gib = glc * (ggw * (GW / gpw)) * grho;\n"   // unbiased: picked contribution / its probability
"                        }\n"
"                    }\n"
"                }\n"
"            }\n"
"        }\n"
"        // TEMPORAL ACCUMULATION of the visibility fraction (the denoise target):\n"
"        // reproject this world hit into the PREVIOUS frame's screen and blend with\n"
"        // history (exponential moving average). World-position validation rejects\n"
"        // disocclusions and moving occluders, so those fall back to the current\n"
"        // (noisier) estimate instead of ghosting. This averages out the per-frame\n"
"        // sample jitter -> smooth penumbra + no screen-locked shower-door shimmer.\n"
"        float va = vis;\n"
"        float3 seca = sec;\n"
"        float3 gia = gib;\n"
"        float goa = gob;\n"   // BEAUTY B1: the occlusion follows gia through every path below
// G4-1: a rate-skipped pixel's baseline is its own previous value (an
// identity hold), NOT the zero gib -- and this must live OUTSIDE the
// reprojection block below, because that block is gated on cam.history > 0
// (the VISIBILITY history) and the hold promise has to survive that cvar
// being off. Measured before this line existed: at rt_metal_history 0 a
// rate-2 boot wrote S,0,S,0 per pixel and two boots differed by 68.8% of
// the frame purely on frame parity -- identically on GL and MT, the
// shared-sidecar signature reading a real defect. The reprojected read
// below still overrides this wherever the history machinery is live.
// giIn always holds last frame's write (every kernel path writes it, the
// G1 rule) -- EXCEPT the very first trace after creation/resize, when the
// other ping-pong slot has never been written; hasPrev is 0 exactly then,
// so the guard leaves those pixels at zero for that one frame.
"        if (cam.gi != 0u && !gifire && cam.hasPrev != 0u) { float4 gh0 = giIn.read(gid); gia = gh0.rgb; goa = gh0.a; }\n"
"        if (cam.history > 0.0f && cam.hasPrev != 0u) {\n"
"            float3 rel = hit - float3(cam.pOrigin);\n"
"            float tz = dot(rel, float3(cam.pForward));\n"
"            if (tz > 0.0625f) {\n"                                     // in front of the previous camera
"                float sxr = dot(rel, float3(cam.pRight)) / tz;\n"
"                float syr = dot(rel, float3(cam.pUp)) / tz;\n"
"                float fx = ((sxr / cam.pTanx) + 1.0f) * 0.5f * float(cam.w) - 0.5f;\n"
"                float fy = ((syr / cam.pTany) + 1.0f) * 0.5f * float(cam.h) - 0.5f;\n"
"                // bound-check in the FLOAT domain before converting (out-of-range\n"
"                // float->int is undefined; a grazing hit can push fx far off-screen)\n"
"                if (fx >= -0.5f && fx < float(cam.w) - 0.5f && fy >= -0.5f && fy < float(cam.h) - 0.5f) {\n"
"                    float4 H = histIn.read(uint2(uint(round(fx)), uint(round(fy))));\n"
"                    float footprint = tz * 2.0f * cam.pTanx / float(cam.w);\n"  // world units / pixel at that depth
"                    float thr = max(1.0f, footprint * 3.0f);\n"
"                    if (length(H.xyz - hit) < thr) {\n"                         // same surface point -> reuse history
"                        float dd = fabs(vis - H.w);\n"                          // large change (moving shadow) -> trust current more
"                        float wgt = cam.history * (1.0f - smoothstep(0.35f, 0.85f, dd));\n"
"                        va = mix(vis, H.w, wgt);\n"
// The secondary rides the SAME reprojection and world-position validation. It is
// the estimate that needs denoising most -- one non-dominant light is sampled per
// pixel per frame -- and its history weight is the plain cam.history: dd above is
// the DOMINANT light's change and says nothing about this one.
"                        if (cam.lsample != 0u) seca = mix(sec, secIn.read(uint2(uint(round(fx)), uint(round(fy)))).rgb, cam.history);\n"
// The GI colour rides the SAME reprojection and world-position validation
// (GIARC G1). Its history weight is its OWN cam.gihistory -- deep by default,
// GI is low-frequency and one sample per pixel per frame needs it -- and dd
// above is the dominant light's change, which says nothing about bounce light.
// G4-1: a rate-skipped pixel took no sample this frame, so its history passes
// through UNCHANGED -- gib is zero there, and mixing it in would drain the
// EMA to black at the skip rate (the trap the G4 plan pre-registered). A
// skipped pixel whose reprojection FAILED (disocclusion, off-screen) still
// falls through to gia = gib = 0 -- up to rate-1 frames of missing bounce at
// a freshly exposed silhouette, the stated cost of the feature, judged by
// the eye like everything temporal.
"                        if (cam.gi != 0u) { float4 gh1 = giIn.read(uint2(uint(round(fx)), uint(round(fy)))); gia = gifire ? mix(gib, gh1.rgb, cam.gihistory) : gh1.rgb; goa = gifire ? mix(gob, gh1.a, cam.gihistory) : gh1.a; }\n"
"                    }\n"
"                }\n"
"            }\n"
"        }\n"
"        histOut.write(float4(hit, va), gid);\n"
"        secOut.write(float4(seca, 0.0f), gid);\n"                // store world pos + accumulated visibility
"        giOut.write(float4(gia, (cam.giao > 0.0f) ? goa : 0.0f), gid);\n"   // zeros when gi is off, so an enable ramps from black, never from garbage; B1: the occlusion in alpha, 0 at giao 0 (the old bytes)
"        if (cam.walllight > 0.0f) {\n"
"            // FULL RT WALL LIGHTING: the scene is rendered fullbright (albedo), so the\n"
"            // composite multiplies albedo * this term to become the final shading. Output\n"
"            // = ambient fill + every in-range light's per-pixel diffuse (Lsum), with the\n"
"            // DOMINANT light soft-shadowed: its occluded fraction blc*bg*(1-va) is\n"
"            // removed, scaled by (1 - darkness) so rt_metal_darkness is the shadow\n"
"            // floor in this mode too -- 0 = full-depth shadows (the old behaviour,\n"
"            // bit-exact), 1 = no darkening. Dynamic lights are already in Lsum, so\n"
"            // they light walls here for free. The 6x normalises the modest exported\n"
"            // light colours so walllight~1 gives a lightmap-like brightness.\n"
// The secondary subtraction is CLAMPED to the light left after the dominant's own
// shadow: the estimator is unbiased, but one sample divided by its probability can
// overshoot the true non-dominant sum, and the composite MULTIPLIES this term into
// the scene -- an overshoot would print as a black bruise rather than as noise.
// Clamping the SUBTRACTION leaves the old expression exact when seca is zero.
"            float3 dom = blc * bg * (1.0f - cam.darkness) * (1.0f - va);\n"
"            float3 sub = min(seca * (1.0f - cam.darkness), max(Lsum - dom, float3(0.0f)));\n"
// Clamped for the same reason the estimator above is: the composite MULTIPLIES
// this term, so nothing may drive it negative. dom2 is exact rather than
// estimated, but the three subtractions share one budget.
"            float3 sub2 = min(dom2, max(Lsum - dom - sub, float3(0.0f)));\n"
"            float3 Lamb = float3(cam.ambient);\n"
"            if (cam.giao > 0.0f) Lamb *= (1.0f - cam.giao * goa);\n"   // BEAUTY B1: AO darkens the sourceless fill only; a uniform branch, so 0 is the old expression
"            float3 Lrt = Lamb + (cam.walllight * 6.0f) * (Lsum - dom - sub - sub2);\n"
// ONE-BOUNCE GI composes here (rt_metal_gi): beside the ambient fill in role,
// inside the walllight*6 scale in units, BEFORE the saturation mix and the
// lmax shoulder so the whole tone chain applies. The uniform branch keeps
// gi 0 the old expression textually -- the rt_metal_color / lmax house shape.
"            if (cam.gi != 0u) Lrt += (cam.walllight * 6.0f) * (cam.giintensity * gia);\n"
"            col = max(Lrt, float3(0.0f));\n"
"            // rt_metal_color doubles as the wall-lighting saturation knob: 1 is\n"
"            // neutral (bit-exact old look, hence the uniform branch), below greys\n"
"            // the light, above exaggerates hue. Clamped again after the mix --\n"
"            // extrapolation past 1 can push a channel negative, and the composite\n"
"            // would multiply that into the scene as negative light.\n"
"            if (cam.colorstr != 1.0f) {\n"
"                float lum = dot(col, float3(0.299f, 0.587f, 0.114f));\n"
"                col = max(mix(float3(lum), col, cam.colorstr), float3(0.0f));\n"
"            }\n"
"            // SOFT CEILING (rt_metal_lmax, 2026-08-16). The term above has a FLOOR and\n"
"            // no ceiling: Lsum is a raw sum over every in-range light and walllight*6\n"
"            // scales it, so a surface beside its own fixture reads 5-7 while the frame's\n"
"            // median sits at 1-2 (measured on Seb's demo15: p50 1.0-2.0, p90 2.7, p95\n"
"            // 3.2, p99 4-5, max 5.1-7.5). Multiplied into fine albedo detail -- a wall's\n"
"            // texels beside a switch, a fiend's dorsal ridge -- that goes far past white,\n"
"            // EDR presents it above white and the MetalFX HDR scaler overshoots it into\n"
"            // the bright single pixels QA reported at silhouettes and fixtures. Below\n"
"            // the knee nothing changes (bit-exact); above it a max-channel Reinhard\n"
"            // shoulder (the DPD_SHOULDER house shape, hue-preserving) asymptotes at\n"
"            // knee * 5/3, so 3.3 -> 3.1, 5 -> 4.0, 7.5 -> 4.4 at knee 2.5. Placed AFTER\n"
"            // the saturation mix so rt_metal_color cannot push a channel back over it.\n"
"            // 0 = no ceiling, the old bytes exactly (the branch is not taken).\n"
"            if (cam.lmax > 0.0f) {\n"
"                float lm = max(col.r, max(col.g, col.b));\n"
"                if (lm > cam.lmax) {\n"
"                    float e = lm - cam.lmax;\n"
"                    float k = cam.lmax * (2.0f / 3.0f);\n"
"                    col *= (cam.lmax + e * k / (k + e)) / lm;\n"
"                }\n"
"            }\n"
"        } else {\n"
"        // Colored composite term (HDR, RGBA16F surface): term = mix(shadow, lit, va).\n"
"        // Shadow end = the grey darkness floor. Lit end = white PLUS, for a DYNAMIC\n"
"        // dominant light only, an additive brighten toward that light's colour\n"
"        // (colorstr * geometry * lightColor) so explosions / rocket & muzzle flashes\n"
"        // actually FLARE the room in their real colour, gated by RT visibility so the\n"
"        // flare carries the moving shadow. Static lights keep litRGB = 1 (their glow\n"
"        // is already in the baked lightmap; adding it again would wash the level out),\n"
"        // so static-lit areas and shadows reproduce the previous grey behaviour.\n"
"        float3 litRGB = float3(1.0f);\n"
"        if (bdyn) litRGB += cam.colorstr * bg * blc;\n"
"        float3 shadowRGB = float3(cam.darkness);\n"
"        col = mix(shadowRGB, litRGB, va);\n"
"        }\n"                                                  // end over-lightmap vs wall-lighting branch
"    } else {\n"
"        col = float3(1.0f, 1.0f, 1.0f);\n"                     // sky / no hit: leave the scene unchanged
"        histOut.write(float4(1.0e18f, 1.0e18f, 1.0e18f, 1.0f), gid);\n"
"        secOut.write(float4(0.0f), gid);\n"  // sky sentinel: far pos (rejected on reuse), vis=1
"        giOut.write(float4(0.0f), gid);\n"
"    }\n"
// ALPHA CARRIES THE HIT DISTANCE. It used to be a constant 1.0 and every
// consumer reads .rgb (the GL composite, and the RT-liquids surface path in
// shader_glsl.h), so the channel was free. The composite uses it to make its
// reprojection translation-aware -- see kCompFS. 0 means sky or no hit, which
// tells the composite there is nothing to refine against.
// RT_METAL_GIBIAS (measure-only): the term buffer carries the classification
// instead of the lighting term, so RT_METAL_TERMDUMP reads it out through
// the readback path that already exists.
"    if (cam.gbias != 0u) col = gdbg;\n"
"    outtex.write(float4(col, hitw ? res.distance : 0.0f), gid);\n"
"}\n";

// GOD RAYS: a second, smaller kernel marching K jittered points along each pixel's
// view ray, evaluating the tile-culled light list at each point (falloff only — fog
// has no surface, so no N·L) and shadow-testing the locally DOMINANT light with one
// any-hit ray. The mean in-scattered colour goes to a half-float surface that the
// GL volumetric murk consumes as an additive source term. It reads THIS frame's
// history texture (written by rt_trace earlier in the same command buffer — Metal
// hazard-tracks encoder order) for the per-pixel hit distance, so it never traces
// primary rays of its own.
static const char *kShaftSrc =
"#include <metal_stdlib>\n"
"#include <metal_raytracing>\n"
"using namespace metal;\n"
"using namespace raytracing;\n"
"struct ShaftCam {\n"
"    packed_float3 origin;\n"
"    packed_float3 forward;\n"
"    packed_float3 right;\n"
"    packed_float3 up;\n"
"    float tanx;\n"
"    float tany;\n"
"    uint w;\n"          // shaft grid size
"    uint h;\n"
"    uint fullw;\n"      // history texture (RT viewport) size
"    uint fullh;\n"
"    uint frame;\n"
"    uint numLights;\n"
"    uint samples;\n"    // K points along each ray (1-16)
"    float dist;\n"      // max shaft distance; also the ray length against sky
"    float history;\n"   // temporal blend weight (0 = off)
"    float residual;\n"  // weight of the UNSHADOWED non-dominant lights
"    uint hasPrev;\n"    // previous shaft output valid
"    packed_float3 pForward;\n"   // previous-frame camera basis: the EMA texture was
"    packed_float3 pRight;\n"     // written through it, so the read is reprojected
"    packed_float3 pUp;\n"
"    float pTanx;\n"
"    float pTany;\n"
"    uint hasPrevCam;\n"          // 0 = same-pixel blend (reprojection off / no basis)
"    uint lsample;\n"             // rt_metal_lightsample: stochastic light pick instead of dominant-only
"    float wclamp;\n"             // rt_metal_lightsample_clamp: 1/p weight ceiling (0 = unclamped, the old bytes; appended 2026-08-29)
"};\n"
"kernel void rt_shaft(texture2d<float, access::write> outtex [[texture(0)]],\n"
"                     texture2d<float, access::read> hist [[texture(1)]],\n"       // THIS frame's history: world hit pos
"                     texture2d<float, access::read> prevShaft [[texture(2)]],\n"  // previous shaft output (temporal)
"                     instance_acceleration_structure accel [[buffer(0)]],\n"
"                     constant ShaftCam &cam [[buffer(1)]],\n"
"                     device const float *lights [[buffer(2)]],\n"
"#if RT_BLUENOISE\n"
"                     device const uchar *bn [[buffer(3)]],\n"
"#endif\n"
"                     uint2 gid [[thread_position_in_grid]],\n"
"                     uint tid [[thread_index_in_threadgroup]],\n"
"                     uint2 tgs [[threads_per_threadgroup]],\n"
"                     uint sgi [[simdgroup_index_in_threadgroup]],\n"
"                     uint lane [[thread_index_in_simdgroup]])\n"
"{\n"
"    threadgroup float tgl[256 * RTL];\n"
"    threadgroup atomic_uint tflags[8];\n"
"    threadgroup uint tlist[256];\n"
"    threadgroup uint tcount;\n"
"    threadgroup float3 rmin[8], rmax[8];\n"
"    uint nl = min(cam.numLights, 256u);\n"
"    uint tgcount = tgs.x * tgs.y;\n"
"    for (uint k = tid; k < nl * RTL; k += tgcount) tgl[k] = lights[k];\n"
"    for (uint k = tid; k < 8u; k += tgcount) atomic_store_explicit(&tflags[k], 0u, memory_order_relaxed);\n"
"    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"    bool inbounds = (gid.x < cam.w && gid.y < cam.h);\n"
"    float sx = (2.0f * (float(gid.x) + 0.5f) / float(cam.w) - 1.0f) * cam.tanx;\n"
"    float sy = (2.0f * (float(gid.y) + 0.5f) / float(cam.h) - 1.0f) * cam.tany;\n"
"    float3 ro = float3(cam.origin);\n"
"    float3 rd = normalize(float3(cam.forward) + sx * float3(cam.right) + sy * float3(cam.up));\n"
"    // ray length from this frame's primary-hit history (point-sampled); the sky\n"
"    // sentinel (1e18) becomes 'march to the shaft distance', so beams cross sky\n"
"    uint2 hp = uint2(min((gid.x * cam.fullw) / max(cam.w, 1u), cam.fullw - 1u),\n"
"                     min((gid.y * cam.fullh) / max(cam.h, 1u), cam.fullh - 1u));\n"
"    float4 H = hist.read(hp);\n"
"    float enddist = (H.x > 1.0e17f) ? cam.dist : min(length(H.xyz - ro), cam.dist);\n"
"    float3 ep = ro + rd * enddist;\n"
"    // Tile AABB over ray ENDPOINTS — then the camera origin is folded in at the\n"
"    // reduce, so the box bounds every camera->endpoint segment in the tile. That is\n"
"    // what makes the cull valid for lights anywhere along the march, not just at\n"
"    // the wall. No sky-tile skip: sky pixels have real endpoints here.\n"
"    float3 pmin = inbounds ? ep : float3( 1.0e30f);\n"
"    float3 pmax = inbounds ? ep : float3(-1.0e30f);\n"
"    pmin = simd_min(pmin);\n"
"    pmax = simd_max(pmax);\n"
"    if (lane == 0u) { rmin[sgi] = pmin; rmax[sgi] = pmax; }\n"
"    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"    if (tid == 0u) {\n"
"        uint nsg = (tgcount + 31u) / 32u;\n"
"        float3 bmin = rmin[0]; float3 bmax = rmax[0];\n"
"        for (uint s = 1u; s < nsg; ++s) { bmin = min(bmin, rmin[s]); bmax = max(bmax, rmax[s]); }\n"
"        rmin[0] = min(bmin, ro); rmax[0] = max(bmax, ro);\n"
"    }\n"
"    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"    float3 bmin = rmin[0]; float3 bmax = rmax[0];\n"
"    for (uint li = tid; li < nl; li += tgcount) {\n"
"        float3 lp = float3(tgl[li*RTL+0u], tgl[li*RTL+1u], tgl[li*RTL+2u]);\n"
"        float lr = tgl[li*RTL+3u];\n"
"        float3 v = clamp(lp, bmin, bmax) - lp;\n"
"        if (dot(v, v) < lr * lr)\n"
"            atomic_fetch_or_explicit(&tflags[li >> 5u], 1u << (li & 31u), memory_order_relaxed);\n"
"    }\n"
"    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"    if (tid == 0u) {\n"
"#if RT_NOTILE\n"
"        for (uint li = 0u; li < nl; ++li) tlist[li] = li;\n"
"        tcount = nl;\n"
"#else\n"
"        uint n = 0u;\n"
"        for (uint li = 0u; li < nl; ++li)\n"
"            if (atomic_load_explicit(&tflags[li >> 5u], memory_order_relaxed) & (1u << (li & 31u))) tlist[n++] = li;\n"
"        tcount = n;\n"
"#endif\n"
"    }\n"
"    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"    if (!inbounds) return;\n"
"    uint K = clamp(cam.samples, 1u, 16u);\n"
"#if RT_BLUENOISE\n"
"    float jf = rt_bnjitter(gid, cam.frame, cam.history, bn);\n"
"#else\n"
"    float ign = fract(52.9829189f * fract(0.06711056f * float(gid.x) + 0.00583715f * float(gid.y)));\n"
"    float jf = (cam.history > 0.0f) ? fract(ign + float(cam.frame) * 0.61803399f) : ign;\n"
"#endif\n"
"    float tmax = max(enddist - 1.0f, 0.0f);\n"
"    intersector<triangle_data, instancing> si;\n"
"    si.set_triangle_cull_mode(triangle_cull_mode::none);\n"
"    si.accept_any_intersection(true);\n"
"    float3 sum = float3(0.0f);\n"
"    for (uint i = 0u; i < K; ++i) {\n"
"        float t = (float(i) + jf) / float(K) * tmax;\n"
"        float3 p = ro + rd * t;\n"
"        float3 Ls = float3(0.0f);\n"
"        float bestw = 0.0f; float3 blp = float3(0.0f); float bdl = 1.0f; float3 bc = float3(0.0f); float brad = 1.0f;\n"
"        float W2 = 0.0f; float3 slp = float3(0.0f); float sdl = 1.0f; float3 sc = float3(0.0f); float srad = 1.0f; float sw = 0.0f;\n"
"        float js = (cam.lsample != 0u) ? RT_SELSEQ(gid, cam.frame, 1u + i, cam.history) : 0.0f;\n"
"        for (uint ii = 0u; ii < tcount; ++ii) {\n"
"            uint li = tlist[ii];\n"
"            float3 lp = float3(tgl[li*RTL+0u], tgl[li*RTL+1u], tgl[li*RTL+2u]);\n"
"            float lr = tgl[li*RTL+3u];\n"
"            float3 tl = lp - p; float d2 = dot(tl, tl);\n"
"            if (d2 >= lr*lr) continue;\n"
"            float dl = max(sqrt(d2), 0.001f);\n"
"            float a = 1.0f - dl / lr; a *= a;\n"                    // same falloff as the surface pass
// SPOT CONE -- LOCKSTEP with the surface kernel above and the fog kernel below
"            a *= rt_cone(float3(tgl[li*RTL+8u], tgl[li*RTL+9u], tgl[li*RTL+10u]), tgl[li*RTL+11u], tgl[li*RTL+12u], tgl[li*RTL+13u], -tl / dl);\n"
"            float3 c = float3(tgl[li*RTL+4u], tgl[li*RTL+5u], tgl[li*RTL+6u]) * a;\n"
"            if (cam.residual > 0.0f || cam.lsample != 0u) Ls += c;\n"   // Ls feeds the residual term and the pick's total
"            float w = c.r + c.g + c.b;\n"
"            if (w > bestw) { bestw = w; blp = lp; bdl = dl; bc = c; brad = lr; }\n"
// STOCHASTIC PICK -- LOCKSTEP with the fog kernel's block: same exact
// single-uniform scheme, same 1/p weighting, same reason (one ray per sample
// point, so spend it in proportion to scatter rather than always on the loudest).
"            if (cam.lsample != 0u && w > 0.0f) {\n"
"                W2 += w;\n"
"                float pick = js * W2;\n"
"                if (pick <= w) { js = pick / w; slp = lp; sdl = dl; sc = c; srad = lr; sw = w; }\n"
"                else { float rem = W2 - w; js = (rem > 0.0f) ? ((pick - w) / rem) : 0.0f; }\n"
"            }\n"
"        }\n"
// THE 1/p CLAMP (rt_metal_lightsample_clamp, 2026-08-29; LOCKSTEP with the fog
// kernel's identical line). The surface arm has always bounded its estimator;
// this arm never did, so an unlucky pick of a dim light (p = sw/W2 small) was
// an unbounded firefly -- Seb's demo17 speckle, the blotchy flicker in thick
// fog, attributed by four single-variable A/Bs (test/flicker.py's header).
// Clamping the ratio biases dim lights DOWN (energy loss where p < 1/clamp),
// which is the stated trade; 0 = unclamped, the old expression byte for byte.
"        if (cam.lsample != 0u && sw > 0.0f) {\n"
"            float pr = W2 / sw;\n"
"            if (cam.wclamp > 0.0f) pr = min(pr, cam.wclamp);\n"
"            blp = slp; bdl = sdl; bc = sc * pr; brad = srad; bestw = sw;\n"
"        }\n"
"        if (bestw > 0.0f) {\n"
"            // Aim at a jittered point on a DISC around the light, not its centre:\n"
"            // Quake torch lights have their flame model sitting exactly at the\n"
"            // light origin, so a centre ray hits the flame geometry every time and\n"
"            // the beams black out. The disc (same trick as the surface kernel's\n"
"            // penumbra sampling) misses the small model on most samples AND gives\n"
"            // the beam edges their softness.\n"
"            float3 ld = (blp - p) / bdl;\n"
"            float R = clamp(brad * 0.15f, 8.0f, 40.0f);\n"
"            float3 up0 = (fabs(ld.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);\n"
"            float3 T = normalize(cross(up0, ld));\n"
"            float3 B = cross(ld, T);\n"
"            float rr = sqrt((float(i) + 0.5f) / float(K)) * R;\n"
"            float th = float(i) * 2.39996323f + 6.2831853f * jf;\n"
"            float3 tgt = blp + (cos(th) * rr) * T + (sin(th) * rr) * B;\n"
"            float3 d = tgt - p; float sd = length(d);\n"
"            ray sr;\n"
"            sr.origin = p;\n"                                       // a fog point: no surface, no normal offset
"            sr.direction = d / max(sd, 0.001f);\n"
"            sr.min_distance = 0.0f;\n"
"            sr.max_distance = max(sd - 1.0f, 0.0f);\n"              // stop short of the light's own geometry
"            float vis = (si.intersect(sr, accel, 0x3u).type == intersection_type::none) ? 1.0f : 0.0f;\n"
"            sum += bc * vis;\n"                                     // beams from occlusion
"            if (cam.residual > 0.0f && cam.lsample == 0u) sum += cam.residual * (Ls - bc);\n"   // fill superseded by the pick
"        }\n"
"    }\n"
"    float3 shaft = sum / float(K);\n"
"    if (cam.history > 0.0f && cam.hasPrev != 0u)\n"
"    {\n"
"        // REPROJECTED EMA -- same remap as the fog kernel\n"
"        uint2 pp = gid; bool okp = true;\n"
"        if (cam.hasPrevCam != 0u) {\n"
"            float tz = dot(rd, float3(cam.pForward));\n"
"            if (tz > 1.0e-4f) {\n"
"                float fx = ((dot(rd, float3(cam.pRight)) / tz) / cam.pTanx + 1.0f) * 0.5f * float(cam.w) - 0.5f;\n"
"                float fy = ((dot(rd, float3(cam.pUp)) / tz) / cam.pTany + 1.0f) * 0.5f * float(cam.h) - 0.5f;\n"
"                int ix = int(round(fx)), iy = int(round(fy));\n"
"                if (ix >= 0 && iy >= 0 && ix < int(cam.w) && iy < int(cam.h)) pp = uint2(uint(ix), uint(iy)); else okp = false;\n"
"            } else okp = false;\n"
"        }\n"
"        if (okp) shaft = mix(shaft, prevShaft.read(pp).rgb, cam.history);\n"
"    }\n"
"    outtex.write(float4(shaft, 1.0f), gid);\n"
"}\n";

// FULL IN-KERNEL FOG LIGHTING: the endgame. This kernel owns the whole volumetric
// integral -- per march step it evaluates the murk's density model (noise volume +
// baked world field) AND the tile-culled lights, with a strided disc-jittered
// shadow ray to the locally dominant light, accumulating premultiplied scattered
// light and transmittance. The GL murk pass just composites the result.
//
// THE DENSITY MODEL BELOW IS A DELIBERATE LOCKSTEP DUPLICATE of the GL murk in
// shader_glsl.h MODE_VOLUMETRICFOG (the march loop). Change one, change the other,
// or the kernel-vs-GL A/B tone match drifts. The authored colours are the unlit
// term, so the GL look is the baseline and the light ADDS to it.
static const char *kFogSrc =
"#include <metal_stdlib>\n"
"#include <metal_raytracing>\n"
"using namespace metal;\n"
"using namespace raytracing;\n"
// DPD_CURL (spliced ahead of this source from shader_density.h) spells its
// constructors vec3; alias it so the shared macro expands to MSL here. The
// kernel's own text stays float3 throughout.
"#define vec3 float3\n"
"struct FogCam {\n"
"    packed_float3 origin;\n"
"    packed_float3 forward;\n"
"    packed_float3 right;\n"
"    packed_float3 up;\n"
"    float tanx;\n"
"    float tany;\n"
"    uint w;\n"          // fog grid size
"    uint h;\n"
"    uint fullw;\n"      // history texture (RT viewport) size
"    uint fullh;\n"
"    uint frame;\n"
"    uint numLights;\n"
"    uint steps;\n"      // 2-64 march steps
"    uint stride;\n"     // shadow rays every Nth step, held between
"    float dist;\n"      // max march distance (r_volumetric_dist)
"    float history;\n"
"    float intensity;\n" // RT light gain on top of the authored tint
"    float residual;\n"  // unshadowed non-dominant weight
"    float beams;\n"     // density-INDEPENDENT god-ray strength (the old shafts look)
"    uint hasPrev;\n"
"    packed_float3 pForward;\n"   // previous-frame camera basis: the EMA texture was
"    packed_float3 pRight;\n"     // written through it, so the read is reprojected
"    packed_float3 pUp;\n"
"    float pTanx;\n"
"    float pTany;\n"
"    uint hasPrevCam;\n"          // 0 = same-pixel blend (reprojection off / no basis)
"    packed_float3 windoffset;\n"   // density model from here down (lockstep block)
"    packed_float3 color;\n"
"    packed_float3 watercolor;\n"
"    packed_float3 slimecolor;\n"
"    packed_float3 lavacolor;\n"
"    packed_float3 fieldorigin;\n"
"    packed_float3 fieldinvsize;\n"
"    float density;\n"
"    float height;\n"
"    float basez;\n"
"    float noisescale;\n"
"    float noisethresh;\n"
"    float waterdensity;\n"
"    float slimedensity;\n"
"    float lavadensity;\n"
"    float watermode;\n"
"    float flooroffset;\n"
"    float floormode;\n"
"    float watermist;\n"
"    float mistheight;\n"
"    float sdfrange;\n"
"    float corner;\n"
"    float fieldmaxh;\n"
"    float grounddensity;\n"      // GROUND FOG from here (lockstep additions)
"    float groundheight;\n"
"    float groundnoisescale;\n"
"    float groundthresh;\n"
"    float grounddeform;\n"
"    float groundoffset;\n"
"    packed_float3 groundwindoffset;\n"
"    packed_float3 groundcolor;\n"
"    float lavaglow;\n"           // warm in-air glow near lava (lockstep append)
"    float skytrans;\n"           // sky transmittance floor (1 - r_volumetric_skyfog; 0 = uncapped)
"    packed_float3 irrorigin;\n"  // AMBIENT IRRADIANCE (F5, lockstep append)
"    packed_float3 irrinvsize;\n"
"    float irrgain;\n"
"    float irrfloor;\n"
"    float irrstrength;\n"        // 0 = the self-lit murk exactly (and the honesty gate)
"    float extinction;\n"         // r_volumetric_extinction; 1 = the classic model exactly
"    float swirlamp;\n"           // KH SWIRL (F3, lockstep append): curl amplitude, wu
"    float swirlscale;\n"
"    float swirlkh;\n"            // interface-band boost
"    float swirlspare;\n"
"    float mistlavacut;\n"        // lava suppression of the surface mist band (F7; merge-serialised LAST)
"    uint histcentre;\n"          // rt_metal_fog_upsample: read the march end at the texel CENTRE (see hp below)
"    uint lsample;\n"             // rt_metal_lightsample: stochastic light pick instead of dominant-only
"    uint adstride;\n"            // rt_metal_fog_stride_adaptive: casts spread as T falls (appended 2026-08-28)
"    uint stepjitter;\n"          // rt_metal_fog_stepjitter: per-step jitter spread along the ray (S2; appended 2026-08-18)
"    float wclamp;\n"             // rt_metal_lightsample_clamp: 1/p weight ceiling (0 = unclamped, the old bytes; appended 2026-08-29)
"    uint lshybrid;\n"            // rt_metal_lightsample_hybrid: 1 = dominant always shadowed + pick over the REST (two rays), 2 = alternating casts (one ray), 3 = single-pass (the reservoir decides which half, no second loop); 0 = the pick alone, the old bytes (appended 2026-09-03)
"    uint froxel;\n"             // A1 FROXEL (2026-09-06): 1 = the RT_FROXEL build is running (informational; the arm is a preprocessor variant)
"    uint nslices;\n"            // froxel slice count (the march's step count in that build)
"    float fcurve;\n"            // exponential slice spacing exponent (0 = uniform to dist)
"    float fhistory;\n"          // per-cell temporal blend weight
"    packed_float3 pOrigin;\n"   // previous-frame EYE: the cell reprojection puts a WORLD point through the full previous camera
"    uint fhasPrev;\n"           // the other slot's volume holds a settled history under the SAME slice geometry
"    uint fcastphase;\n"         // 1 = the cast schedule advances one cell per frame, so every cell is freshly cast within stride frames (the hold bias averages out under the history)
"    float liquidfloor;\n"       // r_volumetric_liquidfloor (2026-09-07): the liquid surface as the air's floor (appended LAST)
"    float liquidlight;\n"       // BEAUTY B2 (2026-09-16): how much of the lit terms survive INSIDE a liquid (0 = suppressed, the authored look)
"};\n"
"constexpr sampler nsamp(filter::linear, mip_filter::none, address::repeat);\n"        // noise: tiling is load-bearing
"constexpr sampler fsamp(filter::linear, mip_filter::none, address::clamp_to_edge);\n" // field: matches TEXF_CLAMP
// A1 FROXEL FOG VOLUME (2026-09-06, SEPTEMBER2 A1). Compiled as a SECOND PSO of
// this same source with RT_FROXEL 1; the shipped rt_fog is RT_FROXEL 0, whose
// preprocessed text is the old kernel's exactly (the off switch is structural,
// not argued). Under RT_FROXEL the march runs over FIXED view-aligned depth
// slices -- fx_t maps slice fraction u to distance, exponential so the near
// fog (the ground layer, the corner puffs) gets most of the cells -- and each
// cell STORES its fog instead of integrating it: A = (radiance x density,
// density), B = (the held cast light for the beam term, the lava-glow kernel).
// A cell's content is a property of a WORLD point, not of a screen texel, so
// the history is reprojected per cell at the cell's own depth through the
// full previous camera (origin included) and read trilinearly -- the thing a
// 2D fog history structurally cannot do (measured 2026-09-03: any 2D history
// under translation INCREASES the error). rt_froxelintegrate then sums the
// accumulated cells front to back into the fog surface the composite already
// consumes. Cells beyond the march end are NOT written; the previous frame's
// march end (depthOut, signed: negative = sky) is what says whether a history
// tap was marched at all (the depth-aware upsample's lesson: content must
// have been marched to the place its label says).
"#if RT_FROXEL\n"
"constexpr sampler vsamp(filter::linear, mip_filter::none, address::clamp_to_edge, coord::normalized);\n"
"static inline float fx_t(float u, float dist, float a) { return (a > 0.01f) ? dist * (exp(a * u) - 1.0f) / (exp(a) - 1.0f) : dist * u; }\n"   // LOCKSTEP with fi_t in kFroxelIntegrateSrc
"static inline float fx_u(float t, float dist, float a) { return (a > 0.01f) ? log(1.0f + t * (exp(a) - 1.0f) / dist) / a : t / dist; }\n"
"#endif\n"
"kernel void rt_fog(texture2d<float, access::write> outtex [[texture(0)]],\n"
"                   texture2d<float, access::read> hist [[texture(1)]],\n"       // THIS frame's history: world hit pos
"                   texture2d<float, access::read> prevFog [[texture(2)]],\n"    // previous fog output (temporal)
"                   texture3d<float> noise [[texture(3)]],\n"
"                   texture3d<float> field [[texture(4)]],\n"
"                   texture3d<float> irrtex [[texture(5)]],\n"  // ambient irradiance grid (F5)
"                   texture2d<float, access::write> depthOut [[texture(6)]],\n"  // scatter-centroid depth (fog history reprojection, 2026-09-03)
"                   instance_acceleration_structure accel [[buffer(0)]],\n"
"                   constant FogCam &cam [[buffer(1)]],\n"
"                   device const float *lights [[buffer(2)]],\n"
"#if RT_BLUENOISE\n"
"                   device const uchar *bn [[buffer(3)]],\n"
"#endif\n"
"#if RT_FROXEL\n"
"                   texture3d<float, access::sample> prevA [[texture(7)]],\n"   // the other slot's accumulated volumes
"                   texture3d<float, access::sample> prevB [[texture(8)]],\n"
"                   texture3d<float, access::write> outA [[texture(9)]],\n"    // this slot's
"                   texture3d<float, access::write> outB [[texture(10)]],\n"
"                   texture2d<float, access::read> prevEnd [[texture(11)]],\n" // the other slot's signed march end (depthOut's froxel meaning)
"#endif\n"
"                   uint2 gid [[thread_position_in_grid]],\n"
"                   uint tid [[thread_index_in_threadgroup]],\n"
"                   uint2 tgs [[threads_per_threadgroup]],\n"
"                   uint sgi [[simdgroup_index_in_threadgroup]],\n"
"                   uint lane [[thread_index_in_simdgroup]])\n"
"{\n"
"    threadgroup float tgl[256 * RTL];\n"
"    threadgroup atomic_uint tflags[8];\n"
"    threadgroup uint tlist[256];\n"
"    threadgroup uint tcount;\n"
"    threadgroup float3 rmin[8], rmax[8];\n"
"    uint nl = min(cam.numLights, 256u);\n"
"    uint tgcount = tgs.x * tgs.y;\n"
"    for (uint k = tid; k < nl * RTL; k += tgcount) tgl[k] = lights[k];\n"
"    for (uint k = tid; k < 8u; k += tgcount) atomic_store_explicit(&tflags[k], 0u, memory_order_relaxed);\n"
"    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"    bool inbounds = (gid.x < cam.w && gid.y < cam.h);\n"
"    float sx = (2.0f * (float(gid.x) + 0.5f) / float(cam.w) - 1.0f) * cam.tanx;\n"
"    float sy = (2.0f * (float(gid.y) + 0.5f) / float(cam.h) - 1.0f) * cam.tany;\n"
"    float3 ro = float3(cam.origin);\n"
"    float3 rd = normalize(float3(cam.forward) + sx * float3(cam.right) + sy * float3(cam.up));\n"
"    // WHERE THE MARCH END COMES FROM. The ray above goes through this fog\n"
"    // texel's CENTRE; the history texel it reads its end distance from used to\n"
"    // be the one at the texel's lower-left CORNER (gid*full/w) -- up to a whole\n"
"    // history texel away from the ray. Harmless under a bilinear magnification,\n"
"    // fatal under a depth-aware one: wherever a silhouette falls between the\n"
"    // corner and the centre, the texel is a sky-direction ray marched to a\n"
"    // wall's distance (or a wall-direction ray marched through the wall to the\n"
"    // sky's), fog that belongs to NEITHER surface, and an upsample that trusts\n"
"    // its label paints it as blobs along every edge (measured: the first cut\n"
"    // of rt_metal_fog_upsample). With histcentre the history is read at the\n"
"    // texel centre, ((2*gid+1)*full)/(2*w) in exact integer arithmetic, so\n"
"    // direction and end distance come from the same place to within half a\n"
"    // history texel. The composite's tap labels use the SAME expression\n"
"    // (shader_glsl.h / shader_msl.h VolumetricTapDepth, LOCKSTEP). histcentre 0\n"
"    // is the old mapping, byte for byte.\n"
"    uint2 hp = cam.histcentre != 0u\n"
"        ? uint2(min(((2u * gid.x + 1u) * cam.fullw) / (2u * max(cam.w, 1u)), cam.fullw - 1u),\n"
"                min(((2u * gid.y + 1u) * cam.fullh) / (2u * max(cam.h, 1u)), cam.fullh - 1u))\n"
"        : uint2(min((gid.x * cam.fullw) / max(cam.w, 1u), cam.fullw - 1u),\n"
"                min((gid.y * cam.fullh) / max(cam.h, 1u), cam.fullh - 1u));\n"
"    float4 H = hist.read(hp);\n"
"    float enddist = (H.x > 1.0e17f) ? cam.dist : min(length(H.xyz - ro), cam.dist);\n"
"    float3 ep = ro + rd * enddist;\n"
"    float3 pmin = inbounds ? ep : float3( 1.0e30f);\n"
"    float3 pmax = inbounds ? ep : float3(-1.0e30f);\n"
"    pmin = simd_min(pmin);\n"
"    pmax = simd_max(pmax);\n"
"    if (lane == 0u) { rmin[sgi] = pmin; rmax[sgi] = pmax; }\n"
"    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"    if (tid == 0u) {\n"
"        uint nsg = (tgcount + 31u) / 32u;\n"
"        float3 bmin = rmin[0]; float3 bmax = rmax[0];\n"
"        for (uint s = 1u; s < nsg; ++s) { bmin = min(bmin, rmin[s]); bmax = max(bmax, rmax[s]); }\n"
"        rmin[0] = min(bmin, ro); rmax[0] = max(bmax, ro);\n"   // segment AABB: endpoints U camera
"    }\n"
"    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"    float3 bmin = rmin[0]; float3 bmax = rmax[0];\n"
"    for (uint li = tid; li < nl; li += tgcount) {\n"
"        float3 lp = float3(tgl[li*RTL+0u], tgl[li*RTL+1u], tgl[li*RTL+2u]);\n"
"        float lr = tgl[li*RTL+3u];\n"
"        float3 v = clamp(lp, bmin, bmax) - lp;\n"
"        if (dot(v, v) < lr * lr)\n"
"            atomic_fetch_or_explicit(&tflags[li >> 5u], 1u << (li & 31u), memory_order_relaxed);\n"
"    }\n"
"    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"    if (tid == 0u) {\n"
"#if RT_NOTILE\n"
"        for (uint li = 0u; li < nl; ++li) tlist[li] = li;\n"
"        tcount = nl;\n"
"#else\n"
"        uint n = 0u;\n"
"        for (uint li = 0u; li < nl; ++li)\n"
"            if (atomic_load_explicit(&tflags[li >> 5u], memory_order_relaxed) & (1u << (li & 31u))) tlist[n++] = li;\n"
"        tcount = n;\n"
"#endif\n"
"    }\n"
"    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"    if (!inbounds) return;\n"
"    uint steps = clamp(cam.steps, 2u, 64u);\n"
"#if RT_FROXEL\n"
"    steps = clamp(cam.nslices, 2u, 64u);\n"   // the slice count IS the step count: the casts, the disc index and the jitter all key on it
"    float fend = enddist;\n"                   // the effective march end this frame (the T break can pull it in)
"#endif\n"
"    uint stride = clamp(cam.stride, 1u, 8u);\n"
"    float marchlen = enddist;\n"
"    float dt = marchlen / float(steps);\n"
"#if RT_BLUENOISE\n"
"    float jf = rt_bnjitter(gid, cam.frame, cam.history, bn);\n"
"#else\n"
"    float ign = fract(52.9829189f * fract(0.06711056f * float(gid.x) + 0.00583715f * float(gid.y)));\n"
"    float jf = (cam.history > 0.0f) ? fract(ign + float(cam.frame) * 0.61803399f) : ign;\n"
"#endif\n"
"    intersector<triangle_data, instancing> si;\n"
"    si.set_triangle_cull_mode(triangle_cull_mode::none);\n"
"    si.accept_any_intersection(true);\n"
"    float T = 1.0f;\n"
"    uint nextcast = 0u;\n"   // adaptive stride: the step index of the next shadow cast
"    float3 scattered = float3(0.0f);\n"
"    float3 beamsum = float3(0.0f);\n"      // path integral for the beam term; shouldered after the loop
"    float lavasum = 0.0f;\n"               // path integral for the in-air lava glow; same treatment
"    float3 lastLight = float3(0.0f);\n"
"    float dsum = 0.0f, wsum = 0.0f;\n"   // scatter-centroid depth: extinction-weighted mean march distance
"    float hdomvis = 1.0f, hrestvis = 1.0f;\n"   // hybrid mode 2: each half's last measured visibility, held between its casts
"    for (uint i = 0u; i < steps; ++i) {\n"
"        // S2 (rt_metal_fog_stepjitter): spread the jitter ALONG the ray. One\n"
"        // ray-wide jf puts every step of this texel on the same sawtooth phase,\n"
"        // and that phase differs from the neighbour's by IGN's lattice -- so the\n"
"        // integrated error is one lattice-locked sawtooth per texel, which is\n"
"        // exactly the standing grid test/roll.py measures. A Kronecker walk\n"
"        // (golden-ratio stride) gives each step its own equidistributed phase;\n"
"        // 24 shifted sawtooths sum to far less amplitude at the lattice\n"
"        // frequency. NOT a per-step hash to white: white is what a 3x3 filter\n"
"        // attenuates least, and the speckle route has been rejected twice by eye.\n"
"        // 0 = the literal (i + jf)*dt, byte for byte.\n"
"        float jfi = (cam.stepjitter != 0u) ? fract(jf + float(i) * 0.61803399f) : jf;\n"
"#if RT_FROXEL\n"
"        float ct0 = fx_t(float(i) / float(steps), cam.dist, cam.fcurve);\n"
"        float ct1 = fx_t(float(i + 1u) / float(steps), cam.dist, cam.fcurve);\n"
"        if (ct0 >= enddist) break;\n"                 // cells past the hit are not marched and not written
"        float cvis = min(ct1, enddist) - ct0;\n"       // the cell's VISIBLE length (the last one is partial)
"        float dts = cvis;\n"
"        float t = ct0 + jfi * cvis;\n"                 // jittered within the visible part: over frames the cell averages its own density
"#else\n"
"        float dts = dt;\n"
"        float t = (float(i) + jfi) * dt;\n"
"#endif\n"
"        float3 p = ro + rd * t;\n"
"        // ---- density model: LOCKSTEP with shader_glsl.h MODE_VOLUMETRICFOG ----\n"
"        // KH SWIRL (F3) -- LOCKSTEP with shader_density.h: displace the noise\n"
"        // LOOKUP (never the field fetch) by the shared DPD_CURL field; the\n"
"        // phase rides windoffset at DPD_SWIRL_ADVECT. amp 0 = value-exact.\n"
"        float3 nwoff = p + float3(cam.windoffset);\n"
"        if (cam.swirlamp > 0.0f) {\n"
"            float3 swq = (p + float3(cam.windoffset) * DPD_SWIRL_ADVECT) * cam.swirlscale;\n"
"            nwoff += cam.swirlamp * DPD_CURL(swq);\n"
"        }\n"
"        float3 n = noise.sample(nsamp, nwoff * cam.noisescale).rgb;\n"
"        float4 world = field.sample(fsamp, (p - float3(cam.fieldorigin)) * float3(cam.fieldinvsize));\n"
"        float abovefloor = world.r * cam.fieldmaxh - cam.flooroffset;\n"
"        // LIQUID FLOOR (r_volumetric_liquidfloor, 2026-09-07), LOCKSTEP with\n"
"        // shader_density.h DPD_DENSITY_MODEL: a liquid surface is the floor of\n"
"        // the air above it, else the air murk vanishes over every lake and steps\n"
"        // at the waterline. Uniform branch: 0 is the old expression textually.\n"
"        float surfdist = (world.g - 0.5f) * 2.0f * cam.sdfrange;\n"
"        if (cam.liquidfloor > 0.0f)\n"
"            abovefloor = min(abovefloor, mix(abovefloor, max(-surfdist, 0.0f), cam.liquidfloor));\n"
"        float abovecam = p.z - cam.basez;\n"
"        float above = mix(abovecam, abovefloor, cam.floormode) + (0.5f - n.g) * cam.height * 0.6f;\n"
"        float hfall = exp(-max(above, 0.0f) / cam.height);\n"
"        float patch = smoothstep(cam.noisethresh, 1.0f, n.r);\n"
"        float density = cam.density * hfall * patch;\n"
"        density *= 1.0f + cam.corner * world.a * world.a;\n"
"        // GROUND FOG (dry ice) -- LOCKSTEP with shader_glsl.h MODE_VOLUMETRICFOG\n"
"        // (the rationale comment lives there); grounddensity is pre-gated by the\n"
"        // fill sites (floor cvar + a live baked field)\n"
"        float ground = 0.0f;\n"
"        if (cam.grounddensity > 0.0f) {\n"
"            float nb = n.b;\n"   // the air fetch's third seed: same texel, no extra fetch (lockstep with the GL march)
"            // KH SWIRL on the ground billow -- LOCKSTEP with shader_density.h:\n"
"            // own wind phase, Gaussian boost at the mist/air interface, the\n"
"            // boost gated on floormode (abovefloor is garbage at _floor 0)\n"
"            float3 gwoff = p + float3(cam.groundwindoffset);\n"
"            if (cam.swirlamp > 0.0f) {\n"
"                float3 gswq = (p + float3(cam.groundwindoffset) * DPD_SWIRL_ADVECT) * cam.swirlscale;\n"
"                float kh = (abovefloor - cam.groundoffset - cam.groundheight) / max(cam.groundheight, 1.0f);\n"
"                gwoff += (cam.swirlamp * (1.0f + cam.swirlkh * cam.floormode * exp(-kh * kh))) * DPD_CURL(gswq);\n"
"            }\n"
"            float2 gn = noise.sample(nsamp, gwoff * cam.groundnoisescale).rg;\n"
"            float gabove = abovefloor - cam.groundoffset + (0.5f - nb) * cam.grounddeform;\n"
"            float gfall = exp(-max(gabove, 0.0f) / cam.groundheight);\n"
"            float gpatch = smoothstep(cam.groundthresh, 1.0f, gn.r) * (0.75f + 0.5f * gn.g);\n"
"            ground = cam.grounddensity * gfall * gpatch;\n"
"            density += ground;\n"
"        }\n"
"        // (surfdist is declared beside abovefloor above: the liquid floor reads it)\n"
"        // MIST OVER LAVA (r_volumetric_mistlavacut): the cut scales the mist\n"
"        // band away where the nearest liquid is lava, exactly as the shared\n"
"        // text spells it (shader_density.h, LOCKSTEP). This line was the\n"
"        // kernel's ONLY missing consumer of cam.mistlavacut -- the float was\n"
"        // plumbed through all five ABI sites and read by nobody, so the cut\n"
"        // worked on the GL march and not on Seb's own (kernel) path.\n"
"        float lavakind = smoothstep(0.75f, 1.0f, world.b);\n"
"        density += cam.watermist * exp(-max(-surfdist, 0.0f) / cam.mistheight) * (1.0f - cam.mistlavacut * lavakind);\n"
"        float inliquid = smoothstep(0.49f, 0.51f, world.g) * cam.watermode;\n"
"        // LAVA GLOW (r_volumetric_lavaglow): warm emission the AIR picks up\n"
"        // near lava, keyed on the field's kind channel (B holds the NEAREST\n"
"        // span's kind even in dry cells) and the signed distance. A path\n"
"        // integral shouldered after the march like the beams. LOCKSTEP with\n"
"        // shader_glsl.h MODE_VOLUMETRICFOG -- change one, change both.\n"
"        lavasum += T * lavakind * exp(-max(-surfdist, 0.0f) / DPD_LAVAGLOW_H) * (1.0f - inliquid) * dts * (1.0f / DPD_LAVAGLOW_REF);\n"
"        float3 liquidtint = world.b < 0.25f ? float3(cam.watercolor)\n"
"                          : (world.b < 0.75f ? float3(cam.slimecolor) : float3(cam.lavacolor));\n"
"        // PER-LIQUID DENSITY -- the same kind selection liquidtint just made,\n"
"        // LOCKSTEP with shader_density.h DPD_DENSITY_MODEL. All three are equal\n"
"        // unless r_volumetric_slimedensity/_lavadensity are set off -1.\n"
"        float liqdens = world.b < 0.25f ? cam.waterdensity\n"
"                      : (world.b < 0.75f ? cam.slimedensity : cam.lavadensity);\n"
"        density = mix(density, liqdens, inliquid);\n"
"        float3 tint = mix(float3(cam.color), liquidtint, inliquid);\n"
"        tint = mix(tint, float3(cam.groundcolor), (1.0f - inliquid) * ground / max(density, 1e-4f));\n"
"        // AMBIENT IRRADIANCE (r_volumetric_ambient) -- LOCKSTEP with\n"
"        // shader_density.h: air + ground tint only (liquid colours untouched),\n"
"        // *2 undoes the grid's 128-equals-lit storage, min() caps at 1 so\n"
"        // ambient only DARKENS the authored base -- the RT light term below\n"
"        // stays the thing that ADDS. irrstrength 0 = the old bytes exactly.\n"
"        float3 irr = irrtex.sample(fsamp, (p - float3(cam.irrorigin)) * float3(cam.irrinvsize)).rgb;\n"
"        tint *= mix(min(float3(cam.irrfloor) + cam.irrgain * (irr * 2.0f), float3(1.0f)), float3(1.0f), max(inliquid, 1.0f - cam.irrstrength));\n"
"        // ---- per-step lighting: re-evaluated at the scheduled cast steps, held between ----\n"
"        // ADAPTIVE STRIDE (rt_metal_fog_stride_adaptive): deep in the fog the\n"
"        // held light's update rate is invisible (every contribution is\n"
"        // multiplied by T) but its shadow ray is full price, so the schedule\n"
"        // widens as T falls -- x2 below 50%, x4 below 25%. At adstride 0 the\n"
"        // scheduler degenerates to exactly the old (i %% stride) == 0 walk:\n"
"        // nextcast advances by the fixed stride from 0. The disc-radius index\n"
"        // kk below keeps its i/stride form -- under the adaptive schedule the\n"
"        // outer disc is undersampled only where T has already muted the\n"
"        // result, and keeping it makes adstride 0 byte-exact.\n"
"        uint effstride = (cam.adstride != 0u) ? ((T < 0.25f) ? (stride * 4u) : ((T < 0.5f) ? (stride * 2u) : stride)) : stride;\n"
"        if (i >= nextcast) {\n"
"#if RT_FROXEL\n"
// CAST PHASE (2026-09-06): with fixed cells a fixed schedule always holds the
// SAME cast point over the SAME cells, and at stride 6 that read +2.5% against
// the truth (the hold spans ~200 units far out). Shifting the schedule by
// (frame % stride) after the first cast -- cell 0 is always cast, so the near
// fog is never black -- gives every cell a fresh cast within stride frames,
// and the per-cell history then averages the hold error out instead of
// converging on it. The adaptive stride still widens the schedule as T falls.
"            nextcast = i + effstride - ((i == 0u && cam.fcastphase != 0u) ? (cam.frame % stride) : 0u);\n"
"#else\n"
"            nextcast = i + effstride;\n"
"#endif\n"
"            float3 Ls = float3(0.0f);\n"
"            float bestw = 0.0f; float3 blp = float3(0.0f); float bdl = 1.0f; float3 bc = float3(0.0f); float brad = 1.0f;\n"
"            float W2 = 0.0f; float3 slp = float3(0.0f); float sdl = 1.0f; float3 sc = float3(0.0f); float srad = 1.0f; float sw = 0.0f;\n"
"            float js = (cam.lsample != 0u) ? RT_SELSEQ(gid, cam.frame, 1u + i, cam.history) : 0.0f;\n"
"            for (uint ii = 0u; ii < tcount; ++ii) {\n"
"                uint li = tlist[ii];\n"
"                float3 lp = float3(tgl[li*RTL+0u], tgl[li*RTL+1u], tgl[li*RTL+2u]);\n"
"                float lr = tgl[li*RTL+3u];\n"
"                float3 tl = lp - p; float d2 = dot(tl, tl);\n"
"                if (d2 >= lr*lr) continue;\n"
"                float dl = max(sqrt(d2), 0.001f);\n"
"                float a = 1.0f - dl / lr; a *= a;\n"
// SPOT CONE -- LOCKSTEP with the surface and shaft kernels above. Applied before
// the fog weight so the two multiply independently: the cone says WHERE this
// light reaches, the weight says how loudly it scatters once it gets there.
"                a *= rt_cone(float3(tgl[li*RTL+8u], tgl[li*RTL+9u], tgl[li*RTL+10u]), tgl[li*RTL+11u], tgl[li*RTL+12u], tgl[li*RTL+13u], -tl / dl);\n"
// The per-light FOG WEIGHT (slot 7) is applied HERE and nowhere else, which is
// what makes it a fog-only control: it scales both what this light scatters and
// how loudly it argues in the dominant-light vote below, while the surface
// kernel above never reads slot 7 at all. Every producer but the M5 thunderbolt
// writes 1.0, and multiplying by 1.0f is exact - that is the no-op proof for
// this commit.
"                float fw = tgl[li*RTL+7u];\n"
"                float3 c = float3(tgl[li*RTL+4u], tgl[li*RTL+5u], tgl[li*RTL+6u]) * (a * fw);\n"
"                if (cam.residual > 0.0f || cam.lsample != 0u) Ls += c;\n"   // Ls feeds the residual term and the pick's total
"                float w = c.r + c.g + c.b;\n"
"                if (w > bestw) { bestw = w; blp = lp; bdl = dl; bc = c; brad = lr; }\n"
// STOCHASTIC PICK (rt_metal_lightsample), the same exact single-uniform scheme as
// the surface kernel. Here it REPLACES the dominant choice rather than adding to
// it: one shadow ray per cast is all the fog budget allows, so spending it on a
// light chosen in proportion to its scatter -- torches included -- is strictly
// better than always spending it on the loudest. lastLight then divides by the
// pick probability, which makes sum(c_i * v_i) unbiased instead of
// "the dominant, shadowed, plus everything else unshadowed".
"                if (cam.lsample != 0u && w > 0.0f) {\n"
"                    W2 += w;\n"
"                    float pick = js * W2;\n"
"                    if (pick <= w) { js = pick / w; slp = lp; sdl = dl; sc = c; srad = lr; sw = w; }\n"
"                    else { float rem = W2 - w; js = (rem > 0.0f) ? ((pick - w) / rem) : 0.0f; }\n"
"                }\n"
"            }\n"
// THE DOMINANT + PICK HYBRID (rt_metal_lightsample_hybrid, 2026-09-03). The pick
// alone makes every cast BINARY -- the one light it chose is either lit or
// blocked, so a texel whose casts picked shadowed lights goes black for the
// frame, and the 5x5 filter spreads that texel into the soft dark blob Seb
// reported. Here the dominant light is ALWAYS shadow-tested on its own ray and
// the pick is re-drawn over the REST (a second pass over the tile list with
// the dominant excluded), so a cast can no longer lose more than the rest's
// share: estimate = c_dom * vis_dom + (W2 - w_dom) * vis_pick. Mode 1 spends a
// second shadow ray per cast on it; mode 2 alternates casts between the two
// halves, holding the other half's last visibility, at one ray per cast.
"            float3 hc = float3(0.0f); float3 hlp = float3(0.0f); float hdl = 1.0f; float hrad = 1.0f; float hw = 0.0f; float Wr = 0.0f;\n"
"            if (cam.lshybrid != 0u && cam.lshybrid != 3u && cam.lsample != 0u && bestw > 0.0f && W2 > bestw) {\n"
"                float js2 = RT_SELSEQ(gid, cam.frame, 1u + i, cam.history);\n"   // the same stratified stream: the rest's CDF is the pick's minus the dominant
"                for (uint ii = 0u; ii < tcount; ++ii) {\n"
"                    uint li = tlist[ii];\n"
"                    float3 lp = float3(tgl[li*RTL+0u], tgl[li*RTL+1u], tgl[li*RTL+2u]);\n"
"                    float lr = tgl[li*RTL+3u];\n"
"                    float3 tl = lp - p; float d2 = dot(tl, tl);\n"
"                    if (d2 >= lr*lr) continue;\n"
"                    float dl = max(sqrt(d2), 0.001f);\n"
"                    float a = 1.0f - dl / lr; a *= a;\n"
"                    a *= rt_cone(float3(tgl[li*RTL+8u], tgl[li*RTL+9u], tgl[li*RTL+10u]), tgl[li*RTL+11u], tgl[li*RTL+12u], tgl[li*RTL+13u], -tl / dl);\n"
"                    float3 c = float3(tgl[li*RTL+4u], tgl[li*RTL+5u], tgl[li*RTL+6u]) * (a * tgl[li*RTL+7u]);\n"
"                    float w = c.r + c.g + c.b;\n"
"                    if (w <= 0.0f || all(lp == blp)) continue;\n"   // the dominant is out of the rest's draw
"                    Wr += w;\n"
"                    float pick = js2 * Wr;\n"
"                    if (pick <= w) { js2 = pick / w; hlp = lp; hdl = dl; hc = c; hrad = lr; hw = w; }\n"
"                    else { float rem = Wr - w; js2 = (rem > 0.0f) ? ((pick - w) / rem) : 0.0f; }\n"
"                }\n"
"            }\n"
// MODE 3, THE SINGLE-PASS HYBRID (2026-09-06): no second loop. The reservoir
// above already drew one light over ALL of them. Landing on the dominant makes
// this a DOMINANT cast; landing anywhere else makes it a REST cast -- and
// conditional on not being the dominant, the reservoir's draw IS the rest's
// distribution (w / Wr), so the rest estimate below is the same unbiased one
// modes 1 and 2 use. What changes is the SCHEDULE: the dominant gets a ray in
// proportion to its weight instead of every other cast -- nearly every cast
// under a torch, rarely in a hall of equal lights, where its held visibility
// lags. That lag is the variance this mode trades for the second loop's cost
// (+0.43 ms on the fog stage for mode 2, measured 2026-09-03). The first cast
// still seeds the dominant's held visibility when it lands on the rest.
"            bool h3dom = false;\n"
"            if (cam.lshybrid == 3u && cam.lsample != 0u && bestw > 0.0f && W2 > bestw && sw > 0.0f) {\n"
"                h3dom = all(slp == blp);\n"
"                hlp = slp; hdl = sdl; hc = sc; hrad = srad; hw = sw;\n"
"            }\n"
// THE 1/p CLAMP -- LOCKSTEP with the shaft kernel's identical block above,
// and the demo17 speckle's fix: see that block's comment for the mechanism.
"            if (cam.lsample != 0u && sw > 0.0f && !(cam.lshybrid != 0u && hw > 0.0f)) {\n"
"                float pr = W2 / sw;\n"
"                if (cam.wclamp > 0.0f) pr = min(pr, cam.wclamp);\n"
"                blp = slp; bdl = sdl; bc = sc * pr; brad = srad; bestw = sw;\n"
"            }\n"
"            lastLight = float3(0.0f);\n"
"            if (cam.lshybrid != 0u && hw > 0.0f) {\n"
"                // the hybrid: the dominant on its own ray, the rest through the pick\n"
"                uint kk = i / stride;\n"
"                uint ncasts = (steps + stride - 1u) / stride;\n"
"                float rrh = sqrt((float(kk) + 0.5f) / float(ncasts));\n"
"                float th = float(i) * 2.39996323f + 6.2831853f * jf;\n"
"                // mode 2's first cast measures BOTH halves (two rays once), or the held\n"
"                // rest visibility would start at an assumed 1.0 and bias the ray bright\n"
"                // (measured +4.9% median against the truth before this line)\n"
"                bool doDom = (cam.lshybrid == 3u) ? (h3dom || kk == 0u) : ((cam.lshybrid == 1u) || ((kk & 1u) == 0u));\n"
"                bool doRest = (cam.lshybrid == 3u) ? (!h3dom) : ((cam.lshybrid == 1u) || ((kk & 1u) == 1u) || (kk == 0u));\n"
"                if (doDom) {\n"
"                    float3 ld = (blp - p) / bdl;\n"
"                    float R = clamp(brad * 0.15f, 8.0f, 40.0f);\n"
"                    float3 up0 = (fabs(ld.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);\n"
"                    float3 Tb = normalize(cross(up0, ld)); float3 Bb = cross(ld, Tb);\n"
"                    float3 tgt = blp + (cos(th) * rrh * R) * Tb + (sin(th) * rrh * R) * Bb;\n"
"                    float3 d = tgt - p; float sd = length(d);\n"
"                    ray sr; sr.origin = p; sr.direction = d / max(sd, 0.001f); sr.min_distance = 0.0f; sr.max_distance = max(sd - 1.0f, 0.0f);\n"
"                    hdomvis = (si.intersect(sr, accel, 0x3u).type == intersection_type::none) ? 1.0f : 0.0f;\n"
"                }\n"
"                if (doRest) {\n"
"                    float3 ld = (hlp - p) / hdl;\n"
"                    float R = clamp(hrad * 0.15f, 8.0f, 40.0f);\n"
"                    float3 up0 = (fabs(ld.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);\n"
"                    float3 Tb = normalize(cross(up0, ld)); float3 Bb = cross(ld, Tb);\n"
"                    float3 tgt = hlp + (cos(th + 2.0f) * rrh * R) * Tb + (sin(th + 2.0f) * rrh * R) * Bb;\n"
"                    float3 d = tgt - p; float sd = length(d);\n"
"                    ray sr; sr.origin = p; sr.direction = d / max(sd, 0.001f); sr.min_distance = 0.0f; sr.max_distance = max(sd - 1.0f, 0.0f);\n"
"                    hrestvis = (si.intersect(sr, accel, 0x3u).type == intersection_type::none) ? 1.0f : 0.0f;\n"
"                }\n"
"                // the rest's estimate: c_pick / p_pick = Wr in weight units, spread over the rest's summed colour\n"
"                float3 restc = Ls - bc;\n"
"                lastLight = bc * hdomvis + max(restc, float3(0.0f)) * hrestvis;\n"
"                float ll = max(lastLight.r, max(lastLight.g, lastLight.b));\n"
"                lastLight *= DPD_LMAX / (DPD_LMAX + ll);\n"
"            } else\n"
"            if (bestw > 0.0f) {\n"
"                // disc-jittered target: flame models sit AT light origins, a centre\n"
"                // ray hits them every time (the god-ray lesson)\n"
"                float3 ld = (blp - p) / bdl;\n"
"                float R = clamp(brad * 0.15f, 8.0f, 40.0f);\n"
"                float3 up0 = (fabs(ld.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);\n"
"                float3 Tb = normalize(cross(up0, ld));\n"
"                float3 Bb = cross(ld, Tb);\n"
"                // per-CAST equal-area disc sampling: indexing by the march step\n"
"                // made the radius a function of depth, so near-camera casts\n"
"                // always aimed near the disc centre (straight into the flame\n"
"                // before light-cores; still a sampling bias after them)\n"
"                uint kk = i / stride;\n"
"                uint ncasts = (steps + stride - 1u) / stride;\n"
"                float rr = sqrt((float(kk) + 0.5f) / float(ncasts)) * R;\n"
"                float th = float(i) * 2.39996323f + 6.2831853f * jf;\n"
"                float3 tgt = blp + (cos(th) * rr) * Tb + (sin(th) * rr) * Bb;\n"
"                float3 d = tgt - p; float sd = length(d);\n"
"                ray sr;\n"
"                sr.origin = p;\n"
"                sr.direction = d / max(sd, 0.001f);\n"
"                sr.min_distance = 0.0f;\n"
"                sr.max_distance = max(sd - 1.0f, 0.0f);\n"
"                float vis = (si.intersect(sr, accel, 0x3u).type == intersection_type::none) ? 1.0f : 0.0f;\n"
"                lastLight = bc * vis;\n"
// The unshadowed residual fill exists ONLY to stop non-dominant lights (torches,
// almost always) vanishing from the fog; with the stochastic pick every light is
// shadow-tested in expectation, so the fill would double-count. Ignored, not
// removed: rt_metal_lightsample 0 restores it byte for byte.
"                if (cam.residual > 0.0f && cam.lsample == 0u) lastLight += cam.residual * (Ls - bc);\n"
"                // soft shoulder BEFORE either consumer: bc is unnormalised (a\n"
"                // plain Quake light uploads ~1 per channel) and the residual\n"
"                // sum grows with light count, so clusters blew both the fog\n"
"                // and beam terms out. Max-channel norm: a typical single\n"
"                // light is barely touched; blowups asymptote at DPD_LMAX.\n"
"                float ll = max(lastLight.r, max(lastLight.g, lastLight.b));\n"
"                lastLight *= DPD_LMAX / (DPD_LMAX + ll);\n"
"            }\n"
"        }\n"
"        // the light ADDS to the authored tint and scatters WITH the density, so\n"
"        // beams brighten inside murk banks; suppressed in liquid (authored look)\n"
"        float3 tintstep = tint + lastLight * cam.intensity * (1.0f - inliquid * (1.0f - cam.liquidlight));\n"   // B2: LOCKSTEP x4 (tintstep, lavaK, curB, beamsum)
"        float step_t = exp(-density * dts * 0.01f * cam.extinction);\n"  // LOCKSTEP shader_glsl.h step_t (extinction)

"        // DENSITY-INDEPENDENT beam term: the old god-ray look. Accumulated as\n"
"        // a path integral normalised by DPD_BEAM_REF world units (the old\n"
"        // dt*0.01 weight summed to marchlen/100 -- LINEAR in view distance, so\n"
"        // a 3000-unit sightline bloomed 30x a close wall) and soft-saturated\n"
"        // after the loop. Still dims behind murk via T, still suppressed in\n"
"        // liquid. The stride-held lastLight is a piecewise-constant integrand;\n"
"        // weighting each held step by dt is the correct Riemann sum.\n"
"#if RT_FROXEL\n"
// THE CELL STORE. A = radiance x density (so a lit dense puff outweighs a dark
// thin patch when the integrate pass divides the accumulated density back
// out) and the density itself; B = the held cast light (the beam integrand,
// liquid-suppressed as the beamsum line below spells it) and the lava-glow
// kernel (LOCKSTEP with the lavasum line above). The history is read at this
// cell's world centre through the FULL previous camera; the far trilinear tap
// must have been MARCHED last frame (its cell START inside that ray's march
// end), or the tap would be stale content from a wall's interior.
"        {\n"
"            float lavaK = lavakind * exp(-max(-surfdist, 0.0f) / DPD_LAVAGLOW_H) * (1.0f - inliquid * (1.0f - cam.liquidlight));\n"
"            float4 curA = float4(tintstep * density, density);\n"
"            float4 curB = float4(lastLight * (1.0f - inliquid * (1.0f - cam.liquidlight)), lavaK);\n"
"            float4 accA = curA, accB = curB;\n"
"            if (cam.fhasPrev != 0u && cam.fhistory > 0.0f) {\n"
"                float3 P = ro + rd * (ct0 + 0.5f * cvis);\n"
"                float3 rel = P - float3(cam.pOrigin);\n"
"                float tz = dot(rel, float3(cam.pForward));\n"
"                if (tz > 1.0e-3f) {\n"
"                    float fx = ((dot(rel, float3(cam.pRight)) / tz) / cam.pTanx + 1.0f) * 0.5f;\n"
"                    float fy = ((dot(rel, float3(cam.pUp)) / tz) / cam.pTany + 1.0f) * 0.5f;\n"
"                    if (fx >= 0.0f && fy >= 0.0f && fx < 1.0f && fy < 1.0f) {\n"
"                        float dprev = length(rel);\n"
"                        float pe = fabs(prevEnd.read(uint2(min(uint(fx * float(cam.w)), cam.w - 1u), min(uint(fy * float(cam.h)), cam.h - 1u))).x);\n"
"                        float wz = fx_u(dprev, cam.dist, cam.fcurve);\n"
"                        float kfar = min(floor(wz * float(steps) - 0.5f) + 1.0f, float(steps) - 1.0f);\n"
"                        float tfar = fx_t(kfar / float(steps), cam.dist, cam.fcurve);\n"
"                        if (wz < 1.0f && tfar < pe) {\n"
"                            float3 uvw = float3(fx, fy, wz);\n"
"                            accA = mix(curA, prevA.sample(vsamp, uvw), cam.fhistory);\n"
"                            accB = mix(curB, prevB.sample(vsamp, uvw), cam.fhistory);\n"
"                        }\n"
"                    }\n"
"                }\n"
"            }\n"
"            outA.write(accA, uint3(gid, i));\n"
"            outB.write(accB, uint3(gid, i));\n"
"        }\n"
"        T *= step_t;\n"
"        if (T < 0.003f) { fend = min(ct1, enddist); break; }\n"
"#else\n"
"        beamsum += T * lastLight * (1.0f - inliquid * (1.0f - cam.liquidlight)) * (dt * (1.0f / DPD_BEAM_REF));\n"
"        scattered += T * (1.0f - step_t) * tintstep;\n"
"        { float sw = T * (1.0f - step_t); dsum += sw * t; wsum += sw; }\n"
"        T *= step_t;\n"
"        if (T < 0.003f) break;\n"
"#endif\n"
"    }\n"
"#if RT_FROXEL\n"
// The volume holds this frame's cells; rt_froxelintegrate sums the ACCUMULATED
// ones. Only the march end leaves this kernel, signed: negative = sky (the
// integrate pass applies the sky cap; the next frame's cells read it as the
// validity bound for their history taps).
"    depthOut.write(float4((H.x > 1.0e17f) ? -fend : fend, 0.0f, 0.0f, 0.0f), gid);\n"
"#else\n"
"    // beams knob + Reinhard shoulder (max-channel): every channel stays\n"
"    // below 1 by construction, so beams alone can never flat-white the\n"
"    // RGBA8 composite -- long halls saturate to thick glow instead.\n"
"    // Applied before the EMA so history blends post-shoulder values.\n"
"    // The normalisation reference and the shoulder shape now come from\n"
"    // shader_density.h, shared with the GL shafts consumption.\n"
"    float3 bterm = cam.beams * beamsum;\n"
"    scattered += DPD_SHOULDER(bterm);\n"
"    // in-air lava glow: knob * lava colour * path integral, same shoulder\n"
"    float3 lavag = cam.lavaglow * float3(cam.lavacolor) * lavasum;\n"
"    scattered += lavag / (1.0f + max(lavag.r, max(lavag.g, lavag.b)));\n"
"    // SKY FOG CAP -- LOCKSTEP with the two marches (shader_glsl.h carries the\n"
"    // rationale). They key on rawdepth >= 0.9999; here sky is the primary\n"
"    // pass's 1e18 history sentinel. Applied PRE-EMA, or history drags the\n"
"    // capped and uncapped states across each other on every toggle.\n"
"    if (H.x > 1.0e17f && T < cam.skytrans) {\n"
"        scattered *= (1.0f - cam.skytrans) / (1.0f - T);\n"
"        T = cam.skytrans;\n"
"    }\n"

"    float4 outc = float4(scattered, T);\n"
"    if (cam.history > 0.0f && cam.hasPrev != 0u) {\n"
"        // REPROJECTED EMA (rt_metal_reproject): the previous fog frame was\n"
"        // rendered through the previous camera; a same-pixel blend smears the\n"
"        // fog across turns. Static camera rounds back to gid exactly.\n"
"        uint2 pp = gid; bool okp = true;\n"
"        if (cam.hasPrevCam != 0u) {\n"
"            float tz = dot(rd, float3(cam.pForward));\n"
"            if (tz > 1.0e-4f) {\n"
"                float fx = ((dot(rd, float3(cam.pRight)) / tz) / cam.pTanx + 1.0f) * 0.5f * float(cam.w) - 0.5f;\n"
"                float fy = ((dot(rd, float3(cam.pUp)) / tz) / cam.pTany + 1.0f) * 0.5f * float(cam.h) - 0.5f;\n"
"                int ix = int(round(fx)), iy = int(round(fy));\n"
"                if (ix >= 0 && iy >= 0 && ix < int(cam.w) && iy < int(cam.h)) pp = uint2(uint(ix), uint(iy)); else okp = false;\n"
"            } else okp = false;\n"
"        }\n"
"        if (okp) outc = mix(outc, prevFog.read(pp), cam.history);\n"   // ALL FOUR channels: A is transmittance
"    }\n"
"    outtex.write(outc, gid);\n"
// WHERE THIS TEXEL'S FOG IS. The extinction-weighted mean march distance is the
// depth the in-scattered light came from -- for Seb's density that is ~100
// units out while the wall behind it is 300-1000, and under a 4-unit strafe
// step the two move by 12 and 2 fog texels a frame. The history pass
// (rt_fogtemporal, rt_metal_fog_reproject_depth) reprojects each texel's
// history through the FULL previous camera at this depth; the march end would
// mis-reproject the near fog five times over. A ray that scattered nothing
// reports its march end. Written every frame, bound always (the declared-but-
// unbound validation abort); the colour arithmetic above is untouched.
"    depthOut.write(float4((wsum > 1.0e-6f) ? (dsum / wsum) : enddist, 0.0f, 0.0f, 0.0f), gid);\n"
"#endif\n"
"}\n";

// S1 of the fog-buffer plan: a 3x3 [1 2 1]^2 depth- and transmittance-aware
// filter over the fog kernel's RAW output, at FOG resolution, before the EMA
// and before the composite magnifies it. This is the only stage that removes
// the IGN lattice at SOURCE: IGN and blue noise both put their energy at
// Nyquist by design (invisible at 1:1, magnified ~3x by the 8/3 upsample), and
// a [1 2 1] has ZERO response at Nyquist. Depth affinity keeps silhouettes:
// each tap is weighted by how well its march-end depth matches the centre's,
// using the SAME history-texel mapping the fog kernel used to read its own
// march end (hp, LOCKSTEP with rt_fog above and with VolumetricTapDepth in
// shader_glsl.h / dp_vf_tapdepth in shader_msl.h). Transmittance affinity
// (the alpha channel) keeps a fog bank's edge from bleeding into clear air.
static const char *kFogFilterSrc =
"struct FiltCam {\n"
"    uint w, h;            // fog buffer size\n"
"    uint fullw, fullh;    // history (trace) buffer size\n"
"    uint histcentre;      // LOCKSTEP with rt_fog's hp\n"
"    float depthtol;       // relative depth tolerance (rt_metal_fog_upsample_depth)\n"
"    float farclip;        // cam.dist: the sky sentinel's depth\n"
"    float3 ro;            // eye, for the hit -> distance reduction\n"
"    uint mode;            // 1 = 3x3, 2 = 5x5 (3 = the cost A/B; 4 = two 5x5 passes, the second dilated)\n"
"    uint dilate;          // tap spacing in texels (1; the a-trous second pass uses 2)\n"
"};\n"
"static inline uint2 ff_hp(uint2 g, constant FiltCam &c) {\n"
"    return c.histcentre != 0u\n"
"        ? uint2(min(((2u * g.x + 1u) * c.fullw) / (2u * max(c.w, 1u)), c.fullw - 1u),\n"
"                min(((2u * g.y + 1u) * c.fullh) / (2u * max(c.h, 1u)), c.fullh - 1u))\n"
"        : uint2(min((g.x * c.fullw) / max(c.w, 1u), c.fullw - 1u),\n"
"                min((g.y * c.fullh) / max(c.h, 1u), c.fullh - 1u));\n"
"}\n"
"static inline float ff_depth(texture2d<float, access::read> hist, uint2 g, constant FiltCam &c) {\n"
"    float4 H = hist.read(ff_hp(g, c));\n"
"    return (H.x > 1.0e17f) ? c.farclip : min(length(H.xyz - c.ro), c.farclip);\n"
"}\n"
"kernel void rt_fogfilter(texture2d<float, access::write> outtex [[texture(0)]],\n"
"                         texture2d<float, access::read> raw [[texture(1)]],\n"
"                         texture2d<float, access::read> hist [[texture(2)]],\n"
"                         constant FiltCam &c [[buffer(0)]],\n"
"                         uint2 gid [[thread_position_in_grid]])\n"
"{\n"
"    if (gid.x >= c.w || gid.y >= c.h) return;\n"
"    float4 cen = raw.read(gid);\n"
"    float dc = ff_depth(hist, gid, c);\n"
"    int r = (c.mode == 1u) ? 1 : 2;\n"
"    int dl = max(int(c.dilate), 1);\n"
"    float4 acc = float4(0.0f); float wsum = 0.0f;\n"
"    for (int dy = -r; dy <= r; ++dy) {\n"
"        int yy = int(gid.y) + dy * dl; if (yy < 0 || yy >= int(c.h)) continue;\n"
"        float wy = (r == 1) ? (dy == 0 ? 2.0f : 1.0f) : (dy == 0 ? 6.0f : (abs(dy) == 1 ? 4.0f : 1.0f));\n"
"        for (int dx = -r; dx <= r; ++dx) {\n"
"            int xx = int(gid.x) + dx * dl; if (xx < 0 || xx >= int(c.w)) continue;\n"
"            float wx = (r == 1) ? (dx == 0 ? 2.0f : 1.0f) : (dx == 0 ? 6.0f : (abs(dx) == 1 ? 4.0f : 1.0f));\n"
"            uint2 g = uint2(xx, yy);\n"
"            float4 v = raw.read(g);\n"
"            // depth affinity: a tap whose march ended on a different surface\n"
"            // (relative tolerance, as the upsample uses) is a silhouette, and\n"
"            // must not be averaged across. Mode 3 = the cost A/B: skip the\n"
"            // history reads and weight by transmittance alone.\n"
"            float wd = 1.0f;\n"
"            if (c.mode < 3u) {\n"
"                float dt = ff_depth(hist, g, c);\n"
"                float dd = abs(dt - dc) / max(dc, 1.0f);\n"
"                wd = (dd < c.depthtol) ? 1.0f : 0.0f;\n"
"            }\n"
"            // transmittance affinity: keep a bank's edge against clear air\n"
"            float wt = 1.0f - min(abs(v.a - cen.a) * 4.0f, 1.0f);\n"
"            float w = wx * wy * wd * wt;\n"
"            acc += v * w; wsum += w;\n"
"        }\n"
"    }\n"
"    // the centre always has weight > 0 (wd = 1, wt = 1), so wsum > 0\n"
"    outtex.write(acc / wsum, gid);\n"
"}\n";

// BLUENOISE slice 2: the fog history pass (rt_metal_fog_clamp). One thread
// per fog texel: read this frame's RAW scatter and its 3x3 neighbourhood,
// reproject the previous EMA through the previous camera basis (rotation-only
// -- LOCKSTEP with rt_fog's own EMA read above, the same expression
// character for character), CLAMP that history into the neighbourhood's
// range (mode 1: min/max; mode 2: mean +/- k sigma -- the variance clip every
// RT denoiser ships beside the box), blend, write the new EMA. Mode 3 skips
// the clamp and is the A/B that separates the reorder (raw accumulation,
// filter once for display) from the clamp itself. Alpha is transmittance and
// rides the same clamp: it is smooth, and a history that disagrees with the
// current transmittance is exactly a door that opened. tonemap (slice 3,
// rt_metal_fog_tonemapema) blends in a c/(1+luma) domain and inverts on the
// way out -- the Karis weight: a bright pick weighs less than its linear
// worth in the average. It biases the mean DOWN under variance, which is why
// it is its own switch and its help text says so.
static const char *kFogTemporalSrc =
"struct FogTemporalCam {\n"
"    packed_float3 forward;\n"
"    packed_float3 right;\n"
"    packed_float3 up;\n"
"    float tanx, tany;\n"
"    uint w, h;\n"
"    float history;\n"
"    uint hasPrev;\n"
"    uint hasPrevCam;\n"
"    packed_float3 pForward;\n"
"    packed_float3 pRight;\n"
"    packed_float3 pUp;\n"
"    float pTanx, pTany;\n"
"    uint mode;\n"
"    float k;\n"
"    uint tonemap;\n"
"    packed_float3 origin;\n"     // rt_metal_fog_reproject_depth (2026-09-03): this frame's eye ...
"    packed_float3 pOrigin;\n"    // ... and the previous frame's, for the translation-aware read
"    uint reprojdepth;\n"         // 0 = rotation-only remap through the previous basis (the 2026-09-03 morning arm), 1 = reproject the texel's scatter centroid through the FULL previous camera
"    float depthtol;\n"           // relative disagreement between the previous frame's centroid depth there and this texel's reprojected distance that REJECTS the history
"};\n"
"constexpr sampler ftsamp(filter::linear, coord::normalized, address::clamp_to_edge);\n"
"static inline float4 ft_tm(float4 c, uint on) {\n"
"    if (on == 0u) return c;\n"
"    float l = dot(c.rgb, float3(0.299f, 0.587f, 0.114f));\n"
"    return float4(c.rgb / (1.0f + l), c.a);\n"
"}\n"
"static inline float4 ft_itm(float4 c, uint on) {\n"
"    if (on == 0u) return c;\n"
"    float l = dot(c.rgb, float3(0.299f, 0.587f, 0.114f));\n"
"    return float4(c.rgb / max(1.0f - l, 1.0e-4f), c.a);\n"
"}\n"
"kernel void rt_fogtemporal(texture2d<float, access::write> outema [[texture(0)]],\n"
"                           texture2d<float, access::read> raw [[texture(1)]],\n"
"                           texture2d<float, access::sample> prevema [[texture(2)]],\n"
"                           texture2d<float, access::read> curdepth [[texture(3)]],\n"    // this frame's scatter centroid (rt_fog's depthOut)
"                           texture2d<float, access::sample> prevdepth [[texture(4)]],\n" // the previous frame's
"                           constant FogTemporalCam &c [[buffer(0)]],\n"
"                           uint2 gid [[thread_position_in_grid]])\n"
"{\n"
"    if (gid.x >= c.w || gid.y >= c.h) return;\n"
"    float4 cur = ft_tm(raw.read(gid), c.tonemap);\n"
"    float4 outc = cur;\n"
"    if (c.history > 0.0f && c.hasPrev != 0u) {\n"
"        uint2 pp = gid; bool okp = true; float2 uv = float2(0.0f); bool bil = false;\n"
"        if (c.hasPrevCam != 0u) {\n"
"            float sx = (2.0f * (float(gid.x) + 0.5f) / float(c.w) - 1.0f) * c.tanx;\n"
"            float sy = (2.0f * (float(gid.y) + 0.5f) / float(c.h) - 1.0f) * c.tany;\n"
"            float3 rd = normalize(float3(c.forward) + sx * float3(c.right) + sy * float3(c.up));\n"
"            if (c.reprojdepth != 0u) {\n"
"                // TRANSLATION-AWARE: this texel's fog lives at its scatter centroid; put that\n"
"                // point through the FULL previous camera (origin included -- the composite's\n"
"                // rt_metal_reproject_depth shape) and read the history there BILINEARLY.\n"
"                float dcur = curdepth.read(gid).x;\n"
"                if (c.reprojdepth == 4u) dcur *= 2.0f; else if (c.reprojdepth == 5u) dcur *= 0.5f;\n"   // DEBUG probes: the depth's scale
"                float3 P = float3(c.origin) + rd * dcur;\n"
"                // mode 2: DEBUG, the translation negated (a sign-error probe); mode 3: DEBUG,\n"
"                // rotation-only geometry with the bilinear read (isolates the sampler)\n"
"                float3 po = (c.reprojdepth == 2u) ? (2.0f * float3(c.origin) - float3(c.pOrigin)) : ((c.reprojdepth == 3u) ? float3(c.origin) : float3(c.pOrigin));\n"
"                float3 rel = P - po;\n"
"                float tz = dot(rel, float3(c.pForward));\n"
"                if (tz > 1.0e-3f) {\n"
"                    float fx = ((dot(rel, float3(c.pRight)) / tz) / c.pTanx + 1.0f) * 0.5f;\n"
"                    float fy = ((dot(rel, float3(c.pUp)) / tz) / c.pTany + 1.0f) * 0.5f;\n"
"                    if (fx >= 0.0f && fy >= 0.0f && fx < 1.0f && fy < 1.0f) {\n"
"                        uv = float2(fx, fy); bil = true;\n"
"                        // DISOCCLUSION: the history there was fog at the previous frame's own\n"
"                        // centroid; if that disagrees with where THIS fog sits from the old\n"
"                        // eye, it is a different piece of fog (a doorway, a silhouette) --\n"
"                        // drop it rather than smear it. A ray that scattered nothing carries\n"
"                        // its march end and fails this against real fog, correctly.\n"
"                        float dprev = prevdepth.sample(ftsamp, uv).x;\n"
"                        float dexp = length(rel);\n"
"                        if (fabs(dprev - dexp) > c.depthtol * max(dexp, 1.0f)) okp = false;\n"
"                    } else okp = false;\n"
"                } else okp = false;\n"
"            } else {\n"
"            float tz = dot(rd, float3(c.pForward));\n"
"            if (tz > 1.0e-4f) {\n"
"                float fx = ((dot(rd, float3(c.pRight)) / tz) / c.pTanx + 1.0f) * 0.5f * float(c.w) - 0.5f;\n"
"                float fy = ((dot(rd, float3(c.pUp)) / tz) / c.pTany + 1.0f) * 0.5f * float(c.h) - 0.5f;\n"
"                int ix = int(round(fx)), iy = int(round(fy));\n"
"                if (ix >= 0 && iy >= 0 && ix < int(c.w) && iy < int(c.h)) pp = uint2(uint(ix), uint(iy)); else okp = false;\n"
"            } else okp = false;\n"
"            }\n"
"        }\n"
"        if (okp) {\n"
"            float4 hist = ft_tm(bil ? prevema.sample(ftsamp, uv) : prevema.read(pp), c.tonemap);\n"
"            if (c.mode == 1u || c.mode == 2u) {\n"
"                float4 lo = cur, hi = cur, mean = float4(0.0f), m2 = float4(0.0f); float n = 0.0f;\n"
"                for (int dy = -1; dy <= 1; ++dy) {\n"
"                    int yy = int(gid.y) + dy; if (yy < 0 || yy >= int(c.h)) continue;\n"
"                    for (int dx = -1; dx <= 1; ++dx) {\n"
"                        int xx = int(gid.x) + dx; if (xx < 0 || xx >= int(c.w)) continue;\n"
"                        float4 v = ft_tm(raw.read(uint2(uint(xx), uint(yy))), c.tonemap);\n"
"                        lo = min(lo, v); hi = max(hi, v); mean += v; m2 += v * v; n += 1.0f;\n"
"                    }\n"
"                }\n"
"                if (c.mode == 2u) {\n"
"                    mean /= n;\n"
"                    float4 sd = sqrt(max(m2 / n - mean * mean, float4(0.0f)));\n"
"                    lo = mean - c.k * sd; hi = mean + c.k * sd;\n"
"                }\n"
"                hist = clamp(hist, lo, hi);\n"
"            }\n"
"            outc = mix(cur, hist, c.history);\n"
"        }\n"
"    }\n"
"    outema.write(ft_itm(outc, c.tonemap), gid);\n"
"}\n";

// A1 FROXEL: the integrate pass. Sums the accumulated cells front to back
// into the fog surface the composite consumes (RGB scatter, A transmittance),
// clipped at THIS frame's march end (signed: negative = sky). Everything the
// old kernel did after its loop -- the beam shoulder, the lava glow, the sky
// cap -- happens here instead, LOCKSTEP with the RT_FROXEL 0 arm of rt_fog.
// The (1 - e^(-sigma k))/sigma form is the analytic single-scatter integral of
// a cell whose radiance-times-density and density were accumulated
// SEPARATELY, so the division recovers the density-weighted mean radiance.
static const char *kFroxelIntegrateSrc =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct FroxelCam {\n"
"    uint w, h, n;\n"
"    float dist;\n"
"    float curve;\n"
"    float extinction;\n"
"    float beams;\n"
"    float lavaglow;\n"
"    float skytrans;\n"
"    packed_float3 lavacolor;\n"
"};\n"
"static inline float fi_t(float u, float dist, float a) { return (a > 0.01f) ? dist * (exp(a * u) - 1.0f) / (exp(a) - 1.0f) : dist * u; }\n"   // LOCKSTEP with fx_t in kFogSrc
"kernel void rt_froxelintegrate(texture2d<float, access::write> outtex [[texture(0)]],\n"
"                               texture3d<float, access::read> volA [[texture(1)]],\n"
"                               texture3d<float, access::read> volB [[texture(2)]],\n"
"                               texture2d<float, access::read> endtex [[texture(3)]],\n"
"                               constant FroxelCam &c [[buffer(0)]],\n"
"                               uint2 gid [[thread_position_in_grid]])\n"
"{\n"
"    if (gid.x >= c.w || gid.y >= c.h) return;\n"
"    float e = endtex.read(gid).x;\n"
"    bool sky = e < 0.0f; float enddist = fabs(e);\n"
"    float T = 1.0f; float3 scattered = float3(0.0f); float3 beamsum = float3(0.0f); float lavasum = 0.0f;\n"
"    for (uint j = 0u; j < c.n; ++j) {\n"
"        float t0 = fi_t(float(j) / float(c.n), c.dist, c.curve);\n"
"        if (t0 >= enddist) break;\n"
"        float t1 = fi_t(float(j + 1u) / float(c.n), c.dist, c.curve);\n"
"        float len = min(t1, enddist) - t0;\n"
"        float4 A = volA.read(uint3(gid, j));\n"
"        float4 B = volB.read(uint3(gid, j));\n"
"        float sigma = max(A.a, 0.0f);\n"
"        float k = len * 0.01f * c.extinction;\n"
"        float step_t = exp(-sigma * k);\n"
"        float g = (sigma > 1.0e-5f) ? (1.0f - step_t) / sigma : k;\n"
"        scattered += T * A.rgb * g;\n"
"        beamsum += T * B.rgb * (len * (1.0f / DPD_BEAM_REF));\n"
"        lavasum += T * B.a * (len * (1.0f / DPD_LAVAGLOW_REF));\n"
"        T *= step_t;\n"
"        if (T < 0.003f) break;\n"
"    }\n"
"    float3 bterm = c.beams * beamsum;\n"
"    scattered += DPD_SHOULDER(bterm);\n"
"    float3 lavag = c.lavaglow * float3(c.lavacolor) * lavasum;\n"
"    scattered += lavag / (1.0f + max(lavag.r, max(lavag.g, lavag.b)));\n"
"    if (sky && T < c.skytrans) {\n"
"        scattered *= (1.0f - c.skytrans) / (1.0f - T);\n"
"        T = c.skytrans;\n"
"    }\n"
"    outtex.write(float4(scattered, T), gid);\n"
"}\n";
// CPU mirror of FroxelCam: nine 4-byte fields then a packed_float3, 48 bytes.
typedef struct { uint32_t w, h, n; float dist, curve, extinction, beams, lavaglow, skytrans; float lavacolor[3]; } RTFroxelCam;
_Static_assert(sizeof(RTFroxelCam) == 48, "RTFroxelCam must mirror the MSL FroxelCam (48 bytes)");

// CPU-side mirror of the shaft kernel's ShaftCam (same tight-packing rules).
typedef struct {
	float origin[3];
	float forward[3];
	float right[3];
	float up[3];
	float tanx, tany;
	uint32_t w, h;
	uint32_t fullw, fullh;
	uint32_t frame;
	uint32_t numLights;
	uint32_t samples;
	float dist;
	float history;
	float residual;
	uint32_t hasPrev;
	float pForward[3];   // previous-frame camera basis (reprojected EMA read)
	float pRight[3];
	float pUp[3];
	float pTanx, pTany;
	uint32_t hasPrevCam;
	uint32_t lsample;    // rt_metal_lightsample (appended 2026-08-17)
	float wclamp;        // rt_metal_lightsample_clamp: 1/p weight ceiling (appended 2026-08-29)
} RTShaftCam;

// CPU-side mirror of the fog kernel's FogCam (same tight-packing rules).
typedef struct {
	float origin[3];
	float forward[3];
	float right[3];
	float up[3];
	float tanx, tany;
	uint32_t w, h;
	uint32_t fullw, fullh;
	uint32_t frame;
	uint32_t numLights;
	uint32_t steps;
	uint32_t stride;
	float dist;
	float history;
	float intensity;
	float residual;
	float beams;
	uint32_t hasPrev;
	float pForward[3];   // previous-frame camera basis (reprojected EMA read)
	float pRight[3];
	float pUp[3];
	float pTanx, pTany;
	uint32_t hasPrevCam;
	float windoffset[3];
	float color[3];
	float watercolor[3];
	float slimecolor[3];
	float lavacolor[3];
	float fieldorigin[3];
	float fieldinvsize[3];
	float density;
	float height;
	float basez;
	float noisescale;
	float noisethresh;
	float waterdensity;
	float slimedensity;
	float lavadensity;
	float watermode;
	float flooroffset;
	float floormode;
	float watermist;
	float mistheight;
	float sdfrange;
	float corner;
	float fieldmaxh;
	float grounddensity;   // GROUND FOG from here (lockstep additions)
	float groundheight;
	float groundnoisescale;
	float groundthresh;
	float grounddeform;
	float groundoffset;
	float groundwindoffset[3];
	float groundcolor[3];
	float lavaglow;        // warm in-air glow near lava (lockstep append)
	float skytrans;        // sky transmittance floor (1 - r_volumetric_skyfog; 0 = uncapped)
	float irrorigin[3];    // AMBIENT IRRADIANCE (F5, lockstep append)
	float irrinvsize[3];
	float irrgain;
	float irrfloor;
	float irrstrength;     // 0 = the self-lit murk exactly (and the honesty gate)
	float extinction;      // r_volumetric_extinction; 1 = the classic model exactly
	float swirlamp;        // KH SWIRL (F3, lockstep append): curl amplitude, wu
	float swirlscale;
	float swirlkh;         // interface-band boost
	float swirlspare;
	float mistlavacut;     // lava suppression of the surface mist band (F7; merge-serialised LAST)
	uint32_t histcentre;   // rt_metal_fog_upsample: march end read at the texel centre (appended 2026-08-16)
	uint32_t lsample;      // rt_metal_lightsample: stochastic light pick (appended 2026-08-17)
	uint32_t adstride;     // rt_metal_fog_stride_adaptive: transmittance-adaptive cast stride (appended 2026-08-28)
	uint32_t stepjitter;   // rt_metal_fog_stepjitter: per-step jitter spread (S2; appended 2026-08-18)
	float wclamp;          // rt_metal_lightsample_clamp: 1/p weight ceiling (appended 2026-08-29)
	uint32_t lshybrid;     // rt_metal_lightsample_hybrid: dominant always shadowed + pick over the rest (appended 2026-09-03; 0 = the old bytes)
	uint32_t froxel;       // A1 FROXEL (appended 2026-09-06): informational -- the arm is the RT_FROXEL 1 PSO
	uint32_t nslices;      // froxel slice count
	float fcurve;          // exponential slice-spacing exponent (0 = uniform)
	float fhistory;        // per-cell history weight
	float pOrigin[3];      // previous-frame eye (the cell reprojection's translation)
	uint32_t fhasPrev;     // the other slot's volume is a settled history under the same geometry
	uint32_t fcastphase;   // rt_metal_fog_froxel_castphase: per-frame cast schedule phase
	float liquidfloor;     // r_volumetric_liquidfloor (2026-09-07): LOCKSTEP with the FogCam text above
	float liquidlight;     // BEAUTY B2 (appended 2026-09-16): lit terms inside a liquid, 0 = suppressed (old bytes)
} RTFogCam;
// the kernel ABI struct mirrors MSL FogCam byte for byte: tightly packed 4-byte
// fields, append-only. If a field lands on one side only, the kernel mis-shades
// silently -- keep the two lists in the same order.
_Static_assert(sizeof(RTFogCam) == 492, "RTFogCam must mirror the MSL FogCam field-for-field: 123 tightly packed 4-byte fields");

// CPU-side mirror of the kernel's Cam (tight packing, matches packed_float3).
typedef struct {
	float origin[3];
	float forward[3];
	float right[3];
	float up[3];
	float tanx, tany;
	uint32_t w, h;
	uint32_t frame;      // plumbed for later temporal jitter
	uint32_t numLights;  // number of real lights in the lights buffer
	uint32_t samples;    // rt_metal_samples: shadow rays per pixel
	float softness;      // rt_metal_softness: penumbra width (fraction of light radius)
	float darkness;      // rt_metal_darkness: scene multiplier where fully shadowed
	float pOrigin[3];    // previous-frame camera basis (temporal reprojection)
	float pForward[3];
	float pRight[3];
	float pUp[3];
	float pTanx, pTany;
	float history;       // rt_metal_history: temporal EMA weight (0 = off)
	uint32_t hasPrev;    // 1 if previous-frame history is valid
	uint32_t numDynamic; // lights[0..numDynamic) are dynamic (colored brighten applies)
	float colorstr;      // rt_metal_color: dynamic-light colored brighten strength (0 = off)
	float walllight;     // rt_metal_walllight: >0 = full RT lighting intensity (scene is fullbright albedo)
	float ambient;       // rt_metal_ambient: ambient fill for wall-lighting mode
	uint32_t entnorms;   // 1 = the entity normals buffer holds real per-vertex normals
	float lmax;          // rt_metal_lmax: wall-lighting term soft ceiling knee (0 = off = old bytes; appended 2026-08-16)
	uint32_t lsample;    // rt_metal_lightsample: stochastic secondary light (appended 2026-08-17)
	uint32_t lsrays;     // rt_metal_lightsample_rays: shadow rays spent on that secondary
	uint32_t gi;         // rt_metal_gi: one-bounce diffuse GI (GIARC G1, appended 2026-08-29)
	float gidist;        // rt_metal_gi_dist: bounce ray max length, wu
	float gialbedo;      // rt_metal_gi_albedo: constant bounce reflectance
	float gihistory;     // rt_metal_gi_history: GI colour EMA weight (stillness-floored at encode)
	float giintensity;   // rt_metal_gi_intensity: gain inside the walllight*6 scale
	float giemissive;    // rt_metal_gi_emissive: emissive-instance bounce gain (G3, appended 2026-08-29; 0 = the G1 mask and bytes)
	uint32_t girate;     // rt_metal_gi_rate: 1-in-N bounce-sample rotation (G4-1, appended 2026-08-29; 1 = every pixel, the old bytes)
	float gialbtex;      // rt_metal_gi_albedo_tex: blend to the bounce surface's own mean colour (G4-2, appended 2026-08-29; 0 = the old bytes)
	uint32_t gbias;      // RT_METAL_GIBIAS: measure-only debug arm (GIARC G4, appended 2026-08-29; 0 = the shipped kernel)
	uint32_t gifallback; // rt_metal_gi_fallback: full-light-list fallback at a bounce point no tile light reaches (G4-3, appended 2026-08-30; 0 = the old bytes)
	float gitiledilate;  // rt_metal_gi_tiledilate: dilate the tile cull by this FRACTION of gidist for the bounce pick (G4-4, appended 2026-08-31; 0 = the old bytes)
	float sundir[3];     // SEPTEMBER2 D (appended 2026-09-06): unit vector toward the sun
	float suncol[3];     // the sun's colour in Lsum units
	float sunpen;        // tan(penumbra)
	uint32_t sun;        // 0 = no sun ray, the old bytes
	uint32_t liqrt;      // SEPTEMBER2 C2 (appended 2026-09-09): 1 = the blended-liquid intersect and the liquid pair
	float liqreflect;    // rt_metal_liquids_reflect: reflection gain
	float giao;          // BEAUTY B1 (appended 2026-09-16): ambient occlusion from the bounce ray on the ambient fill (0 = old bytes)
	float giaodist;      // rt_metal_gi_ao_dist: the contact range in world units
	float contact;       // BEAUTY B3 (appended 2026-09-17): contact-hardened dominant shadow, 0 = old bytes
	uint32_t shadowlights;    // rt_metal_shadowlights (appended 2026-09-19): brightest lights shadow-tested per pixel, 1 = the dominant alone, the old bytes
	uint32_t shadowlightrays; // rt_metal_shadowlights_rays: rays per runner-up
} RTCam;
// The MSL Cam is all packed_float3 (12 bytes, 4-aligned) and 4-byte scalars, so
// its size is the field count times four with no padding; this pins the C
// mirror to the same arithmetic. The F4 lesson: a short bind reads its tail
// PAST the buffer silently in release and aborts under validation.
_Static_assert(sizeof(RTCam) == 284, "RTCam must mirror the MSL Cam field-for-field, tightly packed");

static id<MTLDevice>               s_dev;
static id<MTLCommandQueue>         s_queue;
// set when the device/queue were handed in by the renderer (RT_Metal_InitWithDevice)
// rather than created here: shutdown must then drop the references without
// implying it owned them. See METAL.md Phase 5.
static int                         s_sharedDevice;
static id<MTLComputePipelineState> s_pso;

// blue-noise jitter (rt_metal_bluenoise, the weave fix): the committed 64x64x8
// table as ONE shared buffer, bound unconditionally at every kernel dispatch
// (binding an index a PSO ignores is legal; the reverse -- declared but
// unbound -- is the validation abort this tree has been burned by). The 0/1
// choice is compiled into the PSOs (#define RT_BLUENOISE) and lazily rebuilt
// when the cvar flips -- a one-off hitch, the tier-crossing precedent.
static id<MTLBuffer>               s_bnbuf;
static int                         s_bluenoiseWant = 1;
static int                         s_bluenoiseCompiled = -1;   // what the live PSOs carry; -1 = never
static void                        rt_compile_kernels(void);   // defined below the init that calls it

// world acceleration structure (BLAS, built once per map)
static id<MTLBuffer>               s_vbuf;      // packed float3 vertices
static id<MTLBuffer>               s_ibuf;      // uint32 indices
static id<MTLBuffer>               s_wabuf;     // per-TRIANGLE RGBA8 mean colour (GIARC G4-2, the coloured
                                                // bounce); world lifecycle exactly -- dropped and rebuilt
                                                // with s_vbuf, degenerate 4 bytes of grey when unsupplied
static id<MTLAccelerationStructure> s_accel;    // world BLAS
static unsigned long               s_worldToken;

// lava sheets: a separate STATIC emissive instance (rt_metal_lavaemissive).
// Shares s_vbuf (SetWorld always runs first); own index buffer and token so a
// cvar toggle rebuilds ONLY this BLAS -- a SetWorld rebuild would drop the
// baked fog field until the next map bake. Degenerate stand-in when empty.
static id<MTLBuffer>               s_libuf;     // lava uint32 indices (or the degenerate triple)
static id<MTLBuffer>               s_lavadegv;  // zero-area degenerate verts (own buffer: world verts 0,1,2 are a REAL triangle)
static id<MTLAccelerationStructure> s_lavaAccel;
static unsigned long               s_lavaToken = (unsigned long)-1;

// sky brushes: the OPEN SKY instance (rt_metal_skyopen). Same shape as lava --
// shares s_vbuf, own index buffer and token -- and shares s_lavadegv for the
// empty stand-in (the degenerate verts are just zeros; one buffer serves both).
static id<MTLBuffer>               s_skyibuf;
static id<MTLAccelerationStructure> s_skyAccel;
static unsigned long               s_skyToken = (unsigned long)-1;

// opaque liquids: the EMISSIVE liquid instance (rt_metal_liquidemissive). Same
// shape a third time -- shares s_vbuf, own index buffer and token, shares
// s_lavadegv for the empty stand-in. Primary rays stop at the sheet and shade
// it emissive (term 1.0); the kernel never fetches its triangles, so unlike
// the reverted SHADED design it needs no trace-side buffer bind at all.
static id<MTLBuffer>               s_liqibuf;
static id<MTLAccelerationStructure> s_liqAccel;
static unsigned long               s_liqToken = (unsigned long)-1;
// SEPTEMBER2 C2 (2026-09-09): the BLENDED liquids, a seventh instance at mask 0x40 that
// only the C2 intersect sees (never the primary mask). Same lifecycle as the opaque list.
static id<MTLBuffer>               s_bliqibuf;
static id<MTLAccelerationStructure> s_bliqAccel;
static unsigned long               s_bliqToken = (unsigned long)-1;
static int                         s_liqrt;            // rt_metal_liquids_rt (gated on rt_metal_liquids > 0 by the caller)
static float                       s_liqReflect = 1.0f;

// dynamic entity geometry (monsters/items/doors), rebuilt every frame in world
// space -> per-frame entity BLAS. ASYNC: everything the CPU rewrites per frame
// (and every AS object the in-flight trace reads) is DOUBLE-BUFFERED by the
// frame parity s_par, so frame N's GPU work can still be reading slot p while
// the CPU fills slot p^1 for frame N+1 (see metal/async-plan.md).
static id<MTLBuffer>               s_evbuf[2];      // entity verts (packed float3, world space)
static id<MTLBuffer>               s_enbuf[2];      // entity vertex NORMALS (packed float3, world space; zeros = none)
static NSUInteger                  s_enbufCap[2];
static int                         s_entHasNorms[2];   // 1 when the slot's normals are real (rt_metal_smoothnormals)
static id<MTLBuffer>               s_eibuf[2];      // entity indices (uint32)
static id<MTLBuffer>               s_escratch[2];   // entity BLAS build scratch
static id<MTLAccelerationStructure> s_entityAccel[2]; // entity BLAS
static NSUInteger                  s_evbufCap[2], s_eibufCap[2], s_entityAccelCap[2], s_escratchCap[2];
static int                         s_numentitytris;
// LIGHT-CORE stream: flame models in their own BLAS/instance so shadow rays can
// skip them (they sit ON their light's origin and blocked their own light) and
// the surface kernel can shade them emissive. Double-buffered by s_par exactly
// like the entity set; DEDICATED scratch -- both BLAS builds share an encoder
// and may overlap, so s_escratch must never be reused here.
static id<MTLBuffer>               s_lcvbuf[2], s_lcibuf[2];
static NSUInteger                  s_lcvbufCap[2], s_lcibufCap[2];
static id<MTLAccelerationStructure> s_lcAccel[2];
static NSUInteger                  s_lcAccelCap[2];
static id<MTLBuffer>               s_lcscratch[2];
static NSUInteger                  s_lcscratchCap[2];
static int                         s_numlctris;

// top-level instance AS (instance 0 = world BLAS, instance 1 = entity BLAS),
// rebuilt every frame (2 instances -> trivial cost). The kernel traces this.
static id<MTLBuffer>               s_instbuf[2];    // 7x MTLAccelerationStructureInstanceDescriptor per slot (SEPTEMBER2 C2: was 6)
static id<MTLBuffer>               s_tscratch[2];   // TLAS build scratch
static id<MTLAccelerationStructure> s_tlas[2];
static NSUInteger                  s_tlasCap[2], s_tscratchCap[2];

// async pipeline: parity of the slot being ENCODED this frame; the composite
// draws the other slot (previous frame's trace). s_pendingCB[p] is the command
// buffer whose trace writes s_surf[p]; s_syncMode (RT_METAL_SYNC=1) restores
// the old trace-then-composite-same-frame order for A/B verification.
static int                         s_par;
static id<MTLCommandBuffer>        s_pendingCB[2];
static int                         s_syncMode;
// RT_METAL_VERBOSE=1: per-change diagnostic chatter (the view-culled light
// count, and anything else that fires at per-event rates during play). OFF by
// default since the 92 GB incident: under Cmd+R every stderr line accumulates
// in Xcode's debug console FOREVER, at large per-line overhead -- the light
// count alone measured 1,350 lines per 72-second demo11 run (~19/s under
// ordinary movement), and an hour of play ran the console to 92.16 GB and an
// out-of-application-memory halt. The game process itself measured flat
// (735 MB across a 14-minute synthetic horde soak); the leak was the console.
static int                         s_verbose;
// RT_METAL_PROFILE=1: the 120-frame profile line (and KERNELMS implies it).
// Ungated it printed ~1 line per 2 seconds into every ordinary session's
// console from the moment 8-1c gave the Metal path the report -- the only
// high-rate line no config change could turn off. The bench harness sets it.
static int                         s_profile;

// RT_METAL_KERNELMS=1 measurement mode: each stage (AS+trace / fog / shaft)
// commits its own command buffer so per-stage GPUStartTime/GPUEndTime are
// separable; a second stderr profile line reports them. Commit order keeps
// queue execution order, and the stages already depend on each other through
// tracked resources (fog/shaft read the history the trace writes), so the
// existing shown-slot wait still covers everything. Slightly perturbs async
// overlap — use for RELATIVE attribution, not absolute frame cost.
static int                         s_kernelMs;
static id<MTLCommandBuffer>        s_kcb[2][4];    // per-slot stage CBs: 0=AS+trace, 1=fog, 2=shaft, 3=AS alone
static double                      s_kernAccum[4]; // 120-frame GPU-ms sums for the report line
static int                         s_kernN;
// RT_METAL_ASSPLIT=1 (needs KERNELMS): commit the acceleration-structure builds
// in their OWN command buffer so stage 0 becomes the trace dispatch ALONE and
// stage 3 is the AS work. It answers a question arithmetic could not: the
// trace-scale ladder implies ~61% of that stage is fixed w.r.t. trace
// resolution, and the stage has always been labelled AS+trace, so "the fixed
// floor" and "the acceleration structures" had never been separated by
// measurement. It is a SUB-MODE rather than a change to KERNELMS because
// splitting a command buffer perturbs overlap, and every trace figure on record
// -- including perf-sweep.sh's parsed line -- was taken unsplit; changing that
// silently would invalidate the comparisons rather than extend them.
static int                         s_asSplit;

// real lights (7 floats each: origin xyz, radius, colour rgb). The first
// s_numdynamic of them are DYNAMIC (prepended by the caller); only those get the
// colored brighten. Double-buffered by slot parity like the entity buffers:
// SetLights writes slot s_par while the OTHER slot's command buffer may still be
// reading its own copy in flight — the written slot's previous CB was waited on
// at the last composite, so the copy never races the GPU. Grow-only, reused.
static id<MTLBuffer>               s_lightbuf[2];
static NSUInteger                  s_lightbufCap[2];
static int                         s_numlights;    // describe slot s_par's contents —
static int                         s_numdynamic;   // refreshed every frame before the encode

// camera for the next trace
static RTCam                       s_cam;
static int                         s_hasCam;
static int                         s_active;   // 1 if the last composite fully ran (RT is relighting)
static uint32_t                    s_frameCounter;   // drives the jitter rotation (wraps at 1024)
static int                         s_framePin;       // RT_METAL_FRAMEPIN: hold that rotation at 0 (see RT_Metal_InitWithDevice)
static uint32_t                    s_frameOffset;    // RT_METAL_FRAMEOFFSET: the counter's value at each temporal reset (independent realisations for a truth)
static float                       s_scale = 1.0f;   // trace resolution / viewport (rt_metal_scale)
static int                         s_reproject = 1;  // rt_metal_reproject: composite reprojects the shown term
static int                         s_reprojDepth = 1; // rt_metal_reproject_depth: and does it translation-aware
static int                         s_sameFrame;      // rt_metal_sameframe: show THIS frame's trace (the RT_METAL_SYNC slot order, as a runtime cvar)

// The camera each slot's trace was ENCODED with. The composite SHOWS a slot one
// frame later, by which time s_cam holds the new frame's camera -- without this
// snapshot the term is multiplied on screen-locked and one frame stale, which
// reads as the RT lighting trailing the view on fast mouse turns (the reported
// "drunken" effect). Rotation-only reprojection through this basis fixes it.
typedef struct {
	float origin[3], forward[3], right[3], up[3];
	float tanx, tany;
	int valid;
} rt_slotcam_t;
static rt_slotcam_t                s_slotCam[2];
static GLint                       s_gltexFilterCur;   // the term pair's current GL min/mag filter

// shared surfaces + GL views (double-buffered: trace writes slot s_par while
// the composite reads slot s_par^1)
static IOSurfaceRef   s_surf[2];
static id<MTLTexture> s_mtex[2];
static GLuint         s_gltex[2];
static int            s_w, s_h;
static CGLContextObj  s_glctx;

// Shared-device path only: the renderer's own handle for each slot's term
// texture, so the composite can bind it like any other texture (slice 5-3).
// The GL path leaves all three arrays at zero -- there the handle IS s_gltex.
//
// s_termHandleTexPtr holds the ADDRESS of the MTLTexture the handle was minted
// for, and is compared for identity only, never dereferenced. It is what makes
// a stale handle impossible: rt_pair_ensure can replace s_mtex[slot] on a
// resize, and the handle table holds its own strong reference, so without the
// pointer check a handle would keep the OLD texture alive and go on showing it
// with nothing anywhere saying so.
static int   s_termHandle[2];
static void *s_termHandleTexPtr[2];
static int   s_termHandleFlags[2];
// METAL.md Phase 6-3: the same adoption for the FOG and SHAFT outputs. Their
// flags never vary -- both are sampled LINEAR and clamped, exactly as their GL
// rectangle names are -- so unlike the term above these need no flags cache,
// only the pointer-identity one. Pointer identity and not presence, for the 5-1
// reason: rt_pair_ensure can replace the MTLTexture on a resize while the
// renderer's handle table holds its own strong reference, and a presence check
// would keep showing the OLD texture with nothing anywhere saying so.
#define RT_ADOPT_FOGFLAGS (TEXF_FORCELINEAR | TEXF_CLAMP)
static int   s_fogHandle[2];
static void *s_fogHandleTexPtr[2];
// SEPTEMBER2 C2: the liquid pair (own term | reflection, side by side at DOUBLE the trace
// width -- one texture, one adopted handle, one sampler index, because DP_TEX_RTTERM is the
// last surface texture and appending exactly one index after it is the safe case of the
// rebasing class). Allocated lazily on the first frame the arm is on; released with the
// main pair. The handles ride rt_adopt_release like the fog's.
static IOSurfaceRef   s_liqsurf[2];
static id<MTLTexture> s_liqmtex[2];
static GLuint         s_liqgltex[2];
static int            s_liqHandle[2];
static void          *s_liqHandleTexPtr[2];
static int   s_shHandle[2];
static void *s_shHandleTexPtr[2];
static int rt_adopt_slot(void *texptr, int *handle, void **cached, int w, int h);

// temporal accumulation: ping-pong history textures (RGBA32F, .xyz = world hit
// position, .w = accumulated visibility). GPU-private; reallocated on resize.
static id<MTLTexture> s_histTex[2];
static id<MTLTexture> s_secTex[2];    // rt_metal_lightsample: the secondary estimate's EMA (ping-pongs with s_histParity)
static id<MTLTexture> s_giTex[2];     // rt_metal_gi: the GI colour EMA, its OWN pair (GIARC: s_secTex is retained for mode-2 A/B)
static int            s_histParity;   // read s_histTex[parity], write the other

// god-ray shaft pass: its own PSO and surface pair (reduced resolution, GL_LINEAR
// because the murk magnifies it), slot-indexed like s_surf so the existing
// shown-slot wait covers the shaft encoder too
static id<MTLComputePipelineState> s_shaftPso;
static IOSurfaceRef   s_shsurf[2];
static id<MTLTexture> s_shmtex[2];
static GLuint         s_shgltex[2];
static int            s_shw, s_shh;      // shaft buffer size (own key, != s_w/s_h)
static int            s_shValid[2];      // slot holds a fully-encoded shaft frame
static int            s_shHasPrev;       // previous shaft output usable for temporal blend
static int            s_lastShown = -1;  // slot the last composite showed (and waited on)

// --- fog-kernel state -------------------------------------------------------
static int   s_fogEnable;
static int   s_fogSteps = 16;
static float s_fogScale = 0.5f;
static float s_fogHistory = 0.5f;
static float s_fogIntensity = 0.5f;  // matches the cvar default (the shouldered midpoint)
static int   s_fogStride = 2;
static float s_fogResidual;
static float s_fogBeams = 0.5f;      // density-independent beam gain (matches the cvar default)
static int   s_fogHistCentre = 0;    // rt_metal_fog_upsample: the kernel reads its march end at the texel centre
static int   s_fogStepJitter = 0;    // rt_metal_fog_stepjitter (S2): per-step jitter spread along the fog ray
static int   s_fogFilter = 0;        // rt_metal_fog_filter (S1): 0 off, 1 = 3x3, 2 = 5x5 depth-aware filter at fog res
static float s_fogFilterDepth = 0.1f; // S1: relative depth tolerance for the filter's affinity
// BLUENOISE slice 2 (2026-09-03): the fog history gets a neighbourhood clamp,
// and with it a DEEPER ceiling. Today rt_fog blends its own output with last
// frame's FILTERED surface (an EMA whose feedback is re-filtered every frame,
// so at history h the display carries the filter ~1/(1-h) times over) and
// nothing validates what the reprojected read returned -- which is why the
// EMA is capped at 0.9 C-side and the parked floor stops there: deeper
// history without a clamp is ghosting on every light flick, door and
// teleport. With rt_metal_fog_clamp on, rt_fog writes the RAW scatter (its
// own EMA skipped by hasPrev = 0; the jitter rotation is gated on history,
// so it keeps turning), rt_fogtemporal clamps the reprojected history to the
// current frame's 3x3 neighbourhood (mode 1 min/max, mode 2 mean +/- k sigma,
// mode 3 no clamp -- the A/B that isolates the reorder) and accumulates into
// s_fogema, and the spatial filter runs ONCE on that for display. The
// history ceiling is 0.95 under the clamp and stays 0.9 off it. 0 = today's
// path byte for byte (rt_fog's text is untouched: the kernel is simply
// handed hasPrev = 0).
static id<MTLTexture> s_fogema[2];   // the EMA history under the clamp (ping-pong by slot, like the surfaces)
static int   s_fogEmaValid[2];       // slot holds a settled history (0 after a toggle / resize)
static int   s_fogClamp = 0;         // rt_metal_fog_clamp: 0 off, 1 min/max, 2 variance clip, 3 decoupled EMA only
static float s_fogClampK = 1.5f;     // rt_metal_fog_clamp_k: the variance clip's sigma multiplier
static int   s_fogTonemapEma = 0;    // rt_metal_fog_tonemapema (slice 3): blend in a c/(1+luma) domain
static id<MTLComputePipelineState> s_fogTemporalPso;
static id<MTLTexture> s_fogdepth[2];  // rt_fog's scatter-centroid depth per slot (R16F; the history pass reprojects through it)
static int   s_fogReprojDepth = 0;   // rt_metal_fog_reproject_depth: translation-aware history read (needs the clamp pass)
// A1 FROXEL (2026-09-06): rt_metal_fog_froxel. The fog kernel compiled a second
// time with RT_FROXEL 1 (lazily, on first enable -- the source is kept from
// the fog kernel's own compile so the two share every define), the integrate
// pass, and the two volume pairs: A = (radiance x density, density), B = (beam
// light, lava glow), ping-pong by slot like every other history here.
static id<MTLComputePipelineState> s_froxelPso;
static id<MTLComputePipelineState> s_froxelIntPso;
static NSString *s_froxelSrc;        // the RT_FROXEL 1 fog source, rebuilt whenever the fog kernel is
static int   s_froxelTried = 0;      // compile attempted (a failure prints once and disables the arm)
static int   s_froxel = 0;           // rt_metal_fog_froxel
static int   s_froxelSlices = 24;    // rt_metal_fog_froxel_slices
static float s_froxelHistory = 0.9f; // rt_metal_fog_froxel_history
static float s_froxelCurve = 0.0f;   // rt_metal_fog_froxel_curve (0 = derived from s_froxelNear)
static float s_froxelNear = 6.0f;    // rt_metal_fog_froxel_near: the first cell's depth in world units
static int   s_froxelCastPhase = 0;  // rt_metal_fog_froxel_castphase (measured negative; A/B only)
static id<MTLTexture> s_froxA[2], s_froxB[2];
static int   s_froxValid[2];         // slot holds a settled volume (0 after a toggle / resize / temporal reset)
static int   s_froxW, s_froxH, s_froxN;
static float s_fogReprojTol = 0.3f;  // rt_metal_fog_reproject_tol: relative centroid-depth disagreement that rejects the history
static int   s_lsHybrid = 0;         // rt_metal_lightsample_hybrid: 0 pick alone, 1 dominant + pick (two rays), 2 alternating (one ray), 3 single-pass (one ray, no second loop)
static id<MTLComputePipelineState> s_fogFilterPso;
static id<MTLTexture> s_fograw[2];   // S1: the fog kernel's RAW output when the filter is on (the filter writes s_fogmtex)
static int   s_lightSample = 0;      // rt_metal_lightsample MODE: 0 off, 1 fog-only (fog+shaft pick, surface dominant-only), 2 full
static float s_lsClamp = 0.0f;       // rt_metal_lightsample_clamp: fog/shaft 1/p ceiling (0 = unclamped)
static int   s_giEnable = 0;         // rt_metal_gi (GIARC G1)
static int   s_giBias = 0;           // RT_METAL_GIBIAS: the measure-only GI debug arm (GIARC G4)
static float s_giHistoryRaw = 0.9f;  // the cvar's value before the stillness floor (applied at encode, beside the fog/shaft floors)
// WARCHEST session 2 (rt_metal_refit): refit the dynamic BLAS in place when
// topology allows. Per-slot book-keeping: the triangle count each slot's AS
// was last BUILT with (a refit is only valid against that), whether it was
// built with the Refit usage (an AS built without it cannot be refitted), and
// how many consecutive refits it has taken (Quake's animation is jumpy, so a
// long-refitted BVH loosens -- force a rebuild on a cadence).
#define RT_REFIT_REBUILD_EVERY 16
static int        s_refitMode;
static NSUInteger s_entityAccelTris[2], s_lcAccelTris[2];
static int        s_entityRefittable[2], s_lcRefittable[2];
static int        s_refitAgeE[2], s_refitAgeL[2];
static unsigned   s_refitCount, s_buildCount;   // KERNELMS counter line
// SEPTEMBER2 A3 (2026-09-06): rt_metal_as_skipstatic. The entity and light-core
// BLASes were refit EVERY frame with no "nothing moved" arm (the 2026-08-30 AS
// floor record). Each gather now hashes the bytes it uploads; a slot whose AS
// was last built or refit from exactly these bytes (and this triangle count)
// skips its refit, and when BOTH skip and the TLAS was built against the same
// six structure objects the TLAS build is skipped too -- the whole AS stage
// gone for that frame. Byte-exact by construction: a skipped refit of identical
// input is the identical BVH. What it can buy in PLAY is bounded by how often
// nothing animates: every flame model lerps every frame, so a torch in view
// keeps the light-core BLAS moving; the counter line says the share.
static int        s_asSkip;                     // rt_metal_as_skipstatic
// SEPTEMBER2 D: the sky light (RT_Metal_SetSun)
static float      s_sunDir[3], s_sunCol[3], s_sunPen;
static int        s_sun;
static uint64_t   s_evHash[2], s_lcHash[2];     // hash of the slot's uploaded bytes (0 = none)
static uint64_t   s_entityAccelHash[2], s_lcAccelHash[2];   // the hash the slot's AS was last built/refit from
static int        s_tlasBuilt[2];               // the slot's TLAS holds a completed build against s_tlasStatic
static const void *s_tlasStatic[2][7];          // the six structure objects that TLAS was built against (identity only, never dereferenced)
static unsigned   s_skipCount;                  // frames whose whole AS stage was skipped (KERNELMS counter line)
static uint64_t rt_hash64(const void *data, size_t bytes, uint64_t seed)
{
	// 8 bytes at a time, multiply-rotate mixing; fast enough for the entity
	// stream's few hundred KB a frame (the CPU sits at ~0.6 of a core).
	const uint8_t *b = (const uint8_t *)data;
	uint64_t h = seed ^ 0x9E3779B97F4A7C15ull;
	size_t i = 0;
	for (; i + 8 <= bytes; i += 8) { uint64_t w; memcpy(&w, b + i, 8); h ^= w; h *= 0xBF58476D1CE4E5B9ull; h = (h << 31) | (h >> 33); }
	uint64_t tail = 0; memcpy(&tail, b + i, bytes - i); h ^= tail; h *= 0x94D049BB133111EBull; h ^= h >> 29;
	return h ? h : 1;   // 0 is the "no hash" sentinel
}
static int        s_fogAdStride;   // rt_metal_fog_stride_adaptive (WARCHEST session 3)
static rt_fog_shade_t s_fogShade;    // per-frame density-model parameters
static int   s_hasFogShade;
// CPU copies of the volumes (the engine frees its own buffers immediately);
// uploaded lazily into 3D textures when the device exists and a dirty flag is set
static unsigned char *s_noiseCopy;   // size^3 * 4
static int            s_noiseSize;
static int            s_noiseDirty;
static unsigned char *s_fieldCopy;   // size[0]*size[1]*size[2] * 4
static int            s_fieldSize[3];
static int            s_fieldDirty;
static unsigned char *s_irrCopy;     // size[0]*size[1]*size[2] * 4, the ambient irradiance grid
static int            s_irrSize[3];
static int            s_irrDirty;
// device-lifetime 3D textures (NOT tied to the GL context; survive vid_restart)
static id<MTLTexture> s_noiseTex3D;
static id<MTLTexture> s_fieldTex3D;
static id<MTLTexture> s_irrTex3D;
// fog kernel output pair: same slotting as the shaft pair, GL_LINEAR rect
static id<MTLComputePipelineState> s_fogPso;
static IOSurfaceRef   s_fogsurf[2];
static id<MTLTexture> s_fogmtex[2];
static GLuint         s_foggltex[2];
static int            s_fogw, s_fogh;
static int            s_fogValid[2];
static int            s_fogHasPrev;
static int            s_hasPrev;      // previous frame's history + camera are valid (0 after (re)alloc)
// previous-frame camera basis, fed to the kernel for reprojection
static float          s_prevOrigin[3], s_prevForward[3], s_prevRight[3], s_prevUp[3];
static float          s_prevTanx, s_prevTany;

// verification: console-requested one-shot backbuffer dump (RT_Metal_RequestDump)
static int            s_dumpNow;

// profiling: the per-frame AS rebuild runs in its own command buffer and is
// waited on separately, so its cost is invisible in the trace timing. Recorded
// here (GPU ms, and the CPU wall ms actually lost to the commit+wait round trip)
// and folded into the 120-frame report.
static double         s_asGpuMs, s_asCpuMs;
static double rt_now_ms(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) * 1.0e-6; }

// GL composite program (multiply the RT shadow term into the scene)
static GLuint s_prog;
static GLuint s_vao;
static GLint  s_locRtoff = -1;   // "rtoff" uniform (viewport origin in the bound fbo)
static GLint  s_locRtscale = -1; // "rtscale" uniform (trace texels per viewport pixel)
static GLint  s_locRp[10] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};  // reprojection uniforms (see kCompFS)
static GLint  s_locUp = -1, s_locUp2 = -1, s_locS2D = -1;      // term-upsample uniforms (see kCompFS)

// Fullscreen triangle from gl_VertexID (no vertex buffer needed).
static const char *kCompVS =
"#version 150\n"
"void main() {\n"
"    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
"    // z = -0.875 -> window depth 0.0625 under glDepthRange(0,1). That is exactly\n"
"    // the near depth-range the engine draws the first-person view model into\n"
"    // (gl_rmain.c: GL_DepthRange(0, 0.0625) for RENDER_VIEWMODEL / short-depth\n"
"    // materials like the muzzleflash). Drawn depth-tested GL_LESS, the composite\n"
"    // then passes only where the stored scene depth is GREATER than 0.0625 (the\n"
"    // world), and is rejected over the gun/muzzleflash — so the RT shadow no\n"
"    // longer darkens the first-person weapon with the world's shadow behind it.\n"
"    gl_Position = vec4(p * 2.0 - 1.0, -0.875, 1.0);\n"
"}\n";

// Sample the RT term (an HDR RGB multiplier: <1 darkens in shadow, can exceed 1
// to brighten toward a dynamic light's colour) and output it; drawn with multiply
// blend (GL_DST_COLOR,GL_ZERO) so scene *= term per channel.
// rtoff = the GL viewport's origin within the bound framebuffer. gl_FragCoord is
// in FRAMEBUFFER space but the RT texture is only viewport-sized, so subtracting
// the origin maps one to the other. It is (0,0) for a full-framebuffer view and
// nonzero when the 3D view is a subregion (scr_viewsize < 100, r_letterbox, the
// side-by-side stereo eyes).
// rtscale = trace resolution / viewport (rt_metal_scale): maps viewport pixels
// to trace texels. Exactly (1,1) at full res, so the multiply is bit-neutral.
// REPROJECTION (rt_metal_reproject): the shown term was traced one frame ago
// with the SHOWN slot's camera. Rotation-only remap: build this pixel's ray in
// the CURRENT camera basis, project it into the shown basis, sample the term
// there. Exact for mouse turns (direction is depth-independent); translation
// leaves a few units of residual, invisible on the low-frequency term. No depth
// read anywhere (sampling the bound FBO's depth is a feedback loop).
//   rpCurF/R/U = current basis, R and U pre-scaled by tanx/tany
//   rpShF/R/U  = shown basis, R and U pre-DIVIDED by its tanx/tany
//   rpParams   = (enable, tracew, traceh, unused); rpView = viewport size
static const char *kCompFS =
"#version 150\n"
"uniform sampler2DRect rttex;\n"
"uniform sampler2D rtdepthtex;\n"     // TERM UPSAMPLE: the scene depth texture (offscreen path only; 0 disables)
"uniform vec4 rtUp;\n"                // (term w, term h, smooth tol, edge tol); z <= 0 = plain bilinear, byte for byte
"uniform vec4 rtUp2;\n"               // viewport (x, y, w, h), GL window coords -- LOCKSTEP shader_msl.h MODE_RTCOMPOSITE
"uniform vec2 ScreenToDepth;\n"
"uniform vec2 rtoff;\n"
"uniform vec2 rtscale;\n"
"uniform vec3 rpCurF;\n"
"uniform vec3 rpCurR;\n"
"uniform vec3 rpCurU;\n"
"uniform vec3 rpShF;\n"
"uniform vec3 rpShR;\n"
"uniform vec3 rpShU;\n"
"uniform vec4 rpParams;\n"
"uniform vec2 rpView;\n"
"uniform vec3 rpCurO;\n"
"uniform vec3 rpShO;\n"
"out vec4 frag;\n"
"void main() {\n"
"    vec2 tc = (gl_FragCoord.xy - rtoff) * rtscale;\n"
"    if (rpParams.x > 0.5) {\n"
"        vec2 ndc = (gl_FragCoord.xy - rtoff) / rpView * 2.0 - 1.0;\n"
"        vec3 dir = rpCurF + ndc.x * rpCurR + ndc.y * rpCurU;\n"
"        float tz = dot(dir, rpShF);\n"
"        if (tz > 0.0001) {\n"
"            vec2 f = (vec2(dot(dir, rpShR), dot(dir, rpShU)) / tz + 1.0) * 0.5 * rpParams.yz;\n"
"            tc = clamp(f, vec2(0.5), rpParams.yz - 0.5);\n"   // clamp to edge: a smeared rim beats a bright one
             // TRANSLATION-AWARE REFINE (rpParams.w). The rotation-only remap
             // above is exact for turning the head, because a direction does not
             // care where the eye is -- but it ignores the camera MOVING between
             // the frame that was traced and the frame being shown. At arm's
             // length that parallax is what fringes an entity's silhouette.
             // One fixed-point iteration fixes it: the rotation-only answer tells
             // us roughly which traced pixel this is, alpha there gives that
             // pixel's hit DISTANCE, and distance along this pixel's own ray
             // reconstructs the world point -- which we then project through the
             // FULL previous camera, origin included.
"            if (rpParams.w > 0.5) {\n"
"                float hd = texture(rttex, tc).a;\n"
"                if (hd > 0.0) {\n"                            // 0 = sky or no hit: nothing to refine against
"                    vec3 P = rpCurO + normalize(dir) * hd;\n"
"                    vec3 v = P - rpShO;\n"
"                    float tz2 = dot(v, rpShF);\n"
"                    if (tz2 > 0.0001) {\n"
"                        vec2 f2 = (vec2(dot(v, rpShR), dot(v, rpShU)) / tz2 + 1.0) * 0.5 * rpParams.yz;\n"
                         // Disocclusion guard. A refine that jumps a long way has
                         // crossed a silhouette, so the distance we sampled belongs
                         // to different geometry and the correction is worse than
                         // the error -- keep the rotation-only answer there.
"                        if (all(lessThan(abs(f2 - f), rpParams.yz * 0.08)))\n"
"                            tc = clamp(f2, vec2(0.5), rpParams.yz - 0.5);\n"
"                    }\n"
"                }\n"
"            }\n"
"        }\n"
"    }\n"
     // TERM UPSAMPLE (rt_metal_term_upsample, WARCHEST session 1) -- the fog
     // upsample's shape applied to the lighting term. LOCKSTEP with the MSL
     // arm in shader_msl.h MODE_RTCOMPOSITE, which carries the full comment;
     // the one difference is that tc here is already in TEXELS (rect sampler),
     // so the texel arithmetic drops the * tsz.
"    if (rtUp.z > 0.0) {\n"
"        vec2 tsz = rtUp.xy;\n"
"        vec2 fp = tc - 0.5;\n"
"        vec2 ff = fract(fp);\n"
"        vec2 i0 = floor(fp);\n"
"        vec2 ia = clamp(i0, vec2(0.0), tsz - 1.0);\n"
"        vec2 ib = clamp(i0 + 1.0, vec2(0.0), tsz - 1.0);\n"
"        vec2 dsz = vec2(textureSize(rtdepthtex, 0));\n"
"        float dpx = min(-(ScreenToDepth.y / (texture(rtdepthtex, gl_FragCoord.xy / dsz).r + ScreenToDepth.x)), 1.0e7);\n"
"        float d00 = min(-(ScreenToDepth.y / (texture(rtdepthtex, (rtUp2.xy + (ia + 0.5) / tsz * rtUp2.zw) / dsz).r + ScreenToDepth.x)), 1.0e7);\n"
"        float d10 = min(-(ScreenToDepth.y / (texture(rtdepthtex, (rtUp2.xy + (vec2(ib.x, ia.y) + 0.5) / tsz * rtUp2.zw) / dsz).r + ScreenToDepth.x)), 1.0e7);\n"
"        float d01 = min(-(ScreenToDepth.y / (texture(rtdepthtex, (rtUp2.xy + (vec2(ia.x, ib.y) + 0.5) / tsz * rtUp2.zw) / dsz).r + ScreenToDepth.x)), 1.0e7);\n"
"        float d11 = min(-(ScreenToDepth.y / (texture(rtdepthtex, (rtUp2.xy + (ib + 0.5) / tsz * rtUp2.zw) / dsz).r + ScreenToDepth.x)), 1.0e7);\n"
"        float b00 = (1.0 - ff.x) * (1.0 - ff.y);\n"
"        float b10 = ff.x * (1.0 - ff.y);\n"
"        float b01 = (1.0 - ff.x) * ff.y;\n"
"        float b11 = ff.x * ff.y;\n"
"        float dref = max(dpx, 1.0);\n"
"        if (abs(b00 * d00 + b10 * d10 + b01 * d01 + b11 * d11 - dpx) > rtUp.z * dref) {\n"
"            float te = max(rtUp.w * dref, 0.001);\n"
"            float w00 = b00 * (1.0 - smoothstep(0.0, te, abs(d00 - dpx)));\n"
"            float w10 = b10 * (1.0 - smoothstep(0.0, te, abs(d10 - dpx)));\n"
"            float w01 = b01 * (1.0 - smoothstep(0.0, te, abs(d01 - dpx)));\n"
"            float w11 = b11 * (1.0 - smoothstep(0.0, te, abs(d11 - dpx)));\n"
"            float wsum = w00 + w10 + w01 + w11;\n"
"            if (wsum > 0.001) {\n"
"                vec3 mr = (texelFetch(rttex, ivec2(ia)).rgb * w00\n"
"                         + texelFetch(rttex, ivec2(int(ib.x), int(ia.y))).rgb * w10\n"
"                         + texelFetch(rttex, ivec2(int(ia.x), int(ib.y))).rgb * w01\n"
"                         + texelFetch(rttex, ivec2(ib)).rgb * w11) / wsum;\n"
"                frag = vec4(mr, 1.0);\n"
"                return;\n"
"            }\n"
"        }\n"
"    }\n"
"    vec3 m = texture(rttex, tc).rgb;\n"
"    frag = vec4(m, 1.0);\n"
"}\n";

// Block until any in-flight (async) GPU frame is finished, then forget it.
// Called before anything the in-flight work might still read is torn down.
static void rt_wait_pending(void)
{
	for (int i = 0; i < 2; i++) {
		if (s_pendingCB[i]) { [s_pendingCB[i] waitUntilCompleted]; s_pendingCB[i] = nil; }
		for (int k = 0; k < 4; k++) {
			if (s_kcb[i][k]) { [s_kcb[i][k] waitUntilCompleted]; s_kcb[i][k] = nil; }
		}
	}
}

// METAL.md Phase 5 enabler: the sidecar can be handed the RENDERER's device and
// queue instead of making its own. Two MTLDevice objects for one GPU are legal
// but their resources cannot be shared, which is exactly what forces today's
// IOSurface/CGL bridge. Once vid_metal.m owns a device, the sidecar shares it
// and its outputs become plain MTLTextures the renderer samples -- so Phase 5
// is mostly deletion rather than new code.
//
// Passing nil/nil reproduces the old behaviour exactly (create our own), which
// is what the GL path does, so this is a provable no-op there.
void RT_Metal_InitWithDevice(void *device, void *queue)
{
	@autoreleasepool {
		if (s_dev)
		{
			// The early-out keeps whatever device we already hold and DISCARDS
			// the one just offered. That is correct only because VID_Shutdown
			// brings the sidecar down between renderpaths -- vid_sdl.c's
			// shutdown comment states that invariant in prose and it holds
			// today. It is an invariant owned by a distant caller, though,
			// which is the shape slice 5-1 found four times over (the `ensure`
			// predicates keyed on the wrong third of the triple), so slice 5-6
			// makes it CHECKED rather than assumed.
			//
			// If it ever slips, the sidecar goes on serving the OLD device and
			// every texture it publishes into the renderer's table is a
			// cross-device resource -- bound into a live encoder, with nothing
			// anywhere saying so. Report and keep the old behaviour: the
			// warning is the whole value, and recovering here would be a second
			// untested path on a route that is currently unreachable.
			int wantShared = device ? 1 : 0;
			id<MTLDevice> want = device ? (__bridge id<MTLDevice>)device : nil;
			if (wantShared != s_sharedDevice || (want && want != s_dev))
				fprintf(stderr, "RT_Metal: WARNING: re-init on a %s device while a %s sidecar is still up. "
				                "VID_Shutdown must bring it down between renderpaths (METAL.md 5-6); "
				                "keeping the old device, expect cross-device resources.\n",
				        wantShared ? "renderer-shared" : "private",
				        s_sharedDevice ? "renderer-shared" : "private");
			return;
		}
		if (device)
		{
			s_dev = (__bridge id<MTLDevice>)device;
			s_queue = queue ? (__bridge id<MTLCommandQueue>)queue : nil;
			s_sharedDevice = 1;
		}
		// Belt and braces on the adopted-handle table. VID_Shutdown now brings the
		// sidecar down on both renderpaths (slice 5-3-1), so rt_adopt_release has
		// already run and these are already zero -- but a handle surviving into a
		// session whose texture table was rebuilt would index SOMEONE ELSE'S
		// texture after slot reuse, and bind it with no error anywhere. Zero, not
		// destroy: if the table did go away, the handles no longer refer to us.
		for (int i = 0; i < 2; i++) { s_termHandle[i] = 0; s_termHandleTexPtr[i] = NULL; s_termHandleFlags[i] = 0; }
		// RT_METAL_SYNC=1: verification fallback — trace and composite the SAME
		// frame (the pre-async order), for byte-exact A/B against old builds.
		{ const char *e = getenv("RT_METAL_SYNC"); s_syncMode = (e && atoi(e)) ? 1 : 0; }
		{ const char *e = getenv("RT_METAL_VERBOSE"); s_verbose = (e && atoi(e)) ? 1 : 0; }
		{ const char *e = getenv("RT_METAL_PROFILE"); s_profile = (e && atoi(e)) ? 1 : 0; }
		// RT_METAL_KERNELMS=1: per-stage command buffers + a second profile line
		// with separable per-kernel GPU times (see the s_kcb declaration).
		{ const char *e = getenv("RT_METAL_KERNELMS"); s_kernelMs = (e && atoi(e)) ? 1 : 0; }
		{ const char *e = getenv("RT_METAL_ASSPLIT"); s_asSplit = (s_kernelMs && e && atoi(e)) ? 1 : 0; }
		// RT_METAL_FRAMEPIN=1: hold the jitter rotation at phase 0 instead of
		// advancing it per composite (METAL.md Phase 5). What this buys is
		// narrower than it first looks, and the narrow version is the useful
		// one -- all three numbers below are measured on the frozen e1m3 bed,
		// two boots, GL:
		//
		//   rt_metal_history 0,   no pin  ->  0 px differ
		//   rt_metal_history 0.5, no pin  ->  25261 px differ (8.2%, max 4)
		//   rt_metal_history 0.5, pinned  ->  0 px differ
		//
		// So a history-0 bed needs NO pin, and that is not luck: the kernels
		// read cam.frame only under `cam.history > 0.0f` (see the `float jf =`
		// line in each of the three kernel sources), so with history off the
		// jitter falls back to pixel-derived noise, which cannot vary per
		// frame. Setting the three histories to 0 is therefore the whole of
		// the determinism story for the ordinary case.
		//
		// The pin exists for the case that is otherwise UNBEDDABLE: history
		// ON. There the frame counter rotates the sample set and the EMA
		// accumulates over however many frames elapsed before the screenshot,
		// which is load-dependent and differs between backends because the
		// Metal restart and shader compile take longer. Pinning collapses
		// both at once -- every trace becomes identical, so the EMA converges
		// geometrically to that same image (0.75^N, under 1e-6 by fifty
		// frames) no matter how many frames it took. That is what lets a bed
		// exercise the temporal path at all rather than only the single-frame
		// one. Note RT_Metal_ResetTemporal could not have done this: it zeroes
		// the phase, but the phase at screenshot time is (frames since reset),
		// which still varies.
		//
		// An env var rather than a cvar, matching RT_METAL_SYNC/NOTILE/KERNELMS
		// above: verification aids stay out of the cvar namespace so they can
		// never be archived into a player's config.
		{ const char *e = getenv("RT_METAL_FRAMEPIN"); s_framePin = (e && atoi(e)) ? 1 : 0; }
		// RT_METAL_FRAMEOFFSET=N (2026-09-03): start the jitter/pick frame counter at N
		// instead of 0, so N playbacks of one timedemo give N INDEPENDENT
		// realisations of the same frames -- their mean is the unbiased truth a
		// single 1-spp history-0 playback is not (its texels are frequently dark
		// from zero picks, so every smoother arm reads "brighter than truth" at
		// the median). Bed-only, like FRAMEPIN.
		{ const char *e = getenv("RT_METAL_FRAMEOFFSET"); s_frameOffset = e ? (uint32_t)atoi(e) : 0u; }
		// GIARC G4, MEASURE-ONLY. 1 = how often is the brightest light REACHING
		// a bounce point outside the PRIMARY pixel's tile list (item 4, the
		// bias G1 states and the brief says to measure before building the
		// world-space grid that would fix it). 2 = what do bounce rays DO --
		// hit, miss out of range, or escape through sky (item 3's premise: a
		// sky term can only beat a plain rt_metal_ambient raise if sky is
		// reachable from enough of the scene to vary spatially). Both ride the
		// term buffer out through RT_METAL_TERMDUMP.
		{ const char *e = getenv("RT_METAL_GIBIAS"); s_giBias = e ? atoi(e) : 0;
		  if (s_giBias) fprintf(stderr, "RT GI debug arm %d ACTIVE -- the term buffer carries a CLASSIFICATION, not lighting\n", s_giBias); }
		// sensible defaults matching the M4 look, until RT_Metal_SetTuning runs
		s_cam.samples = 8; s_cam.softness = 0.12f; s_cam.darkness = 0.40f; s_cam.history = 0.90f;
		if (!s_dev)
			s_dev = MTLCreateSystemDefaultDevice();
		if (!s_dev) { fprintf(stderr, "RT_Metal: no Metal device\n"); return; }
		if (![s_dev supportsRaytracing]) { fprintf(stderr, "RT_Metal: device has no hardware ray tracing\n"); s_dev = nil; s_sharedDevice = 0; return; }
		if (!s_queue)
			s_queue = [s_dev newCommandQueue];

		if (!s_bnbuf) {
			s_bnbuf = [s_dev newBufferWithBytes:rt_bluenoise64 length:sizeof(rt_bluenoise64)
										options:MTLResourceStorageModeShared];
			// once per init, never per frame (the 92 GB lesson); smoke's liveness hook
			fprintf(stderr, "RT_Metal: blue-noise jitter table resident (%d bytes, %d slices)\n",
					(int)sizeof(rt_bluenoise64), (int)(sizeof(rt_bluenoise64) / sizeof(rt_bluenoise64[0])));
		}
		rt_compile_kernels();
		if (!s_pso) { s_dev = nil; s_queue = nil; return; }
		// Say WHOSE device this is. It is the one fact that decides whether the
		// IOSurface/CGL bridge is in play, and once Phase 5 lands it is the
		// difference between the sidecar's output being a foreign surface the
		// renderer has to import and a plain texture it can just sample.
		fprintf(stderr, "RT_Metal: device '%s' ready (unified=%d, raytracing=%d, %s)\n",
				s_dev.name.UTF8String, (int)s_dev.hasUnifiedMemory, (int)[s_dev supportsRaytracing],
				s_sharedDevice ? "SHARED with the renderer, own queue" : "own device (GL path, IOSurface bridge)");
	}
}

// Compile (or lazily RE-compile) the three kernels against the current
// blue-noise choice. On the FIRST compile a trace failure leaves s_pso nil
// and the init caller treats it as fatal, exactly the old semantics; on a
// recompile (cvar flip) any failure keeps the previous PSOs running and
// warns, so a toggle can never kill a live session. Command buffers in
// flight retain the old PSOs, so replacement needs no waits (the documented
// texture-replacement pattern).
static void rt_compile_kernels(void)
{
	@autoreleasepool {
		NSError *err = nil;
		int bn = s_bluenoiseWant ? 1 : 0;
		// the table's slice count, so the kernels cycle `frame & (T-1)` for
		// whatever shape test/bluenoise-gen.py wrote (8 or 16)
		unsigned bns = (unsigned)(sizeof(rt_bluenoise64) / sizeof(rt_bluenoise64[0]));
		// RT_METAL_NOTILE=1 compiles the kernel with the per-tile light cull
		// bypassed (identity survivor list) — an A/B verification aid: with it
		// set, output must be bit-identical to the tiled path.
		const char *notile = getenv("RT_METAL_NOTILE");
		int nt = (notile && atoi(notile)) ? 1 : 0;
		NSString *src = [NSString stringWithFormat:@"#define RT_NOTILE %d\n#define RTL %du\n#define RT_BLUENOISE %d\n#define RT_BN_SLICES %uu\n%s%s%s",
						 nt, RT_LIGHT_STRIDE, bn, bns, kConeSrc, kJitterSrc, kTraceSrc];
		id<MTLLibrary> lib = [s_dev newLibraryWithSource:src options:nil error:&err];
		id<MTLComputePipelineState> pso = lib ? [s_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"rt_trace"] error:&err] : nil;
		if (pso)
			s_pso = pso;
		else
			fprintf(stderr, "RT_Metal: kernel compile failed%s: %s\n",
					s_pso ? " (keeping previous pipeline)" : "",
					err ? err.localizedDescription.UTF8String : "unknown");
		// the god-ray kernel is optional: a compile failure only disables shafts
		{
			NSError *serr = nil;
			NSString *ssrc = [NSString stringWithFormat:@"#define RT_NOTILE %d\n#define RTL %du\n#define RT_BLUENOISE %d\n#define RT_BN_SLICES %uu\n%s%s%s",
							  nt, RT_LIGHT_STRIDE, bn, bns, kConeSrc, kJitterSrc, kShaftSrc];
			id<MTLLibrary> slib = [s_dev newLibraryWithSource:ssrc options:nil error:&serr];
			id<MTLComputePipelineState> spso = slib ? [s_dev newComputePipelineStateWithFunction:[slib newFunctionWithName:@"rt_shaft"] error:&serr] : nil;
			if (spso)
				s_shaftPso = spso;
			else if (!s_shaftPso)
				fprintf(stderr, "RT_Metal: shaft kernel compile failed (god rays disabled): %s\n",
						serr ? serr.localizedDescription.UTF8String : "unknown");
		}
		// S1: the fog filter kernel. Optional in the same way -- a compile failure
		// only disables the filter, and the fog kernel then writes s_fogmtex
		// directly as before.
		{
			NSError *xerr = nil;
			NSString *xsrc = [NSString stringWithFormat:@"%s%s", kConeSrc, kFogFilterSrc];
			id<MTLLibrary> xlib = [s_dev newLibraryWithSource:xsrc options:nil error:&xerr];
			id<MTLComputePipelineState> xpso = xlib ? [s_dev newComputePipelineStateWithFunction:[xlib newFunctionWithName:@"rt_fogfilter"] error:&xerr] : nil;
			if (xpso)
				s_fogFilterPso = xpso;
			else if (!s_fogFilterPso)
				fprintf(stderr, "RT_Metal: fog filter kernel compile failed (rt_metal_fog_filter disabled): %s\n",
						xerr ? xerr.localizedDescription.UTF8String : "unknown");
		}
		// BLUENOISE slice 2: the fog history pass. Optional the same way -- a
		// compile failure only disables rt_metal_fog_clamp, and the fog kernel
		// keeps its own EMA exactly as before.
		{
			NSError *terr = nil;
			NSString *tsrc = [NSString stringWithFormat:@"%s%s", kConeSrc, kFogTemporalSrc];
			id<MTLLibrary> tlib = [s_dev newLibraryWithSource:tsrc options:nil error:&terr];
			id<MTLComputePipelineState> tpso = tlib ? [s_dev newComputePipelineStateWithFunction:[tlib newFunctionWithName:@"rt_fogtemporal"] error:&terr] : nil;
			if (tpso)
				s_fogTemporalPso = tpso;
			else if (!s_fogTemporalPso)
				fprintf(stderr, "RT_Metal: fog temporal kernel compile failed (rt_metal_fog_clamp disabled): %s\n",
						terr ? terr.localizedDescription.UTF8String : "unknown");
		}
		// the fog kernel is optional too: a compile failure only disables in-kernel fog
		{
			NSError *ferr = nil;
			// The fog shaping constants (light shoulder knee, beam path-integral
			// reference, in-air lava glow falloff and reference) now come from
			// shader_density.h, which the GL murk march splices in too -- one
			// definition instead of named #defines here against bare literals
			// there. They were calibrated 2026-08-01 on Seb's demo6 bloom bed
			// (worst frame: 58% flat-white at his knobs before, 4.8% at DOUBLE
			// the knobs after); the density MODEL below is still a hand-kept
			// lockstep pair, see shader_density.h for exactly what is and is not
			// shared.
			// RT_BLUENOISE selects the jitter arm (the weave fix): 1 splices the
			// blue-noise helper and table read, 0 preprocesses to the classic
			// IGN source and the fog kernel signature loses the bn buffer.
			NSString *fsrc = [NSString stringWithFormat:@"#define RT_NOTILE %d\n#define RTL %du\n#define RT_BLUENOISE %d\n#define RT_BN_SLICES %uu\n#define RT_FROXEL 0\n%s%s%s%s",
							  nt, RT_LIGHT_STRIDE, bn, bns, kConeSrc, kJitterSrc, DPD_SHADER_PRELUDE, kFogSrc];
			// A1 FROXEL: the same source with RT_FROXEL 1, compiled lazily on the
			// first enable (rt_ensure_froxel_pso) -- one fog-kernel compile is
			// already the largest here, and the arm is off by default. Any
			// recompile of the fog kernel invalidates the froxel PSO with it.
			s_froxelSrc = [NSString stringWithFormat:@"#define RT_NOTILE %d\n#define RTL %du\n#define RT_BLUENOISE %d\n#define RT_BN_SLICES %uu\n#define RT_FROXEL 1\n%s%s%s%s",
							  nt, RT_LIGHT_STRIDE, bn, bns, kConeSrc, kJitterSrc, DPD_SHADER_PRELUDE, kFogSrc];
			s_froxelPso = nil; s_froxelTried = 0;
			id<MTLLibrary> flib = [s_dev newLibraryWithSource:fsrc options:nil error:&ferr];
			id<MTLComputePipelineState> fpso = flib ? [s_dev newComputePipelineStateWithFunction:[flib newFunctionWithName:@"rt_fog"] error:&ferr] : nil;
			if (fpso)
				s_fogPso = fpso;
			else if (!s_fogPso)
				fprintf(stderr, "RT_Metal: fog kernel compile failed (in-kernel fog disabled): %s\n",
						ferr ? ferr.localizedDescription.UTF8String : "unknown");
		}
		s_bluenoiseCompiled = bn;
	}
}

// rt_metal_bluenoise, pushed per frame from cl_screen (the SetFogTuning
// shape). A change lazily rebuilds the PSOs — live-togglable for A/B without
// a vid_restart.
void RT_Metal_SetBlueNoise(int enable)
{
	s_bluenoiseWant = enable ? 1 : 0;
	if (s_dev && s_bluenoiseCompiled >= 0 && s_bluenoiseCompiled != s_bluenoiseWant)
		rt_compile_kernels();
}

// the historical entry point: make our own device and queue, exactly as before
void RT_Metal_Init(void)
{
	RT_Metal_InitWithDevice(NULL, NULL);
}

void RT_Metal_SetWorld(const float *verts3f, int numverts, const int *tris3i, int numtris, const unsigned char *albedo4, unsigned long token)
{
	if (!s_dev || !s_queue) return;
	if (numverts < 3 || numtris < 1 || !verts3f || !tris3i) return;
	if (s_accel && token == s_worldToken) return;   // already built for this world

	@autoreleasepool {
		rt_wait_pending();   // an in-flight async trace may still read the old BLAS
		s_vbuf = nil; s_ibuf = nil; s_accel = nil; s_wabuf = nil;   // drop any previous world
		// the lava and sky BLASes index into s_vbuf -- drop them with the world and
		// force rebuilds on the SetLavaSurfaces/SetSkySurfaces calls that follow
		// this one every frame
		s_lavaAccel = nil; s_libuf = nil; s_lavaToken = (unsigned long)-1;
		s_skyAccel = nil; s_skyibuf = nil; s_skyToken = (unsigned long)-1;
		s_liqAccel = nil; s_liqibuf = nil; s_liqToken = (unsigned long)-1;
		s_bliqAccel = nil; s_bliqibuf = nil; s_bliqToken = (unsigned long)-1;

		s_vbuf = [s_dev newBufferWithBytes:verts3f length:(NSUInteger)numverts * 3 * sizeof(float) options:MTLResourceStorageModeShared];
		s_ibuf = [s_dev newBufferWithBytes:tris3i  length:(NSUInteger)numtris  * 3 * sizeof(uint32_t) options:MTLResourceStorageModeShared];
		// the coloured-bounce albedo (G4-2): per-triangle RGBA8, always created
		// so the kernel's buffer(9) is never unbound (the declared-but-unbound
		// validation abort this tree has been burned by); grey stand-in when
		// the caller has none
		if (albedo4)
			s_wabuf = [s_dev newBufferWithBytes:albedo4 length:(NSUInteger)numtris * 4 options:MTLResourceStorageModeShared];
		if (!s_wabuf)
		{
			static const unsigned char greytri[4] = { 128, 128, 128, 255 };
			s_wabuf = [s_dev newBufferWithBytes:greytri length:4 options:MTLResourceStorageModeShared];
		}

		MTLAccelerationStructureTriangleGeometryDescriptor *tri = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
		tri.vertexBuffer = s_vbuf; tri.vertexStride = sizeof(float) * 3; tri.vertexFormat = MTLAttributeFormatFloat3;
		tri.indexBuffer = s_ibuf;  tri.indexType = MTLIndexTypeUInt32;   tri.triangleCount = (NSUInteger)numtris;
		MTLPrimitiveAccelerationStructureDescriptor *pd = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
		pd.geometryDescriptors = @[tri];

		MTLAccelerationStructureSizes sz = [s_dev accelerationStructureSizesWithDescriptor:pd];
		s_accel = [s_dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
		id<MTLBuffer> scratch = [s_dev newBufferWithLength:sz.buildScratchBufferSize options:MTLResourceStorageModePrivate];
		id<MTLCommandBuffer> bcb = [s_queue commandBuffer];
		id<MTLAccelerationStructureCommandEncoder> ae = [bcb accelerationStructureCommandEncoder];
		[ae buildAccelerationStructure:s_accel descriptor:pd scratchBuffer:scratch scratchBufferOffset:0];
		[ae endEncoding];
		[bcb commit];
		[bcb waitUntilCompleted];
		if (bcb.error) { fprintf(stderr, "RT_Metal: AS build failed: %s\n", bcb.error.localizedDescription.UTF8String); s_accel = nil; return; }

		s_worldToken = token;
		s_hasPrev = 0;   // new world: previous-frame history is from the old map, discard it (avoids a 1-frame changelevel shadow ghost)
		s_shHasPrev = 0; // same for the shaft and fog temporal blends
		s_fogHasPrev = 0;
		// the baked fog field describes the OLD world; drop it. The fog kernel falls
		// back to the GL murk until the new map's bake re-calls SetFogField
		free(s_fieldCopy); s_fieldCopy = NULL;
		s_fieldSize[0] = s_fieldSize[1] = s_fieldSize[2] = 0;
		s_fieldTex3D = nil;
		s_fieldDirty = 0;
		// and the irradiance grid with it -- an old map's light must not tint a new world
		free(s_irrCopy); s_irrCopy = NULL;
		s_irrSize[0] = s_irrSize[1] = s_irrSize[2] = 0;
		s_irrTex3D = nil;
		s_irrDirty = 0;
		fprintf(stderr, "RT_Metal: built acceleration structure (%d tris, %d verts, %.1f MB)\n",
				numtris, numverts, sz.accelerationStructureSize / 1048576.0);
	}
}

void RT_Metal_SetCamera(const float origin[3], const float forward[3], const float right[3], const float up[3], float tanx, float tany)
{
	memcpy(s_cam.origin,  origin,  sizeof(float) * 3);
	memcpy(s_cam.forward, forward, sizeof(float) * 3);
	memcpy(s_cam.right,   right,   sizeof(float) * 3);
	memcpy(s_cam.up,      up,      sizeof(float) * 3);
	s_cam.tanx = tanx;
	s_cam.tany = tany;
	s_hasCam = 1;
}

void RT_Metal_SetLights(const float *data, int numlights, int numdynamic)
{
	if (!s_dev) return;
	if (numlights < 0) numlights = 0;
	if (numdynamic < 0) numdynamic = 0; else if (numdynamic > numlights) numdynamic = numlights;
	@autoreleasepool {
		// grow-only per-slot reuse (see the declaration comment for why a fresh
		// slot, not a fresh buffer, is what makes the copy race-free)
		int p = s_par;
		NSUInteger need = (NSUInteger)(numlights > 0 ? numlights : 1) * RT_LIGHT_STRIDE * sizeof(float);
		if (!s_lightbuf[p] || s_lightbufCap[p] < need) {
			s_lightbuf[p] = [s_dev newBufferWithLength:need options:MTLResourceStorageModeShared];
			s_lightbufCap[p] = need;
		}
		if (numlights > 0 && data)
			memcpy(s_lightbuf[p].contents, data, (size_t)numlights * RT_LIGHT_STRIDE * sizeof(float));
		// Diagnostic only under RT_METAL_VERBOSE=1: this fires on EVERY
		// view-culled count change, which is ~19 lines/s under ordinary
		// movement -- the line that ran Xcode's console to 92 GB (see
		// s_verbose's declaration comment).
		static int lastreport = -1;
		if (s_verbose && numlights != lastreport) { fprintf(stderr, "RT_Metal: %d real lights (view-culled)\n", numlights); lastreport = numlights; }
		s_numlights = numlights;
		s_numdynamic = numdynamic;   // lights[0..numdynamic) are dynamic (prepended by the caller)
	}
}

// A degenerate zero-area triangle stands in for an empty stream so every BLAS
// and the 4-instance TLAS remain valid each frame (it can never be hit).
static const float    s_degverts[9] = {0,0,0, 0,0,0, 0,0,0};
static const uint32_t s_degtris[3]  = {0u, 1u, 2u};

void RT_Metal_SetLavaSurfaces(const int *tris3i, int numtris, unsigned long token)
{
	if (!s_dev || !s_queue || !s_vbuf) return;   // needs the world verts SetWorld uploaded
	if (numtris < 0 || !tris3i) numtris = 0;
	if (s_lavaAccel && token == s_lavaToken) return;   // already built for this list

	@autoreleasepool {
		rt_wait_pending();   // an in-flight async trace may still read the old BLAS
		s_lavaAccel = nil; s_libuf = nil;

		MTLAccelerationStructureTriangleGeometryDescriptor *tri = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
		if (numtris < 1) {
			// degenerate stand-in with its OWN zero verts: indices 0,1,2 into the
			// world vertex buffer would be a real, hittable triangle
			if (!s_lavadegv)
				s_lavadegv = [s_dev newBufferWithBytes:s_degverts length:sizeof(s_degverts) options:MTLResourceStorageModeShared];
			s_libuf = [s_dev newBufferWithBytes:s_degtris length:sizeof(s_degtris) options:MTLResourceStorageModeShared];
			tri.vertexBuffer = s_lavadegv; tri.vertexStride = sizeof(float) * 3; tri.vertexFormat = MTLAttributeFormatFloat3;
			tri.indexBuffer = s_libuf;     tri.indexType = MTLIndexTypeUInt32;   tri.triangleCount = 1;
		} else {
			s_libuf = [s_dev newBufferWithBytes:tris3i length:(NSUInteger)numtris * 3 * sizeof(uint32_t) options:MTLResourceStorageModeShared];
			tri.vertexBuffer = s_vbuf;  tri.vertexStride = sizeof(float) * 3; tri.vertexFormat = MTLAttributeFormatFloat3;
			tri.indexBuffer = s_libuf;  tri.indexType = MTLIndexTypeUInt32;   tri.triangleCount = (NSUInteger)numtris;
		}
		MTLPrimitiveAccelerationStructureDescriptor *pd = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
		pd.geometryDescriptors = @[tri];
		MTLAccelerationStructureSizes sz = [s_dev accelerationStructureSizesWithDescriptor:pd];
		s_lavaAccel = [s_dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
		id<MTLBuffer> scratch = [s_dev newBufferWithLength:sz.buildScratchBufferSize options:MTLResourceStorageModePrivate];
		id<MTLCommandBuffer> bcb = [s_queue commandBuffer];
		id<MTLAccelerationStructureCommandEncoder> ae = [bcb accelerationStructureCommandEncoder];
		[ae buildAccelerationStructure:s_lavaAccel descriptor:pd scratchBuffer:scratch scratchBufferOffset:0];
		[ae endEncoding];
		[bcb commit];
		[bcb waitUntilCompleted];
		if (bcb.error) { fprintf(stderr, "RT_Metal: lava AS build failed: %s\n", bcb.error.localizedDescription.UTF8String); s_lavaAccel = nil; return; }
		s_lavaToken = token;
		if (numtris > 0)
			fprintf(stderr, "RT_Metal: built lava acceleration structure (%d tris)\n", numtris);
	}
}

// The lava function's twin for the OPEN SKY instance (rt_metal_skyopen); the
// two differ only in which statics they fill and in sharing s_lavadegv.
void RT_Metal_SetSkySurfaces(const int *tris3i, int numtris, unsigned long token)
{
	if (!s_dev || !s_queue || !s_vbuf) return;   // needs the world verts SetWorld uploaded
	if (numtris < 0 || !tris3i) numtris = 0;
	if (s_skyAccel && token == s_skyToken) return;   // already built for this list

	@autoreleasepool {
		rt_wait_pending();   // an in-flight async trace may still read the old BLAS
		s_skyAccel = nil; s_skyibuf = nil;

		MTLAccelerationStructureTriangleGeometryDescriptor *tri = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
		if (numtris < 1) {
			// degenerate stand-in with its OWN zero verts (shared with lava's):
			// indices 0,1,2 into the world vertex buffer would be a real triangle
			if (!s_lavadegv)
				s_lavadegv = [s_dev newBufferWithBytes:s_degverts length:sizeof(s_degverts) options:MTLResourceStorageModeShared];
			s_skyibuf = [s_dev newBufferWithBytes:s_degtris length:sizeof(s_degtris) options:MTLResourceStorageModeShared];
			tri.vertexBuffer = s_lavadegv; tri.vertexStride = sizeof(float) * 3; tri.vertexFormat = MTLAttributeFormatFloat3;
			tri.indexBuffer = s_skyibuf;   tri.indexType = MTLIndexTypeUInt32;   tri.triangleCount = 1;
		} else {
			s_skyibuf = [s_dev newBufferWithBytes:tris3i length:(NSUInteger)numtris * 3 * sizeof(uint32_t) options:MTLResourceStorageModeShared];
			tri.vertexBuffer = s_vbuf;    tri.vertexStride = sizeof(float) * 3; tri.vertexFormat = MTLAttributeFormatFloat3;
			tri.indexBuffer = s_skyibuf;  tri.indexType = MTLIndexTypeUInt32;   tri.triangleCount = (NSUInteger)numtris;
		}
		MTLPrimitiveAccelerationStructureDescriptor *pd = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
		pd.geometryDescriptors = @[tri];
		MTLAccelerationStructureSizes sz = [s_dev accelerationStructureSizesWithDescriptor:pd];
		s_skyAccel = [s_dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
		id<MTLBuffer> scratch = [s_dev newBufferWithLength:sz.buildScratchBufferSize options:MTLResourceStorageModePrivate];
		id<MTLCommandBuffer> bcb = [s_queue commandBuffer];
		id<MTLAccelerationStructureCommandEncoder> ae = [bcb accelerationStructureCommandEncoder];
		[ae buildAccelerationStructure:s_skyAccel descriptor:pd scratchBuffer:scratch scratchBufferOffset:0];
		[ae endEncoding];
		[bcb commit];
		[bcb waitUntilCompleted];
		if (bcb.error) { fprintf(stderr, "RT_Metal: sky AS build failed: %s\n", bcb.error.localizedDescription.UTF8String); s_skyAccel = nil; return; }
		s_skyToken = token;
		if (numtris > 0)
			fprintf(stderr, "RT_Metal: built sky acceleration structure (%d tris)\n", numtris);
	}
}

// And the third of the family: the EMISSIVE liquid instance
// (rt_metal_liquidemissive). Identical build to lava/sky; the kernel treats a
// hit as emissive (term 1.0) and never fetches these triangles, so nothing
// here rides the trace encoder.
void RT_Metal_SetLiquidSurfaces(const int *tris3i, int numtris, unsigned long token)
{
	if (!s_dev || !s_queue || !s_vbuf) return;   // needs the world verts SetWorld uploaded
	if (numtris < 0 || !tris3i) numtris = 0;
	if (s_liqAccel && token == s_liqToken) return;   // already built for this list

	@autoreleasepool {
		rt_wait_pending();   // an in-flight async trace may still read the old BLAS
		s_liqAccel = nil; s_liqibuf = nil;

		MTLAccelerationStructureTriangleGeometryDescriptor *tri = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
		if (numtris < 1) {
			// degenerate stand-in with its OWN zero verts (shared with lava's):
			// indices 0,1,2 into the world vertex buffer would be a real triangle
			if (!s_lavadegv)
				s_lavadegv = [s_dev newBufferWithBytes:s_degverts length:sizeof(s_degverts) options:MTLResourceStorageModeShared];
			s_liqibuf = [s_dev newBufferWithBytes:s_degtris length:sizeof(s_degtris) options:MTLResourceStorageModeShared];
			tri.vertexBuffer = s_lavadegv; tri.vertexStride = sizeof(float) * 3; tri.vertexFormat = MTLAttributeFormatFloat3;
			tri.indexBuffer = s_liqibuf;   tri.indexType = MTLIndexTypeUInt32;   tri.triangleCount = 1;
		} else {
			s_liqibuf = [s_dev newBufferWithBytes:tris3i length:(NSUInteger)numtris * 3 * sizeof(uint32_t) options:MTLResourceStorageModeShared];
			tri.vertexBuffer = s_vbuf;    tri.vertexStride = sizeof(float) * 3; tri.vertexFormat = MTLAttributeFormatFloat3;
			tri.indexBuffer = s_liqibuf;  tri.indexType = MTLIndexTypeUInt32;   tri.triangleCount = (NSUInteger)numtris;
		}
		MTLPrimitiveAccelerationStructureDescriptor *pd = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
		pd.geometryDescriptors = @[tri];
		MTLAccelerationStructureSizes sz = [s_dev accelerationStructureSizesWithDescriptor:pd];
		s_liqAccel = [s_dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
		id<MTLBuffer> scratch = [s_dev newBufferWithLength:sz.buildScratchBufferSize options:MTLResourceStorageModePrivate];
		id<MTLCommandBuffer> bcb = [s_queue commandBuffer];
		id<MTLAccelerationStructureCommandEncoder> ae = [bcb accelerationStructureCommandEncoder];
		[ae buildAccelerationStructure:s_liqAccel descriptor:pd scratchBuffer:scratch scratchBufferOffset:0];
		[ae endEncoding];
		[bcb commit];
		[bcb waitUntilCompleted];
		if (bcb.error) { fprintf(stderr, "RT_Metal: liquid AS build failed: %s\n", bcb.error.localizedDescription.UTF8String); s_liqAccel = nil; return; }
		s_liqToken = token;
		if (numtris > 0)
			fprintf(stderr, "RT_Metal: built liquid acceleration structure (%d tris)\n", numtris);
	}
}

// SEPTEMBER2 C2 (2026-09-09): the BLENDED liquids' own structure -- instance 6, mask 0x40.
// The opaque entry point above, with its own statics and token; the kernel fetches
// these triangles for the liquid normal, so the index buffer rides the trace at
// buffer(10) (always bound, degenerate included -- the declared-but-unbound rule).
void RT_Metal_SetBlendedLiquidSurfaces(const int *tris3i, int numtris, unsigned long token)
{
	if (!s_dev || !s_queue || !s_vbuf) return;   // needs the world verts SetWorld uploaded
	if (numtris < 0 || !tris3i) numtris = 0;
	if (s_bliqAccel && token == s_bliqToken) return;   // already built for this list

	@autoreleasepool {
		rt_wait_pending();   // an in-flight async trace may still read the old BLAS
		s_bliqAccel = nil; s_bliqibuf = nil;

		MTLAccelerationStructureTriangleGeometryDescriptor *tri = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
		if (numtris < 1) {
			// degenerate stand-in with its OWN zero verts (shared with lava's):
			// indices 0,1,2 into the world vertex buffer would be a real triangle
			if (!s_lavadegv)
				s_lavadegv = [s_dev newBufferWithBytes:s_degverts length:sizeof(s_degverts) options:MTLResourceStorageModeShared];
			s_bliqibuf = [s_dev newBufferWithBytes:s_degtris length:sizeof(s_degtris) options:MTLResourceStorageModeShared];
			tri.vertexBuffer = s_lavadegv; tri.vertexStride = sizeof(float) * 3; tri.vertexFormat = MTLAttributeFormatFloat3;
			tri.indexBuffer = s_bliqibuf;   tri.indexType = MTLIndexTypeUInt32;   tri.triangleCount = 1;
		} else {
			s_bliqibuf = [s_dev newBufferWithBytes:tris3i length:(NSUInteger)numtris * 3 * sizeof(uint32_t) options:MTLResourceStorageModeShared];
			tri.vertexBuffer = s_vbuf;    tri.vertexStride = sizeof(float) * 3; tri.vertexFormat = MTLAttributeFormatFloat3;
			tri.indexBuffer = s_bliqibuf;  tri.indexType = MTLIndexTypeUInt32;   tri.triangleCount = (NSUInteger)numtris;
		}
		MTLPrimitiveAccelerationStructureDescriptor *pd = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
		pd.geometryDescriptors = @[tri];
		MTLAccelerationStructureSizes sz = [s_dev accelerationStructureSizesWithDescriptor:pd];
		s_bliqAccel = [s_dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
		id<MTLBuffer> scratch = [s_dev newBufferWithLength:sz.buildScratchBufferSize options:MTLResourceStorageModePrivate];
		id<MTLCommandBuffer> bcb = [s_queue commandBuffer];
		id<MTLAccelerationStructureCommandEncoder> ae = [bcb accelerationStructureCommandEncoder];
		[ae buildAccelerationStructure:s_bliqAccel descriptor:pd scratchBuffer:scratch scratchBufferOffset:0];
		[ae endEncoding];
		[bcb commit];
		[bcb waitUntilCompleted];
		if (bcb.error) { fprintf(stderr, "RT_Metal: blended liquid AS build failed: %s\n", bcb.error.localizedDescription.UTF8String); s_bliqAccel = nil; return; }
		s_bliqToken = token;
		if (numtris > 0)
			fprintf(stderr, "RT_Metal: built BLENDED liquid acceleration structure (C2) (%d tris)\n", numtris);
	}
}

void RT_Metal_SetEntities(const float *verts3f, int numverts, const int *tris3i, int numtris, const float *norms3f)
{
	if (!s_dev) return;
	if (numtris < 0) numtris = 0;
	if (numverts < 0) numverts = 0;
	@autoreleasepool {
		const void *vsrc; const void *isrc; const void *nsrc; NSUInteger nv, nt;
		if (numtris < 1 || numverts < 3 || !verts3f || !tris3i) {
			vsrc = s_degverts; isrc = s_degtris; nsrc = NULL; nv = 3; nt = 1;
		} else {
			vsrc = verts3f; isrc = tris3i; nsrc = norms3f; nv = (NSUInteger)numverts; nt = (NSUInteger)numtris;
		}
		NSUInteger vbytes = nv * 3 * sizeof(float);
		NSUInteger ibytes = nt * 3 * sizeof(uint32_t);
		int p = s_par;   // write the slot the NEXT composite will encode; the in-flight frame reads the other
		if (!s_evbuf[p] || vbytes > s_evbufCap[p]) { s_evbuf[p] = [s_dev newBufferWithLength:vbytes options:MTLResourceStorageModeShared]; s_evbufCap[p] = vbytes; }
		if (!s_eibuf[p] || ibytes > s_eibufCap[p]) { s_eibuf[p] = [s_dev newBufferWithLength:ibytes options:MTLResourceStorageModeShared]; s_eibufCap[p] = ibytes; }
		memcpy(s_evbuf[p].contents, vsrc, vbytes);
		memcpy(s_eibuf[p].contents, isrc, ibytes);   // int indices and uint32 share bit layout for non-negative values
		s_evHash[p] = s_asSkip ? rt_hash64(vsrc, vbytes, nt) : 0;   // A3: the slot's content identity (indices ride the triangle count: the gather emits them in order)
		// vertex normals ride a parallel buffer (kernel buffer index 7). Without
		// them the slot still gets a bindable degenerate buffer and the flag stays
		// 0, so the kernel shades with the geometric face normal exactly as before.
		if (nsrc) {
			if (!s_enbuf[p] || vbytes > s_enbufCap[p]) { s_enbuf[p] = [s_dev newBufferWithLength:vbytes options:MTLResourceStorageModeShared]; s_enbufCap[p] = vbytes; }
			memcpy(s_enbuf[p].contents, nsrc, vbytes);
			s_entHasNorms[p] = 1;
		} else {
			if (!s_enbuf[p]) { s_enbuf[p] = [s_dev newBufferWithLength:sizeof(s_degverts) options:MTLResourceStorageModeShared]; s_enbufCap[p] = sizeof(s_degverts); memcpy(s_enbuf[p].contents, s_degverts, sizeof(s_degverts)); }
			s_entHasNorms[p] = 0;
		}
		s_numentitytris  = (int)nt;
	}
}

void RT_Metal_SetLightCores(const float *verts3f, int numverts, const int *tris3i, int numtris)
{
	if (!s_dev) return;
	if (numtris < 0) numtris = 0;
	if (numverts < 0) numverts = 0;
	@autoreleasepool {
		const void *vsrc; const void *isrc; NSUInteger nv, nt;
		static int announced;
		if (numtris < 1 || numverts < 3 || !verts3f || !tris3i) {
			vsrc = s_degverts; isrc = s_degtris; nv = 3; nt = 1;
		} else {
			vsrc = verts3f; isrc = tris3i; nv = (NSUInteger)numverts; nt = (NSUInteger)numtris;
			if (!announced) { fprintf(stderr, "RT_Metal: light-core stream active (%d tris)\n", numtris); announced = 1; }
		}
		NSUInteger vbytes = nv * 3 * sizeof(float);
		NSUInteger ibytes = nt * 3 * sizeof(uint32_t);
		int p = s_par;   // write the slot the NEXT composite will encode (see SetEntities)
		if (!s_lcvbuf[p] || vbytes > s_lcvbufCap[p]) { s_lcvbuf[p] = [s_dev newBufferWithLength:vbytes options:MTLResourceStorageModeShared]; s_lcvbufCap[p] = vbytes; }
		if (!s_lcibuf[p] || ibytes > s_lcibufCap[p]) { s_lcibuf[p] = [s_dev newBufferWithLength:ibytes options:MTLResourceStorageModeShared]; s_lcibufCap[p] = ibytes; }
		memcpy(s_lcvbuf[p].contents, vsrc, vbytes);
		memcpy(s_lcibuf[p].contents, isrc, ibytes);
		s_lcHash[p] = s_asSkip ? rt_hash64(vsrc, vbytes, nt) : 0;
		s_numlctris = (int)nt;
	}
}

// --- god-ray (shaft) state ------------------------------------------------
static int   s_shaftEnable;      // master, from rt_metal_shafts
static int   s_shaftSamples = 6; // K points along each view ray
static float s_shaftScale = 0.5f;    // buffer resolution vs RT viewport
static float s_shaftHistory = 0.5f;  // temporal blend weight
static float s_shaftDist = 2000.0f;  // max shaft distance (and sky ray length)
static float s_shaftResidual;        // unshadowed non-dominant weight

void RT_Metal_SetShaftTuning(int enable, int samples, float scale, float history, float dist, float residual)
{
	if (samples < 1) samples = 1; else if (samples > 16) samples = 16;
	if (scale < 0.125f) scale = 0.125f; else if (scale > 1.0f) scale = 1.0f;
	if (history < 0.0f) history = 0.0f; else if (history > 0.9f) history = 0.9f;
	if (dist < 256.0f) dist = 256.0f; else if (dist > 16384.0f) dist = 16384.0f;
	if (residual < 0.0f) residual = 0.0f; else if (residual > 1.0f) residual = 1.0f;
	s_shaftEnable   = enable ? 1 : 0;
	s_shaftSamples  = samples;
	s_shaftScale    = scale;
	s_shaftHistory  = history;
	s_shaftDist     = dist;
	s_shaftResidual = residual;
}

void RT_Metal_SetFogNoise(const unsigned char *rgba, int size)
{
	size_t bytes;
	if (!rgba || size < 2)
		return;
	bytes = (size_t)size * size * size * 4;
	free(s_noiseCopy);
	s_noiseCopy = (unsigned char *)malloc(bytes);
	if (!s_noiseCopy) { s_noiseSize = 0; return; }
	memcpy(s_noiseCopy, rgba, bytes);
	s_noiseSize = size;
	s_noiseDirty = 1;
}

void RT_Metal_SetFogField(const unsigned char *rgba, const int size[3], const float origin[3], const float invsize[3], float sdfrange, float maxh)
{
	size_t bytes;
	if (!rgba || !size || size[0] < 2 || size[1] < 2 || size[2] < 2)
		return;
	bytes = (size_t)size[0] * size[1] * size[2] * 4;
	free(s_fieldCopy);
	s_fieldCopy = (unsigned char *)malloc(bytes);
	if (!s_fieldCopy) { s_fieldSize[0] = s_fieldSize[1] = s_fieldSize[2] = 0; return; }
	memcpy(s_fieldCopy, rgba, bytes);
	memcpy(s_fieldSize, size, sizeof(int) * 3);
	// origin/invsize/sdfrange/maxh are NOT stored here: the kernel takes the
	// field parameters from the per-frame fog shade (R_Volumetric_GetFogKernelParams)
	(void)origin; (void)invsize; (void)sdfrange; (void)maxh;
	s_fieldDirty = 1;
}

void RT_Metal_SetFogIrradiance(const unsigned char *rgba, const int size[3])
{
	size_t bytes;
	if (!rgba || !size || size[0] < 2 || size[1] < 2 || size[2] < 2)
		return;
	bytes = (size_t)size[0] * size[1] * size[2] * 4;
	free(s_irrCopy);
	s_irrCopy = (unsigned char *)malloc(bytes);
	if (!s_irrCopy) { s_irrSize[0] = s_irrSize[1] = s_irrSize[2] = 0; return; }
	memcpy(s_irrCopy, rgba, bytes);
	memcpy(s_irrSize, size, sizeof(int) * 3);
	// origin/invsize/gain/floor/strength ride the per-frame fog shade, exactly
	// as the field's parameters do
	s_irrDirty = 1;
}

// Has the sidecar got an irradiance grid? The world build drops it on purpose
// (an old map's light must not tint a new world), and the ONLY producer used to
// be the murk's own bake inside R_RenderView, which always ran after the world
// reached the sidecar. cl_particles_lighting 2 broke that ordering by baking the
// same grid from R_DrawParticles: on a map's first frame the bake -- and its
// hand-over -- can land BEFORE the acceleration structure is built, the build
// then drops the copy, and R_Volumetric_GetIrradianceGrid's cache never bakes
// again, so the fog kernel runs the whole map with NO ambient term and nothing
// says so. The murk re-hands the grid whenever this returns 0.
int RT_Metal_HasFogIrradiance(void)
{
	return (s_irrCopy != NULL || s_irrTex3D != nil) ? 1 : 0;
}

void RT_Metal_SetFogShade(const rt_fog_shade_t *shade)
{
	if (!shade)
		return;
	s_fogShade = *shade;
	s_hasFogShade = 1;
}

void RT_Metal_SetFogTuning(int enable, int steps, float scale, float history, float intensity, int stride, float residual, float beams)
{
	if (steps < 2) steps = 2; else if (steps > 64) steps = 64;
	if (scale < 0.125f) scale = 0.125f; else if (scale > 1.0f) scale = 1.0f;
	// 0.95 is the clamp's ceiling (BLUENOISE slice 2); the encode caps it at
	// 0.9 again whenever rt_metal_fog_clamp is off, so the off arm sees the
	// value this line always produced
	if (history < 0.0f) history = 0.0f; else if (history > 0.95f) history = 0.95f;
	if (intensity < 0.0f) intensity = 0.0f; else if (intensity > 4.0f) intensity = 4.0f;
	if (stride < 1) stride = 1; else if (stride > 8) stride = 8;
	if (residual < 0.0f) residual = 0.0f; else if (residual > 1.0f) residual = 1.0f;
	if (beams < 0.0f) beams = 0.0f; else if (beams > 4.0f) beams = 4.0f;
	s_fogEnable    = enable ? 1 : 0;
	s_fogSteps     = steps;
	s_fogScale     = scale;
	s_fogHistory   = history;
	s_fogIntensity = intensity;
	s_fogStride    = stride;
	s_fogResidual  = residual;
	s_fogBeams     = beams;
}

int RT_Metal_GetFogTexture(unsigned int *gltex, int *w, int *h)
{
	// shown-slot discipline as the shafts: waited-on, stable until the next encode
	if (s_lastShown < 0 || !s_fogValid[s_lastShown])
		return 0;
	// Two names, one meaning, chosen by path -- the shape RT_Metal_GetTermTexture
	// already uses (METAL.md 6-3). The shared arm keys on the MTLTexture, never
	// the IOSurface or the GL name: both are NULL by construction there, and
	// keying a validity test on them is the 5-1 defect class exactly.
	if (s_sharedDevice)
	{
		int h2 = rt_adopt_slot((__bridge void *)s_fogmtex[s_lastShown],
		                       &s_fogHandle[s_lastShown], &s_fogHandleTexPtr[s_lastShown],
		                       s_fogw, s_fogh);
		if (!h2)
			return 0;
		*gltex = (unsigned int)h2;
		*w = s_fogw;
		*h = s_fogh;
		return 1;
	}
	if (!s_foggltex[s_lastShown])
		return 0;
	*gltex = (unsigned int)s_foggltex[s_lastShown];
	*w = s_fogw;
	*h = s_fogh;
	return 1;
}

int RT_Metal_GetTermTexture(unsigned int *gltex, int *w, int *h)
{
	// Same shown-slot discipline as the shafts below, but the main trace is encoded
	// unconditionally, so s_lastShown plus a live name is the whole validity test.
	// The shown slot's command buffer was waited on before the composite drew, and
	// nothing re-encodes that slot until the NEXT frame's composite.
	//
	// Two names, one meaning, chosen by path: the shared-device arm hands back the
	// renderer texture handle the composite adopted, where GL hands back its
	// rectangle name. Keeping one accessor rather than adding a parallel one is
	// deliberate -- every consumer wants the same thing ("the term I just showed"),
	// and a second entry point is a second thing to forget to update.
	if (s_lastShown < 0)
		return 0;
	if (s_sharedDevice)
	{
		if (!s_termHandle[s_lastShown])
			return 0;
		*gltex = (unsigned int)s_termHandle[s_lastShown];
		*w = s_w;
		*h = s_h;
		return 1;
	}
	if (!s_gltex[s_lastShown])
		return 0;
	*gltex = (unsigned int)s_gltex[s_lastShown];
	*w = s_w;
	*h = s_h;
	return 1;
}

// SEPTEMBER2 C2: the liquid pair the SHOWN trace wrote -- the term accessor's discipline
// exactly, on the pair's own handles. Returns 0 (and the consumer takes the old text)
// whenever the arm is off or the pair has not been traced yet; the width reported is
// the FULL double width, the consumer halves it.
int RT_Metal_GetLiquidTexture(unsigned int *gltex, int *w, int *h)
{
	if (s_lastShown < 0 || !s_liqrt || !s_liqmtex[0])
		return 0;
	if (s_sharedDevice)
	{
		int h2 = rt_adopt_slot((__bridge void *)s_liqmtex[s_lastShown],
		                       &s_liqHandle[s_lastShown], &s_liqHandleTexPtr[s_lastShown],
		                       2 * s_w, s_h);
		if (!h2)
			return 0;
		*gltex = (unsigned int)h2;
		*w = 2 * s_w;
		*h = s_h;
		return 1;
	}
	if (!s_liqgltex[s_lastShown])
		return 0;
	*gltex = (unsigned int)s_liqgltex[s_lastShown];
	*w = 2 * s_w;
	*h = s_h;
	return 1;
}

// METAL.md Phase 5 slice 2: read the SHOWN term buffer back and report a hash of
// it. This is the phase's strongest instrument, and it is strong for one reason:
// the two backends run the SAME kernel source on the SAME MTLDevice with the SAME
// uploaded inputs, so the only thing that differs is how the output texture was
// allocated (plain vs IOSurface-backed). A byte-identical term buffer therefore
// proves the entire compute half ported correctly BEFORE a single pixel is
// composited -- which is what makes a wrong pixel in slice 5-3 have exactly one
// suspect (the draw) instead of five.
//
// getBytes works identically on both paths because both allocations are
// MTLStorageModeShared today (see rt_pair_ensure). Slice 5-6 moves the shared
// path to Private and must revisit this with a blit.
//
// Determinism, measured in slice 5-1 and required for the hash to mean anything:
// with rt_metal_history 0 the kernels never read cam.frame, so a frozen bed is
// already reproducible; with history > 0, RT_METAL_FRAMEPIN=1 is what makes it so.
void RT_Metal_TermProbe(void)
{
	@autoreleasepool {
		if (!s_dev) { fprintf(stderr, "rt_metal_termprobe: no Metal device\n"); return; }
		if (s_lastShown < 0 || !s_mtex[s_lastShown]) {
			fprintf(stderr, "rt_metal_termprobe: no shown term buffer (lastShown %d) -- rt_metal 1 and a rendered frame are needed\n", s_lastShown);
			return;
		}
		id<MTLTexture> tex = s_mtex[s_lastShown];
		NSUInteger w = tex.width, h = tex.height;
		NSUInteger rowbytes = w * 8;             // RGBA16Float
		size_t total = (size_t)rowbytes * h;
		unsigned char *bytes = (unsigned char *)malloc(total);
		if (!bytes) { fprintf(stderr, "rt_metal_termprobe: out of memory (%zu bytes)\n", total); return; }
		[tex getBytes:bytes bytesPerRow:rowbytes fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];

		// FNV-1a 64, the house hash (Collision_ParityHash / the cmdtrace digest
		// use the same shape). Also count zero texels: an all-zero buffer hashes
		// perfectly consistently and would otherwise be a very convincing pass.
		uint64_t hash = 1469598103934665603ULL;
		size_t zeros = 0;
		for (size_t i = 0; i < total; i++) { hash ^= bytes[i]; hash *= 1099511628211ULL; }
		for (size_t i = 0; i < total; i += 8) {
			int nz = 0;
			for (int b = 0; b < 8; b++) if (bytes[i + b]) { nz = 1; break; }
			if (!nz) zeros++;
		}
		free(bytes);
		fprintf(stderr, "rt_metal_termprobe: slot %d %ux%u %s hash %016llx zerotexels %zu of %zu\n",
		           s_lastShown, (unsigned)w, (unsigned)h,
		           s_sharedDevice ? "shared" : "iosurface",
		           (unsigned long long)hash, zeros, (size_t)(w * h));
	}
}

int RT_Metal_GetShaftsTexture(unsigned int *gltex, int *w, int *h)
{
	// only ever hand out the slot the last composite SHOWED: that slot's command
	// buffer (which contains the shaft encoder) has been waited on, so the GPU is
	// done with it, and it stays untouched until the next composite re-encodes it
	if (s_lastShown < 0 || !s_shValid[s_lastShown])
		return 0;
	// 6-3, same two-names-one-meaning shape as the fog accessor above.
	if (s_sharedDevice)
	{
		int h2 = rt_adopt_slot((__bridge void *)s_shmtex[s_lastShown],
		                       &s_shHandle[s_lastShown], &s_shHandleTexPtr[s_lastShown],
		                       s_shw, s_shh);
		if (!h2)
			return 0;
		*gltex = (unsigned int)h2;
		*w = s_shw;
		*h = s_shh;
		return 1;
	}
	if (!s_shgltex[s_lastShown])
		return 0;
	*gltex = (unsigned int)s_shgltex[s_lastShown];
	*w = s_shw;
	*h = s_shh;
	return 1;
}

// Console-only A/B switch for the translation-aware half of the reprojection.
// 0 leaves the rotation-only remap exactly as it was.
// BEAUTY B2 (2026-09-16): the fog kernel multiplied every lit term by
// (1 - inliquid) at four LOCKSTEP sites -- "suppressed in liquid (authored
// look)". This lets a fraction through, so the shafts and the in-scattered light
// the march already computes appear under water. Change-only line, the
// console-ink rule; 0 is the old text's arithmetic exactly (x * (1 - inliquid)).
static float s_fogLiquidLight;
void RT_Metal_SetFogLiquidLight(float amount)
{
	float a = (amount < 0.0f) ? 0.0f : (amount > 1.0f ? 1.0f : amount);
	if ((a > 0.0f) != (s_fogLiquidLight > 0.0f))
		fprintf(stderr, a > 0.0f ? "RT fog: light in the water armed (%.2f)\n" : "RT fog: light in the water off\n", a);
	s_fogLiquidLight = a;
}

void RT_Metal_SetFogHistCentre(int enable)
{
	s_fogHistCentre = enable ? 1 : 0;
}

// S2 of the fog-buffer plan: spread the fog march's jitter along the ray
// (see the kernel comment at the march line). 0 reproduces the old bytes.
void RT_Metal_SetFogStepJitter(int enable)
{
	s_fogStepJitter = enable ? 1 : 0;
}

// S1 of the fog-buffer plan: the depth-aware filter over the fog kernel's
// output at fog resolution. mode 0 off (the kernel writes the surface directly,
// byte for byte), 1 = 3x3, 2 = 5x5; depthtol is the relative tolerance the
// depth-aware upsample already uses (rt_metal_fog_upsample_depth).
void RT_Metal_SetFogFilter(int mode, float depthtol)
{
	s_fogFilter = (mode < 0) ? 0 : (mode > 4 ? 4 : mode);   // 4 = the 5x5 twice, the second pass dilated (2026-09-03)
	s_fogFilterDepth = (depthtol < 0.001f) ? 0.001f : (depthtol > 0.5f ? 0.5f : depthtol);
}

// EVERY LIGHT CASTS A SHADOW (rt_metal_lightsample, 2026-08-17; mode split
// 2026-08-28). The design is one estimator -- shadow-test a light chosen in
// proportion to what it delivers here and divide by that probability, so lights
// that lose the dominance vote (every torch, almost everywhere) stop being
// summed unshadowed -- but the two halves fare differently: the fog/shaft
// kernels absorb the variance (march-step averaging plus their reprojected
// EMAs), while the surface kernel's per-pixel pick under wall lighting IS the
// scene's lighting and read as speckle (the 2026-08-17 QA rejection). Hence a
// MODE, split here on the C side with zero ABI change: 0 = off (dominant-only
// kernels byte for byte, unshadowed residual fill included); 1 = FOG-ONLY (the
// fog and shaft cams get the flag, the surface cam never does, so the rejected
// arm is unreachable by construction); 2 = the full 2026-08-17 estimator, kept
// verbatim for A/B. The fog/shaft cam fills read s_lightSample as a boolean,
// which under mode storage is exactly (mode >= 1).
void RT_Metal_SetLightSample(int enable, int rays, float wclamp)
{
	int mode = (enable < 0) ? 0 : (enable > 2 ? 2 : enable);
	// The fog/shaft 1/p clamp (2026-08-29, the demo17 speckle fix): 0 =
	// unclamped, the old estimator byte for byte; floored at 1 otherwise (a
	// ceiling below 1 would invert the weighting, which nobody can want).
	float cl = (wclamp > 0.0f) ? fmaxf(wclamp, 1.0f) : 0.0f;
	// change-only, so it is rate-safe under the console-ink rule; it is the
	// liveness line smoke greps (the feature has no other console evidence).
	// The clamp rides the same line so its own liveness is visible; the mode
	// prefix is what smoke matches, so appending keeps those greps whole.
	if (mode != s_lightSample || cl != s_lsClamp)
		fprintf(stderr, "RT lightsample mode %d (%s), clamp %.1f\n", mode,
		        mode == 0 ? "dominant-only" : (mode == 1 ? "fog-only stochastic" : "full stochastic"), cl);
	s_lightSample = mode;
	s_lsClamp = cl;
	s_cam.lsample = (s_lightSample == 2) ? 1u : 0u;
	s_cam.lsrays = (uint32_t)(rays < 1 ? 1 : (rays > 16 ? 16 : rays));
}

void RT_Metal_SetRefit(int mode)
{
	s_refitMode = mode ? 1 : 0;
}

// ONE-BOUNCE GI (rt_metal_gi, GIARC G1, 2026-08-29). All six knobs arrive
// together each frame, the SetLightSample shape; 0 leaves the kernel's gi
// branches untaken, the old term byte for byte. emissive (G3) widens the
// bounce mask to the core/lava instances and scales their emission; 0 keeps
// the G1 bounce mask and bytes exactly.
void RT_Metal_SetGI(int enable, float dist, float albedo, float history, float intensity, float emissive, int rate, float albtex, int fallback, float tiledilate)
{
	int v = enable ? 1 : 0;
	float e = (emissive < 0.0f) ? 0.0f : (emissive > 8.0f ? 8.0f : emissive);
	uint32_t rr = (uint32_t)((rate < 1) ? 1 : (rate > 8 ? 8 : rate));
	float at = (albtex < 0.0f) ? 0.0f : (albtex > 1.0f ? 1.0f : albtex);
	float td = (tiledilate < 0.0f) ? 0.0f : (tiledilate > 1.0f ? 1.0f : tiledilate);
	// change-only, the console-ink rule; it is the liveness line smoke greps.
	// The emissive state and the sample rate ride the same line (mode
	// suffixes, the lightsample precedent) so their liveness is grep-able
	// without a second print.
	if (v != s_giEnable || (v && (e > 0.0f) != (s_cam.giemissive > 0.0f)) || (v && rr != s_cam.girate) || (v && (at > 0.0f) != (s_cam.gialbtex > 0.0f))
	    || (v && (fallback ? 1u : 0u) != s_cam.gifallback) || (v && (td > 0.0f) != (s_cam.gitiledilate > 0.0f)))
	{
		if (v)
			fprintf(stderr, "RT GI armed (one-bounce%s%s%s%s, rate 1/%u)\n", e > 0.0f ? "+emissive" : "", at > 0.0f ? "+albedo" : "", fallback ? "+fallback" : "", td > 0.0f ? "+dilate" : "", rr);
		else
			fprintf(stderr, "RT GI off\n");
	}
	s_giEnable = v;
	s_cam.gi = (uint32_t)v;
	s_cam.gidist = (dist < 64.0f) ? 64.0f : (dist > 8192.0f ? 8192.0f : dist);
	s_cam.gialbedo = (albedo < 0.0f) ? 0.0f : (albedo > 1.0f ? 1.0f : albedo);
	s_giHistoryRaw = (history < 0.0f) ? 0.0f : (history > 0.98f ? 0.98f : history);
	s_cam.giintensity = (intensity < 0.0f) ? 0.0f : (intensity > 8.0f ? 8.0f : intensity);
	s_cam.giemissive = e;
	s_cam.girate = rr;
	s_cam.gialbtex = at;
	s_cam.gbias = (uint32_t)s_giBias;
	memcpy(s_cam.sundir, s_sunDir, sizeof(float) * 3);   // SEPTEMBER2 D
	memcpy(s_cam.suncol, s_sunCol, sizeof(float) * 3);
	s_cam.sunpen = s_sunPen;
	s_cam.sun = (uint32_t)s_sun;
	s_cam.gifallback = (uint32_t)(fallback ? 1 : 0);
	s_cam.gitiledilate = td;
	s_cam.liqrt = (uint32_t)s_liqrt;          // SEPTEMBER2 C2
	s_cam.liqreflect = s_liqReflect;
}

void RT_Metal_SetFogAdaptiveStride(int mode)
{
	int v = mode ? 1 : 0;
	// change-only: the feature's console evidence (smoke greps it)
	if (v != s_fogAdStride)
		fprintf(stderr, "RT fog adaptive stride %s\n", v ? "active" : "off");
	s_fogAdStride = v;
}

// BLUENOISE slice 2 / 3: rt_metal_fog_clamp, _clamp_k, rt_metal_fog_tonemapema,
// pushed per frame from cl_screen (the SetFogFilter shape). The mode line is
// change-only and is the feature's only console evidence (smoke greps it).
void RT_Metal_SetFogClamp(int mode, float k, int tonemap)
{
	static int last = -1;
	int state;
	if (mode < 0) mode = 0; else if (mode > 3) mode = 3;
	if (k < 0.1f) k = 0.1f; else if (k > 8.0f) k = 8.0f;
	s_fogClamp = mode;
	s_fogClampK = k;
	s_fogTonemapEma = tonemap ? 1 : 0;
	state = mode + 16 * s_fogTonemapEma;
	if (state != last) {
		last = state;
		fprintf(stderr, "RT fog clamp mode %d (tonemap %d)\n", mode, s_fogTonemapEma);
	}
}

// rt_metal_fog_reproject_depth / _tol (2026-09-03): the history pass's translation-
// aware read. Change-only line, like the clamp's.
void RT_Metal_SetFogReproject(int depth, float tol)
{
	static int last = -1;
	if (tol < 0.02f) tol = 0.02f; else if (tol > 2.0f) tol = 2.0f;
	if (depth < 0) depth = 0; else if (depth > 5) depth = 5;
	s_fogReprojDepth = depth;   // 1 = translation-aware; 2, 3 = the debug probes (see the kernel)
	s_fogReprojTol = tol;
	if (s_fogReprojDepth != last) { last = s_fogReprojDepth; fprintf(stderr, "RT fog history reprojection: %s\n", s_fogReprojDepth == 1 ? "translation-aware (scatter depth)" : (s_fogReprojDepth == 0 ? "rotation-only" : "DEBUG probe")); }
}

// A1 FROXEL (2026-09-06): rt_metal_fog_froxel and its three knobs, pushed per
// frame from cl_screen. The mode line is change-only and is the feature's
// console evidence beside the volume allocation line.
// SEPTEMBER2 D: the sky light. dir is TOWARD the sun (unit), col in Lsum units,
// pen the sun disc's angular radius in DEGREES. Change-only line.
void RT_Metal_SetSun(const float *dir, const float *col, float pen_deg, int enable)
{
	static int last = -1;
	s_sun = (enable && dir && col) ? 1 : 0;
	if (s_sun) { memcpy(s_sunDir, dir, sizeof(float) * 3); memcpy(s_sunCol, col, sizeof(float) * 3); }
	if (pen_deg < 0.0f) pen_deg = 0.0f; else if (pen_deg > 20.0f) pen_deg = 20.0f;
	s_sunPen = (float)tan(pen_deg * (M_PI / 180.0));
	if (s_sun != last) { last = s_sun; if (s_sun) fprintf(stderr, "RT sky light ON (toward %.2f %.2f %.2f, colour %.2f %.2f %.2f, penumbra %.1f deg)\n", s_sunDir[0], s_sunDir[1], s_sunDir[2], s_sunCol[0], s_sunCol[1], s_sunCol[2], pen_deg); else fprintf(stderr, "RT sky light off\n"); }
}

// SEPTEMBER2 A3: rt_metal_as_skipstatic. Change-only line, the feature's console evidence.
void RT_Metal_SetASSkip(int enable)
{
	static int last = -1;
	s_asSkip = enable ? 1 : 0;
	if (s_asSkip != last) { last = s_asSkip; fprintf(stderr, "RT AS skip-static %s\n", s_asSkip ? "ON (unchanged BLAS/TLAS reused)" : "off"); }
}

// SEPTEMBER2 C2 (2026-09-09): rt_metal_liquids_rt / rt_metal_liquids_reflect. Change-only
// line, the feature's console evidence (the pair is a texture nobody else can see).
void RT_Metal_SetLiquidRT(int enable, float reflect)
{
	static int last = -1;
	s_liqrt = enable ? 1 : 0;
	s_liqReflect = (reflect < 0.0f) ? 0.0f : (reflect > 4.0f ? 4.0f : reflect);
	if (s_liqrt != last) { last = s_liqrt; fprintf(stderr, "RT liquid pair %s\n", s_liqrt ? "ON (per-pixel own term + Fresnel reflection on blended water)" : "off"); }
}

void RT_Metal_SetFogFroxel(int enable, int slices, float history, float curve, float near, int castphase)
{
	s_froxelCastPhase = castphase ? 1 : 0;
	static int last = -1;
	if (slices < 4) slices = 4; else if (slices > 64) slices = 64;
	if (history < 0.0f) history = 0.0f; else if (history > 0.98f) history = 0.98f;
	if (curve < 0.0f) curve = 0.0f; else if (curve > 8.0f) curve = 8.0f;
	if (near < 1.0f) near = 1.0f; else if (near > 64.0f) near = 64.0f;
	s_froxel = enable ? 1 : 0;
	s_froxelSlices = slices;
	s_froxelHistory = history;
	s_froxelCurve = curve;
	s_froxelNear = near;
	if (s_froxel != last) { last = s_froxel; fprintf(stderr, "RT froxel fog %s\n", s_froxel ? "ON (per-cell history)" : "off (the 2D fog history)"); }
}

// rt_metal_lightsample_hybrid (2026-09-03): the fog cast shadows the dominant
// light on its own ray and picks over the rest. Change-only line.
void RT_Metal_SetLightSampleHybrid(int mode)
{
	static int last = -1;
	if (mode < 0) mode = 0; else if (mode > 3) mode = 3;
	s_lsHybrid = mode;
	if (mode != last) { last = mode; fprintf(stderr, "RT fog light pick: %s\n", mode == 0 ? "single pick" : (mode == 1 ? "dominant + pick (two rays)" : (mode == 2 ? "dominant / pick alternating" : "single-pass dominant / pick"))); }
}

// BEAUTY B1 (2026-09-16): ambient occlusion from the bounce ray. Change-only
// liveness line; the kernel branch is gated on giao > 0 and the GI history's
// alpha is written 0 while it is off, so 0 is the old term byte for byte.
void RT_Metal_SetGIAO(float ao, float dist)
{
	float a = (ao < 0.0f) ? 0.0f : (ao > 1.0f ? 1.0f : ao);
	float d = (dist < 1.0f) ? 1.0f : (dist > 1024.0f ? 1024.0f : dist);
	if ((a > 0.0f) != (s_cam.giao > 0.0f))
		fprintf(stderr, a > 0.0f ? "RT GI ambient occlusion armed (%.2f over %.0f units)\n" : "RT GI ambient occlusion off\n", a, d);
	s_cam.giao = a;
	s_cam.giaodist = d;
}

// BEAUTY B3 (2026-09-17): contact-hardened shadows. Change-only line.
void RT_Metal_SetContact(float amount)
{
	float a = (amount < 0.0f) ? 0.0f : (amount > 1.0f ? 1.0f : amount);
	if ((a > 0.0f) != (s_cam.contact > 0.0f))
		fprintf(stderr, a > 0.0f ? "RT contact-hardened shadows armed (%.2f)\n" : "RT contact-hardened shadows off\n", a);
	s_cam.contact = a;
}

// THE ROUND SPOTLIGHTS (2026-09-19). How many of a pixel's brightest lights get
// a shadow test, and how many rays each runner-up spends. 1 = the dominant
// alone, which is every frame this kernel has ever rendered. Change-only line.
void RT_Metal_SetShadowLights(int lights, int rays)
{
	int l = lights < 1 ? 1 : (lights > 4 ? 4 : lights);
	int r = rays < 1 ? 1 : (rays > 8 ? 8 : rays);
	if ((uint32_t)l != s_cam.shadowlights)
		fprintf(stderr, l > 1 ? "RT shadow lights: %d per pixel (%d rays each past the dominant)\n" : "RT shadow lights: the dominant alone\n", l, r);
	s_cam.shadowlights = (uint32_t)l;
	s_cam.shadowlightrays = (uint32_t)r;
}

void RT_Metal_SetTermMax(float knee)
{
	// 0 = off (the old bytes: the kernel's branch is not taken); otherwise the
	// knee is floored at 1 -- a knee below the neutral term would compress the
	// ordinary lightmap-like range, which is a look change nobody asked for
	s_cam.lmax = (knee > 0.0f) ? fmaxf(knee, 1.0f) : 0.0f;
}

void RT_Metal_SetReprojectDepth(int enable)
{
	s_reprojDepth = enable ? 1 : 0;
}

static int rt_reproj_test_mode(void);   // defined with the accessors below

// Is the composite's reprojection LIVE, as opposed to clamped to its disabled
// path? Under rt_metal_sameframe the shown slot is this frame's own, so the
// remap is an exact identity in algebra but NOT in IEEE (the tan
// pre-scale/pre-divide round trip), and s_reproject forces the term sampler
// LINEAR even at full trace resolution -- so same-frame clamps the remap
// consumers to their disabled paths, which are documented bit-for-bit
// reproductions of the pre-reprojection composite, and full res gets its
// bit-exact NEAREST fetch back. RT_METAL_REPROJ_TEST holds the clamp OPEN:
// the perturbation lives inside the accessors, and an unconditional clamp
// would silently kill the only moving-camera instrument the frozen beds have.
// RT_METAL_SYNC deliberately does NOT clamp -- it keeps its historical
// slot-order-only meaning so measurements taken under it stay comparable.
//
// THE FOG/SHAFT EMA GATES DO NOT USE THIS HELPER, ON PURPOSE. Their history
// (s_fogmtex[curslot^1] / s_shmtex[curslot^1]) is one ENCODE frame old
// whatever slot is shown, so its reprojection has real work under same-frame
// too -- clamping it would smear the fog on turns. Two gates, split on
// purpose: hasPrevCam keys on s_reproject raw.
static int rt_reproject_live(void)
{
	return s_reproject && (!s_sameFrame || rt_reproj_test_mode());
}

void RT_Metal_SetSameFrame(int enable)
{
	int want = enable ? 1 : 0;
	if (want == s_sameFrame)
		return;
	// The transition frame is the hazard, not the steady state: flipping
	// async->same-frame leaves the OTHER slot's command buffer un-waited, and
	// the very next frame's SetLights/SetEntities rewrite that slot's buffers
	// -- exactly the race the invariant on s_lightbuf forbids ("the written
	// slot's previous CB was waited on at the last composite"). Drain
	// everything in flight instead. The shown slot's SURFACES are untouched
	// (rt_wait_pending only waits on and nils command buffers), so
	// s_lastShown stays valid and every consumer keeps a coherent frame
	// across the flip -- which is why this deliberately does NOT touch
	// s_lastShown: the rt_abandon_gl/rt_release_surface precedents set -1
	// because they destroy the surfaces the slot points at; this does not.
	rt_wait_pending();
	s_sameFrame = want;
}

void RT_Metal_SetTuning(int samples, float softness, float darkness, float history, float colorstr, float walllight, float ambient, float scale, int reproject)
{
	if (samples < 1) samples = 1; else if (samples > 64) samples = 64;
	if (softness < 0.0f) softness = 0.0f; else if (softness > 4.0f) softness = 4.0f;
	if (darkness < 0.0f) darkness = 0.0f; else if (darkness > 1.0f) darkness = 1.0f;
	if (history < 0.0f) history = 0.0f; else if (history > 0.98f) history = 0.98f;  // cap: avoid history that never refreshes
	if (colorstr < 0.0f) colorstr = 0.0f; else if (colorstr > 4.0f) colorstr = 4.0f;
	if (walllight < 0.0f) walllight = 0.0f; else if (walllight > 8.0f) walllight = 8.0f;
	if (ambient < 0.0f) ambient = 0.0f; else if (ambient > 2.0f) ambient = 2.0f;
	if (scale < 0.25f) scale = 0.25f; else if (scale > 1.0f) scale = 1.0f;
	s_cam.samples  = (uint32_t)samples;
	s_cam.softness = softness;
	s_cam.darkness = darkness;
	s_cam.history  = history;
	s_cam.colorstr = colorstr;
	s_cam.walllight = walllight;
	s_cam.ambient  = ambient;
	s_scale = scale;
	s_reproject = reproject ? 1 : 0;
}

// ---------------------------------------------------------------------------
// shared IOSurface + GL blit machinery (unchanged from the blit spike)
// ---------------------------------------------------------------------------

static void rt_pair_release(IOSurfaceRef surf[2], id<MTLTexture> __strong mtex[2], GLuint gltex[2], bool deleteGL);

// Give the renderer back its handles for the term pair (shared-device path).
// The GL counterpart is glDeleteTextures in rt_pair_release; both mean "the name
// I lent you is no longer mine to lend". Always safe: the arrays are zero on the
// GL path, and Metal_Texture_Destroy ignores 0.
static void rt_adopt_release(void)
{
	for (int i = 0; i < 2; i++) {
		Metal_Texture_Destroy(s_termHandle[i]);
		s_termHandle[i] = 0;
		s_termHandleTexPtr[i] = NULL;
		s_termHandleFlags[i] = 0;
		// 6-3: the fog and shaft handles ride the SAME release, because they
		// have the same lifetime -- every caller of this function is a device
		// or context teardown, and leaving either behind would hand a stale
		// texture to the next renderpath.
		Metal_Texture_Destroy(s_fogHandle[i]);
		s_fogHandle[i] = 0;
		s_fogHandleTexPtr[i] = NULL;
		Metal_Texture_Destroy(s_shHandle[i]);
		s_shHandle[i] = 0;
		s_shHandleTexPtr[i] = NULL;
		Metal_Texture_Destroy(s_liqHandle[i]);   // SEPTEMBER2 C2: same lifetime, same release
		s_liqHandle[i] = 0;
		s_liqHandleTexPtr[i] = NULL;
	}
}

// The pointer-identity adoption, factored so fog and shafts share one body
// rather than becoming two more near-identical copies. The term keeps its own
// (rt_adopt_ensure below) because it also caches FLAGS, which vary with
// rt_metal_scale and reprojection; these two do not.
static int rt_adopt_slot(void *texptr, int *handle, void **cached, int w, int h)
{
	if (!texptr)
		return 0;
	if (*handle && *cached == texptr)
		return *handle;
	Metal_Texture_Destroy(*handle);
	*handle = Metal_Texture_Adopt(texptr, w, h, RT_ADOPT_FOGFLAGS);
	*cached = *handle ? texptr : NULL;
	return *handle;
}

// Make sure slot `slot`'s term texture is adopted into the renderer's texture
// table with the sampler `wantflags` asks for, minting a handle only when the
// texture object or the filtering has actually changed. Returns the handle, or
// 0 -- and 0 must reach the caller as "do not draw", never as "draw with handle
// 0", which binds nothing and would multiply the whole scene by black.
static int rt_adopt_ensure(int slot, int wantflags)
{
	void *texptr = (__bridge void *)s_mtex[slot];

	if (!texptr)
		return 0;
	if (s_termHandle[slot] && s_termHandleTexPtr[slot] == texptr && s_termHandleFlags[slot] == wantflags)
		return s_termHandle[slot];

	Metal_Texture_Destroy(s_termHandle[slot]);
	s_termHandle[slot] = Metal_Texture_Adopt(texptr, s_w, s_h, wantflags);
	s_termHandleTexPtr[slot] = s_termHandle[slot] ? texptr : NULL;
	s_termHandleFlags[slot] = s_termHandle[slot] ? wantflags : 0;
	return s_termHandle[slot];
}

static void rt_abandon_gl(void)   // GL context destroyed: names are stale, don't glDelete
{
	rt_wait_pending();   // in-flight trace still writes the surfaces below
	s_prog = 0;
	s_vao = 0;
	rt_adopt_release();   // the Metal-path counterpart of not-glDeleting the names below
	rt_pair_release(s_surf,   s_mtex,   s_gltex,   false);
	rt_pair_release(s_shsurf, s_shmtex, s_shgltex, false);
	rt_pair_release(s_fogsurf, s_fogmtex, s_foggltex, false);
	rt_pair_release(s_liqsurf, s_liqmtex, s_liqgltex, false);   // SEPTEMBER2 C2
	s_shValid[0] = s_shValid[1] = 0;
	s_fogValid[0] = s_fogValid[1] = 0;
	s_w = s_h = 0;
	s_shw = s_shh = 0;
	s_shHasPrev = 0;
	s_fogw = s_fogh = 0;
	s_fogHasPrev = 0;
	s_lastShown = -1;   // every GL name above is stale: nothing is showable
}

// Lazily build the multiply-composite program + a VAO (once per GL context).
static bool rt_ensure_composite_program(void)
{
	GLint ok;
	if (!s_prog) {
		char log[512];
		GLuint vs = glCreateShader(GL_VERTEX_SHADER);
		glShaderSource(vs, 1, &kCompVS, NULL); glCompileShader(vs);
		glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
		if (!ok) { glGetShaderInfoLog(vs, sizeof log, NULL, log); fprintf(stderr, "RT_Metal: composite VS: %s\n", log); }
		GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
		glShaderSource(fs, 1, &kCompFS, NULL); glCompileShader(fs);
		glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
		if (!ok) { glGetShaderInfoLog(fs, sizeof log, NULL, log); fprintf(stderr, "RT_Metal: composite FS: %s\n", log); }
		s_prog = glCreateProgram();
		glAttachShader(s_prog, vs); glAttachShader(s_prog, fs);
		glLinkProgram(s_prog);
		glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
		glDeleteShader(vs); glDeleteShader(fs);
		if (!ok) { glGetProgramInfoLog(s_prog, sizeof log, NULL, log); fprintf(stderr, "RT_Metal: composite link: %s\n", log); glDeleteProgram(s_prog); s_prog = 0; return false; }
		// bind the sampler to texture unit 0 (restore the caller's program)
		GLint prevProg = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
		glUseProgram(s_prog);
		GLint loc = glGetUniformLocation(s_prog, "rttex");
		if (loc >= 0) glUniform1i(loc, 0);
		loc = glGetUniformLocation(s_prog, "rtdepthtex");
		if (loc >= 0) glUniform1i(loc, 1);   // TERM UPSAMPLE: scene depth on unit 1
		s_locRtoff = glGetUniformLocation(s_prog, "rtoff");
		s_locRtscale = glGetUniformLocation(s_prog, "rtscale");
		s_locUp  = glGetUniformLocation(s_prog, "rtUp");
		s_locUp2 = glGetUniformLocation(s_prog, "rtUp2");
		s_locS2D = glGetUniformLocation(s_prog, "ScreenToDepth");
		{
			static const char *rpn[10] = { "rpCurF", "rpCurR", "rpCurU", "rpShF", "rpShR", "rpShU", "rpParams", "rpView", "rpCurO", "rpShO" };
			for (int ri = 0; ri < 10; ri++) s_locRp[ri] = glGetUniformLocation(s_prog, rpn[ri]);
		}
		glUseProgram((GLuint)prevProg);
	}
	if (!s_vao)
		glGenVertexArrays(1, &s_vao);
	return s_prog && s_vao;
}

static void rt_release_fog_surface(void);
static void rt_release_liquid_surface(void);   // SEPTEMBER2 C2
static bool rt_ensure_liquid_surface(int w, int h);

static void rt_release_shaft_surface(void)
{
	rt_wait_pending();   // never release a surface an in-flight shaft pass still writes
	rt_pair_release(s_shsurf, s_shmtex, s_shgltex, true);
	s_shValid[0] = s_shValid[1] = 0;
	s_shw = s_shh = 0;
	s_shHasPrev = 0;
	// NOT s_lastShown = -1: same reasoning as rt_release_fog_surface below.
}

// ---------------------------------------------------------------------------
// Shared IOSurface pair machinery. Every output the sidecar hands GL -- the
// term, the fog buffer, the shaft buffer -- is the same double-buffered
// IOSurface + MTLTexture + GL-rectangle triple; one ensure/release pair
// serves all three (this replaced three hand-copied builders plus a fourth
// inlined teardown in rt_abandon_gl).
// ---------------------------------------------------------------------------

static void rt_pair_release(IOSurfaceRef surf[2], id<MTLTexture> __strong mtex[2], GLuint gltex[2], bool deleteGL)
{
	for (int i = 0; i < 2; i++) {
		if (gltex[i]) { if (deleteGL) glDeleteTextures(1, &gltex[i]); gltex[i] = 0; }
		mtex[i] = nil;
		if (surf[i]) { CFRelease(surf[i]); surf[i] = NULL; }
	}
}

// (re)create the pair at w x h. RGBA16F (half-float) so the outputs are HDR:
// the term can exceed 1.0 to BRIGHTEN toward a dynamic light's colour. label
// prefixes the error lines ("", "fog ", "shaft "), keeping the historic
// stderr strings; the caller does its own flag/size cleanup on failure.
static bool rt_pair_ensure(IOSurfaceRef surf[2], id<MTLTexture> __strong mtex[2], GLuint gltex[2],
                           int w, int h, GLint filter, const char *label)
{
	// METAL.md Phase 5 slice 2: on the shared-device path the whole reason for
	// the triple is gone. The IOSurface exists so a SECOND MTLDevice's writes
	// can reach GL, and the GL rectangle name exists so GL can sample it --
	// with the sidecar on the renderer's own device, the kernel's write target
	// IS the texture the renderer will bind, so only the middle third survives.
	// surf[] stays NULL and gltex[] stays 0, which every release/guard site
	// already tolerates (rt_pair_release guards both; the accessors gate on the
	// GL name existing, so GL-side consumers can never receive a bogus handle).
	//
	// Storage is SHARED here on purpose, not an oversight: rt_metal_termprobe
	// reads these bytes back with getBytes for the cross-backend identity gate,
	// and the IOSurface-backed textures on the GL path are Shared too, so the
	// two paths stay comparable by the same code. Moving the outputs to
	// MTLStorageModePrivate is slice 5-6's own change, deliberately separate so
	// the byte gate can attribute "the port is correct" and "the port plus a
	// storage change is correct" independently.
	//
	// `filter` is unused in this arm and that is correct: filtering is sampler
	// state on Metal, chosen where the texture is bound (slice 5-3), not
	// texture state chosen here. The NEAREST-at-full-res-without-reproject
	// predicate survives as a sampler choice, not as a glTexParameteri.
	if (s_sharedDevice) {
		for (int i = 0; i < 2; i++) {
			MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
																						 width:w height:h mipmapped:NO];
			td.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
			td.storageMode = MTLStorageModeShared;
			mtex[i] = [s_dev newTextureWithDescriptor:td];
			if (!mtex[i]) { fprintf(stderr, "RT_Metal: %stexture failed\n", label); rt_pair_release(surf, mtex, gltex, false); return false; }
		}
		return true;
	}
	for (int i = 0; i < 2; i++) {
		NSDictionary *props = @{
			(id)kIOSurfaceWidth:           @(w),
			(id)kIOSurfaceHeight:          @(h),
			(id)kIOSurfaceBytesPerElement: @8,
			(id)kIOSurfacePixelFormat:     @((uint32_t)'RGhA'),   // kCVPixelFormatType_64RGBAHalf
		};
		surf[i] = IOSurfaceCreate((__bridge CFDictionaryRef)props);
		if (!surf[i]) { fprintf(stderr, "RT_Metal: %sIOSurfaceCreate failed\n", label); rt_pair_release(surf, mtex, gltex, true); return false; }

		MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
																					 width:w height:h mipmapped:NO];
		td.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
		td.storageMode = MTLStorageModeShared;
		mtex[i] = [s_dev newTextureWithDescriptor:td iosurface:surf[i] plane:0];
		if (!mtex[i]) { fprintf(stderr, "RT_Metal: %stexture failed\n", label); rt_pair_release(surf, mtex, gltex, true); return false; }

		glGenTextures(1, &gltex[i]);
		glBindTexture(GL_TEXTURE_RECTANGLE, gltex[i]);
		glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, filter);
		glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, filter);
		CGLError cglerr = CGLTexImageIOSurface2D(CGLGetCurrentContext(), GL_TEXTURE_RECTANGLE, GL_RGBA16F, w, h,
												 GL_RGBA, GL_HALF_FLOAT, surf[i], 0);
		glBindTexture(GL_TEXTURE_RECTANGLE, 0);
		if (cglerr != kCGLNoError) { fprintf(stderr, "RT_Metal: %sCGLTexImageIOSurface2D failed: %s\n", label, CGLErrorString(cglerr)); rt_pair_release(surf, mtex, gltex, true); return false; }
	}
	return true;
}

static void rt_release_surface(void)
{
	rt_wait_pending();   // never release a surface an in-flight trace still writes
	rt_adopt_release();  // hand the renderer's handles back BEFORE dropping the textures
	rt_pair_release(s_surf, s_mtex, s_gltex, true);
	s_histTex[0] = nil; s_histTex[1] = nil;   // temporal history invalid at a new size
	s_secTex[0] = nil; s_secTex[1] = nil;
	s_giTex[0] = nil; s_giTex[1] = nil;
	s_hasPrev = 0;
	s_w = s_h = 0;
	s_slotCam[0].valid = s_slotCam[1].valid = 0;
	s_lastShown = -1;   // the main pair is gone: nothing is showable until a new trace lands
	// the shaft and fog kernels read the (now stale-sized) history texture, so
	// their pairs go with the main one; lazily recreated at the new size
	rt_release_shaft_surface();
	rt_release_fog_surface();
	rt_release_liquid_surface();   // SEPTEMBER2 C2: sized from the main pair, so it goes with it
}

static bool rt_ensure_surface(int w, int h, bool fullres)
{
	// Keyed on the MTLTexture, the one third of the triple that exists on BOTH
	// paths. Keying it on the IOSurface (which is NULL on the shared-device
	// path) makes this early-out never fire there, so every frame would
	// rt_release_surface() and rebuild -- and that path sets s_lastShown = -1,
	// so nothing is ever showable. Equivalent on GL by the pair invariant:
	// rt_pair_ensure creates all three thirds together and fails as a unit,
	// rt_pair_release clears all three together.
	if (s_mtex[0] && s_mtex[1] && w == s_w && h == s_h)
		return true;

	rt_release_surface();
	s_w = w; s_h = h;
	// full-res: NEAREST keeps the composite's 1:1 fetch bit-exact (the historic
	// look). Reduced res: LINEAR so the magnified term stays smooth. Reprojection
	// samples at fractional coordinates, so it wants LINEAR even at full res --
	// the composite flips the parameter at draw time when the mode changes.
	GLint filter = (fullres && !rt_reproject_live()) ? GL_NEAREST : GL_LINEAR;
	s_gltexFilterCur = filter;

	// TWO surface/texture pairs: the async pipeline traces into one while the
	// composite reads the other (previous frame).
	if (!rt_pair_ensure(s_surf, s_mtex, s_gltex, w, h, filter, "")) { rt_release_surface(); return false; }

	// (re)allocate the temporal-accumulation history ping-pong at the new size.
	// GPU-private RGBA32F; not cleared — the first frame runs with hasPrev=0 (no
	// read) and writes every pixel, so frame 2 onward reads fully-valid history.
	{
		MTLTextureDescriptor *hd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
																					   width:w height:h mipmapped:NO];
		hd.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
		hd.storageMode = MTLStorageModePrivate;
		s_histTex[0] = [s_dev newTextureWithDescriptor:hd];
		s_histTex[1] = [s_dev newTextureWithDescriptor:hd];
		if (!s_histTex[0] || !s_histTex[1]) { fprintf(stderr, "RT_Metal: history texture alloc failed\n"); rt_release_surface(); return false; }
		// The stochastic secondary's own accumulation (rt_metal_lightsample): it is
		// a COLOUR, so it cannot ride the visibility history's four used channels,
		// and 16F is ample for a term the composite multiplies. Allocated
		// unconditionally -- the kernel declares the pair in every build, and a
		// declared-but-unbound texture is the documented Metal validation trap.
		MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
																					   width:w height:h mipmapped:NO];
		sd.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
		sd.storageMode = MTLStorageModePrivate;
		s_secTex[0] = [s_dev newTextureWithDescriptor:sd];
		s_secTex[1] = [s_dev newTextureWithDescriptor:sd];
		if (!s_secTex[0] || !s_secTex[1]) { fprintf(stderr, "RT_Metal: secondary history alloc failed\n"); rt_release_surface(); return false; }
		// The GI colour EMA (rt_metal_gi): its own pair, same descriptor.
		// Allocated unconditionally for the same reason as s_secTex above --
		// the kernel declares the pair in every build, and a declared-but-
		// unbound texture is the documented Metal validation trap.
		s_giTex[0] = [s_dev newTextureWithDescriptor:sd];
		s_giTex[1] = [s_dev newTextureWithDescriptor:sd];
		if (!s_giTex[0] || !s_giTex[1]) { fprintf(stderr, "RT_Metal: GI history alloc failed\n"); rt_release_surface(); return false; }
		s_histParity = 0;
		s_hasPrev = 0;
	}

	return true;
}

// A1 FROXEL: the volume pairs. Released with the fog surface (a fog resize
// re-allocates everything), on a slice-count change, and when the cvar goes
// off (100 MB at Best's 1080p geometry is not worth holding for an A/B).
static void rt_release_froxel_volumes(void)
{
	if (!s_froxA[0] && !s_froxB[0]) return;
	rt_wait_pending();
	s_froxA[0] = nil; s_froxA[1] = nil; s_froxB[0] = nil; s_froxB[1] = nil;
	s_froxValid[0] = s_froxValid[1] = 0;
	s_froxW = s_froxH = s_froxN = 0;
}

static bool rt_ensure_froxel_volumes(int w, int h, int n)
{
	if (s_froxA[0] && s_froxA[1] && s_froxB[0] && s_froxB[1] && w == s_froxW && h == s_froxH && n == s_froxN)
		return true;
	rt_release_froxel_volumes();
	MTLTextureDescriptor *d = [[MTLTextureDescriptor alloc] init];
	d.textureType = MTLTextureType3D;
	d.pixelFormat = MTLPixelFormatRGBA16Float;
	d.width = (NSUInteger)w; d.height = (NSUInteger)h; d.depth = (NSUInteger)n;
	d.mipmapLevelCount = 1;
	d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
	d.storageMode = MTLStorageModePrivate;
	for (int i = 0; i < 2; ++i) { s_froxA[i] = [s_dev newTextureWithDescriptor:d]; s_froxB[i] = [s_dev newTextureWithDescriptor:d]; }
	if (!s_froxA[0] || !s_froxA[1] || !s_froxB[0] || !s_froxB[1]) {
		fprintf(stderr, "RT_Metal: froxel volume alloc failed (%dx%dx%d) -- rt_metal_fog_froxel falls back to the 2D fog\n", w, h, n);
		rt_release_froxel_volumes();
		return false;
	}
	s_froxValid[0] = s_froxValid[1] = 0;
	s_froxW = w; s_froxH = h; s_froxN = n;
	fprintf(stderr, "RT froxel fog volume %dx%dx%d (%.1f MB)\n", w, h, n, (double)w * h * n * 8.0 * 4.0 / (1024.0 * 1024.0));
	return true;
}

// The froxel PSOs, compiled on first enable from the source the fog kernel's
// own compile stashed (so the two share every define). A compile failure
// prints once and the arm stays off: the fog kernel is untouched.
static void rt_ensure_froxel_pso(void)
{
	if (s_froxelPso && s_froxelIntPso) return;
	if (s_froxelTried || !s_froxelSrc || !s_dev) return;
	s_froxelTried = 1;
	@autoreleasepool {
		NSError *err = nil;
		id<MTLLibrary> lib = [s_dev newLibraryWithSource:s_froxelSrc options:nil error:&err];
		id<MTLComputePipelineState> pso = lib ? [s_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"rt_fog"] error:&err] : nil;
		if (!pso) { fprintf(stderr, "RT_Metal: froxel fog kernel compile failed (rt_metal_fog_froxel disabled): %s\n", err ? err.localizedDescription.UTF8String : "unknown"); return; }
		NSString *isrc = [NSString stringWithFormat:@"%s%s", DPD_SHADER_PRELUDE, kFroxelIntegrateSrc];
		NSError *ierr = nil;
		id<MTLLibrary> ilib = [s_dev newLibraryWithSource:isrc options:nil error:&ierr];
		id<MTLComputePipelineState> ipso = ilib ? [s_dev newComputePipelineStateWithFunction:[ilib newFunctionWithName:@"rt_froxelintegrate"] error:&ierr] : nil;
		if (!ipso) { fprintf(stderr, "RT_Metal: froxel integrate kernel compile failed (rt_metal_fog_froxel disabled): %s\n", ierr ? ierr.localizedDescription.UTF8String : "unknown"); return; }
		s_froxelPso = pso; s_froxelIntPso = ipso;
		fprintf(stderr, "RT froxel fog kernels compiled\n");
	}
}

static void rt_release_fog_surface(void)
{
	rt_wait_pending();
	rt_release_froxel_volumes();
	rt_pair_release(s_fogsurf, s_fogmtex, s_foggltex, true);
	s_fograw[0] = nil; s_fograw[1] = nil;
	s_fogema[0] = nil; s_fogema[1] = nil;
	s_fogdepth[0] = nil; s_fogdepth[1] = nil;
	s_fogEmaValid[0] = s_fogEmaValid[1] = 0;
	s_fogValid[0] = s_fogValid[1] = 0;
	s_fogw = s_fogh = 0;
	s_fogHasPrev = 0;
	// NOT s_lastShown = -1: the fog accessor gates on s_fogValid per slot, and
	// blanking the shown slot would also kill the term and force the fullbright
	// flash the composite's re-show path exists to prevent.
}

// The fog kernel's output pair, at its own (reduced) resolution. GL_LINEAR: the
// murk composite magnifies it.
// SEPTEMBER2 C2: the liquid pair, DOUBLE the trace width (own | reflection), lazily on the
// first frame the arm is on. RGBA16F like the term; LINEAR, the consumer magnifies it the
// way it magnifies the term. The handles are released with the term's (rt_adopt_release).
static void rt_release_liquid_surface(void)
{
	rt_wait_pending();
	for (int i = 0; i < 2; i++) { Metal_Texture_Destroy(s_liqHandle[i]); s_liqHandle[i] = 0; s_liqHandleTexPtr[i] = NULL; }
	rt_pair_release(s_liqsurf, s_liqmtex, s_liqgltex, true);
}
static bool rt_ensure_liquid_surface(int w, int h)
{
	if (w < 1 || h < 1) return false;
	rt_release_liquid_surface();
	if (!rt_pair_ensure(s_liqsurf, s_liqmtex, s_liqgltex, 2 * w, h, GL_LINEAR, "liquid ")) { rt_release_liquid_surface(); return false; }
	fprintf(stderr, "RT liquid pair %dx%d allocated (own term | reflection)\n", 2 * w, h);
	return true;
}

static bool rt_ensure_fog_surface(int w, int h)
{
	if (s_fogmtex[0] && s_fogmtex[1] && w == s_fogw && h == s_fogh)   // MTLTexture: see rt_ensure_surface
		return true;

	rt_release_fog_surface();
	s_fogw = w; s_fogh = h;
	if (!rt_pair_ensure(s_fogsurf, s_fogmtex, s_foggltex, w, h, GL_LINEAR, "fog ")) { rt_release_fog_surface(); return false; }
	// S1's scratch pair: the fog kernel writes RAW here when the filter is on and
	// the filter writes the real surface. Allocated with the surfaces, same size,
	// RGBA16F like the surfaces themselves, ShaderRead|ShaderWrite (the s_secTex
	// shape). Released with them above.
	if (!s_fograw[0] || !s_fograw[1] || (int)s_fograw[0].width != w || (int)s_fograw[0].height != h) {
		MTLTextureDescriptor *rd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:w height:h mipmapped:NO];
		rd.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
		rd.storageMode = MTLStorageModePrivate;
		s_fograw[0] = [s_dev newTextureWithDescriptor:rd];
		s_fograw[1] = [s_dev newTextureWithDescriptor:rd];
		if (!s_fograw[0] || !s_fograw[1]) { fprintf(stderr, "RT_Metal: fog filter scratch alloc failed\n"); s_fograw[0] = nil; s_fograw[1] = nil; }
	}
	// BLUENOISE slice 2's EMA history pair, same shape (the clamp's accumulator;
	// the surfaces above then hold the DISPLAY, filtered once)
	if (!s_fogema[0] || !s_fogema[1] || (int)s_fogema[0].width != w || (int)s_fogema[0].height != h) {
		MTLTextureDescriptor *ed = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:w height:h mipmapped:NO];
		ed.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
		ed.storageMode = MTLStorageModePrivate;
		s_fogema[0] = [s_dev newTextureWithDescriptor:ed];
		s_fogema[1] = [s_dev newTextureWithDescriptor:ed];
		s_fogEmaValid[0] = s_fogEmaValid[1] = 0;
		if (!s_fogema[0] || !s_fogema[1]) { fprintf(stderr, "RT_Metal: fog history alloc failed\n"); s_fogema[0] = nil; s_fogema[1] = nil; }
	}
	// the scatter-centroid depth pair rt_fog writes every frame (the history pass's
	// reprojection depth): R16F -- half precision at 3000 units is ~1.5 units
	if (!s_fogdepth[0] || !s_fogdepth[1] || (int)s_fogdepth[0].width != w || (int)s_fogdepth[0].height != h) {
		MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Float width:w height:h mipmapped:NO];
		dd.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
		// SHARED, not Private: RT_METAL_FOGDUMP reads this pair back with getBytes,
		// which is invalid on a Private texture and SEGFAULTED the first probe
		// run (2026-09-03). Unified memory makes Shared free for a 480x270 R16F.
		dd.storageMode = MTLStorageModeShared;
		s_fogdepth[0] = [s_dev newTextureWithDescriptor:dd];
		s_fogdepth[1] = [s_dev newTextureWithDescriptor:dd];
		if (!s_fogdepth[0] || !s_fogdepth[1]) { fprintf(stderr, "RT_Metal: fog depth alloc failed\n"); s_fogdepth[0] = nil; s_fogdepth[1] = nil; }
	}
	fprintf(stderr, "RT_Metal: fog buffer %dx%d\n", w, h);
	return true;
}

// The god-ray surface pair, at its own (reduced) resolution. GL_LINEAR: the
// murk samples this at a larger size, and beams magnified with NEAREST would
// show the shaft grid as blocks.
static bool rt_ensure_shaft_surface(int w, int h)
{
	if (s_shmtex[0] && s_shmtex[1] && w == s_shw && h == s_shh)   // MTLTexture: see rt_ensure_surface
		return true;

	rt_release_shaft_surface();
	s_shw = w; s_shh = h;
	if (!rt_pair_ensure(s_shsurf, s_shmtex, s_shgltex, w, h, GL_LINEAR, "shaft ")) { rt_release_shaft_surface(); return false; }
	fprintf(stderr, "RT_Metal: shaft buffer %dx%d\n", w, h);
	return true;
}

// Spike verification aid: if RT_METAL_DUMP is set, dump the final window
// backbuffer (scene + RT composite + HUD) as [int32 w][int32 h][RGBA8 bottom-up].
// Called from VID_Finish (end of frame) so it captures everything. By default it
// dumps once at frame 120 (a settled gameplay frame, not the loading plaque) to
// RT_METAL_DUMP. Set RT_METAL_DUMPFRAMES to a comma-separated list of frame
// numbers (e.g. "90,150,210") to capture several time points in ONE run, each
// written to "<RT_METAL_DUMP>.f<frame>" — used for static-camera A/B tests
// (e.g. verifying flickering lights change the shadows over time).
// Shared decision core for BOTH renderpaths' dump arms (METAL.md Phase 8-1a
// split it out of the GL-only function below; the sequence of checks is that
// function's, byte for byte, so the GL arm's behaviour is unchanged). Env
// parse once, the on-demand consume, the playback-frame counter, the ".fN"
// naming. Returns the output path (namebuf-backed for multi-frame names) or
// NULL when this frame does not dump; *outframe gets the frame number for the
// stderr line. ONE counter whichever arm calls: only one renderpath runs per
// vid_restart, and run-stable numbering is the counter's whole contract.
static const char *rt_dump_decide(int width, int height, int counting, char *namebuf, size_t namesize, int *outframe)
{
	static int inited = -1;
	static const char *path = NULL;
	static int frames[512];
	static int nframes = 0;
	static int multi = 0;
	static int frame = 0;
	int ondemand = s_dumpNow;   // console-requested snapshot (RT_Metal_RequestDump)
	s_dumpNow = 0;
	if (inited < 0) {
		inited = 1;
		path = getenv("RT_METAL_DUMP");
		const char *fr = getenv("RT_METAL_DUMPFRAMES");
		if (fr && *fr) {
			const char *p = fr;
			while (*p && nframes < 512) {
				int v = atoi(p);
				if (v > 0) frames[nframes++] = v;
				while (*p && *p != ',') p++;
				if (*p == ',') p++;
			}
			multi = (nframes > 1);
		}
		if (nframes == 0) { frames[0] = 120; nframes = 1; }
	}
	if (!path) return NULL;
	if (width < 1 || height < 1) return NULL;
	// Only frames the caller flags as ALIGNED (timedemo playback frames — one per
	// demo packet, so frame N is the same content in every run) advance the
	// counter. Presented-frame counting included the load screen, whose frame
	// count varies run to run: two runs' ".f900" were DIFFERENT demo frames, and
	// dump A/Bs read as wildly nondeterministic. Cost a determinism hunt.
	if (counting) ++frame;
	// RT_METAL_CAMTRACK=1 (BLUENOISE slice 0): one stderr line per COUNTED
	// (timedemo playback) frame with this frame's encode camera, so a demo's
	// parked and moving stretches can be found from one playback instead of
	// guessed from dump bursts. Needs RT_METAL_DUMP set (the counter above
	// is the frame numbering the dumps use, and it only runs under it).
	// Bed-only by construction: ~one line per frame is exactly the
	// console-ink hazard, so it is an env var and never a cvar.
	{
		static int ct = -1;
		if (ct < 0) { const char *e = getenv("RT_METAL_CAMTRACK"); ct = (e && atoi(e)) ? 1 : 0; }
		if (ct && counting)
			fprintf(stderr, "RT_Metal: camtrack f%d eye %.1f %.1f %.1f fwd %.3f %.3f %.3f\n", frame,
					s_cam.origin[0], s_cam.origin[1], s_cam.origin[2], s_cam.forward[0], s_cam.forward[1], s_cam.forward[2]);
	}
	if (!ondemand)
	{
		int i, match = 0;
		if (!counting) return NULL;
		for (i = 0; i < nframes; i++) if (frames[i] == frame) { match = 1; break; }
		if (!match) return NULL;
	}
	*outframe = frame;
	if (multi && !ondemand) { snprintf(namebuf, namesize, "%s.f%d", path, frame); return namebuf; }
	return path;
}

// The dump file: [int32 w][int32 h] then w*h*4 of RGBA8, bottom-up (GL row
// order) — both arms write the identical format, so the gate tooling never
// asks which renderpath produced a file.
static void rt_dump_write(const char *outpath, int width, int height, int frame, const unsigned char *rgba)
{
	FILE *f = fopen(outpath, "wb");
	if (f) {
		int32_t hdr[2] = { width, height };
		fwrite(hdr, sizeof hdr, 1, f);
		fwrite(rgba, (size_t)width * height * 4, 1, f);
		fclose(f);
		fprintf(stderr, "RT_Metal: dumped %dx%d frame %d to %s\n", width, height, frame, outpath);
	}
}

// RT_METAL_TERMDUMP=1: beside every frame dump, write the SHOWN RT term buffer
// too -- "<outpath>.term" = [int32 w][int32 h][RGBA16F, bottom-up, the kernel's
// row order] -- so a silhouette artefact can be attributed to the TERM (the
// kernel) or to the raster passes after it without guessing. Reads the shown
// slot exactly as RT_Metal_TermProbe does: that slot's command buffer was
// waited on in the composite and nothing re-encodes it until the next one, so
// getBytes at VID_Finish is race-free. Alpha is the hit distance (0 = sky),
// which is the reader's free depth-edge mask. Off = zero cost, no branch taken.
static void rt_dump_term(const char *outpath, int frame)
{
	static int want = -1;
	if (want < 0) { const char *e = getenv("RT_METAL_TERMDUMP"); want = (e && atoi(e)) ? 1 : 0; }
	if (!want || !s_dev || s_lastShown < 0 || !s_mtex[s_lastShown]) return;
	@autoreleasepool {
		id<MTLTexture> tex = s_mtex[s_lastShown];
		NSUInteger w = tex.width, h = tex.height;
		NSUInteger rowbytes = w * 8;             // RGBA16Float
		size_t total = (size_t)rowbytes * h;
		unsigned char *bytes = (unsigned char *)malloc(total);
		char name[1100];
		FILE *f;
		if (!bytes) return;
		[tex getBytes:bytes bytesPerRow:rowbytes fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
		snprintf(name, sizeof name, "%s.term", outpath);
		f = fopen(name, "wb");
		if (f) {
			int32_t hdr[2] = { (int32_t)w, (int32_t)h };
			fwrite(hdr, sizeof hdr, 1, f);
			fwrite(bytes, total, 1, f);
			fclose(f);
			fprintf(stderr, "RT_Metal: dumped %ux%u term (slot %d) frame %d to %s\n", (unsigned)w, (unsigned)h, s_lastShown, frame, name);
		}
		free(bytes);
	}
}

// RT_METAL_FOGDUMP=1 (BLUENOISE slice 0, 2026-09-03): beside every frame
// dump, write the SHOWN fog surface too -- "<outpath>.fog" = [int32 w][int32
// h][RGBA16F, the kernel's row order: row 0 = bottom], RGB = in-scattered
// light, A = transmittance. This is the fog BEFORE the depth-aware
// magnification, the murk composite and MetalFX: the quantity every
// kernel-side despeckle claim is actually about, where the composited frame
// is fog x upsample x composite x scaler and the scaler confounds every
// number read off it. With rt_metal_fog_filter on it is the FILTERED surface
// (what the composite consumes; the raw scatter is in s_fograw). Reads the
// shown slot exactly as rt_dump_term does. The stderr line carries that
// slot's ENCODE camera so a dumped frame can be classified parked / moving
// without a second playback (the S2 eye-track lesson: the dumps carry no
// camera). test/flicker.py fogseq reads the file. Off = zero cost.
static void rt_dump_fog(const char *outpath, int frame)
{
	static int want = -1;
	if (want < 0) { const char *e = getenv("RT_METAL_FOGDUMP"); want = (e && atoi(e)) ? 1 : 0; }
	if (!want || !s_dev || s_lastShown < 0 || !s_fogValid[s_lastShown] || !s_fogmtex[s_lastShown]) return;
	@autoreleasepool {
		id<MTLTexture> tex = s_fogmtex[s_lastShown];
		const rt_slotcam_t *c = &s_slotCam[s_lastShown];
		NSUInteger w = tex.width, h = tex.height;
		NSUInteger rowbytes = w * 8;             // RGBA16Float
		size_t total = (size_t)rowbytes * h;
		unsigned char *bytes = (unsigned char *)malloc(total);
		char name[1100];
		FILE *f;
		if (!bytes) return;
		[tex getBytes:bytes bytesPerRow:rowbytes fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
		snprintf(name, sizeof name, "%s.fog", outpath);
		f = fopen(name, "wb");
		if (f) {
			int32_t hdr[2] = { (int32_t)w, (int32_t)h };
			fwrite(hdr, sizeof hdr, 1, f);
			fwrite(bytes, total, 1, f);
			fclose(f);
			fprintf(stderr, "RT_Metal: dumped %ux%u fog (slot %d) frame %d to %s | eye %.1f %.1f %.1f fwd %.3f %.3f %.3f\n",
					(unsigned)w, (unsigned)h, s_lastShown, frame, name,
					c->origin[0], c->origin[1], c->origin[2], c->forward[0], c->forward[1], c->forward[2]);
		}
		free(bytes);
		// and the scatter-centroid depth beside it ("<outpath>.fogd", R16F, same
		// rows) when the pair exists -- the history reprojection's own input
		if (s_fogdepth[s_lastShown]) {
			id<MTLTexture> dtex = s_fogdepth[s_lastShown];
			size_t dtotal = (size_t)w * 2 * h;
			unsigned char *dbytes = (unsigned char *)malloc(dtotal);
			if (dbytes) {
				[dtex getBytes:dbytes bytesPerRow:w * 2 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
				snprintf(name, sizeof name, "%s.fogd", outpath);
				f = fopen(name, "wb");
				if (f) { int32_t hdr[2] = { (int32_t)w, (int32_t)h }; fwrite(hdr, sizeof hdr, 1, f); fwrite(dbytes, dtotal, 1, f); fclose(f); }
				free(dbytes);
			}
		}
	}
}

void RT_Metal_DumpFrame(int width, int height, int counting)
{
	char namebuf[1024];
	int frame = 0;
	const char *outpath = rt_dump_decide(width, height, counting, namebuf, sizeof namebuf, &frame);
	if (!outpath) return;

	unsigned char *px = (unsigned char *)malloc((size_t)width * height * 4);
	if (!px) return;
	GLint prevRead = 0;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glReadBuffer(GL_BACK);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, px);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevRead);

	rt_dump_write(outpath, width, height, frame, px);
	rt_dump_term(outpath, frame);
	rt_dump_fog(outpath, frame);
	free(px);
}

// METAL.md Phase 8-1a: the Metal renderpath's arm of the same instrument. The
// GL arm above reads the window backbuffer; there is no readable drawable on
// this path (framebufferOnly, and the present has not happened yet at
// VID_Finish time), so this reads fbo 0's colour — the backend screen
// texture, scene + HUD, pre-present — through the by-name readback rather
// than Metal_Backend_ReadPixels, whose source follows whatever render target
// was bound last. Two stated limits: this arm cannot see the present pass
// (VID_METAL_PROBE covers that), and under an EDR frame the readback clamps
// to 8-bit — identically for both ends of any A/B, so still discriminating
// (the caller prints a one-shot note; vid.edr_active is not visible from
// this file, deliberately).
void RT_Metal_DumpFrameMetal(int width, int height, int counting)
{
	char namebuf[1024];
	int frame = 0;
	size_t i, n;
	const char *outpath = rt_dump_decide(width, height, counting, namebuf, sizeof namebuf, &frame);
	if (!outpath) return;

	unsigned char *px = (unsigned char *)malloc((size_t)width * height * 4);
	if (!px) return;
	if (!Metal_Backend_ReadScreenBGRA(width, height, px)) { free(px); return; }
	// the readback contract is BGRA (GL_ReadPixelsBGRA's); the dump format is
	// the GL arm's RGBA — swap in place so identical pixels produce identical
	// files across the two arms
	n = (size_t)width * height;
	for (i = 0; i < n; i++) { unsigned char t = px[i*4+0]; px[i*4+0] = px[i*4+2]; px[i*4+2] = t; }
	rt_dump_write(outpath, width, height, frame, px);
	rt_dump_term(outpath, frame);
	rt_dump_fog(outpath, frame);
	free(px);
}

// The camera basis of the most recently SHOWN trace, pre-divided for
// reprojection (right/tanx, up/tany). The murk consumes the fog/shaft textures
// from that same slot, so it reprojects through this basis exactly like the
// composite does. Returns 0 when nothing valid is shown.
// RT_METAL_REPROJ_TEST, read once. 0 = off, 1 = the 5-4 perturbation (2 degrees
// of YAW plus an 8-unit step), 2 = that plus 2 degrees of PITCH.
//
// MODE 2 EXISTS BECAUSE MODE 1 CANNOT SEE A V ERROR. A pure yaw moves the
// horizontal coordinate and leaves the vertical one alone, so a reprojected
// fetch that had its v sense wrong -- flipped, or normalised against the wrong
// axis -- would remap correctly under mode 1 and be reported as parity. That is
// this tree's own recorded lesson about symmetric fixtures (the 6-1 cube probe
// that could not see a width/height transposition), aimed at the one axis a
// two-degree yaw leaves symmetric. Mode 1 is unchanged so slice 5-4's recorded
// numbers still reproduce.
static int rt_reproj_test_mode(void)
{
	static int rptest = -1;
	if (rptest < 0) { const char *e = getenv("RT_METAL_REPROJ_TEST"); rptest = e ? atoi(e) : 0; }
	return rptest;
}

int RT_Metal_GetShownCamera(float forward[3], float rightovertanx[3], float upovertany[3])
{
	if (s_lastShown < 0 || !s_slotCam[s_lastShown].valid)
		return 0;
	// Under rt_metal_sameframe the shown camera IS this frame's camera, so the
	// murk's remap is an identity -- returning 0 sends its one consumer
	// (R_Volumetric_SetReprojUniforms) down the documented disabled path, the
	// direct fetch, which is exactly right. The REPROJ_TEST exception keeps
	// the perturbation instrument live; see rt_reproject_live.
	if (s_sameFrame && !rt_reproj_test_mode())
		return 0;
	{
		const rt_slotcam_t *c = &s_slotCam[s_lastShown];
		float f[3], r[3], u[3];
		int mode;
		if (c->tanx <= 0.0f || c->tany <= 0.0f)
			return 0;
		for (int i = 0; i < 3; i++) { f[i] = c->forward[i]; r[i] = c->right[i]; u[i] = c->up[i]; }
		// THE MURK'S REMAP IS AN IDENTITY ON A STATIC CAMERA, so the frozen
		// parity bed cannot see it at all -- enabling rt_metal_reproject there
		// changes nothing on either backend, and a parity number taken under it
		// describes two shaders agreeing about doing nothing. That is the 4d
		// no-op-bed trap pointed straight at the one part of 6-3b that is
		// arithmetic rather than transcription, so the murk's consumer gets the
		// same treatment 5-4 gave the composite's: a deterministic pretend
		// camera, small enough to stay inside the guards rather than be
		// rejected by them, under which the two backends can be shown to
		// compute the SAME remap and not merely to compute something.
		//
		// The rotations are applied to the UNIT basis and the tan division
		// follows, so the perturbed camera is a real one rather than a sheared
		// approximation of one.
		mode = rt_reproj_test_mode();
		if (mode)
		{
			const float cy = 0.99939083f, sy = 0.03489950f;   // cos/sin 2 degrees
			float f0 = f[0], f1 = f[1], r0 = r[0], r1 = r[1], u0 = u[0], u1 = u[1];
			f[0] = f0 * cy - f1 * sy; f[1] = f0 * sy + f1 * cy;   // yaw about world z
			r[0] = r0 * cy - r1 * sy; r[1] = r0 * sy + r1 * cy;
			u[0] = u0 * cy - u1 * sy; u[1] = u0 * sy + u1 * cy;
			if (mode >= 2)
			{
				// pitch about the (already yawed) right axis: forward and up
				// rotate in their own plane, right is the axis and is unmoved
				for (int i = 0; i < 3; i++) {
					float fi = f[i], ui = u[i];
					f[i] = fi * cy + ui * sy;
					u[i] = ui * cy - fi * sy;
				}
			}
		}
		for (int i = 0; i < 3; i++) {
			forward[i] = f[i];
			rightovertanx[i] = r[i] / c->tanx;
			upovertany[i] = u[i] / c->tany;
		}
	}
	return 1;
}

// The whole reprojection block, so the two composites upload the same NUMBERS.
// The GL bridge below reads it and so does the Metal path's shader setup; only
// the shader EXPRESSION is written twice, which is unavoidable across two
// languages and is marked LOCKSTEP at both sites (kCompFS here,
// MODE_RTCOMPOSITE in shader_msl.h).
//
// cur* is this frame's camera with right/up pre-scaled by its tangents; sh* is
// the camera the shown term was traced with, pre-DIVIDED by its. The tangent
// guard disables the remap rather than leaving a stale uniform behind, which is
// what the GL uploader used to do -- unreachable either way, since a real
// camera's tangents are always positive, but "disabled" is the answer that
// cannot be silently wrong.
int RT_Metal_GetReprojection(rt_reproj_t *out)
{
	const rt_slotcam_t *c;

	if (!out || s_lastShown < 0)
		return 0;
	c = &s_slotCam[s_lastShown];
	// rt_reproject_live, not s_reproject: under rt_metal_sameframe the remap is
	// an identity and this returning 0 is what makes it a PROVABLE no-op -- the
	// disabled path is bit-for-bit the pre-reprojection composite (see the
	// header note above), where a live identity would differ in IEEE.
	if (!rt_reproject_live() || !c->valid || c->tanx <= 0.0f || c->tany <= 0.0f)
		return 0;

	for (int i = 0; i < 3; i++) {
		out->curF[i] = s_cam.forward[i];
		out->curR[i] = s_cam.right[i] * s_cam.tanx;
		out->curU[i] = s_cam.up[i] * s_cam.tany;
		out->shF[i]  = c->forward[i];
		out->shR[i]  = c->right[i] / c->tanx;
		out->shU[i]  = c->up[i] / c->tany;
		out->curO[i] = s_cam.origin[i];
		out->shO[i]  = c->origin[i];
	}
	out->params[0] = 1.0f;
	out->params[1] = (float)s_w;
	out->params[2] = (float)s_h;
	out->params[3] = s_reprojDepth ? 1.0f : 0.0f;

	// RT_METAL_REPROJ_TEST=1: pretend the shown frame was traced from somewhere
	// else, so the remap has real work to do.
	//
	// It exists because a STATIC camera makes reprojection an identity -- the
	// current and shown bases are the same, so the arithmetic round-trips to the
	// coordinate it started from and the feature is invisible on every frozen
	// bed. Measured: enabling rt_metal_reproject on the e1m3 vantage changes 0
	// of 307200 px on BOTH backends. A parity number taken there would be
	// describing two shaders agreeing about doing nothing, which is exactly the
	// no-op bed the 4d HDR shoulder round warns about.
	//
	// A fixed 2-degree yaw plus an 8-unit sideways step, applied to the SHOWN
	// basis only: the yaw drives the rotation-only remap and the step drives the
	// translation refine, and 2 degrees is small enough to stay inside the
	// refine's 8%-of-buffer disocclusion guard rather than being rejected by it.
	// Deterministic, so both backends can be compared against each other under
	// it -- which is the real proof, being that they compute the SAME remap and
	// not merely that each computes something.
	{
		int rptest = rt_reproj_test_mode();
		if (rptest)
		{
			const float c = 0.99939083f, s = 0.03489950f;   // cos/sin 2 degrees
			float f0 = out->shF[0], f1 = out->shF[1];
			float r0 = out->shR[0], r1 = out->shR[1];
			float u0 = out->shU[0], u1 = out->shU[1];
			out->shF[0] = f0 * c - f1 * s; out->shF[1] = f0 * s + f1 * c;
			out->shR[0] = r0 * c - r1 * s; out->shR[1] = r0 * s + r1 * c;
			out->shU[0] = u0 * c - u1 * s; out->shU[1] = u0 * s + u1 * c;
			out->shO[0] += 8.0f;
			// mode 2 adds pitch, which mode 1 leaves entirely untouched -- see
			// rt_reproj_test_mode. Applied here too so the composite and the
			// murk are perturbed by the SAME camera under either mode; mode 1
			// is byte-unchanged, so 5-4's recorded numbers still reproduce.
			if (rptest >= 2)
			{
				int i;
				for (i = 0; i < 3; i++) {
					float fi = out->shF[i], ui = out->shU[i];
					out->shF[i] = fi * c + ui * s;
					out->shU[i] = ui * c - fi * s;
				}
			}
		}
	}
	return 1;
}

// Rig hardening for dump A/Bs: the pre-demo composite count varies run to run,
// which seeds the jitter-rotation phase and the temporal chains differently and
// read as nondeterminism in timedemo dumps. Called at timedemo start so every
// run's temporal state begins from the same point. Gameplay never calls this.
void RT_Metal_ResetTemporal(void)
{
	s_frameCounter = s_frameOffset;
	s_hasPrev = 0;
	s_fogHasPrev = 0;
	s_shHasPrev = 0;
	s_fogValid[0] = s_fogValid[1] = 0;
	s_shValid[0] = s_shValid[1] = 0;
	s_froxValid[0] = s_froxValid[1] = 0;   // A1 FROXEL: the volumes restart with the other chains
	s_slotCam[0].valid = s_slotCam[1].valid = 0;
}

void RT_Metal_RequestDump(void)
{
	s_dumpNow = 1;   // captured on the next RT_Metal_DumpFrame (VID_Finish)
}

// ENCODE the per-frame entity BLAS (from s_evbuf[p]/s_eibuf[p]) and 2-instance
// TLAS builds (instance 0 = static world BLAS, instance 1 = entity BLAS, both
// identity/world space) into the caller's command buffer — the trace encoded
// after them in the SAME buffer sees a fully-built TLAS (in-buffer ordering).
// Grows the per-parity AS/scratch objects only when they need to be bigger.
static bool rt_build_dynamic_as(id<MTLCommandBuffer> cb, int p)
{
	if (!s_dev || !s_accel) return false;
	if (!s_evbuf[p] || !s_eibuf[p] || s_numentitytris < 1) return false;
	if (!s_lcvbuf[p] || !s_lcibuf[p] || s_numlctris < 1) return false;
	if (!s_lavaAccel) return false;   // SetLavaSurfaces runs right after SetWorld every frame (degenerate when no lava)
	if (!s_skyAccel) return false;    // ditto SetSkySurfaces (degenerate when no sky / rt_metal_skyopen 0)
	if (!s_bliqAccel) return false;   // ditto SetBlendedLiquidSurfaces (SEPTEMBER2 C2; degenerate when no blended liquids / rt_metal_liquids_rt 0)
	if (!s_liqAccel) return false;    // ditto SetLiquidSurfaces (degenerate when no opaque liquids / rt_metal_liquidemissive 0)

	// --- entity BLAS descriptor ---
	MTLAccelerationStructureTriangleGeometryDescriptor *tri = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
	tri.vertexBuffer = s_evbuf[p]; tri.vertexStride = sizeof(float) * 3; tri.vertexFormat = MTLAttributeFormatFloat3;
	tri.indexBuffer = s_eibuf[p];  tri.indexType = MTLIndexTypeUInt32;   tri.triangleCount = (NSUInteger)s_numentitytris;
	MTLPrimitiveAccelerationStructureDescriptor *pd = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
	pd.geometryDescriptors = @[tri];
	// REFIT (rt_metal_refit): the usage is part of the DESCRIPTOR, so mode 0
	// must not set it -- that is what keeps the off-switch byte-exact. An AS
	// built without it can never be refitted (s_entityRefittable).
	if (s_refitMode)
		pd.usage = MTLAccelerationStructureUsageRefit;
	MTLAccelerationStructureSizes esz = [s_dev accelerationStructureSizesWithDescriptor:pd];
	if (!s_entityAccel[p] || esz.accelerationStructureSize > s_entityAccelCap[p]) {
		s_entityAccel[p] = [s_dev newAccelerationStructureWithSize:esz.accelerationStructureSize];
		s_entityAccelCap[p] = esz.accelerationStructureSize;
		s_entityRefittable[p] = 0;   // a fresh object holds no build to refit against
		s_entityAccelHash[p] = 0;
	}
	{
		NSUInteger escr = esz.buildScratchBufferSize > esz.refitScratchBufferSize ? esz.buildScratchBufferSize : esz.refitScratchBufferSize;
		if (!s_escratch[p] || escr > s_escratchCap[p]) {
			s_escratch[p] = [s_dev newBufferWithLength:escr options:MTLResourceStorageModePrivate];
			s_escratchCap[p] = escr;
		}
	}
	qbool refitE = (s_refitMode && s_entityRefittable[p]
	                && s_entityAccelTris[p] == (NSUInteger)s_numentitytris
	                && s_refitAgeE[p] < RT_REFIT_REBUILD_EVERY) ? true : false;

	// --- light-core BLAS descriptor (flame models; own scratch, see statics) ---
	MTLAccelerationStructureTriangleGeometryDescriptor *ltri = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
	ltri.vertexBuffer = s_lcvbuf[p]; ltri.vertexStride = sizeof(float) * 3; ltri.vertexFormat = MTLAttributeFormatFloat3;
	ltri.indexBuffer = s_lcibuf[p];  ltri.indexType = MTLIndexTypeUInt32;   ltri.triangleCount = (NSUInteger)s_numlctris;
	MTLPrimitiveAccelerationStructureDescriptor *lpd = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
	lpd.geometryDescriptors = @[ltri];
	if (s_refitMode)
		lpd.usage = MTLAccelerationStructureUsageRefit;
	MTLAccelerationStructureSizes lsz = [s_dev accelerationStructureSizesWithDescriptor:lpd];
	if (!s_lcAccel[p] || lsz.accelerationStructureSize > s_lcAccelCap[p]) {
		s_lcAccel[p] = [s_dev newAccelerationStructureWithSize:lsz.accelerationStructureSize];
		s_lcAccelCap[p] = lsz.accelerationStructureSize;
		s_lcRefittable[p] = 0;
		s_lcAccelHash[p] = 0;
	}
	{
		NSUInteger lscr = lsz.buildScratchBufferSize > lsz.refitScratchBufferSize ? lsz.buildScratchBufferSize : lsz.refitScratchBufferSize;
		if (!s_lcscratch[p] || lscr > s_lcscratchCap[p]) {
			s_lcscratch[p] = [s_dev newBufferWithLength:lscr options:MTLResourceStorageModePrivate];
			s_lcscratchCap[p] = lscr;
		}
	}
	qbool refitL = (s_refitMode && s_lcRefittable[p]
	                && s_lcAccelTris[p] == (NSUInteger)s_numlctris
	                && s_refitAgeL[p] < RT_REFIT_REBUILD_EVERY) ? true : false;

	// --- TLAS descriptor (6 instances: world, entities, light-cores, lava, sky, liquids) ---
	if (!s_instbuf[p] || s_instbuf[p].length < 7 * sizeof(MTLAccelerationStructureInstanceDescriptor))   // SEPTEMBER2 C2: seven instances (the blended liquids are the seventh)
		s_instbuf[p] = [s_dev newBufferWithLength:7 * sizeof(MTLAccelerationStructureInstanceDescriptor) options:MTLResourceStorageModeShared];
	MTLAccelerationStructureInstanceDescriptor *inst = (MTLAccelerationStructureInstanceDescriptor *)s_instbuf[p].contents;
	MTLPackedFloat4x3 I;               // column-major; columns 0-2 = basis, column 3 = translation
	I.columns[0] = MTLPackedFloat3Make(1, 0, 0);
	I.columns[1] = MTLPackedFloat3Make(0, 1, 0);
	I.columns[2] = MTLPackedFloat3Make(0, 0, 1);
	I.columns[3] = MTLPackedFloat3Make(0, 0, 0);
	// LOCKSTEP: these instance masks pair with the mask literals at the kernel
	// intersect sites (primary 0x3Fu sees everything: world, entities, cores,
	// lava, sky, liquids; every shadow ray 0x3u sees world + entities only --
	// cores, lava, sky and liquids are emitters or openings, never occluders).
	// Change one side, change the other.
	inst[0].transformationMatrix = I;
	inst[0].options = MTLAccelerationStructureInstanceOptionDisableTriangleCulling;  // matches cull_mode::none
	inst[0].mask = 0x01;                      // world
	inst[0].intersectionFunctionTableOffset = 0;
	inst[0].accelerationStructureIndex = 0;   // -> instancedAccelerationStructures[0] = world
	inst[1] = inst[0];
	inst[1].accelerationStructureIndex = 1;   // -> instancedAccelerationStructures[1] = entity
	inst[1].mask = 0x02;                      // shadow-casting entities
	inst[2] = inst[0];
	inst[2].accelerationStructureIndex = 2;   // -> instancedAccelerationStructures[2] = light-cores
	inst[2].mask = 0x04;                      // emissive flames: shadow rays skip these
	inst[3] = inst[0];
	inst[3].accelerationStructureIndex = 3;   // -> instancedAccelerationStructures[3] = lava sheets
	inst[3].mask = 0x08;                      // emissive lava: shadow rays skip it too
	inst[4] = inst[0];
	inst[4].accelerationStructureIndex = 4;   // -> instancedAccelerationStructures[4] = sky brushes
	inst[4].mask = 0x10;                      // OPEN SKY: primary rays see it (and treat the hit as a miss); shadow rays skip it
	inst[5] = inst[0];
	inst[5].accelerationStructureIndex = 5;   // -> instancedAccelerationStructures[5] = opaque liquids
	inst[5].mask = 0x20;                      // EMISSIVE liquids: primary rays stop and shade term 1.0; shadow rays pass through
	inst[6] = inst[0];
	inst[6].accelerationStructureIndex = 6;   // -> instancedAccelerationStructures[6] = BLENDED liquids (SEPTEMBER2 C2)
	inst[6].mask = 0x40;                      // C2 ONLY: never in the primary mask (the floor keeps its term), never a shadow occluder; the C2 intersect alone sees it
	MTLInstanceAccelerationStructureDescriptor *td = [MTLInstanceAccelerationStructureDescriptor descriptor];
	td.instanceDescriptorBuffer = s_instbuf[p];
	td.instanceDescriptorBufferOffset = 0;
	td.instanceDescriptorStride = sizeof(MTLAccelerationStructureInstanceDescriptor);
	td.instanceCount = 7;
	td.instancedAccelerationStructures = @[s_accel, s_entityAccel[p], s_lcAccel[p], s_lavaAccel, s_skyAccel, s_liqAccel, s_bliqAccel];   // order == accelerationStructureIndex
	td.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeDefault;
	MTLAccelerationStructureSizes tsz = [s_dev accelerationStructureSizesWithDescriptor:td];
	if (!s_tlas[p] || tsz.accelerationStructureSize > s_tlasCap[p]) {
		s_tlas[p] = [s_dev newAccelerationStructureWithSize:tsz.accelerationStructureSize];
		s_tlasCap[p] = tsz.accelerationStructureSize;
		s_tlasBuilt[p] = 0;
	}
	if (!s_tscratch[p] || tsz.buildScratchBufferSize > s_tscratchCap[p]) {
		s_tscratch[p] = [s_dev newBufferWithLength:tsz.buildScratchBufferSize options:MTLResourceStorageModePrivate];
		s_tscratchCap[p] = tsz.buildScratchBufferSize;
	}

	// Encode entity BLAS build, then TLAS build, in separate encoders (the BLAS
	// must finish before the TLAS reads it — in-buffer encoder order guarantees
	// it). No commit here: the caller commits once with the trace included.
	double t0 = rt_now_ms();
	// A3: skip what has not changed. A slot's AS is a function of the bytes it
	// was built from; equal hash and count means the identical BVH already sits
	// there (built two frames ago, completed at that slot's own wait). The TLAS
	// is skipped only when both BLASes are AND it was built against these same
	// six objects -- a replaced world, lava, sky or liquid structure (a map
	// change, a cvar rebuild) or a reallocated BLAS is a different TLAS.
	qbool skipE = (s_asSkip && s_evHash[p] && s_entityAccelHash[p] == s_evHash[p] && s_entityAccelTris[p] == (NSUInteger)s_numentitytris) ? true : false;
	qbool skipL = (s_asSkip && s_lcHash[p] && s_lcAccelHash[p] == s_lcHash[p] && s_lcAccelTris[p] == (NSUInteger)s_numlctris) ? true : false;
	{
		const void *now6[7] = { (__bridge const void *)s_accel, (__bridge const void *)s_entityAccel[p], (__bridge const void *)s_lcAccel[p], (__bridge const void *)s_lavaAccel, (__bridge const void *)s_skyAccel, (__bridge const void *)s_liqAccel, (__bridge const void *)s_bliqAccel };
		if (skipE && skipL && s_tlasBuilt[p] && memcmp(now6, s_tlasStatic[p], sizeof(now6)) == 0) {
			s_skipCount++;
			s_asCpuMs = rt_now_ms() - t0;
			s_asGpuMs = 0.0;
			return true;   // nothing to encode: the slot's TLAS is this frame's exactly
		}
		memcpy(s_tlasStatic[p], now6, sizeof(now6));
	}
	id<MTLAccelerationStructureCommandEncoder> ae1 = (skipE && skipL) ? nil : [cb accelerationStructureCommandEncoder];
	// REFIT in place when the slot's AS was built with the usage, against the
	// SAME triangle count, and within the quality cadence; else a real build.
	// In-place safety is the existing design's: this slot's vertex buffer is
	// already rewritten and its AS rebuilt into the same object every second
	// frame, so the refit inherits exactly the rotation's fencing.
	if (skipE) {
		// unchanged: the slot's BLAS stands
	} else if (refitE) {
		[ae1 refitAccelerationStructure:s_entityAccel[p] descriptor:pd destination:s_entityAccel[p] scratchBuffer:s_escratch[p] scratchBufferOffset:0];
		s_refitAgeE[p]++;
		s_refitCount++;
		s_entityAccelHash[p] = s_evHash[p];
	} else {
		[ae1 buildAccelerationStructure:s_entityAccel[p] descriptor:pd scratchBuffer:s_escratch[p] scratchBufferOffset:0];
		s_entityRefittable[p] = s_refitMode;
		s_entityAccelTris[p] = (NSUInteger)s_numentitytris;
		s_entityAccelHash[p] = s_evHash[p];
		s_refitAgeE[p] = 0;
		s_buildCount++;
	}
	if (skipL) {
		// unchanged: the slot's light-core BLAS stands
	} else if (refitL) {
		[ae1 refitAccelerationStructure:s_lcAccel[p] descriptor:lpd destination:s_lcAccel[p] scratchBuffer:s_lcscratch[p] scratchBufferOffset:0];
		s_refitAgeL[p]++;
		s_lcAccelHash[p] = s_lcHash[p];
	} else {
		[ae1 buildAccelerationStructure:s_lcAccel[p] descriptor:lpd scratchBuffer:s_lcscratch[p] scratchBufferOffset:0];
		s_lcRefittable[p] = s_refitMode;
		s_lcAccelTris[p] = (NSUInteger)s_numlctris;
		s_lcAccelHash[p] = s_lcHash[p];
		s_refitAgeL[p] = 0;
	}
	if (ae1) [ae1 endEncoding];
	// the liveness line, change-only per the console-ink rule
	{
		static int refitReported;
		if (refitE && !refitReported) {
			refitReported = 1;
			fprintf(stderr, "RT refit active (dynamic BLAS refit in place, rebuild every %d or on topology change)\n", RT_REFIT_REBUILD_EVERY);
		}
	}
	id<MTLAccelerationStructureCommandEncoder> ae2 = [cb accelerationStructureCommandEncoder];
	[ae2 buildAccelerationStructure:s_tlas[p] descriptor:td scratchBuffer:s_tscratch[p] scratchBufferOffset:0];
	[ae2 endEncoding];
	s_tlasBuilt[p] = 1;
	s_asCpuMs = rt_now_ms() - t0;   // pure encode cost now (build runs inside the frame cb)
	s_asGpuMs = 0.0;                // no longer separable from the combined cb GPU time
	return true;
}

// Lazily (re)create the fog kernel's 3D textures from the CPU copies. A FRESH
// texture per change, never a mutation: encoded command buffers retain their
// resources, so an in-flight frame keeps reading the old texture safely and no
// wait is needed.
static void rt_ensure_fog_textures(void)
{
	if (!s_dev)
		return;
	if (s_noiseDirty && s_noiseCopy) {
		MTLTextureDescriptor *td = [MTLTextureDescriptor new];
		td.textureType = MTLTextureType3D;
		td.pixelFormat = MTLPixelFormatRGBA8Unorm;
		td.width = s_noiseSize; td.height = s_noiseSize; td.depth = s_noiseSize;
		td.usage = MTLTextureUsageShaderRead;
		td.storageMode = MTLStorageModeShared;
		id<MTLTexture> t = [s_dev newTextureWithDescriptor:td];
		if (t) {
			[t replaceRegion:MTLRegionMake3D(0, 0, 0, s_noiseSize, s_noiseSize, s_noiseSize)
				 mipmapLevel:0 slice:0 withBytes:s_noiseCopy
				 bytesPerRow:(NSUInteger)s_noiseSize * 4
			   bytesPerImage:(NSUInteger)s_noiseSize * s_noiseSize * 4];
			s_noiseTex3D = t;
			s_noiseDirty = 0;
			fprintf(stderr, "RT_Metal: fog noise %d^3 uploaded\n", s_noiseSize);
		}
	}
	if (s_fieldDirty && s_fieldCopy) {
		MTLTextureDescriptor *td = [MTLTextureDescriptor new];
		td.textureType = MTLTextureType3D;
		td.pixelFormat = MTLPixelFormatRGBA8Unorm;
		td.width = s_fieldSize[0]; td.height = s_fieldSize[1]; td.depth = s_fieldSize[2];
		td.usage = MTLTextureUsageShaderRead;
		td.storageMode = MTLStorageModeShared;
		id<MTLTexture> t = [s_dev newTextureWithDescriptor:td];
		if (t) {
			[t replaceRegion:MTLRegionMake3D(0, 0, 0, s_fieldSize[0], s_fieldSize[1], s_fieldSize[2])
				 mipmapLevel:0 slice:0 withBytes:s_fieldCopy
				 bytesPerRow:(NSUInteger)s_fieldSize[0] * 4
			   bytesPerImage:(NSUInteger)s_fieldSize[0] * s_fieldSize[1] * 4];
			s_fieldTex3D = t;
			s_fieldDirty = 0;
			fprintf(stderr, "RT_Metal: fog field %dx%dx%d uploaded\n", s_fieldSize[0], s_fieldSize[1], s_fieldSize[2]);
		}
	}
	if (s_irrDirty && s_irrCopy) {
		MTLTextureDescriptor *td = [MTLTextureDescriptor new];
		td.textureType = MTLTextureType3D;
		td.pixelFormat = MTLPixelFormatRGBA8Unorm;
		td.width = s_irrSize[0]; td.height = s_irrSize[1]; td.depth = s_irrSize[2];
		td.usage = MTLTextureUsageShaderRead;
		td.storageMode = MTLStorageModeShared;
		id<MTLTexture> t = [s_dev newTextureWithDescriptor:td];
		if (t) {
			[t replaceRegion:MTLRegionMake3D(0, 0, 0, s_irrSize[0], s_irrSize[1], s_irrSize[2])
				 mipmapLevel:0 slice:0 withBytes:s_irrCopy
				 bytesPerRow:(NSUInteger)s_irrSize[0] * 4
			   bytesPerImage:(NSUInteger)s_irrSize[0] * s_irrSize[1] * 4];
			s_irrTex3D = t;
			s_irrDirty = 0;
			fprintf(stderr, "RT_Metal: fog irradiance %dx%dx%d uploaded\n", s_irrSize[0], s_irrSize[1], s_irrSize[2]);
		}
	}
}

// Resolution-aware profiling, reported every 120 frames. ASYNC-era
// semantics. (This line's format used to be pinned by test/rt-suite.py's
// parser; that harness was retired 2026-08-18 and NOTHING parses this line
// now -- test/perf/perf-sweep.sh reads the KERNELMS line below instead, so
// THAT is the one whose format is load-bearing.)
//   as cpu    = CPU time to ENCODE the BLAS/TLAS builds (they now run
//               inside the frame command buffer); as gpu = 0 (no longer
//               separable)
//   trace cpu = the late-stall wait on the SHOWN slot (async: ~0 when
//               the GPU kept up; sync mode: the full trace wait)
//   trace gpu = GPU busy time of the shown slot's whole command buffer
//               (AS builds + trace)
//   total     = wall time of the sidecar's part of RT_Metal_Composite —
//               what the RT adds to the CPU frame. On GL that includes the
//               bridge composite draw (the call sits after it, unchanged);
//               on the shared-device path the draw belongs to the caller,
//               so total there is the sidecar's own cost.
//
// ONE function, called from both arms of the seam (METAL.md Phase 8-1c):
// this block used to live only in the GL bridge tail, so the Metal
// renderpath — the shipped default — had no RT profile line at all, and
// the same-frame cost measurement had no instrument. Statics live here,
// so whichever arm runs feeds the same 120-frame window.
static void rt_profile_accum(int width, int height, double traceCpuMs, double traceGpuMs, double tCpu0)
{
	static int pframe = 0;
	static double aAs = 0.0, aAsG = 0.0, aTr = 0.0, aTrG = 0.0, aTot = 0.0;
	static int naccum = 0;
	// Verification runs only (RT_METAL_PROFILE=1, or KERNELMS which needs the
	// report, or VERBOSE): its two siblings were always gated and this one was
	// not -- see s_profile's declaration comment.
	if (!(s_profile || s_kernelMs || s_verbose))
		return;
	aAs += s_asCpuMs; aAsG += s_asGpuMs;
	aTr += traceCpuMs; aTrG += traceGpuMs;
	aTot += rt_now_ms() - tCpu0;
	naccum++;
	if (++pframe >= 120) {
		double n = (double)naccum;
		fprintf(stderr, "RT_Metal: %dx%d avg of %d: as %.2f ms cpu (%.2f gpu) | trace %.2f ms cpu (%.2f gpu) | total %.2f ms cpu | %.2f Mpix, %d lights, %d ent tris | budget@120Hz=8.33ms\n",
				width, height, naccum, aAs / n, aAsG / n, aTr / n, aTrG / n, aTot / n,
				(double)width * height / 1.0e6, s_numlights, s_numentitytris);
		// KERNELMS: the separable per-stage picture. Its own prefix, and
		// test/perf/perf-sweep.sh PARSES it (`trace X fog Y shaft Z ms`) --
		// this line's format is the load-bearing one now, not the one above.
		if (s_kernelMs && s_kernN > 0) {
			double kn = (double)s_kernN;
			fprintf(stderr, "RT_Metal-kern: avg of %d: trace %.2f fog %.2f shaft %.2f ms gpu | trace %dx%d\n",
					s_kernN, s_kernAccum[0] / kn, s_kernAccum[1] / kn, s_kernAccum[2] / kn, s_w, s_h);
			// WARCHEST session 2: the refit share, its own prefix (the kern and
			// profile line formats are parsed and sacred). Window-reset counters.
			// ASSPLIT: its own prefix. The RT_Metal-kern line above is PARSED by
			// perf-sweep.sh and stays exactly as it was; in split mode its "trace"
			// figure is the dispatch alone, which is why this line prints both.
			if (s_asSplit)
				fprintf(stderr, "RT_Metal-asplit: as %.3f trace-only %.3f ms gpu (as is %.0f%% of the pair)\n",
						s_kernAccum[3] / kn, s_kernAccum[0] / kn,
						100.0 * s_kernAccum[3] / (s_kernAccum[3] + s_kernAccum[0] + 1e-9));
			if (s_refitCount || s_buildCount || s_skipCount) {
				fprintf(stderr, "RT_Metal-refit: %u refit / %u rebuild / %u skipped this window\n", s_refitCount, s_buildCount, s_skipCount);
				s_refitCount = 0; s_buildCount = 0; s_skipCount = 0;
			}
			s_kernAccum[0] = s_kernAccum[1] = s_kernAccum[2] = s_kernAccum[3] = 0.0; s_kernN = 0;
		}
		pframe = 0; naccum = 0;
		aAs = aAsG = aTr = aTrG = aTot = 0.0;
	}
}

int RT_Metal_Composite(int width, int height, int scenedepthvalid, int vpx, int vpy, unsigned int depthtexname, const float *screentodepth, float uptol)
{
	s_active = 0;   // cleared up-front; set to 1 only on a fully-successful composite below,
	                // so RT_Metal_Active() reports whether the RT is REALLY relighting the frame
	                // (the wall-lighting fullbright forcing keys on it — never fullbright without RT).
	                // On the shared-device path the setter is RT_Metal_MarkComposited, called by
	                // the caller after ITS draw; see the seam.
	if (!s_dev || !s_pso || !s_queue) return RT_COMPOSITE_NONE;
	if (width < 1 || height < 1) return RT_COMPOSITE_NONE;
	if (!s_accel || !s_hasCam) return RT_COMPOSITE_NONE;   // nothing to trace yet -> leave the frame untouched

	// The GL path tracks its context so a vid_restart can invalidate the stale
	// GL names (rt_abandon_gl). There is no CGL context on the shared-device
	// path and nothing GL-shaped to invalidate; the equivalent device-identity
	// check belongs with the bridge narrowing in slice 5-6. `cur` stays in the
	// function's scope because the composite tail re-validates the shown
	// surfaces through it -- that tail is GL-only and unreachable on the shared
	// path, but it must still compile.
	CGLContextObj cur = NULL;
	if (!s_sharedDevice)
	{
		cur = CGLGetCurrentContext();
		if (!cur) return RT_COMPOSITE_NONE;
		if (cur != s_glctx) { rt_abandon_gl(); s_glctx = cur; }
	}

	@autoreleasepool {
		double tCpu0 = rt_now_ms();       // wall clock for the WHOLE sidecar frame (profile report)
		double traceGpuMs = 0.0, traceCpuMs = 0.0;

		// trace resolution: rt_metal_scale of the viewport. Derived BEFORE the
		// command buffer exists so a scale change resizes outside any encode
		// (the fog/shaft ensures run mid-encode and drop a frame on resize).
		int tw = (int)(width  * s_scale + 0.5f); if (tw < 1) tw = 1; if (tw > width)  tw = width;
		int th = (int)(height * s_scale + 0.5f); if (th < 1) th = 1; if (th > height) th = height;
		if (!rt_ensure_surface(tw, th, tw == width && th == height))
			return RT_COMPOSITE_NONE;

		int curslot = s_par, prevslot = s_par ^ 1;

		// 1. ENCODE this frame's GPU work (entity BLAS + TLAS builds, then the
		//    trace) into ONE command buffer and commit it WITHOUT waiting: the
		//    GPU traces this frame while the engine gets on with GL work. The
		//    composite below shows the PREVIOUS frame's finished trace (async
		//    mode) or waits for this one (RT_METAL_SYNC=1 verification mode).
		//    See metal/async-plan.md.
		s_cam.w = (uint32_t)tw;
		s_cam.h = (uint32_t)th;
		if (!s_lightbuf[curslot]) RT_Metal_SetLights(NULL, 0, 0);   // ensure a bindable buffer exists
		s_cam.numLights = (uint32_t)s_numlights;
		s_cam.numDynamic = (uint32_t)s_numdynamic;

		// STILLNESS DENOISE: with the camera parked, the per-frame jitter phase
		// keeps rotating and the fog/shaft EMAs (default 0.5) only average ~2
		// frames of it -- a faint stipple that motion normally hides. When the
		// camera has not moved since last frame AND no dynamic light is live (a
		// muzzle flash while aiming must never smear), the fog/shaft blends are
		// floored at 0.75 (~4-frame average). Surface shadows keep their own
		// cvar untouched. Epsilons: 0.25 units of translation and ~0.06 degrees
		// of rotation count as parked; Quake's idle view is exactly static.
		int rtstill = s_hasPrev && s_numdynamic == 0
			&& fabsf(s_cam.origin[0] - s_prevOrigin[0]) < 0.25f
			&& fabsf(s_cam.origin[1] - s_prevOrigin[1]) < 0.25f
			&& fabsf(s_cam.origin[2] - s_prevOrigin[2]) < 0.25f
			&& (s_cam.forward[0] * s_prevForward[0] + s_cam.forward[1] * s_prevForward[1] + s_cam.forward[2] * s_prevForward[2]) > 0.9999995f
			&& s_cam.tanx == s_prevTanx && s_cam.tany == s_prevTany;
		// The GI EMA takes the stillness floor exactly as the fog/shaft blends
		// do (history 0 stays 0: the frozen beds' determinism gate).
		s_cam.gihistory = (rtstill && s_giHistoryRaw > 0.0f) ? fmaxf(s_giHistoryRaw, 0.75f) : s_giHistoryRaw;
		// RT_METAL_FRAMEPIN holds the jitter phase so a frozen bed is reproducible;
		// the counter still advances, so nothing else that might read it drifts.
		s_frameCounter = (s_frameCounter + 1) & 1023u;
		s_cam.frame = s_framePin ? 0u : s_frameCounter;

		// feed the previous-frame camera basis for temporal reprojection
		memcpy(s_cam.pOrigin,  s_prevOrigin,  sizeof(float) * 3);
		memcpy(s_cam.pForward, s_prevForward, sizeof(float) * 3);
		memcpy(s_cam.pRight,   s_prevRight,   sizeof(float) * 3);
		memcpy(s_cam.pUp,      s_prevUp,      sizeof(float) * 3);
		s_cam.pTanx = s_prevTanx; s_cam.pTany = s_prevTany;
		s_cam.hasPrev = s_hasPrev ? 1u : 0u;

		// Encode the dynamic entity BLAS + the world/entity TLAS for this frame. If
		// the caller set no casters this frame, a degenerate placeholder keeps the
		// TLAS valid. Must succeed before we can trace.
		if (!s_evbuf[curslot] || s_numentitytris < 1) RT_Metal_SetEntities(NULL, 0, NULL, 0, NULL);
		if (!s_lcvbuf[curslot] || s_numlctris < 1) RT_Metal_SetLightCores(NULL, 0, NULL, 0);
		s_cam.entnorms = s_entHasNorms[curslot] ? 1u : 0u;
		id<MTLCommandBuffer> cb = [s_queue commandBuffer];
		if (!rt_build_dynamic_as(cb, curslot))
			return RT_COMPOSITE_NONE;

		// RT_METAL_ASSPLIT: close the AS work into its own command buffer and open
		// a fresh one for the trace dispatch. Same queue, so execution order is
		// preserved, and the trace's dependency on the TLAS is by tracked resource
		// either way -- this only moves where the buffer boundary falls.
		if (s_asSplit) {
			[cb commit];
			s_kcb[curslot][3] = cb;
			cb = [s_queue commandBuffer];
		}

		// SEPTEMBER2 C2: the liquid pair exists only once the arm has been on; texture(7)
		// is declared in every build, so a stand-in (the term itself, never written
		// while the arm is off) keeps it bound -- the declared-but-unbound trap.
		if (s_liqrt && !s_liqmtex[0])
			rt_ensure_liquid_surface(s_w, s_h);
		id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
		[ce setComputePipelineState:s_pso];
		[ce setTexture:s_mtex[curslot] atIndex:0];
		[ce setTexture:s_histTex[s_histParity] atIndex:1];          // read previous-frame history
		[ce setTexture:s_histTex[s_histParity ^ 1] atIndex:2];      // write this-frame history
		[ce setTexture:s_secTex[s_histParity] atIndex:3];           // read the previous secondary estimate
		[ce setTexture:s_secTex[s_histParity ^ 1] atIndex:4];       // write this frame's
		[ce setTexture:s_giTex[s_histParity] atIndex:5];            // read the previous GI colour EMA (rt_metal_gi)
		[ce setTexture:s_giTex[s_histParity ^ 1] atIndex:6];        // write this frame's
		[ce setTexture:(s_liqmtex[curslot] ? s_liqmtex[curslot] : s_mtex[curslot]) atIndex:7];   // SEPTEMBER2 C2: the liquid pair (this slot), or the stand-in
		[ce setAccelerationStructure:s_tlas[curslot] atBufferIndex:0];   // trace the TLAS (world + entities)
		[ce useResource:s_accel usage:MTLResourceUsageRead];             // child BLASes are referenced by the
		[ce useResource:s_entityAccel[curslot] usage:MTLResourceUsageRead];   // TLAS indirectly -> must be made resident
		[ce useResource:s_lcAccel[curslot] usage:MTLResourceUsageRead];
		[ce useResource:s_lavaAccel usage:MTLResourceUsageRead];   // was missing since the lava arc (unified memory forgave it)
		[ce useResource:s_skyAccel usage:MTLResourceUsageRead];
		[ce useResource:s_liqAccel usage:MTLResourceUsageRead];
		[ce useResource:s_bliqAccel usage:MTLResourceUsageRead];   // SEPTEMBER2 C2
		[ce setBuffer:s_vbuf offset:0 atIndex:1];
		[ce setBuffer:s_ibuf offset:0 atIndex:2];
		[ce setBytes:&s_cam length:sizeof(RTCam) atIndex:3];
		[ce setBuffer:s_lightbuf[curslot] offset:0 atIndex:4];
		[ce setBuffer:s_evbuf[curslot] offset:0 atIndex:5];
		[ce setBuffer:s_eibuf[curslot] offset:0 atIndex:6];
		[ce setBuffer:s_enbuf[curslot] offset:0 atIndex:7];
		[ce setBuffer:s_bnbuf offset:0 atIndex:8];   // blue-noise table; ignored by RT_BLUENOISE 0 PSOs
		[ce setBuffer:s_wabuf offset:0 atIndex:9];   // per-tri world albedo (G4-2); SetWorld guarantees non-nil whenever s_accel exists
		[ce setBuffer:s_bliqibuf offset:0 atIndex:10];   // SEPTEMBER2 C2: the blended liquids' indices (degenerate when none; never nil once SetBlendedLiquidSurfaces has run)
		[ce dispatchThreads:MTLSizeMake(tw, th, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
		[ce endEncoding];

		// remember the camera this slot was traced with (the composite that shows
		// it next frame reprojects the term through it)
		memcpy(s_slotCam[curslot].origin,  s_cam.origin,  sizeof(float) * 3);
		memcpy(s_slotCam[curslot].forward, s_cam.forward, sizeof(float) * 3);
		memcpy(s_slotCam[curslot].right,   s_cam.right,   sizeof(float) * 3);
		memcpy(s_slotCam[curslot].up,      s_cam.up,      sizeof(float) * 3);
		s_slotCam[curslot].tanx = s_cam.tanx;
		s_slotCam[curslot].tany = s_cam.tany;
		s_slotCam[curslot].valid = 1;

		// KERNELMS: the AS builds + trace get their own command buffer; fog and
		// shaft each get one below. Committed in stage order = queue order.
		if (s_kernelMs) {
			[cb commit];
			s_kcb[curslot][0] = cb;
			s_pendingCB[curslot] = cb;   // provisionally the frame's last CB; fog/shaft update it
			cb = nil;
		}

		// GOD RAYS: a second, smaller compute pass in the SAME command buffer. It
		// reads the history texture rt_trace just wrote (encoder order is the
		// ordering guarantee) and the previous frame's shaft output for temporal
		// blending, and lands in the shaft surface for THIS slot — so the shown-slot
		// wait below covers it and the murk can consume the shown frame safely.
		{
			// FULL IN-KERNEL FOG: when wanted, it owns the beams too -- the shaft
			// kernel is skipped and its slot invalidated, so the murk never consumes
			// a stale beam buffer alongside the fog output.
			int fogReady = 0;
			// BOTH KERNELS NOW ENCODE ON EITHER PATH (METAL.md Phase 6-3b).
			// They were skipped on the shared device for two slices because
			// their only consumer -- MODE_VOLUMETRICFOG -- sentinelled magenta
			// there, so the work had no possible reader; 6-3a then adopted
			// their outputs into the renderer's texture table and 6-3b writes
			// the two MSL arms, which is what makes this line safe to drop.
			//
			// Nothing else in either encode path needed changing, and that is
			// worth stating because it is the reason the two halves could be
			// split: rt_pair_ensure has had a shared-device arm since 5-2 that
			// creates the MTLTexture alone (no IOSurface, no GL name), and the
			// fog and shaft surfaces go through it exactly as the term buffer
			// already did.
			//
			// The shaft condition below still keys on !fogWanted -- kernel fog
			// owns the beams and supersedes the screen-space tier -- and it
			// carried its own !s_sharedDevice term rather than inheriting this
			// one, so it needed the same edit. Folding them would have un-gated
			// the shafts silently.
			int fogWanted = (s_fogEnable && s_fogPso);
			if (fogWanted) {
				rt_ensure_fog_textures();
				if (s_noiseTex3D && s_hasFogShade) {
					int fw = (int)(width  * s_fogScale + 0.5f); if (fw < 1) fw = 1;
					int fh = (int)(height * s_fogScale + 0.5f); if (fh < 1) fh = 1;
					if (rt_ensure_fog_surface(fw, fh)) {
						RTFogCam fc;
						memcpy(fc.origin,  s_cam.origin,  sizeof(float) * 3);
						memcpy(fc.forward, s_cam.forward, sizeof(float) * 3);
						memcpy(fc.right,   s_cam.right,   sizeof(float) * 3);
						memcpy(fc.up,      s_cam.up,      sizeof(float) * 3);
						fc.tanx = s_cam.tanx; fc.tany = s_cam.tany;
						fc.w = (uint32_t)fw; fc.h = (uint32_t)fh;
						fc.fullw = (uint32_t)tw; fc.fullh = (uint32_t)th;   // history texture is trace-sized
						fc.frame = s_cam.frame;
						fc.numLights = s_cam.numLights;
						fc.steps = (uint32_t)s_fogSteps;
						fc.stride = (uint32_t)s_fogStride;
						fc.dist = s_fogShade.dist;
						// STILLNESS DENOISE, deepened for the stochastic pick (2026-08-29,
						// the demo17 speckle): with rt_metal_lightsample >= 1 each cast
						// samples ONE light, and a parked view staring into thick fog
						// integrates that variance with only the user's history (~3-4
						// frames at Seb's 0.7) -- the blotchy flicker demo17 f2600-2609
						// shows. Parked with no dynamic lights, the floor rises to 0.9
						// (~10-frame average; measured on that bed 0.80 -> 0.37 mean
						// flicker, changed pixels 2.4% -> 0.55%). Mode 0 keeps the
						// original 0.75 floor -- its promise is the old behaviour.
						// The 1/p clamp was measured on the same bed and is NOT the fix
						// (bulk pick variance, not the tail): see test/flicker.py.
						// BLUENOISE slice 2: the ceiling is 0.9 with the clamp off (the value
						// SetFogTuning always produced) and 0.95 with it on; the parked floor
						// under the pick deepens the same way.
						{
							float fogcap = s_fogClamp ? 0.95f : 0.9f;
							float fogh = fminf(s_fogHistory, fogcap);
							fc.history = (rtstill && fogh > 0.0f) ? fmaxf(fogh, s_lightSample ? fogcap : 0.75f) : fogh;   // stillness denoise
						}
						// A1 FROXEL: decided here, before the clamp and the hasPrev fills below,
						// because it supersedes the whole 2D history path (rt_fog's own EMA and
						// the clamp pass both skipped). The volumes need the depth pair (the
						// march end rides it) and both PSOs; any of them missing = the 2D fog.
						int froxel = 0;
						if (s_froxel) {
							rt_ensure_froxel_pso();
							if (s_froxelPso && s_froxelIntPso && s_fogdepth[0] && s_fogdepth[1] && rt_ensure_froxel_volumes(fw, fh, s_froxelSlices)) froxel = 1;
						} else if (s_froxA[0])
							rt_release_froxel_volumes();
						if (froxel) fc.history = s_froxelHistory;   // > 0 so the jitter and the pick advance per frame (the fields' other reader, the 2D EMA, is compiled out)
						fc.intensity = s_fogIntensity;
						fc.residual = s_fogResidual;
						fc.beams = s_fogBeams;
						fc.hasPrev = (s_fogHasPrev && s_fogValid[curslot ^ 1]) ? 1u : 0u;
						memcpy(fc.pForward, s_cam.pForward, sizeof(float) * 3);
						memcpy(fc.pRight,   s_cam.pRight,   sizeof(float) * 3);
						memcpy(fc.pUp,      s_cam.pUp,      sizeof(float) * 3);
						fc.pTanx = s_cam.pTanx; fc.pTany = s_cam.pTany;
						fc.hasPrevCam = (s_reproject && s_cam.hasPrev) ? 1u : 0u;
						memcpy(fc.windoffset, s_fogShade.windoffset, sizeof(float) * 3);
						memcpy(fc.color,      s_fogShade.color,      sizeof(float) * 3);
						memcpy(fc.watercolor, s_fogShade.watercolor, sizeof(float) * 3);
						memcpy(fc.slimecolor, s_fogShade.slimecolor, sizeof(float) * 3);
						memcpy(fc.lavacolor,  s_fogShade.lavacolor,  sizeof(float) * 3);
						memcpy(fc.fieldorigin,  s_fogShade.fieldorigin,  sizeof(float) * 3);
						memcpy(fc.fieldinvsize, s_fogShade.fieldinvsize, sizeof(float) * 3);
						fc.density = s_fogShade.density;
						fc.height = s_fogShade.height;
						fc.basez = s_fogShade.basez;
						fc.noisescale = s_fogShade.noisescale;
						fc.noisethresh = s_fogShade.noisethresh;
						fc.waterdensity = s_fogShade.waterdensity;
						fc.slimedensity = s_fogShade.slimedensity;
						fc.lavadensity  = s_fogShade.lavadensity;
						// mirror the GL "field && cvar" gate: without the baked field
						// the floor/liquid keying must fall back to camera-relative air
						fc.watermode = s_fieldTex3D ? s_fogShade.watermode : 0.0f;
						fc.flooroffset = s_fogShade.flooroffset;
						fc.floormode = s_fieldTex3D ? s_fogShade.floormode : 0.0f;
						fc.watermist = s_fieldTex3D ? s_fogShade.watermist : 0.0f;
						fc.mistheight = s_fogShade.mistheight;
						fc.sdfrange = s_fogShade.sdfrange;
						fc.corner = s_fieldTex3D ? s_fogShade.corner : 0.0f;
						fc.fieldmaxh = s_fogShade.fieldmaxh;
						// ground keys off the baked field's floor channel: without a live
						// field the fallback binds NOISE at the field slot, so ground must
						// be dead (same honesty rule as watermist/corner above)
						fc.grounddensity = s_fieldTex3D ? s_fogShade.grounddensity : 0.0f;
						fc.groundheight = s_fogShade.groundheight;
						fc.groundnoisescale = s_fogShade.groundnoisescale;
						fc.groundthresh = s_fogShade.groundthresh;
						fc.grounddeform = s_fogShade.grounddeform;
						fc.groundoffset = s_fogShade.groundoffset;
						memcpy(fc.groundwindoffset, s_fogShade.groundwindoffset, sizeof(float) * 3);
						memcpy(fc.groundcolor, s_fogShade.groundcolor, sizeof(float) * 3);
						fc.lavaglow = s_fogShade.lavaglow;
						fc.skytrans = s_fogShade.skytrans;
						// the ambient grid's honesty gate, same rule as the field's
						// above: without a live grid texture the fallback binds NOISE
						// at its slot, so the strength must be dead
						memcpy(fc.irrorigin, s_fogShade.irrorigin, sizeof(float) * 3);
						memcpy(fc.irrinvsize, s_fogShade.irrinvsize, sizeof(float) * 3);
						fc.irrgain = s_fogShade.irrgain;
						fc.irrfloor = s_fogShade.irrfloor;
						fc.irrstrength = s_irrTex3D ? s_fogShade.irrstrength : 0.0f;
						fc.extinction = s_fogShade.extinction > 0.0f ? s_fogShade.extinction : 1.0f;
						// KH SWIRL: plain copies -- the KH boost's floormode gate is
						// in-kernel via cam.floormode, which is field-gated above
						fc.swirlamp = s_fogShade.swirlamp;
						fc.swirlscale = s_fogShade.swirlscale;
						fc.swirlkh = s_fogShade.swirlkh;
						fc.swirlspare = 0.0f;
						fc.mistlavacut = s_fogShade.mistlavacut;
						fc.histcentre = s_fogHistCentre ? 1u : 0u;
						fc.lsample = s_lightSample ? 1u : 0u;   // mode >= 1: fog-only and full both pick here
						fc.adstride = s_fogAdStride ? 1u : 0u;
						fc.stepjitter = s_fogStepJitter ? 1u : 0u;
						fc.wclamp = s_lsClamp;
						fc.lshybrid = (uint32_t)s_lsHybrid;
						fc.froxel = (uint32_t)froxel;
						fc.nslices = (uint32_t)s_froxelSlices;
						// THE CURVE IS A WORLD-UNIT REQUIREMENT, NOT A SLICE COUNT (measured 2026-09-06):
						// at 24 slices over his 1200 units the first cell was 11 units deep and the
						// converged froxel read +1.4% BRIGHTER than the truth (cells coarser than the
						// density model's variation bias the single-point extinction estimate); at 48
						// slices (5.7-unit near cells) the bias vanished (-0.01%). So the exponent is
						// derived per frame from the first cell's depth in UNITS (rt_metal_fog_froxel_near)
						// and the live dist, by bisection on dist*(e^(a/N)-1)/(e^a-1) = near (monotone
						// decreasing in a); an explicit rt_metal_fog_froxel_curve > 0 overrides.
						if (s_froxelCurve > 0.0f) fc.fcurve = s_froxelCurve;
						else {
							float N = (float)s_froxelSlices, D = fc.dist, want = s_froxelNear, lo = 0.05f, hi = 8.0f;
							if (D / N <= want) fc.fcurve = 0.0f;   // uniform cells are already fine enough
							else {
								for (int it = 0; it < 40; ++it) {
									float a = 0.5f * (lo + hi);
									float first = D * (expf(a / N) - 1.0f) / (expf(a) - 1.0f);
									if (first > want) lo = a; else hi = a;
								}
								fc.fcurve = 0.5f * (lo + hi);
							}
						}
						fc.fhistory = s_froxelHistory;
						memcpy(fc.pOrigin, s_cam.pOrigin, sizeof(float) * 3);
						fc.fhasPrev = (froxel && s_froxValid[curslot ^ 1] && fc.hasPrevCam) ? 1u : 0u;
						fc.fcastphase = s_froxelCastPhase ? 1u : 0u;
						fc.liquidfloor = s_fogShade.liquidfloor;
						fc.liquidlight = s_fogLiquidLight;   // BEAUTY B2
						id<MTLCommandBuffer> fcb = s_kernelMs ? [s_queue commandBuffer] : cb;
						id<MTLComputeCommandEncoder> fe = [fcb computeCommandEncoder];
						[fe setComputePipelineState:(froxel ? s_froxelPso : s_fogPso)];
						// S1: with the filter on, the fog kernel writes its RAW output to the
						// scratch texture and the filter below writes the real surface. Off,
						// it binds the surface directly, exactly as before -- byte for byte.
						int fogfilt = (s_fogFilter && s_fogFilterPso && s_fograw[0] && s_fograw[1]) ? s_fogFilter : 0;
						// BLUENOISE slice 2: with the clamp on, rt_fog writes RAW and skips its
						// own EMA (hasPrev 0 -- the kernel text is untouched); the history
						// pass below accumulates. The real hasPrev is kept for that pass.
						int fogclamp = (s_fogClamp && s_fogTemporalPso && s_fograw[0] && s_fograw[1] && s_fogema[0] && s_fogema[1] && s_fogdepth[0] && s_fogdepth[1]) ? s_fogClamp : 0;
						if (froxel) fogclamp = 0;   // the froxel volume IS the history; the 2D clamp pass does not run under it
						uint32_t fogHasPrevReal = fc.hasPrev;
						if (fogclamp || froxel) fc.hasPrev = 0u;
						[fe setTexture:((fogfilt || fogclamp) ? s_fograw[curslot] : s_fogmtex[curslot]) atIndex:0];
						[fe setTexture:s_histTex[s_histParity ^ 1] atIndex:1];
						[fe setTexture:s_fogmtex[curslot ^ 1] atIndex:2];
						[fe setTexture:s_noiseTex3D atIndex:3];
						[fe setTexture:(s_fieldTex3D ? s_fieldTex3D : s_noiseTex3D) atIndex:4];
						[fe setTexture:(s_irrTex3D ? s_irrTex3D : s_noiseTex3D) atIndex:5];
						[fe setTexture:(s_fogdepth[0] ? s_fogdepth[curslot] : s_fogmtex[curslot ^ 1]) atIndex:6];   // scatter depth out; a bound stand-in if the alloc failed (the declared-but-unbound abort)
						[fe setAccelerationStructure:s_tlas[curslot] atBufferIndex:0];
						[fe useResource:s_accel usage:MTLResourceUsageRead];
						[fe useResource:s_entityAccel[curslot] usage:MTLResourceUsageRead];
						[fe useResource:s_lcAccel[curslot] usage:MTLResourceUsageRead];
						[fe useResource:s_lavaAccel usage:MTLResourceUsageRead];
						[fe useResource:s_skyAccel usage:MTLResourceUsageRead];
						[fe useResource:s_liqAccel usage:MTLResourceUsageRead];
						[fe useResource:s_bliqAccel usage:MTLResourceUsageRead];
						[fe setBytes:&fc length:sizeof(fc) atIndex:1];
						[fe setBuffer:s_lightbuf[curslot] offset:0 atIndex:2];
						[fe setBuffer:s_bnbuf offset:0 atIndex:3];   // blue-noise table; ignored by RT_BLUENOISE 0 PSOs
						if (froxel) {
							[fe setTexture:s_froxA[curslot ^ 1] atIndex:7];
							[fe setTexture:s_froxB[curslot ^ 1] atIndex:8];
							[fe setTexture:s_froxA[curslot] atIndex:9];
							[fe setTexture:s_froxB[curslot] atIndex:10];
							[fe setTexture:s_fogdepth[curslot ^ 1] atIndex:11];
						}
						[fe dispatchThreads:MTLSizeMake(fw, fh, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
						[fe endEncoding];
						if (froxel) {
							// A1 FROXEL: sum the accumulated cells into the surface the old kernel
							// would have written (raw scratch when the display filter is on, which
							// then runs on it exactly as before). Same command buffer: under
							// KERNELMS it is inside the fog stage, honestly.
							RTFroxelCam ic;
							memset(&ic, 0, sizeof(ic));
							ic.w = (uint32_t)fw; ic.h = (uint32_t)fh; ic.n = (uint32_t)s_froxelSlices;
							ic.dist = fc.dist; ic.curve = fc.fcurve; ic.extinction = fc.extinction;
							ic.beams = fc.beams; ic.lavaglow = fc.lavaglow; ic.skytrans = fc.skytrans;
							memcpy(ic.lavacolor, fc.lavacolor, sizeof(float) * 3);
							id<MTLComputeCommandEncoder> ie = [fcb computeCommandEncoder];
							[ie setComputePipelineState:s_froxelIntPso];
							[ie setTexture:(fogfilt ? s_fograw[curslot] : s_fogmtex[curslot]) atIndex:0];
							[ie setTexture:s_froxA[curslot] atIndex:1];
							[ie setTexture:s_froxB[curslot] atIndex:2];
							[ie setTexture:s_fogdepth[curslot] atIndex:3];
							[ie setBytes:&ic length:sizeof(ic) atIndex:0];
							[ie dispatchThreads:MTLSizeMake(fw, fh, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
							[ie endEncoding];
							s_froxValid[curslot] = 1;
						} else
							s_froxValid[curslot] = 0;
						if (fogclamp) {
							// BLUENOISE slice 2: raw -> clamped, reprojected EMA. The history
							// read is the OTHER slot's EMA (one encode frame old, the surfaces'
							// own parity), valid only once that slot has been written under
							// the clamp -- a toggle or resize starts from the raw frame.
							struct { float forward[3], right[3], up[3]; float tanx, tany; uint32_t w, h; float history; uint32_t hasPrev, hasPrevCam;
							         float pForward[3], pRight[3], pUp[3]; float pTanx, pTany; uint32_t mode; float k; uint32_t tonemap;
							         float origin[3], pOrigin[3]; uint32_t reprojdepth; float depthtol; } tc;
							_Static_assert(sizeof(tc) == 152, "FogTemporalCam must match the MSL struct: 38 tightly packed 4-byte fields");
							memset(&tc, 0, sizeof(tc));
							memcpy(tc.forward, fc.forward, sizeof(float) * 3);
							memcpy(tc.right,   fc.right,   sizeof(float) * 3);
							memcpy(tc.up,      fc.up,      sizeof(float) * 3);
							tc.tanx = fc.tanx; tc.tany = fc.tany;
							tc.w = fc.w; tc.h = fc.h;
							tc.history = fc.history;
							tc.hasPrev = (fogHasPrevReal && s_fogEmaValid[curslot ^ 1]) ? 1u : 0u;
							tc.hasPrevCam = fc.hasPrevCam;
							memcpy(tc.pForward, fc.pForward, sizeof(float) * 3);
							memcpy(tc.pRight,   fc.pRight,   sizeof(float) * 3);
							memcpy(tc.pUp,      fc.pUp,      sizeof(float) * 3);
							tc.pTanx = fc.pTanx; tc.pTany = fc.pTany;
							tc.mode = (uint32_t)fogclamp;
							tc.k = s_fogClampK;
							tc.tonemap = s_fogTonemapEma ? 1u : 0u;
							memcpy(tc.origin,  fc.origin,     sizeof(float) * 3);
							memcpy(tc.pOrigin, s_cam.pOrigin, sizeof(float) * 3);
							tc.reprojdepth = (uint32_t)s_fogReprojDepth;
							tc.depthtol = s_fogReprojTol;
							id<MTLComputeCommandEncoder> te = [fcb computeCommandEncoder];
							[te setComputePipelineState:s_fogTemporalPso];
							[te setTexture:s_fogema[curslot] atIndex:0];
							[te setTexture:s_fograw[curslot] atIndex:1];
							[te setTexture:s_fogema[curslot ^ 1] atIndex:2];
							[te setTexture:s_fogdepth[curslot] atIndex:3];
							[te setTexture:s_fogdepth[curslot ^ 1] atIndex:4];
							[te setBytes:&tc length:sizeof(tc) atIndex:0];
							[te dispatchThreads:MTLSizeMake(fw, fh, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
							[te endEncoding];
							s_fogEmaValid[curslot] = 1;
							if (!fogfilt) {
								// no display filter: the EMA IS the surface (same format and size,
								// so a blit is the whole copy)
								id<MTLBlitCommandEncoder> be = [fcb blitCommandEncoder];
								[be copyFromTexture:s_fogema[curslot] toTexture:s_fogmtex[curslot]];
								[be endEncoding];
							}
						} else
							s_fogEmaValid[curslot] = 0;
						if (fogfilt) {
							// S1: filter raw -> surface. Same command buffer, so under KERNELMS it
							// folds into the fog stage (which test/perf/perf-sweep.sh parses --
							// the stage number honestly includes it). The EMA in rt_fog read
							// prevFog = LAST frame's filtered surface, so the history feeds back
							// the filtered frame (SVGF's choice).
							// LAYOUT IS THE MSL's FiltCam, byte for byte (2026-08-19): float3
							// is 16-byte aligned AND 16 bytes wide in MSL, so `ro` sits at 32-48
							// and `mode` at 48, and the struct rounds up to 64. The first cut
							// declared mode at 44 in a 48-byte block: the kernel read mode from
							// PAST the bind -- undefined, in practice 0 -- so the 5x5 (mode 2)
							// and the mode-3 cost A/B never reached it and "5x5 reads identical
							// to 3x3" was identical because the 5x5 never ran. Under Metal API
							// validation the short bind aborts the process at the dispatch
							// ("argument has a length(64)"); the all-live validation boot is
							// what caught it. _Static_assert pins the size.
							struct { uint32_t w, h, fullw, fullh, histcentre; float depthtol, farclip, pad0; float ro[3]; float pad1; uint32_t mode; uint32_t dilate; uint32_t pad2[2]; } fcam;
							_Static_assert(sizeof(fcam) == 64, "FiltCam must match the MSL struct (float3 ro at 32, mode at 48, 64 bytes)");
							memset(&fcam, 0, sizeof(fcam));
							fcam.w = (uint32_t)fw; fcam.h = (uint32_t)fh;
							fcam.fullw = fc.fullw; fcam.fullh = fc.fullh;
							fcam.histcentre = fc.histcentre;
							fcam.depthtol = s_fogFilterDepth;
							fcam.farclip = fc.dist;
							fcam.ro[0] = fc.origin[0]; fcam.ro[1] = fc.origin[1]; fcam.ro[2] = fc.origin[2];
							fcam.mode = (uint32_t)fogfilt;
							fcam.dilate = 1u;
							id<MTLTexture> filtin = fogclamp ? s_fogema[curslot] : s_fograw[curslot];   // slice 2: filter the EMA once for display
							// MODE 4 (2026-09-03 evening, the "fizz"): the fog's per-frame grain is a
							// coherent worm texture three to four texels across -- the blue-noise
							// table's own spacing -- which a single 5x5 cannot average. A second
							// 5x5 pass with its taps two texels apart (a-trous) gives a 13x13
							// footprint for the price of one more pass; the depth and
							// transmittance affinities still hold every silhouette. The
							// intermediate rides the OTHER slot's raw scratch, free by now.
							if (fogfilt == 4 && s_fograw[curslot ^ 1]) {
								fcam.mode = 2u; fcam.dilate = 1u;
								id<MTLComputeCommandEncoder> xe = [fcb computeCommandEncoder];
								[xe setComputePipelineState:s_fogFilterPso];
								[xe setTexture:s_fograw[curslot ^ 1] atIndex:0];
								[xe setTexture:filtin atIndex:1];
								[xe setTexture:s_histTex[s_histParity ^ 1] atIndex:2];
								[xe setBytes:&fcam length:sizeof(fcam) atIndex:0];
								[xe dispatchThreads:MTLSizeMake(fw, fh, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
								[xe endEncoding];
								filtin = s_fograw[curslot ^ 1];
								fcam.dilate = 2u;
							}
							id<MTLComputeCommandEncoder> xe = [fcb computeCommandEncoder];
							[xe setComputePipelineState:s_fogFilterPso];
							[xe setTexture:s_fogmtex[curslot] atIndex:0];
							[xe setTexture:filtin atIndex:1];
							[xe setTexture:s_histTex[s_histParity ^ 1] atIndex:2];
							[xe setBytes:&fcam length:sizeof(fcam) atIndex:0];
							[xe dispatchThreads:MTLSizeMake(fw, fh, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
							[xe endEncoding];
						}
						if (s_kernelMs) {
							[fcb commit];
							s_kcb[curslot][1] = fcb;
							s_pendingCB[curslot] = fcb;
						}
						s_fogValid[curslot] = 1;
						s_fogHasPrev = 1;
						fogReady = 1;
					}
				}
			}
			if (!fogReady) {
				s_fogValid[curslot] = 0;
				s_fogHasPrev = 0;
			}

			int shaftReady = 0;
			// superseded whenever in-kernel fog is wanted, ready or not: beams belong
			// to the fog integral there, and a half-configured frame must not show both
			if (!fogWanted && s_shaftEnable && s_shaftPso) {
				int sw = (int)(width  * s_shaftScale + 0.5f); if (sw < 1) sw = 1;
				int sh = (int)(height * s_shaftScale + 0.5f); if (sh < 1) sh = 1;
				if (rt_ensure_shaft_surface(sw, sh)) {
					RTShaftCam sc;
					memcpy(sc.origin,  s_cam.origin,  sizeof(float) * 3);
					memcpy(sc.forward, s_cam.forward, sizeof(float) * 3);
					memcpy(sc.right,   s_cam.right,   sizeof(float) * 3);
					memcpy(sc.up,      s_cam.up,      sizeof(float) * 3);
					sc.tanx = s_cam.tanx; sc.tany = s_cam.tany;
					sc.w = (uint32_t)sw; sc.h = (uint32_t)sh;
					sc.fullw = (uint32_t)tw; sc.fullh = (uint32_t)th;   // history texture is trace-sized
					sc.frame = s_cam.frame;
					sc.numLights = s_cam.numLights;
					sc.samples = (uint32_t)s_shaftSamples;
					sc.dist = s_shaftDist;
					// deepened floor under the stochastic pick -- the fog fill's
					// demo17 comment carries the mechanism and the measurement
					sc.history = (rtstill && s_shaftHistory > 0.0f) ? fmaxf(s_shaftHistory, s_lightSample ? 0.9f : 0.75f) : s_shaftHistory;   // stillness denoise
					sc.residual = s_shaftResidual;
					sc.hasPrev = (s_shHasPrev && s_shValid[curslot ^ 1]) ? 1u : 0u;
					memcpy(sc.pForward, s_cam.pForward, sizeof(float) * 3);
					memcpy(sc.pRight,   s_cam.pRight,   sizeof(float) * 3);
					memcpy(sc.pUp,      s_cam.pUp,      sizeof(float) * 3);
					sc.pTanx = s_cam.pTanx; sc.pTany = s_cam.pTany;
					sc.hasPrevCam = (s_reproject && s_cam.hasPrev) ? 1u : 0u;
					sc.lsample = s_lightSample ? 1u : 0u;   // mode >= 1: fog-only and full both pick here
					sc.wclamp = s_lsClamp;
					id<MTLCommandBuffer> hcb = s_kernelMs ? [s_queue commandBuffer] : cb;
					id<MTLComputeCommandEncoder> se = [hcb computeCommandEncoder];
					[se setComputePipelineState:s_shaftPso];
					[se setTexture:s_shmtex[curslot] atIndex:0];
					[se setTexture:s_histTex[s_histParity ^ 1] atIndex:1];   // rt_trace's output this frame
					[se setTexture:s_shmtex[curslot ^ 1] atIndex:2];         // previous shaft frame
					[se setAccelerationStructure:s_tlas[curslot] atBufferIndex:0];
					[se useResource:s_accel usage:MTLResourceUsageRead];
					[se useResource:s_entityAccel[curslot] usage:MTLResourceUsageRead];
					[se useResource:s_lcAccel[curslot] usage:MTLResourceUsageRead];
					[se useResource:s_lavaAccel usage:MTLResourceUsageRead];
					[se useResource:s_skyAccel usage:MTLResourceUsageRead];
					[se useResource:s_liqAccel usage:MTLResourceUsageRead];
					[se useResource:s_bliqAccel usage:MTLResourceUsageRead];
					[se setBytes:&sc length:sizeof(sc) atIndex:1];
					[se setBuffer:s_lightbuf[curslot] offset:0 atIndex:2];
					[se setBuffer:s_bnbuf offset:0 atIndex:3];   // blue-noise table; ignored by RT_BLUENOISE 0 PSOs
					[se dispatchThreads:MTLSizeMake(sw, sh, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
					[se endEncoding];
					if (s_kernelMs) {
						[hcb commit];
						s_kcb[curslot][2] = hcb;
						s_pendingCB[curslot] = hcb;
					}
					s_shValid[curslot] = 1;
					s_shHasPrev = 1;
					shaftReady = 1;
				}
			}
			if (!shaftReady) {
				s_shValid[curslot] = 0;   // this slot's shaft data is stale; never hand it out
				s_shHasPrev = 0;
			}
		}

		if (cb) {                    // KERNELMS mode committed per stage above, cb is nil there
			[cb commit];             // NO wait: the GPU works while the frame finishes on the CPU
			s_pendingCB[curslot] = cb;
		}

		// snapshot this frame's camera + swap the history ping-pong, so the next
		// frame's ENCODE reprojects into the history this trace writes (the
		// history chain is ordered by the command queue, not by the CPU).
		memcpy(s_prevOrigin,  s_cam.origin,  sizeof(float) * 3);
		memcpy(s_prevForward, s_cam.forward, sizeof(float) * 3);
		memcpy(s_prevRight,   s_cam.right,   sizeof(float) * 3);
		memcpy(s_prevUp,      s_cam.up,      sizeof(float) * 3);
		s_prevTanx = s_cam.tanx; s_prevTany = s_cam.tany;
		s_hasPrev = 1;
		s_histParity ^= 1;
		s_par ^= 1;                  // next frame encodes (and SetEntities fills) the other slot

		// Pick the slot to SHOW: previous frame's trace (async, the default) or
		// the one just committed (same-frame: the rt_metal_sameframe cvar, or
		// RT_METAL_SYNC=1, its env-var ancestor kept for A/Bs). In async, first
		// frames after init have nothing finished to show -> skip the composite
		// once (loading plaque); in same-frame, frame 1 shows its own trace.
		int samefr = (s_syncMode || s_sameFrame);
		int show = samefr ? curslot : prevslot;
		id<MTLCommandBuffer> scb = s_pendingCB[show];
		// SAME-FRAME FALLBACK (Phase 8-2). On an ordinary same-frame frame the
		// current slot's CB was committed forty lines up, so scb is never nil
		// here -- the nil case is a mid-encode fog/shaft resize that drained
		// everything (reachable under KERNELMS), where the old code would have
		// proceeded on the current slot with no wait, safe only through an
		// invariant nothing asserted. Fall back to RE-SHOWING the previous slot
		// instead: its only writer finished at its own composite's wait, and
		// rt_release_surface zeroes s_slotCam[].valid on a size change, so the
		// re-show branch below self-disables exactly where the surfaces went
		// away. This keeps the anti-white-frame mitigation async's re-show
		// provides -- in wall-lighting mode a skipped composite is one blinding
		// albedo-fullbright frame.
		if (samefr && !scb)
		{
			show = prevslot;
			scb = s_pendingCB[show];   // nil in same-frame steady state -> the re-show branch below
		}
		if (!scb) {
			// Nothing in flight for that slot: a mid-frame resize (a fog/shaft
			// scale change) just waited everything out, or this is the first
			// frame. If the slot holds a previously completed trace, RE-SHOW it
			// rather than skip: in wall-lighting mode a skipped composite leaves
			// the scene albedo-fullbright for one blinding white frame. Never
			// touch the OTHER slot here -- it was just re-encoded and the GPU is
			// writing it.
			// Tests the MTLTexture, which is the one third of the triple that
			// exists on BOTH paths (slice 5-2). Equivalent to the old
			// `!s_surf[show] || !s_gltex[show]` on GL by the pair invariant:
			// rt_pair_ensure creates all three thirds together and fails as a
			// unit, and rt_pair_release clears all three together, so the
			// three are never independently present. Getting this wrong is not
			// subtle -- a skipped composite in wall-lighting mode leaves the
			// scene albedo-fullbright for one blinding white frame, in Seb's
			// configuration only and on none of the 23 parity vantages.
			if (!s_slotCam[show].valid || !s_mtex[show])
				return RT_COMPOSITE_NONE;
		} else {
		{
			double tw = rt_now_ms();
			if (scb.status != MTLCommandBufferStatusCompleted)
				[scb waitUntilCompleted];   // async: normally a no-op (one frame old); sync: the full trace
			traceCpuMs = rt_now_ms() - tw;
			traceGpuMs = (scb.GPUEndTime - scb.GPUStartTime) * 1000.0;
		}
		if (scb.error) {
			// Rate-limited: a PERSISTENT GPU fault would otherwise print every
			// frame -- and each error also drops s_lastShown, which flips the
			// murk's fallback state line, a per-frame line PAIR at 60 fps (the
			// console-volume incident's worst latent case).
			static double s_lastErrMs;
			double nowms = rt_now_ms();
			if (nowms - s_lastErrMs > 1000.0)
			{
				s_lastErrMs = nowms;
				fprintf(stderr, "RT_Metal: trace error: %s\n", scb.error.localizedDescription.UTF8String);
			}
			s_pendingCB[show] = nil;
			s_lastShown = -1;   // never hand consumers a slot in an unknown state
			return RT_COMPOSITE_NONE;
		}
		s_pendingCB[show] = nil;     // consumed: its surface is now safe to composite AND to re-encode later
		if (s_kernelMs) {
			// harvest per-stage GPU times for the shown slot. The wait above was on
			// its LAST stage CB; resource dependencies mean the earlier stages
			// finished before it, so these extra waits are belt-and-braces no-ops.
			// (traceGpuMs above covers only that last stage in this mode.)
			for (int k = 0; k < 4; k++) {
				id<MTLCommandBuffer> kcb = s_kcb[show][k];
				if (!kcb) continue;
				if (kcb.status != MTLCommandBufferStatusCompleted) [kcb waitUntilCompleted];
				s_kernAccum[k] += (kcb.GPUEndTime - kcb.GPUStartTime) * 1000.0;
				s_kcb[show][k] = nil;
			}
			s_kernN++;
		}
		}                            // end of the fresh-command-buffer path
		s_lastShown = show;          // the murk may now consume this slot's shaft buffer

		// METAL.md Phase 5, THE SEAM. Everything above this line is pure Metal
		// -- surface ensure, AS build, kernel dispatch, slot rotation, the wait
		// and its error handling -- and everything below is the GL bridge: a
		// GLSL composite program, rectangle binds, seventeen glGet readbacks.
		// Every qgl* is NULL on the Metal renderpath, so none of it may run.
		//
		// On the shared-device path the term is PUBLISHED here and drawn by the
		// caller (slice 5-3). The draw is an ordinary backend draw and it belongs
		// in C beside the other screen-quad composites, not in this file: it needs
		// no Metal of its own, only a texture handle, and writing it here would
		// rebuild a private bridge one API down from the one this phase exists to
		// delete. What crosses the seam is therefore a handle and a status.
		//
		// The status split is what keeps the safety invariant honest. s_active
		// stays 0 here -- cl_screen.c forces r_fullbright whenever
		// RT_Metal_Active(), so a path that reported active without drawing would
		// render the world as raw albedo with no compensating multiply, blinding
		// and only in wall-lighting configurations. The caller sets it through
		// RT_Metal_MarkComposited AFTER its draw, so every way of not drawing --
		// including returning early between here and there -- lands on RT
		// inactive, which is the correct fallback rather than a white frame.
		if (s_sharedDevice)
		{
			// The profile line, HERE and not only in the GL tail below (Phase
			// 8-1c): everything it reports — the encode cost, the late-stall
			// wait, the shown slot's GPU time — is already measured by this
			// point, and the composite draw on this path belongs to the caller.
			rt_profile_accum(width, height, traceCpuMs, traceGpuMs, tCpu0);
			// Filtering is sampler state on this path, so the GL tail's
			// glTexParameteri predicate becomes a flag choice here, and it must
			// stay the same predicate: reprojection samples fractionally and
			// wants LINEAR even at full res; with it off, full res keeps the
			// bit-exact NEAREST fetch. LOCKSTEP with the `GLint want =` line in
			// the GL tail below.
			int wantflags = TEXF_CLAMP | ((s_w == width && s_h == height && !rt_reproject_live())
			                              ? TEXF_FORCENEAREST : TEXF_FORCELINEAR);
			if (!rt_adopt_ensure(show, wantflags))
			{
				static int warned;
				if (!warned)
				{
					warned = 1;
					fprintf(stderr, "RT_Metal: could not publish the term buffer to the renderer; "
					                "RT will not relight this path (METAL.md Phase 5 slice 3)\n");
				}
				return RT_COMPOSITE_NONE;
			}
			return RT_COMPOSITE_PUBLISHED;
		}

		// 2. Composite: draw a fullscreen triangle that samples the RT shadow term
		//    and MULTIPLIES it into the currently-bound scene framebuffer (the 3D
		//    view, before the 2D/HUD pass). We are mid-frame inside DP's R_Mesh, so
		//    save & restore every GL state we touch to keep gl_backend's cache valid.
		if (!rt_ensure_composite_program())
			return RT_COMPOSITE_NONE;

		GLint prevProg = 0, prevVAO = 0, prevActive = 0, prevRect = 0;
		GLint bsRGB = GL_ONE, bdRGB = GL_ZERO, bsA = GL_ONE, bdA = GL_ZERO;
		GLint prevDepthFunc = GL_LESS;
		GLboolean prevBlend = glIsEnabled(GL_BLEND);
		GLboolean prevDepth = glIsEnabled(GL_DEPTH_TEST);
		GLboolean prevScissor = glIsEnabled(GL_SCISSOR_TEST);
		// Called mid-3D-render (from R_RenderView, right after the scene render), so
		// unlike the old end-of-frame hook the engine's 3D state is still live: face
		// culling is ON (and DarkPlaces inverts the winding convention, so the
		// fullscreen triangle would be culled away entirely), and polygon-offset fill
		// is ON with whatever bias the last surface batch set (which would shift the
		// quad off the 0.0625 view-model depth and break the weapon mask). Both must
		// be disabled for the composite and restored exactly afterwards.
		GLboolean prevCull = glIsEnabled(GL_CULL_FACE);
		GLboolean prevPolyOff = glIsEnabled(GL_POLYGON_OFFSET_FILL);
		GLboolean prevDepthMask = GL_TRUE;
		GLfloat prevDepthRange[2] = { 0.0f, 1.0f };
		GLboolean prevMask[4];
		glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
		glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);
		glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
		glGetIntegerv(GL_BLEND_SRC_RGB, &bsRGB);   glGetIntegerv(GL_BLEND_DST_RGB, &bdRGB);
		glGetIntegerv(GL_BLEND_SRC_ALPHA, &bsA);   glGetIntegerv(GL_BLEND_DST_ALPHA, &bdA);
		glGetBooleanv(GL_COLOR_WRITEMASK, prevMask);
		glGetIntegerv(GL_DEPTH_FUNC, &prevDepthFunc);
		glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
		glGetFloatv(GL_DEPTH_RANGE, prevDepthRange);

		glActiveTexture(GL_TEXTURE0);
		glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE, &prevRect);
		if (s_shValid[show] && s_shgltex[show]) {
			// re-validate the shaft pair's GL view too, while the rect target is ours
			glBindTexture(GL_TEXTURE_RECTANGLE, s_shgltex[show]);
			CGLTexImageIOSurface2D(cur, GL_TEXTURE_RECTANGLE, GL_RGBA16F, s_shw, s_shh,
								   GL_RGBA, GL_HALF_FLOAT, s_shsurf[show], 0);
		}
		if (s_fogValid[show] && s_foggltex[show]) {
			glBindTexture(GL_TEXTURE_RECTANGLE, s_foggltex[show]);
			CGLTexImageIOSurface2D(cur, GL_TEXTURE_RECTANGLE, GL_RGBA16F, s_fogw, s_fogh,
								   GL_RGBA, GL_HALF_FLOAT, s_fogsurf[show], 0);
		}
		glBindTexture(GL_TEXTURE_RECTANGLE, s_gltex[show]);
		CGLTexImageIOSurface2D(cur, GL_TEXTURE_RECTANGLE, GL_RGBA16F, s_w, s_h,
							   GL_RGBA, GL_HALF_FLOAT, s_surf[show], 0);   // re-validate GL's view of Metal's write (HDR RGBA16F; TRACE-sized)
		{
			// reprojection samples fractionally and wants LINEAR even at full res;
			// with it off, full res keeps the bit-exact NEAREST fetch
			GLint want = (s_w == width && s_h == height && !rt_reproject_live()) ? GL_NEAREST : GL_LINEAR;
			if (want != s_gltexFilterCur) {
				for (int fi = 0; fi < 2; fi++) {
					glBindTexture(GL_TEXTURE_RECTANGLE, s_gltex[fi]);
					glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, want);
					glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, want);
				}
				glBindTexture(GL_TEXTURE_RECTANGLE, s_gltex[show]);
				s_gltexFilterCur = want;
			}
		}

		// TERM UPSAMPLE: the scene depth on unit 1, bound with the same
		// capture-and-restore discipline the rect on unit 0 gets. depthtexname 0
		// (direct path, feature off, renderbuffer depth) leaves it unbound and
		// the shader's rtUp.z gate keeps the sampler unread -- GL tolerates that
		// where Metal would not.
		GLint prevTex2D1 = 0;
		if (depthtexname) {
			glActiveTexture(GL_TEXTURE1);
			glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex2D1);
			glBindTexture(GL_TEXTURE_2D, (GLuint)depthtexname);
			glActiveTexture(GL_TEXTURE0);
		}
		glUseProgram(s_prog);
		if (s_locRtoff >= 0) glUniform2f(s_locRtoff, (GLfloat)vpx, (GLfloat)vpy);
		if (s_locRtscale >= 0) glUniform2f(s_locRtscale, (GLfloat)s_w / (GLfloat)width, (GLfloat)s_h / (GLfloat)height);
		{
			// reprojection basis: current camera rays -> the shown slot's trace pixels
			// The VALUES come from RT_Metal_GetReprojection, which the Metal
			// path's shader setup also reads -- one source, so the two
			// composites cannot drift in arithmetic, only in shader text.
			// A zeroed block with params[0] = 0 is the disabled case, which is
			// what the old `rpen` flag expressed.
			rt_reproj_t rp;
			if (!RT_Metal_GetReprojection(&rp))
				memset(&rp, 0, sizeof(rp));
			if (s_locRp[0] >= 0) glUniform3f(s_locRp[0], rp.curF[0], rp.curF[1], rp.curF[2]);
			if (s_locRp[1] >= 0) glUniform3f(s_locRp[1], rp.curR[0], rp.curR[1], rp.curR[2]);
			if (s_locRp[2] >= 0) glUniform3f(s_locRp[2], rp.curU[0], rp.curU[1], rp.curU[2]);
			if (s_locRp[3] >= 0) glUniform3f(s_locRp[3], rp.shF[0], rp.shF[1], rp.shF[2]);
			if (s_locRp[4] >= 0) glUniform3f(s_locRp[4], rp.shR[0], rp.shR[1], rp.shR[2]);
			if (s_locRp[5] >= 0) glUniform3f(s_locRp[5], rp.shU[0], rp.shU[1], rp.shU[2]);
			if (s_locRp[6] >= 0) glUniform4f(s_locRp[6], rp.params[0], rp.params[1], rp.params[2], rp.params[3]);
			if (s_locRp[7] >= 0) glUniform2f(s_locRp[7], (GLfloat)width, (GLfloat)height);
			// the traced camera's ORIGIN, snapshotted per slot since the async
			// composite landed but never read until the translation-aware refine
			if (s_locRp[8] >= 0) glUniform3f(s_locRp[8], rp.curO[0], rp.curO[1], rp.curO[2]);
			if (s_locRp[9] >= 0) glUniform3f(s_locRp[9], rp.shO[0], rp.shO[1], rp.shO[2]);
		}
		{
			// TERM UPSAMPLE feeds -- LOCKSTEP shader_msl.h (same names, same values,
			// same tolerance shaping as the fog upsample: smooth = tol/4, edge = tol)
			int upen = (depthtexname != 0 && uptol > 0.0f);
			if (s_locUp  >= 0) glUniform4f(s_locUp,  (GLfloat)s_w, (GLfloat)s_h, upen ? uptol * 0.25f : 0.0f, uptol);
			if (s_locUp2 >= 0) glUniform4f(s_locUp2, (GLfloat)vpx, (GLfloat)vpy, (GLfloat)width, (GLfloat)height);
			if (s_locS2D >= 0 && screentodepth) glUniform2f(s_locS2D, screentodepth[0], screentodepth[1]);
		}
		glBindVertexArray(s_vao);
		// Depth-test the composite against the scene depth so the shadow multiply
		// only lands on WORLD pixels, never the first-person view model. The VS puts
		// the quad at window depth 0.0625 (the view-model near range); with GL_LESS it
		// passes only where the stored depth is greater (the world). Force the depth
		// range to [0,1] (the engine may have left it at the view-model [0,0.0625])
		// and disable depth writes so we never disturb the scene depth buffer.
		//
		// This is only valid when the bound framebuffer's depth is THIS frame's scene
		// depth — i.e. DarkPlaces rendered the view straight into it (the trivial-blend
		// path, scenedepthvalid != 0). On the offscreen post-process path (bloom / fxaa
		// / resolution scale / the transient viewblend screen tints for damage,
		// underwater, powerups) fbo 0's depth is stale, so gating against it would
		// mis-mask; fall back to the ungated multiply (identical to the pre-mask
		// behaviour — the view model may be darkened on those frames, but shadows are
		// never dropped and depth is never misread).
		if (scenedepthvalid) {
			if (!prevDepth) glEnable(GL_DEPTH_TEST);
			glDepthFunc(GL_LESS);
			glDepthMask(GL_FALSE);
			glDepthRange(0.0, 1.0);
		} else {
			if (prevDepth) glDisable(GL_DEPTH_TEST);
		}
		// Report the mask state whenever it changes. The weapon mask was silently
		// disabled for every user whose v_gamma != 1 (any post-process pass moved the
		// scene depth out of the composited framebuffer), which is invisible without
		// this line — tests/smoke.sh asserts on it.
		{
			static int lastreport = -1;
			int state = scenedepthvalid ? 1 : 0;
			if (state != lastreport) {
				lastreport = state;
				fprintf(stderr, "RT_Metal: composite viewmodel mask %s (%dx%d at %d,%d)\n",
						state ? "ACTIVE" : "DISABLED (view model may be darkened)", width, height, vpx, vpy);
			}
		}
		if (prevScissor) glDisable(GL_SCISSOR_TEST);
		if (prevCull) glDisable(GL_CULL_FACE);
		if (prevPolyOff) glDisable(GL_POLYGON_OFFSET_FILL);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		if (!prevBlend) glEnable(GL_BLEND);
		glBlendFunc(GL_DST_COLOR, GL_ZERO);      // result = term * scene
		glDrawArrays(GL_TRIANGLES, 0, 3);

		// restore exactly what we found
		if (depthtexname) {
			glActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex2D1);
			glActiveTexture(GL_TEXTURE0);
		}
		glBindTexture(GL_TEXTURE_RECTANGLE, (GLuint)prevRect);
		glActiveTexture((GLenum)prevActive);
		glUseProgram((GLuint)prevProg);
		glBindVertexArray((GLuint)prevVAO);
		glBlendFuncSeparate((GLenum)bsRGB, (GLenum)bdRGB, (GLenum)bsA, (GLenum)bdA);
		if (!prevBlend) glDisable(GL_BLEND);
		glDepthRange((GLclampd)prevDepthRange[0], (GLclampd)prevDepthRange[1]);
		glDepthMask(prevDepthMask);
		glDepthFunc((GLenum)prevDepthFunc);
		if (prevDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
		if (prevScissor) glEnable(GL_SCISSOR_TEST);
		if (prevCull) glEnable(GL_CULL_FACE);
		if (prevPolyOff) glEnable(GL_POLYGON_OFFSET_FILL);
		glColorMask(prevMask[0], prevMask[1], prevMask[2], prevMask[3]);

		rt_profile_accum(width, height, traceCpuMs, traceGpuMs, tCpu0);
		s_active = 1;   // composite fully ran: the RT IS relighting this frame.
		                // The GL path's own setter -- here the draw and the decision are
		                // the same piece of code, so there is nothing to hand a caller.
		                // LOCKSTEP with RT_Metal_MarkComposited, the shared path's setter.
	}
	return RT_COMPOSITE_DREW;
}

// The shared-device path's setter for the flag the line above sets on GL: the
// caller drew the composite through the backend and it succeeded. Deliberately
// not folded into RT_Metal_GetTermTexture or the seam -- "the term is available"
// and "the term has been multiplied into the scene" are different claims, and
// the whole safety of the wall-lighting fullbright forcing rests on only the
// second one setting this.
void RT_Metal_MarkComposited(void)
{
	s_active = 1;
}

// Did the last RT_Metal_Composite fully run (device up, AS ready, trace + composite
// drawn)? The wall-lighting fullbright forcing gates on this so a Mac where RT init
// failed — or a frame where the composite bailed — never renders a blinding fullbright
// world without the compensating albedo*Lrt multiply.
int RT_Metal_Active(void) { return s_active; }

void RT_Metal_Shutdown(void)
{
	rt_wait_pending();   // finish any in-flight async frame before teardown
	if (s_glctx && s_glctx == CGLGetCurrentContext()) {
		rt_release_surface();
		if (s_prog) { glDeleteProgram(s_prog); s_prog = 0; }
		if (s_vao)  { glDeleteVertexArrays(1, &s_vao); s_vao = 0; }
	} else {
		rt_abandon_gl();
	}
	s_glctx = NULL;
	s_accel = nil;
	s_vbuf = nil;
	s_ibuf = nil;
	s_wabuf = nil;
	s_lavaAccel = nil;
	s_libuf = nil;
	s_lavadegv = nil;
	s_lavaToken = (unsigned long)-1;
	s_skyAccel = nil;
	s_skyibuf = nil;
	s_skyToken = (unsigned long)-1;
	s_liqAccel = nil;
	s_liqibuf = nil;
	s_liqToken = (unsigned long)-1;
	for (int i = 0; i < 2; i++) {
		s_entityAccel[i] = nil;
		s_evbuf[i] = nil;
		s_eibuf[i] = nil;
		s_enbuf[i] = nil;
		s_escratch[i] = nil;
		s_lcAccel[i] = nil;
		s_lcvbuf[i] = nil;
		s_lcibuf[i] = nil;
		s_lcscratch[i] = nil;
		s_lcAccelCap[i] = s_lcvbufCap[i] = s_lcibufCap[i] = s_lcscratchCap[i] = 0;
		s_tlas[i] = nil;
		s_instbuf[i] = nil;
		s_tscratch[i] = nil;
		s_evbufCap[i] = s_eibufCap[i] = s_entityAccelCap[i] = s_escratchCap[i] = 0;
		s_enbufCap[i] = 0;
		s_entHasNorms[i] = 0;
		s_tlasCap[i] = s_tscratchCap[i] = 0;
	}
	s_par = 0;
	s_numentitytris = 0;
	s_numlctris = 0;
	s_histTex[0] = nil; s_histTex[1] = nil;
	s_secTex[0] = nil; s_secTex[1] = nil;
	s_giTex[0] = nil; s_giTex[1] = nil;
	s_hasPrev = 0; s_histParity = 0;
	s_shaftPso = nil;
	s_fogPso = nil;
	s_noiseTex3D = nil; s_fieldTex3D = nil; s_irrTex3D = nil;
	free(s_noiseCopy); s_noiseCopy = NULL; s_noiseSize = 0; s_noiseDirty = 0;
	free(s_fieldCopy); s_fieldCopy = NULL;
	s_fieldSize[0] = s_fieldSize[1] = s_fieldSize[2] = 0; s_fieldDirty = 0;
	free(s_irrCopy); s_irrCopy = NULL;
	s_irrSize[0] = s_irrSize[1] = s_irrSize[2] = 0; s_irrDirty = 0;
	s_lightbuf[0] = s_lightbuf[1] = nil;
	s_lightbufCap[0] = s_lightbufCap[1] = 0;
	s_numlights = 0;
	s_worldToken = 0;
	s_hasCam = 0;
	s_pso = nil;
	// dropping the references is right either way; when the device and queue
	// were handed in by the renderer (RT_Metal_InitWithDevice) they outlive us
	// and ARC keeps them alive for their real owner.
	s_queue = nil;
	s_dev = nil;
	s_sharedDevice = 0;
}

#else  // !__APPLE__ : never linked on non-Apple, but keep the symbols defined.

void RT_Metal_Init(void) {}
void RT_Metal_InitWithDevice(void *device, void *queue) { (void)device; (void)queue; }
void RT_Metal_Shutdown(void) {}
void RT_Metal_SetWorld(const float *v, int nv, const int *t, int nt, const unsigned char *a4, unsigned long tok) { (void)v;(void)nv;(void)t;(void)nt;(void)a4;(void)tok; }
void RT_Metal_SetLavaSurfaces(const int *t, int nt, unsigned long tok) { (void)t;(void)nt;(void)tok; }
void RT_Metal_SetCamera(const float o[3], const float f[3], const float r[3], const float u[3], float tx, float ty) { (void)o;(void)f;(void)r;(void)u;(void)tx;(void)ty; }
void RT_Metal_SetLights(const float *data, int numlights, int numdynamic) { (void)data; (void)numlights; (void)numdynamic; }
void RT_Metal_SetEntities(const float *v, int nv, const int *t, int nt, const float *n3f) { (void)v;(void)nv;(void)t;(void)nt;(void)n3f; }
void RT_Metal_SetLightCores(const float *v, int nv, const int *t, int nt) { (void)v;(void)nv;(void)t;(void)nt; }
void RT_Metal_SetReprojectDepth(int enable) { (void)enable; }
void RT_Metal_SetFogHistCentre(int enable) { (void)enable; }
void RT_Metal_SetFogStepJitter(int enable) { (void)enable; }
void RT_Metal_SetFogFilter(int mode, float depthtol) { (void)mode; (void)depthtol; }
void RT_Metal_SetTermMax(float knee) { (void)knee; }
void RT_Metal_SetLightSample(int enable, int rays, float wclamp) { (void)enable; (void)rays; (void)wclamp; }
void RT_Metal_SetGI(int enable, float dist, float albedo, float history, float intensity, float emissive, int rate, float albtex, int fallback, float tiledilate) { (void)enable; (void)dist; (void)albedo; (void)history; (void)intensity; (void)emissive; (void)rate; (void)albtex; (void)fallback; (void)tiledilate; }
void RT_Metal_SetGIAO(float ao, float dist) { (void)ao; (void)dist; }
void RT_Metal_SetFogLiquidLight(float amount) { (void)amount; }
void RT_Metal_SetContact(float amount) { (void)amount; }
void RT_Metal_SetShadowLights(int lights, int rays) { (void)lights; (void)rays; }
void RT_Metal_SetRefit(int mode) { (void)mode; }
void RT_Metal_SetFogAdaptiveStride(int mode) { (void)mode; }
void RT_Metal_SetFogClamp(int mode, float k, int tonemap) { (void)mode; (void)k; (void)tonemap; }
void RT_Metal_SetFogReproject(int depth, float tol) { (void)depth; (void)tol; }
void RT_Metal_SetFogFroxel(int enable, int slices, float history, float curve, float near, int castphase) { (void)enable; (void)slices; (void)history; (void)curve; (void)near; (void)castphase; }
void RT_Metal_SetASSkip(int enable) { (void)enable; }
void RT_Metal_SetSun(const float *dir, const float *col, float pen_deg, int enable) { (void)dir; (void)col; (void)pen_deg; (void)enable; }
void RT_Metal_SetLightSampleHybrid(int mode) { (void)mode; }
void RT_Metal_SetSameFrame(int enable) { (void)enable; }
void RT_Metal_SetTuning(int samples, float softness, float darkness, float history, float colorstr, float walllight, float ambient, float scale, int reproject) { (void)samples;(void)softness;(void)darkness;(void)history;(void)colorstr;(void)walllight;(void)ambient;(void)scale;(void)reproject; }
void RT_Metal_SetSkySurfaces(const int *tris3i, int numtris, unsigned long token) { (void)tris3i;(void)numtris;(void)token; }
void RT_Metal_SetLiquidSurfaces(const int *tris3i, int numtris, unsigned long token) { (void)tris3i;(void)numtris;(void)token; }
void RT_Metal_SetBlendedLiquidSurfaces(const int *tris3i, int numtris, unsigned long token) { (void)tris3i;(void)numtris;(void)token; }
void RT_Metal_SetLiquidRT(int enable, float reflect) { (void)enable; (void)reflect; }
int RT_Metal_GetLiquidTexture(unsigned int *gltex, int *w, int *h) { (void)gltex;(void)w;(void)h; return 0; }
void RT_Metal_SetShaftTuning(int enable, int samples, float scale, float history, float dist, float residual) { (void)enable;(void)samples;(void)scale;(void)history;(void)dist;(void)residual; }
int RT_Metal_GetShaftsTexture(unsigned int *gltex, int *w, int *h) { (void)gltex;(void)w;(void)h; return 0; }
void RT_Metal_TermProbe(void) {}
int RT_Metal_GetTermTexture(unsigned int *gltex, int *w, int *h) { (void)gltex;(void)w;(void)h; return 0; }
void RT_Metal_SetFogNoise(const unsigned char *rgba, int size) { (void)rgba;(void)size; }
void RT_Metal_SetFogField(const unsigned char *rgba, const int size[3], const float origin[3], const float invsize[3], float sdfrange, float maxh) { (void)rgba;(void)size;(void)origin;(void)invsize;(void)sdfrange;(void)maxh; }
void RT_Metal_SetFogIrradiance(const unsigned char *rgba, const int size[3]) { (void)rgba;(void)size; }
int RT_Metal_HasFogIrradiance(void) { return 0; }
void RT_Metal_SetFogShade(const rt_fog_shade_t *shade) { (void)shade; }
void RT_Metal_SetFogTuning(int enable, int steps, float scale, float history, float intensity, int stride, float residual, float beams) { (void)enable;(void)steps;(void)scale;(void)history;(void)intensity;(void)stride;(void)residual;(void)beams; }
int RT_Metal_GetFogTexture(unsigned int *gltex, int *w, int *h) { (void)gltex;(void)w;(void)h; return 0; }
int RT_Metal_Composite(int width, int height, int scenedepthvalid, int vpx, int vpy, unsigned int depthtexname, const float *screentodepth, float uptol) { (void)width; (void)height; (void)scenedepthvalid; (void)vpx; (void)vpy; (void)depthtexname; (void)screentodepth; (void)uptol; return RT_COMPOSITE_NONE; }
void RT_Metal_MarkComposited(void) {}
int RT_Metal_Active(void) { return 0; }
void RT_Metal_DumpFrame(int width, int height, int counting) { (void)width; (void)height; (void)counting; }
void RT_Metal_DumpFrameMetal(int width, int height, int counting) { (void)width; (void)height; (void)counting; }
void RT_Metal_ResetTemporal(void) {}
void RT_Metal_RequestDump(void) {}
int RT_Metal_GetShownCamera(float f[3], float r[3], float u[3]) { (void)f;(void)r;(void)u; return 0; }
int RT_Metal_GetReprojection(rt_reproj_t *out) { (void)out; return 0; }

#endif // __APPLE__
