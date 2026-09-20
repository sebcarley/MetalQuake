/*
QuakeM5 -- the Metal drawing backend (METAL.md Phase 3, slice 3). See
metal_backend.h for the division of labour with gl_backend.c, why this file
keeps its own state shadow, and what is deliberately refused.

Compiled only on macOS (makefile.inc gates the object, as for rt_metal.o,
vid_metal.o and metal_textures.o). ARC is on.
*/

#if defined(__APPLE__) && defined(USE_METAL_RENDERER)

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <stdatomic.h>
#include "quakedef.h"
#include "metal_backend.h"
#include "metal_textures.h"
#include "metal_fx.h"
#include "vid_metal.h"
// METAL.md Phase 6-2b: shader_msl.h's MODE_VOLUMETRICFOG arm splices
// DPD_DENSITY_MODEL from here, which is the same text shader_glsl.h's march
// compiles. Must precede the #include of shader_msl.h below.
#include "shader_density.h"

// The ten parallel attribute slots. DarkPlaces has no interleaved vertex
// struct: R_Mesh_PrepareVertices_* re-establishes all ten every call, each with
// its own pointer, stride and buffer, which maps one-to-one onto ten Metal
// vertex buffer bindings. Slice 4's uniforms go above these, at index 16.
#define MB_NUMATTRIBS       10
#define MB_ATTRIB_POSITION  0
#define MB_ATTRIB_COLOR     1
#define MB_ATTRIB_TEXCOORD0 2
#define MB_BUFFERINDEX_PARAMS 16

// 2D needs exactly one. The surface shader will want more at Phase 4, but a
// bind above this is a bug worth hearing about rather than silently dropping.
#define MB_MAX_TEXUNITS 16

// program ids. Slice 3 has no ported shader, so every draw that arrives
// through the normal path gets the sentinel -- magenta, never black, which is
// Phase 3's own acceptance requirement: an unported arm must be unmistakable on
// screen rather than looking like geometry that failed to draw.
#define MB_PROGRAM_SENTINEL 0
#define MB_PROGRAM_PROBE2D  1
#define MB_PROGRAM_CLEAR    2
#define MB_PROGRAM_PRESENT  3
#define MB_PROGRAM_BLIT     4    // METAL.md Phase 7-5: the format-converting copy
#define MB_PROGRAM_FIRSTREAL 5   // ids at or above this index mb_programs[]

// ---------------------------------------------------------------------------
// the shaders
//
// Deliberately NOT the ported uber-shader -- that is slice 4, and it arrives
// with shader_msl.h and the reflection-driven uniform machinery. What is here
// is the minimum that lets the drawing machinery be exercised end to end and
// proven by readback: a passthrough that reads the position, colour and
// texcoord0 slots and multiplies the colour by the texture, a magenta sentinel,
// and a vertex_id-generated triangle for mid-pass clears.
//
// The passthrough takes CLIP-SPACE positions and applies no matrix, so slice 3
// exercises no uniform path at all. That is the honest boundary between this
// slice and the next.

static const char *kBackendSrc =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"\n"
"struct mb_vin\n"
"{\n"
"	float4 position  [[attribute(0)]];\n"
"	float4 color     [[attribute(1)]];\n"
"	float4 texcoord0 [[attribute(2)]];\n"
"};\n"
"\n"
"struct mb_vout\n"
"{\n"
"	float4 position [[position]];\n"
"	float4 color;\n"
"	float2 texcoord0;\n"
"};\n"
"\n"
"vertex mb_vout mb_pass_v(mb_vin in [[stage_in]])\n"
"{\n"
"	mb_vout o;\n"
"	o.position  = float4(in.position.xyz, 1.0);\n"
"	o.color     = in.color;\n"
"	o.texcoord0 = in.texcoord0.xy;\n"
"	return o;\n"
"}\n"
"\n"
"fragment float4 mb_probe_f(mb_vout in [[stage_in]],\n"
"                           texture2d<float> tex [[texture(0)]],\n"
"                           sampler smp [[sampler(0)]])\n"
"{\n"
"	return in.color * tex.sample(smp, in.texcoord0);\n"
"}\n"
"\n"
"fragment float4 mb_sentinel_f(mb_vout in [[stage_in]])\n"
"{\n"
"	// METAL.md Phase 3: an unported shader mode is magenta, never black.\n"
"	return float4(1.0, 0.0, 1.0, 1.0);\n"
"}\n"
"\n"
"struct mb_clearparams\n"
"{\n"
"	float4 color;\n"
"	float  depth;\n"
"};\n"
"\n"
"vertex float4 mb_clear_v(uint vid [[vertex_id]],\n"
"                         constant mb_clearparams &p [[buffer(16)]])\n"
"{\n"
"	// one oversized triangle covering the whole viewport; the scissor rect\n"
"	// confines it, which is what makes a mid-pass scissored clear correct\n"
"	float2 c = float2((vid == 2u) ? 3.0 : -1.0, (vid == 1u) ? 3.0 : -1.0);\n"
"	return float4(c, p.depth, 1.0);\n"
"}\n"
"\n"
"fragment float4 mb_clear_f(constant mb_clearparams &p [[buffer(16)]])\n"
"{\n"
"	return p.color;\n"
"}\n"
"\n"
"struct mb_present_out\n"
"{\n"
"	float4 position [[position]];\n"
"	float2 uv;\n"
"};\n"
"\n"
"vertex mb_present_out mb_present_v(uint vid [[vertex_id]])\n"
"{\n"
"	float2 c = float2((vid == 2u) ? 3.0 : -1.0, (vid == 1u) ? 3.0 : -1.0);\n"
"	mb_present_out o;\n"
"	o.position = float4(c, 0.0, 1.0);\n"
"	// THE one v-flip in the whole renderer. The screen texture holds a GL-layout\n"
"	// image (row 0 at the bottom) because R_Viewport_InitOrtho's Metal arm negates\n"
"	// the projection's y row; the drawable is displayed y-down. Sampling with\n"
"	// uv.y = (c.y+1)/2 rather than (1-c.y)/2 is the flip, and it happens here so\n"
"	// that every other coordinate in the engine keeps GL's numerics.\n"
"	o.uv = float2((c.x + 1.0) * 0.5, (c.y + 1.0) * 0.5);\n"
"	return o;\n"
"}\n"
"\n"
"fragment float4 mb_present_f(mb_present_out in [[stage_in]],\n"
"                             texture2d<float> src [[texture(0)]])\n"
"{\n"
"	// nearest, so the present is byte-exact when the drawable and the screen\n"
"	// texture agree in size -- which makes a readback of either comparable\n"
"	constexpr sampler s(filter::nearest, address::clamp_to_edge);\n"
"	return src.sample(s, in.uv);\n"
"}\n"
"\n"
"struct mb_blitparams\n"
"{\n"
"	float4 uvrect;   // xy = source origin, zw = source extent, normalised\n"
"};\n"
"\n"
"vertex mb_present_out mb_blit_v(uint vid [[vertex_id]],\n"
"                                constant mb_blitparams &p [[buffer(16)]])\n"
"{\n"
"	float2 c = float2((vid == 2u) ? 3.0 : -1.0, (vid == 1u) ? 3.0 : -1.0);\n"
"	mb_present_out o;\n"
"	o.position = float4(c, 0.0, 1.0);\n"
"	// NO V-FLIP, and that is the whole difference from mb_present_v above.\n"
"	// Both sides of a copy hold GL-layout images, so row r of the source\n"
"	// belongs at row r of the destination -- a straight index copy, exactly\n"
"	// what the blit-encoder path this stands in for does. Metal rasterises\n"
"	// NDC y=+1 at the viewport's FIRST row, so v runs down from the rect's\n"
"	// origin as c.y falls, which is (1 - c.y) * 0.5 rather than the present\n"
"	// pass's (c.y + 1) * 0.5.\n"
"	o.uv = p.uvrect.xy + float2((c.x + 1.0) * 0.5, (1.0 - c.y) * 0.5) * p.uvrect.zw;\n"
"	return o;\n"
"}\n"
"\n"
"fragment float4 mb_blit_f(mb_present_out in [[stage_in]],\n"
"                          texture2d<float> src [[texture(0)]])\n"
"{\n"
"	constexpr sampler s(filter::nearest, address::clamp_to_edge);\n"
"	return src.sample(s, in.uv);\n"
"}\n";

typedef struct mb_clearparams_s
{
	float color[4];
	float depth;
	float pad[3];
}
mb_clearparams_t;

// ---------------------------------------------------------------------------
// the state shadow
//
// See metal_backend.h for why this cannot be gl_backend.c's gl_state: the three
// vertex-pointer setters record NOTHING under RENDERPATH_METAL, and the vertex
// layout is exactly what the pipeline key is built from.

typedef struct mb_attrib_s
{
	int         enabled;
	int         components;
	int         gltype;        // bit 31 set means unnormalised
	size_t      stride;
	int         bufferhandle;
	size_t      offset;
	const void *pointer;       // client-side fallback, no buffer
}
mb_attrib_t;

typedef struct mb_state_s
{
	int   blendfunc1, blendfunc2;
	int   blendenable;
	int   blendop;					///< dpblendop_t; 0 = ADD, the state everything else assumes
	int   depthmask;
	int   depthtest;
	int   depthfunc;
	float depthrange[2];
	int   stencilenable, stencilwritemask, stencilfail, stencilzfail, stencilzpass;
	int   stencilcompare, stencilref, stencilcomparemask;
	float polygonoffset[2];
	int   cullface, cullenable;
	int   alphatocoverage;
	int   colormask;
	float color4f[4];
	int   scissortest;
	int   scissor[4];
	int   viewport[4];
	int   texunit[MB_MAX_TEXUNITS];
	mb_attrib_t attrib[MB_NUMATTRIBS];
	int   program;
}
mb_state_t;

static mb_state_t mb_state;

// encoder lifetime, defined with the frame machinery below
static void mb_end_encoder(void);

// ---------------------------------------------------------------------------
// device-side objects

static id<MTLDevice>              mb_dev;
static id<MTLCommandQueue>        mb_queue;
static id<MTLLibrary>             mb_lib;
static id<MTLCommandBuffer>       mb_cb;
static id<MTLRenderCommandEncoder> mb_enc;

// METAL_FRAMEMS=1 (see Metal_Backend_EndFrame): the presenting command
// buffer's GPU time, accumulated from its completion handler on the GPU
// callback thread and read on the main thread every 120 frames.
static int mb_framems;
static unsigned int mb_framems_frames;
static _Atomic unsigned long long mb_framems_ns;
static _Atomic unsigned long long mb_framems_n;
// METAL_FRAMEMS=2: one timed region per frame (Metal_Backend_ProfileRegion)
static _Atomic unsigned long long mb_regionms_ns;
static _Atomic unsigned long long mb_regionms_n;
static const char *mb_region_label = "";
static int mb_region_open;

// THE FRAME POOL (2026-08-09). An SDL app's C main loop drains no autorelease
// pool, so every AUTORELEASED Metal object this file creates per frame --
// render pass descriptors, render encoders, command buffers, the drawable --
// stayed referenced by the root pool until quit. In a release build that is a
// few hundred bytes a frame and hid inside every soak's noise; under Xcode's
// Metal API validation each captive encoder is a ~98 KB
// MTLDebugRenderCommandEncoder, and Seb's 24-minute Cmd+R session reached
// 33.5 GB -- measured LIVE on the running process: 333,179 encoders,
// 666,364 render pass descriptors, 120,019 command buffers, all still
// alive. rt_metal.m has always pooled its own per-frame work, which is why
// the sidecar never showed this and why months of GL+sidecar sessions were
// clean; the renderer path never had a pool. Drain-and-renew at frame start
// rather than a push/pop pair, so a frame that never reaches EndFrame (no
// drawable, restart mid-frame) cannot stack pools; Shutdown drains the last
// one. These two functions are what @autoreleasepool compiles to -- the
// block form cannot span a frame across function boundaries.
extern void *objc_autoreleasePoolPush(void);
extern void  objc_autoreleasePoolPop(void *);
static void *mb_framepool;

static id<MTLTexture> mb_targetcolor;
static id<MTLTexture> mb_targetdepth;
static int            mb_targetwidth, mb_targetheight;

// the persistent GL-layout render target the whole frame is drawn into, and
// its depth (METAL.md Phase 4a). Depth32Float, not Depth32Float_Stencil8:
// R_SetStencil has zero call sites in this tree, and the plain format keeps the
// pipeline key clear of the 260-overflows-a-byte trap by construction.
static id<MTLTexture> mb_screentex;
static id<MTLTexture> mb_screendepth;
static int            mb_screenw, mb_screenh;

// which FBO handle the target pair currently belongs to; 0 = the screen pair.
// SetRenderTarget compares against this so the no-change case -- called from
// R_Mesh_Start/Finish constantly -- stays a single int compare.
static int mb_currentfbo;

// ---------------------------------------------------------------------------
// the framebuffer table (METAL.md Phase 4a)
//
// GL FBOs are persistent server objects with attachments bound once at
// creation; Metal rebuilds a MTLRenderPassDescriptor per encoder. So an "FBO"
// here is just a record of texture HANDLES (metal_textures.h handles, exactly
// as texnum carries them), resolved to id<MTLTexture> at select time. Handles,
// not id<>s, so a texture freed and reallocated cannot leave a dangling
// retained object inside a forgotten fbo record.

typedef struct mb_fbo_s
{
	int   colorhandle[4];
	int   depthhandle;
	qbool used;
}
mb_fbo_t;

#define MB_MAX_FBOS 64
static mb_fbo_t mb_fbos[MB_MAX_FBOS];   // slot 0 reserved: fbo 0 is the screen pair

// Install fbo's attachments as the current target pair. Returns false for an
// unknown or empty handle (caller falls back to the screen pair).
static qbool mb_fbo_select(int fbo)
{
	mb_fbo_t *f;
	id<MTLTexture> color, depth;
	if (fbo <= 0 || fbo >= MB_MAX_FBOS || !mb_fbos[fbo].used)
		return false;
	f = &mb_fbos[fbo];
	color = f->colorhandle[0] ? (__bridge id<MTLTexture>)Metal_Texture_GetTexture(f->colorhandle[0]) : nil;
	depth = f->depthhandle    ? (__bridge id<MTLTexture>)Metal_Texture_GetTexture(f->depthhandle)    : nil;
	if (!color && !depth)
		return false;
	mb_currentfbo   = fbo;
	mb_targetcolor  = color;
	mb_targetdepth  = depth;
	mb_targetwidth  = color ? (int)color.width  : (depth ? (int)depth.width  : 0);
	mb_targetheight = color ? (int)color.height : (depth ? (int)depth.height : 0);
	return true;
}

static int   mb_pendingclearmask;
static float mb_pendingclearcolor[4];
static float mb_pendingcleardepth;
static int   mb_pendingclearstencil;

static qbool mb_started;
static int   mb_warned_fbo, mb_warned_copytex, mb_warned_clientarray;
static int   mb_warned_texunit, mb_warned_stencilclear, mb_warned_readback;

static cvar_t r_metal_forceencoderrestart = {CF_CLIENT, "r_metal_forceencoderrestart", "0", "METAL.md Phase 3: end and restart the render encoder before every draw. Metal encoders start stateless, so this is the instrument for the state-replay bug class -- r_metal_drawprobe runs itself with it off and on and requires the two readbacks to be byte-identical"};

// ---------------------------------------------------------------------------
// the pipeline-state cache
//
// The shape is Phase 0's (vid_metal.m:55-146): a plain-C key struct, compared
// whole, backing a flat array. Extended here with the vertex-layout hash, the
// blend equation, alpha-to-coverage and the depth attachment format, and given
// a hash index because METAL.md estimates 300-800 live pipelines against Phase
// 0's 64-entry linear scan.
//
// Only what Metal must BAKE is in the key. Cull, scissor, viewport, depth bias
// and the depth-stencil state are encoder-level and deliberately absent -- that
// is what keeps the pipeline count in the hundreds rather than the thousands.

typedef struct mb_pipelinekey_s
{
	unsigned int   layouthash;
	unsigned short program;
	unsigned short blendsrc, blenddst;
	// blendop is a dpblendop_t (gl_backend.h), widened in place from the old
	// blendsubtract boolean -- 0 and 1 keep their old meanings exactly, so a
	// zeroed key is still ADD and every memset stays correct.
	unsigned char  blendenable, blendop, colormask, alphatocoverage;
	// unsigned short, NOT char: MTLPixelFormatDepth32Float_Stencil8 is 260 and a
	// byte narrowing would silently fold it to 4, declaring a pipeline depth
	// format that disagrees with the attached texture -- a validation failure at
	// best. 4a only uses Depth32Float (252), which would fit, but the trap class
	// costs two bytes to delete.
	unsigned short colorformat, depthformat;
	unsigned char  samplecount;
	unsigned char  pad[1];
}
mb_pipelinekey_t;

#define MB_PSO_MAX  1024
#define MB_PSO_HASH 512

typedef struct mb_pso_s
{
	mb_pipelinekey_t key;
	id<MTLRenderPipelineState> pso;
	int next;   // 1-based, 0 = end of chain
}
mb_pso_t;

static mb_pso_t mb_psos[MB_PSO_MAX];
static int      mb_numpsos;                 // 1-based count; slot 0 unused
static int      mb_psohash[MB_PSO_HASH];    // 1-based index, 0 = empty
static int      mb_psofull;

// the depth-stencil state cache. Small by construction -- 2D uses one entry.
typedef struct mb_dsskey_s
{
	unsigned char depthtest, depthwrite, depthfunc, stencilenable;
	unsigned char sfail, szfail, szpass, scompare;
	unsigned char writemask, comparemask, ref, pad;
}
mb_dsskey_t;

#define MB_DSS_MAX 64
static mb_dsskey_t mb_dsskeys[MB_DSS_MAX];
static id<MTLDepthStencilState> mb_dsss[MB_DSS_MAX];
static int mb_numdsss;

// ---------------------------------------------------------------------------
// the shader programs
//
// One MSL library per (mode, permutation), compiled at runtime because the
// offline Metal toolchain is not installed. Reflection is taken once at compile
// time from a CANONICAL pipeline -- the uniform struct's layout cannot depend on
// blend state or attachment format, so there is no need to reflect every real
// pipeline the cache later builds.

static const char *builtinmslstrings[] =
{
#include "shader_msl.h"
	0
};

