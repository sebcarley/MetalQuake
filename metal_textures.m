/*
QuakeM5 -- the Metal texture objects (METAL.md Phase 3). See metal_textures.h
for the division of labour with gl_textures.c and why handles are integers.

Compiled only on macOS (makefile.inc gates the object, as for rt_metal.o and
vid_metal.o). ARC is on.
*/

#if defined(__APPLE__) && defined(USE_METAL_RENDERER)

#import <Metal/Metal.h>

#include "quakedef.h"
#include "metal_textures.h"
#include "metal_fx.h"
#include "vid_metal.h"

// ---------------------------------------------------------------------------
// the handle table
//
// gl_textures.c stores the handle in rtexture_t.texnum, which is public (the
// R_GetTexture macro reads it from outside the texture system), so it must stay
// a plain int. Slot 0 is reserved as "no texture" because GL uses 0 that way and
// every existing `if (texnum)` test in the engine relies on it.

typedef struct mt_tex_s
{
	id<MTLTexture> tex;
	int   width, height, depth, sides;
	int   flags;
	int   textype;
	int   bytesperpixel;   // of the INPUT data handed to us
	qbool used;
}
mt_tex_t;

#define MT_MAX_TEXTURES 8192
static mt_tex_t mt_textures[MT_MAX_TEXTURES];
static int mt_numtextures = 1;   // slot 0 reserved

// sampler cache. GL puts filtering on the texture (glTexParameteri); Metal puts
// it in a separate immutable object, so one sampler serves every texture that
// shares a filter/wrap/mip combination. The key is tiny -- the sampler's own
// resolved min/mag/mip/wrap/anisotropy, see mt_sampler_key -- so a linear scan
// over a few entries is the right structure.
typedef struct mt_sampler_s
{
	int key;
	id<MTLSamplerState> state;
}
mt_sampler_s_t;

#define MT_MAX_SAMPLERS 32
static mt_sampler_s_t mt_samplers[MT_MAX_SAMPLERS];
static int mt_numsamplers;

// ---------------------------------------------------------------------------
// format table
//
// Only the four textype_t values the 2D path can reach are implemented. The
// rest belong to the world and are refused BY NAME rather than mapped to
// something plausible -- see metal_textures.h.

static const char *mt_textype_name(int textype)
{
	switch (textype)
	{
	case TEXTYPE_PALETTE:        return "PALETTE";
	case TEXTYPE_RGBA:           return "RGBA";
	case TEXTYPE_BGRA:           return "BGRA";
	case TEXTYPE_ALPHA:          return "ALPHA";
	case TEXTYPE_SRGB_PALETTE:   return "SRGB_PALETTE";
	case TEXTYPE_SRGB_RGBA:      return "SRGB_RGBA";
	case TEXTYPE_SRGB_BGRA:      return "SRGB_BGRA";
	case TEXTYPE_DXT1:           return "DXT1";
	case TEXTYPE_DXT1A:          return "DXT1A";
	case TEXTYPE_DXT3:           return "DXT3";
	case TEXTYPE_DXT5:           return "DXT5";
	case TEXTYPE_ETC1:           return "ETC1";
	case TEXTYPE_COLORBUFFER:    return "COLORBUFFER";
	case TEXTYPE_COLORBUFFER16F: return "COLORBUFFER16F";
	case TEXTYPE_COLORBUFFER32F: return "COLORBUFFER32F";
	case TEXTYPE_DEPTHBUFFER16:  return "DEPTHBUFFER16";
	case TEXTYPE_DEPTHBUFFER24:  return "DEPTHBUFFER24";
	case TEXTYPE_DEPTHBUFFER24STENCIL8: return "DEPTHBUFFER24STENCIL8";
	default:                     return "unknown";
	}
}

