/*
QuakeM5 -- the command-digest gate (METAL.md Phase 1).

WHAT IT IS. A debug-only build that hashes the renderer's backend call stream
into a per-frame 64-bit digest. Two binaries that issue an IDENTICAL stream
produce an identical digest sequence, which is a proof of behavioural
equivalence that does not depend on the driver, the GPU, or the screenshot
noise floor.

WHY IT EXISTS. CLAUDE.md records the demo5 byte gate going BLIND twice -- once
when a same-binary two-boot control differed by more bytes than the candidate
change, once when a feature that provably could not fire still "failed" it. Its
own lesson is "when the gate is blind, find a counter". This is that counter,
built once instead of improvised per session. Its first job is METAL.md Phase 3,
where 167 uniform-setting call sites in gl_rmain.c get an indirection so a
single R_SetupShader_Surface can serve both backends; that refactor touches
Seb's shipped GL renderer and must be provably inert.

ZERO COST WHEN OFF. Without -DDP_CMDTRACE every macro below expands to NOTHING,
exactly like CHECKGLERROR (glquake.h). The shipping build therefore needs no
gate of its own -- there is no code to gate.

THREE LEVELS, and only the first is a GATE. The ladder is diagnostic: it was
built to answer "which part of the call stream is unstable?", and the answer
turned out to matter.

    DP_CMDTRACE=1  SHAPE   call identity, order and counts. NOTHING else.
                           MEASURED STABLE across processes and across rebuilds,
                           and it is the level to gate on.
    DP_CMDTRACE=2  VALUES  + integer arguments (through the stable-identity
                           helpers below). Still run-varying -- diagnostic only.
    DP_CMDTRACE=3  FULL    + float bits and pointed-to array contents. Also
                           run-varying; gl_rmain.c feeds r_refdef.scene.time
                           straight into LavaParams, so this level inherits the
                           very timing wobble that blinds the byte gate.

NOTHING UNSTABLE IS EVER HASHED, and "unstable" was wider than expected.
Raw pointers move with ASLR, so they reduce to presence. Less obviously, GL
OBJECT NAMES -- texture numbers, buffer names, FBOs, programs, uniform
locations -- are assigned by the driver and are NOT stable across processes:
measured, two boots of one binary on a frozen scene produced an identical call
count and three completely different hashes. Each therefore reduces to a SERIAL,
the order in which this run first saw it, so the tenth distinct texture is 10 in
every run. Getting this wrong produces a digest that looks authoritative and is
noise, which is the exact failure mode this instrument exists to end.

Output goes to a FILE named by DP_CMDTRACE_OUT, never to stderr: stderr is set
non-blocking at startup (sys_shared.c) and a per-frame writer can hit EAGAIN and
silently lose bytes.
*/

#ifndef DPCMDTRACE_H
#define DPCMDTRACE_H

#ifdef DP_CMDTRACE

#include <stddef.h>

struct r_meshbuffer_s;
struct rtexture_s;

