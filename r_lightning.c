
#include "quakedef.h"
#include "image.h"
#include "r_shadow.h"		// R_RTLight_Update, for the M5 bolt's world lighting
#include "cl_collision.h"	// CL_TraceLine, for clipping the bolt to the world

cvar_t r_lightningbeam_thickness = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_thickness", "8", "thickness of the lightning beam effect"};
cvar_t r_lightningbeam_scroll = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_scroll", "5", "speed of texture scrolling on the lightning beam effect"};
cvar_t r_lightningbeam_repeatdistance = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_repeatdistance", "128", "how far to stretch the texture along the lightning beam effect"};
cvar_t r_lightningbeam_color_red = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_color_red", "1", "color of the lightning beam effect"};
cvar_t r_lightningbeam_color_green = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_color_green", "1", "color of the lightning beam effect"};
cvar_t r_lightningbeam_color_blue = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_color_blue", "1", "color of the lightning beam effect"};
cvar_t r_lightningbeam_qmbtexture = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_qmbtexture", "0", "load the qmb textures/particles/lightning.pcx texture instead of generating one, can look better"};

// ---------------------------------------------------------------------------
// M5 enhanced thunderbolt
//
// The stock beam (CL_Beam_AddPolygons, below) is one straight quad triple: a
// tidy zigzag from the scrolling texture and nothing else. This replaces the
// PATH between the two locked endpoints with a midpoint-displaced polyline
// carrying forked branches and braided filaments, re-rolled on a fixed cadence
// so the crackle is framerate-independent.
//
// r_lightningbeam_m5 0 falls back to the stock functions untouched.
// ---------------------------------------------------------------------------
cvar_t r_lightningbeam_m5 = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5", "1", "M5 enhanced thunderbolt: jagged, branching, re-rolling lightning that lights the room. 0 = stock DarkPlaces beam"};
cvar_t r_lightningbeam_m5_rate = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_rate", "15", "how many times a second the bolt re-rolls its shape (framerate-independent)"};
cvar_t r_lightningbeam_m5_hold = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_hold", "8", "how many re-rolls the bolt holds one overall pose before jumping to another, like a plasma globe. 0 = re-roll everything every time"};
cvar_t r_lightningbeam_m5_holdlevels = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_holdlevels", "2", "how many of the coarsest subdivision levels the pose holds; the rest keep crackling"};
cvar_t r_lightningbeam_m5_seglen = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_seglen", "18", "target world units per bolt segment; shorter means more, finer kinks"};
cvar_t r_lightningbeam_m5_jitter = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_jitter", "0.05", "how far the bolt wanders off the straight line, as a fraction of its length"};
cvar_t r_lightningbeam_m5_decay = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_decay", "0.68", "per-subdivision-level falloff of the displacement; below 0.5 reads as a uniform zigzag"};
cvar_t r_lightningbeam_m5_axial = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_axial", "0.18", "how much segment LENGTHS vary; 0 spaces the kinks evenly, which reads mechanical"};
cvar_t r_lightningbeam_m5_branches = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_branches", "2", "maximum forked branches that split off and die out in mid-air"};
cvar_t r_lightningbeam_m5_branchchance = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_branchchance", "0.35", "per-node probability of spawning a fork"};
cvar_t r_lightningbeam_m5_filaments = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_filaments", "2", "thinner threads that split off the main channel and rejoin it"};
cvar_t r_lightningbeam_m5_corewidth = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_corewidth", "0.5", "width of the hot core, as a fraction of r_lightningbeam_thickness"};
cvar_t r_lightningbeam_m5_corevolume = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_corevolume", "1.2", "how much body the core has. Spread across the core's passes rather than piled onto one, so this widens the channel rather than just brightening it"};
cvar_t r_lightningbeam_m5_coretube = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_coretube", "1", "draw the core as a tube (two extra passes rolled about its own axis) instead of a single flat card"};
cvar_t r_lightningbeam_m5_coreinner = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_coreinner", "0.3", "width of the white-hot filament at the very centre, as a fraction of the core. 0 = off"};
cvar_t r_lightningbeam_m5_coretexture = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_coretexture", "1", "use the M5 core texture instead of the stock beam one, which only lights the middle half of the ribbon and swings 25x in brightness along its length"};
cvar_t r_lightningbeam_m5_sheathwidth = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_sheathwidth", "1.3", "width of the soft outer sheath, as a fraction of r_lightningbeam_thickness"};
cvar_t r_lightningbeam_m5_whiteness = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_whiteness", "0.75", "how far the core is lifted toward white from the r_lightningbeam_color_* hue"};
cvar_t r_lightningbeam_m5_sheathalpha = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_sheathalpha", "0.55", "brightness of the outer sheath"};
cvar_t r_lightningbeam_m5_branchalpha = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_branchalpha", "0.5", "brightness of forks and filaments at their root; they always taper to nothing"};
cvar_t r_lightningbeam_m5_flicker = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_flicker", "1", "depth of the brightness flicker. Hard-clamped in code so the bolt can never fall below 55% of peak, at any value - see M5_FLICKER_MIN"};
cvar_t r_lightningbeam_m5_light = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_light", "1", "how brightly the bolt lights the room. 0 = off. Needs RT Shadows (rt_metal) - see SETTINGS.md"};
cvar_t r_lightningbeam_m5_lightcount = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_lightcount", "5", "lights spaced along the bolt. Brightness is normalised for this, so changing it does not change how bright the room gets"};
cvar_t r_lightningbeam_m5_lightradius = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_lightradius", "320", "reach of each light along the bolt, world units"};
cvar_t r_lightningbeam_m5_lightwhite = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_lightwhite", "0.35", "how far the cast light is desaturated toward white from the beam hue"};
cvar_t r_lightningbeam_m5_impact = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_impact", "1.6", "brightness of the flash where the bolt lands, relative to the channel lights"};
cvar_t r_lightningbeam_m5_hitfade = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_hitfade", "0.25", "how long the connect flare takes to die away, seconds. Not zero: the hit state flips at the 10Hz weapon tick, and an instant boost would be a 10Hz strobe"};
cvar_t r_lightningbeam_m5_hitboost = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_hitboost", "2.2", "how much brighter the bolt burns while it is actually damaging something"};
cvar_t r_lightningbeam_m5_fog = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_fog", "1", "how strongly the bolt lights the volumetric fog, independently of the light it casts on walls. Needs rt_metal_fog"};
cvar_t r_lightningbeam_m5_muzzle = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_muzzle", "1", "start your own bolt at the gun's barrel rather than at your waist, so it leaves the weapon however you are looking"};
cvar_t r_lightningbeam_m5_muzzleforward = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_muzzleforward", "22", "muzzle offset along the gun, world units"};
cvar_t r_lightningbeam_m5_muzzleright = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_muzzleright", "5", "muzzle offset across the gun, world units"};
cvar_t r_lightningbeam_m5_muzzleup = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_muzzleup", "-8", "muzzle offset above the gun, world units"};
cvar_t r_lightningbeam_m5_muzzlefade = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_muzzlefade", "0.15", "how much of the bolt is pulled toward the muzzle; the far end never moves"};
cvar_t r_lightningbeam_m5_clip = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_clip", "2", "stop the bolt being drawn through walls: 0 off, 1 the main channel, 2 forks and filaments too"};
cvar_t r_lightningbeam_m5_clipback = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_clipback", "4", "how far a clipped node is held off the surface it hit, world units"};
cvar_t r_lightningbeam_m5_endtrace = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_endtrace", "1", "re-trace the bolt's landing point so it ends on a real surface. Only does anything under cl_beams_instantaimhack, whose endpoint is on no surface at all"};
cvar_t r_lightningbeam_m5_muzzlenodes = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_muzzlenodes", "1", "let the arc wander between the gun's three electrodes instead of always leaving the right-hand one. Switches at most 15 times a second whatever the re-roll rate. 0 = the fixed right-hand prong"};
cvar_t r_lightningbeam_m5_frizzle = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_frizzle", "1", "fray every free tip into finer, dimmer, wilder branches the way real lightning does, and taper the tips away instead of stopping them square. 0 = the old blunt tips"};
cvar_t r_lightningbeam_m5_frizzle_gens = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_frizzle_gens", "2", "how many generations deep the tip fray branches"};
cvar_t r_lightningbeam_m5_frizzle_sigma1 = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_frizzle_sigma1", "16", "how far the first generation of tip branches leans off its parent, degrees"};
cvar_t r_lightningbeam_m5_frizzle_sigma2 = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_frizzle_sigma2", "40", "how far the LAST generation leans off its parent, degrees; the spread ramps from sigma1 to this"};
cvar_t r_lightningbeam_m5_frizzle_dim = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_frizzle_dim", "0.5", "how much dimmer each generation of tip branches is than the one it grew from"};
cvar_t r_lightningbeam_m5_frizzle_streamers = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_frizzle_streamers", "1", "density of the tiny hairs thrown off the last stretch of each branch. 0 = none"};
cvar_t r_lightningbeam_m5_frizzle_taper = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_frizzle_taper", "0.82", "arc fraction past which a free tip starts dissolving to nothing. 1 = no taper"};
cvar_t r_lightningbeam_m5_frizzle_persist = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_frizzle_persist", "0.5", "how brightly the PREVIOUS roll's fray lingers under the current one, so the tips shimmer rather than strobe at the re-roll rate. 0 = off"};
cvar_t r_lightningbeam_m5_seed = {CF_CLIENT, "r_lightningbeam_m5_seed", "0", "test hook: pin the re-roll index so the bolt's shape is identical every frame and every boot. 0 = live. Also pins the ghost's fade, so a frozen bed sees both sets"};
cvar_t r_lightningbeam_m5_sdf_falloff = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_sdf_falloff", "15", "how sharply the distance-field bolt falls off across its own width. Higher is tighter; the old ribbon gets this shape from a texture whose body is mostly dark"};
cvar_t r_lightningbeam_m5_ballcolor_red = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_ballcolor_red", "0.45", "M5 ball lightning: the ball's base colour, red. The shell, its filaments, its arcs and its light all key on this (max-normalised), independent of the thunderbolt's r_lightningbeam_color_*. Default a violet-blue, ten percent bluer than the reference image on Seb's word (2026-09-06)"};
cvar_t r_lightningbeam_m5_ballcolor_green = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_ballcolor_green", "0.28", "M5 ball lightning: the ball's base colour, green"};
cvar_t r_lightningbeam_m5_ballcolor_blue = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_ballcolor_blue", "1", "M5 ball lightning: the ball's base colour, blue"};
cvar_t r_lightningbeam_m5_ballpressure = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_ballpressure", "1.2", "M5 ball lightning: strength of the violet cast that creeps in from the screen's EDGES as a ball comes within about one and a half of its reaches of you (Metal postprocess; Seb's tenth look, \"screen pressure\"). 0 = none"};
cvar_t r_lightningbeam_m5_ballarcs = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_ballarcs", "1", "M5 ball lightning: 1 draws the ball's arcs as PLASMA FILAMENTS -- thinner, smoother, violet, no forks or frizzle, the globe's own filaments reaching out to the target; 0 draws them as ordinary M5 lightning bolts. A/B for Seb's eye (2026-09-06)"};
cvar_t r_lightningbeam_m5_ballsize = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_ballsize", "22", "M5 ball lightning: radius in world units of the plasma knot drawn at each ball -- a re-rolling tangle of short M5 bolt strands inside a faint halo, sharing the bolt's core and sheath colours, its 15 Hz crackle and its fizz. Needs m5_balllightning (the game half) and r_lightningbeam_m5"};
cvar_t r_lightningbeam_m5_fizz = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_fizz", "0.666", "boil the distance-field bolt's own radius, so its edge writhes and its width breathes along its length -- the shape analogue of the lava shimmer, and noise rather than sines for the same reason. This is the FRACTION of the radius the boil swings, so 0.3 is +/-30%. 0 = the old capsule exactly. Needs the SDF bolt, which means a renderer that can accumulate under max(): GL cannot, and its arm is a magenta sentinel"};
cvar_t r_lightningbeam_m5_fizz_scale = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_fizz_scale", "0.35", "spatial frequency of the bolt fizz, in cycles per WORLD unit -- so the boil has a fixed physical wavelength and does not stretch with segment length. the shipped 0.35 puts a little under three world units in a cycle"};
cvar_t r_lightningbeam_m5_fizz_speed = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_fizz_speed", "0.5", "how fast the bolt fizz travels ALONG the beam, in cycles per second. Negative runs it the other way"};
cvar_t r_lightningbeam_m5_sdf_gain = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_sdf_gain", "1", "brightness of the distance-field bolt. The old ribbon draws the channel five times over and ADDS them; max() keeps only the brightest, which is the whole point, so the energy comes back here instead. Calibrated against the ribbon, not guessed"};
cvar_t r_lightningbeam_m5_sdf = {CF_CLIENT | CF_ARCHIVE, "r_lightningbeam_m5_sdf", "1", "draw the bolt as distance-field capsules accumulated under max(), which removes the over-bright cross where segments meet. Needs a renderer that can blend with max(); 0, and every renderer that cannot, uses the old additive ribbon"};

/// Per-slot connect decay. beam_t has no spare field and cannot grow without
/// touching the stock struct, so this is pure render state living here. Keyed by
/// entity as well as slot, because CL_NewBeam recycles a slot by owner and a
/// recycled slot must not inherit the previous owner's flare.
static double m5_hittime[MAX_BEAMS];
static int m5_hitent[MAX_BEAMS];

/// The shape report is emitted before the tips are frayed (it reports on the
/// channel, which is finished by then), so the frizzle half of it has to wait
/// until the end of the build. Set by the report, consumed once.
static qbool m5_reportpending = false;

static texture_t cl_beams_externaltexture;
static texture_t cl_beams_builtintexture;
static texture_t cl_beams_m5coretexture;

static void r_lightningbeams_start(void)
{
	memset(m5_hittime, 0, sizeof(m5_hittime));
	memset(m5_hitent, 0, sizeof(m5_hitent));
	memset(&cl_beams_externaltexture, 0, sizeof(cl_beams_externaltexture));
	memset(&cl_beams_builtintexture, 0, sizeof(cl_beams_builtintexture));
	memset(&cl_beams_m5coretexture, 0, sizeof(cl_beams_m5coretexture));
}

static void CL_Beams_SetupExternalTexture(void)
{
	if (Mod_LoadTextureFromQ3Shader(r_main_mempool, "r_lightning.c", &cl_beams_externaltexture, "textures/particles/lightning", false, false, TEXF_ALPHA | TEXF_FORCELINEAR, MATERIALFLAG_WALL | MATERIALFLAG_NOCULLFACE | MATERIALFLAG_VERTEXCOLOR | MATERIALFLAG_ALPHAGEN_VERTEX | MATERIALFLAG_ADD | MATERIALFLAG_BLENDED | MATERIALFLAG_NOSHADOW))
		Cvar_SetValueQuick(&r_lightningbeam_qmbtexture, false);
}

static void CL_Beams_SetupBuiltinTexture(void)
{
	// beam direction is horizontal in the lightning texture
	int texwidth = 128;
	int texheight = 64;
	float r, g, b, intensity, thickness = texheight * 0.25f, border = thickness + 2.0f, ithickness = 1.0f / thickness, center, n;
	int x, y;
	unsigned char *data;
	skinframe_t *skinframe;
	float centersamples[17][2];

	// make a repeating noise pattern for the beam path
	for (x = 0; x < 16; x++)
	{
		centersamples[x][0] = lhrandom(border, texheight - border);
		centersamples[x][1] = lhrandom(0.2f, 1.00f);
	}
	centersamples[16][0] = centersamples[0][0];
	centersamples[16][1] = centersamples[0][1];

	data = (unsigned char *)Mem_Alloc(tempmempool, texwidth * texheight * 4);

	// iterate by columns and draw the entire column of pixels
	for (x = 0; x < texwidth; x++)
	{
		r = x * 16.0f / texwidth;
		y = (int)r;
		g = r - y;
		center = centersamples[y][0] * (1.0f - g) + centersamples[y+1][0] * g;
		n = centersamples[y][1] * (1.0f - g) + centersamples[y + 1][1] * g;
		for (y = 0; y < texheight; y++)
		{
			intensity = 1.0f - fabs((y - center) * ithickness);
			if (intensity > 0)
			{
				intensity = pow(intensity * n, 2);
				r = intensity * 1.000f * 255.0f;
				g = intensity * 2.000f * 255.0f;
				b = intensity * 4.000f * 255.0f;
				data[(y * texwidth + x) * 4 + 2] = (unsigned char)(bound(0, r, 255));
				data[(y * texwidth + x) * 4 + 1] = (unsigned char)(bound(0, g, 255));
				data[(y * texwidth + x) * 4 + 0] = (unsigned char)(bound(0, b, 255));
			}
			else
				intensity = 0.0f;
			data[(y * texwidth + x) * 4 + 3] = (unsigned char)255;
		}
	}

	skinframe = R_SkinFrame_LoadInternalBGRA("lightningbeam", TEXF_FORCELINEAR, data, texwidth, texheight, 0, 0, 0, false);
	Mod_LoadCustomMaterial(r_main_mempool, &cl_beams_builtintexture, "cl_beams_builtintexture", 0, MATERIALFLAG_WALL | MATERIALFLAG_NOCULLFACE | MATERIALFLAG_VERTEXCOLOR | MATERIALFLAG_ALPHAGEN_VERTEX | MATERIALFLAG_ADD | MATERIALFLAG_BLENDED | MATERIALFLAG_NOSHADOW, skinframe);
	Mem_Free(data);
}

static void r_lightningbeams_shutdown(void)
{
	memset(&cl_beams_externaltexture, 0, sizeof(cl_beams_externaltexture));
	memset(&cl_beams_builtintexture, 0, sizeof(cl_beams_builtintexture));
	memset(&cl_beams_m5coretexture, 0, sizeof(cl_beams_m5coretexture));
}

static void r_lightningbeams_newmap(void)
{
	memset(m5_hittime, 0, sizeof(m5_hittime));
	memset(m5_hitent, 0, sizeof(m5_hitent));
	if (cl_beams_externaltexture.currentskinframe)
		R_SkinFrame_MarkUsed(cl_beams_externaltexture.currentskinframe);
	if (cl_beams_builtintexture.currentskinframe)
		R_SkinFrame_MarkUsed(cl_beams_builtintexture.currentskinframe);
	if (cl_beams_m5coretexture.currentskinframe)
		R_SkinFrame_MarkUsed(cl_beams_m5coretexture.currentskinframe);
}

static void CL_Beam_M5_Test_f(cmd_state_t *cmd);

