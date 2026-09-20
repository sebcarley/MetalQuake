/*
QuakeM5 -- the Metal drawing backend (METAL.md Phase 3, slice 3).

Plain C, in the rt_metal.h / metal_textures.h mould: no Objective-C or Metal
types cross this header, so gl_backend.c stays a C translation unit.

DIVISION OF LABOUR. gl_backend.c remains the backend's front door on both
paths. Its ~50 public entry points are already backend-agnostic -- the drawing
code (gl_rsurf.c, gl_draw.c, sbar.c, menu.c, ft2.c and the rest) makes no raw GL
calls at all -- and its bookkeeping (the mesh-buffer expandable array, the
firsttriangle arithmetic, the 16-bit-preferred index selection, gl_paranoid's
index validation and the r_stat_draws* counters) is common to both. So each of
its forty switch statements grew a RENDERPATH_METAL arm that forwards here, and
everything above it is untouched. In particular r_stat_draws* are incremented
before the switch, which is what makes METAL.md's "free counters must be
identical across backends" check free.

WHY THIS FILE KEEPS ITS OWN STATE SHADOW. It would be natural to read
gl_backend.c's gl_state and save the duplication, and for the scalar toggles
that would even work -- depth test/write/func/range, the blend FACTORS, colour
mask, scissor test, alpha-to-coverage, the active unit, the constant colour and
polygon offset are all assigned outside their switch, and so is the logical
unit->texture binding. But the vertex layout is not: R_Mesh_VertexPointer,
R_Mesh_ColorPointer and R_Mesh_TexCoordPointer keep their ENTIRE compare-and-
assign block inside the GL arm, so under RENDERPATH_METAL they record nothing
whatsoever. gl_state.blend, cullface/cullfaceenable and framebufferobject are
in the same position. Since the vertex layout is precisely what the pipeline
key is built from, the shadow has to live here. Hoisting those assignments in
shipped GL code was considered and rejected: the fields are GL-shaped, the
hoist would have to preserve GL's own change-detection early-outs exactly, and
it risks Seb's renderer to save a struct.

GL ENUMS ARE THE CURRENCY. quakedef.h pulls in glquake.h, so this file's
implementation can map GL_SRC_ALPHA, GL_LEQUAL, GL_BACK and friends to their
Metal equivalents itself. The alternative -- translating in gl_backend.c --
would have put new code in the shipped GL file for no gain. Callers pass the
same values they pass GL.

HANDLES, NOT POINTERS, exactly as metal_textures.h argues at greater length. A
buffer is a small positive integer living in the r_meshbuffer_t's existing
`bufferobject` field, with slot 0 meaning "none" so GL's `if (bufferobject)`
tests keep their meaning. The spare `devicebuffer` void * left over from D3D9
is deliberately NOT used: it is a raw pointer in a plain-C struct that gets
memset, which under ARC means bridge-retain bookkeeping with no owner.

SCOPE. This slice serves the drawing machinery only -- state, pipeline state,
encoder lifetime, buffers, vertex layout and the draw call. It does NOT carry a
frame: CL_UpdateScreen's early-out and the R_RenderView guard both stay up
until slice 4 gives the backend a real shader, and vid_metal.m's Phase 0 clear
and marker triangle are untouched (its private pipeline cache is superseded by
this one and is deleted in slice 4, when the frame loop moves here and the
deletion is free). Render targets and framebuffer objects are REFUSED WITH A
MESSAGE rather than silently mishandled -- a map-less console/menu/HUD frame
targets the backbuffer exclusively, so Phase 3 needs none, and Phase 4a owes
them real work. The shader is a small built-in passthrough plus a magenta
sentinel, not the ported uber-shader; slice 4 owes that too.
*/

#ifndef METAL_BACKEND_H
#define METAL_BACKEND_H

#include "qtypes.h"

// The Metal objects are built for the macOS CLIENT only (makefile.inc's
// OBJ_METAL_MACOS), but gl_backend.c -- which calls every function below from
// its RENDERPATH_METAL arms -- is in OBJ_COMMON and links into the dedicated
// server and every non-Apple target too. Rather than guard forty call sites,
// the declarations become static-inline no-ops when the feature is absent.
// Keeping the stubs HERE, beside the prototypes, is deliberate: a separate
// stub file would be a hand-kept parallel list of thirty names, which is
// exactly the shape of duplicate this project has been burned by.
// clear bits, so glquake.h's GL_*_BUFFER_BIT need not cross this header.
// OUTSIDE the feature guard: gl_backend.c's GL_Clear arm translates into them
// whether or not the backend is compiled in.
#define MB_CLEAR_COLOR   1
#define MB_CLEAR_DEPTH   2
#define MB_CLEAR_STENCIL 4

