/*
Copyright (C) 1996-1997 Id Software, Inc.

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
// r_main.c

#include "quakedef.h"
#include "r_shadow.h"
#include "polygon.h"
#include "image.h"
#include "shader_density.h"   // METAL.md Phase 2: murk constants shared with the Metal fog kernel
#include "dpcmdtrace_intercept.h"   // METAL.md Phase 1: no-op without -DDP_CMDTRACE
#include "ft2.h"
#include "csprogs.h"
#include "cl_video.h"
#include "cl_collision.h"
#include "metal_backend.h"
extern cvar_t rt_metal_pipeline;   // SEPTEMBER2 A2 (vid_sdl.c): the mid-frame commit points
// METAL.md Phase 8-4: no call sites here yet -- the R_RenderView gate arrives
// at 8-5. Included NOW so the header's static-inline stub arm compiles under
// `make sv-release` from day one; that arm is otherwise compiled by nobody
// until 8-5, which is exactly the class that has broken the server link twice.
#include "metal_fx.h"

#ifdef WIN32
// Enable NVIDIA High Performance Graphics while using Integrated Graphics.
#ifdef __cplusplus
extern "C" {
#endif
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
#ifdef __cplusplus
}
#endif
#endif

mempool_t *r_main_mempool;
rtexturepool_t *r_main_texturepool;

int r_textureframe = 0; ///< used only by R_GetCurrentTexture, incremented per view and per UI render

static qbool r_loadnormalmap;
static qbool r_loadgloss;
qbool r_loadfog;
static qbool r_loaddds;
static qbool r_savedds;
static qbool r_gpuskeletal;

//
// screen size info
//
r_refdef_t r_refdef;

cvar_t r_motionblur = {CF_CLIENT | CF_ARCHIVE, "r_motionblur", "0", "screen motionblur - value represents intensity, somewhere around 0.5 recommended - NOTE: bad performance on multi-gpu!"};
cvar_t r_damageblur = {CF_CLIENT | CF_ARCHIVE, "r_damageblur", "0", "screen motionblur based on damage - value represents intensity, somewhere around 0.5 recommended - NOTE: bad performance on multi-gpu!"};
cvar_t r_motionblur_averaging = {CF_CLIENT | CF_ARCHIVE, "r_motionblur_averaging", "0.1", "sliding average reaction time for velocity (higher = slower adaption to change)"};
cvar_t r_motionblur_randomize = {CF_CLIENT | CF_ARCHIVE, "r_motionblur_randomize", "0.1", "randomizing coefficient to workaround ghosting"};
cvar_t r_motionblur_minblur = {CF_CLIENT | CF_ARCHIVE, "r_motionblur_minblur", "0.5", "factor of blur to apply at all times (always have this amount of blur no matter what the other factors are)"};
cvar_t r_motionblur_maxblur = {CF_CLIENT | CF_ARCHIVE, "r_motionblur_maxblur", "0.9", "maxmimum amount of blur"};
cvar_t r_motionblur_velocityfactor = {CF_CLIENT | CF_ARCHIVE, "r_motionblur_velocityfactor", "1", "factoring in of player velocity to the blur equation - the faster the player moves around the map, the more blur they get"};
cvar_t r_motionblur_velocityfactor_minspeed = {CF_CLIENT | CF_ARCHIVE, "r_motionblur_velocityfactor_minspeed", "400", "lower value of velocity when it starts to factor into blur equation"};
cvar_t r_motionblur_velocityfactor_maxspeed = {CF_CLIENT | CF_ARCHIVE, "r_motionblur_velocityfactor_maxspeed", "800", "upper value of velocity when it reaches the peak factor into blur equation"};
cvar_t r_motionblur_mousefactor = {CF_CLIENT | CF_ARCHIVE, "r_motionblur_mousefactor", "2", "factoring in of mouse acceleration to the blur equation - the faster the player turns their mouse, the more blur they get"};
cvar_t r_motionblur_mousefactor_minspeed = {CF_CLIENT | CF_ARCHIVE, "r_motionblur_mousefactor_minspeed", "0", "lower value of mouse acceleration when it starts to factor into blur equation"};
cvar_t r_motionblur_mousefactor_maxspeed = {CF_CLIENT | CF_ARCHIVE, "r_motionblur_mousefactor_maxspeed", "50", "upper value of mouse acceleration when it reaches the peak factor into blur equation"};

cvar_t r_depthfirst = {CF_CLIENT | CF_ARCHIVE, "r_depthfirst", "0", "renders a depth-only version of the scene before normal rendering begins to eliminate overdraw, values: 0 = off, 1 = world depth, 2 = world and model depth"};
cvar_t r_useinfinitefarclip = {CF_CLIENT | CF_ARCHIVE, "r_useinfinitefarclip", "1", "enables use of a special kind of projection matrix that has an extremely large farclip"};
cvar_t r_farclip_base = {CF_CLIENT, "r_farclip_base", "65536", "farclip (furthest visible distance) for rendering when r_useinfinitefarclip is 0"};
cvar_t r_farclip_world = {CF_CLIENT, "r_farclip_world", "2", "adds map size to farclip multiplied by this value"};
cvar_t r_nearclip = {CF_CLIENT, "r_nearclip", "1", "distance from camera of nearclip plane" };
cvar_t r_deformvertexes = {CF_CLIENT, "r_deformvertexes", "1", "allows use of deformvertexes in shader files (can be turned off to check performance impact)"};
cvar_t r_transparent = {CF_CLIENT, "r_transparent", "1", "allows use of transparent surfaces (can be turned off to check performance impact)"};
cvar_t r_transparent_alphatocoverage = {CF_CLIENT, "r_transparent_alphatocoverage", "1", "enables GL_ALPHA_TO_COVERAGE antialiasing technique on alphablend and alphatest surfaces when using vid_samples 2 or higher"};
cvar_t r_transparent_sortsurfacesbynearest = {CF_CLIENT, "r_transparent_sortsurfacesbynearest", "1", "sort entity and world surfaces by nearest point on bounding box instead of using the center of the bounding box, usually reduces sorting artifacts"};
cvar_t r_transparent_useplanardistance = {CF_CLIENT, "r_transparent_useplanardistance", "0", "sort transparent meshes by distance from view plane rather than spherical distance to the chosen point"};
cvar_t r_showoverdraw = {CF_CLIENT, "r_showoverdraw", "0", "shows overlapping geometry"};
cvar_t r_showbboxes = {CF_CLIENT, "r_showbboxes", "0", "shows bounding boxes of server entities, value controls opacity scaling (1 = 10%,  10 = 100%)"};
cvar_t r_showbboxes_client = {CF_CLIENT, "r_showbboxes_client", "0", "shows bounding boxes of clientside qc entities, value controls opacity scaling (1 = 10%,  10 = 100%)"};
cvar_t r_showsurfaces = {CF_CLIENT, "r_showsurfaces", "0", "1 shows surfaces as different colors, or a value of 3 shows an approximation to vertex or object color (for a very approximate view of the game)"};
cvar_t r_showtris = {CF_CLIENT, "r_showtris", "0", "shows triangle outlines, value controls brightness (can be above 1)"};
cvar_t r_shownormals = {CF_CLIENT, "r_shownormals", "0", "shows per-vertex surface normals and tangent vectors for bumpmapped lighting"};
cvar_t r_showlighting = {CF_CLIENT, "r_showlighting", "0", "shows areas lit by lights, useful for finding out why some areas of a map render slowly (bright orange = lots of passes = slow), a value of 2 disables depth testing which can be interesting but not very useful"};
cvar_t r_showcollisionbrushes = {CF_CLIENT, "r_showcollisionbrushes", "0", "draws collision brushes in quake3 maps (mode 1), mode 2 disables rendering of world (trippy!)"};
cvar_t r_showcollisionbrushes_polygonfactor = {CF_CLIENT, "r_showcollisionbrushes_polygonfactor", "-1", "expands outward the brush polygons a little bit, used to make collision brushes appear infront of walls"};
cvar_t r_showcollisionbrushes_polygonoffset = {CF_CLIENT, "r_showcollisionbrushes_polygonoffset", "0", "nudges brush polygon depth in hardware depth units, used to make collision brushes appear infront of walls"};
cvar_t r_showdisabledepthtest = {CF_CLIENT, "r_showdisabledepthtest", "0", "disables depth testing on r_show* cvars, allowing you to see what hidden geometry the graphics card is processing"};
cvar_t r_showspriteedges = {CF_CLIENT, "r_showspriteedges", "0", "renders a debug outline to show the polygon shape of each sprite frame rendered (may be 2 or more in case of interpolated animations), for debugging rendering bugs with specific view types"};
cvar_t r_showparticleedges = {CF_CLIENT, "r_showparticleedges", "0", "renders a debug outline to show the polygon shape of each particle, for debugging rendering bugs with specific view types"};
cvar_t r_drawportals = {CF_CLIENT, "r_drawportals", "0", "shows portals (separating polygons) in world interior in quake1 maps"};
cvar_t r_drawentities = {CF_CLIENT, "r_drawentities","1", "draw entities (doors, players, projectiles, etc)"};
cvar_t r_draw2d = {CF_CLIENT, "r_draw2d","1", "draw 2D stuff (dangerous to turn off)"};
cvar_t r_drawworld = {CF_CLIENT, "r_drawworld","1", "draw world (most static stuff)"};
cvar_t r_drawviewmodel = {CF_CLIENT, "r_drawviewmodel","1", "draw your weapon model"};
cvar_t r_drawexteriormodel = {CF_CLIENT, "r_drawexteriormodel","1", "draw your player model (e.g. in chase cam, reflections)"};
cvar_t r_cullentities_trace = {CF_CLIENT, "r_cullentities_trace", "1", "probabistically cull invisible entities"};
cvar_t r_cullentities_trace_entityocclusion = {CF_CLIENT, "r_cullentities_trace_entityocclusion", "1", "check for occluding entities such as doors, not just world hull"};
cvar_t r_cullentities_trace_samples = {CF_CLIENT, "r_cullentities_trace_samples", "2", "number of samples to test for entity culling (in addition to center sample)"};
cvar_t r_cullentities_trace_tempentitysamples = {CF_CLIENT, "r_cullentities_trace_tempentitysamples", "-1", "number of samples to test for entity culling of temp entities (including all CSQC entities), -1 disables trace culling on these entities to prevent flicker (pvs still applies)"};
cvar_t r_cullentities_trace_enlarge = {CF_CLIENT, "r_cullentities_trace_enlarge", "0", "box enlargement for entity culling"};
cvar_t r_cullentities_trace_expand = {CF_CLIENT, "r_cullentities_trace_expand", "0", "box expanded by this many units for entity culling"};
cvar_t r_cullentities_trace_pad = {CF_CLIENT, "r_cullentities_trace_pad", "8", "accept traces that hit within this many units of the box"};
cvar_t r_cullentities_trace_delay = {CF_CLIENT, "r_cullentities_trace_delay", "1", "number of seconds until the entity gets actually culled"};
cvar_t r_cullentities_trace_eyejitter = {CF_CLIENT, "r_cullentities_trace_eyejitter", "16", "randomly offset rays from the eye by this much to reduce the odds of flickering"};
cvar_t r_sortentities = {CF_CLIENT, "r_sortentities", "0", "sort entities before drawing (might be faster)"};
cvar_t r_speeds = {CF_CLIENT, "r_speeds","0", "displays rendering statistics and per-subsystem timings"};
cvar_t r_fullbright = {CF_CLIENT, "r_fullbright","0", "makes map very bright and renders faster"};

cvar_t r_fullbright_directed = {CF_CLIENT, "r_fullbright_directed", "0", "render fullbright things (unlit worldmodel and EF_FULLBRIGHT entities, but not fullbright shaders) using a constant light direction instead to add more depth while keeping uniform brightness"};
cvar_t r_fullbright_directed_ambient = {CF_CLIENT, "r_fullbright_directed_ambient", "0.5", "ambient light multiplier for directed fullbright"};
cvar_t r_fullbright_directed_diffuse = {CF_CLIENT, "r_fullbright_directed_diffuse", "0.75", "diffuse light multiplier for directed fullbright"};
cvar_t r_fullbright_directed_pitch = {CF_CLIENT, "r_fullbright_directed_pitch", "20", "constant pitch direction ('height') of the fake light source to use for fullbright"};
cvar_t r_fullbright_directed_pitch_relative = {CF_CLIENT, "r_fullbright_directed_pitch_relative", "0", "whether r_fullbright_directed_pitch is interpreted as absolute (0) or relative (1) pitch"};

cvar_t r_wateralpha = {CF_CLIENT | CF_ARCHIVE, "r_wateralpha","1", "opacity of water polygons"};
cvar_t r_wateralpha_force = {CF_CLIENT | CF_ARCHIVE, "r_wateralpha_force", "0", "honour r_wateralpha even on maps whose vis data was not built for transparent water (all stock id1 maps). Vanilla vis may cull underwater geometry seen from above the surface, so distant pool interiors can be missing -- with the volumetric liquid murk on, the murk hides that long before it shows"};
cvar_t r_dynamic = {CF_CLIENT | CF_ARCHIVE, "r_dynamic","1", "enables dynamic lights (rocket glow and such)"};
cvar_t r_fullbrights = {CF_CLIENT | CF_ARCHIVE, "r_fullbrights", "1", "enables glowing pixels in quake textures (changes need r_restart to take effect)"};
cvar_t r_shadows = {CF_CLIENT | CF_ARCHIVE, "r_shadows", "0", "casts fake stencil shadows from models onto the world (rtlights are unaffected by this); when set to 2, always cast the shadows in the direction set by r_shadows_throwdirection, otherwise use the model lighting."};
cvar_t r_shadows_darken = {CF_CLIENT | CF_ARCHIVE, "r_shadows_darken", "0.5", "how much shadowed areas will be darkened"};
cvar_t r_shadows_throwdistance = {CF_CLIENT | CF_ARCHIVE, "r_shadows_throwdistance", "500", "how far to cast shadows from models"};
cvar_t r_shadows_throwdirection = {CF_CLIENT | CF_ARCHIVE, "r_shadows_throwdirection", "0 0 -1", "override throwing direction for r_shadows 2"};
cvar_t r_shadows_drawafterrtlighting = {CF_CLIENT | CF_ARCHIVE, "r_shadows_drawafterrtlighting", "0", "draw fake shadows AFTER realtime lightning is drawn. May be useful for simulating fast sunlight on large outdoor maps with only one noshadow rtlight. The price is less realistic appearance of dynamic light shadows."};
cvar_t r_shadows_castfrombmodels = {CF_CLIENT | CF_ARCHIVE, "r_shadows_castfrombmodels", "0", "do cast shadows from bmodels"};
cvar_t r_shadows_focus = {CF_CLIENT | CF_ARCHIVE, "r_shadows_focus", "0 0 0", "offset the shadowed area focus"};
cvar_t r_shadows_shadowmapscale = {CF_CLIENT | CF_ARCHIVE, "r_shadows_shadowmapscale", "0.25", "higher values increase shadowmap quality at a cost of area covered (multiply global shadowmap precision) for fake shadows. Needs shadowmapping ON."};
cvar_t r_shadows_shadowmapbias = {CF_CLIENT | CF_ARCHIVE, "r_shadows_shadowmapbias", "-1", "sets shadowmap bias for fake shadows. -1 sets the value of r_shadow_shadowmapping_bias. Needs shadowmapping ON."};
cvar_t r_q1bsp_skymasking = {CF_CLIENT, "r_q1bsp_skymasking", "1", "allows sky polygons in quake1 maps to obscure other geometry"};
cvar_t r_polygonoffset_submodel_factor = {CF_CLIENT, "r_polygonoffset_submodel_factor", "0", "biases depth values of world submodels such as doors, to prevent z-fighting artifacts in Quake maps"};
cvar_t r_polygonoffset_submodel_offset = {CF_CLIENT, "r_polygonoffset_submodel_offset", "14", "biases depth values of world submodels such as doors, to prevent z-fighting artifacts in Quake maps"};
cvar_t r_polygonoffset_decals_factor = {CF_CLIENT, "r_polygonoffset_decals_factor", "0", "biases depth values of decals to prevent z-fighting artifacts"};
cvar_t r_polygonoffset_decals_offset = {CF_CLIENT, "r_polygonoffset_decals_offset", "-14", "biases depth values of decals to prevent z-fighting artifacts"};
cvar_t r_fog_exp2 = {CF_CLIENT, "r_fog_exp2", "0", "uses GL_EXP2 fog (as in Nehahra) rather than realistic GL_EXP fog"};
cvar_t r_fog_clear = {CF_CLIENT, "r_fog_clear", "1", "clears renderbuffer with fog color before render starts"};
cvar_t r_drawfog = {CF_CLIENT | CF_ARCHIVE, "r_drawfog", "1", "allows one to disable fog rendering"};
#ifdef USE_RT_METAL
#include "rt_metal.h"
extern cvar_t rt_metal;             // registered in vid_sdl.c
extern cvar_t rt_metal_term_upsample;   // WARCHEST session 1: the term upsample is a depth-as-texture asker
extern cvar_t rt_metal_term_upsample_depth;
extern cvar_t rt_metal_reproject;   // remap stale RT outputs through the shown camera
extern cvar_t rt_metal_shafts;
extern cvar_t rt_metal_shafts_intensity;
extern cvar_t rt_metal_liquids;
extern cvar_t rt_metal_liquids_minlight;
extern cvar_t rt_metal_liquids_own;          // SEPTEMBER2 C1: the liquid surface's OWN light term, per batch
extern cvar_t rt_metal_liquids_own_shadows;
extern cvar_t rt_metal_liquids_rt;           // SEPTEMBER2 C2: the per-pixel liquid pair (own term + reflection)
extern cvar_t rt_metal_fog;
extern cvar_t rt_metal_fog_intensity;
extern cvar_t rt_metal_fog_upsample;        // depth-aware magnification of the fog kernel's buffer
extern cvar_t rt_metal_fog_upsample_depth;
extern cvar_t rt_metal_walllight;
extern cvar_t rt_metal_viewmodel;   // the view weapon's own RT-matched model light
extern cvar_t rt_metal_glowpass;    // emission survives wall lighting (see R_RTGlow_Pass)
#ifndef GL_TEXTURE_RECTANGLE
#define GL_TEXTURE_RECTANGLE 0x84F5   // not in glquake.h; the shaft buffer is IOSurface-backed and rectangle-only
#endif
#endif
// EMISSION SURVIVES WALL LIGHTING (rt_metal_glowpass): true while R_RTGlow_Pass
// re-draws emissive batches additively after the RT composite. Declared
// unconditionally so the surface-path readers need no guards; nothing ever
// sets them without USE_RT_METAL.
static qbool r_rtglowpass_active = false;
static qbool r_rtglowpass_redglow = false;   // r_redglow live for the entity being re-drawn
cvar_t r_volumetric = {CF_CLIENT | CF_ARCHIVE, "r_volumetric", "0", "master switch for volumetric fog; makes the camera's scene depth available as a sampleable texture, which forces the offscreen scene render path"};
cvar_t m5_packfog = {CF_CLIENT | CF_ARCHIVE, "m5_packfog", "1", "when a map sets fog in its worldspawn and the volumetric murk is running, let the murk take the map's fog COLOUR and stand the classic fog pass down, instead of drawing both. Density stays yours. Of the shipped games only Dimension of the Machine sets map fog, on 7 of its 25 maps; no stock Quake, Armagon, Dissolution or Dimension of the Past map does"};
cvar_t r_volumetric_density = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_density", "0.18", "murk thickness in OPEN air. Lower than it looks: r_volumetric_corner multiplies it up by several times in corners and crevices, which is where the fog is meant to gather. 0 disables the air murk and leaves the liquids"};
cvar_t r_volumetric_height = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_height", "180", "height in world units over which the murk thins out above the fog bed"};
cvar_t r_volumetric_heightbase = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_heightbase", "-48", "height of the fog bed relative to the camera, in world units; negative puts it below eye level, near the floor"};
cvar_t r_volumetric_noisescale = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_noisescale", "0.0016", "world units to noise coordinates; the pattern repeats every 1/this units"};
cvar_t r_volumetric_noisethresh = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_noisethresh", "0.30", "noise below this is empty air, which breaks the murk into banks instead of a uniform haze"};
cvar_t r_volumetric_wind = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_wind", "10 4 1.5", "wind velocity in world units per second, scrolling the noise volume. MUST BE QUOTED: r_volumetric_wind \"10 4 1.5\""};
cvar_t r_volumetric_color = {CF_CLIENT, "r_volumetric_color", "0.52 0.60 0.74", "convenience setter for r_volumetric_color_red/_green/_blue -- the VALUE MUST BE QUOTED, e.g. r_volumetric_color \"0.9 0 0\", because the console only ever takes the first token of an unquoted assignment. The three channel cvars are the real ones and need no quoting"};
cvar_t r_volumetric_color_red = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_color_red", "0.52", "red channel of the air murk colour"};
cvar_t r_volumetric_color_green = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_color_green", "0.60", "green channel of the air murk colour"};
cvar_t r_volumetric_color_blue = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_color_blue", "0.74", "blue channel of the air murk colour. This is also what a fully obscured distance fades TO, so lower all three for gloom rather than glare"};
cvar_t r_volumetric_steps = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_steps", "24", "raymarch samples per pixel; the dominant cost"};
cvar_t r_volumetric_dist = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_dist", "3000", "how far along the view ray the murk is integrated, in world units"};
cvar_t r_volumetric_scale = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_scale", "0.5", "resolution of the fog buffer relative to the screen; 0.5 is quarter the pixels"};
cvar_t r_volumetric_floor = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_floor", "1", "settle the murk on the floor the sample is standing over, read from a field baked from the map at load; 0 reverts to the old camera-relative bed, which slides when you jump"};
cvar_t r_volumetric_flooroffset = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_flooroffset", "0", "raises (+) or lowers (-) the fog bed relative to the local floor, in world units; only used when r_volumetric_floor is on"};
cvar_t r_volumetric_fieldcell = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_fieldcell", "64", "world units per cell of the baked floor/liquid field; smaller resolves narrow ledges and small pools but costs more to bake and more memory. Takes effect on the next map load"};
cvar_t r_volumetric_water = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_water", "1", "thicken and tint the murk inside water, slime and lava volumes, using the baked field"};
cvar_t r_volumetric_waterdensity = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_waterdensity", "0.4", "murk thickness inside a liquid volume; independent of r_volumetric_density, so the air can be left clear. This wants to be LOW: the murk saturates at roughly 70/value world units, and anything past that is flat tint with no depth left in it"};
// PER-LIQUID DENSITY (2026-09-06). The murk's liquid TINT has always been
// per-kind -- water, slime and lava each have their own colour -- while the
// DENSITY was one shared scalar, so a waterdensity tuned to make swimming
// opaque gave a lava lake the same murk. Measured on Seb's e3m2 demo29 f1500:
// at his waterdensity 4.3 the fog buffer over the lava reads transmittance
// 0.276 against 0.612 at the shipped 0.4, and the orange is the lava tint
// riding on it. Slime defaults to -1 = INHERIT r_volumetric_waterdensity. Lava
// shipped at -1 for one day and that fixed nothing for the case it was built
// for (a raised water density); it now ships at 0.4, water's OWN shipped value,
// so a fresh install is byte-identical and a raised waterdensity stops reaching
// lava. It reaches an existing config because nobody had archived it.
cvar_t r_volumetric_slimedensity = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_slimedensity", "-1", "murk thickness inside SLIME, overriding r_volumetric_waterdensity for that liquid alone. -1 (the default) inherits the water value, which is what this always did"};
cvar_t r_volumetric_lavadensity = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_lavadensity", "0.4", "murk thickness inside LAVA. Ships at 0.4, the value water ships with, so a fresh install is unchanged -- but a water density raised for swimming no longer thickens the haze over a lava lake with it (2026-09-06, e3m2). -1 inherits the water value, which is what this did before. The tint is r_volumetric_lavacolor_*; the warm glow the AIR picks up near lava is r_volumetric_lavaglow"};
// The two accessors every consumer goes through, so the -1 inherit rule is
// written ONCE. Never read the cvars directly.
// WATERSURFACE (r_watersurface_clear): the in-liquid murk density AS THE VIEW SEES
// IT. From the air a pool should be seen into; once submerged the murk is the
// authored gloom. One factor, updated once per main view (R_RenderView) and eased
// over a fraction of a second at the waterline, multiplied into every site that
// hands the water density to a consumer -- the GL march, the fog kernel, the
// liquid fade, the CPU particle hook, the probe, and the slime and lava inherits.
// EXACTLY 1.0f while r_watersurface is 0, so those sites hand over the same
// bytes they always did.
static float r_watersurface_clearfactor = 1.0f;
static float R_Volumetric_WaterDensityView(void)
{
	return max(0.0f, r_volumetric_waterdensity.value) * r_watersurface_clearfactor;
}

static float R_Volumetric_SlimeDensity(void)
{
	return r_volumetric_slimedensity.value < 0.0f ? R_Volumetric_WaterDensityView() : max(0.0f, r_volumetric_slimedensity.value);
}
static float R_Volumetric_LavaDensity(void)
{
	return r_volumetric_lavadensity.value < 0.0f ? R_Volumetric_WaterDensityView() : max(0.0f, r_volumetric_lavadensity.value);
}
cvar_t r_volumetric_corner = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_corner", "2.5", "how much thicker the murk gets in corners and crevices, from the enclosure baked into the world field; 0 spreads it evenly through open air instead"};
cvar_t r_volumetric_mistlavacut = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_mistlavacut", "1", "how strongly lava suppresses the mist band that lies on liquid surfaces, 0-1. Cold mist on molten rock is the one liquid where that band reads wrong. 0 = the old density exactly"};
cvar_t r_volumetric_liquidfloor = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_liquidfloor", "1", "the LIQUID SURFACE IS THE AIR'S FLOOR (2026-09-07), 0-1. The air murk settles on the local floor, and over a pool the column's floor is the pool BED -- so the murk over every lake read hundreds of units above its floor and vanished, while it hung over the bank beside it: a hard step along the waterline (Seb's e1m4 report, measured). With this on, the height above the floor is capped at the height above the nearest liquid, which the baked field already carries as its signed distance; the ground layer settles on the water too. Beside a pool the cap also reaches a little sideways (the distance is to the nearest liquid, not straight down), thickening the murk within a cell or two of the edge. 0 = the old expression byte for byte; fractions blend"};
cvar_t r_volumetric_watermist = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_watermist", "0.5", "density of the mist lying on top of a liquid surface; 0 disables it"};
cvar_t r_volumetric_mistheight = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_mistheight", "26", "how far the surface mist reaches above the waterline, in world units"};
cvar_t r_volumetric_ground = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_ground", "2.5", "density of the GROUND FOG: a shallow dry-ice layer hugging each room's floor, with its own billow and drift. 0 disables. Needs r_volumetric_floor 1 and the baked field (any stock map)"};
cvar_t r_volumetric_groundheight = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_groundheight", "24", "ground fog falloff height in world units -- how thick the dry-ice layer sits (24 = below the knee; the eye is ~40 units up)"};
cvar_t r_volumetric_grounddeform = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_grounddeform", "14", "ground fog top swell amplitude in world units: the layer's surface rolls by this much as the large-scale noise drifts over it"};
cvar_t r_volumetric_groundnoisescale = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_groundnoisescale", "0.004", "ground fog billow frequency (world units -> noise). The volume tiles every 1/scale units with 8 puffs per tile: 0.004 = ~31-unit puffs, 0.002 = ~62"};
cvar_t r_volumetric_groundthresh = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_groundthresh", "0.25", "ground fog billow threshold 0-0.95: higher = sparser, more defined cloud banks. NOTE the noise concentrates near 0.5, so values much past 0.5 thin the layer to nothing"};
cvar_t r_volumetric_groundoffset = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_groundoffset", "0", "raises (or lowers, negative) the ground fog layer relative to the local floor, in world units"};
cvar_t r_volumetric_groundwind = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_groundwind", "4 1.7 0", "ground fog drift velocity in world units/sec. THE VALUE MUST BE QUOTED: r_volumetric_groundwind \"4 1.7 0\". Deliberately different from r_volumetric_wind so the layer creeps independently of the air mist. The magnitude is matched to the air murk's PERCEIVED drift: the billow is sampled at 2.5x the air's spatial frequency (31-unit puffs vs 78), so equal speeds read 2.5x faster -- the old \"14 6 0\" streamed rather than crept"};
cvar_t r_volumetric_groundcolor = {CF_CLIENT, "r_volumetric_groundcolor", "0.62 0.68 0.80", "sets the three r_volumetric_groundcolor_* channels at once. THE VALUE MUST BE QUOTED"};
cvar_t r_volumetric_groundcolor_red = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_groundcolor_red", "0.62", "ground fog colour, red channel"};
cvar_t r_volumetric_groundcolor_green = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_groundcolor_green", "0.68", "ground fog colour, green channel"};
cvar_t r_volumetric_groundcolor_blue = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_groundcolor_blue", "0.80", "ground fog colour, blue channel"};
cvar_t r_volumetric_particles = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_particles", "1", "fade particles, blood, gibs and explosions into the murk by distance, which the screen-space murk pass cannot do for itself because they are drawn after it. This is the strength of that fade: 1 is the calibrated match to the murk around them, 0 restores the old behaviour where blood and explosions read bright and clear through fog thick enough to hide the wall behind them"};
cvar_t r_volumetric_particles_alpha = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_particles_alpha", "1", "how alpha-blended particles (smoke, dust) fade into the murk: 1 = by scaling their ALPHA, so what shows through as they recede is the fog's own composited pixel -- lit by the kernel, beams and all -- and a black puff at the far wall is shrouded exactly as the wall is; 0 = the old lerp of their COLOUR toward the fog's authored base colour, which for a dark fog leaves a distant puff black at full opacity (the liquid-fade defect of 2026-08-31, here). Additive particles were always alpha-faded and are unaffected"};
cvar_t r_volumetric_liquidfade = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_liquidfade", "0.5", "fade alpha-blended water and slime into the murk by distance, which the screen-space murk pass cannot do for itself because a transparent surface writes no depth and is drawn after it. The twin of r_volumetric_particles, and this is the strength of that fade. THE DEFAULT IS 0.5 AND NOT 1: the fade over-estimates density by ignoring the noise patch term -- exactly as the CPU particle hook does -- so at 1 the water at your feet fogs as hard as the pool across the room and the near water stops reading as water. The demo18 ladder put the useful range at 0.25-0.5 and 0.5 is its midpoint, which is the value that passed QA. 0 leaves blended liquid reading at full brightness through fog thick enough to hide the wall behind it, restores the pre-2026-08-31 picture exactly, and is what every frozen bed pins. Needs r_wateralpha_force 1 on stock maps, or nothing renders blended in the first place"};
cvar_t r_volumetric_lavaglow = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_lavaglow", "0.5", "warm glow the air picks up near lava surfaces, in the lava colour (r_volumetric_lavacolor_*). Soft-limited like the god-ray beams, so it can never white the frame out. 0 disables; needs the liquid murk (r_volumetric_water 1) and the baked field"};
cvar_t r_volumetric_skyfog = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_skyfog", "0.8", "the most of the SKY the murk may take (0-1). Both fog marches deliberately run to full range against sky, which at ordinary densities replaces the sky with lit fog -- and under the fog kernel + EDR that lit fog's per-column structure reads as bright beams against near-black roof shadow (the e1m1 dark-boxes report). The cap floors the sky's own share of a sky pixel at 1 minus this and rescales the fog's share to match, so nothing over-adds and beams over sky survive. 1 = uncapped, the old behaviour exactly"};
cvar_t r_volumetric_ambient = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_ambient", "1", "light the murk from the world's own static lighting, read from a coarse irradiance grid baked at map load: dark rooms give dark fog, lit rooms give lit fog, and the ground mist stops glowing on its own. This is the blend strength -- 1 full, 0 restores the self-lit murk exactly"};
cvar_t r_volumetric_ambientgain = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_ambientgain", "1", "how strongly the baked lighting drives the murk's brightness; the calibration knob for matching the fog to the walls around it. 1 (since 2026-09-19) means a cell the map lights at its own full brightness gives fully lit fog and everything dimmer darkens in proportion; the old 1.5 clamped more than a third of all open air at fully lit even with an honest grid"};
cvar_t r_volumetric_ambientfloor = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_ambientfloor", "0.12", "the murk's minimum brightness share in a pitch-black room, so fog never goes fully invisible-black; 0 lets total darkness swallow it entirely"};
cvar_t r_volumetric_ambientdilate = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_ambientdilate", "1", "how the irradiance grid fills the cells inside walls, which trilinear filtering mixes into the air beside them: 1 = the distance-weighted MEAN of the open air around the cell, so a dark corridor beside a bright room stays dark; 0 = the BRIGHTEST open neighbour (the 2026-08-12 bake exactly), which on a grid where only one cell in twenty is open air pulls nearly every sample up to fully lit. Rebakes at once"};
cvar_t r_volumetric_irrcell = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_irrcell", "64", "world units per cell of the baked irradiance grid; smaller resolves light pools more tightly but costs bake time and memory. Takes effect on the next map load"};
cvar_t r_volumetric_dlight = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_dlight", "1", "dynamic lights scatter in the murk on the GL march tier: muzzle flashes, explosions and the thunderbolt light the fog around them. Gain; 0 skips the loop. (With the Metal fog kernel active this tier is superseded -- the kernel already lights fog from the full light list with shadow rays)"};
cvar_t r_volumetric_dlight_g = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_dlight_g", "0.4", "Henyey-Greenstein phase g for the march-tier dynamic lights, 0 isotropic to 0.9 strongly forward-favouring: higher makes lights bloom hardest when you look towards them through fog"};
cvar_t r_volumetric_scatter = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_scatter", "1", "scattering coefficient scale on the march-tier dynamic-light in-scatter term; 1 is the calibrated neutral"};
cvar_t r_volumetric_extinction = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_extinction", "1", "extinction (Beer-Lambert) scale on the murk's transmittance: above 1 the fog obscures more per unit of density without brightening. 1 is byte-neutral to the classic model"};
cvar_t r_volumetric_swirl = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_swirl", "6", "Kelvin-Helmholtz swirl on the murk: an analytic curl (divergence-free) field displaces the noise lookups so fog banks and ground mist ROLL and shear instead of sliding as one sheet. Amplitude in world units; 0 restores the un-swirled lookups exactly"};
cvar_t r_volumetric_swirlscale = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_swirlscale", "0.008", "spatial frequency of the swirl field (per world unit): smaller = broader, lazier circulation; larger = tighter eddies"};
cvar_t r_volumetric_swirlkh = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_swirlkh", "1.5", "extra swirl amplitude in a band at the ground-mist/air interface -- the Kelvin-Helmholtz billow read, where cloud tops roll up. Needs the floor-anchored bed (r_volumetric_floor 1); 0 leaves the interface no livelier than the body"};
cvar_t r_volumetric_noise2 = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_noise2", "1", "the fog-bake v2 noise: Perlin-Worley clumping, more octaves, a ridged streak and a bake-side domain warp replace the plain two-octave lattice -- cauliflower banks instead of blobs on a grid -- and the field is seeded per MAP, so no two levels share a fog pattern. 0 regenerates the classic noise exactly"};
cvar_t r_volumetric_noisesize = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_noisesize", "96", "texels per axis of the v2 noise volume (32-128): more texels carry the finer octaves without softening them. The classic generator ignores this and stays at 64"};
cvar_t r_volumetric_noise2_octaves = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_noise2_octaves", "4", "fBm octaves in the v2 density field (2-5); archived since 2026-09-03; rebakes on change"};
cvar_t r_volumetric_noise2_warp = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_noise2_warp", "0.35", "bake-side domain warp strength (0-1): what turns lattice blobs into folded, curled banks. Archived since 2026-09-03; rebakes on change"};
cvar_t r_volumetric_noise2_clump = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_noise2_clump", "0.5", "Perlin-Worley clumping weight (0-1): how much the density gathers into cauliflower puffs. Archived since 2026-09-03; rebakes on change"};
cvar_t r_volumetric_noise2_ridge = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_noise2_ridge", "0.25", "ridged-octave weight (0-1): thin bright wisps and streaks through the banks. Archived since 2026-09-03; rebakes on change"};
cvar_t r_volumetric_noise2_contrast = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_noise2_contrast", "1", "spread multiplier on the v2 density distribution. 1 matches the classic noise's spread EXACTLY -- r_volumetric_noisethresh and _groundthresh are tuned against it, so move this knowing every threshold moves with it. Archived since 2026-09-03; rebakes on change"};
cvar_t r_volumetric_watercolor = {CF_CLIENT, "r_volumetric_watercolor", "0.05 0.16 0.18", "convenience setter for r_volumetric_watercolor_red/_green/_blue; MUST BE QUOTED (see r_volumetric_color)"};
cvar_t r_volumetric_watercolor_red = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_watercolor_red", "0.05", "red channel of the murk colour inside water"};
cvar_t r_volumetric_watercolor_green = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_watercolor_green", "0.16", "green channel of the murk colour inside water"};
cvar_t r_volumetric_watercolor_blue = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_watercolor_blue", "0.18", "blue channel of the murk colour inside water; keep all three dark for gloom rather than haze"};
cvar_t r_volumetric_slimecolor = {CF_CLIENT, "r_volumetric_slimecolor", "0.09 0.15 0.05", "convenience setter for r_volumetric_slimecolor_red/_green/_blue; MUST BE QUOTED (see r_volumetric_color)"};
cvar_t r_volumetric_slimecolor_red = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_slimecolor_red", "0.09", "red channel of the murk colour inside slime"};
cvar_t r_volumetric_slimecolor_green = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_slimecolor_green", "0.15", "green channel of the murk colour inside slime"};
cvar_t r_volumetric_slimecolor_blue = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_slimecolor_blue", "0.05", "blue channel of the murk colour inside slime"};
cvar_t r_volumetric_lavacolor = {CF_CLIENT, "r_volumetric_lavacolor", "0.80 0.24 0.05", "convenience setter for r_volumetric_lavacolor_red/_green/_blue; MUST BE QUOTED (see r_volumetric_color)"};
cvar_t r_volumetric_lavacolor_red = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_lavacolor_red", "0.80", "red channel of the murk colour inside lava"};
cvar_t r_volumetric_lavacolor_green = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_lavacolor_green", "0.24", "green channel of the murk colour inside lava"};
cvar_t r_volumetric_lavacolor_blue = {CF_CLIENT | CF_ARCHIVE, "r_volumetric_lavacolor_blue", "0.05", "blue channel of the murk colour inside lava"};
cvar_t r_volumetric_debug = {CF_CLIENT, "r_volumetric_debug", "0", "visualise the reconstructed scene depth (requires r_volumetric 1): 1 = depth greyscale (near white, far black), 2 = world position XYZ as RGB banded every 256 units, 3 = box-filtered 64-unit world XY checkerboard (must stay welded to geometry), 4 = sampling density in pixels per checker cell (green >=8 solid, yellow 2-8 crawls under camera motion, red <2 aliasing guaranteed), 5 = height above the local floor from the baked field, banded every 64 units, 6 = liquid mask from the baked field (blue water, green slime, red lava), 7 = enclosure, the corner/crevice term exactly as the murk weights it"};
cvar_t r_redglow = {CF_CLIENT | CF_ARCHIVE, "r_redglow", "1.5", "make SATURATED REDS emit light of their own -- warning bands on Ogre grenades, red buttons, health boxes, and anything else genuinely red. This is the emission strength; 0 disables it entirely (toggling rebuilds the shaders, a one-off hitch). It tests the texel, not the surface, so it needs no per-asset artwork"};
cvar_t r_redglow_threshold = {CF_CLIENT | CF_ARCHIVE, "r_redglow_threshold", "0.45", "how red a texel must be before it emits, as the red channel's dominance over the greater of green and blue (0-1). Lower catches more; too low and Quake's brown brick starts to glow, because brown is dark orange"};
cvar_t r_redglow_minlevel = {CF_CLIENT | CF_ARCHIVE, "r_redglow_minlevel", "0.22", "how BRIGHT a red texel must be before it emits. Keeps dark maroon shadow detail and dried blood out of it"};
static qbool r_lavaflow_announced = false;
static float r_lavashimmer_lastreach = -1.0f;   // effective reach last announced
static float r_lavashimmer_lastsdf = -1.0f;     // and the bake range it was capped against
extern cvar_t m5_stock;   // r_shadow.c -- STOCK MODE, the 1996 read-side master
cvar_t r_lavashimmer = {CF_CLIENT | CF_ARCHIVE, "r_lavashimmer", "4", "heat haze: air above lava refracts the scene behind it, in pixels of displacement. The only cue in the game that says DANGEROUS before you stand in it. Needs the Metal renderer; 0 = off, and the shader is not compiled at all (toggling rebuilds shaders -- a one-off hitch)"};
cvar_t r_lavashimmer_height = {CF_CLIENT | CF_ARCHIVE, "r_lavashimmer_height", "128", "how far a lava surface heats the air around it, in world units -- the thickness of the column a view ray has to cross to be bent. SILENTLY CAPPED by the baked field's signed-distance range, which is twice r_volumetric_fieldcell (so 128 at the default 64-unit cell); past that the field cannot tell you how far from lava you are, and anything relying on it would haze the whole map"};
cvar_t r_lavashimmer_scale = {CF_CLIENT | CF_ARCHIVE, "r_lavashimmer_scale", "0.25", "size of the heat eddies: HIGHER is finer and more shimmer-like, lower is a slow warp. The first version of this effect was far too coarse and read as the masonry bending"};
cvar_t r_lavashimmer_speed = {CF_CLIENT | CF_ARCHIVE, "r_lavashimmer_speed", "0.55", "how fast the hot air rises through the haze. Heat shimmer is rapid; slow values read as a wobble"};
cvar_t r_lavashimmer_dist = {CF_CLIENT | CF_ARCHIVE, "r_lavashimmer_dist", "1200", "how far away a surface can be and still ripple, in world units. Also the cheap early-out: past it a pixel costs one depth sample and nothing else, and it is the length of the march that looks for hot air"};
cvar_t r_lavashimmer_taps = {CF_CLIENT | CF_ARCHIVE, "r_lavashimmer_taps", "6", "how many points along each view ray are tested for lava-heated air (1-16). This is what makes a far wall ripple when there is a lava pool BETWEEN it and you, rather than only surfaces that are themselves near lava. More taps grade the amount of haze more finely; fewer are cheaper and read more like an on/off mask"};
cvar_t r_lavashimmer_path = {CF_CLIENT | CF_ARCHIVE, "r_lavashimmer_path", "192", "how much lava-heated air a view ray must cross for FULL shimmer, in world units. Lower makes a thin wisp of hot air ripple as hard as a whole pool; higher reserves the full effect for looking the length of a lava lake"};
/// Is the heat shimmer live this frame? ONE expression, five consumers -- the
/// static parm, the scene-depth publish, the two frame-shape predicates and the
/// field resolve -- because the 4d bloom defect was two expressions that agreed
/// until a third branch moved one of them.
qbool R_LavaShimmer_Wanted(void)
{
	return r_lavashimmer.value > 0.0f && vid.m5postfx && !m5_stock.integer;
}
cvar_t r_lavaflow = {CF_CLIENT | CF_ARCHIVE, "r_lavaflow", "1", "make lava FLOW as well as boil: the crust drifts bodily across the sheet instead of only churning in place. Rides the boil's existing noise lookups, so it costs two extra instructions and no extra texture fetch. 0 = the old churn exactly"};
cvar_t r_lavaflow_speed = {CF_CLIENT | CF_ARCHIVE, "r_lavaflow_speed", "0.03", "how fast the lava crust drifts, in noise-volume widths per second"};
cvar_t r_lavaboil = {CF_CLIENT | CF_ARCHIVE, "r_lavaboil", "0.6", "animate lava surfaces with a boiling churn driven by the volumetric noise volume: the texture domain-warps and the glow pulses in slow hot spots. 0 = the old static sheet (toggling rebuilds the shaders -- a one-off hitch)"};
cvar_t r_waterswirl = {CF_CLIENT | CF_ARCHIVE, "r_waterswirl", "0.5", "animate water and slime surfaces with a gentle noise-volume churn on top of the classic scroll -- the sheet stops reading as a sliding decal. 0 = the old look exactly (toggling with r_teleportswirl also 0 rebuilds the shaders -- a one-off hitch)"};
// WATERSURFACE (2026-09-12, brief WATERSURFACE.md): the liquid surface refracts the
// frame beneath it. Screen-space -- one copy of the composited frame per view with a
// blended liquid in it, sampled per liquid fragment at a noise-displaced screen
// position; no extra scene render. The FTE look Seb asked for, minus its second and
// third scene renders per water batch.
cvar_t r_watersurface = {CF_CLIENT | CF_ARCHIVE, "r_watersurface", "1", "water and slime surfaces refract the scene beneath them (ON since 2026-09-13, Seb's eye; inert until the water renders blended, i.e. r_wateralpha_force on a stock map): the pool floor and the murk ripple under a moving surface instead of showing through a flat sheet. Screen-space, one frame copy per view with water in it, no extra scene render. 0 = the old sheet exactly (toggling rebuilds the shaders -- a one-off hitch)"};
cvar_t r_watersurface_distort = {CF_CLIENT | CF_ARCHIVE, "r_watersurface_distort", "0.1", "how far the water surface displaces what lies beneath it, as a fraction of the screen at the ripple's full tilt. 0.1 is FTE's own strength (its STRENGTH_REFR: '0.1 = fairly gentle, 0.2 = big waves'). Scaled by the murk's fade, so a surface the fog has dissolved does not go on shimmering the fog beneath it"};
cvar_t r_watersurface_speed = {CF_CLIENT | CF_ARCHIVE, "r_watersurface_speed", "1", "pace of the ripple: 1 is FTE's own (two layers of the water's texture scrolling at 0.1 and 0.097 tiles a second across each other, the surface warp at one radian a second); 2 doubles it"};
cvar_t r_watersurface_warp = {CF_CLIENT | CF_ARCHIVE, "r_watersurface_warp", "1", "the classic per-pixel wobble of the water's own texture (FTE's defaultwarp: tc + sin(tc + time) * 0.125), as a multiple of that amplitude; 0 = the texture sits still and only the swirl churns it"};
cvar_t r_watersurface_bump = {CF_CLIENT | CF_ARCHIVE, "r_watersurface_bump", "4", "how steep the ripple's normal is, read out of the water texture's own luminance gradient (FTE makes its water normal map from the diffuse the same way; 4 is tenebrae's bumpscale). Higher = a more broken, more sparkling surface and a stronger refraction at the same r_watersurface_distort"};
cvar_t r_watersurface_guard = {CF_CLIENT, "r_watersurface_guard", "1", "reject a displaced sample that lands on something NEARER than the water surface (a torch on the bank would otherwise be pulled into the pool). 0 = the raw displacement, for A/B"};
cvar_t r_watersurface_opacity = {CF_CLIENT | CF_ARCHIVE, "r_watersurface_opacity", "0", "the surface texture's share of the picture when you look straight down into the water (0 = FTE's fully clear water); the Fresnel term raises it toward 1 as the view angle flattens"};
cvar_t r_watersurface_fresnel = {CF_CLIENT | CF_ARCHIVE, "r_watersurface_fresnel", "5", "how quickly the surface texture takes over as the view angle flattens -- the Fresnel exponent on the RIPPLED normal. 5 is FTE's effective exponent (measured 2026-09-13: its #FRESNEL=4 is a dead define); lower shows the texture sooner"};
cvar_t r_watersurface_taper = {CF_CLIENT | CF_ARCHIVE, "r_watersurface_taper", "48", "world units below the surface over which the ripple's displacement fades to nothing, so the floor at the water's edge is not smeared out from under it. FTE has no taper at its stock water styles (a hard seam at the far bank); 0 reproduces that"};
cvar_t r_watersurface_tint_red = {CF_CLIENT | CF_ARCHIVE, "r_watersurface_tint_red", "0.7", "tint of the view refracted through water, red (FTE's TINT_REFR 0.7 0.8 0.7); fades with the surface so far water is the murk's own pixel"};
cvar_t r_watersurface_tint_green = {CF_CLIENT | CF_ARCHIVE, "r_watersurface_tint_green", "0.8", "tint of the view refracted through water, green"};
cvar_t r_watersurface_tint_blue = {CF_CLIENT | CF_ARCHIVE, "r_watersurface_tint_blue", "0.7", "tint of the view refracted through water, blue"};
cvar_t r_watersurface_clear = {CF_CLIENT | CF_ARCHIVE, "r_watersurface_clear", "1", "how thick the in-water murk is when you look INTO water from the air, as a fraction of r_volumetric_waterdensity. 1 = the same murk as when submerged, which is what Seb chose on 2026-09-13 (\"1.0 is right\") -- deep pools hide their bottoms, shallow ones stay readable; 0 = FTE's crystal-clear water from the bank; the submerged view keeps the full density, eased over a fraction of a second at the waterline. A pool you can see into from the bank and a murk you cannot see through once you are in it are the same water"};
// WATERSURFACE asks for the OFFSCREEN path with a depth TEXTURE, on both
// backends -- the murk's own shape (r_volumetric in R_BlendView_IsTrivial, the
// depth-as-texture choice in R_Bloom_StartFrame, the scene-depth publish in
// R_RenderView). The refraction's depth guard samples the scene depth, and
// without an asker of its own it ran only where something ELSE had asked:
// measured 2026-09-12 on the rt_liquid parity vantage, the feature's own notify
// line read "guard on" on Metal (the Metal-only shimmer asker had forced the
// offscreen path) and "guard off" on GL (the bare direct path, no depth
// texture), and 453 shoreline pixels disagreed between the backends. At 0
// nothing here is asked, so the direct path is untouched.
static qbool R_WaterSurface_Wanted(void)
{
	return r_watersurface.integer != 0;
}
// BEAUTY A4 (2026-09-16): soft particles. The distance, in world units, over
// which a particle fades where it meets a wall or the floor, instead of the
// hard line a billboard draws across the geometry it intersects. 0 = today.
// BEAUTY C2 (2026-09-17): caustics on submerged surfaces.
cvar_t r_caustics = {CF_CLIENT | CF_ARCHIVE, "r_caustics", "0.6", "BEAUTY C2: CAUSTICS on the floors and walls of pools -- a moving cell pattern of light on every world surface that lies inside a liquid, seen through the water from above (or from within). Per pixel, from the baked world field (the murk's own liquid map, so no surface list and no new texture in the surface shader) and the noise volume the lava boil binds; multiplied into the surface's colour, so under wall lighting the RT composite carries it as a light modulation. This is the strength, 0.6 by default since 2026-09-17 on Seb's eye. Needs r_volumetric_liquidfade above 0 (the field and the world position ride that parm) and r_lavaboil above 0 (the noise volume) -- both the shipped defaults; inert otherwise, and inert on entities, the HUD and the liquids themselves. 0 = today's bytes. Console-only; exec caustics_on.cfg / caustics_off.cfg"};
cvar_t r_caustics_scale = {CF_CLIENT | CF_ARCHIVE, "r_caustics_scale", "0.012", "r_caustics: the pattern's scale in cycles per world unit (0.012 = cells ~80 units across)"};
cvar_t r_caustics_speed = {CF_CLIENT | CF_ARCHIVE, "r_caustics_speed", "0.35", "r_caustics: how fast the pattern drifts and changes"};
cvar_t cl_particles_soft = {CF_CLIENT | CF_ARCHIVE, "cl_particles_soft", "24", "BEAUTY A4: soft particles -- smoke, fire, dust and every other particle fade out over this many world units where they intersect a wall or the floor, instead of drawing a hard line across it (a smoke puff sitting in a doorway, an explosion's fireball half inside the wall it hit). Each particle fragment compares its own depth with the scene's behind it and fades by the gap. Needs the scene depth, which every offscreen path publishes (r_viewfbo, the murk, EDR, the temporal scaler -- every tier); on the bare direct path it stands down. 16-32 is the sensible range and 24 is the shipped default since 2026-09-17, on Seb's eye. 0 = the hard-edged 2001 picture exactly (toggling rebuilds the shaders -- a one-off hitch)"};
static qbool R_SoftParticles_Wanted(void)
{
	return cl_particles_soft.value > 0.0f && !m5_stock.integer;
}
// BEAUTY A5: refracting particles (the shockwave). The cvar lives in
// cl_particles.c beside the blend it names.
static qbool R_PartRefract_Wanted(void)
{
	return cl_particles_refract.value > 0.0f && !m5_stock.integer;
}
cvar_t r_teleportswirl = {CF_CLIENT | CF_ARCHIVE, "r_teleportswirl", "1", "animate teleporter starfields with a slow rotation about the tile centre plus a churn -- the pad reads as a live vortex instead of wallpaper. 0 = the old look exactly (toggling with r_waterswirl also 0 rebuilds the shaders -- a one-off hitch)"};
cvar_t r_teleportswirl_pivot = {CF_CLIENT | CF_ARCHIVE, "r_teleportswirl_pivot", "2", "what the teleporter starfield rotates ABOUT. 2 = NOTHING -- the rotation is off and the noise churn is the whole animation, which is the only arrangement that is both uniform on every pad and free of seams. 1 = the nearest texture tile centre, which is uniform on every pad wherever it sits in the map and is a real rotation at the surface. 0 = the pre-2026-09-01 behaviour, a fixed texcoord (0.5,0.5) -- and because Q1 texcoords are absolute world coordinates over the texture size, that pivot is near the MAP ORIGIN, so a pad N tiles out rotates on an N-tile lever arm. Measured on start.bsp: its 24 teleport faces ran 94 to 999 px/s, a 10.6x spread inside one map -- and a tile pivot fixes that but seams at every tile boundary, because a rotation cannot be both uniform across surfaces and continuous within one. Hence the shipped 2. r_teleportswirl_pivot 0 with r_teleportswirl_churn 1 restores the old frame exactly"};
cvar_t r_teleportswirl_churn = {CF_CLIENT | CF_ARCHIVE, "r_teleportswirl_churn", "8", "how fast the teleporter starfield's noise warp EVOLVES, as a multiple of the historic rate. The warp is evaluated on the already-rotated coordinate, so at 1 it is a fixed deformation carried around by the rotation rather than something you watch change. Higher makes the starfield visibly boil. Ships at 8: measured on the start.bsp curtain the motion goes 2.73 / 7.16 / 9.14 / 10.63 at rates 1 / 4 / 8 / 16 and saturates past 16, so 8 is the knee. 1 = the historic rate exactly"};
cvar_t r_gamma_analytic = {CF_CLIENT | CF_ARCHIVE, "r_gamma_analytic", "0", "evaluate the gamma curve in the shader instead of looking it up in a 256-entry 8-bit table. Numerically the same curve -- and slightly more accurate, since the table quantises both its input and its output -- but it has no 1.0 ceiling, which is what extended-range (EDR) output needs. Ignored while v_psycho is on or vid_sRGB is in effect, because the table is then not this curve. Toggling rebuilds the shaders, a one-off hitch"};
cvar_t r_edr = {CF_CLIENT | CF_ARCHIVE, "r_edr", "0", "extended dynamic range (HDR) output on a Metal HDR display (METAL.md Phase 7; also on the Video Options menu): highlights that today clip at white are handed to the compositor above it instead, so the scene can genuinely outshine the HUD. 0 off (the shipped default), 1 on where the display reports EDR potential, 2 ask anyway on a display that reports none -- a diagnostic, not a setting. Needs the Metal renderpath; the other two prerequisites are FORCED while it is on (a 16-bit float scene buffer as if r_viewfbo 2, and the analytic gamma curve as if r_gamma_analytic 1 -- the 256-entry LUT has no headroom by construction), so this one cvar is the whole switch. Turning it on or off rebuilds the shaders unless r_gamma_analytic is already 1, a one-off hitch. r_edr_report explains any refusal"};

// The ONE predicate for "EDR forces its prerequisites" (Phase 8 -- 'HDR always
// available'). Five consumers: the edr_wanted write in R_UpdateVariables, the
// scene-buffer format in R_Bloom_StartFrame, the offscreen-path force in
// R_BlendView_IsTrivial, the GAMMAANALYTIC static parm, and the HdrShoulder
// gate. One function rather than five agreeing expressions, per the 4d bloom
// lesson (a permutation bit and an attribute enable derived from two
// expressions that merely happened to agree). Deliberately does NOT test
// r_viewfbo or r_gamma_analytic: those are what it forces. VID_GetGammaAnalytic
// is the term that refuses under v_psycho and vid_sRGB.
static qbool R_EDR_Wanted(void)
{
	float ig[3], sc[3], bs[3], cb;
	return r_edr.integer != 0
		&& !m5_stock.integer
		&& vid.renderpath == RENDERPATH_METAL
		&& VID_GetGammaAnalytic(ig, sc, bs, &cb);
}

// STOCK MODE's two effective-value accessors -- defined below, beside the cvars
// they read. The offscreen scene path and the bloom chain are each ASKED FOR in
// more than one place and 1996 asked for neither; reading the player's cvar
// THROUGH these is what lets m5_stock stay a pure read-side override, so
// m5_stock 0 restores their own picture bit for bit and no tuning of theirs is
// ever written over. One function per quantity rather than an `if` at each site
// -- the 4d bloom lesson, where a permutation bit and an attribute enable came
// from two expressions that merely happened to agree.
//
// NOTE what stock deliberately does NOT suppress: the GAMMA stage. GLQuake
// applied gamma through the hardware ramps; here it is the postprocess, so a
// player on a non-unit v_gamma keeps the offscreen path under stock and keeps
// their brightness. Suppressing it would not be period, it would just be dark.
int R_ViewFBO(void);
qbool R_Bloom_Wanted(void);

cvar_t r_lavaglow = {CF_CLIENT | CF_ARCHIVE, "r_lavaglow", "1.5", "brightness of lava's emissive glow (the whole visible lava image is its glow layer). 1 = the classic brightness, higher = hotter; the brightest texels saturate to white-hot"};
cvar_t r_transparentdepthmasking = {CF_CLIENT | CF_ARCHIVE, "r_transparentdepthmasking", "0", "enables depth writes on transparent meshes whose materially is normally opaque, this prevents seeing the inside of a transparent mesh"};
cvar_t r_transparent_sortmindist = {CF_CLIENT | CF_ARCHIVE, "r_transparent_sortmindist", "0", "lower distance limit for transparent sorting"};
cvar_t r_transparent_sortmaxdist = {CF_CLIENT | CF_ARCHIVE, "r_transparent_sortmaxdist", "32768", "upper distance limit for transparent sorting"};
cvar_t r_transparent_sortarraysize = {CF_CLIENT | CF_ARCHIVE, "r_transparent_sortarraysize", "4096", "number of distance-sorting layers"};
cvar_t r_celshading = {CF_CLIENT | CF_ARCHIVE, "r_celshading", "0", "cartoon-style light shading (OpenGL 2.x only)"}; // FIXME remove OpenGL 2.x only once implemented for DX9
cvar_t r_celoutlines = {CF_CLIENT | CF_ARCHIVE, "r_celoutlines", "0", "cartoon-style outlines (requires r_shadow_deferred)"};

cvar_t gl_fogenable = {CF_CLIENT, "gl_fogenable", "0", "nehahra fog enable (for Nehahra compatibility only)"};
cvar_t gl_fogdensity = {CF_CLIENT, "gl_fogdensity", "0.25", "nehahra fog density (recommend values below 0.1) (for Nehahra compatibility only)"};
cvar_t gl_fogred = {CF_CLIENT, "gl_fogred","0.3", "nehahra fog color red value (for Nehahra compatibility only)"};
cvar_t gl_foggreen = {CF_CLIENT, "gl_foggreen","0.3", "nehahra fog color green value (for Nehahra compatibility only)"};
cvar_t gl_fogblue = {CF_CLIENT, "gl_fogblue","0.3", "nehahra fog color blue value (for Nehahra compatibility only)"};
cvar_t gl_fogstart = {CF_CLIENT, "gl_fogstart", "0", "nehahra fog start distance (for Nehahra compatibility only)"};
cvar_t gl_fogend = {CF_CLIENT, "gl_fogend","0", "nehahra fog end distance (for Nehahra compatibility only)"};
cvar_t gl_skyclip = {CF_CLIENT, "gl_skyclip", "4608", "nehahra farclip distance - the real fog end (for Nehahra compatibility only)"};

cvar_t r_texture_dds_load = {CF_CLIENT | CF_ARCHIVE, "r_texture_dds_load", "0", "load compressed dds/filename.dds texture instead of filename.tga, if the file exists (requires driver support)"};
cvar_t r_texture_dds_save = {CF_CLIENT | CF_ARCHIVE, "r_texture_dds_save", "0", "save compressed dds/filename.dds texture when filename.tga is loaded, so that it can be loaded instead next time"};

cvar_t r_usedepthtextures = {CF_CLIENT | CF_ARCHIVE, "r_usedepthtextures", "1", "use depth texture instead of depth renderbuffer where possible, uses less video memory but may render slower (or faster) depending on hardware"};
cvar_t r_hdr_shoulder = {CF_CLIENT | CF_ARCHIVE, "r_hdr_shoulder", "0", "roll highlights off instead of clipping them, when the scene is rendered into a float buffer (needs r_viewfbo 2). This is the KNEE: below it nothing changes at all, above it the picture rolls smoothly towards white instead of flat-clipping, and the roll is applied on the brightest channel so hot highlights keep their hue. 0 disables. 0.75 is a good starting point. Does nothing at r_viewfbo 0 or 1, where the scene is already clamped before it gets here"};
cvar_t r_viewfbo = {CF_CLIENT | CF_ARCHIVE, "r_viewfbo", "0", "enables use of an 8bit (1) or 16bit (2) or 32bit (3) per component float framebuffer render, which may be at a different resolution than the video mode; the default setting of 0 uses a framebuffer render when required, and renders directly to the screen otherwise"};
cvar_t r_rendertarget_debug = {CF_CLIENT, "r_rendertarget_debug", "-1", "replaces the view with the contents of the specified render target (by number - note that these can fluctuate depending on scene)"};
cvar_t r_viewscale = {CF_CLIENT | CF_ARCHIVE, "r_viewscale", "1", "scaling factor for resolution of the fbo rendering method, must be > 0, can be above 1 for a costly antialiasing behavior, typical values are 0.5 for 1/4th as many pixels rendered, or 1 for normal rendering"};
cvar_t r_metalfx = {CF_CLIENT | CF_ARCHIVE, "r_metalfx", "0", "how the postprocessed frame reaches the screen: 0 plain bilinear, 1 the MetalFX SPATIAL scaler (METAL.md Phase 8-5), 2 the MetalFX TEMPORAL scaler -- which accumulates jittered frames across time with motion vectors, so it also denoises the fog and shadow grain and can run at r_viewscale 1 with no resolution loss at all. Needs the Metal renderer, and 1 additionally needs r_viewscale below 1 (a 1:1 spatial upscale is a pointless copy; a 1:1 temporal one is anti-aliasing). The scale knob stays r_viewscale. Falls back to the bilinear path silently whenever the scaler cannot run (GL renderer, r_viewscale_fpsscaling, r_viewfbo 3, envmap, stereo, and for 2 also motion blur). The HUD and console stay native-resolution either way"};
cvar_t r_fxaa_post_span = {CF_CLIENT | CF_ARCHIVE, "r_fxaa_post_span", "8", "how far the post-upscale FXAA may blend ALONG an edge, in native pixels. THE NUMBER THAT DECIDES WHICH EDGES IT CAN FIX: the filter's farthest tap is span/2, so at the shipped 8 it reaches 4 px and resolves a staircase whose steps are shorter than that -- a STEEP silhouette -- while a SHALLOW one (a near-horizontal roof line, a lit ledge receding to a vanishing point) has steps of 6-18 px and survives almost untouched. Measured against an analytic ground-truth edge 2026-09-18: on a 1:6 slope span 8 removes 31%% of the staircase and span 16 removes 64%%, at the cost of a steep edge reading slightly softer (it is already solved at 8). Above ~24 both get worse, because the taps land on unrelated content. 8 is the old behaviour exactly. Clamped 4..32. Console only"};
cvar_t r_fxaa_post = {CF_CLIENT | CF_ARCHIVE, "r_fxaa_post", "0", "run FXAA at NATIVE resolution AFTER the MetalFX upscale, instead of only at render resolution with the rest of the postprocess. THE REASON IT EXISTS: the postprocess target is sized from r_fb.rt_screen, so FXAA, gamma, the fringe and the saturation all run at r_viewscale -- 1280x720 on Good/Better/Best, 720x405 on Superfast -- and the scaler then MAGNIFIES the already-antialiased frame. FXAA therefore never sees the staircase the magnification creates, which is exactly the jagged silhouette at a roof against sky or a near object against a far one (measured 2026-09-18: FXAA off is indistinguishable from FXAA on at those edges). With this on, the scaler writes a pooled native-resolution target and one extra fullscreen pass antialiases THAT, at the resolution the staircase actually exists at. Needs the Metal renderer and r_metalfx; falls back to the plain path silently otherwise (the frame is still correct, just not post-antialiased). r_fxaa still controls the render-res pass -- r_fxaa 0 with this on is the clean configuration, one FXAA at the right resolution; both on is a legitimate but slightly softer double. Costs one 1080p fullscreen pass. Console only"};
cvar_t r_smaa = {CF_CLIENT | CF_ARCHIVE, "r_smaa", "0", "analytic MLAA -- SMAA's orthogonal core, computed in the shader with no lookup tables -- at NATIVE resolution after the MetalFX upscale, in place of r_fxaa_post. WHY IT EXISTS: FXAA corrects at most HALF A PIXEL and only near a step's ENDS, so a shallow silhouette (a roof line against sky, a lit ledge receding) whose steps run 6-18 px keeps four pixels in six untouched, and raising r_fxaa_post_span past ~24 makes every slope WORSE because the taps land on unrelated content. Measured against an analytic ground-truth edge (test/aaedge.py): on a 1:6 slope the staircase is 0.340 with no AA, 0.230 at span 8, 0.124 at span 16 and 0.008 here. This pass finds each step's WHOLE extent along the edge and blends every pixel in it by that pixel's own coverage, which is a ramp rather than a nudge. Three fullscreen native-resolution passes where FXAA is one, so it is not free. Needs the Metal renderer and r_metalfx; falls back silently otherwise, and the frame is still correct, merely not antialiased. IT SUPERSEDES r_fxaa_post -- two edge filters stacked is a softer frame for no gain -- and says so once when both are set. r_fxaa (the render-resolution pass) is independent, and the tier table now pins it OFF because at r_viewscale 0.375 it only softens the scaler's input. **AN M5 QUALITY TIER LEVER SINCE 2026-09-19**: 1 on every tier but Stock, because the 09-18 finding is that this pass and a raster-exact RT term (rt_metal_scale 1) are a PAIR -- neither half reaches Seb's roof-against-sky edge alone (measured on aaroof.dem: 0.362 untreated, 0.384 with this alone, 0.318 with the term alone, 0.229 with both)"};
cvar_t r_smaa_threshold = {CF_CLIENT, "r_smaa_threshold", "0.08", "the luma step that counts as an edge for r_smaa, ABSOLUTE on the display-encoded frame this pass is fed. Lower catches more edges and risks treating texture as geometry; higher leaves shallow contrast aliased. Note the frame reaches ~1.756 under r_edr 1, so a highlight's edges cross an absolute threshold more readily than a midtone's -- correct rather than a defect, and r_smaa_adapt is what keeps the detector off ordinary texture. Console only"};
cvar_t r_smaa_search = {CF_CLIENT, "r_smaa_search", "16", "how far r_smaa walks along an edge line looking for the step's end, in native pixels each way. THIS IS THE REACH, and unlike FXAA's span it costs accuracy rather than correctness when it runs out: a run longer than this contributes nothing from the capped side, so the pass leaves it alone rather than inventing a slope. Seb's reported steps are 6-18 px, so 16 covers them; the search loop is the pass's whole cost, and doubling this roughly doubles it. Clamped 4..32. Console only"};
cvar_t r_smaa_adapt = {CF_CLIENT, "r_smaa_adapt", "2", "r_smaa's local-contrast adaptation factor (SMAA's own): an edge survives only if its own luma step is at least 1/this of the biggest step in its neighbourhood, so the strong silhouette in a busy region wins and the texture around it does not also register. This is what stops a morphological pass smearing detail. 1 keeps only the locally strongest edges, large values disable the adaptation. Console only"};
cvar_t r_smaa_debug = {CF_CLIENT, "r_smaa_debug", "0", "show r_smaa's intermediates instead of the antialiased frame: 1 the EDGES (red a left boundary, green a top one), 2 the blend WEIGHTS as the last pass gathers them (red the horizontal correction, green the vertical). A pass that silently did not run renders a perfectly correct frame that is merely not antialiased, which is the failure this tree keeps paying for; these are what say which of the three passes is at fault. Console only"};
cvar_t r_metalfx_jitter = {CF_CLIENT, "r_metalfx_jitter", "1", "sub-pixel jitter of the raster projection under r_metalfx 2, as a fraction of a render pixel (0 disables it). Temporal upscaling still denoises without jitter -- it just cannot RECONSTRUCT, because every frame samples the same points -- so 0 is a legitimate configuration and the safest A/B. Console only"};
cvar_t r_metalfx_jitterfix = {CF_CLIENT, "r_metalfx_jitterfix", "0", "pin the jitter to a CONSTANT offset in render pixels instead of walking the Halton sequence, as \"x y\" -- the probe that settles the jitter's magnitude and sign against an unjittered frame. 0 walks the sequence. Console only"};
cvar_t r_metalfx_signs = {CF_CLIENT, "r_metalfx_signs", "1", "temporal-upscaling sign conventions, a bitmask: 1 negates jitterOffsetX, 2 negates jitterOffsetY, 4 negates the motion vectors' x, 8 negates their y. The default is MEASURED, not read off the header, and the measurement is repeatable with this cvar: pin a constant jitter (r_metalfx_jitterfix \"2 0\") and cross-correlate the frame against r_metalfx_jitterfix \"0 0\" -- the scaler is told the jitter so it can UNDO it, so the correct sign cancels the raster shift and a wrong one DOUBLES it. Measured on this hardware: x wrong at 0 (-6 output px) and right at 1 (0 px); y right at 0 and wrong at 2 (-5 px). The asymmetry is the GL-layout v-flip, which gives the y axis a sign the x axis does not have. Console only"};
cvar_t r_metalfx_viewmodel = {CF_CLIENT, "r_metalfx_viewmodel", "1", "under r_metalfx 2, give the view weapon its OWN motion vectors instead of the world's. The weapon is rigidly attached to the camera, so camera-only vectors are wrong for it by the entire camera motion -- measured at 8-13% of the previous frame lingering on it during a turn, over the fifth of the screen a player watches most. Identified from the short depth range gl_rmain.c draws it into, so it costs one compare and no geometry pass. 0 reverts to camera-only. Console only"};
cvar_t r_metalfx_entities = {CF_CLIENT, "r_metalfx_entities", "1", "under r_metalfx 2, give moving ENTITIES (monsters, projectiles, doors, lifts) their own motion vectors instead of the world's. Without it a fast-moving object is told it moved with the background and carries 19-29% of its previous frame as a trail (measured on a static-camera rocket bed). Rigid-body only: a walking monster's body gets the right vector, its swinging limbs get the body's. 0 = the camera-only fill plus the view weapon, i.e. T2a byte for byte. Console only"};
cvar_t r_metalfx_reactive = {CF_CLIENT | CF_ARCHIVE, "r_metalfx_reactive", "0", "under r_metalfx 2, feed the temporal scaler a REACTIVE MASK from the dynamic-light footprint: per pixel, how much any moving light (rocket glow, muzzle flash, explosion) reaches the world point seen there, telling the scaler to distrust its history by that much. This is the one ghost no motion vector can describe -- a light moving across STILL geometry has the geometry's (zero) vector -- measured at 19-29% of the previous frame lingering on the lit floor beside a rocket. The value is the gain (1 = a light at full strength means full distrust; try 2-4). 0 = no mask, byte for byte. ARCHIVED since 2026-08-20, because it is an M5 Quality lever: an unarchived lever resets to 0 on the next launch and the tier would read Custom every boot. Console only (the tier row sets it)"};
cvar_t r_metalfx_reactive_particles = {CF_CLIENT, "r_metalfx_reactive_particles", "3", "under r_metalfx_reactive, also feed the reactive mask the PARTICLES' own screen footprint -- smoke, blood, sparks, bubbles, the explosion burst -- which is the ghost the light footprint cannot see by itself: particles write no depth and carry no motion vector, so nothing else tells the scaler they are there, and on the static-camera rocket bed 78% of what moves behind a rocket is its smoke trail. Every particle batch the scene drew is stamped into the mask, weighted by its visible strength times its texture's own shape times this gain -- a soft puff marks a soft disc, not a square. MEASURED (2026-08-19, ghostbed f419-421, majority boot state): the smoke's lingering history is ~0.06 of the previous frame with no mask; the stamp alone (light arm off) takes it to within ~0.015 of the scaler's own reset-every-frame yardstick (0.320 -> 0.279 against 0.263), with no rise in the crawl row, and the gain sweep 1/3/8/30 read identically there because overlapping puffs saturate -- the gain only matters for a lone faint particle (rocket smoke is an additive 0x30-0x60 grey at low alpha). On a ROCKET the light footprint already covers the fresh smoke (it lives inside the rocket's own light), so the stamp's own reach is the lightless particles: blood, gib trails, sparks, debris after a flash has died. Cost inside the motion pass's window-to-window noise on ghostbed and demo12. 0 = the light footprint only, byte for byte. Console only"};
cvar_t r_metalfx_reactive_trail = {CF_CLIENT, "r_metalfx_reactive_trail", "0", "under r_metalfx_reactive_particles, also stamp the PREVIOUS frame's particle footprint (re-projected through this frame's camera) -- the trailing edge, where a puff WAS and the history still holds it. The value scales that stamp. MEASURED 2026-08-19 on the rocket bed: no effect on any class or frame (particle-now 0.278 either way, particle-was 0.081 vs 0.080) -- overlapping puffs already cover each other's trailing edge -- so it ships OFF and costs a second replay when on. Kept as the measured off-switch. Console only"};
cvar_t r_metalfx_reactive_force = {CF_CLIENT, "r_metalfx_reactive_force", "0", "PROBE: under a live reactive mask, overwrite it with a constant -- 1 = white everywhere (the scaler is told to ignore its history at every pixel), -1 = black everywhere (a mask that is present but says nothing), 2 = leave the mask alone but set the scaler's own RESET flag every frame (the API's way of discarding history, the yardstick a white mask should match). The ends of what the mask can do, for measuring the scaler's own behaviour end to end; not a play setting. 0 = the real mask. Console only"};
cvar_t r_metalfx_reactive_debug = {CF_CLIENT, "r_metalfx_reactive_debug", "0", "under r_metalfx 2 with a reactive mask live, show the MASK instead of the scene (white = the scaler is told to distrust its history there; the history is reset every frame while it is on). Soft puff shapes plus the light footprint's disc is right; squares around puffs, or anything behind a wall, is wrong. Look at it before reading any number. Console only"};
cvar_t r_metalfx_debugview = {CF_CLIENT, "r_metalfx_debugview", "0", "under r_metalfx 2, show the motion-vector buffer instead of the scene: red/green are the x/y displacement about mid grey, and the VALUE is the full-scale displacement in pixels (20 maps +/-20 px across the colour range). The history is reset every frame while it is on, so what you see is this frame's vectors rather than an accumulation. The one instrument that makes the orientation and sign questions answerable by looking. Console only"};
cvar_t r_viewscale_fpsscaling = {CF_CLIENT | CF_ARCHIVE, "r_viewscale_fpsscaling", "0", "change resolution based on framerate"};
cvar_t r_viewscale_fpsscaling_min = {CF_CLIENT | CF_ARCHIVE, "r_viewscale_fpsscaling_min", "0.0625", "worst acceptable quality"};
cvar_t r_viewscale_fpsscaling_multiply = {CF_CLIENT | CF_ARCHIVE, "r_viewscale_fpsscaling_multiply", "5", "adjust quality up or down by the frametime difference from 1.0/target, multiplied by this factor"};
cvar_t r_viewscale_fpsscaling_stepsize = {CF_CLIENT | CF_ARCHIVE, "r_viewscale_fpsscaling_stepsize", "0.01", "smallest adjustment to hit the target framerate (this value prevents minute oscillations)"};
cvar_t r_viewscale_fpsscaling_stepmax = {CF_CLIENT | CF_ARCHIVE, "r_viewscale_fpsscaling_stepmax", "1.00", "largest adjustment to hit the target framerate (this value prevents wild overshooting of the estimate)"};
cvar_t r_viewscale_fpsscaling_target = {CF_CLIENT | CF_ARCHIVE, "r_viewscale_fpsscaling_target", "70", "desired framerate"};

cvar_t r_glsl_skeletal = {CF_CLIENT | CF_ARCHIVE, "r_glsl_skeletal", "1", "render skeletal models faster using a gpu-skinning technique"};
cvar_t r_glsl_deluxemapping = {CF_CLIENT | CF_ARCHIVE, "r_glsl_deluxemapping", "1", "use per pixel lighting on deluxemap-compiled q3bsp maps (or a value of 2 forces deluxemap shading even without deluxemaps)"};
cvar_t r_glsl_offsetmapping = {CF_CLIENT | CF_ARCHIVE, "r_glsl_offsetmapping", "0", "offset mapping effect (also known as parallax mapping or virtual displacement mapping)"};
cvar_t r_glsl_offsetmapping_steps = {CF_CLIENT | CF_ARCHIVE, "r_glsl_offsetmapping_steps", "2", "offset mapping steps (note: too high values may be not supported by your GPU)"};
cvar_t r_glsl_offsetmapping_reliefmapping = {CF_CLIENT | CF_ARCHIVE, "r_glsl_offsetmapping_reliefmapping", "0", "relief mapping effect (higher quality)"};
cvar_t r_glsl_offsetmapping_reliefmapping_steps = {CF_CLIENT | CF_ARCHIVE, "r_glsl_offsetmapping_reliefmapping_steps", "10", "relief mapping steps (note: too high values may be not supported by your GPU)"};
cvar_t r_glsl_offsetmapping_reliefmapping_refinesteps = {CF_CLIENT | CF_ARCHIVE, "r_glsl_offsetmapping_reliefmapping_refinesteps", "5", "relief mapping refine steps (these are a binary search executed as the last step as given by r_glsl_offsetmapping_reliefmapping_steps)"};
cvar_t r_glsl_offsetmapping_scale = {CF_CLIENT | CF_ARCHIVE, "r_glsl_offsetmapping_scale", "0.04", "how deep the offset mapping effect is"};
cvar_t r_glsl_offsetmapping_lod = {CF_CLIENT | CF_ARCHIVE, "r_glsl_offsetmapping_lod", "0", "apply distance-based level-of-detail correction to number of offsetmappig steps, effectively making it render faster on large open-area maps"};
cvar_t r_glsl_offsetmapping_lod_distance = {CF_CLIENT | CF_ARCHIVE, "r_glsl_offsetmapping_lod_distance", "32", "first LOD level distance, second level (-50% steps) is 2x of this, third (33%) - 3x etc."};
cvar_t r_glsl_postprocess = {CF_CLIENT | CF_ARCHIVE, "r_glsl_postprocess", "0", "use a GLSL postprocessing shader"};
cvar_t r_glsl_postprocess_uservec1 = {CF_CLIENT | CF_ARCHIVE, "r_glsl_postprocess_uservec1", "0 0 0 0", "a 4-component vector to pass as uservec1 to the postprocessing shader (only useful if default.glsl has been customized)"};
cvar_t r_glsl_postprocess_uservec2 = {CF_CLIENT | CF_ARCHIVE, "r_glsl_postprocess_uservec2", "0 0 0 0", "a 4-component vector to pass as uservec2 to the postprocessing shader (only useful if default.glsl has been customized)"};
cvar_t r_glsl_postprocess_uservec3 = {CF_CLIENT | CF_ARCHIVE, "r_glsl_postprocess_uservec3", "0 0 0 0", "a 4-component vector to pass as uservec3 to the postprocessing shader (only useful if default.glsl has been customized)"};
cvar_t r_glsl_postprocess_uservec4 = {CF_CLIENT | CF_ARCHIVE, "r_glsl_postprocess_uservec4", "0 0 0 0", "a 4-component vector to pass as uservec4 to the postprocessing shader (only useful if default.glsl has been customized)"};
cvar_t r_glsl_postprocess_uservec1_enable = {CF_CLIENT | CF_ARCHIVE, "r_glsl_postprocess_uservec1_enable", "1", "enables postprocessing uservec1 usage, creates USERVEC1 define (only useful if default.glsl has been customized)"};
cvar_t r_glsl_postprocess_uservec2_enable = {CF_CLIENT | CF_ARCHIVE, "r_glsl_postprocess_uservec2_enable", "1", "enables postprocessing uservec2 usage, creates USERVEC1 define (only useful if default.glsl has been customized)"};
cvar_t r_glsl_postprocess_uservec3_enable = {CF_CLIENT | CF_ARCHIVE, "r_glsl_postprocess_uservec3_enable", "1", "enables postprocessing uservec3 usage, creates USERVEC1 define (only useful if default.glsl has been customized)"};
cvar_t r_glsl_postprocess_uservec4_enable = {CF_CLIENT | CF_ARCHIVE, "r_glsl_postprocess_uservec4_enable", "1", "enables postprocessing uservec4 usage, creates USERVEC1 define (only useful if default.glsl has been customized)"};
cvar_t r_colorfringe = {CF_CLIENT | CF_ARCHIVE, "r_colorfringe", "0", "Chromatic aberration. Values higher than 0.025 will noticeably distort the image"};
cvar_t r_fxaa = {CF_CLIENT | CF_ARCHIVE, "r_fxaa", "0", "fast approximate anti aliasing"};

cvar_t r_water = {CF_CLIENT | CF_ARCHIVE, "r_water", "0", "whether to use reflections and refraction on water surfaces (note: r_wateralpha must be set below 1)"};
cvar_t r_water_cameraentitiesonly = {CF_CLIENT | CF_ARCHIVE, "r_water_cameraentitiesonly", "0", "whether to only show QC-defined reflections/refractions (typically used for camera- or portal-like effects)"};
cvar_t r_water_clippingplanebias = {CF_CLIENT | CF_ARCHIVE, "r_water_clippingplanebias", "1", "a rather technical setting which avoids black pixels around water edges"};
cvar_t r_water_resolutionmultiplier = {CF_CLIENT | CF_ARCHIVE, "r_water_resolutionmultiplier", "0.5", "multiplier for screen resolution when rendering refracted/reflected scenes, 1 is full quality, lower values are faster"};
cvar_t r_water_refractdistort = {CF_CLIENT | CF_ARCHIVE, "r_water_refractdistort", "0.01", "how much water refractions shimmer"};
cvar_t r_water_reflectdistort = {CF_CLIENT | CF_ARCHIVE, "r_water_reflectdistort", "0.01", "how much water reflections shimmer"};
cvar_t r_water_scissormode = {CF_CLIENT, "r_water_scissormode", "3", "scissor (1) or cull (2) or both (3) water renders"};
cvar_t r_water_lowquality = {CF_CLIENT, "r_water_lowquality", "0", "special option to accelerate water rendering: 1 disables all dynamic lights, 2 disables particles too"};
cvar_t r_water_hideplayer = {CF_CLIENT | CF_ARCHIVE, "r_water_hideplayer", "0", "if set to 1 then player will be hidden in refraction views, if set to 2 then player will also be hidden in reflection views, player is always visible in camera views"};

cvar_t r_lerpsprites = {CF_CLIENT | CF_ARCHIVE, "r_lerpsprites", "0", "enables animation smoothing on sprites"};
cvar_t r_lerpmodels = {CF_CLIENT | CF_ARCHIVE, "r_lerpmodels", "1", "enables animation smoothing on models"};
cvar_t r_nolerp_list = {CF_CLIENT | CF_ARCHIVE, "r_nolerp_list", "progs/v_nail.mdl,progs/v_nail2.mdl,progs/flame.mdl,progs/flame2.mdl,progs/braztall.mdl,progs/brazshrt.mdl,progs/longtrch.mdl,progs/flame_pyre.mdl,progs/v_saw.mdl,progs/v_xfist.mdl,progs/h2stuff/newfire.mdl", "comma separated list of models that will not have their animations smoothed"};
cvar_t r_lerplightstyles = {CF_CLIENT | CF_ARCHIVE, "r_lerplightstyles", "0", "enable animation smoothing on flickering lights"};
cvar_t r_waterscroll = {CF_CLIENT | CF_ARCHIVE, "r_waterscroll", "1", "makes water scroll around, value controls how much"};

cvar_t r_bloom = {CF_CLIENT | CF_ARCHIVE, "r_bloom", "0", "enables bloom effect (makes bright pixels affect neighboring pixels)"};
cvar_t r_bloom_colorscale = {CF_CLIENT | CF_ARCHIVE, "r_bloom_colorscale", "1", "how bright the glow is"};

cvar_t r_bloom_brighten = {CF_CLIENT | CF_ARCHIVE, "r_bloom_brighten", "1", "how bright the glow is, after subtract/power"};
cvar_t r_bloom_blur = {CF_CLIENT | CF_ARCHIVE, "r_bloom_blur", "4", "how large the glow is"};
cvar_t r_bloom_resolution = {CF_CLIENT | CF_ARCHIVE, "r_bloom_resolution", "320", "what resolution to perform the bloom effect at (independent of screen resolution)"};
cvar_t r_bloom_colorexponent = {CF_CLIENT | CF_ARCHIVE, "r_bloom_colorexponent", "1", "how exaggerated the glow is"};

int R_ViewFBO(void)
{
	return m5_stock.integer ? 0 : r_viewfbo.integer;
}
qbool R_Bloom_Wanted(void)
{
	return r_bloom.integer && !m5_stock.integer;
}
cvar_t r_bloom_colorsubtract = {CF_CLIENT | CF_ARCHIVE, "r_bloom_colorsubtract", "0.1", "reduces bloom colors by a certain amount"};
cvar_t r_bloom_scenebrightness = {CF_CLIENT | CF_ARCHIVE, "r_bloom_scenebrightness", "1", "global rendering brightness when bloom is enabled"};
// BEAUTY A1 (2026-09-16): a modern HDR bloom in place of the 2001 blur. The
// old chain downscales the scene to r_bloom_resolution, squares it
// colorexponent times and box-blurs it with offset quads -- no threshold on
// the float scene buffer and no mip chain, so every bright thing becomes a
// wide haze rather than a hot core with a tight glow. This one: a soft
// threshold with a quadratic knee on the scene at half resolution, a
// downsample chain (the 13-tap box), an additive tent upsample back up it,
// and the EXISTING composite untouched. r_bloom stays the master; at
// r_bloom_m5 0 R_Bloom_MakeTexture is the old text byte for byte and the
// old six knobs mean what they did; at 1 those six are ignored (the
// composite's subtract is forced to 0) and these four shape it.
cvar_t r_bloom_m5 = {CF_CLIENT | CF_ARCHIVE, "r_bloom_m5", "0", "BEAUTY A1 (2026-09-16): 1 = the modern HDR bloom -- a soft threshold on the float scene buffer (r_viewfbo 2, which r_edr and r_metalfx 2 force), a five-level downsample chain and a tent upsample, so a muzzle flash, a lava pool or a bolt core gets a hot core with a tight glow instead of the 2001 blur's wide haze. Needs r_bloom 1 (the master and its menu row); r_bloom_resolution, _colorexponent, _colorscale, _colorsubtract, _brighten and _blur are the OLD chain's and are ignored while this is on -- r_bloom_m5_threshold, _knee, _intensity and _levels shape this one. Costs about the same as the old chain. 0 = the 2001 blur exactly. exec bloom_m5_on.cfg / bloom_m5_off.cfg"};
cvar_t r_bloom_m5_threshold = {CF_CLIENT | CF_ARCHIVE, "r_bloom_m5_threshold", "1", "r_bloom_m5: scene brightness (brightest channel) above which a pixel blooms. 1 = only what is brighter than white -- which on an 8-bit scene buffer (r_viewfbo 0) is nothing, so there this wants ~0.6; on the float buffer the flash, the lava and the bolt sit above it"};
cvar_t r_bloom_m5_knee = {CF_CLIENT | CF_ARCHIVE, "r_bloom_m5_knee", "0.5", "r_bloom_m5: how softly the threshold engages, in the same units -- pixels from threshold-knee up to threshold+knee bloom partially along a quadratic, so a torch at 0.7 gives a little and a flash at 3 gives everything. 0 = a hard cut"};
cvar_t r_bloom_m5_intensity = {CF_CLIENT | CF_ARCHIVE, "r_bloom_m5_intensity", "0.35", "r_bloom_m5: how much of the blurred bright light is added back to the picture. Linear over the whole chain; the composite clamps the add at white per channel"};
cvar_t r_bloom_m5_levels = {CF_CLIENT | CF_ARCHIVE, "r_bloom_m5_levels", "5", "r_bloom_m5: how many halvings the chain descends (1-8). Each level widens the glow by about a factor of two: 3 is a tight halo, 5 a soft one that reaches across a room, 7 a haze. Levels smaller than 2x2 are skipped"};

cvar_t r_hdr_scenebrightness = {CF_CLIENT | CF_ARCHIVE, "r_hdr_scenebrightness", "1", "global rendering brightness"};
cvar_t r_hdr_glowintensity = {CF_CLIENT | CF_ARCHIVE, "r_hdr_glowintensity", "1", "how bright light emitting textures should appear"};
cvar_t r_hdr_irisadaptation = {CF_CLIENT | CF_ARCHIVE, "r_hdr_irisadaptation", "0", "adjust scene brightness according to light intensity at player location"};
cvar_t r_hdr_irisadaptation_multiplier = {CF_CLIENT | CF_ARCHIVE, "r_hdr_irisadaptation_multiplier", "2", "brightness at which value will be 1.0"};
cvar_t r_hdr_irisadaptation_minvalue = {CF_CLIENT | CF_ARCHIVE, "r_hdr_irisadaptation_minvalue", "0.5", "minimum value that can result from multiplier / brightness"};
cvar_t r_hdr_irisadaptation_maxvalue = {CF_CLIENT | CF_ARCHIVE, "r_hdr_irisadaptation_maxvalue", "4", "maximum value that can result from multiplier / brightness"};
cvar_t r_hdr_irisadaptation_value = {CF_CLIENT, "r_hdr_irisadaptation_value", "1", "current value as scenebrightness multiplier, changes continuously when irisadaptation is active"};
cvar_t r_hdr_irisadaptation_fade_up = {CF_CLIENT | CF_ARCHIVE, "r_hdr_irisadaptation_fade_up", "0.1", "fade rate at which value adjusts to darkness"};
cvar_t r_hdr_irisadaptation_fade_down = {CF_CLIENT | CF_ARCHIVE, "r_hdr_irisadaptation_fade_down", "0.5", "fade rate at which value adjusts to brightness"};
cvar_t r_hdr_irisadaptation_radius = {CF_CLIENT | CF_ARCHIVE, "r_hdr_irisadaptation_radius", "15", "lighting within this many units of the eye is averaged"};

cvar_t r_smoothnormals_areaweighting = {CF_CLIENT, "r_smoothnormals_areaweighting", "1", "uses significantly faster (and supposedly higher quality) area-weighted vertex normals and tangent vectors rather than summing normalized triangle normals and tangents"};

cvar_t developer_texturelogging = {CF_CLIENT, "developer_texturelogging", "0", "produces a textures.log file containing names of skins and map textures the engine tried to load"};

cvar_t gl_lightmaps = {CF_CLIENT, "gl_lightmaps", "0", "draws only lightmaps, no texture (for level designers), a value of 2 keeps normalmap shading"};

cvar_t r_test = {CF_CLIENT, "r_test", "0", "internal development use only, leave it alone (usually does nothing anyway)"};

cvar_t r_batch_multidraw = {CF_CLIENT | CF_ARCHIVE, "r_batch_multidraw", "1", "issue multiple glDrawElements calls when rendering a batch of surfaces with the same texture (otherwise the index data is copied to make it one draw)"};
cvar_t r_batch_multidraw_mintriangles = {CF_CLIENT | CF_ARCHIVE, "r_batch_multidraw_mintriangles", "0", "minimum number of triangles to activate multidraw path (copying small groups of triangles may be faster)"};
cvar_t r_batch_debugdynamicvertexpath = {CF_CLIENT | CF_ARCHIVE, "r_batch_debugdynamicvertexpath", "0", "force the dynamic batching code path for debugging purposes"};
cvar_t r_batch_dynamicbuffer = {CF_CLIENT | CF_ARCHIVE, "r_batch_dynamicbuffer", "0", "use vertex/index buffers for drawing dynamic and copytriangles batches"};

cvar_t r_glsl_saturation = {CF_CLIENT | CF_ARCHIVE, "r_glsl_saturation", "1", "saturation multiplier (only working in glsl!)"};
cvar_t r_glsl_saturation_redcompensate = {CF_CLIENT | CF_ARCHIVE, "r_glsl_saturation_redcompensate", "0", "a 'vampire sight' addition to desaturation effect, does compensation for red color, r_glsl_restart is required"};

cvar_t r_glsl_vertextextureblend_usebothalphas = {CF_CLIENT | CF_ARCHIVE, "r_glsl_vertextextureblend_usebothalphas", "0", "use both alpha layers on vertex blended surfaces, each alpha layer sets amount of 'blend leak' on another layer, requires mod_q3shader_force_terrain_alphaflag on."};

// FIXME: This cvar would grow to a ridiculous size after several launches and clean exits when used during surface sorting.
cvar_t r_framedatasize = {CF_CLIENT | CF_ARCHIVE, "r_framedatasize", "0.5", "size of renderer data cache used during one frame (for skeletal animation caching, light processing, etc)"};
cvar_t r_buffermegs[R_BUFFERDATA_COUNT] =
{
	{CF_CLIENT | CF_ARCHIVE, "r_buffermegs_vertex", "4", "vertex buffer size for one frame"},
	{CF_CLIENT | CF_ARCHIVE, "r_buffermegs_index16", "1", "index buffer size for one frame (16bit indices)"},
	{CF_CLIENT | CF_ARCHIVE, "r_buffermegs_index32", "1", "index buffer size for one frame (32bit indices)"},
	{CF_CLIENT | CF_ARCHIVE, "r_buffermegs_uniform", "0.25", "uniform buffer size for one frame"},
};

cvar_t r_q1bsp_lightmap_updates_enabled = {CF_CLIENT, "r_q1bsp_lightmap_updates_enabled", "1", "allow lightmaps to be updated on Q1BSP maps (don't turn this off except for debugging)"};
cvar_t r_q1bsp_lightmap_updates_combine = {CF_CLIENT | CF_ARCHIVE, "r_q1bsp_lightmap_updates_combine", "2", "combine lightmap texture updates to make fewer glTexSubImage2D calls, modes: 0 = immediately upload lightmaps (may be thousands of small 3x3 updates), 1 = combine to one call, 2 = combine to one full texture update (glTexImage2D) which tells the driver it does not need to lock the resource (faster on most drivers)"};
cvar_t r_q1bsp_lightmap_updates_hidden_surfaces = {CF_CLIENT | CF_ARCHIVE, "r_q1bsp_lightmap_updates_hidden_surfaces", "0", "update lightmaps on surfaces that are not visible, so that updates only occur on frames where lightstyles changed value (animation or light switches), only makes sense with combine = 2"};

extern cvar_t v_glslgamma_2d;

extern qbool v_flipped_state;

r_framebufferstate_t r_fb;

/// shadow volume bsp struct with automatically growing nodes buffer
svbsp_t r_svbsp;

int r_uniformbufferalignment = 32; // dynamically updated to match GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT

rtexture_t *r_texture_blanknormalmap;
rtexture_t *r_texture_white;
rtexture_t *r_texture_white3d;
rtexture_t *r_texture_grey128;
rtexture_t *r_texture_black;
rtexture_t *r_texture_notexture;
rtexture_t *r_texture_whitecube;
rtexture_t *r_texture_normalizationcube;
rtexture_t *r_texture_fogattenuation;
rtexture_t *r_texture_fogheighttexture;
rtexture_t *r_texture_gammaramps;
unsigned int r_texture_gammaramps_serial;
//rtexture_t *r_texture_fogintensity;
rtexture_t *r_texture_reflectcube;

// TODO: hash lookups?
typedef struct cubemapinfo_s
{
	char basename[64];
	rtexture_t *texture;
}
cubemapinfo_t;

int r_texture_numcubemaps;
cubemapinfo_t *r_texture_cubemaps[MAX_CUBEMAPS];

unsigned int r_queries[MAX_OCCLUSION_QUERIES];
unsigned int r_numqueries;
unsigned int r_maxqueries;

typedef struct r_qwskincache_s
{
	char name[MAX_QPATH];
	skinframe_t *skinframe;
}
r_qwskincache_t;

static r_qwskincache_t *r_qwskincache;
static int r_qwskincache_size;

/// vertex coordinates for a quad that covers the screen exactly
extern const float r_screenvertex3f[12];
const float r_screenvertex3f[12] =
{
	0, 0, 0,
	1, 0, 0,
	1, 1, 0,
	0, 1, 0
};

void R_ModulateColors(float *in, float *out, int verts, float r, float g, float b)
{
	int i;
	for (i = 0;i < verts;i++)
	{
		out[0] = in[0] * r;
		out[1] = in[1] * g;
		out[2] = in[2] * b;
		out[3] = in[3];
		in += 4;
		out += 4;
	}
}

void R_FillColors(float *out, int verts, float r, float g, float b, float a)
{
	int i;
	for (i = 0;i < verts;i++)
	{
		out[0] = r;
		out[1] = g;
		out[2] = b;
		out[3] = a;
		out += 4;
	}
}

// FIXME: move this to client?
void FOG_clear(void)
{
	if (gamemode == GAME_NEHAHRA)
	{
		Cvar_SetQuick(&gl_fogenable, "0");
		Cvar_SetQuick(&gl_fogdensity, "0.2");
		Cvar_SetQuick(&gl_fogred, "0.3");
		Cvar_SetQuick(&gl_foggreen, "0.3");
		Cvar_SetQuick(&gl_fogblue, "0.3");
	}
	r_refdef.fog_density = 0;
	r_refdef.fog_red = 0;
	r_refdef.fog_green = 0;
	r_refdef.fog_blue = 0;
	r_refdef.fog_alpha = 1;
	r_refdef.fog_start = 0;
	r_refdef.fog_end = 16384;
	r_refdef.fog_height = 1<<30;
	r_refdef.fog_fadedepth = 128;
	memset(r_refdef.fog_height_texturename, 0, sizeof(r_refdef.fog_height_texturename));
}

static void R_BuildBlankTextures(void)
{
	unsigned char data[4];
	data[2] = 128; // normal X
	data[1] = 128; // normal Y
	data[0] = 255; // normal Z
	data[3] = 255; // height
	r_texture_blanknormalmap = R_LoadTexture2D(r_main_texturepool, "blankbump", 1, 1, data, TEXTYPE_BGRA, TEXF_PERSISTENT, -1, NULL);
	data[0] = 255;
	data[1] = 255;
	data[2] = 255;
	data[3] = 255;
	r_texture_white = R_LoadTexture2D(r_main_texturepool, "blankwhite", 1, 1, data, TEXTYPE_BGRA, TEXF_PERSISTENT, -1, NULL);
	// the 3D twin, for slots the shader declares as a VOLUME. Binding the 2D
	// white where MSL expects texture3d is a Metal validation ABORT (GL merely
	// tolerates it) -- run J4 caught exactly that on the menu frames when the
	// per-map noise bake started returning NULL with no world loaded. 2x2x2,
	// not 1x1x1: Metal_Texture_Create types a texture 3D only when depth > 1,
	// so a one-deep "volume" silently comes back MTLTextureType2D and aborts
	// identically -- measured, not guessed.
	{
		unsigned char white3d[2 * 2 * 2 * 4];
		memset(white3d, 255, sizeof(white3d));
		r_texture_white3d = R_LoadTexture3D(r_main_texturepool, "blankwhite3d", 2, 2, 2, white3d, TEXTYPE_BGRA, TEXF_PERSISTENT, -1, NULL);
	}
	data[0] = 128;
	data[1] = 128;
	data[2] = 128;
	data[3] = 255;
	r_texture_grey128 = R_LoadTexture2D(r_main_texturepool, "blankgrey128", 1, 1, data, TEXTYPE_BGRA, TEXF_PERSISTENT, -1, NULL);
	data[0] = 0;
	data[1] = 0;
	data[2] = 0;
	data[3] = 255;
	r_texture_black = R_LoadTexture2D(r_main_texturepool, "blankblack", 1, 1, data, TEXTYPE_BGRA, TEXF_PERSISTENT, -1, NULL);
}

static void R_BuildNoTexture(void)
{
	r_texture_notexture = R_LoadTexture2D(r_main_texturepool, "notexture", 16, 16, Image_GenerateNoTexture(), TEXTYPE_BGRA, TEXF_MIPMAP | TEXF_PERSISTENT, -1, NULL);
}

static void R_BuildWhiteCube(void)
{
	unsigned char data[6*1*1*4];
	memset(data, 255, sizeof(data));
	r_texture_whitecube = R_LoadTextureCubeMap(r_main_texturepool, "whitecube", 1, data, TEXTYPE_BGRA, TEXF_CLAMP | TEXF_PERSISTENT, -1, NULL);
}

static void R_BuildNormalizationCube(void)
{
	int x, y, side;
	vec3_t v;
	vec_t s, t, intensity;
#define NORMSIZE 64
	unsigned char *data;
	data = (unsigned char *)Mem_Alloc(tempmempool, 6*NORMSIZE*NORMSIZE*4);
	for (side = 0;side < 6;side++)
	{
		for (y = 0;y < NORMSIZE;y++)
		{
			for (x = 0;x < NORMSIZE;x++)
			{
				s = (x + 0.5f) * (2.0f / NORMSIZE) - 1.0f;
				t = (y + 0.5f) * (2.0f / NORMSIZE) - 1.0f;
				switch(side)
				{
				default:
				case 0:
					v[0] = 1;
					v[1] = -t;
					v[2] = -s;
					break;
				case 1:
					v[0] = -1;
					v[1] = -t;
					v[2] = s;
					break;
				case 2:
					v[0] = s;
					v[1] = 1;
					v[2] = t;
					break;
				case 3:
					v[0] = s;
					v[1] = -1;
					v[2] = -t;
					break;
				case 4:
					v[0] = s;
					v[1] = -t;
					v[2] = 1;
					break;
				case 5:
					v[0] = -s;
					v[1] = -t;
					v[2] = -1;
					break;
				}
				intensity = 127.0f / sqrt(DotProduct(v, v));
				data[((side*64+y)*64+x)*4+2] = (unsigned char)(128.0f + intensity * v[0]);
				data[((side*64+y)*64+x)*4+1] = (unsigned char)(128.0f + intensity * v[1]);
				data[((side*64+y)*64+x)*4+0] = (unsigned char)(128.0f + intensity * v[2]);
				data[((side*64+y)*64+x)*4+3] = 255;
			}
		}
	}
	r_texture_normalizationcube = R_LoadTextureCubeMap(r_main_texturepool, "normalcube", NORMSIZE, data, TEXTYPE_BGRA, TEXF_CLAMP | TEXF_PERSISTENT, -1, NULL);
	Mem_Free(data);
}

static void R_BuildFogTexture(void)
{
	int x, b;
#define FOGWIDTH 256
	unsigned char data1[FOGWIDTH][4];
	//unsigned char data2[FOGWIDTH][4];
	double d, r, alpha;

	r_refdef.fogmasktable_start = r_refdef.fog_start;
	r_refdef.fogmasktable_alpha = r_refdef.fog_alpha;
	r_refdef.fogmasktable_range = r_refdef.fogrange;
	r_refdef.fogmasktable_density = r_refdef.fog_density;

	r = r_refdef.fogmasktable_range / FOGMASKTABLEWIDTH;
	for (x = 0;x < FOGMASKTABLEWIDTH;x++)
	{
		d = (x * r - r_refdef.fogmasktable_start);
		if(developer_extra.integer)
			Con_DPrintf("%f ", d);
		d = max(0, d);
		if (r_fog_exp2.integer)
			alpha = exp(-r_refdef.fogmasktable_density * r_refdef.fogmasktable_density * 0.0001 * d * d);
		else
			alpha = exp(-r_refdef.fogmasktable_density * 0.004 * d);
		if(developer_extra.integer)
			Con_DPrintf(" : %f ", alpha);
		alpha = 1 - (1 - alpha) * r_refdef.fogmasktable_alpha;
		if(developer_extra.integer)
			Con_DPrintf(" = %f\n", alpha);
		r_refdef.fogmasktable[x] = bound(0, alpha, 1);
	}

	for (x = 0;x < FOGWIDTH;x++)
	{
		b = (int)(r_refdef.fogmasktable[x * (FOGMASKTABLEWIDTH - 1) / (FOGWIDTH - 1)] * 255);
		data1[x][0] = b;
		data1[x][1] = b;
		data1[x][2] = b;
		data1[x][3] = 255;
		//data2[x][0] = 255 - b;
		//data2[x][1] = 255 - b;
		//data2[x][2] = 255 - b;
		//data2[x][3] = 255;
	}
	if (r_texture_fogattenuation)
	{
		R_UpdateTexture(r_texture_fogattenuation, &data1[0][0], 0, 0, 0, FOGWIDTH, 1, 1, 0);
		//R_UpdateTexture(r_texture_fogattenuation, &data2[0][0], 0, 0, 0, FOGWIDTH, 1, 1, 0);
	}
	else
	{
		r_texture_fogattenuation = R_LoadTexture2D(r_main_texturepool, "fogattenuation", FOGWIDTH, 1, &data1[0][0], TEXTYPE_BGRA, TEXF_FORCELINEAR | TEXF_CLAMP | TEXF_PERSISTENT, -1, NULL);
		//r_texture_fogintensity = R_LoadTexture2D(r_main_texturepool, "fogintensity", FOGWIDTH, 1, &data2[0][0], TEXTYPE_BGRA, TEXF_FORCELINEAR | TEXF_CLAMP, NULL);
	}
}

static void R_BuildFogHeightTexture(void)
{
	unsigned char *inpixels;
	int size;
	int x;
	int y;
	int j;
	float c[4];
	float f;
	inpixels = NULL;
	dp_strlcpy(r_refdef.fogheighttexturename, r_refdef.fog_height_texturename, sizeof(r_refdef.fogheighttexturename));
	if (r_refdef.fogheighttexturename[0])
		inpixels = loadimagepixelsbgra(r_refdef.fogheighttexturename, true, false, false, NULL);
	if (!inpixels)
	{
		r_refdef.fog_height_tablesize = 0;
		if (r_texture_fogheighttexture)
			R_FreeTexture(r_texture_fogheighttexture);
		r_texture_fogheighttexture = NULL;
		if (r_refdef.fog_height_table2d)
			Mem_Free(r_refdef.fog_height_table2d);
		r_refdef.fog_height_table2d = NULL;
		if (r_refdef.fog_height_table1d)
			Mem_Free(r_refdef.fog_height_table1d);
		r_refdef.fog_height_table1d = NULL;
		return;
	}
	size = image_width;
	r_refdef.fog_height_tablesize = size;
	r_refdef.fog_height_table1d = (unsigned char *)Mem_Alloc(r_main_mempool, size * 4);
	r_refdef.fog_height_table2d = (unsigned char *)Mem_Alloc(r_main_mempool, size * size * 4);
	memcpy(r_refdef.fog_height_table1d, inpixels, size * 4);
	Mem_Free(inpixels);
	// LadyHavoc: now the magic - what is that table2d for?  it is a cooked
	// average fog color table accounting for every fog layer between a point
	// and the camera.  (Note: attenuation is handled separately!)
	for (y = 0;y < size;y++)
	{
		for (x = 0;x < size;x++)
		{
			Vector4Clear(c);
			f = 0;
			if (x < y)
			{
				for (j = x;j <= y;j++)
				{
					Vector4Add(c, r_refdef.fog_height_table1d + j*4, c);
					f++;
				}
			}
			else
			{
				for (j = x;j >= y;j--)
				{
					Vector4Add(c, r_refdef.fog_height_table1d + j*4, c);
					f++;
				}
			}
			f = 1.0f / f;
			r_refdef.fog_height_table2d[(y*size+x)*4+0] = (unsigned char)(c[0] * f);
			r_refdef.fog_height_table2d[(y*size+x)*4+1] = (unsigned char)(c[1] * f);
			r_refdef.fog_height_table2d[(y*size+x)*4+2] = (unsigned char)(c[2] * f);
			r_refdef.fog_height_table2d[(y*size+x)*4+3] = (unsigned char)(c[3] * f);
		}
	}
	r_texture_fogheighttexture = R_LoadTexture2D(r_main_texturepool, "fogheighttable", size, size, r_refdef.fog_height_table2d, TEXTYPE_BGRA, TEXF_ALPHA | TEXF_CLAMP, -1, NULL);
}

//=======================================================================================================================================================

static const char *builtinshaderstrings[] =
{
#include "shader_glsl.h"
0
};

//=======================================================================================================================================================

typedef struct shaderpermutationinfo_s
{
	const char *pretext;
	const char *name;
}
shaderpermutationinfo_t;

typedef struct shadermodeinfo_s
{
	const char *sourcebasename;
	const char *extension;
	const char **builtinshaderstrings;
	const char *pretext;
	const char *name;
	char *filename;
	char *builtinstring;
	int builtincrc;
}
shadermodeinfo_t;

// NOTE: MUST MATCH ORDER OF SHADERPERMUTATION_* DEFINES!
shaderpermutationinfo_t shaderpermutationinfo[SHADERPERMUTATION_COUNT] =
{
	{"#define USEDIFFUSE\n", " diffuse"},
	{"#define USEVERTEXTEXTUREBLEND\n", " vertextextureblend"},
	{"#define USEVIEWTINT\n", " viewtint"},
	{"#define USECOLORMAPPING\n", " colormapping"},
	{"#define USESATURATION\n", " saturation"},
	{"#define USEFOGINSIDE\n", " foginside"},
	{"#define USEFOGOUTSIDE\n", " fogoutside"},
	{"#define USEFOGHEIGHTTEXTURE\n", " fogheighttexture"},
	{"#define USEFOGALPHAHACK\n", " fogalphahack"},
	{"#define USEGAMMARAMPS\n", " gammaramps"},
	{"#define USECUBEFILTER\n", " cubefilter"},
	{"#define USEGLOW\n", " glow"},
	{"#define USEBLOOM\n", " bloom"},
	{"#define USESPECULAR\n", " specular"},
	{"#define USEPOSTPROCESSING\n", " postprocessing"},
	{"#define USEREFLECTION\n", " reflection"},
	{"#define USEOFFSETMAPPING\n", " offsetmapping"},
	{"#define USEOFFSETMAPPING_RELIEFMAPPING\n", " reliefmapping"},
	{"#define USESHADOWMAP2D\n", " shadowmap2d"},
	{"#define USESHADOWMAPVSDCT\n", " shadowmapvsdct"}, // TODO make this a static parm
	{"#define USESHADOWMAPORTHO\n", " shadowmaportho"},
	{"#define USEDEFERREDLIGHTMAP\n", " deferredlightmap"},
	{"#define USEALPHAKILL\n", " alphakill"},
	{"#define USEREFLECTCUBE\n", " reflectcube"},
	{"#define USENORMALMAPSCROLLBLEND\n", " normalmapscrollblend"},
	{"#define USEBOUNCEGRID\n", " bouncegrid"},
	{"#define USEBOUNCEGRIDDIRECTIONAL\n", " bouncegriddirectional"}, // TODO make this a static parm
	{"#define USETRIPPY\n", " trippy"},
	{"#define USEDEPTHRGB\n", " depthrgb"},
	{"#define USEALPHAGENVERTEX\n", " alphagenvertex"},
	{"#define USESKELETAL\n", " skeletal"},
	{"#define USEOCCLUDE\n", " occlude"}
};

// NOTE: MUST MATCH ORDER OF SHADERMODE_* ENUMS!
shadermodeinfo_t shadermodeinfo[SHADERLANGUAGE_COUNT][SHADERMODE_COUNT] =
{
	// SHADERLANGUAGE_GLSL
	{
		{"combined", "glsl", builtinshaderstrings, "#define MODE_GENERIC\n", " generic"},
		{"combined", "glsl", builtinshaderstrings, "#define MODE_POSTPROCESS\n", " postprocess"},
		{"combined", "glsl", builtinshaderstrings, "#define MODE_DEPTH_OR_SHADOW\n", " depth/shadow"},
		{"combined", "glsl", builtinshaderstrings, "#define MODE_FLATCOLOR\n", " flatcolor"},
		{"combined", "glsl", builtinshaderstrings, "#define MODE_VERTEXCOLOR\n", " vertexcolor"},
		{"combined", "glsl", builtinshaderstrings, "#define MODE_LIGHTMAP\n", " lightmap"},
		{"combined", "glsl", builtinshaderstrings, "#define MODE_LIGHTDIRECTIONMAP_MODELSPACE\n", " lightdirectionmap_modelspace"},
		{"combined", "glsl", builtinshaderstrings, "#define MODE_LIGHTDIRECTIONMAP_TANGENTSPACE\n", " lightdirectionmap_tangentspace"},
		{"combined", "glsl", builtinshaderstrings, "#define MODE_LIGHTDIRECTIONMAP_FORCED_LIGHTMAP\n", " lightdirectionmap_forced_lightmap"},
		{"combined", "glsl", builtinshaderstrings, "#define MODE_LIGHTDIRECTIONMAP_FORCED_VERTEXCOLOR\n", " lightdirectionmap_forced_vertexcolor"},
		{"combined", "glsl", builtinshaderstrings, "#define MODE_LIGHTGRID\n", " lightgrid"},
		{"combined", "glsl", builtinshaderstrings, "#define MODE_LIGHTDIRECTION\n", " lightdirection"},
		{"combined", "glsl", builtinshaderstrings, "#define MODE_LIGHTSOURCE\n", " lightsource"},
		{"combined", "glsl", builtinshaderstrings, "#define MODE_REFRACTION\n", " refraction"},
		{"combined", "glsl", builtinshaderstrings, "#define MODE_WATER\n", " water"},
		{"combined", "glsl", builtinshaderstrings, "#define MODE_DEFERREDGEOMETRY\n", " deferredgeometry"},
		{"combined", "glsl", builtinshaderstrings, "#define MODE_DEFERREDLIGHTSOURCE\n", " deferredlightsource"},
		{"combined", "glsl", builtinshaderstrings, "#define MODE_VOLUMETRICFOG\n", " volumetricfog"},
		// METAL.md Phase 5 slice 4. The RT composite's own mode, and it is
		// METAL-ONLY BY CONSTRUCTION: the GL path composites through the
		// sidecar's private GLSL program (rt_metal.m's kCompVS/kCompFS) and
		// never asks for this mode, so shader_glsl.h has no MODE_RTCOMPOSITE
		// arm and never compiles one. The row exists because the tables are
		// indexed by the mode enum and must stay the same length; r_glsl_dumpshader
		// walks it, but that only writes files and dedupes by filename.
		{"combined", "glsl", builtinshaderstrings, "#define MODE_RTCOMPOSITE\n", " rtcomposite"},
		// F1. The thunderbolt's capsule-SDF accumulation pass. Metal only, but
		// unlike RTCOMPOSITE above shader_glsl.h DOES carry an arm for it -- a
		// magenta sentinel, as a peer of MODE_POSTPROCESS in that chain. The
		// selection predicate is a capability (vid.blendequationmax) rather than
		// a renderpath test, so "GL can never ask" is a property of a runtime
		// flag rather than of the source, and a sentinel is what makes a mistake
		// there loud instead of silently landing in the shared surface main().
		{"combined", "glsl", builtinshaderstrings, "#define MODE_M5BOLT\n", " m5bolt"},
		// The MetalFX-temporal arc's camera-only motion-vector fill. Metal only,
		// with a magenta sentinel on the GL side for the same reason M5BOLT has
		// one: the selection is a runtime predicate, not a #ifdef.
		{"combined", "glsl", builtinshaderstrings, "#define MODE_MOTIONFILL\n", " motionfill"},
		// T2b: per-entity motion vectors over the fill. Same Metal-only shape.
		{"combined", "glsl", builtinshaderstrings, "#define MODE_MOTIONVECTOR\n", " motionvector"},
		// The particles' footprint stamped into the reactive mask. Same shape.
		{"combined", "glsl", builtinshaderstrings, "#define MODE_REACTIVESTAMP\n", " reactivestamp"},
		// BEAUTY A1 (2026-09-16): the modern bloom's passes. Both backends, one
		// call site, a literal permutation 0 -- the RTCOMPOSITE shape. The name is
		// the one shader_glsl.h carried DEAD since 2001 (a MODE_BLOOMBLUR arm with
		// no enum entry, and a BloomBlur_Parameters uniform gl_rmain.c already
		// looked up); that text is replaced by the live arm.
		{"combined", "glsl", builtinshaderstrings, "#define MODE_BLOOMBLUR\n", " bloomblur"},
		// 2026-09-18: FXAA at NATIVE resolution, after the MetalFX upscale. The
		// postprocess (FXAA included) runs at RENDER resolution -- its target is
		// sized from r_fb.rt_screen -- so the scaler magnifies an already-
		// antialiased frame and FXAA never sees the staircase the magnification
		// creates. Metal only (MetalFX is), magenta sentinel on GL for the reason
		// M5BOLT and MOTIONFILL have one.
		{"combined", "glsl", builtinshaderstrings, "#define MODE_FXAAPOST\n", " fxaapost"},
		// 2026-09-18, SMAA.md: analytic MLAA at NATIVE resolution, three passes.
		// FXAA is structurally exhausted on a shallow silhouette -- it corrects at
		// most half a pixel near a step's ENDS, so on the 6-18 px steps Seb reported
		// four pixels in six get nothing, and raising its span past ~24 makes every
		// slope worse because the taps land on unrelated content. A morphological
		// pass finds each step's WHOLE extent and blends every pixel in it by that
		// pixel's own coverage. Metal only with magenta sentinels on GL, the
		// MODE_FXAAPOST shape.
		{"combined", "glsl", builtinshaderstrings, "#define MODE_SMAAEDGES\n", " smaaedges"},
		{"combined", "glsl", builtinshaderstrings, "#define MODE_SMAAWEIGHTS\n", " smaaweights"},
		{"combined", "glsl", builtinshaderstrings, "#define MODE_SMAABLEND\n", " smaablend"},
	},
};

struct r_glsl_permutation_s;
typedef struct r_glsl_permutation_s
{
	/// hash lookup data
	struct r_glsl_permutation_s *hashnext;
	unsigned int mode;
	uint64_t permutation;

	/// indicates if we have tried compiling this permutation already
	qbool compiled;
	/// 0 if compilation failed
	int program;
	// texture units assigned to each detected uniform
	int tex_Texture_First;
	int tex_Texture_Second;
	int tex_Texture_GammaRamps;
	int tex_Texture_Normal;
	int tex_Texture_Color;
	int tex_Texture_Gloss;
	int tex_Texture_Glow;
	int tex_Texture_SecondaryNormal;
	int tex_Texture_SecondaryColor;
	int tex_Texture_SecondaryGloss;
	int tex_Texture_SecondaryGlow;
	int tex_Texture_Pants;
	int tex_Texture_Shirt;
	int tex_Texture_FogHeightTexture;
	int tex_Texture_FogMask;
	int tex_Texture_LightGrid;
	int tex_Texture_Lightmap;
	int tex_Texture_Deluxemap;
	int tex_Texture_Attenuation;
	int tex_Texture_Cube;
	int tex_Texture_Refraction;
	int tex_Texture_Reflection;
	int tex_Texture_ShadowMap2D;
	int tex_Texture_CubeProjection;
	int tex_Texture_ScreenNormalMap;
	int tex_Texture_ScreenDepth;
	int tex_Texture_VolumeNoise;
	int tex_Texture_VolumeField;
	int tex_Texture_VolumeIrr;
	int tex_Texture_Shafts;
	int tex_Texture_RTTerm;
	int tex_Texture_RTLiquid;   // SEPTEMBER2 C2
	int tex_Texture_WaterScreen;   // WATERSURFACE: the frame copy, the index after RTLiquid in the walk
	int tex_Texture_KernelFog;
	int tex_Texture_ScreenDiffuse;
	int tex_Texture_ScreenSpecular;
	int tex_Texture_ReflectMask;
	int tex_Texture_ReflectCube;
	int tex_Texture_BounceGrid;
	/// locations of detected uniforms in program object, or -1 if not found
	int loc_Texture_First;
	int loc_Texture_Second;
	int loc_Texture_GammaRamps;
	int loc_Texture_Normal;
	int loc_Texture_Color;
	int loc_Texture_Gloss;
	int loc_Texture_Glow;
	int loc_Texture_SecondaryNormal;
	int loc_Texture_SecondaryColor;
	int loc_Texture_SecondaryGloss;
	int loc_Texture_SecondaryGlow;
	int loc_Texture_Pants;
	int loc_Texture_Shirt;
	int loc_Texture_FogHeightTexture;
	int loc_Texture_FogMask;
	int loc_Texture_LightGrid;
	int loc_Texture_Lightmap;
	int loc_Texture_Deluxemap;
	int loc_Texture_Attenuation;
	int loc_Texture_Cube;
	int loc_Texture_Refraction;
	int loc_Texture_Reflection;
	int loc_Texture_ShadowMap2D;
	int loc_Texture_CubeProjection;
	int loc_Texture_ScreenNormalMap;
	int loc_Texture_ScreenDepth;
	int loc_Texture_VolumeNoise;
	int loc_Texture_VolumeField;
	int loc_Texture_VolumeIrr;
	int loc_Texture_Shafts;
	int loc_Texture_RTTerm;
	int loc_Texture_RTLiquid;   // SEPTEMBER2 C2: the liquid pair (own term | reflection), the index after RTTerm
	int loc_Texture_WaterScreen;   // WATERSURFACE
	int loc_Texture_KernelFog;
	int loc_Texture_ScreenDiffuse;
	int loc_Texture_ScreenSpecular;
	int loc_Texture_ReflectMask;
	int loc_Texture_ReflectCube;
	int loc_Texture_BounceGrid;
	int loc_Alpha;
	int loc_SoftParticle;      // BEAUTY A4: (1/fade, viewport x, viewport y, 0)
	int loc_SoftParticleTex;   // BEAUTY A4: (1/depthtex w, 1/depthtex h, ScreenToDepth.xy)
	int loc_PartRefract;       // BEAUTY A5: (strength px, cell centre s, cell centre t, 2/cell width)
	int loc_PartRefractTex;    // BEAUTY A5: (1/copy w, 1/copy h, viewport x, viewport y)
	int loc_BloomBlur_Parameters;
	int loc_ClientTime;
	int loc_Color_Ambient;
	int loc_Color_Diffuse;
	int loc_Color_Specular;
	int loc_Color_Glow;
	int loc_Color_Pants;
	int loc_Color_Shirt;
	int loc_DeferredColor_Ambient;
	int loc_DeferredColor_Diffuse;
	int loc_DeferredColor_Specular;
	int loc_DeferredMod_Diffuse;
	int loc_DeferredMod_Specular;
	int loc_DistortScaleRefractReflect;
	int loc_EyePosition;
	int loc_FogColor;
	int loc_FogHeightFade;
	int loc_FogPlane;
	int loc_FogPlaneViewDist;
	int loc_FogRangeRecip;
	int loc_LightColor;
	int loc_LightDir;
	int loc_LightGridMatrix;
	int loc_LightGridNormalMatrix;
	int loc_LightPosition;
	int loc_OffsetMapping_ScaleSteps;
	int loc_OffsetMapping_LodDistance;
	int loc_OffsetMapping_Bias;
	int loc_PixelSize;
	int loc_FxaaSpan;
	int loc_ReflectColor;
	int loc_ReflectFactor;
	int loc_ReflectOffset;
	int loc_RefractColor;
	int loc_Saturation;
	int loc_ScreenCenterRefractReflect;
	int loc_ScreenScaleRefractReflect;
	int loc_ScreenToDepth;
	int loc_ShadowMap_Parameters;
	int loc_ShadowMap_TextureScale;
	int loc_SpecularPower;
	int loc_Skeletal_Transform12;
	int loc_UserVec1;
	int loc_UserVec2;
	int loc_UserVec3;
	int loc_UserVec4;
	int loc_ColorFringe;
	int loc_HdrShoulder;
	int loc_BallPressure;
	int loc_VolumetricDebugMode;
	int loc_ViewToWorld;
	int loc_FrustumScale;
	int loc_VolumetricFarClip;
	int loc_VolumetricOrigin;
	int loc_VolumetricParams;
	int loc_VolumetricNoise;
	int loc_VolumetricWind;
	int loc_VolumetricColor;
	int loc_VolumetricFieldOrigin;
	int loc_VolumetricFieldParams;
	int loc_VolumetricLiquid;
	int loc_VolumetricMist;
	int loc_VolumetricMistLavaCut;
	int loc_VolumetricLiquidFloor;
	int loc_VolumetricLiquidDens;
	int loc_VolumetricShafts;
	int loc_VolumetricReproj0;
	int loc_VolumetricReproj1;
	int loc_VolumetricReproj2;
	int loc_RTLiquid;
	int loc_WaterScreen;   // WATERSURFACE
	int loc_WaterScreenSize;
	int loc_WaterScreenTime;
	int loc_WaterScreenLook;
	int loc_WaterScreenTint;
	int loc_LavaParams;
	int loc_WaterParams;
	int loc_BoltFizz;
	int loc_BoltEye;
	int loc_BoltClip;
	int loc_LavaShimmer;
	int loc_LavaShimmer2;
	int loc_RedGlow;
	int loc_RTLiquidScale;
	int loc_RTLiquidOwn;
	int loc_RTLiquidRT;         // SEPTEMBER2 C2: x = the pair is live, y = half its width in texels
	int loc_ModelToWorld;
	int loc_LiquidFade;
	int loc_LiquidFadeAir;
	int loc_LiquidFadeGround;
	int loc_LiquidFadeLiquid;
	int loc_CausticParams;   // BEAUTY C2: (strength, scale, time, 0), 0 on every batch but a world opaque surface
	int loc_LiquidFadeOrigin;
	int loc_LiquidFadeFieldOrigin;
	int loc_LiquidFadeFieldScale;
	int loc_VolumetricKernelFog;
	int loc_VolumetricUpsample;
	int loc_VolumetricUpsample2;
	int loc_GammaAnalyticA;
	int loc_GammaAnalyticB;
	int loc_GammaAnalyticC;
	int loc_VolumetricWaterColor;
	int loc_VolumetricSlimeColor;
	int loc_VolumetricLavaColor;
	int loc_VolumetricGround;
	int loc_VolumetricGround2;
	int loc_VolumetricIrr;
	int loc_VolumetricIrrOrigin;
	int loc_VolumetricIrrInvSize;
	int loc_VolumetricSwirl;
	int loc_MarchLights;
	int loc_MarchLightParams;
	int loc_VolumetricGroundWind;
	int loc_VolumetricGroundColor;
	int loc_FogSurfaceAmount;
	int loc_ViewTintColor;
	int loc_ViewToLight;
	int loc_ModelToLight;
	int loc_TexMatrix;
	int loc_BackgroundTexMatrix;
	int loc_ModelViewProjectionMatrix;
	int loc_ModelViewMatrix;
	int loc_PixelToScreenTexCoord;
	int loc_ModelToReflectCube;
	int loc_ShadowMapMatrix;
	int loc_BloomColorSubtract;
	int loc_NormalmapScrollBlend;
	int loc_BounceGridMatrix;
	int loc_BounceGridIntensity;
	/// uniform block bindings
	int ubibind_Skeletal_Transform12_UniformBlock;
	/// uniform block indices
	int ubiloc_Skeletal_Transform12_UniformBlock;
}
r_glsl_permutation_t;

#define SHADERPERMUTATION_HASHSIZE 256


// non-degradable "lightweight" shader parameters to keep the permutations simpler
// these can NOT degrade! only use for simple stuff
enum
{
	SHADERSTATICPARM_SATURATION_REDCOMPENSATE = 0, ///< red compensation filter for saturation
	SHADERSTATICPARM_EXACTSPECULARMATH = 1, ///< (lightsource or deluxemapping) use exact reflection map for specular effects, as opposed to the usual OpenGL approximation
	SHADERSTATICPARM_POSTPROCESS_USERVEC1 = 2, ///< postprocess uservec1 is enabled
	SHADERSTATICPARM_POSTPROCESS_USERVEC2 = 3, ///< postprocess uservec2 is enabled
	SHADERSTATICPARM_POSTPROCESS_USERVEC3 = 4, ///< postprocess uservec3 is enabled
	SHADERSTATICPARM_POSTPROCESS_USERVEC4 = 5,  ///< postprocess uservec4 is enabled
	SHADERSTATICPARM_VERTEXTEXTUREBLEND_USEBOTHALPHAS = 6, // use both alpha layers while blending materials, allows more advanced microblending
	SHADERSTATICPARM_OFFSETMAPPING_USELOD = 7,  ///< LOD for offsetmapping
	SHADERSTATICPARM_SHADOWMAPPCF_1 = 8, ///< PCF 1
	SHADERSTATICPARM_SHADOWMAPPCF_2 = 9, ///< PCF 2
	SHADERSTATICPARM_SHADOWSAMPLER = 10, ///< sampler
	SHADERSTATICPARM_CELSHADING = 11, ///< celshading (alternative diffuse and specular math)
	SHADERSTATICPARM_CELOUTLINES = 12, ///< celoutline (depth buffer analysis to produce outlines)
	SHADERSTATICPARM_FXAA = 13, ///< fast approximate anti aliasing
	SHADERSTATICPARM_COLORFRINGE = 14, ///< colorfringe (chromatic aberration)
	SHADERSTATICPARM_VOLUMETRICDEBUG = 15, ///< volumetric scene-depth reconstruction visualiser
	SHADERSTATICPARM_VOLUMETRICSHAFTS = 16, ///< RT god rays consumed by the volumetric murk (Mac only)
	SHADERSTATICPARM_RTLIQUIDS = 17, ///< liquid surfaces multiply in the RT lighting term (Mac only)
	SHADERSTATICPARM_VOLUMETRICKERNELFOG = 18, ///< murk composites the Metal fog kernel's output (Mac only)
	SHADERSTATICPARM_LAVA = 19, ///< lava boil: 3D-noise domain warp + glow pulse on lava surfaces
	SHADERSTATICPARM_REDGLOW = 20, ///< r_redglow: saturated reds emit, so warning bands and buttons burn
	SHADERSTATICPARM_GAMMAANALYTIC = 21, ///< r_gamma_analytic: evaluate the gamma curve, do not sample the LUT (METAL.md Phase 7)
	SHADERSTATICPARM_WATERSWIRL = 22, ///< F3: water/slime churn + teleporter starfield rotation on the surface texcoords
	SHADERSTATICPARM_LAVASHIMMER = 23, ///< r_lavashimmer: hot air above lava refracts the scene behind it (postprocess; MSL body only)
	SHADERSTATICPARM_VOLUMETRICLIQUIDFADE = 24, ///< r_volumetric_liquidfade: the murk applied per fragment to alpha-blended liquids
	SHADERSTATICPARM_WATERSCREEN = 25, ///< r_watersurface: blended liquids refract the frame beneath them (screen-space)
	SHADERSTATICPARM_SOFTPARTICLES = 26, ///< cl_particles_soft: MODE_GENERIC fades a fragment by its distance to the scene depth behind it (BEAUTY A4)
	SHADERSTATICPARM_PARTREFRACT = 27 ///< cl_particles_refract: MODE_GENERIC replaces a refract particle's fragment with the displaced frame copy (BEAUTY A5)
};
#define SHADERSTATICPARMS_COUNT 28

static const char *shaderstaticparmstrings_list[SHADERSTATICPARMS_COUNT];
static int shaderstaticparms_count = 0;

static unsigned int r_compileshader_staticparms[(SHADERSTATICPARMS_COUNT + 0x1F) >> 5] = {0};
#define R_COMPILESHADER_STATICPARM_ENABLE(p) r_compileshader_staticparms[(p) >> 5] |= (1 << ((p) & 0x1F))

extern qbool r_shadow_shadowmapsampler;
extern int r_shadow_shadowmappcf;
qbool R_CompileShader_CheckStaticParms(void)
{
	static int r_compileshader_staticparms_save[(SHADERSTATICPARMS_COUNT + 0x1F) >> 5];
	memcpy(r_compileshader_staticparms_save, r_compileshader_staticparms, sizeof(r_compileshader_staticparms));
	memset(r_compileshader_staticparms, 0, sizeof(r_compileshader_staticparms));

	// detect all
	if (r_glsl_saturation_redcompensate.integer && !m5_stock.integer)
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_SATURATION_REDCOMPENSATE);
	if (r_glsl_vertextextureblend_usebothalphas.integer)
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_VERTEXTEXTUREBLEND_USEBOTHALPHAS);
	if (r_shadow_glossexact.integer)
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_EXACTSPECULARMATH);
	if (r_glsl_postprocess.integer)
	{
		if (r_glsl_postprocess_uservec1_enable.integer)
			R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_POSTPROCESS_USERVEC1);
		if (r_glsl_postprocess_uservec2_enable.integer)
			R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_POSTPROCESS_USERVEC2);
		if (r_glsl_postprocess_uservec3_enable.integer)
			R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_POSTPROCESS_USERVEC3);
		if (r_glsl_postprocess_uservec4_enable.integer)
			R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_POSTPROCESS_USERVEC4);
	}
	if (r_fxaa.integer && !m5_stock.integer)
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_FXAA);
	if (r_glsl_offsetmapping_lod.integer && r_glsl_offsetmapping_lod_distance.integer > 0)
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_OFFSETMAPPING_USELOD);

	if (r_shadow_shadowmapsampler)
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_SHADOWSAMPLER);
	if (r_shadow_shadowmappcf > 1)
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_SHADOWMAPPCF_2);
	else if (r_shadow_shadowmappcf)
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_SHADOWMAPPCF_1);
	if (r_celshading.integer)
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_CELSHADING);
	if (r_celoutlines.integer)
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_CELOUTLINES);
	if (r_colorfringe.value && !m5_stock.integer)
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_COLORFRINGE);
	// only compile the visualiser in when it can actually run (it needs the depth
	// texture that r_volumetric provides); the mode itself is a runtime uniform, so
	// switching between debug 1/2/3 does not trigger a shader recompile
	if (r_volumetric.integer && r_volumetric_debug.integer)
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_VOLUMETRICDEBUG);
#ifdef USE_RT_METAL
	// god rays: only compile the rect sampler + march term into the murk when the
	// Metal sidecar can actually supply the shaft buffer
	if (rt_metal.integer && rt_metal_shafts.integer && r_volumetric.integer && !rt_metal_fog.integer)
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_VOLUMETRICSHAFTS);
	// full in-kernel fog supersedes the screen-space shafts (the fog integral owns
	// the beams); the murk gains a composite-only branch with the GL march kept as
	// the per-frame fallback
	if (rt_metal.integer && rt_metal_fog.integer && r_volumetric.integer)
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_VOLUMETRICKERNELFOG);
	// liquids sample the RT term during the transparent pass; compile the
	// sampler into the surface shaders only when the fix is actually on.
	//
	// BOTH RENDERPATHS since 5-5-3. This was GL32-only for two slices, not for
	// any shader reason -- the MSL arm and the Metal bind have existed since
	// 5-5 -- but because no bed could measure it, and three sessions recorded
	// the wrong reason for that. The story is worth the lines, because the
	// wrong reason was believed by everyone who looked:
	//
	// "the pre-load sv_freezenonclients suppresses the r_wateralpha path" is
	// RETRACTED. It does not. What suppressed it was the QRP replacement pack:
	// an external image for *04water1 makes Mod_LoadTextureFromQ3Shader
	// succeed, and model_brush.c then skips the ENTIRE Q1 liquid
	// classification, so the texture arrives with no MATERIALFLAG_WATERALPHA
	// for r_wateralpha to act on and no liquid supercontents. Frozen and
	// unfrozen printed IDENTICAL flags. See m5_liquidflags, which fixes it,
	// and the CLAUDE.md fact -- the lesson there is the instrument: two rounds
	// of pixel A/Bs can only ever say "no difference", one print of
	// basematerialflags/currentalpha at the surface said which of six
	// candidate mechanisms it was in a single boot.
	//
	// With that fixed the ordinary frozen bed measures it, controls at zero:
	// the rt_liquid vantage isolates at 18.41% of pixels moved between
	// rt_metal_liquids 0 and 0.45, and GL vs Metal agree at the RT norm.
	if (rt_metal.integer && rt_metal_liquids.value > 0.0f)
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_RTLIQUIDS);
#endif
	// lava boil: compile the 3D-noise domain warp into the surface shaders only
	// when it is on, and only where a 3D sampler exists.
	//
	// THIS TEST USED TO READ `== RENDERPATH_GL32` AND THAT WAS NEVER A METAL
	// DECISION (METAL.md 6-4). It was written 2026-08-01, three days before
	// RENDERPATH_METAL existed, and its own comment names its real subject:
	// dp_texture3D needs the GLSL130 path, i.e. it is GLES2 that cannot have
	// it. Adding a third renderpath to the enum silently swept Metal into an
	// exclusion aimed at something else -- which is the hazard of spelling
	// "not GLES2" as "is GL32". Metal has had 3D textures since 6-1, so it is
	// spelled as what it means now.
	if (r_lavaboil.value > 0.0f && vid.renderpath != RENDERPATH_GLES2 && !m5_stock.integer)
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_LAVA);
	// water/teleport swirl (F3): the same 3D-sampler constraint as the lava
	// boil, spelled the same capability way. Its OWN parm rather than a widened
	// USELAVA, deliberately: an always-compiled WaterParams would add one
	// uniform call per surface batch in every lava-boil config and move the
	// cmdtrace digest off its baseline even with both swirl cvars 0 -- with its
	// own parm, cvars-off compiles the identical uniform set and the digest
	// self-preserves.
	if ((r_waterswirl.value > 0.0f || r_teleportswirl.value > 0.0f) && vid.renderpath != RENDERPATH_GLES2 && !m5_stock.integer)
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_WATERSWIRL);
	if (r_redglow.value > 0.0f && !m5_stock.integer)
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_REDGLOW);
	// LIQUIDFOG (r_volumetric_liquidfade): the murk, applied per fragment to an
	// alpha-blended liquid. Needs a 3D sampler, so it is spelled as the
	// CAPABILITY the lava boil and the swirl are spelled as -- "not GLES2",
	// never "is GL32", which is the equality test that silently stopped being
	// true the day RENDERPATH_METAL joined the enum (METAL.md 6-4).
	//
	// Gated on the CVAR ALONE and not on r_volumetric: the murk's own gate
	// moves with the map and the frame, and a static parm that follows it would
	// rebuild every shader on a map change. At r_volumetric 0 the uniform is 0
	// and the compiled branch is not taken, which costs one uniform compare.
	if (r_volumetric_liquidfade.value > 0.0f && vid.renderpath != RENDERPATH_GLES2 && !m5_stock.integer)
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_VOLUMETRICLIQUIDFADE);
	// WATERSURFACE (r_watersurface): the liquid surface's screen-space refraction.
	// It fetches the noise volume, so the same 3D-sampler capability test as the
	// swirl and the fade; gated on the cvar alone -- the copy it samples is taken
	// per frame and the uniform is 0 on every batch that is not a blended liquid.
	if (r_watersurface.integer && vid.renderpath != RENDERPATH_GLES2 && !m5_stock.integer)
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_WATERSCREEN);
	// SOFT PARTICLES (BEAUTY A4): compiled into every GENERIC permutation while
	// the cvar is on; the uniform is exactly zero on every draw that is not a
	// particle batch, so the console, the menu, the HUD and the bloom passes are
	// the old arithmetic (the RTLiquids exact-no-op shape).
	if (R_SoftParticles_Wanted() && vid.renderpath != RENDERPATH_GLES2)
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_SOFTPARTICLES);
	// REFRACTING PARTICLES (BEAUTY A5): the same shape -- into every GENERIC
	// permutation, the uniform exactly zero on every draw but a refract batch.
	if (R_PartRefract_Wanted() && vid.renderpath != RENDERPATH_GLES2)
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_PARTREFRACT);
	// HEAT SHIMMER, gated on the CAPABILITY and never on the renderpath's identity
	if (R_LavaShimmer_Wanted())
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_LAVASHIMMER);
	// ANALYTIC GAMMA (METAL.md Phase 7-1). Gated on the same predicate the
	// uniform upload uses, so the compiled shader and the values it is fed can
	// never disagree about which of the two curves is in force -- the 4d bloom
	// lesson, where a permutation bit and an attribute enable were derived from
	// two expressions that merely happened to agree. EDR forces it (Phase 8):
	// R_EDR_Wanted's own terms include the VID_GetGammaAnalytic refusal, so the
	// forced arm cannot enable the parm in a configuration the curve refuses.
	if (r_gamma_analytic.integer)
	{
		float ig[3], sc[3], bs[3], cb;
		if (VID_GetGammaAnalytic(ig, sc, bs, &cb))
			R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_GAMMAANALYTIC);
	}
	else if (R_EDR_Wanted())
		R_COMPILESHADER_STATICPARM_ENABLE(SHADERSTATICPARM_GAMMAANALYTIC);

	return memcmp(r_compileshader_staticparms, r_compileshader_staticparms_save, sizeof(r_compileshader_staticparms)) != 0;
}

#define R_COMPILESHADER_STATICPARM_EMIT(p, n) \
	if(r_compileshader_staticparms[(p) >> 5] & (1 << ((p) & 0x1F))) \
		shaderstaticparmstrings_list[shaderstaticparms_count++] = "#define " n "\n"; \
	else \
		shaderstaticparmstrings_list[shaderstaticparms_count++] = "\n"
static void R_CompileShader_AddStaticParms(unsigned int mode, uint64_t permutation)
{
	shaderstaticparms_count = 0;

	// emit all
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_SATURATION_REDCOMPENSATE, "SATURATION_REDCOMPENSATE");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_EXACTSPECULARMATH, "USEEXACTSPECULARMATH");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_POSTPROCESS_USERVEC1, "USERVEC1");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_POSTPROCESS_USERVEC2, "USERVEC2");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_POSTPROCESS_USERVEC3, "USERVEC3");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_POSTPROCESS_USERVEC4, "USERVEC4");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_VERTEXTEXTUREBLEND_USEBOTHALPHAS, "USEBOTHALPHAS");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_OFFSETMAPPING_USELOD, "USEOFFSETMAPPING_LOD");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_SHADOWMAPPCF_1, "USESHADOWMAPPCF 1");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_SHADOWMAPPCF_2, "USESHADOWMAPPCF 2");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_SHADOWSAMPLER, "USESHADOWSAMPLER");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_CELSHADING, "USECELSHADING");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_CELOUTLINES, "USECELOUTLINES");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_FXAA, "USEFXAA");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_COLORFRINGE, "USECOLORFRINGE");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_VOLUMETRICDEBUG, "USEVOLUMETRICDEBUG");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_VOLUMETRICSHAFTS, "USEVOLUMETRICSHAFTS");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_RTLIQUIDS, "USERTLIQUIDS");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_VOLUMETRICKERNELFOG, "USEVOLUMETRICKERNELFOG");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_LAVA, "USELAVA");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_LAVASHIMMER, "USELAVASHIMMER");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_REDGLOW, "USEREDGLOW");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_GAMMAANALYTIC, "USEGAMMAANALYTIC");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_WATERSWIRL, "USEWATERSWIRL");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_VOLUMETRICLIQUIDFADE, "USEVOLUMETRICLIQUIDFADE");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_WATERSCREEN, "USEWATERSCREEN");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_SOFTPARTICLES, "USESOFTPARTICLES");
	R_COMPILESHADER_STATICPARM_EMIT(SHADERSTATICPARM_PARTREFRACT, "USEPARTREFRACT");
}

/// information about each possible shader permutation
r_glsl_permutation_t *r_glsl_permutationhash[SHADERMODE_COUNT][SHADERPERMUTATION_HASHSIZE];
/// currently selected permutation
r_glsl_permutation_t *r_glsl_permutation;
/// storage for permutations linked in the hash table
memexpandablearray_t r_glsl_permutationarray;

/*
=======================================================================
UNIFORM INDIRECTION (METAL.md Phase 3)

R_SetupShader_Surface and its neighbours set ~200 uniforms per batch across
167 call sites, and every one of them called qglUniform* directly. That is
the single largest reason the renderer could not serve a second backend:
Metal has no such thing as a free-standing uniform, only a constant buffer.

These wrappers are the seam. Under GL each is a passthrough to the identical
qgl call, so the emitted GL command stream is unchanged BY CONSTRUCTION --
which is exactly what the Phase 1 command-digest gate measures, and why that
instrument was built before this refactor rather than after. Under Metal a
"location" becomes a byte offset into a per-draw constant struct (obtained by
reflection) and these become memcpys into the staging block; the negative
location keeps its meaning of "this uniform is not in this permutation", so
every existing `if (loc >= 0)` guard at the call sites stays correct.

The guards are deliberately left AT the call sites rather than folded in
here. Five of them wrap side effects -- matrix concatenations,
R_Volumetric_GetNoiseTexture(), the RT-liquids rectangle bind -- and hoisting
those into an unconditional argument list would run real work on every batch
for permutations that do not use it.
=======================================================================
*/
static int R_Shader_GetUniformLocation(const r_glsl_permutation_t *p, const char *name)
{
	// Under Metal a "location" is a BYTE OFFSET into the pipeline's reflected
	// constant struct (or an opaque encoding for a texture binding); -1 keeps
	// meaning "not in this permutation", which is what every call-site guard
	// tests -- including the five that wrap side effects.
	if (vid.renderpath == RENDERPATH_METAL)
		return Metal_Backend_GetUniformLocation(p->program, name);
	return qglGetUniformLocation(p->program, name);
}
// Make a compiled program current. Not a digest-intercepted entry point on
// either path, so this wrapper is invisible to the command gate by construction.
static void R_Shader_UseProgram(int program)
{
	if (vid.renderpath == RENDERPATH_METAL)
	{
		Metal_Backend_SetProgram(program);
		return;
	}
	qglUseProgram(program);CHECKGLERROR
}
static void R_Shader_Uniform1i(int loc, int v0)
{
	if (vid.renderpath == RENDERPATH_METAL) { Metal_Backend_SetUniformInt(loc, v0); return; }
	if (loc >= 0) qglUniform1i(loc, v0);
}
static void R_Shader_Uniform1f(int loc, float v0)
{
	if (vid.renderpath == RENDERPATH_METAL) { Metal_Backend_SetUniformFloats(loc, &v0, 1); return; }
	if (loc >= 0) qglUniform1f(loc, v0);
}
static void R_Shader_Uniform2f(int loc, float v0, float v1)
{
	if (vid.renderpath == RENDERPATH_METAL) { float v[2]; v[0] = v0; v[1] = v1; Metal_Backend_SetUniformFloats(loc, v, 2); return; }
	if (loc >= 0) qglUniform2f(loc, v0, v1);
}
static void R_Shader_Uniform3f(int loc, float v0, float v1, float v2)
{
	if (vid.renderpath == RENDERPATH_METAL) { float v[3]; v[0] = v0; v[1] = v1; v[2] = v2; Metal_Backend_SetUniformFloats(loc, v, 3); return; }
	if (loc >= 0) qglUniform3f(loc, v0, v1, v2);
}
static void R_Shader_Uniform4f(int loc, float v0, float v1, float v2, float v3)
{
	if (vid.renderpath == RENDERPATH_METAL) { float v[4]; v[0] = v0; v[1] = v1; v[2] = v2; v[3] = v3; Metal_Backend_SetUniformFloats(loc, v, 4); return; }
	if (loc >= 0) qglUniform4f(loc, v0, v1, v2, v3);
}
static void R_Shader_Uniform4fv(int loc, int count, const float *v)
{
	// vec4 ARRAY upload: on Metal the reflected member offset plus a flat copy
	// (MSL float4 arrays are tightly packed, 16-byte stride), on GL the counted
	// glUniform4fv against the array's element-0 location
	if (vid.renderpath == RENDERPATH_METAL) { Metal_Backend_SetUniformFloats(loc, v, 4 * count); return; }
	if (loc >= 0) qglUniform4fv(loc, count, v);
}
static void R_Shader_UniformMatrix3fv(int loc, int count, qbool transpose, const float *v)
{
	if (vid.renderpath == RENDERPATH_METAL) { Metal_Backend_SetUniformFloats(loc, v, 9 * count); return; }
	if (loc >= 0) qglUniformMatrix3fv(loc, count, transpose, v);
}
static void R_Shader_UniformMatrix4fv(int loc, int count, qbool transpose, const float *v)
{
	// MSL float4x4 is column-major and so is Matrix4x4_ToArrayFloatGL's output,
	// so this is a straight 64-byte copy; `transpose` is false at every call site.
	if (vid.renderpath == RENDERPATH_METAL) { Metal_Backend_SetUniformFloats(loc, v, 16 * count); return; }
	if (loc >= 0) qglUniformMatrix4fv(loc, count, transpose, v);
}

static r_glsl_permutation_t *R_GLSL_FindPermutation(unsigned int mode, uint64_t permutation)
{
	//unsigned int hashdepth = 0;
	unsigned int hashindex = (permutation * 0x1021) & (SHADERPERMUTATION_HASHSIZE - 1);
	r_glsl_permutation_t *p;
	for (p = r_glsl_permutationhash[mode][hashindex];p;p = p->hashnext)
	{
		if (p->mode == mode && p->permutation == permutation)
		{
			//if (hashdepth > 10)
			//	Con_Printf("R_GLSL_FindPermutation: Warning: %i:%i has hashdepth %i\n", mode, permutation, hashdepth);
			return p;
		}
		//hashdepth++;
	}
	p = (r_glsl_permutation_t*)Mem_ExpandableArray_AllocRecord(&r_glsl_permutationarray);
	p->mode = mode;
	p->permutation = permutation;
	p->hashnext = r_glsl_permutationhash[mode][hashindex];
	r_glsl_permutationhash[mode][hashindex] = p;
	//if (hashdepth > 10)
	//	Con_Printf("R_GLSL_FindPermutation: Warning: %i:%i has hashdepth %i\n", mode, permutation, hashdepth);
	return p;
}

static char *R_ShaderStrCat(const char **strings)
{
	char *string, *s;
	const char **p = strings;
	const char *t;
	size_t len = 0;
	for (p = strings;(t = *p);p++)
		len += strlen(t);
	len++;
	s = string = (char *)Mem_Alloc(r_main_mempool, len);
	len = 0;
	for (p = strings;(t = *p);p++)
	{
		len = strlen(t);
		memcpy(s, t, len);
		s += len;
	}
	*s = 0;
	return string;
}

static char *R_ShaderStrCat(const char **strings);
static void R_InitShaderModeInfo(void)
{
	int i, language;
	shadermodeinfo_t *modeinfo;
	// we have a bunch of things to compute that weren't calculated at engine compile time - all filenames should have a crc of the builtin strings to prevent accidental overrides (any customization must be updated to match engine)
	for (language = 0; language < SHADERLANGUAGE_COUNT; language++)
	{
		for (i = 0; i < SHADERMODE_COUNT; i++)
		{
			char filename[MAX_QPATH];
			modeinfo = &shadermodeinfo[language][i];
			modeinfo->builtinstring = R_ShaderStrCat(modeinfo->builtinshaderstrings);
			modeinfo->builtincrc = CRC_Block((const unsigned char *)modeinfo->builtinstring, strlen(modeinfo->builtinstring));
			dpsnprintf(filename, sizeof(filename), "%s/%s_crc%i.%s", modeinfo->extension, modeinfo->sourcebasename, modeinfo->builtincrc, modeinfo->extension);
			modeinfo->filename = Mem_strdup(r_main_mempool, filename);
		}
	}
}

static char *ShaderModeInfo_GetShaderText(shadermodeinfo_t *modeinfo, qbool printfromdisknotice, qbool builtinonly)
{
	char *shaderstring;
	// if the mode has no filename we have to return the builtin string
	if (builtinonly || !modeinfo->filename)
		return Mem_strdup(r_main_mempool, modeinfo->builtinstring);
	// note that FS_LoadFile appends a 0 byte to make it a valid string
	shaderstring = (char *)FS_LoadFile(modeinfo->filename, r_main_mempool, false, NULL);
	if (shaderstring)
	{
		if (printfromdisknotice)
			Con_DPrintf("Loading shaders from file %s...\n", modeinfo->filename);
		return shaderstring;
	}
	// fall back to builtinstring
	return Mem_strdup(r_main_mempool, modeinfo->builtinstring);
}

static void R_GLSL_CompilePermutation(r_glsl_permutation_t *p, unsigned int mode, uint64_t permutation)
{
	unsigned i;
	int ubibind;
	int sampler;
	shadermodeinfo_t *modeinfo = &shadermodeinfo[SHADERLANGUAGE_GLSL][mode];
	char *sourcestring;
	char permutationname[256];
	int vertstrings_count = 0;
	int geomstrings_count = 0;
	int fragstrings_count = 0;
	int metalstrings_first = 0;
	const char *vertstrings_list[32+5+SHADERSTATICPARMS_COUNT+1];
	const char *geomstrings_list[32+5+SHADERSTATICPARMS_COUNT+1];
	const char *fragstrings_list[32+5+SHADERSTATICPARMS_COUNT+1];

	if (p->compiled)
		return;
	p->compiled = true;
	p->program = 0;

	permutationname[0] = 0;
	sourcestring = ShaderModeInfo_GetShaderText(modeinfo, true, false);

	dp_strlcat(permutationname, modeinfo->filename, sizeof(permutationname));

	// we need 140 for r_glsl_skeletal (GL_ARB_uniform_buffer_object)
	if(vid.support.glshaderversion >= 140)
	{
		vertstrings_list[vertstrings_count++] = "#version 140\n";
		geomstrings_list[geomstrings_count++] = "#version 140\n";
		fragstrings_list[fragstrings_count++] = "#version 140\n";
		vertstrings_list[vertstrings_count++] = "#define GLSL140\n";
		geomstrings_list[geomstrings_count++] = "#define GLSL140\n";
		fragstrings_list[fragstrings_count++] = "#define GLSL140\n";
	}
	// if we can do #version 130, we should (this improves quality of offset/reliefmapping thanks to textureGrad)
	else if(vid.support.glshaderversion >= 130)
	{
		vertstrings_list[vertstrings_count++] = "#version 130\n";
		geomstrings_list[geomstrings_count++] = "#version 130\n";
		fragstrings_list[fragstrings_count++] = "#version 130\n";
		vertstrings_list[vertstrings_count++] = "#define GLSL130\n";
		geomstrings_list[geomstrings_count++] = "#define GLSL130\n";
		fragstrings_list[fragstrings_count++] = "#define GLSL130\n";
	}
	// if we can do #version 120, we should (this adds the invariant keyword)
	else if(vid.support.glshaderversion >= 120)
	{
		vertstrings_list[vertstrings_count++] = "#version 120\n";
		geomstrings_list[geomstrings_count++] = "#version 120\n";
		fragstrings_list[fragstrings_count++] = "#version 120\n";
		vertstrings_list[vertstrings_count++] = "#define GLSL120\n";
		geomstrings_list[geomstrings_count++] = "#define GLSL120\n";
		fragstrings_list[fragstrings_count++] = "#define GLSL120\n";
	}
	// GLES also adds several things from GLSL120
	switch(vid.renderpath)
	{
	case RENDERPATH_GLES2:
		vertstrings_list[vertstrings_count++] = "#define GLES\n";
		geomstrings_list[geomstrings_count++] = "#define GLES\n";
		fragstrings_list[fragstrings_count++] = "#define GLES\n";
		break;
	default:
		break;
	}

	// METAL.md Phase 2: the murk's shaping constants and its max-channel
	// shoulder, spliced from shader_density.h -- the SAME text the Metal fog
	// kernel splices, so the numbers cannot drift apart. They used to be named
	// #defines in the kernel against bare literals here, which meant the two
	// lockstep sites could not even be diffed.
	// METAL.md Phase 3 slice 4: everything from here to the shader body is
	// language-neutral -- the density prelude, the stage define, the mode
	// pretext, the permutation bits and the static parms are all just #defines.
	// The Metal compiler is handed exactly this slice of the SAME list, so the
	// two languages cannot select different features even in principle. What is
	// skipped is above (the GLSL #version lines, which have no MSL analogue) and
	// below (the GLSL body itself).
	metalstrings_first = vertstrings_count;

	vertstrings_list[vertstrings_count++] = DPD_SHADER_PRELUDE;
	geomstrings_list[geomstrings_count++] = DPD_SHADER_PRELUDE;
	fragstrings_list[fragstrings_count++] = DPD_SHADER_PRELUDE;

	// the first pretext is which type of shader to compile as
	// (later these will all be bound together as a program object)
	vertstrings_list[vertstrings_count++] = "#define VERTEX_SHADER\n";
	geomstrings_list[geomstrings_count++] = "#define GEOMETRY_SHADER\n";
	fragstrings_list[fragstrings_count++] = "#define FRAGMENT_SHADER\n";

	// the second pretext is the mode (for example a light source)
	vertstrings_list[vertstrings_count++] = modeinfo->pretext;
	geomstrings_list[geomstrings_count++] = modeinfo->pretext;
	fragstrings_list[fragstrings_count++] = modeinfo->pretext;
	dp_strlcat(permutationname, modeinfo->name, sizeof(permutationname));

	// now add all the permutation pretexts
	for (i = 0;i < SHADERPERMUTATION_COUNT;i++)
	{
		if (permutation & (1ll<<i))
		{
			vertstrings_list[vertstrings_count++] = shaderpermutationinfo[i].pretext;
			geomstrings_list[geomstrings_count++] = shaderpermutationinfo[i].pretext;
			fragstrings_list[fragstrings_count++] = shaderpermutationinfo[i].pretext;
			dp_strlcat(permutationname, shaderpermutationinfo[i].name, sizeof(permutationname));
		}
		else
		{
			// keep line numbers correct
			vertstrings_list[vertstrings_count++] = "\n";
			geomstrings_list[geomstrings_count++] = "\n";
			fragstrings_list[fragstrings_count++] = "\n";
		}
	}

	// add static parms
	R_CompileShader_AddStaticParms(mode, permutation);
	memcpy((char *)(vertstrings_list + vertstrings_count), shaderstaticparmstrings_list, sizeof(*vertstrings_list) * shaderstaticparms_count);
	vertstrings_count += shaderstaticparms_count;
	memcpy((char *)(geomstrings_list + geomstrings_count), shaderstaticparmstrings_list, sizeof(*vertstrings_list) * shaderstaticparms_count);
	geomstrings_count += shaderstaticparms_count;
	memcpy((char *)(fragstrings_list + fragstrings_count), shaderstaticparmstrings_list, sizeof(*vertstrings_list) * shaderstaticparms_count);
	fragstrings_count += shaderstaticparms_count;

	// now append the shader text itself
	vertstrings_list[vertstrings_count++] = sourcestring;
	geomstrings_list[geomstrings_count++] = sourcestring;
	fragstrings_list[fragstrings_count++] = sourcestring;

	// we don't currently use geometry shaders for anything, so just empty the list
	geomstrings_count = 0;

	// compile the shader program
	switch (vid.renderpath)
	{
	case RENDERPATH_METAL:
		// the shared pretext only: -1 drops the GLSL body appended just above,
		// and metal_backend appends shader_msl.h in its place
		p->program = Metal_Backend_CompilePermutation(mode, permutation,
			vertstrings_list + metalstrings_first, vertstrings_count - metalstrings_first - 1);
		break;
	case RENDERPATH_GL32:
	case RENDERPATH_GLES2:
		if (vertstrings_count + geomstrings_count + fragstrings_count)
			p->program = GL_Backend_CompileProgram(vertstrings_count, vertstrings_list, geomstrings_count, geomstrings_list, fragstrings_count, fragstrings_list);
		break;
	}
	if (p->program)
	{
		CHECKGLERROR
		R_Shader_UseProgram(p->program);
		// look up all the uniform variable names we care about, so we don't
		// have to look them up every time we set them

#if 0
		// debugging aid
		{
			GLint activeuniformindex = 0;
			GLint numactiveuniforms = 0;
			char uniformname[128];
			GLsizei uniformnamelength = 0;
			GLint uniformsize = 0;
			GLenum uniformtype = 0;
			memset(uniformname, 0, sizeof(uniformname));
			qglGetProgramiv(p->program, GL_ACTIVE_UNIFORMS, &numactiveuniforms);
			Con_Printf("Shader has %i uniforms\n", numactiveuniforms);
			for (activeuniformindex = 0;activeuniformindex < numactiveuniforms;activeuniformindex++)
			{
				qglGetActiveUniform(p->program, activeuniformindex, sizeof(uniformname) - 1, &uniformnamelength, &uniformsize, &uniformtype, uniformname);
				Con_Printf("Uniform %i name \"%s\" size %i type %i\n", (int)activeuniformindex, uniformname, (int)uniformsize, (int)uniformtype);
			}
		}
#endif

		p->loc_Texture_First              = R_Shader_GetUniformLocation(p, "Texture_First");
		p->loc_Texture_Second             = R_Shader_GetUniformLocation(p, "Texture_Second");
		p->loc_Texture_GammaRamps         = R_Shader_GetUniformLocation(p, "Texture_GammaRamps");
		p->loc_Texture_Normal             = R_Shader_GetUniformLocation(p, "Texture_Normal");
		p->loc_Texture_Color              = R_Shader_GetUniformLocation(p, "Texture_Color");
		p->loc_Texture_Gloss              = R_Shader_GetUniformLocation(p, "Texture_Gloss");
		p->loc_Texture_Glow               = R_Shader_GetUniformLocation(p, "Texture_Glow");
		p->loc_Texture_SecondaryNormal    = R_Shader_GetUniformLocation(p, "Texture_SecondaryNormal");
		p->loc_Texture_SecondaryColor     = R_Shader_GetUniformLocation(p, "Texture_SecondaryColor");
		p->loc_Texture_SecondaryGloss     = R_Shader_GetUniformLocation(p, "Texture_SecondaryGloss");
		p->loc_Texture_SecondaryGlow      = R_Shader_GetUniformLocation(p, "Texture_SecondaryGlow");
		p->loc_Texture_Pants              = R_Shader_GetUniformLocation(p, "Texture_Pants");
		p->loc_Texture_Shirt              = R_Shader_GetUniformLocation(p, "Texture_Shirt");
		p->loc_Texture_FogHeightTexture   = R_Shader_GetUniformLocation(p, "Texture_FogHeightTexture");
		p->loc_Texture_FogMask            = R_Shader_GetUniformLocation(p, "Texture_FogMask");
		p->loc_Texture_LightGrid          = R_Shader_GetUniformLocation(p, "Texture_LightGrid");
		p->loc_Texture_Lightmap           = R_Shader_GetUniformLocation(p, "Texture_Lightmap");
		p->loc_Texture_Deluxemap          = R_Shader_GetUniformLocation(p, "Texture_Deluxemap");
		p->loc_Texture_Attenuation        = R_Shader_GetUniformLocation(p, "Texture_Attenuation");
		p->loc_Texture_Cube               = R_Shader_GetUniformLocation(p, "Texture_Cube");
		p->loc_Texture_Refraction         = R_Shader_GetUniformLocation(p, "Texture_Refraction");
		p->loc_Texture_Reflection         = R_Shader_GetUniformLocation(p, "Texture_Reflection");
		p->loc_Texture_ShadowMap2D        = R_Shader_GetUniformLocation(p, "Texture_ShadowMap2D");
		p->loc_Texture_CubeProjection     = R_Shader_GetUniformLocation(p, "Texture_CubeProjection");
		p->loc_Texture_ScreenNormalMap    = R_Shader_GetUniformLocation(p, "Texture_ScreenNormalMap");
		p->loc_Texture_ScreenDepth        = R_Shader_GetUniformLocation(p, "Texture_ScreenDepth");
		p->loc_Texture_VolumeNoise        = R_Shader_GetUniformLocation(p, "Texture_VolumeNoise");
		p->loc_Texture_VolumeField        = R_Shader_GetUniformLocation(p, "Texture_VolumeField");
		p->loc_Texture_VolumeIrr          = R_Shader_GetUniformLocation(p, "Texture_VolumeIrr");
		p->loc_Texture_Shafts             = R_Shader_GetUniformLocation(p, "Texture_Shafts");
		p->loc_Texture_RTTerm             = R_Shader_GetUniformLocation(p, "Texture_RTTerm");
		p->loc_Texture_RTLiquid           = R_Shader_GetUniformLocation(p, "Texture_RTLiquid");
		p->loc_Texture_WaterScreen        = R_Shader_GetUniformLocation(p, "Texture_WaterScreen");   // WATERSURFACE
		p->loc_Texture_KernelFog          = R_Shader_GetUniformLocation(p, "Texture_KernelFog");
		p->loc_Texture_ScreenDiffuse      = R_Shader_GetUniformLocation(p, "Texture_ScreenDiffuse");
		p->loc_Texture_ScreenSpecular     = R_Shader_GetUniformLocation(p, "Texture_ScreenSpecular");
		p->loc_Texture_ReflectMask        = R_Shader_GetUniformLocation(p, "Texture_ReflectMask");
		p->loc_Texture_ReflectCube        = R_Shader_GetUniformLocation(p, "Texture_ReflectCube");
		p->loc_Texture_BounceGrid         = R_Shader_GetUniformLocation(p, "Texture_BounceGrid");
		p->loc_Alpha                      = R_Shader_GetUniformLocation(p, "Alpha");
		p->loc_SoftParticle               = R_Shader_GetUniformLocation(p, "SoftParticle");
		p->loc_SoftParticleTex            = R_Shader_GetUniformLocation(p, "SoftParticleTex");
		p->loc_PartRefract                = R_Shader_GetUniformLocation(p, "PartRefract");
		p->loc_PartRefractTex             = R_Shader_GetUniformLocation(p, "PartRefractTex");
		p->loc_BloomBlur_Parameters       = R_Shader_GetUniformLocation(p, "BloomBlur_Parameters");
		p->loc_ClientTime                 = R_Shader_GetUniformLocation(p, "ClientTime");
		p->loc_Color_Ambient              = R_Shader_GetUniformLocation(p, "Color_Ambient");
		p->loc_Color_Diffuse              = R_Shader_GetUniformLocation(p, "Color_Diffuse");
		p->loc_Color_Specular             = R_Shader_GetUniformLocation(p, "Color_Specular");
		p->loc_Color_Glow                 = R_Shader_GetUniformLocation(p, "Color_Glow");
		p->loc_Color_Pants                = R_Shader_GetUniformLocation(p, "Color_Pants");
		p->loc_Color_Shirt                = R_Shader_GetUniformLocation(p, "Color_Shirt");
		p->loc_DeferredColor_Ambient      = R_Shader_GetUniformLocation(p, "DeferredColor_Ambient");
		p->loc_DeferredColor_Diffuse      = R_Shader_GetUniformLocation(p, "DeferredColor_Diffuse");
		p->loc_DeferredColor_Specular     = R_Shader_GetUniformLocation(p, "DeferredColor_Specular");
		p->loc_DeferredMod_Diffuse        = R_Shader_GetUniformLocation(p, "DeferredMod_Diffuse");
		p->loc_DeferredMod_Specular       = R_Shader_GetUniformLocation(p, "DeferredMod_Specular");
		p->loc_DistortScaleRefractReflect = R_Shader_GetUniformLocation(p, "DistortScaleRefractReflect");
		p->loc_EyePosition                = R_Shader_GetUniformLocation(p, "EyePosition");
		p->loc_FogColor                   = R_Shader_GetUniformLocation(p, "FogColor");
		p->loc_FogHeightFade              = R_Shader_GetUniformLocation(p, "FogHeightFade");
		p->loc_FogPlane                   = R_Shader_GetUniformLocation(p, "FogPlane");
		p->loc_FogPlaneViewDist           = R_Shader_GetUniformLocation(p, "FogPlaneViewDist");
		p->loc_FogRangeRecip              = R_Shader_GetUniformLocation(p, "FogRangeRecip");
		p->loc_LightColor                 = R_Shader_GetUniformLocation(p, "LightColor");
		p->loc_LightGridMatrix            = R_Shader_GetUniformLocation(p, "LightGridMatrix");
		p->loc_LightGridNormalMatrix      = R_Shader_GetUniformLocation(p, "LightGridNormalMatrix");
		p->loc_LightDir                   = R_Shader_GetUniformLocation(p, "LightDir");
		p->loc_LightPosition              = R_Shader_GetUniformLocation(p, "LightPosition");
		p->loc_OffsetMapping_ScaleSteps   = R_Shader_GetUniformLocation(p, "OffsetMapping_ScaleSteps");
		p->loc_OffsetMapping_LodDistance  = R_Shader_GetUniformLocation(p, "OffsetMapping_LodDistance");
		p->loc_OffsetMapping_Bias         = R_Shader_GetUniformLocation(p, "OffsetMapping_Bias");
		p->loc_PixelSize                  = R_Shader_GetUniformLocation(p, "PixelSize");
		p->loc_FxaaSpan                   = R_Shader_GetUniformLocation(p, "FxaaSpan");
		p->loc_ReflectColor               = R_Shader_GetUniformLocation(p, "ReflectColor");
		p->loc_ReflectFactor              = R_Shader_GetUniformLocation(p, "ReflectFactor");
		p->loc_ReflectOffset              = R_Shader_GetUniformLocation(p, "ReflectOffset");
		p->loc_RefractColor               = R_Shader_GetUniformLocation(p, "RefractColor");
		p->loc_Saturation                 = R_Shader_GetUniformLocation(p, "Saturation");
		p->loc_ScreenCenterRefractReflect = R_Shader_GetUniformLocation(p, "ScreenCenterRefractReflect");
		p->loc_ScreenScaleRefractReflect  = R_Shader_GetUniformLocation(p, "ScreenScaleRefractReflect");
		p->loc_ScreenToDepth              = R_Shader_GetUniformLocation(p, "ScreenToDepth");
		p->loc_ShadowMap_Parameters       = R_Shader_GetUniformLocation(p, "ShadowMap_Parameters");
		p->loc_ShadowMap_TextureScale     = R_Shader_GetUniformLocation(p, "ShadowMap_TextureScale");
		p->loc_SpecularPower              = R_Shader_GetUniformLocation(p, "SpecularPower");
		p->loc_UserVec1                   = R_Shader_GetUniformLocation(p, "UserVec1");
		p->loc_UserVec2                   = R_Shader_GetUniformLocation(p, "UserVec2");
		p->loc_UserVec3                   = R_Shader_GetUniformLocation(p, "UserVec3");
		p->loc_UserVec4                   = R_Shader_GetUniformLocation(p, "UserVec4");
		p->loc_ColorFringe                = R_Shader_GetUniformLocation(p, "ColorFringe");
		p->loc_HdrShoulder                = R_Shader_GetUniformLocation(p, "HdrShoulder");
		p->loc_BallPressure               = R_Shader_GetUniformLocation(p, "BallPressure");
		p->loc_VolumetricDebugMode        = R_Shader_GetUniformLocation(p, "VolumetricDebugMode");
		p->loc_ViewToWorld                = R_Shader_GetUniformLocation(p, "ViewToWorld");
		p->loc_FrustumScale               = R_Shader_GetUniformLocation(p, "FrustumScale");
		p->loc_VolumetricFarClip          = R_Shader_GetUniformLocation(p, "VolumetricFarClip");
		p->loc_VolumetricOrigin           = R_Shader_GetUniformLocation(p, "VolumetricOrigin");
		p->loc_VolumetricParams           = R_Shader_GetUniformLocation(p, "VolumetricParams");
		p->loc_VolumetricNoise            = R_Shader_GetUniformLocation(p, "VolumetricNoise");
		p->loc_VolumetricWind             = R_Shader_GetUniformLocation(p, "VolumetricWind");
		p->loc_VolumetricColor            = R_Shader_GetUniformLocation(p, "VolumetricColor");
		p->loc_VolumetricFieldOrigin      = R_Shader_GetUniformLocation(p, "VolumetricFieldOrigin");
		p->loc_VolumetricFieldParams      = R_Shader_GetUniformLocation(p, "VolumetricFieldParams");
		p->loc_VolumetricLiquid           = R_Shader_GetUniformLocation(p, "VolumetricLiquid");
		p->loc_VolumetricMist             = R_Shader_GetUniformLocation(p, "VolumetricMist");
		p->loc_VolumetricMistLavaCut      = R_Shader_GetUniformLocation(p, "VolumetricMistLavaCut");
		p->loc_VolumetricLiquidFloor      = R_Shader_GetUniformLocation(p, "VolumetricLiquidFloor");
		p->loc_VolumetricLiquidDens       = R_Shader_GetUniformLocation(p, "VolumetricLiquidDens");
		p->loc_VolumetricShafts           = R_Shader_GetUniformLocation(p, "VolumetricShafts");
		p->loc_VolumetricReproj0          = R_Shader_GetUniformLocation(p, "VolumetricReproj0");
		p->loc_VolumetricReproj1          = R_Shader_GetUniformLocation(p, "VolumetricReproj1");
		p->loc_VolumetricReproj2          = R_Shader_GetUniformLocation(p, "VolumetricReproj2");
		p->loc_RTLiquid                   = R_Shader_GetUniformLocation(p, "RTLiquid");
		p->loc_WaterScreen                = R_Shader_GetUniformLocation(p, "WaterScreen");   // WATERSURFACE
		p->loc_WaterScreenSize            = R_Shader_GetUniformLocation(p, "WaterScreenSize");
		p->loc_WaterScreenTime            = R_Shader_GetUniformLocation(p, "WaterScreenTime");
		p->loc_WaterScreenLook            = R_Shader_GetUniformLocation(p, "WaterScreenLook");
		p->loc_WaterScreenTint            = R_Shader_GetUniformLocation(p, "WaterScreenTint");
		p->loc_LavaParams                 = R_Shader_GetUniformLocation(p, "LavaParams");
		p->loc_WaterParams                = R_Shader_GetUniformLocation(p, "WaterParams");
		p->loc_BoltFizz                   = R_Shader_GetUniformLocation(p, "BoltFizz");
		p->loc_BoltEye                    = R_Shader_GetUniformLocation(p, "BoltEye");
		p->loc_BoltClip                   = R_Shader_GetUniformLocation(p, "BoltClip");
		p->loc_LavaShimmer                = R_Shader_GetUniformLocation(p, "LavaShimmer");
		p->loc_LavaShimmer2               = R_Shader_GetUniformLocation(p, "LavaShimmer2");
		p->loc_RedGlow                    = R_Shader_GetUniformLocation(p, "RedGlow");
		p->loc_RTLiquidScale              = R_Shader_GetUniformLocation(p, "RTLiquidScale");
		p->loc_RTLiquidOwn                = R_Shader_GetUniformLocation(p, "RTLiquidOwn");
		p->loc_RTLiquidRT                 = R_Shader_GetUniformLocation(p, "RTLiquidRT");
		p->loc_ModelToWorld               = R_Shader_GetUniformLocation(p, "ModelToWorld");
		p->loc_LiquidFade                 = R_Shader_GetUniformLocation(p, "LiquidFade");
		p->loc_LiquidFadeAir              = R_Shader_GetUniformLocation(p, "LiquidFadeAir");
		p->loc_LiquidFadeGround           = R_Shader_GetUniformLocation(p, "LiquidFadeGround");
		p->loc_LiquidFadeLiquid           = R_Shader_GetUniformLocation(p, "LiquidFadeLiquid");
		p->loc_CausticParams              = R_Shader_GetUniformLocation(p, "CausticParams");
		p->loc_LiquidFadeOrigin           = R_Shader_GetUniformLocation(p, "LiquidFadeOrigin");
		p->loc_LiquidFadeFieldOrigin      = R_Shader_GetUniformLocation(p, "LiquidFadeFieldOrigin");
		p->loc_LiquidFadeFieldScale       = R_Shader_GetUniformLocation(p, "LiquidFadeFieldScale");
		p->loc_VolumetricKernelFog        = R_Shader_GetUniformLocation(p, "VolumetricKernelFog");
		p->loc_VolumetricUpsample         = R_Shader_GetUniformLocation(p, "VolumetricUpsample");
		p->loc_VolumetricUpsample2        = R_Shader_GetUniformLocation(p, "VolumetricUpsample2");
		p->loc_GammaAnalyticA             = R_Shader_GetUniformLocation(p, "GammaAnalyticA");
		p->loc_GammaAnalyticB             = R_Shader_GetUniformLocation(p, "GammaAnalyticB");
		p->loc_GammaAnalyticC             = R_Shader_GetUniformLocation(p, "GammaAnalyticC");
		p->loc_VolumetricWaterColor       = R_Shader_GetUniformLocation(p, "VolumetricWaterColor");
		p->loc_VolumetricSlimeColor       = R_Shader_GetUniformLocation(p, "VolumetricSlimeColor");
		p->loc_VolumetricLavaColor        = R_Shader_GetUniformLocation(p, "VolumetricLavaColor");
		p->loc_VolumetricGround           = R_Shader_GetUniformLocation(p, "VolumetricGround");
		p->loc_VolumetricGround2          = R_Shader_GetUniformLocation(p, "VolumetricGround2");
		p->loc_VolumetricIrr              = R_Shader_GetUniformLocation(p, "VolumetricIrr");
		p->loc_VolumetricIrrOrigin        = R_Shader_GetUniformLocation(p, "VolumetricIrrOrigin");
		p->loc_VolumetricIrrInvSize       = R_Shader_GetUniformLocation(p, "VolumetricIrrInvSize");
		p->loc_VolumetricSwirl            = R_Shader_GetUniformLocation(p, "VolumetricSwirl");
		// an ARRAY uniform: GL drivers may report it as "MarchLights" or
		// "MarchLights[0]", so try both; Metal reflects the bare member name
		p->loc_MarchLights                = R_Shader_GetUniformLocation(p, "MarchLights");
		if (p->loc_MarchLights < 0)
			p->loc_MarchLights            = R_Shader_GetUniformLocation(p, "MarchLights[0]");
		p->loc_MarchLightParams           = R_Shader_GetUniformLocation(p, "MarchLightParams");
		p->loc_VolumetricGroundWind       = R_Shader_GetUniformLocation(p, "VolumetricGroundWind");
		p->loc_VolumetricGroundColor      = R_Shader_GetUniformLocation(p, "VolumetricGroundColor");
		p->loc_FogSurfaceAmount           = R_Shader_GetUniformLocation(p, "FogSurfaceAmount");
		p->loc_ViewTintColor              = R_Shader_GetUniformLocation(p, "ViewTintColor");
		p->loc_ViewToLight                = R_Shader_GetUniformLocation(p, "ViewToLight");
		p->loc_ModelToLight               = R_Shader_GetUniformLocation(p, "ModelToLight");
		p->loc_TexMatrix                  = R_Shader_GetUniformLocation(p, "TexMatrix");
		p->loc_BackgroundTexMatrix        = R_Shader_GetUniformLocation(p, "BackgroundTexMatrix");
		p->loc_ModelViewMatrix            = R_Shader_GetUniformLocation(p, "ModelViewMatrix");
		p->loc_ModelViewProjectionMatrix  = R_Shader_GetUniformLocation(p, "ModelViewProjectionMatrix");
		p->loc_PixelToScreenTexCoord      = R_Shader_GetUniformLocation(p, "PixelToScreenTexCoord");
		p->loc_ModelToReflectCube         = R_Shader_GetUniformLocation(p, "ModelToReflectCube");
		p->loc_ShadowMapMatrix            = R_Shader_GetUniformLocation(p, "ShadowMapMatrix");
		p->loc_BloomColorSubtract         = R_Shader_GetUniformLocation(p, "BloomColorSubtract");
		p->loc_NormalmapScrollBlend       = R_Shader_GetUniformLocation(p, "NormalmapScrollBlend");
		p->loc_BounceGridMatrix           = R_Shader_GetUniformLocation(p, "BounceGridMatrix");
		p->loc_BounceGridIntensity        = R_Shader_GetUniformLocation(p, "BounceGridIntensity");
		// initialize the samplers to refer to the texture units we use
		p->tex_Texture_First = -1;
		p->tex_Texture_Second = -1;
		p->tex_Texture_GammaRamps = -1;
		p->tex_Texture_Normal = -1;
		p->tex_Texture_Color = -1;
		p->tex_Texture_Gloss = -1;
		p->tex_Texture_Glow = -1;
		p->tex_Texture_SecondaryNormal = -1;
		p->tex_Texture_SecondaryColor = -1;
		p->tex_Texture_SecondaryGloss = -1;
		p->tex_Texture_SecondaryGlow = -1;
		p->tex_Texture_Pants = -1;
		p->tex_Texture_Shirt = -1;
		p->tex_Texture_FogHeightTexture = -1;
		p->tex_Texture_FogMask = -1;
		p->tex_Texture_LightGrid = -1;
		p->tex_Texture_Lightmap = -1;
		p->tex_Texture_Deluxemap = -1;
		p->tex_Texture_Attenuation = -1;
		p->tex_Texture_Cube = -1;
		p->tex_Texture_Refraction = -1;
		p->tex_Texture_Reflection = -1;
		p->tex_Texture_ShadowMap2D = -1;
		p->tex_Texture_CubeProjection = -1;
		p->tex_Texture_ScreenNormalMap = -1;
		p->tex_Texture_ScreenDepth = -1;
		p->tex_Texture_VolumeNoise = -1;
		p->tex_Texture_VolumeField = -1;
		p->tex_Texture_VolumeIrr = -1;
		p->tex_Texture_Shafts = -1;
		p->tex_Texture_RTTerm = -1;
		p->tex_Texture_RTLiquid = -1;
		p->tex_Texture_WaterScreen = -1;
		p->tex_Texture_KernelFog = -1;
		p->tex_Texture_ScreenDiffuse = -1;
		p->tex_Texture_ScreenSpecular = -1;
		p->tex_Texture_ReflectMask = -1;
		p->tex_Texture_ReflectCube = -1;
		p->tex_Texture_BounceGrid = -1;
		// bind the texture samplers in use
		sampler = 0;
		if (p->loc_Texture_First           >= 0) {p->tex_Texture_First            = sampler;R_Shader_Uniform1i(p->loc_Texture_First           , sampler);sampler++;}
		if (p->loc_Texture_Second          >= 0) {p->tex_Texture_Second           = sampler;R_Shader_Uniform1i(p->loc_Texture_Second          , sampler);sampler++;}
		if (p->loc_Texture_GammaRamps      >= 0) {p->tex_Texture_GammaRamps       = sampler;R_Shader_Uniform1i(p->loc_Texture_GammaRamps      , sampler);sampler++;}
		if (p->loc_Texture_Normal          >= 0) {p->tex_Texture_Normal           = sampler;R_Shader_Uniform1i(p->loc_Texture_Normal          , sampler);sampler++;}
		if (p->loc_Texture_Color           >= 0) {p->tex_Texture_Color            = sampler;R_Shader_Uniform1i(p->loc_Texture_Color           , sampler);sampler++;}
		if (p->loc_Texture_Gloss           >= 0) {p->tex_Texture_Gloss            = sampler;R_Shader_Uniform1i(p->loc_Texture_Gloss           , sampler);sampler++;}
		if (p->loc_Texture_Glow            >= 0) {p->tex_Texture_Glow             = sampler;R_Shader_Uniform1i(p->loc_Texture_Glow            , sampler);sampler++;}
		if (p->loc_Texture_SecondaryNormal >= 0) {p->tex_Texture_SecondaryNormal  = sampler;R_Shader_Uniform1i(p->loc_Texture_SecondaryNormal , sampler);sampler++;}
		if (p->loc_Texture_SecondaryColor  >= 0) {p->tex_Texture_SecondaryColor   = sampler;R_Shader_Uniform1i(p->loc_Texture_SecondaryColor  , sampler);sampler++;}
		if (p->loc_Texture_SecondaryGloss  >= 0) {p->tex_Texture_SecondaryGloss   = sampler;R_Shader_Uniform1i(p->loc_Texture_SecondaryGloss  , sampler);sampler++;}
		if (p->loc_Texture_SecondaryGlow   >= 0) {p->tex_Texture_SecondaryGlow    = sampler;R_Shader_Uniform1i(p->loc_Texture_SecondaryGlow   , sampler);sampler++;}
		if (p->loc_Texture_Pants           >= 0) {p->tex_Texture_Pants            = sampler;R_Shader_Uniform1i(p->loc_Texture_Pants           , sampler);sampler++;}
		if (p->loc_Texture_Shirt           >= 0) {p->tex_Texture_Shirt            = sampler;R_Shader_Uniform1i(p->loc_Texture_Shirt           , sampler);sampler++;}
		if (p->loc_Texture_FogHeightTexture>= 0) {p->tex_Texture_FogHeightTexture = sampler;R_Shader_Uniform1i(p->loc_Texture_FogHeightTexture, sampler);sampler++;}
		if (p->loc_Texture_FogMask         >= 0) {p->tex_Texture_FogMask          = sampler;R_Shader_Uniform1i(p->loc_Texture_FogMask         , sampler);sampler++;}
		if (p->loc_Texture_LightGrid       >= 0) {p->tex_Texture_LightGrid        = sampler;R_Shader_Uniform1i(p->loc_Texture_LightGrid       , sampler);sampler++;}
		if (p->loc_Texture_Lightmap        >= 0) {p->tex_Texture_Lightmap         = sampler;R_Shader_Uniform1i(p->loc_Texture_Lightmap        , sampler);sampler++;}
		if (p->loc_Texture_Deluxemap       >= 0) {p->tex_Texture_Deluxemap        = sampler;R_Shader_Uniform1i(p->loc_Texture_Deluxemap       , sampler);sampler++;}
		if (p->loc_Texture_Attenuation     >= 0) {p->tex_Texture_Attenuation      = sampler;R_Shader_Uniform1i(p->loc_Texture_Attenuation     , sampler);sampler++;}
		if (p->loc_Texture_Cube            >= 0) {p->tex_Texture_Cube             = sampler;R_Shader_Uniform1i(p->loc_Texture_Cube            , sampler);sampler++;}
		if (p->loc_Texture_Refraction      >= 0) {p->tex_Texture_Refraction       = sampler;R_Shader_Uniform1i(p->loc_Texture_Refraction      , sampler);sampler++;}
		if (p->loc_Texture_Reflection      >= 0) {p->tex_Texture_Reflection       = sampler;R_Shader_Uniform1i(p->loc_Texture_Reflection      , sampler);sampler++;}
		if (p->loc_Texture_ShadowMap2D     >= 0) {p->tex_Texture_ShadowMap2D      = sampler;R_Shader_Uniform1i(p->loc_Texture_ShadowMap2D     , sampler);sampler++;}
		if (p->loc_Texture_CubeProjection  >= 0) {p->tex_Texture_CubeProjection   = sampler;R_Shader_Uniform1i(p->loc_Texture_CubeProjection  , sampler);sampler++;}
		if (p->loc_Texture_ScreenNormalMap >= 0) {p->tex_Texture_ScreenNormalMap  = sampler;R_Shader_Uniform1i(p->loc_Texture_ScreenNormalMap , sampler);sampler++;}
		if (p->loc_Texture_ScreenDepth     >= 0) {p->tex_Texture_ScreenDepth      = sampler;R_Shader_Uniform1i(p->loc_Texture_ScreenDepth     , sampler);sampler++;}
		if (p->loc_Texture_VolumeNoise     >= 0) {p->tex_Texture_VolumeNoise      = sampler;R_Shader_Uniform1i(p->loc_Texture_VolumeNoise     , sampler);sampler++;}
		if (p->loc_Texture_VolumeField     >= 0) {p->tex_Texture_VolumeField      = sampler;R_Shader_Uniform1i(p->loc_Texture_VolumeField     , sampler);sampler++;}
		if (p->loc_Texture_VolumeIrr       >= 0) {p->tex_Texture_VolumeIrr        = sampler;R_Shader_Uniform1i(p->loc_Texture_VolumeIrr       , sampler);sampler++;}
		if (p->loc_Texture_Shafts          >= 0) {p->tex_Texture_Shafts           = sampler;R_Shader_Uniform1i(p->loc_Texture_Shafts          , sampler);sampler++;}
		if (p->loc_Texture_RTTerm          >= 0) {p->tex_Texture_RTTerm           = sampler;R_Shader_Uniform1i(p->loc_Texture_RTTerm          , sampler);sampler++;}
		if (p->loc_Texture_RTLiquid        >= 0) {p->tex_Texture_RTLiquid         = sampler;R_Shader_Uniform1i(p->loc_Texture_RTLiquid        , sampler);sampler++;}   // SEPTEMBER2 C2: LOCKSTEP shader_msl.h DP_TEX_RTLIQUID = DP_TEX_RTTERM + 1
		if (p->loc_Texture_WaterScreen     >= 0) {p->tex_Texture_WaterScreen      = sampler;R_Shader_Uniform1i(p->loc_Texture_WaterScreen     , sampler);sampler++;}   // WATERSURFACE: LOCKSTEP shader_msl.h DP_TEX_WATERSCREEN, the index after RTLiquid
		if (p->loc_Texture_KernelFog       >= 0) {p->tex_Texture_KernelFog        = sampler;R_Shader_Uniform1i(p->loc_Texture_KernelFog       , sampler);sampler++;}
		if (p->loc_Texture_ScreenDiffuse   >= 0) {p->tex_Texture_ScreenDiffuse    = sampler;R_Shader_Uniform1i(p->loc_Texture_ScreenDiffuse   , sampler);sampler++;}
		if (p->loc_Texture_ScreenSpecular  >= 0) {p->tex_Texture_ScreenSpecular   = sampler;R_Shader_Uniform1i(p->loc_Texture_ScreenSpecular  , sampler);sampler++;}
		if (p->loc_Texture_ReflectMask     >= 0) {p->tex_Texture_ReflectMask      = sampler;R_Shader_Uniform1i(p->loc_Texture_ReflectMask     , sampler);sampler++;}
		if (p->loc_Texture_ReflectCube     >= 0) {p->tex_Texture_ReflectCube      = sampler;R_Shader_Uniform1i(p->loc_Texture_ReflectCube     , sampler);sampler++;}
		if (p->loc_Texture_BounceGrid      >= 0) {p->tex_Texture_BounceGrid       = sampler;R_Shader_Uniform1i(p->loc_Texture_BounceGrid      , sampler);sampler++;}
		// get the uniform block indices so we can bind them
		p->ubiloc_Skeletal_Transform12_UniformBlock = -1;
#ifndef USE_GLES2 /* FIXME: GLES3 only */
		// raw GL, and the skeletal path is Phase 4b -- leaving the index at -1
		// on Metal is what the two consumers below already test for
		if (vid.renderpath != RENDERPATH_METAL)
			p->ubiloc_Skeletal_Transform12_UniformBlock = qglGetUniformBlockIndex(p->program, "Skeletal_Transform12_UniformBlock");
#endif
		// clear the uniform block bindings
		p->ubibind_Skeletal_Transform12_UniformBlock = -1;
		// bind the uniform blocks in use
		ubibind = 0;
#ifndef USE_GLES2 /* FIXME: GLES3 only */
		if (p->ubiloc_Skeletal_Transform12_UniformBlock >= 0) {p->ubibind_Skeletal_Transform12_UniformBlock = ubibind;qglUniformBlockBinding(p->program, p->ubiloc_Skeletal_Transform12_UniformBlock, ubibind);ubibind++;}
#endif
		// we're done compiling and setting up the shader, at least until it is used
		CHECKGLERROR
		Con_DPrintf("^5GLSL shader %s compiled (%i textures).\n", permutationname, sampler);
	}
	else
		Con_Printf("^1GLSL shader %s failed!  some features may not work properly.\n", permutationname);

	// free the strings
	if (sourcestring)
		Mem_Free(sourcestring);
}

static void R_SetupShader_SetPermutationGLSL(unsigned int mode, uint64_t permutation)
{
	r_glsl_permutation_t *perm = R_GLSL_FindPermutation(mode, permutation);
	if (r_glsl_permutation != perm)
	{
		r_glsl_permutation = perm;
		if (!r_glsl_permutation->program)
		{
			if (!r_glsl_permutation->compiled)
			{
				Con_DPrintf("Compiling shader mode %u permutation %" PRIx64 "\n", mode, permutation);
				R_GLSL_CompilePermutation(perm, mode, permutation);
			}
			if (!r_glsl_permutation->program)
			{
				// remove features until we find a valid permutation
				unsigned i;
				for (i = 0;i < SHADERPERMUTATION_COUNT;i++)
				{
					// reduce i more quickly whenever it would not remove any bits
					uint64_t j = 1ll<<(SHADERPERMUTATION_COUNT-1-i);
					if (!(permutation & j))
						continue;
					permutation -= j;
					r_glsl_permutation = R_GLSL_FindPermutation(mode, permutation);
					if (!r_glsl_permutation->compiled)
						R_GLSL_CompilePermutation(perm, mode, permutation);
					if (r_glsl_permutation->program)
						break;
				}
				if (i >= SHADERPERMUTATION_COUNT)
				{
					//Con_Printf("Could not find a working OpenGL 2.0 shader for permutation %s %s\n", shadermodeinfo[mode].filename, shadermodeinfo[mode].pretext);
					r_glsl_permutation = R_GLSL_FindPermutation(mode, permutation);
					R_Shader_UseProgram(0);
					return; // no bit left to clear, entire mode is broken
				}
			}
		}
		CHECKGLERROR
		R_Shader_UseProgram(r_glsl_permutation->program);
	}
	if (r_glsl_permutation->loc_ModelViewProjectionMatrix >= 0) R_Shader_UniformMatrix4fv(r_glsl_permutation->loc_ModelViewProjectionMatrix, 1, false, gl_modelviewprojection16f);
	if (r_glsl_permutation->loc_ModelViewMatrix >= 0) R_Shader_UniformMatrix4fv(r_glsl_permutation->loc_ModelViewMatrix, 1, false, gl_modelview16f);
	if (r_glsl_permutation->loc_ClientTime >= 0) R_Shader_Uniform1f(r_glsl_permutation->loc_ClientTime, cl.time);
	CHECKGLERROR
}

void R_GLSL_Restart_f(cmd_state_t *cmd)
{
	unsigned int i, limit;
	switch(vid.renderpath)
	{
	case RENDERPATH_METAL:
		// A SEPARATE arm on purpose: the GL arm's GL_Backend_FreeProgram is a
		// qglDeleteProgram through a NULL pointer here. And it is not optional
		// -- this function fires inside R_RenderView on ANY static-parm change
		// (an r_redglow toggle mid-game), so the permutation records, the Metal
		// program table AND the PSO cache must all drop together: PSO keys
		// embed program ids, freed ids get reused, and a stale PSO would
		// silently render the OLD shader under the new id.
		{
			r_glsl_permutation_t *p;
			r_glsl_permutation = NULL;
			limit = (unsigned int)Mem_ExpandableArray_IndexRange(&r_glsl_permutationarray);
			for (i = 0;i < limit;i++)
			{
				if ((p = (r_glsl_permutation_t*)Mem_ExpandableArray_RecordAtIndex(&r_glsl_permutationarray, i)))
					Mem_ExpandableArray_FreeRecord(&r_glsl_permutationarray, (void*)p);
			}
			memset(r_glsl_permutationhash, 0, sizeof(r_glsl_permutationhash));
			Metal_Backend_FreePrograms();
		}
		break;
	case RENDERPATH_GL32:
	case RENDERPATH_GLES2:
		{
			r_glsl_permutation_t *p;
			r_glsl_permutation = NULL;
			limit = (unsigned int)Mem_ExpandableArray_IndexRange(&r_glsl_permutationarray);
			for (i = 0;i < limit;i++)
			{
				if ((p = (r_glsl_permutation_t*)Mem_ExpandableArray_RecordAtIndex(&r_glsl_permutationarray, i)))
				{
					GL_Backend_FreeProgram(p->program);
					Mem_ExpandableArray_FreeRecord(&r_glsl_permutationarray, (void*)p);
				}
			}
			memset(r_glsl_permutationhash, 0, sizeof(r_glsl_permutationhash));
		}
		break;
	}
}

static void R_GLSL_DumpShader_f(cmd_state_t *cmd)
{
	unsigned i;
	int language, mode, dupe;
	char *text;
	shadermodeinfo_t *modeinfo;
	qfile_t *file;

	for (language = 0;language < SHADERLANGUAGE_COUNT;language++)
	{
		modeinfo = shadermodeinfo[language];
		for (mode = 0;mode < SHADERMODE_COUNT;mode++)
		{
			// don't dump the same file multiple times (most or all shaders come from the same file)
			for (dupe = mode - 1;dupe >= 0;dupe--)
				if (!strcmp(modeinfo[mode].filename, modeinfo[dupe].filename))
					break;
			if (dupe >= 0)
				continue;
			text = modeinfo[mode].builtinstring;
			if (!text)
				continue;
			file = FS_OpenRealFile(modeinfo[mode].filename, "w", false);
			if (file)
			{
				FS_Print(file, "/* The engine may define the following macros:\n");
				FS_Print(file, "#define VERTEX_SHADER\n#define GEOMETRY_SHADER\n#define FRAGMENT_SHADER\n");
				for (i = 0;i < SHADERMODE_COUNT;i++)
					FS_Print(file, modeinfo[i].pretext);
				for (i = 0;i < SHADERPERMUTATION_COUNT;i++)
					FS_Print(file, shaderpermutationinfo[i].pretext);
				FS_Print(file, "*/\n");
				FS_Print(file, text);
				FS_Close(file);
				Con_Printf("%s written\n", modeinfo[mode].filename);
			}
			else
				Con_Printf(CON_ERROR "failed to write to %s\n", modeinfo[mode].filename);
		}
	}
}

#ifdef USE_RT_METAL
// METAL.md Phase 5 slice 4. Select the RT composite's own shader and upload the
// reprojection block for the frame about to be drawn. Metal only, by
// construction: the GL path composites through the sidecar's private GLSL
// program and never reaches here. Sibling of R_SetupShader_Generic, which is why
// it lives beside it -- the draw itself stays in RT_SceneComposite, and this
// keeps every piece of shader knowledge in this file.
//
// The term texture is bound by HANDLE rather than as an rtexture_t: it is the
// sidecar's own MTLTexture, adopted into the renderer's table (slice 5-3), so
// there is no rtexture_t to pass. Unit 0 is named as a literal for the same
// reason it is at the call site -- Texture_First heads the sampler walk, so
// nothing can be allocated before it.
void R_SetupShader_RTComposite(unsigned int termhandle, const rt_reproj_t *rp, rtexture_t *depthtexture, int termw, int termh)
{
	R_SetupShader_SetPermutationGLSL(SHADERMODE_RTCOMPOSITE, 0);
	Metal_Backend_TexBind(0, (int)termhandle);
	// TERM UPSAMPLE (rt_metal_term_upsample): the scene depth for the affinity
	// labels, bound by the murk's read-only-attachment precedent (depth writes
	// are off for the view-model mask, so sampling the attached depth is
	// defined and validation-proven). depthtexture NULL -- the direct path, a
	// renderbuffer depth, or the feature off -- binds white instead, because a
	// declared-but-unbound texture is the documented Metal validation trap,
	// and the rtUp.z gate below keeps the values unread.
	if (r_glsl_permutation->tex_Texture_ScreenDepth >= 0)
		R_Mesh_TexBind(r_glsl_permutation->tex_Texture_ScreenDepth, depthtexture ? depthtexture : r_texture_white);
	{
		// Uploaded by name rather than through cached loc_ fields: this runs
		// once per frame on one draw, so the lookup cost is irrelevant, and it
		// keeps nine fields out of r_glsl_permutation_t that no other mode
		// would ever use. A zeroed block (rp NULL) sets rpParams.x to 0, which
		// is the shader's "no reprojection" path and composites the term
		// exactly as slice 5-3 did.
		rt_reproj_t zero;
		const r_glsl_permutation_t *p = r_glsl_permutation;
		int loc;
		if (!rp) { memset(&zero, 0, sizeof(zero)); rp = &zero; }
		if ((loc = R_Shader_GetUniformLocation(p, "rpCurF")) >= 0) R_Shader_Uniform3f(loc, rp->curF[0], rp->curF[1], rp->curF[2]);
		if ((loc = R_Shader_GetUniformLocation(p, "rpCurR")) >= 0) R_Shader_Uniform3f(loc, rp->curR[0], rp->curR[1], rp->curR[2]);
		if ((loc = R_Shader_GetUniformLocation(p, "rpCurU")) >= 0) R_Shader_Uniform3f(loc, rp->curU[0], rp->curU[1], rp->curU[2]);
		if ((loc = R_Shader_GetUniformLocation(p, "rpShF"))  >= 0) R_Shader_Uniform3f(loc, rp->shF[0],  rp->shF[1],  rp->shF[2]);
		if ((loc = R_Shader_GetUniformLocation(p, "rpShR"))  >= 0) R_Shader_Uniform3f(loc, rp->shR[0],  rp->shR[1],  rp->shR[2]);
		if ((loc = R_Shader_GetUniformLocation(p, "rpShU"))  >= 0) R_Shader_Uniform3f(loc, rp->shU[0],  rp->shU[1],  rp->shU[2]);
		if ((loc = R_Shader_GetUniformLocation(p, "rpParams")) >= 0) R_Shader_Uniform4f(loc, rp->params[0], rp->params[1], rp->params[2], rp->params[3]);
		if ((loc = R_Shader_GetUniformLocation(p, "rpCurO")) >= 0) R_Shader_Uniform3f(loc, rp->curO[0], rp->curO[1], rp->curO[2]);
		if ((loc = R_Shader_GetUniformLocation(p, "rpShO"))  >= 0) R_Shader_Uniform3f(loc, rp->shO[0],  rp->shO[1],  rp->shO[2]);
		// TERM UPSAMPLE feeds -- LOCKSTEP rt_metal.m's kCompFS block: same
		// names, same values, the fog upsample's tolerance shaping (smooth =
		// tol/4, edge = tol). Disabled (depthtexture NULL) sends rtUp.z = 0,
		// which is the byte-exact plain path.
		{
			float tol = bound(0.001f, rt_metal_term_upsample_depth.value, 0.5f);
			int upen = (depthtexture != NULL && rt_metal_term_upsample.integer);
			if ((loc = R_Shader_GetUniformLocation(p, "rtUp"))  >= 0) R_Shader_Uniform4f(loc, (float)termw, (float)termh, upen ? tol * 0.25f : 0.0f, tol);
			if ((loc = R_Shader_GetUniformLocation(p, "rtUp2")) >= 0) R_Shader_Uniform4f(loc, (float)r_refdef.view.viewport.x, (float)r_refdef.view.viewport.y, (float)r_refdef.view.viewport.width, (float)r_refdef.view.viewport.height);
			if ((loc = R_Shader_GetUniformLocation(p, "ScreenToDepth")) >= 0) R_Shader_Uniform2f(loc, r_refdef.view.viewport.screentodepth[0], r_refdef.view.viewport.screentodepth[1]);
		}
	}
}
#endif // USE_RT_METAL

// METAL.md Phase 7-1: defined beside the other shader-uniform helpers, far
// below, but first called from here. The argument (7-3) is the output ceiling.
static void R_Shader_SetGammaAnalyticUniforms(float ceiling);
static void R_GammaAnalyticTest_f(struct cmd_state_s *cmd);
void R_SetupShader_Generic(rtexture_t *t, qbool usegamma, qbool notrippy, qbool suppresstexalpha)
{
	uint64_t permutation = 0;
	if (r_trippy.integer && !notrippy)
		permutation |= SHADERPERMUTATION_TRIPPY;
	permutation |= SHADERPERMUTATION_VIEWTINT;
	if (t)
		permutation |= SHADERPERMUTATION_DIFFUSE;
	if (usegamma && v_glslgamma_2d.integer && !vid.sRGB2D && r_texture_gammaramps && !vid_gammatables_trivial)
		permutation |= SHADERPERMUTATION_GAMMARAMPS;
	if (suppresstexalpha)
		permutation |= SHADERPERMUTATION_REFLECTCUBE;
	if (vid.allowalphatocoverage)
		GL_AlphaToCoverage(false);
	switch (vid.renderpath)
	{
	// METAL.md Phase 3 slice 4: Metal FALLS THROUGH to the shared body. That is
	// what slice 1's uniform indirection was built for -- R_Shader_Uniform* and
	// R_Mesh_TexBind dispatch internally, so one tail serves both backends and
	// there is no second copy to drift. The GL text below is untouched, which is
	// what the command digest proves.
	case RENDERPATH_METAL:
	case RENDERPATH_GL32:
	case RENDERPATH_GLES2:
		R_SetupShader_SetPermutationGLSL(SHADERMODE_GENERIC, permutation);
		if (r_glsl_permutation->tex_Texture_First >= 0)
			R_Mesh_TexBind(r_glsl_permutation->tex_Texture_First, t);
		if (r_glsl_permutation->tex_Texture_GammaRamps >= 0)
			R_Mesh_TexBind(r_glsl_permutation->tex_Texture_GammaRamps, r_texture_gammaramps);
		// BEAUTY A4: the soft-particle depth unit is DECLARED in every GENERIC
		// permutation while the parm is on and bound by nobody but the particle
		// batch -- on Metal a declared-but-unbound unit holds whatever the last
		// draw left there (the fifth and sixth shipped defects of that class),
		// so every GENERIC setup binds a placeholder and zeroes the fade; the
		// particle pass re-binds the scene depth per batch (R_SoftParticles_Batch).
		if (r_glsl_permutation->tex_Texture_ScreenDepth >= 0)
			R_Mesh_TexBind(r_glsl_permutation->tex_Texture_ScreenDepth, r_texture_white);
		if (r_glsl_permutation->loc_SoftParticle >= 0)
			R_Shader_Uniform4f(r_glsl_permutation->loc_SoftParticle, 0.0f, 0.0f, 0.0f, 0.0f);
		// BEAUTY A5: the same rule for the refract batch's frame-copy unit.
		if (r_glsl_permutation->tex_Texture_WaterScreen >= 0)
			R_Mesh_TexBind(r_glsl_permutation->tex_Texture_WaterScreen, r_texture_white);
		if (r_glsl_permutation->loc_PartRefract >= 0)
			R_Shader_Uniform4f(r_glsl_permutation->loc_PartRefract, 0.0f, 0.0f, 0.0f, 0.0f);
		// CEILING 1.0, NOT the display headroom (METAL.md Phase 7-3). This is
		// the 2D path -- console, menus, HUD, the loading screen -- and the HUD
		// staying SDR is deliberate: it is the in-frame reference the by-eye EDR
		// test needs, since HUD white then saturates at exactly 1.0 while the
		// scene beside it runs to the headroom.
		R_Shader_SetGammaAnalyticUniforms(1.0f);
		break;
	}
}

void R_SetupShader_Generic_NoTexture(qbool usegamma, qbool notrippy)
{
	R_SetupShader_Generic(NULL, usegamma, notrippy, false);
}

/*
===============
R_SoftParticles_Batch  (BEAUTY A4, 2026-09-16)

Called by the particle callback before each batch it draws, with the fade
distance in world units (0 = no fade: the viewmodel flash, or the feature off).
Binds the main view's scene depth to GENERIC's ScreenDepth unit and feeds the
two uniforms the fragment needs to linearise both its own window depth and the
scene's (the murk's own ScreenToDepth pair) and to find the scene texel under
the fragment (the viewport origin and the depth texture's size -- the depth
texture is the pooled screen target, which can be larger than the viewport).
Everything here is a no-op unless the current permutation declares the unit,
which only happens while cl_particles_soft is on, and it stands down to the
placeholder whenever the scene depth of THIS frame is not published (the bare
direct path, a sub-view, stereo).
===============
*/
void R_SoftParticles_Batch(float fadedist)
{
	qbool live;
	if (!r_glsl_permutation || r_glsl_permutation->loc_SoftParticle < 0)
		return;
	live = fadedist > 0.0f && r_fb.scenedepthvalid && r_fb.scenedepthtexture && r_refdef.view.ismain;
	if (r_glsl_permutation->tex_Texture_ScreenDepth >= 0)
		R_Mesh_TexBind(r_glsl_permutation->tex_Texture_ScreenDepth, live ? r_fb.scenedepthtexture : r_texture_white);
	if (!live)
	{
		R_Shader_Uniform4f(r_glsl_permutation->loc_SoftParticle, 0.0f, 0.0f, 0.0f, 0.0f);
		return;
	}
	R_Shader_Uniform4f(r_glsl_permutation->loc_SoftParticle, 1.0f / fadedist, (float)r_refdef.view.viewport.x, (float)r_refdef.view.viewport.y, 0.0f);
	if (r_glsl_permutation->loc_SoftParticleTex >= 0)
		R_Shader_Uniform4f(r_glsl_permutation->loc_SoftParticleTex, 1.0f / (float)max(1, R_TextureWidth(r_fb.scenedepthtexture)), 1.0f / (float)max(1, R_TextureHeight(r_fb.scenedepthtexture)), r_fb.scenedepth_screentodepth[0], r_fb.scenedepth_screentodepth[1]);
	{
		static int armed = -1;
		if (armed != 1)
		{
			armed = 1;
			Con_DPrintf("soft particles armed (fade %.0f units)\n", fadedist);
		}
	}
}

/*
===============
R_PartRefract_Batch  (BEAUTY A5, 2026-09-16)

Called by the particle callback before each batch: strength in pixels (0 = not
a refract batch, or the feature off), the cell's centre in atlas texcoords and
2/(cell width), so the fragment measures its radial direction from the cell's
own centre. Binds the water's frame copy (taken this view because a refract
particle was queued -- r_fb.refractseen) and stands down to the placeholder
whenever that copy is not this frame's. The uniform is what the fragment tests,
so a zero here is the old GENERIC arithmetic on every other batch.
===============
*/
void R_PartRefract_Batch(float strength, float cs, float ct, float invhalf)
{
	qbool live;
	if (!r_glsl_permutation || r_glsl_permutation->loc_PartRefract < 0)
		return;
	live = strength > 0.0f && r_fb.waterscreen_valid && r_fb.waterscreen && r_refdef.view.ismain;
	if (r_glsl_permutation->tex_Texture_WaterScreen >= 0)
		R_Mesh_TexBind(r_glsl_permutation->tex_Texture_WaterScreen, live ? r_fb.waterscreen : r_texture_white);
	if (!live)
	{
		R_Shader_Uniform4f(r_glsl_permutation->loc_PartRefract, 0.0f, 0.0f, 0.0f, 0.0f);
		return;
	}
	// the strength is authored at 1080p and scales with the viewport height
	R_Shader_Uniform4f(r_glsl_permutation->loc_PartRefract, strength * (float)r_refdef.view.viewport.height / 1080.0f, cs, ct, invhalf);
	if (r_glsl_permutation->loc_PartRefractTex >= 0)
		R_Shader_Uniform4f(r_glsl_permutation->loc_PartRefractTex, 1.0f / (float)max(1, R_TextureWidth(r_fb.waterscreen)), 1.0f / (float)max(1, R_TextureHeight(r_fb.waterscreen)), (float)r_refdef.view.viewport.x, (float)r_refdef.view.viewport.y);
	{
		static int armed = -1;
		if (armed != 1)
		{
			armed = 1;
			Con_DPrintf("particle refraction armed (%.0f px)\n", strength);
		}
	}
}

void R_SetupShader_DepthOrShadow(qbool notrippy, qbool depthrgb, qbool skeletal)
{
	uint64_t permutation = 0;
	if (r_trippy.integer && !notrippy)
		permutation |= SHADERPERMUTATION_TRIPPY;
	if (depthrgb)
		permutation |= SHADERPERMUTATION_DEPTHRGB;
	if (skeletal)
		permutation |= SHADERPERMUTATION_SKELETAL;

	if (vid.allowalphatocoverage)
		GL_AlphaToCoverage(false);
	switch (vid.renderpath)
	{
	// Metal falls through -- R_SetupShader_SetPermutationGLSL dispatches, the
	// skeletal UBO line below is guarded on ubiloc >= 0, which stays -1 on
	// Metal (the compile path never assigns it there). Without this fold the
	// sky depth-mask would draw through whatever program the 2D pass left
	// current, with a stale ortho MVP, writing garbage depth.
	case RENDERPATH_METAL:
	case RENDERPATH_GL32:
	case RENDERPATH_GLES2:
		R_SetupShader_SetPermutationGLSL(SHADERMODE_DEPTH_OR_SHADOW, permutation);
#ifndef USE_GLES2 /* FIXME: GLES3 only */
		if (r_glsl_permutation->ubiloc_Skeletal_Transform12_UniformBlock >= 0 && rsurface.batchskeletaltransform3x4buffer) qglBindBufferRange(GL_UNIFORM_BUFFER, r_glsl_permutation->ubibind_Skeletal_Transform12_UniformBlock, rsurface.batchskeletaltransform3x4buffer->bufferobject, rsurface.batchskeletaltransform3x4offset, rsurface.batchskeletaltransform3x4size);
#endif
		break;
	}
}

#define BLENDFUNC_ALLOWS_COLORMOD      1
#define BLENDFUNC_ALLOWS_FOG           2
#define BLENDFUNC_ALLOWS_FOG_HACK0     4
#define BLENDFUNC_ALLOWS_FOG_HACKALPHA 8
#define BLENDFUNC_ALLOWS_ANYFOG        (BLENDFUNC_ALLOWS_FOG | BLENDFUNC_ALLOWS_FOG_HACK0 | BLENDFUNC_ALLOWS_FOG_HACKALPHA)
static int R_BlendFuncFlags(int src, int dst)
{
	int r = 0;

	// a blendfunc allows colormod if:
	// a) it can never keep the destination pixel invariant, or
	// b) it can keep the destination pixel invariant, and still can do so if colormodded
	// this is to prevent unintended side effects from colormod

	// a blendfunc allows fog if:
	// blend(fog(src), fog(dst)) == fog(blend(src, dst))
	// this is to prevent unintended side effects from fog

	// these checks are the output of fogeval.pl

	r |= BLENDFUNC_ALLOWS_COLORMOD;
	if(src == GL_DST_ALPHA && dst == GL_ONE) r |= BLENDFUNC_ALLOWS_FOG_HACK0;
	if(src == GL_DST_ALPHA && dst == GL_ONE_MINUS_DST_ALPHA) r |= BLENDFUNC_ALLOWS_FOG;
	if(src == GL_DST_COLOR && dst == GL_ONE_MINUS_SRC_ALPHA) r &= ~BLENDFUNC_ALLOWS_COLORMOD;
	if(src == GL_DST_COLOR && dst == GL_ONE_MINUS_SRC_COLOR) r |= BLENDFUNC_ALLOWS_FOG;
	if(src == GL_DST_COLOR && dst == GL_SRC_ALPHA) r &= ~BLENDFUNC_ALLOWS_COLORMOD;
	if(src == GL_DST_COLOR && dst == GL_SRC_COLOR) r &= ~BLENDFUNC_ALLOWS_COLORMOD;
	if(src == GL_DST_COLOR && dst == GL_ZERO) r &= ~BLENDFUNC_ALLOWS_COLORMOD;
	if(src == GL_ONE && dst == GL_ONE) r |= BLENDFUNC_ALLOWS_FOG_HACK0;
	if(src == GL_ONE && dst == GL_ONE_MINUS_SRC_ALPHA) r |= BLENDFUNC_ALLOWS_FOG_HACKALPHA;
	if(src == GL_ONE && dst == GL_ZERO) r |= BLENDFUNC_ALLOWS_FOG;
	if(src == GL_ONE_MINUS_DST_ALPHA && dst == GL_DST_ALPHA) r |= BLENDFUNC_ALLOWS_FOG;
	if(src == GL_ONE_MINUS_DST_ALPHA && dst == GL_ONE) r |= BLENDFUNC_ALLOWS_FOG_HACK0;
	if(src == GL_ONE_MINUS_DST_COLOR && dst == GL_SRC_COLOR) r |= BLENDFUNC_ALLOWS_FOG;
	if(src == GL_ONE_MINUS_SRC_ALPHA && dst == GL_ONE) r |= BLENDFUNC_ALLOWS_FOG_HACK0;
	if(src == GL_ONE_MINUS_SRC_ALPHA && dst == GL_SRC_ALPHA) r |= BLENDFUNC_ALLOWS_FOG;
	if(src == GL_ONE_MINUS_SRC_ALPHA && dst == GL_SRC_COLOR) r &= ~BLENDFUNC_ALLOWS_COLORMOD;
	if(src == GL_ONE_MINUS_SRC_COLOR && dst == GL_SRC_COLOR) r &= ~BLENDFUNC_ALLOWS_COLORMOD;
	if(src == GL_SRC_ALPHA && dst == GL_ONE) r |= BLENDFUNC_ALLOWS_FOG_HACK0;
	if(src == GL_SRC_ALPHA && dst == GL_ONE_MINUS_SRC_ALPHA) r |= BLENDFUNC_ALLOWS_FOG;
	if(src == GL_ZERO && dst == GL_ONE) r |= BLENDFUNC_ALLOWS_FOG;
	if(src == GL_ZERO && dst == GL_SRC_COLOR) r &= ~BLENDFUNC_ALLOWS_COLORMOD;

	return r;
}

// the murk's 3D noise volume, reused by the lava boil (defined with the
// volumetrics below; idempotent, NULL until first created). Not RT-gated:
// the lava boil needs it on every build
static rtexture_t *R_Volumetric_GetNoiseTexture(void);
// LIQUIDFOG: the baked world field, for the liquid fade's one fetch. A
// SEPARATE accessor from R_Volumetric_GetField and deliberately so -- see its
// definition beside the field itself; the short version is that this one must
// never trigger a bake.
static rtexture_t *R_Volumetric_LiquidFadeField(void);
// LIQUIDFOG: uploads the fade's uniforms and returns the strength it applied
// (0 = inert on this batch), so the caller can stand the classic fog down on
// exactly the batches the fade owns.
static float R_Volumetric_LiquidFadeUniforms(qbool isliquid);
// ...and the map-fog override the fade's tint reads. The OTHER forward
// declaration of this, beside R_Volumetric_GetFogKernelParams, sits inside a
// USE_RT_METAL block -- so it does not exist on the dedicated-server build,
// where gl_rmain.c still compiles. Caught by `make sv-release`, which is the
// only thing that ever catches this shape (guard the FEATURE, never the
// platform -- and then re-run the server link, because nothing else will
// tell you).
qbool M5_MapFogColor(float *out);
#ifdef USE_RT_METAL
// texture units holding the RT term rectangle for the liquid fix; the rectangle
// target is invisible to the backend's 2D/3D/cube caches, so nothing else would
// ever clear those bindings -- the transparent pass unbinds them explicitly
static unsigned int r_rtliquid_boundunits;
static void R_RTLiquid_UnbindAll(void)
{
	int u;
	if (!r_rtliquid_boundunits)
		return;
	for (u = 0; u < 32; u++)
		if (r_rtliquid_boundunits & (1u << u))
		{
			GL_ActiveTexture(u);
			qglBindTexture(GL_TEXTURE_RECTANGLE, 0);
		}
	r_rtliquid_boundunits = 0;
}
#endif

// Gibs and severed heads are red, and r_redglow would happily set flying viscera
// alight -- which is not what "red should burn" means. Matched by model name, the
// same way RT_IsLightCoreModel (cl_screen.c) picks out torch flames. progs/h_* is
// safe as a prefix: every one of them is a thrown head, h_mega.mdl included, which
// is the Enforcer's (qc/enforcer.qc).
static qbool R_IsGibModel(const model_t *model)
{
	if (!model || !model->name[0])
		return false;
	return !strncmp(model->name, "progs/gib", 9)
	    || !strncmp(model->name, "progs/h_", 8)
	    || !strcmp(model->name, "progs/zom_gib.mdl");
}

void R_SetupShader_Surface(const float rtlightambient[3], const float rtlightdiffuse[3], const float rtlightspecular[3], rsurfacepass_t rsurfacepass, int texturenumsurfaces, const msurface_t **texturesurfacelist, void *surfacewaterplane, qbool notrippy, qbool ui)
{
	// select a permutation of the lighting shader appropriate to this
	// combination of texture, entity, light source, and fogging, only use the
	// minimum features necessary to avoid wasting rendering time in the
	// fragment shader on features that are not being used
	uint64_t permutation = 0;
	unsigned int mode = 0;
	int blendfuncflags;
	texture_t *t = rsurface.texture;
	float m16f[16];
	// LIQUIDFOG: the fade strength this batch got, which is also what tells the
	// classic-fog line whether to stand down (0 on every batch but a blended
	// liquid, so it is the RTLiquid no-op shape again)
	float liquidfade = 0.0f;
	matrix4x4_t tempmatrix;
	r_waterstate_waterplane_t *waterplane = (r_waterstate_waterplane_t *)surfacewaterplane;
	// EMISSION SURVIVES WALL LIGHTING (rt_metal_glowpass; see R_RTGlow_Pass).
	// glowdefer: this batch draws BEFORE an RT composite that will multiply the
	// whole frame, so its emission (Color_Glow, RedGlow) is withheld here and
	// re-added at authored brightness by the glow pass. Applies to the scene
	// and rtlight passes on opaque batches only: BLENDED batches draw after the
	// composite (the pending flag is already down by then, but the test keeps
	// r_transparent 0's inline blended draws honest too), and the view weapon
	// escapes the multiply via the composite's depth mask.
	qbool glowdefer = r_fb.rtglowpending && !ui
	 && (rsurfacepass == RSURFPASS_BASE || rsurfacepass == RSURFPASS_RTLIGHT)
	 && !(t->currentmaterialflags & MATERIALFLAG_BLENDED)
	 && !(rsurface.ent_flags & RENDER_VIEWMODEL);
	if (r_trippy.integer && !notrippy)
		permutation |= SHADERPERMUTATION_TRIPPY;
	if (t->currentmaterialflags & MATERIALFLAG_ALPHATEST)
		permutation |= SHADERPERMUTATION_ALPHAKILL;
	if (t->currentmaterialflags & MATERIALFLAG_OCCLUDE)
		permutation |= SHADERPERMUTATION_OCCLUDE;
	if (t->r_water_waterscroll[0] && t->r_water_waterscroll[1])
		permutation |= SHADERPERMUTATION_NORMALMAPSCROLLBLEND; // todo: make generic
	if (rsurfacepass == RSURFPASS_BACKGROUND)
	{
		// distorted background
		if (t->currentmaterialflags & MATERIALFLAG_WATERSHADER)
		{
			mode = SHADERMODE_WATER;
			if (t->currentmaterialflags & MATERIALFLAG_ALPHAGEN_VERTEX)
				permutation |= SHADERPERMUTATION_ALPHAGEN_VERTEX;
			if((r_wateralpha.value < 1) && (t->currentmaterialflags & MATERIALFLAG_WATERALPHA))
			{
				// this is the right thing to do for wateralpha
				GL_BlendFunc(GL_ONE, GL_ZERO);
				blendfuncflags = R_BlendFuncFlags(GL_ONE, GL_ZERO);
			}
			else
			{
				// this is the right thing to do for entity alpha
				GL_BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				blendfuncflags = R_BlendFuncFlags(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			}
		}
		else if (t->currentmaterialflags & MATERIALFLAG_REFRACTION)
		{
			mode = SHADERMODE_REFRACTION;
			if (t->currentmaterialflags & MATERIALFLAG_ALPHAGEN_VERTEX)
				permutation |= SHADERPERMUTATION_ALPHAGEN_VERTEX;
			GL_BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			blendfuncflags = R_BlendFuncFlags(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		}
		else
		{
			mode = SHADERMODE_GENERIC;
			permutation |= SHADERPERMUTATION_DIFFUSE | SHADERPERMUTATION_ALPHAKILL;
			GL_BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			blendfuncflags = R_BlendFuncFlags(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		}
		if (vid.allowalphatocoverage)
			GL_AlphaToCoverage(false);
	}
	else if (rsurfacepass == RSURFPASS_DEFERREDGEOMETRY)
	{
		if (r_glsl_offsetmapping.integer && ((R_TextureFlags(t->nmaptexture) & TEXF_ALPHA) || t->offsetbias != 0.0f))
		{
			switch(t->offsetmapping)
			{
			case OFFSETMAPPING_LINEAR: permutation |= SHADERPERMUTATION_OFFSETMAPPING;break;
			case OFFSETMAPPING_RELIEF: permutation |= SHADERPERMUTATION_OFFSETMAPPING | SHADERPERMUTATION_OFFSETMAPPING_RELIEFMAPPING;break;
			case OFFSETMAPPING_DEFAULT: permutation |= SHADERPERMUTATION_OFFSETMAPPING;if (r_glsl_offsetmapping_reliefmapping.integer) permutation |= SHADERPERMUTATION_OFFSETMAPPING_RELIEFMAPPING;break;
			case OFFSETMAPPING_OFF: break;
			}
		}
		if (t->currentmaterialflags & MATERIALFLAG_VERTEXTEXTUREBLEND)
			permutation |= SHADERPERMUTATION_VERTEXTEXTUREBLEND;
		// normalmap (deferred prepass), may use alpha test on diffuse
		mode = SHADERMODE_DEFERREDGEOMETRY;
		GL_BlendFunc(GL_ONE, GL_ZERO);
		blendfuncflags = R_BlendFuncFlags(GL_ONE, GL_ZERO);
		if (vid.allowalphatocoverage)
			GL_AlphaToCoverage(false);
	}
	else if (rsurfacepass == RSURFPASS_RTLIGHT)
	{
		if (r_glsl_offsetmapping.integer && ((R_TextureFlags(t->nmaptexture) & TEXF_ALPHA) || t->offsetbias != 0.0f))
		{
			switch(t->offsetmapping)
			{
			case OFFSETMAPPING_LINEAR: permutation |= SHADERPERMUTATION_OFFSETMAPPING;break;
			case OFFSETMAPPING_RELIEF: permutation |= SHADERPERMUTATION_OFFSETMAPPING | SHADERPERMUTATION_OFFSETMAPPING_RELIEFMAPPING;break;
			case OFFSETMAPPING_DEFAULT: permutation |= SHADERPERMUTATION_OFFSETMAPPING;if (r_glsl_offsetmapping_reliefmapping.integer) permutation |= SHADERPERMUTATION_OFFSETMAPPING_RELIEFMAPPING;break;
			case OFFSETMAPPING_OFF: break;
			}
		}
		if (t->currentmaterialflags & MATERIALFLAG_VERTEXTEXTUREBLEND)
			permutation |= SHADERPERMUTATION_VERTEXTEXTUREBLEND;
		if (t->currentmaterialflags & MATERIALFLAG_ALPHAGEN_VERTEX)
			permutation |= SHADERPERMUTATION_ALPHAGEN_VERTEX;
		// light source
		mode = SHADERMODE_LIGHTSOURCE;
		if (rsurface.rtlight->currentcubemap != r_texture_whitecube)
			permutation |= SHADERPERMUTATION_CUBEFILTER;
		if (VectorLength2(rtlightdiffuse) > 0)
			permutation |= SHADERPERMUTATION_DIFFUSE;
		if (VectorLength2(rtlightspecular) > 0)
			permutation |= SHADERPERMUTATION_SPECULAR | SHADERPERMUTATION_DIFFUSE;
		if (r_refdef.fogenabled)
			permutation |= r_texture_fogheighttexture ? SHADERPERMUTATION_FOGHEIGHTTEXTURE : (r_refdef.fogplaneviewabove ? SHADERPERMUTATION_FOGOUTSIDE : SHADERPERMUTATION_FOGINSIDE);
		if (t->colormapping)
			permutation |= SHADERPERMUTATION_COLORMAPPING;
		if (r_shadow_usingshadowmap2d)
		{
			permutation |= SHADERPERMUTATION_SHADOWMAP2D;
			if(r_shadow_shadowmapvsdct)
				permutation |= SHADERPERMUTATION_SHADOWMAPVSDCT;

			if (r_shadow_shadowmap2ddepthbuffer)
				permutation |= SHADERPERMUTATION_DEPTHRGB;
		}
		if (t->reflectmasktexture)
			permutation |= SHADERPERMUTATION_REFLECTCUBE;
		GL_BlendFunc(GL_SRC_ALPHA, GL_ONE);
		blendfuncflags = R_BlendFuncFlags(GL_SRC_ALPHA, GL_ONE);
		if (vid.allowalphatocoverage)
			GL_AlphaToCoverage(false);
	}
	else if (t->currentmaterialflags & MATERIALFLAG_LIGHTGRID)
	{
		if (r_glsl_offsetmapping.integer && ((R_TextureFlags(t->nmaptexture) & TEXF_ALPHA) || t->offsetbias != 0.0f))
		{
			switch(t->offsetmapping)
			{
			case OFFSETMAPPING_LINEAR: permutation |= SHADERPERMUTATION_OFFSETMAPPING;break;
			case OFFSETMAPPING_RELIEF: permutation |= SHADERPERMUTATION_OFFSETMAPPING | SHADERPERMUTATION_OFFSETMAPPING_RELIEFMAPPING;break;
			case OFFSETMAPPING_DEFAULT: permutation |= SHADERPERMUTATION_OFFSETMAPPING;if (r_glsl_offsetmapping_reliefmapping.integer) permutation |= SHADERPERMUTATION_OFFSETMAPPING_RELIEFMAPPING;break;
			case OFFSETMAPPING_OFF: break;
			}
		}
		if (t->currentmaterialflags & MATERIALFLAG_VERTEXTEXTUREBLEND)
			permutation |= SHADERPERMUTATION_VERTEXTEXTUREBLEND;
		if (t->currentmaterialflags & MATERIALFLAG_ALPHAGEN_VERTEX)
			permutation |= SHADERPERMUTATION_ALPHAGEN_VERTEX;
		// directional model lighting
		mode = SHADERMODE_LIGHTGRID;
		if ((t->glowtexture || t->backgroundglowtexture) && r_hdr_glowintensity.value > 0 && !gl_lightmaps.integer)
			permutation |= SHADERPERMUTATION_GLOW;
		permutation |= SHADERPERMUTATION_DIFFUSE;
		if (t->glosstexture || t->backgroundglosstexture)
			permutation |= SHADERPERMUTATION_SPECULAR;
		if (r_refdef.fogenabled)
			permutation |= r_texture_fogheighttexture ? SHADERPERMUTATION_FOGHEIGHTTEXTURE : (r_refdef.fogplaneviewabove ? SHADERPERMUTATION_FOGOUTSIDE : SHADERPERMUTATION_FOGINSIDE);
		if (t->colormapping)
			permutation |= SHADERPERMUTATION_COLORMAPPING;
		if (r_shadow_usingshadowmaportho && !(rsurface.ent_flags & RENDER_NOSELFSHADOW))
		{
			permutation |= SHADERPERMUTATION_SHADOWMAPORTHO;
			permutation |= SHADERPERMUTATION_SHADOWMAP2D;

			if (r_shadow_shadowmap2ddepthbuffer)
				permutation |= SHADERPERMUTATION_DEPTHRGB;
		}
		if (t->currentmaterialflags & MATERIALFLAG_REFLECTION)
			permutation |= SHADERPERMUTATION_REFLECTION;
		if (r_shadow_usingdeferredprepass && !(t->currentmaterialflags & MATERIALFLAG_BLENDED))
			permutation |= SHADERPERMUTATION_DEFERREDLIGHTMAP;
		if (t->reflectmasktexture)
			permutation |= SHADERPERMUTATION_REFLECTCUBE;
		if (r_shadow_bouncegrid_state.texture && cl.csqc_vidvars.drawworld && !notrippy)
		{
			permutation |= SHADERPERMUTATION_BOUNCEGRID;
			if (r_shadow_bouncegrid_state.directional)
				permutation |= SHADERPERMUTATION_BOUNCEGRIDDIRECTIONAL;
		}
		GL_BlendFunc(t->currentblendfunc[0], t->currentblendfunc[1]);
		blendfuncflags = R_BlendFuncFlags(t->currentblendfunc[0], t->currentblendfunc[1]);
		// when using alphatocoverage, we don't need alphakill
		if (vid.allowalphatocoverage)
		{
			if (r_transparent_alphatocoverage.integer)
			{
				GL_AlphaToCoverage((t->currentmaterialflags & MATERIALFLAG_ALPHATEST) != 0);
				permutation &= ~SHADERPERMUTATION_ALPHAKILL;
			}
			else
				GL_AlphaToCoverage(false);
		}
	}
	else if (t->currentmaterialflags & MATERIALFLAG_MODELLIGHT)
	{
		if (r_glsl_offsetmapping.integer && ((R_TextureFlags(t->nmaptexture) & TEXF_ALPHA) || t->offsetbias != 0.0f))
		{
			switch(t->offsetmapping)
			{
			case OFFSETMAPPING_LINEAR: permutation |= SHADERPERMUTATION_OFFSETMAPPING;break;
			case OFFSETMAPPING_RELIEF: permutation |= SHADERPERMUTATION_OFFSETMAPPING | SHADERPERMUTATION_OFFSETMAPPING_RELIEFMAPPING;break;
			case OFFSETMAPPING_DEFAULT: permutation |= SHADERPERMUTATION_OFFSETMAPPING;if (r_glsl_offsetmapping_reliefmapping.integer) permutation |= SHADERPERMUTATION_OFFSETMAPPING_RELIEFMAPPING;break;
			case OFFSETMAPPING_OFF: break;
			}
		}
		if (t->currentmaterialflags & MATERIALFLAG_VERTEXTEXTUREBLEND)
			permutation |= SHADERPERMUTATION_VERTEXTEXTUREBLEND;
		if (t->currentmaterialflags & MATERIALFLAG_ALPHAGEN_VERTEX)
			permutation |= SHADERPERMUTATION_ALPHAGEN_VERTEX;
		// directional model lighting
		mode = SHADERMODE_LIGHTDIRECTION;
		if ((t->glowtexture || t->backgroundglowtexture) && r_hdr_glowintensity.value > 0 && !gl_lightmaps.integer)
			permutation |= SHADERPERMUTATION_GLOW;
		if (VectorLength2(t->render_modellight_diffuse))
			permutation |= SHADERPERMUTATION_DIFFUSE;
		if (VectorLength2(t->render_modellight_specular) > 0)
			permutation |= SHADERPERMUTATION_SPECULAR;
		if (r_refdef.fogenabled)
			permutation |= r_texture_fogheighttexture ? SHADERPERMUTATION_FOGHEIGHTTEXTURE : (r_refdef.fogplaneviewabove ? SHADERPERMUTATION_FOGOUTSIDE : SHADERPERMUTATION_FOGINSIDE);
		if (t->colormapping)
			permutation |= SHADERPERMUTATION_COLORMAPPING;
		if (r_shadow_usingshadowmaportho && !(rsurface.ent_flags & RENDER_NOSELFSHADOW))
		{
			permutation |= SHADERPERMUTATION_SHADOWMAPORTHO;
			permutation |= SHADERPERMUTATION_SHADOWMAP2D;

			if (r_shadow_shadowmap2ddepthbuffer)
				permutation |= SHADERPERMUTATION_DEPTHRGB;
		}
		if (t->currentmaterialflags & MATERIALFLAG_REFLECTION)
			permutation |= SHADERPERMUTATION_REFLECTION;
		if (r_shadow_usingdeferredprepass && !(t->currentmaterialflags & MATERIALFLAG_BLENDED))
			permutation |= SHADERPERMUTATION_DEFERREDLIGHTMAP;
		if (t->reflectmasktexture)
			permutation |= SHADERPERMUTATION_REFLECTCUBE;
		if (r_shadow_bouncegrid_state.texture && cl.csqc_vidvars.drawworld && !notrippy)
		{
			permutation |= SHADERPERMUTATION_BOUNCEGRID;
			if (r_shadow_bouncegrid_state.directional)
				permutation |= SHADERPERMUTATION_BOUNCEGRIDDIRECTIONAL;
		}
		GL_BlendFunc(t->currentblendfunc[0], t->currentblendfunc[1]);
		blendfuncflags = R_BlendFuncFlags(t->currentblendfunc[0], t->currentblendfunc[1]);
		// when using alphatocoverage, we don't need alphakill
		if (vid.allowalphatocoverage)
		{
			if (r_transparent_alphatocoverage.integer)
			{
				GL_AlphaToCoverage((t->currentmaterialflags & MATERIALFLAG_ALPHATEST) != 0);
				permutation &= ~SHADERPERMUTATION_ALPHAKILL;
			}
			else
				GL_AlphaToCoverage(false);
		}
	}
	else
	{
		if (r_glsl_offsetmapping.integer && ((R_TextureFlags(t->nmaptexture) & TEXF_ALPHA) || t->offsetbias != 0.0f))
		{
			switch(t->offsetmapping)
			{
			case OFFSETMAPPING_LINEAR: permutation |= SHADERPERMUTATION_OFFSETMAPPING;break;
			case OFFSETMAPPING_RELIEF: permutation |= SHADERPERMUTATION_OFFSETMAPPING | SHADERPERMUTATION_OFFSETMAPPING_RELIEFMAPPING;break;
			case OFFSETMAPPING_DEFAULT: permutation |= SHADERPERMUTATION_OFFSETMAPPING;if (r_glsl_offsetmapping_reliefmapping.integer) permutation |= SHADERPERMUTATION_OFFSETMAPPING_RELIEFMAPPING;break;
			case OFFSETMAPPING_OFF: break;
			}
		}
		if (t->currentmaterialflags & MATERIALFLAG_VERTEXTEXTUREBLEND)
			permutation |= SHADERPERMUTATION_VERTEXTEXTUREBLEND;
		if (t->currentmaterialflags & MATERIALFLAG_ALPHAGEN_VERTEX)
			permutation |= SHADERPERMUTATION_ALPHAGEN_VERTEX;
		// lightmapped wall
		if ((t->glowtexture || t->backgroundglowtexture) && r_hdr_glowintensity.value > 0 && !gl_lightmaps.integer)
			permutation |= SHADERPERMUTATION_GLOW;
		if (r_refdef.fogenabled && !ui)
			permutation |= r_texture_fogheighttexture ? SHADERPERMUTATION_FOGHEIGHTTEXTURE : (r_refdef.fogplaneviewabove ? SHADERPERMUTATION_FOGOUTSIDE : SHADERPERMUTATION_FOGINSIDE);
		if (t->colormapping)
			permutation |= SHADERPERMUTATION_COLORMAPPING;
		if (r_shadow_usingshadowmaportho && !(rsurface.ent_flags & RENDER_NOSELFSHADOW))
		{
			permutation |= SHADERPERMUTATION_SHADOWMAPORTHO;
			permutation |= SHADERPERMUTATION_SHADOWMAP2D;

			if (r_shadow_shadowmap2ddepthbuffer)
				permutation |= SHADERPERMUTATION_DEPTHRGB;
		}
		if (t->currentmaterialflags & MATERIALFLAG_REFLECTION)
			permutation |= SHADERPERMUTATION_REFLECTION;
		if (r_shadow_usingdeferredprepass && !(t->currentmaterialflags & MATERIALFLAG_BLENDED))
			permutation |= SHADERPERMUTATION_DEFERREDLIGHTMAP;
		if (t->reflectmasktexture)
			permutation |= SHADERPERMUTATION_REFLECTCUBE;
		if (r_glsl_deluxemapping.integer >= 1 && rsurface.uselightmaptexture && r_refdef.scene.worldmodel && r_refdef.scene.worldmodel->brushq3.deluxemapping)
		{
			// deluxemapping (light direction texture)
			if (rsurface.uselightmaptexture && r_refdef.scene.worldmodel && r_refdef.scene.worldmodel->brushq3.deluxemapping && r_refdef.scene.worldmodel->brushq3.deluxemapping_modelspace)
				mode = SHADERMODE_LIGHTDIRECTIONMAP_MODELSPACE;
			else
				mode = SHADERMODE_LIGHTDIRECTIONMAP_TANGENTSPACE;
			permutation |= SHADERPERMUTATION_DIFFUSE;
			if (VectorLength2(t->render_lightmap_specular) > 0)
				permutation |= SHADERPERMUTATION_SPECULAR | SHADERPERMUTATION_DIFFUSE;
		}
		else if (r_glsl_deluxemapping.integer >= 2)
		{
			// fake deluxemapping (uniform light direction in tangentspace)
			if (rsurface.uselightmaptexture)
				mode = SHADERMODE_LIGHTDIRECTIONMAP_FORCED_LIGHTMAP;
			else
				mode = SHADERMODE_LIGHTDIRECTIONMAP_FORCED_VERTEXCOLOR;
			permutation |= SHADERPERMUTATION_DIFFUSE;
			if (VectorLength2(t->render_lightmap_specular) > 0)
				permutation |= SHADERPERMUTATION_SPECULAR | SHADERPERMUTATION_DIFFUSE;
		}
		else if (rsurface.uselightmaptexture)
		{
			// ordinary lightmapping (q1bsp, q3bsp)
			mode = SHADERMODE_LIGHTMAP;
		}
		else
		{
			// ordinary vertex coloring (q3bsp)
			mode = SHADERMODE_VERTEXCOLOR;
		}
		if (r_shadow_bouncegrid_state.texture && cl.csqc_vidvars.drawworld && !notrippy)
		{
			permutation |= SHADERPERMUTATION_BOUNCEGRID;
			if (r_shadow_bouncegrid_state.directional)
				permutation |= SHADERPERMUTATION_BOUNCEGRIDDIRECTIONAL;
		}
		GL_BlendFunc(t->currentblendfunc[0], t->currentblendfunc[1]);
		blendfuncflags = R_BlendFuncFlags(t->currentblendfunc[0], t->currentblendfunc[1]);
		// when using alphatocoverage, we don't need alphakill
		if (vid.allowalphatocoverage)
		{
			if (r_transparent_alphatocoverage.integer)
			{
				GL_AlphaToCoverage((t->currentmaterialflags & MATERIALFLAG_ALPHATEST) != 0);
				permutation &= ~SHADERPERMUTATION_ALPHAKILL;
			}
			else
				GL_AlphaToCoverage(false);
		}
	}
	// The glow re-add pass is ADDITIVE whatever the material said: GL_ONE/GL_ONE
	// lands in the FOG_HACK0 class below, so classic fog darkens the re-added
	// emission without tinting it -- the standard additive-pass treatment.
	if (r_rtglowpass_active)
	{
		GL_BlendFunc(GL_ONE, GL_ONE);
		blendfuncflags = R_BlendFuncFlags(GL_ONE, GL_ONE);
	}
	if(!(blendfuncflags & BLENDFUNC_ALLOWS_ANYFOG))
		permutation &= ~(SHADERPERMUTATION_FOGHEIGHTTEXTURE | SHADERPERMUTATION_FOGOUTSIDE | SHADERPERMUTATION_FOGINSIDE);
	if(blendfuncflags & BLENDFUNC_ALLOWS_FOG_HACKALPHA && !ui)
		permutation |= SHADERPERMUTATION_FOGALPHAHACK;
	switch(vid.renderpath)
	{
	// Metal falls through to the shared body -- see the note in
	// R_SetupShader_Generic. The two raw-GL calls further down are both behind
	// `loc >= 0` guards on uniforms the MSL does not declare (the skeletal
	// uniform block, and the RT-liquids rectangle bind), so they are unreachable
	// on this path by the same mechanism that keeps them unreachable on a GL
	// permutation that lacks them.
	case RENDERPATH_METAL:
	case RENDERPATH_GL32:
	case RENDERPATH_GLES2:
		RSurf_PrepareVerticesForBatch(BATCHNEED_ARRAY_VERTEX | BATCHNEED_ARRAY_NORMAL | BATCHNEED_ARRAY_VECTOR | (rsurface.modellightmapcolor4f ? BATCHNEED_ARRAY_VERTEXCOLOR : 0) | BATCHNEED_ARRAY_TEXCOORD | (rsurface.uselightmaptexture ? BATCHNEED_ARRAY_LIGHTMAP : 0) | BATCHNEED_ALLOWMULTIDRAW, texturenumsurfaces, texturesurfacelist);
		RSurf_UploadBuffersForBatch();
		// this has to be after RSurf_PrepareVerticesForBatch
		if (rsurface.batchskeletaltransform3x4buffer)
			permutation |= SHADERPERMUTATION_SKELETAL;
		R_SetupShader_SetPermutationGLSL(mode, permutation);
#ifndef USE_GLES2 /* FIXME: GLES3 only */
		if (r_glsl_permutation->ubiloc_Skeletal_Transform12_UniformBlock >= 0 && rsurface.batchskeletaltransform3x4buffer) qglBindBufferRange(GL_UNIFORM_BUFFER, r_glsl_permutation->ubibind_Skeletal_Transform12_UniformBlock, rsurface.batchskeletaltransform3x4buffer->bufferobject, rsurface.batchskeletaltransform3x4offset, rsurface.batchskeletaltransform3x4size);
#endif
		if (r_glsl_permutation->loc_ModelToReflectCube >= 0) {Matrix4x4_ToArrayFloatGL(&rsurface.matrix, m16f);R_Shader_UniformMatrix4fv(r_glsl_permutation->loc_ModelToReflectCube, 1, false, m16f);}
		// LIQUIDFOG: model -> world for the fade's world-space field lookup. The
		// same rsurface.matrix the line above hands ModelToReflectCube, under
		// its own name; a liquid on a brush model (a door, a lift) has a matrix
		// that is not the identity and would sample the field in the wrong room.
		if (r_glsl_permutation->loc_ModelToWorld >= 0) {Matrix4x4_ToArrayFloatGL(&rsurface.matrix, m16f);R_Shader_UniformMatrix4fv(r_glsl_permutation->loc_ModelToWorld, 1, false, m16f);}
		if (mode == SHADERMODE_LIGHTSOURCE)
		{
			if (r_glsl_permutation->loc_ModelToLight >= 0) {Matrix4x4_ToArrayFloatGL(&rsurface.entitytolight, m16f);R_Shader_UniformMatrix4fv(r_glsl_permutation->loc_ModelToLight, 1, false, m16f);}
			if (r_glsl_permutation->loc_LightPosition >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_LightPosition, rsurface.entitylightorigin[0], rsurface.entitylightorigin[1], rsurface.entitylightorigin[2]);
			if (r_glsl_permutation->loc_LightColor >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_LightColor, 1, 1, 1); // DEPRECATED
			if (r_glsl_permutation->loc_Color_Ambient >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_Color_Ambient, rtlightambient[0], rtlightambient[1], rtlightambient[2]);
			if (r_glsl_permutation->loc_Color_Diffuse >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_Color_Diffuse, rtlightdiffuse[0], rtlightdiffuse[1], rtlightdiffuse[2]);
			if (r_glsl_permutation->loc_Color_Specular >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_Color_Specular, rtlightspecular[0], rtlightspecular[1], rtlightspecular[2]);

			// additive passes are only darkened by fog, not tinted
			if (r_glsl_permutation->loc_FogColor >= 0)
				R_Shader_Uniform3f(r_glsl_permutation->loc_FogColor, 0, 0, 0);
			if (r_glsl_permutation->loc_SpecularPower >= 0) R_Shader_Uniform1f(r_glsl_permutation->loc_SpecularPower, t->specularpower * (r_shadow_glossexact.integer ? 0.25f : 1.0f) - 1.0f);
			// THE OPTIONAL SAMPLERS ON THE ADDITIVE PASS (2026-09-13). The static
			// parms -- the lava boil / water swirl noise volume, the liquid fade's
			// baked field, the RT liquid term and pair, and WATERSURFACE's scene
			// depth and frame copy -- are compiled into EVERY surface permutation,
			// this mode's included, and every one of them is bound in the other arm
			// below and NEVER here. That is correct as far as the look goes (none
			// of those effects belongs on an additive rtlight pass: the arming
			// uniforms are never written for this program, so the shader takes its
			// no-op branches), but a unit the shader DECLARES and nothing binds
			// holds whatever the LAST permutation left there, and Metal API
			// validation checks the TYPE. It passed for months by layout luck: the
			// volumes sat at the same indices in every mode. WATERSURFACE's two
			// 2D slots moved them along, a mode-11 entity batch then left a 3D
			// texture at this mode's ScreenDepth index and a 2D one at its
			// VolumeField index, and the attract demo's first muzzle flash ABORTED
			// under validation at plain defaults. Bind a correctly-typed
			// placeholder to each declared unit so the pass is well-formed
			// whatever drew before it. Seb's own configuration never reached this:
			// wall lighting strips RENDER_LIGHT, so no dlight pass draws there.
			if (r_glsl_permutation->tex_Texture_ScreenDepth >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_ScreenDepth, r_texture_white);
			if (r_glsl_permutation->tex_Texture_WaterScreen >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_WaterScreen, r_texture_white);
			if (r_glsl_permutation->tex_Texture_VolumeNoise >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_VolumeNoise, r_texture_white3d);
			if (r_glsl_permutation->tex_Texture_VolumeField >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_VolumeField, r_texture_white3d);
			if (r_glsl_permutation->tex_Texture_RTTerm      >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_RTTerm,      r_texture_white);
			if (r_glsl_permutation->tex_Texture_RTLiquid    >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_RTLiquid,    r_texture_white);
		}
		else
		{
			if (mode == SHADERMODE_FLATCOLOR)
			{
				if (r_glsl_permutation->loc_Color_Ambient >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_Color_Ambient, t->render_modellight_ambient[0], t->render_modellight_ambient[1], t->render_modellight_ambient[2]);
			}
			else if (mode == SHADERMODE_LIGHTGRID)
			{
				if (r_glsl_permutation->loc_Color_Ambient >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_Color_Ambient, t->render_lightmap_ambient[0], t->render_lightmap_ambient[1], t->render_lightmap_ambient[2]);
				if (r_glsl_permutation->loc_Color_Diffuse >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_Color_Diffuse, t->render_lightmap_diffuse[0], t->render_lightmap_diffuse[1], t->render_lightmap_diffuse[2]);
				if (r_glsl_permutation->loc_Color_Specular >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_Color_Specular, t->render_lightmap_specular[0], t->render_lightmap_specular[1], t->render_lightmap_specular[2]);
				// other LightGrid uniforms handled below
			}
			else if (mode == SHADERMODE_LIGHTDIRECTION)
			{
				if (r_glsl_permutation->loc_Color_Ambient >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_Color_Ambient, t->render_modellight_ambient[0], t->render_modellight_ambient[1], t->render_modellight_ambient[2]);
				if (r_glsl_permutation->loc_Color_Diffuse >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_Color_Diffuse, t->render_modellight_diffuse[0], t->render_modellight_diffuse[1], t->render_modellight_diffuse[2]);
				if (r_glsl_permutation->loc_Color_Specular >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_Color_Specular, t->render_modellight_specular[0], t->render_modellight_specular[1], t->render_modellight_specular[2]);
				if (r_glsl_permutation->loc_DeferredMod_Diffuse >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_DeferredMod_Diffuse, t->render_rtlight_diffuse[0], t->render_rtlight_diffuse[1], t->render_rtlight_diffuse[2]);
				if (r_glsl_permutation->loc_DeferredMod_Specular >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_DeferredMod_Specular, t->render_rtlight_specular[0], t->render_rtlight_specular[1], t->render_rtlight_specular[2]);
				if (r_glsl_permutation->loc_LightColor >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_LightColor, 1, 1, 1); // DEPRECATED
				if (r_glsl_permutation->loc_LightDir >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_LightDir, t->render_modellight_lightdir_local[0], t->render_modellight_lightdir_local[1], t->render_modellight_lightdir_local[2]);
			}
			else
			{
				if (r_glsl_permutation->loc_Color_Ambient >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_Color_Ambient, t->render_lightmap_ambient[0], t->render_lightmap_ambient[1], t->render_lightmap_ambient[2]);
				if (r_glsl_permutation->loc_Color_Diffuse >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_Color_Diffuse, t->render_lightmap_diffuse[0], t->render_lightmap_diffuse[1], t->render_lightmap_diffuse[2]);
				if (r_glsl_permutation->loc_Color_Specular >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_Color_Specular, t->render_lightmap_specular[0], t->render_lightmap_specular[1], t->render_lightmap_specular[2]);
				if (r_glsl_permutation->loc_DeferredMod_Diffuse >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_DeferredMod_Diffuse, t->render_rtlight_diffuse[0], t->render_rtlight_diffuse[1], t->render_rtlight_diffuse[2]);
				if (r_glsl_permutation->loc_DeferredMod_Specular >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_DeferredMod_Specular, t->render_rtlight_specular[0], t->render_rtlight_specular[1], t->render_rtlight_specular[2]);
			}
			// additive passes are only darkened by fog, not tinted
			if (r_glsl_permutation->loc_FogColor >= 0 && !ui)
			{
				if(blendfuncflags & BLENDFUNC_ALLOWS_FOG_HACK0)
					R_Shader_Uniform3f(r_glsl_permutation->loc_FogColor, 0, 0, 0);
				else
					R_Shader_Uniform3f(r_glsl_permutation->loc_FogColor, r_refdef.fogcolor[0], r_refdef.fogcolor[1], r_refdef.fogcolor[2]);
			}
			// When the volumetric murk is being composited it already accounts for the
			// air in front of every OPAQUE surface, so those must not also apply the
			// engine's per-fragment fog or it is counted twice. Transparent surfaces
			// keep it: they do not write depth, so the screen-space pass cannot see
			// them and would otherwise fog them as if they sat at the opaque surface
			// behind them.
			// LIQUIDFOG (r_volumetric_liquidfade): ...and one class of
			// transparent surface now HAS a per-fragment murk of its own, so
			// it must stand the classic fog down as well or a map that sets
			// _fog would obscure a liquid twice. WATERALPHA|BLENDED is the
			// same pair the RT liquid arm below tests -- exactly alpha-blended
			// Q1 water and slime; lava and teleporters are opaque and stay
			// with the murk's own pass. Never on a `ui` batch: the 2D frame
			// runs through this same shader.
			// ...and NOT MATERIALFLAG_ADD. The fade scales alpha, and on an
			// ADDITIVE blend (GL_SRC_ALPHA, GL_ONE) alpha is BRIGHTNESS rather
			// than coverage, so the same multiply would dim the surface instead
			// of dissolving it. No stock Q1 liquid is additive, so this term
			// changes nothing today; it is here so that the day one is, the
			// failure is an absent fade rather than a wrong one.
			liquidfade = R_Volumetric_LiquidFadeUniforms(!ui
				&& !(t->currentmaterialflags & MATERIALFLAG_ADD)
				&& (t->currentmaterialflags & (MATERIALFLAG_WATERALPHA | MATERIALFLAG_BLENDED)) == (MATERIALFLAG_WATERALPHA | MATERIALFLAG_BLENDED));
			if (r_glsl_permutation->loc_FogSurfaceAmount >= 0)
				R_Shader_Uniform1f(r_glsl_permutation->loc_FogSurfaceAmount,
					((r_fb.volumetricactive && !ui && !(t->currentmaterialflags & MATERIALFLAG_BLENDED)) || liquidfade > 0.0f) ? 0.0f : 1.0f);
			// WATERSURFACE (r_watersurface): the liquid surface refracts the frame
			// beneath it. The copy was taken in R_RenderScene after the murk; the
			// shader samples it at the fragment's own screen position displaced by
			// the noise volume and composes its surface OVER that sample by the alpha
			// it would otherwise have blended with -- the blend state is untouched.
			// WATERALPHA|BLENDED is the pair the fade and the RT liquid arm test:
			// exactly alpha-blended Q1 water and slime. .z is 0 on every other batch,
			// the RTLiquid no-op shape.
			//
			// THE DEPTH GUARD samples the scene depth, which is the bound target's
			// own attachment while the transparent pass draws -- the murk's
			// read-only-attachment precedent (depth writes are off for a blended
			// surface). r_transparentdepthmasking would turn those writes back on
			// and make it a real feedback loop, so the guard stands down under it.
			if (r_glsl_permutation->loc_WaterScreen >= 0)
			{
				qbool wslive = !ui && r_watersurface.integer && r_fb.waterscreen_valid && r_fb.waterscreen
				 && !r_fb.water.renderingscene && !(t->currentmaterialflags & MATERIALFLAG_ADD)
				 && (t->currentmaterialflags & (MATERIALFLAG_WATERALPHA | MATERIALFLAG_BLENDED)) == (MATERIALFLAG_WATERALPHA | MATERIALFLAG_BLENDED);
				qbool wsguard = wslive && r_watersurface_guard.integer && r_fb.scenedepthvalid && r_fb.scenedepthtexture && !r_transparentdepthmasking.integer;
				float wsinvw = r_fb.screentexturewidth > 0 ? 1.0f / (float)r_fb.screentexturewidth : 0.0f;
				float wsinvh = r_fb.screentextureheight > 0 ? 1.0f / (float)r_fb.screentextureheight : 0.0f;
				{
					// THE RIPPLE SOURCE (2026-09-13, from FTE measured): FTE builds its water
					// normal map from the 64x64 PALETTED texture whatever replaces the diffuse,
					// so a hi-res pack never touches the ripple. The shader samples the bound
					// diffuse with a mip BIAS that lands on its 64-texel level (log2(width/64):
					// 3 for a 512 QRP TGA, 0 for the stock wad texture), carried in the live
					// flag as 1 + bias so the flag keeps its > 0 test.
					float wsbias = 0.0f;
					if (wslive && t->currentskinframe && t->currentskinframe->base)
						wsbias = max(0.0f, log2f(max(64.0f, (float)R_TextureWidth(t->currentskinframe->base)) / 64.0f));
					R_Shader_Uniform4f(r_glsl_permutation->loc_WaterScreen, (float)r_refdef.view.viewport.x, (float)r_refdef.view.viewport.y, wslive ? 1.0f + wsbias : 0.0f, wslive ? max(0.0f, r_watersurface_distort.value) : 0.0f);
				}
				if (r_glsl_permutation->loc_WaterScreenSize >= 0)
					R_Shader_Uniform4f(r_glsl_permutation->loc_WaterScreenSize, wsinvw, wsinvh, (float)r_refdef.view.viewport.width * wsinvw, (float)r_refdef.view.viewport.height * wsinvh);
				if (r_glsl_permutation->loc_WaterScreenTime >= 0)
					R_Shader_Uniform4f(r_glsl_permutation->loc_WaterScreenTime, (float)r_refdef.scene.time * r_watersurface_speed.value, wsguard ? 1.0f : 0.0f, r_refdef.view.viewport.screentodepth[0], r_refdef.view.viewport.screentodepth[1]);
				if (r_glsl_permutation->loc_WaterScreenLook >= 0)
					R_Shader_Uniform4f(r_glsl_permutation->loc_WaterScreenLook, bound(0.0f, r_watersurface_opacity.value, 1.0f), max(0.1f, r_watersurface_fresnel.value), wslive ? 0.125f * max(0.0f, r_watersurface_warp.value) : 0.0f, max(0.0f, r_watersurface_bump.value));
				if (r_glsl_permutation->loc_WaterScreenTint >= 0)
					R_Shader_Uniform4f(r_glsl_permutation->loc_WaterScreenTint, max(0.0f, r_watersurface_tint_red.value), max(0.0f, r_watersurface_tint_green.value), max(0.0f, r_watersurface_tint_blue.value), max(0.0f, r_watersurface_taper.value));
				if (r_glsl_permutation->tex_Texture_WaterScreen >= 0)
					R_Mesh_TexBind(r_glsl_permutation->tex_Texture_WaterScreen, wslive ? r_fb.waterscreen : r_texture_white);
				if (r_glsl_permutation->tex_Texture_ScreenDepth >= 0)
					R_Mesh_TexBind(r_glsl_permutation->tex_Texture_ScreenDepth, wsguard ? r_fb.scenedepthtexture : r_texture_white);
				if (wslive)
				{
					// first-event: the arm lives inside a static parm, which appears in no
					// permutation number and no shader name, so this is its only console evidence
					static int wsreported;
					if (!wsreported) { wsreported = 1; Con_Printf("water surface armed (screen-space refraction, distort %.3f, guard %s)\n", r_watersurface_distort.value, wsguard ? "on" : "off"); }
				}
			}
#ifdef USE_RT_METAL
			// LIQUIDS UNDER RT: water/slime alpha surfaces draw after the RT
			// composite, so they are the one thing on screen the RT lighting
			// multiply never reached -- bright, flat and seamed, worst in
			// wall-lighting mode where the composite IS the scene's lighting.
			// Sample the composite's own term texture per fragment and multiply
			// it in. WATERALPHA|BLENDED identifies exactly alpha-blended Q1
			// water/slime; lava and teleporters are opaque and already got the
			// composite. RT_Metal_Active() is this-frame-accurate here (the
			// transparent pass runs after the composite), so water is never
			// darkened on a frame the composite did not draw.
			if (r_glsl_permutation->loc_RTLiquid >= 0)
			{
				float rtliquidamount = 0.0f;
				unsigned int rtliquidtex = 0;
				int rtliquidw = 0, rtliquidh = 0;
				if (rt_metal_liquids.value > 0.0f && rt_metal.integer && RT_Metal_Active()
				 && (t->currentmaterialflags & (MATERIALFLAG_WATERALPHA | MATERIALFLAG_BLENDED)) == (MATERIALFLAG_WATERALPHA | MATERIALFLAG_BLENDED)
				 && RT_Metal_GetTermTexture(&rtliquidtex, &rtliquidw, &rtliquidh))
					rtliquidamount = bound(0.0f, rt_metal_liquids.value, 1.0f);
				// .w is a FLOOR under the term, and it is 0 unless the term is
				// actually being applied -- so a batch that takes no term takes
				// no clamp either, and rt_metal_liquids_minlight 0 is the
				// unclamped term byte for byte.
				R_Shader_Uniform4f(r_glsl_permutation->loc_RTLiquid, (float)r_refdef.view.viewport.x, (float)r_refdef.view.viewport.y, rtliquidamount,
					rtliquidamount > 0.0f ? max(0.0f, rt_metal_liquids_minlight.value) : 0.0f);
				// SEPTEMBER2 C1 (rt_metal_liquids_own): the surface's OWN light. The
				// sampled term is the lighting of whatever lies UNDER the surface
				// (the primary ray passes through the liquid), which prints the pool
				// floor's shadows onto the water. This evaluates the sidecar's light
				// list at the batch's world-space centre on the CPU -- one constant
				// per batch, so a large lake reads flat; the per-pixel second term
				// buffer is the honest successor (CLAUDE.md carries its scope) and
				// this is the measurement that decides whether it is worth its
				// ~0.3-0.6 ms -- and the shader blends the sampled term toward it.
				if (r_glsl_permutation->loc_RTLiquidOwn >= 0)
				{
					float own[3] = { 0.0f, 0.0f, 0.0f }, ownw = 0.0f;
					if (rtliquidamount > 0.0f && rt_metal_liquids_own.value > 0.0f && texturenumsurfaces > 0)
					{
						vec3_t bmin, bmax, c, cw;
						int si;
						VectorCopy(texturesurfacelist[0]->mins, bmin);
						VectorCopy(texturesurfacelist[0]->maxs, bmax);
						for (si = 1; si < texturenumsurfaces; si++)
						{
							const msurface_t *ls = texturesurfacelist[si];
							int q;
							for (q = 0; q < 3; q++) { if (ls->mins[q] < bmin[q]) bmin[q] = ls->mins[q]; if (ls->maxs[q] > bmax[q]) bmax[q] = ls->maxs[q]; }
						}
						VectorAdd(bmin, bmax, c);
						VectorScale(c, 0.5f, c);
						Matrix4x4_Transform(&rsurface.matrix, c, cw);   // brush submodels carry their own matrix
						// A PER-FRAME CACHE ON A 256-UNIT CELL of the batch centre. Measured
						// before it existed: e1m1's slime is 17-24 liquid batches a frame (the
						// BSP splits one pool many ways), two tracelines each -- 2000-2900
						// evaluations a second at 120 fps, enough to pull the arm below the
						// presentation cap. Batches whose centres share a cell this frame share
						// one evaluation, so the cost is the number of distinct pools in view.
						{
							static struct { int frame; int cell[3]; float out[3]; qbool ok; } owncache[64];
							static int owncursor;
							int cell[3] = { (int)floor(cw[0] / 256.0f), (int)floor(cw[1] / 256.0f), (int)floor(cw[2] / 256.0f) };
							int ci, hit = -1;
							for (ci = 0; ci < 64; ci++)
								if (owncache[ci].frame == host.framecount && owncache[ci].cell[0] == cell[0] && owncache[ci].cell[1] == cell[1] && owncache[ci].cell[2] == cell[2]) { hit = ci; break; }
							if (hit < 0)
							{
								hit = owncursor; owncursor = (owncursor + 1) & 63;
								owncache[hit].frame = host.framecount;
								owncache[hit].cell[0] = cell[0]; owncache[hit].cell[1] = cell[1]; owncache[hit].cell[2] = cell[2];
								owncache[hit].ok = RT_LightSumAt(cw, rt_metal_liquids_own_shadows.integer, owncache[hit].out);
								if (owncache[hit].ok)
								{
									static int owncalls; static double ownlast;
									owncalls++;
									if (host.realtime - ownlast > 1.0) { if (ownlast > 0.0) Con_DPrintf("RT liquid own light: %d evaluations in the last second\n", owncalls); ownlast = host.realtime; owncalls = 0; }
								}
							}
							VectorCopy(owncache[hit].out, own);
						}
						if (own[0] >= 0.0f && rt_metal_liquids_own.value > 0.0f)
						{
							// liveness (first event) and a per-second call count under developer:
							// the evaluator runs once per liquid BATCH per frame, and a map's batch
							// count is the whole of this prototype's CPU cost
							static int ownreported;
							ownw = bound(0.0f, rt_metal_liquids_own.value, 1.0f);
							if (!ownreported) { ownreported = 1; Con_Printf("RT liquid own light armed (blend %.2f, %d shadow rays a pool)\n", ownw, bound(0, rt_metal_liquids_own_shadows.integer, 16)); }
						}
					}
					R_Shader_Uniform4f(r_glsl_permutation->loc_RTLiquidOwn, own[0], own[1], own[2], ownw);
				}
				// the term texture is TRACE-sized (rt_metal_scale): map viewport pixels to texels
				if (r_glsl_permutation->loc_RTLiquidScale >= 0)
					R_Shader_Uniform2f(r_glsl_permutation->loc_RTLiquidScale,
						rtliquidw > 0 ? (float)rtliquidw / (float)r_refdef.view.viewport.width : 1.0f,
						rtliquidh > 0 ? (float)rtliquidh / (float)r_refdef.view.viewport.height : 1.0f);
				// SEPTEMBER2 C2: the per-pixel liquid pair (own term | reflection), on the unit
				// after the term's. Live only when the sidecar traced it this frame; otherwise
				// the uniform is 0 and, on Metal, the unit takes white -- the declared-but-
				// unbound trap, the Texture_RTTerm shape below.
				if (r_glsl_permutation->loc_RTLiquidRT >= 0)
				{
					unsigned int liqtex = 0;
					int liqw = 0, liqh = 0;
					float liqlive = 0.0f;
					if (rtliquidamount > 0.0f && rt_metal_liquids_rt.integer && RT_Metal_GetLiquidTexture(&liqtex, &liqw, &liqh))
						liqlive = 1.0f;
					R_Shader_Uniform4f(r_glsl_permutation->loc_RTLiquidRT, liqlive, liqlive > 0.0f ? (float)(liqw / 2) : 0.0f, 0.0f, 0.0f);
					if (r_glsl_permutation->tex_Texture_RTLiquid >= 0 && r_glsl_permutation->tex_Texture_RTLiquid < 32)
					{
						if (vid.renderpath == RENDERPATH_METAL)
							Metal_Backend_TexBind(r_glsl_permutation->tex_Texture_RTLiquid, liqlive > 0.0f ? (int)liqtex : 0);
						else if (liqlive > 0.0f)
						{
							GL_ActiveTexture(r_glsl_permutation->tex_Texture_RTLiquid);
							qglBindTexture(GL_TEXTURE_RECTANGLE, (GLuint)liqtex);
							r_rtliquid_boundunits |= 1u << r_glsl_permutation->tex_Texture_RTLiquid;
						}
					}
					if (liqlive > 0.0f)
					{
						static int rtpairreported;
						if (!rtpairreported) { rtpairreported = 1; Con_Printf("RT liquid pair armed (per-pixel own term + reflection)\n"); }
					}
				}
				// A DECLARED UNIT THAT NOBODY BINDS INHERITS THE LAST SHADER'S
				// TEXTURE, and on Metal that is not merely untidy. USERTLIQUIDS
				// and USELAVA are both STATIC PARMS, so every surface shader
				// declares Texture_RTTerm (2D) and Texture_VolumeNoise (3D) --
				// and the walk hands them adjacent units whose numbers move with
				// the permutation. A lightmap batch leaves its 3D noise volume
				// on the unit the next batch's RTTerm claims, and Metal's API
				// VALIDATION traps on the type mismatch: "incorrect type of
				// texture (MTLTextureType3D) bound at Texture binding at index 3
				// (expect MTLTextureType2D) for Texture_RTTerm". The render is
				// correct either way -- RTLiquid.z is 0 on those batches, so the
				// shader's own mix() discards whatever was sampled -- which is
				// why every pixel bed in this arc passed and why only Seb's
				// Xcode DEBUG build, where validation is ON, ever saw it.
				// Clearing the unit costs nothing and makes the binding say what
				// the shader will actually read: zero.
				if (vid.renderpath == RENDERPATH_METAL && rtliquidamount <= 0.0f
				 && r_glsl_permutation->tex_Texture_RTTerm >= 0 && r_glsl_permutation->tex_Texture_RTTerm < 32)
					Metal_Backend_TexBind(r_glsl_permutation->tex_Texture_RTTerm, 0);
				if (rtliquidamount > 0.0f && r_glsl_permutation->tex_Texture_RTTerm >= 0 && r_glsl_permutation->tex_Texture_RTTerm < 32)
				{
					if (vid.renderpath == RENDERPATH_METAL)
					{
						// No rectangle textures and no foreignness to work
						// around: the term was adopted into the renderer's own
						// table (slice 5-3), so this is an ordinary bind and
						// needs no unbind afterwards -- which is why
						// R_RTLiquid_UnbindAll has nothing to do on this path.
						Metal_Backend_TexBind(r_glsl_permutation->tex_Texture_RTTerm, (int)rtliquidtex);
					}
					else
					{
						// foreign rectangle texture: raw bind through GL_ActiveTexture
						// (NEVER qglActiveTexture -- it desyncs gl_state.unit), and
						// recorded in a mask so the transparent pass can unbind it
						GL_ActiveTexture(r_glsl_permutation->tex_Texture_RTTerm);
						qglBindTexture(GL_TEXTURE_RECTANGLE, (GLuint)rtliquidtex);
						r_rtliquid_boundunits |= 1u << r_glsl_permutation->tex_Texture_RTTerm;
					}
				}
			}
#endif
			// LAVA BOIL (r_lavaboil, USELAVA): warp amplitude + time phase +
			// pulse gain for lava batches, zeros otherwise (the shader's exact
			// no-op path). The noise volume is the murk's, created on demand
			// with a white fallback while absent (an ordinary 3D texture, so a
			// plain R_Mesh_TexBind is safe -- no rectangle hazard).
			// RED GLOW (r_redglow, USEREDGLOW): strength, and the two edges of
			// the redness ramp. Uniform, not per-batch -- the test is on the
			// texel, so it needs no knowledge of which surface it is on.
			// ...and zeroed for UI batches, or the red HUD digits light up too:
			// the 2D pass runs through this same shader, which the difference
			// image caught immediately. Gibs are zeroed for taste (see
			// R_IsGibModel): they are red, but glowing viscera is not the point.
			if (r_glsl_permutation->loc_RedGlow >= 0)
			{
				// glowdefer: withheld for the glow re-add pass (rt_metal_glowpass)
				qbool rgoff = ui || glowdefer || (rsurface.entity && R_IsGibModel(rsurface.entity->model));
				R_Shader_Uniform4f(r_glsl_permutation->loc_RedGlow, rgoff ? 0.0f : r_redglow.value,
					r_redglow_threshold.value, r_redglow_minlevel.value, 0.0f);
			}
			if (r_glsl_permutation->loc_LavaParams >= 0)
			{
				if ((t->supercontents & SUPERCONTENTS_LAVA) && r_lavaboil.value > 0.0f)
				{
					// FIRST EVENT ONLY. A per-batch print here would be one of the
					// highest-rate lines in the engine, which is the shape that ran
					// Seb's console to an out-of-memory halt; this fires once per
					// process and is the smoke net for the flow reaching the shader.
					if (r_lavaflow.value > 0.0f && !r_lavaflow_announced)
					{
						r_lavaflow_announced = true;
						Con_DPrintf("M5 lava: crust flowing (r_lavaflow %.2f, speed %.3f)\n",
							r_lavaflow.value, r_lavaflow_speed.value);
					}
					// .w is the drift distance THIS FRAME, not a speed: the shader
					// subtracts it from the noise lookup, so accumulating it here
					// keeps the shader's half to a multiply-add. r_lavaflow 0 makes
					// it exactly 0 and the lookups are the old ones, byte for byte.
					R_Shader_Uniform4f(r_glsl_permutation->loc_LavaParams, r_lavaboil.value * 0.06f, (float)(r_refdef.scene.time * 0.055), 0.55f,
						(r_lavaflow.value > 0.0f) ? (float)(r_refdef.scene.time * r_lavaflow.value * max(0.0f, r_lavaflow_speed.value)) : 0.0f);
				}
				else
					R_Shader_Uniform4f(r_glsl_permutation->loc_LavaParams, 0.0f, 0.0f, 0.0f, 0.0f);
			}
			// WATER/TELEPORT SWIRL (F3): mode 2 for the *teleport/*rift
			// starfields (the name-classified marker flag), mode 1 for real
			// Q1 water/slime -- WATERALPHA is the discriminator the liquid
			// classifier gives exactly those and not lava/teleport/glassmirror.
			// basematerialflags, not currentmaterialflags, so opaque water
			// swirls the same as forced-alpha water. Zeroed per non-matching
			// batch = the exact-no-op shape.
			if (r_glsl_permutation->loc_WaterParams >= 0)
			{
				// .z is the MODE and now also selects the pivot: 2 = the old world
				// pivot, 3 = the nearest tile centre. Reusing .z rather than adding
				// a uniform keeps the constant struct exactly the size it was --
				// the F3 overflow is why that is worth caring about. .w scales the
				// churn's time; water passes 1.0, i.e. unchanged.
				if ((t->basematerialflags & MATERIALFLAG_M5TELEPORT) && r_teleportswirl.value > 0.0f)
				{
					// CHANGE-ONLY, and that is the whole guard: this sits in a
					// PER-BATCH path, and an unconditional print here is the
					// console-ink out-of-memory hazard this tree has already paid
					// for once. It is also the feature's only console evidence --
					// the arm lives inside a static parm, which appears in no
					// permutation number and no shader name, so without this line
					// a broken pivot is invisible to every bed.
					static int lastteleswirl = -1;
					int teleswirlstate = r_teleportswirl_pivot.integer * 10000 + (int)(max(0.0f, r_teleportswirl_churn.value) * 100.0f);
					if (teleswirlstate != lastteleswirl)
					{
						static const char *pivotname[3] = {"world origin (pre-2026-09-01)", "nearest tile centre", "none -- churn only"};
						lastteleswirl = teleswirlstate;
						Con_DPrintf("teleport swirl: pivot %s, churn x%.2f\n",
							pivotname[bound(0, r_teleportswirl_pivot.integer, 2)], max(0.0f, r_teleportswirl_churn.value));
					}
					R_Shader_Uniform4f(r_glsl_permutation->loc_WaterParams, r_teleportswirl.value * 0.06f, (float)r_refdef.scene.time, (r_teleportswirl_pivot.integer >= 2 ? 4.0f : (r_teleportswirl_pivot.integer ? 3.0f : 2.0f)), max(0.0f, r_teleportswirl_churn.value));
				}
				else if ((t->basematerialflags & MATERIALFLAG_WATERALPHA) && r_waterswirl.value > 0.0f)
					R_Shader_Uniform4f(r_glsl_permutation->loc_WaterParams, r_waterswirl.value * 0.06f, (float)r_refdef.scene.time, 1.0f, 1.0f);
				else
					R_Shader_Uniform4f(r_glsl_permutation->loc_WaterParams, 0.0f, 0.0f, 0.0f, 0.0f);
			}
			// the noise volume serves the boil AND the swirl, so the bind is
			// hoisted out of the LavaParams block: with only USEWATERSWIRL
			// compiled, LavaParams is stripped (loc -1) but the volume slot is
			// live and must still be fed
			if (r_glsl_permutation->tex_Texture_VolumeNoise >= 0)
			{
				rtexture_t *lavanoise = R_Volumetric_GetNoiseTexture();
				// the fallback MUST be the 3D white: this slot is texture3d in the
				// MSL, and a 2D texture here is a validation abort (run J4's find)
				R_Mesh_TexBind(r_glsl_permutation->tex_Texture_VolumeNoise, lavanoise ? lavanoise : r_texture_white3d);
			}
			// LIQUIDFOG's baked world field. Guarded on the SLOT and never on
			// the cvar, and bound on every batch that declares it: a declared
			// unit that nobody binds inherits the last shader's texture, and on
			// Metal that traps under API validation (three shipped defects of
			// exactly that class -- see the RTLiquid block above). The fallback
			// must be the 3D white, because this slot is a texture3d in the MSL
			// and a 2D texture here is the same validation abort by another
			// route. An ordinary rtexture_t, so a plain R_Mesh_TexBind serves on
			// both backends -- none of the RT term's rectangle machinery applies.
			if (r_glsl_permutation->tex_Texture_VolumeField >= 0)
			{
				rtexture_t *fadefield = R_Volumetric_LiquidFadeField();
				R_Mesh_TexBind(r_glsl_permutation->tex_Texture_VolumeField, fadefield ? fadefield : r_texture_white3d);
			}
			if (r_glsl_permutation->loc_DistortScaleRefractReflect >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_DistortScaleRefractReflect, r_water_refractdistort.value * t->refractfactor, r_water_refractdistort.value * t->refractfactor, r_water_reflectdistort.value * t->reflectfactor, r_water_reflectdistort.value * t->reflectfactor);
			if (r_glsl_permutation->loc_ScreenScaleRefractReflect >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_ScreenScaleRefractReflect, r_fb.water.screenscale[0], r_fb.water.screenscale[1], r_fb.water.screenscale[0], r_fb.water.screenscale[1]);
			if (r_glsl_permutation->loc_ScreenCenterRefractReflect >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_ScreenCenterRefractReflect, r_fb.water.screencenter[0], r_fb.water.screencenter[1], r_fb.water.screencenter[0], r_fb.water.screencenter[1]);
			if (r_glsl_permutation->loc_RefractColor >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_RefractColor, t->refractcolor4f[0], t->refractcolor4f[1], t->refractcolor4f[2], t->refractcolor4f[3] * t->currentalpha);
			if (r_glsl_permutation->loc_ReflectColor >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_ReflectColor, t->reflectcolor4f[0], t->reflectcolor4f[1], t->reflectcolor4f[2], t->reflectcolor4f[3] * t->currentalpha);
			if (r_glsl_permutation->loc_ReflectFactor >= 0) R_Shader_Uniform1f(r_glsl_permutation->loc_ReflectFactor, t->reflectmax - t->reflectmin);
			if (r_glsl_permutation->loc_ReflectOffset >= 0) R_Shader_Uniform1f(r_glsl_permutation->loc_ReflectOffset, t->reflectmin);
			if (r_glsl_permutation->loc_SpecularPower >= 0) R_Shader_Uniform1f(r_glsl_permutation->loc_SpecularPower, t->specularpower * (r_shadow_glossexact.integer ? 0.25f : 1.0f) - 1.0f);
			if (r_glsl_permutation->loc_NormalmapScrollBlend >= 0) R_Shader_Uniform2f(r_glsl_permutation->loc_NormalmapScrollBlend, t->r_water_waterscroll[0], t->r_water_waterscroll[1]);
		}
		if (r_glsl_permutation->loc_TexMatrix >= 0) {Matrix4x4_ToArrayFloatGL(&t->currenttexmatrix, m16f);R_Shader_UniformMatrix4fv(r_glsl_permutation->loc_TexMatrix, 1, false, m16f);}
		if (r_glsl_permutation->loc_BackgroundTexMatrix >= 0) {Matrix4x4_ToArrayFloatGL(&t->currentbackgroundtexmatrix, m16f);R_Shader_UniformMatrix4fv(r_glsl_permutation->loc_BackgroundTexMatrix, 1, false, m16f);}
		if (r_glsl_permutation->loc_ShadowMapMatrix >= 0) {Matrix4x4_ToArrayFloatGL(&r_shadow_shadowmapmatrix, m16f);R_Shader_UniformMatrix4fv(r_glsl_permutation->loc_ShadowMapMatrix, 1, false, m16f);}
		if (permutation & SHADERPERMUTATION_SHADOWMAPORTHO)
		{
			if (r_glsl_permutation->loc_ShadowMap_TextureScale >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_ShadowMap_TextureScale, r_shadow_modelshadowmap_texturescale[0], r_shadow_modelshadowmap_texturescale[1], r_shadow_modelshadowmap_texturescale[2], r_shadow_modelshadowmap_texturescale[3]);
			if (r_glsl_permutation->loc_ShadowMap_Parameters >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_ShadowMap_Parameters, r_shadow_modelshadowmap_parameters[0], r_shadow_modelshadowmap_parameters[1], r_shadow_modelshadowmap_parameters[2], r_shadow_modelshadowmap_parameters[3]);
		}
		else
		{
			if (r_glsl_permutation->loc_ShadowMap_TextureScale >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_ShadowMap_TextureScale, r_shadow_lightshadowmap_texturescale[0], r_shadow_lightshadowmap_texturescale[1], r_shadow_lightshadowmap_texturescale[2], r_shadow_lightshadowmap_texturescale[3]);
			if (r_glsl_permutation->loc_ShadowMap_Parameters >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_ShadowMap_Parameters, r_shadow_lightshadowmap_parameters[0], r_shadow_lightshadowmap_parameters[1], r_shadow_lightshadowmap_parameters[2], r_shadow_lightshadowmap_parameters[3]);
		}

		// glowdefer: withheld for the glow re-add pass (rt_metal_glowpass)
		if (r_glsl_permutation->loc_Color_Glow >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_Color_Glow, glowdefer ? 0.0f : t->render_glowmod[0], glowdefer ? 0.0f : t->render_glowmod[1], glowdefer ? 0.0f : t->render_glowmod[2]);
		if (r_glsl_permutation->loc_Alpha >= 0) R_Shader_Uniform1f(r_glsl_permutation->loc_Alpha, t->currentalpha * ((t->basematerialflags & MATERIALFLAG_WATERSHADER && r_fb.water.enabled && !r_refdef.view.isoverlay) ? t->r_water_wateralpha : 1));
		if (r_glsl_permutation->loc_EyePosition >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_EyePosition, rsurface.localvieworigin[0], rsurface.localvieworigin[1], rsurface.localvieworigin[2]);
		if (r_glsl_permutation->loc_Color_Pants >= 0)
		{
			if (t->pantstexture)
				R_Shader_Uniform3f(r_glsl_permutation->loc_Color_Pants, t->render_colormap_pants[0], t->render_colormap_pants[1], t->render_colormap_pants[2]);
			else
				R_Shader_Uniform3f(r_glsl_permutation->loc_Color_Pants, 0, 0, 0);
		}
		if (r_glsl_permutation->loc_Color_Shirt >= 0)
		{
			if (t->shirttexture)
				R_Shader_Uniform3f(r_glsl_permutation->loc_Color_Shirt, t->render_colormap_shirt[0], t->render_colormap_shirt[1], t->render_colormap_shirt[2]);
			else
				R_Shader_Uniform3f(r_glsl_permutation->loc_Color_Shirt, 0, 0, 0);
		}
		if (r_glsl_permutation->loc_FogPlane >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_FogPlane, rsurface.fogplane[0], rsurface.fogplane[1], rsurface.fogplane[2], rsurface.fogplane[3]);
		if (r_glsl_permutation->loc_FogPlaneViewDist >= 0) R_Shader_Uniform1f(r_glsl_permutation->loc_FogPlaneViewDist, rsurface.fogplaneviewdist);
		if (r_glsl_permutation->loc_FogRangeRecip >= 0) R_Shader_Uniform1f(r_glsl_permutation->loc_FogRangeRecip, rsurface.fograngerecip);
		if (r_glsl_permutation->loc_FogHeightFade >= 0) R_Shader_Uniform1f(r_glsl_permutation->loc_FogHeightFade, rsurface.fogheightfade);
		if (r_glsl_permutation->loc_OffsetMapping_ScaleSteps >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_OffsetMapping_ScaleSteps,
				r_glsl_offsetmapping_scale.value*t->offsetscale,
				max(1, (permutation & SHADERPERMUTATION_OFFSETMAPPING_RELIEFMAPPING) ? r_glsl_offsetmapping_reliefmapping_steps.integer : r_glsl_offsetmapping_steps.integer),
				1.0 / max(1, (permutation & SHADERPERMUTATION_OFFSETMAPPING_RELIEFMAPPING) ? r_glsl_offsetmapping_reliefmapping_steps.integer : r_glsl_offsetmapping_steps.integer),
				max(1, r_glsl_offsetmapping_reliefmapping_refinesteps.integer)
			);
		if (r_glsl_permutation->loc_OffsetMapping_LodDistance >= 0) R_Shader_Uniform1f(r_glsl_permutation->loc_OffsetMapping_LodDistance, r_glsl_offsetmapping_lod_distance.integer * r_refdef.view.quality);
		if (r_glsl_permutation->loc_OffsetMapping_Bias >= 0) R_Shader_Uniform1f(r_glsl_permutation->loc_OffsetMapping_Bias, t->offsetbias);
		if (r_glsl_permutation->loc_ScreenToDepth >= 0) R_Shader_Uniform2f(r_glsl_permutation->loc_ScreenToDepth, r_refdef.view.viewport.screentodepth[0], r_refdef.view.viewport.screentodepth[1]);
		if (r_glsl_permutation->loc_PixelToScreenTexCoord >= 0) R_Shader_Uniform2f(r_glsl_permutation->loc_PixelToScreenTexCoord, 1.0f/r_fb.screentexturewidth, 1.0f/r_fb.screentextureheight);
		if (r_glsl_permutation->loc_BounceGridMatrix >= 0) {Matrix4x4_Concat(&tempmatrix, &r_shadow_bouncegrid_state.matrix, &rsurface.matrix);Matrix4x4_ToArrayFloatGL(&tempmatrix, m16f);R_Shader_UniformMatrix4fv(r_glsl_permutation->loc_BounceGridMatrix, 1, false, m16f);}
		if (r_glsl_permutation->loc_BounceGridIntensity >= 0) R_Shader_Uniform1f(r_glsl_permutation->loc_BounceGridIntensity, r_shadow_bouncegrid_state.intensity*r_refdef.view.colorscale);
		if (r_glsl_permutation->loc_LightGridMatrix >= 0 && r_refdef.scene.worldmodel)
		{
			float m9f[9];
			Matrix4x4_Concat(&tempmatrix, &r_refdef.scene.worldmodel->brushq3.lightgridworldtotexturematrix, &rsurface.matrix);
			Matrix4x4_ToArrayFloatGL(&tempmatrix, m16f);
			R_Shader_UniformMatrix4fv(r_glsl_permutation->loc_LightGridMatrix, 1, false, m16f);
			Matrix4x4_Normalize3(&tempmatrix, &rsurface.matrix);
			Matrix4x4_ToArrayFloatGL(&tempmatrix, m16f);
			m9f[0] = m16f[0];m9f[1] = m16f[1];m9f[2] = m16f[2];
			m9f[3] = m16f[4];m9f[4] = m16f[5];m9f[5] = m16f[6];
			m9f[6] = m16f[8];m9f[7] = m16f[9];m9f[8] = m16f[10];
			R_Shader_UniformMatrix3fv(r_glsl_permutation->loc_LightGridNormalMatrix, 1, false, m9f);
		}

		// GLOW RE-ADD PASS (rt_metal_glowpass; see R_RTGlow_Pass): collapse every
		// non-emission contributor to zero, leaving the shader's glow + redglow
		// adds alone. Placed after ALL the colour uploads above so nothing can
		// re-write these; specular, the deferred taps and the bouncegrid tap are
		// included because each adds colour independently of Ambient/Diffuse.
		if (r_rtglowpass_active)
		{
			if (r_glsl_permutation->loc_Color_Ambient >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_Color_Ambient, 0, 0, 0);
			if (r_glsl_permutation->loc_Color_Diffuse >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_Color_Diffuse, 0, 0, 0);
			if (r_glsl_permutation->loc_Color_Specular >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_Color_Specular, 0, 0, 0);
			if (r_glsl_permutation->loc_DeferredMod_Diffuse >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_DeferredMod_Diffuse, 0, 0, 0);
			if (r_glsl_permutation->loc_DeferredMod_Specular >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_DeferredMod_Specular, 0, 0, 0);
			if (r_glsl_permutation->loc_BounceGridIntensity >= 0) R_Shader_Uniform1f(r_glsl_permutation->loc_BounceGridIntensity, 0);
		}

		if (r_glsl_permutation->tex_Texture_First           >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_First            , r_texture_white                                     );
		if (r_glsl_permutation->tex_Texture_Second          >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_Second           , r_texture_white                                     );
		if (r_glsl_permutation->tex_Texture_GammaRamps      >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_GammaRamps       , r_texture_gammaramps                                );
		if (r_glsl_permutation->tex_Texture_Normal          >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_Normal           , t->nmaptexture                       );
		if (r_glsl_permutation->tex_Texture_Color           >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_Color            , t->basetexture                       );
		if (r_glsl_permutation->tex_Texture_Gloss           >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_Gloss            , t->glosstexture                      );
		if (r_glsl_permutation->tex_Texture_Glow            >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_Glow             , t->glowtexture                       );
		if (r_glsl_permutation->tex_Texture_SecondaryNormal >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_SecondaryNormal  , t->backgroundnmaptexture             );
		if (r_glsl_permutation->tex_Texture_SecondaryColor  >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_SecondaryColor   , t->backgroundbasetexture             );
		if (r_glsl_permutation->tex_Texture_SecondaryGloss  >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_SecondaryGloss   , t->backgroundglosstexture            );
		if (r_glsl_permutation->tex_Texture_SecondaryGlow   >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_SecondaryGlow    , t->backgroundglowtexture             );
		if (r_glsl_permutation->tex_Texture_Pants           >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_Pants            , t->pantstexture                      );
		if (r_glsl_permutation->tex_Texture_Shirt           >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_Shirt            , t->shirttexture                      );
		if (r_glsl_permutation->tex_Texture_ReflectMask     >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_ReflectMask      , t->reflectmasktexture                );
		if (r_glsl_permutation->tex_Texture_ReflectCube     >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_ReflectCube      , t->reflectcubetexture ? t->reflectcubetexture : r_texture_whitecube);
		if (r_glsl_permutation->tex_Texture_FogHeightTexture>= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_FogHeightTexture , r_texture_fogheighttexture                          );
		if (r_glsl_permutation->tex_Texture_FogMask         >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_FogMask          , r_texture_fogattenuation                            );
		if (r_glsl_permutation->tex_Texture_Lightmap        >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_Lightmap         , rsurface.lightmaptexture ? rsurface.lightmaptexture : r_texture_white);
		if (r_glsl_permutation->tex_Texture_Deluxemap       >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_Deluxemap        , rsurface.deluxemaptexture ? rsurface.deluxemaptexture : r_texture_blanknormalmap);
		if (r_glsl_permutation->tex_Texture_Attenuation     >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_Attenuation      , r_shadow_attenuationgradienttexture                 );
		if (rsurfacepass == RSURFPASS_BACKGROUND)
		{
			if (r_glsl_permutation->tex_Texture_Refraction  >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_Refraction        , waterplane->rt_refraction ? waterplane->rt_refraction->colortexture[0] : r_texture_black);
			if (r_glsl_permutation->tex_Texture_First       >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_First             , waterplane->rt_camera ? waterplane->rt_camera->colortexture[0] : r_texture_black);
			if (r_glsl_permutation->tex_Texture_Reflection  >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_Reflection        , waterplane->rt_reflection ? waterplane->rt_reflection->colortexture[0] : r_texture_black);
		}
		else
		{
			if (r_glsl_permutation->tex_Texture_Reflection >= 0 && waterplane) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_Reflection        , waterplane->rt_reflection ? waterplane->rt_reflection->colortexture[0] : r_texture_black);
		}
		if (r_glsl_permutation->tex_Texture_ScreenNormalMap >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_ScreenNormalMap   , r_shadow_prepassgeometrynormalmaptexture            );
		if (r_glsl_permutation->tex_Texture_ScreenDiffuse   >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_ScreenDiffuse     , r_shadow_prepasslightingdiffusetexture              );
		if (r_glsl_permutation->tex_Texture_ScreenSpecular  >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_ScreenSpecular    , r_shadow_prepasslightingspeculartexture             );
		if (rsurface.rtlight || (r_shadow_usingshadowmaportho && !(rsurface.ent_flags & RENDER_NOSELFSHADOW)))
		{
			if (r_glsl_permutation->tex_Texture_ShadowMap2D     >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_ShadowMap2D, r_shadow_shadowmap2ddepthtexture                           );
			if (rsurface.rtlight)
			{
				if (r_glsl_permutation->tex_Texture_Cube            >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_Cube              , rsurface.rtlight->currentcubemap                    );
				if (r_glsl_permutation->tex_Texture_CubeProjection  >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_CubeProjection    , r_shadow_shadowmapvsdcttexture                      );
			}
		}
		if (r_glsl_permutation->tex_Texture_BounceGrid  >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_BounceGrid, r_shadow_bouncegrid_state.texture);
		if (r_glsl_permutation->tex_Texture_LightGrid   >= 0 && r_refdef.scene.worldmodel) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_LightGrid, r_refdef.scene.worldmodel->brushq3.lightgridtexture);
		CHECKGLERROR
		break;
	}
}

void R_SetupShader_DeferredLight(const rtlight_t *rtlight)
{
	// select a permutation of the lighting shader appropriate to this
	// combination of texture, entity, light source, and fogging, only use the
	// minimum features necessary to avoid wasting rendering time in the
	// fragment shader on features that are not being used
	uint64_t permutation = 0;
	unsigned int mode = 0;
	const float *lightcolorbase = rtlight->currentcolor;
	float ambientscale = rtlight->ambientscale;
	float diffusescale = rtlight->diffusescale;
	float specularscale = rtlight->specularscale;
	// this is the location of the light in view space
	vec3_t viewlightorigin;
	// this transforms from view space (camera) to light space (cubemap)
	matrix4x4_t viewtolight;
	matrix4x4_t lighttoview;
	float viewtolight16f[16];
	// light source
	mode = SHADERMODE_DEFERREDLIGHTSOURCE;
	if (rtlight->currentcubemap != r_texture_whitecube)
		permutation |= SHADERPERMUTATION_CUBEFILTER;
	if (diffusescale > 0)
		permutation |= SHADERPERMUTATION_DIFFUSE;
	if (specularscale > 0 && r_shadow_gloss.integer > 0)
		permutation |= SHADERPERMUTATION_SPECULAR | SHADERPERMUTATION_DIFFUSE;
	if (r_shadow_usingshadowmap2d)
	{
		permutation |= SHADERPERMUTATION_SHADOWMAP2D;
		if (r_shadow_shadowmapvsdct)
			permutation |= SHADERPERMUTATION_SHADOWMAPVSDCT;

		if (r_shadow_shadowmap2ddepthbuffer)
			permutation |= SHADERPERMUTATION_DEPTHRGB;
	}
	if (vid.allowalphatocoverage)
		GL_AlphaToCoverage(false);
	Matrix4x4_Transform(&r_refdef.view.viewport.viewmatrix, rtlight->shadoworigin, viewlightorigin);
	Matrix4x4_Concat(&lighttoview, &r_refdef.view.viewport.viewmatrix, &rtlight->matrix_lighttoworld);
	Matrix4x4_Invert_Full(&viewtolight, &lighttoview);
	Matrix4x4_ToArrayFloatGL(&viewtolight, viewtolight16f);
	switch(vid.renderpath)
	{
	case RENDERPATH_METAL:
		break; // METAL_TODO: Phase 0 stub (METAL.md)
	case RENDERPATH_GL32:
	case RENDERPATH_GLES2:
		R_SetupShader_SetPermutationGLSL(mode, permutation);
		if (r_glsl_permutation->loc_LightPosition             >= 0) R_Shader_Uniform3f(       r_glsl_permutation->loc_LightPosition            , viewlightorigin[0], viewlightorigin[1], viewlightorigin[2]);
		if (r_glsl_permutation->loc_ViewToLight               >= 0) R_Shader_UniformMatrix4fv(r_glsl_permutation->loc_ViewToLight              , 1, false, viewtolight16f);
		if (r_glsl_permutation->loc_DeferredColor_Ambient     >= 0) R_Shader_Uniform3f(       r_glsl_permutation->loc_DeferredColor_Ambient    , lightcolorbase[0] * ambientscale , lightcolorbase[1] * ambientscale , lightcolorbase[2] * ambientscale );
		if (r_glsl_permutation->loc_DeferredColor_Diffuse     >= 0) R_Shader_Uniform3f(       r_glsl_permutation->loc_DeferredColor_Diffuse    , lightcolorbase[0] * diffusescale , lightcolorbase[1] * diffusescale , lightcolorbase[2] * diffusescale );
		if (r_glsl_permutation->loc_DeferredColor_Specular    >= 0) R_Shader_Uniform3f(       r_glsl_permutation->loc_DeferredColor_Specular   , lightcolorbase[0] * specularscale, lightcolorbase[1] * specularscale, lightcolorbase[2] * specularscale);
		if (r_glsl_permutation->loc_ShadowMap_TextureScale    >= 0) R_Shader_Uniform4f(       r_glsl_permutation->loc_ShadowMap_TextureScale   , r_shadow_lightshadowmap_texturescale[0], r_shadow_lightshadowmap_texturescale[1], r_shadow_lightshadowmap_texturescale[2], r_shadow_lightshadowmap_texturescale[3]);
		if (r_glsl_permutation->loc_ShadowMap_Parameters      >= 0) R_Shader_Uniform4f(       r_glsl_permutation->loc_ShadowMap_Parameters     , r_shadow_lightshadowmap_parameters[0], r_shadow_lightshadowmap_parameters[1], r_shadow_lightshadowmap_parameters[2], r_shadow_lightshadowmap_parameters[3]);
		if (r_glsl_permutation->loc_SpecularPower             >= 0) R_Shader_Uniform1f(       r_glsl_permutation->loc_SpecularPower            , (r_shadow_gloss.integer == 2 ? r_shadow_gloss2exponent.value : r_shadow_glossexponent.value) * (r_shadow_glossexact.integer ? 0.25f : 1.0f) - 1.0f);
		if (r_glsl_permutation->loc_ScreenToDepth             >= 0) R_Shader_Uniform2f(       r_glsl_permutation->loc_ScreenToDepth            , r_refdef.view.viewport.screentodepth[0], r_refdef.view.viewport.screentodepth[1]);
		if (r_glsl_permutation->loc_PixelToScreenTexCoord     >= 0) R_Shader_Uniform2f(       r_glsl_permutation->loc_PixelToScreenTexCoord    , 1.0f/r_fb.screentexturewidth, 1.0f/r_fb.screentextureheight);

		if (r_glsl_permutation->tex_Texture_Attenuation       >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_Attenuation        , r_shadow_attenuationgradienttexture                 );
		if (r_glsl_permutation->tex_Texture_ScreenNormalMap   >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_ScreenNormalMap    , r_shadow_prepassgeometrynormalmaptexture            );
		if (r_glsl_permutation->tex_Texture_Cube              >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_Cube               , rsurface.rtlight->currentcubemap                    );
		if (r_glsl_permutation->tex_Texture_ShadowMap2D       >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_ShadowMap2D        , r_shadow_shadowmap2ddepthtexture                    );
		if (r_glsl_permutation->tex_Texture_CubeProjection    >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_CubeProjection     , r_shadow_shadowmapvsdcttexture                      );
		break;
	}
}

#define SKINFRAME_HASH 1024

typedef struct
{
	unsigned int loadsequence; // incremented each level change
	memexpandablearray_t array;
	skinframe_t *hash[SKINFRAME_HASH];
}
r_skinframe_t;
r_skinframe_t r_skinframe;

void R_SkinFrame_PrepareForPurge(void)
{
	r_skinframe.loadsequence++;
	// wrap it without hitting zero
	if (r_skinframe.loadsequence >= 200)
		r_skinframe.loadsequence = 1;
}

void R_SkinFrame_MarkUsed(skinframe_t *skinframe)
{
	if (!skinframe)
		return;
	// mark the skinframe as used for the purging code
	skinframe->loadsequence = r_skinframe.loadsequence;
}

void R_SkinFrame_PurgeSkinFrame(skinframe_t *s)
{
	if (s == NULL)
		return;
	if (s->merged == s->base)
		s->merged = NULL;
	R_PurgeTexture(s->stain); s->stain = NULL;
	R_PurgeTexture(s->merged); s->merged = NULL;
	R_PurgeTexture(s->base); s->base = NULL;
	R_PurgeTexture(s->pants); s->pants = NULL;
	R_PurgeTexture(s->shirt); s->shirt = NULL;
	R_PurgeTexture(s->nmap); s->nmap = NULL;
	R_PurgeTexture(s->gloss); s->gloss = NULL;
	R_PurgeTexture(s->glow); s->glow = NULL;
	R_PurgeTexture(s->fog); s->fog = NULL;
	R_PurgeTexture(s->reflect); s->reflect = NULL;
	s->loadsequence = 0;
}

void R_SkinFrame_Purge(void)
{
	int i;
	skinframe_t *s;
	for (i = 0;i < SKINFRAME_HASH;i++)
	{
		for (s = r_skinframe.hash[i];s;s = s->next)
		{
			if (s->loadsequence && s->loadsequence != r_skinframe.loadsequence)
				R_SkinFrame_PurgeSkinFrame(s);
		}
	}
}

skinframe_t *R_SkinFrame_FindNextByName( skinframe_t *last, const char *name ) {
	skinframe_t *item;
	char basename[MAX_QPATH];

	Image_StripImageExtension(name, basename, sizeof(basename));

	if( last == NULL ) {
		int hashindex;
		hashindex = CRC_Block((unsigned char *)basename, strlen(basename)) & (SKINFRAME_HASH - 1);
		item = r_skinframe.hash[hashindex];
	} else {
		item = last->next;
	}

	// linearly search through the hash bucket
	for( ; item ; item = item->next ) {
		if( !strcmp( item->basename, basename ) ) {
			return item;
		}
	}
	return NULL;
}

skinframe_t *R_SkinFrame_Find(const char *name, int textureflags, int comparewidth, int compareheight, int comparecrc, qbool add)
{
	skinframe_t *item;
	int compareflags = textureflags & TEXF_IMPORTANTBITS;
	int hashindex;
	char basename[MAX_QPATH];

	Image_StripImageExtension(name, basename, sizeof(basename));

	hashindex = CRC_Block((unsigned char *)basename, strlen(basename)) & (SKINFRAME_HASH - 1);
	for (item = r_skinframe.hash[hashindex];item;item = item->next)
		if (!strcmp(item->basename, basename) &&
			item->textureflags == compareflags &&
			item->comparewidth == comparewidth &&
			item->compareheight == compareheight &&
			item->comparecrc == comparecrc)
			break;

	if (!item)
	{
		if (!add)
			return NULL;
		item = (skinframe_t *)Mem_ExpandableArray_AllocRecord(&r_skinframe.array);
		memset(item, 0, sizeof(*item));
		dp_strlcpy(item->basename, basename, sizeof(item->basename));
		item->textureflags = compareflags;
		item->comparewidth = comparewidth;
		item->compareheight = compareheight;
		item->comparecrc = comparecrc;
		item->next = r_skinframe.hash[hashindex];
		r_skinframe.hash[hashindex] = item;
	}
	else if (textureflags & TEXF_FORCE_RELOAD)
		R_SkinFrame_PurgeSkinFrame(item);

	R_SkinFrame_MarkUsed(item);
	return item;
}

#define R_SKINFRAME_LOAD_AVERAGE_COLORS(cnt, getpixel) \
	{ \
		unsigned long long avgcolor[5], wsum; \
		int pix, comp, w; \
		avgcolor[0] = 0; \
		avgcolor[1] = 0; \
		avgcolor[2] = 0; \
		avgcolor[3] = 0; \
		avgcolor[4] = 0; \
		wsum = 0; \
		for(pix = 0; pix < cnt; ++pix) \
		{ \
			w = 0; \
			for(comp = 0; comp < 3; ++comp) \
				w += getpixel; \
			if(w) /* ignore perfectly black pixels because that is better for model skins */ \
			{ \
				++wsum; \
				/* comp = 3; -- not needed, comp is always 3 when we get here */ \
				w = getpixel; \
				for(comp = 0; comp < 3; ++comp) \
					avgcolor[comp] += getpixel * w; \
				avgcolor[3] += w; \
			} \
			/* comp = 3; -- not needed, comp is always 3 when we get here */ \
			avgcolor[4] += getpixel; \
		} \
		if(avgcolor[3] == 0) /* no pixels seen? even worse */ \
			avgcolor[3] = 1; \
		skinframe->avgcolor[0] = avgcolor[2] / (255.0 * avgcolor[3]); \
		skinframe->avgcolor[1] = avgcolor[1] / (255.0 * avgcolor[3]); \
		skinframe->avgcolor[2] = avgcolor[0] / (255.0 * avgcolor[3]); \
		skinframe->avgcolor[3] = avgcolor[4] / (255.0 * cnt); \
	}

skinframe_t *R_SkinFrame_LoadExternal(const char *name, int textureflags, qbool complain, qbool fallbacknotexture)
{
	skinframe_t *skinframe;

	if (cls.state == ca_dedicated)
		return NULL;

	// return an existing skinframe if already loaded
	skinframe = R_SkinFrame_Find(name, textureflags, 0, 0, 0, false);
	if (skinframe && skinframe->base)
		return skinframe;

	// if the skinframe doesn't exist this will create it
	return R_SkinFrame_LoadExternal_SkinFrame(skinframe, name, textureflags, complain, fallbacknotexture);
}

extern cvar_t gl_picmip;
skinframe_t *R_SkinFrame_LoadExternal_SkinFrame(skinframe_t *skinframe, const char *name, int textureflags, qbool complain, qbool fallbacknotexture)
{
	int j;
	unsigned char *pixels;
	unsigned char *bumppixels;
	unsigned char *basepixels = NULL;
	int basepixels_width = 0;
	int basepixels_height = 0;
	rtexture_t *ddsbase = NULL;
	qbool ddshasalpha = false;
	float ddsavgcolor[4];
	char basename[MAX_QPATH];
	int miplevel = R_PicmipForFlags(textureflags);
	int savemiplevel = miplevel;
	int mymiplevel;
	char vabuf[1024];

	if (cls.state == ca_dedicated)
		return NULL;

	Image_StripImageExtension(name, basename, sizeof(basename));

	// check for DDS texture file first
	if (!r_loaddds || !(ddsbase = R_LoadTextureDDSFile(r_main_texturepool, va(vabuf, sizeof(vabuf), "dds/%s.dds", basename), vid.sRGB3D, textureflags, &ddshasalpha, ddsavgcolor, miplevel, false)))
	{
		basepixels = loadimagepixelsbgra(name, complain, true, false, &miplevel);
		if (basepixels == NULL && fallbacknotexture)
			basepixels = Image_GenerateNoTexture();
		if (basepixels == NULL)
			return NULL;
	}

	// FIXME handle miplevel

	if (developer_loading.integer)
		Con_Printf("loading skin \"%s\"\n", name);

	// we've got some pixels to store, so really allocate this new texture now
	if (!skinframe)
		skinframe = R_SkinFrame_Find(name, textureflags, 0, 0, 0, true);
	textureflags &= ~TEXF_FORCE_RELOAD;
	skinframe->stain = NULL;
	skinframe->merged = NULL;
	skinframe->base = NULL;
	skinframe->pants = NULL;
	skinframe->shirt = NULL;
	skinframe->nmap = NULL;
	skinframe->gloss = NULL;
	skinframe->glow = NULL;
	skinframe->fog = NULL;
	skinframe->reflect = NULL;
	skinframe->hasalpha = false;
	// we could store the q2animname here too

	if (ddsbase)
	{
		skinframe->base = ddsbase;
		skinframe->hasalpha = ddshasalpha;
		VectorCopy(ddsavgcolor, skinframe->avgcolor);
		if (r_loadfog && skinframe->hasalpha)
			skinframe->fog = R_LoadTextureDDSFile(r_main_texturepool, va(vabuf, sizeof(vabuf), "dds/%s_mask.dds", skinframe->basename), false, textureflags | TEXF_ALPHA, NULL, NULL, miplevel, true);
		//Con_Printf("Texture %s has average colors %f %f %f alpha %f\n", name, skinframe->avgcolor[0], skinframe->avgcolor[1], skinframe->avgcolor[2], skinframe->avgcolor[3]);
	}
	else
	{
		basepixels_width = image_width;
		basepixels_height = image_height;
		skinframe->base = R_LoadTexture2D (r_main_texturepool, skinframe->basename, basepixels_width, basepixels_height, basepixels, vid.sRGB3D ? TEXTYPE_SRGB_BGRA : TEXTYPE_BGRA, textureflags & (gl_texturecompression_color.integer && gl_texturecompression.integer ? ~0 : ~TEXF_COMPRESS), miplevel, NULL);
		if (textureflags & TEXF_ALPHA)
		{
			for (j = 3;j < basepixels_width * basepixels_height * 4;j += 4)
			{
				if (basepixels[j] < 255)
				{
					skinframe->hasalpha = true;
					break;
				}
			}
			if (r_loadfog && skinframe->hasalpha)
			{
				// has transparent pixels
				pixels = (unsigned char *)Mem_Alloc(tempmempool, image_width * image_height * 4);
				for (j = 0;j < image_width * image_height * 4;j += 4)
				{
					pixels[j+0] = 255;
					pixels[j+1] = 255;
					pixels[j+2] = 255;
					pixels[j+3] = basepixels[j+3];
				}
				skinframe->fog = R_LoadTexture2D (r_main_texturepool, va(vabuf, sizeof(vabuf), "%s_mask", skinframe->basename), image_width, image_height, pixels, TEXTYPE_BGRA, textureflags & (gl_texturecompression_color.integer && gl_texturecompression.integer ? ~0 : ~TEXF_COMPRESS), miplevel, NULL);
				Mem_Free(pixels);
			}
		}
		R_SKINFRAME_LOAD_AVERAGE_COLORS(basepixels_width * basepixels_height, basepixels[4 * pix + comp]);
#ifndef USE_GLES2
		//Con_Printf("Texture %s has average colors %f %f %f alpha %f\n", name, skinframe->avgcolor[0], skinframe->avgcolor[1], skinframe->avgcolor[2], skinframe->avgcolor[3]);
		if (r_savedds && skinframe->base)
			R_SaveTextureDDSFile(skinframe->base, va(vabuf, sizeof(vabuf), "dds/%s.dds", skinframe->basename), r_texture_dds_save.integer < 2, skinframe->hasalpha);
		if (r_savedds && skinframe->fog)
			R_SaveTextureDDSFile(skinframe->fog, va(vabuf, sizeof(vabuf), "dds/%s_mask.dds", skinframe->basename), r_texture_dds_save.integer < 2, true);
#endif
	}

	if (r_loaddds)
	{
		mymiplevel = savemiplevel;
		if (r_loadnormalmap)
			skinframe->nmap = R_LoadTextureDDSFile(r_main_texturepool, va(vabuf, sizeof(vabuf), "dds/%s_norm.dds", skinframe->basename), false, (TEXF_ALPHA | textureflags) & (r_mipnormalmaps.integer ? ~0 : ~TEXF_MIPMAP), NULL, NULL, mymiplevel, true);
		skinframe->glow = R_LoadTextureDDSFile(r_main_texturepool, va(vabuf, sizeof(vabuf), "dds/%s_glow.dds", skinframe->basename), vid.sRGB3D, textureflags, NULL, NULL, mymiplevel, true);
		if (r_loadgloss)
			skinframe->gloss = R_LoadTextureDDSFile(r_main_texturepool, va(vabuf, sizeof(vabuf), "dds/%s_gloss.dds", skinframe->basename), vid.sRGB3D, textureflags, NULL, NULL, mymiplevel, true);
		skinframe->pants = R_LoadTextureDDSFile(r_main_texturepool, va(vabuf, sizeof(vabuf), "dds/%s_pants.dds", skinframe->basename), vid.sRGB3D, textureflags, NULL, NULL, mymiplevel, true);
		skinframe->shirt = R_LoadTextureDDSFile(r_main_texturepool, va(vabuf, sizeof(vabuf), "dds/%s_shirt.dds", skinframe->basename), vid.sRGB3D, textureflags, NULL, NULL, mymiplevel, true);
		skinframe->reflect = R_LoadTextureDDSFile(r_main_texturepool, va(vabuf, sizeof(vabuf), "dds/%s_reflect.dds", skinframe->basename), vid.sRGB3D, textureflags, NULL, NULL, mymiplevel, true);
	}

	// _norm is the name used by tenebrae and has been adopted as standard
	if (r_loadnormalmap && skinframe->nmap == NULL)
	{
		mymiplevel = savemiplevel;
		if ((pixels = loadimagepixelsbgra(va(vabuf, sizeof(vabuf), "%s_norm", skinframe->basename), false, false, false, &mymiplevel)) != NULL)
		{
			skinframe->nmap = R_LoadTexture2D (r_main_texturepool, va(vabuf, sizeof(vabuf), "%s_nmap", skinframe->basename), image_width, image_height, pixels, TEXTYPE_BGRA, (TEXF_ALPHA | textureflags) & (r_mipnormalmaps.integer ? ~0 : ~TEXF_MIPMAP) & (gl_texturecompression_normal.integer && gl_texturecompression.integer ? ~0 : ~TEXF_COMPRESS), mymiplevel, NULL);
			Mem_Free(pixels);
			pixels = NULL;
		}
		else if (r_shadow_bumpscale_bumpmap.value > 0 && (bumppixels = loadimagepixelsbgra(va(vabuf, sizeof(vabuf), "%s_bump", skinframe->basename), false, false, false, &mymiplevel)) != NULL)
		{
			pixels = (unsigned char *)Mem_Alloc(tempmempool, image_width * image_height * 4);
			Image_HeightmapToNormalmap_BGRA(bumppixels, pixels, image_width, image_height, false, r_shadow_bumpscale_bumpmap.value);
			skinframe->nmap = R_LoadTexture2D (r_main_texturepool, va(vabuf, sizeof(vabuf), "%s_nmap", skinframe->basename), image_width, image_height, pixels, TEXTYPE_BGRA, (TEXF_ALPHA | textureflags) & (r_mipnormalmaps.integer ? ~0 : ~TEXF_MIPMAP) & (gl_texturecompression_normal.integer && gl_texturecompression.integer ? ~0 : ~TEXF_COMPRESS), mymiplevel, NULL);
			Mem_Free(pixels);
			Mem_Free(bumppixels);
		}
		else if (r_shadow_bumpscale_basetexture.value > 0)
		{
			pixels = (unsigned char *)Mem_Alloc(tempmempool, basepixels_width * basepixels_height * 4);
			Image_HeightmapToNormalmap_BGRA(basepixels, pixels, basepixels_width, basepixels_height, false, r_shadow_bumpscale_basetexture.value);
			skinframe->nmap = R_LoadTexture2D (r_main_texturepool, va(vabuf, sizeof(vabuf), "%s_nmap", skinframe->basename), basepixels_width, basepixels_height, pixels, TEXTYPE_BGRA, (TEXF_ALPHA | textureflags) & (r_mipnormalmaps.integer ? ~0 : ~TEXF_MIPMAP) & (gl_texturecompression_normal.integer && gl_texturecompression.integer ? ~0 : ~TEXF_COMPRESS), mymiplevel, NULL);
			Mem_Free(pixels);
		}
#ifndef USE_GLES2
		if (r_savedds && skinframe->nmap)
			R_SaveTextureDDSFile(skinframe->nmap, va(vabuf, sizeof(vabuf), "dds/%s_norm.dds", skinframe->basename), r_texture_dds_save.integer < 2, true);
#endif
	}

	// _luma is supported only for tenebrae compatibility
	// _blend and .blend are supported only for Q3 & QL compatibility, this hack can be removed if better Q3 shader support is implemented
	// _glow is the preferred name
	mymiplevel = savemiplevel;
	if (skinframe->glow == NULL && ((pixels = loadimagepixelsbgra(va(vabuf, sizeof(vabuf), "%s_glow", skinframe->basename), false, false, false, &mymiplevel)) || (pixels = loadimagepixelsbgra(va(vabuf, sizeof(vabuf), "%s.blend", skinframe->basename), false, false, false, &mymiplevel)) || (pixels = loadimagepixelsbgra(va(vabuf, sizeof(vabuf), "%s_blend", skinframe->basename), false, false, false, &mymiplevel)) || (pixels = loadimagepixelsbgra(va(vabuf, sizeof(vabuf), "%s_luma", skinframe->basename), false, false, false, &mymiplevel))))
	{
		skinframe->glow = R_LoadTexture2D (r_main_texturepool, va(vabuf, sizeof(vabuf), "%s_glow", skinframe->basename), image_width, image_height, pixels, vid.sRGB3D ? TEXTYPE_SRGB_BGRA : TEXTYPE_BGRA, textureflags & (gl_texturecompression_glow.integer && gl_texturecompression.integer ? ~0 : ~TEXF_COMPRESS), mymiplevel, NULL);
#ifndef USE_GLES2
		if (r_savedds && skinframe->glow)
			R_SaveTextureDDSFile(skinframe->glow, va(vabuf, sizeof(vabuf), "dds/%s_glow.dds", skinframe->basename), r_texture_dds_save.integer < 2, true);
#endif
		Mem_Free(pixels);pixels = NULL;
	}

	mymiplevel = savemiplevel;
	if (skinframe->gloss == NULL && r_loadgloss && (pixels = loadimagepixelsbgra(va(vabuf, sizeof(vabuf), "%s_gloss", skinframe->basename), false, false, false, &mymiplevel)))
	{
		skinframe->gloss = R_LoadTexture2D (r_main_texturepool, va(vabuf, sizeof(vabuf), "%s_gloss", skinframe->basename), image_width, image_height, pixels, vid.sRGB3D ? TEXTYPE_SRGB_BGRA : TEXTYPE_BGRA, (TEXF_ALPHA | textureflags) & (gl_texturecompression_gloss.integer && gl_texturecompression.integer ? ~0 : ~TEXF_COMPRESS), mymiplevel, NULL);
#ifndef USE_GLES2
		if (r_savedds && skinframe->gloss)
			R_SaveTextureDDSFile(skinframe->gloss, va(vabuf, sizeof(vabuf), "dds/%s_gloss.dds", skinframe->basename), r_texture_dds_save.integer < 2, true);
#endif
		Mem_Free(pixels);
		pixels = NULL;
	}

	mymiplevel = savemiplevel;
	if (skinframe->pants == NULL && (pixels = loadimagepixelsbgra(va(vabuf, sizeof(vabuf), "%s_pants", skinframe->basename), false, false, false, &mymiplevel)))
	{
		skinframe->pants = R_LoadTexture2D (r_main_texturepool, va(vabuf, sizeof(vabuf), "%s_pants", skinframe->basename), image_width, image_height, pixels, vid.sRGB3D ? TEXTYPE_SRGB_BGRA : TEXTYPE_BGRA, textureflags & (gl_texturecompression_color.integer && gl_texturecompression.integer ? ~0 : ~TEXF_COMPRESS), mymiplevel, NULL);
#ifndef USE_GLES2
		if (r_savedds && skinframe->pants)
			R_SaveTextureDDSFile(skinframe->pants, va(vabuf, sizeof(vabuf), "dds/%s_pants.dds", skinframe->basename), r_texture_dds_save.integer < 2, false);
#endif
		Mem_Free(pixels);
		pixels = NULL;
	}

	mymiplevel = savemiplevel;
	if (skinframe->shirt == NULL && (pixels = loadimagepixelsbgra(va(vabuf, sizeof(vabuf), "%s_shirt", skinframe->basename), false, false, false, &mymiplevel)))
	{
		skinframe->shirt = R_LoadTexture2D (r_main_texturepool, va(vabuf, sizeof(vabuf), "%s_shirt", skinframe->basename), image_width, image_height, pixels, vid.sRGB3D ? TEXTYPE_SRGB_BGRA : TEXTYPE_BGRA, textureflags & (gl_texturecompression_color.integer && gl_texturecompression.integer ? ~0 : ~TEXF_COMPRESS), mymiplevel, NULL);
#ifndef USE_GLES2
		if (r_savedds && skinframe->shirt)
			R_SaveTextureDDSFile(skinframe->shirt, va(vabuf, sizeof(vabuf), "dds/%s_shirt.dds", skinframe->basename), r_texture_dds_save.integer < 2, false);
#endif
		Mem_Free(pixels);
		pixels = NULL;
	}

	mymiplevel = savemiplevel;
	if (skinframe->reflect == NULL && (pixels = loadimagepixelsbgra(va(vabuf, sizeof(vabuf), "%s_reflect", skinframe->basename), false, false, false, &mymiplevel)))
	{
		skinframe->reflect = R_LoadTexture2D (r_main_texturepool, va(vabuf, sizeof(vabuf), "%s_reflect", skinframe->basename), image_width, image_height, pixels, vid.sRGB3D ? TEXTYPE_SRGB_BGRA : TEXTYPE_BGRA, textureflags & (gl_texturecompression_reflectmask.integer && gl_texturecompression.integer ? ~0 : ~TEXF_COMPRESS), mymiplevel, NULL);
#ifndef USE_GLES2
		if (r_savedds && skinframe->reflect)
			R_SaveTextureDDSFile(skinframe->reflect, va(vabuf, sizeof(vabuf), "dds/%s_reflect.dds", skinframe->basename), r_texture_dds_save.integer < 2, true);
#endif
		Mem_Free(pixels);
		pixels = NULL;
	}

	if (basepixels)
		Mem_Free(basepixels);

	return skinframe;
}

skinframe_t *R_SkinFrame_LoadInternalBGRA(const char *name, int textureflags, const unsigned char *skindata, int width, int height, int comparewidth, int compareheight, int comparecrc, qbool sRGB)
{
	int i;
	skinframe_t *skinframe;
	char vabuf[1024];

	if (cls.state == ca_dedicated)
		return NULL;

	// if already loaded just return it, otherwise make a new skinframe
	skinframe = R_SkinFrame_Find(name, textureflags, comparewidth, compareheight, comparecrc, true);
	if (skinframe->base)
		return skinframe;
	textureflags &= ~TEXF_FORCE_RELOAD;

	skinframe->stain = NULL;
	skinframe->merged = NULL;
	skinframe->base = NULL;
	skinframe->pants = NULL;
	skinframe->shirt = NULL;
	skinframe->nmap = NULL;
	skinframe->gloss = NULL;
	skinframe->glow = NULL;
	skinframe->fog = NULL;
	skinframe->reflect = NULL;
	skinframe->hasalpha = false;

	// if no data was provided, then clearly the caller wanted to get a blank skinframe
	if (!skindata)
		return NULL;

	if (developer_loading.integer)
		Con_Printf("loading 32bit skin \"%s\"\n", name);

	if (r_loadnormalmap && r_shadow_bumpscale_basetexture.value > 0)
	{
		unsigned char *a = (unsigned char *)Mem_Alloc(tempmempool, width * height * 8);
		unsigned char *b = a + width * height * 4;
		Image_HeightmapToNormalmap_BGRA(skindata, b, width, height, false, r_shadow_bumpscale_basetexture.value);
		skinframe->nmap = R_LoadTexture2D(r_main_texturepool, va(vabuf, sizeof(vabuf), "%s_nmap", skinframe->basename), width, height, b, TEXTYPE_BGRA, (textureflags | TEXF_ALPHA) & (r_mipnormalmaps.integer ? ~0 : ~TEXF_MIPMAP), -1, NULL);
		Mem_Free(a);
	}
	skinframe->base = skinframe->merged = R_LoadTexture2D(r_main_texturepool, skinframe->basename, width, height, skindata, sRGB ? TEXTYPE_SRGB_BGRA : TEXTYPE_BGRA, textureflags, -1, NULL);
	if (textureflags & TEXF_ALPHA)
	{
		for (i = 3;i < width * height * 4;i += 4)
		{
			if (skindata[i] < 255)
			{
				skinframe->hasalpha = true;
				break;
			}
		}
		if (r_loadfog && skinframe->hasalpha)
		{
			unsigned char *fogpixels = (unsigned char *)Mem_Alloc(tempmempool, width * height * 4);
			memcpy(fogpixels, skindata, width * height * 4);
			for (i = 0;i < width * height * 4;i += 4)
				fogpixels[i] = fogpixels[i+1] = fogpixels[i+2] = 255;
			skinframe->fog = R_LoadTexture2D(r_main_texturepool, va(vabuf, sizeof(vabuf), "%s_fog", skinframe->basename), width, height, fogpixels, TEXTYPE_BGRA, textureflags, -1, NULL);
			Mem_Free(fogpixels);
		}
	}

	R_SKINFRAME_LOAD_AVERAGE_COLORS(width * height, skindata[4 * pix + comp]);
	//Con_Printf("Texture %s has average colors %f %f %f alpha %f\n", name, skinframe->avgcolor[0], skinframe->avgcolor[1], skinframe->avgcolor[2], skinframe->avgcolor[3]);

	return skinframe;
}

skinframe_t *R_SkinFrame_LoadInternalQuake(const char *name, int textureflags, int loadpantsandshirt, int loadglowtexture, const unsigned char *skindata, int width, int height)
{
	int i;
	int featuresmask;
	skinframe_t *skinframe;

	if (cls.state == ca_dedicated)
		return NULL;

	// if already loaded just return it, otherwise make a new skinframe
	skinframe = R_SkinFrame_Find(name, textureflags, width, height, skindata ? CRC_Block(skindata, width*height) : 0, true);
	if (skinframe->base)
		return skinframe;
	//textureflags &= ~TEXF_FORCE_RELOAD;

	skinframe->stain = NULL;
	skinframe->merged = NULL;
	skinframe->base = NULL;
	skinframe->pants = NULL;
	skinframe->shirt = NULL;
	skinframe->nmap = NULL;
	skinframe->gloss = NULL;
	skinframe->glow = NULL;
	skinframe->fog = NULL;
	skinframe->reflect = NULL;
	skinframe->hasalpha = false;

	// if no data was provided, then clearly the caller wanted to get a blank skinframe
	if (!skindata)
		return NULL;

	if (developer_loading.integer)
		Con_Printf("loading quake skin \"%s\"\n", name);

	// we actually don't upload anything until the first use, because mdl skins frequently go unused, and are almost never used in both modes (colormapped and non-colormapped)
	skinframe->qpixels = (unsigned char *)Mem_Alloc(r_main_mempool, width*height); // FIXME LEAK
	memcpy(skinframe->qpixels, skindata, width*height);
	skinframe->qwidth = width;
	skinframe->qheight = height;

	featuresmask = 0;
	for (i = 0;i < width * height;i++)
		featuresmask |= palette_featureflags[skindata[i]];

	skinframe->hasalpha = false;
	// fence textures
	if (name[0] == '{')
		skinframe->hasalpha = true;
	skinframe->qhascolormapping = loadpantsandshirt && (featuresmask & (PALETTEFEATURE_PANTS | PALETTEFEATURE_SHIRT));
	skinframe->qgeneratenmap = r_shadow_bumpscale_basetexture.value > 0;
	skinframe->qgeneratemerged = true;
	skinframe->qgeneratebase = skinframe->qhascolormapping;
	skinframe->qgenerateglow = loadglowtexture && (featuresmask & PALETTEFEATURE_GLOW);

	R_SKINFRAME_LOAD_AVERAGE_COLORS(width * height, ((unsigned char *)palette_bgra_complete)[skindata[pix]*4 + comp]);
	//Con_Printf("Texture %s has average colors %f %f %f alpha %f\n", name, skinframe->avgcolor[0], skinframe->avgcolor[1], skinframe->avgcolor[2], skinframe->avgcolor[3]);

	return skinframe;
}

static void R_SkinFrame_GenerateTexturesFromQPixels(skinframe_t *skinframe, qbool colormapped)
{
	int width;
	int height;
	unsigned char *skindata;
	char vabuf[1024];

	if (!skinframe->qpixels)
		return;

	if (!skinframe->qhascolormapping)
		colormapped = false;

	if (colormapped)
	{
		if (!skinframe->qgeneratebase)
			return;
	}
	else
	{
		if (!skinframe->qgeneratemerged)
			return;
	}

	width = skinframe->qwidth;
	height = skinframe->qheight;
	skindata = skinframe->qpixels;

	if (skinframe->qgeneratenmap)
	{
		unsigned char *a, *b;
		skinframe->qgeneratenmap = false;
		a = (unsigned char *)Mem_Alloc(tempmempool, width * height * 8);
		b = a + width * height * 4;
		// use either a custom palette or the quake palette
		Image_Copy8bitBGRA(skindata, a, width * height, palette_bgra_complete);
		Image_HeightmapToNormalmap_BGRA(a, b, width, height, false, r_shadow_bumpscale_basetexture.value);
		skinframe->nmap = R_LoadTexture2D(r_main_texturepool, va(vabuf, sizeof(vabuf), "%s_nmap", skinframe->basename), width, height, b, TEXTYPE_BGRA, (skinframe->textureflags | TEXF_ALPHA) & (r_mipnormalmaps.integer ? ~0 : ~TEXF_MIPMAP), -1, NULL);
		Mem_Free(a);
	}

	if (skinframe->qgenerateglow)
	{
		skinframe->qgenerateglow = false;
		if (skinframe->hasalpha) // fence textures
			skinframe->glow = R_LoadTexture2D(r_main_texturepool, va(vabuf, sizeof(vabuf), "%s_glow", skinframe->basename), width, height, skindata, vid.sRGB3D ? TEXTYPE_SRGB_PALETTE : TEXTYPE_PALETTE, skinframe->textureflags | TEXF_ALPHA, -1, palette_bgra_onlyfullbrights_transparent); // glow
		else
			skinframe->glow = R_LoadTexture2D(r_main_texturepool, va(vabuf, sizeof(vabuf), "%s_glow", skinframe->basename), width, height, skindata, vid.sRGB3D ? TEXTYPE_SRGB_PALETTE : TEXTYPE_PALETTE, skinframe->textureflags, -1, palette_bgra_onlyfullbrights); // glow
	}

	if (colormapped)
	{
		skinframe->qgeneratebase = false;
		skinframe->base  = R_LoadTexture2D(r_main_texturepool, va(vabuf, sizeof(vabuf), "%s_nospecial", skinframe->basename), width, height, skindata, vid.sRGB3D ? TEXTYPE_SRGB_PALETTE : TEXTYPE_PALETTE, skinframe->textureflags, -1, skinframe->glow ? palette_bgra_nocolormapnofullbrights : palette_bgra_nocolormap);
		skinframe->pants = R_LoadTexture2D(r_main_texturepool, va(vabuf, sizeof(vabuf), "%s_pants", skinframe->basename), width, height, skindata, vid.sRGB3D ? TEXTYPE_SRGB_PALETTE : TEXTYPE_PALETTE, skinframe->textureflags, -1, palette_bgra_pantsaswhite);
		skinframe->shirt = R_LoadTexture2D(r_main_texturepool, va(vabuf, sizeof(vabuf), "%s_shirt", skinframe->basename), width, height, skindata, vid.sRGB3D ? TEXTYPE_SRGB_PALETTE : TEXTYPE_PALETTE, skinframe->textureflags, -1, palette_bgra_shirtaswhite);
	}
	else
	{
		skinframe->qgeneratemerged = false;
		if (skinframe->hasalpha) // fence textures
			skinframe->merged = R_LoadTexture2D(r_main_texturepool, skinframe->basename, width, height, skindata, vid.sRGB3D ? TEXTYPE_SRGB_PALETTE : TEXTYPE_PALETTE, skinframe->textureflags | TEXF_ALPHA, -1, skinframe->glow ? palette_bgra_nofullbrights_transparent : palette_bgra_transparent);
		else
			skinframe->merged = R_LoadTexture2D(r_main_texturepool, skinframe->basename, width, height, skindata, vid.sRGB3D ? TEXTYPE_SRGB_PALETTE : TEXTYPE_PALETTE, skinframe->textureflags, -1, skinframe->glow ? palette_bgra_nofullbrights : palette_bgra_complete);
	}

	if (!skinframe->qgeneratemerged && !skinframe->qgeneratebase)
	{
		Mem_Free(skinframe->qpixels);
		skinframe->qpixels = NULL;
	}
}

skinframe_t *R_SkinFrame_LoadInternal8bit(const char *name, int textureflags, const unsigned char *skindata, int width, int height, const unsigned int *palette, const unsigned int *alphapalette)
{
	int i;
	skinframe_t *skinframe;
	char vabuf[1024];

	if (cls.state == ca_dedicated)
		return NULL;

	// if already loaded just return it, otherwise make a new skinframe
	skinframe = R_SkinFrame_Find(name, textureflags, width, height, skindata ? CRC_Block(skindata, width*height) : 0, true);
	if (skinframe->base)
		return skinframe;
	textureflags &= ~TEXF_FORCE_RELOAD;

	skinframe->stain = NULL;
	skinframe->merged = NULL;
	skinframe->base = NULL;
	skinframe->pants = NULL;
	skinframe->shirt = NULL;
	skinframe->nmap = NULL;
	skinframe->gloss = NULL;
	skinframe->glow = NULL;
	skinframe->fog = NULL;
	skinframe->reflect = NULL;
	skinframe->hasalpha = false;

	// if no data was provided, then clearly the caller wanted to get a blank skinframe
	if (!skindata)
		return NULL;

	if (developer_loading.integer)
		Con_Printf("loading embedded 8bit image \"%s\"\n", name);

	skinframe->base = skinframe->merged = R_LoadTexture2D(r_main_texturepool, skinframe->basename, width, height, skindata, TEXTYPE_PALETTE, textureflags, -1, palette);
	if ((textureflags & TEXF_ALPHA) && alphapalette)
	{
		for (i = 0;i < width * height;i++)
		{
			if (((unsigned char *)palette)[skindata[i]*4+3] < 255)
			{
				skinframe->hasalpha = true;
				break;
			}
		}
		if (r_loadfog && skinframe->hasalpha)
			skinframe->fog = R_LoadTexture2D(r_main_texturepool, va(vabuf, sizeof(vabuf), "%s_fog", skinframe->basename), width, height, skindata, TEXTYPE_PALETTE, textureflags, -1, alphapalette);
	}

	R_SKINFRAME_LOAD_AVERAGE_COLORS(width * height, ((unsigned char *)palette)[skindata[pix]*4 + comp]);
	//Con_Printf("Texture %s has average colors %f %f %f alpha %f\n", name, skinframe->avgcolor[0], skinframe->avgcolor[1], skinframe->avgcolor[2], skinframe->avgcolor[3]);

	return skinframe;
}

skinframe_t *R_SkinFrame_LoadMissing(void)
{
	skinframe_t *skinframe;

	if (cls.state == ca_dedicated)
		return NULL;

	skinframe = R_SkinFrame_Find("missing", TEXF_FORCENEAREST, 0, 0, 0, true);
	skinframe->stain = NULL;
	skinframe->merged = NULL;
	skinframe->base = NULL;
	skinframe->pants = NULL;
	skinframe->shirt = NULL;
	skinframe->nmap = NULL;
	skinframe->gloss = NULL;
	skinframe->glow = NULL;
	skinframe->fog = NULL;
	skinframe->reflect = NULL;
	skinframe->hasalpha = false;

	skinframe->avgcolor[0] = rand() / RAND_MAX;
	skinframe->avgcolor[1] = rand() / RAND_MAX;
	skinframe->avgcolor[2] = rand() / RAND_MAX;
	skinframe->avgcolor[3] = 1;

	return skinframe;
}

skinframe_t *R_SkinFrame_LoadNoTexture(void)
{
	if (cls.state == ca_dedicated)
		return NULL;

	return R_SkinFrame_LoadInternalBGRA("notexture", TEXF_FORCENEAREST, Image_GenerateNoTexture(), 16, 16, 0, 0, 0, false);
}

skinframe_t *R_SkinFrame_LoadInternalUsingTexture(const char *name, int textureflags, rtexture_t *tex, int width, int height, qbool sRGB)
{
	skinframe_t *skinframe;
	if (cls.state == ca_dedicated)
		return NULL;
	// if already loaded just return it, otherwise make a new skinframe
	skinframe = R_SkinFrame_Find(name, textureflags, width, height, 0, true);
	if (skinframe->base)
		return skinframe;
	textureflags &= ~TEXF_FORCE_RELOAD;
	skinframe->stain = NULL;
	skinframe->merged = NULL;
	skinframe->base = NULL;
	skinframe->pants = NULL;
	skinframe->shirt = NULL;
	skinframe->nmap = NULL;
	skinframe->gloss = NULL;
	skinframe->glow = NULL;
	skinframe->fog = NULL;
	skinframe->reflect = NULL;
	skinframe->hasalpha = (textureflags & TEXF_ALPHA) != 0;
	// if no data was provided, then clearly the caller wanted to get a blank skinframe
	if (!tex)
		return NULL;
	if (developer_loading.integer)
		Con_Printf("loading 32bit skin \"%s\"\n", name);
	skinframe->base = skinframe->merged = tex;
	Vector4Set(skinframe->avgcolor, 1, 1, 1, 1); // bogus placeholder
	return skinframe;
}

//static char *suffix[6] = {"ft", "bk", "rt", "lf", "up", "dn"};
typedef struct suffixinfo_s
{
	const char *suffix;
	qbool flipx, flipy, flipdiagonal;
}
suffixinfo_t;
static suffixinfo_t suffix[3][6] =
{
	{
		{"px",   false, false, false},
		{"nx",   false, false, false},
		{"py",   false, false, false},
		{"ny",   false, false, false},
		{"pz",   false, false, false},
		{"nz",   false, false, false}
	},
	{
		{"posx", false, false, false},
		{"negx", false, false, false},
		{"posy", false, false, false},
		{"negy", false, false, false},
		{"posz", false, false, false},
		{"negz", false, false, false}
	},
	{
		{"rt",    true, false,  true},
		{"lf",   false,  true,  true},
		{"ft",    true,  true, false},
		{"bk",   false, false, false},
		{"up",    true, false,  true},
		{"dn",    true, false,  true}
	}
};

static int componentorder[4] = {0, 1, 2, 3};

static rtexture_t *R_LoadCubemap(const char *basename)
{
	int i, j, cubemapsize, forcefilter;
	unsigned char *cubemappixels, *image_buffer;
	rtexture_t *cubemaptexture;
	char name[256];

	// HACK: if the cubemap name starts with a !, the cubemap is nearest-filtered
	forcefilter = TEXF_FORCELINEAR;
	if (basename && basename[0] == '!')
	{
		basename++;
		forcefilter = TEXF_FORCENEAREST;
	}
	// must start 0 so the first loadimagepixels has no requested width/height
	cubemapsize = 0;
	cubemappixels = NULL;
	cubemaptexture = NULL;
	// keep trying different suffix groups (posx, px, rt) until one loads
	for (j = 0;j < 3 && !cubemappixels;j++)
	{
		// load the 6 images in the suffix group
		for (i = 0;i < 6;i++)
		{
			// generate an image name based on the base and and suffix
			dpsnprintf(name, sizeof(name), "%s%s", basename, suffix[j][i].suffix);
			// load it
			if ((image_buffer = loadimagepixelsbgra(name, false, false, false, NULL)))
			{
				// an image loaded, make sure width and height are equal
				if (image_width == image_height && (!cubemappixels || image_width == cubemapsize))
				{
					// if this is the first image to load successfully, allocate the cubemap memory
					if (!cubemappixels && image_width >= 1)
					{
						cubemapsize = image_width;
						// note this clears to black, so unavailable sides are black
						cubemappixels = (unsigned char *)Mem_Alloc(tempmempool, 6*cubemapsize*cubemapsize*4);
					}
					// copy the image with any flipping needed by the suffix (px and posx types don't need flipping)
					if (cubemappixels)
						Image_CopyMux(cubemappixels+i*cubemapsize*cubemapsize*4, image_buffer, cubemapsize, cubemapsize, suffix[j][i].flipx, suffix[j][i].flipy, suffix[j][i].flipdiagonal, 4, 4, componentorder);
				}
				else
					Con_Printf("Cubemap image \"%s\" (%ix%i) is not square, OpenGL requires square cubemaps.\n", name, image_width, image_height);
				// free the image
				Mem_Free(image_buffer);
			}
		}
	}
	// if a cubemap loaded, upload it
	if (cubemappixels)
	{
		if (developer_loading.integer)
			Con_Printf("loading cubemap \"%s\"\n", basename);

		cubemaptexture = R_LoadTextureCubeMap(r_main_texturepool, basename, cubemapsize, cubemappixels, vid.sRGB3D ? TEXTYPE_SRGB_BGRA : TEXTYPE_BGRA, (gl_texturecompression_lightcubemaps.integer && gl_texturecompression.integer ? TEXF_COMPRESS : 0) | forcefilter | TEXF_CLAMP, -1, NULL);
		Mem_Free(cubemappixels);
	}
	else
	{
		Con_DPrintf("failed to load cubemap \"%s\"\n", basename);
		if (developer_loading.integer)
		{
			Con_Printf("(tried tried images ");
			for (j = 0;j < 3;j++)
				for (i = 0;i < 6;i++)
					Con_Printf("%s\"%s%s.tga\"", j + i > 0 ? ", " : "", basename, suffix[j][i].suffix);
			Con_Print(" and was unable to find any of them).\n");
		}
	}
	return cubemaptexture;
}

rtexture_t *R_GetCubemap(const char *basename)
{
	int i;
	for (i = 0;i < r_texture_numcubemaps;i++)
		if (r_texture_cubemaps[i] != NULL)
			if (!strcasecmp(r_texture_cubemaps[i]->basename, basename))
				return r_texture_cubemaps[i]->texture ? r_texture_cubemaps[i]->texture : r_texture_whitecube;
	if (i >= MAX_CUBEMAPS || !r_main_mempool)
		return r_texture_whitecube;
	r_texture_numcubemaps++;
	r_texture_cubemaps[i] = (cubemapinfo_t *)Mem_Alloc(r_main_mempool, sizeof(cubemapinfo_t));
	dp_strlcpy(r_texture_cubemaps[i]->basename, basename, sizeof(r_texture_cubemaps[i]->basename));
	r_texture_cubemaps[i]->texture = R_LoadCubemap(r_texture_cubemaps[i]->basename);
	return r_texture_cubemaps[i]->texture;
}

static void R_Main_FreeViewCache(void)
{
	if (r_refdef.viewcache.entityvisible)
		Mem_Free(r_refdef.viewcache.entityvisible);
	if (r_refdef.viewcache.world_pvsbits)
		Mem_Free(r_refdef.viewcache.world_pvsbits);
	if (r_refdef.viewcache.world_leafvisible)
		Mem_Free(r_refdef.viewcache.world_leafvisible);
	if (r_refdef.viewcache.world_surfacevisible)
		Mem_Free(r_refdef.viewcache.world_surfacevisible);
	memset(&r_refdef.viewcache, 0, sizeof(r_refdef.viewcache));
}

static void R_Main_ResizeViewCache(void)
{
	int numentities = r_refdef.scene.numentities;
	int numleafs = r_refdef.scene.worldmodel ? r_refdef.scene.worldmodel->brush.num_leafs : 1;
	int numsurfaces = r_refdef.scene.worldmodel ? r_refdef.scene.worldmodel->num_surfaces : 1;
	if (r_refdef.viewcache.maxentities < numentities)
	{
		r_refdef.viewcache.maxentities = numentities;
		if (r_refdef.viewcache.entityvisible)
			Mem_Free(r_refdef.viewcache.entityvisible);
		r_refdef.viewcache.entityvisible = (unsigned char *)Mem_Alloc(r_main_mempool, r_refdef.viewcache.maxentities);
	}
	// bones_was_here: r_refdef.viewcache.world_pvsbits was (re)allocated here, now done in Mod_BSP_FatPVS()
	if (r_refdef.viewcache.world_numleafs != numleafs)
	{
		r_refdef.viewcache.world_numleafs = numleafs;
		if (r_refdef.viewcache.world_leafvisible)
			Mem_Free(r_refdef.viewcache.world_leafvisible);
		r_refdef.viewcache.world_leafvisible = (unsigned char *)Mem_Alloc(r_main_mempool, r_refdef.viewcache.world_numleafs);
	}
	if (r_refdef.viewcache.world_numsurfaces != numsurfaces)
	{
		r_refdef.viewcache.world_numsurfaces = numsurfaces;
		if (r_refdef.viewcache.world_surfacevisible)
			Mem_Free(r_refdef.viewcache.world_surfacevisible);
		r_refdef.viewcache.world_surfacevisible = (unsigned char *)Mem_Alloc(r_main_mempool, r_refdef.viewcache.world_numsurfaces);
	}
}

extern rtexture_t *loadingscreentexture;
static void gl_main_start(void)
{
	loadingscreentexture = NULL;
	r_texture_blanknormalmap = NULL;
	r_texture_white = NULL;
	r_texture_white3d = NULL;
	r_texture_grey128 = NULL;
	r_texture_black = NULL;
	r_texture_whitecube = NULL;
	r_texture_normalizationcube = NULL;
	r_texture_fogattenuation = NULL;
	r_texture_fogheighttexture = NULL;
	r_texture_gammaramps = NULL;
	r_texture_numcubemaps = 0;
	r_uniformbufferalignment = 32;

	r_loaddds = r_texture_dds_load.integer != 0;
	r_savedds = vid.support.ext_texture_compression_s3tc && r_texture_dds_save.integer;

	switch(vid.renderpath)
	{
	case RENDERPATH_METAL:
		// METAL: same content-loading policy as GL32; the UBO alignment query is
		// GL-only (Metal constant-buffer offsets are handled in metal_backend).
		r_loadnormalmap = true;
		r_loadgloss = true;
		r_loadfog = false;
		break;
	case RENDERPATH_GL32:
	case RENDERPATH_GLES2:
		r_loadnormalmap = true;
		r_loadgloss = true;
		r_loadfog = false;
#ifdef GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT
		qglGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &r_uniformbufferalignment);
#endif
		break;
	}

	R_AnimCache_Free();
	R_FrameData_Reset();
	R_BufferData_Reset();

	r_numqueries = 0;
	r_maxqueries = 0;
	memset(r_queries, 0, sizeof(r_queries));

	r_qwskincache = NULL;
	r_qwskincache_size = 0;

	// due to caching of texture_t references, the collision cache must be reset
	Collision_Cache_Reset(true);

	// set up r_skinframe loading system for textures
	memset(&r_skinframe, 0, sizeof(r_skinframe));
	r_skinframe.loadsequence = 1;
	Mem_ExpandableArray_NewArray(&r_skinframe.array, r_main_mempool, sizeof(skinframe_t), 256);

	r_main_texturepool = R_AllocTexturePool();
	R_BuildBlankTextures();
	R_BuildNoTexture();
	R_BuildWhiteCube();
#ifndef USE_GLES2
	R_BuildNormalizationCube();
#endif //USE_GLES2
	r_texture_fogattenuation = NULL;
	r_texture_fogheighttexture = NULL;
	r_texture_gammaramps = NULL;
	//r_texture_fogintensity = NULL;
	memset(&r_fb, 0, sizeof(r_fb));
	Mem_ExpandableArray_NewArray(&r_fb.rendertargets, r_main_mempool, sizeof(r_rendertarget_t), 128);
	r_glsl_permutation = NULL;
	memset(r_glsl_permutationhash, 0, sizeof(r_glsl_permutationhash));
	Mem_ExpandableArray_NewArray(&r_glsl_permutationarray, r_main_mempool, sizeof(r_glsl_permutation_t), 256);
	memset(&r_svbsp, 0, sizeof (r_svbsp));

	memset(r_texture_cubemaps, 0, sizeof(r_texture_cubemaps));
	r_texture_numcubemaps = 0;

	r_refdef.fogmasktable_density = 0;

#ifdef __ANDROID__
	// For Steelstorm Android
	// FIXME CACHE the program and reload
	// FIXME see possible combinations for SS:BR android
	Con_DPrintf("Compiling most used shaders for SS:BR android... START\n");
	R_SetupShader_SetPermutationGLSL(0, 12);
	R_SetupShader_SetPermutationGLSL(0, 13);
	R_SetupShader_SetPermutationGLSL(0, 8388621);
	R_SetupShader_SetPermutationGLSL(3, 0);
	R_SetupShader_SetPermutationGLSL(3, 2048);
	R_SetupShader_SetPermutationGLSL(5, 0);
	R_SetupShader_SetPermutationGLSL(5, 2);
	R_SetupShader_SetPermutationGLSL(5, 2048);
	R_SetupShader_SetPermutationGLSL(5, 8388608);
	R_SetupShader_SetPermutationGLSL(11, 1);
	R_SetupShader_SetPermutationGLSL(11, 2049);
	R_SetupShader_SetPermutationGLSL(11, 8193);
	R_SetupShader_SetPermutationGLSL(11, 10241);
	Con_DPrintf("Compiling most used shaders for SS:BR android... END\n");
#endif
}

extern unsigned int r_shadow_occlusion_buf;

static void R_Volumetric_FreeField(void);
// drops both volumetric textures without freeing them, for the paths that tear the
// whole texture pool down underneath us
static void R_Volumetric_ForgetTextures(void);
static void R_Volumetric_Probe_f(cmd_state_t *cmd);

static void R_ReactiveStamp_Forget(void);   // the reactive stamp's stash, defined with the mask pass below

static void gl_main_shutdown(void)
{
	R_RenderTarget_FreeUnused(true);
	Mem_ExpandableArray_FreeArray(&r_fb.rendertargets);
	R_AnimCache_Free();
	R_FrameData_Reset();
	R_BufferData_Reset();

	R_Main_FreeViewCache();

	switch(vid.renderpath)
	{
	case RENDERPATH_METAL:
		break; // METAL_TODO: Phase 0 stub (METAL.md)
	case RENDERPATH_GL32:
	case RENDERPATH_GLES2:
#if defined(GL_SAMPLES_PASSED) && !defined(USE_GLES2)
		if (r_maxqueries)
			qglDeleteQueries(r_maxqueries, r_queries);
#endif
		break;
	}
	r_shadow_occlusion_buf = 0;
	r_numqueries = 0;
	r_maxqueries = 0;
	memset(r_queries, 0, sizeof(r_queries));

	r_qwskincache = NULL;
	r_qwskincache_size = 0;

	// clear out the r_skinframe state
	Mem_ExpandableArray_FreeArray(&r_skinframe.array);
	memset(&r_skinframe, 0, sizeof(r_skinframe));

	if (r_svbsp.nodes)
		Mem_Free(r_svbsp.nodes);
	memset(&r_svbsp, 0, sizeof (r_svbsp));
	// both volumetric textures live in r_main_texturepool, so they die with it; drop
	// the cached pointers first or the next map hands the shader freed textures
	R_Volumetric_ForgetTextures();
	R_FreeTexturePool(&r_main_texturepool);
	loadingscreentexture = NULL;
	r_texture_blanknormalmap = NULL;
	r_texture_white = NULL;
	r_texture_white3d = NULL;
	r_texture_grey128 = NULL;
	r_texture_black = NULL;
	r_texture_whitecube = NULL;
	r_texture_normalizationcube = NULL;
	r_texture_fogattenuation = NULL;
	r_texture_fogheighttexture = NULL;
	r_texture_gammaramps = NULL;
	r_texture_numcubemaps = 0;
	//r_texture_fogintensity = NULL;
	memset(&r_fb, 0, sizeof(r_fb));
	R_ReactiveStamp_Forget();
	R_GLSL_Restart_f(cmd_local);

	r_glsl_permutation = NULL;
	memset(r_glsl_permutationhash, 0, sizeof(r_glsl_permutationhash));
	Mem_ExpandableArray_FreeArray(&r_glsl_permutationarray);
}

static void gl_main_newmap(void)
{
	// FIXME: move this code to client
	char *entities, entname[MAX_QPATH];
	// FIRST, because the entity-lump branch below returns early: the baked floor and
	// liquid field describes the old world and must not survive into the new one
	R_Volumetric_FreeField();
	if (r_qwskincache)
		Mem_Free(r_qwskincache);
	r_qwskincache = NULL;
	r_qwskincache_size = 0;
	if (cl.worldmodel)
	{
		dpsnprintf(entname, sizeof(entname), "%s.ent", cl.worldnamenoextension);
		if ((entities = (char *)FS_LoadFile(entname, tempmempool, true, NULL)))
		{
			CL_ParseEntityLump(entities);
			Mem_Free(entities);
			return;
		}
		if (cl.worldmodel->brush.entities)
			CL_ParseEntityLump(cl.worldmodel->brush.entities);
	}
	R_Main_FreeViewCache();

	R_FrameData_Reset();
	R_BufferData_Reset();
}

/*
The legacy "r g b" colour cvars are convenience SETTERS for the per-channel cvars; the
channels are the source of truth and are the ones that get archived.

This exists because the console only ever assigns the FIRST token of an unquoted line --
Cvar_Command does Cvar_SetQuick(v, Cmd_Argv(cmd, 1)) and silently drops the rest. So
`r_volumetric_color 0.90 0.00 0.00` set the whole colour string to "0.90", and the old
parser then filled green and blue from unrelated built-in defaults, giving a confident
pink. Every attempt to pick a shade came back red or blue and no amount of retyping
helped. Per-channel cvars need no quoting and cannot half-apply.
*/
static void R_Volumetric_SplitColorCvar(cvar_t *var, cvar_t *r, cvar_t *g, cvar_t *b)
{
	float v[3];
	int n = 0;

	if (var->string && *var->string)
		n = sscanf(var->string, "%f %f %f", &v[0], &v[1], &v[2]);
	if (n < 1)
		return;
	// a partial value repeats the last component supplied, so "0.9" is grey -- wrong in a
	// way you can see, rather than a plausible blend with whatever the defaults were
	if (n < 2) v[1] = v[0];
	if (n < 3) v[2] = v[1];
	Cvar_SetValueQuick(r, v[0]);
	Cvar_SetValueQuick(g, v[1]);
	Cvar_SetValueQuick(b, v[2]);
}

static void R_Volumetric_ColorCallback(cvar_t *var)
{
	R_Volumetric_SplitColorCvar(var, &r_volumetric_color_red, &r_volumetric_color_green, &r_volumetric_color_blue);
}
static void R_Volumetric_WaterColorCallback(cvar_t *var)
{
	R_Volumetric_SplitColorCvar(var, &r_volumetric_watercolor_red, &r_volumetric_watercolor_green, &r_volumetric_watercolor_blue);
}
static void R_Volumetric_SlimeColorCallback(cvar_t *var)
{
	R_Volumetric_SplitColorCvar(var, &r_volumetric_slimecolor_red, &r_volumetric_slimecolor_green, &r_volumetric_slimecolor_blue);
}
static void R_Volumetric_LavaColorCallback(cvar_t *var)
{
	R_Volumetric_SplitColorCvar(var, &r_volumetric_lavacolor_red, &r_volumetric_lavacolor_green, &r_volumetric_lavacolor_blue);
}

static void R_Volumetric_GroundColorCallback(cvar_t *var)
{
	R_Volumetric_SplitColorCvar(var, &r_volumetric_groundcolor_red, &r_volumetric_groundcolor_green, &r_volumetric_groundcolor_blue);
}

void GL_Main_Init(void)
{
	int i;
	r_main_mempool = Mem_AllocPool("Renderer", 0, NULL);
	R_InitShaderModeInfo();

	Cmd_AddCommand(CF_CLIENT, "r_glsl_restart", R_GLSL_Restart_f, "unloads GLSL shaders, they will then be reloaded as needed");
	Cmd_AddCommand(CF_CLIENT, "r_glsl_dumpshader", R_GLSL_DumpShader_f, "dumps the engine internal default.glsl shader into glsl/default.glsl");
	// FIXME: the client should set up r_refdef.fog stuff including the fogmasktable
	if (gamemode == GAME_NEHAHRA)
	{
		Cvar_RegisterVariable (&gl_fogenable);
		Cvar_RegisterVariable (&gl_fogdensity);
		Cvar_RegisterVariable (&gl_fogred);
		Cvar_RegisterVariable (&gl_foggreen);
		Cvar_RegisterVariable (&gl_fogblue);
		Cvar_RegisterVariable (&gl_fogstart);
		Cvar_RegisterVariable (&gl_fogend);
		Cvar_RegisterVariable (&gl_skyclip);
	}
	Cvar_RegisterVariable(&r_motionblur);
	Cvar_RegisterVariable(&r_damageblur);
	Cvar_RegisterVariable(&r_motionblur_averaging);
	Cvar_RegisterVariable(&r_motionblur_randomize);
	Cvar_RegisterVariable(&r_motionblur_minblur);
	Cvar_RegisterVariable(&r_motionblur_maxblur);
	Cvar_RegisterVariable(&r_motionblur_velocityfactor);
	Cvar_RegisterVariable(&r_motionblur_velocityfactor_minspeed);
	Cvar_RegisterVariable(&r_motionblur_velocityfactor_maxspeed);
	Cvar_RegisterVariable(&r_motionblur_mousefactor);
	Cvar_RegisterVariable(&r_motionblur_mousefactor_minspeed);
	Cvar_RegisterVariable(&r_motionblur_mousefactor_maxspeed);
	Cvar_RegisterVariable(&r_depthfirst);
	Cvar_RegisterVariable(&r_useinfinitefarclip);
	Cvar_RegisterVariable(&r_farclip_base);
	Cvar_RegisterVariable(&r_farclip_world);
	Cvar_RegisterVariable(&r_nearclip);
	Cvar_RegisterVariable(&r_deformvertexes);
	Cvar_RegisterVariable(&r_transparent);
	Cvar_RegisterVariable(&r_transparent_alphatocoverage);
	Cvar_RegisterVariable(&r_transparent_sortsurfacesbynearest);
	Cvar_RegisterVariable(&r_transparent_useplanardistance);
	Cvar_RegisterVariable(&r_showoverdraw);
	Cvar_RegisterVariable(&r_showbboxes);
	Cvar_RegisterVariable(&r_showbboxes_client);
	Cvar_RegisterVariable(&r_showsurfaces);
	Cvar_RegisterVariable(&r_showtris);
	Cvar_RegisterVariable(&r_shownormals);
	Cvar_RegisterVariable(&r_showlighting);
	Cvar_RegisterVariable(&r_showcollisionbrushes);
	Cvar_RegisterVariable(&r_showcollisionbrushes_polygonfactor);
	Cvar_RegisterVariable(&r_showcollisionbrushes_polygonoffset);
	Cvar_RegisterVariable(&r_showdisabledepthtest);
	Cvar_RegisterVariable(&r_showspriteedges);
	Cvar_RegisterVariable(&r_showparticleedges);
	Cvar_RegisterVariable(&r_drawportals);
	Cvar_RegisterVariable(&r_drawentities);
	Cvar_RegisterVariable(&r_draw2d);
	Cvar_RegisterVariable(&r_drawworld);
	Cvar_RegisterVariable(&r_cullentities_trace);
	Cvar_RegisterVariable(&r_cullentities_trace_entityocclusion);
	Cvar_RegisterVariable(&r_cullentities_trace_samples);
	Cvar_RegisterVariable(&r_cullentities_trace_tempentitysamples);
	Cvar_RegisterVariable(&r_cullentities_trace_enlarge);
	Cvar_RegisterVariable(&r_cullentities_trace_expand);
	Cvar_RegisterVariable(&r_cullentities_trace_pad);
	Cvar_RegisterVariable(&r_cullentities_trace_delay);
	Cvar_RegisterVariable(&r_cullentities_trace_eyejitter);
	Cvar_RegisterVariable(&r_sortentities);
	Cvar_RegisterVariable(&r_drawviewmodel);
	Cvar_RegisterVariable(&r_drawexteriormodel);
	Cvar_RegisterVariable(&r_speeds);
	Cvar_RegisterVariable(&r_fullbrights);
	Cvar_RegisterVariable(&r_wateralpha);
	Cvar_RegisterVariable(&r_wateralpha_force);
	Cvar_RegisterVariable(&r_dynamic);
	Cvar_RegisterVariable(&r_fullbright_directed);
	Cvar_RegisterVariable(&r_fullbright_directed_ambient);
	Cvar_RegisterVariable(&r_fullbright_directed_diffuse);
	Cvar_RegisterVariable(&r_fullbright_directed_pitch);
	Cvar_RegisterVariable(&r_fullbright_directed_pitch_relative);
	Cvar_RegisterVariable(&r_fullbright);
	Cvar_RegisterVariable(&r_shadows);
	Cvar_RegisterVariable(&r_shadows_darken);
	Cvar_RegisterVariable(&r_shadows_drawafterrtlighting);
	Cvar_RegisterVariable(&r_shadows_castfrombmodels);
	Cvar_RegisterVariable(&r_shadows_throwdistance);
	Cvar_RegisterVariable(&r_shadows_throwdirection);
	Cvar_RegisterVariable(&r_shadows_focus);
	Cvar_RegisterVariable(&r_shadows_shadowmapscale);
	Cvar_RegisterVariable(&r_shadows_shadowmapbias);
	Cvar_RegisterVariable(&r_q1bsp_skymasking);
	Cvar_RegisterVariable(&r_polygonoffset_submodel_factor);
	Cvar_RegisterVariable(&r_polygonoffset_submodel_offset);
	Cvar_RegisterVariable(&r_polygonoffset_decals_factor);
	Cvar_RegisterVariable(&r_polygonoffset_decals_offset);
	Cvar_RegisterVariable(&r_fog_exp2);
	Cvar_RegisterVariable(&r_fog_clear);
	Cvar_RegisterVariable(&r_drawfog);
	Cvar_RegisterVariable(&r_volumetric);
	Cvar_RegisterVariable(&m5_packfog);
	Cvar_RegisterVariable(&r_volumetric_density);
	Cvar_RegisterVariable(&r_volumetric_height);
	Cvar_RegisterVariable(&r_volumetric_heightbase);
	Cvar_RegisterVariable(&r_volumetric_noisescale);
	Cvar_RegisterVariable(&r_volumetric_noisethresh);
	Cvar_RegisterVariable(&r_volumetric_wind);
	Cvar_RegisterVariable(&r_volumetric_color);
	Cvar_RegisterVariable(&r_volumetric_steps);
	Cvar_RegisterVariable(&r_volumetric_dist);
	Cvar_RegisterVariable(&r_volumetric_scale);
	Cvar_RegisterVariable(&r_volumetric_floor);
	Cvar_RegisterVariable(&r_volumetric_flooroffset);
	Cvar_RegisterVariable(&r_volumetric_fieldcell);
	Cvar_RegisterVariable(&r_volumetric_water);
	Cvar_RegisterVariable(&r_volumetric_waterdensity);
	Cvar_RegisterVariable(&r_volumetric_slimedensity);
	Cvar_RegisterVariable(&r_volumetric_lavadensity);
	Cvar_RegisterVariable(&r_volumetric_corner);
	Cvar_RegisterVariable(&r_volumetric_watermist);
	Cvar_RegisterVariable(&r_volumetric_mistlavacut);
	Cvar_RegisterVariable(&r_volumetric_liquidfloor);
	Cvar_RegisterVariable(&r_volumetric_mistheight);
	Cvar_RegisterVariable(&r_volumetric_ground);
	Cvar_RegisterVariable(&r_volumetric_groundheight);
	Cvar_RegisterVariable(&r_volumetric_grounddeform);
	Cvar_RegisterVariable(&r_volumetric_groundnoisescale);
	Cvar_RegisterVariable(&r_volumetric_groundthresh);
	Cvar_RegisterVariable(&r_volumetric_groundoffset);
	Cvar_RegisterVariable(&r_volumetric_groundwind);
	Cvar_RegisterVariable(&r_volumetric_groundcolor);
	Cvar_RegisterVariable(&r_volumetric_watercolor);
	Cvar_RegisterVariable(&r_volumetric_slimecolor);
	Cvar_RegisterVariable(&r_volumetric_lavacolor);
	Cvar_RegisterVariable(&r_volumetric_color_red);
	Cvar_RegisterVariable(&r_volumetric_color_green);
	Cvar_RegisterVariable(&r_volumetric_color_blue);
	Cvar_RegisterVariable(&r_volumetric_watercolor_red);
	Cvar_RegisterVariable(&r_volumetric_watercolor_green);
	Cvar_RegisterVariable(&r_volumetric_watercolor_blue);
	Cvar_RegisterVariable(&r_volumetric_slimecolor_red);
	Cvar_RegisterVariable(&r_volumetric_slimecolor_green);
	Cvar_RegisterVariable(&r_volumetric_slimecolor_blue);
	Cvar_RegisterVariable(&r_volumetric_lavacolor_red);
	Cvar_RegisterVariable(&r_volumetric_lavacolor_green);
	Cvar_RegisterVariable(&r_volumetric_lavacolor_blue);
	Cvar_RegisterVariable(&r_volumetric_groundcolor_red);
	Cvar_RegisterVariable(&r_volumetric_groundcolor_green);
	Cvar_RegisterVariable(&r_volumetric_groundcolor_blue);
	Cvar_RegisterVariable(&r_volumetric_particles);
	Cvar_RegisterVariable(&r_volumetric_particles_alpha);
	Cvar_RegisterVariable(&r_volumetric_liquidfade);
	Cvar_RegisterVariable(&r_volumetric_lavaglow);
	Cvar_RegisterVariable(&r_volumetric_skyfog);
	Cvar_RegisterVariable(&r_volumetric_ambient);
	Cvar_RegisterVariable(&r_volumetric_ambientgain);
	Cvar_RegisterVariable(&r_volumetric_ambientfloor);
	Cvar_RegisterVariable(&r_volumetric_ambientdilate);
	Cvar_RegisterVariable(&r_volumetric_irrcell);
	Cvar_RegisterVariable(&r_volumetric_dlight);
	Cvar_RegisterVariable(&r_volumetric_dlight_g);
	Cvar_RegisterVariable(&r_volumetric_scatter);
	Cvar_RegisterVariable(&r_volumetric_extinction);
	Cvar_RegisterVariable(&r_volumetric_swirl);
	Cvar_RegisterVariable(&r_volumetric_swirlscale);
	Cvar_RegisterVariable(&r_volumetric_swirlkh);
	Cvar_RegisterVariable(&r_volumetric_noise2);
	Cvar_RegisterVariable(&r_volumetric_noisesize);
	Cvar_RegisterVariable(&r_volumetric_noise2_octaves);
	Cvar_RegisterVariable(&r_volumetric_noise2_warp);
	Cvar_RegisterVariable(&r_volumetric_noise2_clump);
	Cvar_RegisterVariable(&r_volumetric_noise2_ridge);
	Cvar_RegisterVariable(&r_volumetric_noise2_contrast);
	// after the channels exist, so a config that still sets the old string form lands
	Cvar_RegisterCallback(&r_volumetric_color, R_Volumetric_ColorCallback);
	Cvar_RegisterCallback(&r_volumetric_watercolor, R_Volumetric_WaterColorCallback);
	Cvar_RegisterCallback(&r_volumetric_slimecolor, R_Volumetric_SlimeColorCallback);
	Cvar_RegisterCallback(&r_volumetric_lavacolor, R_Volumetric_LavaColorCallback);
	Cvar_RegisterCallback(&r_volumetric_groundcolor, R_Volumetric_GroundColorCallback);
	Cvar_RegisterVariable(&r_volumetric_debug);
	Cvar_RegisterVariable(&r_redglow);
	Cvar_RegisterVariable(&r_redglow_threshold);
	Cvar_RegisterVariable(&r_redglow_minlevel);
	Cvar_RegisterVariable(&r_gamma_analytic);
	Cvar_RegisterVariable(&r_edr);
	Cmd_AddCommand(CF_CLIENT, "r_gamma_analytic_test", R_GammaAnalyticTest_f, "compare the analytic gamma curve against the shipped 256-entry LUT at 256 points (METAL.md Phase 7-1)");
	Cvar_RegisterVariable(&r_lavaboil);
	Cvar_RegisterVariable(&r_waterswirl);
	Cvar_RegisterVariable(&r_watersurface);
	Cvar_RegisterVariable(&cl_particles_soft);
	Cvar_RegisterVariable(&r_caustics);
	Cvar_RegisterVariable(&r_caustics_scale);
	Cvar_RegisterVariable(&r_caustics_speed);
	Cvar_RegisterVariable(&r_watersurface_distort);
	Cvar_RegisterVariable(&r_watersurface_speed);
	Cvar_RegisterVariable(&r_watersurface_warp);
	Cvar_RegisterVariable(&r_watersurface_bump);
	Cvar_RegisterVariable(&r_watersurface_guard);
	Cvar_RegisterVariable(&r_watersurface_opacity);
	Cvar_RegisterVariable(&r_watersurface_fresnel);
	Cvar_RegisterVariable(&r_watersurface_taper);
	Cvar_RegisterVariable(&r_watersurface_tint_red);
	Cvar_RegisterVariable(&r_watersurface_tint_green);
	Cvar_RegisterVariable(&r_watersurface_tint_blue);
	Cvar_RegisterVariable(&r_watersurface_clear);
	Cvar_RegisterVariable(&r_teleportswirl);
	Cvar_RegisterVariable(&r_teleportswirl_pivot);
	Cvar_RegisterVariable(&r_teleportswirl_churn);
	Cvar_RegisterVariable(&r_lavaflow);
	Cvar_RegisterVariable(&r_lavaflow_speed);
	Cvar_RegisterVariable(&r_lavashimmer);
	Cvar_RegisterVariable(&r_lavashimmer_height);
	Cvar_RegisterVariable(&r_lavashimmer_scale);
	Cvar_RegisterVariable(&r_lavashimmer_speed);
	Cvar_RegisterVariable(&r_lavashimmer_dist);
	Cvar_RegisterVariable(&r_lavashimmer_taps);
	Cvar_RegisterVariable(&r_lavashimmer_path);
	Cvar_RegisterVariable(&r_lavaglow);
	Cmd_AddCommand(CF_CLIENT, "r_volumetric_probe", R_Volumetric_Probe_f, "report what the volumetric murk sees: liquid, height above the local floor, corner term, resulting density, and a vertical profile up from that floor. Probes at the eye, or at \"x y z\" if given -- and since it prints the coordinates it is also this engine's stand-in for viewpos");
	Cvar_RegisterVariable(&r_transparentdepthmasking);
	Cvar_RegisterVariable(&r_transparent_sortmindist);
	Cvar_RegisterVariable(&r_transparent_sortmaxdist);
	Cvar_RegisterVariable(&r_transparent_sortarraysize);
	Cvar_RegisterVariable(&r_texture_dds_load);
	Cvar_RegisterVariable(&r_texture_dds_save);
	Cvar_RegisterVariable(&r_usedepthtextures);
	Cvar_RegisterVariable(&r_hdr_shoulder);
	Cvar_RegisterVariable(&r_viewfbo);
	Cvar_RegisterVariable(&r_rendertarget_debug);
	Cvar_RegisterVariable(&r_viewscale);
	Cvar_RegisterVariable(&r_metalfx);
	Cvar_RegisterVariable(&r_fxaa_post);
	Cvar_RegisterVariable(&r_fxaa_post_span);
	Cvar_RegisterVariable(&r_smaa);
	Cvar_RegisterVariable(&r_smaa_threshold);
	Cvar_RegisterVariable(&r_smaa_search);
	Cvar_RegisterVariable(&r_smaa_adapt);
	Cvar_RegisterVariable(&r_smaa_debug);
	Cvar_RegisterVariable(&r_metalfx_jitter);
	Cvar_RegisterVariable(&r_metalfx_jitterfix);
	Cvar_RegisterVariable(&r_metalfx_signs);
	Cvar_RegisterVariable(&r_metalfx_viewmodel);
	Cvar_RegisterVariable(&r_metalfx_entities);
	Cvar_RegisterVariable(&r_metalfx_reactive);
	Cvar_RegisterVariable(&r_metalfx_reactive_particles);
	Cvar_RegisterVariable(&r_metalfx_reactive_trail);
	Cvar_RegisterVariable(&r_metalfx_reactive_force);
	Cvar_RegisterVariable(&r_metalfx_reactive_debug);
	Cvar_RegisterVariable(&r_metalfx_debugview);
	Cvar_RegisterVariable(&r_viewscale_fpsscaling);
	Cvar_RegisterVariable(&r_viewscale_fpsscaling_min);
	Cvar_RegisterVariable(&r_viewscale_fpsscaling_multiply);
	Cvar_RegisterVariable(&r_viewscale_fpsscaling_stepsize);
	Cvar_RegisterVariable(&r_viewscale_fpsscaling_stepmax);
	Cvar_RegisterVariable(&r_viewscale_fpsscaling_target);
	Cvar_RegisterVariable(&r_glsl_deluxemapping);
	Cvar_RegisterVariable(&r_glsl_offsetmapping);
	Cvar_RegisterVariable(&r_glsl_offsetmapping_steps);
	Cvar_RegisterVariable(&r_glsl_offsetmapping_reliefmapping);
	Cvar_RegisterVariable(&r_glsl_offsetmapping_reliefmapping_steps);
	Cvar_RegisterVariable(&r_glsl_offsetmapping_reliefmapping_refinesteps);
	Cvar_RegisterVariable(&r_glsl_offsetmapping_scale);
	Cvar_RegisterVariable(&r_glsl_offsetmapping_lod);
	Cvar_RegisterVariable(&r_glsl_offsetmapping_lod_distance);
	Cvar_RegisterVariable(&r_glsl_postprocess);
	Cvar_RegisterVariable(&r_glsl_postprocess_uservec1);
	Cvar_RegisterVariable(&r_glsl_postprocess_uservec2);
	Cvar_RegisterVariable(&r_glsl_postprocess_uservec3);
	Cvar_RegisterVariable(&r_glsl_postprocess_uservec4);
	Cvar_RegisterVariable(&r_glsl_postprocess_uservec1_enable);
	Cvar_RegisterVariable(&r_glsl_postprocess_uservec2_enable);
	Cvar_RegisterVariable(&r_glsl_postprocess_uservec3_enable);
	Cvar_RegisterVariable(&r_glsl_postprocess_uservec4_enable);
	Cvar_RegisterVariable(&r_celshading);
	Cvar_RegisterVariable(&r_celoutlines);
	Cvar_RegisterVariable(&r_fxaa);

	Cvar_RegisterVariable(&r_water);
	Cvar_RegisterVariable(&r_water_cameraentitiesonly);
	Cvar_RegisterVariable(&r_water_resolutionmultiplier);
	Cvar_RegisterVariable(&r_water_clippingplanebias);
	Cvar_RegisterVariable(&r_water_refractdistort);
	Cvar_RegisterVariable(&r_water_reflectdistort);
	Cvar_RegisterVariable(&r_water_scissormode);
	Cvar_RegisterVariable(&r_water_lowquality);
	Cvar_RegisterVariable(&r_water_hideplayer);

	Cvar_RegisterVariable(&r_lerpsprites);
	Cvar_RegisterVariable(&r_lerpmodels);
	Cvar_RegisterVariable(&r_nolerp_list);
	Cvar_RegisterVariable(&r_lerplightstyles);
	Cvar_RegisterVariable(&r_waterscroll);
	Cvar_RegisterVariable(&r_bloom);
	Cvar_RegisterVariable(&r_colorfringe);
	Cvar_RegisterVariable(&r_bloom_colorscale);
	Cvar_RegisterVariable(&r_bloom_brighten);
	Cvar_RegisterVariable(&r_bloom_m5);
	Cvar_RegisterVariable(&r_bloom_m5_threshold);
	Cvar_RegisterVariable(&r_bloom_m5_knee);
	Cvar_RegisterVariable(&r_bloom_m5_intensity);
	Cvar_RegisterVariable(&r_bloom_m5_levels);
	Cvar_RegisterVariable(&r_bloom_blur);
	Cvar_RegisterVariable(&r_bloom_resolution);
	Cvar_RegisterVariable(&r_bloom_colorexponent);
	Cvar_RegisterVariable(&r_bloom_colorsubtract);
	Cvar_RegisterVariable(&r_bloom_scenebrightness);
	Cvar_RegisterVariable(&r_hdr_scenebrightness);
	Cvar_RegisterVariable(&r_hdr_glowintensity);
	Cvar_RegisterVariable(&r_hdr_irisadaptation);
	Cvar_RegisterVariable(&r_hdr_irisadaptation_multiplier);
	Cvar_RegisterVariable(&r_hdr_irisadaptation_minvalue);
	Cvar_RegisterVariable(&r_hdr_irisadaptation_maxvalue);
	Cvar_RegisterVariable(&r_hdr_irisadaptation_value);
	Cvar_RegisterVariable(&r_hdr_irisadaptation_fade_up);
	Cvar_RegisterVariable(&r_hdr_irisadaptation_fade_down);
	Cvar_RegisterVariable(&r_hdr_irisadaptation_radius);
	Cvar_RegisterVariable(&r_smoothnormals_areaweighting);
	Cvar_RegisterVariable(&developer_texturelogging);
	Cvar_RegisterVariable(&gl_lightmaps);
	Cvar_RegisterVariable(&r_test);
	Cvar_RegisterVariable(&r_batch_multidraw);
	Cvar_RegisterVariable(&r_batch_multidraw_mintriangles);
	Cvar_RegisterVariable(&r_batch_debugdynamicvertexpath);
	Cvar_RegisterVariable(&r_glsl_skeletal);
	Cvar_RegisterVariable(&r_glsl_saturation);
	Cvar_RegisterVariable(&r_glsl_saturation_redcompensate);
	Cvar_RegisterVariable(&r_glsl_vertextextureblend_usebothalphas);
	Cvar_RegisterVariable(&r_framedatasize);
	for (i = 0;i < R_BUFFERDATA_COUNT;i++)
		Cvar_RegisterVariable(&r_buffermegs[i]);
	Cvar_RegisterVariable(&r_batch_dynamicbuffer);
	Cvar_RegisterVariable(&r_q1bsp_lightmap_updates_enabled);
	Cvar_RegisterVariable(&r_q1bsp_lightmap_updates_combine);
	Cvar_RegisterVariable(&r_q1bsp_lightmap_updates_hidden_surfaces);
	if (gamemode == GAME_NEHAHRA || gamemode == GAME_TENEBRAE)
		Cvar_SetQuick(&r_fullbrights, "0");
#ifdef DP_MOBILETOUCH
	// GLES devices have terrible depth precision in general, so...
	Cvar_SetValueQuick(&r_nearclip, 4);
	Cvar_SetValueQuick(&r_farclip_base, 4096);
	Cvar_SetValueQuick(&r_farclip_world, 0);
	Cvar_SetValueQuick(&r_useinfinitefarclip, 0);
#endif
	R_RegisterModule("GL_Main", gl_main_start, gl_main_shutdown, gl_main_newmap, NULL, NULL);
}

void Render_Init(void)
{
	gl_backend_init();
	R_Textures_Init();
	GL_Main_Init();
	Font_Init();
	GL_Draw_Init();
	R_Shadow_Init();
	R_Sky_Init();
	GL_Surf_Init();
	Sbar_Init();
	R_Particles_Init();
	R_Explosion_Init();
	R_LightningBeams_Init();
	CL_MeshEntities_Init();
	Mod_RenderInit();
}

static void R_GetCornerOfBox(vec3_t out, const vec3_t mins, const vec3_t maxs, int signbits)
{
	out[0] = ((signbits & 1) ? mins : maxs)[0];
	out[1] = ((signbits & 2) ? mins : maxs)[1];
	out[2] = ((signbits & 4) ? mins : maxs)[2];
}

static qbool _R_CullBox(const vec3_t mins, const vec3_t maxs, int numplanes, const mplane_t *planes, int ignore)
{
	int i;
	const mplane_t *p;
	vec3_t corner;
	if (r_trippy.integer)
		return false;
	for (i = 0;i < numplanes;i++)
	{
		if(i == ignore)
			continue;
		p = planes + i;
		R_GetCornerOfBox(corner, mins, maxs, p->signbits);
		if (DotProduct(p->normal, corner) < p->dist)
			return true;
	}
	return false;
}

qbool R_CullFrustum(const vec3_t mins, const vec3_t maxs)
{
	// skip nearclip plane, it often culls portals when you are very close, and is almost never useful
	return _R_CullBox(mins, maxs, r_refdef.view.numfrustumplanes, r_refdef.view.frustum, 4);
}

qbool R_CullBox(const vec3_t mins, const vec3_t maxs, int numplanes, const mplane_t *planes)
{
	// nothing to ignore
	return _R_CullBox(mins, maxs, numplanes, planes, -1);
}

//==================================================================================

// LadyHavoc: this stores temporary data used within the same frame

typedef struct r_framedata_mem_s
{
	struct r_framedata_mem_s *purge; // older mem block to free on next frame
	size_t size; // how much usable space
	size_t current; // how much space in use
	size_t mark; // last "mark" location, temporary memory can be freed by returning to this
	size_t wantedsize; // how much space was allocated
	unsigned char *data; // start of real data (16byte aligned)
}
r_framedata_mem_t;

static r_framedata_mem_t *r_framedata_mem;

void R_FrameData_Reset(void)
{
	while (r_framedata_mem)
	{
		r_framedata_mem_t *next = r_framedata_mem->purge;
		Mem_Free(r_framedata_mem);
		r_framedata_mem = next;
	}
}

static void R_FrameData_Resize(qbool mustgrow)
{
	size_t wantedsize;
	wantedsize = (size_t)(r_framedatasize.value * 1024*1024);
	wantedsize = bound(65536, wantedsize, 1000*1024*1024);
	if (!r_framedata_mem || r_framedata_mem->wantedsize != wantedsize || mustgrow)
	{
		r_framedata_mem_t *newmem = (r_framedata_mem_t *)Mem_Alloc(r_main_mempool, wantedsize);
		newmem->wantedsize = wantedsize;
		newmem->data = (unsigned char *)(((size_t)(newmem+1) + 15) & ~15);
		newmem->size = (unsigned char *)newmem + wantedsize - newmem->data;
		newmem->current = 0;
		newmem->mark = 0;
		newmem->purge = r_framedata_mem;
		r_framedata_mem = newmem;
	}
}

void R_FrameData_NewFrame(void)
{
	R_FrameData_Resize(false);
	if (!r_framedata_mem)
		return;
	// if we ran out of space on the last frame, free the old memory now
	while (r_framedata_mem->purge)
	{
		// repeatedly remove the second item in the list, leaving only head
		r_framedata_mem_t *next = r_framedata_mem->purge->purge;
		Mem_Free(r_framedata_mem->purge);
		r_framedata_mem->purge = next;
	}
	// reset the current mem pointer
	r_framedata_mem->current = 0;
	r_framedata_mem->mark = 0;
}

void *R_FrameData_Alloc(size_t size)
{
	void *data;
	float newvalue;

	// align to 16 byte boundary - the data pointer is already aligned, so we
	// only need to ensure the size of every allocation is also aligned
	size = (size + 15) & ~15;

	while (!r_framedata_mem || r_framedata_mem->current + size > r_framedata_mem->size)
	{
		// emergency - we ran out of space, allocate more memory
		// note: this has no upper-bound, we'll fail to allocate memory eventually and just die
		newvalue = r_framedatasize.value * 2.0f;
		// upper bound based on architecture - if we try to allocate more than this we could overflow, better to loop until we error out on allocation failure
		if (sizeof(size_t) >= 8)
			newvalue = bound(0.25f, newvalue, (float)(1ll << 42));
		else
			newvalue = bound(0.25f, newvalue, (float)(1 << 10));
		// this might not be a growing it, but we'll allocate another buffer every time
		Cvar_SetValueQuick(&r_framedatasize, newvalue);
		R_FrameData_Resize(true);
	}

	data = r_framedata_mem->data + r_framedata_mem->current;
	r_framedata_mem->current += size;

	// count the usage for stats
	r_refdef.stats[r_stat_framedatacurrent] = max(r_refdef.stats[r_stat_framedatacurrent], (int)r_framedata_mem->current);
	r_refdef.stats[r_stat_framedatasize] = max(r_refdef.stats[r_stat_framedatasize], (int)r_framedata_mem->size);

	return (void *)data;
}

void *R_FrameData_Store(size_t size, void *data)
{
	void *d = R_FrameData_Alloc(size);
	if (d && data)
		memcpy(d, data, size);
	return d;
}

void R_FrameData_SetMark(void)
{
	if (!r_framedata_mem)
		return;
	r_framedata_mem->mark = r_framedata_mem->current;
}

void R_FrameData_ReturnToMark(void)
{
	if (!r_framedata_mem)
		return;
	r_framedata_mem->current = r_framedata_mem->mark;
}

//==================================================================================

// avoid reusing the same buffer objects on consecutive frames
#define R_BUFFERDATA_CYCLE 3

typedef struct r_bufferdata_buffer_s
{
	struct r_bufferdata_buffer_s *purge; // older buffer to free on next frame
	size_t size; // how much usable space
	size_t current; // how much space in use
	r_meshbuffer_t *buffer; // the buffer itself
}
r_bufferdata_buffer_t;

static int r_bufferdata_cycle = 0; // incremented and wrapped each frame
static r_bufferdata_buffer_t *r_bufferdata_buffer[R_BUFFERDATA_CYCLE][R_BUFFERDATA_COUNT];

/// frees all dynamic buffers
void R_BufferData_Reset(void)
{
	int cycle, type;
	r_bufferdata_buffer_t **p, *mem;
	for (cycle = 0;cycle < R_BUFFERDATA_CYCLE;cycle++)
	{
		for (type = 0;type < R_BUFFERDATA_COUNT;type++)
		{
			// free all buffers
			p = &r_bufferdata_buffer[cycle][type];
			while (*p)
			{
				mem = *p;
				*p = (*p)->purge;
				if (mem->buffer)
					R_Mesh_DestroyMeshBuffer(mem->buffer);
				Mem_Free(mem);
			}
		}
	}
}

// resize buffer as needed (this actually makes a new one, the old one will be recycled next frame)
static void R_BufferData_Resize(r_bufferdata_type_t type, qbool mustgrow, size_t minsize)
{
	r_bufferdata_buffer_t *mem = r_bufferdata_buffer[r_bufferdata_cycle][type];
	size_t size;
	float newvalue = r_buffermegs[type].value;

	// increase the cvar if we have to (but only if we already have a mem)
	if (mustgrow && mem)
		newvalue *= 2.0f;
	newvalue = bound(0.25f, newvalue, 256.0f);
	while (newvalue * 1024*1024 < minsize)
		newvalue *= 2.0f;

	// clamp the cvar to valid range
	newvalue = bound(0.25f, newvalue, 256.0f);
	if (r_buffermegs[type].value != newvalue)
		Cvar_SetValueQuick(&r_buffermegs[type], newvalue);

	// calculate size in bytes
	size = (size_t)(newvalue * 1024*1024);
	size = bound(131072, size, 256*1024*1024);

	// allocate a new buffer if the size is different (purge old one later)
	// or if we were told we must grow the buffer
	if (!mem || mem->size != size || mustgrow)
	{
		mem = (r_bufferdata_buffer_t *)Mem_Alloc(r_main_mempool, sizeof(*mem));
		mem->size = size;
		mem->current = 0;
		if (type == R_BUFFERDATA_VERTEX)
			mem->buffer = R_Mesh_CreateMeshBuffer(NULL, mem->size, "dynamicbuffervertex", false, false, true, false);
		else if (type == R_BUFFERDATA_INDEX16)
			mem->buffer = R_Mesh_CreateMeshBuffer(NULL, mem->size, "dynamicbufferindex16", true, false, true, true);
		else if (type == R_BUFFERDATA_INDEX32)
			mem->buffer = R_Mesh_CreateMeshBuffer(NULL, mem->size, "dynamicbufferindex32", true, false, true, false);
		else if (type == R_BUFFERDATA_UNIFORM)
			mem->buffer = R_Mesh_CreateMeshBuffer(NULL, mem->size, "dynamicbufferuniform", false, true, true, false);
		mem->purge = r_bufferdata_buffer[r_bufferdata_cycle][type];
		r_bufferdata_buffer[r_bufferdata_cycle][type] = mem;
	}
}

void R_BufferData_NewFrame(void)
{
	int type;
	r_bufferdata_buffer_t **p, *mem;
	// cycle to the next frame's buffers
	r_bufferdata_cycle = (r_bufferdata_cycle + 1) % R_BUFFERDATA_CYCLE;
	// if we ran out of space on the last time we used these buffers, free the old memory now
	for (type = 0;type < R_BUFFERDATA_COUNT;type++)
	{
		if (r_bufferdata_buffer[r_bufferdata_cycle][type])
		{
			R_BufferData_Resize((r_bufferdata_type_t)type, false, 131072);
			// free all but the head buffer, this is how we recycle obsolete
			// buffers after they are no longer in use
			p = &r_bufferdata_buffer[r_bufferdata_cycle][type]->purge;
			while (*p)
			{
				mem = *p;
				*p = (*p)->purge;
				if (mem->buffer)
					R_Mesh_DestroyMeshBuffer(mem->buffer);
				Mem_Free(mem);
			}
			// reset the current offset
			r_bufferdata_buffer[r_bufferdata_cycle][type]->current = 0;
		}
	}
}

r_meshbuffer_t *R_BufferData_Store(size_t datasize, const void *data, r_bufferdata_type_t type, int *returnbufferoffset)
{
	r_bufferdata_buffer_t *mem;
	int offset = 0;
	int padsize;

	*returnbufferoffset = 0;

	// align size to a byte boundary appropriate for the buffer type, this
	// makes all allocations have aligned start offsets
	if (type == R_BUFFERDATA_UNIFORM)
		padsize = (datasize + r_uniformbufferalignment - 1) & ~(r_uniformbufferalignment - 1);
	else
		padsize = (datasize + 15) & ~15;

	// if we ran out of space in this buffer we must allocate a new one
	if (!r_bufferdata_buffer[r_bufferdata_cycle][type] || r_bufferdata_buffer[r_bufferdata_cycle][type]->current + padsize > r_bufferdata_buffer[r_bufferdata_cycle][type]->size)
		R_BufferData_Resize(type, true, padsize);

	// if the resize did not give us enough memory, fail
	if (!r_bufferdata_buffer[r_bufferdata_cycle][type] || r_bufferdata_buffer[r_bufferdata_cycle][type]->current + padsize > r_bufferdata_buffer[r_bufferdata_cycle][type]->size)
		Sys_Error("R_BufferData_Store: failed to create a new buffer of sufficient size\n");

	mem = r_bufferdata_buffer[r_bufferdata_cycle][type];
	offset = (int)mem->current;
	mem->current += padsize;

	// upload the data to the buffer at the chosen offset
	if (offset == 0)
		R_Mesh_UpdateMeshBuffer(mem->buffer, NULL, mem->size, false, 0);
	R_Mesh_UpdateMeshBuffer(mem->buffer, data, datasize, true, offset);

	// count the usage for stats
	r_refdef.stats[r_stat_bufferdatacurrent_vertex + type] = max(r_refdef.stats[r_stat_bufferdatacurrent_vertex + type], (int)mem->current);
	r_refdef.stats[r_stat_bufferdatasize_vertex + type] = max(r_refdef.stats[r_stat_bufferdatasize_vertex + type], (int)mem->size);

	// return the buffer offset
	*returnbufferoffset = offset;

	return mem->buffer;
}

//==================================================================================

// LadyHavoc: animcache originally written by Echon, rewritten since then

/**
 * Animation cache prevents re-generating mesh data for an animated model
 * multiple times in one frame for lighting, shadowing, reflections, etc.
 */

void R_AnimCache_Free(void)
{
}

void R_AnimCache_ClearCache(void)
{
	int i;
	entity_render_t *ent;

	for (i = 0;i < r_refdef.scene.numentities;i++)
	{
		ent = r_refdef.scene.entities[i];
		ent->animcache_vertex3f = NULL;
		ent->animcache_vertex3f_vertexbuffer = NULL;
		ent->animcache_vertex3f_bufferoffset = 0;
		ent->animcache_normal3f = NULL;
		ent->animcache_normal3f_vertexbuffer = NULL;
		ent->animcache_normal3f_bufferoffset = 0;
		ent->animcache_svector3f = NULL;
		ent->animcache_svector3f_vertexbuffer = NULL;
		ent->animcache_svector3f_bufferoffset = 0;
		ent->animcache_tvector3f = NULL;
		ent->animcache_tvector3f_vertexbuffer = NULL;
		ent->animcache_tvector3f_bufferoffset = 0;
		ent->animcache_skeletaltransform3x4 = NULL;
		ent->animcache_skeletaltransform3x4buffer = NULL;
		ent->animcache_skeletaltransform3x4offset = 0;
		ent->animcache_skeletaltransform3x4size = 0;
	}
}

qbool R_AnimCache_GetEntity(entity_render_t *ent, qbool wantnormals, qbool wanttangents)
{
	model_t *model = ent->model;
	int numvertices;

	// see if this ent is worth caching
	if (!model || !model->Draw || !model->AnimateVertices)
		return false;
	// nothing to cache if it contains no animations and has no skeleton
	if (!model->surfmesh.isanimated && !(model->num_bones && ent->skeleton && ent->skeleton->relativetransforms))
		return false;
	// see if it is already cached for gpuskeletal
	if (ent->animcache_skeletaltransform3x4)
		return false;
	// see if it is already cached as a mesh
	if (ent->animcache_vertex3f)
	{
		// check if we need to add normals or tangents
		if (ent->animcache_normal3f)
			wantnormals = false;
		if (ent->animcache_svector3f)
			wanttangents = false;
		if (!wantnormals && !wanttangents)
			return false;
	}

	// check which kind of cache we need to generate
	if (r_gpuskeletal && model->num_bones > 0 && model->surfmesh.data_skeletalindex4ub)
	{
		// cache the skeleton so the vertex shader can use it
		r_refdef.stats[r_stat_animcache_skeletal_count] += 1;
		r_refdef.stats[r_stat_animcache_skeletal_bones] += model->num_bones;
		r_refdef.stats[r_stat_animcache_skeletal_maxbones] = max(r_refdef.stats[r_stat_animcache_skeletal_maxbones], model->num_bones);
		ent->animcache_skeletaltransform3x4 = (float *)R_FrameData_Alloc(sizeof(float[3][4]) * model->num_bones);
		Mod_Skeletal_BuildTransforms(model, ent->frameblend, ent->skeleton, NULL, ent->animcache_skeletaltransform3x4);
		// note: this can fail if the buffer is at the grow limit
		ent->animcache_skeletaltransform3x4size = sizeof(float[3][4]) * model->num_bones;
		ent->animcache_skeletaltransform3x4buffer = R_BufferData_Store(ent->animcache_skeletaltransform3x4size, ent->animcache_skeletaltransform3x4, R_BUFFERDATA_UNIFORM, &ent->animcache_skeletaltransform3x4offset);
	}
	else if (ent->animcache_vertex3f)
	{
		// mesh was already cached but we may need to add normals/tangents
		// (this only happens with multiple views, reflections, cameras, etc)
		if (wantnormals || wanttangents)
		{
			numvertices = model->surfmesh.num_vertices;
			if (wantnormals)
				ent->animcache_normal3f = (float *)R_FrameData_Alloc(sizeof(float[3])*numvertices);
			if (wanttangents)
			{
				ent->animcache_svector3f = (float *)R_FrameData_Alloc(sizeof(float[3])*numvertices);
				ent->animcache_tvector3f = (float *)R_FrameData_Alloc(sizeof(float[3])*numvertices);
			}
			model->AnimateVertices(model, ent->frameblend, ent->skeleton, NULL, wantnormals ? ent->animcache_normal3f : NULL, wanttangents ? ent->animcache_svector3f : NULL, wanttangents ? ent->animcache_tvector3f : NULL);
			r_refdef.stats[r_stat_animcache_shade_count] += 1;
			r_refdef.stats[r_stat_animcache_shade_vertices] += numvertices;
			r_refdef.stats[r_stat_animcache_shade_maxvertices] = max(r_refdef.stats[r_stat_animcache_shade_maxvertices], numvertices);
		}
	}
	else
	{
		// generate mesh cache
		numvertices = model->surfmesh.num_vertices;
		ent->animcache_vertex3f = (float *)R_FrameData_Alloc(sizeof(float[3])*numvertices);
		if (wantnormals)
			ent->animcache_normal3f = (float *)R_FrameData_Alloc(sizeof(float[3])*numvertices);
		if (wanttangents)
		{
			ent->animcache_svector3f = (float *)R_FrameData_Alloc(sizeof(float[3])*numvertices);
			ent->animcache_tvector3f = (float *)R_FrameData_Alloc(sizeof(float[3])*numvertices);
		}
		model->AnimateVertices(model, ent->frameblend, ent->skeleton, ent->animcache_vertex3f, ent->animcache_normal3f, ent->animcache_svector3f, ent->animcache_tvector3f);
		if (wantnormals || wanttangents)
		{
			r_refdef.stats[r_stat_animcache_shade_count] += 1;
			r_refdef.stats[r_stat_animcache_shade_vertices] += numvertices;
			r_refdef.stats[r_stat_animcache_shade_maxvertices] = max(r_refdef.stats[r_stat_animcache_shade_maxvertices], numvertices);
		}
		r_refdef.stats[r_stat_animcache_shape_count] += 1;
		r_refdef.stats[r_stat_animcache_shape_vertices] += numvertices;
		r_refdef.stats[r_stat_animcache_shape_maxvertices] = max(r_refdef.stats[r_stat_animcache_shape_maxvertices], numvertices);
	}
	return true;
}

void R_AnimCache_CacheVisibleEntities(void)
{
	int i;

	// TODO: thread this
	// NOTE: R_PrepareRTLights() also caches entities

	for (i = 0;i < r_refdef.scene.numentities;i++)
		if (r_refdef.viewcache.entityvisible[i])
			R_AnimCache_GetEntity(r_refdef.scene.entities[i], true, true);
}

//==================================================================================

qbool R_CanSeeBox(int numsamples, vec_t eyejitter, vec_t entboxenlarge, vec_t entboxexpand, vec_t pad, vec3_t eye, vec3_t entboxmins, vec3_t entboxmaxs)
{
	long unsigned int i;
	int j;
	vec3_t eyemins, eyemaxs;
	vec3_t boxmins, boxmaxs;
	vec3_t padmins, padmaxs;
	vec3_t start;
	vec3_t end;
	model_t *model = r_refdef.scene.worldmodel;
	static vec3_t positions[] = {
		{ 0.5f, 0.5f, 0.5f },
		{ 0.0f, 0.0f, 0.0f },
		{ 0.0f, 0.0f, 1.0f },
		{ 0.0f, 1.0f, 0.0f },
		{ 0.0f, 1.0f, 1.0f },
		{ 1.0f, 0.0f, 0.0f },
		{ 1.0f, 0.0f, 1.0f },
		{ 1.0f, 1.0f, 0.0f },
		{ 1.0f, 1.0f, 1.0f },
	};

	// sample count can be set to -1 to skip this logic, for flicker-prone objects
	if (numsamples < 0)
		return true;

	// view origin is not used for culling in portal/reflection/refraction renders or isometric views
	if (!r_refdef.view.usevieworiginculling)
		return true;

	if (!r_cullentities_trace_entityocclusion.integer && (!model || !model->brush.TraceLineOfSight))
		return true;

	// expand the eye box a little
	eyemins[0] = eye[0] - eyejitter;
	eyemaxs[0] = eye[0] + eyejitter;
	eyemins[1] = eye[1] - eyejitter;
	eyemaxs[1] = eye[1] + eyejitter;
	eyemins[2] = eye[2] - eyejitter;
	eyemaxs[2] = eye[2] + eyejitter;
	// expand the box a little
	boxmins[0] = (entboxenlarge + 1) * entboxmins[0] - entboxenlarge * entboxmaxs[0] - entboxexpand;
	boxmaxs[0] = (entboxenlarge + 1) * entboxmaxs[0] - entboxenlarge * entboxmins[0] + entboxexpand;
	boxmins[1] = (entboxenlarge + 1) * entboxmins[1] - entboxenlarge * entboxmaxs[1] - entboxexpand;
	boxmaxs[1] = (entboxenlarge + 1) * entboxmaxs[1] - entboxenlarge * entboxmins[1] + entboxexpand;
	boxmins[2] = (entboxenlarge + 1) * entboxmins[2] - entboxenlarge * entboxmaxs[2] - entboxexpand;
	boxmaxs[2] = (entboxenlarge + 1) * entboxmaxs[2] - entboxenlarge * entboxmins[2] + entboxexpand;
	// make an even larger box for the acceptable area
	padmins[0] = boxmins[0] - pad;
	padmaxs[0] = boxmaxs[0] + pad;
	padmins[1] = boxmins[1] - pad;
	padmaxs[1] = boxmaxs[1] + pad;
	padmins[2] = boxmins[2] - pad;
	padmaxs[2] = boxmaxs[2] + pad;

	// return true if eye overlaps enlarged box
	if (BoxesOverlap(boxmins, boxmaxs, eyemins, eyemaxs))
		return true;

	VectorCopy(eye, start);
	// try specific positions in the box first - note that these can be cached
	if (r_cullentities_trace_entityocclusion.integer)
	{
		for (i = 0; i < sizeof(positions) / sizeof(positions[0]); i++)
		{
			trace_t trace;
			end[0] = boxmins[0] + (boxmaxs[0] - boxmins[0]) * positions[i][0];
			end[1] = boxmins[1] + (boxmaxs[1] - boxmins[1]) * positions[i][1];
			end[2] = boxmins[2] + (boxmaxs[2] - boxmins[2]) * positions[i][2];
			//trace_t trace = CL_TraceLine(start, end, MOVE_NORMAL, NULL, SUPERCONTENTS_SOLID, SUPERCONTENTS_SKY, MATERIALFLAGMASK_TRANSLUCENT, 0.0f, true, false, NULL, true, true);
			trace = CL_Cache_TraceLineSurfaces(start, end, MOVE_NORMAL, SUPERCONTENTS_SOLID, 0, MATERIALFLAGMASK_TRANSLUCENT);
			// not picky - if the trace ended anywhere in the box we're good
			if (BoxesOverlap(trace.endpos, trace.endpos, padmins, padmaxs))
				return true;
		}
	}
	else
	{
		// try center
		VectorMAM(0.5f, boxmins, 0.5f, boxmaxs, end);
		if (model->brush.TraceLineOfSight(model, start, end, padmins, padmaxs))
			return true;
	}

	// try various random positions
	for (j = 0; j < numsamples; j++)
	{
		VectorSet(start, lhrandom(eyemins[0], eyemaxs[0]), lhrandom(eyemins[1], eyemaxs[1]), lhrandom(eyemins[2], eyemaxs[2]));
		VectorSet(end, lhrandom(boxmins[0], boxmaxs[0]), lhrandom(boxmins[1], boxmaxs[1]), lhrandom(boxmins[2], boxmaxs[2]));
		if (r_cullentities_trace_entityocclusion.integer)
		{
			trace_t trace = CL_TraceLine(start, end, MOVE_NORMAL, NULL, SUPERCONTENTS_SOLID, SUPERCONTENTS_SKY, MATERIALFLAGMASK_TRANSLUCENT, 0.0f, true, false, NULL, true, true);
			// not picky - if the trace ended anywhere in the box we're good
			if (BoxesOverlap(trace.endpos, trace.endpos, padmins, padmaxs))
				return true;
		}
		else if (model->brush.TraceLineOfSight(model, start, end, padmins, padmaxs))
			return true;
	}

	return false;
}


static void R_View_UpdateEntityVisible (void)
{
	int i;
	int renderimask;
	int samples;
	entity_render_t *ent;

	if (r_refdef.envmap || r_fb.water.hideplayer)
		renderimask = RENDER_EXTERIORMODEL | RENDER_VIEWMODEL;
	else if (chase_active.integer || r_fb.water.renderingscene)
		renderimask = RENDER_VIEWMODEL;
	else
		renderimask = RENDER_EXTERIORMODEL;
	if (!r_drawviewmodel.integer)
		renderimask |= RENDER_VIEWMODEL;
	if (!r_drawexteriormodel.integer)
		renderimask |= RENDER_EXTERIORMODEL;
	memset(r_refdef.viewcache.entityvisible, 0, r_refdef.scene.numentities);
	if (r_refdef.scene.worldmodel && !r_novis.integer && r_refdef.scene.worldmodel->brush.BoxTouchingVisibleLeafs)
	{
		// worldmodel can check visibility
		for (i = 0;i < r_refdef.scene.numentities;i++)
		{
			ent = r_refdef.scene.entities[i];
			if (r_refdef.viewcache.world_novis && !(ent->flags & RENDER_VIEWMODEL))
			{
				r_refdef.viewcache.entityvisible[i] = false;
				continue;
			}
			if (!(ent->flags & renderimask))
			if (!R_CullFrustum(ent->mins, ent->maxs) || (ent->model && ent->model->type == mod_sprite && (ent->model->sprite.sprnum_type == SPR_LABEL || ent->model->sprite.sprnum_type == SPR_LABEL_SCALE)))
			if ((ent->flags & (RENDER_NODEPTHTEST | RENDER_WORLDOBJECT | RENDER_VIEWMODEL)) || r_refdef.scene.worldmodel->brush.BoxTouchingVisibleLeafs(r_refdef.scene.worldmodel, r_refdef.viewcache.world_leafvisible, ent->mins, ent->maxs))
				r_refdef.viewcache.entityvisible[i] = true;
		}
	}
	else
	{
		// no worldmodel or it can't check visibility
		for (i = 0;i < r_refdef.scene.numentities;i++)
		{
			ent = r_refdef.scene.entities[i];
			if (!(ent->flags & renderimask))
			if (!R_CullFrustum(ent->mins, ent->maxs) || (ent->model && ent->model->type == mod_sprite && (ent->model->sprite.sprnum_type == SPR_LABEL || ent->model->sprite.sprnum_type == SPR_LABEL_SCALE)))
				r_refdef.viewcache.entityvisible[i] = true;
		}
	}
	if (r_cullentities_trace.integer)
	{
		for (i = 0;i < r_refdef.scene.numentities;i++)
		{
			if (!r_refdef.viewcache.entityvisible[i])
				continue;
			ent = r_refdef.scene.entities[i];
			if (!(ent->flags & (RENDER_VIEWMODEL | RENDER_WORLDOBJECT | RENDER_NODEPTHTEST)) && !(ent->model && (ent->model->name[0] == '*')))
			{
				samples = ent->last_trace_visibility == 0 ? r_cullentities_trace_tempentitysamples.integer : r_cullentities_trace_samples.integer;
				if (R_CanSeeBox(samples, r_cullentities_trace_eyejitter.value, r_cullentities_trace_enlarge.value, r_cullentities_trace_expand.value, r_cullentities_trace_pad.value, r_refdef.view.origin, ent->mins, ent->maxs))
					ent->last_trace_visibility = host.realtime;
				if (ent->last_trace_visibility < host.realtime - r_cullentities_trace_delay.value)
					r_refdef.viewcache.entityvisible[i] = 0;
			}
		}
	}
}

/// only used if skyrendermasked, and normally returns false
static int R_DrawBrushModelsSky (void)
{
	int i, sky;
	entity_render_t *ent;

	sky = false;
	for (i = 0;i < r_refdef.scene.numentities;i++)
	{
		if (!r_refdef.viewcache.entityvisible[i])
			continue;
		ent = r_refdef.scene.entities[i];
		if (!ent->model || !ent->model->DrawSky)
			continue;
		ent->model->DrawSky(ent);
		sky = true;
	}
	return sky;
}

static void R_DrawNoModel(entity_render_t *ent);
static void R_DrawModels(void)
{
	int i;
	entity_render_t *ent;

	for (i = 0;i < r_refdef.scene.numentities;i++)
	{
		if (!r_refdef.viewcache.entityvisible[i])
			continue;
		ent = r_refdef.scene.entities[i];
		r_refdef.stats[r_stat_entities]++;

		if (ent->model && ent->model->Draw != NULL)
			ent->model->Draw(ent);
		else
			R_DrawNoModel(ent);
	}
}

#ifdef USE_RT_METAL
// EMISSION SURVIVES WALL LIGHTING (rt_metal_glowpass). Under rt_metal_walllight
// the RT composite multiplies the whole frame by the RT term -- the scene's
// entire lighting -- and authored EMISSION must not receive lighting: a lamp
// face whose dominant light is occluded otherwise renders extinguished (e1m1's
// bollards), and one whose term is high renders over-bright. Per the ordering
// rule (anything that ADDS light goes after the RT multiply), on frames where
// the composite will multiply, R_SetupShader_Surface withholds Color_Glow and
// RedGlow from the pre-composite passes and this pass re-draws the emissive
// batches ADDITIVELY straight after the multiply: final = albedo * term + glow,
// the authored emission exactly. It runs BEFORE the murk, so fog still swallows
// emission like any other surface light (and classic fog darkens it through the
// additive-pass FOG_HACK0 treatment). The batch overrides all live in
// R_SetupShader_Surface / R_QueueModelSurfaceList behind r_rtglowpass_active,
// so this works identically on both renderpaths -- every override is a
// name-fed uniform. The view weapon escapes the composite via its depth mask
// and keeps its in-pass glow, so it is excluded here (it would double).
static void R_RTGlow_DrawEntity(entity_render_t *ent)
{
	if (!ent->model || !ent->model->Draw)
		return;
	if (ent->flags & RENDER_VIEWMODEL)
		return;
	// Sprites NEVER: R_Model_Sprite_Draw enqueues the transparent queue
	// directly, bypassing the DEPTHSORTED guard below, so re-drawing a sprite
	// entity here would queue it TWICE and double its brightness. They are
	// self-lit billboards drawn after the composite anyway -- no emission to
	// re-add. (Found by the four-lens review; two finders, same mechanism.)
	if (ent->model->type == mod_sprite)
		return;
	// r_redglow emits from any red-enough texel, so with it live every opaque
	// batch is a candidate; without it only glow-layer batches draw. Gibs are
	// excluded from redglow exactly as the scene pass excludes them.
	r_rtglowpass_redglow = r_redglow.value > 0.0f && !m5_stock.integer && !R_IsGibModel(ent->model);
	ent->model->Draw(ent);
}

static void R_RTGlow_Pass(void)
{
	int i;
	r_rtglowpass_active = true;
	if (cl.csqc_vidvars.drawworld && r_refdef.scene.worldmodel && r_refdef.scene.worldmodel->Draw)
		R_RTGlow_DrawEntity(r_refdef.scene.worldentity);
	for (i = 0; i < r_refdef.scene.numentities; i++)
	{
		if (!r_refdef.viewcache.entityvisible[i])
			continue;
		R_RTGlow_DrawEntity(r_refdef.scene.entities[i]);
	}
	r_rtglowpass_active = false;
}
#endif

static void R_DrawModelsDepth(void)
{
	int i;
	entity_render_t *ent;

	for (i = 0;i < r_refdef.scene.numentities;i++)
	{
		if (!r_refdef.viewcache.entityvisible[i])
			continue;
		ent = r_refdef.scene.entities[i];
		if (ent->model && ent->model->DrawDepth != NULL)
			ent->model->DrawDepth(ent);
	}
}

static void R_DrawModelsDebug(void)
{
	int i;
	entity_render_t *ent;

	for (i = 0;i < r_refdef.scene.numentities;i++)
	{
		if (!r_refdef.viewcache.entityvisible[i])
			continue;
		ent = r_refdef.scene.entities[i];
		if (ent->model && ent->model->DrawDebug != NULL)
			ent->model->DrawDebug(ent);
	}
}

static void R_DrawModelsAddWaterPlanes(void)
{
	int i;
	entity_render_t *ent;

	for (i = 0;i < r_refdef.scene.numentities;i++)
	{
		if (!r_refdef.viewcache.entityvisible[i])
			continue;
		ent = r_refdef.scene.entities[i];
		if (ent->model && ent->model->DrawAddWaterPlanes != NULL)
			ent->model->DrawAddWaterPlanes(ent);
	}
}

static float irisvecs[7][3] = {{0, 0, 0}, {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};

void R_HDR_UpdateIrisAdaptation(const vec3_t point)
{
	if (r_hdr_irisadaptation.integer)
	{
		vec3_t p;
		vec3_t ambient;
		vec3_t diffuse;
		vec3_t diffusenormal;
		vec3_t forward;
		vec_t brightness = 0.0f;
		vec_t goal;
		vec_t current;
		vec_t d;
		int c;
		VectorCopy(r_refdef.view.forward, forward);
		for (c = 0;c < (int)(sizeof(irisvecs)/sizeof(irisvecs[0]));c++)
		{
			p[0] = point[0] + irisvecs[c][0] * r_hdr_irisadaptation_radius.value;
			p[1] = point[1] + irisvecs[c][1] * r_hdr_irisadaptation_radius.value;
			p[2] = point[2] + irisvecs[c][2] * r_hdr_irisadaptation_radius.value;
			R_CompleteLightPoint(ambient, diffuse, diffusenormal, p, LP_LIGHTMAP | LP_RTWORLD | LP_DYNLIGHT, r_refdef.scene.lightmapintensity, r_refdef.scene.ambientintensity);
			d = DotProduct(forward, diffusenormal);
			brightness += VectorLength(ambient);
			if (d > 0)
				brightness += d * VectorLength(diffuse);
		}
		brightness *= 1.0f / c;
		brightness += 0.00001f; // make sure it's never zero
		goal = r_hdr_irisadaptation_multiplier.value / brightness;
		goal = bound(r_hdr_irisadaptation_minvalue.value, goal, r_hdr_irisadaptation_maxvalue.value);
		current = r_hdr_irisadaptation_value.value;
		if (current < goal)
			current = min(current + r_hdr_irisadaptation_fade_up.value * cl.realframetime, goal);
		else if (current > goal)
			current = max(current - r_hdr_irisadaptation_fade_down.value * cl.realframetime, goal);
		if (fabs(r_hdr_irisadaptation_value.value - current) > 0.0001f)
			Cvar_SetValueQuick(&r_hdr_irisadaptation_value, current);
	}
	else if (r_hdr_irisadaptation_value.value != 1.0f)
		Cvar_SetValueQuick(&r_hdr_irisadaptation_value, 1.0f);
}

extern cvar_t r_lockvisibility;
extern cvar_t r_lockpvs;

static void R_View_SetFrustum(const int *scissor)
{
	int i;
	double fpx = +1, fnx = -1, fpy = +1, fny = -1;
	vec3_t forward, left, up, origin, v;
	if(r_lockvisibility.integer)
		return;
	if(scissor)
	{
		// flipped x coordinates (because x points left here)
		fpx =  1.0 - 2.0 * (scissor[0]              - r_refdef.view.viewport.x) / (double) (r_refdef.view.viewport.width);
		fnx =  1.0 - 2.0 * (scissor[0] + scissor[2] - r_refdef.view.viewport.x) / (double) (r_refdef.view.viewport.width);
		// non-flipped y coordinates
		fny = -1.0 + 2.0 * (scissor[1]              - r_refdef.view.viewport.y) / (double) (r_refdef.view.viewport.height);
		fpy = -1.0 + 2.0 * (scissor[1] + scissor[3] - r_refdef.view.viewport.y) / (double) (r_refdef.view.viewport.height);
	}

	// we can't trust r_refdef.view.forward and friends in reflected scenes
	Matrix4x4_ToVectors(&r_refdef.view.matrix, forward, left, up, origin);

#if 0
	r_refdef.view.frustum[0].normal[0] = 0 - 1.0 / r_refdef.view.frustum_x;
	r_refdef.view.frustum[0].normal[1] = 0 - 0;
	r_refdef.view.frustum[0].normal[2] = -1 - 0;
	r_refdef.view.frustum[1].normal[0] = 0 + 1.0 / r_refdef.view.frustum_x;
	r_refdef.view.frustum[1].normal[1] = 0 + 0;
	r_refdef.view.frustum[1].normal[2] = -1 + 0;
	r_refdef.view.frustum[2].normal[0] = 0 - 0;
	r_refdef.view.frustum[2].normal[1] = 0 - 1.0 / r_refdef.view.frustum_y;
	r_refdef.view.frustum[2].normal[2] = -1 - 0;
	r_refdef.view.frustum[3].normal[0] = 0 + 0;
	r_refdef.view.frustum[3].normal[1] = 0 + 1.0 / r_refdef.view.frustum_y;
	r_refdef.view.frustum[3].normal[2] = -1 + 0;
#endif

#if 0
	zNear = r_refdef.nearclip;
	nudge = 1.0 - 1.0 / (1<<23);
	r_refdef.view.frustum[4].normal[0] = 0 - 0;
	r_refdef.view.frustum[4].normal[1] = 0 - 0;
	r_refdef.view.frustum[4].normal[2] = -1 - -nudge;
	r_refdef.view.frustum[4].dist = 0 - -2 * zNear * nudge;
	r_refdef.view.frustum[5].normal[0] = 0 + 0;
	r_refdef.view.frustum[5].normal[1] = 0 + 0;
	r_refdef.view.frustum[5].normal[2] = -1 + -nudge;
	r_refdef.view.frustum[5].dist = 0 + -2 * zNear * nudge;
#endif



#if 0
	r_refdef.view.frustum[0].normal[0] = m[3] - m[0];
	r_refdef.view.frustum[0].normal[1] = m[7] - m[4];
	r_refdef.view.frustum[0].normal[2] = m[11] - m[8];
	r_refdef.view.frustum[0].dist = m[15] - m[12];

	r_refdef.view.frustum[1].normal[0] = m[3] + m[0];
	r_refdef.view.frustum[1].normal[1] = m[7] + m[4];
	r_refdef.view.frustum[1].normal[2] = m[11] + m[8];
	r_refdef.view.frustum[1].dist = m[15] + m[12];

	r_refdef.view.frustum[2].normal[0] = m[3] - m[1];
	r_refdef.view.frustum[2].normal[1] = m[7] - m[5];
	r_refdef.view.frustum[2].normal[2] = m[11] - m[9];
	r_refdef.view.frustum[2].dist = m[15] - m[13];

	r_refdef.view.frustum[3].normal[0] = m[3] + m[1];
	r_refdef.view.frustum[3].normal[1] = m[7] + m[5];
	r_refdef.view.frustum[3].normal[2] = m[11] + m[9];
	r_refdef.view.frustum[3].dist = m[15] + m[13];

	r_refdef.view.frustum[4].normal[0] = m[3] - m[2];
	r_refdef.view.frustum[4].normal[1] = m[7] - m[6];
	r_refdef.view.frustum[4].normal[2] = m[11] - m[10];
	r_refdef.view.frustum[4].dist = m[15] - m[14];

	r_refdef.view.frustum[5].normal[0] = m[3] + m[2];
	r_refdef.view.frustum[5].normal[1] = m[7] + m[6];
	r_refdef.view.frustum[5].normal[2] = m[11] + m[10];
	r_refdef.view.frustum[5].dist = m[15] + m[14];
#endif

	if (r_refdef.view.useperspective)
	{
		// calculate frustum corners, which are used to calculate deformed frustum planes for shadow caster culling
		VectorMAMAM(1024, forward, fnx * 1024.0 * r_refdef.view.frustum_x, left, fny * 1024.0 * r_refdef.view.frustum_y, up, r_refdef.view.frustumcorner[0]);
		VectorMAMAM(1024, forward, fpx * 1024.0 * r_refdef.view.frustum_x, left, fny * 1024.0 * r_refdef.view.frustum_y, up, r_refdef.view.frustumcorner[1]);
		VectorMAMAM(1024, forward, fnx * 1024.0 * r_refdef.view.frustum_x, left, fpy * 1024.0 * r_refdef.view.frustum_y, up, r_refdef.view.frustumcorner[2]);
		VectorMAMAM(1024, forward, fpx * 1024.0 * r_refdef.view.frustum_x, left, fpy * 1024.0 * r_refdef.view.frustum_y, up, r_refdef.view.frustumcorner[3]);

		// then the normals from the corners relative to origin
		CrossProduct(r_refdef.view.frustumcorner[2], r_refdef.view.frustumcorner[0], r_refdef.view.frustum[0].normal);
		CrossProduct(r_refdef.view.frustumcorner[1], r_refdef.view.frustumcorner[3], r_refdef.view.frustum[1].normal);
		CrossProduct(r_refdef.view.frustumcorner[0], r_refdef.view.frustumcorner[1], r_refdef.view.frustum[2].normal);
		CrossProduct(r_refdef.view.frustumcorner[3], r_refdef.view.frustumcorner[2], r_refdef.view.frustum[3].normal);

		// in a NORMAL view, forward cross left == up
		// in a REFLECTED view, forward cross left == down
		// so our cross products above need to be adjusted for a left handed coordinate system
		CrossProduct(forward, left, v);
		if(DotProduct(v, up) < 0)
		{
			VectorNegate(r_refdef.view.frustum[0].normal, r_refdef.view.frustum[0].normal);
			VectorNegate(r_refdef.view.frustum[1].normal, r_refdef.view.frustum[1].normal);
			VectorNegate(r_refdef.view.frustum[2].normal, r_refdef.view.frustum[2].normal);
			VectorNegate(r_refdef.view.frustum[3].normal, r_refdef.view.frustum[3].normal);
		}

		// Leaving those out was a mistake, those were in the old code, and they
		// fix a reproducable bug in this one: frustum culling got fucked up when viewmatrix was an identity matrix
		// I couldn't reproduce it after adding those normalizations. --blub
		VectorNormalize(r_refdef.view.frustum[0].normal);
		VectorNormalize(r_refdef.view.frustum[1].normal);
		VectorNormalize(r_refdef.view.frustum[2].normal);
		VectorNormalize(r_refdef.view.frustum[3].normal);

		// make the corners absolute
		VectorAdd(r_refdef.view.frustumcorner[0], r_refdef.view.origin, r_refdef.view.frustumcorner[0]);
		VectorAdd(r_refdef.view.frustumcorner[1], r_refdef.view.origin, r_refdef.view.frustumcorner[1]);
		VectorAdd(r_refdef.view.frustumcorner[2], r_refdef.view.origin, r_refdef.view.frustumcorner[2]);
		VectorAdd(r_refdef.view.frustumcorner[3], r_refdef.view.origin, r_refdef.view.frustumcorner[3]);

		// one more normal
		VectorCopy(forward, r_refdef.view.frustum[4].normal);

		r_refdef.view.frustum[0].dist = DotProduct (r_refdef.view.origin, r_refdef.view.frustum[0].normal);
		r_refdef.view.frustum[1].dist = DotProduct (r_refdef.view.origin, r_refdef.view.frustum[1].normal);
		r_refdef.view.frustum[2].dist = DotProduct (r_refdef.view.origin, r_refdef.view.frustum[2].normal);
		r_refdef.view.frustum[3].dist = DotProduct (r_refdef.view.origin, r_refdef.view.frustum[3].normal);
		r_refdef.view.frustum[4].dist = DotProduct (r_refdef.view.origin, r_refdef.view.frustum[4].normal) + r_refdef.nearclip;
	}
	else
	{
		VectorScale(left, -1.0f, r_refdef.view.frustum[0].normal);
		VectorScale(left,  1.0f, r_refdef.view.frustum[1].normal);
		VectorScale(up, -1.0f, r_refdef.view.frustum[2].normal);
		VectorScale(up,  1.0f, r_refdef.view.frustum[3].normal);
		VectorScale(forward, -1.0f, r_refdef.view.frustum[4].normal);
		r_refdef.view.frustum[0].dist = DotProduct (r_refdef.view.origin, r_refdef.view.frustum[0].normal) - r_refdef.view.ortho_x;
		r_refdef.view.frustum[1].dist = DotProduct (r_refdef.view.origin, r_refdef.view.frustum[1].normal) - r_refdef.view.ortho_x;
		r_refdef.view.frustum[2].dist = DotProduct (r_refdef.view.origin, r_refdef.view.frustum[2].normal) - r_refdef.view.ortho_y;
		r_refdef.view.frustum[3].dist = DotProduct (r_refdef.view.origin, r_refdef.view.frustum[3].normal) - r_refdef.view.ortho_y;
		r_refdef.view.frustum[4].dist = DotProduct (r_refdef.view.origin, r_refdef.view.frustum[4].normal) - r_refdef.farclip;
	}
	r_refdef.view.numfrustumplanes = 5;

	if (r_refdef.view.useclipplane)
	{
		r_refdef.view.numfrustumplanes = 6;
		r_refdef.view.frustum[5] = r_refdef.view.clipplane;
	}

	for (i = 0;i < r_refdef.view.numfrustumplanes;i++)
		PlaneClassify(r_refdef.view.frustum + i);

	// LadyHavoc: note to all quake engine coders, Quake had a special case
	// for 90 degrees which assumed a square view (wrong), so I removed it,
	// Quake2 has it disabled as well.

	// rotate R_VIEWFORWARD right by FOV_X/2 degrees
	//RotatePointAroundVector( r_refdef.view.frustum[0].normal, up, forward, -(90 - r_refdef.fov_x / 2));
	//r_refdef.view.frustum[0].dist = DotProduct (r_refdef.view.origin, frustum[0].normal);
	//PlaneClassify(&frustum[0]);

	// rotate R_VIEWFORWARD left by FOV_X/2 degrees
	//RotatePointAroundVector( r_refdef.view.frustum[1].normal, up, forward, (90 - r_refdef.fov_x / 2));
	//r_refdef.view.frustum[1].dist = DotProduct (r_refdef.view.origin, frustum[1].normal);
	//PlaneClassify(&frustum[1]);

	// rotate R_VIEWFORWARD up by FOV_X/2 degrees
	//RotatePointAroundVector( r_refdef.view.frustum[2].normal, left, forward, -(90 - r_refdef.fov_y / 2));
	//r_refdef.view.frustum[2].dist = DotProduct (r_refdef.view.origin, frustum[2].normal);
	//PlaneClassify(&frustum[2]);

	// rotate R_VIEWFORWARD down by FOV_X/2 degrees
	//RotatePointAroundVector( r_refdef.view.frustum[3].normal, left, forward, (90 - r_refdef.fov_y / 2));
	//r_refdef.view.frustum[3].dist = DotProduct (r_refdef.view.origin, frustum[3].normal);
	//PlaneClassify(&frustum[3]);

	// nearclip plane
	//VectorCopy(forward, r_refdef.view.frustum[4].normal);
	//r_refdef.view.frustum[4].dist = DotProduct (r_refdef.view.origin, frustum[4].normal) + r_nearclip.value;
	//PlaneClassify(&frustum[4]);
}

static void R_View_Update(const int *myscissor)
{
	R_Main_ResizeViewCache();
	R_View_SetFrustum(myscissor);
	R_View_WorldVisibility(!r_refdef.view.usevieworiginculling);
	R_View_UpdateEntityVisible();
}

float viewscalefpsadjusted = 1.0f;

void R_SetupView(qbool allowwaterclippingplane, int viewfbo, rtexture_t *viewdepthtexture, rtexture_t *viewcolortexture, int viewx, int viewy, int viewwidth, int viewheight)
{
	const float *customclipplane = NULL;
	float plane[4];
	int viewy_adjusted;
	if (r_refdef.view.useclipplane && allowwaterclippingplane)
	{
		// LadyHavoc: couldn't figure out how to make this approach work the same in DPSOFTRAST
		vec_t dist = r_refdef.view.clipplane.dist - r_water_clippingplanebias.value;
		vec_t viewdist = DotProduct(r_refdef.view.origin, r_refdef.view.clipplane.normal);
		if (viewdist < r_refdef.view.clipplane.dist + r_water_clippingplanebias.value)
			dist = r_refdef.view.clipplane.dist;
		plane[0] = r_refdef.view.clipplane.normal[0];
		plane[1] = r_refdef.view.clipplane.normal[1];
		plane[2] = r_refdef.view.clipplane.normal[2];
		plane[3] = -dist;
		customclipplane = plane;
	}

	// GL is weird because it's bottom to top, r_refdef.view.y is top to bottom.
	// Unless the render target is a FBO...
	viewy_adjusted = viewfbo ? viewy : vid.mode.height - viewheight - viewy;

	if (!r_refdef.view.useperspective)
		R_Viewport_InitOrtho3D(&r_refdef.view.viewport, &r_refdef.view.matrix, viewx, viewy_adjusted, viewwidth, viewheight, r_refdef.view.ortho_x, r_refdef.view.ortho_y, -r_refdef.farclip, r_refdef.farclip, customclipplane);
	else if (vid.stencil && r_useinfinitefarclip.integer)
		R_Viewport_InitPerspectiveInfinite(&r_refdef.view.viewport, &r_refdef.view.matrix, viewx, viewy_adjusted, viewwidth, viewheight, r_refdef.view.frustum_x, r_refdef.view.frustum_y, r_refdef.nearclip, customclipplane);
	else
		R_Viewport_InitPerspective(&r_refdef.view.viewport, &r_refdef.view.matrix, viewx, viewy_adjusted, viewwidth, viewheight, r_refdef.view.frustum_x, r_refdef.view.frustum_y, r_refdef.nearclip, r_refdef.farclip, customclipplane);
	// The MetalFX-TEMPORAL arc's sub-pixel jitter, applied HERE rather than
	// inside the five R_Viewport_Init* builders, and to the FINISHED matrix.
	//
	// Those builders would entangle it with three separate things: the Metal
	// z-remap block negates GL m[9], v_flipped negates m[8], and
	// R_Viewport_ApplyNearClipPlaneFloatGL READS both to build the oblique
	// water clip plane. Adding the offset after Matrix4x4_FromArrayFloatGL
	// avoids all three -- and the shadowmap MVP, which is concatenated from
	// this very matrix (r_shadow.c), inherits the jitter for free, which is
	// what keeps model shadows registered with the geometry.
	//
	// matrix4x4_t is row-major with MATRIX4x4_OPENGLORIENTATION off, so GL's
	// m[8] and m[9] -- the column-2 slots that add a w-proportional shift to
	// clip x and y, i.e. an NDC translation -- are m[0][2] and m[1][2] here.
	//
	// Gated on taawanted AND on this being the main view's perspective
	// viewport: the water reflection/refraction sub-renders and the envmap
	// call R_SetupView too, they are sampled as textures rather than shown,
	// and jittering them would inject noise no temporal pass ever resolves.
	// Frustum culling is unaffected either way -- R_View_SetFrustum builds its
	// planes from r_refdef.view.matrix and the frustum scalars, never from the
	// projection (its extract-from-MVP form is #if 0).
	if (r_fb.taawanted && r_refdef.view.ismain && !r_refdef.envmap && r_refdef.view.useperspective
	 && !r_refdef.view.useclipplane && (r_fb.taa_jitter[0] != 0.0f || r_fb.taa_jitter[1] != 0.0f))
	{
		// jitter is in render PIXELS; an NDC span of 2 covers viewwidth of them
		r_refdef.view.viewport.projectmatrix.m[0][2] += r_fb.taa_jitter[0] * 2.0f / (float)max(viewwidth, 1);
		r_refdef.view.viewport.projectmatrix.m[1][2] += r_fb.taa_jitter[1] * 2.0f / (float)max(viewheight, 1);
	}
	R_Mesh_SetRenderTargets(viewfbo);
	R_SetViewport(&r_refdef.view.viewport);
}

// The MetalFX-TEMPORAL arc: decide this frame's jitter and whether the history
// survives, then publish both. Called once per main-view render, BEFORE
// R_ResetViewRendering3D, because R_SetupView reads r_fb.taa_jitter.
//
// The sequence is Halton(2,3) over 16 phases, the standard low-discrepancy
// choice: 16 sub-pixel positions that fill the pixel evenly and repeat, so the
// scaler sees a complete sample set within half a second at any playable frame
// rate. r_metalfx_jitterfix pins a constant instead -- that is the probe which
// settles the offset's magnitude and sign against an unjittered frame, and it
// must therefore bypass the sequence entirely rather than bias it.
static float R_TAA_Halton(unsigned int index, unsigned int base)
{
	float f = 1.0f, r = 0.0f;
	while (index > 0)
	{
		f /= (float)base;
		r += f * (float)(index % base);
		index /= base;
	}
	return r;
}

static void R_TAA_BeginFrame(int viewwidth, int viewheight)
{
	float fx = 0.0f, fy = 0.0f;
	qbool jumped;
	(void)viewwidth; (void)viewheight;

	r_fb.taawanted = R_MetalFX_TemporalWanted() && r_refdef.view.ismain && !r_refdef.envmap && !R_Stereo_Active();
	if (!r_fb.taawanted)
	{
		// Not this frame: the history is stale from here on, so the next frame
		// that DOES run the path must reset rather than reproject against a
		// camera that is any number of frames old.
		r_fb.taavalid = false;
		r_fb.taa_jitter[0] = r_fb.taa_jitter[1] = 0.0f;
		return;
	}
	r_fb.taa_frame++;

	if (r_metalfx_jitterfix.string[0] && strcmp(r_metalfx_jitterfix.string, "0"))
	{
		// "x y", quoted -- the console keeps only the first token otherwise,
		// which is the documented multi-token cvar trap
		if (sscanf(r_metalfx_jitterfix.string, "%f %f", &fx, &fy) != 2)
			fy = 0.0f;
	}
	else if (r_metalfx_jitter.value != 0.0f)
	{
		// Halton is on [0,1); centre it on the pixel so the mean offset is zero
		// and no systematic shift accumulates.
		fx = (R_TAA_Halton(r_fb.taa_frame % 16u + 1u, 2u) - 0.5f) * r_metalfx_jitter.value;
		fy = (R_TAA_Halton(r_fb.taa_frame % 16u + 1u, 3u) - 0.5f) * r_metalfx_jitter.value;
	}
	r_fb.taa_jitter[0] = fx;
	r_fb.taa_jitter[1] = fy;

	// A history cannot survive a camera that teleported. 64 units is well above
	// any single frame of running (320 units/sec at 60 fps is 5) and well below
	// a teleport, a respawn or a demo seek.
	jumped = r_fb.taavalid &&
		(fabs(r_refdef.view.origin[0] - r_fb.taa_prevorigin[0]) > 64.0f ||
		 fabs(r_refdef.view.origin[1] - r_fb.taa_prevorigin[1]) > 64.0f ||
		 fabs(r_refdef.view.origin[2] - r_fb.taa_prevorigin[2]) > 64.0f);
	r_fb.taareset = !r_fb.taavalid || jumped;
}

// cl_main.c, built by V_CalcRefdef -- the same file-local extern r_lightning.c
// and clvm_cmds.c already take for it, rather than a new header declaration for
// one global.
extern matrix4x4_t viewmodelmatrix_withbob;

// T2b helpers, defined with the motion pass further down.
static struct taa_prevent_s *R_TAA_PrevEnt(int entitynumber);
static qbool R_MotionVector_EntityWanted(const entity_render_t *ent);

// Snapshot the camera the frame was rendered WITH, for the next frame's motion
// vectors. Called after R_SetupView has built the viewport, so the view matrix
// and the frustum scalars are this frame's real ones.
static void R_TAA_EndFrame(void)
{
	if (!r_fb.taawanted)
		return;
	r_fb.taa_prevview = r_refdef.view.viewport.viewmatrix;
	r_fb.taa_prevfrustum[0] = r_refdef.view.frustum_x;
	r_fb.taa_prevfrustum[1] = r_refdef.view.frustum_y;
	VectorCopy(r_refdef.view.origin, r_fb.taa_prevorigin);
	// The weapon's own transform, for the viewmodel arm of the motion pass.
	// viewmodelmatrix_withbob is what cl_main.c hands the viewmodel entity as
	// its render matrix, so it carries the bob -- which is exactly the part a
	// "the weapon does not move on screen" approximation would get wrong.
	r_fb.taa_prevviewmodel = viewmodelmatrix_withbob;
	r_fb.taa_prevviewmodelvalid = true;
	// T2b: every visible network entity's matrix, stamped with this frame, so
	// next frame's pass can tell a genuine previous frame from a stale slot.
	if (r_metalfx_entities.integer)
	{
		int i;
		for (i = 0; i < r_refdef.scene.numentities; i++)
		{
			entity_render_t *ent = r_refdef.scene.entities[i];
			struct taa_prevent_s *pe;
			if (!r_refdef.viewcache.entityvisible[i] || !R_MotionVector_EntityWanted(ent))
				continue;
			pe = R_TAA_PrevEnt(ent->entitynumber);
			if (!pe)
				continue;
			pe->matrix = ent->matrix;
			pe->stamp = r_fb.taa_frame;
		}
	}
	r_fb.taavalid = true;
}

void R_EntityMatrix(const matrix4x4_t *matrix)
{
	if (gl_modelmatrixchanged || memcmp(matrix, &gl_modelmatrix, sizeof(matrix4x4_t)))
	{
		gl_modelmatrixchanged = false;
		gl_modelmatrix = *matrix;
		Matrix4x4_Concat(&gl_modelviewmatrix, &gl_viewmatrix, &gl_modelmatrix);
		Matrix4x4_Concat(&gl_modelviewprojectionmatrix, &gl_projectionmatrix, &gl_modelviewmatrix);
		Matrix4x4_ToArrayFloatGL(&gl_modelviewmatrix, gl_modelview16f);
		Matrix4x4_ToArrayFloatGL(&gl_modelviewprojectionmatrix, gl_modelviewprojection16f);
		CHECKGLERROR
		switch(vid.renderpath)
		{
		// Metal falls through: R_Shader_UniformMatrix4fv dispatches, and without
		// this the 2D geometry would land wherever the last matrix left it --
		// which reads as "nothing drew", not as "the matrix is stale".
		case RENDERPATH_METAL:
		case RENDERPATH_GL32:
		case RENDERPATH_GLES2:
			if (r_glsl_permutation && r_glsl_permutation->loc_ModelViewProjectionMatrix >= 0) R_Shader_UniformMatrix4fv(r_glsl_permutation->loc_ModelViewProjectionMatrix, 1, false, gl_modelviewprojection16f);
			if (r_glsl_permutation && r_glsl_permutation->loc_ModelViewMatrix >= 0) R_Shader_UniformMatrix4fv(r_glsl_permutation->loc_ModelViewMatrix, 1, false, gl_modelview16f);
			break;
		}
	}
}

void R_ResetViewRendering2D_Common(int viewfbo, rtexture_t *viewdepthtexture, rtexture_t *viewcolortexture, int viewx, int viewy, int viewwidth, int viewheight, float x2, float y2)
{
	r_viewport_t viewport;
	int viewy_adjusted;

	CHECKGLERROR

	// GL is weird because it's bottom to top, r_refdef.view.y is top to bottom.
	// Unless the render target is a FBO...
	viewy_adjusted = viewfbo ? viewy : vid.mode.height - viewheight - viewy;

	R_Viewport_InitOrtho(&viewport, &identitymatrix, viewx, viewy_adjusted, viewwidth, viewheight, 0, 0, x2, y2, -10, 100, NULL);
	R_Mesh_SetRenderTargets(viewfbo);
	R_SetViewport(&viewport);
	GL_Scissor(viewport.x, viewport.y, viewport.width, viewport.height);
	GL_Color(1, 1, 1, 1);
	GL_ColorMask(r_refdef.view.colormask[0], r_refdef.view.colormask[1], r_refdef.view.colormask[2], 1);
	GL_BlendFunc(GL_ONE, GL_ZERO);
	GL_ScissorTest(false);
	GL_DepthMask(false);
	GL_DepthRange(0, 1);
	GL_DepthTest(false);
	GL_DepthFunc(GL_LEQUAL);
	R_EntityMatrix(&identitymatrix);
	R_Mesh_ResetTextureState();
	GL_PolygonOffset(0, 0);
	switch(vid.renderpath)
	{
	case RENDERPATH_METAL:
		break; // METAL_TODO: Phase 0 stub (METAL.md)
	case RENDERPATH_GL32:
	case RENDERPATH_GLES2:
		qglEnable(GL_POLYGON_OFFSET_FILL);CHECKGLERROR
		break;
	}
	GL_CullFace(GL_NONE);

	CHECKGLERROR
}

void R_ResetViewRendering2D(int viewfbo, rtexture_t *viewdepthtexture, rtexture_t *viewcolortexture, int viewx, int viewy, int viewwidth, int viewheight)
{
	R_ResetViewRendering2D_Common(viewfbo, viewdepthtexture, viewcolortexture, viewx, viewy, viewwidth, viewheight, 1.0f, 1.0f);
}

void R_ResetViewRendering3D(int viewfbo, rtexture_t *viewdepthtexture, rtexture_t *viewcolortexture, int viewx, int viewy, int viewwidth, int viewheight)
{
	R_SetupView(true, viewfbo, viewdepthtexture, viewcolortexture, viewx, viewy, viewwidth, viewheight);
	GL_Scissor(r_refdef.view.viewport.x, r_refdef.view.viewport.y, r_refdef.view.viewport.width, r_refdef.view.viewport.height);
	GL_Color(1, 1, 1, 1);
	GL_ColorMask(r_refdef.view.colormask[0], r_refdef.view.colormask[1], r_refdef.view.colormask[2], 1);
	GL_BlendFunc(GL_ONE, GL_ZERO);
	GL_ScissorTest(true);
	GL_DepthMask(true);
	GL_DepthRange(0, 1);
	GL_DepthTest(true);
	GL_DepthFunc(GL_LEQUAL);
	R_EntityMatrix(&identitymatrix);
	R_Mesh_ResetTextureState();
	GL_PolygonOffset(r_refdef.polygonfactor, r_refdef.polygonoffset);
	switch(vid.renderpath)
	{
	case RENDERPATH_METAL:
		break; // METAL_TODO: Phase 0 stub (METAL.md)
	case RENDERPATH_GL32:
	case RENDERPATH_GLES2:
		qglEnable(GL_POLYGON_OFFSET_FILL);CHECKGLERROR
		break;
	}
	GL_CullFace(r_refdef.view.cullface_back);
}

/*
================
R_RenderView_UpdateViewVectors
================
*/
void R_RenderView_UpdateViewVectors(void)
{
	// break apart the view matrix into vectors for various purposes
	// it is important that this occurs outside the RenderScene function because that can be called from reflection renders, where the vectors come out wrong
	// however the r_refdef.view.origin IS updated in RenderScene intentionally - otherwise the sky renders at the wrong origin, etc
	Matrix4x4_ToVectors(&r_refdef.view.matrix, r_refdef.view.forward, r_refdef.view.left, r_refdef.view.up, r_refdef.view.origin);
	VectorNegate(r_refdef.view.left, r_refdef.view.right);
	// make an inverted copy of the view matrix for tracking sprites
	Matrix4x4_Invert_Full(&r_refdef.view.inverse_matrix, &r_refdef.view.matrix);
}

void R_RenderTarget_FreeUnused(qbool force)
{
	unsigned int i, j, end;
	end = (unsigned int)Mem_ExpandableArray_IndexRange(&r_fb.rendertargets); // checked
	for (i = 0; i < end; i++)
	{
		r_rendertarget_t *r = (r_rendertarget_t *)Mem_ExpandableArray_RecordAtIndex(&r_fb.rendertargets, i);
		// free resources for rendertargets that have not been used for a while
		// (note: this check is run after the frame render, so any targets used
		// this frame will not be affected even at low framerates)
		if (r && (host.realtime - r->lastusetime > 0.2 || force))
		{
			if (r->fbo)
				R_Mesh_DestroyFramebufferObject(r->fbo);
			for (j = 0; j < sizeof(r->colortexture) / sizeof(r->colortexture[0]); j++)
				if (r->colortexture[j])
					R_FreeTexture(r->colortexture[j]);
			if (r->depthtexture)
				R_FreeTexture(r->depthtexture);
			Mem_ExpandableArray_FreeRecord(&r_fb.rendertargets, r);
		}
	}
}

static void R_CalcTexCoordsForView(float x, float y, float w, float h, float tw, float th, float *texcoord2f)
{
	float iw = 1.0f / tw, ih = 1.0f / th, x1, y1, x2, y2;
	x1 = x * iw;
	x2 = (x + w) * iw;
	y1 = (th - y) * ih;
	y2 = (th - y - h) * ih;
	texcoord2f[0] = x1;
	texcoord2f[2] = x2;
	texcoord2f[4] = x2;
	texcoord2f[6] = x1;
	texcoord2f[1] = y1;
	texcoord2f[3] = y1;
	texcoord2f[5] = y2;
	texcoord2f[7] = y2;
}

r_rendertarget_t *R_RenderTarget_Get(int texturewidth, int textureheight, textype_t depthtextype, qbool depthisrenderbuffer, textype_t colortextype0, textype_t colortextype1, textype_t colortextype2, textype_t colortextype3)
{
	unsigned int i, j, end;
	r_rendertarget_t *r = NULL;
	char vabuf[256];
	// first try to reuse an existing slot if possible
	end = (unsigned int)Mem_ExpandableArray_IndexRange(&r_fb.rendertargets); // checked
	for (i = 0; i < end; i++)
	{
		r = (r_rendertarget_t *)Mem_ExpandableArray_RecordAtIndex(&r_fb.rendertargets, i);
		// depthisrenderbuffer is part of the match: a caller asking for a write-only
		// depth renderbuffer must never be handed a target whose depth is a sampleable
		// texture (or vice versa). Before this was compared, the water/refraction and
		// envmap targets — which request renderbuffer depth with the same depthtextype
		// and can share the screen target's dimensions — could silently be given the
		// volumetrics depth-texture target, and r->depthisrenderbuffer became a lie.
		if (r && r->lastusetime != host.realtime && r->texturewidth == texturewidth && r->textureheight == textureheight && r->depthtextype == depthtextype && r->depthisrenderbuffer == depthisrenderbuffer && r->colortextype[0] == colortextype0 && r->colortextype[1] == colortextype1 && r->colortextype[2] == colortextype2 && r->colortextype[3] == colortextype3)
			break;
	}
	if (i == end)
	{
		// no unused exact match found, so we have to make one in the first unused slot
		r = (r_rendertarget_t *)Mem_ExpandableArray_AllocRecord(&r_fb.rendertargets);
		r->texturewidth = texturewidth;
		r->textureheight = textureheight;
		r->colortextype[0] = colortextype0;
		r->colortextype[1] = colortextype1;
		r->colortextype[2] = colortextype2;
		r->colortextype[3] = colortextype3;
		r->depthtextype = depthtextype;
		r->depthisrenderbuffer = depthisrenderbuffer;
		for (j = 0; j < 4; j++)
			if (r->colortextype[j])
				r->colortexture[j] = R_LoadTexture2D(r_main_texturepool, va(vabuf, sizeof(vabuf), "rendertarget%i_%i_type%i", i, j, (int)r->colortextype[j]), r->texturewidth, r->textureheight, NULL, r->colortextype[j], TEXF_RENDERTARGET | TEXF_FORCELINEAR | TEXF_CLAMP, -1, NULL);
		if (r->depthtextype)
		{
			if (r->depthisrenderbuffer)
				r->depthtexture = R_LoadTextureRenderBuffer(r_main_texturepool, va(vabuf, sizeof(vabuf), "renderbuffer%i_depth_type%i", i, (int)r->depthtextype), r->texturewidth, r->textureheight, r->depthtextype);
			else
				r->depthtexture = R_LoadTexture2D(r_main_texturepool, va(vabuf, sizeof(vabuf), "rendertarget%i_depth_type%i", i, (int)r->depthtextype), r->texturewidth, r->textureheight, NULL, r->depthtextype, TEXF_RENDERTARGET | TEXF_FORCELINEAR | TEXF_CLAMP, -1, NULL);
		}
		r->fbo = R_Mesh_CreateFramebufferObject(r->depthtexture, r->colortexture[0], r->colortexture[1], r->colortexture[2], r->colortexture[3]);
	}
	r_refdef.stats[r_stat_rendertargets_used]++;
	r_refdef.stats[r_stat_rendertargets_pixels] += r->texturewidth * r->textureheight;
	r->lastusetime = host.realtime;
	R_CalcTexCoordsForView(0, 0, r->texturewidth, r->textureheight, r->texturewidth, r->textureheight, r->texcoord2f);
	return r;
}

static void R_Water_StartFrame(int viewwidth, int viewheight)
{
	int waterwidth, waterheight;

	if (viewwidth > (int)vid.maxtexturesize_2d || viewheight > (int)vid.maxtexturesize_2d)
		return;

	// set waterwidth and waterheight to the water resolution that will be
	// used (often less than the screen resolution for faster rendering)
	waterwidth = (int)bound(16, viewwidth * r_water_resolutionmultiplier.value, viewwidth);
	waterheight = (int)bound(16, viewheight * r_water_resolutionmultiplier.value, viewheight);

	if (!r_water.integer || r_showsurfaces.integer || r_lockvisibility.integer || r_lockpvs.integer)
		waterwidth = waterheight = 0;

	// set up variables that will be used in shader setup
	r_fb.water.waterwidth = waterwidth;
	r_fb.water.waterheight = waterheight;
	r_fb.water.texturewidth = waterwidth;
	r_fb.water.textureheight = waterheight;
	r_fb.water.camerawidth = waterwidth;
	r_fb.water.cameraheight = waterheight;
	r_fb.water.screenscale[0] = 0.5f;
	r_fb.water.screenscale[1] = 0.5f;
	r_fb.water.screencenter[0] = 0.5f;
	r_fb.water.screencenter[1] = 0.5f;
	r_fb.water.enabled = waterwidth != 0;

	r_fb.water.maxwaterplanes = MAX_WATERPLANES;
	r_fb.water.numwaterplanes = 0;
}

void R_Water_AddWaterPlane(msurface_t *surface, int entno)
{
	int planeindex, bestplaneindex, vertexindex;
	vec3_t mins, maxs, normal, center, v, n;
	vec_t planescore, bestplanescore;
	mplane_t plane;
	r_waterstate_waterplane_t *p;
	texture_t *t = R_GetCurrentTexture(surface->texture);

	rsurface.texture = t;
	RSurf_PrepareVerticesForBatch(BATCHNEED_ARRAY_VERTEX | BATCHNEED_ARRAY_NORMAL | BATCHNEED_NOGAPS, 1, ((const msurface_t **)&surface));
	// if the model has no normals, it's probably off-screen and they were not generated, so don't add it anyway
	if (!rsurface.batchnormal3f || rsurface.batchnumvertices < 1)
		return;
	// average the vertex normals, find the surface bounds (after deformvertexes)
	Matrix4x4_Transform(&rsurface.matrix, rsurface.batchvertex3f, v);
	Matrix4x4_Transform3x3(&rsurface.matrix, rsurface.batchnormal3f, n);
	VectorCopy(n, normal);
	VectorCopy(v, mins);
	VectorCopy(v, maxs);
	for (vertexindex = 1;vertexindex < rsurface.batchnumvertices;vertexindex++)
	{
		Matrix4x4_Transform(&rsurface.matrix, rsurface.batchvertex3f + vertexindex*3, v);
		Matrix4x4_Transform3x3(&rsurface.matrix, rsurface.batchnormal3f + vertexindex*3, n);
		VectorAdd(normal, n, normal);
		mins[0] = min(mins[0], v[0]);
		mins[1] = min(mins[1], v[1]);
		mins[2] = min(mins[2], v[2]);
		maxs[0] = max(maxs[0], v[0]);
		maxs[1] = max(maxs[1], v[1]);
		maxs[2] = max(maxs[2], v[2]);
	}
	VectorNormalize(normal);
	VectorMAM(0.5f, mins, 0.5f, maxs, center);

	VectorCopy(normal, plane.normal);
	VectorNormalize(plane.normal);
	plane.dist = DotProduct(center, plane.normal);
	PlaneClassify(&plane);
	if (PlaneDiff(r_refdef.view.origin, &plane) < 0)
	{
		// skip backfaces (except if nocullface is set)
//		if (!(t->currentmaterialflags & MATERIALFLAG_NOCULLFACE))
//			return;
		VectorNegate(plane.normal, plane.normal);
		plane.dist *= -1;
		PlaneClassify(&plane);
	}


	// find a matching plane if there is one
	bestplaneindex = -1;
	bestplanescore = 1048576.0f;
	for (planeindex = 0, p = r_fb.water.waterplanes;planeindex < r_fb.water.numwaterplanes;planeindex++, p++)
	{
		if(p->camera_entity == t->camera_entity)
		{
			planescore = 1.0f - DotProduct(plane.normal, p->plane.normal) + fabs(plane.dist - p->plane.dist) * 0.001f;
			if (bestplaneindex < 0 || bestplanescore > planescore)
			{
				bestplaneindex = planeindex;
				bestplanescore = planescore;
			}
		}
	}
	planeindex = bestplaneindex;

	// if this surface does not fit any known plane rendered this frame, add one
	if (planeindex < 0 || bestplanescore > 0.001f)
	{
		if (r_fb.water.numwaterplanes < r_fb.water.maxwaterplanes)
		{
			// store the new plane
			planeindex = r_fb.water.numwaterplanes++;
			p = r_fb.water.waterplanes + planeindex;
			p->plane = plane;
			// clear materialflags and pvs
			p->materialflags = 0;
			p->pvsvalid = false;
			p->camera_entity = t->camera_entity;
			VectorCopy(mins, p->mins);
			VectorCopy(maxs, p->maxs);
		}
		else
		{
			// We're totally screwed.
			return;
		}
	}
	else
	{
		// merge mins/maxs when we're adding this surface to the plane
		p = r_fb.water.waterplanes + planeindex;
		p->mins[0] = min(p->mins[0], mins[0]);
		p->mins[1] = min(p->mins[1], mins[1]);
		p->mins[2] = min(p->mins[2], mins[2]);
		p->maxs[0] = max(p->maxs[0], maxs[0]);
		p->maxs[1] = max(p->maxs[1], maxs[1]);
		p->maxs[2] = max(p->maxs[2], maxs[2]);
	}
	// merge this surface's materialflags into the waterplane
	p->materialflags |= t->currentmaterialflags;
	if(!(p->materialflags & MATERIALFLAG_CAMERA))
	{
		// merge this surface's PVS into the waterplane
		if (p->materialflags & (MATERIALFLAG_WATERSHADER | MATERIALFLAG_REFRACTION | MATERIALFLAG_REFLECTION) && r_refdef.scene.worldmodel && r_refdef.scene.worldmodel->brush.FatPVS
		 && r_refdef.scene.worldmodel->brush.PointInLeaf && r_refdef.scene.worldmodel->brush.PointInLeaf(r_refdef.scene.worldmodel, center)->clusterindex >= 0)
		{
			r_refdef.scene.worldmodel->brush.FatPVS(r_refdef.scene.worldmodel, center, 2, &p->pvsbits, r_main_mempool, p->pvsvalid);
			p->pvsvalid = true;
		}
	}
}

extern cvar_t r_drawparticles;
extern cvar_t r_drawdecals;

static void R_Water_ProcessPlanes(int fbo, rtexture_t *depthtexture, rtexture_t *colortexture, int viewx, int viewy, int viewwidth, int viewheight)
{
	int myscissor[4];
	r_refdef_view_t originalview;
	r_refdef_view_t myview;
	int planeindex, qualityreduction = 0, old_r_dynamic = 0, old_r_shadows = 0, old_r_worldrtlight = 0, old_r_dlight = 0, old_r_particles = 0, old_r_decals = 0;
	r_waterstate_waterplane_t *p;
	vec3_t visorigin;
	r_rendertarget_t *rt;

	originalview = r_refdef.view;

	// lowquality hack, temporarily shut down some cvars and restore afterwards
	qualityreduction = r_water_lowquality.integer;
	if (qualityreduction > 0)
	{
		if (qualityreduction >= 1)
		{
			old_r_shadows = r_shadows.integer;
			old_r_worldrtlight = r_shadow_realtime_world.integer;
			old_r_dlight = r_shadow_realtime_dlight.integer;
			Cvar_SetValueQuick(&r_shadows, 0);
			Cvar_SetValueQuick(&r_shadow_realtime_world, 0);
			Cvar_SetValueQuick(&r_shadow_realtime_dlight, 0);
		}
		if (qualityreduction >= 2)
		{
			old_r_dynamic = r_dynamic.integer;
			old_r_particles = r_drawparticles.integer;
			old_r_decals = r_drawdecals.integer;
			Cvar_SetValueQuick(&r_dynamic, 0);
			Cvar_SetValueQuick(&r_drawparticles, 0);
			Cvar_SetValueQuick(&r_drawdecals, 0);
		}
	}

	for (planeindex = 0, p = r_fb.water.waterplanes; planeindex < r_fb.water.numwaterplanes; planeindex++, p++)
	{
		p->rt_reflection = NULL;
		p->rt_refraction = NULL;
		p->rt_camera = NULL;
	}

	// render views
	r_refdef.view = originalview;
	r_refdef.view.showdebug = false;
	r_refdef.view.width = r_fb.water.waterwidth;
	r_refdef.view.height = r_fb.water.waterheight;
	r_refdef.view.useclipplane = true;
	myview = r_refdef.view;
	r_fb.water.renderingscene = true;
	for (planeindex = 0, p = r_fb.water.waterplanes;planeindex < r_fb.water.numwaterplanes;planeindex++, p++)
	{
		if (r_water_cameraentitiesonly.value != 0 && !p->camera_entity)
			continue;

		if (p->materialflags & (MATERIALFLAG_WATERSHADER | MATERIALFLAG_REFLECTION))
		{
			rt = R_RenderTarget_Get(r_fb.water.waterwidth, r_fb.water.waterheight, TEXTYPE_DEPTHBUFFER24STENCIL8, true, r_fb.rt_screen->colortextype[0], TEXTYPE_UNUSED, TEXTYPE_UNUSED, TEXTYPE_UNUSED);
			if (rt->colortexture[0] == NULL || rt->depthtexture == NULL)
				goto error;
			r_refdef.view = myview;
			Matrix4x4_Reflect(&r_refdef.view.matrix, p->plane.normal[0], p->plane.normal[1], p->plane.normal[2], p->plane.dist, -2);
			Matrix4x4_OriginFromMatrix(&r_refdef.view.matrix, r_refdef.view.origin);
			if(r_water_scissormode.integer)
			{
				R_SetupView(true, rt->fbo, rt->depthtexture, rt->colortexture[0], 0, 0, r_fb.water.waterwidth, r_fb.water.waterheight);
				if (R_ScissorForBBox(p->mins, p->maxs, myscissor))
				{
					p->rt_reflection = NULL;
					p->rt_refraction = NULL;
					p->rt_camera = NULL;
					continue;
				}
			}

			r_refdef.view.clipplane = p->plane;
			// reflected view origin may be in solid, so don't cull with it
			r_refdef.view.usevieworiginculling = false;
			// reverse the cullface settings for this render
			r_refdef.view.cullface_front = GL_FRONT;
			r_refdef.view.cullface_back = GL_BACK;
			// combined pvs (based on what can be seen from each surface center)
			if (r_refdef.scene.worldmodel && r_refdef.scene.worldmodel->brush.num_pvsclusterbytes)
			{
				r_refdef.view.usecustompvs = true;
				if (p->pvsvalid)
					memcpy(r_refdef.viewcache.world_pvsbits, p->pvsbits, r_refdef.scene.worldmodel->brush.num_pvsclusterbytes);
				else
					memset(r_refdef.viewcache.world_pvsbits, 0xFF, r_refdef.scene.worldmodel->brush.num_pvsclusterbytes);
			}

			r_fb.water.hideplayer = ((r_water_hideplayer.integer >= 2) && !chase_active.integer);
			R_ResetViewRendering3D(rt->fbo, rt->depthtexture, rt->colortexture[0], 0, 0, rt->texturewidth, rt->textureheight);
			GL_ScissorTest(false);
			R_ClearScreen(r_refdef.fogenabled);
			GL_ScissorTest(true);
			R_View_Update(r_water_scissormode.integer & 2 ? myscissor : NULL);
			R_AnimCache_CacheVisibleEntities();
			if(r_water_scissormode.integer & 1)
				GL_Scissor(myscissor[0], myscissor[1], myscissor[2], myscissor[3]);
			R_RenderScene(rt->fbo, rt->depthtexture, rt->colortexture[0], 0, 0, rt->texturewidth, rt->textureheight);

			r_fb.water.hideplayer = false;
			p->rt_reflection = rt;
		}

		// render the normal view scene and copy into texture
		// (except that a clipping plane should be used to hide everything on one side of the water, and the viewer's weapon model should be omitted)
		if (p->materialflags & (MATERIALFLAG_WATERSHADER | MATERIALFLAG_REFRACTION))
		{
			rt = R_RenderTarget_Get(r_fb.water.waterwidth, r_fb.water.waterheight, TEXTYPE_DEPTHBUFFER24STENCIL8, true, r_fb.rt_screen->colortextype[0], TEXTYPE_UNUSED, TEXTYPE_UNUSED, TEXTYPE_UNUSED);
			if (rt->colortexture[0] == NULL || rt->depthtexture == NULL)
				goto error;
			r_refdef.view = myview;
			if(r_water_scissormode.integer)
			{
				R_SetupView(true, rt->fbo, rt->depthtexture, rt->colortexture[0], 0, 0, r_fb.water.waterwidth, r_fb.water.waterheight);
				if (R_ScissorForBBox(p->mins, p->maxs, myscissor))
				{
					p->rt_reflection = NULL;
					p->rt_refraction = NULL;
					p->rt_camera = NULL;
					continue;
				}
			}

			// combined pvs (based on what can be seen from each surface center)
			if (r_refdef.scene.worldmodel && r_refdef.scene.worldmodel->brush.num_pvsclusterbytes)
			{
				r_refdef.view.usecustompvs = true;
				if (p->pvsvalid)
					memcpy(r_refdef.viewcache.world_pvsbits, p->pvsbits, r_refdef.scene.worldmodel->brush.num_pvsclusterbytes);
				else
					memset(r_refdef.viewcache.world_pvsbits, 0xFF, r_refdef.scene.worldmodel->brush.num_pvsclusterbytes);
			}

			r_fb.water.hideplayer = ((r_water_hideplayer.integer >= 1) && !chase_active.integer);

			r_refdef.view.clipplane = p->plane;
			VectorNegate(r_refdef.view.clipplane.normal, r_refdef.view.clipplane.normal);
			r_refdef.view.clipplane.dist = -r_refdef.view.clipplane.dist;

			if((p->materialflags & MATERIALFLAG_CAMERA) && p->camera_entity)
			{
				// we need to perform a matrix transform to render the view... so let's get the transformation matrix
				r_fb.water.hideplayer = false; // we don't want to hide the player model from these ones
				CL_VM_TransformView(p->camera_entity - MAX_EDICTS, &r_refdef.view.matrix, &r_refdef.view.clipplane, visorigin);
				R_RenderView_UpdateViewVectors();
				if(r_refdef.scene.worldmodel && r_refdef.scene.worldmodel->brush.FatPVS)
				{
					r_refdef.view.usecustompvs = true;
					r_refdef.scene.worldmodel->brush.FatPVS(r_refdef.scene.worldmodel, visorigin, 2, &r_refdef.viewcache.world_pvsbits, r_main_mempool, false);
				}
			}

			PlaneClassify(&r_refdef.view.clipplane);

			R_ResetViewRendering3D(rt->fbo, rt->depthtexture, rt->colortexture[0], 0, 0, rt->texturewidth, rt->textureheight);
			GL_ScissorTest(false);
			R_ClearScreen(r_refdef.fogenabled);
			GL_ScissorTest(true);
			R_View_Update(r_water_scissormode.integer & 2 ? myscissor : NULL);
			R_AnimCache_CacheVisibleEntities();
			if(r_water_scissormode.integer & 1)
				GL_Scissor(myscissor[0], myscissor[1], myscissor[2], myscissor[3]);
			R_RenderScene(rt->fbo, rt->depthtexture, rt->colortexture[0], 0, 0, rt->texturewidth, rt->textureheight);

			r_fb.water.hideplayer = false;
			p->rt_refraction = rt;
		}
		else if (p->materialflags & MATERIALFLAG_CAMERA)
		{
			rt = R_RenderTarget_Get(r_fb.water.waterwidth, r_fb.water.waterheight, TEXTYPE_DEPTHBUFFER24STENCIL8, true, r_fb.rt_screen->colortextype[0], TEXTYPE_UNUSED, TEXTYPE_UNUSED, TEXTYPE_UNUSED);
			if (rt->colortexture[0] == NULL || rt->depthtexture == NULL)
				goto error;
			r_refdef.view = myview;

			r_refdef.view.clipplane = p->plane;
			VectorNegate(r_refdef.view.clipplane.normal, r_refdef.view.clipplane.normal);
			r_refdef.view.clipplane.dist = -r_refdef.view.clipplane.dist;

			r_refdef.view.width = r_fb.water.camerawidth;
			r_refdef.view.height = r_fb.water.cameraheight;
			r_refdef.view.frustum_x = 1; // tan(45 * M_PI / 180.0);
			r_refdef.view.frustum_y = 1; // tan(45 * M_PI / 180.0);
			r_refdef.view.ortho_x = 90; // abused as angle by VM_CL_R_SetView
			r_refdef.view.ortho_y = 90; // abused as angle by VM_CL_R_SetView

			if(p->camera_entity)
			{
				// we need to perform a matrix transform to render the view... so let's get the transformation matrix
				CL_VM_TransformView(p->camera_entity - MAX_EDICTS, &r_refdef.view.matrix, &r_refdef.view.clipplane, visorigin);
			}

			// note: all of the view is used for displaying... so
			// there is no use in scissoring

			// reverse the cullface settings for this render
			r_refdef.view.cullface_front = GL_FRONT;
			r_refdef.view.cullface_back = GL_BACK;
			// also reverse the view matrix
			Matrix4x4_ConcatScale3(&r_refdef.view.matrix, 1, 1, -1); // this serves to invert texcoords in the result, as the copied texture is mapped the wrong way round
			R_RenderView_UpdateViewVectors();
			if(p->camera_entity && r_refdef.scene.worldmodel && r_refdef.scene.worldmodel->brush.FatPVS)
			{
				r_refdef.view.usecustompvs = true;
				r_refdef.scene.worldmodel->brush.FatPVS(r_refdef.scene.worldmodel, visorigin, 2, &r_refdef.viewcache.world_pvsbits, r_main_mempool, false);
			}

			// camera needs no clipplane
			r_refdef.view.useclipplane = false;
			// TODO: is the camera origin always valid?  if so we don't need to clear this
			r_refdef.view.usevieworiginculling = false;

			PlaneClassify(&r_refdef.view.clipplane);

			r_fb.water.hideplayer = false;

			R_ResetViewRendering3D(rt->fbo, rt->depthtexture, rt->colortexture[0], 0, 0, rt->texturewidth, rt->textureheight);
			GL_ScissorTest(false);
			R_ClearScreen(r_refdef.fogenabled);
			GL_ScissorTest(true);
			R_View_Update(NULL);
			R_AnimCache_CacheVisibleEntities();
			R_RenderScene(rt->fbo, rt->depthtexture, rt->colortexture[0], 0, 0, rt->texturewidth, rt->textureheight);

			r_fb.water.hideplayer = false;
			p->rt_camera = rt;
		}

	}
	r_fb.water.renderingscene = false;
	r_refdef.view = originalview;
	R_ResetViewRendering3D(fbo, depthtexture, colortexture, viewx, viewy, viewwidth, viewheight);
	R_View_Update(NULL);
	R_AnimCache_CacheVisibleEntities();
	goto finish;
error:
	r_refdef.view = originalview;
	r_fb.water.renderingscene = false;
	Cvar_SetValueQuick(&r_water, 0);
	Con_Printf("R_Water_ProcessPlanes: Error: texture creation failed!  Turned off r_water.\n");
finish:
	// lowquality hack, restore cvars
	if (qualityreduction > 0)
	{
		if (qualityreduction >= 1)
		{
			Cvar_SetValueQuick(&r_shadows, old_r_shadows);
			Cvar_SetValueQuick(&r_shadow_realtime_world, old_r_worldrtlight);
			Cvar_SetValueQuick(&r_shadow_realtime_dlight, old_r_dlight);
		}
		if (qualityreduction >= 2)
		{
			Cvar_SetValueQuick(&r_dynamic, old_r_dynamic);
			Cvar_SetValueQuick(&r_drawparticles, old_r_particles);
			Cvar_SetValueQuick(&r_drawdecals, old_r_decals);
		}
	}
}

/// F1. Defined with the bolt pass far below, but both frame-shape decisions --
/// the sampleable depth attachment here and the offscreen path in
/// R_BlendView_IsTrivial -- are taken before it runs.
static qbool R_LightningM5_Wanted(void);
#ifdef USE_RT_METAL
// WARCHEST session 1 (2026-08-28): the RT term upsample reads the scene depth
// in the composite, so it is an ASKER in the depth-as-texture choice below --
// the same mechanism the murk, the M5 bolt and the shimmer already use. Note
// depth-as-texture measured FASTER than a renderbuffer on this hardware
// (volumetrics step 1), so the ask costs nothing.
//
// NARROWED the same night, on principle rather than on a defect: every OTHER
// asker in the predicate also forces the offscreen path, so its ask is never
// a dangling allocation switch -- this one now counts only when the
// offscreen path is live for a reason the predicate cannot already see
// (r_viewfbo itself, or EDR via R_EDR_Wanted, the house single-predicate;
// the murk and MetalFX temporal ask for themselves on the lines beside).
// The investigation that led here is worth its two sentences: at rt-vantage
// defaults Metal's frame differed from GL's by the feature's own edge
// sharpening (rt_ogre 2960 px @ max 122) and a first bisection wrongly
// blamed this asker's ALLOCATION switch -- the truth is that the M5 bolt's
// asker (vid.blendequationmax, Metal-only) forces the offscreen path on
// Metal at plain defaults, so the upsample was genuinely LIVE there and
// structurally dormant on GL: the documented METAL-ONLY-BY-CAPABILITY
// class, handled where that class is always handled, by a parity-preamble
// pin. No allocation mechanism was ever involved.
static qbool R_EDR_Wanted(void);
static qbool R_RTTermUpsample_Wanted(void)
{
	return rt_metal.integer && rt_metal_term_upsample.integer
		&& (R_ViewFBO() >= 1 || R_EDR_Wanted());
}
#else
#define R_RTTermUpsample_Wanted() false
#endif

// The MetalFX-TEMPORAL arc's one predicate, in the R_EDR_Wanted shape: named
// consumers rather than four expressions that happen to agree. Like EDR's it is
// a one-frame-lagged PREDICTION when read from R_Bloom_StartFrame and
// R_BlendView_IsTrivial -- it says what the frame is being set up FOR, and the
// gate below re-decides for real once the sizes are known. Every way of the
// gate refusing lands on the spatial or bilinear path for that frame, and on a
// history reset for the next one, so a wrong prediction costs a frame of
// accumulation and never a wrong picture.
//
// Temporal upscaling forces two prerequisites, both of which the engine already
// knows how to force for other features: the offscreen scene path (r_volumetric
// and r_edr both do it, and without it there is no render-res colour buffer to
// upscale), and a SAMPLEABLE depth texture rather than a write-only
// renderbuffer -- the scaler takes depth as an input, and so does the
// motion-vector pass that feeds it.
qbool R_MetalFX_TemporalWanted(void)
{
	return r_metalfx.integer >= 2 && vid.renderpath == RENDERPATH_METAL && MetalFX_TemporalAvailable();
}

static void R_Bloom_StartFrame(void)
{
	int screentexturewidth, screentextureheight;
	textype_t textype = TEXTYPE_COLORBUFFER;
	double scale;

	// clear the pointers to rendertargets from last frame as they're stale
	r_fb.rt_screen = NULL;
	r_fb.rt_bloom = NULL;

	switch (vid.renderpath)
	{
	case RENDERPATH_METAL:
		// METAL: depth is always a sampleable texture; float scene buffers native.
		r_fb.usedepthtextures = r_usedepthtextures.integer != 0;
		if (R_ViewFBO() == 2) textype = TEXTYPE_COLORBUFFER16F;
		if (R_ViewFBO() == 3) textype = TEXTYPE_COLORBUFFER32F;
		// EDR forces the float scene buffer (Phase 8): an 8-bit scene has no
		// range to extend, and requiring the player to also know about
		// r_viewfbo made the feature three cvars instead of one. Never
		// DOWNGRADES -- r_viewfbo 3 keeps its 32F.
		if (textype == TEXTYPE_COLORBUFFER && R_EDR_Wanted()) textype = TEXTYPE_COLORBUFFER16F;
		// ...and so does the TEMPORAL scaler (2026-08-19): on an 8-bit scene
		// buffer it accumulates but does not reconstruct -- sharpness sat at
		// bilinear's for a whole session until the float buffer was found to
		// be the limiter (93% of native gradient energy with it). The menu and
		// the tier table may put a player on r_metalfx 2 without EDR, so the
		// prerequisite is forced here rather than documented. The HDR
		// shoulder's own gate is NOT widened by this: that would be a look
		// change, and this is a precision one.
		if (textype == TEXTYPE_COLORBUFFER && R_MetalFX_TemporalWanted()) textype = TEXTYPE_COLORBUFFER16F;
		break;
	case RENDERPATH_GL32:
		r_fb.usedepthtextures = r_usedepthtextures.integer != 0;
		if (R_ViewFBO() == 2) textype = TEXTYPE_COLORBUFFER16F;
		if (R_ViewFBO() == 3) textype = TEXTYPE_COLORBUFFER32F;
		break;
	case RENDERPATH_GLES2:
		r_fb.usedepthtextures = false;
		break;
	}

	if (r_viewscale_fpsscaling.integer)
	{
		double actualframetime;
		double targetframetime;
		double adjust;
		actualframetime = r_refdef.lastdrawscreentime;
		targetframetime = (1.0 / r_viewscale_fpsscaling_target.value);
		adjust = (targetframetime - actualframetime) * r_viewscale_fpsscaling_multiply.value;
		adjust = bound(-r_viewscale_fpsscaling_stepmax.value, adjust, r_viewscale_fpsscaling_stepmax.value);
		if (r_viewscale_fpsscaling_stepsize.value > 0)
		{
			if (adjust > 0)
				adjust = floor(adjust / r_viewscale_fpsscaling_stepsize.value) * r_viewscale_fpsscaling_stepsize.value;
			else
				adjust = ceil(adjust / r_viewscale_fpsscaling_stepsize.value) * r_viewscale_fpsscaling_stepsize.value;
		}
		viewscalefpsadjusted += adjust;
		viewscalefpsadjusted = bound(r_viewscale_fpsscaling_min.value, viewscalefpsadjusted, 1.0f);
	}
	else
		viewscalefpsadjusted = 1.0f;

	scale = r_viewscale.value * sqrt(viewscalefpsadjusted);
	if (vid.mode.samples)
		scale *= sqrt(vid.mode.samples); // supersampling
	scale = bound(0.03125f, scale, 4.0f);
	screentexturewidth = (int)ceil(r_refdef.view.width * scale);
	screentextureheight = (int)ceil(r_refdef.view.height * scale);
	screentexturewidth = bound(1, screentexturewidth, (int)vid.maxtexturesize_2d);
	screentextureheight = bound(1, screentextureheight, (int)vid.maxtexturesize_2d);

	// set bloomwidth and bloomheight to the bloom resolution that will be
	// used (often less than the screen resolution for faster rendering)
	if (r_bloom_m5.integer)
	{
		// BEAUTY A1: the modern chain's level 0 is HALF the scene buffer, whatever
		// r_bloom_resolution says -- the threshold wants the scene's own detail
		// and the chain makes its own smaller levels below this one.
		r_fb.bloomwidth = max(1, screentexturewidth / 2);
		r_fb.bloomheight = max(1, screentextureheight / 2);
	}
	else
	{
		r_fb.bloomheight = bound(1, r_bloom_resolution.value * 0.75f, screentextureheight);
		r_fb.bloomwidth = r_fb.bloomheight * screentexturewidth / screentextureheight;
	}
	r_fb.bloomwidth = bound(1, r_fb.bloomwidth, screentexturewidth);
	r_fb.bloomwidth = bound(1, r_fb.bloomwidth, (int)vid.maxtexturesize_2d);
	r_fb.bloomheight = bound(1, r_fb.bloomheight, (int)vid.maxtexturesize_2d);

	if ((R_Bloom_Wanted() || (!R_Stereo_Active() && (r_motionblur.value > 0 || r_damageblur.value > 0))) && ((r_bloom_resolution.integer < 4 || r_bloom_blur.value < 1 || r_bloom_blur.value >= 512) || r_refdef.view.width > (int)vid.maxtexturesize_2d || r_refdef.view.height > (int)vid.maxtexturesize_2d))
	{
		Cvar_SetValueQuick(&r_bloom, 0);
		Cvar_SetValueQuick(&r_motionblur, 0);
		Cvar_SetValueQuick(&r_damageblur, 0);
	}
	if (!R_Bloom_Wanted())
		r_fb.bloomwidth = r_fb.bloomheight = 0;

	// allocate motionblur ghost texture if needed - this is the only persistent texture and is only useful on the main view
	if (r_refdef.view.ismain && (r_fb.screentexturewidth != screentexturewidth || r_fb.screentextureheight != screentextureheight || r_fb.textype != textype))
	{
		if (r_fb.ghosttexture)
			R_FreeTexture(r_fb.ghosttexture);
		r_fb.ghosttexture = NULL;
		if (r_fb.waterscreen)   // WATERSURFACE: re-made at the new size below
			R_FreeTexture(r_fb.waterscreen);
		r_fb.waterscreen = NULL;
		r_fb.waterscreen_valid = false;

		r_fb.screentexturewidth = screentexturewidth;
		r_fb.screentextureheight = screentextureheight;
		r_fb.textype = textype;

		if (r_fb.screentexturewidth && r_fb.screentextureheight)
		{
			if (r_motionblur.value > 0 || r_damageblur.value > 0)
				r_fb.ghosttexture = R_LoadTexture2D(r_main_texturepool, "framebuffermotionblur", r_fb.screentexturewidth, r_fb.screentextureheight, NULL, r_fb.textype, TEXF_RENDERTARGET | TEXF_FORCELINEAR | TEXF_CLAMP, -1, NULL);
			r_fb.ghosttexture_valid = false;
		}
	}
	// WATERSURFACE: the frame copy the liquid surface refracts. Screen-sized and
	// of the screen target's own type, like the ghost texture; made the first
	// frame the cvar is on and released the first frame it is off, so a player
	// who never switches it on never pays the memory.
	if (r_refdef.view.ismain && r_fb.screentexturewidth && r_fb.screentextureheight)
	{
		if ((r_watersurface.integer || R_PartRefract_Wanted()) && !r_fb.waterscreen)   // BEAUTY A5: the refract particles share the copy
			r_fb.waterscreen = R_LoadTexture2D(r_main_texturepool, "framebufferwatersurface", r_fb.screentexturewidth, r_fb.screentextureheight, NULL, r_fb.textype, TEXF_RENDERTARGET | TEXF_FORCELINEAR | TEXF_CLAMP, -1, NULL);
		else if (!r_watersurface.integer && !R_PartRefract_Wanted() && r_fb.waterscreen)
		{
			R_FreeTexture(r_fb.waterscreen);
			r_fb.waterscreen = NULL;
		}
		if (!r_fb.waterscreen)
			r_fb.waterscreen_valid = false;
	}

	// Volumetrics needs to READ the camera's scene depth, so ask for depth as a
	// sampleable texture rather than a write-only renderbuffer. Keep the packed
	// 24-bit depth + 8-bit stencil format either way: DarkPlaces uses stencil for
	// shadow volumes and the GL context requests stencil 8. This deliberately
	// overrides r_usedepthtextures 0 — volumetrics cannot work without it. With
	// r_volumetric 0 the argument is 'true' exactly as before, so the render path
	// is unchanged. R_MetalFX_TemporalWanted joins it for the same reason: the
	// temporal scaler takes scene depth as an input, and so does the
	// motion-vector pass that feeds it.
	r_fb.rt_screen = R_RenderTarget_Get(screentexturewidth, screentextureheight, TEXTYPE_DEPTHBUFFER24STENCIL8, !(r_volumetric.integer || R_LightningM5_Wanted() || R_LavaShimmer_Wanted() || R_MetalFX_TemporalWanted() || R_RTTermUpsample_Wanted() || R_WaterSurface_Wanted() || R_SoftParticles_Wanted()), textype, TEXTYPE_UNUSED, TEXTYPE_UNUSED, TEXTYPE_UNUSED);

	r_refdef.view.clear = true;
}

// BEAUTY A1 (2026-09-16): one pass of the modern bloom chain -- a fullscreen quad
// through MODE_BLOOMBLUR reading src into dst. kind selects the shader's arm
// (LOCKSTEP shader_glsl.h / shader_msl.h MODE_BLOOMBLUR): 1 the prefilter (soft
// threshold + Karis weighting, p1 threshold, p2 knee, p3 intensity), 2 the 13-tap
// downsample, 3 the 9-tap tent upsample, which is drawn ADDITIVELY onto a level
// that already holds its own downsampled content. The state calls are the old
// chain's exactly (cull off, depth test off, the blend), because those are the
// ones proven on both backends; only the viewport follows the destination.
static void R_Bloom_M5_Pass(r_rendertarget_t *src, r_rendertarget_t *dst, int kind, float p1, float p2, float p3, qbool additive)
{
	r_viewport_t vp;
	R_Viewport_InitOrtho(&vp, &identitymatrix, 0, 0, dst->texturewidth, dst->textureheight, 0, 0, 1, 1, -10, 100, NULL);
	R_Mesh_SetRenderTargets(dst->fbo);
	R_SetViewport(&vp);
	GL_CullFace(GL_NONE);
	GL_DepthTest(false);
	GL_BlendFunc(GL_ONE, additive ? GL_ONE : GL_ZERO);
	GL_Color(1, 1, 1, 1);
	R_Mesh_PrepareVertices_Generic_Arrays(4, r_screenvertex3f, NULL, src->texcoord2f);
	R_SetupShader_SetPermutationGLSL(SHADERMODE_BLOOMBLUR, 0);
	if (r_glsl_permutation->tex_Texture_First      >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_First, src->colortexture[0]);
	if (r_glsl_permutation->loc_BloomBlur_Parameters >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_BloomBlur_Parameters, (float)kind, p1, p2, p3);
	// one texel of the SOURCE: the taps are in source texels on every arm
	if (r_glsl_permutation->loc_PixelSize          >= 0) R_Shader_Uniform2f(r_glsl_permutation->loc_PixelSize, 1.0f / (float)src->texturewidth, 1.0f / (float)src->textureheight);
	R_Mesh_Draw(0, 4, 0, 2, polygonelement3i, NULL, 0, polygonelement3s, NULL, 0);
	r_refdef.stats[r_stat_bloom_drawpixels] += dst->texturewidth * dst->textureheight;
}

// BEAUTY A1: the chain. Level 0 is the prefiltered scene at half resolution
// (R_Bloom_StartFrame sized r_fb.bloomwidth/height for it); each level below
// is half the one above through the 13-tap box; then from the smallest up, each
// level is tent-upsampled and ADDED onto the one above, so level 0 ends up the
// sum of every scale and is handed to the composite as r_fb.rt_bloom. The
// intensity is folded into the prefilter (every pass is linear, so it is the
// same number wherever it multiplies) and the composite's subtract is forced to
// 0 by R_BlendView, so the composite adds exactly this sum. Pooled targets: the
// pool refuses to hand out a target already used this frame, so every level is
// its own texture whatever the sizes, and an additive draw back onto a level
// re-opens that target with its content kept (fbo 0's Load reopen, the RT
// composite's precedent).
static void R_Bloom_M5_MakeTexture(void)
{
	r_rendertarget_t *lvl[8];
	int n = bound(1, r_bloom_m5_levels.integer, 8), i, w, h;
	textype_t textype = r_fb.rt_screen->colortextype[0];
	float threshold = max(0.0f, r_bloom_m5_threshold.value);
	float knee = bound(0.0f, r_bloom_m5_knee.value, threshold);
	float intensity = max(0.0f, r_bloom_m5_intensity.value);
	static int bloomregion = -1;
	static int lastn = -1, lastw = -1, lasth = -1;

	r_refdef.stats[r_stat_bloom]++;
	// METAL_FRAMEMS=2 times the whole chain on the GPU (the cost instrument; a
	// no-op otherwise, and on GL always)
	if (bloomregion < 0) { const char *e = getenv("METAL_FRAMEMS"); bloomregion = (e && atoi(e) == 2) ? 1 : 0; }
	if (bloomregion) Metal_Backend_ProfileRegion(1, "bloom chain");

	w = r_fb.bloomwidth;
	h = r_fb.bloomheight;
	lvl[0] = R_RenderTarget_Get(w, h, TEXTYPE_UNUSED, false, textype, TEXTYPE_UNUSED, TEXTYPE_UNUSED, TEXTYPE_UNUSED);
	R_Bloom_M5_Pass(r_fb.rt_screen, lvl[0], 1, threshold, knee, intensity, false);
	for (i = 1; i < n; i++)
	{
		w = max(1, w / 2);
		h = max(1, h / 2);
		if (w < 2 || h < 2)
		{
			n = i;   // a level smaller than 2x2 has nothing left to blur
			break;
		}
		lvl[i] = R_RenderTarget_Get(w, h, TEXTYPE_UNUSED, false, textype, TEXTYPE_UNUSED, TEXTYPE_UNUSED, TEXTYPE_UNUSED);
		R_Bloom_M5_Pass(lvl[i - 1], lvl[i], 2, 0, 0, 0, false);
	}
	for (i = n - 2; i >= 0; i--)
		R_Bloom_M5_Pass(lvl[i + 1], lvl[i], 3, 0, 0, 0, true);
	r_fb.rt_bloom = lvl[0];

	if (bloomregion) Metal_Backend_ProfileRegion(0, "bloom chain");

	// change-only: the compile line is the shader's evidence, this is the chain's
	if (n != lastn || r_fb.bloomwidth != lastw || r_fb.bloomheight != lasth)
	{
		lastn = n; lastw = r_fb.bloomwidth; lasth = r_fb.bloomheight;
		Con_DPrintf("M5 bloom: chain armed (%d levels from %dx%d)\n", n, r_fb.bloomwidth, r_fb.bloomheight);
	}
}

static void R_Bloom_MakeTexture(void)
{
	int x, range, dir;
	float xoffset, yoffset, r, brighten;
	float colorscale = r_bloom_colorscale.value;
	r_viewport_t bloomviewport;
	r_rendertarget_t *prev, *cur;
	textype_t textype = r_fb.rt_screen->colortextype[0];

	// BEAUTY A1: the modern chain takes over here; everything below is the
	// 2001 blur, untouched
	if (r_bloom_m5.integer)
	{
		R_Bloom_M5_MakeTexture();
		return;
	}

	r_refdef.stats[r_stat_bloom]++;
	// METAL_FRAMEMS=2 times this chain too, under the same label as the modern
	// one, so a toggle in one boot reads the two like for like (a no-op otherwise)
	{
		static int oldregion = -1;
		if (oldregion < 0) { const char *e = getenv("METAL_FRAMEMS"); oldregion = (e && atoi(e) == 2) ? 1 : 0; }
		if (oldregion) Metal_Backend_ProfileRegion(1, "bloom chain");
	}

	R_Viewport_InitOrtho(&bloomviewport, &identitymatrix, 0, 0, r_fb.bloomwidth, r_fb.bloomheight, 0, 0, 1, 1, -10, 100, NULL);

	// scale down screen texture to the bloom texture size
	CHECKGLERROR
	prev = r_fb.rt_screen;
	cur = R_RenderTarget_Get(r_fb.bloomwidth, r_fb.bloomheight, TEXTYPE_UNUSED, false, textype, TEXTYPE_UNUSED, TEXTYPE_UNUSED, TEXTYPE_UNUSED);
	R_Mesh_SetRenderTargets(cur->fbo);
	R_SetViewport(&bloomviewport);
	GL_CullFace(GL_NONE);
	GL_DepthTest(false);
	GL_BlendFunc(GL_ONE, GL_ZERO);
	GL_Color(colorscale, colorscale, colorscale, 1);
	R_Mesh_PrepareVertices_Generic_Arrays(4, r_screenvertex3f, NULL, prev->texcoord2f);
	// TODO: do boxfilter scale-down in shader?
	R_SetupShader_Generic(prev->colortexture[0], false, true, true);
	R_Mesh_Draw(0, 4, 0, 2, polygonelement3i, NULL, 0, polygonelement3s, NULL, 0);
	r_refdef.stats[r_stat_bloom_drawpixels] += r_fb.bloomwidth * r_fb.bloomheight;
	// we now have a properly scaled bloom image

	// multiply bloom image by itself as many times as desired to darken it
	// TODO: if people actually use this it could be done more quickly in the previous shader pass
	for (x = 1;x < min(r_bloom_colorexponent.value, 32);)
	{
		prev = cur;
		cur = R_RenderTarget_Get(r_fb.bloomwidth, r_fb.bloomheight, TEXTYPE_UNUSED, false, textype, TEXTYPE_UNUSED, TEXTYPE_UNUSED, TEXTYPE_UNUSED);
		R_Mesh_SetRenderTargets(cur->fbo);
		x *= 2;
		r = bound(0, r_bloom_colorexponent.value / x, 1); // always 0.5 to 1
		if(x <= 2)
			GL_Clear(GL_COLOR_BUFFER_BIT, NULL, 1.0f, 0);
		GL_BlendFunc(GL_SRC_COLOR, GL_ZERO); // square it
		GL_Color(1,1,1,1); // no fix factor supported here
		R_Mesh_PrepareVertices_Generic_Arrays(4, r_screenvertex3f, NULL, prev->texcoord2f);
		R_SetupShader_Generic(prev->colortexture[0], false, true, false);
		R_Mesh_Draw(0, 4, 0, 2, polygonelement3i, NULL, 0, polygonelement3s, NULL, 0);
		r_refdef.stats[r_stat_bloom_drawpixels] += r_fb.bloomwidth * r_fb.bloomheight;
	}
	CHECKGLERROR

	range = r_bloom_blur.integer * r_fb.bloomwidth / 320;
	brighten = r_bloom_brighten.value;
	brighten = sqrt(brighten);
	if(range >= 1)
		brighten *= (3 * range) / (2 * range - 1); // compensate for the "dot particle"

	for (dir = 0;dir < 2;dir++)
	{
		prev = cur;
		cur = R_RenderTarget_Get(r_fb.bloomwidth, r_fb.bloomheight, TEXTYPE_UNUSED, false, textype, TEXTYPE_UNUSED, TEXTYPE_UNUSED, TEXTYPE_UNUSED);
		R_Mesh_SetRenderTargets(cur->fbo);
		// blend on at multiple vertical offsets to achieve a vertical blur
		// TODO: do offset blends using GLSL
		// TODO instead of changing the texcoords, change the target positions to prevent artifacts at edges
		CHECKGLERROR
		GL_BlendFunc(GL_ONE, GL_ZERO);
		CHECKGLERROR
		R_SetupShader_Generic(prev->colortexture[0], false, true, false);
		CHECKGLERROR
		for (x = -range;x <= range;x++)
		{
			if (!dir){xoffset = 0;yoffset = x;}
			else {xoffset = x;yoffset = 0;}
			xoffset /= (float)prev->texturewidth;
			yoffset /= (float)prev->textureheight;
			// compute a texcoord array with the specified x and y offset
			r_fb.offsettexcoord2f[0] = xoffset+prev->texcoord2f[0];
			r_fb.offsettexcoord2f[1] = yoffset+prev->texcoord2f[1];
			r_fb.offsettexcoord2f[2] = xoffset+prev->texcoord2f[2];
			r_fb.offsettexcoord2f[3] = yoffset+prev->texcoord2f[3];
			r_fb.offsettexcoord2f[4] = xoffset+prev->texcoord2f[4];
			r_fb.offsettexcoord2f[5] = yoffset+prev->texcoord2f[5];
			r_fb.offsettexcoord2f[6] = xoffset+prev->texcoord2f[6];
			r_fb.offsettexcoord2f[7] = yoffset+prev->texcoord2f[7];
			// this r value looks like a 'dot' particle, fading sharply to
			// black at the edges
			// (probably not realistic but looks good enough)
			//r = ((range*range+1)/((float)(x*x+1)))/(range*2+1);
			//r = brighten/(range*2+1);
			r = brighten / (range * 2 + 1);
			if(range >= 1)
				r *= (1 - x*x/(float)((range+1)*(range+1)));
			if (r <= 0)
				continue;
			CHECKGLERROR
			GL_Color(r, r, r, 1);
			CHECKGLERROR
			R_Mesh_PrepareVertices_Generic_Arrays(4, r_screenvertex3f, NULL, r_fb.offsettexcoord2f);
			CHECKGLERROR
			R_Mesh_Draw(0, 4, 0, 2, polygonelement3i, NULL, 0, polygonelement3s, NULL, 0);
			r_refdef.stats[r_stat_bloom_drawpixels] += r_fb.bloomwidth * r_fb.bloomheight;
			CHECKGLERROR
			GL_BlendFunc(GL_ONE, GL_ONE);
			CHECKGLERROR
		}
	}

	// now we have the bloom image, so keep track of it
	r_fb.rt_bloom = cur;
	{
		static int oldregion2 = -1;
		if (oldregion2 < 0) { const char *e = getenv("METAL_FRAMEMS"); oldregion2 = (e && atoi(e) == 2) ? 1 : 0; }
		if (oldregion2) Metal_Backend_ProfileRegion(0, "bloom chain");
	}
}

static qbool R_BlendView_IsTrivial(int viewwidth, int viewheight, int width, int height)
{
	// Scaling requested?
	if (viewwidth != width || viewheight != height)
		return false;
	// Higher bit depth or explicit FBO requested?
	if (R_ViewFBO())
		return false;
	// Non-trivial postprocessing shader permutation?
	if (r_fb.bloomwidth
	|| r_refdef.viewblend[3] > 0
	|| !vid_gammatables_trivial
	|| r_glsl_postprocess.integer
	|| ((!R_Stereo_ColorMasking() && r_glsl_saturation.value != 1)))
		return false;
	// Other reasons for a non-trivial default postprocessing shader?
	// (See R_CompileShader_CheckStaticParms but only those relevant for MODE_POSTPROCESS in shader_glsl.h)
	// Skip: if (r_glsl_saturation_redcompensate.integer) (already covered by saturation above).
	// Skip: if (r_glsl_postprocess.integer) (already covered by r_glsl_postprocess above).
	// Skip: if (r_glsl_postprocess_uservec1_enable.integer) (already covered by r_glsl_postprocessing above).
	if (r_fxaa.integer && !m5_stock.integer)
		return false;
	if (r_colorfringe.value && !m5_stock.integer)
		return false;
	// Volumetrics reads the scene depth back, which only exists as a texture on the
	// offscreen scene target — the direct-to-window path has no sampleable depth at
	// all. So force the offscreen path. (r_bloom achieves the same thing indirectly
	// via r_fb.bloomwidth above; this follows the r_viewfbo / r_fxaa / r_colorfringe
	// pattern of testing the cvar inline.)
	if (r_volumetric.integer)
		return false;

	// F1's bolt pass reads the scene depth back for occlusion, exactly as the
	// murk does, so it needs the same offscreen path for the same reason.
	if (R_LightningM5_Wanted())
		return false;

	// the heat shimmer reads scene depth back, exactly as the murk above does
	if (R_LavaShimmer_Wanted())
		return false;
	// WATERSURFACE: the refraction guard samples the scene depth, so the feature
	// forces the offscreen path like the murk does -- see R_WaterSurface_Wanted.
	if (R_WaterSurface_Wanted())
		return false;
	// BEAUTY A4: soft particles compare against the scene depth, so they ask
	// for the offscreen path exactly as the water and the murk do.
	if (R_SoftParticles_Wanted())
		return false;
	// EDR renders into the forced float scene buffer, so the direct-to-window
	// path cannot carry it -- same shape as the r_volumetric clause above. At
	// r_viewfbo >= 1 the earlier bail already fired; this covers r_viewfbo 0
	// with r_edr on.
	if (R_EDR_Wanted())
		return false;
	// Temporal upscaling reads the render-res scene colour AND its depth; the
	// direct-to-window path has neither. Same shape as the r_volumetric clause.
	if (R_MetalFX_TemporalWanted())
		return false;
	return true;
}

// METAL.md Phase 8-5: the MetalFX gate. Decides, per frame, whether the
// postprocess should write a render-res pooled intermediate for the spatial
// scaler to upscale into fbo 0, instead of blitting straight to the window
// through the fused bilinear draw. Returns the pooled target when every
// condition holds, NULL otherwise -- and NULL always means "take today's
// path", so any refusal degrades gracefully mid-session, per frame.
// Only called under !skipblend, so r_fb.rt_screen exists; guarded anyway.
static r_rendertarget_t *R_MetalFX_GetPostprocessTarget(int fbo, int x, int y, int width, int height)
{
	int inw, inh;
	textype_t textype;
	if (vid.renderpath != RENDERPATH_METAL || !r_metalfx.integer || !MetalFX_Available() || !r_fb.rt_screen)
		return NULL;
	// The destination must be the real window, whole: fbo 0 and full-frame.
	// The fbo/envmap terms are load-bearing -- envmap renders with ismain
	// true into an offscreen target and would otherwise blast the live
	// screen texture. A sub-frame view (viewsize < 100, r_letterbox, the
	// geometric stereo modes) is refused by the full-frame terms: the scaler
	// writes the WHOLE screen texture and a partial destination rect cannot
	// mean that.
	if (fbo != 0 || r_refdef.envmap || x != 0 || y != 0 || width != vid.mode.width || height != vid.mode.height)
		return NULL;
	// Stereo must be refused EXPLICITLY, not left to the full-frame terms:
	// the anaglyph modes are full-frame and render R_RenderView twice per
	// host frame under complementary colour masks that compose in fbo 0 --
	// the scaler has no colour mask and writes all four channels, so the
	// second eye's upscale would wipe the first eye's image and its HUD
	// (found by the 8-5 adversarial review; the geometric modes were already
	// refused, which is what made the original comment's claim plausible).
	if (R_Stereo_Active())
		return NULL;
	// r_rendertarget_debug resolves its source by pool index, and under this
	// gate the postprocess DESTINATION is a pool record -- the debug cvar
	// could select it as Texture_First while it is the colour attachment, a
	// read-write hazard neither backend defines. The debug view predates the
	// pool destination; refuse rather than interact.
	if (r_rendertarget_debug.integer >= 0)
		return NULL;
	inw = r_fb.rt_screen->texturewidth;
	inh = r_fb.rt_screen->textureheight;
	// The SPATIAL scaler only upscales: 1:1 would be a pointless copy and
	// downscale is not its job (r_viewscale > 1 supersampling keeps the
	// bilinear path). TEMPORAL at 1:1 is NOT pointless -- it is anti-aliasing
	// and fog denoising with no resolution loss, which is the configuration the
	// top tier wants -- and the device says it is allowed: the boot probe
	// reports an input-content scale range of 1.000-3.000 on this hardware, so
	// the floor here is the device's own answer rather than a guess.
	if (r_metalfx.integer >= 2 ? (inw > width || inh > height) : (inw >= width || inh >= height))
		return NULL;
	// fpsscaling moves the input size most frames -- refused in v1 rather
	// than re-minting a scaler inside the frame loop
	if (r_viewscale_fpsscaling.integer)
		return NULL;
	if (vid.mode.samples)
		return NULL;
	textype = r_fb.rt_screen->colortextype[0];
	// vid.edr_active is what formats mb_screentex this frame (the backend's
	// BeginFrame read the same value earlier in this same frame), so it is
	// the honest output-format selector; on the one frame of an EDR toggle
	// edge it can disagree with rt_screen's textype, which the scaler cache
	// handles as an ordinary mixed-format key
	if (r_metalfx.integer >= 2)
	{
		// Temporal needs a sampleable depth texture; the predicate forces one
		// via R_Bloom_StartFrame, but a frame where that force has not taken
		// effect yet (the cvar moved this frame) must refuse rather than
		// encode against a renderbuffer.
		if (!r_fb.rt_screen->depthtexture || !r_fb.scenedepthvalid)
			return NULL;
		// Two temporal accumulators in series would double-smear, and the
		// motion blur runs on the same render-res frame just before this.
		if (r_refdef.view.ismain && (r_motionblur.value > 0 || r_damageblur.value > 0))
			return NULL;
		if (!MetalFX_TemporalReady(inw, inh, width, height, textype, vid.edr_active))
			return NULL;
	}
	else if (!MetalFX_ScalerReady(inw, inh, width, height, textype, vid.edr_active))
		return NULL;
	// the R_Bloom_MakeTexture shape: colour only, no depth -- the postprocess
	// draw needs none, and the pool keys on the whole tuple so this can never
	// be handed a target another consumer holds this frame
	return R_RenderTarget_Get(inw, inh, TEXTYPE_UNUSED, false, textype, TEXTYPE_UNUSED, TEXTYPE_UNUSED, TEXTYPE_UNUSED);
}

// T2b: the per-entity pass's state lives beside the depth-only draw it hooks
// (far below); declared here because the walk that drives it is here.
static qbool r_motionpass_active;
static matrix4x4_t r_motionpass_prevmodel;
static float r_motionpass_debug;

// T2b: the side table of previous-frame model matrices. Rigid-body only.
static struct taa_prevent_s *R_TAA_PrevEnt(int entitynumber)
{
	if (entitynumber <= 0)
		return NULL;
	if (entitynumber >= r_fb.taa_prevent_max)
	{
		// grow to cl.max_entities once; entitynumber is bounded by it
		int want = max(cl.max_entities, entitynumber + 1);
		struct taa_prevent_s *n = (struct taa_prevent_s *)Mem_Alloc(r_main_mempool, want * sizeof(*n));
		if (!n)
			return NULL;
		if (r_fb.taa_prevent)
		{
			memcpy(n, r_fb.taa_prevent, r_fb.taa_prevent_max * sizeof(*n));
			Mem_Free(r_fb.taa_prevent);
		}
		r_fb.taa_prevent = n;
		r_fb.taa_prevent_max = want;
	}
	return &r_fb.taa_prevent[entitynumber];
}

// Is this an entity the per-entity pass should draw? The world is the fill's;
// the view weapon is T2a's; sprites are billboards with no depth; anything
// not a network entity has no previous frame to reproject from.
static qbool R_MotionVector_EntityWanted(const entity_render_t *ent)
{
	if (!ent->model || ent->model->type == mod_sprite)
		return false;
	if (ent == r_refdef.scene.worldentity)
		return false;
	if (ent->flags & (RENDER_VIEWMODEL | RENDER_EXTERIORMODEL))
		return false;
	if (ent->entitynumber <= 0)
		return false;
	return true;
}

// The MetalFX-TEMPORAL arc's motion-vector pass. Renders screen-space motion
// vectors, in pixels, into a pooled render-res target for the temporal scaler.
//
// ONE RENDER PASS, WITH NO DEPTH ATTACHMENT, and that is the load-bearing
// choice. The scene depth must be SAMPLED here (the vectors are reconstructed
// from it), and sampling a depth texture that is also attached to the bound
// framebuffer is a feedback loop -- render.h states the rule and the murk obeys
// it the same way. Leaving depth unattached also means the entity pass that
// draws over this one (the arc's next slice) must do its own depth test in the
// fragment shader against the same sampled texture, which is what lets it cope
// with the viewmodel's compressed depth range for free.
//
// The target is TEXTYPE_COLORBUFFER16F -- RGBA16Float -- deliberately, rather
// than a new two-channel TEXTYPE: mt_pixelformat is a place this tree has
// broken before, the pool already knows this type, and the boot probe measured
// the scaler accepting RGBA16Float for motion. Two channels are wasted; the
// buffer is 7 MB at 1280x720.
// ---------------------------------------------------------------------------
// r_fxaa_post: FXAA at NATIVE resolution, after the MetalFX upscale.
//
// The postprocess target is sized from r_fb.rt_screen -- the RENDER resolution
// -- so FXAA runs at 1280x720 (720x405 on Superfast) and the scaler then
// magnifies the antialiased frame. The staircase the eye sees at a roof
// against sky, or a near object against a far one, is created BY that
// magnification and FXAA never sees it: measured 2026-09-18, FXAA off is
// indistinguishable from FXAA on at exactly those edges.
//
// So the scaler is given a pooled NATIVE-resolution target instead of the
// screen texture, and this pass antialiases that into fbo 0. One extra
// fullscreen pass at the output resolution.
// SMAA.md slice 2: r_smaa drops into exactly this slot, so the target, the
// destination-format rule, the scaler's output-usage bits and the
// graceful-degradation retry are shared rather than rebuilt. Either filter
// wanting the pass is what allocates it.
static qbool R_SMAA_Wanted(void)
{
	return vid.renderpath == RENDERPATH_METAL && r_smaa.integer != 0 && r_metalfx.integer != 0;
}

static r_rendertarget_t *R_PostAA_Target(int width, int height)
{
	if (vid.renderpath != RENDERPATH_METAL || !r_metalfx.integer)
		return NULL;
	if (!r_fxaa_post.integer && !r_smaa.integer)
		return NULL;
	if (!r_fb.rt_screen)
		return NULL;
	// THE DESTINATION MUST CARRY THE SCALER'S OUTPUT FORMAT, WHICH IS THE SCREEN
	// TEXTURE'S -- not the scene buffer's. metal_fx.m refuses a destination whose
	// pixelFormat differs from mfx_scaler.outputTextureFormat, and the backend
	// picks that from vid.edr_active (RGBA16Float under EDR, BGRA8 otherwise)
	// while r_fb.rt_screen is the FLOAT scene buffer whenever r_viewfbo >= 2.
	// Using the scene buffer's format here refused every encode with EDR off,
	// which cost an hour and read as "the feature does nothing".
	return R_RenderTarget_Get(width, height, TEXTYPE_UNUSED, false,
	                          vid.edr_active ? TEXTYPE_COLORBUFFER16F : TEXTYPE_COLORBUFFER,
	                          TEXTYPE_UNUSED, TEXTYPE_UNUSED, TEXTYPE_UNUSED);
}

// Draw the upscaled frame through FXAA into the real destination. PixelSize is
// the NATIVE target's texel size, not the render-res one -- that is the entire
// point of the pass, and getting it wrong would antialias at the wrong scale
// exactly as silently as the render-res pass already does.
static void R_FXAAPost_Draw(r_rendertarget_t *src, int dstfbo, rtexture_t *dstcolor, int x, int y, int width, int height)
{
	R_ResetViewRendering2D(dstfbo, NULL, dstcolor, x, y, width, height);
	GL_DepthTest(false);
	GL_DepthMask(false);
	GL_BlendFunc(GL_ONE, GL_ZERO);
	GL_Color(1, 1, 1, 1);
	R_Mesh_PrepareVertices_Generic_Arrays(4, r_screenvertex3f, NULL, src->texcoord2f);
	R_SetupShader_SetPermutationGLSL(SHADERMODE_FXAAPOST, 0);
	if (r_glsl_permutation->tex_Texture_First >= 0)
		R_Mesh_TexBind(r_glsl_permutation->tex_Texture_First, src->colortexture[0]);
	if (r_glsl_permutation->loc_PixelSize >= 0)
		R_Shader_Uniform2f(r_glsl_permutation->loc_PixelSize, 1.0f / src->texturewidth, 1.0f / src->textureheight);
	if (r_glsl_permutation->loc_FxaaSpan >= 0)
		R_Shader_Uniform1f(r_glsl_permutation->loc_FxaaSpan, bound(4.0f, r_fxaa_post_span.value, 32.0f));
	R_Mesh_Draw(0, 4, 0, 2, polygonelement3i, NULL, 0, polygonelement3s, NULL, 0);
	{
		// CHANGE-ONLY, and the feature's only textual evidence: a pass that
		// silently did not run renders a perfectly correct frame that is merely
		// not post-antialiased, which is the invisible-failure class this tree
		// keeps paying for. Rate is one line per state change, never per frame.
		static int said = -1;
		int now = (width << 16) | (height & 0xFFFF);
		if (said != now)
		{
			said = now;
			Con_DPrintf("FXAA post-upscale armed (%dx%d, native)\n", width, height);
		}
	}
}

// ---------------------------------------------------------------------------
// r_smaa: analytic MLAA at NATIVE resolution, three passes, in the same slot.
//
// WHY A THIRD FILTER AND NOT A WIDER FXAA. dp_fxaa clamps its blend direction
// to maxspan and takes its farthest tap at dir*0.5, so it reaches span/2 and
// corrects at most half a pixel -- and only near a step's ENDS. Seb's jagged
// edge steps every 6-18 px, so four pixels in six get nothing, and the ground
// truth says raising the span past ~24 makes BOTH slopes worse because the taps
// land on unrelated content. That is structural, not tuning. A morphological
// pass finds each step's whole extent along the edge and blends every pixel in
// it by its own coverage, which is the ramp the geometry actually wants.
//
// The three passes and the geometry are LOCKSTEP with test/aaedge.py's mlaa(),
// which is the ground-truth validator they were written against and the only
// place the algorithm is checked against a known right answer.
static void R_SMAA_Pass(r_rendertarget_t *src, rtexture_t *second, int dstfbo, rtexture_t *dstcolor,
                        int x, int y, int width, int height, int mode, float debug)
{
	const r_glsl_permutation_t *pp;
	int loc;

	R_ResetViewRendering2D(dstfbo, NULL, dstcolor, x, y, width, height);
	GL_DepthTest(false);
	GL_DepthMask(false);
	GL_BlendFunc(GL_ONE, GL_ZERO);
	GL_Color(1, 1, 1, 1);
	R_Mesh_PrepareVertices_Generic_Arrays(4, r_screenvertex3f, NULL, src->texcoord2f);
	R_SetupShader_SetPermutationGLSL(mode, 0);
	pp = r_glsl_permutation;
	// EVERY UNIT THE ARM DECLARES IS BOUND. On Metal a unit the shader declares
	// but nothing binds holds whatever the last shader left there -- three
	// shipped defects in this tree are that class -- so the blend pass's second
	// sampler is bound unconditionally, with the source standing in when there
	// is no weights texture to give it.
	if (pp->tex_Texture_First >= 0)
		R_Mesh_TexBind(pp->tex_Texture_First, src->colortexture[0]);
	if (pp->tex_Texture_Second >= 0)
		R_Mesh_TexBind(pp->tex_Texture_Second, second ? second : src->colortexture[0]);
	// PixelSize is one texel of the NATIVE target. All three passes run at the
	// output resolution -- that is the whole point of the round, and the render-
	// res value would antialias at the wrong scale exactly as silently as the
	// postprocess's own FXAA already does.
	if (pp->loc_PixelSize >= 0)
		R_Shader_Uniform2f(pp->loc_PixelSize, 1.0f / src->texturewidth, 1.0f / src->textureheight);
	// By NAME, not a cached permutation field: three modes nothing else uses,
	// one draw each per frame, so the lookup cost is irrelevant and no field is
	// spent (the MOTIONFILL / RTCOMPOSITE shape).
	if ((loc = R_Shader_GetUniformLocation(pp, "SmaaParams")) >= 0)
		R_Shader_Uniform4f(loc, max(0.001f, r_smaa_threshold.value),
		                        (float)(int)bound(4.0f, r_smaa_search.value, 32.0f),
		                        debug, max(1.0f, r_smaa_adapt.value));
	R_Mesh_Draw(0, 4, 0, 2, polygonelement3i, NULL, 0, polygonelement3s, NULL, 0);
}

static void R_SMAA_Draw(r_rendertarget_t *src, int dstfbo, rtexture_t *dstcolor, int x, int y, int width, int height)
{
	r_rendertarget_t *rt_e, *rt_w;
	int dbg = r_smaa_debug.integer;

	// Two intermediates at native size, both ordinary TEXTYPE_COLORBUFFER: no
	// new textype (mt_pixelformat is a place this tree has broken before), and
	// the pool refuses to hand the same slot out twice in one frame, so these
	// are distinct targets by construction rather than by luck. 8 bits is ample
	// for both -- the edges are flags, and a weight is a coverage in [0,0.5],
	// so a level is 1/255 of a pixel.
	rt_e = R_RenderTarget_Get(src->texturewidth, src->textureheight, TEXTYPE_UNUSED, false, TEXTYPE_COLORBUFFER, TEXTYPE_UNUSED, TEXTYPE_UNUSED, TEXTYPE_UNUSED);
	rt_w = rt_e ? R_RenderTarget_Get(src->texturewidth, src->textureheight, TEXTYPE_UNUSED, false, TEXTYPE_COLORBUFFER, TEXTYPE_UNUSED, TEXTYPE_UNUSED, TEXTYPE_UNUSED) : NULL;
	if (!rt_e || !rt_w || rt_e == rt_w)
	{
		// The pass must never cost more than itself: an intermediate the pool
		// cannot supply degrades to the FXAA it replaces, never to an unfiltered
		// frame and never to a failed encode (the r_fxaa_post lesson, where a
		// refused destination dropped three tiers to no upscaling at all --
		// silently, and FASTER, which is the worst shape a failure can take).
		static qbool said;
		if (!said) { said = true; Con_Printf(CON_WARN "r_smaa: no intermediate render target available; falling back to r_fxaa_post this session\n"); }
		R_FXAAPost_Draw(src, dstfbo, dstcolor, x, y, width, height);
		return;
	}
	R_SMAA_Pass(src, NULL, rt_e->fbo, rt_e->colortexture[0], 0, 0, rt_e->texturewidth, rt_e->textureheight, SHADERMODE_SMAAEDGES, 0.0f);
	if (dbg == 1)
	{
		// the edges, straight to the destination -- the weights pass would
		// overwrite them and there is nothing else that can show pass 1 alone
		R_ResetViewRendering2D(dstfbo, NULL, dstcolor, x, y, width, height);
		GL_DepthTest(false); GL_DepthMask(false); GL_BlendFunc(GL_ONE, GL_ZERO); GL_Color(1, 1, 1, 1);
		R_Mesh_PrepareVertices_Generic_Arrays(4, r_screenvertex3f, NULL, rt_e->texcoord2f);
		R_SetupShader_Generic(rt_e->colortexture[0], false, true, true);
		R_Mesh_Draw(0, 4, 0, 2, polygonelement3i, NULL, 0, polygonelement3s, NULL, 0);
	}
	else
	{
		R_SMAA_Pass(rt_e, NULL, rt_w->fbo, rt_w->colortexture[0], 0, 0, rt_w->texturewidth, rt_w->textureheight, SHADERMODE_SMAAWEIGHTS, 0.0f);
		R_SMAA_Pass(src, rt_w->colortexture[0], dstfbo, dstcolor, x, y, width, height, SHADERMODE_SMAABLEND, dbg >= 2 ? 1.0f : 0.0f);
	}
	{
		// CHANGE-ONLY, and the pass's only textual evidence: a filter that
		// silently did not run renders a perfectly correct frame that is merely
		// not antialiased. One line per state change, never per frame (the
		// 92 GB console-ink incident).
		// THE KEY MUST CARRY EVERY FIELD THE LINE REPORTS. The first cut keyed
		// on the size and the debug flag alone while printing the search reach
		// too, so changing the reach printed nothing and smoke's check for it
		// failed against a feature that was working -- a change-only line that
		// cannot see one of its own values is a liveness claim with a hole in
		// it. 12 bits each for the size (4095 covers 3840x2160), 6 for the
		// reach (clamped to 32), one for the debug flag.
		static int said = -1;
		int search = (int)bound(4.0f, r_smaa_search.value, 32.0f);
		int now = (width & 0xFFF) | ((height & 0xFFF) << 12) | ((search & 0x3F) << 24) | (dbg ? (1 << 30) : 0);
		if (said != now)
		{
			said = now;
			Con_DPrintf("SMAA armed (%dx%d, native, search %d%s)\n", width, height,
			            search, dbg ? ", DEBUG VIEW" : "");
		}
	}
}

// The one place that decides which post-upscale filter runs. SMAA.md trap 8:
// Seb's config archives r_fxaa_post 1, so two edge filters would otherwise
// stack into a softer frame for no gain. SMAA wins, and says so once.
static void R_PostAA_Draw(r_rendertarget_t *src, int dstfbo, rtexture_t *dstcolor, int x, int y, int width, int height)
{
	if (R_SMAA_Wanted())
	{
		if (r_fxaa_post.integer)
		{
			static qbool said;
			if (!said) { said = true; Con_DPrintf("r_smaa supersedes r_fxaa_post; the FXAA post pass is not run\n"); }
		}
		R_SMAA_Draw(src, dstfbo, dstcolor, x, y, width, height);
	}
	else
		R_FXAAPost_Draw(src, dstfbo, dstcolor, x, y, width, height);
}

static float r_reactive_gain;   // >0 while R_MotionVector_Draw is drawing the reactive mask instead of vectors

static void R_MotionVector_Draw(rtexture_t *viewdepthtexture, int dstfbo, rtexture_t *dstcolor, int viewwidth, int viewheight, float debugscale)
{
	float m16f[16];
	int loc;

	R_ResetViewRendering2D(dstfbo, NULL, dstcolor, 0, 0, viewwidth, viewheight);
	GL_DepthTest(false);
	GL_DepthMask(false);
	GL_BlendFunc(GL_ONE, GL_ZERO);
	GL_Color(1, 1, 1, 1);
	R_Mesh_PrepareVertices_Generic_Arrays(4, r_screenvertex3f, NULL, r_fb.rt_screen->texcoord2f);
	R_SetupShader_SetPermutationGLSL(SHADERMODE_MOTIONFILL, 0);
	if (r_glsl_permutation->tex_Texture_ScreenDepth >= 0)
		R_Mesh_TexBind(r_glsl_permutation->tex_Texture_ScreenDepth, viewdepthtexture);
	// The forward half of the reconstruction is the murk's own published block,
	// by the same uniform names, so these are the cached loc_ fields already.
	if (r_glsl_permutation->loc_ViewToWorld       >= 0) { Matrix4x4_ToArrayFloatGL(&r_fb.scenedepth_viewtoworld, m16f); R_Shader_UniformMatrix4fv(r_glsl_permutation->loc_ViewToWorld, 1, false, m16f); }
	if (r_glsl_permutation->loc_FrustumScale      >= 0) R_Shader_Uniform2f(r_glsl_permutation->loc_FrustumScale, r_fb.scenedepth_frustum_x, r_fb.scenedepth_frustum_y);
	if (r_glsl_permutation->loc_ScreenToDepth     >= 0) R_Shader_Uniform2f(r_glsl_permutation->loc_ScreenToDepth, r_fb.scenedepth_screentodepth[0], r_fb.scenedepth_screentodepth[1]);
	if (r_glsl_permutation->loc_VolumetricFarClip >= 0) R_Shader_Uniform1f(r_glsl_permutation->loc_VolumetricFarClip, r_fb.scenedepth_farclip);
	{
		// The backward half, by name -- one draw per frame, so the lookup cost
		// is irrelevant and no permutation fields are spent on a mode nothing
		// else uses (the R_SetupShader_RTComposite shape).
		const r_glsl_permutation_t *pp = r_glsl_permutation;
		float sx = (r_metalfx_signs.integer & 4) ? -1.0f : 1.0f;
		float sy = (r_metalfx_signs.integer & 8) ? -1.0f : 1.0f;
		Matrix4x4_ToArrayFloatGL(&r_fb.taa_prevview, m16f);
		if ((loc = R_Shader_GetUniformLocation(pp, "MotionPrevView")) >= 0) R_Shader_UniformMatrix4fv(loc, 1, false, m16f);
		if ((loc = R_Shader_GetUniformLocation(pp, "MotionParams")) >= 0)
			R_Shader_Uniform4f(loc, r_fb.taa_prevfrustum[0], r_fb.taa_prevfrustum[1],
			                        sx * (float)viewwidth, sy * (float)viewheight);
		if ((loc = R_Shader_GetUniformLocation(pp, "MotionDebug")) >= 0) R_Shader_Uniform1f(loc, debugscale);
		{
			// The reactive mask's light list: the first 8 DYNAMIC lights --
			// cl_main.c links cl.dlights into r_refdef.scene.lights ahead of the
			// static ones, and isdynamic distinguishes them. Same origin
			// convention as the sidecar's own gather: matrix_lighttoworld's
			// translation.
			float lights[8 * 4];
			int nl = 0, li;
			if (r_reactive_gain > 0.0f)
			{
				for (li = 0; li < r_refdef.scene.numlights && nl < 8; li++)
				{
					const rtlight_t *rtl = r_refdef.scene.lights[li];
					vec3_t org;
					if (!rtl || rtl->radius <= 0.0f || rtl->isstatic)
						continue;
					Matrix4x4_OriginFromMatrix(&rtl->matrix_lighttoworld, org);
					lights[nl*4+0] = org[0]; lights[nl*4+1] = org[1]; lights[nl*4+2] = org[2];
					lights[nl*4+3] = rtl->radius;
					nl++;
				}
			}
			if ((loc = R_Shader_GetUniformLocation(pp, "ReactiveParams")) >= 0) R_Shader_Uniform2f(loc, r_reactive_gain, (float)nl);
			if (nl > 0 && (loc = R_Shader_GetUniformLocation(pp, "ReactiveLights")) >= 0) R_Shader_Uniform4fv(loc, 8, lights);
		}
		{
			// prev_viewmodel * inverse(cur_viewmodel): moves a world point to
			// where that MODEL point sat last frame. Refused until the previous
			// frame's matrix exists, and refused if the current one is not
			// invertible -- both leave the shader's branch disabled and the
			// weapon on the camera-only answer, which is what it had before.
			matrix4x4_t inv, delta;
			qbool vmok = r_fb.taa_prevviewmodelvalid && r_metalfx_viewmodel.integer
			          && Matrix4x4_Invert_Full(&inv, &viewmodelmatrix_withbob) != 0;
			if (vmok)
			{
				Matrix4x4_Concat(&delta, &r_fb.taa_prevviewmodel, &inv);
				Matrix4x4_ToArrayFloatGL(&delta, m16f);
			}
			else
				Matrix4x4_ToArrayFloatGL(&identitymatrix, m16f);
			if ((loc = R_Shader_GetUniformLocation(pp, "MotionPrevModel")) >= 0) R_Shader_UniformMatrix4fv(loc, 1, false, m16f);
			// .x is the short depth range gl_rmain.c draws the weapon into
			// (GL_DepthRange(0, 0.0625) for RENDER_VIEWMODEL | RENDER_NODEPTHTEST);
			// LOCKSTEP with that literal.
			if ((loc = R_Shader_GetUniformLocation(pp, "MotionViewmodel")) >= 0) R_Shader_Uniform2f(loc, 0.0625f, vmok ? 1.0f : 0.0f);
		}
	}
	R_Mesh_Draw(0, 4, 0, 2, polygonelement3i, NULL, 0, polygonelement3s, NULL, 0);
}

// T2b: draw every moving network entity OVER the fill, each with its own
// previous-frame matrix. Same target, same 2D-reset state as the fill; the
// depth-only draw path does the walk and the shader does the depth test by
// hand (the target has no depth attachment). Entities with no table entry are
// SKIPPED, not drawn with a zero delta: the fill already gave them the camera
// motion, and a zero delta over it would erase that, which is worse.
static void R_MotionVector_Entities(int dstfbo, rtexture_t *dstcolor, int viewwidth, int viewheight, float debugscale)
{
	int i;
	if (!r_metalfx_entities.integer || !r_fb.taavalid || !r_fb.scenedepthvalid)
		return;
	R_ResetViewRendering3D(dstfbo, NULL, dstcolor, 0, 0, viewwidth, viewheight);
	GL_ScissorTest(false);
	r_motionpass_debug = debugscale;
	r_motionpass_active = true;
	for (i = 0; i < r_refdef.scene.numentities; i++)
	{
		entity_render_t *ent = r_refdef.scene.entities[i];
		struct taa_prevent_s *pe;
		if (!r_refdef.viewcache.entityvisible[i])
			continue;
		if (!R_MotionVector_EntityWanted(ent))
			continue;
		pe = R_TAA_PrevEnt(ent->entitynumber);
		// stamp == last frame's: the entry describes the previous frame of THIS
		// entity. Anything else (first sight, respawn reusing the number, a
		// frame skipped) has no honest previous matrix -- leave it to the fill.
		if (!pe || pe->stamp != r_fb.taa_frame - 1u)
			continue;
		r_motionpass_prevmodel = pe->matrix;
		R_DrawModelSurfaces(ent, false, false, true, false, false, false);
	}
	r_motionpass_active = false;
	// back to the 2D state the rest of the tail expects
	R_ResetViewRendering2D(dstfbo, NULL, dstcolor, 0, 0, viewwidth, viewheight);
}

// Allocate this frame's motion target and fill it. NULL means the temporal path
// cannot run this frame, which every caller treats as "take the other path".
static r_rendertarget_t *R_MotionVector_Pass(rtexture_t *viewdepthtexture, int viewwidth, int viewheight)
{
	r_rendertarget_t *rt;
	if (!r_fb.taawanted || !viewdepthtexture || !r_fb.scenedepthvalid)
	{
		// Same reason as the backend's warning: returning NULL here makes the
		// caller render a correct, unscaled, FASTER frame, so this failure is
		// invisible in both the picture and the frame rate unless it speaks.
		if (r_fb.taawanted)
		{
			static qbool warned;
			if (!warned)
			{
				warned = true;
				Con_Printf(CON_WARN "MetalFX: temporal wants to run but the scene depth is not available (depthtexture %p, scenedepthvalid %d); falling back to the unscaled path\n",
					(void *)viewdepthtexture, r_fb.scenedepthvalid);
			}
		}
		return NULL;
	}
	rt = R_RenderTarget_Get(viewwidth, viewheight, TEXTYPE_UNUSED, false, TEXTYPE_COLORBUFFER16F, TEXTYPE_UNUSED, TEXTYPE_UNUSED, TEXTYPE_UNUSED);
	if (!rt || !rt->colortexture[0])
		return NULL;
	R_MotionVector_Draw(viewdepthtexture, rt->fbo, rt->colortexture[0], viewwidth, viewheight, 0.0f);
	R_MotionVector_Entities(rt->fbo, rt->colortexture[0], viewwidth, viewheight, 0.0f);
	return rt;
}

// ---------------------------------------------------------------------------
// THE REACTIVE STAMP (2026-08-19): transparent batches replayed into the mask.
//
// The light-fed reactive arm above cannot see particles -- they write no depth,
// carry no motion vector, and sit outside any light sphere -- and on the
// static-camera rocket bed 78% of what moves behind a rocket is its SMOKE
// TRAIL, lingering at a third of the previous frame. So the particle callback
// (cl_particles.c) appends the vertex arrays it just drew -- world-space
// positions, the premultiplied colour, the texcoords, the texture -- into a
// persistent stash, and the mask pass replays them here with
// SHADERMODE_REACTIVESTAMP: additive into the mask, depth-tested by hand
// against the sampled scene depth (the target has no depth attachment, the
// rule the whole motion pass rests on), weighted by the particle's own visible
// strength times its texture's SHAPE -- a soft puff marks a soft disc, where a
// constant white quad would mark the transparent corners too and switch TAA
// off in a square halo round every puff, which is exactly where the fog
// twinkle would return.
//
// Two buffers, not one: the PREVIOUS frame's footprint is kept and stamped as
// well (r_metalfx_reactive_trail) -- the trailing edge, where a puff WAS and
// the history still holds it. The buffers are stamped with r_fb.taa_frame (the
// entity table's discipline, never host.framecount: the counter restarts with
// the renderer) and the previous one replays only when its stamp is exactly
// taa_frame - 1 and the history is valid. Texture pointers would dangle across
// a vid_restart (r_part_shutdown frees the particle pool), so gl_main_shutdown
// zeroes both buffers' counts and stamps; the allocations are r_main_mempool's
// and persist, grow-only.
//
// The interface is generic on purpose (render.h): the explosion shell and
// sprites build their batches the same way and are the next candidates.
typedef struct rstamp_run_s { rtexture_t *texture; int first; int count; } rstamp_run_t;
typedef struct rstamp_buf_s
{
	float *vertex3f, *color4f, *texcoord2f;
	int numvertices, maxvertices;
	rstamp_run_t *runs;
	int numruns, maxruns;
	unsigned int stamp;          // r_fb.taa_frame the contents belong to; 0 = empty
} rstamp_buf_t;
static rstamp_buf_t r_stamp[2];  // [0] this frame, [1] the previous -- rotated, never copied
static int *r_stamp_elem3i;      // the quad pattern, absolute indices, grown with the larger buffer
static int r_stamp_maxquads;
static qbool r_stamp_armed;      // true only around the MAIN R_RenderScene of a frame whose mask will run

qbool R_ReactiveStamp_Active(void)
{
	return r_stamp_armed;
}

static rstamp_buf_t *R_ReactiveStamp_Current(void)
{
	if (r_stamp[0].stamp != r_fb.taa_frame)
	{
		// a new frame: what was current becomes previous, and the older
		// buffer is recycled for this frame (rotation, not a copy)
		rstamp_buf_t t = r_stamp[1];
		r_stamp[1] = r_stamp[0];
		r_stamp[0] = t;
		r_stamp[0].numvertices = 0;
		r_stamp[0].numruns = 0;
		r_stamp[0].stamp = r_fb.taa_frame;
	}
	return &r_stamp[0];
}

void R_ReactiveStamp_Append(rtexture_t *texture, int numvertices, const float *vertex3f, const float *color4f, const float *texcoord2f)
{
	rstamp_buf_t *b;
	rstamp_run_t *run;
	if (!r_stamp_armed || numvertices <= 0 || !vertex3f || !color4f || !texcoord2f)
		return;
	// the water reflection/refraction sub-scenes draw particles too, before
	// the main scene; they are sampled as textures and must not stamp
	if (r_fb.water.renderingscene)
		return;
	b = R_ReactiveStamp_Current();
	if (b->numvertices + numvertices > b->maxvertices)
	{
		int want = max(4096, b->maxvertices * 2);
		while (want < b->numvertices + numvertices)
			want *= 2;
		b->vertex3f   = (float *)Mem_Realloc(r_main_mempool, b->vertex3f,   want * sizeof(float[3]));
		b->color4f    = (float *)Mem_Realloc(r_main_mempool, b->color4f,    want * sizeof(float[4]));
		b->texcoord2f = (float *)Mem_Realloc(r_main_mempool, b->texcoord2f, want * sizeof(float[2]));
		b->maxvertices = want;
	}
	memcpy(b->vertex3f   + b->numvertices * 3, vertex3f,   numvertices * sizeof(float[3]));
	memcpy(b->color4f    + b->numvertices * 4, color4f,    numvertices * sizeof(float[4]));
	memcpy(b->texcoord2f + b->numvertices * 2, texcoord2f, numvertices * sizeof(float[2]));
	// coalesce with the previous run when the texture matches and the range
	// is contiguous -- nearly every particle is the one atlas, so the replay
	// is then a draw or two per frame rather than one per callback batch
	run = b->numruns ? &b->runs[b->numruns - 1] : NULL;
	if (run && run->texture == texture && run->first + run->count == b->numvertices)
		run->count += numvertices;
	else
	{
		if (b->numruns >= b->maxruns)
		{
			int want = max(64, b->maxruns * 2);
			b->runs = (rstamp_run_t *)Mem_Realloc(r_main_mempool, b->runs, want * sizeof(rstamp_run_t));
			b->maxruns = want;
		}
		run = &b->runs[b->numruns++];
		run->texture = texture;
		run->first = b->numvertices;
		run->count = numvertices;
	}
	b->numvertices += numvertices;
}

// Forget the stash (restart: the textures it points at are gone). Allocations
// stay -- they are r_main_mempool's and grow-only.
static void R_ReactiveStamp_Forget(void)
{
	int i;
	for (i = 0; i < 2; i++)
	{
		r_stamp[i].numvertices = 0;
		r_stamp[i].numruns = 0;
		r_stamp[i].stamp = 0;
	}
	r_stamp_armed = false;
}

// Replay one buffer into the mask target. State is the entity walk's 3D reset
// with depth off, blend additive; the shader does the depth test by hand.
// Returns the number of draws issued (0 = nothing to stamp or refused).
static int R_ReactiveStamp_Replay(const rstamp_buf_t *b, float gain, rtexture_t *viewdepthtexture)
{
	int i, draws = 0, loc;
	int quads = b->numvertices / 4;
	if (b->numvertices < 4 || b->numruns < 1 || gain <= 0.0f)
		return 0;
	if (quads > r_stamp_maxquads)
	{
		int want = max(1024, r_stamp_maxquads * 2);
		while (want < quads)
			want *= 2;
		r_stamp_elem3i = (int *)Mem_Realloc(r_main_mempool, r_stamp_elem3i, want * 6 * sizeof(int));
		for (i = 0; i < want; i++)
		{
			r_stamp_elem3i[i * 6 + 0] = i * 4 + 0;
			r_stamp_elem3i[i * 6 + 1] = i * 4 + 1;
			r_stamp_elem3i[i * 6 + 2] = i * 4 + 2;
			r_stamp_elem3i[i * 6 + 3] = i * 4 + 0;
			r_stamp_elem3i[i * 6 + 4] = i * 4 + 2;
			r_stamp_elem3i[i * 6 + 5] = i * 4 + 3;
		}
		r_stamp_maxquads = want;
	}
	R_Mesh_PrepareVertices_Generic_Arrays(b->numvertices, b->vertex3f, b->color4f, b->texcoord2f);
	R_SetupShader_SetPermutationGLSL(SHADERMODE_REACTIVESTAMP, 0);
	if (!r_glsl_permutation || !r_glsl_permutation->program)
	{
		// A mode with no program draws the magenta SENTINEL, which would write
		// 1.0 into the mask over every particle -- full distrust, visible only
		// as twinkle inside smoke. Speak once and stamp nothing.
		static qbool warned;
		if (!warned)
		{
			warned = true;
			Con_Printf(CON_WARN "MetalFX: reactive stamp shader unavailable -- particles are not masked this session\n");
		}
		return 0;
	}
	if (r_glsl_permutation->tex_Texture_ScreenDepth >= 0)
		R_Mesh_TexBind(r_glsl_permutation->tex_Texture_ScreenDepth, viewdepthtexture);
	if ((loc = R_Shader_GetUniformLocation(r_glsl_permutation, "StampParams")) >= 0)
		R_Shader_Uniform2f(loc, gain, 0.0f);
	for (i = 0; i < b->numruns; i++)
	{
		const rstamp_run_t *run = &b->runs[i];
		if (run->count < 4)
			continue;
		if (r_glsl_permutation->tex_Texture_First >= 0)
			R_Mesh_TexBind(r_glsl_permutation->tex_Texture_First, run->texture);
		R_Mesh_Draw(run->first, run->count, run->first / 2, run->count / 2, r_stamp_elem3i, NULL, 0, NULL, NULL, 0);
		draws++;
	}
	return draws;
}

// Draw the whole mask into (dstfbo, dstcolor): the light footprint through the
// fill shader's reactive arm (a straight overwrite), then this frame's particle
// stamp and the previous frame's, additive. Used by the pass and by the debug
// view, so what the debug view shows IS what the scaler is handed.
static void R_ReactiveMask_Draw(rtexture_t *viewdepthtexture, int dstfbo, rtexture_t *dstcolor, int viewwidth, int viewheight)
{
	float pgain = r_metalfx_reactive_particles.value;
	float tgain = r_metalfx_reactive_trail.value;
	r_reactive_gain = r_metalfx_reactive.value;
	R_MotionVector_Draw(viewdepthtexture, dstfbo, dstcolor, viewwidth, viewheight, 0.0f);
	r_reactive_gain = 0.0f;
	if (r_metalfx_reactive_force.integer == 1 || r_metalfx_reactive_force.integer == -1)
	{
		// the probe: a constant mask, white or black, over everything
		float v = r_metalfx_reactive_force.integer > 0 ? 1.0f : 0.0f;
		R_ResetViewRendering2D(dstfbo, NULL, dstcolor, 0, 0, viewwidth, viewheight);
		GL_DepthTest(false);
		GL_DepthMask(false);
		GL_BlendFunc(GL_ONE, GL_ZERO);
		GL_Color(v, v, v, 1.0f);
		R_Mesh_PrepareVertices_Generic_Arrays(4, r_screenvertex3f, NULL, r_fb.rt_screen->texcoord2f);
		R_SetupShader_Generic_NoTexture(false, true);
		R_Mesh_Draw(0, 4, 0, 2, polygonelement3i, NULL, 0, polygonelement3s, NULL, 0);
		GL_Color(1, 1, 1, 1);
		return;
	}
	if (pgain > 0.0f && r_stamp[0].stamp == r_fb.taa_frame && r_stamp[0].numvertices >= 4)
	{
		static qbool engaged;
		int draws;
		// the entity walk's state, exactly: the 3D reset re-derives the
		// scene's jittered projection so in.Position.z compares like for like
		// with the scene depth; depth test/write off (no attachment; the test
		// is in the shader), scissor off, no cull, additive into the unorm mask
		R_ResetViewRendering3D(dstfbo, NULL, dstcolor, 0, 0, viewwidth, viewheight);
		GL_ScissorTest(false);
		GL_DepthTest(false);
		GL_DepthMask(false);
		GL_CullFace(GL_NONE);
		GL_PolygonOffset(0, 0);
		GL_BlendFunc(GL_ONE, GL_ONE);
		GL_Color(1, 1, 1, 1);
		R_EntityMatrix(&identitymatrix);
		draws = R_ReactiveStamp_Replay(&r_stamp[0], pgain, viewdepthtexture);
		// the trail: the previous frame's footprint, only if it IS the
		// previous frame's and the history it would protect against survived
		if (tgain > 0.0f && r_fb.taavalid && !r_fb.taareset && r_stamp[1].stamp == r_fb.taa_frame - 1u)
			draws += R_ReactiveStamp_Replay(&r_stamp[1], pgain * tgain, viewdepthtexture);
		if (r_glsl_permutation && r_glsl_permutation->tex_Texture_ScreenDepth >= 0)
			R_Mesh_TexBind(r_glsl_permutation->tex_Texture_ScreenDepth, NULL);
		// back to the 2D state the rest of the tail expects
		R_ResetViewRendering2D(dstfbo, NULL, dstcolor, 0, 0, viewwidth, viewheight);
		if (draws && !engaged)
		{
			// once per session: the positive proof smoke can assert -- every
			// way this pass fails is silent and faster
			engaged = true;
			Con_Printf("MetalFX: reactive stamp engaged (%d particle quads this frame)\n", r_stamp[0].numvertices / 4);
		}
	}
}

// The reactive mask: a second pooled target (RGBA8 -- no new TEXTYPE; MetalFX
// reads the first channel) filled by the fill shader in its reactive arm, plus
// the particle stamp. NULL when the cvar is 0, and the scaler is then handed no
// mask at all.
static r_rendertarget_t *R_ReactiveMask_Pass(rtexture_t *viewdepthtexture, int viewwidth, int viewheight)
{
	r_rendertarget_t *rt;
	if (!r_fb.taawanted || !viewdepthtexture || !r_fb.scenedepthvalid || r_metalfx_reactive.value <= 0.0f)
		return NULL;
	rt = R_RenderTarget_Get(viewwidth, viewheight, TEXTYPE_UNUSED, false, TEXTYPE_COLORBUFFER, TEXTYPE_UNUSED, TEXTYPE_UNUSED, TEXTYPE_UNUSED);
	if (!rt || !rt->colortexture[0])
		return NULL;
	R_ReactiveMask_Draw(viewdepthtexture, rt->fbo, rt->colortexture[0], viewwidth, viewheight);
	return rt;
}

static void R_MotionBlurView(int viewfbo, rtexture_t *viewdepthtexture, rtexture_t *viewcolortexture, int viewx, int viewy, int viewwidth, int viewheight)
{
	R_EntityMatrix(&identitymatrix);

	if(r_refdef.view.ismain && !R_Stereo_Active() && (r_motionblur.value > 0 || (r_damageblur.value > 0 && cl.cshifts[CSHIFT_DAMAGE].percent != 0)) && r_fb.ghosttexture)
	{
		// declare variables
		float blur_factor, blur_mouseaccel, blur_velocity;
		static float blur_average;
		static vec3_t blur_oldangles; // used to see how quickly the mouse is moving

		// set a goal for the factoring
		blur_velocity = bound(0, (VectorLength(cl.movement_velocity) - r_motionblur_velocityfactor_minspeed.value)
			/ max(1, r_motionblur_velocityfactor_maxspeed.value - r_motionblur_velocityfactor_minspeed.value), 1);
		blur_mouseaccel = bound(0, ((fabs(VectorLength(cl.viewangles) - VectorLength(blur_oldangles)) * 10) - r_motionblur_mousefactor_minspeed.value)
			/ max(1, r_motionblur_mousefactor_maxspeed.value - r_motionblur_mousefactor_minspeed.value), 1);
		blur_factor = ((blur_velocity * r_motionblur_velocityfactor.value)
			+ (blur_mouseaccel * r_motionblur_mousefactor.value));

		// from the goal, pick an averaged value between goal and last value
		cl.motionbluralpha = bound(0, (cl.time - cl.oldtime) / max(0.001, r_motionblur_averaging.value), 1);
		blur_average = blur_average * (1 - cl.motionbluralpha) + blur_factor * cl.motionbluralpha;

		// enforce minimum amount of blur
		blur_factor = blur_average * (1 - r_motionblur_minblur.value) + r_motionblur_minblur.value;

		//Con_Printf("motionblur: direct factor: %f, averaged factor: %f, velocity: %f, mouse accel: %f \n", blur_factor, blur_average, blur_velocity, blur_mouseaccel);

		// calculate values into a standard alpha
		cl.motionbluralpha = 1 - exp(-
				(
					(r_motionblur.value * blur_factor / 80)
					+
					(r_damageblur.value * (cl.cshifts[CSHIFT_DAMAGE].percent / 1600))
				)
				/
				max(0.0001, cl.time - cl.oldtime) // fps independent
				);

		// randomization for the blur value to combat persistent ghosting
		cl.motionbluralpha *= lhrandom(1 - r_motionblur_randomize.value, 1 + r_motionblur_randomize.value);
		cl.motionbluralpha = bound(0, cl.motionbluralpha, r_motionblur_maxblur.value);

		// apply the blur on top of the current view
		R_ResetViewRendering2D(viewfbo, viewdepthtexture, viewcolortexture, viewx, viewy, viewwidth, viewheight);
		if (cl.motionbluralpha > 0 && !r_refdef.envmap && r_fb.ghosttexture_valid)
		{
			GL_BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			GL_Color(1, 1, 1, cl.motionbluralpha);
			R_CalcTexCoordsForView(0, 0, viewwidth, viewheight, viewwidth, viewheight, r_fb.ghosttexcoord2f);
			R_Mesh_PrepareVertices_Generic_Arrays(4, r_screenvertex3f, NULL, r_fb.ghosttexcoord2f);
			R_SetupShader_Generic(r_fb.ghosttexture, false, true, true);
			R_Mesh_Draw(0, 4, 0, 2, polygonelement3i, NULL, 0, polygonelement3s, NULL, 0);
			r_refdef.stats[r_stat_bloom_drawpixels] += viewwidth * viewheight;
		}

		// updates old view angles for next pass
		VectorCopy(cl.viewangles, blur_oldangles);

		// copy view into the ghost texture
		R_Mesh_CopyToTexture(r_fb.ghosttexture, 0, 0, viewx, viewy, viewwidth, viewheight);
		r_refdef.stats[r_stat_bloom_copypixels] += viewwidth * viewheight;
		r_fb.ghosttexture_valid = true;
	}
}

/*
===============================================================================

VOLUMETRIC MURK (step 2)

A screen-space pass that reconstructs each pixel's world position from the scene
depth published in step 1, marches the view ray, and accumulates a fog term.
Density is an analytic height falloff multiplied by a SINGLE octave of animated
3D noise sampled at world position — one octave, not a fractal stack, because the
softness is the point (the Assassin's Creed 4 recipe).

Placement: this runs after opaque geometry but BEFORE transparent surfaces,
particles and sprites. Those do not write depth, so a screen-space pass that ran
after them would fog them as though they sat at the opaque surface behind them.
Transparent passes therefore keep using the engine's existing per-fragment fog.

The fog is rendered into its OWN half-resolution target rather than straight into
the scene: the scene's depth is an attachment of the scene framebuffer, and
sampling a texture that is attached to the bound framebuffer is a feedback loop
and undefined in GL. Rendering elsewhere and compositing back avoids that, and is
also how the half-resolution saving is taken.

===============================================================================
*/

#define VOL_NOISE_SIZE  64      // texels per axis of the noise volume
#define VOL_NOISE_CELLS 8       // lattice cells per axis; the volume tiles seamlessly

static rtexture_t *r_volumetric_noisetexture;
// the cache key: which generator, at what size and tuning, seeded for which
// world (v2 only -- the classic generator is map-agnostic and lives for the
// session, exactly as it always did)
static int r_volumetric_noisebuiltv2 = -1;
static int r_volumetric_noisebuiltsize;
static float r_volumetric_noisebuilttune;
static const model_t *r_volumetric_noisemodel;

static float R_Volumetric_Hash(int x, int y, int z, int seed)
{
	unsigned int h = (unsigned int)x * 374761393u + (unsigned int)y * 668265263u
	               + (unsigned int)z * 1442695040u + (unsigned int)seed * 1274126177u;
	h = (h ^ (h >> 13)) * 1274126177u;
	h ^= h >> 16;
	return (float)(h & 0xFFFFFFu) * (1.0f / (float)0xFFFFFFu);
}

// Tiling GRADIENT noise on the same wrapping lattice. Value noise (above) puts a
// random VALUE at each lattice corner, and interpolating those leaves the extrema
// pinned to the corners -- so its blobs sit on a regular axis-aligned grid with
// squared-off edges. On a large flat floor viewed from above, where you see the
// field face-on and Quake's own architecture is axis-aligned too, that reads as a
// visible checkerboard. Gradient noise puts a random DIRECTION at each corner and
// interpolates the dot products, which forces the value to zero AT every corner
// and moves the extrema off-lattice: same cost, no grid.
static float R_Volumetric_GradDot(int x, int y, int z, int seed, float dx, float dy, float dz)
{
	// 12 edge-midpoint gradients, the classic Perlin set: every component is 0 or
	// +-1, so the dot product is two adds and no multiply, and the set has no
	// directional bias that would reintroduce axis alignment.
	static const signed char g[12][3] = {
		{ 1, 1, 0},{-1, 1, 0},{ 1,-1, 0},{-1,-1, 0},
		{ 1, 0, 1},{-1, 0, 1},{ 1, 0,-1},{-1, 0,-1},
		{ 0, 1, 1},{ 0,-1, 1},{ 0, 1,-1},{ 0,-1,-1}};
	const signed char *v = g[(int)(R_Volumetric_Hash(x, y, z, seed) * 11.999f)];
	return v[0] * dx + v[1] * dy + v[2] * dz;
}

static float R_Volumetric_GradientNoise(float x, float y, float z, int seed, int cells)
{
	int xi = (int)floor(x), yi = (int)floor(y), zi = (int)floor(z);
	float xf = x - xi, yf = y - yi, zf = z - zi;
	// quintic fade, not smoothstep: gradient noise shows second-derivative creases
	// at the lattice planes with a cubic fade, which is the very artefact this is
	// here to remove
	float u = xf * xf * xf * (xf * (xf * 6.0f - 15.0f) + 10.0f);
	float v = yf * yf * yf * (yf * (yf * 6.0f - 15.0f) + 10.0f);
	float w = zf * zf * zf * (zf * (zf * 6.0f - 15.0f) + 10.0f);
	int x0 = ((xi % cells) + cells) % cells, x1 = (x0 + 1) % cells;
	int y0 = ((yi % cells) + cells) % cells, y1 = (y0 + 1) % cells;
	int z0 = ((zi % cells) + cells) % cells, z1 = (z0 + 1) % cells;
	float c000 = R_Volumetric_GradDot(x0, y0, z0, seed, xf,        yf,        zf);
	float c100 = R_Volumetric_GradDot(x1, y0, z0, seed, xf - 1.0f, yf,        zf);
	float c010 = R_Volumetric_GradDot(x0, y1, z0, seed, xf,        yf - 1.0f, zf);
	float c110 = R_Volumetric_GradDot(x1, y1, z0, seed, xf - 1.0f, yf - 1.0f, zf);
	float c001 = R_Volumetric_GradDot(x0, y0, z1, seed, xf,        yf,        zf - 1.0f);
	float c101 = R_Volumetric_GradDot(x1, y0, z1, seed, xf - 1.0f, yf,        zf - 1.0f);
	float c011 = R_Volumetric_GradDot(x0, y1, z1, seed, xf,        yf - 1.0f, zf - 1.0f);
	float c111 = R_Volumetric_GradDot(x1, y1, z1, seed, xf - 1.0f, yf - 1.0f, zf - 1.0f);
	{
		float a = c000 + (c100 - c000) * u, b = c010 + (c110 - c010) * u;
		float c = c001 + (c101 - c001) * u, d = c011 + (c111 - c011) * u;
		return (a + (b - a) * v) + ((c + (d - c) * v) - (a + (b - a) * v)) * w;
	}
}

// Two octaves of the above, remapped to 0..1. The second octave is what stops the
// remaining large blobs from reading as a regular pattern; it wraps too, because
// VOL_NOISE_CELLS * 2 still divides VOL_NOISE_SIZE.
//
// VOL_NOISE_GAIN is chosen so the output's spread MATCHES the value noise this
// replaced. That is load-bearing, not cosmetic: r_volumetric_noisethresh (0.30)
// and r_volumetric_groundthresh (0.25) are smoothstep edges tuned against the old
// distribution, and this noise concentrates near 0.5 exactly as the old one did --
// push the spread and every threshold silently re-tunes itself, which is the
// documented way to make a whole fog layer vanish.
#define VOL_NOISE_GAIN 0.94f
static float R_Volumetric_FBM(float x, float y, float z, int seed)
{
	float n = R_Volumetric_GradientNoise(x, y, z, seed, VOL_NOISE_CELLS)
	        + R_Volumetric_GradientNoise(x * 2.0f, y * 2.0f, z * 2.0f, seed + 101, VOL_NOISE_CELLS * 2) * 0.5f;
	return bound(0.0f, 0.485f + n * (VOL_NOISE_GAIN / 1.5f), 1.0f);
}

/*
===============================================================================

FOG-BAKE V2 (r_volumetric_noise2): the same volume, richer contents.

The sampling contract is untouched -- same texture name, same two fetches per
march step, same wrap addressing, and the whole volume still maps onto
1/r_volumetric_noisescale world units (625 at the default), so the STRICT
tiling period cannot change from here. What changes is what the eye can lock
onto inside the tile: the classic two-octave lattice reads as blobs on a grid,
and this replaces it with domain-warped Perlin-Worley clumps, extra octaves and
a ridged streak, seeded per MAP NAME so no two levels share a fog field. (The
visible-repeat kill across tiles arrives with the swirl feature, which warps
the LOOKUP coordinate at march time; this bake owns the in-tile quality.)

THE DISTRIBUTION IS MOMENT-MATCHED, NOT ADJACENT: every channel is linearly
remapped to the classic generator's mean 0.485 / sd 0.188 after generation,
because r_volumetric_noisethresh (0.30) and _groundthresh (0.25) are smoothstep
edges tuned against exactly that distribution and a drifted spread is the
documented way to make a whole fog layer silently vanish. All shaping -- warp,
clump, ridge, contrast -- happens bake-side; runtime cost is unchanged by
construction.

Threaded over Z slabs on the TaskQueue (the bouncegrid photons idiom): every
noise function here is a pure function of its inputs through the lattice hash,
so slabs share nothing but read-only context.
===============================================================================
*/

// Centred fractal stack of the tiling gradient noise; the moment match after
// generation makes exact output ranges irrelevant, consistency is what counts.
static float R_Volumetric_FBM2(float x, float y, float z, int seed, int cells, int octaves)
{
	float n = 0.0f, amp = 1.0f, norm = 0.0f;
	int o;
	for (o = 0; o < octaves; o++)
	{
		n += R_Volumetric_GradientNoise(x, y, z, seed + o * 101, cells) * amp;
		norm += amp;
		amp *= 0.5f;
		x *= 2.0f; y *= 2.0f; z *= 2.0f;
		cells *= 2;
	}
	return n / norm;
}

// Tiling Worley F1 against a PRECOMPUTED jittered-grid feature table (one point
// per lattice cell) -- the table replaces 81 hashes per evaluation and is what
// keeps the bake inside its budget. Coordinates are in lattice cells, so F1 is
// ~0.5 at a puff edge and rarely above 1.1.
static float R_Volumetric_WorleyF1(float x, float y, float z, const float *feat, int cells)
{
	int xi = (int)floor(x), yi = (int)floor(y), zi = (int)floor(z);
	int dx, dy, dz;
	float best = 1e9f;
	for (dz = -1; dz <= 1; dz++)
	{
		for (dy = -1; dy <= 1; dy++)
		{
			for (dx = -1; dx <= 1; dx++)
			{
				int cx = xi + dx, cy = yi + dy, cz = zi + dz;
				int wx = ((cx % cells) + cells) % cells;
				int wy = ((cy % cells) + cells) % cells;
				int wz = ((cz % cells) + cells) % cells;
				const float *f = feat + (((size_t)wz * cells + wy) * cells + wx) * 3;
				float fx = cx + f[0] - x, fy = cy + f[1] - y, fz = cz + f[2] - z;
				float d2 = fx * fx + fy * fy + fz * fz;
				if (d2 < best)
					best = d2;
			}
		}
	}
	return sqrt(best);
}

typedef struct volnoise2ctx_s
{
	float *rawr, *rawg, *rawb;   // centred raw fields, size^3 each
	const float *featA, *featB;  // worley feature tables
	int size, cells, cellsA, cellsB;
	int seed[4];
	int octaves;
	float warp, clump, ridge;
}
volnoise2ctx_t;

static void R_Volumetric_Noise2_Slab(taskqueue_task_t *t)
{
	volnoise2ctx_t *ctx = (volnoise2ctx_t *)t->p[0];
	int z0 = (int)t->i[0], z1 = (int)t->i[1];
	int size = ctx->size, x, y, z;
	float s = (float)ctx->cells / (float)size;
	for (z = z0; z < z1; z++)
	{
		for (y = 0; y < size; y++)
		{
			for (x = 0; x < size; x++)
			{
				size_t idx = ((size_t)z * size + y) * size + x;
				float px = x * s, py = y * s, pz = z * s;
				float wx = px, wy = py, wz = pz;
				float per, pn, wA, wB, wor, pw, base;
				// bake-side domain warp (Quilez): the offsets are themselves
				// periodic, so the warped field still tiles seamlessly
				if (ctx->warp > 0.0f)
				{
					float wamp = ctx->warp * ctx->cells * 0.25f;
					wx += wamp * R_Volumetric_FBM2(px, py, pz, ctx->seed[0] + 900, ctx->cells, 2);
					wy += wamp * R_Volumetric_FBM2(px, py, pz, ctx->seed[0] + 910, ctx->cells, 2);
					wz += wamp * R_Volumetric_FBM2(px, py, pz, ctx->seed[0] + 920, ctx->cells, 2);
				}
				// R: domain-warped Perlin-Worley with an optional ridged streak
				per = R_Volumetric_FBM2(wx, wy, wz, ctx->seed[0], ctx->cells, ctx->octaves);
				pn = 0.5f + per;
				wA = 1.0f - min(R_Volumetric_WorleyF1(wx, wy, wz, ctx->featA, ctx->cellsA) / 1.1f, 1.0f);
				wB = 1.0f - min(R_Volumetric_WorleyF1(wx * 2.0f, wy * 2.0f, wz * 2.0f, ctx->featB, ctx->cellsB) / 1.1f, 1.0f);
				wor = wA * 0.65f + wB * 0.35f;
				// the HZD remap: Worley as the Perlin's new lower bound, which is
				// what gathers the density into cauliflower puffs
				pw = (pn - (wor - 1.0f)) / (2.0f - wor);
				base = pn + (pw - pn) * ctx->clump;
				if (ctx->ridge > 0.0f)
				{
					float rn = R_Volumetric_FBM2(wx * 2.0f, wy * 2.0f, wz * 2.0f, ctx->seed[1], ctx->cells * 2, 1);
					base += ctx->ridge * (1.0f - 2.0f * fabs(rn));
				}
				ctx->rawr[idx] = base;
				// G (bed undulation) and B (ground swell): the classic two-octave
				// character, decorrelated seeds, sharing the warp -- their consumers
				// tuned against smooth large-scale fields, so no clump or ridge here
				ctx->rawg[idx] = R_Volumetric_FBM2(wx, wy, wz, ctx->seed[2], ctx->cells, 2);
				ctx->rawb[idx] = R_Volumetric_FBM2(wx, wy, wz, ctx->seed[3], ctx->cells, 2);
			}
		}
	}
	t->done = 1;
}

#define VOL_NOISE2_SLABS 16

static rtexture_t *R_Volumetric_BuildNoiseV2(int size, const char *mapname)
{
	unsigned int words[4];
	volnoise2ctx_t ctx;
	taskqueue_task_t tasks[VOL_NOISE2_SLABS], donetask;
	randomseed_t rs;
	float *raw, *featA, *featB;
	unsigned char *data;
	double mean[3], sd[3];
	size_t total = (size_t)size * size * size, i;
	int c, slab, zper;
	double starttime = Sys_DirtyTime();
	rtexture_t *tex;

	memset(&ctx, 0, sizeof(ctx));
	ctx.size = size;
	ctx.cells = VOL_NOISE_CELLS;      // 8 base cells: the blob size the murk was tuned on
	ctx.cellsA = VOL_NOISE_CELLS;
	ctx.cellsB = VOL_NOISE_CELLS * 2;
	ctx.octaves = bound(2, r_volumetric_noise2_octaves.integer, 5);
	ctx.warp = bound(0.0f, r_volumetric_noise2_warp.value, 1.0f);
	ctx.clump = bound(0.0f, r_volumetric_noise2_clump.value, 1.0f);
	ctx.ridge = bound(0.0f, r_volumetric_noise2_ridge.value, 1.0f);

	Math_SeedFromString(words, mapname, 0x4D35F04Eu);
	for (c = 0; c < 4; c++)
		ctx.seed[c] = (int)words[c];

	raw = (float *)Mem_Alloc(tempmempool, total * 3 * sizeof(float));
	featA = (float *)Mem_Alloc(tempmempool, (size_t)ctx.cellsA * ctx.cellsA * ctx.cellsA * 3 * sizeof(float));
	featB = (float *)Mem_Alloc(tempmempool, (size_t)ctx.cellsB * ctx.cellsB * ctx.cellsB * 3 * sizeof(float));
	data = (unsigned char *)Mem_Alloc(tempmempool, total * 4);
	if (!raw || !featA || !featB || !data)
	{
		if (raw) Mem_Free(raw);
		if (featA) Mem_Free(featA);
		if (featB) Mem_Free(featB);
		if (data) Mem_Free(data);
		return NULL;
	}
	ctx.rawr = raw;
	ctx.rawg = raw + total;
	ctx.rawb = raw + total * 2;
	ctx.featA = featA;
	ctx.featB = featB;

	// blue-noise-flavoured clump seeds: one jittered feature point per lattice
	// cell, drawn from the per-map seed (all four words vary -- the s[3] rule)
	Math_RandomSeed_FromInts(&rs, words[0], words[1], words[2], words[3] ^ 0x57524C59u);
	for (i = 0; i < (size_t)ctx.cellsA * ctx.cellsA * ctx.cellsA * 3; i++)
		featA[i] = Math_randomf(&rs);
	for (i = 0; i < (size_t)ctx.cellsB * ctx.cellsB * ctx.cellsB * 3; i++)
		featB[i] = Math_randomf(&rs);

	zper = (size + VOL_NOISE2_SLABS - 1) / VOL_NOISE2_SLABS;
	for (slab = 0; slab < VOL_NOISE2_SLABS; slab++)
	{
		int z0 = slab * zper, z1 = min(z0 + zper, size);
		TaskQueue_Setup(&tasks[slab], NULL, R_Volumetric_Noise2_Slab, z0, max(z0, z1), &ctx, NULL);
	}
	TaskQueue_Enqueue(VOL_NOISE2_SLABS, tasks);
	TaskQueue_Setup(&donetask, NULL, TaskQueue_Task_CheckTasksDone, VOL_NOISE2_SLABS, 0, tasks, NULL);
	TaskQueue_Enqueue(1, &donetask);
	TaskQueue_WaitForTaskDone(&donetask);

	// moment-match each channel to the classic distribution (0.485 / 0.188):
	// the thresholds tuned against the old noise keep meaning what they meant
	for (c = 0; c < 3; c++)
	{
		const float *src = raw + total * c;
		double m = 0.0, v = 0.0, scale;
		float target_sd = 0.188f * (c == 0 ? max(0.05f, r_volumetric_noise2_contrast.value) : 1.0f);
		for (i = 0; i < total; i++)
			m += src[i];
		m /= total;
		for (i = 0; i < total; i++)
			v += (src[i] - m) * (src[i] - m);
		v = sqrt(v / total);
		scale = v > 1e-6 ? target_sd / v : 0.0;
		mean[c] = m;
		sd[c] = v;
		for (i = 0; i < total; i++)
		{
			float out = 0.485f + (float)((src[i] - m) * scale);
			data[i * 4 + c] = (unsigned char)bound(0, (int)(out * 255.0f), 255);
		}
	}
	for (i = 0; i < total; i++)
		data[i * 4 + 3] = 255;   // A dead on GL without TEXF_ALPHA, deliberate (the classic rule)

	Con_DPrintf("volumetric noise v2: raw moments R %.3f/%.3f G %.3f/%.3f B %.3f/%.3f (matched to 0.485/0.188)\n",
		mean[0], sd[0], mean[1], sd[1], mean[2], sd[2]);

	tex = R_LoadTexture3D(r_main_texturepool, "volumetricnoise",
		size, size, size, data, TEXTYPE_RGBA, TEXF_FORCELINEAR, 0, NULL);
#ifdef USE_RT_METAL
	// same bytes to the fog kernel, as ever; the sidecar copies them
	RT_Metal_SetFogNoise(data, size);
#endif
	// %dx%dx%d, never %d^3: the console eats ^3 as a colour escape
	Con_Printf("volumetric noise v2: %dx%dx%d, seeded '%s', %.0f KB, baked in %.0f ms\n",
		size, size, size, mapname, total * 4 / 1024.0, (Sys_DirtyTime() - starttime) * 1000.0);
	Mem_Free(raw);
	Mem_Free(featA);
	Mem_Free(featB);
	Mem_Free(data);
	return tex;
}

// Built lazily; the classic generator once per session, the v2 generator once
// per MAP (the key below). R = the density field, G = a second, independent
// field used to undulate the height of the fog bed so the murk forms layered
// banks with real vertical structure rather than one flat slab.
static rtexture_t *R_Volumetric_GetNoiseTexture(void)
{
	unsigned char *data;
	int x, y, z;
	float s;
	double sum = 0.0, sumsq = 0.0;
	int nsamples = 0;
	int v2 = r_volumetric_noise2.integer != 0;
	int nsize = v2 ? bound(32, r_volumetric_noisesize.integer, 128) : VOL_NOISE_SIZE;
	const model_t *world = cl.worldmodel;   // NULL at the menu: the v2 seed falls back to ""
	float tunekey = v2 ? (r_volumetric_noise2_warp.value
	                    + r_volumetric_noise2_clump.value * 10.0f
	                    + r_volumetric_noise2_ridge.value * 100.0f
	                    + r_volumetric_noise2_contrast.value * 1000.0f
	                    + r_volumetric_noise2_octaves.integer * 10000.0f) : 0.0f;

	if (r_volumetric_noisetexture && r_volumetric_noisebuiltv2 == v2
	 && r_volumetric_noisebuiltsize == nsize && r_volumetric_noisebuilttune == tunekey
	 && (!v2 || r_volumetric_noisemodel == world))
		return r_volumetric_noisetexture;
	if (r_volumetric_noisetexture)
		R_FreeTexture(r_volumetric_noisetexture);
	r_volumetric_noisetexture = NULL;
	r_volumetric_noisebuiltv2 = -1;
	r_volumetric_noisemodel = NULL;

	if (v2)
	{
		// no world, no bake: the per-map seed would be meaningless, the murk
		// cannot run mapless anyway, and the lava bind's white fallback covers
		// the menu. Without this the boot burned a full bake on seed '' and
		// immediately threw it away when the first map loaded.
		if (!world)
			return NULL;
		r_volumetric_noisetexture = R_Volumetric_BuildNoiseV2(nsize, world->name);
		if (r_volumetric_noisetexture)
		{
			r_volumetric_noisebuiltv2 = 1;
			r_volumetric_noisebuiltsize = nsize;
			r_volumetric_noisebuilttune = tunekey;
			r_volumetric_noisemodel = world;
		}
		return r_volumetric_noisetexture;
	}

	data = (unsigned char *)Mem_Alloc(tempmempool, VOL_NOISE_SIZE * VOL_NOISE_SIZE * VOL_NOISE_SIZE * 4);
	if (!data)
		return NULL;
	s = (float)VOL_NOISE_CELLS / (float)VOL_NOISE_SIZE;
	for (z = 0; z < VOL_NOISE_SIZE; z++)
	{
		for (y = 0; y < VOL_NOISE_SIZE; y++)
		{
			for (x = 0; x < VOL_NOISE_SIZE; x++)
			{
				unsigned char *p = data + ((z * VOL_NOISE_SIZE + y) * VOL_NOISE_SIZE + x) * 4;
				float n0 = R_Volumetric_FBM(x * s, y * s, z * s, 1);
				float n1 = R_Volumetric_FBM(x * s, y * s, z * s, 7);
				float n2 = R_Volumetric_FBM(x * s, y * s, z * s, 13);
				sum += n0; sumsq += n0 * n0; nsamples++;
				p[0] = (unsigned char)bound(0, (int)(n0 * 255.0f), 255);
				p[1] = (unsigned char)bound(0, (int)(n1 * 255.0f), 255);
				// B: a third decorrelated field for the ground layer's top swell
				// (was a duplicate of R that nothing sampled). A stays 255: without
				// TEXF_ALPHA the GL internal format is GL_RGB and A reads as 1.0.
				p[2] = (unsigned char)bound(0, (int)(n2 * 255.0f), 255);
				p[3] = 255;
			}
		}
	}
	// The thresholds (r_volumetric_noisethresh, _groundthresh) are smoothstep edges
	// tuned against this distribution, so print it: a mean that has drifted off 0.5
	// or a spread that has changed is the fastest explanation for a fog layer that
	// suddenly reads too thin or too thick.
	Con_DPrintf("volumetric noise: mean %.3f, sd %.3f (%d samples)\n",
		sum / nsamples, sqrt(sumsq / nsamples - (sum / nsamples) * (sum / nsamples)), nsamples);
	r_volumetric_noisetexture = R_LoadTexture3D(r_main_texturepool, "volumetricnoise",
		VOL_NOISE_SIZE, VOL_NOISE_SIZE, VOL_NOISE_SIZE, data, TEXTYPE_RGBA,
		TEXF_FORCELINEAR, 0, NULL);
#ifdef USE_RT_METAL
	// the fog kernel evaluates the same density model and needs the same bytes; the
	// sidecar copies them (this buffer is freed on the next line)
	RT_Metal_SetFogNoise(data, VOL_NOISE_SIZE);
#endif
	Mem_Free(data);
	if (r_volumetric_noisetexture)
	{
		r_volumetric_noisebuiltv2 = 0;
		r_volumetric_noisebuiltsize = VOL_NOISE_SIZE;
		r_volumetric_noisebuilttune = 0.0f;
		r_volumetric_noisemodel = NULL;
	}
	return r_volumetric_noisetexture;
}

/*
===============================================================================

VOLUMETRICS: the baked world field

A coarse 3D map of the level, built once per map load and sampled once per
raymarch step. It answers the two questions the murk needs and the camera
cannot: how far above the local floor is this point, and is it inside a liquid.

  R  height above the floor directly below the sample, 0..VOL_FIELD_MAXH units
  G  signed distance to the nearest liquid boundary, positive inside, biased so
     that 0.5 is the surface
  B  which liquid: 0 water, 0.5 slime, 1 lava
  A  enclosure: how boxed-in the cell is, 0 in the open, 1 buried in solid

Enclosure is what keeps the murk from reading as generic cloudiness. It is the
cell occupancy blurred over a couple of cells, so it rises towards every surface
and rises FASTEST where several surfaces meet -- next to a flat wall it lands
near a third, in a wall/floor corner near a half, in a slot higher still.
Squaring it in the shader turns that into "corners and crevices" rather than
"anywhere near geometry".

G is a signed distance rather than a plain inside/outside flag on purpose:
trilinear filtering of a signed distance reconstructs the zero crossing -- the
waterline -- to within a couple of units even though the field itself is 64
units per cell. An inside/outside flag would smear the same waterline across a
whole cell, and the eye is far more forgiving about the *shape* of a pool than
about a waterline that does not line up with the water surface it can see.

R is deliberately defined inside solid too (it goes to zero at the floor and
stays there below it), so that a cell straddling the floor plane interpolates
sensibly rather than against garbage. Note this is an invariant the bake has to
WORK to maintain, and for a long time did not: the column walk only learns where
the floor is when it crosses one, so everything below the first floor -- and
every cell of a fully-solid column, which never crosses one at all -- was left
holding height above the WORLD BOTTOM. Two passes restore it: a per-column
back-fill below the first floor, and a lateral min-dilate that lends floorless
columns their neighbours' floor. Both are in R_Volumetric_GetField and both are
load-bearing; without them the murk thins towards every floor and every wall,
which is the opposite of the intended look.
===============================================================================
*/

#define VOL_FIELD_MAXXY  128     // cells per horizontal axis, ceiling
#define VOL_FIELD_MAXZ   96      // cells vertically, ceiling
#define VOL_FIELD_MAXH   1024.0f // world units encodable in the height channel
#define VOL_FIELD_PROBE  8.0f    // vertical spacing of the column contents probes
#define VOL_FIELD_PROBES 512     // ceiling on probes per column, for very tall maps
#define VOL_FIELD_SPANS  16      // liquid spans tracked per column

typedef struct volfieldspan_s
{
	float bottom, top;
	float kind;              // 0 water, 0.5 slime, 1 lava
}
volfieldspan_t;

static rtexture_t *r_volumetric_fieldtexture;
// A CPU copy of the field is kept alongside the texture. It is small (128-342 KB on
// stock maps) and it is the only way to answer "what does the murk actually see at
// this point in the world", which is the question every look-wrong report turns into.
static unsigned char *r_volumetric_fielddata;
static int r_volumetric_fieldsize[3];
static const model_t *r_volumetric_fieldmodel;   // the world this was baked for
static float r_volumetric_fieldcellbuilt;        // cell size it was baked at
static float r_volumetric_fieldorigin[3];        // world position of the field corner
static float r_volumetric_fieldinvsize[3];       // world units -> 0..1 texture coords
static float r_volumetric_fieldsdfrange;         // world units the liquid distance spans

// --- the baked irradiance grid (r_volumetric_ambient) ----------------------
// A coarse RGB map of the level's STATIC lighting, sampled from the lightmap
// machinery once per map load and multiplied into the murk's base colour per
// march step -- dark rooms give dark fog. Kept beside the field because the
// two share their whole lifecycle (bake on demand, free on map change).
#define VOL_IRR_MAXXY 96
#define VOL_IRR_MAXZ  64
static rtexture_t *r_volumetric_irrtexture;
static unsigned char *r_volumetric_irrdata;      // CPU copy, handed to the sidecar
static int r_volumetric_irrsize[3];
static const model_t *r_volumetric_irrmodel;
static float r_volumetric_irrcellbuilt;
static int r_volumetric_irrdilatebuilt = -1;
static float r_volumetric_irrorigin[3];
static float r_volumetric_irrinvsize[3];

static void R_Volumetric_FreeIrradiance(void)
{
	if (r_volumetric_irrtexture)
		R_FreeTexture(r_volumetric_irrtexture);
	if (r_volumetric_irrdata)
		Mem_Free(r_volumetric_irrdata);
	r_volumetric_irrtexture = NULL;
	r_volumetric_irrdata = NULL;
	r_volumetric_irrmodel = NULL;
	r_volumetric_irrcellbuilt = 0;
}

static void R_Volumetric_FreeField(void)
{
	if (r_volumetric_fieldtexture)
		R_FreeTexture(r_volumetric_fieldtexture);
	if (r_volumetric_fielddata)
		Mem_Free(r_volumetric_fielddata);
	r_volumetric_fieldtexture = NULL;
	r_volumetric_fielddata = NULL;
	r_volumetric_fieldmodel = NULL;
	r_volumetric_fieldcellbuilt = 0;
}

static void R_Volumetric_ForgetTextures(void)
{
	if (r_volumetric_fielddata)
		Mem_Free(r_volumetric_fielddata);
	r_volumetric_fielddata = NULL;
	r_volumetric_fieldtexture = NULL;
	r_volumetric_fieldmodel = NULL;
	r_volumetric_fieldcellbuilt = 0;
	r_volumetric_noisetexture = NULL;
	r_volumetric_noisebuiltv2 = -1;
	r_volumetric_noisemodel = NULL;
	// the pool owns the GPU texture on this path, so forget rather than free
	if (r_volumetric_irrdata)
		Mem_Free(r_volumetric_irrdata);
	r_volumetric_irrdata = NULL;
	r_volumetric_irrtexture = NULL;
	r_volumetric_irrmodel = NULL;
	r_volumetric_irrcellbuilt = 0;
}

/*
Turn the raw cell occupancy in the alpha channel into the enclosure term.

Two steps, and the second one is not optional. Blurring occupancy gives a genuinely
useful number *for cells that are open air*: zero in the middle of a room, about a
third against a flat wall, and roughly double that where two walls meet, because two
directions contribute instead of one. That is exactly the corner/wall discrimination
the murk wants.

But a Quake map is mostly solid rock, and every solid cell sits at 1.0. Left alone,
trilinear interpolation from an open cell towards a wall races up to that 1.0, so a
sample twenty units off a flat wall reads like it is buried in a crevice and the term
degenerates into "near any geometry at all". So the second step overwrites each solid
cell with the largest enclosure among its open neighbours: the field then continues
the open-air trend across the boundary instead of spiking, and a flat wall keeps
reading as a flat wall right up to its face.
*/
static void R_Volumetric_BlurEnclosure(unsigned char *data, unsigned char *scratch, const int *n)
{
	size_t total = (size_t)n[0] * n[1] * n[2];
	unsigned char *occ = scratch;             // raw occupancy, kept for the dilate
	unsigned char *tmp = scratch + total;
	size_t stride[3];
	size_t t;
	int pass, ax, i, j, k, d;

	stride[0] = 1;
	stride[1] = n[0];
	stride[2] = (size_t)n[0] * n[1];
	for (t = 0; t < total; t++)
		occ[t] = data[t * 4 + 3];

	// separable 1-2-1, twice per axis: reaches two cells, about 128 world units at
	// the default resolution, which is the scale at which a room corner reads as a
	// corner rather than as two unrelated walls
	for (pass = 0; pass < 2; pass++)
	{
		for (ax = 0; ax < 3; ax++)
		{
			for (k = 0; k < n[2]; k++)
			{
				for (j = 0; j < n[1]; j++)
				{
					for (i = 0; i < n[0]; i++)
					{
						int coord[3], lo, hi, mid;
						size_t o = ((size_t)k * n[1] + j) * n[0] + i;
						coord[0] = i; coord[1] = j; coord[2] = k;
						mid = data[o * 4 + 3];
						// clamp at the edges rather than wrapping: the world ends there
						lo = coord[ax] > 0          ? data[(o - stride[ax]) * 4 + 3] : mid;
						hi = coord[ax] < n[ax] - 1  ? data[(o + stride[ax]) * 4 + 3] : mid;
						tmp[o] = (unsigned char)((lo + 2 * mid + hi) >> 2);
					}
				}
			}
			for (t = 0; t < total; t++)
				data[t * 4 + 3] = tmp[t];
		}
	}

	// dilate the open-air values into the solid cells
	for (t = 0; t < total; t++)
		tmp[t] = data[t * 4 + 3];
	for (k = 0; k < n[2]; k++)
	{
		for (j = 0; j < n[1]; j++)
		{
			for (i = 0; i < n[0]; i++)
			{
				int coord[3], best = -1;
				size_t o = ((size_t)k * n[1] + j) * n[0] + i;
				if (occ[o] <= 128)
					continue;                 // already open air, keep the blurred value
				coord[0] = i; coord[1] = j; coord[2] = k;
				for (ax = 0; ax < 3; ax++)
				{
					for (d = -1; d <= 1; d += 2)
					{
						size_t nb;
						if (coord[ax] + d < 0 || coord[ax] + d >= n[ax])
							continue;
						nb = o + (size_t)(d * (ptrdiff_t)stride[ax]);
						if (occ[nb] <= 128 && (int)tmp[nb] > best)
							best = tmp[nb];
					}
				}
				if (best >= 0)
					data[o * 4 + 3] = (unsigned char)best;
			}
		}
	}
}

// Built lazily on the first frame that needs it rather than from the newmap hook,
// so a session that never turns the murk on never pays for it.
static rtexture_t *R_Volumetric_GetField(void)
{
	model_t *world = cl.worldmodel;
	volfieldspan_t spans[VOL_FIELD_SPANS];
	unsigned char *data;
	float *probefloor, *probesdf, *probekind, *probesolid;
	unsigned char *scratch, *colfloored;
	float mins[3], maxs[3], cell[3], cellsize, probestep, sdfrange, curfloor, firstfloor;
	int n[3], i, j, k, s, numprobes, numspans, prevclass, spanopen, offwet, firstfloork;
	float qx, qy;
	double starttime;
	size_t bytes;

	if (!world || !world->PointSuperContents)
		return NULL;
	cellsize = bound(8.0f, r_volumetric_fieldcell.value, 512.0f);
	if (r_volumetric_fieldtexture && r_volumetric_fieldmodel == world
	 && r_volumetric_fieldcellbuilt == cellsize)
		return r_volumetric_fieldtexture;
	R_Volumetric_FreeField();

	starttime = Sys_DirtyTime();

	// One cell of margin all round, so the trilinear filter has real data at the very
	// edge of the world instead of clamping against a half cell of nothing.
	for (i = 0; i < 3; i++)
	{
		mins[i] = world->normalmins[i] - cellsize;
		maxs[i] = world->normalmaxs[i] + cellsize;
		n[i] = (int)ceil((maxs[i] - mins[i]) / cellsize);
	}
	n[0] = bound(2, n[0], VOL_FIELD_MAXXY);
	n[1] = bound(2, n[1], VOL_FIELD_MAXXY);
	n[2] = bound(2, n[2], VOL_FIELD_MAXZ);
	for (i = 0; i < 3; i++)
	{
		cell[i] = (maxs[i] - mins[i]) / n[i];
		r_volumetric_fieldorigin[i] = mins[i];
		r_volumetric_fieldinvsize[i] = 1.0f / (maxs[i] - mins[i]);
	}
	// Truncate the signed distance at a couple of cells. Storing the true distance
	// would make a dry cell next to a flooded one read -1024 against +40, and the
	// interpolated zero crossing would sit almost on top of the wet cell instead of
	// halfway between them.
	sdfrange = 2.0f * max(cell[0], max(cell[1], cell[2]));
	r_volumetric_fieldsdfrange = sdfrange;

	// Fine enough to place a floor to within a few units, but never so fine that a
	// very tall map turns the bake into millions of BSP descents.
	// the footprint offsets for liquid detection, out towards the cell corners
	qx = cell[0] * 0.35f;
	qy = cell[1] * 0.35f;

	probestep = min(VOL_FIELD_PROBE, cell[2] * 0.5f);
	probestep = max(probestep, (maxs[2] - mins[2]) / VOL_FIELD_PROBES);
	numprobes = (int)ceil((maxs[2] - mins[2]) / probestep) + 1;

	bytes = (size_t)n[0] * n[1] * n[2] * 4;
	data = (unsigned char *)Mem_Alloc(tempmempool, bytes);
	probefloor = (float *)Mem_Alloc(tempmempool, numprobes * 4 * sizeof(float));
	if (!data || !probefloor)
	{
		if (data) Mem_Free(data);
		if (probefloor) Mem_Free(probefloor);
		return NULL;
	}
	probesdf = probefloor + numprobes;
	probekind = probesdf + numprobes;
	probesolid = probekind + numprobes;
	scratch = (unsigned char *)Mem_Alloc(tempmempool, (size_t)n[0] * n[1] * n[2] * 2);
	// which columns found a real floor; drives the wall dilate after the walk
	colfloored = (unsigned char *)Mem_Alloc(tempmempool, (size_t)n[0] * n[1]);
	if (!scratch || !colfloored)
	{
		if (scratch) Mem_Free(scratch);
		if (colfloored) Mem_Free(colfloored);
		Mem_Free(probefloor);
		Mem_Free(data);
		return NULL;
	}

	for (j = 0; j < n[1]; j++)
	{
		for (i = 0; i < n[0]; i++)
		{
			vec3_t p;
			p[0] = mins[0] + (i + 0.5f) * cell[0];
			p[1] = mins[1] + (j + 0.5f) * cell[1];

			// Walk the column from the bottom up, recording the floor beneath each
			// probe and the liquid spans it passes through. Everything below the world
			// counts as solid, which is also what the BSP says about the void.
			numspans = 0;
			spanopen = 0;
			curfloor = mins[2];
			prevclass = 1;
			offwet = 0;
			firstfloor = mins[2];
			firstfloork = -1;
			for (k = 0; k < numprobes; k++)
			{
				int contents, cclass, wet;
				p[2] = mins[2] + k * probestep;
				contents = world->PointSuperContents(world, 0, p);
				if (contents & SUPERCONTENTS_LAVA)       cclass = 4;
				else if (contents & SUPERCONTENTS_SLIME) cclass = 3;
				else if (contents & SUPERCONTENTS_WATER) cclass = 2;
				else if (contents & SUPERCONTENTS_SOLID) cclass = 1; // sky is solid too
				else                                     cclass = 0;

				// solid -> open: the floor lies between this probe and the last one.
				// Floors come from the CENTRE only -- a floor is a surface you stand on
				// and wants the honest answer at the cell's own position.
				if (prevclass == 1 && cclass != 1)
				{
					curfloor = p[2] - probestep * 0.5f;
					if (firstfloork < 0)
					{
						firstfloor = curfloor;
						firstfloork = k;
					}
				}
				probefloor[k] = curfloor;
				probesolid[k] = (cclass == 1) ? 1.0f : 0.0f;
				prevclass = cclass;

				// LIQUID, on the other hand, is sampled across the cell's whole
				// footprint. One probe at the column centre misses a quarter of the
				// water in a stock map -- measured on e1m4 -- because a 64-unit cell
				// can easily have rock at its centre and lake at its corners. A gap in
				// the murk is far more visible than a little water bleeding into a
				// wall the ray stops at anyway, so this deliberately biases wet.
				// Every fourth step only: liquid volumes are thick, and this keeps the
				// bake at roughly 2x rather than 5x.
				//
				// ...AND EVERY STEP WHILE A SPAN IS OPEN (2026-09-09, Seb's demo40 on
				// e1m4). The stride's result was carried forward through the next
				// three steps, so a span whose top the footprint alone carried closed
				// up to three probe steps (24 units) ABOVE the real surface. In the
				// field that is a strip of "inside liquid" along every corridor wall
				// that stands beside a water channel -- the ground fog and the air
				// murk are swallowed inside liquid, so the fog stopped short of the
				// walls -- and over a lake it lifted the mist band a hand's height off
				// the water (his "too high a clear gap", wedge-shaped at grazing
				// angles). Probing every step while the span is open closes the top
				// at the probe step (8 units); the cost is the footprint's four probes
				// per step inside liquid volumes only, a small share of any level.
				if ((k & 3) == 0 || spanopen)
				{
					int oi;
					offwet = 0;
					for (oi = 0; oi < 4; oi++)
					{
						vec3_t q;
						int oc, ocontents;
						q[0] = p[0] + ((oi & 1) ? qx : -qx);
						q[1] = p[1] + ((oi & 2) ? qy : -qy);
						q[2] = p[2];
						ocontents = world->PointSuperContents(world, 0, q);
						if (ocontents & SUPERCONTENTS_LAVA)       oc = 4;
						else if (ocontents & SUPERCONTENTS_SLIME) oc = 3;
						else if (ocontents & SUPERCONTENTS_WATER) oc = 2;
						else                                      oc = 0;
						if (oc > offwet)
							offwet = oc;
					}
				}
				wet = max(cclass >= 2 ? cclass : 0, offwet);

				if (wet >= 2)
				{
					if (!spanopen)
					{
						if (numspans >= VOL_FIELD_SPANS)
							break;
						spans[numspans].bottom = p[2] - probestep * 0.5f;
						spans[numspans].kind = (wet == 4) ? 1.0f : (wet == 3) ? 0.5f : 0.0f;
						spanopen = 1;
					}
					spans[numspans].top = p[2] + probestep * 0.5f;
				}
				else if (spanopen)
				{
					spanopen = 0;
					numspans++;
				}
			}
			if (spanopen)
				numspans++;
			// probes past a break above stay at whatever the last walk left; fill the
			// tail so the voxel loop below never reads uninitialised floor values
			for (; k < numprobes; k++)
			{
				probefloor[k] = curfloor;
				probesolid[k] = 0.0f;
			}

			// BELOW THE FIRST FLOOR the walk had nothing to report yet, so it left
			// the world minimum behind and those probes store height-above-WORLD-
			// BOTTOM -- hundreds of units where the channel is documented to read
			// zero. They are not academic: a sample standing ON the floor
			// trilinearly mixes the cells straddling it, so the garbage underneath
			// pulls `above` UP and the murk thins exactly where it should be
			// thickest. Measured on e1m3 before this: density 0.028 at the floor
			// rising to 0.545 at eye height -- an inverted profile, which reads in
			// game as walking through mist at waist height over clear air.
			// Clamping to the floor makes (z - floor) negative below it, which the
			// bound() in the resample turns into the documented zero.
			if (firstfloork > 0)
				for (k = 0; k < firstfloork; k++)
					probefloor[k] = firstfloor;
			colfloored[j * n[0] + i] = (firstfloork >= 0) ? 1 : 0;

			// Signed distance to the nearest liquid boundary. min(top - z, z - bottom)
			// is positive inside a span and equals minus the distance to the nearest
			// face outside it, so the largest value over all spans is the answer in
			// both cases.
			for (k = 0; k < numprobes; k++)
			{
				float z = mins[2] + k * probestep;
				float best = -sdfrange, bestkind = 0.0f;
				for (s = 0; s < numspans; s++)
				{
					float d = min(spans[s].top - z, z - spans[s].bottom);
					if (d > best)
					{
						best = d;
						bestkind = spans[s].kind;
					}
				}
				probesdf[k] = bound(-sdfrange, best, sdfrange);
				probekind[k] = bestkind;
			}

			// resample the fine column onto the field's own vertical cells
			for (k = 0; k < n[2]; k++)
			{
				float z = mins[2] + (k + 0.5f) * cell[2];
				int probe = bound(0, (int)((z - mins[2]) / probestep + 0.5f), numprobes - 1);
				unsigned char *px = data + (((size_t)k * n[1] + j) * n[0] + i) * 4;
				float h = (z - probefloor[probe]) * (1.0f / VOL_FIELD_MAXH);
				float d = 0.5f + probesdf[probe] * (0.5f / sdfrange);
				px[0] = (unsigned char)bound(0, (int)(h * 255.0f + 0.5f), 255);
				px[1] = (unsigned char)bound(0, (int)(d * 255.0f + 0.5f), 255);
				px[2] = (unsigned char)bound(0, (int)(probekind[probe] * 255.0f + 0.5f), 255);
				// raw occupancy: the fraction of this cell's height that is solid. The
				// blur below turns it into the enclosure term.
				{
					int p0 = (int)ceil(k * cell[2] / probestep);
					int p1 = (int)floor((k + 1) * cell[2] / probestep);
					int count = 0;
					float solid = 0.0f;
					int pi;
					for (pi = max(0, p0); pi <= min(p1, numprobes - 1); pi++)
					{
						solid += probesolid[pi];
						count++;
					}
					px[3] = count ? (unsigned char)bound(0, (int)(solid / count * 255.0f + 0.5f), 255)
					              : (unsigned char)bound(0, (int)(probesolid[probe] * 255.0f + 0.5f), 255);
				}
			}
		}
	}

	// FULLY-SOLID COLUMNS -- every wall, since sky counts as solid too -- never see
	// a solid->open transition at all, so the clamp above has no floor to clamp to
	// and their whole R stack is height-above-world-bottom. Trilinear filtering
	// drags every sample within half a cell of a wall towards that, which is the
	// band of clear air that hugs every corridor wall. The ground fog suffers most:
	// its 24-unit falloff turns a +150-unit error into total clearance (3.5e-4x)
	// where the air murk's 180 only thins it 2.3x.
	//
	// Give them the nearest real floor instead. EIGHT lateral neighbours, because a
	// sample in open air trilinearly mixes the eight cells around it and a DIAGONAL
	// wall column is reachable that way; min(), because the defect is R being too
	// large and a hole in the murk is far more visible than a little fog bleeding
	// into a wall the ray stops at anyway. One pass is sufficient for exactly the
	// same reason the neighbourhood is eight-wide: a cell more than one column deep
	// into a wall can never be mixed into a sample taken in open air.
	// Reads only floored columns and writes only floorless ones, so it is safe in
	// place. Must run BEFORE R_Volumetric_BlurEnclosure, which consumes the alpha
	// channel, but it touches R only and the two are independent.
	for (j = 0; j < n[1]; j++)
	{
		for (i = 0; i < n[0]; i++)
		{
			if (colfloored[j * n[0] + i])
				continue;
			for (k = 0; k < n[2]; k++)
			{
				int di, dj, best = -1;
				for (dj = -1; dj <= 1; dj++)
				{
					for (di = -1; di <= 1; di++)
					{
						int ni = i + di, nj = j + dj, v;
						if ((di == 0 && dj == 0) || ni < 0 || nj < 0 || ni >= n[0] || nj >= n[1])
							continue;
						if (!colfloored[nj * n[0] + ni])
							continue;
						v = data[(((size_t)k * n[1] + nj) * n[0] + ni) * 4];
						if (best < 0 || v < best)
							best = v;
					}
				}
				if (best >= 0)
					data[(((size_t)k * n[1] + j) * n[0] + i) * 4] = (unsigned char)best;
			}
		}
	}
	Mem_Free(colfloored);

	R_Volumetric_BlurEnclosure(data, scratch, n);

	// TEXF_ALPHA is NOT optional: TEXTYPE_RGBA without it resolves to internal format
	// GL_RGB, so the enclosure channel would be dropped on upload and every sample
	// would read back a.=1 -- a flat density multiplier instead of a corner term.
	r_volumetric_fieldtexture = R_LoadTexture3D(r_main_texturepool, "volumetricfield",
		n[0], n[1], n[2], data, TEXTYPE_RGBA, TEXF_FORCELINEAR | TEXF_CLAMP | TEXF_ALPHA, 0, NULL);
	Mem_Free(scratch);
	Mem_Free(probefloor);
	if (!r_volumetric_fieldtexture)
	{
		Mem_Free(data);
		return NULL;
	}
	r_volumetric_fielddata = data;   // handed over, freed by R_Volumetric_FreeField
	r_volumetric_fieldsize[0] = n[0];
	r_volumetric_fieldsize[1] = n[1];
	r_volumetric_fieldsize[2] = n[2];
	r_volumetric_fieldmodel = world;
	r_volumetric_fieldcellbuilt = cellsize;
#ifdef USE_RT_METAL
	// hand the freshly-baked field to the fog kernel (copied); re-fires on every
	// rebake, and RT_Metal_SetWorld drops the old copy on map change
	RT_Metal_SetFogField(r_volumetric_fielddata, n, r_volumetric_fieldorigin, r_volumetric_fieldinvsize, sdfrange, VOL_FIELD_MAXH);
#endif
	// plain print, not DPrintf: the brief's rule is that anything baked at load
	// prints its timing, once (flipped from Con_DPrintf in the fog-bake round)
	Con_Printf("volumetric field: %dx%dx%d cells (%.0f/%.0f/%.0f units), %.0f KB, baked in %.0f ms\n",
		n[0], n[1], n[2], cell[0], cell[1], cell[2], bytes / 1024.0,
		(Sys_DirtyTime() - starttime) * 1000.0);
	return r_volumetric_fieldtexture;
}

/*
================
R_Volumetric_GetIrradianceGrid

The murk's ambient light source: a coarse RGB grid of the level's STATIC
lighting, one R_CompleteLightPoint(LP_LIGHTMAP) per open cell centre. The
recorded rejection of the lightmap as a source for RT light COLOUR (monochrome
on id1, no direction, samples the surface below the point) does not apply
here: ambient BRIGHTNESS is exactly what a monochrome lightmap gives, hue
stays with the murk's authored colours, and "open air inherits its floor's
brightness" is a perfectly good definition of how bright the fog above that
floor should read. Dynamic lights are deliberately absent -- they are the
march loop's and the fog kernel's job, per frame.

Values are stored at 128 = fully lit (the engine's own overbright convention,
headroom to 2.0); the shader multiplies by 2 to undo it. Solid cells inherit
the BRIGHTEST open neighbour, two passes deep, because trilinear filtering
near a wall mixes up to two cells into the wall and an unfilled black cell
there would darken the fog against every surface. No blur: trilinear over
128-unit cells is already smoother than any light pool the lightmap resolves.
================
*/
static rtexture_t *R_Volumetric_GetIrradianceGrid(void)
{
	model_t *world = r_refdef.scene.worldmodel;
	unsigned char *data, *openmask;
	size_t irrhist[256], irropen = 0;   // the calibration witness: open-air cells only
	float mins[3], maxs[3], cell[3], cellsize;
	int n[3], i, j, k, q, pass;
	double starttime;
	size_t bytes, total;

	if (!world || !world->PointSuperContents)
		return NULL;
	cellsize = bound(32.0f, r_volumetric_irrcell.value, 512.0f);
	if (r_volumetric_irrtexture && r_volumetric_irrmodel == world
	 && r_volumetric_irrcellbuilt == cellsize
	 && r_volumetric_irrdilatebuilt == (r_volumetric_ambientdilate.integer != 0))
		return r_volumetric_irrtexture;
	R_Volumetric_FreeIrradiance();

	starttime = Sys_DirtyTime();

	// one cell of margin, exactly as the field does, so the trilinear filter has
	// real data at the world's edge
	for (i = 0; i < 3; i++)
	{
		mins[i] = world->normalmins[i] - cellsize;
		maxs[i] = world->normalmaxs[i] + cellsize;
		n[i] = (int)ceil((maxs[i] - mins[i]) / cellsize);
	}
	n[0] = bound(2, n[0], VOL_IRR_MAXXY);
	n[1] = bound(2, n[1], VOL_IRR_MAXXY);
	n[2] = bound(2, n[2], VOL_IRR_MAXZ);
	for (i = 0; i < 3; i++)
	{
		cell[i] = (maxs[i] - mins[i]) / n[i];
		r_volumetric_irrorigin[i] = mins[i];
		r_volumetric_irrinvsize[i] = 1.0f / (maxs[i] - mins[i]);
	}

	total = (size_t)n[0] * n[1] * n[2];
	bytes = total * 4;
	data = (unsigned char *)Mem_Alloc(tempmempool, bytes);
	openmask = (unsigned char *)Mem_Alloc(tempmempool, total);
	if (!data || !openmask)
	{
		if (data) Mem_Free(data);
		if (openmask) Mem_Free(openmask);
		return NULL;
	}

	for (k = 0; k < n[2]; k++)
	{
		for (j = 0; j < n[1]; j++)
		{
			for (i = 0; i < n[0]; i++)
			{
				size_t idx = ((size_t)k * n[1] + j) * n[0] + i;
				unsigned char *p = data + idx * 4;
				vec3_t centre;
				float amb[3], dif[3], dir[3];
				centre[0] = mins[0] + (i + 0.5f) * cell[0];
				centre[1] = mins[1] + (j + 0.5f) * cell[1];
				centre[2] = mins[2] + (k + 0.5f) * cell[2];
				p[3] = 255;   // A unread by design (no TEXF_ALPHA; GL_RGB on GL)
				if (world->PointSuperContents(world, 0, centre) & (SUPERCONTENTS_SOLID | SUPERCONTENTS_SKY))
				{
					openmask[idx] = 0;
					continue;   // filled from open neighbours below
				}
				openmask[idx] = 1;
				// lightmapintensity 1, ambientintensity 0: the grid carries the
				// map's AUTHORED values; runtime brightness knobs stay runtime
				R_CompleteLightPoint(amb, dif, dir, centre, LP_LIGHTMAP, 1.0f, 0.0f);
				for (q = 0; q < 3; q++)
				{
					// amb + 0.25*dif is the particle lighting convention (the
					// murk should read as lit like the dust in the same air)
					float v = (amb[q] + 0.25f * dif[q]) * 127.5f;
					p[q] = (unsigned char)bound(0, (int)v, 255);
				}
			}
		}
	}

	// two max-dilate passes: solid cells take their brightest open (or
	// previously-filled) neighbour over the full 26-neighbourhood, so the two
	// cell layers trilinear filtering can reach inside a wall carry the light
	// of the air beside them rather than black
	for (pass = 0; pass < 2; pass++)
	{
		for (k = 0; k < n[2]; k++)
		{
			for (j = 0; j < n[1]; j++)
			{
				for (i = 0; i < n[0]; i++)
				{
					size_t idx = ((size_t)k * n[1] + j) * n[0] + i;
					int di, dj, dk, bestlum = -1;
					size_t bestidx = 0;
					float wsum = 0, wacc[3] = {0, 0, 0};
					if (openmask[idx])
						continue;
					for (dk = -1; dk <= 1; dk++)
					{
						for (dj = -1; dj <= 1; dj++)
						{
							for (di = -1; di <= 1; di++)
							{
								int ni = i + di, nj = j + dj, nk = k + dk;
								size_t nidx;
								int lum;
								if ((di | dj | dk) == 0 || ni < 0 || nj < 0 || nk < 0 || ni >= n[0] || nj >= n[1] || nk >= n[2])
									continue;
								nidx = ((size_t)nk * n[1] + nj) * n[0] + ni;
								if (!openmask[nidx])
									continue;
								lum = data[nidx * 4] + data[nidx * 4 + 1] + data[nidx * 4 + 2];
								{
									// face neighbours 1, edges 1/2, corners 1/3
									float w = 1.0f / (float)(di * di + dj * dj + dk * dk);
									wsum += w;
									for (q = 0; q < 3; q++)
										wacc[q] += w * data[nidx * 4 + q];
								}
								if (lum > bestlum)
								{
									bestlum = lum;
									bestidx = nidx;
								}
							}
						}
					}
					if (bestlum >= 0)
					{
						// THE MEAN, NOT THE MAX (2026-09-19). Only about one cell in
						// twenty of this grid is open air, so almost every trilinear
						// sample in a corridor mixes fill cells in -- and a fill that
						// takes the brightest of 26 neighbours, twice over, hands a
						// dark passage the light of the room next door. Measured on
						// five id1 maps: open air sits at a median byte of 64-96
						// while the whole grid sat at 148, past the point where the
						// murk's multiplier clamps at 1, so the fog read fully lit
						// nearly everywhere. The mean still keeps black out of the
						// walls, which is all the fill was ever for.
						for (q = 0; q < 3; q++)
							data[idx * 4 + q] = r_volumetric_ambientdilate.integer
								? (unsigned char)bound(0, (int)(wacc[q] / wsum + 0.5f), 255)
								: data[bestidx * 4 + q];
						openmask[idx] = 2;   // filled; promoted after the sweep
					}
				}
			}
		}
		// promote this pass's fills so the second pass can spread from them
		for (total = (size_t)n[0] * n[1] * n[2], i = 0; (size_t)i < total; i++)
			if (openmask[i] == 2)
				openmask[i] = 3;   // nonzero like 1, but the witness below can still tell air from fill
	}
	// the witness reads OPEN AIR alone: dilated wall cells copy their brightest
	// neighbour and cells the dilate never reached are black, and both would
	// bend the percentiles of the thing the murk actually samples
	memset(irrhist, 0, sizeof irrhist);
	irropen = 0;
	for (total = (size_t)n[0] * n[1] * n[2], i = 0; (size_t)i < total; i++)
	{
		if (openmask[i] == 1)
		{
			int l = (int)(0.2126f * data[(size_t)i * 4] + 0.7152f * data[(size_t)i * 4 + 1] + 0.0722f * data[(size_t)i * 4 + 2]);
			irrhist[bound(0, l, 255)]++;
			irropen++;
		}
	}
	Mem_Free(openmask);

	// no TEXF_ALPHA on purpose: A is never read, so GL's silent GL_RGB
	// resolution is harmless here (the noise-volume precedent, not the field's)
	r_volumetric_irrtexture = R_LoadTexture3D(r_main_texturepool, "volumetricirradiance",
		n[0], n[1], n[2], data, TEXTYPE_RGBA, TEXF_FORCELINEAR | TEXF_CLAMP, 0, NULL);
	if (!r_volumetric_irrtexture)
	{
		Mem_Free(data);
		return NULL;
	}
	r_volumetric_irrdata = data;   // handed over, freed by R_Volumetric_FreeIrradiance
	r_volumetric_irrsize[0] = n[0];
	r_volumetric_irrsize[1] = n[1];
	r_volumetric_irrsize[2] = n[2];
	r_volumetric_irrmodel = world;
	r_volumetric_irrcellbuilt = cellsize;
	r_volumetric_irrdilatebuilt = (r_volumetric_ambientdilate.integer != 0);
#ifdef USE_RT_METAL
	// hand the grid to the fog kernel (copied); re-fires on every rebake, and the
	// sidecar drops its copy with the field on world change
	RT_Metal_SetFogIrradiance(r_volumetric_irrdata, n);
#endif
	// THE CALIBRATION WITNESS (2026-09-19). The murk's ambient multiplier is
	// min(ambientfloor + ambientgain * 2 * irr, 1), so it stops darkening
	// anything the moment a cell's stored luma passes (1 - floor)/(2*gain).
	// At the shipped 0.12 / 1.5 that is byte 75 of 255 -- and if a map's
	// grid sits above it the whole feature is inert while looking perfectly
	// plumbed (measured on e1m3: per-texel multiplier 1.00 at the median).
	// One line at bake time turns "is the fog taking the room's light?" into
	// a number instead of an argument; developer-only, once per bake.
	if (developer.integer && irropen)
	{
		double satbyte = (1.0 - bound(0.0f, r_volumetric_ambientfloor.value, 1.0f))
		               / (2.0 * max(1.0f / 255.0f, r_volumetric_ambientgain.value)) * 255.0;
		size_t acc = 0, below = 0, pc[3] = {0, 0, 0};
		int want[3] = {5, 50, 95}, wi = 0;
		for (q = 0; q < 256; q++)
		{
			if (q < satbyte)
				below += irrhist[q];
			acc += irrhist[q];
			while (wi < 3 && acc * 100 >= irropen * (size_t)want[wi])
				pc[wi++] = q;
		}
		Con_DPrintf("volumetric irradiance: open-air cell luma p5/p50/p95 = %d/%d/%d of 255 (%d cells); %.1f%% below the darkening knee (byte %.0f at gain %.2f floor %.2f)\n",
			(int)pc[0], (int)pc[1], (int)pc[2], (int)irropen, 100.0 * below / (double)irropen, satbyte,
			r_volumetric_ambientgain.value, r_volumetric_ambientfloor.value);
		// ...and what the murk actually SAMPLES, which is the number that
		// matters: trilinear reads at scattered open-air points, through the
		// same multiplier the shaders apply
		{
			unsigned int seed = 12345u, tries, got = 0, sat = 0;
			size_t mh[101];
			memset(mh, 0, sizeof mh);
			for (tries = 0; tries < 20000 && got < 4000; tries++)
			{
				vec3_t pt, irr;
				float mult;
				for (q = 0; q < 3; q++)
				{
					seed = seed * 1664525u + 1013904223u;
					pt[q] = mins[q] + (maxs[q] - mins[q]) * ((seed >> 8) / 16777216.0f);
				}
				if (world->PointSuperContents(world, 0, pt) & (SUPERCONTENTS_SOLID | SUPERCONTENTS_SKY | SUPERCONTENTS_LIQUIDSMASK))
					continue;
				if (!R_Volumetric_SampleIrradiance(pt, irr))
					break;
				mult = r_volumetric_ambientfloor.value + r_volumetric_ambientgain.value * (0.2126f * irr[0] + 0.7152f * irr[1] + 0.0722f * irr[2]);
				if (mult >= 1.0f)
					sat++;
				mh[(int)(bound(0.0f, mult, 1.0f) * 100.0f)]++;
				got++;
			}
			if (got)
			{
				size_t a2 = 0;
				int w2 = 0, pcs[3] = {0, 0, 0};
				for (q = 0; q <= 100; q++)
				{
					a2 += mh[q];
					while (w2 < 3 && a2 * 100 >= (size_t)got * (size_t)want[w2])
						pcs[w2++] = q;
				}
				Con_DPrintf("volumetric irradiance: the murk's ambient multiplier at %u open-air points p5/p50/p95 = %.2f/%.2f/%.2f, %.1f%% clamped at fully lit (dilate %d)\n",
					got, pcs[0] * 0.01, pcs[1] * 0.01, pcs[2] * 0.01, 100.0 * sat / (double)got, r_volumetric_ambientdilate.integer != 0);
			}
		}
	}
	// plain print, not DPrintf: the brief's rule is that anything baked at load
	// prints its timing, once
	Con_Printf("volumetric irradiance: %dx%dx%d cells (%.0f units), %.0f KB, baked in %.0f ms\n",
		n[0], n[1], n[2], cell[0], bytes / 1024.0,
		(Sys_DirtyTime() - starttime) * 1000.0);
	return r_volumetric_irrtexture;
}

/*
================
R_Volumetric_ResolveIrradiance / R_Volumetric_SampleIrradiance

SEPTEMBER S7 (2026-09-02): the CPU side of the irradiance grid, for lit
particles (cl_particles_lighting 2). The grid was built for the murk and bakes
lazily at its first use; particles read it through this pair so that the bake
site is the caller's choice (R_DrawParticles, between passes) and never a draw
batch. Deliberately NOT resolved at the map-load site R_Volumetric_ResolveField
uses for the shimmer's field: the bake reads r_refdef.scene.worldmodel through
R_CompleteLightPoint, and at that site the pointer still names the PREVIOUS map
(it is assigned in CL_UpdateWorld, cl_main.c), so a grid baked there would carry
the wrong lightmap under the right cache key. The sampler mirrors
R_Volumetric_SampleField: texel centres at (i+0.5)/n, clamped, RGB only, and it
undoes the 128 = fully lit storage convention (the shader's *2.0).
================
*/
qbool R_Volumetric_ResolveIrradiance(void)
{
	if (!cl.worldmodel || r_refdef.scene.worldmodel != cl.worldmodel)
		return false;
	return R_Volumetric_GetIrradianceGrid() != NULL && r_volumetric_irrdata != NULL;
}

qbool R_Volumetric_SampleIrradiance(const vec3_t p, vec3_t out)
{
	float uvw[3];
	int i, base[3];

	if (!r_volumetric_irrdata || r_volumetric_irrmodel != r_refdef.scene.worldmodel)
		return false;

	for (i = 0; i < 3; i++)
	{
		float t = (p[i] - r_volumetric_irrorigin[i]) * r_volumetric_irrinvsize[i] * r_volumetric_irrsize[i] - 0.5f;
		t = bound(0.0f, t, (float)(r_volumetric_irrsize[i] - 1));
		base[i] = (int)t;
		if (base[i] > r_volumetric_irrsize[i] - 2)
			base[i] = max(0, r_volumetric_irrsize[i] - 2);
		uvw[i] = t - base[i];
	}
	for (i = 0; i < 3; i++)
	{
		int ox, oy, oz;
		float acc = 0.0f;
		for (oz = 0; oz < 2; oz++)
		{
			for (oy = 0; oy < 2; oy++)
			{
				for (ox = 0; ox < 2; ox++)
				{
					int cx = min(base[0] + ox, r_volumetric_irrsize[0] - 1);
					int cy = min(base[1] + oy, r_volumetric_irrsize[1] - 1);
					int cz = min(base[2] + oz, r_volumetric_irrsize[2] - 1);
					float w = (ox ? uvw[0] : 1.0f - uvw[0]) * (oy ? uvw[1] : 1.0f - uvw[1]) * (oz ? uvw[2] : 1.0f - uvw[2]);
					acc += w * r_volumetric_irrdata[(((size_t)cz * r_volumetric_irrsize[1] + cy) * r_volumetric_irrsize[0] + cx) * 4 + i];
				}
			}
		}
		out[i] = acc * (2.0f / 255.0f);   // stored at 127.5 = fully lit
	}
	return true;
}

/*
================
R_Volumetric_Probe_f

"r_volumetric_probe" -- prints what the murk actually sees at the eye, trilinearly
sampled from the same field the shader reads, next to the engine's own contents at
the same point. Every "it does not look right here" report reduces to whether the
field agrees with the world, and this is the only way to see that directly.
================
*/
// Trilinear fetch of the baked field at an arbitrary world point, matching GL's
// normalised 3D fetch exactly: texel centres sit at (i+0.5)/n. Returns 0 when
// there is no field to read. The caller gets the four channels in 0..1 --
// R height above the local floor (x VOL_FIELD_MAXH), G signed liquid distance
// (0.5 = surface), B liquid kind, A enclosure.
static int R_Volumetric_SampleField(const vec3_t p, float f[4])
{
	float uvw[3];
	int i, base[3];

	if (!r_volumetric_fielddata)
		return 0;

	for (i = 0; i < 3; i++)
	{
		float t = (p[i] - r_volumetric_fieldorigin[i]) * r_volumetric_fieldinvsize[i] * r_volumetric_fieldsize[i] - 0.5f;
		t = bound(0.0f, t, (float)(r_volumetric_fieldsize[i] - 1));
		base[i] = (int)t;
		if (base[i] > r_volumetric_fieldsize[i] - 2)
			base[i] = max(0, r_volumetric_fieldsize[i] - 2);
		uvw[i] = t - base[i];
	}
	for (i = 0; i < 4; i++)
	{
		int ox, oy, oz;
		float acc = 0.0f;
		for (oz = 0; oz < 2; oz++)
		{
			for (oy = 0; oy < 2; oy++)
			{
				for (ox = 0; ox < 2; ox++)
				{
					int cx = min(base[0] + ox, r_volumetric_fieldsize[0] - 1);
					int cy = min(base[1] + oy, r_volumetric_fieldsize[1] - 1);
					int cz = min(base[2] + oz, r_volumetric_fieldsize[2] - 1);
					float w = (ox ? uvw[0] : 1.0f - uvw[0]) * (oy ? uvw[1] : 1.0f - uvw[1]) * (oz ? uvw[2] : 1.0f - uvw[2]);
					acc += w * r_volumetric_fielddata[(((size_t)cz * r_volumetric_fieldsize[1] + cy) * r_volumetric_fieldsize[0] + cx) * 4 + i];
				}
			}
		}
		f[i] = acc * (1.0f / 255.0f);
	}
	return 1;
}

// The FIELD-DRIVEN part of the murk density at a sampled point -- everything the
// CPU can see. This is NOT the full model: the shader multiplies the air murk by
// a noise `patch` term and the ground layer by its own `gpatch`, and both average
// well below 1, so this reads as an UPPER BOUND on what the march actually
// integrates. It is deliberately not a third lockstep copy; it exists to show the
// deterministic vertical SHAPE, which is what the height knobs control.
// The authoritative pair remains shader_glsl.h MODE_VOLUMETRICFOG and rt_metal.m
// kFogSrc -- keep the term list here in step with them, but never the noise.
static float R_Volumetric_FieldDensity(const float f[4], float *out_abovefloor)
{
	float abovefloor, sdf, density;

	abovefloor = f[0] * VOL_FIELD_MAXH - r_volumetric_flooroffset.value;
	sdf = (f[1] - 0.5f) * 2.0f * r_volumetric_fieldsdfrange;
	// LIQUID FLOOR, LOCKSTEP with shader_density.h DPD_DENSITY_MODEL and the kernel
	if (r_volumetric_liquidfloor.value > 0.0f)
	{
		float k = bound(0.0f, r_volumetric_liquidfloor.value, 1.0f);
		abovefloor = min(abovefloor, abovefloor + (max(-sdf, 0.0f) - abovefloor) * k);
	}
	if (out_abovefloor)
		*out_abovefloor = abovefloor;

	if (sdf > 0.0f && r_volumetric_water.integer)
		// per-kind, LOCKSTEP with shader_density.h's liqdens and rt_metal.m's
		// kFogSrc: f[2] is the field's B channel, the nearest span's kind
		return f[2] < 0.25f ? R_Volumetric_WaterDensityView()
		     : (f[2] < 0.75f ? R_Volumetric_SlimeDensity() : R_Volumetric_LavaDensity());

	density = r_volumetric_density.value * exp(-max(abovefloor, 0.0f) / max(1.0f, r_volumetric_height.value));
	density *= 1.0f + max(0.0f, r_volumetric_corner.value) * f[3] * f[3];
	if (r_volumetric_ground.value > 0.0f && r_volumetric_floor.integer)
	{
		float gabove = abovefloor - r_volumetric_groundoffset.value;
		density += r_volumetric_ground.value * exp(-max(gabove, 0.0f) / max(1.0f, r_volumetric_groundheight.value));
	}
	density += r_volumetric_watermist.value * exp(-max(-sdf, 0.0f) / max(1.0f, r_volumetric_mistheight.value));
	return density;
}

/*
================
R_Volumetric_TransmittanceToPoint

How much of a point's own light survives the murk between it and the eye, and the
colour the murk fades it towards. Returns 1 (leaving the tint untouched) whenever
the murk is not running, so callers can apply it unconditionally.

This exists because the murk is a SCREEN-SPACE pass, composited in R_RenderScene
BEFORE the transparent queue is flushed. Particles, blood, explosion shells and
sprites are all drawn after that and write no depth, so nothing in the murk pass
can ever attenuate them: they read bright and clear through fog thick enough to
hide the wall behind them. Fading them per-point on the CPU is the only hook the
ordering allows, and it is where the engine's classic fog has always done it
(RSurf_FogVertex in the particle callback).

Deliberately a CHEAP approximation, NOT a third lockstep copy of the density
model: three Simpson-weighted taps of the field-driven density (see
R_Volumetric_FieldDensity) between the eye and the point. One tap at the point
alone is wrong for any long sightline, and a full march per particle is not
affordable. It ignores the noise `patch` terms because the noise volume lives
only on the GPU, which OVER-estimates the density -- the noise breaks the murk
into banks and only the covered fraction really attenuates. r_volumetric_particles
scales the optical depth and is the taste control for exactly that.
================
*/
float R_Volumetric_TransmittanceToPoint(const vec3_t p, vec3_t out_tint)
{
	vec3_t mid;
	float f0[4], f1[4], f2[4], dist, tau, sdf;

	if (!r_fb.volumetricactive || !r_volumetric_fielddata || r_volumetric_particles.value <= 0.0f)
		return 1.0f;

	dist = VectorDistance(r_refdef.view.origin, p);
	if (dist < 1.0f)
		return 1.0f;

	mid[0] = (r_refdef.view.origin[0] + p[0]) * 0.5f;
	mid[1] = (r_refdef.view.origin[1] + p[1]) * 0.5f;
	mid[2] = (r_refdef.view.origin[2] + p[2]) * 0.5f;
	if (!R_Volumetric_SampleField(r_refdef.view.origin, f0)
	 || !R_Volumetric_SampleField(mid, f1)
	 || !R_Volumetric_SampleField(p, f2))
		return 1.0f;

	// the march accumulates exp(-density * dt * 0.01) per step, so the optical
	// depth over a segment is 0.01 * mean density * length -- same 0.01 as the
	// shader, and the same convention behind the probe's "obscures by half at
	// ~69.3/density units"
	tau = (R_Volumetric_FieldDensity(f0, NULL)
	     + R_Volumetric_FieldDensity(f1, NULL) * 4.0f
	     + R_Volumetric_FieldDensity(f2, NULL)) * (1.0f / 6.0f)
	     * dist * 0.01f * r_volumetric_particles.value;

	if (out_tint)
	{
		// the colour a fully obscured point fades to, chosen at the point the way
		// the shader chooses it: the liquid's own colour inside a liquid, else the
		// air murk shading towards the ground layer's colour by its share
		sdf = (f2[1] - 0.5f) * 2.0f * r_volumetric_fieldsdfrange;
		if (sdf > 0.0f && r_volumetric_water.integer)
		{
			if (f2[2] < 0.25f)
				VectorSet(out_tint, r_volumetric_watercolor_red.value, r_volumetric_watercolor_green.value, r_volumetric_watercolor_blue.value);
			else if (f2[2] < 0.75f)
				VectorSet(out_tint, r_volumetric_slimecolor_red.value, r_volumetric_slimecolor_green.value, r_volumetric_slimecolor_blue.value);
			else
				VectorSet(out_tint, r_volumetric_lavacolor_red.value, r_volumetric_lavacolor_green.value, r_volumetric_lavacolor_blue.value);
		}
		else
		{
			float above = f2[0] * VOL_FIELD_MAXH - r_volumetric_flooroffset.value;
			float total = R_Volumetric_FieldDensity(f2, NULL);
			float groundshare = 0.0f;
			if (r_volumetric_ground.value > 0.0f && r_volumetric_floor.integer && total > 1e-4f)
				groundshare = bound(0.0f, r_volumetric_ground.value
					* exp(-max(above - r_volumetric_groundoffset.value, 0.0f) / max(1.0f, r_volumetric_groundheight.value)) / total, 1.0f);
			out_tint[0] = r_volumetric_color_red.value   + (r_volumetric_groundcolor_red.value   - r_volumetric_color_red.value)   * groundshare;
			out_tint[1] = r_volumetric_color_green.value + (r_volumetric_groundcolor_green.value - r_volumetric_color_green.value) * groundshare;
			out_tint[2] = r_volumetric_color_blue.value  + (r_volumetric_groundcolor_blue.value  - r_volumetric_color_blue.value)  * groundshare;
		}
	}
	return exp(-tau);
}

static void R_Volumetric_Probe_f(cmd_state_t *cmd)
{
	vec3_t p;
	float f[4], sdf, abovefloor, basez;
	int i, contents;
	const char *kindname;

	if (!R_Volumetric_GetField() || !r_volumetric_fielddata)
	{
		Con_Printf("volumetric probe: no field (need r_volumetric 1 and a loaded map)\n");
		return;
	}

	// "r_volumetric_probe" reads at the eye -- which doubles as this engine's
	// missing `viewpos`, since the header line prints the coordinates.
	// "r_volumetric_probe x y z" aims it anywhere, which is the only way to
	// interrogate a point you cannot stand in (inside a wall, say).
	if (Cmd_Argc(cmd) >= 4)
	{
		p[0] = atof(Cmd_Argv(cmd, 1));
		p[1] = atof(Cmd_Argv(cmd, 2));
		p[2] = atof(Cmd_Argv(cmd, 3));
	}
	else if (Cmd_Argc(cmd) != 1)
	{
		Con_Printf("usage: r_volumetric_probe [x y z]   (no arguments probes at the eye)\n");
		return;
	}
	else
		VectorCopy(r_refdef.view.origin, p);

	R_Volumetric_SampleField(p, f);
	sdf = (f[1] - 0.5f) * 2.0f * r_volumetric_fieldsdfrange;
	kindname = f[2] < 0.25f ? "water" : (f[2] < 0.75f ? "slime" : "lava");

	contents = cl.worldmodel && cl.worldmodel->PointSuperContents
	         ? cl.worldmodel->PointSuperContents(cl.worldmodel, 0, p) : 0;

	Con_Printf("volumetric probe at %.0f %.0f %.0f  (field %dx%dx%d, %.0f units/cell)\n",
		p[0], p[1], p[2], r_volumetric_fieldsize[0], r_volumetric_fieldsize[1], r_volumetric_fieldsize[2],
		r_volumetric_fieldcellbuilt);
	Con_Printf("  engine says   : %s\n",
		(contents & SUPERCONTENTS_LAVA) ? "LAVA" : (contents & SUPERCONTENTS_SLIME) ? "SLIME"
		: (contents & SUPERCONTENTS_WATER) ? "WATER" : (contents & SUPERCONTENTS_SOLID) ? "SOLID" : "empty air");
	Con_Printf("  field says    : %s (signed distance %+.0f units, nearest liquid is %s)\n",
		sdf > 0.0f ? "IN LIQUID" : "not in liquid", sdf, kindname);
	Con_Printf("  height above local floor : %.0f units\n", f[0] * VOL_FIELD_MAXH);
	if (r_volumetric_liquidfloor.value > 0.0f && sdf < 0.0f && -sdf < f[0] * VOL_FIELD_MAXH)
		Con_Printf("  liquid floor            : the nearest liquid is %.0f units away, so the murk uses THAT as its floor (r_volumetric_liquidfloor %.2f)\n", -sdf, r_volumetric_liquidfloor.value);
	Con_Printf("  enclosure / corner term  : %.2f -> murk x%.2f\n",
		f[3], 1.0f + max(0.0f, r_volumetric_corner.value) * f[3] * f[3]);
	Con_Printf("  murk density here        : %.2f  (obscures by half at ~%.0f units)\n",
		sdf > 0.0f ? R_Volumetric_WaterDensityView() : r_volumetric_density.value * (1.0f + max(0.0f, r_volumetric_corner.value) * f[3] * f[3]),
		69.3f / max(0.001f, sdf > 0.0f ? R_Volumetric_WaterDensityView() : r_volumetric_density.value));

	// VERTICAL PROFILE. The one question a single reading cannot answer: does
	// the murk actually settle on the floor, or is it a slab floating at knee
	// height? Walk up from the local floor under this column and print the
	// field-driven density at each step. Noise-free (see R_Volumetric_FieldDensity),
	// so these are upper bounds -- but the SHAPE is what the height knobs own.
	R_Volumetric_FieldDensity(f, &abovefloor);
	basez = p[2] - abovefloor;
	Con_Printf("  vertical profile above the local floor (z = %.0f), noise-free:\n", basez);
	for (i = 0; i < 9; i++)
	{
		static const float heights[9] = {0, 8, 16, 24, 32, 48, 64, 96, 144};
		vec3_t q;
		float qf[4], qabove, qdensity;
		VectorSet(q, p[0], p[1], basez + heights[i]);
		if (!R_Volumetric_SampleField(q, qf))
			break;
		qdensity = R_Volumetric_FieldDensity(qf, &qabove);
		Con_Printf("    +%3.0f units (z %6.0f): field says above=%4.0f  density %.3f%s\n",
			heights[i], q[2], qabove, qdensity,
			((qf[1] - 0.5f) * 2.0f * r_volumetric_fieldsdfrange) > 0.0f ? "  [in liquid]" : "");
	}
}

static void R_Volumetric_ParseVec3(const char *s, float *out, float dx, float dy, float dz);

#ifdef USE_RT_METAL
/*
================
R_Volumetric_GetFogKernelParams

Fills the Metal fog kernel's density-model parameters with EXACTLY the values the
GL murk's uniforms would carry this frame, clamps included -- the kernel and the
shader evaluate the same model and must see the same numbers. floormode/watermode
carry only the cvar intent here; the sidecar zeroes them itself when it holds no
field, mirroring the GL "field && cvar" gate.
================
*/
qbool M5_MapFogColor(float *out);	// below, next to R_Volumetric_Active

void R_Volumetric_GetFogKernelParams(rt_fog_shade_t *out)
{
	float wind[3];
	R_Volumetric_ParseVec3(r_volumetric_wind.string, wind, 10.0f, 4.0f, 1.5f);
	out->windoffset[0] = wind[0] * cl.time;
	out->windoffset[1] = wind[1] * cl.time;
	out->windoffset[2] = wind[2] * cl.time;
	VectorSet(out->color,      r_volumetric_color_red.value,      r_volumetric_color_green.value,      r_volumetric_color_blue.value);
	M5_MapFogColor(out->color);	// a map's own fog colour wins; lockstep with the GL march below
	VectorSet(out->watercolor, r_volumetric_watercolor_red.value, r_volumetric_watercolor_green.value, r_volumetric_watercolor_blue.value);
	VectorSet(out->slimecolor, r_volumetric_slimecolor_red.value, r_volumetric_slimecolor_green.value, r_volumetric_slimecolor_blue.value);
	VectorSet(out->lavacolor,  r_volumetric_lavacolor_red.value,  r_volumetric_lavacolor_green.value,  r_volumetric_lavacolor_blue.value);
	VectorCopy(r_volumetric_fieldorigin, out->fieldorigin);
	VectorCopy(r_volumetric_fieldinvsize, out->fieldinvsize);
	out->density      = r_volumetric_density.value;
	out->height       = max(1.0f, r_volumetric_height.value);
	out->basez        = r_refdef.view.origin[2] + r_volumetric_heightbase.value;
	out->dist         = max(1.0f, r_volumetric_dist.value);
	out->noisescale   = r_volumetric_noisescale.value;
	out->noisethresh  = bound(0.0f, r_volumetric_noisethresh.value, 0.95f);
	out->waterdensity = R_Volumetric_WaterDensityView();
	out->slimedensity = R_Volumetric_SlimeDensity();
	out->lavadensity  = R_Volumetric_LavaDensity();
	out->watermode    = r_volumetric_water.integer ? 1.0f : 0.0f;
	out->flooroffset  = r_volumetric_flooroffset.value;
	out->floormode    = r_volumetric_floor.integer ? 1.0f : 0.0f;
	// folded mode gates, matching the GL murk's uniform upload exactly: with
	// r_volumetric_water 0 the GL shows no surface mist and with _floor 0 no
	// corner gathering, so the kernel must not either (the sidecar's own gate
	// covers only the missing-field case). This kept kernel-vs-GL toggling
	// honest in the water-off/floor-off configurations.
	out->watermist    = r_volumetric_water.integer ? max(0.0f, r_volumetric_watermist.value) : 0.0f;
	out->mistheight   = max(1.0f, r_volumetric_mistheight.value);
	out->sdfrange     = max(1.0f, r_volumetric_fieldsdfrange);
	out->corner       = r_volumetric_floor.integer ? max(0.0f, r_volumetric_corner.value) : 0.0f;
	out->fieldmaxh    = VOL_FIELD_MAXH;
	// GROUND FOG: clamps identical to the GL uniform upload (the honesty rule);
	// density carries the floor-cvar gate, the sidecar adds the live-field gate
	{
		float groundwind[3];
		R_Volumetric_ParseVec3(r_volumetric_groundwind.string, groundwind, 4.0f, 1.7f, 0.0f);
		out->groundwindoffset[0] = groundwind[0] * cl.time;
		out->groundwindoffset[1] = groundwind[1] * cl.time;
		out->groundwindoffset[2] = groundwind[2] * cl.time;
		VectorSet(out->groundcolor, r_volumetric_groundcolor_red.value, r_volumetric_groundcolor_green.value, r_volumetric_groundcolor_blue.value);
		out->grounddensity    = r_volumetric_floor.integer ? max(0.0f, r_volumetric_ground.value) : 0.0f;
		out->groundheight     = max(1.0f, r_volumetric_groundheight.value);
		out->groundnoisescale = r_volumetric_groundnoisescale.value;
		out->groundthresh     = bound(0.0f, r_volumetric_groundthresh.value, 0.95f);
		// LAVA GLOW: clamp identical to the GL upload (VolumetricGround2.z)
		out->lavaglow         = r_volumetric_water.integer ? max(0.0f, r_volumetric_lavaglow.value) : 0.0f;
		out->grounddeform     = max(0.0f, r_volumetric_grounddeform.value);
		out->groundoffset     = r_volumetric_groundoffset.value;
		// SKY FOG CAP: clamp identical to the GL upload (VolumetricGround2.w)
		out->skytrans         = 1.0f - bound(0.0f, r_volumetric_skyfog.value, 1.0f);
		// AMBIENT IRRADIANCE: clamps identical to the GL upload (VolumetricIrr).
		// Strength carries only the cvar here; the sidecar adds its own live-grid
		// gate (s_irrTex3D), exactly as the field-dependent lanes above do.
		VectorCopy(r_volumetric_irrorigin, out->irrorigin);
		VectorCopy(r_volumetric_irrinvsize, out->irrinvsize);
		out->irrgain          = max(0.0f, r_volumetric_ambientgain.value);
		out->irrfloor         = bound(0.0f, r_volumetric_ambientfloor.value, 1.0f);
		out->irrstrength      = bound(0.0f, r_volumetric_ambient.value, 1.0f);
		// EXTINCTION: clamp identical to the GL upload (MarchLightParams.w)
		out->extinction       = max(0.01f, r_volumetric_extinction.value);
		// KH SWIRL: clamps identical to the GL upload (VolumetricSwirl). The
		// floormode gate on the KH boost lives in the shader (cam.floormode is
		// already field-honesty-gated at the fc fill), so plain copies here.
		out->swirlamp         = max(0.0f, r_volumetric_swirl.value);
		out->swirlscale       = r_volumetric_swirlscale.value;
		out->swirlkh          = max(0.0f, r_volumetric_swirlkh.value);
		out->swirlspare       = 0.0f;
		// MIST-LAVA CUT (F7): appended AFTER the stack's fields at merge -- the
		// append order is the merge's, not either branch's. LOCKSTEP with the
		// struct, the FogCam text, the RTFogCam mirror and the fc fill.
		out->mistlavacut      = bound(0.0f, r_volumetric_mistlavacut.value, 1.0f);
		out->liquidfloor      = bound(0.0f, r_volumetric_liquidfloor.value, 1.0f);
	}
}
#endif

// A partially-supplied value REPEATS its last component rather than falling back to the
// built-in defaults for the rest. The console drops every token after the first of an
// unquoted assignment, so "10 4 1.5" typed without quotes arrives here as "10"; blending
// that with unrelated defaults produces a confident, plausible, wrong answer that is
// almost impossible to diagnose from the screen. Repeating is visibly wrong instead.
static void R_Volumetric_ParseVec3(const char *s, float *out, float dx, float dy, float dz)
{
	out[0] = dx; out[1] = dy; out[2] = dz;
	if (s && *s)
	{
		float a = dx, b = dy, c = dz;
		int n = sscanf(s, "%f %f %f", &a, &b, &c);
		if (n >= 1) { out[0] = a; out[1] = a; out[2] = a; }
		if (n >= 2) { out[1] = b; out[2] = b; }
		if (n >= 3) { out[2] = c; }
	}
}

// True when the murk should be drawn this frame. Requires step 1's depth (which
// r_volumetric forces) and a nonzero density.
// Fill the murk's reprojection uniforms for a consumed RT texture of texw x texh
// (the fog or shaft buffer): the shown trace's camera basis, pre-divided, plus
// the enable flag in .w. Zeroed when reprojection is off or nothing valid is
// shown, which restores the old screen-locked fetch exactly.
/*
================
R_Shader_SetGammaAnalyticUniforms

METAL.md Phase 7-1. Feeds the analytic gamma curve to whichever of the two
shader sites is about to draw. Both sites call this, so the packing exists once
rather than being written out twice and drifting -- the mistake the 4d bloom
round records in its own form.

Packed as three vec4s because that is what both languages want: A = per-channel
inverse gamma with the shared contrast boost in .w, B = per-channel scale,
C = per-channel base. Silent no-op when the permutation did not compile the arm
in, which is the ordinary `loc < 0` shape every other uniform here uses.
================
*/
/*
================
R_GammaAnalyticTest_f

METAL.md Phase 7-1's instrument, and it is the reason the analytic curve can be
believed rather than merely reasoned about. THREE implementations of one formula
exist in this tree and this command measures all three against each other at 256
points spanning the input range:

  1. BuildGammaTable16    (palette.c)     -- the shipped LUT, 256 entries
  2. VID_ApplyGammaToColor(vid_shared.c)  -- the CPU analytic form
  3. the shader arm       (USEGAMMAANALYTIC) -- what Phase 7 adds

It compares 1 against 2 directly. It CANNOT reach 3 from here, and says so
rather than implying otherwise: what covers the shader is the pixel bed, where
r_gamma_analytic 0 against 1 must move only by the table's own quantisation.

EXPECT A SMALL NONZERO NUMBER, and understand why before reading it as a defect.
The LUT is 256 entries of EIGHT BITS, sampled with linear interpolation: it
quantises its input to 1/255 steps and its output to 1/255 levels. The analytic
form does neither, so at the sample points the two should agree to within the
table's own output rounding -- about half a level -- and the analytic value is
the more accurate of the two wherever they differ. A deviation of MORE than a
level or so means the formulae have genuinely diverged.

Reported in 8-bit levels because that is the unit the difference would be seen
in, and the worst input is printed so a systematic error shows up as a location
rather than only as a magnitude.
================
*/
static void R_GammaAnalyticTest_f(struct cmd_state_s *cmd)
{
	unsigned short ramp[256 * 3];
	float ig[3], sc[3], bs[3], cb;
	double worst = 0.0, sum = 0.0;
	int worsti = -1, worstc = -1, i, c, n = 0;

	if (!VID_GetGammaAnalytic(ig, sc, bs, &cb))
	{
		Con_Printf("gamma analytic: NOT AVAILABLE in this configuration (v_psycho or vid_sRGB); the LUT is the curve\n");
		return;
	}
	VID_BuildGammaTables(&ramp[0], 256);
	for (i = 0; i < 256; i++)
	{
		float in = i / 255.0f;
		for (c = 0; c < 3; c++)
		{
			// the table as the renderer actually consumes it: 16-bit entry
			// quantised to the 8 bits the gammaramps texture is built with
			double lut = (double)(unsigned char)(ramp[i + c * 256] * 255.0 / 65535.0 + 0.5) / 255.0;
			double t = cb * in / ((cb - 1.0) * in + 1.0);
			double ana = pow(t, ig[c]) * sc[c] + bs[c];
			double d;
			if (ana < 0.0) ana = 0.0;
			if (ana > 1.0) ana = 1.0;
			d = fabs(ana - lut) * 255.0;
			sum += d; n++;
			if (d > worst) { worst = d; worsti = i; worstc = c; }
		}
	}
	Con_Printf("gamma analytic vs LUT: max %.3f levels (at input %d/255, channel %s), mean %.4f over %d samples\n",
		worst, worsti, worstc == 0 ? "r" : (worstc == 1 ? "g" : "b"), sum / n, n);
	Con_Printf("gamma analytic: invgamma %.4f %.4f %.4f  scale %.4f %.4f %.4f  base %.4f %.4f %.4f  boost %.4f\n",
		ig[0], ig[1], ig[2], sc[0], sc[1], sc[2], bs[0], bs[1], bs[2], cb);
	// the smoke test greps this verdict; 1.5 levels is comfortably above the
	// table's own half-level rounding and far below any real divergence
	Con_Printf("gamma analytic: %s\n", worst <= 1.5 ? "MATCHES the LUT within its quantisation" : "DIVERGES from the LUT");
}

static void R_Shader_SetGammaAnalyticUniforms(float ceiling)
{
	float ig[3], sc[3], bs[3], cb;
	if (r_glsl_permutation->loc_GammaAnalyticA < 0)
		return;
	if (!VID_GetGammaAnalytic(ig, sc, bs, &cb))
	{
		// Cannot happen with the static parm compiled in -- the enable
		// predicate is this same call -- but a shader fed a stale curve would
		// be silently wrong rather than loudly, so the identity is spelled out
		// instead of assumed: boost 1 and gamma 1 make the expression c*1+0.
		ig[0] = ig[1] = ig[2] = 1.0f;
		sc[0] = sc[1] = sc[2] = 1.0f;
		bs[0] = bs[1] = bs[2] = 0.0f;
		cb = 1.0f;
	}
	R_Shader_Uniform4f(r_glsl_permutation->loc_GammaAnalyticA, ig[0], ig[1], ig[2], cb);
	// B.w is the OUTPUT CEILING (METAL.md Phase 7-3), which is why it stopped
	// being the spare 0.0f it shipped as at 7-1. The shader floors it at 1.0, so
	// this cannot make the picture darker however it is called.
	if (r_glsl_permutation->loc_GammaAnalyticB >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_GammaAnalyticB, sc[0], sc[1], sc[2], ceiling);
	if (r_glsl_permutation->loc_GammaAnalyticC >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_GammaAnalyticC, bs[0], bs[1], bs[2], 0.0f);
}

static void R_Volumetric_SetReprojUniforms(int texw, int texh)
{
	if (r_glsl_permutation->loc_VolumetricReproj0 < 0)
		return;
#ifdef USE_RT_METAL
	{
		float fwd[3], rd[3], ud[3];
		if (rt_metal_reproject.integer && RT_Metal_GetShownCamera(fwd, rd, ud))
		{
			R_Shader_Uniform4f(r_glsl_permutation->loc_VolumetricReproj0, fwd[0], fwd[1], fwd[2], 1.0f);
			R_Shader_Uniform4f(r_glsl_permutation->loc_VolumetricReproj1, rd[0], rd[1], rd[2], (float)texw);
			R_Shader_Uniform4f(r_glsl_permutation->loc_VolumetricReproj2, ud[0], ud[1], ud[2], (float)texh);
			return;
		}
	}
#else
	(void)texw; (void)texh;
#endif
	R_Shader_Uniform4f(r_glsl_permutation->loc_VolumetricReproj0, 0.0f, 0.0f, 1.0f, 0.0f);
}

// THE WORLD FIELD BAKES AT THE SAME SITE ON BOTH BACKENDS (2026-08-16), and
// when only the heat shimmer wants it, it bakes DURING MAP LOAD rather than at
// first use inside R_BlendView. The murk resolves the field lazily in RenderFog
// (in-frame, both backends, symmetric); the shimmer used to resolve it lazily
// in R_BlendView, and R_LavaShimmer_Wanted() is Metal-only (vid.m5postfx) -- so
// at r_volumetric 0 Metal baked in-frame and GL never baked at all. Measured on
// the parity bed, WHEN the field bakes changes 90-98% of every subsequent
// frame's pixels (a ~1-pixel vertical shift of the whole scene, persistent for
// the boot), on GL and Metal alike; the asymmetry is what broke every
// r_volumetric-0 vantage from the F7 merge to 2026-08-16 (spawn 4.01 / rt_wall
// 3.69 against recorded 0.0005 / 0.0006). WHICH sites are equivalent is
// EMPIRICAL and the mechanism is OPEN (CLAUDE.md hard-won fact): a bake during
// map load (this function, fired the moment cl.worldmodel is set, from the
// loading-screen refreshes) is byte-identical to no bake at all against GL; a
// bake in RenderFog on both backends crosses at the recorded values; but a bake
// at the first post-SIGNONS frame boundary, before Metal_Backend_BeginFrame, is
// SHIFTED again (spawn 4.0158), and pre-baking for the murk on a bare
// cl.worldmodel test split GL's murk_shafts boots into two states by boot
// position -- so "between frames" is not the invariant, and no single site is
// clean in every configuration. Not heap corruption (ASan boots clean through
// bake, restart and map load). Hence the narrowest rule that is measured clean
// everywhere: pre-bake here ONLY when the shimmer wants the field and the murk
// will NOT bake it in RenderFog; otherwise leave the murk's own resolve to run
// first, on both backends. Called every frame; after the first it is compares.
void R_Volumetric_ResolveField(void)
{
	if (!cl.worldmodel)
		return;
	if (r_volumetric.integer && (r_volumetric_floor.integer || r_volumetric_water.integer || r_volumetric_debug.integer >= 5))
		return; // the murk resolves it in RenderFog on both backends -- the measured-symmetric site
	if (R_LavaShimmer_Wanted())
		R_Volumetric_GetField();
}

static qbool R_Volumetric_Active(void)
{
	return r_fb.volumetricactive && r_fb.scenedepthvalid && r_fb.scenedepthtexture != NULL;
}

/*
================
R_Volumetric_LiquidFadeField
R_Volumetric_LiquidFadeUniforms

LIQUIDFOG (r_volumetric_liquidfade). The murk cannot fog an alpha-blended
liquid: it composites inside R_RenderScene BEFORE R_MeshQueue_RenderTransparent
and reads scene depth, which a transparent surface never writes. So the moment
r_wateralpha_force makes water blended it leaves the pass that was fogging it
correctly and reads at full brightness through fog thick enough to hide the wall
behind it. The fix is the hook classic fog has always used for this class --
evaluate it per fragment in the liquid's OWN shader, off the fragment's own eye
vector, touching no depth buffer.

These two feed that shader arm. The ARITHMETIC lives in shader_density.h's
DPD_LIQUID_FADE, spliced by both language arms; what lives here is the packing,
and it is a LOCKSTEP TRIO: this function, that macro, and
R_Volumetric_FieldDensity above -- whose cheap three-tap approximation the
shader arm is the GPU twin of. Same clamps, same terms, same 0.01 optical-depth
convention.

THE FIELD ACCESSOR MUST NEVER BAKE, and that is why it is not simply
R_Volumetric_GetField(). WHEN the world field bakes shifts the whole scene about
a pixel for the rest of the boot, on BOTH backends (the 2026-08-16 fact, and
R_Volumetric_ResolveField exists solely to keep the bake at one
measured-symmetric site). A bake reached from inside the transparent pass would
be a fourth site, in-frame, and on a Metal-only or GL-only predicate it would be
an asymmetric one -- exactly the shape that left the parity bed dark for four
days. So this returns the CACHE or nothing: it repeats GetField's own cache test
and never its build. In practice the cache is always warm on a frame that can
reach here, because R_Volumetric_RenderFog resolves the field earlier in the
same frame; when it is not, the strength goes to 0 and the fade is simply absent
for that frame rather than popping the scene.
================
*/
static rtexture_t *R_Volumetric_LiquidFadeField(void)
{
	// the murk must actually be running -- the fade is the murk reaching a
	// surface the murk's own pass could not, never a fog of its own
	if (!r_fb.volumetricactive || !r_volumetric_fielddata)
		return NULL;
	// and the field must be the one this world baked (GetField's cache test,
	// minus its build). r_volumetric_fieldorigin/_fieldinvsize are set by the
	// same bake, so texture and mapping cannot disagree.
	if (!r_volumetric_fieldtexture || r_volumetric_fieldmodel != cl.worldmodel)
		return NULL;
	// the exact condition under which RenderFog resolves it at all: with both
	// of these off it never bakes, so there is nothing warm to read
	if (!(r_volumetric_floor.integer || r_volumetric_water.integer))
		return NULL;
	return r_volumetric_fieldtexture;
}

// Uploads the fade's uniforms and returns the strength it actually applied --
// 0 on every batch that is not an alpha-blended liquid, which is the exact
// no-op the RTLiquid arm above uses and what makes the shader's branch a
// uniform compare rather than work. The caller uses the return value to stand
// the classic fog down on the same batch, so the two can never stack.
static float R_Volumetric_LiquidFadeUniforms(qbool isliquid)
{
	float strength = 0.0f, watermode, floormode;
	rtexture_t *field = R_Volumetric_LiquidFadeField();

	if (r_glsl_permutation->loc_LiquidFade < 0)
		return 0.0f;
	if (isliquid && field && r_volumetric_liquidfade.value > 0.0f)
		strength = r_volumetric_liquidfade.value;

	// floormode/watermode gate the same terms the march's do, and for the same
	// reason: without a field, abovefloor and the signed distance are garbage
	floormode = (field && r_volumetric_floor.integer) ? 1.0f : 0.0f;
	watermode = (field && r_volumetric_water.integer) ? 1.0f : 0.0f;

	// NO COLOUR IS COMPUTED HERE, and that is the point. The fade modulates
	// ALPHA, so the colour a fading surface reveals is the murk's own
	// composited pixel rather than a reconstruction of it -- see
	// shader_density.h's DPD_LIQUID_FADE for what reconstructing it cost.

	// LOCKSTEP shader_density.h DPD_LIQUID_FADE -- the packing comment there is
	// the contract, and every clamp below is R_Volumetric_FieldDensity's.
	R_Shader_Uniform4f(r_glsl_permutation->loc_LiquidFade, strength, max(1.0f, r_volumetric_dist.value), max(1.0f, r_volumetric_fieldsdfrange), r_volumetric_flooroffset.value);
	if (r_glsl_permutation->loc_LiquidFadeAir >= 0)
		R_Shader_Uniform4f(r_glsl_permutation->loc_LiquidFadeAir, r_volumetric_density.value, max(1.0f, r_volumetric_height.value), max(0.0f, r_volumetric_corner.value) * floormode, VOL_FIELD_MAXH);
	if (r_glsl_permutation->loc_LiquidFadeGround >= 0)
		R_Shader_Uniform4f(r_glsl_permutation->loc_LiquidFadeGround, max(0.0f, r_volumetric_ground.value) * floormode, max(1.0f, r_volumetric_groundheight.value), r_volumetric_groundoffset.value, watermode * max(0.0f, r_volumetric_watermist.value));
	if (r_glsl_permutation->loc_LiquidFadeLiquid >= 0)
		R_Shader_Uniform4f(r_glsl_permutation->loc_LiquidFadeLiquid, R_Volumetric_WaterDensityView(), watermode, max(1.0f, r_volumetric_mistheight.value), bound(0.0f, r_volumetric_liquidfloor.value, 1.0f));
	if (r_glsl_permutation->loc_LiquidFadeOrigin >= 0)
		R_Shader_Uniform3f(r_glsl_permutation->loc_LiquidFadeOrigin, r_refdef.view.origin[0], r_refdef.view.origin[1], r_refdef.view.origin[2]);
	if (r_glsl_permutation->loc_LiquidFadeFieldOrigin >= 0)
		R_Shader_Uniform3f(r_glsl_permutation->loc_LiquidFadeFieldOrigin, r_volumetric_fieldorigin[0], r_volumetric_fieldorigin[1], r_volumetric_fieldorigin[2]);
	if (r_glsl_permutation->loc_LiquidFadeFieldScale >= 0)
		R_Shader_Uniform3f(r_glsl_permutation->loc_LiquidFadeFieldScale, r_volumetric_fieldinvsize[0], r_volumetric_fieldinvsize[1], r_volumetric_fieldinvsize[2]);
	// BEAUTY C2: caustics ride the same field and world position, on WORLD opaque
	// batches only (a liquid batch takes the fade; an entity carries no field
	// meaning; the HUD none). Exactly zero elsewhere, the RTLiquids no-op shape.
	if (r_glsl_permutation->loc_CausticParams >= 0)
	{
		qbool caust = r_caustics.value > 0.0f && !isliquid && field && rsurface.entity == r_refdef.scene.worldentity && !r_refdef.view.isoverlay;
		if (caust)
		{
			static qbool announced;
			if (!announced) { announced = true; Con_DPrintf("caustics armed (strength %.2f)\n", r_caustics.value); }
		}
		R_Shader_Uniform4f(r_glsl_permutation->loc_CausticParams, caust ? r_caustics.value : 0.0f, max(0.0005f, r_caustics_scale.value), (float)(r_refdef.scene.time * r_caustics_speed.value), 0.0f);
	}
	return strength;
}

/*
================
M5_MapFogColor

Some maps set fog in their worldspawn, and DarkPlaces honours it -- including
the "_fog" spelling, because CL_ParseEntityLump strips a leading underscore
from every key before matching (cl_parse.c:405), so what mappers write as a
compiler directive lands on the real fog handler.

Nothing ever taught the classic fog and the volumetric murk about each other:
R_Volumetric_Active() does not test r_refdef.fogenabled, so on such a map BOTH
draw. Opaque surfaces are de-conflicted at the FogSurfaceAmount uniform, but
transparent surfaces, sprites, the framebuffer clear and every particle are
not -- with r_volumetric_particles on, particles are faded twice.

So when the murk is running, it takes the map's fog COLOUR and the classic
pass stands down. Density stays the player's: a mapper's density is tuned for
a fog model this engine is no longer using for them, and the murk's own
thickness is a setting the player owns.

Of the five gamedirs here only Dimension of the Machine is affected -- 7 of its
25 maps set fog, and not one map in stock Quake, Armagon, Dissolution or
Dimension of the Past does. So this is a no-op everywhere else by content, not
merely by gating.
================
*/
qbool M5_MapFogColor(float *out)
{
	if (!m5_packfog.integer || !r_refdef.fog_density || !r_volumetric.integer)
		return false;
	if (out)
		VectorSet(out, r_refdef.fog_red, r_refdef.fog_green, r_refdef.fog_blue);
	return true;
}

/*
================
R_Volumetric_RenderFog

Renders the murk into a half-resolution target and composites it into the scene
that is currently bound. Called from R_RenderScene between the opaque and
transparent passes.
================
*/
// THE FOG'S SHROUD OVER THE BALL (BALLLIGHTNING.md, Seb's eleventh look): the
// murk composite records what it composited FROM this frame -- the kernel's
// fog texture (a GL name adopted into the renderer's table) or the GL march's
// render target, both rgb = scatter, ALPHA = TRANSMITTANCE to the scene depth
// -- and the bolt pass samples it so the globe and the bolt are dimmed by the
// fog between the eye and them, like a torch would be. Frame-stamped: a frame
// with no murk binds white (transmittance 1).
static rtexture_t *m5_fogshroud_rt;
static unsigned int m5_fogshroud_gl;
static int m5_fogshroud_frame = -1;

static void R_Volumetric_RenderFog(int viewfbo, rtexture_t *viewdepthtexture, rtexture_t *viewcolortexture, int viewx, int viewy, int viewwidth, int viewheight)
{
	r_rendertarget_t *rt;
	rtexture_t *noise, *field, *irr;
	int fogwidth, fogheight;
	float scale, m16f[16], wind[3], color[3], watercolor[3], slimecolor[3], lavacolor[3];
	float groundwind[3], groundcolor[3];
	float basez, floormode, watermode, ambientmode;
#ifdef USE_RT_METAL
	unsigned int shafttex;
	int shaftw, shafth;
	unsigned int fogtex;
	int fogtw, fogth;
#endif

	if (!R_Volumetric_Active())
		return;
	noise = R_Volumetric_GetNoiseTexture();
	if (!noise)
		return;
	// Both the floor anchoring and the liquid keying read the baked world field. If
	// it will not build -- no world, or an allocation failure -- fall back to the
	// camera-relative bed and pure air murk rather than dropping the pass.
	field = (r_volumetric_floor.integer || r_volumetric_water.integer) ? R_Volumetric_GetField() : NULL;
	floormode = (field && r_volumetric_floor.integer) ? 1.0f : 0.0f;
	watermode = (field && r_volumetric_water.integer) ? 1.0f : 0.0f;
	if (!field)
		field = noise; // never leave a sampler unbound
	// the ambient irradiance grid, same lifecycle as the field: bake on demand,
	// fall back to full-bright ambient (strength forced 0 at upload) if it fails
	irr = r_volumetric_ambient.value > 0.0f ? R_Volumetric_GetIrradianceGrid() : NULL;
	ambientmode = irr ? 1.0f : 0.0f;
#ifdef USE_RT_METAL
	// RE-HAND IT IF THE SIDECAR HAS NONE (2026-09-19). The grid is handed over at
	// BAKE time, and the world build drops the sidecar's copy on purpose -- which
	// is only safe while the bake always follows the build. cl_particles_lighting
	// 2 bakes the same grid from R_DrawParticles, so on a map's first frame the
	// hand-over can precede the build, the build drops it, and the cache above
	// never bakes again: the fog kernel then runs the entire map with no ambient
	// term at all, silently. One pointer test a frame closes it for good, whoever
	// bakes first.
	if (irr && r_volumetric_irrdata && !RT_Metal_HasFogIrradiance())
		RT_Metal_SetFogIrradiance(r_volumetric_irrdata, r_volumetric_irrsize);
#endif
	if (!irr)
		irr = noise; // never leave a sampler unbound
#ifdef USE_RT_METAL
	// god rays: resolve the sidecar's shaft buffer HERE, with the other texture
	// resolves, never mid-bind-sequence. 0 when RT is off, shafts are off, or no
	// trace has completed yet -- the intensity uniform is forced to 0 then, so the
	// murk is bit-identical to the shaftless murk.
	shafttex = 0; shaftw = shafth = 0;
	if (rt_metal.integer && rt_metal_shafts.integer && !rt_metal_fog.integer)
		RT_Metal_GetShaftsTexture(&shafttex, &shaftw, &shafth);
	// FULL IN-KERNEL FOG: when the Metal kernel marched the integral this frame,
	// the murk pass is a single masked composite of its output and everything
	// below is skipped. Unavailable (first frames, compile failure, RT off) ->
	// fall through to the GL march, so fog never pops out.
	fogtex = 0; fogtw = fogth = 0;
	if (rt_metal.integer && rt_metal_fog.integer)
		RT_Metal_GetFogTexture(&fogtex, &fogtw, &fogth);
	// Say which path ran, once per change. Whether the kernel actually owns the
	// fog or has quietly fallen back to the GL march is otherwise UNOBSERVABLE:
	// R_TimeReport uses the same "volumetric" label on both, and the only tell is
	// a 4x swing in an r_speeds stat shared with bloom. That makes "the god rays
	// do nothing" impossible to triage -- you cannot tell a dead light term from
	// a fog kernel that never ran. Same state-change shape as the RT composite's
	// view-model mask line, which tests/smoke.sh already asserts on.
	{
		static int lastfogpath = -1;
		int fogpath = fogtex ? 1 : 0;
		if (rt_metal.integer && rt_metal_fog.integer && fogpath != lastfogpath)
		{
			Con_Printf("volumetric: %s\n", fogpath
				? "RT fog kernel ACTIVE (god rays and fog lighting live)"
				: "RT fog kernel unavailable -- falling back to the GL march (no fog lighting)");
			lastfogpath = fogpath;
		}
		else if (!(rt_metal.integer && rt_metal_fog.integer))
			lastfogpath = -1;
	}
	if (fogtex)
	{
		R_ResetViewRendering2D(viewfbo, viewdepthtexture, viewcolortexture, viewx, viewy, viewwidth, viewheight);
		// VIEWMODEL MASK: the gun is not in the RT BLAS, so the kernel fogged the
		// wall behind it; pin every fragment of this quad at window depth 0.0625
		// (near == far collapses the range) and pass GL_LESS only where the stored
		// scene depth is GREATER -- the world, sky included -- never the gun. The
		// same trick as the RT composite's fullscreen triangle.
		GL_DepthTest(true);
		GL_DepthFunc(GL_LESS);
		GL_DepthMask(false);
		GL_DepthRange(0.0625f, 0.0625f);
		GL_BlendFunc(GL_ONE, GL_SRC_ALPHA);   // alpha IS transmittance
		GL_Color(1, 1, 1, 1);
		R_Mesh_PrepareVertices_Generic_Arrays(4, r_screenvertex3f, NULL, r_fb.rt_screen->texcoord2f);
		R_SetupShader_SetPermutationGLSL(SHADERMODE_VOLUMETRICFOG, 0);
		if (r_glsl_permutation->tex_Texture_ScreenDepth  >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_ScreenDepth , viewdepthtexture);
		if (r_glsl_permutation->tex_Texture_VolumeNoise  >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_VolumeNoise , noise);
		if (r_glsl_permutation->tex_Texture_VolumeField  >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_VolumeField , field);
		if (r_glsl_permutation->tex_Texture_VolumeIrr    >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_VolumeIrr   , irr);
		// remember the kernel's texture for the bolt pass's shroud
		m5_fogshroud_gl = fogtex;
		m5_fogshroud_rt = NULL;
		m5_fogshroud_frame = (int)host.framecount;
		if (r_glsl_permutation->tex_Texture_KernelFog >= 0)
		{
			if (vid.renderpath == RENDERPATH_METAL)
				// Adopted into the renderer's own texture table (6-3a), so an
				// ordinary bind -- no rectangle target, and nothing to unbind
				// afterwards. Same shape as the RT term at the liquids site.
				Metal_Backend_TexBind(r_glsl_permutation->tex_Texture_KernelFog, (int)fogtex);
			else
			{
				GL_ActiveTexture(r_glsl_permutation->tex_Texture_KernelFog);
				qglBindTexture(GL_TEXTURE_RECTANGLE, (GLuint)fogtex);
			}
		}
		if (r_glsl_permutation->loc_VolumetricKernelFog >= 0)
			R_Shader_Uniform4f(r_glsl_permutation->loc_VolumetricKernelFog,
				viewwidth ? (float)fogtw / (float)viewwidth : 0.0f,
				viewheight ? (float)fogth / (float)viewheight : 0.0f,
				(float)r_refdef.view.viewport.x, (float)r_refdef.view.viewport.y);
		// the reprojected fetch reconstructs this pixel's world ray, so the
		// reconstruction basis must be set on this early-return path too
		if (r_glsl_permutation->loc_ViewToWorld >= 0) { float m16f[16]; Matrix4x4_ToArrayFloatGL(&r_fb.scenedepth_viewtoworld, m16f); R_Shader_UniformMatrix4fv(r_glsl_permutation->loc_ViewToWorld, 1, false, m16f); }
		if (r_glsl_permutation->loc_FrustumScale >= 0) R_Shader_Uniform2f(r_glsl_permutation->loc_FrustumScale, r_fb.scenedepth_frustum_x, r_fb.scenedepth_frustum_y);
		R_Volumetric_SetReprojUniforms(fogtw, fogth);
		// DEPTH-AWARE UPSAMPLE (rt_metal_fog_upsample): the composite now reads
		// the scene depth, so the linearisation pair the march feeds is fed HERE
		// too -- on Metal an unwritten member reads whatever the last shader left
		// at that offset (shader_msl.h's Reproj note), so both are written every
		// frame, and Upsample2.z is written 0 rather than left when the switch is
		// off. The trace size is the history the kernel took its march ends from
		// (RT_Metal_GetTermTexture is the trace-sized accessor; only its size is
		// wanted here). Tolerances: the smooth test at a quarter of the edge
		// tolerance -- the bilinear-predicted-depth error is second order on any
		// plane, so it stays under that down to ~1.5 degrees of grazing.
		if (r_glsl_permutation->loc_ScreenToDepth     >= 0) R_Shader_Uniform2f(r_glsl_permutation->loc_ScreenToDepth, r_fb.scenedepth_screentodepth[0], r_fb.scenedepth_screentodepth[1]);
		if (r_glsl_permutation->loc_VolumetricFarClip >= 0) R_Shader_Uniform1f(r_glsl_permutation->loc_VolumetricFarClip, r_fb.scenedepth_farclip);
		if (r_glsl_permutation->loc_VolumetricUpsample >= 0 && r_glsl_permutation->loc_VolumetricUpsample2 >= 0)
		{
			unsigned int termtex = 0;
			int tracew = 0, traceh = 0;
			// (the lower bound is deliberately absurd: 0.001 puts every pixel on
			// the four-tap edge path, which is the cost-measurement recipe)
			float tol = bound(0.001f, rt_metal_fog_upsample_depth.value, 0.5f);
			qbool up = rt_metal_fog_upsample.integer && viewwidth > 0 && viewheight > 0
				&& RT_Metal_GetTermTexture(&termtex, &tracew, &traceh) && tracew > 0 && traceh > 0;
			R_Shader_Uniform4f(r_glsl_permutation->loc_VolumetricUpsample, up ? (float)tracew : 1.0f, up ? (float)traceh : 1.0f, (float)max(viewwidth, 1), (float)max(viewheight, 1));
			R_Shader_Uniform4f(r_glsl_permutation->loc_VolumetricUpsample2, tol * 0.25f, tol, up ? 1.0f : 0.0f, 0.0f);
		}
		// METAL_FRAMEMS=2 times this one draw on the GPU (the depth-aware
		// upsample's cost instrument; a no-op otherwise, on GL always)
		{
			static int murkregion = -1;
			if (murkregion < 0) { const char *e = getenv("METAL_FRAMEMS"); murkregion = (e && atoi(e) == 2) ? 1 : 0; }
			if (murkregion) Metal_Backend_ProfileRegion(1, "murk composite");
			R_Mesh_Draw(0, 4, 0, 2, polygonelement3i, NULL, 0, polygonelement3s, NULL, 0);
			if (murkregion) Metal_Backend_ProfileRegion(0, "murk composite");
			else if (vid.renderpath == RENDERPATH_METAL && rt_metal_pipeline.integer >= 2)
				Metal_Backend_Kick();   // A2 mode 2: the murk composite goes to the GPU now, ahead of the post encode
		}
		if (r_glsl_permutation->tex_Texture_KernelFog >= 0 && vid.renderpath != RENDERPATH_METAL)
		{
			GL_ActiveTexture(r_glsl_permutation->tex_Texture_KernelFog);
			qglBindTexture(GL_TEXTURE_RECTANGLE, 0);
		}
		GL_DepthRange(0.0f, 1.0f);
		GL_DepthFunc(GL_LEQUAL);
		GL_DepthTest(false);
		r_refdef.stats[r_stat_bloom_drawpixels] += viewwidth * viewheight;
		R_ResetViewRendering3D(viewfbo, viewdepthtexture, viewcolortexture, viewx, viewy, viewwidth, viewheight);
		if (r_timereport_active)
			R_TimeReport("volumetric");
		return;
	}
#endif

	scale = bound(0.125f, r_volumetric_scale.value, 1.0f);
	fogwidth = (int)ceil(viewwidth * scale);
	fogheight = (int)ceil(viewheight * scale);
	if (fogwidth < 1 || fogheight < 1)
		return;
	// No depth attachment: the pass samples the scene depth as a texture instead,
	// which is only legal because we are rendering somewhere other than the scene.
	rt = R_RenderTarget_Get(fogwidth, fogheight, TEXTYPE_UNUSED, false, TEXTYPE_COLORBUFFER16F, TEXTYPE_UNUSED, TEXTYPE_UNUSED, TEXTYPE_UNUSED);
	if (!rt || !rt->colortexture[0])
		return;

	R_Volumetric_ParseVec3(r_volumetric_wind.string, wind, 10.0f, 4.0f, 1.5f);
	// colours come from the per-channel cvars, never from a string: see
	// R_Volumetric_SplitColorCvar for why the string form could not be trusted
	VectorSet(color,      r_volumetric_color_red.value,      r_volumetric_color_green.value,      r_volumetric_color_blue.value);
	M5_MapFogColor(color);	// a map's own fog colour wins; lockstep with the kernel params above
	VectorSet(watercolor, r_volumetric_watercolor_red.value, r_volumetric_watercolor_green.value, r_volumetric_watercolor_blue.value);
	VectorSet(slimecolor, r_volumetric_slimecolor_red.value, r_volumetric_slimecolor_green.value, r_volumetric_slimecolor_blue.value);
	VectorSet(lavacolor,  r_volumetric_lavacolor_red.value,  r_volumetric_lavacolor_green.value,  r_volumetric_lavacolor_blue.value);
	R_Volumetric_ParseVec3(r_volumetric_groundwind.string, groundwind, 4.0f, 1.7f, 0.0f);
	VectorSet(groundcolor, r_volumetric_groundcolor_red.value, r_volumetric_groundcolor_green.value, r_volumetric_groundcolor_blue.value);
	// Fallback bed for r_volumetric_floor 0, and for maps where the field would not
	// build. Anchoring to the world model's bounding-box minimum instead was the
	// obvious thing to try and is wrong: on a map with a deep pit or a basement that
	// is far below whatever room you are in, the height falloff has already decayed to
	// nothing by the time it reaches you and the murk is invisible. Camera-relative is
	// an approximation -- the bed slides if you jump, which is exactly what the baked
	// field now fixes -- but it needs no per-map data at all.
	basez = r_refdef.view.origin[2] + r_volumetric_heightbase.value;

	// --- pass 1: march the murk into the fog target ---
	R_ResetViewRendering2D(rt->fbo, NULL, rt->colortexture[0], 0, 0, fogwidth, fogheight);
	GL_DepthTest(false);
	GL_DepthMask(false);
	GL_BlendFunc(GL_ONE, GL_ZERO);
	GL_Color(1, 1, 1, 1);
	R_Mesh_PrepareVertices_Generic_Arrays(4, r_screenvertex3f, NULL, r_fb.rt_screen->texcoord2f);
	R_SetupShader_SetPermutationGLSL(SHADERMODE_VOLUMETRICFOG, 0);
	if (r_glsl_permutation->tex_Texture_ScreenDepth  >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_ScreenDepth , viewdepthtexture);
	if (r_glsl_permutation->tex_Texture_VolumeNoise  >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_VolumeNoise , noise);
	if (r_glsl_permutation->tex_Texture_VolumeField  >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_VolumeField , field);
	if (r_glsl_permutation->tex_Texture_VolumeIrr    >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_VolumeIrr   , irr);
#ifdef USE_RT_METAL
	// The shaft buffer is a FOREIGN GL_TEXTURE_RECTANGLE (IOSurface-backed, no
	// rtexture_t). R_Mesh_TexBind has no rectangle case and would silently bind
	// nothing, so bind raw -- but through GL_ActiveTexture, never qglActiveTexture:
	// the backend caches the active-unit selector in gl_state.unit, and a raw call
	// desyncs it so every later R_Mesh_TexBind lands on the wrong unit. The
	// rectangle target itself is invisible to the backend's 2D/3D/cube caches, so
	// the bind is safe; it is explicitly unbound after the draw.
	if (r_glsl_permutation->tex_Texture_Shafts >= 0 && shafttex)
	{
		if (vid.renderpath == RENDERPATH_METAL)
			// 6-3b: adopted, so an ordinary bind. The whole rectangle hazard
			// this comment block describes is a GL-path concern only, and with
			// this arm and the kernel-fog one above the tree has no raw
			// GL_TEXTURE_RECTANGLE bind left outside the sidecar itself.
			Metal_Backend_TexBind(r_glsl_permutation->tex_Texture_Shafts, (int)shafttex);
		else
		{
			GL_ActiveTexture(r_glsl_permutation->tex_Texture_Shafts);
			qglBindTexture(GL_TEXTURE_RECTANGLE, (GLuint)shafttex);
		}
	}
#endif
	if (r_glsl_permutation->loc_ScreenToDepth        >= 0) R_Shader_Uniform2f(r_glsl_permutation->loc_ScreenToDepth, r_fb.scenedepth_screentodepth[0], r_fb.scenedepth_screentodepth[1]);
	if (r_glsl_permutation->loc_FrustumScale         >= 0) R_Shader_Uniform2f(r_glsl_permutation->loc_FrustumScale, r_fb.scenedepth_frustum_x, r_fb.scenedepth_frustum_y);
	if (r_glsl_permutation->loc_VolumetricFarClip    >= 0) R_Shader_Uniform1f(r_glsl_permutation->loc_VolumetricFarClip, r_fb.scenedepth_farclip);
	if (r_glsl_permutation->loc_ViewToWorld          >= 0) {Matrix4x4_ToArrayFloatGL(&r_fb.scenedepth_viewtoworld, m16f);R_Shader_UniformMatrix4fv(r_glsl_permutation->loc_ViewToWorld, 1, false, m16f);}
	if (r_glsl_permutation->loc_VolumetricOrigin     >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_VolumetricOrigin, r_refdef.view.origin[0], r_refdef.view.origin[1], r_refdef.view.origin[2]);
	if (r_glsl_permutation->loc_VolumetricParams     >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_VolumetricParams, r_volumetric_density.value, max(1.0f, r_volumetric_height.value), basez, max(1.0f, r_volumetric_dist.value));
	if (r_glsl_permutation->loc_VolumetricNoise      >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_VolumetricNoise, r_volumetric_noisescale.value, bound(0.0f, r_volumetric_noisethresh.value, 0.95f), (float)bound(2, r_volumetric_steps.integer, 128), cl.time);
	if (r_glsl_permutation->loc_VolumetricWind       >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_VolumetricWind, wind[0], wind[1], wind[2]);
	if (r_glsl_permutation->loc_VolumetricColor      >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_VolumetricColor, color[0], color[1], color[2]);
	if (r_glsl_permutation->loc_VolumetricFieldOrigin >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_VolumetricFieldOrigin, r_volumetric_fieldorigin[0], r_volumetric_fieldorigin[1], r_volumetric_fieldorigin[2]);
	if (r_glsl_permutation->loc_VolumetricFieldParams >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_VolumetricFieldParams, r_volumetric_fieldinvsize[0], r_volumetric_fieldinvsize[1], r_volumetric_fieldinvsize[2], VOL_FIELD_MAXH);
	if (r_glsl_permutation->loc_VolumetricLiquid      >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_VolumetricLiquid, R_Volumetric_WaterDensityView(), watermode, r_volumetric_flooroffset.value, floormode);
	if (r_glsl_permutation->loc_VolumetricLiquidDens  >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_VolumetricLiquidDens, R_Volumetric_WaterDensityView(), R_Volumetric_SlimeDensity(), R_Volumetric_LavaDensity(), 0.0f);
	if (r_glsl_permutation->loc_VolumetricMistLavaCut >= 0) R_Shader_Uniform1f(r_glsl_permutation->loc_VolumetricMistLavaCut, bound(0.0f, r_volumetric_mistlavacut.value, 1.0f));
	if (r_glsl_permutation->loc_VolumetricLiquidFloor >= 0) R_Shader_Uniform1f(r_glsl_permutation->loc_VolumetricLiquidFloor, bound(0.0f, r_volumetric_liquidfloor.value, 1.0f));
	if (r_glsl_permutation->loc_VolumetricMist        >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_VolumetricMist, watermode * max(0.0f, r_volumetric_watermist.value), max(1.0f, r_volumetric_mistheight.value), max(1.0f, r_volumetric_fieldsdfrange), max(0.0f, r_volumetric_corner.value) * floormode);
	// GROUND FOG: density folded with floormode (needs the field's floor channel);
	// clamps mirrored EXACTLY in R_Volumetric_GetFogKernelParams (honesty rule)
	if (r_glsl_permutation->loc_VolumetricGround      >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_VolumetricGround, max(0.0f, r_volumetric_ground.value) * floormode, max(1.0f, r_volumetric_groundheight.value), r_volumetric_groundnoisescale.value, bound(0.0f, r_volumetric_groundthresh.value, 0.95f));
	// .z = lava glow strength; .w = the sky transmittance floor (1 - r_volumetric_skyfog,
	// the last spare lane) -- clamp mirrored EXACTLY in R_Volumetric_GetFogKernelParams
	if (r_glsl_permutation->loc_VolumetricGround2     >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_VolumetricGround2, max(0.0f, r_volumetric_grounddeform.value), r_volumetric_groundoffset.value, watermode * max(0.0f, r_volumetric_lavaglow.value), 1.0f - bound(0.0f, r_volumetric_skyfog.value, 1.0f));
	if (r_glsl_permutation->loc_VolumetricGroundWind  >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_VolumetricGroundWind, groundwind[0], groundwind[1], groundwind[2]);
	if (r_glsl_permutation->loc_VolumetricGroundColor >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_VolumetricGroundColor, groundcolor[0], groundcolor[1], groundcolor[2]);
	// AMBIENT IRRADIANCE: x gain, y floor, z strength (0 kills the whole term at
	// the mix, and is forced 0 when the grid failed to bake or the cvar is off);
	// clamps mirrored EXACTLY in R_Volumetric_GetFogKernelParams (honesty rule)
	if (r_glsl_permutation->loc_VolumetricIrr         >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_VolumetricIrr, max(0.0f, r_volumetric_ambientgain.value), bound(0.0f, r_volumetric_ambientfloor.value, 1.0f), bound(0.0f, r_volumetric_ambient.value, 1.0f) * ambientmode, 0.0f);
	if (r_glsl_permutation->loc_VolumetricIrrOrigin   >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_VolumetricIrrOrigin, r_volumetric_irrorigin[0], r_volumetric_irrorigin[1], r_volumetric_irrorigin[2]);
	if (r_glsl_permutation->loc_VolumetricIrrInvSize  >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_VolumetricIrrInvSize, r_volumetric_irrinvsize[0], r_volumetric_irrinvsize[1], r_volumetric_irrinvsize[2]);
	// KH SWIRL: clamps mirrored EXACTLY in R_Volumetric_GetFogKernelParams (honesty rule)
	if (r_glsl_permutation->loc_VolumetricSwirl       >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_VolumetricSwirl, max(0.0f, r_volumetric_swirl.value), r_volumetric_swirlscale.value, max(0.0f, r_volumetric_swirlkh.value), 0.0f);
	// MARCH-TIER DYNAMIC LIGHTS (r_volumetric_dlight): the nearest 8 scene
	// lights, colour premultiplied by the guarded stylescale, the M5 fog weight
	// (memset-0 means unset means 1.0 -- the thunderbolt convention) and the
	// gain. The maths mirrors the RT sidecar's gather (cl_screen.c); the murk's
	// own march does the scattering, so no shadow rays on this tier by design.
	if (r_glsl_permutation->loc_MarchLights >= 0 && r_glsl_permutation->loc_MarchLightParams >= 0)
	{
		float ml[64];
		float mldist[8];
		int mlcount = 0;
		memset(ml, 0, sizeof(ml));
		if (r_volumetric_dlight.value > 0.0f)
		{
			int li, s;
			float reach = max(1.0f, r_volumetric_dist.value);
			for (li = 0; li < r_refdef.scene.numlights; li++)
			{
				const rtlight_t *rtl = r_refdef.scene.lights[li];
				float stylescale = 1.0f, fw, d, c[3];
				int slot;
				if (!rtl || rtl->radius <= 0.0f)
					continue;
				if (rtl->style >= 0 && rtl->style < MAX_LIGHTSTYLES)
					stylescale = r_refdef.scene.rtlightstylevalue[rtl->style];
				fw = rtl->m5fogweight > 0.0f ? rtl->m5fogweight : 1.0f;
				VectorScale(rtl->color, stylescale * fw * r_volumetric_dlight.value, c);
				if (c[0] + c[1] + c[2] <= 0.0f)
					continue;
				d = VectorDistance(r_refdef.view.origin, rtl->shadoworigin) - rtl->radius;
				if (d > reach)
					continue;
				// insertion sort by closest-approach distance, keep the 8 nearest
				slot = mlcount < 8 ? mlcount : 7;
				if (mlcount >= 8 && d >= mldist[7])
					continue;
				while (slot > 0 && mldist[slot - 1] > d)
				{
					mldist[slot] = mldist[slot - 1];
					memcpy(ml + slot * 8, ml + (slot - 1) * 8, sizeof(float) * 8);
					slot--;
				}
				mldist[slot] = d;
				s = slot * 8;
				ml[s + 0] = rtl->shadoworigin[0];
				ml[s + 1] = rtl->shadoworigin[1];
				ml[s + 2] = rtl->shadoworigin[2];
				ml[s + 3] = rtl->radius;
				ml[s + 4] = c[0];
				ml[s + 5] = c[1];
				ml[s + 6] = c[2];
				ml[s + 7] = 0.0f;
				if (mlcount < 8)
					mlcount++;
			}
		}
		R_Shader_Uniform4fv(r_glsl_permutation->loc_MarchLights, 16, ml);
		R_Shader_Uniform4f(r_glsl_permutation->loc_MarchLightParams, (float)mlcount,
			bound(0.0f, r_volumetric_dlight_g.value, 0.95f),
			max(0.0f, r_volumetric_scatter.value),
			max(0.01f, r_volumetric_extinction.value));
	}
#ifdef USE_RT_METAL
	if (r_glsl_permutation->loc_VolumetricShafts      >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_VolumetricShafts, shafttex && fogwidth ? (float)shaftw / (float)fogwidth : 0.0f, shafttex && fogheight ? (float)shafth / (float)fogheight : 0.0f, shafttex ? bound(0.0f, rt_metal_shafts_intensity.value, 4.0f) : 0.0f, 0.0f);
	R_Volumetric_SetReprojUniforms(shaftw, shafth);
	// fallback frame with the kernel-fog parm compiled in: force the GL march branch
	if (r_glsl_permutation->loc_VolumetricKernelFog   >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_VolumetricKernelFog, 0.0f, 0.0f, 0.0f, 0.0f);
#endif
	if (r_glsl_permutation->loc_VolumetricWaterColor  >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_VolumetricWaterColor, watercolor[0], watercolor[1], watercolor[2]);
	if (r_glsl_permutation->loc_VolumetricSlimeColor  >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_VolumetricSlimeColor, slimecolor[0], slimecolor[1], slimecolor[2]);
	if (r_glsl_permutation->loc_VolumetricLavaColor   >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_VolumetricLavaColor, lavacolor[0], lavacolor[1], lavacolor[2]);
	R_Mesh_Draw(0, 4, 0, 2, polygonelement3i, NULL, 0, polygonelement3s, NULL, 0);
#ifdef USE_RT_METAL
	if (r_glsl_permutation->tex_Texture_Shafts >= 0 && shafttex && vid.renderpath != RENDERPATH_METAL)
	{
		// drop the rectangle binding: it is outside the backend's cache, so nothing
		// else would ever clear it, and it would keep the IOSurface referenced.
		// GL only -- the Metal bind above went through the backend's own table,
		// which tracks and clears it like any other texture.
		GL_ActiveTexture(r_glsl_permutation->tex_Texture_Shafts);
		qglBindTexture(GL_TEXTURE_RECTANGLE, 0);
	}
#endif
	r_refdef.stats[r_stat_bloom_drawpixels] += fogwidth * fogheight;

	// --- pass 2: composite back over the scene ---
	// The shader wrote premultiplied colour in RGB and transmittance in A, so
	// scene * A + RGB is exactly GL_ONE, GL_SRC_ALPHA. Bilinear magnification of
	// the half-res buffer is adequate for a field this soft; the depth-aware
	// bilateral upsample belongs with the light shafts.
	R_ResetViewRendering2D(viewfbo, viewdepthtexture, viewcolortexture, viewx, viewy, viewwidth, viewheight);
	GL_DepthTest(false);
	GL_DepthMask(false);
	GL_BlendFunc(GL_ONE, GL_SRC_ALPHA);
	GL_Color(1, 1, 1, 1);
	R_Mesh_PrepareVertices_Generic_Arrays(4, r_screenvertex3f, NULL, rt->texcoord2f);
	// suppresstexalpha MUST be false. It forces the sampled alpha to 1, and this
	// blend is GL_ONE, GL_SRC_ALPHA -- alpha IS the transmittance. With it on, the
	// composite degenerates to scattered + scene, so the murk only ever ADDS light
	// and never obscures anything behind it. That was true from step 2 until this
	// was found: raising the density past the point where the scattered term
	// saturates did nothing at all, because the scene was always drawn at full
	// strength underneath.
	R_SetupShader_Generic(rt->colortexture[0], false, true, false);
	// remember it for the bolt pass's shroud (GL march path)
	m5_fogshroud_rt = rt->colortexture[0];
	m5_fogshroud_gl = 0;
	m5_fogshroud_frame = (int)host.framecount;
	R_Mesh_Draw(0, 4, 0, 2, polygonelement3i, NULL, 0, polygonelement3s, NULL, 0);

	// hand the 3D view back exactly as it was found
	R_ResetViewRendering3D(viewfbo, viewdepthtexture, viewcolortexture, viewx, viewy, viewwidth, viewheight);
	if (r_timereport_active)
		R_TimeReport("volumetric");
}

// ---------------------------------------------------------------------------
// F1: the thunderbolt's capsule-SDF pass
//
// WHAT IT IS FOR. Additively-blended ribbons SUM wherever they overlap, and
// consecutive segments of a polyline always overlap at their shared joint, so
// every kink in the bolt reads about twice as bright as the limbs meeting there
// -- the bright cross or diamond, worst near the muzzle. Accumulating under
// max() instead makes an overlap idempotent, so the seam cannot exist.
//
// SHAPE. One pooled scene-resolution 16F target, cleared to black; every segment
// drawn once as a camera-facing quad carrying capsule-local coordinates; then a
// single additive composite of the result into the scene. That is one more
// fullscreen add than the ribbon path did, in exchange for the joints.
//
// WHERE IT SITS. Immediately after the transparent queue, which is where the
// ribbon used to draw, so its position relative to the RT composite, the murk,
// bloom, the HDR shoulder, the gamma ramps and EDR is unchanged. It goes after
// the murk deliberately: nothing transparent has ever been fogged by the murk
// (it is a depth-driven screen-space pass and transparents write no depth), and
// the bolt keeps the per-fragment classic fog it always had -- see the note at
// the composite.
static float *m5bolt_vert3f, *m5bolt_color4f, *m5bolt_tc0, *m5bolt_tc1, *m5bolt_tc2;
static int *m5bolt_element3i;
static unsigned short *m5bolt_element3s;
static int m5bolt_maxsegs;
static qbool m5bolt_compilewarned;		// once, not once per frame (the OOM-by-console lesson)

static qbool R_LightningM5_Wanted(void)
{
	// The raw WANT, used to force the frame shape. Deliberately without the
	// stereo test and the compile latch: those decide whether the pass runs on a
	// given frame, and letting them move the render-target shape as well would
	// make the whole frame's layout flicker with them.
	return r_lightningbeam_m5.integer && !m5_stock.integer && r_lightningbeam_m5_sdf.integer && vid.blendequationmax;
}

static void R_LightningM5_RenderSDF(int viewfbo, rtexture_t *viewdepthtexture, rtexture_t *viewcolortexture, int viewx, int viewy, int viewwidth, int viewheight)
{
	const unsigned char *segs;
	int numsegs, i, v, e;
	r_rendertarget_t *rt;
	r_viewport_t vp;
	vec3_t eye, fwd, left, up;
	int segbytes = CL_Beam_M5_SDFSegmentBytes();
	float falloff = max(0.25f, r_lightningbeam_m5_sdf_falloff.value);
	// THE FIZZ. Amplitude is a FRACTION of the radius, bounded so a cvar value
	// cannot demand a quad wider than the pad the shader's clamp guarantees.
	float fizz = bound(0.0f, r_lightningbeam_m5_fizz.value, 0.9f);
	float fizzpad = 1.0f + fizz;			// exactly 1.0 at fizz 0 -- the old geometry
	float arc = 0.0f;					// running length down the segment stream

	// THE LATCH, updated before anything else and deliberately not conditional on
	// there being segments to draw. A permutation-0 compile failure degrades
	// SILENTLY to program 0 -- R_SetupShader_SetPermutationGLSL has no bits left
	// to strip, so it calls UseProgram(0) and returns -- and the emitter runs
	// during the relink, long before this pass, so it has to be told a frame in
	// advance. Every failure direction therefore lands on the old ribbon rather
	// than on no bolt at all; the cost is one ribbon frame after the cvar is
	// switched on, which is also the frame that pays for the compile.
	if (!R_LightningM5_Wanted())
		CL_Beam_M5_SDFSetLatch(false);
	else
	{
		R_SetupShader_SetPermutationGLSL(SHADERMODE_M5BOLT, 0);
		if (r_glsl_permutation && r_glsl_permutation->program)
			CL_Beam_M5_SDFSetLatch(true);
		else
		{
			CL_Beam_M5_SDFSetLatch(false);
			if (!m5bolt_compilewarned)
			{
				m5bolt_compilewarned = true;
				Con_Printf(CON_ERROR "M5 bolt: the SDF shader will not compile; falling back to the additive ribbon\n");
			}
		}
	}

	segs = (const unsigned char *)CL_Beam_M5_SDFSegments(&numsegs);
	if (!segs || numsegs < 1)
		return;
	// The scene depth must be sampleable: the accumulation target carries no
	// depth attachment of its own (it cannot -- reading the depth texture bound
	// to the current framebuffer is a feedback loop), so occlusion is a texture
	// read in the shader. R_LightningM5_Wanted is what forces the offscreen path
	// and the texture-rather-than-renderbuffer depth, exactly as r_volumetric
	// does; this is the belt-and-braces test for a frame where the shape was
	// refused for some other reason.
	//
	// Taken from the PARAMETER, not from r_fb.scenedepthtexture: that one is
	// published only under r_volumetric.integer, which this feature does not
	// require and must not start requiring.
	if (!viewdepthtexture)
		return;

	rt = R_RenderTarget_Get(viewwidth, viewheight, TEXTYPE_UNUSED, false, TEXTYPE_COLORBUFFER16F, TEXTYPE_UNUSED, TEXTYPE_UNUSED, TEXTYPE_UNUSED);
	if (!rt)
		return;

	if (numsegs > m5bolt_maxsegs)
	{
		int want = max(numsegs, m5bolt_maxsegs ? m5bolt_maxsegs * 2 : 2048);
		m5bolt_vert3f   = (float *)Mem_Realloc(r_main_mempool, m5bolt_vert3f,   want * 4 * sizeof(float[3]));
		m5bolt_color4f  = (float *)Mem_Realloc(r_main_mempool, m5bolt_color4f,  want * 4 * sizeof(float[4]));
		m5bolt_tc0      = (float *)Mem_Realloc(r_main_mempool, m5bolt_tc0,      want * 4 * sizeof(float[2]));
		m5bolt_tc1      = (float *)Mem_Realloc(r_main_mempool, m5bolt_tc1,      want * 4 * sizeof(float[3]));
		// THE FIZZ'S ARC CARRIER. tc1 is passed as the svector3f slot, which is
		// three components by contract and already full (length, radius, falloff),
		// so this rides tvector3f -- the next slot, which this pass left NULL.
		m5bolt_tc2      = (float *)Mem_Realloc(r_main_mempool, m5bolt_tc2,      want * 4 * sizeof(float[3]));
		m5bolt_element3i = (int *)Mem_Realloc(r_main_mempool, m5bolt_element3i, want * 6 * sizeof(int));
		m5bolt_element3s = (unsigned short *)Mem_Realloc(r_main_mempool, m5bolt_element3s, want * 6 * sizeof(unsigned short));
		m5bolt_maxsegs = want;
	}

	// The camera comes from the view MATRIX, never from r_refdef.view.origin --
	// same rule the ribbon emitter follows and for the same reason (origin is
	// written in R_SetupView; the matrix is current from V_CalcRefdef). Note
	// argument two is LEFT, not right.
	Matrix4x4_ToVectors(&r_refdef.view.matrix, fwd, left, up, eye);

	for (i = 0, v = 0, e = 0; i < numsegs; i++)
	{
		const float *a  = (const float *)(segs + (size_t)i * segbytes);
		const float *b  = a + 3;
		float ra = a[6], rb = a[7];
		const float *ca = a + 8, *cb = a + 11;
		float bake = a[14];
		float shape = bake >= 1.5f ? a[15] : falloff;		// CapsuleShape.z: a bolt's falloff, a shell's boil amplitude
		vec3_t axis, tocam, side, ea, eb, corner;
		float len, cap, wa, wb;
		int base = v;

		VectorSubtract(b, a, axis);
		len = (float)VectorLength(axis);
		if (len < 1e-4f)
		{
			// A ZERO-LENGTH capsule is a SPHERE -- the ball lightning's shell (bake
			// 2, r_lightning.c CL_Beam_M5_AddBall). Its quad has no axis of its
			// own, so it takes the view's up: a camera-facing square of side 2R.
			if (bake < 1.5f)
				continue;
			VectorCopy(up, axis);
			len = 0.0f;
		}
		else
			VectorScale(axis, 1.0f / len, axis);
		// Camera-facing about the segment's own axis. Degenerate when the segment
		// points straight at the eye, which under cl_beams_instantaimhack is the
		// normal case for your own bolt, so it falls back exactly as the ribbon
		// emitter does rather than emitting a zero-area quad.
		VectorSubtract(eye, a, tocam);
		CrossProduct(axis, tocam, side);
		if (VectorLength2(side) < 1e-6f)
		{
			CrossProduct(axis, up, side);
			if (VectorLength2(side) < 1e-6f)
				CrossProduct(axis, left, side);
			if (VectorLength2(side) < 1e-6f)
				continue;
		}
		VectorNormalize(side);

		// Extend past both ends by the local radius so the rounded caps have
		// somewhere to be evaluated, and widen by the same so the profile reaches
		// zero inside the quad rather than at its edge.
		// PAD FOR THE FIZZ. The profile reaches zero exactly AT the quad edge
		// (wa is exactly the radius), so a boil that GROWS R would be cut square
		// by the quad rather than falling off -- unmistakable, and the reason
		// this pad is not optional. The shader's noise is normalised and clamped
		// to [-1,1], so R can never exceed base * fizzpad and the pad is a
		// guarantee. Exactly 1.0 when the cvar is 0, i.e. the old geometry.
		// a shell (bake 2) boils its silhouette by up to 16% in the shader, so its
		// pad is at least 1.3 whatever the bolt's fizz is set to
		// -- by its boil AMPLITUDE (a[15], r_lightning.c m5sdfseg_t.boil, 0.16 gentle
		// to 0.42 violent since 2026-09-10), which the shell carries down to the
		// shader in CapsuleShape.z, the slot a bolt uses for its falloff
		if (bake >= 1.5f)
		{
			float sp = max(fizzpad, 1.05f + a[15]);
			cap = max(ra, rb) * sp;
			wa = ra * sp; wb = rb * sp;
		}
		else
		{
			cap = max(ra, rb) * fizzpad;
			wa = ra * fizzpad; wb = rb * fizzpad;
		}
		VectorMA(a, -cap, axis, ea);
		VectorMA(b,  cap, axis, eb);

		// corner order: a-side, a+side, b+side, b-side
		VectorMA(ea, -wa, side, corner); VectorCopy(corner, (m5bolt_vert3f + v * 3));
		m5bolt_tc0[v * 2 + 0] = -cap; m5bolt_tc0[v * 2 + 1] = -wa;
		m5bolt_tc1[v * 3 + 0] = len;  m5bolt_tc1[v * 3 + 1] = ra; m5bolt_tc1[v * 3 + 2] = shape;
		if (bake >= 1.5f) { m5bolt_tc2[v * 3 + 0] = a[0]; m5bolt_tc2[v * 3 + 1] = a[1]; m5bolt_tc2[v * 3 + 2] = a[2]; }	// a shell carries its CENTRE here, not the arc
		else { m5bolt_tc2[v * 3 + 0] = arc; m5bolt_tc2[v * 3 + 1] = 0.0f; m5bolt_tc2[v * 3 + 2] = 0.0f; }
		m5bolt_color4f[v * 4 + 0] = ca[0]; m5bolt_color4f[v * 4 + 1] = ca[1]; m5bolt_color4f[v * 4 + 2] = ca[2]; m5bolt_color4f[v * 4 + 3] = bake;
		v++;
		VectorMA(ea,  wa, side, corner); VectorCopy(corner, (m5bolt_vert3f + v * 3));
		m5bolt_tc0[v * 2 + 0] = -cap; m5bolt_tc0[v * 2 + 1] =  wa;
		m5bolt_tc1[v * 3 + 0] = len;  m5bolt_tc1[v * 3 + 1] = ra; m5bolt_tc1[v * 3 + 2] = shape;
		if (bake >= 1.5f) { m5bolt_tc2[v * 3 + 0] = a[0]; m5bolt_tc2[v * 3 + 1] = a[1]; m5bolt_tc2[v * 3 + 2] = a[2]; }	// a shell carries its CENTRE here, not the arc
		else { m5bolt_tc2[v * 3 + 0] = arc; m5bolt_tc2[v * 3 + 1] = 0.0f; m5bolt_tc2[v * 3 + 2] = 0.0f; }
		m5bolt_color4f[v * 4 + 0] = ca[0]; m5bolt_color4f[v * 4 + 1] = ca[1]; m5bolt_color4f[v * 4 + 2] = ca[2]; m5bolt_color4f[v * 4 + 3] = bake;
		v++;
		VectorMA(eb,  wb, side, corner); VectorCopy(corner, (m5bolt_vert3f + v * 3));
		m5bolt_tc0[v * 2 + 0] = len + cap; m5bolt_tc0[v * 2 + 1] =  wb;
		m5bolt_tc1[v * 3 + 0] = len;       m5bolt_tc1[v * 3 + 1] = rb; m5bolt_tc1[v * 3 + 2] = shape;
		if (bake >= 1.5f) { m5bolt_tc2[v * 3 + 0] = a[0]; m5bolt_tc2[v * 3 + 1] = a[1]; m5bolt_tc2[v * 3 + 2] = a[2]; }	// a shell carries its CENTRE here, not the arc
		else { m5bolt_tc2[v * 3 + 0] = arc; m5bolt_tc2[v * 3 + 1] = 0.0f; m5bolt_tc2[v * 3 + 2] = 0.0f; }
		m5bolt_color4f[v * 4 + 0] = cb[0]; m5bolt_color4f[v * 4 + 1] = cb[1]; m5bolt_color4f[v * 4 + 2] = cb[2]; m5bolt_color4f[v * 4 + 3] = bake;
		v++;
		VectorMA(eb, -wb, side, corner); VectorCopy(corner, (m5bolt_vert3f + v * 3));
		m5bolt_tc0[v * 2 + 0] = len + cap; m5bolt_tc0[v * 2 + 1] = -wb;
		m5bolt_tc1[v * 3 + 0] = len;       m5bolt_tc1[v * 3 + 1] = rb; m5bolt_tc1[v * 3 + 2] = shape;
		if (bake >= 1.5f) { m5bolt_tc2[v * 3 + 0] = a[0]; m5bolt_tc2[v * 3 + 1] = a[1]; m5bolt_tc2[v * 3 + 2] = a[2]; }	// a shell carries its CENTRE here, not the arc
		else { m5bolt_tc2[v * 3 + 0] = arc; m5bolt_tc2[v * 3 + 1] = 0.0f; m5bolt_tc2[v * 3 + 2] = 0.0f; }
		m5bolt_color4f[v * 4 + 0] = cb[0]; m5bolt_color4f[v * 4 + 1] = cb[1]; m5bolt_color4f[v * 4 + 2] = cb[2]; m5bolt_color4f[v * 4 + 3] = bake;
		v++;

		m5bolt_element3i[e + 0] = base + 0; m5bolt_element3i[e + 1] = base + 1; m5bolt_element3i[e + 2] = base + 2;
		m5bolt_element3i[e + 3] = base + 0; m5bolt_element3i[e + 4] = base + 2; m5bolt_element3i[e + 5] = base + 3;
		m5bolt_element3s[e + 0] = (unsigned short)(base + 0); m5bolt_element3s[e + 1] = (unsigned short)(base + 1); m5bolt_element3s[e + 2] = (unsigned short)(base + 2);
		m5bolt_element3s[e + 3] = (unsigned short)(base + 0); m5bolt_element3s[e + 4] = (unsigned short)(base + 2); m5bolt_element3s[e + 5] = (unsigned short)(base + 3);
		e += 6;
		// AFTER the corners, which all carry this segment's own starting arc. A
		// plain running sum over the whole stream is enough -- it need not be a
		// true per-bolt arc length, only monotone and continuous WITHIN a line,
		// which is what removes the joint repeat and makes the boil agree across
		// a joint. Forks simply start their own offset, which is harmless.
		arc += len;
		// the element arrays are unsigned short on one side: stop before wrapping
		if (v + 4 > 65535)
			break;
	}
	if (e < 3)
		return;
	if (developer.integer >= 2)
	{
		static double lastreport;
		if (cl.time - lastreport > 1.0)
		{
			lastreport = cl.time;
			Con_Printf("M5 bolt SDF: segs %d verts %d tris %d target %dx%d\n", numsegs, v, e / 3, viewwidth, viewheight);
		}
	}

	// ---- accumulate ----
	// ORDER IS LOAD-BEARING on Metal: set the target, then turn the scissor off,
	// then clear. A clear issued before the target switch lands on the OLD target,
	// and a clear with the scissor still on is a drawn quad rather than the free
	// load-action clear the backend turns this sequence into.
	R_Mesh_SetRenderTargets(rt->fbo);
	GL_ScissorTest(false);
	{
		// TRANSPARENT black, not GL_Clear's opaque default: the alpha channel is
		// the OCCLUSION the ball's shell accumulates (under max, like the colour)
		// and the composite dims the scene by it, so a cleared alpha of 1 would
		// black the whole frame out.
		static const float clearcolor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		GL_Clear(GL_COLOR_BUFFER_BIT, clearcolor, 1.0f, 0);
	}
	// The live 3D projection, rebased to this target's origin. A COPY: writing
	// r_refdef.view.viewport itself would follow the frame out of this function.
	vp = r_refdef.view.viewport;
	vp.x = 0;
	vp.y = 0;
	R_SetViewport(&vp);
	R_EntityMatrix(&identitymatrix);
	GL_DepthTest(false);			// no depth attachment here; occlusion is in the shader
	GL_DepthMask(false);
	GL_CullFace(GL_NONE);
	GL_PolygonOffset(0, 0);
	GL_Color(1, 1, 1, 1);
	GL_BlendFunc(GL_ONE, GL_ONE);
	GL_BlendEquationEx(DPBLENDOP_MAX);
	R_Mesh_PrepareVertices_Mesh_Arrays(v, m5bolt_vert3f, m5bolt_tc1, m5bolt_tc2, NULL, m5bolt_color4f, m5bolt_tc0, NULL);
	R_SetupShader_SetPermutationGLSL(SHADERMODE_M5BOLT, 0);
	// (amplitude, scene time, cycles per world unit, cycles per second along
	// the beam). Amplitude 0 is the old capsule byte for byte -- the shader
	// branches on it and the CPU pad above collapses to 1.0.
	if (r_glsl_permutation->loc_BoltFizz >= 0)
	{
		// CHANGE-ONLY: this is a per-frame path and an unconditional print here is
		// the console-ink out-of-memory hazard. It is also the fizz's only textual
		// evidence -- the arm is inside a mode that appears in no permutation
		// number -- so it is what a smoke check can hold on to.
		static int lastboltfizz = -1;
		int boltfizzstate = (int)(fizz * 1000.0f);
		if (boltfizzstate != lastboltfizz)
		{
			lastboltfizz = boltfizzstate;
			if (fizz > 0.0f)
				Con_DPrintf("m5bolt fizz armed (amplitude %.2f, %.3f cycles/unit, %.1f/s)\n",
					fizz, max(0.0f, r_lightningbeam_m5_fizz_scale.value), r_lightningbeam_m5_fizz_speed.value);
			else
				Con_DPrintf("m5bolt fizz off\n");
		}
		R_Shader_Uniform4f(r_glsl_permutation->loc_BoltFizz, fizz, (float)r_refdef.scene.time,
			max(0.0f, r_lightningbeam_m5_fizz_scale.value), r_lightningbeam_m5_fizz_speed.value);
	}
	// the ball's shell bulges its quad toward the eye to anchor its boil in the
	// world; set explicitly rather than trusting whatever the last surface left
	if (r_glsl_permutation->loc_BoltEye >= 0)
		R_Shader_Uniform3f(r_glsl_permutation->loc_BoltEye, r_refdef.view.origin[0], r_refdef.view.origin[1], r_refdef.view.origin[2]);
	// the clip planes, to linearise the depths the shroud compares
	if (r_glsl_permutation->loc_BoltClip >= 0)
		R_Shader_Uniform2f(r_glsl_permutation->loc_BoltClip, (float)r_refdef.nearclip, (float)r_refdef.farclip);
	// the fog's shroud: this frame's murk output, or white (transmittance 1)
	if (r_glsl_permutation->tex_Texture_Second >= 0)
	{
		if (m5_fogshroud_frame == (int)host.framecount && m5_fogshroud_rt)
			R_Mesh_TexBind(r_glsl_permutation->tex_Texture_Second, m5_fogshroud_rt);
		else if (m5_fogshroud_frame == (int)host.framecount && m5_fogshroud_gl && vid.renderpath == RENDERPATH_METAL)
			Metal_Backend_TexBind(r_glsl_permutation->tex_Texture_Second, (int)m5_fogshroud_gl);
		else
			R_Mesh_TexBind(r_glsl_permutation->tex_Texture_Second, r_texture_white);
	}
	R_Mesh_TexBind(r_glsl_permutation->tex_Texture_ScreenDepth, viewdepthtexture);
	R_Mesh_Draw(0, v, 0, e / 3, m5bolt_element3i, NULL, 0, m5bolt_element3s, NULL, 0);
	// back to ADD immediately: coronas are the very next draws in this frame and
	// the equation is sticky (r_shadow.c's bracket discipline, same reason)
	GL_BlendEquationEx(DPBLENDOP_ADD);

	// ---- composite ----
	// PREMULTIPLIED over, through the stock GENERIC shader: the accumulation
	// buffer holds the colour the bolt should ADD and, in its alpha, the
	// occlusion the ball's shell wants to SUBTRACT from the scene behind it
	// (a capsule writes alpha 0, so with no ball in frame this is exactly the
	// old additive composite). suppresstexalpha MUST stay false here -- the
	// alpha is the whole point -- for the same reason the murk's composite
	// documents at length.
	R_ResetViewRendering2D(viewfbo, viewdepthtexture, viewcolortexture, viewx, viewy, viewwidth, viewheight);
	GL_DepthTest(false);
	GL_DepthMask(false);
	GL_BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	GL_Color(1, 1, 1, 1);
	R_Mesh_PrepareVertices_Generic_Arrays(4, r_screenvertex3f, NULL, rt->texcoord2f);
	R_SetupShader_Generic(rt->colortexture[0], false, true, false);
	R_Mesh_Draw(0, 4, 0, 2, polygonelement3i, NULL, 0, polygonelement3s, NULL, 0);

	// hand the 3D view back exactly as it was found -- the parameter tuple, never
	// r_fb.rt_screen, because a water sub-view's tuple is not the main view's
	R_ResetViewRendering3D(viewfbo, viewdepthtexture, viewcolortexture, viewx, viewy, viewwidth, viewheight);
	if (r_timereport_active)
		R_TimeReport("m5bolt");
}

// viewdepthtexture is the SOURCE scene depth (rt_screen's depth attachment) when it is
// sampleable, else NULL; the volumetric debug visualiser reads it. fbo/depthtexture/
// colortexture/x/y/width/height describe the DESTINATION, as before.
static void R_BlendView(rtexture_t *viewcolortexture, rtexture_t *viewdepthtexture, int fbo, rtexture_t *depthtexture, rtexture_t *colortexture, int x, int y, int width, int height)
{
	uint64_t permutation;
	float uservecs[4][4];
	rtexture_t *viewtexture;
	rtexture_t *bloomtexture;
	rtexture_t *debugfield;

	R_EntityMatrix(&identitymatrix);

	// Resolve the debug visualiser's field BEFORE the shader is set up: the first
	// call bakes it, and baking uploads a texture, which is not something to do in
	// the middle of a R_Mesh_TexBind sequence. The noise volume stands in when the
	// field is not wanted or will not build, purely so the sampler3D is never left
	// unbound while the program that declares it is current.
	// the baked world field: the debug visualiser's modes 5 and 6 want it, and so
	// does the heat shimmer, which reads the same two channels to find lava
	debugfield = ((r_volumetric.integer && r_volumetric_debug.integer >= 5) || R_LavaShimmer_Wanted()) ? R_Volumetric_GetField() : NULL;
	if (!debugfield && r_volumetric.integer && r_volumetric_debug.integer)
		debugfield = R_Volumetric_GetNoiseTexture();

	if (r_fb.bloomwidth)
	{
		// make the bloom texture
		R_Bloom_MakeTexture();
	}

#if _MSC_VER >= 1400
#define sscanf sscanf_s
#endif
	memset(uservecs, 0, sizeof(uservecs));
	if (r_glsl_postprocess_uservec1_enable.integer)
		sscanf(r_glsl_postprocess_uservec1.string, "%f %f %f %f", &uservecs[0][0], &uservecs[0][1], &uservecs[0][2], &uservecs[0][3]);
	if (r_glsl_postprocess_uservec2_enable.integer)
		sscanf(r_glsl_postprocess_uservec2.string, "%f %f %f %f", &uservecs[1][0], &uservecs[1][1], &uservecs[1][2], &uservecs[1][3]);
	if (r_glsl_postprocess_uservec3_enable.integer)
		sscanf(r_glsl_postprocess_uservec3.string, "%f %f %f %f", &uservecs[2][0], &uservecs[2][1], &uservecs[2][2], &uservecs[2][3]);
	if (r_glsl_postprocess_uservec4_enable.integer)
		sscanf(r_glsl_postprocess_uservec4.string, "%f %f %f %f", &uservecs[3][0], &uservecs[3][1], &uservecs[3][2], &uservecs[3][3]);

	// render to the screen fbo
	R_ResetViewRendering2D(fbo, depthtexture, colortexture, x, y, width, height);
	GL_Color(1, 1, 1, 1);
	GL_BlendFunc(GL_ONE, GL_ZERO);

	viewtexture = viewcolortexture;
	bloomtexture = r_fb.rt_bloom ? r_fb.rt_bloom->colortexture[0] : NULL;

	if (r_rendertarget_debug.integer >= 0)
	{
		r_rendertarget_t *rt = (r_rendertarget_t *)Mem_ExpandableArray_RecordAtIndex(&r_fb.rendertargets, r_rendertarget_debug.integer);
		if (rt && rt->colortexture[0])
		{
			viewtexture = rt->colortexture[0];
			bloomtexture = NULL;
		}
	}

	R_Mesh_PrepareVertices_Mesh_Arrays(4, r_screenvertex3f, NULL, NULL, NULL, NULL, r_fb.rt_screen->texcoord2f, bloomtexture ? r_fb.rt_bloom->texcoord2f : NULL);
	switch(vid.renderpath)
	{
	// Metal falls through (METAL.md Phase 4a): every setter in this arm is a
	// loc-guarded R_Shader_Uniform* or an R_Mesh_TexBind, all of which
	// dispatch. Phase 4d implemented the remaining postprocess bits in the MSL,
	// so they no longer degrade to the plain blit; the two that stay unported
	// (POSTPROCESSING, VOLUMETRICDEBUG) sentinel loudly.
	case RENDERPATH_METAL:
	case RENDERPATH_GL32:
	case RENDERPATH_GLES2:
		permutation =
			// SHADERPERMUTATION_BLOOM is derived from BLOOMTEXTURE, not from
			// r_fb.bloomwidth, and the difference is load-bearing on Metal:
			// the bit makes the shader read the bloom texcoords on attribute 6,
			// and that attribute is enabled by the PrepareVertices call above
			// from `bloomtexture` -- which the r_rendertarget_debug branch
			// twelve lines up nulls while leaving bloomwidth set. Derived
			// separately they disagree in exactly that case, and Metal is
			// strict where GL is lax: a stage_in read of an attribute the
			// vertex descriptor omits is a pipeline-creation error, so
			// `r_bloom 1; r_rendertarget_debug 0` failed to build a pipeline at
			// all (measured, and it is the reason this line changed). Deriving
			// both from one value makes them agree by construction. Inert
			// everywhere else: bloomwidth is nonzero exactly when
			// R_Bloom_MakeTexture ran and set rt_bloom, so outside the debug
			// branch the two expressions are equivalent -- which the command
			// digest gate proves rather than assumes.
			(bloomtexture ? SHADERPERMUTATION_BLOOM : 0)
			| (r_refdef.viewblend[3] > 0 ? SHADERPERMUTATION_VIEWTINT : 0)
			| (!vid_gammatables_trivial ? SHADERPERMUTATION_GAMMARAMPS : 0)
			| (r_glsl_postprocess.integer ? SHADERPERMUTATION_POSTPROCESSING : 0)
			| ((!R_Stereo_ColorMasking() && r_glsl_saturation.value != 1) ? SHADERPERMUTATION_SATURATION : 0);
		R_SetupShader_SetPermutationGLSL(SHADERMODE_POSTPROCESS, permutation);
		// The scene's ceiling is the granted display headroom (METAL.md Phase
		// 7-3), read straight from its single writer rather than re-derived from
		// a predicate here. 1.0 until 7-6 gives that field a writer, so this is
		// the old clamp exactly.
		R_Shader_SetGammaAnalyticUniforms(vid.edr_headroom);
		if (r_glsl_permutation->tex_Texture_First           >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_First     , viewtexture);
		if (r_glsl_permutation->tex_Texture_Second          >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_Second    , bloomtexture);
		if (r_glsl_permutation->tex_Texture_GammaRamps      >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_GammaRamps, r_texture_gammaramps       );
		// VOLUMETRICS debug visualiser: bind the scene depth and hand over everything
		// needed to rebuild view- and world-space position from a depth sample. Safe to
		// sample here because R_ResetViewRendering2D above has already bound the
		// DESTINATION framebuffer, so rt_screen's depth is no longer an attachment of
		// the bound target (sampling an attached texture would be a GL feedback loop).
		if (r_glsl_permutation->tex_Texture_ScreenDepth     >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_ScreenDepth, viewdepthtexture);
		// debug modes 5 and 6 read the baked world field, resolved above
		if (r_glsl_permutation->tex_Texture_VolumeField     >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_VolumeField, debugfield);
		if (r_glsl_permutation->loc_VolumetricFieldOrigin   >= 0) R_Shader_Uniform3f(r_glsl_permutation->loc_VolumetricFieldOrigin, r_volumetric_fieldorigin[0], r_volumetric_fieldorigin[1], r_volumetric_fieldorigin[2]);
		if (r_glsl_permutation->loc_VolumetricFieldParams   >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_VolumetricFieldParams, r_volumetric_fieldinvsize[0], r_volumetric_fieldinvsize[1], r_volumetric_fieldinvsize[2], VOL_FIELD_MAXH);
		if (r_glsl_permutation->loc_VolumetricDebugMode     >= 0) R_Shader_Uniform1f(r_glsl_permutation->loc_VolumetricDebugMode, r_fb.scenedepthvalid ? (float)r_volumetric_debug.integer : 0.0f);
		if (r_glsl_permutation->loc_ScreenToDepth           >= 0) R_Shader_Uniform2f(r_glsl_permutation->loc_ScreenToDepth      , r_fb.scenedepth_screentodepth[0], r_fb.scenedepth_screentodepth[1]);
		if (r_glsl_permutation->loc_FrustumScale            >= 0) R_Shader_Uniform2f(r_glsl_permutation->loc_FrustumScale       , r_fb.scenedepth_frustum_x, r_fb.scenedepth_frustum_y);
		if (r_glsl_permutation->loc_VolumetricFarClip       >= 0) R_Shader_Uniform1f(r_glsl_permutation->loc_VolumetricFarClip  , r_fb.scenedepth_farclip);
		if (r_glsl_permutation->loc_ViewToWorld             >= 0) {float m16f[16];Matrix4x4_ToArrayFloatGL(&r_fb.scenedepth_viewtoworld, m16f);R_Shader_UniformMatrix4fv(r_glsl_permutation->loc_ViewToWorld, 1, false, m16f);}
		// HEAT SHIMMER: displacement in pixels; the reach above the surface,
		// PRE-DIVIDED by the field's signed-distance scale so the shader needs no
		// second uniform; the distance cutoff; and the ripple's clock. Zeroed unless
		// the field baked AND the depth block above really published, so a map with
		// no lava, or a frame that never got a sampleable depth, cannot wobble.
		// The reach is CAPPED at the field's own signed-distance range, and that cap
		// is what keeps the far field silent: the bake truncates the distance a
		// couple of cells out, so a reach beyond it leaves every dry cell whose
		// nearest liquid is lava reading as faintly hot -- which the march would
		// then sum once per tap. Capped, the falloff is guaranteed to have reached
		// exactly zero by the truncation. It is a cap and not a clamp on the cvar so
		// that a map with a finer bake quietly uses the largest reach it can support
		// rather than the player's value being rewritten under them.
		if (r_glsl_permutation->loc_LavaShimmer             >= 0)
		{
			float reach = max(1.0f, min(r_lavashimmer_height.value, r_volumetric_fieldsdfrange));
			// Announce the EFFECTIVE reach, because the cap above is otherwise
			// silent and a player who raised the cvar deserves to know it did
			// nothing. Not a per-frame print despite firing on change: both inputs
			// are a cvar and a per-map bake constant, neither of which can move
			// within a frame -- which is the distinction the console-flood incident
			// turned on. It is also the smoke net for the march being live.
			if (debugfield && r_fb.scenedepthvalid
			 && (reach != r_lavashimmer_lastreach || r_volumetric_fieldsdfrange != r_lavashimmer_lastsdf))
			{
				r_lavashimmer_lastreach = reach;
				r_lavashimmer_lastsdf = r_volumetric_fieldsdfrange;
				Con_DPrintf("M5 lava: heat haze marching %d taps, reach %.0f of %.0f max, full at %.0f units of hot air\n",
					bound(1, r_lavashimmer_taps.integer, 16), reach, r_volumetric_fieldsdfrange, max(1.0f, r_lavashimmer_path.value));
			}
			R_Shader_Uniform4f(r_glsl_permutation->loc_LavaShimmer,
				(debugfield && r_fb.scenedepthvalid) ? max(0.0f, r_lavashimmer.value) : 0.0f,
				reach / (2.0f * max(1.0f, r_volumetric_fieldsdfrange)),
				max(1.0f, r_lavashimmer_dist.value), (float)r_refdef.scene.time);
		}
		// .z is the tap count for the along-the-ray march and .w the reciprocal of
		// the path length that saturates it, so the shader divides nothing. The tap
		// count is clamped here rather than in the shader because it is a loop bound:
		// a console typo of -1 or 10000 must not reach the GPU.
		if (r_glsl_permutation->loc_LavaShimmer2            >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_LavaShimmer2,
			max(0.001f, r_lavashimmer_scale.value), max(0.0f, r_lavashimmer_speed.value),
			(float)bound(1, r_lavashimmer_taps.integer, 16), 1.0f / max(1.0f, r_lavashimmer_path.value));
		// the murk's tiling noise volume is the turbulence source; it self-creates
		// with a white fallback, so this cannot fail to a black frame
		if (r_glsl_permutation->tex_Texture_VolumeNoise     >= 0) R_Mesh_TexBind(r_glsl_permutation->tex_Texture_VolumeNoise, R_Volumetric_GetNoiseTexture());
		if (r_glsl_permutation->loc_ViewTintColor           >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_ViewTintColor     , r_refdef.viewblend[0], r_refdef.viewblend[1], r_refdef.viewblend[2], r_refdef.viewblend[3]);
		if (r_glsl_permutation->loc_PixelSize               >= 0) R_Shader_Uniform2f(r_glsl_permutation->loc_PixelSize         , 1.0/r_fb.screentexturewidth, 1.0/r_fb.screentextureheight);
		if (r_glsl_permutation->loc_UserVec1                >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_UserVec1          , uservecs[0][0], uservecs[0][1], uservecs[0][2], uservecs[0][3]);
		if (r_glsl_permutation->loc_UserVec2                >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_UserVec2          , uservecs[1][0], uservecs[1][1], uservecs[1][2], uservecs[1][3]);
		if (r_glsl_permutation->loc_UserVec3                >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_UserVec3          , uservecs[2][0], uservecs[2][1], uservecs[2][2], uservecs[2][3]);
		if (r_glsl_permutation->loc_UserVec4                >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_UserVec4          , uservecs[3][0], uservecs[3][1], uservecs[3][2], uservecs[3][3]);
		if (r_glsl_permutation->loc_Saturation              >= 0) R_Shader_Uniform1f(r_glsl_permutation->loc_Saturation        , r_glsl_saturation.value);
		if (r_glsl_permutation->loc_PixelToScreenTexCoord   >= 0) R_Shader_Uniform2f(r_glsl_permutation->loc_PixelToScreenTexCoord, 1.0f/r_fb.screentexturewidth, 1.0f/r_fb.screentextureheight);
		// BEAUTY A1: under r_bloom_m5 the chain already thresholded, so the
		// composite adds it whole (his archived 0.3125 would eat the glow's tail)
		if (r_glsl_permutation->loc_BloomColorSubtract      >= 0) { float bcs = r_bloom_m5.integer ? 0.0f : r_bloom_colorsubtract.value; R_Shader_Uniform4f(r_glsl_permutation->loc_BloomColorSubtract   , bcs, bcs, bcs, 0.0f); }
		if (r_glsl_permutation->loc_ColorFringe             >= 0) R_Shader_Uniform1f(r_glsl_permutation->loc_ColorFringe, r_colorfringe.value );
		// M5 ball lightning's screen pressure: a violet cast from the edges as a
		// ball nears the eye (r_lightning.c measures it per frame; MSL only)
		if (r_glsl_permutation->loc_BallPressure            >= 0) R_Shader_Uniform4f(r_glsl_permutation->loc_BallPressure, 0.45f, 0.15f, 1.0f, CL_Beam_M5_BallPressure());
		// Only meaningful with a FLOAT scene buffer: at r_viewfbo 0/1 the scene is
		// already clamped to 1.0 by the time it reaches here, so a shoulder could
		// only dim the picture, never recover a highlight. Zeroed otherwise, which
		// is also the exact old code path.
		//
		// .y is the asymptote the roll-off aims at -- 1.0 (SDR white) until EDR
		// engages, whereupon it is whatever the OS granted. Same single source as
		// the gamma ceiling above, deliberately: the two must not be able to
		// disagree about how much range the frame has. Note the knee is bounded
		// at 0.99 and NOT rescaled by the headroom: r_hdr_shoulder is authored
		// against SDR white, so the knee stays put and the headroom lengthens the
		// roll-off above it, which is the whole point of having one.
		if (r_glsl_permutation->loc_HdrShoulder             >= 0) R_Shader_Uniform2f(r_glsl_permutation->loc_HdrShoulder,
			(R_ViewFBO() >= 2 || R_EDR_Wanted()) ? bound(0.0f, r_hdr_shoulder.value, 0.99f) : 0.0f,
			vid.edr_headroom);
		break;
	}
	R_Mesh_Draw(0, 4, 0, 2, polygonelement3i, NULL, 0, polygonelement3s, NULL, 0);
	r_refdef.stats[r_stat_bloom_drawpixels] += r_refdef.view.width * r_refdef.view.height;
}

matrix4x4_t r_waterscrollmatrix;

void R_UpdateFog(void)
{
	// Nehahra fog
	if (gamemode == GAME_NEHAHRA)
	{
		if (gl_fogenable.integer)
		{
			r_refdef.oldgl_fogenable = true;
			r_refdef.fog_density = gl_fogdensity.value;
			r_refdef.fog_red = gl_fogred.value;
			r_refdef.fog_green = gl_foggreen.value;
			r_refdef.fog_blue = gl_fogblue.value;
			r_refdef.fog_alpha = 1;
			r_refdef.fog_start = 0;
			r_refdef.fog_end = gl_skyclip.value;
			r_refdef.fog_height = 1<<30;
			r_refdef.fog_fadedepth = 128;
		}
		else if (r_refdef.oldgl_fogenable)
		{
			r_refdef.oldgl_fogenable = false;
			r_refdef.fog_density = 0;
			r_refdef.fog_red = 0;
			r_refdef.fog_green = 0;
			r_refdef.fog_blue = 0;
			r_refdef.fog_alpha = 0;
			r_refdef.fog_start = 0;
			r_refdef.fog_end = 0;
			r_refdef.fog_height = 1<<30;
			r_refdef.fog_fadedepth = 128;
		}
	}

	// fog parms
	r_refdef.fog_alpha = bound(0, r_refdef.fog_alpha, 1);
	r_refdef.fog_start = max(0, r_refdef.fog_start);
	r_refdef.fog_end = max(r_refdef.fog_start + 0.01, r_refdef.fog_end);

	// When the murk has taken the map's fog over (M5_MapFogColor), the classic
	// pass stands down rather than drawing on top of it.
	if (r_refdef.fog_density && r_drawfog.integer && !M5_MapFogColor(NULL))
	{
		r_refdef.fogenabled = true;
		// this is the point where the fog reaches 0.9986 alpha, which we
		// consider a good enough cutoff point for the texture
		// (0.9986 * 256 == 255.6)
		if (r_fog_exp2.integer)
			r_refdef.fogrange = 32 / (r_refdef.fog_density * r_refdef.fog_density) + r_refdef.fog_start;
		else
			r_refdef.fogrange = 2048 / r_refdef.fog_density + r_refdef.fog_start;
		r_refdef.fogrange = bound(r_refdef.fog_start, r_refdef.fogrange, r_refdef.fog_end);
		r_refdef.fograngerecip = 1.0f / r_refdef.fogrange;
		r_refdef.fogmasktabledistmultiplier = FOGMASKTABLEWIDTH * r_refdef.fograngerecip;
		if (strcmp(r_refdef.fogheighttexturename, r_refdef.fog_height_texturename))
			R_BuildFogHeightTexture();
		// fog color was already set
		// update the fog texture
		if (r_refdef.fogmasktable_start != r_refdef.fog_start || r_refdef.fogmasktable_alpha != r_refdef.fog_alpha || r_refdef.fogmasktable_density != r_refdef.fog_density || r_refdef.fogmasktable_range != r_refdef.fogrange)
			R_BuildFogTexture();
		r_refdef.fog_height_texcoordscale = 1.0f / max(0.125f, r_refdef.fog_fadedepth);
		r_refdef.fog_height_tablescale = r_refdef.fog_height_tablesize * r_refdef.fog_height_texcoordscale;
	}
	else
		r_refdef.fogenabled = false;

	// fog color
	if (r_refdef.fog_density)
	{
		r_refdef.fogcolor[0] = r_refdef.fog_red;
		r_refdef.fogcolor[1] = r_refdef.fog_green;
		r_refdef.fogcolor[2] = r_refdef.fog_blue;

		Vector4Set(r_refdef.fogplane, 0, 0, 1, -r_refdef.fog_height);
		r_refdef.fogplaneviewdist = DotProduct(r_refdef.fogplane, r_refdef.view.origin) + r_refdef.fogplane[3];
		r_refdef.fogplaneviewabove = r_refdef.fogplaneviewdist >= 0;
		r_refdef.fogheightfade = -0.5f/max(0.125f, r_refdef.fog_fadedepth);

		{
			vec3_t fogvec;
			VectorCopy(r_refdef.fogcolor, fogvec);
			//   color.rgb *= ContrastBoost * SceneBrightness;
			VectorScale(fogvec, r_refdef.view.colorscale, fogvec);
			r_refdef.fogcolor[0] = bound(0.0f, fogvec[0], 1.0f);
			r_refdef.fogcolor[1] = bound(0.0f, fogvec[1], 1.0f);
			r_refdef.fogcolor[2] = bound(0.0f, fogvec[2], 1.0f);
		}
	}
}

void R_UpdateVariables(void)
{
	// STOCK MODE's only textual evidence. It compiles no permutation of its own and
	// appears in no shader name -- it SUBTRACTS features -- so a console line is the
	// one thing a test can asserton. Change-only, because this is a per-frame path
	// and an unconditional print here is the 92 GB console-ink hazard.
	{
		static int laststock = -1;   // outside {0,1}, so the first evaluation always reports
		if (m5_stock.integer != laststock)
		{
			laststock = m5_stock.integer;
			Con_Printf(laststock ? "M5 stock mode ON -- 1996 rendering (fork features suppressed; replacement world textures skipped at the next map load)\n"
			                     : "M5 stock mode off\n");
		}
	}

	R_Textures_Frame();

	// EDR: the renderer's half of the ask (METAL.md Phase 7-4). One expression,
	// evaluated once a frame, so nothing downstream re-derives it -- the display
	// half is ANDed in vid_metal.m, which is the only place that can see it.
	//
	// r_viewfbo >= 2 is not a nicety: below it the scene buffer is 8-bit and
	// nothing in the frame can exceed 1.0, so there would be no extended range
	// to hand anybody. r_gamma_analytic is 7-1's, and the reason it comes first
	// in the phase -- the LUT is a colour-indexed lookup and cannot carry a
	// value above 1.0 through it at all.
	// Since Phase 8 the r_viewfbo and r_gamma_analytic terms are gone from this
	// predicate ON PURPOSE: R_EDR_Wanted is what FORCES both, so testing the
	// cvars here would make the ask depend on the very things it supplies.
	vid.edr_wanted = R_EDR_Wanted();

	r_refdef.scene.ambientintensity = r_ambient.value * (1.0f / 64.0f);

	r_refdef.farclip = r_farclip_base.value;
	if (r_refdef.scene.worldmodel)
		r_refdef.farclip += r_refdef.scene.worldmodel->radius * r_farclip_world.value * 2;
	r_refdef.nearclip = bound (0.001f, r_nearclip.value, r_refdef.farclip - 1.0f);

	if (r_shadow_frontsidecasting.integer < 0 || r_shadow_frontsidecasting.integer > 1)
		Cvar_SetValueQuick(&r_shadow_frontsidecasting, 1);
	r_refdef.polygonfactor = 0;
	r_refdef.polygonoffset = 0;

	r_refdef.scene.rtworld = r_shadow_realtime_world.integer != 0;
	r_refdef.scene.rtworldshadows = r_shadow_realtime_world_shadows.integer && vid.stencil;
	r_refdef.scene.rtdlight = r_shadow_realtime_dlight.integer != 0 && !gl_flashblend.integer && r_dynamic.integer;
	r_refdef.scene.rtdlightshadows = r_refdef.scene.rtdlight && r_shadow_realtime_dlight_shadows.integer && vid.stencil;
	r_refdef.scene.lightmapintensity = r_refdef.scene.rtworld ? r_shadow_realtime_world_lightmaps.value : 1;
	if (r_refdef.scene.worldmodel)
	{
		r_refdef.scene.lightmapintensity *= r_refdef.scene.worldmodel->lightmapscale;

		// Apply the default lightstyle to the lightmap even on q3bsp
		if (cl.worldmodel && cl.worldmodel->type == mod_brushq3) {
			r_refdef.scene.lightmapintensity *= r_refdef.scene.rtlightstylevalue[0];
		}
	}
	if (r_showsurfaces.integer)
	{
		r_refdef.scene.rtworld = false;
		r_refdef.scene.rtworldshadows = false;
		r_refdef.scene.rtdlight = false;
		r_refdef.scene.rtdlightshadows = false;
		r_refdef.scene.lightmapintensity = 0;
	}

	r_gpuskeletal = false;
	switch(vid.renderpath)
	{
	case RENDERPATH_GL32:
		r_gpuskeletal = r_glsl_skeletal.integer && !r_showsurfaces.integer;
		// fall through
	// METAL.md Phase 4a: Metal joins HERE, below GL32's r_gpuskeletal line (CPU
	// skeletal stays; the GPU path is 4b's call) and above the gamma-ramp
	// build, which was the dormant gap recorded at Phase 3 -- without it
	// r_texture_gammaramps stays NULL forever on Metal, GAMMARAMPS can never
	// bind, and Seb's non-unit gamma renders untranslated. The build itself is
	// a plain BGRA R_LoadTexture2D/R_UpdateTexture, proven on Metal since the
	// texture layer landed.
	case RENDERPATH_METAL:
	case RENDERPATH_GLES2:
		if(!vid_gammatables_trivial)
		{
			if(!r_texture_gammaramps || vid_gammatables_serial != r_texture_gammaramps_serial)
			{
				// build GLSL gamma texture
#define RAMPWIDTH 256
				unsigned short ramp[RAMPWIDTH * 3];
				unsigned char rampbgr[RAMPWIDTH][4];
				int i;

				r_texture_gammaramps_serial = vid_gammatables_serial;

				VID_BuildGammaTables(&ramp[0], RAMPWIDTH);
				for(i = 0; i < RAMPWIDTH; ++i)
				{
					rampbgr[i][0] = (unsigned char) (ramp[i + 2 * RAMPWIDTH] * 255.0 / 65535.0 + 0.5);
					rampbgr[i][1] = (unsigned char) (ramp[i + RAMPWIDTH] * 255.0 / 65535.0 + 0.5);
					rampbgr[i][2] = (unsigned char) (ramp[i] * 255.0 / 65535.0 + 0.5);
					rampbgr[i][3] = 0;
				}
				if (r_texture_gammaramps)
				{
					R_UpdateTexture(r_texture_gammaramps, &rampbgr[0][0], 0, 0, 0, RAMPWIDTH, 1, 1, 0);
				}
				else
				{
					r_texture_gammaramps = R_LoadTexture2D(r_main_texturepool, "gammaramps", RAMPWIDTH, 1, &rampbgr[0][0], TEXTYPE_BGRA, TEXF_FORCELINEAR | TEXF_CLAMP | TEXF_PERSISTENT, -1, NULL);
				}
			}
		}
		else
		{
			// remove GLSL gamma texture
		}
		break;
	}
}

static r_refdef_scene_type_t r_currentscenetype = RST_CLIENT;
static r_refdef_scene_t r_scenes_store[ RST_COUNT ];
/*
================
R_SelectScene
================
*/
void R_SelectScene( r_refdef_scene_type_t scenetype ) {
	if( scenetype != r_currentscenetype ) {
		// store the old scenetype
		r_scenes_store[ r_currentscenetype ] = r_refdef.scene;
		r_currentscenetype = scenetype;
		// move in the new scene
		r_refdef.scene = r_scenes_store[ r_currentscenetype ];
	}
}

/*
================
R_GetScenePointer
================
*/
r_refdef_scene_t * R_GetScenePointer( r_refdef_scene_type_t scenetype )
{
	// of course, we could also add a qbool that provides a lock state and a ReleaseScenePointer function..
	if( scenetype == r_currentscenetype ) {
		return &r_refdef.scene;
	} else {
		return &r_scenes_store[ scenetype ];
	}
}

static int R_SortEntities_Compare(const void *ap, const void *bp)
{
	const entity_render_t *a = *(const entity_render_t **)ap;
	const entity_render_t *b = *(const entity_render_t **)bp;

	// 1. compare model
	if(a->model < b->model)
		return -1;
	if(a->model > b->model)
		return +1;

	// 2. compare skin
	// TODO possibly calculate the REAL skinnum here first using
	// skinscenes?
	if(a->skinnum < b->skinnum)
		return -1;
	if(a->skinnum > b->skinnum)
		return +1;

	// everything we compared is equal
	return 0;
}
static void R_SortEntities(void)
{
	// below or equal 2 ents, sorting never gains anything
	if(r_refdef.scene.numentities <= 2)
		return;
	// sort
	qsort(r_refdef.scene.entities, r_refdef.scene.numentities, sizeof(*r_refdef.scene.entities), R_SortEntities_Compare);
}

/*
================
R_RenderView
================
*/
extern cvar_t r_shadow_bouncegrid;
extern cvar_t v_isometric;
extern void V_MakeViewIsometric(void);
void R_RenderView(int fbo, rtexture_t *depthtexture, rtexture_t *colortexture, int x, int y, int width, int height)
{
	matrix4x4_t originalmatrix = r_refdef.view.matrix, offsetmatrix;
	int viewfbo = 0;
	rtexture_t *viewdepthtexture = NULL;
	rtexture_t *viewcolortexture = NULL;
	int viewx = r_refdef.view.x, viewy = r_refdef.view.y, viewwidth = r_refdef.view.width, viewheight = r_refdef.view.height;
	qbool skipblend;

	// METAL.md Phase 4a: THE LAST DOOR IS OPEN. Phase 0 refused here because
	// every qgl* pointer is NULL on this path; the opaque-world pipeline now
	// runs through the Metal backend instead. Entities render as the magenta
	// sentinel until Phase 4b -- unmistakable on purpose, never black.
	// finish any 2D rendering that was queued
	DrawQ_Finish();

	if (r_timereport_active)
		R_TimeReport("start");
	r_textureframe++; // used only by R_GetCurrentTexture
	rsurface.entity = NULL; // used only by R_GetCurrentTexture and RSurf_ActiveModelEntity

	if(R_CompileShader_CheckStaticParms())
		R_GLSL_Restart_f(cmd_local);

	if (!r_drawentities.integer)
		r_refdef.scene.numentities = 0;
	else if (r_sortentities.integer)
		R_SortEntities();

	R_AnimCache_ClearCache();

	/* adjust for stereo display */
	if(R_Stereo_Active())
	{
		Matrix4x4_CreateFromQuakeEntity(&offsetmatrix, 0, r_stereo_separation.value * (0.5f - r_stereo_side), 0, 0, r_stereo_angle.value * (0.5f - r_stereo_side), 0, 1);
		Matrix4x4_Concat(&r_refdef.view.matrix, &originalmatrix, &offsetmatrix);
	}

	if (r_refdef.view.isoverlay)
	{
		// TODO: FIXME: move this into its own backend function maybe? [2/5/2008 Andreas]
		R_Mesh_SetRenderTargets(0);
		GL_Clear(GL_DEPTH_BUFFER_BIT, NULL, 1.0f, 0);
		R_TimeReport("depthclear");

		r_refdef.view.showdebug = false;

		r_fb.water.enabled = false;
		r_fb.water.numwaterplanes = 0;

		R_RenderScene(0, NULL, NULL, r_refdef.view.x, r_refdef.view.y, r_refdef.view.width, r_refdef.view.height);

		r_refdef.view.matrix = originalmatrix;

		CHECKGLERROR
		return;
	}

	if (!r_refdef.scene.entities || r_refdef.view.width * r_refdef.view.height == 0 || !r_renderview.integer || cl_videoplaying/* || !r_refdef.scene.worldmodel*/)
	{
		r_refdef.view.matrix = originalmatrix;
		return;
	}

	r_refdef.view.usevieworiginculling = !r_trippy.value && r_refdef.view.useperspective;
	if (v_isometric.integer && r_refdef.view.ismain)
		V_MakeViewIsometric();

	// STOCK MODE takes the scene-brightness default of 1: this is a LIGHTING
	// scale, not panel calibration, and a value tuned against the RT term
	// under-exposes a baked lightmap. Read-side, so nothing is written.
	r_refdef.view.colorscale = (m5_stock.integer ? 1.0f : r_hdr_scenebrightness.value) * r_hdr_irisadaptation_value.value;

	if(vid_sRGB.integer && vid_sRGB_fallback.integer && !vid.sRGB3D)
		// in sRGB fallback, behave similar to true sRGB: convert this
		// value from linear to sRGB
		r_refdef.view.colorscale = Image_sRGBFloatFromLinearFloat(r_refdef.view.colorscale);

	R_RenderView_UpdateViewVectors();

	R_Shadow_UpdateWorldLightSelection();

	// this will set up r_fb.rt_screen
	R_Bloom_StartFrame();

	// apply bloom brightness offset
	if(r_fb.rt_bloom)
		r_refdef.view.colorscale *= r_bloom_scenebrightness.value;

	skipblend = R_BlendView_IsTrivial(r_fb.rt_screen->texturewidth, r_fb.rt_screen->textureheight, width, height);
	if (skipblend)
	{
		// Render to the screen right away.
		viewfbo = fbo;
		viewdepthtexture = depthtexture;
		viewcolortexture = colortexture;
		viewx = x;
		viewy = y;
		viewwidth = width;
		viewheight = height;
	}
	else if (r_fb.rt_screen)
	{
		// R_Bloom_StartFrame probably set up an fbo for us to render into, it will be rendered to the window later in R_BlendView
		viewfbo = r_fb.rt_screen->fbo;
		viewdepthtexture = r_fb.rt_screen->depthtexture;
		viewcolortexture = r_fb.rt_screen->colortexture[0];
		viewx = 0;
		viewy = 0;
		viewwidth = r_fb.rt_screen->texturewidth;
		viewheight = r_fb.rt_screen->textureheight;
	}

	R_Water_StartFrame(viewwidth, viewheight);

	CHECKGLERROR
	if (r_timereport_active)
		R_TimeReport("viewsetup");

	// The MetalFX-TEMPORAL arc: decide this frame's jitter and whether the
	// history survives BEFORE the viewport is built, because R_SetupView reads
	// r_fb.taa_jitter as it finishes the projection matrix.
	R_TAA_BeginFrame(viewwidth, viewheight);

	R_ResetViewRendering3D(viewfbo, viewdepthtexture, viewcolortexture, viewx, viewy, viewwidth, viewheight);

	// clear the whole fbo every frame - otherwise the driver will consider
	// it to be an inter-frame texture and stall in multi-gpu configurations
	if (r_fb.rt_screen)
		GL_ScissorTest(false);
	R_ClearScreen(r_refdef.fogenabled);
	if (r_timereport_active)
		R_TimeReport("viewclear");

	r_refdef.view.clear = true;

	r_refdef.view.showdebug = true;

	R_View_Update(NULL);
	if (r_timereport_active)
		R_TimeReport("visibility");

	R_AnimCache_CacheVisibleEntities();
	if (r_timereport_active)
		R_TimeReport("animcache");

	R_Shadow_UpdateBounceGridTexture();
	// R_Shadow_UpdateBounceGridTexture called R_TimeReport a few times internally, so we don't need to do that here.

	r_fb.water.numwaterplanes = 0;
	if (r_fb.water.enabled)
		R_RenderWaterPlanes(viewfbo, viewdepthtexture, viewcolortexture, viewx, viewy, viewwidth, viewheight);

	// Decide up front whether the murk will be composited this frame: the opaque
	// surfaces about to be drawn need to know, so they can skip the engine's own
	// per-fragment fog and avoid double-counting it. Transparent surfaces keep it,
	// because they do not write depth and the screen-space pass cannot see them.
	// Any source of murk is enough on its own: r_volumetric_density 0 with
	// r_volumetric_water 1 is the "liquid only, clear air" configuration, and the
	// RT god rays are a light source that needs the pass even with BOTH densities
	// at zero (beams glowing in otherwise clear air).
	r_fb.volumetricactive = r_volumetric.integer && r_volumetric_debug.integer == 0
	                     && (r_volumetric_density.value > 0.0f
	                         || (r_volumetric_water.integer && r_volumetric_waterdensity.value > 0.0f)
#ifdef USE_RT_METAL
	                         || (rt_metal.integer && (rt_metal_shafts.integer || rt_metal_fog.integer))
#endif
	                         )
	                     && r_refdef.view.ismain
	                     && !r_refdef.envmap && !skipblend;

	// A CONFIGURATION THAT CANNOT DELIVER SAYS SO -- ONCE, on change. This is
	// the twin of RT_BuildWorldGeometry's "rt_metal_liquids is INERT here"
	// line, for the defect that one was found next to: turning on Force Water
	// Alpha while the murk is running produces a BROKEN PICTURE IN SILENCE.
	// The moment water renders blended it leaves the opaque pass -- where the
	// murk WAS fogging it correctly -- and the murk's screen-space pass cannot
	// reach it, so it reads at full brightness through fog thick enough to hide
	// the wall behind it. Seb's words on first meeting it: "water looks wrong
	// ... it shines right through the fog". Nothing said so anywhere; the menu
	// row offers the switch and the console offers no explanation at all,
	// which is where he set it from.
	//
	// Change-only, and the change test is what keeps it out of the per-frame
	// console-ink hazard that once cost a 92 GB out-of-memory halt.
	if (cl.worldmodel)
	{
		static int lastliquidfogwarn = -1;
		// the enabling set from R_GetCurrentTexture's own wateralpha test --
		// LOCKSTEP with it, and with cl_screen.c's wateralphalive
		qbool blended = (cl.worldmodel->brush.supportwateralpha || r_wateralpha_force.integer
		              || r_water.integer || r_novis.integer || r_trippy.integer)
		             && r_wateralpha.value < 1.0f;
		// 0 = nothing to say (opaque liquid, or no murk); 1 = the broken
		// configuration; 2 = the fade is carrying it. State 2 is not chatter:
		// it is the feature's ONLY console evidence -- a static parm appears in
		// no permutation number and no shader name -- and it is what smoke run
		// Q asserts to prove the arm compiled and dispatched under validation.
		int state = (r_fb.volumetricactive && blended)
		          ? (r_volumetric_liquidfade.value > 0.0f ? 2 : 1) : 0;
		if (state != lastliquidfogwarn)
		{
			lastliquidfogwarn = state;
			if (state == 1)
				Con_Printf("water is rendering BLENDED and the volumetric murk cannot fog it -- a transparent surface writes no depth, so the murk's screen-space pass never sees it and liquids read at full brightness through fog that hides the wall behind them. r_volumetric_liquidfade 0.5 (the default) fades them per fragment instead; r_wateralpha_force 0 gives opaque water, which the murk fogs correctly.\n");
			else if (state == 2)
				Con_Printf("murk liquid fade armed (strength %.2f) -- blended water and slime are fogged per fragment\n", r_volumetric_liquidfade.value);
		}
	}

	// for the actual view render we use scissoring a fair amount, so scissor
	// test needs to be on
	if (r_fb.rt_screen)
		GL_ScissorTest(true);
	GL_Scissor(r_refdef.view.viewport.x, r_refdef.view.viewport.y, r_refdef.view.viewport.width, r_refdef.view.viewport.height);
	// VOLUMETRICS: publish this frame's scene depth and everything needed to
	// reconstruct position from it, for any later screen-space pass. Main view only
	// (water/reflection and envmap views never set it, so they are skipped for free),
	// and only when the scene actually went to the offscreen target with a sampleable
	// depth texture — r_volumetric forces that path via R_BlendView_IsTrivial.
	// Published BEFORE R_RenderScene: the murk consumes ViewToWorld/FrustumScale
	// from INSIDE the scene render, and publishing after it meant the GL murk
	// reconstructed fresh depth with a one-frame-stale matrix — a subtle swim of
	// the fog against the world during camera motion. Every input here is fixed
	// once R_SetupView has run, so publishing early is safe; the depth CONTENT the
	// murk samples is rendered by the opaque pass, which precedes it in the scene.
	r_fb.scenedepthvalid = 0;
	r_fb.waterseen = false;   // WATERSURFACE: set again by R_GetCurrentTexture as this scene's liquids are queued
	r_fb.refractseen = false;   // BEAUTY A5: set again by R_DrawParticles as this scene's refract particles are queued
	if (r_refdef.view.ismain)
	{
		// WATERSURFACE (r_watersurface_clear): ease the in-water density between its
		// from-the-air and submerged values; snapped to exactly 1 while the feature is
		// off so every density consumer hands over its old bytes.
		float wstarget = (r_watersurface.integer && !cl.view_underwater) ? bound(0.0f, r_watersurface_clear.value, 1.0f) : 1.0f;
		float wsk = min(1.0f, (float)cl.realframetime * 6.0f);
		r_watersurface_clearfactor += (wstarget - r_watersurface_clearfactor) * wsk;
		if (!r_watersurface.integer)
			r_watersurface_clearfactor = 1.0f;
	}
	// The heat shimmer reconstructs world position from this block exactly as the
	// murk does, so it must ask for it too. Gating this on r_volumetric ALONE is
	// what made the first shimmer attempt silently inert: every uniform arrived,
	// the mask read zero because the matrices behind it were never published, and
	// the only visible change was the frame-path flip below.
	// R_MetalFX_TemporalWanted joins the askers for the same reason the shimmer
	// did: the motion-vector pass reconstructs world position from this exact
	// block, and gating it on r_volumetric alone is what made the first shimmer
	// attempt silently inert -- every uniform arrived and the matrices behind
	// them were never published.
	if ((r_volumetric.integer || R_LavaShimmer_Wanted() || R_MetalFX_TemporalWanted() || R_WaterSurface_Wanted() || R_SoftParticles_Wanted()) && r_refdef.view.ismain && !r_refdef.envmap
	 && !skipblend && viewdepthtexture && !R_Stereo_Active())
	{
		// world <- GL view space. r_refdef.view.inverse_matrix is NOT usable here: it
		// inverts r_refdef.view.matrix, which lacks the basematrix axis swap that
		// R_Viewport_InitPerspective folds into viewport.viewmatrix. So invert the
		// viewport's own view matrix, exactly as R_SetupShader_DeferredLight does.
		Matrix4x4_Invert_Full(&r_fb.scenedepth_viewtoworld, &r_refdef.view.viewport.viewmatrix);
		r_fb.scenedepthtexture = viewdepthtexture;
		r_fb.scenedepth_screentodepth[0] = r_refdef.view.viewport.screentodepth[0];
		r_fb.scenedepth_screentodepth[1] = r_refdef.view.viewport.screentodepth[1];
		r_fb.scenedepth_frustum_x = r_refdef.view.frustum_x;
		r_fb.scenedepth_frustum_y = r_refdef.view.frustum_y;
		r_fb.scenedepth_farclip = r_refdef.farclip;
		r_fb.scenedepthvalid = 1;
	}

	// Arm the Metal RT composite for THIS scene render only. Water reflection and
	// refraction views are rendered above, before this point, so they never see it.
	r_fb.rtcompositepending = (fbo == 0 && r_refdef.view.ismain && !r_refdef.envmap);
#ifdef USE_RT_METAL
	// Arm the glow re-add pass (rt_metal_glowpass) with it. RT_Metal_Active()
	// here is the PREVIOUS frame's outcome -- the same one-frame-lagged
	// prediction the wall-lighting fullbright force in SCR_DrawScreen already
	// runs on, so the two agree about which frames are wall-lit. If the
	// composite then bails mid-frame, the glow pass is skipped (the hook
	// re-checks RT_Metal_Active) and emission is withheld for that one frame --
	// the same frame the whole lighting model is already flickering.
	r_fb.rtglowpending = r_fb.rtcompositepending && rt_metal_glowpass.integer
	 && rt_metal.integer && rt_metal_walllight.value > 0.0f && RT_Metal_Active()
	 && !r_showsurfaces.integer && !gl_lightmaps.integer;
#endif
	// The reactive stamp captures ONLY during this, the main scene: the water
	// sub-scenes ran above with ismain still true and taawanted already set,
	// so a sticky predicate would arm them too (their particles are sampled as
	// textures, never shown). The append also refuses under
	// r_fb.water.renderingscene, belt and braces.
	r_stamp_armed = r_fb.taawanted && r_fb.scenedepthvalid && viewdepthtexture
	 && r_metalfx_reactive.value > 0.0f && r_metalfx_reactive_particles.value > 0.0f;
	R_RenderScene(viewfbo, viewdepthtexture, viewcolortexture, viewx, viewy, viewwidth, viewheight);
	r_stamp_armed = false;
	r_fb.water.numwaterplanes = 0;
	r_fb.rtcompositepending = false;
	r_fb.rtglowpending = false;

	// postprocess uses textures that are not aligned with the viewport we're rendering, so no scissoring
	GL_ScissorTest(false);


	R_MotionBlurView(viewfbo, viewdepthtexture, viewcolortexture, viewx, viewy, viewwidth, viewheight);
	if (!skipblend)
	{
		// METAL.md Phase 8-5: under the MetalFX gate the postprocess writes a
		// render-res pooled intermediate and the spatial scaler upscales that
		// into the screen texture -- so gamma, shoulder, FXAA, fringe and the
		// bloom composite all run at render res, which is the whole pixel
		// saving. The HUD then draws over fbo 0 at native res via DrawQ_Start
		// below, which re-selects fbo 0 immediately; nothing draws between
		// the upscale and that re-selection.
		r_rendertarget_t *rt_pp = R_MetalFX_GetPostprocessTarget(fbo, x, y, width, height);
		if (rt_pp)
		{
			// r_fxaa_post: hand the scaler a pooled NATIVE-resolution target and
			// antialias THAT into the real destination, because the postprocess's
			// own FXAA ran at render resolution and cannot see the staircase this
			// upscale is about to create. NULL keeps the scaler writing the screen
			// texture directly, which is the shipped path byte for byte.
			r_rendertarget_t *rt_aa = R_PostAA_Target(width, height);
			int aahandle = rt_aa ? R_GetTexture(rt_aa->colortexture[0]) : 0;
			R_BlendView(viewcolortexture, viewdepthtexture, rt_pp->fbo, rt_pp->depthtexture, rt_pp->colortexture[0], 0, 0, rt_pp->texturewidth, rt_pp->textureheight);
			// An encode-time refusal is the ONE refusal the gate cannot see,
			// and without this fallback it would be a frozen 3D view under a
			// live HUD for the rest of the session (fbo 0 is persistent and
			// every pass loads it). Re-running R_BlendView against the real
			// destination costs one extra postprocess draw on a frame that
			// already failed, and restores the design's per-frame safety
			// property for the whole refusal class.
			if (r_metalfx.integer >= 2)
			{
				// The MetalFX-TEMPORAL arc. The motion pass runs AFTER
				// R_BlendView, not before, for one reason: R_BlendView is what
				// re-selects a render target when it finishes, and running the
				// motion pass first would leave its own target bound for the
				// postprocess to overwrite. Depth is unattached in that pass,
				// so the scene depth this samples is the one R_RenderScene just
				// wrote.
				// METAL_FRAMEMS=3 times the motion pass (fill + entities) as the
				// frame's one region instead of the murk composite. Cost instrument
				// only; the env is read once.
				static int mvregion = -1;
				r_rendertarget_t *rt_mv;
				qbool debugview;
				if (mvregion < 0) { const char *e = getenv("METAL_FRAMEMS"); mvregion = (e && atoi(e) == 3) ? 1 : 0; }
				if (mvregion) Metal_Backend_ProfileRegion(1, "motion pass");
				rt_mv = R_MotionVector_Pass(viewdepthtexture, rt_pp->texturewidth, rt_pp->textureheight);
				// the reactive mask is part of the same region: it is a third quad
				// through the same shader, and its cost belongs with the pass
				r_rendertarget_t *rt_re = rt_mv ? R_ReactiveMask_Pass(viewdepthtexture, rt_pp->texturewidth, rt_pp->textureheight) : NULL;
				if (mvregion) Metal_Backend_ProfileRegion(0, "motion pass");
				debugview = r_metalfx_debugview.value > 0 || (r_metalfx_reactive_debug.integer && rt_re);
				qbool ok = false;
				if (rt_mv)
				{
					if (r_metalfx_reactive_debug.integer && rt_re)
					{
						// Overwrite the postprocessed scene with the REACTIVE
						// MASK -- the same draw the pass just made, so what is
						// shown is what the scaler is handed -- and force the
						// history reset so it is this frame's mask, not an
						// accumulation of masks.
						R_ReactiveMask_Draw(viewdepthtexture, rt_pp->fbo, rt_pp->colortexture[0],
						                    rt_pp->texturewidth, rt_pp->textureheight);
					}
					else if (debugview)
					{
						// Overwrite the postprocessed scene with the motion
						// buffer, about mid grey, and force a history reset
						// every frame so what reaches the screen is THIS
						// frame's vectors rather than an accumulation of them.
						// The cvar's value is the full-scale displacement in
						// pixels, so 20 maps +/-20 px across the colour range.
						R_MotionVector_Draw(viewdepthtexture, rt_pp->fbo, rt_pp->colortexture[0],
						                    rt_pp->texturewidth, rt_pp->textureheight,
						                    1.0f / max(0.001f, r_metalfx_debugview.value));
						R_MotionVector_Entities(rt_pp->fbo, rt_pp->colortexture[0],
						                        rt_pp->texturewidth, rt_pp->textureheight,
						                        1.0f / max(0.001f, r_metalfx_debugview.value));
					}
					ok = Metal_Backend_TemporalUpscaleToScreen(R_GetTexture(rt_pp->colortexture[0]),
					                                           R_GetTexture(viewdepthtexture),
					                                           R_GetTexture(rt_mv->colortexture[0]),
					                                           rt_re ? R_GetTexture(rt_re->colortexture[0]) : 0,
					                                           r_fb.taa_jitter[0] * ((r_metalfx_signs.integer & 1) ? -1.0f : 1.0f),
					                                           r_fb.taa_jitter[1] * ((r_metalfx_signs.integer & 2) ? -1.0f : 1.0f),
					                                           r_fb.taareset || debugview || r_metalfx_reactive_force.integer == 2,
					                                           aahandle);
					if (!ok && aahandle)
					{
						// THE POST-AA DESTINATION MUST NEVER COST THE UPSCALE. A pooled
						// target can be refused by the scaler (its usage bits are fixed
						// when the pool first creates it, which can be before MetalFX's
						// temporal probe has published what it needs), and failing the
						// whole encode would drop Better/Best/Ultimate to NO upscaling
						// at all -- silently, and faster, which is the worst shape a
						// failure can take here. Degrade to no post-AA instead.
						static qbool said;
						if (!said) { said = true; Con_Printf(CON_WARN "MetalFX: the post-upscale FXAA target was refused; r_fxaa_post is inactive this session (the upscale itself is unaffected)\n"); }
						aahandle = 0; rt_aa = NULL;
						ok = Metal_Backend_TemporalUpscaleToScreen(R_GetTexture(rt_pp->colortexture[0]),
					                                           R_GetTexture(viewdepthtexture),
					                                           R_GetTexture(rt_mv->colortexture[0]),
					                                           rt_re ? R_GetTexture(rt_re->colortexture[0]) : 0,
					                                           r_fb.taa_jitter[0] * ((r_metalfx_signs.integer & 1) ? -1.0f : 1.0f),
					                                           r_fb.taa_jitter[1] * ((r_metalfx_signs.integer & 2) ? -1.0f : 1.0f),
					                                           r_fb.taareset || debugview || r_metalfx_reactive_force.integer == 2,
					                                           0);
					}
				}
				if (ok && rt_aa)
					R_PostAA_Draw(rt_aa, fbo, colortexture, x, y, width, height);
				if (!ok)
				{
					// Same contract as the spatial arm: an encode-time refusal
					// is the one refusal the gate cannot see, and without this
					// it would be a frozen 3D view under a live HUD for the
					// rest of the session. The history is stale after it, so
					// the next frame resets.
					r_fb.taavalid = false;
					R_BlendView(viewcolortexture, viewdepthtexture, fbo, depthtexture, colortexture, x, y, width, height);
				}
			}
			else
			{
				qbool sok = Metal_Backend_SpatialUpscaleToScreen(R_GetTexture(rt_pp->colortexture[0]), aahandle);
				if (!sok && aahandle)   // the same contract as the temporal arm above
				{
					rt_aa = NULL;
					sok = Metal_Backend_SpatialUpscaleToScreen(R_GetTexture(rt_pp->colortexture[0]), 0);
				}
				if (!sok)
					R_BlendView(viewcolortexture, viewdepthtexture, fbo, depthtexture, colortexture, x, y, width, height);
				else if (rt_aa)
					R_PostAA_Draw(rt_aa, fbo, colortexture, x, y, width, height);
			}
		}
		else
			R_BlendView(viewcolortexture, viewdepthtexture, fbo, depthtexture, colortexture, x, y, width, height);
	}
	if (r_timereport_active)
		R_TimeReport("blendview");

	// Snapshot the camera this frame was rendered with, for the next frame's
	// motion vectors. After the upscale, so a frame that bailed has already
	// cleared taavalid and the next one resets instead of reprojecting against
	// a camera any number of frames old.
	R_TAA_EndFrame();

	r_refdef.view.matrix = originalmatrix;

	CHECKGLERROR

	// go back to 2d rendering
	DrawQ_Start();
}

void R_RenderWaterPlanes(int viewfbo, rtexture_t *viewdepthtexture, rtexture_t *viewcolortexture, int viewx, int viewy, int viewwidth, int viewheight)
{
	if (cl.csqc_vidvars.drawworld && r_refdef.scene.worldmodel && r_refdef.scene.worldmodel->DrawAddWaterPlanes)
	{
		r_refdef.scene.worldmodel->DrawAddWaterPlanes(r_refdef.scene.worldentity);
		if (r_timereport_active)
			R_TimeReport("waterworld");
	}

	R_DrawModelsAddWaterPlanes();
	if (r_timereport_active)
		R_TimeReport("watermodels");

	if (r_fb.water.numwaterplanes)
	{
		R_Water_ProcessPlanes(viewfbo, viewdepthtexture, viewcolortexture, viewx, viewy, viewwidth, viewheight);
		if (r_timereport_active)
			R_TimeReport("waterscenes");
	}
}

extern cvar_t cl_locs_show;
static void R_DrawLocs(void);
static void R_DrawEntityBBoxes(prvm_prog_t *prog);
static void R_DrawModelDecals(void);
extern qbool r_shadow_usingdeferredprepass;
extern int r_shadow_shadowmapatlas_modelshadows_size;
void R_RenderScene(int viewfbo, rtexture_t *viewdepthtexture, rtexture_t *viewcolortexture, int viewx, int viewy, int viewwidth, int viewheight)
{
	qbool shadowmapping = false;

	if (r_timereport_active)
		R_TimeReport("beginscene");

	r_refdef.stats[r_stat_renders]++;

	R_UpdateFog();

	R_MeshQueue_BeginScene();

	R_SkyStartFrame();

	Matrix4x4_CreateTranslate(&r_waterscrollmatrix, sin(r_refdef.scene.time) * 0.025 * r_waterscroll.value, sin(r_refdef.scene.time * 0.8f) * 0.025 * r_waterscroll.value, 0);

	if (r_timereport_active)
		R_TimeReport("skystartframe");

	if (cl.csqc_vidvars.drawworld)
	{
		if (r_refdef.scene.worldmodel && r_refdef.scene.worldmodel->DrawSky)
		{
			r_refdef.scene.worldmodel->DrawSky(r_refdef.scene.worldentity);
			if (r_timereport_active)
				R_TimeReport("worldsky");
		}

		if (R_DrawBrushModelsSky() && r_timereport_active)
			R_TimeReport("bmodelsky");

		if (skyrendermasked && skyrenderlater)
		{
			// we have to force off the water clipping plane while rendering sky
			R_SetupView(false, viewfbo, viewdepthtexture, viewcolortexture, viewx, viewy, viewwidth, viewheight);
			R_Sky();
			R_SetupView(true, viewfbo, viewdepthtexture, viewcolortexture, viewx, viewy, viewwidth, viewheight);
			if (r_timereport_active)
				R_TimeReport("sky");
		}
	}

	// save the framebuffer info for R_Shadow_RenderMode_Reset during this view render
	r_shadow_viewfbo = viewfbo;
	r_shadow_viewdepthtexture = viewdepthtexture;
	r_shadow_viewcolortexture = viewcolortexture;
	r_shadow_viewx = viewx;
	r_shadow_viewy = viewy;
	r_shadow_viewwidth = viewwidth;
	r_shadow_viewheight = viewheight;

	R_Shadow_PrepareModelShadows();
	R_Shadow_PrepareLights();
	if (r_timereport_active)
		R_TimeReport("preparelights");

	// render all the shadowmaps that will be used for this view
	shadowmapping = R_Shadow_ShadowMappingEnabled();
	if (shadowmapping || r_shadow_shadowmapatlas_modelshadows_size)
	{
		R_Shadow_DrawShadowMaps();
		if (r_timereport_active)
			R_TimeReport("shadowmaps");
	}

	// render prepass deferred lighting if r_shadow_deferred is on, this produces light buffers that will be sampled in forward pass
	if (r_shadow_usingdeferredprepass)
		R_Shadow_DrawPrepass();

	// now we begin the forward pass of the view render
	if (r_depthfirst.integer >= 1 && cl.csqc_vidvars.drawworld && r_refdef.scene.worldmodel && r_refdef.scene.worldmodel->DrawDepth)
	{
		r_refdef.scene.worldmodel->DrawDepth(r_refdef.scene.worldentity);
		if (r_timereport_active)
			R_TimeReport("worlddepth");
	}
	if (r_depthfirst.integer >= 2)
	{
		R_DrawModelsDepth();
		if (r_timereport_active)
			R_TimeReport("modeldepth");
	}

	if (cl.csqc_vidvars.drawworld && r_refdef.scene.worldmodel && r_refdef.scene.worldmodel->Draw)
	{
		r_refdef.scene.worldmodel->Draw(r_refdef.scene.worldentity);
		if (r_timereport_active)
			R_TimeReport("world");
	}

	R_DrawModels();
	if (r_timereport_active)
		R_TimeReport("models");

	if (!r_shadow_usingdeferredprepass)
	{
		R_Shadow_DrawLights();
		if (r_timereport_active)
			R_TimeReport("rtlights");
	}

	if (cl.csqc_vidvars.drawworld)
	{
		R_DrawModelDecals();
		if (r_timereport_active)
			R_TimeReport("modeldecals");

		R_DrawParticles();
		if (r_timereport_active)
			R_TimeReport("particles");

		R_DrawExplosions();
		if (r_timereport_active)
			R_TimeReport("explosions");
	}

	if (r_refdef.view.showdebug)
	{
		if (cl_locs_show.integer)
		{
			R_DrawLocs();
			if (r_timereport_active)
				R_TimeReport("showlocs");
		}

		if (r_drawportals.integer)
		{
			R_DrawPortals();
			if (r_timereport_active)
				R_TimeReport("portals");
		}

		if (r_showbboxes_client.value > 0)
		{
			R_DrawEntityBBoxes(CLVM_prog);
			if (r_timereport_active)
				R_TimeReport("clbboxes");
		}
		if (r_showbboxes.value > 0)
		{
			R_DrawEntityBBoxes(SVVM_prog);
			if (r_timereport_active)
				R_TimeReport("svbboxes");
		}
	}

#ifdef USE_RT_METAL
	// Metal RT shadows/lighting go in HERE, after opaque geometry and crucially
	// BEFORE the murk. In full wall-lighting mode the RT composite is a screen-wide
	// multiply that supplies the scene's entire lighting, so anything composited
	// before it gets lit by it -- and fog is in-scattered light that must not be lit
	// a second time. Compositing the murk first made it vanish entirely with
	// rt_metal 1. The trade is that transparent surfaces, drawn after this, no longer
	// receive the RT term; for self-lit flames and particles that is arguably more
	// correct anyway.
	if (r_fb.rtcompositepending)
	{
		qbool glowpending = r_fb.rtglowpending;
		r_fb.rtcompositepending = false;
		r_fb.rtglowpending = false;   // cleared BEFORE the murk/transparents: their batches must not defer emission
		// TERM UPSAMPLE gate: the composite may sample viewdepthtexture only
		// when it is genuinely the scene's SAMPLEABLE depth -- the main view's
		// rt_screen with the depth-as-texture branch (which the feature's own
		// asker forces whenever it is wanted). A renderbuffer depth or the
		// direct path (viewdepthtexture NULL) sends false and the composite
		// keeps plain bilinear.
		RT_SceneComposite(viewfbo, viewdepthtexture, viewcolortexture, viewx, viewy, viewwidth, viewheight,
			(viewdepthtexture && r_fb.rt_screen && viewdepthtexture == r_fb.rt_screen->depthtexture && !r_fb.rt_screen->depthisrenderbuffer) ? true : false);
		// EMISSION AFTER THE MULTIPLY (rt_metal_glowpass): re-add the withheld
		// glow/redglow additively, only if the composite really multiplied --
		// RT_Metal_Active() is this-frame-accurate from here on.
		if (glowpending && RT_Metal_Active())
			R_RTGlow_Pass();
	}
#endif

	// VOLUMETRIC MURK: after opaque geometry and the RT lighting, before transparent
	// surfaces. Transparent surfaces do not write depth, so fogging them from a
	// depth-driven screen-space pass would place them at whatever opaque surface sits
	// behind them.
	R_Volumetric_RenderFog(viewfbo, viewdepthtexture, viewcolortexture, viewx, viewy, viewwidth, viewheight);

	// WATERSURFACE (r_watersurface): copy the composited frame -- RT term, glow,
	// murk, all of it -- before the transparent pass, so a blended liquid can
	// refract what lies beneath it. Only when a blended liquid was queued for this
	// view (a frame with no water in sight pays nothing), main view only, never
	// inside a water sub-scene. One blit: the texture is the screen target's own
	// type, so on Metal it is a copy and never a conversion.
	r_fb.waterscreen_valid = false;
	if (((r_watersurface.integer && r_fb.waterseen) || (R_PartRefract_Wanted() && r_fb.refractseen)) && r_fb.waterscreen && r_refdef.view.ismain && !r_refdef.envmap
	 && !r_fb.water.renderingscene && !R_Stereo_Active() && viewwidth > 0 && viewheight > 0
	 && viewwidth <= r_fb.screentexturewidth && viewheight <= r_fb.screentextureheight)
	{
		R_Mesh_CopyToTexture(r_fb.waterscreen, 0, 0, viewx, viewy, viewwidth, viewheight);
		r_refdef.stats[r_stat_bloom_copypixels] += viewwidth * viewheight;
		r_fb.waterscreen_valid = true;
	}
	if (r_transparent.integer)
	{
		R_MeshQueue_RenderTransparent();
		if (r_timereport_active)
			R_TimeReport("drawtrans");
	}
#ifdef USE_RT_METAL
	// unconditional: with r_transparent 0 blended surfaces draw inline during
	// the opaque pass and can leave the rectangle term bound -- nothing else
	// ever clears that target
	R_RTLiquid_UnbindAll();
#endif

	// F1: the thunderbolt, accumulated under max() and composited once. Here
	// because this is where the additive ribbon it replaces used to draw -- after
	// the murk (nothing transparent has ever been fogged by it) and before the
	// coronas -- so everything downstream sees the same ordering it always did.
	R_LightningM5_RenderSDF(viewfbo, viewdepthtexture, viewcolortexture, viewx, viewy, viewwidth, viewheight);

	if (r_refdef.view.showdebug && r_refdef.scene.worldmodel && r_refdef.scene.worldmodel->DrawDebug && (r_showtris.value > 0 || r_shownormals.value != 0 || r_showcollisionbrushes.value > 0 || r_showoverdraw.value > 0))
	{
		r_refdef.scene.worldmodel->DrawDebug(r_refdef.scene.worldentity);
		if (r_timereport_active)
			R_TimeReport("worlddebug");
		R_DrawModelsDebug();
		if (r_timereport_active)
			R_TimeReport("modeldebug");
	}

	if (cl.csqc_vidvars.drawworld)
	{
		R_Shadow_DrawCoronas();
		if (r_timereport_active)
			R_TimeReport("coronas");
	}
}

static const unsigned short bboxelements[36] =
{
	5, 1, 3, 5, 3, 7,
	6, 2, 0, 6, 0, 4,
	7, 3, 2, 7, 2, 6,
	4, 0, 1, 4, 1, 5,
	4, 5, 7, 4, 7, 6,
	1, 0, 2, 1, 2, 3,
};

#define BBOXEDGES 13
static const float bboxedges[BBOXEDGES][6] =
{
	// whole box
	{ 0, 0, 0, 1, 1, 1 },
	// bottom edges
	{ 0, 0, 0, 0, 1, 0 },
	{ 0, 0, 0, 1, 0, 0 },
	{ 0, 1, 0, 1, 1, 0 },
	{ 1, 0, 0, 1, 1, 0 },
	// top edges
	{ 0, 0, 1, 0, 1, 1 },
	{ 0, 0, 1, 1, 0, 1 },
	{ 0, 1, 1, 1, 1, 1 },
	{ 1, 0, 1, 1, 1, 1 },
	// vertical edges
	{ 0, 0, 0, 0, 0, 1 },
	{ 1, 0, 0, 1, 0, 1 },
	{ 0, 1, 0, 0, 1, 1 },
	{ 1, 1, 0, 1, 1, 1 },
};

static void R_DrawBBoxMesh(vec3_t mins, vec3_t maxs, float cr, float cg, float cb, float ca)
{
	int numvertices = BBOXEDGES * 8;
	float vertex3f[BBOXEDGES * 8 * 3], color4f[BBOXEDGES * 8 * 4];
	int numtriangles = BBOXEDGES * 12;
	unsigned short elements[BBOXEDGES * 36];
	int i, edge;
	float *v, *c, f1, f2, edgemins[3], edgemaxs[3];

	RSurf_ActiveModelEntity(r_refdef.scene.worldentity, false, false, false);

	GL_BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	GL_DepthMask(false);
	GL_DepthRange(0, 1);
	GL_PolygonOffset(r_refdef.polygonfactor, r_refdef.polygonoffset);

	for (edge = 0; edge < BBOXEDGES; edge++)
	{
		for (i = 0; i < 3; i++)
		{
			edgemins[i] = mins[i] + (maxs[i] - mins[i]) * bboxedges[edge][i] - 0.25f;
			edgemaxs[i] = mins[i] + (maxs[i] - mins[i]) * bboxedges[edge][3 + i] + 0.25f;
		}
		vertex3f[edge * 24 + 0] = edgemins[0]; vertex3f[edge * 24 + 1] = edgemins[1]; vertex3f[edge * 24 + 2] = edgemins[2];
		vertex3f[edge * 24 + 3] = edgemaxs[0]; vertex3f[edge * 24 + 4] = edgemins[1]; vertex3f[edge * 24 + 5] = edgemins[2];
		vertex3f[edge * 24 + 6] = edgemins[0]; vertex3f[edge * 24 + 7] = edgemaxs[1]; vertex3f[edge * 24 + 8] = edgemins[2];
		vertex3f[edge * 24 + 9] = edgemaxs[0]; vertex3f[edge * 24 + 10] = edgemaxs[1]; vertex3f[edge * 24 + 11] = edgemins[2];
		vertex3f[edge * 24 + 12] = edgemins[0]; vertex3f[edge * 24 + 13] = edgemins[1]; vertex3f[edge * 24 + 14] = edgemaxs[2];
		vertex3f[edge * 24 + 15] = edgemaxs[0]; vertex3f[edge * 24 + 16] = edgemins[1]; vertex3f[edge * 24 + 17] = edgemaxs[2];
		vertex3f[edge * 24 + 18] = edgemins[0]; vertex3f[edge * 24 + 19] = edgemaxs[1]; vertex3f[edge * 24 + 20] = edgemaxs[2];
		vertex3f[edge * 24 + 21] = edgemaxs[0]; vertex3f[edge * 24 + 22] = edgemaxs[1]; vertex3f[edge * 24 + 23] = edgemaxs[2];
		for (i = 0; i < 36; i++)
			elements[edge * 36 + i] = edge * 8 + bboxelements[i];
	}
	R_FillColors(color4f, numvertices, cr, cg, cb, ca);
	if (r_refdef.fogenabled)
	{
		for (i = 0, v = vertex3f, c = color4f; i < numvertices; i++, v += 3, c += 4)
		{
			f1 = RSurf_FogVertex(v);
			f2 = 1 - f1;
			c[0] = c[0] * f1 + r_refdef.fogcolor[0] * f2;
			c[1] = c[1] * f1 + r_refdef.fogcolor[1] * f2;
			c[2] = c[2] * f1 + r_refdef.fogcolor[2] * f2;
		}
	}
	R_Mesh_PrepareVertices_Generic_Arrays(numvertices, vertex3f, color4f, NULL);
	R_Mesh_ResetTextureState();
	R_SetupShader_Generic_NoTexture(false, false);
	R_Mesh_Draw(0, numvertices, 0, numtriangles, NULL, NULL, 0, elements, NULL, 0);
}

static void R_DrawEntityBBoxes_Callback(const entity_render_t *ent, const rtlight_t *rtlight, int numsurfaces, int *surfacelist)
{
	// hacky overloading of the parameters
	prvm_prog_t *prog = (prvm_prog_t *)rtlight;
	int i;
	float color[4];
	prvm_edict_t *edict;

	GL_CullFace(GL_NONE);
	R_SetupShader_Generic_NoTexture(false, false);

	for (i = 0;i < numsurfaces;i++)
	{
		edict = PRVM_EDICT_NUM(surfacelist[i]);
		switch ((int)PRVM_serveredictfloat(edict, solid))
		{
			case SOLID_NOT:      Vector4Set(color, 1, 1, 1, 0.05);break;
			case SOLID_TRIGGER:  Vector4Set(color, 1, 0, 1, 0.10);break;
			case SOLID_BBOX:     Vector4Set(color, 0, 1, 0, 0.10);break;
			case SOLID_SLIDEBOX: Vector4Set(color, 1, 0, 0, 0.10);break;
			case SOLID_BSP:      Vector4Set(color, 0, 0, 1, 0.05);break;
			case SOLID_CORPSE:   Vector4Set(color, 1, 0.5, 0, 0.05);break;
			default:             Vector4Set(color, 0, 0, 0, 0.50);break;
		}
		if (prog == CLVM_prog)
			color[3] *= r_showbboxes_client.value;
		else
			color[3] *= r_showbboxes.value;
		color[3] = bound(0, color[3], 1);
		GL_DepthTest(!r_showdisabledepthtest.integer);
		R_DrawBBoxMesh(edict->priv.server->areamins, edict->priv.server->areamaxs, color[0], color[1], color[2], color[3]);
	}
}

static void R_DrawEntityBBoxes(prvm_prog_t *prog)
{
	int i;
	prvm_edict_t *edict;
	vec3_t center;

	if (prog == NULL)
		return;

	for (i = 0; i < prog->num_edicts; i++)
	{
		edict = PRVM_EDICT_NUM(i);
		if (edict->free)
			continue;
		// exclude the following for now, as they don't live in world coordinate space and can't be solid:
		if (PRVM_gameedictedict(edict, tag_entity) != 0)
			continue;
		if (prog == SVVM_prog && PRVM_serveredictedict(edict, viewmodelforclient) != 0)
			continue;
		VectorLerp(edict->priv.server->areamins, 0.5f, edict->priv.server->areamaxs, center);
		R_MeshQueue_AddTransparent(TRANSPARENTSORT_DISTANCE, center, R_DrawEntityBBoxes_Callback, (entity_render_t *)NULL, i, (rtlight_t *)prog);
	}
}

static const int nomodelelement3i[24] =
{
	5, 2, 0,
	5, 1, 2,
	5, 0, 3,
	5, 3, 1,
	0, 2, 4,
	2, 1, 4,
	3, 0, 4,
	1, 3, 4
};

static const unsigned short nomodelelement3s[24] =
{
	5, 2, 0,
	5, 1, 2,
	5, 0, 3,
	5, 3, 1,
	0, 2, 4,
	2, 1, 4,
	3, 0, 4,
	1, 3, 4
};

static const float nomodelvertex3f[6*3] =
{
	-16,   0,   0,
	 16,   0,   0,
	  0, -16,   0,
	  0,  16,   0,
	  0,   0, -16,
	  0,   0,  16
};

static const float nomodelcolor4f[6*4] =
{
	0.0f, 0.0f, 0.5f, 1.0f,
	0.0f, 0.0f, 0.5f, 1.0f,
	0.0f, 0.5f, 0.0f, 1.0f,
	0.0f, 0.5f, 0.0f, 1.0f,
	0.5f, 0.0f, 0.0f, 1.0f,
	0.5f, 0.0f, 0.0f, 1.0f
};

static void R_DrawNoModel_TransparentCallback(const entity_render_t *ent, const rtlight_t *rtlight, int numsurfaces, int *surfacelist)
{
	int i;
	float f1, f2, *c;
	float color4f[6*4];

	RSurf_ActiveCustomEntity(&ent->matrix, &ent->inversematrix, ent->flags, ent->shadertime, ent->colormod[0], ent->colormod[1], ent->colormod[2], ent->alpha, 6, nomodelvertex3f, NULL, NULL, NULL, NULL, nomodelcolor4f, 8, nomodelelement3i, nomodelelement3s, false, false);

	// this is only called once per entity so numsurfaces is always 1, and
	// surfacelist is always {0}, so this code does not handle batches

	if (rsurface.ent_flags & RENDER_ADDITIVE)
	{
		GL_BlendFunc(GL_SRC_ALPHA, GL_ONE);
		GL_DepthMask(false);
	}
	else if (ent->alpha < 1)
	{
		GL_BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		GL_DepthMask(false);
	}
	else
	{
		GL_BlendFunc(GL_ONE, GL_ZERO);
		GL_DepthMask(true);
	}
	GL_DepthRange(0, (rsurface.ent_flags & RENDER_VIEWMODEL) ? 0.0625 : 1);
	GL_PolygonOffset(rsurface.basepolygonfactor, rsurface.basepolygonoffset);
	GL_DepthTest(!(rsurface.ent_flags & RENDER_NODEPTHTEST));
	GL_CullFace((rsurface.ent_flags & RENDER_DOUBLESIDED) ? GL_NONE : r_refdef.view.cullface_back);
	memcpy(color4f, nomodelcolor4f, sizeof(float[6*4]));
	for (i = 0, c = color4f;i < 6;i++, c += 4)
	{
		c[0] *= ent->render_fullbright[0] * r_refdef.view.colorscale;
		c[1] *= ent->render_fullbright[1] * r_refdef.view.colorscale;
		c[2] *= ent->render_fullbright[2] * r_refdef.view.colorscale;
		c[3] *= ent->alpha;
	}
	if (r_refdef.fogenabled)
	{
		for (i = 0, c = color4f;i < 6;i++, c += 4)
		{
			f1 = RSurf_FogVertex(nomodelvertex3f + 3*i);
			f2 = 1 - f1;
			c[0] = (c[0] * f1 + r_refdef.fogcolor[0] * f2);
			c[1] = (c[1] * f1 + r_refdef.fogcolor[1] * f2);
			c[2] = (c[2] * f1 + r_refdef.fogcolor[2] * f2);
		}
	}
//	R_Mesh_ResetTextureState();
	R_SetupShader_Generic_NoTexture(false, false);
	R_Mesh_PrepareVertices_Generic_Arrays(6, nomodelvertex3f, color4f, NULL);
	R_Mesh_Draw(0, 6, 0, 8, nomodelelement3i, NULL, 0, nomodelelement3s, NULL, 0);
}

void R_DrawNoModel(entity_render_t *ent)
{
	vec3_t org;
	Matrix4x4_OriginFromMatrix(&ent->matrix, org);
	if ((ent->flags & RENDER_ADDITIVE) || (ent->alpha < 1))
		R_MeshQueue_AddTransparent((ent->flags & RENDER_NODEPTHTEST) ? TRANSPARENTSORT_HUD : TRANSPARENTSORT_DISTANCE, org, R_DrawNoModel_TransparentCallback, ent, 0, rsurface.rtlight);
	else
		R_DrawNoModel_TransparentCallback(ent, rsurface.rtlight, 0, NULL);
}

void R_CalcBeam_Vertex3f (float *vert, const float *org1, const float *org2, float width)
{
	vec3_t right1, right2, diff, normal;

	VectorSubtract (org2, org1, normal);

	// calculate 'right' vector for start
	VectorSubtract (r_refdef.view.origin, org1, diff);
	CrossProduct (normal, diff, right1);
	VectorNormalize (right1);

	// calculate 'right' vector for end
	VectorSubtract (r_refdef.view.origin, org2, diff);
	CrossProduct (normal, diff, right2);
	VectorNormalize (right2);

	vert[ 0] = org1[0] + width * right1[0];
	vert[ 1] = org1[1] + width * right1[1];
	vert[ 2] = org1[2] + width * right1[2];
	vert[ 3] = org1[0] - width * right1[0];
	vert[ 4] = org1[1] - width * right1[1];
	vert[ 5] = org1[2] - width * right1[2];
	vert[ 6] = org2[0] - width * right2[0];
	vert[ 7] = org2[1] - width * right2[1];
	vert[ 8] = org2[2] - width * right2[2];
	vert[ 9] = org2[0] + width * right2[0];
	vert[10] = org2[1] + width * right2[1];
	vert[11] = org2[2] + width * right2[2];
}

void R_CalcSprite_Vertex3f(float *vertex3f, const vec3_t origin, const vec3_t left, const vec3_t up, float scalex1, float scalex2, float scaley1, float scaley2)
{
	vertex3f[ 0] = origin[0] + left[0] * scalex2 + up[0] * scaley1;
	vertex3f[ 1] = origin[1] + left[1] * scalex2 + up[1] * scaley1;
	vertex3f[ 2] = origin[2] + left[2] * scalex2 + up[2] * scaley1;
	vertex3f[ 3] = origin[0] + left[0] * scalex2 + up[0] * scaley2;
	vertex3f[ 4] = origin[1] + left[1] * scalex2 + up[1] * scaley2;
	vertex3f[ 5] = origin[2] + left[2] * scalex2 + up[2] * scaley2;
	vertex3f[ 6] = origin[0] + left[0] * scalex1 + up[0] * scaley2;
	vertex3f[ 7] = origin[1] + left[1] * scalex1 + up[1] * scaley2;
	vertex3f[ 8] = origin[2] + left[2] * scalex1 + up[2] * scaley2;
	vertex3f[ 9] = origin[0] + left[0] * scalex1 + up[0] * scaley1;
	vertex3f[10] = origin[1] + left[1] * scalex1 + up[1] * scaley1;
	vertex3f[11] = origin[2] + left[2] * scalex1 + up[2] * scaley1;
}

static int R_Mesh_AddVertex(rmesh_t *mesh, float x, float y, float z)
{
	int i;
	float *vertex3f;
	float v[3];
	VectorSet(v, x, y, z);
	for (i = 0, vertex3f = mesh->vertex3f;i < mesh->numvertices;i++, vertex3f += 3)
		if (VectorDistance2(v, vertex3f) < mesh->epsilon2)
			break;
	if (i == mesh->numvertices)
	{
		if (mesh->numvertices < mesh->maxvertices)
		{
			VectorCopy(v, vertex3f);
			mesh->numvertices++;
		}
		return mesh->numvertices;
	}
	else
		return i;
}

void R_Mesh_AddPolygon3f(rmesh_t *mesh, int numvertices, float *vertex3f)
{
	int i;
	int *e, element[3];
	element[0] = R_Mesh_AddVertex(mesh, vertex3f[0], vertex3f[1], vertex3f[2]);vertex3f += 3;
	element[1] = R_Mesh_AddVertex(mesh, vertex3f[0], vertex3f[1], vertex3f[2]);vertex3f += 3;
	e = mesh->element3i + mesh->numtriangles * 3;
	for (i = 0;i < numvertices - 2;i++, vertex3f += 3)
	{
		element[2] = R_Mesh_AddVertex(mesh, vertex3f[0], vertex3f[1], vertex3f[2]);
		if (mesh->numtriangles < mesh->maxtriangles)
		{
			*e++ = element[0];
			*e++ = element[1];
			*e++ = element[2];
			mesh->numtriangles++;
		}
		element[1] = element[2];
	}
}

static void R_Mesh_AddPolygon3d(rmesh_t *mesh, int numvertices, double *vertex3d)
{
	int i;
	int *e, element[3];
	element[0] = R_Mesh_AddVertex(mesh, vertex3d[0], vertex3d[1], vertex3d[2]);vertex3d += 3;
	element[1] = R_Mesh_AddVertex(mesh, vertex3d[0], vertex3d[1], vertex3d[2]);vertex3d += 3;
	e = mesh->element3i + mesh->numtriangles * 3;
	for (i = 0;i < numvertices - 2;i++, vertex3d += 3)
	{
		element[2] = R_Mesh_AddVertex(mesh, vertex3d[0], vertex3d[1], vertex3d[2]);
		if (mesh->numtriangles < mesh->maxtriangles)
		{
			*e++ = element[0];
			*e++ = element[1];
			*e++ = element[2];
			mesh->numtriangles++;
		}
		element[1] = element[2];
	}
}

#define R_MESH_PLANE_DIST_EPSILON (1.0 / 32.0)
void R_Mesh_AddBrushMeshFromPlanes(rmesh_t *mesh, int numplanes, mplane_t *planes)
{
	int planenum, planenum2;
	int w;
	int tempnumpoints;
	mplane_t *plane, *plane2;
	double maxdist;
	double temppoints[2][256*3];
	// figure out how large a bounding box we need to properly compute this brush
	maxdist = 0;
	for (w = 0;w < numplanes;w++)
		maxdist = max(maxdist, fabs(planes[w].dist));
	// now make it large enough to enclose the entire brush, and round it off to a reasonable multiple of 1024
	maxdist = floor(maxdist * (4.0 / 1024.0) + 1) * 1024.0;
	for (planenum = 0, plane = planes;planenum < numplanes;planenum++, plane++)
	{
		w = 0;
		tempnumpoints = 4;
		PolygonD_QuadForPlane(temppoints[w], plane->normal[0], plane->normal[1], plane->normal[2], plane->dist, maxdist);
		for (planenum2 = 0, plane2 = planes;planenum2 < numplanes && tempnumpoints >= 3;planenum2++, plane2++)
		{
			if (planenum2 == planenum)
				continue;
			PolygonD_Divide(tempnumpoints, temppoints[w], plane2->normal[0], plane2->normal[1], plane2->normal[2], plane2->dist, R_MESH_PLANE_DIST_EPSILON, 0, NULL, NULL, 256, temppoints[!w], &tempnumpoints, NULL);
			w = !w;
		}
		if (tempnumpoints < 3)
			continue;
		// generate elements forming a triangle fan for this polygon
		R_Mesh_AddPolygon3d(mesh, tempnumpoints, temppoints[w]);
	}
}

static qbool R_TestQ3WaveFunc(q3wavefunc_t func, const float *parms)
{
	if(parms[0] == 0 && parms[1] == 0)
		return false;
	if(func >> Q3WAVEFUNC_USER_SHIFT) // assumes rsurface to be set!
		if(rsurface.userwavefunc_param[bound(0, (func >> Q3WAVEFUNC_USER_SHIFT) - 1, Q3WAVEFUNC_USER_COUNT - 1)] == 0)
			return false;
	return true;
}

static float R_EvaluateQ3WaveFunc(q3wavefunc_t func, const float *parms)
{
	double index, f;
	index = parms[2] + rsurface.shadertime * parms[3];
	index -= floor(index);
	switch (func & ((1 << Q3WAVEFUNC_USER_SHIFT) - 1))
	{
	default:
	case Q3WAVEFUNC_NONE:
	case Q3WAVEFUNC_NOISE:
	case Q3WAVEFUNC_COUNT:
		f = 0;
		break;
	case Q3WAVEFUNC_SIN: f = sin(index * M_PI * 2);break;
	case Q3WAVEFUNC_SQUARE: f = index < 0.5 ? 1 : -1;break;
	case Q3WAVEFUNC_SAWTOOTH: f = index;break;
	case Q3WAVEFUNC_INVERSESAWTOOTH: f = 1 - index;break;
	case Q3WAVEFUNC_TRIANGLE:
		index *= 4;
		f = index - floor(index);
		if (index < 1)
		{
			// f = f;
		}
		else if (index < 2)
			f = 1 - f;
		else if (index < 3)
			f = -f;
		else
			f = -(1 - f);
		break;
	}
	f = parms[0] + parms[1] * f;
	if(func >> Q3WAVEFUNC_USER_SHIFT) // assumes rsurface to be set!
		f *= rsurface.userwavefunc_param[bound(0, (func >> Q3WAVEFUNC_USER_SHIFT) - 1, Q3WAVEFUNC_USER_COUNT - 1)];
	return (float) f;
}

static void R_tcMod_ApplyToMatrix(matrix4x4_t *texmatrix, q3shaderinfo_layer_tcmod_t *tcmod, int currentmaterialflags)
{
	int w, h, idx;
	float shadertime;
	float f;
	float offsetd[2];
	float tcmat[12];
	matrix4x4_t matrix, temp;
	// if shadertime exceeds about 9 hours (32768 seconds), just wrap it,
	// it's better to have one huge fixup every 9 hours than gradual
	// degradation over time which looks consistently bad after many hours.
	//
	// tcmod scroll in particular suffers from this degradation which can't be
	// effectively worked around even with floor() tricks because we don't
	// know if tcmod scroll is the last tcmod being applied, and for clampmap
	// a workaround involving floor() would be incorrect anyway...
	shadertime = rsurface.shadertime;
	if (shadertime >= 32768.0f)
		shadertime -= floor(rsurface.shadertime * (1.0f / 32768.0f)) * 32768.0f;
	switch(tcmod->tcmod)
	{
		case Q3TCMOD_COUNT:
		case Q3TCMOD_NONE:
			if (currentmaterialflags & MATERIALFLAG_WATERSCROLL)
				matrix = r_waterscrollmatrix;
			else
				matrix = identitymatrix;
			break;
		case Q3TCMOD_ENTITYTRANSLATE:
			// this is used in Q3 to allow the gamecode to control texcoord
			// scrolling on the entity, which is not supported in darkplaces yet.
			Matrix4x4_CreateTranslate(&matrix, 0, 0, 0);
			break;
		case Q3TCMOD_ROTATE:
			Matrix4x4_CreateTranslate(&matrix, 0.5, 0.5, 0);
			Matrix4x4_ConcatRotate(&matrix, tcmod->parms[0] * rsurface.shadertime, 0, 0, 1);
			Matrix4x4_ConcatTranslate(&matrix, -0.5, -0.5, 0);
			break;
		case Q3TCMOD_SCALE:
			Matrix4x4_CreateScale3(&matrix, tcmod->parms[0], tcmod->parms[1], 1);
			break;
		case Q3TCMOD_SCROLL:
			// this particular tcmod is a "bug for bug" compatible one with regards to
			// Quake3, the wrapping is unnecessary with our shadetime fix but quake3
			// specifically did the wrapping and so we must mimic that...
			offsetd[0] = tcmod->parms[0] * rsurface.shadertime;
			offsetd[1] = tcmod->parms[1] * rsurface.shadertime;
			Matrix4x4_CreateTranslate(&matrix, offsetd[0] - floor(offsetd[0]), offsetd[1] - floor(offsetd[1]), 0);
			break;
		case Q3TCMOD_PAGE: // poor man's animmap (to store animations into a single file, useful for HTTP downloaded textures)
			w = (int) tcmod->parms[0];
			h = (int) tcmod->parms[1];
			f = rsurface.shadertime / (tcmod->parms[2] * w * h);
			f = f - floor(f);
			idx = (int) floor(f * w * h);
			Matrix4x4_CreateTranslate(&matrix, (idx % w) / tcmod->parms[0], (idx / w) / tcmod->parms[1], 0);
			break;
		case Q3TCMOD_STRETCH:
			f = 1.0f / R_EvaluateQ3WaveFunc(tcmod->wavefunc, tcmod->waveparms);
			Matrix4x4_CreateFromQuakeEntity(&matrix, 0.5f * (1 - f), 0.5 * (1 - f), 0, 0, 0, 0, f);
			break;
		case Q3TCMOD_TRANSFORM:
			VectorSet(tcmat +  0, tcmod->parms[0], tcmod->parms[1], 0);
			VectorSet(tcmat +  3, tcmod->parms[2], tcmod->parms[3], 0);
			VectorSet(tcmat +  6, 0                   , 0                , 1);
			VectorSet(tcmat +  9, tcmod->parms[4], tcmod->parms[5], 0);
			Matrix4x4_FromArray12FloatGL(&matrix, tcmat);
			break;
		case Q3TCMOD_TURBULENT:
			// this is handled in the RSurf_PrepareVertices function
			matrix = identitymatrix;
			break;
	}
	temp = *texmatrix;
	Matrix4x4_Concat(texmatrix, &matrix, &temp);
}

static void R_LoadQWSkin(r_qwskincache_t *cache, const char *skinname)
{
	int textureflags = (r_mipskins.integer ? TEXF_MIPMAP : 0) | TEXF_PICMIP;
	char name[MAX_QPATH];
	skinframe_t *skinframe;
	unsigned char pixels[296*194];
	dp_strlcpy(cache->name, skinname, sizeof(cache->name));
	dpsnprintf(name, sizeof(name), "skins/%s.pcx", cache->name);
	if (developer_loading.integer)
		Con_Printf("loading %s\n", name);
	skinframe = R_SkinFrame_Find(name, textureflags, 0, 0, 0, false);
	if (!skinframe || !skinframe->base)
	{
		unsigned char *f;
		fs_offset_t filesize;
		skinframe = NULL;
		f = FS_LoadFile(name, tempmempool, true, &filesize);
		if (f)
		{
			if (LoadPCX_QWSkin(f, (int)filesize, pixels, 296, 194))
				skinframe = R_SkinFrame_LoadInternalQuake(name, textureflags, true, r_fullbrights.integer, pixels, image_width, image_height);
			Mem_Free(f);
		}
	}
	cache->skinframe = skinframe;
}

texture_t *R_GetCurrentTexture(texture_t *t)
{
	int i, q;
	const entity_render_t *ent = rsurface.entity;
	model_t *model = ent->model; // when calling this, ent must not be NULL
	q3shaderinfo_layer_tcmod_t *tcmod;
	float specularscale = 0.0f;

	if (t->update_lastrenderframe == r_textureframe && t->update_lastrenderentity == (void *)ent && !rsurface.forcecurrenttextureupdate)
		return t->currentframe;
	t->update_lastrenderframe = r_textureframe;
	t->update_lastrenderentity = (void *)ent;

	if(ent->entitynumber >= MAX_EDICTS && ent->entitynumber < 2 * MAX_EDICTS)
		t->camera_entity = ent->entitynumber;
	else
		t->camera_entity = 0;

	// switch to an alternate material if this is a q1bsp animated material
	{
		texture_t *texture = t;
		int s = rsurface.ent_skinnum;
		if ((unsigned int)s >= (unsigned int)model->numskins)
			s = 0;
		if (model->skinscenes)
		{
			if (model->skinscenes[s].framecount > 1)
				s = model->skinscenes[s].firstframe + (unsigned int) (rsurface.shadertime * model->skinscenes[s].framerate) % model->skinscenes[s].framecount;
			else
				s = model->skinscenes[s].firstframe;
		}
		if (s > 0)
			t = t + s * model->num_surfaces;
		if (t->animated)
		{
			// use an alternate animation if the entity's frame is not 0,
			// and only if the texture has an alternate animation
			if (t->animated == 2) // q2bsp
				t = t->anim_frames[0][ent->framegroupblend[0].frame % t->anim_total[0]];
			else if (rsurface.ent_alttextures && t->anim_total[1])
				t = t->anim_frames[1][(t->anim_total[1] >= 2) ? ((int)(rsurface.shadertime * 5.0f) % t->anim_total[1]) : 0];
			else
				t = t->anim_frames[0][(t->anim_total[0] >= 2) ? ((int)(rsurface.shadertime * 5.0f) % t->anim_total[0]) : 0];
		}
		texture->currentframe = t;
	}

	// update currentskinframe to be a qw skin or animation frame
	if (rsurface.ent_qwskin >= 0)
	{
		i = rsurface.ent_qwskin;
		if (!r_qwskincache || r_qwskincache_size != cl.maxclients)
		{
			r_qwskincache_size = cl.maxclients;
			if (r_qwskincache)
				Mem_Free(r_qwskincache);
			r_qwskincache = (r_qwskincache_t *)Mem_Alloc(r_main_mempool, sizeof(*r_qwskincache) * r_qwskincache_size);
		}
		if (strcmp(r_qwskincache[i].name, cl.scores[i].qw_skin))
			R_LoadQWSkin(&r_qwskincache[i], cl.scores[i].qw_skin);
		t->currentskinframe = r_qwskincache[i].skinframe;
		if (t->materialshaderpass && t->currentskinframe == NULL)
			t->currentskinframe = t->materialshaderpass->skinframes[LoopingFrameNumberFromDouble(rsurface.shadertime * t->materialshaderpass->framerate, t->materialshaderpass->numframes)];
	}
	else if (t->materialshaderpass && t->materialshaderpass->numframes >= 2)
		t->currentskinframe = t->materialshaderpass->skinframes[LoopingFrameNumberFromDouble(rsurface.shadertime * t->materialshaderpass->framerate, t->materialshaderpass->numframes)];
	if (t->backgroundshaderpass && t->backgroundshaderpass->numframes >= 2)
		t->backgroundcurrentskinframe = t->backgroundshaderpass->skinframes[LoopingFrameNumberFromDouble(rsurface.shadertime * t->backgroundshaderpass->framerate, t->backgroundshaderpass->numframes)];

	t->currentmaterialflags = t->basematerialflags;
	t->currentalpha = rsurface.entity->alpha * t->basealpha;
	if (t->basematerialflags & MATERIALFLAG_WATERALPHA && (model->brush.supportwateralpha || r_wateralpha_force.integer || r_water.integer || r_novis.integer || r_trippy.integer))
		t->currentalpha *= r_wateralpha.value;
	if(t->basematerialflags & MATERIALFLAG_WATERSHADER && r_fb.water.enabled && !r_refdef.view.isoverlay)
		t->currentmaterialflags |= MATERIALFLAG_ALPHA | MATERIALFLAG_BLENDED | MATERIALFLAG_NOSHADOW; // we apply wateralpha later
	if(!r_fb.water.enabled || r_refdef.view.isoverlay)
		t->currentmaterialflags &= ~(MATERIALFLAG_WATERSHADER | MATERIALFLAG_REFRACTION | MATERIALFLAG_REFLECTION | MATERIALFLAG_CAMERA);

	// decide on which type of lighting to use for this surface
	if (rsurface.entity->render_modellight_forced)
		t->currentmaterialflags |= MATERIALFLAG_MODELLIGHT;
	if (rsurface.entity->render_rtlight_disabled)
		t->currentmaterialflags |= MATERIALFLAG_NORTLIGHT;
	if (rsurface.entity->render_lightgrid)
		t->currentmaterialflags |= MATERIALFLAG_LIGHTGRID;
	if (t->currentmaterialflags & MATERIALFLAG_CUSTOMBLEND && !(R_BlendFuncFlags(t->customblendfunc[0], t->customblendfunc[1]) & BLENDFUNC_ALLOWS_COLORMOD))
	{
		// some CUSTOMBLEND blendfuncs are too weird, we have to ignore colormod and view colorscale
		t->currentmaterialflags = (t->currentmaterialflags | MATERIALFLAG_MODELLIGHT | MATERIALFLAG_NORTLIGHT) & ~MATERIALFLAG_LIGHTGRID;
		for (q = 0; q < 3; q++)
		{
			t->render_glowmod[q] = rsurface.entity->glowmod[q];
			t->render_modellight_lightdir_world[q] = q == 2;
			t->render_modellight_lightdir_local[q] = q == 2;
			t->render_modellight_ambient[q] = 1;
			t->render_modellight_diffuse[q] = 0;
			t->render_modellight_specular[q] = 0;
			t->render_lightmap_ambient[q] = 0;
			t->render_lightmap_diffuse[q] = 0;
			t->render_lightmap_specular[q] = 0;
			t->render_rtlight_diffuse[q] = 0;
			t->render_rtlight_specular[q] = 0;
		}
	}
	else if ((t->currentmaterialflags & MATERIALFLAG_FULLBRIGHT) || !(rsurface.ent_flags & RENDER_LIGHT))
	{
		// fullbright is basically MATERIALFLAG_MODELLIGHT but with ambient locked to 1,1,1 and no shading
		t->currentmaterialflags = (t->currentmaterialflags | MATERIALFLAG_NORTLIGHT | MATERIALFLAG_MODELLIGHT) & ~MATERIALFLAG_LIGHTGRID;
		for (q = 0; q < 3; q++)
		{
			t->render_glowmod[q] = rsurface.entity->render_glowmod[q] * r_refdef.view.colorscale;
			t->render_modellight_ambient[q] = rsurface.entity->render_fullbright[q] * r_refdef.view.colorscale;
			t->render_modellight_lightdir_world[q] = q == 2;
			t->render_modellight_lightdir_local[q] = q == 2;
			t->render_modellight_diffuse[q] = 0;
			t->render_modellight_specular[q] = 0;
			t->render_lightmap_ambient[q] = 0;
			t->render_lightmap_diffuse[q] = 0;
			t->render_lightmap_specular[q] = 0;
			t->render_rtlight_diffuse[q] = 0;
			t->render_rtlight_specular[q] = 0;
		}
	}
	else if (t->currentmaterialflags & MATERIALFLAG_LIGHTGRID)
	{
		t->currentmaterialflags &= ~MATERIALFLAG_MODELLIGHT;
		for (q = 0; q < 3; q++)
		{
			t->render_glowmod[q] = rsurface.entity->render_glowmod[q] * r_refdef.view.colorscale;
			t->render_modellight_lightdir_world[q] = q == 2;
			t->render_modellight_lightdir_local[q] = q == 2;
			t->render_modellight_ambient[q] = 0;
			t->render_modellight_diffuse[q] = 0;
			t->render_modellight_specular[q] = 0;
			t->render_lightmap_ambient[q] = rsurface.entity->render_lightmap_ambient[q] * r_refdef.view.colorscale;
			t->render_lightmap_diffuse[q] = rsurface.entity->render_lightmap_diffuse[q] * 2 * r_refdef.view.colorscale;
			t->render_lightmap_specular[q] = rsurface.entity->render_lightmap_specular[q] * 2 * r_refdef.view.colorscale;
			t->render_rtlight_diffuse[q] = rsurface.entity->render_rtlight_diffuse[q] * r_refdef.view.colorscale;
			t->render_rtlight_specular[q] = rsurface.entity->render_rtlight_specular[q] * r_refdef.view.colorscale;
		}
	}
	else if ((rsurface.ent_flags & (RENDER_DYNAMICMODELLIGHT | RENDER_CUSTOMIZEDMODELLIGHT)) || rsurface.modeltexcoordlightmap2f == NULL)
	{
		// ambient + single direction light (modellight)
		t->currentmaterialflags = (t->currentmaterialflags | MATERIALFLAG_MODELLIGHT) & ~MATERIALFLAG_LIGHTGRID;
		for (q = 0; q < 3; q++)
		{
			t->render_glowmod[q] = rsurface.entity->render_glowmod[q] * r_refdef.view.colorscale;
			t->render_modellight_lightdir_world[q] = rsurface.entity->render_modellight_lightdir_world[q];
			t->render_modellight_lightdir_local[q] = rsurface.entity->render_modellight_lightdir_local[q];
			t->render_modellight_ambient[q] = rsurface.entity->render_modellight_ambient[q] * r_refdef.view.colorscale;
			t->render_modellight_diffuse[q] = rsurface.entity->render_modellight_diffuse[q] * r_refdef.view.colorscale;
			t->render_modellight_specular[q] = rsurface.entity->render_modellight_specular[q] * r_refdef.view.colorscale;
			t->render_lightmap_ambient[q] = 0;
			t->render_lightmap_diffuse[q] = 0;
			t->render_lightmap_specular[q] = 0;
			t->render_rtlight_diffuse[q] = rsurface.entity->render_rtlight_diffuse[q] * r_refdef.view.colorscale;
			t->render_rtlight_specular[q] = rsurface.entity->render_rtlight_specular[q] * r_refdef.view.colorscale;
		}
	}
	else
	{
		// lightmap - 2x diffuse and specular brightness because bsp files have 0-2 colors as 0-1
		for (q = 0; q < 3; q++)
		{
			t->render_glowmod[q] = rsurface.entity->render_glowmod[q] * r_refdef.view.colorscale;
			t->render_modellight_lightdir_world[q] = q == 2;
			t->render_modellight_lightdir_local[q] = q == 2;
			t->render_modellight_ambient[q] = 0;
			t->render_modellight_diffuse[q] = 0;
			t->render_modellight_specular[q] = 0;
			t->render_lightmap_ambient[q] = rsurface.entity->render_lightmap_ambient[q] * r_refdef.view.colorscale;
			t->render_lightmap_diffuse[q] = rsurface.entity->render_lightmap_diffuse[q] * 2 * r_refdef.view.colorscale;
			t->render_lightmap_specular[q] = rsurface.entity->render_lightmap_specular[q] * 2 * r_refdef.view.colorscale;
			t->render_rtlight_diffuse[q] = rsurface.entity->render_rtlight_diffuse[q] * r_refdef.view.colorscale;
			t->render_rtlight_specular[q] = rsurface.entity->render_rtlight_specular[q] * r_refdef.view.colorscale;
		}
	}

	if (t->currentmaterialflags & MATERIALFLAG_VERTEXCOLOR)
	{
		// since MATERIALFLAG_VERTEXCOLOR uses the lightmapcolor4f vertex
		// attribute, we punt it to the lightmap path and hope for the best,
		// but lighting doesn't work.
		//
		// FIXME: this is fine for effects but CSQC polygons should be subject
		// to lighting.
		t->currentmaterialflags &= ~(MATERIALFLAG_MODELLIGHT | MATERIALFLAG_LIGHTGRID);
		for (q = 0; q < 3; q++)
		{
			t->render_glowmod[q] = rsurface.entity->render_glowmod[q] * r_refdef.view.colorscale;
			t->render_modellight_lightdir_world[q] = q == 2;
			t->render_modellight_lightdir_local[q] = q == 2;
			t->render_modellight_ambient[q] = 0;
			t->render_modellight_diffuse[q] = 0;
			t->render_modellight_specular[q] = 0;
			t->render_lightmap_ambient[q] = 0;
			t->render_lightmap_diffuse[q] = rsurface.entity->render_fullbright[q] * r_refdef.view.colorscale;
			t->render_lightmap_specular[q] = 0;
			t->render_rtlight_diffuse[q] = 0;
			t->render_rtlight_specular[q] = 0;
		}
	}

	// LAVA GLOW (r_lavaglow): scale lava's emission -- the whole visible lava
	// image IS its glow layer (the base texture is black). 1.0 is IEEE-exact
	// the classic brightness.
	if ((t->supercontents & SUPERCONTENTS_LAVA) && r_lavaglow.value != 1.0f)
		for (q = 0; q < 3; q++)
			t->render_glowmod[q] *= r_lavaglow.value;

	for (q = 0; q < 3; q++)
	{
		t->render_colormap_pants[q] = rsurface.entity->colormap_pantscolor[q];
		t->render_colormap_shirt[q] = rsurface.entity->colormap_shirtcolor[q];
	}

	if (rsurface.ent_flags & RENDER_ADDITIVE)
		t->currentmaterialflags |= MATERIALFLAG_ADD | MATERIALFLAG_BLENDED | MATERIALFLAG_NOSHADOW;
	else if (t->currentalpha < 1)
		t->currentmaterialflags |= MATERIALFLAG_ALPHA | MATERIALFLAG_BLENDED | MATERIALFLAG_NOSHADOW;
	// LadyHavoc: prevent bugs where code checks add or alpha at higher priority than customblend by clearing these flags
	if (t->currentmaterialflags & MATERIALFLAG_CUSTOMBLEND)
		t->currentmaterialflags &= ~(MATERIALFLAG_ADD | MATERIALFLAG_ALPHA);
	if (rsurface.ent_flags & RENDER_DOUBLESIDED)
		t->currentmaterialflags |= MATERIALFLAG_NOSHADOW | MATERIALFLAG_NOCULLFACE;
	if (rsurface.ent_flags & (RENDER_NODEPTHTEST | RENDER_VIEWMODEL))
		t->currentmaterialflags |= MATERIALFLAG_SHORTDEPTHRANGE;
	if (t->backgroundshaderpass)
		t->currentmaterialflags |= MATERIALFLAG_VERTEXTEXTUREBLEND;
	if (t->currentmaterialflags & MATERIALFLAG_BLENDED)
	{
		if (t->currentmaterialflags & (MATERIALFLAG_REFRACTION | MATERIALFLAG_WATERSHADER | MATERIALFLAG_CAMERA))
			t->currentmaterialflags &= ~MATERIALFLAG_BLENDED;
	}
	else
		t->currentmaterialflags &= ~(MATERIALFLAG_REFRACTION | MATERIALFLAG_WATERSHADER | MATERIALFLAG_CAMERA);
	// WATERSURFACE: a blended liquid is being classified for this view, so the
	// frame copy it refracts is worth taking before the transparent pass.
	if (r_watersurface.integer && !r_refdef.view.isoverlay && !(t->currentmaterialflags & MATERIALFLAG_ADD)
	 && (t->currentmaterialflags & (MATERIALFLAG_WATERALPHA | MATERIALFLAG_BLENDED)) == (MATERIALFLAG_WATERALPHA | MATERIALFLAG_BLENDED))
		r_fb.waterseen = true;
	if (vid.allowalphatocoverage && r_transparent_alphatocoverage.integer >= 2 && ((t->currentmaterialflags & (MATERIALFLAG_BLENDED | MATERIALFLAG_ALPHA | MATERIALFLAG_ADD | MATERIALFLAG_CUSTOMBLEND)) == (MATERIALFLAG_BLENDED | MATERIALFLAG_ALPHA)))
	{
		// promote alphablend to alphatocoverage (a type of alphatest) if antialiasing is on
		t->currentmaterialflags = (t->currentmaterialflags & ~(MATERIALFLAG_BLENDED | MATERIALFLAG_ALPHA)) | MATERIALFLAG_ALPHATEST;
	}
	if ((t->currentmaterialflags & (MATERIALFLAG_BLENDED | MATERIALFLAG_NODEPTHTEST)) == MATERIALFLAG_BLENDED && r_transparentdepthmasking.integer && !(t->basematerialflags & MATERIALFLAG_BLENDED))
		t->currentmaterialflags |= MATERIALFLAG_TRANSDEPTH;

	// there is no tcmod
	if (t->currentmaterialflags & MATERIALFLAG_WATERSCROLL)
	{
		t->currenttexmatrix = r_waterscrollmatrix;
		t->currentbackgroundtexmatrix = r_waterscrollmatrix;
	}
	else if (!(t->currentmaterialflags & MATERIALFLAG_CUSTOMSURFACE))
	{
		Matrix4x4_CreateIdentity(&t->currenttexmatrix);
		Matrix4x4_CreateIdentity(&t->currentbackgroundtexmatrix);
	}

	if (t->materialshaderpass)
		for (i = 0, tcmod = t->materialshaderpass->tcmods;i < Q3MAXTCMODS && tcmod->tcmod;i++, tcmod++)
			R_tcMod_ApplyToMatrix(&t->currenttexmatrix, tcmod, t->currentmaterialflags);

	t->colormapping = VectorLength2(t->render_colormap_pants) + VectorLength2(t->render_colormap_shirt) >= (1.0f / 1048576.0f);
	if (t->currentskinframe->qpixels)
		R_SkinFrame_GenerateTexturesFromQPixels(t->currentskinframe, t->colormapping);
	t->basetexture = (!t->colormapping && t->currentskinframe->merged) ? t->currentskinframe->merged : t->currentskinframe->base;
	if (!t->basetexture)
		t->basetexture = r_texture_notexture;
	t->pantstexture = t->colormapping ? t->currentskinframe->pants : NULL;
	t->shirttexture = t->colormapping ? t->currentskinframe->shirt : NULL;
	t->nmaptexture = t->currentskinframe->nmap;
	if (!t->nmaptexture)
		t->nmaptexture = r_texture_blanknormalmap;
	t->glosstexture = r_texture_black;
	t->glowtexture = t->currentskinframe->glow;
	t->fogtexture = t->currentskinframe->fog;
	t->reflectmasktexture = t->currentskinframe->reflect;
	if (t->backgroundshaderpass)
	{
		for (i = 0, tcmod = t->backgroundshaderpass->tcmods; i < Q3MAXTCMODS && tcmod->tcmod; i++, tcmod++)
			R_tcMod_ApplyToMatrix(&t->currentbackgroundtexmatrix, tcmod, t->currentmaterialflags);
		t->backgroundbasetexture = (!t->colormapping && t->backgroundcurrentskinframe->merged) ? t->backgroundcurrentskinframe->merged : t->backgroundcurrentskinframe->base;
		t->backgroundnmaptexture = t->backgroundcurrentskinframe->nmap;
		t->backgroundglosstexture = r_texture_black;
		t->backgroundglowtexture = t->backgroundcurrentskinframe->glow;
		if (!t->backgroundnmaptexture)
			t->backgroundnmaptexture = r_texture_blanknormalmap;
		// make sure that if glow is going to be used, both textures are not NULL
		if (!t->backgroundglowtexture && t->glowtexture)
			t->backgroundglowtexture = r_texture_black;
		if (!t->glowtexture && t->backgroundglowtexture)
			t->glowtexture = r_texture_black;
	}
	else
	{
		t->backgroundbasetexture = r_texture_white;
		t->backgroundnmaptexture = r_texture_blanknormalmap;
		t->backgroundglosstexture = r_texture_black;
		t->backgroundglowtexture = NULL;
	}
	t->specularpower = r_shadow_glossexponent.value;
	// TODO: store reference values for these in the texture?
	if (r_shadow_gloss.integer > 0)
	{
		if (t->currentskinframe->gloss || (t->backgroundcurrentskinframe && t->backgroundcurrentskinframe->gloss))
		{
			if (r_shadow_glossintensity.value > 0)
			{
				t->glosstexture = t->currentskinframe->gloss ? t->currentskinframe->gloss : r_texture_white;
				t->backgroundglosstexture = (t->backgroundcurrentskinframe && t->backgroundcurrentskinframe->gloss) ? t->backgroundcurrentskinframe->gloss : r_texture_white;
				specularscale = r_shadow_glossintensity.value;
			}
		}
		else if (r_shadow_gloss.integer >= 2 && r_shadow_gloss2intensity.value > 0)
		{
			t->glosstexture = r_texture_white;
			t->backgroundglosstexture = r_texture_white;
			specularscale = r_shadow_gloss2intensity.value;
			t->specularpower = r_shadow_gloss2exponent.value;
		}
	}
	specularscale *= t->specularscalemod;
	t->specularpower *= t->specularpowermod;

	// lightmaps mode looks bad with dlights using actual texturing, so turn
	// off the colormap and glossmap, but leave the normalmap on as it still
	// accurately represents the shading involved
	if (gl_lightmaps.integer && ent != &cl_meshentities[MESH_UI].render)
	{
		t->basetexture = r_texture_grey128;
		t->pantstexture = r_texture_black;
		t->shirttexture = r_texture_black;
		if (gl_lightmaps.integer < 2)
			t->nmaptexture = r_texture_blanknormalmap;
		t->glosstexture = r_texture_black;
		t->glowtexture = NULL;
		t->fogtexture = NULL;
		t->reflectmasktexture = NULL;
		t->backgroundbasetexture = NULL;
		if (gl_lightmaps.integer < 2)
			t->backgroundnmaptexture = r_texture_blanknormalmap;
		t->backgroundglosstexture = r_texture_black;
		t->backgroundglowtexture = NULL;
		specularscale = 0;
		t->currentmaterialflags = MATERIALFLAG_WALL | (t->currentmaterialflags & (MATERIALFLAG_NOCULLFACE | MATERIALFLAG_MODELLIGHT | MATERIALFLAG_NODEPTHTEST | MATERIALFLAG_SHORTDEPTHRANGE));
	}

	if (specularscale != 1.0f)
	{
		for (q = 0; q < 3; q++)
		{
			t->render_modellight_specular[q] *= specularscale;
			t->render_lightmap_specular[q] *= specularscale;
			t->render_rtlight_specular[q] *= specularscale;
		}
	}

	t->currentblendfunc[0] = GL_ONE;
	t->currentblendfunc[1] = GL_ZERO;
	if (t->currentmaterialflags & MATERIALFLAG_ADD)
	{
		t->currentblendfunc[0] = GL_SRC_ALPHA;
		t->currentblendfunc[1] = GL_ONE;
	}
	else if (t->currentmaterialflags & MATERIALFLAG_ALPHA)
	{
		t->currentblendfunc[0] = GL_SRC_ALPHA;
		t->currentblendfunc[1] = GL_ONE_MINUS_SRC_ALPHA;
	}
	else if (t->currentmaterialflags & MATERIALFLAG_CUSTOMBLEND)
	{
		t->currentblendfunc[0] = t->customblendfunc[0];
		t->currentblendfunc[1] = t->customblendfunc[1];
	}

	return t;
}

rsurfacestate_t rsurface;

void RSurf_ActiveModelEntity(const entity_render_t *ent, qbool wantnormals, qbool wanttangents, qbool prepass)
{
	model_t *model = ent->model;
	//if (rsurface.entity == ent && (!model->surfmesh.isanimated || (!wantnormals && !wanttangents)))
	//	return;
	rsurface.entity = (entity_render_t *)ent;
	rsurface.skeleton = ent->skeleton;
	memcpy(rsurface.userwavefunc_param, ent->userwavefunc_param, sizeof(rsurface.userwavefunc_param));
	rsurface.ent_skinnum = ent->skinnum;
	rsurface.ent_qwskin = (ent->entitynumber <= cl.maxclients && ent->entitynumber >= 1 && cls.protocol == PROTOCOL_QUAKEWORLD && cl.scores[ent->entitynumber - 1].qw_skin[0] && !strcmp(ent->model->name, "progs/player.mdl")) ? (ent->entitynumber - 1) : -1;
	rsurface.ent_flags = ent->flags;
	if (r_fullbright_directed.integer && (r_fullbright.integer || !model->lit))
		rsurface.ent_flags |= RENDER_LIGHT | RENDER_DYNAMICMODELLIGHT;
#ifdef USE_RT_METAL
	// The view weapon carries its own RT-matched model light (rt_metal_viewmodel,
	// filled in CL_UpdateEntityShading_Entity), so it must reach the modellight
	// branch of R_GetCurrentTexture rather than the fullbright one that wall
	// lighting would otherwise force it into. LOCAL flags only, exactly as the
	// r_fullbright_directed line above does: writing ent->flags would enrol the gun
	// in the realtime dlight pass (r_shadow.c gates that on ent->flags) and
	// double-light it for anyone running r_shadow_realtime_dlight 1.
	if ((ent->flags & RENDER_VIEWMODEL) && ent->render_modellight_forced && !ent->render_lightgrid
	 && (VectorLength2(ent->render_modellight_ambient) > 0 || VectorLength2(ent->render_modellight_diffuse) > 0)
	 && rt_metal_viewmodel.value > 0.0f && rt_metal_walllight.value > 0.0f && rt_metal.integer)
		rsurface.ent_flags |= RENDER_LIGHT | RENDER_DYNAMICMODELLIGHT;
#endif
	rsurface.shadertime = r_refdef.scene.time - ent->shadertime;
	rsurface.matrix = ent->matrix;
	rsurface.inversematrix = ent->inversematrix;
	rsurface.matrixscale = Matrix4x4_ScaleFromMatrix(&rsurface.matrix);
	rsurface.inversematrixscale = 1.0f / rsurface.matrixscale;
	R_EntityMatrix(&rsurface.matrix);
	Matrix4x4_Transform(&rsurface.inversematrix, r_refdef.view.origin, rsurface.localvieworigin);
	Matrix4x4_TransformStandardPlane(&rsurface.inversematrix, r_refdef.fogplane[0], r_refdef.fogplane[1], r_refdef.fogplane[2], r_refdef.fogplane[3], rsurface.fogplane);
	rsurface.fogplaneviewdist = r_refdef.fogplaneviewdist * rsurface.inversematrixscale;
	rsurface.fograngerecip = r_refdef.fograngerecip * rsurface.matrixscale;
	rsurface.fogheightfade = r_refdef.fogheightfade * rsurface.matrixscale;
	rsurface.fogmasktabledistmultiplier = FOGMASKTABLEWIDTH * rsurface.fograngerecip;
	memcpy(rsurface.frameblend, ent->frameblend, sizeof(ent->frameblend));
	rsurface.ent_alttextures = ent->framegroupblend[0].frame != 0;
	rsurface.basepolygonfactor = r_refdef.polygonfactor;
	rsurface.basepolygonoffset = r_refdef.polygonoffset;
	if (ent->model->brush.submodel && !prepass)
	{
		rsurface.basepolygonfactor += r_polygonoffset_submodel_factor.value;
		rsurface.basepolygonoffset += r_polygonoffset_submodel_offset.value;
	}
	// if the animcache code decided it should use the shader path, skip the deform step
	rsurface.entityskeletaltransform3x4 = ent->animcache_skeletaltransform3x4;
	rsurface.entityskeletaltransform3x4buffer = ent->animcache_skeletaltransform3x4buffer;
	rsurface.entityskeletaltransform3x4offset = ent->animcache_skeletaltransform3x4offset;
	rsurface.entityskeletaltransform3x4size = ent->animcache_skeletaltransform3x4size;
	rsurface.entityskeletalnumtransforms = rsurface.entityskeletaltransform3x4 ? model->num_bones : 0;
	if (model->surfmesh.isanimated && model->AnimateVertices && !rsurface.entityskeletaltransform3x4)
	{
		if (ent->animcache_vertex3f)
		{
			r_refdef.stats[r_stat_batch_entitycache_count]++;
			r_refdef.stats[r_stat_batch_entitycache_surfaces] += model->num_surfaces;
			r_refdef.stats[r_stat_batch_entitycache_vertices] += model->surfmesh.num_vertices;
			r_refdef.stats[r_stat_batch_entitycache_triangles] += model->surfmesh.num_triangles;
			rsurface.modelvertex3f = ent->animcache_vertex3f;
			rsurface.modelvertex3f_vertexbuffer = ent->animcache_vertex3f_vertexbuffer;
			rsurface.modelvertex3f_bufferoffset = ent->animcache_vertex3f_bufferoffset;
			rsurface.modelsvector3f = wanttangents ? ent->animcache_svector3f : NULL;
			rsurface.modelsvector3f_vertexbuffer = wanttangents ? ent->animcache_svector3f_vertexbuffer : NULL;
			rsurface.modelsvector3f_bufferoffset = wanttangents ? ent->animcache_svector3f_bufferoffset : 0;
			rsurface.modeltvector3f = wanttangents ? ent->animcache_tvector3f : NULL;
			rsurface.modeltvector3f_vertexbuffer = wanttangents ? ent->animcache_tvector3f_vertexbuffer : NULL;
			rsurface.modeltvector3f_bufferoffset = wanttangents ? ent->animcache_tvector3f_bufferoffset : 0;
			rsurface.modelnormal3f = wantnormals ? ent->animcache_normal3f : NULL;
			rsurface.modelnormal3f_vertexbuffer = wantnormals ? ent->animcache_normal3f_vertexbuffer : NULL;
			rsurface.modelnormal3f_bufferoffset = wantnormals ? ent->animcache_normal3f_bufferoffset : 0;
		}
		else if (wanttangents)
		{
			r_refdef.stats[r_stat_batch_entityanimate_count]++;
			r_refdef.stats[r_stat_batch_entityanimate_surfaces] += model->num_surfaces;
			r_refdef.stats[r_stat_batch_entityanimate_vertices] += model->surfmesh.num_vertices;
			r_refdef.stats[r_stat_batch_entityanimate_triangles] += model->surfmesh.num_triangles;
			rsurface.modelvertex3f = (float *)R_FrameData_Alloc(model->surfmesh.num_vertices * sizeof(float[3]));
			rsurface.modelsvector3f = (float *)R_FrameData_Alloc(model->surfmesh.num_vertices * sizeof(float[3]));
			rsurface.modeltvector3f = (float *)R_FrameData_Alloc(model->surfmesh.num_vertices * sizeof(float[3]));
			rsurface.modelnormal3f = (float *)R_FrameData_Alloc(model->surfmesh.num_vertices * sizeof(float[3]));
			model->AnimateVertices(model, rsurface.frameblend, rsurface.skeleton, rsurface.modelvertex3f, rsurface.modelnormal3f, rsurface.modelsvector3f, rsurface.modeltvector3f);
			rsurface.modelvertex3f_vertexbuffer = NULL;
			rsurface.modelvertex3f_bufferoffset = 0;
			rsurface.modelvertex3f_vertexbuffer = 0;
			rsurface.modelvertex3f_bufferoffset = 0;
			rsurface.modelsvector3f_vertexbuffer = 0;
			rsurface.modelsvector3f_bufferoffset = 0;
			rsurface.modeltvector3f_vertexbuffer = 0;
			rsurface.modeltvector3f_bufferoffset = 0;
			rsurface.modelnormal3f_vertexbuffer = 0;
			rsurface.modelnormal3f_bufferoffset = 0;
		}
		else if (wantnormals)
		{
			r_refdef.stats[r_stat_batch_entityanimate_count]++;
			r_refdef.stats[r_stat_batch_entityanimate_surfaces] += model->num_surfaces;
			r_refdef.stats[r_stat_batch_entityanimate_vertices] += model->surfmesh.num_vertices;
			r_refdef.stats[r_stat_batch_entityanimate_triangles] += model->surfmesh.num_triangles;
			rsurface.modelvertex3f = (float *)R_FrameData_Alloc(model->surfmesh.num_vertices * sizeof(float[3]));
			rsurface.modelsvector3f = NULL;
			rsurface.modeltvector3f = NULL;
			rsurface.modelnormal3f = (float *)R_FrameData_Alloc(model->surfmesh.num_vertices * sizeof(float[3]));
			model->AnimateVertices(model, rsurface.frameblend, rsurface.skeleton, rsurface.modelvertex3f, rsurface.modelnormal3f, NULL, NULL);
			rsurface.modelvertex3f_vertexbuffer = NULL;
			rsurface.modelvertex3f_bufferoffset = 0;
			rsurface.modelvertex3f_vertexbuffer = 0;
			rsurface.modelvertex3f_bufferoffset = 0;
			rsurface.modelsvector3f_vertexbuffer = 0;
			rsurface.modelsvector3f_bufferoffset = 0;
			rsurface.modeltvector3f_vertexbuffer = 0;
			rsurface.modeltvector3f_bufferoffset = 0;
			rsurface.modelnormal3f_vertexbuffer = 0;
			rsurface.modelnormal3f_bufferoffset = 0;
		}
		else
		{
			r_refdef.stats[r_stat_batch_entityanimate_count]++;
			r_refdef.stats[r_stat_batch_entityanimate_surfaces] += model->num_surfaces;
			r_refdef.stats[r_stat_batch_entityanimate_vertices] += model->surfmesh.num_vertices;
			r_refdef.stats[r_stat_batch_entityanimate_triangles] += model->surfmesh.num_triangles;
			rsurface.modelvertex3f = (float *)R_FrameData_Alloc(model->surfmesh.num_vertices * sizeof(float[3]));
			rsurface.modelsvector3f = NULL;
			rsurface.modeltvector3f = NULL;
			rsurface.modelnormal3f = NULL;
			model->AnimateVertices(model, rsurface.frameblend, rsurface.skeleton, rsurface.modelvertex3f, NULL, NULL, NULL);
			rsurface.modelvertex3f_vertexbuffer = NULL;
			rsurface.modelvertex3f_bufferoffset = 0;
			rsurface.modelvertex3f_vertexbuffer = 0;
			rsurface.modelvertex3f_bufferoffset = 0;
			rsurface.modelsvector3f_vertexbuffer = 0;
			rsurface.modelsvector3f_bufferoffset = 0;
			rsurface.modeltvector3f_vertexbuffer = 0;
			rsurface.modeltvector3f_bufferoffset = 0;
			rsurface.modelnormal3f_vertexbuffer = 0;
			rsurface.modelnormal3f_bufferoffset = 0;
		}
		rsurface.modelgeneratedvertex = true;
	}
	else
	{
		if (rsurface.entityskeletaltransform3x4)
		{
			r_refdef.stats[r_stat_batch_entityskeletal_count]++;
			r_refdef.stats[r_stat_batch_entityskeletal_surfaces] += model->num_surfaces;
			r_refdef.stats[r_stat_batch_entityskeletal_vertices] += model->surfmesh.num_vertices;
			r_refdef.stats[r_stat_batch_entityskeletal_triangles] += model->surfmesh.num_triangles;
		}
		else
		{
			r_refdef.stats[r_stat_batch_entitystatic_count]++;
			r_refdef.stats[r_stat_batch_entitystatic_surfaces] += model->num_surfaces;
			r_refdef.stats[r_stat_batch_entitystatic_vertices] += model->surfmesh.num_vertices;
			r_refdef.stats[r_stat_batch_entitystatic_triangles] += model->surfmesh.num_triangles;
		}
		rsurface.modelvertex3f  = model->surfmesh.data_vertex3f;
		rsurface.modelvertex3f_vertexbuffer = model->surfmesh.data_vertex3f_vertexbuffer;
		rsurface.modelvertex3f_bufferoffset = model->surfmesh.data_vertex3f_bufferoffset;
		rsurface.modelsvector3f = model->surfmesh.data_svector3f;
		rsurface.modelsvector3f_vertexbuffer = model->surfmesh.data_svector3f_vertexbuffer;
		rsurface.modelsvector3f_bufferoffset = model->surfmesh.data_svector3f_bufferoffset;
		rsurface.modeltvector3f = model->surfmesh.data_tvector3f;
		rsurface.modeltvector3f_vertexbuffer = model->surfmesh.data_tvector3f_vertexbuffer;
		rsurface.modeltvector3f_bufferoffset = model->surfmesh.data_tvector3f_bufferoffset;
		rsurface.modelnormal3f  = model->surfmesh.data_normal3f;
		rsurface.modelnormal3f_vertexbuffer = model->surfmesh.data_normal3f_vertexbuffer;
		rsurface.modelnormal3f_bufferoffset = model->surfmesh.data_normal3f_bufferoffset;
		rsurface.modelgeneratedvertex = false;
	}
	rsurface.modellightmapcolor4f  = model->surfmesh.data_lightmapcolor4f;
	rsurface.modellightmapcolor4f_vertexbuffer = model->surfmesh.data_lightmapcolor4f_vertexbuffer;
	rsurface.modellightmapcolor4f_bufferoffset = model->surfmesh.data_lightmapcolor4f_bufferoffset;
	rsurface.modeltexcoordtexture2f  = model->surfmesh.data_texcoordtexture2f;
	rsurface.modeltexcoordtexture2f_vertexbuffer = model->surfmesh.data_texcoordtexture2f_vertexbuffer;
	rsurface.modeltexcoordtexture2f_bufferoffset = model->surfmesh.data_texcoordtexture2f_bufferoffset;
	rsurface.modeltexcoordlightmap2f  = model->surfmesh.data_texcoordlightmap2f;
	rsurface.modeltexcoordlightmap2f_vertexbuffer = model->surfmesh.data_texcoordlightmap2f_vertexbuffer;
	rsurface.modeltexcoordlightmap2f_bufferoffset = model->surfmesh.data_texcoordlightmap2f_bufferoffset;
	rsurface.modelskeletalindex4ub = model->surfmesh.data_skeletalindex4ub;
	rsurface.modelskeletalindex4ub_vertexbuffer = model->surfmesh.data_skeletalindex4ub_vertexbuffer;
	rsurface.modelskeletalindex4ub_bufferoffset = model->surfmesh.data_skeletalindex4ub_bufferoffset;
	rsurface.modelskeletalweight4ub = model->surfmesh.data_skeletalweight4ub;
	rsurface.modelskeletalweight4ub_vertexbuffer = model->surfmesh.data_skeletalweight4ub_vertexbuffer;
	rsurface.modelskeletalweight4ub_bufferoffset = model->surfmesh.data_skeletalweight4ub_bufferoffset;
	rsurface.modelelement3i = model->surfmesh.data_element3i;
	rsurface.modelelement3i_indexbuffer = model->surfmesh.data_element3i_indexbuffer;
	rsurface.modelelement3i_bufferoffset = model->surfmesh.data_element3i_bufferoffset;
	rsurface.modelelement3s = model->surfmesh.data_element3s;
	rsurface.modelelement3s_indexbuffer = model->surfmesh.data_element3s_indexbuffer;
	rsurface.modelelement3s_bufferoffset = model->surfmesh.data_element3s_bufferoffset;
	rsurface.modellightmapoffsets = model->surfmesh.data_lightmapoffsets;
	rsurface.modelnumvertices = model->surfmesh.num_vertices;
	rsurface.modelnumtriangles = model->surfmesh.num_triangles;
	rsurface.modelsurfaces = model->data_surfaces;
	rsurface.batchgeneratedvertex = false;
	rsurface.batchfirstvertex = 0;
	rsurface.batchnumvertices = 0;
	rsurface.batchfirsttriangle = 0;
	rsurface.batchnumtriangles = 0;
	rsurface.batchvertex3f  = NULL;
	rsurface.batchvertex3f_vertexbuffer = NULL;
	rsurface.batchvertex3f_bufferoffset = 0;
	rsurface.batchsvector3f = NULL;
	rsurface.batchsvector3f_vertexbuffer = NULL;
	rsurface.batchsvector3f_bufferoffset = 0;
	rsurface.batchtvector3f = NULL;
	rsurface.batchtvector3f_vertexbuffer = NULL;
	rsurface.batchtvector3f_bufferoffset = 0;
	rsurface.batchnormal3f  = NULL;
	rsurface.batchnormal3f_vertexbuffer = NULL;
	rsurface.batchnormal3f_bufferoffset = 0;
	rsurface.batchlightmapcolor4f = NULL;
	rsurface.batchlightmapcolor4f_vertexbuffer = NULL;
	rsurface.batchlightmapcolor4f_bufferoffset = 0;
	rsurface.batchtexcoordtexture2f = NULL;
	rsurface.batchtexcoordtexture2f_vertexbuffer = NULL;
	rsurface.batchtexcoordtexture2f_bufferoffset = 0;
	rsurface.batchtexcoordlightmap2f = NULL;
	rsurface.batchtexcoordlightmap2f_vertexbuffer = NULL;
	rsurface.batchtexcoordlightmap2f_bufferoffset = 0;
	rsurface.batchskeletalindex4ub = NULL;
	rsurface.batchskeletalindex4ub_vertexbuffer = NULL;
	rsurface.batchskeletalindex4ub_bufferoffset = 0;
	rsurface.batchskeletalweight4ub = NULL;
	rsurface.batchskeletalweight4ub_vertexbuffer = NULL;
	rsurface.batchskeletalweight4ub_bufferoffset = 0;
	rsurface.batchelement3i = NULL;
	rsurface.batchelement3i_indexbuffer = NULL;
	rsurface.batchelement3i_bufferoffset = 0;
	rsurface.batchelement3s = NULL;
	rsurface.batchelement3s_indexbuffer = NULL;
	rsurface.batchelement3s_bufferoffset = 0;
	rsurface.forcecurrenttextureupdate = false;
}

void RSurf_ActiveCustomEntity(const matrix4x4_t *matrix, const matrix4x4_t *inversematrix, int entflags, double shadertime, float r, float g, float b, float a, int numvertices, const float *vertex3f, const float *texcoord2f, const float *normal3f, const float *svector3f, const float *tvector3f, const float *color4f, int numtriangles, const int *element3i, const unsigned short *element3s, qbool wantnormals, qbool wanttangents)
{
	rsurface.entity = r_refdef.scene.worldentity;
	if (r != 1.0f || g != 1.0f || b != 1.0f || a != 1.0f) {
		// HACK to provide a valid entity with modded colors to R_GetCurrentTexture.
		// A better approach could be making this copy only once per frame.
		static entity_render_t custom_entity;
		int q;
		custom_entity = *rsurface.entity;
		for (q = 0; q < 3; ++q) {
			float colormod = q == 0 ? r : q == 1 ? g : b;
			custom_entity.render_fullbright[q] *= colormod;
			custom_entity.render_modellight_ambient[q] *= colormod;
			custom_entity.render_modellight_diffuse[q] *= colormod;
			custom_entity.render_lightmap_ambient[q] *= colormod;
			custom_entity.render_lightmap_diffuse[q] *= colormod;
			custom_entity.render_rtlight_diffuse[q] *= colormod;
		}
		custom_entity.alpha *= a;
		rsurface.entity = &custom_entity;
	}
	rsurface.skeleton = NULL;
	rsurface.ent_skinnum = 0;
	rsurface.ent_qwskin = -1;
	rsurface.ent_flags = entflags;
	rsurface.shadertime = r_refdef.scene.time - shadertime;
	rsurface.modelnumvertices = numvertices;
	rsurface.modelnumtriangles = numtriangles;
	rsurface.matrix = *matrix;
	rsurface.inversematrix = *inversematrix;
	rsurface.matrixscale = Matrix4x4_ScaleFromMatrix(&rsurface.matrix);
	rsurface.inversematrixscale = 1.0f / rsurface.matrixscale;
	R_EntityMatrix(&rsurface.matrix);
	Matrix4x4_Transform(&rsurface.inversematrix, r_refdef.view.origin, rsurface.localvieworigin);
	Matrix4x4_TransformStandardPlane(&rsurface.inversematrix, r_refdef.fogplane[0], r_refdef.fogplane[1], r_refdef.fogplane[2], r_refdef.fogplane[3], rsurface.fogplane);
	rsurface.fogplaneviewdist *= rsurface.inversematrixscale;
	rsurface.fograngerecip = r_refdef.fograngerecip * rsurface.matrixscale;
	rsurface.fogheightfade = r_refdef.fogheightfade * rsurface.matrixscale;
	rsurface.fogmasktabledistmultiplier = FOGMASKTABLEWIDTH * rsurface.fograngerecip;
	memset(rsurface.frameblend, 0, sizeof(rsurface.frameblend));
	rsurface.frameblend[0].lerp = 1;
	rsurface.ent_alttextures = false;
	rsurface.basepolygonfactor = r_refdef.polygonfactor;
	rsurface.basepolygonoffset = r_refdef.polygonoffset;
	rsurface.entityskeletaltransform3x4 = NULL;
	rsurface.entityskeletaltransform3x4buffer = NULL;
	rsurface.entityskeletaltransform3x4offset = 0;
	rsurface.entityskeletaltransform3x4size = 0;
	rsurface.entityskeletalnumtransforms = 0;
	r_refdef.stats[r_stat_batch_entitycustom_count]++;
	r_refdef.stats[r_stat_batch_entitycustom_surfaces] += 1;
	r_refdef.stats[r_stat_batch_entitycustom_vertices] += rsurface.modelnumvertices;
	r_refdef.stats[r_stat_batch_entitycustom_triangles] += rsurface.modelnumtriangles;
	if (wanttangents)
	{
		rsurface.modelvertex3f = (float *)vertex3f;
		rsurface.modelsvector3f = svector3f ? (float *)svector3f : (float *)R_FrameData_Alloc(rsurface.modelnumvertices * sizeof(float[3]));
		rsurface.modeltvector3f = tvector3f ? (float *)tvector3f : (float *)R_FrameData_Alloc(rsurface.modelnumvertices * sizeof(float[3]));
		rsurface.modelnormal3f = normal3f ? (float *)normal3f : (float *)R_FrameData_Alloc(rsurface.modelnumvertices * sizeof(float[3]));
	}
	else if (wantnormals)
	{
		rsurface.modelvertex3f = (float *)vertex3f;
		rsurface.modelsvector3f = NULL;
		rsurface.modeltvector3f = NULL;
		rsurface.modelnormal3f = normal3f ? (float *)normal3f : (float *)R_FrameData_Alloc(rsurface.modelnumvertices * sizeof(float[3]));
	}
	else
	{
		rsurface.modelvertex3f = (float *)vertex3f;
		rsurface.modelsvector3f = NULL;
		rsurface.modeltvector3f = NULL;
		rsurface.modelnormal3f = NULL;
	}
	rsurface.modelvertex3f_vertexbuffer = 0;
	rsurface.modelvertex3f_bufferoffset = 0;
	rsurface.modelsvector3f_vertexbuffer = 0;
	rsurface.modelsvector3f_bufferoffset = 0;
	rsurface.modeltvector3f_vertexbuffer = 0;
	rsurface.modeltvector3f_bufferoffset = 0;
	rsurface.modelnormal3f_vertexbuffer = 0;
	rsurface.modelnormal3f_bufferoffset = 0;
	rsurface.modelgeneratedvertex = true;
	rsurface.modellightmapcolor4f  = (float *)color4f;
	rsurface.modellightmapcolor4f_vertexbuffer = 0;
	rsurface.modellightmapcolor4f_bufferoffset = 0;
	rsurface.modeltexcoordtexture2f  = (float *)texcoord2f;
	rsurface.modeltexcoordtexture2f_vertexbuffer = 0;
	rsurface.modeltexcoordtexture2f_bufferoffset = 0;
	rsurface.modeltexcoordlightmap2f  = NULL;
	rsurface.modeltexcoordlightmap2f_vertexbuffer = 0;
	rsurface.modeltexcoordlightmap2f_bufferoffset = 0;
	rsurface.modelskeletalindex4ub = NULL;
	rsurface.modelskeletalindex4ub_vertexbuffer = NULL;
	rsurface.modelskeletalindex4ub_bufferoffset = 0;
	rsurface.modelskeletalweight4ub = NULL;
	rsurface.modelskeletalweight4ub_vertexbuffer = NULL;
	rsurface.modelskeletalweight4ub_bufferoffset = 0;
	rsurface.modelelement3i = (int *)element3i;
	rsurface.modelelement3i_indexbuffer = NULL;
	rsurface.modelelement3i_bufferoffset = 0;
	rsurface.modelelement3s = (unsigned short *)element3s;
	rsurface.modelelement3s_indexbuffer = NULL;
	rsurface.modelelement3s_bufferoffset = 0;
	rsurface.modellightmapoffsets = NULL;
	rsurface.modelsurfaces = NULL;
	rsurface.batchgeneratedvertex = false;
	rsurface.batchfirstvertex = 0;
	rsurface.batchnumvertices = 0;
	rsurface.batchfirsttriangle = 0;
	rsurface.batchnumtriangles = 0;
	rsurface.batchvertex3f  = NULL;
	rsurface.batchvertex3f_vertexbuffer = NULL;
	rsurface.batchvertex3f_bufferoffset = 0;
	rsurface.batchsvector3f = NULL;
	rsurface.batchsvector3f_vertexbuffer = NULL;
	rsurface.batchsvector3f_bufferoffset = 0;
	rsurface.batchtvector3f = NULL;
	rsurface.batchtvector3f_vertexbuffer = NULL;
	rsurface.batchtvector3f_bufferoffset = 0;
	rsurface.batchnormal3f  = NULL;
	rsurface.batchnormal3f_vertexbuffer = NULL;
	rsurface.batchnormal3f_bufferoffset = 0;
	rsurface.batchlightmapcolor4f = NULL;
	rsurface.batchlightmapcolor4f_vertexbuffer = NULL;
	rsurface.batchlightmapcolor4f_bufferoffset = 0;
	rsurface.batchtexcoordtexture2f = NULL;
	rsurface.batchtexcoordtexture2f_vertexbuffer = NULL;
	rsurface.batchtexcoordtexture2f_bufferoffset = 0;
	rsurface.batchtexcoordlightmap2f = NULL;
	rsurface.batchtexcoordlightmap2f_vertexbuffer = NULL;
	rsurface.batchtexcoordlightmap2f_bufferoffset = 0;
	rsurface.batchskeletalindex4ub = NULL;
	rsurface.batchskeletalindex4ub_vertexbuffer = NULL;
	rsurface.batchskeletalindex4ub_bufferoffset = 0;
	rsurface.batchskeletalweight4ub = NULL;
	rsurface.batchskeletalweight4ub_vertexbuffer = NULL;
	rsurface.batchskeletalweight4ub_bufferoffset = 0;
	rsurface.batchelement3i = NULL;
	rsurface.batchelement3i_indexbuffer = NULL;
	rsurface.batchelement3i_bufferoffset = 0;
	rsurface.batchelement3s = NULL;
	rsurface.batchelement3s_indexbuffer = NULL;
	rsurface.batchelement3s_bufferoffset = 0;
	rsurface.forcecurrenttextureupdate = true;

	if (rsurface.modelnumvertices && rsurface.modelelement3i)
	{
		if ((wantnormals || wanttangents) && !normal3f)
		{
			rsurface.modelnormal3f = (float *)R_FrameData_Alloc(rsurface.modelnumvertices * sizeof(float[3]));
			Mod_BuildNormals(0, rsurface.modelnumvertices, rsurface.modelnumtriangles, rsurface.modelvertex3f, rsurface.modelelement3i, rsurface.modelnormal3f, r_smoothnormals_areaweighting.integer != 0);
		}
		if (wanttangents && !svector3f)
		{
			rsurface.modelsvector3f = (float *)R_FrameData_Alloc(rsurface.modelnumvertices * sizeof(float[3]));
			rsurface.modeltvector3f = (float *)R_FrameData_Alloc(rsurface.modelnumvertices * sizeof(float[3]));
			Mod_BuildTextureVectorsFromNormals(0, rsurface.modelnumvertices, rsurface.modelnumtriangles, rsurface.modelvertex3f, rsurface.modeltexcoordtexture2f, rsurface.modelnormal3f, rsurface.modelelement3i, rsurface.modelsvector3f, rsurface.modeltvector3f, r_smoothnormals_areaweighting.integer != 0);
		}
	}
}

float RSurf_FogPoint(const float *v)
{
	// this code is identical to the USEFOGINSIDE/USEFOGOUTSIDE code in the shader
	float FogPlaneViewDist = r_refdef.fogplaneviewdist;
	float FogPlaneVertexDist = DotProduct(r_refdef.fogplane, v) + r_refdef.fogplane[3];
	float FogHeightFade = r_refdef.fogheightfade;
	float fogfrac;
	unsigned int fogmasktableindex;
	if (r_refdef.fogplaneviewabove)
		fogfrac = min(0.0f, FogPlaneVertexDist) / (FogPlaneVertexDist - FogPlaneViewDist) * min(1.0f, min(0.0f, FogPlaneVertexDist) * FogHeightFade);
	else
		fogfrac = FogPlaneViewDist / (FogPlaneViewDist - max(0.0f, FogPlaneVertexDist)) * min(1.0f, (min(0.0f, FogPlaneVertexDist) + FogPlaneViewDist) * FogHeightFade);
	fogmasktableindex = (unsigned int)(VectorDistance(r_refdef.view.origin, v) * fogfrac * r_refdef.fogmasktabledistmultiplier);
	return r_refdef.fogmasktable[min(fogmasktableindex, FOGMASKTABLEWIDTH - 1)];
}

float RSurf_FogVertex(const float *v)
{
	// this code is identical to the USEFOGINSIDE/USEFOGOUTSIDE code in the shader
	float FogPlaneViewDist = rsurface.fogplaneviewdist;
	float FogPlaneVertexDist = DotProduct(rsurface.fogplane, v) + rsurface.fogplane[3];
	float FogHeightFade = rsurface.fogheightfade;
	float fogfrac;
	unsigned int fogmasktableindex;
	if (r_refdef.fogplaneviewabove)
		fogfrac = min(0.0f, FogPlaneVertexDist) / (FogPlaneVertexDist - FogPlaneViewDist) * min(1.0f, min(0.0f, FogPlaneVertexDist) * FogHeightFade);
	else
		fogfrac = FogPlaneViewDist / (FogPlaneViewDist - max(0.0f, FogPlaneVertexDist)) * min(1.0f, (min(0.0f, FogPlaneVertexDist) + FogPlaneViewDist) * FogHeightFade);
	fogmasktableindex = (unsigned int)(VectorDistance(rsurface.localvieworigin, v) * fogfrac * rsurface.fogmasktabledistmultiplier);
	return r_refdef.fogmasktable[min(fogmasktableindex, FOGMASKTABLEWIDTH - 1)];
}

void RSurf_UploadBuffersForBatch(void)
{
	// upload buffer data for generated vertex data (dynamicvertex case) or index data (copytriangles case) and models that lack it to begin with (e.g. DrawQ_FlushUI)
	// note that if rsurface.batchvertex3f_vertexbuffer is NULL, dynamicvertex is forced as we don't account for the proper base vertex here.
	if (rsurface.batchvertex3f && !rsurface.batchvertex3f_vertexbuffer)
		rsurface.batchvertex3f_vertexbuffer = R_BufferData_Store(rsurface.batchnumvertices * sizeof(float[3]), rsurface.batchvertex3f, R_BUFFERDATA_VERTEX, &rsurface.batchvertex3f_bufferoffset);
	if (rsurface.batchsvector3f && !rsurface.batchsvector3f_vertexbuffer)
		rsurface.batchsvector3f_vertexbuffer = R_BufferData_Store(rsurface.batchnumvertices * sizeof(float[3]), rsurface.batchsvector3f, R_BUFFERDATA_VERTEX, &rsurface.batchsvector3f_bufferoffset);
	if (rsurface.batchtvector3f && !rsurface.batchtvector3f_vertexbuffer)
		rsurface.batchtvector3f_vertexbuffer = R_BufferData_Store(rsurface.batchnumvertices * sizeof(float[3]), rsurface.batchtvector3f, R_BUFFERDATA_VERTEX, &rsurface.batchtvector3f_bufferoffset);
	if (rsurface.batchnormal3f && !rsurface.batchnormal3f_vertexbuffer)
		rsurface.batchnormal3f_vertexbuffer = R_BufferData_Store(rsurface.batchnumvertices * sizeof(float[3]), rsurface.batchnormal3f, R_BUFFERDATA_VERTEX, &rsurface.batchnormal3f_bufferoffset);
	if (rsurface.batchlightmapcolor4f && !rsurface.batchlightmapcolor4f_vertexbuffer)
		rsurface.batchlightmapcolor4f_vertexbuffer = R_BufferData_Store(rsurface.batchnumvertices * sizeof(float[4]), rsurface.batchlightmapcolor4f, R_BUFFERDATA_VERTEX, &rsurface.batchlightmapcolor4f_bufferoffset);
	if (rsurface.batchtexcoordtexture2f && !rsurface.batchtexcoordtexture2f_vertexbuffer)
		rsurface.batchtexcoordtexture2f_vertexbuffer = R_BufferData_Store(rsurface.batchnumvertices * sizeof(float[2]), rsurface.batchtexcoordtexture2f, R_BUFFERDATA_VERTEX, &rsurface.batchtexcoordtexture2f_bufferoffset);
	if (rsurface.batchtexcoordlightmap2f && !rsurface.batchtexcoordlightmap2f_vertexbuffer)
		rsurface.batchtexcoordlightmap2f_vertexbuffer = R_BufferData_Store(rsurface.batchnumvertices * sizeof(float[2]), rsurface.batchtexcoordlightmap2f, R_BUFFERDATA_VERTEX, &rsurface.batchtexcoordlightmap2f_bufferoffset);
	if (rsurface.batchskeletalindex4ub && !rsurface.batchskeletalindex4ub_vertexbuffer)
		rsurface.batchskeletalindex4ub_vertexbuffer = R_BufferData_Store(rsurface.batchnumvertices * sizeof(unsigned char[4]), rsurface.batchskeletalindex4ub, R_BUFFERDATA_VERTEX, &rsurface.batchskeletalindex4ub_bufferoffset);
	if (rsurface.batchskeletalweight4ub && !rsurface.batchskeletalweight4ub_vertexbuffer)
		rsurface.batchskeletalweight4ub_vertexbuffer = R_BufferData_Store(rsurface.batchnumvertices * sizeof(unsigned char[4]), rsurface.batchskeletalweight4ub, R_BUFFERDATA_VERTEX, &rsurface.batchskeletalweight4ub_bufferoffset);

	if (rsurface.batchelement3s && !rsurface.batchelement3s_indexbuffer)
		rsurface.batchelement3s_indexbuffer = R_BufferData_Store(rsurface.batchnumtriangles * sizeof(short[3]), rsurface.batchelement3s, R_BUFFERDATA_INDEX16, &rsurface.batchelement3s_bufferoffset);
	else if (rsurface.batchelement3i && !rsurface.batchelement3i_indexbuffer)
		rsurface.batchelement3i_indexbuffer = R_BufferData_Store(rsurface.batchnumtriangles * sizeof(int[3]), rsurface.batchelement3i, R_BUFFERDATA_INDEX32, &rsurface.batchelement3i_bufferoffset);

	R_Mesh_VertexPointer(     3, GL_FLOAT, sizeof(float[3]), rsurface.batchvertex3f, rsurface.batchvertex3f_vertexbuffer, rsurface.batchvertex3f_bufferoffset);
	R_Mesh_ColorPointer(      4, GL_FLOAT, sizeof(float[4]), rsurface.batchlightmapcolor4f, rsurface.batchlightmapcolor4f_vertexbuffer, rsurface.batchlightmapcolor4f_bufferoffset);
	R_Mesh_TexCoordPointer(0, 2, GL_FLOAT, sizeof(float[2]), rsurface.batchtexcoordtexture2f, rsurface.batchtexcoordtexture2f_vertexbuffer, rsurface.batchtexcoordtexture2f_bufferoffset);
	R_Mesh_TexCoordPointer(1, 3, GL_FLOAT, sizeof(float[3]), rsurface.batchsvector3f, rsurface.batchsvector3f_vertexbuffer, rsurface.batchsvector3f_bufferoffset);
	R_Mesh_TexCoordPointer(2, 3, GL_FLOAT, sizeof(float[3]), rsurface.batchtvector3f, rsurface.batchtvector3f_vertexbuffer, rsurface.batchtvector3f_bufferoffset);
	R_Mesh_TexCoordPointer(3, 3, GL_FLOAT, sizeof(float[3]), rsurface.batchnormal3f, rsurface.batchnormal3f_vertexbuffer, rsurface.batchnormal3f_bufferoffset);
	R_Mesh_TexCoordPointer(4, 2, GL_FLOAT, sizeof(float[2]), rsurface.batchtexcoordlightmap2f, rsurface.batchtexcoordlightmap2f_vertexbuffer, rsurface.batchtexcoordlightmap2f_bufferoffset);
	R_Mesh_TexCoordPointer(5, 2, GL_FLOAT, sizeof(float[2]), NULL, NULL, 0);
	R_Mesh_TexCoordPointer(6, 4, GL_UNSIGNED_BYTE | 0x80000000, sizeof(unsigned char[4]), rsurface.batchskeletalindex4ub, rsurface.batchskeletalindex4ub_vertexbuffer, rsurface.batchskeletalindex4ub_bufferoffset);
	R_Mesh_TexCoordPointer(7, 4, GL_UNSIGNED_BYTE, sizeof(unsigned char[4]), rsurface.batchskeletalweight4ub, rsurface.batchskeletalweight4ub_vertexbuffer, rsurface.batchskeletalweight4ub_bufferoffset);
}

static void RSurf_RenumberElements(const int *inelement3i, int *outelement3i, int numelements, int adjust)
{
	int i;
	for (i = 0;i < numelements;i++)
		outelement3i[i] = inelement3i[i] + adjust;
}

static const int quadedges[6][2] = {{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}};
void RSurf_PrepareVerticesForBatch(int batchneed, int texturenumsurfaces, const msurface_t **texturesurfacelist)
{
	int deformindex;
	int firsttriangle;
	int numtriangles;
	int firstvertex;
	int endvertex;
	int numvertices;
	int surfacefirsttriangle;
	int surfacenumtriangles;
	int surfacefirstvertex;
	int surfaceendvertex;
	int surfacenumvertices;
	int batchnumsurfaces = texturenumsurfaces;
	int batchnumvertices;
	int batchnumtriangles;
	int i, j;
	qbool gaps;
	qbool dynamicvertex;
	float amplitude;
	float animpos;
	float center[3], forward[3], right[3], up[3], v[3], newforward[3], newright[3], newup[3];
	float waveparms[4];
	unsigned char *ub;
	q3shaderinfo_deform_t *deform;
	const msurface_t *surface, *firstsurface;
	if (!texturenumsurfaces)
		return;
	// find vertex range of this surface batch
	gaps = false;
	firstsurface = texturesurfacelist[0];
	firsttriangle = firstsurface->num_firsttriangle;
	batchnumvertices = 0;
	batchnumtriangles = 0;
	firstvertex = endvertex = firstsurface->num_firstvertex;
	for (i = 0;i < texturenumsurfaces;i++)
	{
		surface = texturesurfacelist[i];
		if (surface != firstsurface + i)
			gaps = true;
		surfacefirstvertex = surface->num_firstvertex;
		surfaceendvertex = surfacefirstvertex + surface->num_vertices;
		surfacenumvertices = surface->num_vertices;
		surfacenumtriangles = surface->num_triangles;
		if (firstvertex > surfacefirstvertex)
			firstvertex = surfacefirstvertex;
		if (endvertex < surfaceendvertex)
			endvertex = surfaceendvertex;
		batchnumvertices += surfacenumvertices;
		batchnumtriangles += surfacenumtriangles;
	}

	r_refdef.stats[r_stat_batch_batches]++;
	if (gaps)
		r_refdef.stats[r_stat_batch_withgaps]++;
	r_refdef.stats[r_stat_batch_surfaces] += batchnumsurfaces;
	r_refdef.stats[r_stat_batch_vertices] += batchnumvertices;
	r_refdef.stats[r_stat_batch_triangles] += batchnumtriangles;

	// we now know the vertex range used, and if there are any gaps in it
	rsurface.batchfirstvertex = firstvertex;
	rsurface.batchnumvertices = endvertex - firstvertex;
	rsurface.batchfirsttriangle = firsttriangle;
	rsurface.batchnumtriangles = batchnumtriangles;

	// check if any dynamic vertex processing must occur
	dynamicvertex = false;

	// we must use vertexbuffers for rendering, we can upload vertex buffers
	// easily enough but if the basevertex is non-zero it becomes more
	// difficult, so force dynamicvertex path in that case - it's suboptimal
	// but the most optimal case is to have the geometry sources provide their
	// own anyway.
	if (!rsurface.modelvertex3f_vertexbuffer && firstvertex != 0)
		dynamicvertex = true;

	// a cvar to force the dynamic vertex path to be taken, for debugging
	if (r_batch_debugdynamicvertexpath.integer)
	{
		if (!dynamicvertex)
		{
			r_refdef.stats[r_stat_batch_dynamic_batches_because_cvar] += 1;
			r_refdef.stats[r_stat_batch_dynamic_surfaces_because_cvar] += batchnumsurfaces;
			r_refdef.stats[r_stat_batch_dynamic_vertices_because_cvar] += batchnumvertices;
			r_refdef.stats[r_stat_batch_dynamic_triangles_because_cvar] += batchnumtriangles;
		}
		dynamicvertex = true;
	}

	// if there is a chance of animated vertex colors, it's a dynamic batch
	if ((batchneed & BATCHNEED_ARRAY_VERTEXCOLOR) && texturesurfacelist[0]->lightmapinfo)
	{
		if (!dynamicvertex)
		{
			r_refdef.stats[r_stat_batch_dynamic_batches_because_lightmapvertex] += 1;
			r_refdef.stats[r_stat_batch_dynamic_surfaces_because_lightmapvertex] += batchnumsurfaces;
			r_refdef.stats[r_stat_batch_dynamic_vertices_because_lightmapvertex] += batchnumvertices;
			r_refdef.stats[r_stat_batch_dynamic_triangles_because_lightmapvertex] += batchnumtriangles;
		}
		dynamicvertex = true;
	}

	for (deformindex = 0, deform = rsurface.texture->deforms;deformindex < Q3MAXDEFORMS && deform->deform && r_deformvertexes.integer;deformindex++, deform++)
	{
		switch (deform->deform)
		{
		default:
		case Q3DEFORM_PROJECTIONSHADOW:
		case Q3DEFORM_TEXT0:
		case Q3DEFORM_TEXT1:
		case Q3DEFORM_TEXT2:
		case Q3DEFORM_TEXT3:
		case Q3DEFORM_TEXT4:
		case Q3DEFORM_TEXT5:
		case Q3DEFORM_TEXT6:
		case Q3DEFORM_TEXT7:
		case Q3DEFORM_NONE:
			break;
		case Q3DEFORM_AUTOSPRITE:
			if (!dynamicvertex)
			{
				r_refdef.stats[r_stat_batch_dynamic_batches_because_deformvertexes_autosprite] += 1;
				r_refdef.stats[r_stat_batch_dynamic_surfaces_because_deformvertexes_autosprite] += batchnumsurfaces;
				r_refdef.stats[r_stat_batch_dynamic_vertices_because_deformvertexes_autosprite] += batchnumvertices;
				r_refdef.stats[r_stat_batch_dynamic_triangles_because_deformvertexes_autosprite] += batchnumtriangles;
			}
			dynamicvertex = true;
			batchneed |= BATCHNEED_ARRAY_VERTEX | BATCHNEED_ARRAY_NORMAL | BATCHNEED_ARRAY_VECTOR | BATCHNEED_ARRAY_TEXCOORD;
			break;
		case Q3DEFORM_AUTOSPRITE2:
			if (!dynamicvertex)
			{
				r_refdef.stats[r_stat_batch_dynamic_batches_because_deformvertexes_autosprite2] += 1;
				r_refdef.stats[r_stat_batch_dynamic_surfaces_because_deformvertexes_autosprite2] += batchnumsurfaces;
				r_refdef.stats[r_stat_batch_dynamic_vertices_because_deformvertexes_autosprite2] += batchnumvertices;
				r_refdef.stats[r_stat_batch_dynamic_triangles_because_deformvertexes_autosprite2] += batchnumtriangles;
			}
			dynamicvertex = true;
			batchneed |= BATCHNEED_ARRAY_VERTEX | BATCHNEED_ARRAY_TEXCOORD;
			break;
		case Q3DEFORM_NORMAL:
			if (!dynamicvertex)
			{
				r_refdef.stats[r_stat_batch_dynamic_batches_because_deformvertexes_normal] += 1;
				r_refdef.stats[r_stat_batch_dynamic_surfaces_because_deformvertexes_normal] += batchnumsurfaces;
				r_refdef.stats[r_stat_batch_dynamic_vertices_because_deformvertexes_normal] += batchnumvertices;
				r_refdef.stats[r_stat_batch_dynamic_triangles_because_deformvertexes_normal] += batchnumtriangles;
			}
			dynamicvertex = true;
			batchneed |= BATCHNEED_ARRAY_VERTEX | BATCHNEED_ARRAY_NORMAL | BATCHNEED_ARRAY_TEXCOORD;
			break;
		case Q3DEFORM_WAVE:
			if(!R_TestQ3WaveFunc(deform->wavefunc, deform->waveparms))
				break; // if wavefunc is a nop, ignore this transform
			if (!dynamicvertex)
			{
				r_refdef.stats[r_stat_batch_dynamic_batches_because_deformvertexes_wave] += 1;
				r_refdef.stats[r_stat_batch_dynamic_surfaces_because_deformvertexes_wave] += batchnumsurfaces;
				r_refdef.stats[r_stat_batch_dynamic_vertices_because_deformvertexes_wave] += batchnumvertices;
				r_refdef.stats[r_stat_batch_dynamic_triangles_because_deformvertexes_wave] += batchnumtriangles;
			}
			dynamicvertex = true;
			batchneed |= BATCHNEED_ARRAY_VERTEX | BATCHNEED_ARRAY_NORMAL | BATCHNEED_ARRAY_TEXCOORD;
			break;
		case Q3DEFORM_BULGE:
			if (!dynamicvertex)
			{
				r_refdef.stats[r_stat_batch_dynamic_batches_because_deformvertexes_bulge] += 1;
				r_refdef.stats[r_stat_batch_dynamic_surfaces_because_deformvertexes_bulge] += batchnumsurfaces;
				r_refdef.stats[r_stat_batch_dynamic_vertices_because_deformvertexes_bulge] += batchnumvertices;
				r_refdef.stats[r_stat_batch_dynamic_triangles_because_deformvertexes_bulge] += batchnumtriangles;
			}
			dynamicvertex = true;
			batchneed |= BATCHNEED_ARRAY_VERTEX | BATCHNEED_ARRAY_NORMAL | BATCHNEED_ARRAY_TEXCOORD;
			break;
		case Q3DEFORM_MOVE:
			if(!R_TestQ3WaveFunc(deform->wavefunc, deform->waveparms))
				break; // if wavefunc is a nop, ignore this transform
			if (!dynamicvertex)
			{
				r_refdef.stats[r_stat_batch_dynamic_batches_because_deformvertexes_move] += 1;
				r_refdef.stats[r_stat_batch_dynamic_surfaces_because_deformvertexes_move] += batchnumsurfaces;
				r_refdef.stats[r_stat_batch_dynamic_vertices_because_deformvertexes_move] += batchnumvertices;
				r_refdef.stats[r_stat_batch_dynamic_triangles_because_deformvertexes_move] += batchnumtriangles;
			}
			dynamicvertex = true;
			batchneed |= BATCHNEED_ARRAY_VERTEX;
			break;
		}
	}
	if (rsurface.texture->materialshaderpass)
	{
		switch (rsurface.texture->materialshaderpass->tcgen.tcgen)
		{
		default:
		case Q3TCGEN_TEXTURE:
			break;
		case Q3TCGEN_LIGHTMAP:
			if (!dynamicvertex)
			{
				r_refdef.stats[r_stat_batch_dynamic_batches_because_tcgen_lightmap] += 1;
				r_refdef.stats[r_stat_batch_dynamic_surfaces_because_tcgen_lightmap] += batchnumsurfaces;
				r_refdef.stats[r_stat_batch_dynamic_vertices_because_tcgen_lightmap] += batchnumvertices;
				r_refdef.stats[r_stat_batch_dynamic_triangles_because_tcgen_lightmap] += batchnumtriangles;
			}
			dynamicvertex = true;
			batchneed |= BATCHNEED_ARRAY_LIGHTMAP;
			break;
		case Q3TCGEN_VECTOR:
			if (!dynamicvertex)
			{
				r_refdef.stats[r_stat_batch_dynamic_batches_because_tcgen_vector] += 1;
				r_refdef.stats[r_stat_batch_dynamic_surfaces_because_tcgen_vector] += batchnumsurfaces;
				r_refdef.stats[r_stat_batch_dynamic_vertices_because_tcgen_vector] += batchnumvertices;
				r_refdef.stats[r_stat_batch_dynamic_triangles_because_tcgen_vector] += batchnumtriangles;
			}
			dynamicvertex = true;
			batchneed |= BATCHNEED_ARRAY_VERTEX;
			break;
		case Q3TCGEN_ENVIRONMENT:
			if (!dynamicvertex)
			{
				r_refdef.stats[r_stat_batch_dynamic_batches_because_tcgen_environment] += 1;
				r_refdef.stats[r_stat_batch_dynamic_surfaces_because_tcgen_environment] += batchnumsurfaces;
				r_refdef.stats[r_stat_batch_dynamic_vertices_because_tcgen_environment] += batchnumvertices;
				r_refdef.stats[r_stat_batch_dynamic_triangles_because_tcgen_environment] += batchnumtriangles;
			}
			dynamicvertex = true;
			batchneed |= BATCHNEED_ARRAY_VERTEX | BATCHNEED_ARRAY_NORMAL;
			break;
		}
		if (rsurface.texture->materialshaderpass->tcmods[0].tcmod == Q3TCMOD_TURBULENT)
		{
			if (!dynamicvertex)
			{
				r_refdef.stats[r_stat_batch_dynamic_batches_because_tcmod_turbulent] += 1;
				r_refdef.stats[r_stat_batch_dynamic_surfaces_because_tcmod_turbulent] += batchnumsurfaces;
				r_refdef.stats[r_stat_batch_dynamic_vertices_because_tcmod_turbulent] += batchnumvertices;
				r_refdef.stats[r_stat_batch_dynamic_triangles_because_tcmod_turbulent] += batchnumtriangles;
			}
			dynamicvertex = true;
			batchneed |= BATCHNEED_ARRAY_VERTEX | BATCHNEED_ARRAY_TEXCOORD;
		}
	}

	// the caller can specify BATCHNEED_NOGAPS to force a batch with
	// firstvertex = 0 and endvertex = numvertices (no gaps, no firstvertex),
	// we ensure this by treating the vertex batch as dynamic...
	if ((batchneed & BATCHNEED_ALWAYSCOPY) || ((batchneed & BATCHNEED_NOGAPS) && (gaps || firstvertex > 0)))
	{
		if (!dynamicvertex)
		{
			r_refdef.stats[r_stat_batch_dynamic_batches_because_nogaps] += 1;
			r_refdef.stats[r_stat_batch_dynamic_surfaces_because_nogaps] += batchnumsurfaces;
			r_refdef.stats[r_stat_batch_dynamic_vertices_because_nogaps] += batchnumvertices;
			r_refdef.stats[r_stat_batch_dynamic_triangles_because_nogaps] += batchnumtriangles;
		}
		dynamicvertex = true;
	}

	// if we're going to have to apply the skeletal transform manually, we need to batch the skeletal data
	if (dynamicvertex && rsurface.entityskeletaltransform3x4)
		batchneed |= BATCHNEED_ARRAY_SKELETAL;

	rsurface.batchvertex3f = rsurface.modelvertex3f;
	rsurface.batchvertex3f_vertexbuffer = rsurface.modelvertex3f_vertexbuffer;
	rsurface.batchvertex3f_bufferoffset = rsurface.modelvertex3f_bufferoffset;
	rsurface.batchsvector3f = rsurface.modelsvector3f;
	rsurface.batchsvector3f_vertexbuffer = rsurface.modelsvector3f_vertexbuffer;
	rsurface.batchsvector3f_bufferoffset = rsurface.modelsvector3f_bufferoffset;
	rsurface.batchtvector3f = rsurface.modeltvector3f;
	rsurface.batchtvector3f_vertexbuffer = rsurface.modeltvector3f_vertexbuffer;
	rsurface.batchtvector3f_bufferoffset = rsurface.modeltvector3f_bufferoffset;
	rsurface.batchnormal3f = rsurface.modelnormal3f;
	rsurface.batchnormal3f_vertexbuffer = rsurface.modelnormal3f_vertexbuffer;
	rsurface.batchnormal3f_bufferoffset = rsurface.modelnormal3f_bufferoffset;
	rsurface.batchlightmapcolor4f = rsurface.modellightmapcolor4f;
	rsurface.batchlightmapcolor4f_vertexbuffer  = rsurface.modellightmapcolor4f_vertexbuffer;
	rsurface.batchlightmapcolor4f_bufferoffset  = rsurface.modellightmapcolor4f_bufferoffset;
	rsurface.batchtexcoordtexture2f = rsurface.modeltexcoordtexture2f;
	rsurface.batchtexcoordtexture2f_vertexbuffer  = rsurface.modeltexcoordtexture2f_vertexbuffer;
	rsurface.batchtexcoordtexture2f_bufferoffset  = rsurface.modeltexcoordtexture2f_bufferoffset;
	rsurface.batchtexcoordlightmap2f = rsurface.modeltexcoordlightmap2f;
	rsurface.batchtexcoordlightmap2f_vertexbuffer = rsurface.modeltexcoordlightmap2f_vertexbuffer;
	rsurface.batchtexcoordlightmap2f_bufferoffset = rsurface.modeltexcoordlightmap2f_bufferoffset;
	rsurface.batchskeletalindex4ub = rsurface.modelskeletalindex4ub;
	rsurface.batchskeletalindex4ub_vertexbuffer = rsurface.modelskeletalindex4ub_vertexbuffer;
	rsurface.batchskeletalindex4ub_bufferoffset = rsurface.modelskeletalindex4ub_bufferoffset;
	rsurface.batchskeletalweight4ub = rsurface.modelskeletalweight4ub;
	rsurface.batchskeletalweight4ub_vertexbuffer = rsurface.modelskeletalweight4ub_vertexbuffer;
	rsurface.batchskeletalweight4ub_bufferoffset = rsurface.modelskeletalweight4ub_bufferoffset;
	rsurface.batchelement3i = rsurface.modelelement3i;
	rsurface.batchelement3i_indexbuffer = rsurface.modelelement3i_indexbuffer;
	rsurface.batchelement3i_bufferoffset = rsurface.modelelement3i_bufferoffset;
	rsurface.batchelement3s = rsurface.modelelement3s;
	rsurface.batchelement3s_indexbuffer = rsurface.modelelement3s_indexbuffer;
	rsurface.batchelement3s_bufferoffset = rsurface.modelelement3s_bufferoffset;
	rsurface.batchskeletaltransform3x4 = rsurface.entityskeletaltransform3x4;
	rsurface.batchskeletaltransform3x4buffer = rsurface.entityskeletaltransform3x4buffer;
	rsurface.batchskeletaltransform3x4offset = rsurface.entityskeletaltransform3x4offset;
	rsurface.batchskeletaltransform3x4size = rsurface.entityskeletaltransform3x4size;
	rsurface.batchskeletalnumtransforms = rsurface.entityskeletalnumtransforms;

	// if any dynamic vertex processing has to occur in software, we copy the
	// entire surface list together before processing to rebase the vertices
	// to start at 0 (otherwise we waste a lot of room in a vertex buffer).
	//
	// if any gaps exist and we do not have a static vertex buffer, we have to
	// copy the surface list together to avoid wasting upload bandwidth on the
	// vertices in the gaps.
	//
	// if gaps exist and we have a static vertex buffer, we can choose whether
	// to combine the index buffer ranges into one dynamic index buffer or
	// simply issue multiple glDrawElements calls (BATCHNEED_ALLOWMULTIDRAW).
	//
	// in many cases the batch is reduced to one draw call.

	rsurface.batchmultidraw = false;
	rsurface.batchmultidrawnumsurfaces = 0;
	rsurface.batchmultidrawsurfacelist = NULL;

	if (!dynamicvertex)
	{
		// static vertex data, just set pointers...
		rsurface.batchgeneratedvertex = false;
		// if there are gaps, we want to build a combined index buffer,
		// otherwise use the original static buffer with an appropriate offset
		if (gaps)
		{
			r_refdef.stats[r_stat_batch_copytriangles_batches] += 1;
			r_refdef.stats[r_stat_batch_copytriangles_surfaces] += batchnumsurfaces;
			r_refdef.stats[r_stat_batch_copytriangles_vertices] += batchnumvertices;
			r_refdef.stats[r_stat_batch_copytriangles_triangles] += batchnumtriangles;
			if ((batchneed & BATCHNEED_ALLOWMULTIDRAW) && r_batch_multidraw.integer && batchnumtriangles >= r_batch_multidraw_mintriangles.integer)
			{
				rsurface.batchmultidraw = true;
				rsurface.batchmultidrawnumsurfaces = texturenumsurfaces;
				rsurface.batchmultidrawsurfacelist = texturesurfacelist;
				return;
			}
			// build a new triangle elements array for this batch
			rsurface.batchelement3i = (int *)R_FrameData_Alloc(batchnumtriangles * sizeof(int[3]));
			rsurface.batchfirsttriangle = 0;
			numtriangles = 0;
			for (i = 0;i < texturenumsurfaces;i++)
			{
				surfacefirsttriangle = texturesurfacelist[i]->num_firsttriangle;
				surfacenumtriangles = texturesurfacelist[i]->num_triangles;
				memcpy(rsurface.batchelement3i + 3*numtriangles, rsurface.modelelement3i + 3*surfacefirsttriangle, surfacenumtriangles*sizeof(int[3]));
				numtriangles += surfacenumtriangles;
			}
			rsurface.batchelement3i_indexbuffer = NULL;
			rsurface.batchelement3i_bufferoffset = 0;
			rsurface.batchelement3s = NULL;
			rsurface.batchelement3s_indexbuffer = NULL;
			rsurface.batchelement3s_bufferoffset = 0;
			if (endvertex <= 65536)
			{
				// make a 16bit (unsigned short) index array if possible
				rsurface.batchelement3s = (unsigned short *)R_FrameData_Alloc(batchnumtriangles * sizeof(unsigned short[3]));
				for (i = 0;i < numtriangles*3;i++)
					rsurface.batchelement3s[i] = rsurface.batchelement3i[i];
			}
		}
		else
		{
			r_refdef.stats[r_stat_batch_fast_batches] += 1;
			r_refdef.stats[r_stat_batch_fast_surfaces] += batchnumsurfaces;
			r_refdef.stats[r_stat_batch_fast_vertices] += batchnumvertices;
			r_refdef.stats[r_stat_batch_fast_triangles] += batchnumtriangles;
		}
		return;
	}

	// something needs software processing, do it for real...
	// we only directly handle separate array data in this case and then
	// generate interleaved data if needed...
	rsurface.batchgeneratedvertex = true;
	r_refdef.stats[r_stat_batch_dynamic_batches] += 1;
	r_refdef.stats[r_stat_batch_dynamic_surfaces] += batchnumsurfaces;
	r_refdef.stats[r_stat_batch_dynamic_vertices] += batchnumvertices;
	r_refdef.stats[r_stat_batch_dynamic_triangles] += batchnumtriangles;

	// now copy the vertex data into a combined array and make an index array
	// (this is what Quake3 does all the time)
	// we also apply any skeletal animation here that would have been done in
	// the vertex shader, because most of the dynamic vertex animation cases
	// need actual vertex positions and normals
	//if (dynamicvertex)
	{
		rsurface.batchvertex3f = NULL;
		rsurface.batchvertex3f_vertexbuffer = NULL;
		rsurface.batchvertex3f_bufferoffset = 0;
		rsurface.batchsvector3f = NULL;
		rsurface.batchsvector3f_vertexbuffer = NULL;
		rsurface.batchsvector3f_bufferoffset = 0;
		rsurface.batchtvector3f = NULL;
		rsurface.batchtvector3f_vertexbuffer = NULL;
		rsurface.batchtvector3f_bufferoffset = 0;
		rsurface.batchnormal3f = NULL;
		rsurface.batchnormal3f_vertexbuffer = NULL;
		rsurface.batchnormal3f_bufferoffset = 0;
		rsurface.batchlightmapcolor4f = NULL;
		rsurface.batchlightmapcolor4f_vertexbuffer = NULL;
		rsurface.batchlightmapcolor4f_bufferoffset = 0;
		rsurface.batchtexcoordtexture2f = NULL;
		rsurface.batchtexcoordtexture2f_vertexbuffer = NULL;
		rsurface.batchtexcoordtexture2f_bufferoffset = 0;
		rsurface.batchtexcoordlightmap2f = NULL;
		rsurface.batchtexcoordlightmap2f_vertexbuffer = NULL;
		rsurface.batchtexcoordlightmap2f_bufferoffset = 0;
		rsurface.batchskeletalindex4ub = NULL;
		rsurface.batchskeletalindex4ub_vertexbuffer = NULL;
		rsurface.batchskeletalindex4ub_bufferoffset = 0;
		rsurface.batchskeletalweight4ub = NULL;
		rsurface.batchskeletalweight4ub_vertexbuffer = NULL;
		rsurface.batchskeletalweight4ub_bufferoffset = 0;
		rsurface.batchelement3i = (int *)R_FrameData_Alloc(batchnumtriangles * sizeof(int[3]));
		rsurface.batchelement3i_indexbuffer = NULL;
		rsurface.batchelement3i_bufferoffset = 0;
		rsurface.batchelement3s = NULL;
		rsurface.batchelement3s_indexbuffer = NULL;
		rsurface.batchelement3s_bufferoffset = 0;
		rsurface.batchskeletaltransform3x4buffer = NULL;
		rsurface.batchskeletaltransform3x4offset = 0;
		rsurface.batchskeletaltransform3x4size = 0;
		// we'll only be setting up certain arrays as needed
		if (batchneed & BATCHNEED_ARRAY_VERTEX)
			rsurface.batchvertex3f = (float *)R_FrameData_Alloc(batchnumvertices * sizeof(float[3]));
		if (batchneed & BATCHNEED_ARRAY_NORMAL)
			rsurface.batchnormal3f = (float *)R_FrameData_Alloc(batchnumvertices * sizeof(float[3]));
		if (batchneed & BATCHNEED_ARRAY_VECTOR)
		{
			rsurface.batchsvector3f = (float *)R_FrameData_Alloc(batchnumvertices * sizeof(float[3]));
			rsurface.batchtvector3f = (float *)R_FrameData_Alloc(batchnumvertices * sizeof(float[3]));
		}
		if (batchneed & BATCHNEED_ARRAY_VERTEXCOLOR)
			rsurface.batchlightmapcolor4f = (float *)R_FrameData_Alloc(batchnumvertices * sizeof(float[4]));
		if (batchneed & BATCHNEED_ARRAY_TEXCOORD)
			rsurface.batchtexcoordtexture2f = (float *)R_FrameData_Alloc(batchnumvertices * sizeof(float[2]));
		if (batchneed & BATCHNEED_ARRAY_LIGHTMAP)
			rsurface.batchtexcoordlightmap2f = (float *)R_FrameData_Alloc(batchnumvertices * sizeof(float[2]));
		if (batchneed & BATCHNEED_ARRAY_SKELETAL)
		{
			rsurface.batchskeletalindex4ub = (unsigned char *)R_FrameData_Alloc(batchnumvertices * sizeof(unsigned char[4]));
			rsurface.batchskeletalweight4ub = (unsigned char *)R_FrameData_Alloc(batchnumvertices * sizeof(unsigned char[4]));
		}
		numvertices = 0;
		numtriangles = 0;
		for (i = 0;i < texturenumsurfaces;i++)
		{
			surfacefirstvertex = texturesurfacelist[i]->num_firstvertex;
			surfacenumvertices = texturesurfacelist[i]->num_vertices;
			surfacefirsttriangle = texturesurfacelist[i]->num_firsttriangle;
			surfacenumtriangles = texturesurfacelist[i]->num_triangles;
			// copy only the data requested
			if (batchneed & (BATCHNEED_ARRAY_VERTEX | BATCHNEED_ARRAY_NORMAL | BATCHNEED_ARRAY_VECTOR | BATCHNEED_ARRAY_VERTEXCOLOR | BATCHNEED_ARRAY_TEXCOORD | BATCHNEED_ARRAY_LIGHTMAP))
			{
				if (batchneed & BATCHNEED_ARRAY_VERTEX)
				{
					if (rsurface.batchvertex3f)
						memcpy(rsurface.batchvertex3f + 3*numvertices, rsurface.modelvertex3f + 3*surfacefirstvertex, surfacenumvertices * sizeof(float[3]));
					else
						memset(rsurface.batchvertex3f + 3*numvertices, 0, surfacenumvertices * sizeof(float[3]));
				}
				if (batchneed & BATCHNEED_ARRAY_NORMAL)
				{
					if (rsurface.modelnormal3f)
						memcpy(rsurface.batchnormal3f + 3*numvertices, rsurface.modelnormal3f + 3*surfacefirstvertex, surfacenumvertices * sizeof(float[3]));
					else
						memset(rsurface.batchnormal3f + 3*numvertices, 0, surfacenumvertices * sizeof(float[3]));
				}
				if (batchneed & BATCHNEED_ARRAY_VECTOR)
				{
					if (rsurface.modelsvector3f)
					{
						memcpy(rsurface.batchsvector3f + 3*numvertices, rsurface.modelsvector3f + 3*surfacefirstvertex, surfacenumvertices * sizeof(float[3]));
						memcpy(rsurface.batchtvector3f + 3*numvertices, rsurface.modeltvector3f + 3*surfacefirstvertex, surfacenumvertices * sizeof(float[3]));
					}
					else
					{
						memset(rsurface.batchsvector3f + 3*numvertices, 0, surfacenumvertices * sizeof(float[3]));
						memset(rsurface.batchtvector3f + 3*numvertices, 0, surfacenumvertices * sizeof(float[3]));
					}
				}
				if (batchneed & BATCHNEED_ARRAY_VERTEXCOLOR)
				{
					if (rsurface.modellightmapcolor4f)
						memcpy(rsurface.batchlightmapcolor4f + 4*numvertices, rsurface.modellightmapcolor4f + 4*surfacefirstvertex, surfacenumvertices * sizeof(float[4]));
					else
						memset(rsurface.batchlightmapcolor4f + 4*numvertices, 0, surfacenumvertices * sizeof(float[4]));
				}
				if (batchneed & BATCHNEED_ARRAY_TEXCOORD)
				{
					if (rsurface.modeltexcoordtexture2f)
						memcpy(rsurface.batchtexcoordtexture2f + 2*numvertices, rsurface.modeltexcoordtexture2f + 2*surfacefirstvertex, surfacenumvertices * sizeof(float[2]));
					else
						memset(rsurface.batchtexcoordtexture2f + 2*numvertices, 0, surfacenumvertices * sizeof(float[2]));
				}
				if (batchneed & BATCHNEED_ARRAY_LIGHTMAP)
				{
					if (rsurface.modeltexcoordlightmap2f)
						memcpy(rsurface.batchtexcoordlightmap2f + 2*numvertices, rsurface.modeltexcoordlightmap2f + 2*surfacefirstvertex, surfacenumvertices * sizeof(float[2]));
					else
						memset(rsurface.batchtexcoordlightmap2f + 2*numvertices, 0, surfacenumvertices * sizeof(float[2]));
				}
				if (batchneed & BATCHNEED_ARRAY_SKELETAL)
				{
					if (rsurface.modelskeletalindex4ub)
					{
						memcpy(rsurface.batchskeletalindex4ub + 4*numvertices, rsurface.modelskeletalindex4ub + 4*surfacefirstvertex, surfacenumvertices * sizeof(unsigned char[4]));
						memcpy(rsurface.batchskeletalweight4ub + 4*numvertices, rsurface.modelskeletalweight4ub + 4*surfacefirstvertex, surfacenumvertices * sizeof(unsigned char[4]));
					}
					else
					{
						memset(rsurface.batchskeletalindex4ub + 4*numvertices, 0, surfacenumvertices * sizeof(unsigned char[4]));
						memset(rsurface.batchskeletalweight4ub + 4*numvertices, 0, surfacenumvertices * sizeof(unsigned char[4]));
						ub = rsurface.batchskeletalweight4ub + 4*numvertices;
						for (j = 0;j < surfacenumvertices;j++)
							ub[j*4] = 255;
					}
				}
			}
			RSurf_RenumberElements(rsurface.modelelement3i + 3*surfacefirsttriangle, rsurface.batchelement3i + 3*numtriangles, 3*surfacenumtriangles, numvertices - surfacefirstvertex);
			numvertices += surfacenumvertices;
			numtriangles += surfacenumtriangles;
		}

		// generate a 16bit index array as well if possible
		// (in general, dynamic batches fit)
		if (numvertices <= 65536)
		{
			rsurface.batchelement3s = (unsigned short *)R_FrameData_Alloc(batchnumtriangles * sizeof(unsigned short[3]));
			for (i = 0;i < numtriangles*3;i++)
				rsurface.batchelement3s[i] = rsurface.batchelement3i[i];
		}

		// since we've copied everything, the batch now starts at 0
		rsurface.batchfirstvertex = 0;
		rsurface.batchnumvertices = batchnumvertices;
		rsurface.batchfirsttriangle = 0;
		rsurface.batchnumtriangles = batchnumtriangles;
	}

	// apply skeletal animation that would have been done in the vertex shader
	if (rsurface.batchskeletaltransform3x4)
	{
		const unsigned char *si;
		const unsigned char *sw;
		const float *t[4];
		const float *b = rsurface.batchskeletaltransform3x4;
		float *vp, *vs, *vt, *vn;
		float w[4];
		float m[3][4], n[3][4];
		float tp[3], ts[3], tt[3], tn[3];
		r_refdef.stats[r_stat_batch_dynamicskeletal_batches] += 1;
		r_refdef.stats[r_stat_batch_dynamicskeletal_surfaces] += batchnumsurfaces;
		r_refdef.stats[r_stat_batch_dynamicskeletal_vertices] += batchnumvertices;
		r_refdef.stats[r_stat_batch_dynamicskeletal_triangles] += batchnumtriangles;
		si = rsurface.batchskeletalindex4ub;
		sw = rsurface.batchskeletalweight4ub;
		vp = rsurface.batchvertex3f;
		vs = rsurface.batchsvector3f;
		vt = rsurface.batchtvector3f;
		vn = rsurface.batchnormal3f;
		memset(m[0], 0, sizeof(m));
		memset(n[0], 0, sizeof(n));
		for (i = 0;i < batchnumvertices;i++)
		{
			t[0] = b + si[0]*12;
			if (sw[0] == 255)
			{
				// common case - only one matrix
				m[0][0] = t[0][ 0];
				m[0][1] = t[0][ 1];
				m[0][2] = t[0][ 2];
				m[0][3] = t[0][ 3];
				m[1][0] = t[0][ 4];
				m[1][1] = t[0][ 5];
				m[1][2] = t[0][ 6];
				m[1][3] = t[0][ 7];
				m[2][0] = t[0][ 8];
				m[2][1] = t[0][ 9];
				m[2][2] = t[0][10];
				m[2][3] = t[0][11];
			}
			else if (sw[2] + sw[3])
			{
				// blend 4 matrices
				t[1] = b + si[1]*12;
				t[2] = b + si[2]*12;
				t[3] = b + si[3]*12;
				w[0] = sw[0] * (1.0f / 255.0f);
				w[1] = sw[1] * (1.0f / 255.0f);
				w[2] = sw[2] * (1.0f / 255.0f);
				w[3] = sw[3] * (1.0f / 255.0f);
				// blend the matrices
				m[0][0] = t[0][ 0] * w[0] + t[1][ 0] * w[1] + t[2][ 0] * w[2] + t[3][ 0] * w[3];
				m[0][1] = t[0][ 1] * w[0] + t[1][ 1] * w[1] + t[2][ 1] * w[2] + t[3][ 1] * w[3];
				m[0][2] = t[0][ 2] * w[0] + t[1][ 2] * w[1] + t[2][ 2] * w[2] + t[3][ 2] * w[3];
				m[0][3] = t[0][ 3] * w[0] + t[1][ 3] * w[1] + t[2][ 3] * w[2] + t[3][ 3] * w[3];
				m[1][0] = t[0][ 4] * w[0] + t[1][ 4] * w[1] + t[2][ 4] * w[2] + t[3][ 4] * w[3];
				m[1][1] = t[0][ 5] * w[0] + t[1][ 5] * w[1] + t[2][ 5] * w[2] + t[3][ 5] * w[3];
				m[1][2] = t[0][ 6] * w[0] + t[1][ 6] * w[1] + t[2][ 6] * w[2] + t[3][ 6] * w[3];
				m[1][3] = t[0][ 7] * w[0] + t[1][ 7] * w[1] + t[2][ 7] * w[2] + t[3][ 7] * w[3];
				m[2][0] = t[0][ 8] * w[0] + t[1][ 8] * w[1] + t[2][ 8] * w[2] + t[3][ 8] * w[3];
				m[2][1] = t[0][ 9] * w[0] + t[1][ 9] * w[1] + t[2][ 9] * w[2] + t[3][ 9] * w[3];
				m[2][2] = t[0][10] * w[0] + t[1][10] * w[1] + t[2][10] * w[2] + t[3][10] * w[3];
				m[2][3] = t[0][11] * w[0] + t[1][11] * w[1] + t[2][11] * w[2] + t[3][11] * w[3];
			}
			else
			{
				// blend 2 matrices
				t[1] = b + si[1]*12;
				w[0] = sw[0] * (1.0f / 255.0f);
				w[1] = sw[1] * (1.0f / 255.0f);
				// blend the matrices
				m[0][0] = t[0][ 0] * w[0] + t[1][ 0] * w[1];
				m[0][1] = t[0][ 1] * w[0] + t[1][ 1] * w[1];
				m[0][2] = t[0][ 2] * w[0] + t[1][ 2] * w[1];
				m[0][3] = t[0][ 3] * w[0] + t[1][ 3] * w[1];
				m[1][0] = t[0][ 4] * w[0] + t[1][ 4] * w[1];
				m[1][1] = t[0][ 5] * w[0] + t[1][ 5] * w[1];
				m[1][2] = t[0][ 6] * w[0] + t[1][ 6] * w[1];
				m[1][3] = t[0][ 7] * w[0] + t[1][ 7] * w[1];
				m[2][0] = t[0][ 8] * w[0] + t[1][ 8] * w[1];
				m[2][1] = t[0][ 9] * w[0] + t[1][ 9] * w[1];
				m[2][2] = t[0][10] * w[0] + t[1][10] * w[1];
				m[2][3] = t[0][11] * w[0] + t[1][11] * w[1];
			}
			si += 4;
			sw += 4;
			// modify the vertex
			VectorCopy(vp, tp);
			vp[0] = tp[0] * m[0][0] + tp[1] * m[0][1] + tp[2] * m[0][2] + m[0][3];
			vp[1] = tp[0] * m[1][0] + tp[1] * m[1][1] + tp[2] * m[1][2] + m[1][3];
			vp[2] = tp[0] * m[2][0] + tp[1] * m[2][1] + tp[2] * m[2][2] + m[2][3];
			vp += 3;
			if (vn)
			{
				// the normal transformation matrix is a set of cross products...
				CrossProduct(m[1], m[2], n[0]);
				CrossProduct(m[2], m[0], n[1]);
				CrossProduct(m[0], m[1], n[2]); // is actually transpose(inverse(m)) * det(m)
				VectorCopy(vn, tn);
				vn[0] = tn[0] * n[0][0] + tn[1] * n[0][1] + tn[2] * n[0][2];
				vn[1] = tn[0] * n[1][0] + tn[1] * n[1][1] + tn[2] * n[1][2];
				vn[2] = tn[0] * n[2][0] + tn[1] * n[2][1] + tn[2] * n[2][2];
				VectorNormalize(vn);
				vn += 3;
				if (vs)
				{
					VectorCopy(vs, ts);
					vs[0] = ts[0] * n[0][0] + ts[1] * n[0][1] + ts[2] * n[0][2];
					vs[1] = ts[0] * n[1][0] + ts[1] * n[1][1] + ts[2] * n[1][2];
					vs[2] = ts[0] * n[2][0] + ts[1] * n[2][1] + ts[2] * n[2][2];
					VectorNormalize(vs);
					vs += 3;
					VectorCopy(vt, tt);
					vt[0] = tt[0] * n[0][0] + tt[1] * n[0][1] + tt[2] * n[0][2];
					vt[1] = tt[0] * n[1][0] + tt[1] * n[1][1] + tt[2] * n[1][2];
					vt[2] = tt[0] * n[2][0] + tt[1] * n[2][1] + tt[2] * n[2][2];
					VectorNormalize(vt);
					vt += 3;
				}
			}
		}
		rsurface.batchskeletaltransform3x4 = NULL;
		rsurface.batchskeletalnumtransforms = 0;
	}

	// q1bsp surfaces rendered in vertex color mode have to have colors
	// calculated based on lightstyles
	if ((batchneed & BATCHNEED_ARRAY_VERTEXCOLOR) && texturesurfacelist[0]->lightmapinfo)
	{
		// generate color arrays for the surfaces in this list
		int c[4];
		int scale;
		int size3;
		const int *offsets;
		const unsigned char *lm;
		rsurface.batchlightmapcolor4f = (float *)R_FrameData_Alloc(batchnumvertices * sizeof(float[4]));
		rsurface.batchlightmapcolor4f_vertexbuffer = NULL;
		rsurface.batchlightmapcolor4f_bufferoffset = 0;
		numvertices = 0;
		for (i = 0;i < texturenumsurfaces;i++)
		{
			surface = texturesurfacelist[i];
			offsets = rsurface.modellightmapoffsets + surface->num_firstvertex;
			surfacenumvertices = surface->num_vertices;
			if (surface->lightmapinfo->samples)
			{
				for (j = 0;j < surfacenumvertices;j++)
				{
					lm = surface->lightmapinfo->samples + offsets[j];
					scale = r_refdef.scene.lightstylevalue[surface->lightmapinfo->styles[0]];
					VectorScale(lm, scale, c);
					if (surface->lightmapinfo->styles[1] != 255)
					{
						size3 = ((surface->lightmapinfo->extents[0]>>4)+1)*((surface->lightmapinfo->extents[1]>>4)+1)*3;
						lm += size3;
						scale = r_refdef.scene.lightstylevalue[surface->lightmapinfo->styles[1]];
						VectorMA(c, scale, lm, c);
						if (surface->lightmapinfo->styles[2] != 255)
						{
							lm += size3;
							scale = r_refdef.scene.lightstylevalue[surface->lightmapinfo->styles[2]];
							VectorMA(c, scale, lm, c);
							if (surface->lightmapinfo->styles[3] != 255)
							{
								lm += size3;
								scale = r_refdef.scene.lightstylevalue[surface->lightmapinfo->styles[3]];
								VectorMA(c, scale, lm, c);
							}
						}
					}
					c[0] >>= 7;
					c[1] >>= 7;
					c[2] >>= 7;
					Vector4Set(rsurface.batchlightmapcolor4f + 4*numvertices, min(c[0], 255) * (1.0f / 255.0f), min(c[1], 255) * (1.0f / 255.0f), min(c[2], 255) * (1.0f / 255.0f), 1);
					numvertices++;
				}
			}
			else
			{
				for (j = 0;j < surfacenumvertices;j++)
				{
					Vector4Set(rsurface.batchlightmapcolor4f + 4*numvertices, 0, 0, 0, 1);
					numvertices++;
				}
			}
		}
	}

	// if vertices are deformed (sprite flares and things in maps, possibly
	// water waves, bulges and other deformations), modify the copied vertices
	// in place
	for (deformindex = 0, deform = rsurface.texture->deforms;deformindex < Q3MAXDEFORMS && deform->deform && r_deformvertexes.integer;deformindex++, deform++)
	{
		float scale;
		switch (deform->deform)
		{
		default:
		case Q3DEFORM_PROJECTIONSHADOW:
		case Q3DEFORM_TEXT0:
		case Q3DEFORM_TEXT1:
		case Q3DEFORM_TEXT2:
		case Q3DEFORM_TEXT3:
		case Q3DEFORM_TEXT4:
		case Q3DEFORM_TEXT5:
		case Q3DEFORM_TEXT6:
		case Q3DEFORM_TEXT7:
		case Q3DEFORM_NONE:
			break;
		case Q3DEFORM_AUTOSPRITE:
			Matrix4x4_Transform3x3(&rsurface.inversematrix, r_refdef.view.forward, newforward);
			Matrix4x4_Transform3x3(&rsurface.inversematrix, r_refdef.view.right, newright);
			Matrix4x4_Transform3x3(&rsurface.inversematrix, r_refdef.view.up, newup);
			VectorNormalize(newforward);
			VectorNormalize(newright);
			VectorNormalize(newup);
//			rsurface.batchvertex3f = R_FrameData_Store(batchnumvertices * sizeof(float[3]), rsurface.batchvertex3f);
//			rsurface.batchvertex3f_vertexbuffer = NULL;
//			rsurface.batchvertex3f_bufferoffset = 0;
//			rsurface.batchsvector3f = R_FrameData_Store(batchnumvertices * sizeof(float[3]), rsurface.batchsvector3f);
//			rsurface.batchsvector3f_vertexbuffer = NULL;
//			rsurface.batchsvector3f_bufferoffset = 0;
//			rsurface.batchtvector3f = R_FrameData_Store(batchnumvertices * sizeof(float[3]), rsurface.batchtvector3f);
//			rsurface.batchtvector3f_vertexbuffer = NULL;
//			rsurface.batchtvector3f_bufferoffset = 0;
//			rsurface.batchnormal3f = R_FrameData_Store(batchnumvertices * sizeof(float[3]), rsurface.batchnormal3f);
//			rsurface.batchnormal3f_vertexbuffer = NULL;
//			rsurface.batchnormal3f_bufferoffset = 0;
			// sometimes we're on a renderpath that does not use vectors (GL11/GL13/GLES1)
			if (!VectorLength2(rsurface.batchnormal3f + 3*rsurface.batchfirstvertex))
				Mod_BuildNormals(rsurface.batchfirstvertex, batchnumvertices, batchnumtriangles, rsurface.batchvertex3f, rsurface.batchelement3i + 3 * rsurface.batchfirsttriangle, rsurface.batchnormal3f, r_smoothnormals_areaweighting.integer != 0);
			if (!VectorLength2(rsurface.batchsvector3f + 3*rsurface.batchfirstvertex))
				Mod_BuildTextureVectorsFromNormals(rsurface.batchfirstvertex, batchnumvertices, batchnumtriangles, rsurface.batchvertex3f, rsurface.batchtexcoordtexture2f, rsurface.batchnormal3f, rsurface.batchelement3i + 3 * rsurface.batchfirsttriangle, rsurface.batchsvector3f, rsurface.batchtvector3f, r_smoothnormals_areaweighting.integer != 0);
			// a single autosprite surface can contain multiple sprites...
			for (j = 0;j < batchnumvertices - 3;j += 4)
			{
				VectorClear(center);
				for (i = 0;i < 4;i++)
					VectorAdd(center, rsurface.batchvertex3f + 3*(j+i), center);
				VectorScale(center, 0.25f, center);
				VectorCopy(rsurface.batchnormal3f + 3*j, forward);
				VectorCopy(rsurface.batchsvector3f + 3*j, right);
				VectorCopy(rsurface.batchtvector3f + 3*j, up);
				for (i = 0;i < 4;i++)
				{
					VectorSubtract(rsurface.batchvertex3f + 3*(j+i), center, v);
					VectorMAMAMAM(1, center, DotProduct(forward, v), newforward, DotProduct(right, v), newright, DotProduct(up, v), newup, rsurface.batchvertex3f + 3*(j+i));
				}
			}
			// if we get here, BATCHNEED_ARRAY_NORMAL and BATCHNEED_ARRAY_VECTOR are in batchneed, so no need to check
			Mod_BuildNormals(rsurface.batchfirstvertex, batchnumvertices, batchnumtriangles, rsurface.batchvertex3f, rsurface.batchelement3i + 3 * rsurface.batchfirsttriangle, rsurface.batchnormal3f, r_smoothnormals_areaweighting.integer != 0);
			Mod_BuildTextureVectorsFromNormals(rsurface.batchfirstvertex, batchnumvertices, batchnumtriangles, rsurface.batchvertex3f, rsurface.batchtexcoordtexture2f, rsurface.batchnormal3f, rsurface.batchelement3i + 3 * rsurface.batchfirsttriangle, rsurface.batchsvector3f, rsurface.batchtvector3f, r_smoothnormals_areaweighting.integer != 0);
			break;
		case Q3DEFORM_AUTOSPRITE2:
			Matrix4x4_Transform3x3(&rsurface.inversematrix, r_refdef.view.forward, newforward);
			Matrix4x4_Transform3x3(&rsurface.inversematrix, r_refdef.view.right, newright);
			Matrix4x4_Transform3x3(&rsurface.inversematrix, r_refdef.view.up, newup);
			VectorNormalize(newforward);
			VectorNormalize(newright);
			VectorNormalize(newup);
//			rsurface.batchvertex3f = R_FrameData_Store(batchnumvertices * sizeof(float[3]), rsurface.batchvertex3f);
//			rsurface.batchvertex3f_vertexbuffer = NULL;
//			rsurface.batchvertex3f_bufferoffset = 0;
			{
				const float *v1, *v2;
				vec3_t start, end;
				float f, l;
				struct
				{
					float length2;
					const float *v1;
					const float *v2;
				}
				shortest[2];
				memset(shortest, 0, sizeof(shortest));
				// a single autosprite surface can contain multiple sprites...
				for (j = 0;j < batchnumvertices - 3;j += 4)
				{
					VectorClear(center);
					for (i = 0;i < 4;i++)
						VectorAdd(center, rsurface.batchvertex3f + 3*(j+i), center);
					VectorScale(center, 0.25f, center);
					// find the two shortest edges, then use them to define the
					// axis vectors for rotating around the central axis
					for (i = 0;i < 6;i++)
					{
						v1 = rsurface.batchvertex3f + 3*(j+quadedges[i][0]);
						v2 = rsurface.batchvertex3f + 3*(j+quadedges[i][1]);
						l = VectorDistance2(v1, v2);
						// this length bias tries to make sense of square polygons, assuming they are meant to be upright
						if (v1[2] != v2[2])
							l += (1.0f / 1024.0f);
						if (shortest[0].length2 > l || i == 0)
						{
							shortest[1] = shortest[0];
							shortest[0].length2 = l;
							shortest[0].v1 = v1;
							shortest[0].v2 = v2;
						}
						else if (shortest[1].length2 > l || i == 1)
						{
							shortest[1].length2 = l;
							shortest[1].v1 = v1;
							shortest[1].v2 = v2;
						}
					}
					VectorLerp(shortest[0].v1, 0.5f, shortest[0].v2, start);
					VectorLerp(shortest[1].v1, 0.5f, shortest[1].v2, end);
					// this calculates the right vector from the shortest edge
					// and the up vector from the edge midpoints
					VectorSubtract(shortest[0].v1, shortest[0].v2, right);
					VectorNormalize(right);
					VectorSubtract(end, start, up);
					VectorNormalize(up);
					// calculate a forward vector to use instead of the original plane normal (this is how we get a new right vector)
					VectorSubtract(rsurface.localvieworigin, center, forward);
					//Matrix4x4_Transform3x3(&rsurface.inversematrix, r_refdef.view.forward, forward);
					VectorNegate(forward, forward);
					VectorReflect(forward, 0, up, forward);
					VectorNormalize(forward);
					CrossProduct(up, forward, newright);
					VectorNormalize(newright);
					// rotate the quad around the up axis vector, this is made
					// especially easy by the fact we know the quad is flat,
					// so we only have to subtract the center position and
					// measure distance along the right vector, and then
					// multiply that by the newright vector and add back the
					// center position
					// we also need to subtract the old position to undo the
					// displacement from the center, which we do with a
					// DotProduct, the subtraction/addition of center is also
					// optimized into DotProducts here
					l = DotProduct(right, center);
					for (i = 0;i < 4;i++)
					{
						v1 = rsurface.batchvertex3f + 3*(j+i);
						f = DotProduct(right, v1) - l;
						VectorMAMAM(1, v1, -f, right, f, newright, rsurface.batchvertex3f + 3*(j+i));
					}
				}
			}
			if(batchneed & (BATCHNEED_ARRAY_NORMAL | BATCHNEED_ARRAY_VECTOR)) // otherwise these can stay NULL
			{
//				rsurface.batchnormal3f = R_FrameData_Alloc(batchnumvertices * sizeof(float[3]));
//				rsurface.batchnormal3f_vertexbuffer = NULL;
//				rsurface.batchnormal3f_bufferoffset = 0;
				Mod_BuildNormals(rsurface.batchfirstvertex, batchnumvertices, batchnumtriangles, rsurface.batchvertex3f, rsurface.batchelement3i + 3 * rsurface.batchfirsttriangle, rsurface.batchnormal3f, r_smoothnormals_areaweighting.integer != 0);
			}
			if(batchneed & BATCHNEED_ARRAY_VECTOR) // otherwise these can stay NULL
			{
//				rsurface.batchsvector3f = R_FrameData_Alloc(batchnumvertices * sizeof(float[3]));
//				rsurface.batchsvector3f_vertexbuffer = NULL;
//				rsurface.batchsvector3f_bufferoffset = 0;
//				rsurface.batchtvector3f = R_FrameData_Alloc(batchnumvertices * sizeof(float[3]));
//				rsurface.batchtvector3f_vertexbuffer = NULL;
//				rsurface.batchtvector3f_bufferoffset = 0;
				Mod_BuildTextureVectorsFromNormals(rsurface.batchfirstvertex, batchnumvertices, batchnumtriangles, rsurface.batchvertex3f, rsurface.batchtexcoordtexture2f, rsurface.batchnormal3f, rsurface.batchelement3i + 3 * rsurface.batchfirsttriangle, rsurface.batchsvector3f, rsurface.batchtvector3f, r_smoothnormals_areaweighting.integer != 0);
			}
			break;
		case Q3DEFORM_NORMAL:
			// deform the normals to make reflections wavey
			rsurface.batchnormal3f = (float *)R_FrameData_Store(batchnumvertices * sizeof(float[3]), rsurface.batchnormal3f);
			rsurface.batchnormal3f_vertexbuffer = NULL;
			rsurface.batchnormal3f_bufferoffset = 0;
			for (j = 0;j < batchnumvertices;j++)
			{
				float vertex[3];
				float *normal = rsurface.batchnormal3f + 3*j;
				VectorScale(rsurface.batchvertex3f + 3*j, 0.98f, vertex);
				normal[0] = rsurface.batchnormal3f[j*3+0] + deform->parms[0] * noise4f(      vertex[0], vertex[1], vertex[2], rsurface.shadertime * deform->parms[1]);
				normal[1] = rsurface.batchnormal3f[j*3+1] + deform->parms[0] * noise4f( 98 + vertex[0], vertex[1], vertex[2], rsurface.shadertime * deform->parms[1]);
				normal[2] = rsurface.batchnormal3f[j*3+2] + deform->parms[0] * noise4f(196 + vertex[0], vertex[1], vertex[2], rsurface.shadertime * deform->parms[1]);
				VectorNormalize(normal);
			}
			if(batchneed & BATCHNEED_ARRAY_VECTOR) // otherwise these can stay NULL
			{
//				rsurface.batchsvector3f = R_FrameData_Alloc(batchnumvertices * sizeof(float[3]));
//				rsurface.batchsvector3f_vertexbuffer = NULL;
//				rsurface.batchsvector3f_bufferoffset = 0;
//				rsurface.batchtvector3f = R_FrameData_Alloc(batchnumvertices * sizeof(float[3]));
//				rsurface.batchtvector3f_vertexbuffer = NULL;
//				rsurface.batchtvector3f_bufferoffset = 0;
				Mod_BuildTextureVectorsFromNormals(rsurface.batchfirstvertex, batchnumvertices, batchnumtriangles, rsurface.batchvertex3f, rsurface.batchtexcoordtexture2f, rsurface.batchnormal3f, rsurface.batchelement3i + 3 * rsurface.batchfirsttriangle, rsurface.batchsvector3f, rsurface.batchtvector3f, r_smoothnormals_areaweighting.integer != 0);
			}
			break;
		case Q3DEFORM_WAVE:
			// deform vertex array to make wavey water and flags and such
			waveparms[0] = deform->waveparms[0];
			waveparms[1] = deform->waveparms[1];
			waveparms[2] = deform->waveparms[2];
			waveparms[3] = deform->waveparms[3];
			if(!R_TestQ3WaveFunc(deform->wavefunc, waveparms))
				break; // if wavefunc is a nop, don't make a dynamic vertex array
			// this is how a divisor of vertex influence on deformation
			animpos = deform->parms[0] ? 1.0f / deform->parms[0] : 100.0f;
			scale = R_EvaluateQ3WaveFunc(deform->wavefunc, waveparms);
//			rsurface.batchvertex3f = R_FrameData_Store(batchnumvertices * sizeof(float[3]), rsurface.batchvertex3f);
//			rsurface.batchvertex3f_vertexbuffer = NULL;
//			rsurface.batchvertex3f_bufferoffset = 0;
//			rsurface.batchnormal3f = R_FrameData_Store(batchnumvertices * sizeof(float[3]), rsurface.batchnormal3f);
//			rsurface.batchnormal3f_vertexbuffer = NULL;
//			rsurface.batchnormal3f_bufferoffset = 0;
			for (j = 0;j < batchnumvertices;j++)
			{
				// if the wavefunc depends on time, evaluate it per-vertex
				if (waveparms[3])
				{
					waveparms[2] = deform->waveparms[2] + (rsurface.batchvertex3f[j*3+0] + rsurface.batchvertex3f[j*3+1] + rsurface.batchvertex3f[j*3+2]) * animpos;
					scale = R_EvaluateQ3WaveFunc(deform->wavefunc, waveparms);
				}
				VectorMA(rsurface.batchvertex3f + 3*j, scale, rsurface.batchnormal3f + 3*j, rsurface.batchvertex3f + 3*j);
			}
			// if we get here, BATCHNEED_ARRAY_NORMAL is in batchneed, so no need to check
			Mod_BuildNormals(rsurface.batchfirstvertex, batchnumvertices, batchnumtriangles, rsurface.batchvertex3f, rsurface.batchelement3i + 3 * rsurface.batchfirsttriangle, rsurface.batchnormal3f, r_smoothnormals_areaweighting.integer != 0);
			if(batchneed & BATCHNEED_ARRAY_VECTOR) // otherwise these can stay NULL
			{
//				rsurface.batchsvector3f = R_FrameData_Alloc(batchnumvertices * sizeof(float[3]));
//				rsurface.batchsvector3f_vertexbuffer = NULL;
//				rsurface.batchsvector3f_bufferoffset = 0;
//				rsurface.batchtvector3f = R_FrameData_Alloc(batchnumvertices * sizeof(float[3]));
//				rsurface.batchtvector3f_vertexbuffer = NULL;
//				rsurface.batchtvector3f_bufferoffset = 0;
				Mod_BuildTextureVectorsFromNormals(rsurface.batchfirstvertex, batchnumvertices, batchnumtriangles, rsurface.batchvertex3f, rsurface.batchtexcoordtexture2f, rsurface.batchnormal3f, rsurface.batchelement3i + 3 * rsurface.batchfirsttriangle, rsurface.batchsvector3f, rsurface.batchtvector3f, r_smoothnormals_areaweighting.integer != 0);
			}
			break;
		case Q3DEFORM_BULGE:
			// deform vertex array to make the surface have moving bulges
//			rsurface.batchvertex3f = R_FrameData_Store(batchnumvertices * sizeof(float[3]), rsurface.batchvertex3f);
//			rsurface.batchvertex3f_vertexbuffer = NULL;
//			rsurface.batchvertex3f_bufferoffset = 0;
//			rsurface.batchnormal3f = R_FrameData_Store(batchnumvertices * sizeof(float[3]), rsurface.batchnormal3f);
//			rsurface.batchnormal3f_vertexbuffer = NULL;
//			rsurface.batchnormal3f_bufferoffset = 0;
			for (j = 0;j < batchnumvertices;j++)
			{
				scale = sin(rsurface.batchtexcoordtexture2f[j*2+0] * deform->parms[0] + rsurface.shadertime * deform->parms[2]) * deform->parms[1];
				VectorMA(rsurface.batchvertex3f + 3*j, scale, rsurface.batchnormal3f + 3*j, rsurface.batchvertex3f + 3*j);
			}
			// if we get here, BATCHNEED_ARRAY_NORMAL is in batchneed, so no need to check
			Mod_BuildNormals(rsurface.batchfirstvertex, batchnumvertices, batchnumtriangles, rsurface.batchvertex3f, rsurface.batchelement3i + 3 * rsurface.batchfirsttriangle, rsurface.batchnormal3f, r_smoothnormals_areaweighting.integer != 0);
			if(batchneed & BATCHNEED_ARRAY_VECTOR) // otherwise these can stay NULL
			{
//				rsurface.batchsvector3f = R_FrameData_Alloc(batchnumvertices * sizeof(float[3]));
//				rsurface.batchsvector3f_vertexbuffer = NULL;
//				rsurface.batchsvector3f_bufferoffset = 0;
//				rsurface.batchtvector3f = R_FrameData_Alloc(batchnumvertices * sizeof(float[3]));
//				rsurface.batchtvector3f_vertexbuffer = NULL;
//				rsurface.batchtvector3f_bufferoffset = 0;
				Mod_BuildTextureVectorsFromNormals(rsurface.batchfirstvertex, batchnumvertices, batchnumtriangles, rsurface.batchvertex3f, rsurface.batchtexcoordtexture2f, rsurface.batchnormal3f, rsurface.batchelement3i + 3 * rsurface.batchfirsttriangle, rsurface.batchsvector3f, rsurface.batchtvector3f, r_smoothnormals_areaweighting.integer != 0);
			}
			break;
		case Q3DEFORM_MOVE:
			// deform vertex array
			if(!R_TestQ3WaveFunc(deform->wavefunc, deform->waveparms))
				break; // if wavefunc is a nop, don't make a dynamic vertex array
			scale = R_EvaluateQ3WaveFunc(deform->wavefunc, deform->waveparms);
			VectorScale(deform->parms, scale, waveparms);
//			rsurface.batchvertex3f = R_FrameData_Store(batchnumvertices * sizeof(float[3]), rsurface.batchvertex3f);
//			rsurface.batchvertex3f_vertexbuffer = NULL;
//			rsurface.batchvertex3f_bufferoffset = 0;
			for (j = 0;j < batchnumvertices;j++)
				VectorAdd(rsurface.batchvertex3f + 3*j, waveparms, rsurface.batchvertex3f + 3*j);
			break;
		}
	}

	if (rsurface.batchtexcoordtexture2f && rsurface.texture->materialshaderpass)
	{
	// generate texcoords based on the chosen texcoord source
		switch(rsurface.texture->materialshaderpass->tcgen.tcgen)
		{
		default:
		case Q3TCGEN_TEXTURE:
			break;
		case Q3TCGEN_LIGHTMAP:
	//		rsurface.batchtexcoordtexture2f = R_FrameData_Alloc(batchnumvertices * sizeof(float[2]));
	//		rsurface.batchtexcoordtexture2f_vertexbuffer = NULL;
	//		rsurface.batchtexcoordtexture2f_bufferoffset = 0;
			if (rsurface.batchtexcoordlightmap2f)
				memcpy(rsurface.batchtexcoordtexture2f, rsurface.batchtexcoordlightmap2f, batchnumvertices * sizeof(float[2]));
			break;
		case Q3TCGEN_VECTOR:
	//		rsurface.batchtexcoordtexture2f = R_FrameData_Alloc(batchnumvertices * sizeof(float[2]));
	//		rsurface.batchtexcoordtexture2f_vertexbuffer = NULL;
	//		rsurface.batchtexcoordtexture2f_bufferoffset = 0;
			for (j = 0;j < batchnumvertices;j++)
			{
				rsurface.batchtexcoordtexture2f[j*2+0] = DotProduct(rsurface.batchvertex3f + 3*j, rsurface.texture->materialshaderpass->tcgen.parms);
				rsurface.batchtexcoordtexture2f[j*2+1] = DotProduct(rsurface.batchvertex3f + 3*j, rsurface.texture->materialshaderpass->tcgen.parms + 3);
			}
			break;
		case Q3TCGEN_ENVIRONMENT:
			// make environment reflections using a spheremap
			rsurface.batchtexcoordtexture2f = (float *)R_FrameData_Alloc(batchnumvertices * sizeof(float[2]));
			rsurface.batchtexcoordtexture2f_vertexbuffer = NULL;
			rsurface.batchtexcoordtexture2f_bufferoffset = 0;
			for (j = 0;j < batchnumvertices;j++)
			{
				// identical to Q3A's method, but executed in worldspace so
				// carried models can be shiny too

				float viewer[3], d, reflected[3], worldreflected[3];

				VectorSubtract(rsurface.localvieworigin, rsurface.batchvertex3f + 3*j, viewer);
				// VectorNormalize(viewer);

				d = DotProduct(rsurface.batchnormal3f + 3*j, viewer);

				reflected[0] = rsurface.batchnormal3f[j*3+0]*2*d - viewer[0];
				reflected[1] = rsurface.batchnormal3f[j*3+1]*2*d - viewer[1];
				reflected[2] = rsurface.batchnormal3f[j*3+2]*2*d - viewer[2];
				// note: this is proportinal to viewer, so we can normalize later

				Matrix4x4_Transform3x3(&rsurface.matrix, reflected, worldreflected);
				VectorNormalize(worldreflected);

				// note: this sphere map only uses world x and z!
				// so positive and negative y will LOOK THE SAME.
				rsurface.batchtexcoordtexture2f[j*2+0] = 0.5 + 0.5 * worldreflected[1];
				rsurface.batchtexcoordtexture2f[j*2+1] = 0.5 - 0.5 * worldreflected[2];
			}
			break;
		}
		// the only tcmod that needs software vertex processing is turbulent, so
		// check for it here and apply the changes if needed
		// and we only support that as the first one
		// (handling a mixture of turbulent and other tcmods would be problematic
		//  without punting it entirely to a software path)
		if (rsurface.texture->materialshaderpass->tcmods[0].tcmod == Q3TCMOD_TURBULENT)
		{
			amplitude = rsurface.texture->materialshaderpass->tcmods[0].parms[1];
			animpos = rsurface.texture->materialshaderpass->tcmods[0].parms[2] + rsurface.shadertime * rsurface.texture->materialshaderpass->tcmods[0].parms[3];
	//		rsurface.batchtexcoordtexture2f = R_FrameData_Alloc(batchnumvertices * sizeof(float[2]));
	//		rsurface.batchtexcoordtexture2f_vertexbuffer = NULL;
	//		rsurface.batchtexcoordtexture2f_bufferoffset = 0;
			for (j = 0;j < batchnumvertices;j++)
			{
				rsurface.batchtexcoordtexture2f[j*2+0] += amplitude * sin(((rsurface.batchvertex3f[j*3+0] + rsurface.batchvertex3f[j*3+2]) * 1.0 / 1024.0f + animpos) * M_PI * 2);
				rsurface.batchtexcoordtexture2f[j*2+1] += amplitude * sin(((rsurface.batchvertex3f[j*3+1]                                ) * 1.0 / 1024.0f + animpos) * M_PI * 2);
			}
		}
	}
}

void RSurf_DrawBatch(void)
{
	// sometimes a zero triangle surface (usually a degenerate patch) makes it
	// through the pipeline, killing it earlier in the pipeline would have
	// per-surface overhead rather than per-batch overhead, so it's best to
	// reject it here, before it hits glDraw.
	if (rsurface.batchnumtriangles == 0)
		return;
#if 0
	// batch debugging code
	if (r_test.integer && rsurface.entity == r_refdef.scene.worldentity && rsurface.batchvertex3f == r_refdef.scene.worldentity->model->surfmesh.data_vertex3f)
	{
		int i;
		int j;
		int c;
		const int *e;
		e = rsurface.batchelement3i + rsurface.batchfirsttriangle*3;
		for (i = 0;i < rsurface.batchnumtriangles*3;i++)
		{
			c = e[i];
			for (j = 0;j < rsurface.entity->model->num_surfaces;j++)
			{
				if (c >= rsurface.modelsurfaces[j].num_firstvertex && c < (rsurface.modelsurfaces[j].num_firstvertex + rsurface.modelsurfaces[j].num_vertices))
				{
					if (rsurface.modelsurfaces[j].texture != rsurface.texture)
						Sys_Error("RSurf_DrawBatch: index %i uses different texture (%s) than surface %i which it belongs to (which uses %s)\n", c, rsurface.texture->name, j, rsurface.modelsurfaces[j].texture->name);
					break;
				}
			}
		}
	}
#endif
	if (rsurface.batchmultidraw)
	{
		// issue multiple draws rather than copying index data
		int numsurfaces = rsurface.batchmultidrawnumsurfaces;
		const msurface_t **surfacelist = rsurface.batchmultidrawsurfacelist;
		int i, j, k, firstvertex, endvertex, firsttriangle, endtriangle;
		for (i = 0;i < numsurfaces;)
		{
			// combine consecutive surfaces as one draw
			for (k = i, j = i + 1;j < numsurfaces;k = j, j++)
				if (surfacelist[j] != surfacelist[k] + 1)
					break;
			firstvertex = surfacelist[i]->num_firstvertex;
			endvertex = surfacelist[k]->num_firstvertex + surfacelist[k]->num_vertices;
			firsttriangle = surfacelist[i]->num_firsttriangle;
			endtriangle = surfacelist[k]->num_firsttriangle + surfacelist[k]->num_triangles;
			R_Mesh_Draw(firstvertex, endvertex - firstvertex, firsttriangle, endtriangle - firsttriangle, rsurface.batchelement3i, rsurface.batchelement3i_indexbuffer, rsurface.batchelement3i_bufferoffset, rsurface.batchelement3s, rsurface.batchelement3s_indexbuffer, rsurface.batchelement3s_bufferoffset);
			i = j;
		}
	}
	else
	{
		// there is only one consecutive run of index data (may have been combined)
		R_Mesh_Draw(rsurface.batchfirstvertex, rsurface.batchnumvertices, rsurface.batchfirsttriangle, rsurface.batchnumtriangles, rsurface.batchelement3i, rsurface.batchelement3i_indexbuffer, rsurface.batchelement3i_bufferoffset, rsurface.batchelement3s, rsurface.batchelement3s_indexbuffer, rsurface.batchelement3s_bufferoffset);
	}
}

static int RSurf_FindWaterPlaneForSurface(const msurface_t *surface)
{
	// pick the closest matching water plane
	int planeindex, vertexindex, bestplaneindex = -1;
	float d, bestd;
	vec3_t vert;
	const float *v;
	r_waterstate_waterplane_t *p;
	qbool prepared = false;
	bestd = 0;
	for (planeindex = 0, p = r_fb.water.waterplanes;planeindex < r_fb.water.numwaterplanes;planeindex++, p++)
	{
		if(p->camera_entity != rsurface.texture->camera_entity)
			continue;
		d = 0;
		if(!prepared)
		{
			RSurf_PrepareVerticesForBatch(BATCHNEED_ARRAY_VERTEX, 1, &surface);
			prepared = true;
			if(rsurface.batchnumvertices == 0)
				break;
		}
		for (vertexindex = 0, v = rsurface.batchvertex3f + rsurface.batchfirstvertex * 3;vertexindex < rsurface.batchnumvertices;vertexindex++, v += 3)
		{
			Matrix4x4_Transform(&rsurface.matrix, v, vert);
			d += fabs(PlaneDiff(vert, &p->plane));
		}
		if (bestd > d || bestplaneindex < 0)
		{
			bestd = d;
			bestplaneindex = planeindex;
		}
	}
	return bestplaneindex;
	// NOTE: this MAY return a totally unrelated water plane; we can ignore
	// this situation though, as it might be better to render single larger
	// batches with useless stuff (backface culled for example) than to
	// render multiple smaller batches
}

void RSurf_SetupDepthAndCulling(bool ui)
{
	// submodels are biased to avoid z-fighting with world surfaces that they
	// may be exactly overlapping (avoids z-fighting artifacts on certain
	// doors and things in Quake maps)
	GL_DepthRange(0, (rsurface.texture->currentmaterialflags & MATERIALFLAG_SHORTDEPTHRANGE) ? 0.0625 : 1);
	GL_PolygonOffset(rsurface.basepolygonfactor + rsurface.texture->biaspolygonfactor, rsurface.basepolygonoffset + rsurface.texture->biaspolygonoffset);
	GL_DepthTest(!ui && !(rsurface.texture->currentmaterialflags & MATERIALFLAG_NODEPTHTEST));
	GL_CullFace((rsurface.texture->currentmaterialflags & MATERIALFLAG_NOCULLFACE) ? GL_NONE : r_refdef.view.cullface_back);
}

static void R_DrawTextureSurfaceList_Sky(int texturenumsurfaces, const msurface_t **texturesurfacelist)
{
	int j;
	const float *v;
	float p[3], mins[3], maxs[3];
	int scissor[4];
	// transparent sky would be ridiculous
	if (rsurface.texture->currentmaterialflags & MATERIALFLAGMASK_DEPTHSORTED)
		return;
	R_SetupShader_Generic_NoTexture(false, false);
	skyrenderlater = true;
	RSurf_SetupDepthAndCulling(false);
	GL_DepthMask(true);

	// add the vertices of the surfaces to a world bounding box so we can scissor the sky render later
	if (r_sky_scissor.integer)
	{
		RSurf_PrepareVerticesForBatch(BATCHNEED_ARRAY_VERTEX | BATCHNEED_NOGAPS, texturenumsurfaces, texturesurfacelist);
		for (j = 0, v = rsurface.batchvertex3f + 3 * rsurface.batchfirstvertex; j < rsurface.batchnumvertices; j++, v += 3)
		{
			Matrix4x4_Transform(&rsurface.matrix, v, p);
			if (j > 0)
			{
				if (mins[0] > p[0]) mins[0] = p[0];
				if (mins[1] > p[1]) mins[1] = p[1];
				if (mins[2] > p[2]) mins[2] = p[2];
				if (maxs[0] < p[0]) maxs[0] = p[0];
				if (maxs[1] < p[1]) maxs[1] = p[1];
				if (maxs[2] < p[2]) maxs[2] = p[2];
			}
			else
			{
				VectorCopy(p, mins);
				VectorCopy(p, maxs);
			}
		}
		if (!R_ScissorForBBox(mins, maxs, scissor))
		{
			if (skyscissor[2])
			{
				if (skyscissor[0] > scissor[0])
				{
					skyscissor[2] += skyscissor[0] - scissor[0];
					skyscissor[0] = scissor[0];
				}
				if (skyscissor[1] > scissor[1])
				{
					skyscissor[3] += skyscissor[1] - scissor[1];
					skyscissor[1] = scissor[1];
				}
				if (skyscissor[0] + skyscissor[2] < scissor[0] + scissor[2])
					skyscissor[2] = scissor[0] + scissor[2] - skyscissor[0];
				if (skyscissor[1] + skyscissor[3] < scissor[1] + scissor[3])
					skyscissor[3] = scissor[1] + scissor[3] - skyscissor[1];
			}
			else
				Vector4Copy(scissor, skyscissor);
		}
	}

	// LadyHavoc: HalfLife maps have freaky skypolys so don't use
	// skymasking on them, and Quake3 never did sky masking (unlike
	// software Quake and software Quake2), so disable the sky masking
	// in Quake3 maps as it causes problems with q3map2 sky tricks,
	// and skymasking also looks very bad when noclipping outside the
	// level, so don't use it then either.
	if (r_refdef.scene.worldmodel && r_refdef.scene.worldmodel->brush.skymasking && (r_refdef.scene.worldmodel->brush.isq3bsp ? r_q3bsp_renderskydepth.integer : r_q1bsp_skymasking.integer) && !r_refdef.viewcache.world_novis && !r_trippy.integer)
	{
		R_Mesh_ResetTextureState();
		if (skyrendermasked)
		{
			R_SetupShader_DepthOrShadow(false, false, false);
			// depth-only (masking)
			GL_ColorMask(0, 0, 0, 0);
			// just to make sure that braindead drivers don't draw
			// anything despite that colormask...
			GL_BlendFunc(GL_ZERO, GL_ONE);
			RSurf_PrepareVerticesForBatch(BATCHNEED_ARRAY_VERTEX | BATCHNEED_ALLOWMULTIDRAW, texturenumsurfaces, texturesurfacelist);
			R_Mesh_PrepareVertices_Vertex3f(rsurface.batchnumvertices, rsurface.batchvertex3f, rsurface.batchvertex3f_vertexbuffer, rsurface.batchvertex3f_bufferoffset);
		}
		else
		{
			R_SetupShader_Generic_NoTexture(false, false);
			// fog sky
			GL_BlendFunc(GL_ONE, GL_ZERO);
			RSurf_PrepareVerticesForBatch(BATCHNEED_ARRAY_VERTEX | BATCHNEED_NOGAPS, texturenumsurfaces, texturesurfacelist);
			GL_Color(r_refdef.fogcolor[0], r_refdef.fogcolor[1], r_refdef.fogcolor[2], 1);
			R_Mesh_PrepareVertices_Generic_Arrays(rsurface.batchnumvertices, rsurface.batchvertex3f, NULL, NULL);
		}
		RSurf_DrawBatch();
		if (skyrendermasked)
			GL_ColorMask(r_refdef.view.colormask[0], r_refdef.view.colormask[1], r_refdef.view.colormask[2], 1);
	}
	R_Mesh_ResetTextureState();
	GL_Color(1, 1, 1, 1);
}

extern rtexture_t *r_shadow_prepasslightingdiffusetexture;
extern rtexture_t *r_shadow_prepasslightingspeculartexture;
static void R_DrawTextureSurfaceList_GL20(int texturenumsurfaces, const msurface_t **texturesurfacelist, qbool writedepth, qbool prepass, qbool ui)
{
	if (r_fb.water.renderingscene && (rsurface.texture->currentmaterialflags & (MATERIALFLAG_WATERSHADER | MATERIALFLAG_REFRACTION | MATERIALFLAG_REFLECTION | MATERIALFLAG_CAMERA)))
		return;
	if (prepass)
	{
		// render screenspace normalmap to texture
		GL_DepthMask(true);
		R_SetupShader_Surface(vec3_origin, vec3_origin, vec3_origin, RSURFPASS_DEFERREDGEOMETRY, texturenumsurfaces, texturesurfacelist, NULL, false, false);
		RSurf_DrawBatch();
		return;
	}

	// bind lightmap texture

	// water/refraction/reflection/camera surfaces have to be handled specially
	if ((rsurface.texture->currentmaterialflags & (MATERIALFLAG_WATERSHADER | MATERIALFLAG_REFRACTION | MATERIALFLAG_CAMERA | MATERIALFLAG_REFLECTION)))
	{
		int start, end, startplaneindex;
		for (start = 0;start < texturenumsurfaces;start = end)
		{
			startplaneindex = RSurf_FindWaterPlaneForSurface(texturesurfacelist[start]);
			if(startplaneindex < 0)
			{
				// this happens if the plane e.g. got backface culled and thus didn't get a water plane. We can just ignore this.
				// Con_Printf("No matching water plane for surface with material flags 0x%08x - PLEASE DEBUG THIS\n", rsurface.texture->currentmaterialflags);
				end = start + 1;
				continue;
			}
			for (end = start + 1;end < texturenumsurfaces && startplaneindex == RSurf_FindWaterPlaneForSurface(texturesurfacelist[end]);end++)
				;
			// now that we have a batch using the same planeindex, render it
			if ((rsurface.texture->currentmaterialflags & (MATERIALFLAG_WATERSHADER | MATERIALFLAG_REFRACTION | MATERIALFLAG_CAMERA)))
			{
				// render water or distortion background
				GL_DepthMask(true);
				R_SetupShader_Surface(vec3_origin, vec3_origin, vec3_origin, RSURFPASS_BACKGROUND, end-start, texturesurfacelist + start, (void *)(r_fb.water.waterplanes + startplaneindex), false, false);
				RSurf_DrawBatch();
				// blend surface on top
				GL_DepthMask(false);
				R_SetupShader_Surface(vec3_origin, vec3_origin, vec3_origin, RSURFPASS_BASE, end-start, texturesurfacelist + start, NULL, false, false);
				RSurf_DrawBatch();
			}
			else if ((rsurface.texture->currentmaterialflags & MATERIALFLAG_REFLECTION))
			{
				// render surface with reflection texture as input
				GL_DepthMask(writedepth && !(rsurface.texture->currentmaterialflags & MATERIALFLAG_BLENDED));
				R_SetupShader_Surface(vec3_origin, vec3_origin, vec3_origin, RSURFPASS_BASE, end-start, texturesurfacelist + start, (void *)(r_fb.water.waterplanes + startplaneindex), false, false);
				RSurf_DrawBatch();
			}
		}
		return;
	}

	// render surface batch normally (the glow re-add pass rewrites nothing:
	// same geometry, same depths, additive colour only)
	GL_DepthMask(writedepth && !(rsurface.texture->currentmaterialflags & MATERIALFLAG_BLENDED) && !r_rtglowpass_active);
	R_SetupShader_Surface(vec3_origin, vec3_origin, vec3_origin, RSURFPASS_BASE, texturenumsurfaces, texturesurfacelist, NULL, (rsurface.texture->currentmaterialflags & MATERIALFLAG_SKY) != 0 || ui, ui);
	RSurf_DrawBatch();
}

static void R_DrawTextureSurfaceList_ShowSurfaces(int texturenumsurfaces, const msurface_t **texturesurfacelist, qbool writedepth)
{
	int vi;
	int j;
	int texturesurfaceindex;
	int k;
	const msurface_t *surface;
	float surfacecolor4f[4];
	float c[4];
	texture_t *t = rsurface.texture;

//	R_Mesh_ResetTextureState();
	R_SetupShader_Generic_NoTexture(false, false);

	GL_BlendFunc(GL_ONE, GL_ZERO);
	GL_DepthMask(writedepth);

	switch (r_showsurfaces.integer)
	{
		case 1:
		default:
			RSurf_PrepareVerticesForBatch(BATCHNEED_ARRAY_VERTEX | BATCHNEED_ARRAY_VERTEXCOLOR | BATCHNEED_ALWAYSCOPY, texturenumsurfaces, texturesurfacelist);
			vi = 0;
			for (texturesurfaceindex = 0;texturesurfaceindex < texturenumsurfaces;texturesurfaceindex++)
			{
				surface = texturesurfacelist[texturesurfaceindex];
				k = (int)(((size_t)surface) / sizeof(msurface_t));
				Vector4Set(surfacecolor4f, (k & 0xF) * (1.0f / 16.0f), (k & 0xF0) * (1.0f / 256.0f), (k & 0xF00) * (1.0f / 4096.0f), 1);
				for (j = 0;j < surface->num_vertices;j++)
				{
					Vector4Copy(surfacecolor4f, rsurface.batchlightmapcolor4f + 4 * vi);
					vi++;
				}
			}
			break;
		case 3:
			if(t && t->currentskinframe)
			{
				Vector4Copy(t->currentskinframe->avgcolor, c);
				c[3] *= t->currentalpha;
			}
			else
			{
				Vector4Set(c, 1, 0, 1, 1);
			}
			if (t && (t->pantstexture || t->shirttexture))
			{
				VectorMAM(0.7, t->render_colormap_pants, 0.3, t->render_colormap_shirt, c);
			}
			VectorScale(c, 2 * r_refdef.view.colorscale, c);
			if(t->currentmaterialflags & MATERIALFLAG_WATERALPHA)
				c[3] *= r_wateralpha.value;
			RSurf_PrepareVerticesForBatch(BATCHNEED_ARRAY_VERTEX | BATCHNEED_ARRAY_VERTEXCOLOR | BATCHNEED_ALWAYSCOPY, texturenumsurfaces, texturesurfacelist);
			vi = 0;
			if (rsurface.modellightmapcolor4f)
			{
				for (texturesurfaceindex = 0;texturesurfaceindex < texturenumsurfaces;texturesurfaceindex++)
				{
					surface = texturesurfacelist[texturesurfaceindex];
					for (j = 0;j < surface->num_vertices;j++)
					{
						float *ptr = rsurface.batchlightmapcolor4f + 4 * vi;
						Vector4Multiply(ptr, c, ptr);
						vi++;
					}
				}
			}
			else
			{
				for (texturesurfaceindex = 0;texturesurfaceindex < texturenumsurfaces;texturesurfaceindex++)
				{
					surface = texturesurfacelist[texturesurfaceindex];
					for (j = 0;j < surface->num_vertices;j++)
					{
						float *ptr = rsurface.batchlightmapcolor4f + 4 * vi;
						Vector4Copy(c, ptr);
						vi++;
					}
				}
			}
			break;
	}
	R_Mesh_PrepareVertices_Generic_Arrays(rsurface.batchnumvertices, rsurface.batchvertex3f, rsurface.batchlightmapcolor4f, rsurface.batchtexcoordtexture2f);
	RSurf_DrawBatch();
}

static void R_DrawModelTextureSurfaceList(int texturenumsurfaces, const msurface_t **texturesurfacelist, qbool writedepth, qbool prepass, qbool ui)
{
	CHECKGLERROR
	RSurf_SetupDepthAndCulling(ui);
	if (r_showsurfaces.integer && r_refdef.view.showdebug)
	{
		R_DrawTextureSurfaceList_ShowSurfaces(texturenumsurfaces, texturesurfacelist, writedepth);
		return;
	}
	// METAL.md Phase 3 slice 4: Metal falls through. Despite the name,
	// R_DrawTextureSurfaceList_GL20 contains no GL of its own -- it is the shared
	// material path, and every backend call inside it dispatches. This one stub
	// is what made the whole 2D frame come out black on the first run with the
	// door open: everything upstream worked and the draw was simply never issued.
	switch (vid.renderpath)
	{
	case RENDERPATH_METAL:
	case RENDERPATH_GL32:
	case RENDERPATH_GLES2:
		R_DrawTextureSurfaceList_GL20(texturenumsurfaces, texturesurfacelist, writedepth, prepass, ui);
		break;
	}
	CHECKGLERROR
}

static void R_DrawSurface_TransparentCallback(const entity_render_t *ent, const rtlight_t *rtlight, int numsurfaces, int *surfacelist)
{
	int i, j;
	int texturenumsurfaces, endsurface;
	texture_t *texture;
	const msurface_t *surface;
	const msurface_t *texturesurfacelist[MESHQUEUE_TRANSPARENT_BATCHSIZE];

	RSurf_ActiveModelEntity(ent, true, true, false);

	if (r_transparentdepthmasking.integer)
	{
		qbool setup = false;
		for (i = 0;i < numsurfaces;i = j)
		{
			j = i + 1;
			surface = rsurface.modelsurfaces + surfacelist[i];
			texture = surface->texture;
			rsurface.texture = R_GetCurrentTexture(texture);
			rsurface.lightmaptexture = NULL;
			rsurface.deluxemaptexture = NULL;
			rsurface.uselightmaptexture = false;
			// scan ahead until we find a different texture
			endsurface = min(i + 1024, numsurfaces);
			texturenumsurfaces = 0;
			texturesurfacelist[texturenumsurfaces++] = surface;
			for (;j < endsurface;j++)
			{
				surface = rsurface.modelsurfaces + surfacelist[j];
				if (texture != surface->texture)
					break;
				texturesurfacelist[texturenumsurfaces++] = surface;
			}
			if (!(rsurface.texture->currentmaterialflags & MATERIALFLAG_TRANSDEPTH))
				continue;
			// render the range of surfaces as depth
			if (!setup)
			{
				setup = true;
				GL_ColorMask(0,0,0,0);
				GL_Color(1,1,1,1);
				GL_DepthTest(true);
				GL_BlendFunc(GL_ONE, GL_ZERO);
				GL_DepthMask(true);
//				R_Mesh_ResetTextureState();
			}
			RSurf_SetupDepthAndCulling(false);
			RSurf_PrepareVerticesForBatch(BATCHNEED_ARRAY_VERTEX | BATCHNEED_ALLOWMULTIDRAW, texturenumsurfaces, texturesurfacelist);
			R_SetupShader_DepthOrShadow(false, false, !!rsurface.batchskeletaltransform3x4);
			R_Mesh_PrepareVertices_Vertex3f(rsurface.batchnumvertices, rsurface.batchvertex3f, rsurface.batchvertex3f_vertexbuffer, rsurface.batchvertex3f_bufferoffset);
			RSurf_DrawBatch();
		}
		if (setup)
			GL_ColorMask(r_refdef.view.colormask[0], r_refdef.view.colormask[1], r_refdef.view.colormask[2], 1);
	}

	for (i = 0;i < numsurfaces;i = j)
	{
		j = i + 1;
		surface = rsurface.modelsurfaces + surfacelist[i];
		texture = surface->texture;
		rsurface.texture = R_GetCurrentTexture(texture);
		// scan ahead until we find a different texture
		endsurface = min(i + MESHQUEUE_TRANSPARENT_BATCHSIZE, numsurfaces);
		texturenumsurfaces = 0;
		texturesurfacelist[texturenumsurfaces++] = surface;
			rsurface.lightmaptexture = surface->lightmaptexture;
			rsurface.deluxemaptexture = surface->deluxemaptexture;
			rsurface.uselightmaptexture = surface->lightmaptexture != NULL;
			for (;j < endsurface;j++)
			{
				surface = rsurface.modelsurfaces + surfacelist[j];
				if (texture != surface->texture || rsurface.lightmaptexture != surface->lightmaptexture)
					break;
				texturesurfacelist[texturenumsurfaces++] = surface;
			}
		// render the range of surfaces
		R_DrawModelTextureSurfaceList(texturenumsurfaces, texturesurfacelist, false, false, false);
	}
	rsurface.entity = NULL; // used only by R_GetCurrentTexture and RSurf_ActiveModelEntity
}

static void R_ProcessTransparentTextureSurfaceList(int texturenumsurfaces, const msurface_t **texturesurfacelist)
{
	// transparent surfaces get pushed off into the transparent queue
	int surfacelistindex;
	const msurface_t *surface;
	vec3_t tempcenter, center;
	for (surfacelistindex = 0;surfacelistindex < texturenumsurfaces;surfacelistindex++)
	{
		surface = texturesurfacelist[surfacelistindex];
		if (r_transparent_sortsurfacesbynearest.integer)
		{
			tempcenter[0] = bound(surface->mins[0], rsurface.localvieworigin[0], surface->maxs[0]);
			tempcenter[1] = bound(surface->mins[1], rsurface.localvieworigin[1], surface->maxs[1]);
			tempcenter[2] = bound(surface->mins[2], rsurface.localvieworigin[2], surface->maxs[2]);
		}
		else
		{
			tempcenter[0] = (surface->mins[0] + surface->maxs[0]) * 0.5f;
			tempcenter[1] = (surface->mins[1] + surface->maxs[1]) * 0.5f;
			tempcenter[2] = (surface->mins[2] + surface->maxs[2]) * 0.5f;
		}
		Matrix4x4_Transform(&rsurface.matrix, tempcenter, center);
		if (rsurface.entity->transparent_offset) // transparent offset
		{
			center[0] += r_refdef.view.forward[0]*rsurface.entity->transparent_offset;
			center[1] += r_refdef.view.forward[1]*rsurface.entity->transparent_offset;
			center[2] += r_refdef.view.forward[2]*rsurface.entity->transparent_offset;
		}
		R_MeshQueue_AddTransparent((rsurface.entity->flags & RENDER_WORLDOBJECT) ? TRANSPARENTSORT_SKY : (rsurface.texture->currentmaterialflags & MATERIALFLAG_NODEPTHTEST) ? TRANSPARENTSORT_HUD : rsurface.texture->transparentsort, center, R_DrawSurface_TransparentCallback, rsurface.entity, surface - rsurface.modelsurfaces, rsurface.rtlight);
	}
}

// T2b (the MetalFX-temporal arc): while this is set, the depth-only draw path
// below renders MOTION VECTORS instead of depth -- the entity's posed vertices
// through this frame's MVP and the previous frame's, writing the screen-space
// delta into the motion target. The depth-only path is reused because it is
// already exactly the walk a motion pass needs: position-only batches, the
// blended / alpha-tested / NODEPTHTEST surfaces skipped, texture batching and
// the submodel surface range handled. Only the shader and one matrix differ.

static void R_DrawTextureSurfaceList_DepthOnly(int texturenumsurfaces, const msurface_t **texturesurfacelist)
{
	if ((rsurface.texture->currentmaterialflags & (MATERIALFLAG_NODEPTHTEST | MATERIALFLAG_BLENDED | MATERIALFLAG_ALPHATEST)))
		return;
	if (r_fb.water.renderingscene && (rsurface.texture->currentmaterialflags & (MATERIALFLAG_WATERSHADER | MATERIALFLAG_REFLECTION)))
		return;
	RSurf_SetupDepthAndCulling(false);
	RSurf_PrepareVerticesForBatch(BATCHNEED_ARRAY_VERTEX | BATCHNEED_ALLOWMULTIDRAW, texturenumsurfaces, texturesurfacelist);
	R_Mesh_PrepareVertices_Vertex3f(rsurface.batchnumvertices, rsurface.batchvertex3f, rsurface.batchvertex3f_vertexbuffer, rsurface.batchvertex3f_bufferoffset);
	if (r_motionpass_active)
	{
		// The motion target has no depth attachment, so the real depth test is
		// off (GL_DepthTest false from the 2D reset) and the shader does it by
		// hand against the sampled scene depth. Blend is a straight overwrite.
		matrix4x4_t prevmv, prevmvp;
		float m16f[16];
		int loc;
		GL_DepthTest(false);
		GL_DepthMask(false);
		GL_BlendFunc(GL_ONE, GL_ZERO);
		R_SetupShader_SetPermutationGLSL(SHADERMODE_MOTIONVECTOR, 0);
		if (r_glsl_permutation->tex_Texture_ScreenDepth >= 0)
			R_Mesh_TexBind(r_glsl_permutation->tex_Texture_ScreenDepth, r_fb.scenedepthtexture);
		// previous MVP = projection * previous view * previous model. The
		// projection is THIS frame's (it carries the jitter and the Metal
		// remap, and the current MVP the shader compares against carries the
		// same, so the two sides agree); the view and model are last frame's.
		Matrix4x4_Concat(&prevmv, &r_fb.taa_prevview, &r_motionpass_prevmodel);
		Matrix4x4_Concat(&prevmvp, &r_refdef.view.viewport.projectmatrix, &prevmv);
		Matrix4x4_ToArrayFloatGL(&prevmvp, m16f);
		if ((loc = R_Shader_GetUniformLocation(r_glsl_permutation, "MotionPrevMVP")) >= 0) R_Shader_UniformMatrix4fv(loc, 1, false, m16f);
		if ((loc = R_Shader_GetUniformLocation(r_glsl_permutation, "MotionParams")) >= 0)
			R_Shader_Uniform4f(loc, 0.0f, 0.0f,
			                   ((r_metalfx_signs.integer & 4) ? -1.0f : 1.0f) * (float)r_fb.rt_screen->texturewidth,
			                   ((r_metalfx_signs.integer & 8) ? -1.0f : 1.0f) * (float)r_fb.rt_screen->textureheight);
		if ((loc = R_Shader_GetUniformLocation(r_glsl_permutation, "MotionDebug")) >= 0) R_Shader_Uniform1f(loc, r_motionpass_debug);
		if ((loc = R_Shader_GetUniformLocation(r_glsl_permutation, "MotionDepthEps")) >= 0) R_Shader_Uniform1f(loc, 1.0e-5f);
		RSurf_DrawBatch();
		return;
	}
	R_SetupShader_DepthOrShadow(false, false, !!rsurface.batchskeletaltransform3x4);
	RSurf_DrawBatch();
}

static void R_ProcessModelTextureSurfaceList(int texturenumsurfaces, const msurface_t **texturesurfacelist, qbool writedepth, qbool depthonly, qbool prepass, qbool ui)
{
	CHECKGLERROR
	// GLOW RE-ADD PASS: transparents were already queued by the scene pass and
	// must NEVER be queued twice (they would draw twice); sky has no emission;
	// and the waterplane classes (r_water reflections/refraction/camera) render
	// whole captured images, which an additive re-draw would add a second time.
	if (r_rtglowpass_active && (rsurface.texture->currentmaterialflags & (MATERIALFLAGMASK_DEPTHSORTED | MATERIALFLAG_SKY | MATERIALFLAG_WATERSHADER | MATERIALFLAG_REFRACTION | MATERIALFLAG_REFLECTION | MATERIALFLAG_CAMERA)))
		return;
	if (ui)
		R_DrawModelTextureSurfaceList(texturenumsurfaces, texturesurfacelist, writedepth, prepass, ui);
	else if (depthonly)
		R_DrawTextureSurfaceList_DepthOnly(texturenumsurfaces, texturesurfacelist);
	else if (prepass)
	{
		if (!(rsurface.texture->currentmaterialflags & MATERIALFLAG_WALL))
			return;
		if (rsurface.texture->currentmaterialflags & MATERIALFLAGMASK_DEPTHSORTED)
			R_ProcessTransparentTextureSurfaceList(texturenumsurfaces, texturesurfacelist);
		else
			R_DrawModelTextureSurfaceList(texturenumsurfaces, texturesurfacelist, writedepth, prepass, ui);
	}
	else if ((rsurface.texture->currentmaterialflags & MATERIALFLAG_SKY) && (!r_showsurfaces.integer || r_showsurfaces.integer == 3))
		R_DrawTextureSurfaceList_Sky(texturenumsurfaces, texturesurfacelist);
	else if (!(rsurface.texture->currentmaterialflags & MATERIALFLAG_WALL))
		return;
	else if (((rsurface.texture->currentmaterialflags & MATERIALFLAGMASK_DEPTHSORTED) || (r_showsurfaces.integer == 3 && (rsurface.texture->currentmaterialflags & MATERIALFLAG_ALPHATEST))))
	{
		// in the deferred case, transparent surfaces were queued during prepass
		if (!r_shadow_usingdeferredprepass)
			R_ProcessTransparentTextureSurfaceList(texturenumsurfaces, texturesurfacelist);
	}
	else
	{
		// the alphatest check is to make sure we write depth for anything we skipped on the depth-only pass earlier
		R_DrawModelTextureSurfaceList(texturenumsurfaces, texturesurfacelist, writedepth || (rsurface.texture->currentmaterialflags & MATERIALFLAG_ALPHATEST), prepass, ui);
	}
	CHECKGLERROR
}

static void R_QueueModelSurfaceList(entity_render_t *ent, int numsurfaces, const msurface_t **surfacelist, int flagsmask, qbool writedepth, qbool depthonly, qbool prepass, qbool ui)
{
	int i, j;
	texture_t *texture;
	R_FrameData_SetMark();
	// break the surface list down into batches by texture and use of lightmapping
	for (i = 0;i < numsurfaces;i = j)
	{
		j = i + 1;
		// texture is the base texture pointer, rsurface.texture is the
		// current frame/skin the texture is directing us to use (for example
		// if a model has 2 skins and it is on skin 1, then skin 0 tells us to
		// use skin 1 instead)
		texture = surfacelist[i]->texture;
		rsurface.texture = R_GetCurrentTexture(texture);
		if (!(rsurface.texture->currentmaterialflags & flagsmask) || (rsurface.texture->currentmaterialflags & MATERIALFLAG_NODRAW))
		{
			// if this texture is not the kind we want, skip ahead to the next one
			for (;j < numsurfaces && texture == surfacelist[j]->texture;j++)
				;
			continue;
		}
		// GLOW RE-ADD PASS (rt_metal_glowpass): nothing on this batch can emit --
		// no glow layer, and redglow dead for this entity -- so skip the fill
		if (r_rtglowpass_active
		 && !rsurface.texture->glowtexture && !rsurface.texture->backgroundglowtexture
		 && !r_rtglowpass_redglow)
		{
			for (;j < numsurfaces && texture == surfacelist[j]->texture;j++)
				;
			continue;
		}
		if(depthonly || prepass)
		{
			rsurface.lightmaptexture = NULL;
			rsurface.deluxemaptexture = NULL;
			rsurface.uselightmaptexture = false;
			// simply scan ahead until we find a different texture or lightmap state
			for (;j < numsurfaces && texture == surfacelist[j]->texture;j++)
				;
		}
		else
		{
			rsurface.lightmaptexture = surfacelist[i]->lightmaptexture;
			rsurface.deluxemaptexture = surfacelist[i]->deluxemaptexture;
			rsurface.uselightmaptexture = surfacelist[i]->lightmaptexture != NULL;
			// simply scan ahead until we find a different texture or lightmap state
			for (;j < numsurfaces && texture == surfacelist[j]->texture && rsurface.lightmaptexture == surfacelist[j]->lightmaptexture;j++)
				;
		}
		// render the range of surfaces
		R_ProcessModelTextureSurfaceList(j - i, surfacelist + i, writedepth, depthonly, prepass, ui);
	}
	R_FrameData_ReturnToMark();
}

float locboxvertex3f[6*4*3] =
{
	1,0,1, 1,0,0, 1,1,0, 1,1,1,
	0,1,1, 0,1,0, 0,0,0, 0,0,1,
	1,1,1, 1,1,0, 0,1,0, 0,1,1,
	0,0,1, 0,0,0, 1,0,0, 1,0,1,
	0,0,1, 1,0,1, 1,1,1, 0,1,1,
	1,0,0, 0,0,0, 0,1,0, 1,1,0
};

unsigned short locboxelements[6*2*3] =
{
	 0, 1, 2, 0, 2, 3,
	 4, 5, 6, 4, 6, 7,
	 8, 9,10, 8,10,11,
	12,13,14, 12,14,15,
	16,17,18, 16,18,19,
	20,21,22, 20,22,23
};

static void R_DrawLoc_Callback(const entity_render_t *ent, const rtlight_t *rtlight, int numsurfaces, int *surfacelist)
{
	int i, j;
	cl_locnode_t *loc = (cl_locnode_t *)ent;
	vec3_t mins, size;
	float vertex3f[6*4*3];
	CHECKGLERROR
	GL_BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	GL_DepthMask(false);
	GL_DepthRange(0, 1);
	GL_PolygonOffset(r_refdef.polygonfactor, r_refdef.polygonoffset);
	GL_DepthTest(true);
	GL_CullFace(GL_NONE);
	R_EntityMatrix(&identitymatrix);

//	R_Mesh_ResetTextureState();

	i = surfacelist[0];
	GL_Color(((i & 0x0007) >> 0) * (1.0f / 7.0f) * r_refdef.view.colorscale,
			 ((i & 0x0038) >> 3) * (1.0f / 7.0f) * r_refdef.view.colorscale,
			 ((i & 0x01C0) >> 6) * (1.0f / 7.0f) * r_refdef.view.colorscale,
			surfacelist[0] < 0 ? 0.5f : 0.125f);

	if (VectorCompare(loc->mins, loc->maxs))
	{
		VectorSet(size, 2, 2, 2);
		VectorMA(loc->mins, -0.5f, size, mins);
	}
	else
	{
		VectorCopy(loc->mins, mins);
		VectorSubtract(loc->maxs, loc->mins, size);
	}

	for (i = 0;i < 6*4*3;)
		for (j = 0;j < 3;j++, i++)
			vertex3f[i] = mins[j] + size[j] * locboxvertex3f[i];

	R_Mesh_PrepareVertices_Generic_Arrays(6*4, vertex3f, NULL, NULL);
	R_SetupShader_Generic_NoTexture(false, false);
	R_Mesh_Draw(0, 6*4, 0, 6*2, NULL, NULL, 0, locboxelements, NULL, 0);
}

void R_DrawLocs(void)
{
	int index;
	cl_locnode_t *loc, *nearestloc;
	vec3_t center;
	nearestloc = CL_Locs_FindNearest(cl.movement_origin);
	for (loc = cl.locnodes, index = 0;loc;loc = loc->next, index++)
	{
		VectorLerp(loc->mins, 0.5f, loc->maxs, center);
		R_MeshQueue_AddTransparent(TRANSPARENTSORT_DISTANCE, center, R_DrawLoc_Callback, (entity_render_t *)loc, loc == nearestloc ? -1 : index, NULL);
	}
}

void R_DecalSystem_Reset(decalsystem_t *decalsystem)
{
	if (decalsystem->decals)
		Mem_Free(decalsystem->decals);
	memset(decalsystem, 0, sizeof(*decalsystem));
}

static void R_DecalSystem_SpawnTriangle(decalsystem_t *decalsystem, const float *v0, const float *v1, const float *v2, const float *t0, const float *t1, const float *t2, const float *c0, const float *c1, const float *c2, int triangleindex, int surfaceindex, unsigned int decalsequence)
{
	tridecal_t *decal;
	int i;

	// expand or initialize the system
	if (decalsystem->maxdecals <= decalsystem->numdecals)
	{
		decalsystem_t old = *decalsystem;
		qbool useshortelements;
		decalsystem->maxdecals = max(16, decalsystem->maxdecals * 2);
		useshortelements = decalsystem->maxdecals * 3 <= 65536;
		decalsystem->decals = (tridecal_t *)Mem_Alloc(cls.levelmempool, decalsystem->maxdecals * (sizeof(tridecal_t) + sizeof(float[3][3]) + sizeof(float[3][2]) + sizeof(float[3][4]) + sizeof(int[3]) + (useshortelements ? sizeof(unsigned short[3]) : 0)));
		decalsystem->color4f = (float *)(decalsystem->decals + decalsystem->maxdecals);
		decalsystem->texcoord2f = (float *)(decalsystem->color4f + decalsystem->maxdecals*12);
		decalsystem->vertex3f = (float *)(decalsystem->texcoord2f + decalsystem->maxdecals*6);
		decalsystem->element3i = (int *)(decalsystem->vertex3f + decalsystem->maxdecals*9);
		decalsystem->element3s = (useshortelements ? ((unsigned short *)(decalsystem->element3i + decalsystem->maxdecals*3)) : NULL);
		if (decalsystem->numdecals)
			memcpy(decalsystem->decals, old.decals, decalsystem->numdecals * sizeof(tridecal_t));
		if (old.decals)
			Mem_Free(old.decals);
		for (i = 0;i < decalsystem->maxdecals*3;i++)
			decalsystem->element3i[i] = i;
		if (useshortelements)
			for (i = 0;i < decalsystem->maxdecals*3;i++)
				decalsystem->element3s[i] = i;
	}

	// grab a decal and search for another free slot for the next one
	decal = &decalsystem->decals[decalsystem->numdecals++];

	// initialize the decal
	decal->lived = 0;
	decal->triangleindex = triangleindex;
	decal->surfaceindex = surfaceindex;
	decal->decalsequence = decalsequence;
	decal->color4f[0][0] = c0[0];
	decal->color4f[0][1] = c0[1];
	decal->color4f[0][2] = c0[2];
	decal->color4f[0][3] = 1;
	decal->color4f[1][0] = c1[0];
	decal->color4f[1][1] = c1[1];
	decal->color4f[1][2] = c1[2];
	decal->color4f[1][3] = 1;
	decal->color4f[2][0] = c2[0];
	decal->color4f[2][1] = c2[1];
	decal->color4f[2][2] = c2[2];
	decal->color4f[2][3] = 1;
	decal->vertex3f[0][0] = v0[0];
	decal->vertex3f[0][1] = v0[1];
	decal->vertex3f[0][2] = v0[2];
	decal->vertex3f[1][0] = v1[0];
	decal->vertex3f[1][1] = v1[1];
	decal->vertex3f[1][2] = v1[2];
	decal->vertex3f[2][0] = v2[0];
	decal->vertex3f[2][1] = v2[1];
	decal->vertex3f[2][2] = v2[2];
	decal->texcoord2f[0][0] = t0[0];
	decal->texcoord2f[0][1] = t0[1];
	decal->texcoord2f[1][0] = t1[0];
	decal->texcoord2f[1][1] = t1[1];
	decal->texcoord2f[2][0] = t2[0];
	decal->texcoord2f[2][1] = t2[1];
	TriangleNormal(v0, v1, v2, decal->plane);
	VectorNormalize(decal->plane);
	decal->plane[3] = DotProduct(v0, decal->plane);
}

extern cvar_t cl_decals_bias;
extern cvar_t cl_decals_models;
extern cvar_t cl_decals_newsystem_intensitymultiplier;
// baseparms, parms, temps
static void R_DecalSystem_SplatTriangle(decalsystem_t *decalsystem, float r, float g, float b, float a, float s1, float t1, float s2, float t2, unsigned int decalsequence, qbool dynamic, float (*planes)[4], matrix4x4_t *projection, int triangleindex, int surfaceindex)
{
	int cornerindex;
	int index;
	float v[9][3];
	const float *vertex3f;
	const float *normal3f;
	int numpoints;
	float points[2][9][3];
	float temp[3];
	float tc[9][2];
	float f;
	float c[9][4];
	const int *e;

	e = rsurface.modelelement3i + 3*triangleindex;

	vertex3f = rsurface.modelvertex3f;
	normal3f = rsurface.modelnormal3f;

	if (normal3f)
	{
		for (cornerindex = 0;cornerindex < 3;cornerindex++)
		{
			index = 3*e[cornerindex];
			VectorMA(vertex3f + index, cl_decals_bias.value, normal3f + index, v[cornerindex]);
		}
	}
	else
	{
		for (cornerindex = 0;cornerindex < 3;cornerindex++)
		{
			index = 3*e[cornerindex];
			VectorCopy(vertex3f + index, v[cornerindex]);
		}
	}

	// cull backfaces
	//TriangleNormal(v[0], v[1], v[2], normal);
	//if (DotProduct(normal, localnormal) < 0.0f)
	//	continue;
	// clip by each of the box planes formed from the projection matrix
	// if anything survives, we emit the decal
	numpoints = PolygonF_Clip(3        , v[0]        , planes[0][0], planes[0][1], planes[0][2], planes[0][3], 1.0f/64.0f, sizeof(points[0])/sizeof(points[0][0]), points[1][0]);
	if (numpoints < 3)
		return;
	numpoints = PolygonF_Clip(numpoints, points[1][0], planes[1][0], planes[1][1], planes[1][2], planes[1][3], 1.0f/64.0f, sizeof(points[0])/sizeof(points[0][0]), points[0][0]);
	if (numpoints < 3)
		return;
	numpoints = PolygonF_Clip(numpoints, points[0][0], planes[2][0], planes[2][1], planes[2][2], planes[2][3], 1.0f/64.0f, sizeof(points[0])/sizeof(points[0][0]), points[1][0]);
	if (numpoints < 3)
		return;
	numpoints = PolygonF_Clip(numpoints, points[1][0], planes[3][0], planes[3][1], planes[3][2], planes[3][3], 1.0f/64.0f, sizeof(points[0])/sizeof(points[0][0]), points[0][0]);
	if (numpoints < 3)
		return;
	numpoints = PolygonF_Clip(numpoints, points[0][0], planes[4][0], planes[4][1], planes[4][2], planes[4][3], 1.0f/64.0f, sizeof(points[0])/sizeof(points[0][0]), points[1][0]);
	if (numpoints < 3)
		return;
	numpoints = PolygonF_Clip(numpoints, points[1][0], planes[5][0], planes[5][1], planes[5][2], planes[5][3], 1.0f/64.0f, sizeof(points[0])/sizeof(points[0][0]), v[0]);
	if (numpoints < 3)
		return;
	// some part of the triangle survived, so we have to accept it...
	if (dynamic)
	{
		// dynamic always uses the original triangle
		numpoints = 3;
		for (cornerindex = 0;cornerindex < 3;cornerindex++)
		{
			index = 3*e[cornerindex];
			VectorCopy(vertex3f + index, v[cornerindex]);
		}
	}
	for (cornerindex = 0;cornerindex < numpoints;cornerindex++)
	{
		// convert vertex positions to texcoords
		Matrix4x4_Transform(projection, v[cornerindex], temp);
		tc[cornerindex][0] = (temp[1]+1.0f)*0.5f * (s2-s1) + s1;
		tc[cornerindex][1] = (temp[2]+1.0f)*0.5f * (t2-t1) + t1;
		// calculate distance fade from the projection origin
		f = a * (1.0f-fabs(temp[0])) * cl_decals_newsystem_intensitymultiplier.value;
		f = bound(0.0f, f, 1.0f);
		c[cornerindex][0] = r * f;
		c[cornerindex][1] = g * f;
		c[cornerindex][2] = b * f;
		c[cornerindex][3] = 1.0f;
		//VectorMA(v[cornerindex], cl_decals_bias.value, localnormal, v[cornerindex]);
	}
	if (dynamic)
		R_DecalSystem_SpawnTriangle(decalsystem, v[0], v[1], v[2], tc[0], tc[1], tc[2], c[0], c[1], c[2], triangleindex, surfaceindex, decalsequence);
	else
		for (cornerindex = 0;cornerindex < numpoints-2;cornerindex++)
			R_DecalSystem_SpawnTriangle(decalsystem, v[0], v[cornerindex+1], v[cornerindex+2], tc[0], tc[cornerindex+1], tc[cornerindex+2], c[0], c[cornerindex+1], c[cornerindex+2], -1, surfaceindex, decalsequence);
}
static void R_DecalSystem_SplatEntity(entity_render_t *ent, const vec3_t worldorigin, const vec3_t worldnormal, float r, float g, float b, float a, float s1, float t1, float s2, float t2, float worldsize, unsigned int decalsequence)
{
	matrix4x4_t projection;
	decalsystem_t *decalsystem;
	qbool dynamic;
	model_t *model;
	const msurface_t *surface;
	const msurface_t *surfaces;
	const texture_t *texture;
	int numtriangles;
	int surfaceindex;
	int triangleindex;
	float localorigin[3];
	float localnormal[3];
	float localmins[3];
	float localmaxs[3];
	float localsize;
	//float normal[3];
	float planes[6][4];
	float angles[3];
	bih_t *bih;
	int bih_triangles_count;
	int bih_triangles[256];
	int bih_surfaces[256];

	decalsystem = &ent->decalsystem;
	model = ent->model;
	if (!model || !ent->allowdecals || ent->alpha < 1 || (ent->flags & (RENDER_ADDITIVE | RENDER_NODEPTHTEST)))
	{
		R_DecalSystem_Reset(&ent->decalsystem);
		return;
	}

	if (!model->brush.data_leafs && !cl_decals_models.integer)
	{
		if (decalsystem->model)
			R_DecalSystem_Reset(decalsystem);
		return;
	}

	if (decalsystem->model != model)
		R_DecalSystem_Reset(decalsystem);
	decalsystem->model = model;

	RSurf_ActiveModelEntity(ent, true, false, false);

	Matrix4x4_Transform(&rsurface.inversematrix, worldorigin, localorigin);
	Matrix4x4_Transform3x3(&rsurface.inversematrix, worldnormal, localnormal);
	VectorNormalize(localnormal);
	localsize = worldsize*rsurface.inversematrixscale;
	localmins[0] = localorigin[0] - localsize;
	localmins[1] = localorigin[1] - localsize;
	localmins[2] = localorigin[2] - localsize;
	localmaxs[0] = localorigin[0] + localsize;
	localmaxs[1] = localorigin[1] + localsize;
	localmaxs[2] = localorigin[2] + localsize;

	//VectorCopy(localnormal, planes[4]);
	//VectorVectors(planes[4], planes[2], planes[0]);
	AnglesFromVectors(angles, localnormal, NULL, false);
	AngleVectors(angles, planes[0], planes[2], planes[4]);
	VectorNegate(planes[0], planes[1]);
	VectorNegate(planes[2], planes[3]);
	VectorNegate(planes[4], planes[5]);
	planes[0][3] = DotProduct(planes[0], localorigin) - localsize;
	planes[1][3] = DotProduct(planes[1], localorigin) - localsize;
	planes[2][3] = DotProduct(planes[2], localorigin) - localsize;
	planes[3][3] = DotProduct(planes[3], localorigin) - localsize;
	planes[4][3] = DotProduct(planes[4], localorigin) - localsize;
	planes[5][3] = DotProduct(planes[5], localorigin) - localsize;

#if 1
// works
{
	matrix4x4_t forwardprojection;
	Matrix4x4_CreateFromQuakeEntity(&forwardprojection, localorigin[0], localorigin[1], localorigin[2], angles[0], angles[1], angles[2], localsize);
	Matrix4x4_Invert_Simple(&projection, &forwardprojection);
}
#else
// broken
{
	float projectionvector[4][3];
	VectorScale(planes[0], ilocalsize, projectionvector[0]);
	VectorScale(planes[2], ilocalsize, projectionvector[1]);
	VectorScale(planes[4], ilocalsize, projectionvector[2]);
	projectionvector[0][0] = planes[0][0] * ilocalsize;
	projectionvector[0][1] = planes[1][0] * ilocalsize;
	projectionvector[0][2] = planes[2][0] * ilocalsize;
	projectionvector[1][0] = planes[0][1] * ilocalsize;
	projectionvector[1][1] = planes[1][1] * ilocalsize;
	projectionvector[1][2] = planes[2][1] * ilocalsize;
	projectionvector[2][0] = planes[0][2] * ilocalsize;
	projectionvector[2][1] = planes[1][2] * ilocalsize;
	projectionvector[2][2] = planes[2][2] * ilocalsize;
	projectionvector[3][0] = -(localorigin[0]*projectionvector[0][0]+localorigin[1]*projectionvector[1][0]+localorigin[2]*projectionvector[2][0]);
	projectionvector[3][1] = -(localorigin[0]*projectionvector[0][1]+localorigin[1]*projectionvector[1][1]+localorigin[2]*projectionvector[2][1]);
	projectionvector[3][2] = -(localorigin[0]*projectionvector[0][2]+localorigin[1]*projectionvector[1][2]+localorigin[2]*projectionvector[2][2]);
	Matrix4x4_FromVectors(&projection, projectionvector[0], projectionvector[1], projectionvector[2], projectionvector[3]);
}
#endif

	dynamic = model->surfmesh.isanimated;
	surfaces = model->data_surfaces;

	bih = NULL;
	bih_triangles_count = -1;
	if(!dynamic)
	{
		if(model->render_bih.numleafs)
			bih = &model->render_bih;
		else if(model->collision_bih.numleafs)
			bih = &model->collision_bih;
	}
	if(bih)
		bih_triangles_count = BIH_GetTriangleListForBox(bih, sizeof(bih_triangles) / sizeof(*bih_triangles), bih_triangles, bih_surfaces, localmins, localmaxs);
	if(bih_triangles_count == 0)
		return;
	if(bih_triangles_count > (int) (sizeof(bih_triangles) / sizeof(*bih_triangles))) // hit too many, likely bad anyway
		return;
	if(bih_triangles_count > 0)
	{
		for (triangleindex = 0; triangleindex < bih_triangles_count; ++triangleindex)
		{
			surfaceindex = bih_surfaces[triangleindex];
			surface = surfaces + surfaceindex;
			texture = surface->texture;
			if (!texture)
				continue;
			if (texture->currentmaterialflags & (MATERIALFLAG_BLENDED | MATERIALFLAG_NODEPTHTEST | MATERIALFLAG_SKY | MATERIALFLAG_SHORTDEPTHRANGE | MATERIALFLAG_WATERSHADER | MATERIALFLAG_REFRACTION))
				continue;
			if (texture->surfaceflags & Q3SURFACEFLAG_NOMARKS)
				continue;
			R_DecalSystem_SplatTriangle(decalsystem, r, g, b, a, s1, t1, s2, t2, decalsequence, dynamic, planes, &projection, bih_triangles[triangleindex], surfaceindex);
		}
	}
	else
	{
		for (surfaceindex = model->submodelsurfaces_start;surfaceindex < model->submodelsurfaces_end;surfaceindex++)
		{
			surface = surfaces + surfaceindex;
			// check cull box first because it rejects more than any other check
			if (!dynamic && !BoxesOverlap(surface->mins, surface->maxs, localmins, localmaxs))
				continue;
			// skip transparent surfaces
			texture = surface->texture;
			if (!texture)
				continue;
			if (texture->currentmaterialflags & (MATERIALFLAG_BLENDED | MATERIALFLAG_NODEPTHTEST | MATERIALFLAG_SKY | MATERIALFLAG_SHORTDEPTHRANGE | MATERIALFLAG_WATERSHADER | MATERIALFLAG_REFRACTION))
				continue;
			if (texture->surfaceflags & Q3SURFACEFLAG_NOMARKS)
				continue;
			numtriangles = surface->num_triangles;
			for (triangleindex = 0; triangleindex < numtriangles; triangleindex++)
				R_DecalSystem_SplatTriangle(decalsystem, r, g, b, a, s1, t1, s2, t2, decalsequence, dynamic, planes, &projection, triangleindex + surface->num_firsttriangle, surfaceindex);
		}
	}
}

// do not call this outside of rendering code - use R_DecalSystem_SplatEntities instead
static void R_DecalSystem_ApplySplatEntities(const vec3_t worldorigin, const vec3_t worldnormal, float r, float g, float b, float a, float s1, float t1, float s2, float t2, float worldsize, unsigned int decalsequence)
{
	int renderentityindex;
	float worldmins[3];
	float worldmaxs[3];
	entity_render_t *ent;

	worldmins[0] = worldorigin[0] - worldsize;
	worldmins[1] = worldorigin[1] - worldsize;
	worldmins[2] = worldorigin[2] - worldsize;
	worldmaxs[0] = worldorigin[0] + worldsize;
	worldmaxs[1] = worldorigin[1] + worldsize;
	worldmaxs[2] = worldorigin[2] + worldsize;

	R_DecalSystem_SplatEntity(r_refdef.scene.worldentity, worldorigin, worldnormal, r, g, b, a, s1, t1, s2, t2, worldsize, decalsequence);

	for (renderentityindex = 0;renderentityindex < r_refdef.scene.numentities;renderentityindex++)
	{
		ent = r_refdef.scene.entities[renderentityindex];
		if (!BoxesOverlap(ent->mins, ent->maxs, worldmins, worldmaxs))
			continue;

		R_DecalSystem_SplatEntity(ent, worldorigin, worldnormal, r, g, b, a, s1, t1, s2, t2, worldsize, decalsequence);
	}
}

typedef struct r_decalsystem_splatqueue_s
{
	vec3_t worldorigin;
	vec3_t worldnormal;
	float color[4];
	float tcrange[4];
	float worldsize;
	unsigned int decalsequence;
}
r_decalsystem_splatqueue_t;

int r_decalsystem_numqueued = 0;
r_decalsystem_splatqueue_t r_decalsystem_queue[MAX_DECALSYSTEM_QUEUE];

void R_DecalSystem_SplatEntities(const vec3_t worldorigin, const vec3_t worldnormal, float r, float g, float b, float a, float s1, float t1, float s2, float t2, float worldsize)
{
	r_decalsystem_splatqueue_t *queue;

	if (r_decalsystem_numqueued == MAX_DECALSYSTEM_QUEUE)
		return;

	queue = &r_decalsystem_queue[r_decalsystem_numqueued++];
	VectorCopy(worldorigin, queue->worldorigin);
	VectorCopy(worldnormal, queue->worldnormal);
	Vector4Set(queue->color, r, g, b, a);
	Vector4Set(queue->tcrange, s1, t1, s2, t2);
	queue->worldsize = worldsize;
	queue->decalsequence = cl.decalsequence++;
}

static void R_DecalSystem_ApplySplatEntitiesQueue(void)
{
	int i;
	r_decalsystem_splatqueue_t *queue;

	for (i = 0, queue = r_decalsystem_queue;i < r_decalsystem_numqueued;i++, queue++)
		R_DecalSystem_ApplySplatEntities(queue->worldorigin, queue->worldnormal, queue->color[0], queue->color[1], queue->color[2], queue->color[3], queue->tcrange[0], queue->tcrange[1], queue->tcrange[2], queue->tcrange[3], queue->worldsize, queue->decalsequence);
	r_decalsystem_numqueued = 0;
}

extern cvar_t cl_decals_max;
static void R_DrawModelDecals_FadeEntity(entity_render_t *ent)
{
	int i;
	decalsystem_t *decalsystem = &ent->decalsystem;
	unsigned int killsequence;
	tridecal_t *decal;
	float frametime;
	float lifetime;

	if (!decalsystem->numdecals)
		return;

	if (r_showsurfaces.integer)
		return;

	if (ent->model != decalsystem->model || ent->alpha < 1 || (ent->flags & RENDER_ADDITIVE))
	{
		R_DecalSystem_Reset(decalsystem);
		return;
	}

	killsequence = cl.decalsequence - bound(1, (unsigned int) cl_decals_max.integer, cl.decalsequence);
	lifetime = cl_decals_time.value + cl_decals_fadetime.value;

	if (decalsystem->lastupdatetime)
		frametime = (r_refdef.scene.time - decalsystem->lastupdatetime);
	else
		frametime = 0;
	decalsystem->lastupdatetime = r_refdef.scene.time;

	for (i = 0, decal = decalsystem->decals;i < decalsystem->numdecals;i++, decal++)
	{
		decal->lived += frametime;
		if (killsequence > decal->decalsequence || decal->lived >= lifetime)
		{
			*decal = decalsystem->decals[--decalsystem->numdecals];
			--i, --decal;  // Consider the just moved decal next.
		}
	}

	if (decalsystem->numdecals <= 0)
	{
		// if there are no decals left, reset decalsystem
		R_DecalSystem_Reset(decalsystem);
	}
}

extern skinframe_t *decalskinframe;
static void R_DrawModelDecals_Entity(entity_render_t *ent)
{
	int i;
	decalsystem_t *decalsystem = &ent->decalsystem;
	int numdecals;
	tridecal_t *decal;
	float faderate;
	float alpha;
	float *v3f;
	float *c4f;
	float *t2f;
	const int *e;
	const unsigned char *surfacevisible = ent == r_refdef.scene.worldentity ? r_refdef.viewcache.world_surfacevisible : NULL;
	int numtris = 0;

	numdecals = decalsystem->numdecals;
	if (!numdecals)
		return;

	if (r_showsurfaces.integer)
		return;

	if (ent->model != decalsystem->model || ent->alpha < 1 || (ent->flags & RENDER_ADDITIVE))
	{
		R_DecalSystem_Reset(decalsystem);
		return;
	}

	// if the model is static it doesn't matter what value we give for
	// wantnormals and wanttangents, so this logic uses only rules applicable
	// to a model, knowing that they are meaningless otherwise
	RSurf_ActiveModelEntity(ent, false, false, false);

	decalsystem->lastupdatetime = r_refdef.scene.time;

	faderate = 1.0f / max(0.001f, cl_decals_fadetime.value);

	// update vertex positions for animated models
	v3f = decalsystem->vertex3f;
	c4f = decalsystem->color4f;
	t2f = decalsystem->texcoord2f;
	for (i = 0, decal = decalsystem->decals;i < numdecals;i++, decal++)
	{
		if (!decal->color4f[0][3])
			continue;

		if (surfacevisible && !surfacevisible[decal->surfaceindex])
			continue;

		// skip backfaces
		if (decal->triangleindex < 0 && DotProduct(r_refdef.view.origin, decal->plane) < decal->plane[3])
			continue;

		// update color values for fading decals
		if (decal->lived >= cl_decals_time.value)
			alpha = 1 - faderate * (decal->lived - cl_decals_time.value);
		else
			alpha = 1.0f;

		c4f[ 0] = decal->color4f[0][0] * alpha;
		c4f[ 1] = decal->color4f[0][1] * alpha;
		c4f[ 2] = decal->color4f[0][2] * alpha;
		c4f[ 3] = 1;
		c4f[ 4] = decal->color4f[1][0] * alpha;
		c4f[ 5] = decal->color4f[1][1] * alpha;
		c4f[ 6] = decal->color4f[1][2] * alpha;
		c4f[ 7] = 1;
		c4f[ 8] = decal->color4f[2][0] * alpha;
		c4f[ 9] = decal->color4f[2][1] * alpha;
		c4f[10] = decal->color4f[2][2] * alpha;
		c4f[11] = 1;

		t2f[0] = decal->texcoord2f[0][0];
		t2f[1] = decal->texcoord2f[0][1];
		t2f[2] = decal->texcoord2f[1][0];
		t2f[3] = decal->texcoord2f[1][1];
		t2f[4] = decal->texcoord2f[2][0];
		t2f[5] = decal->texcoord2f[2][1];

		// update vertex positions for animated models
		if (decal->triangleindex >= 0 && decal->triangleindex < rsurface.modelnumtriangles)
		{
			e = rsurface.modelelement3i + 3*decal->triangleindex;
			VectorCopy(rsurface.modelvertex3f + 3*e[0], v3f);
			VectorCopy(rsurface.modelvertex3f + 3*e[1], v3f + 3);
			VectorCopy(rsurface.modelvertex3f + 3*e[2], v3f + 6);
		}
		else
		{
			VectorCopy(decal->vertex3f[0], v3f);
			VectorCopy(decal->vertex3f[1], v3f + 3);
			VectorCopy(decal->vertex3f[2], v3f + 6);
		}

		if (r_refdef.fogenabled)
		{
			alpha = RSurf_FogVertex(v3f);
			VectorScale(c4f, alpha, c4f);
			alpha = RSurf_FogVertex(v3f + 3);
			VectorScale(c4f + 4, alpha, c4f + 4);
			alpha = RSurf_FogVertex(v3f + 6);
			VectorScale(c4f + 8, alpha, c4f + 8);
		}

		v3f += 9;
		c4f += 12;
		t2f += 6;
		numtris++;
	}

	if (numtris > 0)
	{
		r_refdef.stats[r_stat_drawndecals] += numtris;

		// now render the decals all at once
		// (this assumes they all use one particle font texture!)
		RSurf_ActiveCustomEntity(&rsurface.matrix, &rsurface.inversematrix, rsurface.ent_flags, ent->shadertime, 1, 1, 1, 1, numdecals*3, decalsystem->vertex3f, decalsystem->texcoord2f, NULL, NULL, NULL, decalsystem->color4f, numtris, decalsystem->element3i, decalsystem->element3s, false, false);
//		R_Mesh_ResetTextureState();
		R_Mesh_PrepareVertices_Generic_Arrays(numtris * 3, decalsystem->vertex3f, decalsystem->color4f, decalsystem->texcoord2f);
		GL_DepthMask(false);
		GL_DepthRange(0, 1);
		GL_PolygonOffset(rsurface.basepolygonfactor + r_polygonoffset_decals_factor.value, rsurface.basepolygonoffset + r_polygonoffset_decals_offset.value);
		GL_DepthTest(true);
		GL_CullFace(GL_NONE);
		GL_BlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
		R_SetupShader_Generic(decalskinframe->base, false, false, false);
		R_Mesh_Draw(0, numtris * 3, 0, numtris, decalsystem->element3i, NULL, 0, decalsystem->element3s, NULL, 0);
	}
}

static void R_DrawModelDecals(void)
{
	int i, numdecals;

	// fade faster when there are too many decals
	numdecals = r_refdef.scene.worldentity->decalsystem.numdecals;
	for (i = 0;i < r_refdef.scene.numentities;i++)
		numdecals += r_refdef.scene.entities[i]->decalsystem.numdecals;

	R_DrawModelDecals_FadeEntity(r_refdef.scene.worldentity);
	for (i = 0;i < r_refdef.scene.numentities;i++)
		if (r_refdef.scene.entities[i]->decalsystem.numdecals)
			R_DrawModelDecals_FadeEntity(r_refdef.scene.entities[i]);

	R_DecalSystem_ApplySplatEntitiesQueue();

	numdecals = r_refdef.scene.worldentity->decalsystem.numdecals;
	for (i = 0;i < r_refdef.scene.numentities;i++)
		numdecals += r_refdef.scene.entities[i]->decalsystem.numdecals;

	r_refdef.stats[r_stat_totaldecals] += numdecals;

	if (r_showsurfaces.integer || !r_drawdecals.integer)
		return;

	R_DrawModelDecals_Entity(r_refdef.scene.worldentity);

	for (i = 0;i < r_refdef.scene.numentities;i++)
	{
		if (!r_refdef.viewcache.entityvisible[i])
			continue;
		if (r_refdef.scene.entities[i]->decalsystem.numdecals)
			R_DrawModelDecals_Entity(r_refdef.scene.entities[i]);
	}
}

static void R_DrawDebugModel(void)
{
	entity_render_t *ent = rsurface.entity;
	int j, flagsmask;
	const msurface_t *surface;
	model_t *model = ent->model;

	if (!sv.active  && !cls.demoplayback && ent != r_refdef.scene.worldentity)
		return;

	if (r_showoverdraw.value > 0)
	{
		float c = r_refdef.view.colorscale * r_showoverdraw.value * 0.125f;
		flagsmask = MATERIALFLAG_SKY | MATERIALFLAG_WALL;
		R_SetupShader_Generic_NoTexture(false, false);
		GL_DepthTest(false);
		GL_DepthMask(false);
		GL_DepthRange(0, 1);
		GL_BlendFunc(GL_ONE, GL_ONE);
		for (j = model->submodelsurfaces_start;j < model->submodelsurfaces_end;j++)
		{
			if (ent == r_refdef.scene.worldentity && !r_refdef.viewcache.world_surfacevisible[j])
				continue;
			surface = model->data_surfaces + j;
			rsurface.texture = R_GetCurrentTexture(surface->texture);
			if ((rsurface.texture->currentmaterialflags & flagsmask) && surface->num_triangles)
			{
				RSurf_PrepareVerticesForBatch(BATCHNEED_ARRAY_VERTEX | BATCHNEED_NOGAPS, 1, &surface);
				GL_CullFace((rsurface.texture->currentmaterialflags & MATERIALFLAG_NOCULLFACE) ? GL_NONE : r_refdef.view.cullface_back);
				if ((rsurface.texture->currentmaterialflags & MATERIALFLAG_BLENDED))
					GL_Color(c, 0, 0, 1.0f);
				else if (ent == r_refdef.scene.worldentity)
					GL_Color(c, c, c, 1.0f);
				else
					GL_Color(0, c, 0, 1.0f);
				R_Mesh_PrepareVertices_Generic_Arrays(rsurface.batchnumvertices, rsurface.batchvertex3f, NULL, NULL);
				RSurf_DrawBatch();
			}
		}
		rsurface.texture = NULL;
	}

	flagsmask = MATERIALFLAG_SKY | MATERIALFLAG_WALL;

//	R_Mesh_ResetTextureState();
	R_SetupShader_Generic_NoTexture(false, false);
	GL_DepthRange(0, 1);
	GL_DepthTest(!r_showdisabledepthtest.integer);
	GL_DepthMask(false);
	GL_BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	if (r_showcollisionbrushes.value > 0 && model->collision_bih.numleafs)
	{
		int triangleindex;
		int bihleafindex;
		qbool cullbox = false;
		const q3mbrush_t *brush;
		const bih_t *bih = &model->collision_bih;
		const bih_leaf_t *bihleaf;
		float vertex3f[3][3];
		GL_PolygonOffset(r_refdef.polygonfactor + r_showcollisionbrushes_polygonfactor.value, r_refdef.polygonoffset + r_showcollisionbrushes_polygonoffset.value);
		for (bihleafindex = 0, bihleaf = bih->leafs;bihleafindex < bih->numleafs;bihleafindex++, bihleaf++)
		{
			if (cullbox && R_CullFrustum(bihleaf->mins, bihleaf->maxs))
				continue;
			switch (bihleaf->type)
			{
			case BIH_BRUSH:
				brush = model->brush.data_brushes + bihleaf->itemindex;
				if (brush->colbrushf && brush->colbrushf->numtriangles)
				{
					GL_Color((bihleafindex & 31) * (1.0f / 32.0f) * r_refdef.view.colorscale, ((bihleafindex >> 5) & 31) * (1.0f / 32.0f) * r_refdef.view.colorscale, ((bihleafindex >> 10) & 31) * (1.0f / 32.0f) * r_refdef.view.colorscale, r_showcollisionbrushes.value);
					R_Mesh_PrepareVertices_Generic_Arrays(brush->colbrushf->numpoints, brush->colbrushf->points->v, NULL, NULL);
					R_Mesh_Draw(0, brush->colbrushf->numpoints, 0, brush->colbrushf->numtriangles, brush->colbrushf->elements, NULL, 0, NULL, NULL, 0);
				}
				break;
			case BIH_COLLISIONTRIANGLE:
				triangleindex = bihleaf->itemindex;
				VectorCopy(model->brush.data_collisionvertex3f + 3*model->brush.data_collisionelement3i[triangleindex*3+0], vertex3f[0]);
				VectorCopy(model->brush.data_collisionvertex3f + 3*model->brush.data_collisionelement3i[triangleindex*3+1], vertex3f[1]);
				VectorCopy(model->brush.data_collisionvertex3f + 3*model->brush.data_collisionelement3i[triangleindex*3+2], vertex3f[2]);
				GL_Color((bihleafindex & 31) * (1.0f / 32.0f) * r_refdef.view.colorscale, ((bihleafindex >> 5) & 31) * (1.0f / 32.0f) * r_refdef.view.colorscale, ((bihleafindex >> 10) & 31) * (1.0f / 32.0f) * r_refdef.view.colorscale, r_showcollisionbrushes.value);
				R_Mesh_PrepareVertices_Generic_Arrays(3, vertex3f[0], NULL, NULL);
				R_Mesh_Draw(0, 3, 0, 1, polygonelement3i, NULL, 0, polygonelement3s, NULL, 0);
				break;
			case BIH_RENDERTRIANGLE:
				triangleindex = bihleaf->itemindex;
				VectorCopy(model->surfmesh.data_vertex3f + 3*model->surfmesh.data_element3i[triangleindex*3+0], vertex3f[0]);
				VectorCopy(model->surfmesh.data_vertex3f + 3*model->surfmesh.data_element3i[triangleindex*3+1], vertex3f[1]);
				VectorCopy(model->surfmesh.data_vertex3f + 3*model->surfmesh.data_element3i[triangleindex*3+2], vertex3f[2]);
				GL_Color((bihleafindex & 31) * (1.0f / 32.0f) * r_refdef.view.colorscale, ((bihleafindex >> 5) & 31) * (1.0f / 32.0f) * r_refdef.view.colorscale, ((bihleafindex >> 10) & 31) * (1.0f / 32.0f) * r_refdef.view.colorscale, r_showcollisionbrushes.value);
				R_Mesh_PrepareVertices_Generic_Arrays(3, vertex3f[0], NULL, NULL);
				R_Mesh_Draw(0, 3, 0, 1, polygonelement3i, NULL, 0, polygonelement3s, NULL, 0);
				break;
			}
		}
	}

	GL_PolygonOffset(r_refdef.polygonfactor, r_refdef.polygonoffset);

#ifndef USE_GLES2
	if (r_showtris.value > 0 && qglPolygonMode)
	{
		if (r_showdisabledepthtest.integer)
		{
			GL_BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			GL_DepthMask(false);
		}
		else
		{
			GL_BlendFunc(GL_ONE, GL_ZERO);
			GL_DepthMask(true);
		}
		qglPolygonMode(GL_FRONT_AND_BACK, GL_LINE);CHECKGLERROR
		for (j = model->submodelsurfaces_start; j < model->submodelsurfaces_end; j++)
		{
			if (ent == r_refdef.scene.worldentity && !r_refdef.viewcache.world_surfacevisible[j])
				continue;
			surface = model->data_surfaces + j;
			rsurface.texture = R_GetCurrentTexture(surface->texture);
			if ((rsurface.texture->currentmaterialflags & flagsmask) && surface->num_triangles)
			{
				RSurf_PrepareVerticesForBatch(BATCHNEED_ARRAY_VERTEX | BATCHNEED_ARRAY_NORMAL | BATCHNEED_ARRAY_VECTOR | BATCHNEED_NOGAPS, 1, &surface);
				if ((rsurface.texture->currentmaterialflags & MATERIALFLAG_BLENDED))
					GL_Color(r_refdef.view.colorscale, 0, 0, r_showtris.value);
				else if (ent == r_refdef.scene.worldentity)
					GL_Color(r_refdef.view.colorscale, r_refdef.view.colorscale, r_refdef.view.colorscale, r_showtris.value);
				else
					GL_Color(0, r_refdef.view.colorscale, 0, r_showtris.value);
				R_Mesh_PrepareVertices_Generic_Arrays(rsurface.batchnumvertices, rsurface.batchvertex3f, NULL, NULL);
				RSurf_DrawBatch();
			}
		}
		qglPolygonMode(GL_FRONT_AND_BACK, GL_FILL);CHECKGLERROR
		rsurface.texture = NULL;
	}

# if 0
	// FIXME!  implement r_shownormals with just triangles
	if (r_shownormals.value != 0 && qglBegin)
	{
		int l, k;
		vec3_t v;
		if (r_showdisabledepthtest.integer)
		{
			GL_BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			GL_DepthMask(false);
		}
		else
		{
			GL_BlendFunc(GL_ONE, GL_ZERO);
			GL_DepthMask(true);
		}
		for (j = model->submodelsurfaces_start; j < model->submodelsurfaces_end; j++)
		{
			if (ent == r_refdef.scene.worldentity && !r_refdef.viewcache.world_surfacevisible[j])
				continue;
			surface = model->data_surfaces + j;
			rsurface.texture = R_GetCurrentTexture(surface->texture);
			if ((rsurface.texture->currentmaterialflags & flagsmask) && surface->num_triangles)
			{
				RSurf_PrepareVerticesForBatch(BATCHNEED_ARRAY_VERTEX | BATCHNEED_ARRAY_NORMAL | BATCHNEED_ARRAY_VECTOR | BATCHNEED_NOGAPS, 1, &surface);
				qglBegin(GL_LINES);
				if (r_shownormals.value < 0 && rsurface.batchnormal3f)
				{
					for (k = 0, l = rsurface.batchfirstvertex;k < rsurface.batchnumvertices;k++, l++)
					{
						VectorCopy(rsurface.batchvertex3f + l * 3, v);
						GL_Color(0, 0, r_refdef.view.colorscale, 1);
						qglVertex3f(v[0], v[1], v[2]);
						VectorMA(v, -r_shownormals.value, rsurface.batchnormal3f + l * 3, v);
						GL_Color(r_refdef.view.colorscale, r_refdef.view.colorscale, r_refdef.view.colorscale, 1);
						qglVertex3f(v[0], v[1], v[2]);
					}
				}
				if (r_shownormals.value > 0 && rsurface.batchsvector3f)
				{
					for (k = 0, l = rsurface.batchfirstvertex;k < rsurface.batchnumvertices;k++, l++)
					{
						VectorCopy(rsurface.batchvertex3f + l * 3, v);
						GL_Color(r_refdef.view.colorscale, 0, 0, 1);
						qglVertex3f(v[0], v[1], v[2]);
						VectorMA(v, r_shownormals.value, rsurface.batchsvector3f + l * 3, v);
						GL_Color(r_refdef.view.colorscale, r_refdef.view.colorscale, r_refdef.view.colorscale, 1);
						qglVertex3f(v[0], v[1], v[2]);
					}
				}
				if (r_shownormals.value > 0 && rsurface.batchtvector3f)
				{
					for (k = 0, l = rsurface.batchfirstvertex;k < rsurface.batchnumvertices;k++, l++)
					{
						VectorCopy(rsurface.batchvertex3f + l * 3, v);
						GL_Color(0, r_refdef.view.colorscale, 0, 1);
						qglVertex3f(v[0], v[1], v[2]);
						VectorMA(v, r_shownormals.value, rsurface.batchtvector3f + l * 3, v);
						GL_Color(r_refdef.view.colorscale, r_refdef.view.colorscale, r_refdef.view.colorscale, 1);
						qglVertex3f(v[0], v[1], v[2]);
					}
				}
				if (r_shownormals.value > 0 && rsurface.batchnormal3f)
				{
					for (k = 0, l = rsurface.batchfirstvertex;k < rsurface.batchnumvertices;k++, l++)
					{
						VectorCopy(rsurface.batchvertex3f + l * 3, v);
						GL_Color(0, 0, r_refdef.view.colorscale, 1);
						qglVertex3f(v[0], v[1], v[2]);
						VectorMA(v, r_shownormals.value, rsurface.batchnormal3f + l * 3, v);
						GL_Color(r_refdef.view.colorscale, r_refdef.view.colorscale, r_refdef.view.colorscale, 1);
						qglVertex3f(v[0], v[1], v[2]);
					}
				}
				qglEnd();
				CHECKGLERROR
			}
		}
		rsurface.texture = NULL;
	}
# endif
#endif
}

int r_maxsurfacelist = 0;
const msurface_t **r_surfacelist = NULL;
void R_DrawModelSurfaces(entity_render_t *ent, qbool skysurfaces, qbool writedepth, qbool depthonly, qbool debug, qbool prepass, qbool ui)
{
	int i, j, flagsmask;
	model_t *model = ent->model;
	msurface_t *surfaces;
	unsigned char *update;
	int numsurfacelist = 0;
	if (model == NULL)
		return;

	if (r_maxsurfacelist < model->num_surfaces)
	{
		r_maxsurfacelist = model->num_surfaces;
		if (r_surfacelist)
			Mem_Free((msurface_t **)r_surfacelist);
		r_surfacelist = (const msurface_t **) Mem_Alloc(r_main_mempool, r_maxsurfacelist * sizeof(*r_surfacelist));
	}

	if (r_showsurfaces.integer && r_showsurfaces.integer != 3)
		RSurf_ActiveModelEntity(ent, false, false, false);
	else if (prepass)
		RSurf_ActiveModelEntity(ent, true, true, true);
	else if (depthonly)
		RSurf_ActiveModelEntity(ent, model->wantnormals, model->wanttangents, false);
	else
		RSurf_ActiveModelEntity(ent, true, true, false);

	surfaces = model->data_surfaces;
	update = model->brushq1.lightmapupdateflags;

	flagsmask = skysurfaces ? MATERIALFLAG_SKY : MATERIALFLAG_WALL;

	if (debug)
	{
		R_DrawDebugModel();
		rsurface.entity = NULL; // used only by R_GetCurrentTexture and RSurf_ActiveModelEntity
		return;
	}

	// check if this is an empty model
	if (model->submodelsurfaces_start >= model->submodelsurfaces_end)
		return;

	rsurface.lightmaptexture = NULL;
	rsurface.deluxemaptexture = NULL;
	rsurface.uselightmaptexture = false;
	rsurface.texture = NULL;
	rsurface.rtlight = NULL;
	numsurfacelist = 0;

	// add visible surfaces to draw list
	if (ent == r_refdef.scene.worldentity)
	{
		// for the world entity, check surfacevisible
		for (i = model->submodelsurfaces_start;i < model->submodelsurfaces_end;i++)
		{
			j = model->modelsurfaces_sorted[i];
			if (r_refdef.viewcache.world_surfacevisible[j])
				r_surfacelist[numsurfacelist++] = surfaces + j;
		}

		// don't do anything if there were no surfaces added (none of the world entity is visible)
		if (!numsurfacelist)
		{
			rsurface.entity = NULL; // used only by R_GetCurrentTexture and RSurf_ActiveModelEntity
			return;
		}
	}
	else if (ui)
	{
		// for ui we have to preserve the order of surfaces (not using modelsurfaces_sorted)
		for (i = model->submodelsurfaces_start; i < model->submodelsurfaces_end; i++)
			r_surfacelist[numsurfacelist++] = surfaces + i;
	}
	else
	{
		// add all surfaces
		for (i = model->submodelsurfaces_start; i < model->submodelsurfaces_end; i++)
			r_surfacelist[numsurfacelist++] = surfaces + model->modelsurfaces_sorted[i];
	}

	/*
	 * Mark lightmaps as dirty if their lightstyle's value changed. We do this by
	 * using style chains because most styles do not change on most frames, and most
	 * surfaces do not have styles on them. Mods like Arcane Dimensions (e.g. ad_necrokeep)
	 * break this rule and animate most surfaces.
	 */
	if (update && !skysurfaces && !depthonly && !prepass && model->brushq1.num_lightstyles && r_refdef.scene.lightmapintensity > 0 && r_q1bsp_lightmap_updates_enabled.integer)
	{
		model_brush_lightstyleinfo_t *style;

		// For each lightstyle, check if its value changed and mark the lightmaps as dirty if so
		for (i = 0, style = model->brushq1.data_lightstyleinfo; i < model->brushq1.num_lightstyles; i++, style++)
		{
			if (style->value != r_refdef.scene.lightstylevalue[style->style])
			{
				int* list = style->surfacelist;
				style->value = r_refdef.scene.lightstylevalue[style->style];
				// Value changed - mark the surfaces belonging to this style chain as dirty
				for (j = 0; j < style->numsurfaces; j++)
					update[list[j]] = true;
			}
		}
		// Now check if update flags are set on any surfaces that are visible
		if (r_q1bsp_lightmap_updates_hidden_surfaces.integer)
		{
			/*
			 * We can do less frequent texture uploads (approximately 10hz for animated
			 * lightstyles) by rebuilding lightmaps on surfaces that are not currently visible.
			 * For optimal efficiency, this includes the submodels of the worldmodel, so we
			 * use model->num_surfaces, not nummodelsurfaces.
			 */
			for (i = 0; i < model->num_surfaces;i++)
				if (update[i])
					R_BuildLightMap(ent, surfaces + i, r_q1bsp_lightmap_updates_combine.integer);
		}
		else
		{
			for (i = 0; i < numsurfacelist; i++)
				if (update[r_surfacelist[i] - surfaces])
					R_BuildLightMap(ent, (msurface_t *)r_surfacelist[i], r_q1bsp_lightmap_updates_combine.integer);
		}
	}

	R_QueueModelSurfaceList(ent, numsurfacelist, r_surfacelist, flagsmask, writedepth, depthonly, prepass, ui);

	// add to stats if desired
	if (r_speeds.integer && !skysurfaces && !depthonly)
	{
		r_refdef.stats[r_stat_entities_surfaces] += numsurfacelist;
		for (j = 0;j < numsurfacelist;j++)
			r_refdef.stats[r_stat_entities_triangles] += r_surfacelist[j]->num_triangles;
	}

	rsurface.entity = NULL; // used only by R_GetCurrentTexture and RSurf_ActiveModelEntity
}

void R_DebugLine(vec3_t start, vec3_t end)
{
	model_t *mod = CL_Mesh_UI();
	msurface_t *surf;
	int e0, e1, e2, e3;
	float offsetx, offsety, x1, y1, x2, y2, width = 1.0f;
	float r1 = 1.0f, g1 = 0.0f, b1 = 0.0f, alpha1 = 0.25f;
	float r2 = 1.0f, g2 = 1.0f, b2 = 0.0f, alpha2 = 0.25f;
	vec4_t w[2], s[2];

	// transform to screen coords first
	Vector4Set(w[0], start[0], start[1], start[2], 1);
	Vector4Set(w[1], end[0], end[1], end[2], 1);
	R_Viewport_TransformToScreen(&r_refdef.view.viewport, w[0], s[0]);
	R_Viewport_TransformToScreen(&r_refdef.view.viewport, w[1], s[1]);
	x1 = s[0][0] * vid_conwidth.value / vid.mode.width;
	y1 = (vid.mode.height - s[0][1]) * vid_conheight.value / vid.mode.height;
	x2 = s[1][0] * vid_conwidth.value / vid.mode.width;
	y2 = (vid.mode.height - s[1][1]) * vid_conheight.value / vid.mode.height;
	//Con_DPrintf("R_DebugLine: %.0f,%.0f to %.0f,%.0f\n", x1, y1, x2, y2);

	// add the line to the UI mesh for drawing later

	// width is measured in real pixels
	if (fabs(x2 - x1) > fabs(y2 - y1))
	{
		offsetx = 0;
		offsety = 0.5f * width * vid_conheight.value / vid.mode.height;
	}
	else
	{
		offsetx = 0.5f * width * vid_conwidth.value / vid.mode.width;
		offsety = 0;
	}
	surf = Mod_Mesh_AddSurface(mod, Mod_Mesh_GetTexture(mod, "white", 0, 0, MATERIALFLAG_WALL | MATERIALFLAG_VERTEXCOLOR | MATERIALFLAG_ALPHAGEN_VERTEX | MATERIALFLAG_ALPHA | MATERIALFLAG_BLENDED | MATERIALFLAG_NOSHADOW), true);
	e0 = Mod_Mesh_IndexForVertex(mod, surf, x1 - offsetx, y1 - offsety, 10, 0, 0, -1, 0, 0, 0, 0, r1, g1, b1, alpha1);
	e1 = Mod_Mesh_IndexForVertex(mod, surf, x2 - offsetx, y2 - offsety, 10, 0, 0, -1, 0, 0, 0, 0, r2, g2, b2, alpha2);
	e2 = Mod_Mesh_IndexForVertex(mod, surf, x2 + offsetx, y2 + offsety, 10, 0, 0, -1, 0, 0, 0, 0, r2, g2, b2, alpha2);
	e3 = Mod_Mesh_IndexForVertex(mod, surf, x1 + offsetx, y1 + offsety, 10, 0, 0, -1, 0, 0, 0, 0, r1, g1, b1, alpha1);
	Mod_Mesh_AddTriangle(mod, surf, e0, e1, e2);
	Mod_Mesh_AddTriangle(mod, surf, e0, e2, e3);

}


void R_DrawCustomSurface(skinframe_t *skinframe, const matrix4x4_t *texmatrix, int materialflags, int firstvertex, int numvertices, int firsttriangle, int numtriangles, qbool writedepth, qbool prepass, qbool ui)
{
	static texture_t texture;

	// fake enough texture and surface state to render this geometry

	texture.update_lastrenderframe = -1; // regenerate this texture
	texture.basematerialflags = materialflags | MATERIALFLAG_CUSTOMSURFACE | MATERIALFLAG_WALL;
	texture.basealpha = 1.0f;
	texture.currentskinframe = skinframe;
	texture.currenttexmatrix = *texmatrix; // requires MATERIALFLAG_CUSTOMSURFACE
	texture.offsetmapping = OFFSETMAPPING_OFF;
	texture.offsetscale = 1;
	texture.specularscalemod = 1;
	texture.specularpowermod = 1;
	texture.transparentsort = TRANSPARENTSORT_DISTANCE;

	R_DrawCustomSurface_Texture(&texture, texmatrix, materialflags, firstvertex, numvertices, firsttriangle, numtriangles, writedepth, prepass, ui);
}

void R_DrawCustomSurface_Texture(texture_t *texture, const matrix4x4_t *texmatrix, int materialflags, int firstvertex, int numvertices, int firsttriangle, int numtriangles, qbool writedepth, qbool prepass, qbool ui)
{
	static msurface_t surface;
	const msurface_t *surfacelist = &surface;

	// fake enough texture and surface state to render this geometry
	surface.texture = texture;
	surface.num_triangles = numtriangles;
	surface.num_firsttriangle = firsttriangle;
	surface.num_vertices = numvertices;
	surface.num_firstvertex = firstvertex;

	// now render it
	rsurface.texture = R_GetCurrentTexture(surface.texture);
	rsurface.lightmaptexture = NULL;
	rsurface.deluxemaptexture = NULL;
	rsurface.uselightmaptexture = false;
	R_DrawModelTextureSurfaceList(1, &surfacelist, writedepth, prepass, ui);
}