void R_LightningBeams_Init(void)
{
	Cvar_RegisterVariable(&r_lightningbeam_thickness);
	Cvar_RegisterVariable(&r_lightningbeam_scroll);
	Cvar_RegisterVariable(&r_lightningbeam_repeatdistance);
	Cvar_RegisterVariable(&r_lightningbeam_color_red);
	Cvar_RegisterVariable(&r_lightningbeam_color_green);
	Cvar_RegisterVariable(&r_lightningbeam_color_blue);
	Cvar_RegisterVariable(&r_lightningbeam_qmbtexture);
	Cvar_RegisterVariable(&r_lightningbeam_m5);
	Cvar_RegisterVariable(&r_lightningbeam_m5_rate);
	Cvar_RegisterVariable(&r_lightningbeam_m5_hold);
	Cvar_RegisterVariable(&r_lightningbeam_m5_holdlevels);
	Cvar_RegisterVariable(&r_lightningbeam_m5_seglen);
	Cvar_RegisterVariable(&r_lightningbeam_m5_jitter);
	Cvar_RegisterVariable(&r_lightningbeam_m5_decay);
	Cvar_RegisterVariable(&r_lightningbeam_m5_axial);
	Cvar_RegisterVariable(&r_lightningbeam_m5_branches);
	Cvar_RegisterVariable(&r_lightningbeam_m5_branchchance);
	Cvar_RegisterVariable(&r_lightningbeam_m5_filaments);
	Cvar_RegisterVariable(&r_lightningbeam_m5_corewidth);
	Cvar_RegisterVariable(&r_lightningbeam_m5_corevolume);
	Cvar_RegisterVariable(&r_lightningbeam_m5_coretube);
	Cvar_RegisterVariable(&r_lightningbeam_m5_coreinner);
	Cvar_RegisterVariable(&r_lightningbeam_m5_coretexture);
	Cvar_RegisterVariable(&r_lightningbeam_m5_sheathwidth);
	Cvar_RegisterVariable(&r_lightningbeam_m5_whiteness);
	Cvar_RegisterVariable(&r_lightningbeam_m5_sheathalpha);
	Cvar_RegisterVariable(&r_lightningbeam_m5_branchalpha);
	Cvar_RegisterVariable(&r_lightningbeam_m5_flicker);
	Cvar_RegisterVariable(&r_lightningbeam_m5_light);
	Cvar_RegisterVariable(&r_lightningbeam_m5_lightcount);
	Cvar_RegisterVariable(&r_lightningbeam_m5_lightradius);
	Cvar_RegisterVariable(&r_lightningbeam_m5_lightwhite);
	Cvar_RegisterVariable(&r_lightningbeam_m5_impact);
	Cvar_RegisterVariable(&r_lightningbeam_m5_hitboost);
	Cvar_RegisterVariable(&r_lightningbeam_m5_hitfade);
	Cvar_RegisterVariable(&r_lightningbeam_m5_fog);
	Cvar_RegisterVariable(&r_lightningbeam_m5_muzzle);
	Cvar_RegisterVariable(&r_lightningbeam_m5_muzzleforward);
	Cvar_RegisterVariable(&r_lightningbeam_m5_muzzleright);
	Cvar_RegisterVariable(&r_lightningbeam_m5_muzzleup);
	Cvar_RegisterVariable(&r_lightningbeam_m5_muzzlefade);
	Cvar_RegisterVariable(&r_lightningbeam_m5_clip);
	Cvar_RegisterVariable(&r_lightningbeam_m5_clipback);
	Cvar_RegisterVariable(&r_lightningbeam_m5_endtrace);
	Cvar_RegisterVariable(&r_lightningbeam_m5_muzzlenodes);
	Cvar_RegisterVariable(&r_lightningbeam_m5_frizzle);
	Cvar_RegisterVariable(&r_lightningbeam_m5_frizzle_gens);
	Cvar_RegisterVariable(&r_lightningbeam_m5_frizzle_sigma1);
	Cvar_RegisterVariable(&r_lightningbeam_m5_frizzle_sigma2);
	Cvar_RegisterVariable(&r_lightningbeam_m5_frizzle_dim);
	Cvar_RegisterVariable(&r_lightningbeam_m5_frizzle_streamers);
	Cvar_RegisterVariable(&r_lightningbeam_m5_frizzle_taper);
	Cvar_RegisterVariable(&r_lightningbeam_m5_frizzle_persist);
	Cvar_RegisterVariable(&r_lightningbeam_m5_seed);
	Cvar_RegisterVariable(&r_lightningbeam_m5_sdf);
	Cvar_RegisterVariable(&r_lightningbeam_m5_sdf_gain);
	Cvar_RegisterVariable(&r_lightningbeam_m5_sdf_falloff);
	Cvar_RegisterVariable(&r_lightningbeam_m5_fizz);
	Cvar_RegisterVariable(&r_lightningbeam_m5_ballsize);
	Cvar_RegisterVariable(&r_lightningbeam_m5_ballarcs);
	Cvar_RegisterVariable(&r_lightningbeam_m5_ballpressure);
	Cvar_RegisterVariable(&r_lightningbeam_m5_ballcolor_red);
	Cvar_RegisterVariable(&r_lightningbeam_m5_ballcolor_green);
	Cvar_RegisterVariable(&r_lightningbeam_m5_ballcolor_blue);
	Cvar_RegisterVariable(&r_lightningbeam_m5_fizz_scale);
	Cvar_RegisterVariable(&r_lightningbeam_m5_fizz_speed);
	Cmd_AddCommand(CF_CLIENT, "r_lightningbeam_m5_test", CL_Beam_M5_Test_f, "verify the bolt's flicker floor: sweeps the envelope at 1 kHz with the depth forced past its cvar bound and reports the extremes");
	R_RegisterModule("R_LightningBeams", r_lightningbeams_start, r_lightningbeams_shutdown, r_lightningbeams_newmap, NULL, NULL);
}

static void CL_Beam_AddQuad(model_t *mod, msurface_t *surf, const vec3_t start, const vec3_t end, const vec3_t offset, float t1, float t2)
{
	int e0, e1, e2, e3;
	vec3_t n;
	vec3_t dir;
	float c[4];

	Vector4Set(c, r_lightningbeam_color_red.value, r_lightningbeam_color_green.value, r_lightningbeam_color_blue.value, 1.0f);

	VectorSubtract(end, start, dir);
	CrossProduct(dir, offset, n);
	VectorNormalize(n);

	e0 = Mod_Mesh_IndexForVertex(mod, surf, start[0] + offset[0], start[1] + offset[1], start[2] + offset[2], n[0], n[1], n[2], t1, 0, 0, 0, c[0], c[1], c[2], c[3]);
	e1 = Mod_Mesh_IndexForVertex(mod, surf, start[0] - offset[0], start[1] - offset[1], start[2] - offset[2], n[0], n[1], n[2], t1, 1, 0, 0, c[0], c[1], c[2], c[3]);
	e2 = Mod_Mesh_IndexForVertex(mod, surf, end[0] - offset[0], end[1] - offset[1], end[2] - offset[2], n[0], n[1], n[2], t2, 1, 0, 0, c[0], c[1], c[2], c[3]);
	e3 = Mod_Mesh_IndexForVertex(mod, surf, end[0] + offset[0], end[1] + offset[1], end[2] + offset[2], n[0], n[1], n[2], t2, 0, 0, 0, c[0], c[1], c[2], c[3]);
	Mod_Mesh_AddTriangle(mod, surf, e0, e1, e2);
	Mod_Mesh_AddTriangle(mod, surf, e0, e2, e3);
}

void CL_Beam_AddPolygons(const beam_t *b)
{
	vec3_t beamdir, right, up, offset, start, end;
	vec_t beamscroll = r_refdef.scene.time * -r_lightningbeam_scroll.value;
	vec_t beamrepeatscale = 1.0f / r_lightningbeam_repeatdistance.value;
	float length, t1, t2;
	model_t *mod;
	msurface_t *surf;

	if (r_lightningbeam_qmbtexture.integer && cl_beams_externaltexture.currentskinframe == NULL)
		CL_Beams_SetupExternalTexture();
	if (!r_lightningbeam_qmbtexture.integer && cl_beams_builtintexture.currentskinframe == NULL)
		CL_Beams_SetupBuiltinTexture();

	// calculate beam direction (beamdir) vector and beam length
	// get difference vector
	CL_Beam_CalculatePositions(b, start, end);
	VectorSubtract(end, start, beamdir);
	// find length of difference vector
	length = sqrt(DotProduct(beamdir, beamdir));
	// calculate scale to make beamdir a unit vector (normalized)
	t1 = 1.0f / length;
	// scale beamdir so it is now normalized
	VectorScale(beamdir, t1, beamdir);

	// calculate up vector such that it points toward viewer, and rotates around the beamdir
	// get direction from start of beam to viewer
	VectorSubtract(r_refdef.view.origin, start, up);
	// remove the portion of the vector that moves along the beam
	// (this leaves only a vector pointing directly away from the beam)
	t1 = -DotProduct(up, beamdir);
	VectorMA(up, t1, beamdir, up);
	// generate right vector from forward and up, the result is unnormalized
	CrossProduct(beamdir, up, right);
	// now normalize the right vector and up vector
	VectorNormalize(right);
	VectorNormalize(up);

	// calculate T coordinate scrolling (start and end texcoord along the beam)
	t1 = beamscroll;
	t1 = t1 - (int)t1;
	t2 = t1 + beamrepeatscale * length;

	// the beam is 3 polygons in this configuration:
	//  *   2
	//   * *
	// 1*****
	//   * *
	//  *   3
	// they are showing different portions of the beam texture, creating an
	// illusion of a beam that appears to curl around in 3D space
	// (and realize that the whole polygon assembly orients itself to face
	//  the viewer)

	mod = CL_Mesh_Scene();
	surf = Mod_Mesh_AddSurface(mod, r_lightningbeam_qmbtexture.integer ? &cl_beams_externaltexture : &cl_beams_builtintexture, false);
	// polygon 1
	VectorM(r_lightningbeam_thickness.value, right, offset);
	CL_Beam_AddQuad(mod, surf, start, end, offset, t1, t2);
	// polygon 2
	VectorMAM(r_lightningbeam_thickness.value * 0.70710681f, right, r_lightningbeam_thickness.value * 0.70710681f, up, offset);
	CL_Beam_AddQuad(mod, surf, start, end, offset, t1 + 0.33f, t2 + 0.33f);
	// polygon 3
	VectorMAM(r_lightningbeam_thickness.value * 0.70710681f, right, r_lightningbeam_thickness.value * -0.70710681f, up, offset);
	CL_Beam_AddQuad(mod, surf, start, end, offset, t1 + 0.66f, t2 + 0.66f);
}

// ===========================================================================
// M5 enhanced thunderbolt
//
// Everything below is reached only when r_lightningbeam_m5 is set. Nothing
// above this line is touched by it, which is the whole no-op proof for the
// master gate.
// ===========================================================================

/// absolute cap on the level-0 displacement, world units. Keeps a bolt inside a
/// 128-unit Quake corridor however long the beam or however high the jitter.
#define M5BOLT_MAXDEV		64.0f
/// tan(36 degrees) / 2. Cap on a node's displacement as a fraction of the chord
/// it straddles, which holds the direction change at that node under 72 degrees.
/// Sharper than that and the ribbon miter below folds back through itself.
#define M5BOLT_KINKFRAC		0.363f
/// nodes closer together than this are merged away at build time: a zero-length
/// span has no direction, and the billboard basis dies on it.
#define M5BOLT_MINSEG		0.5f
/// Ribbon half-width ceiling as a fraction of the distance from the eye, which
/// bounds how wide any one node can get in SCREEN space (~0.04 is about 40 px
/// of half-width at 1080p).
///
/// This is load-bearing, not a nicety, and it matters most when you fire ALONG
/// your own view axis - the normal case for the lightning gun. The bolt then
/// recedes from the eye, so its near nodes project far off-centre while the
/// impact sits under the crosshair. At a constant world width those near nodes
/// become huge screen-space slabs joined to distant hairlines, and the bolt
/// reads as a shard of glass rather than a discharge. Clamping in world units
/// cannot fix it; the clamp has to scale with distance. Far geometry is
/// unaffected - at 500 units the sheath is already well under this ceiling, so
/// the authored widths still mean what they say wherever you can see them.
#define M5BOLT_NEARWIDTH	0.12f
/// Ribbon nodes closer to the eye than this are not emitted at all, and the
/// ribbon restarts after them.
///
/// The bolt's own start point is the player's waist, which sits within a few
/// units of the camera, so a ribbon vertex offset perpendicular from it can land
/// BEHIND the near plane - where projection wraps and the triangle smears right
/// across the frame as a straight streak. Narrowing the ribbon cannot fix that;
/// the vertex has to not exist. Losing the first few units of the bolt is free:
/// the view-model covers exactly that stretch.
#define M5BOLT_NEARCLIP		16.0f
/// A node closer to the straight line than this is not worth a trace - it cannot
/// have crossed into anything the line did not already cross.
#define M5BOLT_CLIPMIN		8.0f
/// How far a pose boundary may wander from the regular grid, as a fraction of
/// the hold. Must stay below 1 so the boundary sequence remains monotonic.
#define M5BOLT_HOLDJITTER	0.45f
/// Free tips that frizzle can grow from: the main channel plus every fork.
/// Filaments are not here - both of their ends are shared channel nodes.
/// How often the arc may step between the gun's electrodes, Hz. A compile-time
/// constant on purpose: "no faster than about 15" is then true of every config.
#define M5BOLT_MUZZLERATE	15.0f
#define M5BOLT_MAXTIPS		5
/// Nodes in one frizzle branch. (1 << 2) + 1, so CL_Beam_M5_Displace gets its
/// power-of-two span count and two levels of subdivision to work with.
#define M5BOLT_FRIZZNODES	5
/// Branches each frizzle generation throws. Two is what makes a fray read as a
/// fray; three fills the tip in and starts to look like a brush.
#define M5BOLT_FRIZZFANOUT	2
/// Length of a first-generation frizzle branch, as a fraction of the parent
/// branch's own length, and the shortening applied at each generation after.
#define M5BOLT_FRIZZLEN		0.14f
#define M5BOLT_FRIZZLENDECAY	0.62f
/// Micro-streamer length as a fraction of the parent branch, and how many of the
/// parent's terminal nodes may throw one.
#define M5BOLT_STREAMERLEN	0.05f
#define M5BOLT_STREAMERNODES	3
#define M5BOLT_MAXSTREAMERS	3
/// HARD FLOOR on the flicker envelope, as a fraction of peak. Deliberately a
/// compile-time constant and not a cvar: combined with the rate being fixed too
/// (M5_FLICKER_F1/F2), this makes "the bolt never strobes" a property of the
/// build rather than of the config. No cvar value can defeat it.
#define M5_FLICKER_MIN		0.55f
/// Envelope frequencies, Hz. Deliberately incommensurate, so the sum never
/// repeats into a single strong periodicity, and deliberately NOT the shape
/// re-roll rate - see the comment in CL_Beam_M5_Envelope.
#define M5_FLICKER_F1		2.7f
#define M5_FLICKER_F2		6.3f
#define M5_FLICKER_W1		0.62f
#define M5_FLICKER_W2		0.38f	// W1 + W2 == 1, so the mix lands exactly in [0,1]

/// Peak colour of ONE channel light at r_lightningbeam_m5_light 1, before the
/// overlap normalisation. Calibrated, not guessed: in wall-lighting mode the
/// surface kernel evaluates ambient + walllight*6*(Lsum - shadow), so at Seb's
/// walllight 0.5 the multiplier on Lsum is 3.0, and with no HDR shoulder
/// anything past Lrt ~ 1.0 clips a mid-grey wall to flat white. A well-lit
/// Quake corridor sits at Lsum 0.3-0.6; the player muzzle flash is 4.0 per
/// channel and DOES blow out, which is fine for one frame and not for a beam
/// that is held down. 0.5 with the default count and radius lands Lsum near 0.8
/// at the channel - about 1.5x a lit wall, which reads as "the lightning is
/// lighting the room" rather than "the room is white".
#define M5_LIGHT_BASE		0.9f
/// The impact flash is pulled this far back along the last segment. The beam's
/// end sits ON the surface it hit, and under cl_beams_instantaimhack it is
/// re-derived along the view axis and never re-traced - so it is regularly
/// inside solid. Unlike CL_AllocLightFlash, the templight path never calls
/// CL_FindNonSolidLocation, so without this the flash lights nothing at all and
/// its shadow ray is blocked from the first step.
#define M5_IMPACT_PULLBACK	12.0f

/// LOCKSTEP with CL_Beams_SetupBuiltinTexture above, which multiplies the beam
/// texture's intensity by 1 / 2 / 4 for R / G / B. That bake is why
/// r_lightningbeam_color "1 1 1" renders BLUE-white rather than white. Anything
/// deriving a colour from those cvars - the bolt vertices here, and the world
/// light and fog in CL_Beam_M5_AddLights - must divide it back out, or it will
/// not match what the player can see. Retune the generator and this must follow.
static const float m5_texweight_builtin[3] = { 1.0f, 2.0f, 4.0f };
static const float m5_texweight_qmb[3] = { 1.0f, 1.0f, 1.0f };

/// throttles the developer-2 shape report to once a second
static double m5_lastreport;
/// traces the world clip actually performed for the beam being reported - the
/// honest cost figure, since frame timings on this machine cannot resolve it
static int m5_cliptraces;

/// Per-beam constant hash in [0,1). Decorrelates two simultaneous bolts so they
/// neither re-roll on the same frame nor breathe in step.
/// Avalanche hash of (entity, slot, tick) plus a salt. Every bit of the output
/// depends on every bit of the input, which is the whole point — see
/// CL_Beam_M5_Seed for the defect that made this necessary.
static unsigned int CL_Beam_M5_Hash(unsigned int a, unsigned int b, unsigned int c, unsigned int salt)
{
	unsigned int h = a * 2654435761u + b * 40503u + c * 2246822519u + salt;
	h ^= h >> 13;
	h *= 1274126177u;
	h ^= h >> 16;
	return h;
}

/// Per-beam constant hash in [0,1). Decorrelates two simultaneous bolts so they
/// neither re-roll on the same frame nor breathe in step. Passing 0 for the tick
/// makes this bit-identical to the form it had before CL_Beam_M5_Hash was
/// factored out, so the flicker phases and the roll offset do not move.
static float CL_Beam_M5_Phase(int entity, int slot, unsigned int salt)
{
	return (float)(CL_Beam_M5_Hash((unsigned int)entity, (unsigned int)slot, 0u, salt) & 0xFFFFu)
	     * (1.0f / 65536.0f);
}

/// The M5 core texture. The stock beam texture (CL_Beams_SetupBuiltinTexture,
/// above) is why the bolt reads thin and weak, and no width or alpha knob can
/// reach it — measured from the generator itself:
///
///  - its lit band is `texheight * 0.25` either side of centre, so only the
///    MIDDLE HALF of the ribbon is ever lit. An authored half-width of 5 units
///    shows as about 2.5.
///  - its per-column intensity is `lhrandom(0.2, 1.0)` SQUARED, so peak
///    brightness swings 0.04 to 1.0 along the beam — a 25x range. Long stretches
///    of the channel are effectively black. That is the "weak" reading.
///  - its centre wanders over most of the height, which on the stock straight
///    quad IS the zigzag, but on a geometrically jagged ribbon fights the
///    geometry and adds apparent thinness.
///
/// This one is the same size and material, but lights 84% of the ribbon, floors
/// the per-column intensity at 0.65 before a gentler 1.5 power (so the dim
/// stretches lift ~13x while the bright peaks are untouched, and nothing new can
/// blow out), and pins the centre so a core is a core.
///
/// LOCKSTEP: it keeps the same 1:2:4 R:G:B bake as the stock generator, because
/// that is what gives the blue-white edge falloff and m5_texweight_builtin
/// divides it back out. Change the weights here and that table must follow.
static void CL_Beams_SetupM5CoreTexture(void)
{
	int texwidth = 128, texheight = 64;
	float thickness = texheight * 0.42f, ithickness = 1.0f / thickness;
	float centre = texheight * 0.5f, r, g, bl, intensity, frac, n, c;
	int x, y, i;
	unsigned char *data;
	skinframe_t *skinframe;
	float samples[17][2];

	for (i = 0; i < 16; i++)
	{
		samples[i][0] = lhrandom(centre - 3.0f, centre + 3.0f);		// centre barely wanders
		samples[i][1] = lhrandom(0.65f, 1.00f);						// and never goes dark
	}
	samples[16][0] = samples[0][0];
	samples[16][1] = samples[0][1];

	data = (unsigned char *)Mem_Alloc(tempmempool, texwidth * texheight * 4);
	for (x = 0; x < texwidth; x++)
	{
		r = x * 16.0f / texwidth;
		i = (int)r;
		frac = r - i;
		c = samples[i][0] * (1.0f - frac) + samples[i + 1][0] * frac;
		n = samples[i][1] * (1.0f - frac) + samples[i + 1][1] * frac;
		for (y = 0; y < texheight; y++)
		{
			intensity = 1.0f - fabs((y - c) * ithickness);
			if (intensity > 0)
			{
				intensity = pow(intensity * n, 1.5);
				r  = intensity * 1.000f * 255.0f;
				g  = intensity * 2.000f * 255.0f;
				bl = intensity * 4.000f * 255.0f;
				data[(y * texwidth + x) * 4 + 2] = (unsigned char)(bound(0, r, 255));
				data[(y * texwidth + x) * 4 + 1] = (unsigned char)(bound(0, g, 255));
				data[(y * texwidth + x) * 4 + 0] = (unsigned char)(bound(0, bl, 255));
			}
			data[(y * texwidth + x) * 4 + 3] = (unsigned char)255;
		}
	}
	skinframe = R_SkinFrame_LoadInternalBGRA("m5lightningcore", TEXF_FORCELINEAR, data, texwidth, texheight, 0, 0, 0, false);
	Mod_LoadCustomMaterial(r_main_mempool, &cl_beams_m5coretexture, "cl_beams_m5coretexture", 0,
		MATERIALFLAG_WALL | MATERIALFLAG_NOCULLFACE | MATERIALFLAG_VERTEXCOLOR
		| MATERIALFLAG_ALPHAGEN_VERTEX | MATERIALFLAG_ADD | MATERIALFLAG_BLENDED | MATERIALFLAG_NOSHADOW,
		skinframe);
	Mem_Free(data);
}

