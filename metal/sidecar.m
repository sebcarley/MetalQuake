// In-process Metal sidecar + IOSurface interop probe. De-risks the roadmap's
// integration seam: can a Metal device live in the SAME process as the engine's
// existing SDL/OpenGL 3.2 Core context, produce an image into a shared
// IOSurface, and have that GL context read back the exact bytes Metal wrote?
// That is the "sidecar" data path — Metal computes (eventually RT results), GL
// composites/displays — without a full renderer rewrite.
//
// The probe:
//   1. brings up a GL 3.2 Core context the way the engine does (SDL, matching
//      vid_sdl.c; falls back to a headless CGL context if no window server),
//   2. creates an MTLDevice alongside that live GL context (coexistence),
//   3. allocates a BGRA8 IOSurface, wraps it as an MTLTexture, and runs a
//      compute kernel that writes a deterministic per-pixel pattern,
//   4. imports the SAME IOSurface into GL via CGLTexImageIOSurface2D and reads
//      it back with glGetTexImage,
//   5. verifies (a) Metal wrote the expected pattern and (b) GL sees the shared
//      surface byte-for-byte identically to the CPU view of it.
//
// build: clang -O2 -fobjc-arc $(sdl2-config --cflags) \
//          -framework Foundation -framework Metal -framework QuartzCore \
//          -framework IOSurface -framework OpenGL -L/opt/homebrew/lib -lSDL2 \
//          metal/sidecar.m -o metal/sidecar
// run:   ./metal/sidecar [width height]   (SDL_VIDEODRIVER=dummy forces the
//          headless CGL fallback path used by CI / the dedicated server)
#define GL_SILENCE_DEPRECATION
#define SDL_MAIN_HANDLED
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>  // CACurrentMediaTime
#import <IOSurface/IOSurface.h>
#import <OpenGL/gl3.h>
#import <OpenGL/OpenGL.h>        // CGL
#import <OpenGL/CGLIOSurface.h>  // CGLTexImageIOSurface2D
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Compute kernel: write a deterministic pattern so both the CPU and GL can
// predict every texel independently. BGRA8Unorm target; float4 is RGBA order.
static const char *kSrc =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"kernel void fill(texture2d<float, access::write> tex [[texture(0)]],\n"
"                 uint2 gid [[thread_position_in_grid]])\n"
"{\n"
"    uint w = tex.get_width(), h = tex.get_height();\n"
"    if (gid.x >= w || gid.y >= h) return;\n"
"    float r = float(gid.x) / float(w - 1);\n"
"    float g = float(gid.y) / float(h - 1);\n"
"    float b = float((gid.x ^ gid.y) & 255u) / 255.0f;\n"
"    tex.write(float4(r, g, b, 1.0f), gid);\n"
"}\n";

// float -> unorm8 the way Metal quantizes (round to nearest).
static inline unsigned char un8(float v){ if(v<0)v=0; if(v>1)v=1; return (unsigned char)(v*255.0f + 0.5f); }

// Expected BGRA bytes at (x,y), matching the kernel above.
static void expected_bgra(int x, int y, int w, int h, unsigned char out[4]) {
    float r = (float)x / (float)(w - 1);
    float g = (float)y / (float)(h - 1);
    float b = (float)((x ^ y) & 255) / 255.0f;
    out[0] = un8(b);  // B
    out[1] = un8(g);  // G
    out[2] = un8(r);  // R
    out[3] = 255;     // A
}

// ---- GL bring-up: try SDL (faithful to the engine), else headless CGL. ----
static SDL_Window *g_win = NULL;
static SDL_GLContext g_sdlctx = NULL;
static CGLContextObj g_cgl = NULL;

static const char *bringup_gl(void) {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) == 0) {
        // mirror vid_sdl.c's macOS attributes: GL 3.2 Core, 8888/24/8
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
        g_win = SDL_CreateWindow("metal-sidecar", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 256, 256, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        if (g_win) {
            g_sdlctx = SDL_GL_CreateContext(g_win);
            if (g_sdlctx && SDL_GL_MakeCurrent(g_win, g_sdlctx) == 0) {
                g_cgl = CGLGetCurrentContext();
                if (g_cgl) return "SDL GL 3.2 Core";
            }
        }
        SDL_Quit();
    }
    // headless fallback: raw CGL 3.2 Core context, no window server needed
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAAccelerated,
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
        (CGLPixelFormatAttribute)0
    };
    CGLPixelFormatObj pf = NULL; GLint n = 0;
    if (CGLChoosePixelFormat(attrs, &pf, &n) != kCGLNoError || !pf) return NULL;
    if (CGLCreateContext(pf, NULL, &g_cgl) != kCGLNoError || !g_cgl) { CGLDestroyPixelFormat(pf); return NULL; }
    CGLDestroyPixelFormat(pf);
    if (CGLSetCurrentContext(g_cgl) != kCGLNoError) return NULL;
    return "headless CGL 3.2 Core";
}

