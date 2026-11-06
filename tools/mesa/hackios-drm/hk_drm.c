/*
 * The libdrm of xf86drm.h (see there): Linux's DRM requests to the kernel's GPU driver as calls
 * of its IOKit user client. A request's number (the low byte of the ioctl code) is the call's
 * selector, its structure goes in and comes back as it is; a buffer's memory is the client
 * memory of the type that is the buffer's handle.
 *
 * freedreno knows its device by a file descriptor. It gets one that means nothing (/dev/null):
 * there is one GPU and one connection to it in a process.
 *
 * turnip (Mesa's Vulkan driver; Metal over Vulkan, TODO T38 (12)) is built against this too. It
 * finds its device through drmGetDevices2, which lists the one GPU with /dev/null as its node, and
 * opens the connection on its first request; and it wants sync objects, which the kernel's driver
 * does not have. They are kept here instead (the part "sync objects" below): built with
 * HK_DRM_SYNCOBJ (turnip's copy) DRM_CAP_SYNCOBJ says 1. gallium's copy says 0, as before, so its
 * fences stay what they were.
 */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <IOKit/IOKitLib.h>

#include "xf86drm.h"
#include "drm-uapi/msm_drm.h"

#define HK_GPU_SERVICE     "HKGPU"
#define HK_GPU_SURFACE     0x7e            /* the request that is not Linux's */
#define HK_GPU_DESCRIBE    0xffffffffu     /* ... asked only what the buffer would be like */

static io_connect_t connection;
static int connection_fd = -1;
static pthread_mutex_t connection_lock = PTHREAD_MUTEX_INITIALIZER;

/* the one connection of this process to the GPU's driver, made the first time it is needed */
static int
connected(void)
{
	io_service_t service;

	pthread_mutex_lock(&connection_lock);
	if (connection == 0) {
		service = IOServiceGetMatchingService(0, IOServiceMatching(HK_GPU_SERVICE));
		if (service != 0) {
			if (IOServiceOpen(service, mach_task_self(), 0, &connection) != 0) {
				connection = 0;
			}
			IOObjectRelease(service);
		}
		if (connection != 0 && connection_fd < 0) {
			connection_fd = open("/dev/null", O_RDWR);
		}
	}
	pthread_mutex_unlock(&connection_lock);
	return connection != 0;
}

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	(void)busid;
	(void)type;
	if (strcmp(name, "msm") != 0 || !connected()) {
		return -1;
	}
	return dup(connection_fd);
}

static int
call(unsigned number, void *data, size_t size)
{
	size_t returned = size;
	kern_return_t result = connected() ? IOConnectCallStructMethod(connection, number, data, size, data, &returned) :
	    kIOReturnNotFound;

	switch (result) {
	case 0:
		return 0;
	case kIOReturnNoMemory:
		return ENOMEM;
	case kIOReturnBadArgument:
		return EINVAL;
	case kIOReturnTimeout:
		return ETIMEDOUT;
	case kIOReturnBusy:
		return EBUSY;
	case kIOReturnNotFound:
		return ENOENT;
	case kIOReturnUnsupported: {
		/* a request the driver does not have: said once a request, the caller gets ENOSYS and goes on */
		static uint8_t told[256];

		if (!told[number & 0xff]) {
			told[number & 0xff] = 1;
			fprintf(stderr, "hk_drm: request %#x is not the driver's (ENOSYS); said once\n", number);
		}
		return ENOSYS;
	}
	default:
		return EIO;
	}
}

/*
 * WHICH SURFACES A SUBMISSION NAMES (M5g). Turnip gives every submission the list of every buffer the
 * device has, which costs Linux's driver little. This kernel does two things for each one
 * (HKGPU.cpp, submit()) that freedreno, naming only what a batch uses, never paid for on this scale: an
 * IOSurface is pushed out of the processor's cache, whole, if 4 ms have gone by since the last time; and
 * it is marked as being drawn until this submission is done, which is what the display waits for
 * before it shows one. So in the window server, with every layer's surface imported, each frame
 * flushed every surface (3.1 ms a submission at 30 a second by the end of run 211412, 85% of the time
 * to record a frame, and growing with the surfaces) and marked all of them.
 *
 * air_vk.c knows what a recording uses, and says so before its vkQueueSubmit. The surfaces it did not
 * name are left out of the list that goes to the kernel; every other buffer stays, and the kernel's
 * record of use for those is as it was. A destroy of a texture waits for the fence of the work that
 * used it in air_vk.c (when_done), so a buffer left out cannot be taken from under the GPU.
 */