/// Which "pose" the bolt is holding. Seb's ask: it should stick in one overall
/// form for about half a second and then jump somewhere else, like the arcs in a
/// plasma globe, while still crackling in detail.
///
/// Stateless by necessity - beam_t has no spare field and beams are recycled by
/// owning entity, so there is nowhere to keep a countdown. The pose is therefore
/// a pure function of the re-roll index. Boundaries are pushed off the regular
/// grid by a hash of the pose number so the hold is irregular rather than
/// metronomic; the jitter is bounded below half the hold, which keeps the
/// boundary sequence monotonic, so only floor(roll/hold) and its two neighbours
/// can ever be the answer and the O(1) check below is exact.
///
/// Deriving it from the integer roll rather than from cl.time directly matters:
/// a pose boundary is then always also a shape re-roll, so the jump lands on a
/// crackle instead of arriving as a second, separate event.
static int CL_Beam_M5_PoseIndex(int entity, int slot, int roll, int hold)
{
	int jit = (int)(hold * M5BOLT_HOLDJITTER * 0.5f);
	int k = roll / hold;
	if (jit > 0)
	{
		unsigned int e = (unsigned int)entity, sl = (unsigned int)slot;
		int b0 = k * hold + (int)(CL_Beam_M5_Hash(e, sl, (unsigned int)k, 0x7feb352du) % (unsigned int)(2 * jit + 1)) - jit;
		int b1 = (k + 1) * hold + (int)(CL_Beam_M5_Hash(e, sl, (unsigned int)(k + 1), 0x7feb352du) % (unsigned int)(2 * jit + 1)) - jit;
		if (roll < b0)
			k--;
		else if (roll >= b1)
			k++;
	}
	return k;
}

/// Seed the shape RNG for one beam at one re-roll.
///
/// This exists because seeding it the obvious way DOES NOT WORK, and the failure
/// is invisible. Math_rand64 (mathlib.c) returns `(o[3] << 32) + o[2]`, where
/// o[3] is the 128-bit product's LEAST-significant word — and mul128 makes that
/// word `low32(s[3] * mul[3])`, a function of s[3] and nothing else. So s[3]
/// runs as its own self-contained 32-bit LCG and the top 32 bits of every draw,
/// which are the bits Math_crandomf actually reads, depend on s[3] ALONE. The
/// other three seed words contribute below float32 resolution.
///
/// Seeding as (entity, slot, roll, CONSTANT) therefore produced ONE FROZEN CURVE
/// for every bolt, every re-roll and every entity in the game — verified against
/// the real arithmetic, the draw sequence was identical in all cases. It read in
/// game as "the bolt always leans the same way and never varies", which is
/// exactly what it was.
///
/// Hashing into all four words fixes it; the low word is the one that must vary.
static void CL_Beam_M5_Seed(int entity, int slot, int tick, randomseed_t *out)
{
	unsigned int e = (unsigned int)entity, s = (unsigned int)slot, t = (unsigned int)tick;
	Math_RandomSeed_FromInts(out,
		CL_Beam_M5_Hash(e, s, t, 0x9e3779b9u),
		CL_Beam_M5_Hash(e, s, t, 0x85ebca6bu),
		CL_Beam_M5_Hash(e, s, t, 0xc2b2ae35u),
		CL_Beam_M5_Hash(e, s, t, 0x27d4eb2fu));	// FromInts ORs this one odd itself
}

/// The brightness envelope. Two incommensurate sinusoids, mixed to exactly
/// [0,1], then compressed into [M5_FLICKER_MIN, 1].
///
/// Two properties hold by construction rather than by tuning, and both matter:
/// the result can never leave [0.55, 1] for ANY cvar value (depth is bounded and
/// the floor is a constant), so there is no dark phase to strobe against; and
/// the signal is band-limited to 6.3 Hz and sinusoidal, so it has a bounded slew
/// rate rather than square-wave edges.
///
/// It is deliberately NOT locked to the shape re-roll. The re-roll runs at ~15 Hz
/// and 15-20 Hz is the most provocative band there is; putting a luminance
/// modulation on that same clock would place a coherent 15 Hz component into
/// frame brightness. Decorrelated, the crackle is carried by the SHAPE - a
/// spatial change at near-constant emitted energy - while brightness moves slowly
/// and shallowly underneath.
static float CL_Beam_M5_Envelope(double time, float ph1, float ph2)
{
	float depth = bound(0.0f, r_lightningbeam_m5_flicker.value, 1.0f);
	float n = 0.5f + 0.5f * (M5_FLICKER_W1 * sinf((float)(time * (2.0 * M_PI * M5_FLICKER_F1)) + ph1)
	                       + M5_FLICKER_W2 * sinf((float)(time * (2.0 * M_PI * M5_FLICKER_F2)) + ph2));
	return 1.0f - depth * (1.0f - M5_FLICKER_MIN) * (1.0f - n);
}

/// A stable perpendicular basis for the beam. Derived from the beam AXIS and a
/// seed-drawn reference vector, never from the view - so the shape is rigid in
/// beam space and does not swim when the player turns. Rolling the reference per
/// re-roll also spins the bolt about its own axis for free.
static void CL_Beam_M5_Basis(const vec3_t axis, randomseed_t *seed, vec3_t u, vec3_t v)
{
	vec3_t ref;
	int tries;
	for (tries = 0; tries < 4; tries++)
	{
		// unit BALL, not a cube: cross-producting a cube-sampled reference makes
		// the bend plane cluster on the four diagonals (measured 1.45x over 20k
		// rolls), so the bolt would still have preferred directions - just four
		// of them instead of one
		VectorLehmerRandom(seed, ref);
		CrossProduct(axis, ref, u);
		if (VectorLength2(u) > 1e-4f)
			break;
	}
	if (VectorLength2(u) <= 1e-4f)
	{
		// astronomically unlikely, but a parallel reference would give a zero
		// basis and a flat bolt; fall back to a world axis that cannot be parallel
		VectorSet(ref, 0, 0, 1);
		if (fabs(axis[2]) > 0.9f)
			VectorSet(ref, 1, 0, 0);
		CrossProduct(axis, ref, u);
	}
	VectorNormalize(u);
	CrossProduct(axis, u, v);
	VectorNormalize(v);
}

/// Recursive midpoint displacement over pt[0 .. count-1], whose two ends are
/// already the locked endpoints. count-1 must be a power of two.
///
/// startfade ramps the displacement in over the first fraction of the line, so
/// the path leaves its origin travelling straight. For the main channel that is
/// load-bearing as well as truthful: the bolt originates at the player's own
/// muzzle, so a node a thirtieth of the way along sits ~30 units from the eye,
/// where an otherwise perfectly reasonable 20-unit sideways displacement
/// subtends 35 degrees and throws a streak clear across the frame. Displacement
/// amplitude is uniform along the line but the ANGLE it subtends is not.
/// slowlevels: how many of the COARSEST levels draw from `slow` instead of
/// `seed`. Level 0 alone is not enough to hold a pose - level 1 still carries
/// about two thirds of the gross form, and re-rolling it every tick washes the
/// held shape straight out.
static void CL_Beam_M5_Displace(vec3_t *pt, int count, int levels, float amp0, float decay,
                                const vec3_t u, const vec3_t v, const vec3_t axis, float axial,
                                float startfade, randomseed_t *seed, randomseed_t *slow, int slowlevels)
{
	float amp = amp0;
	int k, i;
	for (k = 0; k < levels; k++)
	{
		int span = (count - 1) >> k;
		int half = span >> 1;
		randomseed_t *src = (slow && k < slowlevels) ? slow : seed;
		if (half < 1)
			break;
		for (i = half; i < count - 1; i += span)
		{
			vec3_t mid, disp;
			// The three random scalars are drawn HERE and not inline in the
			// VectorMAM/VectorMA scale slots below, because those macros expand
			// each scale argument once PER COMPONENT. Called inline they drew nine
			// numbers instead of three and handed every axis a different scalar,
			// which discards the stable (u,v) basis CL_Beam_M5_Basis just built and
			// burns the seeded stream three times too fast.
			float du = Math_crandomf(src) * amp;
			float dv = Math_crandomf(src) * amp;
			float da = Math_crandomf(src) * amp * axial;
			float chord, cap, dlen;
			VectorMAM(0.5f, pt[i - half], 0.5f, pt[i + half], mid);
			chord = (float)VectorDistance(pt[i - half], pt[i + half]);
			VectorMAM(du, u, dv, v, disp);
			// hold the turn at this node under 72 degrees so the ribbon miter
			// cannot fold back through itself
			cap = chord * M5BOLT_KINKFRAC;
			dlen = (float)VectorLength(disp);
			if (dlen > cap && dlen > 0.0f)
				VectorScale(disp, cap / dlen, disp);
			// the axial term is what makes segment LENGTHS vary. Without it the
			// nodes stay evenly spaced along the axis and the bolt reads as a
			// mechanical zigzag however wild the perpendicular displacement is.
			VectorMA(disp, da, axis, disp);
			if (startfade > 0.0f)
			{
				float f = ((float)i / (float)(count - 1)) / startfade;
				if (f < 1.0f)
					VectorScale(disp, f * f * (3.0f - 2.0f * f), disp);		// smoothstep
			}
			VectorAdd(mid, disp, pt[i]);
		}
		amp *= decay;
	}
}

/// Merge away nodes that landed on top of their predecessor. Always keeps the
/// first and last, so the endpoints stay locked. Returns the surviving count.
static int CL_Beam_M5_Compact(vec3_t *pt, int count)
{
	int i, n = 1;
	// NOTE: VectorCopy is a comma-expression macro that evaluates its OUTPUT
	// argument once per component, so `pt[n++]` would advance n three times per
	// node and scatter the three components across three different points. Keep
	// the increment out of the macro.
	for (i = 1; i < count - 1; i++)
		if (VectorDistance2(pt[i], pt[n - 1]) > M5BOLT_MINSEG * M5BOLT_MINSEG)
		{
			VectorCopy(pt[i], pt[n]);
			n++;
		}
	VectorCopy(pt[count - 1], pt[n]);
	n++;
	return n;
}

/// Pull each node of a sub-line back to what is actually visible from an anchor
/// on that line, so the bolt cannot be drawn through a wall, a pillar, a door or
/// a lift. Endpoints are exempt: node 0 is the muzzle and the last node is the
/// impact, and both are contracts with CL_Beam_CalculatePositions.
///
/// A per-node TETHER, not a segment walk. Clipping segment by segment would
/// change the polyline's topology and force ribbon restarts; pulling each node
/// back along the line from a known-good anchor keeps the node count and the
/// index arithmetic that every sub-line depends on.
///
/// Two properties worth stating because they are easy to break:
/// - It reads nothing from r_refdef.view, so it CANNOT make the bolt swim. It
///   changes when a door opens, which is correct, not swimming.
/// - It draws NO random numbers. If it ever touched the seed it would shift the
///   stream and every fork and filament downstream would change with the world
///   geometry, which would be untraceable.
/// cliplast: whether the final node may move. False for the main channel (its
/// end is the impact point) and for filaments (both ends are shared channel
/// nodes); TRUE for forks, whose tip is a free end in mid-air and is exactly the
/// thing most likely to be buried in a wall.
static int CL_Beam_M5_ClipLine(vec3_t *pt, int count, const vec3_t anchor, qbool anchorisline,
                               const vec3_t linestart, const vec3_t lineaxis, float backoff,
                               qbool cliplast)
{
	int i, last = cliplast ? count : count - 1, clipped = 0;
	for (i = 1; i < last; i++)
	{
		vec3_t from, dir;
		trace_t tr;
		float dist;
		if (anchorisline)
		{
			// nearest point on the straight start->end line: a short local trace,
			// and the line itself is what the server actually traced
			vec3_t rel;
			VectorSubtract(pt[i], linestart, rel);
			VectorMA(linestart, DotProduct(rel, lineaxis), lineaxis, from);
		}
		else
			VectorCopy(anchor, from);		// forks and filaments tether to their own root

		VectorSubtract(pt[i], from, dir);
		dist = (float)VectorLength(dir);
		// Most nodes sit within a few units of the line and cannot be through
		// anything the line itself did not already pass. Skipping those is where
		// almost all of this function's cost goes: at defaults it removes roughly
		// two thirds of the traces and changes nothing on screen.
		if (dist < M5BOLT_CLIPMIN)
			continue;
		VectorScale(dir, 1.0f / dist, dir);
		m5_cliptraces++;
		tr = CL_TraceLine(from, pt[i], MOVE_NOMONSTERS, NULL,
			SUPERCONTENTS_SOLID | SUPERCONTENTS_SKY, 0, 0,
			collision_extendmovelength.value, true, false, NULL, false, false);
		// startsolid means the anchor itself is buried - under instantaimhack the
		// straight line is not the server's traced line, so this really happens.
		// Clipping against a garbage trace is worse than not clipping.
		if (tr.startsolid || tr.fraction >= 1.0f)
			continue;
		VectorMA(from, max(0.0f, dist * tr.fraction - backoff), dir, pt[i]);
		clipped++;
	}
	return clipped;
}

extern matrix4x4_t viewmodelmatrix_withbob;		// cl_main.c, built by V_CalcRefdef
extern cvar_t cl_beams_instantaimhack;			// cl_main.c

/// Where the bolt should APPEAR to come from: the barrel of the gun you can see.
///
/// The stock start point is either the player's origin (waist) or the server's
/// `origin + 16`, neither of which is the barrel, and both of which sit a fixed
/// distance BELOW the eye. That offset rotates into view as you pitch, which is
/// why the bolt's origin reads wrong looking up or down - and it sits right on
/// M5BOLT_NEARCLIP's boundary, so the first nodes flicker in and out.
///
/// viewmodelmatrix_withbob is built by V_CalcRefdef (view.c:930) from gunorg and
/// gunangles including bob and kick, and V_CalcRefdef runs BEFORE
/// CSQC_RelinkAllEntities, so it is this frame's and it is valid here. Returns
/// false when there is no view model to attach to.
/// Which of the gun's three electrodes the arc is standing on this instant:
/// -1 left, 0 centre, +1 right.
///
/// A real arc between electrodes does not sit still, and ours always left the
/// right-hand prong, which is the one thing that gave away that the origin was a
/// fixed offset rather than a contact point. This walks it.
///
/// Its own 15 Hz clock, NOT the shape re-roll's. At the default rate the two are
/// the same integer -- same phase, same rate -- so the electrode changes on the
/// same tick the shape does and the jump is hidden inside a change that was
/// happening anyway. Raise r_lightningbeam_m5_rate and the shape speeds up while
/// this stays at 15, which is the specified ceiling and is now a property of the
/// build rather than of the config, the way the flicker floor is.
///
/// A pure hash, not a draw from the shape stream: taking a number from `seed`
/// here would shift every fork and filament downstream of it.
/// Split from the clock so the self-test can drive the real mapping over a run
/// of ticks. Testing the hash alone would pass while this function was pinned.
static float CL_Beam_M5_MuzzleNodeAt(int entity, int slot, int tick)
{
	unsigned int h = CL_Beam_M5_Hash(entity, slot, tick, 0x2545f491u);
	return (float)((int)(h % 3u) - 1);
}

static float CL_Beam_M5_MuzzleNode(int entity, int slot)
{
	int tick;
	if (!r_lightningbeam_m5_muzzlenodes.integer)
		return 1.0f;			// the right-hand prong, exactly as before
	tick = (int)floor(cl.time * M5BOLT_MUZZLERATE + CL_Beam_M5_Phase(entity, slot, 0x9e3779b9u));
	return CL_Beam_M5_MuzzleNodeAt(entity, slot, tick);
}

/// node: which electrode the arc is standing on this instant, -1 left, 0 centre,
/// +1 right. 1 is where it always sat and is what the feature switched off gives.
static qbool CL_Beam_M5_Muzzle(const beam_t *b, vec3_t out, float node)
{
	vec3_t local;
	if (!r_lightningbeam_m5_muzzle.integer || b->entity != cl.viewentity || chase_active.integer)
		return false;
	// Quake entity space: x forward, y LEFT, z up
	VectorSet(local, r_lightningbeam_m5_muzzleforward.value,
	                -r_lightningbeam_m5_muzzleright.value * node,
	                 r_lightningbeam_m5_muzzleup.value);
	Matrix4x4_Transform(&viewmodelmatrix_withbob, local, out);
	// V_CalcRefdef leaves the matrix as identity when there is no view model
	// (chase cam, intermission, not connected); a muzzle at the world origin
	// would be spectacular, so reject it
	return VectorDistance2(out, r_refdef.view.origin) < 4096.0f;	// within 64 units of the eye
}

/// Grow one frizzle branch from `root` heading `dir`, and recurse.
///
/// This is the Reed & Wyvill shape: each generation throws a fixed fan of
/// shorter, dimmer, wilder children off the previous one's tip, so a free end
/// frays into progressively finer filaments instead of stopping. `sigma` is the
/// lean off the parent in RADIANS and ramps sigma1 -> sigma2 across the
/// generations; brightness is multiplied by `dim` at each one.
///
/// Every child is displaced with the same recursion the channel uses, at an
/// amplitude derived from its own length and lean - a sub-line handed the
/// channel's absolute amplitude pegs against the kink cap at every level and
/// comes out a shard, which is the same trap the fork and filament loops
/// document.
static void CL_Beam_M5_FrizzleBranch(m5boltpath_t *out, const vec3_t root, const vec3_t dir,
                                     float len, float width, float alpha, float taper,
                                     int gen, int gens, float sigma1, float sigma2, float dim,
                                     float decay, float axial, float toffset, randomseed_t *seed)
{
	vec3_t tip, fu, fv, cdir, rnd;
	m5boltline_t *ln;
	int first, c, child;
	float sigma, amp;
	if (gen >= gens || len < 1.0f || alpha <= 0.002f)
		return;
	sigma = (gens > 1) ? (sigma1 + (sigma2 - sigma1) * ((float)gen / (float)(gens - 1))) : sigma1;
	for (child = 0; child < M5BOLT_FRIZZFANOUT; child++)
	{
		float rx, ry, rz;
		if (out->numlines >= M5BOLT_MAXLINES || out->numpoints + M5BOLT_FRIZZNODES > M5BOLT_MAXPOINTS)
			return;
		// Draw the three scalars into locals first: VectorSet is a comma-expression
		// macro that evaluates each argument once per component, so calling
		// Math_crandomf inline would draw nine numbers and burn the stream.
		rx = Math_crandomf(seed); ry = Math_crandomf(seed); rz = Math_crandomf(seed);
		VectorSet(rnd, rx, ry, rz);
		if (VectorLength2(rnd) < 1e-4f)
			continue;
		VectorNormalize(rnd);
		// lean off the parent by roughly sigma. tan() of the half-angle is the
		// sideways fraction of a unit step, which is what makes sigma read as an
		// angle rather than as an arbitrary blend weight.
		VectorMA(dir, tanf(sigma), rnd, cdir);
		if (VectorLength2(cdir) < 1e-4f)
			continue;
		VectorNormalize(cdir);
		VectorMA(root, len, cdir, tip);

		first = out->numpoints;
		for (c = 0; c < M5BOLT_FRIZZNODES; c++)
			VectorLerp(root, (float)c / (float)(M5BOLT_FRIZZNODES - 1), tip, out->points[first + c]);
		CL_Beam_M5_Basis(cdir, seed, fu, fv);
		amp = min(len * tanf(sigma) * 0.9f, M5BOLT_MAXDEV);
		CL_Beam_M5_Displace(out->points + first, M5BOLT_FRIZZNODES, 2, amp, decay,
			fu, fv, cdir, axial, 0.0f, seed, NULL, 0);
		VectorCopy(root, out->points[first]);
		// Frizzle grows from the very point the channel landed on, so the first
		// generation genuinely does head into the wall and has to be pulled back
		// like a fork - last node included, since its tip is a free end.
		//
		// Only the FIRST generation, though, and that is a cost decision with a
		// number behind it: clipping every generation took one bolt from 31 clip
		// traces a frame to 201, which is a real slice of the budget for hairs a
		// few pixels long. Later generations grow from a tip this pass has already
		// pulled into open space, and are 40% shorter and half as bright again at
		// each step.
		if (r_lightningbeam_m5_clip.integer > 1 && gen == 0)
			CL_Beam_M5_ClipLine(out->points + first, M5BOLT_FRIZZNODES, out->points[first], false, NULL, NULL,
				bound(0.0f, r_lightningbeam_m5_clipback.value, 16.0f), true);
		out->numpoints += M5BOLT_FRIZZNODES;

		ln = &out->lines[out->numlines++];
		ln->first = first; ln->count = M5BOLT_FRIZZNODES;
		ln->width = width;
		ln->widthtip = width * 0.25f;
		ln->alpha = alpha;
		ln->alphatip = 0.0f;
		ln->toffset = toffset + 0.17f * (float)(child + 1); ln->trepeat = 1.6f;
		ln->roll = 0.0f;
		ln->whiten = 0;
		ln->taperstart = taper;

		CL_Beam_M5_FrizzleBranch(out, tip, cdir, len * M5BOLT_FRIZZLENDECAY,
			width * 0.62f, alpha * dim, taper, gen + 1, gens, sigma1, sigma2, dim,
			decay, axial, toffset + 0.31f, seed);
	}
}

