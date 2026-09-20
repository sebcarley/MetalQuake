# The instrument shelf

Correctness gates, pixel beds and measurement instruments for this fork. Most of
what lives here now serves the **Metal renderer / RT / volumetrics** arcs; the
collision and bouncegrid material this file opened with in July is the minority.

**Almost nothing here changes engine behaviour** — the one exception is
`bluenoise-gen.py`, which WRITES a committed engine header (`rt_bluenoise.h`).

## Index — what each instrument is for

**Gates (run these before believing a change is inert):**

| Script | What it settles |
|---|---|
| `cmdtrace.sh` | "GL is untouched" — hashes every backend call into a per-frame digest. Baseline `1645 66c209c4e0e81cab`. **Take three runs and use the mode**; its excursion has been measured from 1223 to 1752 on an unchanged tree. |
| `parity-4a.sh` | The GL-vs-Metal pixel bed: 36 frozen vantages, two same-backend control boots per side, then the cross. Since 2026-08-21 the default run is a ten-vantage CORE set (one per subsystem); `PARITY_VANTAGES=full` runs everything and is REQUIRED after any merge; `PARITY_VANTAGES="a b"` names a subset; `PARITY_EXTRA` appends cvars. **This is the byte gate** — its controls really do come out at zero. |
| `tgacmp.py` | The comparator `parity-4a.sh` calls: 2D and world gates plus a 16x16 block map so a failure says *where*. |
| `run-regime.sh` | Collision ray-cast bit-identity (see below) and a demo perf mode. |
| `avi2mov.swift` | Turns a DarkPlaces video capture (raw I420 AVI, which QuickTime reads and refuses) into an HEVC `.mov` (H.264 on request) with its PCM audio, AVFoundation only -- no ffmpeg on this machine. `swiftc -O test/avi2mov.swift -o /tmp/avi2mov && /tmp/avi2mov in.avi out.mov 60`. Feeds whichever writer input is ready; the two obvious feed orders both deadlock (the header says why). |
| `hordesoak.sh` | The headless horde soak with a SCRIPTED player (SEPTEMBER2 H): dm4, horde and the director on, every weapon, the super nailgun held while he turns at `SOAK_YAW` deg/s, `impulse 211` every 20 s; `SOAK_GOD=1` is the fat arm. Prints the wave, director and status lines and a one-line summary. An instrument for the director's numbers, not a gate. |

**Measurement (numbers, not gates — see the warning below):**

