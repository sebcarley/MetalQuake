/*
QuakeM5 -- MetalFX spatial upscaling (METAL.md Phase 8-4: scaffolding). See
metal_fx.h for what this slice deliberately does and does not provide.

This file owns the probe: one descriptor-and-factory round trip per backend
start, against the device vid_metal.m owns, to learn (a) whether a spatial
scaler exists on this device at all and (b) which MTLTextureUsage bits it
demands of the textures the app supplies. MTLFXSpatialScaler.h puts the
responsibility for those bits on the APP ("the minimum set of MTLTextureUsage
bits that you are responsible for setting in your texture descriptors"), and
its output texture must additionally be private storage -- which
metal_backend.m's screen texture already is.

The probe scaler itself is released as soon as the two usage words are read:
8-5 owns the real keyed scaler cache, and keeping a scaler alive here would
be a second owner for it to fight. The probe's dimensions and formats are a
fixed representative pair (720p -> 1080p, BGRA8Unorm both sides -- the
non-EDR screen format and a mid-range r_viewscale); the usage requirements
are properties of the scaler implementation rather than of any particular
size, and 8-5's real scaler re-asserts them at its own creation, so a
format-dependent surprise cannot ship silently on this probe's word alone.

Compiled only on macOS (makefile.inc gates the object, as for rt_metal.o and
the other Metal objects -- with an explicit make rule, because GNU make's
built-in %.o: %.m rule would silently compile without ARC). ARC is on.
*/

#if defined(__APPLE__) && defined(USE_METAL_RENDERER)

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

#include "quakedef.h"
#include "r_textures.h"
#include "metal_fx.h"
#include "vid_metal.h"

static qbool        mfx_available;
static unsigned int mfx_colorusage;
static unsigned int mfx_outputusage;

// METAL.md Phase 8-5: the one-slot keyed scaler cache. A spatial scaler is
// immutable in its dimensions, formats and colour-processing mode, so a new
// key releases the old scaler and mints a new one -- the mb_screentex
// recreate precedent. One slot suffices because exactly one configuration is
// live at a time (window size x r_viewscale x EDR state); a resize or a
// viewscale change is a one-off re-mint, not a per-frame cost. A key whose
// factory FAILED is remembered too, or the gate would pay the factory cost
// again every frame of a configuration MetalFX refuses (r_viewfbo 3's
// RGBA32Float input is the live example).
// The MetalFX-TEMPORAL arc: a second one-slot cache in the same shape. Kept
// entirely separate from the spatial one -- both can be minted in a session
// (the cvar is a three-state cycle and a player may A/B them), and a temporal
// scaler carries accumulated history that a shared slot would silently drop.
static qbool        mfx_tavailable;
static unsigned int mfx_tcolorusage, mfx_tdepthusage, mfx_tmotionusage, mfx_toutputusage;
static float        mfx_tminscale, mfx_tmaxscale;
static id<MTLFXTemporalScaler> mfx_tscaler;
static int   mfx_tkey_inw, mfx_tkey_inh, mfx_tkey_outw, mfx_tkey_outh;
static int   mfx_tkey_textype;
static qbool mfx_tkey_hdr;
static qbool mfx_tkey_failed;
static qbool mfx_tencode_warned;

static id<MTLFXSpatialScaler> mfx_scaler;
static int   mfx_key_inw, mfx_key_inh, mfx_key_outw, mfx_key_outh;
static int   mfx_key_textype;
static qbool mfx_key_hdr;
static qbool mfx_key_failed;
static qbool mfx_encode_warned;

static void mfx_release_scaler(void)
{
	mfx_scaler = nil;
	mfx_key_inw = mfx_key_inh = mfx_key_outw = mfx_key_outh = 0;
	mfx_key_textype = 0;
	mfx_key_hdr = false;
	mfx_key_failed = false;
	mfx_encode_warned = false;
}

static void mfx_release_temporal(void)
{
	mfx_tscaler = nil;
	mfx_tkey_inw = mfx_tkey_inh = mfx_tkey_outw = mfx_tkey_outh = 0;
	mfx_tkey_textype = 0;
	mfx_tkey_hdr = false;
	mfx_tkey_failed = false;
	mfx_tencode_warned = false;
}

