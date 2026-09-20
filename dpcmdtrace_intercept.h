/*
QuakeM5 -- command-digest interception macros (METAL.md Phase 1).

Include this AFTER glquake.h / gl_backend.h in a translation unit whose backend
calls you want in the digest. Without -DDP_CMDTRACE it defines nothing at all.

HOW IT WORKS, and why it is safe.

Every macro here is FUNCTION-LIKE, so the preprocessor expands it only where the
identifier is immediately followed by "(". Every declaration (glquake.h),
definition (vid_shared.c) and address-of site (the openglfuncs[] table) has ")"
or "}" after the name instead, so none of them can be corrupted -- only genuine
call sites expand. Verified across the whole tree before this was written.

Each macro expands to a COMMA EXPRESSION, not a brace block. That is load
bearing: a great many call sites look like

    if (loc_Foo >= 0) qglUniform3f(loc_Foo, a, b, c);

and a brace block there would break the dangling-else shape and change control
flow. A comma expression is an expression, so the statement stays a statement.

The self-reference inside each replacement is safe by the C standard: a macro is
not re-expanded during its own expansion, so the inner qglUniform3f(...) is the
real function pointer, not infinite recursion.

WHERE TO INCLUDE IT.

  gl_rmain.c   -- the ONLY file in the tree containing qglUniform* or
                  qglGetUniformLocation call sites (all 168 and all 127 of them),
                  so the uniform layer's blast radius is one translation unit.
  callers of gl_backend.h -- gl_rsurf.c, r_shadow.c, cl_screen.c, ...
  NOT gl_backend.c -- that file DEFINES these functions. Including it there
                  would rewrite the definitions into infinite self-calls, and
                  would also double-count the backend's internal self-calls.
                  The digest we want is the API call stream, not the internals.

WHAT IS DELIBERATELY NOT HASHED: raw pointers (ASLR), and floats at level 1.
See dpcmdtrace.h.
*/

#ifndef DPCMDTRACE_INTERCEPT_H
#define DPCMDTRACE_INTERCEPT_H

#include "dpcmdtrace.h"

#ifdef DP_CMDTRACE

// ---------------------------------------------------------------------------
// uniform layer -- the METAL.md Phase 3 gate.
//
// Hashing HERE rather than at the future R_Shader_Uniform* wrappers is what
// makes the gate refactor-proof: Phase 3's indirection still bottoms out in
// these same calls, so the before and after hash identically by construction.

#define qglGetUniformLocation(p, n) \
	(DPCMD_CALL(GetUniformLocation), DPCMD_ARG_GL(p), DPCMD_ARG_BYTES((n), strlen(n)), \
	 qglGetUniformLocation((p), (n)))

#define qglUniform1f(l, a) \
	(DPCMD_CALL(Uniform1f), DPCMD_ARG_LOC(l), DPCMD_ARG_F(a), qglUniform1f((l), (a)))

#define qglUniform1i(l, a) \
	(DPCMD_CALL(Uniform1i), DPCMD_ARG_LOC(l), DPCMD_ARG_I(a), qglUniform1i((l), (a)))

#define qglUniform2f(l, a, b) \
	(DPCMD_CALL(Uniform2f), DPCMD_ARG_LOC(l), DPCMD_ARG_F(a), DPCMD_ARG_F(b), \
	 qglUniform2f((l), (a), (b)))

#define qglUniform3f(l, a, b, c) \
	(DPCMD_CALL(Uniform3f), DPCMD_ARG_LOC(l), DPCMD_ARG_F(a), DPCMD_ARG_F(b), DPCMD_ARG_F(c), \
	 qglUniform3f((l), (a), (b), (c)))

#define qglUniform4f(l, a, b, c, d) \
	(DPCMD_CALL(Uniform4f), DPCMD_ARG_LOC(l), DPCMD_ARG_F(a), DPCMD_ARG_F(b), DPCMD_ARG_F(c), DPCMD_ARG_F(d), \
	 qglUniform4f((l), (a), (b), (c), (d)))

