/*
 * location_test: where the phone is, as iOS's location daemon says it (next-features' Wi-Fi location: locationd, Apple's positioning
 * service over a joined network, no modem). Signed with src/location_test.entitlements on the host (codesign), since locationd
 * takes its preferences and an effective bundle only from a client with those entitlements.
 *
 * usage: location_test STEP...
 *   wait SECONDS
 *   enable           Location Services on (what Settings > Privacy & Security does) and this program's bundle allowed
 *   state            what CoreLocation says of the service and of this client
 *   fix SECONDS      ask for a position and print what arrives, for at most SECONDS
 *   watch SECONDS    every position for SECONDS at the best accuracy, and at the end how far they spread: is the position jumping?
 *   wait-net SECONDS until en0 has an address, for at most SECONDS (the owner joins Wi-Fi by hand; the address is not printed)
 *   alarm SECONDS    the program's end, from now on, instead of 100 s
 *   files            where the daemon keeps its switch: its preferences' files under /var, and the keys in them that say "Enabled"
 * Each step says what it is about to ask first, and the whole program ends by itself after 100 s: a call into CoreLocation has been seen not to come back (run 013103).
 * A position itself never leaves this program: accuracy, age and distances between positions only. No coordinates, no network's name or address.
 */
#import <Foundation/Foundation.h>
#import <CoreLocation/CoreLocation.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

@interface CLLocationManager (HKPrivate)
+ (void)setLocationServicesEnabled:(BOOL)enabled;
+ (void)setAuthorizationStatusByType:(int)status forBundleIdentifier:(NSString *)bundle;
+ (int)authorizationStatusForBundleIdentifier:(NSString *)bundle;
- (instancetype)initWithEffectiveBundleIdentifier:(NSString *)bundle;
@end

@interface CLLocation (HKPrivate)
- (int)type;    /* where it came from, as locationd says: private, its numbers not documented; printed as it is */
@end

static NSString *const bundle_id = @"com.apple.Maps";

@interface Listener : NSObject <CLLocationManagerDelegate>
@property (nonatomic) int fixes;
@property (nonatomic, strong) CLLocation *first, *last;
@property (nonatomic) double best_accuracy, worst_accuracy, furthest, biggest_step;
@property (nonatomic) NSMutableDictionary<NSNumber *, NSNumber *> *types;
@end

@implementation Listener
- (void)locationManager:(CLLocationManager *)manager didUpdateLocations:(NSArray<CLLocation *> *)locations
{
	/* (the position itself is the owner's: its accuracy, age and how far it moved are what the test needs, and it stays out of the logs) */
	for (CLLocation *l in locations) {
		double from_first = self.first != nil ? [l distanceFromLocation:self.first] : 0;
		double from_last = self.last != nil ? [l distanceFromLocation:self.last] : 0;
		int type = [l respondsToSelector:@selector(type)] ? l.type : -1;

		printf("location_test: a position: accuracy %.0f m, %.0f s old, altitude accuracy %.0f m, %.0f m from the first, %.0f m from the last, type %d\n", l.horizontalAccuracy,
		    -l.timestamp.timeIntervalSinceNow, l.verticalAccuracy, from_first, from_last, type);
		if (l.horizontalAccuracy >= 0) {
			if (self.first == nil) {
				self.first = l;
				self.best_accuracy = self.worst_accuracy = l.horizontalAccuracy;
				self.types = [NSMutableDictionary new];
			}
			self.best_accuracy = MIN(self.best_accuracy, l.horizontalAccuracy);
			self.worst_accuracy = MAX(self.worst_accuracy, l.horizontalAccuracy);
			self.furthest = MAX(self.furthest, from_first);
			self.biggest_step = MAX(self.biggest_step, from_last);
			self.types[@(type)] = @(self.types[@(type)].intValue + 1);
			self.last = l;
		}
		self.fixes++;
	}
	fflush(stdout);
}

- (void)locationManager:(CLLocationManager *)manager didFailWithError:(NSError *)error
{
	printf("location_test: failed: %s %ld\n", error.domain.UTF8String, (long)error.code);
	fflush(stdout);
}

- (void)locationManagerDidChangeAuthorization:(CLLocationManager *)manager
{
	printf("location_test: authorization now %d\n", (int)manager.authorizationStatus);
	fflush(stdout);
}
@end

/* a call that may not come back (run 013625, +locationServicesEnabled on the main thread): at most 15 s, and the next step goes on */
static void
timed(const char *name, void (^call)(void))
{
	dispatch_semaphore_t done = dispatch_semaphore_create(0);

	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		call();
		dispatch_semaphore_signal(done);
	});
	for (int i = 0; i < 150; i++) {
		if (dispatch_semaphore_wait(done, DISPATCH_TIME_NOW) == 0) {
			printf("location_test: %s came back after %d ms\n", name, i * 100);
			fflush(stdout);
			return;
		}
		[NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
	}
	printf("location_test: %s did NOT come back in 15 s\n", name);
	fflush(stdout);
}

