
#include "quakedef.h"
#include "cl_video.h"
#include "image.h"
#include "jpeg.h"
#include "image_png.h"
#include "cl_collision.h"
#include "libcurl.h"
#include "csprogs.h"
#include "r_stats.h"
#include "metal_backend.h"
#include "gl_backend.h"   // METAL.md Phase 5: the RT composite is an ordinary backend draw
#include "r_shadow.h"     // BEAUTY C1: R_RTLight_Update, for the storm's fog light
#ifdef CONFIG_VIDEO_CAPTURE
#include "cap_avi.h"
#include "cap_ogg.h"

#ifdef USE_RT_METAL
#include "rt_metal.h"   // SPIKE: in-process Metal RT sidecar (soft-shadow composite)
extern cvar_t rt_metal;
extern cvar_t rt_metal_liquids;   // read only by the inert-configuration report below
extern cvar_t rt_metal_samples;    // runtime shadow tuning knobs (registered in vid_sdl.c)
extern cvar_t rt_metal_scale;      // trace resolution as a fraction of the viewport
extern cvar_t rt_metal_smoothnormals; // interpolate entity vertex normals in the kernel
extern cvar_t rt_metal_reproject;  // composite remaps the stale term through the shown camera
extern cvar_t rt_metal_reproject_depth;  // ...and does it translation-aware, via the hit distance in alpha
extern cvar_t rt_metal_sameframe;  // trace and composite the SAME frame (fringing fix, costs the async overlap)
extern cvar_t rt_metal_bluenoise;  // blue-noise kernel jitter (the weave fix); 0 = classic IGN dither exactly
extern cvar_t rt_metal_lightcores; // flame models become a separate emissive instance
extern cvar_t rt_metal_lavaemissive; // lava sheets become a separate emissive instance (closes the see-through-lighting leak)
extern cvar_t rt_metal_lavalights;   // lava lakes feed warm lights into the RT light list
extern cvar_t rt_metal_skyopen;      // sky brushes become the OPEN SKY instance (primary rays reach the sentinel)
extern cvar_t rt_metal_liquidemissive; // opaque water/slime become their own EMISSIVE instance
extern cvar_t rt_metal_liquids_rt;     // SEPTEMBER2 C2: blended water/slime become their own instance for the liquid term + reflection
extern cvar_t rt_metal_liquids_reflect;
extern cvar_t r_wateralpha_force;    // the transparent-water enabling set (gl_rmain.c owns these two;
extern cvar_t r_water;               //  render.h already externs r_wateralpha / r_novis / r_trippy)
extern cvar_t rt_metal_softness;
extern cvar_t rt_metal_darkness;
extern cvar_t rt_metal_culldist;
extern cvar_t rt_metal_history;
extern cvar_t rt_metal_color;      // strength of the dynamic-light colored brighten
extern cvar_t rt_metal_walllight;  // full RT wall lighting (render fullbright, RT does all lighting)
extern cvar_t rt_metal_ambient;    // ambient fill for wall-lighting mode
extern cvar_t rt_metal_shafts;     // god rays (volumetric light shafts)
extern cvar_t rt_metal_shafts_samples;
extern cvar_t rt_metal_shafts_scale;
extern cvar_t rt_metal_shafts_history;
extern cvar_t rt_metal_shafts_dist;
extern cvar_t rt_metal_shafts_residual;
extern cvar_t rt_metal_fog;        // full in-kernel fog lighting
extern cvar_t rt_metal_fog_steps;
extern cvar_t rt_metal_fog_scale;
extern cvar_t rt_metal_fog_intensity;
extern cvar_t rt_metal_fog_stride;
extern cvar_t rt_metal_fog_history;
extern cvar_t rt_metal_fog_residual;
extern cvar_t rt_metal_fog_beams;
extern cvar_t rt_metal_fog_upsample;
extern cvar_t rt_metal_fog_stepjitter;
extern cvar_t rt_metal_fog_filter;
extern cvar_t rt_metal_fog_clamp;      // BLUENOISE slice 2: fog history clamp (0 = the old path)
extern cvar_t rt_metal_fog_clamp_k;
extern cvar_t rt_metal_fog_tonemapema;  // BLUENOISE slice 3: tonemapped fog accumulation
extern cvar_t rt_metal_fog_reproject_depth;  // translation-aware fog history (needs the clamp pass)
extern cvar_t rt_metal_fog_froxel;           // A1 FROXEL: the 3D fog volume with a per-cell history
extern cvar_t rt_metal_pipeline;             // A2: commit the raster before the trace is waited on
extern cvar_t rt_metal_as_skipstatic;        // A3: reuse an unchanged BLAS/TLAS
extern cvar_t rt_metal_sun, rt_metal_sun_light, rt_metal_sun_color, rt_metal_sun_mangle, rt_metal_sun_penumbra;   // D: the sky light
extern cvar_t rt_metal_fog_froxel_slices;
extern cvar_t rt_metal_fog_froxel_history;
extern cvar_t rt_metal_fog_froxel_curve;
extern cvar_t rt_metal_fog_froxel_near;
extern cvar_t rt_metal_fog_froxel_castphase;
extern cvar_t rt_metal_fog_reproject_tol;
extern cvar_t rt_metal_lightsample_hybrid;  // dominant + pick in the fog cast
extern cvar_t rt_metal_fog_upsample_depth;
extern cvar_t rt_metal_lmax;
extern cvar_t rt_metal_refit;
extern cvar_t rt_metal_fog_stride_adaptive;
extern cvar_t rt_metal_term_upsample;
extern cvar_t rt_metal_term_upsample_depth;
extern cvar_t rt_metal_lightsample;
extern cvar_t rt_metal_lightsample_rays;
extern cvar_t rt_metal_lightsample_clamp;  // fog/shaft 1/p ceiling (demo17 speckle fix)
extern cvar_t rt_metal_gi;              // one-bounce diffuse GI (GIARC G1)
extern cvar_t rt_metal_gi_dist;
extern cvar_t rt_metal_gi_albedo;
extern cvar_t rt_metal_gi_history;
extern cvar_t rt_metal_gi_intensity;
extern cvar_t rt_metal_gi_emissive;    // emissive-instance bounce (GIARC G3)
extern cvar_t rt_metal_gi_rate;        // 1-in-N bounce-sample rotation (GIARC G4-1)
extern cvar_t rt_metal_gi_albedo_tex;
extern cvar_t rt_metal_gi_fallback;  // coloured bounce (GIARC G4-2)
extern cvar_t rt_metal_gi_tiledilate; // dilated tile cull for the bounce pick (GIARC G4-4)
extern cvar_t rt_metal_gi_ao, rt_metal_gi_ao_dist; // BEAUTY B1: AO from the bounce ray
extern cvar_t rt_metal_fog_liquidlight; // BEAUTY B2: light in the water
extern cvar_t rt_metal_contact; // BEAUTY B3: contact-hardened shadows
extern cvar_t rt_metal_shadowlights;      // 2026-09-19: brightest lights shadow-tested per pixel
extern cvar_t rt_metal_shadowlights_rays;
extern cvar_t r_skylightning, r_skylightning_period, r_skylightning_hold, r_skylightning_fog; // BEAUTY C1: lightning in the sky
extern cvar_t rt_metal_viewmodel;         // the view weapon's own RT-matched lighting
extern cvar_t rt_metal_viewmodel_shadows;
extern cvar_t rt_metal_viewmodel_smooth;
// The view weapon's light is evaluated over a SEPARATE, much smaller list than the
// sidecar's 2048: it is one point, so anything the cull leaves in range of the eye
// is already far more than the eye can tell apart, and this keeps the per-frame
// scratch off the sidecar's.
#define RT_VML_MAXLIGHTS    256
#define RT_VML_MAXSHADOW    16    // ceiling on rt_metal_viewmodel_shadows
// How much of the term is delivered as flat ambient rather than through N.L. Too
// low and the facets turned away from the light read black (the gun looks
// half-missing); too high and it flattens back towards the bug this fixes.
#define RT_VML_AMBIENTSHARE 0.35f
extern cvar_t r_volumetric;        // fog kernel needs the murk pass to consume it
void R_Volumetric_GetFogKernelParams(rt_fog_shade_t *out);   // gl_rmain.c
extern cvar_t r_fullbright;        // gl_rmain.c: render the world unlit (albedo) — driven by wall-lighting mode
int R_Shadow_GetWorldLightPositions(float *dst, int maxlights, const float *vieworigin, float culldist);   // r_shadow.c
extern float r_shadow_maplightbudget;   // r_shadow.c, set at map load
extern cvar_t m5_packlight;
// e1m3's budget (mean light luminance x mean radius). id1's other maps sit
// between 146 and 183, so the baseline is the low end of the range the whole
// look was tuned against rather than an average of it.
#define M5_MAPLIGHT_BASELINE 146.0f
// Deliberately tight. This corrects a mapper's exposure choice, and a mapper
// who wanted a dark level should still get a dark level.
#define M5_MAPLIGHT_MINGAIN 0.55f
#define M5_MAPLIGHT_MAXGAIN 1.60f
#endif
#endif

// we have to include snd_main.h here only to get access to snd_renderbuffer->format.speed when writing the AVI headers
#include "snd_main.h"
#include "dpcmdtrace_intercept.h"   // METAL.md Phase 1: no-op without -DDP_CMDTRACE

cvar_t scr_viewsize = {CF_CLIENT | CF_ARCHIVE, "viewsize","100", "how large the view should be, 110 disables inventory bar, 120 disables status bar"};
cvar_t scr_fov = {CF_CLIENT | CF_ARCHIVE, "fov","90", "field of vision, 1-170 degrees, default 90, some players use 110-130"};
cvar_t scr_conalpha = {CF_CLIENT | CF_ARCHIVE, "scr_conalpha", "0.9", "opacity of console background gfx/conback (when console isn't forced fullscreen)"};
cvar_t scr_conalphafactor = {CF_CLIENT | CF_ARCHIVE, "scr_conalphafactor", "1", "opacity of console background gfx/conback relative to scr_conalpha; when 0, gfx/conback is not drawn"};
cvar_t scr_conalpha2factor = {CF_CLIENT | CF_ARCHIVE, "scr_conalpha2factor", "0", "opacity of console background gfx/conback2 relative to scr_conalpha; when 0, gfx/conback2 is not drawn"};
cvar_t scr_conalpha3factor = {CF_CLIENT | CF_ARCHIVE, "scr_conalpha3factor", "0", "opacity of console background gfx/conback3 relative to scr_conalpha; when 0, gfx/conback3 is not drawn"};
cvar_t scr_conbrightness = {CF_CLIENT | CF_ARCHIVE, "scr_conbrightness", "1", "brightness of console background (0 = black, 1 = image)"};
cvar_t scr_conforcewhiledisconnected = {CF_CLIENT, "scr_conforcewhiledisconnected", "1", "1 forces fullscreen console while disconnected, 2 also forces it when the listen server has started but the client is still loading"};
cvar_t scr_conheight = {CF_CLIENT | CF_ARCHIVE, "scr_conheight", "0.5", "fraction of screen height occupied by console (reduced as necessary for visibility of loading progress and infobar)"};
cvar_t scr_conscroll_x = {CF_CLIENT | CF_ARCHIVE, "scr_conscroll_x", "0", "scroll speed of gfx/conback in x direction"};
cvar_t scr_conscroll_y = {CF_CLIENT | CF_ARCHIVE, "scr_conscroll_y", "0", "scroll speed of gfx/conback in y direction"};
cvar_t scr_conscroll2_x = {CF_CLIENT | CF_ARCHIVE, "scr_conscroll2_x", "0", "scroll speed of gfx/conback2 in x direction"};
cvar_t scr_conscroll2_y = {CF_CLIENT | CF_ARCHIVE, "scr_conscroll2_y", "0", "scroll speed of gfx/conback2 in y direction"};
cvar_t scr_conscroll3_x = {CF_CLIENT | CF_ARCHIVE, "scr_conscroll3_x", "0", "scroll speed of gfx/conback3 in x direction"};
cvar_t scr_conscroll3_y = {CF_CLIENT | CF_ARCHIVE, "scr_conscroll3_y", "0", "scroll speed of gfx/conback3 in y direction"};
#ifdef CONFIG_MENU
cvar_t scr_menuforcewhiledisconnected = {CF_CLIENT, "scr_menuforcewhiledisconnected", "0", "forces menu while disconnected"};
#endif
cvar_t scr_centertime = {CF_CLIENT, "scr_centertime","2", "how long centerprint messages show"};
cvar_t scr_showram = {CF_CLIENT | CF_ARCHIVE, "showram","1", "show ram icon if low on surface cache memory (not used)"};
cvar_t scr_showturtle = {CF_CLIENT | CF_ARCHIVE, "showturtle","0", "show turtle icon when framerate is too low"};
cvar_t scr_showpause = {CF_CLIENT | CF_ARCHIVE, "showpause","1", "show pause icon when game is paused"};
cvar_t scr_showbrand = {CF_CLIENT, "showbrand","0", "shows gfx/brand.tga in a corner of the screen (different values select different positions, including centered)"};
cvar_t scr_printspeed = {CF_CLIENT, "scr_printspeed","0", "speed of intermission printing (episode end texts), a value of 0 disables the slow printing"};
cvar_t scr_loadingscreen_background = {CF_CLIENT, "scr_loadingscreen_background","0", "show the last visible background during loading screen (costs one screenful of video memory)"};
cvar_t scr_loadingscreen_scale = {CF_CLIENT, "scr_loadingscreen_scale","1", "scale factor of the background"};
cvar_t scr_loadingscreen_scale_base = {CF_CLIENT, "scr_loadingscreen_scale_base","0", "0 = console pixels, 1 = video pixels"};
cvar_t scr_loadingscreen_scale_limit = {CF_CLIENT, "scr_loadingscreen_scale_limit","0", "0 = no limit, 1 = until first edge hits screen edge, 2 = until last edge hits screen edge, 3 = until width hits screen width, 4 = until height hits screen height"};
cvar_t scr_loadingscreen_picture = {CF_CLIENT, "scr_loadingscreen_picture", "gfx/loading", "picture shown during loading"};
cvar_t scr_loadingscreen_count = {CF_CLIENT, "scr_loadingscreen_count","1", "number of loading screen files to use randomly (named loading.tga, loading2.tga, loading3.tga, ...)"};
cvar_t scr_loadingscreen_firstforstartup = {CF_CLIENT, "scr_loadingscreen_firstforstartup","0", "remove loading.tga from random scr_loadingscreen_count selection and only display it on client startup, 0 = normal, 1 = firstforstartup"};
cvar_t scr_loadingscreen_barcolor = {CF_CLIENT, "scr_loadingscreen_barcolor", "0 0 1", "rgb color of loadingscreen progress bar"};
cvar_t scr_loadingscreen_barheight = {CF_CLIENT, "scr_loadingscreen_barheight", "8", "the height of the loadingscreen progress bar"};
cvar_t scr_loadingscreen_maxfps = {CF_CLIENT, "scr_loadingscreen_maxfps", "20", "maximum FPS for loading screen so it will not update very often (this reduces loading time with lots of models)"};
cvar_t scr_infobar_height = {CF_CLIENT, "scr_infobar_height", "8", "the height of the infobar items"};
cvar_t scr_sbarscale = {CF_CLIENT | CF_READONLY, "scr_sbarscale", "1", "current vid_height/vid_conheight, for compatibility with csprogs that read this cvar (found in Fitzquake-derived engines and FTEQW; despite the name it's not specific to the status bar)"};
cvar_t vid_conwidthauto = {CF_CLIENT | CF_ARCHIVE, "vid_conwidthauto", "1", "automatically update vid_conwidth to match aspect ratio"};
cvar_t vid_conwidth = {CF_CLIENT | CF_ARCHIVE, "vid_conwidth", "640", "virtual width of 2D graphics system (note: changes may be overwritten, see vid_conwidthauto)"};
cvar_t vid_conheight = {CF_CLIENT | CF_ARCHIVE, "vid_conheight", "480", "virtual height of 2D graphics system"};
cvar_t vid_pixelheight = {CF_CLIENT | CF_ARCHIVE, "vid_pixelheight", "1", "adjusts vertical field of vision to account for non-square pixels (1280x1024 on a CRT monitor for example)"};
cvar_t scr_screenshot_jpeg = {CF_CLIENT | CF_ARCHIVE, "scr_screenshot_jpeg","1", "save jpeg instead of targa or PNG"};
cvar_t scr_screenshot_jpeg_quality = {CF_CLIENT | CF_ARCHIVE, "scr_screenshot_jpeg_quality","0.9", "image quality of saved jpeg"};
cvar_t scr_screenshot_png = {CF_CLIENT | CF_ARCHIVE, "scr_screenshot_png","0", "save png instead of targa"};
cvar_t scr_screenshot_gammaboost = {CF_CLIENT | CF_ARCHIVE, "scr_screenshot_gammaboost","1", "gamma correction on saved screenshots and videos, 1.0 saves unmodified images"};
cvar_t scr_screenshot_alpha = {CF_CLIENT, "scr_screenshot_alpha","0", "try to write an alpha channel to screenshots (debugging feature)"};
cvar_t scr_screenshot_timestamp = {CF_CLIENT | CF_ARCHIVE, "scr_screenshot_timestamp", "1", "use a timestamp based number of the type YYYYMMDDHHMMSSsss instead of sequential numbering"};
// scr_screenshot_name is defined in fs.c
#ifdef CONFIG_VIDEO_CAPTURE
cvar_t cl_capturevideo = {CF_CLIENT, "cl_capturevideo", "0", "enables saving of video to a .avi file using uncompressed I420 colorspace and PCM audio, note that scr_screenshot_gammaboost affects the brightness of the output)"};
cvar_t cl_capturevideo_demo_stop = {CF_CLIENT | CF_ARCHIVE, "cl_capturevideo_demo_stop", "1", "automatically stops video recording when demo ends"};
cvar_t cl_capturevideo_printfps = {CF_CLIENT | CF_ARCHIVE, "cl_capturevideo_printfps", "1", "prints the frames per second captured in capturevideo (is only written to stdout and any log file, not to the console as that would be visible on the video), value is seconds of wall time between prints"};
cvar_t cl_capturevideo_width = {CF_CLIENT | CF_ARCHIVE, "cl_capturevideo_width", "0", "scales all frames to this resolution before saving the video"};
cvar_t cl_capturevideo_height = {CF_CLIENT | CF_ARCHIVE, "cl_capturevideo_height", "0", "scales all frames to this resolution before saving the video"};
cvar_t cl_capturevideo_realtime = {CF_CLIENT, "cl_capturevideo_realtime", "0", "causes video saving to operate in realtime (mostly useful while playing, not while capturing demos), this can produce a much lower quality video due to poor sound/video sync and will abort saving if your machine stalls for over a minute"};
cvar_t cl_capturevideo_fps = {CF_CLIENT | CF_ARCHIVE, "cl_capturevideo_fps", "30", "how many frames per second to save (29.97 for NTSC, 30 for typical PC video, 15 can be useful)"};
cvar_t cl_capturevideo_nameformat = {CF_CLIENT | CF_ARCHIVE, "cl_capturevideo_nameformat", "dpvideo", "prefix for saved videos (the date is encoded using strftime escapes)"};
cvar_t cl_capturevideo_number = {CF_CLIENT | CF_ARCHIVE, "cl_capturevideo_number", "1", "number to append to video filename, incremented each time a capture begins"};
cvar_t cl_capturevideo_ogg = {CF_CLIENT | CF_ARCHIVE, "cl_capturevideo_ogg", "1", "save captured video data as Ogg/Vorbis/Theora streams"};
cvar_t cl_capturevideo_framestep = {CF_CLIENT | CF_ARCHIVE, "cl_capturevideo_framestep", "1", "when set to n >= 1, render n frames to capture one (useful for motion blur like effects)"};
#endif
cvar_t r_letterbox = {CF_CLIENT, "r_letterbox", "0", "reduces vertical height of view to simulate a letterboxed movie effect (can be used by mods for cutscenes)"};
cvar_t r_stereo_separation = {CF_CLIENT, "r_stereo_separation", "4", "separation distance of eyes in the world (negative values are only useful for cross-eyed viewing)"};
cvar_t r_stereo_sidebyside = {CF_CLIENT, "r_stereo_sidebyside", "0", "side by side views for those who can't afford glasses but can afford eye strain (note: use a negative r_stereo_separation if you want cross-eyed viewing)"};
cvar_t r_stereo_horizontal = {CF_CLIENT, "r_stereo_horizontal", "0", "aspect skewed side by side view for special decoder/display hardware"};
cvar_t r_stereo_vertical = {CF_CLIENT, "r_stereo_vertical", "0", "aspect skewed top and bottom view for special decoder/display hardware"};
cvar_t r_stereo_redblue = {CF_CLIENT, "r_stereo_redblue", "0", "red/blue anaglyph stereo glasses (note: most of these glasses are actually red/cyan, try that one too)"};
cvar_t r_stereo_redcyan = {CF_CLIENT, "r_stereo_redcyan", "0", "red/cyan anaglyph stereo glasses, the kind given away at drive-in movies like Creature From The Black Lagoon In 3D"};
cvar_t r_stereo_redgreen = {CF_CLIENT, "r_stereo_redgreen", "0", "red/green anaglyph stereo glasses (for those who don't mind yellow)"};
cvar_t r_stereo_angle = {CF_CLIENT, "r_stereo_angle", "0", "separation angle of eyes (makes the views look different directions, as an example, 90 gives a 90 degree separation where the views are 45 degrees left and 45 degrees right)"};
cvar_t scr_stipple = {CF_CLIENT, "scr_stipple", "0", "interlacing-like stippling of the display"};
cvar_t scr_refresh = {CF_CLIENT, "scr_refresh", "1", "allows you to completely shut off rendering for benchmarking purposes"};
cvar_t scr_screenshot_name_in_mapdir = {CF_CLIENT | CF_ARCHIVE, "scr_screenshot_name_in_mapdir", "0", "if set to 1, screenshots are placed in a subdirectory named like the map they are from"};
cvar_t net_graph = {CF_CLIENT | CF_ARCHIVE, "net_graph", "0", "shows a graph of packet sizes and other information, 0 = off, 1 = show client netgraph, 2 = show client and server netgraphs (when hosting a server)"};
cvar_t cl_demo_mousegrab = {CF_CLIENT, "cl_demo_mousegrab", "0", "Allows reading the mouse input while playing demos. Useful for camera mods developed in csqc. (0: never, 1: always)"};
cvar_t timedemo_screenshotframelist = {CF_CLIENT, "timedemo_screenshotframelist", "", "when performing a timedemo, take screenshots of each frame in this space-separated list - example: 1 201 401"};
cvar_t vid_touchscreen_outlinealpha = {CF_CLIENT, "vid_touchscreen_outlinealpha", "0", "opacity of touchscreen area outlines"};
cvar_t vid_touchscreen_overlayalpha = {CF_CLIENT, "vid_touchscreen_overlayalpha", "0.25", "opacity of touchscreen area icons"};

extern cvar_t sbar_info_pos;
extern cvar_t r_fog_clear;

int jpeg_supported = false;

qbool	scr_initialized;		// ready to draw

qbool scr_loading = false;  // we are in a loading screen

unsigned int        scr_con_current;
static unsigned int scr_con_margin_bottom;

extern int	con_vislines;

extern int cl_punchangle_applied;

static void SCR_ScreenShot_f(cmd_state_t *cmd);
static void R_Envmap_f(cmd_state_t *cmd);

// backend
void R_ClearScreen(qbool fogcolor);

/*
===============================================================================

CENTER PRINTING

===============================================================================
*/

char		scr_centerstring[MAX_INPUTLINE];
float		scr_centertime_start;	// for slow victory printing
float		scr_centertime_off;
int			scr_center_lines;
int			scr_erase_lines;
int			scr_erase_center;
char        scr_infobarstring[MAX_INPUTLINE];
float       scr_infobartime_off;

/*
==============
SCR_CenterPrint

Called for important messages that should stay in the center of the screen
for a few moments
==============
*/
void SCR_CenterPrint(const char *str)
{
	scr_centertime_off = scr_centertime.value;
	scr_centertime_start = cl.time;

	// Print the message to the console and update the scr buffer
	// but only if it's different to the previous message
	if (strcmp(str, scr_centerstring) == 0)
		return;
	dp_strlcpy(scr_centerstring, str, sizeof(scr_centerstring));
	scr_center_lines = 1;
	if (str[0] == '\0') // happens when stepping out of a centreprint trigger on alk1.2 start.bsp
		return;
	Con_CenterPrint(str);

// count the number of lines for centering
	while (*str)
	{
		if (*str == '\n')
			scr_center_lines++;
		str++;
	}
}

/*
============
SCR_Centerprint_f

Print something to the center of the screen using SCR_Centerprint
============
*/
static void SCR_Centerprint_f (cmd_state_t *cmd)
{
	char msg[MAX_INPUTLINE];
	unsigned int i, c, p;

	c = Cmd_Argc(cmd);
	if(c >= 2)
	{
		// Merge all the cprint arguments into one string
		dp_strlcpy(msg, Cmd_Argv(cmd,1), sizeof(msg));
		for(i = 2; i < c; ++i)
		{
			dp_strlcat(msg, " ", sizeof(msg));
			dp_strlcat(msg, Cmd_Argv(cmd, i), sizeof(msg));
		}

		c = (unsigned int)strlen(msg);
		for(p = 0, i = 0; i < c; ++i)
		{
			if(msg[i] == '\\')
			{
				if(msg[i+1] == 'n')
					msg[p++] = '\n';
				else if(msg[i+1] == '\\')
					msg[p++] = '\\';
				else {
					msg[p++] = '\\';
					msg[p++] = msg[i+1];
				}
				++i;
			} else {
				msg[p++] = msg[i];
			}
		}
		msg[p] = '\0';
		SCR_CenterPrint(msg);
	}
}

