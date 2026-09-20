/*
==============================================================================

M5 BALL LIGHTNING -- the ninth weapon (BALLLIGHTNING.md)

Gated on the m5_balllightning cvar (registered engine-side beside m5_venom):
  0  off. The weapon does not exist: impulse 202 says so and does nothing else,
     the cycle keys skip it, and no ball can ever be spawned.
  1  on. Owning the thunderbolt grants the ball gun beside it (IT_M5BALL, the
     spare IT_EXTRA_WEAPON bit); impulse 202 selects it, and again for the bolt.

The thunderbolt is a straight hitscan line: hold fire, thirty damage a tick to
whatever the beam touches. The ball is the same energy as an AREA and a TIME:
a slow sphere of plasma launched down the aim line that arcs at the two nearest
living things within reach every tenth of a second, leans gently toward what it
is zapping, and detonates when its life runs out, when it meets water, or when
it hits a wall (Seb's call, 2026-09-06). Same cells, a different shape of danger.

WHAT THE CLIENT DOES WITH IT. The ball carries EF_M5BALL (protocol.h) and a
PFLAGS_FULLDYNAMIC light. The engine draws the plasma knot at every EF_M5BALL
entity through the bolt renderer (r_lightning.c, CL_Beam_M5_AddBall), gives the
light the bolt's own hue and flicker (the beam colour cvars are CLIENT-only and
invisible to this VM, so .color here carries only a brightness), weights it in
the fog like the bolt's lights, and roots the arcs on the moving entity. The
arcs are TE_LIGHTNING1/2 beams: one per OWNING ENTITY (CL_ParseBeam replaces
a beam with the same owner), so the second arc is owned by a helper entity
(m5arc) that follows the ball and carries EF_M5BALL | EF_NODRAW -- tagged so
the arc roots on it, NODRAW so no knot is drawn there. TE_LIGHTNING1 is the
connect signal (the bolt's own): an arc that damaged something burns brighter.
The glow trail and the discharge burst are named m5/effectinfo.txt blocks
(m5ball_glow through .traileffectnum, m5ball_burst through pointparticles).

Physics: MOVETYPE_FLY + SOLID_TRIGGER. A SOLID_TRIGGER mover clips against
bmodels only (sv_phys.c: type = MOVE_NOMONSTERS), so the ball drifts THROUGH
monsters and players and stops on the world and on doors; SV_Impact then fires
touch with the bsp entity as `other`, which is the detonation. Monsters walking
into it fire touch too (SV_TouchAreaGrid) and are ignored there.

Traps this file was written against (CLAUDE.md's hard-won facts):
  - LightningDamage runs three tracelines and clobbers trace_endpos; nothing
    here reads trace_* after calling it, and a target's origin is copied first.
  - every dprint is FIRST-EVENT per map (globals reset on map load) -- a
    ten-hertz print under developer 1 is the 92 GB console-ink incident.
  - particleeffectnum on a name the effectinfo does not carry returns 0 with a
    console warning; both handles are looked up once per map and skipped at 0.

==============================================================================
*/

void(entity own, vector from, vector to, float connected) M5Ball_Arc;	// defined with the think, used by the touch

float m5_ball_reported_launch;	// first-event dprints, one each per map
float m5_ball_reported_arc;
float m5_ball_reported_hit;
float m5_ball_reported_chain;
float m5_ball_reported_earth;	// the first earthing per map, with how far it got
float m5_ball_fxready;			// effectinfo handles looked up (once per map)
float m5_ball_glowfx;
float m5_ball_burstfx;
float m5_ball_fizzlefx;
float m5_ball_shatterfx;

void() M5Ball_Effects =
{
	if (m5_ball_fxready)
		return;
	m5_ball_fxready = TRUE;
	m5_ball_glowfx = particleeffectnum ("m5ball_glow");
	m5_ball_burstfx = particleeffectnum ("m5ball_burst");
	m5_ball_fizzlefx = particleeffectnum ("m5ball_fizzle");
	m5_ball_shatterfx = particleeffectnum ("m5ball_shatter");
	dprint ("M5 ball: effects glow ");	// asserted by tests/smoke.sh (both > 0)
	dprint (ftos(m5_ball_glowfx));
	dprint (" burst ");
	dprint (ftos(m5_ball_burstfx));
	dprint ("\n");
};

/*
=================
M5Ball_Sync

Called every W_WeaponFrame. Grants IT_M5BALL to a player who owns the
thunderbolt while the cvar is on, so the stock item checks in W_ChangeWeapon
and the cycle commands work unchanged; withdraws it the moment the cvar goes
off, dropping to the best weapon if the ball gun was in hand. At cvar 0 on a
player who never had the bit this is one cvar() read and nothing else.
=================
*/
void() M5Ball_Sync =
{
	if (cvar("m5_balllightning"))
	{
		if ((self.items & IT_LIGHTNING) && !(self.items & IT_M5BALL))
			self.items = self.items | IT_M5BALL;
	}
	else if (self.items & IT_M5BALL)
	{
		self.items = self.items - IT_M5BALL;
		if (self.weapon == IT_M5BALL)
		{
			self.weapon = W_BestWeapon ();
			W_SetCurrentAmmo ();
		}
	}
};