#define HK_DRM_HANDLES 8193
#define HK_DRM_NAMES   256

static int surface_of_handle[HK_DRM_HANDLES];   /* the IOSurface a handle is, or 0 */
static pthread_mutex_t names_lock = PTHREAD_MUTEX_INITIALIZER;
static int names[HK_DRM_NAMES];
static unsigned name_count;
static bool names_given;
/* Of those, what the submission writes. A surface it only reads goes to the kernel without MSM_SUBMIT_BO_WRITE, which is how the kernel
 * knows it need not wait for the panel to finish fetching it (HKGPU.cpp, submit(), and FETCHED WHILE DRAWN in MiniIOMobileFramebuffer.cpp). */
static int writes[HK_DRM_NAMES];
static unsigned write_count;
static bool writes_given;

void hk_drm_name_surfaces(const int *first, unsigned first_count, const int *second, unsigned second_count);

void
hk_drm_name_surfaces(const int *first, unsigned first_count, const int *second, unsigned second_count)
{
	pthread_mutex_lock(&names_lock);
	name_count = 0;
	/* more than the table holds is every surface, as it was */
	names_given = first_count + second_count <= HK_DRM_NAMES;
	for (unsigned i = 0; names_given && i < first_count; i++) {
		names[name_count++] = first[i];
	}
	for (unsigned i = 0; names_given && i < second_count; i++) {
		names[name_count++] = second[i];
	}
	pthread_mutex_unlock(&names_lock);
}

void hk_drm_written_surfaces(const int *first, unsigned first_count, const int *second, unsigned second_count);

void
hk_drm_written_surfaces(const int *first, unsigned first_count, const int *second, unsigned second_count)
{
	pthread_mutex_lock(&names_lock);
	write_count = 0;
	writes_given = first_count + second_count <= HK_DRM_NAMES;
	for (unsigned i = 0; writes_given && i < first_count; i++) {
		writes[write_count++] = first[i];
	}
	for (unsigned i = 0; writes_given && i < second_count; i++) {
		writes[write_count++] = second[i];
	}
	pthread_mutex_unlock(&names_lock);
}

/* The call of a submission with the unnamed surfaces left out of its list. Turnip's own list stays as it is. */
static int
submit_named(struct drm_msm_gem_submit *submit)
{
	static struct drm_msm_gem_submit_bo *bos;
	static struct drm_msm_gem_submit_cmd *cmds;
	static uint32_t *moved;
	static uint32_t bos_room, cmds_room;
	const struct drm_msm_gem_submit_bo *from = (const void *)(uintptr_t)submit->bos;
	struct drm_msm_gem_submit plain = *submit;
	uint32_t kept = 0;

	pthread_mutex_lock(&names_lock);
	if (!names_given || submit->nr_cmds == 0) {
		names_given = false;
		pthread_mutex_unlock(&names_lock);
		return call((unsigned)(DRM_COMMAND_BASE + DRM_MSM_GEM_SUBMIT), submit, sizeof(*submit));
	}
	names_given = false;
	if (submit->nr_bos > bos_room) {
		free(bos);
		free(moved);
		bos = malloc(submit->nr_bos * sizeof(*bos));
		moved = malloc(submit->nr_bos * sizeof(*moved));
		bos_room = bos != NULL && moved != NULL ? submit->nr_bos : 0;
	}
	if (submit->nr_cmds > cmds_room) {
		free(cmds);
		cmds = malloc(submit->nr_cmds * sizeof(*cmds));
		cmds_room = cmds != NULL ? submit->nr_cmds : 0;
	}
	if (submit->nr_bos > bos_room || submit->nr_cmds > cmds_room) {
		pthread_mutex_unlock(&names_lock);
		return call((unsigned)(DRM_COMMAND_BASE + DRM_MSM_GEM_SUBMIT), submit, sizeof(*submit));
	}
	for (uint32_t i = 0; i < submit->nr_bos; i++) {
		const int surface = from[i].handle < HK_DRM_HANDLES ? surface_of_handle[from[i].handle] : 0;
		bool keep = surface == 0;

		for (unsigned n = 0; !keep && n < name_count; n++) {
			keep = names[n] == surface;
		}
		moved[i] = kept;
		if (keep) {
			bos[kept] = from[i];
			if (surface != 0 && writes_given) {
				bool written = false;

				for (unsigned n = 0; !written && n < write_count; n++) {
					written = writes[n] == surface;
				}
				if (!written) {
					bos[kept].flags &= ~(uint32_t)MSM_SUBMIT_BO_WRITE;
				}
			}
			kept++;
		}
	}
	memcpy(cmds, (const void *)(uintptr_t)submit->cmds, submit->nr_cmds * sizeof(*cmds));
	for (uint32_t i = 0; i < submit->nr_cmds; i++) {
		cmds[i].submit_idx = moved[cmds[i].submit_idx];
	}
	plain.bos = (uint64_t)(uintptr_t)bos;
	plain.cmds = (uint64_t)(uintptr_t)cmds;
	plain.nr_bos = kept;

	writes_given = false;

	const int error = call((unsigned)(DRM_COMMAND_BASE + DRM_MSM_GEM_SUBMIT), &plain, sizeof(plain));

	pthread_mutex_unlock(&names_lock);
	submit->fence = plain.fence;
	submit->fence_fd = plain.fence_fd;
	return error;
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	int error = call((unsigned)(request & 0xff), arg, IOCPARM_LEN(request));

	/* DRM_IOCTL_GEM_CLOSE: that number may be another kind of buffer next */
	if (error == 0 && (request & 0xff) == 0x09 && ((const uint32_t *)arg)[0] < HK_DRM_HANDLES) {
		surface_of_handle[((const uint32_t *)arg)[0]] = 0;
	}
	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), (void *)(uintptr_t)data, size);
}

