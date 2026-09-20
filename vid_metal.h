/*
QuakeM5 -- the native Metal video layer (METAL.md, Phase 0).

Plain-C interface in the rt_metal.h mould: no Objective-C or Metal types leak
into any C translation unit. vid_metal.m owns the CAMetalLayer, the MTLDevice
and the MTLCommandQueue for the whole Metal renderer; later phases
(metal_backend.m, metal_textures.m, metal_shader.m) borrow them from here, and
the RT sidecar shares the same device at Phase 5.

Everything here is macOS-only and safe to call only when vid.renderpath is
RENDERPATH_METAL (except VID_Metal_Shutdown, which is a no-op when never
initialised).
*/

#ifndef VID_METAL_H
#define VID_METAL_H

#include "qtypes.h"

struct SDL_Window;

// Create the Metal view/layer/device/queue against an SDL_WINDOW_METAL window.
// Returns false (with a console error) if no Metal device is available; the
// caller is expected to fall back to the GL path.
qbool VID_Metal_Init(struct SDL_Window *sdlwindow, qbool vsync);

// Present one frame: clear the drawable to the Phase 0 marker colour and
// present it. Tracks window resizes via the drawable size each call.
void VID_Metal_Finish(void);

// Apply vsync to the layer (CAMetalLayer.displaySyncEnabled) at runtime.
// Called from VID_SetVsync_c's Metal arm -- the GL arm's
// SDL_GL_SetSwapInterval needs a GL context this path never creates, so
// before Phase 8-2 the callback early-outed and NEITHER a menu vsync change
// NOR the timedemo force-off (cl_demo.c re-invokes the callback at timedemo
// start/finish exactly so benchmarks never run synced) ever reached the
// layer. Safe to call when the layer is not up (no-op).
void VID_Metal_SetVsync(qbool enable);

// The renderer's MTLDevice, as an opaque pointer so this header stays plain C
// (the rt_metal.h convention). NULL when the Metal path is not up. Callers
// bridge it back: (__bridge id<MTLDevice>)VID_Metal_GetDevice().
// metal_textures.m creates its textures from this device, and at METAL.md
// Phase 5 the RT sidecar adopts the same one via RT_Metal_InitWithDevice --
// two MTLDevice objects for one GPU cannot share resources, which is the whole
// reason the sidecar's output has to cross into GL through an IOSurface today.
void *VID_Metal_GetDevice(void);

// The renderer's MTLCommandQueue, opaque for the same reason. NULL when the
// Metal path is not up. metal_backend.m submits its command buffers here.
// The RT sidecar does NOT share it -- Phase 5-1 deliberately passes NULL for
// the queue (vid_sdl.c has the decision in full: the backend commits one
// command buffer per frame at end-of-frame, so putting the trace on this
// queue would serialise it behind an open render buffer and erase the async
// overlap). The renderer<->sidecar seam is ordered by the CPU wait on the
// shown slot's command buffer in rt_metal.m, not by queue submission order;
// an earlier version of this comment claimed the opposite and stood for
// three phases (corrected at Phase 8-1d).
void *VID_Metal_GetQueue(void);

// Register the video layer's own cvars and commands (METAL.md Phase 7-4: the
// EDR staging knobs and r_edr_report). Called ONCE from VID_Init, beside the
// rt_metal_* block that establishes the pattern -- not from VID_Metal_Init,
// which runs again on every vid_restart.
//
// The master gate r_edr is NOT here: it lives with the renderer's cvars in
// gl_rmain.c, because R_UpdateVariables is what reads it and OBJ_COMMON code
// cannot call into this file. What is here is only what needs AppKit to answer.
void VID_Metal_RegisterCvars(void);

// Tear everything down (view, layer references, device, queue). Safe to call
// when never initialised, and safe to call twice.
void VID_Metal_Shutdown(void);

#endif
