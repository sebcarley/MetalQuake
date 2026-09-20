# Replacement content — provenance and credits

The assets themselves live in `m5/` and are **deliberately not committed** — `/m5/` is
gitignored, and this fork's `origin` is the public upstream. This file records what is
installed, where it came from, and what each source asks for in return.

Nothing here is redistributed by this repository. It is a record of a local install.

---

## Models — Authentic Model Improvements (AMI)

| | |
|---|---|
| Source | <https://github.com/NightFright2k19/quake_authmdl> |
| Version | **r26** (2021-09-10), `auth_mdl-r26.zip` |
| SHA-256 | `acfe756d8bf575b8f75ccfea43a1a0e0a18729f22288ac4400fd13c77114c24b` |
| Format | Quake MDL (so `.frame` numbering is preserved by construction) |
| Installed | `m5/progs/` (73 models) and `m5/maps/` (15 item BSPs) |

Licence, quoted from the pack's own `auth_mdl.txt`:

> Authors may use the contents of this file as a base for modification or reuse. You may
> distribute this file, provided you include this text file, with no modifications.

**Provenance caveat, recorded deliberately.** Nine of the models — Chthon, Death Knight,
Enforcer, Fiend, Ogre, Scrag, Vore, Zombie and the Rocket Launcher — are converted from
**QuakeEX, the commercial Quake remaster (MachineGames / Nightdive Studios)**. The pack
cannot grant rights it does not hold, so that portion is not freely redistributable whatever
its own licence text says; the pack ships MachineGames' `NOTICE.txt` alongside. Installing it
locally for personal use alongside a legitimately owned copy of Quake is one thing;
redistributing it is another, and this repo does neither.

The remaining models are community-authored — credits in `auth_mdl.txt` name osjclatchford,
Chillo, Dwere, Lunaran, Sock, Tea Monster, capnbubs, Barnaby, Bloodshot, Skiffy, Plague,
Jonatan "eldrone" Pöljö, Tribal, Seven and talisa, compiled by NightFright.

### What was deliberately NOT installed

The zip ships `id1/progs.dat`, `hipnotic/progs.dat` and `rogue/progs.dat`. **None of them are
installed.** They would sit above `m5/progs.dat` in the search path and silently replace this
fork's entire QuakeC lane — horde mode, the lightning burn, the Doom shotgun, every M5
impulse. `tests/smoke.sh` would catch it, but it is easier not to do it. Only the contents of
`id1/pakz.pak` were extracted.

### Verified at install

- **Frame counts** checked against vanilla for all 73 models. One difference:
  `progs/player.mdl` has **144** frames against vanilla's 143. That is one *more*, so every
  frame QuakeC asks for still resolves — a shortfall would have silently clamped the model to
  frame 0, which is the failure mode to watch for in any future pack.
- **No external images** in the pack (MDL and BSP only), so the alpha trap that removes
  surfaces from the RT acceleration structure does not apply to it. It will apply to any
  texture pack.
- **Triangles: 10,670 → 28,338 (×2.66)** across replaced models. Entity triangles at a
  measured horde peak go from 614 to roughly 1,600 — **0.6% of `RT_ENT_MAXTRIS` (262,144)**,
  so the silent-truncation cap is nowhere near being a problem.
- No `unknown/unsupported type` on e1m1 / e1m3 / e1m4 / e1m7.

---

## Textures — Quake Revitalization Project (QRP)

| | |
|---|---|
| Source | <http://qrp.quakeone.com/> (the project's own mirror host is dead; fetched from a ModDB mirror) |
| Version | `QRP_map_textures_v.1.00.pk3` (2010-12-06), from `QRP_map_textures_v.1.00.pk3.7z` |
| SHA-256 (.7z) | `ec05b443c77af87d5285f608684c1501158e4a74f03b090115120df7680b990d` |
| Installed | `m5/QRP_map_textures_v.1.00.pk3` (417 MB, left as a pk3 — one file to remove) |
| Contents | 682 TGA: 570 diffuse + **112 `_luma` glow layers** |
| Not installed | the normal-map add-on (~354 MB) — see below |
| Credits | Moon[Drunk] (diffuse and luma), My-Key (normal maps) |