// Returns MTLPixelFormatInvalid for anything this slice does not implement.
// PALETTE is expanded to BGRA by gl_textures.c before it reaches us (it hands
// over 4-byte pixels), so it shares the BGRA format.
static MTLPixelFormat mt_pixelformat(int textype, int *bytesperpixel)
{
	switch (textype)
	{
	case TEXTYPE_PALETTE:
	case TEXTYPE_BGRA:
		*bytesperpixel = 4; return MTLPixelFormatBGRA8Unorm;
	case TEXTYPE_RGBA:
		*bytesperpixel = 4; return MTLPixelFormatRGBA8Unorm;
	case TEXTYPE_ALPHA:
		*bytesperpixel = 1; return MTLPixelFormatA8Unorm;
	// METAL.md Phase 4a: the render-target formats. Never uploaded to (the pool
	// passes NULL pixels and Metal_Texture_Upload no-ops), so bytesperpixel is
	// informational.
	case TEXTYPE_COLORBUFFER:
		*bytesperpixel = 4; return MTLPixelFormatBGRA8Unorm;
	case TEXTYPE_COLORBUFFER16F:
		*bytesperpixel = 8; return MTLPixelFormatRGBA16Float;
	case TEXTYPE_COLORBUFFER32F:
		*bytesperpixel = 16; return MTLPixelFormatRGBA32Float;
	// All three depth types map to Depth32Float: Apple GPUs have no D24, the
	// engine never reads stencil (R_SetStencil has zero call sites), and the
	// plain format keeps Depth32Float_Stencil8's enum value 260 -- which
	// overflowed the pipeline key's old byte field -- out of the system
	// entirely. NEAREST-only filtering is already forced upstream
	// (gl_textures.c rewrites the flags for every TEXTYPE_DEPTHBUFFER*).
	case TEXTYPE_DEPTHBUFFER16:
	case TEXTYPE_DEPTHBUFFER24:
	case TEXTYPE_DEPTHBUFFER24STENCIL8:
		*bytesperpixel = 4; return MTLPixelFormatDepth32Float;
	default:
		*bytesperpixel = 0; return MTLPixelFormatInvalid;
	}
}

// a render target lives in private memory and needs the RenderTarget usage bit;
// depth targets are never sampled by the 4a shader set but ShaderRead costs
// nothing and Phase 6's kernel fog wants to read scene depth eventually
static qbool mt_isrendertarget(int textype, int flags)
{
	switch (textype)
	{
	case TEXTYPE_COLORBUFFER:
	case TEXTYPE_COLORBUFFER16F:
	case TEXTYPE_COLORBUFFER32F:
	case TEXTYPE_DEPTHBUFFER16:
	case TEXTYPE_DEPTHBUFFER24:
	case TEXTYPE_DEPTHBUFFER24STENCIL8:
		return true;
	default:
		return (flags & TEXF_RENDERTARGET) != 0;
	}
}

// ---------------------------------------------------------------------------
// samplers

extern cvar_t gl_texture_anisotropy;
extern int  gl_filter_min, gl_filter_mag;
extern qbool gl_filter_force;

// The two GL enums GL_SetupTextureParameters (gl_textures.c) would hand
// glTexParameteri for these flags. A DIRECT TRANSCRIPTION of that function's
// three-armed tree, arm for arm and in the same order, so the two can be read
// side by side -- not a paraphrase.
//
// It is a transcription because the paraphrase it replaced got the tree wrong
// in three independent ways, each of which the old single `nearest` boolean
// hid by construction. All three are recorded because only the first was
// suspected before the bed was built:
//
//  1. gl_filter_force INVERTED. GL gates BOTH force-flag arms on
//     !gl_filter_force and then falls through to the else arm -- so a forced
//     mode OVERRIDES TEXF_FORCENEAREST/TEXF_FORCELINEAR and uses
//     gl_filter_min/_mag. The old predicate required !gl_filter_force in BOTH
//     of its clauses, so a forced mode made every Metal texture LINEAR
//     unconditionally: the exact inverse for `gl_texturemode GL_NEAREST
//     force`. Measured, not reasoned -- that command moves 47.3% of the GL
//     frame and 0 of 307200 Metal pixels. It was a total no-op on this path.
//  2. gl_filter_mag was NEVER CONSULTED. It is an independent enum, not a
//     function of gl_filter_min: modes[4] is {"GL_NEAREST_MIPMAP_LINEAR",
//     GL_NEAREST_MIPMAP_LINEAR, GL_NEAREST}, so GL magnifies NEAREST while the
//     old code -- testing only gl_filter_min against the two nearest enums --
//     magnified LINEAR.
//  3. The MIP filter rides INSIDE the minification enum, so it is not a
//     function of "is this nearest". Two cases the old code could not express:
//     GL_NEAREST and GL_LINEAR carry no mip term, so selecting either as the
//     texture mode disables mipmapping on a TEXF_MIPMAP texture (reachable
//     with a plain UNFORCED `gl_texturemode GL_LINEAR`); and GL's FORCELINEAR
//     arm picks its own mip filter from gl_filter_min, which the old code
//     hardwired to linear.
static void mt_glfilters(int flags, int *outmin, int *outmag)
{
	if (!gl_filter_force && (flags & TEXF_FORCENEAREST))
	{
		*outmin = (flags & TEXF_MIPMAP) ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST;
		*outmag = GL_NEAREST;
	}
	else if (!gl_filter_force && (flags & TEXF_FORCELINEAR))
	{
		if (flags & TEXF_MIPMAP)
			*outmin = (gl_filter_min == GL_NEAREST_MIPMAP_LINEAR || gl_filter_min == GL_LINEAR_MIPMAP_LINEAR)
			        ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST;
		else
			*outmin = GL_LINEAR;
		*outmag = GL_LINEAR;
	}
	else
	{
		*outmin = (flags & TEXF_MIPMAP) ? gl_filter_min : gl_filter_mag;
		*outmag = gl_filter_mag;
	}
}

