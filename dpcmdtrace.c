/*
QuakeM5 -- the command-digest gate (METAL.md Phase 1). See dpcmdtrace.h for
what this is and why it exists.

The whole file compiles to nothing without -DDP_CMDTRACE.
*/

#include "quakedef.h"
#include "dpcmdtrace.h"

#ifdef DP_CMDTRACE

#include "model_shared.h"   // r_meshbuffer_t
#include "r_textures.h"     // rtexture_t

// FNV-1a, 64-bit. The 32-bit form of exactly this already lives in the tree as
// Collision_ParityHash (collision.c) backing Collision_TraceParityTest, an
// existing determinism instrument -- same shape, widened, so the two read alike.
#define DPCMD_FNV_BASIS 14695981039346656037ULL
#define DPCMD_FNV_PRIME 1099511628211ULL

int dpcmd_level;

static unsigned long long dpcmd_hash = DPCMD_FNV_BASIS;
static unsigned long long dpcmd_calls;
static unsigned long long dpcmd_totalcalls;
static qfile_t *dpcmd_file;
static int dpcmd_frames;

static void DPCMD_Accum(const void *data, size_t len)
{
	const unsigned char *p = (const unsigned char *)data;
	size_t i;
	for (i = 0; i < len; i++)
		dpcmd_hash = (dpcmd_hash ^ p[i]) * DPCMD_FNV_PRIME;
}

void DPCMD_Call(int id)
{
	unsigned int v;
	if (!dpcmd_level)
		return;
	v = (unsigned int)id;
	DPCMD_Accum(&v, sizeof(v));
	dpcmd_calls++;
}

void DPCMD_I(long long v)
{
	// LEVEL 2+. At level 1 only the call ids are hashed, which is pure
	// STRUCTURE -- order and counts -- and cannot be perturbed by any value.
	if (dpcmd_level < 2)
		return;
	DPCMD_Accum(&v, sizeof(v));
}

void DPCMD_F(float v)
{
	// LEVEL 2 ONLY, deliberately. gl_rmain.c hands r_refdef.scene.time straight
	// to LavaParams, so hashing float bits makes the digest as timing-sensitive
	// as the byte gate it is meant to replace.
	if (dpcmd_level < 3)
		return;
	DPCMD_Accum(&v, sizeof(v));
}

void DPCMD_Bytes(const void *p, size_t len)
{
	if (dpcmd_level < 3 || !p || !len)
		return;
	DPCMD_Accum(p, len);
}

/*
GL object names are NOT stable across processes -- measured, not assumed. On a
frozen scene two boots produced an identical call COUNT (1645) and an identical
3-frame structure, but three completely different hashes, because the driver
hands out different texture and buffer numbers each run. Hashing them directly
would have made every comparison fail for a reason that has nothing to do with
the engine.

So a GL name is replaced by a SERIAL: the order in which this run first saw it.
The tenth distinct texture is 10 in every run, whatever the driver called it.
Creation order is deterministic for a deterministic scene, which is the property
we actually want to compare.
*/
#define DPCMD_SERIAL_SLOTS 16384   /* power of two; ~4k textures + buffers fits easily */

typedef struct dpcmd_serialtable_s
{
	int name[DPCMD_SERIAL_SLOTS];
	int serial[DPCMD_SERIAL_SLOTS];
	int next;
	int overflow;
}
dpcmd_serialtable_t;

static dpcmd_serialtable_t dpcmd_texserial, dpcmd_bufserial, dpcmd_objserial;

static long long DPCMD_Serial(dpcmd_serialtable_t *t, int glname)
{
	unsigned int i, h;
	if (!glname)
		return 0;   // 0 is "none" in GL and needs no serial
	h = ((unsigned int)glname * 2654435761u) & (DPCMD_SERIAL_SLOTS - 1);
	for (i = 0; i < DPCMD_SERIAL_SLOTS; i++)
	{
		unsigned int s = (h + i) & (DPCMD_SERIAL_SLOTS - 1);
		if (t->name[s] == glname)
			return t->serial[s];
		if (!t->name[s])
		{
			t->name[s] = glname;
			t->serial[s] = ++t->next;
			return t->serial[s];
		}
	}
	// full: degrade to a constant rather than to the unstable raw name, and
	// count it so the harness can see the instrument ran out of room
	t->overflow++;
	return -1;
}

void DPCMD_Buf(const struct r_meshbuffer_s *b)
{
	if (!dpcmd_level)
		return;
	DPCMD_I(DPCMD_Serial(&dpcmd_bufserial, b ? ((const r_meshbuffer_t *)b)->bufferobject : 0));
}