/// Throw a few very short hairs sideways off the last stretch of a branch.
///
/// The fan above gives a tip its silhouette; these give it TEXTURE - in the
/// reference photographs the last stretch of every streamer is furred with
/// hair-thin stubs far too short to read as branches. They are two-segment and
/// alpha-tapered to nothing, so they cost almost nothing to emit.
static void CL_Beam_M5_Streamers(m5boltpath_t *out, const vec3_t *pt, int count,
                                 float len, float width, float alpha, float density,
                                 float decay, randomseed_t *seed)
{
	int i, n, emitted = 0;
	if (count < 2 || density <= 0.0f || len < 1.0f)
		return;
	// only the terminal fifth of the branch, and never its very first node
	n = max(1, count / 5);
	for (i = max(1, count - n); i < count && emitted < M5BOLT_MAXSTREAMERS; i++)
	{
		vec3_t tang, sdir, tip, su, sv, rnd;
		m5boltline_t *ln;
		int first, c;
		float rx, ry, rz;
		if (Math_randomf(seed) >= bound(0.0f, density, 1.0f))
			continue;
		if (out->numlines >= M5BOLT_MAXLINES || out->numpoints + M5BOLT_STREAMERNODES > M5BOLT_MAXPOINTS)
			return;
		VectorSubtract(pt[i], pt[i - 1], tang);
		if (VectorLength2(tang) < 1e-4f)
			continue;
		VectorNormalize(tang);
		rx = Math_crandomf(seed); ry = Math_crandomf(seed); rz = Math_crandomf(seed);
		VectorSet(rnd, rx, ry, rz);
		if (VectorLength2(rnd) < 1e-4f)
			continue;
		VectorNormalize(rnd);
		// mostly sideways - a hair that follows the branch is invisible against it
		VectorMA(rnd, 0.35f, tang, sdir);
		if (VectorLength2(sdir) < 1e-4f)
			continue;
		VectorNormalize(sdir);
		VectorMA(pt[i], len, sdir, tip);

		first = out->numpoints;
		for (c = 0; c < M5BOLT_STREAMERNODES; c++)
			VectorLerp(pt[i], (float)c / (float)(M5BOLT_STREAMERNODES - 1), tip, out->points[first + c]);
		CL_Beam_M5_Basis(sdir, seed, su, sv);
		CL_Beam_M5_Displace(out->points + first, M5BOLT_STREAMERNODES, 1,
			min(len * 0.35f, M5BOLT_MAXDEV), decay, su, sv, sdir, 0.0f, 0.0f, seed, NULL, 0);
		VectorCopy(pt[i], out->points[first]);
		out->numpoints += M5BOLT_STREAMERNODES;

		ln = &out->lines[out->numlines++];
		ln->first = first; ln->count = M5BOLT_STREAMERNODES;
		ln->width = width;
		ln->widthtip = 0.0f;			// sub-pixel by the tip, by construction
		ln->alpha = alpha;
		ln->alphatip = 0.0f;
		ln->toffset = 0.43f * (float)(emitted + 1); ln->trepeat = 2.0f;
		ln->roll = 0.0f;
		ln->whiten = 0;
		ln->taperstart = 0.35f;
		emitted++;
	}
}

/// Fray every free tip of an already-built path, and append the result.
///
/// Runs off its OWN seed stream, derived from the roll with a fixed salt, so the
/// channel, the filaments and the forks come out bit-identical whether frizzle is
/// on or off - which is what makes the A/Bs single-variable and the cvar-0 case a
/// structural no-op rather than a numerical coincidence.
///
/// Called TWICE per bolt: once for this roll at full brightness, and once for the
/// PREVIOUS roll, faded, so the fine detail cross-dissolves instead of switching
/// at the re-roll rate. The ghost is replayed from the previous roll's seed rather
/// than from stored geometry, which costs no memory and, more importantly, keeps
/// it anchored to the bolt's CURRENT endpoints - stored world-space geometry would
/// detach into a spray behind the bolt on a fast swing. It is not re-jittered: the
/// same seed gives the same shape it had last roll.
static void CL_Beam_M5_Frizzle(m5boltpath_t *out, int entity, int slot, int roll, float bright,
                               const int *tipfirst, const int *tipcount, int numtips,
                               float thickness, float length, float decay, float axial, float taper)
{
	randomseed_t fseed;
	int t, gens;
	float sigma1, sigma2, dim, width, alpha, flen, streamers;
	if (bright <= 0.002f || numtips < 1)
		return;
	// A separate stream keyed on the roll with its own salt. XOR with a constant is
	// a bijection, so roll and roll-1 can never collide, and the avalanche in
	// CL_Beam_M5_Hash keeps it independent of the shape and pose streams.
	CL_Beam_M5_Seed(entity, slot, roll ^ 0x5bf03635, &fseed);
	gens = bound(1, r_lightningbeam_m5_frizzle_gens.integer, 3);
	sigma1 = bound(0.0f, r_lightningbeam_m5_frizzle_sigma1.value, 70.0f) * (float)(M_PI / 180.0);
	sigma2 = bound(0.0f, r_lightningbeam_m5_frizzle_sigma2.value, 70.0f) * (float)(M_PI / 180.0);
	dim = bound(0.1f, r_lightningbeam_m5_frizzle_dim.value, 1.0f);
	streamers = bound(0.0f, r_lightningbeam_m5_frizzle_streamers.value, 1.0f);
	width = thickness * bound(0.1f, r_lightningbeam_m5_corewidth.value, 1.0f) * 0.42f;
	alpha = bound(0.0f, r_lightningbeam_m5_branchalpha.value, 1.0f) * 1.1f * bright;
	flen = length * M5BOLT_FRIZZLEN;
	for (t = 0; t < numtips; t++)
	{
		const vec3_t *pt = (const vec3_t *)(out->points + tipfirst[t]);
		int n = tipcount[t];
		vec3_t dir;
		// The tip inherits its parent's full width so the energy carries INTO the
		// fray rather than vanishing at the taper - a channel that dissolves into
		// nothing with nothing growing out of it just reads as a weak bolt.
		if (n < 2)
			continue;
		VectorSubtract(pt[n - 1], pt[n - 2], dir);
		if (VectorLength2(dir) < 1e-4f)
			continue;
		VectorNormalize(dir);
		CL_Beam_M5_FrizzleBranch(out, pt[n - 1], dir, flen, width, alpha, taper,
			0, gens, sigma1, sigma2, dim, decay, axial, 0.07f * (float)(t + 1), &fseed);
		CL_Beam_M5_Streamers(out, pt, n, length * M5BOLT_STREAMERLEN,
			width * 0.5f, alpha * 0.35f, streamers, decay, &fseed);
	}
}

static void CL_Beam_M5_BallHue(vec3_t hue);		// the ball's palette (defined with the ball producer below)

