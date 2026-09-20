/*  Copyright (C) 1996-1997  Id Software, Inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

    See file, 'COPYING', for details.
*/

void (entity targ, entity inflictor, entity attacker, float damage) T_Damage;
void () player_run;
void(entity bomb, entity attacker, float rad, entity ignore) T_RadiusDamage;
void(vector org, vector vel, float damage) SpawnBlood;
void() SuperDamageSound;

// M5 ball lightning (m5ball.qc, compiled after this file)
void() W_FireBall;
void(float wep, float stockpitch) M5Feel_Kick;		// m5feel.qc (SEPTEMBER2 G)
void() M5Feel_GrenadeBounce;
void(entity nail) M5Feel_NailTracer;
void(vector org, vector nrm) M5Feel_AxeSparks;
void() M5Ball_Sync;
void() M5Ball_Select;


// called by worldspawn
void() W_Precache =
{
	precache_sound ("weapons/r_exp3.wav");	// new rocket explosion
	precache_sound ("weapons/rocket1i.wav");	// spike gun
	precache_sound ("weapons/sgun1.wav");
	precache_sound ("weapons/guncock.wav");	// player shotgun
	precache_sound ("weapons/ric1.wav");	// ricochet (used in c code)
	precache_sound ("weapons/ric2.wav");	// ricochet (used in c code)
	precache_sound ("weapons/ric3.wav");	// ricochet (used in c code)
	precache_sound ("weapons/spike2.wav");	// super spikes
	precache_sound ("weapons/tink1.wav");	// spikes tink (used in c code)
	precache_sound ("weapons/grenade.wav");	// grenade launcher
	precache_sound ("weapons/bounce.wav");		// grenade bounce
	precache_sound ("weapons/shotgn2.wav");	// super shotgun
	precache_sound ("ambience/buzz1.wav");	// M5 ball lightning: the ball's hum (a stock looped ambient)
	precache_sound ("ambience/drone6.wav");	// M5 ball lightning: the ball's drone
	precache_sound ("ambience/thunder1.wav");	// M5 ball lightning: the discharge
};

float() crandom =
{
	return 2*(random() - 0.5);
};

/*
================
W_FireAxe
================
*/
void() W_FireAxe =
{
	local vector source;
	local vector org;

	makevectors (self.v_angle);
	source = self.origin + '0 0 16';
	traceline (source, source + v_forward*64, FALSE, self);
	if (trace_fraction == 1.0)
		return;

	org = trace_endpos - v_forward*4;

	if (trace_ent.takedamage)
	{
		trace_ent.axhitme = 1;
		SpawnBlood (org, '0 0 0', 20);
		T_Damage (trace_ent, self, self, 20);
	}
	else
	{	// hit wall
		sound (self, CHAN_WEAPON, "player/axhit2.wav", 1, ATTN_NORM);
		WriteByte (MSG_BROADCAST, SVC_TEMPENTITY);
		WriteByte (MSG_BROADCAST, TE_GUNSHOT);
		WriteCoord (MSG_BROADCAST, org_x);
		WriteCoord (MSG_BROADCAST, org_y);
		WriteCoord (MSG_BROADCAST, org_z);
		M5Feel_AxeSparks (org, trace_plane_normal);	// no-op at m5_axesparks 0
	}
};


//============================================================================


vector() wall_velocity =
{
	local vector vel;

	vel = normalize (self.velocity);
	vel = normalize(vel + v_up*(random()- 0.5) + v_right*(random()- 0.5));
	vel = vel + 2*trace_plane_normal;
	vel = vel * 200;

	return vel;
};


/*
================
SpawnMeatSpray
================
*/
void(vector org, vector vel) SpawnMeatSpray =
{
	local entity missile;

	missile = spawn ();
	missile.owner = self;
	missile.movetype = MOVETYPE_BOUNCE;
	missile.solid = SOLID_NOT;

	makevectors (self.angles);

	missile.velocity = vel;
	missile.velocity_z = missile.velocity_z + 250 + 50*random();

	missile.avelocity = '3000 1000 2000';

	// set missile duration
	missile.nextthink = time + 1;
	missile.think = SUB_Remove;

	setmodel (missile, "progs/zom_gib.mdl");
	setsize (missile, '0 0 0', '0 0 0');
	setorigin (missile, org);
};

/*
================
SpawnBlood
================
*/
void(vector org, vector vel, float damage) SpawnBlood =
{
	particle (org, vel*0.1, 73, damage*2);
};

/*
================
spawn_touchblood
================
*/
void(float damage) spawn_touchblood =
{
	local vector vel;

	vel = wall_velocity () * 0.2;
	SpawnBlood (self.origin + vel*0.01, vel, damage);
};


/*
================
SpawnChunk
================
*/
void(vector org, vector vel) SpawnChunk =
{
	particle (org, vel*0.02, 0, 10);
};

/*
==============================================================================

MULTI-DAMAGE

Collects multiple small damages into a single damage

==============================================================================
*/

entity multi_ent;
float multi_damage;
float m5_boomstick_reported;	// one diagnostic line per map (the console-volume incident)

void() ClearMultiDamage =
{
	multi_ent = world;
	multi_damage = 0;
};

void() ApplyMultiDamage =
{
	if (!multi_ent)
		return;
	T_Damage (multi_ent, self, self, multi_damage);
};

void(entity hit, float damage) AddMultiDamage =
{
	if (!hit)
		return;

	if (hit != multi_ent)
	{
		ApplyMultiDamage ();
		multi_damage = damage;
		multi_ent = hit;
	}
	else
		multi_damage = multi_damage + damage;
};

