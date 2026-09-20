// Real-geometry RT parity probe: load DarkPlaces world triangles (from
// collision_exportworldtris), build a Metal acceleration structure from them,
// fire deterministic rays, and compare Metal's hardware hits against an
// independent CPU Moller-Trumbore reference over the SAME triangle set. Answers
// "does the float Metal AS agree with a CPU intersector on real map geometry?"
//
// build: clang -O2 -fobjc-arc -framework Foundation -framework Metal metal/rtparity.m -o metal/rtparity
// run:   ./metal/rtparity <worldtris.txt> [numrays]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <simd/simd.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static const char *kSrc =
"#include <metal_stdlib>\n"
"#include <metal_raytracing>\n"
"using namespace metal;\n"
"using namespace raytracing;\n"
"kernel void rtparity(primitive_acceleration_structure accel [[buffer(0)]],\n"
"                     device const float3 *origins [[buffer(1)]],\n"
"                     device const float3 *dirs [[buffer(2)]],\n"
"                     constant float &tmax [[buffer(3)]],\n"
"                     device float *out [[buffer(4)]],\n"
"                     uint tid [[thread_position_in_grid]])\n"
"{\n"
"    ray r; r.origin = origins[tid]; r.direction = dirs[tid];\n"
"    r.min_distance = 0.0f; r.max_distance = tmax;\n"
"    intersector<triangle_data> isect;\n"
"    isect.set_triangle_cull_mode(triangle_cull_mode::none);\n"
"    intersection_result<triangle_data> res = isect.intersect(r, accel);\n"
"    out[tid] = (res.type == intersection_type::triangle) ? res.distance : -1.0f;\n"
"}\n";

// CPU reference: double-sided Moller-Trumbore, parametric t in [0,tmax), else -1
static float ray_tri(const float *o, const float *d, const float *a, const float *b, const float *c, float tmax) {
    float e1[3]={b[0]-a[0],b[1]-a[1],b[2]-a[2]}, e2[3]={c[0]-a[0],c[1]-a[1],c[2]-a[2]};
    float p[3]={d[1]*e2[2]-d[2]*e2[1], d[2]*e2[0]-d[0]*e2[2], d[0]*e2[1]-d[1]*e2[0]};
    float det=e1[0]*p[0]+e1[1]*p[1]+e1[2]*p[2];
    if (fabsf(det) < 1e-9f) return -1.0f;
    float inv=1.0f/det, tv[3]={o[0]-a[0],o[1]-a[1],o[2]-a[2]};
    float u=(tv[0]*p[0]+tv[1]*p[1]+tv[2]*p[2])*inv; if (u<0.0f||u>1.0f) return -1.0f;
    float q[3]={tv[1]*e1[2]-tv[2]*e1[1], tv[2]*e1[0]-tv[0]*e1[2], tv[0]*e1[1]-tv[1]*e1[0]};
    float v=(d[0]*q[0]+d[1]*q[1]+d[2]*q[2])*inv; if (v<0.0f||u+v>1.0f) return -1.0f;
    float t=(e2[0]*q[0]+e2[1]*q[1]+e2[2]*q[2])*inv; if (t<0.0f||t>=tmax) return -1.0f;
    return t;
}

