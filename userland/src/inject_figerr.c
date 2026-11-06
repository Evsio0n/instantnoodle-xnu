/*
 * libhackios-figerr.dylib: who says -12914 (TODO, M5e: CMPhoto's "PHFigDecoder - failed async decode", the full-size HDR photo).
 *
 * VideoToolbox and CMPhoto report their errors through FigSignalErrorAt in CoreMedia, called through stub islands with no table to hook. So its entry is patched in this process's
 * private copy of the page: the first four instructions go to fig_entry below, which says the error and who called, runs them, and goes on. It only watches. For a test root, named in
 * etc/inject.conf for the programs to watch; the addresses it prints are unslid, for `ipsw dyld a2s`.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <execinfo.h>
#include <fcntl.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <CoreGraphics/CoreGraphics.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/param.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);
extern const char *getprogname(void);

#define CACHE_BASE 0x180000000UL

static int console = -1;
static uintptr_t slide;
void *hk_fig_goes_on;

__attribute__((used)) void
hk_fig_says(int64_t error, uintptr_t caller)
{
	static unsigned said;
	char line[420];
	void *frames[12];

	if ((int32_t)error == 0 || said >= 200) {
		return;
	}
	said++;
	int at = snprintf(line, sizeof(line), "figerr[%s %d]: %d from %#lx:", getprogname(), getpid(), (int32_t)error, (unsigned long)(caller - slide));
	if ((int32_t)error == -12914 || said < 40) {
		int count = backtrace(frames, 12);

		for (int i = 2; i < count && at < (int)sizeof(line) - 24; i++) {
			at += snprintf(line + at, sizeof(line) - at, " %#lx", (unsigned long)((uintptr_t)frames[i] - slide));
		}
	}
	at += snprintf(line + at, sizeof(line) - at, "\n");
	if (console >= 0) {
		write(console, line, (size_t)at < sizeof(line) ? (size_t)at : sizeof(line) - 1);
	}
}

extern uint32_t CVPixelBufferGetPixelFormatType(const void *);
extern size_t CVPixelBufferGetWidth(const void *);
extern size_t CVPixelBufferGetHeight(const void *);
extern CFTypeID CVPixelBufferGetTypeID(void);
void *hk_celeste_goes_on;
static const uint32_t celeste_expected[4] = { 0x6db63bef, 0x6d0133ed, 0x6d022beb, 0x6d0323e9 };
#define CELESTE_FIRST_FOUR \
	"stp d15, d14, [sp, #-0xa0]!\n" \
	"stp d13, d12, [sp, #0x10]\n" \
	"stp d11, d10, [sp, #0x20]\n" \
	"stp d9, d8, [sp, #0x30]\n"

/* What VideoToolbox's rotation was asked: the arguments of vt_Rotate_Celeste, its first route, in the order of its one caller (20F66) */
__attribute__((used)) void
hk_celeste_says(const void *source, const void *destination, uint32_t degrees, const double *rects)
{
	static unsigned said;
	char line[300];

	if (said++ >= 60 || source == NULL || destination == NULL || CFGetTypeID(source) != CVPixelBufferGetTypeID() || CFGetTypeID(destination) != CVPixelBufferGetTypeID()) {
		return;
	}
	const uint32_t from = CVPixelBufferGetPixelFormatType(source), to = CVPixelBufferGetPixelFormatType(destination);
	const int at = snprintf(line, sizeof(line), "figerr[%s %d]: at %llu rotation of %c%c%c%c %zux%zu to %c%c%c%c %zux%zu, %u degrees; d0-d7 %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f\n", getprogname(), getpid(), clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000,
	    (char)(from >> 24), (char)(from >> 16), (char)(from >> 8), (char)from, CVPixelBufferGetWidth(source), CVPixelBufferGetHeight(source),
	    (char)(to >> 24), (char)(to >> 16), (char)(to >> 8), (char)to, CVPixelBufferGetWidth(destination), CVPixelBufferGetHeight(destination),
	    degrees, rects[0], rects[1], rects[2], rects[3], rects[4], rects[5], rects[6], rects[7]);

	if (console >= 0) {
		write(console, line, (size_t)at < sizeof(line) ? (size_t)at : sizeof(line) - 1);
	}
}

