/*
QuakeM5 -- the native Metal video layer (METAL.md, Phase 0).

Owns the SDL_MetalView, CAMetalLayer, MTLDevice and MTLCommandQueue for the
Metal renderer. Phase 0 scope: bring the window up, clear it to a marker
colour every frame, present, resize cleanly, shut down cleanly, and survive a
gl <-> metal vid_restart round-trip. The renderer proper arrives with
metal_backend.m in later phases; the device and queue created here are the
ones it (and, at Phase 5, the RT sidecar) will use.

Compiled only on macOS (makefile.inc gates the object on DP_MAKE_TARGET
macosx, matching rt_metal.o). ARC is on: no manual retain/release.
*/

#ifdef __APPLE__

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
// METAL.md Phase 7-4: NSScreen is the ONLY source of the display's EDR headroom,
// and it costs no framework -- otool shows Cocoa already linked, because
// sdl2-config emits it. CoreGraphics (for CGColorSpaceCreateWithName) is the one
// genuinely new link, and it is added to both build systems in this slice.
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <SDL.h>
#include <SDL_metal.h>

#include "quakedef.h"
#include "vid_metal.h"
#include "metal_backend.h"

// ---------------------------------------------------------------------------
// state

static SDL_Window        *vm_window;   // borrowed from vid_sdl.c, not owned
static SDL_MetalView      vm_view;     // owned; SDL_Metal_DestroyView on shutdown
static CAMetalLayer      *vm_layer;    // borrowed from the view
static id<MTLDevice>      vm_dev;
static id<MTLCommandQueue> vm_queue;
static unsigned long      vm_frames;   // presented frames, for the stderr heartbeat
static int                vm_probe;    // VID_METAL_PROBE: read the drawable back and report

// ---------------------------------------------------------------------------
// EDR (METAL.md Phase 7-4)
//
// THE PROPERTY TO GATE ON IS NOT THE ONE THAT SOUNDS RIGHT, and this is the
// measurement the whole phase was re-planned around. On the MAG 272U X24 with
// macOS HDR mode ON and the display set as main:
//
//     maximumExtendedDynamicRangeColorComponentValue           1.0
//     maximumPotentialExtendedDynamicRangeColorComponentValue  4.65
//
// macOS grants headroom only once a layer ASKS for it, so the CURRENT value
// sits at 1.0 on a perfectly capable display until something requests EDR. A
// design that gates capability on the current value -- which is what this file's
// own Phase 0 comment and METAL.md's first draft both said to do -- therefore
// never engages at all, on any hardware, and would have read as "the display
// does not support it". Gate CAPABILITY on potential; drive the shoulder from
// CURRENT; re-read current every frame, because it falls as SDR brightness rises
// and with thermals. HDR mode is necessary, not sufficient.
//
// vm_edr_headroom HOLDS its last good value rather than snapping to 1.0 when the
// screen cannot be resolved (a drag between displays, a window mid-teardown): a
// one-frame collapse to SDR white would be a visible flash, and the honest
// answer to "I momentarily cannot tell" is "the same as last frame".

static cvar_t r_edr_stage = {CF_CLIENT, "r_edr_stage", "3", "METAL.md Phase 7-4, a diagnostic: how much of the EDR ask to actually make, so the OS's grant can be attributed to one thing. 1 = wantsExtendedDynamicRangeContent only, 2 = also the extended colour space, 3 = also the 16-bit float drawable (the shipped ask). 0 makes no ask at all even when r_edr is on. Takes effect on the next frame"};
static cvar_t r_edr_colorspace = {CF_CLIENT | CF_ARCHIVE, "r_edr_colorspace", "1", "which colour space the EDR drawable is tagged with: 0 = none (no colour matching at all), 1 = extended sRGB, 2 = extended LINEAR sRGB, 3 = extended Display P3. 1 is right for this engine and 2 is the trap -- DarkPlaces' postprocess emits DISPLAY-ENCODED values, so tagging them linear makes the compositor apply the display transfer function twice and 0.5 grey shows at about 0.73. NO AUTOMATED TEST IN THIS TREE CAN SEE THIS CHOICE: screenshots read the offscreen scene texture, never the drawable, so it is by eye only"};

