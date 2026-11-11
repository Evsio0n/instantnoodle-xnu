/* Read-only 20F66 libIOABP service matching check. Run as mobile. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdbool.h>
#include <stdio.h>

struct match_result {
    kern_return_t status;
    unsigned entries;
    unsigned wrong_property;
};

static struct match_result
query(const char *class_name, CFStringRef protocol)
{
    struct match_result result = { .status = KERN_RESOURCE_SHORTAGE };
    CFMutableDictionaryRef dict = IOServiceMatching(class_name);
    if (!dict)
        return result;
    if (protocol)
        CFDictionarySetValue(dict, CFSTR("ABPInterfaceProtocol"), protocol);
    io_iterator_t iterator = IO_OBJECT_NULL;
    result.status = IOServiceGetMatchingServices(MACH_PORT_NULL, dict, &iterator);
    if (result.status != KERN_SUCCESS || !iterator)
        return result;

    io_object_t entry;
    while ((entry = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        result.entries++;
        if (protocol) {
            CFTypeRef value = IORegistryEntryCreateCFProperty(entry,
                CFSTR("ABPInterfaceProtocol"), NULL, 0);
            if (!value || CFGetTypeID(value) != CFStringGetTypeID() ||
                !CFEqual(value, protocol))
                result.wrong_property++;
            if (value)
                CFRelease(value);
        }
        IOObjectRelease(entry);
    }
    if (!IOIteratorIsValid(iterator))
        result.status = kIOReturnNotReady;
    IOObjectRelease(iterator);
    return result;
}

int main(void)
{
    const char *interface = "AppleBasebandPCIInterface";
    const struct { const char *label; CFStringRef key; unsigned maximum; } channels[] = {
        { "pbl", CFSTR("pbl"), 1 }, { "sahara", CFSTR("sahara"), 1 },
        { "qmux1", CFSTR("qmux1"), 1 }, { "qmux2", CFSTR("qmux2"), 1 },
        { "negative", CFSTR("hk-nonexistent-abp-protocol"), 0 },
    };
    struct match_result resources = query("IOResources", NULL);
    struct match_result control = query("AppleBasebandPCIControl", NULL);
    struct match_result interfaces = query(interface, NULL);
    bool observed = !resources.status && resources.entries && !control.status &&
        !interfaces.status;
    printf("abp-match: resources=%u resources_kr=%d control=%u control_kr=%d "
        "interfaces=%u interfaces_kr=%d end\n", resources.entries, resources.status,
        control.entries, control.status, interfaces.entries, interfaces.status);
    for (unsigned i = 0; i < sizeof(channels)/sizeof(channels[0]); i++) {
        struct match_result result = query(interface, channels[i].key);
        printf("abp-match: protocol=%s entries=%u wrong_property=%u kr=%d end\n",
            channels[i].label, result.entries, result.wrong_property, result.status);
        observed = observed && !result.status && !result.wrong_property &&
            result.entries <= channels[i].maximum;
    }
    /* Presence is a result, not a requirement. Never open the user client. */
    return observed ? 0 : 1;
}