/*
=================
M5Ball_Select

impulse 202: the ball gun if the thunderbolt is owned and the cells are there;
pressed with the ball gun already in hand, back to the thunderbolt. Wording
matches W_ChangeWeapon's own refusals.
=================
*/
void() M5Ball_Select =
{
	if (!cvar("m5_balllightning"))
	{
		sprint (self, "ball lightning is off: m5_balllightning 1\n");
		return;
	}
	if (self.weapon == IT_M5BALL)
	{
		if (self.items & IT_LIGHTNING)
		{
			self.weapon = IT_LIGHTNING;
			W_SetCurrentAmmo ();
		}
		return;
	}
	if (!(self.items & IT_LIGHTNING))
	{
		sprint (self, "no weapon.\n");
		return;
	}
	if (self.ammo_cells < cvar("m5_balllightning_cost"))
	{
		sprint (self, "not enough ammo.\n");
		return;
	}
	self.items = self.items | IT_M5BALL;
	self.weapon = IT_M5BALL;
	W_SetCurrentAmmo ();
};

/*
=================
M5Ball_Discharge

Life over, a wall, or liquid (or sky). T_RadiusDamage credits the owner and
takes findradius(damage + 40), the vanilla shape; the burst -- blink, ring,
sparks and a 0.15 s flash light -- is the m5ball_burst effectinfo block, fired
at the ball's position. The arc helper goes with the ball.
=================
*/
void() M5Ball_ClearTint =
{
	// (nothing to clear since the cast moved client-side; kept as the one
	// place a future server-side pressure would be undone)
};

// The arc helpers are a CHAIN (m5_arc -> m5_arc -> ...), one per arc past the
// first (m5_balllightning_arcs - 1 of them); this walks it.
void() M5Ball_RemoveHelpers =
{
	local entity h, n;

	h = self.m5_arc;
	self.m5_arc = world;
	while (h != world)
	{
		n = h.m5_arc;
		remove (h);
		h = n;
	}
};

void() M5Ball_Discharge =
{
	local float burst;
	local entity e;

	M5Ball_ClearTint ();

	burst = cvar("m5_balllightning_burst");
	if (burst > 0)
	{
		T_RadiusDamage (self, self.owner, burst, world);
		// and everything in the blast that lived through it catches fire
		// (M5_Ignite is gated on m5_burn itself) -- destructive, Seb's word
		e = findradius (self.origin, burst + 40);
		while (e)
		{
			if (e.takedamage && e.health > 0 && e != self.owner)
				M5_IgnitePlasma (e, self.owner);
			e = e.chain;
		}
	}
	if (m5_ball_burstfx > 0)
		pointparticles (m5_ball_burstfx, self.origin, '0 0 0', 1);
	sound (self, CHAN_AUTO, "ambience/thunder1.wav", 1, ATTN_NORM);
	M5Ball_RemoveHelpers ();
	remove (self);
};

/*
=================
M5Ball_ShatterThink / M5Ball_DirectHit

A DIRECT HIT -- the globe itself touching a living thing -- is fatal to
anything but a Vore or a Shambler, which take m5_balllightning_hit (Seb,
2026-09-06), and the globe DISINTEGRATES: frame 4 tells the client to blow
the shell apart over the alpha countdown (a quarter second), the shatter
burst fires, the thunder claps, and the blast and the ignition of the
ordinary discharge land on everything around. The owner counts once the
ball is armed.
=================
*/
void() M5Ball_ShatterThink =
{
	local float left;

	left = self.m5_ballend - time;
	if (left <= 0)
	{
		M5Ball_ClearTint ();
		M5Ball_RemoveHelpers ();
		remove (self);
		return;
	}
	self.alpha = left / 0.4;
	if (self.alpha < 0.05)
		self.alpha = 0.05;
	self.nextthink = time + 0.05;
};

void(entity victim) M5Ball_DirectHit =
{
	local float dmg, burst;
	local entity e;

	if (victim.classname == "monster_shambler" || victim.classname == "monster_shalrath")
		dmg = cvar("m5_balllightning_hit");
	else
		dmg = 10000;		// fatal, gibs and all
	T_Damage (victim, self, self.owner, dmg);
	M5_IgnitePlasma (victim, self.owner);

	// the blast on everyone else -- lethal to whoever stands beside the victim
	// (m5_balllightning_hitblast; Seb: "should kill adjacent enemies") -- and
	// the fire
	burst = cvar("m5_balllightning_hitblast");
	if (burst > 0)
	{
		T_RadiusDamage (self, self.owner, burst, victim);
		e = findradius (self.origin, burst + 40);
		while (e)
		{
			if (e.takedamage && e.health > 0 && e != self.owner && e != victim)
				M5_IgnitePlasma (e, self.owner);
			e = e.chain;
		}
	}
	if (m5_ball_shatterfx > 0)
		pointparticles (m5_ball_shatterfx, self.origin, '0 0 0', 1);
	sound (self, CHAN_AUTO, "ambience/thunder1.wav", 1, ATTN_NORM);
	sound (self, CHAN_WEAPON, "weapons/r_exp3.wav", 0.7, ATTN_NORM);

	// the disintegration: it stops dead, and the client blows the shell apart
	self.velocity = '0 0 0';
	self.movetype = MOVETYPE_NONE;
	self.solid = SOLID_NOT;
	self.frame = 4;
	self.alpha = 1;
	self.m5_ballend = time + 0.4;
	self.think = M5Ball_ShatterThink;
	self.nextthink = time + 0.05;
	if (!m5_ball_reported_hit)
	{
		m5_ball_reported_hit = TRUE;
		dprint ("M5 ball: direct hit\n");
	}
};