/* (the four instructions the patch overwrote: vt_Rotate_Celeste's own, 20F66) */
__attribute__((naked)) static void
celeste_entry(void)
{
	__asm__(
	    "sub sp, sp, #0xa0\n"
	    "stp x0, x1, [sp]\n"
	    "stp x2, x3, [sp, #0x10]\n"
	    "stp x4, x5, [sp, #0x20]\n"
	    "stp x6, x7, [sp, #0x30]\n"
	    "stp d0, d1, [sp, #0x40]\n"
	    "stp d2, d3, [sp, #0x50]\n"
	    "stp d4, d5, [sp, #0x60]\n"
	    "stp d6, d7, [sp, #0x70]\n"
	    "stp x29, x30, [sp, #0x80]\n"
	    "add x29, sp, #0x80\n"
	    "mov x0, x1\n"
	    "mov x1, x3\n"
	    "mov x2, x5\n"
	    "add x3, sp, #0x40\n"
	    "bl _hk_celeste_says\n"
	    "ldp x29, x30, [sp, #0x80]\n"
	    "ldp d6, d7, [sp, #0x70]\n"
	    "ldp d4, d5, [sp, #0x60]\n"
	    "ldp d2, d3, [sp, #0x50]\n"
	    "ldp d0, d1, [sp, #0x40]\n"
	    "ldp x6, x7, [sp, #0x30]\n"
	    "ldp x4, x5, [sp, #0x20]\n"
	    "ldp x2, x3, [sp, #0x10]\n"
	    "ldp x0, x1, [sp]\n"
	    "add sp, sp, #0xa0\n"
	    CELESTE_FIRST_FOUR
	    "adrp x16, _hk_celeste_goes_on@PAGE\n"
	    "ldr x16, [x16, _hk_celeste_goes_on@PAGEOFF]\n"
	    "br x16\n");
}

/*
 * Where an HDR photo's highlights stop (K33). Photos does not draw them: PXImageModulationManager decides from the asset and the
 * screen, and PXImageLayerModulator puts a CAFilter on the image's layer and the gain map in a sublayer of its own, for the window
 * server to composite. So each value on that way is said as it is set. Numbers and types only: an asset is never printed, its name is in its description.
 */
#define PX_SAID 400
static unsigned px_said;

static void
px_say(const char *format, ...)
{
	char line[300];
	va_list ap;

	if (px_said++ >= PX_SAID || console < 0) {
		return;
	}
	const int prefix = snprintf(line, sizeof(line), "figerr[%s %d]: ", getprogname(), getpid());

	va_start(ap, format);
	const int at = prefix + vsnprintf(line + prefix, sizeof(line) - (size_t)prefix - 1, format, ap);
	va_end(ap);
	line[MIN(at, (int)sizeof(line) - 2)] = '\n';
	write(console, line, (size_t)MIN(at + 1, (int)sizeof(line) - 1));
}