// The temporal half of the boot probe. Its own descriptor round trip, because
// the four usage words and the supported scale range are properties of the
// TEMPORAL scaler and nothing about the spatial one predicts them. The probe
// scaler is released the moment they are read, for the reason the spatial
// probe is: 8-5's cache is the single owner and a second one would fight it.
static void mfx_probe_temporal(id<MTLDevice> dev)
{
	MTLFXTemporalScalerDescriptor *td;
	id<MTLFXTemporalScaler> scaler;

	if (![MTLFXTemporalScalerDescriptor supportsDevice:dev])
	{
		Con_Printf("MetalFX: temporal scaling not supported on this device\n");
		return;
	}
	// Reported by the DEVICE rather than by a scaler, and asked for first
	// because it decides whether 1:1 -- temporal as pure anti-aliasing with no
	// resolution loss, which is the configuration the top tier wants -- is a
	// key this device will ever accept.
	mfx_tminscale = [MTLFXTemporalScalerDescriptor supportedInputContentMinScaleForDevice:dev];
	mfx_tmaxscale = [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:dev];

	td = [[MTLFXTemporalScalerDescriptor alloc] init];
	td.inputWidth   = 1280;
	td.inputHeight  = 720;
	td.outputWidth  = 1920;
	td.outputHeight = 1080;
	td.colorTextureFormat  = MTLPixelFormatBGRA8Unorm;
	td.depthTextureFormat  = MTLPixelFormatDepth32Float;
	td.motionTextureFormat = MTLPixelFormatRGBA16Float;
	td.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
	scaler = [td newTemporalScalerWithDevice:dev];
	if (!scaler)
	{
		Con_Printf(CON_WARN "MetalFX: supportsDevice says yes for temporal but the factory returned nil; treating temporal scaling as unavailable\n");
		return;
	}
	mfx_tcolorusage  = (unsigned int)scaler.colorTextureUsage;
	mfx_tdepthusage  = (unsigned int)scaler.depthTextureUsage;
	mfx_tmotionusage = (unsigned int)scaler.motionTextureUsage;
	mfx_toutputusage = (unsigned int)scaler.outputTextureUsage;
	mfx_tavailable   = true;
	scaler = nil;

	// 2026-09-18: probe the FLOAT output format too, and take the UNION. The
	// probe above asks with BGRA8, but the live scaler runs RGBA16Float
	// whenever the scene buffer is float (r_edr, and r_metalfx 2 forces it), and
	// a scaler's outputTextureUsage is a property of the INSTANCE -- this file
	// already carries a runtime check saying so. That mattered not at all while
	// the screen texture was the only destination, because Phase 8-4 gives it
	// these bits unconditionally; r_fxaa_post makes an ordinary pooled render
	// target a destination, and its usage is fixed when the pool creates it.
	// Without the union the float-format scaler refuses that target, and the
	// caller then degrades to no post-AA for the whole session.
	td.outputTextureFormat = MTLPixelFormatRGBA16Float;
	scaler = [td newTemporalScalerWithDevice:dev];
	if (scaler)
	{
		mfx_tcolorusage  |= (unsigned int)scaler.colorTextureUsage;
		mfx_tdepthusage  |= (unsigned int)scaler.depthTextureUsage;
		mfx_tmotionusage |= (unsigned int)scaler.motionTextureUsage;
		mfx_toutputusage |= (unsigned int)scaler.outputTextureUsage;
		scaler = nil;
	}

	Con_Printf("MetalFX: temporal scaler available (usage colour 0x%x depth 0x%x motion 0x%x output 0x%x, input scale %.3f-%.3f)\n",
		mfx_tcolorusage, mfx_tdepthusage, mfx_tmotionusage, mfx_toutputusage, mfx_tminscale, mfx_tmaxscale);
	// Colour, depth and motion all come from R_RenderTarget_Get, whose Metal
	// arm gives every render target RenderTarget|ShaderRead
	// (metal_textures.m's mt_isrendertarget). Checked at runtime once per
	// start rather than assumed, exactly as the spatial probe does.
	if (((mfx_tcolorusage | mfx_tdepthusage | mfx_tmotionusage) &
	     ~(unsigned int)(MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead)) != 0)
		Con_Printf(CON_WARN "MetalFX: the pooled render-target usage (RenderTarget|ShaderRead) does NOT cover the temporal scaler's input requirements (colour 0x%x depth 0x%x motion 0x%x)\n",
			mfx_tcolorusage, mfx_tdepthusage, mfx_tmotionusage);
}