static void SCR_DrawCenterString (void)
{
	char	*start;
	int		x, y;
	int		remaining;
	int		color;

	if(cl.intermission == 2) // in finale,
		if(sb_showscores) // make TAB hide the finale message (sb_showscores overrides finale in sbar.c)
			return;

	if(scr_centertime.value <= 0 && !cl.intermission)
		return;

// the finale prints the characters one at a time, except if printspeed is an absurdly high value
	if (cl.intermission && scr_printspeed.value > 0 && scr_printspeed.value < 1000000)
		remaining = (int)(scr_printspeed.value * (cl.time - scr_centertime_start));
	else
		remaining = 9999;

	scr_erase_center = 0;
	start = scr_centerstring;

	if (remaining < 1)
		return;

	if (scr_center_lines <= 4)
		y = (int)(vid_conheight.integer*0.35);
	else
		y = 48;

	color = -1;
	do
	{
		// scan the number of characters on the line, not counting color codes
		char *newline = strchr(start, '\n');
		int l = newline ? (newline - start) : (int)strlen(start);
		float width = DrawQ_TextWidth(start, l, 8, 8, false, FONT_CENTERPRINT);

		x = (int) (vid_conwidth.integer - width)/2;
		if (l > 0)
		{
			if (remaining < l)
				l = remaining;
			DrawQ_String(x, y, start, l, 8, 8, 1, 1, 1, 1, 0, &color, false, FONT_CENTERPRINT);
			remaining -= l;
			if (remaining <= 0)
				return;
		}
		y += 8;

		if (!newline)
			break;
		start = newline + 1; // skip the \n
	} while (1);
}

static void SCR_CheckDrawCenterString (void)
{
	if (scr_center_lines > scr_erase_lines)
		scr_erase_lines = scr_center_lines;

	if (cl.time > cl.oldtime)
		scr_centertime_off -= cl.time - cl.oldtime;

	// don't draw if this is a normal stats-screen intermission,
	// only if it is not an intermission, or a finale intermission
	if (cl.intermission == 1)
		return;
	if (scr_centertime_off <= 0 && !cl.intermission)
		return;
	if (key_dest != key_game)
		return;

	SCR_DrawCenterString ();
}

static void SCR_DrawNetGraph_DrawGraph (int graphx, int graphy, int graphwidth, int graphheight, float graphscale, int graphlimit, const char *label, float textsize, int packetcounter, netgraphitem_t *netgraph)
{
	netgraphitem_t *graph;
	int j, x, y;
	int totalbytes = 0;
	char bytesstring[128];
	float g[NETGRAPH_PACKETS][7];
	float *a;
	float *b;
	DrawQ_Fill(graphx, graphy, graphwidth, graphheight + textsize * 2, 0, 0, 0, 0.5, 0);
	// draw the bar graph itself
	memset(g, 0, sizeof(g));
	for (j = 0;j < NETGRAPH_PACKETS;j++)
	{
		graph = netgraph + j;
		g[j][0] = 1.0f - 0.25f * (host.realtime - graph->time);
		g[j][1] = 1.0f;
		g[j][2] = 1.0f;
		g[j][3] = 1.0f;
		g[j][4] = 1.0f;
		g[j][5] = 1.0f;
		g[j][6] = 1.0f;
		if (graph->unreliablebytes == NETGRAPH_LOSTPACKET)
			g[j][1] = 0.00f;
		else if (graph->unreliablebytes == NETGRAPH_CHOKEDPACKET)
			g[j][2] = 0.90f;
		else
		{
			if(netgraph[j].time >= netgraph[(j+NETGRAPH_PACKETS-1)%NETGRAPH_PACKETS].time)
				if(graph->unreliablebytes + graph->reliablebytes + graph->ackbytes >= graphlimit * (netgraph[j].time - netgraph[(j+NETGRAPH_PACKETS-1)%NETGRAPH_PACKETS].time))
					g[j][2] = 0.98f;
			g[j][3] = 1.0f    - graph->unreliablebytes * graphscale;
			g[j][4] = g[j][3] - graph->reliablebytes   * graphscale;
			g[j][5] = g[j][4] - graph->ackbytes        * graphscale;
			// count bytes in the last second
			if (host.realtime - graph->time < 1.0f)
				totalbytes += graph->unreliablebytes + graph->reliablebytes + graph->ackbytes;
		}
		if(graph->cleartime >= 0)
			g[j][6] = 0.5f + 0.5f * (2.0 / M_PI) * atan((M_PI / 2.0) * (graph->cleartime - graph->time));
		g[j][1] = bound(0.0f, g[j][1], 1.0f);
		g[j][2] = bound(0.0f, g[j][2], 1.0f);
		g[j][3] = bound(0.0f, g[j][3], 1.0f);
		g[j][4] = bound(0.0f, g[j][4], 1.0f);
		g[j][5] = bound(0.0f, g[j][5], 1.0f);
		g[j][6] = bound(0.0f, g[j][6], 1.0f);
	}
	// render the lines for the graph
	for (j = 0;j < NETGRAPH_PACKETS;j++)
	{
		a = g[j];
		b = g[(j+1)%NETGRAPH_PACKETS];
		if (a[0] < 0.0f || b[0] > 1.0f || b[0] < a[0])
			continue;
		DrawQ_Line(1, graphx + graphwidth * a[0], graphy + graphheight * a[2], graphx + graphwidth * b[0], graphy + graphheight * b[2], 1.0f, 1.0f, 1.0f, 1.0f, 0);
		DrawQ_Line(1, graphx + graphwidth * a[0], graphy + graphheight * a[1], graphx + graphwidth * b[0], graphy + graphheight * b[1], 1.0f, 0.0f, 0.0f, 1.0f, 0);
		DrawQ_Line(1, graphx + graphwidth * a[0], graphy + graphheight * a[5], graphx + graphwidth * b[0], graphy + graphheight * b[5], 0.0f, 1.0f, 0.0f, 1.0f, 0);
		DrawQ_Line(1, graphx + graphwidth * a[0], graphy + graphheight * a[4], graphx + graphwidth * b[0], graphy + graphheight * b[4], 1.0f, 1.0f, 1.0f, 1.0f, 0);
		DrawQ_Line(1, graphx + graphwidth * a[0], graphy + graphheight * a[3], graphx + graphwidth * b[0], graphy + graphheight * b[3], 1.0f, 0.5f, 0.0f, 1.0f, 0);
		DrawQ_Line(1, graphx + graphwidth * a[0], graphy + graphheight * a[6], graphx + graphwidth * b[0], graphy + graphheight * b[6], 0.0f, 0.0f, 1.0f, 1.0f, 0);
	}
	x = graphx;
	y = graphy + graphheight;
	dpsnprintf(bytesstring, sizeof(bytesstring), "%i", totalbytes);
	DrawQ_String(x, y, label      , 0, textsize, textsize, 1.0f, 1.0f, 1.0f, 1.0f, 0, NULL, false, FONT_DEFAULT);y += textsize;
	DrawQ_String(x, y, bytesstring, 0, textsize, textsize, 1.0f, 1.0f, 1.0f, 1.0f, 0, NULL, false, FONT_DEFAULT);y += textsize;
}

/*
==============
SCR_DrawNetGraph
==============
*/
static void SCR_DrawNetGraph (void)
{
	int i, separator1, separator2, graphwidth, graphheight, netgraph_x, netgraph_y, textsize, index, netgraphsperrow, graphlimit;
	float graphscale;
	netconn_t *c;
	char vabuf[1024];

	if (cls.state != ca_connected)
		return;
	if (!cls.netcon)
		return;
	if (!net_graph.integer)
		return;

	separator1 = 2;
	separator2 = 4;
	textsize = 8;
	graphwidth = 120;
	graphheight = 70;
	graphscale = 1.0f / 1500.0f;
	graphlimit = cl_rate.integer;

	netgraphsperrow = (vid_conwidth.integer + separator2) / (graphwidth * 2 + separator1 + separator2);
	netgraphsperrow = max(netgraphsperrow, 1);

	index = 0;
	netgraph_x = (vid_conwidth.integer + separator2) - (1 + (index % netgraphsperrow)) * (graphwidth * 2 + separator1 + separator2);
	netgraph_y = (vid_conheight.integer - 48 - sbar_info_pos.integer + separator2) - (1 + (index / netgraphsperrow)) * (graphheight + textsize + separator2);
	c = cls.netcon;
	SCR_DrawNetGraph_DrawGraph(netgraph_x                          , netgraph_y, graphwidth, graphheight, graphscale, graphlimit, "incoming", textsize, c->incoming_packetcounter, c->incoming_netgraph);
	SCR_DrawNetGraph_DrawGraph(netgraph_x + graphwidth + separator1, netgraph_y, graphwidth, graphheight, graphscale, graphlimit, "outgoing", textsize, c->outgoing_packetcounter, c->outgoing_netgraph);
	index++;

	if (sv.active && net_graph.integer >= 2)
	{
		for (i = 0;i < svs.maxclients;i++)
		{
			c = svs.clients[i].netconnection;
			if (!c)
				continue;
			netgraph_x = (vid_conwidth.integer + separator2) - (1 + (index % netgraphsperrow)) * (graphwidth * 2 + separator1 + separator2);
			netgraph_y = (vid_conheight.integer - 48 + separator2) - (1 + (index / netgraphsperrow)) * (graphheight + textsize + separator2);
			SCR_DrawNetGraph_DrawGraph(netgraph_x                          , netgraph_y, graphwidth, graphheight, graphscale, graphlimit, va(vabuf, sizeof(vabuf), "%s", svs.clients[i].name), textsize, c->outgoing_packetcounter, c->outgoing_netgraph);
			SCR_DrawNetGraph_DrawGraph(netgraph_x + graphwidth + separator1, netgraph_y, graphwidth, graphheight, graphscale, graphlimit, ""                           , textsize, c->incoming_packetcounter, c->incoming_netgraph);
			index++;
		}
	}
}

/*
==============
SCR_DrawTurtle
==============
*/
static void SCR_DrawTurtle (void)
{
	static int	count;

	if (cls.state != ca_connected)
		return;

	if (!scr_showturtle.integer)
		return;

	if (cl.realframetime < 0.1)
	{
		count = 0;
		return;
	}

	count++;
	if (count < 3)
		return;

	DrawQ_Pic (0, 0, Draw_CachePic ("gfx/turtle"), 0, 0, 1, 1, 1, 1, 0);
}

/*
==============
SCR_DrawNet
==============
*/
static void SCR_DrawNet (void)
{
	if (cls.state != ca_connected)
		return;
	if (host.realtime - cl.last_received_message < 0.3)
		return;
	if (cls.demoplayback)
		return;

	DrawQ_Pic (64, 0, Draw_CachePic ("gfx/net"), 0, 0, 1, 1, 1, 1, 0);
}

/*
==============
DrawPause
==============
*/
static void SCR_DrawPause (void)
{
	cachepic_t	*pic;

	if (cls.state != ca_connected)
		return;

	if (!scr_showpause.integer)		// turn off for screenshots
		return;

	if (!cl.paused)
		return;

	pic = Draw_CachePic ("gfx/pause");
	DrawQ_Pic ((vid_conwidth.integer - Draw_GetPicWidth(pic))/2, (vid_conheight.integer - Draw_GetPicHeight(pic))/2, pic, 0, 0, 1, 1, 1, 1, 0);
}

/*
==============
SCR_DrawBrand
==============
*/
static void SCR_DrawBrand (void)
{
	cachepic_t	*pic;
	float		x, y;

	if (!scr_showbrand.value)
		return;

	pic = Draw_CachePic ("gfx/brand");

	switch ((int)scr_showbrand.value)
	{
	case 1:	// bottom left
		x = 0;
		y = vid_conheight.integer - Draw_GetPicHeight(pic);
		break;
	case 2:	// bottom centre
		x = (vid_conwidth.integer - Draw_GetPicWidth(pic)) / 2;
		y = vid_conheight.integer - Draw_GetPicHeight(pic);
		break;
	case 3:	// bottom right
		x = vid_conwidth.integer - Draw_GetPicWidth(pic);
		y = vid_conheight.integer - Draw_GetPicHeight(pic);
		break;
	case 4:	// centre right
		x = vid_conwidth.integer - Draw_GetPicWidth(pic);
		y = (vid_conheight.integer - Draw_GetPicHeight(pic)) / 2;
		break;
	case 5:	// top right
		x = vid_conwidth.integer - Draw_GetPicWidth(pic);
		y = 0;
		break;
	case 6:	// top centre
		x = (vid_conwidth.integer - Draw_GetPicWidth(pic)) / 2;
		y = 0;
		break;
	case 7:	// top left
		x = 0;
		y = 0;
		break;
	case 8:	// centre left
		x = 0;
		y = (vid_conheight.integer - Draw_GetPicHeight(pic)) / 2;
		break;
	default:
		return;
	}

	DrawQ_Pic (x, y, pic, 0, 0, 1, 1, 1, 1, 0);
}

/*
==============
SCR_DrawQWDownload
==============
*/
static int SCR_DrawQWDownload(int offset)
{
	// sync with SCR_InfobarHeight
	int len;
	float x, y;
	float size = scr_infobar_height.value;
	char temp[256];

	if (!cls.qw_downloadname[0])
	{
		cls.qw_downloadspeedrate = 0;
		cls.qw_downloadspeedtime = host.realtime;
		cls.qw_downloadspeedcount = 0;
		return 0;
	}
	if (host.realtime >= cls.qw_downloadspeedtime + 1)
	{
		cls.qw_downloadspeedrate = cls.qw_downloadspeedcount;
		cls.qw_downloadspeedtime = host.realtime;
		cls.qw_downloadspeedcount = 0;
	}
	if (cls.protocol == PROTOCOL_QUAKEWORLD)
		dpsnprintf(temp, sizeof(temp), "Downloading %s %3i%% (%i) at %i bytes/s", cls.qw_downloadname, cls.qw_downloadpercent, cls.qw_downloadmemorycursize, cls.qw_downloadspeedrate);
	else
		dpsnprintf(temp, sizeof(temp), "Downloading %s %3i%% (%i/%i) at %i bytes/s", cls.qw_downloadname, cls.qw_downloadpercent, cls.qw_downloadmemorycursize, cls.qw_downloadmemorymaxsize, cls.qw_downloadspeedrate);
	len = (int)strlen(temp);
	x = (vid_conwidth.integer - DrawQ_TextWidth(temp, len, size, size, true, FONT_INFOBAR)) / 2;
	y = vid_conheight.integer - size - offset;
	DrawQ_Fill(0, y, vid_conwidth.integer, size, 0, 0, 0, cls.signon == SIGNONS ? 0.5 : 1, 0);
	DrawQ_String(x, y, temp, len, size, size, 1, 1, 1, 1, 0, NULL, true, FONT_INFOBAR);
	return size;
}
/*
==============
SCR_DrawInfobarString
==============
*/
static int SCR_DrawInfobarString(int offset)
{
	int len;
	float x, y;
	float size = scr_infobar_height.value;

	len = (int)strlen(scr_infobarstring);
	x = (vid_conwidth.integer - DrawQ_TextWidth(scr_infobarstring, len, size, size, false, FONT_INFOBAR)) / 2;
	y = vid_conheight.integer - size - offset;
	DrawQ_Fill(0, y, vid_conwidth.integer, size, 0, 0, 0, cls.signon == SIGNONS ? 0.5 : 1, 0);
	DrawQ_String(x, y, scr_infobarstring, len, size, size, 1, 1, 1, 1, 0, NULL, false, FONT_INFOBAR);
	return size;
}

/*
==============
SCR_DrawCurlDownload
==============
*/
static int SCR_DrawCurlDownload(int offset)
{
	// sync with SCR_InfobarHeight
	int len;
	int nDownloads;
	int i;
	float x, y;
	float size = scr_infobar_height.value;
	Curl_downloadinfo_t *downinfo;
	char temp[256];
	char addinfobuf[128];
	const char *addinfo;

	downinfo = Curl_GetDownloadInfo(&nDownloads, &addinfo, addinfobuf, sizeof(addinfobuf));
	if(!downinfo)
		return 0;

	y = vid_conheight.integer - size * nDownloads - offset;

	if(addinfo)
	{
		len = (int)strlen(addinfo);
		x = (vid_conwidth.integer - DrawQ_TextWidth(addinfo, len, size, size, true, FONT_INFOBAR)) / 2;
		DrawQ_Fill(0, y - size, vid_conwidth.integer, size, 1, 1, 1, cls.signon == SIGNONS ? 0.8 : 1, 0);
		DrawQ_String(x, y - size, addinfo, len, size, size, 0, 0, 0, 1, 0, NULL, true, FONT_INFOBAR);
	}

	for(i = 0; i != nDownloads; ++i)
	{
		if(downinfo[i].queued)
			dpsnprintf(temp, sizeof(temp), "Still in queue: %s", downinfo[i].filename);
		else if(downinfo[i].progress <= 0)
			dpsnprintf(temp, sizeof(temp), "Downloading %s ...  ???.?%% @ %.1f KiB/s", downinfo[i].filename, downinfo[i].speed / 1024.0);
		else
			dpsnprintf(temp, sizeof(temp), "Downloading %s ...  %5.1f%% @ %.1f KiB/s", downinfo[i].filename, 100.0 * downinfo[i].progress, downinfo[i].speed / 1024.0);
		len = (int)strlen(temp);
		x = (vid_conwidth.integer - DrawQ_TextWidth(temp, len, size, size, true, FONT_INFOBAR)) / 2;
		DrawQ_Fill(0, y + i * size, vid_conwidth.integer, size, 0, 0, 0, cls.signon == SIGNONS ? 0.5 : 1, 0);
		DrawQ_String(x, y + i * size, temp, len, size, size, 1, 1, 1, 1, 0, NULL, true, FONT_INFOBAR);
	}

	Z_Free(downinfo);

	return size * (nDownloads + (addinfo ? 1 : 0));
}

/*
==============
SCR_DrawInfobar
==============
*/
static void SCR_DrawInfobar(void)
{
	unsigned int offset = 0;
	offset += SCR_DrawQWDownload(offset);
	offset += SCR_DrawCurlDownload(offset);
	if(scr_infobartime_off > 0)
		offset += SCR_DrawInfobarString(offset);
	if(!offset && scr_loading)
		offset = scr_loadingscreen_barheight.integer;
	if(offset != scr_con_margin_bottom)
		Con_DPrintf("broken console margin calculation: %d != %d\n", offset, scr_con_margin_bottom);
}

static int SCR_InfobarHeight(void)
{
	int offset = 0;
	Curl_downloadinfo_t *downinfo;
	const char *addinfo;
	int nDownloads;
	char addinfobuf[128];

	if (cl.time > cl.oldtime)
		scr_infobartime_off -= cl.time - cl.oldtime;
	if(scr_infobartime_off > 0)
		offset += 1;
	if(cls.qw_downloadname[0])
		offset += 1;

	downinfo = Curl_GetDownloadInfo(&nDownloads, &addinfo, addinfobuf, sizeof(addinfobuf));
	if(downinfo)
	{
		offset += (nDownloads + (addinfo ? 1 : 0));
		Z_Free(downinfo);
	}
	offset *= scr_infobar_height.value;

	return offset;
}

/*
==============
SCR_InfoBar_f
==============
*/
static void SCR_InfoBar_f(cmd_state_t *cmd)
{
	if(Cmd_Argc(cmd) == 3)
	{
		scr_infobartime_off = atof(Cmd_Argv(cmd, 1));
		dp_strlcpy(scr_infobarstring, Cmd_Argv(cmd, 2), sizeof(scr_infobarstring));
	}
	else
	{
		Con_Printf("usage:\ninfobar expiretime \"string\"\n");
	}
}
//=============================================================================

/*
==================
SCR_SetUpToDrawConsole
==================
*/
static void SCR_SetUpToDrawConsole (void)
{
#ifdef CONFIG_MENU
	static int framecounter = 0;
#endif

	Con_CheckResize ();

#ifdef CONFIG_MENU
	if (scr_menuforcewhiledisconnected.integer && key_dest == key_game && cls.state == ca_disconnected)
	{
		if (framecounter >= 2)
			MR_ToggleMenu(1);
		else
			framecounter++;
	}
	else
		framecounter = 0;
#endif

	if (scr_conforcewhiledisconnected.integer >= 2 && key_dest == key_game && cls.signon != SIGNONS)
		key_consoleactive |= KEY_CONSOLEACTIVE_FORCED;
	else if (scr_conforcewhiledisconnected.integer >= 1 && key_dest == key_game && cls.signon != SIGNONS && !sv.active)
		key_consoleactive |= KEY_CONSOLEACTIVE_FORCED;
	else
		key_consoleactive &= ~KEY_CONSOLEACTIVE_FORCED;

	// decide on the height of the console
	if (key_consoleactive & KEY_CONSOLEACTIVE_USER)
		scr_con_current = vid_conheight.integer * scr_conheight.value;
	else
		scr_con_current = 0; // none visible
}

/*
==================
SCR_DrawConsole
==================
*/
void SCR_DrawConsole (void)
{
	// infobar and loading progress are not drawn simultaneously
	scr_con_margin_bottom = SCR_InfobarHeight();
	if (!scr_con_margin_bottom && scr_loading)
		scr_con_margin_bottom = scr_loadingscreen_barheight.integer;
	if (key_consoleactive & KEY_CONSOLEACTIVE_FORCED)
	{
		// full screen
		Con_DrawConsole (vid_conheight.integer - scr_con_margin_bottom, true);
	}
	else if (scr_con_current)
		Con_DrawConsole (min(scr_con_current, vid_conheight.integer - scr_con_margin_bottom), false);
	else
		con_vislines = 0;
}

//=============================================================================

/*
=================
SCR_SizeUp_f

Keybinding command
=================
*/
static void SCR_SizeUp_f(cmd_state_t *cmd)
{
	Cvar_SetValueQuick(&scr_viewsize, scr_viewsize.value + 10);
}


/*
=================
SCR_SizeDown_f

Keybinding command
=================
*/
static void SCR_SizeDown_f(cmd_state_t *cmd)
{
	Cvar_SetValueQuick(&scr_viewsize, scr_viewsize.value - 10);
}

#ifdef CONFIG_VIDEO_CAPTURE
void SCR_CaptureVideo_EndVideo(void);
#endif
void CL_Screen_Shutdown(void)
{
#ifdef CONFIG_VIDEO_CAPTURE
	SCR_CaptureVideo_EndVideo();
#endif
}

#ifdef USE_RT_METAL
// Thin wrapper: the probe itself lives in rt_metal.m (it needs the Metal
// objects), and Cmd_AddCommand wants a cmd_state_t handler.
static void RT_Metal_TermProbe_f(cmd_state_t *cmd)
{
	(void)cmd;
	RT_Metal_TermProbe();
}
#endif

