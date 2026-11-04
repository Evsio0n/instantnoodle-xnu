/*
 * libhackios-camera.dylib: code of ours inside the Camera application (CameraUI.framework), for what a
 * phone's cameras are and an iPhone X's are not (build.sh's INJECT puts it in the program "Camera").
 * On any board but the OnePlus 8 it does nothing at all.
 *
 * The zoom button. An iPhone X's back camera is two, the wide one and the telephoto one, and its
 * button ("1x") goes from the one to the other. The OnePlus 8's Back Camera is the wide IMX586 alone
 * (camera_device.c), and CameraUI then puts the button in its single-camera-plus-toggle mode
 * (-[CAMZoomControl _handleButtonTappedForSingleCameraZoomPlusToggle], CameraUI of iOS 16.5):
 *   - a tap while the button shows the smallest zoom asks the delegate to toggle,
 *     -[CAMViewfinderViewController toggleBetweenZoomFactorsForZoomControl:], which for the back camera
 *     changes the *device* (-_toggleBetweenBackFacingZoomFactorsForZoomControl: and
 *     -_handleUserChangedToMode:device:), to the telephoto one -- which is not there, so nothing happened
 *     (OnePlus 8 run 131307: two taps, no zoom asked for; the owner: the button did not become 2x);
 *   - a tap at any other zoom sets the smallest one back, through
 *     -[CAMZoomControl _setZoomFactor:interactionType:shouldNotifyDelegate:] with interaction 1, a
 *     button's, for which -[CAMViewfinderViewController zoomControl:didChangeZoomFactor:interactionType:]
 *     ramps the camera's zoom (-_changeToZoomFactor:rampDuration:zoomRampTuning:shouldAnimate:).
 * Ours: the back camera's toggle sets 2x in that same way, and the control's own second tap takes it back
 * to 1x. 2x and more is the IMX586's mode 6, the middle of the sensor at its full resolution -- a real 2x,
 * not a blown-up 1x (camera_device.c asks the kernel for it when the zoom reaches 2). The front camera,
 * and a back camera that should not zoom now (the original says why in Camera's log), keep the original.
 * [observed: CameraUI's code read in the dyld shared cache of iOS 16.5 (20F66); the hook untested on
 * the phone when written]
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

static void
say(const char *format, ...)
{
	char line[300];
	va_list args;
	int n, fd;

	va_start(args, format);
	n = vsnprintf(line, sizeof(line) - 1, format, args);
	va_end(args);
	if (n <= 0) {
		return;
	}
	n = n > (int)sizeof(line) - 2 ? (int)sizeof(line) - 2 : n;
	line[n++] = '\n';
	fd = open("/dev/console", O_WRONLY);
	if (fd >= 0) {
		(void)write(fd, "redwood: camera app: ", 21);
		(void)write(fd, line, (size_t)n);
		close(fd);
	}
}

/* a boot-arg as a whole word of kern.bootargs ("cam_05x=0", not inside "xcam_05x=00") */
static bool
boot_arg_set(const char *what)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1, n = strlen(what);

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return false;
	}
	for (const char *at = arguments; (at = strstr(at, what)) != NULL; at += n) {
		if ((at == arguments || at[-1] == ' ') && (at[n] == ' ' || at[n] == 0)) {
			return true;
		}
	}
	return false;
}

static bool
on_instantnoodle(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;

	return sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "board=instantnoodle") != NULL;
}

static void (*toggle_original)(id, SEL, id);

/* -[CAMViewfinderViewController toggleBetweenZoomFactorsForZoomControl:] */
static void
toggle_zoom(id self, SEL cmd, id control)
{
	SEL set_zoom = sel_registerName("_setZoomFactor:interactionType:shouldNotifyDelegate:");
	long position = ((long (*)(id, SEL))objc_msgSend)(self, sel_registerName("_currentDevicePosition"));
	id graph = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("_currentGraphConfiguration"));
	bool wanted = ((bool (*)(id, SEL, id))objc_msgSend)(self, sel_registerName("_wantsZoomControlForGraphConfiguration:"), graph);

	/* position 0: the back camera (1 is the front one) */
	if (position == 0 && wanted && control != NULL && class_respondsToSelector(object_getClass(control), set_zoom)) {
		say("the zoom button: 2x, the wide camera's middle (in place of an iPhone X's telephoto camera)");
		((void (*)(id, SEL, double, long, bool))objc_msgSend)(control, set_zoom, 2.0, 1, true);
		return;
	}
	toggle_original(self, cmd, control);
}