static float vm_edr_headroom = 1.0f;   // last good CURRENT value; held, never snapped
static float vm_edr_potential = 1.0f;  // last good POTENTIAL value; the capability gate
static int   vm_edr_appliedstage = -1; // what the layer is configured for; -1 = never touched
static int   vm_edr_appliedcs = -1;
// The colour space the layer came up with, so dropping the ask RESTORES it
// rather than clearing it. Not pedantry: CAMetalLayer.colorspace is not
// documented to start nil, and assigning NULL where the system had put a real
// space would silently turn colour matching off for the whole window -- a real
// change to the presented picture that NO instrument in this tree can see,
// because screenshots read the offscreen scene texture and never the drawable.
static CGColorSpaceRef vm_layer_cs0;
static int   vm_edr_saidclass;         // the SDL_MetalView class-name report, once per boot
static double vm_edr_reportstart;      // r_edr_report's time series
static int    vm_edr_reportnext = -1;
static int    vm_edr_probearm;         // r_edr_probe: capture the NEXT frame's drawable

// ---------------------------------------------------------------------------
// METAL.md Phase 3 slice 4: Phase 0's private pipeline-state cache, its marker
// shader and vm_pipeline_get lived here. They were scaffolding -- one triangle
// taken through a real key/lookup/lazy-compile so the design was exercised
// before metal_backend.m depended on it -- and metal_backend.m's cache
// superseded them at slice 3. Deleted here, where the frame loop moved and the
// deletion became free, exactly as slice 3's commit said it would be.
//
// The clear colour below survives as the pre-first-frame tint only.

#define VM_CLEAR_R 0.09
#define VM_CLEAR_G 0.03
#define VM_CLEAR_B 0.12

// ---------------------------------------------------------------------------
// EDR: reading the display

// The screen the WINDOW is on, which is not NSScreen.mainScreen -- that one
// follows the key window, so it answers a different question the moment the game
// loses focus, and answers it plausibly. SDL_GetWindowWMInfo is rejected for a
// different reason: this links sdl2-compat, so that call goes through an
// emulation shim, while the view SDL handed us is the real object either way.
static NSScreen *vm_screen(void)
{
	NSView *v;
	if (!vm_view)
		return nil;
	v = (__bridge NSView *)vm_view;
	// The plan flagged this as an open question -- whether sdl2-compat's
	// SDL_Metal_CreateView still returns a genuine NSView -- so it is CHECKED
	// and REPORTED rather than assumed, once per boot.
	if (!vm_edr_saidclass)
	{
		vm_edr_saidclass = 1;
		Con_DPrintf("Metal video: SDL_Metal_CreateView returned a %s\n",
			[NSStringFromClass([v class]) UTF8String]);
	}
	if (![v isKindOfClass:[NSView class]])
		return nil;
	return v.window.screen;
}

// Refresh the cached headroom pair. Holds the old values when the screen cannot
// be resolved; both are floored at 1.0 (SDR white) and ceilinged well above any
// real display, because these numbers reach a shader divide.
static void vm_edr_poll(void)
{
	NSScreen *s = vm_screen();
	float cur, pot;
	if (!s)
		return;
	cur = (float)s.maximumExtendedDynamicRangeColorComponentValue;
	pot = (float)s.maximumPotentialExtendedDynamicRangeColorComponentValue;
	vm_edr_headroom  = (cur > 1.0f) ? ((cur < 16.0f) ? cur : 16.0f) : 1.0f;
	vm_edr_potential = (pot > 1.0f) ? ((pot < 16.0f) ? pot : 16.0f) : 1.0f;
}

// The FULL predicate: the renderer's half (vid.edr_wanted, one expression in
// R_UpdateVariables) AND the display's half, which only this file can see.
// r_edr 2 forces the ask past a display reporting no potential -- a diagnostic
// for exactly the case where the reported capability is what is in doubt.
static qbool vm_edr_asking(void)
{
	if (!vid.edr_wanted)
		return false;
	if (r_edr.integer >= 2)
		return true;
	return vm_edr_potential > 1.0f;
}

