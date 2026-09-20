# Metal GPU ray-tracing groundwork (Phase 1 spikes)

Validation spikes for the native-Metal GPU ray-tracing direction (see the RT
review). These are standalone command-line probes — not yet wired into the
engine — that de-risk the roadmap's biggest unknowns before any renderer
integration. All PASS on Apple M5 / macOS 26.5.

Runtime shader compilation (`newLibraryWithSource:`) is used, so the separately
downloadable offline Metal Toolchain (`xcrun metal`) is **not required**.

## Probes

| file | proves |
|---|---|
| `probe0.m`  | Metal device bring-up, runtime shader compile, compute dispatch. Reports `supportsRaytracing`, `hasUnifiedMemory`. |
| `rtprobe.m` | Hardware ray tracing: build an `MTLAccelerationStructure` from a known triangle, intersect rays with hand-computed answers. |
| `rtparity.m`| Real-geometry parity: build an AS from exported DarkPlaces world triangles and compare Metal hits against a CPU Moller-Trumbore reference over the same triangles. |
| `sidecar.m` | In-process integration seam: bring up a GL 3.2 Core context the way the engine does, create an `MTLDevice` alongside it, have Metal write into a shared `IOSurface`, and verify the GL context consumes the exact result. De-risks the "Metal computes, GL composites" data path. |

## Build & run

    clang -fobjc-arc      -framework Foundation -framework Metal metal/probe0.m  -o metal/probe0  && ./metal/probe0
    clang -fobjc-arc      -framework Foundation -framework Metal metal/rtprobe.m -o metal/rtprobe && ./metal/rtprobe
    clang -O2 -fobjc-arc  -framework Foundation -framework Metal metal/rtparity.m -o metal/rtparity

The sidecar probe additionally links SDL2 (mirroring the engine's context) plus
QuartzCore/IOSurface/OpenGL:

    clang -O2 -fobjc-arc $(sdl2-config --cflags) \
      -framework Foundation -framework Metal -framework QuartzCore \
      -framework IOSurface -framework OpenGL -L/opt/homebrew/lib -lSDL2 \
      metal/sidecar.m -o metal/sidecar && ./metal/sidecar
    # SDL_VIDEODRIVER=dummy ./metal/sidecar   # forces the headless CGL fallback (CI / dedicated)

Export geometry from the engine (headless), then run the parity probe:

    make sv-release
    ./darkplaces-dedicated -userdir "$PWD/test/runtime" +map e1m1 +collision_exportworldtris worldtris-e1m1.txt +quit
    ./metal/rtparity "$PWD/test/runtime/id1/worldtris-e1m1.txt" 20000

## Phase-1 result (Apple M5, macOS 26.5.1)

- device = Apple M5, `supportsRaytracing = 1`, `hasUnifiedMemory = 1`.
- Hardware RT: correct hits (incl. back-facing rays).
- Real geometry (e1m1, 26702 verts / 15670 tris, map diagonal 4153): 20000 rays,
  **0 class-mismatches, 0 dist-mismatches, max hit-distance error 0.0005 units** —
  ~60x tighter than the engine's 0.03125 impactnudge. The float-AS-vs-geometry
  precision concern is retired at Quake map scale.
- Sidecar interop: an `MTLDevice` (Apple M5, `supportsRaytracing = 1`) is created
  and used in the **same process** as a live GL 3.2 Core context (`GL_VERSION`
  reports `4.1 Metal - 90.5`, exactly what the engine gets on macOS). Metal writes
  a deterministic image into a shared BGRA8 `IOSurface`; GL imports the *same*
  surface via `CGLTexImageIOSurface2D`, attaches it to an FBO, and reads back
  logical RGBA — **max channel diff 0** across 256², 640×360 and 1920×1080, on
  both the SDL and headless-CGL bring-up paths. The zero-copy "Metal computes, GL
  composites" data path is validated. (Sync here is `waitUntilCompleted`; see below.)

## Phase-2 result — first in-engine integration (Apple M5, macOS 26.5.1)

The sidecar is now wired into the engine itself (`rt_metal.m` / `rt_metal.h` at the
repo root, compiled into the client only on macOS — see `makefile.inc`). It is not
a standalone probe: an `MTLDevice` is created in `VID_InitModeGL` against the
engine's *real* live SDL GL 3.2 Core context, and each frame — when the `rt_metal`
cvar is on — a Metal compute kernel writes the deterministic pattern into a shared
`IOSurface` which is imported (`CGLTexImageIOSurface2D`) and composited over the
frame with `glBlitFramebuffer`, in `VID_Finish` just before the buffer swap.

Verified in-engine on `e1m1`: `RT_Metal: device 'Apple M5' ready`, and the
composited window backbuffer (read straight back from the swap chain) is a
**byte-exact, channel- and orientation-correct** reproduction of the kernel
pattern — corners black / magenta / cyan / **yellow** (yellow, not cyan, proves no
BGRA swap; row 0 at top proves no vertical flip). `rt_metal 0` never enters the
blit path, so the frame is untouched. No `CHECKGLERROR` trips across the run.

A blit (not the `R_BlendView` post-process quad) was chosen deliberately:
`R_BlendView` is skipped whenever post-processing is trivial (`gl_rmain.c`), feeds
normalised `GL_TEXTURE_2D` texcoords, and would need a forged `rtexture_t`; a direct
framebuffer blit always runs, does a logical colour copy (so BGRA→RGBA needs no
swizzle), and gives exact control of the vertical flip.

## Not yet done (next phases)

- Compare against the engine's *own* tracer output (edge-plane method + nudge +
  material filtering) as a belt-and-suspenders check — optional; the geometric
  parity above already bounds it.
- Sidecar sync hardening: everything so far uses a coarse `[cmdbuf waitUntilCompleted]`
  barrier. Production needs a fine-grained Metal↔GL sync (shared `MTLSharedEvent`
  / `GL_ARB_sync` fence) so the two queues pipeline instead of stalling per frame.
  **This is the immediate next spike** now that pixels are on screen.
- Runtime world BLAS built from live in-engine geometry (not the `worldtris` txt
  export), then a first RT AO/occlusion buffer written by the kernel and shown
  through the now-proven blit.
- Dynamic BLAS refit for animated casters; RT shadows into the ShadowMapCompare seam.