#define PX_SETTER(NAME, TYPE, FORMAT, CONVERT) \
	static void (*NAME##_was)(id, SEL, TYPE); \
	static void NAME##_now(id self, SEL cmd, TYPE value) \
	{ \
		px_say("%s %s " FORMAT, class_getName(object_getClass(self)), sel_getName(cmd), CONVERT(value)); \
		NAME##_was(self, cmd, value); \
	}
#define PX_INT(X) ((int)(X))
#define PX_LONG(X) ((long)(X))
#define PX_DOUBLE(X) ((double)(X))
PX_SETTER(px_b, BOOL, "%d", PX_INT)
PX_SETTER(px_q, long, "%ld", PX_LONG)
PX_SETTER(px_f, float, "%f", PX_DOUBLE)
PX_SETTER(px_d, double, "%f", PX_DOUBLE)

/* (when: the clock of rwtouch's t-mark lines, so that an open of a photo is timed from its tap) */
static unsigned long long
px_now(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
}

/* A picture of the size a full view shows reaching a layer: when the base is really there (K33: the 371x495 preview comes first). Sizes only. */
static void (*px_contents_was)(id, SEL, id);
static void
px_contents_now(id self, SEL cmd, id contents)
{
	static CFTypeID image_type;
	size_t width = 0, height = 0;

	if (image_type == 0) {
		image_type = CGImageGetTypeID();
	}
	if (contents != nil && CFGetTypeID(contents) == image_type) {
		width = CGImageGetWidth((CGImageRef)contents);
		height = CGImageGetHeight((CGImageRef)contents);
	}
	if (width >= 300 && height >= 300) {
		px_say("contents %zux%zu at %llu", width, height, px_now());
	}
	px_contents_was(self, cmd, contents);
}

static float (*px_gain_was)(id, SEL, id);
static float
px_gain_now(id self, SEL cmd, id asset)
{
	const float gain = px_gain_was(self, cmd, asset);

	px_say("+%s = %f", sel_getName(cmd), (double)gain);
	return gain;
}

static long (*px_format_was)(id, SEL, id);
static long
px_format_now(id self, SEL cmd, id asset)
{
	const long format = px_format_was(self, cmd, asset);

	px_say("+%s = %ld", sel_getName(cmd), format);
	return format;
}

static void (*px_map_was)(id, SEL, CGImageRef, BOOL);
static void
px_map_now(id self, SEL cmd, CGImageRef image, BOOL animated)
{
	/* (how much there is in the gain map, as numbers: the compositor's copy reads 0 0 0 0 at its centre, run 031334) */
	unsigned top[4] = { 0 }, nonzero = 0, looked = 0;
	const size_t width = image != NULL ? CGImageGetWidth(image) : 0, height = image != NULL ? CGImageGetHeight(image) : 0;
	const size_t bits = image != NULL ? CGImageGetBitsPerPixel(image) : 0, row = image != NULL ? CGImageGetBytesPerRow(image) : 0;
	CFDataRef bytes = image != NULL ? CGDataProviderCopyData(CGImageGetDataProvider(image)) : NULL;

	if (bytes != NULL && bits != 0 && bits % 8 == 0) {
		const uint8_t *data = CFDataGetBytePtr(bytes);
		const size_t size = (size_t)CFDataGetLength(bytes), stride = bits / 8;

		for (size_t j = 0; j < 32; j++) {
			for (size_t i = 0; i < 32; i++) {
				const size_t at = (height - 1) * j / 31 * row + (width - 1) * i / 31 * stride;
				unsigned any = 0;

				for (size_t k = 0; k < MIN(stride, 4) && at + k < size; k++) {
					top[k] = data[at + k] > top[k] ? data[at + k] : top[k];
					any |= data[at + k];
				}
				nonzero += any != 0;
				looked++;
			}
		}
	}
	if (bytes != NULL) {
		CFRelease(bytes);
	}
	px_say("setGainMapImage: %zux%zu at %llu, %zu bits, row %zu, alpha %d, animated %d; of %u points %u not zero, highest by byte %u %u %u %u", width, height, px_now(), bits, row,
	    image != NULL ? (int)CGImageGetAlphaInfo(image) : -1, (int)animated, looked, nonzero, top[0], top[1], top[2], top[3]);
	px_map_was(self, cmd, image, animated);
}

static void (*px_add_was)(id, SEL, id);
static void
px_add_now(id self, SEL cmd, id layer)
{
	px_say("_addFilterToLayer: %s", layer != nil ? class_getName(object_getClass(layer)) : "nil");
	px_add_was(self, cmd, layer);
}

static void (*px_value_was)(id, SEL, id, id);
static void
px_value_now(id self, SEL cmd, id value, id key)
{
	id type = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("type"));
	CFStringRef shown = value != nil ? CFCopyDescription((CFTypeRef)value) : NULL;
	char type_text[40] = "", key_text[40] = "", value_text[160] = "";

	if (type != nil) {
		CFStringGetCString((CFStringRef)type, type_text, sizeof(type_text), kCFStringEncodingUTF8);
	}
	if (key != nil) {
		CFStringGetCString((CFStringRef)key, key_text, sizeof(key_text), kCFStringEncodingUTF8);
	}
	if (shown != NULL) {
		CFStringGetCString(shown, value_text, sizeof(value_text), kCFStringEncodingUTF8);
		CFRelease(shown);
	}
	if (strcmp(type_text, "edrGain") == 0) {
		px_say("CAFilter %s %s = %s", type_text, key_text, value_text);
	}
	px_value_was(self, cmd, value, key);
}

static void
px_hook(const char *class_name, const char *selector, bool of_class, IMP now, IMP *was)
{
	Class class = objc_getClass(class_name);
	Method method = class == Nil ? NULL : of_class ? class_getClassMethod(class, sel_registerName(selector)) : class_getInstanceMethod(class, sel_registerName(selector));

	if (method == NULL) {
		dprintf(console, "figerr[%s]: no %s of %s\n", getprogname(), selector, class_name);
		return;
	}
	*was = method_setImplementation(method, now);
}