void CL_Screen_Init(void)
{
	int i;
	Cvar_RegisterVariable (&scr_fov);
	Cvar_RegisterVariable (&scr_viewsize);
	Cvar_RegisterVariable (&scr_conalpha);
	Cvar_RegisterVariable (&scr_conalphafactor);
	Cvar_RegisterVariable (&scr_conalpha2factor);
	Cvar_RegisterVariable (&scr_conalpha3factor);
	Cvar_RegisterVariable (&scr_conscroll_x);
	Cvar_RegisterVariable (&scr_conscroll_y);
	Cvar_RegisterVariable (&scr_conscroll2_x);
	Cvar_RegisterVariable (&scr_conscroll2_y);
	Cvar_RegisterVariable (&scr_conscroll3_x);
	Cvar_RegisterVariable (&scr_conscroll3_y);
	Cvar_RegisterVariable (&scr_conbrightness);
	Cvar_RegisterVariable (&scr_conforcewhiledisconnected);
	Cvar_RegisterVariable (&scr_conheight);
#ifdef CONFIG_MENU
	Cvar_RegisterVariable (&scr_menuforcewhiledisconnected);
#endif
	Cvar_RegisterVariable (&scr_loadingscreen_background);
	Cvar_RegisterVariable (&scr_loadingscreen_scale);
	Cvar_RegisterVariable (&scr_loadingscreen_scale_base);
	Cvar_RegisterVariable (&scr_loadingscreen_scale_limit);
	Cvar_RegisterVariable (&scr_loadingscreen_picture);
	Cvar_RegisterVariable (&scr_loadingscreen_count);
	Cvar_RegisterVariable (&scr_loadingscreen_firstforstartup);
	Cvar_RegisterVariable (&scr_loadingscreen_barcolor);
	Cvar_RegisterVariable (&scr_loadingscreen_barheight);
	Cvar_RegisterVariable (&scr_loadingscreen_maxfps);
	Cvar_RegisterVariable (&scr_infobar_height);
	Cvar_RegisterVariable (&scr_showram);
	Cvar_RegisterVariable (&scr_showturtle);
	Cvar_RegisterVariable (&scr_showpause);
	Cvar_RegisterVariable (&scr_showbrand);
	Cvar_RegisterVariable (&scr_centertime);
	Cvar_RegisterVariable (&scr_printspeed);
	Cvar_RegisterVariable (&scr_sbarscale);
	Cvar_RegisterVariable (&vid_conwidth);
	Cvar_RegisterVariable (&vid_conheight);
	Cvar_RegisterVariable (&vid_pixelheight);
	Cvar_RegisterVariable (&vid_conwidthauto);
	Cvar_RegisterVariable (&scr_screenshot_jpeg);
	Cvar_RegisterVariable (&scr_screenshot_jpeg_quality);
	Cvar_RegisterVariable (&scr_screenshot_png);
	Cvar_RegisterVariable (&scr_screenshot_gammaboost);
	Cvar_RegisterVariable (&scr_screenshot_name_in_mapdir);
	Cvar_RegisterVariable (&scr_screenshot_alpha);
	Cvar_RegisterVariable (&scr_screenshot_timestamp);
#ifdef CONFIG_VIDEO_CAPTURE
	Cvar_RegisterVariable (&cl_capturevideo);
	Cvar_RegisterVariable (&cl_capturevideo_demo_stop);
	Cvar_RegisterVariable (&cl_capturevideo_printfps);
	Cvar_RegisterVariable (&cl_capturevideo_width);
	Cvar_RegisterVariable (&cl_capturevideo_height);
	Cvar_RegisterVariable (&cl_capturevideo_realtime);
	Cvar_RegisterVariable (&cl_capturevideo_fps);
	Cvar_RegisterVariable (&cl_capturevideo_nameformat);
	Cvar_RegisterVariable (&cl_capturevideo_number);
	Cvar_RegisterVariable (&cl_capturevideo_ogg);
	Cvar_RegisterVariable (&cl_capturevideo_framestep);
#endif
	Cvar_RegisterVariable (&r_letterbox);
	Cvar_RegisterVariable(&r_stereo_separation);
	Cvar_RegisterVariable(&r_stereo_sidebyside);
	Cvar_RegisterVariable(&r_stereo_horizontal);
	Cvar_RegisterVariable(&r_stereo_vertical);
	Cvar_RegisterVariable(&r_stereo_redblue);
	Cvar_RegisterVariable(&r_stereo_redcyan);
	Cvar_RegisterVariable(&r_stereo_redgreen);
	Cvar_RegisterVariable(&r_stereo_angle);
	Cvar_RegisterVariable(&scr_stipple);
	Cvar_RegisterVariable(&scr_refresh);
	Cvar_RegisterVariable(&net_graph);
	Cvar_RegisterVirtual(&net_graph, "shownetgraph");
	Cvar_RegisterVariable(&cl_demo_mousegrab);
	Cvar_RegisterVariable(&timedemo_screenshotframelist);
	Cvar_RegisterVariable(&vid_touchscreen_outlinealpha);
	Cvar_RegisterVariable(&vid_touchscreen_overlayalpha);
	Cvar_RegisterVariable(&r_speeds_graph);
	for (i = 0;i < (int)(sizeof(r_speeds_graph_filter)/sizeof(r_speeds_graph_filter[0]));i++)
		Cvar_RegisterVariable(&r_speeds_graph_filter[i]);
	Cvar_RegisterVariable(&r_speeds_graph_length);
	Cvar_RegisterVariable(&r_speeds_graph_seconds);
	Cvar_RegisterVariable(&r_speeds_graph_x);
	Cvar_RegisterVariable(&r_speeds_graph_y);
	Cvar_RegisterVariable(&r_speeds_graph_width);
	Cvar_RegisterVariable(&r_speeds_graph_height);
	Cvar_RegisterVariable(&r_speeds_graph_maxtimedelta);
	Cvar_RegisterVariable(&r_speeds_graph_maxdefault);
	Cmd_AddCommand(CF_CLIENT, "r_speeds_dump", R_Stats_Dump_f, "print the last completed frame's renderer statistics to the console (add 'all' for zero counters too) -- the cross-backend draw/vertex-count check the METAL.md parity harness uses");
#ifdef USE_RT_METAL
	Cmd_AddCommand(CF_CLIENT, "rt_metal_termprobe", RT_Metal_TermProbe_f, "hash the RT sidecar's shown term buffer and print it -- the METAL.md Phase 5 cross-backend check that the trace produces identical bytes on the GL and Metal paths");
#endif

	// if we want no console, turn it off here too
	if (Sys_CheckParm ("-noconsole"))
		Cvar_SetQuick(&scr_conforcewhiledisconnected, "0");

	Cmd_AddCommand(CF_CLIENT, "cprint", SCR_Centerprint_f, "print something at the screen center");
	Cmd_AddCommand(CF_CLIENT, "sizeup",SCR_SizeUp_f, "increase view size (increases viewsize cvar)");
	Cmd_AddCommand(CF_CLIENT, "sizedown",SCR_SizeDown_f, "decrease view size (decreases viewsize cvar)");
	Cmd_AddCommand(CF_CLIENT, "screenshot",SCR_ScreenShot_f, "takes a screenshot of the next rendered frame");
	Cmd_AddCommand(CF_CLIENT, "envmap", R_Envmap_f, "render a cubemap (skybox) of the current scene");
	Cmd_AddCommand(CF_CLIENT, "infobar", SCR_InfoBar_f, "display a text in the infobar (usage: infobar expiretime string)");

#ifdef CONFIG_VIDEO_CAPTURE
	SCR_CaptureVideo_Ogg_Init();
#endif

	scr_initialized = true;
}

/*
==================
SCR_ScreenShot_f
==================
*/
void SCR_ScreenShot_f(cmd_state_t *cmd)
{
	static int shotnumber;
	static char old_prefix_name[MAX_QPATH];
	char prefix_name[MAX_QPATH];
	char filename[MAX_QPATH];
	unsigned char *buffer1;
	unsigned char *buffer2;
	qbool jpeg = (scr_screenshot_jpeg.integer != 0);
	qbool png = (scr_screenshot_png.integer != 0) && !jpeg;
	char vabuf[1024];

	if (Cmd_Argc(cmd) == 2)
	{
		const char *ext;
		dp_strlcpy(filename, Cmd_Argv(cmd, 1), sizeof(filename));
		ext = FS_FileExtension(filename);
		if (!strcasecmp(ext, "jpg"))
		{
			jpeg = true;
			png = false;
		}
		else if (!strcasecmp(ext, "tga"))
		{
			jpeg = false;
			png = false;
		}
		else if (!strcasecmp(ext, "png"))
		{
			jpeg = false;
			png = true;
		}
		else
		{
			Con_Printf("screenshot: supplied filename must end in .jpg or .tga or .png\n");
			return;
		}
	}
	else if (scr_screenshot_timestamp.integer)
	{
		int shotnumber100;

		// TODO maybe make capturevideo and screenshot use similar name patterns?
		Sys_TimeString(vabuf, sizeof(vabuf), "%Y%m%d%H%M%S");
		if (scr_screenshot_name_in_mapdir.integer && cl.worldbasename[0])
			dpsnprintf(prefix_name, sizeof(prefix_name), "%s/%s%s", cl.worldbasename, scr_screenshot_name.string, vabuf);
		else
			dpsnprintf(prefix_name, sizeof(prefix_name), "%s%s", scr_screenshot_name.string, vabuf);

		// find a file name to save it to
		for (shotnumber100 = 0;shotnumber100 < 100;shotnumber100++)
			if (!FS_SysFileExists(va(vabuf, sizeof(vabuf), "%s/screenshots/%s-%02d.tga", fs_gamedir, prefix_name, shotnumber100))
			 && !FS_SysFileExists(va(vabuf, sizeof(vabuf), "%s/screenshots/%s-%02d.jpg", fs_gamedir, prefix_name, shotnumber100))
			 && !FS_SysFileExists(va(vabuf, sizeof(vabuf), "%s/screenshots/%s-%02d.png", fs_gamedir, prefix_name, shotnumber100)))
				break;
		if (shotnumber100 >= 100)
		{
			Con_Print("Couldn't create the image file - already 100 shots taken this second!\n");
			return;
		}

		dpsnprintf(filename, sizeof(filename), "screenshots/%s-%02d.%s", prefix_name, shotnumber100, jpeg ? "jpg" : png ? "png" : "tga");
	}
	else
	{
		// TODO maybe make capturevideo and screenshot use similar name patterns?
		Sys_TimeString(vabuf, sizeof(vabuf), scr_screenshot_name.string);
		if (scr_screenshot_name_in_mapdir.integer && cl.worldbasename[0])
			dpsnprintf(prefix_name, sizeof(prefix_name), "%s/%s", cl.worldbasename, vabuf);
		else
			dpsnprintf(prefix_name, sizeof(prefix_name), "%s", vabuf);

		// if prefix changed, gamedir or map changed, reset the shotnumber so
		// we scan again
		// FIXME: should probably do this whenever FS_Rescan or something like that occurs?
		if (strcmp(old_prefix_name, prefix_name))
		{
			dpsnprintf(old_prefix_name, sizeof(old_prefix_name), "%s", prefix_name );
			shotnumber = 0;
		}

		// find a file name to save it to
		for (;shotnumber < 1000000;shotnumber++)
			if (!FS_SysFileExists(va(vabuf, sizeof(vabuf), "%s/screenshots/%s%06d.tga", fs_gamedir, prefix_name, shotnumber))
			 && !FS_SysFileExists(va(vabuf, sizeof(vabuf), "%s/screenshots/%s%06d.jpg", fs_gamedir, prefix_name, shotnumber))
			 && !FS_SysFileExists(va(vabuf, sizeof(vabuf), "%s/screenshots/%s%06d.png", fs_gamedir, prefix_name, shotnumber)))
				break;
		if (shotnumber >= 1000000)
		{
			Con_Print("Couldn't create the image file - you already have 1000000 screenshots!\n");
			return;
		}

		dpsnprintf(filename, sizeof(filename), "screenshots/%s%06d.%s", prefix_name, shotnumber, jpeg ? "jpg" : png ? "png" : "tga");

		shotnumber++;
	}

	buffer1 = (unsigned char *)Mem_Alloc(tempmempool, vid.mode.width * vid.mode.height * 4);
	buffer2 = (unsigned char *)Mem_Alloc(tempmempool, vid.mode.width * vid.mode.height * (scr_screenshot_alpha.integer ? 4 : 3));

	if (SCR_ScreenShot (filename, buffer1, buffer2, 0, 0, vid.mode.width, vid.mode.height, false, false, false, jpeg, png, true, scr_screenshot_alpha.integer != 0))
		Con_Printf("Wrote %s\n", filename);
	else
	{
		Con_Printf(CON_ERROR "Unable to write %s\n", filename);
		if(jpeg || png)
		{
			if(SCR_ScreenShot (filename, buffer1, buffer2, 0, 0, vid.mode.width, vid.mode.height, false, false, false, false, false, true, scr_screenshot_alpha.integer != 0))
			{
				dp_strlcpy(filename + strlen(filename) - 3, "tga", 4);
				Con_Printf("Wrote %s\n", filename);
			}
		}
	}

	Mem_Free (buffer1);
	Mem_Free (buffer2);
}

#ifdef CONFIG_VIDEO_CAPTURE
static void SCR_CaptureVideo_BeginVideo(void)
{
	double r, g, b;
	unsigned int i;
	int width = cl_capturevideo_width.integer, height = cl_capturevideo_height.integer;
	char timestring[128];

	if (cls.capturevideo.active)
		return;
	memset(&cls.capturevideo, 0, sizeof(cls.capturevideo));
	// soundrate is figured out on the first SoundFrame

	if(width == 0 && height != 0)
		width = (int) (height * (double)vid.mode.width / ((double)vid.mode.height * vid_pixelheight.value)); // keep aspect
	if(width != 0 && height == 0)
		height = (int) (width * ((double)vid.mode.height * vid_pixelheight.value) / (double)vid.mode.width); // keep aspect

	if(width < 2 || width > vid.mode.width) // can't scale up
		width = vid.mode.width;
	if(height < 2 || height > vid.mode.height) // can't scale up
		height = vid.mode.height;

	// ensure it's all even; if not, scale down a little
	if(width % 1)
		--width;
	if(height % 1)
		--height;

	cls.capturevideo.width = width;
	cls.capturevideo.height = height;
	cls.capturevideo.active = true;
	cls.capturevideo.framerate = bound(1, cl_capturevideo_fps.value, 1001) * bound(1, cl_capturevideo_framestep.integer, 64);
	cls.capturevideo.framestep = cl_capturevideo_framestep.integer;
	cls.capturevideo.soundrate = S_GetSoundRate();
	cls.capturevideo.soundchannels = S_GetSoundChannels();
	cls.capturevideo.startrealtime = host.realtime;
	cls.capturevideo.frame = cls.capturevideo.lastfpsframe = 0;
	cls.capturevideo.starttime = cls.capturevideo.lastfpstime = host.realtime;
	cls.capturevideo.soundsampleframe = 0;
	cls.capturevideo.realtime = cl_capturevideo_realtime.integer != 0;
	cls.capturevideo.outbuffer = (unsigned char *)Mem_Alloc(tempmempool, width * height * 4 + 18); // +18 ?
	Sys_TimeString(timestring, sizeof(timestring), cl_capturevideo_nameformat.string);
	dpsnprintf(cls.capturevideo.basename, sizeof(cls.capturevideo.basename), "video/%s%03i", timestring, cl_capturevideo_number.integer);
	Cvar_SetValueQuick(&cl_capturevideo_number, cl_capturevideo_number.integer + 1);

	// capture demos as fast as possible
	host.restless = !cls.capturevideo.realtime;

	/*
	for (i = 0;i < 256;i++)
	{
		unsigned char j = (unsigned char)bound(0, 255*pow(i/255.0, gamma), 255);
		cls.capturevideo.rgbgammatable[0][i] = j;
		cls.capturevideo.rgbgammatable[1][i] = j;
		cls.capturevideo.rgbgammatable[2][i] = j;
	}
	*/
/*
R = Y + 1.4075 * (Cr - 128);
G = Y + -0.3455 * (Cb - 128) + -0.7169 * (Cr - 128);
B = Y + 1.7790 * (Cb - 128);
Y = R *  .299 + G *  .587 + B *  .114;
Cb = R * -.169 + G * -.332 + B *  .500 + 128.;
Cr = R *  .500 + G * -.419 + B * -.0813 + 128.;
*/

	// identity gamma table
	BuildGammaTable16(1.0f, 1.0f, 1.0f, 0.0f, 1.0f, cls.capturevideo.vidramp, 256);
	BuildGammaTable16(1.0f, 1.0f, 1.0f, 0.0f, 1.0f, cls.capturevideo.vidramp + 256, 256);
	BuildGammaTable16(1.0f, 1.0f, 1.0f, 0.0f, 1.0f, cls.capturevideo.vidramp + 256*2, 256);
	if(scr_screenshot_gammaboost.value != 1)
	{
		double igamma = 1 / scr_screenshot_gammaboost.value;
		for (i = 0;i < 256 * 3;i++)
			cls.capturevideo.vidramp[i] = (unsigned short) (0.5 + pow(cls.capturevideo.vidramp[i] * (1.0 / 65535.0), igamma) * 65535.0);
	}

	for (i = 0;i < 256;i++)
	{
		r = 255*cls.capturevideo.vidramp[i]/65535.0;
		g = 255*cls.capturevideo.vidramp[i+256]/65535.0;
		b = 255*cls.capturevideo.vidramp[i+512]/65535.0;
		// NOTE: we have to round DOWN here, or integer overflows happen. Sorry for slightly wrong looking colors sometimes...
		// Y weights from RGB
		cls.capturevideo.rgbtoyuvscaletable[0][0][i] = (short)(r *  0.299);
		cls.capturevideo.rgbtoyuvscaletable[0][1][i] = (short)(g *  0.587);
		cls.capturevideo.rgbtoyuvscaletable[0][2][i] = (short)(b *  0.114);
		// Cb weights from RGB
		cls.capturevideo.rgbtoyuvscaletable[1][0][i] = (short)(r * -0.169);
		cls.capturevideo.rgbtoyuvscaletable[1][1][i] = (short)(g * -0.332);
		cls.capturevideo.rgbtoyuvscaletable[1][2][i] = (short)(b *  0.500);
		// Cr weights from RGB
		cls.capturevideo.rgbtoyuvscaletable[2][0][i] = (short)(r *  0.500);
		cls.capturevideo.rgbtoyuvscaletable[2][1][i] = (short)(g * -0.419);
		cls.capturevideo.rgbtoyuvscaletable[2][2][i] = (short)(b * -0.0813);
		// range reduction of YCbCr to valid signal range
		cls.capturevideo.yuvnormalizetable[0][i] = 16 + i * (236-16) / 256;
		cls.capturevideo.yuvnormalizetable[1][i] = 16 + i * (240-16) / 256;
		cls.capturevideo.yuvnormalizetable[2][i] = 16 + i * (240-16) / 256;
	}

	GL_CaptureVideo_BeginVideo();

	if (cl_capturevideo_ogg.integer)
	{
		if(SCR_CaptureVideo_Ogg_Available())
		{
			SCR_CaptureVideo_Ogg_BeginVideo();
			return;
		}
		else
			Con_Print("cl_capturevideo_ogg: libraries not available. Capturing in AVI instead.\n");
	}

	SCR_CaptureVideo_Avi_BeginVideo();
}

void SCR_CaptureVideo_EndVideo(void)
{
	if (!cls.capturevideo.active)
		return;
	cls.capturevideo.active = false;
	host.restless = false;

	Con_Printf("Finishing capture of %s.%s (%d frames, %d audio frames)\n", cls.capturevideo.basename, cls.capturevideo.formatextension, cls.capturevideo.frame, cls.capturevideo.soundsampleframe);

	GL_CaptureVideo_EndVideo(); // must be called before writeEndVideo !

	if (cls.capturevideo.videofile)
		cls.capturevideo.writeEndVideo();

	if (cls.capturevideo.outbuffer)
	{
		Mem_Free (cls.capturevideo.outbuffer);
		cls.capturevideo.outbuffer = NULL;
	}

	// If demo capture failed don't leave the demo playing.
	// CL_StopPlayback shuts down when demo capture finishes successfully.
	if (cls.capturevideo.error && Sys_CheckParm("-capturedemo"))
		host.state = host_shutdown;

	memset(&cls.capturevideo, 0, sizeof(cls.capturevideo));
}

void SCR_CaptureVideo_SoundFrame(const portable_sampleframe_t *paintbuffer, size_t length)
{
	cls.capturevideo.soundsampleframe += (int)length;
	cls.capturevideo.writeSoundFrame(paintbuffer, length);
}

static void SCR_CaptureVideo(void)
{
	int newframenum;
	int newframestepframenum;

	if (cl_capturevideo.integer)
	{
		if (!cls.capturevideo.active)
			SCR_CaptureVideo_BeginVideo();
		if (cls.capturevideo.error)
		{
			// specific error message was printed already
			Cvar_SetValueQuick(&cl_capturevideo, 0);
			SCR_CaptureVideo_EndVideo();
			return;
		}

		if (cls.capturevideo.framerate != cl_capturevideo_fps.value * cl_capturevideo_framestep.integer)
		{
			Con_Printf(CON_WARN "You can not change the video framerate while recording a video.\n");
			Cvar_SetValueQuick(&cl_capturevideo_fps, cls.capturevideo.framerate / (double) cl_capturevideo_framestep.integer);
		}
		// for AVI saving we have to make sure that sound is saved before video
		if (cls.capturevideo.soundrate && !cls.capturevideo.soundsampleframe)
			return;
		if (cls.capturevideo.realtime)
		{
			// preserve sound sync by duplicating frames when running slow
			newframenum = (int)((host.realtime - cls.capturevideo.startrealtime) * cls.capturevideo.framerate);
		}
		else
			newframenum = cls.capturevideo.frame + 1;
		// if falling behind more than one second, stop
		if (newframenum - cls.capturevideo.frame > 60 * (int)ceil(cls.capturevideo.framerate))
		{
			Cvar_SetValueQuick(&cl_capturevideo, 0);
			Con_Printf(CON_ERROR "video saving failed on frame %i, your machine is too slow for this capture speed.\n", cls.capturevideo.frame);
			SCR_CaptureVideo_EndVideo();
			return;
		}
		// write frames
		newframestepframenum = newframenum / cls.capturevideo.framestep;
		if (newframestepframenum != cls.capturevideo.framestepframe)
			GL_CaptureVideo_VideoFrame(newframestepframenum);
		cls.capturevideo.framestepframe = newframestepframenum;
		// report progress
		if(cl_capturevideo_printfps.value && host.realtime > cls.capturevideo.lastfpstime + cl_capturevideo_printfps.value)
		{
			double fps1 = (cls.capturevideo.frame - cls.capturevideo.lastfpsframe) / (host.realtime - cls.capturevideo.lastfpstime + 0.0000001);
			double fps  = (cls.capturevideo.frame                                ) / (host.realtime - cls.capturevideo.starttime   + 0.0000001);
			Sys_Printf("captured %.1fs of video, last second %.3ffps (%.1fx), total %.3ffps (%.1fx)\n",
					cls.capturevideo.frame / cls.capturevideo.framerate,
					fps1, fps1 / cls.capturevideo.framerate,
					fps, fps / cls.capturevideo.framerate);
			cls.capturevideo.lastfpstime = host.realtime;
			cls.capturevideo.lastfpsframe = cls.capturevideo.frame;
		}
		cls.capturevideo.frame = newframenum;
		if (cls.capturevideo.error)
		{
			Cvar_SetValueQuick(&cl_capturevideo, 0);
			Con_Printf(CON_ERROR "video saving failed on frame %i, out of disk space? stopping video capture.\n", cls.capturevideo.frame);
			SCR_CaptureVideo_EndVideo();
		}
	}
	else if (cls.capturevideo.active)
		SCR_CaptureVideo_EndVideo();
}
#endif

/*
===============
R_Envmap_f

Grab six views for environment mapping tests
===============
*/
struct envmapinfo_s
{
	float angles[3];
	const char *name;
	qbool flipx, flipy, flipdiagonaly;
}
envmapinfo[12] =
{
	{{  0,   0, 0}, "rt", false, false, false},
	{{  0, 270, 0}, "ft", false, false, false},
	{{  0, 180, 0}, "lf", false, false, false},
	{{  0,  90, 0}, "bk", false, false, false},
	{{-90, 180, 0}, "up",  true,  true, false},
	{{ 90, 180, 0}, "dn",  true,  true, false},

	{{  0,   0, 0}, "px",  true,  true,  true},
	{{  0,  90, 0}, "py", false,  true, false},
	{{  0, 180, 0}, "nx", false, false,  true},
	{{  0, 270, 0}, "ny",  true, false, false},
	{{-90, 180, 0}, "pz", false, false,  true},
	{{ 90, 180, 0}, "nz", false, false,  true}
};

static void R_Envmap_f(cmd_state_t *cmd)
{
	int j, size;
	char filename[MAX_QPATH], basename[MAX_QPATH];
	unsigned char *buffer1;
	unsigned char *buffer2;
	r_rendertarget_t *rt;

	if (Cmd_Argc(cmd) != 3)
	{
		Con_Print("envmap <basename> <size>: save out 6 cubic environment map images, usable with loadsky, note that size must one of 128, 256, 512, or 1024 and can't be bigger than your current resolution\n");
		return;
	}

	if(cls.state != ca_connected) {
		Con_Printf("envmap: No map loaded\n");
		return;
	}

	dp_strlcpy (basename, Cmd_Argv(cmd, 1), sizeof (basename));
	size = atoi(Cmd_Argv(cmd, 2));
	if (size != 128 && size != 256 && size != 512 && size != 1024)
	{
		Con_Print("envmap: size must be one of 128, 256, 512, or 1024\n");
		return;
	}
	if (size > vid.mode.width || size > vid.mode.height)
	{
		Con_Print("envmap: your resolution is not big enough to render that size\n");
		return;
	}

	r_refdef.envmap = true;

	R_UpdateVariables();

	r_refdef.view.x = 0;
	r_refdef.view.y = 0;
	r_refdef.view.z = 0;
	r_refdef.view.width = size;
	r_refdef.view.height = size;
	r_refdef.view.depth = 1;
	r_refdef.view.useperspective = true;
	r_refdef.view.isoverlay = false;
	r_refdef.view.ismain = true;

	r_refdef.view.frustum_x = 1; // tan(45 * M_PI / 180.0);
	r_refdef.view.frustum_y = 1; // tan(45 * M_PI / 180.0);
	r_refdef.view.ortho_x = 90; // abused as angle by VM_CL_R_SetView
	r_refdef.view.ortho_y = 90; // abused as angle by VM_CL_R_SetView

	buffer1 = (unsigned char *)Mem_Alloc(tempmempool, size * size * 4);
	buffer2 = (unsigned char *)Mem_Alloc(tempmempool, size * size * 3);

	// TODO: use TEXTYPE_COLORBUFFER16F and output to .exr files as well?
	rt = R_RenderTarget_Get(size, size, TEXTYPE_DEPTHBUFFER24STENCIL8, true, TEXTYPE_COLORBUFFER, TEXTYPE_UNUSED, TEXTYPE_UNUSED, TEXTYPE_UNUSED);
	CL_UpdateEntityShading();
	for (j = 0;j < 12;j++)
	{
		dpsnprintf(filename, sizeof(filename), "env/%s%s.tga", basename, envmapinfo[j].name);
		Matrix4x4_CreateFromQuakeEntity(&r_refdef.view.matrix, r_refdef.view.origin[0], r_refdef.view.origin[1], r_refdef.view.origin[2], envmapinfo[j].angles[0], envmapinfo[j].angles[1], envmapinfo[j].angles[2], 1);
		r_refdef.view.quality = 1;
		r_refdef.view.clear = true;
		R_Mesh_Start();
		R_RenderView(rt->fbo, rt->depthtexture, rt->colortexture[0], 0, 0, size, size);
		R_Mesh_Finish();
		SCR_ScreenShot(filename, buffer1, buffer2, 0, vid.mode.height - (r_refdef.view.y + r_refdef.view.height), size, size, envmapinfo[j].flipx, envmapinfo[j].flipy, envmapinfo[j].flipdiagonaly, false, false, false, false);
	}

	Mem_Free (buffer1);
	Mem_Free (buffer2);

	r_refdef.envmap = false;
}

//=============================================================================

void SHOWLMP_decodehide(void)
{
	int i;
	char *lmplabel;
	lmplabel = MSG_ReadString(&cl_message, cl_readstring, sizeof(cl_readstring));
	for (i = 0;i < cl.num_showlmps;i++)
		if (cl.showlmps[i].isactive && strcmp(cl.showlmps[i].label, lmplabel) == 0)
		{
			cl.showlmps[i].isactive = false;
			return;
		}
}