// 256, from 128 on 2026-09-12. Both stages' bindings fill ONE table and each
// stage reflects the whole constant struct, so before the dedupe below every
// member was recorded twice and the murk (mode 17, ~62 members) reached 128
// names with its TEXTURES still to come -- and mb_addreflected dropped them with
// a bare break. Nothing said so: the C side saw no textures, bound none, and
// Metal API validation aborted the first murk draw ("incorrect type of texture
// ... bound at index 1"). Seb's Cmd+R crash-looped on it the day the water
// redesign added three members to the struct. The table now refuses LOUDLY.
#define MB_MAX_UNIFORMS 256
// The staging block for the constant struct. It must be at least as large as the
// BIGGEST struct any compiled permutation declares, because the draw binds
// `prog->uniformbytes` of it and Metal's validation layer refuses a bind shorter
// than what the shader can read -- an abort, not a warning.
//
// 1024 was the original figure and it very nearly shipped a crash. The murk
// (mode 17) at Seb's own configuration -- volumetrics + the RT fog kernel +
// rt_metal_liquids + analytic gamma -- reached 1008 bytes by 2026-08-10, and F3's
// two new float4s (VolumetricSwirl, WaterParams) took it to 1040. Nothing headless
// caught it: `make sdl-release` has API validation OFF, where the clamp below just
// binds a short buffer and the shader reads past it with no visible effect, while
// Seb's Cmd+R (Xcode Debug, validation ON) aborts at the first murk draw. The
// per-draw cost does not change with this number -- setVertexBytes copies
// uniformbytes, not the array -- so the only price is static memory.
//
// 4096 is not another arbitrary bump: it is the architectural ceiling of this
// mechanism, since setVertex/FragmentBytes is limited to 4 KB. A struct that ever
// exceeds THIS has to move to a pooled MTLBuffer; it cannot be fixed by raising
// the number again.
#define MB_UNIFORM_MAXBYTES 4096

// A texture binding's "location". Textures are not struct members, so they have
// no byte offset -- but the sampler-assignment block in gl_rmain.c tests
// `loc >= 0` to decide a texture is present, so they need a location that is
// positive and unmistakable. The high bit set is both.
#define MB_LOC_TEXTURE 0x40000000

typedef struct mb_uniform_s
{
	char name[64];
	int  loc;      // byte offset, or MB_LOC_TEXTURE | index
}
mb_uniform_t;

typedef struct mb_program_s
{
	unsigned int mode;
	uint64_t     permutation;
	id<MTLLibrary>  lib;
	id<MTLFunction> vfunc, ffunc;
	mb_uniform_t uniforms[MB_MAX_UNIFORMS];
	int    numuniforms;
	size_t uniformbytes;
	// THIS PROGRAM'S constant block, staged between draws (2026-09-13). It used
	// to be ONE array shared by every program, which the comment on it called
	// "the same semantics as GL's per-program uniform state, without the state"
	// -- and that is exactly what it was not: a member a program never writes
	// read whatever OTHER program last wrote at that byte offset. Every surface
	// mode writes nearly everything per batch, so it held for months; the
	// rtlight pass (mode 12) writes none of the optional-effect uniforms, and
	// on Metal it drew with another mode's LavaParams / LiquidFade / WaterScreen
	// bytes armed -- the flash and beam_lit vantages read 2.1 / 5.1 against GL
	// with both controls byte-clean. GL keeps uniforms PER PROGRAM, zero until
	// written; so does this now.
	unsigned char uniformdata[MB_UNIFORM_MAXBYTES];
	qbool  used;
}
mb_program_t;

#define MB_MAX_PROGRAMS 256
static mb_program_t mb_programs[MB_MAX_PROGRAMS];
static int mb_numprograms = MB_PROGRAM_FIRSTREAL;   // ids below this are the built-ins

// R_Shader_Uniform* writes into the CURRENT program's own block (mb_program_t
// .uniformdata, GL's per-program semantics); each draw snapshots that block with
// setVertexBytes/setFragmentBytes, which copy, so no double-buffering is needed.
// The built-in programs (ids below MB_PROGRAM_FIRSTREAL, and the sentinel) carry
// their parameters some other way; a write while one of those is current lands
// in this scratch block and nothing reads it.
static unsigned char mb_uniformscratch[MB_UNIFORM_MAXBYTES];

static mb_program_t *mb_programfor(int id)
{
	if (id < MB_PROGRAM_FIRSTREAL || id >= MB_MAX_PROGRAMS || !mb_programs[id].used)
		return NULL;
	return &mb_programs[id];
}
static unsigned char *mb_uniformblock(void)
{
	mb_program_t *p = mb_programfor(mb_state.program);
	return p ? p->uniformdata : mb_uniformscratch;
}

// ---------------------------------------------------------------------------
// the buffer table. Handles, not pointers -- see metal_backend.h.

typedef struct mb_buf_s
{
	id<MTLBuffer> buf;
	size_t size;
	qbool  used;
}
mb_buf_t;

#define MB_MAX_BUFFERS 8192
static mb_buf_t mb_bufs[MB_MAX_BUFFERS];
static int      mb_numbufs = 1;   // slot 0 reserved as "no buffer"

static id<MTLBuffer> mb_bufferfor(int handle)
{
	if (handle <= 0 || handle >= MB_MAX_BUFFERS || !mb_bufs[handle].used)
		return nil;
	return mb_bufs[handle].buf;
}

// the private scratch target r_metal_drawprobe renders into
static id<MTLTexture> mb_probetarget;
static id<MTLTexture> mb_probesaved;
static id<MTLTexture> mb_probesaveddepth;
static int mb_probew, mb_probeh, mb_probesavedw, mb_probesavedh, mb_probesavedfbo;

// ---------------------------------------------------------------------------
// GL enum translation
//
// quakedef.h pulls in glquake.h, so the mapping lives here rather than putting
// new code in the shipped GL file.

static MTLBlendFactor mb_blendfactor(int glfactor)
{
	switch (glfactor)
	{
	case GL_ZERO:                     return MTLBlendFactorZero;
	case GL_ONE:                      return MTLBlendFactorOne;
	case GL_SRC_COLOR:                return MTLBlendFactorSourceColor;
	case GL_ONE_MINUS_SRC_COLOR:      return MTLBlendFactorOneMinusSourceColor;
	case GL_DST_COLOR:                return MTLBlendFactorDestinationColor;
	case GL_ONE_MINUS_DST_COLOR:      return MTLBlendFactorOneMinusDestinationColor;
	case GL_SRC_ALPHA:                return MTLBlendFactorSourceAlpha;
	case GL_ONE_MINUS_SRC_ALPHA:      return MTLBlendFactorOneMinusSourceAlpha;
	case GL_DST_ALPHA:                return MTLBlendFactorDestinationAlpha;
	case GL_ONE_MINUS_DST_ALPHA:      return MTLBlendFactorOneMinusDestinationAlpha;
	case GL_SRC_ALPHA_SATURATE:       return MTLBlendFactorSourceAlphaSaturated;
	default:                          return MTLBlendFactorOne;
	}
}

static MTLBlendOperation mb_blendop(int op)
{
	switch (op)
	{
	case DPBLENDOP_REVSUBTRACT: return MTLBlendOperationReverseSubtract;
	case DPBLENDOP_MAX:         return MTLBlendOperationMax;
	default:                    return MTLBlendOperationAdd;
	}
}

static MTLCompareFunction mb_comparefunc(int glfunc)
{
	switch (glfunc)
	{
	case GL_NEVER:    return MTLCompareFunctionNever;
	case GL_LESS:     return MTLCompareFunctionLess;
	case GL_EQUAL:    return MTLCompareFunctionEqual;
	case GL_LEQUAL:   return MTLCompareFunctionLessEqual;
	case GL_GREATER:  return MTLCompareFunctionGreater;
	case GL_NOTEQUAL: return MTLCompareFunctionNotEqual;
	case GL_GEQUAL:   return MTLCompareFunctionGreaterEqual;
	case GL_ALWAYS:   return MTLCompareFunctionAlways;
	default:          return MTLCompareFunctionLessEqual;
	}
}

// Only the five ops glquake.h declares in this tree. GL_INVERT and the _WRAP
// pair are absent from it entirely -- and so, it turns out, is any caller:
// R_SetStencil has ZERO call sites here, because shadow volumes are among the
// features METAL.md lists as dead in this fork's configuration. The state is
// still shadowed and translated so that Phase 4a inherits something honest
// rather than a stub, but nothing exercises it today.
static MTLStencilOperation mb_stencilop(int glop)
{
	switch (glop)
	{
	case GL_KEEP:    return MTLStencilOperationKeep;
	case GL_ZERO:    return MTLStencilOperationZero;
	case GL_REPLACE: return MTLStencilOperationReplace;
	case GL_INCR:    return MTLStencilOperationIncrementClamp;
	case GL_DECR:    return MTLStencilOperationDecrementClamp;
	default:         return MTLStencilOperationKeep;
	}
}

// DarkPlaces packs an "unnormalised" flag into bit 31 of the gltype, which the
// GL arm strips and inverts into qglVertexAttribPointer's `normalized`
// argument (gl_backend.c:2091). The same bit selects between the normalized and
// unnormalized Metal formats here.
static MTLVertexFormat mb_vertexformat(int components, int gltype)
{
	int normalised = (gltype & 0x80000000) == 0;
	switch (gltype & ~0x80000000)
	{
	case GL_FLOAT:
		switch (components)
		{
		case 1: return MTLVertexFormatFloat;
		case 2: return MTLVertexFormatFloat2;
		case 3: return MTLVertexFormatFloat3;
		default: return MTLVertexFormatFloat4;
		}
	case GL_UNSIGNED_BYTE:
		if (components == 4)
			return normalised ? MTLVertexFormatUChar4Normalized : MTLVertexFormatUChar4;
		return normalised ? MTLVertexFormatUChar2Normalized : MTLVertexFormatUChar2;
	case GL_SHORT:
		if (components == 4)
			return normalised ? MTLVertexFormatShort4Normalized : MTLVertexFormatShort4;
		return normalised ? MTLVertexFormatShort2Normalized : MTLVertexFormatShort2;
	default:
		return MTLVertexFormatFloat4;
	}
}

static unsigned int mb_fnv32(unsigned int h, const void *data, size_t len)
{
	const unsigned char *p = (const unsigned char *)data;
	size_t i;
	for (i = 0; i < len; i++)
	{
		h ^= p[i];
		h *= 16777619u;
	}
	return h;
}

// ---------------------------------------------------------------------------
// pipeline construction

static unsigned int mb_layouthash(void)
{
	unsigned int h = 2166136261u;
	int i;
	for (i = 0; i < MB_NUMATTRIBS; i++)
	{
		// the BINDING (handle, offset, pointer) is per-draw and deliberately
		// not hashed; only the shape bakes into a pipeline
		struct { int enabled, components, gltype; unsigned int stride; } s;
		memset(&s, 0, sizeof(s));
		s.enabled    = mb_state.attrib[i].enabled;
		s.components = mb_state.attrib[i].components;
		s.gltype     = mb_state.attrib[i].gltype;
		s.stride     = (unsigned int)mb_state.attrib[i].stride;
		h = mb_fnv32(h, &s, sizeof(s));
	}
	return h;
}

static MTLVertexDescriptor *mb_vertexdescriptor(void)
{
	MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
	int i;
	for (i = 0; i < MB_NUMATTRIBS; i++)
	{
		const mb_attrib_t *a = mb_state.attrib + i;
		// Attributes 0, 1 and 2 are always declared, because the shaders read
		// them unconditionally and a stage_in read of an attribute absent from
		// the descriptor is a link error. A disabled slot becomes a CONSTANT
		// binding fed with 16 bytes -- which for the colour slot is not a
		// workaround but the design: GL_Color is a constant vertex attribute
		// (gl_backend.c:1471), and MTLVertexStepFunctionConstant is how Metal
		// spells that. It is why there is one shader and two vertex layouts
		// rather than two shaders.
		qbool declare = a->enabled || i <= MB_ATTRIB_TEXCOORD0;
		if (!declare)
			continue;
		vd.attributes[i].format      = a->enabled ? mb_vertexformat(a->components, a->gltype) : MTLVertexFormatFloat4;
		vd.attributes[i].offset      = 0;
		vd.attributes[i].bufferIndex = i;
		if (a->enabled)
		{
			vd.layouts[i].stride       = a->stride ? a->stride : 16;
			vd.layouts[i].stepFunction = MTLVertexStepFunctionPerVertex;
			vd.layouts[i].stepRate     = 1;
		}
		else
		{
			vd.layouts[i].stride       = 16;
			vd.layouts[i].stepFunction = MTLVertexStepFunctionConstant;
			vd.layouts[i].stepRate     = 0;
		}
	}
	return vd;
}

static qbool mb_ensurelibrary(void)
{
	NSError *err = nil;
	if (mb_lib)
		return true;
	if (!mb_dev)
		return false;
	mb_lib = [mb_dev newLibraryWithSource:[NSString stringWithUTF8String:kBackendSrc]
	                              options:nil
	                                error:&err];
	if (!mb_lib)
	{
		Con_Printf(CON_ERROR "Metal_Backend: shader compile failed: %s\n",
		           err ? [[err localizedDescription] UTF8String] : "unknown error");
		return false;
	}
	return true;
}

// A fixed vertex layout used ONLY to make the reflection pipeline link. The
// uniform struct's layout cannot depend on the vertex layout, so this need not
// match whatever the real draw will use -- but it MUST declare every attribute
// slot any shader's stage_in might read, because a read of an undeclared
// attribute is a pipeline-creation error, not a warning. All ten slots as
// float4: declaring attributes a function does not read is legal, reading one
// the descriptor lacks is not. (Learned live: MODE_LIGHTMAP's Attrib_TexCoord4
// at slot 6 failed reflection against the old three-slot version, and the
// whole opaque world silently fell to "shader failed".)
static MTLVertexDescriptor *mb_canonical_vertexdescriptor(void)
{
	MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
	int i;
	for (i = 0; i < MB_NUMATTRIBS; i++)
	{
		vd.attributes[i].format      = MTLVertexFormatFloat4;
		vd.attributes[i].offset      = 0;
		vd.attributes[i].bufferIndex = i;
		vd.layouts[i].stride         = 16;
		vd.layouts[i].stepFunction   = MTLVertexStepFunctionPerVertex;
		vd.layouts[i].stepRate       = 1;
	}
	return vd;
}

// One name, one row. The vertex and fragment stages both reflect the constant
// struct, so a member arrives here twice with the same offset; the second is
// skipped. A name arriving with a DIFFERENT location is a real conflict (two
// textures declared under one name, or a member whose offset differs between
// stages, which the shared struct makes impossible) and is said out loud. A
// full table is said out loud too -- a texture dropped here is a texture the C
// side never binds, which Metal API validation turns into an abort at the draw.
static void mb_addname(mb_program_t *p, const char *name, int loc)
{
	int i;
	for (i = 0; i < p->numuniforms; i++)
	{
		if (strcmp(p->uniforms[i].name, name))
			continue;
		if (p->uniforms[i].loc != loc)
			Con_Printf(CON_ERROR "Metal_Backend: mode %u permutation %llx reflects '%s' at two locations (%d and %d)\n",
			           p->mode, (unsigned long long)p->permutation, name, p->uniforms[i].loc, loc);
		return;
	}
	if (p->numuniforms >= MB_MAX_UNIFORMS)
	{
		Con_Printf(CON_ERROR "Metal_Backend: mode %u permutation %llx uniform table FULL at %d names, '%s' dropped"
		           " -- a dropped texture binds nothing and ABORTS under Metal API validation; raise MB_MAX_UNIFORMS\n",
		           p->mode, (unsigned long long)p->permutation, MB_MAX_UNIFORMS, name);
		return;
	}
	dp_strlcpy(p->uniforms[p->numuniforms].name, name, sizeof(p->uniforms[0].name));
	p->uniforms[p->numuniforms].loc = loc;
	p->numuniforms++;
}

static void mb_addreflected(mb_program_t *p, NSArray<id<MTLBinding>> *bindings)
{
	for (id<MTLBinding> b in bindings)
	{
		if (b.type == MTLBindingTypeBuffer)
		{
			id<MTLBufferBinding> bb = (id<MTLBufferBinding>)b;
			MTLStructType *st;
			if (b.index != MB_BUFFERINDEX_PARAMS)
				continue;   // a vertex-attribute buffer, not the constant struct
			st = bb.bufferStructType;
			if (!st)
				continue;
			if (bb.bufferDataSize > p->uniformbytes)
				p->uniformbytes = bb.bufferDataSize;
			for (MTLStructMember *m in st.members)
				mb_addname(p, [m.name UTF8String], (int)m.offset);
		}
		else if (b.type == MTLBindingTypeTexture)
			mb_addname(p, [b.name UTF8String], MB_LOC_TEXTURE | (int)b.index);
		// samplers are bound alongside their texture by index; nothing to record
	}
}

