/*
QuakeM5 -- MetalFX spatial upscaling (METAL.md Phase 8-4: scaffolding).

Plain C, in the metal_backend.h / metal_textures.h mould: no Objective-C or
MetalFX types cross this header, so the eventual caller -- gl_rmain.c's
R_RenderView gate at Phase 8-5, an OBJ_COMMON file that links into the
dedicated server and every non-Apple target -- stays a C translation unit.
The static-inline no-op stubs in the #else arm are mandatory for exactly that
reason; `make sv-release` has broken twice on this class of file, and the
stubs live HERE beside the prototypes rather than in a separate file (a
parallel list of names is the duplication shape this project keeps losing to).

WHAT 8-4 PROVIDES AND WHAT IT DOES NOT. Scaffolding only: one probe per
backend start that asks the device whether a MetalFX spatial scaler exists at
all, and if so which MTLTextureUsage bits the scaler demands of the textures
the app supplies (the header makes the APP responsible for satisfying them).
No scaler is cached, nothing is encoded, and there is deliberately NO
`r_metalfx` cvar in this slice -- a registered cvar that cannot move a pixel
is the dead-slider defect class CLAUDE.md records; the cvar lands at 8-5 with
its behaviour. The one consumer today is metal_backend.m, whose screen
texture gains the queried output-usage bits UNCONDITIONALLY, so a mid-session
enable at 8-5 never depends on the recreate predicate noticing a cvar.

The usage getters return MTLTextureUsage bit sets carried as plain unsigned
int, and both return 0 when MetalFX is unavailable -- so OR-ing them into a
texture descriptor is exactly a no-op wherever the scaler is absent.
*/

#ifndef METAL_FX_H
#define METAL_FX_H

#include "qtypes.h"

#ifdef USE_METAL_RENDERER

/// Probe the device vid_metal.m owns for spatial-scaler support and cache the
/// scaler's texture-usage requirements. Called from Metal_Backend_Start once
/// the device is known good; prints one boot line either way. Safe to call
/// twice -- a second call re-probes, so a vid_restart can never serve stale
/// answers for a new device.
void MetalFX_Start(void);

/// Forget the probe results. Safe when never started.
void MetalFX_Shutdown(void);

/// True when the device supports spatial scaling AND the factory actually
/// produced a scaler -- a nil factory on a "supported" device is treated as
/// unavailable rather than trusted.
qbool MetalFX_Available(void);

/// The minimal MTLTextureUsage bits the scaler requires on its INPUT colour
/// texture. 0 when unavailable.
unsigned int MetalFX_ColorTextureUsage(void);

/// The minimal MTLTextureUsage bits the scaler requires on its OUTPUT
/// texture. 0 when unavailable.
unsigned int MetalFX_OutputTextureUsage(void);

/// The TEMPORAL scaler's queried outputTextureUsage. Phase 8-4 gave the screen
/// texture the SPATIAL scaler's bits only, which is enough while the screen
/// texture is the sole destination -- but the temporal scaler asks for more
/// (0x7 against 0x5 on this hardware: it wants ShaderWrite), and r_fxaa_post
/// hands it an ordinary pooled render target instead. Without these bits the
/// encode REFUSES, and the refusal is silent in the picture: the frame falls
/// back to a correct, merely un-antialiased path.
unsigned int MetalFX_TemporalOutputTextureUsage(void);

/// METAL.md Phase 8-5: make sure the one-slot scaler cache holds a usable
/// scaler for exactly this key, minting one if the key changed. `intextype`
/// is the pooled intermediate's engine textype (TEXTYPE_COLORBUFFER /
/// 16F / 32F); `hdr` selects the extended-range output format and the HDR
/// colour-processing mode together, by the same expression that formats the
/// screen texture. Returns false -- and remembers the refusal per key, so the
/// factory is never retried per frame -- whenever MetalFX is absent, the
/// textype is unsupported, or the factory declines. The caller's gate treats
/// false as "fall back to the fused bilinear path this frame".
qbool MetalFX_ScalerReady(int inwidth, int inheight, int outwidth, int outheight, int intextype, qbool hdr);

