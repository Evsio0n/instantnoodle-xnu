/*
 * libhackios-call.dylib, for callservicesd: the earpiece and the speaker of a cellular call as iOS's two audio routes.
 *
 * A call's sound does not pass through iOS here: the audio DSP and the modem exchange it between themselves, and the kernel
 * chooses where it is heard (bsd/dev/hk_mic.c: kern.hackios.voice_speaker, 0 the earpiece, 1 the speakers; every call starts
 * at the earpiece). iOS's own routes come from mediaserverd, which knows one output on this phone, so the in-call screen's
 * audio button had nothing to choose between. callservicesd asks TUAudioSystemController (TelephonyUtilities) which routes a
 * call may use and tells it which one the user picked; this library answers those three questions with a receiver and a
 * speaker made the way mediaserverd describes routes, and a pick writes the kernel's switch:
 *     -pickableRoutesForActiveCall, -_pickableRoutesForPhoneCallWithForceNewRequest:, -bestGuessPickableRoutesForAnyCall
 *     -pickRoute:error:
 * The design is the OnePlus 8 Pro port's (its notes, "Earpiece and speaker button"; its library is not public: this one is
 * written from that description). The route dictionaries' keys are read from MediaExperience's own constants
 * (AVSystemController_RouteDescriptionKey_*), which are the ones TUAudioRoute reads them with.
 *
 * Nothing of callservicesd is changed but those four methods' implementations, by name, through the Objective-C runtime; no
 * address is used. Inserted by the kernel for the program named in etc/inject.conf.
 */
#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <os/log.h>
#include <sys/sysctl.h>

static NSString *
constant(const char *name, NSString *otherwise)
{
	void *p = dlsym(RTLD_DEFAULT, name);
	NSString *value = p != NULL ? *(NSString *__unsafe_unretained *)p : nil;

	return value != nil ? value : otherwise;
}

/* -1: the kernel has no such switch (no voice=3: nothing of this library applies) */
static int
speaker_now(void)
{
	int value = 0;
	size_t n = sizeof(value);

	return sysctlbyname("kern.hackios.voice_speaker", &value, &n, NULL, 0) == 0 ? (value != 0) : -1;
}

static NSArray *
call_routes(void)
{
	int speaker = speaker_now();
	NSString *uid = constant("AVSystemController_RouteDescriptionKey_RouteUID", @"RouteUID");
	NSString *name = constant("AVSystemController_RouteDescriptionKey_RouteName", @"RouteName");
	NSString *av = constant("AVSystemController_RouteDescriptionKey_AVAudioRouteName", @"AVAudioRouteName");
	NSString *type = constant("AVSystemController_RouteDescriptionKey_RouteType", @"RouteType");
	NSString *picked = constant("AVSystemController_RouteDescriptionKey_RouteCurrentlyPicked", @"RouteCurrentlyPicked");
	NSString *usual = constant("AVSystemController_PickableRouteType_Default", @"Default");

	if (speaker < 0) {
		return nil;
	}
	return @[
		@{ uid: @"Built-In Receiver", name: @"iPhone", av: @"Receiver", type: usual, picked: @(speaker == 0) },
		@{ uid: @"Speaker", name: @"Speaker", av: @"Speaker", type: @"Override", picked: @(speaker == 1) },
	];
}

static void
replace(Class cls, const char *selector, id block)
{
	SEL sel = sel_registerName(selector);
	Method m = class_getInstanceMethod(cls, sel);

	if (m == NULL) {
		os_log(OS_LOG_DEFAULT, "hackios-call: TUAudioSystemController has no %{public}s: left alone", selector);
		return;
	}
	method_setImplementation(m, imp_implementationWithBlock(block));
}

static void
tell_routes_changed(id controller)
{
	dispatch_async(dispatch_get_main_queue(), ^{
		SEL sel = sel_registerName("_handlePickableRoutesDidChangeNotification:");

		if ([controller respondsToSelector:sel]) {
			((void (*)(id, SEL, id))objc_msgSend)(controller, sel, nil);
		}
	});
}

