/*
==============================================================================

M5 FLAMETHROWER -- the tenth weapon (FLAMETHROWER.md)

Gated on the m5_flamer cvar (registered engine-side beside m5_balllightning):
  0  off. The weapon does not exist: impulse 203 says so and does nothing else,
     the cycle keys skip it, and no napalm can ever be spawned.
  1  on. Owning the thunderbolt grants the flamethrower beside it (IT_M5FLAME,
     item bit 23); impulse 203 selects it (key 0), and again for the bolt.

Seb's decisions (2026-10-03): its own weapon on key 0; CELLS, the bolt's and
the ball's store; the existing burn on monsters (spreading napalm is a later
cvar if ever); and the stream "a completely fresh napalm stream of fiery white
yellow death".

THE STREAM. Every tenth of a second the trigger is held, m5_flamer_globs small
globules leave the muzzle at staggered speeds with a little spread, under
m5_flamer_gravity, so the arc droops the way thrown liquid does. Each globule
is a POINT (setsize 0 0 -- the hull fact: anything wider than 3 units collides
as the 32x32x56 player hull, which would catch every lintel) and SOLID_BBOX on
MOVETYPE_TOSS, the rocket's arrangement: points never clip against points, so
the globules pass through each other, and the owner is skipped by the owner
rule, so they leave the gun cleanly. The fire you see is the m5flame_stream
trail (m5/effectinfo.txt) laid along each globule's flight: a white-yellow core
inside an orange body, with smoke.

ONE LIGHT FOR THE WHOLE STREAM, never one per globule. Every dynamic light
enters the RT light list and the kernels stage 256 at most (rt_metal_lightrank
ranks the cut), so forty lit globules on a horde map would crowd the walls'
own lights out. The light rides m5_flamer_lightdist units down the aim line,
pulled off any wall, and carries the roar; it dies a moment after the trigger
is let go, and the roar with it -- STOPPED by name (misc/null.wav on its
channel): removing an entity does NOT stop its sounds in DarkPlaces, and until
2026-10-05 the looping roar played on at full level for the rest of the map
after the first burst (measured in the mix, -simsound + SND_DUMPMIX).

TOUCH. On anything that takes damage: m5_flamer_damage, then the burn
(M5_IgniteAt forced -- napalm sets alight whatever m5_burn says, the ball's
plasma rule), and the globule is spent. On the world: a FIRE PATCH, pulled
back along the incoming flight (the impact flash's lesson -- a point exactly
on the surface is inside it half the time). On a door, a plat or the sky:
spent, no patch (a patch on a moving brush would hang in the air).

FIRE PATCHES. An invisible SOLID_TRIGGER box carrying a flickering warm light
(PFLAGS_FULLDYNAMIC -- not EF_FLAME, whose light draws a corona) and licks of
white-yellow flame (m5flame_patch, ten times a second), that scorches and
ignites anything that walks through it,
for m5_flamer_patchlife seconds. A globule landing beside a live patch
REFUELS it (up to twice its life from now) rather than lighting a second.
m5_flamer_patches is the cap, and the cap is the whole performance design:
each patch is a light, so the oldest goes when one more is wanted.

WATER. A globule in liquid fizzles in steam; a patch is never placed in one;
firing submerged only hisses (no fuel spent, nothing lit -- unlike the bolt,
napalm has no charge to dump). And anything burning that wades in is doused
(m5burn.qc, for every fire, not only this one's).

SYMMETRIC. A patch burns its owner as readily as a monster, the ball's rule.
Hose the floor at your feet and you will be standing in it.

Traps this file was written against (CLAUDE.md's hard-won facts):
  - every dprint is FIRST-EVENT per map (globals reset on map load) -- a
    ten-hertz print under developer 1 is the 92 GB console-ink incident.
  - particleeffectnum on a missing name returns 0 with a console warning;
    the handles are looked up once per map and skipped at 0.
  - precache is unconditional (world.qc): precache order is modelindex order.
  - findradius is only ever called at an IMPACT, never per tick per globule.

==============================================================================
*/

float m5_flame_reported_stream;	// first-event dprints, one each per map
float m5_flame_reported_ignite;
float m5_flame_reported_patch;
float m5_flame_reported_cap;
float m5_flame_reported_refuel;
float m5_flame_reported_steam;
float m5_flame_fxready;			// effectinfo handles looked up (once per map)
float m5_flame_streamfx;
.vector m5_flameaim;		// the nozzle: the aim its last frame emitted along
float m5_flame_splashfx;
float m5_flame_patchfx;
float m5_flame_steamfx;
float m5_flame_patchcount;		// live fire patches (the cap's count)

