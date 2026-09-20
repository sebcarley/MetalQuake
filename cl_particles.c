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

#include "quakedef.h"

#include "cl_collision.h"
#include "image.h"
#include "r_shadow.h"

// Indexed by ptype_t. DESIGNATED and UNBOUNDED on purpose (SEPTEMBER S7,
// 2026-09-02): the positional form this replaced had 13 rows for a 15-entry
// enum, so pt_explode and pt_explode2 fell on zero-fill -- which happened to
// spell PBLEND_ALPHA / PARTICLE_BILLBOARD / false, the correct row, so nothing
// ever showed. The next type appended to the enum would not have been so lucky.
// The assert below is only meaningful on an array WITHOUT a declared bound: with
// [pt_total] the sizeof ratio is pt_total by definition and the check is vacuous.
//
// The third column is the per-type OPT-IN to cl_particles_lighting. Note the
// reach is really per BLENDMODE: the lit branch sits under PBLEND_ALPHA in the
// draw callback, so an ADD type with the flag set is never lit, and a type whose
// effectinfo block says `blend alpha` (m5's smoke is `type smoke` + `blend
// alpha`) IS lit. Only the flag's absence is decisive.
particletype_t particletype[] =
{
	[pt_dead]           = {PBLEND_INVALID, PARTICLE_INVALID, false}, // should never happen
	[pt_alphastatic]    = {PBLEND_ALPHA, PARTICLE_BILLBOARD, true},   // lit under cl_particles_lighting
	[pt_static]         = {PBLEND_ADD, PARTICLE_BILLBOARD, false},
	[pt_spark]          = {PBLEND_ADD, PARTICLE_SPARK, false},
	[pt_beam]           = {PBLEND_ADD, PARTICLE_HBEAM, false},
	[pt_rain]           = {PBLEND_ADD, PARTICLE_SPARK, false},
	[pt_raindecal]      = {PBLEND_ADD, PARTICLE_ORIENTED_DOUBLESIDED, false},
	[pt_snow]           = {PBLEND_ADD, PARTICLE_BILLBOARD, false},
	[pt_bubble]         = {PBLEND_ADD, PARTICLE_BILLBOARD, false},
	[pt_blood]          = {PBLEND_INVMOD, PARTICLE_BILLBOARD, false},
	[pt_smoke]          = {PBLEND_ADD, PARTICLE_BILLBOARD, true},     // lit when effectinfo makes it `blend alpha` (m5's smoke)
	[pt_decal]          = {PBLEND_INVMOD, PARTICLE_ORIENTED_DOUBLESIDED, false},
	[pt_entityparticle] = {PBLEND_ALPHA, PARTICLE_BILLBOARD, true},
	[pt_explode]        = {PBLEND_ALPHA, PARTICLE_BILLBOARD, false}, // Quake-style colour-ramp explosion
	[pt_explode2]       = {PBLEND_ALPHA, PARTICLE_BILLBOARD, false}, // (cl_particles_quake); spawned with explicit blend/orientation
	[pt_dust]           = {PBLEND_ALPHA, PARTICLE_BILLBOARD, true},    // m5_dust: lit, that is the point of it
};
DP_STATIC_ASSERT(sizeof(particletype) / sizeof(particletype[0]) == pt_total, "particletype[] must carry exactly one row per ptype_t value");

#define PARTICLEEFFECT_UNDERWATER 1
#define PARTICLEEFFECT_NOTUNDERWATER 2
#define PARTICLEEFFECT_FORCENEAREST 4
#define PARTICLEEFFECT_DEFINED 2147483648U

typedef struct particleeffectinfo_s
{
	int effectnameindex; // which effect this belongs to
	// PARTICLEEFFECT_* bits
	int flags;
	// blood effects may spawn very few particles, so proper fraction-overflow
	// handling is very important, this variable keeps track of the fraction
	double particleaccumulator;
	// the math is: countabsolute + requestedcount * countmultiplier * quality
	// absolute number of particles to spawn, often used for decals
	// (unaffected by quality and requestedcount)
	float countabsolute;
	// multiplier for the number of particles CL_ParticleEffect was told to
	// spawn, most effects do not really have a count and hence use 1, so
	// this is often the actual count to spawn, not merely a multiplier
	float countmultiplier;
	// if > 0 this causes the particle to spawn in an evenly spaced line from
	// originmins to originmaxs (causing them to describe a trail, not a box)
	float trailspacing;
	// type of particle to spawn (defines some aspects of behavior)
	ptype_t particletype;
	// blending mode used on this particle type
	pblend_t blendmode;
	// orientation of this particle type (BILLBOARD, SPARK, BEAM, etc)
	porientation_t orientation;
	// range of colors to choose from in hex RRGGBB (like HTML color tags),
	// randomly interpolated at spawn
	unsigned int color[2];
	// a random texture is chosen in this range (note the second value is one
	// past the last choosable, so for example 8,16 chooses any from 8 up and
	// including 15)
	// if start and end of the range are the same, no randomization is done
	int tex[2];
	// range of size values randomly chosen when spawning, plus size increase over time
	float size[3];
	// range of alpha values randomly chosen when spawning, plus alpha fade
	float alpha[3];
	// how long the particle should live (note it is also removed if alpha drops to 0)
	float time[2];
	// how much gravity affects this particle (negative makes it fly up!)
	float gravity;
	// how much bounce the particle has when it hits a surface
	// if negative the particle is removed on impact
	float bounce;
	// if in air this friction is applied
	// if negative the particle accelerates
	float airfriction;
	// if in liquid (water/slime/lava) this friction is applied
	// if negative the particle accelerates
	float liquidfriction;
	// these offsets are added to the values given to particleeffect(), and
	// then an ellipsoid-shaped jitter is added as defined by these
	// (they are the 3 radii)
	float stretchfactor;
	// stretch velocity factor (used for sparks)
	float originoffset[3];
	float relativeoriginoffset[3];
	float velocityoffset[3];
	float relativevelocityoffset[3];
	float originjitter[3];
	float velocityjitter[3];
	float velocitymultiplier;
	// an effect can also spawn a dlight
	float lightradiusstart;
	float lightradiusfade;
	float lighttime;
	float lightcolor[3];
	qbool lightshadow;
	int lightcubemapnum;
	float lightcorona[2];
	unsigned int staincolor[2]; // note: 0x808080 = neutral (particle's own color), these are modding factors for the particle's original color!
	int staintex[2];
	float stainalpha[2];
	float stainsize[2];
	// other parameters
	float rotate[4]; // min/max base angle, min/max rotation over time
	// SEPTEMBER S5 (2026-09-02): `delay min max` -- seconds after the effect
	// fires before this layer's particles APPEAR (delayedspawn, the rain-splash
	// mechanism: no physics, no fade, no draw until then; the lifetime and the
	// spin clock start at the delayed moment). What lets smoke rise AFTER the
	// flash instead of over it. Appended last: the baseline initialiser is
	// positional.
	float delay[2];
	// BEAUTY A6 (2026-09-16): `requirecvar <name>` -- the layer spawns only while
	// that cvar is above 0, decided before any random number is drawn, so a
	// layer that is off costs the effect's realisation nothing. The name is
	// resolved at spawn (a typo prints once and the layer never spawns).
	char requirecvar[32];
}
particleeffectinfo_t;

char particleeffectname[MAX_PARTICLEEFFECTNAME][64];

int numparticleeffectinfo;
particleeffectinfo_t particleeffectinfo[MAX_PARTICLEEFFECTINFO];

static int particlepalette[256];
/*
	0x000000,0x0f0f0f,0x1f1f1f,0x2f2f2f,0x3f3f3f,0x4b4b4b,0x5b5b5b,0x6b6b6b, // 0-7
	0x7b7b7b,0x8b8b8b,0x9b9b9b,0xababab,0xbbbbbb,0xcbcbcb,0xdbdbdb,0xebebeb, // 8-15
	0x0f0b07,0x170f0b,0x1f170b,0x271b0f,0x2f2313,0x372b17,0x3f2f17,0x4b371b, // 16-23
	0x533b1b,0x5b431f,0x634b1f,0x6b531f,0x73571f,0x7b5f23,0x836723,0x8f6f23, // 24-31
	0x0b0b0f,0x13131b,0x1b1b27,0x272733,0x2f2f3f,0x37374b,0x3f3f57,0x474767, // 32-39
	0x4f4f73,0x5b5b7f,0x63638b,0x6b6b97,0x7373a3,0x7b7baf,0x8383bb,0x8b8bcb, // 40-47
	0x000000,0x070700,0x0b0b00,0x131300,0x1b1b00,0x232300,0x2b2b07,0x2f2f07, // 48-55
	0x373707,0x3f3f07,0x474707,0x4b4b0b,0x53530b,0x5b5b0b,0x63630b,0x6b6b0f, // 56-63
	0x070000,0x0f0000,0x170000,0x1f0000,0x270000,0x2f0000,0x370000,0x3f0000, // 64-71
	0x470000,0x4f0000,0x570000,0x5f0000,0x670000,0x6f0000,0x770000,0x7f0000, // 72-79
	0x131300,0x1b1b00,0x232300,0x2f2b00,0x372f00,0x433700,0x4b3b07,0x574307, // 80-87
	0x5f4707,0x6b4b0b,0x77530f,0x835713,0x8b5b13,0x975f1b,0xa3631f,0xaf6723, // 88-95
	0x231307,0x2f170b,0x3b1f0f,0x4b2313,0x572b17,0x632f1f,0x733723,0x7f3b2b, // 96-103
	0x8f4333,0x9f4f33,0xaf632f,0xbf772f,0xcf8f2b,0xdfab27,0xefcb1f,0xfff31b, // 104-111
	0x0b0700,0x1b1300,0x2b230f,0x372b13,0x47331b,0x533723,0x633f2b,0x6f4733, // 112-119
	0x7f533f,0x8b5f47,0x9b6b53,0xa77b5f,0xb7876b,0xc3937b,0xd3a38b,0xe3b397, // 120-127
	0xab8ba3,0x9f7f97,0x937387,0x8b677b,0x7f5b6f,0x775363,0x6b4b57,0x5f3f4b, // 128-135
	0x573743,0x4b2f37,0x43272f,0x371f23,0x2b171b,0x231313,0x170b0b,0x0f0707, // 136-143
	0xbb739f,0xaf6b8f,0xa35f83,0x975777,0x8b4f6b,0x7f4b5f,0x734353,0x6b3b4b, // 144-151
	0x5f333f,0x532b37,0x47232b,0x3b1f23,0x2f171b,0x231313,0x170b0b,0x0f0707, // 152-159
	0xdbc3bb,0xcbb3a7,0xbfa39b,0xaf978b,0xa3877b,0x977b6f,0x876f5f,0x7b6353, // 160-167
	0x6b5747,0x5f4b3b,0x533f33,0x433327,0x372b1f,0x271f17,0x1b130f,0x0f0b07, // 168-175
	0x6f837b,0x677b6f,0x5f7367,0x576b5f,0x4f6357,0x475b4f,0x3f5347,0x374b3f, // 176-183
	0x2f4337,0x2b3b2f,0x233327,0x1f2b1f,0x172317,0x0f1b13,0x0b130b,0x070b07, // 184-191
	0xfff31b,0xefdf17,0xdbcb13,0xcbb70f,0xbba70f,0xab970b,0x9b8307,0x8b7307, // 192-199
	0x7b6307,0x6b5300,0x5b4700,0x4b3700,0x3b2b00,0x2b1f00,0x1b0f00,0x0b0700, // 200-207
	0x0000ff,0x0b0bef,0x1313df,0x1b1bcf,0x2323bf,0x2b2baf,0x2f2f9f,0x2f2f8f, // 208-215
	0x2f2f7f,0x2f2f6f,0x2f2f5f,0x2b2b4f,0x23233f,0x1b1b2f,0x13131f,0x0b0b0f, // 216-223
	0x2b0000,0x3b0000,0x4b0700,0x5f0700,0x6f0f00,0x7f1707,0x931f07,0xa3270b, // 224-231
	0xb7330f,0xc34b1b,0xcf632b,0xdb7f3b,0xe3974f,0xe7ab5f,0xefbf77,0xf7d38b, // 232-239
	0xa77b3b,0xb79b37,0xc7c337,0xe7e357,0x7fbfff,0xabe7ff,0xd7ffff,0x670000, // 240-247
	0x8b0000,0xb30000,0xd70000,0xff0000,0xfff393,0xfff7c7,0xffffff,0x9f5b53  // 248-255
*/

int		ramp1[8] = {0x6f, 0x6d, 0x6b, 0x69, 0x67, 0x65, 0x63, 0x61};
int		ramp2[8] = {0x6f, 0x6e, 0x6d, 0x6c, 0x6b, 0x6a, 0x68, 0x66};
int		ramp3[8] = {0x6d, 0x6b, 6, 5, 4, 3};

//static int explosparkramp[8] = {0x4b0700, 0x6f0f00, 0x931f07, 0xb7330f, 0xcf632b, 0xe3974f, 0xffe7b5, 0xffffff};

// particletexture_t is a rectangle in the particlefonttexture
typedef struct particletexture_s
{
	rtexture_t *texture;
	float s1, t1, s2, t2;
}
particletexture_t;

static rtexturepool_t *particletexturepool;
static rtexture_t *particlefonttexture;
static particletexture_t particletexture[MAX_PARTICLETEXTURES];
skinframe_t *decalskinframe;

// texture numbers in particle font
static const int tex_smoke[8] = {0, 1, 2, 3, 4, 5, 6, 7};
static const int tex_bulletdecal[8] = {8, 9, 10, 11, 12, 13, 14, 15};
static const int tex_blooddecal[8] = {16, 17, 18, 19, 20, 21, 22, 23};
static const int tex_bloodparticle[8] = {24, 25, 26, 27, 28, 29, 30, 31};
static const int tex_rainsplash = 32;
static const int tex_square = 33;
static const int tex_beam = 60;
static const int tex_bubble = 62;
static const int tex_raindrop = 61;
static const int tex_particle = 63;
// SEPTEMBER S6: the shotgun shell casing. 34 is the lowest free cell (33 is
// tex_square, deliberately white and in use). tex_shellcasing is the RESOLVED
// index, re-resolved on every R_InitParticleTexture: the cell when the engine
// generates its own font, tex_particle when an external particles/particlefont.tga
// replaces the atlas wholesale (that path generates no procedural cell at all, so
// cell 34 there is whatever the pack put in it). Assigned explicitly on BOTH
// branches, because vid_restart re-runs the init and a stale 34 would survive a
// procedural -> external transition.
static const int tex_shellcasing_cell = 34;
static int tex_shellcasing = 63;
// SEPTEMBER S7/S5 (2026-09-02): two more procedural cells, same resolution
// discipline as the casing. 35 = DUST, a hard-edged white disc (the soft blob at
// dust sizes reads as a smudge; a mote is a point). 36 = RING, for the explosion
// shockwave: a thin bright annulus with a faint fill, additive, expanding.
static const int tex_dust_cell = 35;
static int tex_dust = 63;
static const int tex_ring_cell = 36;
static int tex_ring = 63;
// BEAUTY A2/A3 (2026-09-13): two cells authored for a HOT look, the answer to
// "less of a soft gl effect". 37 is the muzzle FLASH -- a starburst with a hot
// core; 38 is the SPARK STREAK -- a thin bright line along the cell's s axis,
// which PARTICLE_SPARK stretches along the velocity (R_CalcBeam_Vertex3f puts
// s1 at the start of the streak and s2 at its end; t is the thickness). Both
// are additive, so alpha IS brightness and the RGB is white for the vertex
// colour to tint. Resolved like the casing: the cell when the engine's own
// font is in use, tex_particle when an external particlefont.tga replaces it.
static const int tex_flash_cell = 37;
static int tex_flash = 63;
static const int tex_sparkhot_cell = 38;
static int tex_sparkhot = 63;
// BEAUTY A2 (2026-09-16): two more authored cells. 39 is the EMBER -- a hard
// bright bead with a rim and a short halo, additive, for the slow bouncing
// grains of an explosion (PARTICLE_SPARK stretches it a little along the
// velocity); the blob read as a soft dot at any size. 40 is the blood DROPLET
// -- one hard-edged teardrop, dark red, INVERTED like the blood cells (24-31)
// because pt_blood draws inverse-modulate: outside the drop the cell is white,
// which inverts to black, which under that blend leaves the wall alone. It
// replaces the eight random-blotch cells on the airborne blood under
// cl_particles_blood_droplet. Resolved like the casing: -1 for the droplet
// means "use the blood cells", the fallback when an external font is in use.
static const int tex_ember_cell = 39;
static int tex_ember = 63;
static const int tex_droplet_cell = 40;
static int tex_droplet = -1;
// The atlas cell size in use (cl_particles_texsize applied), and the size the
// font was last generated at. 64 is the 2001 font, texel for texel.
static int particletexsize = 64;
static int particletexsize_applied = 0;
static int M5_BloodTex(void);
static float M5_BloodAngle(void);

particleeffectinfo_t baselineparticleeffectinfo =
{
	0, //int effectnameindex; // which effect this belongs to
	// PARTICLEEFFECT_* bits
	0, //int flags;
	// blood effects may spawn very few particles, so proper fraction-overflow
	// handling is very important, this variable keeps track of the fraction
	0.0, //double particleaccumulator;
	// the math is: countabsolute + requestedcount * countmultiplier * quality
	// absolute number of particles to spawn, often used for decals
	// (unaffected by quality and requestedcount)
	0.0f, //float countabsolute;
	// multiplier for the number of particles CL_ParticleEffect was told to
	// spawn, most effects do not really have a count and hence use 1, so
	// this is often the actual count to spawn, not merely a multiplier
	0.0f, //float countmultiplier;
	// if > 0 this causes the particle to spawn in an evenly spaced line from
	// originmins to originmaxs (causing them to describe a trail, not a box)
	0.0f, //float trailspacing;
	// type of particle to spawn (defines some aspects of behavior)
	pt_alphastatic, //ptype_t particletype;
	// blending mode used on this particle type
	PBLEND_ALPHA, //pblend_t blendmode;
	// orientation of this particle type (BILLBOARD, SPARK, BEAM, etc)
	PARTICLE_BILLBOARD, //porientation_t orientation;
	// range of colors to choose from in hex RRGGBB (like HTML color tags),
	// randomly interpolated at spawn
	{0xFFFFFF, 0xFFFFFF}, //unsigned int color[2];
	// a random texture is chosen in this range (note the second value is one
	// past the last choosable, so for example 8,16 chooses any from 8 up and
	// including 15)
	// if start and end of the range are the same, no randomization is done
	{63, 63 /* tex_particle */}, //int tex[2];
	// range of size values randomly chosen when spawning, plus size increase over time
	{1, 1, 0.0f}, //float size[3];
	// range of alpha values randomly chosen when spawning, plus alpha fade
	{0.0f, 256.0f, 256.0f}, //float alpha[3];
	// how long the particle should live (note it is also removed if alpha drops to 0)
	{16777216.0f, 16777216.0f}, //float time[2];
	// how much gravity affects this particle (negative makes it fly up!)
	0.0f, //float gravity;
	// how much bounce the particle has when it hits a surface
	// if negative the particle is removed on impact
	0.0f, //float bounce;
	// if in air this friction is applied
	// if negative the particle accelerates
	0.0f, //float airfriction;
	// if in liquid (water/slime/lava) this friction is applied
	// if negative the particle accelerates
	0.0f, //float liquidfriction;
	// these offsets are added to the values given to particleeffect(), and
	// then an ellipsoid-shaped jitter is added as defined by these
	// (they are the 3 radii)
	1.0f, //float stretchfactor;
	// stretch velocity factor (used for sparks)
	{0.0f, 0.0f, 0.0f}, //float originoffset[3];
	{0.0f, 0.0f, 0.0f}, //float relativeoriginoffset[3];
	{0.0f, 0.0f, 0.0f}, //float velocityoffset[3];
	{0.0f, 0.0f, 0.0f}, //float relativevelocityoffset[3];
	{0.0f, 0.0f, 0.0f}, //float originjitter[3];
	{0.0f, 0.0f, 0.0f}, //float velocityjitter[3];
	0.0f, //float velocitymultiplier;
	// an effect can also spawn a dlight
	0.0f, //float lightradiusstart;
	0.0f, //float lightradiusfade;
	16777216.0f, //float lighttime;
	{1.0f, 1.0f, 1.0f}, //float lightcolor[3];
	true, //qbool lightshadow;
	0, //int lightcubemapnum;
	{1.0f, 0.25f}, //float lightcorona[2];
	{(unsigned int)-1, (unsigned int)-1}, //unsigned int staincolor[2]; // note: 0x808080 = neutral (particle's own color), these are modding factors for the particle's original color!
	{-1, -1}, //int staintex[2];
	{1.0f, 1.0f}, //float stainalpha[2];
	{2.0f, 2.0f}, //float stainsize[2];
	// other parameters
	{0.0f, 360.0f, 0.0f, 0.0f}, //float rotate[4]; // min/max base angle, min/max rotation over time
	{0.0f, 0.0f}, //float delay[2]; // SEPTEMBER S5: spawn delay min/max seconds
	"", //char requirecvar[32]; // BEAUTY A6
};

cvar_t cl_particles = {CF_CLIENT | CF_ARCHIVE, "cl_particles", "1", "enables particle effects"};
cvar_t cl_particles_quality = {CF_CLIENT | CF_ARCHIVE, "cl_particles_quality", "1", "multiplies number of particles"};
// SEPTEMBER S7 (2026-09-02): particles were entirely UNLIT -- particletype[]'s
// lighting flag was false for every type and the branch it gates had never run.
// Three modes, because the engine's own path is exact but unbounded and the
// cheap path is bounded but approximate, and only a measurement says which ships.
cvar_t cl_particles_lighting = {CF_CLIENT | CF_ARCHIVE, "cl_particles_lighting", "2", "light alpha-blended particles -- smoke, dust, the pointfile -- from the world instead of drawing them at their flat colour. **2 is the default since 2026-09-19**, because the ambient dust ships on and an unlit mote reads as a flat grey speck rather than as ash taking the room's light. 0 = off, the pre-2026-09-19 picture exactly. 1 = the engine's own LightPoint per particle: the baked lightmap beneath it plus every dynamic light in range with a shadow traceline each (exact; the tracelines are uncapped, so a busy frame pays). 2 = the bounded hybrid: the volumetric irradiance grid baked at map load -- the same grid that lights the fog, so smoke reads lit like the fog in the same air -- plus this frame's dynamic lights with the engine's own falloff and no tracelines. Static lights come through the lightmap in both modes; under rt_metal_walllight that is the lightmap the walls have discarded, so cl_particles_lighting_gain is the taste knob"};
// SEPTEMBER S7 item 2: ambient dust. No ambient particle system existed -- every
// particle in the tree is event-driven -- so this is the first thing that puts
// motes in the air for the light to catch. ABSOLUTE count on purpose:
// cl_particles_quality triples every effect and Seb runs 3.
cvar_t m5_dust = {CF_CLIENT | CF_ARCHIVE, "m5_dust", "512", "ambient dust: how many motes to keep drifting in the air around you. **512 is the default since 2026-09-19** -- Seb's own played-in value, made the shipped one on his word (\"whatever my current saved config is ... make that the default config for dust\"); 0 is none, the pre-2026-09-19 picture exactly, and the Stock tier suppresses it whatever the number. An ABSOLUTE count, deliberately not scaled by cl_particles_quality. Spawned in open air within m5_dust_radius -- never inside a wall or a liquid, never behind a wall -- and topped up as they die or drift off. Lit like any other alpha particle, so pair it with cl_particles_lighting or the motes read flat grey in a dark room; the payoff is dust catching torchlight, a muzzle flash, a rocket's glow or the handlamp. 128-512 is the sensible range. The emitter yields to effects: it stops spawning when the particle pool is within 512 of full"};
cvar_t m5_dust_radius = {CF_CLIENT | CF_ARCHIVE, "m5_dust_radius", "320", "how far from the eye dust motes are kept, in world units; a mote that drifts past twice this is recycled"};
cvar_t m5_dust_size = {CF_CLIENT | CF_ARCHIVE, "m5_dust_size", "0.3", "dust mote size in world units. 0.3 (the default since 2026-09-19, Seb's own) is a sub-pixel fleck at 1080p; the old 1.2 is a visible speck"};
cvar_t m5_dust_alpha = {CF_CLIENT | CF_ARCHIVE, "m5_dust_alpha", "1", "dust mote opacity at birth (0-1); each mote fades out over its 8-16 second life. 1 (the default since 2026-09-19, Seb's own) is the maximum and goes with the small m5_dust_size -- a fleck that is fully opaque but covers almost no pixel; the old 0.35 goes with the larger mote"};
cvar_t m5_dust_speed = {CF_CLIENT | CF_ARCHIVE, "m5_dust_speed", "5", "how briskly new dust drifts: the random velocity a mote is born with, world units per second (5 since 2026-09-19, Seb's own; was 6)"};
cvar_t m5_muzzleflash = {CF_CLIENT | CF_ARCHIVE, "m5_muzzleflash", "1", "BEAUTY A3 (2026-09-13; DEFAULT 1 since 2026-09-16 on Seb's QA -- \"the flash is fine\", \"sparks are fine\", then \"default on\"): draw a muzzle flash. Quake has never drawn one -- the flash is a dynamic LIGHT and nothing else, so with the murk on it reads as a soft glow round a barrel that shows no flash of its own. 1 = a brief starburst with a hot core at the muzzle of your shotgun, super shotgun, nailguns, grenade and rocket launchers (the thunderbolt and the ball are their own flash), and a smaller one on every monster's gun as it fires; the M5 shotgun's muzzle spray becomes a spray of hot-core streaks that leaves the barrel's tip and moves with the gun (m5_muzzleflash_sparks and its six companions tune it), instead of the wide yellow rays the round blob cell made of it. Gone in about 80 ms; brighter than white on the float scene buffer, so the bloom gives it a core. 0 = the 1996 picture, the light alone (exec flash_off.cfg)"};
cvar_t m5_muzzleflash_forward = {CF_CLIENT | CF_ARCHIVE, "m5_muzzleflash_forward", "0", "trim on the drawn muzzle flash's position along the gun, world units (the per-weapon table is in cl_particles.c)"};
cvar_t m5_muzzleflash_up = {CF_CLIENT | CF_ARCHIVE, "m5_muzzleflash_up", "0", "trim on the drawn muzzle flash's height above the gun, world units"};
cvar_t m5_muzzleflash_size = {CF_CLIENT | CF_ARCHIVE, "m5_muzzleflash_size", "1", "scale on the drawn muzzle flash's size"};
// Seb, 2026-09-15: "can i tune the muzzle sparks?" -- the flying sparks the
// Doom shotgun throws from its muzzle under m5_muzzleflash. Every knob is a scale
// on the spray as it ships (the M5_SPARKS_* base values below), so 1 everywhere
// is that spray.
cvar_t m5_muzzleflash_sparks = {CF_CLIENT | CF_ARCHIVE, "m5_muzzleflash_sparks", "1", "how many sparks the shotgun throws from its muzzle when m5_muzzleflash is 1 (and m5_shotgun is on), as a scale: 1 = eight (fourteen from the super shotgun), 2 = double, 0 = none -- the flash and the shells stay. Every m5_muzzleflash_sparks_* knob is a scale on the spray as it ships, so 1 everywhere is that spray"};
cvar_t m5_muzzleflash_sparks_speed = {CF_CLIENT | CF_ARCHIVE, "m5_muzzleflash_sparks_speed", "1", "scale on how fast the muzzle sparks fly, the forward push and the scatter together: faster sparks go further and draw longer streaks, because a spark's streak follows its speed"};
cvar_t m5_muzzleflash_sparks_spread = {CF_CLIENT | CF_ARCHIVE, "m5_muzzleflash_sparks_spread", "1", "scale on how widely the muzzle sparks scatter about the line of fire: 0 = a tight jet, 2 = a wide fan"};
cvar_t m5_muzzleflash_sparks_length = {CF_CLIENT | CF_ARCHIVE, "m5_muzzleflash_sparks_length", "1", "scale on the length of each muzzle spark's streak, leaving its speed alone; 0 = points"};
cvar_t m5_muzzleflash_sparks_thickness = {CF_CLIENT | CF_ARCHIVE, "m5_muzzleflash_sparks_thickness", "1", "scale on the thickness of each muzzle spark's streak"};
cvar_t m5_muzzleflash_sparks_life = {CF_CLIENT | CF_ARCHIVE, "m5_muzzleflash_sparks_life", "1", "scale on how long the muzzle sparks last; they fade out over that time"};
cvar_t m5_muzzleflash_sparks_brightness = {CF_CLIENT | CF_ARCHIVE, "m5_muzzleflash_sparks_brightness", "1", "scale on how bright the muzzle sparks start: below 1 dims them, above 1 holds them at full brightness for longer before they fade (one spark on its own cannot go past white)"};
cvar_t m5_dust_swirl = {CF_CLIENT | CF_ARCHIVE, "m5_dust_swirl", "1", "strength of the slow eddying that keeps the dust turning rather than coasting in straight lines (0 = straight drift, damped by air friction)"};
cvar_t m5_dust_tint = {CF_CLIENT | CF_ARCHIVE, "m5_dust_tint", "1", "colour of the dust. **1 (the default since 2026-09-19)** = each mote takes a colour from the BONFIRE-ASH palette -- soot and charcoal, charred brown, a warm and a cool grey, slate blue, one brown, one pale ash and a single off-white in sixteen -- so the cloud reads as flecks of ash rather than as white specks (Seb: \"more like flecks of blue/grey/brown bonfire ash ... it is just too white and noticeable most of the time\"). 0 = the 2026-09-02 grey RAMP from soot-dark to pure white, which is what made it white: the ramp is uniform, so 57%% of motes were above mid-grey against 12%% on the palette, and its mean mote luma is 143 against the palette's 82. `exec dust_white.cfg` is that look back; `exec dust_ash.cfg` is this one with the brightness settings that go with it"};
// STOCK MODE reads the lighting mode through this: 1996 drew particles at their
// flat colour, and a read-side accessor means the player's own setting is never
// written over -- m5_stock 0 restores it exactly. Three consumers.
static int CL_ParticleLightingMode(void);
cvar_t cl_particles_lighting_static = {CF_CLIENT | CF_ARCHIVE, "cl_particles_lighting_static", "1", "scale the STATIC half of a lit particle's light -- the room's own baked lighting, which on a Quake map is the wall torches and the ceiling lights -- while leaving this frame's DYNAMIC lights at full strength. 1 = no change, today's picture exactly. THE SPLIT IS THE POINT: a muzzle flash, a rocket's glow, an explosion and the handlamp are all dynamic, so at 0.4 the dust sits dark in an ordinary torchlit corridor and still flares white the instant something goes off (Seb, 2026-09-19: \"it's bright white -- which it can be when lit with muzzle flashes -- but it's just too white and noticeable most of the time\"). Under cl_particles_lighting 2 it scales the irradiance grid's term, which IS the static one, for free; under mode 1 it costs one extra traceline-free LP_LIGHTMAP probe per particle, and only while it is not 1. It does not touch r_ambient's flat fill, which is a floor you set on purpose"};
cvar_t cl_particles_lighting_gain = {CF_CLIENT | CF_ARCHIVE, "cl_particles_lighting_gain", "2", "multiplier on the light reaching lit particles (cl_particles_lighting > 0); the calibration knob for matching smoke to the walls around it. 2 since 2026-09-19 (Seb's own, shipped with the dust): under rt_metal_walllight the static light a particle reads is the lightmap the walls have DISCARDED, so a gain of 1 leaves motes reading darker than the room they are in"};
cvar_t cl_particles_alpha = {CF_CLIENT | CF_ARCHIVE, "cl_particles_alpha", "1", "multiplies opacity of particles"};
cvar_t cl_particles_size = {CF_CLIENT | CF_ARCHIVE, "cl_particles_size", "1", "multiplies particle size"};
extern cvar_t m5_stock;   // r_shadow.c -- STOCK MODE, the 1996 read-side master
static int CL_ParticleLightingMode(void)
{
	return m5_stock.integer ? 0 : cl_particles_lighting.integer;
}
cvar_t cl_particles_quake = {CF_CLIENT | CF_ARCHIVE, "cl_particles_quake", "0", "0: Fancy particles; 1: Disc particles like GLQuake; 2: Square particles like software-rendered Quake"};
cvar_t cl_particles_blood = {CF_CLIENT | CF_ARCHIVE, "cl_particles_blood", "1", "enables blood effects"};
cvar_t cl_particles_blood_alpha = {CF_CLIENT | CF_ARCHIVE, "cl_particles_blood_alpha", "1", "opacity of blood, does not affect decals"};
// BEAUTY A2 (2026-09-16): the atlas resolution and the droplet cell.
cvar_t cl_particles_texsize = {CF_CLIENT | CF_ARCHIVE, "cl_particles_texsize", "256", "BEAUTY A2: the size in pixels of every cell of the engine's own particle atlas, 64 (the 2001 font, today's picture texel for texel) or 256 (a 2048x2048 atlas, 16 MB: every spark, ember, puff, flash and droplet is authored sixteen times finer, so a particle magnified across a hundred pixels of a 1080p screen keeps its edge instead of the gaussian smear of a 64-pixel cell, and the smoke puffs carry two more octaves of detail). Takes effect at once -- the font is regenerated between frames. Inert when an external particles/particlefont.tga replaces the atlas (that file sets its own cell size). 256 is the SHIPPED default since 2026-09-17, on Seb's eye; 64 is the 2001 picture"};
// BEAUTY A5 (2026-09-16): the shockwave that bends the air. A particle layer
// with `blend refract` samples the composited frame behind it displaced along
// the cell's radial direction by its own coverage times this many pixels.
cvar_t cl_particles_refract = {CF_CLIENT | CF_ARCHIVE, "cl_particles_refract", "12", "BEAUTY A5: refracting particles -- how many screen pixels (at 1080p, scaled with the viewport) a `blend refract` particle layer displaces the frame behind it at full coverage, so an explosion's shockwave ring bends the air it passes through instead of drawing a white ring. The layers are in m5/effectinfo.txt (the explosion's and the ball's shockwaves); the frame copy is the water's, taken only on a frame where such a particle is in view, so nothing is paid otherwise. 8-16 is the sensible range and 12 is the shipped default since 2026-09-17, on Seb's eye. 0 = the layers are never spawned and today's picture is exact (toggling rebuilds the shaders -- a one-off hitch)"};
// BEAUTY A6 (2026-09-16): the scorch that cools. Consumed by effectinfo layers
// through `requirecvar cl_particles_scorchglow`; nothing in C reads it directly.
cvar_t cl_particles_scorchglow = {CF_CLIENT | CF_ARCHIVE, "cl_particles_scorchglow", "1", "BEAUTY A6: the mark a ball lightning leaves where it earths or grounds, and an axe blow's, GLOWS orange on the surface and cools to nothing over a few seconds -- an additive surface-oriented particle laid on the surface's own normal beside the dark scorch decal (m5/effectinfo.txt layers gated on this cvar by name through `requirecvar`). On by default since 2026-09-17, on Seb's eye. 0 = the dark scorch alone, the 1996 picture exactly; the layer is never spawned, before any random number is drawn"};
// BEAUTY B4 (2026-09-17): embers off the torches.
cvar_t m5_torch_embers = {CF_CLIENT | CF_ARCHIVE, "m5_torch_embers", "3", "BEAUTY B4: torches that live -- embers per second rising off every torch, brazier and candle flame in view (the light-core models: flame.mdl, flame2.mdl, AD's braziers, candles and lantern). Each ember is a hot grain (the ember cell) that rises, drifts, cools from orange to a dull red and dies in a second or two, spawned from the flame's tip with the flame's own scatter. 3 by default since 2026-09-17, on Seb's eye; 0 = none, the 1996 picture. 2-4 is the sensible range; the emitter yields to effects like the dust does and spawns at most 16 a frame"};
cvar_t cl_particles_blood_droplet = {CF_CLIENT | CF_ARCHIVE, "cl_particles_blood_droplet", "1", "BEAUTY A2: 1 draws the airborne blood as hard-edged DROPLETS (cell 40, one teardrop at a random angle) instead of the eight random-blotch cells the engine has drawn since 2001, which at any size read as soft red smudges. The blood decals on the walls are untouched. On by default since 2026-09-17, on Seb's eye. 0 = the 2001 blood"};
cvar_t cl_particles_blood_decal_alpha = {CF_CLIENT | CF_ARCHIVE, "cl_particles_blood_decal_alpha", "1", "opacity of blood decal"};
cvar_t cl_particles_blood_decal_scalemin = {CF_CLIENT | CF_ARCHIVE, "cl_particles_blood_decal_scalemin", "1.5", "minimal random scale of decal"};
cvar_t cl_particles_blood_decal_scalemax = {CF_CLIENT | CF_ARCHIVE, "cl_particles_blood_decal_scalemax", "2", "maximal random scale of decal"};
cvar_t cl_particles_blood_bloodhack = {CF_CLIENT | CF_ARCHIVE, "cl_particles_blood_bloodhack", "1", "make certain quake particle() calls create blood effects instead"};
cvar_t cl_particles_bulletimpacts = {CF_CLIENT | CF_ARCHIVE, "cl_particles_bulletimpacts", "1", "enables bulletimpact effects"};
cvar_t cl_particles_explosions_sparks = {CF_CLIENT | CF_ARCHIVE, "cl_particles_explosions_sparks", "1", "enables sparks from explosions"};
cvar_t cl_particles_explosions_shell = {CF_CLIENT | CF_ARCHIVE, "cl_particles_explosions_shell", "0", "enables polygonal shell from explosions"};
cvar_t cl_particles_rain = {CF_CLIENT | CF_ARCHIVE, "cl_particles_rain", "1", "enables rain effects"};
cvar_t cl_particles_snow = {CF_CLIENT | CF_ARCHIVE, "cl_particles_snow", "1", "enables snow effects"};
cvar_t cl_particles_smoke = {CF_CLIENT | CF_ARCHIVE, "cl_particles_smoke", "1", "enables smoke (used by multiple effects)"};
cvar_t cl_particles_smoke_alpha = {CF_CLIENT | CF_ARCHIVE, "cl_particles_smoke_alpha", "0.5", "smoke brightness"};
cvar_t cl_particles_smoke_alphafade = {CF_CLIENT | CF_ARCHIVE, "cl_particles_smoke_alphafade", "0.55", "brightness fade per second"};
cvar_t cl_particles_sparks = {CF_CLIENT | CF_ARCHIVE, "cl_particles_sparks", "1", "enables sparks (used by multiple effects)"};
cvar_t cl_particles_bubbles = {CF_CLIENT | CF_ARCHIVE, "cl_particles_bubbles", "1", "enables bubbles (used by multiple effects)"};
cvar_t cl_particles_visculling = {CF_CLIENT | CF_ARCHIVE, "cl_particles_visculling", "0", "perform a costly check if each particle is visible before drawing"};
cvar_t cl_particles_collisions = {CF_CLIENT | CF_ARCHIVE, "cl_particles_collisions", "1", "allow costly collision detection on particles (sparks that bounce, particles not going through walls, blood hitting surfaces, etc)"};
cvar_t cl_particles_forcetraileffects = {CF_CLIENT, "cl_particles_forcetraileffects", "0", "force trails to be displayed even if a non-trail draw primitive was used (debug/compat feature)"};
cvar_t cl_decals = {CF_CLIENT | CF_ARCHIVE, "cl_decals", "1", "enables decals (bullet holes, blood, etc)"};
cvar_t cl_decals_time = {CF_CLIENT | CF_ARCHIVE, "cl_decals_time", "20", "how long before decals start to fade away"};
cvar_t cl_decals_fadetime = {CF_CLIENT | CF_ARCHIVE, "cl_decals_fadetime", "1", "how long decals take to fade away"};
cvar_t cl_decals_newsystem_intensitymultiplier = {CF_CLIENT | CF_ARCHIVE, "cl_decals_newsystem_intensitymultiplier", "2", "boosts intensity of decals (because the distance fade can make them hard to see otherwise)"};
cvar_t cl_decals_newsystem_immediatebloodstain = {CF_CLIENT | CF_ARCHIVE, "cl_decals_newsystem_immediatebloodstain", "2", "0: no on-spawn blood stains; 1: on-spawn blood stains for pt_blood; 2: always use on-spawn blood stains"};
cvar_t cl_decals_newsystem_bloodsmears = {CF_CLIENT | CF_ARCHIVE, "cl_decals_newsystem_bloodsmears", "1", "enable use of particle velocity as decal projection direction rather than surface normal"};
cvar_t cl_decals_models = {CF_CLIENT | CF_ARCHIVE, "cl_decals_models", "0", "enables decals on animated models"};
cvar_t cl_decals_bias = {CF_CLIENT | CF_ARCHIVE, "cl_decals_bias", "0.125", "distance to bias decals from surface to prevent depth fighting"};
cvar_t cl_decals_max = {CF_CLIENT | CF_ARCHIVE, "cl_decals_max", "4096", "maximum number of decals allowed to exist in the world at once"};


