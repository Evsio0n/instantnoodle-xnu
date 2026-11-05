/*
 * What Mesa's freedreno takes from libdrm's xf86drm.h, for a system without Linux's DRM: the
 * declarations here, the functions in hk_drm.c, which carry the requests to the kernel's
 * GPU driver (the service HKGPU; iokit/Kernel/HKGPU.cpp) as calls of its user client.
 * tools/mk-mesa.sh puts both into Mesa's tree, where they are the "libdrm" freedreno is built
 * against. The request numbers and their structures are Linux's (Mesa's include/drm-uapi).
 */
#ifndef HK_XF86DRM_H
#define HK_XF86DRM_H

#include <fcntl.h>
#include <mach/mach_time.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "drm-uapi/drm.h"

#ifdef __cplusplus
extern "C" {                    /* (turnip is C++) */
#endif

/*
 * Not libdrm's: now, on the clock the kernel's driver measures a wait's end against, in ns.
 *
 * iokit/Kernel/HKGPU.cpp's millisecondsUntil takes a request's timeout as a time of
 * mach_continuous_time(), so this is that function and the same conversion, and not a clock id
 * that happens to agree with it today. CLOCK_MONOTONIC does not: on Darwin it is the date less
 * the date the system says it was started at, tens of thousands of seconds behind on a phone
 * that was powered up before this boot, so that a deadline made of it was over before it began
 * and every wait of turnip's was a busy poll for the whole of the GPU's time (TODO T38 (12),
 * M4; log 061800, "until 67.490280000 s, asked for at 41679.009 s"). In a header, inline, so
 * that hk_drm.c and turnip's get_abs_timeout (tu_knl_drm.h) share it and a library that calls
 * neither -- gallium's goes by msm_pipe_wait, in msm_priv.h -- gets nothing new.
 *
 * Whole ticks first, so that the product cannot overflow however long the phone has been up;
 * the timebase is read once, and two threads reading it at once read the same numbers.
 */
static inline int64_t
hk_drm_gpu_now_ns(void)
{
	static mach_timebase_info_data_t timebase;
	uint64_t ticks = mach_continuous_time();

	if (timebase.denom == 0) {
		mach_timebase_info(&timebase);
	}
	return (int64_t)(ticks / timebase.denom * timebase.numer + ticks % timebase.denom * timebase.numer / timebase.denom);
}

/*
 * The longest wait put to the driver in one request, in ns: an hour, which msm_priv.h calls almost
 * infinite, and it is: the driver's own patience is 5 s a request whatever it is told
 * (GPU_PATIENCE_MS). What this bounds is the arithmetic. millisecondsUntil multiplies the
 * request's seconds by 10^9 in 64 bits, signed; a "forever" of Vulkan's (2^63 - 1 ns from
 * vkDeviceWaitIdle, 2^64 - 1 from tu_queue_wait_fence) added to now and sent as it is would wrap
 * negative there and be a time long past.
 */
#define HK_DRM_LONGEST_WAIT_NS  (3600ull * 1000000000ull)

#define DRM_NODE_PRIMARY        0
#define DRM_NODE_RENDER         2
#define DRM_NODE_MAX            3
#define DRM_BUS_PCI             0
#define DRM_BUS_PLATFORM        2

/* the devices there are (drmGetDevices2: the GPU, the one; turnip finds it so) */
typedef struct _drmPlatformBusInfo {
	char fullname[512];
} drmPlatformBusInfo, *drmPlatformBusInfoPtr;

typedef struct _drmPciBusInfo {         /* (never one here: the GPU is a platform device) */
	uint16_t domain;
	uint8_t bus, dev, func;
} drmPciBusInfo, *drmPciBusInfoPtr;

typedef struct _drmDevice {
	char **nodes;                   /* by DRM_NODE_*; the render node is the one */
	int available_nodes;            /* 1 << DRM_NODE_* */
	int bustype;
	union {
		drmPciBusInfoPtr pci;
		drmPlatformBusInfoPtr platform;
	} businfo;
	union {
		void *platform;
	} deviceinfo;
} drmDevice, *drmDevicePtr;

int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices);
void drmFreeDevices(drmDevicePtr devices[], int count);
int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device);
void drmFreeDevice(drmDevicePtr *device);
int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b);
#define DRM_CLOEXEC             O_CLOEXEC
#define DRM_RDWR                O_RDWR

typedef struct _drmVersion {
	int version_major, version_minor, version_patchlevel;
	int name_len;
	char *name;
	int date_len;
	char *date;
	int desc_len;
	char *desc;
} drmVersion, *drmVersionPtr;

int drmOpenWithType(const char *name, const char *busid, int type);
int drmIoctl(int fd, unsigned long request, void *arg);
int drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size);
int drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr version);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);

/*
 * Buffers and fences as file descriptors: there are none here. But memory that somebody else
 * has made does come in, as Linux's comes in by file descriptor: an IOSurface, by its ID, where
 * freedreno expects the descriptor (see hk_drm_surface() below).
 */
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int surface, uint32_t *handle);
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);
/* binary sync objects, kept in this process (hk_drm.c); no timelines (DRM_CAP_SYNCOBJ_TIMELINE says 0) */
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count);
int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled);
int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count);
int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t handle_count);
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled);
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point,
    uint32_t flags);

/* Not libdrm's: a buffer's memory in this process, in place of mmap() on the device (freedreno_bo.c is patched to call these). */
void *hk_drm_map(int fd, uint32_t handle, uint64_t size);
void hk_drm_unmap(int fd, uint32_t handle, void *address);

/*
 * Not libdrm's either: what the memory of an IOSurface would be like as a buffer (the kernel's
 * request and its answer: struct hk_surface in iokit/Kernel/HKGPU.cpp); 0, or -1 with
 * errno. The buffer itself is made by drmPrimeFDToHandle(), which freedreno calls with its
 * table of buffers locked, so that a buffer on its way out and the same surface on its way in
 * cannot pass each other.
 */
struct hk_drm_surface {
	uint32_t surface;                       /* the IOSurface's ID */
	uint32_t handle;                        /* (the kernel's: which of the two is asked) */
	uint64_t size;                          /* back: how big the buffer is, */
	uint32_t offset, row_bytes;             /* where the pixels start in it and how long a row of them is, */
	uint32_t width, height, pixel_format, planes;   /* and what they are */
};
int hk_drm_surface(int fd, struct hk_drm_surface *surface);

#ifdef __cplusplus
}
#endif

#endif