#ifdef USE_METAL_RENDERER

// ---------------------------------------------------------------------------
// lifecycle

/// Register the backend's cvars. Called once from gl_backend_init on every
/// path, because a cvar that only exists on one renderer is a config trap.
void Metal_Backend_RegisterCvars(void);

/// Bring the backend up against the device and queue vid_metal.m owns. Safe to
/// call twice; a second call resets rather than leaks.
void Metal_Backend_Start(void);

/// Release every pipeline, depth-stencil state and buffer. Safe when never started.
void Metal_Backend_Shutdown(void);

/// Return every piece of state to the values GL_Backend_ResetState installs, and
/// mark the encoder's whole state dirty so the next draw re-applies all of it.
void Metal_Backend_ResetState(void);

/// End the open encoder, commit, and wait. The GL_Finish equivalent.
void Metal_Backend_Finish(void);

/// Start a frame: make the persistent GL-layout screen texture the render
/// target, creating or resizing it as needed. Called where the Phase 0
/// CL_UpdateScreen door used to be.
void Metal_Backend_BeginFrame(int width, int height);

/// Finish a frame: run the v-flip present pass into `drawable` (an opaque
/// id<CAMetalDrawable>) and present it, WITHOUT waiting. `probeframe` instead
/// reads the drawable back and reports -- the only check that covers the present
/// pass itself, since a broken present leaves a black window while every
/// screen-texture readback still passes.
/// Present one frame. probeframe: 0 normal; 1 the Phase 0/3 flatness probe,
/// which reads the drawable back and does NOT present; 2 METAL.md Phase 7-5's
/// EDR probe, which reports UNCLAMPED statistics for the screen texture and
/// the drawable and DOES present -- macOS stops granting headroom to a layer
/// that has stopped putting frames on screen, so a non-presenting EDR probe
/// would measure the state it destroyed.
void Metal_Backend_EndFrame(void *drawable, int probeframe);

/// METAL_FRAMEMS=2: time ONE region of the frame on the GPU. begin=1 closes the
/// frame's command buffer so far and opens a fresh one; begin=0 closes THAT one
/// with a completion handler that accumulates its GPU span under `label`, so
/// the region's draws are the only work in the timed buffer. Reported with the
/// frame line every 120 frames. A no-op unless the env is 2 -- the split costs
/// a command buffer per region per frame, so it is never on in ordinary play.
/// The pixels are unaffected either way: a render pass split across command
/// buffers keeps its attachments (r_metal_forceencoderrestart's proof).
void Metal_Backend_ProfileRegion(int begin, const char *label);
/// SEPTEMBER2 A2: commit the renderer's command buffer so far (the raster) without waiting, so it overlaps the sidecar's trace on the other queue; the next draw opens a fresh buffer. rt_metal_pipeline.
void Metal_Backend_Kick(void);

// ---------------------------------------------------------------------------
// state. None of these touch the GPU -- they record and mark dirty, and the
// next draw applies the lot in one place (see the note on Metal_ApplyState in
// the implementation: Metal encoders start stateless, and scattered state
// replay is the largest bug source in this design).