// Why the ask is NOT being made, in the words of whichever clause failed. There
// are five ways to answer "nothing happened" and a player has no way to tell
// them apart from the picture.
static const char *vm_edr_refusal(void)
{
	float ig[3], sc[3], bs[3], cb;
	if (!r_edr.integer)                          return "r_edr is 0";
	if (vid.renderpath != RENDERPATH_METAL)      return "not the Metal renderpath (vid_renderer metal; vid_restart)";
	// r_viewfbo and r_gamma_analytic stopped being refusals at Phase 8: r_edr
	// FORCES the float scene buffer and the analytic curve itself (gl_rmain.c
	// R_EDR_Wanted), so there is nothing for a player to fix about either.
	if (!VID_GetGammaAnalytic(ig, sc, bs, &cb))  return "the analytic gamma curve refuses here (v_psycho or vid_sRGB), so the LUT is the curve";
	if (r_edr.integer < 2 && !(vm_edr_potential > 1.0f))
		return "this display reports no EDR potential (enable HDR for it in System Settings, or r_edr 2 to ask anyway)";
	if (r_edr_stage.integer <= 0)                return "r_edr_stage is 0";
	return NULL;
}

// Reconfigure the layer to match what is being asked for. Called between frames,
// BEFORE nextDrawable, because CAMetalLayer property changes must not straddle a
// drawable. Idempotent: the applied state is cached, so the common case is two
// integer compares.
static void vm_edr_apply(void)
{
	int stage = vm_edr_asking() ? (int)bound(0, r_edr_stage.integer, 3) : 0;
	int cs    = stage >= 2 ? (int)bound(0, r_edr_colorspace.integer, 3) : 0;

	if (stage == vm_edr_appliedstage && cs == vm_edr_appliedcs)
		return;
	vm_edr_appliedstage = stage;
	vm_edr_appliedcs = cs;

	// STAGE 3 IS THE FORMAT, and at 7-4 it is deliberately reachable only from
	// the console. The shipped default r_edr 0 makes stage 0, so nothing here
	// runs at all and this slice moves no pixels -- which is what lets its
	// acceptance be a byte-identity claim against 7-3.
	vm_layer.pixelFormat = (stage >= 3) ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm;

	if (cs)
	{
		CFStringRef name = kCGColorSpaceExtendedSRGB;
		CGColorSpaceRef space;
		if (cs == 2) name = kCGColorSpaceExtendedLinearSRGB;
		if (cs == 3) name = kCGColorSpaceExtendedDisplayP3;
		space = CGColorSpaceCreateWithName(name);
		vm_layer.colorspace = space;      // the layer retains it
		if (space) CGColorSpaceRelease(space);
	}
	else
		vm_layer.colorspace = vm_layer_cs0;   // restore, never clear -- see the declaration

	vm_layer.wantsExtendedDynamicRangeContent = (stage >= 1) ? YES : NO;

	// Publish the ANSWER so the backend's offscreen screen texture follows the
	// drawable's format rather than deciding for itself. Set here, after the
	// layer is actually reconfigured, so the two can only ever be one frame
	// apart and never permanently disagreed.
	vid.edr_active = (stage >= 3);

	Con_DPrintf("Metal video: EDR ask now stage %d, colorspace %d (drawable %s)\n",
		stage, cs, (stage >= 3) ? "RGBA16Float" : "BGRA8Unorm");
}

// r_edr_report: a TIME SERIES, not a snapshot, because the open question is
// whether the grant arrives at all, how fast it ramps and where it settles --
// and a single reading taken in the same frame as the ask would answer none of
// those. Samples are taken from the frame loop at the offsets below.
static const double vm_edr_reportat[] = { 0.0, 0.5, 1.0, 2.0, 3.0 };
#define VM_EDR_REPORTS ((int)(sizeof(vm_edr_reportat) / sizeof(vm_edr_reportat[0])))

static void vm_edr_reportnow(int sample)
{
	NSScreen *mine = vm_screen();
	NSArray<NSScreen *> *all = [NSScreen screens];
	NSUInteger i;
	Con_Printf("r_edr_report [t+%.1fs] ask=%s stage=%d cs=%d\n",
		vm_edr_reportat[sample], vm_edr_asking() ? "YES" : "no",
		vm_edr_appliedstage, vm_edr_appliedcs);
	for (i = 0; i < [all count]; i++)
	{
		NSScreen *s = [all objectAtIndex:i];
		Con_Printf("  screen %u%s current %.3f  potential %.3f  reference %.3f  %s\n",
			(unsigned)i, (s == mine) ? " <- ours" : "        ",
			s.maximumExtendedDynamicRangeColorComponentValue,
			s.maximumPotentialExtendedDynamicRangeColorComponentValue,
			s.maximumReferenceExtendedDynamicRangeColorComponentValue,
			[[s localizedName] UTF8String]);
	}
	if (!mine)
		Con_Printf("  (the window's screen could not be resolved; headroom is being HELD at %.3f)\n", vm_edr_headroom);
}

