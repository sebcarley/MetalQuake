// Hardware ray-tracing probe: build an MTLAccelerationStructure from one known
// triangle, fire rays with hand-computed answers via an inline-RT compute kernel,
// and verify the hit distances. Proves the Metal RT API + hardware traversal work
// on this machine (the core unknown for the GPU-RT roadmap). Runtime-compiled
// shader, so no offline Metal Toolchain needed.
//
// build: clang -fobjc-arc -framework Foundation -framework Metal metal/rtprobe.m -o metal/rtprobe
// run:   ./metal/rtprobe
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <simd/simd.h>

static const char *kSrc =
"#include <metal_stdlib>\n"
"#include <metal_raytracing>\n"
"using namespace metal;\n"
"using namespace raytracing;\n"
"kernel void rtprobe(primitive_acceleration_structure accel [[buffer(0)]],\n"
"                    device const float3 *rays [[buffer(1)]],\n"   // pairs: origin, direction
"                    device float *out [[buffer(2)]],\n"
"                    uint tid [[thread_position_in_grid]])\n"
"{\n"
"    ray r;\n"
"    r.origin = rays[tid*2u+0u];\n"
"    r.direction = rays[tid*2u+1u];\n"
"    r.min_distance = 0.0f;\n"
"    r.max_distance = 1e6f;\n"
"    intersector<triangle_data> isect;\n"
"    intersection_result<triangle_data> res = isect.intersect(r, accel);\n"
"    out[tid] = (res.type == intersection_type::triangle) ? res.distance : -1.0f;\n"
"}\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev || ![dev supportsRaytracing]) { printf("FAIL: device/raytracing unavailable\n"); return 1; }
        NSError *err = nil;

        // --- known triangle in plane z=1 covering x>=0,y>=0,x+y<=1 ---
        float verts[9] = { 0,0,1,  1,0,1,  0,1,1 };
        id<MTLBuffer> vbuf = [dev newBufferWithBytes:verts length:sizeof(verts) options:MTLResourceStorageModeShared];

        MTLAccelerationStructureTriangleGeometryDescriptor *tri = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        tri.vertexBuffer = vbuf;
        tri.vertexBufferOffset = 0;
        tri.vertexStride = sizeof(float) * 3;     // packed float3
        tri.vertexFormat = MTLAttributeFormatFloat3;
        tri.triangleCount = 1;

        MTLPrimitiveAccelerationStructureDescriptor *pdesc = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        pdesc.geometryDescriptors = @[tri];

        MTLAccelerationStructureSizes sizes = [dev accelerationStructureSizesWithDescriptor:pdesc];
        id<MTLAccelerationStructure> as = [dev newAccelerationStructureWithSize:sizes.accelerationStructureSize];
        id<MTLBuffer> scratch = [dev newBufferWithLength:sizes.buildScratchBufferSize options:MTLResourceStorageModePrivate];

        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> bcb = [q commandBuffer];
        id<MTLAccelerationStructureCommandEncoder> aenc = [bcb accelerationStructureCommandEncoder];
        [aenc buildAccelerationStructure:as descriptor:pdesc scratchBuffer:scratch scratchBufferOffset:0];
        [aenc endEncoding];
        [bcb commit];
        [bcb waitUntilCompleted];
        if (bcb.error) { printf("FAIL: AS build: %s\n", [[bcb.error localizedDescription] UTF8String]); return 2; }
        printf("acceleration structure built (size %lu bytes)\n", (unsigned long)sizes.accelerationStructureSize);

        // --- rays with hand-computed answers ---
        simd_float3 rays[8]; int nrays = 4;
        rays[0] = (simd_float3){0.25f,0.25f,0}; rays[1] = (simd_float3){0,0,1};   // inside  -> hit t=1
        rays[2] = (simd_float3){0.10f,0.10f,0}; rays[3] = (simd_float3){0,0,1};   // inside  -> hit t=1
        rays[4] = (simd_float3){2.00f,2.00f,0}; rays[5] = (simd_float3){0,0,1};   // outside -> miss
        rays[6] = (simd_float3){0.25f,0.25f,3}; rays[7] = (simd_float3){0,0,-1};  // from behind -> hit t=2
        float expect[4] = { 1.0f, 1.0f, -1.0f, 2.0f };
        id<MTLBuffer> rbuf = [dev newBufferWithBytes:rays length:sizeof(simd_float3)*2*nrays options:MTLResourceStorageModeShared];
        id<MTLBuffer> obuf = [dev newBufferWithLength:sizeof(float)*nrays options:MTLResourceStorageModeShared];

        MTLCompileOptions *opt = [MTLCompileOptions new];
        id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:kSrc] options:opt error:&err];
        if (!lib) { printf("FAIL: shader compile: %s\n", [[err localizedDescription] UTF8String]); return 3; }
        id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"rtprobe"] error:&err];
        if (!pso) { printf("FAIL: pipeline: %s\n", [[err localizedDescription] UTF8String]); return 4; }

        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pso];
        [enc setAccelerationStructure:as atBufferIndex:0];
        [enc setBuffer:rbuf offset:0 atIndex:1];
        [enc setBuffer:obuf offset:0 atIndex:2];
        [enc dispatchThreads:MTLSizeMake(nrays,1,1) threadsPerThreadgroup:MTLSizeMake(nrays,1,1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.error) { printf("FAIL: dispatch: %s\n", [[cb.error localizedDescription] UTF8String]); return 5; }

        float *o = (float *)[obuf contents];
        int ok = 1;
        for (int i = 0; i < nrays; i++) {
            int pass = (fabsf(o[i] - expect[i]) < 1e-4f);
            if (!pass) ok = 0;
            printf("  ray %d: got %+.4f  expect %+.1f  %s\n", i, o[i], expect[i], pass ? "ok" : "MISMATCH");
        }
        printf("hardware ray tracing: %s\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 6;
    }
}