void SHOWLMP_decodeshow(void)
{
	int k;
	char lmplabel[256], picname[256];
	float x, y;
	dp_strlcpy (lmplabel,MSG_ReadString(&cl_message, cl_readstring, sizeof(cl_readstring)), sizeof (lmplabel));
	dp_strlcpy (picname, MSG_ReadString(&cl_message, cl_readstring, sizeof(cl_readstring)), sizeof (picname));
	if (gamemode == GAME_NEHAHRA) // LadyHavoc: nasty old legacy junk
	{
		x = MSG_ReadByte(&cl_message);
		y = MSG_ReadByte(&cl_message);
	}
	else
	{
		x = MSG_ReadShort(&cl_message);
		y = MSG_ReadShort(&cl_message);
	}
	if (!cl.showlmps || cl.num_showlmps >= cl.max_showlmps)
	{
		showlmp_t *oldshowlmps = cl.showlmps;
		cl.max_showlmps += 16;
		cl.showlmps = (showlmp_t *) Mem_Alloc(cls.levelmempool, cl.max_showlmps * sizeof(showlmp_t));
		if (oldshowlmps)
		{
			if (cl.num_showlmps)
				memcpy(cl.showlmps, oldshowlmps, cl.num_showlmps * sizeof(showlmp_t));
			Mem_Free(oldshowlmps);
		}
	}
	for (k = 0;k < cl.max_showlmps;k++)
		if (cl.showlmps[k].isactive && !strcmp(cl.showlmps[k].label, lmplabel))
			break;
	if (k == cl.max_showlmps)
		for (k = 0;k < cl.max_showlmps;k++)
			if (!cl.showlmps[k].isactive)
				break;
	cl.showlmps[k].isactive = true;
	dp_strlcpy (cl.showlmps[k].label, lmplabel, sizeof (cl.showlmps[k].label));
	dp_strlcpy (cl.showlmps[k].pic, picname, sizeof (cl.showlmps[k].pic));
	cl.showlmps[k].x = x;
	cl.showlmps[k].y = y;
	cl.num_showlmps = max(cl.num_showlmps, k + 1);
}

void SHOWLMP_drawall(void)
{
	int i;
	for (i = 0;i < cl.num_showlmps;i++)
		if (cl.showlmps[i].isactive)
			DrawQ_Pic(cl.showlmps[i].x, cl.showlmps[i].y, Draw_CachePic_Flags (cl.showlmps[i].pic, CACHEPICFLAG_NOTPERSISTENT), 0, 0, 1, 1, 1, 1, 0);
}

/*
==============================================================================

						SCREEN SHOTS

==============================================================================
*/

// buffer1: 4*w*h
// buffer2: 3*w*h (or 4*w*h if screenshotting alpha too)
qbool SCR_ScreenShot(char *filename, unsigned char *buffer1, unsigned char *buffer2, int x, int y, int width, int height, qbool flipx, qbool flipy, qbool flipdiagonal, qbool jpeg, qbool png, qbool gammacorrect, qbool keep_alpha)
{
	int	indices[4] = {0,1,2,3}; // BGRA
	qbool ret;

	GL_ReadPixelsBGRA(x, y, width, height, buffer1);

	if(gammacorrect && (scr_screenshot_gammaboost.value != 1))
	{
		int i;
		double igamma = 1.0 / scr_screenshot_gammaboost.value;
		unsigned short vidramp[256 * 3];
		// identity gamma table
		BuildGammaTable16(1.0f, 1.0f, 1.0f, 0.0f, 1.0f, vidramp, 256);
		BuildGammaTable16(1.0f, 1.0f, 1.0f, 0.0f, 1.0f, vidramp + 256, 256);
		BuildGammaTable16(1.0f, 1.0f, 1.0f, 0.0f, 1.0f, vidramp + 256*2, 256);
		if(scr_screenshot_gammaboost.value != 1)
		{
			for (i = 0;i < 256 * 3;i++)
				vidramp[i] = (unsigned short) (0.5 + pow(vidramp[i] * (1.0 / 65535.0), igamma) * 65535.0);
		}
		for (i = 0;i < width*height*4;i += 4)
		{
			buffer1[i] = (unsigned char) (vidramp[buffer1[i] + 512] * 255.0 / 65535.0 + 0.5); // B
			buffer1[i+1] = (unsigned char) (vidramp[buffer1[i+1] + 256] * 255.0 / 65535.0 + 0.5); // G
			buffer1[i+2] = (unsigned char) (vidramp[buffer1[i+2]] * 255.0 / 65535.0 + 0.5); // R
			// A
		}
	}

	if(keep_alpha && !jpeg)
	{
		if(!png)
			flipy = !flipy; // TGA: not preflipped
		Image_CopyMux (buffer2, buffer1, width, height, flipx, flipy, flipdiagonal, 4, 4, indices);
		if (png)
			ret = PNG_SaveImage_preflipped (filename, width, height, true, buffer2);
		else
			ret = Image_WriteTGABGRA(filename, width, height, buffer2);
	}
	else
	{
		if(jpeg)
		{
			indices[0] = 2;
			indices[2] = 0; // RGB
		}
		Image_CopyMux (buffer2, buffer1, width, height, flipx, flipy, flipdiagonal, 3, 4, indices);
		if (jpeg)
			ret = JPEG_SaveImage_preflipped (filename, width, height, buffer2);
		else if (png)
			ret = PNG_SaveImage_preflipped (filename, width, height, false, buffer2);
		else
			ret = Image_WriteTGABGR_preflipped (filename, width, height, buffer2);
	}

	return ret;
}

//=============================================================================

int scr_numtouchscreenareas;
scr_touchscreenarea_t scr_touchscreenareas[128];

static void SCR_DrawTouchscreenOverlay(void)
{
	int i;
	scr_touchscreenarea_t *a;
	cachepic_t *pic;
	for (i = 0, a = scr_touchscreenareas;i < scr_numtouchscreenareas;i++, a++)
	{
		if (vid_touchscreen_outlinealpha.value > 0 && a->rect[0] >= 0 && a->rect[1] >= 0 && a->rect[2] >= 4 && a->rect[3] >= 4)
		{
			DrawQ_Fill(a->rect[0] +              2, a->rect[1]                 , a->rect[2] - 4,          1    , 1, 1, 1, vid_touchscreen_outlinealpha.value * (0.5f + 0.5f * a->active), 0);
			DrawQ_Fill(a->rect[0] +              1, a->rect[1] +              1, a->rect[2] - 2,          1    , 1, 1, 1, vid_touchscreen_outlinealpha.value * (0.5f + 0.5f * a->active), 0);
			DrawQ_Fill(a->rect[0]                 , a->rect[1] +              2,          2    , a->rect[3] - 2, 1, 1, 1, vid_touchscreen_outlinealpha.value * (0.5f + 0.5f * a->active), 0);
			DrawQ_Fill(a->rect[0] + a->rect[2] - 2, a->rect[1] +              2,          2    , a->rect[3] - 2, 1, 1, 1, vid_touchscreen_outlinealpha.value * (0.5f + 0.5f * a->active), 0);
			DrawQ_Fill(a->rect[0] +              1, a->rect[1] + a->rect[3] - 2, a->rect[2] - 2,          1    , 1, 1, 1, vid_touchscreen_outlinealpha.value * (0.5f + 0.5f * a->active), 0);
			DrawQ_Fill(a->rect[0] +              2, a->rect[1] + a->rect[3] - 1, a->rect[2] - 4,          1    , 1, 1, 1, vid_touchscreen_outlinealpha.value * (0.5f + 0.5f * a->active), 0);
		}
		pic = a->pic ? Draw_CachePic_Flags(a->pic, CACHEPICFLAG_FAILONMISSING) : NULL;
		if (Draw_IsPicLoaded(pic))
			DrawQ_Pic(a->rect[0], a->rect[1], pic, a->rect[2], a->rect[3], 1, 1, 1, vid_touchscreen_overlayalpha.value * (0.5f + 0.5f * a->active), 0);
		if (a->text && a->text[0])
		{
			int textwidth = DrawQ_TextWidth(a->text, 0, a->textheight, a->textheight, false, FONT_CHAT);
			DrawQ_String(a->rect[0] + (a->rect[2] - textwidth) * 0.5f, a->rect[1] + (a->rect[3] - a->textheight) * 0.5f, a->text, 0, a->textheight, a->textheight, 1.0f, 1.0f, 1.0f, vid_touchscreen_overlayalpha.value, 0, NULL, false, FONT_CHAT);
		}
	}
}

void R_ClearScreen(qbool fogcolor)
{
	float clearcolor[4];
	if (scr_screenshot_alpha.integer)
		// clear to transparency (so png screenshots can contain alpha channel, useful for building model pictures)
		Vector4Set(clearcolor, 0.0f, 0.0f, 0.0f, 0.0f);
	else
		// clear to opaque black (if we're being composited it might otherwise render as transparent)
		Vector4Set(clearcolor, 0.0f, 0.0f, 0.0f, 1.0f);
	if (fogcolor && r_fog_clear.integer)
	{
		R_UpdateFog();
		VectorCopy(r_refdef.fogcolor, clearcolor);
	}
	// clear depth is 1.0
	// clear the screen
	GL_Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | (vid.stencil ? GL_STENCIL_BUFFER_BIT : 0), clearcolor, 1.0f, 0);
}

int r_stereo_side;
extern cvar_t v_isometric;
extern cvar_t v_isometric_verticalfov;

typedef struct loadingscreenstack_s
{
	struct loadingscreenstack_s *prev;
	char msg[MAX_QPATH];
	float absolute_loading_amount_min; // this corresponds to relative completion 0 of this item
	float absolute_loading_amount_len; // this corresponds to relative completion 1 of this item
	float relative_completion; // 0 .. 1
}
loadingscreenstack_t;
static loadingscreenstack_t *loadingscreenstack = NULL;
rtexture_t *loadingscreentexture = NULL; // last framebuffer before loading screen, kept for the background
static float loadingscreentexture_vertex3f[12];
static float loadingscreentexture_texcoord2f[8];
static int loadingscreenpic_number = 0;
/// User-friendly connection status for the menu and/or loading screen,
/// colours and \n not supported.
char cl_connect_status[MAX_QPATH]; // should match size of loadingscreenstack_t msg[]

static void SCR_DrawLoadingScreen(void);

#ifdef USE_RT_METAL
// Milestone 5: gather this frame's dynamic shadow-casting entities (monsters,
// items, rockets, moving doors, ...) as WORLD-SPACE triangles for the Metal RT
// sidecar, so they cast soft shadows from the map's real lights — the thing baked
// lightmaps cannot do. The local player's own first-person gun (RENDER_VIEWMODEL)
// and body (RENDER_EXTERIORMODEL) are excluded, because a self-shadow is visually
// distracting. Only entities the engine already marks as shadow casters
// (RENDER_SHADOW) with solid geometry (model->DrawShadowMap) are included. Must
// run while the animcache is valid (it is, at the RT hook below: R_FrameData is
// reset at the top of CL_UpdateScreen, before R_RenderView fills the cache).
#define RT_ENT_MAXVERTS  (1 << 18)   // compact output verts  (~3 MB scratch)
#define RT_ENT_MAXTRIS   (1 << 18)   // output triangles      (~3 MB scratch)
#define RT_ENT_MAXREMAP  (1 << 20)   // >= any single model's vertex count (brush submodels share the whole-BSP surfmesh)
#define RT_LC_MAXVERTS   (1 << 15)   // light-core stream (flame/candle models; ~780 KB scratch --
#define RT_LC_MAXTRIS    (1 << 15)   // sized for Arcane Dimensions' candle halls, not id1's torches)
static float rt_ent_verts[RT_ENT_MAXVERTS * 3];
static float rt_ent_norms[RT_ENT_MAXVERTS * 3];  // world-space unit vertex normals (smooth shading)
static int   rt_ent_tris[RT_ENT_MAXTRIS * 3];
static float rt_lc_verts[RT_LC_MAXVERTS * 3];    // LIGHT-CORE stream (see RT_IsLightCoreModel)
static int   rt_lc_tris[RT_LC_MAXTRIS * 3];
static int   rt_ent_remap[RT_ENT_MAXREMAP];      // model vertex index -> compact output vertex index
static int   rt_ent_remapgen[RT_ENT_MAXREMAP];   // generation tag, so we skip a per-entity memset
static int   rt_ent_gen;

// Torch/brazier flame models sit exactly on their light's origin, so as ordinary
// shadow casters they blocked their OWN light's disc-jittered shadow rays (torch
// glow traced to black) and, under wall lighting, that self-shadowed term greyed
// the flame's emissive skin. They go to a separate TLAS instance instead: shadow
// rays skip it, and the surface kernel shades its hits as emissive (term 1.0),
// so the flame renders exactly as authored.
static int RT_IsLightCoreModel(const model_t *model)
{
	// id1's torches plus Arcane Dimensions' fixtures. AD reuses the stock
	// flame.mdl/flame2.mdl NAMES for its wall torches (its paks carry their own
	// copies), so those two rows cover both games; the rest are AD's own
	// braziers, candles and hanging lantern, every one of which sits on its
	// light's origin exactly like the stock torches do.
	static const char *corenames[] = {
		"progs/flame.mdl", "progs/flame2.mdl",
		"progs/misc_flame_big.mdl", "progs/misc_flame_med.mdl",
		"progs/misc_candle1.mdl", "progs/misc_candle2.mdl", "progs/misc_candle3.mdl",
		"progs/misc_lantern.mdl",
	};
	size_t i;
	if (!rt_metal_lightcores.integer)
		return false;
	for (i = 0; i < sizeof(corenames) / sizeof(corenames[0]); i++)
		if (!strcmp(model->name, corenames[i]))
			return true;
	return false;
}

static int RT_GatherEntityShadowCasters(const float **out_verts, int *out_numverts, const int **out_tris, const float **out_norms,
                                        const float **out_lcverts, int *out_lcnumverts, const int **out_lctris, int *out_lcnumtris)
{
	int nv = 0, nt = 0;
	int lnv = 0, lnt = 0;
	int i;
	// smooth shading wants the model's vertex normals alongside the positions;
	// the render's own animcache pass has already computed them for every
	// visible entity, so asking again is a cache hit
	int wantnorms = rt_metal_smoothnormals.integer != 0;
	// the light-core outs are plain out-params (unlike the caster stream, whose
	// count is the return value), so they must be valid on EVERY path, including
	// the early bail below
	*out_lcverts = rt_lc_verts;
	*out_lctris = rt_lc_tris;
	*out_lcnumverts = 0;
	*out_lcnumtris = 0;
	// entityvisible[] is (re)sized only by R_RenderView; if the view wasn't
	// rendered this frame (e.g. a loading-plaque frame) it may be unset — bail
	// rather than dereference it.
	if (!r_refdef.viewcache.entityvisible)
		return 0;
	for (i = 0; i < r_refdef.scene.numentities; i++)
	{
		entity_render_t *ent = r_refdef.scene.entities[i];
		model_t *model;
		const float *src;
		const float *nsrc;
		const int *elements;
		int s;
		if (!r_refdef.viewcache.entityvisible[i])
			continue;                                       // only entities visible this view
		model = ent->model;
		if (!model || !model->DrawShadowMap)
			continue;                                       // solid geometry only (rejects sprites/particles)
		if (!(ent->flags & RENDER_SHADOW))
			continue;                                       // engine's cast-shadow opt-in (also excludes the gun)
		if (ent->flags & (RENDER_VIEWMODEL | RENDER_EXTERIORMODEL))
			continue;                                       // exclude the local player's own gun + body
		if (model->surfmesh.num_triangles < 1 || model->surfmesh.num_vertices < 1)
			continue;
		if (model->surfmesh.num_vertices > RT_ENT_MAXREMAP)
			continue;                                       // safety: model larger than the remap table

		// Bake animated (alias) verts; no-op for static/brush models (leaves
		// animcache_vertex3f NULL, so we fall back to the base model-space mesh).
		R_AnimCache_GetEntity(ent, wantnorms ? true : false, false);
		src = ent->animcache_vertex3f ? ent->animcache_vertex3f : model->surfmesh.data_vertex3f;
		nsrc = wantnorms ? (ent->animcache_normal3f ? ent->animcache_normal3f : model->surfmesh.data_normal3f) : NULL;
		elements = model->surfmesh.data_element3i;
		if (!src || !elements)
			continue;

		// New generation invalidates the shared remap table, so each referenced
		// vertex is transformed into world space at most once for this entity.
		rt_ent_gen++;

		// Stream selection: light-core models (flames) bypass the caster stream.
		// With rt_metal_lightcores 0 every entity resolves to the caster-stream
		// pointers and the emit below is exactly the old single-stream path.
		{
			int lc = RT_IsLightCoreModel(model);
			float *overts = lc ? rt_lc_verts : rt_ent_verts;
			float *onorms = (!lc && wantnorms) ? rt_ent_norms : NULL;
			int *otris = lc ? rt_lc_tris : rt_ent_tris;
			int *pnv = lc ? &lnv : &nv;
			int *pnt = lc ? &lnt : &nt;
			int maxv = lc ? RT_LC_MAXVERTS : RT_ENT_MAXVERTS;
			int maxt = lc ? RT_LC_MAXTRIS : RT_ENT_MAXTRIS;

			// Emit only THIS (sub)model's surface range. Brush submodels (doors, plats)
			// share the whole-BSP surfmesh, so iterating all of surfmesh would emit the
			// entire map; for alias models the range is the whole model, so it is uniform.
			for (s = model->submodelsurfaces_start; s < model->submodelsurfaces_end; s++)
			{
				const msurface_t *surf = model->data_surfaces + model->modelsurfaces_sorted[s];
				const int *e = elements + surf->num_firsttriangle * 3;
				int t;
				// skip invisible / non-shadowing faces (a brush submodel like a door can
				// carry caulk/nodraw backfaces); matches the world path and the engine's
				// own shadow-caster selection. Normal alias-model (monster) skins are
				// MATERIALFLAG_WALL, so they are kept and still cast.
				if (surf->texture && (surf->texture->basematerialflags & (MATERIALFLAG_NODRAW | MATERIALFLAG_NOSHADOW)))
					continue;
				for (t = 0; t < surf->num_triangles; t++, e += 3)
				{
					int tri[3];
					int k, ok = 1;
					for (k = 0; k < 3; k++)
					{
						int vi = e[k];
						if (vi < 0 || vi >= model->surfmesh.num_vertices) { ok = 0; break; }
						if (rt_ent_remapgen[vi] != rt_ent_gen)
						{
							if (*pnv >= maxv) { ok = 0; break; }
							Matrix4x4_Transform(&ent->matrix, src + vi * 3, overts + *pnv * 3);
							if (onorms)
							{
								// rotation-only transform (entity scale is uniform in
								// Quake, so renormalising absorbs it); models without
								// normals leave zeros and the kernel falls back per pixel
								if (nsrc)
								{
									Matrix4x4_Transform3x3(&ent->matrix, nsrc + vi * 3, onorms + *pnv * 3);
									VectorNormalize(onorms + *pnv * 3);
								}
								else
									VectorClear(onorms + *pnv * 3);
							}
							rt_ent_remap[vi] = *pnv;
							rt_ent_remapgen[vi] = rt_ent_gen;
							(*pnv)++;
						}
						tri[k] = rt_ent_remap[vi];
					}
					if (!ok || *pnt >= maxt)
						break;                                  // out of scratch: stop emitting
					otris[*pnt * 3 + 0] = tri[0];
					otris[*pnt * 3 + 1] = tri[1];
					otris[*pnt * 3 + 2] = tri[2];
					(*pnt)++;
				}
				if (*pnv >= maxv || *pnt >= maxt)
					break;
			}
		}
		// only a full CASTER scratch stops the gather outright; a full light-core
		// scratch (tiny by design) must not cost monsters their shadows
		if (nv >= RT_ENT_MAXVERTS || nt >= RT_ENT_MAXTRIS)
			break;
	}
	// Truncation is otherwise SILENT (triangles just stop being emitted), so say
	// so once per process. First-event only on purpose: Seb archives developer 1
	// and plays under Xcode, so a per-frame print here is the documented
	// console-ink OOM hazard.
	{
		static qbool rt_ent_truncwarned, rt_lc_truncwarned;
		if (!rt_ent_truncwarned && (nv >= RT_ENT_MAXVERTS || nt >= RT_ENT_MAXTRIS))
		{
			rt_ent_truncwarned = true;
			Con_DPrintf("RT entity gather full (%d verts / %d tris): some entities cast no RT shadow this view\n", nv, nt);
		}
		if (!rt_lc_truncwarned && (lnv >= RT_LC_MAXVERTS || lnt >= RT_LC_MAXTRIS))
		{
			rt_lc_truncwarned = true;
			Con_DPrintf("RT light-core gather full (%d verts / %d tris): some flames render as plain casters this view\n", lnv, lnt);
		}
	}
	*out_verts = rt_ent_verts;
	*out_numverts = nv;
	*out_tris = rt_ent_tris;
	*out_norms = wantnorms ? rt_ent_norms : NULL;
	*out_lcnumverts = lnv;
	*out_lcnumtris = lnt;
	return nt;
}

// Milestone 7: gather this frame's DYNAMIC lights for the Metal RT sidecar, so
// their (often moving) light casts real-time RT shadows. This is the high-value
// case: the map's STATIC lights already have their shadows baked into Quake's
// lightmaps, so RT largely duplicates them — but explosions, rocket/muzzle
// flashes, the player flash, and CSQC dynamic lights are NEVER baked, so a shadow
// swinging as a rocket's glow crosses a room is shadow the engine cannot otherwise
// produce.
//
// Source = r_refdef.scene.lights[0..numlights), the DYNAMIC light list assembled
// each frame by CL_RelinkLightFlashes (cl.dlights) + templights (entity
// muzzleflash / PFLAGS_FULLDYNAMIC / beam ends). It is reset and repopulated in
// CL_UpdateWorld, exactly like scene.entities[] which the entity gather above
// already reads at this same hook, so it is equally valid here. It is DISJOINT
// from the static worldlights (r_shadow_worldlightsarray, exported by
// R_Shadow_GetWorldLightPositions), so appending both lists never double-counts.
//
// Writes 7 floats/light (origin xyz, radius, colour rgb) into dst — the same
// layout the static exporter uses — view-distance culled the same way. The
// player's own muzzle-flash LIGHT is intentionally kept (only the player MODEL is
// excluded, in the entity gather); a flash lighting the room should still cast.
// Returns the number of lights written.
static int RT_GatherDynamicLights(float *dst, int maxlights, const float *vieworigin, float culldist)
{
	int n = 0, i;
	for (i = 0; i < r_refdef.scene.numlights && n < maxlights; i++)
	{
		const rtlight_t *rtl = r_refdef.scene.lights[i];
		vec3_t org;
		float dx, dy, dz, reach, stylescale;
		if (!rtl || rtl->radius <= 0)
			continue;
		// origin is the translation of the light->world matrix (unaffected by the
		// radius scaling baked into its rotation part)
		Matrix4x4_OriginFromMatrix(&rtl->matrix_lighttoworld, org);
		dx = org[0] - vieworigin[0];
		dy = org[1] - vieworigin[1];
		dz = org[2] - vieworigin[2];
		reach = rtl->radius + culldist;
		if (dx*dx + dy*dy + dz*dz > reach*reach)
			continue;                                       // can't affect anything the viewer sees
		// same guarded lightstyle scale as the static path: a styled dynamic light
		// (CSQC/PFLAGS lights can carry one) pulses with its style; style -1 (most
		// dlights/templights) and style 0 fall through to ~1.0, so ordinary
		// explosion/rocket/muzzle flashes are unaffected.
		stylescale = (rtl->style >= 0 && rtl->style < MAX_LIGHTSTYLES)
		           ? r_refdef.scene.rtlightstylevalue[rtl->style] : 1.0f;
		dst[n*RT_LIGHT_STRIDE+0] = org[0];
		dst[n*RT_LIGHT_STRIDE+1] = org[1];
		dst[n*RT_LIGHT_STRIDE+2] = org[2];
		dst[n*RT_LIGHT_STRIDE+3] = rtl->radius;
		dst[n*RT_LIGHT_STRIDE+4] = rtl->color[0] * stylescale;
		dst[n*RT_LIGHT_STRIDE+5] = rtl->color[1] * stylescale;
		dst[n*RT_LIGHT_STRIDE+6] = rtl->color[2] * stylescale;
		// A light that wants to read differently in the FOG than on surfaces
		// carries its own weight; everything else counts normally. memset in
		// R_RTLight_Update leaves this 0, so 0 has to mean "unset" - see the
		// m5fogweight declaration in client.h.
		dst[n*RT_LIGHT_STRIDE+7] = rtl->m5fogweight > 0.0f ? rtl->m5fogweight : 1.0f;
		// SPOT CONE (F6). Zero direction = omni, which is what every producer but
		// the handlamp leaves here (R_RTLight_Update memsets the rtlight), and the
		// kernels treat a zero direction as an exact 1.0 multiply.
		dst[n*RT_LIGHT_STRIDE+8]  = rtl->m5spotdir[0];
		dst[n*RT_LIGHT_STRIDE+9]  = rtl->m5spotdir[1];
		dst[n*RT_LIGHT_STRIDE+10] = rtl->m5spotdir[2];
		dst[n*RT_LIGHT_STRIDE+11] = rtl->m5spotcos;
		dst[n*RT_LIGHT_STRIDE+12] = rtl->m5spotcosinner;
		dst[n*RT_LIGHT_STRIDE+13] = rtl->m5spotfilament;
		n++;
	}
	return n;
}