/*
=================
M5Ball_FizzleThink / M5Ball_Touch

A WALL does not detonate the ball (Seb, 2026-09-06: "no shockwave or explosion
when they hit a wall, just brightly fizzling out with lightning crackle"). It
stops where it struck and dies over m5_balllightning_fizzle seconds: frame 1
tells the client to strobe the knot and the light, alpha carries the countdown
(1 -> 0), the arcs stop, and the crackle is the lightning hit sound. No burst,
no radius damage. SOLID_TRIGGER: `other` is the world or a bsp mover when our
own move hit it (SV_Impact), or a monster who walked into us (SV_TouchAreaGrid),
who is ignored -- the arcs do the work on flesh.
=================
*/
// One arc to earth: a surface within 120 units along a random, downward-
// leaning direction (or straight down), a bright beam into it, sparks and a
// scorch at the spot, and the crackle. The helper owns the beam so the ball's
// own last arc (thrown at the touch) is not replaced.
void(entity own) M5Ball_EarthArc =
{
	local vector dir;

	dir = normalize ('1 0 0' * crandom() + '0 1 0' * crandom() + '0 0 -0.8' - '0 0 1' * random() * 0.6);
	traceline (self.origin, self.origin + dir * 120, TRUE, self);
	if (trace_fraction == 1)
		traceline (self.origin, self.origin + '0 0 -120', TRUE, self);
	if (trace_fraction == 1)
		return;
	M5Ball_Arc (own, self.origin, trace_endpos, TRUE);
	// BEAUTY A6: the surface normal rides the base velocity (unit length, so the
	// spark layers barely notice it); the scorch-glow layer lies on it
	if (m5_ball_fizzlefx > 0)
		pointparticles (m5_ball_fizzlefx, trace_endpos, trace_plane_normal, 1);
	sound (self, CHAN_AUTO, "weapons/lhit.wav", 0.9, ATTN_NORM);
};

void() M5Ball_FizzleThink =
{
	local float left, span;

	span = cvar("m5_balllightning_fizzle");
	if (span < 0.1)
		span = 0.1;
	left = self.m5_ballend - time;
	if (left <= 0)
	{
		M5Ball_ClearTint ();
		M5Ball_RemoveHelpers ();
		remove (self);
		return;
	}
	// SEVERAL ARCS TO EARTH (Seb's eleventh look): one every sixth of a second
	// through the collapse, alternating owners so two can show at once
	if (time >= self.m5_armtime)
	{
		self.m5_armtime = time + 0.17;
		if (self.m5_arc && self.m5_launchtime < 0)
			M5Ball_EarthArc (self.m5_arc);
		else
			M5Ball_EarthArc (self);
		self.m5_launchtime = 0 - self.m5_launchtime;	// flip the owner for the next one
	}
	self.alpha = left / span;
	if (self.alpha < 0.05)
		self.alpha = 0.05;
	self.nextthink = time + 0.05;
};

void() M5Ball_Touch =
{
	local float span;
	local vector v;

	// a living thing walking into the globe is a direct hit (the think catches
	// the globe flying into one); once armed, the owner too
	if (other.takedamage && other.health > 0 && (other != self.owner || time >= self.m5_armtime))
	{
		if (self.frame == 1 || self.frame == 4)
			return;
		M5Ball_DirectHit (other);
		return;
	}
	if (other.solid != SOLID_BSP)
		return;
	if (self.frame == 1 || self.frame == 4)
		return;				// already going
	// back off the surface by the globe's radius less the box, so the drawn
	// sphere is not buried in the wall while it earths
	setorigin (self, self.origin - self.m5_heading * 12);
	span = cvar("m5_balllightning_fizzle");
	if (span < 0.1)
		span = 0.1;
	// EARTHING (Seb's tenth look: "it needs to expend its energy somehow, like
	// a final arc to earth and a pop"): find the surface along the heading and
	// throw one bright arc into it, with the pop, the spray of sparks and the
	// scorch at the spot; then the shell collapses into it (frame 1, the alpha
	// counting down) and it is gone. No blast damage.
	traceline (self.origin, self.origin + self.m5_heading * 80, TRUE, self);
	if (trace_fraction == 1)
		traceline (self.origin, self.origin + '0 0 -80', TRUE, self);	// a floor, then
	v = trace_endpos;
	M5Ball_Arc (self, self.origin, v, TRUE);
	if (m5_ball_fizzlefx > 0)
		pointparticles (m5_ball_fizzlefx, v, trace_plane_normal, 1);	// BEAUTY A6: the normal for the scorch glow
	sound (self, CHAN_WEAPON, "weapons/r_exp3.wav", 0.5, ATTN_NORM);
	sound (self, CHAN_AUTO, "weapons/lhit.wav", 1, ATTN_NORM);
	self.velocity = '0 0 0';
	self.movetype = MOVETYPE_NONE;
	self.frame = 1;
	self.alpha = 1;
	self.m5_ballend = time + span;
	self.m5_armtime = time + 0.17;		// the next arc to earth (M5Ball_FizzleThink)
	self.m5_launchtime = 0 - 1;			// its owner flips between the ball and the helper
	// FIRST EARTHING PER MAP (developer 1), with how far the ball actually got.
	// This is the number Seb's doorway complaint is about: a ball that fouls on
	// an arch reads a few dozen units, one that flies reads hundreds. Asserted
	// by tests/smoke.sh.
	if (!m5_ball_reported_earth)
	{
		m5_ball_reported_earth = 1;
		dprint ("M5 ball: earthed ");
		dprint (ftos (vlen (self.origin - self.owner.origin)));
		dprint (" units from the shooter\n");
	}
	self.think = M5Ball_FizzleThink;
	self.nextthink = time + 0.05;
};

