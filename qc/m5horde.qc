/*
===============================================================================

M5 HORDE MODE

Wave-based survival on any map, gated on the m5_horde cvar (registered by
the engine, toggled from the M5 Fun Mods menu).  A director entity spawned
from worldspawn steals the map's own deathmatch/teleport/start spots as
monster spawn points, and spends a per-wave points budget on a weighted
monster roster.  Variety is guaranteed: heavies may never spend more than
half a wave's budget, so the rabble keeps coming at every wave number.
About one in five monsters spawns already enraged at a different-species
monster instead of the player — Quake's native infighting does the rest.

Waves trickle in (alive cap grows with the wave number), spawns prefer
spots out of the player's sight, cleared waves grant a breather, health,
ammo and weapon milestones.  Death prints the run length and records the
best run in the archived m5_horde_best cvar.  Map monsters are removed in
walkmonster_start/flymonster_start/swimmonster_start (monsters.qc); kill
bookkeeping lives in Killed (combat.qc).

Monster type ids used throughout:
 1 soldier  2 dog  3 knight  4 wizard      (light)
 5 enforcer 6 ogre 7 hknight 8 zombie      (medium)
 9 demon   10 tarbaby 11 shalrath 12 shambler  (heavy)

===============================================================================
*/

// DarkPlaces extension: box trace (dpextensions.qc); used to reject spawn
// spots the monster's hull cannot actually stand in
void(vector v1, vector bmins, vector bmaxs, vector v2, float nomonsters, entity forent) tracebox = #90;

float m5h_wave;			// current wave number (1-based once running)
float m5h_state;		// 0 = break between waves, 1 = wave active
float m5h_statetime;	// when the current break ends
float m5h_budget;		// points left to spend this wave
float m5h_heavyleft;	// heavy-spend allowance left this wave (variety cap)
float m5h_nextspawn;	// earliest time of the next spawn burst
float m5h_hidden;		// last known "player is hidden" state, for the dprint below

// THE DIRECTOR (SEPTEMBER2 H, 2026-09-10; m5_horde_director, default 0 = the
// fixed budget above). Reads the player at each wave's start -- health and
// armour, ammo for the guns owned, how fast the last wave fell and how badly
// it hurt -- into one score, -1 (calm: the player is on the ropes) to +1
// (pressure: the player is fat), and shapes the wave from it: the budget
// x0.6 to x1.5, the species roll tilted toward rabble or toward the heavies,
// the break after the wave 5 to 14 seconds, the supply drop scaled to need.
float m5h_wavestart;	// director: when the current wave began
float m5h_wavebudget;	// director: the budget it began with (after the multiplier)
float m5h_lastclear;	// director: when the last wave was cleared
float m5h_minhealth;	// director: the player's lowest health during the wave
float m5h_dirscore;		// director: the current score (0 with the director off)
float m5h_dirmult;		// director: the current wave's budget multiplier (1 off)
float m5h_dirbreak;		// director: the break it chose after the last wave (8 off)

entity() M5H_Player =
{
	return find (world, classname, "player");
};

float() M5H_SkillScale =
{
	local float s;

	s = cvar("skill");
	if (s <= 0)
		return 0.7;
	if (s == 1)
		return 1;
	if (s == 2)
		return 1.3;
	return 1.6;
};

float() M5H_AliveCap =
{
	local float c;

	c = 12 + m5h_wave * 2;
	if (c > 40)
		c = 40;
	return c;
};

/*
================
M5H_Cost / M5H_UnlockWave / M5H_IsHeavy

The roster tables.  Zombies unlock late (wave 6) so the player has
explosives to gib them with by the time they turn up.
================
*/
float(float t) M5H_Cost =
{
	if (t == 1) return 1;	// soldier
	if (t == 2) return 1;	// dog
	if (t == 3) return 2;	// knight
	if (t == 4) return 2;	// wizard
	if (t == 5) return 2;	// enforcer
	if (t == 6) return 4;	// ogre
	if (t == 7) return 5;	// hell knight
	if (t == 8) return 3;	// zombie
	if (t == 9) return 6;	// demon
	if (t == 10) return 7;	// tarbaby
	if (t == 11) return 8;	// shalrath
	return 12;				// shambler
};

float(float t) M5H_UnlockWave =
{
	if (t == 1) return 1;
	if (t == 2) return 1;
	if (t == 3) return 1;
	if (t == 4) return 2;
	if (t == 5) return 2;
	if (t == 6) return 2;
	if (t == 7) return 4;
	if (t == 8) return 6;
	if (t == 9) return 5;
	if (t == 10) return 7;
	if (t == 11) return 7;
	return 9;				// shambler
};