#define qglUniformMatrix3fv(l, n, t, v) \
	(DPCMD_CALL(UniformMatrix3fv), DPCMD_ARG_LOC(l), DPCMD_ARG_I(n), DPCMD_ARG_I(t), \
	 DPCMD_ARG_BYTES((v), sizeof(float) * 9 * (size_t)(n)), \
	 qglUniformMatrix3fv((l), (n), (t), (v)))

#define qglUniformMatrix4fv(l, n, t, v) \
	(DPCMD_CALL(UniformMatrix4fv), DPCMD_ARG_LOC(l), DPCMD_ARG_I(n), DPCMD_ARG_I(t), \
	 DPCMD_ARG_BYTES((v), sizeof(float) * 16 * (size_t)(n)), \
	 qglUniformMatrix4fv((l), (n), (t), (v)))

#define qglUniformBlockBinding(p, i, b) \
	(DPCMD_CALL(UniformBlockBinding), DPCMD_ARG_GL(p), DPCMD_ARG_I(i), DPCMD_ARG_I(b), \
	 qglUniformBlockBinding((p), (i), (b)))

// ---------------------------------------------------------------------------
// backend layer -- gl_backend.h.
//
// This is the API call stream, which is STRICTLY MORE SENSITIVE than the GL
// stream: it sees calls that the backend's own redundant-state filtering would
// swallow, so a refactor that changed how often the engine asks for a state
// change shows up even when the resulting GL is identical.

#define GL_BlendFunc(a, b) \
	(DPCMD_CALL(BlendFunc), DPCMD_ARG_I(a), DPCMD_ARG_I(b), GL_BlendFunc((a), (b)))
#define GL_BlendEquationSubtract(a) \
	(DPCMD_CALL(BlendEquationSubtract), DPCMD_ARG_I(a), GL_BlendEquationSubtract(a))
#define GL_BlendEquationEx(a) \
	(DPCMD_CALL(BlendEquationEx), DPCMD_ARG_I(a), GL_BlendEquationEx(a))
#define GL_DepthMask(a) \
	(DPCMD_CALL(DepthMask), DPCMD_ARG_I(a), GL_DepthMask(a))
#define GL_DepthTest(a) \
	(DPCMD_CALL(DepthTest), DPCMD_ARG_I(a), GL_DepthTest(a))
#define GL_DepthFunc(a) \
	(DPCMD_CALL(DepthFunc), DPCMD_ARG_I(a), GL_DepthFunc(a))
#define GL_DepthRange(a, b) \
	(DPCMD_CALL(DepthRange), DPCMD_ARG_F(a), DPCMD_ARG_F(b), GL_DepthRange((a), (b)))
#define R_SetStencil(e, w, f, zf, zp, c, cr, cm) \
	(DPCMD_CALL(SetStencil), DPCMD_ARG_I(e), DPCMD_ARG_I(w), DPCMD_ARG_I(f), DPCMD_ARG_I(zf), \
	 DPCMD_ARG_I(zp), DPCMD_ARG_I(c), DPCMD_ARG_I(cr), DPCMD_ARG_I(cm), \
	 R_SetStencil((e), (w), (f), (zf), (zp), (c), (cr), (cm)))
#define GL_PolygonOffset(a, b) \
	(DPCMD_CALL(PolygonOffset), DPCMD_ARG_F(a), DPCMD_ARG_F(b), GL_PolygonOffset((a), (b)))
#define GL_CullFace(a) \
	(DPCMD_CALL(CullFace), DPCMD_ARG_I(a), GL_CullFace(a))
#define GL_AlphaToCoverage(a) \
	(DPCMD_CALL(AlphaToCoverage), DPCMD_ARG_I(a), GL_AlphaToCoverage(a))
#define GL_ColorMask(r, g, b, a) \
	(DPCMD_CALL(ColorMask), DPCMD_ARG_I(r), DPCMD_ARG_I(g), DPCMD_ARG_I(b), DPCMD_ARG_I(a), \
	 GL_ColorMask((r), (g), (b), (a)))
#define GL_Color(r, g, b, a) \
	(DPCMD_CALL(Color), DPCMD_ARG_F(r), DPCMD_ARG_F(g), DPCMD_ARG_F(b), DPCMD_ARG_F(a), \
	 GL_Color((r), (g), (b), (a)))