static void
show_preferences(NSString *folder)
{
	for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:folder error:NULL]) {
		NSString *path = [folder stringByAppendingPathComponent:name];

		if ([name containsString:@"locationd"]) {
			NSDictionary *d = [NSDictionary dictionaryWithContentsOfFile:path];

			printf("location_test: %s: %lu keys\n", path.UTF8String, (unsigned long)d.count);
			for (NSString *key in d) {
				if ([key containsString:@"Enabled"]) {
					printf("location_test:   %s = %s\n", key.UTF8String, [d[key] description].UTF8String);
				}
			}
		}
	}
}

static bool
network_is_up(void)
{
	struct ifaddrs *all, *one;
	bool up = false;

	if (getifaddrs(&all) != 0) {
		return false;
	}
	for (one = all; one != NULL; one = one->ifa_next) {
		if (strcmp(one->ifa_name, "en0") == 0 && one->ifa_addr != NULL && one->ifa_addr->sa_family == AF_INET &&
		    (ntohl(((struct sockaddr_in *)one->ifa_addr)->sin_addr.s_addr) >> 16) != 0xa9fe) {
			up = true;
		}
	}
	freeifaddrs(all);
	return up;
}

int
main(int argc, char **argv)
{
	alarm(100);
	@autoreleasepool {
		Listener *listener = [Listener new];
		CLLocationManager *manager = nil;

		for (int i = 1; i < argc; i++) {
			printf("location_test: step %s\n", argv[i]);
			fflush(stdout);
			if (strcmp(argv[i], "wait") == 0 && i + 1 < argc) {
				[NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:atof(argv[++i])]];
			} else if (strcmp(argv[i], "alarm") == 0 && i + 1 < argc) {
				alarm((unsigned)atoi(argv[++i]));
			} else if (strcmp(argv[i], "wait-net") == 0 && i + 1 < argc) {
				double most = atof(argv[++i]), spent = 0;

				for (; spent < most && !network_is_up(); spent += 1) {
					[NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1]];
				}
				printf("location_test: the network is %s after %.0f s\n", network_is_up() ? "up" : "NOT up", spent);
			} else if (strcmp(argv[i], "watch") == 0 && i + 1 < argc) {
				double most = atof(argv[++i]);
				NSDate *began = [NSDate date];

				if (manager == nil) {
					manager = [[CLLocationManager alloc] initWithEffectiveBundleIdentifier:bundle_id];
					manager.delegate = listener;
				}
				manager.desiredAccuracy = kCLLocationAccuracyBest;
				manager.distanceFilter = kCLDistanceFilterNone;
				[manager startUpdatingLocation];
				while (-began.timeIntervalSinceNow < most) {
					[NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1]];
				}
				[manager stopUpdatingLocation];
				printf("location_test: in %.0f s: %d positions; accuracy %.0f to %.0f m; furthest from the first %.0f m; the biggest step %.0f m; by type:", most, listener.fixes,
				    listener.best_accuracy, listener.worst_accuracy, listener.furthest, listener.biggest_step);
				for (NSNumber *type in listener.types) {
					printf(" %d: %d", type.intValue, listener.types[type].intValue);
				}
				printf("\n");
			} else if (strcmp(argv[i], "enable") == 0) {
				timed("setLocationServicesEnabled", ^{ [CLLocationManager setLocationServicesEnabled:YES]; });
				timed("setAuthorizationStatusByType", ^{ [CLLocationManager setAuthorizationStatusByType:3 forBundleIdentifier:bundle_id]; });
			} else if (strcmp(argv[i], "state") == 0) {
				timed("locationServicesEnabled", ^{ printf("location_test: services %s\n", CLLocationManager.locationServicesEnabled ? "on" : "off"); });
				timed("authorizationStatus", ^{ printf("location_test: %s: authorization %d\n", bundle_id.UTF8String,
				    [CLLocationManager authorizationStatusForBundleIdentifier:bundle_id]); });
			} else if (strcmp(argv[i], "files") == 0) {
				show_preferences(@"/var/mobile/Library/Preferences");
				show_preferences(@"/var/root/Library/Preferences");
				show_preferences(@"/var/db/locationd/Library/Preferences/ByHost");
				show_preferences(@"/var/mobile/Library/Preferences/ByHost");
				show_preferences(@"/var/root/Library/Preferences/ByHost");
			} else if (strcmp(argv[i], "fix") == 0 && i + 1 < argc) {
				double most = atof(argv[++i]);

				if (manager == nil) {
					manager = [[CLLocationManager alloc] initWithEffectiveBundleIdentifier:bundle_id];
					manager.delegate = listener;
					manager.desiredAccuracy = kCLLocationAccuracyHundredMeters;
				}
				[manager startUpdatingLocation];
				for (double spent = 0; spent < most && listener.fixes == 0; spent += 1) {
					[NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1]];
				}
				[manager stopUpdatingLocation];
				printf("location_test: %d positions in at most %.0f s\n", listener.fixes, most);
			} else {
				printf("location_test: what is %s?\n", argv[i]);
				return 2;
			}
			fflush(stdout);
		}
	}
	return 0;
}