static int submit_with_syncobjs(struct drm_msm_gem_submit *submit);

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	if (index == DRM_MSM_GEM_SUBMIT && size >= sizeof(struct drm_msm_gem_submit) &&
	    (((struct drm_msm_gem_submit *)data)->flags & (MSM_SUBMIT_SYNCOBJ_IN | MSM_SUBMIT_SYNCOBJ_OUT)) != 0) {
		return submit_with_syncobjs(data);
	}
	if (index == DRM_MSM_GEM_SUBMIT && size >= sizeof(struct drm_msm_gem_submit)) {
		return -submit_named(data);
	}
	return -call((unsigned)(DRM_COMMAND_BASE + index), data, size);
}

/*
 * The driver calls itself what Linux's is called, and is of the version whose interface it has:
 * 1.7, buffers at addresses the kernel chooses and tells ("softpin"), nothing newer.
 */
drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr version = calloc(1, sizeof(*version));

	(void)fd;
	if (version != NULL) {
		version->version_major = 1;
		version->version_minor = 7;
		version->name = strdup("msm");
		version->name_len = 3;
		version->date = strdup("");
		version->desc = strdup("Adreno on Darwin");
		version->desc_len = (int)strlen(version->desc);
	}
	return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
	if (version != NULL) {
		free(version->name);
		free(version->date);
		free(version->desc);
		free(version);
	}
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
#ifdef HK_DRM_SYNCOBJ
	*value = capability == DRM_CAP_SYNCOBJ;         /* binary ones, kept here (below); no timelines */
#else
	(void)capability;
	*value = 0;
#endif
	return 0;
}

/*
 * The devices there are: one, the GPU, platform device "msm", whose node is /dev/null (what it
 * opens means nothing: there is one connection, made on the first request). No primary node.
 */
int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
	drmDevicePtr device;

	(void)flags;
	if (devices == NULL || max_devices < 1) {
		return 1;
	}
	if ((device = calloc(1, sizeof(*device))) == NULL ||
	    (device->nodes = calloc(DRM_NODE_MAX, sizeof(char *))) == NULL ||
	    (device->businfo.platform = calloc(1, sizeof(*device->businfo.platform))) == NULL) {
		if (device != NULL) {
			free(device->nodes);
			free(device);
		}
		return -ENOMEM;
	}
	device->nodes[DRM_NODE_RENDER] = strdup("/dev/null");
	device->available_nodes = 1 << DRM_NODE_RENDER;
	device->bustype = DRM_BUS_PLATFORM;
	strlcpy(device->businfo.platform->fullname, "/soc/gpu", sizeof(device->businfo.platform->fullname));
	devices[0] = device;
	return 1;
}