// call ids. Order is irrelevant to correctness (the id is just hashed) but
// keep them stable so two builds of DIFFERENT source revisions still compare.
typedef enum dpcmd_id_e
{
	DPCMD_ID_NONE = 0,
	// uniform layer -- the METAL.md Phase 3 gate
	DPCMD_ID_GetUniformLocation, DPCMD_ID_Uniform1f, DPCMD_ID_Uniform1i,
	DPCMD_ID_Uniform2f, DPCMD_ID_Uniform3f, DPCMD_ID_Uniform4f,
	DPCMD_ID_UniformMatrix3fv, DPCMD_ID_UniformMatrix4fv,
	DPCMD_ID_UniformBlockBinding,
	// backend layer -- gl_backend.h
	DPCMD_ID_BlendFunc, DPCMD_ID_BlendEquationSubtract, DPCMD_ID_DepthMask,
	DPCMD_ID_DepthTest, DPCMD_ID_DepthFunc, DPCMD_ID_DepthRange,
	DPCMD_ID_SetStencil, DPCMD_ID_PolygonOffset, DPCMD_ID_CullFace,
	DPCMD_ID_AlphaToCoverage, DPCMD_ID_ColorMask, DPCMD_ID_Color,
	DPCMD_ID_ActiveTexture, DPCMD_ID_Scissor, DPCMD_ID_ScissorTest,
	DPCMD_ID_Clear, DPCMD_ID_ReadPixelsBGRA,
	DPCMD_ID_CreateFramebufferObject, DPCMD_ID_DestroyFramebufferObject,
	DPCMD_ID_SetRenderTargets, DPCMD_ID_CompileProgram, DPCMD_ID_FreeProgram,
	DPCMD_ID_MeshStart, DPCMD_ID_MeshFinish,
	DPCMD_ID_CreateMeshBuffer, DPCMD_ID_UpdateMeshBuffer, DPCMD_ID_DestroyMeshBuffer,
	DPCMD_ID_PrepVertex3f, DPCMD_ID_PrepGeneric, DPCMD_ID_PrepMesh,
	DPCMD_ID_EntityMatrix, DPCMD_ID_VertexPointer, DPCMD_ID_ColorPointer,
	DPCMD_ID_TexCoordPointer, DPCMD_ID_TexBound, DPCMD_ID_CopyToTexture,
	DPCMD_ID_TexBind, DPCMD_ID_ResetTextureState, DPCMD_ID_ClearBindingsForTexture,
	DPCMD_ID_Draw, DPCMD_ID_SetViewport, DPCMD_ID_GetViewport,
	DPCMD_ID_Finish, DPCMD_ID_ClearScreen,
	// APPENDED, never inserted: the header's own rule is that the order must stay
	// stable so two builds of different revisions still compare.
	DPCMD_ID_BlendEquationEx,
	DPCMD_ID_COUNT
}
dpcmd_id_t;

extern int dpcmd_level;   // 0 off, 1 shape (the gate), 2 +ints, 3 +floats

void DPCMD_Init(void);                    // read env, open the output file
void DPCMD_Shutdown(void);                // close it
void DPCMD_FrameEnd(int framenumber);     // emit one line, reset the accumulator

// accumulate. DPCMD_Call starts a record; the arg helpers follow it.
void DPCMD_Call(int id);
void DPCMD_I(long long v);                     // any integer / enum / bool
void DPCMD_F(float v);                         // level 3 only
void DPCMD_Bytes(const void *p, size_t len);   // level 3 only, pointed-to contents
void DPCMD_Buf(const struct r_meshbuffer_s *b); // -> stable serial for its GL name
void DPCMD_Tex(const struct rtexture_s *t);     // -> stable serial for its texnum
void DPCMD_Loc(long long loc);                 // uniform location -> stable serial (-1 kept)
void DPCMD_GLName(long long name);             // fbo/program/texnum -> stable serial
void DPCMD_Ptr(const void *p);                 // presence only, never the address

#define DPCMD_CALL(id)      DPCMD_Call(DPCMD_ID_##id)
#define DPCMD_ARG_I(v)      DPCMD_I((long long)(v))
#define DPCMD_ARG_F(v)      DPCMD_F((float)(v))
#define DPCMD_ARG_BUF(v)    DPCMD_Buf(v)
#define DPCMD_ARG_TEX(v)    DPCMD_Tex(v)
#define DPCMD_ARG_PTR(v)    DPCMD_Ptr(v)
#define DPCMD_ARG_GL(v)     DPCMD_GLName((long long)(v))
#define DPCMD_ARG_LOC(v)    DPCMD_Loc((long long)(v))
#define DPCMD_ARG_BYTES(p,n) DPCMD_Bytes((p),(n))

#else // !DP_CMDTRACE -- everything vanishes, CHECKGLERROR-style

#define DPCMD_Init()                do {} while (0)
#define DPCMD_Shutdown()            do {} while (0)
#define DPCMD_FrameEnd(f)           do {} while (0)
#define DPCMD_CALL(id)              do {} while (0)
#define DPCMD_ARG_I(v)              do {} while (0)
#define DPCMD_ARG_F(v)              do {} while (0)
#define DPCMD_ARG_BUF(v)            do {} while (0)
#define DPCMD_ARG_TEX(v)            do {} while (0)
#define DPCMD_ARG_PTR(v)            do {} while (0)
#define DPCMD_ARG_BYTES(p,n)        do {} while (0)

#endif // DP_CMDTRACE

#endif // DPCMDTRACE_H