void() M5Flame_Effects =
{
	if (m5_flame_fxready)
		return;
	m5_flame_fxready = TRUE;
	m5_flame_streamfx = particleeffectnum ("m5flame_stream");
	m5_flame_splashfx = particleeffectnum ("m5flame_splash");
	m5_flame_patchfx = particleeffectnum ("m5flame_patch");
	m5_flame_steamfx = particleeffectnum ("m5flame_steam");
	dprint ("M5 flamer: effects stream ");	// asserted by tests/smoke.sh (all > 0)
	dprint (ftos(m5_flame_streamfx));
	dprint (" splash ");
	dprint (ftos(m5_flame_splashfx));
	dprint (" patch ");
	dprint (ftos(m5_flame_patchfx));
	dprint (" steam ");
	dprint (ftos(m5_flame_steamfx));
	dprint ("\n");
};

/*
=================
M5Flame_Sync

Called every W_WeaponFrame, the ball's shape: grants IT_M5FLAME to a player
who owns the thunderbolt while the cvar is on, withdraws it the moment the
cvar goes off (dropping to the best weapon if it was in hand).
=================
*/
void() M5Flame_Sync =
{
	if (cvar("m5_flamer"))
	{
		if ((self.items & IT_LIGHTNING) && !(self.items & IT_M5FLAME))
			self.items = self.items | IT_M5FLAME;
	}
	else if (self.items & IT_M5FLAME)
	{
		self.items = self.items - IT_M5FLAME;
		if (self.weapon == IT_M5FLAME)
		{
			self.weapon = W_BestWeapon ();
			W_SetCurrentAmmo ();
		}
	}
};

/*
=================
M5Flame_Select

impulse 203: the flamethrower if the thunderbolt is owned and there are cells;
pressed with it already in hand, back to the thunderbolt. Wording matches
W_ChangeWeapon's own refusals.
=================
*/
void() M5Flame_Select =
{
	if (!cvar("m5_flamer"))
	{
		sprint (self, "the flamethrower is off: m5_flamer 1\n");
		return;
	}
	if (self.weapon == IT_M5FLAME)
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
	if (self.ammo_cells < 1)
	{
		sprint (self, "not enough ammo.\n");
		return;
	}
	self.items = self.items | IT_M5FLAME;
	self.weapon = IT_M5FLAME;
	W_SetCurrentAmmo ();
};

// Every ignition by napalm goes through here, globule or patch, so the
// first-event line counts both (smoke asserts it).
void(entity targ, entity attacker) M5Flame_Ignite =
{
	M5_IgniteAt (targ, attacker, TRUE);
	if (!m5_flame_reported_ignite)
	{
		m5_flame_reported_ignite = TRUE;
		dprint ("M5 flamer: ignited\n");	// asserted by tests/smoke.sh
	}
};

void(vector org) M5Flame_Steam =
{
	if (m5_flame_steamfx > 0)
		pointparticles (m5_flame_steamfx, org, '0 0 30', 1);
	if (!m5_flame_reported_steam)
	{
		m5_flame_reported_steam = TRUE;
		dprint ("M5 flamer: fizzled in water\n");
	}
};

/*
=================
Fire patches
=================
*/
void() M5Flame_PatchDie =
{
	if (m5_flame_patchcount > 0)
		m5_flame_patchcount = m5_flame_patchcount - 1;
	remove (self);
};

void() M5Flame_PatchThink =
{
	local float b;

	if (time >= self.m5_flameend)
	{
		M5Flame_PatchDie ();
		return;
	}
	// the flames: white-yellow licks and embers ten times a second, and the
	// light flickering with them (the last second dims it out)
	if (m5_flame_patchfx > 0)
		pointparticles (m5_flame_patchfx, self.origin, '0 0 0', 1);
	b = cvar("m5_flamer_patchlight") * (0.75 + random() * 0.5);
	if (self.m5_flameend - time < 1)
		b = b * (self.m5_flameend - time);
	self.color = '1 0.62 0.28' * b;
	self.nextthink = time + 0.1;
};

