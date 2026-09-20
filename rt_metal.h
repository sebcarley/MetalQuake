/*
	rt_metal.h — in-process Metal RT sidecar (macOS only)

	Spike goal: prove the engine can run a Metal device in its OWN process and
	present a Metal-authored image through its real live OpenGL context, via a
	shared IOSurface. This is the first actual engine<->Metal integration; the
	standalone validation lives in metal/sidecar.m.

	The entry points are plain C so the engine's C translation units never pull
	in <Metal/Metal.h>. Only rt_metal.m sees Objective-C / Metal. On non-Apple
	builds this file is not compiled in (see makefile.inc) and the call sites in
	vid_sdl.c are #ifdef MACOSX guarded.
*/
#ifndef RT_METAL_H
#define RT_METAL_H

#ifdef __cplusplus
extern "C" {
#endif

// Create the MTLDevice / command queue / compute pipeline. Safe to call once;
// repeated calls are no-ops. Does not touch GL (GL objects are created lazily).
void RT_Metal_Init(void);

// As RT_Metal_Init, but ADOPTS a device and command queue the caller already
// owns (both are id<MTLDevice> / id<MTLCommandQueue>, passed as void* so this
// header stays plain C). Passing NULL for either makes the sidecar create its
// own -- so RT_Metal_Init() is exactly RT_Metal_InitWithDevice(NULL, NULL) and
// the GL path is unchanged.
//
// This exists for METAL.md Phase 5: two MTLDevice objects for one GPU cannot
// share resources, which is the whole reason the sidecar's output has to cross
// into GL through an IOSurface today. Once the Metal renderer owns a device and
// hands it here, the RT outputs become plain MTLTextures the renderer samples
// directly and the bridge (and its hazards) can be compiled out.
void RT_Metal_InitWithDevice(void *device, void *queue);

// Release all Metal and GL resources. Safe to call when uninitialised.
void RT_Metal_Shutdown(void);

// Hand the world render mesh to the sidecar and (re)build a Metal acceleration
// structure from it. `token` identifies the geometry (pass the model pointer):
// the AS is only rebuilt when it changes, so this is cheap to call every frame.
// verts3f is float[3*numverts]; tris3i is int[3*numtris] (indices into verts).
// albedo4 (GIARC G4-2, may be NULL): RGBA8 per TRIANGLE, the mean surface
// colour the coloured bounce multiplies at a world bounce hit. Rides the
// world's exact lifecycle and token -- deliberately NOT its own entry point
// (unlike lava/sky, nothing here must survive a SetWorld rebuild).
void RT_Metal_SetWorld(const float *verts3f, int numverts, const int *tris3i, int numtris, const unsigned char *albedo4, unsigned long token);

// Lava sheets as a separate STATIC emissive instance (rt_metal_lavaemissive):
// indices into the SAME vertex buffer SetWorld uploaded (always call SetWorld
// first each frame). Deliberately NOT folded into SetWorld -- a SetWorld
// rebuild drops the baked fog field until the next map bake, and a cvar
// toggle must never do that. numtris 0 builds a degenerate stand-in.
void RT_Metal_SetLavaSurfaces(const int *tris3i, int numtris, unsigned long token);

// Sky brushes as the OPEN SKY instance (rt_metal_skyopen): indices into the
// SAME vertex buffer SetWorld uploaded, exactly the lava shape (own token,
// never folded into SetWorld -- the fog-field rule). The surface kernel treats
// a hit on this instance as a MISS, so the sky sentinel -- and everything that
// keys on it: the fog kernel's sky handling, r_volumetric_skyfog, beams
// crossing sky windows -- finally fires on map sky, and wall lighting stops
// multiplying the sky sphere by a brush's lighting term. numtris 0 builds a
// degenerate stand-in.
void RT_Metal_SetSkySurfaces(const int *tris3i, int numtris, unsigned long token);

// Opaque water/slime as the EMISSIVE liquid instance (rt_metal_liquidemissive):
// the third of the lava-shaped family (own token, never folded into SetWorld --
// the fog-field rule). Primary rays STOP at the liquid and the surface kernel
// shades the hit emissive (term 1.0), so the composite leaves the raster sheet
// exactly as authored -- Q1 liquids carry no lightmaps, so this IS the
// rt_metal 0 look, and the pool floor's lighting stops printing through under
// wall lighting. Shadow rays still pass through (liquids never occlude); the
// fog and shaft marches stop at the surface via the history. The caller's
// gather folds the transparent-water enabling set, so the list is EMPTY
// whenever liquids render blended and rt_metal_liquids owns the surface
// instead. numtris 0 builds a degenerate stand-in.
void RT_Metal_SetLiquidSurfaces(const int *tris3i, int numtris, unsigned long token);
// SEPTEMBER2 C2 (2026-09-09): the BLENDED liquids -- instance 6 at mask 0x40, which only
// the C2 intersect sees (the primary mask never carries it: the pool floor keeps its
// term). Gathered when liquids render blended and rt_metal_liquids_rt is on; numtris 0
// builds a degenerate stand-in, and the index buffer is bound to the trace always.
void RT_Metal_SetBlendedLiquidSurfaces(const int *tris3i, int numtris, unsigned long token);
// rt_metal_liquids_rt (0/1) and rt_metal_liquids_reflect (gain), per frame.
void RT_Metal_SetLiquidRT(int enable, float reflect);

// Set the camera for the next trace. Vectors are Quake-space (forward/right/up);
// tanx/tany are the tangents of the half horizontal/vertical field of view.
void RT_Metal_SetCamera(const float origin[3], const float forward[3], const float right[3], const float up[3], float tanx, float tany);

// Floats per light in the buffer handed to RT_Metal_SetLights. Every producer
// and all three kernels index by this; change it in one place and the kernels'
// tgl[] caches follow.
//   [0..2] origin xyz
//   [3]    radius
//   [4..6] colour rgb
//   [7]    FOG WEIGHT — scales this light's contribution to the volumetric fog
//          ONLY, both its scattered colour and its say in the fog kernel's
//          dominant-light vote. 1.0 is "counts normally", which is what every
//          producer except the M5 thunderbolt writes. Surfaces ignore it
//          entirely, which is what makes the fog knob independent of the
//          surface lighting.
//   [8..10] SPOT DIRECTION, unit length, pointing the way the light throws.
//          ALL ZERO MEANS OMNI, and that is the no-op contract: every kernel
//          tests the squared length and multiplies by exactly 1.0f when it is
//          zero, so an omni light is bit-identical to the pre-spot build.
//   [11]   SPOT cos(outer half-angle). Read only when [8..10] is non-zero.
//   [12]   SPOT cos(INNER half-angle) -- where the cone reaches full brightness.
//          The shoulder runs from [11] to [12]. <= [11] (which includes the
//          memset zero every non-handlamp producer leaves here) means "unset",
//          and rt_cone falls back to the historic fixed 35% shoulder, so an
//          omni light and a cone that never sets it are both bit-identical to
//          the pre-2026-09-01 build.
//
// THE STRIDE IS INJECTED INTO ALL THREE KERNEL SOURCES AS `RTL` at compile time
// (rt_metal.m, the newLibraryWithSource calls), so the sentence above about
// changing it in one place is now literally true. It was not before F6: the
// kernels carried bare `8u` literals in twenty-one places and a `tgl[256 * 8]`
// staging array in three, so this define and the code disagreed silently.
#define RT_LIGHT_STRIDE 14

// Hand the map's real lights to the sidecar. `data` is RT_LIGHT_STRIDE floats
// per light, laid out as above. Called every frame; copied into a grow-only
// per-slot GPU buffer (double-buffered against the in-flight async frame).
// The FIRST `numdynamic` lights must be the DYNAMIC ones (explosions / flashes),
// prepended by the caller — only those receive the colored brighten in the kernel.
void RT_Metal_SetLights(const float *data, int numlights, int numdynamic);

// Hand this frame's dynamic shadow-casting entity geometry (monsters, items,
// doors, ...) to the sidecar. verts3f is float[3*numverts] in WORLD space;
// tris3i is int[3*numtris] (indices into verts). Copied into reused GPU buffers
// and turned into a per-frame acceleration structure so entities cast RT shadows
// from the real lights. Pass numtris == 0 for "no casters this frame". Call every
// frame before RT_Metal_Composite. The player's own gun/body should be excluded
// by the caller (see cl_screen.c). norms3f is float[3*numverts] world-space unit
// vertex normals for SMOOTH SHADING of entity hits, or NULL for the flat
// per-triangle look (zero-length entries fall back per pixel).
void RT_Metal_SetEntities(const float *verts3f, int numverts, const int *tris3i, int numtris, const float *norms3f);

// Hand this frame's LIGHT-CORE geometry (torch/brazier flame models) to the
// sidecar. Same layout and per-frame contract as RT_Metal_SetEntities, but the
// mesh goes to a separate TLAS instance that shadow rays SKIP (a flame sits on
// its own light's origin and would otherwise block it) and whose hits the
// surface kernel shades as emissive (term 1.0 -- the flame renders exactly as
// authored). BLAS-only input: no normals, never shaded from its vertex data.
// Pass numtris == 0 for none this frame.
void RT_Metal_SetLightCores(const float *verts3f, int numverts, const int *tris3i, int numtris);

// Set the runtime shadow tuning knobs (from the rt_metal_* cvars) for the next
// trace. samples = shadow rays per pixel; softness = area-light radius as a
// fraction of the light radius (penumbra width); darkness = scene multiplier
// where fully shadowed (0 = black, 1 = none); history = temporal accumulation
// weight in [0,0.98] (0 = temporal off; higher = smoother but slower to react);
// colorstr = strength of the colored brighten applied where a DYNAMIC light is
// visible (0 = grey shadows only, the old look; higher = explosions flare harder).
// walllight = FULL wall-lighting intensity: 0 = shadows-over-lightmap mode; >0 = the
// scene is rendered fullbright (albedo) and the kernel outputs full per-pixel lighting
// (ambient + all lights, dominant soft-shadowed) at this intensity, replacing the
// baked lightmap. ambient = the fill light used in that mode. scale = trace
// resolution as a fraction of the viewport (0.25-1; 1 = per-pixel, the historic
// look; lower = faster, softer shadow edges). reproject = remap the shown term
// through the camera it was traced with, so the RT lighting no longer trails
// fast mouse turns (the term is one frame stale in async mode, this frame's
// own under rt_metal_sameframe -- where the remap is an identity and is
// clamped off, see RT_Metal_SetSameFrame). Cheap; call every frame.
void RT_Metal_SetTuning(int samples, float softness, float darkness, float history, float colorstr, float walllight, float ambient, float scale, int reproject);
void RT_Metal_SetReprojectDepth(int enable);

// rt_metal_bluenoise (the weave fix): 1 = the kernels jitter with the committed
// blue-noise table (featureless grain), 0 = the classic IGN dither, byte-exact.
// Call every frame; a CHANGE lazily rebuilds the kernel PSOs (one-off hitch).
void RT_Metal_SetBlueNoise(int enable);

// rt_metal_sameframe: show THIS frame's trace instead of the previous frame's
// (the slot order RT_METAL_SYNC=1 has always forced, promoted to a runtime
// switch at METAL.md Phase 8-2). Removes the one-frame fringing at close
// silhouettes at the cost of the async overlap -- measured 12-15% fps at
// GPU-saturating settings. Call every frame; a mode CHANGE drains the
// in-flight command buffers (the transition frame is the hazard, not the
// steady state). Also clamps the reprojection consumers to their disabled
// paths -- under same-frame the remap is an exact identity in algebra but not
// in IEEE, and clamping restores the bit-exact NEAREST fetch at full trace
// resolution. The fog/shaft EMA reprojection is deliberately NOT clamped:
// that history is still one encode-frame old whatever is shown.
void RT_Metal_SetSameFrame(int enable);

// The camera basis the most recently SHOWN trace was encoded with, pre-divided
// for reprojection (right/tanx, up/tany). Consumers of the shown slot's fog and
// shaft textures reproject through it, matching the composite. Returns 0 when
// nothing valid is shown (outputs untouched).
int RT_Metal_GetShownCamera(float forward[3], float rightovertanx[3], float upovertany[3]);

// Set the god-ray tuning knobs (from the rt_metal_shafts_* cvars) for the next trace.
// enable gates the whole shaft pass; samples = points along each view ray (1-16);
// scale = shaft buffer resolution as a fraction of the RT viewport (0.125-1);
// history = temporal blend of the shaft buffer (0-0.9); dist = maximum shaft distance
// in world units (also used where the ray hits sky); residual = weight of the
// unshadowed non-dominant lights (0-1). Cheap; call every frame.
void RT_Metal_SetShaftTuning(int enable, int samples, float scale, float history, float dist, float residual);

// Per-frame parameters of the volumetric fog DENSITY MODEL, mirrored from the
// r_volumetric_* cvars by gl_rmain.c (R_Volumetric_GetFogKernelParams). The fog
// kernel evaluates the same model as the GL murk shader; keep the two in lockstep.
typedef struct rt_fog_shade_s {
	float windoffset[3];                 // wind velocity * cl.time, world units
	float color[3];                      // authored air murk colour (the unlit term)
	float watercolor[3], slimecolor[3], lavacolor[3];
	float fieldorigin[3], fieldinvsize[3];
	float density, height, basez, dist;
	float noisescale, noisethresh;
	float waterdensity, watermode, flooroffset, floormode;
	// PER-LIQUID DENSITY (2026-09-06), appended at the END like every other ABI
	// growth here; both equal waterdensity unless the cvars are set off -1.
	float slimedensity, lavadensity;
	float watermist, mistheight, sdfrange, corner, fieldmaxh;
	// GROUND FOG (dry-ice layer): appended in lockstep with the kernel's FogCam
	float grounddensity, groundheight, groundnoisescale, groundthresh;
	float grounddeform, groundoffset;
	float groundwindoffset[3];   // wind velocity * cl.time, like windoffset
	float groundcolor[3];
	float lavaglow;              // warm in-air glow near lava (r_volumetric_lavaglow)
	float skytrans;              // sky transmittance floor (1 - r_volumetric_skyfog; 0 = uncapped)
	float irrorigin[3];          // AMBIENT IRRADIANCE grid (F5, append-only like the rest)
	float irrinvsize[3];
	float irrgain;               // r_volumetric_ambientgain
	float irrfloor;              // r_volumetric_ambientfloor
	float irrstrength;           // r_volumetric_ambient; 0 = the self-lit murk exactly
	float extinction;            // r_volumetric_extinction; 1 = the classic model exactly
	// KH SWIRL (F3, append-only like the rest): curl displacement of the noise
	// lookups. amp 0 = the un-swirled lookups exactly.
	float swirlamp;              // r_volumetric_swirl, world units
	float swirlscale;            // r_volumetric_swirlscale
	float swirlkh;               // r_volumetric_swirlkh (interface-band boost)
	float swirlspare;
	// MIST-LAVA CUT (F7): the round's independent append, re-serialised AFTER the
	// stack's fields at merge time -- the order here is the merge's contract, and
	// the other four sites (GetFogKernelParams, the FogCam text, the RTFogCam
	// mirror + static assert, the fc fill) follow it.
	float mistlavacut;           // lava suppression of the surface mist band (r_volumetric_mistlavacut)
	float liquidfloor;           // r_volumetric_liquidfloor: the liquid surface as the air's floor (2026-09-07)
} rt_fog_shade_t;

// Hand the 64^3 RGBA8 tiling noise volume to the sidecar (copied; may be called
// before RT_Metal_Init -- the Metal texture is created lazily). R = density field,
// G = fog-bed undulation, B = the ground layer's top swell; sampled with wrap +
// linear exactly like the GL side.
void RT_Metal_SetFogNoise(const unsigned char *rgba, int size);

// Hand the baked world field to the sidecar (copied; re-uploaded whenever called,
// i.e. on every rebake). Channels as R_Volumetric_GetField documents. Dropped on
// RT_Metal_SetWorld so an old map's field never shades a new world.
void RT_Metal_SetFogField(const unsigned char *rgba, const int size[3], const float origin[3], const float invsize[3], float sdfrange, float maxh);

// Hand the baked ambient irradiance grid to the sidecar (copied, lazy 3D texture,
// dropped with the field on world change). RGB at 128 = fully lit; grid origin,
// scale, gain, floor and strength ride the per-frame fog shade like the field's
// own parameters.
void RT_Metal_SetFogIrradiance(const unsigned char *rgba, const int size[3]);

// Push this frame's density-model parameters (cheap; call every frame).
void RT_Metal_SetFogShade(const rt_fog_shade_t *shade);

// rt_metal_fog_upsample's kernel half: the fog kernel reads each texel's march end
// from the history at the texel CENTRE instead of its lower-left corner, so a
// texel's ray direction and end distance come from the same place -- the
// depth-aware composite labels its taps at that same place and would otherwise
// trust texels that are fog to neither surface at every silhouette. 0 = the old
// mapping byte for byte. Cheap; call every frame.
void RT_Metal_SetFogHistCentre(int enable);
void RT_Metal_SetFogLiquidLight(float amount);   // BEAUTY B2: lit fog terms inside a liquid (0 = suppressed, the old bytes)
/// S2 of the fog-buffer plan: spread the fog march's per-texel jitter along the
/// ray (a golden-ratio Kronecker walk per step) so the integrated error is not
/// one lattice-locked sawtooth. 0 = the old bytes exactly.
void RT_Metal_SetFogStepJitter(int enable);
/// S1 of the fog-buffer plan: a 3x3 (mode 1) or 5x5 (mode 2) depth- and
/// transmittance-aware filter over the fog kernel's raw output at FOG resolution,
/// the only stage that removes the IGN lattice at source. 0 = off, byte for byte.
void RT_Metal_SetFogFilter(int mode, float depthtol);
/// BLUENOISE slice 2 / 3 (2026-09-03): the fog history pass. mode 0 = today's
/// path byte for byte (rt_fog blends its own EMA against last frame's filtered
/// surface); 1 = a min/max neighbourhood clamp of the reprojected history
/// against the current frame's 3x3, 2 = a variance clip at mean +/- k sigma,
/// 3 = the decoupled EMA with no clamp (the reorder A/B). Under any non-zero
/// mode the EMA accumulates the RAW scatter, the display filter runs once on
/// top, and the history ceiling is 0.95 rather than 0.9. tonemap blends in a
/// c/(1+luma) domain (rt_metal_fog_tonemapema; needs mode >= 1).
void RT_Metal_SetFogClamp(int mode, float k, int tonemap);
/// 2026-09-03 afternoon: the history pass reprojects each texel's fog through the
/// FULL previous camera at its scatter-centroid depth (rt_fog writes it beside the
/// fog) and reads the history bilinearly, rejecting it where the previous frame's
/// centroid depth disagrees by more than tol. 0 = the rotation-only remap.
void RT_Metal_SetFogReproject(int depth, float tol);
/// A1 FROXEL (SEPTEMBER2, 2026-09-06): rt_metal_fog_froxel. The fog kernel
/// marches FIXED exponential depth slices to r_volumetric_dist and stores each
/// cell's fog in a 3D volume whose history is reprojected per cell at the
/// cell's own depth through the full previous camera; an integrate pass sums
/// the accumulated cells into the fog surface the composite already consumes.
/// 0 = the shipped rt_fog PSO (RT_FROXEL 0), byte for byte. slices = the
/// march's step count in that arm; history = the per-cell blend; curve = the
/// exponential spacing exponent (0 = uniform).
void RT_Metal_SetFogFroxel(int enable, int slices, float history, float curve, float near, int castphase);
/// SEPTEMBER2 A3: skip the entity / light-core BLAS refit (and the TLAS build) on a slot whose uploaded bytes are unchanged since its last build. Byte-exact by construction.
void RT_Metal_SetASSkip(int enable);
/// SEPTEMBER2 D: the sky light -- one closest-hit ray per pixel toward the sun through the open-sky instance. dir toward the sun (unit), col in Lsum units, pen the disc's angular radius in degrees.
void RT_Metal_SetSun(const float *dir, const float *col, float pen_deg, int enable);
/// rt_metal_lightsample_hybrid: 0 = the single stochastic pick per fog cast (the old
/// bytes), 1 = the dominant light always shadow-tested on its own ray plus the pick
/// over the rest (two rays a cast), 2 = the two halves on alternating casts (one ray),
/// 3 = single-pass: the one reservoir draw decides which half this cast serves (one
/// ray, no second light loop; the dominant is measured in proportion to its weight).
void RT_Metal_SetLightSampleHybrid(int mode);
// rt_metal_lightsample / _rays: shadow-test a light chosen in proportion to its
// contribution (the surface kernel adds a SECOND light beside the dominant; the
// fog and shaft kernels replace the dominant pick), weighted by 1/p so every
// light is shadowed in expectation. 0 restores the dominant-only kernels.
// wclamp (rt_metal_lightsample_clamp, 2026-08-29): ceiling on the fog/shaft
// arms' 1/p pick weight -- the demo17 speckle fix; 0 = unclamped, old bytes.
void RT_Metal_SetLightSample(int enable, int rays, float wclamp);
// WARCHEST session 2: 0 = rebuild the dynamic BLAS every frame (the old path
// byte for byte), 1 = refit in place when topology allows.
void RT_Metal_SetRefit(int mode);
// WARCHEST session 3: adaptive fog shadow-cast stride (0 = the fixed schedule).
void RT_Metal_SetFogAdaptiveStride(int mode);

// ONE-BOUNCE GI (rt_metal_gi, GIARC G1): a cosine-hemisphere bounce ray per
// pixel from the primary hit, direct light evaluated stochastically at the
// bounce hit (the FOGLIGHT pick, 1/p weighted), accumulated in a reprojected
// colour EMA and added into the wall-lighting term before the lmax shoulder.
// enable 0 = the old kernels byte for byte. Call every frame.
void RT_Metal_SetGI(int enable, float dist, float albedo, float history, float intensity, float emissive, int rate, float albtex, int fallback, float tiledilate);

// rt_metal_lmax: a soft ceiling on the wall-lighting term (max-channel Reinhard
// shoulder from `knee` asymptoting at knee*5/3, hue-preserving). The term has a
// floor and no ceiling -- surfaces beside their own light read 5-7 against a
// frame median of 1-2 -- and that tail multiplied into fine albedo detail is the
// bright single pixels QA saw at silhouettes and fixtures under EDR + MetalFX.
// 0 = no ceiling, byte for byte the old term. Cheap; call every frame.
void RT_Metal_SetTermMax(float knee);
void RT_Metal_SetGIAO(float ao, float dist);   // BEAUTY B1: AO from the bounce ray on the ambient fill (0 = off)
void RT_Metal_SetContact(float amount);        // BEAUTY B3: contact-hardened dominant shadow (0 = off)
void RT_Metal_SetShadowLights(int lights, int rays);
int  RT_Metal_HasFogIrradiance(void);         // 0 = the world build dropped it and the murk must re-hand it  // 2026-09-19: brightest lights shadow-tested per pixel (1 = the dominant alone)

// Fog kernel tuning from the rt_metal_fog_* cvars. enable gates the whole pass and
// supersedes the god-ray kernel while set.
// beams = strength of the DENSITY-INDEPENDENT god-ray term (0-4): the classic beam
// look that glows even in clear air, on top of the density-coupled light.
void RT_Metal_SetFogTuning(int enable, int steps, float scale, float history, float intensity, int stride, float residual, float beams);

// The fog kernel's output for the most recently composited frame: premultiplied
// scattered light in RGB, transmittance in A, GL_TEXTURE_RECTANGLE at (w, h).
// Returns 0 when unavailable (off, first frame, kernel compile failure, non-Metal
// builds) -- the murk must then fall back to its own GL march.
int RT_Metal_GetFogTexture(unsigned int *gltex, int *w, int *h);

// The RT lighting term for the most recently COMPOSITED frame, RGBA16F and
// trace-sized. This is the exact texture the composite multiplied into the opaque
// scene; sampling it during the transparent pass lets liquid surfaces receive the
// same lighting the walls got. Returns 0 (outputs untouched) before the first
// completed trace or on non-Metal builds. Valid until the next RT_Metal_Composite.
//
// WHAT THE HANDLE MEANS DEPENDS ON THE PATH, and both are "a texture name the
// caller's renderer understands": on the GL path a GL_TEXTURE_RECTANGLE name,
// addressed in texels (fragment pixel minus the viewport origin); on the
// shared-device path a renderer texture handle for R_Mesh_TexBind /
// Metal_Backend_TexBind, addressed by normalised 0..1 texcoords like every other
// Metal texture. The rect-vs-normalised difference is why the GL composite needs
// its rtoff/rtscale uniforms and the Metal one needs neither.
int RT_Metal_GetTermTexture(unsigned int *gltex, int *w, int *h);
// SEPTEMBER2 C2: the liquid pair the shown trace wrote (own term | reflection, side by
// side: *w is the DOUBLE width). 0 = not live this frame (arm off, no trace yet).
int RT_Metal_GetLiquidTexture(unsigned int *gltex, int *w, int *h);

// The god-ray output for the most recently COMPOSITED frame, as a GL texture.
// Returns 1 and fills gltex (a GL_TEXTURE_RECTANGLE name), w and h when a fully
// traced shaft buffer exists; returns 0 (outputs untouched) when shafts are off,
// the RT has not completed a frame yet, or on non-Metal builds. The texture stays
// valid until the next RT_Metal_Composite.
int RT_Metal_GetShaftsTexture(unsigned int *gltex, int *w, int *h);

// The reprojection block for the frame about to be composited: everything the
// remap needs, computed ONCE here and uploaded by whichever backend is drawing.
// The GL bridge's own composite reads it too, so the two paths cannot drift in
// the VALUES -- only in the shader expression, which is unavoidable (GLSL in
// rt_metal.m's kCompFS, MSL in shader_msl.h's MODE_RTCOMPOSITE) and is marked
// LOCKSTEP at both sites.
//
// cur* is the CURRENT camera (the frame being drawn), with right and up
// PRE-SCALED by its tangents; sh* is the camera the shown term was actually
// traced with, with right and up PRE-DIVIDED by its tangents. That asymmetry is
// the remap: build this pixel's ray in the current basis, project it into the
// shown one. params = (enable, trace width, trace height, translation refine).
typedef struct rt_reproj_s
{
	float curF[3], curR[3], curU[3];
	float shF[3], shR[3], shU[3];
	float params[4];
	float curO[3], shO[3];
}
rt_reproj_t;

// Fill `out` for the slot the composite is showing. Returns 0 (out untouched)
// when there is nothing to reproject against -- no shown slot, no snapshotted
// camera, or reprojection switched off -- in which case the caller composites
// the term unremapped, exactly as it did before slice 5-4.
int RT_Metal_GetReprojection(rt_reproj_t *out);

// METAL.md Phase 5 slice 2: hash the SHOWN term buffer and print one greppable
// line. The cross-backend identity of that hash is the proof that the whole
// compute half ported to the shared-device path -- same kernel, same device,
// same inputs, only the allocation differs. Console: rt_metal_termprobe.
void RT_Metal_TermProbe(void);

// GPU-ray-trace the world from the current camera into a shared IOSurface sized
// (width,height) and MULTIPLY the resulting shadow term into the currently-bound
// scene framebuffer (a fullscreen multiply-blend quad). Called from R_RenderView
// immediately after the scene render, while the scene's own depth buffer is still
// bound, with the engine's GL context current. Saves/restores every GL state it
// touches (including face culling and polygon offset, which are live mid-3D-render)
// so the rest of the frame is unaffected. No-op if Metal init failed or no
// world/camera has been set, leaving the frame untouched.
//
// (width,height) is the 3D VIEWPORT size — also the trace resolution — and (vpx,vpy)
// is that viewport's origin within the bound framebuffer, in GL bottom-left
// coordinates (r_refdef.view.viewport.x/y, which already accounts for the
// window-vs-FBO y flip). The pair is needed because gl_FragCoord is in framebuffer
// space while the RT texture is only viewport-sized; they differ when the view is a
// subregion (scr_viewsize < 100, r_letterbox, side-by-side stereo).
//
// scenedepthvalid must be nonzero only when the bound framebuffer's depth buffer
// holds this frame's scene depth; it gates the view-model depth mask. At the
// R_RenderView hook that is always true, so pass 1. Passing 0 falls back to an
// ungated multiply (the view model gets darkened).
//
// RETURNS one of the three below. On the shared-device path the function does
// everything EXCEPT the drawing -- there is no GL context to draw through and the
// backend draw belongs in C, not in this file -- so it returns PUBLISHED and the
// caller draws, then calls RT_Metal_MarkComposited(). NONE always means "nothing
// was shown, do not draw"; it is the safe answer and every failure returns it.
#define RT_COMPOSITE_NONE      0   // no term this frame: caller draws nothing
#define RT_COMPOSITE_DREW      1   // GL path: the multiply is already on the framebuffer
#define RT_COMPOSITE_PUBLISHED 2   // shared-device path: term is bound-able, caller must draw
int RT_Metal_Composite(int width, int height, int scenedepthvalid, int vpx, int vpy, unsigned int depthtexname, const float *screentodepth, float uptol);

// Declare that the caller's own composite draw succeeded, after a PUBLISHED
// return. This is what makes RT_Metal_Active() true on the shared-device path;
// the GL path sets it inside RT_Metal_Composite instead, since there the draw and
// the decision are the same piece of code. Two setters, one owner each, so there
// is no path that both double-sets and none that forgets: a caller that returns
// early between PUBLISHED and the draw simply leaves RT inactive for the frame,
// which is the fallback the wall-lighting fullbright forcing needs.
void RT_Metal_MarkComposited(void);

// Spike verification aid: dump the final backbuffer (scene + composite + HUD)
// once if the env var RT_METAL_DUMP is set. Call at end of frame (VID_Finish).
// counting: 1 on frames with run-stable numbering (timedemo playback frames);
// only those advance the dump frame counter, so ".fN" names align across runs.
// This is the GL renderpath's arm (glReadPixels of the window backbuffer).
void RT_Metal_DumpFrame(int width, int height, int counting);

// The Metal renderpath's arm of the same dump (METAL.md Phase 8-1a): reads
// fbo 0's colour — the backend screen texture, scene + HUD, pre-present — and
// writes the identical [w][h][RGBA8 bottom-up] file with the identical frame
// numbering (the counter and env parse are shared). Pass vid.mode.width and
// height, which are what sized the screen texture. Cannot see the present
// pass; clamps to 8-bit under EDR (the caller notes that once on stderr).
void RT_Metal_DumpFrameMetal(int width, int height, int counting);

// Reset the jitter-rotation phase and temporal chains to a run-invariant state.
// Called at timedemo start (dump A/B rig); never during normal play.
void RT_Metal_ResetTemporal(void);

// Returns nonzero if the last RT_Metal_Composite fully ran (Metal device + AS ready,
// trace + composite drawn) — i.e. the RT is actually relighting the frame. The full
// wall-lighting mode gates its r_fullbright forcing on this so a Mac where RT init
// failed never renders a blinding fullbright world with no compensating RT lighting.
// Always 0 on non-Metal builds.
int RT_Metal_Active(void);

// Request that the NEXT frame be dumped to RT_METAL_DUMP (in addition to the
// frame-number triggers). Lets a console command capture an exact moment (e.g. a
// menu that is only up momentarily). No-op if RT_METAL_DUMP is unset.
void RT_Metal_RequestDump(void);

#ifdef __cplusplus
}
#endif

#endif // RT_METAL_H