/* the device a descriptor is of: the one there is, whatever the descriptor */
int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	(void)fd;
	return drmGetDevices2(flags, device, 1) == 1 ? 0 : -ENOMEM;
}

void
drmFreeDevice(drmDevicePtr *device)
{
	if (device != NULL) {
		drmFreeDevices(device, 1);
		*device = NULL;
	}
}

int
drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
	return a != NULL && b != NULL && a->bustype == b->bustype;
}

void
drmFreeDevices(drmDevicePtr devices[], int count)
{
	for (int i = 0; devices != NULL && i < count; i++) {
		if (devices[i] != NULL) {
			for (int node = 0; node < DRM_NODE_MAX; node++) {
				free(devices[i]->nodes[node]);
			}
			free(devices[i]->nodes);
			free(devices[i]->businfo.platform);
			free(devices[i]);
		}
	}
}

void *
hk_drm_map(int fd, uint32_t handle, uint64_t size)
{
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;

	(void)fd;
	(void)size;
	/*
	 * Linux gives freedreno its buffers write-combined, with no cache of this processor's between
	 * the program and the memory the GPU sees. So does the kernel's driver here: the memory is
	 * made that way, and this mapping is as the memory is.
	 */
	if (IOConnectMapMemory64(connection, handle, mach_task_self(), &address, &mapped, kIOMapAnywhere) != 0) {
		return (void *)-1;              /* mmap()'s MAP_FAILED */
	}
	return (void *)(uintptr_t)address;
}

void
hk_drm_unmap(int fd, uint32_t handle, void *address)
{
	(void)fd;
	IOConnectUnmapMemory64(connection, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)address);
}

int
hk_drm_surface(int fd, struct hk_drm_surface *surface)
{
	int error;

	(void)fd;
	surface->handle = HK_GPU_DESCRIBE;
	error = call(HK_GPU_SURFACE, surface, sizeof(*surface));
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmPrimeFDToHandle(int fd, int surface, uint32_t *handle)
{
	struct hk_drm_surface request = { .surface = (uint32_t)surface };
	int error = call(HK_GPU_SURFACE, &request, sizeof(request));

	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
	}
	*handle = request.handle;
	if (request.handle < HK_DRM_HANDLES) {
		surface_of_handle[request.handle] = surface;
	}
	return 0;
}

static int
none(void)
{
	errno = ENOSYS;
	return -1;
}

/* Buffers and fences as file descriptors, for handing them to other programs and drivers: none of that here. */
#define NONE(name, ...) int name(__VA_ARGS__) { return none(); }
NONE(drmPrimeHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) uint32_t flags, __attribute__((unused)) int *prime_fd)
NONE(drmSyncobjFDToHandle, __attribute__((unused)) int fd, __attribute__((unused)) int obj_fd,
    __attribute__((unused)) uint32_t *handle)
NONE(drmSyncobjHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int *obj_fd)
NONE(drmSyncobjImportSyncFile, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int sync_file_fd)
NONE(drmSyncobjExportSyncFile, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int *sync_file_fd)
NONE(drmSyncobjTransfer, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t dst_handle,
    __attribute__((unused)) uint64_t dst_point, __attribute__((unused)) uint32_t src_handle,
    __attribute__((unused)) uint64_t src_point, __attribute__((unused)) uint32_t flags)
NONE(drmSyncobjQuery, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t *handles,
    __attribute__((unused)) uint64_t *points, __attribute__((unused)) uint32_t handle_count)
NONE(drmSyncobjTimelineSignal, __attribute__((unused)) int fd, __attribute__((unused)) const uint32_t *handles,
    __attribute__((unused)) uint64_t *points, __attribute__((unused)) uint32_t handle_count)
NONE(drmSyncobjTimelineWait, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t *handles,
    __attribute__((unused)) uint64_t *points, __attribute__((unused)) unsigned num_handles,
    __attribute__((unused)) int64_t timeout_nsec, __attribute__((unused)) unsigned flags,
    __attribute__((unused)) uint32_t *first_signaled)

