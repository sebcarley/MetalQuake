# M5 QuakeC mod source

Game-code half of the M5 fun mods. Compiles to `../m5/progs.dat`; run the
engine with `-game m5` to load it.

Base source: [CleanFixedQuakeC](https://github.com/Jason2Brownlee/CleanFixedQuakeC)
(commit `dacc69ef13d2c961776b708f33ad2be8e87da224`) — the GPL v1.01 Quake game
code with clean-room re-implementations of the v1.06 fixes. GPLv2, see
LICENSE.txt.

## M5 changes (all cvar-gated, vanilla when off)

- `weapons.qc` — `W_FireShotgun` / `W_FireSuperShotgun`: when `m5_shotgun` is
  set, Doom-style ballistics (10/21 pellets, tighter spread, harder view kick).
  The matching muzzle-flash and shell-casing visuals are engine-side in
  `cl_main.c` / `cl_particles.c`.
- `client.qc` — `PlayerJump`: when `m5_movement` is 2, holding jump keeps
  hopping (no pogo-stick suppression).
- `player.qc` — `ThrowGib`: when `m5_gore` is 2, every gib call throws three.
- `m5horde.qc` — horde mode wave director (see the header comment for the
  full design), plus hooks: `world.qc` spawns the director, `monsters.qc`
  strips map monsters, `combat.qc` counts kills, `player.qc` reports the
  run on death. All gated on `m5_horde`.

The `m5_*` cvars are registered by the engine (see `M5_Init` in `cl_main.c`)
and toggled from the M5 Mods options menu.

## Impulses (bindable / console)

The M5 mods reserve the 200-range impulses (`M5ImpulseCommands`, weapons.qc),
e.g. `bind p "impulse 222"`:

| Impulse | Action |
|---------|--------|
| 200 | cycle gore preset 0 → 1 → 2 |
| 201 | toggle Doom shotgun |
| 210 | horde: summon the next wave (works mid-wave; stragglers join it, no clear reward for a skipped wave) |
| 211 | horde: status report (wave / alive / budget) |
| 222 | Pentagram of Protection, 30 s (needs `sv_cheats 1`) |
| 223 | Quad Damage, 30 s (needs `sv_cheats 1`) |

## Testing

`tests/smoke.sh` boots the engine headless and asserts on the console log
(gamedir mount, horde, gore, shotgun, impulses, photo mode). Run it from
the repo root, or from Xcode via the SmokeTests scheme (Cmd+B).

## Building

```sh
./build.sh
```

Needs `../tools/fteqcc/fteqcc` (gitignored); build.sh explains how to
regenerate it.