/*
==============================================================================

BULLETS

==============================================================================
*/

/*
================
TraceAttack
================
*/
void(float damage, vector dir) TraceAttack =
{
	local vector vel, org;

	vel = normalize(dir + v_up*crandom() + v_right*crandom());
	vel = vel + 2*trace_plane_normal;
	vel = vel * 200;

	org = trace_endpos - dir*4;

	if (trace_ent.takedamage)
	{
		SpawnBlood (org, vel*0.2, damage);
		AddMultiDamage (trace_ent, damage);
	}
	else
	{
		WriteByte (MSG_BROADCAST, SVC_TEMPENTITY);
		WriteByte (MSG_BROADCAST, TE_GUNSHOT);
		WriteCoord (MSG_BROADCAST, org_x);
		WriteCoord (MSG_BROADCAST, org_y);
		WriteCoord (MSG_BROADCAST, org_z);
	}
};

/*
================
FireBullets

Used by shotgun, super shotgun, and enemy soldier firing
Go to the trouble of combining multiple pellets into a single damage call.
================
*/
void(float shotcount, vector dir, vector spread) FireBullets =
{
	local vector direction;
	local vector src;

	makevectors(self.v_angle);

	src = self.origin + v_forward*10;
	src_z = self.absmin_z + self.size_z * 0.7;

	ClearMultiDamage ();
	while (shotcount > 0)
	{
		direction = dir + crandom()*spread_x*v_right + crandom()*spread_y*v_up;

		traceline (src, src + direction*2048, FALSE, self);
		if (trace_fraction != 1.0)
			TraceAttack (4, direction);

		shotcount = shotcount - 1;
	}
	ApplyMultiDamage ();
};

/*
================
W_FireShotgun
================
*/
void() W_FireShotgun =
{
	local vector dir;

	sound (self, CHAN_WEAPON, "weapons/guncock.wav", 1, ATTN_NORM);

	if (cvar("m5_shotgun"))
	{
		// M5 Doom-style boomstick: more pellets, tighter spread, harder kick
		if (!m5_boomstick_reported)
		{
			m5_boomstick_reported = TRUE;	// once per map (the console-volume incident)
			dprint ("M5 boomstick fired\n");	// developer 1 only; asserted by tests/smoke.sh
		}
		M5Feel_Kick (IT_SHOTGUN, -5);
		self.currentammo = self.ammo_shells = self.ammo_shells - 1;
		dir = aim (self, 100000);
		FireBullets (10, dir, '0.03 0.03 0');
		return;
	}

	M5Feel_Kick (IT_SHOTGUN, -2);

	self.currentammo = self.ammo_shells = self.ammo_shells - 1;
	dir = aim (self, 100000);
	FireBullets (6, dir, '0.04 0.04 0');
};


/*
================
W_FireSuperShotgun
================
*/
void() W_FireSuperShotgun =
{
	local vector dir;

	if (self.currentammo == 1)
	{
		W_FireShotgun ();
		return;
	}

	sound (self ,CHAN_WEAPON, "weapons/shotgn2.wav", 1, ATTN_NORM);

	if (cvar("m5_shotgun"))
	{
		// M5 Doom-style super shotgun: a proper room-clearer
		M5Feel_Kick (IT_SUPER_SHOTGUN, -7);
		self.currentammo = self.ammo_shells = self.ammo_shells - 2;
		dir = aim (self, 100000);
		FireBullets (21, dir, '0.10 0.06 0');
		return;
	}

	M5Feel_Kick (IT_SUPER_SHOTGUN, -4);

	self.currentammo = self.ammo_shells = self.ammo_shells - 2;
	dir = aim (self, 100000);
	FireBullets (14, dir, '0.14 0.08 0');
};


/*
==============================================================================

ROCKETS

==============================================================================
*/

void()	s_explode1	=	[0,		s_explode2] {};
void()	s_explode2	=	[1,		s_explode3] {};
void()	s_explode3	=	[2,		s_explode4] {};
void()	s_explode4	=	[3,		s_explode5] {};
void()	s_explode5	=	[4,		s_explode6] {};
void()	s_explode6	=	[5,		SUB_Remove] {};

/*
================
M5_SetExplosionModel

SEPTEMBER S5 step 2. m5/effectinfo.txt's L2 BANG and L3 FIREBALL layers were
authored to REPLACE progs/s_explod.spr, so with the file in place the 1996
sprite is a second fireball drawn over the first. m5_explosion_sprite 0 removes
it. Called from the three sites that used to setmodel the sprite directly:
BecomeExplosion below, and the inline copies in ogre.qc and shalrath.qc.

setmodel with an EMPTY STRING is the DarkPlaces idiom for "no model", not a
hack: SV_ModelIndex returns 0 for an empty name (sv_main.c), so .model becomes
"" and .modelindex 0, and dpextensions.qc names exactly that state as one of
the engine's three ways of being invisible.

THE SETMODEL IS ALSO WHAT STOPS THE ROCKET TRAIL, which is why nothing here
just deletes the call. progs/missile.mdl carries MF_ROCKET and grenade.mdl
MF_GRENADE; the engine turns model flags into loadmodel->effects and ORs them
into the render entity, and that is what drives the trail. Changing the model
to a sprite (effects 0) is what ends the trail today. Changing it to nothing
ends it harder: an entity with modelindex 0 and no special effects is not
networked at all (sv_send.c), so the client deactivates it and never runs the
trail on it.

NOT .alpha: alpha 0 is a NO-OP (the server applies only a non-zero value, so 0
means "unset" and networks as opaque), and alpha -1 -- the value that does
work -- is DarkPlaces' Nehahra cigar-smoke sentinel and would turn a grenade's
trail into TR_NEHAHRASMOKE. Either way the model stays, so the trail keeps
running.

The precache in world.qc STAYS whatever this is set to: precache order defines
modelindex, and deleting an entry shifts every model after it.

The s_explode1..6 chain still runs on the off arm. It draws nothing, but it is
what keeps the edict alive for exactly 0.6 s and then removes it; removing it
early would change edict recycling order, a gameplay-visible risk for nothing.
================
*/
float m5_explosion_sprite_reported;	// one diagnostic line per map (the console-volume incident)

