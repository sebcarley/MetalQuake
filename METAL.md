# QuakeM5 — the Metal renderer arc

*Authored 2026-08-04 (Opus 5 session). This is the forward plan for the fork's stated
endgame: a full Metal renderer, as an option alongside GL. Read it with CLAUDE.md, the way
ROADMAP.md was read for the August feature arc. Every anchor below was verified against
source at HEAD `7ab7ae72`.*

## Why, and what was decided

Metal today is the compute sidecar (`rt_metal.m`) bolted to a GL 3.2 renderer through an
IOSurface/CGL bridge with documented hazards — rectangle-texture binds that desync the GL
backend, a 526-line composite that saves/restores GL state around ~14 `glGet` readbacks,
2-slot synchronisation, one frame of latency and a reprojection apparatus to hide it. A
native Metal renderer dissolves that seam, and it is the only route to EDR output on the
OLED — GL on macOS structurally cannot.

Decisions made with Seb, 2026-08-04:

- **Parity first, then diverge.** Metal reproduces the GL look and is A/B-tested against it
  at every milestone. Metal-only features come only after parity is proven.
- **`vid_renderer gl` stays the default and untouched throughout.** Every milestone ends
  committable, with the GL path byte-gated. Pausing the arc at any milestone loses nothing.
  *(Held for the whole parity arc; retired at Phase 8, 2026-08-08, when metal became the
  macOS default on Seb's instruction — see the Phase 8 status block.)*
- **EDR is a first-class goal**, designed in from the start, not retrofitted.
- **Honest scale: ~7,500–10,000 new lines (~1,850 of them moved, not written), 23–32
  sessions to EDR.**

## What exploration established

- **The seam already exists.** `gl_backend.h` (114 lines, ~50 functions) is a complete
  backend-agnostic API, and the drawing code — `gl_rsurf.c`, `gl_draw.c`, `r_sky.c`,
  `r_sprites.c`, `r_lightning.c`, `cl_particles.c`, `r_explosion.c`, `sbar.c`, `menu.c`,
  `model_*.c`, `ft2.c` — makes **zero** raw GL calls. `renderpath_t` (`vid.h:35`) with its
  ~85 single-armed `switch (vid.renderpath)` sites is the vestigial multi-backend
  structure; `r_meshbuffer_t` still carries a spare `void *devicebuffer` slot from D3D9.
- **The GL dependence is concentrated** in `gl_backend.c` (the layer itself),
  `gl_textures.c` (the uploader) and `gl_rmain.c:1091–2468` (the shader block — permutation
  compile plus ~200 `qglUniform*` per batch in `R_SetupShader_Surface`). Genuinely
  scattered raw calls: ~10, of which 6 are the IOSurface bridge (deleted at Phase 5) and
  the rest are dead on macOS (`#if 0` blocks, occlusion queries needing GL 4.4 where macOS
  caps at 4.1, stereo) or trivial (2× polygon-offset enable, 1 init-time `glGet`).
- **Dead in this fork's configuration, not ported in v1** (~7,350 lines): realtime world
  lighting, shadowmaps, bouncegrid GI, deferred prepass, model shadows, water reflection
  planes, motion blur, stereo, video capture, occlusion queries (so `USEOCCLUDE` and its
  UBO are unreachable). Still live and required: map-entity light import
  (`r_shadow.c:5108`) and `R_Shadow_GetWorldLightPositions` (`:4060`) — they feed the
  sidecar — plus dlights and coronas.
- **The 2D path is the material path.** `DrawQ_*` → `Mod_Mesh_*` →
  `R_DrawModelSurfaces(ui=true)`. There is no cheap 2D-only backend — but once 2D works,
  the core exists.
- **Shader reality.** One uber-shader (`shader_glsl.h`, 2,311 lines); the lit modes share
  one 1,160-line `main()` (`:1143–2302`), so skipping the dead modes saves ~18%, not half —
  the MSL port is ~1,850 lines regardless. All 32 permutation bits are used; new features
  must be static parms.
- **Unearned luck:** `R_BUFFERDATA_CYCLE` is already 3 (`gl_rmain.c:4037`) — the dynamic
  buffer ring triple-buffers, which is exactly Metal's CPU/GPU overlap requirement.
- **Environment:** SDL is sdl2-compat forwarding to SDL3; `SDL_Metal_CreateView` /
  `SDL_Metal_GetLayer` confirmed exported. `CAMetalLayer`'s EDR properties and
  `NSScreen.maximumExtendedDynamicRangeColorComponentValue` confirmed in the macOS 26 SDK.
  The offline Metal toolchain is **not installed** — runtime `newLibraryWithSource:` (the
  sidecar's proven approach) is the compile path, `MTLBinaryArchive` (not deprecated) the
  cache. Classic Metal 3 command API, not Metal 4 — the sidecar is classic, the tooling is
  mature, and argument tables solve a draw-call scale Quake does not have. `MTL4Compiler`
  is the one reserve if compile hitching beats the mitigations below.

## Architecture

### Seam and dispatch

- `RENDERPATH_METAL` joins `renderpath_t`. The ~85 existing `switch` sites get explicit
  Metal arms — the compiler's `-Wswitch` enumerates the port surface as a checklist that
  cannot go stale. **No function-pointer table**: the GL arms stay textually unchanged,
  which is what the verification culture rewards.
- `vid_renderer` cvar (`CF_ARCHIVE`, default `"metal"` on macOS since Phase 8 — it spent
  the parity arc unarchived and defaulting `"gl"`), read once in `VID_InitMode`
  (`vid_sdl.c:1935` — the historical dispatch point), applied on `vid_restart`. Console
  recipe: `vid_renderer metal; vid_restart`.
- New files: `vid_metal.m` (window/CAMetalLayer/present/EDR; owns the device and queue),
  `metal_backend.m` (implements `gl_backend.h`), `metal_textures.m` (implements
  `r_textures.h`), `metal_shader.m` (permutation compile, reflection→offsets, binary
  archive), `shader_msl.h` (MSL prologue), `shader_body.h` (the shared shader body, moved
  verbatim out of `shader_glsl.h`).

### The uniform indirection (the crux)

`R_SetupShader_Surface` must serve BOTH backends — this repo has been burned twice by
lockstep duplicates and will not create a 600-line third. `R_Shader_Uniform{1i,1f,2f,3f,4f,
Matrix4fv}` + location lookup: under GL a GL uniform location (the wrapper costs
~0.05 ms/frame, unmeasurable); under Metal a byte offset into a per-draw constant struct,
obtained from `MTLRenderPipelineReflection` (`fragmentBindings` →
`bufferStructType.members[].offset`). **The correctness detail that makes it work**: the
MSL uniform struct members carry the same `#ifdef` guards as the GLSL declarations, so
absent uniforms report `loc < 0` exactly as GL — five `if (loc >= 0)` guards wrap side
effects (matrix concats, `R_Volumetric_GetNoiseTexture()`, the RT-liquids bind) and must
not start firing. Staging rides the existing `R_BufferData_Store` uniform ring.

### Pipeline state and passes

- Only blend / colour mask / alpha-to-coverage / attachment formats / vertex layout bake
  into the `MTLRenderPipelineState` key (16 bytes, hashed); cull, scissor, viewport, depth
  bias and depth-stencil state are encoder-level. On Apple GPUs blending is fixed-function,
  so an estimated 300–800 PSOs cost only ~25–50 real shader compiles.
- Two vertex layouts: colour-array-on and colour-array-off — `GL_Color` is a constant
  vertex attribute (`gl_backend.c:1433`), which Metal expresses as a
  `MTLVertexStepFunctionConstant` slot fed from a 16-byte ring allocation.
- `R_Mesh_SetRenderTargets` + `GL_Clear` map to deferred encoder creation with load
  actions. Exactly one `Metal_EnsureEncoder()` / `Metal_ApplyState()` — Metal encoders
  start stateless, and scattered state replay is the single largest bug source; a debug
  mode forces an encoder restart before every draw and asserts pixel identity.
- **Known trap:** the one live scissored clear (`gl_rmain.c:7580–7582`, fires only when
  `r_fb.rt_screen` is NULL) must become a scissored quad, never a load action, or it wipes
  the HUD on the no-offscreen path — which is not Seb's config, so QA would never catch it.
  Smoke check with `r_viewfbo 0 r_volumetric 0 viewsize 90`.
- Same-device resources are auto-hazard-tracked; the renderer needs no fences. The
  sidecar's manual 2-slot guard is an artefact of the *cross-API* bridge, not of Metal.

### Coordinates: mirror everything, flip once at present

Projection composed as `F·S·P_gl` (z [-1,1]→[0,1], NDC y negated) with
`frontFacingWinding = Clockwise` globally. Window depth, `ScreenToDepth`, `gl_FragCoord`,
viewport y, `R_CalcTexCoordsForView`, screenshots and the composite's 0.0625 view-model
depth all keep GL numerics — render targets store GL-layout images. One dedicated present
pass samples v-flipped into the drawable (~0.1 ms) and is exactly where the EDR mapping
lives. `R_Viewport_TransformToScreen`'s CPU z mapping gains the matching [0,1] form.
Oblique near-clip (water) must compose before `S` — dead until water lands; noted.

### Shaders: one shared body, two prologues

`shader_glsl.h` splits into a GLSL prologue plus `shader_body.h` (the ~1,850-line body,
verbatim); `shader_msl.h` is a second prologue — uniform struct with the same `#ifdef`s,
interpolant structs, `dp_texture2D` → `t.sample(...)` macros, entry points. The body
becomes structurally impossible to drift. **The split MUST land before any MSL exists**, or
the murk density model becomes a three-way lockstep. Proof the split is inert:
`r_glsl_dumpshader` byte-identity on every live permutation — a total proof, stronger than
the comment-control method. Translations: `sampler2DShadow` → `depth2d` + `sample_compare`
(sampler cache keyed on the compare flag); the 3 `sampler2DRect` uses are **not ported**
(they die at Phase 5); `dFdx/dFdy` → `dfdx/dfdy`; the skeletal UBO → a plain buffer bind;
`[[invariant]]` on position. A smoke check asserts GLSL/MSL prologue uniform-name-set
equality. SPIRV-Cross rejected (build dependency, unreadable output in a read-the-source
verification culture); function constants rejected (a fixed struct layout would fire the
five side-effect guards).

### Compile hitching

Runtime MSL compile ≈ 100–400 ms per permutation. Mitigations in order: (1) async
`newLibraryWithSource:completionHandler:`, rendering with the nearest already-compiled
permutation meanwhile — the bit-stripping fallback loop already exists in
`R_SetupShader_SetPermutationGLSL`; (2) `MTLBinaryArchive` persisted to the userdir
(`m5/metalcache-<hash>.bin`, keyed on build id + driver + device; purge command); (3)
precompile the hot set during map load.

### Native RT (Phase 5 — the payoff)

The enabler lands at Phase 0: `RT_Metal_InitWithDevice(dev, queue)`, nil meaning "create
your own" — a provable no-op under GL. At Phase 5 the sidecar shares the renderer's device
~~and queue~~ **— the queue is deliberately NOT shared, corrected at 8-1d: 5-1 shipped
`RT_Metal_InitWithDevice(VID_Metal_GetDevice(), NULL)` (Seb's call, the decision comment in
full at the vid_sdl.c call site), because the backend commits one command buffer per frame
at end-of-frame and putting the trace on that queue would serialise it behind an open render
buffer, erasing the async overlap. The renderer↔sidecar seam is ordered by the CPU wait on
the shown slot's command buffer, not by submission order**; its outputs become plain
`MTLTexture`s bound through `R_Mesh_TexBind`; the
composite becomes an ordinary draw through the backend. ~~Compiled out on the Metal path
(kept for GL):~~ **RUNTIME-DEAD on the Metal path — nothing is compiled out, and the
original wording here was wrong** (corrected at 5-6; the audit is in that status block).
`vid_renderer` switches at **runtime** inside one binary, so every GL line has to stay
present and reachable; "compiled out" only becomes available if this fork ever drops the GL
renderer, which is not the plan. What is runtime-dead: the IOSurface/CGL bridge, the
composite's GL half, `R_RTLiquid_UnbindAll` and the rectangle-texture hazard class at the
liquids bind. What is **not** dead yet — and is Phase 6's, not 5-6's — is the murk's own two
rectangle binds, because MODE_VOLUMETRICFOG sentinels magenta on Metal. The 2-slot async
design and reprojection **stay** — they
are the async architecture, not bridge artefacts. `rt_metal_sameframe` becomes a cheap
experiment: same-frame tracing would obsolete reprojection, at the cost of putting the
trace on the critical path. Measure, don't assume.

### EDR (Phase 7)

> **Status, 2026-08-08: SLICE 7-1 IS DONE (`267af011`) — the gamma ramp is a
> curve, not a table.** That is the plan's own "the real work is gamma", and it
> is the prerequisite everything else in the phase rests on: the LUT is 256
> entries of 8 bits INDEXED BY THE COLOUR, so it can neither accept nor emit a
> value above 1.0 and the ceiling is in its construction. `r_gamma_analytic`
> (default 0, static parm, both backends) evaluates the curve instead. Validated
> as the plan asks — `r_gamma_analytic_test`, 256 samples, max **0.500** 8-bit
> levels against the LUT across five configurations, which is the ROUNDING FLOOR
> rather than a chosen tolerance; the shader arm is byte-identical to the LUT
> path on both backends, and a deliberately broken variant moves 19.417% of the
> frame identically on both. Smoke is 87 checks.
>
> **THE SCREENSHOT READBACK IS HEREBY DECIDED, which the plan requires before any
> EDR code is written: OPTION A — the readback stays 8-BIT AND TONE-MAPPED.**
> Screenshots and every parity capture continue to be the SDR image.
>
> The reasoning, and it is not a preference: **the entire verification apparatus
> of this arc is a DELTA instrument denominated in 8-bit code units.** Every
> acceptance threshold in this document — max delta ≤ 2, p99.9 ≤ 4, mean ≤ 0.3,
> the ">8" outlier counts — is in those units, as is every recorded number from
> Phase 0 to Phase 6. `test/tgacmp.py` parses 8-bit truecolour TGA and subtracts
> raw bytes; `test/lookmetrics.py` inherits that and adds a flat-white canary
> hardcoded at 250, which is the RT wall-lighting instrument. A 16-bit readback
> is not a new file format — it is a change to the `unsigned char *` contract
> that `GL_ReadPixelsBGRA` and all three of its callers are built on, and it
> would retire every documented number in this file at once. Phase 7's own
> acceptance row is written in those units too ("headroom-1.0 byte-identical to
> Phase 4d"), so a 16-bit readback would make its own acceptance unevaluable.
> A 16-bit PNG arm is small (libpng supports it, `qpng_set_IHDR` already takes
> the depth) and can be added later as an EXTRA path for looking at EDR output;
> it must not become the parity path.
>
> **THE GRANT IS THE THING TO GATE ON, AND IT IS NOT THE PROPERTY THIS PLAN
> NAMED.** Measured on the MAG 272U X24 with macOS HDR mode **enabled** and the
> display set as main:
>
> | property | value |
> |---|---|
> | `maximumExtendedDynamicRangeColorComponentValue` | **1.0** |
> | `maximumPotentialExtendedDynamicRangeColorComponentValue` | **4.65** |
>
> macOS grants headroom only once a layer **asks** for it
> (`wantsExtendedDynamicRangeContent`), so the current value sits at 1.0 on a
> perfectly capable display until something requests EDR. **A design that gates
> on `maximumExtendedDynamicRange…` — which is what the plan paragraph above
> said to do — therefore never engages at all.** Gate CAPABILITY on *potential*,
> drive the shoulder from *current*, and re-read current every frame because it
> is dynamic (it falls as SDR brightness rises, and with thermals). This also
> corrects an earlier draft of this block, which read the 1.0 as "no EDR here"
> and declared the phase hardware-blocked: HDR mode is necessary, **not
> sufficient**, and nothing was blocked.
>
> **Organising principle, which is what makes the acceptance provable: tie the
> format chain to the ASK, not to the GRANT.** Once `r_edr` engages, the drawable
> and `mb_screentex` go 16F and stay there; H merely tracks whatever the OS
> allocates, starting at 1.0. That buys byte-identity at `r_edr 0` by
> construction, no format thrash when the OS re-allocates mid-session, and
> byte-identity at `r_edr 1` on a display with no potential — so "degradation on
> a no-headroom display is free" becomes true by construction rather than by
> argument. **`r_edr` is opt-in, default 0** (Seb's call), like everything else
> in this fork.
>
> **Three corrections to this document, each verified, and two of them are 7-1's
> own:**
>
> 1. **"extended LINEAR colour space" above is WRONG for this engine.**
>    DarkPlaces' postprocess emits *display-encoded* values — at default cvars
>    `vid_gammatables_trivial` is true and no curve is applied at all. Tagging
>    those as linear makes the compositor apply the display OETF, and 0.5 grey
>    displays at roughly 0.73. Use an **extended sRGB** space, behind a cvar
>    until it is measured by eye.
> 2. **AppKit does NOT cost a framework in both build systems.** `otool -L`
>    shows `Cocoa.framework` already linked, because `sdl2-config --libs` emits
>    it. Only **CoreGraphics** is genuinely missing (for
>    `CGColorSpaceCreateWithName`). Note also that the two build systems already
>    disagree — the makefile links QuartzCore and not Cocoa, Xcode the reverse.
> 3. **The ceiling substitution is THREE literals, not two, and one of them 7-1
>    introduced.** The analytic gamma clamps to 1.0 *after* the shoulder, so a
>    reshaped shoulder would be undone six lines later:
>    `shader_glsl.h:456` (POSTPROCESS), `:884` (GENERIC) and `shader_msl.h:472`
>    (the shared helper, used by both modes). **Only POSTPROCESS's ceiling
>    rises** — GENERIC is the 2D/UI path and the HUD must stay SDR. That
>    asymmetry is not a nuisance: it is what gives the by-eye test an in-frame
>    reference, since HUD white saturates at exactly 1.0 while the scene runs to
>    H in the same frame.
>
> **The remaining work, scoped from a full seam survey so the next session does
> not re-derive it. The risk is NOT in the drawable:**
>
> - `vm_layer.pixelFormat` (`vid_metal.m:85`) is one line and the present PSO
>   already keys on `drawable.texture.pixelFormat`, so the pipeline adapts free.
>   **Changing only this yields a 16F drawable fed from an 8-bit source** —
>   visually identical to today and a green test that proves nothing. It is the
>   LAST link, not the first.
> - `mb_screentex` is hardcoded `BGRA8Unorm` and `r_viewfbo 2` does **not** reach
>   it — that cvar sizes the offscreen *scene* buffer, and the postprocess pass
>   writes that float scene into fbo 0 == `mb_screentex`, quantising there. This
>   is the real clamp.
> - **`mb_readback` hardcodes 4 bytes per pixel in three places** (`:1982` and
>   its two callers) and the `VID_METAL_PROBE` branch of `Metal_Backend_EndFrame`
>   hardcodes it against `drawable.texture` directly. Latent today because
>   everything is BGRA8; the moment either format moves the readback silently
>   delivers half a frame — *and that probe is the only instrument covering the
>   present pass*. Derive the stride from the pixel format **before** touching
>   any format.
> - **`Metal_Backend_CopyToTexture` breaks at 16F**: it blits fbo 0 into
>   `loadingscreentexture`, created 8-bit at `cl_screen.c:2563`. RGBA16Float →
>   BGRA8Unorm is a validation failure; it needs a render-pass fallback.
> - **A 16F fbo 0 stops clamping intermediate 2D blend results**, so additive UI
>   draws can escape above 1.0. Measurable, and it is exactly what the
>   dequantisation acceptance threshold is designed to catch.
> - **`v_psycho` needs NO new signal path**, which is a saving 7-1 made without
>   meaning to: the `edr_wanted` predicate includes `VID_GetGammaAnalytic()`,
>   and that already refuses under `v_psycho` and under `vid_sRGB`.
> - **Do NOT call `VID_Metal_*` from `gl_rmain.c`** — it is `OBJ_COMMON` and
>   links into `darkplaces-dedicated`. Communicate through `viddef_t` fields.
>   This is the exact class that broke `make sv-release` twice already.
> - `cl_capturevideo`'s `GL_CaptureVideo_*` have **no renderpath switch at all**
>   and issue `qgl*` unconditionally, so video capture is already broken on
>   Metal independently of EDR. The readback helper above could give it a Metal
>   arm nearly free.
>
> **A limitation to record rather than solve: no automated instrument in this
> tree can see the colorspace choice.** Screenshots read `mb_screentex`, never
> the drawable, so extended-sRGB versus the extended display profile is **by eye
> only**. Do not let a green test imply coverage of it.
>
> The shoulder reshape is independent of all of the above and is the best
> effort-to-proof ratio in the phase: the ordering it depends on (shoulder
> BEFORE the ramps, because the ramps are a colour-indexed lookup) is already
> correct on both backends, and the shoulder is force-zeroed unless
> `r_viewfbo >= 2`, so "at H = 1.0 algebraically identical to today" is testable
> **now, with no EDR code in the tree at all**. The `fbo2` vantage is the bed —
> and its own note records that the intuitive settings prove nothing (0 px
> moved), so it must be run at `r_hdr_scenebrightness 6`.

> **Status, 2026-08-08 (later): SLICES 7-2 THROUGH 7-6 ARE DONE — PHASE 7 IS
> COMPLETE. `r_edr 1` puts genuinely above-white pixels on Seb's MAG 272U X24,
> and the OS confirms the allocation.** Measured end to end on the fbo2 camera
> with `v_contrast 4` as the emitter: the drawable comes back **RGBA16Float with
> a per-channel maximum of 1.756 over 516 of 307200 pixels**, against an `r_edr 0`
> control of **8-bit, maximum exactly 1.000, 0 pixels above white** — and 1.756
> is not a number the renderer chose, it is *exactly the headroom NSScreen said
> it had granted*, which is the numeric acceptance this block asked for rather
> than an impression.
>
> **THE ASK IS WHAT MACOS REACTS TO, NOT THE FORMAT, AND THAT IS A TRAP WORTH
> STATING FIRST.** The `r_edr_stage` experiment was run before any format code
> shipped on, exactly as scheduled, and it is unambiguous:
>
> | `r_edr_stage` | what is set | MAG granted | built-in granted |
> |---|---|---|---|
> | 0 | nothing (control) | 1.000 | 1.000 |
> | 1 | `wantsExtendedDynamicRangeContent` only | **1.756** | 1.292 |
> | 2 | + extended sRGB colour space | 1.756 | 1.292 |
> | 3 | + RGBA16Float drawable | 1.756 | 1.292 |
>
> So `wantsExtendedDynamicRangeContent` **alone** takes the grant from 1.000 to
> 1.756 — on an **8-bit BGRA drawable with no colour space**, which cannot carry
> a single value above white. **The grant is therefore not evidence that anything
> extended-range reached the screen**, and a design that treated "current rose
> above 1.0" as confirmation would have reported success while every highlight
> still clipped. It is the "16F drawable fed from an 8-bit source" no-op this
> document already warns about, wearing the OS's own approval as a disguise.
> `vid.edr_active` is gated on the FORMAT (stage 3) for that reason, and
> `vid.edr_headroom` is written only when it is true.
>
> Four more measured facts from the same experiment, none of them guessable:
>
> - **The grant settles far below potential**: 1.756 against a potential of
>   4.655, i.e. **38%**. It is brightness-dependent, and the earlier plan
>   language of "potential becomes actual" would have been wrong by 2.6x.
> - **It is immediate at this scale** — 1.756 is already there at t+0.0 and
>   constant across the full 3 s series, at 0.5 s granularity. No ramp to wait
>   out, which is why nothing here is engineered around one.
> - **BOTH attached displays receive a grant**, not just the one holding the
>   window (the built-in went 1.000 -> 1.292). The grant is a system-wide
>   reallocation triggered by any layer asking, so "did OUR display change" is
>   the wrong question to grep for.
> - **`SDL_Metal_CreateView` returns an `SDL3_cocoametalview`** — a genuine
>   `NSView` subclass, through sdl2-compat. The open question about the view's
>   class is answered, and the `isKindOfClass:` guard stays anyway.
>
> **7-2 — the readback learns its own stride.** `mb_readback`, its two callers
> and the `VID_METAL_PROBE` branch all took bytes-per-pixel from the format
> instead of the literal 4, and the format's conversion to the 8-bit BGRA
> `unsigned char *` contract lives in one function. `r_metal_readbackprobe` is
> the bed: four formats through the real readback path, **0 of 140 bytes off in
> each**. **The fixture is 7x5 and the extents are the finding.** CLAUDE.md's 6-1
> lesson is that a square probe cannot see a width/height transposition, because
> `bytesPerRow` and its transpose are the same number — measured again here, in
> the opposite direction: the transposed-stride break moves **112 of 140 bytes in
> ALL FOUR formats**, which an NxN fixture would have scored a clean pass. All
> three breaks were verified and each is localised to what it broke — a wrong
> bytes-per-pixel moves only the format that has it (63 of 140, max 238), a
> missing channel swap only the format that needed it (70 of 140, max 193).
>
> **7-3 — the shoulder and the ceiling take a headroom.** `HdrShoulder` is a
> `vec2` (knee, headroom) and the analytic gamma's output ceiling is
> `GammaAnalyticB.w`, floored at 1.0 in all three shader sites so a caller that
> forgets degrades to today rather than to black. **POSTPROCESS is handed the
> headroom and MODE_GENERIC is handed 1.0** — the HUD stays SDR, which is not a
> nuisance but the in-frame reference the by-eye test needs. Byte-identical to
> the pre-7-3 binary across **14 captures** (spawn, fbo2, bloom, fxaa on both
> backends, plus spawn/fbo2/bloom again at Seb's gamma trio with
> `r_gamma_analytic 1`), every control 0.0000.
>
> **AND THE BYTE-IDENTITY WAS NOT BELIEVED UNTIL IT WAS BROKEN, which is 7-1's
> own lesson landing for the fourth time in this arc.** Two deliberately broken
> builds, both with clean controls:
>
> | break | moves | on GL | on Metal |
> |---|---|---|---|
> | headroom forced to 2.0 (the shoulder's `.y`) | 16648 px, 5.419% | mean 0.3652 | mean 0.3652 |
> | gamma ceiling floored at 0 and pushed 0.35 | 15400 px, 5.013% | mean 0.2675 | mean 0.2675 |
>
> Identical to four decimal places on both backends, and the two counts differ
> from each other — so the two paths are separately live rather than one masking
> the other, and both the GLSL and the MSL arms genuinely evaluate them. A limit
> worth recording rather than glossing: **the ceiling break moves 0 pixels on the
> `spawn` vantage**, because Seb's gamma trio puts that whole frame in the bottom
> few levels (5-6 measured p99 = 4 of 255) and a ceiling of 0.35 never binds
> there. The ceiling is only demonstrable where the scene has range.
>
> **7-4 — the display speaks; no pixels move.** `r_edr` (0/1/2), `r_edr_stage`,
> `r_edr_colorspace`, `r_edr_report`. The predicate is deliberately SPLIT: the
> renderer's half is one expression in `R_UpdateVariables` writing
> `vid.edr_wanted` (r_edr, Metal, `r_viewfbo >= 2`, `r_gamma_analytic`, and
> `VID_GetGammaAnalytic()` — which is what makes `v_psycho` and `vid_sRGB` refuse
> with no signal path of their own), and the display's half is ANDed in
> `vid_metal.m`, the only place that can see it. Neither side can answer the
> other's question, so neither duplicates it — and `gl_rmain.c` still calls no
> `VID_Metal_*`, which is the rule that broke `make sv-release` twice.
> CoreGraphics is added to both build systems, and the pre-existing
> makefile-vs-Xcode framework disagreement (QuartzCore vs Cocoa) is reconciled by
> naming both in both.
>
> **A hazard closed by construction, not by care: dropping the ask RESTORES the
> layer's original colour space rather than clearing it.** `CAMetalLayer.colorspace`
> is not documented to start nil, and assigning NULL where the system had put a
> real space would silently turn colour matching off for the whole window — a
> genuine change to the presented picture that **no instrument in this tree can
> see**, because screenshots read `mb_screentex` and never the drawable.
>
> **7-5 — the layer asks.** The drawable and `mb_screentex` go RGBA16Float
> together, from one signal (`vid.edr_active`), so they cannot drift into the
> proves-nothing configuration. Two hazards landed with it. `Metal_Backend_CopyToTexture`
> gets a **render-pass fallback** for the format mismatch — a blit encoder cannot
> convert, and under EDR fbo 0 is 16F while both destinations (the loading
> screen's copy of the frame, motion blur's ghost buffer) are 8-bit; the new
> `MB_PROGRAM_BLIT` is the present pass **without its v-flip**, because both sides
> of a copy hold GL-layout images and row r belongs at row r. And the 2D-escape
> hazard — a 16F fbo 0 stops clamping intermediate 2D blends — is what the
> acceptance threshold below is shaped to detect.
>
> **7-6 — the headroom reaches the curve.** One line, read one frame stale, and
> deliberately NOT smoothed: the OS ramps granted headroom and SDR brightness
> together and the two cancel, so a filter would show as a drift where the raw
> value shows as nothing.
>
> **`r_edr_probe` is the only instrument in this tree that can see above 1.0**,
> and it reports the screen texture beside the drawable on purpose: **the two
> agree exactly** (1.756 / 1.756, 516 px / 516 px, identical band histograms),
> which is what proves the extended values came through the scene and survived
> the present pass rather than being manufactured by it. It needs
> `VID_METAL_PROBE=1`, because a readable drawable is the slower configuration.
>
> **The colorspace choice remains BY EYE ONLY** — screenshots read `mb_screentex`,
> never the drawable — and that limitation is now load-bearing rather than
> theoretical, since `r_edr_colorspace 2` (extended *linear*) is a real and
> plausible-looking wrong answer for an engine that emits display-encoded values.
> No green test covers it; Seb's eye does.
>
> **AND SEB'S EYE HAS NOW RULED (QA, 2026-08-08): `r_edr_colorspace 1` (extended
> sRGB) "looks great" and the glow "is lovely all over the place" — the default
> stands confirmed and the by-eye item is CLOSED.** The drag-to-built-in negative
> control also ran by hand: no misbehaviour, the picture just reads very dark
> there — which is his OLED-tuned gamma trio on an SDR-bright LCD plus the
> smaller grant (1.292 against the MAG's 1.756), not a defect. One expectation in
> the docs was falsified and has been corrected in place: **the desktop does NOT
> visibly dim on his setup when the grant lands**, though the grant measurably
> lands — so the "external tell" this block once suggested is not reliable in
> either direction, and the in-frame HUD-vs-scene comparison is the only by-eye
> test worth prescribing.

`CAMetalLayer` RGBA16F + an extended colour space + `wantsExtendedDynamicRangeContent`;
headroom from `NSScreen`, polled on move/display change — never assumed (Seb's current display
mode may report 1.0). *Two clauses of this paragraph were corrected at 7-1 and the corrections
are in the status block below: it said "extended LINEAR", which is wrong for an engine that
emits display-encoded values, and it named the wrong NSScreen property to gate on.*
`r_viewfbo 2` forced on the Metal path. **The real work is gamma**: the ramp LUT (built in
`VID_BuildGammaTables`, sampled in POSTPROCESS *and* GENERIC — two sites) clamps at 1.0 by
construction. It becomes analytic — `BuildGammaTable16`'s formula evaluated in-shader from
four scalars — shipped as a static parm on BOTH backends, default off, validated
numerically against the LUT (256 samples, smoke test) on GL first. *Corrected at 7-1: it is
**ten** scalars, not four, whenever `v_color_enable` is on — a gamma, a scale and a base PER
CHANNEL plus one shared contrast boost — packed as three `vec4`s. And the LUT it is validated
against is **8-bit**, not the 16 its builder's name suggests: `BuildGammaTable16` emits
`unsigned short` but the texture rounds every entry to `unsigned char`, so the agreement to
aim for is half a code unit, not exactness.* `r_hdr_shoulder`
reshapes to asymptote at headroom H; at H = 1.0 it is algebraically identical to today, so
degradation on a no-headroom display is free. `v_psycho` stays on the LUT and disables EDR
while active. The screenshot readback (tone-mapped SDR vs 16-bit) is decided BEFORE any EDR
code — screenshots are the verification instrument.

### Phase 8 — same-frame RT + MetalFX upscaling, spatial then TEMPORAL (THE PLAN)

> **Status: plan APPROVED by Seb as written, 2026-08-08. 8-1 is DONE the same evening —
> four commits (`f2fe3f87` the Metal dump arm, `c90cfab6` probe-present + queue comments,
> `9e242e28` the bench harness, `54440de8` the profile line's seam fix) and the
> measurements below. Seb decided on them the same night: same-frame SHIPS (optimise
> later — 8-3 deferred, not scheduled), the 5-1 queue decision STANDS, and vsync is to
> be ensured. 8-2 is DONE (`199769e8` — `rt_metal_sameframe`, default 0, archived; the
> flip drains in-flight buffers, a missing same-frame buffer falls back to re-showing
> the previous slot, and reprojection is clamped to its documented bit-for-bit disabled
> paths through one `rt_reproject_live()` helper across all three lockstep filter
> sites, with the fog/shaft EMA gates deliberately left live and `RT_METAL_REPROJ_TEST`
> holding the clamp open). Its rider `533285ee` fixed vsync, which was silently inert
> at runtime on the Metal path — including the timedemo force-off, so with vsync
> archived on every Metal benchmark would have capped at the display refresh
> (verified with the pre-fix binary as the failing control: 59.2 fps pinned vs 114.1
> free). Acceptance: cvar 0 byte-identical to the pre-slice binary (4/4 boots, both
> backends); the cvar arm reproduces the SYNC env arm's frames to the byte count on
> the demo11 dumps; flip-safety under API validation with the termprobe stable across
> four flips; `PARITY_EXTRA='rt_metal_sameframe 1'` passes (rt_spawn via the
> rerun-once rule — one odd Metal boot with the cross-equals-control tell — and its
> image legitimately changes: the FORCENEAREST adoption arm's first coverage).
> **Post-ship incident, same night, closed:** Seb's first hour-long session on this
> build ended in a 92.16 GB out-of-application-memory halt — the game process measured
> FLAT (~735 MB, four beds); the eater was Xcode's debug console retaining our own
> diagnostic chatter at combat rates (his config archives `developer 1`). Fixed
> `ccb4b5d5`: chatter behind `RT_METAL_VERBOSE=1`, the 120-frame profile line — 8-1c's
> own regression, the one high-rate print no config could disable — behind
> `RT_METAL_PROFILE=1` (the bench harness and rt-suite set it themselves), QC dprints
> reshaped to first-event, the trace-error print rate-limited. CLAUDE.md carries the
> hard-won fact.
>
> **8-4 is DONE (2026-08-08, late night, one commit; 8-3 stays deferred, so 8-5 is
> next).** MetalFX scaffolding, no cvar: `metal_fx.m`/`.h` in OBJ_METAL_MACOS with the
> explicit ARC make rule and the four pbxproj edits; `-framework MetalFX` on the
> makefile client link and both app-target OTHER_LDFLAGS blocks, never the SV line; the
> metal_backend.h stub pattern — and gl_rmain.c #includes the header TODAY with zero
> call sites, so the #else stub arm compiles under `make sv-release` from day one
> rather than first being exercised by 8-5's caller (the class that has broken the
> server link twice). The probe: `supportsDevice:` + the nil-factory guard, one boot
> line with the queried bits, probe scaler released the moment its two usage words are
> read. **Measured on the M5: `colorTextureUsage` 0x1 (ShaderRead — covered by the
> pooled render-target usage, checked at runtime once per start) and
> `outputTextureUsage` 0x5 (RenderTarget|ShaderRead) — exactly the pair `mb_screentex`
> has carried since Phase 4a.** So on this device/OS the unconditional descriptor OR
> composes to a bit-identical descriptor, the byte gate could only ever pass, and the
> acceptance's teeth are the fail-first checks: a texture deliberately created without
> a required bit fires the creation assert (usage 0x4 against required 0x5), and a
> poisoned colour-usage word fires the pool-coverage warning (0x3). The byte gate ran
> anyway — binary A/B against a HEAD control, spawn + fbo2, both backends —
> **byte-identical, 0 of 307200 px in all four comparisons**, every same-binary control
> clean at zero first (one dirty fbo2 GL control en route was the documented odd-boot
> bed class, draws 532 vs 132 by the counters rule, clean on rerun). Smoke is **101
> checks** (+1: run K2 asserts the MetalFX probe REPORTED, a pattern matching both the
> available and not-supported wordings). Deployment: the pbxproj pins 26.0; make sets
> none and inherits the host SDK — either way far above MetalFX's 13.0 floor, no weak
> linking. Gates: cmdtrace 1645 `66c209c4e0e81cab` three runs at HEAD and three on the
> finished tree, all six on the mode; sv-release links; Xcode Release green.
>
> **8-5 is DONE (2026-08-08, late night, one commit): `r_metalfx` lands with its
> behaviour.** The chosen shape (b) exactly: under the gate the postprocess writes a
> render-res pooled intermediate (the R_Bloom_MakeTexture Get shape), the cached
> spatial scaler upscales it into `mb_screentex` on the frame's own command buffer,
> and the HUD draws over fbo 0 at native res through the standing Load reopen. One
> new backend entry (`Metal_Backend_SpatialUpscaleToScreen`), a one-slot keyed
> scaler cache with per-key failure memoisation in metal_fx.m, and the gate helper
> `R_MetalFX_GetPostprocessTarget` beside `R_BlendView_IsTrivial`. **Acceptance,
> all five instruments:** (i) sharpness, by the new committed `test/sharpness.py`
> (no acutance instrument existed in the tree at all): at 0.667 bilinear keeps
> 62–70% of the native frame's gradient energy (44–46% Laplacian) and **MetalFX
> keeps 85–106% — essentially native** — with tone within 0.5% of native (closer
> than bilinear) and the crops unmistakable by eye; (ii) bench, interleaved
> fullscreen demo11 at Seb's config: native 81–106 fps against **MetalFX@0.667 at
> 140–154**, and bilinear@0.667 144.4/143.8/145.3 against MetalFX 142.5/140.3/140.0
> — **the scaler costs ≈0.2 ms/frame against free bilinear** and returns most of
> the sharpness; (iii) EDR: the probe reports **5373 px above SDR white at max 12.7
> through the scaler** (HDR mode, RGBA16F), screentex and drawable in exact
> agreement — with the honest note that at the absurd-value bed (scenebrightness 6)
> the HDR scaler's edge reconstruction overshoots peaks relative to bilinear (max
> 12.7 vs 5.7); (iv) smoke J5 runs the scaler under API validation across BOTH
> non-EDR keys (a mid-run `r_viewfbo 2` re-key) with the encode-refusal strings in
> its absent pattern, clean; (v) `r_metalfx 0` byte-identity: 0 of 307200 px in all
> eight comparisons (controls + vs the 8-4 HEAD frames, both backends, spawn+fbo2).
> (vi) is moot — libraries are per-session artefacts and the defaults are
> byte-identical. **The frozen bed stays deterministic with the scaler live**
> (all controls 0 of 307200), which is itself a finding: the scaler is
> deterministic. **The four-lens adversarial review found four real defects before
> any gate could** — anaglyph stereo passed the gate twice per frame and the
> second eye's unmasked upscale wiped the first (fixed: `R_Stereo_Active()`
> refusal); the upscale's return was discarded, turning every encode-time refusal
> into a permanently frozen scene (fixed: re-run R_BlendView against the real
> destination — fail-first proven, a build whose every encode refuses renders
> byte-identical to the bilinear arm); the per-key scaler's outputTextureUsage was
> never checked against what the boot probe had baked into mb_screentex (fixed:
> checked at mint, so an escalated requirement is a gate refusal, not a frozen
> frame); and the EDR-off edge frame paired 8-bit input with HDR processing
> (fixed: HDR mode now requires a float input). Plus `r_rendertarget_debug` is
> refused under the gate — the debug view could have selected the pool destination
> as its own source. Steady-state proven unchanged by the fixes: the fixed
> binary's scaled frame is byte-identical to the pre-fix stills, so every
> acceptance number above carries to the shipped binary. Smoke **108**; menu row
> waits for 8-6 QA per the plan. Gates: cmdtrace 1645 `66c209c4e0e81cab` ×3;
> sv-release links; Xcode Release green.
>
> **8-6 is DONE (2026-08-09 morning, one commit, on Seb's instruction to proceed):
> the menu rows land; DEFAULTS STAY OFF** — he has not yet reported the by-eye pass,
> so the plan's default position holds and any default flip is a follow-up decision,
> not this commit. Three rows: **Render Scale** (`r_viewscale` as a preset cycle,
> 100/75/67/50%) and **MetalFX Upscale** on the Video page before Apply (VIDEO_ITEMS
> 13→15, the Renderer/HDR shape — live-applied, ESC keeps them, the r_edr contract),
> and **Same-Frame RT** on the RT Shadows page after Light Cull Distance
> (OPTIONS_RTSHADOWS_ITEMS 16→17, both lockstep functions). The MetalFX row is
> greyed unless the SELECTED renderer is Metal AND Render Scale is below 100% — at
> 100% a live checkbox would be the dead-slider class. **The Video page gained the
> Options-family row-count self-check it never had** (it predates the convention),
> and smoke run G now opens `menu_video` too, so the page is under the same net as
> the others — the check verified to FAIL on a deliberate VIDEO_ITEMS bump ("Video
> rows (15) != VIDEO_ITEMS (16)") and silent on the true counts. Both pages
> verified by screenshot: greying flips with `r_viewscale`, Same-Frame RT sits in
> the RT block, cursor sane on all rows. Docs in the same commit: SETTINGS.md (menu
> locations on all three cvar rows + the MetalFX greying in the honest-controls
> list), GUIDE.html (Performance and Ray tracing sections name the rows).
> **Phase 8 is now complete except 8-3, which stays deferred by Seb's decision.**
> Gates: cmdtrace 1645 `66c209c4e0e81cab` ×3 floored at session start and ×3 on the
> finished tree; smoke 108 clean; sv-release links; Xcode Release green.
> **The 8-6 defaults flip landed 2026-08-09 after Seb's by-eye pass: `vid_vsync`
> and `rt_metal_sameframe` both default ON** (his verdict, given with the leak QA).
> Parity pins `rt_metal_sameframe 0` + `vid_vsync 0` in the shared preamble and
> cmdtrace pins `+vid_vsync 0`, so every recorded baseline stays valid — proven by
> spawn byte-identity against the pre-flip frames. `r_metalfx` stays default 0
> (not part of his verdict). His e1m1 QA also reported three visuals, triaged the
> same day on his own recorded demo12 frames — mechanisms and proposed fixes in
> CLAUDE.md's 2026-08-09 midday record; the sky one exposes a standing coverage
> hole (no vantage anywhere frames murk + sky together).
>
> **8-7 — THE MetalFX-TEMPORAL ARC IS OPEN (2026-08-18), and the phase heading above
> was widened for it.** `r_metalfx` becomes a three-state cvar (0 bilinear, 1 spatial,
> **2 temporal**); default stays 0 and the spatial path is byte-identical, gated on
> parity (`spawn` 0.0005 / `rt_wall` 0.0006, all four two-boot controls clean at 0.0000)
> and cmdtrace 1645 x3. Five commits: `926cbff7` (the `test/roll.py` coherence
> instrument + `test/rollbed.sh`), `09c98676` (the scaler, motion vectors, jitter),
> `0dfcfd30`, `5c49fde8` (the silent fallbacks made to speak + the fullscreen geometry
> trap), `64ac7f77` (the viewmodel's own motion vectors).
>
> **Four things the DEVICE and the BED settled, none of them derivable by reading:**
> the temporal scaler's OUTPUT wants ShaderWrite (usage 0x7) where spatial wanted 0x5,
> so `MetalFX_OutputTextureUsage()` must return the UNION of both — `mb_screentex`'s
> recreate predicate keys only on size and format, so a mid-session switch would
> otherwise meet a texture the scaler refuses and no recreate would fix it; the device
> reports an input-content scale range of **1.000-3.000**, so 1:1 is legal and the
> gate's upscale-only refusal is relaxed for temporal alone; the jitterOffset sign is
> **per-axis** (x negated, y not — the GL-layout v-flip), measured by pinning a constant
> jitter and cross-correlating, because the correct sign CANCELS the raster shift and a
> wrong one DOUBLES it; and the motion vectors are correct (exactly zero on a frozen
> camera, by `r_metalfx_debugview`).
>
> **The verdict is honest and mixed: it ACCUMULATES but does not RECONSTRUCT.** Twinkle
> falls 4-5x and the grid halves — the spatial scaler had been sharpening the mesh, its
> grid reading 129% of native's — but sharpness sits at bilinear's rather than native's.
> **The 8-bit input was the limiter**: with a float scene buffer (`r_viewfbo 2`, which
> `r_edr` forces, so Seb's own path has it) temporal at 0.667 reaches 93% of native
> gradient energy. The twinkle fix is SCALE-INDEPENDENT — the same at 0.667 as at 1.00 —
> so 0.667 is the recommendation at ~15% frame cost, and raising the scale buys detail
> rather than less mesh. **Decision point 3 below is therefore RE-OPENED**: (b)
> postprocess-at-render-res is what shipped and it is what the scaler is fed, and (a)
> upscale-then-postprocess is now the named next experiment rather than a fallback.
>
> **Known gap, measured rather than assumed:** motion vectors are camera-only apart from
> the view weapon, so a fast-moving object carries **19-29% of its previous frame** (a
> static-camera rocket bed). Per-entity vectors are designed and unbuilt. The weapon
> itself is handled — `r_metalfx_viewmodel`, off the short depth range, cutting its
> frame-to-frame crawl 20%.
>
> **8-7 continued (2026-08-18 night / 2026-08-19) — the gap closed in three layers, and
> the measurement turned over twice.** (1) **Per-entity rigid-body motion vectors**
> (`r_metalfx_entities`, T2b, `a8306ff5`): `SHADERMODE_MOTIONVECTOR` over the fill
> through the depth-only draw path, previous matrices in a side table keyed on
> `entitynumber`, by-hand depth test — and it found the "rocket ghost" was never the
> rocket (126 px of model; 10,400 px of change, 78% of it the SMOKE TRAIL). (2) **The
> reactive mask from the dynamic-light footprint** (`r_metalfx_reactive`, T3r,
> `02838a0a`): the fill shader's reactive arm writes `(1-d/r)^2` summed over the first 8
> dlights into a BGRA8 pooled target handed to the scaler as its
> `reactiveMaskTexture`. (3) **The particles' own footprint** (`r_metalfx_reactive_particles`,
> 2026-08-19): the particle callback appends the batch it just drew — world-space quads,
> premultiplied colour, texcoords, texture — to a `taa_frame`-stamped stash, and the mask
> pass replays it with `SHADERMODE_REACTIVESTAMP` (additive, depth by `read()` like the
> M5 bolt, weight = strength × texture shape × gain so a puff marks a soft disc, not a
> square), captured only around the MAIN `R_RenderScene` (the water sub-scenes run first
> with `ismain` still true). **What `test/ghost.py` then said, with the scaler's own
> reset-every-frame as the yardstick:** a white mask everywhere reads within 0.004 of
> reset (MetalFX honours 1.0 = "ignore history"); the smoke's genuine lingering is ~0.06
> of the previous frame with no mask and ~0.015 under either footprint; the ~0.26 the
> metric still reads on smoke is the temporal scaler rendering small bright puffs ~7%
> softer than bilinear with NO history at all — a look, not a ghost; and on a rocket the
> light footprint already covers the fresh smoke (it lives inside the rocket's own light),
> so the stamp's own reach is the lightless particles. The trail stamp (N−1) measured
> nothing and ships off; the gain sweep was flat. **Temporal now forces the float scene
> buffer** the way `r_edr` does (the 8-bit input was the T1 limiter). **And the all-live
> validation boot found S1's `rt_fogfilter` bound 48 bytes to a 64-byte struct** (MSL
> `float3` is 16-aligned): the 5×5 mode had never run — re-measured genuinely running,
> 3×3 0.00418 vs 5×5 0.00415 weave against 0.00571 off, so S1's Nyquist argument holds
> on evidence. Smoke run Q now boots the fog kernel, toggles all three filter modes, and
> asserts the mask and the stamp engaged.
>
> **Post-ship cost note:** a fresh-booted machine widened the single-pair cost to
> −23%..−31%, and a post-reboot-indexing machine narrowed it to 0% — consistent with
> the mechanism (the cost is lost GPU concurrency, so it scales with how much
> concurrency there is to lose) and bracketing today's honest answer at **0–31% by
> machine state, 12–15% on the stable interleaved bed**. The definitive figure is the
> fullscreen block on a settled machine — Seb's play conditions — via
> `BENCH_B='rt_metal_sameframe 1' BENCH_FULLSCREEN=1 sh test/bench-rt.sh`.
>
> **THE PLAN'S OWN COST PRIOR WAS WRONG IN THE INTERESTING DIRECTION.** The first thing
> the newly-seamed profile line showed: async `trace cpu` is NOT ~0 at Seb's config on
> demo11 — it is **14.5–27 ms**, because the GPU does not keep up with the sidecar
> (~150 view-culled lights on e3m1, fog 24 steps, `rt_metal_scale 0.75`). The shown
> slot's command buffer runs **27–50 ms of GPU wall time in async** (stretched ~2× by
> contention with the concurrently-executing other slot and the renderer) against
> **15.5–31 ms in sync** (exclusive, `trace cpu ≈ trace gpu` — in sync mode the profile
> line is a live meter of the sidecar's whole exclusive GPU cost). So the same-frame
> question was never "a new stall vs none"; it is "serial vs pipelined on a GPU the
> sidecar already saturates".
>
> **The measured cost of `RT_METAL_SYNC=1`: 12–15% of fps, consistently, interleaved on
> demo11** (windowed 1080 borderless, Seb's config, `test/bench-rt.sh`): 68.9→58.4,
> 49.0→41.9, 38.3→33.6 fps by adjacent pair; 1-sec minimums 56→45, 39→32, 27→28. The
> absolute level HALVED across the 11-minute run (warm-up 73 → last pair 38/34; no pmset
> thermal record; load flat) — the interleave is why the number survives that: the ratio
> held within 3 points across a 2× level swing. Absolutes from this bed are not
> quotable; the delta is. **Consequence for 8-3**: the early-kick split cannot buy much
> here — `as cpu` is 0.04 ms, the CPU encode is nothing, and the GPU is the bottleneck;
> the 12–15% is lost GPU *concurrency*, which no CPU-side reordering restores. The
> honest compensating lever for a player who wants same-frame correctness is sidecar
> GPU cost (`rt_metal_scale`, `rt_metal_fog_steps`/`_stride`), not 8-3.
>
> **The KERNELMS per-stage split proves the contention mechanism directly**: the SAME
> surface-trace stage measures **19.6–22.8 ms wall in async against 7.1–9.7 ms in sync**
> (fog 5–8 ms in both) — a 2.5–3× stretch from executing beside the other slot and the
> renderer, exactly the wall-time-vs-throughput trade the 2-slot design buys. Exclusive
> sidecar cost at Seb's config ≈ 13–17 ms/frame; the async pipeline hides it behind
> concurrency, sync pays it serially. **One bed trap for the record**: a windowed-
> borderless bench window that loses frontmost status gets presentation-throttled by
> the window server — one sync KERNELMS boot read "14.9 fps" total while its own
> one-second average was 62 (min 0) — 225 wall seconds of drawable starvation, not
> renderer cost. Windowed absolutes can be poisoned silently; the harness's
> `BENCH_FULLSCREEN=1` block is the quotable one, and any windowed run whose total fps
> disagrees hard with its one-second average was throttled, not slow.
>
> **The fringing evidence, quantified and imaged** (`RT_METAL_DUMP` stills at demo11
> f1450/f2800, sync vs async, same harness run): at f1450 (close riveted wall, camera
> tracking past) the mode moves **56.3% of the wall-corner crop's bytes against a boot
> noise of EXACTLY 0 of 600000** in the same crop, falling to max-delta 4 in the far
> corridor — the parallax signature, brightest at the closest geometry. At f2800 the two
> grunts appear fully silhouetted in the diff map (under wall lighting the RT term IS
> their lighting, so one frame of age misregisters the whole monster), whole-frame mode
> mean 13.75 vs boot-noise mean 0.36 (38×). The diff maps are the by-eye evidence; the
> in-motion look remains Seb's QA.
>
> **demo11 as a bed, corrected en route — and the correction is the 5-5-3 lesson
> arriving in a new coat: `cmp` has no magnitude resolution.** The first floor attempt
> read "three boots, all frames differ → does not floor". Wrong: measured with
> magnitudes, majority-state boots reproduce to **0.4–0.7% of bytes, localised**
> (entity-divergence patches; even across sessions), and today's seven boots contained
> exactly ONE odd boot showing an **86% global wash at mean 8.7** — the documented
> odd-Metal-boot class. The gating rule for demo11 dumps: same-mode boots agree ≤0.7%
> with localised block maps; an odd boot announces itself with a >50% global wash and
> is discarded on rerun. (The two-timeline library demo5 needs was tested for and
> refuted here — the odd boots do NOT match each other.)
>
> Also found and fixed en route: the RT profile line (and the KERNELMS report under it)
> lived in the GL bridge tail, below the seam, so the Metal renderpath — the shipped
> default — had no profile line at all until `54440de8`.

#### What the survey established (each fact reshapes a slice)

1. **A naive `rt_metal_sameframe` buys ZERO overlap.** The trace for frame N is kicked
   *late* — inside `R_RenderScene`, after the whole opaque pass (`gl_rmain.c:8420-8424` →
   `RT_SceneComposite`, `cl_screen.c:2203` → encode+commit at `rt_metal.m:2685/:2911`), and
   sync mode's `waitUntilCompleted` (`rt_metal.m:2954-2955`) stalls the CPU at a point where
   nothing of frame N has been submitted — the backend commits its one command buffer per
   frame at end-of-frame (`metal_backend.m:2235-2237`). The trace overlaps only the previous
   frame's still-executing GPU work. Real overlap needs an early-kick/late-show split (8-3).

2. **The cost measurement needs no new code.** `RT_METAL_SYNC=1` is live on the Metal path
   today (`s_syncMode`: declared `rt_metal.m:1018`, read from env once at init `:1328`, one
   functional read at `:2930`). The Phase 7 lesson — schedule the experiment before the
   code — applies directly: 8-1 measures, then Seb decides the budget.

3. **There is no `r_viewscale` blit to replace.** The upscale is an emergent property of one
   draw: the scene renders into a *smaller* `rt_screen` (`gl_rmain.c:5866-5873`, `:5918`)
   and `R_BlendView`'s single `SHADERMODE_POSTPROCESS` draw (`:7743`) samples it
   full-texture onto full-size fbo 0 through a LINEAR sampler — fused with gamma, shoulder,
   FXAA, fringe, saturation, viewblend and the bloom composite. "Replace the blit" means:
   move where postprocess writes, then insert a scaler encode.

4. **Same-frame does NOT fix fog temporal artefacts, and must not pretend to.** All three
   temporal EMA chains are mode-blind — they key on the *encode* slot
   (`s_fogmtex[curslot^1]` at `rt_metal.m:2826`, `s_shmtex[curslot^1]` at `:2885`,
   `s_histTex[s_histParity]` at `:2692`) and the previous *encode* camera, never the shown
   slot. What same-frame fixes is the shown term's age: the composite fringing at
   silhouettes — and, **with `r_transparent 1` only**, the RT-liquids arm, which samples the
   term screen-locked with no remap (`shader_glsl.h:2277`, `shader_msl.h:1444-1447`) and is
   one frame stale today. With `r_transparent 0` liquids draw inline *before* the composite
   and would read the PREVIOUS frame's slot while walls get the fresh one — a new seam, on
   the QA list, not a bonus.

5. **The murk cannot get a different slot age than the walls.** Every shown-slot consumer
   keys on one variable (`show` → `s_lastShown`, `rt_metal.m:2930/:2981`; accessor gates at
   `:1711/:1749/:1824/:2294/:2361`). The slot moves atomically; only the camera/reprojection
   consumers need per-site decisions, and there are exactly three (`gl_rmain.c:7281` murk,
   `cl_screen.c:2273` Metal composite, `rt_metal.m:3105` GL bridge composite).

Also found and verified en route:

- **`r_edr_probe` never presents its frame** — `mb_extendedstats` commits and nils `mb_cb`
  (`metal_backend.m:1998`) before the fall-through present at `:2235` messages nil, against
  its own "this one MUST present" comment (`:2171-2173`). Pre-existing, latent.
- **Two genuinely stale queue comments**: `vid_metal.h:41-45` ("one queue for the whole
  renderer … rather than needing a cross-queue event") and `METAL.md:155-157` ("shares the
  renderer's device and queue"). `rt_metal.h:25-36` and `rt_metal.m:1274-1279` describe the
  API accurately and are NOT stale — leave them alone.
- **The Phase 8 flip bench method did not survive** — no script, cfg or backend attribution
  exists on disk; `benchmark.log`'s three demo11 lines are all from the Xcode **Debug**
  product and don't contain the headline numbers. `test/run-regime.sh:78` references a
  `test/bench.cfg` that does not exist.
- **demo11 is confirmed e3m1** (`maps/e3m1.bsp` in the dem header). Nothing anywhere records
  demo11 as byte-deterministic — the two-timeline library recipe was established on demo5.
- **Nothing asserts on the profile line's `trace cpu`** (rt-suite.py only logs it to CSV),
  and its doc comment already defines the sync-mode meaning ("the full trace wait",
  `rt_metal.m:3186-3188`).
- **Machine state 2026-08-08 pm**: load 2.6-3.1, a 1.9 GB Virtualization VM resident, and
  Seb's own log shows identical demo11 configs at 58.3 vs 76.7 fps — 32% spread. Absolute
  fps is unquotable today; interleaved deltas and `RT_METAL_KERNELMS` are the instruments.

---

#### Slices

Ordering rule: instrument before feature, measurement before design commitment, everything
default-off until Seb's QA. Gates on every commit (foot of this file).

#### 8-1 — Instruments: Metal frame dump, committed bench harness, the sync-cost measurement

**(a) `RT_METAL_DUMP` grows a Metal arm.** Hook: the `RENDERPATH_METAL` arm of `VID_Finish`
(`vid_sdl.c:2134-2137`), placed **before** `VID_Metal_Finish()` — symmetric with the GL
site, which dumps before `SDL_GL_SwapWindow` (`:2153-2157`); same `cls.timedemo` counting
flag, same playback-frames-only numbering, same raw file format
(`[int32 w][int32 h][RGBA8 bottom-up]`, `rt_metal.m:2257-2266`). Pixel source: **fbo 0's
colour explicitly** — a dedicated readback of `mb_screentex` (not `Metal_Backend_ReadPixels`,
which reads whatever `mb_targetcolor` currently is, `metal_backend.m:2401/:2421` — that
coupling would silently break the instrument the moment 8-5 points `R_BlendView` at an
intermediate). Convert BGRA→RGBA in the writer (`mb_readback_convert` emits BGRA for every
format, `metal_backend.m:2290-2333`; the GL dump writes `GL_RGBA`). Stated limits, in the
code comment: the Metal dump cannot see the present pass (that is `VID_METAL_PROBE`'s job),
and under `vid.edr_active` it clamps to 8-bit — identically for both arms of any A/B, so
still discriminating; stderr says so once. `rt_snapshot` stops being a silent no-op on
Metal for free.

*Verified fail-first*: a deliberately mis-indexed variant must move the dumped frame
number; a channel-swapped variant must fail a byte compare against the GL-arm convention.

**(b) A committed demo11 bench harness** — `test/bench-rt.sh`: sandbox userdir, copies
`demo11.dem` + a named config into `<sandbox>/m5/`, pins `vid_borderless 1`,
`cl_nettimesyncfactor 1` + `cl_nettimesyncboundmode 1`, runs interleaved A/B/A/B with the
per-arm cvar delta and `+vid_renderer` explicit on the command line (so the log's
`commandline` field finally records the backend), parses `benchmark.log` and prints the
paired table. Retires the "method did not survive" gap. A dump mode captures the fringing
probe frames (~1400-1500, ~2750-2900; exact frames picked by eye from a first pass).

**(c) The measurements, before any feature code — in this order:**
1. **Floor the instrument first** (the standing rule): same-binary two-boot dump control on
   demo11 at the probe frames, Metal path, before anything is concluded from it. demo11 has
   NO recorded determinism story — demo5's two-timeline library may not transfer. Record how
   many stable timelines demo11 shows; if it does not floor, say so and pick probe frames
   that do (or fall back to demo5 + a by-eye demo11 pass).
2. `RT_METAL_SYNC=1` vs unset, interleaved on demo11, Metal, Seb's config (fullscreen block
   only when the machine has settled; windowed deltas meanwhile). Plus `RT_METAL_KERNELMS=1`
   per-stage GPU times and the profile line's `trace cpu` (under sync: the full-trace wait,
   already documented so at `rt_metal.m:3186-3188`). Discard the first frames after any
   scale change — `traceCpuMs` reads 0 on resize frames (`:2628/:2956`).
3. Dump stills at the fringing frames, sync on vs off — the evidence pair 8-2's acceptance
   is judged against.

Priors, so the numbers have context: trace 2.2-2.3 ms GPU at 1080p·scale-0.5 (recorded);
Seb runs `rt_metal_scale 0.75` ≈ 2.25× the pixels → ~5 ms, plus fog ~2.5-3 ms. Worst-case
naive stall plausibly 5-8 ms against a 15.6 ms frame; possibly far less in practice since
it overlaps frame N-1's GPU work. Measure, don't assume.

**These KERNELMS numbers are also the evidence the 5-1 queue decision named as its revisit
condition** ("Revisit only with RT_METAL_KERNELMS=1 numbers", `vid_sdl.c:2006-2016`). They
go to Seb alongside the sync cost, with a recommendation either way — the queue question
gets decided on the table, not silently inherited.

**(d) Housekeeping, separate commit**: fix the `r_edr_probe` non-present (encode the
present before `mb_extendedstats` steals the buffer, or stop it stealing); correct
`vid_metal.h:41-45`; correct `METAL.md:155-157` in the house style for superseded text
(strike-through plus annotation, the `:158-160` precedent). Verified by the probe still
reporting both surfaces and smoke staying green.

**Decision point for Seb after 8-1(c): the cost budget** — how many fps is the fringing fix
worth, and does the KERNELMS evidence reopen the queue decision? The number decides whether
8-2 ships alone or 8-3 follows immediately.

#### 8-2 — `rt_metal_sameframe`: the cvar, simple shape

`rt_metal_sameframe` (default **0**, archived; menu row deferred to QA). Pushed per frame
through the established setter pattern (`RT_Metal_SetTuning` / `RT_Metal_SetReprojectDepth`
shape, `rt_metal.m:1849-1876`) — the env var is read once at init, so a cvar must be
re-read per frame. `RT_METAL_SYNC=1` keeps its exact current meaning (slot order only, the
historical verification instrument); SETTINGS.md's entry gains one sentence saying the
shipped cvar supersedes it and additionally clamps the reprojection identity. Prior
SYNC-based measurements stay comparable.

The mode itself is the existing `show = curslot` arm. The new code is the safety around
making it a runtime switch:

- **Flip hazard (async→sync)**: the transition frame leaves `s_pendingCB[prevslot]`
  un-waited, and **the very next frame** the CPU rewrites that slot's buffers — violating
  the invariant at `rt_metal.m:1034-1037`. The setter calls `rt_wait_pending()` (`:1264`)
  on any mode change. It does **not** touch `s_lastShown` — the shown slot's surfaces are
  untouched by the drain (it only waits and nils CBs), so consumers stay coherent, and the
  `:1958/:2105` precedents both destroy surfaces, which this setter does not; forcing -1
  would convert a survivable frame into a one-frame RT dropout for nothing. (Design-adversary
  verified: `SetLights`/`SetEntities` run before the setter in the same frame and write the
  slot `s_par` points at, whose CB was already consumed — the transition frame itself is
  safe; the drain lands before the frame that would collide.)
- **Fallback story**: in sync mode the `!scb` re-show branch is unreachable on ordinary
  frames but reachable via mid-encode resize under KERNELMS, where today it would proceed
  unguarded on the current slot (safe only because `rt_wait_pending` already drained it —
  an unstated invariant). The shipped shape makes the fallback explicit: if the same-frame
  CB is missing or errored, re-show `prevslot` when coherent (`s_pendingCB[prevslot]==nil
  && s_slotCam[prevslot].valid && s_mtex[prevslot]`). Verified against the "never touch the
  OTHER slot" comment (`:2938-2939`): not violated — in sync mode prevslot's only writer
  finished last frame, and `rt_release_surface` zeroes `s_slotCam[].valid` on size changes
  so the fallback self-disables exactly where it must. This preserves the anti-white-frame
  mitigation async's re-show provides (`RT_Metal_Active()` — set only by
  `RT_Metal_MarkComposited` after a real draw — is what arms the fullbright forcing,
  `cl_screen.c:2331-2332`; a skipped composite in wall-lighting mode is one blinding frame).
- **Reprojection becomes a provable no-op, not a live identity.** Under same-frame the
  shown camera IS `s_cam`; the remap is an identity in algebra but not IEEE (tan
  pre-scale/pre-divide round trip), and `s_reproject != 0` forces the term sampler LINEAR
  even at full res. So: `RT_Metal_GetReprojection` and `RT_Metal_GetShownCamera` return 0
  under same-frame — **conditional on `!rt_reproj_test_mode()`, so `RT_METAL_REPROJ_TEST`
  stays a live instrument** (the perturbation blocks sit inside both accessors, `:2318` and
  `:2401`, and an unconditional early-out would silently kill the only moving-camera
  instrument the frozen beds have) — and the sampler predicate treats same-frame as
  `!s_reproject` at **all three** LOCKSTEP sites (`rt_metal.m:2129-2131`, `:3012-3013`,
  `:3082`), routed through one helper so a fourth site cannot drift. Both disabled paths
  are documented bit-for-bit reproductions (`rt_metal.h:220-224`; murk disabled uniform at
  `gl_rmain.c:7292`, shader gate `VolumetricReproj0.w > 0.5`). **The EMA gates
  (`fc.hasPrevCam`/`sc.hasPrevCam`, `rt_metal.m:2785/:2879`) are NOT touched** — fog/shaft
  history is still one encode-frame old under same-frame, and clamping `s_reproject` itself
  would smear the fog on turns. Two gates, split on purpose, with a comment saying why.
- **Doc/lockstep updates in the same commit**: `shader_glsl.h:677-679` ("traced one frame
  ago" — false under same-frame), the word "verification" at `rt_metal.m:2927-2929` (the
  both-modes description is otherwise already right), `rt_metal.h:111-113`, SETTINGS.md,
  **GUIDE.html** (new archived cvar = player-visible knob), METAL.md status, CLAUDE.md
  session record, and the stale `parity-4a.sh` `rt_spawn` comment (next bullet).

**Acceptance**:
- *Liveness first, parity second* (the 5-4/6-3b lesson): a frozen camera cannot see the
  difference — the instrument is the 8-1 dump pair at the demo11 fringing frames (moving
  camera), sameframe 1 vs 0: fringing at silhouettes visibly gone, crops in the session
  record; then Seb's by-eye QA on the same spots. QA list also carries: `r_transparent 0`
  water-vs-wall seam check, and a fast-turn fog check (the EMA smear must be unchanged).
- Cost: the interleaved bench delta, quoted with the machine-state caveat.
- No-op gate: `rt_metal_sameframe 0` byte-identical to the pre-slice binary (dump gate at
  shipped defaults, match-either-boot library as floored in 8-1(c); libraries are
  per-session and re-captured after any look-changing commit); the five `rt_*` parity
  vantages reproduce their recorded numbers; cmdtrace; smoke; sv-release; Xcode Release.
- With `PARITY_EXTRA='rt_metal_sameframe 1'` the rt_* vantages must PASS — recorded as an
  agreement check only, never as proof the mode does something. **Known and intended**: on
  `rt_spawn` (scale 1) the term fetch flips FORCELINEAR→FORCENEAREST — a different but
  agreeing image on both backends, and the first coverage the FORCENEAREST adoption arm has
  ever had; parity-4a.sh's comment claiming NEAREST today is stale (reproject 1 forces
  LINEAR) and gets fixed in this commit.

#### 8-3 — Overlap split (conditional on 8-1's number)

Split `RT_SceneComposite` in two. The kick half (input gather + encode + commit — including
`RT_SceneComposite`'s own early-outs, `cl_screen.c:2085-2092`) is called from **one** site:
`R_RenderView`, **between the `r_fb.rtcompositepending` arm at `gl_rmain.c:8214` and the
`R_RenderScene` call at `:8215`** — the only point that is both post-animcache
(`R_AnimCache_CacheVisibleEntities`, `:8149`) and post-arm, and that cannot fire per
water-reflection view (`R_RenderScene` has five call sites; `:5650/:5715/:5774` are the
water recursion). Gated on the same `r_fb.rtcompositepending` predicate. The show half
(wait + publish + composite draw) stays at `:8420-8424`, scene depth bound, before the
murk — the ordering rule untouched. Gated on `RENDERPATH_METAL && rt_metal_sameframe`, so
GL and async keep today's single-site shape byte-for-byte. The viewport is final before the
kick via `R_ResetViewRendering3D` at `:8131`.

The trace then overlaps the CPU's scene encode and frame N-1's GPU drain; the wait at the
show site collects the remainder. Known risk: the surface-ensure (`rt_metal.m:2635`) moves
to the kick half and can release surfaces; the show half already re-validates via
`s_slotCam[].valid && s_mtex[]`. Acceptance: the 8-2 instruments plus the bench delta 8-2
vs 8-3 (the point of the slice), and sameframe-0 byte-identity against 8-2's binary.

An MTLEvent GPU-side wait is recorded as rejected-unless-the-number-still-disappoints: the
renderer's cbuf commits at end-of-frame regardless, so the event only recovers CPU encode
time the early kick already recovers, at the cost of the first cross-queue sync primitive
in the tree.

#### 8-4 — MetalFX scaffolding (no cvar yet, one measured descriptor change)

- `metal_fx.m` / `metal_fx.h` in `OBJ_METAL_MACOS` (`makefile.inc:143`) with an **explicit
  make rule** copying the `metal_backend.o` filter-out + `-fobjc-arc` (`makefile.inc:593-595`
  shape) — GNU make's built-in `%.o: %.m` rule would otherwise silently compile without ARC;
  plus the four pbxproj edits (PBXBuildFile with `COMPILER_FLAGS = "-fobjc-arc"`,
  PBXFileReference, group, sources phase).
- `-framework MetalFX` in **three places**: `makefile.inc:305` (`LDFLAGS_MACOSXSDL` only —
  never the SV line), pbxproj `OTHER_LDFLAGS` Debug (~:999) and Release (~:1068). The two
  extra OTHER_LDFLAGS blocks are the test target and stay untouched.
- `metal_fx.h` follows the `metal_backend.h` pattern: `#ifdef USE_METAL_RENDERER`
  declarations, `#else` static-inline no-op stubs — mandatory, the caller is `gl_rmain.c`
  (OBJ_COMMON); `make sv-release` has broken on this class twice.
- Runtime: `supportsDevice:` + nil-factory guard; one boot line reporting availability and
  the **queried** `colorTextureUsage`/`outputTextureUsage` bits (the header makes the app
  responsible for satisfying them, `MTLFXSpatialScaler.h:118-139`; output must be private
  storage, `:151`).
- **The one behaviour change, measured on its own**: `mb_screentex`'s descriptor
  (`metal_backend.m:2074`) gains the scaler's queried output-usage bits **unconditionally**
  — the recreate predicate at `:2067` keys only on size and format, so a cvar-conditional
  usage bit would never re-mint on a mid-session flip. Creation asserts
  `(usage & outputTextureUsage) == outputTextureUsage`. Byte gate re-run for this change
  alone (expected inert; measured, not assumed). Same query check for the pooled
  intermediate against `colorTextureUsage` (`metal_textures.m:442` gives
  RenderTarget|ShaderRead — probably sufficient; verified at runtime).
- **No `r_metalfx` cvar in this slice** — a registered, archived cvar that cannot move a
  pixel is the dead-slider defect class CLAUDE.md records; the cvar lands in 8-5 with its
  behaviour. Deployment target: pbxproj pins 26.0; make sets none and inherits the host SDK
  (say that, not "parity") — either way far above MetalFX's 13.0 floor, no weak linking.
- Docs: METAL.md status + CLAUDE.md record. Gates: sv-release, smoke, cmdtrace, Xcode
  Release, parity subset for the descriptor change.

#### 8-5 — MetalFX integration: postprocess at render res, scaler to fbo 0

Chosen shape — **(b): postprocess writes a render-res pooled intermediate; the spatial
scaler upscales that into `mb_screentex`; HUD/console then draw over fbo 0 at native res.**

- `R_RenderView` (`gl_rmain.c:8224-8225`): under the gate below, `R_BlendView` is pointed at
  a pooled render-res colour target (the `R_RenderTarget_Get` shape the bloom chain uses;
  `TEXTYPE_COLORBUFFER` / `16F` following `R_Bloom_StartFrame`'s own textype decision, which
  matches `mb_screentex`'s non-EDR/EDR formats exactly) **with the render-res rect passed as
  its destination args**; then one new backend call performs the upscale.
- `R_BlendView`'s internals need no change: `PixelSize` (`gl_rmain.c:7717`) is already
  source-scale, so FXAA/fringe/shoulder/gamma all run at render res — the whole pixel
  saving. (`PixelToScreenTexCoord` at `:7723` is not declared by the POSTPROCESS permutation
  and is inert there — not part of the argument.)
- New backend entry `Metal_Backend_SpatialUpscaleToScreen(int srchandle)`:
  `mb_end_encoder()`, resolve the source via `Metal_Texture_GetTexture`, encode the cached
  scaler into `mb_cb`, next draw reopens fbo 0 with Load (the tested
  `metal_backend.m:1130-1145` shape; design-adversary verified: `DrawQ_Start` re-selects
  fbo 0 immediately after and nothing draws between). Destination is special-cased —
  fbo 0's texture is backend-private and `Metal_Texture_Adopt` refuses non-16F by design.
  Scaler cached in a small keyed cache (in/out dims + formats + mode), re-minted like
  `mb_screentex` (`:2067` precedent).
- Colour processing: **Perceptual** on the 8-bit path (the header's own description — sRGB
  perceptual input/output — is what post-gamma engine output is), **HDR** when
  `vid.edr_active` (RGBA16Float; "reversible tone mapping" per the header — measured, not
  assumed: `r_edr_probe` with the scaler on must still report above-white on the drawable,
  which is why 8-1(d)'s present fix matters).
- **The gate, complete** (design-adversary hardened):
  `RENDERPATH_METAL && r_metalfx && scaler-available && fbo == 0 && !r_refdef.envmap
  && x == 0 && y == 0 && width == vid.mode.width && height == vid.mode.height
  && inW < outW && inH < outH && !r_viewscale_fpsscaling.integer && vid.mode.samples == 0`.
  The `fbo == 0 && !envmap` terms are load-bearing: `envmap` renders with `ismain` true and
  x/y zero into an offscreen target (`cl_screen.c:1388-1420`) and would otherwise blast the
  live screen texture. `inW < outW` refuses `r_viewscale >= 1` (the scaler only upscales;
  1:1 would be a pointless copy). `r_viewscale_fpsscaling` moves the input size most frames
  — refused in v1 rather than re-minting a scaler in the frame loop. Any refusal falls back
  to today's fused bilinear path mid-session, gracefully, per frame.
- `r_metalfx` (default **0**, archived) registered HERE, with its behaviour. The scale knob
  stays `r_viewscale` — one knob for how much, one for how.
- Instrument coupling, stated: the 8-1 Metal dump reads `mb_screentex` explicitly, so it
  sees the SCALED output (post-upscale, pre-HUD readbacks unchanged since `mb_screentex` is
  still fbo 0 at readback time) — the dump remains valid under this slice by construction.
- **Acceptance**: parity numbers do not apply (the image differs by design). Instruments:
  (i) lookmetrics + crops, three-way — native vs bilinear-at-0.667 vs MetalFX-at-0.667: the
  claim is "sharper than bilinear at equal cost, cheaper than native"; (ii) interleaved
  demo11 bench, windowed + fullscreen; (iii) EDR: `r_edr_probe` above-white survives the
  scaler; (iv) smoke J4 (`METAL_DEVICE_WRAPPER_TYPE=1`) with the scaler on — API validation
  is the net for the encoder-lifecycle seams; (v) `r_metalfx 0` byte-identity; (vi)
  gate-library recapture (look-changing commit).
- Docs: SETTINGS.md (`r_metalfx`, the trace-resolution compounding note — effective trace =
  window × `r_viewscale` × `rt_metal_scale`, documented rather than compensated),
  GUIDE.html, METAL.md status, CLAUDE.md record. Menu row (Video page, beside Renderer/HDR)
  after QA.

#### 8-6 — QA flip

After Seb's by-eye pass on both features: defaults stay off unless he says otherwise; menu
rows land with whatever he approves; GUIDE.html updated in the same commit as any default
change.

---

#### Decision points (plan approved as written 2026-08-08 — items 3-5 are thereby decided as recommended; items 1 and 2 stay OPEN until 8-1(c) produces the numbers)

1. **The same-frame cost budget** — 8-1(c) produces the number with zero code. How many fps
   is the fringing fix worth? Recommendation: if the naive stall costs more than ~2-3 fps at
   his config, 8-3 is in scope by default.
2. **The 5-1 queue decision's revisit condition is met by 8-1(c)'s KERNELMS numbers** — they
   get presented with a recommendation either way rather than silently spent.
3. **MetalFX shape** — **RE-OPENED 2026-08-18 (see 8-7)** — recommend (b) postprocess-at-render-res (whole saving, smallest
   shared-code diff, scaler in its designed perceptual regime). (a) upscale-then-postprocess
   is the fallback if (b) reads wrong by eye. (c) present-time upscale is rejected: it would
   upscale the HUD and break screenshot geometry.
4. **Bench harness commit** — recommend committing `test/bench-rt.sh`; the flip session's
   method provably did not survive.
5. **`r_edr_probe` present fix** — recommend fixing in 8-1(d); Phase 8's MetalFX EDR
   acceptance leans on that probe.

#### Gates, every commit

`./test/cmdtrace.sh check` — three runs minimum, floored at HEAD first, **and more until the
floor lands on the mode** (the recorded excursions worsen under exactly today's machine
load); `sh tests/smoke.sh`; `make sv-release -j8`; `xcodebuild -scheme QuakeM5
-configuration Release build`; the relevant parity vantages with same-binary controls read
first; dump-gate libraries captured at shipped defaults, re-captured after any look-changing
commit; every new probe verified to FAIL when deliberately broken; `touch builddate.c`
before any build whose version is quoted. British English; commits at slice boundaries; no
push/branch/tag/rebase; investigation subagents read-only; docs ride the behaviour commit.


## Pre-refactors on shipped GL code, each with its proof

| Refactor | Proof |
|---|---|
| 2× `qglEnable(GL_POLYGON_OFFSET_FILL)` → `GL_PolygonOffsetEnable()`; 1 init-time `glGet` → helper | command digest identical |
| Uniform calls → `R_Shader_Uniform*` (167 sites in `gl_rmain.c`) | **command-digest gate** |
| `r_glsl_permutation_t` locations behind accessors | mechanical; digest gate |
| `shader_glsl.h` → prologue + `shader_body.h` | **`r_glsl_dumpshader` byte-identity** per live permutation |
| `R_Viewport_Init*` gains the z01/y-flip variant | GL passes false, identical arithmetic; digest gate |
| Sidecar device injection | same device/queue under GL; demo5 byte gate (two-boot control first) |

**The command-digest gate** (built at Phase 1): debug-build thunks FNV-1a-hash
(call id, args) for the ~50 `gl_backend.h` entry points plus the 11 `qglUniform*` and
`qglGetUniformLocation`, printing a per-frame digest. Identical digest sequences on demo5 =
identical command stream — a total proof, independent of the pixel gate's documented blind
spots. ~150 lines.

## Milestones

> **Status, 2026-08-04: PHASE 0 IS DONE** (`ca5d79d8`, `c35dbd98`, `02bb36fa`).
> `RENDERPATH_METAL` + all 77 switch sites triaged (`METAL_TODO` marks the arms
> owing later work); `vid_renderer` + dispatch; `vid_metal.m` with the
> pipeline-state cache and a triangle **verified drawn by readback**
> (`VID_METAL_PROBE=1`); `RT_Metal_InitWithDevice` so Phase 5 is mostly
> deletion; `USE_RT_METAL` replaces `MACOSX` as the sidecar's feature guard and
> **`make sv-release` works for the first time**. Smoke 59/59 (6 Metal checks,
> both new assertions verified to FAIL when deliberately broken); Xcode green;
> GL byte-gated inside the two-boot control on demo5.
>
> **Two safety facts learned the hard way, after Seb got stuck in the preview:**
> `vid_renderer` is **deliberately not archived** *(true for the whole parity
> arc; INVERTED at Phase 8 when metal became the default — CF_ARCHIVE restored,
> because with a metal default "vid_renderer gl" must be able to persist or the
> escape hatch runs backwards)* — the path draws nothing,
> including the console, so a saved `metal` relaunches into a blank window with
> no visible way out. And the renderer has **two doors that are both
> load-bearing** (measured, not assumed): `CL_UpdateScreen`'s early-out and the
> guard at the top of `R_RenderView`. Remove either and the Metal path
> segfaults — every `qgl*` pointer is NULL there, because `GL_InitFunctions` is
> only ever called by `VID_InitModeGL`.
>
> **Status, 2026-08-04: PHASE 1 IS DONE** (`ba8dc0d2`). The command-digest gate
> exists as `test/cmdtrace.sh baseline|check` + `dpcmdtrace.{c,h}` +
> `dpcmdtrace_intercept.h`, and it is **validated, not merely written**: level 1
> is stable across boots and rebuilds, and a deliberate pixel-invisible extra
> backend call moves the digest and reverting returns it exactly.
>
> Three findings changed the design and are recorded in CLAUDE.md: a **demo is
> the wrong bed** (two boots agreed on 0 of 6124 demo5 frames — which also means
> demo5's "convergent frames" did not reproduce that day), **GL object names are
> not stable across processes** (so textures, buffers, FBOs, programs and uniform
> locations hash as first-seen serials), and **only structure is stable**, hence
> three levels where two were planned — level 1 is the gate.
>
> The parity bed was **measured rather than scripted**, per the session decision,
> and it corrected four assumptions in this file's own bed list — most
> importantly that `r_lerplightstyles 0` does NOT freeze torches. See CLAUDE.md.
> The six-vantage parity script stays deferred to Phase 3, and two hard limits
> now bound what it will be able to claim (no console reset for the RT jitter
> phase; neither capture path works under Metal until the Phase 4d readback).
>
> **Status, 2026-08-04: PHASE 2 IS DONE, with its scope changed on evidence**
> (`4e7c85bf`). The plan was to split the whole ~1850-line shader body. Measuring
> first killed that and Seb agreed the narrower shape: the body carries **521 raw
> `vec2/3/4` type names and 179 `uniform` declarations** MSL cannot accept, and
> **168 of those declarations sit outside the prologue with 88 physically
> interleaved between the shared `main()`'s six entry points** — so a
> prologue-declares-everything split is not possible against the file as written.
>
> What landed instead fixes a LIVE defect rather than a hypothetical one:
> `shader_density.h` is now the single definition of the murk's shaping constants
> and its max-channel Reinhard shoulder, spliced into both shaders. Those numbers
> were previously named `#define`s in the kernel against **bare literals** in the
> GL march — the two lockstep sites could not be diffed even in principle.
>
> The 95-line density model itself remains a hand-kept pair (16 uniforms whose
> names and packing differ per side). Sharing it needs a name-mapping layer and is
> the natural first move whenever the surface shader is ported.
>
> **Honest note on the proof**: byte-identity of the dumped shader text is *not*
> achievable for a genuine share and was not achieved — the CRC moves
> 36422 → 57874. The constants are substituted literals so the arithmetic is
> unchanged; the e1m4 A/B confirms it once the temporal EMAs are pinned (see
> CLAUDE.md — the unpinned reading looked like a regression and was not).
>
> **Phase 3 STARTED, 2026-08-04** (`ea8df0a0`) — the uniform indirection landed and is
> **proven inert by the command-digest gate**: 167 setter sites and 127 location lookups
> in `gl_rmain.c` now go through `R_Shader_Uniform*` / `R_Shader_GetUniformLocation`, and
> the backend call stream is byte-identical (1645 calls, `66c209c4e0e81cab`, before and
> after). Counts reconcile exactly with the independent Phase 1 survey. This is the proof
> the byte gate could not give, and the reason Phase 1 came first.
>
> **Phase 3 slice 2 landed** (`017a0164`): the Metal texture layer. `gl_textures.c` stays
> the manager on both backends (measured: only 23.6% of it is GL-specific, ~70% is
> backend-agnostic bookkeeping), with the Metal objects behind a plain-C header and
> `texnum` carrying an opaque handle so `R_GetTexture` and all 29 external field reads are
> untouched. Proven by `r_metal_textureprobe` — a known pattern through the real
> `R_LoadTexture2D` path, read back off the GPU, 0 of 1024 bytes differ.
>
> **Correction worth carrying**: texture upload is **EAGER**, not lazy. `R_SetupTexture`
> calls `R_UploadFullTexture` unconditionally at load time; the delayed-upload machinery
> is vestigial (`gltexture_t.inputtexels` is never assigned anywhere). Only UPDATES defer.
> And the 2D path needs exactly ONE textype — `TEXTYPE_BGRA` — because `Draw_NewPic` never
> reads its own textype argument.
>
> **Phase 3 slice 3 landed** (2026-08-05): `metal_backend.m` — the drawing machinery.
> All 39 of `gl_backend.c`'s identical `RENDERPATH_METAL` stubs now forward to it
> (state shadow, pipeline-state cache, lazy encoder, mesh buffers, both vertex
> layouts, `R_Mesh_Draw`), and the shipped GL renderer is **provably untouched**: the
> command digest is byte-identical at `1645 66c209c4e0e81cab`, and every surviving
> original line of `gl_backend.c` is an exact ordered subsequence of the new file.
>
> Proven by **`r_metal_drawprobe`**, not by an absence of errors: three quads driven
> through the real `gl_backend.h` entry points into a scratch target and read back —
> the constant-colour layout, the colour-array layout, and a blended overdraw — then
> the whole thing repeated with `r_metal_forceencoderrestart 1`. Result **0 of 16384
> bytes off, max delta 0**, and the restart pass byte-identical. Both halves were
> verified to FAIL when deliberately broken, and to fail *independently*: a blend that
> never reaches the pipeline key moves the first by exactly 4096 bytes (quad A's
> region, localised by the probe's own block map) and leaves the second green; a
> restarted pass that clears instead of loading moves the second and leaves the first
> green.
>
> Design notes that differ from this file's sketch, each on evidence:
> `mb_apply_state` applies **everything** every draw rather than tracking dirty bits —
> a few hundred 2D draws cannot notice, and it deletes the state-replay bug class this
> file's own risk register names as the largest. The constant-colour slot rides
> `setVertexBytes` (Metal's ≤4 KB inline path) rather than a ring allocation. Phase 0's
> private pipeline cache in `vid_metal.m` is left **untouched** and is deleted in slice
> 4 when the frame loop moves and the deletion is free — a second small cache for one
> slice was judged cheaper than putting the working preview at risk.
>
> **Status, 2026-08-05: PHASE 3 IS DONE** — slice 4 landed and the Metal path
> **carries a frame**. `shader_msl.h` (VERTEXCOLOR + ALPHAGENVERTEX, GENERIC, magenta
> sentinel), the Metal side of `R_Shader_Uniform*` (reflection → byte offsets → a
> staging block), the GL-layout screen texture with one v-flip present pass,
> `GL_ReadPixelsBGRA`'s Metal arm, and the `CL_UpdateScreen` door removed.
>
> **Measured, against a clean bed** (same-binary two-boot controls on both backends,
> run first and zero on every bed):
>
> | bed | GL vs Metal | verdict |
> |---|---|---|
> | console (conback + ~25 lines of font-atlas text + colours + input line) | **max Δ 0, 0 pixels differ** | **pixel-identical** |
> | main menu | 119 px (0.039%), max Δ 175 | the animated cursor — see below |
> | HUD | not established | bed defect, not a renderer result |
>
> The menu's 119 pixels are **proven** to be the blinking cursor rather than a
> rendering difference: GL compared against GL one blink-tick later differs by **120
> px at max Δ 175**, the same magnitude in the same 1/16 block. The renderer is not
> what moved.
>
> The HUD bed is recorded as **not established**, deliberately, rather than passed or
> failed. Every variant tried (equalised `vid_restart` schedules, `host_framerate`,
> `pause`, cheat-pinned health/ammo/armour) showed a correctly-rendered status bar on
> both backends carrying **different game state**. The mechanism is the loading
> screen: it advances `host.realtime` by a backend-dependent number of frames, so
> deferred commands land at different game times. The status bar draws through
> exactly the `DrawQ_Pic`/`DrawQ_String` path the console bed proves byte-exact, so
> the gap is in the instrument, not the evidence — but it is a gap, and Phase 4a
> should build a state-pinned bed before it needs one.
>
> **Two corrections to this file, both on measurement.** `R_RenderView`'s Phase 0
> guard **stays up** — the text above said remove both doors, but the CSQC
> `renderscene()` builtin has no guard of its own and relies entirely on it, so
> removing it drops any CSQC map into the unported 3D path. The 2D frame never
> reaches `R_RenderView` anyway. And the "**the split MUST land before any MSL
> exists**" rule is **scoped to the surface/fog shader**, agreed with Seb: its stated
> reason is the murk density model becoming a three-way lockstep, and a 2D-only MSL
> touches none of it. Six lines of the surface shader are duplicated, marked LOCKSTEP
> at both sites, and covered by a pixel gate the density model has never had.
>
> **Acceptance narrowed on measurement, with Seb's agreement.** This file used to ask
> Phase 3 for "GENERIC + FLATCOLOR + VERTEXCOLOR". Tracing the live 2D path shows only
> **`SHADERMODE_VERTEXCOLOR` + `USEALPHAGENVERTEX`** is reachable from it: every
> `DrawQ_Pic`/`_Fill`/`_String` marks its texture `MATERIALFLAG_VERTEXCOLOR`, which
> `R_UpdateTextureInfo` (`gl_rmain.c:8754`) punts to the lightmap path. **GENERIC** is
> still needed — the gamma-ramp and bloom blits use it, with its own separate
> `Texture_First`/`_Second`/`_GammaRamps` sampler namespace. **FLATCOLOR moves to Phase
> 4a**, where a surface batch can first reach it. Same measure-then-narrow move Phase 2
> made.

> **Status, 2026-08-05: PHASE 4a IS DONE — the opaque world renders on Metal,
> measured at effectively BYTE-PARITY.** On the frozen e1m3 bed (two vantages,
> every same-binary two-boot control clean at zero first): spawn **mean channel
> delta 0.0005, max 1**; hall **mean 0.0009, max 45, 15 px > 8 = 0.0049%**
> against budgets of mean ≤ 0.3 / p99.9 ≤ 4 / <0.5% > 8. Both backends run the
> same Apple GPU underneath, which is why near-byte-parity was achievable at
> all. The free counter check agrees exactly: world_leafs 49, draws 104,
> draws_vertices 1347, draws_elements 2445, batch_batches 24, identical across
> backends via the new `r_speeds_dump`.
>
> Landed across four commits (`ac68bb6b..`): the screen depth texture and the
> five projection remaps in the one generic z_row' = 0.5·z_row + 0.5·w_row form
> (the +0.5·w lands on m[10] for perspective, m[14] for ortho — opposite
> slots); `R_Viewport_TransformToScreen`'s inverse arm for the sky scissor;
> render-target discipline with pending-clear ownership; the FBO table; CPU mip
> chains (level-by-level `Image_MipReduce32`, never the GPU generator); the
> MSL LIGHTMAP/DEPTH_OR_SHADOW/minimal-POSTPROCESS arms with USEREDGLOW live in
> the shared body; the gamma-ramp build on Metal; `R_GLSL_Restart_f`'s own
> Metal arm (programs + PSO cache drop together, proven by live `r_redglow`
> toggles); and all three refusals lifted together.
>
> **The cull/winding question is settled empirically, not argued**:
> `MTLWindingClockwise` against the y-negated projection is correct, proven by
> a fail-first drawprobe pair (GL-front survives, reversed vanishes; flipping
> the constant fails the whole probe half exactly as GL semantics dictate).
> The probe caught two of its own construction mistakes on the way, which is
> the property that makes it an instrument.
>
> **Corrections to this file, on measurement**: the 4a acceptance said
> "LIGHTMAP + FLATCOLOR + DEPTH_OR_SHADOW" — FLATCOLOR is assigned NOWHERE in
> gl_rmain.c (one dead consumer); e1m3's opaque water and the sky sphere
> resolve to VERTEXCOLOR, already ported at Phase 3. Minimal POSTPROCESS came
> forward from 4d because viewblend (damage/underwater) reroutes the whole
> frame through it, and Seb's non-unit gamma makes every real-config frame
> non-trivial — with it came the gamma-ramp texture build (the dormant gap
> Phase 3 recorded). The scissored-clear trap this file warned about is
> handled by the drawn-quad clear and the no-FBO branch is additionally
> dormant in HEAD (`rt_screen` is never NULL). **USELAVA stays GL32-gated**:
> e1m7's lava is static on Metal until Phase 6 brings 3D textures — agreed
> with Seb, recorded as a look divergence.
>
> Entities are the MAGENTA SENTINEL until 4b — torch flames, monsters,
> pickups, the view weapon. The instruments: `test/tgacmp.py` (the committed
> comparator; both gate families + a block map) and `test/parity-4a.sh` (the
> frozen-scene bed: `sv_freezenonclients` BEFORE map load so the lightstyle
> phase is deterministic — deferred freezing left one boot in four a
> quantised lightstyle tick adrift, and `r_q1bsp_lightmap_updates_enabled 0`
> must NOT be pinned, it is the path that BUILDS the lightmaps at load).

> **Status, 2026-08-05: PHASE 4b IS DONE — entities and models render on Metal
> at the same effective byte-parity.** Monsters, torch flames, pickups, the
> view weapon, the colormapped player: `SHADERMODE_LIGHTDIRECTION` joined the
> shared MSL surface arm for the permutation shapes the bed can exercise —
> 0, GLOW, ALPHAKILL, ALPHAGENVERTEX, COLORMAPPING, DIFFUSE — through a
> derived define with a closed negative list (all 32 permutation bits are
> allocated, so it cannot rot); any other mode-11 shape still falls to the
> magenta sentinel, loud, never silently wrong. Exploration proved nothing
> else blocked entities — animcache, ring upload, vertex layouts, reflection
> all already ran — so the whole port is `shader_msl.h` plus instruments:
> **zero gl_rmain.c changes, zero metal_backend.m changes.** Four commits,
> `27b843da..`.
>
> Measured on the frozen e1m3 bed, every same-binary two-boot control zero
> FIRST, every cross-backend world gate PASS, and `r_speeds_dump` counters
> identical across backends on every entity vantage (both dumps):
>
> | vantage | mean | max | px>8 |
> |---|---|---|---|
> | ents_spawn (flame, pickups, GLOW live) | 0.0005 | 1 | 0 |
> | ogre (11 entities, ~12k entity triangles) | 0.0005 | 1 | 0 |
> | viewmodel (SHORTDEPTHRANGE → viewport z) | 0.0005 | 1 | 0 |
> | colormap (player model, perm 808) | 0.0004 | 1 | 0 |
> | ents_hall | 0.0009 | 45 | 15 (0.0049%) |
> | diffuse (whole frame mode 11 + DIFFUSE) | 0.0006 | 20 | 17 (0.0055%) |
>
> The hall-camera max-45 is the documented 4a world delta, not the entities'.
> Permutation coverage is proven by compile lines, never assumed: 0, 800
> (GLOW), 808 (GLOW|COLORMAPPING — whose zero controls also settled the
> player-pose determinism question empirically), 1 and 801 (DIFFUSE, on the
> `r_fullbright 1 + r_fullbright_directed 1` bed — the stock-cvar pair that
> turns the whole frame mode 11+DIFFUSE, which is what earned DIFFUSE a
> verified port instead of a sentinel; SPECULAR provably cannot fire on stock
> content and stays sentineled, with USECELSHADING beside it).
>
> **A real pre-existing v_flipped bug fell out of exploration**, fixed first
> and fail-first (`27b843da`): `GL_CullFace`'s Metal arm never wrote
> `gl_state.cullface`/`cullfaceenable`, and `GL_SetMirrorState` re-applies
> BOTH from that shadow at every v_flipped transition — on Metal it applied
> GL_NONE, culling silently dropped until the next per-batch re-set. Pixel-
> masked (the per-batch `GL_CullFace` covers it, and dropped culling on
> watertight geometry resolves identically under depth test): the new
> `flipped` vantage PASSed before the fix and after it, 0.0009 / max 45 —
> the hall numbers, mirrored. The fix restores the documented exact-mirror
> invariant; the handedness itself (projection x-negation composing with the
> Metal y-remap under the fixed clockwise winding) was proven correct.
>
> **Corrections to this file's 4b row, on measurement.** *Sprite* pixels
> defer to 4c: sprites resolve to LIGHTDIRECTION (not VERTEXCOLOR as the
> Phase 3 notes assumed — `RSurf_ActiveCustomEntity` nulls the lightmap
> texcoords, which is the mode-11 route), so the arm covers them by shape,
> but they draw from the transparent queue and the frozen e1m3 bed shows
> none. *Skeletal buffer* is satisfied by the written-down decision:
> `r_gpuskeletal` is permanently false on Metal (CPU skinning — the same
> correctness-preserving path GL takes at `r_glsl_skeletal 0`), USESKELETAL
> is structurally unreachable there, and Q1 `.mdl` is never skeletal.
>
> **Flagged for 4c/6:** the shared arm still accepts LIGHTMAP/VERTEXCOLOR
> permutations it does not implement — fog bits above all — so mg1's seven
> `_fog` maps render a silently-unfogged world on Metal rather than magenta,
> a 4a scope decision that violates the sentinel rule in spirit; the 4b
> negative list is the template for closing it. Mode-11 fog bits DO sentinel:
> a classic-fog map shows magenta entities on the Metal preview until the
> fog bits port.
>
> Bed facts earned this phase, recorded in parity-4a.sh: only the FIRST
> `angles`+`fixangle` of a boot applies on the frozen bed (one camera per
> boot); notify text never expires on a frozen clock and `con_notifytime 0`
> does not hide it (symmetric across backends, cancels in the diff);
> `r_cullentities_trace` must be 0 on every entity vantage (visibility rides
> `host.realtime`).

> **Status, 2026-08-06: PHASE 4c IS DONE — the effects layer renders on Metal
> at parity.** Sky, transparent water, particles, the thunderbolt, sprites,
> dlights and classic fog, across four commits (`9ad1f88d..`). The final
> seventeen-vantage sweep: every cross-backend world gate PASS, dump-1
> counters identical on all fourteen counter vantages. The effect vantages:
> sky 0.0001/max 1 · water 0.0691/max 54 (unfrozen bed, see below) · points
> 0.0005/1 · beam 0.0006/1 · beam_lit 0.0010/3 · flash 0.0015/40 · sprite
> 0.0006/1 · fog 0.0005/1 — with the 4a/4b vantages at their exact recorded
> numbers. Four transient dirty controls across the 68-boot sweep (the
> documented lightstyle wobble) all re-ran clean; controls-first ordering
> caught each before any cross number was read.
>
> **Most of the phase was proof, not code** — exploration established that
> particles and explosions ride MODE_GENERIC, beams VERTEXCOLOR+ALPHAGEN,
> sprites and the sky sphere mode 11, decals GENERIC + the already-plumbed
> encoder depth bias — all ported before 4c began. The still-life beds
> (4c-1) proved it with pixels and zero engine code: the frozen clock makes
> every effect a permanent freeze-frame (one lightning shot per boot, a
> muzzle flash that never decays, particles frozen mid-air), and
> `pointfile` doubles as a zero-randomness particle injector (the ±4096
> axis beams at its LAST point measured boot-unstable — the bed buries a
> sacrificial last point below the floor, where depth testing occludes
> them).
>
> The genuinely new code: **MODE_LIGHTSOURCE** (4c-2) — live by default via
> muzzle-flash rtlights, previously magenta — joined the shared arm behind
> its own negative list (DIFFUSE/COLORMAPPING/ALPHAGEN/ALPHAKILL ported;
> CUBEFILTER sentineled and Texture_Cube never declared, so the startup
> whitecube refusal stays log-only; REDGLOW deliberately mirrored
> UNATTENUATED per light pass because that is what GL does), and **classic
> fog** (4c-3) for the whole shared arm — closing the 4b flag: FogVertex
> inlined at its single application site, the FogMask index shift made
> explicit in the DP_TEX chain, fog bits out of mode 11's negative list.
> mg1's `_fog` maps and DotM entities now fog on Metal instead of
> rendering unfogged/magenta. LIGHTSOURCE fog stays sentineled (additive-
> pass fog is unexercised on any bed — honest refusal).
>
> **Corrections to this file, on measurement:** the 4a block above says the
> sky sphere resolves to VERTEXCOLOR — it resolves to **mode 11
> no-DIFFUSE** (R_DrawCustomSurface → FULLBRIGHT + entflags 0 → MODELLIGHT),
> and PVS analysis shows **no 4a/4b vantage ever had sky in frame** (spawn
> sees 0 sky faces; hall sits in solid leaf 0 → empty scissor → the sphere
> never drew), so the sky pixels above were all mask-fill. First actual sky
> parity is 4c's. **Coronas need no port**: OFF in a fresh userdir, and the
> Metal path already forces the traceline branch (usequery false) whose
> draw resolves to the 4b arm; no corona>0 light is producible on the
> frozen bed, so corona coverage is by shape, stated honestly. **Explosion
> shells** have no deterministic trigger (projectiles cannot move under the
> freeze; zero exploboxes on e1m3/e1m4/start) and the shell cvar defaults
> 0 — covered by shape (same GENERIC path as particles, which have pixels).
>
> **Divergences and observations, recorded:** GL runs ~25 draws of dlight
> stencil-shadow work per light that Metal structurally lacks — **Metal
> dlights cast no shadows** (pixel-invisible on the beds; the light
> vantages pin `r_shadow_realtime_dlight_shadows 0` so the counter
> instrument keeps its teeth). The r_speeds overlay frame's `draws*` vary
> with the digits printed (not parity material — the counter summary is
> dump-aware), and its `entities_triangles` wobbles ±2 per boot on either
> backend (dump-1 and pixels unaffected; not a gate). The polygon-offset
> METAL_TODOs at `R_ResetViewRendering2D/3D` are inert by design (GL's
> master enable has no Metal equivalent; bias is encoder state, applied
> every draw).
>
> **Bed facts earned this phase, the hard way:** the pre-load
> `sv_freezenonclients` **suppresses the r_wateralpha/r_wateralpha_force
> path outright** (opaque water with the cvars provably live and draw
> counters unchanged; late cvar-setting does not rescue it; mechanism
> unidentified, flagged as its own investigation) — and a DEFERRED freeze
> preserves the alpha but pins cl.time at a load-duration-dependent value,
> so Metal's slower restart pins one lightstyle tick apart from GL: a
> whole-frame ±1 wash that fails the cross gate as pure bed artefact. The
> water vantage therefore runs UNFROZEN with the frame made time-immune
> (r_fullbright + r_waterscroll 0). And the Metal restart+compile stall
> can compress the wall clock until several defers fire in ONE resumed
> cbuf burst — **a screenshot in the same burst as a state change captures
> the PREVIOUS frame**; the fog vantage pushes its shot/quit times out
> (writecfg's SHOT_T/QUIT_T) rather than trusting defer spacing. Requested
> pitch arrives divided by three (the player-angles convention — ask for
> 3×); on e1m4 angles never applied at all in three boots while origin
> provably did (unexplained, side-stepped; e1m3/e4m7 unaffected). e4m7 is
> the only id1 map with a light_globe (the sprite vantage's reason to
> exist).

> **Status, 2026-08-06: PHASE 4d IS DONE — the frame tail renders on Metal, and
> the offscreen scene path ran for the first time.** Five commits
> (`967c8997..162244ee`). Every acceptance item in the 4d row is measured:
> POSTPROCESS complete but for two deliberate refusals, the bloom chain and its
> composite, the gamma LUT, viewblend, FXAA, the HDR shoulder, screenshots, and
> `r_viewfbo` 0/1/2/3.
>
> **The phase turned out to be about COVERAGE, not features.**
> `R_BlendView_IsTrivial` returns true at fresh-userdir defaults, so `skipblend`
> was set on all seventeen prior vantages and the 3D scene rendered straight to
> fbo 0. `R_BlendView` had therefore **never run on Metal on any bed**, and the
> one render target the frame allocates (`rt_screen`) was allocated every frame
> and never rendered into. The minimal POSTPROCESS arm landed at 4a was entirely
> unmeasured. Three `r_viewfbo` vantages with zero engine code (4d-1) were the
> first frames to exercise either.
>
> | vantage | mean | max | covers |
> |---|---|---|---|
> | fbo1 | 0.0000 | 0 | 8-bit offscreen; the plain POSTPROCESS blit |
> | fbo2 | 0.0000 | 1 | 16F scene buffer + the HDR shoulder actually running |
> | fbo3 | 0.0000 | 0 | 32F format coverage |
> | bloom | 0.0000 | 0 | the whole chain + the USEBLOOM composite |
> | fxaa | 0.0000 | 1 | FXAA + colour fringe (the spatial static parms) |
> | tint | 0.0000 | 0 | viewblend + saturation + the gamma LUT |
>
> Both same-binary two-boot controls clean at zero first on every vantage, and
> dump-1 counters identical across backends. The full 23-vantage sweep (92
> boots) reproduced every inherited vantage at its recorded value.
>
> **Three vantages came back with dirty controls and none of them was the
> renderer**, which produced the harness's most useful new rule: the dump-1
> counters already captured discriminate the two cases for free. Identical
> counters between two boots means the same geometry was drawn twice and came
> out different — real. Differing counters mean the boots drew different
> visible sets, so the camera or the screenshot landed at a different moment —
> the bed. `sky`'s odd Metal boot reported draws 77 against the good boot's 53
> and re-ran clean; `viewmodel` the same. `flash` was a genuine bed defect
> found here: its impact puffs are boot-unstable on BOTH backends (the GL
> control alone was max 23 over 41 px), and with `cl_particles 0` its control
> goes to zero and its cross to 0.0008 / max 2 — so the muzzle-flash rtlight
> and the mode-12 pass are byte-clean and the 4c record's 0.0015 for it was a
> lucky run. Particles keep their own deterministic vantage in `points`.
> `water` never produces a zero control because it is the only unfrozen bed;
> recorded, not chased.
>
> **A CORRECTION to the 4a and 4b blocks above.** Both attribute the `hall`
> vantage's mean 0.0009 / max 45 / 15 px to Metal, calling it "the documented 4a
> world delta". It is not Metal. At that camera three of the four frames are
> **byte-identical** — Metal-direct, Metal-offscreen and GL-offscreen agree at 0
> of 307200 px — and **GL direct is the sole outlier**. The reason is structural:
> `Metal_Backend_SetRenderTarget`'s `fbo == 0` arm binds backend-created
> textures, so Metal has no default framebuffer and its "direct" path IS an
> offscreen render. GL's genuine window framebuffer, whose depth/stencil comes
> from the SDL context request rather than from the engine, is the only surface
> in the set that differs. The GL-side cause is not isolated and no Metal gate
> depends on it.
>
> **Two real defects, neither findable by a bed.** MODE_GENERIC hard-coded
> `Texture_GammaRamps` at `[[texture(1)]]`, which is right only alongside
> USEDIFFUSE — and the loading-screen progress bar draws GAMMARAMPS *without*
> DIFFUSE on every map load, putting the ramps on unit 0. Live in Seb's config
> (`v_gamma 0.5`), dormant on every vantage (fresh userdir → trivial gamma), and
> structurally unreachable by a screenshot because the loading screen is gone
> before one is taken. The instrument is therefore an assertion in the engine:
> `Metal_Backend_SetUniformInt` is the one place where the index the MSL declared
> and the unit the walk assigned meet, so a single check there covers all 37
> textures in every mode and permutation and cannot rot. It caught the defect on
> its first run. The second defect (4d-3a) is the bloom bit and the bloom
> attribute being derived from two different expressions that the
> `r_rendertarget_debug` branch decouples — harmless on GL, a
> pipeline-creation failure on Metal.
>
> **Scope decisions, both with Seb.** `USEPOSTPROCESSING` (`r_glsl_postprocess`
> and its uservec sobel/blur) is **sentineled rather than ported**: console-only,
> no menu row, and its own help says it is only useful with a customised
> `default.glsl`, which this fork never has. `USEVOLUMETRICDEBUG` sentinels
> because it needs a `sampler3D` and 3D textures are Phase 6. MODE_POSTPROCESS
> was the last arm in `shader_msl.h` with no negative list; MODE_GENERIC gained
> one for `USETRIPPY` at the same time. Both refusals verified to render magenta.
>
> **Honest limits, recorded rather than glossed.** `rt_screen->texcoord2f` and
> `rt_bloom->texcoord2f` are numerically identical by construction, so no pixel
> bed can distinguish the bloom texcoord carrier from the scene one — what the
> bed proves is that attribute 6 is uploaded and correct. `r_bloom_scenebrightness`
> is dead code that must stay dead (it tests a pointer that is always NULL at
> that point) and `MODE_BLOOMBLUR` has no enum entry in this fork. Motion blur is
> covered by shape only — it calls `lhrandom` per frame and cannot be bedded
> without pinning `r_motionblur_randomize 0`. Video capture's PBO path has no
> Metal arm at all.
>
> **Phase 5 is next, and it is the first milestone Seb can judge against the
> configuration he actually plays.**

> **Status, 2026-08-06: PHASE 5 SLICES 1-3 ARE DONE — the Metal renderpath
> relights.** The sidecar comes up on the renderer's MTLDevice (5-1), its whole
> compute half runs there byte-identically to GL (5-2), and the composite is now
> an ordinary backend draw (5-3). Twenty-seven parity vantages; smoke 79.
>
> **5-3's measurement, every two-boot control clean at zero FIRST:**
>
> | vantage | mean | max | covers |
> |---|---|---|---|
> | rt_spawn | 0.0005 | 1 | RT over the lightmap (walllight 0); full-res trace → the NEAREST sampler |
> | rt_wall | 0.0006 | 1 | walllight 0.8 — **the white-frame canary** — plus the view model and its depth mask |
> | rt_ogre | 0.0006 | 1 | entity BLAS, smooth normals, light cores |
> | rt_lava | 0.0010 | 1 | e1m7: the static lava TLAS instance and the merged lava lights — **informational, see below** |
>
> Max delta 1 on all four, and the RT bed is **deterministic**: every control is
> 0 of 307200 px, which is what 5-1's history-0 measurement predicted and is a
> better instrument than the documented screenshot noise floor. The full
> 27-vantage sweep reproduced every inherited vantage at its recorded value
> (spawn 0.0005/1 · hall, flipped, ents_hall 0.0009/45 · diffuse 0.0006/20 · sky
> 0.0001/1 · the six 4d vantages at 0.0000 · water dirty by construction).
>
> **rt_lava carried one correlated outlier, since diagnosed — see the 5-4 block
> below for the corrected account.** Its normal value is 0.0010 / max 1,
> confirmed four independent ways.
>
> **Three things were proven rather than reasoned about, and each could have
> shipped silently wrong.** (1) The v-orientation: the kernel writes GL window
> rows and Phase 4a chose GL-layout targets, so no flip *should* be needed — but
> a flip error is a 180°-rotated shadow term, which on a symmetric frame reads as
> "the RT looks a bit off". `RT_METAL_COMPOSITE_FLIP=1` ships, composites
> deliberately upside down, and moves 41.75% of pixels, so the texcoords
> demonstrably arrive and the upright variant is the one matching GL to max 1.
> (2) The canary was RUN, at Seb's own walllight 0.8: both backends come out at
> mean luminance 14.09 with 0.000% flat-white (a blinding frame is ~250 and
> near-total), and wall lighting is provably active — 14.09 against 8.85 at
> walllight 0. (3) The RT bed moves pixels at all: rt_metal 0 against the same
> frame differs by 23.76%.
>
> **A real defect in the backend, found by reading and invisible to every
> existing vantage**: `mb_apply_state` widened ANY collapsed depth range to
> zfar 1.0. It was written for the `{0,0}` the state shadow legitimately starts
> at, but a deliberately collapsed range is the technique this composite runs on
> — `GL_DepthRange(0.0625, 0.0625)` pins the quad at one window depth so it
> depth-tests against the scene and leaves the view model alone. Narrowed to the
> exact `{0,0}` pair. Provably inert before 5-3 by enumeration rather than
> argument: `gl_rmain.c:7132` is the only collapsed site in the tree and it is
> the murk, which sentinels magenta here.
>
> **No new shader, and that is a finding.** `R_SetupShader_Generic` with
> `suppresstexalpha` sets REFLECTCUBE, whose MSL arm is `c.rgb *=
> Texture_First.sample(...).rgb` with `c` seeded from VertexColor under the
> always-set VIEWTINT — with white vertex colour that is exactly the bridge
> shader's `vec4(term.rgb, 1.0)`. **`rtoff`/`rtscale` dissolve** rather than
> porting: the viewport transform absorbs the origin and normalised texcoords
> absorb `rt_metal_scale`. They exist on the GL side only because a
> `sampler2DRect` addresses in texels.
>
> **Design decisions worth carrying.** The QUEUE is deliberately not shared,
> departing from this file's wording — the backend commits one command buffer per
> frame at end-of-frame, so sharing the queue risks serialising the trace behind
> it and erasing the async overlap, a regression invisible to every pixel gate
> here. `s_active` has exactly one owner per path (the GL tail inline;
> `RT_Metal_MarkComposited` after the caller's draw on Metal), so no route
> double-sets and none forgets — and every way of not drawing lands on RT
> inactive, which is the correct fallback rather than a white frame. Adopted
> handles are re-minted on POINTER IDENTITY, not presence: `rt_pair_ensure` can
> replace a texture on resize while the handle table holds its own strong
> reference, so a presence check would keep the old one alive and go on showing
> it. And `VID_Shutdown` now brings the sidecar down on BOTH renderpaths — it
> only ever did on GL, under a comment saying no sidecar existed on the Metal
> one, true when written and false since 5-1.
>
> **Deliberately deferred, stated rather than dropped.** The fog and god-ray
> kernels are not encoded on this path until Phase 6 (their only consumer,
> MODE_VOLUMETRICFOG, sentinels magenta) and say so once instead of costing GPU
> time in silence — note the shaft condition keys on `!fogWanted`, so it needed
> the same term rather than inheriting one.

> **Status, 2026-08-07: SLICE 5-4 IS DONE — the Metal composite reprojects.**
> `SHADERMODE_RTCOMPOSITE` is the mechanical port of the bridge's `kCompFS`
> (rotation-only remap plus the translation-aware depth refine) and replaces the
> stock GENERIC shader 5-3 used — GENERIC cannot express per-pixel ray
> arithmetic. It is a **Metal-only mode by construction**: the GL path keeps its
> own private GLSL program and never requests it, so `shader_glsl.h` has no such
> arm and never compiles one.
>
> **One source for the values, two for the expression.**
> `RT_Metal_GetReprojection` computes the block once and both composites upload
> it — the GL uploader was refactored to read it too, and the byte gate proved
> that inert (GL RT frame byte-identical to the pre-5-4 baseline; cmdtrace still
> 1645 `66c209c4e0e81cab`).
>
> **The measurement that matters, because the obvious one is a no-op.** A static
> camera makes reprojection an identity, so enabling it changes **0 of 307200 px
> on both backends** — a parity number there describes two shaders agreeing about
> doing nothing. `RT_METAL_REPROJ_TEST=1` pretends the shown frame was traced
> from 2° and 8 units away: GL moves 63685 px (20.73%, mean 0.3771, max 27),
> Metal moves 63684 px with identical statistics, and the two **still agree at
> 0.0005 / max 1**. Same pattern as `RT_METAL_COMPOSITE_FLIP`, and the same
> lesson as the 4d HDR shoulder.
>
> All four RT vantages reproduce their 5-3 numbers with reprojection now on
> (0.0005 / 0.0006 / 0.0006 / 0.0010, all max 1, controls clean).
>
> **CORRECTION, 2026-08-08 (6-3b): THIS ARM SHIPPED WITH A COMPENSATING PAIR OF
> V-FLIPS AND THE PARAGRAPH ABOVE IS EXACTLY WHY IT SURVIVED.** The MSL negated v
> going into the NDC and again coming out of the remap. The two cancel *exactly*
> — but only while the shown and current cameras differ by a pure yaw about world
> z with both level. Negating the NDC reflects the pixel's ray about the camera's
> horizontal plane; under yaw that reflection commutes with the basis change and
> the output flip undoes it. It is also self-consistent at the identity. So both
> beds the arm ever met agreed with it: the static vantages (identity) and
> `RT_METAL_REPROJ_TEST`, **whose perturbation is two degrees of YAW** — the one
> axis capable of showing the error is the one the fixture left symmetric. That
> is the 6-1 cube-probe lesson in a second place: *a symmetric fixture cannot
> detect a symmetric bug*, and "the two backends move by identical statistics"
> is not proof that either moves correctly.
>
> Found by `RT_METAL_REPROJ_TEST=2` (yaw **plus pitch**, added in 6-3b for the
> murk). On `rt_ogre`, controls clean at zero throughout: mode 1 reads
> 0.0007 / max 1 before and after, mode 2 reads **2.6879 / max 163 before** and
> 0.0007 / max 1 after. Fixed in `5311093b`, and **provably inert wherever the
> old code was right** — unperturbed pre- against post-fix is 0 of 307200 px on
> both backends on `rt_ogre` and `rt_wall`. The frames it changes are the ones
> where the camera pitched between the traced and the shown frame, i.e. ordinary
> play, and Seb runs `rt_metal_reproject 1`.

> **Status, 2026-08-07: SLICE 5-5 IS WRITTEN BUT NOT ENABLED — and that is the
> result, not a shortfall.** The `USERTLIQUIDS` MSL arm and the Metal bind branch
> exist; the static parm is gated to `RENDERPATH_GL32`, so Metal keeps the 5-3
> behaviour (liquids do not take the RT term). **No bed can measure it**: the
> frozen bed suppresses the `r_wateralpha` path outright so the feature is
> provably dead there (and `r_novis`, which `gl_rmain.c:8810` also accepts, does
> not rescue it); the unfrozen `water` recipe keeps the alpha but is bimodal with
> RT on (controls of 0.0000 and 3.6 for identical pairs, because the term rides
> lights and entities the running server moves); a deferred freeze is worse
> (10.04). Seb runs `rt_metal_liquids 0.45`, so shipping an unmeasured multiply
> on his water is the one thing this arc must not do. **Flip one condition when a
> bed exists.**
>
> ~~**`[[position]]` is NOT in GL numerics**~~ — **WRONG, corrected at 5-5-3.**
> This slice inferred a y-flip for the liquids arm from MODE_RTCOMPOSITE's
> behaviour and shipped it. Once a bed existed the inference was measured and
> it is backwards: **`in.Position.y` IS `gl_FragCoord.y`**, because render
> targets store GL-layout images (row 0 = the GL bottom, via the y-negated
> projection) and `[[position]]` counts down from row 0. With the flip, GL vs
> Metal moves 19% of pixels at max 32 and the world gate FAILS; without it,
> 0.0015 / max 1 / PASS. **Phase 6 should read this first** — the murk's
> `gl_FragCoord` consumers arrive there in bulk — and should read it as the
> corrected version. The transferable lesson is not about y at all: this arm
> could not be bedded, so it was reasoned about instead, and the reasoning was
> wrong. Leaving it OFF was the right call and is what contained the error.
>
> **The command-digest gate went blind.** On unchanged HEAD three consecutive
> `cmdtrace check` runs returned 1644, 1648 and 1645. Establish its noise floor
> before believing it, exactly as CLAUDE.md already demands of the byte gate. GL
> was gated on pixels instead and is provably unchanged.
>
> ~~**Follow-up, 2026-08-07: the camera is EXCLUDED and the bed is still
> blocked.**~~ The camera exclusion stands; everything built on top of it was
> chasing the wrong suspect. **The freeze was never involved at all** — see the
> 5-5-3 block below. The `r_wateralpha_force` 0-vs-1 asymmetry that seemed to
> convict it (18.03% unfrozen, 0 frozen) is not reproducible against the fixed
> loader and no mechanism for it survives; frozen and unfrozen print identical
> material flags.
>
> **Frozen-bed camera rules, corrected by six probe boots**: origin applies;
> pitch applies divided (ask 3x); **yaw does not apply at all** — four boots at
> one origin requesting yaw 0/90/180/270 were byte-identical. Every frozen
> vantage looks along the map's spawn heading; to frame something specific, move
> the ORIGIN. Harmless for parity, essential for placing a new vantage.
>
> **Honest limits.** The RT vantages carry a **+1 draw / +4 vertices / +6
> elements** cross-backend counter delta, predicted from first principles before
> measuring and landing exactly: GL's composite is a raw `glDrawArrays` and
> invisible to the counters, Metal's is a counted `R_Mesh_Draw`. Dump-2's entity
> lines carry a small backend gap on these vantages (GL's own boots 2088/2090,
> Metal stable at 2094) — the documented dump-2 contamination class, not a gate;
> dump-1 is the instrument. `gl_filter_force` beats `TEXF_FORCENEAREST` in the
> sampler cache, so a forced `gl_texturemode` would silently give the term LINEAR
> where GL's unconditional `glTexParameteri` would not — inert at bed and Seb
> defaults, recorded rather than fixed.
>
> **The cfg-diff harness the bed's header always asked for now exists** as a
> procedure that was actually run: emit `spawn` and `hall` from the pre-edit and
> post-edit scripts and diff. Both byte-identical (22 and 25 lines).

> **Status, 2026-08-07: SLICE 5-5-3 IS DONE — RT liquids are ON, and the thing
> blocking them for three slices was not in this arc at all.** The bed that
> "could not exist" is an ordinary frozen vantage with controls at zero. What
> stopped it was the **QRP replacement texture pack**: an external image for
> `*04water1` makes `Mod_LoadTextureFromQ3Shader` succeed, and
> `model_brush.c:1878` then `continue`s past the whole Q1 liquid classification,
> so the texture arrives as a plain opaque wall with no `MATERIALFLAG_WATERALPHA`
> and no liquid supercontents. Measured, one variable: `basematerialflags` 0x40
> with the pack against 0x44550 without; `*lava1` `supercontents SOLID|OPAQUE`
> instead of `LAVA`. **It is upstream behaviour and it is not a bed problem** —
> it silently disabled `r_wateralpha`, `rt_metal_liquids`, `r_lavaglow`,
> `r_lavaboil`, `rt_metal_lavaemissive` and `rt_metal_lavalights` in Seb's own
> installation, and put liquids into the RT world BLAS as shadow casters. Fixed
> by `m5_liquidflags` (default 1); `0` reproduces the old frames byte for byte
> (0 of 307200 px, four RT vantages, both backends).
>
> **"The pre-load freeze suppresses the r_wateralpha path" is RETRACTED** from 4c,
> 5-5 and 5-5-2. Frozen and unfrozen print identical flags.
>
> **The bed then caught a real port defect on its first run, which is the whole
> argument for having refused to ship 5-5 enabled.** Flipping the static parm to
> both renderpaths did NOT converge: cross 19.07% at max 32, world gate FAIL,
> controls clean. The cause was 5-5's own inferred y-flip — see the correction
> in that block. Removing it: **0.0015 / max 1 / PASS**.
>
> **Acceptance**, controls clean at zero first on both backends: the new
> `rt_liquid` vantage isolates at **18.41% of pixels moved** between
> `rt_metal_liquids` 0 and 0.45 on GL, and crosses at 0.0015 / max 1. Twenty-eight
> vantages now.

> **Status, 2026-08-07: SLICE 5-6 — the seam audit, and the first look A/B
> against the configuration Seb actually plays.**
>
> **THE SEAM, enumerated rather than estimated.** Runtime-dead on the shared
> path today: `rt_pair_ensure`'s IOSurface + `CGLTexImageIOSurface2D` + GL
> rectangle creation (only the middle third of the triple survives); the
> `s_glctx` / `rt_abandon_gl` context tracking; the composite's whole GL tail;
> `R_RTLiquid_UnbindAll`; and the rectangle-texture hazard class at the liquids
> bind. Still live and **Phase 6's, not this slice's**: the murk's own two
> rectangle binds (`gl_rmain.c` kernel fog and shafts), because
> MODE_VOLUMETRICFOG sentinels magenta on Metal, and the fog/shaft kernels which
> are not encoded on that path at all. **None of it is "compiled out"** — see the
> correction in the Native RT section above; `vid_renderer` is a runtime switch
> in one binary.
>
> **One real hazard closed.** `RT_Metal_InitWithDevice` early-outs on
> `if (s_dev) return`, silently keeping the old device and discarding the offered
> one. That is correct only because `VID_Shutdown` brings the sidecar down
> between renderpaths — an invariant owned by a distant caller and stated only in
> prose, which is the shape 5-1 found four times over. It is now CHECKED: a
> re-init on a different device, or a change of shared-ness, prints a warning
> instead of quietly serving cross-device resources. Unreachable today by
> construction, which is why the smoke round-trip check now also asserts the
> warning never appears.
>
> **`gl_filter_force` decided, and it is narrower than 5-3 recorded.**
> `metal_textures.m` already implements GL's exact rule for ordinary textures.
> The asymmetry is only the RT term: on GL it is a foreign rectangle whose
> filter is set unconditionally in the sidecar's own tail, so it is not in the
> texture manager and `gl_texturemode ... force` cannot reach it; on Metal it is
> adopted, so it obeys the same rule as everything else. Reachable only by that
> command, which is not even an archived cvar. **Recorded as a deliberate,
> benign divergence in Metal's favour** — the consistent behaviour is the
> adopted one — and not worth a texture-system flag to undo.
>
> **`MTLStorageModePrivate` was NOT measured this session and is still open.**
> The 5-2 comment defers it here; it stays deferred, stated rather than dropped.
> It needs `RT_METAL_KERNELMS=1` per-stage deltas at 1080p on both storage
> modes, and it complicates `rt_metal_termprobe`, which `getBytes` the term for
> the smoke suite's cross-backend hash and would need a blit-to-staging.
>
> **THE LOOK A/B**, `test/look-ab.sh` + `test/lookmetrics.py`, both committed:
> his cvar values injected over the RT vantages via `PARITY_EXTRA`, with
> volumetrics and the history pin excluded for reasons the script states in
> full. **Two findings, and the second is the one that matters.**
>
> **His gamma makes the 8-bit readback near-black, so a ratio acceptance cannot
> be evaluated there.** `r_brightness 0.4` + `v_gamma 0.5` + `v_contrast 0.625`
> put the whole frame in the bottom four levels — rt_wall's frame mean is
> **0.81 of 255**, against 17.53 at the bed's neutral gamma, with p99 = 4. A
> "within 1% of frame mean" gate divides by 0.81 there, so one level of
> difference reads as +123%. The gamma path itself is blameless and that was
> measured, not assumed: the three cvars **alone** are BYTE-IDENTICAL across
> backends, 0 of 307200 px.
>
> **The same darkness breaks the CONTROLS, via the notify text.** The RT
> vantages set `developer 1` so the harness can grep compile lines, that output
> lands in the notify area, and notify text never expires on a frozen clock.
> White text stays white through any gamma ramp — so once the scene is crushed
> to the bottom four levels the text is the brightest thing in the frame and
> dominates the statistics. Measured: two-boot controls dirty at max **210** and
> **226** on a frame whose p99 is 4, with the block map confined to the top two
> rows and the whole 3D scene clean. `look-ab.sh` pins `developer 0` +
> `con_notify 0`. **Generalises: any bed that crushes its scene must also
> silence the 2D overlays, because they do not crush with it.**
>
> **The entire cross-backend look difference at his config is ONE unported
> feature: `gl_texturecompression`.** Metal has no compression path at all, so
> the cvar is inert there. Isolated on rt_wall: GL with it on against GL with it
> off moves **97.03%** of pixels (mean 1.88, max 62); Metal with it on against
> Metal with it off is **byte-identical**. Full attribution, each isolated
> alone, controls clean at zero:
>
> | injected alone | cross | verdict |
> |---|---|---|
> | `r_brightness` + `v_gamma` + `v_contrast` | 0.0000 / max 0 | byte-identical |
> | `rt_metal_*` tuning (samples, scale, softness, darkness, color, ambient) | 0.0006 / max 1 | PASS |
> | `gl_texture_anisotropy 2` | 0.0012 / max 2 | PASS |
> | **`gl_texturecompression 1`** | **1.8846 / max 62 / 97.03%** | **unported on Metal** |
>
> So **everything Phase 5 owns is at parity at his settings**, and the one thing
> that is not is a texture-pipeline feature that predates this arc, is inert
> rather than wrong, and produces the BETTER image on Metal. Whether to
> implement compressed uploads is Phase 8's: on unified memory the VRAM saving
> is far weaker than it was on discrete, and it is real work (Metal takes
> pre-compressed data, where GL asks the driver to compress at upload).
>
> **ACCEPTANCE — Phase 5's row is MET.** His configuration, compression
> equalised and the notify text silenced, every two-boot control clean at zero
> on both backends FIRST:
>
> | vantage | cross | frame mean |
> |---|---|---|
> | rt_spawn | 0.0000 / max 1 / PASS | +0.000% |
> | rt_wall (walllight 0.8, the canary) | 0.0000 / max 1 / PASS | +0.000% |
> | rt_ogre | 0.0000 / max 1 / PASS | +0.000% |
> | rt_liquid (liquids 0.45, wateralpha 0.8) | 0.0000 / max 1 / PASS | +0.000% |
> | rt_lava | 0.0000 / max 1 / PASS | — |
>
> "Frame mean within 1%" and "diffs confined to expected regions" are both met
> with room to spare — the cross is *tighter* than the bed's own 0.0005-0.0015,
> because a near-black frame has less to disagree about, which is the same fact
> that makes the ratio gate unusable and is worth holding both ways at once.
> `rt_lava` needed its settle time raised again (26 → 34 s): the look config
> drives the trace at `scale 0.75` and `samples 6` rather than the vantage's
> 0.5/default, so it takes longer still to settle, and at 26 the Metal control
> went dirty in the documented signature (1.2487 / max 157, cross exactly equal
> to it). **A vantage's settle pin has to clear the heaviest configuration it is
> ever run at, not the default one.**

> **Status, 2026-08-07: SLICE 6-1 IS DONE — the Metal path has 3D textures.**
> The hard prerequisite for everything else in Phase 6. `metal_textures.m`
> refused `depth > 1` outright, and the murk needs two volumes (its noise field
> and its baked world field); so does `r_lavaboil`. Volumes are now created,
> uploaded, read back and **sampled**, proven by an instrument that was made to
> fail six different ways before it was believed.
>
> **The proof is two tests, not one, and they fail independently** — the
> standard `r_metal_drawprobe` set at slice 3. A readback proves the bytes
> arrived; it cannot prove a shader can address them, and for 3D that gap is the
> whole risk, because 6-2's murk arm rests entirely on 3D sampling. So
> `r_metal_textureprobe` grew an upload half (whole volume byte-exact against a
> CPU model) and a **sampling** half — a real compute dispatch through the
> renderer's OWN `MTLSamplerState`, at texel centres and at exact midpoints.
>
> All five measured on the SHIPPED 8x4x6 fixture, so the numbers are comparable:
>
> | deliberate break | upload (of 768 B) | sampling (tol 2/255) |
> |---|---|---|
> | z collapsed to slice 0 (2D region) | FAIL 640 | FAIL 225/255 |
> | `bytesPerImage` missing its `* height` | FAIL 432 | FAIL 120/255 |
> | `bytesPerRow` as `height * bpp` | FAIL 768 | FAIL 255/255 |
> | NEAREST sampler where LINEAR was asked | **PASS** | FAIL 15/255 |
> | y/z transposed in the sample coordinate | **PASS** | FAIL 143/255 |
>
> The two PASS cells are the point: those defects leave the upload byte-perfect
> and are invisible to a readback. The z-collapse figure is a check on the check
> — 640 is exactly 768 minus the 128-byte slice 0 that *was* written.
>
> **THE PROBE'S VOLUME IS 8x4x6 AND NOT A CUBE, and that is a measured
> correction rather than taste.** The first version was 8³ — and an 8³ probe
> **cannot see a width/height transposition at all**, because `bytesPerRow` and
> its transpose are the same number. Measured: a build whose `bytesPerRow` read
> `height * bpp` PASSED the cube-shaped probe at 0 of 2048 bytes and 0/255
> sampling. At 8x4x6 the identical break moves **all 768 bytes and 255/255**.
> Non-cubic also makes it non-power-of-two in two axes, which is what the real
> baked field always is. **A symmetric fixture cannot detect a symmetric bug** —
> the same lesson the drawprobe's asymmetric 2x2 source records, arrived at
> independently one dimension up.
>
> **THE SECOND-ORDER BLOCKER, which the depth guard had been hiding.** The tree
> has FOUR 3D producers, not the two Phase 6 needs: the Q3BSP lightgrid, and
> `r_shadow.c`'s bounce grid — which asks for `TEXTYPE_COLORBUFFER16F`/`32F` as
> a volume **with real pixel data**. `mt_isrendertarget` returns true for those
> by textype, so the shared usage/storage block would hand it
> `MTLStorageModePrivate`, on which `replaceRegion:` is invalid. Widening the
> depth guard alone would have converted a silent, harmless refusal into a
> validation failure the moment anyone set `r_shadow_bouncegrid` — default 0 and
> dead in this fork's configuration, so **no bed would ever have caught it**. 3D
> render-target formats are now refused by name; the bounce grid stays exactly
> as dead as it was, and now says so.
>
> **The guard is SPLIT, not widened.** `depth > 1 || sides > 1` became
> `sides > 1` alone plus the render-target test, so enabling volumes cannot
> enable cubemaps by accident. The cubemap refusal is also now rate-limited:
> `gl_main_start` calls `R_BuildWhiteCube` and `R_BuildNormalizationCube`
> unconditionally on both renderpaths, so it was printing **twice on every Metal
> start and every `vid_restart`** — harmless (both consumers are in the MSL
> negative list) but it reads as a regression in whatever landed last.
>
> **What GL got: nothing.** `cmdtrace` is unmoved at **1645
> `66c209c4e0e81cab`** — the mode of six runs on the final tree, five of which
> returned it and one of which excursed (see the instrument note below). The
> only shared-file change is `R_UploadFullTexture`'s Metal arm gaining the
> `depth` argument it always had in scope and never passed — which was correct
> only while every volume was refused.
>
> **Recorded honestly rather than glossed:**
>
> - The `Metal_Texture_Sample3D` pipeline is re-minted on **device pointer
>   identity**, the 5-1 rule, because `Metal_Texture_Shutdown` has **zero
>   callers in the tree** and nothing clears it across a `vid_restart`. It is
>   **correct-by-construction and NOT measured to be load-bearing**: the
>   presence-only early-out also passes, because macOS returns the *same*
>   `MTLDevice` object across a metal→gl→metal round trip (measured —
>   `0xb12afc000` both times). It is kept because it costs nothing and closes a
>   shape this arc has now been burned by twice. That `Metal_Texture_Shutdown`
>   is never called is **pre-existing and still true** — the sampler cache and
>   handle table survive a device teardown too. Not fixed here; it wants its own
>   slice and a bed that can see it.
> - **`gl_filter_force` inverts the field's filter relative to GL, and Phase 6
>   is what makes it matter.** GL gates both `TEXF_FORCENEAREST` and
>   `TEXF_FORCELINEAR` on `!gl_filter_force` and otherwise falls to
>   `gl_filter_min`/`_mag`; `mt_sampler_for`'s two clauses BOTH require
>   `!gl_filter_force`, so it forces LINEAR unconditionally. `gl_texturemode
>   GL_NEAREST force` therefore gives the field NEAREST on GL and LINEAR on
>   Metal. 5-6 recorded this as benign for the RT term; on the field it is not,
>   because the G channel is a signed distance whose **trilinear zero crossing
>   reconstructs the waterline** and NEAREST would snap it to whole 64-unit
>   cells. Separately, the Metal key never consults `gl_filter_mag` at all.
>   **NOT fixed in 6-1**, deliberately: it changes every Metal texture, it is
>   unreachable at Seb's config and at every bed default (no vantage sets
>   `gl_texturemode`), and an unmeasurable change is exactly what this arc
>   refuses to ship. It needs a vantage that sets the forced mode.
>   *(**CLOSED BY 6-1b** — the vantage is `texfilter`, and building it first
>   turned out to matter: the defect was not the small filter divergence this
>   bullet describes but a **stock control the Metal path could not see at
>   all**. See that slice's block below.)*
> - **A GL/Metal semantic gap that 6-2 will sit on top of.** `TEXTYPE_RGBA`
>   *without* `TEXF_ALPHA` resolves to internal format `GL_RGB` on GL, so alpha
>   reads a structural 1.0; on Metal it is `RGBA8Unorm` and the uploaded byte
>   survives. The noise volume is exactly that case and is benign only because
>   its generator writes `p[3] = 255`. The ground-fog round already promoted its
>   B channel from a dead duplicate to a third seed — **A is the obvious next
>   free channel, and it would work on Metal and be a constant 1.0 on GL, with
>   no warning at either site.**
> - 3D mip chains are sized from `max(w, h, d)`, matching
>   `GL_Texture_CalcImageSize` exactly. **No 3D texture in the tree is
>   mipmapped**, so that arm is correct-by-construction and unexercised.
> - No 3D partial-update arm exists, and none is needed: the plan expected the
>   field to be re-sent partially on rebake, but it is destroy-and-recreate
>   (`R_Volumetric_FreeField` then a fresh `R_LoadTexture3D`), and
>   `R_UploadPartialTexture` `Sys_Error`s on any non-2D texture before the
>   renderpath switch. `Metal_Texture_UploadPartial` refuses volumes itself
>   anyway rather than trusting a guard in another file.
> - `MTLStorageModeShared` is kept. Private storage for volumes is the same open
>   question 5-6 left for the sidecar outputs and wants its own measurement, not
>   an argument.
>
> **Instrument note, and it is worse than 5-5-3 recorded.** `cmdtrace`'s
> excursion at unchanged HEAD this session was **422 calls**, not 218: five
> captures gave 1648, 1645, 1645, 1645 and **1223**, the last passing every one
> of the script's own guardrails (trace banner present, frozen tail constant at
> a single state). The modal 1645 matches the standing baseline, and on the
> FINAL tree six runs returned it five times with one excursion. **Three runs
> minimum, take the mode, and never quote a single capture** — at roughly one
> excursion in six runs, a single reading has about a one-in-six chance of
> inventing a regression that is not there, or of hiding one that is.

> **Status, 2026-08-07: SLICE 6-1b IS DONE — the Metal sampler mirrors GL's
> filter rule, and the bed can now see it.** 6-1 deferred this with the right
> reasoning and the wrong estimate of the defect. It read as a filter
> divergence at an unreachable setting; building the vantage first showed it
> was a **stock console control that the Metal renderpath could not see at
> all**.
>
> **The isolation IS the finding, and the cross number is the weaker half.**
> Per the 4d rule, both vantages were isolated against the identical `hall`
> frame before any parity number was quoted — and the isolation is what
> diagnosed it:
>
> | control (vs the same frame, mode unset) | moves on GL | moves on Metal |
> |---|---|---|
> | `gl_texturemode GL_NEAREST force` | 145217 px, **47.3%**, mean 2.1884 | **0 of 307200** |
> | `gl_texturemode GL_NEAREST_MIPMAP_LINEAR` | 115541 px, **37.6%**, mean 0.5524 | **0 of 307200** |
>
> A zero in that right-hand column is a different class of fact from a large
> cross number. The cross merely FAILED (10.84% > 8, and 0.60% > 8) the way any
> sizeable divergence fails; it could not say that one side was ignoring the
> knob. **A parity bed measures agreement, not liveness — if a control is a
> no-op on one backend, only the per-backend isolation says so.** That is the
> reusable half of this slice.
>
> **Three defects, not the one that was suspected**, all hidden by the single
> `nearest` boolean the old predicate collapsed GL's tree into:
>
> 1. `gl_filter_force` **inverted** — both old clauses required
>    `!gl_filter_force`, so a forced mode forced LINEAR, where GL falls through
>    to `gl_filter_min`/`_mag`. The suspected one.
> 2. `gl_filter_mag` **never consulted**. It is an independent enum:
>    `modes[4]` is `{"GL_NEAREST_MIPMAP_LINEAR", GL_NEAREST_MIPMAP_LINEAR,
>    GL_NEAREST}`, so GL magnifies NEAREST there and Metal magnified LINEAR.
> 3. **The mip filter rides inside the minification enum**, so it is not a
>    function of "is this nearest" — found while transcribing, not suspected.
>    `GL_NEAREST`/`GL_LINEAR` carry no mip term, so selecting either **disables
>    mipmapping** on a `TEXF_MIPMAP` texture, and GL's FORCELINEAR arm picks its
>    own mip filter from `gl_filter_min`. Both were hardwired the other way.
>    Note this one is reachable with a plain **unforced** `gl_texturemode
>    GL_LINEAR` — the bullet 6-1 wrote understated the reach.
>
> The fix is a **transcription** of `GL_SetupTextureParameters`' tree returning
> the two GL enums it would set, then a mechanical GL-enum → Metal-filter map —
> deliberately not a second paraphrase, since a paraphrase is what produced all
> three. `mt_sampler_key` now keys on the sampler's **resolved state** rather
> than a hand-listed subset of the inputs, so it cannot under-key again.
>
> **It changes every Metal texture, so the no-op case was measured, not
> argued.** At default filter settings the post-fix Metal `hall` frame is
> **byte-identical to the pre-fix one, 0 of 307200 px** — as the algebra
> predicts (at `GL_LINEAR_MIPMAP_LINEAR` unforced, old and new agree on all
> three arms). `spawn` and `hall` reproduce their recorded crosses exactly
> (0.0005 / max 1; 0.0009 / max 45 / 15 px).
>
> **After the fix**, both new vantages pass the world gate — `texfilter` 0.0012
> / max 68 / 52 px, `texfiltermag` 0.0009 / max 44 / 70 px, all four controls
> 0.0000 — and Metal now moves 145223 px (47.273%) against GL's 145217
> (47.271%) on the same control. The residual sits at the `hall` camera's own
> documented 65 px / 15 px > 8 GL-direct-framebuffer artefact (the `fbo1`
> note), i.e. the filter path contributes nothing measurable beyond it.
>
> **Bed shape worth copying:** both vantages emit exactly ONE cfg line and
> share `hall`'s camera, and `hall` itself emits nothing — so **hall's own
> frame is the isolation baseline**, byte for byte, with no `PARITY_EXTRA` and
> no second bed. Two vantages rather than one because the magnification slip is
> reachable *without* `force`, so `texfilter` alone could not localise it.
>
> **What GL got: nothing.** `cmdtrace` PASS three times at **1645
> `66c209c4e0e81cab`**, the standing baseline, with no excursion. The change is
> `metal_textures.m`-only. Smoke 83/83, `make sv-release` links, Xcode Release
> green.

> **Status, 2026-08-07: SLICE 6-2 IS DONE — the volumetric murk renders on
> Metal.** Split in two, as this plan said it should be if it grew.
>
> **6-2a (`185b2ecf`)** moved the 95-line density model out of `shader_glsl.h`
> into `shader_density.h` as spliced text. **The proof is total rather than
> statistical**, which is why it was worth its own commit: `R_ShaderStrCat`
> joins `builtinshaderstrings` with NO separator, so collapsing 73 array
> elements into one concatenated literal cannot change a byte. Measured —
> `r_glsl_dumpshader` writes **`combined_crc57874.glsl` before and after**, the
> engine's own CRC in its own filename, with `diff -r` clean. A pixel bed would
> have been the weaker instrument here: the driver compiles the same bytes.
>
> **Which seam that crosses, and why the file's own header was not wrong.**
> `shader_density.h` says sharing is blocked by "16 uniforms whose names and
> packing differ per side". True — of the **march versus kernel**: `kFogSrc`
> packs named struct fields where the marches read `vec4` lanes. But the two
> MARCHES read identical uniform NAMES, because the Metal seam is name-based
> reflection and feeds an MSL arm from the very same `R_Shader_Uniform*` calls.
> The barrier is march-to-kernel, not march-to-march. **Hand-kept copies stay at
> two**, exactly as agreed.
>
> **6-2b (`31c2f312`)** is the MSL arm. Frozen e1m3 bed, both two-boot controls
> **byte-clean at zero first**, cross **0.0024 / max 1 / 0 px above delta 1** —
> passing the strict 2D gate, tighter than the world gate this mode would
> otherwise be judged by. **Zero lines of `gl_rmain.c` changed**; the twenty murk
> uniforms are fed by name.
>
> **The isolation was measured before the parity number was quoted**, per 4d's
> rule that a no-op bed always reports perfect parity: `r_volumetric` 0 against 1
> **on Metal** moves **98.42% of pixels at mean 49.29, max 216**. The compile
> line corroborates the shape independently — *"volumetricfog compiled (3
> textures)"*, exactly the three the sampler walk assigns.
>
> Four spellings differ and are aliased rather than forked: the `vecN` names,
> `dp_texture3D`'s sampler pairing (the noise REPEATS, the field CLAMPS — bound
> separately, because one shared sampler is invisible in a still and wrong
> wherever the ray leaves the field), the uniform prefix, and `gl_FragCoord`.
> `in.Position` is used with **no flip**, per 5-5-3's correction of 5-5.
>
> **Two bits refused, both 6-3's rather than permanent**: `USEVOLUMETRICSHAFTS`
> and `USEVOLUMETRICKERNELFOG`, which read a `sampler2DRect` — a type Metal does
> not have. 6-3 makes the sidecar's fog and shaft outputs adopted `MTLTexture`s
> and retires the last two rectangle binds; porting them now would invent a
> binding 6-3 deletes. The kernel-fog arm is an early return *inside* the GLSL
> `main()`, so refusing it refuses that permutation wholesale — the march this
> slice ports is the documented per-frame fallback, the right half to land first.
> `r_volumetric_debug` stays permanently refused in MODE_POSTPROCESS.
>
> **Stated rather than implied:** the bed is LOCAL to this slice, because
> `test/parity-4a.sh` is structurally blind to the murk. **6-5 owes the harness
> real murk vantages.** The e1m4 lake kernel-vs-GL A/B this phase's row asks for
> needs the kernel, which is 6-3's.
>
> *Corrected at 6-3b: this block, and the Phase 6 row below, both said the
> harness **pins** `r_volumetric 0` **on every vantage**. It does not, and never
> did — the line is emitted in the `rt_*` arm and nowhere else; the other
> vantages were murk-free because every boot takes a fresh `mktemp` userdir and
> the cvar defaults to 0. The blindness was real, the mechanism was not, and the
> difference matters: a default can move under a bed and a pin cannot. The claim
> had already propagated into a session brief as fact.*

> **Status, 2026-08-08: SLICE 6-3 IS DONE — the Metal renderpath runs the whole
> fog stack.** The in-kernel fog integral and the screen-space god rays now
> encode on the shared device and are consumed by MSL arms, so the murk on Metal
> is the sidecar's own output rather than the GL-march fallback.
>
> **6-3a (`69177649`)** was the inert half: pointer-identity adoption of the fog
> and shaft outputs into the renderer's texture table, and a shared-device arm on
> both accessors. **6-3b (`73507ec1`)** is the coupled half, and it had to land
> as one change — un-skipping the kernels without the bind arms is a call through
> a NULL `qglBindTexture`, and the bind arms without the MSL arms are a magenta
> frame. It also pulled the bed forward from 6-5 (`a9a5252f`), because the one
> part of the slice that is not transcription needed measuring, not arguing.
>
> | vantage | cross | at HEAD before |
> |---|---|---|
> | `murk` | 0.0017 / max 1 | 0.0017 (unchanged — 6-2b's arm) |
> | `murk_water` | 0.0013 / max 1 | 0.0013 (unchanged) |
> | `murk_kernel` | **0.0000 / max 0** | 136.8154 / 98.7% (magenta) |
> | `murk_shafts` | 0.0044 / max 1 | 141.7234 / 98.7% (magenta) |
>
> All eight controls byte-clean at zero. **Isolation first, per 4d:** kernel fog
> on versus off at the same camera moves **98.714% of the frame at mean 22.6112
> on Metal**, against GL's 22.6131 — the two agree on the *magnitude of the
> feature*, which byte-identity alone would not establish.
>
> **The rectangle is the only non-transcription, and its interesting half is that
> the reprojected coordinate needs no conversion at all.** `GetShownCamera`
> pre-divides right and up by `tanx`/`tany`, and the kernels build rays with
> `sy = (2*(row+0.5)/h - 1) * tany`, so `dot(dir,ShU)/rtz` *is* that `sy/tany`
> and `(it+1)*0.5` collapses to `(row+0.5)/h` — precisely the normalised
> coordinate. GL scales it back into texels only because a rect sampler demands
> them. The same identity settles v with no appeal to a neighbouring arm: row 0
> is the kernel's row 0, the GL bottom, and `in.Position.y` is `gl_FragCoord.y`.
> Size comes from `get_width()`, never `Reproj1.w`/`2.w` — the divide and the
> clamp must share one source.
>
> **One deliberate divergence in form, exactly equal in value.** On the march
> path the C side calls `SetReprojUniforms(shaftw, shafth)` *unconditionally*,
> and those are 0/0 on a frame with no shaft buffer while `Reproj0.w` can still
> be 1. GL survives it (intensity forced to 0, a rect sample of an unbound name
> is a finite zero); Metal would not — `get_width()` is 0, an unfloored divide
> gives NaN, and NaN times zero is NaN. Hence the `max(size,1)` floor and
> skipping the fetch when the intensity is 0.
>
> **THE INSTRUMENT MATTERED MORE THAN THE PORT.** The murk reads
> `RT_Metal_GetShownCamera`, which `RT_METAL_REPROJ_TEST` never touched — 5-4's
> hook lives in `GetReprojection` — so its remap is an exact identity on a parked
> camera and any parity number taken there describes two shaders agreeing about
> doing nothing. The hook now covers both accessors and gained a **mode 2**,
> because mode 1 could not have caught a v error either: two degrees of yaw moves
> only the horizontal coordinate. Mode 1 is byte-unchanged, so 5-4's numbers
> reproduce.
>
> **Mode 2 immediately found a 5-4 defect** — see the correction in the 5-4 block
> above. It is fixed in `5311093b`, separately and with its own gate.
>
> Also: `tests/smoke.sh` asserted on the exact stderr line 6-3b deletes, so it is
> replaced by its positive twin (the kernel must report ACTIVE on Metal) plus an
> `absent` on the retired string — the half that catches a revert, since the
> fallback is silent by design. Smoke is **84 checks**.
>
> **Still owed by Phase 6:** 6-4 and 6-5 — both DONE below.

> **Status, 2026-08-08: PHASE 6 IS COMPLETE.** 6-4 (`10d31106`) crossed the lava
> boil and closed the remaining gates; 6-5 (`31be8ee0`) put the volumetrics back
> into the look A/B, so Seb's whole picture is now measured on both backends.
>
> **6-4 — and the gate was never about Metal.** `SHADERSTATICPARM_LAVA` was
> tested `vid.renderpath == RENDERPATH_GL32`, which reads as a Metal exclusion
> and was not one: the line was written 2026-08-01, three days before
> `RENDERPATH_METAL` existed, and its own comment names the real subject —
> `dp_texture3D` needs the GLSL130 path, i.e. it is **GLES2** that cannot have
> it. Adding a third renderpath to the enum silently swept Metal into an
> exclusion aimed at something else. **The hazard is the spelling**: "not GLES2"
> written as "is GL32" is correct until the enum grows, and then it is wrong
> silently and in the direction nobody checks. Worth a sweep wherever else a
> renderpath is tested by equality.
>
> New `lavaboil` vantage (e1m7, no RT and no murk, so the surface arm is the only
> thing under test): **0.0008 / max 1, both controls 0**. Isolation on each
> backend separately: `r_lavaboil` 0.6 against 0 moves **1619 px at mean 0.0034 /
> max 24 on GL and exactly the same 1619 px, mean and max, on Metal**, block map
> confined to the lava sheet's silhouette. And it is an **exact no-op everywhere
> else**, which is the half that could have gone wrong quietly — USELAVA is on by
> default, so this compiles the block into every Metal surface permutation on
> every map: measured against the pre-6-4 binary on lava-free e1m3, `spawn`,
> `hall` and `ogre` are byte-identical on both backends, 0 px each.
>
> Two documentation-only outcomes. `r_redglow` needed no work — implemented in
> the MSL since Phase 4 — but `shader_msl.h`'s header claimed both it and USELAVA
> were omitted and that `loc_RedGlow` came back −1 on Metal, wrong about half its
> own subject for two phases. And the `USEVOLUMETRICDEBUG` refusal's stated
> reason had **expired**: it cited 3D textures being scheduled for Phase 6, which
> 6-1 delivered. Its standing reason is recorded instead — console-only
> diagnostic, debugging happens on GL, and a debug view has no correct picture to
> compare against, so it is the one shader where a wrong port would be invisible.
> **A refusal whose reason has expired is worse than no comment: it reads as an
> oversight and invites someone to "finish" it.**
>
> **6-5 — the whole picture.** Every vantage at his own configuration, fog kernel
> ACTIVE on both backends, controls clean at zero:
>
> | vantage | cross | px differing of 307200 |
> |---|---|---|
> | `rt_spawn` | 0.0000 / max 1 | 7 (0.002%) |
> | `rt_wall` | 0.0000 / max 1 | 42 (0.014%) |
> | `rt_ogre` | 0.0000 / max 1 | 41 (0.013%) |
> | `rt_liquid` | 0.0000 / max 1 | 6 (0.002%) |
> | `rt_lava` | 0.0000 / max 1 | 1 (0.000%) |
>
> **Re-taken with range in the frame, because on their own those would flatter.**
> 5-6 recorded that his gamma puts the whole readback in the bottom few levels —
> these sit at mean 2.64 — so there is little signal for two renderers to
> disagree about. The identical configuration with the gamma trio neutralised
> gives mean 54.79 / p99 118, and the cross holds at **0.0004 / max 1 over 0.126%
> of the frame**. The dark result is not hiding anything.
>
> `rt_metal_shafts` is excluded and that is not an omission: it is **absent from
> his config**, so it defaults to 0, and `rt_metal_fog 1` supersedes that tier
> anyway. His saved `rt_metal_shafts_*` values are inert twice over — worth
> telling him, since he tuned them.

**Reference-config note — restate in every hand-over until Phase 5.** The Metal path cannot
run `rt_metal` until Phase 5, and Seb's `rt_metal_walllight 0.8` means his GL frame is
RT-lit fullbright, not lightmapped. Parity for Phases 3–4 is judged against a GL run with
`rt_metal 0 r_volumetric 0 r_bloom 0` — NOT his config. **Phase 5 is the first milestone he
can judge against the thing he actually plays.** Before that, his QA is one console line:
does GL still look identical.

*Updated at 5-3: `rt_metal` now runs on the Metal path, and `rt_metal_walllight 0.8` is
measured there (the `rt_wall` vantage). Two things still differ from his config and both are
scheduled: no reprojection until 5-4, so fast turns lag on the preview; and no volumetric
fog until Phase 6, which his `r_volumetric 1` + `rt_metal_fog 1` would otherwise supply. The
full look A/B against his own config is 5-6's, not 5-3's.*

| # | Milestone | Sessions | Acceptance |
|---|---|---|---|
| 0 | Seam, window, clear, one triangle | 2–3 | `RENDERPATH_METAL` + all switch sites triaged; `vid_renderer` + restart round-trip; CAMetalLayer up; clear + present + one triangle through the real pipeline cache; resize/fullscreen clean; sidecar device injectable; makefile + Xcode integration; sv-release unbroken. **GL byte-identical to HEAD on demo5 f120/f3000** (two-boot control first) |
| 1 | Instruments | 1 | Command-digest gate; the parity harness below; noise floors measured on both; nothing renders differently |
| 2 | Shader body split | 1 | `r_glsl_dumpshader` byte-identical for every live permutation; no MSL yet |
| 3 | 2D parity — console, menus, HUD | 3–4 | Textures (palette/RGBA/alpha, picmip, partial updates); state/pipeline/encoder/draw; VERTEXCOLOR + ALPHAGENVERTEX (the only combination the live 2D path reaches) and GENERIC (the gamma-ramp and bloom blits); constant-colour layout. **max Δ ≤ 2, ≥99.9% of pixels Δ ≤ 1 vs GL** — 2D is nearest-filtered quads, a hard gate is correct. Unported shader arms emit a magenta sentinel, never black |
| 4a | Opaque world | 3–4 | LIGHTMAP + FLATCOLOR + DEPTH_OR_SHADOW; depth buffer; render-target pool (framebuffer objects and `R_Mesh_CopyToTexture`, both refused by name in slice 3); the mirrored projection; the scissored-clear handling. e1m3 static vantage: mean ≤ 0.3, p99.9 ≤ 4, <0.5% pixels > 8 |
| 4b | Entities + models | 2–3 | Alias/sprite/skeletal, animcache, skeletal buffer, LIGHTDIRECTION, colormapping; same tolerance, monster vantage; `v_flipped` handedness check |
| 4c | Sky, transparents, particles, dlights | 2–3 | meshqueue, particles, explosions, lightning, coronas, LIGHTSOURCE, decals/stains; same tolerance |
| 4d | Postprocess + frame tail | 1–2 | POSTPROCESS, bloom chain, gamma LUT, viewblend, FXAA, hdr shoulder, screenshots; `r_viewfbo 0/1/2/3` all correct |
| 5 | **Native RT** | 3–4 | Shared device/queue; outputs as `MTLTexture`s; composite via the backend; bridge compiled out on Metal; liquids/lightcores/lavaemissive live. **First A/B against Seb's real config**; frame mean within 1%, diffs confined to expected regions |
| 6 | Fork passes | 3–4 | **DONE 2026-08-08.** VOLUMETRICFOG (GL-march fallback AND kernel-fed composite), redglow, lava boil; four murk vantages plus `lavaboil` in the harness; the whole-config look A/B restored. *The row's "e1m4 lake kernel-vs-GL A/B" was not run and is superseded rather than skipped: e1m4 is excluded from the bed because its camera angles do not apply there (4c), and what that A/B was for — showing the kernel and the GL march agree — is what `murk_kernel` measures directly, at byte-identity, on a map whose camera works. "murk menu pages" needed no work: the menu is renderpath-agnostic C.* |
| 7 | EDR | 2–3 | **DONE 2026-08-08.** 7-1 `r_gamma_analytic` (max 0.500 8-bit levels against the LUT — the rounding floor), the screenshot readback DECIDED as 8-bit and tone-mapped, 7-2 the readback's own stride + `r_metal_readbackprobe`, 7-3 the shoulder and ceiling take a headroom (byte-identical over 14 captures, liveness proven by two breaks moving identically on both backends), 7-4 the display speaks, 7-5 the layer asks, 7-6 the headroom reaches the curve. **Acceptance met: `r_edr 0` byte-identical to Phase 4d; `r_edr` on vs off with the headroom pinned differs by max delta 1, mean 0.0090, 0 px above 1 — the dequantisation signature and nothing else, so the 2D-escape hazard did not fire; GL 0 of 307200 px, r_edr being renderpath-gated.** Live: drawable RGBA16Float, max 1.756 over 516 px, EXACTLY the headroom NSScreen reported granting, against an 8-bit control at max 1.000 and 0 px. *The row's own premise needed correcting first: the OS grants headroom for the ASK, not the FORMAT — `wantsExtendedDynamicRangeContent` alone moves the grant 1.000 → 1.756 on an 8-bit drawable — so the grant is not evidence anything extended-range reached the screen.* |
| 8+ | Divergence | — | **PLAN APPROVED 2026-08-08 for the first two candidates — same-frame RT + MetalFX upscaling; see "Phase 8 — same-frame RT + MetalFX upscaling, spatial then TEMPORAL (THE PLAN)" above. 8-1, 8-2, 8-4, 8-5 and 8-6 are DONE — `r_metalfx` and `rt_metal_sameframe` are live with menu rows (Video page: Render Scale + MetalFX Upscale — the RT Shadows Same-Frame row was removed in the 2026-08-09 menu halving and the cvar is console-only); 8-3 stays deferred by his decision. **8-7, the TEMPORAL scaler, is open since 2026-08-18** — `r_metalfx` is 0/1/2 and the Video row is now a three-state cycle.** The base shipped 2026-08-08: `vid_renderer` defaults to `metal` on macOS (archived again — the escape hatch must persist), `VID_InitMode` falls back to GL when Metal init fails, and `r_edr` forces its own prerequisites (float scene buffer + analytic gamma, one predicate `R_EDR_Wanted`, five consumers), with Renderer and HDR rows on the Video menu. The demo11 headline bench (e3m1, Seb's config, interleaved): fullscreen 1080p240 GL 59.8/59.7 vs **Metal 64.0/64.9 avg (~8% faster), 1-second minimums equal-or-better for Metal in every period**, EDR cost below the noise floor; windowed pairs split inside a load-noise band far wider than the backend difference. Per-entity motion vectors (T2b), the light-fed reactive mask (T3r) and the particle-fed reactive stamp landed 2026-08-18/19 (see the 8-7 block); `r_metalfx 2` awaits Seb's in-motion verdict, and the tier decision (Best/Better/Good → temporal, Fast spatial) is held on it. Remaining candidates: the explosion shell and sprites as mask sources, upscaling BEFORE the postprocess (decision point 3, re-opened), texture compression, `cl_capturevideo`'s Metal arm, a 16-bit PNG extra path for EDR stills. |

Ordering rationale: instruments and the shader split land before any MSL exists (prevents
the three-way lockstep — non-negotiable); native RT comes **before** the fork's fog passes
(gives Seb a meaningful QA round a milestone sooner, and deletes the three `sampler2DRect`
uses so they are never ported at all).

## The cross-backend parity harness

The documented screenshot noise floor (same binary, e1m3: mean 1.62, 81% of pixels differ)
is animated torches, lightstyles and animation phase. The parity bed pins all of it:
`m5_photomode 1` plus `r_lerplightstyles 0`, `v_idlesway 0`, `cl_bob 0`, `cl_particles 0`,
`r_waterscroll 0`, `r_skyscroll1 0`, `r_skyscroll2 0`, `r_lavaboil 0`, the pinned
`cl_nettimesync*` pair, `vid_borderless 1`. Six fixed vantages (noclip +
`prvm_edictset server 1 origin`, per the documented trap): e1m3 spawn, e1m3 corridor, e1m4
lakeside `1092 1184 698`, e1m7 lava overlook, start.bsp hall, and menu/console. **Always
run the same-binary two-boot control first**; if it is not ≤1 LSB on ≥99.9% of pixels, fix
the bed before quoting any cross-backend number. Metrics: mean, p99.9, % > 8, per-channel
frame means, flat-white %, and a 16×16 block-mean map (systematic errors cluster in
blocks; rasterisation LSBs do not). Free counters: `r_stat_draws*` must be identical
across backends — if the batching differs, the pixels may be hiding it.

Smoke additions along the arc: analytic-gamma vs LUT (256 samples); prologue
uniform-name-set equality; `vid_renderer metal` headless 30-frame boot; the gl → metal → gl
restart round-trip.

## Risk register

- **Shader split changes the GL look** → dumpshader byte-identity is a total proof.
- **Uniform refactor changes GL behaviour** (167 sites) → the digest gate, not the pixel gate.
- **MSL becomes a third murk lockstep** → the split lands first; non-negotiable ordering.
- **Compile hitching worse than budgeted** → async + degrade + archive + load-time warm;
  `MTL4Compiler` in reserve.
- **Encoder state replay misses state** → single EnsureEncoder/ApplyState; the debug
  forced-restart mode asserting pixel identity.
- **The scissored clear wipes the HUD** on the no-FBO path (not Seb's config, so his QA
  cannot catch it) → explicit handling + the smoke check above.
- **EDR breaks the screenshot instrument** → decide the readback before any EDR code.
- **Phase 3–4 QA reads as a regression against Seb's config** → the reference-config note
  in every hand-over; RT pulled ahead of fog.
- **Scope** (23–32 sessions) → every milestone ends committable; pausing loses nothing.
