# QuakeM5 — settings reference

Every setting this fork adds, what it is for, and where to find it. Values shown are the
defaults. Settings marked **console-only** have no menu row — open the console with `~` and
type the name (with no value) to see its current value and help text.

Almost everything here is saved into your config automatically when you quit. The handful
that are not are marked *per-session*.

**Quoting rule for multi-number settings:** any cvar that takes several numbers in one
string must be quoted in the console — `r_volumetric_wind "10 4 1.5"`. Unquoted, the
engine silently keeps only the first number. The colour settings sidestep this by being
three separate `_red` / `_green` / `_blue` cvars.

---

## The menu map

Rationalised 2026-08-03, then **halved 2026-08-09**: the tuning pages went from 184 rows to
89, and every surviving slider was *measured* to change pixels at a real configuration before
it kept its place. Removed rows keep their cvars — everything is still tunable from the
console, and this document lists it all. The guiding idea: the best setting is none; the
menus carry the controls a player sets by eye, the console carries the rest.

**Options** (22 rows) is a hub. Top half: the settings you actually change mid-game
(controls, video mode, console, reset, crosshair, mouse speed, invert mouse, field of view,
always run, show framerate, sound volume, music volume). Bottom half: one link per topic.

| Options row | Opens | What that page is for |
|---|---|---|
| Idle View Sway | *(toggle)* | The view drifts gently once you have stood still and stopped moving the mouse. |
| Brightness and Gamma | *(page, 5 rows)* | The master brightness, gamma and contrast trims, scene brightness. |
| M5 Quality | *(cycles)* | One control trading visuals for frame rate across the whole volumetrics + RT stack. |
| Effects and Particles | *(page, 7 rows)* | Particles, blood, decals, and the see-through-water pair. |
| Lighting and Bloom | *(page, 5 rows)* | Dynamic lights, coronas, bloom, glow brightness. |
| Lightning Gun | *(page, 10 rows)* | The enhanced thunderbolt's by-eye knobs. |
| Volumetric Fog | *(page, 12 rows)* | Air murk, liquid murk, ground fog, and the two RT fog sliders. |
| RT Shadows (Metal) | *(page, 7 rows)* | The ray-tracing sidecar's look knobs. macOS only. |
| M5 Fun Mods | *(page, 16 rows)* | The gameplay mods, the ball lightning switch and the weapon feel (SEPTEMBER2 G). |
| Browse Mods | *(page)* | Mount a different gamedir. |

**Honest controls.** A greyed row means *this will not change anything in your current
configuration*, and each page prints the reason at its foot. Since 2026-08-09 greying is
also **real** on the tuning pages: a greyed row no longer changes its cvar when you press
left/right — it used to, silently. The dependencies that exist:

- **Water Alpha (opacity)** is greyed unless **Force Water Alpha** is on. On every stock id1
  map the engine ignores `r_wateralpha` outright, because vanilla vis never gives a water leaf
  a line of sight to open air, and DarkPlaces takes that as "this map was not built for
  see-through water". The force row used to be console-only, which made the slider above it
  the most misleading control on the menus: you could drag it all day and nothing happened.
- **RT Liquid Lighting** is greyed unless the water is genuinely see-through (the pair above).
- **Doom Shotgun, Gore Preset, Lightning Ignites** and **Horde Mode** are greyed when the `m5`
  gamedir is not the primary one — mount a mission pack or another mod and its `progs.dat`
  replaces the M5 QuakeC lane. Bullet time, photo mode and the CPM half of the movement preset
  are engine-side and work in every game.
- On the Lightning Gun page, **Fog Scatter** needs `rt_metal` on (the fog weight rides the
  sidecar's light upload), and **World Light** needs *either* the sidecar *or* the stock
  rtdlight path (Dynamic Lights set to "Lit walls") — outside both, a dynamic light reaches
  no pixel.
- On the Video Options page, **MetalFX Upscale** is greyed unless the selected renderer is
  Metal. It is a three-state cycle (Off / Spatial / Temporal), not a checkbox, and it skips
  **Spatial** when Render Scale is 100% — a 1:1 spatial upscale is a pointless copy, while a
  1:1 *temporal* pass is anti-aliasing and is offered. No state the row can reach is a dead
  one, which is the misleading-control class this list exists to prevent.

**The one-shot presets are gone** (2026-08-09). The three on Effects and Particles overwrote
24 cvars each and silently forced Water Alpha opaque; the four on Lighting and Bloom overwrote
its first nine rows. The M5 Quality row is the fork's one preset, and it touches only the
twenty-eight performance levers it names.

---

## Mission packs — Single Player → Mission Packs

Five add-ons sit alongside Quake on the Single Player menu:

| Entry | Gamedir | Notes |
|---|---|---|
| Quake | — | id1 + the M5 QuakeC. |
| Scourge of Armagon | `hipnotic` | The 1997 mission pack, complete. |
| Dissolution of Eternity | `rogue` | The 1997 mission pack, complete. |
| Dimension of the Past | `dopa` | The 2016 add-on, from the rerelease packaging (BSP2 maps). |
| Dimension of the Machine | `mg1` | The rerelease add-on. Its `horde*` maps load but do not play — that mode is engine-side in the rerelease and does not exist here. |
| Arcane Dimensions | `ad` | The 2020 total conversion, v1.80p1. Its own game code replaces the M5 QuakeC lane entirely — see below. Its CSQC HUD, weather and per-map fog all work here. |

An entry is greyed and marked *absent* if its gamedir is not on disk, so the page only
ever offers what you actually own.

**What comes with you.** Each pack mounts as `m5 <pack>`, which puts the search order at
**pack → m5 → id1**. So the pack's own `progs.dat`, models and sounds win, while the M5
content — the AMI model pack, the QRP textures — still fills in underneath for everything
the pack does not carry itself. Every engine-side M5 feature (ray tracing, the volumetric
fog, the thunderbolt, the red glow, lava) is unaffected, because none of it lives in a
gamedir.

**What does not.** The M5 *QuakeC* mods — horde mode, the Doom shotgun, the gore presets,
the lightning burn, the Scrag venom — need `m5/progs.dat`, and a pack replaces it. Those
rows grey out on the M5 Fun Mods page while a pack is mounted, and say why. Bullet time,
photo mode and the CPM half of the movement preset are engine-side and keep working.
Arcane Dimensions is the extreme case: it is a total conversion with its own weapons,
monsters and game systems, so the whole M5 gameplay lane stands down there by construction
— while every engine-side feature (RT, the murk, the temporal scaler, the thunderbolt)
still applies.

**Switching** restarts the renderer and re-execs your config (the pack's own `quake.rc`
runs, if it has one), and disconnects any game in progress first. It does not need a
restart of the engine. `gamedir m5 hipnotic` from the console does exactly the same thing.

### Per-pack lighting and fog

The packs were "all over the place" until 2026-08-04: Armagon too dark, Dimension of the
Machine sun-baked and glowing orange. Three things were wrong, and all three fixes are inert
in stock Quake.

| Setting | Default | What it does |
|---|---|---|
| `m5_lightcolor255` | 1 | Read a map light's `_color` as 0–255 when any channel exceeds 1, the way ericw-tools and the rerelease write it. DarkPlaces' own convention is 0–1 and it never detected which, so every modern map's lights arrived **up to 255× too bright and hue-biased**. This was almost the whole of "glowing orange". Stock Quake, Armagon and Dissolution contain no `_color` lights at all, so it cannot change them. |
| `m5_packlight` | 1 | Pull a pack map's light *level* towards stock Quake's, 0–1. Under RT wall lighting the map's own light entities **are** the scene's lighting, so a mapper who used dimmer or tighter lights than id's simply renders darker — Armagon at 0.64/164 against e1m3's 0.71/207. Level only; the pack's authored colour is untouched. Never applies in stock Quake, which is the baseline. |
| `m5_packbrightness` | 1 | Per-pack brightness trim, multiplied into the `r_brightness` curve. Set by the look files below rather than by hand. Never applies in stock Quake. |
| `m5_packfog` | 1 | When a map sets fog and the murk is running, let the murk take the map's fog **colour** and stand the classic fog pass down instead of drawing both. Density stays yours. Dimension of the Machine sets map fog on 7 of its 25 maps; Arcane Dimensions sets it on most of its maps (its game code drives DarkPlaces' own fog directly, so this matters far more there). |
| `m5_sharedconfig` | 1 | Save one config for every game, in `m5`, instead of a separate one per gamedir. |

**The look files.** `m5/m5pack_<gamedir>.cfg` is exec'd automatically after your shared config
whenever that pack is loaded. They are plain text, they are meant to be edited, and deleting
one simply leaves that pack uncorrected. **They may only set `m5_*` settings** — the config is
shared now, so a pack writing any other archived cvar would follow you back into Quake the next
time you quit inside it.

Shipped values, and the frame brightness each pack lands at relative to stock Quake:

| Pack | before | after | look file |
|---|---|---|---|
| Armagon | 0.47 | ~1.0 | `m5_packlight 1`, `m5_packbrightness 1.08` |
| Dissolution | 1.60 | ~1.0 | `m5_packlight 1`, `m5_packbrightness 1.05` |
| Dim. of the Machine | 9.4 | ~1.0 | `m5_packlight 1` |
| Dim. of the Past | 14.5 | 1.16 | `m5_packlight 0` — see below |

Dimension of the Past is the one the automatic correction gets wrong, and the reason the look
files exist: its light budget asks for a 2.4× lift, but its albedo and fullbright coverage are
already well above id1's, so the light statistic mispredicts it. With the correction off it
sits at 1.16 and needs nothing.

**Arcane Dimensions** ships `m5_packlight 0` for the same reason as Dimension of the Past: it
is professionally lit (light budgets 118–149 against e1m3's 146) and its gloom and blaze are
the design, not a period house style to correct. Its `_color` lights are the other half of the
story — ad_tears alone has **all 708 of its lights written in 0–255 units**, every one caught
by `m5_lightcolor255`; without that heuristic the map would render as raw fullbright albedo.
Two more AD-specific notes: its flame, candle and lantern models are on the RT light-core list
(`rt_metal_lightcores`), so its fixtures glow as authored instead of self-shadowing to black;
and the shipped `ad/quake.rc` has been trimmed of the lines that would have leaked through the
shared config — AD's own copy binds four keys and switches off `cl_beams_polygons` (which
would have killed the M5 thunderbolt *everywhere*, permanently, after one AD session). The
trimmed lines are commented in place in `ad/quake.rc` with the reasons.

`developer 1` prints an `M5 maplights` line at every map load — light count, mean colour, mean
radius, how many colours were rescaled from 0–255, and the map's light budget. That is the
number `m5_packlight` works from, and the fastest way to see why a map looks the way it does.

**Installing them.** The packs are your own Steam data and are not in the repository. Each
lives in a directory beside `id1` holding nothing but symlinks — `hipnotic/pak0.pak` and
`hipnotic/music` pointing into `steamQuakecommon/`, and so on — so they cost no disk space
and cannot be committed. The classic `hipnotic` and `rogue` directories have no music of
their own (they used CD audio), so their `music` links point at the rerelease copies.

---

## Idle view sway — Options → Idle View Sway

Stand still, stop moving the mouse, and after a moment the view drifts gently — the same
motion the end-of-level camera has. Touch anything and it collapses straight back out.

Quake shipped the whole apparatus for this and then switched it off for release: the three
`v_i*_cycle` / `v_i*_level` sine terms have always been in the code, driven by `v_idlescale`,
and nothing in the engine ever wrote `v_idlescale`. Original Quake did use it in exactly one
place — it forced the value to 1 for the intermission camera — and DarkPlaces lost even that
when it restructured the function. Both are back, the intermission at full strength.

| Setting | Default | What it does |
|---|---|---|
| `v_idlesway` | 1 | The master. 0 restores the dead-still view. |
| `v_idlesway_delay` | 1.5 | Seconds of standing still, with no buttons and no mouse movement, before it starts. |
| `v_idlesway_ramp` | 2 | Seconds to ease in to full strength. It always eases **out** in a quarter of a second, so it can never fight your input. |
| `v_idlesway_scale` | 0.6 | Strength, relative to the classic `v_idlescale 1` drunken view. At the default the peak excursion is 0.37°. |
| `v_idlescale` | 0 | The classic constant version, unchanged. The larger of the two wins, so setting this still gives you a permanently drifting view. |

The shape comes from `v_ipitch_cycle` / `_level` and their yaw and roll counterparts, which are
untouched — so anything written for stock Quake still applies.

**It cannot move your aim.** The sway is added to the local view angles used to build the view
matrix, never to `cl.viewangles`, which is what the server receives. It is also suppressed
while you are dead and in photo mode, which flies its own camera.

`v_idlesway_test` steps the ramp headlessly and prints the rise time, the fall time and the
peak angular excursion; `tests/smoke.sh` asserts it.

---

## Brightness — Options → Brightness and Gamma

**`r_brightness` (0–1, default 0.5) is the one control to reach for.** It is shown as a
slider from 0 to 1 and it scales the gamma curve, so it dims the whole composited frame —
fog, glow and all — rather than the scene lighting alone. That distinction matters: the murk's
scattered colour is authored, not light-driven, and does not pass through the scene-lighting
multiply, so dimming *that* instead would leave the fog looking like it was adding light with
no source behind it.

| Slider | What it should look like |
|---|---|
| 0.00 | Almost unplayable. Inky black on an OLED; you can barely make anything out. |
| 0.15 | Very dark and gloomy, inky corners and shadows. |
| 0.25 | Moody and atmospheric, but comfortably playable if you know the map. |
| **0.50** | **The stock 1996 look — and exactly no change at all.** |
| 1.00 | Clearer and more lit than Quake ever was. |

The midpoint multiplies the gamma value by exactly 1.0, so 0.5 is bit-for-bit the picture the
engine drew before this control existed. Measured on the demo5 bed (e1m3, a deliberately dark
corridor, with the RT and fog features off), frame mean luminance runs **1.5 / 3.1 / 5.4 /
14.8 / 54.4** across those five settings. Your own numbers will be much higher — wall lighting,
the fog and the QRP textures all brighten the picture — so treat the table as the *shape* and
set the value by eye.

The knee at 0.15 is measured rather than guessed: a single straight ramp down to zero put 15%
and 25% within a whisker of 0%, because a power curve collapses a dark scene long before the
slider reaches its end.

The three rows under it are trims, and each one's slider range is exactly its setter's
clamp — the same cvars used to appear on two or three pages over different ranges, so each page
showed the other's value pinned at an end stop:

| Row | Cvar | Range | What it does |
|---|---|---|---|
| Gamma Trim | `v_gamma` | 0.5–2 | The classic inverse gamma, composed with the master. Leave at 1 unless your display needs it. |
| Contrast | `v_contrast` | 0.2–3 | Brightness of white. Above 1 also increases colour saturation. This row used to be labelled *Brightness* on the Options page. |
| Scene Brightness | `r_hdr_scenebrightness` | 0.1–4 | Multiplies the **lighting** handed to surfaces, before gamma. Unlike the master it does not touch the fog's own colour, which is why it is a trim here rather than the main control. |

Console-only since 2026-08-09: `v_brightness` (a black-lift for displays that crush black —
on an OLED leave it at 0) and the whole colour-level family (`v_color_enable` plus the twelve
per-channel `v_color_black/grey/white_*` sliders — a display-calibration tool; if a
console-set profile is active the two curve rows grey and the page says why, and adjusting
them switches the profile off). The Reset row still writes every one of these, so a stray
colour profile is always recoverable from the menu.

**Two things worth knowing at the dark end.** `rt_metal_ambient` (0.15) is a flat fill added so
that surfaces facing away from every light do not go pure black in wall-lighting mode — it is
light with no source, by design, and it is the one thing still holding your blacks up if you
want them absolutely dead. And the display output is 8-bit, so a very dark picture has few
distinct levels to work with; banding there is the drawable's own precision, not the gamma
table's.

---

## M5 Quality preset — Options → M5 Quality

One row that trades visuals for frame rate across the whole volumetrics + RT stack.
Enter or right-arrow cycles **Superfast → Fast → Good → Better → Best → Ultimate** (left goes back); each press
applies the tier's values to the thirty-four performance settings below in one go. The row
reads **Custom** whenever your live values match no tier — hand-tuning any of the
thirty-four flips it back to Custom — and from Custom the first press lands on Better, the
tier nearest the reference look.

What it never touches: every look setting (colours, densities, winds, heights), and the
eleven BEAUTY extras — `rt_metal_gi_ao`, `rt_metal_contact`, `rt_metal_fog_liquidlight`,
`m5_torch_embers`, `r_skylightning`, `r_caustics` and the five `cl_particles_*` — which
stay at their defaults on every tier (see the 2026-09-19 note below for why). Since the
**2026-09-19 AA retier** the tiers DO own the master switches (`r_volumetric`, `rt_metal`)
and, new that day, the antialiasing pair (`r_smaa`, and `r_fxaa`/`r_fxaa_post` pinned off).

**Every non-Stock tier now rasters at Render Scale 37.5% and traces the RT term
pixel-exact to it** (`rt_metal_scale 1`), with MLAA at native after the upscale. That is
the whole shape of the table: the raster is no longer a look lever, because the term
follows it, and the ladder is feature-only. Setting `r_viewscale` by hand still works; the
row then reads Custom, honestly.

**Ultimate is `exec rich.cfg` wired into the menu** — Seb's own 2026-09-19 verdict, "it
looks best (i.e. sky jaggies gone) if I select Ultimate in the menu, and then exec
rich.cfg", in one click. The eleven BEAUTY extras are the one thing it does not take from
that recipe.

**HISTORICAL — superseded 2026-09-19. The 2026-08-14 redo of the lower three tiers.** Their first cut let the fog and
lighting buffers shrink with Render Scale (the fog buffer was 320 pixels wide at Good
against Best's 480), and because the ray-traced fog and lighting layers stop at geometry,
every silhouette they touched stepped — the "one layer looks lower-res than another" look.
The lower tiers now **hold those buffers at Best's absolute size** (fog 480×270 on every
tier; lighting 640×360, Fast 480×270) and spend Render Scale, step count, stride and shadow
samples instead — near-free, because fog cost is per fog pixel. Fast runs the RT fog kernel
like every other tier now (the god-ray swap and its shader-rebuild pause on crossing Fast
are gone), and Better carries fog history 0.8 to buy back its step-count drop.

<!-- docsync:tiertable BEGIN -- generated by test/docsync.py, do not edit by hand -->
| Setting | Stock | Superfast | Fast | Good | Better | Best | Ultimate |
|---|---|---|---|---|---|---|---|
| `m5_stock` | **1** | **0** | **0** | **0** | **0** | **0** | **0** |
| `r_volumetric` | **0** | **1** | **1** | **1** | **1** | **1** | **1** |
| `cl_particles_quake` | **1** | **0** | **0** | **0** | **0** | **0** | **0** |
| `r_lerpmodels` | **0** | **1** | **1** | **1** | **1** | **1** | **1** |
| `r_volumetric_steps` | **8** | **8** | **12** | **16** | **16** | **16** | **16** |
| `r_volumetric_scale` | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 |
| `r_volumetric_skyfog` | 0.8 | 0.8 | 0.8 | 0.8 | 0.8 | 0.8 | 0.8 |
| `rt_metal` | **0** | **1** | **1** | **1** | **1** | **1** | **1** |
| `rt_metal_lightsample` | **0** | **0** | **1** | **1** | **1** | **1** | **1** |
| `rt_metal_fog_beams` | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 |
| `rt_metal_samples` | 2 | 2 | 2 | 2 | 2 | 2 | 2 |
| `rt_metal_shadowlights` | **1** | **3** | **3** | **3** | **3** | **3** | **3** |
| `rt_metal_scale` | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| `rt_metal_fog` | **0** | **1** | **1** | **1** | **1** | **1** | **1** |
| `rt_metal_shafts` | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `rt_metal_fog_steps` | **12** | **12** | **16** | **16** | **24** | **24** | **24** |
| `rt_metal_fog_stride` | **6** | **6** | **4** | **4** | **4** | **4** | **4** |
| `rt_metal_fog_scale` | **0.25** | **0.25** | **0.25** | **0.3333** | **0.3333** | **0.3333** | **0.444** |
| `rt_metal_shafts_samples` | **3** | **3** | **3** | **6** | **6** | **8** | **8** |
| `rt_metal_shafts_scale` | **0.25** | **0.25** | **0.25** | **0.5** | **0.5** | **0.75** | **0.75** |
| `r_viewscale` | **1** | **0.375** | **0.375** | **0.375** | **0.375** | **0.375** | **0.375** |
| `r_metalfx` | **0** | **1** | **1** | **1** | **2** | **2** | **2** |
| `r_metalfx_reactive` | **0** | **0** | **0** | **0** | **3** | **3** | **3** |
| `r_smaa` | **0** | **1** | **1** | **1** | **1** | **1** | **1** |
| `r_fxaa` | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `r_fxaa_post` | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `rt_metal_fog_filter` | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| `rt_metal_fog_history` | **0.7** | **0.7** | **0.7** | **0.7** | **0.6** | **0.6** | **0.6** |
| `rt_metal_fog_intensity` | 0.55 | 0.55 | 0.55 | 0.55 | 0.55 | 0.55 | 0.55 |
| `rt_metal_fog_residual` | 0.1 | 0.1 | 0.1 | 0.1 | 0.1 | 0.1 | 0.1 |
| `rt_metal_fog_froxel` | **0** | **0** | **0** | **0** | **1** | **1** | **1** |
| `rt_metal_fog_froxel_slices` | **16** | **16** | **24** | **24** | **24** | **32** | **32** |
| `rt_metal_lightsample_hybrid` | **0** | **0** | **3** | **3** | **3** | **3** | **3** |
| `rt_metal_gi` | **0** | **0** | **0** | **0** | **0** | **1** | **1** |
| `rt_metal_gi_rate` | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
<!-- docsync:tiertable END -->

(**Bold rows differentiate a tier; plain rows are identical on all six and exist to be
pinned.** `r_volumetric_steps`/`_scale` and the two `rt_metal_shafts_*` rows are inert while
the fog kernel is on — every tier — and are kept so an older configuration normalises onto a
tier cleanly. The plain rows are deliberate: those levers sat at their defaults, owned by no
tier, so nothing restored them after a console session moved them — the same drift that made
the screen-space god-ray tier unreachable. Clicking a tier now pins them; no tier's look
changes. This grid is **generated from `menu.c`** — `python3 test/perf/check-tiers.py
--markdown` — because it was hand-maintained until 2026-08-29 and had silently lost four
shipped rows.)

### 2026-09-19 — THE AA RETIER: the term follows the raster, and every column moved

Seb, on the configuration he had been reaching for by hand: *"Today it looks best (i.e. sky
jaggies gone) if I select Ultimate in the menu, and then 'exec rich.cfg'. This looks good,
and returns fps in the range 85–175 in gameplay."* His instruction was to make Ultimate be
that, and to adjust the rest of the tiers to suit.

**The finding it is built on** (2026-09-18 night). `rt_metal_scale` is a fraction of the
**viewport**, so the RT term follows `r_viewscale`. The jaggies were never the raster's
resolution — they are the *term* being magnified onto the raster: under wall lighting the
term **is** the scene lighting, and a magnified term ramps every silhouette over ~2 rows,
which is exactly the shape a morphological antialiaser cannot see. Measured on his own
`aaroof.dem`: untreated 0.362, `r_smaa` alone **0.384 (worse)**, `rt_metal_scale 1` alone
0.318, and **the pair 0.229**.

**The rule, and it is why the grid above looks the way it does:** the term must be
pixel-exact with the **raster** (`rt_metal_scale 1`), and MLAA (`r_smaa 1`) runs at native
after the upscale. Not "native raster" — the 09-18 ladder held `r_viewscale 1` throughout
and so could not separate the two.

**Why it is also cheaper.** Ultimate's old term was 0.375 × `r_viewscale` 1 = 720×405,
magnified 2.67× onto a 1920×1080 raster. The new one is 1 × `r_viewscale` 0.375 = 720×405
on a 720×405 raster — **the same trace, pixel for pixel**, with the raster 7.1× smaller.
Measured soaked, paired, three beds: **Ultimate as shipped 74.8 fps, the new Ultimate 145
(min 97) / 129 (min 70)**.

**The consequence, and Seb took it with his eyes open when the choice was put to him.** The
top of the ladder is now faster than the *old* Best (106.8), and MetalFX floors
`r_viewscale` at 0.375 — 1/0.3333 is exactly the 3.000 input-content scale limit the probe
reports, and one pixel of `ceil()` rounding the wrong way makes the scaler refuse silently
and fall back to bilinear. So every non-Stock tier rasters at 0.375, the ladder is features
only, and it spans about 1.3× where it used to span 3.2×. **The old 240/180/144/120/80 fps
targets are retired**: they belonged to a table whose most expensive lever has been removed,
and chasing them would only buy the jaggies back.

The rungs, top down: **Ultimate** is `rich` exactly (fog buffer 320×180, 32 froxel slices,
bounce light); **Best** is Ultimate at a 240×135 fog buffer; **Better** is Best without
bounce light and at 24 slices; **Good** drops to the spatial scaler and the 2D fog history —
the biggest lever left, and one that no longer costs the edges because MLAA does that work
now; **Fast** takes a 180×101 fog buffer and 12 GL-march steps; **Superfast** drops the
stochastic fog light pick and its hybrid. **Stock is untouched.**

**Three cvars the table did not own and now does**, all the same defect — a value the table
does not own is a value that drifts: `r_smaa` (archived since it shipped, zero hits in
`menu.c`), `rt_metal_fog_froxel_slices` (console-only and *unarchived* until this change;
it is the live fog march count under the froxel, where `rt_metal_fog_steps` is inert), and
`r_fxaa`/`r_fxaa_post`. **Note what the last one costs you:** clicking any tier now writes
`r_fxaa 0` over an archived 1. That is deliberate — `r_smaa` supersedes `r_fxaa_post`
outright, and `r_fxaa` is the *render*-resolution pass, which at 0.375 only softens the
scaler's input — and `exec fxaa_on.cfg` puts it back.

**What the retier deliberately did NOT take from `rich.cfg`:** the eleven marginal BEAUTY
extras. `rich` cuts them because they fire per *term* pixel and cost 17% at a 2.07 Mpx
native term — but the 09-18 value audit measured the whole set at **3.1%** at a 720×405
term, which is exactly what every tier now has. Eleven approved looks are not worth 3%, and
pinning them off would delete the BEAUTY round from every tier. `exec rich_bare.cfg` is the
one-command A/B, and `tier_ultimate_bare` prices it on the bench.

**What is owed:** a soaked, paired, exclusive-fullscreen block over these seven columns.
Only the two figures above are measured on the new table; every rung size in this section is
a projection from the 09-18 value audit and says so. The live control arms are the phase-AA
rows at the foot of `test/perf/levers.tsv`; every pre-retier arm is **frozen** rather than
deleted (see `FROZEN` in `check-tiers.py`) because it describes a table that no longer
exists.

**2026-08-21: the owed bench lands, and Good goes back to spatial.** The soaked fullscreen
suite (out-tiers3, every row's geometry witnessed at 1920×1080, baseline rounds agreeing
within 0.1–3.9%) measured temporal's real cost at these settings at **22–25%**, not the
~15% the windowed arithmetic had assumed. Temporal-Good measured 142–153 average against
its 180 target — a miss on every bed — so per the standing instruction Good is spatial
again, with Fast's 3×3 filter and no mask (the mask rides the temporal scaler). **Better
measured 136–147 against its 144 target** — it misses on three of the four beds by 4–8 fps
— and is *kept* temporal because Seb named it for temporal; whether it follows Good down
to spatial (which would put it near the spatial control's 164–180) is his call. Best
clears its 120 target on every bed. The full measured grid is below.

**2026-08-20: the top three tiers take the TEMPORAL scaler, and Fast keeps the spatial one.**
Seb's verdict on the arc, in his words: *"taa_on looks great"*, *"fog_filter 2 looks
amazing"*, *"Best should use these settings"* — so `r_metalfx 2` (temporal), its
**reactive mask** (`r_metalfx_reactive 3`, which stops moving light and particles
smearing) and the **5×5 fog filter** land on Best, Better and Good. Best is now his live
configuration exactly. Two consequences worth stating. `r_metalfx_reactive` became
**archived** with this change: an unarchived lever resets to 0 on the next launch and the
tier would read Custom every boot — which is exactly what his config did before the
retier. And **Fast keeps the spatial scaler**: temporal's motion pass costs 1–2 ms of the
frame (3.5–6.5 ms on entity-heavy stretches), which is real money against Fast's ~4 ms
budget, and Fast's whole job is the frame that cannot carry it; it takes the 3×3 filter
instead of the 5×5, which measures **identical** on the instrument (weave 0.00418 vs
0.00415 against 0.00571 with the filter off — a [1 2 1] has zero response at Nyquist, so
once that band is gone a wider kernel has nothing left to take) and costs ~0.2 ms less on
the fog stage. (The re-measure this paragraph originally owed landed on 2026-08-21 — the
grid below — after a windowed 2026-08-20 attempt came back presentation-throttled, every
arm pinned at exactly 120 fps, and was discarded.)

**2026-08-16: Fast's fog buffer drops to 320×180.** The depth-aware fog upsample
(`rt_metal_fog_upsample`, on by default) keeps fog silhouettes clean at fog buffers the
old bilinear magnification stepped on, and Seb approved a 320×180 buffer with it "for
Fast" (not for Good or Better, which keep 480×270). Fast's `rt_metal_fog_scale` is 1/3
because Fast renders at Render Scale 50%: 0.333 × 0.5 × 1920 = the 320 pixels he judged,
not a smaller buffer. Measured (windowed, interleaved, two rounds each): the fog kernel's
GPU stage halves — 1.93→0.91 ms on the start-hall bed, 1.58→0.59 ms on e1m3 — and the
fog-heavy bed's average goes 250→317 fps with the one-second minimum 224→268. (The
fullscreen soaked suite this owed ran on 2026-08-17 and again, post-retier, on
2026-08-21 — the grid below.)