/// Encode the cached scaler into `cmdbuf`, upscaling `srctex` into `dsttex`
/// (all three are __bridge'd Metal objects, the vid_metal.h void* convention).
/// The caller must have ended any open render encoder on the command buffer
/// first -- two open encoders on one buffer is a validation abort. Refuses
/// (loudly, once) rather than encoding if the textures do not match the
/// cached scaler's requirements.
qbool MetalFX_EncodeUpscale(void *cmdbuf, void *srctex, void *dsttex);

// --- the MetalFX-TEMPORAL arc ----------------------------------------------
// A second, independent one-slot cache in the same shape. Temporal scaling is
// not a variant of spatial: it takes depth and motion as well as colour, it
// carries per-frame state (a jitter offset and a reset flag), and its
// descriptor has NO colorProcessingMode at all -- so the 8-5 HDR/Perceptual
// pairing has no analogue here and the EDR behaviour is a measured question
// rather than a configured one.

/// True when the device supports TEMPORAL scaling and the factory produced a
/// scaler at boot. Independent of MetalFX_Available().
qbool MetalFX_TemporalAvailable(void);

/// The input-content scale range the device reports for temporal scaling
/// (output/input). Written only when temporal scaling is available; the gate
/// uses it to decide whether 1:1 -- TAA with no upscale, which is the whole
/// point on the top tier -- is a configuration this device will accept.
void MetalFX_TemporalScaleRange(float *outmin, float *outmax);

/// Make sure the temporal cache holds a scaler for exactly this key, minting
/// one if the key changed, with the same per-key failure memoisation as the
/// spatial cache. Same textype/hdr contract as MetalFX_ScalerReady.
qbool MetalFX_TemporalReady(int inwidth, int inheight, int outwidth, int outheight, int intextype, qbool hdr);

/// Encode the cached temporal scaler. `depthtex` and `motiontex` are this
/// frame's render-resolution scene depth and motion-vector textures;
/// `jitterx`/`jittery` are the sub-pixel offset the raster was jittered by,
/// in render pixels, in the sense MetalFX expects; `reset` discards the
/// accumulated history (map load, teleport, a frame the path did not run).
/// Same encoder and refusal contract as MetalFX_EncodeUpscale.
qbool MetalFX_EncodeTemporalUpscale(void *cmdbuf, void *srctex, void *depthtex, void *motiontex, void *reactivetex, void *dsttex,
                                    float jitterx, float jittery, qbool reset);

#else // !USE_METAL_RENDERER

static inline void MetalFX_Start(void) {}
static inline void MetalFX_Shutdown(void) {}
static inline qbool MetalFX_Available(void) { return false; }
static inline unsigned int MetalFX_ColorTextureUsage(void) { return 0; }
static inline unsigned int MetalFX_OutputTextureUsage(void) { return 0; }
static inline unsigned int MetalFX_TemporalOutputTextureUsage(void) { return 0; }
static inline qbool MetalFX_ScalerReady(int inwidth, int inheight, int outwidth, int outheight, int intextype, qbool hdr) { (void)inwidth; (void)inheight; (void)outwidth; (void)outheight; (void)intextype; (void)hdr; return false; }
static inline qbool MetalFX_EncodeUpscale(void *cmdbuf, void *srctex, void *dsttex) { (void)cmdbuf; (void)srctex; (void)dsttex; return false; }
static inline qbool MetalFX_TemporalAvailable(void) { return false; }
static inline void MetalFX_TemporalScaleRange(float *outmin, float *outmax) { if (outmin) *outmin = 0; if (outmax) *outmax = 0; }
static inline qbool MetalFX_TemporalReady(int inwidth, int inheight, int outwidth, int outheight, int intextype, qbool hdr) { (void)inwidth; (void)inheight; (void)outwidth; (void)outheight; (void)intextype; (void)hdr; return false; }
static inline qbool MetalFX_EncodeTemporalUpscale(void *cmdbuf, void *srctex, void *depthtex, void *motiontex, void *reactivetex, void *dsttex, float jitterx, float jittery, qbool reset) { (void)cmdbuf; (void)srctex; (void)depthtex; (void)motiontex; (void)reactivetex; (void)dsttex; (void)jitterx; (void)jittery; (void)reset; return false; }

#endif // USE_METAL_RENDERER

#endif