void MetalFX_Start(void)
{
	id<MTLDevice> dev;
	MTLFXSpatialScalerDescriptor *sd;
	id<MTLFXSpatialScaler> scaler;

	MetalFX_Shutdown();
	dev = (__bridge id<MTLDevice>)VID_Metal_GetDevice();
	if (!dev)
		return;   // Metal_Backend_Start has already said why, loudly
	// Probed FIRST and unconditionally: temporal availability is independent
	// of spatial, and every early return below is a spatial verdict only.
	mfx_probe_temporal(dev);
	if (![MTLFXSpatialScalerDescriptor supportsDevice:dev])
	{
		Con_Printf("MetalFX: spatial scaling not supported on this device\n");
		return;
	}
	sd = [[MTLFXSpatialScalerDescriptor alloc] init];
	sd.inputWidth   = 1280;
	sd.inputHeight  = 720;
	sd.outputWidth  = 1920;
	sd.outputHeight = 1080;
	sd.colorTextureFormat  = MTLPixelFormatBGRA8Unorm;
	sd.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
	// colorProcessingMode stays the default (Perceptual) -- 8-5's own choice
	// for the 8-bit path, since post-gamma engine output IS sRGB perceptual.
	scaler = [sd newSpatialScalerWithDevice:dev];
	if (!scaler)
	{
		// The nil-factory guard: supportsDevice said yes and the factory
		// still refused. Treated as unavailable rather than trusted.
		Con_Printf(CON_WARN "MetalFX: supportsDevice says yes but the scaler factory returned nil; treating spatial scaling as unavailable\n");
		return;
	}
	mfx_colorusage  = (unsigned int)scaler.colorTextureUsage;
	mfx_outputusage = (unsigned int)scaler.outputTextureUsage;
	mfx_available   = true;
	scaler = nil;   // the probe's whole yield is the two usage words

	Con_Printf("MetalFX: spatial scaler available (colorTextureUsage 0x%x, outputTextureUsage 0x%x)\n", mfx_colorusage, mfx_outputusage);
	// 8-5's input is a pooled render target, and metal_textures.m gives those
	// RenderTarget|ShaderRead (its mt_isrendertarget arm -- the same pair the
	// screen texture uses). The plan judged that "probably sufficient";
	// verified here at runtime rather than assumed, once per start.
	if ((mfx_colorusage & ~(unsigned int)(MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead)) != 0)
		Con_Printf(CON_WARN "MetalFX: the pooled render-target usage (RenderTarget|ShaderRead) does NOT cover colorTextureUsage 0x%x; 8-5 must widen metal_textures.m's render-target arm\n", mfx_colorusage);
}

void MetalFX_Shutdown(void)
{
	mfx_release_scaler();
	mfx_release_temporal();
	mfx_available   = false;
	mfx_colorusage  = 0;
	mfx_outputusage = 0;
	mfx_tavailable    = false;
	mfx_tcolorusage   = 0;
	mfx_tdepthusage   = 0;
	mfx_tmotionusage  = 0;
	mfx_toutputusage  = 0;
	mfx_tminscale     = 0.0f;
	mfx_tmaxscale     = 0.0f;
}