static void VID_Metal_EDRReport_f(cmd_state_t *cmd)
{
	const char *why;
	(void)cmd;
	if (vid.renderpath != RENDERPATH_METAL)
	{
		Con_Printf("r_edr_report: only meaningful on the Metal path (vid_renderer metal; vid_restart)\n");
		return;
	}
	why = vm_edr_refusal();
	Con_Printf("r_edr_report: %s\n", why ? why : "the EDR ask is being made");
	Con_Printf("r_edr_report: sampling for %.0f seconds -- the grant is not instantaneous and one reading proves nothing\n",
		vm_edr_reportat[VM_EDR_REPORTS - 1]);
	vm_edr_reportstart = host.realtime;
	vm_edr_reportnext = 0;
}

static void VID_Metal_EDRProbe_f(cmd_state_t *cmd)
{
	(void)cmd;
	if (vid.renderpath != RENDERPATH_METAL)
	{
		Con_Printf("r_edr_probe: only meaningful on the Metal path (vid_renderer metal; vid_restart)\n");
		return;
	}
	// A CAMetalLayer with framebufferOnly YES cannot be read back at all, and
	// that is the faster configuration, so it stays the default. Say which
	// switch is missing rather than reporting an empty frame.
	if (!vm_probe)
	{
		Con_Printf(CON_ERROR "r_edr_probe: the drawable is not readable -- relaunch with the environment variable VID_METAL_PROBE=1 (framebufferOnly must be NO)\n");
		return;
	}
	vm_edr_probearm = 1;
	Con_Printf("r_edr_probe: armed; the next presented frame will be measured UNCLAMPED\n");
}

void VID_Metal_RegisterCvars(void)
{
	Cvar_RegisterVariable(&r_edr_stage);
	Cvar_RegisterVariable(&r_edr_colorspace);
	Cmd_AddCommand(CF_CLIENT, "r_edr_probe", VID_Metal_EDRProbe_f, "METAL.md Phase 7-5: measure the next presented frame WITHOUT the 8-bit clamp every other instrument here applies -- per-channel maxima, how much of the frame exceeds SDR white, and a band histogram, for both the screen texture and the drawable. Needs VID_METAL_PROBE=1 in the environment, because a readable drawable is the slower configuration");
	Cmd_AddCommand(CF_CLIENT, "r_edr_report", VID_Metal_EDRReport_f, "METAL.md Phase 7: report the EDR headroom every attached display reports, as a time series after the ask, plus the reason the ask is not being made if it is not. The OS's own grant is the ground truth for whether EDR is working -- there is no way to see above 1.0 in a screenshot");
}