void DPCMD_Tex(const struct rtexture_s *t)
{
	// texnum read DIRECTLY -- never through R_GetTexture(), which uploads a
	// dirty texture as a side effect and would have the instrument change the
	// behaviour it is measuring. The struct prefix is exposed for exactly this.
	if (!dpcmd_level)
		return;
	DPCMD_I(DPCMD_Serial(&dpcmd_texserial, t ? ((const rtexture_t *)t)->texnum : 0));
}

void DPCMD_GLName(long long name)
{
	// framebuffer objects, program objects, and raw texnums passed as ints --
	// all driver-assigned, all unstable across processes, same treatment.
	if (!dpcmd_level)
		return;
	DPCMD_I(DPCMD_Serial(&dpcmd_objserial, (int)name));
}

static dpcmd_serialtable_t dpcmd_locserial;

void DPCMD_Loc(long long loc)
{
	// A uniform LOCATION is assigned by the driver at link time and is not
	// stable across processes. -1 is preserved because it is meaningful -- it
	// is the "this uniform is not in this permutation" guard the whole engine
	// branches on -- and every other value becomes a first-seen serial, so the
	// IDENTITY of the uniform is compared rather than the driver's numbering.
	if (dpcmd_level < 2)
		return;
	if (loc < 0) { DPCMD_I(-1); return; }
	DPCMD_I(DPCMD_Serial(&dpcmd_locserial, (int)loc + 1));
}

void DPCMD_Ptr(const void *p)
{
	// presence only. A client-array address is ASLR-dependent and hashing it
	// would make every run differ for no reason.
	DPCMD_I(p ? 1 : 0);
}

void DPCMD_Init(void)
{
	const char *lvl = getenv("DP_CMDTRACE");
	const char *out = getenv("DP_CMDTRACE_OUT");
	if (dpcmd_file)
		return;
	dpcmd_level = lvl ? atoi(lvl) : 0;
	if (dpcmd_level < 0) dpcmd_level = 0;
	if (dpcmd_level > 3) dpcmd_level = 3;
	if (!dpcmd_level)
		return;
	// Banner on stderr so a build that SHOULD be tracing but is not is
	// impossible to mistake for one that is -- the object dirs are keyed on
	// build type, not on CFLAGS, so a stale relink is a real hazard here.
	fprintf(stderr, "DP_CMDTRACE: command-digest build ACTIVE, level %d\n", dpcmd_level);
	fflush(stderr);
	if (!out || !*out)
	{
		Con_Printf(CON_ERROR "DP_CMDTRACE set but DP_CMDTRACE_OUT is not; no digest will be written\n");
		return;
	}
	// FS_OpenRealFile + FS_Printf, the same shape Collision_TraceParityTest uses
	// for its dump. Never stderr: it is non-blocking (sys_shared.c) and a
	// per-frame writer would silently drop lines under load.
	dpcmd_file = FS_OpenRealFile(out, "w", false);
	if (!dpcmd_file)
	{
		Con_Printf(CON_ERROR "DP_CMDTRACE: could not open \"%s\" for writing\n", out);
		return;
	}
	FS_Printf(dpcmd_file, "# QuakeM5 command-digest trace (METAL.md Phase 1)\n");
	FS_Printf(dpcmd_file, "# level %d (%s)\n", dpcmd_level,
	          dpcmd_level >= 3 ? "full: + float bits and array contents" : (dpcmd_level == 2 ? "values: calls + integer args" : "shape: calls, order and counts only"));
	FS_Printf(dpcmd_file, "# build %s\n", buildstring);
	FS_Printf(dpcmd_file, "# frame calls hash\n");
}

void DPCMD_FrameEnd(int framenumber)
{
	if (!dpcmd_level || !dpcmd_file)
		return;
	FS_Printf(dpcmd_file, "%d %llu %016llx\n", framenumber, dpcmd_calls, dpcmd_hash);
	dpcmd_totalcalls += dpcmd_calls;
	dpcmd_frames++;
	// reset per frame so one differing frame does not poison every later line --
	// the diff then points AT the frame that changed instead of everything after it
	dpcmd_hash = DPCMD_FNV_BASIS;
	dpcmd_calls = 0;
}

void DPCMD_Shutdown(void)
{
	if (!dpcmd_file)
		return;
	FS_Printf(dpcmd_file, "# frames %d totalcalls %llu\n", dpcmd_frames, dpcmd_totalcalls);
	FS_Close(dpcmd_file);
	dpcmd_file = NULL;
	Con_Printf("DP_CMDTRACE: %d frames, %llu backend calls\n", dpcmd_frames, dpcmd_totalcalls);
}

#endif // DP_CMDTRACE