void Metal_Backend_SetBlendFunc(int glsrc, int gldst);
void Metal_Backend_SetBlendEquation(int op);			///< dpblendop_t (gl_backend.h)
void Metal_Backend_SetBlendEquationSubtract(qbool negated);
void Metal_Backend_SetDepthMask(qbool enable);
void Metal_Backend_SetDepthTest(qbool enable);
void Metal_Backend_SetDepthFunc(int glfunc);
void Metal_Backend_SetDepthRange(float nearfrac, float farfrac);
void Metal_Backend_SetStencil(qbool enable, int writemask, int glfail, int glzfail, int glzpass, int glcompare, int comparereference, int comparemask);
void Metal_Backend_SetPolygonOffset(float planeoffset, float depthoffset);
/// glface is GL_NONE, GL_FRONT or GL_BACK, already flipped for v_flipped_state
/// by the caller exactly as the GL arm receives it.
void Metal_Backend_SetCullFace(int glface);
void Metal_Backend_SetAlphaToCoverage(qbool enable);
/// mask is gl_state.colormask's packing: bottom four bits r g b a.
void Metal_Backend_SetColorMask(int mask);
/// The CONSTANT vertex colour -- what GL expresses as glVertexAttrib4f on a
/// disabled array, and Metal as a MTLVertexStepFunctionConstant slot.
void Metal_Backend_SetColor(float r, float g, float b, float a);
void Metal_Backend_SetScissor(int x, int y, int width, int height);
void Metal_Backend_SetScissorTest(qbool enable);
void Metal_Backend_SetViewport(int x, int y, int width, int height);

// ---------------------------------------------------------------------------
// render targets and clear

/// fbo 0 selects the screen pair; a nonzero handle selects that FBO's
/// attachments. A CHANGE ends the encoder and swaps the whole target set;
/// no-change is one int compare (R_Mesh_Start/Finish call this constantly).
void Metal_Backend_SetRenderTarget(int fbo);
/// Register an FBO: texture HANDLES (rtexture texnums), 0 for an absent
/// attachment. Returns a positive fbo handle, or 0 on failure.
int  Metal_Backend_CreateFramebufferObject(int depthtexnum, int colortexnum0, int colortexnum1, int colortexnum2, int colortexnum3);
void Metal_Backend_DestroyFramebufferObject(int fbo);
/// glCopyTexSubImage2D's Metal arm: blit a rectangle of the CURRENT colour
/// target into a texture. Both are GL-layout images, so the rows copy straight
/// across with no flip.
void Metal_Backend_CopyToTexture(int texnum, int tx, int ty, int sx, int sy, int width, int height);

/// METAL.md Phase 8-5: encode the MetalFX spatial scaler into the frame's
/// command buffer, upscaling `srchandle` (the render-res postprocess
/// intermediate's texnum) into fbo 0's screen texture. The caller must have
/// run MetalFX_ScalerReady for this frame's exact key first -- this call
/// refuses rather than falling back. The next draw reopens fbo 0 with Load,
/// so the HUD composites over the scaled frame.
/// `dsthandle` 0 means the screen texture; anything else is a pooled
/// NATIVE-resolution target, which is what the post-upscale FXAA pass
/// (r_fxaa_post) needs -- FXAA has to run at the resolution the staircase
/// exists at, and the postprocess runs at render resolution.
qbool Metal_Backend_SpatialUpscaleToScreen(int srchandle, int dsthandle);

/// The MetalFX-TEMPORAL arc: the same shape, with the three extra inputs a
/// temporal scaler needs. `srchandle` / `depthhandle` / `motionhandle` are
/// texnums of this frame's render-res postprocess intermediate, scene depth and
/// motion-vector buffer; `jitterx`/`jittery` are the sub-pixel offset the
/// raster was jittered by; `reset` discards the accumulated history. The
/// caller must have run MetalFX_TemporalReady for this frame's exact key
/// first -- this refuses rather than falling back.
qbool Metal_Backend_TemporalUpscaleToScreen(int srchandle, int depthhandle, int motionhandle, int reactivehandle,
                                            float jitterx, float jittery, qbool reset, int dsthandle);

/// mask is MB_CLEAR_*. With no encoder open this becomes a load action on the
/// next one; with an encoder open, or with the scissor test on, it becomes a
/// scissored quad -- never a load action, or it would wipe what is already
/// drawn. That second case is METAL.md's named HUD-wiping trap.
void Metal_Backend_Clear(int mask, const float *colorvalue, float depthvalue, int stencilvalue);

// ---------------------------------------------------------------------------
// buffers. `handle` is r_meshbuffer_t.bufferobject; pass 0 to have one created
// and returned, mirroring GL's lazy glGenBuffers.

int  Metal_Backend_BufferUpdate(int handle, const void *data, size_t size, qbool subdata, size_t offset, qbool isdynamic, qbool isindexbuffer, const char *name);
void Metal_Backend_BufferDestroy(int handle);