void() M5_SetExplosionModel =
{
	if (cvar("m5_explosion_sprite"))
		setmodel (self, "progs/s_explod.spr");
	else
	{
		if (!m5_explosion_sprite_reported)
		{
			m5_explosion_sprite_reported = TRUE;	// once per map
			dprint ("M5 explosion sprite off\n");	// developer 1 only; asserted by tests/smoke.sh
		}
		setmodel (self, "");
	}
};

void() BecomeExplosion =
{
	self.movetype = MOVETYPE_NONE;
	self.velocity = '0 0 0';
	self.touch = SUB_Null;
	M5_SetExplosionModel ();
	self.solid = SOLID_NOT;
	s_explode1 ();
};

void() T_MissileTouch =
{
	local float damg;

	if (other == self.owner)
		// don't explode on owner
		return;

	if (pointcontents(self.origin) == CONTENT_SKY)
	{
		remove(self);
		return;
	}

	damg = 100 + random()*20;

	if (other.health)
	{
		if (other.classname == "monster_shambler")
			// mostly immune
			damg = damg * 0.5;
		T_Damage (other, self, self.owner, damg );
	}

	// don't do radius damage to the other, because all the damage
	// was done in the impact
	T_RadiusDamage (self, self.owner, 120, other);

	self.origin = self.origin - 8*normalize(self.velocity);

	WriteByte (MSG_BROADCAST, SVC_TEMPENTITY);
	WriteByte (MSG_BROADCAST, TE_EXPLOSION);
	WriteCoord (MSG_BROADCAST, self.origin_x);
	WriteCoord (MSG_BROADCAST, self.origin_y);
	WriteCoord (MSG_BROADCAST, self.origin_z);

	BecomeExplosion ();
};



/*
================
W_FireRocket
================
*/
void() W_FireRocket =
{
	local entity missile;

	self.currentammo = self.ammo_rockets = self.ammo_rockets - 1;

	sound (self, CHAN_WEAPON, "weapons/sgun1.wav", 1, ATTN_NORM);

	M5Feel_Kick (IT_ROCKET_LAUNCHER, -2);

	missile = spawn ();
	missile.owner = self;
	missile.movetype = MOVETYPE_FLYMISSILE;
	missile.solid = SOLID_BBOX;
	missile.classname = "missile";

	// set missile speed
	makevectors (self.v_angle);
	missile.velocity = aim(self, 1000);
	missile.velocity = missile.velocity * 1000;
	missile.angles = vectoangles(missile.velocity);

	missile.touch = T_MissileTouch;

	// set missile duration
	missile.nextthink = time + 5;
	missile.think = SUB_Remove;

	setmodel (missile, "progs/missile.mdl");
	setsize (missile, '0 0 0', '0 0 0');
	setorigin (missile, self.origin + v_forward*8 + '0 0 16');
};

/*
===============================================================================

LIGHTNING

===============================================================================
*/

/*
=================
LightningDamage
=================
*/
// M5: returns 1 if the bolt actually damaged something this shot. The engine
// uses that to burn brighter and sizzle louder the moment you connect - see
// W_FireLightning below and r_lightningbeam_m5_hitboost.
float(vector p1, vector p2, entity from, float damage) LightningDamage =
{
	local entity e1, e2;
	local vector f;
	local float connected;

	connected = 0;
	f = p2 - p1;
	normalize (f);
	f_x = 0 - f_y;
	f_y = f_x;
	f_z = 0;
	f = f*16;

	e1 = e2 = world;

	traceline (p1, p2, FALSE, self);
	if (trace_ent.takedamage)
	{
		particle (trace_endpos, '0 0 100', 225, damage*4);
		T_Damage (trace_ent, from, from, damage);
		M5_Ignite (trace_ent, from);
		connected = 1;
		if (self.classname == "player")
		{
			if (other.classname == "player")
				trace_ent.velocity_z = trace_ent.velocity_z + 400;
		}
	}
	e1 = trace_ent;

	traceline (p1 + f, p2 + f, FALSE, self);
	if (trace_ent != e1 && trace_ent.takedamage)
	{
		particle (trace_endpos, '0 0 100', 225, damage*4);
		T_Damage (trace_ent, from, from, damage);
		M5_Ignite (trace_ent, from);
		connected = 1;
	}
	e2 = trace_ent;

	traceline (p1 - f, p2 - f, FALSE, self);
	if (trace_ent != e1 && trace_ent != e2 && trace_ent.takedamage)
	{
		particle (trace_endpos, '0 0 100', 225, damage*4);
		T_Damage (trace_ent, from, from, damage);
		M5_Ignite (trace_ent, from);
		connected = 1;
	}

	return connected;
};


