/*
==============================================================================

M5 VENOM — the Scrag's spit reads as acid

Gated on the m5_venom cvar (registered engine-side beside m5_burn):
  0  off, vanilla spit
  1  on — the projectile carries its own yellow-green light and its impact
         sizzles

The look Seb asked for is the acid blood from the Alien films: a hot
yellow-green core with a tight falloff, bright enough to light the wall it is
passing and the fog it is passing through, and a brief sizzle where it lands.

Three facts decided the mechanism, all measured rather than assumed:

  - progs/w_spike.mdl has ZERO palette-fullbright texels, in both the id1 copy
    and the AMI replacement that actually loads. So it has no glow layer, .glowmod
    is inert on it, and nothing about the MODEL can be made to emit light by
    setting a field. Its dominant palette indices are 194-200, which is already a
    mustard yellow-green -- the colour was never the problem, the model is simply
    unlit. (The KNIGHT spike sits at 224-239, the fullbright ramp, and glows for
    free. That asymmetry is why one of Quake's two spit projectiles has always
    looked hot and the other has not.)

  - PFLAGS_FULLDYNAMIC gives a real per-entity light with arbitrary RGB, and its
    corona is OPT-IN. The older .glow_size path forces corona 1, which with the
    player's gl_flashblend + r_coronas configuration would put an additive screen
    blob on every spike in flight -- the same trap the thunderbolt work hit.

  - a dynamic light feeds the RT sidecar's light list, so this lights the
    volumetric fog and casts real shadows with no extra work. That is where most
    of the effect comes from.

Cost is bounded: each spike holds one MAX_DLIGHTS slot while it is alive, and a
light contributes exactly nothing past its radius, so the small radius here is
cheap in the kernel's tile cull. A Scrag fires two per volley.

==============================================================================
*/

// The acid colour. Yellow-green, weighted well off pure green so it reads as
// caustic rather than radioactive, and hot enough at the core to lift the
// surface it lands on.
vector M5_VENOM_COLOUR = '0.65 1 0.12';

/*
=================
M5Venom_Light

Hangs an acid light on a spit projectile. Called from Wiz_FastFire on the
missile launch_spike has just built.
=================
*/
// One diagnostic line per map, not per spike (the console-volume incident: a
// scrag pack prints several times a second, and with developer 1 archived it
// all lands in Xcode's console forever). Globals reset on map load, so the
// smoke bed's first spit still prints.
float m5_venom_reported;

void(entity spike) M5Venom_Light =
{
	local float tier;

	tier = cvar("m5_venom");
	if (tier <= 0)
		return;

	// The model itself. w_spike.mdl has no glow layer to scale, so .glowmod is
	// inert on it and the only lever that works is to stop the room darkening
	// it: EF_FULLBRIGHT draws it at its own albedo, which is already the right
	// colour. A proper emissive skin would want an authored _glow texture pair,
	// which is a content drop rather than a code change -- see SETTINGS.md.
	spike.effects = spike.effects | EF_FULLBRIGHT;

	spike.color = M5_VENOM_COLOUR;
	spike.light_lev = 120;
	if (!m5_venom_reported)
	{
		m5_venom_reported = TRUE;
		dprint ("M5 venom: acid light on spit\n");	// asserted by tests/smoke.sh; first spit per map
	}
	// NOSHADOW: the light itself needs no shadow volume -- occlusion for the
	// features that matter (the fog kernel, the RT surface pass) is traced by
	// the sidecar, and asking the GL path for one would cost without showing.
	// No CORONA, deliberately: see the header.
	spike.pflags = PFLAGS_FULLDYNAMIC | PFLAGS_NOSHADOW;
};

/*
=================
M5Venom_Splash

The sizzle where a spit lands. Vanilla sends a TE_WIZSPIKE when the spit hits
the WORLD and nothing at all when it hits flesh, so a Scrag scoring a hit on you
or on another monster produced no impact effect whatsoever. This fires on both.

The light is a throwaway entity because the projectile is removed on touch and
the flash has to outlive it; 0.12 s is long enough to read and short enough that
a wave of Scrags cannot fill MAX_DLIGHTS with corpses of it.
=================
*/
void() M5Venom_SplashFade =
{
	remove (self);
};

void(vector org) M5Venom_Splash =
{
	local entity flash;
	local float tier;

	tier = cvar("m5_venom");
	if (tier <= 0)
		return;

	flash = spawn ();
	setorigin (flash, org);
	flash.classname = "venomsplash";
	flash.movetype = MOVETYPE_NONE;
	flash.solid = SOLID_NOT;
	flash.color = M5_VENOM_COLOUR;
	flash.light_lev = 200;
	flash.pflags = PFLAGS_FULLDYNAMIC | PFLAGS_NOSHADOW;
	flash.think = M5Venom_SplashFade;
	flash.nextthink = time + 0.12;
};