// The min/mag half of a GL filter enum.
static MTLSamplerMinMagFilter mt_minmagfilter(int glfilter)
{
	switch (glfilter)
	{
	case GL_NEAREST:
	case GL_NEAREST_MIPMAP_NEAREST:
	case GL_NEAREST_MIPMAP_LINEAR:
		return MTLSamplerMinMagFilterNearest;
	default:
		return MTLSamplerMinMagFilterLinear;
	}
}

// The mip half of a GL MINIFICATION enum. The default arm is the load-bearing
// one: GL_NEAREST and GL_LINEAR carry no mip term at all, and in GL selecting
// one as GL_TEXTURE_MIN_FILTER disables mipmapping outright however many levels
// the texture actually has. MTLSamplerMipFilterNotMipmapped says the same thing
// -- the texture keeps its chain and the sampler never walks it.
static MTLSamplerMipFilter mt_mipfilter(int glminfilter, int flags)
{
	if (!(flags & TEXF_MIPMAP))
		return MTLSamplerMipFilterNotMipmapped;
	switch (glminfilter)
	{
	case GL_NEAREST_MIPMAP_NEAREST:
	case GL_LINEAR_MIPMAP_NEAREST:
		return MTLSamplerMipFilterNearest;
	case GL_NEAREST_MIPMAP_LINEAR:
	case GL_LINEAR_MIPMAP_LINEAR:
		return MTLSamplerMipFilterLinear;
	default:
		return MTLSamplerMipFilterNotMipmapped;
	}
}

// Keyed on the sampler's OWN RESOLVED STATE rather than on the inputs that
// produced it, so the cache cannot under-key: everything the decision above
// reads -- the TEXF_ bits, gl_filter_min, gl_filter_mag, gl_filter_force --
// can only reach the cache through these arguments, and adding a term to the
// decision cannot leave the key behind. The version this replaced hand-listed
// a subset of the INPUTS and omitted gl_filter_mag entirely, so two textures GL
// would magnify differently shared one sampler.
//
// TEXF_MIPMAP is deliberately no longer a key bit of its own. It reaches the
// key through mipf and aniso, and where a mipped and an unmipped texture now
// collide -- `gl_texturemode GL_NEAREST` with anisotropy 1, where both resolve
// to nearest/nearest/NotMipmapped -- the two sampler states are identical in
// every field, so sharing one is correct rather than merely tolerable.
static int mt_sampler_key(int flags, MTLSamplerMinMagFilter minf, MTLSamplerMinMagFilter magf,
                          MTLSamplerMipFilter mipf, int aniso)
{
	return   ((flags & TEXF_CLAMP) ? 1 : 0)
	       | ((minf == MTLSamplerMinMagFilterNearest) ? 2 : 0)
	       | ((magf == MTLSamplerMinMagFilterNearest) ? 4 : 0)
	       | ((int)mipf << 3)   /* NotMipmapped/Nearest/Linear, 0..2 -- bits 3-4 */
	       | (aniso << 8);
}

static id<MTLSamplerState> mt_sampler_for(int flags)
{
	// GL gates anisotropy on TEXF_MIPMAP too (GL_SetupTextureParameters), so
	// this term is unchanged from the version above; it is hoisted only so the
	// key and the descriptor cannot disagree about it.
	int aniso = (flags & TEXF_MIPMAP) ? bound(1, gl_texture_anisotropy.integer, 16) : 1;
	int i, key, glmin, glmag;
	MTLSamplerMinMagFilter minf, magf;
	MTLSamplerMipFilter mipf;
	id<MTLDevice> dev = (__bridge id<MTLDevice>)VID_Metal_GetDevice();
	MTLSamplerDescriptor *sd;

	if (!dev)
		return nil;
	mt_glfilters(flags, &glmin, &glmag);
	minf = mt_minmagfilter(glmin);
	magf = mt_minmagfilter(glmag);
	mipf = mt_mipfilter(glmin, flags);
	key  = mt_sampler_key(flags, minf, magf, mipf, aniso);

	for (i = 0; i < mt_numsamplers; i++)
		if (mt_samplers[i].key == key)
			return mt_samplers[i].state;
	if (mt_numsamplers >= MT_MAX_SAMPLERS)
	{
		// One-shot, like every other refusal in this file: this sits on the
		// per-draw bind path, and un-guarded it would print per lookup for the
		// rest of the session if the cache ever filled.
		static int mt_warned_samplerfull;
		if (!mt_warned_samplerfull)
		{
			mt_warned_samplerfull = 1;
			Con_Printf(CON_ERROR "Metal_Texture: sampler cache full\n");
		}
		return mt_samplers[0].state;
	}

	sd = [[MTLSamplerDescriptor alloc] init];
	sd.minFilter = minf;
	sd.magFilter = magf;
	sd.mipFilter = mipf;
	sd.sAddressMode = (flags & TEXF_CLAMP) ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
	sd.tAddressMode = sd.sAddressMode;
	sd.rAddressMode = sd.sAddressMode;
	sd.maxAnisotropy = aniso;

	mt_samplers[mt_numsamplers].key = key;
	mt_samplers[mt_numsamplers].state = [dev newSamplerStateWithDescriptor:sd];
	return mt_samplers[mt_numsamplers++].state;
}

