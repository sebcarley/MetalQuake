/*
QuakeM5 -- the Metal texture objects (METAL.md Phase 3).

Plain C, in the rt_metal.h mould: no Objective-C or Metal types cross this
header, so gl_textures.c stays a C translation unit.

DIVISION OF LABOUR. gl_textures.c remains the texture MANAGER on both backends
-- the pool and chain, identifiers, picmip and mipmap policy, the textype
tables, image resizing, DDS parsing, purge lists and cvar callbacks are all
backend-agnostic, and only about ten of its sixty-odd functions touch GL at all.
Duplicating that bookkeeping into a second file would create exactly the kind of
hand-kept parallel copy this project has been burned by. So gl_textures.c keeps
its management logic and calls in here for the Metal-side objects, from the
RENDERPATH_METAL arms of its existing switches.

HANDLES, NOT POINTERS. A texture is identified by a small positive integer that
gl_textures.c stores in the rtexture_t's existing `texnum` field. That field is
public (the R_GetTexture macro reads it from outside gl_textures.c), so keeping
it an opaque int means the struct needs no Metal member and nothing outside the
texture system changes at all.

SCOPE, and what is deliberately refused. This slice serves the 2D path only --
console, menus and HUD -- and that path turns out to need exactly ONE of the
twenty-seven textype_t values: TEXTYPE_BGRA. (Draw_CachePic and Draw_NewPic both
resolve to it, and Draw_NewPic never reads its own textype argument, so ft2.c
asking for RGBA is a no-op -- font atlases land as BGRA like everything else.)
RGBA and ALPHA are implemented anyway because they cost nothing and BGRA-only
would be a surprising trap later; note TEXTYPE_ALPHA has zero external callers
in the whole tree. The compressed DXT/ETC set, shadowmaps, depth and colour
render buffers belong to the world and are REFUSED WITH A MESSAGE rather than
silently mishandled. A texture that quietly comes out wrong is the failure mode
this whole arc is arranged to avoid.

METAL.md Phase 6 (slice 6-1) widened that scope once: VOLUME textures are now
created, uploaded, read back and sampled, because the volumetric murk cannot
exist without two of them (its noise volume and its baked world field) and
r_lavaboil needs the first. CUBEMAPS are still refused, and so are 3D textures
in a RENDER-TARGET format -- the latter because those get private storage, on
which replaceRegion is invalid, and the only thing that asks for one is the
bounce grid, which is dead in this fork's configuration. Both refusals print
once, by name.
*/

#ifndef METAL_TEXTURES_H
#define METAL_TEXTURES_H

#include "qtypes.h"

// Feature-guarded for the same reason metal_backend.h is: metal_textures.o
// lives in makefile.inc's OBJ_METAL_MACOS (macOS client only) while its caller
// gl_textures.c is in OBJ_COMMON and links into the dedicated server too. This
// was missed when the file landed and `make sv-release` broke on six undefined
// symbols -- the identical mistake USE_RT_METAL had already been introduced to
// fix, made again one file over. Guard the FEATURE, never the platform.
#ifdef USE_METAL_RENDERER

/// Create a Metal texture object. `textype` is a textype_t; `flags` are TEXF_*.
/// Returns a positive handle, or 0 if the format is not supported on this path
/// (which is reported once, by name, rather than failing silently).
/// No pixels are uploaded here; R_SetupTexture calls Metal_Texture_Upload
/// immediately afterwards (see the note on that function).
int Metal_Texture_Create(int textype, int width, int height, int depth, int sides, int flags);

/// Adopt an MTLTexture created elsewhere (the RT sidecar's term buffer, METAL.md
/// Phase 5) into the handle table, so R_Mesh_TexBind can bind it like any other.
/// The creator keeps ownership; ARC gives the table its own strong reference, so
/// Metal_Texture_Destroy drops only the renderer's. `flags` are TEXF_* and choose
/// the sampler -- on this path filtering is sampler state, not texture state.
/// Returns 0 (loudly) if the texture is not the expected RGBA16F format.
int Metal_Texture_Adopt(void *mtltex, int width, int height, int flags);

/// Release a texture handle. Safe with 0.
void Metal_Texture_Destroy(int handle);

/// Replace the whole image. `pixels` is in the texture's own input format.
/// Called from R_UploadFullTexture, which R_SetupTexture invokes UNCONDITIONALLY
/// at load time -- upload is EAGER, not deferred to first bind. (Measured; the
/// "delayed upload for non-precached textures" machinery in gltexture_t is
/// vestigial -- its inputtexels field is never assigned anywhere in the tree.)
/// The deferral that does exist applies only to UPDATES of already-uploaded
/// textures, via the dirty flag and R_RealGetTexture. The useful consequence:
/// the whole path down to fully resampled, picmipped, format-selected pixels
/// runs today with no render loop, so this is exercisable before any draw path.
qbool Metal_Texture_Upload(int handle, const unsigned char *pixels);