static uint32_t rs = 0x1234567u;
static float frand(void){ rs = rs*1664525u + 1013904223u; return (float)(rs>>8)*(1.0f/16777216.0f); }

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) { printf("usage: %s <worldtris.txt> [numrays]\n", argv[0]); return 1; }
        int numrays = (argc >= 3) ? atoi(argv[2]) : 20000;

        // --- load geometry ---
        FILE *fp = fopen(argv[1], "r"); if (!fp) { printf("FAIL: cannot open %s\n", argv[1]); return 1; }
        char line[256]; int nv=0, nt=0;
        while (fgets(line, sizeof line, fp)) { if (sscanf(line,"verts %d",&nv)==1) break; }
        while (fgets(line, sizeof line, fp)) { if (sscanf(line,"tris %d",&nt)==1) break; }
        if (nv<3||nt<1) { printf("FAIL: bad header (verts %d tris %d)\n",nv,nt); return 1; }
        float *V = malloc(sizeof(float)*3*nv); uint32_t *T = malloc(sizeof(uint32_t)*3*nt);
        int gv=0, gt=0; unsigned a,b,c;
        while (fgets(line, sizeof line, fp) && gv<nv) if (sscanf(line,"v %f %f %f",&V[gv*3],&V[gv*3+1],&V[gv*3+2])==3) gv++;
        do { if (sscanf(line,"t %u %u %u",&a,&b,&c)==3){T[gt*3]=a;T[gt*3+1]=b;T[gt*3+2]=c;gt++;} } while (gt<nt && fgets(line,sizeof line,fp));
        fclose(fp);
        if (gv!=nv || gt!=nt) { printf("FAIL: parsed %d/%d verts, %d/%d tris\n",gv,nv,gt,nt); return 1; }
        printf("loaded %d verts, %d tris\n", nv, nt);

        float bmin[3]={V[0],V[1],V[2]}, bmax[3]={V[0],V[1],V[2]};
        for (int i=1;i<nv;i++) for (int k=0;k<3;k++){ float x=V[i*3+k]; if(x<bmin[k])bmin[k]=x; if(x>bmax[k])bmax[k]=x; }
        float diag=sqrtf((bmax[0]-bmin[0])*(bmax[0]-bmin[0])+(bmax[1]-bmin[1])*(bmax[1]-bmin[1])+(bmax[2]-bmin[2])*(bmax[2]-bmin[2]));
        float tmax = diag*2.0f;
        printf("bbox [%.0f %.0f %.0f]..[%.0f %.0f %.0f]  diag %.0f\n", bmin[0],bmin[1],bmin[2],bmax[0],bmax[1],bmax[2],diag);

        // --- Metal device + AS ---
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev || ![dev supportsRaytracing]) { printf("FAIL: no RT device\n"); return 1; }
        id<MTLBuffer> vbuf = [dev newBufferWithBytes:V length:sizeof(float)*3*nv options:MTLResourceStorageModeShared];
        id<MTLBuffer> ibuf = [dev newBufferWithBytes:T length:sizeof(uint32_t)*3*nt options:MTLResourceStorageModeShared];
        MTLAccelerationStructureTriangleGeometryDescriptor *tri = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        tri.vertexBuffer = vbuf; tri.vertexStride = sizeof(float)*3; tri.vertexFormat = MTLAttributeFormatFloat3;
        tri.indexBuffer = ibuf; tri.indexType = MTLIndexTypeUInt32; tri.triangleCount = nt;
        MTLPrimitiveAccelerationStructureDescriptor *pd = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        pd.geometryDescriptors = @[tri];
        MTLAccelerationStructureSizes sz = [dev accelerationStructureSizesWithDescriptor:pd];
        id<MTLAccelerationStructure> as = [dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
        id<MTLBuffer> scratch = [dev newBufferWithLength:sz.buildScratchBufferSize options:MTLResourceStorageModePrivate];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> bcb = [q commandBuffer];
        id<MTLAccelerationStructureCommandEncoder> ae = [bcb accelerationStructureCommandEncoder];
        [ae buildAccelerationStructure:as descriptor:pd scratchBuffer:scratch scratchBufferOffset:0];
        [ae endEncoding]; [bcb commit]; [bcb waitUntilCompleted];
        if (bcb.error) { printf("FAIL: AS build: %s\n", [[bcb.error localizedDescription] UTF8String]); return 2; }
        printf("AS built (%.1f MB) for %d tris\n", sz.accelerationStructureSize/1048576.0, nt);

        // --- deterministic rays ---
        simd_float3 *O = malloc(sizeof(simd_float3)*numrays), *D = malloc(sizeof(simd_float3)*numrays);
        for (int i=0;i<numrays;i++) {
            O[i] = (simd_float3){ bmin[0]+frand()*(bmax[0]-bmin[0]), bmin[1]+frand()*(bmax[1]-bmin[1]), bmin[2]+frand()*(bmax[2]-bmin[2]) };
            float dx,dy,dz,l; do { dx=frand()*2-1; dy=frand()*2-1; dz=frand()*2-1; l=sqrtf(dx*dx+dy*dy+dz*dz);} while (l<1e-3f);
            D[i] = (simd_float3){ dx/l, dy/l, dz/l };
        }
        id<MTLBuffer> obuf = [dev newBufferWithBytes:O length:sizeof(simd_float3)*numrays options:MTLResourceStorageModeShared];
        id<MTLBuffer> dbuf = [dev newBufferWithBytes:D length:sizeof(simd_float3)*numrays options:MTLResourceStorageModeShared];
        id<MTLBuffer> hbuf = [dev newBufferWithLength:sizeof(float)*numrays options:MTLResourceStorageModeShared];

        // --- Metal intersect ---
        NSError *err=nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:kSrc] options:[MTLCompileOptions new] error:&err];
        if (!lib) { printf("FAIL: shader: %s\n",[[err localizedDescription] UTF8String]); return 3; }
        id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"rtparity"] error:&err];
        if (!pso) { printf("FAIL: pso: %s\n",[[err localizedDescription] UTF8String]); return 4; }
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:pso];
        [ce setAccelerationStructure:as atBufferIndex:0];
        [ce setBuffer:obuf offset:0 atIndex:1]; [ce setBuffer:dbuf offset:0 atIndex:2];
        [ce setBytes:&tmax length:sizeof(float) atIndex:3]; [ce setBuffer:hbuf offset:0 atIndex:4];
        NSUInteger tg = pso.maxTotalThreadsPerThreadgroup; if (tg>256) tg=256;
        [ce dispatchThreads:MTLSizeMake(numrays,1,1) threadsPerThreadgroup:MTLSizeMake(tg,1,1)];
        [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
        if (cb.error) { printf("FAIL: dispatch: %s\n",[[cb.error localizedDescription] UTF8String]); return 5; }
        float *H = (float *)[hbuf contents];

        // --- CPU reference + compare ---
        int both_hit=0, both_miss=0, class_mismatch=0, dist_mismatch=0, shown=0;
        double maxerr=0, sumerr=0;
        for (int i=0;i<numrays;i++) {
            float o[3]={O[i].x,O[i].y,O[i].z}, d[3]={D[i].x,D[i].y,D[i].z};
            float best=-1.0f;
            for (int j=0;j<nt;j++) {
                float *A=&V[T[j*3]*3],*B=&V[T[j*3+1]*3],*C=&V[T[j*3+2]*3];
                float t=ray_tri(o,d,A,B,C,tmax);
                if (t>=0.0f && (best<0.0f || t<best)) best=t;
            }
            float m=H[i];
            int cpuhit=(best>=0.0f), methit=(m>=0.0f);
            if (!cpuhit && !methit) { both_miss++; continue; }
            if (cpuhit != methit) {
                class_mismatch++;
                if (shown<8){ printf("  CLASS mismatch ray %d: cpu %.3f metal %.3f\n", i, best, m); shown++; }
                continue;
            }
            both_hit++;
            double e=fabs((double)best-(double)m);
            float tol = 0.05f + 1e-4f*best;   // float precision at map scale
            sumerr+=e; if (e>maxerr) maxerr=e;
            if (e>tol) { dist_mismatch++; if (shown<8){ printf("  DIST  mismatch ray %d: cpu %.4f metal %.4f (err %.4f)\n", i, best, m, e); shown++; } }
        }
        printf("\nrays %d: both-hit %d, both-miss %d, class-mismatch %d, dist-mismatch %d\n",
               numrays, both_hit, both_miss, class_mismatch, dist_mismatch);
        printf("hit-distance error: max %.5f, mean %.6f (tol ~0.05 + 1e-4*t)\n", maxerr, both_hit?sumerr/both_hit:0.0);
        int pass = (class_mismatch*1000 <= numrays) && (dist_mismatch*1000 <= numrays); // <=0.1% each (edge/grazing)
        printf("RT PARITY vs CPU reference on real geometry: %s\n", pass ? "PASS" : "FAIL");
        return pass ? 0 : 6;
    }
}