/*
================
RT_ViewmodelLight

Light the VIEW WEAPON from the same light list the sidecar traces.

Why this exists: wall lighting forces r_fullbright, which strips RENDER_LIGHT from
every entity (cl_main.c, "either fullbright or lit"), and the RT composite that
hands all the other entities their lighting back is depth-masked OFF the weapon on
purpose -- it sits in the bottom sixteenth of the depth buffer so the multiply
cannot print the wall BEHIND it onto it. Every other opaque thing is flattened and
then relit; the gun was only flattened. So it rendered at constant albedo in every
room, and the muzzle flash and handlamp lit everything except the hands holding
them.

The mask is still right -- it stops the WRONG term reaching the gun. This supplies
a right one, evaluated at the eye against the same lights, with the same falloff,
the same walllight gain and the same lmax knee the wall term uses, so the weapon
belongs to the room instead of to a lightmap scale the mode no longer uses.

Fills ambient/diffuse/dir for the model-light path. Returns false when the feature
is off or unavailable, and then writes nothing -- the caller keeps stock behaviour.
================
*/
// SEPTEMBER2 C1 (2026-09-06): the ray tracer's light sum at a WORLD point, on the
// CPU -- the weapon light's shape (below) made general. Gathers the sidecar's own
// list around p, sums the kernels' (1 - d/r)^2 falloff, shadow-tests the loudest
// nshadow with tracelines from p (bounded whatever the map holds), then the wall
// term's tone chain: walllight * 6, the saturation knob, the ambient fill, the lmax
// shoulder. First consumer: a blended liquid SURFACE's own unshadowed-ish light,
// one constant per batch (rt_metal_liquids_own), so the water stops printing the
// pool floor's shadows. Returns false when the RT term is not the scene's lighting.
qbool RT_LightSumAt(const vec3_t p, int nshadow, float out[3])
{
	static float ls_buf[RT_VML_MAXLIGHTS * RT_LIGHT_STRIDE];
	vec3_t sum, from;
	float best[RT_VML_MAXSHADOW];
	int besti[RT_VML_MAXSHADOW];
	int nd, ns, nl, i, q, nbest = 0;
	float scale, lmax, sat, lum;
	if (!rt_metal.integer || rt_metal_walllight.value <= 0.0f || !RT_Metal_Active())
		return false;
	nd = RT_GatherDynamicLights(ls_buf, RT_VML_MAXLIGHTS, p, rt_metal_culldist.value);
	ns = R_Shadow_GetWorldLightPositions(ls_buf + nd * RT_LIGHT_STRIDE, RT_VML_MAXLIGHTS - nd, p, rt_metal_culldist.value);
	nl = nd + ns;
	nshadow = bound(0, nshadow, RT_VML_MAXSHADOW);
	VectorClear(sum);
	VectorSet(from, p[0], p[1], p[2] + 2.0f);   // a whisker above the surface, so the trace does not start inside it
	for (i = 0; i < nl; i++)
	{
		const float *L = ls_buf + i * RT_LIGHT_STRIDE;
		vec3_t rel;
		float d, radius = L[3], atten, contrib;
		if (radius <= 0.0f) continue;
		VectorSubtract(L, p, rel);
		d = VectorLength(rel);
		if (d >= radius) continue;
		atten = 1.0f - d / radius; atten *= atten;
		contrib = (L[4] + L[5] + L[6]) * atten;
		if (contrib <= 0.0f) continue;
		if (nshadow > 0)
		{
			int slot = nbest;
			if (nbest < nshadow) nbest++;
			else
			{
				int worst = 0, k;
				for (k = 1; k < nbest; k++) if (best[k] < best[worst]) worst = k;
				slot = (best[worst] >= contrib) ? -1 : worst;
			}
			if (slot >= 0) { best[slot] = contrib; besti[slot] = i; }
		}
		for (q = 0; q < 3; q++) sum[q] += L[4 + q] * atten;
	}
	for (i = 0; i < nbest; i++)
	{
		const float *L = ls_buf + besti[i] * RT_LIGHT_STRIDE;
		vec3_t org, rel;
		float d, radius = L[3], atten;
		VectorCopy(L, org);
		if (CL_TraceLine(from, org, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, MATERIALFLAGMASK_TRANSLUCENT, collision_extendmovelength.value, true, false, NULL, false, true).fraction >= 1)
			continue;
		VectorSubtract(org, p, rel);
		d = VectorLength(rel);
		if (d >= radius) continue;
		atten = 1.0f - d / radius; atten *= atten;
		for (q = 0; q < 3; q++) sum[q] = max(0.0f, sum[q] - L[4 + q] * atten);
	}
	// the wall term's arithmetic, LOCKSTEP with RT_ViewmodelLight below and the kernel
	scale = rt_metal_walllight.value * 6.0f;
	VectorScale(sum, scale, sum);
	sat = rt_metal_color.value;
	if (sat != 1.0f)
	{
		lum = sum[0] * 0.299f + sum[1] * 0.587f + sum[2] * 0.114f;
		for (q = 0; q < 3; q++) sum[q] = max(0.0f, lum + (sum[q] - lum) * sat);
	}
	for (q = 0; q < 3; q++) sum[q] += rt_metal_ambient.value;
	lmax = rt_metal_lmax.value;
	if (lmax > 0.0f)
	{
		float m = max(sum[0], max(sum[1], sum[2]));
		if (m > lmax)
		{
			float e = m - lmax, kk = lmax * 0.6666667f;
			float k = (lmax + e * kk / (kk + e)) / m;
			VectorScale(sum, k, sum);
		}
	}
	VectorCopy(sum, out);
	return true;
}

qbool RT_ViewmodelLight(float *out_ambient, float *out_diffuse, float *out_dir)
{
	// per-light scratch: origin xyz + the contribution we computed for it
	static float vml_buf[RT_VML_MAXLIGHTS * RT_LIGHT_STRIDE];
	// smoothed state, so a dominant-light hand-over or a shadow-test flip while
	// walking eases rather than pops (see rt_metal_viewmodel_smooth)
	static float vml_amb[3], vml_dif[3], vml_dir[3];
	static qbool vml_hasprev;
	vec3_t eye, fwd, left, up, sum, dirsum;
	float best[RT_VML_MAXSHADOW];
	int besti[RT_VML_MAXSHADOW];
	int nd, ns, nl, i, q, nbest = 0, nshadow;
	float scale, lmax, sat, lum, blend;

	if (!rt_metal.integer || rt_metal_walllight.value <= 0.0f || rt_metal_viewmodel.value <= 0.0f || !RT_Metal_Active())
	{
		vml_hasprev = false;   // so re-enabling does not ease out of a stale frame
		return false;
	}

	// The eye, from the view MATRIX -- r_refdef.view.origin is only written inside
	// R_RenderView and this runs before it (the documented trap).
	Matrix4x4_ToVectors(&r_refdef.view.matrix, fwd, left, up, eye);

	nd = RT_GatherDynamicLights(vml_buf, RT_VML_MAXLIGHTS, eye, rt_metal_culldist.value);
	ns = R_Shadow_GetWorldLightPositions(vml_buf + nd * RT_LIGHT_STRIDE, RT_VML_MAXLIGHTS - nd, eye, rt_metal_culldist.value);
	nl = nd + ns;

	nshadow = bound(0, rt_metal_viewmodel_shadows.integer, RT_VML_MAXSHADOW);
	VectorClear(sum);
	VectorClear(dirsum);

	for (i = 0; i < nl; i++)
	{
		const float *L = vml_buf + i * RT_LIGHT_STRIDE;
		vec3_t rel;
		float d, radius = L[3], atten, contrib;
		if (radius <= 0.0f)
			continue;
		VectorSubtract(L, eye, rel);
		d = VectorLength(rel);
		if (d >= radius)
			continue;                     // a light contributes EXACTLY nothing past its radius
		atten = 1.0f - d / radius;
		atten *= atten;                   // the kernels' own (1 - d/r)^2 falloff
		contrib = (L[4] + L[5] + L[6]) * atten;
		if (contrib <= 0.0f)
			continue;
		// keep the running top-N by contribution, for the shadow tests below
		if (nshadow > 0)
		{
			int slot = nbest;
			if (nbest < nshadow)
				nbest++;
			else
			{
				int worst = 0, k;
				for (k = 1; k < nbest; k++)
					if (best[k] < best[worst])
						worst = k;
				if (best[worst] >= contrib)
					slot = -1;
				else
					slot = worst;
			}
			if (slot >= 0)
			{
				best[slot] = contrib;
				besti[slot] = i;
			}
		}
		// store the attenuation back so the shadow pass can subtract exactly what
		// it vetoes, rather than recomputing and drifting
		for (q = 0; q < 3; q++)
		{
			float e = L[4 + q] * atten;
			sum[q] += e;
		}
		VectorNormalize(rel);
		VectorMA(dirsum, contrib, rel, dirsum);
	}

	// Occlusion, for the few loudest only: this is what makes the gun darken as you
	// step behind a pillar. Bounded by nshadow regardless of how many lights the map
	// has, so ad_sepulcher's 2759 costs the same as e1m3's 66.
	for (i = 0; i < nbest; i++)
	{
		const float *L = vml_buf + besti[i] * RT_LIGHT_STRIDE;
		vec3_t org, rel;
		float d, radius = L[3], atten;
		VectorCopy(L, org);
		if (CL_TraceLine(eye, org, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, MATERIALFLAGMASK_TRANSLUCENT, collision_extendmovelength.value, true, false, NULL, false, true).fraction >= 1)
			continue;                     // visible: keep everything we accumulated
		VectorSubtract(org, eye, rel);
		d = VectorLength(rel);
		if (d >= radius)
			continue;
		atten = 1.0f - d / radius;
		atten *= atten;
		for (q = 0; q < 3; q++)
			sum[q] = max(0.0f, sum[q] - L[4 + q] * atten);
		VectorNormalize(rel);
		// hoisted: Vector* macros evaluate every argument once PER COMPONENT
		{
			float veto = -((L[4] + L[5] + L[6]) * atten);
			VectorMA(dirsum, veto, rel, dirsum);
		}
	}

	// Match the wall term's own arithmetic so the gun sits in the same tonal world:
	// Lsum * walllight * 6 (rt_metal.m), then the saturation knob, then the ambient
	// fill, then the lmax shoulder. Skipping the shoulder would let the gun blow out
	// beside its own light exactly the way walls did before rt_metal_lmax existed.
	scale = rt_metal_walllight.value * 6.0f * rt_metal_viewmodel.value;
	VectorScale(sum, scale, sum);

	sat = rt_metal_color.value;
	if (sat != 1.0f)
	{
		lum = sum[0] * 0.299f + sum[1] * 0.587f + sum[2] * 0.114f;
		for (q = 0; q < 3; q++)
			sum[q] = max(0.0f, lum + (sum[q] - lum) * sat);
	}

	for (q = 0; q < 3; q++)
		sum[q] += rt_metal_ambient.value;

	lmax = rt_metal_lmax.value;
	if (lmax > 0.0f)
	{
		// max-channel Reinhard with the same asymptote (knee * 5/3) as the kernel's,
		// so hue survives and no knob value can push the weapon past white
		float m = max(sum[0], max(sum[1], sum[2]));
		if (m > lmax)
		{
			// LOCKSTEP with the kernel's shoulder (rt_metal.m, "SOFT CEILING"):
			// knee + e*k/(k+e), k = knee*2/3, asymptote knee*5/3. The first cut
			// (2026-08-21 to 2026-09-02) had (1 + 2/3*over)/(1 + over), which
			// DECREASES above the knee -- the gun got DIMMER as its room got
			// brighter, past 2.5. Found while copying it for rt_metal_lightcap.
			float e = m - lmax, kk = lmax * 0.6666667f;
			float k = (lmax + e * kk / (kk + e)) / m;
			VectorScale(sum, k, sum);
		}
	}

	// Split into an ambient floor and a directional term. The lit facets then read
	// at the full term (matching a wall facing the same light) while the facets
	// turned away keep the floor instead of going black -- which is the difference
	// between "shaded" and "half the gun is missing".
	// braces are load-bearing: the Vector* macros are comma expressions, so a
	// braceless if/else around one does not compile
	if (VectorLength2(dirsum) > 0.0f)
	{
		VectorNormalize(dirsum);
	}
	else
	{
		VectorSet(dirsum, 0.0f, 0.0f, 1.0f);
	}
	for (q = 0; q < 3; q++)
	{
		out_ambient[q] = sum[q] * RT_VML_AMBIENTSHARE;
		out_diffuse[q] = sum[q] * (1.0f - RT_VML_AMBIENTSHARE);
	}
	VectorCopy(dirsum, out_dir);

	// Ease it. cl.time is the wrong clock here (it pauses); host frametime is what
	// the view itself moves on.
	blend = 1.0f;
	if (rt_metal_viewmodel_smooth.value > 0.0f && vml_hasprev && cl.realframetime > 0)
	{
		blend = 1.0f - expf(-(float)cl.realframetime / rt_metal_viewmodel_smooth.value);
		blend = bound(0.0f, blend, 1.0f);
	}
	for (q = 0; q < 3; q++)
	{
		vml_amb[q] += (out_ambient[q] - vml_amb[q]) * blend;
		vml_dif[q] += (out_diffuse[q] - vml_dif[q]) * blend;
		vml_dir[q] += (out_dir[q] - vml_dir[q]) * blend;
	}
	vml_hasprev = true;
	VectorCopy(vml_amb, out_ambient);
	VectorCopy(vml_dif, out_diffuse);
	if (VectorLength2(vml_dir) > 0.0f)
	{
		VectorCopy(vml_dir, out_dir);
		VectorNormalize(out_dir);
	}
	// First-event liveness line (the console-ink rule: once per process, never per
	// frame). tests/smoke.sh greps it -- the feature falls back to the flat gun
	// SILENTLY on any predicate above, so without this a regression that stopped
	// it engaging would leave every bed quiet.
	{
		static qbool vml_announced;
		if (!vml_announced)
		{
			vml_announced = true;
			Con_DPrintf("RT viewmodel light active (%d lights in range)\n", nl);
		}
	}
	return true;
}

// Build the RT world geometry = ONLY the world model (*0)'s own DRAWABLE surfaces.
// CRITICAL: the Q1BSP surfmesh (data_vertex3f/data_element3i) is SHARED across the
// WHOLE BSP — the world plus every inline submodel (*1..*N): doors, buttons, and
// the invisible SOLID_TRIGGER brushes (trigger_message etc.). Feeding the whole
// surfmesh to the world BLAS made those invisible trigger boxes cast RT shadows
// ("invisible boxes"), and rest-pose-ghosted moving submodels. So walk only the
// world model's own surface range (submodelsurfaces_start..end, via the
// texture-sorted view which permutes only within that range) and emit its
// triangles, skipping NODRAW/NOSHADOW surfaces (caulk, void-facing sky, water) to
// match the engine's own shadow-caster selection. Visible moving submodels
// (doors/plats) are cast separately by RT_GatherEntityShadowCasters; invisible
// triggers are neither drawn nor client render entities, so they now cast nothing.
// The result indexes into the shared vertex array (unreferenced verts are ignored
// by the AS build). Cached on the world-model pointer — rebuilt only on map change.
static int   *rt_world_tris;        // filtered element3i (indices into surfmesh verts)
static unsigned char *rt_world_albedo; // per-TRIANGLE RGBA8 mean surface colour (GIARC G4-2):
                                       // the texture's skinframe avgcolor, the same quantity
                                       // r_shadow.c's bounce grid reads as its reflect colour.
                                       // Parallel to rt_world_tris (main caster list only --
                                       // the emissive/sky/liquid instances never shade from it)
static int    rt_world_numtris;
static int    rt_world_capacity;    // in triangles
static unsigned long rt_world_gen;  // bumped whenever the geometry is (re)built -> BLAS token
// lava sheets get their own triangle list and generation: they build a separate
// emissive instance (rt_metal_lavaemissive), and their gen is DELIBERATELY not
// rt_world_gen -- a cvar toggle must rebuild only the lava BLAS, because a
// SetWorld rebuild drops the sidecar's baked fog field until the next map bake
// (the field is only re-sent on a rebake; see RT_Metal_SetWorld)
static int   *rt_world_lavatris;
static int    rt_world_numlavatris;
static int    rt_world_lavacapacity;  // in triangles
static unsigned long rt_world_lavagen;
static int    rt_world_lastlavamode = -1;
// sky brushes get their own list too (rt_metal_skyopen): they become the OPEN
// SKY instance the surface kernel treats as a miss, so the sky sentinel -- and
// everything that keys on it (the fog kernel's sky handling, r_volumetric_skyfog,
// beams crossing sky windows) -- can actually fire on map sky. Same own-gen
// rationale as lava: a cvar toggle must never force a SetWorld rebuild.
static int   *rt_world_skytris;
static int    rt_world_numskytris;
static int    rt_world_skycapacity;   // in triangles
static unsigned long rt_world_skygen;
static int    rt_world_lastskymode = -1;
// opaque liquids (rt_metal_liquidemissive): water/slime that will render OPAQUE
// this frame become their own EMISSIVE instance -- primary rays stop at the
// sheet and the composite leaves it exactly as authored, instead of printing
// the pool floor's lighting term through it. The mode folds in the
// transparent-water enabling set, so the list empties the moment liquids go
// blended and the instance can never double-light the rt_metal_liquids path.
static int   *rt_world_liqtris;
static int    rt_world_numliqtris;
static int    rt_world_liqcapacity;   // in triangles
static int   *rt_world_bliqtris;      // SEPTEMBER2 C2: the BLENDED liquids (instance 6, mask 0x40)
static int    rt_world_numbliqtris;
static int    rt_world_bliqcapacity;
static unsigned long rt_world_bliqgen;
static int    rt_world_lastbliqmode = -1;
static unsigned long rt_world_liqgen;
static int    rt_world_lastliquidmode = -1;
// synthetic lava lights (rt_metal_lavalights): one warm emitter per 256-unit
// XY patch of lava surface, gathered in the same walk. Enumeration is
// unconditional (independent of rt_metal_lavaemissive); the cvar gates
// consumption at upload time only.
#define RT_LAVA_MAXLIGHTS 64
static float  rt_lava_lights[RT_LAVA_MAXLIGHTS * 7];   // xyz, radius, rgb
static int    rt_lava_numlights;
static const void *rt_world_lastmodel;
static const void *rt_world_lastverts;
static int    rt_world_lastnumtris;
static int    rt_world_lastnumverts;

static void RT_BuildWorldGeometry(const model_t *model)
{
	int s, nt = 0, maxtris;
	// Cache on the model pointer AND the surfmesh vertex-array pointer AND the whole-
	// BSP vertex + triangle counts. The model_t struct is reused across maps (fixed
	// mod_known slot recycled by Mem_ExpandableArray_AllocRecord), but its surfmesh
	// data is reallocated per map, so keying on the vertex pointer + counts reliably
	// detects a map change even if the model pointer is recycled (the ABA case). This
	// also prevents feeding a previous (larger) map's index buffer against a new
	// (smaller) map's vertex array — which would drive an out-of-range read in the
	// Metal acceleration-structure build.
	int lavamode = rt_metal_lavaemissive.integer != 0;
	int skymode = rt_metal_skyopen.integer != 0;
	// opaque-liquid mode folds in the transparent-water enabling set (mirroring
	// RSurf_GetCurrentTexture's wateralpha gate, gl_rmain.c): when water will
	// render BLENDED this frame, the gather must come up empty -- the blended
	// path with rt_metal_liquids owns that configuration, never both. All of
	// these are per-frame mutable, which is why liquidmode is memoed below.
	int wateralphalive = r_water.integer
	 || ((model->brush.supportwateralpha || r_wateralpha_force.integer || r_novis.integer || r_trippy.integer)
	     && r_wateralpha.value < 1.0f);
	int liquidmode = (rt_metal_liquidemissive.integer != 0) && !wateralphalive;
	// SEPTEMBER2 C2: the SAME surfaces, gathered when they render BLENDED instead -- the
	// two modes are exclusive by construction (one predicate, inverted).
	int bliquidmode = (rt_metal_liquids_rt.integer != 0) && wateralphalive;
	// A CONFIGURATION THAT CANNOT DELIVER SAYS SO -- ONCE, on change.
	// rt_metal_liquids multiplies the RT term into water that renders BLENDED,
	// and on a stock id1 map nothing renders blended without r_wateralpha_force:
	// vanilla vis fails Mod_Q1BSP_CheckWaterAlphaSupport, so r_wateralpha is
	// ignored with nothing said anywhere (CLAUDE.md's hard-won fact). The cost
	// of that silence is measured rather than hypothetical -- Seb tuned
	// rt_metal_liquids to 0.45 and it had NEVER ONCE RUN for him, for months,
	// because he set it from the console where the menu's own greying and its
	// footer explanation (menu.c, M_Options_RTShadows_Draw) never reach. This is
	// that explanation's console voice, keyed on the SAME predicate.
	// Change-only, per the console-ink rule: this is a per-frame path.
	{
		static int rt_liquids_lastinert = -1;
		int inert = rt_metal.integer && rt_metal_liquids.value > 0.0f && !wateralphalive;
		if (inert != rt_liquids_lastinert)
		{
			rt_liquids_lastinert = inert;
			if (inert)
				Con_Printf("rt_metal_liquids %.2f is INERT here -- water is not rendering blended, so there is no transparent surface to light. Set r_wateralpha_force 1 with r_wateralpha below 1 (Options -> Effects and Particles), or leave it to rt_metal_liquidemissive, which owns opaque liquids.\n", rt_metal_liquids.value);
		}
	}
	int lnt = 0, snt = 0, qnt = 0, bnt = 0;
	int lcellx[RT_LAVA_MAXLIGHTS], lcelly[RT_LAVA_MAXLIGHTS], lcount[RT_LAVA_MAXLIGHTS];
	float lsumx[RT_LAVA_MAXLIGHTS], lsumy[RT_LAVA_MAXLIGHTS], lmaxz[RT_LAVA_MAXLIGHTS];
	int nlcells = 0, ltruncated = 0, i;
	int mapchanged = !(model == rt_world_lastmodel
	 && model->surfmesh.data_vertex3f == rt_world_lastverts
	 && model->surfmesh.num_vertices == rt_world_lastnumverts
	 && model->surfmesh.num_triangles == rt_world_lastnumtris);
	if (!mapchanged && lavamode == rt_world_lastlavamode && skymode == rt_world_lastskymode && liquidmode == rt_world_lastliquidmode && bliquidmode == rt_world_lastbliqmode)
		return;                                             // already built for this map + lava/sky/liquid mode
	maxtris = model->surfmesh.num_triangles;                // upper bound (whole BSP)
	if (maxtris > rt_world_capacity)
	{
		free(rt_world_tris);
		free(rt_world_albedo);
		rt_world_tris = (int *)malloc((size_t)maxtris * 3 * sizeof(int));
		rt_world_albedo = (unsigned char *)malloc((size_t)maxtris * 4);
		if (!rt_world_albedo) { free(rt_world_tris); rt_world_tris = NULL; }
		rt_world_capacity = rt_world_tris ? maxtris : 0;
	}
	if (maxtris > rt_world_lavacapacity)
	{
		free(rt_world_lavatris);
		rt_world_lavatris = (int *)malloc((size_t)maxtris * 3 * sizeof(int));
		rt_world_lavacapacity = rt_world_lavatris ? maxtris : 0;
	}
	if (maxtris > rt_world_skycapacity)
	{
		free(rt_world_skytris);
		rt_world_skytris = (int *)malloc((size_t)maxtris * 3 * sizeof(int));
		rt_world_skycapacity = rt_world_skytris ? maxtris : 0;
	}
	if (maxtris > rt_world_liqcapacity)
	{
		free(rt_world_liqtris);
		rt_world_liqtris = (int *)malloc((size_t)maxtris * 3 * sizeof(int));
		rt_world_liqcapacity = rt_world_liqtris ? maxtris : 0;
	}
	if (maxtris > rt_world_bliqcapacity)
	{
		free(rt_world_bliqtris);
		rt_world_bliqtris = (int *)malloc((size_t)maxtris * 3 * sizeof(int));
		rt_world_bliqcapacity = rt_world_bliqtris ? maxtris : 0;
	}
	if (!rt_world_tris || !rt_world_lavatris || !rt_world_skytris || !rt_world_liqtris || !rt_world_bliqtris) { rt_world_numtris = rt_world_numlavatris = rt_world_numskytris = rt_world_numliqtris = rt_world_numbliqtris = 0; rt_world_lastmodel = NULL; return; }   // retry next frame
	for (s = model->submodelsurfaces_start; s < model->submodelsurfaces_end; s++)
	{
		const msurface_t *surf = model->data_surfaces + model->modelsurfaces_sorted[s];
		const int *e;
		int t;
		// lava first: it carries NOSHADOW (so the filter below would drop it) but
		// with rt_metal_lavaemissive it becomes a separate emissive instance --
		// primary rays stop at the sheet instead of shading the sunken geometry.
		// The supercontents test deliberately excludes *teleport / *rift (they get
		// WATER supercontents at load and keep the old behaviour).
		if (surf->texture && (surf->texture->supercontents & SUPERCONTENTS_LAVA))
		{
			// lava-light grid: accumulate this surface's centre into its 256-unit
			// XY cell (always -- the light list must not depend on lavaemissive)
			float cx = (surf->mins[0] + surf->maxs[0]) * 0.5f;
			float cy = (surf->mins[1] + surf->maxs[1]) * 0.5f;
			int gx = (int)floor(cx / 256.0f), gy = (int)floor(cy / 256.0f), c;
			for (c = 0; c < nlcells; c++)
				if (lcellx[c] == gx && lcelly[c] == gy)
					break;
			if (c == nlcells)
			{
				if (nlcells < RT_LAVA_MAXLIGHTS)
				{
					lcellx[c] = gx; lcelly[c] = gy;
					lsumx[c] = lsumy[c] = 0.0f;
					lmaxz[c] = surf->maxs[2];
					lcount[c] = 0;
					nlcells++;
				}
				else
				{
					ltruncated = 1;
					c = -1;
				}
			}
			if (c >= 0)
			{
				lsumx[c] += cx;
				lsumy[c] += cy;
				if (surf->maxs[2] > lmaxz[c]) lmaxz[c] = surf->maxs[2];
				lcount[c]++;
			}
			if (lavamode)
			{
				e = model->surfmesh.data_element3i + surf->num_firsttriangle * 3;
				for (t = 0; t < surf->num_triangles && lnt < maxtris; t++, e += 3)
				{
					rt_world_lavatris[lnt*3+0] = e[0];
					rt_world_lavatris[lnt*3+1] = e[1];
					rt_world_lavatris[lnt*3+2] = e[2];
					lnt++;
				}
				continue;
			}
			// lavamode off: fall through to the NOSHADOW filter (which drops it
			// from the main caster list, as it always did)
		}
		// sky next (it carries neither NODRAW nor NOSHADOW, so without this it
		// lands in the world list as an ordinary caster -- which is exactly what
		// kept every sky-sentinel branch in the tree from ever firing on map
		// sky): with rt_metal_skyopen it becomes the OPEN SKY instance instead
		if (skymode && surf->texture && (surf->texture->basematerialflags & MATERIALFLAG_SKY))
		{
			e = model->surfmesh.data_element3i + surf->num_firsttriangle * 3;
			for (t = 0; t < surf->num_triangles && snt < maxtris; t++, e += 3)
			{
				rt_world_skytris[snt*3+0] = e[0];
				rt_world_skytris[snt*3+1] = e[1];
				rt_world_skytris[snt*3+2] = e[2];
				snt++;
			}
			continue;
		}
		// opaque liquids: water/slime only. WATERALPHA distinguishes real
		// liquids from *teleport / *rift (WATER supercontents, lava-style
		// flags -- they keep their old behaviour); the image-alpha exclusions
		// keep custom translucent replacement content out. Liquids carry
		// NOSHADOW, so anything not diverted here still drops at the filter
		// below exactly as before.
		if (liquidmode && surf->texture
		 && (surf->texture->supercontents & (SUPERCONTENTS_WATER | SUPERCONTENTS_SLIME))
		 && !(surf->texture->supercontents & SUPERCONTENTS_LAVA)
		 && (surf->texture->basematerialflags & MATERIALFLAG_WATERALPHA)
		 && !(surf->texture->basematerialflags & (MATERIALFLAG_ALPHA | MATERIALFLAG_BLENDED | MATERIALFLAG_NODRAW)))
		{
			e = model->surfmesh.data_element3i + surf->num_firsttriangle * 3;
			for (t = 0; t < surf->num_triangles && qnt < maxtris; t++, e += 3)
			{
				rt_world_liqtris[qnt*3+0] = e[0];
				rt_world_liqtris[qnt*3+1] = e[1];
				rt_world_liqtris[qnt*3+2] = e[2];
				qnt++;
			}
			continue;
		}
		// SEPTEMBER2 C2: the same liquids when they render BLENDED -- the seventh instance,
		// which only the C2 intersect sees. Still NOSHADOW-dropped from the caster list below.
		if (bliquidmode && surf->texture
		 && (surf->texture->supercontents & (SUPERCONTENTS_WATER | SUPERCONTENTS_SLIME))
		 && !(surf->texture->supercontents & SUPERCONTENTS_LAVA)
		 && (surf->texture->basematerialflags & MATERIALFLAG_WATERALPHA)
		 && !(surf->texture->basematerialflags & (MATERIALFLAG_ALPHA | MATERIALFLAG_BLENDED | MATERIALFLAG_NODRAW)))
		{
			e = model->surfmesh.data_element3i + surf->num_firsttriangle * 3;
			for (t = 0; t < surf->num_triangles && bnt < maxtris; t++, e += 3)
			{
				rt_world_bliqtris[bnt*3+0] = e[0];
				rt_world_bliqtris[bnt*3+1] = e[1];
				rt_world_bliqtris[bnt*3+2] = e[2];
				bnt++;
			}
			continue;
		}
		if (surf->texture && (surf->texture->basematerialflags & (MATERIALFLAG_NODRAW | MATERIALFLAG_NOSHADOW)))
			continue;                                       // invisible / non-shadowing -> not a caster
		{
			// per-surface mean colour for the coloured bounce (GIARC G4-2):
			// skinframe avgcolor is 0-1 floats, populated by the shared loader
			// for every ordinary texture and already consumed as the bounce
			// grid's reflect colour (r_shadow.c). Hoisted per surface; grey
			// fallback for anything without a skinframe. avgcolor deliberately
			// ignores pure-black pixels (its macro says so), so a dark texture
			// reads a touch brighter than its true mean -- acceptable for
			// bounce, stated here.
			unsigned char ar = 128, ag = 128, ab = 128;
			if (surf->texture && surf->texture->currentskinframe)
			{
				const float *ac = surf->texture->currentskinframe->avgcolor;
				ar = (unsigned char)bound(0.0f, ac[0] * 255.0f, 255.0f);
				ag = (unsigned char)bound(0.0f, ac[1] * 255.0f, 255.0f);
				ab = (unsigned char)bound(0.0f, ac[2] * 255.0f, 255.0f);
			}
			e = model->surfmesh.data_element3i + surf->num_firsttriangle * 3;
			for (t = 0; t < surf->num_triangles && nt < maxtris; t++, e += 3)
			{
				rt_world_tris[nt*3+0] = e[0];
				rt_world_tris[nt*3+1] = e[1];
				rt_world_tris[nt*3+2] = e[2];
				rt_world_albedo[nt*4+0] = ar;
				rt_world_albedo[nt*4+1] = ag;
				rt_world_albedo[nt*4+2] = ab;
				rt_world_albedo[nt*4+3] = 255;
				nt++;
			}
		}
	}
	rt_world_numtris = nt;
	rt_world_numlavatris = lnt;
	rt_world_numskytris = snt;
	rt_world_numliqtris = qnt;
	rt_world_numbliqtris = bnt;   // SEPTEMBER2 C2
	// emit the merged lava lights: one per occupied grid cell, sat just above
	// the sheet, warm lava colour. Capped hard at RT_LAVA_MAXLIGHTS with an
	// explicit report (the no-silent-caps rule).
	rt_lava_numlights = nlcells;
	for (i = 0; i < nlcells; i++)
	{
		float *L = rt_lava_lights + i * 7;
		L[0] = lsumx[i] / (float)lcount[i];
		L[1] = lsumy[i] / (float)lcount[i];
		L[2] = lmaxz[i] + 24.0f;
		L[3] = 350.0f;
		L[4] = 1.0f; L[5] = 0.4f; L[6] = 0.1f;
	}
	if (ltruncated)
		Con_DPrintf("RT: lava light grid truncated at %d cells\n", RT_LAVA_MAXLIGHTS);
	if (nlcells)
		Con_DPrintf("RT: %d lava lights (256u grid)\n", nlcells);
	rt_world_lastmodel = model;
	rt_world_lastverts = model->surfmesh.data_vertex3f;
	rt_world_lastnumverts = model->surfmesh.num_vertices;
	rt_world_lastnumtris = model->surfmesh.num_triangles;
	rt_world_lastlavamode = lavamode;
	rt_world_lastskymode = skymode;
	rt_world_lastliquidmode = liquidmode;
	rt_world_lastbliqmode = bliquidmode;
	// the main gen bumps ONLY on a map change: the main list's content ignores the
	// lava cvar, and a spurious SetWorld rebuild would drop the baked fog field
	if (mapchanged)
		rt_world_gen++;                                     // geometry changed -> RT_Metal_SetWorld rebuilds the BLAS
	rt_world_lavagen++;                                     // lava list changed -> RT_Metal_SetLavaSurfaces rebuilds
	rt_world_skygen++;                                      // sky list changed -> RT_Metal_SetSkySurfaces rebuilds
	rt_world_liqgen++;                                      // liquid list changed -> RT_Metal_SetLiquidSurfaces rebuilds
	rt_world_bliqgen++;                                     // SEPTEMBER2 C2: the blended list too
}