void Metal_Texture_InvalidateSamplers(void)
{
	int i;
	for (i = 0; i < mt_numsamplers; i++)
		mt_samplers[i].state = nil;
	mt_numsamplers = 0;
}

// ---------------------------------------------------------------------------
// textures

int Metal_Texture_Create(int textype, int width, int height, int depth, int sides, int flags)
{
	id<MTLDevice> dev = (__bridge id<MTLDevice>)VID_Metal_GetDevice();
	MTLTextureDescriptor *td;
	MTLPixelFormat pf;
	int bpp, slot;

	if (!dev)
		return 0;

	pf = mt_pixelformat(textype, &bpp);
	if (pf == MTLPixelFormatInvalid)
	{
		// Loud, once, by name. A texture that quietly comes out wrong is the
		// failure mode this arc is arranged to avoid.
		static int reported[64];
		int idx = (textype >= 0 && textype < 64) ? textype : 0;
		if (!reported[idx])
		{
			reported[idx] = 1;
			Con_Printf(CON_ERROR "Metal_Texture: format %s is not implemented on the Metal path yet "
			           "(METAL.md Phase 4 owes the world formats); this texture will be missing\n",
			           mt_textype_name(textype));
		}
		return 0;
	}
	// Cubemaps stay refused; 3D landed with METAL.md Phase 6, which needs two of
	// them (the murk's noise volume and its baked world field) plus r_lavaboil's.
	// The two were ONE guard until 6-1 and are split rather than widened, so
	// enabling 3D cannot enable cubemaps by accident.
	if (sides > 1)
	{
		// Rate-limited, unlike the version this replaced, because it is NOT a
		// rare event: gl_main_start calls R_BuildWhiteCube and
		// R_BuildNormalizationCube unconditionally on both renderpaths, so this
		// fired twice on every Metal start and every vid_restart. Harmless --
		// USEREFLECTCUBE and USECUBEFILTER are both in the MSL negative list, so
		// a cubemap texnum of 0 can never reach a Metal draw -- but two errors
		// in a fresh log read as a regression in whatever landed most recently.
		static qbool reported;
		if (!reported)
		{
			reported = true;
			Con_Printf(CON_ERROR "Metal_Texture: cubemap textures are not implemented "
			           "(the two built-in cubes; both consumers are sentinelled off); "
			           "these textures will be missing\n");
		}
		return 0;
	}
	// A 3D RENDER-TARGET format is refused, and this is not tidiness. The tree
	// has four 3D producers, not the two Phase 6 needs: r_shadow.c's bouncegrid
	// asks for TEXTYPE_COLORBUFFER16F/32F as a volume WITH pixel data, and
	// mt_isrendertarget below would hand that MTLStorageModePrivate -- on which
	// replaceRegion is invalid. Widening the depth guard without this would have
	// turned a silent, harmless refusal into a validation failure the moment
	// anyone set r_shadow_bouncegrid (default 0, and dead in this fork's
	// configuration, which is exactly why no bed would have caught it).
	// The bouncegrid stays as dead as it was before 6-1, and now says so.
	if (depth > 1 && mt_isrendertarget(textype, flags))
	{
		// Once-guarded like the cubemap refusal above: the bounce grid rebuilds
		// on a timer, so an unguarded line here would scroll the console.
		static qbool rtreported;
		if (!rtreported)
		{
			rtreported = true;
			Con_Printf(CON_ERROR "Metal_Texture: 3D render-target formats are not implemented "
			           "(%s, %dx%dx%d); this texture will be missing\n",
			           mt_textype_name(textype), width, height, depth);
		}
		return 0;
	}
	if (mt_numtextures >= MT_MAX_TEXTURES)
	{
		Con_Printf(CON_ERROR "Metal_Texture: handle table full (%d)\n", MT_MAX_TEXTURES);
		return 0;
	}

	// reuse a freed slot before growing, so long sessions with many map loads
	// do not walk the table off the end
	for (slot = 1; slot < mt_numtextures; slot++)
		if (!mt_textures[slot].used)
			break;
	if (slot == mt_numtextures)
		mt_numtextures++;

	if (depth > 1)
	{
		// Built by hand because there is no texture3DDescriptor convenience
		// initialiser. The 2D arm below is left on its own initialiser rather
		// than folded into this one, so 6-1 cannot alter a single 2D descriptor.
		td = [[MTLTextureDescriptor alloc] init];
		td.textureType = MTLTextureType3D;
		td.pixelFormat = pf;
		td.width  = (NSUInteger)width;
		td.height = (NSUInteger)height;
		td.depth  = (NSUInteger)depth;
		// A 3D chain runs until ALL THREE axes reach 1, so the level count comes
		// from the largest of them -- not from max(width, height) as the 2D
		// initialiser computes. No 3D texture in this tree is mipmapped (both
		// murk volumes pass miplevel 0 and neither sets TEXF_MIPMAP), so this
		// arm is correct-by-construction rather than measured; it is written out
		// because a silently-short chain is the failure mode Phase 4a already
		// paid for once on the 2D side.
		if (flags & TEXF_MIPMAP)
		{
			int m = max(width, max(height, depth)), levels = 1;
			while (m > 1) { m >>= 1; levels++; }
			td.mipmapLevelCount = (NSUInteger)levels;
		}
		else
			td.mipmapLevelCount = 1;
	}
	else
	{
		td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf
		                                                        width:(NSUInteger)width
		                                                       height:(NSUInteger)height
		                                                    mipmapped:(flags & TEXF_MIPMAP) ? YES : NO];
	}
	if (mt_isrendertarget(textype, flags))
	{
		// the same usage/storage pair metal_backend.m's own screen texture uses
		// Plus the MetalFX scalers' queried output usage (2026-09-18). Phase 8-4
		// gave those bits to the backend's own screen texture because that was
		// the scaler's only destination; r_fxaa_post makes an ordinary pooled
		// target a destination too, and the TEMPORAL scaler asks for more than
		// the spatial one (0x7 against 0x5 here -- ShaderWrite). Without them the
		// encode refuses and the frame falls back SILENTLY to a correct but
		// un-antialiased path. Both are 0 when MetalFX is unavailable, so this is
		// the old value exactly on any machine without it.
		td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead
		         | (MTLTextureUsage)MetalFX_OutputTextureUsage()
		         | (MTLTextureUsage)MetalFX_TemporalOutputTextureUsage();
		td.storageMode = MTLStorageModePrivate;
	}
	else
	{
		td.usage = MTLTextureUsageShaderRead;
		td.storageMode = MTLStorageModeShared;   // unified memory: no staging blit needed
	}

	memset(&mt_textures[slot], 0, sizeof(mt_textures[slot]));
	mt_textures[slot].tex = [dev newTextureWithDescriptor:td];
	if (!mt_textures[slot].tex)
	{
		Con_Printf(CON_ERROR "Metal_Texture: creation failed (%dx%d %s)\n", width, height, mt_textype_name(textype));
		return 0;
	}
	mt_textures[slot].width = width;
	mt_textures[slot].height = height;
	mt_textures[slot].depth = depth;
	mt_textures[slot].sides = sides;
	mt_textures[slot].flags = flags;
	mt_textures[slot].textype = textype;
	mt_textures[slot].bytesperpixel = bpp;
	mt_textures[slot].used = true;
	return slot;
}