int Metal_Backend_CompilePermutation(unsigned int mode, uint64_t permutation, const char **strings, int numstrings)
{
	NSMutableString *src;
	MTLCompileOptions *opts;
	MTLRenderPipelineDescriptor *pd;
	MTLRenderPipelineReflection *refl = nil;
	id<MTLRenderPipelineState> probe;
	NSError *err = nil;
	mb_program_t *p;
	int i, id_;

	if (!mb_dev)
		return 0;
	for (i = MB_PROGRAM_FIRSTREAL; i < mb_numprograms; i++)
		if (mb_programs[i].used && mb_programs[i].mode == mode && mb_programs[i].permutation == permutation)
			return i;
	if (mb_numprograms >= MB_MAX_PROGRAMS)
	{
		Con_Printf(CON_ERROR "Metal_Backend: out of shader program slots (%d)\n", MB_MAX_PROGRAMS);
		return 0;
	}

	// the caller's pretext, then the shared body -- the same two halves the GLSL
	// compiler is handed, in the same order
	src = [NSMutableString string];
	for (i = 0; i < numstrings; i++)
		if (strings[i])
			[src appendString:[NSString stringWithUTF8String:strings[i]]];
	for (i = 0; builtinmslstrings[i]; i++)
		[src appendString:[NSString stringWithUTF8String:builtinmslstrings[i]]];

	opts = [[MTLCompileOptions alloc] init];
	// METAL.md wants [[invariant]] on position for cross-pass depth agreement;
	// this is its API-level companion and costs nothing to set now.
	opts.preserveInvariance = YES;

	id_ = mb_numprograms;
	p = &mb_programs[id_];
	memset(p->uniforms, 0, sizeof(p->uniforms));
	memset(p->uniformdata, 0, sizeof(p->uniformdata));   // GL's fresh-link zero
	p->numuniforms = 0;
	p->uniformbytes = 0;
	p->mode = mode;
	p->permutation = permutation;

	p->lib = [mb_dev newLibraryWithSource:src options:opts error:&err];
	if (!p->lib)
	{
		Con_Printf(CON_ERROR "Metal_Backend: MSL compile failed for mode %u permutation %llx:\n%s\n",
		           mode, (unsigned long long)permutation,
		           err ? [[err localizedDescription] UTF8String] : "unknown error");
		return 0;
	}
	p->vfunc = [p->lib newFunctionWithName:@"dp_vertex"];
	p->ffunc = [p->lib newFunctionWithName:@"dp_fragment"];
	if (!p->vfunc || !p->ffunc)
	{
		Con_Printf(CON_ERROR "Metal_Backend: mode %u permutation %llx compiled but is missing dp_vertex or dp_fragment\n",
		           mode, (unsigned long long)permutation);
		p->lib = nil; p->vfunc = nil; p->ffunc = nil;
		return 0;
	}

	// Reflect once, off a canonical pipeline. BindingInfo alone gives the
	// bindings; BufferTypeInfo is what makes bufferStructType (and therefore the
	// member offsets this whole seam depends on) come back non-nil.
	pd = [[MTLRenderPipelineDescriptor alloc] init];
	pd.vertexFunction   = p->vfunc;
	pd.fragmentFunction = p->ffunc;
	pd.vertexDescriptor = mb_canonical_vertexdescriptor();
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	probe = [mb_dev newRenderPipelineStateWithDescriptor:pd
	                                             options:(MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo)
	                                          reflection:&refl
	                                               error:&err];
	if (!probe || !refl)
	{
		Con_Printf(CON_ERROR "Metal_Backend: reflection pipeline failed for mode %u permutation %llx: %s\n",
		           mode, (unsigned long long)permutation,
		           err ? [[err localizedDescription] UTF8String] : "no reflection returned");
		p->lib = nil; p->vfunc = nil; p->ffunc = nil;
		return 0;
	}
	mb_addreflected(p, refl.vertexBindings);
	mb_addreflected(p, refl.fragmentBindings);
	if (p->uniformbytes > MB_UNIFORM_MAXBYTES)
	{
		// THIS IS A CRASH, not a warning, on any build with Metal API validation
		// on -- which is every Xcode Debug run, i.e. exactly how Seb plays. The
		// clamp below keeps the staging array from being overrun, but it binds a
		// buffer SHORTER than the struct the shader declares, and the validation
		// layer aborts the process at the first draw. Release builds have
		// validation off and limp on, which is why this can reach a hand-back.
		// Raise MB_UNIFORM_MAXBYTES (see the note at its definition; above 4096
		// the mechanism itself has to change). smoke run M asserts this line
		// never appears.
		Con_Printf(CON_ERROR "Metal_Backend: mode %u wants a %d byte constant struct, over the %d staged"
		           " -- ABORTS under Metal API validation; raise MB_UNIFORM_MAXBYTES\n",
		           mode, (int)p->uniformbytes, MB_UNIFORM_MAXBYTES);
		p->uniformbytes = MB_UNIFORM_MAXBYTES;
	}

	p->used = true;
	mb_numprograms++;
	Con_DPrintf("Metal_Backend: compiled mode %u permutation %llx -> program %d, %d reflected names, %d byte constants\n",
	            mode, (unsigned long long)permutation, id_, p->numuniforms, (int)p->uniformbytes);
	return id_;
}

void Metal_Backend_FreePrograms(void)
{
	int i;
	mb_end_encoder();
	for (i = 0; i < MB_MAX_PROGRAMS; i++)
	{
		mb_programs[i].lib = nil;
		mb_programs[i].vfunc = nil;
		mb_programs[i].ffunc = nil;
		mb_programs[i].used = false;
		mb_programs[i].numuniforms = 0;
		mb_programs[i].uniformbytes = 0;
		memset(mb_programs[i].uniformdata, 0, sizeof(mb_programs[i].uniformdata));
	}
	mb_numprograms = MB_PROGRAM_FIRSTREAL;
	// the PSO cache goes WITH the programs -- its keys embed program ids
	for (i = 0; i <= mb_numpsos && i < MB_PSO_MAX; i++)
	{
		mb_psos[i].pso = nil;
		memset(&mb_psos[i].key, 0, sizeof(mb_psos[i].key));
		mb_psos[i].next = 0;
	}
	memset(mb_psohash, 0, sizeof(mb_psohash));
	mb_numpsos = 0;
	mb_psofull = 0;
	mb_state.program = MB_PROGRAM_SENTINEL;
}

void Metal_Backend_SetProgram(int program)
{
	mb_state.program = mb_programfor(program) ? program : MB_PROGRAM_SENTINEL;
}

int Metal_Backend_GetUniformLocation(int program, const char *name)
{
	mb_program_t *p = mb_programfor(program);
	int i;
	if (!p || !name)
		return -1;
	for (i = 0; i < p->numuniforms; i++)
		if (!strcmp(p->uniforms[i].name, name))
			return p->uniforms[i].loc;
	return -1;   // absent from this permutation, exactly as GL reports it
}

void Metal_Backend_SetUniformFloats(int loc, const float *values, int numfloats)
{
	if (loc < 0 || (loc & MB_LOC_TEXTURE))
		return;
	if ((size_t)loc + numfloats * sizeof(float) > MB_UNIFORM_MAXBYTES)
		return;
	memcpy(mb_uniformblock() + loc, values, numfloats * sizeof(float));
}

// METAL.md Phase 4d. The texture-index lockstep rule (stated at the head of
// shader_msl.h) says an MSL arm must declare its [[texture(n)]] slots in the
// order gl_rmain.c's sampler walk visits them, numbered densely over exactly
// the ones it declares. Nothing enforced it, and a violation is SILENT: the
// engine binds the texture to the unit the walk assigned, the shader samples
// the index it declared, and a sample from an unbound slot returns zero rather
// than erroring. This reports one for what it is.
//
// It lives here because this is the one place both numbers meet -- see the
// comment in Metal_Backend_SetUniformInt. One site covers all 37 textures,
// every shader mode and every permutation the engine ever compiles, so unlike
// a per-mode assertion list it cannot rot.
static void mb_texindex_mismatch(int loc, int unit)
{
	mb_program_t *p = mb_programfor(mb_state.program);
	const char *name = "(unknown)";
	int i;
	if (p)
		for (i = 0; i < p->numuniforms; i++)
			if (p->uniforms[i].loc == loc)
			{
				name = p->uniforms[i].name;
				break;
			}
	Con_Printf(CON_ERROR "Metal_Backend: TEXTURE INDEX LOCKSTEP BROKEN -- mode %u permutation %llu: "
		"the MSL declares %s at [[texture(%d)]], but the sampler walk assigns it unit %d. "
		"R_Mesh_TexBind will bind where the shader does not sample, and the sample returns zero. "
		"See the texture-index rule at the head of shader_msl.h.\n",
		p ? p->mode : 0u, p ? (unsigned long long)p->permutation : 0ull,
		name, loc & ~MB_LOC_TEXTURE, unit);
}

void Metal_Backend_SetUniformInt(int loc, int value)
{
	// A texture "location" reaches here from the sampler-unit assignment block,
	// which tells a GLSL sampler which unit it lives on. MSL bakes the index into
	// the function signature, so there is nothing to write -- and writing would
	// scribble on whatever constant happens to sit at that offset.
	//
	// It is exactly the right place to CHECK the index, though, and for a while
	// it was throwing that check away: `value` is the unit the dense sampler walk
	// just assigned, and `loc` carries the index the MSL declared. They must be
	// the same number.
	if (loc & MB_LOC_TEXTURE)
	{
		if ((loc & ~MB_LOC_TEXTURE) != value)
			mb_texindex_mismatch(loc, value);
		return;
	}
	if (loc < 0)
		return;
	if ((size_t)loc + sizeof(int) > MB_UNIFORM_MAXBYTES)
		return;
	memcpy(mb_uniformblock() + loc, &value, sizeof(int));
}

static id<MTLRenderPipelineState> mb_pipeline_get(const mb_pipelinekey_t *key)
{
	NSError *err = nil;
	MTLRenderPipelineDescriptor *pd;
	MTLRenderPipelineColorAttachmentDescriptor *ca;
	id<MTLRenderPipelineState> pso;
	unsigned int h;
	int bucket, i;

	h = mb_fnv32(2166136261u, key, sizeof(*key));
	bucket = (int)(h & (MB_PSO_HASH - 1));
	for (i = mb_psohash[bucket]; i; i = mb_psos[i].next)
		if (!memcmp(&mb_psos[i].key, key, sizeof(*key)))
			return mb_psos[i].pso;

	// the built-ins live in one hardcoded library; real permutations each carry
	// their own, compiled by Metal_Backend_CompilePermutation
	if (key->program < MB_PROGRAM_FIRSTREAL && !mb_ensurelibrary())
		return nil;
	if (mb_numpsos + 1 >= MB_PSO_MAX)
	{
		if (!mb_psofull)
		{
			mb_psofull = 1;
			Con_Printf(CON_ERROR "Metal_Backend: pipeline cache full at %d entries; further pipelines will not be created\n", MB_PSO_MAX);
		}
		return nil;
	}

	pd = [[MTLRenderPipelineDescriptor alloc] init];
	if (key->program >= MB_PROGRAM_FIRSTREAL)
	{
		mb_program_t *prog = mb_programfor(key->program);
		if (!prog)
			return nil;
		pd.vertexFunction   = prog->vfunc;
		pd.fragmentFunction = prog->ffunc;
		pd.vertexDescriptor = mb_vertexdescriptor();
	}
	else
	switch (key->program)
	{
	case MB_PROGRAM_CLEAR:
		pd.vertexFunction   = [mb_lib newFunctionWithName:@"mb_clear_v"];
		pd.fragmentFunction = [mb_lib newFunctionWithName:@"mb_clear_f"];
		// generated from vertex_id, so no vertex descriptor at all
		break;
	case MB_PROGRAM_PRESENT:
		pd.vertexFunction   = [mb_lib newFunctionWithName:@"mb_present_v"];
		pd.fragmentFunction = [mb_lib newFunctionWithName:@"mb_present_f"];
		// generated from vertex_id, so no vertex descriptor at all
		break;
	case MB_PROGRAM_BLIT:
		pd.vertexFunction   = [mb_lib newFunctionWithName:@"mb_blit_v"];
		pd.fragmentFunction = [mb_lib newFunctionWithName:@"mb_blit_f"];
		// generated from vertex_id, so no vertex descriptor at all
		break;
	case MB_PROGRAM_PROBE2D:
		pd.vertexFunction   = [mb_lib newFunctionWithName:@"mb_pass_v"];
		pd.fragmentFunction = [mb_lib newFunctionWithName:@"mb_probe_f"];
		pd.vertexDescriptor = mb_vertexdescriptor();
		break;
	default:
		pd.vertexFunction   = [mb_lib newFunctionWithName:@"mb_pass_v"];
		pd.fragmentFunction = [mb_lib newFunctionWithName:@"mb_sentinel_f"];
		pd.vertexDescriptor = mb_vertexdescriptor();
		break;
	}
	pd.rasterSampleCount        = key->samplecount ? key->samplecount : 1;
	pd.alphaToCoverageEnabled   = key->alphatocoverage ? YES : NO;
	pd.depthAttachmentPixelFormat = (MTLPixelFormat)key->depthformat;

	ca = pd.colorAttachments[0];
	ca.pixelFormat      = (MTLPixelFormat)key->colorformat;
	ca.blendingEnabled  = key->blendenable ? YES : NO;
	ca.sourceRGBBlendFactor        = mb_blendfactor(key->blendsrc);
	ca.sourceAlphaBlendFactor      = mb_blendfactor(key->blendsrc);
	ca.destinationRGBBlendFactor   = mb_blendfactor(key->blenddst);
	ca.destinationAlphaBlendFactor = mb_blendfactor(key->blenddst);
	ca.rgbBlendOperation           = mb_blendop(key->blendop);
	ca.alphaBlendOperation         = mb_blendop(key->blendop);
	ca.writeMask = ((key->colormask & 1) ? MTLColorWriteMaskRed   : 0)
	             | ((key->colormask & 2) ? MTLColorWriteMaskGreen : 0)
	             | ((key->colormask & 4) ? MTLColorWriteMaskBlue  : 0)
	             | ((key->colormask & 8) ? MTLColorWriteMaskAlpha : 0);

	pso = [mb_dev newRenderPipelineStateWithDescriptor:pd error:&err];
	if (!pso)
	{
		Con_Printf(CON_ERROR "Metal_Backend: pipeline creation failed: %s\n",
		           err ? [[err localizedDescription] UTF8String] : "unknown error");
		return nil;
	}

	i = ++mb_numpsos;
	mb_psos[i].key  = *key;
	mb_psos[i].pso  = pso;
	mb_psos[i].next = mb_psohash[bucket];
	mb_psohash[bucket] = i;
	return pso;
}

static id<MTLDepthStencilState> mb_dss_get(void)
{
	mb_dsskey_t key;
	MTLDepthStencilDescriptor *dd;
	MTLStencilDescriptor *sd;
	int i;
	// No depth attachment means no depth test, exactly as GL behaves with no
	// depth buffer -- and asking Metal to compare against an absent attachment
	// is a validation error rather than a no-op.
	qbool hasdepth = (mb_targetdepth != nil);

	memset(&key, 0, sizeof(key));
	key.depthtest     = (unsigned char)(hasdepth && mb_state.depthtest);
	// depthtest is part of the WRITE condition too: GL writes no depth while
	// GL_DEPTH_TEST is disabled, whatever glDepthMask says. Without the third
	// term, 2D quads (depth test off, mask left true by R_ClearScreen's setup)
	// would stamp their quad depths over the 3D scene's buffer.
	key.depthwrite    = (unsigned char)(hasdepth && mb_state.depthmask && mb_state.depthtest);
	key.depthfunc     = (unsigned char)mb_comparefunc(key.depthtest ? mb_state.depthfunc : GL_ALWAYS);
	key.stencilenable = (unsigned char)(hasdepth && mb_state.stencilenable);
	key.sfail         = (unsigned char)mb_stencilop(mb_state.stencilfail);
	key.szfail        = (unsigned char)mb_stencilop(mb_state.stencilzfail);
	key.szpass        = (unsigned char)mb_stencilop(mb_state.stencilzpass);
	key.scompare      = (unsigned char)mb_comparefunc(mb_state.stencilcompare);
	key.writemask     = (unsigned char)(mb_state.stencilwritemask & 0xFF);
	key.comparemask   = (unsigned char)(mb_state.stencilcomparemask & 0xFF);
	key.ref           = (unsigned char)(mb_state.stencilref & 0xFF);

	for (i = 0; i < mb_numdsss; i++)
		if (!memcmp(&mb_dsskeys[i], &key, sizeof(key)))
			return mb_dsss[i];
	if (mb_numdsss >= MB_DSS_MAX)
		return mb_dsss[0];

	dd = [[MTLDepthStencilDescriptor alloc] init];
	dd.depthCompareFunction = (MTLCompareFunction)key.depthfunc;
	dd.depthWriteEnabled    = key.depthwrite ? YES : NO;
	if (key.stencilenable)
	{
		sd = [[MTLStencilDescriptor alloc] init];
		sd.stencilFailureOperation   = (MTLStencilOperation)key.sfail;
		sd.depthFailureOperation     = (MTLStencilOperation)key.szfail;
		sd.depthStencilPassOperation = (MTLStencilOperation)key.szpass;
		sd.stencilCompareFunction    = (MTLCompareFunction)key.scompare;
		sd.writeMask                 = key.writemask;
		sd.readMask                  = key.comparemask;
		dd.frontFaceStencil = sd;
		dd.backFaceStencil  = sd;
	}

	mb_dsskeys[mb_numdsss] = key;
	mb_dsss[mb_numdsss] = [mb_dev newDepthStencilStateWithDescriptor:dd];
	return mb_dsss[mb_numdsss++];
}

// ---------------------------------------------------------------------------
// encoder lifetime
//
// Exactly one place creates an encoder and exactly one place applies state.
// Metal encoders start stateless and scattered state replay is the single
// largest bug source in this design (METAL.md's risk register), so
// mb_apply_state deliberately applies EVERYTHING on every draw rather than
// tracking dirty bits: a few hundred 2D draws a frame cannot notice, and it
// removes the entire "which encoder has which state" bug class. If a profile
// ever justifies dirty tracking it can be added behind the same single
// function, with r_metal_forceencoderrestart as the regression net.
//
// NOTE R_Mesh_Start / R_Mesh_Finish are NOT frame boundaries and must never
// become encoder boundaries: DrawQ_Finish() issues the whole console, menu and
// HUD AFTER R_Mesh_Finish() has run (cl_screen.c:2434-2435). They are state
// resets. Hence lazy creation here, at the first draw or clear.

static void mb_end_encoder(void)
{
	if (mb_enc)
	{
		[mb_enc endEncoding];
		mb_enc = nil;
	}
}

static void mb_flush(void)
{
	mb_end_encoder();
	if (mb_cb)
	{
		[mb_cb commit];
		[mb_cb waitUntilCompleted];
		mb_cb = nil;
	}
}

static qbool mb_ensure_encoder(void)
{
	MTLRenderPassDescriptor *rp;

	if (mb_enc)
		return true;
	if (!mb_dev || !mb_queue || !mb_targetcolor)
		return false;

	if (!mb_cb)
	{
		mb_cb = [mb_queue commandBuffer];
		mb_cb.label = @"QuakeM5 backend";
	}

	rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = mb_targetcolor;
	if (mb_pendingclearmask & MB_CLEAR_COLOR)
	{
		rp.colorAttachments[0].loadAction = MTLLoadActionClear;
		rp.colorAttachments[0].clearColor = MTLClearColorMake(mb_pendingclearcolor[0], mb_pendingclearcolor[1],
		                                                      mb_pendingclearcolor[2], mb_pendingclearcolor[3]);
	}
	else
		rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;

	if (mb_targetdepth)
	{
		rp.depthAttachment.texture = mb_targetdepth;
		if (mb_pendingclearmask & MB_CLEAR_DEPTH)
		{
			rp.depthAttachment.loadAction = MTLLoadActionClear;
			rp.depthAttachment.clearDepth = mb_pendingcleardepth;
		}
		else
			rp.depthAttachment.loadAction = MTLLoadActionLoad;
		rp.depthAttachment.storeAction = MTLStoreActionStore;
	}
	mb_pendingclearmask = 0;

	mb_enc = [mb_cb renderCommandEncoderWithDescriptor:rp];
	if (!mb_enc)
		return false;
	// METAL.md's mirror-everything decision: the projection negates NDC y, so
	// the winding convention is inverted once, globally, and render targets
	// hold GL-layout images.
	[mb_enc setFrontFacingWinding:MTLWindingClockwise];
	return true;
}