/*
================
CL_SkyLightning_Update  (BEAUTY C1, 2026-09-17; the fog half added the same day
on Seb's word, "can be lit like the lightning gun/thunderbolt")

The storm's state machine, run ONCE per frame from the relink window
(CSQC_RelinkAllEntities, beside M5_Torch_Relink). It owns the schedule, the SUN
the flash drives for two or three frames, the fog light, and the thunder.

WHY THE RELINK WINDOW, when the schedule used to live in the RT composite. Two
reasons, and the second one is the whole of the fog half. A scene light must be
pushed where every other light producer in this tree pushes one: the composite
runs inside R_RenderView AFTER the raster has already drawn its dlight passes, so
a light added there would reach the RT term and the murk but never the walls --
invisible under wall lighting, where no dlight pass runs at all, and wrong at
rt_metal_walllight 0, which is what the Stock and Superfast tiers ship. And it
runs after RT_Metal_SetLights, so the fog would not have seen the light that
frame either.

Consequence, and it is the documented trap: r_refdef.view.origin is NOT valid in
this window. The eye comes from r_refdef.view.matrix through Matrix4x4_ToVectors,
which IS current here (V_CalcRefdef has run, R_SetupView has not) -- the same
source M5_Torch_Relink takes, and the reason the thunder's own origin moved with
the block rather than staying as it was.

THE FOG LIGHT. The sun is a surface term and nothing else -- the fog kernel's
daylight slice is unbuilt -- so before this the flash lit every wall under open
sky and left the air between them exactly as dark as it had been. The M5
thunderbolt has solved the identical problem since its own round: a light whose
slot-7 FOG WEIGHT scales both what it scatters and how loudly it argues in the
fog's dominant-light vote, while the surface kernel never reads slot 7 at all
(rt_metal.m says so at the read). So the flash pushes ONE light up its own
direction carrying the sun's colour DIVIDED by that weight, and the weight
itself: the product is exactly the sun's colour scaled by r_skylightning_fog, so
the fog sees the flash at the strength the sky does, and the surfaces see a
sixtieth of it -- nothing, beside the sun already blasting them.

It is occlusion-aware for free, and that is why it is a point light high above
rather than a lift of the ambient term. The fog kernel shadow-tests its dominant
light; sky brushes are TLAS instance 4 at mask 0x20 and every shadow ray in every
kernel runs at 0x3u, so a ray from fog under open sky passes straight through the
sky and reaches the flash, while a ray from fog in a sealed room stops at its
ceiling. The GL murk march has no shadow rays by design, so on THAT tier the
flash lights fog indoors too; recorded rather than fixed, it is the cheaper tier.
================
*/
// The surface/fog split. colour = sun / WEIGHT and weight = WEIGHT, so the
// product the fog kernel forms is the sun's own colour and the surface kernel --
// which never reads the weight -- gets a sixtieth of it.
#define SKYLIGHTNING_FOGWEIGHT  60.0f
#define SKYLIGHTNING_FOGDIST   1500.0f	// how far up its own direction the light sits, world units
#define SKYLIGHTNING_FOGRADIUS 5000.0f	// a light contributes EXACTLY nothing past its radius, so this is the reach

static vec3_t sl_sundir, sl_suncol;	// this frame's flash, read by RT_SceneComposite's sun block below
static qbool  sl_sunactive;

static void CL_SkyLightning_PushFogLight(const vec3_t eye, const vec3_t dir, const vec3_t suncol)
{
	matrix4x4_t m;
	vec3_t org, c;
	rtlight_t *rtl;
	// the M5 headroom convention: never starve the muzzle flashes and explosions
	// CL_RelinkLightFlashes pushes after us
	if (r_refdef.scene.numlights >= MAX_DLIGHTS - 32)
		return;
	VectorMA(eye, SKYLIGHTNING_FOGDIST, dir, org);
	VectorScale(suncol, 1.0f / SKYLIGHTNING_FOGWEIGHT, c);
	Matrix4x4_CreateFromQuakeEntity(&m, org[0], org[1], org[2], 0, 0, 0, SKYLIGHTNING_FOGRADIUS);
	rtl = &r_refdef.scene.templights[r_refdef.scene.numlights];
	// corona 0 + coronasizescale 0 + flags 0, for the reason CL_Beam_M5_PushLight
	// spells out at length: R_Shadow_DrawCoronas draws an additive screen blob for
	// every scene light with corona > 0, and Seb runs gl_flashblend 1 with
	// r_coronas 1 -- a blob scaled to THIS light's radius is precisely the
	// white-out the flash must not cause.
	R_RTLight_Update(rtl, false, &m, c, -1, NULL, false, 0, 0, 1, 0, 0, 0);
	// AFTER the update, which memsets the whole rtlight. The thunderbolt and the
	// handlamp are the only other producers that set this; see RT_LIGHT_STRIDE.
	rtl->m5fogweight = SKYLIGHTNING_FOGWEIGHT * r_skylightning_fog.value;
	r_refdef.scene.lights[r_refdef.scene.numlights] = rtl;
	r_refdef.scene.numlights++;
}

void CL_SkyLightning_Update(void)
{
	static double sl_next, sl_end, sl_second, sl_secondend, sl_thunder;
	static float sl_yaw, sl_pitch;
	static qbool sl_announced;
	double now = cl.time;
	vec3_t fwd, left, up, eye;

	sl_sunactive = false;
	if (!(r_skylightning.value > 0.0f && rt_metal_walllight.value > 0.0f && rt_world_numskytris > 0
	      && cls.state == ca_connected && cls.signon == SIGNONS))
	{
		sl_next = sl_end = sl_thunder = 0.0;
		return;
	}
	Matrix4x4_ToVectors(&r_refdef.view.matrix, fwd, left, up, eye);
	{
		float period = max(2.0f, r_skylightning_period.value);
		if (sl_next <= 0.0 || now < sl_next - period * 1.5)   // unset, or a map change put the clock back
			sl_next = now + period * lhrandom(0.33f, 1.5f);
		if (now >= sl_next && sl_end <= 0.0)
		{
			sl_yaw = lhrandom(0.0f, 360.0f);
			sl_pitch = -lhrandom(40.0f, 75.0f);
			sl_end = now + bound(0.02f, r_skylightning_hold.value, 5.0f);
			sl_second = sl_end + 0.05 + lhrandom(0.0f, 0.08f);
			sl_secondend = sl_second + 0.035;
			sl_thunder = now + lhrandom(1.0f, 4.0f);
			sl_next = now + period * lhrandom(0.33f, 1.5f);
			if (!sl_announced) { sl_announced = true; Con_DPrintf("sky lightning: flash (yaw %.0f pitch %.0f, thunder in %.1f s)\n", sl_yaw, sl_pitch, sl_thunder - now); }
		}
		if (sl_end > 0.0)
		{
			float gain = 0.0f;
			if (now < sl_end) gain = 1.0f;
			else if (now >= sl_second && now < sl_secondend) gain = 0.6f;
			if (gain > 0.0f)
			{
				float yaw = sl_yaw * (float)(M_PI / 180.0), pitch = sl_pitch * (float)(M_PI / 180.0);
				sl_sundir[0] = -(cosf(pitch) * cosf(yaw));
				sl_sundir[1] = -(cosf(pitch) * sinf(yaw));
				sl_sundir[2] = -sinf(pitch);
				sl_suncol[0] = 0.75f; sl_suncol[1] = 0.85f; sl_suncol[2] = 1.0f;
				VectorScale(sl_suncol, 3.0f * r_skylightning.value * gain, sl_suncol);
				sl_sunactive = true;
				// the fog half. Skipped outright at 0, which is what makes the off
				// switch the old frames rather than a light multiplied by zero.
				if (r_skylightning_fog.value > 0.0f)
					CL_SkyLightning_PushFogLight(eye, sl_sundir, sl_suncol);
			}
			if (sl_thunder > 0.0 && now >= sl_thunder)
			{
				// from ~700 units away in the flash's horizontal direction, so it sends fully into the room reverb and sits on that side
				vec3_t at; float ty = sl_yaw * (float)(M_PI / 180.0);
				at[0] = eye[0] + cosf(ty) * 700.0f; at[1] = eye[1] + sinf(ty) * 700.0f; at[2] = eye[2] + 200.0f;
				S_StartSound(-1, 0, S_PrecacheSound("ambience/thunder1.wav", false, false), at, 1.0f, 0.25f);
				sl_thunder = 0.0;
			}
			if (now >= sl_secondend && sl_thunder <= 0.0)
				sl_end = 0.0;   // this flash is over
		}
	}
}