void() W_FireLightning =
{
	local vector org;
	local vector hitpos;
	local vector drawpos;
	local float cells;
	local float connected;

	if (self.ammo_cells < 1)
	{
		self.weapon = W_BestWeapon ();
		W_SetCurrentAmmo ();
		return;
	}

	// explode if under water
	if (self.waterlevel > 1)
	{
		cells = self.ammo_cells;
		self.ammo_cells = 0;
		W_SetCurrentAmmo ();
		T_RadiusDamage (self, self, 35*cells, world);
		return;
	}

	if (self.t_width < time)
	{
		sound (self, CHAN_WEAPON, "weapons/lhit.wav", 1, ATTN_NORM);
		self.t_width = time + 0.6;
	}
	M5Feel_Kick (IT_LIGHTNING, -2);	// every tick: a buzz, not a lean (m5feel.qc)

	self.currentammo = self.ammo_cells = self.ammo_cells - 1;

	org = self.origin + '0 0 16';

	traceline (org, org + v_forward*600, TRUE, self);

	// M5: the damage pass has to run BEFORE the beam is written, so this shot's
	// beam can say whether it connected. LightningDamage runs three tracelines of
	// its own and clobbers trace_endpos, so the impact point is saved first -
	// without that the beam would be sent to the wrong place, or the hit state
	// would be a server frame stale (~100 ms, plainly visible).
	hitpos = trace_endpos;

	// M5: a SECOND trace, this one WITH monsters, used only for the DRAWN
	// endpoint. Vanilla traces the beam with nomonsters TRUE, so the bolt is
	// drawn straight through whatever you are hitting and always ends on the wall
	// behind it - which is why hitting something never looked any different from
	// missing. Damage is untouched: LightningDamage uses its p1/p2 arguments only
	// to build its own offset and as its own tracelines' endpoints, and it is
	// still handed hitpos exactly as before.
	traceline (org, org + v_forward*600, FALSE, self);
	drawpos = trace_endpos;

	connected = LightningDamage (self.origin, hitpos + v_forward*4, self, 30);

	WriteByte (MSG_BROADCAST, SVC_TEMPENTITY);
	// TE_LIGHTNING1 vs 2 is the connect signal. Both are ordinary lightning
	// beams that differ only in their bolt model, which reaches the client as
	// beam_t.model - so this costs no protocol change and no extra traffic.
	// The engine reads it in CL_Beam_M5_BuildPath (r_lightning.c) and burns the
	// bolt brighter; see r_lightningbeam_m5_hitboost.
	if (connected)
		WriteByte (MSG_BROADCAST, TE_LIGHTNING1);
	else
		WriteByte (MSG_BROADCAST, TE_LIGHTNING2);
	WriteEntity (MSG_BROADCAST, self);
	WriteCoord (MSG_BROADCAST, org_x);
	WriteCoord (MSG_BROADCAST, org_y);
	WriteCoord (MSG_BROADCAST, org_z);
	WriteCoord (MSG_BROADCAST, drawpos_x);
	WriteCoord (MSG_BROADCAST, drawpos_y);
	WriteCoord (MSG_BROADCAST, drawpos_z);

	// and the audible half of the feedback. Its own channel and its own much
	// faster cadence than the 0.6 s firing tick above, so connecting reads as a
	// continuous sizzle rather than a slow click.
	if (connected)
	{
		if (self.m5_zaptime < time)
		{
			sound (self, CHAN_AUTO, "weapons/lhit.wav", 1, ATTN_NORM);
			self.m5_zaptime = time + 0.15;
		}
	}
};


//=============================================================================


void() GrenadeExplode =
{
	T_RadiusDamage (self, self.owner, 120, world);

	WriteByte (MSG_BROADCAST, SVC_TEMPENTITY);
	WriteByte (MSG_BROADCAST, TE_EXPLOSION);
	WriteCoord (MSG_BROADCAST, self.origin_x);
	WriteCoord (MSG_BROADCAST, self.origin_y);
	WriteCoord (MSG_BROADCAST, self.origin_z);

	BecomeExplosion ();
};

void() GrenadeTouch =
{
	if (other == self.owner)
		// don't explode on owner
		return;
	if (other.takedamage == DAMAGE_AIM)
	{
		GrenadeExplode();
		return;
	}
	// bounce sound (m5feel.qc: the stock one at m5_grenadebounce 0)
	M5Feel_GrenadeBounce ();
	if (self.velocity == '0 0 0')
		self.avelocity = '0 0 0';
};

/*
================
W_FireGrenade
================
*/
void() W_FireGrenade =
{
	local entity missile;

	self.currentammo = self.ammo_rockets = self.ammo_rockets - 1;

	sound (self, CHAN_WEAPON, "weapons/grenade.wav", 1, ATTN_NORM);

	M5Feel_Kick (IT_GRENADE_LAUNCHER, -2);

	missile = spawn ();
	missile.owner = self;
	missile.movetype = MOVETYPE_BOUNCE;
	missile.solid = SOLID_BBOX;
	missile.classname = "grenade";

	// set missile speed
	makevectors (self.v_angle);

	if (self.v_angle_x)
		missile.velocity = v_forward*600 + v_up * 200 + crandom()*v_right*10 + crandom()*v_up*10;
	else
	{
		missile.velocity = aim(self, 10000);
		missile.velocity = missile.velocity * 600;
		missile.velocity_z = 200;
	}

	missile.avelocity = '300 300 300';

	missile.angles = vectoangles(missile.velocity);

	missile.touch = GrenadeTouch;

	// set missile duration
	missile.nextthink = time + 2.5;
	missile.think = GrenadeExplode;

	setmodel (missile, "progs/grenade.mdl");
	setsize (missile, '0 0 0', '0 0 0');
	setorigin (missile, self.origin);
};


//=============================================================================

void() spike_touch;
void() superspike_touch;