/* Put ours in the place of a method, keeping the one that was there. */
static bool
replace(const char *name, const char *selector, IMP ours, void *theirs)
{
	Class class = objc_getClass(name);
	Method method = class != NULL ? class_getInstanceMethod(class, sel_registerName(selector)) : NULL;

	if (method == NULL) {
		say("-[%s %s] is not there: left as it is", name, selector);
		return false;
	}
	if (method_getImplementation(method) != ours) {
		*(IMP *)theirs = method_setImplementation(method, ours);
	}
	return true;
}


/*
 * The 0.5x: the OnePlus 8's ultra-wide IMX481 is a Back Ultra Wide Camera (AVCaptureDeviceTypeBuiltInUltraWideCamera)
 * of the capture stack (camera_device.c's third stream, inject_media.c's entry in AVCaptureSession.plist), but CameraUI
 * goes by a table of models (AVGestalt's, read in -[CAMCaptureCapabilities initWithHostProcess:]), where an iPhone X has
 * a telephoto and a dual camera and no ultra-wide. Its capabilities are made over after that init, by their ivars' names:
 * an ultra-wide at the back, no telephoto, no dual, no wide-dual or triple (devices this phone does not have), the zoom
 * platter, and the ultra-wide's field twice the wide one's (__backWideDualCameraSwitchOverZoomFactor, which
 * -superWideToWideZoomFactorForDevicePosition: answers for the back, and the AVGestalt ratio at 0x2a8, written only where
 * the ivar there is a double). CameraUI then emulates a wide-dual camera's zoom (+[CAMZoomControlUtilities
 * shouldEmulateWideDualCameraZoomForMode:...]): its dial 0.5x, 1x and on, the ultra-wide device below 1x
 * (+deviceForEmulatingZoomFactor:..., CAMCaptureDevice 5, BackSuperWide), the wide one from 1x; and a 2x point is put
 * into its zoom factors (+shouldApplyContinuousZoomForMode:...), where the wide camera's zoom of 2 is the IMX586's mode
 * 6. Another iOS than 16.5 (20F66): left alone. Side effect: no back dual camera, so Portrait at the back is CameraUI's
 * without one. [observed: CameraUI's code read in the dyld shared cache of iOS 16.5; the result: to be observed]
 */
static bool
on_ios_20f66(void)
{
	char build[32] = "";
	size_t size = sizeof(build) - 1;

	return sysctlbyname("kern.osversion", build, &size, NULL, 0) == 0 && strcmp(build, "20F66") == 0;
}

static bool
set_ivar(id object, const char *name, char type, double value)
{
	Ivar ivar = class_getInstanceVariable(object_getClass(object), name);
	const char *encoding = ivar != NULL ? ivar_getTypeEncoding(ivar) : NULL;

	if (encoding == NULL || encoding[0] != type) {
		say("CAMCaptureCapabilities' %s is not there as a %c: left alone", name, type);
		return false;
	}
	if (type == 'B') {
		*(bool *)((char *)object + ivar_getOffset(ivar)) = value != 0;
	} else {
		*(double *)((char *)object + ivar_getOffset(ivar)) = value;
	}
	return true;
}

static id (*capabilities_init_original)(id, SEL, long);