static void CL_Particles_ParseEffectInfo(const char *textstart, const char *textend, const char *filename)
{
	int arrayindex;
	int argc;
	int i;
	int linenumber;
	particleeffectinfo_t *info = NULL;
	const char *text = textstart;
	char argv[16][1024];
	for (linenumber = 1;;linenumber++)
	{
		argc = 0;
		for (arrayindex = 0;arrayindex < 16;arrayindex++)
			argv[arrayindex][0] = 0;
		for (;;)
		{
			if (!COM_ParseToken_Simple(&text, true, false, true))
				return;
			if (!strcmp(com_token, "\n"))
				break;
			if (argc < 16)
			{
				dp_strlcpy(argv[argc], com_token, sizeof(argv[argc]));
				argc++;
			}
		}
		if (argc < 1)
			continue;
#define checkparms(n) if (argc != (n)) {Con_Printf("%s:%i: error while parsing: %s given %i parameters, should be %i parameters\n", filename, linenumber, argv[0], argc, (n));break;}
#define readints(array, n) checkparms(n+1);for (arrayindex = 0;arrayindex < argc - 1;arrayindex++) array[arrayindex] = strtol(argv[1+arrayindex], NULL, 0)
#define readfloats(array, n) checkparms(n+1);for (arrayindex = 0;arrayindex < argc - 1;arrayindex++) array[arrayindex] = atof(argv[1+arrayindex])
#define readint(var) checkparms(2);var = strtol(argv[1], NULL, 0)
#define readfloat(var) checkparms(2);var = atof(argv[1])
#define readbool(var) checkparms(2);var = strtol(argv[1], NULL, 0) != 0
		if (!strcmp(argv[0], "effect"))
		{
			int effectnameindex;
			checkparms(2);
			if (numparticleeffectinfo >= MAX_PARTICLEEFFECTINFO)
			{
				Con_Printf("%s:%i: too many effects!\n", filename, linenumber);
				break;
			}
			for (effectnameindex = 1;effectnameindex < MAX_PARTICLEEFFECTNAME;effectnameindex++)
			{
				if (particleeffectname[effectnameindex][0])
				{
					if (!strcmp(particleeffectname[effectnameindex], argv[1]))
						break;
				}
				else
				{
					dp_strlcpy(particleeffectname[effectnameindex], argv[1], sizeof(particleeffectname[effectnameindex]));
					break;
				}
			}
			// if we run out of names, abort
			if (effectnameindex == MAX_PARTICLEEFFECTNAME)
			{
				Con_Printf("%s:%i: too many effects!\n", filename, linenumber);
				break;
			}
			for(i = 0; i < numparticleeffectinfo; ++i)
			{
				info = particleeffectinfo + i;
				if(!(info->flags & PARTICLEEFFECT_DEFINED))
					if(info->effectnameindex == effectnameindex)
						break;
			}
			if(i < numparticleeffectinfo)
				continue;
			info = particleeffectinfo + numparticleeffectinfo++;
			// copy entire info from baseline, then fix up the nameindex
			*info = baselineparticleeffectinfo;
			info->effectnameindex = effectnameindex;
			continue;
		}
		else if (info == NULL)
		{
			Con_Printf("%s:%i: command %s encountered before effect\n", filename, linenumber, argv[0]);
			break;
		}

		info->flags |= PARTICLEEFFECT_DEFINED;
		if (!strcmp(argv[0], "countabsolute")) {readfloat(info->countabsolute);}
		else if (!strcmp(argv[0], "count")) {readfloat(info->countmultiplier);}
		else if (!strcmp(argv[0], "type"))
		{
			checkparms(2);
			if (!strcmp(argv[1], "alphastatic")) info->particletype = pt_alphastatic;
			else if (!strcmp(argv[1], "static")) info->particletype = pt_static;
			else if (!strcmp(argv[1], "spark")) info->particletype = pt_spark;
			else if (!strcmp(argv[1], "beam")) info->particletype = pt_beam;
			else if (!strcmp(argv[1], "rain")) info->particletype = pt_rain;
			else if (!strcmp(argv[1], "raindecal")) info->particletype = pt_raindecal;
			else if (!strcmp(argv[1], "snow")) info->particletype = pt_snow;
			else if (!strcmp(argv[1], "bubble")) info->particletype = pt_bubble;
			else if (!strcmp(argv[1], "blood")) {info->particletype = pt_blood;info->gravity = 1;}
			else if (!strcmp(argv[1], "smoke")) info->particletype = pt_smoke;
			else if (!strcmp(argv[1], "decal")) info->particletype = pt_decal;
			else if (!strcmp(argv[1], "entityparticle")) info->particletype = pt_entityparticle;
			else Con_Printf("%s:%i: unrecognized particle type %s\n", filename, linenumber, argv[1]);
			info->blendmode = particletype[info->particletype].blendmode;
			info->orientation = particletype[info->particletype].orientation;
		}
		else if (!strcmp(argv[0], "blend"))
		{
			checkparms(2);
			if (!strcmp(argv[1], "alpha")) info->blendmode = PBLEND_ALPHA;
			else if (!strcmp(argv[1], "add")) info->blendmode = PBLEND_ADD;
			else if (!strcmp(argv[1], "invmod")) info->blendmode = PBLEND_INVMOD;
			else if (!strcmp(argv[1], "refract")) info->blendmode = PBLEND_REFRACT;   // BEAUTY A5
			else Con_Printf("%s:%i: unrecognized blendmode %s\n", filename, linenumber, argv[1]);
		}
		else if (!strcmp(argv[0], "orientation"))
		{
			checkparms(2);
			if (!strcmp(argv[1], "billboard")) info->orientation = PARTICLE_BILLBOARD;
			else if (!strcmp(argv[1], "spark")) info->orientation = PARTICLE_SPARK;
			else if (!strcmp(argv[1], "oriented")) info->orientation = PARTICLE_ORIENTED_DOUBLESIDED;
			else if (!strcmp(argv[1], "beam")) info->orientation = PARTICLE_HBEAM;
			else Con_Printf("%s:%i: unrecognized orientation %s\n", filename, linenumber, argv[1]);
		}
		else if (!strcmp(argv[0], "color")) {readints(info->color, 2);}
		else if (!strcmp(argv[0], "tex")) {readints(info->tex, 2);}
		else if (!strcmp(argv[0], "size")) {readfloats(info->size, 2);}
		else if (!strcmp(argv[0], "sizeincrease")) {readfloat(info->size[2]);}
		else if (!strcmp(argv[0], "alpha")) {readfloats(info->alpha, 3);}
		else if (!strcmp(argv[0], "time")) {readfloats(info->time, 2);}
		else if (!strcmp(argv[0], "delay")) {readfloats(info->delay, 2);}   // SEPTEMBER S5
		else if (!strcmp(argv[0], "requirecvar")) {checkparms(2);dp_strlcpy(info->requirecvar, argv[1], sizeof(info->requirecvar));}   // BEAUTY A6
		else if (!strcmp(argv[0], "gravity")) {readfloat(info->gravity);}
		else if (!strcmp(argv[0], "bounce")) {readfloat(info->bounce);}
		else if (!strcmp(argv[0], "airfriction")) {readfloat(info->airfriction);}
		else if (!strcmp(argv[0], "liquidfriction")) {readfloat(info->liquidfriction);}
		else if (!strcmp(argv[0], "originoffset")) {readfloats(info->originoffset, 3);}
		else if (!strcmp(argv[0], "relativeoriginoffset")) {readfloats(info->relativeoriginoffset, 3);}
		else if (!strcmp(argv[0], "velocityoffset")) {readfloats(info->velocityoffset, 3);}
		else if (!strcmp(argv[0], "relativevelocityoffset")) {readfloats(info->relativevelocityoffset, 3);}
		else if (!strcmp(argv[0], "originjitter")) {readfloats(info->originjitter, 3);}
		else if (!strcmp(argv[0], "velocityjitter")) {readfloats(info->velocityjitter, 3);}
		else if (!strcmp(argv[0], "velocitymultiplier")) {readfloat(info->velocitymultiplier);}
		else if (!strcmp(argv[0], "lightradius")) {readfloat(info->lightradiusstart);}
		else if (!strcmp(argv[0], "lightradiusfade")) {readfloat(info->lightradiusfade);}
		else if (!strcmp(argv[0], "lighttime")) {readfloat(info->lighttime);}
		else if (!strcmp(argv[0], "lightcolor")) {readfloats(info->lightcolor, 3);}
		else if (!strcmp(argv[0], "lightshadow")) {readbool(info->lightshadow);}
		else if (!strcmp(argv[0], "lightcubemapnum")) {readint(info->lightcubemapnum);}
		else if (!strcmp(argv[0], "lightcorona")) {readints(info->lightcorona, 2);}
		else if (!strcmp(argv[0], "underwater")) {checkparms(1);info->flags |= PARTICLEEFFECT_UNDERWATER;}
		else if (!strcmp(argv[0], "notunderwater")) {checkparms(1);info->flags |= PARTICLEEFFECT_NOTUNDERWATER;}
		else if (!strcmp(argv[0], "trailspacing")) {readfloat(info->trailspacing);if (info->trailspacing > 0) info->countmultiplier = 1.0f / info->trailspacing;}
		else if (!strcmp(argv[0], "stretchfactor")) {readfloat(info->stretchfactor);}
		else if (!strcmp(argv[0], "staincolor")) {readints(info->staincolor, 2);}
		else if (!strcmp(argv[0], "stainalpha")) {readfloats(info->stainalpha, 2);}
		else if (!strcmp(argv[0], "stainsize")) {readfloats(info->stainsize, 2);}
		else if (!strcmp(argv[0], "staintex")) {readints(info->staintex, 2);}
		else if (!strcmp(argv[0], "stainless")) {info->staintex[0] = -2; info->staincolor[0] = (unsigned int)-1; info->staincolor[1] = (unsigned int)-1; info->stainalpha[0] = 1; info->stainalpha[1] = 1; info->stainsize[0] = 2; info->stainsize[1] = 2; }
		else if (!strcmp(argv[0], "rotate")) {readfloats(info->rotate, 4);}
		else if (!strcmp(argv[0], "forcenearest")) {checkparms(1);info->flags |= PARTICLEEFFECT_FORCENEAREST;}
		else
			Con_Printf("%s:%i: skipping unknown command %s\n", filename, linenumber, argv[0]);
#undef checkparms
#undef readints
#undef readfloats
#undef readint
#undef readfloat
	}
}

int CL_ParticleEffectIndexForName(const char *name)
{
	int i;
	for (i = 1;i < MAX_PARTICLEEFFECTNAME && particleeffectname[i][0];i++)
		if (!strcmp(particleeffectname[i], name))
			return i;
	return 0;
}

const char *CL_ParticleEffectNameForIndex(int i)
{
	if (i < 1 || i >= MAX_PARTICLEEFFECTNAME)
		return NULL;
	return particleeffectname[i];
}

// MUST match effectnameindex_t in client.h
static const char *standardeffectnames[EFFECT_TOTAL] =
{
	"",
	"TE_GUNSHOT",
	"TE_GUNSHOTQUAD",
	"TE_SPIKE",
	"TE_SPIKEQUAD",
	"TE_SUPERSPIKE",
	"TE_SUPERSPIKEQUAD",
	"TE_WIZSPIKE",
	"TE_KNIGHTSPIKE",
	"TE_EXPLOSION",
	"TE_EXPLOSIONQUAD",
	"TE_TAREXPLOSION",
	"TE_TELEPORT",
	"TE_LAVASPLASH",
	"TE_SMALLFLASH",
	"TE_FLAMEJET",
	"EF_FLAME",
	"TE_BLOOD",
	"TE_SPARK",
	"TE_PLASMABURN",
	"TE_TEI_G3",
	"TE_TEI_SMOKE",
	"TE_TEI_BIGEXPLOSION",
	"TE_TEI_PLASMAHIT",
	"EF_STARDUST",
	"TR_ROCKET",
	"TR_GRENADE",
	"TR_BLOOD",
	"TR_WIZSPIKE",
	"TR_SLIGHTBLOOD",
	"TR_KNIGHTSPIKE",
	"TR_VORESPIKE",
	"TR_NEHAHRASMOKE",
	"TR_NEXUIZPLASMA",
	"TR_GLOWTRAIL",
	"SVC_PARTICLE"
};

static void CL_Particles_LoadEffectInfo(const char *customfile)
{
	int i;
	int filepass;
	unsigned char *filedata;
	fs_offset_t filesize;
	char filename[MAX_QPATH];
	numparticleeffectinfo = 0;
	memset(particleeffectinfo, 0, sizeof(particleeffectinfo));
	memset(particleeffectname, 0, sizeof(particleeffectname));
	for (i = 0;i < EFFECT_TOTAL;i++)
		dp_strlcpy(particleeffectname[i], standardeffectnames[i], sizeof(particleeffectname[i]));
	for (filepass = 0;;filepass++)
	{
		if (filepass == 0)
		{
			if (customfile)
				dp_strlcpy(filename, customfile, sizeof(filename));
			else
				dp_strlcpy(filename, "effectinfo.txt", sizeof(filename));
		}
		else if (filepass == 1)
		{
			if (!cl.worldbasename[0] || customfile)
				continue;
			dpsnprintf(filename, sizeof(filename), "%s_effectinfo.txt", cl.worldnamenoextension);
		}
		else
			break;
		filedata = FS_LoadFile(filename, tempmempool, true, &filesize);
		if (!filedata)
			continue;
		CL_Particles_ParseEffectInfo((const char *)filedata, (const char *)filedata + filesize, filename);
		Mem_Free(filedata);
	}
}

static void CL_Particles_LoadEffectInfo_f(cmd_state_t *cmd)
{
	CL_Particles_LoadEffectInfo(Cmd_Argc(cmd) > 1 ? Cmd_Argv(cmd, 1) : NULL);
}

/*
===============
M5 muzzle sparks (BEAUTY A2 + Seb's QA, 2026-09-15)

Seb on the first spray: "too slender and weedy ... dont stick to the gun barrel
(like they're drawn off centre and also obviously seem to drift when we're
strafing)". Three causes, each visible on a scripted strafing demo: the spray
started at a fixed point in the PLAYER MODEL's frame, which projects near the
middle of the screen well above the view weapon's barrel tip; it was placed at
packet parse, a frame before the view weapon moves; and the sparks never took
the shooter's velocity, so a strafe slid the gun out from under them. The local
player's spray is now OWED to the relink window beside the flash, spawned at the
view weapon's own muzzle (the flash's mesh read) along the barrel's own axis,
with the player's velocity added, as a real gun's sparks carry the gun's motion.
The base values are fuller than the first cut -- thicker, more of them, shorter
streaks -- and every one is a console scale (m5_muzzleflash_sparks_*).
===============
*/
#define M5_SPARKS_SG		8		// sparks per shotgun shot at m5_muzzleflash_sparks 1
#define M5_SPARKS_SSG		14		// per super shotgun shot
#define M5_SPARKS_MAX		128		// the local spray's own pool: ten shots of the heaviest setting
#define M5_SPARKS_THICK		1.4f	// streak thickness (the first cut's 0.5 was "slender and weedy")
#define M5_SPARKS_STRETCH	0.5f	// streak length per unit speed (PARTICLE_SPARK draws 0.08 x speed x stretch)
#define M5_SPARKS_PUSH		320.0f	// along the barrel, world units a second
#define M5_SPARKS_LIFT		10.0f	// a little up the gun's own up axis
#define M5_SPARKS_JITTER	150.0f	// scatter about the push
#define M5_SPARKS_ORGJITTER	1.0f	// spawn scatter at the tip; the tip is close to the eye
#define M5_SPARKS_ALPHA_MIN	255
#define M5_SPARKS_ALPHA_MAX	400		// the local spray skips the additive clamp, so this is above white
#define M5_SPARKS_FADE		1300.0f	// alpha a second
#define M5_SPARKS_LIFE		0.3f	// seconds
#define M5_SPARKS_GRAVITY	0.3f	// share of the world's gravity

typedef struct m5sparkknobs_s
{
	int n;
	float speed, spread, stretch, thick, life, bright;
}
m5sparkknobs_t;

// every knob a scale on the base values; speed scales the scatter with the push
// so the fan keeps its shape, life scales the fade inversely so the sparks fade
// over the new life
static void M5_MuzzleSparks_Knobs(m5sparkknobs_t *k, int shells)
{
	float count = bound(0.0f, m5_muzzleflash_sparks.value, 10.0f);
	k->speed = bound(0.1f, m5_muzzleflash_sparks_speed.value, 5.0f);
	k->spread = bound(0.0f, m5_muzzleflash_sparks_spread.value, 10.0f);
	k->stretch = bound(0.0f, m5_muzzleflash_sparks_length.value, 10.0f);
	k->thick = bound(0.1f, m5_muzzleflash_sparks_thickness.value, 10.0f);
	k->life = bound(0.1f, m5_muzzleflash_sparks_life.value, 10.0f);
	k->bright = bound(0.0f, m5_muzzleflash_sparks_brightness.value, 10.0f);
	k->n = (int)((shells == 2 ? M5_SPARKS_SSG : M5_SPARKS_SG) * count + 0.5f);
}

// A chase camera has no view weapon: the spray leaves the player model's muzzle
// point as ordinary world particles, carrying the player's velocity.
static void M5_MuzzleSparks_SpawnWorld(const vec3_t org, const vec3_t fwd, const vec3_t up, int shells)
{
	m5sparkknobs_t k;
	int i;
	vec3_t vel, inherit;
	M5_MuzzleSparks_Knobs(&k, shells);
	if (cl.movement_predicted)
		VectorCopy(cl.movement_velocity, inherit);
	else
		VectorCopy(cl.velocity, inherit);
	for (i = 0; i < 3; i++)
		vel[i] = (fwd[i] * M5_SPARKS_PUSH + up[i] * M5_SPARKS_LIFT) * k.speed + inherit[i];
	for (i = 0; i < k.n; i++)
		CL_NewParticle(org, pt_spark, 0xFFE8B0, 0xFFA030, tex_sparkhot, M5_SPARKS_THICK * k.thick, 0, lhrandom(M5_SPARKS_ALPHA_MIN, M5_SPARKS_ALPHA_MAX) * k.bright, M5_SPARKS_FADE / k.life, M5_SPARKS_GRAVITY, 0, org[0], org[1], org[2], vel[0], vel[1], vel[2], 0, 0, M5_SPARKS_ORGJITTER, M5_SPARKS_JITTER * k.spread * k.speed, true, M5_SPARKS_LIFE * k.life, M5_SPARKS_STRETCH * k.stretch, PBLEND_ADD, PARTICLE_SPARK, -1, -1, -1, 1, 1, 0, 0, NULL);
}

// The LOCAL spray lives in the view weapon's own frame (x along the barrel, y
// left, z up) and is drawn each frame as one-frame particles through the
// weapon's current matrix -- the flash's shape. So it moves with the gun
// whatever the player does: a strafe carries it, the bob carries it, and each
// streak is drawn along its motion relative to the barrel rather than along a
// world velocity that includes the strafe (the first re-anchor, which spawned
// world particles at the muzzle with the player's velocity added, kept the
// sparks at the tip but slanted every streak sideways on a strafe).
typedef struct m5spark_s
{
	float org[3], vel[3];
	float alpha, alphafade, size, stretch;
	double die;
	int color;
}
m5spark_t;
static m5spark_t m5_spark[M5_SPARKS_MAX];
static int m5_nsparks;
static double m5_sparks_time;
static int m5_sparks_owed;			// shells whose spray is owed to the relink window; 0 = none

static void M5_MuzzleSparks_Birth(const vec3_t muzzle, int shells)
{
	m5sparkknobs_t k;
	int i, l1, l2;
	float j[3];
	M5_MuzzleSparks_Knobs(&k, shells);
	for (i = 0; i < k.n && m5_nsparks < M5_SPARKS_MAX; i++)
	{
		m5spark_t *sp = &m5_spark[m5_nsparks++];
		VectorRandom(j);
		VectorMA(muzzle, M5_SPARKS_ORGJITTER, j, sp->org);
		VectorRandom(j);
		sp->vel[0] = M5_SPARKS_PUSH * k.speed + j[0] * M5_SPARKS_JITTER * k.spread * k.speed;
		sp->vel[1] = j[1] * M5_SPARKS_JITTER * k.spread * k.speed;
		sp->vel[2] = M5_SPARKS_LIFT * k.speed + j[2] * M5_SPARKS_JITTER * k.spread * k.speed;
		sp->alpha = lhrandom(M5_SPARKS_ALPHA_MIN, M5_SPARKS_ALPHA_MAX) * k.bright;
		sp->alphafade = M5_SPARKS_FADE / k.life;
		sp->die = cl.time + M5_SPARKS_LIFE * k.life;
		sp->size = M5_SPARKS_THICK * k.thick;
		sp->stretch = M5_SPARKS_STRETCH * k.stretch;
		// the white-hot to orange lerp CL_NewParticle would make, fixed at birth
		l2 = (int)lhrandom(0.5, 256.5);
		l1 = 256 - l2;
		sp->color = ((((0xFF * l1 + 0xFF * l2) >> 8) & 0xFF) << 16) | ((((0xE8 * l1 + 0xA0 * l2) >> 8) & 0xFF) << 8) | (((0xB0 * l1 + 0x30 * l2) >> 8) & 0xFF);
	}
}

/*
===============
M5_ShotgunShellEject

Doom shotgun visuals: a brass shell (two for the super shotgun) tumbles out
to the player's right, plus a short burst of muzzle sparks.  Called from
CL_MoveLerpEntityStates on the rising edge of EF_MUZZLEFLASH; the gating on
m5_shotgun/active weapon lives in M5_ShotgunBoosted.
===============
*/
void M5_ShotgunShellEject(entity_t *ent)
{
	int i, shells, shelltex, shellcolor1, shellcolor2;
	vec3_t shellofs = {16, -7, 20}, shellvel = {24, -70, 70}, sparkofs = {22, -2, 18}, sparkvel = {260, 0, 20};
	vec3_t org, vel, sorg, svel;

	if (!M5_ShotgunBoosted(ent) || !cl_particles.integer)
		return;

	Matrix4x4_Transform(&ent->render.matrix, shellofs, org);
	Matrix4x4_Transform3x3(&ent->render.matrix, shellvel, vel);
	shells = (cl.stats[STAT_ACTIVEWEAPON] == IT_SUPER_SHOTGUN) ? 2 : 1;
	// SEPTEMBER S6: the shaped hull (cell 34) or the round blob it always was.
	// Hoisted ABOVE the loop on purpose: the argument list below ends in two
	// lhrandom() calls and C leaves argument evaluation order unspecified, so a
	// sub-expression inside the list could reorder rand() consumption and move
	// the byte gate for a reason unrelated to the feature. Everything else in
	// the call is untouched -- the angle and spin were always there. And the
	// count is deliberately NOT scaled by cl_particles_quality: Seb runs 3, and
	// three casings per barrel would be absurd.
	shelltex = m5_shotgun_casing.integer ? tex_shellcasing : tex_particle;
	// The cell carries its own red and brass, so the on-arm's vertex colour is
	// near-white (a slight per-casing lerp); the blob keeps the old brass lerp.
	shellcolor1 = m5_shotgun_casing.integer ? 0xF0F0F0 : 0x907030;
	shellcolor2 = m5_shotgun_casing.integer ? 0xFFFFFF : 0xD8A848;
	for (i = 0; i < shells; i++)
		CL_NewParticle(org, pt_alphastatic, shellcolor1, shellcolor2, shelltex, 1.3f, 0, 255, 0, 1, 0.6f, org[0], org[1], org[2], vel[0], vel[1], vel[2], 0.5f, 8, 1, 30, false, 1.6f, 1, PBLEND_ALPHA, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, lhrandom(0, 360), lhrandom(-400, 400), NULL);

	Matrix4x4_Transform(&ent->render.matrix, sparkofs, sorg);
	Matrix4x4_Transform3x3(&ent->render.matrix, sparkvel, svel);
	if (m5_muzzleflash.integer)
	{
		// BEAUTY A2 (2026-09-13): the FINE spray. Seb's demo45 f954: the five
		// warm sparks below, drawn with the round blob at thickness 1.4 and a
		// 21-unit stretch, were the wide yellow gaussian rays he called soft.
		// Same sparks, the hot-core streak cell, a third of the thickness, a
		// wider fan, a shorter life -- thin bright lines that die fast.
		// Seb, 2026-09-15: the local player's spray is owed to the relink window
		// (M5_MuzzleSparks_Update), which births it at the view weapon's own
		// muzzle in the weapon's own frame -- see the block comment above. A
		// chase camera has no view weapon: the model's muzzle point, at once.
		if (!chase_active.integer)
			m5_sparks_owed = shells;
		else
		{
			vec3_t fwd, left, up, pos;
			Matrix4x4_ToVectors(&ent->render.matrix, fwd, left, up, pos);
			VectorNormalize(fwd);
			VectorNormalize(up);
			M5_MuzzleSparks_SpawnWorld(sorg, fwd, up, shells);
		}
	}
	else
	for (i = 0; i < 5; i++)
		CL_NewParticle(sorg, pt_spark, 0xFFD080, 0xFF9020, tex_particle, 1.4f, 0, lhrandom(180, 300), 1500, 0.3f, 0, sorg[0], sorg[1], sorg[2], svel[0], svel[1], svel[2], 0, 0, 2, 90, true, 0.2f, 1, PBLEND_ADD, PARTICLE_SPARK, -1, -1, -1, 1, 1, 0, 0, NULL);
}

/*
===============
M5 muzzle flash (BEAUTY A3, 2026-09-13)

Quake draws no muzzle flash: EF_MUZZLEFLASH is a dynamic light and nothing else
(cl_main.c, the muzzleflashorigin block), which is why the "flare" on Seb's
demo45 f540 is the murk scattering that light round a barrel with nothing at its
tip. These spawn one: a starburst cell (37) tinted per weapon plus a small white
core (the round blob), additive, alpha above 255 so the float scene buffer
carries it above white, gone in ~80 ms.

Two anchorings, on purpose. The LOCAL PLAYER's flash must sit on the VIEW WEAPON,
whose muzzle is viewmodelmatrix_withbob (built by V_CalcRefdef) plus a
per-weapon offset in the bolt's own convention (r_lightning.c: x forward, y
LEFT, z up) -- and that matrix is a frame stale at packet-parse time, where the
rising edge fires, so the arm only RECORDS the weapon and the spawn happens in
the relink window beside CL_Dust_Update, where the matrix is this frame's.
Every OTHER entity (a grunt, an enforcer, another player) flashes at once, at
the muzzle light's own point (18,0,0) in entity space, like the casing.

The view weapon is drawn into the bottom sixteenth of the depth buffer, so a
particle at the muzzle is hidden wherever the barrel's pixels cover it; the
offsets put the burst at the barrel's TIP, where it emerges from the metal.
===============
*/
#define M5_FLASH_LIFE 0.07		// seconds the flash lives; a real one is shorter, this one has to be seen
static int m5_flash_weapon;			// the local player's weapon at the rising edge; 0 = nothing owed
static double m5_flash_start;		// cl.time of that edge
static float m5_flash_angle;		// the burst's rotation, held for the flash's life (a re-roll per frame is a strobe of shapes)
static int m5_flash_side;			// 0 = the forward-most tip; +1 the left barrel (model y > 0), -1 the right
static int m5_flash_printed;