/*
=================
M5Ball_Drift

The flight (Seb's eighth look: "wobble very slowly but follow more or less
precisely a straight flight path"): the velocity is rebuilt every tick from
the HEADING at the launch speed, plus a slow sinusoidal sway sideways and up
of m5_balllightning_wobble units per second -- a drift, never a swerve. The
heading itself is bent only by M5Ball_Think's slight gravitational lean
toward nearby prey.
=================
*/
void() M5Ball_Drift =
{
	local float speed, w, t;

	speed = cvar("m5_balllightning_speed");
	w = cvar("m5_balllightning_wobble");
	t = time + self.m5_phase;
	makevectors (vectoangles (self.m5_heading));
	self.velocity = self.m5_heading * speed
		+ v_right * (sin (t * 1.3) * w)
		+ v_up * (cos (t * 0.9) * w * 0.6);
};

/*
=================
M5Ball_Lean

THE LEAN: gravitational, not conscious, and SLIGHT (Seb, 2026-09-10: "no
homing - it's a dumb ball of plasma, but it may gravitate gently toward
massive objects or monsters"). The heading bends toward `toward` by
m5_balllightning_pull degrees a second at point blank, falling off with the
square of the distance across the reach, and scaled by the thing's MASS --
its width over a soldier's, so a shambler pulls twice what a grunt does. With
nothing in reach it looks a little further (1.5 reaches) for the nearest
living thing in sight and leans at half strength.
=================
*/
void(entity toward, float radius) M5Ball_Lean =
{
	local entity e;
	local float pull, d, bestd, mass;

	pull = cvar("m5_balllightning_pull");
	if (pull <= 0)
		return;
	if (toward == world)
	{
		bestd = radius * 1.5 + 1;
		e = findradius (self.origin, radius * 1.5);
		while (e)
		{
			if (e.takedamage && e.health > 0 && e.classname != "m5ball" && e.classname != "m5arc" && e != self.owner)
			{
				d = vlen (e.origin - self.origin);
				if (d < bestd)
				{
					traceline (self.origin, e.origin, FALSE, e);
					if (trace_fraction == 1)
					{
						toward = e;
						bestd = d;
					}
				}
			}
			e = e.chain;
		}
		if (toward == world)
			return;
		pull = pull * 0.5;
		radius = radius * 1.5;
	}
	d = 1 - vlen (toward.origin - self.origin) / radius;
	if (d < 0)
		d = 0;
	mass = toward.size_x / 32;		// a soldier is 32 wide, a shambler 64
	if (mass < 0.6)
		mass = 0.6;
	if (mass > 2)
		mass = 2;
	d = pull * mass * d * d * 0.1 * 0.01745;		// degrees this tick -> radians, small-angle
	self.m5_heading = normalize (self.m5_heading + normalize (toward.origin - self.origin) * d);
};

/*
=================
M5Ball_ChainFrom

The next link of a chained arc: the nearest living thing within `reach` of
`from` that `from` can see and that this tick has not already zapped. The
victim is the pass entity, so the trace can land on the candidate.
=================
*/
entity(entity from, float reach) M5Ball_ChainFrom =
{
	local entity e, best;
	local float d, bestd;

	best = world;
	bestd = reach + 1;
	e = findradius (from.origin, reach);
	while (e)
	{
		if (e != from && e.takedamage && e.health > 0 && e.m5_zaptick != time
		    && e.classname != "m5ball" && e.classname != "m5arc"
		    && (e != self.owner || time >= self.m5_armtime))
		{
			d = vlen (e.origin - from.origin);
			if (d < bestd)
			{
				traceline (from.origin, e.origin, FALSE, from);
				if (trace_fraction == 1 || trace_ent == e)
				{
					best = e;
					bestd = d;
				}
			}
		}
		e = e.chain;
	}
	return best;
};