// ---------------------------------------------------------------------------
// vertex layout. One Metal buffer binding per attribute, because DarkPlaces has
// no interleaved vertex struct -- there are ten parallel attribute slots
// (GLSLATTRIB_POSITION 0, COLOR 1, TEXCOORD0..7 2..9), each with its own
// pointer, stride and buffer. A NULL pointer disables the slot, which for
// colour is what selects the constant-colour layout.
//
// `gltype` carries DarkPlaces' unnormalised flag in bit 31, the same convention
// qglVertexAttribPointer's `normalized` argument is derived from.

void Metal_Backend_VertexPointer(int components, int gltype, size_t stride, const void *pointer, int bufferhandle, size_t bufferoffset);
void Metal_Backend_ColorPointer(int components, int gltype, size_t stride, const void *pointer, int bufferhandle, size_t bufferoffset);
void Metal_Backend_TexCoordPointer(unsigned int unitnum, int components, int gltype, size_t stride, const void *pointer, int bufferhandle, size_t bufferoffset);

/// texhandle is the rtexture_t's texnum, i.e. a metal_textures.h handle; 0 unbinds.
void Metal_Backend_TexBind(unsigned int unitnum, int texhandle);

// ---------------------------------------------------------------------------
// draw. Index handles are metal buffer handles resolved by the caller from the
// index r_meshbuffer_t, 16-bit preferred exactly as the GL arm prefers it; both
// zero means a non-indexed draw. Indices are absolute vertex numbers, so there
// is no base-vertex to apply.

void Metal_Backend_Draw(int firstvertex, int numvertices, unsigned int numelements, int index16handle, size_t offset16, int index32handle, size_t offset32);

// ---------------------------------------------------------------------------
// shaders and uniforms
//
// The seam slice 1 built the 167 R_Shader_Uniform* call sites for. Under GL a
// "location" is a GL uniform location; here it is a BYTE OFFSET into a per-draw
// constant struct, obtained by reflecting the compiled pipeline. Negative keeps
// its meaning of "this uniform is not in this permutation", which is what every
// existing `if (loc >= 0)` guard at the call sites tests -- including the five
// that wrap real side effects and must not start firing.

/// Compile (or return from cache) the MSL for one permutation. `strings` is the
/// SAME #define pretext R_GLSL_CompilePermutation builds for the GLSL compiler,
/// so the two languages cannot select different features. Returns a positive
/// program id, or 0 if the compile failed (the caller then behaves exactly as it
/// does for a failed GLSL compile).
int Metal_Backend_CompilePermutation(unsigned int mode, uint64_t permutation, const char **strings, int numstrings);

/// Drop every compiled program AND the pipeline cache -- together, always:
/// PSO keys embed program ids, freed ids get reused, and a stale PSO would
/// silently render the old shader under a new id. Called from
/// R_GLSL_Restart_f's Metal arm, which fires on any static-parm change.
void Metal_Backend_FreePrograms(void);

/// Make a compiled program current for subsequent draws. 0 selects the sentinel.
void Metal_Backend_SetProgram(int program);

/// Byte offset of a constant-struct member, an opaque encoding for a texture
/// binding, or -1 if this permutation does not declare `name`.
int Metal_Backend_GetUniformLocation(int program, const char *name);

/// Write into the current program's staging block. `numfloats` is 1-4, or 16 for
/// a matrix. A location that turns out to name a texture is ignored rather than
/// written -- in MSL the sampler index is baked into the function signature, so
/// GL's "tell the shader which unit this sampler is on" uniform has no meaning
/// and writing it would corrupt the constant buffer.
void Metal_Backend_SetUniformFloats(int loc, const float *values, int numfloats);
void Metal_Backend_SetUniformInt(int loc, int value);

// ---------------------------------------------------------------------------
// verification (r_metal_drawprobe). Nothing renders on this path yet, so a
// readback is the only honest proof: a silently-failed pipeline and a working
// one produce identical logs. Between Begin and End the caller drives the REAL
// public entry points in gl_backend.h, so what is proven is the shipped path
// and not a private replica of it.

/// GL_ReadPixelsBGRA's Metal arm. Reads back from the CURRENT colour target,
/// which holds a GL-layout image (row 0 at the bottom, per the y-negation in
/// R_Viewport_InitOrtho's Metal arm) -- so x/y/width/height mean exactly what
/// they mean to glReadPixels and the rows come out in GL's own order with no
/// flip anywhere. That is the whole payoff of mirroring the projection rather
/// than rendering Metal-native and flipping later. Returns false, having zeroed
/// `outpixels`, if no target is bound.
qbool Metal_Backend_ReadPixels(int x, int y, int width, int height, unsigned char *outpixels);