qbool VID_Metal_Init(struct SDL_Window *sdlwindow, qbool vsync)
{
	if (vm_dev)
		VID_Metal_Shutdown();

	vm_window = (SDL_Window *)sdlwindow;
	vm_view = SDL_Metal_CreateView(vm_window);
	if (!vm_view)
	{
		Con_Printf(CON_ERROR "VID_Metal_Init: SDL_Metal_CreateView failed: %s\n", SDL_GetError());
		// through Shutdown like the two branches below, so vm_window does not
		// dangle -- the caller destroys the SDL window on this return
		VID_Metal_Shutdown();
		return false;
	}
	vm_layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(vm_view);
	if (!vm_layer)
	{
		Con_Printf(CON_ERROR "VID_Metal_Init: no CAMetalLayer behind the SDL Metal view\n");
		VID_Metal_Shutdown();
		return false;
	}

	vm_dev = MTLCreateSystemDefaultDevice();
	if (!vm_dev)
	{
		Con_Printf(CON_ERROR "VID_Metal_Init: no Metal device\n");
		VID_Metal_Shutdown();
		return false;
	}
	vm_queue = [vm_dev newCommandQueue];
	vm_queue.label = @"QuakeM5 renderer";

	vm_layer.device = vm_dev;
	// SDR by default; METAL.md Phase 7-4's vm_edr_apply moves this to
	// RGBA16Float, between frames, when the EDR ask is actually being made.
	vm_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	// Remember the colour space the system gave us BEFORE anything asks for
	// EDR, so dropping the ask restores it exactly rather than clearing it.
	// CGColorSpaceRetain because ARC does not manage CF types.
	vm_layer_cs0 = vm_layer.colorspace;
	if (vm_layer_cs0) CGColorSpaceRetain(vm_layer_cs0);
	// VID_METAL_PROBE=1 makes the drawable readable so VID_Metal_Finish can blit
	// it back and report pixel values to stderr. That is the only way to VERIFY
	// this path actually draws -- the engine's screenshot path is GL, and a
	// pipeline that silently failed to build looks identical to a working one
	// (both leave the clear colour). Off by default: framebufferOnly YES is the
	// faster configuration. Grows into the Phase 3 screenshot readback.
	vm_probe = (getenv("VID_METAL_PROBE") && atoi(getenv("VID_METAL_PROBE"))) ? 1 : 0;
	vm_layer.framebufferOnly = vm_probe ? NO : YES;
	// The initial value; live changes (menu, console, and the timedemo
	// force-off) arrive through VID_Metal_SetVsync via the cvar callback's
	// Metal arm (Phase 8-2 rider -- this line was init-only for eight phases,
	// which was harmless while nobody archived vsync on).
	vm_layer.displaySyncEnabled = vsync ? YES : NO;

	{
		int w = 0, h = 0;
		SDL_Metal_GetDrawableSize(vm_window, &w, &h);
		if (w > 0 && h > 0)
			vm_layer.drawableSize = CGSizeMake(w, h);
	}

	Con_Printf("Metal video: %s\n", [vm_dev.name UTF8String]);

	// The Phase 0 stderr banner ("the window is a flat VIOLET clear... NO
	// CONSOLE... you cannot get stranded") lived here until Phase 8 made Metal
	// the default. Its premise expired at Phase 3 -- this path draws its own
	// console -- and both of its escape hatches inverted when the default
	// flipped: vid_renderer is ARCHIVED again (so "gl" can persist), and a
	// failed Metal init falls back to GL in VID_InitMode. One line on stderr
	// remains, because a black window with a live process is still the one
	// state where the console cannot speak.
	fprintf(stderr, "QuakeM5: Metal renderer up (%s). Fallback: vid_renderer gl ; vid_restart\n",
	        [vm_dev.name UTF8String]);
	fflush(stderr);
	vm_frames = 0;
	return true;
}

void *VID_Metal_GetDevice(void)
{
	return (__bridge void *)vm_dev;
}

void *VID_Metal_GetQueue(void)
{
	return (__bridge void *)vm_queue;
}

void VID_Metal_SetVsync(qbool enable)
{
	if (!vm_layer)
		return;
	if (vm_layer.displaySyncEnabled == (enable ? YES : NO))
		return;
	vm_layer.displaySyncEnabled = enable ? YES : NO;
	Con_DPrintf("Metal video: vsync %s\n", enable ? "activated" : "deactivated");
}

