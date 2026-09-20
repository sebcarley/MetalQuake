
#ifndef CL_SCREEN_H
#define CL_SCREEN_H

#include "qtypes.h"

void SHOWLMP_decodehide(void);
void SHOWLMP_decodeshow(void);
void SHOWLMP_drawall(void);

extern struct cvar_s vid_conwidth;
extern struct cvar_s vid_conheight;
extern struct cvar_s vid_pixelheight;
extern struct cvar_s scr_screenshot_jpeg;
extern struct cvar_s scr_screenshot_jpeg_quality;
extern struct cvar_s scr_screenshot_png;
extern struct cvar_s scr_screenshot_gammaboost;
extern struct cvar_s scr_screenshot_name;

extern char cl_connect_status[MAX_QPATH];

void CL_Screen_NewMap(void);
void CL_Screen_Init(void);
void CL_Screen_Shutdown(void);
void CL_UpdateScreen(void);

qbool R_Stereo_Active(void);
qbool R_Stereo_ColorMasking(void);

#ifdef MACOSX
// Composite the Metal RT shadow term into the scene. Called from R_RenderScene
// (gl_rmain.c) right after the opaque pass, while the scene depth is still bound.
// Takes the render target the scene is being drawn into, because on the Metal
// renderpath the composite is an ordinary backend draw issued from here and
// R_ResetViewRendering2D/3D need exactly this set (METAL.md Phase 5 slice 3);
// the GL path composites through its own bridge and ignores them.
// (struct rtexture_s rather than rtexture_t, matching the struct cvar_s
// references above: this header includes only qtypes.h and is included by
// translation units that never see r_textures.h.)
void RT_SceneComposite(int viewfbo, struct rtexture_s *viewdepthtexture, struct rtexture_s *viewcolortexture,
                       int viewx, int viewy, int viewwidth, int viewheight, qbool depthsampleable);
// Light the view weapon from the RT light list (rt_metal_viewmodel). Called from
// CL_UpdateEntityShading_Entity, which is the one place per-entity model light is
// written. Returns false and writes nothing when the feature or wall lighting is
// off, so the caller keeps the stock path. See the definition in cl_screen.c for
// why the weapon needs its own term at all.
qbool RT_ViewmodelLight(float *out_ambient, float *out_diffuse, float *out_dir);
/// SEPTEMBER2 C1: the ray tracer's light sum at a world point on the CPU (the weapon light's shape); nshadow tracelines; false when the RT term is not the scene's lighting.
qbool RT_LightSumAt(const vec3_t p, int nshadow, float out[3]);
#endif

#endif