/*
=================
M5Ball_Think

Every 0.1 s: die if due; zap and ignite every living thing within
m5_balllightning_radius that a traceline from the ball reaches, the damage
falling with distance; draw arcs to the nearest m5_balllightning_arcs of them
(the ball owns the first beam, the helper chain the rest) and fork each landed
arc on from its victim; lean the heading gently toward the nearest.
=================
*/
void(entity own, vector from, vector to, float connected) M5Ball_Arc =
{
	WriteByte (MSG_BROADCAST, SVC_TEMPENTITY);
	// TE_LIGHTNING1 vs 2 is the bolt's own connect signal, read client-side as
	// the beam's model (r_lightning.c widens its scope to EF_M5BALL owners)
	if (connected)
		WriteByte (MSG_BROADCAST, TE_LIGHTNING1);
	else
		WriteByte (MSG_BROADCAST, TE_LIGHTNING2);
	WriteEntity (MSG_BROADCAST, own);
	WriteCoord (MSG_BROADCAST, from_x);
	WriteCoord (MSG_BROADCAST, from_y);
	WriteCoord (MSG_BROADCAST, from_z);
	WriteCoord (MSG_BROADCAST, to_x);
	WriteCoord (MSG_BROADCAST, to_y);
	WriteCoord (MSG_BROADCAST, to_z);
};

// One zap. The owner is damaged DIRECTLY: LightningDamage's tracelines run
// with the ball as pass entity and so skip the ball's owner by engine rule --
// the second half of why an armed ball never hurt the player who fired it.
float(entity targ, vector at, float dmg) M5Ball_Zap =
{
	if (targ == self.owner)
	{
		T_Damage (targ, self, self.owner, dmg);
		M5_IgnitePlasma (targ, self.owner);
		return TRUE;
	}
	return LightningDamage (self.origin, at, self.owner, dmg);
};

// SOUND PRESSURE (Seb, 2026-09-06): as a ball comes within its reach of its
// owner its drone is re-struck a step higher in pitch (change-only, so it is
// not a per-tick stream). The screen half is the renderer's (see below).
void() M5Ball_Pressure =
{
	local entity p;
	local float d, radius, a, pitch;

	p = self.owner;
	if (!cvar("m5_balllightning_pressure") || p.classname != "player" || p.health <= 0)
		return;
	radius = cvar("m5_balllightning_radius");
	d = vlen (p.origin - self.origin);
	a = 1 - d / (radius * 1.6);
	if (a < 0)
		a = 0;
	// (the screen's violet EDGE cast is drawn client-side by the renderer from
	// the nearest ball's distance -- r_lightningbeam_m5_ballpressure -- not
	// stuffed from here: a v_cshift is a whole-screen wash, and Seb asked for
	// an edge)
	// the drone, a step higher for every quarter of the way in
	pitch = 100 + floor ((1 - d / (radius * 1.6)) * 4) * 18;
	if (pitch < 100)
		pitch = 100;
	if (pitch != self.m5_dronepitch)
	{
		self.m5_dronepitch = pitch;
		sound7 (self, CHAN_VOICE, "ambience/drone6.wav", 1, ATTN_NORM, pitch, 0);
	}
};