void VID_Metal_Finish(void)
{
	if (!vm_dev || !vm_layer)
		return;

	@autoreleasepool {
		int w = 0, h = 0;
		id<CAMetalDrawable> drawable;

		// track resizes; drawableSize writes are cheap when unchanged but the
		// comparison keeps the layer from re-validating every frame
		SDL_Metal_GetDrawableSize(vm_window, &w, &h);
		if (w > 0 && h > 0 && ((int)vm_layer.drawableSize.width != w || (int)vm_layer.drawableSize.height != h))
			vm_layer.drawableSize = CGSizeMake(w, h);

		// EDR (METAL.md Phase 7-4), both before nextDrawable and in this order:
		// the poll feeds the capability gate the apply reads, and a CAMetalLayer
		// reconfiguration must not straddle a drawable. At the shipped default
		// r_edr 0 the poll is one AppKit property read and the apply is two
		// integer compares.
		vm_edr_poll();
		vm_edr_apply();

		// THE HEADROOM REACHES THE CURVE (METAL.md Phase 7-6). One line, and
		// every slice before it exists to make this line safe: 7-1 gave the
		// gamma a form that can emit above 1.0, 7-3 gave the shoulder and the
		// ceiling somewhere to put it, 7-5 gave the drawable somewhere to carry
		// it. 1.0 whenever the ask is not actually in force, which is what makes
		// r_edr 0 identical to Phase 6 rather than merely close to it.
		//
		// R_BlendView reads this ONE FRAME STALE, because this is the end of the
		// frame and that was the middle of it. That is correct rather than
		// tolerated: the OS varies granted headroom slowly, over hundreds of
		// milliseconds, so a frame of lag is invisible -- and the alternative,
		// polling AppKit from inside the render, would put a system call on the
		// hot path to chase a number that cannot have moved. NOT smoothed, for a
		// measured reason: the OS ramps granted headroom and SDR brightness
		// together and the two cancel, so a filter here would show as a visible
		// drift where the raw value shows as nothing.
		vid.edr_headroom = vid.edr_active ? vm_edr_headroom : 1.0f;

		// the armed r_edr_report series. Sampled here rather than from the
		// command, because "did the grant arrive, and when" cannot be answered
		// inside the frame that asked.
		while (vm_edr_reportnext >= 0 && vm_edr_reportnext < VM_EDR_REPORTS
		       && host.realtime - vm_edr_reportstart >= vm_edr_reportat[vm_edr_reportnext])
		{
			vm_edr_reportnow(vm_edr_reportnext);
			if (++vm_edr_reportnext >= VM_EDR_REPORTS)
				vm_edr_reportnext = -1;
		}

		drawable = [vm_layer nextDrawable];
		if (!drawable)
			return; // window occluded or mid-resize; skip the frame
		// counted here, not after presenting, so the probe's early return inside
		// EndFrame cannot freeze the counter (it did, and re-fired every frame)
		++vm_frames;

		// METAL.md Phase 3 slice 4: the frame has already been rendered into the
		// backend's GL-layout screen texture by the time we get here -- everything
		// CL_UpdateScreen does now runs on this path. All that is left is the
		// v-flip present pass and the present itself, both of which the backend
		// owns because it owns the command buffer they have to share.
		// mode 2 (the armed EDR probe) wins over the flatness probe's fixed
		// frame 120, because it is asked for by hand and that one is automatic.
		Metal_Backend_EndFrame((__bridge void *)drawable,
			vm_edr_probearm ? 2 : ((vm_probe && vm_frames == 120) ? 1 : 0));
		if (vm_edr_probearm)
			vm_edr_probearm = 0;

		// The Phase 0 stderr heartbeat lived here until Phase 8. With Metal the
		// DEFAULT renderer it was five lines a minute of Xcode-console noise in
		// every ordinary session, answering a question ("is the invisible path
		// alive") that stopped existing when this path started drawing the
		// console at Phase 3.
	}
}

void VID_Metal_Shutdown(void)
{
	vm_frames = 0;
	// The layer dies with the view, so the cached "what is applied" pair must
	// not survive it: a gl -> metal -> gl round trip rebuilds the layer with the
	// SDR defaults, and a stale cache would make vm_edr_apply believe the ask
	// was still in force and skip re-making it.
	vm_edr_appliedstage = vm_edr_appliedcs = -1;
	vm_edr_headroom = vm_edr_potential = 1.0f;
	vid.edr_active = false;
	vid.edr_headroom = 1.0f;
	vm_edr_saidclass = 0;
	vm_edr_reportnext = -1;
	if (vm_layer_cs0) { CGColorSpaceRelease(vm_layer_cs0); vm_layer_cs0 = NULL; }
	vm_queue = nil;
	vm_dev = nil;
	vm_layer = nil;
	if (vm_view)
	{
		SDL_Metal_DestroyView(vm_view);
		vm_view = NULL;
	}
	vm_window = NULL;
}

#else // !__APPLE__

#include "quakedef.h"
#include "vid_metal.h"

qbool VID_Metal_Init(struct SDL_Window *sdlwindow, qbool vsync) { (void)sdlwindow; (void)vsync; return false; }
void *VID_Metal_GetDevice(void) { return 0; }
void *VID_Metal_GetQueue(void) { return 0; }
void VID_Metal_SetVsync(qbool enable) { (void)enable; }
void VID_Metal_Finish(void) {}
void VID_Metal_RegisterCvars(void) {}
void VID_Metal_Shutdown(void) {}

#endif
