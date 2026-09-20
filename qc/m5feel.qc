/*
==============================================================================

M5 WEAPON FEEL (SEPTEMBER2 Part G, 2026-09-10)

Five small things the 1996 guns never had, each behind its own cvar where 0 is
the 1996 bytes exactly and each a row on Options -> M5 Fun Mods so the lot can
go back to stock from the menu (Seb's rule for every weapon change). The kick,
the tracers, the barrel streams and the axe sparks are ON by default since
2026-09-16 (Seb, of feel_on.cfg: "feel_on is good. make that default"); the
bounce his ear rejected stays off. exec feel_off.cfg is the 1996 guns.

  m5_kick          per-weapon screen kick: a pitch snap sized to the gun plus
                   the eye SHOVED BACK along the aim (DarkPlaces' punchvector,
                   which stock QuakeC never named). Stock is -2 degrees on
                   every gun and no shove; the value scales the table.
  m5_grenadebounce the bounce re-struck at a random pitch, its volume following
                   the impact speed, a dull tap once the grenade is barely
                   rolling, and a guard so a rolling grenade does not
                   machine-gun the sample. REJECTED by Seb's ear 2026-09-15
                   ("it sounds like a xylophone"): stays an option at 0.
  m5_nailtracer    your own nails leave a short hot streak (an effectinfo
                   trail, so the murk fades it and the fog kernel sees nothing
                   new -- no light per nail by choice) -- on every
                   m5_nailtracer_every'th nail (3; Seb, 2026-09-15: "make
                   tracers only every 3rd or fifth round").
  m5_nailbarrels   the nailgun's two alternating streams leave the two barrel
                   tips, where the muzzle flash is drawn, instead of from
                   outside them (Seb, 2026-09-15: "It has *two* barrels ...
                   they were too wide before"). See M5Feel_NailOrigin.
  m5_axesparks     an axe blow on a wall throws sparks off the surface with a
                   blink of light and a ricochet ring.

All of it is read through cvar() in the server VM, so the cvars are declared
CF_SERVER engine-side (cl_main.c, the m5_shotgun shape). Every dprint here is
FIRST-EVENT per map (the console-ink incident): tests/smoke.sh asserts them.

==============================================================================
*/

float m5feel_fxready;			// effectinfo handles looked up (once per map)
float m5feel_nailfx;
float m5feel_axefx;
float m5feel_reported_kick;		// first-event dprints
float m5feel_reported_bounce;
float m5feel_reported_tracer;
float m5feel_reported_sparks;
float m5feel_reported_barrels;
.float m5_nailtick;				// nails until the next tracer (m5_nailtracer_every)

void() M5Feel_Effects =
{
	if (m5feel_fxready)
		return;
	m5feel_fxready = TRUE;
	m5feel_nailfx = particleeffectnum ("m5nail_trail");
	m5feel_axefx = particleeffectnum ("m5axe_sparks");
};

/*
=================
M5Feel_Kick

Called by every W_Fire* in place of its `self.punchangle_x = -2` (or the Doom
shotgun's -5), with the STOCK value as the second argument: at m5_kick 0 the
stock value is written and nothing else happens, so 0 is the old bytes. Above
0 the weapon's own pitch and shove are scaled by the cvar; where the stock
pitch is the harder one (the Doom shotgun) it wins, so the two mods stack
rather than fight. The shove is along -v_forward from the CURRENT aim.
The bolt calls this every tenth-second tick: punchangle decays at 10 deg/s
and punchvector at 20 u/s (sv_user.c), so a re-struck small kick reads as a
continuous buzz rather than a growing lean.
=================
*/
void(float wep, float stockpitch) M5Feel_Kick =
{
	local float k, pitch, back;

	k = cvar("m5_kick");
	if (k <= 0)
	{
		self.punchangle_x = stockpitch;
		return;
	}
	if (k > 2)
		k = 2;

	if (wep == IT_AXE)              { pitch = 0.6;  back = 0;   }
	else if (wep == IT_SHOTGUN)     { pitch = 3;    back = 2;   }
	else if (wep == IT_SUPER_SHOTGUN) { pitch = 6;  back = 5;   }
	else if (wep == IT_NAILGUN)     { pitch = 0.8;  back = 0.6; }
	else if (wep == IT_SUPER_NAILGUN) { pitch = 1.2; back = 1; }
	else if (wep == IT_GRENADE_LAUNCHER) { pitch = 4; back = 4; }
	else if (wep == IT_ROCKET_LAUNCHER) { pitch = 7; back = 8; }
	else if (wep == IT_LIGHTNING)   { pitch = 0.8 + random() * 0.7; back = 1.2; }
	else if (wep == IT_M5BALL)      { pitch = 5;    back = 6;   }
	else                            { pitch = 2;    back = 0;   }

	// the harder of the stock pitch and the table (stockpitch is negative)
	if (0 - stockpitch > pitch)
		pitch = 0 - stockpitch;
	self.punchangle_x = 0 - pitch * k;
	makevectors (self.v_angle);
	self.punchvector = v_forward * (0 - back * k);
	if (!m5feel_reported_kick)
	{
		m5feel_reported_kick = TRUE;
		dprint ("M5 feel: weapon kick\n");	// developer 1 only; asserted by tests/smoke.sh
	}
};

