# test/perf — the performance sweep suite

Built 2026-08-12 for the optimisation arc. `PERFPLAN.md` at the repo root is the
session plan this suite serves; this file is the operating manual.

## The quiet-machine checklist (do all of it, every time)

The tree's bench history is a catalogue of numbers poisoned by exactly these.

1. **Display**: the 240 Hz monitor as the ONLY display (third screen
   disconnected), set to 1920×1080 at 240 Hz in System Settings → Displays.
2. **Load**: nothing else running — no Chrome, no Xcode build, no Spotlight
   indexing after a reboot. `uptime` load below ~2.5. The sweep warns at 3.
3. **Frontmost**: the suite runs fullscreen by default (`PERF_FULLSCREEN=1`);
   a backgrounded window is presentation-throttled to the refresh rate and the
   numbers are garbage (documented: three runs pinned at exactly 120 on the
   120 Hz display).
4. **Binary**: the sweep re-runs `make sdl-release` itself — never trust "up
   to date" (the stale-binary trap is documented twice over).
5. **One sweep at a time**: never run cmdtrace, smoke or a second sweep
   concurrently. The cmdtrace excursions were measured under exactly that.
6. **Soak first** for a tier-table run: the quoted tier figures are "soaked"
   (a demo looped for ~10 minutes before the first counted round, so the
   machine is at its settled clock state), and a cold run reads 10-20% high.
7. **Read every row's `vidmode`** (added 2026-08-19). The sweep now PINS
   `vid_desktopfullscreen 0` + 1920x1080 — it used to inherit whatever the
   copied config archived, so nobody could say which fullscreen mode
   `out-tiers2` used — and records the run's `Video Mode:` in a `vidmode`
   column; a fullscreen run that landed anywhere else (the intermittent
   1920x1017 class) is flagged `!` on the arm, warned about, and dropped by
   `perf-report.py` with a printed count. Start a fresh `PERF_OUT` — an old
   `results.tsv` has the 14-column header.

## The three tools

```
sh test/perf/perf-audit.sh                   # ~1 min: what actually runs where
sh test/perf/perf-sweep.sh A                 # ~60-90 min: the broad screen
PERF_KERNELMS=1 sh test/perf/perf-sweep.sh B 'fog|rt|vol'   # the deep sweep
python3 test/perf/perf-report.py             # render report.html + findings.json
```

Phase A runs every A-row of `levers.tsv` on its bed — mostly demo14 (short,
fog+lava+RT-heavy), with effect arms on their matched beds (lightning on
demo9, lava on demo7, dark+torch on demo12). Phase B re-runs the shortlist
with magnitude scaling and per-stage GPU ms (`RT_METAL_KERNELMS=1`), and adds
demo11 (real play, e3m1) via `PERF_BEDS="demo11 demo14"`.

## Reading results

- `out/results.tsv` — every run, machine-readable, append-only.
- `out/report.html` — the findings report (the deliverable for Seb).
- `out/findings.json` — consumed by the retier step.

The doctrine the report enforces: warmup discarded, medians not means, deltas
only against the same bed's same-sweep baseline, and the null arms' spread
printed beside every table — **a delta inside the floor is noise, whatever it
looks like**.

## The beds

| demo | map | why it's here |
|---|---|---|
| demo14 | start (lava hall) | short (~1500 frames), fog+lava+RT dense — the screen bed |
| demo11 | e3m1 | Seb's real-play recording — the Phase B / tier-verification bed |
| demo12 | e1m1 | dark + slime + fixtures — the gloom look under load |
| demo7 | e1m7 | lava + Chthon — lava levers |
| demo9 | e1m5 | sustained lightning — bolt levers |
| demo5 | e1m3 | the classic long bed (6120 frames) — tier verification |
| demo8/10 | start | lightning alternates |
| demo6/13 | e1m2 | variety for the rolling tier verification |

Tier verification (plan phase 4) runs each candidate tier over demo5 + demo11 +
demo12 + demo14 back-to-back — the "rolling" real-play suite — and quotes
average AND 1-second minimum per bed.

## Interpreting stage ms

`RT_METAL_KERNELMS=1` prints per-stage GPU times (trace / fog / shaft) averaged
over 120-frame windows; the sweep records their medians per run. Stage deltas
are the optimisation-grade instrument — fps deltas alone can't say WHICH kernel
paid. The budget line to keep in view: 240 fps = **4.17 ms** for the whole
frame; 120 fps = 8.33 ms.

**`RT_METAL_ASSPLIT=1`** (2026-08-30, needs KERNELMS) splits that trace stage in
two. It is labelled `AS+trace` in the source for a reason: one command buffer
holds the entity BLAS, the light-core BLAS, the TLAS build AND the trace
dispatch, so every earlier statement about "the fixed floor" was arithmetic on a
two-point ladder rather than a measurement. With the split on, the parsed
`RT_Metal-kern` line's `trace` figure becomes the DISPATCH ALONE and a second
line appears with its own prefix:

```
RT_Metal-asplit: as 0.162 trace-only 0.293 ms gpu (as is 36% of the pair)
```

It is a sub-mode rather than a change to KERNELMS on purpose — splitting a
command buffer perturbs overlap, and every trace figure on record was taken
unsplit, so **do not mix split and unsplit numbers in one comparison.** Two
sanity checks worth repeating before trusting a run: the two figures should sum
to the unsplit stage within a few percent, and `as` should be FLAT as
`rt_metal_scale` moves (it was, at ~0.19 ms across a 4x pixel range). What it
found on the day it landed is in PERFPLAN item 10 — the dispatch has ~0.09 ms of
fixed cost of its own, the AS half swings 2x with entity load, and the "61%
fixed" figure is a property of the bed rather than of the engine.

`METAL_FRAMEMS=1` (2026-08-16) prints the RENDERER's own command-buffer GPU
time per 120 frames (sidecar kernels excluded); `METAL_FRAMEMS=2` additionally
times one region — today the murk composite draw — in its own command buffer.
Two caveats measured on the day it landed: the whole-frame number stretches
with DVFS whenever the window is presentation-throttled and with the sidecar's
buffers interleaving on the GPU (2.3–5.0 ms for one unchanged arm), so use the
REGION mode for a pass, and toggle the cvar under test IN ONE BOOT (defer …
every 2 s) so the arms share a clock state — separate boots do not.