/*
 * ---- sync objects --------------------------------------------------------------------------------
 *
 * Linux's are the kernel's; the driver here has none, so they are kept in this process. There is
 * one ring and the GPU does its work in the order it is given, so a sync object is no more than
 * what will signal it: nothing yet, a submission (its fence: done when the GPU is past it), or
 * signalled. A submission's in-objects need no waiting on the GPU's side -- what signals them was
 * submitted before, on the same ring, or was signalled by the processor -- and its out-objects
 * become its fence (submit_with_syncobjs). Waiting for a submitted one is the driver's fence wait.
 */
#define SYNCOBJS        4096
enum { SYNC_NONE, SYNC_SUBMITTED, SYNC_SIGNALLED };
static struct syncobj {
	uint8_t used, state;
	uint32_t fence;
} syncobjs[SYNCOBJS];
static pthread_mutex_t syncobj_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t syncobj_changed = PTHREAD_COND_INITIALIZER;

static struct syncobj *
syncobj_of(uint32_t handle)
{
	return handle > 0 && handle < SYNCOBJS && syncobjs[handle].used ? &syncobjs[handle] : NULL;
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd;
	pthread_mutex_lock(&syncobj_lock);
	for (uint32_t i = 1; i < SYNCOBJS; i++) {
		if (!syncobjs[i].used) {
			syncobjs[i] = (struct syncobj){ 1, flags & DRM_SYNCOBJ_CREATE_SIGNALED ? SYNC_SIGNALLED : SYNC_NONE, 0 };
			pthread_mutex_unlock(&syncobj_lock);
			*handle = i;
			return 0;
		}
	}
	pthread_mutex_unlock(&syncobj_lock);
	errno = ENOMEM;
	return -ENOMEM;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	struct syncobj *sync;

	(void)fd;
	pthread_mutex_lock(&syncobj_lock);
	if ((sync = syncobj_of(handle)) != NULL) {
		sync->used = 0;
	}
	pthread_mutex_unlock(&syncobj_lock);
	if (sync == NULL) {
		errno = EINVAL;
		return -EINVAL;
	}
	return 0;
}

static int
syncobjs_set(const uint32_t *handles, uint32_t count, uint8_t state)
{
	int error = 0;

	pthread_mutex_lock(&syncobj_lock);
	for (uint32_t i = 0; i < count; i++) {
		struct syncobj *sync = syncobj_of(handles[i]);

		if (sync == NULL) {
			error = EINVAL;
		} else {
			sync->state = state;
		}
	}
	pthread_cond_broadcast(&syncobj_changed);
	pthread_mutex_unlock(&syncobj_lock);
	if (error != 0) {
		errno = error;
		return -error;
	}
	return 0;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	return syncobjs_set(handles, count, SYNC_NONE);
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	return syncobjs_set(handles, count, SYNC_SIGNALLED);
}