void() M5Ball_Think =
{
	local entity e, best, own, pick, hop, next;
	local float bestd, pickd, d, radius, damage, hit, narcs, chainr, drawn, hops;
	local vector v, v2;

	self.nextthink = time + 0.1;

	// GROWTH: the ball leaves the gun a third of its size and reaches full size
	// over its first m5_balllightning_grow seconds (~a few feet); the client
	// scales the shell by alpha in the idle and zapping states
	d = cvar("m5_balllightning_grow");
	if (d < 0.05)
		d = 0.05;
	d = (time - self.m5_launchtime) / d;
	if (d > 1)
		d = 1;
	self.alpha = 0.22 + 0.78 * d;	// a quarter of its size at the muzzle (Seb, 2026-09-10: "start smaller near us")

	// CONTENT_WATER is -3 and slime, lava and sky sit below it
	if (time >= self.m5_ballend || pointcontents(self.origin) <= CONTENT_WATER)
	{
		M5Ball_Discharge ();
		return;
	}

	// (the arc helpers are placed where each arc roots, below -- on the ball
	// for an arc from the ball, on the victim for a chained fork)

	M5Ball_Pressure ();

	radius = cvar("m5_balllightning_radius");
	damage = cvar("m5_balllightning_damage");

	// A DIRECT HIT: the globe's own body overlapping a living thing's box.
	// The globe is r_lightningbeam_m5_ballsize wide client-side (22); the
	// server's box is 18 (setsize below), so touching means within a few units
	e = findradius (self.origin, 60);
	while (e)
	{
		if (e.takedamage && e.health > 0 && e.classname != "m5ball" && e.classname != "m5arc"
		    && (e != self.owner || time >= self.m5_armtime))
		{
			// nearest point of the box to the centre
			v = self.origin;
			if (v_x < e.absmin_x) v_x = e.absmin_x; else if (v_x > e.absmax_x) v_x = e.absmax_x;
			if (v_y < e.absmin_y) v_y = e.absmin_y; else if (v_y > e.absmax_y) v_y = e.absmax_y;
			if (v_z < e.absmin_z) v_z = e.absmin_z; else if (v_z > e.absmax_z) v_z = e.absmax_z;
			if (vlen (v - self.origin) < 20)
			{
				M5Ball_DirectHit (e);
				return;
			}
		}
		e = e.chain;
	}

	// EVERYTHING IN REACH (Seb, 2026-09-10: "too likely to miss nearby enemies as
	// it sails past them. it should hit & ignite anything nearby even if it
	// doesn't kill them"). Pass one MARKS every living thing the ball can see
	// within its reach (no damage yet: a mark is cheap and T_Damage is not);
	// pass two zaps and ignites every marked thing, the damage falling with
	// distance; pass three draws the arcs -- the ball's own beam to the
	// nearest, the helpers' to the next nearest, and each landed arc CHAINS on
	// from its victim (m5_balllightning_chain). id's T_RadiusDamage damages
	// inside a findradius walk the same way, so the chain survives a kill.
	narcs = cvar("m5_balllightning_arcs");
	if (narcs < 1)
		narcs = 1;
	if (narcs > 6)
		narcs = 6;
	chainr = cvar("m5_balllightning_chain");

	best = world;
	bestd = radius + 1;
	e = findradius (self.origin, radius);
	while (e)
	{
		// the owner is fair game once the ball is ARMED (m5_balllightning_arm):
		// a standing shot is always clear, a fool who chases it is not
		if (e.takedamage && e.health > 0 && e.classname != "m5ball" && e.classname != "m5arc"
		    && (e != self.owner || time >= self.m5_armtime))
		{
			// in sight from the ball, monsters included, so an arc never
			// reaches through a wall or another monster. The TARGET is the
			// pass entity: a trace passed the BALL skips the ball's OWNER by
			// engine rule (SV_Move ignores the passedict's owner), which is why
			// an armed ball could never see the player who fired it
			traceline (self.origin, e.origin, FALSE, e);
			if (trace_fraction == 1)
			{
				d = vlen (e.origin - self.origin);
				e.m5_zaptick = time;
				e.m5_zapdist = d;
				e.m5_zaphit = FALSE;
				if (d < bestd)
				{
					best = e;
					bestd = d;
				}
			}
		}
		e = e.chain;
	}

	if (best == world)
	{
		self.frame = 0;		// idle: the client draws the dancing many
		M5Ball_Lean (world, radius);
		M5Ball_Drift ();
		return;
	}
	self.frame = 2;			// zapping: a brighter core and one or two thick filaments

	// pass two: the zaps. LightningDamage's tracelines ignore self (the ball)
	// and stop on the target; its three-line fan can graze a neighbour,
	// exactly as the bolt's does. Target origins are copied FIRST: the calls
	// clobber trace_endpos and nothing below may read it. Damage grows with
	// PROXIMITY, up to double at point blank: a direct or near hit in one
	// room is an ogre kill (Seb, 2026-09-06), a distant graze a warning --
	// and whatever it zaps is set alight (plasma: whatever m5_burn says).
	e = findradius (self.origin, radius);
	while (e)
	{
		if (e.m5_zaptick == time && e.health > 0)
		{
			v = e.origin;
			d = e.m5_zapdist;
			e.m5_zaphit = M5Ball_Zap (e, v, damage * (2 - d / radius));
			M5_IgnitePlasma (e, self.owner);
		}
		e = e.chain;
	}

	// pass three: the arcs, nearest first, one owner each (the ball, then its
	// helpers down the chain), and a landed arc FORKS from its victim to the
	// next living thing that victim can see within m5_balllightning_chain, up
	// to two hops -- so a pack dies together. A fork is damaged from the
	// victim's position and DRAWN from there: the helper that owns the beam
	// stands on the victim, because the client roots every EF_M5BALL-owned
	// beam on its owner's origin.
	own = self;
	drawn = 0;
	while (drawn < narcs && own != world)
	{
		pick = world;
		pickd = radius + 1;
		e = findradius (self.origin, radius);
		while (e)
		{
			if (e.m5_zaptick == time && e.m5_arctick != time && e.m5_zapdist < pickd)
			{
				pick = e;
				pickd = e.m5_zapdist;
			}
			e = e.chain;
		}
		if (pick == world)
			break;
		pick.m5_arctick = time;
		v = pick.origin;
		if (own != self)
			setorigin (own, self.origin);
		M5Ball_Arc (own, self.origin, v, pick.m5_zaphit);
		drawn = drawn + 1;
		own = own.m5_arc;

		hop = pick;
		hops = 0;
		while (chainr > 0 && hops < 2 && own != world && drawn < narcs)
		{
			next = M5Ball_ChainFrom (hop, chainr);
			if (next == world)
				break;
			next.m5_zaptick = time;
			next.m5_arctick = time;
			next.m5_zapdist = radius;	// never a primary this tick
			v = hop.origin;
			v2 = next.origin;
			hit = LightningDamage (v, v2, self.owner, damage * 0.7);
			M5_IgnitePlasma (next, self.owner);
			if (!m5_ball_reported_chain)
			{
				m5_ball_reported_chain = TRUE;
				dprint ("M5 ball: chain\n");	// first fork per map (developer 1); not asserted -- it needs two monsters close together
			}
			setorigin (own, v);
			M5Ball_Arc (own, v, v2, hit);
			drawn = drawn + 1;
			own = own.m5_arc;
			hop = next;
			hops = hops + 1;
		}
	}

	if (self.attack_finished < time)
	{
		sound (self, CHAN_AUTO, "weapons/lhit.wav", 1, ATTN_NORM);
		self.attack_finished = time + 0.15;
	}
	if (!m5_ball_reported_arc)
	{
		m5_ball_reported_arc = TRUE;
		dprint ("M5 ball: arc\n");	// asserted by tests/smoke.sh; first arc per map
	}

	M5Ball_Lean (best, radius);
	M5Ball_Drift ();
};