// Anything that walks through: scorched every half second (per thing, so a
// crowd is each scorched once rather than one victim per frame) and set alight.
// The inflictor is the victim itself, the burn's own rule: the fire is ON them,
// so a kill is credited to whoever lit it without a phantom knockback.
void() M5Flame_PatchTouch =
{
	local entity patch;

	if (!other.takedamage || other.health <= 0)
		return;
	if (time < other.m5_flamehurt)
		return;
	other.m5_flamehurt = time + 0.5;
	patch = self;
	M5Flame_Ignite (other, patch.owner);
	self = other;
	T_Damage (other, other, patch.owner, cvar("m5_flamer_patchdamage"));
	self = patch;
};

void(vector org) M5Flame_Patch =
{
	local entity e, oldest, patch, oself;
	local float life, cap, pc;

	// never in a liquid, never inside the wall the pull-back failed to clear
	pc = pointcontents (org);
	if (pc != CONTENT_EMPTY)
	{
		if (pc <= CONTENT_WATER && pc != CONTENT_SKY)
			M5Flame_Steam (org);
		return;
	}
	life = cvar("m5_flamer_patchlife");
	if (life <= 0)
		return;

	// REFUEL a patch already burning here (the stream lands on the same spot
	// thirty times a second; stacking would hit the cap in a third of one)
	e = findradius (org, 40);
	while (e)
	{
		if (e.classname == "m5flamepatch")
		{
			e.m5_flameend = e.m5_flameend + 0.5;
			if (e.m5_flameend > time + life * 2)
				e.m5_flameend = time + life * 2;
			if (!m5_flame_reported_refuel)
			{
				m5_flame_reported_refuel = TRUE;
				dprint ("M5 flamer: patch refuelled\n");	// asserted by tests/smoke.sh
			}
			return;
		}
		e = e.chain;
	}

	// THE CAP: each patch is a dynamic light, so the oldest goes
	cap = cvar("m5_flamer_patches");
	if (cap < 1)
		return;
	while (m5_flame_patchcount >= cap)
	{
		oldest = world;
		e = find (world, classname, "m5flamepatch");
		while (e)
		{
			if (oldest == world || e.m5_flamestart < oldest.m5_flamestart)
				oldest = e;
			e = find (e, classname, "m5flamepatch");
		}
		if (oldest == world)
		{
			m5_flame_patchcount = 0;	// the count drifted (it cannot, but never loop on it)
			break;
		}
		oself = self;
		self = oldest;
		M5Flame_PatchDie ();
		self = oself;
		if (!m5_flame_reported_cap)
		{
			m5_flame_reported_cap = TRUE;
			dprint ("M5 flamer: patch cap ");	// asserted by tests/smoke.sh
			dprint (ftos(cap));
			dprint (" reached, oldest retired\n");
		}
	}

	patch = spawn ();
	patch.classname = "m5flamepatch";
	patch.owner = self.owner;
	patch.movetype = MOVETYPE_NONE;
	patch.solid = SOLID_TRIGGER;
	setmodel (patch, "");
	setsize (patch, '-14 -14 -6', '14 14 22');
	setorigin (patch, org);
	// NOT EF_FLAME: its light carries a CORONA, and with r_coronas on that
	// draws as a glowing egg sitting on the floor (measured on the first
	// stills). A PFLAGS_FULLDYNAMIC light has no corona unless asked, and its
	// colour is ours to flicker; the flames are m5flame_patch, fired by the think.
	patch.pflags = PFLAGS_FULLDYNAMIC | PFLAGS_NOSHADOW;
	patch.light_lev = 280;
	patch.color = '1 0.62 0.28' * cvar("m5_flamer_patchlight");
	patch.touch = M5Flame_PatchTouch;
	patch.think = M5Flame_PatchThink;
	patch.nextthink = time + 0.1;
	patch.m5_flamestart = time;
	patch.m5_flameend = time + life;
	m5_flame_patchcount = m5_flame_patchcount + 1;
	if (!m5_flame_reported_patch)
	{
		m5_flame_reported_patch = TRUE;
		dprint ("M5 flamer: fire patch lit\n");	// asserted by tests/smoke.sh
	}
};

/*
=================
The globules
=================
*/
void() M5Flame_GlobThink =
{
	local float pc;

	pc = pointcontents (self.origin);
	if (pc <= CONTENT_WATER && pc != CONTENT_SKY)
	{
		M5Flame_Steam (self.origin);
		remove (self);
		return;
	}
	if (time >= self.m5_flameend || pc == CONTENT_SKY)
	{
		remove (self);	// burnt out in the air
		return;
	}
	self.nextthink = time + 0.1;
};