/*
===============
launch_spike

Used for both the player and the ogre
===============
*/
void(vector org, vector dir) launch_spike =
{
	newmis = spawn ();
	newmis.owner = self;
	newmis.movetype = MOVETYPE_FLYMISSILE;
	newmis.solid = SOLID_BBOX;

	newmis.angles = vectoangles(dir);

	newmis.touch = spike_touch;
	newmis.classname = "spike";
	newmis.think = SUB_Remove;
	newmis.nextthink = time + 6;
	setmodel (newmis, "progs/spike.mdl");
	setsize (newmis, VEC_ORIGIN, VEC_ORIGIN);
	setorigin (newmis, org);

	newmis.velocity = dir * 1000;
	M5Feel_NailTracer (newmis);		// player nails only, no-op at m5_nailtracer 0
};

void() W_FireSuperSpikes =
{
	local vector dir;

	sound (self, CHAN_WEAPON, "weapons/spike2.wav", 1, ATTN_NORM);
	self.attack_finished = time + 0.2;
	self.currentammo = self.ammo_nails = self.ammo_nails - 2;
	dir = aim (self, 1000);
	launch_spike (self.origin + '0 0 16', dir);
	newmis.touch = superspike_touch;
	setmodel (newmis, "progs/s_spike.mdl");
	setsize (newmis, VEC_ORIGIN, VEC_ORIGIN);
	M5Feel_Kick (IT_SUPER_NAILGUN, -2);
};

void(float ox) W_FireSpikes =
{
	local vector dir;

	makevectors (self.v_angle);

	if (self.ammo_nails >= 2 && self.weapon == IT_SUPER_NAILGUN)
	{
		W_FireSuperSpikes ();
		return;
	}

	if (self.ammo_nails < 1)
	{
		self.weapon = W_BestWeapon ();
		W_SetCurrentAmmo ();
		return;
	}

	sound (self, CHAN_WEAPON, "weapons/rocket1i.wav", 1, ATTN_NORM);
	self.attack_finished = time + 0.2;
	self.currentammo = self.ammo_nails = self.ammo_nails - 1;
	dir = aim (self, 1000);
	launch_spike (M5Feel_NailOrigin(ox), dir);	// id's origin at m5_nailbarrels 0; the barrel tips at 1

	M5Feel_Kick (IT_NAILGUN, -2);
};


void() spike_touch =
{
	if (other == self.owner)
		return;

	if (other.solid == SOLID_TRIGGER)
		// trigger field, do nothing
		return;

	if (pointcontents(self.origin) == CONTENT_SKY)
	{
		remove(self);
		return;
	}

	// The acid sizzle, on BOTH branches. Vanilla sends no impact effect at all
	// on a flesh hit, so a Scrag scoring on you produced nothing to see. It
	// goes here rather than inside the branches so that it cannot land between
	// a WriteByte and its WriteCoords. No-op with m5_venom 0.
	if (self.classname == "wizspike")
		M5Venom_Splash (self.origin);

	// hit something that bleeds
	if (other.takedamage)
	{
		spawn_touchblood (9);
		T_Damage (other, self, self.owner, 9);
	}
	else
	{
		WriteByte (MSG_BROADCAST, SVC_TEMPENTITY);

		if (self.classname == "wizspike")
			WriteByte (MSG_BROADCAST, TE_WIZSPIKE);
		else if (self.classname == "knightspike")
			WriteByte (MSG_BROADCAST, TE_KNIGHTSPIKE);
		else
			WriteByte (MSG_BROADCAST, TE_SPIKE);
		WriteCoord (MSG_BROADCAST, self.origin_x);
		WriteCoord (MSG_BROADCAST, self.origin_y);
		WriteCoord (MSG_BROADCAST, self.origin_z);
	}

	remove(self);

};

void() superspike_touch =
{
	if (other == self.owner)
		return;

	if (other.solid == SOLID_TRIGGER)
		// trigger field, do nothing
		return;

	if (pointcontents(self.origin) == CONTENT_SKY)
	{
		remove(self);
		return;
	}

	// hit something that bleeds
	if (other.takedamage)
	{
		spawn_touchblood (18);
		T_Damage (other, self, self.owner, 18);
	}
	else
	{
		WriteByte (MSG_BROADCAST, SVC_TEMPENTITY);
		WriteByte (MSG_BROADCAST, TE_SUPERSPIKE);
		WriteCoord (MSG_BROADCAST, self.origin_x);
		WriteCoord (MSG_BROADCAST, self.origin_y);
		WriteCoord (MSG_BROADCAST, self.origin_z);
	}

	remove(self);

};


/*
===============================================================================

PLAYER WEAPON USE

===============================================================================
*/