qbool CL_Beam_M5_BuildPath(const beam_t *b, const vec3_t start, const vec3_t end, m5boltpath_t *out)
{
	randomseed_t seed, slowseed;
	vec3_t axis, u, v, bstart, bend, muzzle;
	int hold = 0, pose = 0, slowlevels = 0;
	m5boltline_t *ln;
	const float *texw;
	float length, amp0, jitter, decay, axial, thickness, hmax, seglen, hitmul;
	float beamrgb[3];
	int levels, nodes, i, c, slot, forks, filaments, maxforks, maxfilaments, clipped = 0;
	int roll = 0, frizzle, baselines = 0;
	int isball = 0;
	float maintaper = 0.0f, rollfrac = 0.0f;
	int tipfirst[M5BOLT_MAXTIPS], tipcount[M5BOLT_MAXTIPS], numtips = 0;

	m5_cliptraces = 0;
	VectorCopy(start, bstart);
	VectorCopy(end, bend);

	// Re-trace the landing point. Under cl_beams_instantaimhack -- which Seb runs
	// -- CL_Beam_CalculatePositions rebuilds the end as eye + len*viewforward
	// (cl_main.c) while the start stays at the waist. That endpoint is on NO
	// SURFACE AT ALL: it is simply a distance along the view axis. So the drawn
	// beam is a slanted line that ends in mid-air, which is why it can appear to
	// run through a close wall or pillar, and why the impact flash had nothing to
	// land on. Re-tracing along the line the player is actually aiming down, for
	// the length the server gave us, puts the end back on real geometry without
	// changing the weapon's reach.
	if (r_lightningbeam_m5_endtrace.integer && b->entity == cl.viewentity
	 && cl_beams_instantaimhack.integer && !chase_active.integer)
	{
		vec3_t dir;
		float len;
		VectorSubtract(bend, bstart, dir);
		len = (float)VectorLength(dir);
		if (len > 1.0f)
		{
			trace_t tr;
			VectorScale(dir, 1.0f / len, dir);
			VectorMA(bstart, len, dir, bend);
			tr = CL_TraceLine(bstart, bend, MOVE_NOMONSTERS, NULL,
				SUPERCONTENTS_SOLID | SUPERCONTENTS_SKY, 0, 0,
				collision_extendmovelength.value, true, false, NULL, false, false);
			if (!tr.startsolid)
				VectorCopy(tr.endpos, bend);
		}
	}

	VectorSubtract(bend, bstart, axis);
	length = (float)VectorLength(axis);
	if (length < 1.0f)
		return false;			// degenerate beam: nothing sane to build
	VectorScale(axis, 1.0f / length, axis);

	// The scene mesh indexes vertices as unsigned short. A bolt is ~150 vertices
	// against a 65536 ceiling, so this can only trip with an absurd number of
	// simultaneous beams - but a silent wrap would corrupt every effect sharing
	// the mesh, so refuse rather than risk it.
	if (CL_Mesh_Scene()->surfmesh.num_vertices > 60000)
		return false;

	slot = (int)(b - cl.beams);

	// Seed from the beam identity and the RE-ROLL INDEX, not from the frame. The
	// shape is therefore bit-identical for every frame inside one roll interval
	// and at any framerate, and it changes exactly r_lightningbeam_m5_rate times
	// a second. Note this uses the seeded Math_* generator throughout and never
	// rand()/lhrandom: drawing from the global stream here would desynchronise
	// every particle consumer downstream of us.
	{
		float rate = bound(2.0f, r_lightningbeam_m5_rate.value, 30.0f);
		float phase = CL_Beam_M5_Phase(b->entity, slot, 0x9e3779b9u);
		double rollpos = cl.time * rate + phase;
		roll = (int)floor(rollpos);
		rollfrac = (float)(rollpos - floor(rollpos));
		// Test hook: pin the roll and the ghost's fade together, so a frozen bed
		// gets a bit-identical bolt AND a bit-identical ghost every boot. Pinning
		// only the roll would leave the ghost's alpha riding cl.time.
		if (r_lightningbeam_m5_seed.integer)
		{
			roll = r_lightningbeam_m5_seed.integer;
			rollfrac = 0.0f;
		}
		CL_Beam_M5_Seed(b->entity, slot, roll, &seed);
		hold = bound(0, r_lightningbeam_m5_hold.integer, 30);
		if (hold > 0)
		{
			// a SECOND stream, indexed by pose rather than by roll. The coarse
			// levels drawn from it hold their shape for the whole pose, so the
			// bolt keeps an overall form while the fine detail crackles under it.
			pose = CL_Beam_M5_PoseIndex(b->entity, slot, roll, hold);
			CL_Beam_M5_Seed(b->entity, slot, pose + 0x40000000, &slowseed);
			slowlevels = bound(0, r_lightningbeam_m5_holdlevels.integer, 5);
		}
	}

	seglen = bound(8.0f, r_lightningbeam_m5_seglen.value, 128.0f);
	levels = (int)floor(log(length / seglen) / log(2.0));
	levels = bound(3, levels, 5);
	nodes = (1 << levels) + 1;

	decay = bound(0.4f, r_lightningbeam_m5_decay.value, 0.9f);
	axial = bound(0.0f, r_lightningbeam_m5_axial.value, 0.4f);
	// jitter is a displacement PER UNIT OF LENGTH; every sub-line below scales it
	// by its own length rather than reusing the channel's absolute amplitude
	jitter = bound(0.0f, r_lightningbeam_m5_jitter.value, 0.25f);
	// M5 ball lightning: an arc OWNED BY A BALL is drawn as a plasma filament
	// under r_lightningbeam_m5_ballarcs -- a smoother, thinner, violet, unforked
	// version of the bolt. Every override below is `if (isball)`, so a player
	// beam's path is textually and numerically the old one.
	isball = r_lightningbeam_m5_ballarcs.integer && b->entity > 0 && b->entity < cl.num_entities
	      && cl.entities_active[b->entity] && (cl.entities[b->entity].render.effects & EF_M5BALL);
	if (isball)
	{
		jitter *= 0.6f;
		decay = 0.4f;			// the low octave bends it, the high ones barely ripple
	}
	amp0 = min(jitter * length, M5BOLT_MAXDEV);

	// ---- the main channel -------------------------------------------------
	// the basis follows the POSE: the plane the bolt bends in is most of what
	// reads as "which way it is leaning", so it must hold with the pose or the
	// bolt would still flail every tick
	CL_Beam_M5_Basis(axis, (hold > 0 && slowlevels > 0) ? &slowseed : &seed, u, v);
	for (i = 0; i < nodes; i++)
		VectorLerp(bstart, (float)i / (float)(nodes - 1), bend, out->points[i]);
	CL_Beam_M5_Displace(out->points, nodes, levels, amp0, decay, u, v, axis, axial, 0.22f,
		&seed, hold > 0 ? &slowseed : NULL, slowlevels);
	// endpoints are never displaced by the recursion, but say so explicitly:
	// they are the contract with CL_Beam_CalculatePositions and with the light walk
	VectorCopy(bstart, out->points[0]);
	VectorCopy(bend, out->points[nodes - 1]);

	// Pull the first stretch onto the gun's barrel. A weighted BLEND, not a
	// translation: the weight is 1 at the start and smoothsteps to 0 by
	// _muzzlefade along the beam, so the far end and the impact point cannot
	// move. This is the only view-dependent thing in the shape, and it has to be
	// - the weapon it is attached to is view-locked, and there is no world-space
	// way to know where the drawn gun is. The property that still holds is the
	// one that matters: the bolt does not swim where the player is AIMING.
	if (CL_Beam_M5_Muzzle(b, muzzle, CL_Beam_M5_MuzzleNode(b->entity, slot)))
	{
		vec3_t delta;
		float fade = bound(0.02f, r_lightningbeam_m5_muzzlefade.value, 0.5f);
		VectorSubtract(muzzle, bstart, delta);
		for (i = 0; i < nodes - 1; i++)
		{
			float t = ((float)i / (float)(nodes - 1)) / fade;
			float w;
			if (t >= 1.0f)
				break;
			w = 1.0f - (t * t * (3.0f - 2.0f * t));		// smoothstep, 1 at the muzzle
			VectorMA(out->points[i], w, delta, out->points[i]);
		}
	}
	if (r_lightningbeam_m5_clip.integer > 0)
		clipped += CL_Beam_M5_ClipLine(out->points, nodes, NULL, true, bstart, axis,
			bound(0.0f, r_lightningbeam_m5_clipback.value, 16.0f), false);
	nodes = CL_Beam_M5_Compact(out->points, nodes);

	out->numpoints = nodes;
	out->channelfirst = 0;
	out->channelcount = nodes;
	out->length = length;

	// Register the channel as the first free tip for frizzle. Registered HERE,
	// after compaction, so the count is the surviving one - and registered first
	// so that if an absurd fork count exhausts the line budget it is the forks'
	// frays that get dropped, never the channel's.
	tipfirst[numtips] = 0;
	tipcount[numtips] = nodes;
	numtips++;

	// ---- colour -----------------------------------------------------------
	// beamrgb is what the player actually SEES: the cvar times the weighting
	// baked into the texture. hue normalises that to a max channel of 1 so the
	// brightness knobs mean the same thing whatever hue is chosen.
	texw = r_lightningbeam_qmbtexture.integer ? m5_texweight_qmb : m5_texweight_builtin;
	beamrgb[0] = r_lightningbeam_color_red.value * texw[0];
	beamrgb[1] = r_lightningbeam_color_green.value * texw[1];
	beamrgb[2] = r_lightningbeam_color_blue.value * texw[2];
	hmax = max(beamrgb[0], max(beamrgb[1], beamrgb[2]));
	// A zero colour means the player asked for no beam at all. Leaving hue at
	// zero here is what lets CL_Beam_M5_AddPolygons detect that and skip the
	// whitening, rather than lifting a zero hue back up into a grey core.
	if (hmax < 1e-6f)
		hmax = 1.0f;
	for (c = 0; c < 3; c++)
		out->hue[c] = beamrgb[c] / hmax;
	if (isball)
		CL_Beam_M5_BallHue(out->hue);		// the ball's own palette, not the thunderbolt's

	// Is this bolt currently doing damage? The server says so by sending
	// TE_LIGHTNING1 instead of TE_LIGHTNING2, which reaches us as the beam's
	// model - free, no protocol change, no extra traffic. Scoped to PLAYER-owned
	// beams because the Shambler's attack (qc/shambler.qc) is a TE_LIGHTNING1 in
	// vanilla and always has been; without this test every Shambler bolt would
	// read as connecting. Entities 1..maxclients are the players.
	{
		// The raw flag flips at the 10 Hz weapon tick, so a strong boost applied
		// straight from it would be a 10 Hz luminance strobe - the exact band the
		// flicker floor exists to keep out of the frame. Decay it instead: the
		// flare is continuous while the beam stays on a target and eases off when
		// you sweep away.
		float fade = bound(0.0f, r_lightningbeam_m5_hitfade.value, 1.0f);
		// M5 ball lightning: an arc OWNED BY A BALL (or its arc helper, both
		// tagged EF_M5BALL client-side) is a connect too -- qc/m5ball.qc sends
		// TE_LIGHTNING1 only for an arc that damaged something. The Shambler
		// carries no such tag, so its scope exclusion stands.
		qbool raw = (b->model != NULL && b->model == cl.model_bolt
		          && ((b->entity >= 1 && b->entity <= cl.maxclients)
		           || (b->entity > 0 && b->entity < cl.num_entities && cl.entities_active[b->entity]
		               && (cl.entities[b->entity].render.effects & EF_M5BALL))));
		if (slot >= 0 && slot < MAX_BEAMS)
		{
			if (m5_hitent[slot] != b->entity)
			{
				m5_hitent[slot] = b->entity;
				m5_hittime[slot] = -1000.0;
			}
			if (raw)
				m5_hittime[slot] = cl.time;
			out->hit = (fade > 0.0f)
				? 1.0f - bound(0.0f, (float)((cl.time - m5_hittime[slot]) / fade), 1.0f)
				: (raw ? 1.0f : 0.0f);
		}
		else
			out->hit = raw ? 1.0f : 0.0f;
	}

	out->envelope = CL_Beam_M5_Envelope(cl.time,
		CL_Beam_M5_Phase(b->entity, slot, 0x1b873593u) * (float)(2.0 * M_PI),
		CL_Beam_M5_Phase(b->entity, slot, 0xcc9e2d51u) * (float)(2.0 * M_PI));

	// A rate-limited shape report, so "the bolt looks wrong" can be turned into
	// numbers without a debugger. maxdev is the furthest any node strays from the
	// straight start->end line; it should sit near amp0 and never exceed
	// M5BOLT_MAXDEV. A node count that is not (1 << levels) + 1 or slightly under
	// means the path array has been corrupted.
	if (developer.integer >= 2 && cl.time - m5_lastreport > 1.0)
	{
		float maxdev = 0.0f;
		m5_lastreport = cl.time;
		for (i = 0; i < nodes; i++)
		{
			vec3_t rel, proj;
			float t, dev;
			VectorSubtract(out->points[i], bstart, rel);
			t = DotProduct(rel, axis);
			VectorMA(bstart, t, axis, proj);
			dev = (float)VectorDistance(out->points[i], proj);
			if (dev > maxdev)
				maxdev = dev;
		}
		Con_Printf("M5 bolt: len %.0f levels %d nodes %d amp %.1f maxdev %.1f clip %d/%d pose %d env %.2f %s\n",
			(double)length, levels, nodes, (double)amp0, (double)maxdev, clipped, m5_cliptraces, pose,
			(double)out->envelope, out->hit > 0.01f ? "CONNECTED" : "free");
		m5_reportpending = true;
	}

	// The connect flare. Colour alone cannot do this: the core's dominant channel
	// is already at or past 1.0 where the beam texture peaks, and the blend is
	// additive, so multiplying it clips to the identical pixel - which is exactly
	// why the first attempt at this read as nothing at all. Widths, counts and
	// radii cannot clip, so those are what carry it. Every factor is written
	// 1 + k(h-1), so at hitboost 1 the whole feature is exactly inert.
	hitmul = 1.0f + (bound(1.0f, r_lightningbeam_m5_hitboost.value, 3.0f) - 1.0f) * out->hit;

	// ---- core and sheath --------------------------------------------------
	thickness = max(0.1f, r_lightningbeam_thickness.value);
	if (isball)
		thickness *= 0.5f;
	out->numlines = 0;

	// The terminal dissolve is part of the frizzle feature and is switched with
	// it: with frizzle off every line keeps taperstart 0, which is the branch the
	// ribbon emitter never takes, so the geometry is the old geometry exactly.
	// Bounded below 1 because the emitter divides by (1 - taperstart).
	frizzle = (r_lightningbeam_m5_frizzle.integer && !isball) ? 1 : 0;
	maintaper = frizzle ? bound(0.3f, r_lightningbeam_m5_frizzle_taper.value, 0.95f) : 0.0f;
	if (frizzle && r_lightningbeam_m5_frizzle_taper.value >= 1.0f)
		maintaper = 0.0f;			// 1 means "no taper", per the cvar's own help

	{
		// The core is emitted as SEVERAL passes rolled about its own axis, so it
		// reads as a tube rather than a flat card. The material is additive, so
		// the passes must share a fixed energy budget - piling three passes on at
		// full alpha would simply treble the brightness and clip, which is the
		// opposite of the "more volume, not more glare" the QA asked for.
		float corew = thickness * bound(0.1f, r_lightningbeam_m5_corewidth.value, 1.0f)
		            * (1.0f + 0.45f * (hitmul - 1.0f));
		float vol = bound(1.0f, r_lightningbeam_m5_corevolume.value, 3.0f);
		int tube = r_lightningbeam_m5_coretube.integer ? 1 : 0;
		int npass = tube ? 3 : 1;
		float pa = vol / (float)npass;
		static const float rolls[3] = { 0.0f, 0.7853982f, -0.7853982f };		// 0, +45, -45
		static const float offs[3]  = { 0.00f, 0.29f, 0.58f };
		static const float wide[3]  = { 1.00f, 1.30f, 1.30f };
		int pass;
		for (pass = 0; pass < npass; pass++)
		{
			ln = &out->lines[out->numlines++];
			ln->first = 0; ln->count = nodes;
			ln->width = ln->widthtip = corew * wide[pass];
			// the core is NOT modulated by the envelope: a steady hot filament
			// inside a breathing sheath reads far more like lightning than
			// everything pulsing together, and it keeps the brightest pixels stable
			ln->alpha = ln->alphatip = pa;
			ln->toffset = offs[pass]; ln->trepeat = 1.0f;
			ln->roll = rolls[pass];
			ln->whiten = isball ? 0 : 1;		// a plasma filament keeps its violet; no white core
			ln->taperstart = maintaper;
		}

		// the white-hot filament at the very centre. Narrow, so it cannot bloom,
		// and it is what stops the tube reading as a fat blur.
		if (r_lightningbeam_m5_coreinner.value > 0.0f && out->numlines < M5BOLT_MAXLINES)
		{
			ln = &out->lines[out->numlines++];
			ln->first = 0; ln->count = nodes;
			ln->width = ln->widthtip = corew * bound(0.05f, r_lightningbeam_m5_coreinner.value, 1.0f);
			ln->alpha = ln->alphatip = 1.0f;
			ln->toffset = 0.83f; ln->trepeat = 1.0f;
			ln->roll = 0.0f;
			ln->whiten = isball ? 0 : 1;		// a plasma filament keeps its violet; no white core
			ln->taperstart = maintaper;
		}
	}

	// (the cap guard is new: the sheath used to rely on the budget arithmetic
	// leaving room, which stopped being true once frizzle could fill the array)
	if (out->numlines >= M5BOLT_MAXLINES)
		return true;
	ln = &out->lines[out->numlines++];
	ln->first = 0; ln->count = nodes;
	ln->width = ln->widthtip = thickness * bound(0.5f, r_lightningbeam_m5_sheathwidth.value, 4.0f)
	                         * (1.0f + 0.25f * (hitmul - 1.0f));
	// half-depth envelope on everything but the core
	ln->alpha = ln->alphatip = bound(0.0f, r_lightningbeam_m5_sheathalpha.value, 1.0f) * (0.5f + 0.5f * out->envelope);
	// a different S phase and a slower repeat, so the sheath is structurally its
	// own thing rather than a scaled copy of the core
	ln->toffset = 0.37f; ln->trepeat = 0.6f;
	ln->roll = 0.0f;
	ln->whiten = 0;
	ln->taperstart = maintaper;

	// ---- filaments: threads that split off the channel and rejoin it -------
	// They share two channel nodes, so the split and the rejoin come for free.
	maxfilaments = bound(0, (int)r_lightningbeam_m5_filaments.integer + (out->hit > 0.5f ? 1 : 0), 3);
	if (isball)
		maxfilaments = 0;
	for (filaments = 0; filaments < maxfilaments && out->numlines < M5BOLT_MAXLINES; filaments++)
	{
		int span = (nodes > 12 && Math_randomf(&seed) < 0.5f) ? 8 : 4;
		int a, first;
		vec3_t fu, fv, fdir;
		if (nodes < span + 2 || out->numpoints + span + 1 > M5BOLT_MAXPOINTS)
			break;
		// keep filaments off the near-camera end for the same reason the channel
		// tapers there
		a = nodes / 8 + Math_randomrangei(&seed, 0, nodes - span - 1 - nodes / 8);
		a = bound(0, a, nodes - span - 1);
		first = out->numpoints;
		for (i = 0; i <= span; i++)
			VectorCopy(out->points[a + i], out->points[first + i]);
		VectorSubtract(out->points[first + span], out->points[first], fdir);
		if (VectorLength2(fdir) < 1e-4f)
			break;
		VectorNormalize(fdir);
		CL_Beam_M5_Basis(fdir, &seed, fu, fv);
		// Amplitude is per-unit-of-LENGTH, scaled to this filament's own span.
		// Handing a sub-line the whole beam's amplitude pegs it against the kink
		// cap at every level and it comes out as a shard rather than a thread.
		CL_Beam_M5_Displace(out->points + first, span + 1, (span == 8) ? 3 : 2,
			min(jitter * (float)VectorDistance(out->points[first], out->points[first + span]) * 0.8f, M5BOLT_MAXDEV),
			decay, fu, fv, fdir, axial, 0.0f, &seed, NULL, 0);
		if (r_lightningbeam_m5_clip.integer > 1)
			clipped += CL_Beam_M5_ClipLine(out->points + first, span + 1, out->points[first], false, NULL, NULL,
				bound(0.0f, r_lightningbeam_m5_clipback.value, 16.0f), false);
		out->numpoints += span + 1;

		ln = &out->lines[out->numlines++];
		ln->first = first; ln->count = span + 1;
		ln->width = thickness * bound(0.1f, r_lightningbeam_m5_corewidth.value, 1.0f) * 0.45f;
		ln->widthtip = ln->width;
		ln->alpha = ln->alphatip = bound(0.0f, r_lightningbeam_m5_branchalpha.value, 1.0f) * 1.2f * (0.5f + 0.5f * out->envelope);
		ln->toffset = 0.61f + 0.22f * filaments; ln->trepeat = 1.0f;
		ln->roll = 0.0f;
		ln->whiten = 0;
		// NO terminal taper: a filament's far end is a shared CHANNEL node, not a
		// free tip, so dissolving it would open a gap at the rejoin
		ln->taperstart = 0.0f;
	}

	// ---- forks: branches that leave the channel and die in mid-air ---------
	maxforks = bound(0, (int)r_lightningbeam_m5_branches.integer, 4);
	if (isball)
		maxforks = 0;
	{
		float chance = bound(0.0f, r_lightningbeam_m5_branchchance.value, 1.0f);
		int fnodes = (levels >= 4) ? 9 : 5;
		int flevels = (fnodes == 9) ? 3 : 2;
		// never fork in the first sixth: a branch at the muzzle reads wrong
		for (i = nodes / 6 + 1, forks = 0; i < nodes - 1 && forks < maxforks && out->numlines < M5BOLT_MAXLINES; i++)
		{
			vec3_t fdir, rnd, tip, fu, fv;
			float flen;
			int first;
			if (Math_randomf(&seed) >= chance)
				continue;
			if (out->numpoints + fnodes > M5BOLT_MAXPOINTS)
				break;
			VectorSubtract(out->points[i + 1], out->points[i], fdir);
			if (VectorLength2(fdir) < 1e-4f)
				continue;
			VectorNormalize(fdir);
			VectorSet(rnd, Math_crandomf(&seed), Math_crandomf(&seed), Math_crandomf(&seed));
			if (VectorLength2(rnd) < 1e-4f)
				continue;
			VectorNormalize(rnd);
			// Inherit most of the parent direction. A large random component
			// sprays forks sideways and even back past the camera, which reads
			// as an explosion rather than a discharge.
			VectorLerp(fdir, 0.35f, rnd, fdir);
			// Keep a healthy forward component. A fork that heads back toward the
			// camera has its TIP nearer the eye than its root, so it projects
			// wider as it goes and reads as a bright wedge rather than a branch -
			// perspective, not taper. Requiring roughly 60 degrees of forward is
			// also how real lightning branches.
			// (bounded: fdir exactly anti-parallel to axis is a fixed point of the
			// nudge, so this must never be a while loop)
			for (c = 0; c < 6 && DotProduct(fdir, axis) < 0.45f; c++)
			{
				VectorMA(fdir, 0.35f, axis, fdir);
				if (VectorLength2(fdir) < 1e-4f)
					break;
				VectorNormalize(fdir);
			}
			if (DotProduct(fdir, axis) < 0.2f)
				continue;			// gave up: skip this fork rather than emit a wedge
			if (VectorLength2(fdir) < 1e-4f)
				continue;
			VectorNormalize(fdir);
			flen = 0.35f * length * (1.0f - (float)i / (float)(nodes - 1));
			if (flen < seglen)
				continue;
			VectorMA(out->points[i], flen, fdir, tip);

			first = out->numpoints;
			for (c = 0; c < fnodes; c++)
				VectorLerp(out->points[i], (float)c / (float)(fnodes - 1), tip, out->points[first + c]);
			CL_Beam_M5_Basis(fdir, &seed, fu, fv);
			// wilder per unit length than the channel it left, but scaled to the
			// fork's OWN length - see the note in the filament loop above
			CL_Beam_M5_Displace(out->points + first, fnodes, flevels,
				min(jitter * flen * 1.6f, M5BOLT_MAXDEV), decay, fu, fv, fdir, axial, 0.0f, &seed, NULL, 0);
			VectorCopy(out->points[i], out->points[first]);
			// forks reach up to 0.35 * length off the channel, so they are by far
			// the most likely thing to end up inside a wall
			if (r_lightningbeam_m5_clip.integer > 1)
				clipped += CL_Beam_M5_ClipLine(out->points + first, fnodes, out->points[first], false, NULL, NULL,
					bound(0.0f, r_lightningbeam_m5_clipback.value, 16.0f), true);
			out->numpoints += fnodes;

			ln = &out->lines[out->numlines++];
			ln->first = first; ln->count = fnodes;
			ln->width = thickness * bound(0.1f, r_lightningbeam_m5_corewidth.value, 1.0f) * 0.5f;
			ln->widthtip = ln->width * 0.3f;
			// forks END IN AIR: they taper to nothing rather than being clipped,
			// so there is no hard stop anywhere in the frame
			ln->alpha = bound(0.0f, r_lightningbeam_m5_branchalpha.value, 1.0f) * (0.5f + 0.5f * out->envelope);
			ln->alphatip = 0.0f;
			ln->toffset = 0.11f * (float)(forks + 1); ln->trepeat = 1.3f;
			ln->roll = 0.0f;
			ln->whiten = 0;
			ln->taperstart = maintaper;
			if (numtips < M5BOLT_MAXTIPS)
			{
				tipfirst[numtips] = first;
				tipcount[numtips] = fnodes;
				numtips++;
			}
			forks++;
		}
	}

	// ---- frizzle: fray every free tip -------------------------------------
	baselines = out->numlines;
	if (frizzle)
	{
		float persist = bound(0.0f, r_lightningbeam_m5_frizzle_persist.value, 1.0f);
		CL_Beam_M5_Frizzle(out, b->entity, slot, roll, 1.0f, tipfirst, tipcount, numtips,
			thickness, length, decay, axial, maintaper);
		// The previous roll's fray, fading out across this roll. Without it the fine
		// tips switch instantaneously at the re-roll rate, which the eye reads as a
		// strobe on exactly the structure this feature adds; cross-dissolved they
		// read as a discharge lingering in the air. It goes SECOND so the live set
		// wins the array budget.
		if (persist > 0.0f)
			CL_Beam_M5_Frizzle(out, b->entity, slot, roll - 1, persist * (1.0f - rollfrac),
				tipfirst, tipcount, numtips, thickness, length, decay, axial, maintaper);
	}
	out->numfrizzlines = out->numlines - baselines;
	// The frizzle half of the shape report. Separate because the report above runs
	// while the channel is the only thing built; this is also the smoke check's
	// liveness hook, so the counts are the greppable part.
	if (m5_reportpending)
	{
		m5_reportpending = false;
		Con_Printf("M5 frizzle: %s tips %d lines %d frizzlines %d points %d traces %d node %+.0f taper %.2f ghost %.2f\n",
			frizzle ? "on" : "off", numtips, out->numlines, out->numfrizzlines, out->numpoints,
			m5_cliptraces, (double)CL_Beam_M5_MuzzleNode(b->entity, slot), (double)maintaper,
			(double)(frizzle ? bound(0.0f, r_lightningbeam_m5_frizzle_persist.value, 1.0f) * (1.0f - rollfrac) : 0.0f));
	}
	return true;
}

/// Emit one polyline as a camera-facing ribbon.
///
/// The basis is computed PER NODE and shared between the two quads that meet
/// there, so the ribbon does not split open at a kink the way independent
/// per-segment billboards do. Note the stock renderer computes one basis for the
/// whole beam and has no guard at all for a beam pointing at the camera, which
/// under cl_beams_instantaimhack is the normal case for your own lightning gun.
static void CL_Beam_M5_AddRibbon(model_t *mod, msurface_t *surf, const m5boltpath_t *p,
                                 const m5boltline_t *ln, const float *colour, float t0, float sscale)
{
	const vec3_t *pt = (const vec3_t *)(p->points + ln->first);
	int i, n = ln->count, prev0 = -1, prev1 = -1;
	float arc = 0.0f, total = 0.0f;
	vec3_t eye, eyefwd, eyeleft, eyeup;

	if (n < 2)
		return;

	// Take the camera from the view MATRIX, not from r_refdef.view.origin.
	// V_CalcRefdef fills the matrix before CL_RelinkBeams runs, but view.origin
	// is only written later, during rendering (R_SetupView) - so reading it here
	// billboards against LAST frame's eye. Everything downstream is affected: the
	// near-clip test, the end-on fade and the ribbon's orientation, and the
	// one-frame error is what made the near-clip boundary chatter while turning.
	// (The stock CL_Beam_AddPolygons has the same latent lag; it is deliberately
	// left alone so the master gate stays a provable no-op.)
	Matrix4x4_ToVectors(&r_refdef.view.matrix, eyefwd, eyeleft, eyeup, eye);
	for (i = 1; i < n; i++)
		total += (float)VectorDistance(pt[i - 1], pt[i]);
	if (total < 1e-4f)
		return;

	for (i = 0; i < n; i++)
	{
		vec3_t din, dout, tang, tv, rightv, nrm, off, va, vb;
		float frac, halfwidth, alpha, s, miter, l2, tvlen, endon;
		int e0, e1;

		if (i > 0)
		{
			VectorSubtract(pt[i], pt[i - 1], din);
			VectorNormalize(din);
		}
		if (i < n - 1)
		{
			VectorSubtract(pt[i + 1], pt[i], dout);
			VectorNormalize(dout);
		}
		if (i == 0)
			VectorCopy(dout, din);
		if (i == n - 1)
			VectorCopy(din, dout);

		VectorAdd(din, dout, tang);
		if (VectorLength2(tang) < 1e-6f)
			VectorCopy(din, tang);		// near-reversal: fall back to the incoming direction
		VectorNormalize(tang);

		VectorSubtract(eye, pt[i], tv);
		CrossProduct(tang, tv, rightv);
		l2 = VectorLength2(rightv);

		// |tang x tv| == |tv| * sin(angle between the span and the eye ray), so
		// the foreshortening term falls straight out of the cross product we
		// already needed. A camera-facing ribbon has NO valid orientation when
		// its axis points at the eye, and whichever fallback is chosen renders as
		// a long bright wedge converging on the vanishing point. Fading it is
		// both the fix and the physically right answer: a cylinder seen end-on
		// projects to a dot, not a blade. The x2 keeps the fade confined to spans
		// within ~30 degrees of the view axis, so an ordinary bolt fired forwards
		// still reads as a continuous channel.
		tvlen = (float)VectorLength(tv);
		// floored rather than taken to zero: the lightning gun is normally fired
		// straight down the view axis, so most of the channel is near-parallel to
		// the eye and a fade to nothing would erase the bolt in exactly the case
		// the player looks at most.
		endon = (tvlen > 1e-3f) ? 0.4f + 0.6f * min(1.0f, (sqrtf(l2) / tvlen) * 1.5f) : 0.4f;

		// too close to the eye to project safely - drop the node and restart the
		// ribbon after it, rather than bridging a triangle across the near plane
		if (tvlen < M5BOLT_NEARCLIP)
		{
			prev0 = prev1 = -1;
			if (i < n - 1)
				arc += (float)VectorDistance(pt[i], pt[i + 1]);
			continue;
		}

		if (l2 < 1e-6f)
		{
			// this span points straight at the eye - the case the stock beam
			// renderer collapses on silently
			CrossProduct(tang, eyeup, rightv);
			l2 = VectorLength2(rightv);
		}
		if (l2 < 1e-6f)
			CrossProduct(tang, eyeleft, rightv);
		if (VectorLength2(rightv) < 1e-6f)
		{
			// unreachable in practice - tang cannot be parallel to both view.up
			// and view.right - but it is a ribbon break like the near-clip above,
			// so it must restart the strip and keep the arc length walking, or it
			// would bridge across the gap and desync every S coordinate after it
			prev0 = prev1 = -1;
			if (i < n - 1)
				arc += (float)VectorDistance(pt[i], pt[i + 1]);
			continue;
		}
		VectorNormalize(rightv);
		CrossProduct(tang, rightv, nrm);

		// Widen the ribbon at a kink so the outer edge stays continuous. Bounded
		// tightly on purpose: mitering a WIDE ribbon around a sharp corner throws
		// a flare several times the beam's own width, which at a distance reads
		// as a bright shard stuck to the bolt. The soft beam texture hides the
		// small notch that under-mitering leaves far better than the eye forgives
		// that flare.
		miter = min(1.4f, 1.0f / max(0.5f, DotProduct(tang, dout)));

		frac = arc / total;
		halfwidth = (ln->width + (ln->widthtip - ln->width) * frac) * miter;
		// bound the SCREEN-space width near the camera - see M5BOLT_NEARWIDTH
		halfwidth = min(halfwidth, tvlen * M5BOLT_NEARWIDTH);
		alpha = (ln->alpha + (ln->alphatip - ln->alpha) * frac) * endon;
		// The terminal dissolve. A free tip that simply stops is a blunt quad end,
		// which reads as "drawn" rather than "discharged"; past taperstart the
		// width dies as the SQUARE of the remaining fraction while alpha dies
		// linearly, so the last stretch narrows to a hair BEFORE it fades instead
		// of fading as a stub of full width. taperstart 0 skips the branch
		// entirely, leaving the two lerps above byte-exactly as they were - which
		// is what the r_lightningbeam_m5_frizzle 0 no-op rests on.
		if (ln->taperstart > 0.0f && frac > ln->taperstart)
		{
			float tt = bound(0.0f, 1.0f - (frac - ln->taperstart) / (1.0f - ln->taperstart), 1.0f);
			halfwidth *= tt * tt;
			alpha *= tt;
		}
		s = t0 + ln->toffset + arc * sscale * ln->trepeat;

		// Roll the offset about the segment's own tangent. At roll 0 this is the
		// plain camera-facing card and bit-identical to before. The core's extra
		// passes sit at +/-45 degrees: each still projects to most of its width
		// but is displaced in DEPTH, so additively they build a cross-section
		// instead of a flat sheet. A 90-degree pass would be exactly edge-on and
		// invisible, which is why the tube is two passes and not one.
		if (ln->roll != 0.0f)
		{
			float cr = cosf(ln->roll), sr = sinf(ln->roll);
			VectorMAM(halfwidth * cr, rightv, halfwidth * sr, nrm, off);
		}
		else
			VectorM(halfwidth, rightv, off);
		VectorAdd(pt[i], off, va);
		VectorSubtract(pt[i], off, vb);
		e0 = Mod_Mesh_IndexForVertex(mod, surf, va[0], va[1], va[2], nrm[0], nrm[1], nrm[2], s, 0, 0, 0, colour[0], colour[1], colour[2], alpha);
		e1 = Mod_Mesh_IndexForVertex(mod, surf, vb[0], vb[1], vb[2], nrm[0], nrm[1], nrm[2], s, 1, 0, 0, colour[0], colour[1], colour[2], alpha);
		// Bridge to the previous rib only if there IS one. Testing `i > 0` here
		// instead was the "straight beam from off the map" defect: the skips above
		// set prev0/prev1 to -1 to mean "the ribbon restarts here", and nothing
		// read it, so the first node after any skip emitted triangles with vertex
		// index -1. Mod_Mesh_AddTriangle does not validate, -1 narrows to 65535 in
		// the unsigned short element array, and the out-of-range fetch reads as
		// position (0,0,0) - the map origin. One triangle came out a degenerate
		// sliver, the other a wide smooth wedge running from the world origin to
		// the full ribbon width at this node.
		if (prev0 >= 0)
		{
			Mod_Mesh_AddTriangle(mod, surf, prev0, prev1, e1);
			Mod_Mesh_AddTriangle(mod, surf, prev0, e1, e0);
		}
		prev0 = e0;
		prev1 = e1;
		if (i < n - 1)
			arc += (float)VectorDistance(pt[i], pt[i + 1]);
	}
}