void() M5Flame_GlobTouch =
{
	local vector back;

	if (other.classname == "m5flame" || other.classname == "m5flamepatch")
		return;
	if (pointcontents (self.origin) == CONTENT_SKY)
	{
		remove (self);
		return;
	}
	if (other.takedamage)
	{
		if (other.health > 0)
		{
			T_Damage (other, self, self.owner, cvar("m5_flamer_damage"));
			M5Flame_Ignite (other, self.owner);
		}
		remove (self);
		return;
	}
	// the impact splash, then a patch on the world (only): the touch runs
	// before the physics reflects the velocity, so self.velocity IS the impact
	back = normalize (self.velocity);
	if (m5_flame_splashfx > 0)
		pointparticles (m5_flame_splashfx, self.origin - back * 2, back * -80, 1);
	if (other == world)
		M5Flame_Patch (self.origin - back * 6);
	remove (self);
};

/*
=================
The stream's light

One per player. W_FireFlame keeps pushing its end out while the trigger is
held; the think follows the aim line at 20 Hz and removes it (and so the
roar it carries) a moment after the last tick.
=================
*/
void(entity p, float count, float window, entity nozzle) M5Flame_Emit;

// m5_flamer_sfx scales the ignition and the wind-down; a sound's volume is
// capped at 1
float(float base) M5Flame_SfxVol =
{
	local float v;
	v = base * cvar("m5_flamer_sfx");
	if (v > 1)
		v = 1;
	return v;
};

void() M5Flame_LightThink =
{
	local entity p;
	local float dist, rate, dt, n;
	local vector org;

	p = self.owner;
	if (time >= self.m5_flameend || p.health <= 0 || p.weapon != IT_M5FLAME || p.m5_flamelight != self)
	{
		// silence the roar first: a remove alone leaves a looping sound
		// playing for ever (see the header)
		if (self.cnt)
			sound7 (self, CHAN_VOICE, "misc/null.wav", 1, ATTN_NORM, 100, 0);
		if (p.m5_flamelight == self)
		{
			p.m5_flamelight = world;
			// the WIND-DOWN on the gun (m5_flamer_sfx): the light's remove
			// cuts the roar, and the valve shuts and the flame gutters out
			// on the weapon channel, where the next ignition will cut it
			// at a third of full scale it starts about level with the roar
			// it replaces (measured in the mix) and falls from there
			if (cvar("m5_flamer_sfx") > 0)
				sound7 (p, CHAN_WEAPON, "m5/flamer_winddown.wav", M5Flame_SfxVol (0.33), ATTN_NORM, 100, 0);
		}
		remove (self);
		return;
	}
	// the ROAR comes in under the whoosh's tail (m5_flamer_sfx): struck on
	// the light, so the remove ends it
	if (!self.cnt && time >= self.wait)
	{
		self.cnt = 1;
		sound7 (self, CHAN_VOICE, "ambience/fire1.wav", 1, ATTN_NORM, 135, 0);
	}
	// THE NOZZLE: napalm every server frame while the trigger is held, at
	// m5_flamer_globs per tenth of a second, from the aim of this moment
	if (p.button0 && p.waterlevel <= 1 && p.ammo_cells >= 1)
	{
		rate = cvar("m5_flamer_globs") * 10;
		dt = time - self.m5_flamelast;
		if (dt > 0.1)
			dt = 0.1;
		self.m5_flameacc = self.m5_flameacc + rate * dt;
		n = floor (self.m5_flameacc);
		self.m5_flameacc = self.m5_flameacc - n;
		if (n > 16)
			n = 16;
		M5Flame_Emit (p, n, dt, self);
	}
	self.m5_flamelast = time;

	makevectors (p.v_angle);
	dist = cvar("m5_flamer_lightdist");
	org = p.origin + '0 0 16';
	traceline (org, org + v_forward * dist, TRUE, p);
	setorigin (self, trace_endpos - v_forward * 16);
	// ONCE A SERVER FRAME. Not "time + 0.001": SV_RunThink re-runs a think
	// until its nextthink passes the end of the frame, so that ran the nozzle
	// about thirteen times a frame and took the game to 23 fps on the bed
	self.nextthink = time + frametime;
};