qbool MetalFX_ScalerReady(int inwidth, int inheight, int outwidth, int outheight, int intextype, qbool hdr)
{
	id<MTLDevice> dev;
	MTLFXSpatialScalerDescriptor *sd;
	MTLPixelFormat informat, outformat;

	if (!mfx_available)
		return false;
	if (inwidth  == mfx_key_inw  && inheight  == mfx_key_inh &&
	    outwidth == mfx_key_outw && outheight == mfx_key_outh &&
	    intextype == mfx_key_textype && hdr == mfx_key_hdr)
		return mfx_scaler != nil && !mfx_key_failed;

	// a new key: release the old scaler and record the key BEFORE the factory
	// runs, so a failure is memoised against it and never retried per frame
	mfx_release_scaler();
	mfx_key_inw = inwidth;  mfx_key_inh = inheight;
	mfx_key_outw = outwidth; mfx_key_outh = outheight;
	mfx_key_textype = intextype;
	mfx_key_hdr = hdr;
	mfx_key_failed = true;

	// the input format follows the pooled intermediate's textype, which is
	// r_fb.rt_screen's own (LOCKSTEP with mt_pixelformat in metal_textures.m
	// for these three cases -- a drift here is caught by the format check in
	// MetalFX_EncodeUpscale against the real texture object)
	switch (intextype)
	{
	case TEXTYPE_COLORBUFFER:    informat = MTLPixelFormatBGRA8Unorm;  break;
	case TEXTYPE_COLORBUFFER16F: informat = MTLPixelFormatRGBA16Float; break;
	case TEXTYPE_COLORBUFFER32F: informat = MTLPixelFormatRGBA32Float; break;
	default:
		Con_Printf(CON_WARN "MetalFX: unsupported input textype %d; falling back to the bilinear path\n", intextype);
		return false;
	}
	// the output format is mb_screentex's, by the same expression that sizes
	// it (vid.edr_active); the mode is the header's own pairing -- perceptual
	// for sRGB-encoded 8-bit output, reversible-tone-mapped HDR for the
	// extended-range float drawable. On the one frame of an EDR toggle edge
	// the two sides can disagree (rt_screen re-types a frame before the
	// drawable does); that frame gets a mixed-format scaler, which is valid,
	// and the next frame's key re-mints.
	outformat = hdr ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm;

	dev = (__bridge id<MTLDevice>)VID_Metal_GetDevice();
	if (!dev)
		return false;
	sd = [[MTLFXSpatialScalerDescriptor alloc] init];
	sd.inputWidth   = (NSUInteger)inwidth;
	sd.inputHeight  = (NSUInteger)inheight;
	sd.outputWidth  = (NSUInteger)outwidth;
	sd.outputHeight = (NSUInteger)outheight;
	sd.colorTextureFormat  = informat;
	sd.outputTextureFormat = outformat;
	// HDR processing is keyed on the INPUT being a float format as well as on
	// EDR: on the one frame of an EDR-off toggle edge the input is still the
	// 8-bit scene buffer -- display-encoded sRGB that cannot exceed 1.0 --
	// and reversible tone mapping is the wrong regime for it (8-5 review D4).
	// Perceptual into a float output is valid; the formats are checked
	// separately.
	sd.colorProcessingMode = (hdr && informat != MTLPixelFormatBGRA8Unorm)
	                             ? MTLFXSpatialScalerColorProcessingModeHDR
	                             : MTLFXSpatialScalerColorProcessingModePerceptual;
	mfx_scaler = [sd newSpatialScalerWithDevice:dev];
	if (!mfx_scaler)
	{
		Con_Printf(CON_WARN "MetalFX: scaler factory refused %dx%d -> %dx%d (textype %d%s); falling back to the bilinear path\n",
			inwidth, inheight, outwidth, outheight, intextype, hdr ? ", hdr" : "");
		return false;
	}
	// the input is a pooled render target, whose usage is
	// RenderTarget|ShaderRead (metal_textures.m's mt_isrendertarget arm) --
	// refuse the key if THIS scaler demands more, rather than dying in
	// validation at encode time
	if (((unsigned int)mfx_scaler.colorTextureUsage & ~(unsigned int)(MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead)) != 0)
	{
		Con_Printf(CON_ERROR "MetalFX: this scaler's colorTextureUsage 0x%x exceeds the pooled render-target usage; widen metal_textures.m's render-target arm\n",
			(unsigned int)mfx_scaler.colorTextureUsage);
		mfx_scaler = nil;
		return false;
	}
	// the output side of the same check (8-5 review D3): mb_screentex's usage
	// is RenderTarget|ShaderRead plus the BOOT PROBE's outputTextureUsage --
	// the probe was one fixed configuration, and nothing guarantees THIS
	// key's scaler asks for no more. Refusing here makes an escalated
	// requirement a graceful gate refusal (bilinear fallback, once per key)
	// instead of an encode-time failure the gate can no longer route around.
	if (((unsigned int)mfx_scaler.outputTextureUsage & ~((unsigned int)(MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead) | mfx_outputusage)) != 0)
	{
		Con_Printf(CON_ERROR "MetalFX: this scaler's outputTextureUsage 0x%x exceeds the screen texture's usage; the boot probe queried 0x%x\n",
			(unsigned int)mfx_scaler.outputTextureUsage, mfx_outputusage);
		mfx_scaler = nil;
		return false;
	}
	mfx_key_failed = false;
	// once per key change by construction -- the resize / viewscale / EDR
	// rate, never per frame (the 92 GB console lesson)
	Con_Printf("MetalFX: spatial scaler %dx%d -> %dx%d (%s, %s)\n",
		inwidth, inheight, outwidth, outheight,
		informat == MTLPixelFormatBGRA8Unorm ? "BGRA8" : (informat == MTLPixelFormatRGBA16Float ? "RGBA16F" : "RGBA32F"),
		hdr ? "hdr" : "perceptual");
	return true;
}