| Script | What it measures |
|---|---|
| `sharpness.py` | Acutance proxy (gradient + Laplacian). RELATIVE only: same scene, same camera. Written for the MetalFX 8-5 acceptance. |
| `weave.py` | Nyquist-band checkerboard energy — how LOUD the dither/grain is. Cannot tell a lattice from grain of equal energy; that is what `roll.py` is for. |
| `roll.py` | Pattern COHERENCE: high-passes two consecutive frames and cross-correlates over lags. Says whether a pattern is static, translating, or incoherent. |
| `rollbed.sh` | The frozen fog bed `roll.py`'s numbers are taken on. **A METRIC bed, not a byte bed** — two boots of one binary differ by mean 0.999 / max 211. |
| `rimcount.py` | Silhouette-outlier counting on the RT term dump (`RT_METAL_TERMDUMP`). |
| `flicker.py` | Consecutive-frame flicker over `RT_METAL_DUMP` raw frames — the crosshair-crop metric plus PNG/diff-image export. Born 2026-08-29 on Seb's demo17 fog speckle; the header carries the repeatable bench-rt bed and the recorded baselines (lightsample 0 collapses the speckle 10×). The acceptance bed for fog variance work. `fogseq` (2026-09-03) reads the FOG-BUFFER dumps `RT_METAL_FOGDUMP=1` writes beside each frame — the kernel's own surface before the magnification, the composite and MetalFX — as a relative metric; `fogpng` exports one; `burst` splits an 8-frame burst per texel into the VISIBLE temporal band and the Nyquist bin (a consecutive-frame delta is the period-2 alternation the eye cannot see, and it mis-ranked the spatiotemporal table); `darkspots` reads the fog buffer's local-contrast tail and exports the contrast map — the map at ×4 is what showed the fog's raw grain to be three to four texels across and the wider filter removing it. |
| `lookmetrics.py` | Absolute per-frame look: channel means, p99, flat-white %. |
| `look-ab.sh` | The look A/B at Seb's own config across the RT vantages. |
| `bench-rt.sh` | Interleaved A/B fps harness. **Read its header before benching** — it carries the `1920x1017` desktop-fullscreen trap. |
| `perf/` | The lever sweep: `perf-sweep.sh` phases A/B/T against `levers.tsv`, `perf-report.py` renders it. Has its own README. **Note it parses the `RT_Metal-kern:` KERNELMS line**, so that line's format is load-bearing. Since 2026-08-19 every row carries a `vidmode` column and a fullscreen run off 1920x1080 is flagged `!` and dropped by the report. |
| `ghost.py` | The temporal scaler's GHOST FRACTION — how much of the previous frame lingers — split by what is moving (particles now / particles gone / everything else) via a `cl_particles 0` oracle boot, with the spatial arm as the no-history control (must read ~0), a threshold sweep, a crawl row that catches over-masking, and a `--floor` mode. Lived as heredocs until 2026-08-19. |
| `ghostbed.sh` | The bed `ghost.py` reads: dumps `ghostbed.dem` frames 417-421 (or any demo/frames) under a pinned preamble, writes a manifest proving what ran (gamedirs, Video Mode, binary version), and can run a control binary from the repo root (`GHOST_BIN`). |
| `fogbed.sh` | ONE arm, ONE demo playback with Seb's config: frame dumps AND fog-buffer dumps (`RT_METAL_FOGDUMP=1`) at the requested playback frames, then `flicker.py` on both. `FOGBED_CAMTRACK=1` adds `RT_METAL_CAMTRACK=1`, one camera line per playback frame, so a demo's parked and moving stretches can be found from one playback (demo22 f3400–3407 walking / f5650–5657 parked is the 2026-09-03 bed). `FOGBED_BIN` runs a control binary; `FOGBED_CONFIG` a config SNAPSHOT (his live file is rewritten at every quit — snapshot it, and state every cvar an arm depends on in the arm); `FOGBED_TIMEOUT` (400 s) kills a hung engine; `RT_METAL_FRAMEOFFSET=N` in the env arg starts the jitter/pick counter at N, so N playbacks at history 0.01 give N independent realisations whose MEAN is the unbiased truth (a single 1-spp frame is skewed dark). Read a moving arm against its own frozen control (`RT_METAL_FRAMEPIN=1` + `rt_metal_lightsample 0`) and quote the excess. |
| `bluenoise-check.py` | Validates a blue-noise table OFFLINE before any boot: per-slice spatial spectrum bands, temporal blueness of each texel's sequence, the jitter/pick tap coupling, and two EMA convergence simulations (a jitter integrand and the fog light pick) at history 0.7 and 0.9 against an i.i.d. stream. It caught the first STBN cut's isotropic-3D-Gaussian mistake (45% of each slice's power below radius 4) and named the 8-slice cycle's EMA cap. |
| `bluenoise-gen.py` | Regenerates `rt_bluenoise.h` — since 2026-09-03 a 64×64×16 SPATIOTEMPORAL table (`--legacy` reproduces the old eight independent slices byte for byte). The one script here that emits engine source; regenerating it moves every frozen blue-arm baseline, which is why `parity-4a.sh` pins `rt_metal_bluenoise 0`. |

(`rt-suite.py` / `rt-report.py`, the oldest harness, were retired on 2026-08-18: a machine-specific Xcode DerivedData path, and `.app`-bundle numbers comparable with nothing else here.)

## The demo beds these instruments run on

Demos live in Seb's userdir (`m5/` is gitignored), so they are **not in the repo** and
must be re-recorded if lost. Recording one is a headless boot with deferred console
commands: `disconnect`, then `record <name> <map>`, then the inputs, then `stop`.

| Demo | What it is | Used by |
|---|---|---|
| `demo5` / `demo11` / `demo12` / `demo13` / `demo14` / `demo15` | Seb's own recordings — the perf and look beds. `demo13` f2018-2023 is the fog/ladder bed; `demo12` carries his e1m1 QA frames; `demo11` is the primary perf bed. | `perf/`, `bench-rt.sh` |
| `ghostbed` | **STATIC camera, rocket fired twice** (e1m3). The maximal case for camera-only motion vectors: every vector is exactly zero while the rocket moves 12-24 px/frame. Frames 417-421 are the bed (rocket in flight, trail behind it); the historic crop is `820 440 1140 760`. **A METRIC bed**: the `r_metalfx 0` arm agrees to 0.08-0.17% of pixels across sessions and binaries (measured 2026-08-19, `ghost.py --floor`), localised; temporal arms carry a global low-amplitude wash of 2-3% px at mean ~0.07 (the jitter phase differs per boot); and about 1 boot in 5 lands a MINORITY particle realisation that reads 1.9% px localised against the majority — an arm in that state reads ~0.07 more "ghost" against a majority-state truth, so read the `--floor` of the truth and rerun once. Never a byte gate. | `ghost.py` / `ghostbed.sh` (temporal ghosting) |
| `turnbed` | **Fast sustained turn with the weapon out** (e1m3). The viewmodel's error is proportional to camera motion, so this is where it is visible; a dark or static bed cannot measure it. | `r_metalfx_viewmodel` |
| `rollstill` / `rollbed` | Early parked-camera attempts, superseded by `test/rollbed.sh`, which needs no demo. | — |

## The rule that governs all of the above

**Establish an instrument's own noise floor before believing a number from it.** Two
boots of one binary, same everything. This tree has recorded the byte gate going blind
twice, the command digest going blind once, and a fullscreen bench reporting an
impossible 60% speed-up — each caught only by a control that should have been run first.

## Correctness net (gameplay safety) — headless

The collision ray-cast core (`collision.c`, `bih.c`, `model_brush.c`) is shared by
server physics, client prediction, monster AI, and QuakeC `traceline`. A change to
it must be **bit-identical** or gameplay breaks. The `collision_dumptraces` command
(dedicated server, no display) fires a deterministic, seeded set of line / box /
line-against-surfaces traces through that core and writes full-precision results.

Workflow around any change to the trace core:

    make sv-release
    ./test/run-regime.sh baseline   # capture golden dumps BEFORE your change
    # ... make your change, rebuild ...
    ./test/run-regime.sh check      # must report all PASS (bit-identical)

Baselines are local and platform-specific (float trace results are deterministic
per arch/compiler, not across them), so they are gitignored — capture them right
before the change you are validating.

### Coverage note
Stock id1 is Q1BSP. Q1 world line/box traces use the hull/clipnode path; only
`TraceLineAgainstSurfaces` reaches the triangle-BIH path. To exercise the BIH
world-traversal path, add a Q3BSP map to `MAPS` in `run-regime.sh`.

## Performance net

- **Frame time:** `./test/run-regime.sh perf` replays the stock demos via the
  engine's built-in `-benchmark` (already `srand(0)`-deterministic) → `benchmark.log`.
  Needs a GL display.
- **Bouncegrid:** `r_shadow_bouncegrid_debugreport 1` prints build time + an FNV
  hash of the grid after each build; the hash also proves an optimization is
  bit-identical. See `p0_bouncegrid.cfg`.
- **Lightmap bake:** `mod_generatelightmaps` prints total + per-phase timing.
- **Ray-cast throughput:** `collision_dumptraces` reports traces/sec (the
  micro-benchmark for BIH / ray-triangle optimizations).