void() M5Flame_Light =
{
	local entity l;
	local float b;

	l = self.m5_flamelight;
	if (l == world)
	{
		l = spawn ();
		l.classname = "m5flamelight";
		l.owner = self;
		l.movetype = MOVETYPE_NONE;
		l.solid = SOLID_NOT;
		setmodel (l, "");
		setorigin (l, self.origin + '0 0 16');
		l.light_lev = 360;
		l.pflags = PFLAGS_FULLDYNAMIC | PFLAGS_NOSHADOW;
		l.think = M5Flame_LightThink;
		l.nextthink = time + frametime;
		l.m5_flamelast = time;
		l.m5_flameacc = 1;	// the first globule leaves on the first frame
		self.m5_flamelight = l;
		// the roar: the torches' own looping fire, struck on the light so the
		// remove ends it. m5_flamer_sfx 1 (2026-10-05, Seb: "needs an
		// ignition, then whoosh! sound and a wind down sound"): the trigger
		// strikes our own ignition (the spark, the catch, the whoosh --
		// generated by qc/make_flamer_sounds.py) and the roar waits 0.15 s to
		// come in under the whoosh's tail (the think starts it); release plays
		// the wind-down. 0 = the roar at once and the rocket whoosh, as before.
		if (cvar("m5_flamer_sfx") > 0)
		{
			l.wait = time + 0.15;
			// a flare-up over the roar, not a blast: about three times its level
			sound7 (self, CHAN_WEAPON, "m5/flamer_ignite.wav", M5Flame_SfxVol (0.6), ATTN_NORM, 100, 0);
		}
		else
		{
			l.cnt = 1;
			sound7 (l, CHAN_VOICE, "ambience/fire1.wav", 1, ATTN_NORM, 135, 0);
			sound7 (self, CHAN_WEAPON, "weapons/rocket1i.wav", 0.8, ATTN_NORM, 70, 0);
		}
	}
	// white-yellow, flickering only a little: a ten-hertz jump of +-15 percent
	// read as the stream pulsing in bursts (Seb, 2026-10-04)
	b = cvar("m5_flamer_light") * (0.96 + random() * 0.08);
	l.color = '1 0.82 0.45' * b;
	l.m5_flameend = time + 0.15;
};

/*
=================
M5Flame_Emit

`count` globules from `p`'s muzzle along p's aim AT THIS MOMENT, back-dated
evenly across the last `window` seconds as if fired continuously: each is
placed where it would be by now, gravity included, its velocity bent to
match, clamped short of a wall. Called every SERVER FRAME by the nozzle
(M5Flame_LightThink) -- a HOSEPIPE (Seb, 2026-10-04: "still pulses instead of
a stream. it should be like a hosepipe"): the first cut spawned a tick's
globules every tenth of a second, so new fire appeared in steps along the
first stretch of the arc, and a fast turn left a jointed chain of ten-hertz
segments where a hose draws a curve.
=================
*/
void(entity p, float count, float window, entity nozzle) M5Flame_Emit =
{
	local entity g;
	local float speed, spread, life, i, age, grav, f, trail;
	local vector org, dir, aim, aim0, vel, pos;

	if (count < 1)
		return;
	makevectors (p.v_angle);
	// FROM THE MUZZLE as drawn: 30 units out and 14 below the eye (the eye is
	// 22 above the origin), the low centre of the frame, so the stream reads
	// as a hose rising to the crosshair rather than a disc stacked on it --
	// and never inside the eye's own near plane
	org = p.origin + '0 0 22' + v_forward * 30 - v_up * 14;
	// steered onto the aim: the crosshair's point, from a muzzle below the eye
	traceline (p.origin + '0 0 22', p.origin + '0 0 22' + v_forward * 2048, TRUE, p);
	aim = normalize (trace_endpos - org);
	if (aim * v_forward < 0.5)
		aim = v_forward;
	// THE AIM OF THE MOMENT EACH GLOBULE LEFT, not the aim of this frame: a
	// globule back-dated by `age` is sent along the aim interpolated between
	// the last frame's and this one's, so a sweep lays its napalm (and so its
	// damage and its patches) along a CURVE. Along one frame's aim it was a
	// fan of spokes, one per server frame -- which is exactly what the drawn
	// stream read as in a turn (a string of beads, 2026-10-04) and why twice
	// the rate gave the same beads: two globules on every spoke.
	aim0 = nozzle.m5_flameaim;
	if (aim0 == '0 0 0' || aim0 * aim < 0.5)
		aim0 = aim;
	nozzle.m5_flameaim = aim;

	speed = cvar("m5_flamer_speed");
	spread = cvar("m5_flamer_spread");
	life = cvar("m5_flamer_life");
	if (life < 0.1)
		life = 0.1;
	trail = cvar("m5_flamer_servertrail");
	// (the stream as SEEN is the client's hose, from the gun model's own
	// nozzle every rendered frame: cl_particles.c, m5_flamer_hose. Anything
	// sent from here lands a frame or two behind the view, which in a fast
	// turn put the fire at the gun 13-26 degrees off to the side, and one aim
	// per server frame fanned it into spokes.)
	i = 0;
	while (i < count)
	{
		age = window * (i + random()) / count;
		f = 1;
		if (window > 0)
			f = 1 - age / window;
		dir = normalize (aim0 + (aim - aim0) * f) + v_right * crandom() * spread + v_up * crandom() * spread;
		g = spawn ();
		g.owner = p;
		g.classname = "m5flame";
		g.movetype = MOVETYPE_TOSS;
		g.solid = SOLID_BBOX;
		g.gravity = cvar("m5_flamer_gravity");
		if (g.gravity <= 0)
			g.gravity = 0.001;	// 0 reads as 1 to the engine
		vel = normalize (dir) * speed * (0.96 + random() * 0.08) + p.velocity * 0.4;
		grav = cvar("sv_gravity") * g.gravity;
		pos = org + vel * age - '0 0 0.5' * grav * age * age;
		vel_z = vel_z - grav * age;
		// never into a wall a hose's length ahead of the muzzle
		traceline (org, pos, TRUE, p);
		if (trace_fraction < 1)
			pos = org + (trace_endpos - org) * 0.9;
		g.velocity = vel;
		setmodel (g, "");
		setsize (g, '0 0 0', '0 0 0');
		setorigin (g, pos);
		// INVISIBLE by default (2026-10-04): the stream the player sees is the
		// client's own hose, drawn from the gun's nozzle every rendered frame
		// (cl_particles.c, m5_flamer_hose). The globules carry the damage, the
		// ignition and the fire patches alone, and a model-less entity with no
		// trail is never SENT at all (sv_send.c culls "!modelindex &&
		// !specialvisibilityradius"). m5_flamer_servertrail puts the old trail
		// back on them -- for a client without the hose, or another player's
		// stream on a server: glow_trail is the reason the entity is sent, and
		// the traileffectnum then replaces the glow trail itself.
		if (trail)
		{
			g.glow_trail = 1;
			if (m5_flame_streamfx > 0)
				g.traileffectnum = m5_flame_streamfx;
		}
		g.touch = M5Flame_GlobTouch;
		g.think = M5Flame_GlobThink;
		g.nextthink = time + 0.1;
		g.m5_flameend = time + life * (0.85 + random() * 0.3);
		i = i + 1;
	}

	if (!m5_flame_reported_stream)
	{
		m5_flame_reported_stream = TRUE;
		dprint ("M5 flamer: stream\n");	// asserted by tests/smoke.sh; first emission per map
	}
};