void() W_SetCurrentAmmo =
{
	// get out of any weapon firing states
	player_run ();

	self.items = self.items - ( self.items & (IT_SHELLS | IT_NAILS | IT_ROCKETS | IT_CELLS) );

	if (self.weapon == IT_AXE)
	{
		self.currentammo = 0;
		self.weaponmodel = "progs/v_axe.mdl";
		self.weaponframe = 0;
	}
	else if (self.weapon == IT_SHOTGUN)
	{
		self.currentammo = self.ammo_shells;
		self.weaponmodel = "progs/v_shot.mdl";
		self.weaponframe = 0;
		self.items = self.items | IT_SHELLS;
	}
	else if (self.weapon == IT_SUPER_SHOTGUN)
	{
		self.currentammo = self.ammo_shells;
		self.weaponmodel = "progs/v_shot2.mdl";
		self.weaponframe = 0;
		self.items = self.items | IT_SHELLS;
	}
	else if (self.weapon == IT_NAILGUN)
	{
		self.currentammo = self.ammo_nails;
		self.weaponmodel = "progs/v_nail.mdl";
		self.weaponframe = 0;
		self.items = self.items | IT_NAILS;
	}
	else if (self.weapon == IT_SUPER_NAILGUN)
	{
		self.currentammo = self.ammo_nails;
		self.weaponmodel = "progs/v_nail2.mdl";
		self.weaponframe = 0;
		self.items = self.items | IT_NAILS;
	}
	else if (self.weapon == IT_GRENADE_LAUNCHER)
	{
		self.currentammo = self.ammo_rockets;
		self.weaponmodel = "progs/v_rock.mdl";
		self.weaponframe = 0;
		self.items = self.items | IT_ROCKETS;
	}
	else if (self.weapon == IT_ROCKET_LAUNCHER)
	{
		self.currentammo = self.ammo_rockets;
		self.weaponmodel = "progs/v_rock2.mdl";
		self.weaponframe = 0;
		self.items = self.items | IT_ROCKETS;
	}
	else if (self.weapon == IT_LIGHTNING)
	{
		self.currentammo = self.ammo_cells;
		self.weaponmodel = "progs/v_light.mdl";
		self.weaponframe = 0;
		self.items = self.items | IT_CELLS;
	}
	else if (self.weapon == IT_M5BALL)
	{
		// M5 ball lightning: the thunderbolt's cells, Dissolution's plasma gun
		// (m5/progs/v_plasma.mdl, borrowed -- see docs/CONTENT.md)
		self.currentammo = self.ammo_cells;
		if (cvar("m5_hasplasma"))
			self.weaponmodel = "progs/v_plasma.mdl";
		else
			self.weaponmodel = "progs/v_light.mdl";	// a public install: the thunderbolt's own
		self.weaponframe = 0;
		self.items = self.items | IT_CELLS;
	}
	else
	{
		self.currentammo = 0;
		self.weaponmodel = string_null;
		self.weaponframe = 0;
	}
};

float() W_BestWeapon =
{
	local float it;

	it = self.items;

	if (self.waterlevel <= 1 && self.ammo_cells >= 1 && (it & IT_LIGHTNING) )
		return IT_LIGHTNING;
	else if(self.ammo_nails >= 2 && (it & IT_SUPER_NAILGUN) )
		return IT_SUPER_NAILGUN;
	else if(self.ammo_shells >= 2 && (it & IT_SUPER_SHOTGUN) )
		return IT_SUPER_SHOTGUN;
	else if(self.ammo_nails >= 1 && (it & IT_NAILGUN) )
		return IT_NAILGUN;
	else if(self.ammo_shells >= 1 && (it & IT_SHOTGUN) )
		return IT_SHOTGUN;

	return IT_AXE;
};

float() W_CheckNoAmmo =
{
	if (self.currentammo > 0)
		return TRUE;

	if (self.weapon == IT_AXE)
		return TRUE;

	self.weapon = W_BestWeapon ();

	W_SetCurrentAmmo ();

	// drop the weapon down
	return FALSE;
};

/*
============
W_Attack

An attack impulse can be triggered now
============
*/
void()	player_axe1;
void()	player_axeb1;
void()	player_axec1;
void()	player_axed1;
void()	player_shot1;
void()	player_nail1;
void()	player_light1;
void()	player_rocket1;

void() W_Attack =
{
	local float r;

	if (!W_CheckNoAmmo ())
		return;

	// calculate forward angle for velocity
	makevectors(self.v_angle);
	// wake monsters up
	self.show_hostile = time + 1;

	if (self.weapon == IT_AXE)
	{
		sound (self, CHAN_WEAPON, "weapons/ax1.wav", 1, ATTN_NORM);
		if (cvar("m5_kick") > 0)
			M5Feel_Kick (IT_AXE, 0);	// stock sets no kick on a swing; leave it untouched at 0
		r = random();
		if (r < 0.25)
			player_axe1 ();
		else if (r<0.5)
			player_axeb1 ();
		else if (r<0.75)
			player_axec1 ();
		else
			player_axed1 ();
		self.attack_finished = time + 0.5;
	}
	else if (self.weapon == IT_SHOTGUN)
	{
		player_shot1 ();
		W_FireShotgun ();
		self.attack_finished = time + 0.5;
	}
	else if (self.weapon == IT_SUPER_SHOTGUN)
	{
		player_shot1 ();
		W_FireSuperShotgun ();
		self.attack_finished = time + 0.7;
	}
	else if (self.weapon == IT_NAILGUN)
	{
		player_nail1 ();
	}
	else if (self.weapon == IT_SUPER_NAILGUN)
	{
		player_nail1 ();
	}
	else if (self.weapon == IT_GRENADE_LAUNCHER)
	{
		player_rocket1();
		W_FireGrenade();
		self.attack_finished = time + 0.6;
	}
	else if (self.weapon == IT_ROCKET_LAUNCHER)
	{
		player_rocket1();
		W_FireRocket();
		self.attack_finished = time + 0.8;
	}
	else if (self.weapon == IT_LIGHTNING)
	{
		player_light1();
		self.attack_finished = time + 0.1;
		sound (self, CHAN_AUTO, "weapons/lstart.wav", 1, ATTN_NORM);
	}
	else if (self.weapon == IT_M5BALL)
	{
		// M5 ball lightning: one ball per press, the rocket launcher's firing
		// pose (the thunderbolt's frames call W_FireLightning themselves)
		player_rocket1();
		W_FireBall();
		r = cvar("m5_balllightning_refire");
		if (r < 0.2)
			r = 0.2;
		self.attack_finished = time + r;
	}
};