/// One mip level, dimensions of THAT level. Driven from R_UploadFullTexture's
/// CPU mip loop so both backends carry byte-identical Image_MipReduce32 chains
/// (never the GPU generator -- its filter differs and would bias every mipped
/// surface). Level 0 with the full size is equivalent to Metal_Texture_Upload.
/// `depth` is 1 for ordinary 2D textures and the slice count for a volume
/// (METAL.md Phase 6): it is a real argument rather than a default because a
/// 2D-region upload into a 3D texture writes slice 0 and leaves the rest
/// undefined, with no error from Metal and nothing visible in any log.
qbool Metal_Texture_UploadLevel(int handle, int level, const unsigned char *pixels, int width, int height, int depth);

/// Replace a sub-rectangle (R_UploadPartialTexture -- lightmaps, glyph atlases).
qbool Metal_Texture_UploadPartial(int handle, const unsigned char *pixels, int x, int y, int width, int height);

/// Drop every cached MTLSamplerState. Called when the filter or anisotropy
/// cvars change, where GL instead re-walks every live texture applying
/// glTexParameteri -- Metal keeps filtering in the sampler, not the texture.
void Metal_Texture_InvalidateSamplers(void);

/// Release every texture and sampler (renderer shutdown / vid_restart).
void Metal_Texture_Shutdown(void);

/// Verification hook (VID_METAL_PROBE): read a texture back into `out`, which
/// must hold width*height*DEPTH*bytesperpixel bytes -- the depth term matters
/// since Phase 6 and is not inferable from a 2D reading of the call. Returns
/// false if the handle is unknown or the readback could not be made. A
/// readback is the cheapest way to prove an upload actually worked -- a
/// texture that silently failed to upload is indistinguishable from one that
/// did -- but note it proves only that the BYTES arrived, which for a volume
/// is not the interesting half; see Metal_Texture_Sample3D below.
qbool Metal_Texture_Readback(int handle, unsigned char *out);

/// Sample a 3D texture at `n` normalised (0..1) coordinates through the
/// renderer's OWN MTLSamplerState, writing n*4 floats. Verification only
/// (r_metal_textureprobe, METAL.md Phase 6 slice 6-1) -- it stalls on
/// waitUntilCompleted and is never called from a frame.
///
/// It exists because a readback proves only that the bytes arrived. A z axis
/// that is collapsed, flipped or transposed reads back perfectly and samples
/// wrongly, and 6-2's murk arm is built entirely on 3D sampling. Returns false
/// for a 2D texture: this instrument is for volumes.
qbool Metal_Texture_Sample3D(int handle, const float *coords, float *out, int n);

/// The MTLTexture and the MTLSamplerState for a handle, as opaque pointers so
/// this header stays plain C (the VID_Metal_GetDevice convention). NULL for an
/// unknown handle. metal_backend.m bridges them back to bind a draw:
/// (__bridge id<MTLTexture>)Metal_Texture_GetTexture(h). GL keeps filtering on
/// the texture object, Metal keeps it in a separate sampler, which is why these
/// are two calls rather than one.
void *Metal_Texture_GetTexture(int handle);
void *Metal_Texture_GetSampler(int handle);

/// The sampler to use on a texture unit that has no texture bound. Metal's
/// API validation traps on a sampler a shader declares and nothing binds --
/// which the release build never sees, because validation is off there. See
/// the implementation for why the texture stays nil.
void *Metal_Texture_GetNullSampler(void);

#else // !USE_METAL_RENDERER

static inline int  Metal_Texture_Create(int textype, int width, int height, int depth, int sides, int flags) { (void)textype; (void)width; (void)height; (void)depth; (void)sides; (void)flags; return 0; }
static inline int  Metal_Texture_Adopt(void *mtltex, int width, int height, int flags) { (void)mtltex; (void)width; (void)height; (void)flags; return 0; }
static inline void Metal_Texture_Destroy(int handle) { (void)handle; }
static inline qbool Metal_Texture_Upload(int handle, const unsigned char *pixels) { (void)handle; (void)pixels; return false; }
static inline qbool Metal_Texture_UploadLevel(int handle, int level, const unsigned char *pixels, int width, int height, int depth) { (void)handle; (void)level; (void)pixels; (void)width; (void)height; (void)depth; return false; }
static inline qbool Metal_Texture_UploadPartial(int handle, const unsigned char *pixels, int x, int y, int width, int height) { (void)handle; (void)pixels; (void)x; (void)y; (void)width; (void)height; return false; }
static inline void Metal_Texture_InvalidateSamplers(void) {}
static inline void Metal_Texture_Shutdown(void) {}
static inline qbool Metal_Texture_Readback(int handle, unsigned char *out) { (void)handle; (void)out; return false; }
static inline qbool Metal_Texture_Sample3D(int handle, const float *coords, float *out, int n) { (void)handle; (void)coords; (void)out; (void)n; return false; }
static inline void *Metal_Texture_GetTexture(int handle) { (void)handle; return 0; }
static inline void *Metal_Texture_GetSampler(int handle) { (void)handle; return 0; }
static inline void *Metal_Texture_GetNullSampler(void) { return 0; }

#endif // USE_METAL_RENDERER

#endif