// Adopt a texture this layer did not create, so the renderer can bind it like
// any other. The RT sidecar's term buffer is the one caller (METAL.md Phase 5):
// rt_metal.m creates it against the SAME MTLDevice and keeps owning it, and ARC
// gives this table a strong reference of its own -- so Metal_Texture_Destroy
// drops the renderer's reference and never the sidecar's, whichever goes first.
//
// `flags` are TEXF_* and they are load-bearing rather than cosmetic: they are
// the sampler cache's entire key, which is where the GL path's glTexParameteri
// filtering choice has to land on this side. rt_metal.m's
// NEAREST-at-full-res-without-reproject predicate therefore survives as a
// sampler choice made here, exactly as rt_pair_ensure's comment promised.
//
// The format is CHECKED, not assumed. An adopted texture is the one kind that
// cannot be wrong at creation time, so the mismatch would surface as a shader
// reading plausible nonsense -- the failure mode this arc exists to refuse.
int Metal_Texture_Adopt(void *mtltex, int width, int height, int flags)
{
	id<MTLTexture> tex = (__bridge id<MTLTexture>)mtltex;
	int bpp, slot;

	if (!tex)
		return 0;
	if (tex.pixelFormat != mt_pixelformat(TEXTYPE_COLORBUFFER16F, &bpp))
	{
		Con_Printf(CON_ERROR "Metal_Texture_Adopt: expected %s, got MTLPixelFormat %u\n",
		           mt_textype_name(TEXTYPE_COLORBUFFER16F), (unsigned)tex.pixelFormat);
		return 0;
	}
	if (mt_numtextures >= MT_MAX_TEXTURES)
	{
		Con_Printf(CON_ERROR "Metal_Texture_Adopt: handle table full (%d)\n", MT_MAX_TEXTURES);
		return 0;
	}

	for (slot = 1; slot < mt_numtextures; slot++)
		if (!mt_textures[slot].used)
			break;
	if (slot == mt_numtextures)
		mt_numtextures++;

	memset(&mt_textures[slot], 0, sizeof(mt_textures[slot]));
	mt_textures[slot].tex = tex;
	mt_textures[slot].width = width;
	mt_textures[slot].height = height;
	mt_textures[slot].depth = 1;
	mt_textures[slot].sides = 1;
	mt_textures[slot].flags = flags;
	mt_textures[slot].textype = TEXTYPE_COLORBUFFER16F;
	mt_textures[slot].bytesperpixel = bpp;
	mt_textures[slot].used = true;
	return slot;
}

