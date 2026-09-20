/*
==============================================================================

M5 BURN — the lightning gun sets things on fire

Gated on the m5_burn cvar (registered engine-side beside m5_gore):
  0  off, vanilla lightning
  1  on — victims catch fire, smoke, and take burn damage over time
  2  ludicrous — burns for twice as long and bites harder

Almost all of this was already in the engine and simply unreachable. EF_FLAME
(protocol.h) has always drawn 300/sec of orange smoke AND allocated a warm
dynamic light (cl_particles.c, EFFECT_EF_FLAME) — stock QuakeC just never
declared the bit, so nothing could ask for it. Two consequences worth knowing:

  - dynamic lights feed the RT sidecar's light list, so a burning monster
    LIGHTS THE VOLUMETRIC FOG, casts real shadows, and lights the walls around
    it. That falls out for free; none of it is faked here.
  - the fire is drawn entirely by the client from one effects bit, so the
    server cost is this file and nothing else.

The burn is swept from StartFrame on a half-second throttle rather than given
its own think chain, because monsters drive their animation through .think and
borrowing it would break them. The sweep idles at zero cost while nothing is
alight (m5_burncount).

==============================================================================
*/

float m5_burnnext;		// next sweep time
float m5_burncount;		// how many entities are alight; 0 = the sweep sleeps

/*
================
M5_Douse

Put an entity out and hand back everything the burn borrowed.
================
*/
void(entity e) M5_Douse =
{
	if (!e.m5_burnuntil)
		return;
	e.m5_burnuntil = 0;
	e.m5_burntick = 0;
	e.m5_burnfrom = world;
	e.m5_burnforce = 0;
	e.effects = e.effects - (e.effects & EF_FLAME);
	e.colormod = '1 1 1';
	e.glowmod = '1 1 1';
	if (m5_burncount > 0)
		m5_burncount = m5_burncount - 1;
};

/*
================
M5_Ignite

Set a target alight, or top up a fire already burning. Symmetric on purpose:
anything that takes damage can burn, so a Shambler's bolt sets the PLAYER
alight too, and a burning zombie keeps getting back up. That is the fun of it,
and m5_burn defaults to 0, so nobody meets it without asking. (The Shambler's
only vanilla resistance is to EXPLOSIONS, half damage, in combat.qc -- it has
no lightning immunity, so it burns like everything else.)
================
*/
void(entity targ, entity attacker, float force) M5_IgniteAt =
{
	local float tier, dur;

	tier = cvar("m5_burn");
	// PLASMA (SEPTEMBER2, 2026-09-10): the ball lightning sets alight whatever
	// it zaps whether or not Lightning Ignites is on -- it is plasma, and Seb's
	// brief for it is "hit & ignite anything nearby even if it doesn't kill
	// them". A forced fire burns at tier 1 under m5_burn 0 and is not doused by
	// the tier check in M5_BurnThink; the bolt's own fires still obey m5_burn.
	if (tier <= 0)
	{
		if (!force)
			return;
		tier = 1;
	}
	if (targ == world || targ.takedamage == DAMAGE_NO || targ.health <= 0)
		return;
	if (force)
		targ.m5_burnforce = TRUE;

	dur = 4;
	if (tier >= 2)
		dur = 8;

	if (!targ.m5_burnuntil)
	{
		m5_burncount = m5_burncount + 1;
		targ.m5_burntick = time + 0.5;
		// FIRST ignition only (the console-volume incident): the thunderbolt
		// re-ignites its victim on every 10 Hz damage tick, and this line used
		// to sit below the refresh too -- 10-30 lines/s into Xcode's console
		// while the beam was held on a crowd, with developer 1 archived.
		dprint ("M5 burn: ignited\n");	// developer 1 only; asserted by tests/smoke.sh
	}
	if (targ.m5_burnuntil < time + dur)
		targ.m5_burnuntil = time + dur;
	targ.m5_burnfrom = attacker;
	targ.effects = targ.effects | EF_FLAME;
};

void(entity targ, entity attacker) M5_Ignite =
{
	M5_IgniteAt (targ, attacker, FALSE);
};

// the ball's: burns whatever m5_burn says (see M5_IgniteAt)
void(entity targ, entity attacker) M5_IgnitePlasma =
{
	M5_IgniteAt (targ, attacker, TRUE);
};

/*
================
M5_BurnThink

The half-second sweep: burn damage, the charring ramp, and expiry.

colormod ramps the whole skin down towards a dark ember red, which reads on
the stock 1996 skins as scorching. glowmod ramps the model's GLOW layer hot —
that is the honest path for emissive wounds, but stock Quake skins carry
almost no fullbright texels, so it does nothing visible until glow skins are
authored. It is set anyway: it costs a vector, and the moment a _glow skin
lands the embers appear with no further code.
================
*/
void() M5_BurnThink =
{
	local entity e, oself;
	local float tier, dmg, frac, dur;

	if (m5_burncount <= 0)
		return;

	tier = cvar("m5_burn");
	dmg = 3;
	dur = 4;
	if (tier >= 2)
	{
		dmg = 6;
		dur = 8;
	}

	e = nextent (world);
	while (e != world)
	{
		if (e.m5_burnuntil)
		{
			if ((tier <= 0 && !e.m5_burnforce) || e.health <= 0 || time > e.m5_burnuntil)
			{
				M5_Douse (e);
			}
			else if (time > e.m5_burntick)
			{
				e.m5_burntick = time + 0.5;
				// how far through the burn we are, 0 at ignition to 1 at the end
				frac = 1 - (e.m5_burnuntil - time) / dur;
				if (frac < 0)
					frac = 0;
				if (frac > 1)
					frac = 1;
				e.colormod = '1 1 1' + frac * '-0.45 -0.7 -0.75';
				e.glowmod = '1 1 1' + frac * '1.5 0.2 -0.6';

				oself = self;
				self = e;
				// inflictor is the victim itself: the fire is on them, so a
				// burning monster that dies is credited to whoever lit it
				// without the corpse being flung about by a phantom impact
				T_Damage (e, e, e.m5_burnfrom, dmg);
				self = oself;
			}
		}
		e = nextent (e);
	}
};

/*
================
M5_BurnFrame

Called every server frame from StartFrame; throttles itself to twice a second.
================
*/
void() M5_BurnFrame =
{
	if (time < m5_burnnext)
		return;
	m5_burnnext = time + 0.5;
	M5_BurnThink ();
};