/*
============
W_ChangeWeapon

============
*/
void() W_ChangeWeapon =
{
	local float it, am, fl;

	it = self.items;
	am = 0;
	fl = 0;

	if (self.impulse == 1)
	{
		fl = IT_AXE;
	}
	else if (self.impulse == 2)
	{
		fl = IT_SHOTGUN;
		if (self.ammo_shells < 1)
			am = 1;
	}
	else if (self.impulse == 3)
	{
		fl = IT_SUPER_SHOTGUN;
		if (self.ammo_shells < 2)
			am = 1;
	}
	else if (self.impulse == 4)
	{
		fl = IT_NAILGUN;
		if (self.ammo_nails < 1)
			am = 1;
	}
	else if (self.impulse == 5)
	{
		fl = IT_SUPER_NAILGUN;
		if (self.ammo_nails < 2)
			am = 1;
	}
	else if (self.impulse == 6)
	{
		fl = IT_GRENADE_LAUNCHER;
		if (self.ammo_rockets < 1)
			am = 1;
	}
	else if (self.impulse == 7)
	{
		fl = IT_ROCKET_LAUNCHER;
		if (self.ammo_rockets < 1)
			am = 1;
	}
	else if (self.impulse == 8)
	{
		fl = IT_LIGHTNING;
		if (self.ammo_cells < 1)
			am = 1;
	}

	self.impulse = 0;

	if (!(self.items & fl))
	{
		// don't have the weapon or the ammo
		sprint (self, "no weapon.\n");
		return;
	}

	if (am)
	{
		// don't have the ammo
		sprint (self, "not enough ammo.\n");
		return;
	}

	// set weapon, set ammo
	self.weapon = fl;
	W_SetCurrentAmmo ();
};

/*
============
CheatCommand
============
*/
void() CheatCommand =
{
	if (deathmatch || coop)
		return;

	self.ammo_rockets = 100;
	self.ammo_nails = 200;
	self.ammo_shells = 100;
	self.items = self.items |
		IT_AXE |
		IT_SHOTGUN |
		IT_SUPER_SHOTGUN |
		IT_NAILGUN |
		IT_SUPER_NAILGUN |
		IT_GRENADE_LAUNCHER |
		IT_ROCKET_LAUNCHER |
		IT_KEY1 | IT_KEY2;

	self.ammo_cells = 200;
	self.items = self.items | IT_LIGHTNING;

	self.weapon = IT_ROCKET_LAUNCHER;
	self.impulse = 0;
	W_SetCurrentAmmo ();
};

/*
============
CycleWeaponCommand

Go to the next weapon with ammo
============
*/
void() CycleWeaponCommand =
{
	local float it, am;

	it = self.items;
	self.impulse = 0;

	while (1)
	{
		am = 0;

		if (self.weapon == IT_LIGHTNING)
		{
			// M5: the ball gun sits past the thunderbolt when it is granted
			if (it & IT_M5BALL)
			{
				self.weapon = IT_M5BALL;
				if (self.ammo_cells < cvar("m5_balllightning_cost"))
					am = 1;
			}
			else
				self.weapon = IT_AXE;
		}
		else if (self.weapon == IT_M5BALL)
		{
			self.weapon = IT_AXE;
		}
		else if (self.weapon == IT_AXE)
		{
			self.weapon = IT_SHOTGUN;
			if (self.ammo_shells < 1)
				am = 1;
		}
		else if (self.weapon == IT_SHOTGUN)
		{
			self.weapon = IT_SUPER_SHOTGUN;
			if (self.ammo_shells < 2)
				am = 1;
		}
		else if (self.weapon == IT_SUPER_SHOTGUN)
		{
			self.weapon = IT_NAILGUN;
			if (self.ammo_nails < 1)
				am = 1;
		}
		else if (self.weapon == IT_NAILGUN)
		{
			self.weapon = IT_SUPER_NAILGUN;
			if (self.ammo_nails < 2)
				am = 1;
		}
		else if (self.weapon == IT_SUPER_NAILGUN)
		{
			self.weapon = IT_GRENADE_LAUNCHER;
			if (self.ammo_rockets < 1)
				am = 1;
		}
		else if (self.weapon == IT_GRENADE_LAUNCHER)
		{
			self.weapon = IT_ROCKET_LAUNCHER;
			if (self.ammo_rockets < 1)
				am = 1;
		}
		else if (self.weapon == IT_ROCKET_LAUNCHER)
		{
			self.weapon = IT_LIGHTNING;
			if (self.ammo_cells < 1)
				am = 1;
		}

		if ( (self.items & self.weapon) && am == 0)
		{
			W_SetCurrentAmmo ();
			return;
		}
	}
};