/*
================
RT_SceneComposite

Gather this frame's RT inputs and composite the ray-traced shadow term into the
scene. Called from R_RenderView (gl_rmain.c) immediately after the scene render,
NOT at the end of the frame: at that point the scene's own depth buffer is still
bound, so the composite can always depth-mask the first-person weapon. The old
end-of-frame hook could only do that on DarkPlaces' trivial-blend path, which any
post-process pass disables — including a v_gamma other than 1, which silently
un-masked the weapon for anyone who had touched their gamma.

Everything it reads is valid at that point: the visible-entity cache and the
animation cache are filled earlier in R_RenderView (R_View_Update /
R_AnimCache_CacheVisibleEntities), and the scene entity/light lists come from
CL_UpdateWorld. r_refdef.view.matrix is the CURRENT view matrix, which inside
R_RenderView still carries the stereo eye offset (the old hook ran after it was
restored, so both eyes traced from the same point).
================
*/
void RT_SceneComposite(int viewfbo, rtexture_t *viewdepthtexture, rtexture_t *viewcolortexture,
                       int viewx, int viewy, int viewwidth, int viewheight, qbool depthsampleable)
{
	model_t *rtworld = r_refdef.scene.worldmodel;
	vec3_t rtfwd, rtleft, rtup, rtorg, rtright;
	int rtstatus;
	// TERM UPSAMPLE (rt_metal_term_upsample, WARCHEST session 1): armed only
	// when the caller vouched that viewdepthtexture is the scene's sampleable
	// depth. The state line is the feature's console evidence (change-only --
	// the console-ink rule); the pixels' own gates prove the rest.
	qbool termup = (depthsampleable && rt_metal_term_upsample.integer) ? true : false;
	{
		static int lastup = -1;
		int up = termup ? 1 : 0;
		if (rt_metal.integer && up != lastup)
		{
			fprintf(stderr, "RT term upsample %s\n", up ? "armed (scene depth sampleable)" : "dormant (no sampleable scene depth)");
			lastup = up;
		}
	}

	if (!rt_metal.integer || scr_loading || !rtworld || rtworld->surfmesh.num_triangles < 1)
		return;
	// Build/reuse the world BLAS geometry from ONLY model *0's own drawable
	// surfaces (NOT the whole shared surfmesh, which includes invisible trigger
	// brushes and other submodels — see RT_BuildWorldGeometry).
	RT_BuildWorldGeometry(rtworld);
	if (rt_world_numtris < 1)
		return;                                             // degenerate world (all surfaces filtered) -> skip RT
	// Token = the world-geometry generation (bumped only when the filtered geometry
	// actually changes), so RT_Metal_SetWorld's BLAS cache invalidates in lockstep
	// with this frame's rt_world_tris/verts — never a stale-index-vs-new-verts mix.
	RT_Metal_SetWorld(rtworld->surfmesh.data_vertex3f, rtworld->surfmesh.num_vertices,
					  rt_world_tris, rt_world_numtris, rt_world_albedo, rt_world_gen);
	// lava sheets ride a separate STATIC emissive instance (own token: a cvar
	// toggle must rebuild only this BLAS, never SetWorld's -- see the statics)
	RT_Metal_SetLavaSurfaces(rt_world_lavatris, rt_world_numlavatris, rt_world_lavagen);
	// sky brushes ride the OPEN SKY instance (rt_metal_skyopen; same own-token shape)
	RT_Metal_SetSkySurfaces(rt_world_skytris, rt_world_numskytris, rt_world_skygen);
	// opaque liquids ride their own EMISSIVE instance (rt_metal_liquidemissive; same own-token shape)
	RT_Metal_SetLiquidSurfaces(rt_world_liqtris, rt_world_numliqtris, rt_world_liqgen);
	// SEPTEMBER2 C2: the blended liquids ride their own instance (mask 0x40; same own-token shape)
	RT_Metal_SetBlendedLiquidSurfaces(rt_world_bliqtris, rt_world_numbliqtris, rt_world_bliqgen);
	Matrix4x4_ToVectors(&r_refdef.view.matrix, rtfwd, rtleft, rtup, rtorg);
	rtright[0] = -rtleft[0]; rtright[1] = -rtleft[1]; rtright[2] = -rtleft[2];
	// upload the lights relevant to this view (culled by distance): this frame's
	// DYNAMIC lights (explosions / rockets / muzzle & player flashes — never baked
	// into lightmaps, so their moving RT shadows are the real payoff) PLUS the
	// map's STATIC worldlights (already baked, so RT mostly duplicates them). The
	// two lists are disjoint. Dynamic lights go FIRST so that if a very dense map
	// overflows the kernel's 256-light cache, the high-value moving lights survive
	// the truncation rather than the static ones.
	{
		static float rt_lightbuf[2048 * RT_LIGHT_STRIDE];
		int nd = RT_GatherDynamicLights(rt_lightbuf, 2048, rtorg, rt_metal_culldist.value);
		int ns = R_Shadow_GetWorldLightPositions(rt_lightbuf + nd * RT_LIGHT_STRIDE, 2048 - nd, rtorg, rt_metal_culldist.value);
		int nl = nd + ns;
		// M5 per-map level normalisation. Under wall lighting the map's own
		// light entities ARE the scene's lighting, delivered with a fixed
		// gain, so a pack whose mapper used dimmer or tighter lights than id1
		// simply renders darker -- Armagon at 0.64/164 against e1m3's
		// 0.71/207 -- and one who used brighter, wider ones renders hotter.
		// Nothing in the pipeline noticed.
		//
		// The gain pulls this map's light budget towards the id1 baseline.
		// Applied ONLY to the static lights: the dynamic ones ahead of them
		// are muzzle flashes, explosions and the venom, which are ours and
		// already calibrated, and the lava lights after them are synthetic.
		// Level only -- every channel gets the same factor, so a pack's
		// authored hue survives exactly.
		//
		// Off in stock Quake by construction, not by calibration: id1 IS the
		// baseline, so there is nothing to correct towards.
		if (m5_packlight.value > 0.0f && r_shadow_maplightbudget > 1.0f && ns > 0 && gamemode != GAME_NORMAL)
		{
			float want = M5_MAPLIGHT_BASELINE / r_shadow_maplightbudget;
			float g = 1.0f + (want - 1.0f) * bound(0.0f, m5_packlight.value, 1.0f);
			g = bound(M5_MAPLIGHT_MINGAIN, g, M5_MAPLIGHT_MAXGAIN);
			if (g != 1.0f)
			{
				int i;
				for (i = nd; i < nl; i++)
				{
					rt_lightbuf[i*RT_LIGHT_STRIDE+4] *= g;
					rt_lightbuf[i*RT_LIGHT_STRIDE+5] *= g;
					rt_lightbuf[i*RT_LIGHT_STRIDE+6] *= g;
				}
			}
		}
		// lava lights (rt_metal_lavalights): constant warm emitters gathered at
		// map load, appended LAST so they truncate first under pressure. The
		// cvar scales the colour; lava is not in the shadow-ray mask, so these
		// are never occluded by their own sheet.
		if (rt_metal_lavalights.value > 0.0f && rt_lava_numlights > 0)
		{
			float s = rt_metal_lavalights.value;
			int i;
			for (i = 0; i < rt_lava_numlights && nl < 2048; i++)
			{
				const float *L = rt_lava_lights + i * 7;
				float dx = L[0] - rtorg[0], dy = L[1] - rtorg[1], dz = L[2] - rtorg[2];
				float reach = L[3] + rt_metal_culldist.value;
				if (dx*dx + dy*dy + dz*dz > reach * reach)
					continue;
				// THE ONE PACK SITE THAT DOES NOT FOLLOW THE STRIDE: rt_lava_lights
				// is its own 7-float-per-light array, so this copies 7 and fills the
				// rest by hand. rt_lightbuf is static and NEVER cleared, and which
				// index a lava light lands at shifts every frame with how many
				// dynamic and static lights survive the cull -- so any slot left
				// unwritten here inherits another light's residue from last frame.
				// Before F6 that was invisible because slot 7 was the last one and
				// it is written just below; the spot slots make it live, and all
				// three kernels read them.
				memcpy(rt_lightbuf + nl * RT_LIGHT_STRIDE, L, sizeof(float) * 7);
				rt_lightbuf[nl*RT_LIGHT_STRIDE+4] *= s;
				rt_lightbuf[nl*RT_LIGHT_STRIDE+5] *= s;
				rt_lightbuf[nl*RT_LIGHT_STRIDE+6] *= s;
				rt_lightbuf[nl*RT_LIGHT_STRIDE+7] = 1.0f;	// lava fogs like any other light
				rt_lightbuf[nl*RT_LIGHT_STRIDE+8]  = 0.0f;	// omni: lava throws in every direction
				rt_lightbuf[nl*RT_LIGHT_STRIDE+9]  = 0.0f;
				rt_lightbuf[nl*RT_LIGHT_STRIDE+10] = 0.0f;
				rt_lightbuf[nl*RT_LIGHT_STRIDE+11] = 0.0f;
				rt_lightbuf[nl*RT_LIGHT_STRIDE+12] = 0.0f;
				rt_lightbuf[nl*RT_LIGHT_STRIDE+13] = 0.0f;
				nl++;
			}
		}
		RT_Metal_SetLights(rt_lightbuf, nl, nd);   // first nd are dynamic (colored brighten)
	}
	// gather this frame's dynamic shadow casters (monsters/items/doors) so they
	// cast RT shadows too (the player's own gun+body are excluded in the gather);
	// flame models ride a separate light-core stream (emissive, never occludes)
	{
		const float *entverts; const int *enttris; const float *entnorms; int entnv = 0;
		const float *lcverts = NULL; const int *lctris = NULL; int lcnv = 0, lcnt = 0;
		int entnt = RT_GatherEntityShadowCasters(&entverts, &entnv, &enttris, &entnorms,
		                                         &lcverts, &lcnv, &lctris, &lcnt);
		RT_Metal_SetEntities(entverts, entnv, enttris, entnt, entnorms);
		RT_Metal_SetLightCores(lcverts, lcnv, lctris, lcnt);
	}
	// push the runtime tuning knobs (samples / softness / darkness / temporal history / colored-light strength / full wall lighting + ambient)
	RT_Metal_SetSameFrame(rt_metal_sameframe.integer);
	RT_Metal_SetReprojectDepth(rt_metal_reproject_depth.integer);
	RT_Metal_SetBlueNoise(rt_metal_bluenoise.integer);
	RT_Metal_SetTuning(rt_metal_samples.integer, rt_metal_softness.value, rt_metal_darkness.value, rt_metal_history.value, rt_metal_color.value, rt_metal_walllight.value, rt_metal_ambient.value, rt_metal_scale.value, rt_metal_reproject.integer);
	RT_Metal_SetShaftTuning(rt_metal_shafts.integer, rt_metal_shafts_samples.integer, rt_metal_shafts_scale.value, rt_metal_shafts_history.value, rt_metal_shafts_dist.value, rt_metal_shafts_residual.value);
	{
		// full in-kernel fog lighting: push this frame's density-model parameters and
		// tuning. The kernel consumes them at the NEXT encode, and its output is shown
		// one frame after that -- cvar edits reach the fog about two frames late.
		rt_fog_shade_t fogshade;
		R_Volumetric_GetFogKernelParams(&fogshade);
		RT_Metal_SetFogShade(&fogshade);
		RT_Metal_SetFogTuning(rt_metal_fog.integer && r_volumetric.integer, rt_metal_fog_steps.integer, rt_metal_fog_scale.value, rt_metal_fog_history.value, rt_metal_fog_intensity.value, rt_metal_fog_stride.integer, rt_metal_fog_residual.value, rt_metal_fog_beams.value);
		// the depth-aware upsample's kernel half (see RT_Metal_SetFogHistCentre):
		// the composite's tap labels and the kernel's march ends must be read at
		// the same place, so the one cvar drives both
		RT_Metal_SetFogHistCentre(rt_metal_fog_upsample.integer);
		RT_Metal_SetFogStepJitter(rt_metal_fog_stepjitter.integer);
		RT_Metal_SetFogFilter(rt_metal_fog_filter.integer, rt_metal_fog_upsample_depth.value);
		RT_Metal_SetFogClamp(rt_metal_fog_clamp.integer, rt_metal_fog_clamp_k.value, rt_metal_fog_tonemapema.integer);
		RT_Metal_SetFogReproject(rt_metal_fog_reproject_depth.integer, rt_metal_fog_reproject_tol.value);
		RT_Metal_SetFogFroxel(rt_metal_fog_froxel.integer, rt_metal_fog_froxel_slices.integer, rt_metal_fog_froxel_history.value, rt_metal_fog_froxel_curve.value, rt_metal_fog_froxel_near.value, rt_metal_fog_froxel_castphase.integer);
		RT_Metal_SetLightSampleHybrid(rt_metal_lightsample_hybrid.integer);
	}
	RT_Metal_SetTermMax(rt_metal_lmax.value);
	RT_Metal_SetGIAO(rt_metal_gi_ao.value, rt_metal_gi_ao_dist.value);   // BEAUTY B1
	RT_Metal_SetFogLiquidLight(rt_metal_fog_liquidlight.value);   // BEAUTY B2
	RT_Metal_SetContact(rt_metal_contact.value);   // BEAUTY B3
	RT_Metal_SetShadowLights(rt_metal_shadowlights.integer, rt_metal_shadowlights_rays.integer);
	RT_Metal_SetLightSample(rt_metal_lightsample.integer, rt_metal_lightsample_rays.integer, rt_metal_lightsample_clamp.value);
	RT_Metal_SetGI(rt_metal_gi.integer, rt_metal_gi_dist.value, rt_metal_gi_albedo.value, rt_metal_gi_history.value, rt_metal_gi_intensity.value, rt_metal_gi_emissive.value, rt_metal_gi_rate.integer, rt_metal_gi_albedo_tex.value, rt_metal_gi_fallback.integer, rt_metal_gi_tiledilate.value);
	RT_Metal_SetRefit(rt_metal_refit.integer);
	RT_Metal_SetASSkip(rt_metal_as_skipstatic.integer);
	// SEPTEMBER2 C2: the liquid pair needs the surface consumer live (rt_metal_liquids > 0) and the wall-lighting arm
	RT_Metal_SetLiquidRT(rt_metal_liquids_rt.integer && rt_metal_liquids.value > 0.0f && rt_metal_walllight.value > 0.0f, rt_metal_liquids_reflect.value);
	// SEPTEMBER2 D: the sky light -- the map's ericw sun keys, each overridable by a cvar.
	// ericw's mangle is "yaw pitch roll" with NEGATIVE pitch shining DOWN (the light's
	// travel direction, mathematical pitch, not Quake's inverted one); the kernel wants
	// the unit vector TOWARD the sun, so the travel vector is built directly and negated.
	{
		float sunlight = rt_metal_sun_light.value > 0.0f ? rt_metal_sun_light.value : cl.m5sun_light;
		float mangle[3], col[3], dir[3], pen;
		int have = cl.m5sun_havemangle;
		VectorCopy(cl.m5sun_mangle, mangle);
		if (rt_metal_sun_mangle.string[0] && sscanf(rt_metal_sun_mangle.string, "%f %f", &mangle[0], &mangle[1]) == 2) have = 1;
		VectorCopy(cl.m5sun_color, col);
		if (rt_metal_sun_color.string[0] && sscanf(rt_metal_sun_color.string, "%f %f %f", &col[0], &col[1], &col[2]) == 3)
			if (col[0] > 1.0f || col[1] > 1.0f || col[2] > 1.0f) VectorScale(col, 1.0f / 255.0f, col);
		pen = rt_metal_sun_penumbra.value >= 0.0f ? rt_metal_sun_penumbra.value : cl.m5sun_penumbra;
		// BEAUTY C1: on the frames it is lit, the flash OWNS the sun. The schedule,
		// the fog light and the thunder all live in CL_SkyLightning_Update, which runs
		// once per frame in the relink window -- see its comment for why a light
		// cannot be pushed from here. This is only the read.
		if (sl_sunactive)
		{
			RT_Metal_SetSun(sl_sundir, sl_suncol, 1.0f, 1);
			have = -1;   // the flash has set the sun this frame; the block below must not overwrite it
		}
		if (have == -1)
			;
		else if (rt_metal_sun.value > 0.0f && sunlight > 0.0f && have)
		{
			float yaw = mangle[0] * (float)(M_PI / 180.0), pitch = mangle[1] * (float)(M_PI / 180.0);
			dir[0] = -(cosf(pitch) * cosf(yaw)); dir[1] = -(cosf(pitch) * sinf(yaw)); dir[2] = -sinf(pitch);
			VectorScale(col, sunlight / 256.0f * rt_metal_sun.value, col);   // light/256, the loader's own unit for a map light
			RT_Metal_SetSun(dir, col, pen, 1);
		}
		else
			RT_Metal_SetSun(NULL, NULL, 0.0f, 0);
	}
	RT_Metal_SetFogAdaptiveStride(rt_metal_fog_stride_adaptive.integer);
	RT_Metal_SetCamera(rtorg, rtfwd, rtright, rtup, r_refdef.view.frustum_x, r_refdef.view.frustum_y);
	// Trace at the 3D VIEWPORT resolution and tell the composite where that viewport
	// sits in the bound framebuffer (viewport.x/y are already GL bottom-left, with the
	// window-vs-FBO y flip applied by R_SetupView). The previous hook always traced at
	// full window size, which silently misaligned the term whenever the view was a
	// subregion (scr_viewsize < 100, r_letterbox, side-by-side stereo). The scene depth
	// is always this frame's here, so the view-model mask is always allowed (1).
	// SEPTEMBER2 A2 (rt_metal_pipeline): put the raster on the GPU NOW, so it runs
	// beside the trace the composite is about to commit and wait for (the
	// mechanism is in Metal_Backend_Kick's comment). Metal path only; the GL
	// bridge has no command buffer to kick.
	{
		static int lastkick = -1;
		int kick = (vid.renderpath == RENDERPATH_METAL && rt_metal_pipeline.integer) ? 1 : 0;
		if (kick != lastkick)   // change-only: a reordering is invisible to every counter and pixel, so this line is its liveness evidence (smoke greps it)
		{
			lastkick = kick;
			Con_Printf("RT pipelining: %s\n", kick ? "raster committed ahead of the trace" : "off (the trace, then the whole frame)");
		}
		if (kick)
			Metal_Backend_Kick();
	}
	rtstatus = RT_Metal_Composite(r_refdef.view.viewport.width, r_refdef.view.viewport.height, 1,
					   r_refdef.view.viewport.x, r_refdef.view.viewport.y,
					   termup ? (unsigned int)R_GetTexture(viewdepthtexture) : 0u,
					   r_refdef.view.viewport.screentodepth,
					   bound(0.001f, rt_metal_term_upsample_depth.value, 0.5f));

	if (rtstatus == RT_COMPOSITE_PUBLISHED)
	{
		// METAL.md Phase 5 slice 3. On the Metal renderpath the sidecar traces and
		// PUBLISHES its term buffer, and the multiply is drawn from here as an
		// ordinary backend draw -- no GL context, no rectangle textures, no state
		// save/restore around seventeen glGet readbacks, and no shader of its own.
		//
		// Shape copied from the murk's kernel-fog composite (R_Volumetric_RenderFog,
		// gl_rmain.c), whose comment already names this composite's depth trick.
		// GL_DepthRange collapsed to a single value pins every fragment of the quad
		// at window depth 0.0625, so it depth-tests against the scene and leaves the
		// first-person weapon (drawn nearer than that) unmultiplied.
		//
		// The shader is MODE_RTCOMPOSITE, the mechanical port of the bridge's own
		// kCompFS (slice 5-4). Slice 5-3 used the stock GENERIC shader, which was
		// exactly right while there was nothing to do but multiply -- REFLECTCUBE
		// means "multiply RGB, drop the texture's alpha" there and VIEWTINT seeds
		// the colour from GL_Color, so white vertex colour reproduced the bridge's
		// vec4(term.rgb, 1.0) with nothing new compiled. REPROJECTION is what it
		// cannot express: remapping each pixel through the camera the term was
		// traced with is per-pixel ray arithmetic, not a texcoord transform.
		//
		// rtoff and rtscale still dissolve rather than being ported: the viewport
		// transform absorbs the origin, and normalised texcoords over a trace-sized
		// texture absorb rt_metal_scale. Both uniforms exist on the GL side only
		// because a sampler2DRect addresses in texels.
		unsigned int rttermtex = 0;
		int rttermw = 0, rttermh = 0;
		rt_reproj_t rtrp;
		qbool rthasrp;

		if (RT_Metal_GetTermTexture(&rttermtex, &rttermw, &rttermh))
		{
			// The full-texture case of R_CalcTexCoordsForView, which every other
			// screen-quad draw in the engine pairs with r_screenvertex3f: the v flip
			// is built in, because render targets store GL-layout images and the
			// kernel writes GL window rows (its row 0 is the BOTTOM).
			static const float rttc[8] = {0,1, 1,1, 1,0, 0,0};
			// ...and the deliberately wrong one. A flip error here is a
			// 180-degree-rotated shadow term, which on a roughly symmetric frame
			// reads as "the RT looks a bit off" rather than as a bug -- so the
			// orientation is proven by showing that this variant MOVES pixels,
			// not by reasoning about it. RT_METAL_COMPOSITE_FLIP=1.
			static const float rttcflip[8] = {0,0, 1,0, 1,1, 0,1};
			static int rtflip = -1;
			if (rtflip < 0)
			{
				const char *e = getenv("RT_METAL_COMPOSITE_FLIP");
				rtflip = (e && atoi(e)) ? 1 : 0;
				if (rtflip)
					Con_Printf(CON_WARN "RT_METAL_COMPOSITE_FLIP: compositing the RT term upside down on purpose\n");
			}

			R_ResetViewRendering2D(viewfbo, viewdepthtexture, viewcolortexture, viewx, viewy, viewwidth, viewheight);
			GL_DepthTest(true);
			GL_DepthFunc(GL_LESS);
			GL_DepthMask(false);
			GL_DepthRange(0.0625f, 0.0625f);
			GL_CullFace(GL_NONE);
			GL_PolygonOffset(0, 0);
			GL_BlendFunc(GL_DST_COLOR, GL_ZERO);   // scene *= term
			GL_Color(1, 1, 1, 1);
			R_Mesh_PrepareVertices_Generic_Arrays(4, r_screenvertex3f, NULL, rtflip ? rttcflip : rttc);
			// The reprojection block, or nothing -- RT_Metal_GetReprojection
			// returns 0 when there is no snapshotted camera to remap against or
			// rt_metal_reproject is off, and the shader's disabled path is then
			// bit-for-bit the slice 5-3 composite.
			rthasrp = RT_Metal_GetReprojection(&rtrp) ? true : false;
			R_SetupShader_RTComposite(rttermtex, rthasrp ? &rtrp : NULL, termup ? viewdepthtexture : NULL, rttermw, rttermh);
			R_Mesh_Draw(0, 4, 0, 2, polygonelement3i, NULL, 0, polygonelement3s, NULL, 0);
			GL_DepthRange(0, 1);
			GL_DepthFunc(GL_LEQUAL);
			GL_DepthTest(false);
			R_ResetViewRendering3D(viewfbo, viewdepthtexture, viewcolortexture, viewx, viewy, viewwidth, viewheight);

			// Only now is the RT really relighting the frame. Everything above can
			// bail, and every way of bailing leaves RT inactive -- which is the
			// fallback SCR_DrawScreen's fullbright forcing needs, rather than a
			// world rendered as raw albedo with no compensating multiply.
			RT_Metal_MarkComposited();
			// A2 mode 2: commit the composite too, so the GPU has it while the CPU
			// encodes the murk, the post and the HUD (the bubble after the wait --
			// see Metal_Backend_Kick's comment; mode 1's single kick measured nil)
			if (rt_metal_pipeline.integer >= 2)
				Metal_Backend_Kick();

			// The same one-shot state line the GL bridge prints, so one grep serves
			// both paths (tests/smoke.sh asserts on it). Always ACTIVE here: this
			// hook is called with the scene depth bound, which is why the call above
			// passes scenedepthvalid 1 unconditionally.
			{
				static int rtreported;
				if (!rtreported)
				{
					rtreported = 1;
					fprintf(stderr, "RT_Metal: composite viewmodel mask %s (%dx%d at %d,%d)\n",
							"ACTIVE", rttermw, rttermh, viewx, viewy);
				}
			}
		}
	}
}
#else
// BEAUTY C1: the storm drives the RT sun and pushes a light the RT fog kernel
// reads, so on a build without the sidecar there is nothing for it to do. The
// relink window calls it unconditionally from cl_main.c -- which the DEDICATED
// SERVER links -- and this is the arm that keeps `make sv-release` linking; see
// the standing rule about a client-only home for a symbol a common file calls.
void CL_SkyLightning_Update(void) { }
#endif // USE_RT_METAL

static void SCR_DrawScreen (void)
{
	Draw_Frame();
	DrawQ_Start();
	R_Mesh_Start();
	R_UpdateVariables();

	// Quake uses clockwise winding, so these are swapped
	r_refdef.view.cullface_front = GL_BACK;
	r_refdef.view.cullface_back = GL_FRONT;

#ifdef USE_RT_METAL
	// Full RT wall-lighting mode renders the WORLD unlit (albedo) via r_fullbright, so
	// the Metal RT composite can multiply its own per-pixel lighting onto it (replacing
	// the baked lightmap). Force it here — BEFORE R_RenderView below — whenever the mode
	// is active, and restore it when the mode turns off (only if WE forced it, so a user
	// who set r_fullbright by hand is left alone). Must precede the scene render.
	{
		static qbool rt_forced_fullbright = false;
		// Gate on RT_Metal_Active(): only force fullbright when the RT composite is ACTUALLY
		// relighting (device up + last frame composited). On a Mac where Metal RT init failed
		// (no device / no raytracing / kernel compile error) or a frame where the composite
		// bailed, this stays false, so we never render a blinding fullbright world without the
		// compensating albedo*Lrt multiply. (One-frame lag is harmless: the previous frame's
		// over-lightmap composite already sets it, so enabling wall-lighting transitions cleanly.)
		qbool want_fb = (rt_metal.integer && rt_metal_walllight.value > 0.0f && RT_Metal_Active()
		                 && !scr_loading && cls.signon == SIGNONS);
		if (want_fb && !r_fullbright.integer)
		{
			Cvar_SetValueQuick(&r_fullbright, 1);
			rt_forced_fullbright = true;
		}
		else if (!want_fb && rt_forced_fullbright)
		{
			Cvar_SetValueQuick(&r_fullbright, 0);
			rt_forced_fullbright = false;
		}
	}
#endif

	if (!scr_loading && cls.signon == SIGNONS)
	{
		float size;

		size = scr_viewsize.value * (1.0 / 100.0);
		size = min(size, 1);

		if (r_stereo_sidebyside.integer)
		{
			r_refdef.view.width = (int)(vid.mode.width * size / 2.5);
			r_refdef.view.height = (int)(vid.mode.height * size / 2.5 * (1 - bound(0, r_letterbox.value, 100) / 100));
			r_refdef.view.depth = 1;
			r_refdef.view.x = (int)((vid.mode.width - r_refdef.view.width * 2.5) * 0.5);
			r_refdef.view.y = (int)((vid.mode.height - r_refdef.view.height)/2);
			r_refdef.view.z = 0;
			if (r_stereo_side)
				r_refdef.view.x += (int)(r_refdef.view.width * 1.5);
		}
		else if (r_stereo_horizontal.integer)
		{
			r_refdef.view.width = (int)(vid.mode.width * size / 2);
			r_refdef.view.height = (int)(vid.mode.height * size * (1 - bound(0, r_letterbox.value, 100) / 100));
			r_refdef.view.depth = 1;
			r_refdef.view.x = (int)((vid.mode.width - r_refdef.view.width * 2.0)/2);
			r_refdef.view.y = (int)((vid.mode.height - r_refdef.view.height)/2);
			r_refdef.view.z = 0;
			if (r_stereo_side)
				r_refdef.view.x += (int)(r_refdef.view.width);
		}
		else if (r_stereo_vertical.integer)
		{
			r_refdef.view.width = (int)(vid.mode.width * size);
			r_refdef.view.height = (int)(vid.mode.height * size * (1 - bound(0, r_letterbox.value, 100) / 100) / 2);
			r_refdef.view.depth = 1;
			r_refdef.view.x = (int)((vid.mode.width - r_refdef.view.width)/2);
			r_refdef.view.y = (int)((vid.mode.height - r_refdef.view.height * 2.0)/2);
			r_refdef.view.z = 0;
			if (r_stereo_side)
				r_refdef.view.y += (int)(r_refdef.view.height);
		}
		else
		{
			r_refdef.view.width = (int)(vid.mode.width * size);
			r_refdef.view.height = (int)(vid.mode.height * size * (1 - bound(0, r_letterbox.value, 100) / 100));
			r_refdef.view.depth = 1;
			r_refdef.view.x = (int)((vid.mode.width - r_refdef.view.width)/2);
			r_refdef.view.y = (int)((vid.mode.height - r_refdef.view.height)/2);
			r_refdef.view.z = 0;
		}

		// LadyHavoc: viewzoom (zoom in for sniper rifles, etc)
		// LadyHavoc: this is designed to produce widescreen fov values
		// when the screen is wider than 4/3 width/height aspect, to do
		// this it simply assumes the requested fov is the vertical fov
		// for a 4x3 display, if the ratio is not 4x3 this makes the fov
		// higher/lower according to the ratio
		r_refdef.view.useperspective = true;
		r_refdef.view.frustum_y = tan(scr_fov.value * M_PI / 360.0) * (3.0 / 4.0) * cl.viewzoom;
		r_refdef.view.frustum_x = r_refdef.view.frustum_y * (float)r_refdef.view.width / (float)r_refdef.view.height / vid_pixelheight.value;

		r_refdef.view.frustum_x *= r_refdef.frustumscale_x;
		r_refdef.view.frustum_y *= r_refdef.frustumscale_y;
		r_refdef.view.ortho_x = atan(r_refdef.view.frustum_x) * (360.0 / M_PI); // abused as angle by VM_CL_R_SetView
		r_refdef.view.ortho_y = atan(r_refdef.view.frustum_y) * (360.0 / M_PI); // abused as angle by VM_CL_R_SetView

		r_refdef.view.ismain = true;

		// if CSQC is loaded, it is required to provide the CSQC_UpdateView function,
		// and won't render a view if it does not call that.
		if (CLVM_prog->loaded && !(CLVM_prog->flag & PRVM_CSQC_SIMPLE))
			CL_VM_UpdateView(r_stereo_side ? 0.0 : max(0.0, cl.time - cl.oldtime));
		else
		{
			// Prepare the scene mesh for rendering - this is lightning beams and other effects rendered as normal surfaces
			CL_MeshEntities_Scene_FinalizeRenderEntity();

			CL_UpdateEntityShading();
			R_RenderView(0, NULL, NULL, r_refdef.view.x, r_refdef.view.y, r_refdef.view.width, r_refdef.view.height);
		}
	}

	// Don't apply debugging stuff like r_showsurfaces to the UI
	r_refdef.view.showdebug = false;

	if (!r_stereo_sidebyside.integer && !r_stereo_horizontal.integer && !r_stereo_vertical.integer)
	{
		r_refdef.view.width = vid.mode.width;
		r_refdef.view.height = vid.mode.height;
		r_refdef.view.depth = 1;
		r_refdef.view.x = 0;
		r_refdef.view.y = 0;
		r_refdef.view.z = 0;
		r_refdef.view.useperspective = false;
	}

	if (cls.timedemo && cls.td_frames > 0 && timedemo_screenshotframelist.string && timedemo_screenshotframelist.string[0])
	{
		const char *t;
		int framenum;
		t = timedemo_screenshotframelist.string;
		while (*t)
		{
			while (*t == ' ')
				t++;
			if (!*t)
				break;
			framenum = atof(t);
			if (framenum == cls.td_frames)
				break;
			while (*t && *t != ' ')
				t++;
		}
		if (*t)
		{
			// we need to take a screenshot of this frame...
			char filename[MAX_QPATH];
			unsigned char *buffer1;
			unsigned char *buffer2;
			dpsnprintf(filename, sizeof(filename), "timedemoscreenshots/%s%06d.tga", cls.demoname, cls.td_frames);
			buffer1 = (unsigned char *)Mem_Alloc(tempmempool, vid.mode.width * vid.mode.height * 4);
			buffer2 = (unsigned char *)Mem_Alloc(tempmempool, vid.mode.width * vid.mode.height * 3);
			SCR_ScreenShot(filename, buffer1, buffer2, 0, 0, vid.mode.width, vid.mode.height, false, false, false, false, false, true, false);
			Mem_Free(buffer1);
			Mem_Free(buffer2);
		}
	}

	// draw 2D stuff

	if(!scr_con_current && !(key_consoleactive & KEY_CONSOLEACTIVE_FORCED))
		if ((key_dest == key_game || key_dest == key_message) && !r_letterbox.value && !scr_loading)
			Con_DrawNotify ();	// only draw notify in game

	if (cl.islocalgame && (key_dest != key_game || key_consoleactive))
		host.paused = true;
	else
		host.paused = false;

	if (!scr_loading && cls.signon == SIGNONS)
	{
		SCR_DrawNet ();
		SCR_DrawTurtle ();
		SCR_DrawPause ();
		if (!r_letterbox.value)
		{
			Sbar_Draw();
			if (CLVM_prog->loaded && CLVM_prog->flag & PRVM_CSQC_SIMPLE)
				CL_VM_DrawHud(r_stereo_side ? 0.0 : max(0.0, cl.time - cl.oldtime));
		}
		SHOWLMP_drawall();
		SCR_CheckDrawCenterString();
	}
	SCR_DrawNetGraph ();
#ifdef CONFIG_MENU
	if(!scr_loading)
		MR_Draw();
#endif
	CL_DrawVideo();
	R_Shadow_EditLights_DrawSelectedLightProperties();

	if (scr_loading)
	{
		// connect_status replaces any dummy_status
		if ((!loadingscreenstack || loadingscreenstack->msg[0] == '\0') && cl_connect_status[0] != '\0')
		{
			loadingscreenstack_t connect_status, *og_ptr = loadingscreenstack;

			connect_status.absolute_loading_amount_min = 0;
			dp_strlcpy(connect_status.msg, cl_connect_status, sizeof(cl_connect_status));
			loadingscreenstack = &connect_status;
			SCR_DrawLoadingScreen();
			loadingscreenstack = og_ptr;
		}
		else
			SCR_DrawLoadingScreen();
	}

	SCR_DrawConsole();
	SCR_DrawInfobar();

	if (!scr_loading)
	{
		SCR_DrawBrand();
		SCR_DrawTouchscreenOverlay();
	}
	if (r_timereport_active)
		R_TimeReport("2d");

	R_TimeReport_EndFrame();
	R_TimeReport_BeginFrame();
	
	if(!scr_loading)
		Sbar_ShowFPS();
		M5_ReelLabel_Draw();

	R_Mesh_Finish();
	DrawQ_Finish();
	R_RenderTarget_FreeUnused(false);
}

static void SCR_ClearLoadingScreenTexture(void)
{
	if(loadingscreentexture)
		R_FreeTexture(loadingscreentexture);
	loadingscreentexture = NULL;
}