void Metal_Texture_Destroy(int handle)
{
	if (handle <= 0 || handle >= MT_MAX_TEXTURES || !mt_textures[handle].used)
		return;
	mt_textures[handle].tex = nil;
	mt_textures[handle].used = false;
}

static mt_tex_t *mt_get(int handle)
{
	if (handle <= 0 || handle >= MT_MAX_TEXTURES || !mt_textures[handle].used)
		return NULL;
	return &mt_textures[handle];
}

void *Metal_Texture_GetTexture(int handle)
{
	mt_tex_t *t = mt_get(handle);
	return t ? (__bridge void *)t->tex : NULL;
}

void *Metal_Texture_GetSampler(int handle)
{
	// The sampler is a function of the texture's TEXF_ flags plus the global
	// filter state, and mt_sampler_for already caches on exactly that -- so the
	// backend asks per texture and never has to know the rule.
	mt_tex_t *t = mt_get(handle);
	return t ? (__bridge void *)mt_sampler_for(t->flags) : NULL;
}

void *Metal_Texture_GetNullSampler(void)
{
	// A sampler for a texture unit that has NO texture. Metal's API validation
	// treats a declared-but-unbound sampler as a hard error and traps the
	// process, while a nil TEXTURE is defined behaviour (samples read zero) --
	// so the fix for that whole class is a real sampler at every unit, and the
	// texture is deliberately left nil so no pixel can change.
	//
	// mt_sampler_for(0) rather than a private descriptor: flags 0 is what an
	// ordinary unflagged texture asks for, so this shares the cache entry the
	// renderer already has rather than minting a second one, and it follows
	// gl_texturemode with everything else.
	return (__bridge void *)mt_sampler_for(0);
}

qbool Metal_Texture_UploadLevel(int handle, int level, const unsigned char *pixels, int width, int height, int depth)
{
	// METAL.md Phase 4a. The caller (R_UploadFullTexture's Metal arm) drives
	// this from the SAME CPU mip loop the GL arm uses -- Image_MipReduce32
	// output, level by level -- so both backends sample byte-identical mip
	// data. Deliberately NOT generateMipmapsForTexture: Metal's blit filter is
	// not Image_MipReduce32, and the difference would sit as a permanent bias
	// in every mipped world surface, squarely inside the parity budget.
	//
	// METAL.md Phase 6 (6-1) added `depth`. It is NOT cosmetic: the 2D-region
	// form below writes one z-slice, so before this the murk's 64x64x64 noise
	// volume would have uploaded its first slice and left the other 63
	// undefined -- an upload that "worked" with no error anywhere. The two
	// region forms are kept separate rather than unified on MTLRegionMake3D
	// with depth 1, so that no 2D upload's arguments change at 6-1.
	mt_tex_t *t = mt_get(handle);
	if (!t || !pixels || width <= 0 || height <= 0 || depth <= 0)
		return false;
	if (level < 0 || (NSUInteger)level >= t->tex.mipmapLevelCount)
		return false;
	if (depth > 1)
	{
		// bytesPerImage is the z stride. The engine's 3D buffers are plain
		// row-major x-fastest, z-slowest (see R_Volumetric_GetNoiseTexture's
		// ((z * SIZE + y) * SIZE + x) * 4 indexing), so one slice is exactly
		// bytesPerRow * height with no padding.
		[t->tex replaceRegion:MTLRegionMake3D(0, 0, 0, (NSUInteger)width, (NSUInteger)height, (NSUInteger)depth)
		          mipmapLevel:(NSUInteger)level
		                slice:0
		            withBytes:pixels
		          bytesPerRow:(NSUInteger)width * (NSUInteger)t->bytesperpixel
		        bytesPerImage:(NSUInteger)width * (NSUInteger)height * (NSUInteger)t->bytesperpixel];
	}
	else
	{
		[t->tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)width, (NSUInteger)height)
		          mipmapLevel:(NSUInteger)level
		            withBytes:pixels
		          bytesPerRow:(NSUInteger)width * (NSUInteger)t->bytesperpixel];
	}
	return true;
}