/*
=================
M5Feel_GrenadeBounce

From GrenadeTouch, self = the grenade. The touch runs BEFORE the physics
reflects the velocity (sv_phys.c SV_Physics_Toss: SV_PushEntity -> SV_Impact,
then ClipVelocity), so self.velocity here is the IMPACT speed. attack_finished
is unused on a grenade and serves as the re-strike guard.
=================
*/
void() M5Feel_GrenadeBounce =
{
	local float speed, vol, pitch;

	if (!cvar("m5_grenadebounce"))
	{
		sound (self, CHAN_WEAPON, "weapons/bounce.wav", 1, ATTN_NORM);
		return;
	}
	if (time < self.attack_finished)
		return;
	self.attack_finished = time + 0.06;
	speed = vlen (self.velocity);
	vol = speed / 450;
	if (vol > 1)
		vol = 1;
	if (vol < 0.15)
		vol = 0.15;
	if (speed < 90)
	{
		// barely rolling: a dull tap, not a boing
		sound7 (self, CHAN_WEAPON, "weapons/tink1.wav", vol * 0.8 + 0.1, ATTN_NORM, 55 + random() * 25, 0);
	}
	else
	{
		pitch = 78 + random() * 44;
		if (speed > 300)
			pitch = pitch - 12 * ((speed - 300) / 500);	// a hard hit rings lower
		if (pitch < 60)
			pitch = 60;
		sound7 (self, CHAN_WEAPON, "weapons/bounce.wav", vol, ATTN_NORM, pitch, 0);
	}
	if (!m5feel_reported_bounce)
	{
		m5feel_reported_bounce = TRUE;
		dprint ("M5 feel: grenade bounce\n");	// asserted by tests/smoke.sh
	}
};

/*
=================
M5Feel_NailTracer

From launch_spike, self = the shooter, nail = the fresh spike. Player nails
only: the ogre's launch_spike goes through here too and is refused by the
classname, so a horde's nails stay bare (and cheap).
=================
*/
void(entity nail) M5Feel_NailTracer =
{
	local float every;

	if (!cvar("m5_nailtracer"))
		return;
	if (self.classname != "player")
		return;
	// every Nth nail: a countdown on the shooter, so the first nail of a map
	// carries one and the cadence runs on across bursts; 1 or less (or an
	// engine without the cvar) is every nail, as before
	every = floor (cvar("m5_nailtracer_every"));
	if (every > 1)
	{
		self.m5_nailtick = self.m5_nailtick - 1;
		if (self.m5_nailtick > 0)
			return;
		self.m5_nailtick = every;
	}
	M5Feel_Effects ();
	if (m5feel_nailfx > 0)
		nail.traileffectnum = m5feel_nailfx;
	if (!m5feel_reported_tracer)
	{
		m5feel_reported_tracer = TRUE;
		dprint ("M5 feel: nail tracer\n");	// asserted by tests/smoke.sh
	}
};

/*
=================
M5Feel_NailOrigin

From W_FireSpikes, self = the player, v_right and v_up set by its makevectors,
ox = id's side for this nail: +4 on the odd weaponframes and -4 on the even ones
(player.qc), and the view model agrees -- its odd shooting frames push a nail out
of the RIGHT barrel and its even ones out of the LEFT. The two barrels sit 3.7
units either side of the eye and 13.7 below it, in the AMI v_nail.mdl and id's
own alike (measured from both files' vertices, 2026-09-15). id's nail starts 4
to the side and 6 below the eye, which projects onto the screen along a WIDER
line than the barrel, so each stream seemed to leave from outside its barrel.
At m5_nailbarrels the nail keeps id's 6 units below the eye, along the view's own
up so it holds at any pitch, and takes the sideways offset that puts it on its
barrel's own screen line: 3.7 x 6 / 13.7 = 1.6. The view weapon draws over
everything, so the nail rides hidden inside the barrel and appears at its tip,
where the flash is drawn. In the world it still hits 6 below the aim, as id's
did, and nearer the crosshair sideways. The super nailgun is untouched (id's
centre line).
=================
*/
vector(float ox) M5Feel_NailOrigin =
{
	if (!cvar("m5_nailbarrels"))
		return self.origin + '0 0 16' + v_right*ox;		// id's, exactly
	if (!m5feel_reported_barrels)
	{
		m5feel_reported_barrels = TRUE;
		dprint ("M5 feel: nails from the barrels\n");	// asserted by tests/smoke.sh
	}
	if (ox > 0)
		return self.origin + self.view_ofs + v_right*1.6 - v_up*6;
	if (ox < 0)
		return self.origin + self.view_ofs - v_right*1.6 - v_up*6;
	return self.origin + self.view_ofs - v_up*6;
};

/*
=================
M5Feel_AxeSparks

From W_FireAxe's wall branch, self = the player, org the hit point pulled a
little off the wall, nrm the wall's normal (trace_plane_normal, still valid
there -- nothing between the traceline and the call runs a trace). The
sparks fly off the surface along the normal (pointparticles' velocity is the
block's base velocity; the block adds its own jitter and gravity).
=================
*/
void(vector org, vector nrm) M5Feel_AxeSparks =
{
	local float r;

	if (!cvar("m5_axesparks"))
		return;
	M5Feel_Effects ();
	if (m5feel_axefx > 0)
		pointparticles (m5feel_axefx, org, nrm * 140, 1);
	r = random();
	if (r < 0.33)
		sound7 (self, CHAN_AUTO, "weapons/ric1.wav", 0.5, ATTN_NORM, 95 + random() * 35, 0);
	else if (r < 0.66)
		sound7 (self, CHAN_AUTO, "weapons/ric2.wav", 0.5, ATTN_NORM, 95 + random() * 35, 0);
	else
		sound7 (self, CHAN_AUTO, "weapons/ric3.wav", 0.5, ATTN_NORM, 95 + random() * 35, 0);
	if (!m5feel_reported_sparks)
	{
		m5feel_reported_sparks = TRUE;
		dprint ("M5 feel: axe sparks\n");	// asserted by tests/smoke.sh
	}
};