qbool MetalFX_EncodeUpscale(void *cmdbuf, void *srctex, void *dsttex)
{
	id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)cmdbuf;
	id<MTLTexture> src = (__bridge id<MTLTexture>)srctex;
	id<MTLTexture> dst = (__bridge id<MTLTexture>)dsttex;

	if (!mfx_scaler || mfx_key_failed || !cb || !src || !dst)
		return false;
	// The caller's gate ran ScalerReady this frame, so every mismatch below is
	// a programming error, not a configuration; each is refused loudly ONCE
	// rather than fed to the validation layer's abort.
	if (src.width  != (NSUInteger)mfx_key_inw  || src.height != (NSUInteger)mfx_key_inh ||
	    dst.width  != (NSUInteger)mfx_key_outw || dst.height != (NSUInteger)mfx_key_outh ||
	    src.pixelFormat != mfx_scaler.colorTextureFormat ||
	    dst.pixelFormat != mfx_scaler.outputTextureFormat ||
	    (src.usage & mfx_scaler.colorTextureUsage)  != mfx_scaler.colorTextureUsage ||
	    (dst.usage & mfx_scaler.outputTextureUsage) != mfx_scaler.outputTextureUsage ||
	    dst.storageMode != MTLStorageModePrivate)
	{
		if (!mfx_encode_warned)
		{
			mfx_encode_warned = true;
			Con_Printf(CON_ERROR "MetalFX: encode refused -- src %ux%u fmt %u usage 0x%x, dst %ux%u fmt %u usage 0x%x storage %u against key %dx%d -> %dx%d\n",
				(unsigned)src.width, (unsigned)src.height, (unsigned)src.pixelFormat, (unsigned)src.usage,
				(unsigned)dst.width, (unsigned)dst.height, (unsigned)dst.pixelFormat, (unsigned)dst.usage, (unsigned)dst.storageMode,
				mfx_key_inw, mfx_key_inh, mfx_key_outw, mfx_key_outh);
		}
		return false;
	}
	mfx_scaler.colorTexture = src;
	mfx_scaler.inputContentWidth  = src.width;
	mfx_scaler.inputContentHeight = src.height;
	mfx_scaler.outputTexture = dst;
	[mfx_scaler encodeToCommandBuffer:cb];
	// drop the texture refs at once: the command buffer retains what the
	// encode references, and a lingering strong ref here would pin a POOLED
	// texture past R_RenderTarget_FreeUnused's release of it
	mfx_scaler.colorTexture = nil;
	mfx_scaler.outputTexture = nil;
	return true;
}

qbool MetalFX_Available(void)
{
	return mfx_available;
}

unsigned int MetalFX_ColorTextureUsage(void)
{
	return mfx_colorusage;
}

unsigned int MetalFX_OutputTextureUsage(void)
{
	// The UNION of both scalers' demands. mb_screentex is the destination of
	// whichever one the cvar selects, and the backend's recreate predicate
	// keys only on size and format -- so the bits must be right for both from
	// the first frame, or a mid-session switch to temporal would find a
	// screen texture the temporal scaler refuses and there would be no
	// recreate to fix it.
	return mfx_outputusage | mfx_toutputusage;
}

unsigned int MetalFX_TemporalOutputTextureUsage(void)
{
	return mfx_toutputusage;
}

qbool MetalFX_TemporalAvailable(void)
{
	return mfx_tavailable;
}

void MetalFX_TemporalScaleRange(float *outmin, float *outmax)
{
	if (outmin) *outmin = mfx_tminscale;
	if (outmax) *outmax = mfx_tmaxscale;
}

