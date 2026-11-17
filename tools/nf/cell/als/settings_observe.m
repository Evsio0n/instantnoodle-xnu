/* This ordinary mobile observer can read the physical registry match,
 * the Settings visibility gate and the current preference. Only the
 * entitled stock Preferences process can verify its switch is operable. */
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

struct sample {
    uint64_t uptimeUs, sensorTicks;
    uint32_t sequence, luxBits;
};
struct observation {
    int any, sensor, calibrated, visible, value;
};

static struct observation readState(void) {
    static const char *mgPath = "/usr/lib/libMobileGestalt.dylib";
    static const char *axPath = "/usr/lib/libAccessibility.dylib";
    static bool (*mg)(CFStringRef);
    static unsigned char (*ax)(void);
    struct observation o = {.visible = -1, .value = -1};
    NSDictionary *match = @{@"IOPropertyMatch": @{@"PrimaryUsagePage": @0xff00, @"PrimaryUsage": @4}};
    io_service_t first = IOServiceGetMatchingService(MACH_PORT_NULL,
        (__bridge_retained CFDictionaryRef)match);
    o.any = first != IO_OBJECT_NULL;
    if (first) IOObjectRelease(first);

    io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL,
        IOServiceMatching("HKFrontLightSensor"));
    if (!service) return o;
    o.sensor = 1;
    CFTypeRef raw = IORegistryEntryCreateCFProperty(service, CFSTR("CalibratedSample"), NULL, 0);
    if (raw && CFGetTypeID(raw) == CFDataGetTypeID() && CFDataGetLength(raw) == sizeof(struct sample)) {
        struct sample s;
        CFDataGetBytes(raw, CFRangeMake(0, sizeof(s)), (UInt8 *)&s);
        o.calibrated = s.sensorTicks && s.uptimeUs && s.sequence && s.luxBits < 0x4f800000u;
    }
    if (raw) CFRelease(raw);
    IOObjectRelease(service);
    if (!mg) mg = dlsym(dlopen(mgPath, RTLD_LAZY | RTLD_LOCAL), "MGGetBoolAnswer");
    if (!ax) ax = dlsym(dlopen(axPath, RTLD_LAZY | RTLD_LOCAL), "_AXSAutoBrightnessEnabled");
    if (mg) o.visible = mg(CFSTR("ambient-light-sensor"));
    if (ax) {
        unsigned value = ax();
        if (value <= 1) o.value = value;
    }
    return o;
}

int main(void) {
    if (getuid() != 501 || geteuid() != 501) return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct observation previous = {0};
    for (unsigned i = 0; i < 900; ++i) {
        @autoreleasepool {
            struct observation o = readState();
            if (!i || memcmp(&previous, &o, sizeof(o)) || i % 30 == 0)
                printf("front-light-ui: usage_match=%d stk33502=%d calibrated=%d MG_ambient=%d BKS_enabled=unknown_unentitled AX_value=%d uid=501\n",
                       o.any, o.sensor, o.calibrated, o.visible, o.value);
            previous = o;
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.0]];
        }
    }
    return 0;
}