#define GL_ActiveTexture(n) \
	(DPCMD_CALL(ActiveTexture), DPCMD_ARG_I(n), GL_ActiveTexture(n))
#define GL_Scissor(x, y, w, h) \
	(DPCMD_CALL(Scissor), DPCMD_ARG_I(x), DPCMD_ARG_I(y), DPCMD_ARG_I(w), DPCMD_ARG_I(h), \
	 GL_Scissor((x), (y), (w), (h)))
#define GL_ScissorTest(a) \
	(DPCMD_CALL(ScissorTest), DPCMD_ARG_I(a), GL_ScissorTest(a))
#define GL_Clear(m, c, d, s) \
	(DPCMD_CALL(Clear), DPCMD_ARG_I(m), DPCMD_ARG_BYTES((c), sizeof(float) * 4), \
	 DPCMD_ARG_F(d), DPCMD_ARG_I(s), GL_Clear((m), (c), (d), (s)))

#define R_Mesh_SetRenderTargets(f) \
	(DPCMD_CALL(SetRenderTargets), DPCMD_ARG_GL(f), R_Mesh_SetRenderTargets(f))
#define R_Mesh_DestroyFramebufferObject(f) \
	(DPCMD_CALL(DestroyFramebufferObject), DPCMD_ARG_GL(f), R_Mesh_DestroyFramebufferObject(f))
#define R_Mesh_CreateFramebufferObject(d, c1, c2, c3, c4) \
	(DPCMD_CALL(CreateFramebufferObject), DPCMD_ARG_TEX(d), DPCMD_ARG_TEX(c1), DPCMD_ARG_TEX(c2), \
	 DPCMD_ARG_TEX(c3), DPCMD_ARG_TEX(c4), \
	 R_Mesh_CreateFramebufferObject((d), (c1), (c2), (c3), (c4)))

#define GL_Backend_FreeProgram(p) \
	(DPCMD_CALL(FreeProgram), DPCMD_ARG_GL(p), GL_Backend_FreeProgram(p))

#define R_Mesh_Start() \
	(DPCMD_CALL(MeshStart), R_Mesh_Start())
#define R_Mesh_Finish() \
	(DPCMD_CALL(MeshFinish), R_Mesh_Finish())
#define R_Mesh_ResetTextureState() \
	(DPCMD_CALL(ResetTextureState), R_Mesh_ResetTextureState())
#define R_Mesh_ClearBindingsForTexture(t) \
	(DPCMD_CALL(ClearBindingsForTexture), DPCMD_ARG_GL(t), R_Mesh_ClearBindingsForTexture(t))

#define R_Mesh_UpdateMeshBuffer(b, d, s, sub, off) \
	(DPCMD_CALL(UpdateMeshBuffer), DPCMD_ARG_BUF(b), DPCMD_ARG_I(s), DPCMD_ARG_I(sub), DPCMD_ARG_I(off), \
	 R_Mesh_UpdateMeshBuffer((b), (d), (s), (sub), (off)))
#define R_Mesh_DestroyMeshBuffer(b) \
	(DPCMD_CALL(DestroyMeshBuffer), DPCMD_ARG_BUF(b), R_Mesh_DestroyMeshBuffer(b))

#define R_Mesh_PrepareVertices_Vertex3f(n, v, b, off) \
	(DPCMD_CALL(PrepVertex3f), DPCMD_ARG_I(n), DPCMD_ARG_BUF(b), DPCMD_ARG_I(off), \
	 DPCMD_ARG_BYTES((v), sizeof(float) * 3 * (size_t)(n)), \
	 R_Mesh_PrepareVertices_Vertex3f((n), (v), (b), (off)))
#define R_Mesh_PrepareVertices_Generic_Arrays(n, v, c, t) \
	(DPCMD_CALL(PrepGeneric), DPCMD_ARG_I(n), DPCMD_ARG_PTR(v), DPCMD_ARG_PTR(c), DPCMD_ARG_PTR(t), \
	 R_Mesh_PrepareVertices_Generic_Arrays((n), (v), (c), (t)))

