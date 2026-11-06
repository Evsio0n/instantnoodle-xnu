/*
 * libhackios-gainmap.dylib: an HDR photo's gain map without CoreImage (K33).
 *
 * CoreImage on Metal stitches its kernels, -newLibraryWithStitchedDescriptor:, which this port's driver does not have: every image it
 * makes comes out empty, with "Metal library creation failed" in the process's log (ci_gain, runs 033712-034446), and a gain map
 * with nothing in it means no highlights. The gain map's own step is a copy, a turn and perhaps a scale, so here it is done on the
 * CPU (hk_gain_image.h) until CoreImage works, and this library goes. For a test root: etc/inject.conf names the programs.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#include "hk_gain_image.h"

extern const char *getprogname(void);

static CGImageRef
gain_now(id self, SEL cmd, CVPixelBufferRef buffer, unsigned orientation, double scale)
{
	(void)self;
	(void)cmd;
	return hk_gain_image(buffer, orientation, scale);
}

static void
gain_install(unsigned tries)
{
	Class class = objc_getClass("PFImageMetadata");
	Method method = class != Nil ? class_getClassMethod(class, sel_registerName("createImageRefFromAuxiliaryImagePixelBuffer:applyingOrientation:scaleFactor:")) : NULL;
	const int console = open("/dev/console", O_WRONLY);

	if (method != NULL) {
		method_setImplementation(method, (IMP)gain_now);
		dprintf(console, "gainmap[%s %d]: gain maps made here and not by CoreImage\n", getprogname(), getpid());
	} else if (tries > 0) {
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
			gain_install(tries - 1);
		});
	} else {
		dprintf(console, "gainmap[%s %d]: PFImageMetadata has no such method here; left alone\n", getprogname(), getpid());
	}
	if (console >= 0) {
		close(console);
	}
}

__attribute__((constructor)) static void
gain_start(void)
{
	dispatch_async(dispatch_get_main_queue(), ^{
		gain_install(20);
	});
}