/// RT_METAL_DUMP's Metal arm (METAL.md Phase 8-1a): read fbo 0's colour -- the
/// backend's screen texture -- BY NAME, regardless of which render target is
/// currently bound. Metal_Backend_ReadPixels follows mb_targetcolor, which is
/// fbo 0 at VID_Finish time today -- but Phase 8-5 points R_BlendView at a
/// pooled intermediate, and an instrument whose pixel source moves with the
/// last SetRenderTargets call is a silent coupling. GL-layout rows (row 0 at
/// the bottom), BGRA8, clamped under an EDR frame. Refuses (returns false)
/// unless width/height equal the screen texture's own, which keeps the
/// caller's buffer sizing honest.
qbool Metal_Backend_ReadScreenBGRA(int width, int height, unsigned char *outpixels);

/// Point the backend at a private scratch colour target and clear the shadow.
/// Returns false with a console error if Metal is not up.
qbool Metal_Backend_ProbeBegin(int width, int height);
/// End the encoder, commit, wait, and blit the scratch target into `outBGRA`,
/// which must hold width*height*4 bytes. Restores the previous target.
qbool Metal_Backend_ProbeEnd(unsigned char *outBGRA);

#else // !USE_METAL_RENDERER

static inline void Metal_Backend_RegisterCvars(void) {}
static inline void Metal_Backend_Start(void) {}
static inline void Metal_Backend_Shutdown(void) {}
static inline void Metal_Backend_ResetState(void) {}
static inline void Metal_Backend_Finish(void) {}
static inline void Metal_Backend_BeginFrame(int width, int height) { (void)width; (void)height; }
static inline void Metal_Backend_EndFrame(void *drawable, int probeframe) { (void)drawable; (void)probeframe; }
static inline void Metal_Backend_ProfileRegion(int begin, const char *label) { (void)begin; (void)label; }
static inline void Metal_Backend_Kick(void) {}
static inline void Metal_Backend_SetBlendFunc(int glsrc, int gldst) { (void)glsrc; (void)gldst; }
static inline void Metal_Backend_SetBlendEquation(int op) { (void)op; }
static inline void Metal_Backend_SetBlendEquationSubtract(qbool negated) { (void)negated; }
static inline void Metal_Backend_SetDepthMask(qbool enable) { (void)enable; }
static inline void Metal_Backend_SetDepthTest(qbool enable) { (void)enable; }
static inline void Metal_Backend_SetDepthFunc(int glfunc) { (void)glfunc; }
static inline void Metal_Backend_SetDepthRange(float nearfrac, float farfrac) { (void)nearfrac; (void)farfrac; }
static inline void Metal_Backend_SetStencil(qbool enable, int writemask, int glfail, int glzfail, int glzpass, int glcompare, int comparereference, int comparemask) { (void)enable; (void)writemask; (void)glfail; (void)glzfail; (void)glzpass; (void)glcompare; (void)comparereference; (void)comparemask; }
static inline void Metal_Backend_SetPolygonOffset(float planeoffset, float depthoffset) { (void)planeoffset; (void)depthoffset; }
static inline void Metal_Backend_SetCullFace(int glface) { (void)glface; }
static inline void Metal_Backend_SetAlphaToCoverage(qbool enable) { (void)enable; }
static inline void Metal_Backend_SetColorMask(int mask) { (void)mask; }
static inline void Metal_Backend_SetColor(float r, float g, float b, float a) { (void)r; (void)g; (void)b; (void)a; }
static inline void Metal_Backend_SetScissor(int x, int y, int width, int height) { (void)x; (void)y; (void)width; (void)height; }
static inline void Metal_Backend_SetScissorTest(qbool enable) { (void)enable; }
static inline void Metal_Backend_SetViewport(int x, int y, int width, int height) { (void)x; (void)y; (void)width; (void)height; }
static inline void Metal_Backend_SetRenderTarget(int fbo) { (void)fbo; }
static inline int  Metal_Backend_CreateFramebufferObject(int depthtexnum, int colortexnum0, int colortexnum1, int colortexnum2, int colortexnum3) { (void)depthtexnum; (void)colortexnum0; (void)colortexnum1; (void)colortexnum2; (void)colortexnum3; return 0; }
static inline void Metal_Backend_DestroyFramebufferObject(int fbo) { (void)fbo; }
static inline void Metal_Backend_CopyToTexture(int texnum, int tx, int ty, int sx, int sy, int width, int height) { (void)texnum; (void)tx; (void)ty; (void)sx; (void)sy; (void)width; (void)height; }
static inline qbool Metal_Backend_SpatialUpscaleToScreen(int srchandle, int dsthandle) { (void)srchandle; (void)dsthandle; return false; }
static inline qbool Metal_Backend_TemporalUpscaleToScreen(int srchandle, int depthhandle, int motionhandle, int reactivehandle, float jitterx, float jittery, qbool reset, int dsthandle) { (void)srchandle; (void)depthhandle; (void)motionhandle; (void)reactivehandle; (void)jitterx; (void)jittery; (void)reset; (void)dsthandle; return false; }
static inline void Metal_Backend_Clear(int mask, const float *colorvalue, float depthvalue, int stencilvalue) { (void)mask; (void)colorvalue; (void)depthvalue; (void)stencilvalue; }
static inline int  Metal_Backend_BufferUpdate(int handle, const void *data, size_t size, qbool subdata, size_t offset, qbool isdynamic, qbool isindexbuffer, const char *name) { (void)data; (void)size; (void)subdata; (void)offset; (void)isdynamic; (void)isindexbuffer; (void)name; return handle; }
static inline void Metal_Backend_BufferDestroy(int handle) { (void)handle; }
static inline void Metal_Backend_VertexPointer(int components, int gltype, size_t stride, const void *pointer, int bufferhandle, size_t bufferoffset) { (void)components; (void)gltype; (void)stride; (void)pointer; (void)bufferhandle; (void)bufferoffset; }
static inline void Metal_Backend_ColorPointer(int components, int gltype, size_t stride, const void *pointer, int bufferhandle, size_t bufferoffset) { (void)components; (void)gltype; (void)stride; (void)pointer; (void)bufferhandle; (void)bufferoffset; }
static inline void Metal_Backend_TexCoordPointer(unsigned int unitnum, int components, int gltype, size_t stride, const void *pointer, int bufferhandle, size_t bufferoffset) { (void)unitnum; (void)components; (void)gltype; (void)stride; (void)pointer; (void)bufferhandle; (void)bufferoffset; }
static inline void Metal_Backend_TexBind(unsigned int unitnum, int texhandle) { (void)unitnum; (void)texhandle; }
static inline void Metal_Backend_Draw(int firstvertex, int numvertices, unsigned int numelements, int index16handle, size_t offset16, int index32handle, size_t offset32) { (void)firstvertex; (void)numvertices; (void)numelements; (void)index16handle; (void)offset16; (void)index32handle; (void)offset32; }
static inline int  Metal_Backend_CompilePermutation(unsigned int mode, uint64_t permutation, const char **strings, int numstrings) { (void)mode; (void)permutation; (void)strings; (void)numstrings; return 0; }
static inline void Metal_Backend_FreePrograms(void) {}
static inline void Metal_Backend_SetProgram(int program) { (void)program; }
static inline int  Metal_Backend_GetUniformLocation(int program, const char *name) { (void)program; (void)name; return -1; }
static inline void Metal_Backend_SetUniformFloats(int loc, const float *values, int numfloats) { (void)loc; (void)values; (void)numfloats; }
static inline void Metal_Backend_SetUniformInt(int loc, int value) { (void)loc; (void)value; }
static inline qbool Metal_Backend_ReadPixels(int x, int y, int width, int height, unsigned char *outpixels) { (void)x; (void)y; (void)width; (void)height; (void)outpixels; return false; }
static inline qbool Metal_Backend_ReadScreenBGRA(int width, int height, unsigned char *outpixels) { (void)width; (void)height; (void)outpixels; return false; }
static inline qbool Metal_Backend_ProbeBegin(int width, int height) { (void)width; (void)height; return false; }
static inline qbool Metal_Backend_ProbeEnd(unsigned char *outBGRA) { (void)outBGRA; return false; }

#endif // USE_METAL_RENDERER

#endif
