/* Exercise the 20F66 discovery contract via ordinary IOKit APIs. No user
 * client is opened. Count initial registry entries and later notifications
 * separately; only property type/equality and counts leave the process. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdbool.h>
#include <stdio.h>

struct watch {
    const char *label;
    const char *class_name;
    const char *event;
    CFStringRef protocol;
    io_iterator_t iterator;
    unsigned initial, events, wrong_property;
    bool invalid, terminating;
};

static void
collect(struct watch *watch, io_iterator_t iterator, unsigned *count)
{
    io_object_t entry;
    while ((entry = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        ++*count;
        /* A terminated entry can already have lost its registry properties. */
        if (watch->protocol && !watch->terminating) {
            CFTypeRef property = IORegistryEntryCreateCFProperty(
                entry, CFSTR("ABPInterfaceProtocol"), NULL, 0);
            if (!property || CFGetTypeID(property) != CFStringGetTypeID() ||
                !CFEqual(property, watch->protocol))
                ++watch->wrong_property;
            if (property) CFRelease(property);
        }
        IOObjectRelease(entry);
    }
    if (!IOIteratorIsValid(iterator)) watch->invalid = true;
}

static void
notification(void *context, io_iterator_t iterator)
{
    struct watch *watch = context;
    collect(watch, iterator, &watch->events);
}

int main(void)
{
    const char *iface = "AppleBasebandPCIInterface";
    struct watch watches[] = {
        {.label="host_control", .class_name="IOResources", .event=kIOFirstMatchNotification},
        {.label="qmux1_first", .class_name=iface, .event=kIOFirstMatchNotification,
         .protocol=CFSTR("qmux1")},
        {.label="qmux1_terminate", .class_name=iface, .event=kIOTerminatedNotification,
         .protocol=CFSTR("qmux1"), .terminating=true},
        {.label="negative", .class_name=iface, .event=kIOFirstMatchNotification,
         .protocol=CFSTR("hk-nonexistent-abp-protocol")},
    };
    const size_t n = sizeof(watches)/sizeof(watches[0]);
    IONotificationPortRef notify = IONotificationPortCreate(MACH_PORT_NULL);
    if (!notify) return 1;
    CFRunLoopSourceRef source = IONotificationPortGetRunLoopSource(notify);
    if (!source) {
        IONotificationPortDestroy(notify);
        return 1;
    }
    CFRunLoopRef runloop = CFRunLoopGetCurrent();
    CFRunLoopAddSource(runloop, source, kCFRunLoopDefaultMode);
    bool ok = true;
    for (size_t i = 0; i < n; ++i) {
        struct watch *watch = &watches[i];
        CFMutableDictionaryRef dict = IOServiceMatching(watch->class_name);
        if (!dict) { ok = false; break; }
        if (watch->protocol)
            CFDictionarySetValue(dict, CFSTR("ABPInterfaceProtocol"), watch->protocol);
        kern_return_t kr = IOServiceAddMatchingNotification(notify, watch->event, dict,
            notification, watch, &watch->iterator);
        if (kr != KERN_SUCCESS || !watch->iterator) {
            fprintf(stderr, "%s registration failed kr=%d\n", watch->label, kr);
            ok = false;
            break;
        }
        collect(watch, watch->iterator, &watch->initial); /* arms the iterator */
    }
    if (ok) (void)CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.10, false);
    CFRunLoopRemoveSource(runloop, source, kCFRunLoopDefaultMode);
    for (size_t i = 0; i < n; ++i) {
        struct watch *watch = &watches[i];
        printf("%s initial=%u later=%u wrong_property=%u invalid=%u\n",
            watch->label, watch->initial, watch->events, watch->wrong_property, watch->invalid);
        ok = ok && !watch->wrong_property && !watch->invalid;
        if (watch->iterator) IOObjectRelease(watch->iterator);
    }
    IONotificationPortDestroy(notify);
    return ok && watches[0].initial > 0 && !watches[3].initial && !watches[3].events ? 0 : 1;
}