float(float t) M5H_IsHeavy =
{
	return t >= 9;
};

float(float t) M5H_IsBig =	// uses the wide 32-radius hull
{
	// dog, ogre, demon, shalrath, shambler (the dog really is that wide)
	return t == 2 || t == 6 || t == 9 || t == 11 || t == 12;
};

/*
================
M5H_AmmoFraction / M5H_DirectorScore

The director's reading of the player. Ammo is averaged over the guns he
OWNS (shells always), so a player who has not been handed the nailgun yet
is not read as starving for nails. The last wave's pace is measured against
2.5 seconds a budget point (a 33-point wave is expected to take ~80 s);
its hurt is how low his health went. Everything is bounded before it is
summed, and the sum is bounded to -1..1.
================
*/
float(entity pl) M5H_AmmoFraction =
{
	local float a, n;

	a = pl.ammo_shells / 100;
	n = 1;
	if (pl.items & (IT_NAILGUN | IT_SUPER_NAILGUN))
	{
		a = a + pl.ammo_nails / 200;
		n = n + 1;
	}
	if (pl.items & (IT_GRENADE_LAUNCHER | IT_ROCKET_LAUNCHER))
	{
		a = a + pl.ammo_rockets / 100;
		n = n + 1;
	}
	if (pl.items & IT_LIGHTNING)
	{
		a = a + pl.ammo_cells / 100;
		n = n + 1;
	}
	a = a / n;
	if (a > 1)
		a = 1;
	return a;
};

float(entity pl) M5H_DirectorScore =
{
	local float h, a, s, dur, pace, hurt;

	h = (pl.health + pl.armorvalue * 0.5) / 100;
	if (h > 1.5)
		h = 1.5;
	if (h < 0)
		h = 0;
	a = M5H_AmmoFraction (pl);
	s = 0.9 * (h - 0.7) + 0.6 * (a - 0.5);
	if (m5h_wavestart > 0 && m5h_lastclear > m5h_wavestart)
	{
		dur = m5h_lastclear - m5h_wavestart;
		if (dur < 1)
			dur = 1;
		pace = (m5h_wavebudget * 2.5) / dur - 1;		// > 0 fast, < 0 slow
		if (pace > 1)
			pace = 1;
		if (pace < -1)
			pace = -1;
		hurt = 1 - m5h_minhealth / 100;
		if (hurt < 0)
			hurt = 0;
		if (hurt > 1)
			hurt = 1;
		s = s + 0.5 * pace - 0.7 * hurt;
	}
	if (s > 1)
		s = 1;
	if (s < -1)
		s = -1;
	return s;
};

/*
================
M5H_RollType

Tier roll keeps every wave mixed: 40% light, 35% medium, 25% heavy, with
heavies further limited by the per-wave heavy-spend allowance.  Falls back
to lighter picks when the roll is locked or unaffordable; returns 0 only
when even a soldier is beyond the remaining budget.
================
*/
float() M5H_RollType =
{
	local float r, t, tries, light, heavy;

	// Test hook: m5_horde_species forces every pick to one species, so a
	// feature that belongs to one monster can be exercised deterministically
	// instead of waiting for the roll to produce it. Console-only, default 0.
	// It still respects the wave unlock and the budget, so it cannot conjure a
	// shambler in wave 1; it just stops the randomness.
	t = cvar("m5_horde_species");
	if (t >= 1 && t <= 12)
	{
		if (m5h_wave < M5H_UnlockWave(t))
			return 0;
		if (M5H_Cost(t) > m5h_budget)
			return 0;
		return t;
	}

	// the director's TILT: a hurt player draws rabble (light 60%, heavy 5% at
	// score -1), a fat one draws the heavies (light 20%, heavy 45% at +1);
	// with the director off the score is 0 and these are the 40/35/25 above
	light = 0.4 - 0.2 * m5h_dirscore;
	heavy = 0.25 + 0.2 * m5h_dirscore;
	tries = 8;
	while (tries > 0)
	{
		tries = tries - 1;
		r = random();
		if (r < light)
			t = 1 + floor(random() * 4);		// light: 1-4
		else if (r < 1 - heavy)
			t = 5 + floor(random() * 4);		// medium: 5-8
		else
			t = 9 + floor(random() * 4);		// heavy: 9-12
		if (t > 12)
			t = 12;
		if (m5h_wave < M5H_UnlockWave(t))
			continue;
		if (M5H_Cost(t) > m5h_budget)
			continue;
		if (M5H_IsHeavy(t) && M5H_Cost(t) > m5h_heavyleft)
			continue;
		return t;
	}
	if (m5h_budget >= 1)
		return 1;								// soldier always fits
	return 0;
};