qbool MetalFX_TemporalReady(int inwidth, int inheight, int outwidth, int outheight, int intextype, qbool hdr)
{
	id<MTLDevice> dev;
	MTLFXTemporalScalerDescriptor *td;
	MTLPixelFormat informat, outformat;

	if (!mfx_tavailable)
		return false;
	if (inwidth  == mfx_tkey_inw  && inheight  == mfx_tkey_inh &&
	    outwidth == mfx_tkey_outw && outheight == mfx_tkey_outh &&
	    intextype == mfx_tkey_textype && hdr == mfx_tkey_hdr)
		return mfx_tscaler != nil && !mfx_tkey_failed;

	// key recorded, and the failure memoised, BEFORE the factory runs -- the
	// 8-5 shape, so a configuration MetalFX refuses costs one factory call
	// rather than one per frame
	mfx_release_temporal();
	mfx_tkey_inw = inwidth;  mfx_tkey_inh = inheight;
	mfx_tkey_outw = outwidth; mfx_tkey_outh = outheight;
	mfx_tkey_textype = intextype;
	mfx_tkey_hdr = hdr;
	mfx_tkey_failed = true;

	switch (intextype)
	{
	case TEXTYPE_COLORBUFFER:    informat = MTLPixelFormatBGRA8Unorm;  break;
	case TEXTYPE_COLORBUFFER16F: informat = MTLPixelFormatRGBA16Float; break;
	case TEXTYPE_COLORBUFFER32F: informat = MTLPixelFormatRGBA32Float; break;
	default:
		Con_Printf(CON_WARN "MetalFX: unsupported temporal input textype %d; falling back\n", intextype);
		return false;
	}
	outformat = hdr ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm;

	dev = (__bridge id<MTLDevice>)VID_Metal_GetDevice();
	if (!dev)
		return false;
	td = [[MTLFXTemporalScalerDescriptor alloc] init];
	td.inputWidth   = (NSUInteger)inwidth;
	td.inputHeight  = (NSUInteger)inheight;
	td.outputWidth  = (NSUInteger)outwidth;
	td.outputHeight = (NSUInteger)outheight;
	td.colorTextureFormat  = informat;
	// LOCKSTEP with metal_textures.m's mt_pixelformat: every TEXTYPE_DEPTHBUFFER*
	// collapses to Depth32Float there, and the motion target is allocated as
	// TEXTYPE_COLORBUFFER16F, which is RGBA16Float. A drift in either is caught
	// by the format comparison in MetalFX_EncodeTemporalUpscale against the real
	// texture objects.
	td.depthTextureFormat  = MTLPixelFormatDepth32Float;
	td.motionTextureFormat = MTLPixelFormatRGBA16Float;
	td.outputTextureFormat = outformat;
	// autoExposure OFF: this scaler is fed the POSTPROCESSED frame -- gamma
	// applied, shoulder applied -- so its values are display-encoded and an
	// exposure estimate has nothing to estimate. inputContentProperties OFF:
	// the key is re-minted on any size change, so dynamic resolution would be
	// machinery for a case that cannot arise. Synchronous initialisation ON:
	// the mint rate is once per key (resize / viewscale / EDR), and a
	// deterministic one-off cost beats a background recompile landing in the
	// middle of a bench run.
	td.autoExposureEnabled = NO;
	td.inputContentPropertiesEnabled = NO;
	td.requiresSynchronousInitialization = YES;
	// The reactive mask (macOS 14.4+): a per-pixel 0..1 telling the scaler how
	// much to DISTRUST its history there. Enabled on every mint, because the
	// descriptor is immutable and the caller decides per frame whether to feed
	// a mask or nil -- and nil means "no mask this frame", which is the
	// documented way to not use it. The mask is fed from the dynamic-light
	// footprint: lighting that moves across STILL geometry has the geometry's
	// (zero) motion vector, which no vector can correct -- this is MetalFX's
	// own mechanism for exactly that case.
	td.reactiveMaskTextureEnabled = YES;
	// BGRA8, not R8: the mask is drawn into an ordinary pooled TEXTYPE_COLORBUFFER
	// target (no new TEXTYPE, which is a place this tree has broken before) and
	// MetalFX reads the reactive value from the first channel of whatever
	// format it is told.
	td.reactiveMaskTextureFormat = MTLPixelFormatBGRA8Unorm;
	mfx_tscaler = [td newTemporalScalerWithDevice:dev];
	if (!mfx_tscaler)
	{
		Con_Printf(CON_WARN "MetalFX: temporal factory refused %dx%d -> %dx%d (textype %d%s); falling back\n",
			inwidth, inheight, outwidth, outheight, intextype, hdr ? ", hdr" : "");
		return false;
	}
	if (((unsigned int)mfx_tscaler.outputTextureUsage &
	     ~((unsigned int)(MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead) | mfx_toutputusage | mfx_outputusage)) != 0)
	{
		Con_Printf(CON_ERROR "MetalFX: this temporal scaler's outputTextureUsage 0x%x exceeds the screen texture's usage; the boot probe queried 0x%x\n",
			(unsigned int)mfx_tscaler.outputTextureUsage, mfx_toutputusage);
		mfx_tscaler = nil;
		return false;
	}
	if ((((unsigned int)mfx_tscaler.colorTextureUsage | (unsigned int)mfx_tscaler.depthTextureUsage | (unsigned int)mfx_tscaler.motionTextureUsage) &
	     ~(unsigned int)(MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead)) != 0)
	{
		Con_Printf(CON_ERROR "MetalFX: this temporal scaler's input usage (colour 0x%x depth 0x%x motion 0x%x) exceeds the pooled render-target usage\n",
			(unsigned int)mfx_tscaler.colorTextureUsage, (unsigned int)mfx_tscaler.depthTextureUsage, (unsigned int)mfx_tscaler.motionTextureUsage);
		mfx_tscaler = nil;
		return false;
	}
	mfx_tkey_failed = false;
	// once per key change, never per frame (the 92 GB console lesson)
	Con_Printf("MetalFX: temporal scaler %dx%d -> %dx%d (%s, %s)\n",
		inwidth, inheight, outwidth, outheight,
		informat == MTLPixelFormatBGRA8Unorm ? "BGRA8" : (informat == MTLPixelFormatRGBA16Float ? "RGBA16F" : "RGBA32F"),
		hdr ? "hdr" : "sdr");
	return true;
}