// k: brightness 1..0 over the flash's life. `oneframe` particles live 1 ms --
// spawned in the relink window and drawn that frame, removed by the next
// frame's update -- so a flash that FOLLOWS the recoiling gun is one fresh
// particle per frame at that frame's muzzle, and never stacks whatever the
// frame rate. Seb's demo45 f539: the first cut spawned one 80 ms particle and
// the shotgun's kick lifted the barrel over it on the very next frame.
static void M5_MuzzleFlash_Spawn(const vec3_t org, float size, int col1, int col2, float k, float ang, qbool oneframe)
{
	float life = oneframe ? 0.001f : M5_FLASH_LIFE;
	float fade = oneframe ? 0.0f : 560.0f / M5_FLASH_LIFE;
	particle_t *p;
	// the burst: the starburst cell, its own hot centre, tinted
	p = CL_NewParticle(org, pt_static, col1, col2, tex_flash, size * (0.8f + 0.3f * k), 0, 560 * k, fade, 0, 0, org[0], org[1], org[2], 0, 0, 0, 0, 0, 0, 0, false, life, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, ang, 0, NULL);
	// the LOCAL flash is drawn OVER the view weapon, depth test off: the gun
	// draws into the bottom sixteenth of the depth buffer and beats any world
	// particle, so a burst centred on the bore was hidden by the barrel's end
	// face entirely (Seb's demo45, three attempts), and in the gun's own range
	// it was still behind the end face by construction; a weapon flash is drawn
	// over the weapon everywhere, and the mesh keeps its position honest
	if (p && oneframe) p->viewmodel = 1;
	if (getenv("M5_FLASHDEBUG"))
		Con_Printf("M5 flash dbg: burst %s size %.2f alpha %.0f tex %d vm %d die %.4f now %.4f\n", p ? "ok" : "NULL", p ? p->size : 0.0f, p ? p->alpha : 0.0f, p ? p->texnum : -1, p ? p->viewmodel : -1, p ? p->die : 0.0, cl.time);
	// a TINY white core -- the bore's heat -- not a blob: the round cell at a
	// third of the burst was itself the soft disc the first cut showed
	p = CL_NewParticle(org, pt_static, 0xFFFFFF, 0xFFF4E4, tex_particle, size * 0.13f, 0, 700 * k, fade, 0, 0, org[0], org[1], org[2], 0, 0, 0, 0, 0, 0, 0, false, life, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
	if (p && oneframe) p->viewmodel = 1;
	if (!m5_flash_printed)
	{
		m5_flash_printed = 1;
		Con_DPrintf("M5 muzzle flash: armed (cells %d/%d)\n", tex_flash, tex_sparkhot);
	}
}

// Burst half-size in world units and the tint, per weapon. The thunderbolt,
// the ball and the axe draw nothing of this kind.
static qbool M5_MuzzleFlash_ViewWeapon(int weapon, float *size, int *col1, int *col2)
{
	switch (weapon)
	{
	case IT_SHOTGUN:          *size = 6.0f; *col1 = 0xFFD8A0; *col2 = 0xFFB060; break;
	case IT_SUPER_SHOTGUN:    *size = 7.5f; *col1 = 0xFFD8A0; *col2 = 0xFFB060; break;
	case IT_NAILGUN:          *size = 3.0f; *col1 = 0xFFF0C8; *col2 = 0xFFD890; break;
	case IT_SUPER_NAILGUN:    *size = 3.5f; *col1 = 0xFFF0C8; *col2 = 0xFFD890; break;
	case IT_GRENADE_LAUNCHER: *size = 4.0f; *col1 = 0xFFE0B0; *col2 = 0xFFC080; break;
	case IT_ROCKET_LAUNCHER:  *size = 6.0f; *col1 = 0xFFC890; *col2 = 0xFF9848; break;
	default: return false;
	}
	*size *= m5_muzzleflash_size.value;
	return true;
}

// The muzzle, read off the VIEW WEAPON'S OWN MESH this frame: animate its
// vertices (the sidecar's entity gather does the same per frame), take the
// forward-most cluster -- the barrel's end face -- and put it through the
// weapon's matrix. A fixed per-weapon offset was tried first and failed on
// Seb's demo45 the frame after each shot: the model's own fire frames lift
// the barrel, and a point that was the tip is behind the metal a frame later
// (the view weapon draws into the bottom sixteenth of the depth buffer, so its
// pixels win over any world particle). The mesh follows every weapon and every
// recoil frame with no table. The cluster is nudged a unit past the tip so the
// burst emerges from the bore rather than being cut by the barrel's silhouette.
static float *m5_flash_verts;
static int m5_flash_nverts;
// side: 0 takes the forward-most tip; +1 / -1 takes the forward-most tip of the
// left / right barrel only (model y > 0 / y < 0) -- the nailgun, whose two barrels
// alternate (see M5_MuzzleFlash_Arm).
static qbool M5_MuzzleFlash_ViewMuzzle(vec3_t out, vec3_t localout, int side)
{
	const entity_render_t *e = &cl.viewent.render;
	const model_t *m = e->model;
	int i, n, count;
	float maxx, sum[3], local[3], fwd[3];
	if (!m || !m->AnimateVertices || m->surfmesh.num_vertices < 3)
		return false;
	n = m->surfmesh.num_vertices;
	if (n > m5_flash_nverts)
	{
		if (m5_flash_verts) Mem_Free(m5_flash_verts);
		m5_flash_nverts = n;
		m5_flash_verts = (float *)Mem_Alloc(cls.permanentmempool, n * sizeof(float[3]));
	}
	m->AnimateVertices(m, e->frameblend, e->skeleton, m5_flash_verts, NULL, NULL, NULL);
	maxx = -1e30f;
	for (i = 0; i < n; i++)
		if (m5_flash_verts[i*3] > maxx && (!side || m5_flash_verts[i*3+1] * side > 0.5f)) maxx = m5_flash_verts[i*3];
	VectorClear(sum); count = 0;
	for (i = 0; i < n; i++)
		if (m5_flash_verts[i*3] > maxx - 2.5f && (!side || m5_flash_verts[i*3+1] * side > 0.5f))
		{
			VectorAdd(sum, m5_flash_verts + i*3, sum);
			count++;
		}
	if (!count)
		return false;
	VectorScale(sum, 1.0f / count, local);
	local[0] += 1.0f + m5_muzzleflash_forward.value;
	local[2] += m5_muzzleflash_up.value;
	if (localout)
		VectorCopy(local, localout);
	Matrix4x4_Transform(&e->matrix, local, out);
	// the matrix is identity with no view model drawn (chase cam, intermission):
	// a flash at the world origin would be spectacular -- the bolt's own guard
	Matrix4x4_OriginFromMatrix(&e->matrix, fwd);
	if (getenv("M5_FLASHDEBUG"))
		Con_Printf("M5 flash dbg: model %s verts %d maxx %.1f count %d local %.1f %.1f %.1f -> world %.1f %.1f %.1f gunorg %.1f %.1f %.1f eye %.1f %.1f %.1f d2 %.0f\n", m->name, n, maxx, count, local[0], local[1], local[2], out[0], out[1], out[2], fwd[0], fwd[1], fwd[2], r_refdef.view.origin[0], r_refdef.view.origin[1], r_refdef.view.origin[2], VectorDistance2(out, fwd));
	return VectorDistance2(out, fwd) < 4096.0f;
}

void M5_MuzzleFlash_Arm(entity_t *ent)
{
	vec3_t local, org;
	if (!m5_muzzleflash.integer || !cl_particles.integer || cls.state != ca_connected || m5_stock.integer)
		return;
	if (ent == cl.entities + cl.viewentity && !chase_active.integer)
	{
		m5_flash_weapon = cl.stats[STAT_ACTIVEWEAPON];		// spawned frame by frame from the relink window
		m5_flash_start = cl.time;
		m5_flash_angle = lhrandom(0, 360);
		// The nailgun's two barrels alternate: the QuakeC fires the RIGHT one on
		// odd weapon frames and the LEFT on even ones (player.qc W_FireSpikes 4 /
		// -4), and its model pushes the nail out of the same barrel on the same
		// frames (v_nail.mdl, AMI and id). The forward-most tip lags a frame
		// behind that -- the model is still lerping out of the last shot's frame
		// when the new one arrives -- so the first, brightest flash frame used to
		// land on the barrel that fired LAST. The side comes from the frame.
		m5_flash_side = (m5_flash_weapon == IT_NAILGUN && cl.stats[STAT_WEAPONFRAME] > 0) ? ((cl.stats[STAT_WEAPONFRAME] & 1) ? -1 : 1) : 0;
		return;
	}
	VectorSet(local, 18, 0, 0);
	Matrix4x4_Transform(&ent->render.matrix, local, org);
	M5_MuzzleFlash_Spawn(org, 2.6f, 0xFFE0B0, 0xFFC080, 1.0f, lhrandom(0, 360), false);
}

// The local spray, in the relink window, where the view weapon's matrix and
// frameblend are this frame's: birth any spray owed by this frame's shot at the
// mesh's muzzle, advance every spark in the weapon's frame (gravity turned into
// that frame), and draw each as a one-frame particle through the matrix. The
// particle update moves every particle by its velocity before drawing it, by
// the step it is about to take, so each is placed that step back; it takes no
// gravity, fade or friction of its own. Drawn like the flash: over the weapon,
// unfogged, allowed above white.
static void M5_MuzzleSparks_Update(void)
{
	const entity_render_t *e = &cl.viewent.render;
	vec3_t muzzle, world, fwd, left, up, pos, grav, wvel;
	float dt, ft;
	int i, k;
	particle_t *pt;
	if (m5_sparks_owed)
	{
		int shells = m5_sparks_owed;
		m5_sparks_owed = 0;
		if (m5_muzzleflash.integer && cl_particles.integer && cls.state == ca_connected && !chase_active.integer && M5_MuzzleFlash_ViewMuzzle(world, muzzle, 0))
			M5_MuzzleSparks_Birth(muzzle, shells);
	}
	dt = bound(0.0f, (float)(cl.time - m5_sparks_time), 0.1f);
	m5_sparks_time = cl.time;
	if (!m5_nsparks)
		return;
	if (!m5_muzzleflash.integer || !cl_particles.integer || cls.state != ca_connected || chase_active.integer || !e->model)
	{
		m5_nsparks = 0;
		return;
	}
	Matrix4x4_ToVectors(&e->matrix, fwd, left, up, pos);
	VectorNormalize(fwd);
	VectorNormalize(left);
	VectorNormalize(up);
	grav[0] = -cl.movevars_gravity * M5_SPARKS_GRAVITY * fwd[2];
	grav[1] = -cl.movevars_gravity * M5_SPARKS_GRAVITY * left[2];
	grav[2] = -cl.movevars_gravity * M5_SPARKS_GRAVITY * up[2];
	ft = bound(0.0f, (float)(cl.time - cl.particles_updatetime), 1.0f);
	for (i = 0, k = 0; i < m5_nsparks; i++)
	{
		m5spark_t *sp = &m5_spark[i];
		sp->alpha -= sp->alphafade * dt;
		if (sp->alpha <= 0 || cl.time >= sp->die)
			continue;
		VectorMA(sp->org, dt, sp->vel, sp->org);
		VectorMA(sp->vel, dt, grav, sp->vel);
		if (k != i)
			m5_spark[k] = *sp;
		sp = &m5_spark[k++];
		Matrix4x4_Transform(&e->matrix, sp->org, world);
		Matrix4x4_Transform3x3(&e->matrix, sp->vel, wvel);
		VectorMA(world, -ft, wvel, world);
		pt = CL_NewParticle(world, pt_spark, sp->color, sp->color, tex_sparkhot, sp->size, 0, sp->alpha, 0, 0, 0, world[0], world[1], world[2], wvel[0], wvel[1], wvel[2], 0, 0, 0, 0, false, 0.001f, sp->stretch, PBLEND_ADD, PARTICLE_SPARK, -1, -1, -1, 1, 1, 0, 0, NULL);
		if (pt)
			pt->viewmodel = 1;
	}
	m5_nsparks = k;
}

void M5_MuzzleFlash_Update(void)
{
	vec3_t org;
	float size, age, k;
	int c1, c2;
	M5_MuzzleSparks_Update();
	if (!m5_flash_weapon)
		return;
	age = (float)(cl.time - m5_flash_start);
	if (age < 0.0f || age > M5_FLASH_LIFE || !m5_muzzleflash.integer || !cl_particles.integer || cls.state != ca_connected)
	{
		m5_flash_weapon = 0;
		return;
	}
	if (!M5_MuzzleFlash_ViewWeapon(m5_flash_weapon, &size, &c1, &c2))
	{
		m5_flash_weapon = 0;
		return;
	}
	if (!M5_MuzzleFlash_ViewMuzzle(org, NULL, m5_flash_side))
		return;
	k = 1.0f - age / M5_FLASH_LIFE;
	M5_MuzzleFlash_Spawn(org, size, c1, c2, k, m5_flash_angle + age * 90.0f, true);
}

/*
===============
CL_InitParticles
===============
*/
void CL_ReadPointFile_f(cmd_state_t *cmd);
void CL_Particles_Init (void)
{
	Cmd_AddCommand(CF_CLIENT, "pointfile", CL_ReadPointFile_f, "display point file produced by qbsp when a leak was detected in the map (a line leading through the leak hole, to an entity inside the level)");
	Cmd_AddCommand(CF_CLIENT, "cl_particles_reloadeffects", CL_Particles_LoadEffectInfo_f, "reloads effectinfo.txt and maps/levelname_effectinfo.txt (where levelname is the current map) if parameter is given, loads from custom file (no levelname_effectinfo are loaded in this case)");

	Cvar_RegisterVariable (&cl_particles);
	Cvar_RegisterVariable (&cl_particles_quality);
	Cvar_RegisterVariable (&cl_particles_lighting);
	Cvar_RegisterVariable (&cl_particles_lighting_gain);
	Cvar_RegisterVariable (&cl_particles_lighting_static);
	Cvar_RegisterVariable (&m5_dust);
	Cvar_RegisterVariable (&m5_dust_radius);
	Cvar_RegisterVariable (&m5_dust_size);
	Cvar_RegisterVariable (&m5_dust_alpha);
	Cvar_RegisterVariable (&m5_dust_speed);
	Cvar_RegisterVariable (&m5_muzzleflash);
	Cvar_RegisterVariable (&m5_muzzleflash_forward);
	Cvar_RegisterVariable (&m5_muzzleflash_up);
	Cvar_RegisterVariable (&m5_muzzleflash_size);
	Cvar_RegisterVariable (&m5_muzzleflash_sparks);
	Cvar_RegisterVariable (&m5_muzzleflash_sparks_speed);
	Cvar_RegisterVariable (&m5_muzzleflash_sparks_spread);
	Cvar_RegisterVariable (&m5_muzzleflash_sparks_length);
	Cvar_RegisterVariable (&m5_muzzleflash_sparks_thickness);
	Cvar_RegisterVariable (&m5_muzzleflash_sparks_life);
	Cvar_RegisterVariable (&m5_muzzleflash_sparks_brightness);
	Cvar_RegisterVariable (&m5_dust_swirl);
	Cvar_RegisterVariable (&m5_dust_tint);
	Cvar_RegisterVariable (&cl_particles_alpha);
	Cvar_RegisterVariable (&cl_particles_size);
	Cvar_RegisterVariable (&cl_particles_quake);
	Cvar_RegisterVariable (&cl_particles_blood);
	Cvar_RegisterVariable (&cl_particles_blood_alpha);
	Cvar_RegisterVariable (&cl_particles_texsize);
	Cvar_RegisterVariable (&cl_particles_refract);
	Cvar_RegisterVariable (&cl_particles_scorchglow);
	Cvar_RegisterVariable (&m5_torch_embers);
	Cvar_RegisterVariable (&cl_particles_blood_droplet);
	Cvar_RegisterVariable (&cl_particles_blood_decal_alpha);
	Cvar_RegisterVariable (&cl_particles_blood_decal_scalemin);
	Cvar_RegisterVariable (&cl_particles_blood_decal_scalemax);
	Cvar_RegisterVariable (&cl_particles_blood_bloodhack);
	Cvar_RegisterVariable (&cl_particles_explosions_sparks);
	Cvar_RegisterVariable (&cl_particles_explosions_shell);
	Cvar_RegisterVariable (&cl_particles_bulletimpacts);
	Cvar_RegisterVariable (&cl_particles_rain);
	Cvar_RegisterVariable (&cl_particles_snow);
	Cvar_RegisterVariable (&cl_particles_smoke);
	Cvar_RegisterVariable (&cl_particles_smoke_alpha);
	Cvar_RegisterVariable (&cl_particles_smoke_alphafade);
	Cvar_RegisterVariable (&cl_particles_sparks);
	Cvar_RegisterVariable (&cl_particles_bubbles);
	Cvar_RegisterVariable (&cl_particles_visculling);
	Cvar_RegisterVariable (&cl_particles_collisions);
	Cvar_RegisterVariable (&cl_particles_forcetraileffects);
	Cvar_RegisterVariable (&cl_decals);
	Cvar_RegisterVariable (&cl_decals_time);
	Cvar_RegisterVariable (&cl_decals_fadetime);
	Cvar_RegisterVariable (&cl_decals_newsystem_intensitymultiplier);
	Cvar_RegisterVariable (&cl_decals_newsystem_immediatebloodstain);
	Cvar_RegisterVariable (&cl_decals_newsystem_bloodsmears);
	Cvar_RegisterVariable (&cl_decals_models);
	Cvar_RegisterVariable (&cl_decals_bias);
	Cvar_RegisterVariable (&cl_decals_max);
}

void CL_Particles_Shutdown (void)
{
}

void CL_SpawnDecalParticleForSurface(int hitent, const vec3_t org, const vec3_t normal, int color1, int color2, int texnum, float size, float alpha);
void CL_SpawnDecalParticleForPoint(const vec3_t org, float maxdist, float size, float alpha, int texnum, int color1, int color2);



/**
 * @brief      Creates a new particle and returns a pointer to it
 *
 * @param[in]  sortorigin         ?
 * @param[in]  ptypeindex         Any of the pt_ enum values (pt_static, pt_blood, etc), see ptype_t near the top of this file
 * @param[in]  pcolor1,pcolor2    Minimum and maximum range of color, randomly interpolated with pcolor2 to decide particle color
 * @param[in]  ptex               Any of the tex_ values such as tex_smoke[rand()&7] or tex_particle
 * @param[in]  psize              Size of particle (or thickness for PARTICLE_SPARK and PARTICLE_*BEAM)
 * @param[in]  psizeincrease      ?
 * @param[in]  palpha             Opacity of particle as 0-255 (can be more than 255)
 * @param[in]  palphafade         Rate of fade per second (so 256 would mean a 256 alpha particle would fade to nothing in 1 second)
 * @param[in]  pgravity           How much effect gravity has on the particle (0-1)
 * @param[in]  pbounce            How much bounce the particle has when it hits a surface (0-1), -1 makes a blood splat when it hits a surface, 0 does not even check for collisions
 * @param[in]  px,py,pz           Starting origin of particle
 * @param[in]  pvx,pvy,pvz        Starting velocity of particle
 * @param[in]  pairfriction       How much the particle slows down, in air, per second (0-1 typically, can slowdown faster than 1)
 * @param[in]  pliquidfriction    How much the particle slows down, in liquids, per second (0-1 typically, can slowdown faster than 1)
 * @param[in]  originjitter       ?
 * @param[in]  velocityjitter     ?
 * @param[in]  pqualityreduction  ?
 * @param[in]  lifetime           How long the particle can live (note it is also removed if alpha drops to nothing)
 * @param[in]  stretch            ?
 * @param[in]  blendmode          One of the PBLEND_ values
 * @param[in]  orientation        One of the PARTICLE_ values
 * @param[in]  staincolor1        Minimum and maximum ranges of stain color, randomly interpolated to decide stain color (-1 to use none)
 * @param[in]  staincolor2        Minimum and maximum ranges of stain color, randomly interpolated to decide stain color (-1 to use none)
 * @param[in]  staintex           Any of the tex_ values such as tex_smoke[rand()&7] or tex_particle
 * @param[in]  angle              Base rotation of the particle geometry around its center normal
 * @param[in]  spin               Rotation speed of the particle geometry around its center normal
 * @param[in]  tint               The tint
 *
 * @return     Pointer to the new particle
 */
particle_t *CL_NewParticle(
	const vec3_t sortorigin,
	unsigned short ptypeindex,
	int pcolor1, int pcolor2,
	int ptex,
	float psize,
	float psizeincrease,
	float palpha,
	float palphafade,
	float pgravity,
	float pbounce,
	float px, float py, float pz,
	float pvx, float pvy, float pvz,
	float pairfriction,
	float pliquidfriction,
	float originjitter,
	float velocityjitter,
	qbool pqualityreduction,
	float lifetime,
	float stretch,
	pblend_t blendmode,
	porientation_t orientation,
	int staincolor1,
	int staincolor2,
	int staintex,
	float stainalpha,
	float stainsize,
	float angle,
	float spin,
	float tint[4])
{
	int l1, l2, r, g, b;
	particle_t *part;
	vec3_t v;
	if (!cl_particles.integer)
		return NULL;
	for (;cl.free_particle < cl.max_particles && cl.particles[cl.free_particle].typeindex;cl.free_particle++);
	if (cl.free_particle >= cl.max_particles)
		return NULL;
	if (!lifetime)
		lifetime = palpha / min(1, palphafade);
	part = &cl.particles[cl.free_particle++];
	if (cl.num_particles < cl.free_particle)
		cl.num_particles = cl.free_particle;
	memset(part, 0, sizeof(*part));
	VectorCopy(sortorigin, part->sortorigin);
	part->typeindex = ptypeindex;
	part->blendmode = blendmode;
	if(orientation == PARTICLE_HBEAM || orientation == PARTICLE_VBEAM)
	{
		particletexture_t *tex = &particletexture[ptex];
		if(tex->t1 == 0 && tex->t2 == 1) // full height of texture?
			part->orientation = PARTICLE_VBEAM;
		else
			part->orientation = PARTICLE_HBEAM;
	}
	else
		part->orientation = orientation;
	l2 = (int)lhrandom(0.5, 256.5);
	l1 = 256 - l2;
	part->color[0] = ((((pcolor1 >> 16) & 0xFF) * l1 + ((pcolor2 >> 16) & 0xFF) * l2) >> 8) & 0xFF;
	part->color[1] = ((((pcolor1 >>  8) & 0xFF) * l1 + ((pcolor2 >>  8) & 0xFF) * l2) >> 8) & 0xFF;
	part->color[2] = ((((pcolor1 >>  0) & 0xFF) * l1 + ((pcolor2 >>  0) & 0xFF) * l2) >> 8) & 0xFF;
	if (vid.sRGB3D)
	{
		part->color[0] = (unsigned char)floor(Image_LinearFloatFromsRGB(part->color[0]) * 255.0f + 0.5f);
		part->color[1] = (unsigned char)floor(Image_LinearFloatFromsRGB(part->color[1]) * 255.0f + 0.5f);
		part->color[2] = (unsigned char)floor(Image_LinearFloatFromsRGB(part->color[2]) * 255.0f + 0.5f);
	}
	part->alpha = palpha;
	part->alphafade = palphafade;
	part->staintexnum = staintex;
	if(staincolor1 >= 0 && staincolor2 >= 0)
	{
		l2 = (int)lhrandom(0.5, 256.5);
		l1 = 256 - l2;
		if(blendmode == PBLEND_INVMOD)
		{
			r = ((((staincolor1 >> 16) & 0xFF) * l1 + ((staincolor2 >> 16) & 0xFF) * l2) * (255 - part->color[0])) / 0x8000; // staincolor 0x808080 keeps color invariant
			g = ((((staincolor1 >>  8) & 0xFF) * l1 + ((staincolor2 >>  8) & 0xFF) * l2) * (255 - part->color[1])) / 0x8000;
			b = ((((staincolor1 >>  0) & 0xFF) * l1 + ((staincolor2 >>  0) & 0xFF) * l2) * (255 - part->color[2])) / 0x8000;
		}
		else
		{
			r = ((((staincolor1 >> 16) & 0xFF) * l1 + ((staincolor2 >> 16) & 0xFF) * l2) * part->color[0]) / 0x8000; // staincolor 0x808080 keeps color invariant
			g = ((((staincolor1 >>  8) & 0xFF) * l1 + ((staincolor2 >>  8) & 0xFF) * l2) * part->color[1]) / 0x8000;
			b = ((((staincolor1 >>  0) & 0xFF) * l1 + ((staincolor2 >>  0) & 0xFF) * l2) * part->color[2]) / 0x8000;
		}
		if(r > 0xFF) r = 0xFF;
		if(g > 0xFF) g = 0xFF;
		if(b > 0xFF) b = 0xFF;
	}
	else
	{
		r = part->color[0]; // -1 is shorthand for stain = particle color
		g = part->color[1];
		b = part->color[2];
	}
	part->staincolor[0] = r;
	part->staincolor[1] = g;
	part->staincolor[2] = b;
	part->stainalpha = palpha * stainalpha;
	part->stainsize = psize * stainsize;
	if(tint)
	{
		if(blendmode != PBLEND_INVMOD) // invmod is immune to tinting
		{
			part->color[0] *= tint[0];
			part->color[1] *= tint[1];
			part->color[2] *= tint[2];
		}
		part->alpha *= tint[3];
		part->alphafade *= tint[3];
		part->stainalpha *= tint[3];
	}
	part->texnum = ptex;
	part->size = psize;
	part->sizeincrease = psizeincrease;
	part->gravity = pgravity;
	part->bounce = pbounce;
	part->stretch = stretch;
	VectorRandom(v);
	part->org[0] = px + originjitter * v[0];
	part->org[1] = py + originjitter * v[1];
	part->org[2] = pz + originjitter * v[2];
	part->vel[0] = pvx + velocityjitter * v[0];
	part->vel[1] = pvy + velocityjitter * v[1];
	part->vel[2] = pvz + velocityjitter * v[2];
	part->airfriction = pairfriction;
	part->liquidfriction = pliquidfriction;
	part->die = cl.time + lifetime;
	part->viewmodel = 0;
	part->delayedspawn = cl.time;
//	part->delayedcollisions = 0;
	part->qualityreduction = pqualityreduction;
	part->angle = angle;
	part->spin = spin;
	// if it is rain or snow, trace ahead and shut off collisions until an actual collision event needs to occur to improve performance
	if (part->typeindex == pt_rain)
	{
		int i;
		particle_t *part2;
		vec3_t endvec;
		trace_t trace;
		// turn raindrop into simple spark and create delayedspawn splash effect
		part->typeindex = pt_spark;
		part->bounce = 0;
		VectorMA(part->org, lifetime, part->vel, endvec);
		trace = CL_TraceLine(part->org, endvec, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID | SUPERCONTENTS_LIQUIDSMASK, 0, 0, collision_extendmovelength.value, true, false, NULL, false, false);
		part->die = cl.time + lifetime * trace.fraction;
		part2 = CL_NewParticle(endvec, pt_raindecal, pcolor1, pcolor2, tex_rainsplash, part->size, part->size * 20, part->alpha, part->alpha / 0.4, 0, 0, trace.endpos[0] + trace.plane.normal[0], trace.endpos[1] + trace.plane.normal[1], trace.endpos[2] + trace.plane.normal[2], trace.plane.normal[0], trace.plane.normal[1], trace.plane.normal[2], 0, 0, 0, 0, pqualityreduction, 0, 1, PBLEND_ADD, PARTICLE_ORIENTED_DOUBLESIDED, -1, -1, -1, 1, 1, 0, 0, NULL);
		if (part2)
		{
			part2->delayedspawn = part->die;
			part2->die += part->die - cl.time;
			for (i = rand() & 7;i < 10;i++)
			{
				part2 = CL_NewParticle(endvec, pt_spark, pcolor1, pcolor2, tex_particle, 0.25f, 0, part->alpha * 2, part->alpha * 4, 1, 0.1, trace.endpos[0] + trace.plane.normal[0], trace.endpos[1] + trace.plane.normal[1], trace.endpos[2] + trace.plane.normal[2], trace.plane.normal[0] * 16, trace.plane.normal[1] * 16, trace.plane.normal[2] * 16 + cl.movevars_gravity * 0.04, 0, 0, 0, 32, pqualityreduction, 0, 1, PBLEND_ADD, PARTICLE_SPARK, -1, -1, -1, 1, 1, 0, 0, NULL);
				if (part2)
				{
					part2->delayedspawn = part->die;
					part2->die += part->die - cl.time;
				}
			}
		}
	}
	else if (part->typeindex == pt_explode || part->typeindex == pt_explode2)
		part->time2 = rand()&3; // time2 is used to progress the colour ramp index

#if 0
	else if (part->bounce != 0 && part->gravity == 0 && part->typeindex != pt_snow)
	{
		float lifetime = part->alpha / (part->alphafade ? part->alphafade : 1);
		vec3_t endvec;
		trace_t trace;
		VectorMA(part->org, lifetime, part->vel, endvec);
		trace = CL_TraceLine(part->org, endvec, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID | SUPERCONTENTS_BODY, true, false, NULL, false);
		part->delayedcollisions = cl.time + lifetime * trace.fraction - 0.1;
	}
#endif

	return part;
}



/**
 * @brief      Creates a simple particle, a square like Quake, or a disc like GLQuake
 *
 * @param[in]  origin                                                 ?
 * @param[in]  color_1,color_2                                        Minimum and maximum range of color, randomly interpolated with pcolor2 to decide particle color
 * @param[in]  gravity                                                How much effect gravity has on the particle (0-1)
 * @param[in]  offset_x,offset_y,offset_z                             Starting origin of particle
 * @param[in]  velocity_offset_x,velocity_offset_y,velocity_offset_z  Starting velocity of particle
 * @param[in]  air_friction                                           How much the particle slows down, in air, per second (0-1 typically, can slowdown faster than 1)
 * @param[in]  liquid_friction                                        How much the particle slows down, in liquids, per second (0-1 typically, can slowdown faster than 1)
 * @param[in]  origin_jitter                                          ?
 * @param[in]  velocity_jitter                                        ?
 * @param[in]  lifetime                                               How long the particle can live (note it is also removed if alpha drops to nothing)
 *
 * @return     Pointer to the new particle
 */
particle_t *CL_NewQuakeParticle(
	const vec3_t origin,
	const unsigned short ptypeindex,
	const int color_1,
	const int color_2,
	const float gravity,
	const float offset_x,
	const float offset_y,
	const float offset_z,
	const float velocity_offset_x,
	const float velocity_offset_y,
	const float velocity_offset_z,
	const float air_friction,
	const float liquid_friction,
	const float origin_jitter,
	const float velocity_jitter,
	const float lifetime)
{
	int texture;

	// Set the particle texture based on the value of cl_particles_quake; defaulting to the GLQuake disc
	if (cl_particles_quake.integer == 2)
		texture = tex_square;
	else
		texture = tex_particle;

	return CL_NewParticle(
		origin,
		ptypeindex,          // type
		color_1,
		color_2,
		texture,
		0.8f,                // size
		0,                   // size increase
		255,                 // alpha
		0,                   // alpha fade
		gravity,
		0,                   // bounce
		offset_x,
		offset_y,
		offset_z,
		velocity_offset_x,
		velocity_offset_y,
		velocity_offset_z,
		air_friction,
		liquid_friction,
		origin_jitter,
		velocity_jitter,
		true,                // quality reduction
		lifetime,
		1,                   // stretch
		PBLEND_ALPHA,        // blend mode
		PARTICLE_BILLBOARD,  // orientation
		-1,                  // stain color 1
		-1,                  // stain color 2
		-1,                  // stain texture
		1,                   // stain alpha
		1,                   // stain size
		0,                   // angle
		0,                   // spin
		NULL                 // tint
	);
}



static void CL_ImmediateBloodStain(particle_t *part)
{
	vec3_t v;
	int staintex;

	// blood creates a splash at spawn, not just at impact, this makes monsters bloody where they are shot
	if (part->staintexnum >= 0 && cl_decals.integer)
	{
		VectorCopy(part->vel, v);
		VectorNormalize(v);
		staintex = part->staintexnum;
		R_DecalSystem_SplatEntities(part->org, v, 1-part->staincolor[0]*(1.0f/255.0f), 1-part->staincolor[1]*(1.0f/255.0f), 1-part->staincolor[2]*(1.0f/255.0f), part->stainalpha*(1.0f/255.0f), particletexture[staintex].s1, particletexture[staintex].t1, particletexture[staintex].s2, particletexture[staintex].t2, part->stainsize);
	}

	// blood creates a splash at spawn, not just at impact, this makes monsters bloody where they are shot
	if (part->typeindex == pt_blood && cl_decals.integer)
	{
		VectorCopy(part->vel, v);
		VectorNormalize(v);
		staintex = tex_blooddecal[rand()&7];
		R_DecalSystem_SplatEntities(part->org, v, part->color[0]*(1.0f/255.0f), part->color[1]*(1.0f/255.0f), part->color[2]*(1.0f/255.0f), part->alpha*(1.0f/255.0f), particletexture[staintex].s1, particletexture[staintex].t1, particletexture[staintex].s2, particletexture[staintex].t2, part->size * 2);
	}
}

void CL_SpawnDecalParticleForSurface(int hitent, const vec3_t org, const vec3_t normal, int color1, int color2, int texnum, float size, float alpha)
{
	int l1, l2;
	entity_render_t *ent = &cl.entities[hitent].render;
	unsigned char color[3];
	if (!cl_decals.integer)
		return;
	if (!ent->allowdecals)
		return;

	l2 = (int)lhrandom(0.5, 256.5);
	l1 = 256 - l2;
	color[0] = ((((color1 >> 16) & 0xFF) * l1 + ((color2 >> 16) & 0xFF) * l2) >> 8) & 0xFF;
	color[1] = ((((color1 >>  8) & 0xFF) * l1 + ((color2 >>  8) & 0xFF) * l2) >> 8) & 0xFF;
	color[2] = ((((color1 >>  0) & 0xFF) * l1 + ((color2 >>  0) & 0xFF) * l2) >> 8) & 0xFF;

	if (vid.sRGB3D)
		R_DecalSystem_SplatEntities(org, normal, Image_LinearFloatFromsRGB(color[0]), Image_LinearFloatFromsRGB(color[1]), Image_LinearFloatFromsRGB(color[2]), alpha*(1.0f/255.0f), particletexture[texnum].s1, particletexture[texnum].t1, particletexture[texnum].s2, particletexture[texnum].t2, size);
	else
		R_DecalSystem_SplatEntities(org, normal, color[0]*(1.0f/255.0f), color[1]*(1.0f/255.0f), color[2]*(1.0f/255.0f), alpha*(1.0f/255.0f), particletexture[texnum].s1, particletexture[texnum].t1, particletexture[texnum].s2, particletexture[texnum].t2, size);
}

void CL_SpawnDecalParticleForPoint(const vec3_t org, float maxdist, float size, float alpha, int texnum, int color1, int color2)
{
	int i;
	vec_t bestfrac;
	vec3_t bestorg;
	vec3_t bestnormal;
	vec3_t org2;
	int besthitent = 0, hitent;
	trace_t trace;
	bestfrac = 10;
	for (i = 0;i < 32;i++)
	{
		VectorRandom(org2);
		VectorMA(org, maxdist, org2, org2);
		trace = CL_TraceLine(org, org2, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID | SUPERCONTENTS_SKY, 0, 0, collision_extendmovelength.value, true, false, &hitent, false, true);
		// take the closest trace result that doesn't end up hitting a NOMARKS
		// surface (sky for example)
		if (bestfrac > trace.fraction && !(trace.hitq3surfaceflags & Q3SURFACEFLAG_NOMARKS))
		{
			bestfrac = trace.fraction;
			besthitent = hitent;
			VectorCopy(trace.endpos, bestorg);
			VectorCopy(trace.plane.normal, bestnormal);
		}
	}
	if (bestfrac < 1)
		CL_SpawnDecalParticleForSurface(besthitent, bestorg, bestnormal, color1, color2, texnum, size, alpha);
}

// generates a cubemap name with prefix flags based on info flags (for now only `!`)
static char *LightCubemapNumToName(char *vabuf, size_t vasize, int lightcubemapnum, int flags)
{
	if (lightcubemapnum <= 0)
		return NULL;
	// `!` is prepended if the cubemap must be nearest-filtered
	if (flags & PARTICLEEFFECT_FORCENEAREST)
		return va(vabuf, vasize, "!cubemaps/%i", lightcubemapnum);
	return va(vabuf, vasize, "cubemaps/%i", lightcubemapnum);
}

static void CL_Sparks(const vec3_t originmins, const vec3_t originmaxs, const vec3_t velocitymins, const vec3_t velocitymaxs, float sparkcount);
static void M5_ImpactFlash(const vec3_t center, const vec3_t originmins, const vec3_t originmaxs);
static void CL_Smoke(const vec3_t originmins, const vec3_t originmaxs, const vec3_t velocitymins, const vec3_t velocitymaxs, float smokecount);
static void CL_NewParticlesFromEffectinfo(int effectnameindex, float pcount, const vec3_t originmins, const vec3_t originmaxs, const vec3_t velocitymins, const vec3_t velocitymaxs, entity_t *ent, int palettecolor, qbool spawndlight, qbool spawnparticles, float tintmins[4], float tintmaxs[4], float fade, qbool wanttrail);
static void CL_ParticleEffect_Fallback(int effectnameindex, float count, const vec3_t originmins, const vec3_t originmaxs, const vec3_t velocitymins, const vec3_t velocitymaxs, entity_t *ent, int palettecolor, qbool spawndlight, qbool spawnparticles, qbool wanttrail)
{
	vec3_t center;
	matrix4x4_t lightmatrix;
	particle_t *part;

	VectorLerp(originmins, 0.5, originmaxs, center);
	Matrix4x4_CreateTranslate(&lightmatrix, center[0], center[1], center[2]);
	if (effectnameindex == EFFECT_SVC_PARTICLE)
	{
		if (cl_particles.integer)
		{
			// bloodhack checks if this effect's color matches regular or lightning blood and if so spawns a blood effect instead
			if (count == 1024)
				CL_NewParticlesFromEffectinfo(EFFECT_TE_EXPLOSION, 1, originmins, originmaxs, velocitymins, velocitymaxs, NULL, 0, spawndlight, spawnparticles, NULL, NULL, 1, wanttrail);
			else if (cl_particles_blood_bloodhack.integer && !cl_particles_quake.integer && (palettecolor == 73 || palettecolor == 225))
				CL_NewParticlesFromEffectinfo(EFFECT_TE_BLOOD, count / 2.0f, originmins, originmaxs, velocitymins, velocitymaxs, NULL, 0, spawndlight, spawnparticles, NULL, NULL, 1, wanttrail);
			else
			{
				count *= cl_particles_quality.value;
				for (;count > 0;count--)
				{
					int k = particlepalette[(palettecolor & ~7) + (rand()&7)];
					CL_NewQuakeParticle(
						center,                                      // origin
						pt_alphastatic,                              // type
						k,                                           // color 1
						k,                                           // color 2
						0.15,                                        // gravity
						lhrandom(originmins[0], originmaxs[0]),      // offset x
						lhrandom(originmins[1], originmaxs[1]),      // offset y
						lhrandom(originmins[2], originmaxs[2]),      // offset z
						lhrandom(velocitymins[0], velocitymaxs[0]),  // velocity offset x
						lhrandom(velocitymins[1], velocitymaxs[1]),  // velocity offset y
						lhrandom(velocitymins[2], velocitymaxs[2]),  // velocity offset z
						0,                                           // air friction
						0,                                           // liquid friction
						8,                                           // origin jitter
						3,                                           // velocity jitter
						lhrandom(0.1, 0.4)                           // lifetime
					);
				}
			}
		}
	}
	else if (effectnameindex == EFFECT_TE_WIZSPIKE)
		CL_NewParticlesFromEffectinfo(EFFECT_SVC_PARTICLE, 30*count, originmins, originmaxs, velocitymins, velocitymaxs, NULL, 20, spawndlight, spawnparticles, NULL, NULL, 1, wanttrail);
	else if (effectnameindex == EFFECT_TE_KNIGHTSPIKE)
		CL_NewParticlesFromEffectinfo(EFFECT_SVC_PARTICLE, 20*count, originmins, originmaxs, velocitymins, velocitymaxs, NULL, 226, spawndlight, spawnparticles, NULL, NULL, 1, wanttrail);
	else if (effectnameindex == EFFECT_TE_SPIKE)
	{
		if (cl_particles_bulletimpacts.integer)
		{
			if (cl_particles_quake.integer)
			{
				if (cl_particles_smoke.integer)
					CL_NewParticlesFromEffectinfo(EFFECT_SVC_PARTICLE, 10*count, originmins, originmaxs, velocitymins, velocitymaxs, NULL, 0, spawndlight, spawnparticles, NULL, NULL, 1, wanttrail);
			}
			else
			{
				CL_Smoke(originmins, originmaxs, velocitymins, velocitymaxs, 4*count);
				CL_Sparks(originmins, originmaxs, velocitymins, velocitymaxs, 15*count);
				M5_ImpactFlash(center, originmins, originmaxs);
			}
		}
		// bullet hole
		R_Stain(center, 16, 40, 40, 40, 64, 88, 88, 88, 64);
		CL_SpawnDecalParticleForPoint(center, 6, 3, 255, tex_bulletdecal[rand()&7], 0xFFFFFF, 0xFFFFFF);
	}
	else if (effectnameindex == EFFECT_TE_SPIKEQUAD)
	{
		if (cl_particles_bulletimpacts.integer)
		{
			if (cl_particles_quake.integer)
			{
				if (cl_particles_smoke.integer)
					CL_NewParticlesFromEffectinfo(EFFECT_SVC_PARTICLE, 10*count, originmins, originmaxs, velocitymins, velocitymaxs, NULL, 0, spawndlight, spawnparticles, NULL, NULL, 1, wanttrail);
			}
			else
			{
				CL_Smoke(originmins, originmaxs, velocitymins, velocitymaxs, 4*count);
				CL_Sparks(originmins, originmaxs, velocitymins, velocitymaxs, 15*count);
				M5_ImpactFlash(center, originmins, originmaxs);
			}
		}
		// bullet hole
		R_Stain(center, 16, 40, 40, 40, 64, 88, 88, 88, 64);
		CL_SpawnDecalParticleForPoint(center, 6, 3, 255, tex_bulletdecal[rand()&7], 0xFFFFFF, 0xFFFFFF);
		CL_AllocLightFlash(NULL, &lightmatrix, 100, 0.15f, 0.15f, 1.5f, 500, 0.2, NULL, -1, true, 1, 0.25, 1, 0, 0, LIGHTFLAG_NORMALMODE | LIGHTFLAG_REALTIMEMODE);
	}
	else if (effectnameindex == EFFECT_TE_SUPERSPIKE)
	{
		if (cl_particles_bulletimpacts.integer)
		{
			if (cl_particles_quake.integer)
			{
				if (cl_particles_smoke.integer)
					CL_NewParticlesFromEffectinfo(EFFECT_SVC_PARTICLE, 20*count, originmins, originmaxs, velocitymins, velocitymaxs, NULL, 0, spawndlight, spawnparticles, NULL, NULL, 1, wanttrail);
			}
			else
			{
				CL_Smoke(originmins, originmaxs, velocitymins, velocitymaxs, 8*count);
				CL_Sparks(originmins, originmaxs, velocitymins, velocitymaxs, 30*count);
				M5_ImpactFlash(center, originmins, originmaxs);
			}
		}
		// bullet hole
		R_Stain(center, 16, 40, 40, 40, 64, 88, 88, 88, 64);
		CL_SpawnDecalParticleForPoint(center, 6, 3, 255, tex_bulletdecal[rand()&7], 0xFFFFFF, 0xFFFFFF);
	}
	else if (effectnameindex == EFFECT_TE_SUPERSPIKEQUAD)
	{
		if (cl_particles_bulletimpacts.integer)
		{
			if (cl_particles_quake.integer)
			{
				if (cl_particles_smoke.integer)
					CL_NewParticlesFromEffectinfo(EFFECT_SVC_PARTICLE, 20*count, originmins, originmaxs, velocitymins, velocitymaxs, NULL, 0, spawndlight, spawnparticles, NULL, NULL, 1, wanttrail);
			}
			else
			{
				CL_Smoke(originmins, originmaxs, velocitymins, velocitymaxs, 8*count);
				CL_Sparks(originmins, originmaxs, velocitymins, velocitymaxs, 30*count);
				M5_ImpactFlash(center, originmins, originmaxs);
			}
		}
		// bullet hole
		R_Stain(center, 16, 40, 40, 40, 64, 88, 88, 88, 64);
		CL_SpawnDecalParticleForPoint(center, 6, 3, 255, tex_bulletdecal[rand()&7], 0xFFFFFF, 0xFFFFFF);
		CL_AllocLightFlash(NULL, &lightmatrix, 100, 0.15f, 0.15f, 1.5f, 500, 0.2, NULL, -1, true, 1, 0.25, 1, 0, 0, LIGHTFLAG_NORMALMODE | LIGHTFLAG_REALTIMEMODE);
	}
	else if (effectnameindex == EFFECT_TE_BLOOD)
	{
		if (!cl_particles_blood.integer)
			return;
		if (cl_particles_quake.integer)
			CL_NewParticlesFromEffectinfo(EFFECT_SVC_PARTICLE, 2*count, originmins, originmaxs, velocitymins, velocitymaxs, NULL, 73, spawndlight, spawnparticles, NULL, NULL, 1, wanttrail);
		else
		{
			static double bloodaccumulator = 0;
			qbool immediatebloodstain = (cl_decals_newsystem_immediatebloodstain.integer >= 1);
			//CL_NewParticle(center, pt_alphastatic, 0x4f0000,0x7f0000, tex_particle, 2.5, 0, 256, 256, 0, 0, lhrandom(originmins[0], originmaxs[0]), lhrandom(originmins[1], originmaxs[1]), lhrandom(originmins[2], originmaxs[2]), 0, 0, 0, 1, 4, 0, 0, true, 0, 1, PBLEND_ALPHA, PARTICLE_BILLBOARD, NULL);
			bloodaccumulator += count * 0.333 * cl_particles_quality.value;
			for (;bloodaccumulator > 0;bloodaccumulator--)
			{
				part = CL_NewParticle(center, pt_blood, 0xFFFFFF, 0xFFFFFF, M5_BloodTex(), 8, 0, cl_particles_blood_alpha.value * 768, cl_particles_blood_alpha.value * 384, 1, -1, lhrandom(originmins[0], originmaxs[0]), lhrandom(originmins[1], originmaxs[1]), lhrandom(originmins[2], originmaxs[2]), lhrandom(velocitymins[0], velocitymaxs[0]), lhrandom(velocitymins[1], velocitymaxs[1]), lhrandom(velocitymins[2], velocitymaxs[2]), 1, 4, 0, 64, true, 0, 1, PBLEND_INVMOD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, M5_BloodAngle(), 0, NULL);
				if (immediatebloodstain && part)
				{
					immediatebloodstain = false;
					CL_ImmediateBloodStain(part);
				}
			}
		}
	}
	else if (effectnameindex == EFFECT_TE_SPARK)
		CL_Sparks(originmins, originmaxs, velocitymins, velocitymaxs, count);
	else if (effectnameindex == EFFECT_TE_PLASMABURN)
	{
		// plasma scorch mark
		R_Stain(center, 40, 40, 40, 40, 64, 88, 88, 88, 64);
		CL_SpawnDecalParticleForPoint(center, 6, 6, 255, tex_bulletdecal[rand()&7], 0xFFFFFF, 0xFFFFFF);
		CL_AllocLightFlash(NULL, &lightmatrix, 200, 1, 1, 1, 1000, 0.2, NULL, -1, true, 1, 0.25, 1, 0, 0, LIGHTFLAG_NORMALMODE | LIGHTFLAG_REALTIMEMODE);
	}
	else if (effectnameindex == EFFECT_TE_GUNSHOT)
	{
		if (cl_particles_bulletimpacts.integer)
		{
			if (cl_particles_quake.integer)
				CL_NewParticlesFromEffectinfo(EFFECT_SVC_PARTICLE, 20*count, originmins, originmaxs, velocitymins, velocitymaxs, NULL, 0, spawndlight, spawnparticles, NULL, NULL, 1, wanttrail);
			else
			{
				CL_Smoke(originmins, originmaxs, velocitymins, velocitymaxs, 4*count);
				CL_Sparks(originmins, originmaxs, velocitymins, velocitymaxs, 20*count);
				M5_ImpactFlash(center, originmins, originmaxs);
			}
		}
		// bullet hole
		R_Stain(center, 16, 40, 40, 40, 64, 88, 88, 88, 64);
		CL_SpawnDecalParticleForPoint(center, 6, 3, 255, tex_bulletdecal[rand()&7], 0xFFFFFF, 0xFFFFFF);
	}
	else if (effectnameindex == EFFECT_TE_GUNSHOTQUAD)
	{
		if (cl_particles_bulletimpacts.integer)
		{
			if (cl_particles_quake.integer)
				CL_NewParticlesFromEffectinfo(EFFECT_SVC_PARTICLE, 20*count, originmins, originmaxs, velocitymins, velocitymaxs, NULL, 0, spawndlight, spawnparticles, NULL, NULL, 1, wanttrail);
			else
			{
				CL_Smoke(originmins, originmaxs, velocitymins, velocitymaxs, 4*count);
				CL_Sparks(originmins, originmaxs, velocitymins, velocitymaxs, 20*count);
				M5_ImpactFlash(center, originmins, originmaxs);
			}
		}
		// bullet hole
		R_Stain(center, 16, 40, 40, 40, 64, 88, 88, 88, 64);
		CL_SpawnDecalParticleForPoint(center, 6, 3, 255, tex_bulletdecal[rand()&7], 0xFFFFFF, 0xFFFFFF);
		CL_AllocLightFlash(NULL, &lightmatrix, 100, 0.15f, 0.15f, 1.5f, 500, 0.2, NULL, -1, true, 1, 0.25, 1, 0, 0, LIGHTFLAG_NORMALMODE | LIGHTFLAG_REALTIMEMODE);
	}
	else if (effectnameindex == EFFECT_TE_EXPLOSION)
	{
		CL_ParticleExplosion(center);
		CL_AllocLightFlash(NULL, &lightmatrix, 350, 4.0f, 2.0f, 0.50f, 700, 0.5, NULL, -1, true, 1, 0.25, 0.25, 1, 1, LIGHTFLAG_NORMALMODE | LIGHTFLAG_REALTIMEMODE);
	}
	else if (effectnameindex == EFFECT_TE_EXPLOSIONQUAD)
	{
		CL_ParticleExplosion(center);
		CL_AllocLightFlash(NULL, &lightmatrix, 350, 2.5f, 2.0f, 4.0f, 700, 0.5, NULL, -1, true, 1, 0.25, 0.25, 1, 1, LIGHTFLAG_NORMALMODE | LIGHTFLAG_REALTIMEMODE);
	}
	else if (effectnameindex == EFFECT_TE_TAREXPLOSION)
	{
		if (cl_particles_quake.integer)
		{
			int i;
			for (i = 0;i < 1024 * cl_particles_quality.value;i++)
			{
				if (i & 1)
					CL_NewParticle(center, pt_alphastatic, particlepalette[66], particlepalette[71], tex_particle, 1.5f, 0, 255, 0, 0, 0, center[0], center[1], center[2], 0, 0, 0, -4, -4, 16, 256, true, (rand() & 1) ? 1.4 : 1.0, 1, PBLEND_ALPHA, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
				else
					CL_NewParticle(center, pt_alphastatic, particlepalette[150], particlepalette[155], tex_particle, 1.5f, 0, 255, 0, 0, 0, center[0], center[1], center[2], 0, 0, lhrandom(-256, 256), 0, 0, 16, 0, true, (rand() & 1) ? 1.4 : 1.0, 1, PBLEND_ALPHA, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
			}
		}
		else
			CL_ParticleExplosion(center);
		CL_AllocLightFlash(NULL, &lightmatrix, 600, 1.6f, 0.8f, 2.0f, 1200, 0.5, NULL, -1, true, 1, 0.25, 0.25, 1, 1, LIGHTFLAG_NORMALMODE | LIGHTFLAG_REALTIMEMODE);
	}
	else if (effectnameindex == EFFECT_TE_SMALLFLASH)
		CL_AllocLightFlash(NULL, &lightmatrix, 200, 2, 2, 2, 1000, 0.2, NULL, -1, true, 1, 0.25, 0.25, 1, 1, LIGHTFLAG_NORMALMODE | LIGHTFLAG_REALTIMEMODE);
	else if (effectnameindex == EFFECT_TE_FLAMEJET)
	{
		count *= cl_particles_quality.value;
		while (count-- > 0)
			CL_NewParticle(center, pt_smoke, 0x6f0f00, 0xe3974f, tex_particle, 4, 0, lhrandom(64, 128), 384, -1, 1.1, lhrandom(originmins[0], originmaxs[0]), lhrandom(originmins[1], originmaxs[1]), lhrandom(originmins[2], originmaxs[2]), lhrandom(velocitymins[0], velocitymaxs[0]), lhrandom(velocitymins[1], velocitymaxs[1]), lhrandom(velocitymins[2], velocitymaxs[2]), 1, 4, 0, 128, true, 0, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
	}
	else if (effectnameindex == EFFECT_TE_LAVASPLASH)
	{
		float i, j, inc, vel;
		vec3_t dir, org;

		inc = 8 / cl_particles_quality.value;
		for (i = -128;i < 128;i += inc)
		{
			for (j = -128;j < 128;j += inc)
			{
				dir[0] = j + lhrandom(0, inc);
				dir[1] = i + lhrandom(0, inc);
				dir[2] = 256;
				org[0] = center[0] + dir[0];
				org[1] = center[1] + dir[1];
				org[2] = center[2] + lhrandom(0, 64);
				vel = lhrandom(50, 120) / VectorLength(dir); // normalize and scale
				CL_NewParticle(center, pt_alphastatic, particlepalette[224], particlepalette[231], tex_particle, 1.5f, 0, 255, 0, 0.05, 0, org[0], org[1], org[2], dir[0] * vel, dir[1] * vel, dir[2] * vel, 0, 0, 0, 0, true, lhrandom(2, 2.62), 1, PBLEND_ALPHA, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
			}
		}
	}
	else if (effectnameindex == EFFECT_TE_TELEPORT)
	{
		float i, j, k, inc, vel;
		vec3_t dir;

		if (cl_particles_quake.integer)
			inc = 4 / cl_particles_quality.value;
		else
			inc = 8 / cl_particles_quality.value;
		for (i = -16;i < 16;i += inc)
		{
			for (j = -16;j < 16;j += inc)
			{
				for (k = -24;k < 32;k += inc)
				{
					VectorSet(dir, i*8, j*8, k*8);
					VectorNormalize(dir);
					vel = lhrandom(50, 113);
					if (cl_particles_quake.integer)
						CL_NewParticle(center, pt_alphastatic, particlepalette[7], particlepalette[14], tex_particle, 1.5f, 0, 255, 0, 0, 0, center[0] + i + lhrandom(0, inc), center[1] + j + lhrandom(0, inc), center[2] + k + lhrandom(0, inc), dir[0] * vel, dir[1] * vel, dir[2] * vel, 0, 0, 0, 0, true, lhrandom(0.2, 0.34), 1, PBLEND_ALPHA, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
					else
						CL_NewParticle(center, pt_alphastatic, particlepalette[7], particlepalette[14], tex_particle, 1.5f, 0, inc * lhrandom(37, 63), inc * 187, 0, 0, center[0] + i + lhrandom(0, inc), center[1] + j + lhrandom(0, inc), center[2] + k + lhrandom(0, inc), dir[0] * vel, dir[1] * vel, dir[2] * vel, 0, 0, 0, 0, true, 0, 1, PBLEND_ALPHA, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
				}
			}
		}
		if (!cl_particles_quake.integer)
			CL_NewParticle(center, pt_static, 0xffffff, 0xffffff, tex_particle, 30, 0, 256, 512, 0, 0, center[0], center[1], center[2], 0, 0, 0, 0, 0, 0, 0, false, 0, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
		CL_AllocLightFlash(NULL, &lightmatrix, 200, 2.0f, 2.0f, 2.0f, 400, 99.0f, NULL, -1, true, 1, 0.25, 1, 0, 0, LIGHTFLAG_NORMALMODE | LIGHTFLAG_REALTIMEMODE);
	}
	else if (effectnameindex == EFFECT_TE_TEI_G3)
		CL_NewParticle(center, pt_beam, 0xFFFFFF, 0xFFFFFF, tex_beam, 8, 0, 256, 256, 0, 0, originmins[0], originmins[1], originmins[2], originmaxs[0], originmaxs[1], originmaxs[2], 0, 0, 0, 0, false, 0, 1, PBLEND_ADD, PARTICLE_HBEAM, -1, -1, -1, 1, 1, 0, 0, NULL);
	else if (effectnameindex == EFFECT_TE_TEI_SMOKE)
	{
		if (cl_particles_smoke.integer)
		{
			count *= 0.25f * cl_particles_quality.value;
			while (count-- > 0)
				CL_NewParticle(center, pt_smoke, 0x202020, 0x404040, tex_smoke[rand()&7], 5, 0, 255, 512, 0, 0, lhrandom(originmins[0], originmaxs[0]), lhrandom(originmins[1], originmaxs[1]), lhrandom(originmins[2], originmaxs[2]), lhrandom(velocitymins[0], velocitymaxs[0]), lhrandom(velocitymins[1], velocitymaxs[1]), lhrandom(velocitymins[2], velocitymaxs[2]), 0, 0, 1.5f, 6.0f, true, 0, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
		}
	}
	else if (effectnameindex == EFFECT_TE_TEI_BIGEXPLOSION)
	{
		CL_ParticleExplosion(center);
		CL_AllocLightFlash(NULL, &lightmatrix, 500, 2.5f, 2.0f, 1.0f, 500, 9999, NULL, -1, true, 1, 0.25, 0.5, 1, 1, LIGHTFLAG_NORMALMODE | LIGHTFLAG_REALTIMEMODE);
	}
	else if (effectnameindex == EFFECT_TE_TEI_PLASMAHIT)
	{
		float f;
		R_Stain(center, 40, 40, 40, 40, 64, 88, 88, 88, 64);
		CL_SpawnDecalParticleForPoint(center, 6, 8, 255, tex_bulletdecal[rand()&7], 0xFFFFFF, 0xFFFFFF);
		if (cl_particles_smoke.integer)
			for (f = 0;f < count;f += 4.0f / cl_particles_quality.value)
				CL_NewParticle(center, pt_smoke, 0x202020, 0x404040, tex_smoke[rand()&7], 5, 0, 255, 512, 0, 0, lhrandom(originmins[0], originmaxs[0]), lhrandom(originmins[1], originmaxs[1]), lhrandom(originmins[2], originmaxs[2]), lhrandom(velocitymins[0], velocitymaxs[0]), lhrandom(velocitymins[1], velocitymaxs[1]), lhrandom(velocitymins[2], velocitymaxs[2]), 0, 0, 20, 155, true, 0, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
		if (cl_particles_sparks.integer)
			for (f = 0;f < count;f += 1.0f / cl_particles_quality.value)
				CL_NewParticle(center, pt_spark, 0x2030FF, 0x80C0FF, tex_particle, 2.0f, 0, lhrandom(64, 255), 512, 0, 0, lhrandom(originmins[0], originmaxs[0]), lhrandom(originmins[1], originmaxs[1]), lhrandom(originmins[2], originmaxs[2]), lhrandom(velocitymins[0], velocitymaxs[0]), lhrandom(velocitymins[1], velocitymaxs[1]), lhrandom(velocitymins[2], velocitymaxs[2]), 0, 0, 0, 465, true, 0, 1, PBLEND_ADD, PARTICLE_SPARK, -1, -1, -1, 1, 1, 0, 0, NULL);
		CL_AllocLightFlash(NULL, &lightmatrix, 500, 0.6f, 1.2f, 2.0f, 2000, 9999, NULL, -1, true, 1, 0.25, 0.25, 1, 1, LIGHTFLAG_NORMALMODE | LIGHTFLAG_REALTIMEMODE);
	}
	else if (effectnameindex == EFFECT_EF_FLAME)
	{
		if (!spawnparticles)
			count = 0;
		count *= 300 * cl_particles_quality.value;
		while (count-- > 0)
			CL_NewParticle(center, pt_smoke, 0x6f0f00, 0xe3974f, tex_particle, 4, 0, lhrandom(64, 128), 384, -1, 0, lhrandom(originmins[0], originmaxs[0]), lhrandom(originmins[1], originmaxs[1]), lhrandom(originmins[2], originmaxs[2]), lhrandom(velocitymins[0], velocitymaxs[0]), lhrandom(velocitymins[1], velocitymaxs[1]), lhrandom(velocitymins[2], velocitymaxs[2]), 1, 4, 16, 128, true, 0, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
		CL_AllocLightFlash(NULL, &lightmatrix, 200, 2.0f, 1.5f, 0.5f, 0, 0, NULL, -1, true, 1, 0.25, 0.25, 1, 1, LIGHTFLAG_NORMALMODE | LIGHTFLAG_REALTIMEMODE);
	}
	else if (effectnameindex == EFFECT_EF_STARDUST)
	{
		if (!spawnparticles)
			count = 0;
		count *= 200 * cl_particles_quality.value;
		while (count-- > 0)
			CL_NewParticle(center, pt_static, 0x903010, 0xFFD030, tex_particle, 4, 0, lhrandom(64, 128), 128, 1, 0, lhrandom(originmins[0], originmaxs[0]), lhrandom(originmins[1], originmaxs[1]), lhrandom(originmins[2], originmaxs[2]), lhrandom(velocitymins[0], velocitymaxs[0]), lhrandom(velocitymins[1], velocitymaxs[1]), lhrandom(velocitymins[2], velocitymaxs[2]), 0.2, 0.8, 16, 128, true, 0, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
		CL_AllocLightFlash(NULL, &lightmatrix, 200, 1.0f, 0.7f, 0.3f, 0, 0, NULL, -1, true, 1, 0.25, 0.25, 1, 1, LIGHTFLAG_NORMALMODE | LIGHTFLAG_REALTIMEMODE);
	}
	else if (!strncmp(particleeffectname[effectnameindex], "TR_", 3))
	{
		vec3_t dir, pos;
		float len, dec, qd;
		int smoke, blood, bubbles, r, color, spawnedcount;

		if (spawndlight && r_refdef.scene.numlights < MAX_DLIGHTS)
		{
			vec4_t light;
			Vector4Set(light, 0, 0, 0, 0);

			if (effectnameindex == EFFECT_TR_ROCKET)
				Vector4Set(light, 3.0f, 1.5f, 0.5f, 200);
			else if (effectnameindex == EFFECT_TR_VORESPIKE)
			{
				if (gamemode == GAME_PRYDON && !cl_particles_quake.integer)
					Vector4Set(light, 0.3f, 0.6f, 1.2f, 100);
				else
					Vector4Set(light, 1.2f, 0.5f, 1.0f, 200);
			}
			else if (effectnameindex == EFFECT_TR_NEXUIZPLASMA)
				Vector4Set(light, 0.75f, 1.5f, 3.0f, 200);

			if (light[3])
			{
				matrix4x4_t traillightmatrix;
				Matrix4x4_CreateFromQuakeEntity(&traillightmatrix, originmaxs[0], originmaxs[1], originmaxs[2], 0, 0, 0, light[3]);
				R_RTLight_Update(&r_refdef.scene.templights[r_refdef.scene.numlights], false, &traillightmatrix, light, -1, NULL, true, 1, 0.25, 0, 1, 1, LIGHTFLAG_NORMALMODE | LIGHTFLAG_REALTIMEMODE);
				r_refdef.scene.lights[r_refdef.scene.numlights] = &r_refdef.scene.templights[r_refdef.scene.numlights];r_refdef.scene.numlights++;
			}
		}

		if (!spawnparticles)
			return;

		if (originmaxs[0] == originmins[0] && originmaxs[1] == originmins[1] && originmaxs[2] == originmins[2])
			return;

		VectorSubtract(originmaxs, originmins, dir);
		len = VectorNormalizeLength(dir);

		if (ent)
		{
			dec = -ent->persistent.trail_time;
			ent->persistent.trail_time += len;
			if (ent->persistent.trail_time < 0.01f)
				return;

			// if we skip out, leave it reset
			ent->persistent.trail_time = 0.0f;
		}
		else
			dec = 0;

		// advance into this frame to reach the first puff location
		VectorMA(originmins, dec, dir, pos);
		len -= dec;

		smoke = cl_particles.integer && cl_particles_smoke.integer;
		blood = cl_particles.integer && cl_particles_blood.integer;
		bubbles = cl_particles.integer && cl_particles_bubbles.integer && !cl_particles_quake.integer && (CL_PointSuperContents(pos) & (SUPERCONTENTS_WATER | SUPERCONTENTS_SLIME));
		qd = 1.0f / cl_particles_quality.value;
		spawnedcount = 0;

		while (len >= 0 && ++spawnedcount <= 16384)
		{
			dec = 3;
			if (blood)
			{
				if (effectnameindex == EFFECT_TR_BLOOD)
				{
					if (cl_particles_quake.integer)
					{
						color = particlepalette[67 + (rand()&3)];
						CL_NewQuakeParticle(center, pt_alphastatic, color, color, 0.25, pos[0], pos[1], pos[2], 0, 0, 0, 0, 0, 3, 0, 2);
					}
					else
					{
						dec = 16;
						CL_NewParticle(center, pt_blood, 0xFFFFFF, 0xFFFFFF, M5_BloodTex(), 8, 0, qd * cl_particles_blood_alpha.value * 768.0f, qd * cl_particles_blood_alpha.value * 384.0f, 1, -1, pos[0], pos[1], pos[2], lhrandom(velocitymins[0], velocitymaxs[0]), lhrandom(velocitymins[1], velocitymaxs[1]), lhrandom(velocitymins[2], velocitymaxs[2]), 1, 4, 0, 64, true, 0, 1, PBLEND_INVMOD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, M5_BloodAngle(), 0, NULL);
					}
				}
				else if (effectnameindex == EFFECT_TR_SLIGHTBLOOD)
				{
					if (cl_particles_quake.integer)
					{
						dec = 6;
						color = particlepalette[67 + (rand()&3)];
						CL_NewQuakeParticle(center, pt_alphastatic, color, color, 0.25, pos[0], pos[1], pos[2], 0, 0, 0, 0, 0, 3, 0, 2);
					}
					else
					{
						dec = 32;
						CL_NewParticle(center, pt_blood, 0xFFFFFF, 0xFFFFFF, M5_BloodTex(), 8, 0, qd * cl_particles_blood_alpha.value * 768.0f, qd * cl_particles_blood_alpha.value * 384.0f, 1, -1, pos[0], pos[1], pos[2], lhrandom(velocitymins[0], velocitymaxs[0]), lhrandom(velocitymins[1], velocitymaxs[1]), lhrandom(velocitymins[2], velocitymaxs[2]), 1, 4, 0, 64, true, 0, 1, PBLEND_INVMOD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, M5_BloodAngle(), 0, NULL);
					}
				}
			}
			if (smoke)
			{
				if (effectnameindex == EFFECT_TR_ROCKET)
				{
					if (cl_particles_quake.integer)
					{
						r = rand()&3;
						color = particlepalette[ramp3[r]];
						CL_NewQuakeParticle(center, pt_alphastatic, color, color, -0.10, pos[0], pos[1], pos[2], 0, 0, 0, 0, 0, 3, 0, 0.1372549 * (6 - r));
					}
					else
					{
						CL_NewParticle(center, pt_smoke, 0x303030, 0x606060, tex_smoke[rand()&7], 3, 0, cl_particles_smoke_alpha.value*62, cl_particles_smoke_alphafade.value*62, 0, 0, pos[0], pos[1], pos[2], 0, 0, 0, 0, 0, 0, 0, true, 0, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
						CL_NewParticle(center, pt_static, 0x801010, 0xFFA020, tex_smoke[rand()&7], 3, 0, cl_particles_smoke_alpha.value*288, cl_particles_smoke_alphafade.value*1400, 0, 0, pos[0], pos[1], pos[2], 0, 0, 0, 0, 0, 0, 20, true, 0, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
					}
				}
				else if (effectnameindex == EFFECT_TR_GRENADE)
				{
					if (cl_particles_quake.integer)
					{
						r = 2 + (rand()%4);
						color = particlepalette[ramp3[r]];
						CL_NewQuakeParticle(center, pt_alphastatic, color, color, -0.15, pos[0], pos[1], pos[2], 0, 0, 0, 0, 0, 3, 0, 0.1372549 * (6 - r));
					}
					else
					{
						CL_NewParticle(center, pt_smoke, 0x303030, 0x606060, tex_smoke[rand()&7], 3, 0, cl_particles_smoke_alpha.value*50, cl_particles_smoke_alphafade.value*75, 0, 0, pos[0], pos[1], pos[2], 0, 0, 0, 0, 0, 0, 0, true, 0, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
					}
				}
				else if (effectnameindex == EFFECT_TR_WIZSPIKE)
				{
					// M5 venom: the acid trail. This has to edit the
					// cl_particles_quake branch, because that is the one the
					// player actually runs -- and it is the same reason
					// effectinfo.txt was not an option here, since
					// cl_particles_quake 1 gates that whole system off.
					// Palette 250-254 is the bright yellow-green ramp; the
					// stock 52-59 is the dull mid-green one, and pt_alphastatic
					// does not add, so the stock trail cannot glow whatever
					// colour it is given.
					if (m5_venom.integer && m5_venom_trail.integer && cl_particles_quake.integer)
					{
						dec = 6;
						color = particlepalette[250 + (rand()&3)];
						CL_NewQuakeParticle(center, pt_static, color, color, 0, pos[0], pos[1], pos[2], 30*dir[1], 30*-dir[0], 0, 0, 0, 0, 0, 0.5);
						CL_NewQuakeParticle(center, pt_static, color, color, 0, pos[0], pos[1], pos[2], 30*-dir[1], 30*dir[0], 0, 0, 0, 0, 0, 0.5);
					}
					else if (cl_particles_quake.integer)
					{
						dec = 6;
						color = particlepalette[52 + (rand()&7)];
						CL_NewQuakeParticle(center, pt_alphastatic, color, color, 0, pos[0], pos[1], pos[2], 30*dir[1], 30*-dir[0], 0, 0, 0, 0, 0, 0.5);
						CL_NewQuakeParticle(center, pt_alphastatic, color, color, 0, pos[0], pos[1], pos[2], 30*-dir[1], 30*dir[0], 0, 0, 0, 0, 0, 0.5);
					}
					else if (gamemode == GAME_GOODVSBAD2)
					{
						dec = 6;
						CL_NewParticle(center, pt_static, 0x00002E, 0x000030, tex_particle, 6, 0, 128, 384, 0, 0, pos[0], pos[1], pos[2], 0, 0, 0, 0, 0, 0, 0, true, 0, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
					}
					else
					{
						color = particlepalette[20 + (rand()&7)];
						CL_NewParticle(center, pt_static, color, color, tex_particle, 2, 0, 64, 192, 0, 0, pos[0], pos[1], pos[2], 0, 0, 0, 0, 0, 0, 0, true, 0, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
					}
				}
				else if (effectnameindex == EFFECT_TR_KNIGHTSPIKE)
				{
					if (cl_particles_quake.integer)
					{
						dec = 6;
						color = particlepalette[230 + (rand()&7)];
						CL_NewQuakeParticle(center, pt_alphastatic, color, color, 0, pos[0], pos[1], pos[2], 30 *  dir[1], 30 * -dir[0], 0, 0, 0, 0, 0, 0.5);
						CL_NewQuakeParticle(center, pt_alphastatic, color, color, 0, pos[0], pos[1], pos[2], 30 * -dir[1], 30 *  dir[0], 0, 0, 0, 0, 0, 0.5);
					}
					else
					{
						color = particlepalette[226 + (rand()&7)];
						CL_NewParticle(center, pt_static, color, color, tex_particle, 2, 0, 64, 192, 0, 0, pos[0], pos[1], pos[2], 0, 0, 0, 0, 0, 0, 0, true, 0, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
					}
				}
				else if (effectnameindex == EFFECT_TR_VORESPIKE)
				{
					if (cl_particles_quake.integer)
					{
						color = particlepalette[152 + (rand()&3)];
						CL_NewQuakeParticle(center, pt_alphastatic, color, color, 0, pos[0], pos[1], pos[2], 0, 0, 0, 0, 0, 8, 0, 0.3);
					}
					else if (gamemode == GAME_GOODVSBAD2)
					{
						dec = 6;
						CL_NewParticle(center, pt_alphastatic, particlepalette[0 + (rand()&255)], particlepalette[0 + (rand()&255)], tex_particle, 6, 0, 255, 384, 0, 0, pos[0], pos[1], pos[2], 0, 0, 0, 0, 0, 0, 0, true, 0, 1, PBLEND_ALPHA, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
					}
					else if (gamemode == GAME_PRYDON)
					{
						dec = 6;
						CL_NewParticle(center, pt_static, 0x103040, 0x204050, tex_particle, 6, 0, 64, 192, 0, 0, pos[0], pos[1], pos[2], 0, 0, 0, 0, 0, 0, 0, true, 0, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
					}
					else
						CL_NewParticle(center, pt_static, 0x502030, 0x502030, tex_particle, 3, 0, 64, 192, 0, 0, pos[0], pos[1], pos[2], 0, 0, 0, 0, 0, 0, 0, true, 0, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
				}
				else if (effectnameindex == EFFECT_TR_NEHAHRASMOKE)
				{
					dec = 7;
					CL_NewParticle(center, pt_alphastatic, 0x303030, 0x606060, tex_smoke[rand()&7], 7, 0, 64, 320, 0, 0, pos[0], pos[1], pos[2], 0, 0, lhrandom(4, 12), 0, 0, 0, 4, false, 0, 1, PBLEND_ALPHA, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
				}
				else if (effectnameindex == EFFECT_TR_NEXUIZPLASMA)
				{
					dec = 4;
					CL_NewParticle(center, pt_static, 0x283880, 0x283880, tex_particle, 4, 0, 255, 1024, 0, 0, pos[0], pos[1], pos[2], 0, 0, 0, 0, 0, 0, 16, true, 0, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
				}
				else if (effectnameindex == EFFECT_TR_GLOWTRAIL)
					CL_NewParticle(center, pt_alphastatic, particlepalette[palettecolor], particlepalette[palettecolor], tex_particle, 5, 0, 128, 320, 0, 0, pos[0], pos[1], pos[2], 0, 0, 0, 0, 0, 0, 0, true, 0, 1, PBLEND_ALPHA, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
			}
			if (bubbles)
			{
				if (effectnameindex == EFFECT_TR_ROCKET)
					CL_NewParticle(center, pt_bubble, 0x404040, 0x808080, tex_bubble, 2, 0, lhrandom(128, 512), 512, -0.25, 1.5, pos[0], pos[1], pos[2], 0, 0, 0, 0.0625, 0.25, 0, 16, true, 0, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
				else if (effectnameindex == EFFECT_TR_GRENADE)
					CL_NewParticle(center, pt_bubble, 0x404040, 0x808080, tex_bubble, 2, 0, lhrandom(128, 512), 512, -0.25, 1.5, pos[0], pos[1], pos[2], 0, 0, 0, 0.0625, 0.25, 0, 16, true, 0, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
			}
			// advance to next time and position
			dec *= qd;
			len -= dec;
			VectorMA (pos, dec, dir, pos);
		}
		if (ent)
			ent->persistent.trail_time = len;
	}
	else
		Con_DPrintf("CL_ParticleEffect_Fallback: no fallback found for effect %s\n", particleeffectname[effectnameindex]);
}

// this is also called on point effects with spawndlight = true and
// spawnparticles = true
static void CL_NewParticlesFromEffectinfo(int effectnameindex, float pcount, const vec3_t originmins, const vec3_t originmaxs, const vec3_t velocitymins, const vec3_t velocitymaxs, entity_t *ent, int palettecolor, qbool spawndlight, qbool spawnparticles, float tintmins[4], float tintmaxs[4], float fade, qbool wanttrail)
{
	qbool found = false;
	char vabuf[1024];
	if (effectnameindex < 1 || effectnameindex >= MAX_PARTICLEEFFECTNAME || !particleeffectname[effectnameindex][0])
	{
		Con_DPrintf("Unknown effect number %i received from server\n", effectnameindex);
		return; // no such effect
	}
	if (!cl_particles_quake.integer && particleeffectinfo[0].effectnameindex)
	{
		int effectinfoindex;
		int supercontents;
		int tex, staintex;
		particleeffectinfo_t *info;
		vec3_t center;
		vec3_t traildir;
		vec3_t trailpos;
		vec3_t rvec;
		vec3_t angles;
		vec3_t velocity;
		vec3_t forward;
		vec3_t right;
		vec3_t up;
		vec_t traillen;
		vec_t trailstep;
		qbool underwater;
		qbool immediatebloodstain;
		particle_t *part;
		float avgtint[4], tint[4], tintlerp;
		// note this runs multiple effects with the same name, each one spawns only one kind of particle, so some effects need more than one
		VectorLerp(originmins, 0.5, originmaxs, center);
		supercontents = CL_PointSuperContents(center);
		underwater = (supercontents & (SUPERCONTENTS_WATER | SUPERCONTENTS_SLIME)) != 0;
		VectorSubtract(originmaxs, originmins, traildir);
		traillen = VectorLength(traildir);
		VectorNormalize(traildir);
		if(tintmins)
		{
			Vector4Lerp(tintmins, 0.5, tintmaxs, avgtint);
		}
		else
		{
			Vector4Set(avgtint, 1, 1, 1, 1);
		}
		for (effectinfoindex = 0, info = particleeffectinfo;effectinfoindex < MAX_PARTICLEEFFECTINFO && info->effectnameindex;effectinfoindex++, info++)
		{
			if ((info->effectnameindex == effectnameindex) && (info->flags & PARTICLEEFFECT_DEFINED))
			{
				qbool definedastrail = info->trailspacing > 0;

				qbool drawastrail = wanttrail;
				if (cl_particles_forcetraileffects.integer)
					drawastrail = drawastrail || definedastrail;

				found = true;
				if ((info->flags & PARTICLEEFFECT_UNDERWATER) && !underwater)
					continue;
				if ((info->flags & PARTICLEEFFECT_NOTUNDERWATER) && underwater)
					continue;

				// spawn a dlight if requested
				if (info->lightradiusstart > 0 && spawndlight)
				{
					matrix4x4_t tempmatrix;
					if (drawastrail)
						Matrix4x4_CreateTranslate(&tempmatrix, originmaxs[0], originmaxs[1], originmaxs[2]);
					else
						Matrix4x4_CreateTranslate(&tempmatrix, center[0], center[1], center[2]);
					if (info->lighttime > 0 && info->lightradiusfade > 0)
					{
						// light flash (explosion, etc)
						// called when effect starts
						CL_AllocLightFlash(NULL, &tempmatrix, info->lightradiusstart, info->lightcolor[0]*avgtint[0]*avgtint[3], info->lightcolor[1]*avgtint[1]*avgtint[3], info->lightcolor[2]*avgtint[2]*avgtint[3], info->lightradiusfade, info->lighttime, LightCubemapNumToName(vabuf, sizeof(vabuf), info->lightcubemapnum, info->flags), -1, info->lightshadow, info->lightcorona[0], info->lightcorona[1], 0, 1, 1, LIGHTFLAG_NORMALMODE | LIGHTFLAG_REALTIMEMODE);
					}
					else if (r_refdef.scene.numlights < MAX_DLIGHTS)
					{
						// glowing entity
						// called by CL_LinkNetworkEntity
						Matrix4x4_Scale(&tempmatrix, info->lightradiusstart, 1);
						rvec[0] = info->lightcolor[0]*avgtint[0]*avgtint[3];
						rvec[1] = info->lightcolor[1]*avgtint[1]*avgtint[3];
						rvec[2] = info->lightcolor[2]*avgtint[2]*avgtint[3];
						R_RTLight_Update(&r_refdef.scene.templights[r_refdef.scene.numlights], false, &tempmatrix, rvec, -1, LightCubemapNumToName(vabuf, sizeof(vabuf), info->lightcubemapnum, info->flags), info->lightshadow, info->lightcorona[0], info->lightcorona[1], 0, 1, 1, LIGHTFLAG_NORMALMODE | LIGHTFLAG_REALTIMEMODE);
						r_refdef.scene.lights[r_refdef.scene.numlights] = &r_refdef.scene.templights[r_refdef.scene.numlights];r_refdef.scene.numlights++;
					}
				}

				if (!spawnparticles)
					continue;

				// spawn particles
				tex = info->tex[0];
				if (info->tex[1] > info->tex[0])
				{
					tex = (int)lhrandom(info->tex[0], info->tex[1]);
					tex = min(tex, info->tex[1] - 1);
				}
				if(info->staintex[0] < 0)
					staintex = info->staintex[0];
				else
				{
					staintex = (int)lhrandom(info->staintex[0], info->staintex[1]);
					staintex = min(staintex, info->staintex[1] - 1);
				}
				if (info->particletype == pt_decal)
				{
					VectorMAM(0.5f, velocitymins, 0.5f, velocitymaxs, velocity);
					AnglesFromVectors(angles, velocity, NULL, false);
					AngleVectors(angles, forward, right, up);
					VectorMAMAMAM(1.0f, center, info->relativeoriginoffset[0], forward, info->relativeoriginoffset[1], right, info->relativeoriginoffset[2], up, trailpos);

					CL_SpawnDecalParticleForPoint(trailpos, info->originjitter[0], lhrandom(info->size[0], info->size[1]), lhrandom(info->alpha[0], info->alpha[1])*avgtint[3], tex, info->color[0], info->color[1]);
				}
				else if (info->orientation == PARTICLE_HBEAM)
				{
					if (!drawastrail)
						continue;

					AnglesFromVectors(angles, traildir, NULL, false);
					AngleVectors(angles, forward, right, up);
					VectorMAMAM(info->relativeoriginoffset[0], forward, info->relativeoriginoffset[1], right, info->relativeoriginoffset[2], up, trailpos);

					CL_NewParticle(center, info->particletype, info->color[0], info->color[1], tex, lhrandom(info->size[0], info->size[1]), info->size[2], lhrandom(info->alpha[0], info->alpha[1]), info->alpha[2], 0, 0, originmins[0] + trailpos[0], originmins[1] + trailpos[1], originmins[2] + trailpos[2], originmaxs[0], originmaxs[1], originmaxs[2], 0, 0, 0, 0, false, lhrandom(info->time[0], info->time[1]), info->stretchfactor, info->blendmode, info->orientation, info->staincolor[0], info->staincolor[1], staintex, lhrandom(info->stainalpha[0], info->stainalpha[1]), lhrandom(info->stainsize[0], info->stainsize[1]), 0, 0, tintmins ? avgtint : NULL);
				}
				else
				{
					float cnt;
					if (!cl_particles.integer)
						continue;
					// BEAUTY A5: a refracting layer is skipped ENTIRELY at cvar 0 --
					// before any random number is drawn, so the rest of the effect's
					// realisation is today's to the byte.
					if (info->blendmode == PBLEND_REFRACT && cl_particles_refract.value <= 0.0f)
						continue;
					// BEAUTY A6: a layer gated on a cvar by name, the same way
					if (info->requirecvar[0])
					{
						cvar_t *rc = Cvar_FindVar(&cvars_all, info->requirecvar, ~0);
						if (!rc)
						{
							static int warned = 0;
							if (!warned++)
								Con_Printf("effectinfo: requirecvar %s: no such cvar, the layer never spawns\n", info->requirecvar);
							continue;
						}
						if (rc->value <= 0.0f)
							continue;
					}
					switch (info->particletype)
					{
					case pt_smoke: if (!cl_particles_smoke.integer) continue;break;
					case pt_spark: if (!cl_particles_sparks.integer) continue;break;
					case pt_bubble: if (!cl_particles_bubbles.integer) continue;break;
					case pt_blood: if (!cl_particles_blood.integer) continue;break;
					case pt_rain: if (!cl_particles_rain.integer) continue;break;
					case pt_snow: if (!cl_particles_snow.integer) continue;break;
					default: break;
					}

					cnt = info->countabsolute;
					cnt += (pcount * info->countmultiplier) * cl_particles_quality.value;
					// if drawastrail is not set, we will
					// use the regular cnt-based random
					// particle spawning at the center; so
					// do NOT apply trailspacing then!
					if (drawastrail && definedastrail)
						cnt += (traillen / info->trailspacing) * cl_particles_quality.value;
					cnt *= fade;
					if (cnt == 0)
						continue;  // nothing to draw
					info->particleaccumulator += cnt;

					if (drawastrail || definedastrail)
						immediatebloodstain = false;
					else
						immediatebloodstain =
							((cl_decals_newsystem_immediatebloodstain.integer >= 1) && (info->particletype == pt_blood))
							||
							((cl_decals_newsystem_immediatebloodstain.integer >= 2) && staintex);

					if (drawastrail)
					{
						VectorCopy(originmins, trailpos);
						trailstep = traillen / cnt;
					}
					else
					{
						VectorCopy(center, trailpos);
						trailstep = 0;
					}

					if (trailstep == 0)
					{
						VectorMAM(0.5f, velocitymins, 0.5f, velocitymaxs, velocity);
						AnglesFromVectors(angles, velocity, NULL, false);
					}
					else
						AnglesFromVectors(angles, traildir, NULL, false);

					AngleVectors(angles, forward, right, up);
					VectorMAMAMAM(1.0f, trailpos, info->relativeoriginoffset[0], forward, info->relativeoriginoffset[1], right, info->relativeoriginoffset[2], up, trailpos);
					VectorMAMAM(info->relativevelocityoffset[0], forward, info->relativevelocityoffset[1], right, info->relativevelocityoffset[2], up, velocity);
					info->particleaccumulator = bound(0, info->particleaccumulator, 16384);
					for (;info->particleaccumulator >= 1;info->particleaccumulator--)
					{
						if (info->tex[1] > info->tex[0])
						{
							tex = (int)lhrandom(info->tex[0], info->tex[1]);
							tex = min(tex, info->tex[1] - 1);
						}
						if (!(drawastrail || definedastrail))
						{
							trailpos[0] = lhrandom(originmins[0], originmaxs[0]);
							trailpos[1] = lhrandom(originmins[1], originmaxs[1]);
							trailpos[2] = lhrandom(originmins[2], originmaxs[2]);
						}
						if(tintmins)
						{
							tintlerp = lhrandom(0, 1);
							Vector4Lerp(tintmins, tintlerp, tintmaxs, tint);
						}
						VectorRandom(rvec);
						part = CL_NewParticle(center, info->particletype, info->color[0], info->color[1], tex, lhrandom(info->size[0], info->size[1]), info->size[2], lhrandom(info->alpha[0], info->alpha[1]), info->alpha[2], info->gravity, info->bounce, trailpos[0] + info->originoffset[0] + info->originjitter[0] * rvec[0], trailpos[1] + info->originoffset[1] + info->originjitter[1] * rvec[1], trailpos[2] + info->originoffset[2] + info->originjitter[2] * rvec[2], lhrandom(velocitymins[0], velocitymaxs[0]) * info->velocitymultiplier + info->velocityoffset[0] + info->velocityjitter[0] * rvec[0] + velocity[0], lhrandom(velocitymins[1], velocitymaxs[1]) * info->velocitymultiplier + info->velocityoffset[1] + info->velocityjitter[1] * rvec[1] + velocity[1], lhrandom(velocitymins[2], velocitymaxs[2]) * info->velocitymultiplier + info->velocityoffset[2] + info->velocityjitter[2] * rvec[2] + velocity[2], info->airfriction, info->liquidfriction, 0, 0, info->countabsolute <= 0, lhrandom(info->time[0], info->time[1]), info->stretchfactor, info->blendmode, info->orientation, info->staincolor[0], info->staincolor[1], staintex, lhrandom(info->stainalpha[0], info->stainalpha[1]), lhrandom(info->stainsize[0], info->stainsize[1]), lhrandom(info->rotate[0], info->rotate[1]), lhrandom(info->rotate[2], info->rotate[3]), tintmins ? tint : NULL);
						// SEPTEMBER S5: a delayed layer. delayedspawn is what the rain
						// splash already uses; the die is pushed by the same amount so
						// the layer keeps its authored lifetime from the moment it shows.
						if (part && (info->delay[0] > 0.0f || info->delay[1] > 0.0f))
						{
							float dly = lhrandom(min(info->delay[0], info->delay[1]), max(info->delay[0], info->delay[1]));
							part->delayedspawn = cl.time + dly;
							part->die += dly;
						}
						if (immediatebloodstain && part)
						{
							immediatebloodstain = false;
							CL_ImmediateBloodStain(part);
						}
						if (trailstep)
							VectorMA(trailpos, trailstep, traildir, trailpos);
					}
				}
			}
		}
	}
	if (!found)
		CL_ParticleEffect_Fallback(effectnameindex, pcount, originmins, originmaxs, velocitymins, velocitymaxs, ent, palettecolor, spawndlight, spawnparticles, wanttrail);
}

void CL_ParticleTrail(int effectnameindex, float pcount, const vec3_t originmins, const vec3_t originmaxs, const vec3_t velocitymins, const vec3_t velocitymaxs, entity_t *ent, int palettecolor, qbool spawndlight, qbool spawnparticles, float tintmins[4], float tintmaxs[4], float fade)
{
	CL_NewParticlesFromEffectinfo(effectnameindex, pcount, originmins, originmaxs, velocitymins, velocitymaxs, ent, palettecolor, spawndlight, spawnparticles, tintmins, tintmaxs, fade, true);
}

void CL_ParticleBox(int effectnameindex, float pcount, const vec3_t originmins, const vec3_t originmaxs, const vec3_t velocitymins, const vec3_t velocitymaxs, entity_t *ent, int palettecolor, qbool spawndlight, qbool spawnparticles, float tintmins[4], float tintmaxs[4], float fade)
{
	CL_NewParticlesFromEffectinfo(effectnameindex, pcount, originmins, originmaxs, velocitymins, velocitymaxs, ent, palettecolor, spawndlight, spawnparticles, tintmins, tintmaxs, fade, false);
}

// note: this one ONLY does boxes!
void CL_ParticleEffect(int effectnameindex, float pcount, const vec3_t originmins, const vec3_t originmaxs, const vec3_t velocitymins, const vec3_t velocitymaxs, entity_t *ent, int palettecolor)
{
	CL_ParticleBox(effectnameindex, pcount, originmins, originmaxs, velocitymins, velocitymaxs, ent, palettecolor, true, true, NULL, NULL, 1);
}

/*
===============
CL_EntityParticles
===============
*/
void CL_EntityParticles (const entity_t *ent)
{
	int i, j;
	vec_t pitch, yaw, dist = 64, beamlength = 16;
	vec3_t org, v;
	static vec3_t avelocities[NUMVERTEXNORMALS];
	if (!cl_particles.integer) return;
	if (cl.time <= cl.oldtime) return; // don't spawn new entity particles while paused

	Matrix4x4_OriginFromMatrix(&ent->render.matrix, org);

	if (!avelocities[0][0])
		for (i = 0;i < NUMVERTEXNORMALS;i++)
			for (j = 0;j < 3;j++)
				avelocities[i][j] = lhrandom(0, 2.55);

	for (i = 0;i < NUMVERTEXNORMALS;i++)
	{
		yaw = cl.time * avelocities[i][0];
		pitch = cl.time * avelocities[i][1];
		v[0] = org[0] + m_bytenormals[i][0] * dist + (cos(pitch)*cos(yaw)) * beamlength;
		v[1] = org[1] + m_bytenormals[i][1] * dist + (cos(pitch)*sin(yaw)) * beamlength;
		v[2] = org[2] + m_bytenormals[i][2] * dist + (-sin(pitch)) * beamlength;
		CL_NewParticle(org, pt_entityparticle, particlepalette[0x6f], particlepalette[0x6f], tex_particle, 1, 0, 255, 0, 0, 0, v[0], v[1], v[2], 0, 0, 0, 0, 0, 0, 0, true, 0, 1, PBLEND_ALPHA, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
	}
}


void CL_ReadPointFile_f(cmd_state_t *cmd)
{
	double org[3], leakorg[3];
	vec3_t vecorg;
	int r, c, s;
	char *pointfile = NULL, *pointfilepos, *t, tchar;
	char name[MAX_QPATH];

	if (!cl.worldmodel)
		return;

	dpsnprintf(name, sizeof(name), "%s.pts", cl.worldnamenoextension);
	pointfile = (char *)FS_LoadFile(name, tempmempool, true, NULL);
	if (!pointfile)
	{
		Con_Printf("Could not open %s\n", name);
		return;
	}

	Con_Printf("Reading %s...\n", name);
	VectorClear(leakorg);
	c = 0;
	s = 0;
	pointfilepos = pointfile;
	while (*pointfilepos)
	{
		while (*pointfilepos == '\n' || *pointfilepos == '\r')
			pointfilepos++;
		if (!*pointfilepos)
			break;
		t = pointfilepos;
		while (*t && *t != '\n' && *t != '\r')
			t++;
		tchar = *t;
		*t = 0;
#if _MSC_VER >= 1400
#define sscanf sscanf_s
#endif
		r = sscanf (pointfilepos,"%lf %lf %lf", &org[0], &org[1], &org[2]);
		VectorCopy(org, vecorg);
		*t = tchar;
		pointfilepos = t;
		if (r != 3)
			break;
		if (c == 0)
			VectorCopy(org, leakorg);
		c++;

		if (cl.num_particles < cl.max_particles - 3)
		{
			s++;
			CL_NewParticle(vecorg, pt_alphastatic, particlepalette[(-c)&15], particlepalette[(-c)&15], tex_particle, 2, 0, 255, 0, 0, 0, org[0], org[1], org[2], 0, 0, 0, 0, 0, 0, 0, true, 1<<30, 1, PBLEND_ALPHA, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
		}
	}
	Mem_Free(pointfile);
	VectorCopy(leakorg, vecorg);
	Con_Printf("%i points read (%i particles spawned)\nLeak at %f %f %f\n", c, s, leakorg[0], leakorg[1], leakorg[2]);

	if (c == 0)
	{
		return;
	}

	CL_NewParticle(vecorg, pt_beam, 0xFF0000, 0xFF0000, tex_beam, 64, 0, 255, 0, 0, 0, org[0] - 4096, org[1], org[2], org[0] + 4096, org[1], org[2], 0, 0, 0, 0, false, 1<<30, 1, PBLEND_ADD, PARTICLE_HBEAM, -1, -1, -1, 1, 1, 0, 0, NULL);
	CL_NewParticle(vecorg, pt_beam, 0x00FF00, 0x00FF00, tex_beam, 64, 0, 255, 0, 0, 0, org[0], org[1] - 4096, org[2], org[0], org[1] + 4096, org[2], 0, 0, 0, 0, false, 1<<30, 1, PBLEND_ADD, PARTICLE_HBEAM, -1, -1, -1, 1, 1, 0, 0, NULL);
	CL_NewParticle(vecorg, pt_beam, 0x0000FF, 0x0000FF, tex_beam, 64, 0, 255, 0, 0, 0, org[0], org[1], org[2] - 4096, org[0], org[1], org[2] + 4096, 0, 0, 0, 0, false, 1<<30, 1, PBLEND_ADD, PARTICLE_HBEAM, -1, -1, -1, 1, 1, 0, 0, NULL);
}

/*
===============
CL_ParseParticleEffect

Parse an effect out of the server message
===============
*/
void CL_ParseParticleEffect (void)
{
	vec3_t org, dir;
	int i, count, msgcount, color;

	MSG_ReadVector(&cl_message, org, cls.protocol);
	for (i=0 ; i<3 ; i++)
		dir[i] = MSG_ReadChar(&cl_message) * (1.0 / 16.0);
	msgcount = MSG_ReadByte(&cl_message);
	color = MSG_ReadByte(&cl_message);

	if (msgcount == 255)
		count = 1024;
	else
		count = msgcount;

	CL_ParticleEffect(EFFECT_SVC_PARTICLE, count, org, org, dir, dir, NULL, color);
}

/*
===============
CL_ParticleExplosion

===============
*/
void CL_ParticleExplosion (const vec3_t org)
{
	int i;
	trace_t trace;

	R_Stain(org, 96, 40, 40, 40, 64, 88, 88, 88, 64);
	CL_SpawnDecalParticleForPoint(org, 40, 48, 255, tex_bulletdecal[rand()&7], 0xFFFFFF, 0xFFFFFF);

	if (cl_particles_quake.integer)
	{
		for (i = 0; i < 1024; i++)
		{
			int color;
			int r = rand()&3;

			if (i & 1)
			{
				color = particlepalette[ramp1[r]];

				CL_NewQuakeParticle(
					org,
					pt_explode,
					color, color,
					0.05,                        // gravity
					org[0], org[1], org[2],      // offset
					0, 0, 0,                     // velocity
					2,                           // air friction
					0,                           // liquid friction
					16,                          // origin jitter
					256,                         // velocity jitter
					5                            // lifetime
				);
			}
			else
			{
				color = particlepalette[ramp2[r]];

				CL_NewQuakeParticle(
					org,
					pt_explode2,
					color, color,
					0.05,                        // gravity
					org[0], org[1], org[2],      // offset
					0, 0, 0,                     // velocity
					0,                           // air friction
					0,                           // liquid friction
					16,                          // origin jitter
					256,                         // velocity jitter
					5                            // lifetime
				);
			}
		}
	}
	else
	{
		i = CL_PointSuperContents(org);
		if (i & (SUPERCONTENTS_SLIME | SUPERCONTENTS_WATER))
		{
			if (cl_particles.integer && cl_particles_bubbles.integer)
				for (i = 0;i < 128 * cl_particles_quality.value;i++)
					CL_NewParticle(org, pt_bubble, 0x404040, 0x808080, tex_bubble, 2, 0, lhrandom(128, 255), 128, -0.125, 1.5, org[0], org[1], org[2], 0, 0, 0, 0.0625, 0.25, 16, 96, true, 0, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
		}
		else
		{
			if (cl_particles.integer && cl_particles_sparks.integer && cl_particles_explosions_sparks.integer)
			{
				for (i = 0;i < 512 * cl_particles_quality.value;i++)
				{
					int k = 0;
					vec3_t v, v2;
					do
					{
						VectorRandom(v2);
						VectorMA(org, 128, v2, v);
						trace = CL_TraceLine(org, v, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, 0, collision_extendmovelength.value, true, false, NULL, false, false);
					}
					while (k++ < 16 && trace.fraction < 0.1f);
					VectorSubtract(trace.endpos, org, v2);
					VectorScale(v2, 2.0f, v2);
					CL_NewParticle(org, pt_spark, 0x903010, 0xFFD030, tex_particle, 1.0f, 0, lhrandom(0, 255), 512, 0, 0, org[0], org[1], org[2], v2[0], v2[1], v2[2], 0, 0, 0, 0, true, 0, 1, PBLEND_ADD, PARTICLE_SPARK, -1, -1, -1, 1, 1, 0, 0, NULL);
				}
			}
		}
	}

	if (cl_particles_explosions_shell.integer)
		R_NewExplosion(org);
}

/*
===============
CL_ParticleExplosion2

===============
*/
void CL_ParticleExplosion2 (const vec3_t org, int colorStart, int colorLength)
{
	int i, k;
	if (!cl_particles.integer) return;

	for (i = 0;i < 512 * cl_particles_quality.value;i++)
	{
		k = particlepalette[colorStart + (i % colorLength)];
		if (cl_particles_quake.integer)
			CL_NewQuakeParticle(org, pt_alphastatic, k, k, 0, org[0], org[1], org[2], 0, 0, 0, -4, -4, 16, 256, 0.3);
		else
			CL_NewParticle(org, pt_alphastatic, k, k, tex_particle, lhrandom(0.5, 1.5), 0, 255, 512, 0, 0, org[0], org[1], org[2], 0, 0, 0, lhrandom(1.5, 3), lhrandom(1.5, 3), 8, 192, true, 0, 1, PBLEND_ALPHA, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
	}
}


// BEAUTY A2 (2026-09-13): the stock bullet impact's grey blob (the round cell at
// size 3, additive) is a large soft disc at point-blank range -- Seb's demo45
// f950-f956, the white disc at the wall. Under m5_muzzleflash it is a small
// starburst instead, warm, gone in 70 ms; at 0 the old call, to the byte.
static void M5_ImpactFlash(const vec3_t center, const vec3_t originmins, const vec3_t originmaxs)
{
	if (m5_muzzleflash.integer)
		CL_NewParticle(center, pt_static, 0xFFE0C0, 0xFFC890, tex_flash, 1.1f, 0, 420, 6000, 0, 0, lhrandom(originmins[0], originmaxs[0]), lhrandom(originmins[1], originmaxs[1]), lhrandom(originmins[2], originmaxs[2]), 0, 0, 0, 0, 0, 0, 0, true, 0.07f, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, lhrandom(0, 360), 0, NULL);
	else
		CL_NewParticle(center, pt_static, 0x808080,0x808080, tex_particle, 3, 0, 256, 512, 0, 0, lhrandom(originmins[0], originmaxs[0]), lhrandom(originmins[1], originmaxs[1]), lhrandom(originmins[2], originmaxs[2]), 0, 0, 0, 0, 0, 0, 0, true, 0, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
}
static void CL_Sparks(const vec3_t originmins, const vec3_t originmaxs, const vec3_t velocitymins, const vec3_t velocitymaxs, float sparkcount)
{
	vec3_t center;
	// BEAUTY A2 (2026-09-13): the impact spray is what Seb's demo45 f954 shows
	// as wide yellow rays -- at point blank these sparks fly back to within a
	// few units of the eye, and the round blob at half a unit is a 50-pixel
	// ribbon there. Under m5_muzzleflash they take the hot-core streak cell at
	// a third of a unit; at 0 the call below is the old one to the byte.
	int sptex = m5_muzzleflash.integer ? tex_sparkhot : tex_particle;
	float spth = m5_muzzleflash.integer ? 0.3f : 0.5f;
	VectorMAM(0.5f, originmins, 0.5f, originmaxs, center);
	if (cl_particles_sparks.integer)
	{
		sparkcount *= cl_particles_quality.value;
		if (m5_muzzleflash.integer)
			sparkcount *= 0.7f;		// the streak cell reads every spark; the blob needed the extras
		while(sparkcount-- > 0)
			CL_NewParticle(center, pt_spark, particlepalette[0x68], particlepalette[0x6f], sptex, m5_muzzleflash.integer ? spth * lhrandom(0.55f, 1.6f) : spth, 0, lhrandom(64, 255), 512, 1, 0, lhrandom(originmins[0], originmaxs[0]), lhrandom(originmins[1], originmaxs[1]), lhrandom(originmins[2], originmaxs[2]), lhrandom(velocitymins[0], velocitymaxs[0]), lhrandom(velocitymins[1], velocitymaxs[1]), lhrandom(velocitymins[2], velocitymaxs[2]) + cl.movevars_gravity * 0.1f, 0, 0, 0, 64, true, 0, 1, PBLEND_ADD, PARTICLE_SPARK, -1, -1, -1, 1, 1, 0, 0, NULL);
	}
}

static void CL_Smoke(const vec3_t originmins, const vec3_t originmaxs, const vec3_t velocitymins, const vec3_t velocitymaxs, float smokecount)
{
	vec3_t center;
	VectorMAM(0.5f, originmins, 0.5f, originmaxs, center);
	if (cl_particles_smoke.integer)
	{
		smokecount *= cl_particles_quality.value;
		while(smokecount-- > 0)
			CL_NewParticle(center, pt_smoke, 0x101010, 0x101010, tex_smoke[rand()&7], 2, 2, 255, 256, 0, 0, lhrandom(originmins[0], originmaxs[0]), lhrandom(originmins[1], originmaxs[1]), lhrandom(originmins[2], originmaxs[2]), lhrandom(velocitymins[0], velocitymaxs[0]), lhrandom(velocitymins[1], velocitymaxs[1]), lhrandom(velocitymins[2], velocitymaxs[2]), 0, 0, 0, smokecount > 0 ? 16 : 0, true, 0, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
	}
}

void CL_ParticleCube (const vec3_t mins, const vec3_t maxs, const vec3_t dir, int count, int colorbase, vec_t gravity, vec_t randomvel)
{
	vec3_t center;
	int k;
	if (!cl_particles.integer) return;
	VectorMAM(0.5f, mins, 0.5f, maxs, center);

	count = (int)(count * cl_particles_quality.value);
	while (count--)
	{
		k = particlepalette[colorbase + (rand()&3)];
		CL_NewParticle(center, pt_alphastatic, k, k, tex_particle, 2, 0, 255, 128, gravity, 0, lhrandom(mins[0], maxs[0]), lhrandom(mins[1], maxs[1]), lhrandom(mins[2], maxs[2]), dir[0], dir[1], dir[2], 0, 0, 0, randomvel, true, 0, 1, PBLEND_ALPHA, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
	}
}

void CL_ParticleRain (const vec3_t mins, const vec3_t maxs, const vec3_t dir, int count, int colorbase, int type)
{
	int k;
	float minz, maxz, lifetime = 30;
	float particle_size;
	vec3_t org;

	if (!cl_particles.integer) return;
	if (dir[2] < 0) // falling
	{
		minz = maxs[2] + dir[2] * 0.1;
		maxz = maxs[2];
		if (cl.worldmodel)
			lifetime = (maxz - cl.worldmodel->normalmins[2]) / max(1, -dir[2]);
	}
	else // rising??
	{
		minz = mins[2];
		maxz = maxs[2] + dir[2] * 0.1;
		if (cl.worldmodel)
			lifetime = (cl.worldmodel->normalmaxs[2] - minz) / max(1, dir[2]);
	}

	count = (int)(count * cl_particles_quality.value);

	switch(type)
	{
	case 0:
		if (!cl_particles_rain.integer) break;

		count *= 4; // ick, this should be in the mod or maps?
		particle_size = (gamemode == GAME_GOODVSBAD2) ? 20 : 0.5;

		while(count--)
		{
			k = particlepalette[colorbase + (rand()&3)];
			VectorSet(org, lhrandom(mins[0], maxs[0]), lhrandom(mins[1], maxs[1]), lhrandom(minz, maxz));
			CL_NewParticle(org, pt_rain, k, k, tex_particle, particle_size, 0, lhrandom(32, 64), 0, 0, -1, org[0], org[1], org[2], dir[0], dir[1], dir[2], 0, 0, 0, 0, true, lifetime, 1, PBLEND_ADD, PARTICLE_SPARK, -1, -1, -1, 1, 1, 0, 0, NULL);
		}
		break;
	case 1:
		if (!cl_particles_snow.integer) break;

		particle_size = (gamemode == GAME_GOODVSBAD2) ? 20 : 1.0;

		while(count--)
		{
			k = particlepalette[colorbase + (rand()&3)];
			VectorSet(org, lhrandom(mins[0], maxs[0]), lhrandom(mins[1], maxs[1]), lhrandom(minz, maxz));
			CL_NewParticle(org, pt_snow, k, k, tex_particle, 1, 0, lhrandom(64, 128), 0, 0, -1, org[0], org[1], org[2], dir[0], dir[1], dir[2], 0, 0, 0, 0, true, lifetime, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL);
		}
		break;
	default:
		Con_Printf ("CL_ParticleRain: unknown type %i (0 = rain, 1 = snow)\n", type);
	}
}

cvar_t r_drawparticles = {CF_CLIENT, "r_drawparticles", "1", "enables drawing of particles"};
static cvar_t r_drawparticles_drawdistance = {CF_CLIENT | CF_ARCHIVE, "r_drawparticles_drawdistance", "2000", "particles further than drawdistance*size will not be drawn"};
static cvar_t r_drawparticles_nearclip_min = {CF_CLIENT | CF_ARCHIVE, "r_drawparticles_nearclip_min", "4", "particles closer than drawnearclip_min will not be drawn"};
static cvar_t r_drawparticles_nearclip_max = {CF_CLIENT | CF_ARCHIVE, "r_drawparticles_nearclip_max", "4", "particles closer than drawnearclip_min will be faded"};
cvar_t r_drawdecals = {CF_CLIENT, "r_drawdecals", "1", "enables drawing of decals"};
static cvar_t r_drawdecals_drawdistance = {CF_CLIENT | CF_ARCHIVE, "r_drawdecals_drawdistance", "500", "decals further than drawdistance*size will not be drawn"};

// BEAUTY A2 (2026-09-16): the cell size is a RUNTIME value now (64 or 256,
// cl_particles_texsize); every generator below is written against these two
// names and the half-cell PT_HALF, so a feather of N texels stays N texels at
// either size (at 64, PT_HALF is exactly the 31.0f the constants used to carry).
#define PARTICLETEXTURESIZE particletexsize
#define PARTICLEFONTSIZE (particletexsize*8)
#define PT_HALF ((float)particletexsize*0.5f-1.0f)

static unsigned char shadebubble(float dx, float dy, vec3_t light)
{
	float dz, f, dot;
	vec3_t normal;
	dz = 1 - (dx*dx+dy*dy);
	if (dz > 0) // it does hit the sphere
	{
		f = 0;
		// back side
		normal[0] = dx;normal[1] = dy;normal[2] = dz;
		VectorNormalize(normal);
		dot = DotProduct(normal, light);
		if (dot > 0.5) // interior reflection
			f += ((dot *  2) - 1);
		else if (dot < -0.5) // exterior reflection
			f += ((dot * -2) - 1);
		// front side
		normal[0] = dx;normal[1] = dy;normal[2] = -dz;
		VectorNormalize(normal);
		dot = DotProduct(normal, light);
		if (dot > 0.5) // interior reflection
			f += ((dot *  2) - 1);
		else if (dot < -0.5) // exterior reflection
			f += ((dot * -2) - 1);
		f *= 128;
		f += 16; // just to give it a haze so you can see the outline
		f = bound(0, f, 255);
		return (unsigned char) f;
	}
	else
		return 0;
}

int particlefontwidth, particlefontheight, particlefontcellwidth, particlefontcellheight, particlefontrows, particlefontcols;
static void CL_Particle_PixelCoordsForTexnum(int texnum, int *basex, int *basey, int *width, int *height)
{
	*basex = (texnum % particlefontcols) * particlefontcellwidth;
	*basey = ((texnum / particlefontcols) % particlefontrows) * particlefontcellheight;
	*width = particlefontcellwidth;
	*height = particlefontcellheight;
}

static void setuptex(int texnum, unsigned char *data, unsigned char *particletexturedata)
{
	int basex, basey, w, h, y;
	CL_Particle_PixelCoordsForTexnum(texnum, &basex, &basey, &w, &h);
	if(w != PARTICLETEXTURESIZE || h != PARTICLETEXTURESIZE)
		Sys_Error("invalid particle texture size for autogenerating");
	for (y = 0;y < PARTICLETEXTURESIZE;y++)
		memcpy(particletexturedata + ((basey + y) * PARTICLEFONTSIZE + basex) * 4, data + y * PARTICLETEXTURESIZE * 4, PARTICLETEXTURESIZE * 4);
}

static void particletextureblotch(unsigned char *data, float radius, float red, float green, float blue, float alpha)
{
	int x, y;
	float cx, cy, dx, dy, f, iradius;
	unsigned char *d;
	cx = (lhrandom(radius + 1, PARTICLETEXTURESIZE - 2 - radius) + lhrandom(radius + 1, PARTICLETEXTURESIZE - 2 - radius)) * 0.5f;
	cy = (lhrandom(radius + 1, PARTICLETEXTURESIZE - 2 - radius) + lhrandom(radius + 1, PARTICLETEXTURESIZE - 2 - radius)) * 0.5f;
	iradius = 1.0f / radius;
	alpha *= (1.0f / 255.0f);
	for (y = 0;y < PARTICLETEXTURESIZE;y++)
	{
		for (x = 0;x < PARTICLETEXTURESIZE;x++)
		{
			dx = (x - cx);
			dy = (y - cy);
			f = (1.0f - sqrt(dx * dx + dy * dy) * iradius) * alpha;
			if (f > 0)
			{
				if (f > 1)
					f = 1;
				d = data + (y * PARTICLETEXTURESIZE + x) * 4;
				d[0] += (int)(f * (blue  - d[0]));
				d[1] += (int)(f * (green - d[1]));
				d[2] += (int)(f * (red   - d[2]));
			}
		}
	}
}

#if 0
static void particletextureclamp(unsigned char *data, int minr, int ming, int minb, int maxr, int maxg, int maxb)
{
	int i;
	for (i = 0;i < PARTICLETEXTURESIZE*PARTICLETEXTURESIZE;i++, data += 4)
	{
		data[0] = bound(minb, data[0], maxb);
		data[1] = bound(ming, data[1], maxg);
		data[2] = bound(minr, data[2], maxr);
	}
}
#endif

static void particletextureinvert(unsigned char *data)
{
	int i;
	for (i = 0;i < PARTICLETEXTURESIZE*PARTICLETEXTURESIZE;i++, data += 4)
	{
		data[0] = 255 - data[0];
		data[1] = 255 - data[1];
		data[2] = 255 - data[2];
	}
}

// Those loops are in a separate function to work around an optimization bug in Mac OS X's GCC
static void R_InitBloodTextures (unsigned char *particletexturedata)
{
	int i, j, k, m;
	size_t datasize = PARTICLETEXTURESIZE*PARTICLETEXTURESIZE*4;
	unsigned char *data = (unsigned char *)Mem_Alloc(tempmempool, datasize);

	// blood particles
	for (i = 0;i < 8;i++)
	{
		memset(data, 255, datasize);
		for (k = 0;k < 24;k++)
			particletextureblotch(data, PARTICLETEXTURESIZE/16, 96, 0, 0, 160);
		//particletextureclamp(data, 32, 32, 32, 255, 255, 255);
		particletextureinvert(data);
		setuptex(tex_bloodparticle[i], data, particletexturedata);
	}

	// blood decals
	for (i = 0;i < 8;i++)
	{
		memset(data, 255, datasize);
		m = 8;
		for (j = 1;j < 10;j++)
			for (k = min(j, m - 1);k < m;k++)
				particletextureblotch(data, (float)j*PARTICLETEXTURESIZE/64.0f, 96, 0, 0, 320 - j * 8);
		//particletextureclamp(data, 32, 32, 32, 255, 255, 255);
		particletextureinvert(data);
		setuptex(tex_blooddecal[i], data, particletexturedata);
	}

	Mem_Free(data);
}

//uncomment this to make engine save out particle font to a tga file when run
//#define DUMPPARTICLEFONT

static float M5_SdRoundBox(float px, float py, float cx, float cy, float hx, float hy, float r)
{
	// signed distance to a rounded box centred (cx,cy) with half-extents (hx,hy)
	float qx = fabs(px - cx) - (hx - r), qy = fabs(py - cy) - (hy - r);
	float ox = max(qx, 0.0f), oy = max(qy, 0.0f);
	return sqrt(ox*ox + oy*oy) + min(max(qx, qy), 0.0f) - r;
}

/*
===============
M5_InitShellCasingTexture

SEPTEMBER S6. A 12-gauge hull for the Doom-shotgun casing, in atlas cell 34: a
RED plastic body with faint ribbing and a star-crimped top, and a BRASS head --
Seb's reference photo, not a guess. The casing has ALWAYS tumbled (the spawn
passes a random start angle and +/-400 deg/s of spin and the billboard
integrates them every frame); it was invisible because tex_particle is a
radially symmetric blob and looks the same at every angle. The texture was
missing, not the machinery.

The cell carries its own HUE, which the blob never did: the two-tone shell
cannot come from one vertex colour, so the on-arm spawns with a near-white
vertex colour and the texture's red and brass pass straight through (the blood
decals are the precedent for authored RGB in this font). Alpha is coverage
ONLY -- the silhouette plus a ~2.5-texel feather, because the font has no
mipmaps and is forced linear, so a hard edge crawls as the casing spins. With
GL_ONE / GL_ONE_MINUS_SRC_ALPHA the source alpha IS coverage, so shading must
never be authored into alpha or the rim goes see-through; the cylinder shading
and the ribbing live in RGB, floored well above black. TEXF_RGBMULTIPLYBYALPHA
premultiplies at upload, so straight RGB + A is the right thing to write.

The head is wider and a different colour from the body on purpose: a symmetric
lozenge is indistinguishable from itself half a turn later and the tumble
would read as a wobble. Prototyped in Python at 130 / 60 / 25 px across a
rotation sweep before this was written -- the bolt-noise precedent.

`stretch` must stay 1 on the casing: under spin the billboard's right/up pair
stays orthonormal and merely pulses in length, so stretch is a size pulse,
not an aspect ratio. The aspect comes from this cell.

The unused cell is provably inert: cells are 64-aligned in the 512x512 font,
UVs are inset one texel and there are no mipmaps, so cell 34's bilinear taps
never leave texels 128-191 and cell 33's never reach 128.
===============
*/
static void M5_InitShellCasingTexture(unsigned char *data, unsigned char *particletexturedata)
{
	const float L = 0.90f, W = 0.26f, WH = 0.30f, HEADFRAC = 0.20f, FEATHER = 2.5f / PT_HALF;
	const float JOINT = L - 2.0f * L * HEADFRAC;	// where the body meets the brass head (y grows downward)
	const float red[3] = {0.80f, 0.16f, 0.12f}, brass[3] = {0.86f, 0.68f, 0.30f};
	const float *base;
	int x, y, k;
	float dx, dy, dbody, dhead, a, w, t, shade, g;
	memset(data, 255, PARTICLETEXTURESIZE*PARTICLETEXTURESIZE*4);
	for (y = 0;y < PARTICLETEXTURESIZE;y++)
	{
		dy = (y - 0.5f*PARTICLETEXTURESIZE) / (PARTICLETEXTURESIZE*0.5f-1);
		for (x = 0;x < PARTICLETEXTURESIZE;x++)
		{
			dx = (x - 0.5f*PARTICLETEXTURESIZE) / (PARTICLETEXTURESIZE*0.5f-1);
			// body: a FLAT crimped top with small corners, down to the joint; head: joint to the base
			dbody = M5_SdRoundBox(dx, dy, 0.0f, (-L + JOINT) * 0.5f, W, (JOINT + L) * 0.5f, 0.07f);
			dhead = M5_SdRoundBox(dx, dy, 0.0f, (JOINT + L) * 0.5f, WH, (L - JOINT) * 0.5f, 0.05f);
			a = bound(0.0f, 0.5f - min(dbody, dhead) / FEATHER, 1.0f);
			// grey cylinder shading across the width, highlight slightly off-axis
			w = (dy > JOINT) ? WH : W;
			t = bound(-1.0f, dx / w + 0.25f, 1.0f);
			shade = 0.42f + 0.58f * pow(1.0f - t * t, 0.35);
			if (dy <= JOINT)
			{
				shade *= 0.93f + 0.07f * cos(dx / W * (float)M_PI * 9.0f);	// the ribbing, faint
				if (dy < -L + 0.16f)
					shade *= 0.72f + 0.28f * ((dy + L) / 0.16f);	// the crimp folds darken the top
			}
			g = fabs(dy - JOINT);
			if (g < 0.05f)
				shade *= 0.5f + 0.5f * (g / 0.05f);	// the groove at the joint
			base = (dy > JOINT) ? brass : red;
			for (k = 0;k < 3;k++)
				data[(y*PARTICLETEXTURESIZE+x)*4+(2-k)] = (unsigned char)(bound(0.0f, base[k] * shade, 1.0f) * 255.0f + 0.5f);	// BGRA
			data[(y*PARTICLETEXTURESIZE+x)*4+3] = (unsigned char)(a * 255.0f + 0.5f);
		}
	}
	setuptex(tex_shellcasing_cell, data, particletexturedata);
}

static void M5_InitDustAndRingTextures(unsigned char *data, unsigned char *particletexturedata)
{
	int x, y;
	float dx, dy, r, a;
	const float FEATHER = 1.5f / PT_HALF;
	// DUST: a disc of radius 0.42 (of the half-cell), feathered 1.5 texels, white.
	// Alpha IS coverage under the alpha blend, so this is a crisp dot at any size.
	memset(data, 255, PARTICLETEXTURESIZE*PARTICLETEXTURESIZE*4);
	for (y = 0;y < PARTICLETEXTURESIZE;y++)
	{
		dy = (y - 0.5f*PARTICLETEXTURESIZE) / (PARTICLETEXTURESIZE*0.5f-1);
		for (x = 0;x < PARTICLETEXTURESIZE;x++)
		{
			dx = (x - 0.5f*PARTICLETEXTURESIZE) / (PARTICLETEXTURESIZE*0.5f-1);
			r = sqrt(dx*dx + dy*dy);
			a = bound(0.0f, (0.42f - r) / FEATHER + 0.5f, 1.0f);
			data[(y*PARTICLETEXTURESIZE+x)*4+3] = (unsigned char)(a * 255.0f + 0.5f);
		}
	}
	setuptex(tex_dust_cell, data, particletexturedata);
	// RING: annulus centred at r 0.70, half-width 0.10, soft-edged, over a faint
	// fill (0.12) so the wave has a body; additive, so alpha here is brightness.
	memset(data, 255, PARTICLETEXTURESIZE*PARTICLETEXTURESIZE*4);
	for (y = 0;y < PARTICLETEXTURESIZE;y++)
	{
		dy = (y - 0.5f*PARTICLETEXTURESIZE) / (PARTICLETEXTURESIZE*0.5f-1);
		for (x = 0;x < PARTICLETEXTURESIZE;x++)
		{
			float ring, fill;
			dx = (x - 0.5f*PARTICLETEXTURESIZE) / (PARTICLETEXTURESIZE*0.5f-1);
			r = sqrt(dx*dx + dy*dy);
			ring = bound(0.0f, 1.0f - fabs(r - 0.70f) / 0.10f, 1.0f);
			ring = ring * ring * (3.0f - 2.0f * ring);
			fill = (r < 0.70f) ? 0.12f * (1.0f - r / 0.70f) : 0.0f;
			a = bound(0.0f, ring + fill, 1.0f) * bound(0.0f, (0.98f - r) / FEATHER, 1.0f);
			data[(y*PARTICLETEXTURESIZE+x)*4+3] = (unsigned char)(a * 255.0f + 0.5f);
		}
	}
	setuptex(tex_ring_cell, data, particletexturedata);
}

/*
===============
M5_InitFlashAndSparkTextures  (BEAUTY A2/A3, 2026-09-13)

Cell 37, the FLASH: a hot gaussian core, a faint body, and nine rays of uneven
angle, length and width fixed by a hash so every boot draws the same cell.
Cell 38, the SPARK STREAK: a thin bright line along s (the axis PARTICLE_SPARK
stretches along the velocity), a hotter thread inside it, tapered to both
ends. Both additive: alpha is brightness, RGB stays white for the vertex tint.
===============
*/
static float M5_Hash01(unsigned int n)
{
	n = (n ^ 61u) ^ (n >> 16); n *= 9u; n ^= n >> 4; n *= 0x27d4eb2du; n ^= n >> 15;
	return (float)(n & 0xFFFFFFu) * (1.0f / 16777216.0f);
}

static void M5_InitFlashAndSparkTextures(unsigned char *data, unsigned char *particletexturedata)
{
	enum { NRAYS = 10 };
	int x, y, k;
	float dx, dy, r, ang, a, core, disc, rays, d, w, env;
	float rayang[NRAYS], raylen[NRAYS], raywid[NRAYS], raygain[NRAYS];
	const float FEATHER = 1.5f / PT_HALF;
	// The first cut was a lens star: a pinpoint core and hairline rays, 2.6% of
	// the cell lit, which on screen at a hundred pixels across was a dot. A
	// muzzle flash is a fat irregular BODY with tongues. Rendered offline first
	// (the BLUENOISE rule): this fills ~10% of the cell, the body solid to
	// 0.45 of the half-size, ten tongues of uneven length and width beyond it.
	for (k = 0; k < NRAYS; k++)
	{
		rayang[k]  = ((float)k + 0.6f * M5_Hash01((unsigned)k * 7u + 1u)) * (2.0f * (float)M_PI / NRAYS);
		raylen[k]  = 0.50f + 0.48f * M5_Hash01((unsigned)k * 7u + 2u);
		raywid[k]  = 0.18f + 0.20f * M5_Hash01((unsigned)k * 7u + 3u);
		raygain[k] = 0.50f + 0.50f * M5_Hash01((unsigned)k * 7u + 4u);
	}
	memset(data, 255, PARTICLETEXTURESIZE*PARTICLETEXTURESIZE*4);
	for (y = 0;y < PARTICLETEXTURESIZE;y++)
	{
		dy = (y - 0.5f*PARTICLETEXTURESIZE) / (PARTICLETEXTURESIZE*0.5f-1);
		for (x = 0;x < PARTICLETEXTURESIZE;x++)
		{
			dx = (x - 0.5f*PARTICLETEXTURESIZE) / (PARTICLETEXTURESIZE*0.5f-1);
			r = sqrt(dx*dx + dy*dy);
			ang = atan2(dy, dx);
			core = exp(-(r*r) / (0.26f*0.26f));
			disc = (r < 0.45f) ? 0.85f * pow(1.0f - r/0.45f, 1.5) : 0.0f;		// the body
			rays = 0.0f;
			for (k = 0; k < NRAYS; k++)
			{
				d = ang - rayang[k];
				while (d >  (float)M_PI) d -= 2.0f * (float)M_PI;
				while (d < -(float)M_PI) d += 2.0f * (float)M_PI;
				w = raywid[k] * (0.5f + 0.5f * (1.0f - r));		// tongues narrow toward the tip
				env = (r < raylen[k]) ? pow(1.0f - r / raylen[k], 1.4) : 0.0f;
				rays += raygain[k] * exp(-(d*d) / (w*w)) * env;
			}
			a = bound(0.0f, core + disc + rays, 1.0f) * bound(0.0f, (0.98f - r) / FEATHER, 1.0f);
			data[(y*PARTICLETEXTURESIZE+x)*4+3] = (unsigned char)(a * 255.0f + 0.5f);
		}
	}
	setuptex(tex_flash_cell, data, particletexturedata);
	memset(data, 255, PARTICLETEXTURESIZE*PARTICLETEXTURESIZE*4);
	for (y = 0;y < PARTICLETEXTURESIZE;y++)
	{
		dy = (y - 0.5f*PARTICLETEXTURESIZE) / (PARTICLETEXTURESIZE*0.5f-1);
		for (x = 0;x < PARTICLETEXTURESIZE;x++)
		{
			dx = (x - 0.5f*PARTICLETEXTURESIZE) / (PARTICLETEXTURESIZE*0.5f-1);
			core = 0.55f * exp(-(dy*dy) / (0.11f*0.11f)) + 0.75f * exp(-(dy*dy) / (0.045f*0.045f));
			env = pow(max(0.0f, 1.0f - fabs(dx)), 0.5) * (0.35f + 0.65f * exp(-(dx*dx) / (0.35f*0.35f)));
			a = bound(0.0f, core * env, 1.0f) * bound(0.0f, (0.98f - fabs(dx)) / FEATHER, 1.0f);
			data[(y*PARTICLETEXTURESIZE+x)*4+3] = (unsigned char)(a * 255.0f + 0.5f);
		}
	}
	setuptex(tex_sparkhot_cell, data, particletexturedata);
}

/*
===============
M5_InitEmberAndDropletTextures  (BEAUTY A2, 2026-09-16)

Cell 39, the EMBER: a solid bright bead to 0.30 of the half-cell with a rim
that falls over two texels, a dim halo to 0.55, and a slight taper along s so
a PARTICLE_SPARK stretch reads as a hot grain in flight rather than a comet.
Additive: alpha is brightness, RGB white for the vertex tint.
Cell 40, the DROPLET: one teardrop, its round end up the cell and its tail
down, dark red inside with a darker rim and a small highlight, WHITE outside,
then inverted like the blood cells -- pt_blood draws PBLEND_INVMOD, so the
inverted white is black and black under that blend leaves the wall alone.
Alpha stays 255 throughout (the blood cells' convention; the vertex alpha
carries the fade).
===============
*/
static void M5_InitEmberAndDropletTextures(unsigned char *data, unsigned char *particletexturedata)
{
	int x, y;
	float dx, dy, r, a, core, halo, taper, t, w, edge, shade;
	const float FEATHER = 1.5f / PT_HALF, RIM = 2.0f / PT_HALF;
	memset(data, 255, PARTICLETEXTURESIZE*PARTICLETEXTURESIZE*4);
	for (y = 0;y < PARTICLETEXTURESIZE;y++)
	{
		dy = (y - 0.5f*PARTICLETEXTURESIZE) / PT_HALF;
		for (x = 0;x < PARTICLETEXTURESIZE;x++)
		{
			dx = (x - 0.5f*PARTICLETEXTURESIZE) / PT_HALF;
			r = sqrt(dx*dx + dy*dy);
			core = bound(0.0f, (0.30f - r) / RIM + 0.5f, 1.0f);
			halo = (r < 0.55f) ? 0.30f * (1.0f - r / 0.55f) : 0.0f;
			taper = 1.0f - 0.25f * fabs(dx);
			a = bound(0.0f, core + halo, 1.0f) * taper * bound(0.0f, (0.98f - r) / FEATHER, 1.0f);
			data[(y*PARTICLETEXTURESIZE+x)*4+3] = (unsigned char)(a * 255.0f + 0.5f);
		}
	}
	setuptex(tex_ember_cell, data, particletexturedata);
	memset(data, 255, PARTICLETEXTURESIZE*PARTICLETEXTURESIZE*4);
	for (y = 0;y < PARTICLETEXTURESIZE;y++)
	{
		dy = (y - 0.5f*PARTICLETEXTURESIZE) / PT_HALF;
		for (x = 0;x < PARTICLETEXTURESIZE;x++)
		{
			dx = (x - 0.5f*PARTICLETEXTURESIZE) / PT_HALF;
			// the drop: an ellipse of half-height 0.48 centred 0.06 up the cell,
			// its width pinched toward the tail (dy > 0 is DOWN the cell, the
			// tail), so it reads as a falling drop
			t = (dy + 0.06f) / 0.48f;
			w = (fabs(t) < 1.0f) ? 0.30f * sqrt(1.0f - t*t) : 0.0f;
			if (dy > -0.06f)
				w *= 1.0f - 0.55f * bound(0.0f, (dy + 0.06f) / 0.54f, 1.0f);
			edge = w - fabs(dx);				// > 0 inside, in half-cell units
			a = bound(0.0f, edge / FEATHER + 0.5f, 1.0f);	// coverage
			// inside: dark red, darker within two texels of the edge, a highlight up and left
			shade = 0.55f + 0.45f * bound(0.0f, edge / RIM, 1.0f);
			shade *= 1.0f + 0.35f * exp(-((dx + 0.10f)*(dx + 0.10f) + (dy + 0.22f)*(dy + 0.22f)) / (0.08f*0.08f));
			// blend the drop's colour over white by coverage (BGRA); inverted below
			data[(y*PARTICLETEXTURESIZE+x)*4+2] = (unsigned char)(255 - (int)(a * (255.0f - bound(0.0f, 96.0f * shade, 255.0f)) + 0.5f));
			data[(y*PARTICLETEXTURESIZE+x)*4+1] = (unsigned char)(255 - (int)(a * (255.0f - bound(0.0f, 4.0f * shade, 255.0f)) + 0.5f));
			data[(y*PARTICLETEXTURESIZE+x)*4+0] = (unsigned char)(255 - (int)(a * (255.0f - bound(0.0f, 6.0f * shade, 255.0f)) + 0.5f));
			data[(y*PARTICLETEXTURESIZE+x)*4+3] = 255;
		}
	}
	particletextureinvert(data);	// dark red on white -> the inverted form pt_blood's blend expects
	setuptex(tex_droplet_cell, data, particletexturedata);
}

// BEAUTY A2: the airborne blood's cell -- the droplet when it is on and the
// engine's own font is in use, one of the eight blotch cells otherwise.
static int M5_BloodTex(void)
{
	if (cl_particles_blood_droplet.integer && tex_droplet >= 0)
		return tex_droplet;
	return tex_bloodparticle[rand()&7];
}

// The droplet takes a random angle; the blotch cells keep their 0 and draw no
// extra random number, so cl_particles_blood_droplet 0 leaves the rand()
// stream -- and therefore today's frames -- untouched.
static float M5_BloodAngle(void)
{
	if (cl_particles_blood_droplet.integer && tex_droplet >= 0)
		return lhrandom(0, 360);
	return 0;
}

static void R_InitParticleTexture (void)
{
	int x, y, d, i, k, m;
	int basex, basey, w, h;
	float dx, dy, f, s1, t1, s2, t2;
	vec3_t light;
	char *buf;
	fs_offset_t filesize;
	char texturename[MAX_QPATH];
	skinframe_t *sf;

	// a note: decals need to modulate (multiply) the background color to
	// properly darken it (stain), and they need to be able to alpha fade,
	// this is a very difficult challenge because it means fading to white
	// (no change to background) rather than black (darkening everything
	// behind the whole decal polygon), and to accomplish this the texture is
	// inverted (dark red blood on white background becomes brilliant cyan
	// and white on black background) so we can alpha fade it to black, then
	// we invert it again during the blendfunc to make it work...

#ifndef DUMPPARTICLEFONT
	decalskinframe = R_SkinFrame_LoadExternal("particles/particlefont.tga", TEXF_ALPHA | TEXF_FORCELINEAR | TEXF_RGBMULTIPLYBYALPHA, false, false);
	if (decalskinframe)
	{
		particlefonttexture = decalskinframe->base;
		// TODO maybe allow custom grid size?
		particlefontwidth = image_width;
		particlefontheight = image_height;
		particlefontcellwidth = image_width / 8;
		particlefontcellheight = image_height / 8;
		particlefontcols = 8;
		particlefontrows = 8;
		// SEPTEMBER S6: an external font replaces the atlas wholesale and no
		// procedural cell exists, so the casing goes back to the round blob.
		tex_shellcasing = tex_particle;
		tex_dust = tex_particle;   // SEPTEMBER S7: same fallback as the casing
		tex_ring = tex_particle;   // SEPTEMBER S5: ditto (the shockwave degrades to a soft flash)
		tex_flash = tex_particle;  // BEAUTY A3: ditto
		tex_sparkhot = tex_particle;
		tex_ember = tex_particle;  // BEAUTY A2: ditto, and the droplet goes back to the blotch cells
		tex_droplet = -1;
		particletexsize_applied = 0;	// an external font has no cell size of ours to re-apply
		Con_DPrintf("M5 shell casing: external particlefont replaces the atlas -- falling back to the round particle\n");
	}
	else
#endif
	{
		unsigned char *particletexturedata, *data, *noise1, *noise2;
		size_t datasize;
		// BEAUTY A2: the cell size is the cvar's, 64 or 256 and nothing else
		particletexsize = (cl_particles_texsize.integer >= 256) ? 256 : 64;
		particletexsize_applied = particletexsize;
		particletexturedata = (unsigned char *)Mem_Alloc(tempmempool, PARTICLEFONTSIZE*PARTICLEFONTSIZE*4);
		datasize = PARTICLETEXTURESIZE*PARTICLETEXTURESIZE*4;
		data = (unsigned char *)Mem_Alloc(tempmempool, datasize);
		noise1 = (unsigned char *)Mem_Alloc(tempmempool, PARTICLETEXTURESIZE*2*PARTICLETEXTURESIZE*2);
		noise2 = (unsigned char *)Mem_Alloc(tempmempool, PARTICLETEXTURESIZE*2*PARTICLETEXTURESIZE*2);

		particlefontwidth = particlefontheight = PARTICLEFONTSIZE;
		particlefontcellwidth = particlefontcellheight = PARTICLETEXTURESIZE;
		particlefontcols = 8;
		particlefontrows = 8;

		memset(particletexturedata, 255, PARTICLEFONTSIZE*PARTICLEFONTSIZE*4);

		// smoke
		for (i = 0;i < 8;i++)
		{
			memset(data, 255, datasize);
			do
			{
				fractalnoise(noise1, PARTICLETEXTURESIZE*2, PARTICLETEXTURESIZE/8);
				fractalnoise(noise2, PARTICLETEXTURESIZE*2, PARTICLETEXTURESIZE/4);
				m = 0;
				for (y = 0;y < PARTICLETEXTURESIZE;y++)
				{
					dy = (y - 0.5f*PARTICLETEXTURESIZE) / (PARTICLETEXTURESIZE*0.5f-1);
					for (x = 0;x < PARTICLETEXTURESIZE;x++)
					{
						dx = (x - 0.5f*PARTICLETEXTURESIZE) / (PARTICLETEXTURESIZE*0.5f-1);
						d = (noise2[y*PARTICLETEXTURESIZE*2+x] - 128) * 3 + 192;
						if (d > 0)
							d = (int)(d * (1-(dx*dx+dy*dy)));
						d = (d * noise1[y*PARTICLETEXTURESIZE*2+x]) >> 7;
						d = bound(0, d, 255);
						data[(y*PARTICLETEXTURESIZE+x)*4+3] = (unsigned char) d;
						if (m < d)
							m = d;
					}
				}
			}
			while (m < 224);
			setuptex(tex_smoke[i], data, particletexturedata);
		}

		// rain splash
		memset(data, 255, datasize);
		for (y = 0;y < PARTICLETEXTURESIZE;y++)
		{
			dy = (y - 0.5f*PARTICLETEXTURESIZE) / (PARTICLETEXTURESIZE*0.5f-1);
			for (x = 0;x < PARTICLETEXTURESIZE;x++)
			{
				dx = (x - 0.5f*PARTICLETEXTURESIZE) / (PARTICLETEXTURESIZE*0.5f-1);
				f = 255.0f * (1.0 - 4.0f * fabs(10.0f - sqrt(dx*dx+dy*dy)));
				data[(y*PARTICLETEXTURESIZE+x)*4+3] = (int) (bound(0.0f, f, 255.0f));
			}
		}
		setuptex(tex_rainsplash, data, particletexturedata);

		// normal particle
		memset(data, 255, datasize);
		for (y = 0;y < PARTICLETEXTURESIZE;y++)
		{
			dy = (y - 0.5f*PARTICLETEXTURESIZE) / (PARTICLETEXTURESIZE*0.5f-1);
			for (x = 0;x < PARTICLETEXTURESIZE;x++)
			{
				dx = (x - 0.5f*PARTICLETEXTURESIZE) / (PARTICLETEXTURESIZE*0.5f-1);
				d = (int)(256 * (1 - (dx*dx+dy*dy)));
				d = bound(0, d, 255);
				data[(y*PARTICLETEXTURESIZE+x)*4+3] = (unsigned char) d;
			}
		}
		setuptex(tex_particle, data, particletexturedata);

		// SEPTEMBER S6: the shotgun shell casing, cell 34, beside the blob it
		// replaces. Generated UNCONDITIONALLY: this runs at module start, not
		// per frame, so a cvar-gated generator would make the console toggle
		// dead until vid_restart; the spawn is what m5_shotgun_casing gates.
		M5_InitShellCasingTexture(data, particletexturedata);
		tex_shellcasing = tex_shellcasing_cell;
		Con_DPrintf("M5 shell casing: procedural cell %d\n", tex_shellcasing_cell);
		M5_InitDustAndRingTextures(data, particletexturedata);
		M5_InitFlashAndSparkTextures(data, particletexturedata);
		tex_dust = tex_dust_cell;
		tex_ring = tex_ring_cell;
		tex_flash = tex_flash_cell;
		tex_sparkhot = tex_sparkhot_cell;
		M5_InitEmberAndDropletTextures(data, particletexturedata);
		tex_ember = tex_ember_cell;
		tex_droplet = tex_droplet_cell;
		Con_DPrintf("M5 particle cells: dust %d, ring %d, flash %d, spark %d, ember %d, droplet %d\n", tex_dust_cell, tex_ring_cell, tex_flash_cell, tex_sparkhot_cell, tex_ember_cell, tex_droplet_cell);
		Con_DPrintf("M5 particle font: %dx%d (%d-pixel cells)\n", PARTICLEFONTSIZE, PARTICLEFONTSIZE, PARTICLETEXTURESIZE);

		// rain
		memset(data, 255, datasize);
		light[0] = 1;light[1] = 1;light[2] = 1;
		VectorNormalize(light);
		for (y = 0;y < PARTICLETEXTURESIZE;y++)
		{
			dy = (y - 0.5f*PARTICLETEXTURESIZE) / (PARTICLETEXTURESIZE*0.5f-1);
			// stretch upper half of bubble by +50% and shrink lower half by -50%
			// (this gives an elongated teardrop shape)
			if (dy > 0.5f)
				dy = (dy - 0.5f) * 2.0f;
			else
				dy = (dy - 0.5f) / 1.5f;
			for (x = 0;x < PARTICLETEXTURESIZE;x++)
			{
				dx = (x - 0.5f*PARTICLETEXTURESIZE) / (PARTICLETEXTURESIZE*0.5f-1);
				// shrink bubble width to half
				dx *= 2.0f;
				data[(y*PARTICLETEXTURESIZE+x)*4+3] = shadebubble(dx, dy, light);
			}
		}
		setuptex(tex_raindrop, data, particletexturedata);

		// bubble
		memset(data, 255, datasize);
		light[0] = 1;light[1] = 1;light[2] = 1;
		VectorNormalize(light);
		for (y = 0;y < PARTICLETEXTURESIZE;y++)
		{
			dy = (y - 0.5f*PARTICLETEXTURESIZE) / (PARTICLETEXTURESIZE*0.5f-1);
			for (x = 0;x < PARTICLETEXTURESIZE;x++)
			{
				dx = (x - 0.5f*PARTICLETEXTURESIZE) / (PARTICLETEXTURESIZE*0.5f-1);
				data[(y*PARTICLETEXTURESIZE+x)*4+3] = shadebubble(dx, dy, light);
			}
		}
		setuptex(tex_bubble, data, particletexturedata);

		// Blood particles and blood decals
		R_InitBloodTextures (particletexturedata);

		// bullet decals
		for (i = 0;i < 8;i++)
		{
			memset(data, 255, datasize);
			for (k = 0;k < 12;k++)
				particletextureblotch(data, PARTICLETEXTURESIZE/16, 0, 0, 0, 128);
			for (k = 0;k < 3;k++)
				particletextureblotch(data, PARTICLETEXTURESIZE/2, 0, 0, 0, 160);
			//particletextureclamp(data, 64, 64, 64, 255, 255, 255);
			particletextureinvert(data);
			setuptex(tex_bulletdecal[i], data, particletexturedata);
		}

#ifdef DUMPPARTICLEFONT
		Image_WriteTGABGRA ("particles/particlefont.tga", PARTICLEFONTSIZE, PARTICLEFONTSIZE, particletexturedata);
#endif
		// BEAUTY A2: an env-gated dump of the generated atlas, for the byte gate
		// (64 must be the 2001 font texel for texel) and for looking at the cells.
		// NOT particles/particlefont.tga -- that name is the external-font hook,
		// and a dump there would replace the atlas on the next boot.
		if (getenv("M5_DUMPPARTICLEFONT"))
			Image_WriteTGABGRA ("m5particlefont_dump.tga", PARTICLEFONTSIZE, PARTICLEFONTSIZE, particletexturedata);

		decalskinframe = R_SkinFrame_LoadInternalBGRA("particlefont", TEXF_ALPHA | TEXF_FORCELINEAR | TEXF_RGBMULTIPLYBYALPHA, particletexturedata, PARTICLEFONTSIZE, PARTICLEFONTSIZE, 0, 0, 0, false);
		particlefonttexture = decalskinframe->base;

		Mem_Free(particletexturedata);
		Mem_Free(data);
		Mem_Free(noise1);
		Mem_Free(noise2);
	}
	for (i = 0;i < MAX_PARTICLETEXTURES;i++)
	{
		CL_Particle_PixelCoordsForTexnum(i, &basex, &basey, &w, &h);
		particletexture[i].texture = particlefonttexture;
		particletexture[i].s1 = (basex + 1) / (float)particlefontwidth;
		particletexture[i].t1 = (basey + 1) / (float)particlefontheight;
		particletexture[i].s2 = (basex + w - 1) / (float)particlefontwidth;
		particletexture[i].t2 = (basey + h - 1) / (float)particlefontheight;
	}

#ifndef DUMPPARTICLEFONT
	particletexture[tex_beam].texture = loadtextureimage(particletexturepool, "particles/nexbeam.tga", false, TEXF_ALPHA | TEXF_FORCELINEAR | TEXF_RGBMULTIPLYBYALPHA, true, vid.sRGB3D);
	if (!particletexture[tex_beam].texture)
#endif
	{
		unsigned char noise3[64][64], data2[64][16][4];
		// nexbeam
		fractalnoise(&noise3[0][0], 64, 4);
		m = 0;
		for (y = 0;y < 64;y++)
		{
			dy = (y - 0.5f*64) / (64*0.5f-1);
			for (x = 0;x < 16;x++)
			{
				dx = (x - 0.5f*16) / (16*0.5f-2);
				d = (int)((1 - sqrt(fabs(dx))) * noise3[y][x]);
				data2[y][x][0] = data2[y][x][1] = data2[y][x][2] = (unsigned char) bound(0, d, 255);
				data2[y][x][3] = 255;
			}
		}

#ifdef DUMPPARTICLEFONT
		Image_WriteTGABGRA ("particles/nexbeam.tga", 64, 64, &data2[0][0][0]);
#endif
		particletexture[tex_beam].texture = R_LoadTexture2D(particletexturepool, "nexbeam", 16, 64, &data2[0][0][0], TEXTYPE_BGRA, TEXF_ALPHA | TEXF_FORCELINEAR | TEXF_RGBMULTIPLYBYALPHA, -1, NULL);
	}
	particletexture[tex_beam].s1 = 0;
	particletexture[tex_beam].t1 = 0;
	particletexture[tex_beam].s2 = 1;
	particletexture[tex_beam].t2 = 1;

	// now load an texcoord/texture override file
	buf = (char *) FS_LoadFile("particles/particlefont.txt", tempmempool, false, &filesize);
	if(buf)
	{
		const char *bufptr;
		bufptr = buf;
		for(;;)
		{
			if(!COM_ParseToken_Simple(&bufptr, true, false, true))
				break;
			if(!strcmp(com_token, "\n"))
				continue; // empty line
			i = atoi(com_token);

			texturename[0] = 0;
			s1 = 0;
			t1 = 0;
			s2 = 1;
			t2 = 1;

			if (COM_ParseToken_Simple(&bufptr, true, false, true) && strcmp(com_token, "\n"))
			{
				dp_strlcpy(texturename, com_token, sizeof(texturename));
				s1 = atof(com_token);
				if (COM_ParseToken_Simple(&bufptr, true, false, true) && strcmp(com_token, "\n"))
				{
					texturename[0] = 0;
					t1 = atof(com_token);
					if (COM_ParseToken_Simple(&bufptr, true, false, true) && strcmp(com_token, "\n"))
					{
						s2 = atof(com_token);
						if (COM_ParseToken_Simple(&bufptr, true, false, true) && strcmp(com_token, "\n"))
						{
							t2 = atof(com_token);
							dp_strlcpy(texturename, "particles/particlefont.tga", sizeof(texturename));
							if (COM_ParseToken_Simple(&bufptr, true, false, true) && strcmp(com_token, "\n"))
								dp_strlcpy(texturename, com_token, sizeof(texturename));
						}
					}
				}
				else
					s1 = 0;
			}
			if (!texturename[0])
			{
				Con_Printf("particles/particlefont.txt: syntax should be texnum x1 y1 x2 y2 texturename or texnum x1 y1 x2 y2 or texnum texturename\n");
				continue;
			}
			if (i < 0 || i >= MAX_PARTICLETEXTURES)
			{
				Con_Printf("particles/particlefont.txt: texnum %i outside valid range (0 to %i)\n", i, MAX_PARTICLETEXTURES);
				continue;
			}
			sf = R_SkinFrame_LoadExternal(texturename, TEXF_ALPHA | TEXF_FORCELINEAR | TEXF_RGBMULTIPLYBYALPHA, true, true); // note: this loads as sRGB if sRGB is active!
			particletexture[i].texture = sf->base;
			particletexture[i].s1 = s1;
			particletexture[i].t1 = t1;
			particletexture[i].s2 = s2;
			particletexture[i].t2 = t2;
		}
		Mem_Free(buf);
	}
}

static void r_part_start(void)
{
	int i;
	// generate particlepalette for convenience from the main one
	for (i = 0;i < 256;i++)
		particlepalette[i] = palette_rgb[i][0] * 65536 + palette_rgb[i][1] * 256 + palette_rgb[i][2];
	particletexturepool = R_AllocTexturePool();
	R_InitParticleTexture ();
	CL_Particles_LoadEffectInfo(NULL);
}

static void r_part_shutdown(void)
{
	R_FreeTexturePool(&particletexturepool);
}

/*
===============
CL_ParticleFont_Update  (BEAUTY A2, 2026-09-16)

Re-generates the engine's own atlas when cl_particles_texsize has moved, so the
change reaches the screen without a vid_restart. Called from the relink window
beside CL_Dust_Update -- between frames, never from R_DrawParticles (which
re-runs per water sub-scene). It is exactly what vid_restart does for this
module (the shutdown/start pair); particles reference cells by INDEX and look
the texture up at draw, so nothing in flight holds the old font. Inert when an
external particlefont.tga is in use (particletexsize_applied is 0 there).
===============
*/
void CL_ParticleFont_Update(void)
{
	int want;
	if (!particletexturepool || !particletexsize_applied)
		return;
	want = (cl_particles_texsize.integer >= 256) ? 256 : 64;
	if (want == particletexsize_applied)
		return;
	r_part_shutdown();
	r_part_start();
}

static void r_part_newmap(void)
{
	if (decalskinframe)
		R_SkinFrame_MarkUsed(decalskinframe);
	CL_Particles_LoadEffectInfo(NULL);
}

unsigned short particle_elements[MESHQUEUE_TRANSPARENT_BATCHSIZE*6];
float particle_vertex3f[MESHQUEUE_TRANSPARENT_BATCHSIZE*12], particle_texcoord2f[MESHQUEUE_TRANSPARENT_BATCHSIZE*8], particle_color4f[MESHQUEUE_TRANSPARENT_BATCHSIZE*16];

void R_Particles_Init (void)
{
	int i;
	for (i = 0;i < MESHQUEUE_TRANSPARENT_BATCHSIZE;i++)
	{
		particle_elements[i*6+0] = i*4+0;
		particle_elements[i*6+1] = i*4+1;
		particle_elements[i*6+2] = i*4+2;
		particle_elements[i*6+3] = i*4+0;
		particle_elements[i*6+4] = i*4+2;
		particle_elements[i*6+5] = i*4+3;
	}

	Cvar_RegisterVariable(&r_drawparticles);
	Cvar_RegisterVariable(&r_drawparticles_drawdistance);
	Cvar_RegisterVariable(&r_drawparticles_nearclip_min);
	Cvar_RegisterVariable(&r_drawparticles_nearclip_max);
	Cvar_RegisterVariable(&r_drawdecals);
	Cvar_RegisterVariable(&r_drawdecals_drawdistance);
	R_RegisterModule("R_Particles", r_part_start, r_part_shutdown, r_part_newmap, NULL, NULL);
}

/*
================
CL_Dust_Update

SEPTEMBER S7 item 2: the ambient dust emitter. Keeps m5_dust motes alive in the
open air around the eye. Runs from CSQC_RelinkAllEntities beside M5_Torch_Relink
-- NOT from R_DrawParticles, which re-runs for every water sub-scene and would
spawn per sub-scene -- and reads the eye from the view MATRIX, the one camera
source that is valid at that point in the frame (r_refdef.view.origin is last
frame's; the documented trap).

Spawn discipline, per frame: at most 32 motes from at most 96 candidates, each
rejected if it is inside solid, sky or liquid, within 32 units of the face, or
behind a wall (one CL_TraceLine from the eye -- bounded by the per-frame cap,
so a busy map costs what an empty one does). The live count comes from
R_DrawParticles' walk of the previous frame (cl_dust_live); the pool has no
priority and CL_NewParticle fails SILENTLY when it is full, so the emitter
yields first: it spawns nothing while the pool is within 512 of its ceiling.
Every count here is absolute -- cl_particles_quality is never consulted.
================
*/
static int cl_dust_live;    // motes alive last frame (counted in R_DrawParticles)
static int cl_dust_count;   // this frame's running count
void CL_Dust_Update(void)
{
	vec3_t fwd, left, up, eye;
	int want, deficit, spawned = 0, tries;
	float radius, alpha;

	if (m5_dust.integer <= 0 || m5_stock.integer || !cl_particles.integer || cls.state != ca_connected || cls.signon != SIGNONS || !cl.worldmodel)
		return;
	if (cl.num_particles > cl.max_particles - 512)
		return;   // pool-share yield: effects come first
	want = min(m5_dust.integer, 4096);
	// a count can never exceed the pool: the belt to the braces above, for any
	// frame where the scene was not walked at all (menu up, loading)
	if (cl_dust_live > cl.num_particles)
		cl_dust_live = cl.num_particles;
	deficit = want - cl_dust_live;
	if (deficit <= 0)
		return;
	Matrix4x4_ToVectors(&r_refdef.view.matrix, fwd, left, up, eye);
	radius = max(64.0f, m5_dust_radius.value);
	alpha = bound(0.02f, m5_dust_alpha.value, 1.0f) * 255.0f;
	for (tries = 0; tries < 96 && spawned < 32 && spawned < deficit; tries++)
	{
		vec3_t v, pos;
		float life;
		VectorRandom(v);   // a random point in the unit sphere
		VectorMA(eye, radius, v, pos);
		if (VectorDistance2(pos, eye) < 32.0f * 32.0f)
			continue;
		if (CL_PointSuperContents(pos) & (SUPERCONTENTS_SOLID | SUPERCONTENTS_SKY | SUPERCONTENTS_LIQUIDSMASK))
			continue;
		if (CL_TraceLine(eye, pos, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, MATERIALFLAGMASK_TRANSLUCENT, collision_extendmovelength.value, true, false, NULL, false, true).fraction < 1)
			continue;
		life = lhrandom(8, 16);
		// each mote a RANDOM shade between soot-dark and white (CL_NewParticle
		// lerps the two colours per particle): Seb liked the dark motes and asked
		// for sharper white ones, so the population carries both and the room's
		// light does the rest. alphafade = alpha/life so the mote fades to nothing
		// exactly as it dies; air friction 0.5 damps the birth drift and the swirl
		// into a slow wander
		// m5_dust_tint 1 (THE DEFAULT since 2026-09-19): a weighted BONFIRE-ASH
		// palette instead of the grey ramp. Seb, 2026-09-19: "tone down the white
		// dust so it's more like flecks of blue/grey/brown bonfire ash. currently
		// it's bright white -- which it can be when lit with muzzle flashes --
		// but it's just too white and noticeable most of the time."
		//
		// THE RAMP IS WHY IT WAS WHITE, and it is arithmetic rather than taste:
		// CL_NewParticle lerps the two endpoints per particle, so soot->white is
		// a UNIFORM population over that range -- mean mote luma 143 of 255, with
		// 57% of motes above mid-grey. This palette reads mean 82 with 12% above
		// it, and only ONE entry in sixteen is the near-white the eye picks out.
		// The 2026-09-06 palette (soot, two browns, khaki green, slate blue,
		// off-white) is its ancestor and measured 86; his word this time was grey
		// rather than green, so the green became a warm and a cool grey and the
		// dark end gained weight. Passing the same colour for both endpoints makes
		// the per-particle lerp exact, so the palette arrives as authored; the
		// room's light then does the shading -- and cl_particles_lighting_static
		// is what keeps that room light from putting it back where it started.
		unsigned int dc1 = 0x202020, dc2 = 0xFFFFFF;
		if (m5_dust_tint.integer)
		{
			static const unsigned int palette[16] = {
				0x1c1a18, 0x1c1a18, 0x1c1a18,   // soot
				0x2e2b28, 0x2e2b28,             // charcoal
				0x45392e, 0x45392e,             // charred brown
				0x6b6158, 0x6b6158,             // warm grey
				0x5c6068, 0x5c6068,             // cool grey
				0x4a5a72, 0x4a5a72,             // slate blue
				0x7a5a3a,                       // brown
				0x9aa0a8,                       // pale ash
				0xd8d0c0};                      // off-white, one in sixteen
			dc1 = dc2 = palette[rand() & 15];
		}
		if (!CL_NewParticle(pos, pt_dust, dc1, dc2, tex_dust, m5_dust_size.value, 0, alpha, alpha / life, 0, 0, pos[0], pos[1], pos[2], 0, 0, 0, 0.5f, 0, 0, m5_dust_speed.value, false, life, 1, PBLEND_ALPHA, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL))
			break;   // the pool is full: stop, do not spin
		spawned++;
	}
	// first-event liveness line (tests/smoke.sh greps it); never per frame
	{
		static qbool dust_announced;
		if (!dust_announced && spawned > 0)
		{
			dust_announced = true;
			Con_DPrintf("M5 dust: emitter live (%d motes wanted within %.0f units)\n", want, radius);
		}
	}
}

/*
================
CL_TorchEmbers_Update  (BEAUTY B4, 2026-09-17)

Embers off the torches. Runs from the relink window beside CL_Dust_Update --
never from R_DrawParticles, which re-runs per water sub-scene. Walks the STATIC
entities (torches, braziers and candles are static in every id1 and AD map),
keeps a per-torch accumulator in seconds, and for each light-core model within
reach and in front of the eye spawns m5_torch_embers embers a second: a hot
grain (cell 39) born at the flame's tip, thrown up and out with the flame's own
scatter, rising against a weak negative gravity, damped by air friction, its
colour picked between a bright orange and a dull red so the population cools,
its alpha fading to nothing as it dies. Lit like the additive things it is
among (PBLEND_ADD, so it is the ember's own glow). Capped at 16 spawns a frame
and yielding to the pool exactly as the dust does. A traceline from the eye
keeps torches behind walls from spawning into rooms you cannot see.
================
*/
// The light-core model NAME test, here rather than beside RT_IsLightCoreModel
// in cl_screen.c because that sits inside USE_RT_METAL and this file links
// into the dedicated server too -- the sv-release trap this tree has paid for
// twice, and paid a third time on the first build of this emitter.
qbool CL_IsLightCoreModelName(const char *name)
{
	static const char *corenames[] = {
		"progs/flame.mdl", "progs/flame2.mdl",
		"progs/misc_flame_big.mdl", "progs/misc_flame_med.mdl",
		"progs/misc_candle1.mdl", "progs/misc_candle2.mdl", "progs/misc_candle3.mdl",
		"progs/misc_lantern.mdl",
	};
	size_t i;
	for (i = 0; i < sizeof(corenames) / sizeof(corenames[0]); i++)
		if (!strcmp(name, corenames[i]))
			return true;
	return false;
}
#define M5_EMBER_MAXTORCH 4096
static float cl_torchember_acc[M5_EMBER_MAXTORCH];
void CL_TorchEmbers_Update(void)
{
	static double lasttime;
	double now, dt;
	vec3_t fwd, left, up, eye;
	int i, spawned = 0;
	float rate;
	if (m5_torch_embers.value <= 0.0f || m5_stock.integer || !cl_particles.integer || cls.state != ca_connected || cls.signon != SIGNONS || !cl.worldmodel)
	{
		lasttime = 0;
		return;
	}
	now = cl.time;
	dt = (lasttime > 0 && now > lasttime) ? (now - lasttime) : 0;
	lasttime = now;
	if (dt <= 0 || dt > 0.25)
		return;
	if (cl.num_particles > cl.max_particles - 512)
		return;   // pool-share yield: effects come first
	rate = min(m5_torch_embers.value, 20.0f);
	Matrix4x4_ToVectors(&r_refdef.view.matrix, fwd, left, up, eye);
	for (i = 0; i < cl.num_static_entities && i < M5_EMBER_MAXTORCH && spawned < 16; i++)
	{
		entity_t *e = cl.static_entities + i;
		vec3_t org, rel, pos, vel;
		float d2, k;
		if (!e->render.model || !CL_IsLightCoreModelName(e->render.model->name))
			continue;
		Matrix4x4_OriginFromMatrix(&e->render.matrix, org);
		VectorSubtract(org, eye, rel);
		d2 = DotProduct(rel, rel);
		if (d2 > 1024.0f * 1024.0f || DotProduct(rel, fwd) < -64.0f)
		{
			cl_torchember_acc[i] = 0;
			continue;
		}
		cl_torchember_acc[i] += rate * (float)dt;
		if (cl_torchember_acc[i] < 1.0f)
			continue;
		// one traceline per torch per ember, not per frame
		if (CL_TraceLine(eye, org, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, MATERIALFLAGMASK_TRANSLUCENT, collision_extendmovelength.value, true, false, NULL, false, false).fraction < 1)
		{
			cl_torchember_acc[i] = 0;
			continue;
		}
		while (cl_torchember_acc[i] >= 1.0f && spawned < 16)
		{
			cl_torchember_acc[i] -= 1.0f;
			// born in the flame's upper half, thrown up and a little out
			pos[0] = org[0] + lhrandom(-4, 4);
			pos[1] = org[1] + lhrandom(-4, 4);
			pos[2] = org[2] + lhrandom(12, 22);
			vel[0] = lhrandom(-14, 14);
			vel[1] = lhrandom(-14, 14);
			vel[2] = lhrandom(22, 46);
			k = lhrandom(1.0f, 2.2f);   // life in seconds
			if (!CL_NewParticle(pos, pt_static, 0xFF9030, 0x80200A, tex_ember, lhrandom(0.5f, 0.9f), 0, 200, 200 / k, -0.06f, 0, pos[0], pos[1], pos[2], vel[0], vel[1], vel[2], 1.2f, 0, 0, 6, false, k, 1, PBLEND_ADD, PARTICLE_BILLBOARD, -1, -1, -1, 1, 1, 0, 0, NULL))
				break;   // the pool is full: stop, do not spin
			spawned++;
		}
	}
	{
		static qbool announced;
		if (!announced && spawned > 0)
		{
			announced = true;
			Con_DPrintf("M5 torch embers: live (%.1f a second per torch)\n", rate);
		}
	}
}

/*
================
CL_ParticleLight_Prepare / CL_ParticleLight

SEPTEMBER S7: light for alpha-blended particles (cl_particles_lighting).

Mode 1 is the engine's own R_CompleteLightPoint, per particle, with ONE change
from the flag set the dormant branch always carried: LP_RTWORLD is dropped.
That arm walks the WHOLE world-light array per particle per frame (66 on e1m3,
2759 on ad_sepulcher) for a term that is provably zero in this fork's
configuration -- r_shadow_realtime_world 0 filters on NORMALMODE and every
world light is registered REALTIMEMODE -- so it was cost with no light in it.
The lightmap and the traced dynamic lights are exactly the engine's.

Mode 2 keeps the engine's dynamic-light arithmetic verbatim (falloff, the
first-order spherical-harmonic combine, the 0.25 diffuse share) but drops the
shadow traceline, and takes the STATIC term from the volumetric irradiance grid
instead of a LightPoint: one trilinear fetch of a 128-unit-cell RGB grid baked
at map load from R_CompleteLightPoint(LP_LIGHTMAP) itself, already storing the
particle convention (amb + 0.25 * dif) -- see R_Volumetric_GetIrradianceGrid.
So the two modes agree in magnitude and differ only in the grid's coarseness
and the missing occlusion test; mode 2's cost is bounded by the dynamic-light
count, which the scene caps at MAX_DLIGHTS, whatever the map.

The dynamic list is compacted ONCE per R_DrawParticles (origin, radius and the
style-scaled currentcolor, which R_Shadow_PrepareLights has filled by then --
the same field the engine's own LP_DYNLIGHT arm reads), so the per-particle
loop touches a few dozen floats rather than an rtlight_t each.
================
*/
extern cvar_t r_volumetric_particles_alpha;   // gl_rmain.c
extern cvar_t r_shadow_lightattenuationlinearscale;
extern cvar_t r_shadow_lightattenuationdividebias;
static struct { vec3_t org; float radius; vec3_t color; } pl_dyn[MAX_DLIGHTS];
static int pl_numdyn;

static void CL_ParticleLight_Prepare(void)
{
	int i;
	pl_numdyn = 0;
	if (CL_ParticleLightingMode() < 2)
		return;
	// bake (once per map) or fetch the grid HERE, between passes and outside
	// any draw batch -- the murk resolves its textures at the same kind of site.
	// Never inside the transparent callback: a 3D texture upload mid-batch would
	// disturb the bound units on GL.
	R_Volumetric_ResolveIrradiance();
	for (i = 0; i < r_refdef.scene.numlights && pl_numdyn < MAX_DLIGHTS; i++)
	{
		const rtlight_t *light = r_refdef.scene.lights[i];
		if (!light || light->radius <= 0.0f)
			continue;
		if (VectorLength2(light->currentcolor) < (1.0f / 1048576.0f))
			continue;   // style currently off
		VectorCopy(light->shadoworigin, pl_dyn[pl_numdyn].org);
		pl_dyn[pl_numdyn].radius = light->radius;
		VectorCopy(light->currentcolor, pl_dyn[pl_numdyn].color);
		pl_numdyn++;
	}
}

static void CL_ParticleLight(const vec3_t org, vec3_t out)
{
	int mode = CL_ParticleLightingMode();
	float statik = max(0.0f, cl_particles_lighting_static.value);
	int q;
	if (mode >= 2)
	{
		float sa[3], sx[3], sy[3], sz[3], sd[3], grid[3], lightdir[3], diffuse[3];
		int i;
		if (!R_Volumetric_SampleIrradiance(org, grid))
			VectorSet(grid, 1.0f, 1.0f, 1.0f);   // no grid (no world / allocation failed): fullbright, as LightPoint's unlit-map arm does
		for (q = 0; q < 3; q++)
			sa[q] = sx[q] = sy[q] = sz[q] = sd[q] = 0;
		for (i = 0; i < pl_numdyn; i++)
		{
			// LOCKSTEP with R_CompleteLightPoint's LP_DYNLIGHT arm (r_shadow.c),
			// minus its CL_TraceLine: same falloff, same accumulation.
			float relativepoint[3], color[3], dist, dist2, intensity, radius = pl_dyn[i].radius;
			VectorSubtract(pl_dyn[i].org, org, relativepoint);
			dist2 = VectorLength2(relativepoint);
			if (dist2 >= radius * radius)
				continue;
			dist = sqrt(dist2) / radius;
			intensity = (1.0f - dist) * r_shadow_lightattenuationlinearscale.value / (r_shadow_lightattenuationdividebias.value + dist*dist) * r_shadow_lightintensityscale.value;
			if (intensity <= 0.0f)
				continue;
			for (q = 0; q < 3; q++)
				color[q] = pl_dyn[i].color[q] * intensity;
			intensity = VectorLength(color);
			VectorNormalize(relativepoint);
			for (q = 0; q < 3; q++)
			{
				sa[q] += 0.5f * color[q];
				sx[q] += relativepoint[0] * color[q];
				sy[q] += relativepoint[1] * color[q];
				sz[q] += relativepoint[2] * color[q];
				sd[q] += intensity * relativepoint[q];
			}
		}
		for (q = 0; q < 3; q++)
			lightdir[q] = sd[q];
		VectorNormalize(lightdir);
		for (q = 0; q < 3; q++)
		{
			float ambient;
			diffuse[q] = (lightdir[0] * sx[q] + lightdir[1] * sy[q] + lightdir[2] * sz[q]);
			ambient = sa[q] + -0.333f * diffuse[q];
			// grid: (amb + 0.25 dif) at lightmapintensity 1 -- scale by the
			// scene's, as the LightPoint arm does; then the engine's own
			// ambient/diffuse split for the dynamic lights and its ambient fill.
			// cl_particles_lighting_static scales the GRID alone, because the
			// grid IS this mode's static term and `ambient`/`diffuse` are this
			// frame's dlights -- so a muzzle flash is untouched by it, which is
			// the whole reason the knob exists. At 1 this is a multiply by 1.0f,
			// exact in IEEE, so the off switch is byte-exact by construction.
			out[q] = grid[q] * r_refdef.scene.lightmapintensity * statik + ambient + 0.25f * diffuse[q] + r_refdef.scene.ambientintensity;
		}
	}
	else
	{
		float a[3], c[3], dir[3];
		R_CompleteLightPoint(a, c, dir, org, LP_LIGHTMAP | LP_DYNLIGHT, r_refdef.scene.lightmapintensity, r_refdef.scene.ambientintensity);
		for (q = 0; q < 3; q++)
			out[q] = a[q] + 0.25f * c[q];
		// mode 1 sums the lightmap and the dynamic lights INSIDE
		// R_CompleteLightPoint, so the split needs a second probe -- but only
		// when it would change something, and LP_LIGHTMAP alone fires no
		// traceline, so the expensive half of this mode is not paid twice.
		// ambientintensity is passed as 0 here on purpose: the flat fill is a
		// floor the player set and is not the room's own light.
		if (statik != 1.0f)
		{
			float sa2[3], sc2[3], sd2[3];
			R_CompleteLightPoint(sa2, sc2, sd2, org, LP_LIGHTMAP, r_refdef.scene.lightmapintensity, 0);
			for (q = 0; q < 3; q++)
			{
				float stat = sa2[q] + 0.25f * sc2[q];
				out[q] += stat * (statik - 1.0f);
			}
		}
	}
	VectorScale(out, cl_particles_lighting_gain.value, out);
	// change-only liveness line (the console-ink rule): the branch is the
	// feature's only observable, and tests/smoke.sh greps it
	{
		static int pl_announced = -1;
		static float pl_static = -1.0f;
		if (pl_announced != mode || pl_static != statik)
		{
			pl_announced = mode;
			pl_static = statik;
			Con_DPrintf("M5 particle lighting: mode %d live (%s%s)\n", mode,
				mode >= 2 ? "irradiance grid + dynamic lights, no tracelines" : "engine LightPoint, lightmap + traced dynamic lights",
				statik != 1.0f ? ", static light scaled" : "");
		}
	}
}

static void R_DrawParticle_TransparentCallback(const entity_render_t *ent, const rtlight_t *rtlight, int numsurfaces, int *surfacelist)
{
	vec3_t vecorg, vecvel, baseright, baseup;
	int surfacelistindex;
	int batchstart, batchcount;
	int viewmodelbatch;
	int refractcell = -1;	// BEAUTY A5: the cell of a refract batch, -1 otherwise
	const particle_t *p;
	pblend_t blendmode;
	rtexture_t *texture;
	float *v3f, *t2f, *c4f;
	particletexture_t *tex;
	float up2[3], v[3], right[3], up[3], fog, ifog, size, len, lenfactor, alpha;
	float murk, imurk;
	vec3_t murktint;
//	float ambient[3], diffuse[3], diffusenormal[3];
	float palpha, spintime, spinrad, spincos, spinsin, spinm1, spinm2, spinm3, spinm4;
	vec4_t colormultiplier;
	float minparticledist_start, minparticledist_end;
	qbool dofade;

	RSurf_ActiveModelEntity(r_refdef.scene.worldentity, false, false, false);

	Vector4Set(colormultiplier, r_refdef.view.colorscale * (1.0 / 256.0f), r_refdef.view.colorscale * (1.0 / 256.0f), r_refdef.view.colorscale * (1.0 / 256.0f), cl_particles_alpha.value * (1.0 / 256.0f));

	r_refdef.stats[r_stat_particles] += numsurfaces;
//	R_Mesh_ResetTextureState();
	GL_DepthMask(false);
	GL_DepthRange(0, 1);
	GL_PolygonOffset(0, 0);
	GL_DepthTest(true);
	GL_CullFace(GL_NONE);

	spintime = r_refdef.scene.time;

	minparticledist_start = DotProduct(r_refdef.view.origin, r_refdef.view.forward) + r_drawparticles_nearclip_min.value;
	minparticledist_end = DotProduct(r_refdef.view.origin, r_refdef.view.forward) + r_drawparticles_nearclip_max.value;
	dofade = (minparticledist_start < minparticledist_end);

	// first generate all the vertices at once
	for (surfacelistindex = 0, v3f = particle_vertex3f, t2f = particle_texcoord2f, c4f = particle_color4f;surfacelistindex < numsurfaces;surfacelistindex++, v3f += 3*4, t2f += 2*4, c4f += 4*4)
	{
		p = cl.particles + surfacelist[surfacelistindex];

		blendmode = (pblend_t)p->blendmode;
		palpha = p->alpha;
		if(dofade && p->orientation != PARTICLE_VBEAM && p->orientation != PARTICLE_HBEAM)
			palpha *= min(1, (DotProduct(p->org, r_refdef.view.forward)  - minparticledist_start) / (minparticledist_end - minparticledist_start));
		alpha = palpha * colormultiplier[3];
		// ensure alpha multiplier saturates properly -- except for the local
		// muzzle flash (BEAUTY A3), whose whole point is to go ABOVE white on the
		// float scene buffer so the bloom gives it a core; an 8-bit buffer
		// clamps it in the blend and loses nothing
		if (alpha > 1.0f && !p->viewmodel)
			alpha = 1.0f;

		// The volumetric murk is a screen-space pass and was composited before
		// this transparent batch ever ran, so it cannot attenuate particles the
		// way it attenuates the world behind them -- blood and smoke otherwise
		// read bright and clear through fog thick enough to hide the wall. Fade
		// them by hand, at exactly the point the classic fog does, and with the
		// same two shapes: additive blends lose alpha, alpha blends lerp towards
		// the murk's own colour.
		murk = R_Volumetric_TransmittanceToPoint(p->org, murktint);
		// BEAUTY A3: the local muzzle flash is at the gun, and the gun is not
		// fogged; at Seb's density the muzzle fifty units out sits at a fraction
		// of the murk's transmittance, which is what left the burst dim on his
		// demo45 after every other cause had been ruled out
		if (p->viewmodel)
			murk = 1.0f;

		switch (blendmode)
		{
		case PBLEND_INVALID:
		case PBLEND_INVMOD:
			// additive and modulate can just fade out in fog (this is correct)
			if (r_refdef.fogenabled)
				alpha *= RSurf_FogVertex(p->org);
			alpha *= murk;
			// collapse alpha into color for these blends (so that the particlefont does not need alpha on most textures)
			alpha *= 1.0f / 256.0f;
			c4f[0] = p->color[0] * alpha;
			c4f[1] = p->color[1] * alpha;
			c4f[2] = p->color[2] * alpha;
			c4f[3] = 0;
			break;
		case PBLEND_ADD:
			// additive and modulate can just fade out in fog (this is correct)
			if (r_refdef.fogenabled && !p->viewmodel)
				alpha *= RSurf_FogVertex(p->org);
			alpha *= murk;
			// collapse alpha into color for these blends (so that the particlefont does not need alpha on most textures)
			c4f[0] = p->color[0] * colormultiplier[0] * alpha;
			c4f[1] = p->color[1] * colormultiplier[1] * alpha;
			c4f[2] = p->color[2] * colormultiplier[2] * alpha;
			c4f[3] = 0;
			break;
		case PBLEND_REFRACT:
			// BEAUTY A5: the vertex carries COVERAGE only (white, alpha); the
			// fragment replaces itself with the displaced frame at that coverage.
			// Faded by the murk like an alpha particle, never by classic fog (the
			// frame it samples is already fogged). A twin still in flight when the
			// cvar goes to 0 (or when the copy is not this frame's) must draw
			// NOTHING -- with the fetch disarmed the fragment is the bare white
			// cell, a white ring over the scene (seen on the still bed).
			if (cl_particles_refract.value <= 0.0f || !r_fb.waterscreen_valid)
			{
				c4f[0] = c4f[1] = c4f[2] = c4f[3] = 0.0f;
				break;
			}
			c4f[0] = c4f[1] = c4f[2] = 1.0f;
			c4f[3] = alpha * murk;
			break;
		case PBLEND_ALPHA:
			c4f[0] = p->color[0] * colormultiplier[0];
			c4f[1] = p->color[1] * colormultiplier[1];
			c4f[2] = p->color[2] * colormultiplier[2];
			c4f[3] = alpha;
			// note: lighting is not cheap! (SEPTEMBER S7: which is why it is a
			// cvar with a bounded mode -- see CL_ParticleLight above)
			if (particletype[p->typeindex].lighting && CL_ParticleLightingMode() > 0)
			{
				float lit[3];
				vecorg[0] = p->org[0];
				vecorg[1] = p->org[1];
				vecorg[2] = p->org[2];
				CL_ParticleLight(vecorg, lit);
				c4f[0] = p->color[0] * colormultiplier[0] * lit[0];
				c4f[1] = p->color[1] * colormultiplier[1] * lit[1];
				c4f[2] = p->color[2] * colormultiplier[2] * lit[2];
			}
			// mix in the fog color
			if (r_refdef.fogenabled)
			{
				fog = RSurf_FogVertex(p->org);
				ifog = 1 - fog;
				c4f[0] = c4f[0] * fog + r_refdef.fogcolor[0] * ifog;
				c4f[1] = c4f[1] * fog + r_refdef.fogcolor[1] * ifog;
				c4f[2] = c4f[2] * fog + r_refdef.fogcolor[2] * ifog;
			}
			// and the same again for the murk -- but by ALPHA, not by colour
			// (r_volumetric_particles_alpha, 2026-09-02). The lerp below
			// reconstructs the murk's authored base colour, which is the UNLIT
			// fog; what is on screen behind the particle is that base darkened by
			// the irradiance grid and lit by the kernel, so a black puff at the
			// far wall lerped toward a near-black teal stayed a black puff at full
			// opacity and "cut through the fog" -- the liquid-fade defect of
			// 2026-08-31 in its particle form. Scaling alpha lets the fog's own
			// composited pixel show through and cannot disagree with it.
			if (murk < 1.0f)
			{
				if (r_volumetric_particles_alpha.integer)
				{
					alpha *= murk;
					c4f[3] = alpha;
				}
				else
				{
					imurk = 1 - murk;
					c4f[0] = c4f[0] * murk + murktint[0] * imurk;
					c4f[1] = c4f[1] * murk + murktint[1] * imurk;
					c4f[2] = c4f[2] * murk + murktint[2] * imurk;
				}
			}
			// for premultiplied alpha we have to apply the alpha to the color (after fog of course)
			VectorScale(c4f, alpha, c4f);
			break;
		}
		// copy the color into the other three vertices
		Vector4Copy(c4f, c4f + 4);
		Vector4Copy(c4f, c4f + 8);
		Vector4Copy(c4f, c4f + 12);

		size = p->size * cl_particles_size.value;
		tex = &particletexture[p->texnum];
		switch(p->orientation)
		{
//		case PARTICLE_INVALID:
		case PARTICLE_BILLBOARD:
			if (p->angle + p->spin)
			{
				spinrad = (p->angle + p->spin * (spintime - p->delayedspawn)) * (float)(M_PI / 180.0f);
				spinsin = sin(spinrad) * size;
				spincos = cos(spinrad) * size;
				spinm1 = -p->stretch * spincos;
				spinm2 = -spinsin;
				spinm3 = spinsin;
				spinm4 = -p->stretch * spincos;
				VectorMAM(spinm1, r_refdef.view.left, spinm2, r_refdef.view.up, right);
				VectorMAM(spinm3, r_refdef.view.left, spinm4, r_refdef.view.up, up);
			}
			else
			{
				VectorScale(r_refdef.view.left, -size * p->stretch, right);
				VectorScale(r_refdef.view.up, size, up);
			}

			v3f[ 0] = p->org[0] - right[0] - up[0];
			v3f[ 1] = p->org[1] - right[1] - up[1];
			v3f[ 2] = p->org[2] - right[2] - up[2];
			v3f[ 3] = p->org[0] - right[0] + up[0];
			v3f[ 4] = p->org[1] - right[1] + up[1];
			v3f[ 5] = p->org[2] - right[2] + up[2];
			v3f[ 6] = p->org[0] + right[0] + up[0];
			v3f[ 7] = p->org[1] + right[1] + up[1];
			v3f[ 8] = p->org[2] + right[2] + up[2];
			v3f[ 9] = p->org[0] + right[0] - up[0];
			v3f[10] = p->org[1] + right[1] - up[1];
			v3f[11] = p->org[2] + right[2] - up[2];
			t2f[0] = tex->s1;t2f[1] = tex->t2;
			t2f[2] = tex->s1;t2f[3] = tex->t1;
			t2f[4] = tex->s2;t2f[5] = tex->t1;
			t2f[6] = tex->s2;t2f[7] = tex->t2;
			break;
		case PARTICLE_ORIENTED_DOUBLESIDED:
			vecvel[0] = p->vel[0];
			vecvel[1] = p->vel[1];
			vecvel[2] = p->vel[2];
			VectorVectors(vecvel, baseright, baseup);
			if (p->angle + p->spin)
			{
				spinrad = (p->angle + p->spin * (spintime - p->delayedspawn)) * (float)(M_PI / 180.0f);
				spinsin = sin(spinrad) * size;
				spincos = cos(spinrad) * size;
				spinm1 = p->stretch * spincos;
				spinm2 = -spinsin;
				spinm3 = spinsin;
				spinm4 = p->stretch * spincos;
				VectorMAM(spinm1, baseright, spinm2, baseup, right);
				VectorMAM(spinm3, baseright, spinm4, baseup, up);
			}
			else
			{
				VectorScale(baseright, size * p->stretch, right);
				VectorScale(baseup, size, up);
			}
			v3f[ 0] = p->org[0] - right[0] - up[0];
			v3f[ 1] = p->org[1] - right[1] - up[1];
			v3f[ 2] = p->org[2] - right[2] - up[2];
			v3f[ 3] = p->org[0] - right[0] + up[0];
			v3f[ 4] = p->org[1] - right[1] + up[1];
			v3f[ 5] = p->org[2] - right[2] + up[2];
			v3f[ 6] = p->org[0] + right[0] + up[0];
			v3f[ 7] = p->org[1] + right[1] + up[1];
			v3f[ 8] = p->org[2] + right[2] + up[2];
			v3f[ 9] = p->org[0] + right[0] - up[0];
			v3f[10] = p->org[1] + right[1] - up[1];
			v3f[11] = p->org[2] + right[2] - up[2];
			t2f[0] = tex->s1;t2f[1] = tex->t2;
			t2f[2] = tex->s1;t2f[3] = tex->t1;
			t2f[4] = tex->s2;t2f[5] = tex->t1;
			t2f[6] = tex->s2;t2f[7] = tex->t2;
			break;
		case PARTICLE_SPARK:
			len = VectorLength(p->vel);
			VectorNormalize2(p->vel, up);
			lenfactor = p->stretch * 0.04 * len;
			if(lenfactor < size * 0.5)
				lenfactor = size * 0.5;
			VectorMA(p->org, -lenfactor, up, v);
			VectorMA(p->org,  lenfactor, up, up2);
			R_CalcBeam_Vertex3f(v3f, v, up2, size);
			t2f[0] = tex->s1;t2f[1] = tex->t2;
			t2f[2] = tex->s1;t2f[3] = tex->t1;
			t2f[4] = tex->s2;t2f[5] = tex->t1;
			t2f[6] = tex->s2;t2f[7] = tex->t2;
			break;
		case PARTICLE_VBEAM:
			R_CalcBeam_Vertex3f(v3f, p->org, p->vel, size);
			VectorSubtract(p->vel, p->org, up);
			VectorNormalize(up);
			v[0] = DotProduct(p->org, up) * (1.0f / 64.0f) * p->stretch;
			v[1] = DotProduct(p->vel, up) * (1.0f / 64.0f) * p->stretch;
			t2f[0] = tex->s2;t2f[1] = v[0];
			t2f[2] = tex->s1;t2f[3] = v[0];
			t2f[4] = tex->s1;t2f[5] = v[1];
			t2f[6] = tex->s2;t2f[7] = v[1];
			break;
		case PARTICLE_HBEAM:
			R_CalcBeam_Vertex3f(v3f, p->org, p->vel, size);
			VectorSubtract(p->vel, p->org, up);
			VectorNormalize(up);
			v[0] = DotProduct(p->org, up) * (1.0f / 64.0f) * p->stretch;
			v[1] = DotProduct(p->vel, up) * (1.0f / 64.0f) * p->stretch;
			t2f[0] = v[0];t2f[1] = tex->t1;
			t2f[2] = v[0];t2f[3] = tex->t2;
			t2f[4] = v[1];t2f[5] = tex->t2;
			t2f[6] = v[1];t2f[7] = tex->t1;
			break;
		}
		if (r_showparticleedges.integer)
		{
			R_DebugLine(v3f, v3f + 3);
			R_DebugLine(v3f + 3, v3f + 6);
			R_DebugLine(v3f + 6, v3f + 9);
			R_DebugLine(v3f + 9, v3f);
		}
	}

	// now render batches of particles based on blendmode and texture
	blendmode = PBLEND_INVALID;
	texture = NULL;
	batchstart = 0;
	batchcount = 0;
	R_Mesh_PrepareVertices_Generic_Arrays(numsurfaces * 4, particle_vertex3f, particle_color4f, particle_texcoord2f);
	for (surfacelistindex = 0;surfacelistindex < numsurfaces;)
	{
		p = cl.particles + surfacelist[surfacelistindex];

		if (texture != particletexture[p->texnum].texture)
		{
			texture = particletexture[p->texnum].texture;
			R_SetupShader_Generic(texture, false, false, false);
		}

		// BEAUTY A3: a batch is also broken on the viewmodel flag, and drawn with
		// the depth test OFF when it is set -- over the weapon, as a weapon flash
		// is drawn everywhere. The view weapon's own compressed depth range was
		// tried first: a burst placed just beyond the barrel's tip is by
		// definition farther than the end face, so a muzzle wider than the burst
		// (the rocket launcher's) hid it whole. Zero on every stock particle, so
		// the batching and the state are the old ones to the byte then.
		viewmodelbatch = p->viewmodel;
		GL_DepthTest(!viewmodelbatch);
		refractcell = -1;
		if (p->blendmode == PBLEND_INVMOD)
		{
			// inverse modulate blend - group these
			GL_BlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
			// iterate until we find a change in settings
			batchstart = surfacelistindex++;
			for (;surfacelistindex < numsurfaces;surfacelistindex++)
			{
				p = cl.particles + surfacelist[surfacelistindex];
				if (p->blendmode != PBLEND_INVMOD || texture != particletexture[p->texnum].texture || p->viewmodel != viewmodelbatch)
					break;
			}
		}
		else if (p->blendmode == PBLEND_REFRACT)
		{
			// BEAUTY A5: refracting particles -- premultiplied over, and grouped by
			// CELL as well as texture, because the batch's uniform carries the
			// cell's centre and size (the radial direction is measured from it)
			int cellnum = p->texnum;
			GL_BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
			batchstart = surfacelistindex++;
			for (;surfacelistindex < numsurfaces;surfacelistindex++)
			{
				p = cl.particles + surfacelist[surfacelistindex];
				if (p->blendmode != PBLEND_REFRACT || p->texnum != cellnum || texture != particletexture[p->texnum].texture || p->viewmodel != viewmodelbatch)
					break;
			}
			refractcell = cellnum;
		}
		else
		{
			// additive or alpha blend - group these
			// (we can group these because we premultiplied the texture alpha)
			GL_BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
			// iterate until we find a change in settings
			batchstart = surfacelistindex++;
			for (;surfacelistindex < numsurfaces;surfacelistindex++)
			{
				p = cl.particles + surfacelist[surfacelistindex];
				if (p->blendmode == PBLEND_INVMOD || p->blendmode == PBLEND_REFRACT || texture != particletexture[p->texnum].texture || p->viewmodel != viewmodelbatch)
					break;
			}
		}

		batchcount = surfacelistindex - batchstart;
		// BEAUTY A5: the refract batch's cell geometry and strength; 0 on every
		// other batch, which also un-arms a refract batch's leftovers on the same
		// texture (the uniform persists across batches that share a setup).
		if (refractcell >= 0)
			R_PartRefract_Batch(cl_particles_refract.value, 0.5f * (particletexture[refractcell].s1 + particletexture[refractcell].s2), 0.5f * (particletexture[refractcell].t1 + particletexture[refractcell].t2), 2.0f / max(1e-6f, particletexture[refractcell].s2 - particletexture[refractcell].s1));
		else
			R_PartRefract_Batch(0.0f, 0.0f, 0.0f, 0.0f);
		refractcell = -1;
		// BEAUTY A4: soft particles -- the fade distance for this batch, 0 for
		// the viewmodel flash (drawn over the gun with the depth test off; a fade
		// against the gun would put it out) and 0 when the feature is off.
		R_SoftParticles_Batch(viewmodelbatch ? 0.0f : cl_particles_soft.value);
		R_Mesh_Draw(batchstart * 4, batchcount * 4, batchstart * 2, batchcount * 2, NULL, NULL, 0, particle_elements, NULL, 0);
		// The MetalFX-temporal arc's REACTIVE STAMP (2026-08-19): hand the
		// batch just drawn -- the same world-space quads, the same premultiplied
		// colour, the same texture -- to the reactive-mask pass, which replays
		// it after the scene so the temporal scaler distrusts its history where
		// the particles landed. Particles write no depth and carry no motion
		// vector, so nothing else can tell the scaler they are there, and on
		// the rocket bed they were 78% of what ghosted. Pure memcpy, armed
		// only on a frame whose mask will run; one branch otherwise.
		if (R_ReactiveStamp_Active())
			R_ReactiveStamp_Append(texture, batchcount * 4, particle_vertex3f + batchstart * 12, particle_color4f + batchstart * 16, particle_texcoord2f + batchstart * 8);
	}
	GL_DepthTest(true);		// BEAUTY A3: a viewmodel batch may have switched it off
}

void R_DrawParticles (void)
{
	int i, a;
	int drawparticles = r_drawparticles.integer;
	float minparticledist_start;
	particle_t *p;
	float gravity, frametime, f, dist, oldorg[3], decaldir[3];
	float drawdist2;
	int hitent;
	trace_t trace;
	qbool update;
	float pt_explode_frame_interval, pt_explode2_frame_interval;
	int color;

	frametime = bound(0, cl.time - cl.particles_updatetime, 1);
	cl.particles_updatetime = bound(cl.time - 1, cl.particles_updatetime + frametime, cl.time + 1);

	// SEPTEMBER S7: hand the dust emitter last call's count and start this one's.
	// ABOVE the empty-pool early-out on purpose: a map change (or the startup
	// demo's disconnect) empties the pool, and a count refreshed only inside the
	// walk would hold the OLD map's figure for ever -- measured: 283 motes
	// "alive" in an empty pool, so the emitter never spawned again.
	cl_dust_live = cl_dust_count;
	cl_dust_count = 0;

	// LadyHavoc: early out conditions
	if (!cl.num_particles)
		return;

	// SEPTEMBER S7: this frame's lit-particle inputs (a no-op below mode 2)
	CL_ParticleLight_Prepare();

	// Handling of the colour ramp for pt_explode and pt_explode2
	pt_explode_frame_interval = frametime * 10;
	pt_explode2_frame_interval = frametime * 15;

	minparticledist_start = DotProduct(r_refdef.view.origin, r_refdef.view.forward) + r_drawparticles_nearclip_min.value;
	gravity = frametime * cl.movevars_gravity;
	update = frametime > 0;
	drawdist2 = r_drawparticles_drawdistance.value * r_refdef.view.quality;
	drawdist2 = drawdist2*drawdist2;

	for (i = 0, p = cl.particles;i < cl.num_particles;i++, p++)
	{
		if (!p->typeindex)
		{
			if (cl.free_particle > i)
				cl.free_particle = i;
			continue;
		}
		if (p->typeindex == pt_dust)
			cl_dust_count++;

		if (update)
		{
			if (p->delayedspawn > cl.time)
				continue;

			p->size += p->sizeincrease * frametime;
			p->alpha -= p->alphafade * frametime;

			if (p->alpha <= 0 || p->die <= cl.time)
				goto killparticle;

			if (p->orientation != PARTICLE_VBEAM && p->orientation != PARTICLE_HBEAM && frametime > 0)
			{
				if (p->liquidfriction && cl_particles_collisions.integer && (CL_PointSuperContents(p->org) & SUPERCONTENTS_LIQUIDSMASK))
				{
					if (p->typeindex == pt_blood)
						p->size += frametime * 8;
					else
						p->vel[2] -= p->gravity * gravity;
					f = 1.0f - min(p->liquidfriction * frametime, 1);
					VectorScale(p->vel, f, p->vel);
				}
				else
				{
					p->vel[2] -= p->gravity * gravity;
					if (p->airfriction)
					{
						f = 1.0f - min(p->airfriction * frametime, 1);
						VectorScale(p->vel, f, p->vel);
					}
				}

				VectorCopy(p->org, oldorg);
				VectorMA(p->org, frametime, p->vel, p->org);
//				if (p->bounce && cl.time >= p->delayedcollisions)
				if (p->bounce && cl_particles_collisions.integer && VectorLength(p->vel))
				{
					trace = CL_TraceLine(oldorg, p->org, MOVE_NORMAL, NULL, SUPERCONTENTS_SOLID | ((p->typeindex == pt_rain || p->typeindex == pt_snow) ? SUPERCONTENTS_LIQUIDSMASK : 0), 0, 0, collision_extendmovelength.value, true, false, &hitent, false, false);
					// if the trace started in or hit something of SUPERCONTENTS_NODROP
					// or if the trace hit something flagged as NOIMPACT
					// then remove the particle
					if (trace.hitq3surfaceflags & Q3SURFACEFLAG_NOIMPACT || ((trace.startsupercontents | trace.hitsupercontents) & SUPERCONTENTS_NODROP) || (trace.startsupercontents & SUPERCONTENTS_SOLID))
						goto killparticle;
					VectorCopy(trace.endpos, p->org);
					// react if the particle hit something
					if (trace.fraction < 1)
					{
						VectorCopy(trace.endpos, p->org);

						if (p->staintexnum >= 0)
						{
							// blood - splash on solid
							if (!(trace.hitq3surfaceflags & Q3SURFACEFLAG_NOMARKS))
							{
								R_Stain(p->org, 16,
									p->staincolor[0], p->staincolor[1], p->staincolor[2], (int)(p->stainalpha * p->stainsize * (1.0f / 160.0f)),
									p->staincolor[0], p->staincolor[1], p->staincolor[2], (int)(p->stainalpha * p->stainsize * (1.0f / 160.0f)));
								if (cl_decals.integer)
								{
									// create a decal for the blood splat
									a = 0xFFFFFF ^ (p->staincolor[0]*65536+p->staincolor[1]*256+p->staincolor[2]);
									if (cl_decals_newsystem_bloodsmears.integer)
									{
										VectorCopy(p->vel, decaldir);
										VectorNormalize(decaldir);
									}
									else
										VectorCopy(trace.plane.normal, decaldir);
									CL_SpawnDecalParticleForSurface(hitent, p->org, decaldir, a, a, p->staintexnum, p->stainsize, p->stainalpha); // staincolor needs to be inverted for decals!
								}
							}
						}

						if (p->typeindex == pt_blood)
						{
							// blood - splash on solid
							if (trace.hitq3surfaceflags & Q3SURFACEFLAG_NOMARKS)
								goto killparticle;
							if(p->staintexnum == -1) // staintex < -1 means no stains at all
							{
								R_Stain(p->org, 16, 64, 16, 16, (int)(p->alpha * p->size * (1.0f / 80.0f)), 64, 32, 32, (int)(p->alpha * p->size * (1.0f / 80.0f)));
								if (cl_decals.integer)
								{
									// create a decal for the blood splat
									if (cl_decals_newsystem_bloodsmears.integer)
									{
										VectorCopy(p->vel, decaldir);
										VectorNormalize(decaldir);
									}
									else
										VectorCopy(trace.plane.normal, decaldir);
									CL_SpawnDecalParticleForSurface(hitent, p->org, decaldir, p->color[0] * 65536 + p->color[1] * 256 + p->color[2], p->color[0] * 65536 + p->color[1] * 256 + p->color[2], tex_blooddecal[rand()&7], p->size * lhrandom(cl_particles_blood_decal_scalemin.value, cl_particles_blood_decal_scalemax.value), cl_particles_blood_decal_alpha.value * 768);
								}
							}
							goto killparticle;
						}
						else if (p->bounce < 0)
						{
							// bounce -1 means remove on impact
							goto killparticle;
						}
						else
						{
							// anything else - bounce off solid
							dist = DotProduct(p->vel, trace.plane.normal) * -p->bounce;
							VectorMA(p->vel, dist, trace.plane.normal, p->vel);
						}
					}
				}

				if (VectorLength2(p->vel) < 0.03)
				{
					if(p->orientation == PARTICLE_SPARK) // sparks are virtually invisible if very slow, so rather let them go off
						goto killparticle;
					VectorClear(p->vel);
				}
			}

			if (p->typeindex != pt_static)
			{
				switch (p->typeindex)
				{
				case pt_entityparticle:
					// particle that removes itself after one rendered frame
					if (p->time2)
						goto killparticle;
					else
						p->time2 = 1;
					break;
				case pt_blood:
					a = CL_PointSuperContents(p->org);
					if (a & (SUPERCONTENTS_SOLID | SUPERCONTENTS_LAVA | SUPERCONTENTS_NODROP))
						goto killparticle;
					break;
				case pt_bubble:
					a = CL_PointSuperContents(p->org);
					if (!(a & (SUPERCONTENTS_WATER | SUPERCONTENTS_SLIME)))
						goto killparticle;
					break;
				case pt_rain:
					a = CL_PointSuperContents(p->org);
					if (a & (SUPERCONTENTS_SOLID | SUPERCONTENTS_LIQUIDSMASK))
						goto killparticle;
					break;
				case pt_snow:
					if (cl.time > p->time2)
					{
						// snow flutter
						p->time2 = cl.time + (rand() & 3) * 0.1;
						p->vel[0] = p->vel[0] * 0.9f + lhrandom(-32, 32);
						p->vel[1] = p->vel[0] * 0.9f + lhrandom(-32, 32);
					}
					a = CL_PointSuperContents(p->org);
					if (a & (SUPERCONTENTS_SOLID | SUPERCONTENTS_LIQUIDSMASK))
						goto killparticle;
					break;
				case pt_dust:
					// SEPTEMBER S7: recycle a mote that has wandered off, into a
					// wall or into water; eddy the survivors. r_refdef.view.origin
					// IS valid here (inside R_RenderView). The eddy is three slow
					// sines on position and time -- an analytic stand-in for the
					// noise volume, which lives only on the GPU -- with the air
					// friction set at birth doing the damping, so the drift settles
					// at a few units a second rather than winding up.
					{
						float dr = max(64.0f, m5_dust_radius.value) * 2.0f;
						if (VectorDistance2(p->org, r_refdef.view.origin) > dr * dr)
							goto killparticle;
						if (CL_PointSuperContents(p->org) & (SUPERCONTENTS_SOLID | SUPERCONTENTS_LIQUIDSMASK))
							goto killparticle;
						if (m5_dust_swirl.value > 0.0f)
						{
							float sw = m5_dust_swirl.value * 4.0f * frametime;
							float t = (float)cl.time * 0.4f;
							p->vel[0] += sw * sinf(p->org[1] * 0.017f + t);
							p->vel[1] += sw * sinf(p->org[2] * 0.021f - t * 1.3f);
							p->vel[2] += sw * 0.5f * sinf(p->org[0] * 0.019f + t * 0.7f);
						}
					}
					break;
				case pt_explode:
					// Progress the particle colour up the ramp
					p->time2 += pt_explode_frame_interval;
					if (p->time2 >= 8)
						p->die = -1;
					else
					{
						color = particlepalette[ramp1[(int)p->time2]];
						p->color[0] = color >> 16;
						p->color[1] = color >>  8;
						p->color[2] = color >>  0;
					}
					break;
				case pt_explode2:
					// Progress the particle colour up the ramp
					p->time2 += pt_explode2_frame_interval;
					if (p->time2 >= 8)
						p->die = -1;
					else
					{
						color = particlepalette[ramp2[(int)p->time2]];
						p->color[0] = color >> 16;
						p->color[1] = color >>  8;
						p->color[2] = color >>  0;
					}
					break;
				default:
					break;
				}
			}
		}
		else if (p->delayedspawn > cl.time)
			continue;
		if (!drawparticles)
			continue;
		// don't render particles too close to the view (they chew fillrate)
		// also don't render particles behind the view (useless)
		// further checks to cull to the frustum would be too slow here
		switch(p->typeindex)
		{
		case pt_beam:
			// beams have no culling
			R_MeshQueue_AddTransparent(TRANSPARENTSORT_DISTANCE, p->sortorigin, R_DrawParticle_TransparentCallback, NULL, i, NULL);
			break;
		default:
			if(cl_particles_visculling.integer)
				if (!r_refdef.viewcache.world_novis)
					if(r_refdef.scene.worldmodel && r_refdef.scene.worldmodel->brush.PointInLeaf)
					{
						mleaf_t *leaf = r_refdef.scene.worldmodel->brush.PointInLeaf(r_refdef.scene.worldmodel, p->org);
						if(leaf)
							if(!CHECKPVSBIT(r_refdef.viewcache.world_pvsbits, leaf->clusterindex))
								continue;
					}
			// anything else just has to be in front of the viewer and visible at this distance
			if (!r_refdef.view.useperspective || (DotProduct(p->org, r_refdef.view.forward) >= minparticledist_start && VectorDistance2(p->org, r_refdef.view.origin) < drawdist2 * (p->size * p->size)))
			{
				R_MeshQueue_AddTransparent(TRANSPARENTSORT_DISTANCE, p->sortorigin, R_DrawParticle_TransparentCallback, NULL, i, NULL);
				if (p->blendmode == PBLEND_REFRACT && r_refdef.view.ismain)
					r_fb.refractseen = true;   // BEAUTY A5: the frame copy is worth taking this view
			}
			break;
		}

		continue;
killparticle:
		p->typeindex = 0;
		if (cl.free_particle > i)
			cl.free_particle = i;
	}

	// reduce cl.num_particles if possible
	while (cl.num_particles > 0 && cl.particles[cl.num_particles - 1].typeindex == 0)
		cl.num_particles--;

	if (cl.num_particles == cl.max_particles && cl.max_particles < MAX_PARTICLES)
	{
		particle_t *oldparticles = cl.particles;
		cl.max_particles = min(cl.max_particles * 2, MAX_PARTICLES);
		cl.particles = (particle_t *) Mem_Alloc(cls.levelmempool, cl.max_particles * sizeof(particle_t));
		memcpy(cl.particles, oldparticles, cl.num_particles * sizeof(particle_t));
		Mem_Free(oldparticles);
	}
}