void(float t) M5H_CallSpawn =
{
	if (t == 1) monster_army ();
	else if (t == 2) monster_dog ();
	else if (t == 3) monster_knight ();
	else if (t == 4) monster_wizard ();
	else if (t == 5) monster_enforcer ();
	else if (t == 6) monster_ogre ();
	else if (t == 7) monster_hell_knight ();
	else if (t == 8) monster_zombie ();
	else if (t == 9) monster_demon1 ();
	else if (t == 10) monster_tarbaby ();
	else if (t == 11) monster_shalrath ();
	else monster_shambler ();
};

/*
================
M5H_ClassName

Map-spawned monsters get their classname from the entity parser; ours must
set it by hand or infighting (which compares classnames) never triggers.
================
*/
string(float t) M5H_ClassName =
{
	if (t == 1) return "monster_army";
	if (t == 2) return "monster_dog";
	if (t == 3) return "monster_knight";
	if (t == 4) return "monster_wizard";
	if (t == 5) return "monster_enforcer";
	if (t == 6) return "monster_ogre";
	if (t == 7) return "monster_hell_knight";
	if (t == 8) return "monster_zombie";
	if (t == 9) return "monster_demon1";
	if (t == 10) return "monster_tarbaby";
	if (t == 11) return "monster_shalrath";
	return "monster_shambler";
};

/*
================
M5H_CountSpots / M5H_SpotByIndex

The map's deathmatch spawns, teleport destinations and player starts,
treated as one flat list.
================
*/
float(string cname) M5H_CountClass =
{
	local entity e;
	local float n;

	n = 0;
	e = find (world, classname, cname);
	while (e != world)
	{
		n = n + 1;
		e = find (e, classname, cname);
	}
	return n;
};

float() M5H_CountSpots =
{
	return M5H_CountClass("info_player_deathmatch")
		+ M5H_CountClass("info_teleport_destination")
		+ M5H_CountClass("info_player_start");
};

entity(string cname, float index) M5H_ClassByIndex =
{
	local entity e;

	e = find (world, classname, cname);
	while (e != world && index > 0)
	{
		index = index - 1;
		e = find (e, classname, cname);
	}
	return e;
};

entity(float index) M5H_SpotByIndex =
{
	local float n;

	n = M5H_CountClass("info_player_deathmatch");
	if (index < n)
		return M5H_ClassByIndex("info_player_deathmatch", index);
	index = index - n;
	n = M5H_CountClass("info_teleport_destination");
	if (index < n)
		return M5H_ClassByIndex("info_teleport_destination", index);
	index = index - n;
	return M5H_ClassByIndex("info_player_start", index);
};

/*
================
M5H_PickSpot

Random spot for a monster of the given hull size.  Hard requirements
(unoccupied, hull actually fits) never relax; soft preferences (distance
from the player, out of sight) relax as retries run out.  Returns world
when nothing suitable was found — the director just tries again next beat.
================
*/
entity(float big) M5H_PickSpot =
{
	local entity pl, sel, head;
	local vector bmins, bmaxs, org;
	local float num, tries, dist, occupied;

	if (big)
	{
		bmins = '-32 -32 -24';
		bmaxs = '32 32 64';
	}
	else
	{
		bmins = '-16 -16 -24';
		bmaxs = '16 16 40';
	}

	pl = M5H_Player();
	num = M5H_CountSpots();
	if (num < 1)
		return world;

	tries = 8;
	while (tries > 0)
	{
		tries = tries - 1;
		sel = M5H_SpotByIndex(floor(random() * num));
		if (sel == world)
			continue;

		// never materialise inside someone; the .m5horde check matters because
		// freshly burst-spawned monsters don't carry FL_MONSTER until their
		// start think has run
		occupied = FALSE;
		head = findradius (sel.origin, 64);
		while (head != world)
		{
			if (head.health > 0)
				if ((head.flags & FL_MONSTER) || head.m5horde || head.classname == "player")
					occupied = TRUE;
			head = head.chain;
		}
		if (occupied)
			continue;

		// the hull must genuinely fit here (some starts are tucked into walls)
		org = sel.origin + '0 0 8';
		tracebox (org, bmins, bmaxs, org, TRUE, world);
		if (trace_startsolid)
			continue;

		if (pl != world)
		{
			dist = vlen (sel.origin - pl.origin);
			if (dist < 160)
				continue;
			if (dist > 3500 && tries > 2)
				continue;
			// prefer spawning out of the player's sight
			traceline (pl.origin + pl.view_ofs, sel.origin + '0 0 24', TRUE, pl);
			if (trace_fraction == 1 && tries > 3)
				continue;
		}
		return sel;
	}
	return world;
};