qbool MetalFX_EncodeTemporalUpscale(void *cmdbuf, void *srctex, void *depthtex, void *motiontex, void *reactivetex, void *dsttex,
                                    float jitterx, float jittery, qbool reset)
{
	id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)cmdbuf;
	id<MTLTexture> src = (__bridge id<MTLTexture>)srctex;
	id<MTLTexture> dep = (__bridge id<MTLTexture>)depthtex;
	id<MTLTexture> mot = (__bridge id<MTLTexture>)motiontex;
	id<MTLTexture> rea = (__bridge id<MTLTexture>)reactivetex;   // nil = no mask this frame
	id<MTLTexture> dst = (__bridge id<MTLTexture>)dsttex;

	if (!mfx_tscaler || mfx_tkey_failed || !cb || !src || !dep || !mot || !dst)
		return false;
	if (src.width  != (NSUInteger)mfx_tkey_inw  || src.height != (NSUInteger)mfx_tkey_inh ||
	    dep.width  != (NSUInteger)mfx_tkey_inw  || dep.height != (NSUInteger)mfx_tkey_inh ||
	    mot.width  != (NSUInteger)mfx_tkey_inw  || mot.height != (NSUInteger)mfx_tkey_inh ||
	    dst.width  != (NSUInteger)mfx_tkey_outw || dst.height != (NSUInteger)mfx_tkey_outh ||
	    src.pixelFormat != mfx_tscaler.colorTextureFormat ||
	    dep.pixelFormat != mfx_tscaler.depthTextureFormat ||
	    mot.pixelFormat != mfx_tscaler.motionTextureFormat ||
	    dst.pixelFormat != mfx_tscaler.outputTextureFormat ||
	    (src.usage & mfx_tscaler.colorTextureUsage)  != mfx_tscaler.colorTextureUsage ||
	    (dep.usage & mfx_tscaler.depthTextureUsage)  != mfx_tscaler.depthTextureUsage ||
	    (mot.usage & mfx_tscaler.motionTextureUsage) != mfx_tscaler.motionTextureUsage ||
	    (dst.usage & mfx_tscaler.outputTextureUsage) != mfx_tscaler.outputTextureUsage ||
	    dst.storageMode != MTLStorageModePrivate)
	{
		if (!mfx_tencode_warned)
		{
			mfx_tencode_warned = true;
			Con_Printf(CON_ERROR "MetalFX: temporal encode refused -- src %ux%u fmt %u, depth %ux%u fmt %u usage 0x%x, motion %ux%u fmt %u usage 0x%x, dst %ux%u fmt %u usage 0x%x storage %u against key %dx%d -> %dx%d\n",
				(unsigned)src.width, (unsigned)src.height, (unsigned)src.pixelFormat,
				(unsigned)dep.width, (unsigned)dep.height, (unsigned)dep.pixelFormat, (unsigned)dep.usage,
				(unsigned)mot.width, (unsigned)mot.height, (unsigned)mot.pixelFormat, (unsigned)mot.usage,
				(unsigned)dst.width, (unsigned)dst.height, (unsigned)dst.pixelFormat, (unsigned)dst.usage, (unsigned)dst.storageMode,
				mfx_tkey_inw, mfx_tkey_inh, mfx_tkey_outw, mfx_tkey_outh);
		}
		return false;
	}
	mfx_tscaler.colorTexture  = src;
	mfx_tscaler.depthTexture  = dep;
	mfx_tscaler.motionTexture = mot;
	if (rea)
	{
		// The mask SPEAKS (2026-08-19): a format or size mismatch used to nil
		// it silently, and a silently-dropped mask is indistinguishable from a
		// working one on screen -- every way this feature fails is silent and
		// faster. One line each way, once per session; smoke asserts both.
		static qbool engaged, refused;
		if (rea.pixelFormat == mfx_tscaler.reactiveMaskTextureFormat
		 && rea.width == (NSUInteger)mfx_tkey_inw && rea.height == (NSUInteger)mfx_tkey_inh)
		{
			mfx_tscaler.reactiveMaskTexture = rea;
			if (!engaged)
			{
				engaged = true;
				Con_Printf("MetalFX: reactive mask %lux%lu engaged\n", (unsigned long)rea.width, (unsigned long)rea.height);
			}
		}
		else
		{
			mfx_tscaler.reactiveMaskTexture = nil;
			if (!refused)
			{
				refused = true;
				Con_Printf(CON_ERROR "MetalFX: reactive mask refused (format %lu vs %lu, %lux%lu vs %dx%d) -- running without it\n",
				           (unsigned long)rea.pixelFormat, (unsigned long)mfx_tscaler.reactiveMaskTextureFormat,
				           (unsigned long)rea.width, (unsigned long)rea.height, mfx_tkey_inw, mfx_tkey_inh);
			}
		}
	}
	else
		mfx_tscaler.reactiveMaskTexture = nil;
	mfx_tscaler.outputTexture = dst;
	mfx_tscaler.inputContentWidth  = src.width;
	mfx_tscaler.inputContentHeight = src.height;
	mfx_tscaler.jitterOffsetX = jitterx;
	mfx_tscaler.jitterOffsetY = jittery;
	// Motion vectors are written in PIXELS of the input buffer, so no rescale.
	mfx_tscaler.motionVectorScaleX = 1.0f;
	mfx_tscaler.motionVectorScaleY = 1.0f;
	// DarkPlaces' projection puts the near plane at depth 0 and the far plane
	// at 1 on the Metal path (the z-remap in gl_backend.c's viewport builders
	// maps GL's [-1,1] onto [0,1] without inverting), so depth is not reversed.
	mfx_tscaler.depthReversed = NO;
	mfx_tscaler.reset = reset ? YES : NO;
	[mfx_tscaler encodeToCommandBuffer:cb];
	// drop the refs at once -- the command buffer retains what the encode
	// references, and a lingering strong ref here would pin POOLED textures
	// past R_RenderTarget_FreeUnused's release of them
	mfx_tscaler.colorTexture  = nil;
	mfx_tscaler.depthTexture  = nil;
	mfx_tscaler.motionTexture = nil;
	mfx_tscaler.reactiveMaskTexture = nil;
	mfx_tscaler.outputTexture = nil;
	return true;
}

#endif // __APPLE__ && USE_METAL_RENDERER