/*
============
CycleWeaponReverseCommand

Go to the prev weapon with ammo
============
*/
void() CycleWeaponReverseCommand =
{
	local float it, am;

	it = self.items;
	self.impulse = 0;

	while (1)
	{
		am = 0;

		if (self.weapon == IT_LIGHTNING)
		{
			self.weapon = IT_ROCKET_LAUNCHER;
			if (self.ammo_rockets < 1)
				am = 1;
		}
		else if (self.weapon == IT_ROCKET_LAUNCHER)
		{
			self.weapon = IT_GRENADE_LAUNCHER;
			if (self.ammo_rockets < 1)
				am = 1;
		}
		else if (self.weapon == IT_GRENADE_LAUNCHER)
		{
			self.weapon = IT_SUPER_NAILGUN;
			if (self.ammo_nails < 2)
				am = 1;
		}
		else if (self.weapon == IT_SUPER_NAILGUN)
		{
			self.weapon = IT_NAILGUN;
			if (self.ammo_nails < 1)
				am = 1;
		}
		else if (self.weapon == IT_NAILGUN)
		{
			self.weapon = IT_SUPER_SHOTGUN;
			if (self.ammo_shells < 2)
				am = 1;
		}
		else if (self.weapon == IT_SUPER_SHOTGUN)
		{
			self.weapon = IT_SHOTGUN;
			if (self.ammo_shells < 1)
				am = 1;
		}
		else if (self.weapon == IT_SHOTGUN)
		{
			self.weapon = IT_AXE;
		}
		else if (self.weapon == IT_AXE)
		{
			// M5: the ball gun sits past the thunderbolt when it is granted
			if (it & IT_M5BALL)
			{
				self.weapon = IT_M5BALL;
				if (self.ammo_cells < cvar("m5_balllightning_cost"))
					am = 1;
			}
			else
			{
				self.weapon = IT_LIGHTNING;
				if (self.ammo_cells < 1)
					am = 1;
			}
		}
		else if (self.weapon == IT_M5BALL)
		{
			self.weapon = IT_LIGHTNING;
			if (self.ammo_cells < 1)
				am = 1;
		}

		if ( (it & self.weapon) && am == 0)
		{
			W_SetCurrentAmmo ();
			return;
		}
	}
};

/*
============
ServerflagsCommand

Just for development
============
*/
void() ServerflagsCommand =
{
	serverflags = serverflags * 2 + 1;
};

void() QuadCheat =
{
	if (deathmatch || coop)
		return;
	self.super_time = 1;
	self.super_damage_finished = time + 30;
	self.items = self.items | IT_QUAD;
	dprint ("quad cheat\n");
};

/*
============
ImpulseCommands

============
*/
/*
============
M5ImpulseCommands

The M5 mods reserve the 200-range impulses, so everything is bindable and
console-invokable (e.g. bind p "impulse 222").  Powerup grants are gated
on sv_cheats like any other cheat.

 200  cycle gore preset          201  toggle Doom shotgun
 202  select ball lightning (the ninth weapon; press again for the bolt)
 210  horde: summon next wave    211  horde: status report
 222  pentagram (30s, cheat)     223  quad damage (30s, cheat)
 224  ring of shadows (30s, cheat)
============
*/
void() M5ImpulseCommands =
{
	local float g;

	if (self.impulse == 200)
	{
		g = cvar("m5_gore") + 1;
		if (g > 2)
			g = 0;
		cvar_set ("m5_gore", ftos(g));
		sprint (self, "gore preset ");
		sprint (self, ftos(g));
		sprint (self, "\n");
	}
	if (self.impulse == 201)
	{
		if (cvar("m5_shotgun"))
		{
			cvar_set ("m5_shotgun", "0");
			sprint (self, "shotgun: vanilla\n");
		}
		else
		{
			cvar_set ("m5_shotgun", "1");
			sprint (self, "shotgun: DOOM\n");
		}
	}
	if (self.impulse == 202)
		M5Ball_Select ();
	if (self.impulse == 210)
		M5Horde_NextWave ();
	if (self.impulse == 211)
		M5Horde_Status ();
	if (self.impulse == 222 && cvar("sv_cheats"))
	{
		self.items = self.items | IT_INVULNERABILITY;
		self.invincible_time = 1;
		self.invincible_finished = time + 30;
		sprint (self, "Pentagram of Protection!\n");
	}
	if (self.impulse == 223 && cvar("sv_cheats"))
	{
		self.items = self.items | IT_QUAD;
		self.super_time = 1;
		self.super_damage_finished = time + 30;
		sprint (self, "Quad Damage!\n");
	}
	if (self.impulse == 224 && cvar("sv_cheats"))
	{
		// The client half is already wired: CheckPowerups (client.qc) does the
		// countdown, the three-second flashing warning and the item-bit removal,
		// and FindTarget (ai.qc) already refuses to acquire an invisible player.
		self.items = self.items | IT_INVISIBILITY;
		self.invisible_time = 1;
		self.invisible_finished = time + 30;
		sprint (self, "Ring of Shadows!\n");
	}
};

void() ImpulseCommands =
{
	if (self.impulse >= 1 && self.impulse <= 8)
		W_ChangeWeapon ();

	if (self.impulse == 9)
		CheatCommand ();
	if (self.impulse == 10)
		CycleWeaponCommand ();
	if (self.impulse == 11)
		ServerflagsCommand ();
	if (self.impulse == 12)
		CycleWeaponReverseCommand ();
	if (self.impulse >= 200 && self.impulse <= 250)
		M5ImpulseCommands ();
	if (self.impulse == 255)
		QuadCheat ();

	self.impulse = 0;
};

/*
============
W_WeaponFrame

Called every frame so impulse events can be handled as well as possible
============
*/
void() W_WeaponFrame =
{
	if (time < self.attack_finished)
		return;

	M5Ball_Sync ();		// grant or withdraw the ball gun with m5_balllightning
	ImpulseCommands ();

	// check for attack
	if (self.button0)
	{
		SuperDamageSound ();
		W_Attack ();
	}
};

/*
========
SuperDamageSound

Plays sound if needed
========
*/
void() SuperDamageSound =
{
	if (self.super_damage_finished > time)
	{
		if (self.super_sound < time)
		{
			self.super_sound = time + 1;
			sound (self, CHAN_BODY, "items/damage3.wav", 1, ATTN_NORM);
		}
	}
	return;
};