/* -[CAMCaptureCapabilities initWithHostProcess:] */
static id
capabilities_init(id self, SEL cmd, long host)
{
	unsigned count = 0;
	Ivar *ivars;
	bool ratio = false;

	self = capabilities_init_original(self, cmd, host);
	if (self == nil) {
		return self;
	}
	set_ivar(self, "_backSuperWideSupported", 'B', 1);
	set_ivar(self, "_backTelephotoSupported", 'B', 0);
	set_ivar(self, "_backDualSupported", 'B', 0);
	set_ivar(self, "_backWideDualSupported", 'B', 0);
	set_ivar(self, "_backTripleCameraSupported", 'B', 0);
	set_ivar(self, "_zoomPlatterSupported", 'B', 1);
	set_ivar(self, "__backWideDualCameraSwitchOverZoomFactor", 'd', 2.0);
	ivars = class_copyIvarList(object_getClass(self), &count);
	for (unsigned i = 0; ivars != NULL && i < count; i++) {
		const char *encoding = ivar_getTypeEncoding(ivars[i]);

		if (ivar_getOffset(ivars[i]) == 0x2a8) {
			if (encoding != NULL && encoding[0] == 'd') {
				*(double *)((char *)self + 0x2a8) = 2.0;
				ratio = true;
				say("CAMCaptureCapabilities' %s (at 0x2a8, the ultra-wide's ratio) set to 2", ivar_getName(ivars[i]));
			} else {
				say("CAMCaptureCapabilities' ivar at 0x2a8 is %s (%s), not a double: left alone", ivar_getName(ivars[i]),
				    encoding ? encoding : "?");
			}
		}
	}
	free(ivars);
	say("the capabilities: an ultra-wide camera at the back (0.5x), no telephoto, no dual camera%s",
	    ratio ? "" : " (the 0x2a8 ratio not found)");
	/* (what this application's AVFoundation sees of it: the device CAMCaptureEngine will ask for) */
	{
		id *type = dlsym(RTLD_DEFAULT, "AVCaptureDeviceTypeBuiltInUltraWideCamera"), *video = dlsym(RTLD_DEFAULT, "AVMediaTypeVideo");
		Class device_class = objc_getClass("AVCaptureDevice");
		id device = type != NULL && video != NULL && device_class != NULL ?
		    ((id (*)(Class, SEL, id, id, long))objc_msgSend)(device_class,
		    sel_registerName("defaultDeviceWithDeviceType:mediaType:position:"), *type, *video, 1) : nil;
		id name = device != nil ? ((id (*)(id, SEL))objc_msgSend)(device, sel_registerName("localizedName")) : nil;
		char text[96] = "";

		if (name != nil) {
			CFStringGetCString((CFStringRef)name, text, sizeof(text), kCFStringEncodingUTF8);
		}
		say("AVCaptureDevice's ultra-wide camera at the back: %s", device != nil ? text : "none");
	}
	return self;
}

static bool (*continuous_zoom_original)(id, SEL, long, long, long, long, id *, id *);
static bool (*zoom_control_supported_original)(id, SEL);

/*
 * -[CAMCaptureCapabilities zoomControlSupported]: worked out at every call, dual || wide-dual || triple || the front's
 * ultra-wide -- not the back's: with no dual camera the zoom control went (run 150652: the pinch's slider alone). Yes
 * at the back with its ultra-wide.
 */
static bool
zoom_control_supported(id self, SEL cmd)
{
	(void)self;
	(void)cmd;
	return true;
}

static double
number_at(CFArrayRef a, CFIndex i)
{
	double v = 0;
	CFNumberRef n = CFArrayGetValueAtIndex(a, i);

	if (n != NULL && CFGetTypeID(n) == CFNumberGetTypeID()) {
		CFNumberGetValue(n, kCFNumberDoubleType, &v);
	}
	return v;
}

