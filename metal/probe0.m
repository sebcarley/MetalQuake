// Minimal Metal probe: device bring-up + RUNTIME shader compilation + a trivial
// compute dispatch. Purpose: determine whether we can build/run Metal compute
// from the command line WITHOUT the separately-downloadable Metal Toolchain
// (runtime newLibraryWithSource uses the driver compiler). If this works, the
// hardware ray-tracing probe can follow the same build path.
//
// build: clang -fobjc-arc -framework Foundation -framework Metal metal/probe0.m -o metal/probe0
// run:   ./metal/probe0
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

static const char *kSrc =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"kernel void addone(device float *a [[buffer(0)]], uint i [[thread_position_in_grid]]) { a[i] = a[i] + 1.0; }\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("FAIL: no Metal device\n"); return 1; }
        printf("device: %s\n", [[dev name] UTF8String]);
        printf("supportsRaytracing: %d\n", (int)[dev supportsRaytracing]);
        printf("hasUnifiedMemory: %d\n", (int)[dev hasUnifiedMemory]);

        NSError *err = nil;
        MTLCompileOptions *opt = [MTLCompileOptions new];
        id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:kSrc] options:opt error:&err];
        if (!lib) { printf("FAIL: runtime shader compile: %s\n", err ? [[err localizedDescription] UTF8String] : "?"); return 2; }
        printf("runtime shader compile: OK\n");

        id<MTLFunction> fn = [lib newFunctionWithName:@"addone"];
        id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:fn error:&err];
        if (!pso) { printf("FAIL: pipeline: %s\n", err ? [[err localizedDescription] UTF8String] : "?"); return 3; }

        const int N = 8;
        id<MTLBuffer> buf = [dev newBufferWithLength:sizeof(float)*N options:MTLResourceStorageModeShared];
        float *p = (float *)[buf contents];
        for (int i = 0; i < N; i++) p[i] = (float)i;

        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pso];
        [enc setBuffer:buf offset:0 atIndex:0];
        [enc dispatchThreads:MTLSizeMake(N,1,1) threadsPerThreadgroup:MTLSizeMake(N,1,1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];

        int ok = 1;
        for (int i = 0; i < N; i++) if (p[i] != (float)i + 1.0f) ok = 0;
        printf("compute add-one: %s (a[3]=%.1f expect 4.0)\n", ok ? "PASS" : "FAIL", p[3]);
        return ok ? 0 : 4;
    }
}