qbool Metal_Texture_Upload(int handle, const unsigned char *pixels)
{
	mt_tex_t *t = mt_get(handle);
	if (!t || !pixels)
		return false;
	return Metal_Texture_UploadLevel(handle, 0, pixels, t->width, t->height, t->depth);
}

qbool Metal_Texture_UploadPartial(int handle, const unsigned char *pixels, int x, int y, int width, int height)
{
	mt_tex_t *t = mt_get(handle);
	if (!t || !pixels)
		return false;
	// Refused here rather than relied upon upstream. R_UploadPartialTexture does
	// Sys_Error on any non-2D texture before the renderpath switch is reached, so
	// this is unreachable today -- but that is an invariant owned by a distant
	// caller and stated only in another file, which is the shape 5-1 found four
	// times over and 5-6 closed a fifth. Without it, a 3D handle would write
	// slice 0 and return true.
	if (t->depth > 1)
		return false;
	if (x < 0 || y < 0 || width <= 0 || height <= 0 || x + width > t->width || y + height > t->height)
		return false;
	[t->tex replaceRegion:MTLRegionMake2D((NSUInteger)x, (NSUInteger)y, (NSUInteger)width, (NSUInteger)height)
	          mipmapLevel:0
	            withBytes:pixels
	          bytesPerRow:(NSUInteger)width * (NSUInteger)t->bytesperpixel];
	return true;
}

qbool Metal_Texture_Readback(int handle, unsigned char *out)
{
	mt_tex_t *t = mt_get(handle);
	if (!t || !out)
		return false;
	if (t->depth > 1)
	{
		// Same z-stride reasoning as the upload. Without the 3D form this
		// returned slice 0 only, which would have made the 6-1 probe compare
		// one slice against a whole volume and report a confident PASS on the
		// bytes it happened to look at.
		[t->tex getBytes:out
		     bytesPerRow:(NSUInteger)t->width * (NSUInteger)t->bytesperpixel
		   bytesPerImage:(NSUInteger)t->width * (NSUInteger)t->height * (NSUInteger)t->bytesperpixel
		      fromRegion:MTLRegionMake3D(0, 0, 0, (NSUInteger)t->width, (NSUInteger)t->height, (NSUInteger)t->depth)
		     mipmapLevel:0
		           slice:0];
		return true;
	}
	[t->tex getBytes:out
	     bytesPerRow:(NSUInteger)t->width * (NSUInteger)t->bytesperpixel
	      fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)t->width, (NSUInteger)t->height)
	     mipmapLevel:0];
	return true;
}

// ---------------------------------------------------------------------------
// 3D sampling probe (METAL.md Phase 6, slice 6-1)
//
// A readback proves the BYTES arrived; it cannot prove a shader can address
// them. For 3D that gap is the whole risk -- a collapsed, flipped or transposed
// z axis reads back perfectly and samples wrongly, and Phase 6-2's murk arm is
// built entirely on 3D sampling. So the probe samples for real.
//
// Deliberately self-contained: its own tiny pipeline off an inline MSL string,
// no MODE_ enum, no shader_msl.h entry. 6-2 owns the murk's arm and this must
// not pre-empt its shape. It DOES bind the renderer's own MTLSamplerState
// rather than a constexpr one, so mt_sampler_for's filter and address decisions
// are under test too -- which is what makes a NEAREST-vs-LINEAR slip visible.

static id<MTLComputePipelineState> mt_probe3d_pso;
// The device the PSO above was built against, compared and NEVER dereferenced.
//
// Re-minting on POINTER IDENTITY rather than on presence is the 5-1 rule for
// exactly this shape, and it is load-bearing here for a reason worth stating:
// Metal_Texture_Shutdown has ZERO callers in the tree (grep -- only its own
// declaration, definition and no-op stub), so nothing clears this on a
// vid_restart. A plain `if (mt_probe3d_pso) return` would therefore keep
// serving a pipeline built against a released MTLDevice across a
// metal -> gl -> metal round trip -- the same silent stale-device shape 5-6
// closed in RT_Metal_InitWithDevice. Smoke's run J does ONE restart, so only a
// second one bites, which is precisely why it needed writing down rather than
// waiting to be measured.
static void *mt_probe3d_dev;