/* +[CAMZoomControlUtilities shouldApplyContinuousZoomForMode:device:videoConfiguration:videoStabilizationStrength:zoomFactors:displayZoomFactors:] */
static bool
continuous_zoom(id self, SEL cmd, long mode, long device, long video, long stabilization, id *factors, id *display)
{
	bool yes = continuous_zoom_original(self, cmd, mode, device, video, stabilization, factors, display);
	CFArrayRef zf, dzf;
	CFIndex n, one = -1;

	if (!yes || factors == NULL || display == NULL || *factors == nil || *display == nil) {
		return yes;
	}
	zf = (CFArrayRef)*factors;
	dzf = (CFArrayRef)*display;
	n = CFArrayGetCount(dzf);
	if (n != CFArrayGetCount(zf) || n < 2 || number_at(dzf, 0) >= 0.99) {
		return yes;             /* (not the ultra-wide's dial: no 0.5x at its start) */
	}
	for (CFIndex i = 0; i < n; i++) {
		double d = number_at(dzf, i);

		if (d > 1.99 && d < 2.01) {
			return yes;     /* (a 2x there already) */
		}
		if (d > 0.99 && d < 1.01) {
			one = i;
		}
	}
	if (one < 0 || number_at(dzf, n - 1) <= 2.0) {
		return yes;
	}
	double z2 = number_at(zf, one) * 2, d2 = number_at(dzf, one) * 2;
	CFNumberRef nz = CFNumberCreate(NULL, kCFNumberDoubleType, &z2), nd = CFNumberCreate(NULL, kCFNumberDoubleType, &d2);
	CFMutableArrayRef mz = CFArrayCreateMutableCopy(NULL, 0, zf), md = CFArrayCreateMutableCopy(NULL, 0, dzf);

	CFArrayInsertValueAtIndex(mz, one + 1, nz);
	CFArrayInsertValueAtIndex(md, one + 1, nd);
	CFRelease(nz);
	CFRelease(nd);
	*factors = (id)CFAutorelease(mz);
	*display = (id)CFAutorelease(md);
	static bool said;
	if (!said) {
		said = true;
		say("the back camera's zoom points: a 2x put in after 1x (zoom factor %.2f), %ld points in all", z2, (long)n + 1);
	}
	return yes;
}

static bool
replace_class_method(const char *name, const char *selector, IMP ours, void *theirs)
{
	Class class = objc_getClass(name);
	Method method = class != NULL ? class_getClassMethod(class, sel_registerName(selector)) : NULL;

	if (method == NULL) {
		say("+[%s %s] is not there: left as it is", name, selector);
		return false;
	}
	if (method_getImplementation(method) != ours) {
		*(IMP *)theirs = method_setImplementation(method, ours);
	}
	return true;
}

__attribute__((constructor))
static void
hk_camera_app_init(void)
{
	if (!on_instantnoodle()) {
		return;
	}
	if (replace("CAMViewfinderViewController", "toggleBetweenZoomFactorsForZoomControl:", (IMP)toggle_zoom, &toggle_original)) {
		say("the zoom button goes to 2x and back (the OnePlus 8's wide camera, its mode 6 at 2x)");
	}
	if (!on_ios_20f66()) {
		say("not iOS 16.5 (20F66): no 0.5x (the ultra-wide camera)");
		return;
	}
	/*
	 * (boot-arg cam_05x=0: the iPhone X's capabilities, Camera's 1x <-> 2x button above, no 0.5x. The 0.5x was behind
	 * cam_05x=1 while CameraUI hid its zoom control for want of -zoomControlSupported -- run 150652, the owner:
	 * "現在的相機是拖動條，不是1x 2x 0.5x"; with it, run 151700: ".5 1x 2", the IMX481 at 0.5x)
	 */
	if (boot_arg_set("cam_05x=0")) {
		say("cam_05x=0: no 0.5x (the ultra-wide camera); the 1x <-> 2x button");
		return;
	}
	if (replace("CAMCaptureCapabilities", "initWithHostProcess:", (IMP)capabilities_init, &capabilities_init_original) &&
	    replace("CAMCaptureCapabilities", "zoomControlSupported", (IMP)zoom_control_supported, &zoom_control_supported_original) &&
	    replace_class_method("CAMZoomControlUtilities",
	    "shouldApplyContinuousZoomForMode:device:videoConfiguration:videoStabilizationStrength:zoomFactors:displayZoomFactors:",
	    (IMP)continuous_zoom, &continuous_zoom_original)) {
		say("the back camera's zoom: 0.5x (the ultra-wide IMX481), 1x and 2x (the wide IMX586)");
	}
}