Measured 2026-08-21 on the Apple M5 at 1080p, exclusive fullscreen (every row's own
`Video Mode:` line witnessed at 1920×1080 — zero flagged rows), at the machine's
**soaked** plateau (ten-minute soak first; sustained load sags the M5 ~35% from a cold
start before it levels out, and an hour of real play runs soaked, so soaked is what is
quoted). Median of two interleaved rounds per bed, average / one-second minimum, across
the four rolling beds (e1m3 · e3m1 real play · e1m1 dark · start lava hall); the
same-config baseline rounds agreed within 0.1–3.9% on every bed (results in
`test/perf/out-tiers3/`):

| Tier | e1m3 | e3m1 (real play) | e1m1 (dark) | start (lava hall) |
|---|---|---|---|---|
| Best (temporal) | 135 / 106 | 128 / 94 | 128 / 92 | 126 / 100 |
| Better (temporal) | 147 / 116 | 137 / 96 | 138 / 98 | 136 / 112 |
| Good (temporal — rejected, see below) | 153 / 108 | 143 / 99 | 145 / 105 | 142 / 114 |
| Fast (spatial) | 273 / 185 | 242 / 130 | 255 / 144 | 287 / 214 |
| Best at the spatial control | 180 / 134 | 164 / 111 | 166 / 112 | 168 / 140 |

Read against the targets (Best ≥120, Better 144, Good 180 averages): **Best clears
everywhere**; **Better misses on three of the four beds by 4–8 fps** and is kept temporal
on Seb's naming — his call whether it follows Good down to spatial (which would put it
near the spatial control's 164–180); **temporal-Good missed everywhere by ~35 fps and is
spatial again** — that is the Good the lever table above ships. The temporal-vs-spatial
control pair puts temporal's real cost at these settings at **22–25%**, not the ~15% the
windowed arithmetic assumed. Fast's one-second floor on the entity-heavy beds remains the
game's own heavy moments, not the GPU.

**2026-08-29: the owed row lands, and bounce lighting comes off Good.** The suite was
re-run on a quiet machine after a fresh boot and a ten-minute soak, now with one-bounce
GI in the table (`out-tiers4`, four beds × two interleaved rounds, **every row witnessed
at exclusive 1920×1080 — zero flagged geometry**). Median average / one-second minimum:

| Tier | e1m3 | e3m1 (real play) | e1m1 (dark) | start (lava hall) | target |
|---|---|---|---|---|---|
| Best (GI on) | 127 / 78 | 115 / 70 | 112 / 72 | 112 / 70 | ≥120 |
| Best, GI off (control) | 133 / 76 | 121 / 66 | 119 / 78 | 116 / 59 | — |
| Better (GI on) | 135 / 94 | 122 / 69 | 120 / 80 | 118 / 70 | 144 |
| **Good (GI off — shipped)** | **207 / 138** | **176 / 112** | **178 / 116** | **180 / 96** | **180** |
| Good with GI (rejected) | 187 / 108 | 160 / 90 | 161 / 103 | 161 / 85 | 180 |
| Fast (GI off) | 279 / 136 | 235 / 123 | 243 / 131 | 277 / 183 | — |

Three readings, in order of how firmly they are settled.

**Good drops bounce lighting, and it is not close.** With GI it reads 160–187 against its
180 target — a miss on three of the four beds; without it, 176–207, which *is* the target.
GI costs Good **15–20 fps (9–10%)**, a bigger slice than it costs Best because Good's trace
runs at the same 641×361 while its frame is shorter, so a fixed ~0.6 ms is a larger
fraction. This is the temporal-Good precedent applied exactly: a tier that misses its
stated target loses the lever. It also settles the row owed since 08-21 — the shipped Good
*is* spatial-Good, measured at 176–207 against the 185–210 that was expected from the 08-17
figures, so the expectation was a little optimistic but the tier clears.

**Best keeps it, on the look rather than the number — Seb's call.** Best reads 127/115/112/112
against ≥120: it clears on e1m3 and misses the three heavy beds by 5–8 fps, of which GI is
4–7. He QA-passed this look the same day, and taking bounce lighting off the tier he plays
to recover ~6 fps is a trade only he can make.

**The whole table has drifted below its targets, and GI is not the main reason.** The GI-off
Best control measures 116–133 today on the same levers that measured 126–135 on 08-21 —
so several fps have gone to everything that landed in between (the fog light pick became a
default on 08-28, chief among them). **Better now misses 144 on all four beds** (135/122/120/118)
with GI accounting for only about 6 of a ~25 fps shortfall. Chasing that one lever at a time
would be the wrong move: the tier *targets* want their own session, either re-cut against
what the stack now costs or bought back with a structural lever (GI at half rate is the
candidate, GIARC G4 item 2). Recorded rather than papered over.

---

## RT Shadows (Metal) — Options → RT Shadows (Metal)

The ray-traced lighting sidecar. Traces the map's real lights on the GPU's ray-tracing
hardware and composites soft shadows (or full lighting) over the scene.

The page's eight rows (2026-08-09, plus Weapon Lighting on 2026-08-21 — the look knobs,
each measured live at a wall-lit bed):