/*
================
M5H_PlayerHidden

TRUE when the player must not be acquired: no player at all, notarget, or
the Ring of Shadows.  These are the same two tests FindTarget (ai.qc) and
monster_use (monsters.qc) apply, repeated here because the horde director
wakes monsters through FoundTarget() directly and so never goes anywhere
near FindTarget — which is exactly why notarget appeared to do nothing in
horde mode.
================
*/
float(entity pl) M5H_PlayerHidden =
{
	if (pl == world)
		return TRUE;
	if (pl.flags & FL_NOTARGET)
		return TRUE;
	if (pl.items & IT_INVISIBILITY)
		return TRUE;
	return FALSE;
};

/*
================
M5H_Aggro

Wake a freshly spawned monster.  One in five picks a brawl with a random
living horde monster of a different species instead of the player —
reservoir-sampled so every candidate is equally likely.

While the player is hidden the brawl is the ONLY option: a hidden player is
treated as absent, the different-species rule relaxes so that a wave of one
species still fights itself, and a monster with nobody left to fight is left
idle rather than falling back to the player.
================
*/
void(entity mon) M5H_Aggro =
{
	local entity targ, cand, samecand, e, oself;
	local float n, samen, hidden;

	if (mon == world)
		return;
	if (mon.health <= 0)
		return;
	if (mon.enemy != world)
		return;

	targ = M5H_Player();
	hidden = M5H_PlayerHidden (targ);
	if (hidden)
		targ = world;

	if (hidden || random() < 0.2)
	{
		n = 0;
		samen = 0;
		cand = world;
		samecand = world;
		e = nextent (world);
		while (e != world)
		{
			if (e.m5horde && e.health > 0 && e != mon)
			{
				if (e.classname != mon.classname)
				{
					n = n + 1;
					if (random() < 1 / n)
						cand = e;
				}
				else
				{
					// same species: reservoir-sampled separately, and used
					// only as the hidden-player fallback below
					samen = samen + 1;
					if (random() < 1 / samen)
						samecand = e;
				}
			}
			e = nextent (e);
		}
		if (cand == world && hidden)
			cand = samecand;
		if (cand != world)
			targ = cand;
	}
	if (targ == world)
		return;

	oself = self;
	self = mon;
	self.enemy = targ;
	FoundTarget ();
	self = oself;
};

/*
================
M5H_SpawnMonster
================
*/
void(entity spot, float t) M5H_SpawnMonster =
{
	local entity mon, oself;

	mon = spawn ();
	mon.m5horde = TRUE;
	mon.classname = M5H_ClassName (t);
	mon.origin = spot.origin + '0 0 8';
	mon.angles = spot.angles;
	spawn_tfog (spot.origin);

	oself = self;
	self = mon;
	M5H_CallSpawn (t);
	self = oself;

	m5h_alive = m5h_alive + 1;
	// force-aggroed by the director's sweep once its start think has run
	mon.m5h_aggrotime = time + 0.8;
};

/*
================
M5H_Rehome

Straggler safety: teleport surviving monsters to fresh spots so a monster
stuck behind a locked door can't stall the wave forever.
================
*/
void() M5H_Rehome =
{
	local entity e, spot;

	e = nextent (world);
	while (e != world)
	{
		if (e.m5horde && e.health > 0)
		{
			spot = M5H_PickSpot (TRUE);
			if (spot != world)
			{
				spawn_tfog (e.origin);
				setorigin (e, spot.origin + '0 0 8');
				spawn_tfog (spot.origin);
			}
		}
		e = nextent (e);
	}
	m5h_lastkill = time;
	bprint ("The horde closes in...\n");
};