/*
=================
W_FireFlame

Called from the player's flame frames every tenth of a second while fire is
held (player.qc, the thunderbolt's two-frame loop). It pays the fuel, kicks,
and keeps the nozzle (the stream's light) alive; the NOZZLE emits the napalm,
every server frame. Fuel: m5_flamer_cost cells a tick, accumulated so the
cells stay whole numbers -- 0.75 spends three cells every four ticks (Seb,
2026-10-04: "reduce the ammo burn rate by 25% whilst keeping the damage the
same").
=================
*/
void() W_FireFlame =
{
	local float cost;

	cost = cvar("m5_flamer_cost");
	if (cost <= 0)
		cost = 1;
	if (self.ammo_cells < 1)
	{
		self.weapon = W_BestWeapon ();
		W_SetCurrentAmmo ();
		return;
	}

	M5Flame_Effects ();

	// submerged: a hiss of steam and nothing else, no fuel spent
	if (self.waterlevel > 1)
	{
		makevectors (self.v_angle);
		M5Flame_Steam (self.origin + '0 0 22' + v_forward * 42 - v_up * 14);
		if (self.t_width < time)
		{
			sound7 (self, CHAN_WEAPON, "weapons/lhit.wav", 0.5, ATTN_NORM, 160, 0);
			self.t_width = time + 0.6;
		}
		return;
	}

	self.m5_flamefuel = self.m5_flamefuel + cost;
	while (self.m5_flamefuel >= 1 && self.ammo_cells >= 1)
	{
		self.ammo_cells = self.ammo_cells - 1;
		self.m5_flamefuel = self.m5_flamefuel - 1;
	}
	self.currentammo = self.ammo_cells;
	M5Feel_Kick (IT_M5FLAME, -0.5);	// a faint shudder every tick (m5feel.qc)
	M5Flame_Light ();
};