| Setting | Default | What it does |
|---|---|---|
| `rt_metal` | 0 | Master switch for the whole RT sidecar. |
| `rt_metal_softness` | 0.12 | Penumbra width — how large an area each light pretends to be. Bigger = softer shadow edges. |
| `rt_metal_darkness` | 0.40 | Shadow darkness floor (0 = deepest shadows, 1 = no darkening). Over the lightmaps it is how dark a fully shadowed surface gets; in wall-lighting mode it floors the dominant light's shadow term, with 0 matching the old full-depth shadows. |
| `rt_metal_color` | 0.6 | Over the lightmaps: strength of the coloured flare when a dynamic light (explosion, muzzle flash) is visible. In wall-lighting mode: saturation of the RT lighting (1 = neutral, below greys the light, above exaggerates hue). |
| `rt_metal_walllight` | 0 | **Full Wall Lighting**: 0 = RT shadows composited over the baked lightmaps; above 0 the RT computes ALL lighting per pixel and this is its intensity. |
| `rt_metal_ambient` | 0.15 | Ambient fill used in wall-lighting mode so unlit corners never go pitch black. |
| `rt_metal_lmax` | 2.5 | A **soft ceiling on the wall-lighting term** (2026-08-16). The term has a floor and had no ceiling: a wall beside its own light reads 5–7 against a frame median of 1–2, and multiplied into fine texture detail — the texels round a switch, a fiend's dorsal ridge — that goes far past white, which HDR shows above white and MetalFX's HDR scaler sharpens into the bright single pixels QA saw at silhouettes and fixtures. Above the knee the term rolls off smoothly, hue kept, and can never exceed knee × 5/3; below it nothing changes. Measured on demo15: knee 2.5 leaves the median untouched (p50 1.0–2.0, p90 2.6) and takes the hottest texels 7.5 → 3.7; the switch's saturated pixels 35 → 1, the corpse's bright ridge pixels 151 → 99 (2.0: → 39, a visibly flatter look). Console-only; 0 = no ceiling, the old look exactly. |
| `rt_metal_lightcap` | 0.75 | **Per-light ceiling** (SEPTEMBER S2, 2026-09-02; **default 0.75 since Seb's pass the same day**): a soft knee on a MAP LIGHT'S uploaded intensity — its `light` value / 256 per channel, so 1 = a `light 256` entity — with `rt_metal_lmax`'s shoulder shape (hue kept, never above knee × 5/3). Built for e1m2's water hall, which its mapper lit with three `light 800`, an 850 and a 700 (no other id1 map goes above 650): each uploads at 3.1 per channel where a normal 200 light is 0.78, and one of them within 300 units pins a wall flat at the lmax ceiling on its own. Measured on Seb's demo20, the RT term's whole-frame MEDIAN in that corridor: 3.82 at 0 (ceiling 4.17; e1m3 sits at 1–2), 3.71 at 1.5, 3.58 at 1.0, **3.45 at 0.75**, 3.17 at 0.5 — while the mid-demo frames fall to e1m3's range (f3300: 2.47 → 1.16 at 0.75). At 0.75 a 200 light moves 0.78 → 0.77, a 300 light 1.17 → 0.93, an 800 light 3.1 → 1.13; 0.5 starts flattening ordinary lights too (200 → 0.65). So the cap takes the giants out of the picture, and what is left in the corridor is the hall's own density — a dozen ordinary lights in reach, summed unshadowed bar the dominant one — which only `rt_metal_lmax` and `rt_metal_walllight` reach, globally. 0 = the pre-cap picture. Static map lights only. Console-only. |
| `rt_metal_lightsample` | 1 | **Every light casts a shadow in the fog — the shipped default since Seb's 2026-08-28 pass** ("looks good on e1m3... overall looks better than off"), and a three-way mode. The kernels can afford about one shadow ray per fog march step, and they used to spend it on the same light every time — the brightest one reaching that point — with everything else added *unshadowed*, which is why a torch beside a stronger light warmed fog straight through solid geometry (a torch's colour sum is ~1.65 against a plain light's ~3, so it loses the vote almost everywhere). **1 = fog and god rays only**: each march step's ray goes to a light chosen at random in proportion to what it actually scatters there, weighted by the pick probability, so over a handful of frames every light — torches included — is correctly shadowed and the `rt_metal_fog_beams` god-ray term gains real per-light structure. The wall/monster lighting is untouched at 1 (byte-identical to 0), because the surface half is what the 2026-08-17 QA rejected as *speckly in motion* — that full estimator survives as **2**, for A/B only. The blotchiness that sank the first attempt was traced to the pick sharing a spatially correlated noise stream (neighbouring pixels chose the *same* light); the pick now rides a proper hash (`test/pickstream.py` is the probe: 0.334 measured against 0.333 for ideal, where the old stream read 0.014–0.029). 0 = the old dominant-light-only fog, byte for byte, with its unshadowed fill (`rt_metal_fog_residual`). His one reservation — "a teeny bit of speckle" in dim fog, worse at low frame rates — was answered the same day by STRATIFYING the picks along each pixel's march (a hashed base plus a golden-ratio stride per cast, `rt_selseq`): one frame's casts sweep the light list evenly instead of drawing independently, which halves the residual shimmer (simulated EMA std 0.068 -> 0.034) at zero ray cost. A tier lever since the same session (identical on all six tiers — the skyfog pin-not-drift precedent). In-game A/B recipes: `exec foglight_off.cfg` / `exec foglight_on.cfg` / `exec foglight_beams3.cfg` / `exec foglight_beams5.cfg`. |
| `rt_metal_lightsample_clamp` | 0 | Ceiling on the fog light pick's 1/p weight — built for the demo17 speckle (2026-08-29) and **measured mostly ineffective there**, kept as a bounded-amplitude lever: on the demo17 bed clamp 8 changed nothing (the blotch variance is the *bulk* of the pick distribution, not the rare-dim-pick tail this bounds) and clamp 2 cut speckle *events* 66% but ate 44% of the fog's light. What actually answered demo17 is the **deepened stillness floor**: parked with no dynamic lights and the pick live, the fog/shaft EMA floor is 0.9 rather than 0.75 (measured 0.80 → 0.385 mean flicker, changed pixels 2.4% → 0.69%, automatic, no config change; movement keeps your own `rt_metal_fog_history`). 0 = unclamped, the old estimator byte for byte. Console-only. |
| `rt_metal_lightsample_rays` | 1 | *(inert unless `rt_metal_lightsample` is **2** — modes 0 and 1 never read it)*  Shadow rays spent on the stochastically chosen **second** light in the surface (wall/monster) kernel; the dominant light keeps its own `rt_metal_samples` rays. 2 halves the noise in those extra shadows for about one extra ray per pixel. Console-only. |
| `rt_metal_gi` | **1** | **One-bounce diffuse GI — the shipped look since G3 (2026-08-29; Seb's eye passed it in motion: "gi_on is better than off")** — each pixel fires one cosine-weighted bounce ray from its surface, evaluates the map's real lights at the bounce point (one stochastic pick per frame, the fog lightsample machinery, so every light shadows in expectation) and accumulates the result in a reprojected temporal history: torch glow bleeds into shadowed corners and rooms read *lit* rather than painted. Needs `rt_metal 1` and `rt_metal_walllight > 0` (the over-lightmap arm's term is a shadow multiplier over baked light — bounce there would double-count). Menu row **Bounce Lighting** on Options → RT Shadows, greyed at wall lighting 0. The light pick at the bounce uses the primary pixel's culled light list — a stated, modest bias at the default `rt_metal_gi_dist`. Costs ~0.6–0.7 ms of trace at Best's geometry — 4–7 fps on Best, 15–20 on Good, measured fullscreen 2026-08-29; a tier lever, **on for Best and Ultimate, off for Stock, Superfast, Fast, Good and Better** (Good missed its old 180 target with it on; Better lost it in the 2026-09-19 AA retier, where it is the Better -> Best rung). AT THE POST-RETIER TERM IT COSTS 3.1%, not the 22% it costs at a 2.07 Mpx native term -- it fires per TERM pixel, and every tier now traces 720x405. Recipes: `exec gi_on.cfg` / `exec gi_off.cfg`. Console-only knobs: `rt_metal_gi_dist` 512 (bounce ray length, wu), `rt_metal_gi_albedo` 0.5 (constant bounce reflectance — folds 1/π and the average Quake wall; the light carries the hue), `rt_metal_gi_history` 0.9 (deep EMA; GI is low-frequency and affords it; the stillness floor applies), `rt_metal_gi_intensity` 1 (gain, inside the walllight scale and before the `rt_metal_lmax` shoulder so it cannot blow past the ceiling). |
| `rt_metal_gi_emissive` | **0.65** | **The emissive bounce (GIARC G3, 2026-08-29)**: a GI bounce ray that lands on a lava sheet or a torch/candle flame takes that surface's *emission* as its radiance — lava throws orange onto ceilings, flames bounce their own glow. The value is the gain (0–8) mapping emission colour onto the light scale; honest overlap, stated: the merged lava lights and the real torch lights already deliver these emitters' direct light, so this adds the area-source glow those sparse points under-represent. **Default 0.65 — Seb's eye passed it in motion on e1m7 the day it landed, and 0.65 is his calibrated value** (the stills' midpoint was 1; both bounded by the `rt_metal_lmax` shoulder even at 4). 0 = the G1 bounce (world + entities only) byte for byte. Console-only. |
| `rt_metal_gi_rate` | 1 | **GI sample rate (GIARC G4-1, 2026-08-29)**: fire the bounce ray on a frame-rotating 1-in-N subset of pixels and let the deep reprojected history fill the rest — bounce light is low-frequency and can afford it. 1 = every pixel every frame (the full-rate bounce, byte for byte); 2 halves the bounce rays, 4 quarters them. The cost is temporal: bounce light lags a touch more on turns, and a freshly exposed silhouette can miss its bounce for up to N−1 frames. The rotation is per 16×16 tile, not per pixel — a per-pixel checkerboard measured **zero saving** (every SIMD group still contained firing lanes); tile-coherent, rate 2 recovers about half of GI's frame cost and rate 4 about three quarters (fullscreen, demo12: Best 112.6 → 115.7 → 117.6 against 118–119 with GI off entirely). **Seb's verdict (same day): rate 1 "looks a tiny bit better" and the look wins — full rate ships**, with rate 2 one console command away if the frames ever matter more; the steady state on a static view is **identical at every rate** to ±1 LSB, measured — only the refresh rate of the accumulator changes. A tier lever at 1 on all six tiers so the table owns the knob. Archived. |
| `rt_metal_gi_albedo_tex` | 1 | **The coloured bounce (GIARC G4-2, 2026-08-29)**: blend the bounce's reflectance from the flat `rt_metal_gi_albedo` constant (0) to each surface's own mean texture colour (1) — a brown-brick corridor bleeds warm brown into its corners, a slime hall bleeds green; the room's own colour in its bounce light, which is most of what reads as *global illumination* rather than a lift. The colour is the texture's load-time average (the same quantity the engine's old bounce-grid used as its reflect colour), so replacement packs like QRP carry through automatically; entity hits keep the constant. Quake's walls average darker than the 0.5 constant, so 1 also dims the bounce a touch — `rt_metal_gi_intensity` buys it back. **Default 1 — Seb's eye passed it in motion the day it landed** ("albedo on is ok"); `exec gi_albedo_off.cfg` reverts to the flat constant for A/B, and 0 = that old bounce byte for byte. Console-only. |
| `rt_metal_gi_fallback` | 0 | **Bounce light that was not arriving (2026-08-30).** The bounce ray's light pick ranges over the *primary* pixel's tile-culled light list, and measurement showed what that costs: on **20-40% of bounce hits no light in that list reaches the bounce point at all**, so the hit contributes exactly zero bounce light -- which is why the bounce reads as a modest lift rather than filling a room. `1` re-runs the same pick over every staged light, but only on those hits, so nothing that already works changes and the surface lighting is untouched; it is unbiased by construction because the fallback fires exactly when the first pass found nothing, so its reservoir starts clean. Measured: 99.8% of the pixels it changes get **brighter**, and it adds about a fifth again of the bounce on e1m3 (~1% on e1m7, where the emissive lava bounce already dominates). **Costs +0.290 ms on the trace stage (+26%)** -- most warps pay the full-list loop even when only some lanes need it -- so it is **default 0**, and the dilated-tile-list implementation it pointed at was built on 2026-08-31 as `rt_metal_gi_tiledilate` -- which did **not** come out cheaper at full dilation (see its row). Inert wherever `rt_metal_gi` is 0. Console-only. |
| `rt_metal_fog_liquidlight` | **1** | **Light in the water** (BEAUTY B2, 2026-09-16): the fog kernel computes its in-scattered light, its god rays and the lava glow at every march step and then throws them away inside a liquid at four LOCKSTEP sites, by design (the authored underwater look is the in-liquid colour and density alone). This is the fraction of those lit terms that survives under water: 1 = the full shafts and the lights' scatter through the murk while submerged, for the march that already runs; the in-liquid density and colour are untouched and the light adds to them. Cost nil (one multiply at each site). 0 = the old arithmetic exactly. Console-only; `exec fog_liquidlight_on.cfg` / `_half.cfg` / `_off.cfg`. |
| `r_skylightning` / `_period` / `_hold` / `_fog` | **1** / 60 / 0.05 / **0.5** | **Lightning in the sky, thunder through the room** (BEAUTY C1, 2026-09-17): on a map with open sky (the sky instance is non-empty), under wall lighting, at a random interval between a third and one and a half times `_period` seconds, the sun path (`rt_metal_sun`'s machinery) is driven for `_hold` seconds with a cold blue-white light from a random high angle — every surface under open sky lit hard with real ray-traced shadows for that instant — then a second flash at 60% a tenth of a second later, and one to four seconds after (the distance) `ambience/thunder1.wav` (in id1's pak) rolls in from ~700 units away in the flash's direction, so it sends fully into the room reverb and sits on that side. The flash replaces the map's own sun for those frames and the map's sun (or none) returns the frame after. `r_skylightning` is the strength (1 = a bright storm). **`_fog` (0.5) is how much of the flash reaches the AIR**: the flash pushes one light up its own direction carrying the sun's colour as a per-light fog weight — the M5 thunderbolt's own mechanism, slot 7 of the light buffer, which scales what a light scatters and how loudly it argues in the fog's dominant vote while the surface kernel never reads it — so the murk lights with the sky. The fog kernel shadow-tests that light, so fog in a sealed room stays dark while fog under open sky flashes; the GL murk march has no shadow rays by design, so on that tier the lift is smaller and reaches indoor fog too. Measured on the e1m3 sky shaft at Seb's config: the fog over the shaft reads 19 of 255 unlit, 46 at `_fog` 0.35, 58 at 0.5, 84 at 1, with nothing clipped — and a BRIGHT-gamma configuration with thick pale murk saturates well below 0.5, so this is the knob to turn first. 0 is the flash as it shipped, surfaces only. Console-only; `exec skylightning_on.cfg` (period 40) / `skylightning_off.cfg` / `skylightning_fog1.cfg` / `skylightning_nofog.cfg`. |
| `r_caustics` / `_scale` / `_speed` | **0.6** / 0.012 / 0.35 | **Caustics on pool floors** (BEAUTY C2, 2026-09-17): a moving cell pattern of light on every world surface that lies inside a liquid, seen through the water from above or from within. Per PIXEL and with no surface list and no new texture in the surface shader: the baked world field (the murk's own liquid map, already bound to the surface shader by the liquid fade) says whether the point eight units above the fragment is inside a liquid, and two ridged fetches of the noise volume the lava boil binds make the pattern, multiplied into the surface's colour before the fog, so under wall lighting the RT composite carries it as a light modulation. World opaque batches only — the liquids, entities and the HUD get exactly zero. Needs `r_volumetric_liquidfade` above 0 and `r_lavaboil` above 0 (the two parms that bind the field and the noise; both the shipped defaults). `_scale` is cycles per world unit, `_speed` the drift. 0 = today's bytes. Console-only; `exec caustics_on.cfg` (0.6) / `caustics_off.cfg`. |
| `rt_metal_shadowlights` | **3** | **THE ROUND SPOTLIGHTS** (2026-09-19). How many of a pixel's BRIGHTEST lights get a shadow test. **1 is what this renderer did until now, and it is the whole of the defect Seb reported**: "some of the lights are not soft, and are cast from unseen sources, casting clean round lit areas in odd locations". The kernel has always summed every light in range into `Lsum` and then shadow-tested exactly ONE of them — the locally dominant one by colour sum — so a light in the next room, inside a ceiling recess or behind a pillar contributed its full `(1 − d/r)²` disc **straight through the geometry**. Read out of the term buffer on his own demo49 f5000 (e2m1): a perfect circle spilling across a wall, a ledge and a floor uninterrupted, and the stochastic estimator (`rt_metal_lightsample 2`) removes it — which is what proves the light is unshadowed rather than mis-shaded. That estimator is the one his eye rejected in August for speckle, so **2–4 is the DETERMINISTIC form**: track the brightest few and shadow-test each, no pick, no variance, no history. Term luminance on that frame p50 1.062 → 0.957 and **p99 2.572 → 1.873** — the bright unshadowed tail comes down hard while the median barely moves, which is the signature of removing light that should not be there. Wall-lighting arm only. **Cost, drift-immune (frozen e2m1, his config, 720×405 trace, toggled every 4 s in one boot): trace stage 0.510 ms at 1, 0.610 at 2, 0.610 at 3, 0.620 at 4** — the second light costs +0.10 ms (~1% of the frame) and the third and fourth are free, because once any lane in a warp is casting runner-up rays the warp has already paid the latency. An M5 Quality lever, identical on every tier that runs the ray tracer. `exec spotfix_off.cfg` is the old behaviour; `spotfix_max.cfg` is 4 lights at 4 rays. |
| `rt_metal_shadowlights_rays` | 2 | Shadow rays spent on each light past the dominant. The dominant keeps `rt_metal_samples` and its contact hardening; these are cheaper because they are dimmer by construction. Their penumbra uses the STATIC per-pixel dither and is never frame-rotated — the dominant's visibility is smoothed by the `rt_metal_history` EMA and these have none, so a rotating sample set would crawl. Console-only. |
| `rt_metal_contact` | **1** | **Contact-hardened shadows** (BEAUTY B3, 2026-09-17, measure-first as the brief asked): today every shadow has the one softness (`rt_metal_softness`) whether its caster stands on the surface or hangs far above it. With this on, one extra CLOSEST-HIT ray per pixel toward the dominant light finds the blocker's distance, and the penumbra disc the `rt_metal_samples` any-hit rays cover is scaled by (receiver − blocker) / receiver — PCSS's shape: a foot sharp on the floor, a pillar's shadow crisp at its base and soft at its far end; an unblocked probe leaves the disc as it was. The fraction blends it. **Measured on the frozen e1m3 spawn at Seb's geometry (480×270 trace, 6 samples, GI on), toggled every 4 s in one boot: trace stage 2.47–2.68 ms off, 2.63–2.83 on — about +0.2 ms, +7–8% of the stage, ~2% of a Best frame.** More than "a few percent of the stage", so it is NOT a tier lever: an A/B for his eye, and Ultimate's if he wants it there. The demo5 term at 0 is byte-identical to the control; at 1 it moves 3.1% of f120's texels (0.3% of f3000's — the effect is local to contact). Console-only; `exec contact_on.cfg` / `contact_off.cfg`. |
| `rt_metal_gi_ao` / `_ao_dist` | **1** / 64 | **Ambient occlusion for free** (BEAUTY B1, 2026-09-16): the one-bounce GI ray is a closest-hit ray and already reports its distance; a short one means the point is enclosed. That distance, smoothed over `_ao_dist` world units (contact = full occlusion, the range = none) and averaged through the GI's own reprojected history (it rides the GI history texture's spare alpha channel, so it follows every reprojection and rate-skip rule the colour does), darkens the FLAT AMBIENT FILL (`rt_metal_ambient`, "light with no source, on purpose" — this is its corrective) by the `_ao` fraction. The direct light has its own shadows and is never touched. Costs two multiplies where the bounce already runs (Good and up); nothing where it does not. 0 = the term byte for byte (the history alpha is written 0 while it is off). Console-only; `exec gi_ao_on.cfg` / `gi_ao_half.cfg` / `gi_ao_off.cfg`. |
| `rt_metal_gi_tiledilate` | 0 | **The same defect, fixed at the cause (2026-08-31).** The tile light cull bounds the AABB of a tile's *primary hit points*, but a bounce point lies up to `rt_metal_gi_dist` away from its hit -- so a light that reaches the bounce and not the tile is culled, and that sample contributes exactly zero. This dilates the cull by that **fraction** of `rt_metal_gi_dist` and builds a second, wider list that only the bounce pick uses; the surface lighting keeps the tight list and is untouched by construction, and the whole arm is skipped unless `rt_metal_gi` and `rt_metal_walllight` are both live, so the three tiers that ship with GI off pay nothing. **Measured over 385,092 bounce samples, nine frames, four demos** -- bias remaining at each fraction: **0: 22.8%. 0.125: 10.1%. 0.25: 4.1%. 0.5: 0.3%. 1: 0.0%** (full dilation bounds every possible bounce point, so zero there is a guarantee rather than a result). **The cost did not come out as predicted.** On the one drift-clean block, dilation 1 costs **+0.21 ms on the trace stage (+41%)** against `rt_metal_gi_fallback`'s +0.16 ms (+31%) -- dearer than the thing it was meant to undercut, because the dilated list is an ordered superset and the pick pays it on *every* bouncing pixel where the fallback pays only on the 20-40% that found nothing. **Settled on a quiet machine, and the verdict is negative.** At the fraction matching `rt_metal_gi_fallback`'s quality (0.5) it is never cheaper than it: on e1m3 the two **tie** (+0.130 ms each, +26.5%; P(dilate cheaper) = 0.67, overlapping distributions) and on `start` the fallback **wins decisively** (+0.150 ms against this feature's +0.260; P = 0.04). The reason is structural, and it is why no fraction rescues it -- the dilated list is an ordered superset paid on *every* bouncing pixel, while the fallback's full-list loop is paid only on the 20-40% that found nothing, and the more compact the map the more the dilation pulls in until its list approaches the full one anyway. The only point that undercuts the fallback is 0.25 on e1m3 (+0.100 ms, P = 1.00), which buys the saving with 4.1% residual bias rather than ~0. **Use `rt_metal_gi_fallback` instead**; this stays as the measured A/B and as the tunable the fallback structurally cannot offer. **Default 0.** Inert wherever `rt_metal_gi` is 0. |
| `rt_metal_liquids` | 0 | **RT Liquid Lighting**: lets water/slime surfaces receive the RT lighting term. **Greyed unless Force Water Alpha is on and Water Alpha is below 1** — opaque water never reaches the transparent pass, so the slider cannot change a pixel. |
| `rt_metal_liquids_minlight` | 5 | A **floor under the RT term** that `rt_metal_liquids` multiplies into blended liquids. Liquids are deliberately absent from the ray-tracing structure — they must not block shadow rays or god rays — so that term is the lighting of whatever lies *under* the surface, and the submerged geometry's **shadows get printed onto the water**: on e1m1 the bases of the pillars read clearly through slime the eye should barely see into. A shadow is a dark excursion, so a floor removes it. Above `rt_metal_lmax` × 5/3 (4.17 at the default knee) the term is clamped everywhere and no submerged shape survives at all — but it also stops varying, so the feature becomes a uniform brightening rather than scene-varying light. That trade is unavoidable here: the printed geometry and the spatial variation are the same signal. 0 = the unclamped term, byte for byte. **Defaults to 5 since 2026-08-31** — the top of the useful range, so the printthrough is gone out of the box; the whole cvar is inert unless `rt_metal_liquids` is above 0, which is off by default, so the shipped picture is unchanged. |
| `rt_metal_liquids_own` | 0 | **The liquid's own light** (SEPTEMBER2 C1, 2026-09-06; **REJECTED by Seb's eye 2026-09-07** — "large square areas of different lighting over the slime": the per-batch constant, shared on a 256-unit cell grid, IS the squares; kept as the A/B, the per-pixel second term is the fix). `rt_metal_liquids` multiplies the ray-traced term into blended water, but the primary ray passes *through* the surface, so that term is the lighting of whatever lies under it and the submerged geometry's shadows print onto the water (the 2026-08-31 record). This blends (0–1) the sampled term toward the light sum **at the surface**, evaluated on the CPU from the ray tracer's own light list at each batch's world-space centre with `rt_metal_liquids_own_shadows` (2) tracelines to its loudest lights — one constant per batch, so a large lake reads flat: the cheap prototype the scoped per-pixel second term (~0.3–0.6 ms) would replace if the look is worth it. Needs `rt_metal_liquids` > 0 and `r_wateralpha_force 1`. `exec liquid_own_on.cfg` / `liquid_own_off.cfg`. 0 = the sampled term byte for byte. |
| `rt_metal_liquids_rt` | 0 | **The liquid pair: per-pixel own light and a reflection** (SEPTEMBER2 C2, 2026-09-09; **REJECTED by Seb the same evening -- "I don't think reflections on the water is the way to go, having pondered on this"; stays 0, kept as the A/B**). Blended water and slime join the acceleration structure as a seventh instance that only this arm intersects (the primary ray still passes through to the pool floor, whose lighting the composite keeps). Where the liquid is in front of that floor the surface kernel lights the liquid point with the wall term's own chain (every in-range light, the dominant one shadow-tested by one ray, the sky light too) and reflects the view ray about the surface to shade ONE hit -- a torch flame or lava sheet by its emission, a wall or monster by its own colour times the same dominant-shadowed light -- weighted by the water's Fresnel (faint looking down, strong across a lake). Both land in a second buffer the liquid shader reads in place of the floor's term, so the pool floor stops printing through and the room shows in the water; the minlight floor and `rt_metal_liquids_own` are bypassed where the pair is live. Deterministic per frame (no noise, no history). Needs `rt_metal_liquids` above 0, `rt_metal_walllight` above 0 and water that renders blended (`r_wateralpha_force 1` on stock maps). `exec liquid_rt_on.cfg` / `liquid_rt_off.cfg`. 0 = the old bytes. |
| `rt_metal_liquids_reflect` | 1 | Gain on the reflection the pair adds (Schlick's Fresnel at F0 0.02 is physical at 1; 0 = the own term alone, no glint; 2 doubles it). `exec liquid_rt_glint2.cfg` / `liquid_rt_noglint.cfg`. |
| `rt_metal_viewmodel` | 1 | **Weapon Lighting** (2026-08-21): light the view weapon from the RT light list. Wall lighting forces `r_fullbright`, which strips `RENDER_LIGHT` from every entity — and the weapon is the only opaque thing that is then *also* masked out of the RT composite that gives everything else its lighting back, so it rendered at flat albedo in every room with the muzzle flash and the handlamp disconnected from it. Measured on e1m3 at three positions: with this off the gun reads 30.3 / 30.3 / 32.3 (constant, which is the bug); with it on, 8.9 / 10.2 / 56.8 — it belongs to the room again. Strength scale, 1 = matched to the wall term. **Greyed unless Full Wall Lighting is above 0**, where the engine's own model lighting already does the job. 0 = the old flat gun, byte for byte. |
| `rt_metal_viewmodel_shadows` | 4 | How many of the brightest lights get a real occlusion test when lighting the weapon (0–16), so it darkens as you step behind cover instead of staying lit through it. Cost is this many CPU tracelines per frame **regardless of map size** — ad_sepulcher's 2,759 lights cost the same as e1m3's 66. Note that removing an occluded light also re-points the dominant direction at the light that *is* visible, so the weapon can read brighter as well as darker (measured at the e1m3 spawn: the light sum falls 0.818 → 0.629 with 2 of 4 lights occluded, while the facets now facing the visible light gain). Console-only. |
| `rt_metal_viewmodel_smooth` | 0.08 | Seconds of smoothing on the weapon's lighting (roughly the time to cover 63% of a change). Without it the gun pops as the dominant light hands over between two lights while you walk, and as a shadow test flips. 0 = instant. Console-only. |

Console-only companions (the first block moved off the menu on 2026-08-09 — the perf levers
belong to the M5 Quality preset, and the shafts tier is superseded outright while
`rt_metal_fog` is on, which no menu greying ever admitted):

| Setting | Default | What it does |
|---|---|---|
| `rt_metal_samples` | 8 | Shadow rays per pixel. More = smoother penumbras, more GPU. An M5 Quality lever. |
| `rt_metal_scale` | 0.5 | Trace resolution as a fraction of the screen. 1 = per-pixel (~3× the GPU cost). An M5 Quality lever. |
| `rt_metal_history` | 0.9 | Temporal smoothing of the shadow term (0 = off). Higher = less grain, slower to react to moving shadows. |
| `rt_metal_bluenoise` | 1 | The kernels' sampling jitter (and the fog light pick's random stream) come from a blue-noise table, so shadow and fog grain reads as featureless noise instead of the woven dither mesh the old interleaved-gradient pattern produced (worst on the upscaled quality tiers). **Since 2026-09-03 the table is SPATIOTEMPORAL** (`rt_bluenoise.h`, 64×64×16, Wolfe et al. EGSR 2022, regenerated by `test/bluenoise-gen.py` and validated offline by `test/bluenoise-check.py`): each texel's 16-frame sequence is blue in time as well as each frame being blue in space, so consecutive frames' errors cancel under the fog's temporal smoothing instead of piling up — the old table's eight independent slices were blue in space and white in time, and an 8-frame cycle is a hard cap on what any temporal filter can average (measured: at history 0.9 the old table read *worse* than a plain hash stream). 0 restores the classic dither exactly — one-off shader rebuild on change. |
| `rt_metal_culldist` | 1000 | How far beyond its radius a light stays relevant. Lower = faster with many lights. |
| `rt_metal_shafts` | 0 | The lighter-weight god-ray tier. See "God rays and RT fog" below. Needs `r_volumetric`; **does nothing at all while `rt_metal_fog` is on** (the fog kernel supersedes it — and every M5 Quality tier runs the kernel since 2026-08-14, so this is console-only). |
| `rt_metal_shafts_samples` / `_scale` / `_intensity` / `_history` | 6 / 0.5 / 0.5 / 0.5 | The shafts tier's tuning. Inert whenever the tier itself is (above). |

| Setting | Default | What it does |
|---|---|---|
| `rt_metal_smoothnormals` | 1 | Shade monsters/items with smoothed normals in the kernel. Fixes low-poly models reading as faceted under muzzle flashes. 0 = the old flat look. |
| `rt_metal_reproject` | 1 | Remap the RT output through the camera it was traced with, so the lighting never trails fast mouse turns. 0 = the old screen-locked behaviour. |
| `rt_metal_reproject_depth` | 1 | Makes that reprojection account for the camera **moving**, not just turning, using the per-pixel hit distance. This is what removes the fringing on things close to you — strafing past an Ogre at arm's length. 0 keeps the rotation-only behaviour. |
| `rt_metal_sameframe` | **1** | Trace and show the **same** frame instead of remapping the previous frame's trace (Phase 8-2; **default on since the 8-6 QA**). Removes what reprojection can only approximate — the residual one-frame fringing at close silhouettes, and monsters lit a frame late under wall lighting. Measured cost: 12–15% of fps at settings that saturate the GPU, because the trace stops overlapping the rest of the frame; `rt_metal_scale` and `rt_metal_fog_steps` claw it back. While on, reprojection is clamped off (it would be a pure identity). Console-only since 2026-08-09 (it shipped as the default after QA — a settled default needs no row). |
| `rt_metal_lightcores` | 1 | Keep torch/brazier flames out of the shadow-caster set and render them emissive. Fixes torches self-shadowing to black and greyed flames. 0 = the old behaviour. |
| `rt_metal_skyopen` | 1 | Sky brushes count as **open sky** in the ray tracer instead of ordinary walls. Fixes wall lighting darkening patches of the sky (e1m1's dark sky boxes — the sky was being multiplied by a brush's lighting term), and is what lets the fog treat sky as sky at all. 0 = the old behaviour exactly. |
| `rt_metal_glowpass` | 1 | Authored **emission survives wall lighting**: glow/fullbright layers and `r_redglow` are withheld from the lit passes and re-added at authored brightness *after* the RT multiply, so a lamp face glows however its lighting term shades it — a shadowed slot lamp used to render extinguished (e1m1's bollards). Fog still darkens the re-added glow. 0 = the old behaviour exactly (emission multiplied by the lighting term). |
| `rt_metal_liquidemissive` | 1 | Opaque water and slime join the ray-traced world as an **emissive** surface, like lava: primary rays stop at the liquid, so the sheet renders exactly as authored — the murky texture `rt_metal 0` shows — instead of the pool floor's lighting printing through it (e1m1's machinery under the slime). Shadow rays still pass through; the fog stops at the surface. Structurally inert when water renders transparent (`rt_metal_liquids` owns that configuration). 0 = the old printthrough exactly. |
| `rt_metal_shafts_dist` | 2000 | Maximum beam distance (also the ray length against sky). |
| `rt_metal_shafts_residual` | 0 | Unshadowed fill from non-dominant lights in the beams. |

---

## Music

Quake's CD soundtrack plays from ogg files: name them `track02.ogg`, `track03.ogg` … and
put them in **`id1/music/`** or **`id1/sound/cdtracks/`** in your DarkPlaces folder (both
are searched, along with two-digit and three-digit spellings). Each map requests its own
track through `svc_cdtrack` as it loads, so no command is needed in normal play;
`cd play 2` tests one by hand — **never `cd play 1`**, which is the CD's data track and
does not exist as a file. `bgmvolume` is the music volume, independent of `volume`.

**If music is silent while sound effects work, suspect the decoder, not the files.** The
engine has no built-in Vorbis decoder and `dlopen`s `libvorbisfile` at startup; success
prints `Loaded library "…"` in the log and failure prints nothing outside `developer 1`.
Since 2026-08-17 the search includes `/opt/homebrew/lib` and `/usr/local/lib` by absolute
path, so it no longer depends on `DYLD_LIBRARY_PATH` reaching the process — which is why
music used to work under Xcode's Cmd+R and nowhere else.

---

## Volumetric fog — Options → Volumetric Fog

The murk: raymarched fog with a floor-hugging bed, crevice gathering, liquid murk,
surface mist and a dry-ice GROUND FOG layer with cloud-like billow and its own drift,
all driven by a 3D field baked per map. The ground layer is lit by the RT fog kernel
like everything else — torchlight pools in it.

The page's twelve rows (2026-08-09 — every one crosses into the fog kernel and measured live):

| Setting | Default | What it does |
|---|---|---|
| `r_volumetric` | 0 | Master switch for all volumetrics. |
| `r_volumetric_density` | 0.18 | Open-air murk density. Keep low — corners multiply it up. |
| `r_volumetric_corner` | 2.5 | **Corner Gathering**: how much the murk thickens in corners and crevices. |
| `r_volumetric_color_red/_green/_blue` | 0.52/0.60/0.74 | Air murk colour. |
| `r_volumetric_waterdensity` | 0.4 | Murk density inside water. No ceiling — 4 is opaque within ~17 units. |
| `r_volumetric_slimedensity` | -1 | Murk density inside **slime**, overriding the water value for that liquid alone. **-1 inherits `r_volumetric_waterdensity`**, which is what this always did, so an existing config is unchanged until you set it. |
| `r_volumetric_lavadensity` | 0.4 | Murk density inside **lava**, on its own since 2026-09-06. Ships at 0.4, the value water ships with, so a fresh install looks the same as before -- but a water density raised for swimming no longer thickens the haze over a lava lake with it. -1 inherits the water value (the old behaviour). The tint is `r_volumetric_lavacolor_*`.
| `r_volumetric_watermist` | 0.5 | **Surface Mist**: band of mist lying on liquid surfaces. |
| `r_volumetric_mistlavacut` | 1 | How strongly lava suppresses that band, 0–1. Cold mist on molten rock is the one liquid where the surface mist reads wrong. Water and slime keep theirs. 0 = the old density exactly. |
| `r_volumetric_liquidfloor` | 1 | **The liquid surface is the air murk's floor** (2026-09-07). The air murk settles on the local floor, and over a pool the column's floor is the pool *bed* — so over every lake the murk read hundreds of units above its floor and vanished while it hung over the bank, a hard step along the waterline (Seb's e1m4 report; measured: the fog over the water read 0.59 of the fog over the bank, 1.12 with this on, and the largest jump along the waterline fell from 2.41 to 0.16 levels). The height above the floor is capped at the height above the nearest liquid, which the baked field carries as its signed distance; the ground layer settles on the water too. Beside a pool the cap reaches a little sideways (nearest liquid, not straight down). 0 = the old expression byte for byte; `exec liquidfloor_off.cfg`. The parity preamble pins 0. |
| `exec cinematic.cfg` / `cinematic_off.cfg` | -- | **Cinematic replay** (SEPTEMBER2 E, 2026-09-10): the ray tracer set past every tier (native trace, 32 shadow rays, native fog buffer at 64 froxel slices with a cast every slice, hybrid pick 1, GI rate 1, TAA at native) plus the capture pinned at 60 fps non-realtime, for `playdemo <name>` then `cl_capturevideo 1` (that order: a starting demo stops the running one, which clears the capture). The capture readback gained its Metal arm the same day (it was GL-only, a NULL call on the default renderer). A capture paces the demo by the frame, so nothing is realtime-bound and two captures are byte-identical. Ogg/Theora when the libraries load (the capture's library lists carry the Homebrew and /usr/local paths since 2026-09-10, the libvorbis lesson), uncompressed AVI otherwise. `cinematic_off.cfg` re-reads your saved config -- run it before quitting. |
| `reellabel "line 1" ["line 2"]` / `m5_reellabel_size` / `_time` / `_corner` | — / 14 / 9 / 0 | **The showreel's caption** (SEPTEMBER2 F, 2026-09-07): a two-line caption drawn in a corner (0 top-left, 1 bottom-right above the fps meter) over a half-black strip for `_time` seconds, colour codes honoured, immune to the demo's own pickup messages (the console notify area is not). `reellabel` alone clears it. Not archived; `exec showreel.cfg` is its only shipped user. |
| `r_volumetric_ground` | 2.5 | **Ground Fog**: density of the dry-ice layer hugging each room's floor. 0 disables. Needs `r_volumetric_floor`. |
| `r_volumetric_groundheight` | 24 | **Ground Height**: how tall the layer sits (24 = below the knee). |
| `rt_metal_fog_intensity` / `_beams` | 0.5 / 0.5 | The two RT fog sliders — see "God rays and RT fog" below. |

Console-only companions (the first block moved off the menu on 2026-08-09):

| Setting | Default | What it does |
|---|---|---|
| `r_volumetric_height` | 180 | Height of the fog bed before it thins out. |
| `r_volumetric_floor` | 1 | Settle the fog bed on each room's real floor (from the baked field). 0 = old camera-relative bed; the ground-fog rows grey without it. |
| `r_volumetric_water` | 1 | Thicken and tint the murk inside liquids; the liquid rows grey without it. |
| `r_volumetric_watercolor_red/_green/_blue` | 0.05/0.16/0.18 | Underwater murk tint. |
| `r_volumetric_mistheight` | 26 | Thickness of the surface-mist band. |
| `r_volumetric_steps` | 24 | Raymarch steps for the **GL** fog only — **dead while the RT fog kernel is active** (the kernel marches with `rt_metal_fog_steps` instead; measured, 16 → 64 moved nothing). An M5 Quality lever for the kernel-off tiers. |
| `r_volumetric_scale` | 0.5 | GL fog buffer resolution — same caveat as `_steps`: the kernel sizes itself from `rt_metal_fog_scale`. |
| `r_volumetric_particles` | 1 | **Fade Particles**: fades blood, gibs, smoke, sparks and explosions into the murk by distance. The murk is a screen-space pass drawn *before* these, so without this they read bright and clear through fog thick enough to hide the wall behind them. 0 restores the old behaviour; above 1 exaggerates. |
| `cl_particles_lighting` | 2 | **Lit particles** (SEPTEMBER S7, 2026-09-02): alpha-blended particles — smoke, dust, the pointfile's billboards — take the world's light instead of drawing at their flat colour. Until now every particle in the engine was unlit: the type table's `lighting` flag was false everywhere and the branch it gates had never run. **2 is the default since 2026-09-19**, shipped beside the ambient dust because an unlit mote is a flat grey speck rather than ash taking the room's light. 0 = the pre-2026-09-19 picture exactly (byte-identical, gated on both backends). **1** = the engine's own LightPoint per particle: the baked lightmap beneath it plus every dynamic light in range with a shadow traceline each — exact, and the tracelines are uncapped. **2** = the bounded hybrid: the volumetric irradiance grid baked at map load (the same grid that lights the fog, so smoke reads lit like the fog in the same air) plus this frame's dynamic lights with the engine's own falloff and no tracelines. Measured per lit particle with a timer inside the branch, at 16,370 lit particles a frame: mode 1 ≈ 0.25 µs, mode 2 ≈ 0.06 µs, and with one dynamic light in range mode 1 ≈ 0.28 µs — so a 500-particle explosion costs 0.13 ms in mode 1 and 0.03 ms in mode 2. Static lights reach both modes only through the lightmap; under `rt_metal_walllight` that is the lightmap the walls have discarded, so the gain below is the taste knob. Console-only. |
| `cl_particles_lighting_gain` | 2 | Multiplier on the light reaching lit particles. The calibration knob for matching smoke to the walls around it. **2 since 2026-09-19** (Seb's own, shipped with the dust): under `rt_metal_walllight` the static light a particle reads is the lightmap the walls have discarded, so a gain of 1 leaves motes darker than the room they are in (console-only). |
| `cl_particles_lighting_static` | 1 | **Scale the STATIC half of a lit particle's light** (2026-09-19) -- the room's own baked lighting, which on a Quake map is the wall torches and the ceiling lights -- **while leaving this frame's DYNAMIC lights at full strength**. 1 = no change. **THE SPLIT IS THE POINT AND IT IS MEASURED**: a muzzle flash, a rocket's glow, an explosion and the handlamp are all dynamic, so on a frozen e1m3 bed with the handlamp lighting the motes the knob moves **23 bytes** of the frame, and with no dynamic light on them it moves **6483** -- and the motes' own mean luma falls 42.65 to 13.64 at 0.3. That is exactly the shape Seb asked for: dust that sits dark in an ordinary corridor and still flares the instant something goes off. Under `cl_particles_lighting 2` it scales the irradiance grid's term, which IS the static one, for free; under mode 1 the lightmap and the dynamic lights are summed inside one `R_CompleteLightPoint`, so it costs one extra **traceline-free** `LP_LIGHTMAP` probe per particle, and only while the cvar is not 1. It deliberately does NOT touch `r_ambient`'s flat fill, which is a floor you set on purpose. At 1 the mode-2 arm is a multiply by `1.0f` and the mode-1 arm does not run at all, so the off switch is byte-exact by construction as well as by measurement. |
| `cl_particles_texsize` | **256** | **The particle atlas resolution** (BEAUTY A2, 2026-09-16): the size in pixels of every cell of the engine's own particle font, **64** (the 2001 font — measured byte-identical to the previous binary's atlas on every cell that existed) or **256** (a 2048×2048 atlas, 16 MB). Every spark, ember, puff, flash, casing and droplet is authored sixteen times finer, so a particle magnified across a hundred pixels of a 1080p screen keeps its edge instead of the gaussian smear a 64-pixel cell becomes, and the smoke puffs carry two more octaves of noise for free (the generator's noise runs to the texel at either size). Takes effect at once — the font is regenerated between frames, no `vid_restart`. Inert when an external `particles/particlefont.tga` replaces the atlas (that file sets its own cell size). Console-only; `exec cells256.cfg` / `cells64.cfg`. |
| `cl_particles_soft` | **24** | **Soft particles** (BEAUTY A4, 2026-09-16): the distance in world units over which every particle fades where it intersects a wall or the floor, instead of the hard line a billboard draws across the geometry it cuts through (a smoke puff in a doorway, a fireball half inside the wall it hit, dust settling onto a floor). Each particle fragment reads the scene depth behind it, linearises both depths with the murk's own pair, and scales its premultiplied colour by the gap over this distance — exact under all three particle blends. A static parm on the 2D/GENERIC shader while the cvar is on; the uniform is exactly zero on every draw that is not a particle batch (the console, the menu, the HUD and the bloom passes are the old arithmetic), the depth unit is bound to a placeholder on every other GENERIC setup (the declared-but-unbound rule), and the viewmodel flash is exempt (drawn over the gun with the depth test off). Needs the scene depth of this frame, so it is an asker for the offscreen path like the murk and the water; on the bare direct path it stands down. 16–32 is the sensible range. 0 = today's picture exactly; toggling rebuilds the shaders. Console-only; `exec soft_on.cfg` / `soft_off.cfg`. |
| `cl_particles_refract` | **12** | **The shockwave that bends the air** (BEAUTY A5, 2026-09-16): the displacement in screen pixels (authored at 1080p, scaled with the viewport) that a `blend refract` particle layer applies to the frame behind it at full coverage. The explosion's shockwave ring and the ball's burst ring have refracting twins in `m5/effectinfo.txt`: each fragment of the ring becomes the frame behind it fetched further out along the ring's radial direction, so the air visibly bends as the wave passes, for the ring's fifth of a second. The frame copy is the water's (`r_fb.waterscreen`, taken after the murk and before the transparent pass), taken only on a frame where such a particle was queued for the main view, so nothing is paid otherwise; a static parm on the 2D/GENERIC shader while the cvar is on, its uniform exactly zero on every draw but a refract batch and the copy's unit bound to a placeholder elsewhere (the declared-but-unbound rule). At 0 the refracting layers are never spawned — before any random number is drawn — so today's frames are today's. 8–16 is the sensible range. Console-only; `exec refract_on.cfg` / `refract_off.cfg`. |
| `cl_particles_scorchglow` | **1** | **The scorch that cools** (BEAUTY A6, 2026-09-16): where a ball lightning earths on a wall or a grounding arc lands, and where an axe strikes stone, the mark GLOWS orange and cools to nothing over about three seconds, over the dark scorch decal. Pure data: an additive surface-oriented particle (`type raindecal`, `orientation oriented`) laid on the surface's own normal, which the QuakeC (`m5ball.qc`) and the bolt renderer now pass as the effect's base velocity; three layers in `m5/effectinfo.txt` gated on this cvar by name through a new engine keyword, **`requirecvar <cvar>`** (the layer spawns only while the named cvar is above 0, decided before any random number is drawn; a misspelt name prints once and the layer never spawns). Rockets are not covered — `TE_EXPLOSION` carries no surface normal (the brief's route (a), a second decal batch, is not built). 0 = the dark scorch alone. Console-only; `exec scorchglow_on.cfg` / `scorchglow_off.cfg`. |
| `cl_particles_blood_droplet` | **1** | **Blood as droplets** (BEAUTY A2, 2026-09-16): 1 draws the airborne blood as one hard-edged teardrop per particle (cell 40, a random angle each) instead of the eight random-blotch cells the engine has drawn since 2001, which at any size read as soft red smudges. The blood decals on walls are untouched. 0 = today's blood, and at 0 the spawn draws no extra random number, so the frames are the old ones. Console-only; `exec droplet_on.cfg` / `droplet_off.cfg`. |
| `r_volumetric_particles_alpha` | 1 | **How particles fade into the fog** (2026-09-02): 1 scales an alpha-blended particle's ALPHA by the murk's transmittance, so what shows through as it recedes is the fog's own composited pixel — lit by the kernel, beams and all — and a black smoke puff at the far wall is shrouded exactly as the wall behind it is. 0 is the old lerp of the particle's COLOUR toward the fog's authored base colour, which for a dark fog left a distant puff black at full opacity, "cutting through the fog" — the liquid-fade defect of 2026-08-31 in its particle form. Additive particles (sparks, flashes) were always alpha-faded and do not change. Console-only. |
| `r_volumetric_liquidfade` | 0.5 | **Fog on clear water**: fades alpha-blended water and slime into the murk by distance. The twin of `_particles`, and it exists for the same structural reason — a transparent surface writes no depth, so the murk's screen-space pass cannot see it and blended liquid reads at full brightness through fog thick enough to hide the wall behind it. Only does anything where liquid renders *blended*, i.e. with `r_wateralpha` below 1 and `r_wateralpha_force 1` on stock maps; the engine says so on the console when that configuration is on and this is not. It scales the ALPHA rather than lerping toward a fog colour, so what shows through as the surface fades is the murk's own composited pixel — beams, irradiance and the winning light's hue arrive correct and free. **0.25-0.5 is the useful range at a thick density**, not 1: the fade over-estimates density by ignoring the noise `patch` term (exactly as the CPU particle hook does), so at 1 the water at your feet fogs as hard as the pool across the room. **The default is 0.5**, the midpoint of that range and the value that passed QA on 2026-08-31; 0 restores the pre-flip picture exactly and is what every frozen bed pins. |
| `r_volumetric_ambient` | 1 | **Fog lit by the world**: the murk's brightness follows the level's own static lighting, read from a coarse irradiance grid baked at map load — dark rooms give dark fog, and the ground mist stops glowing on its own. Blend strength; 0 restores the self-lit murk exactly. |
| `r_volumetric_ambientgain` | 1 | How strongly the baked lighting drives the fog's brightness — the calibration knob for matching the fog to the walls around it. At 1 a spot the map lights fully gives fully lit fog and anything dimmer darkens in proportion (it was 1.5 until 2026-09-19, which left most open air clamped at fully lit). Console-only. |
| `r_volumetric_ambientdilate` | 1 | How the light grid fills the cells inside walls, which filtering mixes into the air beside them. 1 = the average of the open air around the cell, so a dark passage beside a bright room stays dark; 0 = the brightest neighbour (the pre-2026-09-19 bake exactly), which made the fog read fully lit nearly everywhere. Console-only. |
| `r_volumetric_ambientfloor` | 0.12 | The fog's minimum brightness share in a pitch-black room, so it never goes fully invisible. 0 lets total darkness swallow it. Console-only. |
| `r_volumetric_irrcell` | 64 | World units per irradiance-grid cell (next map load). Console-only. |
| `r_volumetric_dlight` | 1 | Dynamic lights scatter in the fog on the **GL-march tier**: muzzle flashes, explosions and the thunderbolt light the murk around them. Gain; 0 disables. With the RT fog kernel active (your config) the kernel's own shadow-rayed light integral supersedes this. |
| `r_volumetric_dlight_g` | 0.4 | How forward-favouring the scattering is (Henyey–Greenstein g): higher makes lights bloom hardest when you look towards them through fog. Console-only. |
| `r_volumetric_scatter` | 1 | Scattering gain on the march-tier light term. Console-only. |
| `r_volumetric_extinction` | 1 | How strongly fog obscures per unit of density, without brightening (Beer–Lambert σ_t). Works on every fog path, kernel included. 1 is the classic model exactly. Console-only. |
| `r_volumetric_heightbase` | −48 | Bed height offset for the old camera-relative mode (`_floor 0`). |
| `r_volumetric_flooroffset` | 0 | Shifts the floor-anchored bed up or down. |
| `r_volumetric_noisescale` / `_noisethresh` | 0.0016 / 0.30 | Scale and threshold of the drifting noise that breaks the fog into patches. |
| `r_volumetric_noise2` | 1 | **Fog-bake v2**: the fog's noise field becomes domain-warped Perlin–Worley clumps with a ridged streak — folded, curled banks instead of blobs on a grid — and is seeded per map, so no two levels share a fog pattern. Spread is matched to the classic noise, so the thresholds above keep meaning what they meant. 0 regenerates the classic noise exactly. |
| `r_volumetric_noisesize` | 96 | Texels per axis of the v2 noise volume (32–128); more carries the finer octaves without softening. Console-only. |
| `r_volumetric_noise2_octaves` / `_warp` / `_clump` / `_ridge` / `_contrast` | 4 / 0.35 / 0.5 / 0.25 / 1 | The v2 shaping knobs (octave count, domain-warp strength, Perlin–Worley clumping, ridged-streak weight, spread multiplier). Archived since 2026-09-03 (they were console-only, and a look Seb tuned in-game would have vanished at the next launch); changing one rebakes the volume on the spot. Direction for a crisper, more chaotic fog: `_contrast` up (sharper clump edges), `_warp` up (more folded banks), `_ridge` up (more wisps). |
| `r_volumetric_swirl` | 6 | **KH swirl (F3)**: an analytic curl field displaces the fog's noise lookups, so banks and ground mist roll and shear as they drift instead of sliding as one sheet. Amplitude in world units; 0 restores the un-swirled lookups exactly. |
| `r_volumetric_swirlscale` | 0.008 | Swirl eddy size (per world unit): smaller = broader, lazier circulation; larger = tighter eddies. |
| `r_volumetric_swirlkh` | 1.5 | Extra swirl in a band at the ground-mist/air interface — the Kelvin–Helmholtz billow, where cloud tops roll up. Needs the floor-anchored bed (`r_volumetric_floor` 1). |
| `r_volumetric_wind` | "10 4 1.5" | Wind drift of the fog patches (quote it!). The swirl's phase rides this, at a slower fraction, so pinning the wind to "0 0 0" freezes the swirl too. |
| `r_volumetric_dist` | 3000 | Maximum march distance. |
| `r_volumetric_fieldcell` | 64 | Baked-field cell size in world units (takes effect next map load; smaller = sharper waterlines, much bigger bake). |
| `r_volumetric_slimecolor_*` / `_lavacolor_*` | greens / red-orange | Murk tints inside slime and lava. |
| `r_volumetric_grounddeform` | 14 | How much the ground fog's top surface rolls as the large-scale noise drifts over it. |
| `r_volumetric_groundnoisescale` | 0.004 | Ground fog billow size (~31-unit puffs; 0.002 = ~62). |
| `r_volumetric_groundthresh` | 0.25 | Billow threshold: higher = sparser, more defined banks (past ~0.5 the layer thins to nothing). |
| `r_volumetric_groundoffset` | 0 | Raises/lowers the layer relative to the floor. |
| `r_volumetric_groundwind` | "4 1.7 0" | Ground fog drift (quote it!). Deliberately different from the air wind so the layer creeps independently. The default matches the air murk's perceived pace — the billow's puffs are 2.5× smaller than the air's, so equal wind speeds would read 2.5× faster. |
| `r_volumetric_groundcolor_red/_green/_blue` | 0.62/0.68/0.80 | The layer's own colour — brighter and cooler than the air murk. |

---

## God rays and RT fog — the two beam systems

Two tiers, both needing `rt_metal 1` and `r_volumetric 1`:

- **`rt_metal_shafts`** — the cheaper screen-space god rays (≈ +1.7 ms GPU). Beams glow in
  clear air and dim behind murk.
- **`rt_metal_fog`** (console/preset-owned; its two sliders sit at the foot of Options →
  Volumetric Fog) — the full
  system (≈ +2.5–3 ms GPU): the Metal kernel marches the whole fog integral, lighting every
  step from the map's lights with real occlusion. While on, it **supersedes** the shafts and
  carries both light terms itself.

RT fog settings (the first two are on the Volumetric Fog page):

| Setting | Default | What it does |
|---|---|---|
| `rt_metal_fog` | 0 | The master (see above). |
| `rt_metal_fog_intensity` | 0.55 | Density-coupled light gain — beams brighten inside murk banks. 0.5 is the designed midpoint. |
| `rt_metal_fog_beams` | 0.5 | Density-INDEPENDENT god-ray glow — beams show even in clear air. 0.5 = clearly visible rays, 1 = thick bloom that saturates softly; the term is soft-limited and can no longer white the frame out on long sightlines. A tier lever at 0.5 on all six tiers since 2026-08-28: with every light shadowed in the fog the beams finally carry per-light structure, and Seb judged the designed midpoint right ("subtle god rays but nice") — an archived lower value from the structureless era normalises the next time a tier is clicked. |
| `rt_metal_fog_residual` | 0.1 | Soft unshadowed fill from non-dominant lights. This is what let torch warmth tint the fog beyond point-blank range before `rt_metal_lightsample` existed — **it is ignored at `rt_metal_lightsample` 1 and 2** (every light is shadow-tested there, so the fill would double-count; note the fill is also what lets torches warm fog *through walls*, which is exactly what mode 1 fixes), and applies again only at `rt_metal_lightsample 0`. The shipped default is `rt_metal_lightsample 1`, on every tier that runs the fog kernel bar Superfast, so on a stock install **this fill is inert and every bit of light in the fog is shadow-tested** (verified in the kernel 2026-09-19: both fill sites are gated on the pick being off). 0 = dominant light only (console-only). |
| `rt_metal_fog_steps` | 16 | March steps (console-only). 12 is a cheap-and-cheerful alternative (~5–10% fps). |
| `rt_metal_fog_stride` | 2 | Shadow rays fire every Nth step (console-only). |
| `rt_metal_fog_scale` | 0.5 | Fog kernel resolution (console-only). |
| `rt_metal_fog_history` | 0.5 | Temporal smoothing (console-only), capped at 0.9 — or **0.95 with `rt_metal_fog_clamp` on**, which is the thing that makes a deeper history safe. When you stand still with no flashes live, this is automatically strengthened to settle the last shimmer (the floor is 0.9 under the light pick, 0.95 under the clamp). |
| `rt_metal_fog_stride_adaptive` | 1 | Space the fog kernel's shadow casts wider as the fog thickens (WARCHEST session 3, 2026-08-28): below 50% transmittance the cast stride doubles, below 25% it quadruples — deep in a bank the held light's update rate is invisible (every contribution is multiplied by the transmittance) but its shadow ray is full price. Measured on demo14: fog stage −0.06–0.12 ms (~2–4%), nil at thin fog — the kernel's existing near-total-extinction break had already harvested most of the deep tail, which is why this is a shaving rather than PERFPLAN's stride-doubling prize (that remains the visible quality trade the tier table owns). 0 = the fixed schedule byte for byte. |
| `rt_metal_refit` | 1 | REFIT the per-frame entity and light-core BLAS in place when their topology has not changed, instead of rebuilding from scratch (WARCHEST session 2, 2026-08-28). A refit updates the existing structure for moved vertices at a fraction of a build's cost; the visible set changing or a 16-refit quality cadence forces a real rebuild. Measured on demo11: 72% of frames refit, trace stage ~−0.1–0.15 ms at that entity load (the saving scales with entity triangles, not pixels). Invisible by design. 0 = rebuild every frame, the old path byte for byte. |
| `rt_metal_term_upsample` | 1 | Depth-aware magnification of the RT LIGHTING TERM at the composite (WARCHEST session 1, 2026-08-28) — the fog upsample's trick applied to the shadow/wall-lighting buffer. Under wall lighting the term IS the scene's lighting, so plain bilinear magnification bled the background's lighting across every silhouette and shadow edges stepped at the trace buffer's pitch; now edge pixels are rebuilt from the taps whose scene depth matches their own, interiors byte-untouched. This is what makes low Trace Resolution values genuinely usable — the route to tracing at a quarter scale without the edges giving it away. Needs the scene depth as a texture, which the feature's own asker provides on any offscreen-path configuration (r_viewfbo ≥ 1, volumetrics, r_edr, MetalFX temporal — i.e. every real config; the bare direct path stays plain bilinear). 0 = the plain magnification byte for byte. |
| `rt_metal_term_upsample_depth` | 0.1 | The term upsample's surface-split tolerance, the fog upsample knob's twin: how far apart (as a fraction of the pixel's own distance) two depths may be before the taps are treated as different surfaces. Console-only. |
| `rt_metal_fog_upsample` | 1 | Depth-aware magnification of the kernel's fog buffer (2026-08-16). The kernel marches each fog texel to whatever geometry it saw, so plain bilinear magnification blended near-wall fog with far-wall fog across every silhouette and the edge stepped at the buffer's pitch — the 480-px staircase along walls against sky and pillars against far walls. Now every pixel at an edge is rebuilt from the taps whose march ended on ITS surface, at full resolution; interiors are untouched byte for byte. The same switch moves the kernel's own march-end lookup to the texel centre (it read the corner, which is what the blur was hiding). Console-only. 0 = the old bilinear magnification AND the old kernel lookup exactly (the A/B switch). |
| `rt_metal_fog_filter` | 0 | A depth-aware **3×3** filter over the ray-traced fog buffer at the fog's own resolution, before it is magnified (2 = 5×5, 3 = a cheaper transmittance-only A/B, **4 = the 5×5 twice with the second pass's taps two texels apart** — an à-trous pair with a 13×13 footprint at two passes' cost, added 2026-09-03 because the fog's raw grain is three to four texels across, which one 5×5 cannot average, and that grain is the standing fizz; cost +0.49 ms on the fog stage, one-boot toggle at his geometry). The fog's jitter puts its grain at the buffer's Nyquist frequency, which the magnification then puts on screen as the standing mesh; a [1 2 1] filter has zero response at exactly that frequency, and this is the only stage that removes it **at source**. Measured on the raw fog buffer: grid amplitude −27% to −39%, and 5×5 buys nothing further (the Nyquist band is gone after 3×3, which is the proof it hit its target). What remains is stonework showing through, not fog grain. Costs **+0.2–0.4 ms** on the fog stage — more than hoped, the dispatch not the arithmetic. **Default off, but every M5 Quality tier now sets it** (Best/Better 2, Good/Fast 1 -- Good took the cheaper 3x3 when it went back to the spatial scaler on 2026-08-21) — Seb's verdict was "fog_filter 2 looks amazing". 0 is the old path byte for byte. **Corrected 2026-08-19:** the first build bound a 48-byte parameter block to the kernel's 64-byte struct, so the *mode* was read past the end of the bind — the 5×5 and the mode-3 A/B never actually ran, and "5×5 reads identical to 3×3" was identical because it was the 3×3 both times; under Metal API validation the short bind aborted the engine. Fixed, and re-measured with the 5×5 genuinely running: raw-buffer weave 0.00571 off, 0.00418 at 3×3, 0.00415 at 5×5 — the Nyquist argument holds, now on evidence. |
| `rt_metal_fog_clamp` | 0 | **The fog history pass** (BLUENOISE slice 2, 2026-09-03). Today the fog kernel blends each frame with last frame's *filtered* fog through a rotation-only reprojection and nothing checks what that read returned — which is why the history is capped at 0.9 and why the parked floor stops there: deeper history without a check ghosts on every light flick, door and teleport. It also means the spatial filter is re-applied to the feedback every frame, so at history *h* the picture carries the filter roughly 1/(1−*h*) times over. With this on, the kernel writes the RAW fog, a separate pass **clamps the reprojected history into the current frame's 3×3 neighbourhood** — 1 = min/max, 2 = mean ± `rt_metal_fog_clamp_k` sigma (the variance clip every RT denoiser ships beside the box), 3 = no clamp at all (the A/B that isolates the reorder) — and accumulates it, and the display filter runs **once** on the result. `rt_metal_fog_history` may then go to 0.95. The walking-camera speckle Seb reported on 2026-09-03 is exactly the case this was built for — **and it did not survive the evening's measurement in motion**: against an eight-realisation mean truth on his demo23 strafe, a single raw fog frame errs by 1.7% of its luma, today's 0.7 history by 2.2%, the clamp at 0.8 by 2.5% and any history at 0.95 by 4–8%, because a 2D screen-space history is content from where the fog WAS and no single displacement registers a volume. Parked it is the smoothest arm there is; moving it smears. **Console A/B for a parked camera only**; the despeckle package is `rt_metal_lightsample_hybrid` + `rt_metal_fog_filter 4`, and the structural fix is a 3D froxel volume (not built). 0 = the old path byte for byte. |
| `rt_metal_fog_clamp_k` | 1.5 | Mode 2's sigma multiplier: how far from the neighbourhood mean the reprojected history may sit before it is clipped. Smaller = less ghosting and less smoothing. Console-only. |
| `rt_metal_fog_tonemapema` | 0 | Blend the fog history in a compressed *c*/(1+luma) domain and invert on the way out (the Karis TAA weight; BLUENOISE slice 3), so one bright light pick weighs less than its linear worth in the average. Needs `rt_metal_fog_clamp` ≥ 1 (the history pass is what blends). It biases the accumulated fog **down** wherever the picks vary — `rt_metal_fog_intensity` is the calibration if the fog dims. 0 = linear blending, the old bytes. |
| `rt_metal_fog_reproject_depth` | 0 | **Translation-aware fog history** (2026-09-03 afternoon, Seb's strafing smear). The fog kernel records where along each ray its in-scattered light came from (the extinction-weighted mean march distance — about 100 units out at his density, against walls 300–1000 away) and the history pass puts that point through the *full* previous camera, eye included, reading last frame's fog bilinearly where it actually was; where the previous frame's own scatter depth there disagrees by more than `rt_metal_fog_reproject_tol` (0.3) the history is dropped rather than smeared. Needs `rt_metal_fog_clamp` ≥ 1. **MEASURED 2026-09-03 EVENING AND IT MAKES THE HISTORY WORSE, NOT BETTER** (mean error against the true fog 4.4% at 0.95 clamped, against 2.2% for today's path): the fog's visible structure lives in the far fog while the extinction-weighted centroid (median 173 units here) sits in the smooth near fog, so displacing the whole texel by the near fog's parallax misplaces exactly the part the eye sees; three probes (the translation negated, the depth doubled and halved, a bilinear rotation-only read) all landed off the truth. A 2D reprojection cannot register a volume; kept as the measured A/B (modes 2–5 are its debug probes). 0 = the rotation-only remap. |
| `rt_metal_sun` | 0 | **The sky light** (SEPTEMBER2 D, 2026-09-06; Seb's verdict 2026-09-07: "ok, but doesn't fit well with lots of maps ... keep it as an option" — so it stays 0, an `exec sun_on.cfg` choice per map): a directional sun for the ray tracer. Each surface pixel casts one closest-hit ray toward the sun, jittered inside the sun's disc, and is sunlit exactly where that ray leaves the level through open sky (the same open-sky structure the fog cap uses) — walls, floors and monsters take a real sun with real shadows. Wall-lighting mode only: under the lightmap the map's own baked sun is already there, and under wall lighting that lightmap is discarded, which is why outdoor Arcane Dimensions read flat until now. The sun comes from the map's ericw keys (`_sunlight`, `_sunlight_color`, `_sunlight_mangle` or `_sun_mangle`, `_sunlight_penumbra`: AD's start, sepulcher and e1m1 declare one; ad_tears and every id1 map do not) unless the cvars below override them. The value is a multiplier on the map's strength (1 = as authored). `exec sun_on.cfg` / `sun_off.cfg`; `sun_e1m1.cfg` puts a hand-set sun over stock Quake's outdoor start. |
| `rt_metal_sun_light` / `_color` / `_mangle` / `_penumbra` | 0 / "" / "" / −1 | Overrides for the map's keys: strength in ericw units (250 strong, 100 soft; 0 = the map's), colour QUOTED (0–1 or 0–255), direction as `"yaw pitch"` QUOTED in ericw's convention (negative pitch shines down: `"300 -60"` is a high south-west sun), and the disc's angular radius in degrees (soft edges; −1 = the map's). A map with no sun and no override gets none. |
| `rt_metal_as_skipstatic` | 1 | **Reuse an unchanged acceleration structure** (SEPTEMBER2 A3, 2026-09-06). Every frame the entity and light-core BLASes were refit and the TLAS rebuilt whatever happened (the AS-floor record of 2026-08-30: 0.17–0.36 ms, content-dependent). With this on each gather hashes the bytes it uploads, a slot whose structure was last built from exactly those bytes skips its refit, and the TLAS is skipped too when both do — identical input, identical BVH, so byte-exact. In play the saving is bounded by how often nothing animates: every flame model lerps every frame, so a torch in view keeps the light-core structure moving; parked with no torches, in photo mode or on a frozen bed the whole AS stage disappears. The KERNELMS refit counter line reports the skipped share. Folding the two AS encoders (the brief's second arm) is **not** done: Apple's guidance is that builds in one encoder may run in parallel, so the second encoder is what orders the TLAS after its BLASes. `exec asskip.cfg` / `asskip_off.cfg`. |
| `rt_metal_pipeline` | 0 | **Same-frame RT pipelining** (SEPTEMBER2 A2, 2026-09-06). Under `rt_metal_sameframe` the composite hook commits the ray tracer's frame and waits for it while the whole raster encoded so far sits in the renderer's command buffer, uncommitted until the end of the frame — so the GPU runs the trace and then the entire renderer buffer in *series*, which is the 12–15% same-frame cost measured on 2026-08-08 (lost concurrency, not a stall). **Built and measured nil, kept as the A/B.** 1 commits the raster's buffer the moment the trace is committed so the two overlap on the GPU — and they do (the sidecar's stages stretch 13–33% beside the raster) — yet the frame is no shorter: +0.7–0.9% on interleaved demo26 pairs at render scale 1, inside the pair noise, because the GPU is saturated during the overlap and concurrency only reorders the work. 2 also commits the RT composite and the murk composite the moment they are encoded and reads −1.5 to −5.7%: the extra buffers cost more than the bubble they fill. So that 12–15% is not commit ordering, and the 2026-08-08 reading stands. Picture byte-identical either way (parity bed, three vantages). Metal only. `exec pipeline.cfg` / `pipeline_off.cfg`. |
| `rt_metal_fog_froxel` | 1 | **The froxel fog volume** (SEPTEMBER2 A1, 2026-09-06; **DEFAULT 1 since 2026-09-07** on Seb's eye — "froxel looks amazing, keep that" — and a tier lever: 1 on Good/Better/Best/Ultimate, 0 on Fast pending a bench of the 48-cell volume on its 320x180 buffer; `exec froxel_off.cfg` is the A/B) — the structural answer to every smear-and-speckle verdict since 3 September. The fog kernel marches *fixed* view-aligned depth slices (exponential, dense near the eye, out to `r_volumetric_dist`) and stores each cell's in-scattered light and density in a 3D volume instead of summing them per texel; the history is then blended **per cell at the cell's own world position** through the full previous camera, eye included — the thing a 2D screen-space history structurally cannot do (measured 2026-09-03: any 2D history under camera translation *increases* the error against the true fog). An integrate pass sums the accumulated cells into the same fog buffer as before, so the composite, the display filter and the depth-aware upsample are untouched. What it buys is a deep history (`rt_metal_fog_froxel_history`, 0.9) with no strafe smear, so the light pick's variance averages over ~10 frames rather than 3 — the route to fewer casts per ray at the same converged look. Cells beyond the wall are not marched (cheaper than today's 24 steps to every hit); the cost is the volume's history reads and one small integrate pass. Supersedes `rt_metal_fog_clamp` / `_reproject_depth` while on. Memory: two RGBA16F volume pairs, ~200 MB at a 480×270×48 buffer (140 MB at Better's 400×225). `exec froxel.cfg` / `froxel_off.cfg`; `froxel24.cfg` (the cheaper, slightly bright arm) and `froxel095.cfg` are the arms. 0 = the shipped fog kernel byte for byte (a separate compiled variant, not a branch). |
| `rt_metal_fog_froxel_slices` | 48 | Depth slices in the volume, which is also the march's step count under it (`rt_metal_fog_steps` is not read). **Measured 2026-09-06 on demo23's strafe against the mean truth: 24 slices converge +1.4% brighter than the true fog** (cells coarser than the density's variation bias the single-point extinction estimate), **48 land on it (−0.01%)** and beat every history arm ever measured on that bed; 16 is +3.7%. Casts still fall every `rt_metal_fog_stride` cells. Console-only. **A TIER LEVER SINCE 2026-09-19, and CF_ARCHIVE with it** (an unarchived lever resets on the next launch and the tier reads Custom every boot): 16/16/24/24/24/32/32 up the table, inert below Better where `rt_metal_fog_froxel` is 0. This is the knob that ACTS under the froxel — `rt_metal_fog_steps` is overwritten by the slice count there. |
| `rt_metal_fog_froxel_history` | 0.9 | The per-cell temporal blend (0 = none, 0.9 ≈ 10 frames, 0.95 ≈ 20). Each cell reprojects its own world point, so this can sit far deeper than `rt_metal_fog_history` without smearing; a light flick or a door lags by about that many frames. Console-only. |
| `rt_metal_fog_froxel_near` | 6 | How deep the **first** cell is, in world units; the exponential spacing is derived from this, the slice count and the live `r_volumetric_dist` every frame, so the near fog keeps its resolution whatever the distance setting (the cell-size bias above is a world-unit fact, not a slice count). Console-only. |
| `rt_metal_fog_froxel_curve` | 0 | 0 = derive the spacing from `_near` (the default); > 0 pins the exponent *a* directly: slice depth = dist·(e^(a·u) − 1)/(e^a − 1). Console-only. |
| `rt_metal_fog_froxel_castphase` | 0 | 1 advances the shadow-cast schedule by one cell per frame (the first cell is always cast) so every cell is freshly cast within `rt_metal_fog_stride` frames. Built to average out the +2.5% bright bias stride 6 shows under the froxel — **and measured not to** (+3.1% with it, and noisier parked): the bias is a forward hold, every cell taking its light from a cast point nearer the eye than itself, which no schedule phase touches; the fix is interpolating between casts, not built. Kept as the A/B; stride 4 is the honest cheaper rung (today's accuracy for three quarters of the casts). Console-only. |
| `rt_metal_lightsample_hybrid` | 0 | **The dark-spot fix at source** (2026-09-03 afternoon). With the single pick every fog cast is binary — the one light it chose is either lit or blocked — so a texel whose casts picked shadowed lights goes black for the frame and the spatial filter spreads it into a soft dark blob. 1 always shadow-tests the dominant light on its own ray and re-draws the pick over the rest, so a cast can lose at most the rest's share (two shadow rays a cast — the fog stage's cast cost roughly doubles); 2 alternates casts between the dominant and the pick at one ray a cast, holding each half's last visibility. The rest's estimate applies the picked light's visibility to the rest's summed colour (lower variance; a slight colour bias only where the rest's lights differ in hue). Needs `rt_metal_lightsample` ≥ 1. **Measured 2026-09-03 evening on Seb's demo22** (visible-band frame flicker, walking / parked, with the blue-noise table): the single pick 1.04 / 0.20, mode 1 0.83 / 0.09, mode 2 0.86 / 0.10 — so mode 2 buys nearly all of mode 1's gain and is the recipe's choice (cost, one-boot toggle at his geometry on a loaded machine: mode 1 +0.75 ms on the fog stage, mode 2 +0.43 ms — the rest's pick loop runs every cast, so it is not free); the error's dark tail against the mean truth halves (−3.3% → −1.8% at p5) and the estimate shifts +1% bright, which is the concave light shoulder losing less to variance. 0 = the single pick, the old bytes. **3 (2026-09-06) is the single-pass form**: no second light loop — the one pick decides which half the cast serves, so the dominant is measured in proportion to its weight rather than every other cast; cheaper than 2 by that loop, with a held visibility that lags where many lights are equal. |
| `rt_metal_fog_stepjitter` | 0 | **Built, measured, rejected — leave at 0.** Spreading the fog march's jitter along each ray made the grid *worse* on the raw buffer (amplitude +24%): moving each step's sample position also moves where it reads the noise volume, which adds lattice structure rather than averaging it. Kept only as an A/B switch with the numbers in its help text. |
| `rt_metal_fog_upsample_depth` | 0.1 | How far apart two depths may be, as a fraction of the pixel's own distance, before the upsample treats them as different surfaces. Lower = sharper edges, more surfaces split; higher = closer to plain bilinear. Console-only tuning knob; the shipped value came off the demo13 ladder bed. |

---

## Water and liquids

| Setting | Default | What it does |
|---|---|---|
| `r_wateralpha_force` | 0 | **Force Water Alpha** (Options → Effects and Particles). Makes `r_wateralpha` actually work on the stock id1 maps (the engine silently ignores it there — vanilla map data fails its visibility test). Needed for see-through water, in-pool murk seen from the bank, and `rt_metal_liquids`. It gained a menu row on 2026-08-03; before that the Water Alpha slider sat above it looking fully live while doing nothing. |
| `r_wateralpha` | 1 | **Water Alpha (opacity)**, the row below it. 1 = opaque, 0.8 is a good see-through value. Greyed until the force row above is on. |
| `m5_liquidflags` | 1 | Console-only. Keeps a liquid's own material flags and contents when a **replacement texture pack** supplies its image. Without it an external `*water`/`*slime`/`*lava` texture loads as a plain opaque wall, which silently disables everything on this page **and** the whole Lava section below, makes liquids cast shadows, and stops them scrolling — the QRP pack replaces every one of id1's liquid textures, so on a QRP install none of it worked. 0 restores the old behaviour exactly. |

---

## Lava (all console-only)

Lava is opaque, 100% emissive (every texel of the classic lava texture is
palette-fullbright — its whole visible image is its glow layer), and since 2026-08-01
it boils, glows hot, warms the fog above it, and lights its room. Everything ships on;
each piece has its own off-switch for A/B. The two menu pages are full, so these five
live in the console.

| Setting | Default | What it does |
|---|---|---|
| `rt_metal_lavaemissive` | 1 | Lava sheets join the ray-traced world as an emissive surface. Fixes the leak where the ray tracer shaded the SUNKEN geometry and printed its lighting onto the sheet (structures read as if through the lava), and stops the RT composite crushing lava's glow. 0 restores the old leak. |
| `r_viewfbo` | 0 | Set to **2** to render the scene into a 16-bit float buffer, so bright things can exceed white instead of clipping on the spot. Costs about 0.81 ms a frame at 1080p (8%). On its own it changes almost nothing — it is the prerequisite for the setting below. |
| `r_hdr_shoulder` | 0 | With `r_viewfbo 2`, rolls highlights off instead of clipping them. This is the **knee**: below it nothing changes at all, above it the picture rolls smoothly towards white, and the roll is applied to the brightest channel so hot highlights keep their colour instead of turning into white blobs. **0.75** is a good starting point. Does nothing at `r_viewfbo 0`. |
| `r_bloom_m5` | 0 | **The modern HDR bloom** (BEAUTY A1, 2026-09-16; `exec bloom_m5_on.cfg` / `bloom_m5_off.cfg`). `r_bloom` stays the master and its menu row; with this on the 2001 chain (downscale to `r_bloom_resolution`, square it `_colorexponent` times, box-blur it with offset quads -- no threshold, no mip chain, so every bright thing becomes a wide haze) is replaced by a soft threshold on the FLOAT scene buffer (`r_viewfbo 2`, which `r_edr` and `r_metalfx 2` force) at half resolution, a downsample chain of halvings (the 13-tap box), and a tent upsample summed back up it; the composite that adds the result is the old one, untouched. A muzzle flash, a lava pool or a bolt core gets a hot core with a tight glow that falls off over the room. The six old knobs (`_resolution`, `_colorexponent`, `_colorscale`, `_colorsubtract`, `_brighten`, `_blur`) are IGNORED while it is on -- the composite's subtract is forced to 0 -- and the four below shape it. Cost, measured 2026-09-16 on the rebooted machine (one boot, frozen e1m3 spawn, 1920x1080, his config with the murk and the scaler off, the cvar toggled every 4 s, `METAL_FRAMEMS=1`): the renderer's command buffer 2.845 ms with the 2001 chain at his `r_bloom_resolution 128` against 2.950 ms with this chain at 960x540 -- **+0.105 ms, +3.7% of the renderer's GPU time**, every window of one arm above every window of the other; it replaces the old chain's passes rather than adding to them. 0 = the 2001 blur exactly, byte for byte. Console-only; awaiting Seb's eye. |
| `r_bloom_m5_threshold` | 1 | Scene brightness (the brightest channel) above which a pixel blooms. 1 = only what is brighter than white: on the float buffer that is the flash, the lava, the bolt and the hottest lights; on an 8-bit scene buffer (`r_viewfbo 0`) nothing is, so there it wants ~0.6. |
| `r_bloom_m5_knee` | 0.5 | How softly the threshold engages, in the same units: from threshold-knee to threshold+knee the bloom fades in along a quadratic, so a torch at 0.7 gives a little and a flash at 3 gives everything. 0 = a hard cut. Clamped to the threshold. |
| `r_bloom_m5_intensity` | 0.35 | How much of the blurred bright light is added back. Linear over the whole chain; the composite clamps the add at white per channel, as it always has. |
| `r_bloom_m5_levels` | 5 | How many halvings the chain descends (1-8); each widens the glow by about a factor of two. 3 is a tight halo, 5 reaches across a room, 7 a haze. A level smaller than 2x2 is skipped. |
| `r_redglow` | 1.5 | **Red burns**: saturated reds emit light of their own — the warning bands on Ogre grenades, red buttons, health boxes, and anything else genuinely red. Stock Quake gives none of these a glow layer, so they used to sit as dead as the stone around them. 0 disables (toggling rebuilds the shaders, a one-off pause). Gibs and severed heads are deliberately left out of it — they are red, but glowing viscera is not the idea. Corpses are not: a dead monster is still the monster's own model, so its red details keep glowing. |
| `r_redglow_threshold` | 0.45 | How red a texel must be before it emits. Lower catches more; too low and the brown brick starts to glow, because brown is dark orange. |
| `r_redglow_minlevel` | 0.22 | How bright a red texel must be before it emits. Keeps dried blood and dark maroon out of it. |
| `r_lavaboil` | 0.6 | Boiling churn: the lava texture domain-warps under drifting 3D noise and the glow pulses in slow hot spots. 0 = the old static sheet (toggling rebuilds shaders — a one-off hitch). |
| `r_waterswirl` | 0.5 | Water and slime surfaces churn gently under the same noise volume, on top of the classic scroll — the sheet stops reading as a sliding decal. 0 = the old look exactly (toggling with `r_teleportswirl` also 0 rebuilds shaders — a one-off hitch). |
| `r_teleportswirl` | 1 | Teleporter starfields churn under the noise volume. 0 = the old look exactly. **Note it is a gate, not a dial** — the shader tests `> 0`, and the value scales only the churn's *amplitude*; measured across an 8000× range of the knob the speed was flat. |
| `r_teleportswirl_pivot` | 2 | What the starfield rotates about. **2 = nothing** — the rotation is off and the churn is the whole animation. 1 = the nearest texture tile centre. 0 = the pre-2026-09-01 behaviour, a fixed texcoord (0.5,0.5). Q1 texcoords are absolute world coordinates over the texture size, so that fixed pivot sits near the **map origin** and a pad N tiles out rotates on an N-tile lever arm: start.bsp's own 24 teleport faces measured **94 to 999 px/s, a 10.6× spread inside one map**, and its three skill pads read 121 / 271 / 439 px/s at an identical standoff. A tile pivot removes the spread but seams at every tile boundary, because a rotation cannot be both uniform across surfaces and continuous within one — hence the shipped 2. `0` with `r_teleportswirl_churn 1` restores the old frame **byte for byte**. |
| `r_teleportswirl_churn` | 8 | How fast the starfield's noise warp evolves, as a multiple of the historic rate. The warp is evaluated on the *already-rotated* coordinate, so before this it was a fixed deformation swept past by the rotation rather than something you watch change — which is why the churn was invisible in play. Measured on the start.bsp curtain the motion goes 2.73 / 7.16 / 9.14 / 10.63 at rates 1 / 4 / 8 / 16 and saturates past 16, so 8 is the knee. 1 = the historic rate. |
| `r_watersurface` | 1 | **The water surface refracts what lies beneath it** (WATERSURFACE, 2026-09-12; remade like for like against FTE's measured style 2 and **shipped ON on Seb's eye, 2026-09-13**). Inert until the water renders blended, i.e. `r_wateralpha_force` on a stock map. Two things make it FTE's look rather than a knob: **the pool floor is drawn** — on a map without transparent-water vis the air's PVS never reaches the leaves below a water surface, so the pool interior was culled and the refraction had nothing but murk to bend; FTE renders its refraction with the PVS of the leaf just below each visible water surface, and this does the same by merging that PVS into the view's (one `FatPVS` per pool), so the floor is drawn into the frame the copy is taken from, lit by the RT term and fogged by the murk like any floor — and **the ripple is read off the water texture's 64×64 level** (a mip bias to log2(width/64)), so a hi-res replacement pack never changes it, exactly as FTE derives its water normal map from the palette texture. Two layers at 0.2× scrolling 0.1 / 0.097 tiles a second across each other, their luminance gradient as per-layer unit normals summed and renormalised, refraction 0.1 of the screen, the Fresnel (exponent 5) on the rippled normal, the classic sine warp on the surface texture, tint 0.7 0.8 0.7. The murk's fade still applies, and the tint and the displacement fade with it, so far water is the murk's own pixel. Both backends. Like the murk it forces the offscreen scene path with a sampleable depth. **Cost, measured 2026-09-13: +0.66 ms of renderer GPU (+6.9%) parked on an all-water frame (`METAL_FRAMEMS=1`, fullscreen), and by paired fullscreen timedemos on a soaked machine (witnesses 0.1-0.5%) 3% of the frame on the e1m2 moat (demo18), 9-10% on the e1m1 slime hall (demo19) and 5-7% on e2m4 (demo26, a tier bed with 123 water leaves) -- the whole feature, the drawn interior included; nothing where no blended water is in view. The tier figures below were taken before the water shipped, so on the watery tier beds (demo24, demo26) they are a few percent high** — the drawn interior, the frame copy and the liquid shader together. 0 = the old sheet and the old visible set exactly (toggling rebuilds shaders — a one-off hitch). `exec watersurface_on.cfg` (the shipped look) / `watersurface_fte.cfg` (FTE like for like: crystal-clear, no taper) / `watersurface_big.cfg` / `watersurface_off.cfg`. |
| `r_watersurface_distort` | 0.1 | How far the surface displaces what lies beneath it, as a fraction of the screen at the ripple's full tilt. **0.1 is FTE's own strength** (its `STRENGTH_REFR`: "0.1 = fairly gentle, 0.2 = big waves"). Scaled by the murk's fade so a surface the fog has dissolved does not go on shimmering the fog beneath it, and tapered to nothing where the floor is within 48 units of the surface, so the water's edge is not smeared. 0.2 is the exaggerated arm. |
| `r_watersurface_warp` | 1 | The classic per-pixel wobble of the water's own texture — FTE's `defaultwarp`, `tc + sin(tc + time) × 0.125` — as a multiple of that amplitude, so the **surface** reads as rippling and not only what is beneath it. 0 = the texture sits still and only the swirl churns it. |
| `r_watersurface_bump` | 4 | How steep the ripple's normal is, read out of the water texture's own luminance gradient (FTE makes its water normal map from the diffuse the same way; 4 is tenebrae's bumpscale). Higher = a more broken, more sparkling surface and a stronger refraction at the same `_distort`. |
| `r_watersurface_speed` | 1 | Pace of the ripple. 1 is FTE's own: two layers of the water's texture, magnified 5×, scrolling at 0.1 and 0.097 tiles a second across each other (a feature crosses its own width about every second — the rapid ripple), and the surface warp at one radian a second. 2 doubles it. |
| `r_watersurface_opacity` | 0 | The surface texture's share when you look straight down into the water. 0 is FTE's (no texture floor straight down; the texture arrives only through the Fresnel as the angle flattens). |
| `r_watersurface_fresnel` | 5 | The Fresnel exponent on the *rippled* normal. 5 is FTE's effective exponent (measured 2026-09-13: its `#FRESNEL=4` is a dead define). Shares at 10 / 15 / 20 / 30° of elevation: 39 / 22 / 12 / 3% texture. Lower shows the texture sooner. |
| `r_watersurface_taper` | 48 | World units below the surface over which the displacement fades to nothing, so the floor at the water's edge is not smeared out from under it. FTE has no taper at its stock styles (a hard seam at the far bank); 0 reproduces that, and `watersurface_on.cfg` sets 0 for the like-for-like. |
| `r_watersurface_tint_red` / `_green` / `_blue` | 0.7 / 0.8 / 0.7 | Tint of the view refracted through the water (FTE's `TINT_REFR`). Fades with the surface: at far water the copy is already the murk's own pixel, and tinting that painted distant pools as green shapes cut out of the fog. |
| `r_watersurface_clear` | 1 | How thick the in-water murk is when you look **into** water from the air, as a fraction of `r_volumetric_waterdensity`. **1 = the same murk as when submerged** — Seb's choice ("1.0 is right. That is perfect"): deep pools hide their bottoms, shallow ones stay readable, and the transition at the waterline is eased over a fraction of a second either way. 0 = FTE's crystal-clear water from the bank (`watersurface_fte.cfg`). Reaches every density consumer (the GL march, the fog kernel, the fade, the particle hook, the probe, the slime and lava inherits). |
| `r_watersurface_guard` | 1 | Reject a displaced sample that lands on something *nearer* than the water surface — a torch on the bank would otherwise be pulled into the pool. Reads the scene depth, so it stands down on the bare direct path (no depth texture) and under `r_transparentdepthmasking`. 0 = the raw displacement, for A/B. Console-only. |
| `r_lavaflow` | 1 | Lava **flows** as well as boils: the crust drifts bodily across the sheet instead of only churning in place. It rides the boil's existing noise lookups, so it costs two instructions and no extra texture fetch. 0 = the old churn exactly. |
| `r_lavaflow_speed` | 0.03 | How fast the crust drifts, in noise-volume widths per second. |
| `r_lavashimmer` | 4 | **Heat haze.** Lava-heated air refracts whatever you look at *through* it, in pixels of displacement — the one cue in the game that says *dangerous* before you are standing in it. Each pixel's view ray is sampled at a few points between your eye and what it hits, so the amount of ripple is the amount of hot air the ray crossed: a wall on the far side of a lava lake wobbles, and so does a monster standing in one. Needs the Metal renderer. 0 = off, and the shader is not compiled at all. |
| `r_lavashimmer_height` | 128 | How far lava heats the air around it, in world units — the thickness of the column a view ray has to cross to be bent. **Silently capped** at twice `r_volumetric_fieldcell` (so 128 at the default 64-unit cell): past that the baked field cannot tell how far from lava a point is, and anything relying on it would haze the whole map. With `developer 1` the console reports the reach actually used on each map. |
| `r_lavashimmer_taps` | 6 | How many points along each view ray are tested for hot air (1–16). More grades the haze more finely; fewer are cheaper and read more like an on/off mask. Measured cost between 6 and 16: inside the run-to-run spread. |
| `r_lavashimmer_path` | 192 | How much hot air a view ray must cross for **full** shimmer, in world units. Lower makes a thin wisp ripple as hard as a whole lake; higher reserves the full effect for looking the length of one. This is the strength knob to reach for if the haze feels too eager or too shy. |
| `r_lavashimmer_scale` | 0.25 | Size of the heat eddies. **Higher is finer and more shimmer-like**; low values read as the masonry slowly bending, which is what the first version of this effect did wrong. |
| `r_lavashimmer_speed` | 0.55 | How fast the hot air rises through the haze. Heat shimmer is rapid — slow values read as a wobble. |
| `r_lavashimmer_dist` | 1200 | How far away a surface can be and still ripple. Also the cheap early-out — past it a pixel costs one depth sample and nothing else — and the length of the march that looks for hot air. |
| `r_lavaglow` | 1.5 | Lava's emission brightness. 1 = the classic look exactly; higher = hotter, with the brightest cells saturating white-hot. |
| `r_volumetric_lavaglow` | 0.5 | Warm glow the AIR picks up near lava, in the lava colour (`r_volumetric_lavacolor_*`). Soft-limited like the god-ray beams — it can never white the frame out. Needs the liquid murk (`r_volumetric_water 1`). |
| `r_volumetric_skyfog` | 0.8 | The most of the SKY the murk may take (0–1). Sky always keeps at least the remainder of its own colour, and the fog's share is rescaled to match, so a fogged skylight reads as sky seen through fog rather than fog instead of sky. Under the fog kernel it needs `rt_metal_skyopen` (on by default) to know where the sky is. 1 = uncapped, the old behaviour exactly. |
| `rt_metal_lavalights` | 1 | Lava lakes feed warm lights into the ray tracer (one per 256-unit patch, capped at 64/map with a console report): walls above lava glow orange with real shadows, and the fog kernel picks the light up. The value scales brightness. |

Scope notes: teleporters (and Scourge of Armagon's `*rift`) share lava's material
branch but are deliberately untouched this round; lava does not block shadow rays or
god-rays (it is an emitter, not an occluder — the same trade-off as the torch
light-cores).

---

## HDR and EDR (all console-only)

`r_viewfbo 2` and `r_hdr_shoulder` above give the scene *internal* range — brightness
above white survives to the end of the frame instead of clipping at the first opportunity.
**EDR is the other end of that pipe**: on a display running in macOS HDR mode, it hands
that above-white brightness to the compositor instead of flattening it, so a muzzle flash
or a lava pool can genuinely outshine the HUD rather than merely reaching the same white
the HUD is already at.

**Metal renderpath only** — the default since Phase 8 — and since the same round it is
**one switch**: `r_edr 1` (or the **HDR (EDR)** row on Options → Video Options) forces its
own prerequisites, so the float scene buffer and the analytic gamma curve come with it and
nothing needs setting up first. `r_edr_report` explains any refusal in a sentence.

| Setting | Default | What it does |
|---|---|---|
| `r_edr` | 0 | The master, and the whole switch — also the **HDR (EDR)** row on Options → Video Options. **1** = on wherever the display reports EDR capability; **2** = ask anyway on a display that reports none (a diagnostic, not a setting). Needs the Metal renderpath; while on, it forces a 16-bit float scene buffer (as if `r_viewfbo 2`) and the analytic gamma curve (as if `r_gamma_analytic 1`) by itself, so neither needs touching. First enable rebuilds the shaders — a one-off pause. |
| `r_gamma_analytic` | 0 | Evaluate the gamma curve instead of looking it up in a 256-entry table. Numerically the same curve, very slightly more accurate — but the table is **indexed by the colour**, so it can neither accept nor emit a value above white, which is why EDR forces this on while it runs. Setting it yourself is only needed for the curve without EDR. Toggling rebuilds the shaders (a one-off pause). Ignored, with a message, while `v_psycho` is on or `vid_sRGB` is in effect. |
| `r_edr_colorspace` | 1 | How the extended-range image is tagged: 0 none, **1 extended sRGB**, 2 extended *linear* sRGB, 3 extended Display P3. 1 is right for this engine; **2 is the trap** — the picture this engine hands over is already display-encoded, so calling it linear makes macOS apply the curve a second time and mid-grey shows far too bright. Nothing automated can check this one, so it is by eye. |
| `r_edr_stage` | 3 | Diagnostic. How much of the request to actually make: 1 = the request alone, 2 = also the colour space, 3 = also the 16-bit float drawable. Only 3 does anything useful; the lower stages exist because they are what proved which part macOS actually reacts to. |
| `r_edr_report` | — | Prints, for every attached display, the headroom macOS is granting right now, the most it *could* grant, and which display holds the game window — sampled over three seconds, because the grant is not instantaneous. If EDR is not engaging it says why in a sentence. |
| `r_edr_probe` | — | Measures the next frame **without** the clamp every other tool here applies: how far above white the brightest pixel goes, and how much of the frame is up there. Needs the app started from a terminal with `VID_METAL_PROBE=1`. |

**What to expect when it works.** The HUD deliberately stays at ordinary white while the
scene runs past it, so the two side by side in one frame are the test: if a muzzle flash
does not look brighter than the health digits, EDR is not reaching the screen. `r_edr 0`
should snap it back exactly. (An earlier version of this section said the desktop and menu
bar visibly dim when the grant lands. On Seb's MAG they do not, and the feature works
regardless — the grant was measured landing at 1.756 with no visible dim — so the dim is
not a tell to rely on either way.)

**Honest limits.** Screenshots are, and stay, the ordinary clipped image — there is no
above-white information in a `.tga`, and the whole verification apparatus of the Metal
work is denominated in 8-bit levels. So a screenshot cannot show you EDR, and neither can
a screen recording. The granted headroom also falls as you raise SDR brightness, and with
heat; this is normal, and the roll-off follows it frame by frame.

---

## M5 fun mods — Options → M5 Fun Mods

Most default off, and the Default column is the authority: the shell casing, noclip flight, the ball gun and four of the five weapon-feel switches ship on. Engine half in C, game half in the `m5` QuakeC (auto-mounted).

| Setting | Default | What it does |
|---|---|---|
| `m5_shotgun` | 0 | Doom-style shotgun: bigger warm muzzle flash, ejected shells, 10/21 pellets, tighter spread, harder kick. |
| `m5_shotgun_casing` | 1 | **On by default since 2026-09-02 (Seb's QA pass).** The ejected shell is a real 12-gauge hull — red plastic body, brass head — that tumbles end over end, instead of the round spark it has always been. The spin was always there — a random start angle and ±400°/s — but a round dot looks the same at every angle. Needs the engine's own particle atlas: a `particles/particlefont.tga` replacement pack supersedes it wholesale and the casing falls back to the round blob (reported once at startup under `developer 1`); a `particles/particlefont.txt` can repoint cell 34 deliberately. Console-only. |
| `m5_dust` | 512 | **Ambient dust** (SEPTEMBER S7, 2026-09-02): how many motes to keep drifting in the air around you. No ambient particle system existed before — every particle in the engine was event-driven. An ABSOLUTE count, deliberately not scaled by `cl_particles_quality`; spawned in open air within `m5_dust_radius`, never inside a wall or a liquid and never behind a wall (one traceline per candidate, at most 32 spawns a frame), recycled when they drift past twice the radius or their 8–16 s life ends, and yielding to effects — the emitter stops while the pool is within 512 of full. Drawn as a crisp disc (atlas cell 35, so a mote is a point rather than a smudge), each mote taking a colour from the bonfire-ash palette (`m5_dust_tint`) and lit like any other alpha particle -- which is why `cl_particles_lighting` ships at 2 beside it; the payoff is dust catching torchlight, a muzzle flash, a rocket's glow or the handlamp. 128–512 is the sensible range; the cost is the lit-particle figure above (a few hundredths of a millisecond) plus the eddy arithmetic. Console-only. |
| `m5_dust_radius` / `_size` / `_alpha` / `_speed` / `_swirl` | 320 / 1.2 / 0.35 / 6 / 1 | Where the dust lives (world units from the eye), mote size, opacity at birth, birth drift (units per second) and the strength of the slow eddying that keeps it turning rather than coasting (0 = straight, damped drift). Console-only. |
| `m5_torch_embers` | **3** | **Torches that live** (BEAUTY B4, 2026-09-17): embers a second rising off every torch, brazier and candle flame in view — the light-core models (`flame.mdl`, `flame2.mdl`, AD's braziers, candles and lantern), walked from the static entity list in the relink window beside the dust. Each ember is a hot grain (the ember cell, 39) born in the flame's upper half, thrown up and a little out, rising against a weak negative gravity and damped by air friction, its colour picked between a bright orange and a dull red so the population cools, gone in one to two seconds. Additive, so it is the ember's own glow. Torches beyond 1024 units, behind the eye or behind a wall (one traceline per ember) spawn none; at most 16 spawns a frame, yielding to the pool like the dust. 0 = none. 2–4 is the sensible range. Console-only; `exec embers_on.cfg` (3) / `embers_off.cfg`. |
| `m5_dust_tint` | **1** | **The BONFIRE-ASH palette, and the default since 2026-09-19** (Seb: *"tone down the white dust so it's more like flecks of blue/grey/brown bonfire ash. currently it's bright white -- which it can be when lit with muzzle flashes -- but it's just too white and noticeable most of the time"*). Each mote takes one of sixteen weighted colours: soot x3, charcoal x2, charred brown x2, a warm grey x2, a cool grey x2, slate blue x2, one brown, one pale ash and **one off-white in sixteen**. **THE OLD RAMP IS WHY IT WAS WHITE, and it is arithmetic rather than taste**: `CL_NewParticle` lerps its two endpoints per particle, so soot-to-white was a UNIFORM population -- **mean mote luma 143 of 255 with 57% of motes above mid-grey, against the palette's 82 and 12%**. The 2026-09-06 palette (soot, two browns, khaki green, slate blue, off-white) is its ancestor and measured 86; his word this time was *grey* rather than green, so the green became two greys and the dark end gained weight. 0 restores the ramp byte for byte (measured: 0 of 2.7 MB against the pre-change binary on a bed whose own two-boot floor is 0). Recipes `exec dust_ash.cfg` / `dust_ash_dark.cfg` / `dust_white.cfg`. **The palette alone is not the whole fix** -- see `cl_particles_lighting_static`. |
| `m5_muzzleflash` | 1 | **Drawn muzzle flash** (BEAUTY A3, 2026-09-13; **DEFAULT 1 since 2026-09-16** on Seb's QA -- his config had archived 1 since the flash passed, so the line drops out on his next quit; the parity preamble pins 0, and smoke run B states 1 on its boot rather than relying on the default). Quake has never drawn one: `EF_MUZZLEFLASH` is a dynamic light and nothing else, so with the murk on the "flare" is the light scattered round a barrel that shows no flash of its own (Seb's demo45 f540). 1 = a starburst with a hot core at the muzzle of the shotgun, super shotgun, nailguns, grenade and rocket launchers — the muzzle is read off the VIEW WEAPON'S OWN animated mesh each frame (the forward-most vertices, so it follows every weapon and every recoil frame with no table), one single-frame particle per frame for 70 ms decaying linearly, drawn over the weapon with the depth test off (a burst centred on the bore is otherwise hidden by the barrel's end face, which draws into the compressed view-weapon depth range) and exempt from the murk's fade (the gun is not fogged; at his density the muzzle fifty units out sat at a fraction of the transmittance) and from the additive alpha clamp, so on the float scene buffer it goes above white for the bloom. A smaller one on every monster's gun as it fires, at the muzzle light's own point. Under the same switch the stock bullet-impact spray takes the hot-core STREAK cell (38) at a third of the thickness — the wide yellow rays on demo45 f954 were the impact and casing sprays drawn with the round blob at point-blank range — the impact's grey blob becomes a small starburst, and the M5 shotgun's muzzle spray becomes the gun-attached streak spray in the row below. The thunderbolt, the ball and the axe draw nothing of this kind. Cells 37 (flash) and 38 (streak) are procedural; an external `particles/particlefont.tga` replaces the atlas and both fall back to the round blob. 0 = the 1996 picture, the light alone. `exec flash_on.cfg` / `flash_off.cfg`. Console-only. Seb's QA 2026-09-15: the flash passes ("the flash is fine", the rocket launcher's included); the shotgun sparks were reworked on his notes (below) and PASS ("sparks are fine"). On the nailgun the flash takes its barrel from the weapon frame's parity (odd right, even left, the QuakeC's and the model's own alternation) rather than the forward-most tip, which lags a frame behind the model's lerp and put the first, brightest flash frame on the barrel that fired last. |
| `m5_muzzleflash_forward` / `_up` / `_size` | 0 / 0 / 1 | Trims on the local flash: world units along the gun past the mesh's own tip, world units above it, and a scale on the burst. `M5_FLASHDEBUG=1` in the environment prints the muzzle derivation per spawn. |
| `m5_muzzleflash_sparks` / `_speed` / `_spread` / `_length` / `_thickness` / `_life` / `_brightness` | 1 each | **The shotgun's muzzle sparks** (2026-09-15, on Seb's QA: "too slender and weedy … dont stick to the gun barrel (like they're drawn off centre and also obviously seem to drift when we're strafing)"). Under `m5_muzzleflash 1` with `m5_shotgun 1`, the shotgun and super shotgun throw 8 / 14 hot-core streaks (cell 38) from the view weapon's own muzzle (the flash's mesh read) along the barrel. The spray lives in the WEAPON'S OWN FRAME (`M5_MuzzleSparks_Update`, the relink window): each spark is advanced there and drawn each frame as a one-frame particle through the weapon's matrix, placed back by the particle update's pending step, drawn like the flash (over the weapon, unfogged, above white). So it stays on the barrel through a strafe, a turn and the bob, and each streak points along its motion relative to the barrel. The three causes of the first cut, each seen on a scripted strafing demo: it started at a fixed point in the PLAYER MODEL's frame (near mid-screen, well above the barrel tip), at packet parse (a frame before the view weapon moves), and in world space with no inheritance (a strafe slid the gun out from under it); a world-space re-anchor at the tip with the player's velocity added kept position but slanted every streak along the strafe, because a spark streak is drawn along its world velocity. Base values: thickness 1.4 (was 0.5), streak 0.5 × speed, push 320 u/s plus 10 up, scatter 150, alpha 255–400 fading 1300 a second over 0.3 s, a third of gravity. The knobs scale those: count (0 = none, up to 10), speed (push and scatter together; 0.1–5), spread (scatter alone), length (streak alone; 0 = points), thickness, life (lifetime up and fade down together), brightness. A chase camera has no view weapon: the spray leaves the model's muzzle point as world particles carrying the player's velocity. The `m5_muzzleflash 0` spray is the 2026-09-12 one, untouched. Console-only; `exec sparks_reset.cfg` puts all seven back. |
| `m5_bullettime` | 0 | Enables the `+bullettime` bind: hold to ease the world into slow motion. Local games only. |
| `m5_bullettime_scale` | 0.3 | How slow bullet time gets. |
| `m5_bullettime_ramp` | 6 | How fast it eases in/out (console-only). |
| `m5_movement` | 0 | 0 classic, 1 CPM air control, 2 easy bunnyhop (hold jump). |
| `m5_noclipfly` | 1 | Noclip flies where you look: forward moves along the full view direction, pitch included, at full speed — no more swimming up and down to change height. The swim keys still add straight up and down. 0 restores the classic yaw-only noclip. |
| `m5_gore` | 0 | 0 vanilla, 1 bloody, 2 ludicrous. |
| `m5_burn` | 0 | **Lightning Ignites**: the lightning gun sets what it hits on fire — flame, smoke and burn damage over time, with the victim charring as it burns. 1 = 4 seconds, 2 = ludicrous (8 seconds, double the bite). The fire carries a real dynamic light, so a burning monster lights the fog, casts ray-traced shadows and lights the walls around it. It cuts both ways: a Shambler's bolt sets **you** alight too. |
| `m5_powerupglow` | **1** | **The quad and the pentagram GLOW rather than spotlight** (2026-09-19). Vanilla gives a powered-up player `EF_DIMLIGHT`, which the engine renders as a fixed WHITE light of radius 200 at colour 1.5 on every channel — and under `rt_metal_walllight` that is 1.5 × 0.8 × 6 = 7.2 against an `rt_metal_lmax` shoulder of 2.5, so the near field saturates flat and the pool ends in a hard rim. Seb: "we are like a white spotlight, casting hard spots, not a glowing blue like in FTEQW". **The white light is 1996's, not ours** — what changed is the lighting underneath it. At 1 the powerups take a `PFLAGS_FULLDYNAMIC` light instead (the ball lightning's own mechanism, and the look he passed there): **blue** for the quad, **red** for the pentagram, magenta for both, at `m5_powerupglow_radius`. Twice the radius at the same peak is a gradient half as steep — a glow rather than a spot — and at 0.9 in the dominant channel the term peaks near 2.4, just under the shoulder, so nothing clips. The value doubles as the intensity (0.5 = half). 0 restores the stock white exactly; `m5_stock` forces the 1996 answer with it. `exec quadglow_on.cfg` / `quadglow_off.cfg` / `quadglow_soft.cfg`. |
| `m5_powerupglow_radius` | 400 | Radius of the powerup glow in world units (`EF_DIMLIGHT`'s own is 200). A light contributes nothing past its radius and the falloff is squared, so a wider radius at the same peak is a SOFTER pool rather than a bigger one — which is the whole of the fix. |
| `m5_venom` | 0 | **Scrag Acid Venom**: the Scrag's spit reads as acid. A yellow-green light rides the projectile — so it lights walls, monsters and the volumetric fog as it flies, and casts ray-traced shadows — the trail burns additive instead of flat green, and the impact sizzles. That last part fixes an omission in vanilla: a spit landing on flesh drew **no impact effect at all**. |
| `m5_venom_trail` | 1 | Retint and brighten the spit's particle trail. 0 keeps the stock green trail while leaving the light and the sizzle (console-only). |
| `m5_balllightning` | 1 | **Ball lightning, the ninth weapon** (BALLLIGHTNING.md, 2026-09-06; **on by default** since the same evening, Seb: "bake it into the game" — key 9 is bound to it by the engine unless your config binds 9 to something else; a **Ball Lightning** row on the M5 Fun Mods page switches it since 2026-09-10). Owning the thunderbolt also grants a launcher for a slow sphere of plasma that drifts down the aim line, arcs at the two nearest living things it can see every tenth of a second (a landing arc burns brighter, the bolt's own connect signal), leans toward what it is zapping, discharges when its life runs out or it touches water — a blink, a shockwave ring, sparks and a flash — and on a wall or door simply stops and fizzles out in a crackling strobe. The ball is drawn as a plasma globe — on Metal a real translucent SPHERE in the bolt's distance-field pass, a dark sun boiling with plasma, rim-lit, dimming what lies behind it (`r_lightningbeam_m5_ballcolor_*` is its palette, `r_lightningbeam_m5_ballsize` its radius) — with fine violet filaments curling out from it and grounding arcs snapping at nearby floor and walls; while its arcs are landing the core flares and the charge goes into one or two thick filaments, like a hand on the glass; its arcs are drawn as plasma filaments too (`r_lightningbeam_m5_ballarcs`); the volumetric fog shrouds the globe and the arcs by the fog between you and them, as it would a torch, carries a light in the bolt's own hue and flicker that scatters in the fog like the bolt's, leaves a short blue-white comet, and hums (the stock `ambience/buzz1.wav`). **Key 9** (impulse 202) selects it, press again for the bolt. Its body is a POINT (2026-09-19): a 20-unit box could never have done what its comment claimed, because Q1BSP carries only three clip hulls and `Mod_Q1BSP_TraceBox` rounds anything at or above `mod_q1bsp_zero_hullsize_cutoff` (3) **up to the 32×32×56 player hull** — and aligns it so the traced box stood 46 units above the ball's centre, catching every arch lintel the player ducks under by standing on the floor (Seb: "still fouls / grounds when firing on the threshold of doorways / arches"). At 2 units wide it takes the POINT hull, what a rocket has always used, so it passes anything with a gap at all; it still stops its own radius off a wall. Same cells as the bolt, paid at launch. View model is Dissolution of Eternity's plasma gun (borrowed, see `docs/CONTENT.md`) where that file is installed, and the thunderbolt's own model where it is not — a public download cannot include the mission pack's file, so the engine reports which at every map start (the read-only `m5_hasplasma`) and the QuakeC precaches and carries accordingly; the glow and burst are `m5/effectinfo.txt` blocks (`m5ball_glow`, `m5ball_burst`), tunable live with `cl_particles_reloadeffects`. 0 removes the weapon entirely. Console-only. |
| `m5_balllightning_arcs` / `_chain` / `_violence` | 4 / 140 / 1.2 | **The 2026-09-10 ball** (Seb: "more lethal and scary"). Every tick the ball now zaps and IGNITES every living thing it can see within its reach — not the two nearest — the damage falling with distance, and the fire takes whatever `m5_burn` says (plasma burns). `_arcs` is how many arcs it can DRAW at once (1–6; one invisible helper per arc past the first); `_chain` is how far a landed arc forks on from its victim to the next living thing the victim can see, up to two hops, at 70 percent of the damage, drawn from the victim — so a pack dies together (0 = no chaining); `_violence` (client-side, seconds) is how long the shell takes to go from the gentle bubble it leaves the gun as to full violence — the silhouette heaving harder and faster, the surface filaments lifting off it as prominences, the eruptions longer, more frequent and throwing sparks (0 = full violence from the start). `exec ball_v1.cfg` is the 2026-09-06 ball for A/B, `ball_v2.cfg` these. Console-only. |
| `m5_balllightning_speed` / `_life` / `_radius` / `_damage` / `_cost` / `_burst` / `_pull` / `_refire` / `_light` / `_charge` / `_fizzle` / `_arm` / `_grow` / `_wobble` / `_hit` / `_hitblast` / `_pressure` | 400 / 3.75 / 200 / 24 / 10 / 80 / 40 / 0.8 / 1.2 / 0.45 / 0.55 / 1 / 0.5 / 14 / 250 / 200 / 1 | The ball's numbers, all Seb's to re-cut in play: launch speed (units/s, also the cap the pull cannot exceed), seconds of life, reach for targets, damage per target per tick at the edge of its reach, doubled at point blank (the bolt does 30 to one; a direct or near hit in one room is an ogre kill), cells spent at the press, the discharge's radius damage (a rocket is 120), how far its heading bends toward the nearest prey (degrees per second at point blank, falling off with the square of the distance across its reach — a slight gravitational lean, never a turn; 0 never bends), the gap between launches with fire held, the brightness of the light it carries (radius 300), the charge — seconds between the trigger press and the ball leaving, the BFG's wind-up, shown as a sphere and light growing at the muzzle — the fizzle — seconds a ball that hits a wall takes to EARTH: it throws an arc into the surface with a pop, then more arcs to earth every sixth of a second, sparks and scorches at each, while the shell collapses into it (no blast damage; life running out or water still discharges with the burst and a thunderclap) — and the arming period, seconds after launch before the ball will turn on its OWNER as well (arcs, ignition, damage): a standing shot is always clear, chasing it is not — and the growth, seconds over which the ball swells from a third of its size at the muzzle to full size, a few feet out — the wobble, the amplitude (units/s) of a slow sinusoidal sway on its otherwise straight course — the direct-hit damage on a Vore or a Shambler (the globe itself touching anything else is fatal outright, and either way it DISINTEGRATES: the shell swells and blows apart, a flash, a ring, a storm of sparks, the thunderclap, a scorch), the disintegration's own SPHERICAL blast on everyone around the victim (200, a rocket-and-a-half, reaching 240 units — an ogre beside the victim is all but dead, and on Quad Damage, which multiplies it by four like every weapon, a direct hit clears the room), and the pressure switch for the drone rising in pitch as a ball comes within its reach of you (the matching violet EDGE cast on the screen is the renderer's, `r_lightningbeam_m5_ballpressure`). **Once the ball is armed it will turn on you: run after it and its arcs and its body are as lethal to you as to anything else.** Console-only. |
| `m5_explosion_sprite` | 0 | The 1996 explosion sprite (`progs/s_explod.spr`), drawn on top of the authored `effectinfo.txt` explosion. **Off by default since 2026-09-02** (Seb: it was the last thing in an explosion with a hard bottom edge against a surface seen at an angle); 1 brings it back. 0 removes it and only it — flash, fireball, smoke, embers, sparks, light and scorch are effectinfo layers and stay. It is the one part of an explosion the volumetric murk cannot fade (drawn after the fog pass, writes no depth), so in thick fog it reads bright and sharp while the new layers sit in the fog at their own distance. Read by the m5 QuakeC, so it changes id1/m5 only — the mission packs and AD run their own progs. Recorded demos keep whichever sprite was in force when they were recorded (console-only). |
| `m5_kick` | 1 | **Weapon Kick** (SEPTEMBER2 G, 2026-09-10; **on by default since 2026-09-16**, Seb: "feel_on is good. make that default"): a per-weapon screen kick, 0 = stock (every gun pitches the view 2 degrees, no more), 1 = the designed kick, up to 2. Each weapon has its own: the axe swing barely, the shotgun a snap, the super shotgun a heavier one with the eye shoved back, the nailguns a fine tremor, the grenade launcher a lob, the rocket launcher a real shove, the thunderbolt a continuous buzz, the ball gun a slow heave. The pitch is Quake's own `punchangle`; the shove is DarkPlaces' `punchvector` (the view origin pushed back along the aim, recovering at 20 units a second), which stock QuakeC never named. The Doom shotgun's harder kick still wins where it is the harder. Seb's QA 2026-09-15: "weapon kick feels fine". |
| `m5_grenadebounce` | 0 | **Grenade Bounces**: the stock bounce re-struck at a random pitch (78–122 percent, a shade lower on hard hits) with the volume following the impact speed, a dull tap once the grenade is barely rolling, and a 60 ms guard so a rolling grenade does not machine-gun the sample. 0 = one `bounce.wav` at one pitch, every time. **REJECTED by Seb's ear 2026-09-15** ("it sounds like a xylophone"): kept as an option, off by default -- the one feel switch that did not ship on when the other four did (2026-09-16) -- and `feel_on.cfg` sets it back to 0. |
| `m5_nailtracer` | 1 | **Nail Tracers** (**on by default since 2026-09-16**): your own nails and super nails leave a short hot streak (`m5/effectinfo.txt` `m5nail_trail`, additive particles gone in a tenth of a second), so a burst is visible in fog where a bare nail model is not — the murk fades it by distance like every particle. Player nails only, and no light per nail, so the ray tracer and the fog kernel see nothing new. On every `m5_nailtracer_every`'th nail. |
| `m5_nailtracer_every` | 3 | A tracer on every Nth of your nails (a countdown on the player, so the first nail of a map carries one and the cadence runs across bursts): 3 = every third (2026-09-15, Seb: "make tracers only every 3rd or fifth round"), 5 = every fifth, 1 = every nail. Console-only; an engine without the cvar reads 0, which is every nail. |
| `m5_nailbarrels` | 1 | **Nails From Barrels** (**on by default since 2026-09-16**; 2026-09-15, Seb: "It has *two* barrels and definitely needs two side by side streams of alternating nails ... they were too wide before ... the nails want to start from the nailgun muzzle flash positions"): the nailgun's two alternating streams leave its two barrel tips. The QuakeC fires the RIGHT barrel on odd weapon frames and the LEFT on even ones (`player.qc` `W_FireSpikes(4)` / `(-4)`), and `v_nail.mdl` pushes its nail out of the same barrel on the same frames; its barrels sit 3.7 units either side of the eye and 13.7 below it, identically in the AMI and id models (measured from both files' vertices). id starts the nail 4 to the side and 6 below the eye, which projects onto the screen along a wider line than the barrel, so each stream seemed to leave from outside it. At 1 `M5Feel_NailOrigin` keeps id's 6 below the eye, along `v_up` so it holds at any pitch, and uses 1.6 sideways, which puts the nail on its barrel's own screen line: hidden inside the barrel (the view weapon draws over everything), it appears at the tip where the flash is, and it still hits where id's did. The super nailgun keeps id's centre line. 0 = id's origin exactly. A first version the same day (`m5_nailcentre`) put every nail on the centre line; Seb: "now it's just one centreline - oops". His verdict on the two streams, 2026-09-16: "perfect". |
| `m5_axesparks` | 1 | **Axe Sparks** (**on by default since 2026-09-16**): an axe blow on a wall throws a burst of hot sparks off the surface (`m5axe_sparks`) with a blink of warm light and a ricochet ring at a random pitch, on top of the stock chip effect and thud. Flesh hits are unchanged. Seb's QA 2026-09-15: "Axe sparks are fine". |
| `m5_horde` | 0 | Wave-survival horde mode on any map (takes effect next map load). |
| `m5_horde_director` | 0 | **Horde Director** (SEPTEMBER2 H, 2026-09-10): the next wave is shaped by how you are doing. At each wave's start the director reads your health and armour, your ammo for the guns you own, how fast the last wave fell (against 2.5 seconds a budget point) and how low your health went in it, into one score from −1 (you are on the ropes) to +1 (you are fat): the points budget is multiplied by 0.6 to 1.5, the species roll tilts toward rabble at −1 (light 60 percent, heavies 5) and toward the heavies at +1 (light 20, heavies 45), the break after a wave runs 5 to 14 seconds instead of 8 (long after a hard wave, short when you are cruising) and the supply drop grows up to double after a wave that hurt. 0 = the fixed budget (18 + 15 × wave, scaled by skill). `impulse 211` prints the score, the multiplier and the break. Takes effect from the next wave. Row on the M5 Fun Mods page, greyed until Horde Mode is on. `test/hordesoak.sh` is the headless soak that reads it on a scripted fight. |
| `m5_horde_best` | 0 | Best-wave record (written by the game; console-only). |
| `m5_horde_species` | 0 | Test hook: force every horde spawn to one species (1 soldier, 2 dog, 3 knight, 4 scrag, 5 enforcer, 6 ogre, 7 hell knight, 8 zombie, 9 demon, 10 spawn, 11 vore, 12 shambler). Wave unlocks and the wave budget still apply. Console-only. |
| `m5_photomode` / `photomode` | 0 | Freeze the world, fly free, hide HUD and weapon; everything restores on exit. |

**Why the Scrag and not, say, the Vore.** `progs/w_spike.mdl` has no palette-fullbright
texels at all, in either the id1 copy or the AMI replacement, so it has no glow layer and
`.glowmod` is inert on it — there is nothing on the model that can be told to emit. Its
dominant palette indices are 194–200, already a mustard yellow-green: the colour was never
the problem, the model was simply unlit. (The *knight* spike sits at 224–239, the fullbright
ramp, and has glowed for free since 1996. That asymmetry is why one of Quake's two spit
projectiles always looked hot and the other did not.) So the effect is carried by a real
per-entity dynamic light rather than by the skin. A further step would be an authored glow
skin for the spike, which is a content drop rather than a code change.

Bindable impulses: 200 cycle gore, 201 toggle shotgun, 202 select ball lightning (needs
`m5_balllightning 1`), 210 next wave, 211 horde status,
222 pentagram, 223 quad and 224 ring of shadows (those last three need `sv_cheats 1`, and
each lasts 30 seconds).

**`notarget` in horde mode** (needs `sv_cheats 1`): monsters ignore you completely and fight
*each other* instead — the wave keeps spawning and killing itself while you watch. Shoot one
and it turns on you, exactly as vanilla notarget does; everything else carries on brawling.
Type `notarget` again to switch it off. The Ring of Shadows is gentler: monsters will not
newly notice you, but anything already chasing you keeps coming.

---

## The bad torch — bind a key to `torch`

A handlamp you carry: a warm, unreliable cone that follows your view. It is **off by
default and player-toggled** — it changes how a level reads, so it is never on unless you
ask for it. There is no menu row; bind it on **Options → Customize Controls** (the row is
called *torch (handlamp)*), or type `bind f torch` in the console. An engine default bind
cannot beat your saved config, so it has to be bound once by hand.

Colour × brightness are baked into the light itself, so the ray tracer's shadows, the fog
kernel's scattering and the god rays all follow it with no extra switches. With
`gl_flashblend 1` — your setting — the beam, its shadows and its scatter through the murk
all come from the RT sidecar; there is no raster fallback, exactly as for the thunderbolt.

| Setting | Default | What it does |
|---|---|---|
| `m5_torch` | 0 | The lamp itself. The `torch` command toggles it. |
| `m5_torch_radius` | 300 | How far it reaches, in world units. **Radius matters far more than intensity** — a light contributes exactly nothing past it, so a dim lamp with a big radius beats a bright one with a small one. |
| `m5_torch_intensity` | 1 | Overall brightness. |
| `m5_torch_cone` | 28 | Half-angle of the beam in degrees (5–80). Around 15 reads as a spot, 70 as a lantern. **How soft the rim is, is `m5_torch_softness` — and until 2026-09-01 it was not soft at all.** |
| `m5_torch_softness` | 0.8 | The fraction of the way from the cone's outer edge to its axis at which the beam reaches full brightness — i.e. how much of it is a flat top rather than a gradient. **At the historic 0.35, 80.3% of a 28° cone is a flat plateau**, which is exactly what read on a wall as a stencilled disc; 0.8 takes that to 44% with no loss of peak brightness, and 1 removes it entirely at about 9% off the beam centre. 0.35 reproduces the pre-2026-09-01 kernel exactly, and any light that never sets it (which is every producer but the lamp) still gets the old fixed shoulder. |
| `m5_torch_filament` | 0 | 0-1: how strongly the beam shows its bulb -- a hot core about a third of the beam across (up to twice the skirt's brightness at 1) with a faint dark ring outside it, the way a cheap reflector images its filament. Applies wherever the cone does: the wall patch, the scatter in fog and the bounce. 0 is the plain cone bit for bit. (2026-09-06.) |
| `m5_torch_flicker` | 0.35 | Depth of the constant grumble (0–1). Hard-clamped in code so the lamp can never fall below 4% of peak whatever you set — see below. |
| `m5_torch_kelvin` | 2850 | Colour temperature of the healthy lamp (1500–3500 K). 2850 is ordinary tungsten. |
| `m5_torch_kelvin_dip` | 1900 | The temperature it sags to during a brownout. Lower is redder, which is what a cooling filament really does and is most of what sells "failing" over "someone turning a dimmer knob". |
| `m5_torch_lag` | 18 | How tightly the beam tracks your view. Lower lags further behind, as though the lamp were swinging in your hand. |
| `m5_torch_sway` | 0.5 | Idle drift when you stand still. 0 pins the beam to your view exactly. Halved from 1 on 2026-09-01. |
| `m5_torch_fogweight` | 3 | How loudly the lamp scatters in the volumetric fog, independently of the light it casts on walls. **The beam in air was always there** — the cone is applied in the fog and shaft kernels, not just on surfaces — it simply read too faint; raised from 1.5 on 2026-09-01. Measured on the e1m3 spawn with the murk on: frame mean +5.1% at 3 and +9.9% at 5, and a whole-frame mean understates a beam that occupies a few percent of it. |
| `m5_torch_dip_period` | 12 | Seconds between ordinary brownouts — a 0.10–0.30 s sag to 20–40% of peak. Was a hard-coded 22; doubled in frequency on 2026-09-01. Floored at 0.6 in code, because the event length is drawn from that same window and a period shorter than the longest event turns every slot into one continuous dip. |
| `m5_torch_swoon_period` | 55 | Seconds between the rare deep swoons — a 0.35–1.00 s sag to 5–10% of peak, i.e. very nearly out. Was a hard-coded 105. Floored at 2.0 for the same reason. **Neither period can build a strobe**: the flicker floor below is independent of both. |

### It is meant to be a bad torch

Three signals multiply together, all pure functions of time: a constant pink-noise grumble,
an ordinary brownout every ~22 seconds that dips to a third for a fraction of a second, and
a rare deep swoon roughly every 105 seconds that nearly kills it for up to a second. Each
dip is a raised cosine, so the lamp swoons and recovers rather than stepping — a step would
read as a strobe. The colour reddens as it falls.

### The flicker floor is a guarantee, not a setting

The lamp can never go dark. A lamp that reaches zero is a strobe, and 3–30 Hz is the worst
band there is, so the floor is a compile-time constant no cvar can reach past.
`m5_torch_test` sweeps 300 seconds at 1 kHz with the depth forced far outside its bound and
reports the minimum, plus a separate check that the backstop itself is live — the smoke
suite greps both verdicts.

## Display, frame rate and vsync

None of these are set in a fresh config, so out of the box you get: **fullscreen at the
desktop's own resolution and refresh rate, vsync off, and no frame-rate cap.**

| Setting | Default | What it does |
|---|---|---|
| `vid_desktopfullscreen` | 1 | Use the desktop's resolution and refresh rate. **This is why the game may not be running at the resolution you think it is** — it follows the desktop, not a game setting. |
| `vid_width` / `vid_height` | — | Only consulted when `vid_desktopfullscreen 0`. Set both, plus `vid_fullscreen 1`, to pin a resolution. |
| `vid_vsync` | **1** | 0 off, 1 sync to every refresh, −1 adaptive (stops syncing when a frame runs late). **Default on since the Phase 8-6 QA** — the 240 Hz recommendation below became the shipped default. Forced off during timedemos, so benchmark numbers are never affected by it. |
| `cl_maxfps` | 0 | Frame-rate cap; 0 is uncapped. |
| `vid_refreshrate` | 0 | 0 uses the display default. Only consulted when `vid_desktopfullscreen 0`. |
| `vid_renderer` | **metal** | Renderer backend, applied on `vid_restart` — also a row on **Options → Video Options**. **Metal is the default on macOS since Phase 8**: it renders the fork's whole picture at measured parity with GL, composites the ray tracing natively instead of through a cross-context bridge, and it is the only path with HDR (EDR) output. `gl` selects the OpenGL renderer, still fully maintained as the reference — and your choice **saves**, so picking `gl` sticks across launches. If Metal cannot start, the engine falls back to GL for that launch by itself. The few genuinely unported shapes sentinel **magenta** on purpose, loudly, rather than rendering something plausible. |
| `r_viewscale` | 1 | Renders the 3D scene at a fraction of the window and scales it back up — **the resolution knob**; 0.667 renders 44% of the pixels. Also the **Render Scale** row on **Options → Video Options**, which cycles 100% → 75% → 67% → 50%. The HUD, console and menus stay native either way. On its own the upscale is plain bilinear, i.e. soft. Values above 1 supersample (costly antialiasing). |
| `r_metalfx` | 0 | **HOW** the frame reaches the screen: **0** plain bilinear, **1** the MetalFX **spatial** scaler, **2** the MetalFX **temporal** scaler. Spatial needs `r_viewscale` below 1 (a 1:1 spatial upscale is a pointless copy); temporal is allowed at **any** scale including 1, because it is not only an upscaler — it accumulates jittered frames over time, which is what removes the fog and shadow crawl. Measured on the frozen bed at 0.667: bilinear keeps 62–70% of the native frame's edge energy, spatial 85–106%. Needs the Metal renderer; falls back to bilinear silently whenever the scaler cannot run (GL, `r_viewscale_fpsscaling`, `r_viewfbo 3`, a sub-frame or stereo view, MSAA, envmap, and for **2** also motion blur). Works with HDR. **Temporal forces a float scene buffer** (2026-08-19, the same way `r_edr` does): on an 8-bit buffer it accumulates but does not reconstruct, and a whole session was spent inside that configuration before the float buffer was found to be the limiter — so `r_viewfbo` need not be set for it. Also the **MetalFX Upscale** row on **Options → Video Options**, greyed only when the renderer is not Metal; it cycles Off → Spatial → Temporal (skipping Spatial at Render Scale 100%, where a spatial upscale would be a pointless copy). **Since 2026-08-20 the M5 Quality tiers own it**: Better, Best and Ultimate select **2**, Superfast, Fast and Good select **1**. **Superfast went BACK to spatial in the 2026-09-19 AA retier**, reverting its one-day move to temporal: that move was made because "spatial cannot reconstruct a silhouette", and `r_smaa` now does that work at native for a measured 0.296 ms, so the tier keeps its edges and its frames. Temporal -> spatial measured x1.210 at Best (2026-09-18), which makes this **the largest single lever left in the tier table** and the reason it is the Good -> Better rung. Good was temporal for one day and went back to spatial on 2026-08-21, when the soaked fullscreen bench measured temporal at 22-25% of the frame. |
| `r_fxaa_post` | 0 | **FXAA at NATIVE resolution, AFTER the MetalFX upscale** (2026-09-18, Seb: "It would be good to my naive mind if FXAA, or any AA *DID* touch it"). THE REASON IT EXISTS: `R_MetalFX_GetPostprocessTarget` sizes the postprocess target from `r_fb.rt_screen`, so FXAA — and gamma, the fringe and the saturation with it — runs at **render** resolution (1280×720 on Good/Better/Best, 720×405 on Superfast) and the scaler then MAGNIFIES the already-antialiased frame. FXAA therefore never sees the staircase the magnification creates, which is exactly the jagged silhouette he reported at a roof against sky and at a near object against a far one — measured on his own demo47 frames, **FXAA off is indistinguishable from FXAA on at those edges**. With this on the scaler writes a pooled NATIVE-resolution target and one extra fullscreen pass antialiases that, at the resolution the staircase actually exists at; the arch soffit's steps go. `SHADERMODE_FXAAPOST`, MSL only with a magenta sentinel on GL (MetalFX is Metal's, and the selection is a runtime predicate rather than a `#ifdef`). Needs `r_metalfx`; inert otherwise. `r_fxaa` still drives the render-res pass, so **`r_fxaa 0` with this on is the clean single-FXAA configuration**, and both on is a legitimate but slightly softer double. Costs one 1080p fullscreen pass. **Two things it needed, both of which failed SILENTLY first**: the TEMPORAL scaler's `outputTextureUsage` is 0x7 against the spatial's 0x5 and Phase 8-4 gave those bits to the screen texture ALONE, so a pooled target carried 0x5, MetalFX refused it and the frame fell back to a correct but un-antialiased path with nothing said — Metal render targets now carry both scalers' queried bits; and `PixelSize` was declared only under `USEFXAA` INSIDE the POSTPROCESS guard, so the new mode would not compile at all until it was declared for itself (smoke run Q's standing `absent "MSL compile failed"` is what caught that). Console only. **A TIER LEVER SINCE 2026-09-19, pinned 0 on every tier**: `r_smaa` supersedes it outright, and two edge filters stacked is a softer frame for no gain. `r_fxaa` became a lever the same day and is pinned 0 too — it is the RENDER-resolution pass, so at `r_viewscale 0.375` it only softens the scaler's input; the first tier click overwrites an archived 1, and `exec fxaa_on.cfg` reverts it. |
| `r_fxaa_post_span` | 8 | How far the post-upscale FXAA may blend **along** an edge, in native pixels — the number that decides which edges it can fix. The filter's farthest tap is half the span, so at 8 it reaches 4 px and resolves a steep staircase; a shallow one (a near-horizontal roofline against the sky) has steps longer than that and needs 16. Higher softens fine detail slightly. Only acts with `r_fxaa_post 1`. Archived; console-only. |
| `r_smaa` | 0 | **Analytic MLAA — SMAA's orthogonal core, computed in the shader with no lookup tables — at NATIVE resolution after the MetalFX upscale, in place of `r_fxaa_post`** (2026-09-18, `SMAA.md`). WHY A SECOND FILTER IN THE SAME SLOT: `dp_fxaa` clamps its blend direction to `maxspan` and takes its farthest tap at `dir*0.5`, so it reaches span/2 and corrects **at most half a pixel, only near a step's ENDS**. Seb's two reported edges differ in exactly that and nothing else — the A-frame leg is steep, steps every 3 px and is fixed; the lit studded band is shallow, steps every 6–18 px, and four pixels in six get nothing. It is the OPPOSITE of a contrast story: the jagged edge is the *higher*-contrast one. Measured against an analytic ground-truth edge (`test/aaedge.py`, which reproduces this file's own FXAA table to within 0.005): on a 1:6 slope the staircase is **0.340 with no AA, 0.230 at span 8, 0.124 at span 16 and 0.008 here**; raising the span past ~24 makes *both* slopes worse, because the taps land on unrelated content. So FXAA is exhausted structurally, not by tuning. This pass instead finds each step's WHOLE extent along the edge and blends every pixel in it by that pixel's own coverage — a ramp across the step rather than a nudge at its ends. Three modes (`SHADERMODE_SMAAEDGES` / `_WEIGHTS` / `_BLEND`), MSL only with magenta sentinels on GL, two pooled native-resolution intermediates. **Cost: +0.296 ms, +2.6% of the renderer’s command buffer** — one boot, frozen e1m3 spawn, 1920×1080, toggled every six seconds with the first window after each toggle dropped (the drift-immune form); the two arms do not overlap at all (ON 11.464–11.526 ms against OFF 11.170–11.264), so the figure needs no error bar. The search loop is all of it, so `r_smaa_search` roughly scales it. **It SUPERSEDES `r_fxaa_post`** (two edge filters stacked is a softer frame for no gain) and says so once; `r_fxaa`, the render-resolution pass, is independent. Needs the Metal renderer and `r_metalfx`. Console only until his eye has it. **AN M5 QUALITY TIER LEVER SINCE 2026-09-19** — 1 on every tier but Stock. It is one half of a PAIR with a raster-exact RT term (`rt_metal_scale 1`); neither half reaches a shallow silhouette alone (aaroof.dem: 0.362 untreated, 0.384 with this alone, 0.318 with the term alone, **0.229 with both**). It needs `r_metalfx >= 1` for a native target, which is why it silently did nothing in `nofx_aa.cfg` at `r_viewscale 1` — the spatial scaler is refused at scale 1, so there was no scaler and no target. |
| `r_smaa_threshold` | 0.08 | The luma step that counts as an edge, **absolute on the display-encoded frame this pass is fed**. Under `r_edr 1` that frame reaches ~1.756, so a highlight's edges cross an absolute threshold more readily than a midtone's — correct rather than a defect (a bright silhouette *is* a stronger edge), and stated because SMAA's paper assumes [0,1] and a reader would otherwise expect a normalisation that is deliberately not there. `r_smaa_adapt` is what keeps the detector off texture. Console only. |
| `r_smaa_search` | 16 | How far the pass walks along an edge line looking for the step's end, in native pixels each way. **This is the reach, and unlike FXAA's span it costs accuracy rather than correctness when it runs out**: a run longer than this contributes nothing from the capped side, so the pass leaves it alone rather than inventing a slope for it. The reported steps are 6–18 px, so 16 covers them; the search loop is the pass's whole cost. Clamped 4..32. Console only. |
| `r_smaa_adapt` | 2 | SMAA's own local-contrast adaptation: an edge survives only if its own luma step is at least 1/this of the biggest step in its neighbourhood, so the strong silhouette in a busy region wins and the texture around it does not also register. **This is what stops a morphological pass smearing detail**, and it is the reason the edge detector takes seven taps rather than three. Console only. |
| `r_smaa_debug` | 0 | 1 shows the EDGES (red a left boundary, green a top one), 2 the blend WEIGHTS as the last pass gathers them (red the horizontal correction, green the vertical). A pass that silently did not run renders a perfectly correct frame that is merely not antialiased — the invisible-failure class this tree keeps paying for — and these are what say which of the three passes is at fault. Console only. |

#### Temporal upscaling (`r_metalfx 2`) — the console-only knobs

Temporal upscaling was added to attack the rolling fog/shadow mesh rather than to save
pixels, and it is **default off pending by-eye QA**. What it does, measured on the frozen
e1m3 bed at `r_viewscale 0.667` with a float scene buffer: the fog's frame-to-frame
"twinkle" — the fraction of its fine detail that re-randomises every frame — drops from
**9.1% (spatial) to 2.0%**, and the grid amplitude halves, because the spatial scaler was
*sharpening* the mesh as well as the picture (its grid reads 129% of native's). The
twinkle fix is **scale-independent**: it is the same at 0.667 as at 1.0, so raising the
render scale buys detail, not less mesh. Cost is about **15% of frame rate** at equal
scale.

| Cvar | Default | What it does |
|---|---|---|
| `r_metalfx_viewmodel` | 1 | Give the view weapon its **own** motion vectors instead of the world's. Without it the weapon is told it moved by the whole camera rotation, which is the largest and most-watched vector error in the frame. The weapon is identified from the short depth range the engine draws it into, so this costs one compare and no extra geometry pass. Measured on a fast turn: the weapon's frame-to-frame shimmer falls **20%** and lands three times closer to the no-scaler reference. 0 reverts to camera-only. |
| `r_metalfx_jitter` | 1 | Sub-pixel jitter of the raster projection, as a fraction of a render pixel. 0 disables it — temporal still denoises without jitter, it just cannot *reconstruct*, so 0 is a legitimate A/B rather than a broken state. |
| `r_metalfx_jitterfix` | 0 | Pin the jitter to a **constant** `"x y"` offset instead of walking the sequence. This is the probe that settles the jitter's magnitude and sign against an unjittered frame; not a play setting. |
| `r_metalfx_signs` | 1 | Sign conventions as a bitmask (1 negates jitterOffsetX, 2 jitterOffsetY, 4 the motion vectors' x, 8 their y). **The default is measured, not read off Apple's header** — the scaler is told the jitter so it can undo it, so the correct sign cancels a constant jitter and a wrong one doubles it. x needs negating and y does not; the asymmetry is the GL-layout vertical flip. |
| `r_metalfx_debugview` | 0 | Show the motion-vector buffer instead of the scene: red/green are the x/y displacement about mid grey, and the **value is the full-scale displacement in pixels** (20 maps ±20 px across the range). History is reset every frame while it is on. This is the instrument that makes the orientation and sign questions answerable by looking. |
| `r_metalfx_entities` | 1 | Give moving **entities** — monsters, projectiles, doors, lifts — their own motion vectors instead of the world's (T2b). Rigid-body only: a walking monster's body gets the right vector, its swinging limbs get the body's. Measured honestly: what was reported as "rocket ghosting" turned out to be the rocket's **light** sweeping the floor, which no motion vector can address (see the note below); this fixes the object itself. Inside the fill's cost to +0.15 ms. 0 = the world fill plus the view weapon only. |
| `r_metalfx_reactive` | 0 | Feed the temporal scaler a **reactive mask** — per pixel, how much to distrust the history — from two sources: the dynamic-light footprint (how much any moving light — rocket glow, muzzle flash, explosion — reaches the world point seen there; this is the one ghost no motion vector can describe, a light moving across *still* geometry) and, under it, the **particles' own footprint** (`r_metalfx_reactive_particles`, below). The value is the light footprint's gain; 3 saturates it (3 and 8 read identically). **Measured against the scaler's own yardstick** (2026-08-19, the static-camera rocket bed, `test/ghost.py`): a mask of 1.0 is honoured — a white mask everywhere reads within 0.004 of MetalFX's reset-every-frame — and what lingers on the smoke with no mask is ~0.06 of the previous frame, which either source takes to ~0.015. Costs +0.2–0.5 ms. Default off, and **archived** since 2026-08-20 because it is an M5 Quality lever (Better, Best and Ultimate set it to 3; Superfast, Fast and Good run the spatial scaler, which cannot use it). |
| `r_metalfx_reactive_particles` | 3 | Under the master above: stamp every particle batch the scene drew — smoke, blood, sparks, bubbles, the explosion burst — into the mask, weighted by its visible strength × its texture's own shape × this gain, so a soft puff marks a soft disc rather than a square. Particles write no depth and carry no motion vector, so nothing else can tell the scaler they are there, and 78% of what moves behind a rocket is its smoke trail. The gain sweep 1/3/8/30 read identically on the rocket bed because overlapping puffs saturate; it only matters for a lone faint particle. **On a rocket the light footprint already covers the fresh smoke** (it lives inside the rocket's own light), so this source's own reach is the lightless particles — blood, gib trails, sparks, debris after a flash has died. Cost inside the motion pass's noise. 0 = the light footprint only. |
| `r_metalfx_reactive_trail` | 0 | Under `_particles`: also stamp the *previous* frame's particle footprint, re-projected through this frame's camera — the trailing edge. Measured: no effect on any class or frame of the rocket bed (overlapping puffs already cover each other's trailing edge), so it ships off and is kept as the measured off-switch. |
| `r_metalfx_reactive_debug` | 0 | 1 = show the reactive **mask** instead of the scene (white = distrust). Soft puff shapes plus the light's disc is right; squares, or anything behind a wall, is wrong. Look at it before reading any number. |
| `r_metalfx_reactive_force` | 0 | Probe: 1 = a white mask everywhere, −1 = black everywhere, 2 = leave the mask alone but set the scaler's own reset flag every frame. The ends of what the mask can do, for measuring the scaler end to end; not a play setting. |

**Known limitation, stated rather than hidden — and re-measured 2026-08-19:** the world, the
view weapon and every moving entity carry their own motion vectors; a dynamic **light**
moving across still geometry and the **particles** that write no depth do not, and the
reactive mask (`r_metalfx_reactive` + `_particles`, above) is the scaler's own mechanism for
both. What the rocket bed then said, with the scaler's reset-every-frame as the yardstick:
the smoke trail's genuine history lingering is small (~0.06 of the previous frame), the
mask removes most of it, and the ~0.26 the metric still reads on smoke is the temporal
scaler rendering small bright puffs ~7% softer than a bilinear upscale — present with *no*
history at all, so not a ghost but a look (slightly softer smoke), which no mask can or
should change. What remains genuinely unmasked is the content the stamp does not see: the
explosion shell (`cl_particles_explosions_shell`), sprites, the thunderbolt and coronas.

**The compounding trap:** the RT trace already has its own resolution knob, and they
multiply. Effective trace resolution = window × `r_viewscale` × `rt_metal_scale`, so at
`r_viewscale 0.667` with the default `rt_metal_scale 0.5` the shadows trace at a third of
the window's linear resolution. That is documented rather than compensated — raise
`rt_metal_scale` if RT edges read too soft under the scaler.

**To check what you are actually rendering at**, look at the `RT_Metal:` line in Xcode's
console (Cmd+Shift+Y) — it prints the trace size and the megapixel count every 120 frames.

### The seven tiers re-measured 2026-09-17 (night) — the BEAUTY round priced

The eleven BEAUTY defaults went ON that evening and the table had never been measured with
any of them. Method: reboot, third screen off, a soak run until the curve was **flat**
(demo26 109.3 → 81.2 fps over fourteen runs, flat to 1.0% over the last four), then paired
rounds (`PERF_ROTATE=2`) with Best on the candidate's own bed immediately before it,
exclusive fullscreen 1920×1080, `-nosound`, one engine at a time. Beds: **demo46** (e1m3,
Seb's own, recorded that afternoon) and **demo26** (e2m4). Four passes, reference-outlier
pairs dropped, **zero flagged geometry on every row**.

| Tier | demo46 (e1m3) | demo26 (e2m4) | ×Best |
|---|---|---|---|
| **Stock** (1996) | **376 / 326** | **362 / 250** | **3.30** |
| Superfast | 254 / 150 | 303 / 191 | 2.49 |
| Fast | 185 / 107 | 228 / 163 | 1.84 |
| Good | 159 / 84 | 161 / 107 | 1.43 |
| Better | 125 / 47 | 136 / 98 | 1.17 |
| **Best** | **111 / 46** | **112 / 76** | 1.00 |
| Ultimate | 81 / 40 | 79 / 61 | 0.72 |

Average / 1-second minimum. The ladder is monotone and evenly graded bar the documented
compression: Ultimate→Best ×1.39, **Best→Better ×1.17**, Better→Good ×1.22, Good→Fast
×1.29, Fast→Superfast ×1.35, Superfast→Stock ×1.33. Ultimate read ×0.725 and ×0.733 on the
two beds — three decimals apart across four pairs and two maps.

**These are a DIFFERENT BED SET from the 2026-09-05/06 tables below and do not supersede
them** — demo46 is new and demo26 is the only bed in common. This file's own rule is that
two suites on two bed sets are not comparable; the one bed that is shared is the anchor,
and it reads **112 tonight against 119–123 on 2026-09-06**, with A5 (+8%), the water
surface (−5 to −7% on this bed) and the BEAUTY round (−3.1%) in between predicting ~118–119.
The ~5% gap is unattributed and would need a `tier_best_0906` time-machine arm inside a
block to split.

**What the BEAUTY round costs: +3.1% of the frame** (95% CI +1.5 .. +4.7%), from ten pairs
of `beauty_ref` against `beauty_off` — the whole round on against the eleven reverted —
run back to back in one script so no idle gap could open between them: **+2.8% on demo46,
+3.4% on demo26**. `beauty_ref` is cvar-identical to `tier_best`, so that ratio is Best
with the round off against Best, measured in the same sitting rather than across suites.

Priced per lever on the **in-boot toggle** (one boot, frozen e1m3 spawn, the cvar flipped
every four seconds sixteen times, `METAL_FRAMEMS=1` grouped by the live arm; read both as
plain per-arm medians and as a centred difference, which agree to within 0.03 ms on every
row), because the paired demo block's own per-run spread is ~3% and cannot resolve a 1%
lever:

| lever | frame delta | ~% of an 11.3 ms frozen frame |
|---|---|---|
| all eleven together | +0.109 .. +0.173 ms | **+1.0 .. +1.6%** — the always-paid share |
| `rt_metal_contact` | +0.112 .. +0.127 ms | ~1.0% (trace stage +0.085 ms, +2.5%) |
| `r_caustics` | +0.072 .. +0.095 ms | ~0.8% |
| `rt_metal_gi_ao` | +0.003 .. +0.028 ms | nil |
| `m5_torch_embers` | −0.061 .. −0.006 ms | nil |
| `rt_metal_fog_liquidlight` | −0.005 .. +0.001 ms | nil |

So about half the 3.1% is paid on every frame and the other half is combat-dependent — the
particle levers (`cl_particles_soft`, `_refract`, `_texsize`, `_scorchglow`,
`_blood_droplet`), which a frozen camera cannot exercise and which are known only as that
residual. **Every lever is under 2%, so none became a tier lever and no cell of
`m5_quality_levers[]` moved.** The round is also free on the bottom tier by construction:
`m5_stock` gates the liquid-fade static parm, so the caustics block is not compiled into
the Stock shader at all, and Stock's own `rt_metal 0` / `r_volumetric 0` remove contact, AO
and liquid-light while embers, soft particles and refraction are suppressed by name.

Two predictions the bench refuted. **`r_caustics` was expected to be the round's tier
lever and is 0.8%** — the cost model behind that expectation ("two ridged noise fetches per
pixel on every world opaque batch") is wrong: the shader does **one** 3D *field* fetch per
opaque fragment and reaches the two noise fetches only inside a second branch, on surfaces
actually below a waterline. And **`rt_metal_contact` reads ~1.0%, half the ~2% its own B3
record estimated** — its "not a tier lever" verdict confirmed rather than overturned.

Against the targets: Best 111/112 misses the ~120 of PERFPLAN and clears the 100 used for
the heavier September bed set; Good 159/161 misses ~180; Better 125/136 meets ~130 on e2m4
and misses on e1m3; Fast's 240 *held minimum* is missed by a wide margin. **None of that is
the round's doing** — back its 3.1% out and Best is ~115, Good ~165, Better ~129, still
short. Which target applies to which bed set is Seb's call and is flagged in BEAUTYBENCH.md.

### Best re-tiered 2026-09-18 — TARGET-110, and the trace cut is the only fog-preserving lever that pays

Seb: *"110 fps target while keeping the sharp fog."* That phrase pins the fog buffer
(`rt_metal_fog_scale` 0.375), the 24 march steps, the unshadowed fill at 0.1, the 3×3
filter and the light pick with its hybrid — so the four cheapest candidates on the table
were ruled out before a run, and only levers *outside* the fog could be spent.

Four beds, ten-minute soak to flat, `PERF_ROTATE=2` paired so the reference runs on the
candidate's own bed immediately before it; **32 of 32 rows exclusive 1920×1080 with zero
flagged geometry**, reference spread 1.5 / 2.0 / 4.0 / 4.4% per bed, no pair contaminated
at the 5% bar.

| arm | demo46 | e2m4 | e3m6 | e1m1 | median |
|---|---|---|---|---|---|
| **Best as it shipped** | **108.7** | **109.2** | **97.3** | **107.6** | — |
| `rt_metal_scale` 0.3125 — **WIRED** | ×1.039 → 112.3 | ×1.033 → 112.0 | ×1.057 → 100.2 | ×1.073 → 110.5 | **×1.048** |
| `rt_metal_gi_rate` 2 | ×1.016 | ×1.004 | ×1.019 | ×1.003 | ×1.010 |
| trace + rate 2 + contact/caustics off | ×1.033 | ×1.035 | ×1.048 | ×1.026 | ×1.034 |
| `r_metalfx` 2 → 1 (spatial) | ×1.212 | ×1.207 | ×1.187 | ×1.220 | **×1.210** |

**Best as it shipped was a 97–109 tier and missed 110 on all four beds.** The trace cut
takes it to **110–112 on three beds of four** and leaves e3m6 at 100; Seb took that
("Go option A"), accepting that e3m6-class maps sit at ~100 rather than spend the temporal
scaler, which is the only lever big enough to close the fourth bed (×1.210 — and note that
is 20.7%, not the 12.5% the 09-04 ladder recorded at a different geometry).

**Two negative results worth as much as the wiring.** Half-rate bounce is at the
instrument floor (×1.003 and ×1.004 on two of four beds), and **the combined package reads
WORSE than the trace cut alone on three beds of four** (×1.034 against ×1.048) — strictly
less work reading slower, which past the noise floor says the skipped-tile carry-forward's
read-and-write is not free. So the BEAUTY round must not be raided for frames: contact
shadows, caustics and the bounce rate are not where the frame is.

**A ladder consequence, stated rather than hidden: Good now traces LARGER than Best**
(0.375 against 0.3125, both at `r_viewscale` 0.667 — 480 px wide against 400). That is the
inversion the 2026-09-06 session fixed by pulling Good down to 0.375 in the first place.
Best is still clearly the better tier overall (bounce lighting, the temporal scaler, 24 fog
steps, the larger fog buffer), but the single cell is inverted and stepping Good to 0.3125
is the one-line fix whenever its look is next put to Seb's eye.

`tier_best_ts375` is the revert control. The soak's own lesson is in CLAUDE.md: a flatness
test cannot tell the COLD plateau from the hot one, and the first attempt at this block read
156.9/157.3/158.2/156.8 — dead flat — against a hot Best of ~112 on that bed.

### Good to Ultimate re-measured 2026-09-06 (evening) — the fog verdict priced

The fog came back that evening (the light pick on the two tiers below Ultimate, 24 march steps on the
top three, Better's history 0.6, then Seb's "sharp1": the unshadowed fill 0.25 → 0.1 and
the 3×3 filter everywhere), so every tier that carries the fog kernel's shadow structure
were re-measured in one soaked block: ten-minute soak, then `PERF_ROTATE=2` pairs with
Best on the candidate's own bed immediately before it, exclusive 1920×1080, three of the
September beds (demo24 e1m1, demo26 e2m4, demo28 e3m6). Every row fullscreen 1080p; the
Best reference's own spread **2.7 / 2.9 / 5.0%** per bed. The two afternoon rows ran as
revert controls beside them.

| Tier | e1m1 | e2m4 | e3m6 | ×Best (median, range) |
|---|---|---|---|---|
| Good | 175 / 101 | 208 / 106 | 158 / 107 | **1.60** (1.55–1.70) |
| Better | 125 / 75 | 141 / 69 | 115 / 58 | **1.15** (1.14–1.16) |
| **Best** | **105–110 / 64–75** | **119–123 / 68–74** | **99–102 / 46–67** | 1.00 |
| Ultimate | 74 / 56 | 82 / 58 | 71 / 49 | **0.68** (0.67–0.70) |
| Best as it stood that afternoon (pick off, 16 steps, fill 0.25, 5×5) | ×1.14 | ×1.14 | ×1.14 | the fog verdict costs Best **12.5%** |
| Better as it stood that afternoon | ×1.31 | ×1.32 | ×1.24 | costs Better **24%** |

Against the targets: Best meets its 100 average on 3/3 beds (e3m6 at 99–102 is on the
line); Better meets 120 on 2/3 (e3m6 115); Ultimate's avg-80 target is met on 1/3 and its
min-50 on 2/3, as before the evening. The ladder reads Ultimate→Best ×1.47, Best→Better
×1.15, Better→Good ×1.40 — monotone, with the Best→Better rung compressed again because
the fog verdict is worth more to Better (whose smaller buffers made the pick a larger
share) than to Best. Superfast, Fast and Stock did not change and keep their 2026-09-05
figures below.

### The six-tier table, measured 2026-09-05 — ONE afternoon, one machine state, five fresh maps

This is the first time every one of the six tiers has been measured in a single soaked
block on a single set of beds, so it supersedes everything below it: the tables further
down mix an August suite on the old four beds with a 4 September patch, and this file's
own rule is that two suites taken on two afternoons are not comparable.

Method: a **10-minute thermal soak** first, then paired rounds (`PERF_ROTATE=2`) in which
Best runs on the candidate's own bed *immediately before it*, exclusive fullscreen
1920×1080, on Seb's own five September demos — demo24–28 = e1m1, e3m2, e2m4, e2m1, e3m6.
Zero flagged geometry, zero lost runs; the Best reference's own spread was **1.1–3.1%**
per bed.

| Tier | Average | 1-second minimum | ×Best |
|---|---|---|---|
| **Stock** (1996) | **324–335** | **216–290** | **2.79** |
| Superfast | 256–339 | 142–245 | 2.50 |

**STOCK, added 2026-09-05.** 1996 GLQuake 1.09 as nearly as this engine can manage it, on
the art already installed. `m5_stock` is a **read-side master**: every fork feature is
suppressed where it is *asked for* rather than by writing the player's cvars, so nothing of
theirs is destroyed and `m5_stock 0` restores their picture bit for bit. Suppressed: bloom,
FXAA, colour fringe, the saturation filter, the HDR shoulder, EDR, the offscreen scene
path, red glow, the lava boil, the heat shimmer, the water and teleporter swirl, the liquid
fog fade, the M5 thunderbolt, ambient dust, lit particles and the handlamp — plus, at map
load, the **replacement world textures**, so the original id art loads instead. The tier
adds `rt_metal 0`, `r_volumetric 0`, `cl_particles_quake 1` (GLQuake's disc particles) and
`r_lerpmodels 0` (the 10 fps animation snap).

**`m5_cheap` (not archived, default 0) is the stock/beautiful SWITCH** (2026-09-22): 1
snapshots every lever in the tier table and applies the Stock row; 0 puts the snapshot back
exactly. `bind F6 "toggle m5_cheap"` and flip it in play (each flip pays the same one-off
shader rebuild hitch a Stock tier click pays). It is a session overlay: `Host_SaveConfig`
brackets the config write so the player's own values are archived even when quitting with
it on, and a fresh boot always starts on the player's own look. Anything changed at the
console while it is on is discarded when it comes off; a tier click on the M5 Quality row
takes the overlay off first. The replacement-art skip below is a map-load decision, so a
first press mid-map keeps that map's art until the next load.

**The original art under stock, fixed 2026-09-22.** The first cut of the texture skip gated
only the second of the two external-image arms in `Mod_Q1BSP_LoadTextures` -- and the
q3-shader path above it succeeds on a bare replacement image with no shader, so QRP loaded
exactly as before (measured on e1m3: 131 MB of textures under stock on the old binary, 21.5 MB
now). Worse, where the gate did fire it skipped the INTERNAL load too, so a texture with
no replacement image -- the shell, battery and rocket boxes' `shot0sid`, `shot0top`,
`batt0top`, `rockettop`, and the e1m3 sky -- was loaded by nobody and drew the "NO TEXTURE
FOUND" checker. Seb: "sometimes the textures don't load in stock mode for ammo boxes and
health." Both arms are gated now (a REAL q3 shader, Arcane Dimensions' kind, is still
honoured) and the internal load always runs.

**Two things it deliberately does and does not do.** It does NOT change the texture
filter: GLQuake 1.09 was *bilinear* — chunky nearest-neighbour is software Quake — so the
default is already period. And it DOES override the whole tone chain (`r_brightness`,
`v_gamma`, `v_contrast`, `r_hdr_scenebrightness`) to their defaults, which is the one place
stock overrides something a player might call calibration. It has to: a curve tuned against
the RT wall-lighting term under-exposes a baked lightmap by roughly eight times, and the
first Stock still came back **black** on Seb's config (mean luma 1.35, median 0.0, against
11.16 at the default curve). Note `rt_metal 0` **alone** does that, with `m5_stock` not
involved — the collision is pre-existing. Overriding brightness and scene brightness but
not gamma was measured and was not enough (2.07); the chain is one calibration and cannot
be taken in halves.

**Measured** (same soaked block, paired against Best on five beds): **324–335 average,
216–290 1-second minimum** — the flattest row in the table by a wide margin, a 3% spread
across five maps, because it is no longer GPU-bound on any of them. Its minimums beat
Superfast's everywhere except e2m4 (216 against 135 on e1m1, 261 against 158 on e3m6) even
where Superfast's average is higher, which is what being CPU-bound and uniform looks like.
| Fast | 190–261 | 105–180 | 1.83 |
| Good | 141–183 | 100–139 | 1.36 |
| Better | 120–157 | 66–93 | 1.20 |
| **Best** | **101–128** | **69–80** | **1.00** |
| Ultimate | 74–90 | 47–64 | 0.72 |

**Better moved on 5 September too**, and it is the ladder change rather than a
performance one: once Best took 16 fog steps the Best→Better rung measured **×1.04**
against ×1.31–1.38 for every other step in the table — not two tiers. Better now has its
own **400×225 fog buffer** (`rt_metal_fog_scale` 0.3125, against Best's 480×270) and a
**320×180 RT term** (`rt_metal_scale` 0.25). Measured paired on the five beds: **×1.201 of
Best, range 1.157–1.260**, i.e. **120–157 avg / 66–93 minimum**, meeting its 120 target on
5/5 beds against 2/5. The fog-buffer half alone was ×1.146 — *more* than turning the fog
light pick off buys there (×1.119) — and it keeps the per-light fog shadowing.
`tier_better_prev0905` reverts both cells, `tier_better_ts375` the trace alone,
`tier_better_fogbuf375` the buffer alone.

**THE STRUCTURAL POINT, and it is the reason the top of the ladder was flat:** four of the
six tiers were allocating the **identical 480×270 fog buffer** while the fog kernel is
~36% of the frame — the most expensive resource in the engine was constant across the half
of the table that had no steps in it. The remaining anomaly is **Good**, which now
allocates more of both buffers than Better (480×270 fog, 640×360 trace) and **more trace
than Best**; it still measures faster because its rate comes from the spatial scaler and
having no bounce lighting, but it is carrying buffer quality it gets no credit for. That
is the next cell.

**Best and Ultimate moved on 5 September** — Best and Ultimate both dropped to 16 fog
march steps and Ultimate's RT term went from 960×540 to 720×405. Measured paired, and both
reproduced by their revert controls: `tier_best_steps24` reads ×0.874 (range 0.871–0.892
over all five beds), i.e. the new Best is **+14.4%**, against +15.0% measured
independently in the candidate block; Ultimate is **+27.8%** on its old self against a
+28.4% prediction. See the 2026-09-05 record in CLAUDE.md.

**The thermal fact, reproduced twice in one day.** The soak curve on demo24 at Seb's own
config ran **148.2 → 110.9 fps over 14 identical runs** in the morning and **148.5 →
111.4** in the afternoon — the same arm, the same bytes, the same bed, landing on the same
hot figure to 0.5%. So the machine's "fast state" is its COLD state, it is worth **+34%**,
and every figure in this table is a hot-state figure by construction. Recovery is much
faster than decay: three minutes of idle is enough to read 30% high again, which is why
the first pair of a block is routinely poisoned and why the paired form is the only shape
that survives.

### The four-bed table, measured 2026-08-30 — SUPERSEDED by the block above

(Soaked, exclusive fullscreen, quiet machine, both times. Best re-ran on 1 September
because it took a smaller trace buffer that day; the two suites are cross-anchored below.)

Every row below is the shipped table benched at exclusive 1920×1080, two interleaved
rounds with the arm order alternated and a baseline at both ends of every round; the
within-round drift came out under 3% throughout, so these are quotable absolutes.

| Tier | Average | 1-second minimum |
|---|---|---|
| Superfast | 281–329 | 139–300 |
| Fast | 226–282 | 115–200 |
| Good | 172–196 | 107–130 |
| Better | 129–140 | 91–94 |
| Best | 123–134 | 79–89 |
| Ultimate | 71–78 | 49–61 |

**4 September, evening: Best's fog buffer is back at 0.375; the rest of the ladder
verdict stands.** Seb's ladder had put a 640×360 fog buffer (`rt_metal_fog_scale` 0.5),
a cast every second step (`rt_metal_fog_stride` 2), `rt_metal_fog_history` 0.6 and
`rt_metal_lightsample_hybrid` 2 into Best (and the stride and hybrid into Ultimate). On
five fresh demos (e1m1, e3m2, e2m4, e2m1, e3m6), exclusive fullscreen, every witness
under 5%: the whole verdict cost Best **33–41%** of its frame, and the buffer alone was
**51–61% of that** — so the buffer went back and Best reads **92–111 fps / 68–88
minimum** on those beds against 68–84 / 51–65 with it. `tier_best_fogbuf05` is the revert
control. **The feature-cost ladder** (the shipped Best against fourteen arms each one cvar
away, `PERF_ROTATE=2`, blue-noise control at +2.7% = the floor): turning OFF the whole ray
tracer +114%, the whole murk +83%, the in-kernel fog integral +56% (ceilings); the
stochastic fog light pick (`rt_metal_lightsample 0`) **+22%**; the temporal upscaler
+12.5%; the stride back at 3 +10–15%; the fog 5×5 filter +5%; bounce lighting +2.7% (at
the floor); the term upsample, dust, lit particles and blue noise nil. Everything
expensive is inside the fog kernel. Ultimate still carries the stride and hybrid and
reads 57–58; its own pass is owed. The old four-bed table above is from other maps and
is not directly comparable with these figures.

**Best's row is MEASURED as of 2026-09-01 (late).** The projection it replaces read
119–131; the measurement came out 123–134, so the published band had been slightly
conservative. Bench: 10-minute soak, exclusive 1920×1080, four beds, two interleaved
rounds, alternating arm order, tail-baseline witness — **every row landed `fullscreen
1920x1080` with zero flagged geometry**, and the drift witness read −0.8 / +2.2 / +1.4 /
−0.8%, mixed in sign and well under the smallest arm effect (6.9%). Per bed, average /
1-second minimum: **e1m3 134/86, e3m1 123/79, e1m1 124/89, start 123/79** — so **Best
clears its ~120 average target on all four beds**, against one of four before the step.

Two same-arm cross-suite reproductions make that row comparable with the five above it,
which were taken on a different afternoon: `tier_best_0821` appears in both suites and
agrees to +4.4 / −3.6 / +0.2 / −0.4%, and tonight's `tier_best_ts5` (Best's levers at the
OLD trace 0.5) reads 124.6 / 115.2 / 113.8 / 113.1 against the 08-30 suite's `tier_best`
at the same 0.5 — 125.1 / 117.3 / 114.0 / 114.9, **within 1.8%**. The machine has not
moved between the two.

**The trace step itself, single-variable** (`rt_metal_scale` 0.5 → 0.375, nothing else
differing): **+7.2 / +6.9 / +9.1 / +8.8%**. **GI's cost at the new trace resolution**
(`rt_metal_gi` alone differing): **3.1 / 5.0 / 3.3 / 4.5%**, lower than the 6.5–7.2%
measured at 0.5 — a smaller trace buffer is fewer bounce rays.

**AND THE COST OF THE STEP IS NOW VISIBLE IN THE TABLE: Best 123–134 against Better's
129–140 is a ladder of roughly 5%**, where the two tiers were 13% apart before Best took
Better's trace resolution. They now differ only in `rt_metal_fog_steps` (24 vs 16) and
`rt_metal_fog_history` (0.7 vs 0.8), plus a pair of shaft settings that are inert while
`rt_metal_shafts` is 0. Stepping Better to `rt_metal_scale` 0.25 (measured +5.7–6.0%)
restores the separation and is the queued follow-up, pending Seb's eye. **Read the two
rows as near-neighbours until that lands** — the prose elsewhere describing Better as
buying back a real step over Best is written against the pre-01-September table.

**Owed, and not derivable from this block: the drift decomposition.** `tier_best_0821`
is a trace-0.5, GI-free, fully-reverted arm, and its like-for-like partner would be the
current engine at 0.5 with GI off. That arm exists — `drift_base` in `levers.tsv` — but
it is phase D and this block ran phase T, so any drift number computed from what is here
would confound GI with the accumulated engine cost. Run phase D alongside next time.

**Re-measured 2026-08-31** (fullscreen 1920×1080, 10-minute soak, four beds, two
interleaved rounds, alternating arm order, tail-baseline witness; **all 84 rows landed
`fullscreen 1920x1080` with zero flagged geometry**, and the drift witness came out
−3.8 / −1.4 / **+2.0** / −3.1% — mixed in sign, so no ramp). Against the targets agreed
2026-08-12: **Fast's 240 held minimum misses on all four beds** (115–200); Good's ~180
average clears on two of four; **Better's ~144 misses on all four** (129–140); Best's
~120 average clears only on e1m3 — **but Best's 1-second minimums are 80–87 everywhere,
comfortably inside the 60–90 Seb said he would settle for.** Superfast meets its 240
average on 4/4 and its 200 minimum on 2/4, e3m1 being the structural floor as before.

**The shortfall is mostly a feature, not decay.** `tier_best_0821` (Best's levers with
every post-21-August per-frame change reverted) runs 8.4–10.5% ahead of Best — but it
also carries `rt_metal_gi 0`, and splitting it with `tier_best_nogi` on three beds gives
**bounce lighting 6.5–7.2%** and **everything else 1.8–3.1%**. So GI, which Seb approved
on 2026-08-29, is roughly three quarters of the gap; the accumulated cost is the small
remainder, matching the 08-29 night session's own ~2–3% estimate. (demo14 was cut short
when Seb needed the display; demo5's own drift witness on that shorter block read −7.3%,
so its +2.1% "everything else" is not separable there — demo11 and demo12, whose
witnesses were +0.1% and −0.9%, carry that number.)

**Two tiers are new** (Seb, 2026-08-30: *"a superfast, and ultimate tier at either end…
there is still AA lacking in best in places"*).

**Ultimate is the antialiasing tier.** It is the only one that renders at native
1920×1080, which turns `r_metalfx 2` from an upscaler into pure antialiasing — the
temporal scaler is legal at any scale including 1, spatial is not, and MSAA would make
MetalFX fall back to bilinear silently, so this is the only route to it. It also traces
the RT term at 960×540 rather than 640×360: under wall lighting that term IS the scene's
lighting, and at Best it is magnified three times onto the screen, which the term upsample
fixes across silhouettes but explicitly cannot fix inside them. `rt_metal_fog_scale 0.25`
here is a **pin, not a cut** — at viewscale 1 it is 480×270, identical in absolute pixels
to Best's 0.375 × 0.667. Measured cost of the sharper term: 12–14% (Ultimate reads 73–80
against 83–91 with the term at 720×405).

**Superfast** spends the ray-tracing buffers first — a quarter of Fast's trace pixels,
57% of its fog pixels — drops `rt_metal_lightsample` (worth a measured 0.300 ms of fog
stage), and renders at 37%. That last one is not cosmetic: at Fast's 50% it came out only
7–12% above Fast, which is not a tier; 0.375 measured +4 to +8.5% on the average and
**+6 to +20% on the 1-second minimum**, putting the step over Fast at 18–20%.

**On Superfast's 240/200 target** (Seb, 2026-08-30): the **240 average is met on all four
beds**. The **200 minimum is met on two** — e1m3 213, start 320 — and on e3m1 it is
**structurally out of reach**: the floor probe ran Superfast with the ray tracer, the fog,
bloom, FXAA, the heat shimmer and the red glow *all* off, and the 1-second minimum still
only reached **176**. That floor is the engine and the demo's own heavy moments; no tier
lever touches it. On e1m1 it is reachable in principle (the bare arm hits 299) but
Superfast sits at 178 and would need a further 12%.

**Better's trace step, provisional the night before, is confirmed**: +7.5 to +8.1% on
every bed against the pre-wiring control, and the ladder is now evenly graded — +18-20%,
+34-58%, +34-41%, +15-17%, +50-56% from Superfast up to Ultimate.

**The 21-to-29 August drift is measured and mostly was not the engine.** With both arms
bounce-lighting-free on the same afternoon, reverting every per-frame change between those
dates recovers **1.7 to 4.5%** (the cross-block control agrees within 0.7–2.0%), against a
cross-suite gap of 2–8%. Bounce lighting itself costs Best 4.4–7.0%.

**QA PASSED 2026-08-30**, both new tiers, on these measured figures: *"looks good. I like
the new tiers."* The values above are the shipped ones. Better's trace step, wired the
night before, is confirmed by the same bench.

### On a 240 Hz OLED

A 240 Hz refresh is 4.2 ms. **These figures were stale and are corrected here**: they
described the 2026-08-13 tiers, before the temporal scaler, bounce lighting and the fog
light pick landed. On the 2026-08-29 soaked fullscreen bench Best runs **112–127** and Fast
**235–279** with a 1-second floor of 123–183, so Fast does not saturate the panel and its
floor does not hold 240. Tearing versus latency is still a live question at Best's rate,
which is roughly half the refresh.

- **`vid_vsync 1` is the recommendation.** At ~120 fps it syncs to every second refresh and
  gives a rock-steady 120, tear-free, for one refresh of added latency — 4.2 ms, which is
  small next to the 8 ms frame itself. A locked 120 generally reads smoother than a
  free-running 100–130.
- **`vid_vsync -1` (adaptive) is the fallback** if heavy scenes bother you: when a frame
  misses, sync drops out rather than halving you to 80 fps, at the cost of a tear on exactly
  those frames.
- **`vid_vsync 0`** is the classic uncapped choice. At 240 Hz a tear line moves off screen
  quickly and is far less objectionable than at 60, so this is a perfectly reasonable
  choice too — but since the 8-6 QA the default is 1.
- `cl_maxfps` is worth setting only to cap heat and fan noise; it does nothing for smoothness
  here because nothing is running away with the frame rate.

Input feel is the one thing measurement cannot settle — try `vid_vsync 1` for an evening and
see whether the aim feels heavier.

---


## Explosions and teleports — `m5/effectinfo.txt`

Quake explosions have never had smoke. The stock effect is a sprite, 512 sparks, a decal, a
stain and a light — and nothing that lingers. `m5/effectinfo.txt` is a **data file**, not
code: DarkPlaces loads it off the search path and it can be re-read live with
`cl_particles_reloadeffects`, so it is tunable in game with no rebuild.

| effect | today | with the file |
|---|---|---|
| `TE_EXPLOSION` | 1536 sparks, no smoke | a white blink, the bang and an expanding SHOCKWAVE ring (cell 36) with a 0.12 s light of their own; ten-puff fireball; hot smoke (a lit tan-brown) from 250–350 ms and cool smoke (mid grey) from 400–550 ms AFTER the flash (the `delay` keyword, so the fireball is seen first — Seb's "try 300 ms"; the two `delay` lines in `m5/effectinfo.txt` are the handle, `cl_particles_reloadeffects` applies them), both rolling; bouncing embers, sparks, decal, the steady light — **182 particles** plus a decal (2026-09-02 second cut on Seb's eye) |
| `TE_TAREXPLOSION` | grey dots | the same ten layers in violet, so a tarbaby matches a rocket |
| `TE_TELEPORT` | **3168 grey dots** | flash, 78 streaks, 42 motes, smoke — **145 particles** |

The teleport figure is the one to notice: it fires on every teleport, every respawn, every
teleporter touch **and three times per horde spawn**, so a 22× cut there is a performance
change as much as a look one.

**It is a partial file and that is deliberate** — the engine falls through to its hardcoded
defaults for any effect not named, so everything else is untouched. But the fallback is
**all-or-nothing per effect name**: naming `TE_EXPLOSION` deletes its decal, light, stain and
sparks in one go, so each is re-supplied in the file. The one loss is the **lightmap stain**,
which effectinfo cannot express; it is invisible under `rt_metal_walllight` (which forces
`r_fullbright` and discards the baked lightmap) and the decal supersedes it.

To turn the whole thing off, rename or delete `m5/effectinfo.txt`. There is no cvar — it is
data, and the engine either finds it or does not.

**Authoring notes**, each of which is a silent failure if broken: a wrong-argument-count line
**aborts parsing of the rest of the file**; `type` must be the first line of a block because
it overwrites `blend`; `type smoke` is **additive** unless `blend alpha` follows it; texture
cells 33 and 41–59 are opaque **white**, not empty (34–40 are the M5 cells: casing, dust, ring, flash, streak, ember, droplet — the explosion's, the ball's and the axe's spark layers use the streak (38) and the ember (39) since 2026-09-16; `tex 63 63` puts the blob back on any layer), and indices ≥64 wrap; and a `type decal` layer
with no `originjitter` spawns nothing at all.

## Enhanced thunderbolt — Options → Lightning Gun

The stock DarkPlaces lightning gun beam is one straight scrolling quad. `r_lightningbeam_m5`
replaces the path between its two endpoints with a jagged, branching discharge that re-rolls
its shape on a fixed cadence, lights the room, and burns brighter the moment it connects with
something. The endpoints do not move: the bolt still starts at your muzzle and lands exactly
where the stock beam landed. **Set it to 0 and you get the stock beam back, exactly.**

**Options → Lightning Gun** is its own page (10 rows since 2026-08-09; split out of
Customize Effects on 2026-08-03 with 25). The rows: the master, Wildness, Branches, Core
Hotness, Core Volume, World Light, Fog Scatter, and the three Beam Colour sliders — the
knobs you set by eye, every one proven to move the frozen-bolt test frame. Everything else
in the family is console-only, including the stock beam toggles (`cl_beams_polygons`,
`cl_beams_lightatend`, `cl_beams_instantaimhack`, `cl_beams_quakepositionhack`) and the
shared thickness/scroll/repeat/QMB rows.

The existing `r_lightningbeam_*` knobs still apply — thickness is the base width, scroll and
repeat distance drive the texture, and the colour sliders drive the bolt, the light it casts
and the fog it lights. The colour rows grey out when *neither* beam system that reads them
is active — the stock quad beam ignores all of them. Fog Scatter greys without `rt_metal`;
World Light greys unless the sidecar *or* the rtdlight path can carry it to a pixel.

| Cvar | Default | Range | What it does |
|---|---|---|---|
| `r_lightningbeam_m5` | 1 | 0/1 | Master. 0 = stock DarkPlaces beam. |
| `..._rate` | 15 | 2–30 | How many times a second the shape re-rolls. Framerate-independent — the bolt crackles at the same speed at 30 fps and 200. |
| `..._jitter` | 0.05 | 0–0.25 | How far the bolt wanders off the straight line, as a fraction of its length. The "wildness" row. |
| `..._axial` | 0.18 | 0–0.5 | How much the segment LENGTHS vary along the bolt; 0 spaces the kinks evenly, which reads mechanical. Console-only. |
| `..._branches` | 2 | 0–4 | Forks that split off and die out in mid-air. |
| `..._filaments` | 2 | 0–3 | Thinner threads that leave the main channel and rejoin it. |
| `..._whiteness` | 0.75 | 0–1 | How far the hot core is lifted toward white from your beam colour. |
| `..._light` | 1 | 0–3 | How brightly the bolt lights walls, floors and monsters. 0 = off. |
| `..._impact` | 1.6 | 0–4 | The flash where the bolt lands, relative to the channel. |
| `..._fog` | 1 | 0–4 | How strongly the bolt lights the volumetric fog — independently of the light it casts on surfaces. |
| `..._hitboost` | 2.2 | 1–3 | How much brighter it burns while it is actually damaging something. |
| `..._hold` | 8 | 0–30 | How many re-rolls it holds one overall pose before jumping to another, like a plasma globe. 8 at the default rate is about half a second. 0 re-rolls everything every time. |
| `..._frizzle` | 1 | 0/1 | Frays every free tip — the channel's landing point and each fork — into finer, dimmer, wilder branches, furs the last stretch with hair-thin streamers, and dissolves the tips away instead of stopping them square. 0 = the old blunt tips. |
| `..._frizzle_taper` | 0.82 | 0.3–1 | Arc fraction past which a free tip starts dissolving. 1 = no taper. |
| `..._frizzle_persist` | 0.5 | 0–1 | How brightly the *previous* roll's fray lingers under the current one, so the fine tips shimmer rather than strobe at the re-roll rate. 0 = off. |
| `..._frizzle_gens` | 2 | 1–3 | How many generations deep the fray branches. |
| `..._frizzle_sigma1` / `_sigma2` | 16 / 40 | 0–70 | How far the first and last generations lean off their parent, in degrees. |
| `..._frizzle_dim` | 0.5 | 0.1–1 | How much dimmer each generation is than the one it grew from. |
| `..._frizzle_streamers` | 1 | 0–1 | Density of the hair-thin stubs on the last stretch of each branch. 0 = none. |
| `..._sdf` | 1 | 0/1 | Draws the bolt as distance-field capsules accumulated with `max()` instead of overlapping additive ribbons, which removes the over-bright cross where segments meet. Needs the Metal renderer; OpenGL always uses the ribbon. |
| `..._sdf_gain` | 1 | any | Brightness of the distance-field bolt. 1.0 is where the centre burns white. |
| `..._sdf_falloff` | 15 | 0.25+ | How sharply it falls off across its own width. Higher is tighter and sharper-edged. |
| `..._fizz` | 0.666 | 0–0.9 | **Boils the bolt's own radius**, so its edge writhes and its width breathes along its length — the shape analogue of the lava shimmer, and noise rather than sines for the same reason. The value is the fraction of the radius the boil swings, so the shipped 0.666 is ±67%. **Seb tuned it by eye on 2026-09-01** — the still he first approved was 0.35 and he settled higher in motion. 0 is the old capsule **byte for byte**. Needs `..._sdf`, so Metal only. |
| `..._fizz_scale` | 0.35 | 0+ | Ripple frequency in cycles per **world** unit, so the boil keeps a fixed physical wavelength and does not stretch with segment length — a little under three units per cycle at the shipped value, i.e. a fine crackle rather than a slow undulation. |
| `..._fizz_speed` | 0.5 | any | How fast the boil travels along the beam, in cycles per second. Deliberately slow against a fine ripple: the crackle comes from the bolt re-rolling its shape fifteen times a second, so the boil itself only needs to drift. Negative runs it the other way. |
| `..._ballsize` | 22 | 4–96 | **Ball lightning** (`m5_balllightning`): radius of the plasma globe drawn at each ball — a blue-white electrode at the centre, nine fine filaments radiating out to the glass in a violet shift of the beam colour, re-rolled at the bolt's rate, drawn through the bolt renderer (the SDF pass on Metal, the ribbon on GL) so they take its thickness and fizz knobs. Console-only. |
| `..._ballcolor_red` / `_green` / `_blue` | 0.45 / 0.28 / 1 | 0+ | **Ball lightning**: the ball's palette, max-normalised — the shell, its filaments, its arcs and its light all key on it, independent of the thunderbolt's beam colour. The shell's boil mixes toward magenta and its rim toward electric blue from this base. Console-only. |
| `..._ballarcs` | 1 | 0/1 | **Ball lightning**: 1 draws the ball's arcs as plasma filaments — thinner, smoother, violet, no forks or frizzle, the globe's own filaments reaching out to the target. 0 draws them as ordinary M5 bolts (an A/B for Seb's eye). Console-only. |
| `..._ballpressure` | 1.2 | 0–2 | **Ball lightning**: strength of the violet cast that creeps in from the screen's edges as a ball comes within about one and a half of its reaches of you — measured client-side from the nearest ball each frame and painted in the Metal postprocess before the shoulder and the gamma. 0 = none. Console-only. |
| `..._muzzlenodes` | 1 | 0/1 | Let the arc wander between the gun's three electrodes instead of always leaving the right-hand one. It steps at most 15 times a second whatever `..._rate` is set to — that ceiling is compiled in, not configurable. 0 = the fixed right-hand prong. |
| `..._seed` | 0 | any | Test hook, console-only: pins the re-roll index so the bolt is identical every frame and every boot. 0 = live. |

**About `..._sdf`.** The additive ribbons the bolt is normally drawn with *sum* wherever
they overlap, and consecutive segments always overlap where they meet — so every kink
renders about twice as bright as the limbs either side of it. That is the bright cross or
diamond you can see at the joints, worst near the muzzle and in screenshots. Accumulating
with `max()` instead makes an overlap idempotent and the seams simply cannot occur; it also
lets a brighter branch win a crossing over a dimmer one.

The beam's colour comes from the same place it always did. The lightning textures bake
`intensity × (1, 2, 4)` into their pixels and clamp each channel, so the bright centre of a
bolt saturates to white while only the dim fringes keep the full weighting — and those
fringes are what make the bolt read electric blue at a blue-heavy colour setting. The
distance-field path samples no texture, so it applies that weighting, and its clipping,
itself. Your `r_lightningbeam_color_*` sliders mean exactly what they meant before.

At `0`, and on OpenGL, the old ribbon is used unchanged.
| `..._corevolume` | 1.2 | 1–3 | How much body the core has. Spread across the core's passes rather than piled onto one, so it widens the channel rather than just brightening it. |
| `..._clip` | 2 | 0–2 | Stop the bolt being drawn through walls: 0 off, 1 the main channel, 2 forks and filaments too. |
| `..._flicker` | 1 | 0–1 | Depth of the brightness flicker. **Clamped in code** — see below. |

Console-only, for finer shaping: `..._seglen` (18, world units per segment — shorter means
more, finer kinks), `..._decay` (0.68, how fast the wander shrinks at each subdivision level),
`..._axial` (0.18, how much segment *lengths* vary — 0 spaces the kinks evenly and reads
mechanical), `..._branchchance` (0.35), `..._corewidth` (0.5) and `..._sheathwidth` (1.3),
both as fractions of `r_lightningbeam_thickness`, `..._sheathalpha` (0.55),
`..._branchalpha` (0.5), `..._lightcount` (5), `..._lightradius` (320) and `..._lightwhite`
(0.35), `..._holdlevels` (2, how many of the coarsest subdivision levels the pose holds),
`..._coretube` (1) and `..._coreinner` (0.3) and `..._coretexture` (1) for the core's body,
`..._clipback` (4, standoff from a clipped surface), `..._hitfade` (0.25 s), `..._muzzle` (1)
with `..._muzzleforward/right/up` (22 / 5 / −8) and `..._muzzlefade` (0.15), and
`..._endtrace` (1).

Three of those are worth understanding rather than just tuning. **`..._muzzle`** starts your
own bolt at the gun's barrel instead of your waist, which is what makes it leave the weapon
properly when you look up or down; only the first 15% is moved, so where the bolt *lands*
never changes. **`..._endtrace`** re-traces where the bolt ends — under
`cl_beams_instantaimhack` the stock endpoint is a distance along your view axis and sits on no
surface at all, which is why the bolt could appear to run through a near wall. **`..._clip`**
pulls any node that strayed into geometry back to what is actually visible; it costs about 30
world line-traces per bolt per frame and fires only on the wide excursions.

`..._lightcount` and `..._lightradius` are worth a word: brightness is normalised for the
count, so changing it redistributes the light along the bolt without changing how bright the
room gets. It is therefore a pure cost knob — 5 rather than 8 is what keeps the feature free
on the benchmark. Radius is a genuine look control, and it matters more than intensity in a
big room: a light contributes *exactly* nothing past its radius.

### The flicker floor is a guarantee, not a setting

`..._flicker` sets the depth of the brightness modulation, but the bolt can never fall below
**55% of its peak** — `M5_FLICKER_MIN` in `r_lightning.c` is a compile-time constant, the two
modulation frequencies (2.7 Hz and 6.3 Hz) are constants too, and the *rate* is deliberately
not exposed at all. No value in any config can produce a strobe. The modulation is also
deliberately not synchronised to the shape re-roll, because 15–20 Hz is the worst band to put
a luminance flicker in; the crackle you see is the shape changing at near-constant brightness.

`r_lightningbeam_m5_test` verifies this from the console: it sweeps the envelope at 1 kHz for
a minute with the depth forced to 4.0, far outside its own bound, and prints the extremes.
`tests/smoke.sh` asserts on the verdict.

### What needs RT Shadows

**The world lighting and the fog scattering only work through the Metal RT sidecar.** With
`rt_metal 0` they do nothing at all — that is not a bug and not tuning. Modern DarkPlaces has
no dynamic-lightmap path, so outside the sidecar a dynamic light is a corona blob and nothing
else. The bolt geometry itself is unaffected and works everywhere. The fog scattering
additionally needs `rt_metal_fog 1` and `r_volumetric 1`.

### Connecting

The server tells the client the bolt is doing damage by sending `TE_LIGHTNING1` instead of
`TE_LIGHTNING2` — same beam, different bolt model, no extra network traffic. Two consequences
worth knowing: a demo recorded before this change will never show the boost, and with the M5
bolt off *and* Polygon Lightning off, a connecting bolt draws `bolt.mdl` rather than
`bolt2.mdl` (visually near-identical). The Shambler's attack has always been a `TE_LIGHTNING1`,
so the connect flag is scoped to player-owned beams and Shambler bolts are unaffected. Since
2026-09-06 the scope also admits beams owned by a ball-lightning ball (tagged `EF_M5BALL`
client-side), so an arc that damages something flares the same way.

---

## Diagnostics (console-only)

| Command / cvar | What it does |
|---|---|
| `r_volumetric_probe` | Prints what the murk sees — liquid or not, height above floor, corner term, resulting density — beside the engine's own answer, then a **vertical profile** walking up from the local floor so you can see whether the fog really settles or floats. The fastest way to turn "this looks wrong here" into a fact. Probes at your eye, or at `r_volumetric_probe x y z` for anywhere else (including inside a wall). It also prints the coordinates, which makes it this engine's stand-in for `viewpos` — there is no such command in DarkPlaces. |
| `r_lightningbeam_m5_test` | Verifies the thunderbolt's flicker floor: sweeps the envelope at 1 kHz with the depth forced past its cvar bound and reports the extremes. Should always say `floor holds`. |
| `r_volumetric_debug` 1–7 | Visualises depth / world position / a world-locked checkerboard / sampling density / height above floor / the liquid mask / the corner term. |
| `m5_fpslog` | Logs presented fps during demo playback. |

---


### Sound (SEPTEMBER2 Part B, 2026-09-06)

The taste rule: Quake's samples are the sound. Nothing here changes a sample's timbre in the open at close range — the dry path is **bit-identical** at the master's 0 (the `SND_DUMPMIX` WAV gate) and stays the loudest thing in the mix at 1. Reverb and filtering are what the *room* does to the sound, never what the sound is. Stereo only; music never enters either.

| Cvar | Default | What it does |
|---|---|---|
| `snd_reverb` | 1 | **Room reverb** (B1): one algorithmic stereo reverb (eight combs and four allpasses per channel, the Freeverb shape, decorrelated left/right) on a send bus after the mixer. The room is read from the level every frame — ten tracelines from where you stand give a mean free path and a sky share — so a crypt rings, a corridor is tight and open ground is nearly dry, cross-fading over half a second between rooms. Each sound's send scales with its distance (far sounds are wetter than near ones); your own weapon sends a quarter. `exec reverb_on.cfg` / `reverb_off.cfg`. |
| `snd_reverb_wet` / `_decay` / `_predelay` / `_lowcut` / `_senddist` | 0.15 / 1 / 15 / 180 / 700 | The wet level at full closedness (0.15 since 2026-09-07: Seb heard 0.3 as "a bit too glossy", halved on his word) (open sky takes it toward a sixth); a scale on the room's decay time (a small room ~0.4 s, a big hall ~2 s at 1); the pre-delay in ms; a high-pass on the send in Hz; the distance at which a sound sends fully. |
| `snd_reverb_report` | 0 | Prints the room estimate once a second (mean free path, sky share, decay, wet). Console-only. |
| `snd_occlusion_lowpass` | 1 | **Occlusion as a low-pass** (B2): a sound the existing occlusion test finds blocked is dulled as well as quietened — a one-pole low-pass per channel at `snd_occlusion_cutoff` (1200 Hz), so a Shambler behind a door reads as a muffled presence rather than a distant one. Needs `snd_spatialization_occlusion`. `exec occlusion_lp_on.cfg` / `occlusion_lp_off.cfg`. |
| `snd_airabsorb` | 0 | **Distance air absorption** (B3, 2026-09-10), 0-1: a far sound loses its top end the way a real one does -- the occlusion low-pass's own one-pole per channel, its cutoff falling with distance. Nothing within `snd_airabsorb_start` (300 units) is touched, bit for bit; by `snd_airabsorb_dist` (2000) the cutoff has fallen to `snd_airabsorb_cutoff` (2500 Hz), geometrically in between; the value scales how far along that fall a sound is taken. A blocked sound takes the lower of this and the occlusion cutoff. Mono world sounds only; music and local sounds never. `exec airabsorb_on.cfg` / `airabsorb_off.cfg`; 0 = the old bytes. |
| `snd_airabsorb_start` / `_dist` / `_cutoff` | 300 / 2000 / 2500 | The untouched radius, the distance the fall completes at, and the cutoff there, Hz. |
| `snd_occlusion_cutoff` | 1200 | The cutoff for a fully blocked sound, Hz. |

## Developer appendix

Two console-only diagnostic cvars belong here too, both default 0 and neither of any use in play: **`r_metal_forceencoderrestart`** ends and restarts the Metal render encoder before every draw (encoders start stateless, so this is the instrument for state-replay bugs; `r_metal_drawprobe` runs itself with it off and on and requires byte-identical readbacks), and **`r_shadow_bouncegrid_debugreport`** prints the bounce grid's build time and an FNV hash of its contents.

Environment variables for verification runs (start the app from a terminal to use them):
`RT_METAL_KERNELMS=1` prints per-kernel GPU times every 120 frames; `RT_METAL_DUMP=<path>`
with `RT_METAL_DUMPFRAMES="120,300"` writes raw framebuffer dumps at those timedemo frames;
`RT_METAL_VERBOSE=1` restores the per-change diagnostic chatter (the view-culled light
count and friends) that is otherwise silenced — ~19 lines/s under ordinary movement, which
under Xcode's debug console once accumulated to a 92 GB out-of-memory halt over an hour of
play, so it is opt-in for the debugging sessions it was written for;
`RT_METAL_PROFILE=1` prints the 120-frame RT profile line (`as/trace/total ms cpu` — the
arc's cost instrument; implied by `RT_METAL_KERNELMS` and `RT_METAL_VERBOSE`, set by
`test/bench-rt.sh` automatically, and off in ordinary play for the same console reason);
`RT_METAL_SYNC=1` forces the same-frame trace order — the historical verification switch,
superseded for players by the `rt_metal_sameframe` cvar, which additionally clamps the
now-identity reprojection (SYNC=1 deliberately keeps its slot-order-only meaning so
measurements taken under it stay comparable across sessions); `RT_METAL_NOTILE=1` bypasses the
tiled light cull; `RT_METAL_COMPOSITE_FLIP=1` composites the RT term upside down on purpose
(a deliberately-broken variant, so the upright one can be shown to be doing something);
`RT_METAL_REPROJ_TEST=1` pretends the shown frame was traced from 2° of yaw and 8 units
away, which is the only way to exercise reprojection on a static bed — **`=2` adds 2° of
pitch, and that second axis is not decoration**: yaw alone moves only the horizontal
coordinate, so it cannot see a vertical-sense error, and one hid in the Metal RT composite
from Phase 5-4 until 6-3b. See `CLAUDE.md` for the measurement methodology that goes with
them.