extern rtexturepool_t *r_main_texturepool;
static void SCR_SetLoadingScreenTexture(void)
{
	int w, h;
	float loadingscreentexture_w;
	float loadingscreentexture_h;

	SCR_ClearLoadingScreenTexture();

	w = vid.mode.width; h = vid.mode.height;
	loadingscreentexture_w = loadingscreentexture_h = 1;

	loadingscreentexture = R_LoadTexture2D(r_main_texturepool, "loadingscreentexture", w, h, NULL, TEXTYPE_COLORBUFFER, TEXF_RENDERTARGET | TEXF_FORCENEAREST | TEXF_CLAMP, -1, NULL);
	R_Mesh_CopyToTexture(loadingscreentexture, 0, 0, 0, 0, vid.mode.width, vid.mode.height);

	loadingscreentexture_vertex3f[2] = loadingscreentexture_vertex3f[5] = loadingscreentexture_vertex3f[8] = loadingscreentexture_vertex3f[11] = 0;
	loadingscreentexture_vertex3f[0] = loadingscreentexture_vertex3f[9] = 0;
	loadingscreentexture_vertex3f[1] = loadingscreentexture_vertex3f[4] = 0;
	loadingscreentexture_vertex3f[3] = loadingscreentexture_vertex3f[6] = vid_conwidth.integer;
	loadingscreentexture_vertex3f[7] = loadingscreentexture_vertex3f[10] = vid_conheight.integer;
	loadingscreentexture_texcoord2f[0] = 0;loadingscreentexture_texcoord2f[1] = loadingscreentexture_h;
	loadingscreentexture_texcoord2f[2] = loadingscreentexture_w;loadingscreentexture_texcoord2f[3] = loadingscreentexture_h;
	loadingscreentexture_texcoord2f[4] = loadingscreentexture_w;loadingscreentexture_texcoord2f[5] = 0;
	loadingscreentexture_texcoord2f[6] = 0;loadingscreentexture_texcoord2f[7] = 0;
}

static void SCR_ChooseLoadingPic(qbool startup)
{
	if(startup && scr_loadingscreen_firstforstartup.integer)
		loadingscreenpic_number = 0;
	else if(scr_loadingscreen_firstforstartup.integer)
		if(scr_loadingscreen_count.integer > 1)
			loadingscreenpic_number = rand() % (scr_loadingscreen_count.integer - 1) + 1;
		else
			loadingscreenpic_number = 0;
	else
		loadingscreenpic_number = rand() % (scr_loadingscreen_count.integer > 1 ? scr_loadingscreen_count.integer : 1);
}

/*
===============
SCR_BeginLoadingPlaque

================
*/
void SCR_BeginLoadingPlaque(qbool startup)
{
	loadingscreenstack_t dummy_status;

	// we need to push a dummy status so CL_UpdateScreen knows we have things to load...
	if (!loadingscreenstack)
	{
		dummy_status.msg[0] = '\0';
		dummy_status.absolute_loading_amount_min = 0;
		loadingscreenstack = &dummy_status;
	}

	SCR_DeferLoadingPlaque(startup);
	if (scr_loadingscreen_background.integer)
		SCR_SetLoadingScreenTexture();
	CL_UpdateScreen();

	if (loadingscreenstack == &dummy_status)
		loadingscreenstack = NULL;
}

void SCR_DeferLoadingPlaque(qbool startup)
{
	SCR_ChooseLoadingPic(startup);
	scr_loading = true;
}

void SCR_EndLoadingPlaque(void)
{
	scr_loading = false;
	SCR_ClearLoadingScreenTexture();
}

//=============================================================================

void SCR_PushLoadingScreen (const char *msg, float len_in_parent)
{
	loadingscreenstack_t *s = (loadingscreenstack_t *) Z_Malloc(sizeof(loadingscreenstack_t));
	s->prev = loadingscreenstack;
	loadingscreenstack = s;

	dp_strlcpy(s->msg, msg, sizeof(s->msg));
	s->relative_completion = 0;

	if(s->prev)
	{
		s->absolute_loading_amount_min = s->prev->absolute_loading_amount_min + s->prev->absolute_loading_amount_len * s->prev->relative_completion;
		s->absolute_loading_amount_len = s->prev->absolute_loading_amount_len * len_in_parent;
		if(s->absolute_loading_amount_len > s->prev->absolute_loading_amount_min + s->prev->absolute_loading_amount_len - s->absolute_loading_amount_min)
			s->absolute_loading_amount_len = s->prev->absolute_loading_amount_min + s->prev->absolute_loading_amount_len - s->absolute_loading_amount_min;
	}
	else
	{
		s->absolute_loading_amount_min = 0;
		s->absolute_loading_amount_len = 1;
	}

	if (scr_loading)
		CL_UpdateScreen();
}

void SCR_PopLoadingScreen (qbool redraw)
{
	loadingscreenstack_t *s = loadingscreenstack;

	if(!s)
	{
		Con_DPrintf("Popping a loading screen item from an empty stack!\n");
		return;
	}

	loadingscreenstack = s->prev;
	if(s->prev)
		s->prev->relative_completion = (s->absolute_loading_amount_min + s->absolute_loading_amount_len - s->prev->absolute_loading_amount_min) / s->prev->absolute_loading_amount_len;
	Z_Free(s);

	if (scr_loading && redraw)
		CL_UpdateScreen();
}

void SCR_ClearLoadingScreen (qbool redraw)
{
	while(loadingscreenstack)
		SCR_PopLoadingScreen(redraw && !loadingscreenstack->prev);
}

static float SCR_DrawLoadingStack_r(loadingscreenstack_t *s, float y, float size)
{
	float x;
	size_t len;
	float total;

	total = 0;
#if 0
	if(s)
	{
		total += SCR_DrawLoadingStack_r(s->prev, y, 8);
		y -= total;
		if(!s->prev || strcmp(s->msg, s->prev->msg))
		{
			len = strlen(s->msg);
			x = (vid_conwidth.integer - DrawQ_TextWidth(s->msg, len, size, size, true, FONT_INFOBAR)) / 2;
			y -= size;
			DrawQ_String(x, y, s->msg, len, size, size, 1, 1, 1, 1, 0, NULL, true, FONT_INFOBAR);
			total += size;
		}
	}
#else
	if(s)
	{
		len = strlen(s->msg);
		x = (vid_conwidth.integer - DrawQ_TextWidth(s->msg, len, size, size, true, FONT_INFOBAR)) / 2;
		y -= size;
		DrawQ_String(x, y, s->msg, len, size, size, 1, 1, 1, 1, 0, NULL, true, FONT_INFOBAR);
		total += size;
	}
#endif
	return total;
}

static void SCR_DrawLoadingStack(void)
{
	float verts[12];
	float colors[16];

	SCR_DrawLoadingStack_r(loadingscreenstack, vid_conheight.integer, scr_loadingscreen_barheight.value);
	if(loadingscreenstack)
	{
		// height = 32; // sorry, using the actual one is ugly
		GL_BlendFunc(GL_SRC_ALPHA, GL_ONE);
		GL_DepthRange(0, 1);
		GL_PolygonOffset(0, 0);
		GL_DepthTest(false);
		//R_Mesh_ResetTextureState();
		verts[2] = verts[5] = verts[8] = verts[11] = 0;
		verts[0] = verts[9] = 0;
		verts[1] = verts[4] = vid_conheight.integer - scr_loadingscreen_barheight.value;
		verts[3] = verts[6] = vid_conwidth.integer * loadingscreenstack->absolute_loading_amount_min;
		verts[7] = verts[10] = vid_conheight.integer;
		
#if _MSC_VER >= 1400
#define sscanf sscanf_s
#endif
		colors[0] = 0; colors[1] = 0; colors[2] = 0; colors[3] = 1;
		colors[4] = 0; colors[5] = 0; colors[6] = 0; colors[7] = 1;
		sscanf(scr_loadingscreen_barcolor.string, "%f %f %f", &colors[8], &colors[9], &colors[10]); colors[11] = 1;
		sscanf(scr_loadingscreen_barcolor.string, "%f %f %f", &colors[12], &colors[13], &colors[14]);  colors[15] = 1;

		R_Mesh_PrepareVertices_Generic_Arrays(4, verts, colors, NULL);
		R_SetupShader_Generic_NoTexture(true, true);
		R_Mesh_Draw(0, 4, 0, 2, polygonelement3i, NULL, 0, polygonelement3s, NULL, 0);
	}
}

static void SCR_DrawLoadingScreen (void)
{
	cachepic_t *loadingscreenpic;
	float loadingscreenpic_vertex3f[12];
	float loadingscreenpic_texcoord2f[8];
	float x, y, w, h, sw, sh, f;
	char vabuf[1024];

	GL_BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	GL_DepthRange(0, 1);
	GL_PolygonOffset(0, 0);
	GL_DepthTest(false);
	GL_Color(1,1,1,1);

	if(loadingscreentexture)
	{
		R_Mesh_PrepareVertices_Generic_Arrays(4, loadingscreentexture_vertex3f, NULL, loadingscreentexture_texcoord2f);
		R_SetupShader_Generic(loadingscreentexture, false, true, true);
		R_Mesh_Draw(0, 4, 0, 2, polygonelement3i, NULL, 0, polygonelement3s, NULL, 0);
	}

	loadingscreenpic = Draw_CachePic_Flags(loadingscreenpic_number ? va(vabuf, sizeof(vabuf), "%s%d", scr_loadingscreen_picture.string, loadingscreenpic_number+1) : scr_loadingscreen_picture.string, loadingscreenpic_number ? CACHEPICFLAG_NOTPERSISTENT : 0);
	w = Draw_GetPicWidth(loadingscreenpic);
	h = Draw_GetPicHeight(loadingscreenpic);

	// apply scale
	w *= scr_loadingscreen_scale.value;
	h *= scr_loadingscreen_scale.value;

	// apply scale base
	if(scr_loadingscreen_scale_base.integer)
	{
		w *= vid_conwidth.integer / (float) vid.mode.width;
		h *= vid_conheight.integer / (float) vid.mode.height;
	}

	// apply scale limit
	sw = w / vid_conwidth.integer;
	sh = h / vid_conheight.integer;
	f = 1;
	switch(scr_loadingscreen_scale_limit.integer)
	{
		case 1:
			f = max(sw, sh);
			break;
		case 2:
			f = min(sw, sh);
			break;
		case 3:
			f = sw;
			break;
		case 4:
			f = sh;
			break;
	}
	if(f > 1)
	{
		w /= f;
		h /= f;
	}

	x = (vid_conwidth.integer - w)/2;
	y = (vid_conheight.integer - h)/2;
	loadingscreenpic_vertex3f[2] = loadingscreenpic_vertex3f[5] = loadingscreenpic_vertex3f[8] = loadingscreenpic_vertex3f[11] = 0;
	loadingscreenpic_vertex3f[0] = loadingscreenpic_vertex3f[9] = x;
	loadingscreenpic_vertex3f[1] = loadingscreenpic_vertex3f[4] = y;
	loadingscreenpic_vertex3f[3] = loadingscreenpic_vertex3f[6] = x + w;
	loadingscreenpic_vertex3f[7] = loadingscreenpic_vertex3f[10] = y + h;
	loadingscreenpic_texcoord2f[0] = 0;loadingscreenpic_texcoord2f[1] = 0;
	loadingscreenpic_texcoord2f[2] = 1;loadingscreenpic_texcoord2f[3] = 0;
	loadingscreenpic_texcoord2f[4] = 1;loadingscreenpic_texcoord2f[5] = 1;
	loadingscreenpic_texcoord2f[6] = 0;loadingscreenpic_texcoord2f[7] = 1;

	R_Mesh_PrepareVertices_Generic_Arrays(4, loadingscreenpic_vertex3f, NULL, loadingscreenpic_texcoord2f);
	R_SetupShader_Generic(Draw_GetPicTexture(loadingscreenpic), true, true, false);
	R_Mesh_Draw(0, 4, 0, 2, polygonelement3i, NULL, 0, polygonelement3s, NULL, 0);

	SCR_DrawLoadingStack();
}

qbool R_Stereo_ColorMasking(void)
{
	return r_stereo_redblue.integer || r_stereo_redgreen.integer || r_stereo_redcyan.integer;
}

qbool R_Stereo_Active(void)
{
	return (vid.mode.stereobuffer || r_stereo_sidebyside.integer || r_stereo_horizontal.integer || r_stereo_vertical.integer || R_Stereo_ColorMasking());
}

static void SCR_UpdateVars(void)
{
	float conwidth = bound(160, vid_conwidth.value, 32768);
	float conheight = bound(90, vid_conheight.value, 24576);
	float conscale = vid.mode.height / vid_conheight.value;
	if (vid_conwidthauto.integer)
		conwidth = floor(conheight * vid.mode.width / (vid.mode.height * vid_pixelheight.value));
	if (vid_conwidth.value != conwidth)
		Cvar_SetValueQuick(&vid_conwidth, conwidth);
	if (vid_conheight.value != conheight)
		Cvar_SetValueQuick(&vid_conheight, conheight);
	if (scr_sbarscale.value != conscale)
		Cvar_SetValueQuick(&scr_sbarscale, conscale);

	// bound viewsize
	if (scr_viewsize.value < 30)
		Cvar_SetValueQuick(&scr_viewsize, 30);
	if (scr_viewsize.value > 120)
		Cvar_SetValueQuick(&scr_viewsize, 120);

	// bound field of view
	if (scr_fov.value < 1)
		Cvar_SetValueQuick(&scr_fov, 1);
	if (scr_fov.value > 170)
		Cvar_SetValueQuick(&scr_fov, 170);

	// intermission is always full screen
	if (cl.intermission)
		sb_lines = 0;
	else
	{
		if (scr_viewsize.value >= 120)
			sb_lines = 0;		// no status bar at all
		else if (scr_viewsize.value >= 110)
			sb_lines = 24;		// no inventory
		else
			sb_lines = 24 + 16 + 8;
	}
}

extern cvar_t cl_minfps;
extern cvar_t cl_minfps_fade;
extern cvar_t cl_minfps_qualitymax;
extern cvar_t cl_minfps_qualitymin;
extern cvar_t cl_minfps_qualitymultiply;
extern cvar_t cl_minfps_qualityhysteresis;
extern cvar_t cl_minfps_qualitystepmax;
extern cvar_t cl_minfps_force;
void CL_UpdateScreen(void)
{
	static double cl_updatescreen_quality = 1;

	vec3_t vieworigin;
	static double drawscreenstart = 0.0;
	double drawscreendelta;
	r_viewport_t viewport;

	// METAL.md Phase 3 slice 4: THE DOOR IS OPEN. Phase 0 turned round here
	// because no drawing backend existed and every qgl* pointer is NULL on this
	// path; the whole 2D pipeline now runs instead, into the backend's GL-layout
	// screen texture, and VID_Finish's present pass puts it on the drawable.
	//
	// The OTHER Phase 0 door -- the guard at the top of R_RenderView -- stays up
	// on purpose, and METAL.md's own slice-4 description is wrong to say it
	// should come down with this one. The CSQC renderscene() builtin
	// (clvm_cmds.c) has no guard of its own and relies entirely on that one, so
	// removing it drops any CSQC map straight into the unported 3D world path.
	// The 2D frame never reaches R_RenderView anyway. Phase 4a lifts it, with
	// map's and envmap's refusals, together.
	//
	// The heat shimmer's world field resolves HERE, during map load, when the
	// murk will not resolve it in RenderFog: WHEN the field bakes shifts the
	// whole scene ~1 px for the rest of the boot on either backend (mechanism
	// open), and the shimmer's Metal-only first-use bake inside R_BlendView is
	// what broke the parity bed from F7 to 2026-08-16. Unconditional -- it
	// gates itself and is a couple of compares after the first call. The
	// measurements that chose this site over the others are at the definition.
	R_Volumetric_ResolveField();
	if (vid.renderpath == RENDERPATH_METAL)
		Metal_Backend_BeginFrame(vid.mode.width, vid.mode.height);

	// TODO: Move to a better place.
	cl_punchangle_applied = 0;

	if(drawscreenstart)
	{
		drawscreendelta = Sys_DirtyTime() - drawscreenstart;
#ifdef CONFIG_VIDEO_CAPTURE
		if (cl_minfps.value > 0 && (cl_minfps_force.integer || !(cls.timedemo || (cls.capturevideo.active && !cls.capturevideo.realtime))) && drawscreendelta >= 0 && drawscreendelta < 60)
#else
		if (cl_minfps.value > 0 && (cl_minfps_force.integer || !cls.timedemo) && drawscreendelta >= 0 && drawscreendelta < 60)
#endif
		{
			// quality adjustment according to render time
			double actualframetime;
			double targetframetime;
			double adjust;
			double f;
			double h;

			// fade lastdrawscreentime
			r_refdef.lastdrawscreentime += (drawscreendelta - r_refdef.lastdrawscreentime) * cl_minfps_fade.value;

			// find actual and target frame times
			actualframetime = r_refdef.lastdrawscreentime;
			targetframetime = (1.0 / cl_minfps.value);

			// we scale hysteresis by quality
			h = cl_updatescreen_quality * cl_minfps_qualityhysteresis.value;

			// calculate adjustment assuming linearity
			f = cl_updatescreen_quality / actualframetime * cl_minfps_qualitymultiply.value;
			adjust = (targetframetime - actualframetime) * f;

			// one sided hysteresis
			if(adjust > 0)
				adjust = max(0, adjust - h);

			// adjust > 0 if:
			//   (targetframetime - actualframetime) * f > h
			//   ((1.0 / cl_minfps.value) - actualframetime) * (cl_updatescreen_quality / actualframetime * cl_minfps_qualitymultiply.value) > (cl_updatescreen_quality * cl_minfps_qualityhysteresis.value)
			//   ((1.0 / cl_minfps.value) - actualframetime) * (cl_minfps_qualitymultiply.value / actualframetime) > cl_minfps_qualityhysteresis.value
			//   (1.0 / cl_minfps.value) * (cl_minfps_qualitymultiply.value / actualframetime) - cl_minfps_qualitymultiply.value > cl_minfps_qualityhysteresis.value
			//   (1.0 / cl_minfps.value) * (cl_minfps_qualitymultiply.value / actualframetime) > cl_minfps_qualityhysteresis.value + cl_minfps_qualitymultiply.value
			//   (1.0 / cl_minfps.value) / actualframetime > (cl_minfps_qualityhysteresis.value + cl_minfps_qualitymultiply.value) / cl_minfps_qualitymultiply.value
			//   (1.0 / cl_minfps.value) / actualframetime > 1.0 + cl_minfps_qualityhysteresis.value / cl_minfps_qualitymultiply.value
			//   cl_minfps.value * actualframetime < 1.0 / (1.0 + cl_minfps_qualityhysteresis.value / cl_minfps_qualitymultiply.value)
			//   actualframetime < 1.0 / cl_minfps.value / (1.0 + cl_minfps_qualityhysteresis.value / cl_minfps_qualitymultiply.value)
			//   actualfps > cl_minfps.value * (1.0 + cl_minfps_qualityhysteresis.value / cl_minfps_qualitymultiply.value)

			// adjust < 0 if:
			//   (targetframetime - actualframetime) * f < 0
			//   ((1.0 / cl_minfps.value) - actualframetime) * (cl_updatescreen_quality / actualframetime * cl_minfps_qualitymultiply.value) < 0
			//   ((1.0 / cl_minfps.value) - actualframetime) < 0
			//   -actualframetime) < -(1.0 / cl_minfps.value)
			//   actualfps < cl_minfps.value

			/*
			Con_Printf("adjust UP if fps > %f, adjust DOWN if fps < %f\n",
					cl_minfps.value * (1.0 + cl_minfps_qualityhysteresis.value / cl_minfps_qualitymultiply.value),
					cl_minfps.value);
			*/

			// don't adjust too much at once
			adjust = bound(-cl_minfps_qualitystepmax.value, adjust, cl_minfps_qualitystepmax.value);

			// adjust!
			cl_updatescreen_quality += adjust;
			cl_updatescreen_quality = bound(max(0.01, cl_minfps_qualitymin.value), cl_updatescreen_quality, cl_minfps_qualitymax.value);
		}
		else
		{
			cl_updatescreen_quality = 1;
			r_refdef.lastdrawscreentime = 0;
		}
	}

	drawscreenstart = Sys_DirtyTime();

	Sbar_ShowFPS_Update();

	if (!scr_initialized || !con_initialized || !scr_refresh.integer)
		return;				// not initialized yet

	if(IS_NEXUIZ_DERIVED(gamemode))
	{
		// play a bit with the palette (experimental)
		palette_rgb_pantscolormap[15][0] = (unsigned char) (128 + 127 * sin(cl.time / exp(1.0f) + 0.0f*M_PI/3.0f));
		palette_rgb_pantscolormap[15][1] = (unsigned char) (128 + 127 * sin(cl.time / exp(1.0f) + 2.0f*M_PI/3.0f));
		palette_rgb_pantscolormap[15][2] = (unsigned char) (128 + 127 * sin(cl.time / exp(1.0f) + 4.0f*M_PI/3.0f));
		palette_rgb_shirtcolormap[15][0] = (unsigned char) (128 + 127 * sin(cl.time /  M_PI  + 5.0f*M_PI/3.0f));
		palette_rgb_shirtcolormap[15][1] = (unsigned char) (128 + 127 * sin(cl.time /  M_PI  + 3.0f*M_PI/3.0f));
		palette_rgb_shirtcolormap[15][2] = (unsigned char) (128 + 127 * sin(cl.time /  M_PI  + 1.0f*M_PI/3.0f));
		memcpy(palette_rgb_pantsscoreboard[15], palette_rgb_pantscolormap[15], sizeof(*palette_rgb_pantscolormap));
		memcpy(palette_rgb_shirtscoreboard[15], palette_rgb_shirtcolormap[15], sizeof(*palette_rgb_shirtcolormap));
	}

#ifdef CONFIG_VIDEO_CAPTURE
	if (vid_hidden && !cls.capturevideo.active
	&& !cl_capturevideo.integer) // so we can start capturing while hidden
#else
	if (vid_hidden)
#endif
	{
		VID_Finish();
		return;
	}

	if (scr_loading)
	{
		if(!loadingscreenstack && !cls.connect_trying && (cls.state != ca_connected || cls.signon == SIGNONS))
			SCR_EndLoadingPlaque();
		else if (scr_loadingscreen_maxfps.value > 0)
		{
			static double lastupdate;
			if (host.realtime - lastupdate < min(1.0f / scr_loadingscreen_maxfps.value, 0.1))
				return;
			lastupdate = host.realtime;
		}
	}

	SCR_UpdateVars();

	R_FrameData_NewFrame();
	R_BufferData_NewFrame();

	Matrix4x4_OriginFromMatrix(&r_refdef.view.matrix, vieworigin);
	R_HDR_UpdateIrisAdaptation(vieworigin);

	r_refdef.view.colormask[0] = 1;
	r_refdef.view.colormask[1] = 1;
	r_refdef.view.colormask[2] = 1;

	SCR_SetUpToDrawConsole();

#ifndef USE_GLES2
	// METAL.md Phase 3 slice 4: this sat behind the CL_UpdateScreen early-out and
	// is a call through NULL the moment that door comes down -- GL_InitFunctions
	// is only ever called by VID_InitModeGL. Metal has no draw-buffer selector at
	// all: the target is whatever render pass the backend is encoding into.
	if (vid.renderpath != RENDERPATH_METAL)
	{
		CHECKGLERROR
		qglDrawBuffer(GL_BACK);CHECKGLERROR
	}
#endif

	R_Viewport_InitOrtho(&viewport, &identitymatrix, 0, 0, vid.mode.width, vid.mode.height, 0, 0, vid_conwidth.integer, vid_conheight.integer, -10, 100, NULL);
	R_Mesh_SetRenderTargets(0);
	R_SetViewport(&viewport);
	GL_ScissorTest(false);
	GL_ColorMask(1,1,1,1);
	GL_DepthMask(true);

	R_ClearScreen(false);
	r_refdef.view.clear = false;
	r_refdef.view.isoverlay = false;

	// calculate r_refdef.view.quality
	r_refdef.view.quality = cl_updatescreen_quality;

	if(scr_stipple.integer)
	{
		Con_Print("FIXME: scr_stipple not implemented\n");
		Cvar_SetValueQuick(&scr_stipple, 0);
	}

#ifndef USE_GLES2
	if (R_Stereo_Active())
	{
		r_stereo_side = 0;

		if (r_stereo_redblue.integer || r_stereo_redgreen.integer || r_stereo_redcyan.integer)
		{
			r_refdef.view.colormask[0] = 1;
			r_refdef.view.colormask[1] = 0;
			r_refdef.view.colormask[2] = 0;
		}

		if (vid.mode.stereobuffer && vid.renderpath != RENDERPATH_METAL)
			qglDrawBuffer(GL_BACK_RIGHT);

		SCR_DrawScreen();

		r_stereo_side = 1;
		r_refdef.view.clear = true;

		if (r_stereo_redblue.integer || r_stereo_redgreen.integer || r_stereo_redcyan.integer)
		{
			r_refdef.view.colormask[0] = 0;
			r_refdef.view.colormask[1] = r_stereo_redcyan.integer || r_stereo_redgreen.integer;
			r_refdef.view.colormask[2] = r_stereo_redcyan.integer || r_stereo_redblue.integer;
		}

		if (vid.mode.stereobuffer && vid.renderpath != RENDERPATH_METAL)
			qglDrawBuffer(GL_BACK_LEFT);

		SCR_DrawScreen();
		r_stereo_side = 0;
	}
	else
#endif
	{
		r_stereo_side = 0;
		SCR_DrawScreen();
	}

#ifdef CONFIG_VIDEO_CAPTURE
	SCR_CaptureVideo();
#endif

	// Metal submits in VID_Finish's present, so there is nothing to flush here --
	// and qglFlush is a NULL call on that path (see the qglDrawBuffer note above).
	if (vid.renderpath != RENDERPATH_METAL)
		qglFlush(); // ensure that the commands are submitted to the GPU before we do other things
	VID_Finish();
}

void CL_Screen_NewMap(void)
{
}