// ---------------------------------------------------------------------------
// The SDF segment stream (F1, the star fix)
//
// Under r_lightningbeam_m5_sdf the bolt is not emitted into the scene mesh at
// all. Every polyline segment is appended here instead, and gl_rmain.c's
// R_LightningM5_RenderSDF expands each one into a camera-facing quad, evaluates
// a rounded-cap capsule in the fragment shader and accumulates the lot under
// max() -- which is what makes a joint stop being twice as bright as its limbs.
//
// The stream is stamped with the frame it was filled on and resets itself on the
// first append of a new frame, so nothing has to be cleared from the outside and
// a frame where the pass never runs cannot leak into the next one.

typedef struct m5sdfseg_s
{
	vec3_t a, b;			///< segment ends, world space
	float  ra, rb;			///< outer radius at each end, world units
	float  ca[3], cb[3];	///< colour x intensity at each end, still DIVIDED by the texture weighting
	float  bake;			///< 1 = the shader applies the textures' 1:2:4 weighting (and its clipping), 0 = neutral, as the QMB texture is
	float  boil;			///< a SHELL (bake 2) only: the silhouette boil amplitude the shader applies (0.16 gentle .. 0.42 violent); carried in CapsuleShape.z, which a shell never reads as a falloff
}
m5sdfseg_t;

static m5sdfseg_t *m5_sdfsegs = NULL;
static int m5_sdfnum = 0, m5_sdfmax = 0, m5_sdfframe = -1;
static qbool m5_sdflatch = false;		///< set by the renderer once both shaders have compiled

void CL_Beam_M5_SDFSetLatch(qbool ok) { m5_sdflatch = ok; }

qbool CL_Beam_M5_SDFActive(void)
{
	// A capability, never a renderpath identity test (the 6-4 spelling rule), and
	// never without the latch: a permutation-0 compile failure degrades SILENTLY
	// to program 0 (gl_rmain.c), so without it a broken shader would mean no bolt
	// at all rather than the old one. Stereo is refused outright -- the
	// colour-mask modes render the whole frame twice and a screen-wide
	// accumulate/composite pair has no colour mask of its own (the 8-5 lesson).
	return r_lightningbeam_m5_sdf.integer != 0 && vid.blendequationmax && m5_sdflatch
	    && !R_Stereo_Active();
}

const void *CL_Beam_M5_SDFSegments(int *count)
{
	// ONE frame of tolerance, not zero, and it is not slack: the relink that
	// fills this and the render that drains it are not guaranteed to fall in the
	// same host frame -- measured, they routinely do not -- so demanding equality
	// threw the whole stream away and drew no bolt at all. Anything older than
	// that is genuinely stale (the feature was switched off, or the client
	// stopped relinking) and must not be redrawn.
	if ((int)host.framecount - m5_sdfframe > 1)
		*count = 0;
	else
		*count = m5_sdfnum;
	return m5_sdfsegs;
}

int CL_Beam_M5_SDFFrame(void) { return m5_sdfframe; }

int CL_Beam_M5_SDFSegmentBytes(void) { return (int)sizeof(m5sdfseg_t); }

static m5sdfseg_t *CL_Beam_M5_SDFAppend(void)
{
	if (m5_sdfframe != (int)host.framecount)
	{
		m5_sdfframe = (int)host.framecount;
		m5_sdfnum = 0;
	}
	if (m5_sdfnum >= m5_sdfmax)
	{
		int want = m5_sdfmax ? m5_sdfmax * 2 : 1024;
		m5_sdfsegs = (m5sdfseg_t *)Mem_Realloc(r_main_mempool, m5_sdfsegs, want * sizeof(m5sdfseg_t));
		m5_sdfmax = want;
	}
	return &m5_sdfsegs[m5_sdfnum++];
}

/// Append one line's segments to the stream.
///
/// This is CL_Beam_M5_AddRibbon's per-node arithmetic minus everything the
/// capsule makes unnecessary. The width and alpha lerps, the terminal taper and
/// the near-clip skip are IDENTICAL and deliberately so -- they are the shape the
/// player already knows, and the whole point of this path is to draw that shape
/// without the joint artefact. What goes is the billboarding (the capsule is
/// camera-facing at expansion time, in the pass), the per-node basis, the miter
/// (nothing to miter: consecutive capsules overlap and max() resolves it), the
/// roll (the three rolled core passes exist ONLY to fake a tube under additive
/// blending, and a capsule already has a cross-section) and the end-on fade.
static void CL_Beam_M5_AddSDFSegments(const m5boltpath_t *p, const m5boltline_t *ln, const float *colour, float ascale)
{
	const vec3_t *pt = (const vec3_t *)(p->points + ln->first);
	int n = ln->count, i, c;
	float total = 0.0f, arc = 0.0f;
	if (n < 2)
		return;
	for (i = 0; i < n - 1; i++)
		total += (float)VectorDistance(pt[i], pt[i + 1]);
	if (total < 1e-4f)
		return;
	for (i = 0; i < n - 1; i++)
	{
		m5sdfseg_t *s;
		float seg = (float)VectorDistance(pt[i], pt[i + 1]);
		float fa = arc / total, fb = (arc + seg) / total;
		float wa, wb, aa, ab;
		arc += seg;
		if (seg < 1e-4f)
			continue;
		wa = ln->width + (ln->widthtip - ln->width) * fa;
		wb = ln->width + (ln->widthtip - ln->width) * fb;
		aa = (ln->alpha + (ln->alphatip - ln->alpha) * fa) * ascale;
		ab = (ln->alpha + (ln->alphatip - ln->alpha) * fb) * ascale;
		if (ln->taperstart > 0.0f)
		{
			float ta = bound(0.0f, 1.0f - (fa - ln->taperstart) / (1.0f - ln->taperstart), 1.0f);
			float tb = bound(0.0f, 1.0f - (fb - ln->taperstart) / (1.0f - ln->taperstart), 1.0f);
			if (fa > ln->taperstart) { wa *= ta * ta; aa *= ta; }
			if (fb > ln->taperstart) { wb *= tb * tb; ab *= tb; }
		}
		if (aa <= 0.001f && ab <= 0.001f)
			continue;
		s = CL_Beam_M5_SDFAppend();
		VectorCopy(pt[i], s->a);
		VectorCopy(pt[i + 1], s->b);
		s->ra = max(wa, 0.05f);
		s->rb = max(wb, 0.05f);
		s->bake = r_lightningbeam_qmbtexture.integer ? 0.0f : 1.0f;
		s->boil = 0.0f;		// a bolt segment: CapsuleShape.z is its falloff, written by the pass from its own cvar
		for (c = 0; c < 3; c++)
		{
			s->ca[c] = colour[c] * aa;
			s->cb[c] = colour[c] * ab;
		}
	}
}

void CL_Beam_M5_AddPolygons(const m5boltpath_t *p)
{
	model_t *mod;
	msurface_t *surf, *surfsheath;
	texture_t *stock, *coretex;
	const float *texw;
	float t0, sscale, whiteness, boost, core[3], sheath[3];
	int i, c;

	if (r_lightningbeam_qmbtexture.integer && cl_beams_externaltexture.currentskinframe == NULL)
		CL_Beams_SetupExternalTexture();
	if (!r_lightningbeam_qmbtexture.integer && cl_beams_builtintexture.currentskinframe == NULL)
		CL_Beams_SetupBuiltinTexture();
	if (r_lightningbeam_m5_coretexture.integer && cl_beams_m5coretexture.currentskinframe == NULL)
		CL_Beams_SetupM5CoreTexture();

	// same scroll and repeat meaning as the stock beam, only measured along the
	// polyline rather than the straight line (so the texture is a little denser,
	// in proportion to how far the bolt actually wanders)
	t0 = r_refdef.scene.time * -r_lightningbeam_scroll.value;
	t0 = t0 - (int)t0;
	sscale = 1.0f / max(1.0f, r_lightningbeam_repeatdistance.value);

	// The vertex colour that PRODUCES a wanted rendered colour is that colour
	// divided by the texture's baked weighting, so the 1:2:4 bake is cancelled
	// rather than assumed. At whiteness 0 the core renders as the player's own
	// hue; at 1 it is genuinely white, whatever the texture bakes.
	texw = r_lightningbeam_qmbtexture.integer ? m5_texweight_qmb : m5_texweight_builtin;
	whiteness = bound(0.0f, r_lightningbeam_m5_whiteness.value, 1.0f);
	// hue's max channel is 1 normally, and 0 only when the player set the beam
	// colour to black. Whitening that would turn "no beam" into a grey one.
	if (max(p->hue[0], max(p->hue[1], p->hue[2])) < 1e-6f)
		whiteness = 0.0f;
	// The connect boost scales the COLOUR, not the alpha: alpha feeds GL_SRC_ALPHA,
	// which is clamped to [0,1] as a blend factor, so pushing it past 1 would do
	// nothing. Vertex colour is a float attribute and survives.
	boost = 1.0f + (bound(1.0f, r_lightningbeam_m5_hitboost.value, 3.0f) - 1.0f) * p->hit;
	for (c = 0; c < 3; c++)
	{
		sheath[c] = p->hue[c] * boost / texw[c];
		core[c] = (p->hue[c] + (1.0f - p->hue[c]) * whiteness) * boost / texw[c];
	}

	// One surface for the whole bolt. The material is additive, so the lines are
	// order-independent and share it happily; keeping one surface PER BEAM (not
	// batching across beams) preserves the stock depth-sort behaviour, since the
	// sort key is the surface bounding box.
	// Two surfaces, because the core and the sheath now want different textures:
	// the sheath keeps the stock beam texture, whose wandering centre is doing
	// useful work there, while the core gets one that is actually lit across its
	// width. Both are additive, so ordering between them is still free, and this
	// is still one pair of surfaces PER BEAM, which is what the depth sort keys
	// on. The whiten flag selects both the colour and the texture - the core
	// lines are exactly the ones that want each.
	// THE SDF FORK. When the renderer can accumulate under max() the bolt goes to
	// the segment stream instead of the scene mesh and is drawn by its own pass
	// later in the frame; see CL_Beam_M5_SDFActive. It must not go to both: the
	// scene-mesh route is auto-queued into the transparent pass, which is exactly
	// what the SDF pass replaces.
	if (CL_Beam_M5_SDFActive())
	{
		// THE SAME VERTEX COLOUR THE RIBBON USES, scaled and nothing else -- and
		// that is a measured result, not an assumption. The obvious-looking move
		// is to multiply the 1:2:4 texture-weight cancellation back out, since
		// the capsule samples no texture to cancel it: measured, that renders
		// COOL (unsaturated band mean RGB 95/97/111) where the ribbon renders
		// WARM (126/104/73). The reason is that the beam texture's own bake
		// CLIPS -- blue saturates above a quarter intensity and green above a
		// half -- so across the bright body of the beam the texture is
		// effectively neutral and what reaches the screen is the pre-divided
		// vertex colour itself. Leave the division in place and the palette
		// matches by construction.
		// ENERGY RE-DERIVATION, and this is the part that cannot be skipped.
		// The core is drawn as SEVERAL passes rolled about the beam's own axis
		// for one reason only: additive blending makes them build a cross-section
		// where a single flat card would read as a sheet. A capsule already HAS a
		// cross-section, and under max() the passes do not add -- they simply
		// overlap and the brightest wins. Emitted as they stand, each carrying its
		// share vol/npass of the budget, the CORE ends up dimmer than the sheath
		// and the sheath wins the centre of the beam: measured, that renders the
		// raw hue at 175/97/55 where the ribbon renders a near-white 147/136/92,
		// which reads as an orange bar instead of a hot white channel.
		//
		// So the rolled siblings are dropped and their budget handed back to the
		// one un-rolled core pass. BuildPath emits the core first, so line 0 is
		// that pass -- an ordering this depends on and which the loop below
		// documents where it is used.
		float coresdf[3], sheathsdf[3];
		float gain = max(0.0f, r_lightningbeam_m5_sdf_gain.value);
		int tubepasses = 0;
		for (i = 0; i < p->numlines; i++)
			if (p->lines[i].roll != 0.0f)
				tubepasses++;
		// THE INTENDED colour, with the texture-weight cancellation multiplied
		// back out -- and the shader then re-applies that weighting WITH ITS
		// CLIPPING, which is the whole trick and the thing three attempts got
		// wrong. The pair is not a no-op, because min() sits between them: at the
		// centre of the profile the weighting saturates to (1,1,1) and the colour
		// arrives untouched, near-white; out at the fringe nothing clips and the
		// full 1:2:4 lands, pulling the edge hard toward blue. White-hot core,
		// electric-blue edge, from one multiply -- which is exactly the structure
		// the ribbon gets from its texture, and exactly what was missing.
		for (c = 0; c < 3; c++)
		{
			coresdf[c] = core[c] * texw[c] * gain;
			sheathsdf[c] = sheath[c] * texw[c] * gain;
		}
		// KEEP THE CORE AND THE SHEATH APART. They are what the bolt's colour IS:
		// the core is whitened toward the hot centre while the sheath carries the
		// player's own hue, and at a blue-heavy setting those are very different
		// colours -- a white-hot thread inside a wide violet glow. Folding them
		// into one capsule was tried and is WRONG: it renders the whole channel in
		// the core's colour and throws the violet away, which is exactly the
		// cream-coloured rope Seb's QA rejected. It looked defensible only because
		// it was measured against the DEFAULT palette, where core and sheath are
		// both warm and the loss is invisible.
		//
		// The rolled tube passes still go: they exist only to fake a tube under
		// additive blending, a capsule has a cross-section already, and left in
		// they merely split the core's energy three ways so the sheath wins the
		// centre. Their budget goes back to the one un-rolled core pass, which is
		// line 0 -- BuildPath emits the core first.
		for (i = 0; i < p->numlines; i++)
		{
			const m5boltline_t *ln2 = &p->lines[i];
			if (ln2->roll != 0.0f)
				continue;
			CL_Beam_M5_AddSDFSegments(p, ln2, ln2->whiten ? coresdf : sheathsdf,
				(i == 0) ? (float)(tubepasses + 1) : 1.0f);
		}
		return;
	}

	mod = CL_Mesh_Scene();
	stock = r_lightningbeam_qmbtexture.integer ? &cl_beams_externaltexture : &cl_beams_builtintexture;
	coretex = (r_lightningbeam_m5_coretexture.integer && cl_beams_m5coretexture.currentskinframe) ? &cl_beams_m5coretexture : stock;
	surf = Mod_Mesh_AddSurface(mod, coretex, false);
	surfsheath = (coretex == stock) ? surf : Mod_Mesh_AddSurface(mod, stock, false);
	for (i = 0; i < p->numlines; i++)
		CL_Beam_M5_AddRibbon(mod, p->lines[i].whiten ? surf : surfsheath, p, &p->lines[i],
			p->lines[i].whiten ? core : sheath, t0, sscale);

	// A bolt fired point-blank can have every node inside the near clip, leaving
	// this surface with no triangles and the all-zero mins/maxs it was memset to.
	// That is a bounding box at the world origin entering the transparent sort, so
	// drop the surface again - it is the last one added, so this is just the
	// counter. Any vertices it emitted without completing a strip are left
	// unreferenced, which costs nothing: the scene mesh is reset every frame.
	// drop either surface if it ended up with nothing in it (a bolt fired
	// point-blank can have every node inside the near clip). Reverse order,
	// because only the LAST surface added can be popped by the counter.
	if (surfsheath != surf && surfsheath->num_triangles == 0)
		mod->num_surfaces--;
	if (surf->num_triangles == 0 && (surfsheath == surf || surfsheath->num_triangles == 0))
		mod->num_surfaces--;
}

/// Push one frame-temporary light, the same hook cl_beams_lightatend has always
/// used: an rtlight written straight into r_refdef.scene.templights and pushed
/// onto the scene list, rebuilt from scratch every frame with no decay
/// bookkeeping. The bolt owns its own brightness through the flicker envelope.
///
/// `corona 0`, `coronasizescale 0` and `flags 0` are all load-bearing.
/// R_Shadow_DrawCoronas walks r_refdef.scene.lights and draws an additive screen
/// blob for EVERY light with corona > 0 - plus an occlusion query or traceline
/// each. With gl_flashblend 1 and r_coronas 1 a chain of eight bolt lights would
/// be a chain of eight blobs, which is precisely the white-out this feature must
/// not cause. `flags = 0` is the second, config-independent layer: that loop
/// skips any light whose flags miss the current lighting mode, and rtlight->flags
/// is read nowhere else for scene lights, so zero is safe everywhere.
static void CL_Beam_M5_PushLight(const vec3_t org, const vec3_t colour, float radius, float fogweight)
{
	matrix4x4_t m;
	vec3_t c;
	// keep headroom so a bolt can never starve the explosion and muzzle flashes
	// that CL_RelinkLightFlashes pushes after us
	if (r_refdef.scene.numlights >= MAX_DLIGHTS - 32)
		return;
	VectorCopy(colour, c);
	Matrix4x4_CreateFromQuakeEntity(&m, org[0], org[1], org[2], 0, 0, 0, radius);
	R_RTLight_Update(&r_refdef.scene.templights[r_refdef.scene.numlights], false, &m, c,
		-1, NULL, false, 0, 0, 1, 0, 0, 0);
	// AFTER the update, which memsets the whole rtlight. This is the only
	// producer in the tree that sets a weight other than 1; see RT_LIGHT_STRIDE.
	r_refdef.scene.templights[r_refdef.scene.numlights].m5fogweight = fogweight;
	r_refdef.scene.lights[r_refdef.scene.numlights] = &r_refdef.scene.templights[r_refdef.scene.numlights];
	r_refdef.scene.numlights++;
}