/*
================
M5H_Reward

Wave-clear breather: health top-up, ammo restock, and the weapon ladder —
SSG at wave 2, nailgun 3, super nailgun 4, grenade launcher 5, rocket
launcher 6, lightning gun 8.
================
*/
void(entity pl) M5H_Reward =
{
	local entity oself;
	local float need;

	// the director's supply drop: more of everything after a wave that left
	// the player on the ropes (score below 0), the stock drop otherwise
	need = 1;
	if (m5h_dirscore < 0)
		need = 1 - m5h_dirscore;		// up to double at -1

	if (pl.health < 100)
	{
		pl.health = pl.health + floor (25 * need);
		if (pl.health > 100)
			pl.health = 100;
	}

	if (m5h_wave >= 2 && !(pl.items & IT_SUPER_SHOTGUN))
	{
		pl.items = pl.items | IT_SUPER_SHOTGUN;
		bprint ("Supply drop: super shotgun\n");
	}
	if (m5h_wave >= 3 && !(pl.items & IT_NAILGUN))
	{
		pl.items = pl.items | IT_NAILGUN;
		bprint ("Supply drop: nailgun\n");
	}
	if (m5h_wave >= 4 && !(pl.items & IT_SUPER_NAILGUN))
	{
		pl.items = pl.items | IT_SUPER_NAILGUN;
		bprint ("Supply drop: super nailgun\n");
	}
	if (m5h_wave >= 5 && !(pl.items & IT_GRENADE_LAUNCHER))
	{
		pl.items = pl.items | IT_GRENADE_LAUNCHER;
		bprint ("Supply drop: grenade launcher\n");
	}
	if (m5h_wave >= 6 && !(pl.items & IT_ROCKET_LAUNCHER))
	{
		pl.items = pl.items | IT_ROCKET_LAUNCHER;
		bprint ("Supply drop: rocket launcher\n");
	}
	if (m5h_wave >= 8 && !(pl.items & IT_LIGHTNING))
	{
		pl.items = pl.items | IT_LIGHTNING;
		bprint ("Supply drop: lightning gun\n");
	}

	pl.ammo_shells = pl.ammo_shells + floor (20 * need);
	if (pl.ammo_shells > 100)
		pl.ammo_shells = 100;
	if (pl.items & (IT_NAILGUN | IT_SUPER_NAILGUN))
	{
		pl.ammo_nails = pl.ammo_nails + floor (40 * need);
		if (pl.ammo_nails > 200)
			pl.ammo_nails = 200;
	}
	if (pl.items & (IT_GRENADE_LAUNCHER | IT_ROCKET_LAUNCHER))
	{
		pl.ammo_rockets = pl.ammo_rockets + floor (8 * need);
		if (pl.ammo_rockets > 100)
			pl.ammo_rockets = 100;
	}
	if (pl.items & IT_LIGHTNING)
	{
		pl.ammo_cells = pl.ammo_cells + floor (20 * need);
		if (pl.ammo_cells > 100)
			pl.ammo_cells = 100;
	}

	sound (pl, CHAN_ITEM, "weapons/pkup.wav", 1, ATTN_NORM);
	oself = self;
	self = pl;
	W_SetCurrentAmmo ();
	self = oself;
};