static void mb_apply_state(int program)
{
	mb_pipelinekey_t key;
	id<MTLRenderPipelineState> pso;
	MTLViewport vp;
	MTLScissorRect sr;
	int i, x, y, w, h;

	memset(&key, 0, sizeof(key));
	key.layouthash      = (program == MB_PROGRAM_CLEAR) ? 0 : mb_layouthash();
	key.program         = (unsigned short)program;
	key.blendsrc        = (unsigned short)mb_state.blendfunc1;
	key.blenddst        = (unsigned short)mb_state.blendfunc2;
	key.blendenable     = (unsigned char)mb_state.blendenable;
	key.blendop         = (unsigned char)mb_state.blendop;
	key.colormask       = (unsigned char)(mb_state.colormask & 15);
	key.alphatocoverage = (unsigned char)mb_state.alphatocoverage;
	key.colorformat     = (unsigned short)(mb_targetcolor ? mb_targetcolor.pixelFormat : MTLPixelFormatInvalid);
	key.depthformat     = (unsigned short)(mb_targetdepth ? mb_targetdepth.pixelFormat : MTLPixelFormatInvalid);
	key.samplecount     = 1;
	// a mid-pass clear must land whatever the caller's blend state happens to
	// be, and must write every channel
	if (program == MB_PROGRAM_CLEAR || program == MB_PROGRAM_PRESENT)
	{
		key.blendenable = 0;
		key.blendsrc = GL_ONE;
		key.blenddst = GL_ZERO;
		key.blendop = 0;
		key.colormask = 15;
		key.alphatocoverage = 0;
	}

	pso = mb_pipeline_get(&key);
	if (!pso)
		return;
	[mb_enc setRenderPipelineState:pso];
	[mb_enc setDepthStencilState:mb_dss_get()];
	if (mb_state.stencilenable)
		[mb_enc setStencilReferenceValue:(uint32_t)mb_state.stencilref];

	// Viewport and scissor keep GL numerics -- render targets store GL-layout
	// images (METAL.md), so a GL rect indexes the same rows Metal will write.
	x = mb_state.viewport[0]; y = mb_state.viewport[1];
	w = mb_state.viewport[2]; h = mb_state.viewport[3];
	if (w <= 0 || h <= 0) { x = 0; y = 0; w = mb_targetwidth; h = mb_targetheight; }
	vp.originX = x; vp.originY = y; vp.width = w; vp.height = h;
	vp.znear = mb_state.depthrange[0]; vp.zfar = mb_state.depthrange[1];
	// gl_state is memset at backend start and GL_Backend_ResetState never
	// assigns depthrange, so the shadow legitimately begins {0,0} on both
	// paths. GL simply never draws before someone calls GL_DepthRange; Metal
	// would silently flatten every fragment to depth 0, so say so instead.
	//
	// It rescues EXACTLY that {0,0} case and nothing else. A DELIBERATELY
	// collapsed range is a real technique here -- GL_DepthRange(0.0625,
	// 0.0625) pins every fragment of a fullscreen quad at one window depth so
	// it depth-tests against the scene while leaving the view model alone
	// (gl_rmain.c's murk composite, and the RT composite at METAL.md Phase 5).
	// Widening those back to 1.0 would depth-test the quad against nothing and
	// erase the mask, with no error anywhere -- so the test is on the pair
	// being zero, not on the two merely being equal.
	if (vp.znear == 0.0 && vp.zfar == 0.0)
		vp.zfar = 1.0;
	[mb_enc setViewport:vp];

	if (mb_state.scissortest)
	{
		x = mb_state.scissor[0]; y = mb_state.scissor[1];
		w = mb_state.scissor[2]; h = mb_state.scissor[3];
	}
	else
	{
		x = 0; y = 0; w = mb_targetwidth; h = mb_targetheight;
	}
	// Metal rejects a scissor rect that leaves the attachment, where GL simply
	// clips it
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > mb_targetwidth)  w = mb_targetwidth  - x;
	if (y + h > mb_targetheight) h = mb_targetheight - y;
	if (w < 0) w = 0;
	if (h < 0) h = 0;
	sr.x = x; sr.y = y; sr.width = w; sr.height = h;
	[mb_enc setScissorRect:sr];

	if (!mb_state.cullenable)
		[mb_enc setCullMode:MTLCullModeNone];
	else
		[mb_enc setCullMode:(mb_state.cullface == GL_BACK) ? MTLCullModeBack : MTLCullModeFront];
	[mb_enc setDepthBias:mb_state.polygonoffset[1] slopeScale:mb_state.polygonoffset[0] clamp:0.0f];

	if (program == MB_PROGRAM_CLEAR)
		return;

	// the per-draw constant struct, snapshotted from THIS program's own block.
	// setVertexBytes/setFragmentBytes COPY, so writes that R_Shader_Uniform*
	// made since the last draw land on this draw and every later one of this
	// program until overwritten -- GL's per-program uniform state, kept.
	{
		mb_program_t *prog = mb_programfor(program);
		if (prog && prog->uniformbytes)
		{
			[mb_enc setVertexBytes:prog->uniformdata length:prog->uniformbytes atIndex:MB_BUFFERINDEX_PARAMS];
			[mb_enc setFragmentBytes:prog->uniformdata length:prog->uniformbytes atIndex:MB_BUFFERINDEX_PARAMS];
		}
	}

	// EVERY UNIT GETS A REAL SAMPLER, INCLUDING THE EMPTY ONES, and that is not
	// tidiness: Metal's API VALIDATION treats a sampler the fragment function
	// declares and nothing binds as a hard error and traps the process. A nil
	// TEXTURE is fine by comparison -- reading one is defined to return zero --
	// so the texture stays nil here and no pixel can move.
	//
	// The case that trips it is a STATIC PARM that declares a sampler in every
	// permutation while the C side binds it only on the batches that use it:
	// USERTLIQUIDS puts Sampler_RTTerm in every surface shader once
	// rt_metal_liquids is above 0, and an ordinary wall never binds it. The
	// render is CORRECT either way (the shader's own mix() weight is 0 there, so
	// the sampled value is discarded), which is exactly why every pixel bed in
	// this arc passed and why nothing caught it: `make sdl-release` runs with
	// validation OFF, and the Xcode DEBUG build Seb runs turns it ON. Latent
	// since 5-5-3 shipped rt_metal_liquids on the Metal path.
	for (i = 0; i < MB_MAX_TEXUNITS; i++)
	{
		int handle = mb_state.texunit[i];
		id<MTLTexture> tex = handle ? (__bridge id<MTLTexture>)Metal_Texture_GetTexture(handle) : nil;
		id<MTLSamplerState> smp = handle ? (__bridge id<MTLSamplerState>)Metal_Texture_GetSampler(handle) : nil;
		if (!smp)
			smp = (__bridge id<MTLSamplerState>)Metal_Texture_GetNullSampler();
		[mb_enc setFragmentTexture:tex atIndex:i];
		[mb_enc setFragmentSamplerState:smp atIndex:i];
	}
}