static id<MTLComputePipelineState> mt_probe3d_pipeline(void)
{
	void *devptr = VID_Metal_GetDevice();
	id<MTLDevice> dev = (__bridge id<MTLDevice>)devptr;
	NSError *err = nil;
	id<MTLLibrary> lib;
	id<MTLFunction> fn;
	NSString *src =
		@"#include <metal_stdlib>\n"
		 "using namespace metal;\n"
		 "kernel void mt_probe3d(texture3d<float, access::sample> tex [[texture(0)]],\n"
		 "                       sampler smp [[sampler(0)]],\n"
		 "                       device const packed_float3 *coords [[buffer(0)]],\n"
		 "                       device float4 *out [[buffer(1)]],\n"
		 "                       uint i [[thread_position_in_grid]])\n"
		 "{\n"
		 "    out[i] = tex.sample(smp, float3(coords[i]));\n"
		 "}\n";

	if (mt_probe3d_pso && mt_probe3d_dev == devptr)
		return mt_probe3d_pso;
	mt_probe3d_pso = nil;   // a device change invalidates it
	if (!dev)
		return nil;
	lib = [dev newLibraryWithSource:src options:nil error:&err];
	if (!lib)
	{
		Con_Printf(CON_ERROR "Metal_Texture_Sample3D: probe shader failed to compile: %s\n",
		           err ? [[err localizedDescription] UTF8String] : "unknown");
		return nil;
	}
	fn = [lib newFunctionWithName:@"mt_probe3d"];
	if (!fn)
		return nil;
	mt_probe3d_pso = [dev newComputePipelineStateWithFunction:fn error:&err];
	if (!mt_probe3d_pso)
		Con_Printf(CON_ERROR "Metal_Texture_Sample3D: probe pipeline failed: %s\n",
		           err ? [[err localizedDescription] UTF8String] : "unknown");
	mt_probe3d_dev = mt_probe3d_pso ? devptr : NULL;
	return mt_probe3d_pso;
}

qbool Metal_Texture_Sample3D(int handle, const float *coords, float *out, int n)
{
	mt_tex_t *t = mt_get(handle);
	id<MTLDevice> dev = (__bridge id<MTLDevice>)VID_Metal_GetDevice();
	id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)VID_Metal_GetQueue();
	id<MTLComputePipelineState> pso;
	id<MTLSamplerState> smp;
	id<MTLBuffer> cbuf, obuf;
	id<MTLCommandBuffer> cb;
	id<MTLComputeCommandEncoder> enc;
	NSUInteger tg;

	if (!t || !coords || !out || n <= 0 || !dev || !queue)
		return false;
	if (t->depth <= 1)
		return false;   // this instrument is for volumes only
	pso = mt_probe3d_pipeline();
	smp = mt_sampler_for(t->flags);
	if (!pso || !smp)
		return false;

	cbuf = [dev newBufferWithBytes:coords length:(NSUInteger)n * 3 * sizeof(float)
	                       options:MTLResourceStorageModeShared];
	obuf = [dev newBufferWithLength:(NSUInteger)n * 4 * sizeof(float)
	                        options:MTLResourceStorageModeShared];
	if (!cbuf || !obuf)
		return false;

	cb = [queue commandBuffer];
	enc = [cb computeCommandEncoder];
	[enc setComputePipelineState:pso];
	[enc setTexture:t->tex atIndex:0];
	[enc setSamplerState:smp atIndex:0];
	[enc setBuffer:cbuf offset:0 atIndex:0];
	[enc setBuffer:obuf offset:0 atIndex:1];
	tg = pso.maxTotalThreadsPerThreadgroup;
	if (tg > (NSUInteger)n)
		tg = (NSUInteger)n;
	[enc dispatchThreads:MTLSizeMake((NSUInteger)n, 1, 1)
	  threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
	[enc endEncoding];
	[cb commit];
	[cb waitUntilCompleted];
	if (cb.error)
	{
		Con_Printf(CON_ERROR "Metal_Texture_Sample3D: dispatch failed: %s\n",
		           [[cb.error localizedDescription] UTF8String]);
		return false;
	}
	memcpy(out, [obuf contents], (size_t)n * 4 * sizeof(float));
	return true;
}

void Metal_Texture_Shutdown(void)
{
	int i;
	for (i = 1; i < mt_numtextures; i++)
	{
		mt_textures[i].tex = nil;
		mt_textures[i].used = false;
	}
	mt_numtextures = 1;
	mt_probe3d_pso = nil;
	mt_probe3d_dev = NULL;
	Metal_Texture_InvalidateSamplers();
}

#else // the feature is off

// Nothing here on purpose. metal_backend.h / metal_textures.h supply
// static-inline no-ops when USE_METAL_RENDERER is absent, so the stub bodies
// that used to live here would be a second, conflicting definition -- and a
// hand-kept parallel list of names, which is the duplication shape this project
// keeps getting burned by. The header owns the stubs; this file owns the real
// implementation and nothing else.

#endif