/*
================
M5Horde_Think

The director's half-second heartbeat.
================
*/
void() M5Horde_Think =
{
	local entity pl, spot, e;
	local float t, burst, nohunt;

	self.nextthink = time + 0.5;

	pl = M5H_Player();
	if (pl == world)
		return;
	if (pl.health <= 0)
		return;

	// While the player is hidden, keep the brawl alive.  Two jobs here:
	//
	// FORGETTING, for notarget only.  Without it just the monsters spawned
	// AFTER the cheat was toggled would ignore you and the wave already on
	// your heels would keep coming — which is how it was reported.  oldenemy
	// goes too, or ai_run (ai.qc) restores the player from it the moment the
	// new target dies.  The Ring of Shadows deliberately does NOT do this:
	// invisibility blocks acquisition, as it does in FindTarget, but it does
	// not make a monster forget a fight it is already in.
	//
	// RE-ENGAGING, for both.  A monster whose opponent dies goes idle and
	// stays idle: vanilla ai_stand only ever re-finds the PLAYER, and
	// FindTarget rightly refuses a hidden one, so nothing in stock AI can
	// hand it a fresh opponent.  Left alone the whole wave drifts to a
	// standstill a minute or two into the brawl.
	if (M5H_PlayerHidden (pl))
	{
		// State-change dprint, following the RT composite's ACTIVE/DISABLED
		// precedent: one deterministic line per toggle, so tests/smoke.sh can
		// assert the brawl actually engaged rather than guessing from timing.
		if (!m5h_hidden)
		{
			m5h_hidden = 1;
			dprint ("M5 horde: player hidden - monsters will brawl\n");
		}

		nohunt = pl.flags & FL_NOTARGET;
		e = nextent (world);
		while (e != world)
		{
			if (e.m5horde && e.health > 0)
			{
				if (nohunt)
				{
					if (e.oldenemy == pl)
						e.oldenemy = world;
					if (e.enemy == pl)
						e.enemy = world;
				}
				// !m5h_aggrotime so a monster still inside its spawn delay
				// is not woken early
				if (e.enemy == world && !e.m5h_aggrotime)
					M5H_Aggro (e);
			}
			e = nextent (e);
		}
	}
	else if (m5h_hidden)
	{
		m5h_hidden = 0;
		dprint ("M5 horde: player visible again\n");
	}

	// wake any monsters whose start think has run (spawned last cycle)
	e = nextent (world);
	while (e != world)
	{
		if (e.m5h_aggrotime && time > e.m5h_aggrotime && e.health > 0)
		{
			e.m5h_aggrotime = 0;
			M5H_Aggro (e);
		}
		e = nextent (e);
	}

	if (m5h_state == 0)
	{
		if (time < m5h_statetime)
			return;
		m5h_wave = m5h_wave + 1;
		m5h_budget = floor ((18 + m5h_wave * 15) * M5H_SkillScale());
		m5h_dirmult = 1;
		m5h_dirscore = 0;
		if (cvar("m5_horde_director"))
		{
			// THE DIRECTOR reads the player and shapes the wave (SEPTEMBER2 H)
			m5h_dirscore = M5H_DirectorScore (pl);
			m5h_dirmult = 1 + 0.5 * m5h_dirscore;
			if (m5h_dirmult < 0.6)
				m5h_dirmult = 0.6;
			if (m5h_dirmult > 1.5)
				m5h_dirmult = 1.5;
			m5h_budget = floor (m5h_budget * m5h_dirmult);
			if (m5h_budget < 4)
				m5h_budget = 4;
			// one line per WAVE under developer 1 (waves are minutes apart);
			// asserted by tests/smoke.sh
			dprint ("M5 horde: director wave ");
			dprint (ftos(m5h_wave));
			dprint (" budget ");
			dprint (ftos(floor (m5h_dirmult * 100 + 0.5)));
			dprint ("% score ");
			dprint (ftos(floor (m5h_dirscore * 100 + 0.5)));	// -100..100
			dprint ("\n");
		}
		m5h_heavyleft = floor (m5h_budget * (0.5 + 0.25 * m5h_dirscore));
		m5h_wavestart = time;
		m5h_wavebudget = m5h_budget;
		m5h_minhealth = pl.health;
		m5h_state = 1;
		m5h_lastkill = time;
		m5h_nextspawn = time;
		bprint ("=== Wave ");
		bprint (ftos(m5h_wave));
		bprint (" ===\n");
		centerprint (pl, "WAVE INCOMING");
		return;
	}

	if (pl.health < m5h_minhealth)
		m5h_minhealth = pl.health;		// the director's "how badly did it hurt"

	// spawn in bursts of up to three per beat so big waves fill quickly
	if (time > m5h_nextspawn)
	{
		burst = 3;
		while (burst > 0 && m5h_budget > 0 && m5h_alive < M5H_AliveCap())
		{
			burst = burst - 1;
			t = M5H_RollType ();
			if (!t)
			{
				m5h_budget = 0;		// nothing affordable left; wind the wave down
				break;
			}
			spot = M5H_PickSpot (M5H_IsBig(t));
			if (spot == world)
				break;				// no usable spot this beat; try again next think
			M5H_SpawnMonster (spot, t);
			m5h_budget = m5h_budget - M5H_Cost(t);
			if (M5H_IsHeavy(t))
				m5h_heavyleft = m5h_heavyleft - M5H_Cost(t);
		}
		m5h_nextspawn = time + 0.2 + random() * 0.3;
	}

	// straggler safety: wave fully spawned but nothing has died for 45 seconds
	if (m5h_budget <= 0 && m5h_alive > 0 && time > m5h_lastkill + 45)
		M5H_Rehome ();

	if (m5h_budget <= 0 && m5h_alive <= 0)
	{
		bprint ("Wave ");
		bprint (ftos(m5h_wave));
		bprint (" cleared!\n");
		centerprint (pl, "WAVE CLEARED");
		m5h_lastclear = time;
		m5h_dirbreak = 8;
		if (cvar("m5_horde_director"))
		{
			// read again with this wave's pace and hurt in: the drop and the
			// break follow the wave just fought -- calm after a hard one
			m5h_dirscore = M5H_DirectorScore (pl);
			m5h_dirbreak = 8 - 5 * m5h_dirscore;
			if (m5h_dirbreak < 5)
				m5h_dirbreak = 5;
			if (m5h_dirbreak > 14)
				m5h_dirbreak = 14;
		}
		M5H_Reward (pl);
		m5h_state = 0;
		m5h_statetime = time + m5h_dirbreak;
	}
};