/*
=================
M5Ball_Launch

The ball leaves the gun. Called from W_FireBall directly at charge 0, or from
the charge helper's think m5_balllightning_charge seconds after the press. Runs
with self = the PLAYER (the helper swaps self and swaps it back).
=================
*/
void() M5Ball_Launch =
{
	local entity ball, arc, prev;
	local float life, n;
	local vector org, dir;

	makevectors (self.v_angle);
	// FROM THE BARREL END, where the charge globe built (Seb's fifth look: it
	// formed at the right-hand node then jumped in front of the eyes), and
	// steered onto the aim line: the aim point is where the eye's line lands,
	// so a ball launched from beside and below the eye still goes where the
	// crosshair says
	org = self.origin + '0 0 12' + v_forward*50;
	traceline (self.origin + '0 0 22', self.origin + '0 0 22' + v_forward*2048, TRUE, self);
	dir = normalize (trace_endpos - org);
	// but never backwards or wildly off the aim (a wall a foot from the muzzle)
	if (dir * v_forward < 0.5)
		dir = v_forward;
	ball = spawn ();
	ball.owner = self;
	ball.classname = "m5ball";
	ball.movetype = MOVETYPE_FLY;
	ball.solid = SOLID_TRIGGER;
	ball.velocity = dir * cvar("m5_balllightning_speed");
	ball.m5_heading = dir;
	ball.m5_phase = random() * 6.2832;
	ball.angles = vectoangles (ball.velocity);
	setmodel (ball, "");	// not a model: the client draws the knot at EF_M5BALL
	// A POINT, and the size is the whole point (Seb, 2026-09-19: "still fouls /
	// grounds when firing on the threshold of doorways / arches -- needs to
	// clear anything we can fit through").
	//
	// THE TWELFTH LOOK'S 20-UNIT BOX COULD NEVER HAVE WORKED, and the reason is
	// Q1BSP rather than the number: a brush map carries THREE clip hulls --
	// point, 32x32x56 (player) and 64x64x88 -- and Mod_Q1BSP_TraceBox picks by
	// WIDTH, rounding anything at or above mod_q1bsp_zero_hullsize_cutoff (3)
	// UP to the player hull. So a 20-wide ball collided as a 32x32x56 player
	// box, and worse, the hull is aligned by (boxmins - hull->clip_mins), which
	// lifts the traced point 14 units: the invisible box stood 46 units above
	// the ball's centre and caught every arch lintel and door head the player
	// himself ducks under by standing on the floor. Going from 20 to 2 crosses
	// the cutoff and puts the ball on the POINT hull -- what a rocket has
	// always used -- so it now passes anything with a gap at all.
	//
	// Nothing else depends on the box: the DIRECT HIT is M5Ball_Think's own
	// findradius(60) plus an explicit 20-unit box-distance test, not the touch,
	// and the touch's living-thing branch is a bonus the think covers anyway.
	setsize (ball, '-1 -1 -1', '1 1 1');
	setorigin (ball, org);
	ball.effects = EF_M5BALL;
	// the light. Radius 300 (a light contributes nothing past its radius, and
	// the bolt learned that a big hall wants ~300); the colour's max channel is
	// the BRIGHTNESS, the client supplies the hue and the flicker.
	ball.color = '1 1 1' * cvar("m5_balllightning_light");
	ball.light_lev = 300;
	ball.pflags = PFLAGS_FULLDYNAMIC | PFLAGS_NOSHADOW;
	if (m5_ball_glowfx > 0)
		ball.traileffectnum = m5_ball_glowfx;
	ball.touch = M5Ball_Touch;
	// the hum: a deep stock drone under the fixture buzz, both looped, both
	// stopped by the remove (stock first -- Seb's answer 6)
	sound (ball, CHAN_VOICE, "ambience/drone6.wav", 1, ATTN_NORM);
	sound (ball, CHAN_BODY, "ambience/buzz1.wav", 0.5, ATTN_NORM);
	ball.m5_armtime = time + cvar("m5_balllightning_arm");
	ball.m5_launchtime = time;
	sound7 (self, CHAN_WEAPON, "weapons/lstart.wav", 1, ATTN_NORM, 190, 0);	// the release: the whine's top note
	ball.m5_dronepitch = 100;
	ball.alpha = 0.22;

	life = cvar("m5_balllightning_life");
	if (life < 0.2)
		life = 0.2;
	ball.m5_ballend = time + life;
	ball.think = M5Ball_Think;
	ball.nextthink = time + 0.1;

	// the other arcs' owners: one beam per owning entity, so each arc past
	// the first needs one of its own -- a CHAIN of m5_balllightning_arcs - 1
	// helpers (ball.m5_arc -> helper.m5_arc -> ...). Tagged so the client roots
	// the beam on it, NODRAW so it draws no knot and hangs no light; the think
	// stands each one where its arc roots (the ball, or a chained victim).
	n = cvar("m5_balllightning_arcs") - 1;
	if (n > 5)
		n = 5;
	prev = ball;
	while (n > 0)
	{
		arc = spawn ();
		arc.owner = self;
		arc.classname = "m5arc";
		arc.movetype = MOVETYPE_NONE;
		arc.solid = SOLID_NOT;
		arc.effects = EF_M5BALL | EF_NODRAW;
		setorigin (arc, ball.origin);
		prev.m5_arc = arc;
		prev = arc;
		n = n - 1;
	}

	if (!m5_ball_reported_launch)
	{
		m5_ball_reported_launch = TRUE;
		dprint ("M5 ball: launched\n");	// asserted by tests/smoke.sh; first ball per map
	}
};