Licence: free to use in any project, commercial or non-commercial, **on condition that the
project name and URL (<http://qrp.quakeone.com>) are credited**. That condition is met by
this file.

### Verified at install

- **Alpha scan of all 682 images.** 14 came back non-opaque and **all 14 are sky** — `sky1`,
  `sky4` and their `_alpha` companions, across the per-map copies. That is legitimate: a Quake
  sky is two layers and the alpha channel *is* the cloud mask, sky is special-cased out of the
  normal texture path in `model_brush.c`, and sky surfaces are never shadow casters. **Zero
  problematic wall textures**, so nothing is dropped from the RT world BLAS and there are no
  light leaks. Worth running on any future pack: a positive result here is invisible in game.
- **Brightness: no retune needed.** Scene luminance across e1m1/e1m3/e1m4/e1m7 moved by
  0.98–1.02×, so `rt_metal_walllight` and `_ambient` are left alone. This contradicted the
  prediction that a modern pack would read brighter — QRP genuinely preserved the 1996 tonal
  values, which is what it set out to do.
- **Five textures exceed `gl_max_size` (2048)** and are therefore silently downscaled:
  `lgmetal`, `lgmetal2`, `lgmetal3`, `lgmetal4` (2560×1536) and `quake.tga` (2304×512).
  Harmless; raise `gl_max_size` to 4096 if you want them at full resolution, at a VRAM cost.
- **Texture memory 264 MB → 374 MB** on e1m3 (172 → 234 textures). Fine on unified memory.
- **Frame cost: within noise.** Interleaved demo5 runs, vanilla content vs AMI + QRP: the
  within-config spread exceeded the between-config difference.

### Found later: the pack silently disabled every liquid feature (2026-08-07)

The alpha scan above was the right check and it passed. This one was not run, and it should
have been: **QRP replaces all 17 of id1's liquid textures** — `#water0/1/2`, `#04water1/2`,
`#04awater1`, `#04mwat1/2`, `#slime*`, `#lava1`, `#teleport` — and an external image is by
itself enough to make `Mod_LoadTextureFromQ3Shader` succeed, after which `model_brush.c`
skips the whole Q1 name-based classification. Measured: `*04water1` arrived with
`basematerialflags 0x40` (a plain wall) instead of `0x44550`, and `*lava1` with
`supercontents SOLID|OPAQUE` instead of `LAVA`. So from the day this pack was installed,
`r_wateralpha`, `r_wateralpha_force`, `rt_metal_liquids`, `r_lavaglow`, `r_lavaboil`,
`rt_metal_lavaemissive` and `rt_metal_lavalights` all did nothing, liquids cast shadows they
should not, and they stopped scrolling. Fixed engine-side by **`m5_liquidflags`** (default 1);
the pack is unmodified and needs no action. **Add this to the checks for any future texture
pack**: for a replaced `*`-prefixed texture, confirm the material flags and supercontents
survive — a pack that quietly turns lava into a wall looks completely normal in a screenshot.

### The normal-map add-on stays out

`rt_metal_walllight > 0` forces `r_fullbright 1` on the raster pass (`cl_screen.c`), so world
surfaces draw as flat albedo with the RT term multiplied over them and normal maps are never
consulted. At `walllight 0.5` the 354 MB add-on would be inert. It only becomes worth having
if wall-lighting is ever turned off.

---

## Game — Arcane Dimensions v1.80p1 (`ad/`)

Installed 2026-08-20 into the repo-root `ad/` gamedir (gitignored, like the mission-pack
gamedirs — but real files rather than symlinks, since AD is not a Steam product).

- **Source**: `ad_v1_80p1final` (October 2020), Seb's own download; 792 MB —
  `pak0.pak` (270 MB), `pak1.pak` (537 MB), `pak2.pak` (22 MB, the 1.80p1 patch),
  `quake.rc`, plus the readme, credits, documentation and changelog text files, all kept
  in `ad/`. The `.def`/`.fgd` files are map-editor definitions and were not installed.
- **Author**: Simon "Sock" O'Callaghan and the AD team — the full roster is in
  `ad/ad_v1_80_credits.txt`.
- **Licence**: QuakeC source under GPL v2-or-later; assets non-commercial,
  free-of-charge redistribution only, readme must accompany them
  (`ad/ad_v1_80_readme.txt`). Nothing from it may ever be committed to this repo —
  `/ad/` is gitignored.
- **Engine support**: the readme names DarkPlaces (2018 build) as a supported engine.
  Verified here 2026-08-20: gamemode `GAME_AD` was already in `com_game.c`; mounts as
  `id1 m5 ad` from the Mission Packs menu; its own progs (crc 10963) and its CSQC HUD
  both load; its weather and per-map fog run through DarkPlaces' own extensions
  (`+ DP features ( FOG SURF RAIN SNOW )` on every map load).

### Two deliberate local edits, both in `ad/quake.rc`

**1. The archived-cvar trim.** AD's shipped `quake.rc` sets archived cvars and binds four
keys, and under `m5_sharedconfig` anything archived from inside a pack follows you back
into every other game. Measured before the edit: one AD session overwrote `crosshair`, set
`cl_beams_polygons 0` — killing the M5 thunderbolt in stock Quake permanently — and wrote
`bind i/h/r/k` into the shared config. The offending lines are commented out in place,
each with the reason, and the fix was verified fail-first (original rc bleeds, trimmed rc
does not; a sentinel `crosshair 3` survives the round trip).

**2. `temp1 1024` → `temp1 3072`** (2026-08-21). AD's dev helpers — translucent direction
arrows (`progs/misc_corner1/2.mdl`) and spinning marker diamonds (`progs/misc_broken.mdl`)
— are gated in AD's QC on `developer`, and this engine's shared config archives
`developer 1` for our own diagnostics, so the whole debug layer drew during normal play.
Bit 2048 turns them off independently of `developer`; 1024 keeps AD's particle system on.
This is AD's own documented worked example, commented in that same file. `temp1` is
`CF_SERVER` only and never archived, so it cannot leak into stock Quake. Verified
fail-first on ad_e1m1: `temp1 1024` spawns **17 arrow and 22 marker entities**, `temp1
3072` spawns **zero**. Takes effect at map load — helpers already spawned are never
removed.

**If AD is ever reinstalled from a fresh download, re-apply BOTH edits** — `ad/` is
gitignored, so a fresh copy loses them and the bolt dies and the arrows come back.

### Not expected to work

AD's QSS-specific particle effects fall back to DarkPlaces' own paths (the readme is
explicit that only QuakeSpasm-Spiked has the full particle system). `saved1` (sprite
particles for QSS/FTE) is left at AD's shipped 0.

---

## Weapon model — Dissolution of Eternity's plasma gun (`m5/progs/v_plasma.mdl`)

> **Not in the public build (2026-09-19).** The model is Dissolution of Eternity's own file, so `RELEASE_PUBLIC=1` never ships it. The engine sets the read-only `m5_hasplasma` at every map start and the QuakeC falls back to `progs/v_light.mdl` when it is 0 — tested on a bare install (id1 paks only): ball launched, `weaponmodel progs/v_light.mdl`, no missing-model error.

Copied 2026-09-06 out of `rogue/pak0.pak` (Seb's own Steam copy of Dissolution of
Eternity, already mounted here as the `rogue` gamedir) into `m5/progs/`, as the VIEW MODEL
for the M5 ball-lightning weapon (`qc/m5ball.qc`, `m5_balllightning`). Seb's word: *"use the
thunderbolt gun as a template ... Packs might have one we can steal?"* — the plasma gun fires
plasma balls in its own pack, so it is the thematic fit; a recoloured copy of `v_light.mdl`
is the fallback if his eye prefers the thunderbolt's silhouette. 56,204 bytes,
sha256 `f075be7a…c51a9682`; one skin, 172 frames.

- **Licence**: id Software / Rogue Entertainment commercial content, the same standing as
  the rest of the `rogue/` gamedir — for Seb's own installation only, never committed
  (`/m5/*` is gitignored). The release script ships `m5/` alongside the packs, so a release
  built from this machine carries it exactly as it carries the pack itself.
- **Where it is referenced**: `qc/world.qc` precaches it unconditionally (precache order is
  modelindex order, so it cannot be cvar-gated), and `W_SetCurrentAmmo` names it for
  `IT_M5BALL`. If the file is absent the engine warns at map load and the ball gun draws
  nothing in hand; the weapon still works.
- Inside the `rogue` gamedir itself the m5 progs are replaced by the pack's own, so the
  ball weapon does not exist there and the pack's own copy is what its plasma gun uses.

## Removing it all

Delete `m5/progs/` (which also removes the borrowed `v_plasma.mdl`), `m5/maps/` and `m5/QRP_map_textures_v.1.00.pk3`. Nothing else is touched;
`m5/progs.dat` is this fork's own build and is not part of any pack. Each can go
independently — models and textures are separate installs. Arcane Dimensions is `ad/` at the
repo root plus `m5/m5pack_ad.cfg`; delete both and it is gone.