// NOT intercepted, deliberately: R_EntityMatrix (defined in gl_rmain.c) and
// R_ClearScreen (declared AND defined in cl_screen.c). Unlike the qgl* function
// POINTERS -- whose declarations read "(*qglFoo)(args)", so the name is followed
// by ")" and no macro can fire -- an ordinary function declaration
// "void R_ClearScreen(qbool fogcolor);" DOES have "(" after the name, so the
// macro would rewrite the declaration and the definition into nonsense. It does
// exactly that if you try; that is how this comment came to be written.
// Nothing is lost: both are covered transitively. R_EntityMatrix's body issues
// the two qglUniformMatrix4fv calls the uniform layer already traces, and
// R_ClearScreen's body calls GL_Clear, which is traced.
// The same care is needed before adding any macro here for a function whose
// definition or a local forward declaration lives in an intercepted file.

#define R_Mesh_VertexPointer(c, g, s, p, b, off) \
	(DPCMD_CALL(VertexPointer), DPCMD_ARG_I(c), DPCMD_ARG_I(g), DPCMD_ARG_I(s), \
	 DPCMD_ARG_BUF(b), DPCMD_ARG_I(off), DPCMD_ARG_PTR(p), \
	 R_Mesh_VertexPointer((c), (g), (s), (p), (b), (off)))
#define R_Mesh_ColorPointer(c, g, s, p, b, off) \
	(DPCMD_CALL(ColorPointer), DPCMD_ARG_I(c), DPCMD_ARG_I(g), DPCMD_ARG_I(s), \
	 DPCMD_ARG_BUF(b), DPCMD_ARG_I(off), DPCMD_ARG_PTR(p), \
	 R_Mesh_ColorPointer((c), (g), (s), (p), (b), (off)))
#define R_Mesh_TexCoordPointer(u, c, g, s, p, b, off) \
	(DPCMD_CALL(TexCoordPointer), DPCMD_ARG_I(u), DPCMD_ARG_I(c), DPCMD_ARG_I(g), DPCMD_ARG_I(s), \
	 DPCMD_ARG_BUF(b), DPCMD_ARG_I(off), DPCMD_ARG_PTR(p), \
	 R_Mesh_TexCoordPointer((u), (c), (g), (s), (p), (b), (off)))

#define R_Mesh_TexBind(u, t) \
	(DPCMD_CALL(TexBind), DPCMD_ARG_I(u), DPCMD_ARG_TEX(t), R_Mesh_TexBind((u), (t)))
#define R_Mesh_CopyToTexture(t, tx, ty, sx, sy, w, h) \
	(DPCMD_CALL(CopyToTexture), DPCMD_ARG_TEX(t), DPCMD_ARG_I(tx), DPCMD_ARG_I(ty), \
	 DPCMD_ARG_I(sx), DPCMD_ARG_I(sy), DPCMD_ARG_I(w), DPCMD_ARG_I(h), \
	 R_Mesh_CopyToTexture((t), (tx), (ty), (sx), (sy), (w), (h)))

#define R_Mesh_Draw(fv, nv, ft, nt, e3i, b3i, o3i, e3s, b3s, o3s) \
	(DPCMD_CALL(Draw), DPCMD_ARG_I(fv), DPCMD_ARG_I(nv), DPCMD_ARG_I(ft), DPCMD_ARG_I(nt), \
	 DPCMD_ARG_BUF(b3i), DPCMD_ARG_I(o3i), DPCMD_ARG_BUF(b3s), DPCMD_ARG_I(o3s), \
	 DPCMD_ARG_PTR(e3i), DPCMD_ARG_PTR(e3s), \
	 R_Mesh_Draw((fv), (nv), (ft), (nt), (e3i), (b3i), (o3i), (e3s), (b3s), (o3s)))

#define R_SetViewport(v) \
	(DPCMD_CALL(SetViewport), DPCMD_ARG_BYTES((v), sizeof(r_viewport_t)), R_SetViewport(v))
#define GL_Finish() \
	(DPCMD_CALL(Finish), GL_Finish())

#endif // DP_CMDTRACE

#endif // DPCMDTRACE_INTERCEPT_H