static void
px_watch(void)
{
#define PX_S(CLASS, SELECTOR, NAME) px_hook(CLASS, SELECTOR, false, (IMP)NAME##_now, (IMP *)&NAME##_was)
	PX_S("PXImageModulationManager", "setCurrentScreenSupportsHDR:", px_b);
	PX_S("PXImageModulationManager", "setRequestedEDRHeadroomFactor:", px_d);
	PX_S("PXImageLayerModulator", "setFilterType:", px_q);
	PX_S("PXImageLayerModulator", "setHdrGain:", px_f);
	px_hook("PXImageModulationManager", "_hdrGainForAsset:", true, (IMP)px_gain_now, (IMP *)&px_gain_was);
	px_hook("PXImageModulationManager", "_contentFormatForAsset:", true, (IMP)px_format_now, (IMP *)&px_format_was);
	px_hook("PXImageLayerModulator", "setGainMapImage:animated:", false, (IMP)px_map_now, (IMP *)&px_map_was);
	px_hook("PXImageLayerModulator", "_addFilterToLayer:", false, (IMP)px_add_now, (IMP *)&px_add_was);
	px_hook("CAFilter", "setValue:forKey:", false, (IMP)px_value_now, (IMP *)&px_value_was);
	px_hook("CALayer", "setContents:", false, (IMP)px_contents_now, (IMP *)&px_contents_was);
	dprintf(console, "figerr[%s %d]: watching Photos' image modulation\n", getprogname(), getpid());
}

/* (the four instructions the patch overwrote: FigSignalErrorAt's own, 20F66) */
__attribute__((naked)) static void
fig_entry(void)
{
	__asm__(
	    "stp x0, x1, [sp, #-0x50]!\n"
	    "stp x2, x3, [sp, #0x10]\n"
	    "stp x4, x5, [sp, #0x20]\n"
	    "stp x6, x7, [sp, #0x30]\n"
	    "stp x29, x30, [sp, #0x40]\n"
	    "add x29, sp, #0x40\n"
	    "mov x1, x30\n"
	    "bl _hk_fig_says\n"
	    "ldp x29, x30, [sp, #0x40]\n"
	    "ldp x6, x7, [sp, #0x30]\n"
	    "ldp x4, x5, [sp, #0x20]\n"
	    "ldp x2, x3, [sp, #0x10]\n"
	    "ldp x0, x1, [sp], #0x50\n"
	    "stp x26, x25, [sp, #-0x50]!\n"
	    "stp x24, x23, [sp, #0x10]\n"
	    "stp x22, x21, [sp, #0x20]\n"
	    "stp x20, x19, [sp, #0x30]\n"
	    "adrp x16, _hk_fig_goes_on@PAGE\n"
	    "ldr x16, [x16, _hk_fig_goes_on@PAGEOFF]\n"
	    "br x16\n");
}

__attribute__((constructor)) static void
fig_watch(void)
{
	size_t length;
	static const uint32_t expected[4] = { 0xa9bb67fa, 0xa9015ff8, 0xa90257f6, 0xa9034ff4 };
	uint32_t *entry = dlsym(RTLD_DEFAULT, "FigSignalErrorAt");

	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	console = open("/dev/console", O_WRONLY);
	if (entry == NULL || memcmp(entry, expected, sizeof(expected)) != 0) {
		dprintf(console, "figerr[%s]: FigSignalErrorAt is %p and not what this was written for; left alone\n", getprogname(), entry);
		return;
	}
	const vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);

	if ((vm_address_t)(entry + 4) > page + vm_page_size ||
	    vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		dprintf(console, "figerr[%s]: cannot write the page of FigSignalErrorAt\n", getprogname());
		return;
	}
	hk_fig_goes_on = entry + 4;
	entry[0] = 0x58000050;  /* ldr x16, #8 */
	entry[1] = 0xd61f0200;  /* br x16 */
	*(void **)(void *)(entry + 2) = (void *)fig_entry;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(entry, 16);
	dprintf(console, "figerr[%s %d]: watching FigSignalErrorAt, slide %#lx\n", getprogname(), getpid(), (unsigned long)slide);
	if (strcmp(getprogname(), "MobileSlideShow") == 0) {
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
			px_watch();
		});
	}

	/* (VideoToolbox's own function, which has no name to look up: by its address, and only if it is what it was on 20F66) */
	uint32_t *celeste = (uint32_t *)(0x194f8dff0UL + slide);
	const vm_address_t celeste_page = (vm_address_t)celeste & ~(vm_address_t)(vm_page_size - 1);

	if (memcmp(celeste, celeste_expected, sizeof(celeste_expected)) != 0 || (vm_address_t)(celeste + 4) > celeste_page + vm_page_size ||
	    vm_protect(mach_task_self(), celeste_page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		dprintf(console, "figerr[%s]: vt_Rotate_Celeste left alone\n", getprogname());
		return;
	}
	hk_celeste_goes_on = celeste + 4;
	celeste[0] = 0x58000050;        /* ldr x16, #8 */
	celeste[1] = 0xd61f0200;        /* br x16 */
	*(void **)(void *)(celeste + 2) = (void *)celeste_entry;
	vm_protect(mach_task_self(), celeste_page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(celeste, 16);
}