// Bind the ten attribute slots. Separated from mb_apply_state because the
// vertex count is needed for the client-array fallback.
static qbool mb_bind_vertexbuffers(int firstvertex, int numvertices)
{
	int i;
	for (i = 0; i < MB_NUMATTRIBS; i++)
	{
		const mb_attrib_t *a = mb_state.attrib + i;
		id<MTLBuffer> buf;
		if (!a->enabled)
		{
			// the constant slot. For colour this is GL_Color's value, which is
			// the whole point of the second vertex layout; for the others it is
			// a harmless zero so the shader's stage_in read is well defined.
			float zero[4] = {0, 0, 0, 0};
			const float *src = (i == MB_ATTRIB_COLOR) ? mb_state.color4f : zero;
			if (i > MB_ATTRIB_TEXCOORD0)
				continue;   // slots above texcoord0 are simply absent from the descriptor
			[mb_enc setVertexBytes:src length:sizeof(float[4]) atIndex:i];
			continue;
		}
		buf = mb_bufferfor(a->bufferhandle);
		if (buf)
		{
			[mb_enc setVertexBuffer:buf offset:a->offset atIndex:i];
			continue;
		}
		if (a->pointer)
		{
			// Metal has no client arrays. In practice every live path uploads
			// through R_BufferData_Store first, so this is the fallback rather
			// than the rule; small payloads ride Metal's inline path.
			size_t len = (size_t)(firstvertex + numvertices) * (a->stride ? a->stride : 16);
			if (len <= 4096)
			{
				[mb_enc setVertexBytes:a->pointer length:len atIndex:i];
				continue;
			}
			if (!mb_warned_clientarray)
			{
				mb_warned_clientarray = 1;
				Con_Printf(CON_WARN "Metal_Backend: a client-side vertex array of %d bytes has no buffer and is too large to inline; the draw is skipped\n", (int)len);
			}
			return false;
		}
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// lifecycle

void Metal_Backend_Start(void)
{
	Metal_Backend_Shutdown();
	mb_dev   = (__bridge id<MTLDevice>)VID_Metal_GetDevice();
	mb_queue = (__bridge id<MTLCommandQueue>)VID_Metal_GetQueue();
	if (!mb_dev || !mb_queue)
	{
		Con_Printf(CON_ERROR "Metal_Backend: no Metal device or queue; the backend is not available\n");
		return;
	}
	mb_started = true;
	Metal_Backend_ResetState();
	Con_Printf("Metal backend started\n");
	{ const char *e = getenv("METAL_FRAMEMS"); mb_framems = e ? atoi(e) : 0; if (mb_framems < 0) mb_framems = 0; }
	// METAL.md Phase 8-4: probe MetalFX against the device that just came up.
	// Must run before the first BeginFrame, whose screen-texture descriptor
	// reads the queried output-usage bits.
	MetalFX_Start();
}

void Metal_Backend_Shutdown(void)
{
	int i;
	mb_end_encoder();
	if (mb_cb)
	{
		[mb_cb commit];
		[mb_cb waitUntilCompleted];
		mb_cb = nil;
	}
	// METAL.md Phase 8-4: forget the MetalFX probe results with the rest of
	// the device state; Metal_Backend_Start re-probes.
	MetalFX_Shutdown();
	// clear field by field rather than memset: these structs hold ARC pointers,
	// and memset over one is the -Wnontrivial-memaccess trap
	for (i = 0; i <= mb_numpsos && i < MB_PSO_MAX; i++)
	{
		mb_psos[i].pso = nil;
		memset(&mb_psos[i].key, 0, sizeof(mb_psos[i].key));
		mb_psos[i].next = 0;
	}
	memset(mb_psohash, 0, sizeof(mb_psohash));
	mb_numpsos = 0;
	mb_psofull = 0;
	for (i = 0; i < mb_numdsss; i++)
		mb_dsss[i] = nil;
	mb_numdsss = 0;
	for (i = 0; i < MB_MAX_BUFFERS; i++)
	{
		mb_bufs[i].buf = nil;
		mb_bufs[i].used = false;
		mb_bufs[i].size = 0;
	}
	mb_numbufs = 1;
	for (i = 0; i < MB_MAX_PROGRAMS; i++)
	{
		mb_programs[i].lib = nil;
		mb_programs[i].vfunc = nil;
		mb_programs[i].ffunc = nil;
		mb_programs[i].used = false;
		mb_programs[i].numuniforms = 0;
		mb_programs[i].uniformbytes = 0;
		memset(mb_programs[i].uniformdata, 0, sizeof(mb_programs[i].uniformdata));
	}
	mb_numprograms = MB_PROGRAM_FIRSTREAL;
	memset(mb_uniformscratch, 0, sizeof(mb_uniformscratch));
	mb_lib = nil;
	mb_targetcolor = nil;
	mb_targetdepth = nil;
	mb_probetarget = nil;
	mb_probesaved = nil;
	mb_probesaveddepth = nil;
	mb_screentex = nil;
	mb_screendepth = nil;
	memset(mb_fbos, 0, sizeof(mb_fbos));
	mb_currentfbo = 0;
	mb_screenw = mb_screenh = 0;
	mb_targetwidth = mb_targetheight = 0;
	mb_started = false;
	// drain the last frame pool -- with the backend down, the next
	// BeginFrame that runs belongs to a fresh Start and pushes its own
	if (mb_framepool)
	{
		objc_autoreleasePoolPop(mb_framepool);
		mb_framepool = NULL;
	}
}

void Metal_Backend_ResetState(void)
{
	int i;
	mb_end_encoder();
	memset(&mb_state, 0, sizeof(mb_state));
	// Mirror GL_Backend_ResetState's values EXACTLY (gl_backend.c:1101-1117),
	// including the fields it leaves at the surrounding memset's zero. This is
	// not tidiness: most of gl_backend.c's setters early-out when the new value
	// equals the shadow, and that comparison is made against gl_state OUTSIDE
	// the renderpath switch. If this shadow ever disagrees with that one, the
	// early-out silently skips the Metal arm and the state is quietly wrong --
	// which on this path shows up as a wrong picture with nothing in the log.
	mb_state.depthtest    = true;
	mb_state.blendfunc1   = GL_ONE;
	mb_state.blendfunc2   = GL_ZERO;
	mb_state.blendenable  = false;
	mb_state.depthmask    = true;
	mb_state.colormask    = 15;
	mb_state.cullface     = GL_FRONT;   // with cullenable false: GL's "no culling"
	mb_state.cullenable   = false;
	mb_state.depthfunc    = GL_LEQUAL;
	mb_state.depthrange[0] = 0.0f;      // gl_state's memset value, deliberately
	mb_state.depthrange[1] = 0.0f;
	mb_state.stencilcompare = GL_ALWAYS;
	mb_state.program      = MB_PROGRAM_SENTINEL;
	for (i = 0; i < 4; i++)
		mb_state.color4f[i] = 1.0f;
	// position is the one slot GL leaves enabled by default
	mb_state.attrib[MB_ATTRIB_POSITION].enabled    = true;
	mb_state.attrib[MB_ATTRIB_POSITION].components = 3;
	mb_state.attrib[MB_ATTRIB_POSITION].gltype     = GL_FLOAT;
	mb_state.attrib[MB_ATTRIB_POSITION].stride     = sizeof(float[3]);
}

void Metal_Backend_Finish(void)
{
	mb_flush();
}

// ---------------------------------------------------------------------------
// state

// blendenable is DERIVED, and MAX ignores its factors, so both setters have to
// recompute it or the answer depends on which was called last -- see the long
// note above GL_BlendEquationEx (gl_backend.c).
static void mb_derive_blendenable(void)
{
	mb_state.blendenable = (mb_state.blendfunc1 != GL_ONE || mb_state.blendfunc2 != GL_ZERO
	                     || mb_state.blendop == DPBLENDOP_MAX);
}

void Metal_Backend_SetBlendFunc(int glsrc, int gldst)
{
	mb_state.blendfunc1 = glsrc;
	mb_state.blendfunc2 = gldst;
	mb_derive_blendenable();
}

void Metal_Backend_SetBlendEquation(int op)
{
	mb_state.blendop = op;
	mb_derive_blendenable();
}

void Metal_Backend_SetBlendEquationSubtract(qbool negated) { Metal_Backend_SetBlendEquation(negated ? DPBLENDOP_REVSUBTRACT : DPBLENDOP_ADD); }
void Metal_Backend_SetDepthMask(qbool enable)              { mb_state.depthmask = enable ? 1 : 0; }
void Metal_Backend_SetDepthTest(qbool enable)              { mb_state.depthtest = enable ? 1 : 0; }
void Metal_Backend_SetDepthFunc(int glfunc)                { mb_state.depthfunc = glfunc; }
void Metal_Backend_SetAlphaToCoverage(qbool enable)        { mb_state.alphatocoverage = enable ? 1 : 0; }
void Metal_Backend_SetColorMask(int mask)                  { mb_state.colormask = mask; }
void Metal_Backend_SetScissorTest(qbool enable)            { mb_state.scissortest = enable ? 1 : 0; }

void Metal_Backend_SetCullFace(int glface)
{
	// GL splits this across an enable flag and a direction; mirror that split
	// exactly, because GL_SetMirrorState re-applies the direction without
	// meaning to change the enablement.
	if (glface == GL_NONE)
		mb_state.cullenable = 0;
	else
	{
		mb_state.cullenable = 1;
		mb_state.cullface = glface;
	}
}

void Metal_Backend_SetDepthRange(float nearfrac, float farfrac)
{
	mb_state.depthrange[0] = nearfrac;
	mb_state.depthrange[1] = farfrac;
}

void Metal_Backend_SetStencil(qbool enable, int writemask, int glfail, int glzfail, int glzpass, int glcompare, int comparereference, int comparemask)
{
	mb_state.stencilenable      = enable ? 1 : 0;
	mb_state.stencilwritemask   = writemask;
	mb_state.stencilfail        = glfail;
	mb_state.stencilzfail       = glzfail;
	mb_state.stencilzpass       = glzpass;
	mb_state.stencilcompare     = glcompare;
	mb_state.stencilref         = comparereference;
	mb_state.stencilcomparemask = comparemask;
}

void Metal_Backend_SetPolygonOffset(float planeoffset, float depthoffset)
{
	mb_state.polygonoffset[0] = planeoffset;
	mb_state.polygonoffset[1] = depthoffset;
}

void Metal_Backend_SetColor(float r, float g, float b, float a)
{
	mb_state.color4f[0] = r;
	mb_state.color4f[1] = g;
	mb_state.color4f[2] = b;
	mb_state.color4f[3] = a;
}

void Metal_Backend_SetScissor(int x, int y, int width, int height)
{
	mb_state.scissor[0] = x; mb_state.scissor[1] = y;
	mb_state.scissor[2] = width; mb_state.scissor[3] = height;
}

void Metal_Backend_SetViewport(int x, int y, int width, int height)
{
	mb_state.viewport[0] = x; mb_state.viewport[1] = y;
	mb_state.viewport[2] = width; mb_state.viewport[3] = height;
}

// ---------------------------------------------------------------------------
// render targets and clear

void Metal_Backend_SetRenderTarget(int fbo)
{
	if (fbo == mb_currentfbo)
		return;   // the common case -- R_Mesh_Start/Finish call this constantly

	// Changing targets. Two disciplines, both learned the cheap way in review
	// rather than the expensive way in a black frame:
	//
	// PENDING-CLEAR OWNERSHIP: a clear registered while target A was current
	// must not become target B's load action. GL semantics are that a clear
	// applies to the framebuffer bound WHEN IT WAS ISSUED, so realise any
	// pending clear against the old target now -- mb_ensure_encoder consumes
	// the pending mask into load actions, and ending immediately makes it a
	// clear-only pass. Rare (clear-then-switch-without-drawing), so the empty
	// pass costs nothing in practice.
	if (mb_pendingclearmask)
	{
		if (mb_ensure_encoder())
			mb_end_encoder();
		mb_pendingclearmask = 0;
	}
	// ENCODER LIFETIME: a live encoder's attachments cannot be re-pointed;
	// end it so the next draw lazily opens one against the new target.
	mb_end_encoder();

	if (fbo == 0)
	{
		mb_currentfbo   = 0;
		mb_targetcolor  = mb_screentex;
		mb_targetdepth  = mb_screendepth;
		mb_targetwidth  = mb_screenw;
		mb_targetheight = mb_screenh;
		return;
	}
	if (!mb_fbo_select(fbo))
	{
		if (!mb_warned_fbo)
		{
			mb_warned_fbo = 1;
			Con_Printf(CON_WARN "Metal_Backend: unknown framebuffer handle %d; rendering continues to the screen\n", fbo);
		}
		mb_currentfbo   = 0;
		mb_targetcolor  = mb_screentex;
		mb_targetdepth  = mb_screendepth;
		mb_targetwidth  = mb_screenw;
		mb_targetheight = mb_screenh;
	}
}

int Metal_Backend_CreateFramebufferObject(int depthtexnum, int colortexnum0, int colortexnum1, int colortexnum2, int colortexnum3)
{
	int i;
	if (!mb_started)
		return 0;
	for (i = 1; i < MB_MAX_FBOS; i++)
	{
		if (mb_fbos[i].used)
			continue;
		mb_fbos[i].colorhandle[0] = colortexnum0;
		mb_fbos[i].colorhandle[1] = colortexnum1;
		mb_fbos[i].colorhandle[2] = colortexnum2;
		mb_fbos[i].colorhandle[3] = colortexnum3;
		mb_fbos[i].depthhandle    = depthtexnum;
		mb_fbos[i].used = true;
		if (colortexnum1 && !mb_warned_fbo)
		{
			// MRT is deferred-lighting territory, dead in this fork's config;
			// say so once rather than silently dropping attachments 1-3
			mb_warned_fbo = 1;
			Con_Printf(CON_WARN "Metal_Backend: multiple colour attachments requested; only attachment 0 renders (deferred is not ported)\n");
		}
		return i;
	}
	Con_Printf(CON_ERROR "Metal_Backend: out of framebuffer handles (%d)\n", MB_MAX_FBOS);
	return 0;
}

void Metal_Backend_DestroyFramebufferObject(int fbo)
{
	if (fbo <= 0 || fbo >= MB_MAX_FBOS)
		return;
	// if the dying FBO is current, fall back to the screen pair first
	if (mb_currentfbo == fbo)
		Metal_Backend_SetRenderTarget(0);
	memset(&mb_fbos[fbo], 0, sizeof(mb_fbos[fbo]));
}

void Metal_Backend_CopyToTexture(int texnum, int tx, int ty, int sx, int sy, int width, int height)
{
	// glCopyTexSubImage2D: rectangle of the framebuffer into a texture. Both
	// sides are GL-layout images (row 0 = bottom), so rows copy straight
	// across. The blit encoder needs its own pass, so the current encoder ends;
	// the next draw lazily reopens one, exactly as a target switch does.
	id<MTLTexture> dst = texnum ? (__bridge id<MTLTexture>)Metal_Texture_GetTexture(texnum) : nil;
	id<MTLBlitCommandEncoder> blit;
	if (!dst || !mb_targetcolor || width <= 0 || height <= 0)
		return;
	if (sx < 0 || sy < 0 || sx + width > mb_targetwidth || sy + height > mb_targetheight)
		return;
	if (tx < 0 || ty < 0 || tx + width > (int)dst.width || ty + height > (int)dst.height)
		return;
	mb_end_encoder();
	if (!mb_cb)
	{
		mb_cb = [mb_queue commandBuffer];
		mb_cb.label = @"QuakeM5 backend";
	}
	// A BLIT ENCODER CANNOT CONVERT (METAL.md Phase 7-5). Under EDR fbo 0 is
	// RGBA16Float while both destinations of this call -- the loading screen's
	// copy of the frame and motion blur's ghost buffer -- are 8-bit
	// TEXTYPE_COLORBUFFER, and copyFromTexture:toTexture: across differing
	// formats is a validation failure, not a conversion. Draw instead, which is
	// available because both destinations carry TEXF_RENDERTARGET; the one that
	// does not would be refused loudly below rather than crashing the encoder.
	if (dst.pixelFormat != mb_targetcolor.pixelFormat)
	{
		mb_pipelinekey_t key;
		id<MTLRenderPipelineState> pso;
		MTLRenderPassDescriptor *rp;
		id<MTLRenderCommandEncoder> enc;
		MTLViewport vp;
		float params[4];
		if (!(dst.usage & MTLTextureUsageRenderTarget))
		{
			if (!mb_warned_copytex)
			{
				mb_warned_copytex = 1;
				Con_Printf(CON_ERROR "Metal_Backend: cannot copy a %d-format framebuffer into a %d-format texture that is not a render target; the copy is skipped\n",
					(int)mb_targetcolor.pixelFormat, (int)dst.pixelFormat);
			}
			return;
		}
		memset(&key, 0, sizeof(key));
		key.program     = MB_PROGRAM_BLIT;
		key.colormask   = 15;
		key.blendsrc    = GL_ONE;
		key.blenddst    = GL_ZERO;
		key.colorformat = (unsigned short)dst.pixelFormat;
		key.depthformat = (unsigned short)MTLPixelFormatInvalid;
		key.samplecount = 1;
		pso = mb_pipeline_get(&key);
		if (!pso)
			return;
		rp = [MTLRenderPassDescriptor renderPassDescriptor];
		rp.colorAttachments[0].texture     = dst;
		// LOAD, not DontCare: this writes a sub-rectangle through a viewport
		// and the rest of the destination must survive, which is what
		// glCopyTexSubImage2D means by "Sub".
		rp.colorAttachments[0].loadAction  = MTLLoadActionLoad;
		rp.colorAttachments[0].storeAction = MTLStoreActionStore;
		enc = [mb_cb renderCommandEncoderWithDescriptor:rp];
		if (!enc)
			return;
		params[0] = (float)sx / (float)mb_targetcolor.width;
		params[1] = (float)sy / (float)mb_targetcolor.height;
		params[2] = (float)width  / (float)mb_targetcolor.width;
		params[3] = (float)height / (float)mb_targetcolor.height;
		vp.originX = tx; vp.originY = ty; vp.width = width; vp.height = height;
		vp.znear = 0.0; vp.zfar = 1.0;
		[enc setViewport:vp];
		[enc setCullMode:MTLCullModeNone];
		[enc setRenderPipelineState:pso];
		[enc setVertexBytes:params length:sizeof(params) atIndex:16];
		[enc setFragmentTexture:mb_targetcolor atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[enc endEncoding];
		return;
	}
	blit = [mb_cb blitCommandEncoder];
	[blit copyFromTexture:mb_targetcolor
	          sourceSlice:0
	          sourceLevel:0
	         sourceOrigin:MTLOriginMake((NSUInteger)sx, (NSUInteger)sy, 0)
	           sourceSize:MTLSizeMake((NSUInteger)width, (NSUInteger)height, 1)
	            toTexture:dst
	     destinationSlice:0
	     destinationLevel:0
	    destinationOrigin:MTLOriginMake((NSUInteger)tx, (NSUInteger)ty, 0)];
	[blit endEncoding];
}

qbool Metal_Backend_SpatialUpscaleToScreen(int srchandle, int dsthandle)
{
	// METAL.md Phase 8-5: the MetalFX spatial upscale. The postprocess has
	// just been drawn into a render-res pooled intermediate; this encodes the
	// cached scaler into the frame's command buffer, writing mb_screentex
	// directly -- which is legal because 8-4 gave the screen texture the
	// scaler's queried output-usage bits unconditionally. The next draw
	// reopens fbo 0 with MTLLoadActionLoad (mb_ensure_encoder's standing
	// rule), so the HUD composites over the scaled frame; the caller's
	// DrawQ_Start does that re-selection immediately, and nothing draws in
	// between. Like Metal_Backend_CopyToTexture above, this ends the render
	// encoder by hand first -- two open encoders on one buffer is a
	// validation abort -- and never commits; the present pass at EndFrame
	// carries the ordering.
	id<MTLTexture> src;

	if (!mb_started || !mb_screentex)
		return false;
	src = (__bridge id<MTLTexture>)Metal_Texture_GetTexture(srchandle);
	if (!src)
		return false;
	mb_end_encoder();
	if (!mb_cb)
	{
		mb_cb = [mb_queue commandBuffer];
		mb_cb.label = @"QuakeM5 backend";
	}
	id<MTLTexture> dst = dsthandle ? (__bridge id<MTLTexture>)Metal_Texture_GetTexture(dsthandle) : mb_screentex;
	if (!dst)
		return false;
	if (!MetalFX_EncodeUpscale((__bridge void *)mb_cb, (__bridge void *)src, (__bridge void *)dst))
	{
		// unreachable when the caller's gate ran MetalFX_ScalerReady this
		// frame; MetalFX_EncodeUpscale has already said why, once
		return false;
	}
	return true;
}

qbool Metal_Backend_TemporalUpscaleToScreen(int srchandle, int depthhandle, int motionhandle, int reactivehandle,
                                            float jitterx, float jittery, qbool reset, int dsthandle)
{
	// The MetalFX-TEMPORAL arc. Structurally identical to the spatial upscale
	// above -- end the encoder by hand, open the frame buffer lazily, encode,
	// never commit -- with the three extra inputs a temporal scaler takes.
	// Every one of them is an ordinary pooled render target this frame, so
	// they carry RenderTarget|ShaderRead, which is what the boot probe
	// measured the scaler asking for.
	id<MTLTexture> src, dep, mot, rea;

	if (!mb_started || !mb_screentex)
		return false;
	src = (__bridge id<MTLTexture>)Metal_Texture_GetTexture(srchandle);
	dep = (__bridge id<MTLTexture>)Metal_Texture_GetTexture(depthhandle);
	mot = (__bridge id<MTLTexture>)Metal_Texture_GetTexture(motionhandle);
	rea = reactivehandle ? (__bridge id<MTLTexture>)Metal_Texture_GetTexture(reactivehandle) : nil;   // optional
	if (!src || !dep || !mot)
	{
		// SILENT FALLBACK IS THE ENEMY HERE. The caller's failure path renders a
		// perfectly correct frame through R_BlendView with no scaler at all --
		// which is FASTER than succeeding, so a silent miss shows up in a
		// benchmark as a speed-up and in the picture as nothing. Say it once.
		static qbool warned;
		if (!warned)
		{
			warned = true;
			Con_Printf(CON_WARN "MetalFX: temporal upscale skipped -- missing texture (colour %d %s, depth %d %s, motion %d %s); the frame falls back to the unscaled path\n",
				srchandle, src ? "ok" : "NULL", depthhandle, dep ? "ok" : "NULL", motionhandle, mot ? "ok" : "NULL");
		}
		return false;
	}
	mb_end_encoder();
	if (!mb_cb)
	{
		mb_cb = [mb_queue commandBuffer];
		mb_cb.label = @"QuakeM5 backend";
	}
	{
		id<MTLTexture> dst = dsthandle ? (__bridge id<MTLTexture>)Metal_Texture_GetTexture(dsthandle) : mb_screentex;
		if (!dst)
			return false;
		if (!MetalFX_EncodeTemporalUpscale((__bridge void *)mb_cb, (__bridge void *)src, (__bridge void *)dep,
	                                   (__bridge void *)mot, (__bridge void *)rea, (__bridge void *)dst,
	                                   jitterx, jittery, reset))
			return false;
	}
	return true;
}

void Metal_Backend_Clear(int mask, const float *colorvalue, float depthvalue, int stencilvalue)
{
	static const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	if (!mb_started)
		return;
	if (!colorvalue)
		colorvalue = black;

	// With no encoder open and no scissor, the cheapest and most faithful clear
	// is a load action on the pass we are about to start.
	if (!mb_enc && !mb_state.scissortest)
	{
		mb_pendingclearmask |= mask;
		if (mask & MB_CLEAR_COLOR)
			memcpy(mb_pendingclearcolor, colorvalue, sizeof(float[4]));
		if (mask & MB_CLEAR_DEPTH)
			mb_pendingcleardepth = depthvalue;
		if (mask & MB_CLEAR_STENCIL)
			mb_pendingclearstencil = stencilvalue;
		return;
	}

	// Otherwise it has to be a drawn quad. A load action would discard whatever
	// is already in the target, and a scissored clear must NOT widen to the
	// whole attachment -- that is METAL.md's named HUD-wiping trap
	// (the `if (r_fb.rt_screen) GL_ScissorTest(false)` pair around R_ClearScreen
	// in R_RenderView -- gl_rmain.c:7802-7804 at the time of writing; the old
	// :7697-7701 anchor had drifted, and METAL.md warns that anchors here do).
	// Phase 3 never reaches it; Phase 4a does, and inherits this. Note the
	// rt_screen == NULL side is dormant in HEAD: R_Bloom_StartFrame assigns
	// rt_screen unconditionally, so the guard is always taken.
	if (mask & MB_CLEAR_STENCIL && !mb_warned_stencilclear)
	{
		mb_warned_stencilclear = 1;
		Con_Printf(CON_WARN "Metal_Backend: a mid-pass stencil clear is not implemented (METAL.md Phase 4a)\n");
	}
	if (!(mask & (MB_CLEAR_COLOR | MB_CLEAR_DEPTH)))
		return;
	if (!mb_ensure_encoder())
		return;
	{
		mb_clearparams_t params;
		int savedmask = mb_state.colormask;
		int saveddepthmask = mb_state.depthmask;
		int saveddepthtest = mb_state.depthtest;
		int saveddepthfunc = mb_state.depthfunc;
		memset(&params, 0, sizeof(params));
		memcpy(params.color, colorvalue, sizeof(float[4]));
		params.depth = (mask & MB_CLEAR_DEPTH) ? depthvalue : 1.0f;
		// write only what was asked for
		mb_state.colormask = (mask & MB_CLEAR_COLOR) ? 15 : 0;
		mb_state.depthmask = (mask & MB_CLEAR_DEPTH) ? 1 : 0;
		mb_state.depthtest = (mask & MB_CLEAR_DEPTH) ? 1 : 0;
		mb_state.depthfunc = GL_ALWAYS;
		mb_apply_state(MB_PROGRAM_CLEAR);
		[mb_enc setVertexBytes:&params length:sizeof(params) atIndex:MB_BUFFERINDEX_PARAMS];
		[mb_enc setFragmentBytes:&params length:sizeof(params) atIndex:MB_BUFFERINDEX_PARAMS];
		[mb_enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		mb_state.colormask = savedmask;
		mb_state.depthmask = saveddepthmask;
		mb_state.depthtest = saveddepthtest;
		mb_state.depthfunc = saveddepthfunc;
	}
}

// ---------------------------------------------------------------------------
// buffers

int Metal_Backend_BufferUpdate(int handle, const void *data, size_t size, qbool subdata, size_t offset, qbool isdynamic, qbool isindexbuffer, const char *name)
{
	int i;
	(void)isdynamic;
	(void)isindexbuffer;
	if (!mb_dev)
		return handle;
	if (size == 0)
		return handle;

	if (handle <= 0 || handle >= MB_MAX_BUFFERS || !mb_bufs[handle].used)
	{
		// allocate a slot, mirroring GL's lazy glGenBuffers
		handle = 0;
		for (i = 1; i < MB_MAX_BUFFERS; i++)
		{
			if (!mb_bufs[i].used)
			{
				handle = i;
				if (i >= mb_numbufs)
					mb_numbufs = i + 1;
				break;
			}
		}
		if (!handle)
		{
			Con_Printf(CON_ERROR "Metal_Backend: out of buffer handles (%d)\n", MB_MAX_BUFFERS);
			return 0;
		}
		mb_bufs[handle].used = true;
		mb_bufs[handle].buf = nil;
		mb_bufs[handle].size = 0;
		subdata = false;   // nothing to update into yet
	}

	if (!subdata || !mb_bufs[handle].buf || mb_bufs[handle].size < offset + size)
	{
		// A fresh allocation rather than a resize. Any command buffer still
		// holding the old one keeps it alive under ARC, so there is nothing to
		// wait for -- the same reasoning rt_metal.m uses for its re-uploads.
		size_t newsize = subdata ? offset + size : size;
		id<MTLBuffer> nb = [mb_dev newBufferWithLength:newsize options:MTLResourceStorageModeShared];
		if (!nb)
		{
			Con_Printf(CON_ERROR "Metal_Backend: failed to allocate a %d byte buffer (%s)\n", (int)newsize, name ? name : "?");
			return handle;
		}
		if (name)
			nb.label = [NSString stringWithUTF8String:name];
		if (subdata && mb_bufs[handle].buf)
			memcpy([nb contents], [mb_bufs[handle].buf contents], mb_bufs[handle].size);
		mb_bufs[handle].buf = nb;
		mb_bufs[handle].size = newsize;
	}

	if (data)
		memcpy((unsigned char *)[mb_bufs[handle].buf contents] + (subdata ? offset : 0), data, size);
	return handle;
}

void Metal_Backend_BufferDestroy(int handle)
{
	if (handle <= 0 || handle >= MB_MAX_BUFFERS)
		return;
	mb_bufs[handle].buf  = nil;
	mb_bufs[handle].size = 0;
	mb_bufs[handle].used = false;
}

// ---------------------------------------------------------------------------
// vertex layout

static void mb_setattrib(int slot, int components, int gltype, size_t stride, const void *pointer, int bufferhandle, size_t bufferoffset)
{
	mb_attrib_t *a = mb_state.attrib + slot;
	a->enabled      = (pointer != NULL);
	a->components   = components;
	a->gltype       = gltype;
	a->stride       = stride;
	a->pointer      = pointer;
	a->bufferhandle = bufferhandle;
	a->offset       = bufferoffset;
}

void Metal_Backend_VertexPointer(int components, int gltype, size_t stride, const void *pointer, int bufferhandle, size_t bufferoffset)
{
	mb_setattrib(MB_ATTRIB_POSITION, components, gltype, stride, pointer, bufferhandle, bufferoffset);
}

void Metal_Backend_ColorPointer(int components, int gltype, size_t stride, const void *pointer, int bufferhandle, size_t bufferoffset)
{
	// pointer == NULL selects the constant-colour layout -- GL_Color's value
	// fed through a MTLVertexStepFunctionConstant slot. See mb_vertexdescriptor.
	mb_setattrib(MB_ATTRIB_COLOR, components, gltype, stride, pointer, bufferhandle, bufferoffset);
}

void Metal_Backend_TexCoordPointer(unsigned int unitnum, int components, int gltype, size_t stride, const void *pointer, int bufferhandle, size_t bufferoffset)
{
	if (unitnum >= MB_NUMATTRIBS - MB_ATTRIB_TEXCOORD0)
		return;
	mb_setattrib(MB_ATTRIB_TEXCOORD0 + (int)unitnum, components, gltype, stride, pointer, bufferhandle, bufferoffset);
}

void Metal_Backend_TexBind(unsigned int unitnum, int texhandle)
{
	if (unitnum >= MB_MAX_TEXUNITS)
	{
		if (!mb_warned_texunit)
		{
			mb_warned_texunit = 1;
			Con_Printf(CON_WARN "Metal_Backend: texture unit %u is above the %d this backend binds\n", unitnum, MB_MAX_TEXUNITS);
		}
		return;
	}
	mb_state.texunit[unitnum] = texhandle;
}

// ---------------------------------------------------------------------------
// draw

void Metal_Backend_Draw(int firstvertex, int numvertices, unsigned int numelements, int index16handle, size_t offset16, int index32handle, size_t offset32)
{
	id<MTLBuffer> ib;

	if (!mb_started)
		return;
	if (r_metal_forceencoderrestart.integer)
		mb_end_encoder();
	if (!mb_ensure_encoder())
		return;

	mb_apply_state(mb_state.program);
	if (!mb_bind_vertexbuffers(firstvertex, numvertices))
		return;

	// 16-bit first, exactly as the GL arm prefers it (gl_backend.c:1902).
	// Indices are absolute vertex numbers, so there is no base vertex to apply.
	if ((ib = mb_bufferfor(index16handle)) != nil)
	{
		[mb_enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
		                   indexCount:numelements
		                    indexType:MTLIndexTypeUInt16
		                  indexBuffer:ib
		            indexBufferOffset:offset16];
	}
	else if ((ib = mb_bufferfor(index32handle)) != nil)
	{
		[mb_enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
		                   indexCount:numelements
		                    indexType:MTLIndexTypeUInt32
		                  indexBuffer:ib
		            indexBufferOffset:offset32];
	}
	else
	{
		[mb_enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:firstvertex vertexCount:numvertices];
	}
}

// ---------------------------------------------------------------------------
// the probe

// ---------------------------------------------------------------------------
// the frame
//
// The renderer draws into a persistent GL-LAYOUT screen texture, and one present
// pass samples it v-flipped into the drawable. METAL.md's architecture, and it
// earns its keep three times over: every coordinate in the engine keeps GL's
// numerics, GL_ReadPixelsBGRA needs no flip and is therefore directly comparable
// with glReadPixels, and Phase 7's EDR mapping has exactly one place to live.

// The readback's two format helpers, defined with the readback below but needed
// by the present-pass probe above it (METAL.md Phase 7-2).
static size_t mb_bytesperpixel(MTLPixelFormat fmt);
static qbool mb_readback_convert(MTLPixelFormat fmt, const void *src, size_t npixels, unsigned char *out);

// ---------------------------------------------------------------------------
// mb_extendedstats (METAL.md Phase 7-5)
//
// UNCLAMPED statistics for one texture, which is the whole point: everything
// else that looks at a rendered frame in this tree goes through the 8-bit
// readback, where 1.0 and 4.0 are the same byte. Reports the per-channel
// maximum, how many pixels carry any channel above SDR white, and a band
// histogram, because "does anything exceed 1.0" and "by how much, and over how
// much of the frame" are different questions and only the second can be
// compared against the headroom the OS says it granted.
//
// Its own command buffer, committed and waited on: this runs at most once per
// arming, from a console command, and interleaving it with the frame's buffer
// would make it a source of the very timing artefacts it exists to measure.
static void mb_extendedstats(id<MTLTexture> tex, const char *label)
{
	static const float bands[] = { 1.0f, 1.25f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f };
	const int nbands = (int)(sizeof(bands) / sizeof(bands[0]));
	id<MTLBuffer> rb;
	id<MTLBlitCommandEncoder> blit;
	id<MTLCommandBuffer> cb;
	size_t w, h, bpp, n, i;
	int c, b, hist[8];
	float mx[4];
	size_t over = 0;

	if (!tex || !mb_dev || !mb_queue)
		return;
	w = tex.width; h = tex.height;
	bpp = mb_bytesperpixel(tex.pixelFormat);
	if (!bpp || !w || !h)
	{
		Con_Printf("r_edr_probe %s: unreadable (pixel format %d)\n", label, (int)tex.pixelFormat);
		return;
	}
	rb = [mb_dev newBufferWithLength:w * h * bpp options:MTLResourceStorageModeShared];
	if (!rb)
		return;
	mb_end_encoder();
	if (mb_cb) { [mb_cb commit]; mb_cb = nil; }
	cb = [mb_queue commandBuffer];
	cb.label = @"QuakeM5 EDR probe";
	blit = [cb blitCommandEncoder];
	[blit copyFromTexture:tex sourceSlice:0 sourceLevel:0
	         sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
	             toBuffer:rb destinationOffset:0
	destinationBytesPerRow:w * bpp destinationBytesPerImage:w * h * bpp];
	[blit endEncoding];
	[cb commit];
	[cb waitUntilCompleted];

	n = w * h;
	memset(hist, 0, sizeof(hist));
	mx[0] = mx[1] = mx[2] = mx[3] = 0.0f;
	for (i = 0; i < n; i++)
	{
		float v[4];
		float peak;
		for (c = 0; c < 4; c++)
		{
			if (tex.pixelFormat == MTLPixelFormatRGBA16Float)
				v[c] = (float)((const __fp16 *)[rb contents])[i * 4 + c];
			else if (tex.pixelFormat == MTLPixelFormatRGBA32Float)
				v[c] = ((const float *)[rb contents])[i * 4 + c];
			else
			{
				// an 8-bit texture cannot carry more than 1.0 by construction;
				// reported anyway rather than refused, because "the drawable is
				// still 8-bit" is exactly the answer this probe exists to give
				// when somebody believes EDR is on and it is not.
				const unsigned char *p = (const unsigned char *)[rb contents] + i * 4;
				int s = (tex.pixelFormat == MTLPixelFormatBGRA8Unorm) ? (c == 0 ? 2 : (c == 2 ? 0 : c)) : c;
				v[c] = p[s] / 255.0f;
			}
			if (v[c] > mx[c]) mx[c] = v[c];
		}
		peak = v[0] > v[1] ? (v[0] > v[2] ? v[0] : v[2]) : (v[1] > v[2] ? v[1] : v[2]);
		if (peak > 1.0f) over++;
		for (b = nbands - 1; b >= 0; b--)
			if (peak > bands[b]) { hist[b]++; break; }
	}
	Con_Printf("r_edr_probe %s: %zux%zu %s  max R %.3f G %.3f B %.3f A %.3f\n",
		label, w, h,
		tex.pixelFormat == MTLPixelFormatRGBA16Float ? "RGBA16Float" :
		(tex.pixelFormat == MTLPixelFormatRGBA32Float ? "RGBA32Float" : "8-bit"),
		mx[0], mx[1], mx[2], mx[3]);
	Con_Printf("r_edr_probe %s: %zu of %zu pixels above SDR white (%.3f%%)  bands >1 %d  >1.25 %d  >1.5 %d  >2 %d  >3 %d  >4 %d  >6 %d\n",
		label, over, n, 100.0 * (double)over / (double)n,
		hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6]);
}

void Metal_Backend_BeginFrame(int width, int height)
{
	MTLTextureDescriptor *td;
	// EDR (METAL.md Phase 7-5). fbo 0 is where the postprocess writes the float
	// scene, and at 8 bits it is THE clamp -- r_viewfbo 2 sizes the offscreen
	// SCENE buffer and never reaches here, so an extended-range drawable fed
	// from this texture would carry nothing above 1.0 whatever the shader did.
	//
	// The format follows vid.edr_active, which vid_metal.m sets from the ask it
	// actually made, so the two cannot drift. It is included in the recreate
	// condition below for the same reason the size is: a stale texture of the
	// wrong format is a validation failure at the first draw, not a picture
	// somebody notices.
	MTLPixelFormat want = vid.edr_active ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm;

	if (!mb_started || !mb_dev || width <= 0 || height <= 0)
		return;
	// drain last frame's autoreleased Metal objects and open this frame's
	// pool -- see the mb_framepool comment at the top of this file
	if (mb_framepool)
		objc_autoreleasePoolPop(mb_framepool);
	mb_framepool = objc_autoreleasePoolPush();
	if (!mb_screentex || mb_screenw != width || mb_screenh != height || mb_screentex.pixelFormat != want)
	{
		mb_end_encoder();
		td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:want
		                                                        width:(NSUInteger)width
		                                                       height:(NSUInteger)height
		                                                    mipmapped:NO];
		// METAL.md Phase 8-4: the MetalFX scaler's queried output-usage bits
		// are added UNCONDITIONALLY, not behind the (8-5) cvar -- this
		// recreate predicate keys only on size and format, so a
		// cvar-conditional bit would never re-mint the texture on a
		// mid-session flip and the scaler would fail validation against a
		// texture created before the cvar was set. 0 wherever MetalFX is
		// unavailable, so the usage is exactly the old pair there. The
		// scaler's output must also be private storage, which this already is.
		td.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | (MTLTextureUsage)MetalFX_OutputTextureUsage();
		td.storageMode = MTLStorageModePrivate;
		mb_screentex = [mb_dev newTextureWithDescriptor:td];
		if (!mb_screentex)
		{
			Con_Printf(CON_ERROR "Metal_Backend: could not create the %dx%d screen texture\n", width, height);
			mb_screenw = mb_screenh = 0;
			return;
		}
		mb_screentex.label = @"QuakeM5 screen";
		// The 8-4 assertion: the CREATED texture must satisfy the scaler's
		// requirement. Read back from the object rather than the descriptor,
		// so it catches both a future edit dropping the OR above and any
		// framework surprise between descriptor and texture.
		if ((mb_screentex.usage & (MTLTextureUsage)MetalFX_OutputTextureUsage()) != (MTLTextureUsage)MetalFX_OutputTextureUsage())
			Con_Printf(CON_ERROR "Metal_Backend: the screen texture's usage 0x%x does not satisfy MetalFX's outputTextureUsage 0x%x; the 8-5 scaler cannot target it\n", (unsigned int)mb_screentex.usage, MetalFX_OutputTextureUsage());
		// the matching persistent depth (METAL.md Phase 4a). Same lifecycle as
		// the colour texture: recreated together on resize, released together at
		// shutdown, and installed as a PAIR wherever fbo 0 is selected.
		td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
		                                                        width:(NSUInteger)width
		                                                       height:(NSUInteger)height
		                                                    mipmapped:NO];
		td.usage       = MTLTextureUsageRenderTarget;
		td.storageMode = MTLStorageModePrivate;
		mb_screendepth = [mb_dev newTextureWithDescriptor:td];
		if (mb_screendepth)
			mb_screendepth.label = @"QuakeM5 screen depth";
		else
			Con_Printf(CON_ERROR "Metal_Backend: could not create the %dx%d screen depth texture; 3D will not depth-test\n", width, height);
		mb_screenw = width;
		mb_screenh = height;
	}
	mb_currentfbo   = 0;
	mb_targetcolor  = mb_screentex;
	mb_targetdepth  = mb_screendepth;
	mb_targetwidth  = mb_screenw;
	mb_targetheight = mb_screenh;
}

// SEPTEMBER2 A2 (2026-09-06): same-frame RT pipelining. Under rt_metal_sameframe
// the composite hook commits the sidecar's trace and WAITS for it, while the
// whole raster encoded so far sits in mb_cb uncommitted until EndFrame -- so
// the GPU runs the trace, then the renderer's entire buffer, in series, and the
// 12-15% same-frame cost measured 2026-08-08 is exactly that lost concurrency.
// Committing mb_cb here puts the raster on mb_queue the moment the trace goes
// on s_queue; the two overlap on the GPU and only the composite (encoded into a
// fresh buffer on the same queue, so ordered after the raster) waits. Same
// work, reordered: the picture is byte-identical by construction, which the
// parity bed measures. The encoder-restart is the ProfileRegion path's, whose
// pixel proof is the 2026-08-16 record's. Note METAL_FRAMEMS=1 then times only
// the post-kick buffer.
void Metal_Backend_Kick(void)
{
	if (!mb_started || !mb_cb)
		return;
	mb_end_encoder();
	[mb_cb commit];
	mb_cb = nil;
}

void Metal_Backend_ProfileRegion(int begin, const char *label)
{
	if (mb_framems < 2 || !mb_started || !mb_queue)   // 2 = the murk composite region, 3 = the motion pass (gl_rmain.c); either way one region per frame
		return;
	if (begin)
	{
		// close everything encoded so far into its own buffer; the region's
		// first draw lazily opens a fresh one (mb_ensure_encoder)
		mb_end_encoder();
		if (mb_cb) { [mb_cb commit]; mb_cb = nil; }
		mb_region_label = label ? label : "";
		mb_region_open = 1;
		return;
	}
	if (!mb_region_open)
		return;
	mb_region_open = 0;
	mb_end_encoder();
	if (!mb_cb)
		return;   // the region issued no draw at all -- nothing to time
	[mb_cb addCompletedHandler:^(id<MTLCommandBuffer> cb) {
		double ms = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
		atomic_fetch_add_explicit(&mb_regionms_ns, (unsigned long long)(ms * 1.0e6), memory_order_relaxed);
		atomic_fetch_add_explicit(&mb_regionms_n, 1ull, memory_order_relaxed);
	}];
	[mb_cb commit];
	mb_cb = nil;
}

void Metal_Backend_EndFrame(void *drawableptr, int probeframe)
{
	id<CAMetalDrawable> drawable = (__bridge id<CAMetalDrawable>)drawableptr;
	MTLRenderPassDescriptor *rp;
	id<MTLRenderCommandEncoder> enc;
	mb_pipelinekey_t key;
	id<MTLRenderPipelineState> pso;
	MTLViewport vp;

	if (!drawable)
		return;
	mb_end_encoder();
	if (!mb_cb)
	{
		mb_cb = [mb_queue commandBuffer];
		mb_cb.label = @"QuakeM5 frame";
	}

	rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture     = drawable.texture;
	rp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;   // the pass covers every pixel
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	enc = [mb_cb renderCommandEncoderWithDescriptor:rp];
	if (enc)
	{
		memset(&key, 0, sizeof(key));
		key.program     = MB_PROGRAM_PRESENT;
		key.colormask   = 15;
		key.blendsrc    = GL_ONE;
		key.blenddst    = GL_ZERO;
		key.colorformat = (unsigned short)drawable.texture.pixelFormat;
		key.depthformat = (unsigned short)MTLPixelFormatInvalid;
		key.samplecount = 1;
		pso = mb_pipeline_get(&key);
		if (pso && mb_screentex)
		{
			vp.originX = 0; vp.originY = 0;
			vp.width  = (double)drawable.texture.width;
			vp.height = (double)drawable.texture.height;
			vp.znear = 0.0; vp.zfar = 1.0;
			[enc setViewport:vp];
			[enc setCullMode:MTLCullModeNone];
			[enc setRenderPipelineState:pso];
			[enc setFragmentTexture:mb_screentex atIndex:0];
			[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		}
		[enc endEncoding];
	}

	// METAL.md Phase 7-5's EDR instrument. It is the ONLY thing in this tree
	// that can see a value above 1.0: screenshots read mb_screentex through a
	// readback whose contract is 8-bit and clamped (deliberately -- see
	// mb_readback_convert), so an EDR frame and an SDR one produce identical
	// bytes there BY DESIGN. Without this, "EDR is working" would rest entirely
	// on the OS's grant, which says what was allocated and not what was written.
	//
	// It reports the SCREEN TEXTURE beside the drawable on purpose: agreement
	// between the two is what proves the extended values came through the scene
	// and survived the present pass, rather than being manufactured by it.
	if (probeframe == 2)
	{
		mb_extendedstats(mb_screentex,    "screentex");
		mb_extendedstats(drawable.texture, "drawable ");
		// Falls through to the present below -- unlike the flatness probe, this
		// one MUST present, because macOS stops granting headroom to a layer
		// that has stopped putting frames on screen. WHICH DID NOT HAPPEN until
		// Phase 8-1d: mb_extendedstats commits the frame's buffer (to order its
		// own readback after the frame's work) and nils mb_cb, so the present
		// fall-through was messaging nil and the probe frame silently never
		// reached the screen, against this comment's stated intent. The present
		// pass itself was already rendered into the drawable's texture inside
		// the committed buffer -- what was missing is only the presentDrawable
		// schedule, so an empty buffer carrying it is the whole fix.
		if (!mb_cb)
		{
			mb_cb = [mb_queue commandBuffer];
			mb_cb.label = @"QuakeM5 probe present";
		}
	}
	else if (probeframe)
	{
		// VID_METAL_PROBE: read back the DRAWABLE, not the screen texture. This
		// is the only check that covers the present pass itself -- a broken
		// present leaves a black window while every screen-texture readback,
		// including the whole parity comparison, still passes happily.
		// STRIDE FROM THE FORMAT (METAL.md Phase 7-2). This branch used to
		// hardcode 4 bytes per pixel against drawable.texture, which is the one
		// texture in the tree whose format Phase 7 is certain to move -- and a
		// half-read drawable would have reported FLAT and been read as "the
		// present pass drew nothing", i.e. the instrument accusing the thing it
		// exists to exonerate.
		size_t w = drawable.texture.width, h = drawable.texture.height;
		size_t bpp = mb_bytesperpixel(drawable.texture.pixelFormat);
		unsigned char *px = bpp ? (unsigned char *)Mem_Alloc(tempmempool, w * h * 4) : NULL;
		id<MTLBuffer> rb = bpp ? [mb_dev newBufferWithLength:w * h * bpp options:MTLResourceStorageModeShared] : nil;
		id<MTLBlitCommandEncoder> blit;
		if (!bpp || !rb)
		{
			fprintf(stderr, "VID_METAL_PROBE present %zux%zu: UNREADABLE (pixel format %d)\n",
			        w, h, (int)drawable.texture.pixelFormat);
			fflush(stderr);
			if (px) Mem_Free(px);
			return;
		}
		blit = [mb_cb blitCommandEncoder];
		[blit copyFromTexture:drawable.texture sourceSlice:0 sourceLevel:0
		         sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
		             toBuffer:rb destinationOffset:0
		destinationBytesPerRow:w * bpp destinationBytesPerImage:w * h * bpp];
		[blit endEncoding];
		[mb_cb commit];
		[mb_cb waitUntilCompleted];
		mb_cb = nil;
		if (!mb_readback_convert(drawable.texture.pixelFormat, [rb contents], w * h, px))
		{
			fprintf(stderr, "VID_METAL_PROBE present %zux%zu: UNCONVERTIBLE (pixel format %d)\n",
			        w, h, (int)drawable.texture.pixelFormat);
			fflush(stderr);
			Mem_Free(px);
			return;
		}
		{
			// count non-background pixels: a presented frame is not one colour
			size_t i, differing = 0;
			const unsigned char *first = px;
			for (i = 0; i < w * h; i++)
				if (memcmp(px + i * 4, first, 3))
					differing++;
			fprintf(stderr, "VID_METAL_PROBE present %zux%zu: %zu of %zu pixels differ from the first -> %s\n",
			        w, h, differing, w * h,
			        differing ? "A FRAME REACHED THE DRAWABLE" : "FLAT (present pass drew nothing)");
			fflush(stderr);
		}
		Mem_Free(px);
		return;   // deliberately not presented, exactly as the Phase 0 probe did
	}

	// METAL_FRAMEMS=1: the RENDERER's own GPU time per frame -- this command
	// buffer's GPUEndTime - GPUStartTime, averaged over 120 presented frames and
	// printed once per window (RT_METAL_KERNELMS's shape, gated the same way, so
	// it can never be the OOM-by-console class). The sidecar's kernels run on
	// their own queue and are NOT in this number; RT_METAL_KERNELMS reports
	// those. It exists because a pass that lives in a fragment shader (the murk
	// composite, the postprocess tail) has no stage line anywhere else -- fps is
	// presentation-throttled the moment the window loses focus, and
	// R_TimeReport is CPU wall time. A frame that flushed mid-way (readbacks,
	// probes) commits more than one buffer; only the presenting one is timed,
	// which under-reports those frames and no ordinary frame.
	if (mb_framems)
	{
		[mb_cb addCompletedHandler:^(id<MTLCommandBuffer> cb) {
			double ms = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
			atomic_fetch_add_explicit(&mb_framems_ns, (unsigned long long)(ms * 1.0e6), memory_order_relaxed);
			atomic_fetch_add_explicit(&mb_framems_n, 1ull, memory_order_relaxed);
		}];
		if ((++mb_framems_frames % 120) == 0)
		{
			unsigned long long n  = atomic_exchange_explicit(&mb_framems_n, 0ull, memory_order_relaxed);
			unsigned long long ns = atomic_exchange_explicit(&mb_framems_ns, 0ull, memory_order_relaxed);
			if (n)
				fprintf(stderr, "Metal_Backend: frame gpu %.3f ms avg over %llu frames (renderer command buffer only; sidecar kernels excluded)\n", (double)ns / (1.0e6 * (double)n), n);
			if (mb_framems >= 2)
			{
				unsigned long long rn  = atomic_exchange_explicit(&mb_regionms_n, 0ull, memory_order_relaxed);
				unsigned long long rns = atomic_exchange_explicit(&mb_regionms_ns, 0ull, memory_order_relaxed);
				if (rn)
					fprintf(stderr, "Metal_Backend: region '%s' gpu %.3f ms avg over %llu buffers\n", mb_region_label, (double)rns / (1.0e6 * (double)rn), rn);
			}
		}
	}

	// present WITHOUT waiting -- mb_flush's waitUntilCompleted is right for a
	// readback and would pin the frame rate to a full GPU round trip here
	[mb_cb presentDrawable:drawable];
	[mb_cb commit];
	mb_cb = nil;
}

// ---------------------------------------------------------------------------
// the readback, and its stride (METAL.md Phase 7-2)
//
// THE SIZE OF A PIXEL WAS A LITERAL 4 IN FOUR PLACES and every one of them was
// right, because every texture in the tree was BGRA8. Phase 7 moves fbo 0 and
// the drawable to RGBA16Float, at which point a hardcoded 4 does not fail: it
// blits half the rows, silently, and the readback is the ONLY instrument
// covering the present pass. So the stride comes from the format, before any
// format moves -- which is the whole of this slice, and it is provably inert
// while nothing is 16F yet.
//
// Refused BY NAME rather than guessed at, the metal_textures.m house style: a
// format this does not know is a readback that must not happen, not one that
// should proceed on a plausible-looking number.
static size_t mb_bytesperpixel(MTLPixelFormat fmt)
{
	switch (fmt)
	{
	case MTLPixelFormatBGRA8Unorm:
	case MTLPixelFormatBGRA8Unorm_sRGB:
	case MTLPixelFormatRGBA8Unorm:
	case MTLPixelFormatRGBA8Unorm_sRGB:
		return 4;
	case MTLPixelFormatRGBA16Float:
		return 8;
	case MTLPixelFormatRGBA32Float:
		return 16;
	default:
		return 0;
	}
}

// Convert a readback staging buffer into the FOUR-BYTE BGRA8 contract that
// GL_ReadPixelsBGRA and all three of its callers are built on.
//
// THE 8-BIT CONTRACT IS A DECISION, NOT AN OVERSIGHT (METAL.md's Phase 7 status
// block records it in full): every acceptance threshold in this arc, every
// number in test/tgacmp.py and test/lookmetrics.py, is denominated in 8-bit
// code units. A 16-bit readback would retire all of them at once and make Phase
// 7's own acceptance unevaluable. So an extended-range drawable is CLAMPED here
// -- the screenshot is the SDR image of an EDR frame, which is exactly what a
// parity instrument wants and exactly what a screenshot has always been.
//
// The BGRA8 arm is a straight memcpy and is therefore the old code byte for
// byte; the others are new and unreachable until 7-5 moves a format.
static qbool mb_readback_convert(MTLPixelFormat fmt, const void *src, size_t npixels, unsigned char *out)
{
	size_t i;
	switch (fmt)
	{
	case MTLPixelFormatBGRA8Unorm:
	case MTLPixelFormatBGRA8Unorm_sRGB:
		memcpy(out, src, npixels * 4);
		return true;
	case MTLPixelFormatRGBA8Unorm:
	case MTLPixelFormatRGBA8Unorm_sRGB:
		for (i = 0; i < npixels; i++)
		{
			const unsigned char *p = (const unsigned char *)src + i * 4;
			out[i * 4 + 0] = p[2];
			out[i * 4 + 1] = p[1];
			out[i * 4 + 2] = p[0];
			out[i * 4 + 3] = p[3];
		}
		return true;
	case MTLPixelFormatRGBA16Float:
		// __fp16 is a native arm64 type under Apple clang, so the decode is a
		// load rather than a hand-rolled bit unpack.
		for (i = 0; i < npixels; i++)
		{
			const __fp16 *p = (const __fp16 *)src + i * 4;
			int c;
			for (c = 0; c < 4; c++)
			{
				float v = (float)p[c];
				// NaN fails both comparisons and lands on 0, deliberately: a
				// NaN in the drawable would otherwise convert to whatever the
				// cast happens to produce, which is not something a byte gate
				// should ever be asked to interpret.
				v = (v > 0.0f) ? ((v < 1.0f) ? v : 1.0f) : 0.0f;
				out[i * 4 + (c == 0 ? 2 : (c == 2 ? 0 : c))] = (unsigned char)(v * 255.0f + 0.5f);
			}
		}
		return true;
	case MTLPixelFormatRGBA32Float:
		for (i = 0; i < npixels; i++)
		{
			const float *p = (const float *)src + i * 4;
			int c;
			for (c = 0; c < 4; c++)
			{
				float v = p[c];
				v = (v > 0.0f) ? ((v < 1.0f) ? v : 1.0f) : 0.0f;
				out[i * 4 + (c == 0 ? 2 : (c == 2 ? 0 : c))] = (unsigned char)(v * 255.0f + 0.5f);
			}
		}
		return true;
	default:
		return false;
	}
}

// Blit a texture sub-rectangle into CPU-visible memory. The one readback path,
// shared by the probe and GL_ReadPixelsBGRA -- synchronous by design, because
// both callers are verification instruments rather than frame work.
static qbool mb_readback(id<MTLTexture> tex, int x, int y, int width, int height, unsigned char *out)
{
	id<MTLBuffer> rb;
	id<MTLBlitCommandEncoder> blit;
	MTLPixelFormat fmt;
	size_t bpp, bytes;

	if (!tex || !mb_dev || !mb_queue || width <= 0 || height <= 0)
		return false;
	fmt = tex.pixelFormat;
	bpp = mb_bytesperpixel(fmt);
	if (!bpp)
	{
		if (!mb_warned_readback)
		{
			mb_warned_readback = 1;
			Con_Printf(CON_ERROR "Metal_Backend: a pixel readback was asked for from an unsupported pixel format (%d); "
				"add it to mb_bytesperpixel and mb_readback_convert rather than letting it guess\n", (int)fmt);
		}
		return false;
	}
	bytes = (size_t)width * (size_t)height * bpp;

	mb_end_encoder();
	if (!mb_cb)
	{
		mb_cb = [mb_queue commandBuffer];
		mb_cb.label = @"QuakeM5 backend readback";
	}
	rb = [mb_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
	if (!rb)
		return false;
	blit = [mb_cb blitCommandEncoder];
	[blit copyFromTexture:tex
	          sourceSlice:0
	          sourceLevel:0
	         sourceOrigin:MTLOriginMake((NSUInteger)x, (NSUInteger)y, 0)
	           sourceSize:MTLSizeMake((NSUInteger)width, (NSUInteger)height, 1)
	             toBuffer:rb
	    destinationOffset:0
	destinationBytesPerRow:(NSUInteger)width * bpp
	destinationBytesPerImage:bytes];
	[blit endEncoding];
	[mb_cb commit];
	[mb_cb waitUntilCompleted];
	mb_cb = nil;
	return mb_readback_convert(fmt, [rb contents], (size_t)width * (size_t)height, out);
}

qbool Metal_Backend_ReadPixels(int x, int y, int width, int height, unsigned char *outpixels)
{
	// The target holds a GL-LAYOUT image -- row 0 at the bottom -- because
	// R_Viewport_InitOrtho's Metal arm negates the projection's y row. So a GL
	// rect indexes the same rows Metal wrote and the result needs no flip: this
	// is the readback behaving as if it were glReadPixels, which is what lets
	// SCR_ScreenShot stay completely backend-agnostic.
	if (!mb_targetcolor)
	{
		// deterministic black beats stack garbage, exactly as the old stub did
		memset(outpixels, 0, (size_t)width * (size_t)height * 4);
		if (!mb_warned_readback)
		{
			mb_warned_readback = 1;
			Con_Printf(CON_WARN "Metal_Backend: a pixel readback was asked for with no colour target bound; returning black\n");
		}
		return false;
	}
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x + width  > mb_targetwidth)  width  = mb_targetwidth  - x;
	if (y + height > mb_targetheight) height = mb_targetheight - y;
	if (width <= 0 || height <= 0)
	{
		memset(outpixels, 0, 4);
		return false;
	}
	if (!mb_readback(mb_targetcolor, x, y, width, height, outpixels))
	{
		memset(outpixels, 0, (size_t)width * (size_t)height * 4);
		return false;
	}
	return true;
}

// METAL.md Phase 8-1a. The dump instrument's pixel source, by name: fbo 0's
// colour is what the present pass will show, and reading it here rather than
// through Metal_Backend_ReadPixels pins the source to the screen texture
// itself -- ReadPixels follows mb_targetcolor, which is fbo 0 at VID_Finish
// time today but moves the moment Phase 8-5 points R_BlendView at a pooled
// intermediate, and an instrument whose source follows the last
// SetRenderTargets call would break silently under the very slice it exists
// to judge. The size check refuses rather than clips: the caller sizes its
// buffer from vid.mode.*, the same values BeginFrame sized the texture with,
// so a mismatch is a bug upstream and not a rectangle to negotiate.
qbool Metal_Backend_ReadScreenBGRA(int width, int height, unsigned char *outpixels)
{
	if (!mb_screentex)
		return false;
	if (width != mb_screenw || height != mb_screenh)
		return false;
	return mb_readback(mb_screentex, 0, 0, width, height, outpixels);
}

qbool Metal_Backend_ProbeBegin(int width, int height)
{
	MTLTextureDescriptor *td;

	if (!mb_started || !mb_dev)
	{
		Con_Printf(CON_ERROR "Metal_Backend: the backend is not started\n");
		return false;
	}
	mb_flush();

	td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
	                                                        width:(NSUInteger)width
	                                                       height:(NSUInteger)height
	                                                    mipmapped:NO];
	td.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	td.storageMode = MTLStorageModePrivate;
	mb_probetarget = [mb_dev newTextureWithDescriptor:td];
	if (!mb_probetarget)
	{
		Con_Printf(CON_ERROR "Metal_Backend: could not create the %dx%d probe target\n", width, height);
		return false;
	}

	mb_probesaved      = mb_targetcolor;
	mb_probesaveddepth = mb_targetdepth;
	mb_probesavedw     = mb_targetwidth;
	mb_probesavedh     = mb_targetheight;
	mb_probesavedfbo   = mb_currentfbo;
	mb_targetcolor = mb_probetarget;
	mb_targetdepth = nil;   // the probe is 2D; no depth attachment
	mb_targetwidth  = mb_probew = width;
	mb_targetheight = mb_probeh = height;

	// deliberately NOT Metal_Backend_ResetState(): that would desync this
	// shadow from gl_state, and gl_backend.c's setters early-out against
	// gl_state, so the very next GL_BlendFunc or GL_DepthTest the probe makes
	// could be skipped before it ever reached us. The probe sets what it needs
	// through the real entry points instead, and the early-outs are then
	// correct for both shadows because they agree.
	mb_state.program = MB_PROGRAM_PROBE2D;
	Metal_Backend_SetViewport(0, 0, width, height);   // gl_state does not mirror the viewport
	return true;
}

qbool Metal_Backend_ProbeEnd(unsigned char *outBGRA)
{
	qbool ok;

	if (!mb_probetarget)
		return false;
	ok = mb_readback(mb_probetarget, 0, 0, mb_probew, mb_probeh, outBGRA);

	mb_targetcolor  = mb_probesaved;
	mb_targetdepth  = mb_probesaveddepth;
	mb_targetwidth  = mb_probesavedw;
	mb_targetheight = mb_probesavedh;
	mb_currentfbo   = mb_probesavedfbo;
	mb_probetarget = nil;
	mb_probesaved  = nil;
	mb_probesaveddepth = nil;
	mb_state.program = MB_PROGRAM_SENTINEL;
	return ok;
}

// ---------------------------------------------------------------------------
// r_metal_drawprobe
//
// METAL.md Phase 3 acceptance. Nothing renders on this path yet, so a log with
// no errors in it proves nothing whatsoever: a pipeline that silently failed to
// build and one that drew perfectly produce identical output. This drives three
// quads through the REAL gl_backend.h entry points into a scratch target and
// reads the result back off the GPU, which is the r_metal_textureprobe pattern
// one layer up.
//
// What the three quads are for, jointly:
//   A  the CONSTANT-colour vertex layout (GL_Color with a NULL colour array)
//   B  the colour-ARRAY layout, in a different texel of the same texture, so
//      the two vertex descriptors and the texcoord slot are both exercised
//   C  A again with alpha blending, which is the only one of the three that can
//      fail if the blend state is recorded but never reaches the pipeline key
// and, across the pair of runs, that all of it survives an encoder restart.

#define MBP_W 64
#define MBP_H 64

// The 2x2 source texture, BGRA bytes, deliberately asymmetric so a transposed
// or channel-swapped readback cannot compare equal by accident. Every value a
// quad multiplies is EVEN, so the expected results below are exact in 8 bits
// rather than a rounding argument.
static const unsigned char mbp_texels[16] =
{
	 64, 128, 192, 255,    /* texel (0,0)  B G R A */
	 32,  96, 160, 255,    /* texel (1,0) */
	  3,   7,  11, 255,    /* texel (0,1) -- never sampled, present to break symmetry */
	250, 251, 252, 255,    /* texel (1,1) -- likewise */
};

static const float mbp_clear[4]   = {51.0f/255.0f, 102.0f/255.0f, 153.0f/255.0f, 1.0f};  // RGBA
static const float mbp_colorA[4]  = {0.5f,  0.5f, 0.5f,  1.0f};
static const float mbp_colorB[4]  = {0.25f, 0.5f, 0.75f, 1.0f};
static const float mbp_colorC[4]  = {1.0f,  1.0f, 1.0f,  0.5f};

// The expected image, computed the way the GPU computes it. Region layout, and
// the ONE convention this depends on: Metal's NDC y = +1 is the top of the
// viewport, i.e. row 0 of the target, and the readback's row 0 is the target's
// row 0. So clip y in [0,1] is the top half and [-1,0] the bottom half.
static void mbp_expected(int x, int y, float *rgba)
{
	const unsigned char *t;
	float src[4], a;
	int i;

	if (y < MBP_H / 2)
	{
		// Top half: the CULL/WINDING probe (METAL.md Phase 4a). Two quads fed
		// as GL-clip-space coordinates with the projection's y-negation applied
		// by hand -- the exact composition the world path uses -- drawn under
		// GL_CullFace(GL_BACK). D is wound front-facing by GL's rule (CCW in
		// y-up clip space) and must SURVIVE; E is the same quad with reversed
		// elements and must be CULLED. This is the empirical answer to whether
		// MTLWindingClockwise composes correctly with the y-negated projection
		// -- the one assumption nothing before Phase 4a ever exercised, whose
		// failure mode is every wall in the game inside-out.
		if (x < MBP_W / 2)
		{
			const unsigned char *t = mbp_texels;   // texel (0,0), as quad A
			rgba[0] = 0.25f * (t[2] / 255.0f);
			rgba[1] = 0.25f * (t[1] / 255.0f);
			rgba[2] = 0.25f * (t[0] / 255.0f);
			rgba[3] = 1.0f;   // GL_Color alpha 1 x texel alpha 1
		}
		else
			memcpy(rgba, mbp_clear, sizeof(float[4]));   // E: culled, clear survives
		return;
	}
	if (x >= MBP_W / 2)  // quad B: colour array x texel (1,0)
	{
		t = mbp_texels + 4;
		rgba[0] = mbp_colorB[0] * (t[2] / 255.0f);
		rgba[1] = mbp_colorB[1] * (t[1] / 255.0f);
		rgba[2] = mbp_colorB[2] * (t[0] / 255.0f);
		rgba[3] = mbp_colorB[3] * (t[3] / 255.0f);
		return;
	}
	// quad A: constant colour x texel (0,0) ...
	t = mbp_texels;
	rgba[0] = mbp_colorA[0] * (t[2] / 255.0f);
	rgba[1] = mbp_colorA[1] * (t[1] / 255.0f);
	rgba[2] = mbp_colorA[2] * (t[0] / 255.0f);
	rgba[3] = mbp_colorA[3] * (t[3] / 255.0f);
	// ... then quad C blended over it with GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA
	src[0] = mbp_colorC[0] * (t[2] / 255.0f);
	src[1] = mbp_colorC[1] * (t[1] / 255.0f);
	src[2] = mbp_colorC[2] * (t[0] / 255.0f);
	src[3] = mbp_colorC[3] * (t[3] / 255.0f);
	a = src[3];
	for (i = 0; i < 4; i++)
		rgba[i] = src[i] * a + rgba[i] * (1.0f - a);
}

// one quad, through the real public entry points
static void mbp_quad(float x0, float x1, float y0, float y1, float s, float t, const float *color4, rtexture_t *tex)
{
	float verts[4 * 3], texcoords[4 * 2], colors[4 * 4];
	unsigned short elements[6] = {0, 1, 2, 0, 2, 3};
	int i;

	verts[0] = x0; verts[1]  = y0; verts[2]  = 0.5f;
	verts[3] = x1; verts[4]  = y0; verts[5]  = 0.5f;
	verts[6] = x1; verts[7]  = y1; verts[8]  = 0.5f;
	verts[9] = x0; verts[10] = y1; verts[11] = 0.5f;
	for (i = 0; i < 4; i++)
	{
		// one texel, squarely -- nearest filtering plus a fixed texcoord means
		// the sample is a known byte rather than a filtering argument
		texcoords[i * 2 + 0] = s;
		texcoords[i * 2 + 1] = t;
		if (color4)
			memcpy(colors + i * 4, color4, sizeof(float[4]));
	}

	// a NULL colour array is the request for the CONSTANT-colour layout, and
	// is the whole reason this helper takes a nullable pointer
	R_Mesh_PrepareVertices_Generic_Arrays(4, verts, color4 ? colors : NULL, texcoords);
	R_Mesh_TexBind(0, tex);
	R_Mesh_Draw(0, 4, 0, 2, NULL, NULL, 0, elements, NULL, 0);
}

// One cull-probe quad: coordinates given in GL CLIP SPACE, the projection's
// y-negation applied here by hand (the probe's passthrough shader has no
// matrix), reversed winding on request. Same texel-and-constant-colour shape as
// quad A so the expected value stays exact in 8 bits.
static void mbp_cullquad(float x0, float x1, float ygl0, float ygl1, qbool reversewinding, rtexture_t *tex)
{
	float verts[4 * 3], texcoords[4 * 2];
	unsigned short fwd[6] = {0, 1, 2, 0, 2, 3};
	unsigned short rev[6] = {2, 1, 0, 3, 2, 0};
	int i;

	// GL-clip-space corners, wound CCW in y-up space (GL's front face), then
	// y negated exactly as R_Viewport_Init*'s Metal arm negates the projection
	verts[0] = x0; verts[1]  = -ygl0; verts[2]  = 0.5f;
	verts[3] = x1; verts[4]  = -ygl0; verts[5]  = 0.5f;
	verts[6] = x1; verts[7]  = -ygl1; verts[8]  = 0.5f;
	verts[9] = x0; verts[10] = -ygl1; verts[11] = 0.5f;
	for (i = 0; i < 4; i++)
	{
		texcoords[i * 2 + 0] = 0.25f;
		texcoords[i * 2 + 1] = 0.25f;
	}
	GL_Color(0.25f, 0.25f, 0.25f, 1.0f);
	R_Mesh_PrepareVertices_Generic_Arrays(4, verts, NULL, texcoords);
	R_Mesh_TexBind(0, tex);
	R_Mesh_Draw(0, 4, 0, 2, NULL, NULL, 0, reversewinding ? rev : fwd, NULL, 0);
}

static qbool mbp_run(unsigned char *out)
{
	rtexturepool_t *pool;
	rtexture_t *tex;

	pool = R_AllocTexturePool();
	tex = R_LoadTexture2D(pool, "metaldrawprobe", 2, 2, mbp_texels, TEXTYPE_BGRA,
	                      TEXF_CLAMP | TEXF_FORCENEAREST, -1, NULL);
	if (!tex)
	{
		Con_Printf(CON_ERROR "r_metal_drawprobe: R_LoadTexture2D returned NULL\n");
		R_FreeTexturePool(&pool);
		return false;
	}
	R_RealGetTexture(tex);   // force the upload, exactly as a bind would

	if (!Metal_Backend_ProbeBegin(MBP_W, MBP_H))
	{
		R_FreeTexturePool(&pool);
		return false;
	}

	// state, through the real setters. Each is set to a value that differs from
	// the reset state somewhere, so none of gl_backend.c's early-outs can hide
	// a missing Metal arm.
	GL_DepthTest(false);
	GL_DepthMask(false);
	GL_DepthRange(0, 1);
	GL_CullFace(GL_NONE);
	GL_ColorMask(1, 1, 1, 1);
	GL_ScissorTest(false);
	GL_BlendFunc(GL_ONE, GL_ZERO);
	GL_Clear(GL_COLOR_BUFFER_BIT, mbp_clear, 1.0f, 0);

	// A -- constant-colour layout. GL_Color's value with a NULL colour array is
	// what Metal spells MTLVertexStepFunctionConstant.
	GL_Color(mbp_colorA[0], mbp_colorA[1], mbp_colorA[2], mbp_colorA[3]);
	mbp_quad(-1.0f, 0.0f, -1.0f, 0.0f, 0.25f, 0.25f, NULL, tex);

	// B -- colour-array layout, second texel. A different vertex descriptor and
	// therefore a different pipeline key.
	mbp_quad(0.0f, 1.0f, -1.0f, 0.0f, 0.75f, 0.25f, mbp_colorB, tex);

	// C -- A's region again, blended. If the blend state were recorded but
	// never reached the pipeline, this would overwrite A instead of mixing.
	GL_BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	GL_Color(mbp_colorC[0], mbp_colorC[1], mbp_colorC[2], mbp_colorC[3]);
	mbp_quad(-1.0f, 0.0f, -1.0f, 0.0f, 0.25f, 0.25f, NULL, tex);

	GL_BlendFunc(GL_ONE, GL_ZERO);

	// D and E -- the cull/winding pair (see mbp_expected). GL clip y in [-1,0]
	// negates into Metal clip [0,1], which the matrix-less probe path maps to
	// the top half of the target. (First cut had the sign backwards and painted
	// D over quad A -- which the probe caught on its very first run, and the
	// culling verdict was readable even through the region mix-up: D survived,
	// E vanished, exactly GL's answer.)
	GL_CullFace(GL_BACK);
	mbp_cullquad(-1.0f, 0.0f, -1.0f, 0.0f, false, tex);   // D: GL-front, must survive
	mbp_cullquad( 0.0f, 1.0f, -1.0f, 0.0f, true,  tex);   // E: reversed, must be culled
	GL_CullFace(GL_NONE);

	if (!Metal_Backend_ProbeEnd(out))
	{
		R_FreeTexturePool(&pool);
		return false;
	}
	R_FreeTexturePool(&pool);
	return true;
}

// When it fails, say WHERE. A byte count alone cannot distinguish a wrong
// colour from a vertically flipped image, and those want opposite fixes.
static void mbp_printmap(const unsigned char *img)
{
	int bx, by, x, y, i;
	char line[16];
	Con_Printf("r_metal_drawprobe: 8x8 block map (. matches, A/B/C/- the region that DOES match, ? none)\n");
	for (by = 0; by < 8; by++)
	{
		for (bx = 0; bx < 8; bx++)
		{
			float want[4];
			const unsigned char *p;
			int match = 1;
			x = bx * (MBP_W / 8) + (MBP_W / 16);
			y = by * (MBP_H / 8) + (MBP_H / 16);
			p = img + ((size_t)y * MBP_W + x) * 4;
			mbp_expected(x, y, want);
			for (i = 0; i < 4; i++)
			{
				int w = (int)(want[i] * 255.0f + 0.5f);
				int g = p[i == 0 ? 2 : (i == 1 ? 1 : (i == 2 ? 0 : 3))];
				if (abs(w - g) > 1)
					match = 0;
			}
			line[bx] = match ? '.' : '?';
		}
		line[8] = 0;
		Con_Printf("  %s\n", line);
	}
}

static void R_Metal_DrawProbe_f(cmd_state_t *cmd)
{
	unsigned char *img, *img2;
	int i, bad = 0, maxdelta = 0, restartdiff = 0;
	size_t bytes = (size_t)MBP_W * MBP_H * 4;
	(void)cmd;

	if (vid.renderpath != RENDERPATH_METAL)
	{
		Con_Printf("r_metal_drawprobe: only meaningful on the Metal path (vid_renderer metal; vid_restart)\n");
		return;
	}

	img  = (unsigned char *)Mem_Alloc(tempmempool, bytes);
	img2 = (unsigned char *)Mem_Alloc(tempmempool, bytes);
	if (!mbp_run(img))
	{
		Mem_Free(img); Mem_Free(img2);
		return;
	}

	for (i = 0; i < (int)bytes; i += 4)
	{
		float want[4];
		int c, delta;
		int x = (i / 4) % MBP_W, y = (i / 4) / MBP_W;
		mbp_expected(x, y, want);
		for (c = 0; c < 4; c++)
		{
			// the readback is BGRA; want[] is RGBA
			int got = img[i + (c == 0 ? 2 : (c == 1 ? 1 : (c == 2 ? 0 : 3)))];
			int exp = (int)(want[c] * 255.0f + 0.5f);
			delta = abs(got - exp);
			if (delta > maxdelta)
				maxdelta = delta;
			if (delta > 1)
				bad++;
		}
	}

	// Second pass with the encoder torn down and rebuilt before every draw.
	// METAL.md's risk register names scattered state replay as the single
	// largest bug source in this design; this is the measurement for it, and it
	// is a self-comparison, so it demands byte identity rather than a tolerance.
	Cvar_SetValueQuick(&r_metal_forceencoderrestart, 1);
	if (mbp_run(img2))
		restartdiff = memcmp(img, img2, bytes) ? 1 : 0;
	else
		restartdiff = -1;
	Cvar_SetValueQuick(&r_metal_forceencoderrestart, 0);

	Con_Printf("r_metal_drawprobe: %d of %d bytes off by more than 1 (max delta %d) -- %s\n",
	           bad, (int)bytes, maxdelta, bad ? "FAIL" : "PASS");
	if (restartdiff < 0)
		Con_Printf("r_metal_drawprobe: encoder-restart pass did not run -- FAIL\n");
	else
		Con_Printf("r_metal_drawprobe: encoder-restart pass is %s -- %s\n",
		           restartdiff ? "DIFFERENT" : "byte-identical", restartdiff ? "FAIL" : "PASS");
	if (bad)
		mbp_printmap(img);

	Mem_Free(img);
	Mem_Free(img2);
}

// ---------------------------------------------------------------------------
// r_metal_readbackprobe (METAL.md Phase 7-2)
//
// The stride and the conversion above are, today, entirely unreachable: every
// texture in the tree is BGRA8, so the 16F and 32F arms are dead code that will
// go live at 7-5 underneath the one instrument nobody can double-check -- the
// screenshot. Shipping them unmeasured is exactly the shape this arc refuses,
// so they get their own bed now rather than an alibi later.
//
// THE FIXTURE IS 7x5, AND THE EXTENTS ARE THE POINT. CLAUDE.md's 6-1 lesson is
// that a symmetric fixture cannot detect a symmetric bug: an NxN probe cannot
// see a width/height transposition at all, because bytesPerRow and its
// transpose are the same number. 7 and 5 are pairwise different and neither is
// a power of two, so a transposed stride (5*bpp for 7*bpp) mis-strides every
// row after the first, and a wrong bytes-per-pixel truncates the copy.
//
// The three channel formulas are pairwise different for the same reason a
// channel swap must not be able to compare equal, and the float arms carry two
// deliberate OUT-OF-RANGE texels because clamping is a claim this makes and a
// claim is not covered by in-range data.
#define MBR_W 7
#define MBR_H 5
#define MBR_N (MBR_W * MBR_H)

// expected BGRA8 for pixel i, before the float arms' clamp overrides
static void mbr_expected(int i, unsigned char *bgra)
{
	bgra[2] = (unsigned char)((i * 7) & 255);          // R
	bgra[1] = (unsigned char)((i * 13 + 5) & 255);     // G
	bgra[0] = (unsigned char)((i * 29 + 17) & 255);    // B
	bgra[3] = (unsigned char)((i * 3 + 200) & 255);    // A
}

// One format: fill a texture from a staging buffer, read it back through the
// REAL mb_readback, compare. Returns the number of bytes that differ.
static int mbr_one(MTLPixelFormat fmt, const char *name, int *maxdelta)
{
	MTLTextureDescriptor *td;
	id<MTLTexture> tex;
	id<MTLBuffer> src;
	id<MTLBlitCommandEncoder> blit;
	unsigned char want[MBR_N * 4], got[MBR_N * 4];
	size_t bpp = mb_bytesperpixel(fmt);
	int i, c, bad = 0;
	qbool isfloat = (fmt == MTLPixelFormatRGBA16Float || fmt == MTLPixelFormatRGBA32Float);

	*maxdelta = 0;
	if (!bpp)
	{
		Con_Printf(CON_ERROR "r_metal_readbackprobe: %s has no byte size -- FAIL\n", name);
		return MBR_N * 4;
	}

	td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:MBR_W height:MBR_H mipmapped:NO];
	td.usage       = MTLTextureUsageShaderRead;
	td.storageMode = MTLStorageModePrivate;
	tex = [mb_dev newTextureWithDescriptor:td];
	src = [mb_dev newBufferWithLength:MBR_N * bpp options:MTLResourceStorageModeShared];
	if (!tex || !src)
	{
		Con_Printf(CON_ERROR "r_metal_readbackprobe: %s allocation failed -- FAIL\n", name);
		return MBR_N * 4;
	}

	for (i = 0; i < MBR_N; i++)
	{
		unsigned char e[4];
		float v[4];   // in SOURCE channel order: R G B A
		mbr_expected(i, e);
		want[i * 4 + 0] = e[0]; want[i * 4 + 1] = e[1];
		want[i * 4 + 2] = e[2]; want[i * 4 + 3] = e[3];
		v[0] = e[2] / 255.0f; v[1] = e[1] / 255.0f; v[2] = e[0] / 255.0f; v[3] = e[3] / 255.0f;
		if (isfloat)
		{
			// the two clamp texels. Deliberately NOT at 0 and 1: index 0 is
			// where an off-by-one buffer slip lands, so a probe that only ever
			// checked the corners would be answering the wrong question.
			if (i == 3)  { v[0] =  4.25f; want[i * 4 + 2] = 255; }
			if (i == 19) { v[1] = -2.50f; want[i * 4 + 1] = 0;   }
		}
		for (c = 0; c < 4; c++)
		{
			if (fmt == MTLPixelFormatRGBA16Float)
				((__fp16 *)[src contents])[i * 4 + c] = (__fp16)v[c];
			else if (fmt == MTLPixelFormatRGBA32Float)
				((float *)[src contents])[i * 4 + c] = v[c];
			else
			{
				// the 8-bit arms take bytes in the format's own channel order
				unsigned char b = (unsigned char)(v[c] * 255.0f + 0.5f);
				int slot = (fmt == MTLPixelFormatBGRA8Unorm) ? (c == 0 ? 2 : (c == 2 ? 0 : c)) : c;
				((unsigned char *)[src contents])[i * 4 + slot] = b;
			}
		}
	}

	mb_end_encoder();
	if (!mb_cb)
	{
		mb_cb = [mb_queue commandBuffer];
		mb_cb.label = @"QuakeM5 readback probe";
	}
	blit = [mb_cb blitCommandEncoder];
	[blit copyFromBuffer:src sourceOffset:0
	   sourceBytesPerRow:MBR_W * bpp sourceBytesPerImage:MBR_N * bpp
	          sourceSize:MTLSizeMake(MBR_W, MBR_H, 1)
	           toTexture:tex destinationSlice:0 destinationLevel:0
	   destinationOrigin:MTLOriginMake(0, 0, 0)];
	[blit endEncoding];
	[mb_cb commit];
	[mb_cb waitUntilCompleted];
	mb_cb = nil;

	memset(got, 0xCD, sizeof(got));   // so a short copy is visible rather than plausible
	if (!mb_readback(tex, 0, 0, MBR_W, MBR_H, got))
	{
		Con_Printf(CON_ERROR "r_metal_readbackprobe: %s readback refused -- FAIL\n", name);
		return MBR_N * 4;
	}

	for (i = 0; i < MBR_N * 4; i++)
	{
		int d = abs((int)got[i] - (int)want[i]);
		if (d > *maxdelta) *maxdelta = d;
		if (d) bad++;
	}
	Con_Printf("r_metal_readbackprobe: %-14s %2d bytes/px, %d of %d bytes off (max delta %d) -- %s\n",
		name, (int)bpp, bad, MBR_N * 4, *maxdelta, bad ? "FAIL" : "ok");
	return bad;
}