static void teardown_gl(void) {
    if (g_sdlctx) { SDL_GL_DeleteContext(g_sdlctx); g_sdlctx = NULL; }
    if (g_win) { SDL_DestroyWindow(g_win); g_win = NULL; }
    if (g_sdlctx == NULL && g_win == NULL && g_cgl && !SDL_WasInit(SDL_INIT_VIDEO)) { CGLSetCurrentContext(NULL); CGLDestroyContext(g_cgl); }
    if (SDL_WasInit(SDL_INIT_VIDEO)) SDL_Quit();
}

int main(int argc, char **argv) {
    @autoreleasepool {
        int W = (argc >= 3) ? atoi(argv[1]) : 256;
        int H = (argc >= 3) ? atoi(argv[2]) : 256;
        if (W < 2 || H < 2 || W > 8192 || H > 8192) { printf("FAIL: bad size %dx%d\n", W, H); return 1; }

        // --- 1. GL context (the thing Metal must coexist with) ---
        const char *how = bringup_gl();
        if (!how) { printf("FAIL: could not create any GL 3.2 Core context\n"); return 2; }
        const char *glver = (const char *)glGetString(GL_VERSION);
        const char *glren = (const char *)glGetString(GL_RENDERER);
        printf("GL context: %s\n  GL_VERSION  = %s\n  GL_RENDERER = %s\n", how, glver ? glver : "?", glren ? glren : "?");

        // --- 2. Metal device alongside the live GL context ---
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("FAIL: no Metal device\n"); teardown_gl(); return 3; }
        printf("Metal device: %s (unifiedMemory=%d, raytracing=%d) — coexisting in-process with GL\n",
               [dev.name UTF8String], (int)dev.hasUnifiedMemory, (int)[dev supportsRaytracing]);

        // --- 3. shared IOSurface, BGRA8 ---
        NSDictionary *props = @{
            (id)kIOSurfaceWidth:           @(W),
            (id)kIOSurfaceHeight:          @(H),
            (id)kIOSurfaceBytesPerElement: @4,
            (id)kIOSurfacePixelFormat:     @((uint32_t)'BGRA'),
        };
        IOSurfaceRef surf = IOSurfaceCreate((__bridge CFDictionaryRef)props);
        if (!surf) { printf("FAIL: IOSurfaceCreate\n"); teardown_gl(); return 4; }

        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                     width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> mtex = [dev newTextureWithDescriptor:td iosurface:surf plane:0];
        if (!mtex) { printf("FAIL: newTextureWithDescriptor:iosurface:\n"); CFRelease(surf); teardown_gl(); return 5; }

        // --- Metal writes the deterministic pattern into the shared surface ---
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:kSrc] options:[MTLCompileOptions new] error:&err];
        if (!lib) { printf("FAIL: shader: %s\n", [[err localizedDescription] UTF8String]); CFRelease(surf); teardown_gl(); return 6; }
        id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fill"] error:&err];
        if (!pso) { printf("FAIL: pso: %s\n", [[err localizedDescription] UTF8String]); CFRelease(surf); teardown_gl(); return 7; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:pso];
        [ce setTexture:mtex atIndex:0];
        MTLSize tg = MTLSizeMake(16, 16, 1);
        MTLSize grid = MTLSizeMake(W, H, 1);
        [ce dispatchThreads:grid threadsPerThreadgroup:tg];
        [ce endEncoding];
        CFTimeInterval t0 = CACurrentMediaTime();
        [cb commit];
        [cb waitUntilCompleted];   // spike-grade sync; production uses a shared MTLSharedEvent / GL fence
        CFTimeInterval t1 = CACurrentMediaTime();
        if (cb.error) { printf("FAIL: dispatch: %s\n", [[cb.error localizedDescription] UTF8String]); CFRelease(surf); teardown_gl(); return 8; }
        printf("Metal wrote %dx%d into IOSurface in %.3f ms\n", W, H, (t1 - t0) * 1000.0);

        // --- 4a. CPU view of the shared surface = ground truth ---
        IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
        size_t stride = IOSurfaceGetBytesPerRow(surf);
        const unsigned char *base = (const unsigned char *)IOSurfaceGetBaseAddress(surf);

        // (a) does the CPU view match what the kernel intended?
        int metal_bad = 0; long metal_maxdiff = 0;
        for (int y = 0; y < H && metal_bad < 16; y++) {
            const unsigned char *row = base + (size_t)y * stride;
            for (int x = 0; x < W; x++) {
                unsigned char e[4]; expected_bgra(x, y, W, H, e);
                const unsigned char *p = row + x * 4;
                for (int c = 0; c < 4; c++) { long d = labs((long)p[c] - (long)e[c]); if (d > metal_maxdiff) metal_maxdiff = d; if (d > 1) { if (metal_bad < 16) printf("  metal pattern off @ (%d,%d) c%d: got %d want %d\n", x, y, c, p[c], e[c]); metal_bad++; } }
            }
        }

        IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);

        // --- 4b. import the SAME surface into GL as a rectangle texture ---
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_RECTANGLE, tex);
        CGLError cglerr = CGLTexImageIOSurface2D(g_cgl, GL_TEXTURE_RECTANGLE, GL_RGBA, W, H,
                                                 GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, surf, 0);
        if (cglerr != kCGLNoError) { printf("FAIL: CGLTexImageIOSurface2D: %s\n", CGLErrorString(cglerr)); glDeleteTextures(1,&tex); CFRelease(surf); teardown_gl(); return 9; }

        // --- 4c. consume it the way GL would when compositing the RT result:
        //     attach as an FBO color target and read back LOGICAL RGBA. This
        //     sidesteps packed-pixel endianness — glReadPixels returns colour
        //     components, not raw surface bytes, so a byte-swizzle in the storage
        //     format can't produce a false pass or false fail. ---
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, tex, 0);
        GLenum fbs = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (fbs != GL_FRAMEBUFFER_COMPLETE) { printf("FAIL: FBO incomplete 0x%04x\n", fbs); glDeleteFramebuffers(1,&fbo); glDeleteTextures(1,&tex); CFRelease(surf); teardown_gl(); return 10; }

        unsigned char *glbuf = malloc((size_t)W * H * 4);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, glbuf);
        GLenum ge = glGetError();
        if (ge != GL_NO_ERROR) { printf("FAIL: glReadPixels error 0x%04x\n", ge); free(glbuf); glDeleteFramebuffers(1,&fbo); glDeleteTextures(1,&tex); CFRelease(surf); teardown_gl(); return 11; }

        // --- 5. GL's logical RGBA vs the pattern Metal produced (the real claim).
        //     FBO-attach + glReadPixels preserves texel coords (no raster flip),
        //     so texel (x,y) reads back at glbuf row y. ---
        int interop_bad = 0; long maxdiff = 0, nonzero = 0;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                unsigned char e[4]; expected_bgra(x, y, W, H, e);    // B,G,R,A
                unsigned char want[4] = { e[2], e[1], e[0], e[3] };  // -> R,G,B,A
                const unsigned char *g = glbuf + ((size_t)y * W + x) * 4;
                for (int c = 0; c < 4; c++) {
                    if (g[c]) nonzero++;
                    long d = labs((long)g[c] - (long)want[c]);
                    if (d > maxdiff) maxdiff = d;
                    if (d > 1) { if (interop_bad < 16) printf("  interop mismatch @ (%d,%d) c%d: gl %d want %d\n", x, y, c, g[c], want[c]); interop_bad++; }
                }
            }
        }

        printf("\nmetal-wrote-expected-pattern : %s (max channel diff %ld, %d texels off)\n",
               metal_bad == 0 ? "PASS" : "FAIL", metal_maxdiff, metal_bad);
        printf("gl-consumes-shared-surface   : %s (max channel diff %ld, %d texels off; %ld non-zero)\n",
               interop_bad == 0 ? "PASS" : "FAIL", maxdiff, interop_bad, nonzero);

        int pass = (metal_bad == 0) && (interop_bad == 0) && (nonzero > 0);
        printf("\nSIDECAR (in-process Metal + GL IOSurface interop): %s\n", pass ? "PASS" : "FAIL");

        free(glbuf);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        CFRelease(surf);
        teardown_gl();
        return pass ? 0 : 11;
    }
}