static void
install(void)
{
	Class cls = objc_getClass("TUAudioSystemController");
	static dispatch_source_t watcher;
	Method pick, active, phone, guess;
	IMP pick_before, active_before, phone_before, guess_before;

	if (cls == Nil || speaker_now() < 0) {
		os_log(OS_LOG_DEFAULT, "hackios-call: %{public}s: iOS's own routes stay", cls == Nil ? "no TUAudioSystemController here" : "the kernel has no voice_speaker (no call sound)");
		return;
	}
	pick = class_getInstanceMethod(cls, sel_registerName("pickRoute:error:"));
	active = class_getInstanceMethod(cls, sel_registerName("pickableRoutesForActiveCall"));
	phone = class_getInstanceMethod(cls, sel_registerName("_pickableRoutesForPhoneCallWithForceNewRequest:"));
	guess = class_getInstanceMethod(cls, sel_registerName("bestGuessPickableRoutesForAnyCall"));
	pick_before = pick != NULL ? method_getImplementation(pick) : NULL;
	active_before = active != NULL ? method_getImplementation(active) : NULL;
	phone_before = phone != NULL ? method_getImplementation(phone) : NULL;
	guess_before = guess != NULL ? method_getImplementation(guess) : NULL;
	replace(cls, "pickableRoutesForActiveCall", ^NSArray *(id self) {
		return call_routes() ?: (active_before != NULL ? ((NSArray *(*)(id, SEL))active_before)(self, sel_registerName("pickableRoutesForActiveCall")) : nil);
	});
	replace(cls, "_pickableRoutesForPhoneCallWithForceNewRequest:", ^NSArray *(id self, BOOL force) {
		return call_routes() ?: (phone_before != NULL ? ((NSArray *(*)(id, SEL, BOOL))phone_before)(self, sel_registerName("_pickableRoutesForPhoneCallWithForceNewRequest:"), force) : nil);
	});
	replace(cls, "bestGuessPickableRoutesForAnyCall", ^NSArray *(id self) {
		return call_routes() ?: (guess_before != NULL ? ((NSArray *(*)(id, SEL))guess_before)(self, sel_registerName("bestGuessPickableRoutesForAnyCall")) : nil);
	});
	replace(cls, "pickRoute:error:", ^BOOL (id self, id route, NSError **error) {
		NSString *av = constant("AVSystemController_RouteDescriptionKey_AVAudioRouteName", @"AVAudioRouteName");
		NSString *which = [route isKindOfClass:NSDictionary.class] ? [(NSDictionary *)route objectForKey:av] : nil;
		int value = [which isEqual:@"Speaker"] ? 1 : [which isEqual:@"Receiver"] ? 0 : -1;

		if (value < 0 && [route respondsToSelector:sel_registerName("isSpeaker")]) {
			value = ((BOOL (*)(id, SEL))objc_msgSend)(route, sel_registerName("isSpeaker")) ? 1 :
			    ((BOOL (*)(id, SEL))objc_msgSend)(route, sel_registerName("isReceiver")) ? 0 : -1;
		}
		if (value < 0 || speaker_now() < 0) {
			return pick_before != NULL ? ((BOOL (*)(id, SEL, id, NSError **))pick_before)(self, sel_registerName("pickRoute:error:"), route, error) : NO;
		}
		int e = sysctlbyname("kern.hackios.voice_speaker", NULL, NULL, &value, sizeof(value));

		os_log(OS_LOG_DEFAULT, "hackios-call: iOS picks the %{public}s for the call: the kernel %{public}s", value ? "speaker" : "earpiece", e == 0 ? "took it" : "did not take it");
		tell_routes_changed(self);
		return e == 0;
	});
	/* the kernel puts the next call back at the earpiece by itself: iOS is told when the switch has changed without it */
	watcher = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
	dispatch_source_set_timer(watcher, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 4);
	dispatch_source_set_event_handler(watcher, ^{
		static int was = -2;
		int now = speaker_now();

		if (now != was) {
			SEL shared = sel_registerName("sharedAudioSystemController");

			if (was != -2 && [(id)cls respondsToSelector:shared]) {
				tell_routes_changed(((id (*)(id, SEL))objc_msgSend)((id)cls, shared));
			}
			was = now;
		}
	});
	dispatch_resume(watcher);
	os_log(OS_LOG_DEFAULT, "hackios-call: a call's routes are the earpiece and the speaker (kern.hackios.voice_speaker)");
}

__attribute__((constructor))
static void
hackios_call_start(void)
{
	/* (after the program's own start: its classes are there, and the main queue runs) */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		install();
	});
}