void CL_Beam_M5_AddLights(const m5boltpath_t *p)
{
	const vec3_t *pt = (const vec3_t *)(p->points + p->channelfirst);
	int n = p->channelcount, count, i, li;
	float total = 0.0f, spacing, acc, next, radius, peak, overlapnorm, white, boost, fogw;
	vec3_t colour, org, dir;

	if (r_lightningbeam_m5_light.value <= 0.0f || n < 2)
		return;

	for (i = 1; i < n; i++)
		total += (float)VectorDistance(pt[i - 1], pt[i]);
	if (total < 1.0f)
		return;

	count = bound(2, r_lightningbeam_m5_lightcount.integer, 16);
	radius = bound(32.0f, r_lightningbeam_m5_lightradius.value, 384.0f);
	white = bound(0.0f, r_lightningbeam_m5_lightwhite.value, 1.0f);
	boost = 1.0f + (bound(1.0f, r_lightningbeam_m5_hitboost.value, 3.0f) - 1.0f) * p->hit;
	spacing = total / (float)count;

	// The sum of (1 - d/R)^2 along an evenly spaced chain of point lights is
	// about 2R/3s, so dividing it out makes _light an effective PEAK knob:
	// _lightcount and _lightradius then redistribute the light without changing
	// how bright the room actually gets. Many small lights are also strictly
	// safer than one big one for the fog kernel, whose dominant-light eviction
	// footprint is exactly the light's radius.
	overlapnorm = max(1.0f, 2.0f * radius / (3.0f * spacing));

	// How loudly the bolt argues in the FOG, independent of what it puts on
	// walls: the fog kernel scales both a light's scattered colour and its say in
	// the dominant-light vote by this, and the surface kernel never reads it.
	// That vote is why the knob is worth having - a bolt that wins it everywhere
	// evicts nearby torches onto the unshadowed residual fill, and their warmth
	// drains out of the fog. Below 1 the bolt yields to a close torch without
	// getting any dimmer on the walls.
	// 0 is the "unset, treat as 1.0" sentinel in the buffer, so a genuine zero
	// is carried as an epsilon instead.
	fogw = bound(0.0f, r_lightningbeam_m5_fog.value, 4.0f);
	if (fogw <= 0.0f)
		fogw = 1e-6f;

	peak = M5_LIGHT_BASE * bound(0.0f, r_lightningbeam_m5_light.value, 3.0f)
	     * p->envelope * boost / overlapnorm;

	// Desaturate toward white a little. Without it a saturated blue bolt casts
	// roughly a third the luminance of a white one at the same setting, and the
	// brightness knob stops meaning the same thing across hue choices. hue itself
	// is the colour the player SEES (the cvars times the texture's baked
	// weighting, normalised) - see the lockstep note on m5_texweight_builtin.
	for (i = 0; i < 3; i++)
		colour[i] = (p->hue[i] + (1.0f - p->hue[i]) * white) * peak;

	// walk the channel by arc length, dropping a light at each interval's centre
	acc = 0.0f;
	next = 0.5f * spacing;
	li = 0;
	for (i = 0; i + 1 < n && li < count; i++)
	{
		float seg = (float)VectorDistance(pt[i], pt[i + 1]);
		while (li < count && acc + seg >= next)
		{
			float f = (seg > 1e-6f) ? (next - acc) / seg : 0.0f;
			VectorLerp(pt[i], f, pt[i + 1], org);
			CL_Beam_M5_PushLight(org, colour, radius, fogw);
			li++;
			next += spacing;
		}
		acc += seg;
	}

	// The impact flash. In the reference images the splash where the bolt lands
	// is what sells "this is really lighting the room".
	if (r_lightningbeam_m5_impact.value > 0.0f)
	{
		vec3_t icol;
		VectorSubtract(pt[n - 1], pt[n - 2], dir);
		if (VectorLength2(dir) > 1e-6f)
		{
			VectorNormalize(dir);
			VectorMA(pt[n - 1], -M5_IMPACT_PULLBACK, dir, org);
		}
		else
			VectorCopy(pt[n - 1], org);
		VectorScale(colour, bound(0.0f, r_lightningbeam_m5_impact.value, 4.0f)
		                  * (1.0f + 0.75f * (boost - 1.0f)), icol);
		CL_Beam_M5_PushLight(org, icol, radius * 1.5f * (1.0f + 0.25f * (boost - 1.0f)), fogw);
	}
}

/*
==============================================================================

M5 BALL LIGHTNING -- the plasma knot (BALLLIGHTNING.md slice 3)

The ball is not a model. It is a SECOND PRODUCER for the bolt renderer: a
tangle of short M5 strands re-rolled at the bolt's own rate inside a sphere of
r_lightningbeam_m5_ballsize, plus one wide faint capsule through the centre as
a halo, all handed to CL_Beam_M5_AddPolygons exactly as a beam's path is -- so
it takes the SDF pass (max-accumulated capsules) on Metal and the additive
ribbon on GL, the core/sheath colours, the whiteness, the fizz and the
thickness knobs, with no second look to keep in step.

Seeded like the bolt (CL_Beam_M5_Seed from the entity number and the roll
index, never the frame), so the knot is bit-identical at any framerate inside
one roll interval and a timedemo replay of it is byte-stable per binary. The
hue block is LOCKSTEP with CL_Beam_M5_BuildPath's; it is copied rather than
shared so the bolt's own path stays textually untouched.

==============================================================================
*/

/// The ball's flicker, for the light the client hangs on it (cl_main.c). Same
/// envelope as the bolt's sheath, phased per entity so two balls never breathe
/// in step.
float CL_Beam_M5_BallEnvelope(int entity)
{
	return CL_Beam_M5_Envelope(cl.time,
		CL_Beam_M5_Phase(entity, 0, 0x1b873593u) * (float)(2.0 * M_PI),
		CL_Beam_M5_Phase(entity, 0, 0xcc9e2d51u) * (float)(2.0 * M_PI));
}

/// The ball's palette: r_lightningbeam_m5_ballcolor_*, max-normalised, so the
/// brightness knobs mean the same thing whatever hue Seb picks. Violet, magenta
/// and electric blue are all derived from this one colour (the shell's boil
/// mixes toward magenta, its rim toward electric blue).
static void CL_Beam_M5_BallHue(vec3_t hue)
{
	float m;
	hue[0] = max(0.0f, r_lightningbeam_m5_ballcolor_red.value);
	hue[1] = max(0.0f, r_lightningbeam_m5_ballcolor_green.value);
	hue[2] = max(0.0f, r_lightningbeam_m5_ballcolor_blue.value);
	m = max(hue[0], max(hue[1], hue[2]));
	if (m < 1e-6f)
	{
		VectorSet(hue, 0.45f, 0.28f, 1.0f);
		return;
	}
	VectorScale(hue, 1.0f / m, hue);
}

/// The ball's STATE, sent by the QuakeC in the entity's frame: 0 idle, 1
/// FIZZLING (it hit a wall: no burst, it crackles and strobes out over the
/// alpha, which counts down 1 -> 0), 2 ZAPPING (arcs landing this tick: a
/// brighter core and one or two THICK filaments, like a hand on the glass).
#define M5BALL_IDLE		0
#define M5BALL_FIZZLE	1
#define M5BALL_ZAP		2
#define M5BALL_CHARGE	3		// building at the muzzle: alpha is the charge progress 0 -> 1
#define M5BALL_SHATTER	4		// a direct hit: the shell blows apart over the alpha countdown 1 -> 0


/// The ball's light colour: the bolt hue, desaturated by r_lightningbeam_m5_lightwhite
/// exactly as the bolt's own lights are, times the brightness the QuakeC sent in
/// the entity's max colour channel, times this frame's envelope -- and the
/// state: brighter while zapping, crackling out while fizzling.
void CL_Beam_M5_BallLight(int entity, float brightness, int frame, float alpha, vec3_t out)
{
	vec3_t hue;
	float white = bound(0.0f, r_lightningbeam_m5_lightwhite.value, 1.0f);
	float env = CL_Beam_M5_BallEnvelope(entity);
	int c;
	if (frame == M5BALL_FIZZLE)
		env = 1.8f * bound(0.0f, alpha, 1.0f) * bound(0.0f, alpha, 1.0f);		// earthing: a flash, then gone
	else if (frame == M5BALL_ZAP)
		env = min(1.0f, env * (1.5f + 0.4f * sinf((float)(cl.time * 38.0))));	// a hard pulse while it feeds
	else if (frame == M5BALL_CHARGE)
		env *= bound(0.0f, alpha, 1.0f) * bound(0.0f, alpha, 1.0f);
	else if (frame == M5BALL_SHATTER)
		env = 2.5f * bound(0.0f, alpha, 1.0f);		// a flash, dying with the countdown
	CL_Beam_M5_BallHue(hue);
	for (c = 0; c < 3; c++)
		out[c] = (hue[c] + (1.0f - hue[c]) * white) * brightness * env;
}

// LOCKSTEP with m5_boltrand / m5_boltnoise in shader_msl.h's MODE_M5BOLT arm --
// the ball's shell boils its silhouette with this noise of the angle about the
// view axis, and the CPU needs the SAME curve to put volcanic spikes where the
// bulges are (Seb's sixth look).
static float CL_Beam_M5_BoltRand(unsigned int n)
{
	n = (n << 13u) ^ n;
	return 1.0f - (float)((n * (n * n * 15731u + 789221u) + 1376312589u) & 0x7fffffffu) / 1073741824.0f;
}
static float CL_Beam_M5_BoltNoise(float x)
{
	float i = floorf(x), f = x - i;
	unsigned int k = (unsigned int)((int)i + 65536);
	f = f * f * (3.0f - 2.0f * f);
	return CL_Beam_M5_BoltRand(k) + (CL_Beam_M5_BoltRand(k + 1u) - CL_Beam_M5_BoltRand(k)) * f;
}
/// The shell's silhouette bulge at angle `ang` about the view axis, in [-1, 1].
/// LOCKSTEP with the `bulge` lines of the shell branch in shader_msl.h -- including
/// the VIOLENCE: the boil runs 1 + 1.2 x vio faster as the ball flies (vio 0..1,
/// which the shader derives back from the boil amplitude in CapsuleShape.z).
static float CL_Beam_M5_ShellBulge(float ang, float t, float vio)
{
	float b;
	t *= 1.0f + 1.2f * vio;
	b = CL_Beam_M5_BoltNoise(ang * 1.4f + t * 3.1f) + 0.6f * CL_Beam_M5_BoltNoise(ang * 3.3f - t * 4.7f + 5.0f);
	return bound(-1.0f, b * (1.0f / 1.6f), 1.0f);
}

static qbool m5_ballreported;

/// SCREEN PRESSURE: the strongest "how close is a ball to my eye" this frame,
/// 0..1, stamped with the frame it was measured on so a frame with no ball
/// reads 0 without anything clearing it. The postprocess paints it in from the
/// edges (gl_rmain.c R_BlendView, BallPressure).
static float m5_ballpressure;
static int m5_ballpressureframe = -1;

float CL_Beam_M5_BallPressure(void)
{
	if ((int)host.framecount - m5_ballpressureframe > 1)
		return 0.0f;
	return m5_ballpressure * bound(0.0f, r_lightningbeam_m5_ballpressure.value, 2.0f);
}