void() M5Horde_Start =
{
	m5h_wave = 0;
	m5h_state = 0;
	m5h_statetime = time + 8;
	m5h_alive = 0;
	m5h_dirbreak = 8;
	m5h_dirmult = 1;
	m5h_dirscore = 0;
	m5h_wavestart = 0;
	m5h_lastclear = 0;
	bprint ("M5 HORDE MODE - survive as long as you can (best: ");
	bprint (ftos(cvar("m5_horde_best")));
	bprint (" waves)\n");
	centerprint (M5H_Player(), "M5 HORDE MODE");
	self.think = M5Horde_Think;
	self.nextthink = time + 0.5;
};

void() M5Horde_Precache =
{
	// soldier
	precache_model ("progs/soldier.mdl");
	precache_model ("progs/h_guard.mdl");
	precache_model ("progs/gib1.mdl");
	precache_model ("progs/gib2.mdl");
	precache_model ("progs/gib3.mdl");
	precache_sound ("soldier/death1.wav");
	precache_sound ("soldier/idle.wav");
	precache_sound ("soldier/pain1.wav");
	precache_sound ("soldier/pain2.wav");
	precache_sound ("soldier/sattck1.wav");
	precache_sound ("soldier/sight1.wav");
	precache_sound ("player/udeath.wav");
	// dog
	precache_model ("progs/dog.mdl");
	precache_model ("progs/h_dog.mdl");
	precache_sound ("dog/dattack1.wav");
	precache_sound ("dog/ddeath.wav");
	precache_sound ("dog/dpain1.wav");
	precache_sound ("dog/dsight.wav");
	precache_sound ("dog/idle.wav");
	// knight
	precache_model ("progs/knight.mdl");
	precache_model ("progs/h_knight.mdl");
	precache_sound ("knight/idle.wav");
	precache_sound ("knight/kdeath.wav");
	precache_sound ("knight/khurt.wav");
	precache_sound ("knight/ksight.wav");
	precache_sound ("knight/sword1.wav");
	precache_sound ("knight/sword2.wav");
	// wizard (scrag)
	precache_model ("progs/wizard.mdl");
	precache_model ("progs/h_wizard.mdl");
	precache_model ("progs/w_spike.mdl");
	precache_sound ("wizard/hit.wav");
	precache_sound ("wizard/wattack.wav");
	precache_sound ("wizard/wdeath.wav");
	precache_sound ("wizard/widle1.wav");
	precache_sound ("wizard/widle2.wav");
	precache_sound ("wizard/wpain.wav");
	precache_sound ("wizard/wsight.wav");
	// enforcer
	precache_model2 ("progs/enforcer.mdl");
	precache_model2 ("progs/h_mega.mdl");
	precache_model2 ("progs/laser.mdl");
	precache_sound2 ("enforcer/death1.wav");
	precache_sound2 ("enforcer/enfire.wav");
	precache_sound2 ("enforcer/enfstop.wav");
	precache_sound2 ("enforcer/idle1.wav");
	precache_sound2 ("enforcer/pain1.wav");
	precache_sound2 ("enforcer/pain2.wav");
	precache_sound2 ("enforcer/sight1.wav");
	precache_sound2 ("enforcer/sight2.wav");
	precache_sound2 ("enforcer/sight3.wav");
	precache_sound2 ("enforcer/sight4.wav");
	// ogre
	precache_model ("progs/ogre.mdl");
	precache_model ("progs/h_ogre.mdl");
	precache_model ("progs/grenade.mdl");
	precache_sound ("ogre/ogdrag.wav");
	precache_sound ("ogre/ogdth.wav");
	precache_sound ("ogre/ogidle.wav");
	precache_sound ("ogre/ogidle2.wav");
	precache_sound ("ogre/ogpain1.wav");
	precache_sound ("ogre/ogsawatk.wav");
	precache_sound ("ogre/ogwake.wav");
	// hell knight
	precache_model2 ("progs/hknight.mdl");
	precache_model2 ("progs/h_hellkn.mdl");
	precache_model2 ("progs/k_spike.mdl");
	precache_sound ("hknight/hit.wav");
	precache_sound2 ("hknight/attack1.wav");
	precache_sound2 ("hknight/death1.wav");
	precache_sound2 ("hknight/grunt.wav");
	precache_sound2 ("hknight/idle.wav");
	precache_sound2 ("hknight/pain1.wav");
	precache_sound2 ("hknight/sight1.wav");
	precache_sound2 ("hknight/slash1.wav");
	// zombie
	precache_model ("progs/zombie.mdl");
	precache_model ("progs/h_zombie.mdl");
	precache_model ("progs/zom_gib.mdl");
	precache_sound ("zombie/idle_w2.wav");
	precache_sound ("zombie/z_fall.wav");
	precache_sound ("zombie/z_gib.wav");
	precache_sound ("zombie/z_hit.wav");
	precache_sound ("zombie/z_idle.wav");
	precache_sound ("zombie/z_idle1.wav");
	precache_sound ("zombie/z_miss.wav");
	precache_sound ("zombie/z_pain.wav");
	precache_sound ("zombie/z_pain1.wav");
	precache_sound ("zombie/z_shot1.wav");
	// demon (fiend)
	precache_model ("progs/demon.mdl");
	precache_model ("progs/h_demon.mdl");
	precache_sound ("demon/ddeath.wav");
	precache_sound ("demon/dhit2.wav");
	precache_sound ("demon/djump.wav");
	precache_sound ("demon/dpain1.wav");
	precache_sound ("demon/idle1.wav");
	precache_sound ("demon/sight2.wav");
	// tarbaby (spawn)
	precache_model2 ("progs/tarbaby.mdl");
	precache_sound2 ("blob/death1.wav");
	precache_sound2 ("blob/hit1.wav");
	precache_sound2 ("blob/land1.wav");
	precache_sound2 ("blob/sight1.wav");
	// shalrath (vore)
	precache_model2 ("progs/shalrath.mdl");
	precache_model2 ("progs/h_shal.mdl");
	precache_model2 ("progs/v_spike.mdl");
	precache_sound2 ("shalrath/attack.wav");
	precache_sound2 ("shalrath/attack2.wav");
	precache_sound2 ("shalrath/death.wav");
	precache_sound2 ("shalrath/idle.wav");
	precache_sound2 ("shalrath/pain.wav");
	precache_sound2 ("shalrath/sight.wav");
	// shambler
	precache_model ("progs/shambler.mdl");
	precache_model ("progs/h_shams.mdl");
	precache_model ("progs/s_light.mdl");
	precache_model ("progs/bolt.mdl");
	precache_sound ("shambler/melee1.wav");
	precache_sound ("shambler/melee2.wav");
	precache_sound ("shambler/sattck1.wav");
	precache_sound ("shambler/sboom.wav");
	precache_sound ("shambler/sdeath.wav");
	precache_sound ("shambler/shurt2.wav");
	precache_sound ("shambler/sidle.wav");
	precache_sound ("shambler/smack.wav");
	precache_sound ("shambler/ssight.wav");
	// rewards
	precache_sound ("weapons/pkup.wav");
};

