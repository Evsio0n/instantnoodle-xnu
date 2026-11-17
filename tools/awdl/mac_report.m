/* macOS-only telemetry from the two existing awdl0 IOReport subgroups.
 * No Wi-Fi manager, capture socket, peer identifier or group change. */
#import <Foundation/Foundation.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef CFTypeRef IOReportSub;
extern CFMutableDictionaryRef IOReportCopyChannelsInGroup(CFStringRef, CFStringRef, uint64_t, uint64_t, uint64_t);
extern IOReportSub IOReportCreateSubscription(void *, CFMutableDictionaryRef, CFMutableDictionaryRef *, uint64_t, CFTypeRef);
extern CFDictionaryRef IOReportCreateSamples(IOReportSub, CFMutableDictionaryRef, CFTypeRef);
extern void IOReportIterate(CFDictionaryRef, int (^)(CFDictionaryRef));
extern CFStringRef IOReportChannelGetChannelName(CFDictionaryRef);
extern int64_t IOReportSimpleGetIntegerValue(CFDictionaryRef, void *);
extern int IOReportStateGetCount(CFDictionaryRef);
extern int IOReportStateGetCurrent(CFDictionaryRef);
extern CFStringRef IOReportStateGetNameForIndex(CFDictionaryRef, int);
extern uint64_t IOReportStateGetResidency(CFDictionaryRef, int);

struct report {
    CFMutableDictionaryRef desired, subscribed;
    IOReportSub subscription;
};

static void prepare(struct report *r, CFStringRef subgroup)
{
    r->desired = IOReportCopyChannelsInGroup(CFSTR("Interface awdl0"), subgroup, 0, 0, 0);
    if (r->desired)
        r->subscription = IOReportCreateSubscription(NULL, r->desired, &r->subscribed, 0, NULL);
}

static NSDictionary *sample(struct report *r, BOOL schedule)
{
    if (!r->desired) return @{@"status": @"group_unavailable"};
    if (!r->subscription || !r->subscribed) return @{@"status": @"subscription_unavailable"};
    CFDictionaryRef samples = IOReportCreateSamples(r->subscription, r->subscribed, NULL);
    if (!samples) return @{@"status": @"sample_unavailable"};
    NSMutableDictionary *values = [NSMutableDictionary dictionary];
    IOReportIterate(samples, ^int(CFDictionaryRef channel) {
        NSString *name = (__bridge NSString *)IOReportChannelGetChannelName(channel);
        if (schedule && [name isEqual:@"AWDL Schedule State"]) {
            int count = IOReportStateGetCount(channel);
            if (count > 0 && count <= 128) {
                NSMutableArray *residencies = [NSMutableArray arrayWithCapacity:count];
                for (int index = 0; index < count; index++)
                    [residencies addObject:@(IOReportStateGetResidency(channel, index))];
                values[name] = residencies;
                int current = IOReportStateGetCurrent(channel);
                if (current >= 0 && current < count) {
                    values[@"current_index"] = @(current);
                    NSString *label = (__bridge NSString *)IOReportStateGetNameForIndex(channel, current);
                    if (label.length && label.length <= 48)
                        values[@"current_state_name"] = label;
                }
            }
        } else if (!schedule && [@[@"UMI Pending", @"UMI Failed", @"UMI Sent Successfully"] containsObject:name]) {
            int64_t value = IOReportSimpleGetIntegerValue(channel, NULL);
            if (value != INT64_MIN) values[name] = @(value);
        }
        return 0;
    });
    CFRelease(samples);
    return @{@"status": values.count ? @"sampled" : @"no_values", @"values":values};
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        char *end = NULL;
        long interval = argc == 2 ? strtol(argv[1], &end, 10) : 1;
        if (argc > 2 || (argc == 2 && (!end || *end)) || interval < 1 || interval > 20) return 2;
        struct report umi = {0}, schedule = {0};
        prepare(&umi, CFSTR("UMI Statistics"));
        prepare(&schedule, CFSTR("AWDL Schedule"));
        for (int n = 0; n < 2; n++) {
            NSDictionary *umiSample = sample(&umi, NO), *scheduleSample = sample(&schedule, YES);
            NSDictionary *row = @{@"uptime_seconds":@(NSProcessInfo.processInfo.systemUptime),
                @"utc_epoch_seconds":@(NSDate.date.timeIntervalSince1970), @"uid":@(getuid()),
                @"group":@"Interface awdl0", @"UMI Statistics":umiSample, @"AWDL Schedule":scheduleSample};
            NSData *json = [NSJSONSerialization dataWithJSONObject:row options:0 error:nil];
            if (!json || fwrite(json.bytes, 1, json.length, stdout) != json.length) return 1;
            putchar('\n');
            fflush(stdout);
            if (!n) {
                if (!umi.subscription && !schedule.subscription) break;
                sleep((unsigned)interval);
            }
        }
        struct report *reports[] = {&umi, &schedule};
        for (unsigned i = 0; i < 2; i++) {
            struct report *r = reports[i];
            if (r->subscription) CFRelease(r->subscription);
            if (r->subscribed) CFRelease(r->subscribed);
            if (r->desired) CFRelease(r->desired);
        }
    }
    return 0;
}
