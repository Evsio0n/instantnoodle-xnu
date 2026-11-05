/*
 * Does the driver's device object, built as an ordinary library, get through Metal?
 *
 * metal_stubdev proved the contract with classes assembled at runtime. This asks the same
 * question of the real thing: /hackios/lib/libhackiosmetal.dylib, whose HKMetalDevice
 * is declared in source and linked against the private `_OBJC_CLASS_$__MTLDevice`. Whether
 * that symbol binds at all is the first thing this program measures -- if dyld cannot find
 * it, the library does not load and the driver has to go back to building its class at
 * runtime.
 *
 * Then the same three steps as before, and in the same order Metal uses them:
 *   step 4   [class isSubclassOfClass:_MTLDevice]
 *   step 5   [[class alloc] initWithAcceleratorPort:service]
 *   MTLAddDevice (0x181a1875c), then MTLCreateSystemDefaultDevice()
 *
 * Discovery is still bypassed, deliberately: no bundle, and the service still has neither
 * MetalPluginName nor MetalPluginClassName, because docs/metal.md section 7 measured what
 * happens when the whole system takes the Metal road with nothing behind it.
 *
 * With --bundle it loads the driver the way Metal's own discovery would -- through NSBundle,
 * out of /hackios/lib/HKMetal.bundle -- instead of dlopen'ing the dylib. That tests the
 * one part of discovery that cannot be reasoned about: whether a flat, unsigned bundle of
 * ours gives up its class to -[NSBundle classNamed:], whose identity test
 * ([NSBundle bundleForClass:cls] == self, 0x1811e0dac) is the thing most likely to refuse.
 * The service is still not given MetalPluginName/MetalPluginClassName, so Metal is not asked
 * to find anything: only the steps are walked, in its order.
 *
 * Run: /hackios/bin/metal_drv_test [--bundle] [LIBRARY [FUNCTION]]   (RC_JOBS=rc.jobs.metaldev)
 *
 * With no arguments it reads /System/Library/VideoDecoders/AV1SW.metallib and offers every
 * function in it to the compiler. With a FUNCTION it compiles and dispatches just that one,
 * which is how a kernel binding nothing but buffers can be picked: the driver has no
 * textures yet, and most of the system's shaders want them.
 *
 * With --picture [SECONDS] it draws the panel instead of a pixel: 64 of Apple's own
 * fragment shaders and Apple's own textured blit, at 1080x2400, put on the screen through
 * the kernel's GPU service so that `./xl screenshot` can take it. See picture_path below;
 * RC_JOBS=rc.jobs.metalpicture runs it.
 *
 * With --time [REPEATS] it measures what all of that COSTS -- a compile, a draw at the
 * panel's own size, a texture, a command buffer, a frame on the screen -- which no mode
 * above it has ever done. See time_path below; RC_JOBS=rc.jobs.metalperf runs it.
 *
 * With --draw-msaa [N] it draws one pass of N samples and reads back its resolve (M3c-3,
 * msaa_path, the msaa-result line); with --draw-one NAME it is --draw-all over the one
 * fragment function of that name (the drawall-result line).
 *
 * A FUNCTION dispatched ends in a compute-result line (M3d, compute_dispatch), and with
 * --compute-all [FROM COUNT] every kernel of the library is dispatched that way in one
 * process (compute_every), with a compute-summary line; both join on the kernel's name
 * across --backend gallium and --backend vk.
 *
 * M4 (TODO T38 (12)): what Metal COSTS on each back end, in "drvtest: cost ..." lines (the
 * section WHAT A WORKLOAD COSTS below says what is in one and how it is measured). --time
 * ends in one for the whole mode (W1); --draw-all and --compute-all with --quiet print no
 * per-shader line, only their summaries and one (W2, W3); --frames N owned|surface draws N
 * frames of one full-screen draw each into a 1080x2400 BGRA8 target, a texture of the
 * driver's own or an IOSurface made here, and prints the per-frame median and p90 (W4,
 * frames_path). --vk-noseen sets AIR_VK_NOSEEN=1 before Metal starts (air_vk.c then does
 * not map an imported surface once, so the kernel does not clean it at every submission).
 *
 * With --burst [N] (M4-1) it commits N empty command buffers and N small ones without waiting
 * for any, releasing what the small ones used right after each -commit, and then checks a
 * pixel of real work: the shape of run 071937's GPU faults, in a few seconds (burst_test, the
 * burst-result line). Every mode ends in "drvtest: exit BACKEND pending-pairs N in-flight M".
 *
 * With --order [METALLIB] (M4-1, the cost part) it checks the orders Metal promises around
 * -replaceRegion:, -getBytes: and a destroy, which air_vk.c no longer buys with a wait at each of
 * them (order_test, the order-result line).
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <IOKit/IOKitLib.h>
/* --frames N surface (M4, W4) draws into an IOSurface this program makes itself; linked
 * with -framework IOSurface -framework CoreFoundation */
#include <IOSurface/IOSurfaceRef.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <sys/ucontext.h>
#include <sys/sysctl.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
/* what every --pixels case must have drawn (M5a): the same model air_metal_test --pixels has */
#include "../../tools/mesa/air/draw_model.h"
#include "../../tools/mesa/air/tex_model.h"
#include "../../tools/mesa/air/adreno_srgb.h"
static int ca_fill(id<MTLDevice> dev);   /* --ca-fill, at the end of this file */
#include "../../tools/air/m5c_rows.h"   /* --m5c, the same rows real Metal runs (tools/air/mtlref) */

#define MTL_CREATE_DEVICE_UNSLID 0x181a34b3cUL
#define MTL_ADD_DEVICE_UNSLID    0x181a1875cUL
extern id MTLCreateSystemDefaultDevice(void);

/*
 * One call of the compiler library's own contract, declared here as hkmetal.m
 * declares the rest of it (tools/mesa/air/air_metal.h is in the Mesa tree and this
 * program must not grow a dependency on it): what textures a module declares.
 *
 * It is asked of the same /hackios/lib/libhackiosair.dylib the driver loaded -- dyld
 * uniques images by install_name, so this dlopen gets that image and not a second one --
 * and of the same `struct rwa_device` the driver holds, which HKMetalDevice hands
 * out through -mesaDevice. So the answer here is the answer the driver will judge a
 * binding by at the draw, and not a second opinion about the same shader.
 *
 * Why this program has to ask at all: it is in the position of an application that did
 * not write the shader and has no MTLTextureDescriptor to read the shape off. Before
 * this, --draw-all bound ONE kind and ONE format for a whole library, and 13 of the 271
 * fragment shaders of AppleMetalGLRenderer were refused by name -- they declare a
 * texture3d, a texture2d<int> or a multisample texture. That refusal was honest and it
 * was about this harness, which means 13 shaders went unmeasured for a reason that has
 * nothing to do with the GPU.
 */
struct rwa_device;

enum rwa_element {
	RWA_ELEMENT_UNKNOWN = 0,
	RWA_ELEMENT_FLOAT,
	RWA_ELEMENT_INT,
	RWA_ELEMENT_UINT,
};

struct rwa_texture_decl {
	unsigned index;
	unsigned type;           /* MTLTextureType */
	unsigned type_known;
	unsigned usage;          /* MTLTextureUsage */
	unsigned element;        /* enum rwa_element */
	unsigned element_bits;
	char metal_type[64];
};

static int (*rwa_module_textures)(struct rwa_device *, const void *, size_t,
				  const char *, struct rwa_texture_decl *, unsigned,
				  unsigned *, char *, size_t);
static struct rwa_device *air_device;

/*
 * How many members an MTLArgumentEncoder has recorded against (buffer, offset), asked of
 * the compiler library itself.
 *
 * It is the one thing about an encoder that cannot be seen from Metal's side: there is no
 * argument buffer to read back, because the frontend resolved every member of every set
 * to a binding and Metal's 8-byte handles mean nothing to a Gallium driver (docs/air.md
 * 8.2), so what an encoder leaves behind is a RECORD in the library and not bytes. This
 * reaches it the way -mesaDevice is reached above: the driver's own accessor for the
 * `struct rwa_buffer` behind an MTLBuffer, and then the library's own count.
 */
static unsigned (*rwa_argbuf_encoded_members)(const void *, size_t);

static unsigned
argbuf_records(id<MTLBuffer> buffer, size_t offset)
{
	SEL mesa = sel_getUid("mesaBuffer");

	if (rwa_argbuf_encoded_members == NULL || buffer == nil ||
	    ![buffer respondsToSelector:mesa]) {
		return 0;
	}
	const void *b = ((const void *(*)(id, SEL))objc_msgSend)(buffer, mesa);

	return b != NULL ? rwa_argbuf_encoded_members(b, offset) : 0;
}

/*
 * WHICH compiler library the driver loaded, by the driver's own rule (hkmetal.m,
 * mesa_available): HK_METAL_BACKEND=vk|gallium in the environment, else the boot-arg
 * metal_backend=vk, else Gallium's. The two libraries implement one contract under the same
 * rwa_* names, and the `struct rwa_device` -mesaDevice hands out belongs to exactly one of
 * them: asking the other library about it would read a Vulkan device as a Gallium screen.
 * So the query below must open the library the driver opened, and it opens it by this same
 * rule rather than by a fixed path. --backend (main) sets the variable before any device
 * exists, which is what lets one boot run this test once per back end.
 */
static bool
backend_is_vk(void)
{
	const char *env = getenv("HK_METAL_BACKEND");
	char args[1024] = "";
	size_t size = sizeof(args) - 1;

	if (env != NULL && (strcmp(env, "vk") == 0 || strcmp(env, "gallium") == 0)) {
		return strcmp(env, "vk") == 0;
	}
	return sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 &&
	       strstr(args, "metal_backend=vk") != NULL;
}

static const char *
compiler_library(void)
{
	return backend_is_vk() ? "/hackios/lib/libvulkan_freedreno.dylib"
			       : "/hackios/lib/libhackiosair.dylib";
}

/*
 * What the Vulkan back end still holds, by its own count (tools/mesa/air/air_vk.c,
 * rwa_vk_counts -- not part of air_metal.h): command buffers and fences out of its pool,
 * buffers and images alive. Printed only on that back end, after the draw and again after
 * this program has released what it made, so that "nothing leaked" is a pair of numbers
 * and not the absence of a crash. RTLD_NOLOAD: the driver has the library open already,
 * and if it has not, there is nothing to count.
 */
/* The same line, and the four numbers handed back in n[] for a caller that keeps a
 * running maximum (draw_every). False, with n[] left zero, when there is nothing to count:
 * another back end, or a library without rwa_vk_counts. */
static bool
print_vk_counts_into(const char *when, unsigned n[4])
{
	n[0] = n[1] = n[2] = n[3] = 0;
	if (!backend_is_vk()) {
		return false;
	}
	void *lib = dlopen("/hackios/lib/libvulkan_freedreno.dylib", RTLD_NOW | RTLD_NOLOAD);
	void (*counts)(unsigned[4]) = lib != NULL ?
	    (void (*)(unsigned[4]))dlsym(lib, "rwa_vk_counts") : NULL;

	if (counts == NULL) {
		printf("drvtest: vk counts %s: the Vulkan library is not loaded or has no "
		       "rwa_vk_counts\n", when);
		return false;
	}
	counts(n);
	printf("drvtest: vk counts %s: %u command buffers, %u fences, %u buffers, %u images "
	       "in use\n", when, n[0], n[1], n[2], n[3]);
	fflush(stdout);
	return true;
}

static void
print_vk_counts(const char *when)
{
	unsigned n[4];

	(void)print_vk_counts_into(when, n);
}

/*
 * ==========================================================================================
 * M4: WHAT A WORKLOAD COSTS, ON EACH BACK END (TODO T38 (12))
 * ==========================================================================================
 *
 * The owner wants the GPU's cost of Metal minimal, and both implementations of the compiler
 * library's contract (tools/mesa/air/air_metal.h) are right on the phone now: Gallium
 * (air_metal.c, libhackiosair) and Vulkan (air_vk.c, inside libvulkan_freedreno). So a
 * workload is run on each and priced, in ONE line per workload and back end:
 *
 *   drvtest: cost BACKEND WN WHAT: wall U us, cpu user U sys U us, gpu U us on S submissions
 *     (baseline gpu U/U us), pipelines U us, per-submission gpu U us; net gpu ...; UNITS N:
 *     ...; setup ...; backboardd frames ...; 0x7d read ...
 *
 * W1 is --time (time_path), W2 --draw-all --quiet, W3 --compute-all --quiet, W4 --frames.
 * Beside it, at the workload's start and end, one cost-env line each:
 *
 *   drvtest: cost-env BACKEND WN start|end at U us: soc T C, gpu T C, gpu clock M MHz
 *     (level L), backboardd frames F [(+N in M ms, R a second)] [THROTTLE-RISK ...]
 *
 * WHAT EACH NUMBER IS
 *
 *   wall        CLOCK_MONOTONIC across the workload (tm_now_ns).
 *   cpu         getrusage(RUSAGE_SELF) across it, user and system apart: every thread of the
 *               process, the driver's completion queue and the keeper below included. System
 *               time is where the kernel's work for us lands -- the submissions, the cache
 *               cleans of a surface at each of them (HKGPU.cpp submit), the waits.
 *   gpu         the 3D core's own time off CP_ALWAYS_ON_COUNTER, and how many submissions it
 *               timed: request kHKGPUWorked (0x7d) on HKGPU (iokit/Kernel/HKGPU.cpp, the
 *               case near its end; bsd/dev/hk_adreno.c hk_gpu_worked). RAW: everything the
 *               core finished in the workload's time, whoever submitted it (THE PRECONDITION).
 *   baseline    the same counter over an idle window right before the workload and one right
 *               after it, each scaled to the workload's wall time; their MEAN is subtracted.
 *   pipelines   the wall time spent making pipeline states inside the workload, apart from
 *               the steady-state draws and dispatches (cost_phase, COST_PIPELINE).
 *   per-submission gpu   (raw - mean baseline) / (raw submissions - mean baseline
 *               submissions): the core's time per submission of OURS.
 *   after the semicolon: the same net, the windows' own numbers, then THE PHASES' work part
 *               (UNITS: draws, dispatches, frames) with its core time and submissions per
 *               unit, the other phases' wall, backboardd's frame rate in the workload and in
 *               both windows, and what this instrument itself cost (reads of 0x7d).
 *   soc, gpu    tenths of a degree out of kern.hackios.thermal's table (bsd/dev/hk_thermal.c,
 *               read every 250 ms by the thermal service, which cpufreq=1 starts): the hottest
 *               of all its zones, and the hotter of gpuss-0/gpuss-1 -- the reading the GPU's
 *               speed policy holds the core low by (HKGPU.cpp gpuSpeed, hk_gpu_temperature).
 *               At or above gpu_hot (boot-arg, 850 = 85.0 C by default) at the END the line
 *               says THROTTLE-RISK: the next workload may run at the lowest level.
 *   gpu clock   msm's GET_PARAM MAX_FREQ (0x40, param 4) on the same connection, which
 *               hk_gpu_parameter answers with core_khz -- the level hk_gpu_set_level last
 *               gave the GMU (gpu_dcvs moves it every 50 ms): 305/400/525 MHz are levels 1/2/3
 *               on the OnePlus 8 (315/450/550 on the redwood).
 *   frames      kern.hackios.frames: the window server's swap number at the panel's last push
 *               (MiniIOMobileFramebuffer.cpp hk_display_frames, bsd/dev/hk_power.c; what
 *               rwtouch's `frames` step reads). Its difference over a period is backboardd's
 *               frames in it. `at U us` is rwtouch's t-mark clock, so the kernel's own
 *               "MiniIOMobileFramebuffer: N frames in M ms" lines can be put beside it.
 *
 * THE PRECONDITION: CAN THE CORE'S TIME BE TOLD APART PER CLIENT? NO -- so (b), a baseline.
 * Read 2026-11-05 in xnu-src: hk_gpu_submit has the core write CP_ALWAYS_ON_COUNTER into the
 * scratch page at both ends of every submission (ring_time: 64 slots at GPU_TIMES_AT, by
 * fence), and gpu_times() adds them into ONE pair of counters for the whole core (gpu_ticks,
 * gpu_timed_count): no client, submit queue or task is kept beside them, and hk_gpu_worked()
 * hands that pair out and zeroes it. The scratch page is the kernel's (PTE_PRIVILEGED under
 * gpu_pt) and no request maps it or answers one fence's times; MSM_SUBMITQUEUE_QUERY answers
 * 0 and MSM_PARAM_TIMESTAMP is mach_absolute_time, not the GPU's. gpu_log=1 prints every
 * submission's fence and time ("tl: Q", "tl: q"), but into the log, which a program cannot
 * read, and the Metal driver never sees its own fences. So the time is the WHOLE core's,
 * and on the home screen backboardd draws through it all the time (about 30 frames a
 * second, about 0.5 ms each). Hence the windows: COST_PRE_MS before (a fixed second,
 * because a workload's length is not known until it has run) and one after as long as the
 * workload was (up to COST_POST_MAX_MS), each scaled to the workload's length, both printed,
 * their mean subtracted. What that cannot see is backboardd drawing differently WHILE we use
 * the GPU, so its frames are counted over the workload and over both windows and printed
 * beside them: a rate in the workload far from the windows' says by how much of
 * backboardd's work the subtraction is off.
 *
 * WHY A KEEPER THREAD READS 0x7d EVERY 20 ms, AND WHY EVERY READ GOES THROUGH ONE PLACE.
 * hk_gpu_worked() RESETS the counters, and the kernel calls it too: HKGPU.cpp submit()'s
 * five-second report ("the GPU worked N us on each of M") takes and zeroes them whenever
 * five seconds of anybody's submissions have passed. A bracket of two reads that spans one
 * of those reports silently loses everything before it, and in a two-second window that is
 * likely, not rare. So while a workload or a window is measured, a thread of this program
 * reads every COST_KEEP_US that nothing else has read in, and adds what it got to this
 * process's running totals (cost_total); a report can then take at most 20 ms of counts. Every read of 0x7d in this
 * program goes through cost_worked_call, --time's own brackets included (tm_gpu_worked),
 * and a bracket is the difference of two snapshots of the totals -- so a read by the keeper
 * or by another bracket inside this one takes nothing away from anybody. The kernel's own
 * five-second report has gaps meanwhile; expected. The keeper costs a read (one ioctl under
 * the kernel's GPU lock) every 20 ms on another core; how many reads and how long they took
 * is the last field of the cost line.
 *
 * THE PHASES. Inside a workload the wall time is split four ways (cost_phase): setup (making
 * textures and buffers, filling them), pipelines, work (from -commandBuffer to
 * -waitUntilCompleted of the draw, dispatch or frame being priced) and readback (-getBytes:,
 * digests). 0x7d is read at both ends of every work phase, so the work's own core time and
 * submissions are known apart from the setup's and the readback's -- which differ between
 * the back ends for reasons that are not the draw's: on Vulkan every -replaceRegion: and
 * -getBytes: is a submission and a wait of its own (air_vk.c, ONE COMMAND BUFFER AT A TIME),
 * on Gallium a transfer map. That is what makes the unit costs comparable: a draw is one
 * Metal command buffer on both, and the cost line says per draw (dispatch, frame) how much
 * core time and how many submissions it took, as well as per submission. [Neither back end
 * flushes and waits per DRAW any more unless the boot has metal_sync=1: hkmetal.m asks both
 * libraries for rwa_device_defer_waits(1), so a Metal command buffer is one rwa_flush on
 * both (docs/air.md 9.3). How many kernel submissions that becomes is each back end's
 * business, and is what the "submissions each" field measures.]
 *
 * --quiet: A TIMED WORKLOAD'S OUTPUT IS HELD IN MEMORY. A sweep prints per shader, and so
 * does the driver (hkmetal.m: a line per pipeline, 272 in --draw-all), and every line to the
 * console is a write, a lock and the kernel's log: system time inside the measurement, and
 * about 45 KB of log per sweep, which a boot of twenty sweeps cannot hold. So with --quiet
 * stdout is pointed at an in-memory stream while the workload runs (cost_capture_start:
 * `stdout` is libc's __stdoutp, which printf reads at every call, the driver's rw_print
 * included) and digested after it (cost_replay): this test's own per-shader lines are
 * counted and dropped, its summaries printed as they were, and the driver's lines printed
 * once each with how many times they came. A signal meanwhile prints the held tail first
 * (fault, cost_capture_rescue).
 */
enum {
	COST_PRE_MS = 1000,             /* the idle window before a workload */
	COST_POST_MAX_MS = 3000,        /* ... and after it: as long as the workload, this at most */
	COST_POST_MIN_MS = 200,
	COST_KEEP_US = 20000,           /* how often the keeper reads 0x7d */
	COST_HELD_SHOWN = 24,           /* distinct held lines of the driver's printed, */
	COST_HELD_KINDS = 256,          /* ... and told apart */
	COST_HELD_KEPT = 64,            /* summaries replayed */
	COST_INVALID = -32768,          /* bsd/sys/hk_thermal.h THERMAL_INVALID: no reading */
};

struct cost_worked {                    /* iokit/Kernel/HKGPU.cpp struct hk_worked: 0x7d's answer */
	uint64_t microseconds;
	uint32_t submissions;
	uint32_t by[7];
};

/* The running totals, as a bracket marks them (cost_worked_since). */
struct cost_mark {
	uint64_t us, subs, by[7];
};

static uint64_t tm_now_ns(void);        /* the --time section's clock, below */

static pthread_mutex_t cost_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
	struct cost_mark sum;           /* everything 0x7d has answered this process */
	unsigned reads;
	uint64_t read_ns;
	uint64_t last_ns;               /* when the last read ended (the keeper skips a read after one) */
} cost_total;                           /* (cost_lock) */
static io_connect_t cost_connection;    /* our own HKGPU client: 0x7d and GET_PARAM */
static int cost_state;                  /* 0 not asked yet, 1 0x7d answers, -1 it cannot (cost_why) */
static char cost_why[200];
static bool cost_quiet;                 /* --quiet */

/* One request 0x7d on `c`, and what it answered added to the totals. */
static kern_return_t
cost_worked_call(io_connect_t c, struct cost_worked *out, size_t *size)
{
	struct cost_worked in = { 0, 0, { 0 } };
	uint64_t t0 = tm_now_ns();
	kern_return_t kr;

	pthread_mutex_lock(&cost_lock);
	kr = IOConnectCallStructMethod(c, 0x7d, &in, sizeof(in), out, size);
	if (kr == KERN_SUCCESS && *size >= sizeof(*out)) {
		cost_total.sum.us += out->microseconds;
		cost_total.sum.subs += out->submissions;
		for (int i = 0; i < 7; i++) {
			cost_total.sum.by[i] += out->by[i];
		}
	}
	cost_total.reads++;
	cost_total.last_ns = tm_now_ns();
	cost_total.read_ns += cost_total.last_ns - t0;
	pthread_mutex_unlock(&cost_lock);
	return kr;
}

/*
 * The same request, answered as "since this bracket's previous read" (`mark`), out of the
 * totals: what --time's tm_gpu_worked has always returned, without the keeper's reads in
 * between taking anything from it.
 */
static kern_return_t
cost_worked_since(io_connect_t c, struct cost_mark *mark, struct cost_worked *out, size_t *size)
{
	kern_return_t kr = cost_worked_call(c, out, size);

	if (kr != KERN_SUCCESS || *size < sizeof(*out)) {
		return kr;
	}
	pthread_mutex_lock(&cost_lock);
	out->microseconds = cost_total.sum.us - mark->us;
	out->submissions = (uint32_t)(cost_total.sum.subs - mark->subs);
	for (int i = 0; i < 7; i++) {
		out->by[i] = (uint32_t)(cost_total.sum.by[i] - mark->by[i]);
	}
	*mark = cost_total.sum;
	pthread_mutex_unlock(&cost_lock);
	return kr;
}

static void
cost_totals(uint64_t *us, uint64_t *subs)
{
	pthread_mutex_lock(&cost_lock);
	*us = cost_total.sum.us;
	*subs = cost_total.sum.subs;
	pthread_mutex_unlock(&cost_lock);
}

static bool
cost_open(void)
{
	if (cost_connection != 0) {
		return true;
	}
	io_service_t s = IOServiceGetMatchingService(0, IOServiceMatching("HKGPU"));
	kern_return_t kr = s != 0 ? IOServiceOpen(s, mach_task_self(), 0, &cost_connection)
				  : KERN_FAILURE;

	if (s != 0) {
		IOObjectRelease(s);
	}
	if (kr != KERN_SUCCESS) {
		cost_connection = 0;
		cost_state = -1;
		snprintf(cost_why, sizeof(cost_why), "no HKGPU client could be opened (%#x)", kr);
		return false;
	}
	return true;
}

/* Everything the core has finished so far, into the totals. False when 0x7d cannot answer. */
static bool
cost_gpu_sync(void)
{
	struct cost_worked w;
	size_t size = sizeof(w);

	if (cost_state < 0 || !cost_open()) {
		return false;
	}
	kern_return_t kr = cost_worked_call(cost_connection, &w, &size);

	if (kr != KERN_SUCCESS || size < sizeof(w)) {
		cost_state = -1;
		snprintf(cost_why, sizeof(cost_why), "request 0x7d answered %#x with %zu of %zu "
			 "bytes (0xe00002c7: a kernel without it, 0xe00002d8: its GPU not started)",
			 kr, size, sizeof(w));
		return false;
	}
	cost_state = 1;
	return true;
}

/*
 * THE KEEPER (the comment above): a read every COST_KEEP_US while something is measured --
 * but none when anything else has read within that time (the sweeps' phases read at every
 * draw), and none at all while it is paused (cost_keeper_pause: --frames' frame loop, which
 * reads at both ends of every frame itself, so that no wake of this thread lands in a
 * frame's own CPU time).
 */
static pthread_mutex_t cost_keeper_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cost_keeper_cond = PTHREAD_COND_INITIALIZER;
static bool cost_keeping, cost_keeper_paused;   /* (cost_keeper_mutex) */
static pthread_t cost_keeper_thread;

static void *
cost_keeper(void *unused)
{
	(void)unused;
	pthread_mutex_lock(&cost_keeper_mutex);
	while (cost_keeping) {
		struct timespec period = { 0, COST_KEEP_US * 1000L };
		uint64_t last;

		if (cost_keeper_paused) {
			pthread_cond_wait(&cost_keeper_cond, &cost_keeper_mutex);
			continue;
		}
		pthread_cond_timedwait_relative_np(&cost_keeper_cond, &cost_keeper_mutex, &period);
		if (!cost_keeping || cost_keeper_paused) {
			continue;
		}
		pthread_mutex_unlock(&cost_keeper_mutex);
		pthread_mutex_lock(&cost_lock);
		last = cost_total.last_ns;
		pthread_mutex_unlock(&cost_lock);
		if (tm_now_ns() - last >= COST_KEEP_US * 1000ull) {
			(void)cost_gpu_sync();
		}
		pthread_mutex_lock(&cost_keeper_mutex);
	}
	pthread_mutex_unlock(&cost_keeper_mutex);
	return NULL;
}

static void
cost_keeper_start(void)
{
	if (cost_keeping || cost_state <= 0) {
		return;
	}
	cost_keeping = true;
	cost_keeper_paused = false;
	if (pthread_create(&cost_keeper_thread, NULL, cost_keeper, NULL) != 0) {
		cost_keeping = false;
		printf("drvtest: cost: no keeper thread, so a five-second report of the kernel's "
		       "inside a bracket can take that bracket's counts\n");
	}
}

static void
cost_keeper_pause(bool pause)
{
	pthread_mutex_lock(&cost_keeper_mutex);
	cost_keeper_paused = pause;
	pthread_cond_signal(&cost_keeper_cond);
	pthread_mutex_unlock(&cost_keeper_mutex);
}

static void
cost_keeper_stop(void)
{
	pthread_mutex_lock(&cost_keeper_mutex);
	if (!cost_keeping) {
		pthread_mutex_unlock(&cost_keeper_mutex);
		return;
	}
	cost_keeping = false;
	pthread_cond_signal(&cost_keeper_cond);
	pthread_mutex_unlock(&cost_keeper_mutex);
	pthread_join(cost_keeper_thread, NULL);
}

/* rwtouch's t-mark clock, to the formula: (mach_absolute_time * numer / denom) / 1000 */
static uint64_t
cost_mark_us(void)
{
	static mach_timebase_info_data_t base;

	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	return (mach_absolute_time() * base.numer / base.denom) / 1000;
}

/* kern.hackios.frames, the window server's swap number; -1 without the sysctl */
static int
cost_frames(void)
{
	int frames = 0;
	size_t size = sizeof(frames);

	return sysctlbyname("kern.hackios.frames", &frames, &size, NULL, 0) == 0 ? frames : -1;
}

/*
 * The hottest zone of the thermal service's table and the hotter of the GPU's two, in
 * tenths: rows "NAME tsensB/S T C   limit ..., stop ..." (hk_thermal.c write_text), T "-"
 * for a sensor with no reading. COST_INVALID for both when the service does not run
 * (thermal=0, or no cpufreq=1: the table then says so in words and has no rows).
 */
static void
cost_temperatures(int *soc, int *gpu)
{
	static char text[4096];
	size_t size = sizeof(text) - 1;

	*soc = *gpu = COST_INVALID;
	if (sysctlbyname("kern.hackios.thermal", text, &size, NULL, 0) != 0) {
		return;
	}
	text[size < sizeof(text) ? size : sizeof(text) - 1] = '\0';
	for (char *line = text, *next; line != NULL && *line != '\0'; line = next) {
		char name[16], value[16];
		unsigned block, sensor;

		next = strchr(line, '\n');
		if (next != NULL) {
			*next++ = '\0';
		}
		if (sscanf(line, "%15s tsens%u/%u %15s", name, &block, &sensor, value) != 4 ||
		    strcmp(value, "-") == 0) {
			continue;
		}
		double c = strtod(value, NULL);
		int tenths = (int)(c * 10.0 + (c < 0 ? -0.5 : 0.5));

		if (*soc == COST_INVALID || tenths > *soc) {
			*soc = tenths;
		}
		if (strncmp(name, "gpuss-", 6) == 0 && (*gpu == COST_INVALID || tenths > *gpu)) {
			*gpu = tenths;
		}
	}
}

/* The 3D core's clock as the kernel last set it (GET_PARAM MAX_FREQ), MHz; 0 not known. */
static unsigned
cost_gpu_mhz(void)
{
	struct {
		uint32_t pipe, param;
		uint64_t value;
		uint32_t len, pad;
	} in = { 1, 0x04, 0, 0, 0 }, out = { 0, 0, 0, 0, 0 };     /* MSM_PIPE_3D0, MSM_PARAM_MAX_FREQ */
	size_t size = sizeof(out);

	if (!cost_open() ||
	    IOConnectCallStructMethod(cost_connection, 0x40, &in, sizeof(in), &out, &size) !=
	    KERN_SUCCESS || size < sizeof(out)) {
		return 0;
	}
	return (unsigned)(out.value / 1000000);
}

/* hk_adreno.c hk_gpu_level_khz: 1-3 are 305/400/525 MHz (SM8250), 315/450/550 (SM7325) */
static unsigned
cost_level(unsigned mhz)
{
	return mhz == 305 || mhz == 315 ? 1 : mhz == 400 || mhz == 450 ? 2 :
	       mhz == 525 || mhz == 550 ? 3 : 0;
}

/* boot-arg gpu_hot (tenths), 850 unless the boot says otherwise (hk_adreno.c) */
static int
cost_gpu_hot(void)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	const char *at;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 &&
	    (at = strstr(args, "gpu_hot=")) != NULL) {
		return atoi(at + 8);
	}
	return 850;
}

static const char *
cost_tenths(int t, char *buf, size_t size)
{
	if (t == COST_INVALID) {
		snprintf(buf, size, "-");
	} else {
		snprintf(buf, size, "%s%d.%d", t < 0 ? "-" : "", (t < 0 ? -t : t) / 10,
			 (t < 0 ? -t : t) % 10);
	}
	return buf;
}

/* A library's name in a cost line: the file's, or its bundle's for a default.metallib. */
static const char *
cost_library_name(const char *path, char *buf, size_t size)
{
	const char *slash = strrchr(path, '/'), *base = slash != NULL ? slash + 1 : path;

	if (strcmp(base, "default.metallib") == 0 && slash != NULL) {
		const char *p = slash;

		while (p > path && p[-1] != '/') {
			p--;
		}
		snprintf(buf, size, "%.*s", (int)(slash - p), p);
	} else {
		snprintf(buf, size, "%s", base);
	}
	return buf;
}

/* What the machine is doing around a workload: the cost-env line's numbers. */
struct cost_env {
	uint64_t at_us;                 /* the t-mark clock */
	int soc, gpu;                   /* tenths */
	unsigned mhz;
	int frames;
};

static void
cost_env_take(struct cost_env *e)
{
	e->at_us = cost_mark_us();
	cost_temperatures(&e->soc, &e->gpu);
	e->mhz = cost_gpu_mhz();
	e->frames = cost_frames();
}

/* A point of a bracket. */
struct cost_snap {
	uint64_t ns;
	struct rusage ru;
	uint64_t us, subs;
	int frames;
};

static void
cost_snap_take(struct cost_snap *s)
{
	(void)cost_gpu_sync();
	s->ns = tm_now_ns();
	getrusage(RUSAGE_SELF, &s->ru);
	cost_totals(&s->us, &s->subs);
	s->frames = cost_frames();
}

/* An idle window: the core's time and backboardd's frames while this process sleeps. */
struct cost_window {
	uint64_t ns, us, subs;
	int frames;
};

static void
cost_idle(unsigned ms, struct cost_window *w)
{
	struct cost_snap a, b;
	struct timespec left = { ms / 1000, (long)(ms % 1000) * 1000000L };

	cost_snap_take(&a);
	while (nanosleep(&left, &left) != 0 && errno == EINTR) {
	}
	cost_snap_take(&b);
	w->ns = b.ns - a.ns;
	w->us = b.us - a.us;
	w->subs = b.subs - a.subs;
	w->frames = a.frames >= 0 && b.frames >= 0 ? b.frames - a.frames : -1;
}

/* THE PHASES (the comment above). */
enum { COST_SETUP, COST_PIPELINE, COST_WORK, COST_READBACK, COST_PHASES };

static struct {
	bool active;
	int phase;
	uint64_t since_ns;              /* when the phase began */
	uint64_t work_us0, work_subs0;  /* the totals when the work phase began */
	uint64_t ns[COST_PHASES];
	uint64_t work_us, work_subs;    /* raw, over every work phase */
	uint64_t last_us, last_subs;    /* the last work phase's (a frame's, W4) */
	unsigned units;                 /* work phases */
	uint64_t pipeline_ns;           /* pipelines timed by their own code (--time's) */
} cost_run;

/*
 * Into `phase`. A read of 0x7d at both ends of a work phase, ordered so the read is not
 * inside it: read then clock going in, clock then read coming out. No-op outside a bracket,
 * so the sweeps call it unconditionally.
 */
static void
cost_phase(int phase)
{
	uint64_t now;

	if (!cost_run.active || phase == cost_run.phase) {
		return;
	}
	if (phase == COST_WORK) {
		(void)cost_gpu_sync();
		now = tm_now_ns();
		cost_totals(&cost_run.work_us0, &cost_run.work_subs0);
		cost_run.units++;
	} else if (cost_run.phase == COST_WORK) {
		uint64_t us, subs;

		now = tm_now_ns();
		(void)cost_gpu_sync();
		cost_totals(&us, &subs);
		cost_run.last_us = us - cost_run.work_us0;
		cost_run.last_subs = subs - cost_run.work_subs0;
		cost_run.work_us += cost_run.last_us;
		cost_run.work_subs += cost_run.last_subs;
	} else {
		now = tm_now_ns();
	}
	cost_run.ns[cost_run.phase] += now - cost_run.since_ns;
	cost_run.phase = phase;
	cost_run.since_ns = now;
}

/* A pipeline timed by its own code (--time's tm_make_pipeline), into the pipelines field */
static void
cost_pipeline_add(uint64_t ns)
{
	if (cost_run.active) {
		cost_run.pipeline_ns += ns;
	}
}

/* --quiet's capture (the comment above) */
static FILE *cost_real_stdout;
static FILE *volatile cost_held;        /* the in-memory stream stdout points at meanwhile */
static char *cost_held_bytes;           /* ... its bytes as of its last flush (open_memstream) */
static size_t cost_held_size;

static void
cost_capture_start(void)
{
	FILE *ms;

	fflush(stdout);
	ms = open_memstream(&cost_held_bytes, &cost_held_size);
	if (ms == NULL) {
		printf("drvtest: cost: no in-memory stream (errno %d), so this workload's output "
		       "goes to the console and is timed with it\n", errno);
		fflush(stdout);
		return;
	}
	cost_real_stdout = stdout;
	cost_held = ms;
	stdout = ms;
}

/*
 * Back to the console, and a copy of what was held, which the caller frees. The stream is
 * NOT closed: a thread of the driver's that read `stdout` a moment before the switch may
 * still be writing into it, and closing it would free the buffer under that thread. It is
 * a few hundred kilobytes for the rest of a short process.
 */
static char *
cost_capture_stop(size_t *length)
{
	FILE *ms = cost_held;
	char *copy;

	*length = 0;
	if (ms == NULL) {
		return NULL;
	}
	flockfile(ms);
	fflush(ms);
	copy = malloc(cost_held_size + 1);
	if (copy != NULL) {
		if (cost_held_size > 0 && cost_held_bytes != NULL) {
			memcpy(copy, cost_held_bytes, cost_held_size);
		}
		copy[cost_held_size] = '\0';
		*length = cost_held_size;
	}
	stdout = cost_real_stdout;
	cost_held = NULL;
	funlockfile(ms);
	return copy;
}

/* From the signal handler: the console back, and the held output's last 8 KB on it. */
static void
cost_capture_rescue(void)
{
	FILE *ms = cost_held;
	static const char say[] = "drvtest: cost: a signal while this workload's output was "
				  "held in memory (--quiet); its last bytes follow\n";

	if (ms == NULL) {
		return;
	}
	cost_held = NULL;
	stdout = cost_real_stdout;
	fflush(stdout);
	fflush(ms);
	if (cost_held_bytes != NULL && cost_held_size > 0) {
		size_t from = cost_held_size > 8192 ? cost_held_size - 8192 : 0;

		(void)write(1, say, sizeof(say) - 1);
		(void)write(1, cost_held_bytes + from, cost_held_size - from);
		(void)write(1, "\n", 1);
	}
}

/*
 * The held output, digested: this test's own lines dropped (counted) but for the
 * summaries, which are printed as they were and LAST, so a sweep still ends in them;
 * the driver's and the libraries' lines once each, with their counts.
 */
static const char *const cost_kept[] = {
	"drvtest: drawall-summary", "drvtest: drawall-depth", "drvtest: drawall: ",
	"drvtest: compute-summary", "drvtest: compute-all:", "drvtest: vk counts",
	"drvtest: cost", "drvtest: SIGNAL",
};

static void
cost_replay(const char *backend, const char *tag, char *held, size_t length)
{
	struct {
		const char *text;
		unsigned count;
	} kinds[COST_HELD_KINDS];
	const char *kept[COST_HELD_KEPT];
	unsigned nkinds = 0, nkept = 0, lines = 0, ours = 0, driver = 0, untold = 0;

	if (held == NULL) {
		return;
	}
	for (char *line = held, *next; *line != '\0'; line = next) {
		bool keep = false;

		next = strchr(line, '\n');
		if (next != NULL) {
			*next++ = '\0';
		} else {
			next = line + strlen(line);
		}
		if (*line == '\0') {
			continue;
		}
		lines++;
		if (strncmp(line, "drvtest: ", 9) == 0) {
			for (size_t k = 0; k < sizeof(cost_kept) / sizeof(cost_kept[0]); k++) {
				keep = keep || strncmp(line, cost_kept[k], strlen(cost_kept[k])) == 0;
			}
			if (keep && nkept < COST_HELD_KEPT) {
				kept[nkept++] = line;
			} else {
				ours++;
			}
			continue;
		}
		driver++;
		unsigned k;

		for (k = 0; k < nkinds && strcmp(kinds[k].text, line) != 0; k++) {
		}
		if (k < nkinds) {
			kinds[k].count++;
		} else if (nkinds < COST_HELD_KINDS) {
			kinds[nkinds].text = line;
			kinds[nkinds++].count = 1;
		} else {
			untold++;
		}
	}
	printf("drvtest: cost-held %s %s: %u lines (%zu bytes) were printed while the workload "
	       "ran and were held in memory: %u of this test's own, dropped; %u of the driver's "
	       "and the libraries', in %u%s distinct texts, each below once with its count; %u "
	       "summaries, last\n", backend, tag, lines, length, ours, driver, nkinds,
	       untold != 0 ? "+" : "", nkept);
	for (unsigned k = 0; k < nkinds && k < COST_HELD_SHOWN; k++) {
		printf("drvtest: cost-held %ux %.220s\n", kinds[k].count, kinds[k].text);
	}
	if (nkinds > COST_HELD_SHOWN || untold != 0) {
		unsigned more = untold;

		for (unsigned k = COST_HELD_SHOWN; k < nkinds; k++) {
			more += kinds[k].count;
		}
		printf("drvtest: cost-held ... and %u more lines in %u%s more distinct texts, not "
		       "shown\n", more, nkinds > COST_HELD_SHOWN ? nkinds - COST_HELD_SHOWN : 0,
		       untold != 0 ? "+" : "");
	}
	for (unsigned k = 0; k < nkept; k++) {
		printf("%s\n", kept[k]);
	}
	fflush(stdout);
}

/* The workload being measured: its two ends, its windows and the machine around it. */
static struct {
	char tag[8];
	struct cost_window pre, post;
	struct cost_snap a, b;
	struct cost_env start, end;
	unsigned reads0;
	uint64_t read_ns0;
} cost_cur;

static void
cost_env_say(const char *when, const struct cost_env *e, const struct cost_env *from)
{
	char soc[16], gpu[16], level[8] = "?";
	int hot = cost_gpu_hot();

	if (cost_level(e->mhz) != 0) {
		snprintf(level, sizeof(level), "%u", cost_level(e->mhz));
	}
	printf("drvtest: cost-env %s %s %s at %llu us: soc %s C, gpu %s C, gpu clock %u MHz "
	       "(level %s), backboardd frames %d", backend_is_vk() ? "vk" : "gallium",
	       cost_cur.tag, when, (unsigned long long)e->at_us,
	       cost_tenths(e->soc, soc, sizeof(soc)), cost_tenths(e->gpu, gpu, sizeof(gpu)),
	       e->mhz, level, e->frames);
	if (from != NULL && e->frames >= 0 && from->frames >= 0 && e->at_us > from->at_us) {
		double ms = (double)(e->at_us - from->at_us) / 1000.0;

		printf(" (+%d in %.0f ms, %.1f a second)", e->frames - from->frames, ms,
		       (e->frames - from->frames) * 1000.0 / ms);
	}
	if (from != NULL && e->gpu != COST_INVALID && e->gpu >= hot) {
		printf(" THROTTLE-RISK (gpu_hot %d.%d C: the core may be held at its lowest level)",
		       hot / 10, hot % 10);
	}
	printf("\n");
	fflush(stdout);
}

/*
 * The start of workload `tag`: the window before it, the machine, the capture (--quiet),
 * the first point. Everything up to cost_end is the workload.
 */
static void
cost_begin(const char *tag)
{
	memset(&cost_cur, 0, sizeof(cost_cur));
	memset(&cost_run, 0, sizeof(cost_run));
	snprintf(cost_cur.tag, sizeof(cost_cur.tag), "%s", tag);
	if (!cost_gpu_sync()) {
		printf("drvtest: cost %s %s: the core's own time is NOT measured: %s\n",
		       backend_is_vk() ? "vk" : "gallium", tag, cost_why);
	}
	pthread_mutex_lock(&cost_lock);
	cost_cur.reads0 = cost_total.reads;
	cost_cur.read_ns0 = cost_total.read_ns;
	pthread_mutex_unlock(&cost_lock);
	cost_keeper_start();
	cost_idle(COST_PRE_MS, &cost_cur.pre);
	cost_env_take(&cost_cur.start);
	cost_env_say("start", &cost_cur.start, NULL);
	if (cost_quiet) {
		printf("drvtest: cost %s %s: the workload's output is held in memory from here "
		       "until it ends (--quiet); a silence after this line is inside it\n",
		       backend_is_vk() ? "vk" : "gallium", tag);
		cost_capture_start();
	}
	cost_run.active = true;
	cost_run.phase = COST_SETUP;
	cost_snap_take(&cost_cur.a);
	cost_run.since_ns = cost_cur.a.ns;
}

static uint64_t
cost_tv_us(struct timeval t)
{
	return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_usec;
}

/*
 * The end of the workload, and its cost line. `units`/`unit` name the work phases (draws,
 * dispatches, frames; NULL when the workload has none, --time); `fmt` says what it was.
 */
static void
cost_end(const char *units, const char *unit, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void
cost_end(const char *units, const char *unit, const char *fmt, ...)
{
	const char *backend = backend_is_vk() ? "vk" : "gallium";
	char what[160];
	va_list ap;
	size_t held_length = 0;
	char *held;

	if (!cost_run.active) {
		return;
	}
	va_start(ap, fmt);
	vsnprintf(what, sizeof(what), fmt, ap);
	va_end(ap);
	cost_phase(COST_SETUP);                 /* a work phase still open is closed */
	cost_snap_take(&cost_cur.b);
	cost_run.ns[cost_run.phase] += cost_cur.b.ns - cost_run.since_ns;
	cost_run.active = false;
	held = cost_capture_stop(&held_length);
	cost_env_take(&cost_cur.end);
	cost_env_say("end", &cost_cur.end, &cost_cur.start);
	cost_replay(backend, cost_cur.tag, held, held_length);
	free(held);

	const struct cost_snap *a = &cost_cur.a, *b = &cost_cur.b;
	uint64_t wall = b->ns - a->ns;
	uint64_t post_ms = wall / 1000000;

	post_ms = post_ms < COST_POST_MIN_MS ? COST_POST_MIN_MS :
		  post_ms > COST_POST_MAX_MS ? COST_POST_MAX_MS : post_ms;
	cost_idle((unsigned)post_ms, &cost_cur.post);
	cost_keeper_stop();

	const struct cost_window *w1 = &cost_cur.pre, *w2 = &cost_cur.post;
	uint64_t user = cost_tv_us(b->ru.ru_utime) - cost_tv_us(a->ru.ru_utime);
	uint64_t sys = cost_tv_us(b->ru.ru_stime) - cost_tv_us(a->ru.ru_stime);
	uint64_t raw_us = b->us - a->us, raw_subs = b->subs - a->subs;
	/* the windows' rates, per nanosecond, and each scaled to the workload's length */
	double r1 = w1->ns ? (double)w1->us / w1->ns : 0, r2 = w2->ns ? (double)w2->us / w2->ns : 0;
	double q1 = w1->ns ? (double)w1->subs / w1->ns : 0;
	double q2 = w2->ns ? (double)w2->subs / w2->ns : 0;
	double b1 = r1 * wall, b2 = r2 * wall, s1 = q1 * wall, s2 = q2 * wall;
	double net_us = raw_us - (b1 + b2) / 2, net_subs = raw_subs - (s1 + s2) / 2;
	uint64_t pipelines = cost_run.ns[COST_PIPELINE] + cost_run.pipeline_ns;
	char per[32] = "-";
	unsigned reads;
	uint64_t read_ns;

	pthread_mutex_lock(&cost_lock);
	reads = cost_total.reads - cost_cur.reads0;
	read_ns = cost_total.read_ns - cost_cur.read_ns0;
	pthread_mutex_unlock(&cost_lock);
	if (cost_state <= 0) {
		printf("drvtest: cost %s %s %s: wall %llu us, cpu user %llu sys %llu us, gpu - us on "
		       "- submissions (baseline gpu -/- us), pipelines %llu us, per-submission gpu - "
		       "us; the core's own time NOT MEASURED: %s\n", backend, cost_cur.tag, what,
		       (unsigned long long)(wall / 1000), (unsigned long long)user,
		       (unsigned long long)sys, (unsigned long long)(pipelines / 1000), cost_why);
		fflush(stdout);
		return;
	}
	if (net_subs >= 0.5) {
		snprintf(per, sizeof(per), "%.1f", net_us / net_subs);
	}
	printf("drvtest: cost %s %s %s: wall %llu us, cpu user %llu sys %llu us, gpu %llu us on "
	       "%llu submissions (baseline gpu %.0f/%.0f us), pipelines %llu us, per-submission "
	       "gpu %s us; net gpu %.0f us on %.1f submissions, baseline submissions %.1f/%.1f, "
	       "windows %.0f/%.0f ms", backend, cost_cur.tag, what,
	       (unsigned long long)(wall / 1000), (unsigned long long)user,
	       (unsigned long long)sys, (unsigned long long)raw_us, (unsigned long long)raw_subs,
	       b1, b2, (unsigned long long)(pipelines / 1000), per, net_us, net_subs, s1, s2,
	       w1->ns / 1e6, w2->ns / 1e6);
	if (units != NULL && cost_run.units > 0) {
		double work = (double)cost_run.ns[COST_WORK];
		double wnet_us = cost_run.work_us - (r1 + r2) / 2 * work;
		double wnet_subs = cost_run.work_subs - (q1 + q2) / 2 * work;
		unsigned n = cost_run.units;

		printf("; %s %u: wall %.0f us, gpu %llu us on %llu submissions (net %.0f on %.1f), "
		       "per %s gpu %.1f us and wall %.1f us, %.2f submissions each", units, n,
		       work / 1000, (unsigned long long)cost_run.work_us,
		       (unsigned long long)cost_run.work_subs, wnet_us, wnet_subs, unit,
		       wnet_us / n, work / 1000 / n, wnet_subs / n);
	}
	printf("; setup %.0f us, readback %.0f us", cost_run.ns[COST_SETUP] / 1000.0,
	       cost_run.ns[COST_READBACK] / 1000.0);
	if (a->frames >= 0 && b->frames >= 0 && w1->frames >= 0 && w2->frames >= 0 && wall > 0) {
		printf("; backboardd frames %d in the workload (%.1f a second), %.1f/%.1f a second "
		       "in the windows", b->frames - a->frames, (b->frames - a->frames) * 1e9 / wall,
		       w1->ns ? w1->frames * 1e9 / w1->ns : 0, w2->ns ? w2->frames * 1e9 / w2->ns : 0);
	}
	printf("; 0x7d read %u times in %.0f us%s\n", reads, read_ns / 1000.0,
	       cost_cur.end.gpu != COST_INVALID && cost_cur.end.gpu >= cost_gpu_hot() ?
	       " THROTTLE-RISK" : "");
	fflush(stdout);
}

/* Both halves of the query, or neither: a table filled from a device that is not the
 * driver's would describe a shader nobody is about to draw. Prints what is missing and
 * leaves the sweep to its old single binding, which is a smaller measurement and not a
 * wrong one. */
static void
load_texture_query(id<MTLDevice> dev)
{
	void *lib = dlopen(compiler_library(), RTLD_NOW);

	if (lib == NULL) {
		printf("drvtest: no compiler library to ask what a shader declares: %s\n",
		       dlerror());
		return;
	}
	*(void **)&rwa_module_textures = dlsym(lib, "rwa_module_textures");
	/* and the argument-buffer record count, which --argbuf asks for and nothing else
	 * needs: a library without it is older than that mode and says so there. */
	*(void **)&rwa_argbuf_encoded_members = dlsym(lib, "rwa_argbuf_encoded_members");
	if (rwa_module_textures == NULL) {
		printf("drvtest: that compiler library has no rwa_module_textures, so it is "
		       "older than this test\n");
		return;
	}
	SEL mesa = sel_getUid("mesaDevice");

	if (![dev respondsToSelector:mesa]) {
		printf("drvtest: this device object does not answer -mesaDevice, so there is "
		       "no screen to read a module with\n");
		rwa_module_textures = NULL;
		return;
	}
	air_device = ((struct rwa_device *(*)(id, SEL))objc_msgSend)(dev, mesa);
	if (air_device == NULL) {
		printf("drvtest: the device has no Mesa screen (a boot without gpu=1), so "
		       "nothing can be read out of a module\n");
		rwa_module_textures = NULL;
	}
}

/* One function's module, as -[_MTLFunction bitcodeData] hands it over: the 0x0B17C0DE
 * wrapper and all, which the compiler library accepts either way. nil when this
 * function does not answer the selector, which is the one thing that would make the
 * whole query moot. */
static NSData *
function_bitcode(id<MTLFunction> fn)
{
	SEL bitcode = sel_getUid("bitcodeData");

	if (fn == nil || ![fn respondsToSelector:bitcode]) {
		return nil;
	}
	NSData *data = ((NSData *(*)(id, SEL))objc_msgSend)(fn, bitcode);

	return data != nil && [data length] >= 4 ? data : nil;
}

/* Steps 3 and 4 of Metal's discovery, by hand, against a bundle of ours. Returns the class
 * Metal would have ended up with, or Nil with the reason printed. */
static Class
class_from_bundle(const char *path)
{
	NSBundle *bundle = [NSBundle bundleWithPath:[NSString stringWithUTF8String:path]];

	if (bundle == nil) {
		printf("drvtest: NSBundle would not open %s\n", path);
		return Nil;
	}
	printf("drvtest: bundle %s, executable %s\n", path,
	       [bundle executablePath] != nil ? [[bundle executablePath] UTF8String] : "(none)");

	/* what step 4 does first, and the test that can refuse it */
	Class named = [bundle classNamed:@"HKMetalDevice"];

	printf("drvtest: [bundle classNamed:] gave %s\n",
	       named != Nil ? class_getName(named) : "nil -- the identity test refused");

	/* and the road Metal takes when that answers nil, which makes no such test */
	Class principal = [bundle principalClass];

	printf("drvtest: [bundle principalClass] gave %s\n",
	       principal != Nil ? class_getName(principal) : "nil");

	Class cls = named != Nil ? named : principal;
	if (cls == Nil) {
		return Nil;
	}
	if ([NSBundle bundleForClass:cls] == bundle) {
		printf("drvtest: and the runtime agrees the class came out of that bundle\n");
	}
	return cls;
}

/* One number over a run of bytes, computed the same way here and on the host, so that two
 * readers of the same module can be compared without carrying the module anywhere. */
static uint64_t
fnv1a(const unsigned char *bytes, size_t length)
{
	uint64_t hash = 0xcbf29ce484222325ULL;

	for (size_t i = 0; i < length; i++) {
		hash ^= bytes[i];
		hash *= 0x100000001b3ULL;
	}
	return hash;
}

/* Where the AIR of one function actually comes from.
 *
 * -[_MTLFunction bitcodeData] (0x181aef9e0) is @required on <MTLFunctionSPI> and returns the
 * function's module with LLVM's 0x0B17C0DE wrapper in front of it: Metal has already read
 * the container, found this function in it and kept the whole metallib mapped, so a driver
 * asks one declared selector and gets bytes. That is the junction with subsystem (1): the
 * frontend in docs/air.md reads exactly such a module.
 *
 * So this prints, for every function of the library, the wrapper's own idea of where the
 * module is and a checksum over it -- which tools/metallib.py computes over the same
 * function of the same file on the host. Equal numbers mean Metal and our own reader hand
 * the compiler the same bytes; different numbers would mean the frontend has been compiling
 * something other than what Metal would give it. */
static void
air_of_each_function(id<MTLLibrary> library)
{
	SEL bitcode = sel_getUid("bitcodeData");

	for (NSString *name in [library functionNames]) {
		id<MTLFunction> fn = [library newFunctionWithName:name];

		if (fn == nil) {
			printf("drvtest: air %-34s no function object\n", [name UTF8String]);
			continue;
		}
		if (![fn respondsToSelector:bitcode]) {
			printf("drvtest: air %-34s does not answer bitcodeData\n",
			       [name UTF8String]);
			continue;
		}
		NSData *data = ((NSData *(*)(id, SEL))objc_msgSend)(fn, bitcode);

		if (data == nil || [data length] < 20) {
			printf("drvtest: air %-34s bitcodeData gave %s\n", [name UTF8String],
			       data == nil ? "nil" : "less than a wrapper");
			continue;
		}
		const unsigned char *bytes = [data bytes];
		uint32_t magic = 0, offset = 0, size = 0;

		memcpy(&magic, bytes + 0, 4);
		memcpy(&offset, bytes + 8, 4);
		memcpy(&size, bytes + 12, 4);
		if (magic != 0x0b17c0deU || (size_t)offset + size > [data length]) {
			printf("drvtest: air %-34s wrapper %#x, offset %u, size %u of %lu -- "
			       "not what the format says\n", [name UTF8String], magic, offset,
			       size, (unsigned long)[data length]);
			continue;
		}
		printf("drvtest: air %-34s module %u bytes at %u of %lu, fnv1a %016llx\n",
		       [name UTF8String], size, offset, (unsigned long)[data length],
		       (unsigned long long)fnv1a(bytes + offset, size));
	}
	fflush(stdout);
}

/* The compute path, asked of the driver exactly as an application would ask.
 *
 * Every function of the library is offered to newComputePipelineStateWithFunction:, because
 * the interesting number is how many of them a driver on this port can compile at all, and
 * because a refusal carries the front end's own sentence about what it could not translate.
 * The first one that compiles and binds nothing but buffers is then dispatched for real:
 * queue, command buffer, encoder, commit, wait, and a look at the memory afterwards. */
/* What kind of texture to bind, and of what format, when nothing else says. A kernel
 * declaring `texture3d<half, sample>` is refused when a 2D texture is bound, and the refusal
 * names the kind -- which is useful once and useless twice, so --tex/--texfmt let the kind be
 * given on the command line after it has been read out of the shader's own air.texture
 * metadata on the machine.
 *
 * The draw sweep no longer needs that: rwa_module_textures answers what each shader declares
 * and --draw-all binds it per shader. These remain the compute path's binding and the draw
 * sweep's fallback for a slot no shader named, and --texslots below remains an override. */
static NSUInteger texture_kind = MTLTextureType2D;
static MTLPixelFormat texture_format = MTLPixelFormatRGBA32Float;

/*
 * And per slot, because a kernel can declare a different kind and element type in each:
 * one of the corpus's reads texture 0 as texture2d<uint> and texture 3 as texture2d<int>,
 * and a harness that binds one format everywhere is refused by name for the others. The
 * maps are filled from the shader's own air.texture entries, read on the machine; a slot
 * nobody named falls back to the single values above.
 *
 * Where --draw-all now reads the declaration itself, these are the deliberate mismatch: a
 * slot named here is bound what the command line says and not what the shader asked for,
 * which is the only way the driver's refusal can be tested at all.
 */
#define RW_TEX_SLOTS 16
static MTLPixelFormat slot_format[RW_TEX_SLOTS];
static NSUInteger slot_kind[RW_TEX_SLOTS];

static MTLPixelFormat
format_named(const char *name)
{
	static const struct { const char *name; MTLPixelFormat format; } formats[] = {
		{ "rgba8unorm", MTLPixelFormatRGBA8Unorm },
		{ "rgba16float", MTLPixelFormatRGBA16Float },
		{ "rgba16uint", MTLPixelFormatRGBA16Uint },
		{ "rgba16sint", MTLPixelFormatRGBA16Sint },
		{ "rgba32float", MTLPixelFormatRGBA32Float },
		{ "rgba32uint", MTLPixelFormatRGBA32Uint },
		{ "rgba32sint", MTLPixelFormatRGBA32Sint },
		{ "r32float", MTLPixelFormatR32Float },
		{ "r32uint", MTLPixelFormatR32Uint },
		/* the depth attachment formats iOS has, for --draw-depth. Depth24Unorm_Stencil8
		 * is absent because Metal's own header marks it API_UNAVAILABLE(ios) and the
		 * device answers NO to -isDepth24Stencil8PixelFormatSupported. */
		{ "depth16unorm", MTLPixelFormatDepth16Unorm },
		{ "depth32float", MTLPixelFormatDepth32Float },
		{ "depth32float_stencil8", MTLPixelFormatDepth32Float_Stencil8 },
		{ "stencil8", MTLPixelFormatStencil8 },
	};

	for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		if (strcmp(name, formats[i].name) == 0) {
			return formats[i].format;
		}
	}
	printf("drvtest: unknown texture format \"%s\"\n", name);
	return 0;
}

static NSUInteger
kind_named(const char *name)
{
	return strcmp(name, "3d") == 0 ? MTLTextureType3D :
	    strcmp(name, "cube") == 0 ? MTLTextureTypeCube :
	    strcmp(name, "1d") == 0 ? MTLTextureType1D :
	    strcmp(name, "2darray") == 0 ? MTLTextureType2DArray : MTLTextureType2D;
}

/* "0=2d:rgba32uint,3=2d:rgba32sint" -- one entry per slot the shader declares. */
static void
parse_slots(const char *spec)
{
	char buf[512];

	snprintf(buf, sizeof(buf), "%s", spec);
	for (char *tok = strtok(buf, ","); tok != NULL; tok = strtok(NULL, ",")) {
		unsigned index = 0;
		char kind[16] = { 0 }, fmt[32] = { 0 };

		if (sscanf(tok, "%u=%15[^:]:%31s", &index, kind, fmt) != 3 ||
		    index >= RW_TEX_SLOTS) {
			printf("drvtest: cannot read the slot spec \"%s\"\n", tok);
			continue;
		}
		slot_kind[index] = kind_named(kind);
		slot_format[index] = format_named(fmt);
	}
}

/* One texel, in whatever the texture's format is. __fp16 does the half conversion with the
 * hardware's own rounding rather than a hand-rolled one, which is the sort of thing that is
 * wrong in the last bit and then blamed on the GPU. */
static void
write_texel(unsigned char *at, MTLPixelFormat format, const float v[4])
{
	switch (format) {
	case MTLPixelFormatRGBA8Unorm:
		for (int c = 0; c < 4; c++) {
			float x = v[c] / 1000.0f;   /* the values run to 1400; scale to [0,1] */

			at[c] = (unsigned char)(x < 0 ? 0 : x > 1 ? 255 : x * 255.0f + 0.5f);
		}
		return;
	case MTLPixelFormatRGBA16Float: {
		__fp16 h[4];

		for (int c = 0; c < 4; c++) {
			h[c] = (__fp16)v[c];
		}
		memcpy(at, h, sizeof(h));
		return;
	}
	case MTLPixelFormatRGBA16Uint: {
		unsigned short u[4];

		for (int c = 0; c < 4; c++) {
			u[c] = (unsigned short)v[c];
		}
		memcpy(at, u, sizeof(u));
		return;
	}
	case MTLPixelFormatRGBA16Sint: {
		short s[4];

		for (int c = 0; c < 4; c++) {
			s[c] = (short)v[c];
		}
		memcpy(at, s, sizeof(s));
		return;
	}
	case MTLPixelFormatRGBA32Uint: {
		unsigned u[4];

		for (int c = 0; c < 4; c++) {
			u[c] = (unsigned)v[c];
		}
		memcpy(at, u, sizeof(u));
		return;
	}
	/*
	 * An integer format takes the NUMBER and not the bits of the float that held it,
	 * which is the convention air_run and air_metal_test both write by
	 * (`written.i[k] = (int32_t)v[c]`, air_metal_test.c) -- and the only one under
	 * which a shader's answer here and its answer there are the same measurement.
	 * Writing 100.0f's bits into a texture2d<int> would have the shader read
	 * 1120403456, a number that looks like a compiler bug.
	 *
	 * RGBA32Sint fell into the four-float case below until a slot could ask for it by
	 * name, which is why the case is here now and not in the first version of this.
	 */
	case MTLPixelFormatRGBA32Sint: {
		int s[4];

		for (int c = 0; c < 4; c++) {
			s[c] = (int)v[c];
		}
		memcpy(at, s, sizeof(s));
		return;
	}
	case MTLPixelFormatR32Uint: {
		unsigned u = (unsigned)v[0];

		memcpy(at, &u, sizeof(u));
		return;
	}
	case MTLPixelFormatR32Float:
		memcpy(at, v, sizeof(float));
		return;
	default:                            /* RGBA32Float and anything else four-float wide */
		memcpy(at, v, 4 * sizeof(float));
		return;
	}
}

/* write_texel backwards, for the one case where the test has to say what landed in a texture
 * rather than what it put there. Only the formats write_texel makes are read: a format it
 * cannot write cannot be in a texture this test seeded.
 */
static void
read_texel(const unsigned char *at, MTLPixelFormat format, float v[4])
{
	switch (format) {
	case MTLPixelFormatRGBA8Unorm:
		for (int c = 0; c < 4; c++) {
			v[c] = at[c] / 255.0f * 1000.0f;   /* write_texel's scale, undone */
		}
		return;
	case MTLPixelFormatRGBA16Float: {
		__fp16 h[4];

		memcpy(h, at, sizeof(h));
		for (int c = 0; c < 4; c++) {
			v[c] = (float)h[c];
		}
		return;
	}
	case MTLPixelFormatRGBA16Uint: {
		unsigned short u[4];

		memcpy(u, at, sizeof(u));
		for (int c = 0; c < 4; c++) {
			v[c] = (float)u[c];
		}
		return;
	}
	case MTLPixelFormatRGBA16Sint: {
		short t[4];

		memcpy(t, at, sizeof(t));
		for (int c = 0; c < 4; c++) {
			v[c] = (float)t[c];
		}
		return;
	}
	case MTLPixelFormatRGBA32Uint: {
		unsigned u[4];

		memcpy(u, at, sizeof(u));
		for (int c = 0; c < 4; c++) {
			v[c] = (float)u[c];
		}
		return;
	}
	case MTLPixelFormatRGBA32Sint: {
		int t[4];

		memcpy(t, at, sizeof(t));
		for (int c = 0; c < 4; c++) {
			v[c] = (float)t[c];
		}
		return;
	}
	case MTLPixelFormatR32Uint: {
		unsigned u;

		memcpy(&u, at, sizeof(u));
		v[0] = (float)u;
		return;
	}
	case MTLPixelFormatR32Float:
		memcpy(v, at, sizeof(float));
		return;
	default:
		memcpy(v, at, 4 * sizeof(float));
		return;
	}
}

/* How wide one texel of a format is, which is what a row's stride is counted in. It was
 * an expression inside the compute path, right for the three formats that path asked
 * for and wrong for R32Float (4 bytes read as 16) the moment a slot could name one. */
static unsigned
texel_bytes_of(MTLPixelFormat format)
{
	switch (format) {
	case MTLPixelFormatRGBA8Unorm:
	case MTLPixelFormatR32Float:
	case MTLPixelFormatR32Uint:
		return 4;
	case MTLPixelFormatRGBA16Float:
	case MTLPixelFormatRGBA16Uint:
	case MTLPixelFormatRGBA16Sint:
		return 8;
	default:                            /* the four-word RGBA32 formats */
		return 16;
	}
}

static MTLTextureDescriptor *
texture_descriptor(MTLPixelFormat format, MTLTextureUsage usage)
{
	MTLTextureDescriptor *td = [[[MTLTextureDescriptor alloc] init] autorelease];

	[td setPixelFormat:format];
	[td setTextureType:texture_kind];
	[td setWidth:64];
	[td setHeight:texture_kind == MTLTextureType1D ? 1 : 64];
	[td setDepth:texture_kind == MTLTextureType3D ? 16 : 1];
	[td setArrayLength:1];
	[td setMipmapLevelCount:1];
	[td setSampleCount:1];
	[td setUsage:usage];
	return td;
}

/*
 * THE RESULT LINE of a dispatch (M3d), one per kernel and back end in one shape whatever
 * happened, so the two runs of one boot (--backend gallium and --backend vk) can be joined
 * on the name the way --draw-all's drawall-result lines are:
 *
 *     drvtest: compute-result BACKEND NAME ran bN HEX ... tN HEX ...
 *     drvtest: compute-result BACKEND NAME ran unchanged
 *     drvtest: compute-result BACKEND NAME no-pipeline REASON
 *     drvtest: compute-result BACKEND NAME no-texture REASON
 *     drvtest: compute-result BACKEND NAME no-dispatch REASON
 *     drvtest: compute-result BACKEND NAME refused-dispatch status S: REASON
 *     drvtest: compute-result BACKEND NAME not-dispatched REASON
 *
 * `ran` lists every OUTPUT: each of the sixteen buffers that is no longer all zeros (bN,
 * FNV-1a over its 4096 bytes) and each texture that no longer holds air_run's texels (tN,
 * FNV-1a over slice 0 as -getBytes: returns it, rows and images tightly packed), in index
 * order; `unchanged` when the dispatch completed and changed nothing, which is a weak
 * agreement (docs/air.md section 5). A buffer or texture that one back end changed and the
 * other did not is then a field on one line only, so equal lines mean equal outputs.
 * REASON is the driver's sentence (the compiler library's, through the NSError) or this
 * harness's own; `not-dispatched` is --compute-all leaving out a kernel that loops on a
 * value its buffers do not hold (compute_hangers). The older lines (dispatched, buffer N)
 * stay as they were, because tools/air/metalcheck.py reads them.
 */
static int compute_dispatch(id<MTLDevice> dev, id<MTLCommandQueue> queue,
			    id<MTLComputePipelineState> first, NSString *firstName);

static const char *
backend_word(void)
{
	return backend_is_vk() ? "vk" : "gallium";
}

/* FNV-1a over slice 0 of a texture compute_dispatch made, read back whole: rows of 64 texels
 * and images of `height` rows tightly packed, which is the order its texels were written in,
 * so the digest of an untouched texture is the one computed while it was seeded. False when
 * there is nothing to read the bytes into. */
static bool
compute_texture_digest(id<MTLTexture> tex, uint64_t *digest)
{
	NSUInteger w = [tex width], h = [tex height], d = [tex depth];
	NSUInteger stride = w * texel_bytes_of([tex pixelFormat]);
	unsigned char *bytes = calloc(1, stride * h * d);

	if (bytes == NULL) {
		return false;
	}
	[tex getBytes:bytes
	  bytesPerRow:stride
	bytesPerImage:stride * h
	   fromRegion:MTLRegionMake3D(0, 0, 0, w, h, d)
	  mipmapLevel:0
		slice:0];
	*digest = fnv1a(bytes, stride * h * d);
	free(bytes);
	return true;
}

static void
compute_path(id<MTLDevice> dev, id<MTLLibrary> library, const char *only)
{
	NSArray<NSString *> *names = only != NULL ?
	    @[ [NSString stringWithUTF8String:only] ] : [library functionNames];
	unsigned compiled = 0, refused = 0;
	id<MTLComputePipelineState> first = nil;
	NSString *firstName = nil;

	for (NSString *name in names) {
		id<MTLFunction> fn = [library newFunctionWithName:name];
		NSError *err = nil;
		id<MTLComputePipelineState> ps = fn != nil ?
		    [dev newComputePipelineStateWithFunction:fn error:&err] : nil;

		if (ps == nil) {
			refused++;
			printf("drvtest: compute %-32s refused: %s\n", [name UTF8String],
			       err != nil ? [[err localizedDescription] UTF8String] : "(no reason)");
			/* the one kernel this run was asked for gets its result line here, since
			 * there is no dispatch to give it one (M3d) */
			if (only != NULL) {
				printf("drvtest: compute-result %s %s no-pipeline %s\n", backend_word(),
				       [name UTF8String],
				       fn == nil ? "the library has no function of that name"
				       : err != nil ? [[err localizedDescription] UTF8String]
						    : "(no reason)");
			}
			continue;
		}
		compiled++;
		printf("drvtest: compute %-32s compiled: %lu threads per group, width %lu, "
		       "%lu bytes of threadgroup memory\n", [name UTF8String],
		       (unsigned long)[ps maxTotalThreadsPerThreadgroup],
		       (unsigned long)[ps threadExecutionWidth],
		       (unsigned long)[ps staticThreadgroupMemoryLength]);
		if (first == nil) {
			first = [ps retain];
			firstName = [name retain];
		}
	}
	printf("drvtest: of %lu functions, %u compiled and %u were refused\n",
	       (unsigned long)[names count], compiled, refused);
	if (first == nil) {
		return;
	}

	/* and one real dispatch, through the queue and the command buffer */
	id<MTLCommandQueue> queue = [dev newCommandQueue];

	printf("drvtest: newCommandQueue gave %s\n",
	       queue != nil ? class_getName(object_getClass(queue)) : "nil");
	if (queue == nil) {
		printf("drvtest: compute-result %s %s no-dispatch the device gave no command "
		       "queue\n", backend_word(), [firstName UTF8String]);
		return;
	}
	(void)compute_dispatch(dev, queue, first, firstName);
}

/*
 * The dispatch of one compiled kernel, as compute_path always made it -- sixteen zeroed
 * buffers, sixteen textures of air_run's texels, a sampler, 4x4x1 threads in one group --
 * and then the compute-result line above. A function of its own since M3d so that
 * --compute-all (compute_every) dispatches every kernel of a library exactly the way the
 * one-kernel jobs (userland/rc.jobs.metal1/2/3, metalcompute) dispatch theirs, and so it
 * releases what it made: one process now makes many of these.
 *
 * Returns 0 for a dispatch that completed, 1 for one the driver refused or that did not
 * complete, 2 when this harness could not set it up.
 */
static int
compute_dispatch(id<MTLDevice> dev, id<MTLCommandQueue> queue,
		 id<MTLComputePipelineState> first, NSString *firstName)
{
	/* Eight buffers, all bound. How many a kernel reads is its own business -- the corpus's
	 * compute kernels take between one and five -- and binding fewer than it declares would
	 * make it read a slot nothing was bound to. Metal's own rule is that a shader sees only
	 * what it declared, so binding more is harmless. */
	enum { RW_TEST_BUFFERS = 16 };
	id<MTLBuffer> bufs[RW_TEST_BUFFERS] = { nil };
	/* what each texture held before the dispatch, as the result line digests it (M3d) */
	uint64_t seeded[RW_TEST_BUFFERS] = { 0 };
	int verdict = 2;

	for (int i = 0; i < RW_TEST_BUFFERS; i++) {
		bufs[i] = [dev newBufferWithLength:4096 options:0];
		if (bufs[i] == nil) {
			printf("drvtest: buffer %d could not be made\n", i);
			printf("drvtest: compute-result %s %s no-dispatch buffer %d could not be "
			       "made\n", backend_word(), [firstName UTF8String], i);
			for (int k = 0; k < i; k++) {
				[bufs[k] release];
			}
			return 2;
		}
		if ([bufs[i] contents] != NULL) {
			memset([bufs[i] contents], 0, 4096);
		}
	}
	/* Textures and a sampler, for the kernels that declare them. Eight of each, the same
	 * reasoning as the buffers: a shader sees only what it declared, and binding fewer than
	 * it declares makes it read a slot nothing was bound to. 64x64 RGBA8 is a shape the
	 * corpus's kernels work in; a format the compiler library cannot translate is refused by
	 * name, which is the answer this test wants either way. */
	id<MTLTexture> texs[RW_TEST_BUFFERS] = { nil };
	id<MTLSamplerState> sampler = nil;
	int textures_made = 0;

	/*
	 * Which format to ask for is not a free choice, and the first run of this taught it:
	 * asking for RGBA8Unorm that the shader writes was refused by name -- "Adreno 7c+ Gen 3
	 * cannot give MTLPixelFormatRGBA8Unorm as a MTLTextureType2D the shader writes" -- and
	 * with no texture made every texture-reading kernel then failed with "reads texture 0
	 * and 0 textures were bound", which is a true sentence about the harness and says
	 * nothing about the kernel.
	 *
	 * So the candidates are tried in order and the first that the hardware will give as a
	 * writable image wins. A kernel whose declaration wants another element type is still
	 * refused, and that refusal names what it wanted -- which is the measurement, because
	 * this test is in the position of an application that did not write the shader.
	 */
	const MTLPixelFormat candidates[] = {
		texture_format, MTLPixelFormatRGBA32Float, MTLPixelFormatRGBA32Uint,
		MTLPixelFormatRGBA16Float, MTLPixelFormatR32Float, MTLPixelFormatRGBA8Unorm,
	};
	MTLTextureDescriptor *td = nil;

	/* Read and write first, then read alone. The first run of this found that NOTHING is
	 * given as a texture a shader writes -- all five candidates refused -- and with no
	 * texture made every reading kernel failed with "reads texture 0 and 0 textures were
	 * bound", which is a sentence about this harness and not about any kernel. Falling back
	 * to read-only measures the eight that only read; the one that writes is then refused
	 * by the library, which is the honest state of things until that question is settled. */
	static const MTLTextureUsage usages[] = {
		MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite,
		MTLTextureUsageShaderRead,
	};

	for (size_t u = 0; u < sizeof(usages) / sizeof(usages[0]) && td == nil; u++) {
		for (size_t f = 0; f < sizeof(candidates) / sizeof(candidates[0]) && td == nil;
		     f++) {
			MTLTextureDescriptor *try = texture_descriptor(candidates[f], usages[u]);
			id<MTLTexture> probe;

			probe = [dev newTextureWithDescriptor:try];
			if (probe != nil) {
				[probe release];
				td = try;
				printf("drvtest: textures are format %lu, usage %lu (the first "
				       "this hardware gives)\n", (unsigned long)candidates[f],
				       (unsigned long)usages[u]);
			}
		}
	}
	if (td == nil) {
		printf("drvtest: no candidate pixel format can be made at all here\n");
	}
	for (int i = 0; td != nil && i < RW_TEST_BUFFERS; i++) {
		MTLTextureDescriptor *own = td;

		/* A slot the shader named gets what it asked for; the rest get what the probe
		 * above found the hardware will give. */
		if (i < RW_TEX_SLOTS && slot_format[i] != 0) {
			NSUInteger keep = texture_kind;

			texture_kind = slot_kind[i];
			own = texture_descriptor(slot_format[i], [td usage]);
			texture_kind = keep;
		}
		texs[i] = [dev newTextureWithDescriptor:own];
		if (texs[i] == nil) {
			printf("drvtest: slot %d (format %lu) was refused\n", i,
			       (unsigned long)[own pixelFormat]);
			break;
		}
		textures_made++;
		/*
		 * The texels are air_run's own (`texel_value`, air_run.c:92):
		 *
		 *     r = 100 + x + 1000 * slot,  g = 200 + y,  b = 300 + x + y + z,  a = 400
		 *
		 * and they are copied rather than invented for two reasons. The thousand per slot
		 * is deliberate there -- it makes a shader that reads the wrong slot return a
		 * visibly wrong texel instead of a plausible one, which air_run's comment records
		 * as the third mistake of that shape it met. And using the same values is what
		 * lets a texture-reading kernel's result be compared between softpipe here and
		 * the Adreno at all: the same shader over different texels is not a comparison.
		 */
		static unsigned char row[64 * 16];
		MTLPixelFormat pf = [own pixelFormat];
		unsigned texel_bytes = texel_bytes_of(pf);
		NSUInteger stride = 64 * texel_bytes;

		seeded[i] = 0xcbf29ce484222325ULL;   /* fnv1a's basis, folded row by row below */
		for (NSUInteger slice = 0; slice < [texs[i] arrayLength] *
		     (texture_kind == MTLTextureTypeCube ? 6 : 1); slice++) {
			for (NSUInteger z = 0; z < [texs[i] depth]; z++) {
				for (unsigned y = 0; y < 64; y++) {
					for (unsigned x = 0; x < 64; x++) {
						float v[4];

						v[0] = 100.0f + x + 1000.0f * i;
						v[1] = 200.0f + y;
						v[2] = 300.0f + x + y + (float)z;
						v[3] = 400.0f;
						write_texel(row + x * texel_bytes, pf, v);
					}
					/* the digest of what slice 0 holds now, in the order
					 * compute_texture_digest reads it back: z, then rows */
					if (slice == 0 && y < [texs[i] height]) {
						for (NSUInteger b = 0; b < stride; b++) {
							seeded[i] = (seeded[i] ^ row[b]) *
								    0x100000001b3ULL;
						}
					}
					[texs[i] replaceRegion:MTLRegionMake3D(0, y, z, 64, 1, 1)
						   mipmapLevel:0
							 slice:slice
						     withBytes:row
						   bytesPerRow:stride
						 bytesPerImage:0];
				}
			}
		}
	}
	MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];

	sampler = [dev newSamplerStateWithDescriptor:sd];
	[sd release];
	printf("drvtest: %d of %d textures made, sampler %s\n", textures_made, RW_TEST_BUFFERS,
	       sampler != nil ? class_getName(object_getClass(sampler)) : "nil");

	/* M4: the dispatch itself is the work phase of a timed sweep, from the command buffer
	 * to its wait (cost_phase; a no-op otherwise). The sixteen textures' uploads above are
	 * setup: on Vulkan each row is a submission and a wait of its own. */
	cost_phase(COST_WORK);
	id<MTLCommandBuffer> cmd = [queue commandBuffer];
	id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];

	printf("drvtest: commandBuffer %s, computeCommandEncoder %s\n",
	       cmd != nil ? class_getName(object_getClass(cmd)) : "nil",
	       enc != nil ? class_getName(object_getClass(enc)) : "nil");
	if (cmd == nil || enc == nil) {
		printf("drvtest: compute-result %s %s no-dispatch the device gave no command "
		       "buffer or no compute encoder\n", backend_word(), [firstName UTF8String]);
		goto release;
	}
	[enc setComputePipelineState:first];
	for (int i = 0; i < RW_TEST_BUFFERS; i++) {
		[enc setBuffer:bufs[i] offset:0 atIndex:i];
		if (texs[i] != nil) {
			[enc setTexture:texs[i] atIndex:i];
		}
		if (sampler != nil) {
			[enc setSamplerState:sampler atIndex:i];
		}
	}
	/* The same grid air_run uses -- 4x4x1 threads in one threadgroup -- so that the words
	 * this prints can be compared with what softpipe computes for the same kernel on the
	 * same (zero) input. A different thread count is a different computation for any kernel
	 * that writes per-thread, and then "the values agree" would mean nothing. */
	[enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(4, 4, 1)];
	[enc endEncoding];
	[cmd commit];
	[cmd waitUntilCompleted];
	cost_phase(COST_READBACK);
	printf("drvtest: dispatched %s: status %lu%s\n", [firstName UTF8String],
	       (unsigned long)[cmd status],
	       [cmd error] != nil ? [[NSString stringWithFormat:@", error: %@",
				      [[cmd error] localizedDescription]] UTF8String] : "");
	int written = 0;

	for (int which = 0; which < RW_TEST_BUFFERS; which++) {
		const unsigned char *bytes = [bufs[which] contents];
		int nonzero = 0;

		if (bytes == NULL) {
			continue;
		}
		for (int i = 0; i < 4096; i++) {
			if (bytes[i] != 0) {
				nonzero++;
			}
		}
		written += nonzero;
		if (nonzero == 0) {
			continue;   /* only the buffers that changed are worth a line */
		}
		printf("drvtest: buffer %d: %d non-zero bytes of 4096, first words %08x %08x "
		       "%08x %08x\n", which, nonzero, ((const unsigned *)bytes)[0],
		       ((const unsigned *)bytes)[1], ((const unsigned *)bytes)[2],
		       ((const unsigned *)bytes)[3]);
	}
	if (written == 0) {
		printf("drvtest: nothing was written to either buffer, so nothing is proven: this "
		       "kernel writes its buffer unconditionally\n");
	}

	/* The result line (M3d): a dispatch that did not complete is a refusal, with the
	 * driver's sentence; one that did lists every buffer and texture it changed, each by
	 * its digest. */
	if ([cmd status] != MTLCommandBufferStatusCompleted) {   /* RW_CB_COMPLETED, below */
		printf("drvtest: compute-result %s %s refused-dispatch status %lu: %s\n",
		       backend_word(), [firstName UTF8String], (unsigned long)[cmd status],
		       [cmd error] != nil ? [[[cmd error] localizedDescription] UTF8String]
					  : "(no error)");
		verdict = 1;
		goto release;
	}
	{
		char line[1024];
		size_t at = 0;
		unsigned changed = 0;

		line[0] = '\0';
		for (int which = 0; which < RW_TEST_BUFFERS; which++) {
			const unsigned char *bytes = [bufs[which] contents];
			bool zero = true;

			for (int i = 0; bytes != NULL && i < 4096 && zero; i++) {
				zero = bytes[i] == 0;
			}
			if (bytes == NULL || zero) {
				continue;
			}
			changed++;
			at += snprintf(line + at, at < sizeof(line) ? sizeof(line) - at : 0,
				       " b%d %016llx", which,
				       (unsigned long long)fnv1a(bytes, 4096));
			at = at < sizeof(line) ? at : sizeof(line) - 1;
		}
		for (int i = 0; i < RW_TEST_BUFFERS; i++) {
			uint64_t now;

			if (texs[i] == nil || !compute_texture_digest(texs[i], &now) ||
			    now == seeded[i]) {
				continue;
			}
			changed++;
			at += snprintf(line + at, at < sizeof(line) ? sizeof(line) - at : 0,
				       " t%d %016llx", i, (unsigned long long)now);
			at = at < sizeof(line) ? at : sizeof(line) - 1;
		}
		printf("drvtest: compute-result %s %s ran%s\n", backend_word(),
		       [firstName UTF8String], changed ? line : " unchanged");
		verdict = 0;
	}
release:
	fflush(stdout);
	for (int i = 0; i < RW_TEST_BUFFERS; i++) {
		[bufs[i] release];
		[texs[i] release];
	}
	[sampler release];
	return verdict;
}

/*
 * --compute-all [FROM COUNT] (M3d): every KERNEL of the library, in -functionNames order,
 * each compiled and dispatched by compute_dispatch exactly as a one-kernel job dispatches
 * its one, with a compute-result line each and a compute-summary line at the end -- the
 * compute counterpart of --draw-all, and like it given a slice: FROM counts kernels only
 * (fragment and vertex functions are not numbered), COUNT how many to attempt from there,
 * 60 unless given, so a library too long for one boot's log is several honest runs.
 *
 * WHAT EACH KERNEL IS BOUND: the sixteen zeroed buffers compute_dispatch always binds, and
 * for each texture slot the kernel DECLARES (rwa_module_textures, the compiler library's own
 * answer, asked of the driver's device as --draw-all asks it) a texture of the declared type
 * and of the element type and width it reads -- half RGBA16Float, ushort RGBA16Uint, short
 * RGBA16Sint, float/uint/int their RGBA32 formats -- which is what the one-kernel jobs of
 * userland/rc.jobs.metal1/2/3 spell out by hand with --texslots, for the same reason (a
 * texture2d<half> written through a 32-bit descriptor is a different store, docs/air.md
 * 5d). A slot --texslots names keeps what the command line says, as everywhere else in
 * this program: the deliberate mismatch stays possible.
 *
 * WHAT IS LEFT OUT unless --compute-hangers: the fourteen kernels of PortraitFilters that
 * tools/air/kstruct.py reads as leaving a loop only on a value from a buffer (a count, a
 * sentinel, a hash entry), which zeros do not hold, so a dispatch of one never ends and
 * the core has to be recovered (docs/air.md section 5; rc.jobs.metalall leaves out the same
 * fourteen). Each still gets its line, `not-dispatched`, with the reason.
 */
static bool compute_all;
static unsigned compute_from, compute_count = 60;
static bool compute_hangers_too;
/* kernels compute_every handed to compute_dispatch, for --quiet's cost line (M4, W3) */
static unsigned compute_every_dispatched;

static const char *const compute_hangers[] = {
	"MetalFaceMask_GenerateMask", "MetalFaceMask_GenerateToothMask",
	"MetalFaceMask_ConnectSpans", "MetalFaceMask_PropagateParents",
	"MetalFaceMask_ResolveAssociations", "MetalFaceMask_AddSeedPoint",
	"MetalFaceMask_DrawSpans", "MetalFaceMask_DrawEye", "fbs_bistochastize_iter",
	"fbs_bistochastize_final", "fbs_pcg_init", "fbs_pcg_iter1", "fbs_slice_trilinear",
	"confidence",
};

static bool
compute_is_hanger(const char *name)
{
	for (size_t i = 0; i < sizeof(compute_hangers) / sizeof(compute_hangers[0]); i++) {
		if (strcmp(name, compute_hangers[i]) == 0) {
			return true;
		}
	}
	return false;
}

/* The format compute_every binds a declared slot with: the element type AND its width,
 * because a kernel's image store is as wide as its element (ir3's stib.u16 for a half)
 * and the descriptor has to agree. 0 when the declaration names no element: the slot is
 * then bound what compute_dispatch's probe finds, as before. */
static MTLPixelFormat
compute_format_for(const struct rwa_texture_decl *d)
{
	bool narrow = d->element_bits == 16;

	switch (d->element) {
	case RWA_ELEMENT_FLOAT:
		return narrow ? MTLPixelFormatRGBA16Float : MTLPixelFormatRGBA32Float;
	case RWA_ELEMENT_UINT:
		return narrow ? MTLPixelFormatRGBA16Uint : MTLPixelFormatRGBA32Uint;
	case RWA_ELEMENT_INT:
		return narrow ? MTLPixelFormatRGBA16Sint : MTLPixelFormatRGBA32Sint;
	default:
		return 0;
	}
}

/* Fills slot_format/slot_kind for every slot `fn` declares and the command line did not
 * name (keep_format[] is the command line's). False and a sentence when the library cannot
 * say, which is that kernel's no-texture line. */
static bool
compute_slots_for(id<MTLFunction> fn, const MTLPixelFormat *keep_format, char *why,
		  size_t why_size)
{
	if (rwa_module_textures == NULL) {
		return true;   /* load_texture_query has said why; the probe's format stands */
	}
	NSData *bc = function_bitcode(fn);

	if (bc == nil) {
		snprintf(why, why_size, "it does not answer bitcodeData, so there is no module to "
			 "read a declaration out of");
		return false;
	}
	struct rwa_texture_decl decl[RW_TEX_SLOTS];
	unsigned n = 0;
	char reason[256] = "";

	if (rwa_module_textures(air_device, [bc bytes], [bc length], NULL, decl, RW_TEX_SLOTS,
				&n, reason, sizeof(reason)) != 0) {
		snprintf(why, why_size, "%s", reason);
		return false;
	}
	for (unsigned k = 0; k < n; k++) {
		unsigned slot = decl[k].index;

		if (slot >= RW_TEX_SLOTS) {
			snprintf(why, why_size, "it declares texture %u and this harness binds %u "
				 "slots", slot, RW_TEX_SLOTS);
			return false;
		}
		if (keep_format[slot] != 0) {
			continue;
		}
		MTLPixelFormat f = compute_format_for(&decl[k]);

		if (f == 0 && !decl[k].type_known) {
			continue;   /* nothing declared: the probe's format and kind stand */
		}
		slot_format[slot] = f != 0 ? f : texture_format;
		slot_kind[slot] = decl[k].type_known ? (NSUInteger)decl[k].type : texture_kind;
	}
	return true;
}

static void
compute_every(id<MTLDevice> dev, id<MTLLibrary> library)
{
	const char *backend = backend_word();
	unsigned seen = 0, attempted = 0, ran = 0, refused = 0, nopipe = 0, notex = 0,
		 skipped = 0, nodispatch = 0;
	MTLPixelFormat keep_format[RW_TEX_SLOTS];
	NSUInteger keep_kind[RW_TEX_SLOTS];
	id<MTLCommandQueue> queue = [dev newCommandQueue];

	if (queue == nil) {
		printf("drvtest: compute-all: the device gave no command queue\n");
		return;
	}
	load_texture_query(dev);
	memcpy(keep_format, slot_format, sizeof(keep_format));
	memcpy(keep_kind, slot_kind, sizeof(keep_kind));
	for (NSString *name in [library functionNames]) {
		@autoreleasepool {
			id<MTLFunction> fn = [library newFunctionWithName:name];
			const char *cname = [name UTF8String];

			if (fn == nil || [fn functionType] != MTLFunctionTypeKernel) {
				[fn release];
				continue;
			}
			if (seen++ < compute_from || attempted >= compute_count) {
				[fn release];
				continue;
			}
			attempted++;
			/* M4: the phases of a timed sweep (cost_phase; a no-op otherwise) */
			cost_phase(COST_SETUP);
			if (!compute_hangers_too && compute_is_hanger(cname)) {
				printf("drvtest: compute-result %s %s not-dispatched it leaves a loop only "
				       "on a value its buffers do not hold when they are zeros "
				       "(tools/air/kstruct.py), so the dispatch would not end; "
				       "--compute-hangers dispatches it anyway\n", backend, cname);
				skipped++;
				[fn release];
				continue;
			}
			NSError *err = nil;
			cost_phase(COST_PIPELINE);
			id<MTLComputePipelineState> ps =
			    [dev newComputePipelineStateWithFunction:fn error:&err];

			cost_phase(COST_SETUP);
			if (ps == nil) {
				const char *reason = err != nil ?
				    [[err localizedDescription] UTF8String] : "(no reason)";

				printf("drvtest: compute %-32s refused: %s\n", cname, reason);
				printf("drvtest: compute-result %s %s no-pipeline %s\n", backend, cname,
				       reason);
				nopipe++;
				[fn release];
				continue;
			}
			printf("drvtest: compute %-32s compiled: %lu threads per group, width %lu, "
			       "%lu bytes of threadgroup memory\n", cname,
			       (unsigned long)[ps maxTotalThreadsPerThreadgroup],
			       (unsigned long)[ps threadExecutionWidth],
			       (unsigned long)[ps staticThreadgroupMemoryLength]);

			char whynot[256] = "";

			memcpy(slot_format, keep_format, sizeof(keep_format));
			memcpy(slot_kind, keep_kind, sizeof(keep_kind));
			if (!compute_slots_for(fn, keep_format, whynot, sizeof(whynot))) {
				printf("drvtest: compute-result %s %s no-texture %s\n", backend, cname,
				       whynot);
				notex++;
			} else {
				compute_every_dispatched++;
				int v = compute_dispatch(dev, queue, ps, name);

				ran += v == 0;
				refused += v == 1;
				nodispatch += v == 2;
			}
			[ps release];
			[fn release];
		}
	}
	memcpy(slot_format, keep_format, sizeof(keep_format));
	memcpy(slot_kind, keep_kind, sizeof(keep_kind));
	printf("drvtest: compute-summary %s of %u kernels from %u: %u ran, %u refused-dispatch, "
	       "%u no-pipeline, %u no-texture, %u no-dispatch, %u not-dispatched (%u kernels in "
	       "the library)\n", backend, attempted, compute_from, ran, refused, nopipe, notex,
	       nodispatch, skipped, seen);
	fflush(stdout);
	[queue release];
	print_vk_counts("after --compute-all and its releases");
}

/*
 * One triangle, with one of Apple's own fragment shaders.
 *
 * `clearFragment20` out of AppleMetalGLRenderer's library writes render target 2 (int4) and
 * reads a 48-byte constant buffer at index 14. It is one of the 255 fragment shaders air_run
 * has drawn and compared pixel by pixel (docs/air.md), so there is a reference to compare
 * against -- and these four words ARE that reference, computed by air_run on softpipe with
 * the same (i * 7 + 13) & 0xff fill this function uses:
 *
 *     air_run: the pixel at (0, 0) of render target 2 came back as:
 *       2458616957 2930221209 3401825461 3873429713
 *     air_run: drew 4x4 into r32g32b32a32_uint, 3 render targets
 *
 * They are written down here rather than computed, which is a real weakness: if the shader's
 * meaning ever changes the constant stays and the test lies. tools/air/metalcheck.py is being
 * written to take the comparison off hardcoded numbers; until it does, the provenance above is
 * the only thing making this honest.
 */
/* Apple's own vertex shader, or the library's generated stage. `clearVertex` reads four
 * float4 corners out of a constant buffer at index 14 and indexes them by air.vertex_id --
 * no vertex attributes, which is why it can be served at all: the contract carries no
 * attribute layout, and all three of the corpus's vertex shaders turn out to read a buffer
 * instead (docs/metal.md 10.24). The corners written below are the ones the generated stage
 * covers, so the two roads draw the same pixels and a difference is a shader difference. */
static const char *vertex_function;
static const char *fragment_function = "clearFragment20";
/* --rtfmt NAME: the pixel format of --draw's three attachments, RGBA32Uint unless it is
 * given. It exists for one shader shape: a fragment function that writes ONE float, such as
 * QuartzCore's path_rect_frag_lpf (position and three flat varyings in, `coverage` out at
 * target 2, no buffer, texture or sampler at all -- the smallest draw the Vulkan back end
 * takes, milestone M3a). Into a four-channel attachment the three channels it does not
 * write hold whatever the back end leaves there, so two back ends could disagree about a
 * correct picture; into R32Float there is nothing else in the texel. */
static MTLPixelFormat draw_target_format = MTLPixelFormatRGBA32Uint;
/* --clear VALUE: what --draw's attachments are cleared to, all four channels, 0 unless it is
 * given. A shader that writes 0 everywhere (path_rect_frag_lpf over the generated stage: no
 * varyings reach it, so its rectangle is empty) leaves a 0-cleared target exactly as an
 * attachment nothing was drawn into: with a clear the shader does not produce, a digest of the
 * picture tells "drew 0" from "drew nothing" (run 011617, where both back ends read back 0). */
static double draw_clear;
static bool draw_all;
/* --draw-depth, which is the draw with a depth attachment under it and so with the first
 * depth test this port has ever run (depth_path). */
static bool draw_depth;
/* --draw-index, which is the draw that goes through an index buffer -- what every drawing
 * call in a real application is, and what this driver declined until now. It runs AFTER the
 * ordinary draw and over the same pipeline and attachments (index_draws), because the
 * measurement is a comparison: the same three indices drawn from two places in one buffer
 * must put the triangle in two different halves of the attachment. */
static bool draw_index;
/* --draw-attrib: the vertex attribute layout through Metal's own MTLVertexDescriptor, on the
 * hardware. Its own flag and not an option of --draw because what it draws is a different
 * pipeline out of two libraries, and because its verdicts are compared with each other and
 * not with the one picture --draw makes. */
static bool draw_attrib;
/* --argbuf: the argument encoder and the buffer offset, on the device. */
static bool argbuf;
/*
 * --draw-msaa [N] (M3c-3): a pass of N samples (4 unless given) drawn with the --draw
 * fragment function into three multisample attachments of --rtfmt's format, each resolved
 * at the end of the pass into a one-sample texture (MTLStoreActionMultisampleResolve with
 * a resolveTexture), and attachment 2's resolve read back. msaa_path says what is printed
 * and what each number has to be.
 */
static bool draw_msaa;
static unsigned msaa_samples = 4;
/* --draw-one NAME (M3c-3): the sweep of --draw-all over one fragment function, by name
 * instead of by index, in a process of its own: the same drawall and drawall-result lines,
 * so a single draw joins with a sweep's line on the name. */
static const char *draw_one;
/* --gallium-depth-stores: draw a fragment shader that stores [[depth]] on the Gallium back
 * end anyway. Without it the sweep refuses such a shader there by name, because that draw
 * times the GPU out on Gallium (docs/KNOWN_ISSUES.md K28) and a sweep should not wedge the
 * core to say so again; this flag is how K28 is measured on purpose. */
static bool gallium_depth_stores;
/* what the last draw_every attempted and drew, for --quiet's cost line (M4, W2) */
static unsigned draw_every_attempted, draw_every_drew;
/*
 * Which slice of the library to draw. Not a convenience: the loader keeps the first 256 KB
 * of a boot's log and the last 1 MB, dropping the middle, and the boot's own noise fills
 * most of that first 256 KB -- so about seventy lines of sweep survive per boot. Two runs
 * stopped at exactly 64 shaders and the limit looked like the driver's until the line number
 * gave it away: 2572 of 11480. A range turns that into five honest boots instead of one
 * number that is quietly a quarter of the answer.
 */
static unsigned draw_from, draw_count = 60;
/* --blit: the blit encoder alone (blit_test), then stop */
static bool blit_only;
static bool pixels_only;         /* --pixels: the mode's M5a form, pixels_path */
static unsigned tex_pixels;      /* M5b: --tex or --texfmt with --pixels: its group there */
/* --seed-rows writes the draw's 4x4 texture one row at a time, at a non-zero region origin,
 * which is how this test used to do it always. It is a flag now because that made every
 * measurement of that texture depend on the driver's origin.y, and because four rows landing
 * in one place is the second explanation of the flip in docs/metal.md 10.26.
 */
/* Metal's own MTLCommandBufferStatus: NotEnqueued 0, Enqueued 1, Committed 2, Scheduled 3,
 * Completed 4, Error 5. This test used to compare against the literal 6, which is what the
 * driver answered until it was corrected to Metal's numbering -- and 6 is not a status at
 * all, so an application comparing with MTLCommandBufferStatusCompleted saw completion the
 * moment -commit returned, before the GPU fence was paid. The driver prints its numbering
 * once per process; a log without that line was written by the old one.
 */
#define RW_CB_COMPLETED 4

static bool seed_rows;
/* --no-texdecl makes the sweep bind its single texture set to every shader, as it did
 * before each shader was given what it declares. It exists because the first sweep with
 * per-shader textures stopped at its third shader, and a sweep that cannot finish measures
 * nothing: with this, the 256 shaders that declare no texture are measurable again while
 * the 15 that do are measured one process at a time (userland/rc.jobs.metaltex).
 */
static bool use_decl = true;

/* 4x4, and 4x4x4 for a 3D slot, because that is air_run's TEX_SIZE and the shape it
 * draws against (air_run.c: a 2D texture 4x4, a 3D one 4x4x4). A texture of another
 * size here would make the device's pixel and the host's reference two different
 * measurements with one name. */
enum { DRAW_TEX = 4 };

/* A descriptor for one fragment-stage texture of the kind a slot asks for. Separate from
 * texture_descriptor() above, which is the compute path's 64x64 and reads the one global
 * kind: here the kind is per slot and comes from the shader. */
static MTLTextureDescriptor *
draw_texture_descriptor(NSUInteger type, MTLPixelFormat format, MTLTextureUsage usage)
{
	MTLTextureDescriptor *td = [[[MTLTextureDescriptor alloc] init] autorelease];

	[td setPixelFormat:format];
	[td setTextureType:type];
	[td setWidth:DRAW_TEX];
	[td setHeight:type == MTLTextureType1D ? 1 : DRAW_TEX];
	[td setDepth:type == MTLTextureType3D ? DRAW_TEX : 1];
	[td setArrayLength:1];
	[td setMipmapLevelCount:1];
	[td setSampleCount:1];
	[td setUsage:usage];
	return td;
}

/*
 * air_run's texels into one texture, every slice and every depth of it:
 *
 *     r = 100 + x + 1000 * slot,  g = 200 + y,  b = 300 + x + y + z,  a = 400
 *
 * The thousand per slot is why a shader that reads the wrong slot comes back visibly
 * wrong instead of plausibly right, and using air_run's own numbers is what lets the
 * pixel this draw produces be compared with the one softpipe produces at all.
 */
static void
fill_draw_texture(id<MTLTexture> tex, unsigned slot)
{
	MTLPixelFormat pf = [tex pixelFormat];
	unsigned bytes = texel_bytes_of(pf);
	unsigned char row[DRAW_TEX * 16];
	NSUInteger slices = [tex textureType] == MTLTextureTypeCube ? 6 : [tex arrayLength];

	for (NSUInteger slice = 0; slice < slices; slice++) {
		for (NSUInteger z = 0; z < [tex depth]; z++) {
			for (NSUInteger y = 0; y < [tex height]; y++) {
				for (NSUInteger x = 0; x < [tex width]; x++) {
					float v[4] = { 100.0f + x + 1000.0f * slot,
						       200.0f + y, 300.0f + x + y + z, 400.0f };

					write_texel(row + x * bytes, pf, v);
				}
				[tex replaceRegion:MTLRegionMake3D(0, y, z, [tex width], 1, 1)
				       mipmapLevel:0
					     slice:slice
					 withBytes:row
				       bytesPerRow:[tex width] * bytes
				     bytesPerImage:0];
			}
		}
	}
}

/*
 * The pixel format to bind for a declared element type: the 32-bit RGBA format of the
 * declared class, always.
 *
 * The width is deliberately not followed. What a binding is refused for is the CLASS --
 * "reads texture 0 as a texture2d<uint> and MTLPixelFormatRGBA32Float is a
 * floating-point or normalised format" -- and the three 32-bit formats are the ones
 * air_run and air_metal_test bind, so a pixel measured here and a pixel computed there
 * are the same measurement. Every texel value in the pattern (100 to 1400) is exact in
 * half and fits in a ushort, so a shader declaring `half` or `ushort` reads the same
 * numbers out of a 32-bit texture as out of a 16-bit one. --texslots still asks for a
 * 16-bit format by name when the point is the format rather than the shader.
 */
static MTLPixelFormat
format_for_element(unsigned element)
{
	switch (element) {
	case RWA_ELEMENT_INT:
		return MTLPixelFormatRGBA32Sint;
	case RWA_ELEMENT_UINT:
		return MTLPixelFormatRGBA32Uint;
	case RWA_ELEMENT_FLOAT:
		return MTLPixelFormatRGBA32Float;
	default:
		/* Nothing states an element type, so the slot is bound what this sweep bound
		 * every slot before it could ask -- draw_path's own dtex[] format. Not
		 * texture_format, which is the COMPUTE path's default: a pixel that changed
		 * because a fallback moved would look like a shader that changed. */
		return MTLPixelFormatRGBA32Uint;
	}
}

/*
 * A slot Metal declares as depth2d<...>, which takes a DEPTH texture and nothing else -- and
 * which this harness bound a colour one under until M3c-2.
 *
 * The corpus has exactly one: texturedFragment2DDepth declares its texture 0 as
 * `depth2d<float, sample>` (the only other depth declaration in AppleMetalGLRenderer's
 * library, depth2d_ms<float, read>, is a multisample texture neither back end makes).
 * rwa_module_textures reports that slot as a 2D texture of FLOAT elements, which is true and
 * is also exactly what an RGBA32Float texture is, so format_for_element bound one -- a
 * binding Metal itself would refuse (a depth2d argument takes a depth pixel format), and
 * one neither back end refused, because nothing compares under it and both read channel 0
 * of a colour texture as happily as the depth of a depth one. It is MTLPixelFormatDepth32Float
 * now, on both back ends, made ShaderRead (what the shader declares) and RenderTarget (so a
 * render pass can clear it).
 *
 * Only `depth2d<`: depth2d_ms and depth2d_array are other kinds, and the first is refused
 * as a multisample texture before a format matters.
 *
 * [M3c-3: the first is no longer refused -- both back ends make a multisample texture now,
 * and slot_is_depth2d_ms below gives a depth2d_ms slot a Depth32Float one of MS_SAMPLES.]
 */
static bool
slot_is_depth2d(const struct rwa_texture_decl *d)
{
	return strncmp(d->metal_type, "depth2d<", 8) == 0;
}

/*
 * A slot Metal declares as a MULTISAMPLE texture (M3c-3): texture2d_ms<T, read> or
 * depth2d_ms<float, read>, which rwa_module_textures reports as MTLTextureType2DMultisample.
 * Six of Apple's 271 fragment shaders declare one, and until M3c-3 this harness made it a
 * texture of ONE sample -- draw_texture_descriptor's sampleCount 1 -- which both compiler
 * libraries refuse by Metal's own rule ("MTLTextureDescriptor sampleCount must be > 1 for
 * multisample textures"), so the sweep said "no MTLTextureType 4 ... could be made" on both.
 *
 * MS_SAMPLES is four because that is the count both GPUs' drivers offer and Metal's
 * commonest: freedreno takes 2 and 4 (valid_sample_count) and turnip's a6xx 2 and 4
 * (sampleCounts). A depth2d_ms slot is a Depth32Float texture, as a depth2d one is.
 */
#define MS_SAMPLES 4

static bool
slot_is_multisample(const struct rwa_texture_decl *d)
{
	return d->type_known && d->type == MTLTextureType2DMultisample;
}

static bool
slot_is_depth2d_ms(const struct rwa_texture_decl *d)
{
	return strncmp(d->metal_type, "depth2d_ms<", 11) == 0;
}

/*
 * What a multisample colour slot is filled with: ONE colour in every sample of every texel,
 * texel (0, 0) of the pattern fill_draw_texture writes into slot `slot` --
 *
 *     r = 100 + 1000 * slot,  g = 200,  b = 300,  a = 400
 *
 * -- because a clear is the only way to fill a multisample texture that Metal has (it
 * cannot be MTLStorageModeShared, so -replaceRegion: cannot reach it, and both compiler
 * libraries refuse the write by name), and a clear fills every sample alike. Read at any
 * coordinate and any sample index it is that colour, so every mean over samples is that
 * colour too, which is what makes the six shaders' outputs computable here (ms_expect).
 */
static void
ms_seed(unsigned slot, double rgba[4])
{
	rgba[0] = 100.0 + 1000.0 * slot;
	rgba[1] = 200.0;
	rgba[2] = 300.0;
	rgba[3] = 400.0;
}

/* clear_depth_texture's twin for colour: a render pass whose colour attachment 0 is the
 * texture, MTLLoadActionClear with `rgba`, and no draw -- which reaches the compiler
 * library's rwa_clear_color on both back ends (hkmetal.m's recordPassEnd). */
static bool
clear_color_texture(id<MTLDevice> dev, id<MTLTexture> tex, const double rgba[4], char *why,
		    size_t why_size)
{
	id<MTLCommandQueue> queue = [dev newCommandQueue];
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	MTLRenderPassColorAttachmentDescriptor *ca = [[rp colorAttachments] objectAtIndexedSubscript:0];
	bool ok = false;

	[ca setTexture:tex];
	[ca setLoadAction:MTLLoadActionClear];
	[ca setClearColor:MTLClearColorMake(rgba[0], rgba[1], rgba[2], rgba[3])];
	[ca setStoreAction:MTLStoreActionStore];

	id<MTLCommandBuffer> cmd = [queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];

	if (enc == nil) {
		snprintf(why, why_size, "the pass that clears its multisample texture gave no "
			 "encoder");
	} else {
		[enc endEncoding];
		[cmd commit];
		[cmd waitUntilCompleted];
		ok = [cmd status] == RW_CB_COMPLETED;
		if (!ok) {
			snprintf(why, why_size, "the pass that clears its multisample texture to "
				 "(%g %g %g %g) ended with status %lu%s%s", rgba[0], rgba[1],
				 rgba[2], rgba[3], (unsigned long)[cmd status],
				 [cmd error] != nil ? ": " : "",
				 [cmd error] != nil ?
				 [[[cmd error] localizedDescription] UTF8String] : "");
		}
	}
	[queue release];
	return ok;
}

/*
 * What a depth2d slot is seeded with: ONE value in every texel, because there is no other way
 * to fill a depth texture that both back ends serve. -replaceRegion: on one is refused by name
 * on both (a depth texel has an aspect the call cannot name), so is a buffer-to-texture blit
 * (Gallium's plain_texture, and the Vulkan back end agrees), and a texture-to-texture copy
 * from R32Float is Gallium's alone (Vulkan copies a depth format only into itself). A render
 * pass that clears it and draws nothing reaches rwa_clear_depth_stencil on both.
 *
 * The value is texel (1, 1) of the depth seed --draw-depth uses for colour textures,
 * (1 + 4 + 0.5) / 64 = 0.0859375, exact as a float and neither 0 nor 1 -- so the 2D depth
 * shader still has to store 0.0859375 there. What it no longer says is WHICH texel was
 * sampled: every texel holds that one number, and the verdict lines say so.
 */
#define DEPTH2D_SEED 0.0859375

static bool
clear_depth_texture(id<MTLDevice> dev, id<MTLTexture> tex, double value, char *why,
		    size_t why_size)
{
	id<MTLCommandQueue> queue = [dev newCommandQueue];
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	MTLRenderPassDepthAttachmentDescriptor *za = [rp depthAttachment];
	bool ok = false;

	[za setTexture:tex];
	[za setLoadAction:MTLLoadActionClear];
	[za setClearDepth:value];
	[za setStoreAction:MTLStoreActionStore];

	id<MTLCommandBuffer> cmd = [queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];

	if (enc == nil) {
		snprintf(why, why_size, "the pass that clears its depth texture gave no encoder");
	} else {
		[enc endEncoding];
		[cmd commit];
		[cmd waitUntilCompleted];
		ok = [cmd status] == RW_CB_COMPLETED;
		if (!ok) {
			snprintf(why, why_size, "the pass that clears its depth texture to %g ended "
				 "with status %lu%s%s", value, (unsigned long)[cmd status],
				 [cmd error] != nil ? ": " : "",
				 [cmd error] != nil ?
				 [[[cmd error] localizedDescription] UTF8String] : "");
		}
	}
	[queue release];
	return ok;
}

static void
release_slots(id<MTLTexture> *tex, unsigned slots)
{
	for (unsigned i = 0; i < slots; i++) {
		[tex[i] release];
		tex[i] = nil;
	}
}

/*
 * The textures one fragment shader declares: made, filled with air_run's texels, and
 * left in out[slot] for the slots it named. False and a sentence when any of that
 * cannot be done, and then the sentence IS the measurement for that shader -- "it
 * declares texture 0 as texture2d_ms<float, read> and no MTLTextureType 4 ... could be
 * made" is a limit of the driver (it binds no multisample texture), where the old
 * "declares texture 0 as a texture2d_ms and the texture bound there is a 2d" was a
 * limit of this harness.
 *
 * [M3c-3: that sentence turned out to be the harness's after all -- it asked for a
 * multisample type of ONE sample, which Metal itself forbids, and Gallium had made
 * multisample textures all along. A multisample slot is made with MS_SAMPLES now and filled
 * by a clear (ms_seed), and a depth2d_ms slot by clear_depth_texture.]
 *
 * Without the query (an older compiler library, or a boot with no GPU) it does nothing
 * and says so by leaving every slot nil: the caller's single --texslots binding then
 * stands, which is exactly the behaviour this sweep had before.
 */
static bool
textures_for_shader(id<MTLDevice> dev, id<MTLFunction> fn, id<MTLTexture> *out,
		    unsigned slots, unsigned *declared, char *why, size_t why_size)
{
	*declared = 0;
	if (rwa_module_textures == NULL) {
		return true;
	}
	NSData *bc = function_bitcode(fn);

	if (bc == nil) {
		snprintf(why, why_size, "it does not answer bitcodeData, so there is no "
			 "module to read a declaration out of");
		return false;
	}
	struct rwa_texture_decl decl[RW_TEX_SLOTS];
	unsigned n = 0;
	char reason[256] = "";

	if (rwa_module_textures(air_device, [bc bytes], [bc length], NULL, decl,
				RW_TEX_SLOTS, &n, reason, sizeof(reason)) != 0) {
		snprintf(why, why_size, "%s", reason);
		return false;
	}
	*declared = n;
	for (unsigned k = 0; k < n; k++) {
		unsigned slot = decl[k].index;

		if (slot >= slots) {
			snprintf(why, why_size, "it declares texture %u and this harness binds "
				 "%u slots", slot, slots);
			return false;
		}
		/* --texslots stays an override, deliberately: binding the wrong thing on
		 * purpose is how the driver's refusal gets tested, and that can only be on
		 * purpose if a person can still ask for it. */
		bool overridden = slot_format[slot] != 0;

		/* A slot the module describes in no way at all -- a computed binding with no
		 * instruction and no air.arg_type_name -- is left to the caller's own
		 * binding rather than replaced by a guess of the same shape. No shader of the
		 * corpus is in this case; making a texture for it anyway would change what
		 * such a shader is bound without anything having asked.
		 */
		if (!overridden && !decl[k].type_known &&
		    decl[k].element == RWA_ELEMENT_UNKNOWN) {
			continue;
		}
		/* A depth2d slot gets a depth texture (slot_is_depth2d says why it did not),
		 * and RenderTarget beside what the shader declares, for the clear that seeds
		 * it. --texslots still overrides it, for the reason it overrides the rest.
		 *
		 * [M3c-3: and a multisample slot gets a multisample texture of MS_SAMPLES,
		 * Private (Metal's only storage mode for one), RenderTarget for the same
		 * reason -- its clear is a render pass -- and Depth32Float for depth2d_ms.] */
		bool multisample = !overridden && slot_is_multisample(&decl[k]);
		bool depth2d = !overridden &&
		    (slot_is_depth2d(&decl[k]) || slot_is_depth2d_ms(&decl[k]));
		MTLPixelFormat format = overridden ? slot_format[slot]
			: depth2d ? MTLPixelFormatDepth32Float
				  : format_for_element(decl[k].element);
		NSUInteger type = overridden ? slot_kind[slot]
			: (decl[k].type_known ? (NSUInteger)decl[k].type : texture_kind);
		MTLTextureDescriptor *td = draw_texture_descriptor(type, format,
		    (MTLTextureUsage)decl[k].usage |
		    (depth2d || multisample ? MTLTextureUsageRenderTarget : 0));

		if (multisample) {
			[td setSampleCount:MS_SAMPLES];
			[td setStorageMode:MTLStorageModePrivate];
		}
		out[slot] = [dev newTextureWithDescriptor:td];
		if (out[slot] == nil) {
			/* The driver has already printed the compiler library's own sentence
			 * about what it would not make; this one says which slot of which
			 * shader asked, which the library cannot know. */
			snprintf(why, why_size, "it declares texture %u as %s and no "
				 "MTLTextureType %lu of MTLPixelFormat %lu could be made for "
				 "it", slot,
				 decl[k].metal_type[0] != '\0' ? decl[k].metal_type :
				 "a texture whose metadata carries no type name",
				 (unsigned long)type, (unsigned long)format);
			return false;
		}
		if (depth2d) {
			char cleared[200] = "";

			if (!clear_depth_texture(dev, out[slot], DEPTH2D_SEED, cleared,
						 sizeof(cleared))) {
				snprintf(why, why_size, "it declares texture %u as %s, and %s",
					 slot, decl[k].metal_type, cleared);
				return false;
			}
			continue;
		}
		/* a multisample colour slot is cleared to ms_seed(slot) in every sample, the
		 * only fill there is for one (ms_seed says why) */
		if (multisample) {
			char cleared[200] = "";
			double rgba[4];

			ms_seed(slot, rgba);
			if (!clear_color_texture(dev, out[slot], rgba, cleared, sizeof(cleared))) {
				snprintf(why, why_size, "it declares texture %u as %s, and %s",
					 slot, decl[k].metal_type, cleared);
				return false;
			}
			continue;
		}
		fill_draw_texture(out[slot], slot);
	}
	return true;
}

static void draw_every(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue,
		       MTLRenderPassDescriptor *rp, id<MTLBuffer> uniform, id<MTLTexture> *dtex,
		       id<MTLSamplerState> dsamp, id<MTLTexture> *targets);

/*
 * The indexed draws, run after the ordinary one and over its pipeline, attachments and
 * bindings -- so that the only thing that differs between the reference picture and these
 * is the index buffer. docs/metal.md 10.35 is the experiment and air_metal_test
 * --draw-index is its reference implementation on softpipe, step for step.
 *
 * The index buffer holds 0 1 2 3 1 2 3. Three indices out of it make one triangle of the
 * quad: 0,1,2 is the half of the attachment below the diagonal, whose right angle is the
 * pixel at (0, 0), and 1,2,3 is the other half, whose right angle is (3, 3). Those two
 * draws differ in NOTHING but indexBufferOffset, so a driver that ignored the index buffer
 * or the offset would draw the first triangle twice and (3, 3) would come back as the clear
 * colour both times. The four pixels on the diagonal have their centres exactly on the
 * shared edge and belong to whichever triangle the rasteriser's fill rule gives them, so
 * the covered count is printed and nothing is concluded from it -- the two right angles are
 * the verdict.
 */
static const uint16_t index_list[7] = { 0, 1, 2, 3, 1, 2, 3 };

/* Whether pixel (x, y) of a 4x4 readback was drawn at all. The clear colour is zero and
 * clearFragment20's first word is 2458616957, so a non-zero first word is a covered pixel
 * -- which the caller's own four words have already confirmed before this is used. */
static bool
index_at(const unsigned grid[16][4], unsigned x, unsigned y)
{
	return grid[y * 4 + x][0] != 0;
}

/* How many of the 64 words of two pictures differ: the whole attachment and not one pixel,
 * because two triangles that differ only along the diagonal differ in four pixels. */
static unsigned
index_diff(const unsigned a[16][4], const unsigned b[16][4])
{
	unsigned n = 0;

	for (unsigned i = 0; i < 16; i++) {
		for (unsigned c = 0; c < 4; c++) {
			n += a[i][c] != b[i][c];
		}
	}
	return n;
}

/* One picture, as four rows of four, with the two corners the verdicts are made of. */
static void
index_print(unsigned n, const char *what, const unsigned grid[16][4])
{
	unsigned covered = 0;

	printf("drvtest: index: %u. %s: ", n, what);
	for (unsigned y = 0; y < 4; y++) {
		for (unsigned x = 0; x < 4; x++) {
			printf("%c", index_at(grid, x, y) ? '#' : '.');
			covered += index_at(grid, x, y);
		}
		printf("%s", y < 3 ? "/" : "");
	}
	printf("  %u of 16 pixels, (0,0) %s, (3,3) %s\n", covered,
	       index_at(grid, 0, 0) ? "drawn" : "clear",
	       index_at(grid, 3, 3) ? "drawn" : "clear");
}

static void
index_draws(id<MTLDevice> dev, id<MTLCommandQueue> queue, MTLRenderPassDescriptor *rp,
	    id<MTLRenderPipelineState> pipeline, id<MTLBuffer> uniform,
	    id<MTLBuffer> corners, id<MTLBuffer> coords, id<MTLTexture> *dtex,
	    id<MTLSamplerState> dsamp, id<MTLTexture> *targets)
{
	/* Step 0 is the draw the caller has just made and whose pixels are still on the
	 * attachment: the non-indexed strip of four. Everything below is compared with it or
	 * with another step of it. */
	static const struct {
		const char *what;
		bool wide;                /* MTLIndexTypeUInt32 rather than UInt16 */
		NSUInteger count, offset;
		NSInteger baseVertex;
		bool must_fail;
	} step[] = {
		{ "the indexed strip of 4 [0 1 2 3]",             false, 4, 0,     0, false },
		{ "indices [0 1 2]: the half below the diagonal", false, 3, 0,     0, false },
		{ "indices [1 2 3]: the half above it",           false, 3, 4 * 2, 0, false },
		{ "indices [0 1 2] with baseVertex 1",            false, 3, 0,     1, false },
		{ "indices [0 1 2] as UInt32",                    true,  3, 0,     0, false },
		{ "indices [1 2 3] as UInt32",                    true,  3, 4 * 4, 0, false },
		{ "UInt16 at an odd offset, which must be refused",   false, 3, 1, 0, true },
		{ "8 indices of a buffer that holds 7, refused too",  false, 8, 0, 0, true },
	};
	enum { STEPS = sizeof(step) / sizeof(step[0]) };
	unsigned grid[STEPS + 1][16][4];
	uint32_t wide[7];
	unsigned drew = 0, refused = 0;

	memset(grid, 0, sizeof(grid));
	[targets[2] getBytes:grid[0]
		 bytesPerRow:4 * 16
		  fromRegion:MTLRegionMake2D(0, 0, 4, 4)
		 mipmapLevel:0];
	for (unsigned i = 0; i < 7; i++) {
		wide[i] = index_list[i];
	}
	id<MTLBuffer> ib16 = [dev newBufferWithBytes:index_list length:sizeof(index_list)
					     options:0];
	id<MTLBuffer> ib32 = [dev newBufferWithBytes:wide length:sizeof(wide) options:0];

	if (ib16 == nil || ib32 == nil) {
		printf("drvtest: index: no index buffer: %s\n",
		       ib16 == nil ? "newBufferWithBytes: gave nil" : "the UInt32 one is nil");
		return;
	}
	printf("drvtest: index: the index buffer is a %s of %lu bytes, holding "
	       "0 1 2 3 1 2 3\n", class_getName(object_getClass(ib16)),
	       (unsigned long)[ib16 length]);
	printf("drvtest: index: step 0 is the non-indexed strip of 4 already drawn above\n");
	index_print(0, "the non-indexed strip of 4", grid[0]);

	for (unsigned s = 0; s < STEPS; s++) {
		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];

		if (enc == nil) {
			printf("drvtest: index: %s: no encoder\n", step[s].what);
			return;
		}
		[enc setRenderPipelineState:pipeline];
		for (int i = 0; i < 16; i++) {
			[enc setFragmentBuffer:uniform offset:0 atIndex:i];
			[enc setVertexBuffer:i == 1 ? coords : corners offset:0 atIndex:i];
			if (i < 4 && dtex[i] != nil) {
				[enc setFragmentTexture:dtex[i] atIndex:i];
			}
			if (dsamp != nil) {
				[enc setFragmentSamplerState:dsamp atIndex:i];
			}
		}
		/* The longest form for every step, because the shorter two ARE this one with
		 * instanceCount 1, baseVertex 0 and baseInstance 0 -- Metal's own header says
		 * as much -- and one call site is one thing to read. */
		[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
				indexCount:step[s].count
				 indexType:step[s].wide ? MTLIndexTypeUInt32
							: MTLIndexTypeUInt16
			       indexBuffer:step[s].wide ? ib32 : ib16
			 indexBufferOffset:step[s].offset
			     instanceCount:1
				baseVertex:step[s].baseVertex
			      baseInstance:0];
		[enc endEncoding];
		[cmd commit];
		[cmd waitUntilCompleted];
		if ([cmd status] != RW_CB_COMPLETED) {
			printf("drvtest: index: %u. %s: status %lu%s%s\n", s + 1, step[s].what,
			       (unsigned long)[cmd status], [cmd error] != nil ? " -- " : "",
			       [cmd error] != nil
				   ? [[[cmd error] localizedDescription] UTF8String] : "");
			refused += step[s].must_fail;
			if (!step[s].must_fail) {
				printf("drvtest: index:    and that step was meant to draw\n");
			}
			continue;
		}
		if (step[s].must_fail) {
			printf("drvtest: index: %u. %s: DREW, and it should not have\n", s + 1,
			       step[s].what);
			continue;
		}
		drew++;
		[targets[2] getBytes:grid[s + 1]
			 bytesPerRow:4 * 16
			  fromRegion:MTLRegionMake2D(0, 0, 4, 4)
			 mipmapLevel:0];
		index_print(s + 1, step[s].what, grid[s + 1]);
	}

	/*
	 * And the comparisons, which are the measurement. Each names what the other answer
	 * would have meant, so a wrong run does not need this file to be read to be
	 * understood.
	 */
	printf("drvtest: index: the indexed strip of 4 differs from the non-indexed one in "
	       "%u of 64 words -- the CONTROL, which a draw ignoring the index buffer passes "
	       "as well\n", index_diff(grid[0], grid[1]));
	printf("drvtest: index: the two halves differ in %u of 64 words, (0,0) is %s in the "
	       "first and %s in the second, (3,3) is %s and %s: %s\n",
	       index_diff(grid[2], grid[3]),
	       index_at(grid[2], 0, 0) ? "drawn" : "clear",
	       index_at(grid[3], 0, 0) ? "drawn" : "clear",
	       index_at(grid[2], 3, 3) ? "drawn" : "clear",
	       index_at(grid[3], 3, 3) ? "drawn" : "clear",
	       (index_at(grid[2], 0, 0) && !index_at(grid[2], 3, 3) &&
		index_at(grid[3], 3, 3) && !index_at(grid[3], 0, 0))
		   ? "THE INDEX BUFFER AND THE OFFSET ARE READ ON THE ADRENO"
		   : "a right angle came back on the wrong side, which is what an ignored "
		     "index buffer or offset gives: the first triangle twice");
	printf("drvtest: index: baseVertex 1 over [0 1 2] differs from [1 2 3] in %u of 64 "
	       "words and from [0 1 2] in %u: %s\n", index_diff(grid[4], grid[3]),
	       index_diff(grid[4], grid[2]),
	       index_diff(grid[4], grid[3]) == 0
		   ? "baseVertex is added to every index"
		   : index_diff(grid[4], grid[2]) == 0 ? "baseVertex was DROPPED"
						       : "neither picture, which wants reading");
	printf("drvtest: index: the UInt32 halves differ from the UInt16 ones in %u and %u of "
	       "64 words: %s\n", index_diff(grid[5], grid[2]), index_diff(grid[6], grid[3]),
	       index_diff(grid[5], grid[2]) == 0 && index_diff(grid[6], grid[3]) == 0
		   ? "the index type is honoured"
		   : "a 32-bit index read two bytes at a time is 0, 0, 1 -- a triangle of no "
		     "area, which covers nothing");
	printf("drvtest: index: %u of 6 drew and %u of 2 were refused by name\n", drew,
	       refused);
	fflush(stdout);
}

/*
 * --draw-attrib: the vertex attribute layout ON THE HARDWARE, through Metal's own API.
 *
 * docs/metal.md 10.44 put MTLVertexDescriptor into the contract and measured it on this
 * machine's softpipe: the offsets, the stride, the formats, the buffer index, the step
 * function and its rate each changed what came back, and the element order proved to be the
 * shader's driver_location order and not Metal's attribute order. What that measurement
 * could NOT touch is the only part made of hardware: the VFD -- Adreno's vertex fetch unit
 * -- which softpipe does not have at all. So three things in it were left as honest
 * negatives, and this mode is here to settle them on the phone:
 *
 *   - the SWAP. MTLVertexFormatUChar4Normalized_BGRA becomes PIPE_FORMAT_B8G8R8A8_UNORM, and
 *     nothing about the byte order is in the fetch's format field: freedreno puts it in
 *     VFD_DECODE_INSTR.swap, which fd6_vertex_swap() reads off the pipe_format. Measured by
 *     storing the coordinate so that it is only right if the swap happens: the first byte of
 *     the element is ZERO and the coordinate lives in bytes 2 and 1, which the BGRA order
 *     delivers as x and y. Without the swap the shader's x is that zero, every pixel samples
 *     column 0 of the texture, and the x gradient of the picture disappears.
 *   - the INSTANCED bit and STEP_RATE, which are two more fields of the same register
 *     (VFD_DECODE_INSTR.instanced and VFD_DECODE[k].STEP_RATE) and have never been
 *     programmed by this driver. Measured in the picture, because no colour attachment can
 *     show an instance count: the coordinate comes out of a SECOND stream that steps per
 *     instance, so all four vertices of one instance share one coordinate and every pixel
 *     holds ONE texel; per vertex instead, the four vertices get four coordinates and the
 *     picture is a gradient of four texels.
 *   - the PLACEHOLDER element for an attribute gated by a function constant, which 10.44
 *     proved accepted and not fetched: on the host the pipeline for such a module dies
 *     inside softpipe before anything can be drawn with it (the `_lph` variants on the
 *     16-bit narrowing of a `half4` attribute, the `_lpf` ones on `store_scratch`). Here ir3 compiles it, so the question becomes
 *     the one that matters: does the GPU come back. An unconfigured attribute that got NO
 *     element at all would leave an empty VFD slot, and an empty slot fetches from address 0
 *     -- which is the shape of the fault that cost this port a boot in 10.18.
 *
 * And it carries over the thing that makes the host measurement evidence rather than a
 * picture: THE DECOY. The module's own [[attribute(1)]] is air.arg_unused -- nothing reads
 * it -- and this mode configures it anyway, pointing at bytes that hold a different texture
 * coordinate. An element array built in Metal's attribute order, or over all three
 * attributes in order, feeds that layout to attribute 3's fetch: the geometry stays
 * perfectly right and every pixel comes back (100, 200, 300, 400). That is the only wrong
 * answer here that a coverage check could not see.
 *
 * The pipeline is Apple's own code on both sides and out of two libraries, which is a legal
 * Metal pipeline and the shape a real one has: QuartzCore's std_vert0_lpf (position is
 * `mvp_matrix * attribute(0)`, so an IDENTITY matrix in its [[buffer(2)]] makes the position
 * the attribute verbatim, and its [[attribute(3)]] `color` becomes the first varying) over
 * AppleMetalGLRenderer's texturedFragment2DUInt (samples texture 0 at that varying and
 * writes the texel to four RGBA32Uint targets). So the COVERAGE measures attribute 0 and
 * every WORD measures attribute 3: pixel (x, y) must hold the texel its own interpolated
 * coordinate lands in, (100 + x, 200 + y, 300 + x + y, 400), up to the vertical orientation
 * of 10.32 -- both orientations are counted and the one that matched is printed, because
 * which attribute fed the fetch has the same answer either way.
 *
 *   metal_drv_test --draw-attrib [--vertexlib PATH] [--vertex NAME] [--gated NAME]
 *                  [--fragment NAME] FRAGMENTLIB
 */

#define ATTR_N 8          /* elements in the stream */
#define ATTR_STREAM 5     /* Metal buffer index of the interleaved stream */
#define ATTR_INSTANCE 6   /* ... of the per-instance stream */
#define ATTR_OTHER 7      /* ... and of the same stream bound somewhere else */

static const char *attrib_vertex_lib =
    "/System/Library/Frameworks/QuartzCore.framework/default.metallib";
static const char *attrib_gated_function = "fixed_vert_lpf_spc";

/*
 * The eight elements of the stream: the four corners of the full-screen quad with a texture
 * coordinate running the whole [0, 1] square, then four at half the distance from the origin
 * whose coordinate is the far corner. The second four exist for one step -- the same data
 * read at twice the stride, where the fetch walks 0, 2, 4, 6 and must produce a picture
 * nothing else here produces.
 */
static const float attrib_pos[ATTR_N][4] = {
	{ -1.0f, -1.0f, 0.0f, 1.0f }, {  1.0f, -1.0f, 0.0f, 1.0f },
	{ -1.0f,  1.0f, 0.0f, 1.0f }, {  1.0f,  1.0f, 0.0f, 1.0f },
	{ -0.5f, -0.5f, 0.0f, 1.0f }, {  0.5f, -0.5f, 0.0f, 1.0f },
	{ -0.5f,  0.5f, 0.0f, 1.0f }, {  0.5f,  0.5f, 0.0f, 1.0f },
};
static const float attrib_uv[ATTR_N][2] = {
	{ 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f }, { 1.0f, 1.0f },
	{ 1.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 1.0f },
};

/* What the DECOY points at, in every element: the centre of texel (0, 0), so an
 * implementation that fed this layout to attribute 3's fetch comes back with
 * (100, 200, 300, 400) in all sixteen pixels -- one number, named in advance, and not a
 * plausible gradient. */
static const float attrib_decoy_uv[2] = { 0.125f, 0.125f };

/* The per-instance stream: four coordinates, two in opposite corners of the texture and two
 * in the other two. Instance 1's is the far corner, which is the texel the instanced step
 * must leave in every pixel; a stream that stepped per vertex would hand these four to the
 * four vertices of ONE instance and the picture would be a gradient instead. */
static const float attrib_instance_uv[4][2] = {
	{ 0.125f, 0.125f }, { 0.875f, 0.875f }, { 0.125f, 0.875f }, { 0.875f, 0.125f },
};

struct attrib_slot {
	NSUInteger format;   /* MTLVertexFormat */
	NSUInteger offset;
	NSUInteger buffer;
};

struct attrib_layout {
	const char *what;
	NSUInteger stride;
	struct attrib_slot pos, uv, decoy;
};

/* Metal's Short4Normalized is value / 32767, so 1.0 is 32767 exactly and the quad's corners
 * are exact; the half-size ones are not, and nothing here compares them. */
static int16_t
attrib_snorm16(float v)
{
	float s = v * 32767.0f;

	return (int16_t)(s < 0.0f ? s - 0.5f : s + 0.5f);
}

/* UChar4Normalized: 255 is 1.0 exactly, so a coordinate of 0 or 1 survives the encoding. */
static unsigned char
attrib_unorm8(float v)
{
	return (unsigned char)(v * 255.0f + 0.5f);
}

/*
 * The stream, written in whatever encoding the layout names. Always ATTR_N elements of the
 * layout's own stride, so a step that reads it at twice the stride has something defined to
 * read instead of running off the end (and being refused for it, which is a different
 * measurement and has its own step).
 *
 * The BGRA case is the swap measurement and is the only place here where the bytes are NOT
 * in the order the components are: byte 0 is zero, byte 1 is v and byte 2 is u, because
 * MTLVertexFormatUChar4Normalized_BGRA delivers (x, y, z, w) = (byte2, byte1, byte0, byte3).
 * If the swap is not programmed the shader's x is byte 0 -- the zero -- and the picture
 * loses its x gradient entirely.
 */
static void
attrib_fill(unsigned char *mem, size_t size, const struct attrib_layout *l)
{
	memset(mem, 0, size);
	for (unsigned i = 0; i < ATTR_N; i++) {
		unsigned char *e = mem + (size_t)i * l->stride;

		if (l->pos.format == MTLVertexFormatFloat4) {
			float v[4] = { attrib_pos[i][0], attrib_pos[i][1], attrib_pos[i][2],
				       attrib_pos[i][3] };

			memcpy(e + l->pos.offset, v, sizeof(v));
		} else {
			int16_t v[4] = { attrib_snorm16(attrib_pos[i][0]),
					 attrib_snorm16(attrib_pos[i][1]),
					 attrib_snorm16(attrib_pos[i][2]),
					 attrib_snorm16(attrib_pos[i][3]) };

			memcpy(e + l->pos.offset, v, sizeof(v));
		}
		if (l->uv.format == MTLVertexFormatFloat4) {
			float v[4] = { attrib_uv[i][0], attrib_uv[i][1], 0.0f, 1.0f };

			memcpy(e + l->uv.offset, v, sizeof(v));
		} else if (l->uv.format == MTLVertexFormatUChar4Normalized_BGRA) {
			unsigned char v[4] = { 0, attrib_unorm8(attrib_uv[i][1]),
					       attrib_unorm8(attrib_uv[i][0]), 255 };

			memcpy(e + l->uv.offset, v, sizeof(v));
		} else {
			unsigned char v[4] = { attrib_unorm8(attrib_uv[i][0]),
					       attrib_unorm8(attrib_uv[i][1]), 0, 255 };

			memcpy(e + l->uv.offset, v, sizeof(v));
		}
		if (l->decoy.format == MTLVertexFormatFloat2) {
			float v[2] = { attrib_decoy_uv[0], attrib_decoy_uv[1] };

			memcpy(e + l->decoy.offset, v, sizeof(v));
		} else {
			/* 0.125 as a half: 2^-3 is exponent field 15 - 3 = 12 with a zero
			 * mantissa, which is 0x3000. Written out because this file has no
			 * float-to-half of its own and the value is a constant. */
			uint16_t v[2] = { 0x3000, 0x3000 };

			memcpy(e + l->decoy.offset, v, sizeof(v));
		}
	}
}

/*
 * The layout as Metal's own object. Both of its arrays are filled at Metal's own indices,
 * and attribute 1 -- the one the module marks air.arg_unused -- is configured on purpose:
 * that is the decoy, and a descriptor configuring an attribute no shader reads is legal and
 * ignored by Metal.
 */
static MTLVertexDescriptor *
attrib_descriptor(const struct attrib_layout *l, NSUInteger step_function,
		  NSUInteger step_rate, NSInteger instance_buffer)
{
	MTLVertexDescriptor *vd = [[MTLVertexDescriptor alloc] init];
	const struct attrib_slot *slot[3] = { &l->pos, &l->decoy, &l->uv };
	const unsigned at[3] = { 0, 1, 3 };

	for (unsigned i = 0; i < 3; i++) {
		MTLVertexAttributeDescriptor *a =
		    [[vd attributes] objectAtIndexedSubscript:at[i]];

		[a setFormat:(MTLVertexFormat)slot[i]->format];
		[a setOffset:slot[i]->offset];
		[a setBufferIndex:slot[i]->buffer];
	}
	MTLVertexBufferLayoutDescriptor *bl =
	    [[vd layouts] objectAtIndexedSubscript:l->pos.buffer];

	[bl setStride:l->stride];
	[bl setStepFunction:MTLVertexStepFunctionPerVertex];
	[bl setStepRate:1];
	if (instance_buffer >= 0) {
		MTLVertexBufferLayoutDescriptor *il =
		    [[vd layouts] objectAtIndexedSubscript:(NSUInteger)instance_buffer];

		[il setStride:8];        /* one float2 coordinate per element */
		[il setStepFunction:(MTLVertexStepFunction)step_function];
		[il setStepRate:step_rate];
	}
	return vd;
}

/* One pipeline over four RGBA32Uint attachments, which is what texturedFragment2DUInt
 * writes. The reason for a sentence rather than a nil is the whole of 10.44: every refusal
 * in this path is the compiler library's own and names what was asked for. */
static id<MTLRenderPipelineState>
attrib_pipeline(id<MTLDevice> dev, id<MTLFunction> vertex, id<MTLFunction> fragment,
		MTLVertexDescriptor *vd, const char *what, bool quiet)
{
	MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
	NSError *err = nil;

	[pd setVertexFunction:vertex];
	[pd setFragmentFunction:fragment];
	[pd setVertexDescriptor:vd];
	for (unsigned i = 0; i < 4; i++) {
		[[[pd colorAttachments] objectAtIndexedSubscript:i]
		    setPixelFormat:MTLPixelFormatRGBA32Uint];
	}
	id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd
									    error:&err];

	[pd release];
	if (ps == nil && !quiet) {
		printf("drvtest: attrib: %s got no pipeline: %s\n", what,
		       err != nil ? [[err localizedDescription] UTF8String] : "(no reason)");
	}
	if (ps == nil && quiet) {
		printf("drvtest: attrib:   %s: refused -- %s\n", what,
		       err != nil ? [[err localizedDescription] UTF8String] : "(no reason)");
	}
	return ps;
}

/* Everything a draw of this mode binds, so that eight draws differ in the layout and in
 * nothing else. The vertex ARGUMENTS (the matrices) and the vertex STREAMS come off one
 * table, as Metal has one; the pipeline's own mask is what splits them in the driver. */
struct attrib_bind {
	id<MTLCommandQueue> queue;
	MTLRenderPassDescriptor *rp;
	id<MTLTexture> target;                /* the attachment read back */
	id<MTLBuffer> argument[8];            /* by Metal buffer index: the matrices */
	id<MTLBuffer> stream[8];              /* by Metal buffer index */
	NSUInteger stream_offset[8];
	id<MTLTexture> tex;
	id<MTLSamplerState> samp;
};

/*
 * One draw and the picture it left, into attachments cleared to zero by the pass. Returns
 * false and says where it stopped: this driver RECORDS the draw and performs it on -commit
 * (10.13), so "no encoder", "status" and "the GPU did not finish" are three different
 * answers and the log has to tell them apart.
 */
static bool
attrib_draw(const struct attrib_bind *b, id<MTLRenderPipelineState> ps,
	    NSUInteger instances, id<MTLBuffer> indices, NSUInteger index_count,
	    NSUInteger index_offset, unsigned grid[16][4], const char *what)
{
	id<MTLCommandBuffer> cmd = [b->queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:b->rp];

	if (enc == nil) {
		printf("drvtest: attrib: %s got no encoder\n", what);
		return false;
	}
	[enc setRenderPipelineState:ps];
	for (unsigned i = 0; i < 8; i++) {
		if (b->argument[i] != nil) {
			[enc setVertexBuffer:b->argument[i] offset:0 atIndex:i];
		}
		if (b->stream[i] != nil) {
			[enc setVertexBuffer:b->stream[i] offset:b->stream_offset[i] atIndex:i];
		}
	}
	[enc setFragmentTexture:b->tex atIndex:0];
	[enc setFragmentSamplerState:b->samp atIndex:0];
	if (indices != nil) {
		[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
				indexCount:index_count
				 indexType:MTLIndexTypeUInt16
			       indexBuffer:indices
			 indexBufferOffset:index_offset
			     instanceCount:instances
				baseVertex:0
			      baseInstance:0];
	} else {
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip
			vertexStart:0
			vertexCount:4
		      instanceCount:instances];
	}
	[enc endEncoding];
	[cmd commit];
	[cmd waitUntilCompleted];
	if ([cmd status] != RW_CB_COMPLETED) {
		printf("drvtest: attrib: %s: status %lu%s%s\n", what, (unsigned long)[cmd status],
		       [cmd error] != nil ? " -- " : "",
		       [cmd error] != nil ? [[[cmd error] localizedDescription] UTF8String] : "");
		return false;
	}
	memset(grid, 0, sizeof(unsigned[16][4]));
	[b->target getBytes:grid
		bytesPerRow:4 * 16
		 fromRegion:MTLRegionMake2D(0, 0, 4, 4)
		mipmapLevel:0];
	return true;
}

/*
 * What the picture says, counted rather than eyeballed. 10.32 established that the texture
 * comes back mirrored vertically against a naive reading of the coordinate, so BOTH
 * orientations are counted and the one that matched is printed: what this mode is about is
 * WHICH attribute fed the fetch, and that question has the same answer either way.
 */
struct attrib_verdict {
	unsigned x_gradient;    /* pixels whose first word is 100 + x */
	unsigned mirrored;      /* ... and whose second is 200 + (3 - y) */
	unsigned upright;       /* ... or 200 + y */
	unsigned decoy_texel;   /* pixels holding texel (0, 0): the decoy's own answer */
	unsigned column0;       /* pixels holding texel (0, y): what a dropped BGRA swap gives */
	unsigned uniform;       /* pixels equal to the pixel at (0, 0) */
	unsigned covered;
};

static void
attrib_judge(const unsigned grid[16][4], struct attrib_verdict *v)
{
	memset(v, 0, sizeof(*v));
	for (unsigned y = 0; y < 4; y++) {
		for (unsigned x = 0; x < 4; x++) {
			const unsigned *w = grid[y * 4 + x];

			v->covered += w[0] != 0;
			v->x_gradient += w[0] == 100 + x;
			v->mirrored += w[0] == 100 + x && w[1] == 200 + (3 - y);
			v->upright += w[0] == 100 + x && w[1] == 200 + y;
			v->decoy_texel += w[0] == 100 && w[1] == 200 && w[2] == 300 &&
					  w[3] == 400;
			v->column0 += w[0] == 100;
			v->uniform += w[0] == grid[0][0] && w[1] == grid[0][1] &&
				      w[2] == grid[0][2] && w[3] == grid[0][3];
		}
	}
}

static void
attrib_print(const char *what, const unsigned grid[16][4], const struct attrib_verdict *v)
{
	printf("drvtest: attrib: %s\n", what);
	printf("drvtest: attrib:   coverage ");
	for (unsigned y = 0; y < 4; y++) {
		for (unsigned x = 0; x < 4; x++) {
			printf("%c", index_at(grid, x, y) ? '#' : '.');
		}
		printf("%s", y < 3 ? "/" : "");
	}
	printf("  %u of 16; (0,0) = %u %u %u %u, (3,3) = %u %u %u %u\n", v->covered,
	       grid[0][0], grid[0][1], grid[0][2], grid[0][3],
	       grid[15][0], grid[15][1], grid[15][2], grid[15][3]);
	printf("drvtest: attrib:   %u of 16 hold texel x, %u hold (x, 3-y), %u hold (x, y), "
	       "%u hold texel (0,0), %u hold column 0, %u are the same as (0,0)\n",
	       v->x_gradient, v->mirrored, v->upright, v->decoy_texel, v->column0, v->uniform);
}

/* Whether a picture is the texel grid the layout describes: every pixel its own texel, in
 * one of the two vertical orientations, over the whole attachment. */
static bool
attrib_is_grid(const struct attrib_verdict *v)
{
	return v->covered == 16 && v->x_gradient == 16 && (v->mirrored == 16 || v->upright == 16);
}

static void
attrib_path(id<MTLDevice> dev, const char *fragment_lib)
{
	NSError *err = nil;
	/* Two libraries, two MTLLibrary objects, one pipeline -- which Metal allows and a real
	 * pipeline does: the attributes are QuartzCore's and the shader that turns their
	 * varying into a readable texel is the GL renderer's. */
	NSData *vdata = [NSData dataWithContentsOfFile:
	    [NSString stringWithUTF8String:attrib_vertex_lib]];
	NSData *fdata = [NSData dataWithContentsOfFile:
	    [NSString stringWithUTF8String:fragment_lib]];

	if (vdata == nil || fdata == nil) {
		printf("drvtest: attrib: cannot read %s\n", vdata == nil ? attrib_vertex_lib
								         : fragment_lib);
		return;
	}
	dispatch_data_t vdd = dispatch_data_create([vdata bytes], [vdata length], NULL,
						   DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	dispatch_data_t fdd = dispatch_data_create([fdata bytes], [fdata length], NULL,
						   DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	id<MTLLibrary> vlib = [dev newLibraryWithData:vdd error:&err];
	id<MTLLibrary> flib = [dev newLibraryWithData:fdd error:&err];

	if (vlib == nil || flib == nil) {
		printf("drvtest: attrib: newLibraryWithData: gave nil for %s\n",
		       vlib == nil ? attrib_vertex_lib : fragment_lib);
		return;
	}
	/* The vertex function's default is the module 10.44 measured on this machine, and the
	 * fragment function's is the only shader in reach that turns a varying into a word a
	 * pixel can be read as. --vertex and --fragment override either; the fragment default
	 * is spelled out rather than taken from `fragment_function`, whose own default
	 * (clearFragment20) writes a constant and would make every picture here identical. */
	const char *vname = vertex_function != NULL ? vertex_function : "std_vert0_lpf";
	const char *fname = (fragment_function != NULL &&
			     strcmp(fragment_function, "clearFragment20") != 0)
	    ? fragment_function : "texturedFragment2DUInt";
	id<MTLFunction> vertex = [vlib newFunctionWithName:
	    [NSString stringWithUTF8String:vname]];
	id<MTLFunction> fragment = [flib newFunctionWithName:
	    [NSString stringWithUTF8String:fname]];

	if (vertex == nil || fragment == nil) {
		printf("drvtest: attrib: no %s in %s\n", vertex == nil ? vname : fname,
		       vertex == nil ? attrib_vertex_lib : fragment_lib);
		return;
	}
	printf("drvtest: attrib: %s (%s) over %s (%s)\n", vname, attrib_vertex_lib, fname,
	       fragment_lib);

	/* Four attachments, because that shader writes four, and 4x4 as everywhere else in
	 * this program: a pixel here, a pixel out of air_metal_test and a pixel out of air_run
	 * are the same measurement. */
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint
							      width:4
							     height:4
							  mipmapped:NO];

	[td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];

	id<MTLTexture> targets[4] = { nil, nil, nil, nil };
	MTLRenderPassDescriptor *rp = [[MTLRenderPassDescriptor renderPassDescriptor] retain];

	for (unsigned i = 0; i < 4; i++) {
		targets[i] = [dev newTextureWithDescriptor:td];
		if (targets[i] == nil) {
			printf("drvtest: attrib: attachment %u could not be made\n", i);
			return;
		}
		MTLRenderPassColorAttachmentDescriptor *a =
		    [[rp colorAttachments] objectAtIndexedSubscript:i];

		[a setTexture:targets[i]];
		[a setLoadAction:MTLLoadActionClear];
		[a setClearColor:MTLClearColorMake(0, 0, 0, 0)];
		[a setStoreAction:MTLStoreActionStore];
	}

	/* The sampled texture, air_run's texels, and the sampler Metal gives by default:
	 * nearest, clamp to edge, normalised coordinates -- so the texel the coordinate falls
	 * in is the texel that comes back, with no weights to round. */
	MTLTextureDescriptor *std_ =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint
							      width:4
							     height:4
							  mipmapped:NO];

	[std_ setUsage:MTLTextureUsageShaderRead];
	id<MTLTexture> tex = [dev newTextureWithDescriptor:std_];

	if (tex == nil) {
		printf("drvtest: attrib: the sampled texture could not be made\n");
		return;
	}
	/*
	 * Seeded in ONE call and read back before anything is drawn, which is draw_path's own
	 * rule and for its reason: per row it would be four calls with a non-zero region
	 * origin, and then every picture below would depend on the driver getting origin.y
	 * right -- a thing this mode is not about. And a draw that samples a texture nobody
	 * has looked at cannot tell a wrong sample from a wrong seed.
	 */
	{
		unsigned char all[4 * 4 * 16], back[4 * 4 * 16];
		unsigned stride_bytes = texel_bytes_of(MTLPixelFormatRGBA32Uint);

		for (unsigned y = 0; y < 4; y++) {
			for (unsigned x = 0; x < 4; x++) {
				float v[4] = { 100.0f + x, 200.0f + y, 300.0f + x + y, 400.0f };

				write_texel(all + (y * 4 + x) * stride_bytes,
					    MTLPixelFormatRGBA32Uint, v);
			}
		}
		[tex replaceRegion:MTLRegionMake2D(0, 0, 4, 4)
		       mipmapLevel:0
			 withBytes:all
		       bytesPerRow:4 * stride_bytes];
		memset(back, 0, sizeof(back));
		[tex getBytes:back
		  bytesPerRow:4 * stride_bytes
		   fromRegion:MTLRegionMake2D(0, 0, 4, 4)
		  mipmapLevel:0];
		for (unsigned y = 0; y < 4; y++) {
			printf("drvtest: attrib: seeded row %u reads back", y);
			for (unsigned x = 0; x < 4; x++) {
				float v[4] = { 0, 0, 0, 0 };

				read_texel(back + (y * 4 + x) * stride_bytes,
					   MTLPixelFormatRGBA32Uint, v);
				printf(" %g/%g", (double)v[0], (double)v[1]);
			}
			printf("   (x/y of each texel; row %u must read y = %u)\n", y, 200 + y);
		}
	}
	MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
	id<MTLSamplerState> samp = [dev newSamplerStateWithDescriptor:sd];

	[sd release];

	/*
	 * The vertex stage's own buffer arguments: mvp_matrix at [[buffer(2)]] gets the
	 * IDENTITY, which is what makes the position the attribute verbatim and the picture
	 * computable; texmat at [[buffer(3)]] is air.arg_unused in this module and is bound
	 * because the module declares it; [[buffer(4)]] is bound for the gated module, which
	 * declares a chroma_matrix there. The identity is the identity in either column order,
	 * which is the whole reason this harness can fill a matrix it did not define.
	 */
	struct attrib_bind b;

	memset(&b, 0, sizeof(b));
	b.queue = [dev newCommandQueue];
	b.rp = rp;
	b.target = targets[3];
	b.tex = tex;
	b.samp = samp;
	for (unsigned i = 2; i <= 4; i++) {
		b.argument[i] = [dev newBufferWithLength:64 options:0];
		if (b.argument[i] == nil || [b.argument[i] contents] == NULL) {
			printf("drvtest: attrib: buffer %u could not be made\n", i);
			return;
		}
		float *f = [b.argument[i] contents];

		memset(f, 0, 64);
		for (unsigned k = 0; k < 4; k++) {
			f[k * 4 + k] = 1.0f;
		}
	}

	/* The two shapes of the stream, and everything about them differs except the numbers
	 * they encode: the stride, all three offsets and all three formats. */
	static const struct attrib_layout wide = {
		"1. Float4 at offset 16 of a 48-byte element, stream at buffer index 5",
		48,
		{ MTLVertexFormatFloat4, 16, ATTR_STREAM },
		{ MTLVertexFormatFloat4, 0, ATTR_STREAM },
		{ MTLVertexFormatFloat2, 32, ATTR_STREAM },
	};
	static const struct attrib_layout narrow = {
		"2. Short4Normalized at offset 4 of a 16-byte element, the same four vertices",
		16,
		{ MTLVertexFormatShort4Normalized, 4, ATTR_STREAM },
		{ MTLVertexFormatUChar4Normalized, 0, ATTR_STREAM },
		{ MTLVertexFormatHalf2, 12, ATTR_STREAM },
	};

	id<MTLBuffer> wide_stream = [dev newBufferWithLength:ATTR_N * wide.stride options:0];
	id<MTLBuffer> narrow_stream = [dev newBufferWithLength:ATTR_N * narrow.stride options:0];
	id<MTLBuffer> bgra_stream = [dev newBufferWithLength:ATTR_N * narrow.stride options:0];
	id<MTLBuffer> instance_stream =
	    [dev newBufferWithLength:sizeof(attrib_instance_uv) options:0];
	id<MTLBuffer> indices = [dev newBufferWithLength:sizeof(index_list) options:0];

	if (wide_stream == nil || narrow_stream == nil || bgra_stream == nil ||
	    instance_stream == nil || indices == nil) {
		printf("drvtest: attrib: a vertex stream buffer could not be made\n");
		return;
	}
	struct attrib_layout bgra = narrow;

	bgra.what = "3. the coordinate as UChar4Normalized_BGRA: the SWAP";
	bgra.uv.format = MTLVertexFormatUChar4Normalized_BGRA;
	attrib_fill([wide_stream contents], [wide_stream length], &wide);
	attrib_fill([narrow_stream contents], [narrow_stream length], &narrow);
	attrib_fill([bgra_stream contents], [bgra_stream length], &bgra);
	memcpy([instance_stream contents], attrib_instance_uv, sizeof(attrib_instance_uv));
	memcpy([indices contents], index_list, sizeof(index_list));
	printf("drvtest: attrib: the stream is 8 elements: four corners of the quad with the "
	       "coordinate running the whole [0,1] square, then four at half the distance with "
	       "the coordinate (1,1). The module's UNUSED attribute 1 is configured too and "
	       "points at (%g, %g), the centre of texel (0,0)\n", (double)attrib_decoy_uv[0],
	       (double)attrib_decoy_uv[1]);

	/* 1. The wide layout, which is the reference every step below is compared with. */
	MTLVertexDescriptor *vd = attrib_descriptor(&wide, MTLVertexStepFunctionPerVertex, 1, -1);
	id<MTLRenderPipelineState> wide_ps = attrib_pipeline(dev, vertex, fragment, vd,
							     wide.what, false);

	[vd release];
	if (wide_ps == nil) {
		return;
	}
	/* What the compiler library built, read off the driver's own pipeline object rather
	 * than assumed: the elements are in the shader's driver_location order, so for
	 * std_vert0_lpf (attributes 0, 1 unused, 3) there must be TWO of them and they must be
	 * attribute 0 and attribute 3. A module of another shape is said so rather than
	 * measured against the wrong expectation. */
	unsigned elements = 0, mask = 0, slots = 0;
	SEL sel_count = sel_getUid("vertexElementCount");
	SEL sel_attr = sel_getUid("vertexElementAttribute:");
	SEL sel_mask = sel_getUid("vertexStreamMask");
	SEL sel_slots = sel_getUid("vertexStreamCount");

	if ([wide_ps respondsToSelector:sel_count] && [wide_ps respondsToSelector:sel_attr]) {
		elements = ((unsigned (*)(id, SEL))objc_msgSend)(wide_ps, sel_count);
		mask = ((unsigned (*)(id, SEL))objc_msgSend)(wide_ps, sel_mask);
		slots = ((unsigned (*)(id, SEL))objc_msgSend)(wide_ps, sel_slots);
		printf("drvtest: attrib: the pipeline holds %u vertex element%s, %u stream "
		       "slot%s, mask 0x%x; elements are", elements, elements == 1 ? "" : "s",
		       slots, slots == 1 ? "" : "s", mask);
		for (unsigned i = 0; i < elements; i++) {
			printf(" [%u]=attribute(%u)", i,
			       ((unsigned (*)(id, SEL, unsigned))objc_msgSend)(wide_ps, sel_attr,
									       i));
		}
		printf("\n");
	} else {
		printf("drvtest: attrib: this pipeline does not report its elements, so the "
		       "element ORDER is measured only by the decoy below\n");
	}

	unsigned g_wide[16][4], g_narrow[16][4], g_bgra[16][4], g_stride[16][4];
	unsigned g_other[16][4], g_idx[16][4], g_half[16][4], g_inst[16][4], g_rate[16][4];
	struct attrib_verdict v;
	bool ok = true;

	b.stream[ATTR_STREAM] = wide_stream;
	if (!attrib_draw(&b, wide_ps, 1, nil, 0, 0, g_wide, wide.what)) {
		return;
	}
	attrib_judge(g_wide, &v);
	attrib_print(wide.what, g_wide, &v);
	if (!attrib_is_grid(&v)) {
		printf("drvtest: attrib:   NOT the picture the layout describes%s\n",
		       v.decoy_texel == 16
			   ? ": every pixel holds texel (0,0), which is where the module's "
			     "UNUSED attribute 1 points -- the elements were not built in "
			     "driver_location order"
			   : "");
		ok = false;
	} else {
		printf("drvtest: attrib:   the coordinate of [[attribute(3)]] reached every "
		       "pixel and the quad of [[attribute(0)]] covered all 16, ON THE ADRENO: "
		       "the offsets, the stride and buffer index 5 are READ, and element 1 is "
		       "attribute 3 and not the unused attribute 1 (%s orientation, 10.32)\n",
		       v.mirrored == 16 ? "mirrored" : "upright");
	}

	/* 2. The same numbers in other formats at another stride and other offsets: equal to
	 *    the bit, or the formats are not what decoded the bytes. 0 and 255 read as a float
	 *    are 0.0 and 3.57e-43, and a Short4Normalized position read as Float4 is four
	 *    denormals -- a quad collapsed into the origin, covering nothing. */
	vd = attrib_descriptor(&narrow, MTLVertexStepFunctionPerVertex, 1, -1);
	id<MTLRenderPipelineState> narrow_ps = attrib_pipeline(dev, vertex, fragment, vd,
							       narrow.what, false);
	[vd release];
	if (narrow_ps == nil) {
		return;
	}
	b.stream[ATTR_STREAM] = narrow_stream;
	if (!attrib_draw(&b, narrow_ps, 1, nil, 0, 0, g_narrow, narrow.what)) {
		return;
	}
	attrib_judge(g_narrow, &v);
	attrib_print(narrow.what, g_narrow, &v);
	{
		unsigned d = index_diff(g_wide, g_narrow);

		printf("drvtest: attrib:   %u of 64 words differ from step 1: %s\n", d,
		       d == 0 ? "two encodings, two strides, three different offsets, ONE "
				"PICTURE -- the VFD decoded the bytes"
			      : "the same numbers in another format came back as another picture");
		ok = ok && d == 0;
	}

	/* 3. THE SWAP, which is the first of the three things softpipe could not answer. */
	vd = attrib_descriptor(&bgra, MTLVertexStepFunctionPerVertex, 1, -1);
	id<MTLRenderPipelineState> bgra_ps = attrib_pipeline(dev, vertex, fragment, vd,
							     bgra.what, false);
	[vd release];
	if (bgra_ps != nil) {
		b.stream[ATTR_STREAM] = bgra_stream;
		if (attrib_draw(&b, bgra_ps, 1, nil, 0, 0, g_bgra, bgra.what)) {
			unsigned d = index_diff(g_narrow, g_bgra);

			attrib_judge(g_bgra, &v);
			attrib_print(bgra.what, g_bgra, &v);
			printf("drvtest: attrib:   %u of 64 words differ from step 2, and the "
			       "coordinate's own byte 0 is ZERO: %s\n", d,
			       d == 0 ? "the BGRA order IS programmed -- VFD_DECODE_INSTR.swap "
					"delivers bytes 2 and 1 as x and y"
				      : v.column0 == 16
					    ? "every pixel holds column 0, which is what reading "
					      "the bytes in memory order gives: the swap was NOT "
					      "programmed"
					    : "neither picture, which is a third answer and wants "
					      "reading");
			ok = ok && d == 0;
		} else {
			ok = false;
		}
		[bgra_ps release];
	} else {
		ok = false;
	}

	/* 4. The stride, with a picture nothing else here produces: the narrow layout read at
	 *    32 bytes instead of 16 over a buffer of eight elements, so the fetch walks
	 *    elements 0, 2, 4 and 6 -- two corners of the full quad and two of the half-size
	 *    one. */
	struct attrib_layout stride2 = narrow;

	stride2.what = "4. the narrow layout at twice its stride: the fetch walks 0, 2, 4, 6";
	stride2.stride = 32;
	vd = attrib_descriptor(&stride2, MTLVertexStepFunctionPerVertex, 1, -1);
	id<MTLRenderPipelineState> stride_ps = attrib_pipeline(dev, vertex, fragment, vd,
							       stride2.what, false);
	[vd release];
	if (stride_ps != nil) {
		b.stream[ATTR_STREAM] = narrow_stream;
		if (attrib_draw(&b, stride_ps, 1, nil, 0, 0, g_stride, stride2.what)) {
			unsigned d = index_diff(g_narrow, g_stride);

			attrib_judge(g_stride, &v);
			attrib_print(stride2.what, g_stride, &v);
			printf("drvtest: attrib:   %u of 64 words differ from step 2: %s\n", d,
			       d ? "the stride decides which element a vertex reads"
				 : "A STRIDE OF 32 DREW WHAT A STRIDE OF 16 DREW, so the stride "
				   "is not read");
			ok = ok && d != 0;
		} else {
			ok = false;
		}
		[stride_ps release];
	} else {
		ok = false;
	}

	/* 5. The buffer index from the other side: the same layout fetched from index 7, with
	 *    the stream bound at 7 and nowhere else. A driver that bound every stream at slot 0
	 *    would fetch from an empty slot here and draw nothing. */
	struct attrib_layout other = narrow;

	other.what = "5. the narrow layout fetched from buffer index 7";
	other.pos.buffer = other.uv.buffer = other.decoy.buffer = ATTR_OTHER;
	vd = attrib_descriptor(&other, MTLVertexStepFunctionPerVertex, 1, -1);
	id<MTLRenderPipelineState> other_ps = attrib_pipeline(dev, vertex, fragment, vd,
							      other.what, false);
	[vd release];
	if (other_ps != nil) {
		b.stream[ATTR_STREAM] = nil;
		b.stream[ATTR_OTHER] = narrow_stream;
		if (attrib_draw(&b, other_ps, 1, nil, 0, 0, g_other, other.what)) {
			unsigned d = index_diff(g_narrow, g_other);

			printf("drvtest: attrib:   %s: %u of 64 words differ from step 2, %s\n",
			       other.what, d,
			       d == 0 ? "so Metal's buffer index is carried all the way down to "
					"VFD_FETCH[k]"
				      : "so the index moved the picture, which it must not");
			ok = ok && d == 0;
		} else {
			ok = false;
		}
		[other_ps release];
		b.stream[ATTR_OTHER] = nil;
		b.stream[ATTR_STREAM] = narrow_stream;
	} else {
		ok = false;
	}

	/* 6 and 7. The indexed forms over a real layout, which is what every draw in a real
	 *          application is -- and here an index is the ELEMENT THE FETCH READS, where in
	 *          10.35 it was the [[vertex_id]] a shader indexed its own buffer with. Four
	 *          indices must give step 2's picture; the last three must give one triangle of
	 *          it. */
	if (attrib_draw(&b, narrow_ps, 1, indices, 4, 0, g_idx,
			"6. -drawIndexedPrimitives: 4 indices [0 1 2 3] over the narrow layout")) {
		unsigned d = index_diff(g_narrow, g_idx);

		printf("drvtest: attrib:   6. indices [0 1 2 3]: %u of 64 words differ from step "
		       "2, %s\n", d,
		       d == 0 ? "so the index buffer selects the element the fetch reads"
			      : "so an index fed the fetch the wrong element");
		ok = ok && d == 0;
	} else {
		ok = false;
	}
	if (attrib_draw(&b, narrow_ps, 1, indices, 3, 4 * 2, g_half,
			"7. the same buffer's last three indices [1 2 3], UInt16 at byte 8")) {
		attrib_judge(g_half, &v);
		attrib_print("7. indices [1 2 3] at byte 8", g_half, &v);
		printf("drvtest: attrib:   %u of 64 words differ from step 6, covering %u of 16: "
		       "%s\n", index_diff(g_idx, g_half), v.covered,
		       v.covered > 0 && v.covered < 16
			   ? "one triangle of the quad out of three of its four elements"
			   : "NOT one triangle, which is what an offset that was not applied or "
			     "an index that was not read would give");
		ok = ok && v.covered > 0 && v.covered < 16;
	} else {
		ok = false;
	}

	/*
	 * 8 and 9. THE INSTANCED BIT and THE STEP RATE, the second thing softpipe could not
	 *          answer about the hardware. The coordinate comes out of a second stream that
	 *          steps per instance; two instances of this geometry cover the same pixels, so
	 *          the second is drawn over the first and every pixel must hold ONE texel --
	 *          instance 1's element, (0.875, 0.875), which is texel (3, 3) = 103 203 306
	 *          400. A stream that stepped per VERTEX would hand the four coordinates to the
	 *          four vertices of one instance and the picture would be a gradient. Then the
	 *          same at a step rate of 2, where both instances share element 0 and the
	 *          picture must be texel (0, 0) = 100 200 300 400 instead -- a different number
	 *          and not a different-looking picture.
	 */
	struct attrib_layout split = narrow;

	split.what = "8. the coordinate from a second stream stepping PER INSTANCE, 2 instances";
	split.uv.format = MTLVertexFormatFloat2;
	split.uv.offset = 0;
	split.uv.buffer = ATTR_INSTANCE;
	for (unsigned step = 0; step < 2; step++) {
		NSUInteger rate = step == 0 ? 1 : 2;
		const char *what = step == 0 ? split.what
		    : "9. the same at a step rate of 2: both instances share element 0";
		unsigned (*grid)[4] = step == 0 ? g_inst : g_rate;
		unsigned want_x = step == 0 ? 3 : 0, want_y = step == 0 ? 3 : 0;

		vd = attrib_descriptor(&split, MTLVertexStepFunctionPerInstance, rate,
				       ATTR_INSTANCE);
		id<MTLRenderPipelineState> ps = attrib_pipeline(dev, vertex, fragment, vd, what,
								false);
		[vd release];
		if (ps == nil) {
			ok = false;
			continue;
		}
		b.stream[ATTR_INSTANCE] = instance_stream;
		if (attrib_draw(&b, ps, 2, nil, 0, 0, grid, what)) {
			unsigned w0 = 100 + want_x, w1 = 200 + want_y;

			attrib_judge(grid, &v);
			attrib_print(what, grid, &v);
			bool flat = v.uniform == 16 && grid[0][0] == w0 && grid[0][1] == w1;

			printf("drvtest: attrib:   expecting %u %u %u %u in all 16 pixels (texel "
			       "(%u,%u)): %s\n", w0, w1, 300 + want_x + want_y, 400, want_x,
			       want_y,
			       flat ? (step == 0 ? "the stream stepped PER INSTANCE on the "
						   "hardware"
						 : "the step rate IS the instance divisor")
				    : v.uniform != 16
					  ? "the picture is a gradient, so that stream stepped "
					    "per VERTEX"
					  : "flat, but not that texel");
			ok = ok && flat;
		} else {
			ok = false;
		}
		[ps release];
		b.stream[ATTR_INSTANCE] = nil;
	}

	/*
	 * 10. THE PLACEHOLDER for an attribute gated by a function constant, which is the third
	 *     thing this machine could not answer: on softpipe the pipeline for such a module
	 *     dies in nir_to_tgsi before any draw. QuartzCore's fixed_vert_*_spc has five
	 *     attributes -- listed 0, 4, 1, 2, 3, so attribute 4 is at driver_location 1 -- and
	 *     three of them (point_size, texcoord0, texcoord1) are gated. This configures the
	 *     two that are NOT gated and leaves the three alone, which is what a pipeline whose
	 *     constants make the predicate false legally does.
	 *
	 *     What is claimed about it is exactly two things, and no more: the pipeline BUILDS,
	 *     and the draw COMES BACK. The picture is printed and nothing is concluded from it
	 *     -- no function constants are supplied here, so what the shader reads when the
	 *     predicate is false is the pipeline's business and not this test's. The claim that
	 *     matters is the second one: an unconfigured attribute with no element at all would
	 *     leave an empty VFD slot, and an empty slot fetches from address 0 (10.18).
	 */
	if (attrib_gated_function != NULL && *attrib_gated_function != '\0') {
		id<MTLFunction> gated = [vlib newFunctionWithName:
		    [NSString stringWithUTF8String:attrib_gated_function]];

		if (gated == nil) {
			printf("drvtest: attrib: 10. no %s in %s, so the function-constant gate "
			       "was not measured\n", attrib_gated_function, attrib_vertex_lib);
		} else {
			struct attrib_layout gate = narrow;

			gate.what = "10. a module whose attributes are gated by function "
				    "constants, with only the two ungated ones configured";
			gate.decoy.format = MTLVertexFormatInvalid;   /* attribute 1 is gated */
			vd = attrib_descriptor(&gate, MTLVertexStepFunctionPerVertex, 1, -1);
			id<MTLRenderPipelineState> ps = attrib_pipeline(dev, gated, fragment, vd,
									gate.what, false);
			[vd release];
			if (ps == nil) {
				printf("drvtest: attrib:   and that refusal is the measurement "
				       "failing: a gated attribute needs no configuration, so "
				       "this pipeline had to build\n");
				ok = false;
			} else {
				unsigned g_gate[16][4];

				b.stream[ATTR_STREAM] = narrow_stream;
				if (attrib_draw(&b, ps, 1, nil, 0, 0, g_gate, gate.what)) {
					attrib_judge(g_gate, &v);
					attrib_print(gate.what, g_gate, &v);
					printf("drvtest: attrib:   the pipeline built and the GPU "
					       "CAME BACK, so the placeholder element for a gated "
					       "attribute fetches memory this library owns and not "
					       "an empty VFD slot. What the pixels hold is NOT a "
					       "claim: no function constants were supplied, so the "
					       "predicate's value -- and what the shader reads "
					       "under it -- is the pipeline's and not this test's\n");
				} else {
					printf("drvtest: attrib:   the pipeline built and the draw "
					       "did NOT come back, which is what an empty VFD slot "
					       "looks like\n");
					ok = false;
				}
				[ps release];
			}
			[gated release];
		}
	}

	/*
	 * And three layouts that must be REFUSED on the device, by name, so the sentences are
	 * read in the log of the machine they are about rather than only on the host: the
	 * twelve-bit offset field, a packed format whose channel order against Gallium's is
	 * unmeasured, and the one buffer table Metal has behind -setVertexBuffer:atIndex:.
	 */
	printf("drvtest: attrib: and the layouts that must be refused, each by name:\n");
	unsigned refused = 0;
	for (unsigned i = 0; i < 3; i++) {
		struct attrib_layout bad = narrow;
		const char *what = i == 0 ? "an attribute at offset 4096"
		    : i == 1 ? "MTLVertexFormatUInt1010102Normalized, a packed word"
			     : "a stream at the buffer index the module reads mvp_matrix from";

		bad.what = what;
		if (i == 0) {
			bad.pos.offset = 4096;
		} else if (i == 1) {
			bad.pos.format = MTLVertexFormatUInt1010102Normalized;
		} else {
			bad.pos.buffer = bad.uv.buffer = bad.decoy.buffer = 2;
		}
		vd = attrib_descriptor(&bad, MTLVertexStepFunctionPerVertex, 1, -1);
		id<MTLRenderPipelineState> ps = attrib_pipeline(dev, vertex, fragment, vd, what,
								true);
		[vd release];
		if (ps != nil) {
			printf("drvtest: attrib:   %s: BUILT, and it should not have\n", what);
			[ps release];
			ok = false;
		} else {
			refused++;
		}
	}
	printf("drvtest: attrib: %u of 3 refused by name\n", refused);
	ok = ok && refused == 3;

	printf("drvtest: attrib: %s\n",
	       ok ? "THE VERTEX ATTRIBUTE LAYOUT IS READ ON THE ADRENO -- the offsets, the "
		    "stride, the formats, the BGRA swap, the buffer index, the index buffer, the "
		    "instanced bit and its step rate each changed what came back, and the element "
		    "order is the shader's driver_location order and not Metal's attribute order"
		  : "SOMETHING ABOVE DISAGREES, and a layout that is nearly right draws a picture "
		    "of the wrong vertices");

	[narrow_ps release];
	[wide_ps release];
	[indices release];
	[instance_stream release];
	[bgra_stream release];
	[narrow_stream release];
	[wide_stream release];
	for (unsigned i = 2; i <= 4; i++) {
		[b.argument[i] release];
	}
	[samp release];
	[tex release];
	for (unsigned i = 0; i < 4; i++) {
		[targets[i] release];
	}
	[rp release];
	fflush(stdout);
}

static void
draw_path(id<MTLDevice> dev, const char *metallib)
{
	static const unsigned expected[4] = { 2458616957u, 2930221209u, 3401825461u, 3873429713u };
	NSError *err = nil;
	NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:metallib]];

	if (data == nil) {
		printf("drvtest: cannot read %s\n", metallib);
		return;
	}
	dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL,
						  DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	id<MTLLibrary> library = [dev newLibraryWithData:dd error:&err];

	if (library == nil) {
		printf("drvtest: draw: newLibraryWithData: gave nil\n");
		return;
	}
	const char *shader = fragment_function;
	id<MTLFunction> fragment = [library newFunctionWithName:
	    [NSString stringWithUTF8String:shader]];

	if (fragment == nil) {
		printf("drvtest: draw: no %s in that library\n", shader);
		return;
	}
	/* Named in the log on purpose: tools/air/metalcheck.py can then produce the host
	 * reference itself, rather than the comparison resting on the four words written into
	 * this file. */
	printf("drvtest: draw: %s %s\n", metallib, shader);

	/* What a shader declares, from the compiler library the driver is already using. The
	 * sweep below binds per shader with it; this single draw keeps its one binding, whose
	 * reference is hand-checked. */
	load_texture_query(dev);

	/* Three attachments, because the shader writes target 2 and Metal numbers them from 0;
	 * the format is the one air_run drew into, so the comparison is of the same thing
	 * (--rtfmt changes it, for a shader that writes one float). */
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:draw_target_format
							      width:4
							     height:4
							  mipmapped:NO];

	[td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];

	id<MTLTexture> targets[3] = { nil, nil, nil };

	for (int i = 0; i < 3; i++) {
		targets[i] = [dev newTextureWithDescriptor:td];
		if (targets[i] == nil) {
			printf("drvtest: draw: attachment %d could not be made\n", i);
			return;
		}
	}

	MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];

	[pd setFragmentFunction:fragment];
	id<MTLFunction> vertex = nil;

	if (vertex_function != NULL) {
		vertex = [library newFunctionWithName:
		    [NSString stringWithUTF8String:vertex_function]];
		if (vertex == nil) {
			printf("drvtest: draw: no %s in that library\n", vertex_function);
			return;
		}
		printf("drvtest: draw: vertex stage is Apple's own %s\n", vertex_function);
	}
	[pd setVertexFunction:vertex];   /* nil: the library generates the stage, as air_run does */
	for (int i = 0; i < 3; i++) {
		[[[pd colorAttachments] objectAtIndexedSubscript:i]
		    setPixelFormat:draw_target_format];
	}
	err = nil;
	id<MTLRenderPipelineState> pipeline = [dev newRenderPipelineStateWithDescriptor:pd
										  error:&err];

	[pd release];
	printf("drvtest: draw: newRenderPipelineStateWithDescriptor: gave %s%s%s\n",
	       pipeline != nil ? class_getName(object_getClass(pipeline)) : "nil",
	       err != nil ? ", error: " : "",
	       err != nil ? [[err localizedDescription] UTF8String] : "");
	if (pipeline == nil) {
		return;
	}

	/* The shader's constant buffer, filled the way air_run fills it, and bound at every
	 * index up to 16 because this one is declared at location 14 and a driver that bound
	 * only index 0 would leave it reading nothing. */
	id<MTLBuffer> uniform = [dev newBufferWithLength:256 options:0];

	if (uniform != nil && [uniform contents] != NULL) {
		unsigned char *bytes = [uniform contents];

		for (int i = 0; i < 256; i++) {
			bytes[i] = (unsigned char)((i * 7 + 13) & 0xff);
		}
	}

	/*
	 * The vertex stage's own buffers. Two of them, because that is what the shaders declare,
	 * and the first version of this bound ONE buffer of corners at every index -- so
	 * texturedVertex2D, which reads positions from buffer 0 and texture coordinates from
	 * buffer 1, read positions as its coordinates and returned an honest answer to an input
	 * nobody meant (103 200 303 400 on the device, docs/metal.md 10.25). A harness that
	 * binds the same thing everywhere cannot be compared with anything.
	 *
	 * Positions go to index 0 and to index 14, because clearVertex declares its buffer at
	 * 14 and texturedVertex2D at 0; coordinates go to 1. The corners are the geometry the
	 * generated stage covers, so the two roads draw the same pixels.
	 */
	id<MTLBuffer> corners = [dev newBufferWithLength:256 options:0];
	id<MTLBuffer> coords = [dev newBufferWithLength:256 options:0];

	if (vertex_function != NULL && corners != nil && [corners contents] != NULL) {
		static const float quad[4][4] = {
			{ -1.0f, -1.0f, 0.0f, 1.0f },
			{  1.0f, -1.0f, 0.0f, 1.0f },
			{ -1.0f,  1.0f, 0.0f, 1.0f },
			{  1.0f,  1.0f, 0.0f, 1.0f },
		};

		memcpy([corners contents], quad, sizeof(quad));
	}
	if (vertex_function != NULL && coords != nil && [coords contents] != NULL) {
		/* the same corners in texture space, so the far corner of the picture reads the
		 * far texel -- which is the half of the comparison that a dropped varying cannot
		 * fake */
		static const float uv[4][2] = {
			{ 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f }, { 1.0f, 1.0f },
		};

		memcpy([coords contents], uv, sizeof(uv));
	}

	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];

	for (int i = 0; i < 3; i++) {
		MTLRenderPassColorAttachmentDescriptor *a =
		    [[rp colorAttachments] objectAtIndexedSubscript:i];

		[a setTexture:targets[i]];
		[a setLoadAction:MTLLoadActionClear];
		[a setClearColor:MTLClearColorMake(draw_clear, draw_clear, draw_clear, draw_clear)];
		[a setStoreAction:MTLStoreActionStore];
	}
	if (draw_clear != 0) {
		printf("drvtest: draw: the attachments are cleared to %g (--clear)\n", draw_clear);
	}

	/* Textures for the fragment stage, when the shader reads any. The first attempt at
	 * drawing with Apple's textured vertex shader never reached the draw at all: it was
	 * refused with "reads texture 0 and 0 textures were bound", which is a sentence about
	 * this harness. The texels are air_run's own, so the picture can be compared. */
	id<MTLTexture> dtex[4] = { nil, nil, nil, nil };
	id<MTLSamplerState> dsamp = nil;
	/*
	 * --texslots 0=3d:FORMAT makes these four 3D textures, 4x4x4, filled the way the
	 * sweep fills a 3D slot (fill_draw_texture: every depth, b = 300 + x + y + z) -- so
	 * that one of Apple's texture3d shaders can be drawn alone, and its digest of
	 * attachment 2 is the t2 of its --draw-all line. Before this the slot's kind was read
	 * and dropped here (only its format was used), which bound a 2D texture to a
	 * texture3d and was refused for it. Any other kind keeps the 2D texture this draw
	 * always made, byte for byte, so the hand-checked 2D measurements stay what they were.
	 */
	bool volume = slot_format[0] != 0 && slot_kind[0] == MTLTextureType3D;
	MTLTextureDescriptor *dtd = volume ?
	    draw_texture_descriptor(MTLTextureType3D, slot_format[0], MTLTextureUsageShaderRead) :
	    texture_descriptor(slot_format[0] != 0 ? slot_format[0] : MTLPixelFormatRGBA32Uint,
			       MTLTextureUsageShaderRead);

	[dtd setWidth:4];
	[dtd setHeight:4];
	for (int i = 0; i < 4; i++) {
		dtex[i] = [dev newTextureWithDescriptor:dtd];
		if (dtex[i] == nil) {
			break;
		}
		unsigned stride_bytes = texel_bytes_of([dtd pixelFormat]);
		unsigned char all[4 * 4 * 16];

		for (unsigned y = 0; y < 4; y++) {
			for (unsigned x = 0; x < 4; x++) {
				float v[4] = { 100.0f + x + 1000.0f * i, 200.0f + y,
					       300.0f + x + y, 400.0f };

				write_texel(all + (y * 4 + x) * stride_bytes,
					    [dtd pixelFormat], v);
			}
		}
		/* One call for the whole picture, and not one per row.
		 *
		 * Per row it was four calls with a non-zero region origin, which made every
		 * measurement of this texture depend on the driver getting origin.y right --
		 * a thing this test is not about. It is also the second explanation of the
		 * vertical flip in docs/metal.md 10.26, which one pixel could not separate
		 * from the first: four rows landing in one place would leave every texel
		 * carrying the last row's y = 203, and pixel (0, 0) would then read
		 * 100 203 303 400 with no flip anywhere.
		 *
		 * A 3D texture is filled as the sweep fills one, a row at a time at
		 * origin (0, y, z): the same texels the --draw-all line was drawn from.
		 */
		if (volume) {
			fill_draw_texture(dtex[i], (unsigned)i);
		} else if (seed_rows) {
			for (unsigned y = 0; y < 4; y++) {
				[dtex[i] replaceRegion:MTLRegionMake2D(0, y, 4, 1)
					  mipmapLevel:0
					    withBytes:all + y * 4 * stride_bytes
					  bytesPerRow:4 * stride_bytes];
			}
		} else {
			[dtex[i] replaceRegion:MTLRegionMake2D(0, 0, 4, 4)
				  mipmapLevel:0
				    withBytes:all
				  bytesPerRow:4 * stride_bytes];
		}
		/* and read it back, because a draw that samples a texture nobody has looked
		 * at cannot tell a wrong sample from a wrong seed
		 */
		if (i == 0) {
			unsigned char back[4 * 4 * 16];

			memset(back, 0, sizeof(back));
			[dtex[i] getBytes:back
			      bytesPerRow:4 * stride_bytes
			       fromRegion:MTLRegionMake2D(0, 0, 4, 4)
			      mipmapLevel:0];
			for (unsigned y = 0; y < 4; y++) {
				printf("drvtest: draw: seeded row %u reads back", y);
				for (unsigned x = 0; x < 4; x++) {
					float v[4] = { 0, 0, 0, 0 };

					read_texel(back + (y * 4 + x) * stride_bytes,
						   [dtd pixelFormat], v);
					printf(" %g/%g", v[0], v[1]);
				}
				printf("   (x/y of each texel; row %u must read y = %u)\n",
				       y, 200 + y);
			}
			/* and the whole volume in ONE read, which is the call that has to
			 * step from one depth to the next: 64 texels, each checked against
			 * what fill_draw_texture wrote (exact for the 16- and 32-bit formats) */
			if (volume) {
				unsigned char vol[4 * 4 * 4 * 16];
				unsigned good = 0;

				memset(vol, 0, sizeof(vol));
				[dtex[i] getBytes:vol
				      bytesPerRow:4 * stride_bytes
				    bytesPerImage:4 * 4 * stride_bytes
				       fromRegion:MTLRegionMake3D(0, 0, 0, 4, 4, 4)
				      mipmapLevel:0
					    slice:0];
				for (unsigned z = 0; z < 4; z++) {
					for (unsigned y = 0; y < 4; y++) {
						for (unsigned x = 0; x < 4; x++) {
							float v[4] = { 0, 0, 0, 0 };

							read_texel(vol + ((z * 4 + y) * 4 + x) *
								   stride_bytes, [dtd pixelFormat], v);
							good += v[0] == 100.0f + x &&
								v[1] == 200.0f + y &&
								v[2] == 300.0f + x + y + z &&
								v[3] == 400.0f;
						}
					}
				}
				printf("drvtest: draw: the 3D texture read back whole (4x4x4 in one "
				       "call): %u of 64 texels as seeded (b = 300 + x + y + z)\n",
				       good);
			}
		}
	}
	MTLSamplerDescriptor *dsd = [[MTLSamplerDescriptor alloc] init];

	dsamp = [dev newSamplerStateWithDescriptor:dsd];
	[dsd release];
	printf("drvtest: draw: %d textures of format %lu for the fragment stage%s\n",
	       dtex[0] != nil ? (dtex[3] != nil ? 4 : 1) : 0, (unsigned long)[dtd pixelFormat],
	       volume ? ", MTLTextureType3D 4x4x4 (--texslots)" : "");

	id<MTLCommandQueue> queue = [dev newCommandQueue];
	id<MTLCommandBuffer> cmd = [queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];

	printf("drvtest: draw: renderCommandEncoderWithDescriptor: gave %s\n",
	       enc != nil ? class_getName(object_getClass(enc)) : "nil");
	if (enc == nil) {
		return;
	}
	[enc setRenderPipelineState:pipeline];
	for (int i = 0; i < 16; i++) {
		[enc setFragmentBuffer:uniform offset:0 atIndex:i];
		/* what each index holds is what some shader declares there: 0 and 14 are
		 * positions, 1 is texture coordinates */
		[enc setVertexBuffer:i == 1 ? coords : corners offset:0 atIndex:i];
		if (i < 4 && dtex[i] != nil) {
			[enc setFragmentTexture:dtex[i] atIndex:i];
		}
		if (dsamp != nil) {
			[enc setFragmentSamplerState:dsamp atIndex:i];
		}
	}
	/* four vertices as a strip: the quad air_run draws over the whole attachment */
	[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	[enc endEncoding];
	[cmd commit];
	[cmd waitUntilCompleted];
	printf("drvtest: draw: status %lu%s%s\n", (unsigned long)[cmd status],
	       [cmd error] != nil ? ", error: " : "",
	       [cmd error] != nil ? [[[cmd error] localizedDescription] UTF8String] : "");
	if ([cmd status] != RW_CB_COMPLETED) {
		return;
	}

	/* What the render path still owes, counted on the device the same way the device's own
	 * protocols were: the encoder and the pipeline state are where the remaining work is,
	 * and a number beats an estimate. */
	for (int which = 0; which < 2; which++) {
		const char *pname = which == 0 ? "MTLRenderCommandEncoder" :
		    "MTLRenderPipelineState";
		id object = which == 0 ? (id)enc : (id)pipeline;
		Protocol *proto = objc_getProtocol(pname);
		unsigned n = 0, missing = 0, shown = 0;

		if (proto == NULL) {
			continue;
		}
		struct objc_method_description *req =
		    protocol_copyMethodDescriptionList(proto, YES, YES, &n);

		for (unsigned i = 0; i < n; i++) {
			if ([object respondsToSelector:req[i].name]) {
				continue;
			}
			missing++;
			if (shown < 6) {
				printf("drvtest: draw:   %s still owes %s\n", pname,
				       sel_getName(req[i].name));
				shown++;
			}
		}
		free(req);
		printf("drvtest: draw: of %u methods %s requires, %u are unanswered\n", n, pname,
		       missing);
	}

	if (draw_all) {
		/* --quiet (M4, W2): the sweep is a timed workload with a cost line (WHAT A
		 * WORKLOAD COSTS, near the top); the single draw above is not in it */
		if (cost_quiet) {
			cost_begin("W2");
		}
		draw_every(dev, library, queue, rp, uniform, dtex, dsamp, targets);
		if (cost_quiet) {
			cost_end("draws", "draw", "draw-all %u shaders", draw_every_attempted);
		}
		goto release;   /* the sweep's objects are this draw's, and are counted the same way */
	}

	unsigned pixel[4] = { 0 };

	[targets[2] getBytes:pixel
		 bytesPerRow:4 * 4 * 4
		  fromRegion:MTLRegionMake2D(0, 0, 1, 1)
		 mipmapLevel:0];
	/* All sixteen pixels, not one.
	 *
	 * One pixel is what made a flat varying look like a vertical flip for three boots:
	 * pixel (0,0) lies in the triangle whose provoking vertex carries the texture
	 * coordinate (0,1), so a FLAT varying puts texel (0,3) there -- exactly what a
	 * mirrored picture would put there. And pixels (3,3) and (0,3) read the same under
	 * flat and under smooth, so of the sixteen only fourteen can tell the two apart.
	 * docs/metal.md 10.32 is the measurement that followed from printing them all.
	 */
	{
		/* the texel's size from the format, so --rtfmt reads back what it drew; for
		 * the default RGBA32Uint this is the four words per pixel it always was */
		unsigned tb = texel_bytes_of(draw_target_format);
		unsigned words = tb >= 4 ? tb / 4 : 1;
		unsigned grid[4 * 4 * 4];

		memset(grid, 0, sizeof(grid));
		[targets[2] getBytes:grid
			 bytesPerRow:4 * tb
			  fromRegion:MTLRegionMake2D(0, 0, 4, 4)
			 mipmapLevel:0];
		for (unsigned gy = 0; gy < 4; gy++) {
			for (unsigned gx = 0; gx < 4; gx++) {
				const unsigned *t = grid + (gy * 4 + gx) * words;

				printf("drvtest: draw: attachment 2 at (%u, %u) =", gx, gy);
				for (unsigned w = 0; w < words; w++) {
					printf(" %u", t[w]);
				}
				printf("\n");
			}
		}
		/*
		 * ONE NUMBER FOR THE WHOLE PICTURE, so two runs -- the two back ends in one
		 * boot (--backend), or two boots -- are compared by one line and not by
		 * sixteen. FNV-1a over the bytes exactly as -getBytes: returned them, rows
		 * tightly packed, the 4x4 texels of attachment 2: nothing is decoded first,
		 * so a difference anywhere in any channel changes it. It is the same code on
		 * both back ends, which is the point.
		 */
		const unsigned char *bytes = (const unsigned char *)grid;
		unsigned long long digest = 0xcbf29ce484222325ull;

		for (unsigned i = 0; i < 4 * 4 * tb; i++) {
			digest = (digest ^ bytes[i]) * 0x100000001b3ull;
		}
		printf("drvtest: draw: digest of attachment 2 (%s, 4x4, pixel format %lu, %u "
		       "bytes): fnv1a64 %016llx\n", shader, (unsigned long)draw_target_format,
		       4 * 4 * tb, digest);
	}
	printf("drvtest: draw: the pixel at (0, 0) of attachment 2 is %u %u %u %u\n",
	       pixel[0], pixel[1], pixel[2], pixel[3]);
	printf("drvtest: draw: softpipe computed                      %u %u %u %u\n",
	       expected[0], expected[1], expected[2], expected[3]);
	if (strcmp(shader, "clearFragment20") != 0) {
		/* The four words above are clearFragment20's; another shader has no constant
		 * here, and metalcheck.py produces its reference by running air_run. */
		printf("drvtest: draw: no constant for %s in this test; metalcheck compares it\n",
		       shader);
	} else {
		printf("drvtest: draw: %s\n",
		       memcmp(pixel, expected, sizeof(pixel)) == 0 ?
		       "THE SAME -- a Metal fragment shader drew on the Adreno" :
		       "DIFFERENT, and a different picture is a failure, not a variation");
	}
	/* The indexed draws go LAST, over this pipeline and these attachments, so that the
	 * picture above is their reference and the only thing that differs between it and
	 * them is the index buffer. */
	if (draw_index) {
		index_draws(dev, queue, rp, pipeline, uniform, corners, coords, dtex, dsamp,
			    targets);
	}
	fflush(stdout);

	/* And what this draw made, released, so that the Vulkan back end's own count of
	 * what it still holds (print_vk_counts, here and in main after the autorelease
	 * pool this runs in has drained the command buffer) can say whether anything
	 * leaked. Last, after every number above is printed: these releases have never
	 * run on the device, and a fault in one must not cost the picture. */
release:
	print_vk_counts("after the draw, before releasing what it made");
	[pipeline release];
	[uniform release];
	[corners release];
	[coords release];
	for (int i = 0; i < 4; i++) {
		[dtex[i] release];
	}
	[dsamp release];
	for (int i = 0; i < 3; i++) {
		[targets[i] release];
	}
	[queue release];
}

/*
 * --draw-depth: whether a depth test discards anything on the Adreno.
 *
 * Every draw this driver has made ran with no depth attachment and so with depth testing
 * off, which is not a choice anyone made -- it is an absence. This is the measurement that
 * closes it, and it is the same experiment air_metal_test runs on softpipe
 * (tools/mesa/air/air_metal_test.c, --draw-depth), so the two can be put side by side:
 *
 *   - a fragment shader that writes COLOUR, drawn four times. Three with
 *     MTLCompareFunctionLess and depth writes on, at depths 0.5, 0.75 and 0.25, and then
 *     once more at 0.75 with MTLCompareFunctionAlways. The quad is the same quad; what
 *     changes is the depth (through MTLViewport's znear, which is the only thing in Metal
 *     that moves this driver's quad in z) and the bytes of its constant buffer, so a draw
 *     that is not discarded leaves a colour the previous one did not. The colour after the
 *     far draw is the test: unchanged means discarded, and the Always control at that same
 *     depth afterwards says the compare function did it rather than the draw.
 *   - a fragment shader that writes DEPTH -- Apple's texturedFragment2DDepth and
 *     texturedFragment3DDepth -- where the buffer must come back holding what the shader
 *     computed. Those two sample a texture and store channel x to [[depth]], so their
 *     texture is seeded in [0, 1] instead of with air_run's pattern, whose 100.0 would
 *     clamp to the far plane and read exactly like the clear value.
 *
 * Which of the two runs is the pipeline's own answer (-writesDepth, out of the compiler
 * library's rwa_render_info) and not a flag here.
 *
 * ON BOTH BACK ENDS since M3c-2: --backend vk runs the same steps through air_vk.c's depth
 * attachment, MTLDepthStencilState (Vulkan dynamic state) and rwa_depth_read, and every run
 * ends in one machine-readable line, drvtest: depth-result (depth_path), so the gallium and
 * vk runs of one boot can be compared line for line. Two things differ by GPU driver and not
 * by this test: Stencil8 is a texture turnip makes and freedreno's Gallium driver does not
 * (both then refuse it as a depth attachment, which has no depth aspect), and
 * texturedFragment2DDepth's texture is a depth2d, which is a Depth32Float texture now and
 * seeded with one value everywhere (DEPTH2D_SEED) -- where it used to be an RGBA32Float
 * texture with the seed below, a binding Metal itself would not accept.
 */
static MTLPixelFormat depth_attachment_format = MTLPixelFormatDepth32Float;

/* 64 values in (0, 1), each exact as a float, so the one that comes back names the texel it
 * came from -- and none of them is 0 or 1, which a cleared or untouched buffer could hold. */
static float
depth_texel_value(unsigned x, unsigned y, unsigned z)
{
	return ((float)(x + 4 * y + 16 * z) + 0.5f) / 64.0f;
}

static void
fill_depth_texture(id<MTLTexture> tex)
{
	MTLPixelFormat pf = [tex pixelFormat];
	unsigned bytes = texel_bytes_of(pf);
	unsigned char row[DRAW_TEX * 16];

	for (NSUInteger z = 0; z < [tex depth]; z++) {
		for (NSUInteger y = 0; y < [tex height]; y++) {
			for (NSUInteger x = 0; x < [tex width]; x++) {
				float d = depth_texel_value((unsigned)x, (unsigned)y,
							    (unsigned)z);
				float v[4] = { d, d, d, d };

				write_texel(row + x * bytes, pf, v);
			}
			[tex replaceRegion:MTLRegionMake3D(0, y, z, [tex width], 1, 1)
			       mipmapLevel:0
				     slice:0
				 withBytes:row
			       bytesPerRow:[tex width] * bytes
			     bytesPerImage:0];
		}
	}
}

/* The depth buffer, through the driver's own aspect reader: -getBytes: has no aspect to
 * name and the compiler library refuses a depth texture through it by name, so this asks
 * for the depth aspect as floats. Returns false when the driver is older than this test. */
static bool
read_depth_buffer(id<MTLTexture> zt, float *depth, const char *what)
{
	SEL sel = sel_getUid("hkReadDepth:stencil:fromRegion:");

	if (![zt respondsToSelector:sel]) {
		printf("drvtest: depth: this texture does not answer "
		       "hkReadDepth:stencil:fromRegion:, so the driver is older than this "
		       "test\n");
		return false;
	}
	BOOL ok = ((BOOL (*)(id, SEL, float *, uint8_t *, MTLRegion))objc_msgSend)(
	    zt, sel, depth, NULL, MTLRegionMake2D(0, 0, DRAW_TEX, DRAW_TEX));

	if (!ok) {
		printf("drvtest: depth: %s: the depth could not be read back\n", what);
		return false;
	}
	bool uniform = true;

	for (unsigned i = 1; i < DRAW_TEX * DRAW_TEX; i++) {
		if (depth[i] != depth[0]) {
			uniform = false;
		}
	}
	printf("drvtest: depth: %s: depth at (0,0) = %.9g, and the 16 texels are %s\n", what,
	       depth[0], uniform ? "all that value" : "NOT all the same");
	return uniform;
}

/*
 * --argbuf: MTLArgumentEncoder on the device, and the buffer OFFSET that goes with it.
 *
 * Three measurements, and what each is worth is different, so they are printed apart:
 *
 *  1. THE FUNCTION PATH. -[MTLFunction newArgumentEncoderWithBufferIndex:] is a method of
 *     Metal's own class, and on this port Metal's own implementation cannot work: it asks
 *     the function for its reflection, which asks the device for -compiler, which is a
 *     two-instruction stub answering nil (0x181aac128), and then reports
 *     "bufferIndex %lu does not identify an argument buffer" and returns nil. The driver
 *     answers it instead, through a category, by reading the function's own bitcode. So
 *     what this measures is WHOSE answer came back: Apple's shaders declare no argument
 *     buffer at all (0 of 456 modules, docs/air.md 8.2), so the answer must still be nil
 *     -- but with the driver's own sentence, which names how many sets the function has.
 *     Metal's sentence and the driver's are told apart in the log by "this function
 *     declares".
 *  2. THE DESCRIPTOR PATH, which needs no shader: -[MTLDevice newArgumentEncoderWithArguments:]
 *     with a pointer, a texture and a sampler member. encodedLength must be 24 and
 *     alignment 8 -- 8 bytes per member, which is what Apple's own metadata says a set is
 *     (7 856 of Resident Evil 4's 7 858 sets; the two that differ are both in the one
 *     kernel that builds indirect command buffers and are refused before an encoder could
 *     be made). Then the three members are encoded and the compiler library is asked how
 *     many records it holds for that (buffer, offset), which is the whole of what an
 *     encoder does here.
 *  3. THE BUFFER OFFSET, which is not about argument buffers at all and is the reason this
 *     mode exists tonight: Apple's own clearFragment20 drawn twice, with its 48-byte
 *     constant block at offset 0 of one buffer and at OFFSET 1164 of another. 1164 is the
 *     offset the window server binds its own fragment buffer at, 55 times in one boot,
 *     and until this work every one of those bindings was refused. The two pictures must
 *     be the same four words, and the same four words the host reference gives
 *     (tools/air/metalcheck.py, and draw_path's own `expected`).
 *
 * What is NOT here, said plainly: a DRAW through an argument buffer. That needs a
 * .metallib holding a shader that declares one, and this port has no container writer --
 * Apple's libraries have no sets, and the game's are not something a job list can reach.
 * The draw-through-an-encoder differential is therefore the host's
 * (tools/air/argbufruncheck.sh, on softpipe), and what the device adds is the three above.
 */
static void
argbuf_path(id<MTLDevice> dev, const char *metallib)
{
	static const unsigned expected[4] = { 2458616957u, 2930221209u, 3401825461u,
					      3873429713u };
	NSError *err = nil;
	NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:metallib]];

	if (data == nil) {
		printf("drvtest: argbuf: cannot read %s\n", metallib);
		return;
	}
	dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL,
						  DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	id<MTLLibrary> library = [dev newLibraryWithData:dd error:&err];

	if (library == nil) {
		printf("drvtest: argbuf: newLibraryWithData: gave nil\n");
		return;
	}

	/* 1. the function path */
	id<MTLFunction> fragment = [library newFunctionWithName:
	    [NSString stringWithUTF8String:fragment_function]];

	if (fragment == nil) {
		printf("drvtest: argbuf: no %s in that library\n", fragment_function);
		return;
	}
	SEL withIndex = sel_getUid("newArgumentEncoderWithBufferIndex:");

	printf("drvtest: argbuf: %s answers newArgumentEncoderWithBufferIndex: %s\n",
	       fragment_function,
	       [fragment respondsToSelector:withIndex] ? "yes" : "NO (nothing implements it)");
	if ([fragment respondsToSelector:withIndex]) {
		id enc = ((id (*)(id, SEL, NSUInteger))objc_msgSend)(fragment, withIndex, 0);

		printf("drvtest: argbuf: an encoder for [[buffer(0)]] of %s is %s (Apple's "
		       "shaders declare no argument buffer, so nil is the right answer and "
		       "the LINE ABOVE from the driver is the measurement)\n",
		       fragment_function,
		       enc != nil ? class_getName(object_getClass(enc)) : "nil");
		[enc release];
	}

	/* 2. the descriptor path */
	Class descClass = objc_lookUpClass("MTLArgumentDescriptor");
	SEL newArgs = sel_getUid("newArgumentEncoderWithArguments:");

	if (descClass != NULL && [dev respondsToSelector:newArgs]) {
		static const NSUInteger type[3] = { 60 /* Pointer */, 58 /* Texture */,
						    59 /* Sampler */ };
		NSMutableArray *args = [NSMutableArray array];

		for (unsigned i = 0; i < 3; i++) {
			id d = [descClass argumentDescriptor];

			[d setIndex:i];
			[d setDataType:(MTLDataType)type[i]];
			[d setArrayLength:0];
			[args addObject:d];
		}
		id enc = ((id (*)(id, SEL, id))objc_msgSend)(dev, newArgs, args);

		if (enc == nil) {
			printf("drvtest: argbuf: newArgumentEncoderWithArguments: gave nil\n");
		} else {
			NSUInteger len = [enc encodedLength], align = [enc alignment];

			printf("drvtest: argbuf: %s: encodedLength %lu (must be 24: 8 bytes a "
			       "member), alignment %lu (must be 8)\n",
			       len == 24 && align == 8 ? "ok" : "WRONG",
			       (unsigned long)len, (unsigned long)align);

			/* and the records, which is what this encoder does instead of
			 * writing: the buffer's own bytes are never read by a draw. */
			id<MTLBuffer> dest = [dev newBufferWithLength:len > 0 ? len : 64
							     options:0];
			id<MTLBuffer> member = [dev newBufferWithLength:256 options:0];
			MTLTextureDescriptor *td = [MTLTextureDescriptor
			    texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint
							 width:4 height:4 mipmapped:NO];
			[td setUsage:MTLTextureUsageShaderRead];
			id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
			MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
			id<MTLSamplerState> samp = [dev newSamplerStateWithDescriptor:sd];

			[sd release];
			[enc setArgumentBuffer:dest offset:0];
			[enc setBuffer:member offset:0 atIndex:0];
			[enc setTexture:tex atIndex:1];
			[enc setSamplerState:samp atIndex:2];
			/* An id past the set's own members, which Metal would write a handle
			 * for and this driver refuses by name: the line comes from the
			 * driver, and what is checked here is that nothing crashed. */
			[enc setTexture:tex atIndex:7];

			unsigned recorded = argbuf_records(dest, 0);

			printf("drvtest: argbuf: %s: %u of 3 members recorded against that "
			       "buffer at offset 0\n", recorded == 3 ? "ok" : "WRONG",
			       recorded);
			[dest release];
			[member release];
			[tex release];
			[samp release];
			[enc release];
		}
	} else {
		printf("drvtest: argbuf: no MTLArgumentDescriptor class or no "
		       "newArgumentEncoderWithArguments: on this device\n");
	}

	/* 3. the buffer offset, which is the window server's own case */
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint
							      width:4 height:4
							  mipmapped:NO];

	[td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];

	id<MTLTexture> targets[3] = { nil, nil, nil };

	for (int i = 0; i < 3; i++) {
		targets[i] = [dev newTextureWithDescriptor:td];
		if (targets[i] == nil) {
			printf("drvtest: argbuf: attachment %d could not be made\n", i);
			return;
		}
	}
	MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];

	[pd setFragmentFunction:fragment];
	[pd setVertexFunction:nil];
	for (int i = 0; i < 3; i++) {
		[[[pd colorAttachments] objectAtIndexedSubscript:i]
		    setPixelFormat:MTLPixelFormatRGBA32Uint];
	}
	err = nil;
	id<MTLRenderPipelineState> pipeline = [dev newRenderPipelineStateWithDescriptor:pd
										  error:&err];
	[pd release];
	if (pipeline == nil) {
		printf("drvtest: argbuf: no pipeline: %s\n",
		       err != nil ? [[err localizedDescription] UTF8String] : "(no reason)");
		return;
	}

	/* The same 48 bytes in two places: at offset 0 of one buffer, and at offset 1164 of
	 * another whose other bytes are deliberately NOT that pattern -- so a driver that
	 * ignored the offset would read the wrong block and print different words, and one
	 * that refused the binding would print a refusal. */
	const NSUInteger at = 1164;
	id<MTLBuffer> flat = [dev newBufferWithLength:256 options:0];
	id<MTLBuffer> moved = [dev newBufferWithLength:4096 options:0];

	if (flat == nil || moved == nil || [flat contents] == NULL ||
	    [moved contents] == NULL) {
		printf("drvtest: argbuf: the two uniform buffers could not be made\n");
		return;
	}
	unsigned char *fb = [flat contents], *mb = [moved contents];

	for (NSUInteger i = 0; i < 256; i++) {
		fb[i] = (unsigned char)((i * 7 + 13) & 0xff);
	}
	memset(mb, 0xa5, 4096);
	for (NSUInteger i = 0; i < 256; i++) {
		mb[at + i] = fb[i];
	}

	id<MTLCommandQueue> queue = [dev newCommandQueue];
	struct { id<MTLBuffer> buffer; NSUInteger offset; const char *what; } run[2] = {
		{ flat, 0, "at offset 0" },
		{ moved, at, "at offset 1164, the window server's own" },
	};

	for (unsigned k = 0; k < 2; k++) {
		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];

		for (int i = 0; i < 3; i++) {
			MTLRenderPassColorAttachmentDescriptor *a =
			    [[rp colorAttachments] objectAtIndexedSubscript:i];

			[a setTexture:targets[i]];
			[a setLoadAction:MTLLoadActionClear];
			[a setClearColor:MTLClearColorMake(0, 0, 0, 0)];
			[a setStoreAction:MTLStoreActionStore];
		}
		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		id<MTLRenderCommandEncoder> enc =
		    [cmd renderCommandEncoderWithDescriptor:rp];

		if (enc == nil) {
			printf("drvtest: argbuf: no render encoder\n");
			return;
		}
		[enc setRenderPipelineState:pipeline];
		/* every index up to 16, as draw_path binds it: this shader's block is
		 * declared at [[buffer(14)]] and a driver that bound only index 0 would
		 * leave it reading nothing */
		for (int i = 0; i < 16; i++) {
			[enc setFragmentBuffer:run[k].buffer offset:run[k].offset atIndex:i];
		}
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		[enc endEncoding];
		[cmd commit];
		[cmd waitUntilCompleted];

		if ([cmd status] != RW_CB_COMPLETED) {
			printf("drvtest: argbuf: the draw %s did not complete: status %lu%s%s\n",
			       run[k].what, (unsigned long)[cmd status],
			       [cmd error] != nil ? ", " : "",
			       [cmd error] != nil
				   ? [[[cmd error] localizedDescription] UTF8String] : "");
			continue;
		}
		unsigned pixel[4] = { 0 };

		[targets[2] getBytes:pixel
			 bytesPerRow:4 * 4 * 4
			  fromRegion:MTLRegionMake2D(0, 0, 1, 1)
			 mipmapLevel:0];
		bool same = pixel[0] == expected[0] && pixel[1] == expected[1] &&
			    pixel[2] == expected[2] && pixel[3] == expected[3];

		printf("drvtest: argbuf: %s: the uniform block %s draws %u %u %u %u\n",
		       same ? "ok" : "WRONG", run[k].what, pixel[0], pixel[1], pixel[2],
		       pixel[3]);
	}
	printf("drvtest: argbuf: the four words above are the host's own reference "
	       "(tools/air/metalcheck.py): %u %u %u %u\n", expected[0], expected[1],
	       expected[2], expected[3]);
}

/*
 * THE RESULT LINE of --draw-depth, one per run in one shape whatever happened, so the two
 * runs of one boot (--backend gallium and --backend vk, M3c-2) can be put side by side the
 * way --draw-all's drawall-result lines are:
 *
 *     drvtest: depth-result BACKEND FRAGMENT FORMAT VERDICT d D0 D1 D2 D3
 *
 * FORMAT is the MTLPixelFormat number. VERDICT is `discards` or `disagrees` for a shader
 * that writes colour (the four-step test), `same` or `different` for one that stores
 * [[depth]] (one step; D1..D3 are then 0), or how it stopped -- `no-pipeline REASON`,
 * `no-texture REASON`, `no-depth-attachment`, `stopped` -- with no depths after it. The
 * depths are the buffer's texel (0, 0) after each step, %.9g, which is exact for
 * Depth32Float and shows Depth16Unorm's quantum.
 */
static char depth_result[320];

static void depth_path_steps(id<MTLDevice> dev, const char *metallib);

static void
depth_path(id<MTLDevice> dev, const char *metallib)
{
	depth_result[0] = '\0';
	depth_path_steps(dev, metallib);
	printf("drvtest: depth-result %s %s %lu %s\n", backend_is_vk() ? "vk" : "gallium",
	       fragment_function, (unsigned long)depth_attachment_format,
	       depth_result[0] != '\0' ? depth_result : "stopped");
	fflush(stdout);
}

static void
depth_path_steps(id<MTLDevice> dev, const char *metallib)
{
	NSError *err = nil;
	NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:metallib]];

	if (data == nil) {
		printf("drvtest: depth: cannot read %s\n", metallib);
		return;
	}
	dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL,
						  DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	id<MTLLibrary> library = [dev newLibraryWithData:dd error:&err];

	if (library == nil) {
		printf("drvtest: depth: newLibraryWithData: gave nil\n");
		return;
	}
	id<MTLFunction> fragment = [library newFunctionWithName:
	    [NSString stringWithUTF8String:fragment_function]];

	if (fragment == nil) {
		printf("drvtest: depth: no %s in that library\n", fragment_function);
		return;
	}
	printf("drvtest: depth: %s %s, depth attachment pixel format %lu\n", metallib,
	       fragment_function, (unsigned long)depth_attachment_format);
	load_texture_query(dev);

	/* Three colour attachments of air_run's own format, as the colour draw has: the
	 * shader this defaults to writes render target 2, and 0 and 1 have to exist for 2
	 * to be bound at all. */
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint
							      width:DRAW_TEX
							     height:DRAW_TEX
							  mipmapped:NO];

	[td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];

	id<MTLTexture> targets[3] = { nil, nil, nil };

	for (int i = 0; i < 3; i++) {
		targets[i] = [dev newTextureWithDescriptor:td];
		if (targets[i] == nil) {
			printf("drvtest: depth: colour attachment %d could not be made\n", i);
			return;
		}
	}

	/* The depth attachment: MTLTextureUsageRenderTarget and nothing else. The compiler
	 * library turns that usage into PIPE_BIND_DEPTH_STENCIL because the FORMAT is a
	 * depth one, which is the one place where the same Metal usage bit means two
	 * different Gallium bindings. */
	MTLTextureDescriptor *ztd =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:depth_attachment_format
							      width:DRAW_TEX
							     height:DRAW_TEX
							  mipmapped:NO];

	[ztd setUsage:MTLTextureUsageRenderTarget];

	id<MTLTexture> zt = [dev newTextureWithDescriptor:ztd];

	if (zt == nil) {
		printf("drvtest: depth: the depth attachment could not be made; the driver has "
		       "printed the compiler library's own reason\n");
		snprintf(depth_result, sizeof(depth_result), "no-depth-attachment");
		return;
	}
	printf("drvtest: depth: the depth attachment is a %s of pixel format %lu\n",
	       class_getName(object_getClass(zt)), (unsigned long)[zt pixelFormat]);

	/* And the proof that it is not readable as colour, which is the contract and not an
	 * accident: a caller asking for a depth texel the colour way is told which call to
	 * use. The driver prints the library's sentence. */
	{
		unsigned char ignored[DRAW_TEX * DRAW_TEX * 8];

		memset(ignored, 0, sizeof(ignored));
		printf("drvtest: depth: -getBytes: on the depth attachment must be refused:\n");
		[zt getBytes:ignored
		 bytesPerRow:DRAW_TEX * 8
		  fromRegion:MTLRegionMake2D(0, 0, DRAW_TEX, DRAW_TEX)
		 mipmapLevel:0];
	}

	MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];

	[pd setFragmentFunction:fragment];
	[pd setVertexFunction:nil];   /* the library generates the stage, as air_run does */
	for (int i = 0; i < 3; i++) {
		[[[pd colorAttachments] objectAtIndexedSubscript:i]
		    setPixelFormat:MTLPixelFormatRGBA32Uint];
	}
	/* Both aspect properties, as an application with a combined format sets them: one
	 * texture behind the two, which is what the library requires of the pair. */
	[pd setDepthAttachmentPixelFormat:depth_attachment_format];
	if (depth_attachment_format == MTLPixelFormatDepth32Float_Stencil8) {
		[pd setStencilAttachmentPixelFormat:depth_attachment_format];
	}
	err = nil;
	id<MTLRenderPipelineState> pipeline = [dev newRenderPipelineStateWithDescriptor:pd
										  error:&err];

	[pd release];
	printf("drvtest: depth: newRenderPipelineStateWithDescriptor: gave %s%s%s\n",
	       pipeline != nil ? class_getName(object_getClass(pipeline)) : "nil",
	       err != nil ? ", error: " : "",
	       err != nil ? [[err localizedDescription] UTF8String] : "");
	if (pipeline == nil) {
		snprintf(depth_result, sizeof(depth_result), "no-pipeline %s",
			 err != nil ? [[err localizedDescription] UTF8String] : "(no reason)");
		return;
	}
	SEL writes = sel_getUid("writesDepth");
	bool shader_writes_depth =
	    [pipeline respondsToSelector:writes] &&
	    ((BOOL (*)(id, SEL))objc_msgSend)(pipeline, writes);

	printf("drvtest: depth: this pipeline's fragment shader %s to [[depth]]\n",
	       shader_writes_depth ? "STORES" : "stores nothing");

	/*
	 * The two depth/stencil states, which is the first MTLDepthStencilState this driver
	 * has ever been asked for. What they answer about themselves is printed, because
	 * those four methods are the whole of <MTLDepthStencilStateSPI> beyond the public
	 * protocol and Apple's base class answers NO to all four -- so "writesDepth = YES"
	 * here is the override being read back, not a default.
	 */
	MTLDepthStencilDescriptor *dsd = [[MTLDepthStencilDescriptor alloc] init];

	[dsd setDepthCompareFunction:MTLCompareFunctionLess];
	[dsd setDepthWriteEnabled:YES];
	id<MTLDepthStencilState> less = [dev newDepthStencilStateWithDescriptor:dsd];

	[dsd setDepthCompareFunction:MTLCompareFunctionAlways];
	id<MTLDepthStencilState> always = [dev newDepthStencilStateWithDescriptor:dsd];

	[dsd release];
	if (less == nil || always == nil) {
		printf("drvtest: depth: newDepthStencilStateWithDescriptor: gave nil, so there "
		       "is no state to test with\n");
		return;
	}
	printf("drvtest: depth: newDepthStencilStateWithDescriptor: gave %s\n",
	       class_getName(object_getClass(less)));
	{
		static const char *const spi[4] = { "readsDepth", "writesDepth", "readsStencil",
						    "writesStencil" };
		unsigned missing = 0;

		printf("drvtest: depth: the Less state answers");
		for (unsigned i = 0; i < 4; i++) {
			SEL s = sel_getUid(spi[i]);

			if (![less respondsToSelector:s]) {
				missing++;
				printf(" %s=unanswered", spi[i]);
				continue;
			}
			printf(" %s=%s", spi[i],
			       ((BOOL (*)(id, SEL))objc_msgSend)(less, s) ? "YES" : "NO");
		}
		printf(" (%u unanswered)\n", missing);

		Protocol *proto = objc_getProtocol("MTLDepthStencilState");
		unsigned n = 0, unanswered = 0;

		if (proto != NULL) {
			struct objc_method_description *req =
			    protocol_copyMethodDescriptionList(proto, YES, YES, &n);

			for (unsigned i = 0; i < n; i++) {
				if (![less respondsToSelector:req[i].name]) {
					printf("drvtest: depth:   MTLDepthStencilState still "
					       "owes %s\n", sel_getName(req[i].name));
					unanswered++;
				}
			}
			free(req);
		}
		printf("drvtest: depth: of %u methods MTLDepthStencilState requires, %u are "
		       "unanswered\n", n, unanswered);
	}

	/* The shader's constant buffer, bound at every index up to 16 because the one this
	 * defaults to is declared at 14 (draw_path says why). Its bytes are what change
	 * between steps, so a draw that was not discarded leaves a different colour. */
	id<MTLBuffer> uniform = [dev newBufferWithLength:256 options:0];

	if (uniform == nil || [uniform contents] == NULL) {
		printf("drvtest: depth: no constant buffer\n");
		return;
	}

	/* The textures the shader declares, if any, made the shape each slot asks for --
	 * and then seeded in [0, 1] when the shader stores what it samples into [[depth]],
	 * because air_run's 100.0 would clamp to the far plane and read as the clear. */
	id<MTLTexture> dtex[RW_TEX_SLOTS] = { nil };
	id<MTLSamplerState> dsamp = nil;
	unsigned declared = 0;
	char texwhy[256] = "";

	if (!textures_for_shader(dev, fragment, dtex, RW_TEX_SLOTS, &declared, texwhy,
				 sizeof(texwhy))) {
		printf("drvtest: depth: %s: %s\n", fragment_function, texwhy);
		snprintf(depth_result, sizeof(depth_result), "no-texture %s", texwhy);
		return;
	}
	bool declared_3d = false;
	/* A depth2d slot is a Depth32Float texture since M3c-2, already seeded by
	 * textures_for_shader with DEPTH2D_SEED in every texel: there is no per-texel write of a
	 * depth texture on either back end (DEPTH2D_SEED says why), so it is left as it is. */
	bool depth_seeded = false;

	for (unsigned i = 0; i < RW_TEX_SLOTS; i++) {
		if (dtex[i] == nil) {
			continue;
		}
		if ([dtex[i] textureType] == MTLTextureType3D) {
			declared_3d = true;
		}
		if ([dtex[i] pixelFormat] == MTLPixelFormatDepth32Float) {
			depth_seeded = true;
			continue;
		}
		if (shader_writes_depth) {
			fill_depth_texture(dtex[i]);
		}
	}
	if (declared) {
		MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];

		dsamp = [dev newSamplerStateWithDescriptor:sd];
		[sd release];
		printf("drvtest: depth: %u texture slot%s declared, seeded with %s\n", declared,
		       declared == 1 ? "" : "s",
		       depth_seeded ? "0.0859375 in every texel (a depth2d slot, given a "
				      "Depth32Float texture cleared to that value)" :
		       shader_writes_depth ? "(x + 4y + 16z + 0.5) / 64 in every channel"
					   : "air_run's texels");
	}

	id<MTLCommandQueue> queue = [dev newCommandQueue];

	/*
	 * One step: a render pass of its own, because a pass is where a load action lives
	 * and these steps differ in whether they clear. The first clears the colour and the
	 * depth; the rest load both, which is how the depth of the previous draw is still
	 * there to be tested against.
	 */
	struct step {
		const char *what;
		float z;
		bool pattern;
		unsigned char fill;
		bool clear;
		bool control;
		bool expect_draw;
	};
	static const struct step colour_steps[4] = {
		{ "the near draw at depth 0.5",            0.5f,  true,  0x00, true,  false, true },
		{ "the far draw at depth 0.75",            0.75f, false, 0xa5, false, false, false },
		{ "the nearer draw at depth 0.25",         0.25f, false, 0x5a, false, false, true },
		{ "the control at depth 0.75 with Always", 0.75f, false, 0x3c, false, true,  true },
	};
	static const struct step depth_steps[1] = {
		{ "the draw, with the test off and depth writes on", 0.5f, true, 0x00, true,
		  true, true },
	};
	const struct step *steps = shader_writes_depth ? depth_steps : colour_steps;
	unsigned nsteps = shader_writes_depth ? 1 : 4;
	unsigned pixel[4][4];
	float depth_at[4];
	bool uniform_depth = true;

	memset(pixel, 0, sizeof(pixel));
	memset(depth_at, 0, sizeof(depth_at));
	for (unsigned s = 0; s < nsteps; s++) {
		unsigned char *bytes = [uniform contents];

		for (int i = 0; i < 256; i++) {
			bytes[i] = steps[s].pattern ?
			    (unsigned char)((i * 7 + 13) & 0xff) : steps[s].fill;
		}

		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];

		for (int i = 0; i < 3; i++) {
			MTLRenderPassColorAttachmentDescriptor *a =
			    [[rp colorAttachments] objectAtIndexedSubscript:i];

			[a setTexture:targets[i]];
			[a setLoadAction:steps[s].clear ? MTLLoadActionClear
							: MTLLoadActionLoad];
			[a setClearColor:MTLClearColorMake(0, 0, 0, 0)];
			[a setStoreAction:MTLStoreActionStore];
		}
		MTLRenderPassDepthAttachmentDescriptor *za = [rp depthAttachment];

		[za setTexture:zt];
		[za setLoadAction:steps[s].clear ? MTLLoadActionClear : MTLLoadActionLoad];
		[za setClearDepth:1.0];
		[za setStoreAction:MTLStoreActionStore];

		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];

		if (enc == nil) {
			printf("drvtest: depth: %s: no encoder\n", steps[s].what);
			return;
		}
		[enc setRenderPipelineState:pipeline];
		[enc setDepthStencilState:steps[s].control ? always : less];
		/* The quad's depth. MTLViewport's znear is the only thing in Metal that moves
		 * it: the generated vertex stage puts all four corners at clip z = 0, and
		 * Metal's NDC z starts at 0 on the near plane, so the window depth of what is
		 * drawn is znear exactly. The rectangle is the whole attachment, which is what
		 * the driver serves. */
		[enc setViewport:(MTLViewport){ 0.0, 0.0, DRAW_TEX, DRAW_TEX, steps[s].z, 1.0 }];
		for (int i = 0; i < 16; i++) {
			[enc setFragmentBuffer:uniform offset:0 atIndex:i];
			if (i < RW_TEX_SLOTS && dtex[i] != nil) {
				[enc setFragmentTexture:dtex[i] atIndex:i];
			}
			if (dsamp != nil) {
				[enc setFragmentSamplerState:dsamp atIndex:i];
			}
		}
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		[enc endEncoding];
		[cmd commit];
		[cmd waitUntilCompleted];
		printf("drvtest: depth: %s: status %lu%s%s\n", steps[s].what,
		       (unsigned long)[cmd status], [cmd error] != nil ? ", error: " : "",
		       [cmd error] != nil ? [[[cmd error] localizedDescription] UTF8String] : "");
		if ([cmd status] != RW_CB_COMPLETED) {
			return;
		}

		[targets[2] getBytes:pixel[s]
			 bytesPerRow:DRAW_TEX * 4 * 4
			  fromRegion:MTLRegionMake2D(0, 0, 1, 1)
			 mipmapLevel:0];
		printf("drvtest: depth: %s: the pixel at (0, 0) of attachment 2 is %u %u %u %u\n",
		       steps[s].what, pixel[s][0], pixel[s][1], pixel[s][2], pixel[s][3]);

		float map[DRAW_TEX * DRAW_TEX];

		if (!read_depth_buffer(zt, map, steps[s].what)) {
			uniform_depth = false;
		}
		depth_at[s] = map[0];
	}

	/* The verdicts. Each says what would have to be true for it to read otherwise, and
	 * the tolerance is the format's: Depth32Float holds 0.25, 0.5 and 0.75 exactly, so
	 * there it is ==; Depth16Unorm holds none of them and gets one step of itself. */
	float tol = depth_attachment_format == MTLPixelFormatDepth16Unorm ?
	    1.0f / 65535.0f : 0.0f;

	if (shader_writes_depth) {
		float want = depth_texel_value(1, 1, declared_3d ? 1 : 0);
		float got = depth_at[0];
		float d = got - want;
		bool same = (d < 0.0f ? -d : d) <= tol;

		printf("drvtest: depth: the shader sampled texel (1, 1, %u) and stored %.9g to "
		       "[[depth]]; the buffer holds %.9g\n", declared_3d ? 1 : 0, want, got);
		if (depth_seeded) {
			printf("drvtest: depth: its depth2d texture holds 0.0859375 in EVERY texel, "
			       "so this says the depth texture was sampled and stored, and not "
			       "which texel\n");
		}
		printf("drvtest: depth: the quad's own depth was 0.5 and the clear was 1, so "
		       "neither could be mistaken for it\n");
		printf("drvtest: depth: %s\n", same && uniform_depth ?
		       "THE SAME -- a Metal fragment shader's [[depth]] reached the depth "
		       "buffer on the Adreno" :
		       "DIFFERENT, and a depth buffer that is nearly right is worse than none");
		snprintf(depth_result, sizeof(depth_result), "%s d %.9g 0 0 0",
			 same && uniform_depth ? "same" : "different", got);
	} else {
		bool far_discarded = memcmp(pixel[1], pixel[0], sizeof(pixel[0])) == 0;
		bool nearer_drawn = memcmp(pixel[2], pixel[0], sizeof(pixel[0])) != 0;
		bool control_drawn = memcmp(pixel[3], pixel[2], sizeof(pixel[0])) != 0;
		static const float want[4] = { 0.5f, 0.5f, 0.25f, 0.75f };
		bool depths_right = true;

		for (unsigned s = 0; s < 4; s++) {
			float d = depth_at[s] - want[s];

			if ((d < 0.0f ? -d : d) > tol) {
				depths_right = false;
			}
		}
		printf("drvtest: depth: the far draw was %s, and its buffer held a different "
		       "fill, so an undiscarded one would have changed the pixel\n",
		       far_discarded ? "DISCARDED (the pixel did not change)"
				     : "NOT discarded -- the pixel changed");
		printf("drvtest: depth: the nearer draw %s\n",
		       nearer_drawn ? "PASSED (the pixel changed)"
				    : "did NOT pass, and it is in front of what was there");
		printf("drvtest: depth: the Always control at the same far depth %s\n",
		       control_drawn ? "DREW (so the compare function discarded the other one, "
				       "not the draw)"
				     : "did not draw either, so the fault is not the depth test");
		printf("drvtest: depth: depth after each step: %.9g %.9g %.9g %.9g (0.5, 0.5 "
		       "kept, 0.25, 0.75)\n", depth_at[0], depth_at[1], depth_at[2],
		       depth_at[3]);
		printf("drvtest: depth: %s\n",
		       far_discarded && nearer_drawn && control_drawn && depths_right &&
		       uniform_depth ?
		       "DEPTH TESTING DISCARDS ON THE ADRENO -- the compare function, the depth "
		       "write and the buffer all agree" :
		       "SOMETHING ABOVE DISAGREES, and a depth test that is nearly right is "
		       "worse than none");
		snprintf(depth_result, sizeof(depth_result), "%s d %.9g %.9g %.9g %.9g",
			 far_discarded && nearer_drawn && control_drawn && depths_right &&
			 uniform_depth ? "discards" : "disagrees",
			 depth_at[0], depth_at[1], depth_at[2], depth_at[3]);
	}
	fflush(stdout);
}

/*
 * Where it died, in terms of the image it died in.
 *
 * A program that faults on this port leaves nothing behind -- the kernel prints "Failed to
 * send exception EXC_CORPSE_NOTIFY" and that is all -- so the fault has to print itself. The
 * addresses that matter here are not in the shared cache but inside
 * /hackios/lib/libhackiosair.dylib, so the slide trick the other probes use (docs/metal.md
 * section 6) is no help: dladdr gives the image and the offset within it, which
 * llvm-addr2line on the machine turns back into a line.
 */
static void
fault(int sig, siginfo_t *info, void *uap)
{
	ucontext_t *uc = uap;
	uintptr_t pc = 0, lr = 0, fp = 0;

	if (uc != NULL && uc->uc_mcontext != NULL) {
		pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
		lr = (uintptr_t)uc->uc_mcontext->__ss.__lr;
		fp = (uintptr_t)uc->uc_mcontext->__ss.__fp;
	}
	/* --quiet (M4): the console back first, with what the workload had printed so far */
	cost_capture_rescue();
	printf("drvtest: SIGNAL %d at %p\n", sig, info != NULL ? info->si_addr : NULL);
	for (int i = 0; i < 10; i++) {
		uintptr_t at = i == 0 ? pc : i == 1 ? lr : 0;
		Dl_info d;

		if (i >= 2) {
			if (fp <= 0x1000) {
				break;
			}
			at = ((uintptr_t *)fp)[1];
			fp = ((uintptr_t *)fp)[0];
			if (at == 0) {
				break;
			}
		}
		if (dladdr((void *)at, &d) != 0 && d.dli_fname != NULL) {
			const char *slash = strrchr(d.dli_fname, '/');

			printf("drvtest:   %-2s %s+%#lx  (%s)\n", i == 0 ? "pc" : i == 1 ? "lr" : "",
			       slash != NULL ? slash + 1 : d.dli_fname,
			       at - (uintptr_t)d.dli_fbase,
			       d.dli_sname != NULL ? d.dli_sname : "no symbol");
		} else {
			printf("drvtest:   %-2s %#lx (in no image)\n",
			       i == 0 ? "pc" : i == 1 ? "lr" : "", at);
		}
	}
	fflush(stdout);
	_exit(sig);
}

/*
 * --blit: every kind of copy an MTLBlitCommandEncoder makes, through Metal's own API, each
 * read back and compared with bytes computed here -- in one command buffer per check, so a
 * failure names the call. RGBA8Unorm so a texel is four bytes and survives every copy exactly.
 */
static int
blit_test(id<MTLDevice> dev)
{
	id<MTLCommandQueue> queue = [dev newCommandQueue];
	id<MTLBuffer> a = [dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
	id<MTLBuffer> b = [dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
	uint8_t *pa = [a contents], *pb = [b contents];
	int failed = 0;

#define BLIT_RUN(encode) ({ \
		id<MTLCommandBuffer> cb = [queue commandBuffer]; \
		id<MTLBlitCommandEncoder> enc = [cb blitCommandEncoder]; \
		if (enc == nil) { printf("drvtest: blit: blitCommandEncoder is nil\n"); return 1; } \
		encode; \
		[enc endEncoding]; [cb commit]; [cb waitUntilCompleted]; \
		[cb status] == MTLCommandBufferStatusCompleted ? nil : [cb error]; })
#define BLIT_CHECK(what, ok, err) do { \
		printf("drvtest: blit: %-48s %s%s%s\n", what, (ok) ? "ok" : "WRONG", \
		       (err) ? " -- " : "", (err) ? [[(err) localizedDescription] UTF8String] : ""); \
		failed |= !(ok); \
	} while (0)

	if (queue == nil || a == nil || b == nil) {
		printf("drvtest: blit: no queue or buffers\n");
		return 1;
	}
	for (unsigned i = 0; i < 65536; i++) {
		pa[i] = (uint8_t)(i * 7 + 13);
	}
	memset(pb, 0, 65536);

	NSError *err = BLIT_RUN([enc copyFromBuffer:a sourceOffset:100 toBuffer:b
				  destinationOffset:300 size:1000]);
	bool ok = err == nil && pb[299] == 0 && pb[1300] == 0;
	for (unsigned i = 0; i < 1000; i++) {
		ok &= pb[300 + i] == pa[100 + i];
	}
	BLIT_CHECK("copyFromBuffer 1000 bytes, 100 -> 300", ok, err);

	err = BLIT_RUN([enc fillBuffer:b range:NSMakeRange(64, 128) value:0xa5]);
	ok = err == nil && pb[63] == 0 && pb[192] == 0;
	for (unsigned i = 64; i < 192; i++) {
		ok &= pb[i] == 0xa5;
	}
	BLIT_CHECK("fillBuffer 128 bytes of 0xa5 at 64", ok, err);

	MTLTextureDescriptor *td = [MTLTextureDescriptor
	    texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64
				     mipmapped:YES];
	td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
	id<MTLTexture> t = [dev newTextureWithDescriptor:td], u = [dev newTextureWithDescriptor:td];
	if (t == nil || u == nil) {
		printf("drvtest: blit: newTextureWithDescriptor: nil\n");
		return 1;
	}
	memset(pb, 0, 65536);
	err = BLIT_RUN(({
		[enc copyFromBuffer:a sourceOffset:8 sourceBytesPerRow:160 sourceBytesPerImage:0
			 sourceSize:MTLSizeMake(16, 8, 1) toTexture:t destinationSlice:0
		   destinationLevel:0 destinationOrigin:MTLOriginMake(4, 2, 0)];
		[enc copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(4, 2, 0)
			  sourceSize:MTLSizeMake(16, 8, 1) toBuffer:b destinationOffset:0
		  destinationBytesPerRow:64 destinationBytesPerImage:0];
	}));
	ok = err == nil;
	for (unsigned y = 0; y < 8; y++) {
		for (unsigned x = 0; x < 64; x++) {
			ok &= pb[y * 64 + x] == pa[8 + y * 160 + x];
		}
	}
	BLIT_CHECK("buffer -> texture -> buffer, 16x8, rows 160/64", ok, err);

	memset(pb, 0, 65536);
	err = BLIT_RUN(({
		[enc copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(4, 2, 0)
			  sourceSize:MTLSizeMake(16, 8, 1) toTexture:u destinationSlice:0
		    destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
		[enc copyFromTexture:u sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
			  sourceSize:MTLSizeMake(16, 8, 1) toBuffer:b destinationOffset:0
		  destinationBytesPerRow:0 destinationBytesPerImage:0];
	}));
	ok = err == nil;
	for (unsigned y = 0; y < 8; y++) {
		for (unsigned x = 0; x < 64; x++) {
			ok &= pb[y * 64 + x] == pa[8 + y * 160 + x];
		}
	}
	BLIT_CHECK("copyFromTexture 16x8, (4,2) -> (0,0)", ok, err);

	for (unsigned i = 0; i < 64 * 64; i++) {
		memcpy(pa + 4 * i, (uint8_t[]){ 200, 100, 50, 255 }, 4);
	}
	memset(pb, 0, 16);
	err = BLIT_RUN(({
		[enc copyFromBuffer:a sourceOffset:0 sourceBytesPerRow:256 sourceBytesPerImage:0
			 sourceSize:MTLSizeMake(64, 64, 1) toTexture:u destinationSlice:0
		   destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
		[enc generateMipmapsForTexture:u];
		[enc copyFromTexture:u sourceSlice:0 sourceLevel:6 sourceOrigin:MTLOriginMake(0, 0, 0)
			  sourceSize:MTLSizeMake(1, 1, 1) toBuffer:b destinationOffset:0
		  destinationBytesPerRow:0 destinationBytesPerImage:0];
	}));
	printf("drvtest: blit: level 6 of %lu reads %u %u %u %u\n",
	       (unsigned long)[u mipmapLevelCount], pb[0], pb[1], pb[2], pb[3]);
	ok = err == nil && pb[0] == 200 && pb[1] == 100 && pb[2] == 50 && pb[3] == 255;
	BLIT_CHECK("generateMipmaps: level 6 of a uniform 64x64", ok, err);

	/*
	 * RENDER PASSES, read back through the blit path just checked: a pass that clears and
	 * draws nothing, a pass that clears and draws twice (the second draw must not clear
	 * again), and a 4-sample pass resolved into a one-sample texture -- once clearing only,
	 * once drawing. Apple's clearFragment01 with the generated quad, as HKTriangle draws,
	 * reading its colour from buffer 14: vertices 0-2 are one half of the quad, 1-3 the other.
	 */
	{
		NSData *data = [NSData dataWithContentsOfFile:
		    @"/System/Library/Extensions/AppleMetalGLRenderer.bundle/default.metallib"];
		dispatch_data_t wrapped = data ? dispatch_data_create([data bytes], [data length], NULL,
		    DISPATCH_DATA_DESTRUCTOR_DEFAULT) : NULL;
		id<MTLLibrary> lib = wrapped ? [dev newLibraryWithData:wrapped error:nil] : nil;
		id<MTLFunction> frag = [lib newFunctionWithName:@"clearFragment01"];
		id<MTLBuffer> orange = [dev newBufferWithLength:256 options:0];
		id<MTLBuffer> green = [dev newBufferWithLength:256 options:0];
		float *fo = [orange contents], *fg = [green contents];

		memset(fo, 0, 256);
		memset(fg, 0, 256);
		fo[0] = 1.0f; fo[1] = 128 / 255.0f; fo[2] = 0; fo[3] = 1;
		fg[0] = 0; fg[1] = 1.0f; fg[2] = 0; fg[3] = 1;
		id<MTLRenderPipelineState> pipe1 = nil, pipe4 = nil;
		for (int samples = 1; samples <= 4 && frag != nil; samples += 3) {
			MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
			NSError *perr = nil;

			[pd setFragmentFunction:frag];
			[[[pd colorAttachments] objectAtIndexedSubscript:0]
			    setPixelFormat:MTLPixelFormatBGRA8Unorm];
			[pd setRasterSampleCount:samples];
			id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd
											    error:&perr];
			[pd release];
			printf("drvtest: blit: pipeline of %d sample%s: %s%s\n", samples,
			       samples == 1 ? "" : "s", ps ? "made" : "NOT MADE",
			       perr ? [[perr localizedDescription] UTF8String] : "");
			if (samples == 1) pipe1 = ps; else pipe4 = ps;
		}
		MTLTextureDescriptor *cd = [MTLTextureDescriptor
		    texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64
					     mipmapped:NO];
		cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		id<MTLTexture> target = [dev newTextureWithDescriptor:cd];
		id<MTLTexture> resolved = [dev newTextureWithDescriptor:cd];
		cd.textureType = MTLTextureType2DMultisample;
		cd.sampleCount = 4;
		cd.storageMode = MTLStorageModePrivate;
		id<MTLTexture> ms = [dev newTextureWithDescriptor:cd];
		printf("drvtest: blit: a 4-sample BGRA8 texture: %s\n", ms ? "made" : "NOT MADE");

		/* count texels of each BGRA colour in a 64x64 readback */
#define PASS_COUNT(tex, b_, g_, r_) ({ \
		memset(pb, 0, 64 * 64 * 4); \
		NSError *e_ = BLIT_RUN([enc copyFromTexture:(tex) sourceSlice:0 sourceLevel:0 \
		    sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:b \
		    destinationOffset:0 destinationBytesPerRow:0 destinationBytesPerImage:0]); \
		unsigned n_ = 0; \
		for (unsigned i_ = 0; e_ == nil && i_ < 64 * 64; i_++) \
			n_ += pb[4 * i_] == (b_) && pb[4 * i_ + 1] == (g_) && pb[4 * i_ + 2] == (r_); \
		n_; })
#define PASS(tex, clear, resolve, drawing) ({ \
		id<MTLCommandBuffer> cb = [queue commandBuffer]; \
		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor]; \
		MTLRenderPassColorAttachmentDescriptor *ca = [[rp colorAttachments] objectAtIndexedSubscript:0]; \
		ca.texture = (tex); ca.loadAction = MTLLoadActionClear; ca.clearColor = (clear); \
		ca.storeAction = (resolve) ? MTLStoreActionMultisampleResolve : MTLStoreActionStore; \
		ca.resolveTexture = (resolve); \
		id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp]; \
		drawing; \
		[enc endEncoding]; [cb commit]; [cb waitUntilCompleted]; \
		[cb status] == MTLCommandBufferStatusCompleted ? nil : [cb error]; })

		err = PASS(target, MTLClearColorMake(0, 0, 1, 1), nil, (void)0);
		unsigned blue = PASS_COUNT(target, 255, 0, 0);
		BLIT_CHECK("a pass that clears and draws nothing", err == nil && blue == 4096, err);

		if (pipe1 != nil) {
			err = PASS(target, MTLClearColorMake(0, 0, 1, 1), nil, ({
				[enc setRenderPipelineState:pipe1];
				[enc setFragmentBuffer:orange offset:0 atIndex:14];
				[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
				[enc setFragmentBuffer:green offset:0 atIndex:14];
				[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:1 vertexCount:3];
			}));
			unsigned o = PASS_COUNT(target, 0, 128, 255), g = PASS_COUNT(target, 0, 255, 0);
			blue = PASS_COUNT(target, 255, 0, 0);
			printf("drvtest: blit: two draws: %u orange, %u green, %u blue of 4096\n", o, g, blue);
			BLIT_CHECK("two draws in one cleared pass keep each other", err == nil && o > 1000 &&
				   g > 1000 && o + g == 4096, err);
		}
		if (ms != nil) {
			err = PASS(ms, MTLClearColorMake(1, 0, 0, 1), resolved, (void)0);
			unsigned red = PASS_COUNT(resolved, 0, 0, 255);
			printf("drvtest: blit: resolved after a clear: %u red; texel 0 reads %u %u %u %u, "
			       "texel 2080 %u %u %u %u\n", red, pb[0], pb[1], pb[2], pb[3], pb[8320],
			       pb[8321], pb[8322], pb[8323]);
			BLIT_CHECK("4 samples cleared, resolved", err == nil && red == 4096, err);
			if (pipe4 != nil) {
				err = PASS(ms, MTLClearColorMake(0, 0, 1, 1), resolved, ({
					[enc setRenderPipelineState:pipe4];
					[enc setFragmentBuffer:green offset:0 atIndex:14];
					[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
				}));
				/* a strip of four: the whole quad, so every resolved texel is green */
				unsigned g = PASS_COUNT(resolved, 0, 255, 0);
				printf("drvtest: blit: 4-sample draw resolved: %u green of 4096\n", g);
				BLIT_CHECK("4 samples drawn, resolved", err == nil && g == 4096, err);
			}
		}
#undef PASS
#undef PASS_COUNT
	}

	printf("drvtest: blit: %s\n", failed ? "FAILED" : "all ok");
	return failed;
#undef BLIT_RUN
#undef BLIT_CHECK
}

/*
 * Every fragment shader of a library, drawn, one line each.
 *
 * One shader drawn is an example; 255 of them have a reference each (docs/air.md: air_run
 * draws every fragment shader of the corpus and the pixel was compared), so drawing them all
 * turns an example into coverage. One process does the whole library because the loader keeps
 * only the first 256 KB and the last 1 MB of a boot's log: 271 processes would lose the middle
 * of their own table, and a count read off a cut log is a lower bound pretending to be a
 * number.
 *
 * The line is deliberately terse and machine-readable -- name, status, four words -- because
 * the comparison belongs to tools/air/metalcheck.py, which produces each reference by running
 * air_run rather than trusting anything written here.
 *
 * Each shader is bound the textures IT declares, asked of the compiler library through
 * rwa_module_textures. The run before this one bound a single kind and format for the whole
 * library and 13 of Apple's 271 fragment shaders were refused by name for declaring a
 * texture3d, a texture2d<int> or a multisample texture: a true sentence about this harness
 * and nothing about those shaders. A slot no shader named keeps the single binding, so the
 * 256 shaders that declare no texture are bound what they were bound before and their pixels
 * stay comparable with that run's.
 */
/*
 * ONE NUMBER PER ATTACHMENT, for the per-shader result line of --draw-all: FNV-1a over the
 * whole 4x4 picture exactly as -getBytes: returns it, rows tightly packed -- the digest
 * --draw prints for its first step, over every attachment instead of one. Nothing is
 * decoded first, so a difference anywhere in any channel changes it, and it is the same
 * code whichever back end drew, which is what lets two logs of one boot be compared
 * shader by shader on the host.
 */
/*
 * M5c: the digest of a shader that writes fewer channels than the attachment has, over the channel it
 * writes. texturedFragment2DStencil and ...Multisample return one ushort (of the corpus's 271, the only
 * two: `air.render_target` of every function, by the native air2nir), and this sweep draws everything
 * into RGBA32Uint, so G, B and A held whatever a GPU leaves, moved from boot to boot, and were carried
 * as "undefined channels aside" through every comparison since M3b. That shape is this harness's and
 * not a Metal one: real Metal refuses the pipeline ("fragment shader color output does not have enough
 * components for the pixel format", on an M1), so it has no value for those channels to match. A
 * stencil READ has one, (s, 0, 0, 1), on real Metal and on vk: view-x32s8.
 */
static bool target_digest_first_word_only;

static unsigned long long
target_digest(id<MTLTexture> target)
{
	unsigned tb = texel_bytes_of(draw_target_format);
	unsigned char grid[4 * 4 * 16];
	unsigned long long digest = 0xcbf29ce484222325ull;

	memset(grid, 0, sizeof(grid));
	[target getBytes:grid bytesPerRow:4 * tb fromRegion:MTLRegionMake2D(0, 0, 4, 4)
	      mipmapLevel:0];
	for (unsigned i = 0; i < 4 * 4 * tb; i++) {
		if (target_digest_first_word_only && i % tb >= 4) {
			continue;
		}
		digest = (digest ^ grid[i]) * 0x100000001b3ull;
	}
	return digest;
}

/*
 * The same FNV-1a over a DEPTH attachment (M3c-2): the sixteen floats the driver's own aspect
 * reader gives back (hkReadDepth -> rwa_depth_read, which decodes with Mesa's unpackers on
 * both back ends, so Depth32Float comes back as the bits that were stored), row-major, as
 * bytes. -getBytes: is no use here -- both compiler libraries refuse a depth texture through
 * it by name -- and the depth at (0, 0) is handed back as well for the human-readable line.
 * False when the driver is older than this test or the read is refused.
 */
static bool
depth_digest(id<MTLTexture> zt, unsigned long long *digest, float *first)
{
	SEL sel = sel_getUid("hkReadDepth:stencil:fromRegion:");
	float map[4 * 4];

	*digest = 0xcbf29ce484222325ull;
	*first = 0.0f;
	if (![zt respondsToSelector:sel]) {
		return false;
	}
	memset(map, 0, sizeof(map));
	if (!((BOOL (*)(id, SEL, float *, uint8_t *, MTLRegion))objc_msgSend)(
		zt, sel, map, NULL, MTLRegionMake2D(0, 0, 4, 4))) {
		return false;
	}
	const unsigned char *b = (const unsigned char *)map;

	for (unsigned i = 0; i < sizeof(map); i++) {
		*digest = (*digest ^ b[i]) * 0x100000001b3ull;
	}
	*first = map[0];
	return true;
}

/*
 * WHAT APPLE'S SIX MULTISAMPLE SHADERS MUST DRAW HERE (M3c-3), computed from the clear that
 * filled their texture and from nothing either back end said -- so a vk line can be judged
 * where Gallium has no line to compare it with (the depth one: KNOWN_ISSUES K28).
 *
 * Texture 0 holds ms_seed(0) = (100, 200, 300, 400) in every sample of every texel. What
 * each shader does with it, read off its own AIR (llvm-dis of AppleMetalGLRenderer's
 * default.metallib; every coordinate is the generated stage's constant 0.375, floored to
 * texel (0, 0)):
 *
 *   texturedFragment2DFloatMultisamplePoint   sum of read(coord, i) over get_num_samples()
 *                                             samples, divided by the count: the seed, as
 *                                             four floats, into render targets 0-3
 *   texturedFragment2DFloatMultisampleLinear  the same sum at texels (0,0) (0,1) (1,0)
 *                                             (1,1), mixed by fract(0.375) and divided by
 *                                             the count: the seed again -- a mix of four
 *                                             equal values is that value, exactly, and so
 *                                             is 4 x 100 / 4 in floats
 *   texturedFragment2DIntMultisample          the integer sum (sdiv by the count): the seed
 *   texturedFragment2DUIntMultisample         (udiv): the seed. 4 x 400 does not wrap
 *   texturedFragment2DStencilMultisample      read(coord, 0) of a texture2d_ms<ushort>,
 *                                             channel 0 as an i16 into target 0: 100 in R,
 *                                             G/B/A of target 0 not defined by the shader
 *                                             (its one-sample twin leaves them holding
 *                                             whatever each GPU leaves, M3b)
 *   texturedFragment2DDepthMultisample        read_depth_2d_ms(coord, 0) stored to
 *                                             [[depth]], no colour: the depth attachment
 *                                             holds DEPTH2D_SEED, the clear of its
 *                                             depth2d_ms texture, in all sixteen texels
 *
 * In this harness's RGBA32Uint attachments a float lands as its bits (100.0 is 1120403456,
 * the convention texturedFragment2DFloat's 1120534528 = 101.0 already shows) and an
 * integer as its value. The four averaging shaders write render targets 0-3; target 3 has
 * no attachment here, and Metal drops that write. A target a shader does not write holds
 * the pass's clear (--clear, 0 unless given), as its bits.
 */
enum ms_kind { MS_FLOAT, MS_INT, MS_UINT, MS_LOW16, MS_DEPTH };

static const struct {
	const char *name;
	enum ms_kind kind;
	unsigned targets;        /* which of t0..t2 the shader writes */
} ms_expect[] = {
	{ "texturedFragment2DFloatMultisamplePoint",  MS_FLOAT, 0x7 },
	{ "texturedFragment2DFloatMultisampleLinear", MS_FLOAT, 0x7 },
	{ "texturedFragment2DIntMultisample",         MS_INT,   0x7 },
	{ "texturedFragment2DUIntMultisample",        MS_UINT,  0x7 },
	{ "texturedFragment2DStencilMultisample",     MS_LOW16, 0x1 },
	{ "texturedFragment2DDepthMultisample",       MS_DEPTH, 0x0 },
};

/* FNV-1a over sixteen copies of one 16-byte texel: the digest target_digest would print for
 * a 4x4 RGBA32Uint attachment every pixel of which is `w`. */
static unsigned long long
uniform_digest(const unsigned w[4])
{
	unsigned char grid[4 * 4 * 16];

	for (unsigned i = 0; i < 16; i++) {
		memcpy(grid + 16 * i, w, 16);
	}
	return fnv1a(grid, sizeof(grid));
}

/*
 * The check: -1 when `name` is not one of the six (or the attachments are not RGBA32Uint,
 * which is what the table is computed for), 1 when every defined channel of every pixel of
 * every attachment -- and the depth, for the depth shader -- is what it must be, 0 when
 * anything is not. The line it prints says what was expected, so a reader needs no table.
 */
static int
ms_check(const char *name, id<MTLTexture> *targets, bool zread, unsigned long long zdigest)
{
	int e = -1;

	for (unsigned i = 0; i < sizeof(ms_expect) / sizeof(ms_expect[0]); i++) {
		if (strcmp(name, ms_expect[i].name) == 0) {
			e = (int)i;
		}
	}
	if (e < 0) {
		return -1;
	}
	if (draw_target_format != MTLPixelFormatRGBA32Uint) {
		printf("drvtest: drawall %-34s ms: no expectation for attachments of pixel format "
		       "%lu (it is computed for RGBA32Uint)\n", name,
		       (unsigned long)draw_target_format);
		return -1;
	}
	double seed[4];
	float clearf = (float)draw_clear;
	unsigned clearbits;

	ms_seed(0, seed);
	memcpy(&clearbits, &clearf, sizeof(clearbits));
	printf("drvtest: drawall %-34s ms: texture 0 is %u samples of (%g %g %g %g) in every "
	       "texel; it must draw", name, MS_SAMPLES, seed[0], seed[1], seed[2], seed[3]);

	bool all = true;

	for (unsigned t = 0; t < 3; t++) {
		unsigned want[4], care = 0xf;

		for (unsigned c = 0; c < 4; c++) {
			want[c] = clearbits;
		}
		if (ms_expect[e].kind == MS_DEPTH) {
			care = 0;           /* its colour output is undefined (undef in its AIR) */
		} else if (ms_expect[e].targets & (1u << t)) {
			for (unsigned c = 0; c < 4; c++) {
				float f = (float)seed[c];

				switch (ms_expect[e].kind) {
				case MS_FLOAT:
					memcpy(&want[c], &f, sizeof(f));
					break;
				case MS_INT:
					want[c] = (unsigned)(int)seed[c];
					break;
				case MS_UINT:
					want[c] = (unsigned)seed[c];
					break;
				default:        /* MS_LOW16: R alone */
					want[c] = c == 0 ? ((unsigned)seed[0] & 0xffffu) : 0;
					break;
				}
			}
			if (ms_expect[e].kind == MS_LOW16) {
				care = 0x1;
			}
		}
		unsigned px[4 * 4 * 4];
		unsigned good = 0;

		memset(px, 0, sizeof(px));
		[targets[t] getBytes:px bytesPerRow:4 * 16 fromRegion:MTLRegionMake2D(0, 0, 4, 4)
			 mipmapLevel:0];
		for (unsigned p = 0; p < 16; p++) {
			bool same = true;

			for (unsigned c = 0; c < 4; c++) {
				if ((care & (1u << c)) && px[4 * p + c] != want[c]) {
					same = false;
				}
			}
			good += same;
		}
		all = all && good == 16;
		if (care == 0) {
			printf(" t%u anything", t);
		} else if (care == 0xf) {
			printf(" t%u %u %u %u %u (digest %016llx)", t, want[0], want[1], want[2],
			       want[3], uniform_digest(want));
		} else {
			printf(" t%u R %u, GBA undefined", t, want[0]);
		}
		printf(" [%u/16]", good);
	}
	if (ms_expect[e].kind == MS_DEPTH) {
		float d[16];

		for (unsigned i = 0; i < 16; i++) {
			d[i] = (float)DEPTH2D_SEED;
		}
		unsigned long long want = fnv1a((const unsigned char *)d, sizeof(d));
		bool same = zread && zdigest == want;

		printf(" d %.9g in all 16 (digest %016llx) [%s]", DEPTH2D_SEED, want,
		       !zread ? "unreadable" : same ? "16/16" : "differs");
		all = all && same;
	}
	printf(" -> %s\n", all ? "as computed" : "NOT as computed");
	return all ? 1 : 0;
}

static void
draw_every(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue,
	   MTLRenderPassDescriptor *rp, id<MTLBuffer> uniform, id<MTLTexture> *dtex,
	   id<MTLSamplerState> dsamp, id<MTLTexture> *targets)
{
	/*
	 * THE RESULT LINE, one per shader and in one shape whatever happened, so the two logs
	 * of a boot that ran this once per back end (--backend) can be joined on the name:
	 *
	 *     drvtest: drawall-result BACKEND NAME drew t0 HEX t1 HEX t2 HEX
	 *     drvtest: drawall-result BACKEND NAME no-texture REASON
	 *     drvtest: drawall-result BACKEND NAME no-pipeline REASON
	 *     drvtest: drawall-result BACKEND NAME refused-draw REASON
	 *
	 * REASON is the driver's own sentence (the compiler library's, through the NSError),
	 * which is what names the rwa_* call that refused. The older lines stay as they were,
	 * because tools/air/metalcheck.py reads them; this one is for the cross-back-end table.
	 *
	 * A SHADER THAT STORES [[depth]] (M3c-2) gets a depth attachment, and its `drew` line
	 * one more field at the END, so the line still joins on the name and t0..t2 mean what
	 * they always meant:
	 *
	 *     drvtest: drawall-result BACKEND NAME drew t0 HEX t1 HEX t2 HEX d HEX
	 *
	 * where d is FNV-1a over the depth attachment's sixteen floats (depth_digest), or
	 * `d unreadable` when the driver would not give them back. How the sweep knows: it
	 * asks for the pipeline as it always did, with three colour attachments and no depth
	 * one, and BOTH compiler libraries refuse a shader that stores to [[depth]] there with
	 * a sentence containing "stores to [[depth]]" (air_metal.c and air_vk.c say so in as
	 * many words). Only after that refusal is the same descriptor asked again with a
	 * MTLPixelFormatDepth32Float attachment -- so a shader that does not store depth is
	 * asked exactly what it was asked before, and the 269 lines without a `d` are the
	 * lines of the runs before this. The pass then clears the depth to 1.0, binds a state
	 * of MTLCompareFunctionAlways with depth writes on (Metal's default state writes no
	 * depth, so without it the store would go nowhere), and the shader's own textures are
	 * seeded in (0, 1) the way --draw-depth seeds them, because the shader stores what it
	 * samples and air_run's 100.0 would land outside every depth there is. In Apple's
	 * library two shaders take this road: texturedFragment2DDepth (a depth2d texture,
	 * DEPTH2D_SEED everywhere, so d is sixteen 0.0859375s) and texturedFragment3DDepth (a
	 * texture3d of RGBA32Float, texel (1, 1, 1) = 0.3359375).
	 *
	 * [M3c-3: three, with texturedFragment2DDepthMultisample (a depth2d_ms texture of
	 * MS_SAMPLES, DEPTH2D_SEED in every sample). And ON GALLIUM NONE OF THEM IS DRAWN unless
	 * --gallium-depth-stores is given: a fragment [[depth]] store times that back end's GPU
	 * out (docs/KNOWN_ISSUES.md K28, run 030433, four core recoveries in one boot), so the
	 * line is a refusal by name there, in the same shape:
	 *
	 *     drvtest: drawall-result gallium NAME refused-draw not drawn: it stores [[depth]] ...
	 *
	 * A SHADER THAT READS A MULTISAMPLE TEXTURE gets one more field at the END of its
	 * `drew` line, after any `d`, which says whether what it drew is what ms_check computed
	 * from the clear that filled its texture:
	 *
	 *     drvtest: drawall-result BACKEND NAME drew t0 HEX t1 HEX t2 HEX [d HEX] ms match
	 *     drvtest: drawall-result BACKEND NAME drew t0 HEX t1 HEX t2 HEX [d HEX] ms differs
	 *
	 * and the human line before it says what each attachment had to hold, with the digest
	 * that makes, so the t0..t2 of a back end with no partner line can still be judged.]
	 */
	const char *backend = backend_is_vk() ? "vk" : "gallium";
	unsigned drew = 0, refused = 0, nopipe = 0, notex = 0, asked = 0;
	/* The depth attachment and the writing state, made the first time a shader needs them
	 * and released at the end, so a sweep that meets no depth shader makes neither. */
	id<MTLTexture> zt = nil;
	id<MTLDepthStencilState> zwrite = nil;
	unsigned depth_pipes = 0;

	unsigned seen = 0;

	/*
	 * WHAT THE VULKAN BACK END HOLDS WHILE THE SWEEP RUNS, every 50 shaders attempted and
	 * once at the end, with the highest command-buffer and fence counts seen at those
	 * points. Run 020900 counted 208 command buffers and 208 fences in use after its 211
	 * shaders, before releasing (and run 020036 54 after 60): each Metal command buffer
	 * gave its rwa fence back only in -dealloc, and
	 * [queue commandBuffer] here is autoreleased with no pool per shader, so nothing was
	 * given back until the sweep ended. hkmetal.m now gives the fence back when the
	 * command buffer completes; these lines are how a run shows whether it does. There is
	 * deliberately NO @autoreleasepool in this loop: one would hide the very thing being
	 * measured. Nothing is printed on Gallium (print_vk_counts_into says so by returning
	 * false).
	 */
	unsigned vk_most_cmd = 0, vk_most_fence = 0;
	bool vk_counted = false;

	for (NSString *name in [library functionNames]) {
		id<MTLFunction> fn = [library newFunctionWithName:name];
		NSError *err = nil;

		if (fn == nil || [fn functionType] != MTLFunctionTypeFragment) {
			continue;
		}
		/* --draw-one NAME (M3c-3): that function and no other, wherever it stands in
		 * -functionNames' order; the index is still counted, so the summary says how
		 * many there were */
		if (draw_one != NULL) {
			seen++;
			if (strcmp([name UTF8String], draw_one) != 0) {
				continue;
			}
		} else if (seen++ < draw_from || drew + refused + nopipe + notex >= draw_count) {
			continue;
		}
		/* M4: the phases of a timed sweep (cost_phase; a no-op outside --quiet's bracket):
		 * setup from here, the pipeline, the draw from -commandBuffer to its wait, the
		 * readback after it */
		cost_phase(COST_SETUP);
		/* Every way out of an attempt below adds one to exactly one of the four
		 * counts, so their sum here is the shaders attempted so far, and a sum that
		 * is a multiple of 50 is the point right after the 50th, 100th... finished. */
		unsigned attempted = drew + refused + nopipe + notex;

		if (attempted > 0 && attempted % 50 == 0) {
			char when[64];
			unsigned n[4];

			snprintf(when, sizeof(when), "during --draw-all after %u shaders",
				 attempted);
			if (print_vk_counts_into(when, n)) {
				vk_counted = true;
				vk_most_cmd = n[0] > vk_most_cmd ? n[0] : vk_most_cmd;
				vk_most_fence = n[1] > vk_most_fence ? n[1] : vk_most_fence;
			}
		}
		/*
		 * What THIS shader declares, made for it, before anything is bound. The slots
		 * it does not name keep the sweep's own dtex[], so the 256 shaders that
		 * declare no texture at all are bound exactly what they were bound before and
		 * their numbers stay comparable with the previous run's.
		 */
		id<MTLTexture> own[RW_TEX_SLOTS] = { nil };
		unsigned ndecl = 0;
		char whynot[256] = "";

		if (use_decl && !textures_for_shader(dev, fn, own, RW_TEX_SLOTS, &ndecl, whynot,
						     sizeof(whynot))) {
			notex++;
			printf("drvtest: drawall %-34s no texture: %s\n", [name UTF8String],
			       whynot);
			printf("drvtest: drawall-result %s %s no-texture %s\n", backend,
			       [name UTF8String], whynot);
			release_slots(own, RW_TEX_SLOTS);
			continue;
		}
		if (ndecl > 0) {
			asked++;
		}
		/* The name BEFORE the work and not only in the row after it.
		 *
		 * The first sweep with per-shader textures stopped dead at its third shader
		 * (logs/070755-run.log): two multisample refusals, then one line from the
		 * driver about a generated vertex stage, then nothing for the rest of the
		 * boot. Everything about which shader that was had to be inferred, and
		 * -[_MTLLibrary functionNames] is not in container order, so it could not be
		 * inferred from the index either. A line here costs one line per shader and
		 * makes the next hang name itself.
		 */
		printf("drvtest: drawall %-34s about to draw, %u declared texture%s\n",
		       [name UTF8String], ndecl, ndecl == 1 ? "" : "s");
		fflush(stdout);
		cost_phase(COST_PIPELINE);
		MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];

		[pd setFragmentFunction:fn];
		[pd setVertexFunction:nil];
		for (int i = 0; i < 3; i++) {
			[[[pd colorAttachments] objectAtIndexedSubscript:i]
			    setPixelFormat:draw_target_format];
		}
		id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd
										   error:&err];
		/* The [[depth]] retry (the comment at the top of this function): that one
		 * refusal, and nothing else, asks again with a depth attachment. */
		bool with_depth = false;

		if (ps == nil && err != nil &&
		    strstr([[err localizedDescription] UTF8String], "stores to [[depth]]") != NULL) {
			printf("drvtest: drawall %-34s stores to [[depth]]: asked again with a "
			       "MTLPixelFormatDepth32Float attachment\n", [name UTF8String]);
			[pd setDepthAttachmentPixelFormat:MTLPixelFormatDepth32Float];
			err = nil;
			ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
			with_depth = ps != nil;
		}
		[pd release];
		cost_phase(COST_SETUP);
		/* K28 (the comment at the top of this function): not drawn on Gallium, by name */
		if (with_depth && !backend_is_vk() && !gallium_depth_stores) {
			refused++;
			printf("drvtest: drawall %-34s not drawn: it stores [[depth]], and on the "
			       "Gallium back end that times the GPU out (KNOWN_ISSUES K28); "
			       "--gallium-depth-stores draws it anyway\n", [name UTF8String]);
			printf("drvtest: drawall-result %s %s refused-draw not drawn: it stores "
			       "[[depth]], which times the Gallium back end's GPU out (KNOWN_ISSUES "
			       "K28)\n", backend, [name UTF8String]);
			[ps release];
			release_slots(own, RW_TEX_SLOTS);
			continue;
		}
		if (with_depth && (zt == nil || zwrite == nil)) {
			MTLTextureDescriptor *ztd = [MTLTextureDescriptor
			    texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
							 width:4 height:4 mipmapped:NO];
			MTLDepthStencilDescriptor *dsd = [[MTLDepthStencilDescriptor alloc] init];

			[ztd setUsage:MTLTextureUsageRenderTarget];
			[dsd setDepthCompareFunction:MTLCompareFunctionAlways];
			[dsd setDepthWriteEnabled:YES];
			if (zt == nil) {
				zt = [dev newTextureWithDescriptor:ztd];
			}
			if (zwrite == nil) {
				zwrite = [dev newDepthStencilStateWithDescriptor:dsd];
			}
			[dsd release];
			if (zt == nil || zwrite == nil) {
				refused++;
				printf("drvtest: drawall-result %s %s refused-draw no %s could be made "
				       "for its [[depth]]\n", backend, [name UTF8String],
				       zt == nil ? "Depth32Float attachment" : "depth/stencil state");
				[ps release];
				release_slots(own, RW_TEX_SLOTS);
				continue;
			}
		}
		if (with_depth) {
			/* what it samples is what it stores: seeded in (0, 1), as --draw-depth
			 * seeds it; a depth2d slot is already DEPTH2D_SEED everywhere */
			depth_pipes++;
			for (unsigned k = 0; k < RW_TEX_SLOTS; k++) {
				if (own[k] != nil &&
				    [own[k] pixelFormat] != MTLPixelFormatDepth32Float) {
					fill_depth_texture(own[k]);
				}
			}
		}
		if (ps == nil) {
			nopipe++;
			printf("drvtest: drawall %-34s no pipeline: %s\n", [name UTF8String],
			       err != nil ? [[err localizedDescription] UTF8String] : "(no reason)");
			printf("drvtest: drawall-result %s %s no-pipeline %s\n", backend,
			       [name UTF8String],
			       err != nil ? [[err localizedDescription] UTF8String] : "(no reason)");
			release_slots(own, RW_TEX_SLOTS);
			continue;
		}
		cost_phase(COST_WORK);
		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		/* The depth attachment is in the pass only for a pipeline made with one, and
		 * taken out again as soon as the encoder has read the descriptor: the caller's
		 * rp is shared with every other shader of the sweep. */
		MTLRenderPassDepthAttachmentDescriptor *za = [rp depthAttachment];

		if (with_depth) {
			[za setTexture:zt];
			[za setLoadAction:MTLLoadActionClear];
			[za setClearDepth:1.0];
			[za setStoreAction:MTLStoreActionStore];
		}
		id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];

		if (with_depth) {
			[za setTexture:nil];
		}
		if (enc == nil) {
			refused++;
			printf("drvtest: drawall-result %s %s refused-draw no render command encoder\n",
			       backend, [name UTF8String]);
			[ps release];
			release_slots(own, RW_TEX_SLOTS);
			continue;
		}
		[enc setRenderPipelineState:ps];
		if (with_depth) {
			[enc setDepthStencilState:zwrite];
		}
		for (int i = 0; i < 16; i++) {
			[enc setFragmentBuffer:uniform offset:0 atIndex:i];
			/* this shader's own texture where it declared one, and the sweep's
			 * single binding where it did not: a slot nobody named is a slot
			 * nothing reads, and leaving the old binding there keeps the 256
			 * shaders that declare none bound exactly as they were */
			if (i < RW_TEX_SLOTS && own[i] != nil) {
				[enc setFragmentTexture:own[i] atIndex:i];
			} else if (i < 4 && dtex[i] != nil) {
				[enc setFragmentTexture:dtex[i] atIndex:i];
			}
			if (dsamp != nil) {
				[enc setFragmentSamplerState:dsamp atIndex:i];
			}
		}
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		[enc endEncoding];
		[cmd commit];
		[cmd waitUntilCompleted];
		cost_phase(COST_READBACK);

		if ([cmd status] != RW_CB_COMPLETED) {
			refused++;
			printf("drvtest: drawall %-34s status %lu %s\n", [name UTF8String],
			       (unsigned long)[cmd status],
			       [cmd error] != nil ?
			       [[[cmd error] localizedDescription] UTF8String] : "");
			printf("drvtest: drawall-result %s %s refused-draw status %lu %s\n", backend,
			       [name UTF8String], (unsigned long)[cmd status],
			       [cmd error] != nil ?
			       [[[cmd error] localizedDescription] UTF8String] : "(no reason)");
			[ps release];
			release_slots(own, RW_TEX_SLOTS);
			continue;
		}
		/* All three attachments, because each shader writes the targets IT declares:
		 * some write target 0, some 1, some 2, and reading only one of them made 50 of
		 * 64 shaders uncomparable with "air_run printed no pixel for render target 2".
		 * Which target to compare is then the reference's business, not the harness's. */
		drew++;
		printf("drvtest: drawall %-34s status %d", [name UTF8String], RW_CB_COMPLETED);
		for (int t = 0; t < 3; t++) {
			unsigned pixel[4] = { 0 };

			[targets[t] getBytes:pixel
				bytesPerRow:4 * 4 * 4
				 fromRegion:MTLRegionMake2D(0, 0, 1, 1)
				mipmapLevel:0];
			printf(" t%d %u %u %u %u", t, pixel[0], pixel[1], pixel[2], pixel[3]);
		}
		/* and the depth, for a shader drawn with a depth attachment (read once, here,
		 * and printed in both lines) */
		unsigned long long zdigest = 0;
		float zfirst = 0.0f;
		bool zread = with_depth && depth_digest(zt, &zdigest, &zfirst);

		if (with_depth) {
			if (zread) {
				printf(" depth %.9g", zfirst);
			} else {
				printf(" depth unreadable");
			}
		}
		/* What was bound, on the same line as the pixel: a row of this table is only
		 * a measurement if what it was measured against is in it, and metalcheck.py
		 * has to know which shape air_run should hold to produce the reference. */
		/* (and the sample count of a multisample one, M3c-3: "0=4:125x4") */
		bool any_ms = false;

		if (ndecl > 0) {
			printf(" tex");
			for (unsigned k = 0; k < RW_TEX_SLOTS; k++) {
				if (own[k] != nil) {
					printf(" %u=%lu:%lu", k,
					       (unsigned long)[own[k] textureType],
					       (unsigned long)[own[k] pixelFormat]);
					if ([own[k] sampleCount] > 1) {
						printf("x%lu", (unsigned long)[own[k] sampleCount]);
						any_ms = true;
					}
				}
			}
		}
		printf("\n");
		/* what a multisample shader had to draw, computed (ms_check), before the
		 * result line that carries the verdict */
		int ms = any_ms ? ms_check([name UTF8String], targets, zread, zdigest) : -1;

		target_digest_first_word_only = [name hasPrefix:@"texturedFragment2DStencil"];
		printf("drvtest: drawall-result %s %s drew", backend, [name UTF8String]);
		for (int t = 0; t < 3; t++) {
			printf(" t%d %016llx", t, target_digest(targets[t]));
		}
		if (with_depth) {
			if (zread) {
				printf(" d %016llx", zdigest);
			} else {
				printf(" d unreadable");
			}
		}
		if (ms >= 0) {
			printf(" ms %s", ms ? "match" : "differs");
		}
		printf("\n");
		[ps release];
		release_slots(own, RW_TEX_SLOTS);
	}
	[zt release];
	[zwrite release];
	/* --draw-one with a name the library has no fragment function of: said in the result
	 * line's own shape, so a job line that misspelt it is a line and not a silence */
	if (draw_one != NULL && drew + refused + nopipe + notex == 0) {
		nopipe++;
		printf("drvtest: drawall-result %s %s no-pipeline the library has no fragment "
		       "function of that name\n", backend, draw_one);
	}
	{
		char when[64];
		unsigned n[4];

		snprintf(when, sizeof(when), "during --draw-all after %u shaders (the end)",
			 drew + refused + nopipe + notex);
		if (print_vk_counts_into(when, n)) {
			vk_counted = true;
			vk_most_cmd = n[0] > vk_most_cmd ? n[0] : vk_most_cmd;
			vk_most_fence = n[1] > vk_most_fence ? n[1] : vk_most_fence;
		}
		if (vk_counted) {
			printf("drvtest: vk counts during --draw-all: at most %u command buffers "
			       "and %u fences in use at those points (every 50 shaders and the "
			       "end)\n", vk_most_cmd, vk_most_fence);
		}
	}
	/* for --quiet's cost line (M4, W2) */
	draw_every_attempted = drew + refused + nopipe + notex;
	draw_every_drew = drew;
	/* before the two summaries, which stay the last lines of a sweep (metalcheck.py reads
	 * the second as the end) */
	printf("drvtest: drawall-depth %s %u stored to [[depth]] and were given a Depth32Float "
	       "attachment for it\n", backend, depth_pipes);
	printf("drvtest: drawall-summary %s drew %u no-pipeline %u refused-draw %u "
	       "no-texture %u (of %u fragment shaders from index %u)\n", backend, drew, nopipe,
	       refused, notex, seen, draw_from);
	printf("drvtest: drawall: %u drew, %u refused at the draw, %u got no pipeline, "
	       "%u could not be given the textures they declare (of the %u fragment shaders "
	       "from index %u; %u declared a texture and were bound what they declared)\n",
	       drew, refused, nopipe, notex, seen, draw_from, asked);
	fflush(stdout);
}

/*
 * --draw-msaa [N]: A MULTISAMPLE PASS AND ITS RESOLVE, ON THE HARDWARE (M3c-3).
 *
 * The fragment function (--fragment, clearFragment20 unless given) drawn over the generated
 * quad into THREE attachments of N samples each (4 unless given) and of --rtfmt's format
 * (RGBA32Uint unless given), cleared to --clear first, each with
 * MTLStoreActionMultisampleResolve and a one-sample resolveTexture -- so what is read back
 * is the resolve, which is the only way a multisample texture reaches the processor. Two
 * passes, because they measure two different things:
 *
 *   full      the strip of four: every sample of every pixel covered, so every sample holds
 *             the shader's colour and so does any resolve of them, averaged or not.
 *             Attachment 2's resolve must be that colour in all 16 pixels, and attachments
 *             0 and 1 (which these shaders do not write) the clear in all 32.
 *   triangle  vertices 0-2 only: the half of the quad below the anti-diagonal, whose edge
 *             runs through the CENTRES of the four pixels on that diagonal. At one sample
 *             each of those is in or out; at N samples it is cut, half its samples on each
 *             side under Vulkan's standard sample positions (2x: offsets of +-0.25 on both
 *             axes; 4x: x+y offsets of -0.5, -0.25, 0.25, 0.5). So an AVERAGE resolve makes
 *             those four pixels exactly half way between the colour and the clear, and a
 *             SAMPLE_ZERO resolve makes each of them one or the other. A "between" count of
 *             4 is the evidence that there were N samples and that they were averaged; 0
 *             with a float format would say the pass was single-sampled.
 *
 * WHAT THE COLOUR MUST BE is computed here and not taken from either back end, for the three
 * of Apple's clear shaders that write render target 2 alone, each returning one member of the
 * ClearFragmentInput at [[buffer(14)]], which this test fills (i * 7 + 13) & 0xff byte by byte:
 *
 *   clearFragment10   float4 f  at bytes 0-15:  572199949 1043804201 1515408453 1987012705
 *                     (as floats 2.1017e-18 0.178894654 1.48687698e16 1.21394141e33,
 *                     all normal, so an exact average of equal samples gives them back)
 *   clearFragment20   int4 i    at bytes 16-31: 2458616957 2930221209 3401825461 3873429713
 *   clearFragment30   uint4 ui  at bytes 32-47: 50066669 504827913 976432165 1448036417
 *
 * (In a 4x4 attachment of one such colour the digest is fc5fe79f32951325 for
 * clearFragment20 -- M3b's own number for it -- and d759cde1d8a7ef25 for clearFragment10.)
 *
 * (clearFragment20's four words are docs/metal.md 10.22's, softpipe's and both back ends'
 * since M3a.) The resolve mode is the one the Vulkan back end uses for the format, which is
 * Vulkan's requirement: SAMPLE_ZERO for an integer format, AVERAGE for a floating-point or
 * normalised one. So `--rtfmt rgba32uint --fragment clearFragment20` (or rgba32sint) is the
 * SAMPLE_ZERO check and `--rtfmt rgba32float --fragment clearFragment10` the AVERAGE one,
 * whose average of N equal floats must be the float itself; `ulps` says by how many units in
 * the last place it is not, which is 0 for an exact resolve.
 *
 * And the processor's way into a multisample texture is tried and must be refused, by the
 * compiler library on both back ends (its sentence is the driver's line just above this
 * program's): -getBytes: must leave the caller's bytes as they were.
 *
 * THE RESULT LINE, in one shape whatever happened, joined across back ends on the name:
 *
 *     drvtest: msaa-result BACKEND FUNCTION FORMAT SAMPLES MODE full G/16 px W W W W
 *              expect W W W W|none ulps U others O/32 tri value V clear C between B half H
 *              digest HEX
 *     drvtest: msaa-result BACKEND FUNCTION FORMAT SAMPLES MODE no-pipeline|no-texture|
 *              refused-draw REASON
 */
static int
clear_member_offset(const char *shader)
{
	return strcmp(shader, "clearFragment10") == 0 ? 0 :
	       strcmp(shader, "clearFragment20") == 0 ? 16 :
	       strcmp(shader, "clearFragment30") == 0 ? 32 : -1;
}

static bool
format_is_integer(MTLPixelFormat f)
{
	return f == MTLPixelFormatRGBA32Uint || f == MTLPixelFormatRGBA32Sint ||
	       f == MTLPixelFormatRGBA16Uint || f == MTLPixelFormatRGBA16Sint ||
	       f == MTLPixelFormatR32Uint;
}

static void
msaa_path(id<MTLDevice> dev, const char *metallib)
{
	const char *backend = backend_is_vk() ? "vk" : "gallium";
	const char *shader = fragment_function;
	const unsigned n = msaa_samples;
	const MTLPixelFormat fmt = draw_target_format;
	const unsigned tb = texel_bytes_of(fmt);
	const char *mode = format_is_integer(fmt) ? "sample-zero" : "average";
	NSError *err = nil;

#define MSAA_STOP(what, reason) \
	printf("drvtest: msaa-result %s %s %lu %u %s %s %s\n", backend, shader, \
	       (unsigned long)fmt, n, mode, (what), (reason))

	if (n < 2) {
		printf("drvtest: msaa: --draw-msaa %u: a multisample pass has two samples or more "
		       "(Metal's own rule for a multisample texture)\n", n);
		MSAA_STOP("no-texture", "a sample count below two");
		return;
	}
	NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:metallib]];
	dispatch_data_t dd = data != nil ? dispatch_data_create([data bytes], [data length], NULL,
								DISPATCH_DATA_DESTRUCTOR_DEFAULT) : NULL;
	id<MTLLibrary> library = dd != NULL ? [dev newLibraryWithData:dd error:&err] : nil;
	id<MTLFunction> fragment = library != nil ?
	    [library newFunctionWithName:[NSString stringWithUTF8String:shader]] : nil;

	if (fragment == nil) {
		printf("drvtest: msaa: no %s in %s\n", shader, metallib);
		MSAA_STOP("no-pipeline", "no such fragment function");
		return;
	}
	printf("drvtest: msaa: %s %s into three %u-sample attachments of pixel format %lu, each "
	       "resolved into a one-sample texture (%s, what Vulkan requires of that format)\n",
	       metallib, shader, n, (unsigned long)fmt, mode);

	MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];

	[pd setFragmentFunction:fragment];
	[pd setVertexFunction:nil];
	for (int i = 0; i < 3; i++) {
		[[[pd colorAttachments] objectAtIndexedSubscript:i] setPixelFormat:fmt];
	}
	/* rasterSampleCount and sampleCount are one property of Metal's under two names; the
	 * driver reads it as -sampleCount (hkmetal.m) */
	[pd setRasterSampleCount:n];
	id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];

	[pd release];
	printf("drvtest: msaa: the %u-sample pipeline: %s%s%s\n", n, ps != nil ? "made" : "NOT MADE",
	       err != nil ? ": " : "", err != nil ? [[err localizedDescription] UTF8String] : "");
	if (ps == nil) {
		MSAA_STOP("no-pipeline", err != nil ? [[err localizedDescription] UTF8String]
						     : "(no reason)");
		return;
	}

	MTLTextureDescriptor *md = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
										 width:4
										height:4
									     mipmapped:NO];
	MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
										 width:4
										height:4
									     mipmapped:NO];

	[md setTextureType:MTLTextureType2DMultisample];
	[md setSampleCount:n];
	[md setStorageMode:MTLStorageModePrivate];
	[md setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];
	[od setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];

	id<MTLTexture> ms[3] = { nil, nil, nil }, one[3] = { nil, nil, nil };
	id<MTLBuffer> uniform = nil;
	id<MTLCommandQueue> queue = nil;
	bool made = true;

	for (int i = 0; i < 3; i++) {
		ms[i] = [dev newTextureWithDescriptor:md];
		one[i] = [dev newTextureWithDescriptor:od];
		made = made && ms[i] != nil && one[i] != nil;
	}
	if (!made) {
		MSAA_STOP("no-texture", "the attachments could not be made (the driver's sentence "
				       "is the line above)");
		goto out;
	}

	/* the processor's way in and out, which must be refused (and the driver says why) */
	{
		unsigned char probe[4 * 4 * 16];
		bool untouched = true;

		memset(probe, 0xa5, sizeof(probe));
		[ms[2] getBytes:probe bytesPerRow:4 * tb fromRegion:MTLRegionMake2D(0, 0, 4, 4)
		    mipmapLevel:0];
		for (size_t i = 0; i < sizeof(probe); i++) {
			untouched = untouched && probe[i] == 0xa5;
		}
		printf("drvtest: msaa: -getBytes: of a %u-sample texture %s (Metal forbids it; "
		       "the compiler library's refusal is the driver's line above)\n", n,
		       untouched ? "left the caller's bytes as they were" :
				   "WROTE INTO the caller's bytes");
		[ms[2] replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:probe
			 bytesPerRow:4 * tb];
		printf("drvtest: msaa: -replaceRegion: of it was asked as well; the driver's line "
		       "above is its refusal\n");
	}

	uniform = [dev newBufferWithLength:256 options:0];
	if (uniform != nil && [uniform contents] != NULL) {
		unsigned char *bytes = [uniform contents];

		for (int i = 0; i < 256; i++) {
			bytes[i] = (unsigned char)((i * 7 + 13) & 0xff);
		}
	}
	queue = [dev newCommandQueue];

	/* the colour it must be, and the clear's bits (both back ends put the clear's float
	 * bits in an integer channel, M3a) */
	unsigned want[4] = { 0, 0, 0, 0 };
	int member = clear_member_offset(shader);
	bool known = member >= 0 && tb == 16 && uniform != nil;
	float clearf = (float)draw_clear;
	unsigned clearbits;

	memcpy(&clearbits, &clearf, sizeof(clearbits));
	if (known) {
		memcpy(want, (const unsigned char *)[uniform contents] + member, sizeof(want));
	}

	unsigned grid[2][4 * 4 * 4];
	unsigned others = 0;

	for (int pass = 0; pass < 2; pass++) {
		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];

		for (int i = 0; i < 3; i++) {
			MTLRenderPassColorAttachmentDescriptor *a =
			    [[rp colorAttachments] objectAtIndexedSubscript:i];

			[a setTexture:ms[i]];
			[a setLoadAction:MTLLoadActionClear];
			[a setClearColor:MTLClearColorMake(draw_clear, draw_clear, draw_clear,
							   draw_clear)];
			[a setStoreAction:MTLStoreActionMultisampleResolve];
			[a setResolveTexture:one[i]];
		}
		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];

		if (enc == nil) {
			MSAA_STOP("refused-draw", "no render command encoder");
			goto out;
		}
		[enc setRenderPipelineState:ps];
		for (int i = 0; i < 16; i++) {
			[enc setFragmentBuffer:uniform offset:0 atIndex:i];
		}
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0
			vertexCount:pass == 0 ? 4 : 3];
		[enc endEncoding];
		[cmd commit];
		[cmd waitUntilCompleted];
		if ([cmd status] != RW_CB_COMPLETED) {
			char reason[320];

			snprintf(reason, sizeof(reason), "status %lu %s", (unsigned long)[cmd status],
				 [cmd error] != nil ?
				 [[[cmd error] localizedDescription] UTF8String] : "(no reason)");
			printf("drvtest: msaa: the %s pass: %s\n", pass == 0 ? "full" : "triangle",
			       reason);
			MSAA_STOP("refused-draw", reason);
			goto out;
		}
		memset(grid[pass], 0, sizeof(grid[pass]));
		[one[2] getBytes:grid[pass] bytesPerRow:4 * tb fromRegion:MTLRegionMake2D(0, 0, 4, 4)
		     mipmapLevel:0];
		if (pass == 0 && tb == 16) {
			/* the attachments these shaders do not write: the clear, resolved */
			for (int i = 0; i < 2; i++) {
				unsigned px[4 * 4 * 4];

				memset(px, 0, sizeof(px));
				[one[i] getBytes:px bytesPerRow:4 * tb
				      fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
				for (int p = 0; p < 16; p++) {
					others += px[4 * p] == clearbits && px[4 * p + 1] == clearbits &&
						  px[4 * p + 2] == clearbits && px[4 * p + 3] == clearbits;
				}
			}
		}
	}

	/* the full pass: all 16 pixels against the colour, and how far a float one is off */
	unsigned good = 0, ulps = 0;

	for (int p = 0; p < 16 && tb == 16; p++) {
		bool same = true;

		for (int c = 0; c < 4; c++) {
			unsigned g = grid[0][4 * p + c];
			unsigned d = g > want[c] ? g - want[c] : want[c] - g;

			same = same && g == want[c];
			if (known && !format_is_integer(fmt) && d > ulps) {
				ulps = d;   /* same sign, so the distance of the bits is in ulps */
			}
		}
		good += known && same;
	}
	/* the triangle pass: each pixel the colour, the clear, or between them -- and of the
	 * between ones, how many are exactly half way, which a float AVERAGE of two covered and
	 * two uncovered samples (or one and one) is: (x + x + c + c) / 4 with c = 0 is x / 2,
	 * exact in floats */
	unsigned value = 0, clear = 0, between = 0, half = 0;
	char map[4][5];

	for (int p = 0; p < 16 && tb == 16; p++) {
		const unsigned *g = &grid[1][4 * p];
		bool is_value = known && g[0] == want[0] && g[1] == want[1] && g[2] == want[2] &&
				g[3] == want[3];
		bool is_clear = g[0] == clearbits && g[1] == clearbits && g[2] == clearbits &&
				g[3] == clearbits;
		bool is_half = false;

		if (known && !is_value && !is_clear && fmt == MTLPixelFormatRGBA32Float &&
		    draw_clear == 0) {
			is_half = true;
			for (int c = 0; c < 4; c++) {
				float w, h;

				memcpy(&w, &want[c], sizeof(w));
				memcpy(&h, &g[c], sizeof(h));
				is_half = is_half && h == w * 0.5f;
			}
		}
		value += is_value;
		clear += is_clear;
		between += !is_value && !is_clear;
		half += is_half;
		map[p / 4][p % 4] = is_value ? 'V' : is_clear ? '.' : is_half ? 'h' : '?';
		map[p / 4][4] = '\0';
	}
	if (tb == 16) {
		for (int y = 0; y < 4; y++) {
			printf("drvtest: msaa: the triangle pass, resolved row %d: %s   (V the colour, "
			       ". the clear, h exactly half way, ? anything else)\n", y, map[y]);
		}
	}
	printf("drvtest: msaa: the full pass resolved: pixel (0, 0) of attachment 2 is %u %u %u %u; "
	       "%u of 16 pixels are", grid[0][0], grid[0][1], grid[0][2], grid[0][3], good);
	if (known) {
		printf(" %u %u %u %u, the member at byte %d of [[buffer(14)]]", want[0], want[1],
		       want[2], want[3], member);
	} else {
		printf(" a computed colour: none here for %s in pixel format %lu (clearFragment10, 20 "
		       "or 30 in a 16-byte format has one)", shader, (unsigned long)fmt);
	}
	printf("; %u of the 32 pixels of attachments 0 and 1 are the clear\n", others);
	if (n == 2 || n == 4) {
		printf("drvtest: msaa: expected under Vulkan's standard %u-sample positions: the "
		       "triangle pass 6 colour, 6 clear and %s\n", n,
		       format_is_integer(fmt) ? "4 of either, none between (SAMPLE_ZERO)"
					      : "4 between, each exactly half way (AVERAGE)");
	}
	if (known && !format_is_integer(fmt)) {
		printf("drvtest: msaa: a float resolve is %u ulps from the colour at worst (0 is an "
		       "exact average of %u equal samples)\n", ulps, n);
	}
	printf("drvtest: msaa-result %s %s %lu %u %s full %u/16 px %u %u %u %u expect", backend,
	       shader, (unsigned long)fmt, n, mode, good, grid[0][0], grid[0][1], grid[0][2],
	       grid[0][3]);
	if (known) {
		printf(" %u %u %u %u", want[0], want[1], want[2], want[3]);
	} else {
		printf(" none");
	}
	printf(" ulps %u others %u/32 tri value %u clear %u between %u half %u digest %016llx\n",
	       ulps, others, value, clear, between, half,
	       (unsigned long long)fnv1a((const unsigned char *)grid[0], 4 * 4 * tb));
	fflush(stdout);

out:
	print_vk_counts("after the msaa passes, before releasing what they made");
	for (int i = 0; i < 3; i++) {
		[ms[i] release];
		[one[i] release];
	}
	[uniform release];
	[queue release];
	[ps release];
#undef MSAA_STOP
}

/*
 * --picture: the same shaders, at the size of the phone's panel, where a person can look
 * at them.
 *
 * Everything this driver has drawn so far went into a 4x4 off-screen attachment and came
 * back as four numbers. That is a measurement and it is not a picture, and they are not
 * the same claim: 4x4 says nothing about an attachment a megapixel wide, about a varying
 * that has to interpolate over a thousand pixels instead of four, or about a sampler
 * asked for a texel between two texels rather than the one a coordinate lands on.
 *
 * What is drawn, in three bands that together are exactly the panel (1080x2400 -- the
 * size the kernel prints at boot: "display 1125x2436 on a panel of 1080x2400"):
 *
 *   rows 0..1103      64 of Apple's own clearFragment shaders, one per square of an 8x8
 *                     mosaic of 135x138 tiles. The number in the name is a mask of two
 *                     bits per render target -- 0 none, 1 float4, 2 int4, 3 uint4 -- so
 *                     the 64 numbered 4k+1 are exactly those that write a float4 to
 *                     target 0, and each reads that float4 out of the constant buffer at
 *                     index 14 (its ClearFragmentInput is { float4, int4, uint4 } and
 *                     every float4 output is the first field). Each square is given a
 *                     colour of its own in that buffer, so a square is one shader's
 *                     answer and the mosaic is 64 of them side by side.
 *   rows 1104..1751   Apple's texturedVertex2D + texturedFragment2DFloat -- the pipeline
 *                     AppleMetalGLRenderer blits a texture with -- sampling an 8x8
 *                     texture over the whole 1080x648 attachment through a LINEAR
 *                     sampler. Every pixel of it is an interpolated texture coordinate
 *                     and a filtered texel, which is the one thing a 4x4 draw over a
 *                     constant coordinate cannot show at all (docs/metal.md 10.32).
 *   rows 1752..2399   the same draw with a NEAREST sampler. Blocky where the band above
 *                     is smooth, and that difference is the sampler state reaching the
 *                     hardware; it is also the band in which every pixel is exactly one
 *                     texel, so every pixel of it can be counted.
 *
 * The pixels reach the panel through the kernel's own GPU service -- kHKShow,
 * iokit/Kernel/HKGPU.cpp, the selector src/gl_test.c already puts its triangle on the
 * screen with -- and that is what `./xl screenshot` reads. Said plainly because it
 * matters: THE PRESENTATION IS A COPY IN THE KERNEL AND IS NOT METAL'S. What is claimed
 * is that every one of the 2 592 000 pixels handed over was computed by this driver's
 * Metal pipelines on the Adreno. Nothing here is drawn by the processor -- no border, no
 * label, no background -- so that the claim needs no exceptions.
 *
 * And the numbers beside it, because "it looks right" is not a measurement:
 *
 *   - the mosaic: every pixel of every tile against the colour that tile's buffer named,
 *     and every one of the 256 (shader, render target) pairs against what the shader's
 *     own name says it writes -- a target the name does not list must still hold the
 *     clear, which is the half a chart of colours alone would not say. Integer targets
 *     are not checked and are counted as not checked;
 *   - the nearest band: every pixel against the texel the sampler must have returned for
 *     it, floor(x / 135) and floor(y / 81);
 *   - the linear band: the 64 pixels that fall exactly on a texel centre. The band is 8
 *     texels over 1080 columns and 8 over 648 rows, so the scales are 135 and 81, both
 *     ODD -- and that is why a pixel centre can coincide with a texel centre at all
 *     (pixel p is texel centre i when p = s*i + (s-1)/2), which leaves the filter no
 *     weight to round there. Chosen, not noticed.
 *
 * All of it was drawn on this machine first, through the same library on softpipe, so
 * every count below has a reference and not an expectation.
 */
enum {
	/* The panel, not the display. A swap through IOMobileFramebuffer is scaled from the
	 * 1125x2436 user space sees down to this; kHKShow is not scaled at all -- it is
	 * a row-by-row copy cut to the panel -- so this is the size to draw. */
	PANEL_W = 1080, PANEL_H = 2400,
	PIC_TILES = 8,
	PIC_TILE_W = PANEL_W / PIC_TILES,               /* 135 */
	PIC_TILE_H = 138,                               /* so eight rows of tiles are 1104, */
	PIC_MOSAIC_H = PIC_TILES * PIC_TILE_H,          /* and 1104 + 648 + 648 is 2400 */
	PIC_BAND_H = (PANEL_H - PIC_MOSAIC_H) / 2,      /* exactly: no row of the panel is */
	PIC_LINEAR_Y = PIC_MOSAIC_H,                    /* left for the processor to fill */
	PIC_NEAREST_Y = PIC_MOSAIC_H + PIC_BAND_H,
	PIC_TEX = 8,
	PIC_SX = PANEL_W / PIC_TEX,                     /* 135, odd */
	PIC_SY = PIC_BAND_H / PIC_TEX,                  /* 81, odd */
};

/* Eight levels of a channel, each an exact byte. A texel written as k/255 comes back as
 * exactly k through an 8-bit unorm attachment, so no check here needs a tolerance and a
 * difference of one is a difference. */
static const unsigned char pic_ramp[PIC_TILES] = { 0, 36, 73, 109, 146, 182, 219, 255 };

/*
 * The colour of mosaic tile (i, j) and of texel (x, y), in the panel's own order: blue
 * first. The four corners of each are four different primaries on purpose -- a channel
 * swapped anywhere between the shader and the panel, or a picture mirrored, moves which
 * corner is which, and that is seen before it is counted.
 */
static void
pic_mosaic_colour(unsigned i, unsigned j, unsigned char bgra[4])
{
	bgra[0] = pic_ramp[PIC_TILES - 1 - i];
	bgra[1] = pic_ramp[j];
	bgra[2] = pic_ramp[i];
	bgra[3] = 255;
}

static void
pic_texel_colour(unsigned x, unsigned y, unsigned char bgra[4])
{
	bgra[0] = pic_ramp[y];
	bgra[1] = pic_ramp[PIC_TILES - 1 - y];
	bgra[2] = pic_ramp[x];
	bgra[3] = 255;
}

/* The panel's pixels as this process holds them, and the request the kernel takes them in
 * (iokit/Kernel/HKGPU.cpp, struct hk_show). Declared here and not included,
 * because this program has no header of the kernel's -- exactly as src/gl_test.c does. */
static unsigned char *pic_panel;

struct pic_show_request {
	uint64_t pixels;
	uint32_t width, height, row_bytes, pad;
};

/*
 * The connection to that service, opened BEFORE anything is drawn and used after every
 * stage that finishes, rather than once at the end.
 *
 * That order is not tidiness, it is what the first boot cost (logs/101536-run.log): the
 * mosaic drew and counted perfectly on the Adreno and then the process stopped dead on
 * its way into the bands, so nothing ever reached the panel and the screenshot caught the
 * loader's own splash. A stage that finished is a picture; it belongs on the screen the
 * moment it is finished, and a stall after it should cost the bands and not the
 * screenshot. The copy persists by itself -- MiniIOMobileFramebufferShow writes the boot
 * framebuffer and, with JOBS="" , nothing else touches it -- so one push is enough to be
 * photographed even if the process never reaches the repainting loop at the end.
 */
static io_connect_t pic_connection;

static void
pic_open_panel(void)
{
	io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("HKGPU"));

	if (service == 0 || IOServiceOpen(service, mach_task_self(), 0, &pic_connection) != 0) {
		pic_connection = 0;
		printf("drvtest: picture: no GPU service in this kernel to put the picture on "
		       "the panel with; the counts below still stand but nothing will be "
		       "seen\n");
	} else {
		printf("drvtest: picture: the panel is open; every stage goes on the screen as "
		       "it is finished\n");
	}
	fflush(stdout);
}

static void
pic_push(const char *what)
{
	struct pic_show_request show = {
		(uint64_t)(uintptr_t)pic_panel, PANEL_W, PANEL_H, PANEL_W * 4, 0
	};

	if (pic_connection == 0 || pic_panel == NULL) {
		return;
	}
	kern_return_t r = IOConnectCallStructMethod(pic_connection, 0x7f, &show, sizeof(show),
						    NULL, NULL);

	printf("drvtest: picture: ON THE PANEL (%#x): %s. A screenshot taken now has it\n", r,
	       what);
	fflush(stdout);
}

static uint64_t
pic_now_ms(void)
{
	static mach_timebase_info_data_t base;

	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	return mach_absolute_time() * base.numer / base.denom / 1000000;
}

/*
 * Every step of the band stage says its own name before it starts, and how long the step
 * before it took.
 *
 * The first boot of this mode had NO line at all between the mosaic's summary and the
 * kernel's idle heartbeat, because on this path nothing prints unless something fails
 * (logs/101536-run.log) -- so the stall had the whole stage to hide in. The second boot,
 * with these lines, put it between two statements: "the probe: asking for an encoder" was
 * the last thing printed. The timestamp is here because that boot still could not say
 * whether the step was stuck or merely slow, and those are different bugs.
 */
static char pic_where[192] = "before anything";
static volatile uint64_t pic_since_ms;
static volatile int pic_alive = 1;

static void
pic_step(const char *what)
{
	uint64_t now = pic_now_ms(), took = pic_since_ms ? now - pic_since_ms : 0;

	printf("drvtest: picture: band step: %s   (the step before took %llu ms)\n", what,
	       (unsigned long long)took);
	fflush(stdout);
	snprintf(pic_where, sizeof(pic_where), "%s", what);
	pic_since_ms = now;
}

/*
 * And a thread that says the process is still alive and still in that step.
 *
 * Silence has meant three different things so far -- blocked, killed, and simply not
 * printing -- and nothing in the log told them apart. This separates two of the three: as
 * long as these lines keep coming, the process is alive and the step named in them has not
 * returned; when they stop, the process is gone. It writes with write(2) and not printf on
 * purpose: if the main thread is stuck holding stdio's lock, a printf here would block too
 * and the one case this is for would be the one case it could not report.
 *
 * It backs off by doubling -- 2, 4, 8, 16 ... seconds -- because the loader keeps the TAIL
 * of a long log, and a line a second for four minutes would push the measurements that
 * matter out of it.
 */
static void *
pic_heartbeat(void *unused)
{
	uint64_t next = 2000, watching = 0;

	(void)unused;
	for (;;) {
		char line[320];
		uint64_t in;
		int n;

		usleep(250000);
		if (!pic_alive) {
			return NULL;
		}
		if (pic_since_ms != watching) {
			watching = pic_since_ms;   /* a new step: count from it, not from the */
			next = 2000;               /* first slow one */
		}
		in = pic_now_ms() - pic_since_ms;
		if (in < next) {
			continue;
		}
		next *= 2;
		n = snprintf(line, sizeof(line), "drvtest: picture: STILL IN \"%s\" after "
			     "%llu ms -- this process is alive and that step has not "
			     "returned\n", pic_where, (unsigned long long)in);
		if (n > 0) {
			ssize_t written = write(1, line, (size_t)n);

			(void)written;
		}
	}
}

/*
 * What the QUEUE says about itself, rather than what this file infers.
 *
 * The second ladder named the stall as -[MTLCommandQueue commandBuffer] on the 65th
 * command buffer of the process, and 64 was Metal's own maxCommandBufferCount: every
 * _MTLCommandBuffer waits on the queue's semaphore in its designated initialiser and owes
 * it a signal, and this driver never paid (hkmetal.m). That was an inference from a
 * number that happened to be 64. These two lines are the measurement instead: the queue
 * reports how many buffers it takes at a time and how many of this process's still hold a
 * slot, and it says OUT LOUD when the next one will block -- before it does.
 */
static unsigned long
pic_queue_state(id<MTLCommandQueue> queue, const char *when)
{
	NSUInteger (*ask)(id, SEL) = (NSUInteger (*)(id, SEL))objc_msgSend;
	SEL outstanding = sel_getUid("hkOutstandingCommandBuffers");
	SEL made = sel_getUid("hkCommandBuffersMade");
	SEL most = sel_getUid("maxCommandBufferCount");

	if (![queue respondsToSelector:outstanding] || ![queue respondsToSelector:most]) {
		printf("drvtest: picture: %s: this queue does not say how many command buffers "
		       "hold a slot, so the next block would again have to be inferred\n", when);
		fflush(stdout);
		return ~0UL;
	}
	unsigned long o = (unsigned long)ask(queue, outstanding);
	unsigned long m = (unsigned long)ask(queue, most);
	unsigned long n = [queue respondsToSelector:made] ?
	    (unsigned long)ask(queue, made) : 0;

	printf("drvtest: picture: %s: the queue takes %lu command buffers at a time, %lu "
	       "made so far, %lu still holding a slot%s\n", when, m, n, o,
	       m != 0 && o >= m ? " -- THE NEXT -commandBuffer WILL BLOCK FOR EVER" : "");
	fflush(stdout);
	return o;
}

/* The backing-off clock starts again at every step, so "STILL IN" counts from the step it
 * names and not from the first slow one. */
static void
pic_watch(void)
{
	pthread_t thread;

	pic_since_ms = pic_now_ms();
	if (pthread_create(&thread, NULL, pic_heartbeat, NULL) == 0) {
		pthread_detach(thread);
		printf("drvtest: picture: a second thread will say every so often that this "
		       "process is alive and which step it is in\n");
	} else {
		printf("drvtest: picture: no second thread, so silence below will again mean "
		       "either blocked or killed\n");
	}
	fflush(stdout);
}

/*
 * Four attachments, all BGRA8Unorm and all one size: the mosaic's shaders write up to
 * four render targets and Metal requires a pass's formats to be the pipeline's, so four
 * it is for every pipeline here. BGRA8Unorm because that is the panel's own order, so
 * what -getBytes: hands back is what the panel takes, with nothing swapped in between.
 */
static bool
pic_targets(id<MTLDevice> dev, id<MTLTexture> *t, unsigned n, unsigned w, unsigned h,
	    MTLRenderPassDescriptor **rp)
{
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
							       width:w
							      height:h
							   mipmapped:NO];

	[td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];
	*rp = [[MTLRenderPassDescriptor renderPassDescriptor] retain];
	for (unsigned i = 0; i < n; i++) {
		t[i] = [dev newTextureWithDescriptor:td];
		if (t[i] == nil) {
			printf("drvtest: picture: attachment %u of %ux%u could not be made\n",
			       i, w, h);
			return false;
		}
		MTLRenderPassColorAttachmentDescriptor *a =
		    [[*rp colorAttachments] objectAtIndexedSubscript:i];

		[a setTexture:t[i]];
		[a setLoadAction:MTLLoadActionClear];
		/* opaque black: a tile nothing drew is black on the panel and black in the
		 * count, and the two then agree about which tile it was */
		[a setClearColor:MTLClearColorMake(0, 0, 0, 1)];
		[a setStoreAction:MTLStoreActionStore];
	}
	return true;
}

static void
pic_release_targets(id<MTLTexture> *t, unsigned n, MTLRenderPassDescriptor *rp)
{
	for (unsigned i = 0; i < n; i++) {
		[t[i] release];
		t[i] = nil;
	}
	[rp release];
}

static id<MTLRenderPipelineState>
pic_pipeline(id<MTLDevice> dev, id<MTLFunction> vertex, id<MTLFunction> fragment,
	     unsigned n, const char *what)
{
	MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
	NSError *err = nil;

	[pd setVertexFunction:vertex];
	[pd setFragmentFunction:fragment];
	for (unsigned i = 0; i < n; i++) {
		[[[pd colorAttachments] objectAtIndexedSubscript:i]
		    setPixelFormat:MTLPixelFormatBGRA8Unorm];
	}
	id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd
									    error:&err];
	[pd release];
	if (ps == nil) {
		printf("drvtest: picture: %s got no pipeline: %s\n", what,
		       err != nil ? [[err localizedDescription] UTF8String] : "(no reason)");
	}
	return ps;
}

/*
 * One draw into the pass, waited on. True when the GPU finished it.
 *
 * `trace` says every part of it out loud, which the band stage needs and the mosaic's 64
 * draws do not: this driver's command buffer RECORDS the draw and performs it on -commit
 * (10.13), so encoding, committing and waiting are three different places to stop and the
 * log has to tell them apart. The mosaic passes false so that 64 working draws do not bury
 * the one that does not.
 */
static bool
pic_draw(id<MTLCommandQueue> queue, MTLRenderPassDescriptor *rp,
	 id<MTLRenderPipelineState> ps, id<MTLBuffer> *vbuf, unsigned nvbuf,
	 id<MTLBuffer> fbuf, unsigned nfbuf, id<MTLTexture> tex, id<MTLSamplerState> samp,
	 const char *what, bool trace)
{
	char line[192];

	/* The second boot stopped between these two calls and the one line covered both, so
	 * they are two lines now. Neither of them has anything in it proportional to the
	 * attachment's area -- -commandBuffer allocates an object and an NSMutableArray, and
	 * the encoder's init reads four properties per attachment (hkmetal.m) -- which
	 * is exactly why the two have to be told apart before anything else is believed. */
	if (trace) {
		snprintf(line, sizeof(line), "%s: asking the queue for a command buffer", what);
		pic_step(line);
	}
	id<MTLCommandBuffer> cmd = [queue commandBuffer];

	if (trace) {
		snprintf(line, sizeof(line), "%s: got the command buffer; asking IT for a "
			 "render encoder", what);
		pic_step(line);
	}
	id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];

	if (trace) {
		snprintf(line, sizeof(line), "%s: got the encoder", what);
		pic_step(line);
	}

	if (enc == nil) {
		printf("drvtest: picture: %s got no encoder\n", what);
		return false;
	}
	[enc setRenderPipelineState:ps];
	for (unsigned i = 0; i < nfbuf; i++) {
		[enc setFragmentBuffer:fbuf offset:0 atIndex:i];
	}
	for (unsigned i = 0; i < nvbuf; i++) {
		[enc setVertexBuffer:vbuf[i] offset:0 atIndex:i];
	}
	if (tex != nil) {
		[enc setFragmentTexture:tex atIndex:0];
	}
	if (samp != nil) {
		[enc setFragmentSamplerState:samp atIndex:0];
	}
	if (trace) {
		snprintf(line, sizeof(line), "%s: bound; encoding the quad", what);
		pic_step(line);
	}
	[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	[enc endEncoding];
	if (trace) {
		snprintf(line, sizeof(line), "%s: encoded; committing (this driver does the "
			 "work here, not at the wait)", what);
		pic_step(line);
	}
	[cmd commit];
	if (trace) {
		snprintf(line, sizeof(line), "%s: committed; waiting", what);
		pic_step(line);
	}
	[cmd waitUntilCompleted];
	if ([cmd status] != RW_CB_COMPLETED) {
		printf("drvtest: picture: %s: status %lu%s%s\n", what,
		       (unsigned long)[cmd status], [cmd error] != nil ? ", " : "",
		       [cmd error] != nil ? [[[cmd error] localizedDescription] UTF8String]
					  : "");
		return false;
	}
	return true;
}

/* One rectangle of an attachment into the panel's buffer at (x0, y0). */
static void
pic_blit(const unsigned char *from, unsigned w, unsigned h, unsigned x0, unsigned y0)
{
	for (unsigned y = 0; y < h; y++) {
		memcpy(pic_panel + ((size_t)(y0 + y) * PANEL_W + x0) * 4,
		       from + (size_t)y * w * 4, (size_t)w * 4);
	}
}

/* The mosaic: 64 of Apple's clearFragment shaders, each into its own square. */
static void
pic_mosaic(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue)
{
	id<MTLTexture> mt[4] = { nil, nil, nil, nil };
	MTLRenderPassDescriptor *mrp = nil;

	if (!pic_targets(dev, mt, 4, PIC_TILE_W, PIC_TILE_H, &mrp)) {
		pic_release_targets(mt, 4, mrp);
		return;
	}
	/* The constant buffer every clearFragment reads: declared at index 14, bound at
	 * every index up to 16 for draw_path's reason -- a driver that bound only index 0
	 * would leave the shader reading nothing. Its first float4 is the square's colour
	 * and is rewritten before each draw, which is safe because each draw is waited on
	 * before the next is encoded. */
	id<MTLBuffer> uniform = [dev newBufferWithLength:256 options:0];
	unsigned char *tile = malloc((size_t)PIC_TILE_W * PIC_TILE_H * 4);

	if (uniform == nil || [uniform contents] == NULL || tile == NULL) {
		printf("drvtest: picture: no constant buffer or no memory for a tile\n");
		pic_release_targets(mt, 4, mrp);
		free(tile);
		return;
	}
	unsigned drew = 0, nopipe = 0, nodraw = 0;
	unsigned long rt_colour = 0, rt_clear = 0, rt_skipped = 0, rt_wrong = 0;
	unsigned long bad_mosaic = 0;

	for (unsigned k = 0; k < PIC_TILES * PIC_TILES; k++) {
		unsigned nn = 4 * k + 1, i = k % PIC_TILES, j = k / PIC_TILES;
		char name[32];

		snprintf(name, sizeof(name), "clearFragment%02X", nn);
		id<MTLFunction> fn = [library newFunctionWithName:
		    [NSString stringWithUTF8String:name]];

		if (fn == nil) {
			printf("drvtest: picture: no %s in that library\n", name);
			nopipe++;
			continue;
		}
		id<MTLRenderPipelineState> ps = pic_pipeline(dev, nil, fn, 4, name);

		if (ps == nil) {
			nopipe++;
			[fn release];
			continue;
		}
		unsigned char want[4];

		pic_mosaic_colour(i, j, want);
		float *u = [uniform contents];

		/* zeroed first: the int4 and uint4 fields of ClearFragmentInput are read by
		 * the integer targets this check skips, and memory nobody set would make
		 * "not checked" mean two things at once */
		memset(u, 0, 256);
		u[0] = want[2] / 255.0f;        /* the shader's float4 is r, g, b, a */
		u[1] = want[1] / 255.0f;
		u[2] = want[0] / 255.0f;
		u[3] = 1.0f;
		if (!pic_draw(queue, mrp, ps, NULL, 0, uniform, 16, nil, nil, name, false)) {
			nodraw++;
			[ps release];
			[fn release];
			continue;
		}
		drew++;
		/* Each of this shader's four targets against what its own name says it
		 * writes. A target the name does not list must still hold the clear. */
		for (unsigned t = 0; t < 4; t++) {
			unsigned kind = (nn >> (2 * t)) & 3;
			unsigned char expect[4] = { 0, 0, 0, 255 };
			unsigned long bad = 0;

			if (kind == 2 || kind == 3) {
				rt_skipped++;
				continue;
			}
			if (kind == 1) {
				memcpy(expect, want, 4);
			}
			[mt[t] getBytes:tile
			    bytesPerRow:PIC_TILE_W * 4
			     fromRegion:MTLRegionMake2D(0, 0, PIC_TILE_W, PIC_TILE_H)
			    mipmapLevel:0];
			for (size_t q = 0; q < (size_t)PIC_TILE_W * PIC_TILE_H; q++) {
				if (tile[q * 4] != expect[0] || tile[q * 4 + 1] != expect[1] ||
				    tile[q * 4 + 2] != expect[2]) {
					bad++;
				}
			}
			if (bad != 0) {
				rt_wrong++;
				if (rt_wrong <= 6) {
					printf("drvtest: picture: %s target %u (%s): %lu of "
					       "%u pixels wrong\n", name, t,
					       kind != 0 ? "its own colour"
							 : "not written, so the clear",
					       bad, PIC_TILE_W * PIC_TILE_H);
				}
			} else if (kind != 0) {
				rt_colour++;
			} else {
				rt_clear++;
			}
			if (t == 0) {
				bad_mosaic += bad;
				pic_blit(tile, PIC_TILE_W, PIC_TILE_H, i * PIC_TILE_W,
					 j * PIC_TILE_H);
			}
		}
		[ps release];
		[fn release];
	}
	free(tile);
	pic_release_targets(mt, 4, mrp);
	printf("drvtest: picture: mosaic: %u of %u shaders drew (%u got no pipeline, %u did "
	       "not finish)\n", drew, PIC_TILES * PIC_TILES, nopipe, nodraw);
	printf("drvtest: picture: mosaic: %lu pixels of %u are not the colour their square's "
	       "buffer named\n", bad_mosaic,
	       PIC_TILES * PIC_TILES * PIC_TILE_W * PIC_TILE_H);
	printf("drvtest: picture: mosaic: of %u (shader, render target) pairs, %lu held the "
	       "colour, %lu stayed at the clear, %lu are integer targets and not checked, "
	       "%lu are WRONG\n", 4 * PIC_TILES * PIC_TILES, rt_colour, rt_clear, rt_skipped,
	       rt_wrong);
	fflush(stdout);
}

/*
 * One named experiment: N attachments of W by H, a pipeline for clearFragment01 -- the
 * shader the mosaic has already drawn 64 times on this GPU -- an encoder, a draw and a
 * readback, every one of them saying its own name first.
 *
 * Four of these run in a ladder before Apple's blit, and between them they name the axis
 * the stall is on. The second boot (logs/101536-run.log's successor) stopped between
 * -commandBuffer and -renderCommandEncoderWithDescriptor: on four 1080x648 attachments,
 * and reading the driver afterwards found NOTHING in either call proportional to the
 * attachment's area: -commandBuffer allocates an object and an NSMutableArray, and the
 * encoder's init reads loadAction, storeAction, resolveTexture and clearColor per
 * attachment and nothing else. So the obvious story -- "a megapixel attachment is too
 * big" -- is not supported by the code, and the ladder is built to find out what is true
 * instead of assuming it:
 *
 *   again-small   four 135x138, the mosaic's own shape, once more. If THIS stalls, size
 *                 has nothing to do with it: what changed is that it is the 65th command
 *                 buffer of the process, with 64 encoders, 64 pipelines and 64 command
 *                 buffers behind it that nothing has freed (this file has no autorelease
 *                 pool, deliberately: the mosaic's objects are small and the leak is
 *                 bounded).
 *   one-big       ONE 1080x648. Size with no count.
 *   four-medium   four 540x324 -- 2.8 MB of attachment against four-big's 11.2 MB, and
 *                 the same count. Count with a quarter of the area.
 *   four-big      four 1080x648, the one that stalled.
 *
 * The first rung that does not finish is the answer, and the heartbeat thread says
 * whether it is stuck or merely slow while it is happening.
 */
static bool
pic_probe(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue,
	  unsigned n, unsigned w, unsigned h, bool full, const char *name)
{
	/* the colour, three different levels of pic_ramp so that a channel in the wrong
	 * place is seen, each an exact byte so the count needs no tolerance */
	const unsigned char want[4] = { 109, 182, 73, 255 };   /* b, g, r, a */
	id<MTLTexture> t[4] = { nil, nil, nil, nil };
	MTLRenderPassDescriptor *rp = nil;
	id<MTLFunction> fn = nil;
	id<MTLRenderPipelineState> ps = nil;
	id<MTLBuffer> u = nil;
	unsigned char *back = NULL;
	bool ok = false;
	char line[192];

	snprintf(line, sizeof(line), "probe %s: %u attachment%s of %ux%u (%lu KB of "
		 "attachment)", name, n, n == 1 ? "" : "s", w, h,
		 (unsigned long)((size_t)n * w * h * 4 / 1024));
	pic_step(line);
	if (!pic_targets(dev, t, n, w, h, &rp)) {
		goto done;
	}
	snprintf(line, sizeof(line), "probe %s: the attachments are made", name);
	pic_step(line);

	fn = [library newFunctionWithName:@"clearFragment01"];
	if (fn == nil) {
		printf("drvtest: picture: probe %s: no clearFragment01\n", name);
		goto done;
	}
	snprintf(line, sizeof(line), "probe %s: the pipeline", name);
	pic_step(line);
	ps = pic_pipeline(dev, nil, fn, n, name);
	if (ps == nil) {
		goto done;
	}
	snprintf(line, sizeof(line), "probe %s: the constant buffer", name);
	pic_step(line);
	u = [dev newBufferWithLength:256 options:0];
	if (u == nil || [u contents] == NULL) {
		printf("drvtest: picture: probe %s: no constant buffer\n", name);
		goto done;
	}
	{
		float *f = [u contents];

		memset(f, 0, 256);
		f[0] = want[2] / 255.0f;
		f[1] = want[1] / 255.0f;
		f[2] = want[0] / 255.0f;
		f[3] = 1.0f;
	}
	snprintf(line, sizeof(line), "probe %s, before it asks for a command buffer", name);
	pic_queue_state(queue, line);
	snprintf(line, sizeof(line), "probe %s", name);
	if (!pic_draw(queue, rp, ps, NULL, 0, u, 16, nil, nil, line, true)) {
		goto done;
	}
	snprintf(line, sizeof(line), "probe %s: drew; reading ONE pixel back", name);
	pic_step(line);
	{
		unsigned char one[4] = { 0, 0, 0, 0 };

		[t[0] getBytes:one
		    bytesPerRow:4
		     fromRegion:MTLRegionMake2D(0, 0, 1, 1)
		    mipmapLevel:0];
		printf("drvtest: picture: probe %s: pixel (0,0) is r g b %u %u %u and must be "
		       "%u %u %u\n", name, one[2], one[1], one[0], want[2], want[1], want[0]);
		fflush(stdout);
		ok = one[0] == want[0] && one[1] == want[1] && one[2] == want[2];
	}
	if (!full) {
		goto done;
	}
	snprintf(line, sizeof(line), "probe %s: the whole %ux%u back in one getBytes:", name,
		 w, h);
	pic_step(line);
	back = malloc((size_t)w * h * 4);
	if (back == NULL) {
		printf("drvtest: picture: probe %s: no memory to read %ux%u into\n", name, w, h);
		goto done;
	}
	memset(back, 0, (size_t)w * h * 4);
	[t[0] getBytes:back
	    bytesPerRow:w * 4
	     fromRegion:MTLRegionMake2D(0, 0, w, h)
	    mipmapLevel:0];
	{
		unsigned long bad = 0;

		for (size_t q = 0; q < (size_t)w * h; q++) {
			if (back[q * 4] != want[0] || back[q * 4 + 1] != want[1] ||
			    back[q * 4 + 2] != want[2]) {
				bad++;
			}
		}
		printf("drvtest: picture: probe %s: %lu pixels of %u are not that colour, so a "
		       "%ux%u render target draws and reads back %s\n", name, bad, w * h, w, h,
		       bad == 0 ? "WHOLE" : "WRONG");
		fflush(stdout);
		ok = ok && bad == 0;
	}
done:
	free(back);
	[u release];
	[ps release];
	[fn release];
	pic_release_targets(t, n, rp);
	snprintf(line, sizeof(line), "probe %s: finished, and its resources are freed", name);
	pic_step(line);
	return ok;
}

/*
 * The ladder, in the order that makes the first failure mean the most.
 *
 * Each rung frees everything it made before the next begins, so a rung is about its own
 * shape and not about what the rungs before it are still holding.
 */
static void
pic_ladder(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue)
{
	pic_step("THE LADDER: four experiments that separate count from size. The first one "
		 "that does not finish is the answer");
	/*
	 * And the verdict on the bug the picture found, named so that a later change cannot
	 * quietly lose it: the mosaic has just made 64 command buffers and this test keeps
	 * every one of them alive ON PURPOSE -- there is no autorelease pool round its loop,
	 * and there must not be one. With a pool, each buffer would be deallocated at the
	 * end of its iteration and the slot would come back from -dealloc, so this would
	 * pass whether or not -commit returns it, which is the whole failure the driver had.
	 * Sixty-four live command buffers holding no slot is the measurement.
	 */
	unsigned long held = pic_queue_state(queue, "after the mosaic's 64 command buffers");

	if (held != ~0UL) {
		printf("drvtest: picture: the mosaic made 64 command buffers and %lu of them "
		       "still hold a queue slot: %s\n", held,
		       held == 0 ? "the slot IS given back at -commit, so the next command "
				   "buffer will not block" :
				   "the slot is NOT given back, and the next command buffer "
				   "will block for ever (this is the driver bug the picture "
				   "found)");
		fflush(stdout);
	}
	pic_probe(dev, library, queue, 4, PIC_TILE_W, PIC_TILE_H, false, "again-small");
	pic_probe(dev, library, queue, 1, PANEL_W, PIC_BAND_H, true, "one-big");
	pic_probe(dev, library, queue, 4, PANEL_W / 2, PIC_BAND_H / 2, false, "four-medium");
	pic_probe(dev, library, queue, 4, PANEL_W, PIC_BAND_H, true, "four-big");
	pic_queue_state(queue, "after the ladder");
	pic_step("THE LADDER: all four rungs finished");
}

/*
 * The two bands: Apple's own textured blit over the whole attachment, linear and nearest.
 *
 * This is where the first boot stopped (logs/101536-run.log): the mosaic's summary was the
 * last line of the process, and after it the kernel's idle heartbeat and nothing else for
 * four minutes. The GPU was not wedged -- the heartbeat went on reading sensors and the run
 * ended cleanly -- and the driver refuses a draw the GPU does not finish with a sentence
 * and a status (that is how Apple's two [[depth]] shaders are known to hang it,
 * logs/094810-run.log: "status 5, the GPU did not finish the draw"). So a silent stop is
 * NOT a hung dispatch, and the suspects are all on this side of the fence.
 *
 * Everything here is therefore built to name the step rather than to survive it:
 *
 *   - every step says its own name BEFORE it starts, flushed. On the way this stage takes,
 *     nothing printed unless something failed, so the stall had the whole stage to hide in;
 *   - between the mosaic and the textured pipeline there is now a PROBE, which is the
 *     cheapest way to cut the suspect space in half in one boot. It draws
 *     clearFragment01 -- the shader the mosaic has already drawn 64 times, with the
 *     generated vertex stage, no texture, no sampler and no vertex buffer -- into the
 *     band-sized attachments, and reads first one pixel and then the whole 1080x648 back.
 *     Three different things can stall in there and each has its own line, so the boot
 *     says which: making four 1080x648 BGRA8Unorm render targets, drawing into one, and
 *     reading one back. If the probe gets through, none of those is the cause and what is
 *     left is Apple's vertex stage, the sampled texture and the sampler;
 *   - nothing here stores to [[depth]], which is the one thing known to hang this GPU.
 *
 * And the mosaic is already on the panel before any of it runs, so a stall now costs the
 * bands and not the screenshot.
 */
static void
pic_bands(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue)
{
	id<MTLTexture> bt[4] = { nil, nil, nil, nil };
	MTLRenderPassDescriptor *brp = nil;
	id<MTLRenderPipelineState> ps = nil;
	id<MTLTexture> grad = nil;
	id<MTLBuffer> vpos = nil, vuv = nil;
	id<MTLFunction> vfn = nil, ffn = nil;
	MTLTextureDescriptor *gd = nil;
	unsigned char *band = NULL;
	static float texels[PIC_TEX * PIC_TEX * 4];
	static const float quad[4][4] = {
		{ -1, -1, 0, 1 }, { 1, -1, 0, 1 }, { -1, 1, 0, 1 }, { 1, 1, 0, 1 },
	};
	static const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };

	pic_step("the memory for one band on this side");
	band = malloc((size_t)PANEL_W * PIC_BAND_H * 4);
	if (band == NULL) {
		printf("drvtest: picture: no memory for a band of %u by %u\n", PANEL_W,
		       PIC_BAND_H);
		goto out;
	}

	pic_step("four 1080x648 BGRA8Unorm render targets for the two bands (the ladder has "
		 "just made and freed the same four)");
	if (!pic_targets(dev, bt, 4, PANEL_W, PIC_BAND_H, &brp)) {
		goto out;
	}
	pic_step("four attachments made");

	/* ---- Apple's own blit ------------------------------------------------------- */
	pic_step("Apple's two functions, texturedVertex2D and texturedFragment2DFloat");
	vfn = [library newFunctionWithName:@"texturedVertex2D"];
	ffn = [library newFunctionWithName:@"texturedFragment2DFloat"];
	printf("drvtest: picture: texturedVertex2D %s, texturedFragment2DFloat %s\n",
	       vfn != nil ? "is here" : "is MISSING", ffn != nil ? "is here" : "is MISSING");
	fflush(stdout);
	if (vfn == nil || ffn == nil) {
		goto out;
	}
	pic_step("the textured pipeline, with Apple's vertex stage");
	ps = pic_pipeline(dev, vfn, ffn, 4, "texturedVertex2D + texturedFragment2DFloat");
	if (ps == nil) {
		goto out;
	}
	pic_step("the textured pipeline is made");

	/* The texture Apple's blit samples: 8x8, RGBA32Float because the shader declares
	 * texture2d<float, sample>, every channel a level of pic_ramp so that what the
	 * sampler returns is exact in eight bits. */
	pic_step("the 8x8 RGBA32Float texture");
	gd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
								width:PIC_TEX
							       height:PIC_TEX
							    mipmapped:NO];
	[gd setUsage:MTLTextureUsageShaderRead];
	grad = [dev newTextureWithDescriptor:gd];
	if (grad == nil) {
		printf("drvtest: picture: the 8x8 texture could not be made\n");
		goto out;
	}
	for (unsigned y = 0; y < PIC_TEX; y++) {
		for (unsigned x = 0; x < PIC_TEX; x++) {
			unsigned char c[4];
			float *t = texels + (y * PIC_TEX + x) * 4;

			pic_texel_colour(x, y, c);
			t[0] = c[2] / 255.0f;
			t[1] = c[1] / 255.0f;
			t[2] = c[0] / 255.0f;
			t[3] = 1.0f;
		}
	}
	/* one call for the whole picture and not one per row, for draw_path's reason: a
	 * seed written at a non-zero region origin is a second thing being measured */
	pic_step("seeding that texture in one replaceRegion:");
	[grad replaceRegion:MTLRegionMake2D(0, 0, PIC_TEX, PIC_TEX)
		mipmapLevel:0
		  withBytes:texels
		bytesPerRow:PIC_TEX * 4 * sizeof(float)];
	/* and read it back, for draw_path's other reason: a draw that samples a texture
	 * nobody has looked at cannot tell a wrong sample from a wrong seed */
	pic_step("reading that texture back");
	{
		static float back[PIC_TEX * PIC_TEX * 4];

		memset(back, 0, sizeof(back));
		[grad getBytes:back
		   bytesPerRow:PIC_TEX * 4 * sizeof(float)
		    fromRegion:MTLRegionMake2D(0, 0, PIC_TEX, PIC_TEX)
		   mipmapLevel:0];
		printf("drvtest: picture: texel (0,0) reads back r g b %g %g %g and was seeded "
		       "%g %g %g\n", (double)back[0], (double)back[1], (double)back[2],
		       (double)texels[0], (double)texels[1], (double)texels[2]);
		fflush(stdout);
	}

	/* What texturedVertex2D reads: buffer 0 one float4 of clip-space position per
	 * vertex, buffer 1 one float2 of texture coordinate. The corners and the
	 * coordinates are air_metal_test's own, so this draw and the host's are one quad.
	 *
	 * Which way up it comes out is the viewport's, not this file's, and it has just
	 * changed: the library now puts clip y = +1 in memory row 0, which is Metal's rule
	 * (docs/metal.md 10.28, landed once 10.32 had fixed the flat varying that was
	 * hiding it). The corner at clip (-1, +1) therefore lands in row 0 and carries the
	 * coordinate v = 1, so the TOP of the band is texel row PIC_TEX-1 and the band is
	 * the texture mirrored vertically.
	 *
	 * This table is deliberately NOT flipped to cancel that. The four corners and the
	 * four coordinates are air_metal_test's own, so that this draw and the host's are
	 * one quad; and compensating in the coordinates would hide the very thing the new
	 * sign made visible, which is how a flat varying passed for a vertical flip for
	 * three boots. What flips is the EXPECTATION, in pic_count_bands.
	 *
	 * The picture says it for itself: texel (0,0) is green and texel (0,7) is blue, so
	 * the band must now come out BLUE at the top and GREEN at the bottom, and two named
	 * pixels below say it in numbers rather than in a count.
	 */
	pic_queue_state(queue, "before the two band draws");
	pic_step("the two vertex buffers Apple's vertex stage reads");
	vpos = [dev newBufferWithLength:64 options:0];
	vuv = [dev newBufferWithLength:64 options:0];
	if (vpos == nil || vuv == nil || [vpos contents] == NULL || [vuv contents] == NULL) {
		printf("drvtest: picture: no vertex buffers\n");
		goto out;
	}
	memcpy([vpos contents], quad, sizeof(quad));
	memcpy([vuv contents], uv, sizeof(uv));

	for (int pass = 0; pass < 2; pass++) {
		const char *what = pass == 0 ? "the linear band" : "the nearest band";
		id<MTLBuffer> vbuf[2] = { vpos, vuv };
		MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];

		pic_step(pass == 0 ? "the linear sampler" : "the nearest sampler");
		[sd setMinFilter:pass == 0 ? MTLSamplerMinMagFilterLinear
					   : MTLSamplerMinMagFilterNearest];
		[sd setMagFilter:pass == 0 ? MTLSamplerMinMagFilterLinear
					   : MTLSamplerMinMagFilterNearest];
		[sd setSAddressMode:MTLSamplerAddressModeClampToEdge];
		[sd setTAddressMode:MTLSamplerAddressModeClampToEdge];
		id<MTLSamplerState> samp = [dev newSamplerStateWithDescriptor:sd];

		[sd release];
		if (samp == nil) {
			printf("drvtest: picture: no %s\n", what);
			continue;
		}
		pic_step(pass == 0 ? "drawing the linear band" : "drawing the nearest band");
		if (pic_draw(queue, brp, ps, vbuf, 2, nil, 0, grad, samp, what, true)) {
			pic_step(pass == 0 ? "the linear band drew; reading it back"
					   : "the nearest band drew; reading it back");
			[bt[0] getBytes:band
			    bytesPerRow:PANEL_W * 4
			     fromRegion:MTLRegionMake2D(0, 0, PANEL_W, PIC_BAND_H)
			    mipmapLevel:0];
			pic_blit(band, PANEL_W, PIC_BAND_H, 0,
				 pass == 0 ? PIC_LINEAR_Y : PIC_NEAREST_Y);
			/* on the screen at once, before the other band is even begun */
			pic_push(what);
		}
		[samp release];
	}
out:
	free(band);
	[vpos release];
	[vuv release];
	[grad release];
	[ps release];
	pic_release_targets(bt, 4, brp);
	[vfn release];
	[ffn release];
	pic_step("done");
}

/* What the two bands must hold, and a handful of pixels named one by one. */
static void
pic_count_bands(void)
{
	unsigned long bad_nearest = 0, bad_centres = 0;

	/*
	 * THE TEXEL ROW IS COUNTED FROM THE BOTTOM, and that is Metal's orientation rather
	 * than a workaround: the viewport puts clip y = +1 in memory row 0 (10.28), the
	 * corner that lands there carries v = 1, so band row r shows texel row
	 * PIC_TEX-1 - r/PIC_SY where it used to show r/PIC_SY. The quad and its
	 * coordinates are unchanged on purpose -- see the note in pic_bands.
	 *
	 * The x axis is untouched. Nothing in the viewport's y sign is a horizontal
	 * statement, which is also why the linear/nearest contrast at column 134 below
	 * reads exactly as it did before the sign went in.
	 */
	for (unsigned q = 0; q < PIC_BAND_H; q++) {
		for (unsigned p = 0; p < PANEL_W; p++) {
			unsigned char want[4];
			const unsigned char *g = pic_panel +
			    ((size_t)(PIC_NEAREST_Y + q) * PANEL_W + p) * 4;

			pic_texel_colour(p / PIC_SX, PIC_TEX - 1 - q / PIC_SY, want);
			if (g[0] != want[0] || g[1] != want[1] || g[2] != want[2]) {
				bad_nearest++;
			}
		}
	}
	for (unsigned j = 0; j < PIC_TEX; j++) {
		for (unsigned i = 0; i < PIC_TEX; i++) {
			unsigned px = PIC_SX * i + (PIC_SX - 1) / 2;
			/* texel row j is now at band row PIC_TEX-1-j */
			unsigned py = PIC_SY * (PIC_TEX - 1 - j) + (PIC_SY - 1) / 2;
			unsigned char want[4];
			const unsigned char *g = pic_panel +
			    ((size_t)(PIC_LINEAR_Y + py) * PANEL_W + px) * 4;

			pic_texel_colour(i, j, want);
			if (g[0] != want[0] || g[1] != want[1] || g[2] != want[2]) {
				if (bad_centres < 6) {
					printf("drvtest: picture: linear texel centre (%u,%u) "
					       "at panel (%u,%u) is r g b %u %u %u and its "
					       "texel is %u %u %u\n", i, j, px,
					       PIC_LINEAR_Y + py, g[2], g[1], g[0], want[2],
					       want[1], want[0]);
				}
				bad_centres++;
			}
		}
	}
	printf("drvtest: picture: nearest band: %lu pixels of %u are not the texel the "
	       "sampler must have returned\n", bad_nearest, PANEL_W * PIC_BAND_H);
	printf("drvtest: picture: linear band: %lu of %u texel centres are not their texel\n",
	       bad_centres, PIC_TEX * PIC_TEX);

	/* The one place the two bands must differ, and the whole of what says the sampler
	 * state reached the hardware: halfway between texel 0 and texel 1 along a row of
	 * texel centres. Nearest can only answer one of the two; linear must answer
	 * between them, and softpipe through this same library answers 18 there.
	 *
	 * Unmoved by the viewport's y sign, and that is the point: this reads the RED
	 * channel, which pic_texel_colour makes a function of x alone, so it is a horizontal
	 * statement and a vertical mirror cannot change it. If this pair ever moves with a
	 * y sign, the cause is not the y sign. */
	unsigned x = PIC_SX - 1, y = (PIC_SY - 1) / 2;
	const unsigned char *l = pic_panel + ((size_t)(PIC_LINEAR_Y + y) * PANEL_W + x) * 4;
	const unsigned char *n = pic_panel + ((size_t)(PIC_NEAREST_Y + y) * PANEL_W + x) * 4;

	printf("drvtest: picture: at column %u, row %u of each band: linear red %u, nearest "
	       "red %u (texel 0 is %u, texel 1 is %u; softpipe gives 18 and 0)\n", x, y,
	       l[2], n[2], pic_ramp[0], pic_ramp[1]);
	printf("drvtest: picture: the sampler %s\n",
	       l[2] > pic_ramp[0] && l[2] < pic_ramp[1] && n[2] == pic_ramp[0] ?
	       "FILTERS: the linear band is between two texels where the nearest one is on "
	       "one of them" :
	       "did not do that, and the two bands are not telling two things apart");

	/* And the orientation itself, as two named pixels rather than as a count: "0 of
	 * 699840" says the band matched its expectation, whatever that expectation was, and
	 * cannot say which way up anything is. With Metal's rule the top of the band is
	 * texel row PIC_TEX-1 and the bottom is texel row 0, so it is BLUE at the top and
	 * GREEN at the bottom; under the old sign it was the other way up. */
	{
		unsigned char top[4], bottom[4];
		const unsigned char *a = pic_panel +
		    ((size_t)(PIC_LINEAR_Y + (PIC_SY - 1) / 2) * PANEL_W +
		     (PIC_SX - 1) / 2) * 4;
		const unsigned char *b = pic_panel +
		    ((size_t)(PIC_LINEAR_Y + PIC_SY * (PIC_TEX - 1) + (PIC_SY - 1) / 2) *
		     PANEL_W + (PIC_SX - 1) / 2) * 4;

		pic_texel_colour(0, PIC_TEX - 1, top);
		pic_texel_colour(0, 0, bottom);
		printf("drvtest: picture: the band's top left is r g b %u %u %u (texel (0,%u), "
		       "must be %u %u %u) and its bottom left is %u %u %u (texel (0,0), must "
		       "be %u %u %u): clip y = +1 is memory row 0\n", a[2], a[1], a[0],
		       PIC_TEX - 1, top[2], top[1], top[0], b[2], b[1], b[0], bottom[2],
		       bottom[1], bottom[0]);
	}

	/* and the four corner squares of the mosaic, which has no filtering in it at all:
	 * what a person can check against the picture by eye */
	static const unsigned corner[4][2] = { { 0, 0 }, { 7, 0 }, { 0, 7 }, { 7, 7 } };

	for (int c = 0; c < 4; c++) {
		unsigned i = corner[c][0], j = corner[c][1];
		unsigned px = i * PIC_TILE_W + PIC_TILE_W / 2;
		unsigned py = j * PIC_TILE_H + PIC_TILE_H / 2;
		unsigned char want[4];
		const unsigned char *g = pic_panel + ((size_t)py * PANEL_W + px) * 4;

		pic_mosaic_colour(i, j, want);
		printf("drvtest: picture: clearFragment%02X at panel (%u,%u) is r g b %u %u %u "
		       "and must be %u %u %u: %s\n", 4 * (j * PIC_TILES + i) + 1, px, py,
		       g[2], g[1], g[0], want[2], want[1], want[0],
		       g[0] == want[0] && g[1] == want[1] && g[2] == want[2] ? "the same"
									    : "DIFFERENT");
	}
	fflush(stdout);
}

/*
 * And then keep repainting it, for as long as the caller asked.
 *
 * The repainting is not what puts the picture there -- pic_push already did that after
 * every stage, and the copy stays in the boot framebuffer by itself. It is for the boot
 * where a daemon of Apple's IS running and draws over the panel between one screenshot
 * and the next: five times a second beats a window server that swaps occasionally. With
 * JOBS="" nothing of this is needed and the first push would have been enough.
 */
/*
 * --stop-compositor (M5c, for a screenshot the owner asked for): the window server stopped while the
 * picture is held. Five repaints a second do not beat a compositor drawing a clock's second hand, and
 * run 123748's shot had the picture in a band across the home screen. SIGSTOP and SIGCONT, around
 * this mode only, on a RAM boot that is about to end.
 */
static bool pic_stop_compositor;

static pid_t
pic_find_process(const char *name)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
	size_t size = 0;

	if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0) {
		return -1;
	}
	size += 16 * sizeof(struct kinfo_proc);
	struct kinfo_proc *all = malloc(size);
	pid_t found = -1;

	if (all != NULL && sysctl(mib, 4, all, &size, NULL, 0) == 0) {
		for (size_t i = 0; i < size / sizeof(struct kinfo_proc); i++) {
			if (strcmp(all[i].kp_proc.p_comm, name) == 0) {
				found = all[i].kp_proc.p_pid;
			}
		}
	}
	free(all);
	return found;
}

static void
pic_show(int seconds)
{
	if (pic_connection == 0) {
		return;
	}
	const pid_t compositor = pic_stop_compositor ? pic_find_process("backboardd") : -1;

	if (compositor > 0) {
		int stopped = kill(compositor, SIGSTOP);

		printf("drvtest: picture: backboardd (%d) stopped for the hold: %d\n", compositor, stopped);
	}
	/* "--picture 0" is still a picture: pic_push has already painted it once. */
	if (seconds < 1) {
		seconds = 1;
	}
	printf("drvtest: picture: repainting for %d seconds; the screenshot can be taken at "
	       "any time now and more than once\n", seconds);
	fflush(stdout);
	for (int i = 0; i < seconds * 5; i++) {
		struct pic_show_request show = {
			(uint64_t)(uintptr_t)pic_panel, PANEL_W, PANEL_H, PANEL_W * 4, 0
		};

		IOConnectCallStructMethod(pic_connection, 0x7f, &show, sizeof(show), NULL,
					  NULL);
		usleep(200000);
	}
	printf("drvtest: picture: done repainting after %d seconds\n", seconds);
	if (compositor > 0) {
		printf("drvtest: picture: backboardd continued: %d\n", kill(compositor, SIGCONT));
	}
	fflush(stdout);
}

static void
picture_path(id<MTLDevice> dev, const char *metallib, int seconds)
{
	NSData *data = [NSData dataWithContentsOfFile:
	    [NSString stringWithUTF8String:metallib]];

	if (data == nil) {
		printf("drvtest: picture: cannot read %s\n", metallib);
		return;
	}
	dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL,
						  DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	NSError *err = nil;
	id<MTLLibrary> library = [dev newLibraryWithData:dd error:&err];

	if (library == nil) {
		printf("drvtest: picture: newLibraryWithData: gave nil\n");
		return;
	}
	pic_panel = calloc(1, (size_t)PANEL_W * PANEL_H * 4);
	if (pic_panel == NULL) {
		printf("drvtest: picture: no memory for %ux%u pixels\n", PANEL_W, PANEL_H);
		return;
	}
	printf("drvtest: picture: %s; the panel is %ux%u, the mosaic %ux%u tiles of %ux%u, "
	       "each band %ux%u\n", metallib, PANEL_W, PANEL_H, PIC_TILES, PIC_TILES,
	       PIC_TILE_W, PIC_TILE_H, PANEL_W, PIC_BAND_H);
	fflush(stdout);

	id<MTLCommandQueue> queue = [dev newCommandQueue];

	/* The panel BEFORE anything is drawn, so that a stage which finishes can be shown
	 * the moment it finishes. The first boot of this mode drew the whole mosaic
	 * correctly and then stopped on its way into the bands, and because the only push
	 * was at the very end the screenshot caught the loader's splash: a verified picture
	 * existed in this process's memory and never reached the screen. */
	pic_open_panel();

	pic_mosaic(dev, library, queue);
	pic_push("the mosaic -- 64 of Apple's fragment shaders, drawn on the Adreno");
	printf("drvtest: picture: the mosaic is done; now the ladder, then the two bands\n");
	fflush(stdout);
	/* From here on a second thread says, every so often, that this process is alive and
	 * which step it is in -- so the next silence means something definite. */
	pic_watch();
	pic_ladder(dev, library, queue);
	pic_bands(dev, library, queue);
	pic_alive = 0;
	pic_count_bands();
	pic_show(seconds);
}

/*
 * ==========================================================================================
 * --time: WHAT THIS DRIVER COSTS, ON THE PHONE, IN MICROSECONDS
 * ==========================================================================================
 *
 * Everything above this line answers "is it right". Nothing above it has ever answered "how
 * long" -- not a dispatch, not a draw, not a frame -- and the two questions fail differently:
 * a wrong draw says so in a pixel, a slow draw says nothing at all. The acceptance test for
 * this driver is a game, and a game is bought with time, so this mode measures the four
 * things a frame is actually made of and prints each with what would make it wrong.
 *
 * WHERE THE TIME GOES HERE, AND WHY IT IS NOT WHERE AN APPLICATION EXPECTS IT.
 *
 * -[HKMetalCommandBuffer commit] does the work (docs/metal.md 10.13). The encoder only
 * RECORDS; commit walks the recorded list calling rwa_render_draw / rwa_dispatch, and each of
 * those flushes the Gallium context and WAITS ON ITS OWN FENCE before returning
 * (tools/mesa/air/air_metal.c, finish_or_say_why with required = true). Three consequences,
 * and the mode is built around them:
 *
 *   1. encoding is nearly free and measures nothing an application cares about;
 *   2. -commit is the whole frame -- our translation, the submission, the GPU's execution and
 *      the wait for it -- one draw after another, with NO overlap between processor and GPU.
 *      So a frame's cost is the SUM of its draws and not the maximum of two pipelines;
 *   3. -waitUntilCompleted has nothing left to wait for and must measure ~0. That is not a
 *      hole to paper over: it is the check that the split is where this comment says it is,
 *      which is why it is measured and printed instead of assumed. A -waitUntilCompleted that
 *      is NOT ~0 means this comment is out of date and every other number here is mis-split.
 *
 * [2026-11-05, M4: THIS COMMENT IS OUT OF DATE, ON BOTH BACK ENDS, and point 3 is how that
 * shows. Since 2026-10-23 (docs/air.md 9.3) hkmetal.m asks the compiler library for
 * rwa_device_defer_waits(1) unless the boot has metal_sync=1, and both libraries honour it:
 * on Gallium (air_metal.c) rwa_render_draw and rwa_dispatch_buffers record into freedreno's
 * batch and return without finish_or_say_why, and rwa_flush at the end of -commit flushes the
 * context once and hands the fence back; on Vulkan (air_vk.c) every draw and dispatch records
 * into the device's one VkCommandBuffer and rwa_flush submits it once with its own VkFence
 * (ONE COMMAND BUFFER AT A TIME there). On both, -waitUntilCompleted waits on that fence
 * (hkmetal.m -hkWaitFence). So by default, on either back end: (1) encoding still only
 * records; (2) -commit is our translation and the submission -- NOT the GPU's execution and
 * not the wait, so the processor and the GPU do overlap across command buffers; (3)
 * -waitUntilCompleted carries the GPU's time and is NOT ~0 (971 us after a one-draw
 * 1080x2400 -commit, docs/air.md 9.3). The measurements below are kept exactly as they were
 * -- same cases, same clock, same brackets -- and are to be read that way: -commit as the
 * processor's side and -waitUntilCompleted as the GPU's. Three of the mode's own derived
 * lines assume the old split and are wrong under the default: "THE SPLIT" (tm_say_gpu_side)
 * subtracts only when the kernel timed about as many submissions as the case made DRAWS,
 * which a batched command buffer no longer is, so it says "NOT the same work" there;
 * tm_submissions counts draws, not submissions; and tm_verdict's frame arithmetic is built
 * from -commit medians, so it prices the processor's side of a frame only. With metal_sync=1
 * all of the original text holds. M4's cost line at the end of the mode (cost_end) has the
 * kernel's own submission count and the core's time for the whole mode beside it.]
 *
 * ONE DRAW IS ONE RENDER PASS IS ONE SUBMISSION. rwa_render_draw creates the surfaces, sets
 * the framebuffer, clears, draws, flushes and fences, then tears the framebuffer down again
 * (air_metal.c around 4124-4346). So eight Metal draws into one encoder are eight kernel
 * submissions and eight passes over the attachment, not one -- which is exactly why this mode
 * times one draw per command buffer AND eight, and prints the marginal cost of a draw
 * separately from the cost of a command buffer.
 * [M4: still eight passes, on both back ends (air_vk.c begins a rendering per draw too), but
 * no longer eight submissions by default: the eight are recorded and go to the kernel at the
 * command buffer's one rwa_flush, as one submission on Vulkan and in one flush on Gallium
 * (docs/air.md 9.3: "the 8 draws go to the kernel in one flush instead of eight"). The
 * marginal cost of a draw is still what the batched case prices, on the processor's side in
 * -commit and on the GPU's in -waitUntilCompleted.]
 *
 * WHAT IS MEASURED, AND WHY THESE AND NOT OTHERS
 *
 *   the clock itself      every number below is this clock's; printed first so that nothing
 *                         within a few times its floor is read as a measurement.
 *   a pipeline state      an AIR -> NIR -> ir3 compile inside -newRenderPipelineStateWith-
 *                         Descriptor:, with NO cache anywhere (air_metal.c: "One compiled
 *                         state, not a cache"). Measured cold (the process's first, which also
 *                         pays for whatever the compiler sets up once) and warm, the same
 *                         function again and then different functions -- because if the same
 *                         function is much cheaper the second time, something IS caching and
 *                         the warm number is not a compile.
 *   a draw                at three sizes: 4x4 (every earlier number in this file), 1080x648
 *                         (the picture's band, so this is comparable with logs/140340) and
 *                         1080x2400, the panel. A 4x4 quad says nothing about a frame: that is
 *                         the whole reason the big sizes are here.
 *   with and without the clear   the same attachments drawn through two render passes that
 *                         differ in ONE field, loadAction. Every draw in a pass whose
 *                         loadAction is Clear re-clears the attachment in this driver
 *                         (hkmetal.m copies _clears into every recorded draw), and an
 *                         application clears once a frame, so the two have to be told apart
 *                         before a per-draw cost means anything.
 *   a texture upload      -replaceRegion: of the whole attachment-sized image, in MB/s: what
 *                         streaming a texture costs.
 *   a readback            -getBytes: of the same. No game does this; it is here because every
 *                         verifying mode in this file does it, so its cost has to be
 *                         attributable and NOT mistaken for the draw's.
 *   command buffers/s     how many the queue sustains with no work in them at all: the floor
 *                         under any frame rate, and the one number that would catch a
 *                         regression of the 64-slot semaphore bug (10.33) as a slowdown
 *                         rather than as a hang.
 *   the present           -getBytes: of the finished attachment plus the kernel's kHKShow
 *                         row copy. NOT Metal's, and said so where it is printed -- but a
 *                         frame that is never shown is not a frame, and this is the only way
 *                         this port has to show one, so a frame rate that leaves it out is a
 *                         frame rate for a game nobody can see.
 *
 * HOW OUR COST IS SEPARATED FROM THE GPU's, AND WHERE IT CANNOT BE
 *
 * Three ways, in order of how much they can be trusted. The first did not exist when this mode
 * was written and was added because the other two were not enough (docs/metal.md 10.43):
 *
 *   (0) ASK THE KERNEL. Request kHKGPUWorked (0x7d) on HKGPU answers
 *       hk_gpu_worked()'s three numbers -- the microseconds the 3D core worked, on how
 *       many submissions, and the histogram -- taken off CP_ALWAYS_ON_COUNTER. Reading resets
 *       them, so each case is bracketed by two reads and gets the core's own time for its own
 *       draws. An older kernel answers kIOReturnUnsupported and the mode says in words that
 *       the GPU's side is not measured, rather than printing a zero.
 *
 * and the two that were there first, which still run because they need nothing of the kernel:
 *
 *   (a) BY SUBTRACTION, in this process, no boot argument needed. The 4x4 case and the
 *       1080x2400 case emit the same commands, bind the same state and make the same number
 *       of submissions; what differs is the number of pixels. So the 4x4 per-draw median is
 *       our cost plus the round trip, and the increase at the bigger sizes is the GPU's --
 *       its fill and the bandwidth of one pass over the attachment. This attributes nothing
 *       it cannot see: it says "fixed" and "proportional to area", not "ours" and "theirs".
 *   (b) BY THE KERNEL'S OWN COUNTER, which needs the boot argument gpu_log=1 and a log to
 *       read afterwards. bsd/dev/hk_adreno.c times every submission off the GPU's
 *       always-on counter and writes "tl: q <fence> took <us>" per submission; HKGPU.cpp
 *       prints, every five seconds in which something was submitted, "the GPU worked N us on
 *       each of M" with a histogram. Those lines land in the same run log as these, so the
 *       reader subtracts. This mode cannot print them itself and does not pretend to: what it
 *       prints instead is HOW MANY submissions each case made, so the kernel's M can be
 *       matched to a case instead of guessed at.
 *
 * And one number that cannot be split at all, said so rather than attributed: the cold
 * pipeline. It contains the compiler's one-time setup and one compile and nothing separates
 * them from inside the process; what can be said is that the second compile no longer pays
 * whatever the first did, and the difference between the two is printed.
 */

enum {
	TM_MAX_SAMPLES = 256,   /* kept per measurement; more are counted and not kept */
	TM_BATCH = 8,           /* draws in the batched command buffer */
};

struct tm_run {
	unsigned n;             /* taken, which may exceed what was kept */
	uint64_t ns[TM_MAX_SAMPLES];
};

static unsigned tm_repeats = 32;
static uint64_t tm_main_entered;        /* the first statement of main() */
static uint64_t tm_mode_entered;        /* the first statement of time_path() */
static uint64_t tm_first_pixel;         /* the first draw of this mode that the GPU finished */
static uint64_t tm_cold_pipeline_ns;    /* the process's first compile, which happens once */
/*
 * The four medians the frame-rate arithmetic at the end is made of, kept here because they are
 * measured in four different groups and the arithmetic has to be done where all four are
 * known. Each is zero until its group has run, and the arithmetic says so rather than
 * substituting a guess.
 */
static uint64_t tm_small_marginal_ns;   /* a further 4x4 draw in the same command buffer */
static uint64_t tm_panel_marginal_ns;   /* a further 1080x2400 draw, trivial fragment shader */
static uint64_t tm_panel_tex_ns;        /* ... and with Apple's textured blit */
static uint64_t tm_buffer_ns;           /* the command buffer itself, once a frame */
static uint64_t tm_warm_pipeline_ns;    /* one compile, warm, a shader not seen before */
static uint64_t tm_present_ns;          /* -getBytes: the panel plus the kernel's copy */
static unsigned long tm_submissions;    /* draws this mode asked the GPU for, for the kernel's count */

/*
 * CLOCK_MONOTONIC and not pic_now_ms's timebase arithmetic, for one reason: pic_now_ms
 * divides down to milliseconds and almost everything measured here is smaller than its
 * resolution -- logs/140340-run.log says "the step before took 0 ms" for a whole 1080x648
 * textured draw, which is the exact shape of a measurement that is not one.
 */
static uint64_t
tm_now_ns(void)
{
	struct timespec t;

	if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
		return 0;
	}
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void
tm_add(struct tm_run *r, uint64_t ns)
{
	if (r->n < TM_MAX_SAMPLES) {
		r->ns[r->n] = ns;
	}
	r->n++;
}

/*
 * One measurement, said in full.
 *
 * The MEDIAN leads, because one slow sample -- a page fault, a migration to another core, the
 * kernel's own five-second report landing in the middle of a draw -- moves a mean and cannot
 * move the median of thirty-two. The MAX is printed beside it so an outlier is seen rather
 * than smoothed away, and the FIRST sample is printed on its own because in this mode the
 * first of anything is a different measurement from the rest: the first pipeline pays for the
 * compiler, the first draw with a pipeline pays for ir3's program link (ir3_cache_lookup
 * happens at draw time, not at pipeline creation), and the first touch of a buffer pays for
 * its pages.
 */
static void
tm_say(const char *what, const struct tm_run *r, const char *wrong)
{
	uint64_t s[TM_MAX_SAMPLES];
	unsigned n = r->n < TM_MAX_SAMPLES ? r->n : TM_MAX_SAMPLES;
	uint64_t total = 0;

	if (n == 0) {
		printf("drvtest: time: %-44s NOT MEASURED (no sample was taken)\n", what);
		fflush(stdout);
		return;
	}
	memcpy(s, r->ns, (size_t)n * sizeof(s[0]));
	for (unsigned i = 1; i < n; i++) {      /* insertion sort; n is at most 256 */
		uint64_t v = s[i];
		unsigned j = i;

		while (j > 0 && s[j - 1] > v) {
			s[j] = s[j - 1];
			j--;
		}
		s[j] = v;
	}
	for (unsigned i = 0; i < n; i++) {
		total += s[i];
	}
	printf("drvtest: time: %-44s n %3u  first %9.1f us  median %9.1f  min %9.1f  "
	       "max %9.1f  mean %9.1f\n", what, n, r->ns[0] / 1000.0, s[n / 2] / 1000.0,
	       s[0] / 1000.0, s[n - 1] / 1000.0, (double)total / 1000.0 / n);
	if (wrong != NULL) {
		printf("drvtest: time:    ^ wrong if: %s\n", wrong);
	}
	fflush(stdout);
}

/* The median of what was kept, for the arithmetic this mode does on its own numbers. */
static uint64_t
tm_median(const struct tm_run *r)
{
	uint64_t s[TM_MAX_SAMPLES];
	unsigned n = r->n < TM_MAX_SAMPLES ? r->n : TM_MAX_SAMPLES;

	if (n == 0) {
		return 0;
	}
	memcpy(s, r->ns, (size_t)n * sizeof(s[0]));
	for (unsigned i = 1; i < n; i++) {
		uint64_t v = s[i];
		unsigned j = i;

		while (j > 0 && s[j - 1] > v) {
			s[j] = s[j - 1];
			j--;
		}
		s[j] = v;
	}
	return s[n / 2];
}

/* The same, plus what it is in megabytes a second -- which is the only form in which a copy
 * of ten megabytes and a copy of two hundred bytes can be compared. */
static void
tm_say_rate(const char *what, const struct tm_run *r, size_t bytes, const char *wrong)
{
	uint64_t median = tm_median(r);

	tm_say(what, r, wrong);
	if (median != 0) {
		printf("drvtest: time:    ^ %zu bytes at the median: %.1f MB/s\n", bytes,
		       (double)bytes * 1000.0 / (double)median);
		fflush(stdout);
	}
}

/*
 * The floor under every number in this mode.
 *
 * Two back-to-back reads of the clock, a thousand times: the smallest non-zero difference is
 * the clock's own granularity, and the median difference is what one reading costs. A
 * measurement of a few times either is noise wearing a number's clothes, and this is printed
 * FIRST so that no one has to take the rest on trust.
 */
static void
tm_clock_floor(void)
{
	struct tm_run gap = { 0 };
	uint64_t smallest = ~0ull;

	for (unsigned i = 0; i < 1000; i++) {
		uint64_t a = tm_now_ns(), b = tm_now_ns();

		if (b > a) {
			tm_add(&gap, b - a);
			if (b - a < smallest) {
				smallest = b - a;
			}
		}
	}
	if (gap.n == 0) {
		printf("drvtest: time: the clock never moved between two reads: either "
		       "CLOCK_MONOTONIC is not implemented here or its resolution is coarser "
		       "than this loop, and NOTHING below can be believed\n");
		fflush(stdout);
		return;
	}
	/*
	 * Said this way after the first run said it the wrong way. The median of the NON-ZERO
	 * gaps is one quantum by construction -- every non-zero gap is one tick -- so printing
	 * it as "one reading costs that much" was a wrong sentence beside a right number
	 * (logs/225409-run.log: "the smallest step is 1000 ns and one reading costs about
	 * 1000 ns"). What the fraction of pairs that moved actually gives is the cost of a
	 * reading: if 44 of 1000 pairs crossed a 1000 ns boundary, a pair takes about
	 * 0.044 x 1000 ns.
	 */
	printf("drvtest: time: the clock: it ticks in steps of %llu ns, and %u of 1000 pairs of "
	       "back-to-back reads fell either side of a tick -- so one reading costs about "
	       "%llu ns and every single number below carries +/-%llu ns of quantisation, which "
	       "a median over %u repeats averages away but a lone sample does not\n",
	       (unsigned long long)smallest, gap.n,
	       (unsigned long long)(smallest * gap.n / 1000 / 2), (unsigned long long)smallest,
	       tm_repeats);
	fflush(stdout);
}

/*
 * One render pipeline, timed.
 *
 * The descriptor is built OUTSIDE the timed region on purpose: setting four pixel formats on
 * a Metal descriptor is Metal's own bookkeeping and has nothing to do with the compile, and
 * folding it in would quietly add a constant to every compile number here.
 *
 * A refusal is a number too -- how long the library took to decide -- so it is returned with
 * its reason instead of printed, and the caller keeps refusals out of the compiled ones'
 * statistics. A refused pipeline counted as a compile would make the driver look faster the
 * more shaders it cannot translate.
 */
static id<MTLRenderPipelineState>
tm_make_pipeline(id<MTLDevice> dev, id<MTLFunction> vertex, id<MTLFunction> fragment,
		 unsigned targets, uint64_t *ns, char *why, size_t size)
{
	MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
	NSError *err = nil;

	[pd setVertexFunction:vertex];
	[pd setFragmentFunction:fragment];
	for (unsigned i = 0; i < targets; i++) {
		[[[pd colorAttachments] objectAtIndexedSubscript:i]
		    setPixelFormat:MTLPixelFormatBGRA8Unorm];
	}
	uint64_t t0 = tm_now_ns();
	id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd
									    error:&err];
	uint64_t t1 = tm_now_ns();

	*ns = t1 - t0;
	cost_pipeline_add(*ns);         /* the whole mode's pipelines field (M4, cost_end) */
	[pd release];
	if (ps == nil && why != NULL) {
		snprintf(why, size, "%s", err != nil ?
		    [[err localizedDescription] UTF8String] : "(no reason)");
	}
	return ps;
}

/*
 * THE GPU'S OWN SIDE, ASKED OF THE KERNEL INSTEAD OF READ OUT OF THE LOG.
 *
 * The first run of this mode could only subtract one case from another and point at the
 * kernel's `tl: q <fence> took <us>` lines for the rest -- and that turned out to be the
 * weakest part of it: those lines number every submission on the machine, this test is not the
 * only thing submitting, and aligning 1285 of them to four cases by eye is exactly the kind of
 * arithmetic that produces a confident wrong answer (docs/metal.md 10.43 shows the three
 * populations it did produce, and the ambiguity in their boundaries).
 *
 * So the kernel now hands the numbers over: request kHKGPUWorked (0x7d) on HKGPU
 * answers hk_gpu_worked()'s three outputs -- the microseconds the 3D core itself worked,
 * on how many submissions, and how many of those were under 0.1, 0.25, 0.5, 1, 2 and 4 ms --
 * measured off CP_ALWAYS_ON_COUNTER at 19.2 ticks a microsecond (bsd/dev/hk_adreno.c).
 * Reading it RESETS it, which is what makes bracketing a case possible: read once to zero,
 * draw, read again.
 *
 * Two things this must never do, and both are why it is written this way:
 *
 *   - IT MUST BE OPTIONAL. An older kernel answers 0x7d with "request 0x7d is not known here"
 *     and kIOReturnUnsupported, and a zero from that is indistinguishable from a GPU that did
 *     no work. So the first failure disables it for the run and says so in words; it never
 *     prints a zero that could be read as a measurement.
 *   - IT MUST SAY THAT IT TAKES THE NUMBERS AWAY. The kernel's own five-second report in
 *     HKGPU.cpp shares these counters, so a boot that runs this mode leaves gaps in those
 *     lines. That is expected rather than a fault, and it is printed once.
 */
struct tm_worked {
	uint64_t microseconds;
	uint32_t submissions;
	uint32_t by[7];
};

static int tm_worked_state;     /* 0 not tried, 1 answers, -1 this kernel has no such request */
/*
 * [M4: the request goes through cost_worked_since, which answers what it always answered --
 * the core's time since this function's previous read -- out of the process's running totals
 * (WHAT A WORKLOAD COSTS, near the top). The cost keeper reads 0x7d every 20 ms while the
 * whole mode is measured, and a raw read here would then get only the last 20 ms; out of the
 * totals it gets all of it, and a five-second report of the kernel's no longer takes a
 * case's counts either.]
 */
static struct cost_mark tm_worked_mark;
_Static_assert(sizeof(struct tm_worked) == sizeof(struct cost_worked),
	       "tm_worked and cost_worked are both HKGPU.cpp's struct hk_worked");

static bool
tm_gpu_worked(struct tm_worked *out)
{
	size_t size = sizeof(*out);

	if (tm_worked_state < 0 || pic_connection == 0) {
		return false;
	}
	kern_return_t kr = cost_worked_since(pic_connection, &tm_worked_mark,
					     (struct cost_worked *)out, &size);

	if (kr != KERN_SUCCESS || size < sizeof(*out)) {
		/* The two answers worth telling apart are printed as the number, because they
		 * mean different things: kIOReturnUnsupported (0xe00002c7) is a kernel without
		 * this request, kIOReturnNotReady (0xe00002d8) is this kernel with the Adreno
		 * not started -- and in the second case nothing else here would be measuring
		 * either. */
		printf("drvtest: time: this kernel does not report GPU time: request 0x7d "
		       "answered %#x and gave %zu of %zu bytes (0xe00002c7 is a kernel without "
		       "the request, 0xe00002d8 is one whose GPU is not started). The GPU's own "
		       "share is NOT measured in this run -- it is only in the kernel's own "
		       "\"tl: q <fence> took <us>\" lines, if this boot has gpu_log=1\n", kr,
		       size, sizeof(*out));
		fflush(stdout);
		tm_worked_state = -1;
		return false;
	}
	if (tm_worked_state == 0) {
		printf("drvtest: time: this kernel reports GPU time (request 0x7d), so each case "
		       "below is bracketed by a read of it. Reading RESETS the kernel's counters, "
		       "which it shares with its own five-second \"the GPU worked N us\" report -- "
		       "so that report will have gaps in this boot, and that is this mode's doing "
		       "and not a fault\n");
		fflush(stdout);
		tm_worked_state = 1;
	}
	return true;
}

/*
 * What the kernel says the GPU did, beside what we measured ourselves.
 *
 * `ours_ns` is the per-draw median this case measured and `draws` how many draws it made, so
 * the two can be compared per draw rather than in bulk. The subtraction is printed only when
 * the kernel timed about as many submissions as this case made: if the counts disagree,
 * something else was submitting and the remainder is not ours -- which is said instead of
 * subtracted.
 */
static void
tm_say_gpu_side(const char *what, const struct tm_worked *w, uint64_t ours_ns,
		unsigned long draws)
{
	if (w->submissions == 0) {
		printf("drvtest: time: %s: the kernel timed NO submission of this case, so its "
		       "GPU share is unknown here (the core finishes before its times are added "
		       "up, which happens at the next submission -- so a case this short can "
		       "leave none)\n", what);
		fflush(stdout);
		return;
	}
	double per = (double)w->microseconds / w->submissions;

	printf("drvtest: time: %s: the kernel says the 3D core itself worked %.1f us on each "
	       "of %u submissions (%u under 0.1 ms, %u under 0.25, %u under 0.5, %u under 1, "
	       "%u under 2, %u under 4, %u longer)\n", what, per, w->submissions, w->by[0],
	       w->by[1], w->by[2], w->by[3], w->by[4], w->by[5], w->by[6]);
	if (draws != 0 && w->submissions >= draws - draws / 8 &&
	    w->submissions <= draws + draws / 8 && ours_ns != 0) {
		double ours = ours_ns / 1000.0;

		printf("drvtest: time: %s: THE SPLIT: %.1f us a draw measured here, %.1f us of "
		       "it the core's own, %.1f us (%.0f%%) this side of the ioctl -- our "
		       "translation, the submission and the wait. The second number is the "
		       "GPU's clock and the first is this processor's, so the remainder carries "
		       "both their errors\n", what, ours, per, ours - per,
		       ours > 0 ? 100.0 * (ours - per) / ours : 0.0);
	} else if (draws != 0) {
		printf("drvtest: time: %s: this case made %lu draws and the kernel timed %u "
		       "submissions, so they are NOT the same work and nothing is subtracted "
		       "here\n", what, draws, w->submissions);
	}
	fflush(stdout);
}

/*
 * The attachments, and the TWO passes over them that differ in one field.
 *
 * Both descriptors hold the same textures; `*clear` loads by clearing and `*load` keeps what
 * is there. That is the only difference between them, which is what makes the difference
 * between their timings the cost of the clear and nothing else. (In this driver the clear is
 * attachment 0's alone -- hkmetal.m reads loadAction only for i == 0 -- so what the
 * difference prices is one full-attachment clear, however many targets are bound.)
 */
static bool
tm_targets(id<MTLDevice> dev, id<MTLTexture> *t, unsigned n, unsigned w, unsigned h,
	   MTLRenderPassDescriptor **clear, MTLRenderPassDescriptor **load)
{
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
							       width:w
							      height:h
							   mipmapped:NO];

	[td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];
	*clear = [[MTLRenderPassDescriptor renderPassDescriptor] retain];
	*load = [[MTLRenderPassDescriptor renderPassDescriptor] retain];
	for (unsigned i = 0; i < n; i++) {
		t[i] = [dev newTextureWithDescriptor:td];
		if (t[i] == nil) {
			printf("drvtest: time: attachment %u of %u at %ux%u could not be made, "
			       "so this size is NOT MEASURED (that is %.1f MB of attachment "
			       "asked for)\n", i, n, w, h,
			       (double)n * w * h * 4 / 1048576.0);
			fflush(stdout);
			return false;
		}
		for (int which = 0; which < 2; which++) {
			MTLRenderPassColorAttachmentDescriptor *a =
			    [[(which == 0 ? *clear : *load) colorAttachments]
				objectAtIndexedSubscript:i];

			[a setTexture:t[i]];
			[a setLoadAction:which == 0 ? MTLLoadActionClear : MTLLoadActionLoad];
			[a setClearColor:MTLClearColorMake(0, 0, 0, 1)];
			[a setStoreAction:MTLStoreActionStore];
		}
	}
	return true;
}

/* What one command buffer with `draws` draws in it cost, in four parts. */
struct tm_parts {
	uint64_t make;          /* -[queue commandBuffer] */
	uint64_t encode;        /* the encoder, the bindings, the draws, -endEncoding */
	uint64_t commit;        /* -commit: in THIS driver, all of the work and the fence */
	uint64_t wait;          /* -waitUntilCompleted: must be ~0 here, and that is the point */
};

/*
 * One command buffer, drawn and waited on, with each of the four parts timed separately.
 *
 * Returns false and says why when the GPU did not finish it: a draw that failed is instant,
 * and a fast number from a draw that did nothing is the one reading this mode must never
 * produce.
 */
static bool
tm_one_buffer(id<MTLCommandQueue> queue, MTLRenderPassDescriptor *rp,
	      id<MTLRenderPipelineState> ps, id<MTLBuffer> uniform, unsigned nuniform,
	      id<MTLBuffer> *vbuf, unsigned nvbuf, id<MTLTexture> tex, id<MTLSamplerState> samp,
	      unsigned draws, struct tm_parts *parts, const char *what)
{
	uint64_t t0 = tm_now_ns();
	id<MTLCommandBuffer> cmd = [queue commandBuffer];
	uint64_t t1 = tm_now_ns();

	if (cmd == nil) {
		printf("drvtest: time: %s: no command buffer\n", what);
		fflush(stdout);
		return false;
	}
	id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];

	if (enc == nil) {
		printf("drvtest: time: %s: no render encoder\n", what);
		fflush(stdout);
		return false;
	}
	[enc setRenderPipelineState:ps];
	for (unsigned i = 0; i < nuniform; i++) {
		[enc setFragmentBuffer:uniform offset:0 atIndex:i];
	}
	for (unsigned i = 0; i < nvbuf; i++) {
		[enc setVertexBuffer:vbuf[i] offset:0 atIndex:i];
	}
	if (tex != nil) {
		[enc setFragmentTexture:tex atIndex:0];
	}
	if (samp != nil) {
		[enc setFragmentSamplerState:samp atIndex:0];
	}
	for (unsigned i = 0; i < draws; i++) {
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	}
	[enc endEncoding];
	uint64_t t2 = tm_now_ns();

	[cmd commit];
	uint64_t t3 = tm_now_ns();

	[cmd waitUntilCompleted];
	uint64_t t4 = tm_now_ns();

	tm_submissions += draws;
	if (tm_first_pixel == 0) {
		tm_first_pixel = t4;
	}
	parts->make = t1 - t0;
	parts->encode = t2 - t1;
	parts->commit = t3 - t2;
	parts->wait = t4 - t3;
	if ([cmd status] != RW_CB_COMPLETED) {
		printf("drvtest: time: %s: status %lu%s%s -- this draw did NOT happen, so its "
		       "time is not a draw's\n", what, (unsigned long)[cmd status],
		       [cmd error] != nil ? ": " : "",
		       [cmd error] != nil ? [[[cmd error] localizedDescription] UTF8String]
					  : "");
		fflush(stdout);
		return false;
	}
	return true;
}

/*
 * THE COLD PATH, AND IT RUNS FIRST BECAUSE IT CANNOT BE REPEATED.
 *
 * One pipeline and one draw, in that order, before anything else in this mode has compiled
 * anything. This is the only measurement here that a process gets one sample of, and it is the
 * one a launching application pays: from the first statement of main() to a pixel the GPU has
 * actually written.
 *
 * What it does NOT include is said as plainly as what it does. The device, the Mesa screen and
 * the GPU context are opened in -initWithAcceleratorPort: (hkmetal.m calls
 * rwa_device_open there), which main() did above, so they are inside the "before this mode"
 * part and not inside the compile. And main()'s part also contains this test's own sweep of
 * every class of the driver, which no application does -- so the breakdown is printed in three
 * pieces rather than as one number that would flatter or damn the driver depending on which
 * piece a reader assumed.
 */
static void
tm_first_frame(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue)
{
	struct tm_run cold = { 0 }, first = { 0 };
	id<MTLTexture> t[1] = { nil };
	MTLRenderPassDescriptor *rp_clear = nil, *rp_load = nil;
	struct tm_parts parts = { 0, 0, 0, 0 };
	char why[512] = { 0 };
	uint64_t ns = 0;
	id<MTLFunction> fn = [library newFunctionWithName:@"clearFragment01"];

	if (fn == nil) {
		printf("drvtest: time: no clearFragment01 in this library, so the cold path is "
		       "NOT MEASURED here (every warm number below still stands)\n");
		fflush(stdout);
		return;
	}
	if (!tm_targets(dev, t, 1, 4, 4, &rp_clear, &rp_load)) {
		[fn release];
		return;
	}
	id<MTLRenderPipelineState> ps = tm_make_pipeline(dev, nil, fn, 1, &ns, why,
							 sizeof(why));

	tm_cold_pipeline_ns = ns;
	tm_add(&cold, ns);
	if (ps == nil) {
		printf("drvtest: time: the process's first pipeline was REFUSED after %.1f us: "
		       "%s\n", ns / 1000.0, why);
		fflush(stdout);
		goto out;
	}
	tm_say("a pipeline, COLD: the process's very first", &cold,
	       "something made a pipeline before this -- nothing above this mode does, so "
	       "that would mean main() changed");
	id<MTLBuffer> uniform = [dev newBufferWithLength:256 options:0];

	if (uniform != nil && [uniform contents] != NULL) {
		memset([uniform contents], 0, 256);
		((float *)[uniform contents])[3] = 1.0f;
	}
	/* nuniform 0 for a buffer that could not be made: binding nil sixteen times would make
	 * the library refuse the draw for a reason that is this harness's and not the GPU's. */
	if (tm_one_buffer(queue, rp_clear, ps, uniform, uniform != nil ? 16 : 0, NULL, 0, nil,
			  nil, 1, &parts, "the cold draw") && tm_first_pixel != 0) {
		tm_add(&first, parts.commit);
		tm_say("-commit, COLD: the first draw of the process (4x4)", &first,
		       "it is near the warm 4x4 number far below -- then ir3 linked this "
		       "program at pipeline creation after all, and not at the draw");
		printf("drvtest: time: from the first statement of main() to the first pixel "
		       "this GPU wrote: %.1f ms = %.1f ms before this mode (the device, the "
		       "Mesa screen, the sweep of every class -- the sweep is this test's and "
		       "no application does it) + %.1f ms this mode's library and panel + %.1f "
		       "ms the cold compile + %.1f ms the cold draw\n",
		       (tm_first_pixel - tm_main_entered) / 1000000.0,
		       (tm_mode_entered - tm_main_entered) / 1000000.0,
		       (double)(tm_first_pixel - tm_mode_entered - tm_cold_pipeline_ns -
				parts.commit) / 1000000.0,
		       tm_cold_pipeline_ns / 1000000.0, parts.commit / 1000000.0);
		fflush(stdout);
	} else {
		printf("drvtest: time: the cold draw did not finish, so the distance from "
		       "main() to a first pixel is NOT MEASURED in this run\n");
		fflush(stdout);
	}
	[uniform release];
out:
	[ps release];
	[fn release];
	[t[0] release];
	[rp_clear release];
	[rp_load release];
}

/*
 * The compiles.
 *
 * Four questions, in this order, because each one is the control for the next:
 *
 *   cold            measured above, in tm_first_frame, because a process has exactly one cold
 *                   sample and spending it inside a loop would waste it. It carries the
 *                   compiler's one-time setup and one compile, and nothing inside this process
 *                   can separate them -- said here rather than attributed.
 *   warm, same      the same function again, over and over. If this is much cheaper than
 *                   cold, the difference is the one-time setup. If it is cheaper than "warm,
 *                   different" below, something is caching compiled code and this number is
 *                   not a compile at all -- air_metal.c says there is no cache
 *                   ("One compiled state, not a cache"), and this is how that claim is
 *                   checked rather than quoted.
 *   warm, different a different function each time, so no cache can be hit. THIS is the
 *                   number that prices a game's shader load.
 *   Apple's vertex  the same again with a real vertex stage (texturedVertex2D) instead of the
 *                   one this driver generates for a fragment-only pipeline: two modules
 *                   through the front end instead of one.
 */
static void
tm_pipelines(id<MTLDevice> dev, id<MTLLibrary> library)
{
	struct tm_run same = { 0 }, different = { 0 }, refused = { 0 }, apple = { 0 };
	char why[512] = { 0 };
	uint64_t ns = 0;
	id<MTLFunction> one = [library newFunctionWithName:@"clearFragment01"];
	NSArray<NSString *> *names = [library functionNames];

	/* clearFragment01 is the shader 10.33 drew 64 times on this GPU, so a pipeline made
	 * from it is a pipeline whose correctness is already known. A library without it still
	 * gets measured, with the first fragment function it does have. */
	if (one == nil) {
		for (NSString *name in names) {
			id<MTLFunction> fn = [library newFunctionWithName:name];

			if (fn != nil && [fn functionType] == MTLFunctionTypeFragment) {
				one = fn;
				printf("drvtest: time: this library has no clearFragment01, so "
				       "the pipeline timings use %s\n", [name UTF8String]);
				break;
			}
			[fn release];
		}
	}
	if (one == nil) {
		printf("drvtest: time: this library has no fragment function at all, so no "
		       "pipeline is timed\n");
		fflush(stdout);
		return;
	}
	id<MTLRenderPipelineState> ps = tm_make_pipeline(dev, nil, one, 1, &ns, why,
							 sizeof(why));

	if (ps == nil) {
		printf("drvtest: time: a pipeline was REFUSED after %.1f us: %s. Nothing below "
		       "this line is a compile's cost\n", ns / 1000.0, why);
		fflush(stdout);
		return;
	}
	[ps release];
	tm_add(&same, ns);

	for (unsigned i = 0; i < tm_repeats; i++) {
		@autoreleasepool {
			ps = tm_make_pipeline(dev, nil, one, 1, &ns, why, sizeof(why));
			if (ps != nil) {
				tm_add(&same, ns);
				[ps release];
			}
		}
	}
	tm_say("a pipeline, warm, the SAME function again", &same,
	       "it is far below the next line -- then something caches compiled code and this "
	       "is a cache hit, not a compile");
	if (tm_cold_pipeline_ns != 0 && tm_median(&same) != 0) {
		printf("drvtest: time: the cold compile cost %.1f us and a warm one %.1f us at "
		       "the median, so whatever the compiler sets up once is worth %.1f us -- "
		       "paid by the first pipeline of a process and by no other\n",
		       tm_cold_pipeline_ns / 1000.0, tm_median(&same) / 1000.0,
		       ((double)tm_cold_pipeline_ns - (double)tm_median(&same)) / 1000.0);
		fflush(stdout);
	}

	unsigned taken = 0;

	for (NSString *name in names) {
		if (taken >= tm_repeats) {
			break;
		}
		@autoreleasepool {
			id<MTLFunction> fn = [library newFunctionWithName:name];

			if (fn == nil || [fn functionType] != MTLFunctionTypeFragment) {
				[fn release];
				continue;
			}
			id<MTLRenderPipelineState> p = tm_make_pipeline(dev, nil, fn, 1, &ns,
								       why, sizeof(why));

			if (p != nil) {
				tm_add(&different, ns);
				[p release];
				taken++;
			} else {
				tm_add(&refused, ns);
			}
			[fn release];
		}
	}
	tm_say("a pipeline, warm, a DIFFERENT function each time", &different,
	       "the functions are all alike -- these are this library's in file order, so a "
	       "library of one shape prices only that shape");
	tm_warm_pipeline_ns = tm_median(&different);
	tm_say("a pipeline the library REFUSED (how long to decide)", &refused,
	       "nothing was refused, in which case this line is absent and not zero");

	id<MTLFunction> vfn = [library newFunctionWithName:@"texturedVertex2D"];
	id<MTLFunction> ffn = [library newFunctionWithName:@"texturedFragment2DFloat"];

	if (vfn != nil && ffn != nil) {
		for (unsigned i = 0; i < tm_repeats; i++) {
			@autoreleasepool {
				id<MTLRenderPipelineState> p = tm_make_pipeline(dev, vfn, ffn, 1,
									       &ns, why,
									       sizeof(why));
				if (p != nil) {
					tm_add(&apple, ns);
					[p release];
				}
			}
		}
		tm_say("a pipeline with Apple's OWN vertex stage (2 modules)", &apple,
		       "it equals the generated-vertex line above -- then one of the two "
		       "modules is not being compiled");
	} else {
		printf("drvtest: time: no texturedVertex2D/texturedFragment2DFloat in this "
		       "library, so the two-module compile is not timed\n");
	}
	[one release];
	[vfn release];
	[ffn release];
	fflush(stdout);
}

/*
 * One size of draw, measured five ways.
 *
 * `targets` attachments of `w` by `h`, the pipeline from clearFragment01 (whose vertex stage
 * this driver generates, so the quad covers the whole attachment -- 10.33) or from Apple's
 * textured blit, and then:
 *
 *   the first draw          with a pipeline nothing has drawn with yet, on its own. ir3 links
 *                           its program at DRAW time (ir3_cache_lookup), not at pipeline
 *                           creation, so the first draw of a pipeline is a different number
 *                           from the rest and a median that swallowed it would hide a cost a
 *                           game pays once per material.
 *   one draw, clearing      the whole round trip as every mode in this file has done it.
 *   one draw, NOT clearing  the same attachments through a pass whose loadAction is Load.
 *   TM_BATCH draws, one buffer  the marginal cost of a draw, with the command buffer paid for
 *                           once. In this driver each of them is still its own submission and
 *                           its own pass, which is exactly what this is here to price.
 *   the four parts          -commandBuffer, the encoding, -commit and -waitUntilCompleted,
 *                           each on its own, from the same runs.
 *
 * and then ONE PIXEL is read back and compared with what the shader was told to write,
 * because a draw that silently did nothing is the fastest draw there is.
 */
static void
tm_draw_case(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue,
	     unsigned targets, unsigned w, unsigned h, bool textured)
{
	id<MTLTexture> t[8] = { nil };
	MTLRenderPassDescriptor *rp_clear = nil, *rp_load = nil;
	id<MTLRenderPipelineState> ps = nil;
	id<MTLFunction> vfn = nil, ffn = nil;
	id<MTLBuffer> uniform = nil, vpos = nil, vuv = nil;
	id<MTLTexture> small = nil;
	id<MTLSamplerState> samp = nil;
	char what[160], why[512] = { 0 };
	struct tm_run clear1 = { 0 }, load1 = { 0 }, loadN = { 0 }, make = { 0 },
		      encode = { 0 }, wait = { 0 }, firstdraw = { 0 };
	struct tm_parts parts = { 0, 0, 0, 0 };
	struct tm_worked worked = { 0, 0, { 0 } };
	bool gpu_side = false;
	uint64_t ns = 0;
	unsigned long before = tm_submissions;
	/* declared here and not where they are first wanted: every failure below leaves by
	 * `goto out`, and a jump over an initialisation is the kind of thing that compiles
	 * on one compiler and not the next */
	id<MTLBuffer> vbuf[2] = { nil, nil };
	unsigned nvbuf = 0, nuniform = 0;
	unsigned char px[4] = { 0, 0, 0, 0 };
	char label[192];
	/* the colour clearFragment01 is told to write, distinct from the clear (black) and from
	 * anything a wrong texel could be */
	static const unsigned char want[4] = { 109, 182, 73, 255 };   /* b g r a */
	static const float quad[4][4] = {
		{ -1, -1, 0, 1 }, { 1, -1, 0, 1 }, { -1, 1, 0, 1 }, { 1, 1, 0, 1 },
	};
	static const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
	static float texels[PIC_TEX * PIC_TEX * 4];

	snprintf(what, sizeof(what), "%s %ux%u x%u", textured ? "Apple's blit at" : "a draw at",
		 w, h, targets);
	printf("drvtest: time: ---- %s (%.1f MB of attachment) ----\n", what,
	       (double)targets * w * h * 4 / 1048576.0);
	fflush(stdout);
	/* The functions first and the attachments after: a library without them (AV1SW's
	 * eleven kernels, say) would otherwise allocate 41 MB of render target before finding
	 * out it has nothing to draw with. */
	if (textured) {
		vfn = [library newFunctionWithName:@"texturedVertex2D"];
		ffn = [library newFunctionWithName:@"texturedFragment2DFloat"];
	} else {
		ffn = [library newFunctionWithName:@"clearFragment01"];
	}
	if (ffn == nil || (textured && vfn == nil)) {
		printf("drvtest: time: %s: the functions this case needs are not in this "
		       "library, so it is NOT MEASURED\n", what);
		goto out;
	}
	if (!tm_targets(dev, t, targets, w, h, &rp_clear, &rp_load)) {
		goto out;
	}
	ps = tm_make_pipeline(dev, vfn, ffn, targets, &ns, why, sizeof(why));
	if (ps == nil) {
		printf("drvtest: time: %s: no pipeline (%s), so this case is NOT MEASURED\n",
		       what, why);
		goto out;
	}
	if (textured) {
		/* Apple's blit reads a position and a texture coordinate per vertex and
		 * samples an 8x8 texture; the corners, the coordinates and the texels are the
		 * picture's own (pic_bands), so this draw and the one 10.33 photographed are
		 * the same draw at a different size. */
		MTLTextureDescriptor *gd =
		    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
			MTLPixelFormatRGBA32Float width:PIC_TEX height:PIC_TEX mipmapped:NO];

		[gd setUsage:MTLTextureUsageShaderRead];
		small = [dev newTextureWithDescriptor:gd];
		vpos = [dev newBufferWithLength:64 options:0];
		vuv = [dev newBufferWithLength:64 options:0];
		if (small == nil || vpos == nil || vuv == nil || [vpos contents] == NULL ||
		    [vuv contents] == NULL) {
			printf("drvtest: time: %s: no texture or vertex buffers\n", what);
			goto out;
		}
		for (unsigned y = 0; y < PIC_TEX; y++) {
			for (unsigned x = 0; x < PIC_TEX; x++) {
				unsigned char c[4];
				float *v = texels + (y * PIC_TEX + x) * 4;

				pic_texel_colour(x, y, c);
				v[0] = c[2] / 255.0f;
				v[1] = c[1] / 255.0f;
				v[2] = c[0] / 255.0f;
				v[3] = 1.0f;
			}
		}
		[small replaceRegion:MTLRegionMake2D(0, 0, PIC_TEX, PIC_TEX)
			 mipmapLevel:0
			   withBytes:texels
			 bytesPerRow:PIC_TEX * 4 * sizeof(float)];
		memcpy([vpos contents], quad, sizeof(quad));
		memcpy([vuv contents], uv, sizeof(uv));
		MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];

		[sd setMinFilter:MTLSamplerMinMagFilterNearest];
		[sd setMagFilter:MTLSamplerMinMagFilterNearest];
		[sd setSAddressMode:MTLSamplerAddressModeClampToEdge];
		[sd setTAddressMode:MTLSamplerAddressModeClampToEdge];
		samp = [dev newSamplerStateWithDescriptor:sd];
		[sd release];
	} else {
		/* The constant buffer every clearFragment reads, bound at every index up to 16
		 * for the mosaic's reason: the shader declares index 14 and a driver that bound
		 * only 0 would leave it reading nothing. */
		uniform = [dev newBufferWithLength:256 options:0];
		if (uniform == nil || [uniform contents] == NULL) {
			printf("drvtest: time: %s: no constant buffer\n", what);
			goto out;
		}
		float *u = [uniform contents];

		memset(u, 0, 256);
		u[0] = want[2] / 255.0f;
		u[1] = want[1] / 255.0f;
		u[2] = want[0] / 255.0f;
		u[3] = 1.0f;
	}
	vbuf[0] = vpos;
	vbuf[1] = vuv;
	nvbuf = textured ? 2 : 0;
	nuniform = textured ? 0 : 16;

	/* The first draw with this pipeline, alone and not in any median. */
	if (!tm_one_buffer(queue, rp_clear, ps, uniform, nuniform, vbuf, nvbuf, small, samp, 1,
			   &parts, what)) {
		goto out;
	}
	tm_add(&firstdraw, parts.commit);
	tm_say("-commit, the FIRST draw with a fresh pipeline", &firstdraw,
	       "the pipeline had drawn before -- it is made in this function and used here "
	       "first, so that would mean this file changed");

	for (unsigned i = 0; i < tm_repeats; i++) {
		@autoreleasepool {
			if (!tm_one_buffer(queue, rp_clear, ps, uniform, nuniform, vbuf, nvbuf,
					   small, samp, 1, &parts, what)) {
				goto out;
			}
			tm_add(&clear1, parts.commit);
			tm_add(&make, parts.make);
			tm_add(&encode, parts.encode);
			tm_add(&wait, parts.wait);
		}
	}
	for (unsigned i = 0; i < tm_repeats; i++) {
		@autoreleasepool {
			if (!tm_one_buffer(queue, rp_load, ps, uniform, nuniform, vbuf, nvbuf,
					   small, samp, 1, &parts, what)) {
				goto out;
			}
			tm_add(&load1, parts.commit);
		}
	}
	/*
	 * The batched loop is the one the kernel's counter is bracketed around, and not the
	 * other two: every one of its draws is a submission of exactly the same work, with no
	 * clear in it, so "per submission" on the kernel's side and "per draw" on ours mean the
	 * same thing. Bracketing the whole case would mix three different draws into one
	 * average and make the subtraction meaningless.
	 */
	gpu_side = tm_gpu_worked(&worked);        /* reads to zero; what it returns here is the past */
	for (unsigned i = 0; i < tm_repeats; i++) {
		@autoreleasepool {
			if (!tm_one_buffer(queue, rp_load, ps, uniform, nuniform, vbuf, nvbuf,
					   small, samp, TM_BATCH, &parts, what)) {
				goto out;
			}
			tm_add(&loadN, parts.commit);
		}
	}
	if (gpu_side) {
		gpu_side = tm_gpu_worked(&worked);
	}
	tm_say("-commit, 1 draw, the pass CLEARS", &clear1,
	       "the attachment is not actually cleared -- this driver reads loadAction only "
	       "for attachment 0, so with more targets this prices one clear and not four");
	tm_say("-commit, 1 draw, the pass KEEPS (no clear)", &load1,
	       "it is not below the clearing line -- then the clear costs nothing measurable "
	       "at this size, which is the expected answer at 4x4 and not at 1080x2400");
	snprintf(label, sizeof(label), "-commit, %u draws in ONE command buffer, no clear",
		 TM_BATCH);
	tm_say(label, &loadN,
	       "it is about equal to one draw -- then the draws are being merged somewhere, "
	       "which this driver does not do: each is its own submission");
	tm_say("-[queue commandBuffer] alone", &make,
	       "it grows through the run -- that is the queue's 64 slots filling, and the "
	       "last line of this mode is the test for it");
	tm_say("the encoding (encoder + bindings + draw + end)", &encode,
	       "it is anywhere near -commit -- in this driver encoding only records, so a "
	       "large number here means work moved and this comment is stale");
	tm_say("-waitUntilCompleted after that -commit", &wait,
	       "it is NOT ~0 -- then the fence is no longer paid inside -commit and every "
	       "split in this mode is wrong [M4: which is the default since 2026-10-23, a "
	       "fence per command buffer on both back ends: expected unless metal_sync=1]");

	uint64_t one = tm_median(&load1), many = tm_median(&loadN);

	if (gpu_side && many != 0) {
		tm_say_gpu_side(what, &worked, many / TM_BATCH,
				(unsigned long)tm_repeats * TM_BATCH);
	}

	if (one != 0 && many > one && TM_BATCH > 1) {
		double marginal = (double)(many - one) / (TM_BATCH - 1);

		printf("drvtest: time: %s: a draw on its own costs %.1f us and each further "
		       "draw in the same command buffer %.1f us, so the command buffer itself "
		       "is worth %.1f us\n", what, one / 1000.0, marginal / 1000.0,
		       (one - marginal) / 1000.0);
		printf("drvtest: time: %s: at %.1f us a draw, a 16.7 ms frame (60 Hz) holds "
		       "%.0f such draws and a 33.3 ms frame (30 Hz) holds %.0f\n", what,
		       marginal / 1000.0, 16700000.0 / marginal, 33300000.0 / marginal);
		if (w * h > 16) {
			/* Both, because the first run printed only the first and the two differ
			 * by the number of targets: 599 us per megapixel of PASS at 1080x648 x4
			 * is 150 us per megapixel actually written, and that is the figure
			 * comparable with the panel's single target. */
			printf("drvtest: time: %s: that is %.2f us per megapixel of the pass and "
			       "%.2f us per megapixel actually written (%u targets), fill and one "
			       "pass over it together; %.1f GB/s of attachment written\n", what,
			       marginal / 1000.0 / ((double)w * h / 1048576.0),
			       marginal / 1000.0 / ((double)w * h * targets / 1048576.0), targets,
			       (double)w * h * targets * 4.0 / marginal);
		}
		fflush(stdout);
		/* kept for the arithmetic at the end of the mode */
		if (w * h == 16 && !textured) {
			tm_small_marginal_ns = (uint64_t)marginal;
		}
		if (w == PANEL_W && h == PANEL_H) {
			if (textured) {
				tm_panel_tex_ns = (uint64_t)marginal;
			} else {
				tm_panel_marginal_ns = (uint64_t)marginal;
				tm_buffer_ns = one > (uint64_t)marginal ?
				    one - (uint64_t)marginal : 0;
			}
		}
	}

	/* And the pixel, because every number above would be better if the draw did nothing. */
	[t[0] getBytes:px
	   bytesPerRow:4
	    fromRegion:MTLRegionMake2D(0, 0, 1, 1)
	   mipmapLevel:0];
	if (textured) {
		/* The viewport puts clip y = +1 in memory row 0 (10.28), so pixel (0,0) is
		 * texel (0, PIC_TEX-1), and with a NEAREST sampler it is that texel exactly. */
		unsigned char expect[4];

		pic_texel_colour(0, PIC_TEX - 1, expect);
		printf("drvtest: time: %s: pixel (0,0) is r g b %u %u %u and texel "
		       "(0,%u) is %u %u %u -- %s\n", what, px[2], px[1], px[0], PIC_TEX - 1,
		       expect[2], expect[1], expect[0],
		       px[0] == expect[0] && px[1] == expect[1] && px[2] == expect[2] ?
		       "the same, so the timed draws drew" : "NOT THE SAME: the timings above "
		       "are of a draw that did something else");
	} else {
		printf("drvtest: time: %s: pixel (0,0) is r g b %u %u %u and its buffer named "
		       "%u %u %u -- %s\n", what, px[2], px[1], px[0], want[2], want[1], want[0],
		       px[0] == want[0] && px[1] == want[1] && px[2] == want[2] ?
		       "the same, so the timed draws drew" : "NOT THE SAME: the timings above "
		       "are of a draw that did something else");
	}
	printf("drvtest: time: %s: this case asked the GPU for %lu submissions (the kernel's "
	       "own count, with gpu_log=1, must agree)\n", what, tm_submissions - before);
	fflush(stdout);
out:
	[ps release];
	[vfn release];
	[ffn release];
	[uniform release];
	[vpos release];
	[vuv release];
	[small release];
	[samp release];
	for (unsigned i = 0; i < targets; i++) {
		[t[i] release];
	}
	[rp_clear release];
	[rp_load release];
}

/*
 * The compute round trip, which is the same question as the draw's asked of the other path.
 *
 * A dispatch goes through the same -commit and the same fence as a draw (hkmetal.m's
 * commit loop calls rwa_dispatch for it), so the difference between this number and the 4x4
 * draw's is the difference between the two paths' own work and nothing else -- which is the
 * only way to say whether the per-submission cost lives in the render path or under both.
 *
 * The grid is 4x4x1 in one threadgroup, air_run's own and 10.15's, so the kernel being timed
 * is doing the work it was already checked doing. Sixteen buffers of 4 KB are bound for
 * compute_path's reason: a kernel sees only what it declared, binding more is harmless and
 * binding fewer makes it read a slot nothing was bound to. A kernel that declares a texture
 * is refused by the library with its reason, which is printed and then the next kernel tried;
 * four are tried and no more, because a mode that spends a boot looking for a kernel it can
 * dispatch has stopped measuring anything.
 */
static void
tm_dispatch(id<MTLDevice> dev, id<MTLLibrary> library, id<MTLCommandQueue> queue)
{
	/*
	 * Twelve tries and not four, and textures bound as well as buffers, because the first
	 * run measured nothing here and said why: all four kernels it tried were refused with
	 * "writes texture 0 and 0 textures were bound" (logs/225409-run.log). That is a true
	 * sentence about this harness and nothing about the kernels, and AV1SW.metallib has
	 * eleven of them, so both limits were this test's own.
	 */
	enum { TM_DISPATCH_BUFFERS = 16, TM_DISPATCH_TRIES = 12 };
	struct tm_run commit = { 0 }, encode = { 0 }, wait = { 0 };
	id<MTLBuffer> bufs[TM_DISPATCH_BUFFERS] = { nil };
	id<MTLTexture> texs[TM_DISPATCH_BUFFERS] = { nil };
	id<MTLSamplerState> samp = nil;
	id<MTLComputePipelineState> ps = nil;
	NSString *chosen = nil;
	unsigned tried = 0, made = 0;

	for (int i = 0; i < TM_DISPATCH_BUFFERS; i++) {
		bufs[i] = [dev newBufferWithLength:4096 options:0];
		if (bufs[i] == nil) {
			printf("drvtest: time: no buffer for the dispatch, so the compute round "
			       "trip is NOT MEASURED\n");
			goto out;
		}
		if ([bufs[i] contents] != NULL) {
			memset([bufs[i] contents], 0, 4096);
		}
	}
	/*
	 * Textures and a sampler for the kernels that declare them, 64x64 and in the first
	 * format this hardware will give as an image a shader writes. Sixteen of each, for the
	 * buffers' reason: a kernel sees only what it declared, so binding more is harmless and
	 * binding fewer leaves it reading a slot nothing was bound to. A kernel whose
	 * declaration wants another element type is still refused BY NAME, and that refusal is
	 * printed -- which is the answer either way.
	 */
	{
		static const MTLPixelFormat want[] = {
			MTLPixelFormatRGBA32Float, MTLPixelFormatRGBA32Uint,
			MTLPixelFormatRGBA16Float, MTLPixelFormatR32Float,
			MTLPixelFormatRGBA8Unorm,
		};
		MTLTextureDescriptor *td = nil;

		for (size_t f = 0; f < sizeof(want) / sizeof(want[0]) && td == nil; f++) {
			MTLTextureDescriptor *try = [MTLTextureDescriptor
			    texture2DDescriptorWithPixelFormat:want[f] width:64 height:64
						     mipmapped:NO];

			[try setUsage:MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite];
			id<MTLTexture> probe = [dev newTextureWithDescriptor:try];

			if (probe != nil) {
				[probe release];
				td = try;
			}
		}
		for (int i = 0; td != nil && i < TM_DISPATCH_BUFFERS; i++) {
			texs[i] = [dev newTextureWithDescriptor:td];
			made += texs[i] != nil;
		}
		MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];

		samp = [dev newSamplerStateWithDescriptor:sd];
		[sd release];
		printf("drvtest: time: for the dispatch: %u of %u textures (64x64, format %lu, "
		       "read and written) and a sampler %s\n", made, TM_DISPATCH_BUFFERS,
		       td != nil ? (unsigned long)[td pixelFormat] : 0UL,
		       samp != nil ? "made" : "NOT made");
		fflush(stdout);
	}
	/*
	 * A kernel that compiles AND whose first dispatch finishes. Both are needed: 10.16
	 * measured nine kernels of which eight finished, and the ninth would otherwise be
	 * timed as a very fast failure.
	 */
	for (NSString *name in [library functionNames]) {
		if (tried >= TM_DISPATCH_TRIES || ps != nil) {
			break;
		}
		id<MTLFunction> fn = [library newFunctionWithName:name];

		if (fn == nil || [fn functionType] != MTLFunctionTypeKernel) {
			[fn release];
			continue;
		}
		tried++;
		NSError *err = nil;
		id<MTLComputePipelineState> candidate =
		    [dev newComputePipelineStateWithFunction:fn error:&err];

		if (candidate == nil) {
			printf("drvtest: time: %s got no compute pipeline: %s\n",
			       [name UTF8String], err != nil ?
			       [[err localizedDescription] UTF8String] : "(no reason)");
			[fn release];
			continue;
		}
		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];

		/* A nil encoder would make the command buffer record nothing, and a command
		 * buffer that recorded nothing COMPLETES -- so without this check an empty
		 * dispatch would be timed as a fast one. */
		if (enc == nil) {
			printf("drvtest: time: no compute encoder for %s, so it is not timed\n",
			       [name UTF8String]);
			[candidate release];
			[fn release];
			continue;
		}
		[enc setComputePipelineState:candidate];
		for (int i = 0; i < TM_DISPATCH_BUFFERS; i++) {
			[enc setBuffer:bufs[i] offset:0 atIndex:i];
			if (texs[i] != nil) {
				[enc setTexture:texs[i] atIndex:i];
			}
			if (samp != nil) {
				[enc setSamplerState:samp atIndex:i];
			}
		}
		[enc dispatchThreadgroups:MTLSizeMake(1, 1, 1)
		    threadsPerThreadgroup:MTLSizeMake(4, 4, 1)];
		[enc endEncoding];
		[cmd commit];
		[cmd waitUntilCompleted];
		tm_submissions++;
		if ([cmd status] == RW_CB_COMPLETED) {
			/*
			 * And whether anything was written, because a dispatch has no pixel to
			 * compare: all sixteen buffers were zeroed above, so a kernel that
			 * leaves them zero either writes nothing or wrote zeros, and that is
			 * worth saying out loud beside its time rather than assuming.
			 */
			unsigned long changed = 0;

			for (int i = 0; i < TM_DISPATCH_BUFFERS; i++) {
				const unsigned char *p = [bufs[i] contents];

				for (unsigned k = 0; p != NULL && k < 4096; k++) {
					changed += p[k] != 0;
				}
			}
			printf("drvtest: time: %s finished and %lu of %u bytes of the bound "
			       "buffers are no longer zero%s\n", [name UTF8String], changed,
			       4096 * TM_DISPATCH_BUFFERS,
			       changed == 0 ? " -- so its time below is of a dispatch that may "
					      "have written nothing" : "");
			ps = candidate;
			chosen = name;
		} else {
			printf("drvtest: time: %s compiled but its first dispatch ended with "
			       "status %lu%s%s, so it is not timed\n", [name UTF8String],
			       (unsigned long)[cmd status], [cmd error] != nil ? ": " : "",
			       [cmd error] != nil ?
			       [[[cmd error] localizedDescription] UTF8String] : "");
			[candidate release];
		}
		[fn release];
	}
	if (ps == nil) {
		printf("drvtest: time: no kernel of this library both compiled and finished in "
		       "%u tries, so the compute round trip is NOT MEASURED here -- run this "
		       "mode again over /System/Library/VideoDecoders/AV1SW.metallib, whose "
		       "eleven kernels 10.15 dispatched on this GPU\n", tried);
		fflush(stdout);
		goto out;
	}
	printf("drvtest: time: the dispatch below is %s, 4x4x1 threads in one threadgroup, 16 "
	       "buffers bound\n", [chosen UTF8String]);
	fflush(stdout);
	/*
	 * Its compile, timed here and not with the render pipelines, because a library may have
	 * kernels and no fragment function at all (AV1SW.metallib is eleven kernels) and the
	 * render group leaves early for such a library. The kernel is one that has already
	 * dispatched, so this is the compile cost of a shader known to work.
	 */
	{
		struct tm_run compile = { 0 };
		id<MTLFunction> again = [library newFunctionWithName:chosen];

		for (unsigned r = 0; again != nil && r < tm_repeats; r++) {
			@autoreleasepool {
				NSError *e = nil;
				uint64_t t0 = tm_now_ns();
				id<MTLComputePipelineState> p =
				    [dev newComputePipelineStateWithFunction:again error:&e];
				uint64_t t1 = tm_now_ns();

				cost_pipeline_add(t1 - t0);     /* (M4, cost_end) */
				if (p != nil) {
					tm_add(&compile, t1 - t0);
					[p release];
				}
			}
		}
		[again release];
		tm_say("a COMPUTE pipeline, warm, the same kernel again", &compile,
		       "a kernel's compile is its own size -- the name is printed above, and "
		       "comparing it with a fragment shader's compares two shaders and not "
		       "two paths");
	}
	struct tm_worked worked = { 0, 0, { 0 } };
	bool gpu_side = tm_gpu_worked(&worked);

	for (unsigned r = 0; r < tm_repeats; r++) {
		@autoreleasepool {
			uint64_t t0 = tm_now_ns();
			id<MTLCommandBuffer> cmd = [queue commandBuffer];
			id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];

			if (enc == nil) {
				printf("drvtest: time: the encoder stopped being given at "
				       "repeat %u\n", r);
				break;
			}
			[enc setComputePipelineState:ps];
			for (int i = 0; i < TM_DISPATCH_BUFFERS; i++) {
				[enc setBuffer:bufs[i] offset:0 atIndex:i];
				if (texs[i] != nil) {
					[enc setTexture:texs[i] atIndex:i];
				}
				if (samp != nil) {
					[enc setSamplerState:samp atIndex:i];
				}
			}
			[enc dispatchThreadgroups:MTLSizeMake(1, 1, 1)
			    threadsPerThreadgroup:MTLSizeMake(4, 4, 1)];
			[enc endEncoding];
			uint64_t t1 = tm_now_ns();

			[cmd commit];
			uint64_t t2 = tm_now_ns();

			[cmd waitUntilCompleted];
			uint64_t t3 = tm_now_ns();

			tm_submissions++;
			if ([cmd status] != RW_CB_COMPLETED) {
				printf("drvtest: time: the dispatch stopped finishing at repeat "
				       "%u (status %lu), so the numbers below cover only what "
				       "came before it\n", r, (unsigned long)[cmd status]);
				break;
			}
			tm_add(&encode, t1 - t0);
			tm_add(&commit, t2 - t1);
			tm_add(&wait, t3 - t2);
		}
	}
	tm_say("-commit, one 4x4x1 compute dispatch", &commit,
	       "this kernel is a big one -- its name is printed above, and a kernel's own work "
	       "is in this number as much as the round trip is");
	if (gpu_side && tm_gpu_worked(&worked)) {
		tm_say_gpu_side("the compute dispatch", &worked, tm_median(&commit),
				(unsigned long)commit.n);
	}
	tm_say("the encoding of that dispatch", &encode,
	       "it is anywhere near -commit: encoding only records here");
	tm_say("-waitUntilCompleted after that dispatch", &wait,
	       "it is not ~0 -- then the fence moved out of -commit on the compute path too "
	       "[M4: it has, by default, on both back ends]");
	[ps release];
out:
	for (int i = 0; i < TM_DISPATCH_BUFFERS; i++) {
		[bufs[i] release];
		[texs[i] release];
	}
	[samp release];
}

/*
 * What memory costs: a texture in, a texture out.
 *
 * -replaceRegion: is how a game's textures arrive, and -getBytes: is how every verifying mode
 * in this file reads an attachment. The second is not a game's cost at all, and is here so
 * that it can be subtracted from the picture mode's time instead of being mistaken for a
 * draw's. Both are reported in MB/s as well, which is the only form in which 256 bytes and
 * ten megabytes are comparable.
 */
static void
tm_memory(id<MTLDevice> dev, unsigned w, unsigned h)
{
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
							       width:w
							      height:h
							   mipmapped:NO];
	struct tm_run up = { 0 }, down = { 0 }, newbuf = { 0 };
	size_t bytes = (size_t)w * h * 4;
	unsigned char *host = malloc(bytes);
	unsigned repeats = tm_repeats > 8 ? 8 : tm_repeats;

	[td setUsage:MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget];
	id<MTLTexture> tex = [dev newTextureWithDescriptor:td];

	if (tex == nil || host == NULL) {
		printf("drvtest: time: no %ux%u texture or no %zu bytes on this side, so the "
		       "upload and the readback are NOT MEASURED\n", w, h, bytes);
		free(host);
		[tex release];
		fflush(stdout);
		return;
	}
	memset(host, 0x5a, bytes);
	/* Eight and not thirty-two: ten megabytes each way is the measurement, and the point
	 * of the repeats is only that one outlier cannot be it. */
	for (unsigned i = 0; i < repeats; i++) {
		uint64_t t0 = tm_now_ns();

		[tex replaceRegion:MTLRegionMake2D(0, 0, w, h)
		       mipmapLevel:0
			 withBytes:host
		       bytesPerRow:(NSUInteger)w * 4];
		tm_add(&up, tm_now_ns() - t0);
	}
	for (unsigned i = 0; i < repeats; i++) {
		uint64_t t0 = tm_now_ns();

		[tex getBytes:host
		  bytesPerRow:(NSUInteger)w * 4
		   fromRegion:MTLRegionMake2D(0, 0, w, h)
		  mipmapLevel:0];
		tm_add(&down, tm_now_ns() - t0);
	}
	for (unsigned i = 0; i < tm_repeats; i++) {
		@autoreleasepool {
			uint64_t t0 = tm_now_ns();
			id<MTLBuffer> b = [dev newBufferWithLength:65536
							   options:MTLResourceStorageModeShared];
			uint64_t t1 = tm_now_ns();

			if (b != nil) {
				tm_add(&newbuf, t1 - t0);
				[b release];
			}
		}
	}
	char line[160];

	snprintf(line, sizeof(line), "-replaceRegion: the whole %ux%u BGRA8", w, h);
	tm_say_rate(line, &up, bytes,
		    "the first sample is the odd one -- the pages of this side's buffer are "
		    "touched by the memset above, so they are not being faulted in here");
	snprintf(line, sizeof(line), "-getBytes: the whole %ux%u BGRA8", w, h);
	tm_say_rate(line, &down, bytes,
		    "a game is thought to pay this: it does not read attachments back, and "
		    "this line exists to be SUBTRACTED from the verifying modes");
	tm_say("-newBufferWithLength: 65536", &newbuf,
	       "the buffer is never touched, so this is the allocation and not the pages: a "
	       "first write to it would fault");
	free(host);
	[tex release];
}

/*
 * How many command buffers a second the queue sustains with nothing in them.
 *
 * This is the floor under every frame rate: a driver that cannot make and retire a command
 * buffer quickly cannot draw a frame quickly whatever the GPU does. It is also the one place
 * where a return of the 64-slot bug (10.33) would show up as a number -- each buffer takes a
 * slot of maxCommandBufferCount at -init and this driver gives it back at -commit, so if the
 * return ever goes away this loop stops dead at the 65th instead of slowing down, and the
 * last line of this mode is the deliberate test for that.
 */
static void
tm_queue(id<MTLCommandQueue> queue)
{
	struct tm_run empty = { 0 };
	unsigned n = tm_repeats * 8 < TM_MAX_SAMPLES ? tm_repeats * 8 : TM_MAX_SAMPLES;

	for (unsigned i = 0; i < n; i++) {
		@autoreleasepool {
			uint64_t t0 = tm_now_ns();
			id<MTLCommandBuffer> cmd = [queue commandBuffer];

			[cmd commit];
			tm_add(&empty, tm_now_ns() - t0);
		}
	}
	tm_say("an EMPTY command buffer, made and committed", &empty,
	       "the autorelease pool is doing the freeing here, so this is not the cost of a "
	       "command buffer that an application keeps a handler on");

	uint64_t median = tm_median(&empty);

	/*
	 * Only when the median is above the clock's own tick. The first run's median was 0 and
	 * 1 microsecond -- that is "below this clock's resolution", and turning it into
	 * "1 000 000 a second" would be quoting the quantum as a measurement.
	 */
	if (median > 2000) {
		printf("drvtest: time: empty command buffers: %.0f a second at the median, so "
		       "no frame can cost less than %.1f us of this driver's bookkeeping per "
		       "command buffer\n", 1000000000.0 / (double)median, median / 1000.0);
	} else {
		printf("drvtest: time: an empty command buffer is BELOW this clock's resolution "
		       "(median %.1f us against a %.1f us tick): all that can be said is that "
		       "it costs at most a microsecond or two, and the queue's bookkeeping is "
		       "not in any frame's way\n", median / 1000.0, 1.0);
	}
	fflush(stdout);
}

/*
 * What it costs to show a frame -- which is NOT Metal's, and is said so where it is printed.
 *
 * This port has no drawable: a finished attachment reaches the panel by being read back into
 * this process (-getBytes:) and handed to the kernel's kHKShow, which copies it row by
 * row into the boot framebuffer (iokit/Kernel/HKGPU.cpp). Both halves are measured
 * because a frame rate that leaves them out is a frame rate for a game nobody can see.
 */
static void
tm_present(id<MTLDevice> dev)
{
	struct tm_run show = { 0 }, read = { 0 };
	unsigned repeats = tm_repeats > 8 ? 8 : tm_repeats;
	size_t bytes = (size_t)PANEL_W * PANEL_H * 4;

	if (pic_panel == NULL) {
		pic_panel = calloc(1, bytes);
	}
	if (pic_panel == NULL) {
		printf("drvtest: time: no memory for the panel, so the present is NOT "
		       "MEASURED\n");
		fflush(stdout);
		return;
	}
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
							       width:PANEL_W
							      height:PANEL_H
							   mipmapped:NO];

	[td setUsage:MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget];
	id<MTLTexture> tex = [dev newTextureWithDescriptor:td];

	if (tex != nil) {
		for (unsigned i = 0; i < repeats; i++) {
			uint64_t t0 = tm_now_ns();

			[tex getBytes:pic_panel
			  bytesPerRow:PANEL_W * 4
			   fromRegion:MTLRegionMake2D(0, 0, PANEL_W, PANEL_H)
			  mipmapLevel:0];
			tm_add(&read, tm_now_ns() - t0);
		}
		tm_say_rate("a frame OUT of Metal: -getBytes: 1080x2400", &read, bytes,
			    "the attachment was never drawn into, so this is a read of "
			    "untouched memory -- which is the right number for the COPY and "
			    "says nothing about a draw");
		[tex release];
	}
	if (pic_connection == 0) {
		printf("drvtest: time: no GPU service open, so the panel copy is NOT MEASURED "
		       "(that is the kernel's kHKShow, not Metal's)\n");
		fflush(stdout);
		return;
	}
	for (unsigned i = 0; i < repeats; i++) {
		struct pic_show_request req = {
			(uint64_t)(uintptr_t)pic_panel, PANEL_W, PANEL_H, PANEL_W * 4, 0
		};
		uint64_t t0 = tm_now_ns();

		IOConnectCallStructMethod(pic_connection, 0x7f, &req, sizeof(req), NULL, NULL);
		tm_add(&show, tm_now_ns() - t0);
	}
	tm_say_rate("the panel copy in the KERNEL (kHKShow), 1080x2400", &show, bytes,
		    "this is counted as Metal's: it is a row-by-row copy in the kernel and the "
		    "same call src/gl_test.c shows a triangle with");

	uint64_t frame = tm_median(&read) + tm_median(&show);

	tm_present_ns = frame;
	if (frame != 0) {
		printf("drvtest: time: showing one finished 1080x2400 frame costs %.1f us "
		       "before any drawing at all, which alone caps this port at %.1f frames a "
		       "second\n", frame / 1000.0, 1000000000.0 / (double)frame);
		fflush(stdout);
	}
}

/*
 * The 64-slot ladder, LAST and on purpose.
 *
 * -[_MTLCommandBuffer initWithQueue:retainedReferences:synchronousDebugMode:] waits on the
 * queue's semaphore (maxCommandBufferCount, 64 by default) and this driver returns the slot
 * at -commit (10.33). Sixty-four live, committed command buffers must therefore hold zero
 * slots and the sixty-fifth must not block.
 *
 * There is deliberately NO autorelease pool in this loop: with one, each buffer would be
 * deallocated at the end of its iteration and the slot would come back from -dealloc, so the
 * ladder would pass whether or not -commit returns it -- which is exactly the bug. Sixty-four
 * live command buffers holding no slot is the measurement.
 *
 * It is last because if the return ever regresses this blocks for ever, and then everything
 * else in this mode has already been printed and the log's final line names the cause.
 */
static void
tm_slots(id<MTLCommandQueue> queue)
{
	id<MTLCommandBuffer> live[64];
	struct tm_run sixtyfifth = { 0 };

	printf("drvtest: time: the slot test: 64 command buffers, committed and kept alive, "
	       "then the 65th. If this is the last line in the log, -commit stopped returning "
	       "the queue's semaphore slot and the 65th -commandBuffer is blocked for ever\n");
	fflush(stdout);
	for (unsigned i = 0; i < 64; i++) {
		live[i] = [[queue commandBuffer] retain];
		[live[i] commit];
	}
	/* The queue's own count, asked rather than inferred -- the same two selectors
	 * pic_queue_state uses, printed here so that every line of this mode reads alike. */
	NSUInteger (*ask)(id, SEL) = (NSUInteger (*)(id, SEL))objc_msgSend;
	SEL outstanding = sel_getUid("hkOutstandingCommandBuffers");
	SEL most = sel_getUid("maxCommandBufferCount");

	if ([queue respondsToSelector:outstanding] && [queue respondsToSelector:most]) {
		printf("drvtest: time: the queue takes %lu command buffers at a time and %lu of "
		       "the 64 alive still hold a slot (it must be 0: the slot goes back at "
		       "-commit)\n", (unsigned long)ask(queue, most),
		       (unsigned long)ask(queue, outstanding));
	} else {
		printf("drvtest: time: this queue does not say how many command buffers hold a "
		       "slot, so a block below would again have to be inferred\n");
	}
	fflush(stdout);
	uint64_t t0 = tm_now_ns();
	id<MTLCommandBuffer> next = [queue commandBuffer];
	uint64_t t1 = tm_now_ns();

	tm_add(&sixtyfifth, t1 - t0);
	[next commit];
	tm_say("the 65th -commandBuffer with 64 alive", &sixtyfifth,
	       "it is near the two-second mark rather than near the ordinary cost above -- "
	       "then the slot is being returned late instead of at -commit");
	for (unsigned i = 0; i < 64; i++) {
		[live[i] release];
	}
}

/*
 * WHAT THE NUMBERS MEAN FOR A GAME, with the arithmetic in the log rather than in somebody's
 * head afterwards.
 *
 * Every assumption is named on the line it is used, because each of them is generous to this
 * driver and a reader who does not know that will read these frame rates as a promise:
 *
 *   - ONE COMMAND BUFFER A FRAME. An application that makes several pays the command buffer's
 *     cost several times.
 *   - NO OVERLAP, which is not an assumption but this driver's behaviour: rwa_render_draw
 *     waits on its own fence inside -commit, so the processor and the GPU take turns and a
 *     frame is the SUM of its draws. A driver that pipelined would reach the maximum of the
 *     two instead of the sum, and that is the single biggest difference between these numbers
 *     and what the same hardware can do.
 *     [M4: true only with metal_sync=1 now. With the default fence per command buffer, on
 *     both back ends, -commit does not wait (the comment at the top of this section), so the
 *     per-draw medians this arithmetic uses are the processor's side alone and the frame
 *     rates below are an upper bound; the GPU's side is -waitUntilCompleted's and the cost
 *     line's. The arithmetic is kept as it was, and says so in the log.]
 *   - EVERY DRAW COVERS THE WHOLE PANEL in the upper bracket and sixteen pixels in the lower
 *     one. A real scene is between them and nearer the lower end, so the two brackets are
 *     printed and the middle is not invented.
 *   - THE PRESENT IS PAID ONCE A FRAME, and it is not Metal's: this port shows a frame by
 *     reading the attachment back and handing it to the kernel's row copy. A game that never
 *     shows anything saves it.
 *   - NOTHING ELSE RUNS. These were measured with the panel to ourselves; with iOS's own
 *     daemons alive the GPU and the one core doing all this are shared.
 *   - THE COMPILES ARE NOT IN THE FRAME. They are counted separately, as what a game pays
 *     before its first frame and again whenever it meets a shader it has not met.
 */
static void
tm_verdict(void)
{
	static const unsigned scene[] = { 1, 10, 50, 200, 1000 };

	printf("drvtest: time: ---- what this costs a game, from the medians above ----\n");
	if (tm_warm_pipeline_ns != 0) {
		printf("drvtest: time: a shader costs %.2f ms to compile and NOTHING caches it, "
		       "so a game with 200 pipelines pays %.1f s before its first frame and one "
		       "with 2000 pays %.0f s. There is no disk cache and no pipeline library on "
		       "this port (10.40: the file Metal looks for does not exist), so it pays "
		       "that again at every launch\n", tm_warm_pipeline_ns / 1000000.0,
		       200.0 * tm_warm_pipeline_ns / 1000000000.0,
		       2000.0 * tm_warm_pipeline_ns / 1000000000.0);
	} else {
		printf("drvtest: time: the compile cost was not measured in this run, so the "
		       "startup arithmetic is missing rather than estimated\n");
	}
	if (tm_present_ns != 0) {
		printf("drvtest: time: showing a frame costs %.2f ms before any drawing, which "
		       "by itself caps this port at %.1f frames a second\n",
		       tm_present_ns / 1000000.0, 1000000000.0 / (double)tm_present_ns);
	}
	if (tm_small_marginal_ns == 0 && tm_panel_marginal_ns == 0) {
		printf("drvtest: time: no per-draw median survived this run, so no frame rate "
		       "is computed here. That is the honest answer and not a zero\n");
		fflush(stdout);
		return;
	}
	printf("drvtest: time: a frame of N draws = the present + one command buffer + N x one "
	       "draw, with no overlap between processor and GPU because this driver fences "
	       "inside -commit. Two brackets: every draw 4x4 (the per-draw floor) and every "
	       "draw the whole 1080x2400 panel (the ceiling)\n");
	printf("drvtest: time: [M4: with a fence per command buffer -- the default unless "
	       "metal_sync=1 -- -commit does not wait, so the per-draw figures below are the "
	       "processor's side alone and these frame rates are an upper bound; the GPU's side "
	       "is in -waitUntilCompleted above and in the cost line at the end]\n");
	for (unsigned i = 0; i < sizeof(scene) / sizeof(scene[0]); i++) {
		double fixed = (double)tm_present_ns + (double)tm_buffer_ns;
		double small = fixed + (double)scene[i] * tm_small_marginal_ns;
		double big = fixed + (double)scene[i] * tm_panel_marginal_ns;

		printf("drvtest: time: %4u draws: %8.2f ms (%6.1f fps) if every draw is 4x4; "
		       "%10.2f ms (%6.2f fps) if every draw fills the panel\n", scene[i],
		       small / 1000000.0, small > 0 ? 1000000000.0 / small : 0.0,
		       big / 1000000.0, big > 0 ? 1000000000.0 / big : 0.0);
	}
	if (tm_panel_marginal_ns != 0 && tm_small_marginal_ns != 0) {
		/*
		 * The ratio of the two is a ratio and NOT a number of draws, which is how the
		 * first run of this printed "below about 13 draws a frame" -- a wrong sentence
		 * next to a right number. What the ratio says is how much of a draw is fixed:
		 * all of it at 4x4, a fraction of it at the panel, and the area at which the
		 * two are equal is what a reader actually wants.
		 */
		printf("drvtest: time: of one full-panel draw's %.2f ms, %.2f ms is also there at "
		       "4x4 (the submission, the fence and this driver's own work per draw) and "
		       "%.2f ms is the pixels: the fixed part is %.0f%% of a full-screen draw and "
		       "all of a small one, and the two are equal at about %.2f megapixels a "
		       "draw\n", tm_panel_marginal_ns / 1000000.0,
		       tm_small_marginal_ns / 1000000.0,
		       (double)(tm_panel_marginal_ns - tm_small_marginal_ns) / 1000000.0,
		       100.0 * tm_small_marginal_ns / (double)tm_panel_marginal_ns,
		       tm_panel_marginal_ns > tm_small_marginal_ns ?
		       2.4719 * tm_small_marginal_ns /
		       (double)(tm_panel_marginal_ns - tm_small_marginal_ns) : 0.0);
	}
	if (tm_panel_tex_ns != 0 && tm_panel_marginal_ns != 0) {
		printf("drvtest: time: Apple's textured blit over the panel costs %.2f ms "
		       "against %.2f ms for the shader that only writes a colour, so one "
		       "texture fetch per pixel over 2 592 000 pixels is worth %.2f ms\n",
		       tm_panel_tex_ns / 1000000.0, tm_panel_marginal_ns / 1000000.0,
		       (double)((int64_t)tm_panel_tex_ns - (int64_t)tm_panel_marginal_ns) /
		       1000000.0);
	}
	fflush(stdout);
}

/*
 * --time [REPEATS]: the mode itself.
 *
 * The order is deliberate: the clock first (so nothing else is read without it), then the
 * compiles (which a game pays before its first frame), then the draws from the smallest shape
 * in this file to the panel's own, then memory, then the queue, then the present, then the
 * slot test. Each group says what it is and what would make it wrong, and the last lines turn
 * the medians into a frame rate with the arithmetic shown, so a reader does not have to redo
 * it to disagree with it.
 */
static void
time_path(id<MTLDevice> dev, const char *metallib, unsigned repeats)
{
	NSData *data = [NSData dataWithContentsOfFile:
	    [NSString stringWithUTF8String:metallib]];

	tm_mode_entered = tm_now_ns();
	tm_repeats = repeats < 4 ? 4 : (repeats > TM_MAX_SAMPLES ? TM_MAX_SAMPLES : repeats);
	printf("drvtest: time: %u repeats of each measurement; every line says what it is and "
	       "what would make it wrong. A median leads, the first sample is printed apart "
	       "because the first of anything here is a different measurement, and the max is "
	       "there so an outlier is seen and not smoothed\n", tm_repeats);
	printf("drvtest: time: this driver does the GPU work inside -commit and waits for the "
	       "fence there (10.13), so -commit is what an application waits on and "
	       "-waitUntilCompleted should measure ~0. Both are printed; if the second is not "
	       "~0, the split in every line below is wrong\n");
	printf("drvtest: time: [M4: that sentence holds with metal_sync=1 only. By default, on "
	       "both back ends, -commit records and submits once (rwa_device_defer_waits) and "
	       "-waitUntilCompleted carries the GPU's time, so read -commit as the processor's "
	       "side and -waitUntilCompleted as the GPU's; the comment above time_path says "
	       "which derived lines that affects]\n");
	printf("drvtest: time: the GPU's share is separated three ways, in order of how much "
	       "they can be trusted: by ASKING THE KERNEL for the core's own time per case "
	       "(request 0x7d, if this kernel has it -- the next line says whether it does), "
	       "by subtracting the 4x4 case from the big ones, and by the kernel's own "
	       "\"tl: q\" lines in this log with gpu_log=1\n");
	fflush(stdout);
	if (data == nil) {
		printf("drvtest: time: cannot read %s\n", metallib);
		return;
	}
	dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL,
						  DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	NSError *err = nil;
	id<MTLLibrary> library = [dev newLibraryWithData:dd error:&err];

	if (library == nil) {
		printf("drvtest: time: newLibraryWithData: gave nil\n");
		return;
	}
	id<MTLCommandQueue> queue = [dev newCommandQueue];

	if (queue == nil) {
		printf("drvtest: time: no command queue\n");
		return;
	}
	/*
	 * [M4, W1: the whole mode is one workload with a cost line (cost_end, before the slot
	 * test, which may block for ever). cost_begin spends COST_PRE_MS in an idle window and
	 * reads the machine's state, which no launching application waits through, so the two
	 * marks of the main-to-first-pixel line are moved on by what it took: that line means
	 * what it meant.]
	 */
	{
		uint64_t before_cost = tm_now_ns();

		cost_begin("W1");
		tm_main_entered += tm_now_ns() - before_cost;
		tm_mode_entered += tm_now_ns() - before_cost;
	}
	/* opened here and not at the end, because the present measurement needs it and
	 * because a mode that stops early should still have shown what it got that far */
	pic_open_panel();

	tm_clock_floor();
	tm_first_frame(dev, library, queue);
	tm_pipelines(dev, library);

	/* 4x4 is every earlier number in this file, 1080x648 is the picture's band (so these
	 * are comparable with logs/140340-run.log, where the same draw read "0 ms"), and
	 * 1080x2400 is the panel: what a frame is. Four targets at 4x4 and at the band, one at
	 * the panel, because the panel with four is 41 MB of attachment and the question this
	 * mode asks is what a frame costs, not what four frames cost. */
	tm_draw_case(dev, library, queue, 4, 4, 4, false);
	tm_draw_case(dev, library, queue, 4, PANEL_W, PIC_BAND_H, false);
	tm_draw_case(dev, library, queue, 1, PANEL_W, PANEL_H, false);
	tm_draw_case(dev, library, queue, 1, PANEL_W, PANEL_H, true);

	tm_dispatch(dev, library, queue);
	tm_memory(dev, PANEL_W, PANEL_H);
	tm_queue(queue);
	tm_present(dev);

	tm_verdict();
	printf("drvtest: time: the whole mode took %.1f s of wall clock, which is NOT a "
	       "measurement of anything -- it is here so that a boot can be given enough "
	       "seconds for it\n", (tm_now_ns() - tm_mode_entered) / 1000000000.0);
	printf("drvtest: time: this mode asked the GPU for %lu submissions in all; with "
	       "gpu_log=1 the kernel's \"tl: q <fence> took <us>\" lines in this same log are "
	       "the GPU's own side of them, and its five-second report prints the average and "
	       "a histogram -- but only in a window in which something was submitted, so a "
	       "burst shorter than five seconds leaves no such line\n", tm_submissions);
	fflush(stdout);
	/* M4, W1: the cost line of the whole mode, with the kernel's own submission count for
	 * it (tm_submissions above counts draws asked for, not submissions) */
	cost_end(NULL, NULL, "time %u repeats", tm_repeats);
	tm_slots(queue);
	printf("drvtest: time: done\n");
	fflush(stdout);
}

/*
 * ==========================================================================================
 * --frames N owned|surface (M4, W4): A FRAME, N TIMES, AND WHAT EACH ONE COSTS
 * ==========================================================================================
 *
 * The workload nearest to what a game or the window server does with Metal: N frames, each
 * ONE command buffer with ONE render pass of ONE full-screen draw into a 1080x2400 BGRA8
 * target, cleared first (an application clears once a frame), committed and waited for --
 * one frame at a time, nothing in flight between two, so each frame's numbers are its own:
 *
 *   owned     the target is a texture of the driver's own (newTextureWithDescriptor:);
 *   surface   it is an IOSurface this program makes -- BGRA, four bytes a texel, the row
 *             length the kernel's own, as drawable_test makes one and as CoreAnimation makes a
 *             drawable's -- imported with newTextureWithDescriptor:iosurface:plane: (hkmetal.m,
 *             hkdrawable.m rwd_texture_for_surface, rwa_texture_from_surface): a drawable's
 *             road.
 *
 * On Vulkan an imported surface is mapped once at import, so the kernel treats it as SEEN
 * and cleans and invalidates the processor's cache over it at EVERY submission (air_vk.c,
 * THE SURFACE MARKED "SEEN"; HKGPU.cpp submit): ten megabytes for this one, in our system
 * time, at every frame. --vk-noseen (AIR_VK_NOSEEN=1, set before Metal starts) turns that
 * mapping off, so the same frames with and without it price that clean; the lines then say
 * "surface-noseen". Gallium never maps a surface it only draws into, so there the kernel
 * cleans it when its seed moved or 4 ms after the last time (GPU_PUSH_AGAIN_MS).
 *
 * THE SHADER: clearFragment01 of AppleMetalGLRenderer, over the stage the library generates
 * (no vertex function: the quad covers the attachment). No fragment shader of that library
 * binds nothing -- all 271 take a constant buffer (llvm-dis of every module, 2026-11-05) --
 * and clearFragment01 binds the least: one 48-byte ClearFragmentInput at [[buffer(14)]],
 * float4 out to colour 0, which --time has drawn into BGRA8 since 10.33 with its pixel
 * checked. It is bound at 14 alone, as its module declares (air.location_index 14).
 *
 * WHAT IS PRINTED: the cost line (cost_end; the N frames are its work phase, the pipeline is
 * its pipelines field, and one warm-up frame before them is setup: ir3 links a program at
 * its first draw); then one frames line, the per-frame median and p90 of the wall time
 * (-commandBuffer to -waitUntilCompleted, the frame's autorelease pool included), of the
 * process's CPU time, user and system apart (getrusage around the same), of the core's time
 * and of the submissions (0x7d at both ends of the frame: a backboardd submission that
 * finished inside one of ours is in it, which the median does not see and the p90 may), and
 * backboardd's frames over the same period; then the pixel at (0, 0), read through the
 * driver after the timed frames, against the colour the buffer names.
 */
enum { FRAMES_MAX = 4096 };

static int
frames_compare(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

/* The median and the 90th percentile (the ceil(0.9 n)-th smallest) of n samples, sorted here. */
static void
frames_stats(uint64_t *v, unsigned n, double *median, double *p90)
{
	unsigned k = (9 * n + 9) / 10;

	*median = *p90 = 0;
	if (n == 0) {
		return;
	}
	qsort(v, n, sizeof(v[0]), frames_compare);
	*median = n % 2 ? (double)v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
	*p90 = (double)v[(k > 0 ? k : 1) - 1];
}

/* A surface the way drawable_test and CoreAnimation make one: no bytes-per-row asked for */
static IOSurfaceRef
frames_surface(int width, int height)
{
	CFMutableDictionaryRef props = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	const CFStringRef keys[4] = {
		kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfacePixelFormat, kIOSurfaceBytesPerElement,
	};
	const int values[4] = { width, height, 0x42475241 /* 'BGRA' */, 4 };
	IOSurfaceRef surface;

	if (props == NULL) {
		return NULL;
	}
	for (int i = 0; i < 4; i++) {
		CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &values[i]);

		CFDictionarySetValue(props, keys[i], n);
		CFRelease(n);
	}
	surface = IOSurfaceCreate(props);
	CFRelease(props);
	return surface;
}

/* One frame: a command buffer, the pass, the draw, -commit, the wait; in its own pool. */
static bool
frames_one(id<MTLCommandQueue> queue, MTLRenderPassDescriptor *rp,
	   id<MTLRenderPipelineState> ps, id<MTLBuffer> uniform, char *why, size_t size)
{
	bool ok = false;

	@autoreleasepool {
		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		id<MTLRenderCommandEncoder> enc = cmd != nil ?
		    [cmd renderCommandEncoderWithDescriptor:rp] : nil;

		if (enc == nil) {
			snprintf(why, size, "no %s", cmd == nil ? "command buffer" : "render encoder");
		} else {
			[enc setRenderPipelineState:ps];
			[enc setFragmentBuffer:uniform offset:0 atIndex:14];
			[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
			[enc endEncoding];
			[cmd commit];
			[cmd waitUntilCompleted];
			ok = [cmd status] == RW_CB_COMPLETED;
			if (!ok) {
				snprintf(why, size, "status %lu: %s", (unsigned long)[cmd status],
					 [cmd error] != nil ?
					 [[[cmd error] localizedDescription] UTF8String] : "(no error)");
			}
		}
	}
	return ok;
}

static void
frames_path(id<MTLDevice> dev, const char *metallib, unsigned n, const char *target)
{
	/* the colour clearFragment01 is told to write (--time's): b g r a */
	static const unsigned char want[4] = { 109, 182, 73, 255 };
	const char *backend = backend_is_vk() ? "vk" : "gallium";
	const char *noseen_env = getenv("AIR_VK_NOSEEN");
	bool surface = strcmp(target, "surface") == 0;
	bool noseen = noseen_env != NULL && strcmp(noseen_env, "1") == 0;
	char label[32], why[512] = "";
	NSError *err = nil;
	IOSurfaceRef surf = NULL;
	id<MTLLibrary> library = nil;
	id<MTLFunction> fn = nil;
	id<MTLCommandQueue> queue = nil;
	id<MTLTexture> tex = nil;
	id<MTLBuffer> uniform = nil;
	id<MTLRenderPipelineState> ps = nil;
	MTLRenderPipelineDescriptor *pd = nil;
	uint64_t *v[5] = { NULL, NULL, NULL, NULL, NULL };     /* wall, user, sys, gpu, subs */
	unsigned done = 0;
	uint64_t warm_ns = 0;

	if (!surface && strcmp(target, "owned") != 0) {
		printf("drvtest: frames: the target is owned or surface, not %s\n", target);
		return;
	}
	n = n == 0 ? 1 : n > FRAMES_MAX ? FRAMES_MAX : n;
	snprintf(label, sizeof(label), "%s%s", target, surface && noseen ? "-noseen" : "");
	if (noseen && !backend_is_vk()) {
		printf("drvtest: frames: AIR_VK_NOSEEN=1 is the Vulkan back end's switch; on "
		       "Gallium it changes nothing\n");
	}
	NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:metallib]];

	if (data != nil) {
		dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL,
							  DISPATCH_DATA_DESTRUCTOR_DEFAULT);

		library = [dev newLibraryWithData:dd error:&err];
	}
	fn = library != nil ? [library newFunctionWithName:@"clearFragment01"] : nil;
	queue = [dev newCommandQueue];
	uniform = [dev newBufferWithLength:256 options:0];
	if (fn == nil || queue == nil || uniform == nil || [uniform contents] == NULL) {
		printf("drvtest: frames %s W4 %s: NOT MEASURED: %s\n", backend, label,
		       library == nil ? "the library could not be read" :
		       fn == nil ? "it has no clearFragment01" :
		       "no command queue or no constant buffer");
		goto out;
	}
	{
		float *u = [uniform contents];

		memset(u, 0, 256);
		u[0] = want[2] / 255.0f;
		u[1] = want[1] / 255.0f;
		u[2] = want[0] / 255.0f;
		u[3] = 1.0f;
	}
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
							       width:PANEL_W
							      height:PANEL_H
							   mipmapped:NO];

	[td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];
	if (surface) {
		surf = frames_surface(PANEL_W, PANEL_H);
		if (surf == NULL) {
			printf("drvtest: frames %s W4 %s: NOT MEASURED: IOSurfaceCreate would not make "
			       "a %ux%u BGRA surface\n", backend, label, PANEL_W, PANEL_H);
			goto out;
		}
		printf("drvtest: frames %s W4 %s: surface %u, %zux%zu, %zu bytes a row, %zu bytes\n",
		       backend, label, IOSurfaceGetID(surf), IOSurfaceGetWidth(surf),
		       IOSurfaceGetHeight(surf), IOSurfaceGetBytesPerRow(surf),
		       IOSurfaceGetAllocSize(surf));
		tex = [dev newTextureWithDescriptor:td iosurface:surf plane:0];
	} else {
		tex = [dev newTextureWithDescriptor:td];
	}
	if (tex == nil) {
		printf("drvtest: frames %s W4 %s: NOT MEASURED: no %ux%u BGRA8 texture %s (the "
		       "driver's line above says why)\n", backend, label, PANEL_W, PANEL_H,
		       surface ? "over the surface" : "of the driver's own");
		goto out;
	}
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	MTLRenderPassColorAttachmentDescriptor *ca = [[rp colorAttachments] objectAtIndexedSubscript:0];

	[ca setTexture:tex];
	[ca setLoadAction:MTLLoadActionClear];
	[ca setClearColor:MTLClearColorMake(0, 0, 0, 1)];
	[ca setStoreAction:MTLStoreActionStore];
	pd = [[MTLRenderPipelineDescriptor alloc] init];
	[pd setFragmentFunction:fn];
	[pd setVertexFunction:nil];
	[[[pd colorAttachments] objectAtIndexedSubscript:0] setPixelFormat:MTLPixelFormatBGRA8Unorm];
	for (int i = 0; i < 5; i++) {
		v[i] = calloc(n, sizeof(uint64_t));
		if (v[i] == NULL) {
			printf("drvtest: frames %s W4 %s: NOT MEASURED: no memory for %u samples\n",
			       backend, label, n);
			goto out;
		}
	}
	printf("drvtest: frames %s W4 %s: %u frames, each one command buffer with one cleared "
	       "pass of one full-screen clearFragment01 draw into a %ux%u BGRA8 %s, committed and "
	       "waited for\n", backend, label, n, PANEL_W, PANEL_H,
	       surface ? "IOSurface" : "texture");
	fflush(stdout);

	cost_begin("W4");
	cost_phase(COST_PIPELINE);
	ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
	cost_phase(COST_SETUP);
	if (ps == nil) {
		cost_end("frames", "frame", "frames 0 of %u %s", n, label);
		printf("drvtest: frames %s W4 %s: NOT MEASURED: no pipeline: %s\n", backend, label,
		       err != nil ? [[err localizedDescription] UTF8String] : "(no reason)");
		goto out;
	}
	{
		uint64_t t0 = tm_now_ns();

		if (!frames_one(queue, rp, ps, uniform, why, sizeof(why))) {
			cost_end("frames", "frame", "frames 0 of %u %s", n, label);
			printf("drvtest: frames %s W4 %s: NOT MEASURED: the warm-up frame did not "
			       "complete: %s\n", backend, label, why);
			goto out;
		}
		warm_ns = tm_now_ns() - t0;
	}
	/* the frames read 0x7d at both ends of each themselves: the keeper waits meanwhile */
	cost_keeper_pause(true);
	for (unsigned i = 0; i < n; i++) {
		struct rusage r0, r1;
		uint64_t t0, t1;
		bool ok;

		cost_phase(COST_WORK);
		t0 = tm_now_ns();
		getrusage(RUSAGE_SELF, &r0);
		ok = frames_one(queue, rp, ps, uniform, why, sizeof(why));
		getrusage(RUSAGE_SELF, &r1);
		t1 = tm_now_ns();
		cost_phase(COST_SETUP);
		if (!ok) {
			printf("drvtest: frames %s W4 %s: frame %u did not complete (%s): the numbers "
			       "below are of the %u before it\n", backend, label, i, why, done);
			break;
		}
		v[0][done] = t1 - t0;
		v[1][done] = cost_tv_us(r1.ru_utime) - cost_tv_us(r0.ru_utime);
		v[2][done] = cost_tv_us(r1.ru_stime) - cost_tv_us(r0.ru_stime);
		v[3][done] = cost_run.last_us;
		v[4][done] = cost_run.last_subs;
		done++;
	}
	cost_keeper_pause(false);
	cost_end("frames", "frame", "frames %u %s", done, label);
	{
		double m[5], p[5];
		int frames = cost_cur.a.frames >= 0 && cost_cur.b.frames >= 0 ?
		    cost_cur.b.frames - cost_cur.a.frames : -1;

		for (int i = 0; i < 5; i++) {
			frames_stats(v[i], done, &m[i], &p[i]);
		}
		printf("drvtest: frames %s W4 %s: %u frames: wall median %.1f p90 %.1f us, cpu user "
		       "median %.1f p90 %.1f sys median %.1f p90 %.1f us, gpu median %.1f p90 %.1f "
		       "us, submissions a frame median %.1f p90 %.0f; the warm-up frame %.1f us (not "
		       "counted); backboardd %d frames in the same %.0f ms\n", backend, label, done,
		       m[0] / 1000.0, p[0] / 1000.0, m[1], p[1], m[2], p[2], m[3], p[3], m[4], p[4],
		       warm_ns / 1000.0, frames, (cost_cur.b.ns - cost_cur.a.ns) / 1e6);
	}
	{
		unsigned char px[4] = { 0, 0, 0, 0 };

		[tex getBytes:px bytesPerRow:4 fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
		printf("drvtest: frames %s W4 %s: pixel (0,0) is r g b %u %u %u and its buffer named "
		       "%u %u %u -- %s\n", backend, label, px[2], px[1], px[0], want[2], want[1],
		       want[0], px[0] == want[0] && px[1] == want[1] && px[2] == want[2] ?
		       "the same, so the timed frames drew" :
		       "NOT THE SAME: the timings above are of frames that did something else");
	}
	fflush(stdout);
out:
	for (int i = 0; i < 5; i++) {
		free(v[i]);
	}
	[ps release];
	[pd release];
	[tex release];
	if (surf != NULL) {
		CFRelease(surf);
	}
	[uniform release];
	[queue release];
	[fn release];
	[library release];
}

/*
 * WHAT EVERY CLASS OF THE DRIVER OWES, COUNTED ON EVERY RUN.
 *
 * 10.17 counted this for the device object and found nothing owed; the objects below it
 * were never counted, and the first boot in which iOS's own daemons could reach them
 * (logs/130132-run.log, metal=1 with launchd alive) answered why that mattered: 88 aborts
 * in a hundred seconds, all of them -[HKMetalCommandQueue setGPUPriority:] and
 * -setCompletionQueue:, each followed by launchd restarting the daemon.
 *
 * Counting them is not simply "ask the runtime", for two reasons, and both of them are the
 * reason this code is longer than the one loop it replaces:
 *
 *  1. THE PROTOCOL MAY NOT BE IN THE PROCESS. MTLDevice and MTLDeviceSPI are declared in
 *     Metal.framework, so every process that asks Metal has them. MTLCommandQueueSPI,
 *     MTLCommandBufferSPI, MTLRenderCommandEncoderSPI, MTLComputeCommandEncoderSPI,
 *     MTLRenderPipelineStateSPI, MTLComputePipelineStateSPI and even the PUBLIC
 *     MTLCommandQueue and MTLRenderCommandEncoder are declared in AGXMetalA11.bundle --
 *     the GPU bundle of a phone whose GPU is Apple's, which never loads here. So
 *     objc_getProtocol returns NULL for them and `protocol_copyMethodDescriptionList` has
 *     nothing to copy. Their required methods are compiled in below, read out of this
 *     phone's own shared cache by tools/metal/protometh.py; where the runtime does have a
 *     protocol it is asked instead, and the two counts are compared out loud.
 *
 *  2. AN ENTRY IN A METHOD TABLE IS NOT BEHAVIOUR (docs/metal.md 10.1). Apple writes an
 *     abstract method as a real entry whose body is [self doesNotRecognizeSelector:_cmd];
 *     `respondsToSelector:` answers YES for it and the abort when it is called names the
 *     RECEIVER -- our class -- so in the log it is indistinguishable from a method that was
 *     never written. _MTLCommandEncoder has 27 such entries for the mesh and object shader
 *     stages alone. A count that only asked `respondsToSelector:` would have said zero
 *     owed while 28 aborts sat waiting, so each implementation found in the chain is also
 *     READ: an abstract body branches to the image's doesNotRecognizeSelector: stub, and
 *     that is what is looked for.
 */
struct rw_protocol {
	const char *name;
	const char *const *selectors;
	unsigned count;
};
struct rw_class {
	const char *name;
	const char *const *protocols;   /* the protocol and what it adopts, NULL-terminated */
};
#define RW_PROTOCOL(name, sym) { name, sym, sizeof(sym) / sizeof(sym[0]) }

/* The required methods of each protocol that is NOT in any process on this phone,
 * read out of AGXMetalA11.bundle in the device's own shared cache. Generated;
 * tools/metal/owes.py prints the same lists and says how they were taken. */
static const char *const spi_commandbufferspi[] = {   /* MTLCommandBufferSPI: 14 required */
	"addPurgedHeap:", "addPurgedResource:", "commitAndWaitUntilSubmittedWithDeadline:",
	"commitWithDeadline:", "deadlineProfile",
	"encodeDashboardFinalizeForResourceGroup:dashboard:value:forIndex:",
	"encodeDashboardFinalizeForResourceGroup:dashboard:values:indices:count:",
	"encodeDashboardTagForResourceGroup:", "isProfilingEnabled", "presentDrawable:options:",
	"profilingResults", "setLogs:", "setProfilingEnabled:", "userDictionary",
};
static const char *const spi_commandqueue[] = {   /* MTLCommandQueue: 7 required */
	"commandBuffer", "commandBufferWithDescriptor:", "commandBufferWithUnretainedReferences",
	"device", "insertDebugCaptureBoundary", "label", "setLabel:",
};
static const char *const spi_commandqueuespi[] = {   /* MTLCommandQueueSPI: 24 required */
	"backgroundTrackingPID", "commitQueue", "commitSynchronously", "completionQueue",
	"disableCrossQueueHazardTracking", "executionEnabled", "finish", "getBackgroundGPUPriority",
	"getGPUPriority", "isOpenGLQueue", "isProfilingEnabled", "maxCommandBufferCount", "qosLevel",
	"setBackgroundGPUPriority:", "setBackgroundGPUPriority:offset:", "setBackgroundTrackingPID:",
	"setCompletionQueue:", "setExecutionEnabled:", "setGPUPriority:", "setGPUPriority:offset:",
	"setProfilingEnabled:", "setSkipRender:", "setSubmissionQueue:", "skipRender",
};
static const char *const spi_computecommandencoder[] = {   /* MTLComputeCommandEncoder: 35 required */
	"dispatchThreadgroups:threadsPerThreadgroup:",
	"dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:",
	"dispatchThreads:threadsPerThreadgroup:", "dispatchType",
	"executeCommandsInBuffer:indirectBuffer:indirectBufferOffset:",
	"executeCommandsInBuffer:withRange:", "memoryBarrierWithResources:count:",
	"memoryBarrierWithScope:", "sampleCountersInBuffer:atSampleIndex:withBarrier:",
	"setAccelerationStructure:atBufferIndex:", "setBuffer:offset:atIndex:",
	"setBufferOffset:atIndex:", "setBuffers:offsets:withRange:", "setBytes:length:atIndex:",
	"setComputePipelineState:", "setImageblockWidth:height:",
	"setIntersectionFunctionTable:atBufferIndex:",
	"setIntersectionFunctionTables:withBufferRange:", "setSamplerState:atIndex:",
	"setSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setSamplerStates:lodMinClamps:lodMaxClamps:withRange:", "setSamplerStates:withRange:",
	"setStageInRegion:", "setStageInRegionWithIndirectBuffer:indirectBufferOffset:",
	"setTexture:atIndex:", "setTextures:withRange:", "setThreadgroupMemoryLength:atIndex:",
	"setVisibleFunctionTable:atBufferIndex:", "setVisibleFunctionTables:withBufferRange:",
	"updateFence:", "useHeap:", "useHeaps:count:", "useResource:usage:",
	"useResources:count:usage:", "waitForFence:",
};
static const char *const spi_computecommandencoderspi[] = {   /* MTLComputeCommandEncoderSPI: 4 required */
	"newKernelDebugInfo", "setThreadgroupDistributionMode:",
	"setThreadgroupDistributionModeWithClusterGroupIndex:", "setThreadgroupPackingDisabled:",
};
static const char *const spi_computepipelinestatespi[] = {   /* MTLComputePipelineStateSPI: 7 required */
	"allocatedSize", "debugInstrumentationData", "getComputeKernelTelemetryID", "gpuHandle",
	"staticThreadgroupMemoryLength", "textureWriteRoundingMode", "uniqueIdentifier",
};
static const char *const spi_pipelinestatewithprogramaddresstable[] = {   /* MTLPipelineStateWithProgramAddressTable: 1 required */
	"pipelineBinaries",
};
static const char *const spi_rendercommandencoder[] = {   /* MTLRenderCommandEncoder: 125 required */
	"dispatchThreadsPerTile:",
	"drawIndexedPatches:patchIndexBuffer:patchIndexBufferOffset:controlPointIndexBuffer:controlPointIndexBufferOffset:indirectBuffer:indirectBufferOffset:",
	"drawIndexedPatches:patchStart:patchCount:patchIndexBuffer:patchIndexBufferOffset:controlPointIndexBuffer:controlPointIndexBufferOffset:instanceCount:baseInstance:",
	"drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:",
	"drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:",
	"drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:",
	"drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:indirectBufferOffset:",
	"drawMeshThreadgroups:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
	"drawMeshThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
	"drawMeshThreads:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
	"drawPatches:patchIndexBuffer:patchIndexBufferOffset:indirectBuffer:indirectBufferOffset:",
	"drawPatches:patchStart:patchCount:patchIndexBuffer:patchIndexBufferOffset:instanceCount:baseInstance:",
	"drawPrimitives:indirectBuffer:indirectBufferOffset:",
	"drawPrimitives:vertexStart:vertexCount:",
	"drawPrimitives:vertexStart:vertexCount:instanceCount:",
	"drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:",
	"executeCommandsInBuffer:indirectBuffer:indirectBufferOffset:",
	"executeCommandsInBuffer:withRange:",
	"memoryBarrierWithResources:count:afterStages:beforeStages:",
	"memoryBarrierWithScope:afterStages:beforeStages:",
	"sampleCountersInBuffer:atSampleIndex:withBarrier:", "setBlendColorRed:green:blue:alpha:",
	"setColorStoreAction:atIndex:", "setColorStoreActionOptions:atIndex:", "setCullMode:",
	"setDepthBias:slopeScale:clamp:", "setDepthClipMode:", "setDepthStencilState:",
	"setDepthStoreAction:", "setDepthStoreActionOptions:",
	"setFragmentAccelerationStructure:atBufferIndex:", "setFragmentBuffer:offset:atIndex:",
	"setFragmentBufferOffset:atIndex:", "setFragmentBuffers:offsets:withRange:",
	"setFragmentBytes:length:atIndex:", "setFragmentIntersectionFunctionTable:atBufferIndex:",
	"setFragmentIntersectionFunctionTables:withBufferRange:", "setFragmentSamplerState:atIndex:",
	"setFragmentSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setFragmentSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
	"setFragmentSamplerStates:withRange:", "setFragmentTexture:atIndex:",
	"setFragmentTextures:withRange:", "setFragmentVisibleFunctionTable:atBufferIndex:",
	"setFragmentVisibleFunctionTables:withBufferRange:", "setFrontFacingWinding:",
	"setMeshBuffer:offset:atIndex:", "setMeshBufferOffset:atIndex:",
	"setMeshBuffers:offsets:withRange:", "setMeshBytes:length:atIndex:",
	"setMeshSamplerState:atIndex:", "setMeshSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setMeshSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
	"setMeshSamplerStates:withRange:", "setMeshTexture:atIndex:", "setMeshTextures:withRange:",
	"setObjectBuffer:offset:atIndex:", "setObjectBufferOffset:atIndex:",
	"setObjectBuffers:offsets:withRange:", "setObjectBytes:length:atIndex:",
	"setObjectSamplerState:atIndex:", "setObjectSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setObjectSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
	"setObjectSamplerStates:withRange:", "setObjectTexture:atIndex:",
	"setObjectTextures:withRange:", "setObjectThreadgroupMemoryLength:atIndex:",
	"setRenderPipelineState:", "setScissorRect:", "setScissorRects:count:",
	"setStencilFrontReferenceValue:backReferenceValue:", "setStencilReferenceValue:",
	"setStencilStoreAction:", "setStencilStoreActionOptions:",
	"setTessellationFactorBuffer:offset:instanceStride:", "setTessellationFactorScale:",
	"setThreadgroupMemoryLength:offset:atIndex:", "setTileAccelerationStructure:atBufferIndex:",
	"setTileBuffer:offset:atIndex:", "setTileBufferOffset:atIndex:",
	"setTileBuffers:offsets:withRange:", "setTileBytes:length:atIndex:",
	"setTileIntersectionFunctionTable:atBufferIndex:",
	"setTileIntersectionFunctionTables:withBufferRange:", "setTileSamplerState:atIndex:",
	"setTileSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setTileSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
	"setTileSamplerStates:withRange:", "setTileTexture:atIndex:", "setTileTextures:withRange:",
	"setTileVisibleFunctionTable:atBufferIndex:",
	"setTileVisibleFunctionTables:withBufferRange:", "setTriangleFillMode:",
	"setVertexAccelerationStructure:atBufferIndex:", "setVertexAmplificationCount:viewMappings:",
	"setVertexBuffer:offset:atIndex:", "setVertexBufferOffset:atIndex:",
	"setVertexBuffers:offsets:withRange:", "setVertexBytes:length:atIndex:",
	"setVertexIntersectionFunctionTable:atBufferIndex:",
	"setVertexIntersectionFunctionTables:withBufferRange:", "setVertexSamplerState:atIndex:",
	"setVertexSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setVertexSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
	"setVertexSamplerStates:withRange:", "setVertexTexture:atIndex:",
	"setVertexTextures:withRange:", "setVertexVisibleFunctionTable:atBufferIndex:",
	"setVertexVisibleFunctionTables:withBufferRange:", "setViewport:", "setViewports:count:",
	"setVisibilityResultMode:offset:", "textureBarrier", "tileHeight", "tileWidth",
	"updateFence:afterStages:", "useHeap:", "useHeap:stages:", "useHeaps:count:",
	"useHeaps:count:stages:", "useResource:usage:", "useResource:usage:stages:",
	"useResources:count:usage:", "useResources:count:usage:stages:",
	"waitForFence:beforeStages:",
};
static const char *const spi_rendercommandencoderspi[] = {   /* MTLRenderCommandEncoderSPI: 34 required */
	"addSplitHandler:",
	"drawMeshThreadgroups:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
	"drawMeshThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
	"drawMeshThreads:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
	"isMemorylessRender", "setDepthClipModeSPI:",
	"setFragmentTexture:atTextureIndex:samplerState:atSamplerIndex:", "setLineWidth:",
	"setMeshBuffer:offset:atIndex:", "setMeshBufferOffset:atIndex:",
	"setMeshBuffers:offsets:withRange:", "setMeshBytes:length:atIndex:",
	"setMeshSamplerState:atIndex:", "setMeshSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setMeshSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
	"setMeshSamplerStates:withRange:", "setMeshTexture:atIndex:", "setMeshTextures:withRange:",
	"setMeshVisibleFunctionTable:atBufferIndex:",
	"setMeshVisibleFunctionTables:withBufferRange:", "setObjectBuffer:offset:atIndex:",
	"setObjectBufferOffset:atIndex:", "setObjectBuffers:offsets:withRange:",
	"setObjectBytes:length:atIndex:", "setObjectSamplerState:atIndex:",
	"setObjectSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
	"setObjectSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
	"setObjectSamplerStates:withRange:", "setObjectTexture:atIndex:",
	"setObjectTextures:withRange:", "setObjectThreadgroupMemoryLength:atIndex:",
	"setObjectVisibleFunctionTable:atBufferIndex:",
	"setObjectVisibleFunctionTables:withBufferRange:", "setVertexAmplificationMode:value:",
};
static const char *const spi_renderpipelinestatespi[] = {   /* MTLRenderPipelineStateSPI: 32 required */
	"allocatedSize", "fragmentDebugInstrumentationData", "fragmentFunctionHandleWithFunction:",
	"getFragmentShaderTelemetryID", "getVertexShaderTelemetryID", "gpuHandle",
	"maxTotalThreadsPerMeshThreadgroup", "maxTotalThreadsPerObjectThreadgroup",
	"meshDebugInstrumentationData", "meshFunctionHandleWithFunction:",
	"meshThreadExecutionWidth", "newFragmentIntersectionFunctionTableWithDescriptor:",
	"newFragmentShaderDebugInfo",
	"newRenderPipelineStateWithAdditionalBinaryFunctions:fragmentAdditionalBinaryFunctions:error:",
	"newTileIntersectionFunctionTableWithDescriptor:",
	"newTileRenderPipelineStateWithAdditionalBinaryFunctions:error:",
	"newVertexIntersectionFunctionTableWithDescriptor:", "newVertexShaderDebugInfo",
	"newVisibleFunctionTableFromFragmentStageWithDescriptor:",
	"newVisibleFunctionTableFromMeshStageWithDescriptor:",
	"newVisibleFunctionTableFromObjectStageWithDescriptor:",
	"newVisibleFunctionTableFromTileStageWithDescriptor:",
	"newVisibleFunctionTableFromVertexStageWithDescriptor:", "objectDebugInstrumentationData",
	"objectFunctionHandleWithFunction:", "objectThreadExecutionWidth",
	"textureWriteRoundingMode", "tileDebugInstrumentationData",
	"tileFunctionHandleWithFunction:", "uniqueIdentifier", "vertexDebugInstrumentationData",
	"vertexFunctionHandleWithFunction:",
};

static const struct rw_protocol rw_protocols[] = {
	RW_PROTOCOL("MTLCommandBufferSPI", spi_commandbufferspi),
	RW_PROTOCOL("MTLCommandQueue", spi_commandqueue),
	RW_PROTOCOL("MTLCommandQueueSPI", spi_commandqueuespi),
	RW_PROTOCOL("MTLComputeCommandEncoder", spi_computecommandencoder),
	RW_PROTOCOL("MTLComputeCommandEncoderSPI", spi_computecommandencoderspi),
	RW_PROTOCOL("MTLComputePipelineStateSPI", spi_computepipelinestatespi),
	RW_PROTOCOL("MTLPipelineStateWithProgramAddressTable", spi_pipelinestatewithprogramaddresstable),
	RW_PROTOCOL("MTLRenderCommandEncoder", spi_rendercommandencoder),
	RW_PROTOCOL("MTLRenderCommandEncoderSPI", spi_rendercommandencoderspi),
	RW_PROTOCOL("MTLRenderPipelineStateSPI", spi_renderpipelinestatespi),
};

static const char *const chain_device[] = {
	"MTLDeviceSPI", "MTLDevice", NULL,
};
static const char *const chain_commandqueue[] = {
	"MTLCommandQueueSPI", "MTLCommandQueue", NULL,
};
static const char *const chain_commandbuffer[] = {
	"MTLCommandBufferSPI", "MTLCommandBuffer", NULL,
};
static const char *const chain_renderencoder[] = {
	"MTLRenderCommandEncoderSPI", "MTLCommandEncoderSPI", "MTLCommandEncoder",
	"MTLRenderCommandEncoder", NULL,
};
static const char *const chain_computeencoder[] = {
	"MTLComputeCommandEncoderSPI", "MTLCommandEncoderSPI", "MTLCommandEncoder",
	"MTLComputeCommandEncoder", NULL,
};
static const char *const chain_renderpipeline[] = {
	"MTLRenderPipelineStateSPI", "MTLRenderPipelineState",
	"MTLPipelineStateWithProgramAddressTable", NULL,
};
static const char *const chain_computepipeline[] = {
	"MTLComputePipelineStateSPI", "MTLComputePipelineState",
	"MTLPipelineStateWithProgramAddressTable", NULL,
};
static const char *const chain_texture[] = {
	"MTLTextureSPI", "MTLTexture", "MTLResource", "MTLResourceSPI", NULL,
};
static const char *const chain_buffer[] = {
	"MTLBuffer", "MTLResource", "MTLResourceSPI", NULL,
};
static const char *const chain_sampler[] = {
	"MTLSamplerStateSPI", "MTLSamplerState", NULL,
};
static const char *const chain_depthstencilstate[] = {
	"MTLDepthStencilStateSPI", "MTLDepthStencilState", NULL,
};

static const struct rw_class rw_classes[] = {
	{ "HKMetalDevice", chain_device },
	{ "HKMetalCommandQueue", chain_commandqueue },
	{ "HKMetalCommandBuffer", chain_commandbuffer },
	{ "HKMetalRenderEncoder", chain_renderencoder },
	{ "HKMetalComputeEncoder", chain_computeencoder },
	{ "HKMetalRenderPipeline", chain_renderpipeline },
	{ "HKMetalComputePipeline", chain_computepipeline },
	{ "HKMetalTexture", chain_texture },
	{ "HKMetalBuffer", chain_buffer },
	{ "HKMetalSampler", chain_sampler },
	{ "HKMetalDepthStencilState", chain_depthstencilstate },
};

/* The target of the first branch out of this body, which for an abstract method of Apple's
 * is the __objc_stubs entry for doesNotRecognizeSelector:. B and BL are decoded in place --
 * imm26, signed, in units of four bytes. (A11 has no pointer authentication, so an IMP is
 * an address and can simply be read.) */
static uintptr_t
first_external_branch(IMP imp)
{
	const uint32_t *words = (const uint32_t *)(uintptr_t)imp;

	if (imp == NULL) {
		return 0;
	}
	for (int i = 0; i < 10; i++) {
		uint32_t word = words[i];

		if ((word >> 26) == 0x05 || (word >> 26) == 0x25) {   /* B, BL */
			intptr_t offset = (intptr_t)((int32_t)(word << 6) >> 6) * 4;
			uintptr_t target = (uintptr_t)(words + i) + offset;

			if (target < (uintptr_t)words || target > (uintptr_t)(words + 10)) {
				return target;
			}
		}
		if (word == 0xd65f03c0) {   /* ret: the body ended and called nothing out */
			break;
		}
	}
	return 0;
}

/*
 * The stub every abstract method of Apple's calls, learned rather than assumed: two methods
 * that are known to be abstract in this Metal are read, and their first outward branch has
 * to be the same address. If it is not, the check is switched off and says so -- a wrong
 * stub address would call every method in the chain abstract, which is the kind of number
 * that looks like a finding.
 *
 * One stub is enough because every base class this driver inherits from (_MTLDevice,
 * _MTLCommandQueue, _MTLCommandBuffer, _MTLCommandEncoder, _MTLResource,
 * _MTLObjectWithLabel, _MTLSamplerState, _MTLDepthStencilState, and the two pipeline
 * states) lives in Metal.framework, and __objc_stubs is per image. A base class from
 * another image -- IOGPU, say, if this driver ever inherits one of its classes -- would
 * have its own stub and would be missed here, which is why the two methods read are both
 * Metal's and why this says so.
 */
static uintptr_t
abstract_stub(void)
{
	static uintptr_t stub;
	static int looked;

	if (!looked) {
		/* -[_MTLDevice initWithAcceleratorPort:] is the abstract method this whole
		 * subsystem started from (10.1); -[_MTLCommandEncoder setMeshTexture:atIndex:]
		 * is one of the 27 mesh-stage entries the driver now overrides. */
		const char *classes[2] = { "_MTLDevice", "_MTLCommandEncoder" };
		const char *selectors[2] = { "initWithAcceleratorPort:",
					     "setMeshTexture:atIndex:" };
		uintptr_t found[2] = { 0, 0 };

		looked = 1;
		for (int i = 0; i < 2; i++) {
			Class cls = objc_getClass(classes[i]);
			Method method = cls != Nil ?
			    class_getInstanceMethod(cls, sel_getUid(selectors[i])) : NULL;

			if (method != NULL) {
				found[i] = first_external_branch(
				    method_getImplementation(method));
			}
		}
		if (found[0] != 0 && found[0] == found[1]) {
			stub = found[0];
			printf("drvtest: sweep: Apple's doesNotRecognizeSelector: stub is at "
			       "%#lx, agreed by both of the methods known to be abstract\n",
			       (unsigned long)stub);
		} else {
			printf("drvtest: sweep: the two methods known to be abstract branch to "
			       "%#lx and %#lx, so the stub could not be identified and no "
			       "method will be CALLED abstract below (the unanswered count is "
			       "unaffected)\n", (unsigned long)found[0], (unsigned long)found[1]);
		}
	}
	return stub;
}

/* Is what answers this selector one of Apple's abstract entries -- an abort wearing a
 * method table entry? */
static int
answer_is_abstract(Class cls, SEL selector)
{
	Method method = class_getInstanceMethod(cls, selector);
	uintptr_t stub = abstract_stub();

	return stub != 0 && method != NULL &&
	       first_external_branch(method_getImplementation(method)) == stub;
}

/* The required selectors of one protocol: the runtime's copy when this process has the
 * protocol, and otherwise the list read out of the cache. Returns 0 only when neither has
 * it, which is itself worth printing. */
static unsigned
required_selectors(const char *name, SEL *into, unsigned room, const char **source)
{
	Protocol *proto = objc_getProtocol(name);
	unsigned n = 0, taken = 0;

	*source = "runtime";
	if (proto != NULL) {
		struct objc_method_description *req =
		    protocol_copyMethodDescriptionList(proto, YES, YES, &n);

		for (unsigned i = 0; i < n && taken < room; i++) {
			into[taken++] = req[i].name;
		}
		free(req);
		/* and if the cache said something different about the same protocol, say so:
		 * one of the two is then out of date, and it matters which */
		for (unsigned i = 0; i < sizeof(rw_protocols) / sizeof(rw_protocols[0]); i++) {
			if (strcmp(rw_protocols[i].name, name) == 0 &&
			    rw_protocols[i].count != n) {
				printf("drvtest: sweep: %s: the runtime requires %u methods and "
				       "the cache said %u; the runtime is used\n", name, n,
				       rw_protocols[i].count);
			}
		}
		return taken;
	}
	for (unsigned i = 0; i < sizeof(rw_protocols) / sizeof(rw_protocols[0]); i++) {
		if (strcmp(rw_protocols[i].name, name) != 0) {
			continue;
		}
		*source = "the cache";
		for (unsigned j = 0; j < rw_protocols[i].count && taken < room; j++) {
			into[taken++] = sel_getUid(rw_protocols[i].selectors[j]);
		}
		return taken;
	}
	*source = "nowhere";
	return 0;
}

/* One class, every protocol of it, counted. No instance is needed and none is made: what
 * `[object respondsToSelector:]` answers is what the class chain answers, so asking the
 * class cannot be defeated by a nil object or by a GPU that is not there. */
static void
sweep_class(const struct rw_class *entry)
{
	Class cls = objc_getClass(entry->name);
	SEL selectors[768];
	unsigned total = 0, unanswered = 0, abstract = 0, shown = 0;
	char which[256] = { 0 };

	if (cls == Nil) {
		printf("drvtest: sweep: %s IS NOT IN THIS PROCESS, so nothing of it was "
		       "counted\n", entry->name);
		return;
	}
	for (unsigned p = 0; entry->protocols[p] != NULL; p++) {
		const char *source = "";
		SEL one[768];
		unsigned n = required_selectors(entry->protocols[p], one,
						sizeof(one) / sizeof(one[0]), &source);

		if (n == 0) {
			printf("drvtest: sweep: %s: no list of %s exists in this process or in "
			       "the compiled-in tables, so its methods were NOT counted\n",
			       entry->name, entry->protocols[p]);
			continue;
		}
		snprintf(which + strlen(which), sizeof(which) - strlen(which), "%s%s(%u,%s)",
			 p == 0 ? "" : "+", entry->protocols[p], n, source);
		for (unsigned i = 0; i < n; i++) {
			int seen = 0;

			/* a selector required by two protocols of the same class is one
			 * method, and counting it twice would inflate every number here;
			 * SELs are interned, so comparing the pointers is comparing the names */
			for (unsigned j = 0; j < total; j++) {
				if (selectors[j] == one[i]) {
					seen = 1;
					break;
				}
			}
			if (seen || total >= sizeof(selectors) / sizeof(selectors[0])) {
				continue;
			}
			selectors[total++] = one[i];
			if (!class_respondsToSelector(cls, one[i])) {
				unanswered++;
				if (shown < 8) {
					printf("drvtest: sweep:   %s still owes %s\n",
					       entry->name, sel_getName(one[i]));
					shown++;
				}
			} else if (answer_is_abstract(cls, one[i])) {
				abstract++;
				if (shown < 8) {
					printf("drvtest: sweep:   %s answers %s with one of "
					       "Apple's abstract stubs, which aborts when it is "
					       "called\n", entry->name, sel_getName(one[i]));
					shown++;
				}
			}
		}
	}
	printf("drvtest: sweep: of %u methods %s requires, %u are unanswered and %u are "
	       "answered by an abstract stub of Apple's [%s]\n", total, entry->name,
	       unanswered, abstract, which);
}

/*
 * Every class the driver has, on every run. The numbers belong in the log of each boot
 * rather than in a measurement someone took once: the protocols are the system's, and the
 * system is what changes under us.
 */
static void
sweep_every_class(void)
{
	unsigned classes = sizeof(rw_classes) / sizeof(rw_classes[0]);

	printf("drvtest: sweep: %u classes, counted the way 10.17 counted the device: "
	       "unanswered means absent from the chain, and an entry whose body refuses is "
	       "answered\n", classes);
	for (unsigned i = 0; i < classes; i++) {
		sweep_class(&rw_classes[i]);
	}
}

/*
 * ==========================================================================================
 * --burst [N] (M4-1): COMMAND BUFFERS NOBODY WAITS FOR, AND WHAT THEY HELD
 * ==========================================================================================
 *
 * Run 071937: --time on the Vulkan back end faulted the GPU's SMMU in four processes of five,
 * right after tm_queue's 256 empty command buffers, in the next case's -getBytes:. That mode
 * reaches the case after a dozen others and some 400 lines of log, so this is the same shape
 * alone, and a little more of it (air_vk.c's M4-1 entry has the cause):
 *
 *   (a) One clear, committed and not waited for, so there is a submission for the empty ones
 *       to follow, as there was in --time. Then N empty command buffers, made, committed and
 *       dropped with no wait, exactly tm_queue's loop -- on the old library each of them left
 *       a wait on the completion queue, and that wait recorded and submitted on a second
 *       thread. Then, AT ONCE and with no wait, what
 *       tm_present does next: a panel-sized texture made and read back. And eight small
 *       reads, because a read is the thing that records outside the driver's rw_pipe_lock.
 *   (b) N command buffers each with one small piece of real work, committed with no wait: odd
 *       ones a clear pass with no draw (no shader, so this mode needs no library), even ones a
 *       buffer copied into a texture, and each then that texture copied into one that stays.
 *       What each used -- its texture, and its buffer if it had one -- is released right after
 *       -commit, which Metal allows: the command buffer holds them, so it is usually the
 *       completion queue that lets go of them last and destroys them, on the old library by
 *       recording there too. Every sixteenth round, a small read on this thread as well.
 *   (c) Real work, waited for: a clear of the panel-sized target and the small texture copied
 *       into it, and the pixels read back. The small one must hold what the LAST of (b) wrote,
 *       which is only so if all of (b) ran, in order; and each of (b) must have completed
 *       without an error (counted by a handler) and the last of (a) too.
 *
 * "ok" here is about THIS PROCESS: right pixels and no command buffer in error. A fault shows
 * first in the kernel's own "THE SMMU REFUSED" line in the same log, and here only once turnip
 * has seen it and says VK_ERROR_DEVICE_LOST, so a run is clean when this line says ok AND that
 * line is absent.
 */
enum { BURST_SIDE = 16 };

static _Atomic unsigned burst_completed, burst_failed;

/* What the i-th of (b) writes, in memory order (BGRA8): different from its neighbours. */
static void
burst_pattern(unsigned i, uint8_t bgra[4])
{
	bgra[0] = (uint8_t)(i * 37 + 11);
	bgra[1] = (uint8_t)(i * 101 + 3);
	bgra[2] = (uint8_t)(i * 13 + 200);
	bgra[3] = 255;
}

/* One step either way, for a clear: its colour is a float on the way there. */
static bool
burst_same(const uint8_t got[4], const uint8_t want[4])
{
	for (unsigned i = 0; i < 4; i++) {
		if (abs((int)got[i] - (int)want[i]) > 1) {
			return false;
		}
	}
	return true;
}

static int
burst_test(id<MTLDevice> dev, unsigned n)
{
	const char *backend = backend_is_vk() ? "vk" : "gallium";
	const size_t panel_bytes = (size_t)PANEL_W * PANEL_H * 4;
	const MTLRegion whole = MTLRegionMake2D(0, 0, BURST_SIDE, BURST_SIDE);
	id<MTLCommandQueue> queue = [dev newCommandQueue];
	MTLTextureDescriptor *small =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
							       width:BURST_SIDE
							      height:BURST_SIDE
							   mipmapped:NO];
	MTLTextureDescriptor *panel =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
							       width:PANEL_W
							      height:PANEL_H
							   mipmapped:NO];

	[small setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];
	[panel setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];

	id<MTLTexture> keep = [dev newTextureWithDescriptor:small];
	id<MTLTexture> target = [dev newTextureWithDescriptor:panel];
	uint8_t *host = malloc(panel_bytes);
	uint8_t texels[BURST_SIDE * BURST_SIDE * 4];

	if (queue == nil || keep == nil || target == nil || host == NULL) {
		printf("drvtest: burst-result %s FAILED there is no queue, no 16x16 and 1080x2400 "
		       "texture or no memory, so nothing was tried\n", backend);
		fflush(stdout);
		return 1;
	}
	printf("drvtest: burst: %u empty command buffers, then %u small ones, none waited for\n",
	       n, n);
	fflush(stdout);

	/* (a): work the empty ones follow, then tm_queue's own loop, keeping the last so that
	 * its end can be asked about */
	@autoreleasepool {
		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];

		pass.colorAttachments[0].texture = keep;
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
		[[cmd renderCommandEncoderWithDescriptor:pass] endEncoding];
		[cmd commit];
	}

	id<MTLCommandBuffer> last_empty = nil;
	uint64_t t0 = tm_now_ns();

	for (unsigned i = 0; i < n; i++) {
		@autoreleasepool {
			id<MTLCommandBuffer> cmd = [queue commandBuffer];

			[cmd commit];
			if (i == n - 1) {
				last_empty = [cmd retain];
			}
		}
	}
	uint64_t t1 = tm_now_ns();

	/* and what came next in the run that faulted, with nothing waited for in between */
	{
		id<MTLTexture> frame = [dev newTextureWithDescriptor:panel];

		[frame getBytes:host
		    bytesPerRow:PANEL_W * 4
		     fromRegion:MTLRegionMake2D(0, 0, PANEL_W, PANEL_H)
		    mipmapLevel:0];
		[frame release];
	}
	for (unsigned i = 0; i < 8; i++) {
		[keep getBytes:texels bytesPerRow:BURST_SIDE * 4 fromRegion:whole mipmapLevel:0];
	}
	uint64_t t2 = tm_now_ns();

	/* (b) */
	unsigned refused = 0;

	for (unsigned i = 0; i < n; i++) {
		@autoreleasepool {
			uint8_t want[4];
			id<MTLTexture> tex = [dev newTextureWithDescriptor:small];
			id<MTLBuffer> src = nil;
			id<MTLCommandBuffer> cmd = [queue commandBuffer];

			burst_pattern(i, want);
			if ((i & 1) != 0 && tex != nil) {
				MTLRenderPassDescriptor *pass =
				    [MTLRenderPassDescriptor renderPassDescriptor];

				pass.colorAttachments[0].texture = tex;
				pass.colorAttachments[0].loadAction = MTLLoadActionClear;
				pass.colorAttachments[0].storeAction = MTLStoreActionStore;
				pass.colorAttachments[0].clearColor =
				    MTLClearColorMake(want[2] / 255.0, want[1] / 255.0,
						      want[0] / 255.0, 1);
				[[cmd renderCommandEncoderWithDescriptor:pass] endEncoding];
			} else if (tex != nil) {
				src = [dev newBufferWithLength:sizeof(texels)
						       options:MTLResourceStorageModeShared];
			}
			if (tex == nil || ((i & 1) == 0 && src == nil)) {
				refused++;
				[cmd commit];
				[src release];
				[tex release];
				continue;
			}
			id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];

			if (src != nil) {
				uint8_t *p = [src contents];

				for (unsigned t = 0; t < BURST_SIDE * BURST_SIDE; t++) {
					memcpy(p + t * 4, want, 4);
				}
				[blit copyFromBuffer:src
					sourceOffset:0
				   sourceBytesPerRow:BURST_SIDE * 4
				 sourceBytesPerImage:sizeof(texels)
					  sourceSize:MTLSizeMake(BURST_SIDE, BURST_SIDE, 1)
					   toTexture:tex
				    destinationSlice:0
				    destinationLevel:0
				   destinationOrigin:MTLOriginMake(0, 0, 0)];
			}
			[blit copyFromTexture:tex
				  sourceSlice:0
				  sourceLevel:0
				 sourceOrigin:MTLOriginMake(0, 0, 0)
				   sourceSize:MTLSizeMake(BURST_SIDE, BURST_SIDE, 1)
				    toTexture:keep
			     destinationSlice:0
			     destinationLevel:0
			    destinationOrigin:MTLOriginMake(0, 0, 0)];
			[blit endEncoding];
			[cmd addCompletedHandler:^(id<MTLCommandBuffer> c) {
				if ([c status] == MTLCommandBufferStatusCompleted &&
				    [c error] == nil) {
					atomic_fetch_add(&burst_completed, 1);
				} else {
					atomic_fetch_add(&burst_failed, 1);
				}
			}];
			[cmd commit];
			[src release];
			[tex release];
			if (i % 16 == 15) {
				[keep getBytes:texels
				   bytesPerRow:BURST_SIDE * 4
				    fromRegion:whole
				   mipmapLevel:0];
			}
		}
	}
	uint64_t t3 = tm_now_ns();

	/* (c): the last of (b) is what `keep` holds once all of (b) has run, in order */
	uint8_t want_small[4], want_clear[4] = { 191, 64, 128, 255 };
	uint8_t got_clear[4] = { 0 }, got_copied[4] = { 0 }, got_small[4] = { 0 };
	NSUInteger final_status = 0;

	burst_pattern(n - 1, want_small);
	@autoreleasepool {
		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];

		pass.colorAttachments[0].texture = target;
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		pass.colorAttachments[0].clearColor = MTLClearColorMake(0.5, 0.25, 0.75, 1);
		[[cmd renderCommandEncoderWithDescriptor:pass] endEncoding];

		id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];

		[blit copyFromTexture:keep
			  sourceSlice:0
			  sourceLevel:0
			 sourceOrigin:MTLOriginMake(0, 0, 0)
			   sourceSize:MTLSizeMake(BURST_SIDE, BURST_SIDE, 1)
			    toTexture:target
		     destinationSlice:0
		     destinationLevel:0
		    destinationOrigin:MTLOriginMake(32, 32, 0)];
		[blit endEncoding];
		[cmd commit];
		[cmd waitUntilCompleted];
		final_status = [cmd status];
	}
	[target getBytes:got_clear
	     bytesPerRow:PANEL_W * 4
	      fromRegion:MTLRegionMake2D(0, 0, 1, 1)
	     mipmapLevel:0];
	[target getBytes:got_copied
	     bytesPerRow:PANEL_W * 4
	      fromRegion:MTLRegionMake2D(32, 32, 1, 1)
	     mipmapLevel:0];
	[keep getBytes:got_small
	   bytesPerRow:BURST_SIDE * 4
	    fromRegion:MTLRegionMake2D(0, 0, 1, 1)
	   mipmapLevel:0];

	/* The handlers run on the completion queue, in order, and may still be on their way. */
	unsigned expected = n - refused;

	for (unsigned i = 0; i < 5000 && atomic_load(&burst_completed) +
					  atomic_load(&burst_failed) < expected; i++) {
		usleep(1000);
	}
	for (unsigned i = 0; i < 5000 && [last_empty status] < MTLCommandBufferStatusCompleted; i++) {
		usleep(1000);
	}

	unsigned completed = atomic_load(&burst_completed), failed = atomic_load(&burst_failed);
	NSUInteger empty_status = [last_empty status];
	bool ok = refused == 0 && completed == n && failed == 0 &&
		  final_status == MTLCommandBufferStatusCompleted &&
		  empty_status == MTLCommandBufferStatusCompleted &&
		  burst_same(got_clear, want_clear) && burst_same(got_copied, want_small) &&
		  burst_same(got_small, want_small);

	print_vk_counts("after the burst, with its last empty command buffer still held");
	printf("drvtest: burst-result %s %s empty %u in %.1f ms then the reads in %.1f ms; small "
	       "%u in %.1f ms: %u completed, %u in error, %u not made; last empty status %lu, "
	       "final status %lu (completed is %lu); clear %u %u %u %u want %u %u %u %u, copied "
	       "%u %u %u %u and small %u %u %u %u want %u %u %u %u\n", backend, ok ? "ok" : "FAILED",
	       n, (t1 - t0) / 1e6, (t2 - t1) / 1e6, n, (t3 - t2) / 1e6, completed, failed, refused,
	       (unsigned long)empty_status, (unsigned long)final_status,
	       (unsigned long)MTLCommandBufferStatusCompleted,
	       got_clear[0], got_clear[1], got_clear[2], got_clear[3],
	       want_clear[0], want_clear[1], want_clear[2], want_clear[3],
	       got_copied[0], got_copied[1], got_copied[2], got_copied[3],
	       got_small[0], got_small[1], got_small[2], got_small[3],
	       want_small[0], want_small[1], want_small[2], want_small[3]);
	fflush(stdout);
	[last_empty release];
	[keep release];
	[target release];
	[queue release];
	free(host);
	return ok ? 0 : 1;
}

/*
 * ==========================================================================================
 * --order [METALLIB] (M4-1, the cost part): WHAT -replaceRegion: AND A DESTROY PROMISE
 * ==========================================================================================
 *
 * air_vk.c no longer submits and waits at each -replaceRegion: and each destroy (UPLOADS and
 * when_done there): a write goes ahead of whatever is recorded next, a destroy waits for a fence
 * by itself. That is a change in WHEN, and what an application may see of it is not allowed to
 * change. So these are the orders Metal promises, each said by an application's calls and read
 * back, and each must hold on both back ends (Gallium's texture_subdata is the reference: a
 * case that fails there is the case's own fault or Gallium's, and the line says which back end):
 *
 *   blit-wait      write, then at once a blit that copies it out, -commit, wait, read the
 *                  destination: every round's own bytes, a different pattern each round
 *   blit-nowait    the same with no wait between the rounds, a destination per round: each
 *                  holds its own round's bytes. A later write must not reach an earlier
 *                  command buffer (in Metal that is the application's hazard; here the two
 *                  are in order on the queue and must stay so)
 *   draw-wait      write, then at once a draw that samples it (texturedFragment2DUInt writes
 *                  the texel to four RGBA32Uint targets, a texture of one colour, so the
 *                  coordinate does not matter), -commit, wait, read a target back
 *   draw-nowait    as blit-nowait
 *   encoded-blit   a blit ENCODED, not committed; THEN the write; then -commit: the new bytes
 *   encoded-draw   the same with a draw: this is the case an upload "ahead of the recording"
 *                  is for
 *   rows           64 rows of a 64x64 texture written one call each, read at once; written again
 *                  bottom up, with a block over them, which overlaps and has an order to keep,
 *                  read at once and copied out; then 1024 rows, again overlapping. Its cost line
 *                  says how many submissions that took (WHAT A WORKLOAD COSTS): on vk it should
 *                  be a handful and no longer 1024
 *   big            a panel-sized write and read, over THE STAGING RING's largest piece
 *   ring           64 writes of 256 KB with no -commit between them, four times the ring,
 *                  into four textures in turn, then each read: the ring full, backpressure
 *   release        a texture made, written and released with no -commit, 64 times (a destroy of
 *                  what the open upload names); and written, copied, committed, released with no
 *                  wait (what a command buffer holds), then the copy read
 *   contract       the destroy of a buffer that unfinished work names, which Metal's objects
 *                  never do because a command buffer holds what it uses, so on the contract's
 *                  own calls, on the driver's own device: a 1 MB buffer copied from and a 8 MB one
 *                  filled, each destroyed right after rwa_flush, its fence waited for after. If
 *                  the memory went early that is an SMMU fault: this line is ok AND no
 *                  "THE SMMU REFUSED" in the log. The longest destroy is printed: no wait in it
 *   surface        a texture over an IOSurface, written, and the surface read by the processor at
 *                  once, past its cache with an inline DC CIVAC and DSB SY (sys_dcache_flush is a
 *                  barrier and a return on this system): that reader cannot be told to wait for a
 *                  flush, so air_vk.c's write is at once there
 *   threads        vk only. Two threads writing, blitting, committing and reading their own
 *                  textures on their own queues, and a third only writing and reading its own,
 *                  all at once: -replaceRegion: and -getBytes: take no lock in hkmetal.m. On
 *                  Gallium they share the context unguarded (hkmetal.m, over rw_pipe_lock), so this
 *                  is not tried there, and the line says so; that is not a result about Gallium.
 *
 * Ends in "drvtest: order-result BACKEND ok|FAILED ..." with each case's own line before it.
 */
enum { ORDER_SIDE = 16, ORDER_ROUNDS = 8, ORDER_ROWS = 64, ORDER_RING_SIDE = 256 };

static unsigned order_cases, order_bad;
static char order_bad_names[256];

static void order_case(const char *name, bool ok, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void
order_case(const char *name, bool ok, const char *fmt, ...)
{
	va_list ap;

	order_cases++;
	if (!ok) {
		order_bad++;
		strlcat(order_bad_names, " ", sizeof(order_bad_names));
		strlcat(order_bad_names, name, sizeof(order_bad_names));
	}
	printf("drvtest: order %s %s %s: ", backend_is_vk() ? "vk" : "gallium", name,
	       ok ? "ok" : "FAILED");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

/* The BGRA8 texel of (round, x, y): different from its neighbours, in round and in place. */
static void
order_texel(unsigned round, unsigned x, unsigned y, uint8_t bgra[4])
{
	burst_pattern(round * 7 + x * 13 + y * 31, bgra);
}

/* A w x h picture of round `round`, tightly packed */
static void
order_picture(uint8_t *p, unsigned round, unsigned w, unsigned h)
{
	for (unsigned y = 0; y < h; y++) {
		for (unsigned x = 0; x < w; x++) {
			order_texel(round, x, y, p + ((size_t)y * w + x) * 4);
		}
	}
}

/* How many of a w x h picture's texels are not round `round`'s; the first one in *first */
static unsigned
order_wrong(const uint8_t *p, unsigned round, unsigned w, unsigned h, char *first, size_t size)
{
	unsigned wrong = 0;

	for (unsigned y = 0; y < h; y++) {
		for (unsigned x = 0; x < w; x++) {
			uint8_t want[4];
			const uint8_t *got = p + ((size_t)y * w + x) * 4;

			order_texel(round, x, y, want);
			if (memcmp(got, want, 4) != 0) {
				if (wrong++ == 0 && first != NULL) {
					snprintf(first, size, "first at (%u, %u): %u %u %u %u want %u %u %u %u",
						 x, y, got[0], got[1], got[2], got[3], want[0], want[1],
						 want[2], want[3]);
				}
			}
		}
	}
	return wrong;
}

static id<MTLTexture>
order_texture(id<MTLDevice> dev, MTLPixelFormat format, unsigned w, unsigned h)
{
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
							       width:w
							      height:h
							   mipmapped:NO];

	[td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];
	return [dev newTextureWithDescriptor:td];
}

static void
order_write(id<MTLTexture> t, const void *bytes, unsigned w, unsigned h, unsigned texel)
{
	[t replaceRegion:MTLRegionMake2D(0, 0, w, h)
	     mipmapLevel:0
	       withBytes:bytes
	     bytesPerRow:w * texel];
}

static void
order_read(id<MTLTexture> t, void *bytes, unsigned w, unsigned h, unsigned texel)
{
	[t getBytes:bytes bytesPerRow:w * texel fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
}

static void
order_blit(id<MTLCommandBuffer> cmd, id<MTLTexture> from, id<MTLTexture> to, unsigned w, unsigned h)
{
	id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];

	[blit copyFromTexture:from
		  sourceSlice:0
		  sourceLevel:0
		 sourceOrigin:MTLOriginMake(0, 0, 0)
		   sourceSize:MTLSizeMake(w, h, 1)
		    toTexture:to
	     destinationSlice:0
	     destinationLevel:0
	    destinationOrigin:MTLOriginMake(0, 0, 0)];
	[blit endEncoding];
}

/* One texturedFragment2DUInt pass: its four targets cleared and written with what it samples */
static void
order_draw(id<MTLCommandBuffer> cmd, id<MTLRenderPipelineState> ps, id<MTLSamplerState> samp,
	   id<MTLTexture> sampled, __unsafe_unretained id<MTLTexture> targets[4])
{
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];

	for (unsigned i = 0; i < 4; i++) {
		MTLRenderPassColorAttachmentDescriptor *a = [[rp colorAttachments] objectAtIndexedSubscript:i];

		[a setTexture:targets[i]];
		[a setLoadAction:MTLLoadActionClear];
		[a setClearColor:MTLClearColorMake(0, 0, 0, 0)];
		[a setStoreAction:MTLStoreActionStore];
	}
	id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];

	[enc setRenderPipelineState:ps];
	[enc setFragmentTexture:sampled atIndex:0];
	[enc setFragmentSamplerState:samp atIndex:0];
	[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	[enc endEncoding];
}

/* What round `round` is in RGBA32Uint, for the sampled texture and for its targets */
static void
order_words(unsigned round, unsigned word[4])
{
	word[0] = 1000 + round;
	word[1] = 2000 + round * 3;
	word[2] = 3000 + round * 5;
	word[3] = 4000 + round * 7;
}

static unsigned
order_words_wrong(id<MTLTexture> target, unsigned round, char *first, size_t size)
{
	unsigned grid[16][4], want[4], wrong = 0;

	memset(grid, 0, sizeof(grid));
	order_read(target, grid, 4, 4, 16);
	order_words(round, want);
	for (unsigned i = 0; i < 16; i++) {
		if (memcmp(grid[i], want, sizeof(want)) != 0 && wrong++ == 0) {
			snprintf(first, size, "texel %u is %u %u %u %u, want %u %u %u %u", i, grid[i][0],
				 grid[i][1], grid[i][2], grid[i][3], want[0], want[1], want[2], want[3]);
		}
	}
	return wrong;
}

/* The processor's view of a surface past its own cache: DC CIVAC a line at a time, as
 * air_metal_test's civac_range: no ctr_el0 to ask on this system, and 64 is this core's. */
static void
order_civac(const void *p, size_t bytes)
{
	uintptr_t a = (uintptr_t)p & ~(uintptr_t)63;

	for (; a < (uintptr_t)p + bytes; a += 64) {
		__asm__ volatile("dc civac, %0" : : "r"(a));
	}
	__asm__ volatile("dsb sy");
}

static void
order_rows_and_blocks(id<MTLDevice> dev, id<MTLCommandQueue> queue)
{
	enum { S = ORDER_ROWS };
	static uint8_t picture[S * S * 4], back[S * S * 4], expect[S * S * 4];
	id<MTLTexture> t = order_texture(dev, MTLPixelFormatBGRA8Unorm, S, S);
	id<MTLTexture> copy = order_texture(dev, MTLPixelFormatBGRA8Unorm, S, S);
	char first[128] = "";
	unsigned wrong_down, wrong_up = 0, wrong_copy = 0, wrong_many = 0;

	if (t == nil || copy == nil) {
		order_case("rows", false, "no 64x64 textures");
		return;
	}
	/* top to bottom, one call a row, and read at once */
	order_picture(picture, 40, S, S);
	for (unsigned y = 0; y < S; y++) {
		[t replaceRegion:MTLRegionMake2D(0, y, S, 1)
		     mipmapLevel:0
		       withBytes:picture + y * S * 4
		     bytesPerRow:S * 4];
	}
	memset(back, 0, sizeof(back));
	order_read(t, back, S, S, 4);
	wrong_down = order_wrong(back, 40, S, S, first, sizeof(first));

	/* bottom to top, another picture, and a block over it, in that order: the block overlaps
	 * what is written, so it must end up on top */
	order_picture(picture, 41, S, S);
	memcpy(expect, picture, sizeof(expect));
	for (unsigned y = S; y-- > 0;) {
		[t replaceRegion:MTLRegionMake2D(0, y, S, 1)
		     mipmapLevel:0
		       withBytes:picture + y * S * 4
		     bytesPerRow:S * 4];
	}
	{
		static uint8_t block[16 * 16 * 4];

		for (unsigned y = 0; y < 16; y++) {
			for (unsigned x = 0; x < 16; x++) {
				order_texel(42, x, y, block + (y * 16 + x) * 4);
				memcpy(expect + ((y + 8) * S + x + 8) * 4, block + (y * 16 + x) * 4, 4);
			}
		}
		[t replaceRegion:MTLRegionMake2D(8, 8, 16, 16)
		     mipmapLevel:0
		       withBytes:block
		     bytesPerRow:16 * 4];
	}
	@autoreleasepool {
		id<MTLCommandBuffer> cmd = [queue commandBuffer];

		order_blit(cmd, t, copy, S, S);
		[cmd commit];
		[cmd waitUntilCompleted];
	}
	memset(back, 0, sizeof(back));
	order_read(t, back, S, S, 4);
	wrong_up = memcmp(back, expect, sizeof(back)) != 0;
	memset(back, 0, sizeof(back));
	order_read(copy, back, S, S, 4);
	wrong_copy = memcmp(back, expect, sizeof(back)) != 0;

	/* and many: 1024 calls, each row over and over, what WHAT A WORKLOAD COSTS prices */
	cost_begin("Wrows");
	cost_phase(COST_WORK);
	for (unsigned i = 0; i < 1024; i++) {
		unsigned y = i % S;
		uint8_t row[S * 4];

		for (unsigned x = 0; x < S; x++) {
			order_texel(i / S, x, y, row + x * 4);
		}
		[t replaceRegion:MTLRegionMake2D(0, y, S, 1) mipmapLevel:0 withBytes:row bytesPerRow:S * 4];
	}
	memset(back, 0, sizeof(back));
	order_read(t, back, S, S, 4);
	cost_phase(COST_SETUP);
	cost_end("writes", "write", "rows 1024 writes and one read");
	wrong_many = order_wrong(back, 1023 / S, S, S, first, sizeof(first));
	order_case("rows", wrong_down == 0 && !wrong_up && !wrong_copy && wrong_many == 0,
		   "%u of %u texels wrong written down the texture; bottom up with a block on top "
		   "%s, its copy %s; 1024 rows, %u wrong %s", wrong_down, S * S,
		   wrong_up ? "WRONG" : "right", wrong_copy ? "WRONG" : "right", wrong_many, first);
	[t release];
	[copy release];
}

/* The contract's own calls on the driver's device, for a destroy of what unfinished work names */
struct rwa_buffer;
struct rwa_fence;

static void
order_contract(id<MTLDevice> dev)
{
	enum { COPIES = 32, COPIED = 4096, SRC = 1 << 20, FILLED = 8 << 20 };
	void *lib = dlopen(compiler_library(), RTLD_NOW);
	struct rwa_device *device = NULL;
	struct rwa_buffer *(*buffer_create)(struct rwa_device *, size_t, char *, size_t) = NULL;
	void *(*buffer_map)(struct rwa_buffer *) = NULL;
	void (*buffer_destroy)(struct rwa_buffer *) = NULL;
	int (*copy_buffer)(struct rwa_buffer *, size_t, struct rwa_buffer *, size_t, size_t, char *,
			   size_t) = NULL;
	int (*fill_buffer)(struct rwa_buffer *, size_t, size_t, uint8_t, char *, size_t) = NULL;
	int (*flush)(struct rwa_device *, struct rwa_fence **, char *, size_t) = NULL;
	int (*fence_wait)(struct rwa_device *, struct rwa_fence *, char *, size_t) = NULL;
	void (*fence_release)(struct rwa_device *, struct rwa_fence *) = NULL;
	SEL mesa = sel_getUid("mesaDevice");
	char why[256] = "";

	if (lib != NULL) {
		*(void **)&buffer_create = dlsym(lib, "rwa_buffer_create");
		*(void **)&buffer_map = dlsym(lib, "rwa_buffer_map");
		*(void **)&buffer_destroy = dlsym(lib, "rwa_buffer_destroy");
		*(void **)&copy_buffer = dlsym(lib, "rwa_copy_buffer");
		*(void **)&fill_buffer = dlsym(lib, "rwa_fill_buffer");
		*(void **)&flush = dlsym(lib, "rwa_flush");
		*(void **)&fence_wait = dlsym(lib, "rwa_fence_wait");
		*(void **)&fence_release = dlsym(lib, "rwa_fence_release");
	}
	if ([dev respondsToSelector:mesa]) {
		device = ((struct rwa_device *(*)(id, SEL))objc_msgSend)(dev, mesa);
	}
	if (device == NULL || buffer_create == NULL || buffer_map == NULL || buffer_destroy == NULL ||
	    copy_buffer == NULL || fill_buffer == NULL || flush == NULL || fence_wait == NULL ||
	    fence_release == NULL) {
		order_case("contract", false, "this library or device does not answer those calls, so "
			   "nothing was tried");
		return;
	}
	struct rwa_buffer *keep = buffer_create(device, COPIES * COPIED, why, sizeof(why));
	struct rwa_fence *fences[COPIES + 1] = { NULL };
	uint64_t longest = 0;
	unsigned failed = 0, wrong = 0;

	if (keep == NULL) {
		order_case("contract", false, "no buffer: %s", why);
		return;
	}
	for (unsigned i = 0; i < COPIES; i++) {
		struct rwa_buffer *src = buffer_create(device, SRC, why, sizeof(why));

		if (src == NULL) {
			failed++;
			continue;
		}
		memset(buffer_map(src), (int)(0x11 + i), SRC);
		if (copy_buffer(src, SRC - COPIED, keep, i * COPIED, COPIED, why, sizeof(why)) != 0 ||
		    flush(device, &fences[i], why, sizeof(why)) != 0) {
			failed++;
		}
		uint64_t t0 = tm_now_ns();

		buffer_destroy(src);    /* with the copy of it perhaps not begun */

		uint64_t took = tm_now_ns() - t0;

		longest = took > longest ? took : longest;
	}
	{
		struct rwa_buffer *big = buffer_create(device, FILLED, why, sizeof(why));

		if (big == NULL || fill_buffer(big, 0, FILLED, 0x5a, why, sizeof(why)) != 0 ||
		    flush(device, &fences[COPIES], why, sizeof(why)) != 0) {
			failed++;
		}
		uint64_t t0 = tm_now_ns();

		buffer_destroy(big);

		uint64_t took = tm_now_ns() - t0;

		longest = took > longest ? took : longest;
	}
	for (unsigned i = 0; i <= COPIES; i++) {
		if (fences[i] != NULL) {
			if (fence_wait(device, fences[i], why, sizeof(why)) != 0) {
				failed++;
			}
			fence_release(device, fences[i]);
		}
	}
	const uint8_t *k = buffer_map(keep);

	for (unsigned i = 0; i < COPIES; i++) {
		for (unsigned b = 0; b < COPIED; b += 512) {
			wrong += k[i * COPIED + b] != (uint8_t)(0x11 + i);
		}
	}
	order_case("contract", failed == 0 && wrong == 0, "%u buffers copied from and one of 8 MB filled, "
		   "each destroyed right after rwa_flush; the longest destroy %.1f us; %u calls failed "
		   "%s; %u sampled bytes wrong", COPIES, longest / 1e3, failed, why, wrong);
	buffer_destroy(keep);
}

struct order_thread {
	id<MTLDevice> dev;
	unsigned seed, rounds;
	bool commits;                   /* false: only writes and reads of its own texture */
	unsigned wrong, failed;
};

/* What a thread does, for threads: its own queue and textures, so Metal has nothing to say */
static void *
order_thread_main(void *arg)
{
	struct order_thread *t = arg;

	@autoreleasepool {
		id<MTLCommandQueue> queue = [t->dev newCommandQueue];
		id<MTLTexture> s = order_texture(t->dev, MTLPixelFormatBGRA8Unorm, ORDER_SIDE, ORDER_SIDE);
		id<MTLTexture> d = order_texture(t->dev, MTLPixelFormatBGRA8Unorm, ORDER_SIDE, ORDER_SIDE);
		uint8_t picture[ORDER_SIDE * ORDER_SIDE * 4], back[ORDER_SIDE * ORDER_SIDE * 4];

		if (queue == nil || s == nil || d == nil) {
			t->failed++;
			return NULL;
		}
		for (unsigned i = 0; i < t->rounds; i++) {
			unsigned round = t->seed + i;

			order_picture(picture, round, ORDER_SIDE, ORDER_SIDE);
			order_write(s, picture, ORDER_SIDE, ORDER_SIDE, 4);
			if (!t->commits) {
				memset(back, 0, sizeof(back));
				order_read(s, back, ORDER_SIDE, ORDER_SIDE, 4);
				t->wrong += order_wrong(back, round, ORDER_SIDE, ORDER_SIDE, NULL, 0) != 0;
				continue;
			}
			@autoreleasepool {
				id<MTLCommandBuffer> cmd = [queue commandBuffer];

				order_blit(cmd, s, d, ORDER_SIDE, ORDER_SIDE);
				[cmd commit];
				if (i % 4 == 3 || i == t->rounds - 1) {
					[cmd waitUntilCompleted];
					t->failed += [cmd status] != MTLCommandBufferStatusCompleted;
					memset(back, 0, sizeof(back));
					order_read(d, back, ORDER_SIDE, ORDER_SIDE, 4);
					t->wrong += order_wrong(back, round, ORDER_SIDE, ORDER_SIDE, NULL, 0) != 0;
				}
			}
		}
		[s release];
		[d release];
		[queue release];
	}
	return NULL;
}

static int
order_test(id<MTLDevice> dev, const char *metallib)
{
	const char *backend = backend_is_vk() ? "vk" : "gallium";
	static uint8_t picture[ORDER_SIDE * ORDER_SIDE * 4], back[ORDER_SIDE * ORDER_SIDE * 4];
	id<MTLCommandQueue> queue = [dev newCommandQueue];
	id<MTLTexture> src = order_texture(dev, MTLPixelFormatBGRA8Unorm, ORDER_SIDE, ORDER_SIDE);
	id<MTLTexture> dst[ORDER_ROUNDS];
	char first[128];
	unsigned wrong;

	printf("drvtest: order %s: what -replaceRegion: and a destroy promise, library %s\n", backend,
	       metallib);
	fflush(stdout);
	if (queue == nil || src == nil) {
		printf("drvtest: order-result %s FAILED no queue or no texture, nothing was tried\n", backend);
		fflush(stdout);
		return 1;
	}
	for (unsigned i = 0; i < ORDER_ROUNDS; i++) {
		dst[i] = order_texture(dev, MTLPixelFormatBGRA8Unorm, ORDER_SIDE, ORDER_SIDE);
	}

	/* blit-wait */
	wrong = 0;
	first[0] = 0;
	for (unsigned i = 0; i < ORDER_ROUNDS; i++) {
		@autoreleasepool {
			id<MTLCommandBuffer> cmd;

			order_picture(picture, i, ORDER_SIDE, ORDER_SIDE);
			order_write(src, picture, ORDER_SIDE, ORDER_SIDE, 4);
			cmd = [queue commandBuffer];
			order_blit(cmd, src, dst[0], ORDER_SIDE, ORDER_SIDE);
			[cmd commit];
			[cmd waitUntilCompleted];
			memset(back, 0, sizeof(back));
			order_read(dst[0], back, ORDER_SIDE, ORDER_SIDE, 4);
			wrong += order_wrong(back, i, ORDER_SIDE, ORDER_SIDE, first, sizeof(first)) != 0;
		}
	}
	order_case("blit-wait", wrong == 0, "%u of %u rounds wrong %s", wrong, (unsigned)ORDER_ROUNDS, first);

	/* blit-nowait */
	wrong = 0;
	first[0] = 0;
	for (unsigned i = 0; i < ORDER_ROUNDS; i++) {
		@autoreleasepool {
			id<MTLCommandBuffer> cmd;

			order_picture(picture, 100 + i, ORDER_SIDE, ORDER_SIDE);
			order_write(src, picture, ORDER_SIDE, ORDER_SIDE, 4);
			cmd = [queue commandBuffer];
			order_blit(cmd, src, dst[i], ORDER_SIDE, ORDER_SIDE);
			[cmd commit];
			if (i == ORDER_ROUNDS - 1) {
				[cmd waitUntilCompleted];
			}
		}
	}
	for (unsigned i = 0; i < ORDER_ROUNDS; i++) {
		memset(back, 0, sizeof(back));
		order_read(dst[i], back, ORDER_SIDE, ORDER_SIDE, 4);
		wrong += order_wrong(back, 100 + i, ORDER_SIDE, ORDER_SIDE, first, sizeof(first)) != 0;
	}
	order_case("blit-nowait", wrong == 0, "%u of %u rounds wrong %s", wrong, (unsigned)ORDER_ROUNDS,
		   first);

	/* encoded-blit: the encoder ended, the command buffer not committed, then the write */
	first[0] = 0;
	order_picture(picture, 200, ORDER_SIDE, ORDER_SIDE);
	order_write(src, picture, ORDER_SIDE, ORDER_SIDE, 4);
	@autoreleasepool {
		id<MTLCommandBuffer> cmd = [queue commandBuffer];

		order_blit(cmd, src, dst[1], ORDER_SIDE, ORDER_SIDE);
		order_picture(picture, 201, ORDER_SIDE, ORDER_SIDE);
		order_write(src, picture, ORDER_SIDE, ORDER_SIDE, 4);
		[cmd commit];
		[cmd waitUntilCompleted];
	}
	memset(back, 0, sizeof(back));
	order_read(dst[1], back, ORDER_SIDE, ORDER_SIDE, 4);
	wrong = order_wrong(back, 201, ORDER_SIDE, ORDER_SIDE, first, sizeof(first));
	order_case("encoded-blit", wrong == 0, "%u texels are not the write made after the encoding %s",
		   wrong, first);

	/* the draws */
	NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:metallib]];
	NSError *err = nil;
	id<MTLLibrary> library = nil;
	id<MTLFunction> fn = nil;
	id<MTLRenderPipelineState> ps = nil;
	MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
	id<MTLSamplerState> samp = [dev newSamplerStateWithDescriptor:sd];
	id<MTLTexture> sampled = order_texture(dev, MTLPixelFormatRGBA32Uint, 4, 4);
	id<MTLTexture> targets[ORDER_ROUNDS][4];

	[sd release];
	if (data != nil) {
		dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL,
							  DISPATCH_DATA_DESTRUCTOR_DEFAULT);

		library = [dev newLibraryWithData:dd error:&err];
	}
	fn = library != nil ? [library newFunctionWithName:@"texturedFragment2DUInt"] : nil;
	if (fn != nil) {
		MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];

		[pd setFragmentFunction:fn];
		[pd setVertexFunction:nil];
		for (unsigned i = 0; i < 4; i++) {
			[[[pd colorAttachments] objectAtIndexedSubscript:i]
			    setPixelFormat:MTLPixelFormatRGBA32Uint];
		}
		ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
		[pd release];
	}
	for (unsigned i = 0; i < ORDER_ROUNDS; i++) {
		for (unsigned a = 0; a < 4; a++) {
			targets[i][a] = order_texture(dev, MTLPixelFormatRGBA32Uint, 4, 4);
		}
	}
	if (ps == nil || sampled == nil || samp == nil) {
		const char *no = library == nil ? "the library could not be read" :
		    fn == nil ? "it has no texturedFragment2DUInt" :
		    ps == nil ? [[err localizedDescription] UTF8String] : "no texture or sampler";

		order_case("draw-wait", false, "not tried: %s", no);
		order_case("draw-nowait", false, "not tried: %s", no);
		order_case("encoded-draw", false, "not tried: %s", no);
	} else {
		unsigned words[16][4];

		/* draw-wait */
		wrong = 0;
		first[0] = 0;
		for (unsigned i = 0; i < ORDER_ROUNDS; i++) {
			@autoreleasepool {
				id<MTLCommandBuffer> cmd;

				for (unsigned t = 0; t < 16; t++) {
					order_words(i, words[t]);
				}
				order_write(sampled, words, 4, 4, 16);
				cmd = [queue commandBuffer];
				order_draw(cmd, ps, samp, sampled, targets[0]);
				[cmd commit];
				[cmd waitUntilCompleted];
				wrong += order_words_wrong(targets[0][0], i, first, sizeof(first)) != 0;
			}
		}
		order_case("draw-wait", wrong == 0, "%u of %u rounds wrong %s", wrong,
			   (unsigned)ORDER_ROUNDS, first);

		/* draw-nowait */
		wrong = 0;
		first[0] = 0;
		for (unsigned i = 0; i < ORDER_ROUNDS; i++) {
			@autoreleasepool {
				id<MTLCommandBuffer> cmd;

				for (unsigned t = 0; t < 16; t++) {
					order_words(100 + i, words[t]);
				}
				order_write(sampled, words, 4, 4, 16);
				cmd = [queue commandBuffer];
				order_draw(cmd, ps, samp, sampled, targets[i]);
				[cmd commit];
				if (i == ORDER_ROUNDS - 1) {
					[cmd waitUntilCompleted];
				}
			}
		}
		for (unsigned i = 0; i < ORDER_ROUNDS; i++) {
			wrong += order_words_wrong(targets[i][0], 100 + i, first, sizeof(first)) != 0;
		}
		order_case("draw-nowait", wrong == 0, "%u of %u rounds wrong %s", wrong,
			   (unsigned)ORDER_ROUNDS, first);

		/* encoded-draw */
		first[0] = 0;
		for (unsigned t = 0; t < 16; t++) {
			order_words(200, words[t]);
		}
		order_write(sampled, words, 4, 4, 16);
		@autoreleasepool {
			id<MTLCommandBuffer> cmd = [queue commandBuffer];

			order_draw(cmd, ps, samp, sampled, targets[1]);
			for (unsigned t = 0; t < 16; t++) {
				order_words(201, words[t]);
			}
			order_write(sampled, words, 4, 4, 16);
			[cmd commit];
			[cmd waitUntilCompleted];
		}
		wrong = order_words_wrong(targets[1][0], 201, first, sizeof(first));
		order_case("encoded-draw", wrong == 0, "%u texels are not the write made after the "
			   "encoding %s", wrong, first);
	}

	/* rows, and the cost of them */
	order_rows_and_blocks(dev, queue);

	/* big: over the ring's largest piece */
	{
		const size_t bytes = (size_t)PANEL_W * PANEL_H * 4;
		uint8_t *host = malloc(bytes), *host2 = malloc(bytes);
		id<MTLTexture> panel = order_texture(dev, MTLPixelFormatBGRA8Unorm, PANEL_W, PANEL_H);

		if (host == NULL || host2 == NULL || panel == nil) {
			order_case("big", false, "no memory or no panel-sized texture");
		} else {
			first[0] = 0;
			order_picture(host, 300, PANEL_W, PANEL_H);
			order_write(panel, host, PANEL_W, PANEL_H, 4);
			memset(host2, 0, bytes);
			order_read(panel, host2, PANEL_W, PANEL_H, 4);
			wrong = order_wrong(host2, 300, PANEL_W, PANEL_H, first, sizeof(first));
			order_case("big", wrong == 0, "%u of %u texels wrong %s", wrong, PANEL_W * PANEL_H, first);
		}
		free(host);
		free(host2);
		[panel release];
	}

	/* ring: four times over, nothing committed */
	{
		const size_t bytes = (size_t)ORDER_RING_SIDE * ORDER_RING_SIDE * 4;
		uint8_t *host = malloc(bytes);
		id<MTLTexture> four[4];
		unsigned made = 0;

		for (unsigned i = 0; i < 4; i++) {
			four[i] = order_texture(dev, MTLPixelFormatBGRA8Unorm, ORDER_RING_SIDE, ORDER_RING_SIDE);
			made += four[i] != nil;
		}
		if (host == NULL || made != 4) {
			order_case("ring", false, "no memory or no 256x256 textures");
		} else {
			uint64_t t0 = tm_now_ns();

			for (unsigned i = 0; i < 64; i++) {
				order_picture(host, 400 + i, ORDER_RING_SIDE, ORDER_RING_SIDE);
				order_write(four[i % 4], host, ORDER_RING_SIDE, ORDER_RING_SIDE, 4);
			}
			uint64_t t1 = tm_now_ns();

			wrong = 0;
			first[0] = 0;
			for (unsigned i = 0; i < 4; i++) {
				memset(host, 0, bytes);
				order_read(four[i], host, ORDER_RING_SIDE, ORDER_RING_SIDE, 4);
				wrong += order_wrong(host, 400 + 60 + i, ORDER_RING_SIDE, ORDER_RING_SIDE,
						     first, sizeof(first));
			}
			order_case("ring", wrong == 0, "64 writes of 256 KB in %.1f ms, none committed: "
				   "%u texels of the last four wrong %s", (t1 - t0) / 1e6, wrong, first);
		}
		for (unsigned i = 0; i < 4; i++) {
			[four[i] release];
		}
		free(host);
	}

	/* release */
	{
		id<MTLTexture> keep = order_texture(dev, MTLPixelFormatBGRA8Unorm, ORDER_SIDE, ORDER_SIDE);

		for (unsigned i = 0; i < 64; i++) {
			id<MTLTexture> t = order_texture(dev, MTLPixelFormatBGRA8Unorm, ORDER_SIDE, ORDER_SIDE);

			order_picture(picture, 500 + i, ORDER_SIDE, ORDER_SIDE);
			order_write(t, picture, ORDER_SIDE, ORDER_SIDE, 4);
			[t release];
		}
		for (unsigned i = 0; i < 64; i++) {
			@autoreleasepool {
				id<MTLTexture> t = order_texture(dev, MTLPixelFormatBGRA8Unorm, ORDER_SIDE,
								 ORDER_SIDE);
				id<MTLCommandBuffer> cmd;

				order_picture(picture, 600 + i, ORDER_SIDE, ORDER_SIDE);
				order_write(t, picture, ORDER_SIDE, ORDER_SIDE, 4);
				cmd = [queue commandBuffer];
				order_blit(cmd, t, keep, ORDER_SIDE, ORDER_SIDE);
				[cmd commit];
				if (i == 63) {
					[cmd waitUntilCompleted];
				}
				[t release];
			}
		}
		first[0] = 0;
		memset(back, 0, sizeof(back));
		order_read(keep, back, ORDER_SIDE, ORDER_SIDE, 4);
		wrong = order_wrong(back, 600 + 63, ORDER_SIDE, ORDER_SIDE, first, sizeof(first));
		print_vk_counts("after the releases");
		order_case("release", wrong == 0, "the last of 64 copies: %u texels wrong %s", wrong, first);
		[keep release];
	}

	/* contract */
	order_contract(dev);

	/* surface: its reader cannot be told to wait for a flush */
	{
		enum { W = 256, H = 64 };
		static uint8_t host[W * H * 4];
		IOSurfaceRef surf = frames_surface(W, H);
		MTLTextureDescriptor *td =
		    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
								       width:W
								      height:H
								   mipmapped:NO];

		[td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];
		id<MTLTexture> t = surf != NULL ? [dev newTextureWithDescriptor:td iosurface:surf plane:0] : nil;

		if (t == nil) {
			order_case("surface", false, "no %ux%u BGRA texture over an IOSurface (the driver's "
				   "line above says why)", W, H);
		} else {
			size_t stride = IOSurfaceGetBytesPerRow(surf);
			unsigned bad = 0;

			first[0] = 0;
			order_picture(host, 700, W, H);
			order_write(t, host, W, H, 4);
			IOSurfaceLock(surf, 1 /* kIOSurfaceLockReadOnly */, NULL);

			const uint8_t *base = IOSurfaceGetBaseAddress(surf);

			order_civac(base, stride * H);
			for (unsigned y = 0; y < H; y++) {
				bad += memcmp(base + y * stride, host + y * W * 4, W * 4) != 0;
			}
			IOSurfaceUnlock(surf, 1, NULL);
			order_case("surface", bad == 0, "read by the processor at once past its cache "
				   "(DC CIVAC): %u of %u rows wrong, %zu bytes a row", bad, H, stride);
		}
		[t release];
		if (surf != NULL) {
			CFRelease(surf);
		}
	}

	/* threads */
	if (backend_is_vk()) {
		struct order_thread work[3] = {
			{ dev, 1000, 512, true, 0, 0 },
			{ dev, 3000, 512, true, 0, 0 },
			{ dev, 5000, 512, false, 0, 0 },
		};
		pthread_t thread[2];
		bool started[2];
		uint64_t t0 = tm_now_ns();

		for (unsigned i = 0; i < 2; i++) {
			started[i] = pthread_create(&thread[i], NULL, order_thread_main, &work[i + 1]) == 0;
		}
		order_thread_main(&work[0]);
		for (unsigned i = 0; i < 2; i++) {
			if (started[i]) {
				pthread_join(thread[i], NULL);
			}
		}
		order_case("threads", work[0].wrong + work[1].wrong + work[2].wrong +
			   work[0].failed + work[1].failed + work[2].failed == 0 && started[0] && started[1],
			   "two threads committing and one only writing and reading, 512 rounds each in "
			   "%.0f ms: wrong %u %u %u, failed %u %u %u", (tm_now_ns() - t0) / 1e6,
			   work[0].wrong, work[1].wrong, work[2].wrong, work[0].failed, work[1].failed,
			   work[2].failed);
	} else {
		printf("drvtest: order gallium threads not tried: -replaceRegion: and -getBytes: share "
		       "Gallium's context unguarded (hkmetal.m, over rw_pipe_lock)\n");
	}

	for (unsigned i = 0; i < ORDER_ROUNDS; i++) {
		[dst[i] release];
		for (unsigned a = 0; a < 4; a++) {
			[targets[i][a] release];
		}
	}
	[src release];
	[sampled release];
	[samp release];
	[ps release];
	[fn release];
	[library release];
	[queue release];
	print_vk_counts("after everything of --order was released");
	printf("drvtest: order-result %s %s %u cases, %u failed:%s\n", backend,
	       order_bad == 0 ? "ok" : "FAILED", order_cases, order_bad,
	       order_bad == 0 ? " none" : order_bad_names);
	fflush(stdout);
	return order_bad != 0;
}

/* Check completion without an explicit wait, plus handlers registered around commit. */
static int
completion_test(id<MTLDevice> dev)
{
	id<MTLCommandQueue> queue = [dev newCommandQueue];
	if (queue == nil) {
		return 1;
	}
	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
										    width:1080 height:2400 mipmapped:NO];
	desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	id<MTLTexture> texture = [dev newTextureWithDescriptor:desc];
	if (texture == nil) {
		[queue release];
		return 1;
	}
	int failures = 0;
	for (unsigned mode = 0; mode < 3; mode++) {
		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = texture;
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		pass.colorAttachments[0].clearColor = MTLClearColorMake(0.5, 0.25, 0.75, 1);
		id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
		if (cmd == nil || enc == nil) {
			failures++;
			break;
		}
		[enc endEncoding];
		dispatch_semaphore_t done = dispatch_semaphore_create(0);
		__block NSUInteger observed = 0;
		void (^handler)(id<MTLCommandBuffer>) = ^(id<MTLCommandBuffer> completed) {
			observed = completed.status;
			dispatch_semaphore_signal(done);
		};
		if (mode == 1) {
			[cmd addCompletedHandler:handler];
		}
		[cmd commit];
		if (mode == 2) {
			[cmd addCompletedHandler:handler];
		}
		bool timed_out = false;
		if (mode == 0) {
			for (unsigned i = 0; i < 5000 && cmd.status < MTLCommandBufferStatusCompleted; i++) {
				usleep(1000);
			}
			observed = cmd.status;
		} else {
			timed_out = dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0;
		}
		bool ok = !timed_out && observed == MTLCommandBufferStatusCompleted;
		if (ok) {
			uint8_t pixel[4] = { 0 };
			[texture getBytes:pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
			ok = abs((int)pixel[0] - 191) <= 1 && abs((int)pixel[1] - 64) <= 1 &&
			    abs((int)pixel[2] - 128) <= 1 && pixel[3] == 255;
		}
		printf("drvtest: completion: %s: status %lu timeout %u: %s\n",
		    mode == 0 ? "no handler" : mode == 1 ? "before commit" : "after commit",
		    (unsigned long)observed, timed_out, ok ? "PASS" : "FAIL");
		failures += !ok;
		/* A timed-out callback may still arrive; keep its semaphore until process exit. */
		if (!timed_out) {
			dispatch_release(done);
		}
	}
	[texture release];
	[queue release];
	return failures != 0;
}

/*
 * What this process left on the GPU, on every way out through exit() (M4-1), so a log shows
 * whether one process's work could still be running when the next one starts. air_vk.c counts
 * it, from each fence's own status, at its own exit handler (exit_line says what the two
 * numbers are), and hands them over in AIR_VK_EXIT because the library has no export to spare;
 * this handler is registered first thing in main, before any device exists, so it runs after
 * that one. Gallium's fences are freedreno's and are not counted by anything here, which the
 * line says rather than printing zeros.
 */
/*
 * --pixels: M5a's half of --draw-attrib, --draw-index, --picture and --blit, through Metal's own
 * API and so through userland/src/hkmetal.m: the encoder's setters that were stubs until now
 * (-setCullMode:, -setFrontFacingWinding:, -setViewport:'s rectangle, -setScissorRect:,
 * -setBlendColorRed:green:blue:alpha:), one colour's clear per attachment, several encoders in a
 * command buffer. What those modes check without it, and it is why this exists: --draw-attrib's
 * attachments are 4x4 and its verdict is a count of a gradient's texels; --draw-index draws a
 * constant colour and looks at two corners; --picture is one texture's bands on the panel, opaque,
 * and a count; --blit's render passes are uniform colours. air_metal_test --pixels has the same
 * pictures through the contract, with modules written for it. This one has only what a Metal
 * library can hold on this phone, which means Apple's own functions, and they serve: a texture
 * of draw_model.h's pattern, sampled nearest at a coordinate that lands every pixel on its own
 * texel, is a fragment stage whose output varies per pixel by a known function.
 *
 *   texturedVertex2D         position out of [[buffer(0)]], coordinate out of [[buffer(1)]], by id
 *   std_vert0_lpf            position from [[attribute(0)]], coordinate from [[attribute(3)]],
 *                            through mvp_matrix at [[buffer(2)]]: the identity, here
 *   texturedFragment2DFloat  the texel at the coordinate, into four attachments
 *
 * Every case is one drvtest: draw-result line (tools/mesa/air/draw_model.h), compared with the
 * model on both back ends: the Gallium library reads none of the encoder's M5a fields, so those
 * cases say gallium-lacks there, and its refusal of the blend colour says "refused".
 */
struct mpx {
	id<MTLDevice> dev;
	id<MTLCommandQueue> queue;
	id<MTLFunction> vtx2d, frag2d, stdv;
	id<MTLSamplerState> samp;
	id<MTLTexture> tsrc, tdst, tcheck;
	id<MTLBuffer> identity;
	const char *backend, *mode;
	bool gallium;
	unsigned right, wrong, lacks, refused;
	unsigned mirror;   /* M5b: rows that are the Gallium library's mirrored 16-bit table */
};

static uint8_t mpx_mem[DM_W * DM_H * 16];

/* One attachment or sampled texture. */
static id<MTLTexture>
mpx_texture(struct mpx *m, unsigned fmt, void (*pattern)(unsigned, unsigned, float[4]))
{
	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)fmt
							       width:DM_W
							      height:DM_H
							   mipmapped:NO];

	[td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];
	id<MTLTexture> t = [m->dev newTextureWithDescriptor:td];

	if (t == nil) {
		printf("drvtest: pixels: a %s texture could not be made\n", dm_format_name(fmt));
		return nil;
	}
	if (pattern != NULL) {
		unsigned texel = dm_texel_bytes(fmt);

		for (unsigned y = 0; y < DM_H; y++) {
			for (unsigned x = 0; x < DM_W; x++) {
				float v[4];

				pattern(x, y, v);
				dm_encode(fmt, v, mpx_mem + ((size_t)y * DM_W + x) * texel);
			}
		}
		[t replaceRegion:MTLRegionMake2D(0, 0, DM_W, DM_H)
		     mipmapLevel:0
		       withBytes:mpx_mem
		     bytesPerRow:DM_W * texel];
	}
	return t;
}

/* The pipeline, or the case's line saying why not. `formats` are the four attachments. */
static id<MTLRenderPipelineState>
mpx_pipeline(struct mpx *m, id<MTLFunction> vertex, MTLVertexDescriptor *vd,
	     const unsigned formats[4], const struct dm_blend *blends, unsigned zformat,
	     const char *name)
{
	MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
	NSError *err = nil;

	[pd setVertexFunction:vertex];
	[pd setFragmentFunction:m->frag2d];
	if (vd != nil) {
		[pd setVertexDescriptor:vd];
	}
	for (unsigned i = 0; i < 4; i++) {
		MTLRenderPipelineColorAttachmentDescriptor *a =
		    [[pd colorAttachments] objectAtIndexedSubscript:i];

		[a setPixelFormat:(MTLPixelFormat)formats[i]];
		if (blends != NULL && i < 2) {
			const struct dm_blend *b = &blends[i];

			[a setBlendingEnabled:b->on != 0];
			[a setRgbBlendOperation:(MTLBlendOperation)b->rgb_op];
			[a setAlphaBlendOperation:(MTLBlendOperation)b->alpha_op];
			[a setSourceRGBBlendFactor:(MTLBlendFactor)b->src_rgb];
			[a setDestinationRGBBlendFactor:(MTLBlendFactor)b->dst_rgb];
			[a setSourceAlphaBlendFactor:(MTLBlendFactor)b->src_alpha];
			[a setDestinationAlphaBlendFactor:(MTLBlendFactor)b->dst_alpha];
			[a setWriteMask:(MTLColorWriteMask)b->mask];
		}
	}
	if (zformat != 0) {
		[pd setDepthAttachmentPixelFormat:(MTLPixelFormat)zformat];
		[pd setStencilAttachmentPixelFormat:(MTLPixelFormat)zformat];
	}
	id<MTLRenderPipelineState> ps = [m->dev newRenderPipelineStateWithDescriptor:pd error:&err];

	[pd release];
	if (ps == nil) {
		dm_refused(m->backend, name,
			   err != nil ? [[err localizedDescription] UTF8String] : "(no reason)");
		m->refused++;
	}
	return ps;
}

/* Attachment 0 of the four is what is judged; 1..3 are there because the shader writes them. */
static MTLRenderPassDescriptor *
mpx_pass(id<MTLTexture> const t[4], MTLLoadAction load, MTLClearColor clear)
{
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];

	for (unsigned i = 0; i < 4; i++) {
		MTLRenderPassColorAttachmentDescriptor *a =
		    [[rp colorAttachments] objectAtIndexedSubscript:i];

		[a setTexture:t[i]];
		[a setLoadAction:load];
		[a setClearColor:clear];
		[a setStoreAction:MTLStoreActionStore];
	}
	return rp;
}

/* The attachment read back and judged: one line, and the counts. */
static bool
mpx_judge(struct mpx *m, id<MTLTexture> t, unsigned fmt, const char *name,
	  const struct dm_picture *want, float extra, bool lacks)
{
	[t getBytes:mpx_mem
	bytesPerRow:DM_W * dm_texel_bytes(fmt)
	 fromRegion:MTLRegionMake2D(0, 0, DM_W, DM_H)
	mipmapLevel:0];
	bool ok = dm_verdict(m->backend, name, fmt, mpx_mem, want, extra, lacks);

	if (ok) {
		m->right++;
	} else if (lacks) {
		m->lacks++;
	} else {
		m->wrong++;
	}
	return ok;
}

/* -commit and the wait; a command buffer that failed is the case's line. */
static bool
mpx_finish(struct mpx *m, id<MTLCommandBuffer> cb, const char *name)
{
	[cb commit];
	[cb waitUntilCompleted];
	if ([cb status] != RW_CB_COMPLETED) {
		dm_refused(m->backend, name, [cb error] != nil
			   ? [[[cb error] localizedDescription] UTF8String] : "(no reason)");
		m->refused++;
		return false;
	}
	return true;
}

/*
 * What a textured triangle leaves: dm_triangle over the coordinate, and each covered pixel
 * the texel its coordinate lands in, which is this harness's whole fragment stage.
 */
static void
mpx_model(struct dm_picture *want, const float win[3][2], const float uv[3][2],
	  const struct dm_viewport *vp, struct dm_rect rect, unsigned cull, unsigned winding,
	  void (*pattern)(unsigned, unsigned, float[4]), bool shared)
{
	static struct dm_picture cover;
	float col[3][4];
	float w[3][2];
	static const float none[4] = { 0, 0, 0, 0 };

	for (unsigned i = 0; i < 3; i++) {
		float clip[2] = { win[i][0] * 2.0f / DM_W - 1.0f, 1.0f - win[i][1] * 2.0f / DM_H };

		dm_window(clip, vp, w[i]);
		col[i][0] = uv[i][0];
		col[i][1] = uv[i][1];
		col[i][2] = 0.0f;
		col[i][3] = 1.0f;
	}
	if (dm_culled(w, cull, winding)) {
		return;
	}
	dm_clear(&cover, none);
	dm_triangle(&cover, w, col, rect, shared);
	for (unsigned y = 0; y < DM_H; y++) {
		for (unsigned x = 0; x < DM_W; x++) {
			if (!cover.care[y][x]) {
				want->care[y][x] = 0;
			} else if (cover.px[y][x][3] > 0.5f) {
				int tx = (int)(cover.px[y][x][0] * DM_W);
				int ty = (int)(cover.px[y][x][1] * DM_H);

				tx = tx < 0 ? 0 : tx >= DM_W ? DM_W - 1 : tx;
				ty = ty < 0 ? 0 : ty >= DM_H ? DM_H - 1 : ty;
				pattern((unsigned)tx, (unsigned)ty, want->px[y][x]);
			}
		}
	}
}

/* The element std_vert0_lpf's descriptor below reads, at Metal's buffer index 5. */
struct mpx_std_vertex {
	float pos[4];
	float uv[2];
	float pad[2];
};

static struct mpx_std_vertex
mpx_stdv(float wx, float wy, float u, float v)
{
	struct mpx_std_vertex e = {
		{ wx * 2.0f / DM_W - 1.0f, 1.0f - wy * 2.0f / DM_H, 0.0f, 1.0f }, { u, v }, { 0, 0 },
	};
	return e;
}

static MTLVertexDescriptor *
mpx_std_descriptor(void)
{
	MTLVertexDescriptor *vd = [[[MTLVertexDescriptor alloc] init] autorelease];
	MTLVertexAttributeDescriptor *a0 = [[vd attributes] objectAtIndexedSubscript:0];
	MTLVertexAttributeDescriptor *a3 = [[vd attributes] objectAtIndexedSubscript:3];
	MTLVertexBufferLayoutDescriptor *l = [[vd layouts] objectAtIndexedSubscript:5];

	[a0 setFormat:MTLVertexFormatFloat4];
	[a0 setOffset:0];
	[a0 setBufferIndex:5];
	[a3 setFormat:MTLVertexFormatFloat2];
	[a3 setOffset:16];
	[a3 setBufferIndex:5];
	[l setStride:sizeof(struct mpx_std_vertex)];
	[l setStepFunction:MTLVertexStepFunctionPerVertex];
	[l setStepRate:1];
	return vd;
}

/* What an encoder over std_vert0_lpf binds, whatever it draws. */
static void
mpx_bind_std(struct mpx *m, id<MTLRenderCommandEncoder> enc, id<MTLRenderPipelineState> ps,
	     id<MTLTexture> tex, const struct mpx_std_vertex *v, unsigned n)
{
	[enc setRenderPipelineState:ps];
	for (unsigned i = 2; i <= 4; i++) {
		[enc setVertexBuffer:m->identity offset:0 atIndex:i];
	}
	[enc setVertexBytes:v length:n * sizeof(*v) atIndex:5];
	[enc setFragmentTexture:tex atIndex:0];
	[enc setFragmentSamplerState:m->samp atIndex:0];
}

/* The asymmetric triangle, in window coordinates and with its coordinate in texels 1:1. */
static const float mpx_tri[3][2] = { { 2.0625f, 1.0625f }, { 4.0625f, 12.0625f },
				     { 13.0625f, 3.0625f } };

static void
mpx_tri_uv(float uv[3][2])
{
	for (unsigned i = 0; i < 3; i++) {
		uv[i][0] = mpx_tri[i][0] / DM_W;
		uv[i][1] = mpx_tri[i][1] / DM_H;
	}
}

static const unsigned mpx_all32f[4] = { DM_RGBA32F, DM_RGBA32F, DM_RGBA32F, DM_RGBA32F };

/* ---- --draw-attrib --pixels: orientation, rectangles and stencil, through the encoder ---- */
static void
mpx_attrib(struct mpx *m)
{
	static struct dm_picture want;
	static const float black[4] = { 0, 0, 0, 0 };
	const struct dm_viewport whole = { 0, 0, DM_W, DM_H };
	id<MTLTexture> t[4];
	/* a thousandth of a pixel's worth of coordinate never changes a nearest texel */
	MTLClearColor none = MTLClearColorMake(0, 0, 0, 0);
	MTLVertexDescriptor *vd = mpx_std_descriptor();
	float uv[3][2];

	mpx_tri_uv(uv);
	for (unsigned i = 0; i < 4; i++) {
		t[i] = mpx_texture(m, DM_RGBA32F, NULL);
	}
	id<MTLRenderPipelineState> ps = mpx_pipeline(m, m->stdv, vd, mpx_all32f, NULL, 0,
						     "orient-pipeline");
	if (ps == nil) {
		return;
	}
	MTLRenderPassDescriptor *rp = mpx_pass(t, MTLLoadActionClear, none);
	struct mpx_std_vertex ccw[3], cw[3];

	for (unsigned i = 0; i < 3; i++) {
		ccw[i] = mpx_stdv(mpx_tri[i][0], mpx_tri[i][1], uv[i][0], uv[i][1]);
	}
	cw[0] = ccw[0];
	cw[1] = ccw[2];
	cw[2] = ccw[1];

	/* 1-13: the asymmetric triangle under every cull mode and winding, both ways round */
	static const char *const cn[] = { "none", "front", "back" };
	static const char *const wn[] = { "cw", "ccw" };
	for (unsigned tw = 0; tw < 2; tw++) {
		for (unsigned cull = 0; cull < 3; cull++) {
			for (unsigned fw = 0; fw < 2; fw++) {
				char name[64];
				id<MTLCommandBuffer> cb = [m->queue commandBuffer];
				id<MTLRenderCommandEncoder> enc =
				    [cb renderCommandEncoderWithDescriptor:rp];

				snprintf(name, sizeof(name), "orient-%stri-cull%s-front%s", wn[tw],
					 cn[cull], wn[fw]);
				mpx_bind_std(m, enc, ps, m->tsrc, tw ? ccw : cw, 3);
				[enc setCullMode:(MTLCullMode)cull];
				[enc setFrontFacingWinding:(MTLWinding)fw];
				[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
				[enc endEncoding];
				if (!mpx_finish(m, cb, name)) {
					continue;
				}
				const float (*win)[2] = mpx_tri;
				float swapped[3][2], swuv[3][2];

				if (!tw) {
					memcpy(swapped[0], mpx_tri[0], sizeof(swapped[0]));
					memcpy(swapped[1], mpx_tri[2], sizeof(swapped[1]));
					memcpy(swapped[2], mpx_tri[1], sizeof(swapped[2]));
					memcpy(swuv[0], uv[0], sizeof(swuv[0]));
					memcpy(swuv[1], uv[2], sizeof(swuv[1]));
					memcpy(swuv[2], uv[1], sizeof(swuv[2]));
					win = (const float (*)[2])swapped;
				}
				dm_clear(&want, black);
				mpx_model(&want, win, tw ? uv : swuv, &whole, dm_whole(), cull, fw,
					  dm_src, false);
				mpx_judge(m, t[0], DM_RGBA32F, name, &want, 0.0f,
					  m->gallium && cull != 0);
			}
		}
	}

	/* 14-19: rectangles. A strip over the whole of clip space through a viewport, with a
	 * coordinate that keeps a pixel on a texel of its own whatever the viewport's size: the
	 * texel is the pixel's place inside the viewport, so a viewport put in the wrong place
	 * or at the wrong y is the picture moved as well as cut. */
	static const struct {
		const char *name;
		MTLViewport vp;
		MTLScissorRect sc;
		bool has_vp, has_sc;
	} rects[] = {
		{ "viewport-origin", { 4, 2, 8, 8, 0, 1 }, { 0, 0, 0, 0 }, true, false },
		{ "viewport-lower-right", { 8, 8, 8, 8, 0, 1 }, { 0, 0, 0, 0 }, true, false },
		{ "viewport-wide", { 1, 11, 14, 4, 0, 1 }, { 0, 0, 0, 0 }, true, false },
		{ "scissor", { 0, 0, 0, 0, 0, 0 }, { 3, 5, 7, 4 }, false, true },
		{ "scissor-corner", { 0, 0, 0, 0, 0, 0 }, { 0, 12, 4, 4 }, false, true },
		{ "viewport-and-scissor", { 2, 2, 12, 12, 0, 1 }, { 6, 0, 4, 16 }, true, true },
	};
	for (unsigned c = 0; c < sizeof(rects) / sizeof(rects[0]); c++) {
		float vw = rects[c].has_vp ? (float)rects[c].vp.width : DM_W;
		float vh = rects[c].has_vp ? (float)rects[c].vp.height : DM_H;
		struct dm_viewport vp = { rects[c].has_vp ? (float)rects[c].vp.originX : 0,
					  rects[c].has_vp ? (float)rects[c].vp.originY : 0, vw, vh };
		struct dm_rect sc = rects[c].has_sc
		    ? (struct dm_rect){ (int)rects[c].sc.x, (int)rects[c].sc.y,
					(int)(rects[c].sc.x + rects[c].sc.width),
					(int)(rects[c].sc.y + rects[c].sc.height) }
		    : dm_whole();
		struct mpx_std_vertex q[4];
		static const float at[4][2] = { { 0, 0 }, { DM_W, 0 }, { 0, DM_H }, { DM_W, DM_H } };
		float quv[4][2];

		for (unsigned i = 0; i < 4; i++) {
			quv[i][0] = at[i][0] / DM_W * vw / DM_W;
			quv[i][1] = at[i][1] / DM_H * vh / DM_H;
			q[i] = mpx_stdv(at[i][0], at[i][1], quv[i][0], quv[i][1]);
		}
		id<MTLCommandBuffer> cb = [m->queue commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];

		mpx_bind_std(m, enc, ps, m->tsrc, q, 4);
		if (rects[c].has_vp) {
			[enc setViewport:rects[c].vp];
		}
		if (rects[c].has_sc) {
			[enc setScissorRect:rects[c].sc];
		}
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		[enc endEncoding];
		if (!mpx_finish(m, cb, rects[c].name)) {
			continue;
		}
		const float t0[3][2] = { { at[0][0], at[0][1] }, { at[1][0], at[1][1] }, { at[2][0], at[2][1] } };
		const float t1[3][2] = { { at[2][0], at[2][1] }, { at[1][0], at[1][1] }, { at[3][0], at[3][1] } };
		const float u0[3][2] = { { quv[0][0], quv[0][1] }, { quv[1][0], quv[1][1] }, { quv[2][0], quv[2][1] } };
		const float u1[3][2] = { { quv[2][0], quv[2][1] }, { quv[1][0], quv[1][1] }, { quv[3][0], quv[3][1] } };

		dm_clear(&want, black);
		mpx_model(&want, t0, u0, &vp, sc, 0, 0, dm_src, true);
		mpx_model(&want, t1, u1, &vp, sc, 0, 0, dm_src, true);
		mpx_judge(m, t[0], DM_RGBA32F, rects[c].name, &want, 0.0f, m->gallium);
	}

	/* 20-21: the stencil, through -setStencilReferenceValue: and its two-faced form: the
	 * triangle replaces a reference, the whole attachment then draws where the stencil
	 * equals it. The triangle is counter-clockwise as seen, so under Metal's default winding
	 * it is back-facing and the second case's back reference is the one that goes in. */
	{
		MTLTextureDescriptor *zd =
		    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
								       width:DM_W
								      height:DM_H
								   mipmapped:NO];

		[zd setUsage:MTLTextureUsageRenderTarget];
		[zd setStorageMode:MTLStorageModePrivate];
		id<MTLTexture> z = [m->dev newTextureWithDescriptor:zd];
		id<MTLRenderPipelineState> zps = mpx_pipeline(m, m->stdv, vd, mpx_all32f, NULL,
							      (unsigned)MTLPixelFormatDepth32Float_Stencil8,
							      "stencil-pipeline");
		MTLStencilDescriptor *wf = [[MTLStencilDescriptor alloc] init];
		MTLStencilDescriptor *wb = [[MTLStencilDescriptor alloc] init];
		MTLStencilDescriptor *tf = [[MTLStencilDescriptor alloc] init];

		[wf setDepthStencilPassOperation:MTLStencilOperationReplace];
		[wb setDepthStencilPassOperation:MTLStencilOperationReplace];
		[wb setReadMask:0x7f];     /* so the two faces are two states on Gallium as well */
		[tf setStencilCompareFunction:MTLCompareFunctionEqual];
		[tf setWriteMask:0];
		MTLDepthStencilDescriptor *wd = [[MTLDepthStencilDescriptor alloc] init];
		MTLDepthStencilDescriptor *td = [[MTLDepthStencilDescriptor alloc] init];

		[wd setFrontFaceStencil:wf];
		[wd setBackFaceStencil:wb];
		[td setFrontFaceStencil:tf];
		[td setBackFaceStencil:tf];
		id<MTLDepthStencilState> ws = [m->dev newDepthStencilStateWithDescriptor:wd];
		id<MTLDepthStencilState> ts = [m->dev newDepthStencilStateWithDescriptor:td];

		for (unsigned pass = 0; z != nil && zps != nil && pass < 2; pass++) {
			const char *name = pass ? "stencil-backface-reference" : "stencil-reference";
			MTLRenderPassDescriptor *zp = mpx_pass(t, MTLLoadActionClear, none);
			MTLRenderPassDepthAttachmentDescriptor *da = [zp depthAttachment];
			MTLRenderPassStencilAttachmentDescriptor *sa = [zp stencilAttachment];

			[da setTexture:z];
			[da setLoadAction:MTLLoadActionClear];
			[da setClearDepth:1.0];
			[da setStoreAction:MTLStoreActionStore];
			[sa setTexture:z];
			[sa setLoadAction:MTLLoadActionClear];
			[sa setClearStencil:3];
			[sa setStoreAction:MTLStoreActionStore];
			struct mpx_std_vertex q[4];
			static const float at[4][2] = { { 0, 0 }, { DM_W, 0 }, { 0, DM_H }, { DM_W, DM_H } };

			for (unsigned i = 0; i < 4; i++) {
				q[i] = mpx_stdv(at[i][0], at[i][1], at[i][0] / DM_W, at[i][1] / DM_H);
			}
			id<MTLCommandBuffer> cb = [m->queue commandBuffer];
			id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:zp];

			mpx_bind_std(m, enc, zps, m->tcheck, ccw, 3);
			[enc setDepthStencilState:ws];
			if (pass) {
				[enc setStencilFrontReferenceValue:0x11 backReferenceValue:0x22];
			} else {
				[enc setStencilReferenceValue:0x22];
			}
			[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[enc setDepthStencilState:ts];
			[enc setStencilReferenceValue:0x22];
			[enc setVertexBytes:q length:sizeof(q) atIndex:5];
			[enc setFragmentTexture:m->tsrc atIndex:0];
			[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
			[enc endEncoding];
			if (!mpx_finish(m, cb, name)) {
				continue;
			}
			/* the triangle's pixels hold the check it drew and then the source over
			 * it, the rest nothing: the second draw passes only where the first
			 * wrote. Both the check and the source are per pixel. */
			dm_clear(&want, black);
			mpx_model(&want, mpx_tri, uv, &whole, dm_whole(), 0, 0, dm_src, false);
			mpx_judge(m, t[0], DM_RGBA32F, name, &want, 0.0f, false);
		}
		[wf release];
		[wb release];
		[tf release];
		[wd release];
		[td release];
		[ws release];
		[ts release];
		[zps release];
		[z release];
	}
	[ps release];
	for (unsigned i = 0; i < 4; i++) {
		[t[i] release];
	}
}

/* ---- --draw-index --pixels ---- */
static void
mpx_index(struct mpx *m)
{
	static struct dm_picture want;
	static const float black[4] = { 0, 0, 0, 0 };
	const struct dm_viewport whole = { 0, 0, DM_W, DM_H };
	MTLClearColor none = MTLClearColorMake(0, 0, 0, 0);
	id<MTLTexture> t[4];

	for (unsigned i = 0; i < 4; i++) {
		t[i] = mpx_texture(m, DM_RGBA32F, NULL);
	}
	id<MTLRenderPipelineState> ps = mpx_pipeline(m, m->vtx2d, nil, mpx_all32f, NULL, 0,
						     "index-pipeline");
	if (ps == nil) {
		return;
	}
	MTLRenderPassDescriptor *rp = mpx_pass(t, MTLLoadActionClear, none);

	/* The grid: 25 vertices, a coordinate that lands each pixel on its own texel, and 16
	 * quads of two triangles, walked in a fixed scrambled order (air_metal_test's own). */
	float pos[32][4], uvb[32][2];
	uint32_t tris[96], order[96];
	unsigned n = 0;

	for (unsigned j = 0; j < 5; j++) {
		for (unsigned i = 0; i < 5; i++) {
			float wx = i * 4.0f, wy = j * 4.0f;

			pos[j * 5 + i][0] = wx * 2.0f / DM_W - 1.0f;
			pos[j * 5 + i][1] = 1.0f - wy * 2.0f / DM_H;
			pos[j * 5 + i][2] = 0.0f;
			pos[j * 5 + i][3] = 1.0f;
			uvb[j * 5 + i][0] = wx / DM_W;
			uvb[j * 5 + i][1] = wy / DM_H;
		}
	}
	for (unsigned j = 0; j < 4; j++) {
		for (unsigned i = 0; i < 4; i++) {
			uint32_t a = j * 5 + i, b = a + 1, c = a + 5, d = c + 1;

			tris[n++] = a; tris[n++] = c; tris[n++] = b;
			tris[n++] = b; tris[n++] = c; tris[n++] = d;
		}
	}
	for (unsigned k = 0; k < 32; k++) {
		unsigned from = (k * 13 + 7) % 32;

		for (unsigned c = 0; c < 3; c++) {
			order[k * 3 + c] = tris[from * 3 + c];
		}
	}
	dm_clear(&want, black);
	for (unsigned y = 0; y < DM_H; y++) {
		for (unsigned x = 0; x < DM_W; x++) {
			dm_src(x, y, want.px[y][x]);
		}
	}

	/* The same picture four ways: flat (the comparison), 16 bits, 32 bits, and a base vertex
	 * with a mesh moved in its buffers behind decoys. Every one is judged against the same
	 * model, and a line each. */
	static const struct {
		const char *name;
		bool indexed, wide;
		unsigned offset;
		int base;
		int shift;
	} cases[] = {
		{ "index-flat", false, false, 0, 0, 0 },
		{ "index-16", true, false, 8, 0, 0 },
		{ "index-32", true, true, 16, 0, 0 },
		{ "index-basevertex", true, false, 0, 7, 7 },
		{ "index-basevertex-neg", true, false, 0, -7, 0 },
	};
	for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		float mpos[64][4], muv[64][2];
		uint16_t i16[110];
		uint32_t i32[110];

		for (unsigned i = 0; i < 64; i++) {
			mpos[i][0] = mpos[i][1] = 0.0f;
			mpos[i][2] = 0.0f;
			mpos[i][3] = 1.0f;
			muv[i][0] = muv[i][1] = 0.0f;
		}
		unsigned count = 96;
		id<MTLCommandBuffer> cb = [m->queue commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];

		[enc setRenderPipelineState:ps];
		[enc setFragmentTexture:m->tsrc atIndex:0];
		[enc setFragmentSamplerState:m->samp atIndex:0];
		if (!cases[c].indexed) {
			float fpos[96][4], fuv[96][2];

			for (unsigned i = 0; i < 96; i++) {
				memcpy(fpos[i], pos[order[i]], sizeof(fpos[i]));
				memcpy(fuv[i], uvb[order[i]], sizeof(fuv[i]));
			}
			[enc setVertexBytes:fpos length:sizeof(fpos) atIndex:0];
			[enc setVertexBytes:fuv length:sizeof(fuv) atIndex:1];
			[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:96];
		} else {
			int add = cases[c].base < 0 ? -cases[c].base : 0;

			memcpy(mpos + cases[c].shift, pos, 25 * sizeof(pos[0]));
			memcpy(muv + cases[c].shift, uvb, 25 * sizeof(uvb[0]));
			[enc setVertexBytes:mpos length:sizeof(mpos) atIndex:0];
			[enc setVertexBytes:muv length:sizeof(muv) atIndex:1];
			for (unsigned i = 0; i < 110; i++) {
				i16[i] = 0x5a;
				i32[i] = 0x5a;
			}
			for (unsigned i = 0; i < 96; i++) {
				i16[cases[c].offset / 2 + i] = (uint16_t)(order[i] + (unsigned)add);
				i32[cases[c].offset / 4 + i] = order[i] + (unsigned)add;
			}
			id<MTLBuffer> ib = [m->dev newBufferWithBytes:cases[c].wide ? (void *)i32 : (void *)i16
							       length:cases[c].wide ? sizeof(i32) : sizeof(i16)
							      options:0];

			[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
					indexCount:count
					 indexType:cases[c].wide ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
				       indexBuffer:ib
				 indexBufferOffset:cases[c].offset
				     instanceCount:1
					baseVertex:cases[c].base
				      baseInstance:0];
			[ib release];
		}
		[enc endEncoding];
		if (mpx_finish(m, cb, cases[c].name)) {
			mpx_judge(m, t[0], DM_RGBA32F, cases[c].name, &want, 0.0f, false);
		}
	}
	[ps release];

	/*
	 * A restart index, over the attribute path: on Gallium the bound check sees vertex 65535
	 * and refuses the draw by name, where a stage that indexes its own buffer would read past
	 * it on the GPU. Two quads of one strip, 16 and 32 bits.
	 */
	MTLVertexDescriptor *vd = mpx_std_descriptor();
	id<MTLRenderPipelineState> sp = mpx_pipeline(m, m->stdv, vd, mpx_all32f, NULL, 0,
						     "restart-pipeline");

	if (sp != nil) {
		static const float corner[8][2] = {
			{ 1, 1 }, { 6, 1 }, { 1, 6 }, { 6, 6 }, { 9, 9 }, { 15, 9 }, { 9, 14 }, { 15, 14 },
		};
		struct mpx_std_vertex v[8];

		for (unsigned i = 0; i < 8; i++) {
			v[i] = mpx_stdv(corner[i][0], corner[i][1], corner[i][0] / DM_W, corner[i][1] / DM_H);
		}
		const uint16_t r16[9] = { 0, 1, 2, 3, 0xffff, 4, 5, 6, 7 };
		const uint32_t r32[9] = { 0, 1, 2, 3, 0xffffffffu, 4, 5, 6, 7 };

		dm_clear(&want, black);
		for (unsigned q = 0; q < 2; q++) {
			const float w0[3][2] = { { corner[q * 4][0], corner[q * 4][1] },
						 { corner[q * 4 + 1][0], corner[q * 4 + 1][1] },
						 { corner[q * 4 + 2][0], corner[q * 4 + 2][1] } };
			const float w1[3][2] = { { corner[q * 4 + 2][0], corner[q * 4 + 2][1] },
						 { corner[q * 4 + 1][0], corner[q * 4 + 1][1] },
						 { corner[q * 4 + 3][0], corner[q * 4 + 3][1] } };
			float u0[3][2], u1[3][2];

			for (unsigned i = 0; i < 3; i++) {
				u0[i][0] = w0[i][0] / DM_W;
				u0[i][1] = w0[i][1] / DM_H;
				u1[i][0] = w1[i][0] / DM_W;
				u1[i][1] = w1[i][1] / DM_H;
			}
			mpx_model(&want, w0, u0, &whole, dm_whole(), 0, 0, dm_src, true);
			mpx_model(&want, w1, u1, &whole, dm_whole(), 0, 0, dm_src, true);
		}
		for (unsigned w = 0; w < 2; w++) {
			const char *name = w ? "index-restart-32" : "index-restart-16";
			id<MTLBuffer> ib = [m->dev newBufferWithBytes:w ? (const void *)r32 : (const void *)r16
							       length:w ? sizeof(r32) : sizeof(r16)
							      options:0];
			id<MTLCommandBuffer> cb = [m->queue commandBuffer];
			id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];

			mpx_bind_std(m, enc, sp, m->tsrc, v, 8);
			[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
					indexCount:9
					 indexType:w ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
				       indexBuffer:ib
				 indexBufferOffset:0
				     instanceCount:1
					baseVertex:0
				      baseInstance:0];
			[enc endEncoding];
			[ib release];
			if (mpx_finish(m, cb, name)) {
				mpx_judge(m, t[0], DM_RGBA32F, name, &want, 0.0f, false);
			}
		}
		[sp release];
	}
	for (unsigned i = 0; i < 4; i++) {
		[t[i] release];
	}
}

/* A full-attachment strip of texturedVertex2D, each pixel on its own texel. */
static void
mpx_bind_quad(struct mpx *m, id<MTLRenderCommandEncoder> enc, id<MTLRenderPipelineState> ps,
	      id<MTLTexture> tex)
{
	static const float qpos[4][4] = {
		{ -1, -1, 0, 1 }, { 1, -1, 0, 1 }, { -1, 1, 0, 1 }, { 1, 1, 0, 1 },
	};
	static const float quv[4][2] = { { 0, 1 }, { 1, 1 }, { 0, 0 }, { 1, 0 } };

	[enc setRenderPipelineState:ps];
	[enc setVertexBytes:qpos length:sizeof(qpos) atIndex:0];
	[enc setVertexBytes:quv length:sizeof(quv) atIndex:1];
	[enc setFragmentTexture:tex atIndex:0];
	[enc setFragmentSamplerState:m->samp atIndex:0];
}

/* ---- --picture --pixels: the blend matrix through Metal's descriptors ---- */
static void
mpx_blend(struct mpx *m)
{
	static const struct { const char *name; struct dm_blend b; } cases[] = {
		{ "srcover", { 1, DM_ADD, DM_ADD, DM_SA, DM_ISA, DM_SA, DM_ISA, DM_ALL } },
		{ "premult", { 1, DM_ADD, DM_ADD, DM_ONE, DM_ISA, DM_ONE, DM_ISA, DM_ALL } },
		{ "oneone", { 1, DM_ADD, DM_ADD, DM_ONE, DM_ONE, DM_ONE, DM_ONE, DM_ALL } },
		{ "min", { 1, DM_MIN, DM_MIN, DM_ONE, DM_ONE, DM_ONE, DM_ONE, DM_ALL } },
		{ "max", { 1, DM_MAX, DM_MAX, DM_ONE, DM_ONE, DM_ONE, DM_ONE, DM_ALL } },
		{ "revsub", { 1, DM_RSUB, DM_RSUB, DM_ONE, DM_ONE, DM_ONE, DM_ONE, DM_ALL } },
		{ "constant", { 1, DM_ADD, DM_ADD, DM_BC, DM_IBC, DM_BA, DM_IBA, DM_ALL } },
		{ "mask-r", { 1, DM_ADD, DM_ADD, DM_ONE, DM_ISA, DM_ONE, DM_ISA, DM_R } },
		{ "mask-rgb", { 1, DM_ADD, DM_ADD, DM_ONE, DM_ISA, DM_ONE, DM_ISA, DM_R | DM_G | DM_B } },
		{ "mask-gba", { 1, DM_ADD, DM_ADD, DM_ONE, DM_ISA, DM_ONE, DM_ISA, DM_G | DM_B | DM_A } },
	};
	static const unsigned formats[] = { DM_RGBA32F, DM_RGBA8, DM_BGRA8, DM_RGBA8_SRGB, DM_BGRA8_SRGB };
	static const float k[4] = { 0.25f, 0.5f, 0.75f, 0.125f };
	static struct dm_picture want;
	MTLClearColor none = MTLClearColorMake(0, 0, 0, 0);

	for (unsigned f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
		unsigned fmt = formats[f];
		const unsigned fs[4] = { fmt, fmt, fmt, fmt };
		id<MTLTexture> t[4];

		for (unsigned i = 0; i < 4; i++) {
			t[i] = mpx_texture(m, fmt, NULL);
		}
		for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
			char name[64];
			struct dm_blend b[2] = { cases[c].b, { 0, 0, 0, 1, 0, 1, 0, 0 } };

			snprintf(name, sizeof(name), "blend-%s-%s", dm_format_name(fmt), cases[c].name);
			id<MTLRenderPipelineState> ps = mpx_pipeline(m, m->vtx2d, nil, fs, b, 0, name);

			if (ps == nil) {
				continue;
			}
			unsigned texel = dm_texel_bytes(fmt);

			for (unsigned y = 0; y < DM_H; y++) {
				for (unsigned x = 0; x < DM_W; x++) {
					float d[4];

					dm_dst(x, y, d);
					dm_encode(fmt, d, mpx_mem + ((size_t)y * DM_W + x) * texel);
				}
			}
			[t[0] replaceRegion:MTLRegionMake2D(0, 0, DM_W, DM_H)
				mipmapLevel:0
				  withBytes:mpx_mem
				bytesPerRow:DM_W * texel];
			MTLRenderPassDescriptor *rp = mpx_pass(t, MTLLoadActionLoad, none);
			id<MTLCommandBuffer> cb = [m->queue commandBuffer];
			id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];

			mpx_bind_quad(m, enc, ps, m->tsrc);
			[enc setBlendColorRed:k[0] green:k[1] blue:k[2] alpha:k[3]];
			[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
			[enc endEncoding];
			if (mpx_finish(m, cb, name)) {
				dm_blend_model(&want, fmt, &cases[c].b, k, dm_src);
				mpx_judge(m, t[0], fmt, name, &want, 0.0f, false);
			}
			[ps release];
		}
		for (unsigned i = 0; i < 4; i++) {
			[t[i] release];
		}
	}
}

/* ---- --blit --pixels: what a pass keeps, clears, stores and hands to the next ---- */
static void
mpx_passes(struct mpx *m)
{
	static struct dm_picture want;
	static const float black[4] = { 0, 0, 0, 0 };
	const struct dm_viewport whole = { 0, 0, DM_W, DM_H };
	MTLClearColor none = MTLClearColorMake(0, 0, 0, 0);
	id<MTLTexture> t[4];
	float uv[3][2];

	mpx_tri_uv(uv);
	for (unsigned i = 0; i < 4; i++) {
		t[i] = mpx_texture(m, DM_RGBA32F, NULL);
	}
	id<MTLRenderPipelineState> ps = mpx_pipeline(m, m->vtx2d, nil, mpx_all32f, NULL, 0, "pass-pipeline");

	if (ps == nil) {
		return;
	}
	float tpos[3][4], tuv[3][2];

	for (unsigned i = 0; i < 3; i++) {
		tpos[i][0] = mpx_tri[i][0] * 2.0f / DM_W - 1.0f;
		tpos[i][1] = 1.0f - mpx_tri[i][1] * 2.0f / DM_H;
		tpos[i][2] = 0.0f;
		tpos[i][3] = 1.0f;
		tuv[i][0] = uv[i][0];
		tuv[i][1] = uv[i][1];
	}

	/* 1: Load keeps: a checkerboard from one pass survives everywhere but under the triangle
	 * of the next, which is a command buffer of its own. */
	{
		MTLRenderPassDescriptor *a = mpx_pass(t, MTLLoadActionClear, none);
		id<MTLCommandBuffer> cb = [m->queue commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:a];

		mpx_bind_quad(m, enc, ps, m->tcheck);
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		[enc endEncoding];
		mpx_finish(m, cb, "pass-load-first");
		MTLRenderPassDescriptor *b = mpx_pass(t, MTLLoadActionLoad, none);

		cb = [m->queue commandBuffer];
		enc = [cb renderCommandEncoderWithDescriptor:b];
		[enc setRenderPipelineState:ps];
		[enc setVertexBytes:tpos length:sizeof(tpos) atIndex:0];
		[enc setVertexBytes:tuv length:sizeof(tuv) atIndex:1];
		[enc setFragmentTexture:m->tsrc atIndex:0];
		[enc setFragmentSamplerState:m->samp atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[enc endEncoding];
		if (mpx_finish(m, cb, "pass-load")) {
			dm_clear(&want, black);
			for (unsigned y = 0; y < DM_H; y++) {
				for (unsigned x = 0; x < DM_W; x++) {
					dm_check(x, y, want.px[y][x]);
				}
			}
			mpx_model(&want, mpx_tri, uv, &whole, dm_whole(), 0, 0, dm_src, false);
			mpx_judge(m, t[0], DM_RGBA32F, "pass-load", &want, 0.0f, false);
		}
	}

	/* 2: Clear: the triangle, and the clear's colour everywhere else, per pixel for it. */
	{
		MTLClearColor c = MTLClearColorMake(0.25, 0.5, 0.75, 1.0);
		static const float cf[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
		MTLRenderPassDescriptor *a = mpx_pass(t, MTLLoadActionClear, c);
		id<MTLCommandBuffer> cb = [m->queue commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:a];

		[enc setRenderPipelineState:ps];
		[enc setVertexBytes:tpos length:sizeof(tpos) atIndex:0];
		[enc setVertexBytes:tuv length:sizeof(tuv) atIndex:1];
		[enc setFragmentTexture:m->tsrc atIndex:0];
		[enc setFragmentSamplerState:m->samp atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[enc endEncoding];
		if (mpx_finish(m, cb, "pass-clear")) {
			dm_clear(&want, cf);
			mpx_model(&want, mpx_tri, uv, &whole, dm_whole(), 0, 0, dm_src, false);
			mpx_judge(m, t[0], DM_RGBA32F, "pass-clear", &want, 0.0f, false);
		}
	}

	/* 3-4: DontCare, either end: all this driver can promise and all an application may rely
	 * on is that a full-attachment draw after a load DontCare is that draw, and that a store
	 * DontCare does not fail. It is served as a keep and a store (hkmetal.m), so what was
	 * stored is judged too: for that case the line is about this driver, not about Metal. */
	{
		MTLRenderPassDescriptor *a = mpx_pass(t, MTLLoadActionDontCare, none);

		[[[a colorAttachments] objectAtIndexedSubscript:0] setStoreAction:MTLStoreActionDontCare];
		id<MTLCommandBuffer> cb = [m->queue commandBuffer];
		id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:a];

		mpx_bind_quad(m, enc, ps, m->tsrc);
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		[enc endEncoding];
		if (mpx_finish(m, cb, "pass-dontcare")) {
			dm_clear(&want, black);
			for (unsigned y = 0; y < DM_H; y++) {
				for (unsigned x = 0; x < DM_W; x++) {
					dm_src(x, y, want.px[y][x]);
				}
			}
			mpx_judge(m, t[0], DM_RGBA32F, "pass-dontcare", &want, 0.0f, false);
		}
	}

	/* 5-8: each attachment its own load action: 0 clears, 1 keeps a per-pixel fill, and with
	 * those two write masks None what is left in them is the load actions alone. Gallium
	 * clears them all with the first colour, which it always has, and says gallium-lacks.
	 * Then two attachments in one pass with their own blend and mask, each judged. */
	{
		const struct dm_blend off[2] = { { 0, 0, 0, 1, 0, 1, 0, 0 }, { 0, 0, 0, 1, 0, 1, 0, 0 } };
		id<MTLRenderPipelineState> none_ps = mpx_pipeline(m, m->vtx2d, nil, mpx_all32f, off, 0,
								  "pass-load-actions");
		/* attachments 2 and 3 keep Metal's default mask: only 0 and 1 are None here */
		if (none_ps != nil) {
			static const float c0[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
			for (unsigned y = 0; y < DM_H; y++) {
				for (unsigned x = 0; x < DM_W; x++) {
					float d[4];

					dm_dst(x, y, d);
					dm_encode(DM_RGBA32F, d, mpx_mem + ((size_t)y * DM_W + x) * 16);
				}
			}
			[t[1] replaceRegion:MTLRegionMake2D(0, 0, DM_W, DM_H)
				mipmapLevel:0
				  withBytes:mpx_mem
				bytesPerRow:DM_W * 16];
			MTLRenderPassDescriptor *a = mpx_pass(t, MTLLoadActionLoad, none);
			MTLRenderPassColorAttachmentDescriptor *a0 =
			    [[a colorAttachments] objectAtIndexedSubscript:0];

			[a0 setLoadAction:MTLLoadActionClear];
			[a0 setClearColor:MTLClearColorMake(c0[0], c0[1], c0[2], c0[3])];
			id<MTLCommandBuffer> cb = [m->queue commandBuffer];
			id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:a];

			mpx_bind_quad(m, enc, none_ps, m->tsrc);
			[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
			[enc endEncoding];
			if (mpx_finish(m, cb, "pass-load-actions")) {
				dm_clear(&want, c0);
				mpx_judge(m, t[0], DM_RGBA32F, "pass-clear-attachment0", &want, 0.0f, false);
				dm_clear(&want, black);
				for (unsigned y = 0; y < DM_H; y++) {
					for (unsigned x = 0; x < DM_W; x++) {
						dm_dst(x, y, want.px[y][x]);
					}
				}
				/* Gallium clears every attachment with attachment 0's colour */
				mpx_judge(m, t[1], DM_RGBA32F, "pass-keep-attachment1", &want, 0.0f, m->gallium);
			}
			[none_ps release];
		}
		const struct dm_blend mrt[2] = {
			{ 1, DM_ADD, DM_ADD, DM_ONE, DM_ISA, DM_ONE, DM_ISA, DM_ALL },
			{ 1, DM_ADD, DM_ADD, DM_ONE, DM_ONE, DM_ONE, DM_ONE, DM_R | DM_A },
		};
		const unsigned two[4] = { DM_RGBA32F, DM_RGBA32F, DM_RGBA32F, DM_RGBA32F };
		id<MTLRenderPipelineState> mps = mpx_pipeline(m, m->vtx2d, nil, two, mrt, 0, "pass-mrt-blend");

		if (mps != nil) {
			for (unsigned y = 0; y < DM_H; y++) {
				for (unsigned x = 0; x < DM_W; x++) {
					float d[4];

					dm_dst(x, y, d);
					dm_encode(DM_RGBA32F, d, mpx_mem + ((size_t)y * DM_W + x) * 16);
				}
			}
			for (unsigned i = 0; i < 2; i++) {
				[t[i] replaceRegion:MTLRegionMake2D(0, 0, DM_W, DM_H)
					mipmapLevel:0
					  withBytes:mpx_mem
					bytesPerRow:DM_W * 16];
			}
			MTLRenderPassDescriptor *a = mpx_pass(t, MTLLoadActionLoad, none);
			id<MTLCommandBuffer> cb = [m->queue commandBuffer];
			id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:a];

			mpx_bind_quad(m, enc, mps, m->tsrc);
			[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
			[enc endEncoding];
			if (mpx_finish(m, cb, "pass-mrt-blend")) {
				dm_blend_model(&want, DM_RGBA32F, &mrt[0], black, dm_src);
				mpx_judge(m, t[0], DM_RGBA32F, "pass-mrt-blend-0", &want, 0.0f, false);
				dm_blend_model(&want, DM_RGBA32F, &mrt[1], black, dm_src);
				mpx_judge(m, t[1], DM_RGBA32F, "pass-mrt-blend-1", &want, 0.0f, false);
			}
			[mps release];
		}
	}

	/* 9: several encoders in ONE command buffer, in order: the first draws the pattern into a
	 * target, a blit copies it, the next samples the copy into another. Sampled in a pass
	 * after the one that drew it, with nothing between them but this driver's barriers. */
	{
		id<MTLTexture> mid[4], copy = mpx_texture(m, DM_RGBA32F, NULL), last[4];

		for (unsigned i = 0; i < 4; i++) {
			mid[i] = mpx_texture(m, DM_RGBA32F, NULL);
			last[i] = mpx_texture(m, DM_RGBA32F, NULL);
		}
		id<MTLCommandBuffer> cb = [m->queue commandBuffer];
		MTLRenderPassDescriptor *a = mpx_pass(mid, MTLLoadActionClear, none);
		id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:a];

		mpx_bind_quad(m, enc, ps, m->tsrc);
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		[enc endEncoding];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];

		[blit copyFromTexture:mid[0]
			  sourceSlice:0
			  sourceLevel:0
			 sourceOrigin:MTLOriginMake(0, 0, 0)
			   sourceSize:MTLSizeMake(DM_W, DM_H, 1)
			    toTexture:copy
		     destinationSlice:0
		     destinationLevel:0
		    destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit endEncoding];
		MTLRenderPassDescriptor *b = mpx_pass(last, MTLLoadActionClear, none);

		enc = [cb renderCommandEncoderWithDescriptor:b];
		mpx_bind_quad(m, enc, ps, mid[0]);
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		[enc endEncoding];
		/* and a third over the first's own attachment again, sampling the blit's copy */
		MTLRenderPassDescriptor *c = mpx_pass(mid, MTLLoadActionClear, none);

		enc = [cb renderCommandEncoderWithDescriptor:c];
		mpx_bind_quad(m, enc, ps, copy);
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		[enc endEncoding];
		if (mpx_finish(m, cb, "pass-encoders")) {
			dm_clear(&want, black);
			for (unsigned y = 0; y < DM_H; y++) {
				for (unsigned x = 0; x < DM_W; x++) {
					dm_src(x, y, want.px[y][x]);
				}
			}
			mpx_judge(m, last[0], DM_RGBA32F, "pass-encoders-sampled", &want, 0.0f, false);
			mpx_judge(m, mid[0], DM_RGBA32F, "pass-encoders-copied", &want, 0.0f, false);
		}
		for (unsigned i = 0; i < 4; i++) {
			[mid[i] release];
			[last[i] release];
		}
		[copy release];
	}
	[ps release];
	for (unsigned i = 0; i < 4; i++) {
		[t[i] release];
	}
}


/*
 * ---- M5b: --tex --pixels, --texfmt --pixels, and more of --blit --pixels ----
 *
 * What those were: --tex and --texfmt say which kind and format a sweep binds for a whole library
 * (they measure a shader, and read nothing of a level or a slice); --blit runs each blit once on one
 * level, and --blit --pixels (M5a) what a pass keeps and clears. The same questions as
 * air_metal_test's --textures, --pixel-formats and --blit --pixels, through Metal's own objects, so
 * that hkmetal.m's half is measured too: MTLTextureDescriptor's levels and slices, -replaceRegion:
 * and -getBytes: with them, the three view calls, MTLSamplerDescriptor's whole state,
 * MTLRenderPassDescriptor's level, slice and depthPlane, a pass that only clears, and the blit
 * encoder. The fragment stage is texturedFragment2DFloat, so what a sampler answers is at
 * coordinates of this file's making, on a quad over four RGBA32Float attachments, of which the first
 * is read. tex_model.h has the numbers; a case the Gallium library has no way to serve says
 * gallium-lacks and what it drew is in the digest.
 */
static id<MTLTexture> mtx_t[4];
static id<MTLRenderPipelineState> mtx_ps;
static uint8_t mtx_mem[64 * 64 * 16 * 2];

static bool
mtx_bytes(struct mpx *m, const char *name, const void *got, const void *want, unsigned count,
	  unsigned texel, unsigned w, bool lacks)
{
	bool ok = tm_bytes(m->backend, name, got, want, count, texel, w, lacks);

	if (ok) {
		m->right++;
	} else if (lacks) {
		m->lacks++;
	} else {
		m->wrong++;
	}
	return ok;
}

/* The quad of mpx_bind_quad with its coordinate scaled and offset, row 0 being v = o. */
static bool
mtx_draw(struct mpx *m, id<MTLTexture> tex, id<MTLSamplerState> samp, float su, float sv, float ou,
	 float ov, const char *name)
{
	static const float qpos[4][4] = {
		{ -1, -1, 0, 1 }, { 1, -1, 0, 1 }, { -1, 1, 0, 1 }, { 1, 1, 0, 1 },
	};
	const float quv[4][2] = {
		{ ou, ov + sv }, { ou + su, ov + sv }, { ou, ov }, { ou + su, ov },
	};
	MTLRenderPassDescriptor *rp = mpx_pass(mtx_t, MTLLoadActionClear, MTLClearColorMake(0, 0, 0, 0));
	id<MTLCommandBuffer> cb = [m->queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];

	[enc setRenderPipelineState:mtx_ps];
	[enc setVertexBytes:qpos length:sizeof(qpos) atIndex:0];
	[enc setVertexBytes:quv length:sizeof(quv) atIndex:1];
	[enc setFragmentTexture:tex atIndex:0];
	[enc setFragmentSamplerState:samp atIndex:0];
	[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	[enc endEncoding];
	return mpx_finish(m, cb, name);
}

static id<MTLTexture>
mtx_make(struct mpx *m, MTLTextureType type, unsigned fmt, unsigned w, unsigned h, unsigned d,
	 unsigned slices, unsigned levels, MTLTextureUsage usage, const char *name)
{
	MTLTextureDescriptor *td = [[MTLTextureDescriptor alloc] init];

	[td setTextureType:type];
	[td setPixelFormat:(MTLPixelFormat)fmt];
	[td setWidth:w];
	[td setHeight:h];
	[td setDepth:d];
	[td setArrayLength:slices];
	[td setMipmapLevelCount:levels];
	[td setUsage:usage];
	id<MTLTexture> t = [m->dev newTextureWithDescriptor:td];

	[td release];
	if (t == nil) {
		dm_refused(m->backend, name, "newTextureWithDescriptor: answered nil");
		m->refused++;
	}
	return t;
}

/* Every level and slice of an RGBA8 texture filled with tm_fill. */
static void
mtx_fill(id<MTLTexture> t, unsigned w, unsigned levels, unsigned slices)
{
	for (unsigned l = 0; l < levels; l++) {
		for (unsigned s = 0; s < slices; s++) {
			unsigned lw = MAX(w >> l, 1);

			tm_fill(mtx_mem, l, s, lw, lw);
			[t replaceRegion:MTLRegionMake2D(0, 0, lw, lw)
			     mipmapLevel:l
				   slice:s
			       withBytes:mtx_mem
			     bytesPerRow:lw * 4
			   bytesPerImage:0];
		}
	}
}

static void
mtx_want_texels(struct dm_picture *want, unsigned level, unsigned slice, unsigned w, unsigned h)
{
	dm_clear(want, (const float[4]){ 0, 0, 0, 0 });
	for (unsigned y = 0; y < DM_H; y++) {
		for (unsigned x = 0; x < DM_W; x++) {
			tm_texelf(level, slice, x * w / DM_W, y * h / DM_H, want->px[y][x]);
		}
	}
}

static id<MTLSamplerState>
mtx_sampler(struct mpx *m, MTLSamplerAddressMode a, MTLSamplerMipFilter mip, MTLSamplerBorderColor b,
	    float lmin, float lmax)
{
	MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];

	[sd setMinFilter:MTLSamplerMinMagFilterNearest];
	[sd setMagFilter:MTLSamplerMinMagFilterNearest];
	[sd setMipFilter:mip];
	[sd setSAddressMode:a];
	[sd setTAddressMode:a];
	[sd setBorderColor:b];
	[sd setLodMinClamp:lmin];
	[sd setLodMaxClamp:lmax];
	id<MTLSamplerState> s = [m->dev newSamplerStateWithDescriptor:sd];

	[sd release];
	return s;
}

/* A picture of tm_texelf through an address mode, the coordinate going from o to o + 16s. */
static void
mtx_want_address(struct dm_picture *want, unsigned mode, float s, float o, const float border[4])
{
	dm_clear(want, (const float[4]){ 0, 0, 0, 0 });
	for (unsigned y = 0; y < DM_H; y++) {
		for (unsigned x = 0; x < DM_W; x++) {
			int i = tm_wrap(mode, ((float)x + 0.5f) * s / 16.0f + o, 16);
			int j = tm_wrap(mode, ((float)y + 0.5f) * s / 16.0f + o, 16);

			if (i < 0 || j < 0) {
				memcpy(want->px[y][x], border, 4 * sizeof(float));
			} else {
				tm_texelf(0, 0, (unsigned)i, (unsigned)j, want->px[y][x]);
			}
		}
	}
}

static void
mtx_tex(struct mpx *m)
{
	static struct dm_picture want;
	static uint8_t got[64 * 64 * 4];
	static const float zero[4] = { 0, 0, 0, 0 };
	const float e = 1.0f / 2048.0f;
	const bool lk = m->gallium;
	char name[64];

	/* -replaceRegion: and -getBytes:, by level and by slice */
	id<MTLTexture> t = mtx_make(m, MTLTextureType2DArray, 70, 16, 16, 1, 3, 3,
				    MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget |
					MTLTextureUsagePixelFormatView, "mtl-array");
	if (t != nil) {
		mtx_fill(t, 16, 3, 3);
		for (unsigned l = 0; l < 3; l++) {
			for (unsigned s = 0; s < 3; s++) {
				unsigned lw = 16 >> l;

				snprintf(name, sizeof(name), "mtl-array-L%u-S%u-rw", l, s);
				memset(got, 0, sizeof(got));
				[t getBytes:got
				bytesPerRow:lw * 4
			      bytesPerImage:0
				 fromRegion:MTLRegionMake2D(0, 0, lw, lw)
				mipmapLevel:l
				      slice:s];
				tm_fill(mtx_mem, l, s, lw, lw);
				mtx_bytes(m, name, got, mtx_mem, lw * lw, 4, lw, false);
			}
		}
		/* THE VIEW CALLS. A slice of a level as a 2D texture; and what it says of itself */
		id<MTLTexture> v = [t newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm
							 textureType:MTLTextureType2D
							      levels:NSMakeRange(1, 1)
							      slices:NSMakeRange(2, 1)];
		if (v == nil) {
			dm_refused(m->backend, "mtl-view-level-slice", "a view is nil here");
			m->lacks++;
		} else {
			if (mtx_draw(m, v, m->samp, 1, 1, 0, 0, "mtl-view-level-slice")) {
				mtx_want_texels(&want, 1, 2, 8, 8);
				mpx_judge(m, mtx_t[0], DM_RGBA32F, "mtl-view-level-slice", &want, e, false);
			}
			printf("drvtest: view-says %s parent %s level %lu slice %lu %lux%lu type %lu\n",
			       m->backend, [v parentTexture] == t ? "yes" : "NO",
			       (unsigned long)[v parentRelativeLevel],
			       (unsigned long)[v parentRelativeSlice], (unsigned long)[v width],
			       (unsigned long)[v height], (unsigned long)[v textureType]);
			[v release];
		}
		/* another format of the one size */
		v = [t newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm
					 textureType:MTLTextureType2D
					      levels:NSMakeRange(0, 1)
					      slices:NSMakeRange(1, 1)];
		if (v == nil) {
			dm_refused(m->backend, "mtl-view-format", "a view is nil here");
			m->lacks++;
		} else {
			if (mtx_draw(m, v, m->samp, 1, 1, 0, 0, "mtl-view-format")) {
				mtx_want_texels(&want, 0, 1, 16, 16);
				for (unsigned y = 0; y < DM_H; y++) {
					for (unsigned x = 0; x < DM_W; x++) {
						float k = want.px[y][x][0];

						want.px[y][x][0] = want.px[y][x][2];
						want.px[y][x][2] = k;
					}
				}
				mpx_judge(m, mtx_t[0], DM_RGBA32F, "mtl-view-format", &want, e, false);
			}
			[v release];
		}
		/* a swizzle, each channel another's */
		MTLTextureSwizzleChannels sw = MTLTextureSwizzleChannelsMake(
			MTLTextureSwizzleAlpha, MTLTextureSwizzleBlue, MTLTextureSwizzleOne,
			MTLTextureSwizzleZero);
		v = [t newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm
					 textureType:MTLTextureType2D
					      levels:NSMakeRange(0, 1)
					      slices:NSMakeRange(0, 1)
					     swizzle:sw];
		if (v == nil) {
			dm_refused(m->backend, "mtl-view-swizzle", "a view is nil here");
			m->lacks++;
		} else {
			if (mtx_draw(m, v, m->samp, 1, 1, 0, 0, "mtl-view-swizzle")) {
				mtx_want_texels(&want, 0, 0, 16, 16);
				for (unsigned y = 0; y < DM_H; y++) {
					for (unsigned x = 0; x < DM_W; x++) {
						float *p = want.px[y][x], a = p[3], b = p[2];

						p[0] = a;
						p[1] = b;
						p[2] = 1.0f;
						p[3] = 0.0f;
					}
				}
				mpx_judge(m, mtx_t[0], DM_RGBA32F, "mtl-view-swizzle", &want, e, false);
			}
			[v release];
		}
		[t release];
	}
	/* a descriptor's own swizzle, which hkmetal.m serves with a view */
	{
		MTLTextureDescriptor *td =
		    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
								       width:16
								      height:16
								   mipmapped:NO];
		[td setUsage:MTLTextureUsageShaderRead];
		[td setSwizzle:MTLTextureSwizzleChannelsMake(MTLTextureSwizzleBlue, MTLTextureSwizzleGreen,
							     MTLTextureSwizzleRed, MTLTextureSwizzleAlpha)];
		id<MTLTexture> ds = [m->dev newTextureWithDescriptor:td];

		if (ds != nil) {
			mtx_fill(ds, 16, 1, 1);
			if (mtx_draw(m, ds, m->samp, 1, 1, 0, 0, "mtl-descriptor-swizzle")) {
				mtx_want_texels(&want, 0, 0, 16, 16);
				for (unsigned y = 0; y < DM_H; y++) {
					for (unsigned x = 0; x < DM_W; x++) {
						float k = want.px[y][x][0];

						want.px[y][x][0] = want.px[y][x][2];
						want.px[y][x][2] = k;
					}
				}
				mpx_judge(m, mtx_t[0], DM_RGBA32F, "mtl-descriptor-swizzle", &want, e, lk);
			}
			[ds release];
		}
	}
	/* the same over an IOSurface (-newTextureWithDescriptor:iosurface:plane:), the bytes written
	 * through the surface's own mapping: B G R A in memory, which BGRA8Unorm reads as the texel,
	 * and the swizzle then exchanges red and blue */
	{
		IOSurfaceRef surf = frames_surface(16, 16);
		MTLTextureDescriptor *td =
		    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
								       width:16
								      height:16
								   mipmapped:NO];
		[td setUsage:MTLTextureUsageShaderRead];
		[td setSwizzle:MTLTextureSwizzleChannelsMake(MTLTextureSwizzleBlue, MTLTextureSwizzleGreen,
							     MTLTextureSwizzleRed, MTLTextureSwizzleAlpha)];
		id<MTLTexture> ss = surf != NULL ? [m->dev newTextureWithDescriptor:td iosurface:surf plane:0] : nil;

		if (ss == nil) {
			dm_refused(m->backend, "mtl-iosurface-swizzle", "no BGRA texture over an IOSurface");
			m->refused++;
		} else {
			IOSurfaceLock(surf, 0, NULL);
			uint8_t *base = IOSurfaceGetBaseAddress(surf);
			size_t stride = IOSurfaceGetBytesPerRow(surf);

			for (unsigned y = 0; y < 16; y++) {
				for (unsigned x = 0; x < 16; x++) {
					uint8_t rgba[4], *to = base + y * stride + x * 4;

					tm_texel(0, 0, x, y, rgba);
					to[0] = rgba[2];
					to[1] = rgba[1];
					to[2] = rgba[0];
					to[3] = rgba[3];
				}
			}
			IOSurfaceUnlock(surf, 0, NULL);
			if (mtx_draw(m, ss, m->samp, 1, 1, 0, 0, "mtl-iosurface-swizzle")) {
				mtx_want_texels(&want, 0, 0, 16, 16);
				for (unsigned y = 0; y < DM_H; y++) {
					for (unsigned x = 0; x < DM_W; x++) {
						float k = want.px[y][x][0];

						want.px[y][x][0] = want.px[y][x][2];
						want.px[y][x][2] = k;
					}
				}
				mpx_judge(m, mtx_t[0], DM_RGBA32F, "mtl-iosurface-swizzle", &want, e, lk);
			}
			[ss release];
		}
		if (surf != NULL) {
			CFRelease(surf);
		}
	}
	/* a 3D region with a bytesPerImage that is not the packed one */
	t = mtx_make(m, MTLTextureType3D, 70, 8, 8, 4, 1, 1, MTLTextureUsageShaderRead, "mtl-3d");
	if (t != nil) {
		const unsigned image = 8 * 4 * 10;

		memset(mtx_mem, 0xa5, image * 4);
		for (unsigned z = 0; z < 4; z++) {
			tm_fill(mtx_mem + (size_t)z * image, 0, z, 8, 8);
		}
		[t replaceRegion:MTLRegionMake3D(0, 0, 0, 8, 8, 4)
		     mipmapLevel:0
			   slice:0
		       withBytes:mtx_mem
		     bytesPerRow:8 * 4
		   bytesPerImage:image];
		static uint8_t all[8 * 8 * 4 * 4], exp[8 * 8 * 4 * 4];
		[t getBytes:all
		bytesPerRow:8 * 4
	      bytesPerImage:8 * 8 * 4
		 fromRegion:MTLRegionMake3D(0, 0, 0, 8, 8, 4)
		mipmapLevel:0
		      slice:0];
		for (unsigned z = 0; z < 4; z++) {
			tm_fill(exp + (size_t)z * 8 * 8 * 4, 0, z, 8, 8);
		}
		mtx_bytes(m, "mtl-3d-bytesperimage", all, exp, 256, 4, 8, false);
		[t release];
	}
	/* MTLSamplerDescriptor: every address mode, and the border colours */
	t = mtx_make(m, MTLTextureType2D, 70, 16, 16, 1, 1, 5, MTLTextureUsageShaderRead, "mtl-sampler");
	if (t != nil) {
		mtx_fill(t, 16, 5, 1);
		static const char *const mode_name[6] = {
			"edge", "mirroredge", "repeat", "mirror", "zero", "border",
		};
		for (unsigned a = 0; a < 6; a++) {
			id<MTLSamplerState> s = mtx_sampler(m, (MTLSamplerAddressMode)a,
							    MTLSamplerMipFilterNotMipmapped,
							    MTLSamplerBorderColorTransparentBlack, 0, 1000);
			snprintf(name, sizeof(name), "mtl-sampler-addr-%s", mode_name[a]);
			if (s != nil && mtx_draw(m, t, s, 3, 3, -1, -1, name)) {
				mtx_want_address(&want, a, 3, -1, zero);
				mpx_judge(m, mtx_t[0], DM_RGBA32F, name, &want, e, false);
			}
			[s release];
		}
		static const float colours[3][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 1 }, { 1, 1, 1, 1 } };
		for (unsigned b = 0; b < 3; b++) {
			id<MTLSamplerState> s = mtx_sampler(m, MTLSamplerAddressModeClampToBorderColor,
							    MTLSamplerMipFilterNotMipmapped,
							    (MTLSamplerBorderColor)b, 0, 1000);
			snprintf(name, sizeof(name), "mtl-sampler-border-%u", b);
			if (s != nil && mtx_draw(m, t, s, 3, 3, -1, -1, name)) {
				mtx_want_address(&want, TM_BORDER, 3, -1, colours[b]);
				mpx_judge(m, mtx_t[0], DM_RGBA32F, name, &want, e, lk && b != 0);
			}
			[s release];
		}
		/* the LOD, by the derivatives: a coordinate stepping 4 texels a pixel is level 2,
		 * and a clamp to [0, 1] makes it level 1. Metal's own default is no clamp. */
		id<MTLSamplerState> s = mtx_sampler(m, MTLSamplerAddressModeRepeat, MTLSamplerMipFilterNearest,
						    MTLSamplerBorderColorTransparentBlack, 0, 1000);
		for (unsigned c = 0; c < 2; c++) {
			if (c == 1) {
				[s release];
				s = mtx_sampler(m, MTLSamplerAddressModeRepeat, MTLSamplerMipFilterNearest,
						MTLSamplerBorderColorTransparentBlack, 0, 1.0f);
			}
			snprintf(name, sizeof(name), c ? "mtl-sampler-lodclamp" : "mtl-sampler-lod");
			if (s != nil && mtx_draw(m, t, s, 4, 4, 0, 0, name)) {
				unsigned l = c ? 1 : 2, lw = 16 >> l;

				dm_clear(&want, zero);
				for (unsigned y = 0; y < DM_H; y++) {
					for (unsigned x = 0; x < DM_W; x++) {
						tm_texelf(l, 0,
							  (unsigned)tm_wrap(TM_REPEAT, ((float)x + 0.5f) / 4.0f, lw),
							  (unsigned)tm_wrap(TM_REPEAT, ((float)y + 0.5f) / 4.0f, lw),
							  want.px[y][x]);
					}
				}
				mpx_judge(m, mtx_t[0], DM_RGBA32F, name, &want, e, false);
			}
		}
		[s release];
		[t release];
	}
	/* THE PASS: into level 1 of a mip chain, into slice 2, into a face and a depth plane, the
	 * neighbours read back. And a pass that draws nothing and only clears one level: hkmetal.m's
	 * own path through a view. A quad of tsrc nearest, into four attachments of one RGBA8 chain's
	 * one level would need four; so one attachment with a pipeline of its own format. */
	{
		MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
		NSError *err = nil;

		[pd setVertexFunction:m->vtx2d];
		[pd setFragmentFunction:m->frag2d];
		[[[pd colorAttachments] objectAtIndexedSubscript:0] setPixelFormat:MTLPixelFormatRGBA8Unorm];
		id<MTLRenderPipelineState> ps = [m->dev newRenderPipelineStateWithDescriptor:pd error:&err];

		[pd release];
		static const struct { const char *name; MTLTextureType type; unsigned d, slices, levels;
				      unsigned level, slice, plane, neighbour; } passes[] = {
			{ "mtl-pass-level1", MTLTextureType2D, 1, 1, 3, 1, 0, 0, 0 },
			{ "mtl-pass-slice2", MTLTextureType2DArray, 1, 4, 1, 0, 2, 0, 1 },
			{ "mtl-pass-face3", MTLTextureTypeCube, 1, 1, 1, 0, 3, 0, 2 },
			{ "mtl-pass-plane2", MTLTextureType3D, 4, 1, 1, 0, 0, 2, 1 },
		};
		for (unsigned i = 0; ps != nil && i < sizeof(passes) / sizeof(passes[0]); i++) {
			unsigned w = passes[i].levels > 1 ? 32 : 16;
			unsigned faces = passes[i].type == MTLTextureTypeCube ? 6 : passes[i].slices;
			id<MTLTexture> p = mtx_make(m, passes[i].type, 70, w, w, passes[i].d, passes[i].slices,
						    passes[i].levels,
						    MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget,
						    passes[i].name);
			if (p == nil) {
				continue;
			}
			if (passes[i].d > 1) {
				for (unsigned z = 0; z < passes[i].d; z++) {
					tm_fill(mtx_mem, 0, z, w, w);
					[p replaceRegion:MTLRegionMake3D(0, 0, z, w, w, 1)
					     mipmapLevel:0
						   slice:0
					       withBytes:mtx_mem
					     bytesPerRow:w * 4
					   bytesPerImage:0];
				}
			} else {
				mtx_fill(p, w, passes[i].levels, faces);
			}
			MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
			MTLRenderPassColorAttachmentDescriptor *a =
			    [[rp colorAttachments] objectAtIndexedSubscript:0];

			[a setTexture:p];
			[a setLevel:passes[i].level];
			[a setSlice:passes[i].slice];
			[a setDepthPlane:passes[i].plane];
			[a setLoadAction:MTLLoadActionLoad];
			[a setStoreAction:MTLStoreActionStore];
			id<MTLCommandBuffer> cb = [m->queue commandBuffer];
			id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];

			mpx_bind_quad(m, enc, ps, m->tsrc);
			[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
			[enc endEncoding];
			if (!mpx_finish(m, cb, passes[i].name)) {
				[p release];
				continue;
			}
			unsigned lw = w >> passes[i].level;

			/* tsrc is RGBA32F dm_src, sampled nearest onto its own pixels */
			memset(got, 0, sizeof(got));
			[p getBytes:got
			bytesPerRow:lw * 4
		      bytesPerImage:0
			 fromRegion:MTLRegionMake3D(0, 0, passes[i].plane, lw, lw, 1)
			mipmapLevel:passes[i].level
			      slice:passes[i].slice];
			for (unsigned y = 0; y < lw; y++) {
				for (unsigned x = 0; x < lw; x++) {
					float s[4];

					dm_src(x, y, s);
					for (unsigned c = 0; c < 4; c++) {
						mtx_mem[((size_t)y * lw + x) * 4 + c] = (uint8_t)dm_unorm8(s[c]);
					}
				}
			}
			mtx_bytes(m, passes[i].name, got, mtx_mem, lw * lw, 4, lw, lk);
			/* a neighbour in the same direction as the pass */
			snprintf(name, sizeof(name), "%s-neighbour", passes[i].name);
			unsigned nl = passes[i].levels > 1 ? 0 : passes[i].level;
			unsigned ns = passes[i].d > 1 ? 0 : (passes[i].levels > 1 ? 0 : passes[i].neighbour);
			unsigned nz = passes[i].d > 1 ? passes[i].neighbour : 0;
			unsigned nw = w >> nl;

			[p getBytes:got
			bytesPerRow:nw * 4
		      bytesPerImage:0
			 fromRegion:MTLRegionMake3D(0, 0, nz, nw, nw, 1)
			mipmapLevel:nl
			      slice:ns];
			tm_fill(mtx_mem, nl, passes[i].d > 1 ? nz : ns, nw, nw);
			mtx_bytes(m, name, got, mtx_mem, nw * nw, 4, nw, lk);
			[p release];
		}
		/* a clear and nothing else, of level 1, slice 1 */
		id<MTLTexture> cl = mtx_make(m, MTLTextureType2DArray, 70, 16, 16, 1, 2, 2,
					     MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget,
					     "mtl-pass-clear");
		if (cl != nil) {
			mtx_fill(cl, 16, 2, 2);
			MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
			MTLRenderPassColorAttachmentDescriptor *a =
			    [[rp colorAttachments] objectAtIndexedSubscript:0];

			[a setTexture:cl];
			[a setLevel:1];
			[a setSlice:1];
			[a setLoadAction:MTLLoadActionClear];
			[a setClearColor:MTLClearColorMake(0.5, 0.25, 0.75, 1.0)];
			[a setStoreAction:MTLStoreActionStore];
			id<MTLCommandBuffer> cb = [m->queue commandBuffer];
			id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];

			[enc endEncoding];
			if (mpx_finish(m, cb, "mtl-pass-clear")) {
				[cl getBytes:got
				 bytesPerRow:8 * 4
			       bytesPerImage:0
				  fromRegion:MTLRegionMake2D(0, 0, 8, 8)
				 mipmapLevel:1
				       slice:1];
				for (unsigned k = 0; k < 64; k++) {
					mtx_mem[k * 4 + 0] = 128;
					mtx_mem[k * 4 + 1] = 64;
					mtx_mem[k * 4 + 2] = 191;
					mtx_mem[k * 4 + 3] = 255;
				}
				mtx_bytes(m, "mtl-pass-clear", got, mtx_mem, 64, 4, 8, lk);
				[cl getBytes:got
				 bytesPerRow:8 * 4
			       bytesPerImage:0
				  fromRegion:MTLRegionMake2D(0, 0, 8, 8)
				 mipmapLevel:1
				       slice:0];
				tm_fill(mtx_mem, 1, 0, 8, 8);
				mtx_bytes(m, "mtl-pass-clear-neighbour", got, mtx_mem, 64, 4, 8, lk);
			}
			[cl release];
		}
		[ps release];
	}
}

/* Every format of tm_formats through Metal's descriptor: its bytes in and out, its texels as
 * texturedFragment2DFloat reads them where it can (a texture2d<float>, so no integer format), and a
 * render into it where it renders. The contract's mode of the same name judges the integer ones. */
static void
mtx_texfmt(struct mpx *m)
{
	static struct dm_picture want;
	static uint8_t mem[DM_W * DM_H * 16], got[DM_W * DM_H * 16], expect[DM_W * DM_H * 16];
	char name[80];

	for (unsigned i = 0; i < sizeof(tm_formats) / sizeof(tm_formats[0]); i++) {
		const struct tm_format *f = &tm_formats[i];
		const bool is_int = f->kind == TM_UINT || f->kind == TM_SINT;
		/* the Gallium library's 40-43 by its own mirror, tex_model.h says why */
		const struct tm_format *mf = m->gallium && tm_gallium_mirror(f->mtl)
						 ? tm_gallium_mirror(f->mtl) : f;
		const bool mirrored = mf != f;

		snprintf(name, sizeof(name), "mtlpf-%s", f->name);
		id<MTLTexture> t = mtx_make(m, MTLTextureType2D, f->mtl, DM_W, DM_H, 1, 1, 1,
					    MTLTextureUsageShaderRead |
						(f->renderable ? MTLTextureUsageRenderTarget : 0), name);
		if (t == nil) {
			continue;
		}
		for (unsigned y = 0; y < DM_H; y++) {
			for (unsigned x = 0; x < DM_W; x++) {
				tm_format_texel(f, x, y, mem + ((size_t)y * DM_W + x) * f->bytes);
			}
		}
		[t replaceRegion:MTLRegionMake2D(0, 0, DM_W, DM_H)
		     mipmapLevel:0
		       withBytes:mem
		     bytesPerRow:DM_W * f->bytes];
		[t getBytes:got
		bytesPerRow:DM_W * f->bytes
		 fromRegion:MTLRegionMake2D(0, 0, DM_W, DM_H)
		mipmapLevel:0];
		snprintf(name, sizeof(name), "mtlpf-%s-bytes", f->name);
		mtx_bytes(m, name, got, mem, DM_W * DM_H, f->bytes, DM_W, false);
		if (!is_int) {
			snprintf(name, sizeof(name), "mtlpf-%s-sample", f->name);
			if (mtx_draw(m, t, m->samp, 1, 1, 0, 0, name)) {
				dm_clear(&want, (const float[4]){ 0, 0, 0, 0 });
				/* M5c: an sRGB read against this GPU's measured curve (adreno_srgb.h) */
				const bool srgb = f->kind == TM_SRGB;
				dm_srgb_curve = srgb ? dm_adreno_srgb_decode : NULL;
				for (unsigned y = 0; y < DM_H; y++) {
					for (unsigned x = 0; x < DM_W; x++) {
						tm_format_read(mf, mem + ((size_t)y * DM_W + x) * f->bytes,
							       want.px[y][x]);
					}
				}
				dm_right_word = mirrored ? "gallium-mirror" : srgb ? "adreno-srgb" : "ok";
				if (mpx_judge(m, mtx_t[0], DM_RGBA32F, name, &want,
					      srgb ? 1.0f / 65536.0f : tm_format_slop(f), false) &&
				    mirrored) {
					m->right--;
					m->mirror++;
				}
				dm_right_word = "ok";
				dm_srgb_curve = NULL;
			}
			if (f->renderable) {
				MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
				NSError *err = nil;

				[pd setVertexFunction:m->vtx2d];
				[pd setFragmentFunction:m->frag2d];
				[[[pd colorAttachments] objectAtIndexedSubscript:0]
				    setPixelFormat:(MTLPixelFormat)f->mtl];
				id<MTLRenderPipelineState> ps =
				    [m->dev newRenderPipelineStateWithDescriptor:pd error:&err];

				[pd release];
				snprintf(name, sizeof(name), "mtlpf-%s-render", f->name);
				if (ps == nil) {
					dm_refused(m->backend, name, err != nil
						   ? [[err localizedDescription] UTF8String] : "(no reason)");
					m->refused++;
				} else {
					MTLRenderPassDescriptor *rp =
					    [MTLRenderPassDescriptor renderPassDescriptor];
					MTLRenderPassColorAttachmentDescriptor *a =
					    [[rp colorAttachments] objectAtIndexedSubscript:0];

					[a setTexture:t];
					[a setLoadAction:MTLLoadActionLoad];
					[a setStoreAction:MTLStoreActionStore];
					id<MTLCommandBuffer> cb = [m->queue commandBuffer];
					id<MTLRenderCommandEncoder> enc =
					    [cb renderCommandEncoderWithDescriptor:rp];

					mpx_bind_quad(m, enc, ps, m->tsrc);
					[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0
						vertexCount:4];
					[enc endEncoding];
					if (mpx_finish(m, cb, name)) {
						unsigned bad = 0;
						const unsigned steps = f->kind == TM_SRGB ? 5 : f->kind == TM_RG11B10 ? 2 : 1;

						[t getBytes:got
						bytesPerRow:DM_W * f->bytes
						 fromRegion:MTLRegionMake2D(0, 0, DM_W, DM_H)
						mipmapLevel:0];
						for (unsigned y = 0; y < DM_H; y++) {
							for (unsigned x = 0; x < DM_W; x++) {
								float v[4];
								uint8_t *e = expect + ((size_t)y * DM_W + x) * f->bytes;

								dm_src(x, y, v);
								tm_format_write(mf, v, e);
								bad += !tm_format_close(mf, got + ((size_t)y * DM_W + x) * f->bytes, e, steps);
							}
						}
						printf("drvtest: draw-result %s %s %s digest %016llx expected %016llx\n",
						       m->backend, name, bad ? "DIFFERS" : mirrored ? "gallium-mirror" : "ok",
						       (unsigned long long)dm_fnv(got, (size_t)DM_W * DM_H * f->bytes, 0xcbf29ce484222325ull),
						       (unsigned long long)dm_fnv(expect, (size_t)DM_W * DM_H * f->bytes, 0xcbf29ce484222325ull));
						if (bad) {
							printf("drvtest: draw-diff %s %s %u of 256 texels differ\n", m->backend, name, bad);
							m->wrong++;
						} else if (mirrored) {
							m->mirror++;
						} else {
							m->right++;
						}
					}
					[ps release];
				}
			}
		}
		[t release];
	}
}

/* ---- and of --blit --pixels: per level and per slice, and -generateMipmapsForTexture: ---- */
static void
mtx_blits(struct mpx *m)
{
	static uint8_t got[32 * 32 * 4], above[32 * 32 * 4], lev[32 * 32 * 4], box[32 * 32 * 4];
	id<MTLTexture> a = mtx_make(m, MTLTextureType2DArray, 70, 16, 16, 1, 3, 3, MTLTextureUsageShaderRead,
				    "mtl-blit");
	id<MTLTexture> b = mtx_make(m, MTLTextureType2DArray, 70, 16, 16, 1, 3, 3, MTLTextureUsageShaderRead,
				    "mtl-blit");
	id<MTLBuffer> buf = [m->dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];

	if (a != nil && b != nil && buf != nil) {
		mtx_fill(a, 16, 3, 3);
		for (unsigned l = 0; l < 3; l++) {
			for (unsigned s = 0; s < 3; s++) {
				unsigned lw = 16 >> l;

				tm_fill(mtx_mem, l, s + 5, lw, lw);
				[b replaceRegion:MTLRegionMake2D(0, 0, lw, lw)
				     mipmapLevel:l
					   slice:s
				       withBytes:mtx_mem
				     bytesPerRow:lw * 4
				   bytesPerImage:0];
			}
		}
		id<MTLCommandBuffer> cb = [m->queue commandBuffer];
		id<MTLBlitCommandEncoder> enc = [cb blitCommandEncoder];

		/* level 1 of slice 2, a region not at the origin, to level 1 of slice 1, elsewhere */
		[enc copyFromTexture:a
			 sourceSlice:2
			 sourceLevel:1
			sourceOrigin:MTLOriginMake(1, 2, 0)
			  sourceSize:MTLSizeMake(4, 3, 1)
			   toTexture:b
		    destinationSlice:1
		    destinationLevel:1
		   destinationOrigin:MTLOriginMake(3, 4, 0)];
		/* out to a buffer and back in to another slice, with a row wider than the region */
		[enc copyFromTexture:a
			 sourceSlice:0
			 sourceLevel:0
			sourceOrigin:MTLOriginMake(2, 3, 0)
			  sourceSize:MTLSizeMake(5, 4, 1)
			    toBuffer:buf
		   destinationOffset:64
	      destinationBytesPerRow:32
	    destinationBytesPerImage:0];
		[enc copyFromBuffer:buf
		       sourceOffset:64
		  sourceBytesPerRow:32
		sourceBytesPerImage:0
			 sourceSize:MTLSizeMake(5, 4, 1)
			  toTexture:b
		   destinationSlice:2
		   destinationLevel:0
		  destinationOrigin:MTLOriginMake(6, 7, 0)];
		[enc endEncoding];
		if (mpx_finish(m, cb, "mtl-blit-level-slice")) {
			[b getBytes:got
			bytesPerRow:8 * 4
		      bytesPerImage:0
			 fromRegion:MTLRegionMake2D(0, 0, 8, 8)
			mipmapLevel:1
			      slice:1];
			tm_fill(mtx_mem, 1, 6, 8, 8);
			for (unsigned y = 0; y < 3; y++) {
				for (unsigned x = 0; x < 4; x++) {
					tm_texel(1, 2, x + 1, y + 2, mtx_mem + ((size_t)(y + 4) * 8 + x + 3) * 4);
				}
			}
			mtx_bytes(m, "mtl-blit-level-slice", got, mtx_mem, 64, 4, 8, false);
			[b getBytes:got
			bytesPerRow:16 * 4
		      bytesPerImage:0
			 fromRegion:MTLRegionMake2D(0, 0, 16, 16)
			mipmapLevel:0
			      slice:2];
			tm_fill(mtx_mem, 0, 7, 16, 16);
			for (unsigned y = 0; y < 4; y++) {
				for (unsigned x = 0; x < 5; x++) {
					tm_texel(0, 0, x + 2, y + 3, mtx_mem + ((size_t)(y + 7) * 16 + x + 6) * 4);
				}
			}
			mtx_bytes(m, "mtl-blit-through-buffer", got, mtx_mem, 256, 4, 16, false);
		}
	}
	[a release];
	[b release];
	[buf release];
	/* -generateMipmapsForTexture:, a gradient, each level against the one above as read back */
	static const struct { const char *name; unsigned fmt; bool srgb; } mips[] = {
		{ "mtl-mip-rgba8", 70, false }, { "mtl-mip-bgra8", 80, false }, { "mtl-mip-srgb", 71, true },
	};
	for (unsigned i = 0; i < sizeof(mips) / sizeof(mips[0]); i++) {
		id<MTLTexture> t = mtx_make(m, MTLTextureType2D, mips[i].fmt, 32, 32, 1, 1, 6,
					    MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget, mips[i].name);
		if (t == nil) {
			continue;
		}
		for (unsigned y = 0; y < 32; y++) {
			for (unsigned x = 0; x < 32; x++) {
				uint8_t *p = above + ((size_t)y * 32 + x) * 4;

				p[0] = (uint8_t)(x * 8);
				p[1] = (uint8_t)(y * 8);
				p[2] = (uint8_t)((x + y) * 4);
				p[3] = (uint8_t)(255 - x * 7);
			}
		}
		[t replaceRegion:MTLRegionMake2D(0, 0, 32, 32) mipmapLevel:0 withBytes:above bytesPerRow:32 * 4];
		id<MTLCommandBuffer> cb = [m->queue commandBuffer];
		id<MTLBlitCommandEncoder> enc = [cb blitCommandEncoder];

		[enc generateMipmapsForTexture:t];
		[enc endEncoding];
		if (mpx_finish(m, cb, mips[i].name)) {
			unsigned aw = 32;

			for (unsigned l = 1; l < 6; l++) {
				unsigned lw = aw / 2;
				char name[64];
				int max = 0;

				snprintf(name, sizeof(name), "%s-L%u", mips[i].name, l);
				[t getBytes:lev
				bytesPerRow:lw * 4
				 fromRegion:MTLRegionMake2D(0, 0, lw, lw)
				mipmapLevel:l];
				unsigned bad = tm_box_level(above, aw, aw, lev, lw, lw, mips[i].srgb,
							    mips[i].srgb ? 2 : 1, &max);
				memset(box, 0, sizeof(box));
				printf("drvtest: draw-result %s %s %s digest %016llx expected %016llx\n",
				       m->backend, name, bad ? "DIFFERS" : "ok",
				       (unsigned long long)dm_fnv(lev, (size_t)lw * lw * 4, 0xcbf29ce484222325ull),
				       (unsigned long long)dm_fnv(box, (size_t)lw * lw * 4, 0xcbf29ce484222325ull));
				printf("drvtest: draw-box %s %s largest difference %d steps, %u values past the tolerance\n",
				       m->backend, name, max, bad);
				if (bad) {
					m->wrong++;
				} else {
					m->right++;
				}
				memcpy(above, lev, (size_t)lw * lw * 4);
				aw = lw;
			}
		}
		[t release];
	}
}

/* Which of the four a mode asks for: 0 --draw-attrib, 1 --draw-index, 2 --picture, 3 --blit. */
/* [M5b: and 4 --tex, 5 --texfmt, each with its own form (the head of the section above).] */
static int
pixels_path(id<MTLDevice> dev, const char *metallib, unsigned group)
{
	static const char *const modes[] = { "draw-attrib", "draw-index", "picture", "blit", "tex", "texfmt" };
	struct mpx m;
	NSError *err = nil;

	/* --blit alone leaves the program's default library, which has none of Apple's textured
	 * functions: these four modes all want the GL renderer's unless told otherwise. */
	if (strstr(metallib, "AV1SW") != NULL) {
		metallib = "/System/Library/Extensions/AppleMetalGLRenderer.bundle/default.metallib";
	}
	memset(&m, 0, sizeof(m));
	m.dev = dev;
	m.mode = modes[group];
	m.backend = backend_word();
	m.gallium = !backend_is_vk();
	NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:metallib]];
	NSData *vdata = [NSData dataWithContentsOfFile:
	    [NSString stringWithUTF8String:attrib_vertex_lib]];

	if (data == nil || vdata == nil) {
		printf("drvtest: pixels: cannot read %s\n", data == nil ? metallib : attrib_vertex_lib);
		return 1;
	}
	dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL,
						  DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	dispatch_data_t vd = dispatch_data_create([vdata bytes], [vdata length], NULL,
						  DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	id<MTLLibrary> lib = [dev newLibraryWithData:dd error:&err];
	id<MTLLibrary> vlib = [dev newLibraryWithData:vd error:&err];

	m.vtx2d = [lib newFunctionWithName:@"texturedVertex2D"];
	m.frag2d = [lib newFunctionWithName:@"texturedFragment2DFloat"];
	m.stdv = [vlib newFunctionWithName:@"std_vert0_lpf"];
	if (m.vtx2d == nil || m.frag2d == nil || m.stdv == nil) {
		printf("drvtest: pixels: texturedVertex2D %s, texturedFragment2DFloat %s, std_vert0_lpf %s\n",
		       m.vtx2d != nil ? "found" : "MISSING", m.frag2d != nil ? "found" : "MISSING",
		       m.stdv != nil ? "found" : "MISSING");
		return 1;
	}
	m.queue = [dev newCommandQueue];
	MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];

	[sd setMinFilter:MTLSamplerMinMagFilterNearest];
	[sd setMagFilter:MTLSamplerMinMagFilterNearest];
	[sd setSAddressMode:MTLSamplerAddressModeClampToEdge];
	[sd setTAddressMode:MTLSamplerAddressModeClampToEdge];
	m.samp = [dev newSamplerStateWithDescriptor:sd];
	[sd release];
	m.tsrc = mpx_texture(&m, DM_RGBA32F, dm_src);
	m.tcheck = mpx_texture(&m, DM_RGBA32F, dm_check);
	m.identity = [dev newBufferWithLength:64 options:0];
	float *f = [m.identity contents];

	memset(f, 0, 64);
	for (unsigned k = 0; k < 4; k++) {
		f[k * 4 + k] = 1.0f;
	}
	printf("drvtest: pixels: --%s through Metal on %s, %ux%u\n", m.mode, m.backend, DM_W, DM_H);
	/* M5b: four RGBA32Float attachments and the sampled pipeline those two modes draw through */
	if (group >= 3) {
		static const unsigned f32[4] = { DM_RGBA32F, DM_RGBA32F, DM_RGBA32F, DM_RGBA32F };

		for (unsigned i = 0; i < 4; i++) {
			mtx_t[i] = mpx_texture(&m, DM_RGBA32F, NULL);
		}
		mtx_ps = mpx_pipeline(&m, m.vtx2d, nil, f32, NULL, 0, "texture-pipeline");
	}
	switch (group) {
	case 0: mpx_attrib(&m); break;
	case 1: mpx_index(&m); break;
	case 2: mpx_blend(&m); break;
	case 4: mtx_tex(&m); break;
	case 5: mtx_texfmt(&m); break;
	default: mpx_passes(&m); mtx_blits(&m); break;
	}
	printf("drvtest: draw-summary %s %s %u ok, %u differ, %u gallium-lacks, %u refused\n",
	       m.backend, m.mode, m.right, m.wrong, m.lacks, m.refused);
	if (m.mirror) {
		printf("drvtest: draw-mirror %s %s %u rows are the Gallium library's mirrored 16-bit table\n",
		       m.backend, m.mode, m.mirror);
	}
	fflush(stdout);
	return m.wrong ? 1 : 0;
}

static void
burst_exit_line(void)
{
	const char *both = getenv("AIR_VK_EXIT");
	unsigned pending = 0, in_flight = 0;

	if (backend_is_vk() && both != NULL && sscanf(both, "%u %u", &pending, &in_flight) == 2) {
		printf("drvtest: exit vk pending-pairs %u in-flight %u\n", pending, in_flight);
	} else {
		printf("drvtest: exit %s pending-pairs - in-flight - (%s)\n",
		       backend_is_vk() ? "vk" : "gallium",
		       backend_is_vk() ? "the Vulkan library was not used, or is too old to say"
				       : "Gallium's fences are not counted here");
	}
	fflush(stdout);
}

int
main(int argc, char **argv)
{
	struct sigaction sa = { 0 };

	/* First, before anything: --time reports the distance from here to the first drawn
	 * pixel, which is what a launching application waits through. */
	tm_main_entered = tm_now_ns();
	atexit(burst_exit_line);
	sa.sa_flags = SA_SIGINFO;
	sa.sa_sigaction = fault;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
	const char *path = "/hackios/lib/libhackiosmetal.dylib";
	const char *metallib = "/System/Library/VideoDecoders/AV1SW.metallib";
	const char *only = NULL;
	bool as_bundle = false;
	bool draw = false;
	bool picture = false;
	int picture_seconds = 60;
	bool timing = false;
	bool completion_only = false;
	unsigned timing_repeats = 32;
	bool frames = false;
	unsigned frames_count = 200;
	const char *frames_target = "owned";
	unsigned burst_count = 0;
	bool order_only = false;
	const char *m5c_library = NULL;   /* --m5c LIB (tools/air/m5c_rows.h) */
	bool ca_fill_only = false;
	int next = 1;

	/*
	 * --backend vk|gallium, FIRST and before anything makes a device: which of the two
	 * implementations of the compiler library's contract the driver loads (hkmetal.m,
	 * mesa_available), through HK_METAL_BACKEND, which wins over the boot-arg
	 * metal_backend=vk. A job line has no environment of its own to set, so this is how
	 * one boot runs the same test once per back end and compares the digest each prints.
	 */
	if (argc > next + 1 && strcmp(argv[next], "--backend") == 0) {
		if (strcmp(argv[next + 1], "vk") != 0 && strcmp(argv[next + 1], "gallium") != 0) {
			printf("drvtest: --backend takes vk or gallium, not %s\n", argv[next + 1]);
			return 2;
		}
		setenv("HK_METAL_BACKEND", argv[next + 1], 1);
		printf("drvtest: HK_METAL_BACKEND=%s\n", argv[next + 1]);
		next += 2;
	}
	/*
	 * --quiet and --vk-noseen (M4), lifted out wherever they stand, as --gallium-depth-stores
	 * is below, and before anything makes a device: --vk-noseen is AIR_VK_NOSEEN=1, which
	 * air_vk.c reads once at rwa_device_open (an imported surface is then not mapped once,
	 * so the kernel does not clean it at every submission); --quiet makes --draw-all and
	 * --compute-all timed workloads whose output is held and digested (WHAT A WORKLOAD
	 * COSTS, near the top).
	 */
	for (int i = next; i < argc; ) {
		bool quiet = strcmp(argv[i], "--quiet") == 0;

		if (!quiet && strcmp(argv[i], "--vk-noseen") != 0) {
			i++;
			continue;
		}
		if (quiet) {
			cost_quiet = true;
		} else {
			setenv("AIR_VK_NOSEEN", "1", 1);
			printf("drvtest: AIR_VK_NOSEEN=1 for this run\n");
		}
		for (int j = i; j + 1 < argc; j++) {
			argv[j] = argv[j + 1];
		}
		argc -= 1;
	}

	/*
	 * --sweep: ask Metal for the system's device and count what every class of the driver
	 * owes, then stop. Nothing is dlopen'd and MTLAddDevice is not called, so this is the
	 * road iOS's own daemons take -- which makes it the one form of this program that can
	 * be added to a boot with metal=1 and launchd alive (rc.jobs.metalprops) to put the
	 * numbers in that log beside whatever the daemons then do.
	 */
	if (argc > next && strcmp(argv[next], "--sweep") == 0) {
		id<MTLDevice> found = MTLCreateSystemDefaultDevice();

		printf("drvtest: sweep: MTLCreateSystemDefaultDevice() = %s\n",
		       found != nil ? class_getName(object_getClass(found)) : "nil");
		if (found == nil) {
			printf("drvtest: sweep: without a device from Metal the driver's "
			       "classes are not loaded; boot with metal=1 and run "
			       "metal_props on first\n");
			return 1;
		}
		sweep_every_class();
		return 0;
	}
	if (argc > next && strcmp(argv[next], "--bundle") == 0) {
		as_bundle = true;
		next++;
	}
	if (argc > next && strcmp(argv[next], "--blit") == 0) {
		blit_only = true;
		next++;
	}
	/* M5a: --pixels after any of --blit, --draw-index, --draw-attrib or --picture is that
	 * mode's second form (pixels_path): lifted out of the list wherever it stands, at the
	 * end of the flags below, for --vertexlib's reason. */
	for (int i = next; i < argc; i++) {
		if (strcmp(argv[i], "--pixels") == 0) {
			pixels_only = true;
			for (int j = i; j + 1 < argc; j++) {
				argv[j] = argv[j + 1];
			}
			argc--;
			break;
		}
	}
	/* [M5b: --tex and --texfmt with --pixels are a mode and take no kind: pixels_path 4 and 5,
	 * and no sweep's meaning of either changes without it.] */
	if (pixels_only && argc > next && strcmp(argv[next], "--tex") == 0) {
		tex_pixels = 4;
		next++;
	} else if (pixels_only && argc > next && strcmp(argv[next], "--texfmt") == 0) {
		tex_pixels = 5;
		next++;
	}
	if (argc > next && strcmp(argv[next], "--completion") == 0) {
		completion_only = true;
		next++;
	}
	if (argc > next && strcmp(argv[next], "--stop-compositor") == 0) {
		pic_stop_compositor = true;
		next++;
	}
	/* --ca-fill: CoreAnimation's solid fill, by its own library and constants */
	if (argc > next && strcmp(argv[next], "--ca-fill") == 0) {
		ca_fill_only = true;
		next++;
	}
	/* --m5c LIB: argument buffers, arrays of descriptors, a draw's stores, function constants */
	if (argc > next + 1 && strcmp(argv[next], "--m5c") == 0) {
		m5c_library = argv[next + 1];
		next += 2;
	}
	/* --order [METALLIB] (M4-1): -replaceRegion: and a destroy, in order (order_test) */
	if (argc > next && strcmp(argv[next], "--order") == 0) {
		order_only = true;
		metallib = "/System/Library/Extensions/AppleMetalGLRenderer.bundle/default.metallib";
		next++;
	}
	/* --burst [N] (M4-1): N empty and N small command buffers nobody waits for (burst_test) */
	if (argc > next && strcmp(argv[next], "--burst") == 0) {
		burst_count = 256;
		next++;
		if (argc > next && argv[next][0] >= '0' && argv[next][0] <= '9') {
			burst_count = (unsigned)strtoul(argv[next], NULL, 10);
			next++;
		}
		if (burst_count == 0) {
			printf("drvtest: --burst 0 is nothing to try\n");
			return 2;
		}
	}
	if (argc > next && strcmp(argv[next], "--draw") == 0) {
		draw = true;
		next++;
	}
	/* --draw-index implies --draw: the indexed steps are compared with the ordinary
	 * draw's own picture, so that draw has to happen first and in the same process. */
	if (argc > next && strcmp(argv[next], "--draw-index") == 0) {
		draw = true;
		draw_index = true;
		next++;
	}
	/* --draw-attrib does NOT imply --draw: it builds its own pipelines, with a vertex
	 * descriptor, and the picture it compares everything with is its own first step. */
	if (argc > next && strcmp(argv[next], "--draw-attrib") == 0) {
		draw_attrib = true;
		next++;
	}
	/* --argbuf: MTLArgumentEncoder, and the buffer offset that goes with it. Its own
	 * flag and not an option of --draw because two of its three measurements draw
	 * nothing at all (an encoder from a function, and one from descriptors), and the
	 * third draws the same shader twice -- so what it prints is a comparison and not a
	 * picture. argbuf_path says what each of the three is worth. */
	if (argc > next && strcmp(argv[next], "--argbuf") == 0) {
		argbuf = true;
		next++;
	}
	/*
	 * --vertexlib and --gated are taken out of the argument list wherever they stand,
	 * before anything else is read, because a flag's meaning must not depend on its
	 * neighbours. The job list writes
	 *     --vertexlib L --vertex V --gated G --fragment F
	 * and a loop that accepted only a contiguous run of these two stopped at --vertex
	 * and left --gated for nobody; the device reported "cannot read --gated" after a
	 * boot that had already spent its twenty minutes. Lifting them out here keeps the
	 * --vertex/--fragment pair loop below exactly as it was.
	 */
	for (int i = next; i + 1 < argc; ) {
		bool lib = strcmp(argv[i], "--vertexlib") == 0;

		if (!lib && strcmp(argv[i], "--gated") != 0) {
			i++;
			continue;
		}
		if (lib) {
			attrib_vertex_lib = argv[i + 1];
		} else {
			/* "none" leaves the gate unmeasured and says so, for a library that
			 * has no module of that shape. */
			attrib_gated_function = strcmp(argv[i + 1], "none") == 0
			    ? NULL : argv[i + 1];
		}
		for (int j = i; j + 2 < argc; j++) {
			argv[j] = argv[j + 2];
		}
		argc -= 2;
	}
	/* --rtfmt NAME is lifted out the same way and for the same reason: --draw's attachment
	 * format (draw_target_format says what it is for), by name as --texfmt takes one. */
	for (int i = next; i + 1 < argc; ) {
		if (strcmp(argv[i], "--rtfmt") != 0) {
			i++;
			continue;
		}
		MTLPixelFormat f = format_named(argv[i + 1]);

		if (f == 0 || f == MTLPixelFormatDepth16Unorm || f == MTLPixelFormatDepth32Float ||
		    f == MTLPixelFormatDepth32Float_Stencil8 || f == MTLPixelFormatStencil8) {
			printf("drvtest: --rtfmt %s is not a colour format this test names\n",
			       argv[i + 1]);
			return 2;
		}
		draw_target_format = f;
		for (int j = i; j + 2 < argc; j++) {
			argv[j] = argv[j + 2];
		}
		argc -= 2;
	}
	/* --clear VALUE, lifted out the same way: --draw's clear colour (draw_clear says why) */
	for (int i = next; i + 1 < argc; ) {
		if (strcmp(argv[i], "--clear") != 0) {
			i++;
			continue;
		}
		draw_clear = strtod(argv[i + 1], NULL);
		for (int j = i; j + 2 < argc; j++) {
			argv[j] = argv[j + 2];
		}
		argc -= 2;
	}
	/* --gallium-depth-stores, lifted out the same way and taking no value (the K28 guard of
	 * draw_every says what it is for) */
	for (int i = next; i < argc; ) {
		if (strcmp(argv[i], "--gallium-depth-stores") != 0) {
			i++;
			continue;
		}
		gallium_depth_stores = true;
		for (int j = i; j + 1 < argc; j++) {
			argv[j] = argv[j + 1];
		}
		argc -= 1;
	}
	/* --picture [SECONDS]: the draw at the size of the panel, put on the panel. Its own
	 * flag and not a size for --draw, because what it produces is not a pixel to compare
	 * but a whole screen, and because it ends in a loop that repaints: a run is over when
	 * XL_SECS says so, and the screenshot is taken while this is still painting. */
	if (argc > next && strcmp(argv[next], "--picture") == 0) {
		picture = true;
		next++;
		if (argc > next && argv[next][0] >= '0' && argv[next][0] <= '9') {
			picture_seconds = (int)strtol(argv[next], NULL, 10);
			next++;
		}
	}
	/*
	 * --time [REPEATS]: what the driver costs rather than whether it is right. Its own
	 * flag and not a size for --draw, for the same reason --picture has one: what it
	 * produces is not a pixel to compare but a time, and a time has to be measured at
	 * the panel's real size over and over before one number of it means anything.
	 * REPEATS defaults to 32, which is enough that one outlier cannot be the
	 * measurement and small enough that four sizes of draw still fit in a boot.
	 */
	if (argc > next && strcmp(argv[next], "--time") == 0) {
		timing = true;
		next++;
		if (argc > next && argv[next][0] >= '0' && argv[next][0] <= '9') {
			timing_repeats = (unsigned)strtoul(argv[next], NULL, 10);
			next++;
		}
	}
	/* --frames N owned|surface (M4, W4): N frames of one full-screen draw (frames_path) */
	if (argc > next + 2 && strcmp(argv[next], "--frames") == 0) {
		frames = true;
		frames_count = (unsigned)strtoul(argv[next + 1], NULL, 10);
		frames_target = argv[next + 2];
		next += 3;
	}
	/* --draw-depth is the draw with a depth attachment under it, which no pass in this
	 * port has ever had. Its own flag because what it measures is not a pixel but
	 * whether a draw was discarded, and that takes four draws of one quad (depth_path
	 * says why each). An optional format name follows, by name and not by number for
	 * --texfmt's reason. */
	if (argc > next && strcmp(argv[next], "--draw-depth") == 0) {
		draw_depth = true;
		next++;
		if (argc > next && strncmp(argv[next], "--", 2) != 0 &&
		    strchr(argv[next], '/') == NULL) {
			MTLPixelFormat f = format_named(argv[next]);

			if (f == 0) {
				printf("drvtest: %s is not a pixel format this test names\n",
				       argv[next]);
				return 2;
			}
			depth_attachment_format = f;
			next++;
		}
	}
	/* --draw-msaa [N] (M3c-3): its own flag for --draw-depth's reason -- what it measures is
	 * a resolve, over two passes (msaa_path says why each) -- with the sample count after
	 * it; --fragment, --rtfmt and --clear choose the shader, the format and the clear. */
	if (argc > next && strcmp(argv[next], "--draw-msaa") == 0) {
		draw_msaa = true;
		next++;
		if (argc > next && argv[next][0] >= '0' && argv[next][0] <= '9') {
			msaa_samples = (unsigned)strtoul(argv[next], NULL, 10);
			next++;
		}
	}
	if (argc > next && strcmp(argv[next], "--draw-all") == 0) {
		draw = true;
		draw_all = true;
		next++;
		if (argc > next + 1 && argv[next][0] >= '0' && argv[next][0] <= '9') {
			draw_from = (unsigned)strtoul(argv[next], NULL, 10);
			draw_count = (unsigned)strtoul(argv[next + 1], NULL, 10);
			next += 2;
		}
	}
	/* --draw-one NAME (M3c-3): --draw-all over the one fragment function of that name */
	if (argc > next + 1 && strcmp(argv[next], "--draw-one") == 0) {
		draw = true;
		draw_all = true;
		draw_one = argv[next + 1];
		next += 2;
	}
	/* --compute-all [FROM COUNT] [--compute-hangers] (M3d): every kernel of the library
	 * dispatched, a compute-result line each (compute_every) */
	if (argc > next && strcmp(argv[next], "--compute-all") == 0) {
		compute_all = true;
		next++;
		if (argc > next + 1 && argv[next][0] >= '0' && argv[next][0] <= '9') {
			compute_from = (unsigned)strtoul(argv[next], NULL, 10);
			compute_count = (unsigned)strtoul(argv[next + 1], NULL, 10);
			next += 2;
		}
		if (argc > next && strcmp(argv[next], "--compute-hangers") == 0) {
			compute_hangers_too = true;
			next++;
		}
	}
	while (argc > next && (strcmp(argv[next], "--seed-rows") == 0 ||
			       strcmp(argv[next], "--no-texdecl") == 0)) {
		if (strcmp(argv[next], "--seed-rows") == 0) {
			seed_rows = true;
		} else {
			use_decl = false;
		}
		next++;
	}
	while (argc > next + 1 && (strcmp(argv[next], "--vertex") == 0 ||
				   strcmp(argv[next], "--fragment") == 0)) {
		if (strcmp(argv[next], "--vertex") == 0) {
			vertex_function = argv[next + 1];
		} else {
			fragment_function = argv[next + 1];
		}
		next += 2;
	}
	while (argc > next + 1 && strncmp(argv[next], "--tex", 5) == 0) {
		if (strcmp(argv[next], "--tex") == 0) {
			const char *kind = argv[next + 1];

			texture_kind = strcmp(kind, "3d") == 0 ? MTLTextureType3D :
			    strcmp(kind, "cube") == 0 ? MTLTextureTypeCube :
			    strcmp(kind, "1d") == 0 ? MTLTextureType1D :
			    strcmp(kind, "2darray") == 0 ? MTLTextureType2DArray :
			    MTLTextureType2D;
		} else if (strcmp(argv[next], "--texslots") == 0) {
			parse_slots(argv[next + 1]);
		} else if (strcmp(argv[next], "--texfmt") == 0) {
			/* By name, mapped with the SDK's own constants: a pixel format written
			 * as a number here would be a number copied out of a header by hand,
			 * and the one thing worse than a wrong format is a wrong format that
			 * looks deliberate. */
			MTLPixelFormat f = format_named(argv[next + 1]);

			if (f != 0) {
				texture_format = f;
			}
		}
		next += 2;
	}
	if (argc > next) {
		metallib = argv[next++];
	}
	if (argc > next) {
		only = argv[next++];
	}

	/*
	 * THE TWO ROADS, and taking the wrong one is an abort rather than a mistake.
	 *
	 * MTLCreateSystemDefaultDevice is a dispatch_sync of a block whose whole body is a
	 * count of Metal's device array: 0 leaves nil, 1 returns that one, and ANYTHING ELSE
	 * is MTLReportFailure("doesn't know what to do with more than one device") and
	 * abort(). MTLAddDevice appends to that same array. So in a boot where the registry
	 * switch is on, Metal's own discovery has already registered our device, and a
	 * program that then adds its own makes two and dies on the next probe -- which is
	 * exactly what happened to triangle_app in the boot of 22 October 05:50, the only
	 * process in the whole system that aborted (docs/metal.md 10.47).
	 *
	 * Apple's daemons never meet this because none of them calls MTLAddDevice; neither
	 * does a game. It is a hazard of our own test programs alone, and the fix is an
	 * order: ask first, and build a device only when Metal has none.
	 */
	id<MTLDevice> found = MTLCreateSystemDefaultDevice();

	printf("drvtest: before anything, MTLCreateSystemDefaultDevice() = %s\n",
	       found != nil ? class_getName(object_getClass(found)) : "nil");
	if (found != nil) {
		printf("drvtest: Metal's own discovery already has a device, so this process "
		       "adds NOTHING: a second one would abort the next probe\n");
	}

	Class cls = Nil;

	if (found == nil && as_bundle) {
		/* Only one of the two images may be loaded: both define the same classes, and
		 * the Objective-C runtime would keep one of them and not say which. */
		cls = class_from_bundle("/hackios/lib/HKMetal.bundle");
		if (cls == Nil) {
			return 1;
		}
	} else if (found == nil) {
		void *lib = dlopen(path, RTLD_NOW);

		if (lib == NULL) {
			printf("drvtest: dlopen(%s) failed: %s\n", path, dlerror());
			printf("drvtest: if that is the class symbol, the device class has to be "
			       "built at runtime after all\n");
			return 1;
		}
		printf("drvtest: %s loaded, so _OBJC_CLASS_$__MTLDevice binds\n", path);
		cls = objc_getClass("HKMetalDevice");
	}
	Class base = objc_getClass("_MTLDevice");
	if (found != nil) {
		goto have_device;           /* steps 4 and 5 are Metal's own work in this boot */
	}
	if (cls == Nil || base == Nil) {
		printf("drvtest: HKMetalDevice %s, _MTLDevice %s\n",
		       cls != Nil ? "is here" : "is missing", base != Nil ? "is here" : "is missing");
		return 1;
	}
	Protocol *spi = objc_getProtocol("MTLDeviceSPI");
	printf("drvtest: step 4 isSubclassOfClass:_MTLDevice %s; conforms to MTLDeviceSPI %s, "
	       "to MTLDevice %s\n",
	       [cls isSubclassOfClass:base] ? "PASSES" : "fails",
	       spi != NULL && [cls conformsToProtocol:spi] ? "yes" : "no",
	       [cls conformsToProtocol:@protocol(MTLDevice)] ? "yes" : "no");

	io_service_t svc = IOServiceGetMatchingService(0, IOServiceMatching("IOAcceleratorES"));
	if (svc == IO_OBJECT_NULL) {
		printf("drvtest: no IOAcceleratorES service to hand it\n");
		return 1;
	}
	id dev = ((id (*)(id, SEL, io_service_t))objc_msgSend)(
	    [cls alloc], sel_getUid("initWithAcceleratorPort:"), svc);
	printf("drvtest: step 5 gave %s\n",
	       dev != nil ? class_getName(object_getClass(dev)) : "nil");
	if (dev == nil) {
		return 1;
	}

	uintptr_t slide = (uintptr_t)&MTLCreateSystemDefaultDevice - MTL_CREATE_DEVICE_UNSLID;
	void (*add_device)(id) = (void (*)(id))(slide + MTL_ADD_DEVICE_UNSLID);

	@try {
		add_device(dev);
		printf("drvtest: MTLAddDevice returned\n");
	} @catch (NSException *e) {
		printf("drvtest: MTLAddDevice threw %s: %s\n", [[e name] UTF8String],
		       [[e reason] UTF8String]);
		return 1;
	}

have_device:
	;
	id<MTLDevice> sys = found != nil ? found : MTLCreateSystemDefaultDevice();
	printf("drvtest: MTLCreateSystemDefaultDevice() = %s\n",
	       sys != nil ? class_getName(object_getClass(sys)) : "nil");
	if (sys == nil) {
		return 1;
	}
	printf("drvtest: it calls itself \"%s\"\n", [[sys name] UTF8String]);
	if (tex_pixels != 0) {
		return pixels_path(sys, metallib, tex_pixels);
	}
	if (blit_only && pixels_only) {
		return pixels_path(sys, metallib, 3);
	}
	if (draw_index && pixels_only) {
		return pixels_path(sys, metallib, 1);
	}
	if (blit_only) {
		return blit_test(sys);
	}
	if (completion_only) {
		return completion_test(sys);
	}
	if (burst_count != 0) {
		return burst_test(sys, burst_count);
	}
	if (order_only) {
		return order_test(sys, metallib);
	}
	if (m5c_library != NULL) {
		return m5c_run(sys, m5c_library, backend_word(), !backend_is_vk());
	}
	if (ca_fill_only) {
		return ca_fill(sys);
	}

	/* What the device still owes, counted by the runtime on the device rather than guessed
	 * from the cache: every method the two protocols require, asked of the real object. An
	 * unanswered one is not a gap in a table, it is an application aborting on
	 * "unrecognized selector", because the gates inside Metal are assertions. */
	for (int which = 0; which < 2; which++) {
		const char *pname = which == 0 ? "MTLDevice" : "MTLDeviceSPI";
		Protocol *proto = objc_getProtocol(pname);
		unsigned n = 0, missing = 0, shown = 0;

		if (proto == NULL) {
			continue;
		}
		struct objc_method_description *req =
		    protocol_copyMethodDescriptionList(proto, YES, YES, &n);

		for (unsigned i = 0; i < n; i++) {
			if ([sys respondsToSelector:req[i].name]) {
				continue;
			}
			missing++;
			if (shown < 8) {
				printf("drvtest:   %s still owes %s\n", pname,
				       sel_getName(req[i].name));
				shown++;
			}
		}
		free(req);
		printf("drvtest: of %u methods %s requires, %u are unanswered (unanswered means "
		       "absent from the chain; an entry whose body refuses is not counted here)\n",
		       n, pname, missing);
	}

	/* and the same count for every other class of the driver, which is where the two
	 * methods the daemons named were missing from */
	sweep_every_class();

	/* The first resource. A buffer here is a GEM allocation of the kernel's own gpu
	 * driver, so this checks the whole path: the kernel gave memory, the GPU has an
	 * address for it, this processor can write it and read back what it wrote. */
	id<MTLBuffer> buf = [sys newBufferWithLength:4096 options:MTLResourceStorageModeShared];

	printf("drvtest: newBufferWithLength:options: gave %s\n",
	       buf != nil ? class_getName(object_getClass(buf)) : "nil");
	if (buf != nil) {
		unsigned char *p = [buf contents];

		printf("drvtest:   length %lu, contents %p, gpu address %s\n",
		       (unsigned long)[buf length], p,
		       [buf gpuAddress] != 0 ?
		       [[NSString stringWithFormat:@"%#llx",
			 (unsigned long long)[buf gpuAddress]] UTF8String] :
		       "not exposed (this one came from the compiler library)");
		if (p != NULL) {
			int same = 1;

			for (int i = 0; i < 4096; i++) {
				p[i] = (unsigned char)(i * 7 + 13);
			}
			for (int i = 0; i < 4096; i++) {
				if (p[i] != (unsigned char)(i * 7 + 13)) {
					same = 0;
					break;
				}
			}
			printf("drvtest:   wrote 4096 bytes and read back %s\n",
			       same ? "the same bytes" : "SOMETHING ELSE");
		}
		/* and what such a buffer still owes, asked of the runtime rather than guessed */
		unsigned n = 0;
		Protocol *bp = objc_getProtocol("MTLBuffer");
		struct objc_method_description *req =
		    bp != NULL ? protocol_copyMethodDescriptionList(bp, YES, YES, &n) : NULL;
		unsigned missing = 0;

		for (unsigned i = 0; i < n; i++) {
			if (![buf respondsToSelector:req[i].name]) {
				if (missing < 12) {
					printf("drvtest:   still owed: %s\n",
					       sel_getName(req[i].name));
				}
				missing++;
			}
		}
		free(req);
		printf("drvtest:   of %u methods MTLBuffer requires, %u are unanswered\n",
		       n, missing);
	}

	/* And what Metal can do with it: the container path needs no driver code at all, which
	 * is where subsystem (1) plugs in one level further down. */
	NSData *data = [NSData dataWithContentsOfFile:
	    [NSString stringWithUTF8String:metallib]];
	if (data == nil) {
		printf("drvtest: cannot read %s\n", metallib);
		return 0;
	}
	printf("drvtest: %s, %lu bytes\n", metallib, (unsigned long)[data length]);
	dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL,
						  DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	NSError *err = nil;
	id<MTLLibrary> mtllib = [sys newLibraryWithData:dd error:&err];

	printf("drvtest: newLibraryWithData: gave %s with %lu functions\n",
	       mtllib != nil ? class_getName(object_getClass(mtllib)) : "nil",
	       mtllib != nil ? (unsigned long)[[mtllib functionNames] count] : 0UL);
	if (mtllib == nil) {
		return 0;
	}
	id<MTLFunction> fn = [mtllib newFunctionWithName:[[mtllib functionNames] firstObject]];

	printf("drvtest: newFunctionWithName: gave %s\n",
	       fn != nil ? class_getName(object_getClass(fn)) : "nil");

	if (timing) {
		time_path(sys, metallib, timing_repeats);
		return 0;
	}
	if (frames) {
		frames_path(sys, metallib, frames_count, frames_target);
		return 0;
	}
	if (picture && pixels_only) {
		return pixels_path(sys, metallib, 2);
	}
	if (picture) {
		picture_path(sys, metallib, picture_seconds);
		return 0;
	}
	if (draw_depth) {
		depth_path(sys, metallib);
		return 0;
	}
	if (draw_msaa) {
		/* in a pool of its own, for --draw's reason: the command buffers -commandBuffer
		 * autoreleased give the Vulkan back end's fences back before the last count */
		@autoreleasepool {
			msaa_path(sys, metallib);
		}
		print_vk_counts("after the msaa passes and their releases");
		return 0;
	}
	if (draw_attrib && pixels_only) {
		return pixels_path(sys, metallib, 0);
	}
	if (draw_attrib) {
		attrib_path(sys, metallib);
		return 0;
	}
	if (argbuf) {
		/* the compiler library's own record count, by the same loader the texture
		 * query uses: one dlopen of the image the driver already holds */
		load_texture_query(sys);
		argbuf_path(sys, metallib);
		return 0;
	}
	if (draw) {
		/* in a pool of its own, so the command buffer -commandBuffer autoreleased is
		 * gone by the second count, and with it the fence the Vulkan back end lent it */
		@autoreleasepool {
			draw_path(sys, metallib);
		}
		print_vk_counts("after the draw and its releases (the first step's 4096-byte "
				"buffer above is still held)");
		return 0;
	}
	if (compute_all) {
		/* --quiet (M4, W3): the sweep is a timed workload with a cost line */
		char libname[96];

		if (cost_quiet) {
			cost_begin("W3");
		}
		compute_every(sys, mtllib);
		if (cost_quiet) {
			cost_end("dispatches", "dispatch", "compute-all %u kernels %s",
				 compute_every_dispatched,
				 cost_library_name(metallib, libname, sizeof(libname)));
		}
		return 0;
	}
	if (only == NULL) {
		air_of_each_function(mtllib);
	}
	compute_path(sys, mtllib, only);
	return 0;
}

/*
 * --ca-fill: CoreAnimation's own solid fill, off its own library with its own constants and vertex
 * layout (hkmetal's `spec` and `layout` lines): fixed_vert_lph_spc and fixed_frag_lph_cph, which in
 * run 134123 wrote a different colour every frame into a target whose vertices say opaque black.
 * It says what comes out for a colour that should come out.
 */
static int
ca_fill(id<MTLDevice> dev)
{
   NSError *e = nil;
   id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@"/System/Library/Frameworks/QuartzCore.framework/default.metallib"]
                                         error:&e];
   if (lib == nil) {
      printf("drvtest: ca-fill: no library: %s\n", e ? [[e localizedDescription] UTF8String] : "?");
      return 1;
   }
   static const struct { const char *name; int count; int idx[8]; int val[8]; int attachments; int vs52; } cases[] = {
      { "fill-A2Xhf", 3, { 4, 6, 30 }, { 1, 2, 0x36 }, 2, 0 },
      { "fill-A2Xhf-one-target", 3, { 4, 6, 30 }, { 1, 2, 0x36 }, 1, 0 },
      { "fill-no-fetch", 2, { 6, 30 }, { 1, 0x36 }, 1, 0 },
      { "fill-vertex-has-texcoord", 3, { 4, 6, 30 }, { 1, 2, 0x36 }, 1, 1 },
   };
   id<MTLCommandQueue> q = [dev newCommandQueue];
   for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
      MTLFunctionConstantValues *fv = [[MTLFunctionConstantValues alloc] init];
      MTLFunctionConstantValues *vv = [[MTLFunctionConstantValues alloc] init];
      for (int k = 0; k < cases[c].count; k++) {
         unsigned char b = (unsigned char)cases[c].val[k];
         [fv setConstantValue:&b type:(cases[c].idx[k] == 4 ? MTLDataTypeBool : MTLDataTypeUChar) atIndex:(NSUInteger)cases[c].idx[k]];
      }
      if (cases[c].vs52) {
         unsigned char one = 1;
         [vv setConstantValue:&one type:MTLDataTypeUChar atIndex:52];
      }
      id<MTLFunction> ff = [lib newFunctionWithName:@"fixed_frag_lph_cph" constantValues:fv error:&e];
      id<MTLFunction> vf = [lib newFunctionWithName:@"fixed_vert_lph_spc" constantValues:vv error:&e];
      MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
      [[vd attributes][0] setFormat:MTLVertexFormatFloat4];
      [[vd attributes][0] setOffset:0];
      [[vd attributes][0] setBufferIndex:1];
      [[vd attributes][1] setFormat:MTLVertexFormatFloat2];
      [[vd attributes][1] setOffset:16];
      [[vd attributes][1] setBufferIndex:1];
      [[vd attributes][2] setFormat:MTLVertexFormatFloat2];
      [[vd attributes][2] setOffset:24];
      [[vd attributes][2] setBufferIndex:1];
      [[vd attributes][3] setFormat:MTLVertexFormatHalf4];
      [[vd attributes][3] setOffset:32];
      [[vd attributes][3] setBufferIndex:1];
      [[vd layouts][1] setStride:48];
      MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
      [d setVertexFunction:vf];
      [d setFragmentFunction:ff];
      [d setVertexDescriptor:vd];
      [[d colorAttachments][0] setPixelFormat:MTLPixelFormatBGRA8Unorm];
      if (cases[c].attachments > 1) {
         [[d colorAttachments][1] setPixelFormat:MTLPixelFormatBGRA8Unorm];
         [[d colorAttachments][1] setWriteMask:MTLColorWriteMaskNone];
      }
      id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:d error:&e];
      if (p == nil) {
         printf("drvtest: ca-fill %s: no pipeline: %s\n", cases[c].name, e ? [[e localizedDescription] UTF8String] : "?");
         continue;
      }
      MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:16 height:16 mipmapped:NO];
      [td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];
      id<MTLTexture> t = [dev newTextureWithDescriptor:td];
      id<MTLTexture> t1 = [dev newTextureWithDescriptor:td];
      struct vtx { float pos[4]; float tc[4]; uint16_t color[4]; uint32_t junk[2]; };
      static const float corner[6][2] = { {-1,-1}, {1,-1}, {-1,1}, {1,-1}, {1,1}, {-1,1} };
      struct vtx verts[6];
      for (int v = 0; v < 6; v++) {
         memset(&verts[v], 0, sizeof(verts[v]));
         verts[v].pos[0] = corner[v][0];
         verts[v].pos[1] = corner[v][1];
         verts[v].pos[3] = 1;
         verts[v].color[0] = 0x3400;   /* 0.25 */
         verts[v].color[1] = 0x3800;   /* 0.5 */
         verts[v].color[2] = 0x3a00;   /* 0.75 */
         verts[v].color[3] = 0x3c00;   /* 1 */
         verts[v].junk[0] = 0x33f1c6d8;
         verts[v].junk[1] = 1;
      }
      const float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
      id<MTLBuffer> vb = [dev newBufferWithBytes:verts length:sizeof(verts) options:MTLResourceStorageModeShared];
      id<MTLBuffer> mb = [dev newBufferWithBytes:identity length:sizeof(identity) options:MTLResourceStorageModeShared];
      MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
      [[rp colorAttachments][0] setTexture:t];
      [[rp colorAttachments][0] setLoadAction:MTLLoadActionClear];
      [[rp colorAttachments][0] setStoreAction:MTLStoreActionStore];
      [[rp colorAttachments][0] setClearColor:MTLClearColorMake(1, 0, 1, 1)];
      if (cases[c].attachments > 1) {
         [[rp colorAttachments][1] setTexture:t1];
         [[rp colorAttachments][1] setLoadAction:MTLLoadActionClear];
         [[rp colorAttachments][1] setStoreAction:MTLStoreActionDontCare];
      }
      id<MTLCommandBuffer> cb = [q commandBuffer];
      id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
      [enc setRenderPipelineState:p];
      [enc setVertexBuffer:vb offset:0 atIndex:1];
      [enc setVertexBuffer:mb offset:0 atIndex:2];
      [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
      [enc endEncoding];
      [cb commit];
      [cb waitUntilCompleted];
      uint8_t px[4] = { 0 };
      [t getBytes:px bytesPerRow:64 fromRegion:MTLRegionMake2D(8, 8, 1, 1) mipmapLevel:0];
      printf("drvtest: ca-fill %s: status %ld b g r a %u %u %u %u (want 191 128 64 255)\n", cases[c].name, (long)[cb status], px[0], px[1], px[2], px[3]);
   }
   return 0;
}