void() M5Horde_WorldSpawn =
{
	local entity hd;

	M5Horde_Precache ();
	hd = spawn ();
	hd.classname = "m5_horde_director";
	hd.think = M5Horde_Start;
	hd.nextthink = time + 2;
};

// impulse 210 — the player (self) summons the next wave early.  Works
// mid-wave too: the current wave's unspawned budget is abandoned and any
// stragglers simply join the incoming wave (they still count towards
// clearing it).  No wave-clear reward for a skipped wave.
void() M5Horde_NextWave =
{
	if (!cvar("m5_horde"))
	{
		sprint (self, "horde mode is off\n");
		return;
	}
	if (m5h_state == 0)
	{
		m5h_statetime = time;
		sprint (self, "the horde answers...\n");
	}
	else
	{
		m5h_budget = 0;
		m5h_state = 0;
		m5h_statetime = time;
		sprint (self, "the horde grows...\n");
	}
};

// impulse 211 — status report to the player (self)
void() M5Horde_Status =
{
	if (!cvar("m5_horde"))
	{
		sprint (self, "horde mode is off\n");
		return;
	}
	sprint (self, "wave ");
	sprint (self, ftos(m5h_wave));
	sprint (self, ": ");
	sprint (self, ftos(m5h_alive));
	sprint (self, " alive, ");
	sprint (self, ftos(m5h_budget));
	sprint (self, " budget left\n");
	if (cvar("m5_horde_director"))
	{
		sprint (self, "director: score ");
		sprint (self, ftos(floor (m5h_dirscore * 100 + 0.5)));
		sprint (self, ", budget ");
		sprint (self, ftos(floor (m5h_dirmult * 100 + 0.5)));
		sprint (self, "%, break ");
		sprint (self, ftos(floor (m5h_dirbreak + 0.5)));
		sprint (self, " s\n");
	}
};

void() M5Horde_PlayerDied =
{
	local float best, survived;

	if (m5h_state == 1)
		survived = m5h_wave - 1;
	else
		survived = m5h_wave;
	if (survived < 0)
		survived = 0;

	bprint ("You survived ");
	bprint (ftos(survived));
	bprint (" waves\n");

	best = cvar("m5_horde_best");
	if (survived > best)
	{
		cvar_set ("m5_horde_best", ftos(survived));
		bprint ("A new record!\n");
	}
};