/*
=================
M5Ball_ChargeThink

The charge helper (classname m5charge, owner = the player) fires the launch
when its time comes, with self swapped to the player so M5Ball_Launch reads
the aim and origin of THAT moment -- the BFG 9000 shape. A dead owner, or one
who dropped the ball gun, keeps the cells he already spent and gets no ball.
=================
*/
void() M5Ball_ChargeThink =
{
	local entity player, helper;
	local float charge, done;

	helper = self;
	player = self.owner;
	charge = cvar("m5_balllightning_charge");
	if (charge < 0.05)
		charge = 0.05;
	done = (time - self.m5_ballend) / charge;	// m5_ballend holds the PRESS time here
	if (player.health <= 0 || player.weapon != IT_M5BALL)
	{
		remove (self);
		return;
	}
	if (done < 1)
	{
		// still building: ride the muzzle, grow with the charge (frame 3, alpha
		// is the progress -- the client draws the sphere at that fraction of
		// its size and the light at that fraction of its brightness)
		self = player;
		makevectors (self.v_angle);
		setorigin (helper, self.origin + '0 0 12' + v_forward*50);
		self = helper;
		self.alpha = 0.05 + 0.95 * done;
		// THE RISING WHINE (Seb's fifth look): the lightning start sound re-struck
		// on the weapon channel every 80 ms at a climbing pitch -- each strike
		// replaces the last, so the pitch steps upward through the wind-up
		if (time >= self.m5_armtime)
		{
			self.m5_armtime = time + 0.08;
			sound7 (player, CHAN_WEAPON, "weapons/lstart.wav", 0.8, ATTN_NORM, 55 + 120 * done, 0);
		}
		self.nextthink = time + 0.03;
		return;
	}
	self = player;
	M5Ball_Launch ();
	self = helper;
	remove (self);
};

/*
=================
W_FireBall

Called from W_Attack with self.weapon == IT_M5BALL. A fixed cell cost at the
press (m5_balllightning_cost) rather than a running one: a ball keeps flying
after its owner switches weapon or dies, and a price paid up front is one the
HUD can show. Firing submerged discharges the whole cell store into the water,
the thunderbolt's own rule -- it is the same energy. Then the CHARGE: the ball
leaves m5_balllightning_charge seconds after the press (a helper edict's think),
so the shot has the BFG's wind-up.
=================
*/
void() W_FireBall =
{
	local entity charger;
	local float cost, cells, charge;

	cost = cvar("m5_balllightning_cost");
	if (cost < 1)
		cost = 1;
	if (self.ammo_cells < cost)
	{
		self.weapon = W_BestWeapon ();
		W_SetCurrentAmmo ();
		return;
	}

	// explode if under water (W_FireLightning's rule, verbatim)
	if (self.waterlevel > 1)
	{
		cells = self.ammo_cells;
		self.ammo_cells = 0;
		W_SetCurrentAmmo ();
		T_RadiusDamage (self, self, 35*cells, world);
		return;
	}

	M5Ball_Effects ();

	self.currentammo = self.ammo_cells = self.ammo_cells - cost;
	sound7 (self, CHAN_WEAPON, "weapons/lstart.wav", 0.8, ATTN_NORM, 55, 0);	// the whine starts low; the charge think raises it
	M5Feel_Kick (IT_M5BALL, -3);

	charge = cvar("m5_balllightning_charge");
	if (charge <= 0)
	{
		M5Ball_Launch ();
		return;
	}
	charger = spawn ();
	charger.owner = self;
	charger.classname = "m5charge";
	charger.movetype = MOVETYPE_NONE;
	charger.solid = SOLID_NOT;
	// the charge made visible: an EF_M5BALL entity in the CHARGE state at the
	// muzzle, sphere and light growing with the progress (Seb's answer 12)
	charger.effects = EF_M5BALL;
	charger.frame = 3;
	charger.alpha = 0.05;
	charger.color = '1 1 1' * cvar("m5_balllightning_light");
	charger.light_lev = 220;
	charger.pflags = PFLAGS_FULLDYNAMIC | PFLAGS_NOSHADOW;
	makevectors (self.v_angle);
	// the CENTRAL node of the plasma gun as drawn (Seb's fifth look: not the
	// right-hand one; sixth: six lower) -- out past the model, below the eye line
	setorigin (charger, self.origin + '0 0 12' + v_forward*50);
	charger.m5_ballend = time;			// the press time; the think measures progress from it
	charger.m5_armtime = time + 0.08;	// next whine strike
	charger.think = M5Ball_ChargeThink;
	charger.nextthink = time + 0.03;
};