static int64_t
now_ns(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

/*
 * The driver's wait for a fence, until `deadline`, which is Linux's and Vulkan's kind of time:
 * CLOCK_MONOTONIC ns (os_time_get_absolute_timeout, the runtime's vkWaitForFences). 0 or ETIME.
 *
 * [The comment here said that clock is the driver's, and it was on Linux. It is not here, and
 * that was the busy poll of M4 (hk_drm_gpu_now_ns in xf86drm.h): every request came back at
 * once as over, with time left by this clock, and was asked again, for the whole of the GPU's
 * time. So the deadline is turned into time left, once, and that into a time of the driver's own
 * clock, once, and the rest of the wait goes by the driver's clock alone: moving the date
 * in the middle of a wait moves CLOCK_MONOTONIC here and must not move the wait.]
 *
 * THREE KINDS OF WAIT, by what is left when it is asked for:
 *   nothing (vkGetFenceStatus: a deadline of 0, or one already past)  one request, ending now.
 *     The driver looks and answers; not done, and it counts one "over" and says the first few
 *     (waitOver in HKGPU.cpp). With its two times next to each other that line is a look like
 *     this one, not a clock that disagrees. 0 by this clock is -now, which is why it is clamped.
 *   up to an hour  one end, as far off as was asked.
 *   more than an hour, "forever" among it (vkDeviceWaitIdle's 2^63 - 1 ns)  an hour from each
 *     request, and no end: HK_DRM_LONGEST_WAIT_NS says what that bounds. Forever here is what it
 *     is on Linux while the GPU answers, and a GPU that stopped answering is the driver's to
 *     recover, after which its fences read as done (gpu_recover=1, hk_adreno.c). If it has given
 *     up on the GPU instead, every request comes back at once, and this goes on asking at the
 *     back-off's slowest until the process goes, as it did before without the back-off.
 *
 * THE BACK-OFF is the last guard against a spin, and whatever else is fixed it stays. The driver
 * returns at once with time left in one case that is not wrong: it rounds its patience DOWN to
 * whole ms, so the last part of a millisecond before any deadline is no patience at all (and a
 * kernel round may make that us: TODO T38 (12)). So: 50 us, then 100, then 200 and no more, and
 * never past the end. A wait that does not end within the kernel's own 5 s comes back here after
 * that long asleep, and pays one back-off for it. Anything but "not yet" is ETIME at once,
 * as before.
 */
static int
fence_wait(uint32_t fence, int64_t deadline)
{
	int64_t left = deadline - now_ns();
	bool forever = left > (int64_t)HK_DRM_LONGEST_WAIT_NS;
	int64_t end = hk_drm_gpu_now_ns() + (left < 0 ? 0 : left);
	unsigned pause_us = 50;

	for (;;) {
		int64_t until = forever ? hk_drm_gpu_now_ns() + (int64_t)HK_DRM_LONGEST_WAIT_NS : end;
		struct drm_msm_wait_fence wait = {
			.fence = fence,
			.timeout = { .tv_sec = until / 1000000000, .tv_nsec = until % 1000000000 },
		};
		int error = call(DRM_COMMAND_BASE + DRM_MSM_WAIT_FENCE, &wait, sizeof(wait));
		int64_t now;

		if (error == 0) {
			return 0;
		}
		now = hk_drm_gpu_now_ns();
		if (error != ETIMEDOUT || (!forever && now >= end)) {
			return ETIME;   /* (the driver's own patience is shorter than some deadlines: then again) */
		}
		if (!forever && (uint64_t)(end - now) / 1000 < pause_us) {
			pause_us = (unsigned)((uint64_t)(end - now) / 1000) + 1;
		}
		usleep(pause_us);
		pause_us = pause_us >= 100 ? 200 : pause_us * 2;
	}
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
	int all = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0, error = 0;
	uint32_t first = count;

	(void)fd;
	if (count == 0) {
		return 0;
	}
	pthread_mutex_lock(&syncobj_lock);
	for (;;) {
		unsigned signalled = 0, waiting_submit = 0, none_at_all = 0;
		uint32_t fence = 0;
		int fence_set = 0;

		first = count;

		for (unsigned i = 0; i < count; i++) {
			struct syncobj *sync = syncobj_of(handles[i]);

			if (sync == NULL) {
				pthread_mutex_unlock(&syncobj_lock);
				errno = EINVAL;
				return -EINVAL;
			}
			if (sync->state == SYNC_SIGNALLED) {
				signalled++;
				first = first < i ? first : i;
			} else if (sync->state == SYNC_SUBMITTED) {
				/* all: the latest of them; any: the earliest (the ring finishes in order) */
				if (!fence_set || (all ? (int32_t)(sync->fence - fence) > 0 : (int32_t)(sync->fence - fence) < 0)) {
					fence = sync->fence;
					fence_set = 1;
					if (!all) {
						first = first < i ? first : i;
					}
				}
			} else if (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) {
				waiting_submit++;
			} else {
				none_at_all++;
			}
		}
		if (all ? signalled == count : signalled > 0) {
			break;
		}
		if (none_at_all > 0 && (all || (signalled == 0 && !fence_set && waiting_submit == 0))) {
			error = EINVAL;         /* (Linux's answer for an object nothing will signal) */
			break;
		}
		if (waiting_submit > 0 && (all || !fence_set)) {
			/* until something is submitted or signalled; the condition's clock is the realtime one */
			struct timespec until;
			int64_t left = timeout_nsec - now_ns();

			if (left <= 0) {
				error = ETIME;
				break;
			}
			if (left > (int64_t)HK_DRM_LONGEST_WAIT_NS) {
				left = (int64_t)HK_DRM_LONGEST_WAIT_NS; /* (this loop asks again) */
			}
			clock_gettime(CLOCK_REALTIME, &until);
			until.tv_sec += left / 1000000000 + (until.tv_nsec + left % 1000000000) / 1000000000;
			until.tv_nsec = (until.tv_nsec + left % 1000000000) % 1000000000;
			if (pthread_cond_timedwait(&syncobj_changed, &syncobj_lock, &until) == ETIMEDOUT && now_ns() >= timeout_nsec) {
				error = ETIME;
				break;
			}
			continue;
		}
		pthread_mutex_unlock(&syncobj_lock);
		error = fence_wait(fence, timeout_nsec);
		pthread_mutex_lock(&syncobj_lock);
		if (error != 0) {
			break;
		}
		/* that fence is past: everything submitted at or before it is done */
		for (uint32_t i = 1; i < SYNCOBJS; i++) {
			if (syncobjs[i].used && syncobjs[i].state == SYNC_SUBMITTED && (int32_t)(syncobjs[i].fence - fence) <= 0) {
				syncobjs[i].state = SYNC_SIGNALLED;
			}
		}
		if (!all) {
			break;
		}
	}
	pthread_mutex_unlock(&syncobj_lock);
	if (error != 0) {
		errno = error;
		return -error;
	}
	if (first_signaled != NULL) {
		*first_signaled = first < count ? first : 0;
	}
	return 0;
}

/*
 * A submission with sync objects: the driver takes it without them (the flags and the arrays
 * cleared for its call, put back after), then its out-objects are its fence, and in-objects asked
 * to be reset after their wait are reset. An in-object that nothing has been submitted for yet
 * cannot be waited for on this ring: said once, and the submission goes ahead.
 */
static int
submit_with_syncobjs(struct drm_msm_gem_submit *submit)
{
	struct drm_msm_gem_submit plain = *submit;
	uint32_t stride = submit->syncobj_stride != 0 ? submit->syncobj_stride : sizeof(struct drm_msm_gem_submit_syncobj);
	int error;

	if (submit->flags & MSM_SUBMIT_SYNCOBJ_IN) {
		static int told;

		pthread_mutex_lock(&syncobj_lock);
		for (uint32_t i = 0; i < submit->nr_in_syncobjs; i++) {
			const struct drm_msm_gem_submit_syncobj *in =
			    (const void *)(uintptr_t)(submit->in_syncobjs + (uint64_t)i * stride);
			struct syncobj *sync = syncobj_of(in->handle);

			if (sync != NULL && sync->state == SYNC_NONE && !told) {
				told = 1;
				fprintf(stderr, "hk_drm: a submission waits for a sync object nothing has signalled yet; not waited for\n");
			}
		}
		pthread_mutex_unlock(&syncobj_lock);
	}
	plain.flags &= ~(uint32_t)(MSM_SUBMIT_SYNCOBJ_IN | MSM_SUBMIT_SYNCOBJ_OUT);
	plain.in_syncobjs = plain.out_syncobjs = 0;
	plain.nr_in_syncobjs = plain.nr_out_syncobjs = plain.syncobj_stride = 0;
	error = submit_named(&plain);
	if (error != 0) {
		return -error;
	}
	submit->fence = plain.fence;
	submit->fence_fd = plain.fence_fd;
	pthread_mutex_lock(&syncobj_lock);
	for (uint32_t i = 0; (submit->flags & MSM_SUBMIT_SYNCOBJ_IN) && i < submit->nr_in_syncobjs; i++) {
		const struct drm_msm_gem_submit_syncobj *in = (const void *)(uintptr_t)(submit->in_syncobjs + (uint64_t)i * stride);
		struct syncobj *sync = syncobj_of(in->handle);

		if (sync != NULL && (in->flags & MSM_SUBMIT_SYNCOBJ_RESET)) {
			sync->state = SYNC_NONE;
		}
	}
	for (uint32_t i = 0; (submit->flags & MSM_SUBMIT_SYNCOBJ_OUT) && i < submit->nr_out_syncobjs; i++) {
		const struct drm_msm_gem_submit_syncobj *out =
		    (const void *)(uintptr_t)(submit->out_syncobjs + (uint64_t)i * stride);
		struct syncobj *sync = syncobj_of(out->handle);

		if (sync != NULL) {
			sync->state = SYNC_SUBMITTED;
			sync->fence = plain.fence;
		}
	}
	pthread_cond_broadcast(&syncobj_changed);
	pthread_mutex_unlock(&syncobj_lock);
	return 0;
}