void CL_Beam_M5_AddBall(int entity, const vec3_t origin, int frame, float alpha)
{
	static m5boltpath_t path, fil;	// ~12 KB each; one at a time, never re-entered
	randomseed_t seed;
	m5boltline_t *ln;
	float R = bound(4.0f, r_lightningbeam_m5_ballsize.value, 96.0f);
	float rate = bound(2.0f, r_lightningbeam_m5_rate.value, 30.0f);
	float phase = CL_Beam_M5_Phase(entity, 0, 0x9e3779b9u);
	int roll = (int)floor(cl.time * rate + phase);
	float thickness = max(0.1f, r_lightningbeam_thickness.value);
	float corew = thickness * bound(0.1f, r_lightningbeam_m5_corewidth.value, 1.0f) * 0.8f;
	float sheathw = thickness * bound(0.5f, r_lightningbeam_m5_sheathwidth.value, 4.0f);
	float sheatha = bound(0.0f, r_lightningbeam_m5_sheathalpha.value, 1.0f);
	const int nodes = 9;			// enough for a curl to read as a curve
	int strands = 4, st, i, first;
	float level = 1.0f;				// brightness of everything, the state's
	float fwid = 1.0f, famp = 0.4f, falpha = 1.0f;	// filament width, curl amplitude and alpha multipliers
	float corescale = 1.0f;
	float vio;						// VIOLENCE 0..1: how far into its flight the ball is (Seb, 2026-09-10)

	// the scene mesh indexes vertices as unsigned short -- the same refusal the
	// bolt makes, for the same reason (a silent wrap corrupts every effect
	// sharing the mesh)
	if (CL_Mesh_Scene()->surfmesh.num_vertices > 60000)
		return;

	// the screen pressure: how close this ball is to the eye, against one and a
	// half of the game's reach (m5_balllightning_radius is CF_CLIENT|CF_SERVER,
	// so it is readable here in a local game); the frame keeps the strongest
	if (frame == M5BALL_IDLE || frame == M5BALL_ZAP)
	{
		float reach = 1.6f * max(32.0f, Cvar_VariableValue(&cvars_all, "m5_balllightning_radius", CF_CLIENT | CF_SERVER));
		float pr = 1.0f - (float)VectorDistance(origin, r_refdef.view.origin) / reach;
		if (m5_ballpressureframe != (int)host.framecount)
		{
			m5_ballpressureframe = (int)host.framecount;
			m5_ballpressure = 0.0f;
		}
		if (pr > m5_ballpressure)
			m5_ballpressure = bound(0.0f, pr, 1.0f);
	}

	// THE BALL'S AGE, timed here from the frame this renderer first saw the entity
	// (nothing in the protocol carries it): m5_balllightning_violence seconds of
	// flight take the shell from the gentle bubble it leaves the gun as to full
	// violence -- the silhouette heaving harder and faster, the filaments lifting
	// off it, the eruptions longer and more frequent (Seb, 2026-09-10: "more
	// violent in its surface perturbation as it flies"). Sixteen slots keyed by
	// entity number; a slot not refreshed for a second is stale (the number was
	// reused by a later ball) and restarts the clock.
	{
		static struct { int entity; double seen, last; } age[16];
		int k, slot = -1, oldest = 0;
		double a;
		for (k = 0; k < 16; k++)
		{
			if (age[k].entity == entity && cl.time - age[k].last < 1.0)
			{
				slot = k;
				break;
			}
			if (age[k].last < age[oldest].last)
				oldest = k;
		}
		if (slot < 0)
		{
			slot = oldest;
			age[slot].entity = entity;
			age[slot].seen = cl.time;
		}
		age[slot].last = cl.time;
		a = cl.time - age[slot].seen;
		if (m5_balllightning_violence.value <= 0.0f)
			vio = 1.0f;
		else
			vio = (float)bound(0.0, a / m5_balllightning_violence.value, 1.0);
		if (frame == M5BALL_CHARGE)
			vio = 0.0f;
		else if (frame == M5BALL_FIZZLE || frame == M5BALL_SHATTER)
			vio = 1.0f;
	}

	if (frame == M5BALL_IDLE || frame == M5BALL_ZAP)
	{
		// UNSTABLE: a mild random brightness per half-roll, so the whole ball
		// flickers as if it cannot quite hold itself together (Seb's fifth look)
		randomseed_t fs;
		CL_Beam_M5_Seed(entity, 4, (int)floor(cl.time * rate * 2.0 + phase), &fs);
		level = 0.8f + 0.4f * Math_randomf(&fs);
	}
	if (frame == M5BALL_FIZZLE)
	{
		// EARTHING on a wall (Seb's tenth look: no flicker out -- it expends its
		// energy in a final arc, thrown by the QuakeC): the shell COLLAPSES over
		// the countdown, bright first, while its wisps go with it
		float g = bound(0.0f, alpha, 1.0f);
		R *= 0.3f + 0.7f * g;
		level = 1.4f * g;
		strands = 2;
		famp = 0.5f;
		corescale = 1.3f;
	}
	else if (frame == M5BALL_ZAP)
	{
		// a hand on the glass: the core flares and the charge goes into one or
		// two thick filaments instead of the dancing many
		strands = 2;
		fwid = 2.6f;
		famp = 0.25f;
		falpha = 1.3f;
		corescale = 1.3f;
	}
	else if (frame == M5BALL_CHARGE)
	{
		// building at the muzzle (the BFG's wind-up made visible): a SMALL globe
		// -- a third of the size, the ball grows to full size once it has flown
		// a few feet (Seb: it read too big forming next to the eye) -- from
		// nothing with the charge, few filaments, dim
		float g = bound(0.0f, alpha, 1.0f);
		R *= 0.25f * (0.15f + 0.85f * g);		// ends where the launched ball starts (a quarter, the QuakeC's alpha 0.22)
		level = 0.2f + 0.8f * g * g;
		strands = 2;
		famp = 0.3f;
	}
	if ((frame == M5BALL_IDLE || frame == M5BALL_ZAP) && alpha > 0.0f && alpha < 1.0f)
		R *= alpha;		// the QuakeC ramps alpha 0.22 -> 1 over the first few feet of flight: a bubble at the muzzle, a globe in the room (Seb, 2026-09-10)
	if (frame == M5BALL_IDLE)
		fwid *= 1.0f + 0.5f * vio;		// the surface filaments thicken as the ball grows violent
	if (frame == M5BALL_SHATTER)
	{
		// DISINTEGRATION (Seb's ninth look): the shell swells to twice its size
		// and thins to nothing over the countdown while every tendril erupts at
		// full reach -- the globe blowing itself apart on whatever it hit
		float g = 1.0f - bound(0.0f, alpha, 1.0f);
		R *= 1.0f + 1.4f * g;
		level = bound(0.0f, alpha, 1.0f) * 1.6f;
		strands = 0;			// no surface wisps; the eruptions carry it
		famp = 0.5f;
	}

	CL_Beam_M5_Seed(entity, 0, roll, &seed);
	path.numpoints = 0;
	path.numlines = 0;
	path.numfrizzlines = 0;
	path.channelfirst = 0;
	path.channelcount = 0;
	path.length = 2.0f * R;
	path.hit = 0.0f;
	path.envelope = CL_Beam_M5_BallEnvelope(entity);
	CL_Beam_M5_BallHue(path.hue);
	// the filaments get their own path so they can carry their own hue: a line
	// has no colour of its own (the path does), and Seb wants violet filaments
	// round a blue-white core
	fil = path;			// a separate path so the filaments carry their own, magenta-shifted hue
	fil.hue[0] = 0.6f + 0.4f * path.hue[0];
	fil.hue[1] = 0.3f * path.hue[1];
	fil.hue[2] = 0.85f * path.hue[2] + 0.1f;

	if (CL_Beam_M5_SDFActive())
	{
		// THE SHELL (Metal only, by Seb's choice): one zero-length capsule flagged
		// bake 2 -- the pass expands it as a camera-facing sphere and the shader
		// shades it as a dark sun boiling with plasma, rim-lit, and OCCLUDING
		// what lies behind it through the accumulation buffer's alpha. Colour is
		// the palette times the state's brightness; the shader takes the rest.
		m5sdfseg_t *sg = CL_Beam_M5_SDFAppend();
		float lvl = level * (frame == M5BALL_ZAP ? 1.3f : 1.0f);
		VectorCopy(origin, sg->a);
		VectorCopy(origin, sg->b);
		sg->ra = sg->rb = R;
		VectorScale(path.hue, lvl, sg->ca);
		VectorCopy(sg->ca, sg->cb);
		sg->bake = 2.0f;
		sg->boil = 0.16f + 0.26f * vio;		// LOCKSTEP with the shell branch's boil/vio lines in shader_msl.h
	}
	else
	{
	// THE ELECTRODE (ribbon path only -- the SDF path draws the shell instead):
	// a small hot sphere at the centre, a bright dot on the ribbon
	first = path.numpoints;
	VectorSet(path.points[first], origin[0], origin[1], origin[2] - 0.06f * R);
	VectorSet(path.points[first + 1], origin[0], origin[1], origin[2] + 0.06f * R);
	path.numpoints += 2;
	ln = &path.lines[path.numlines++];
	ln->first = first; ln->count = 2;
	ln->width = ln->widthtip = R * 0.2f * corescale;
	ln->alpha = ln->alphatip = 0.8f * level * (frame == M5BALL_ZAP ? 1.25f : 1.0f);
	ln->toffset = 0.5f; ln->trepeat = 1.0f;
	ln->roll = 0.0f;
	ln->whiten = 1;
	ln->taperstart = 0.0f;

	// THE GLASS: a faint wide capsule through the centre, so the filaments sit in
	// a just-visible glow. Faint on purpose -- at 0.10 this was a white blob.
	first = path.numpoints;
	VectorSet(path.points[first], origin[0], origin[1], origin[2] - 0.2f * R);
	VectorSet(path.points[first + 1], origin[0], origin[1], origin[2] + 0.2f * R);
	path.numpoints += 2;
	ln = &path.lines[path.numlines++];
	ln->first = first; ln->count = 2;
	ln->width = ln->widthtip = R * 0.9f;
	ln->alpha = ln->alphatip = 0.035f * (0.5f + 0.5f * path.envelope) * level;
	ln->toffset = 0.5f; ln->trepeat = 0.3f;
	ln->roll = 0.0f;
	ln->whiten = 0;
	ln->taperstart = 0.0f;
	}

	// THE FILAMENTS (Seb's fourth look): fewer, softer, magenta, and they PLAY ON
	// THE SURFACE rather than emit from the core -- each is a great-circle arc
	// just above the shell between two random points on it, curling a little off
	// the surface in the middle, wide-sheathed and faint. While ZAPPING they are
	// the two thick discharges reaching outward instead.
	for (st = 0; st < strands && fil.numlines + 2 <= M5BOLT_MAXLINES
	     && fil.numpoints + nodes <= M5BOLT_MAXPOINTS; st++)
	{
		vec3_t a, b, axis, u, v;
		float len, amp, twist, phi, lift;
		int tries;
		if (frame == M5BALL_ZAP)
		{
			// outward: from the shell to well outside it
			for (tries = 0; tries < 8; tries++)
			{
				VectorLehmerRandom(&seed, axis);
				if (VectorLength2(axis) > 0.04f)
					break;
			}
			if (VectorLength2(axis) <= 0.04f)
				continue;
			VectorNormalize(axis);
			VectorMA(origin, 0.95f * R, axis, a);
			VectorMA(origin, R * (1.6f + 0.5f * Math_randomf(&seed)), axis, b);
			lift = 0.0f;
		}
		else
		{
			// on the surface: two points on the shell 60-130 degrees apart, the
			// chord between them, lifted onto the sphere below
			vec3_t da, db;
			for (tries = 0; tries < 8; tries++)
			{
				VectorLehmerRandom(&seed, da);
				VectorLehmerRandom(&seed, db);
				if (VectorLength2(da) < 0.04f || VectorLength2(db) < 0.04f)
					continue;
				VectorNormalize(da);
				VectorNormalize(db);
				if (DotProduct(da, db) < 0.5f && DotProduct(da, db) > -0.65f)
					break;
			}
			VectorMA(origin, R, da, a);
			VectorMA(origin, R, db, b);
			lift = 1.0f;
		}
		VectorSubtract(b, a, axis);
		len = (float)VectorLength(axis);
		if (len < 1.0f)
			continue;
		VectorScale(axis, 1.0f / len, axis);
		CL_Beam_M5_Basis(axis, &seed, u, v);
		amp = famp * R * (0.4f + 0.6f * Math_randomf(&seed));
		twist = (1.0f + 2.0f * Math_randomf(&seed)) * (Math_randomf(&seed) < 0.5f ? -1.0f : 1.0f);
		phi = Math_randomf(&seed) * (float)(2.0 * M_PI);
		first = fil.numpoints;
		for (i = 0; i < nodes; i++)
		{
			float t = (float)i / (float)(nodes - 1);
			float sw = sinf((float)M_PI * t);				// closed at both ends
			float ang = phi + twist * t;
			vec3_t pt, rel;
			VectorMA(a, len * t, axis, pt);
			VectorMA(pt, amp * sw * cosf(ang) * 0.35f, u, pt);
			VectorMA(pt, amp * sw * sinf(ang) * 0.35f, v, pt);
			if (lift > 0.0f)
			{
				// push the chord out onto the shell (just above it), so the wisp
				// rides the surface instead of cutting through the dark interior
				VectorSubtract(pt, origin, rel);
				VectorNormalize(rel);
				// and LIFT it: at full violence the wisp is a prominence arching
				// well off the shell rather than a thread on it (Seb, 2026-09-10:
				// "erupting as an extension of the ball, not merely stuck on")
				VectorMA(origin, R * (1.03f + (0.08f + 0.55f * vio) * sw), rel, pt);
			}
			VectorCopy(pt, fil.points[first + i]);
		}
		fil.numpoints += nodes;

		// the thread: a fine magenta core, faint
		ln = &fil.lines[fil.numlines++];
		ln->first = first; ln->count = nodes;
		ln->width = ln->widthtip = corew * 0.35f * fwid;
		ln->alpha = ln->alphatip = min(1.0f, (lift > 0.0f ? 0.45f + 0.35f * vio : 0.9f) * falpha) * level;
		ln->toffset = 0.17f * (float)st; ln->trepeat = 1.0f;
		ln->roll = 0.0f;
		ln->whiten = 0;
		ln->taperstart = lift > 0.0f ? 0.0f : 0.7f;
		// its glow: wide, soft, faint
		ln = &fil.lines[fil.numlines++];
		ln->first = first; ln->count = nodes;
		ln->width = ln->widthtip = sheathw * (lift > 0.0f ? 1.3f : 0.6f) * fwid;
		ln->alpha = ln->alphatip = sheatha * (lift > 0.0f ? 0.22f : 0.4f) * falpha * (0.5f + 0.5f * path.envelope) * level;
		ln->toffset = 0.37f + 0.1f * (float)st; ln->trepeat = 0.6f;
		ln->roll = 0.0f;
		ln->whiten = 0;
		ln->taperstart = lift > 0.0f ? 0.0f : 0.7f;
	}

	// GROUNDING ARCS (Seb: "occasional arcs if close to a surface, 15 Hz 30% of
	// the time"): each roll of the bolt's rate, three in ten, the ball reaches for
	// a surface within 2.5 R along a random, downward-leaning direction; if one
	// is there it throws a curly filament to it and crackles. Keyed on the roll
	// and the entity, so it is framerate-invariant like everything else here.
	// Not while charging or fizzling.
	if (frame == M5BALL_IDLE || frame == M5BALL_ZAP)
	{
		randomseed_t gs;
		CL_Beam_M5_Seed(entity, 5, roll, &gs);
		if (Math_randomf(&gs) < 0.3f && fil.numlines + 2 <= M5BOLT_MAXLINES && fil.numpoints + nodes <= M5BOLT_MAXPOINTS)
		{
			vec3_t dir, far, u, v;
			trace_t tr;
			VectorLehmerRandom(&gs, dir);
			dir[2] -= 0.6f;					// prefers the floor, as the reference shows
			if (VectorLength2(dir) > 1e-4f)
			{
				VectorNormalize(dir);
				VectorMA(origin, 2.5f * R, dir, far);
				tr = CL_TraceLine(origin, far, MOVE_NOMONSTERS, NULL,
				SUPERCONTENTS_SOLID | SUPERCONTENTS_SKY, 0, 0,
				collision_extendmovelength.value, true, false, NULL, false, false);
				if (tr.fraction < 1.0f && tr.fraction * 2.5f * R > R * 0.9f)
				{
					// a curly filament from the shell to the wall, same recipe as the
					// ones inside, wider and brighter: this one is a discharge
					vec3_t a, axis;
					float len, amp, twist, phi;
					static int lastroll[64];
					VectorMA(origin, R * 0.95f, dir, a);
					VectorSubtract(tr.endpos, a, axis);
					len = (float)VectorLength(axis);
					if (len > 1.0f)
					{
						VectorScale(axis, 1.0f / len, axis);
						CL_Beam_M5_Basis(axis, &gs, u, v);
						amp = 0.25f * len * (0.5f + Math_randomf(&gs));
						twist = (1.0f + 2.0f * Math_randomf(&gs)) * (Math_randomf(&gs) < 0.5f ? -1.0f : 1.0f);
						phi = Math_randomf(&gs) * (float)(2.0 * M_PI);
						first = fil.numpoints;
						for (i = 0; i < nodes; i++)
						{
							float t = (float)i / (float)(nodes - 1);
							float sw = sinf((float)M_PI * t);
							float ang = phi + twist * t;
							VectorMA(a, len * t, axis, fil.points[first + i]);
							VectorMA(fil.points[first + i], amp * sw * cosf(ang), u, fil.points[first + i]);
							VectorMA(fil.points[first + i], amp * sw * sinf(ang), v, fil.points[first + i]);
						}
						fil.numpoints += nodes;
						ln = &fil.lines[fil.numlines++];
						ln->first = first; ln->count = nodes;
						ln->width = ln->widthtip = corew * 0.6f;
						ln->alpha = ln->alphatip = 1.0f * level;
						ln->toffset = 0.5f; ln->trepeat = 1.0f;
						ln->roll = 0.0f;
						ln->whiten = 0;
						ln->taperstart = 0.0f;		// it lands on the wall; no dissolve
						ln = &fil.lines[fil.numlines++];
						ln->first = first; ln->count = nodes;
						ln->width = ln->widthtip = sheathw * 0.7f;
						ln->alpha = ln->alphatip = sheatha * 0.5f * level;
						ln->toffset = 0.2f; ln->trepeat = 0.6f;
						ln->roll = 0.0f;
						ln->whiten = 0;
						ln->taperstart = 0.0f;
						// the crackle, once per roll that lands one
						if (lastroll[entity & 63] != roll)
						{
							vec3_t at;
							int fx;
							VectorCopy(tr.endpos, at);		// trace_t.endpos is double[3]
							lastroll[entity & 63] = roll;
							S_StartSound(-1, 0, S_PrecacheSound("weapons/lhit.wav", false, false), at, 0.35f, 1.0f);
							// and the mark it leaves: a scorch and sparks (m5/effectinfo.txt
							// m5ball_ground), fired client-side -- destruction the room keeps
							fx = CL_ParticleEffectIndexForName("m5ball_ground");
							if (fx > 0)
							{
								// BEAUTY A6: the surface normal as the base velocity, so the
								// scorch-glow layer (orientation oriented) lies on the surface
								vec3_t nrm;
								VectorCopy(tr.plane.normal, nrm);
								CL_ParticleEffect(fx, 1, at, at, nrm, nrm, NULL, 0);
							}
						}
					}
				}
			}
		}
	}

	// VOLCANIC SPIKES (Seb's sixth look: "spikes, filaments when it bulges";
	// eighth: "more spikes ... read more as plasma filaments and less like jaggy
	// lightning bolts ... erupt from the surface, not be stuck on"): the shell's
	// silhouette bulge is a noise of the angle about the view axis, in the
	// quad's own frame (x along the view's up, y along the side the pass derives
	// from it), so the CPU walks the same curve and where it peaks a CURLED
	// plasma filament erupts outward -- rooted INSIDE the shell so it emerges
	// rather than sits on it, wide and bright at the root, thinning and
	// dissolving at the tip, in the filaments' own hue. Only on the SDF path --
	// the ribbon has no shell to erupt from.
	if (CL_Beam_M5_SDFActive() && (frame == M5BALL_IDLE || frame == M5BALL_ZAP || frame == M5BALL_SHATTER))
	{
		vec3_t axis, tocam, side, eye;
		float t = (float)r_refdef.scene.time;
		float thresh = frame == M5BALL_SHATTER ? -0.4f : 0.2f - 0.35f * vio;		// shattering: nearly every bulge erupts; a violent ball, most of them
		float reachmul = (frame == M5BALL_SHATTER ? 2.5f : 1.0f) * (1.0f + 0.9f * vio);
		const int sn = 9;
		int k, eruptfx = -1;
		static int eruptroll[64];
		// the SPARKS: once per roll, every eruption throws a few off its tip
		// (m5/effectinfo.txt m5ball_erupt, fired client-side like the grounding's
		// scorch) so the sparks visibly leave the ball rather than hang on it
		qbool newroll = eruptroll[entity & 63] != roll;
		if (newroll)
		{
			eruptroll[entity & 63] = roll;
			eruptfx = CL_ParticleEffectIndexForName("m5ball_erupt");
		}
		VectorCopy(r_refdef.view.up, axis);
		VectorCopy(r_refdef.view.origin, eye);
		VectorSubtract(eye, origin, tocam);
		CrossProduct(axis, tocam, side);
		if (VectorLength2(side) > 1e-6f)
		{
			VectorNormalize(side);
			for (k = 0; k < 48 && fil.numlines + 3 <= M5BOLT_MAXLINES && fil.numpoints + sn + 2 <= M5BOLT_MAXPOINTS; k++)
			{
				float ang = (float)k * (float)(2.0 * M_PI / 48.0);
				float b = CL_Beam_M5_ShellBulge(ang, t, vio);
				float bl = CL_Beam_M5_ShellBulge(ang - (float)(2.0 * M_PI / 48.0), t, vio);
				float br = CL_Beam_M5_ShellBulge(ang + (float)(2.0 * M_PI / 48.0), t, vio);
				vec3_t dir, root, tip, u, v;
				float reach, amp, twist, phi, len;
				int n;
				// a local maximum of the bulge, and a real one
				if (b < thresh || b < bl || b < br)
					continue;
				VectorScale(axis, cosf(ang), dir);
				VectorMA(dir, sinf(ang), side, dir);
				reach = R * (0.45f + 1.1f * max(0.0f, b - thresh)) * reachmul;
				VectorMA(origin, R * 0.88f, dir, root);				// inside the shell: it EMERGES
				VectorMA(root, reach + R * 0.12f, dir, tip);
				CL_Beam_M5_Basis(dir, &seed, u, v);
				len = (float)VectorDistance(root, tip);
				amp = 0.3f * len * (0.5f + Math_randomf(&seed));
				twist = (1.5f + 2.0f * Math_randomf(&seed)) * (Math_randomf(&seed) < 0.5f ? -1.0f : 1.0f);
				phi = Math_randomf(&seed) * (float)(2.0 * M_PI);
				first = fil.numpoints;
				for (n = 0; n < sn; n++)
				{
					float tt = (float)n / (float)(sn - 1);
					float sw = sinf((float)M_PI * tt) * tt;		// straight at the root, curling toward the tip
					float a2 = phi + twist * tt;
					VectorLerp(root, tt, tip, fil.points[first + n]);
					VectorMA(fil.points[first + n], amp * sw * cosf(a2), u, fil.points[first + n]);
					VectorMA(fil.points[first + n], amp * sw * sinf(a2), v, fil.points[first + n]);
				}
				fil.numpoints += sn;
				// the thread: bright and wide at the root, thin at the tip, the
				// filaments' hue, no white
				ln = &fil.lines[fil.numlines++];
				ln->first = first; ln->count = sn;
				ln->width = corew * 1.1f; ln->widthtip = corew * 0.2f;
				ln->alpha = 1.0f * level; ln->alphatip = 0.35f * level;
				ln->toffset = 0.3f; ln->trepeat = 1.0f;
				ln->roll = 0.0f;
				ln->whiten = 0;
				ln->taperstart = 0.6f;
				// its glow, widest at the root where it breaks the surface
				ln = &fil.lines[fil.numlines++];
				ln->first = first; ln->count = sn;
				ln->width = sheathw * 1.4f; ln->widthtip = sheathw * 0.3f;
				ln->alpha = 0.55f * level; ln->alphatip = 0.08f * level;
				ln->toffset = 0.6f; ln->trepeat = 0.6f;
				ln->roll = 0.0f;
				ln->whiten = 0;
				ln->taperstart = 0.6f;
				// the eruption: a short hot capsule at the root, the flare where it
				// breaks the shell
				first = fil.numpoints;
				VectorMA(origin, R * 0.9f, dir, fil.points[first]);
				VectorMA(origin, R * 1.12f, dir, fil.points[first + 1]);
				fil.numpoints += 2;
				ln = &fil.lines[fil.numlines++];
				ln->first = first; ln->count = 2;
				ln->width = corew * 1.6f * (1.0f + 0.8f * vio); ln->widthtip = corew * 1.6f;	// a wider, hotter crater the more violent the ball
				ln->alpha = ln->alphatip = 0.9f * level;
				ln->toffset = 0.5f; ln->trepeat = 1.0f;
				ln->roll = 0.0f;
				ln->whiten = 1;
				ln->taperstart = 0.0f;
				if (eruptfx > 0)
				{
					vec3_t from, vel;
					VectorLerp(root, 0.45f, tip, from);
					VectorScale(dir, 110.0f + 120.0f * vio, vel);
					CL_ParticleEffect(eruptfx, 1, from, from, vel, vel, NULL, 0);
				}
			}
		}
	}

	if (!m5_ballreported)
	{
		// first-event per process (the console-ink rule); asserted by tests/smoke.sh
		m5_ballreported = true;
		Con_DPrintf("M5 ball: plasma knot active (%d strands, %s)\n", strands,
			CL_Beam_M5_SDFActive() ? "SDF" : "ribbon");
	}
	if (path.numlines > 0)
		CL_Beam_M5_AddPolygons(&path);
	if (fil.numlines > 0)
		CL_Beam_M5_AddPolygons(&fil);
}

/// The flicker floor is a safety property, so it is asserted rather than assumed.
/// Sweeps the envelope at 1 kHz for a minute with the depth forced well past its
/// cvar bound, and reports the extremes. The minimum must never fall below
/// M5_FLICKER_MIN whatever anyone puts in a config; tests/smoke.sh greps the
/// verdict, so this fails loudly if someone later "makes the flicker punchier".
static void CL_Beam_M5_Test_f(cmd_state_t *cmd)
{
	float saved = r_lightningbeam_m5_flicker.value;
	float lo = 1.0f, hi = 0.0f;
	int i;
	(void)cmd;
	Cvar_SetValueQuick(&r_lightningbeam_m5_flicker, 4.0f);		// far outside the 0-1 bound
	for (i = 0; i < 60000; i++)
	{
		float e = CL_Beam_M5_Envelope(i * 0.001, 0.0f, 1.7f);
		if (e < lo)
			lo = e;
		if (e > hi)
			hi = e;
	}
	Cvar_SetValueQuick(&r_lightningbeam_m5_flicker, saved);
	Con_Printf("M5 bolt envelope: min %.3f max %.3f floor %.3f - %s\n",
		(double)lo, (double)hi, (double)M5_FLICKER_MIN,
		lo >= M5_FLICKER_MIN - 1e-4f ? "floor holds" : "FLOOR VIOLATED");

	// Does the shape actually differ between re-rolls? It did not for the whole
	// of this feature's first life: the seed's low word was a constant and it is
	// the only word Math_crandomf can see, so every bolt in the game was one
	// frozen curve (see CL_Beam_M5_Seed). Nothing on screen said so - the bolt
	// looked plausible, it just never changed. Draw the first few values for a
	// run of rolls and report the CLOSEST any two come to each other; frozen
	// reads as exactly 0. Needs no map loaded.
	{
		#define M5_TESTROLLS	64
		#define M5_TESTDRAWS	8
		static float v[M5_TESTROLLS][M5_TESTDRAWS];
		float worst = 1e9f;
		int r, k, r2;
		for (r = 0; r < M5_TESTROLLS; r++)
		{
			randomseed_t sd;
			CL_Beam_M5_Seed(1, 0, r, &sd);
			for (k = 0; k < M5_TESTDRAWS; k++)
				v[r][k] = Math_crandomf(&sd);
		}
		for (r = 0; r < M5_TESTROLLS; r++)
			for (r2 = r + 1; r2 < M5_TESTROLLS; r2++)
			{
				float acc = 0.0f;
				for (k = 0; k < M5_TESTDRAWS; k++)
					acc += (v[r][k] - v[r2][k]) * (v[r][k] - v[r2][k]);
				acc = sqrtf(acc / M5_TESTDRAWS);
				if (acc < worst)
					worst = acc;
			}
		Con_Printf("M5 bolt seed: closest of %d rolls rms %.4f - %s\n",
			M5_TESTROLLS, (double)worst,
			worst > 0.02f ? "decorrelated" : "FROZEN");
		#undef M5_TESTROLLS
		#undef M5_TESTDRAWS
	}

	// THIRD: the arc actually walks the gun's three electrodes. The failure this
	// guards is the one the feature exists to fix, and it is silent -- an arc
	// pinned to one prong looks exactly like an arc that happens not to have
	// moved yet. Hash the same way CL_Beam_M5_MuzzleNode does over a run of
	// ticks and require that all three appear and that none of them takes more
	// than three quarters of them.
	{
		#define M5_TESTTICKS	120
		int hits[3], t, worstshare;
		hits[0] = hits[1] = hits[2] = 0;
		for (t = 0; t < M5_TESTTICKS; t++)
			hits[(int)CL_Beam_M5_MuzzleNodeAt(1, 0, t) + 1]++;
		worstshare = max(hits[0], max(hits[1], hits[2]));
		Con_Printf("M5 bolt muzzle: %d ticks land L%d C%d R%d - %s\n",
			M5_TESTTICKS, hits[0], hits[1], hits[2],
			(hits[0] && hits[1] && hits[2] && worstshare * 4 <= M5_TESTTICKS * 3)
				? "walks all three" : "PINNED");
		#undef M5_TESTTICKS
	}
}