static void R_Metal_ReadbackProbe_f(cmd_state_t *cmd)
{
	int bad = 0, md = 0, worst = 0;
	(void)cmd;

	if (vid.renderpath != RENDERPATH_METAL)
	{
		Con_Printf("r_metal_readbackprobe: only meaningful on the Metal path (vid_renderer metal; vid_restart)\n");
		return;
	}
	if (!mb_started || !mb_dev || !mb_queue)
	{
		Con_Printf(CON_ERROR "r_metal_readbackprobe: the backend is not started\n");
		return;
	}
	bad += mbr_one(MTLPixelFormatBGRA8Unorm,  "BGRA8Unorm",  &md); if (md > worst) worst = md;
	bad += mbr_one(MTLPixelFormatRGBA8Unorm,  "RGBA8Unorm",  &md); if (md > worst) worst = md;
	bad += mbr_one(MTLPixelFormatRGBA16Float, "RGBA16Float", &md); if (md > worst) worst = md;
	bad += mbr_one(MTLPixelFormatRGBA32Float, "RGBA32Float", &md); if (md > worst) worst = md;
	Con_Printf("r_metal_readbackprobe: %d bytes off across four formats (max delta %d) -- %s\n",
		bad, worst, bad ? "FAIL" : "PASS");
}

void Metal_Backend_RegisterCvars(void)
{
	Cvar_RegisterVariable(&r_metal_forceencoderrestart);
	Cmd_AddCommand(CF_CLIENT, "r_metal_drawprobe", &R_Metal_DrawProbe_f, "METAL.md Phase 3: drive three quads through the real gl_backend entry points into a scratch Metal target and read them back, proving the state cache, both vertex layouts, the pipeline cache, blending and encoder lifetime actually work");
	Cmd_AddCommand(CF_CLIENT, "r_metal_readbackprobe", &R_Metal_ReadbackProbe_f, "METAL.md Phase 7-2: fill a deliberately asymmetric 7x5 texture in each colour format the readback claims to handle and read it back through the real path, proving the stride, the half/float decode, the channel order and the clamp before Phase 7 moves a format");
}

#else // the feature is off

// Nothing here on purpose. metal_backend.h / metal_textures.h supply
// static-inline no-ops when USE_METAL_RENDERER is absent, so the stub bodies
// that used to live here would be a second, conflicting definition -- and a
// hand-kept parallel list of names, which is the duplication shape this project
// keeps getting burned by. The header owns the stubs; this file owns the real
// implementation and nothing else.

#endif
