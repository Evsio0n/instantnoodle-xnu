/* A read-only title/value row on the iOS 16.5 Cellular Settings page, for
 * the OnePlus cellular driver. Preferences already loads libhackios-tweaks. */
#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <notify.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <unistd.h>

@protocol HKSpecifier <NSObject>
+ (id)preferenceSpecifierNamed:(NSString *)name target:(id)target set:(SEL)set
                           get:(SEL)get detail:(Class)detail cell:(NSInteger)cell edit:(Class)edit;
+ (id)emptyGroupSpecifier;
- (NSInteger)cellType;
- (NSString *)identifier;
- (void)setProperty:(id)value forKey:(NSString *)key;
@end
@protocol HKCellularList <NSObject>
- (id)specifierForID:(NSString *)identifier;
- (void)reloadSpecifier:(id)specifier animated:(BOOL)animated;
@end

static NSString *const rowID = @"HKServingNetwork";
static const char *const stateNotification = "com.hackios.telephony.public-state-changed";

static int number(const char *name, int unavailable)
{
    int n;
    size_t len = sizeof(n);
    return sysctlbyname(name, &n, &len, NULL, 0) == 0 && len == sizeof(n) ? n : unavailable;
}

static NSString *localized(NSString *key, NSString *fallback)
{
    return [NSBundle.mainBundle localizedStringForKey:key value:fallback table:@"Localizable"];
}

static void log_row(const char *event, int sim, int registered, BOOL hasName)
{
    int fd = open("/dev/console", O_WRONLY | O_NOCTTY);
    if (fd >= 0) {
        dprintf(fd, "cell-settings: %s sim=%d registered=%d has_name=%d\n", event, sim, registered, hasName);
        close(fd);
    }
}

@interface HKCellularNetworkRow : NSObject
@property (nonatomic, weak) id<HKCellularList> list;
@property (nonatomic, weak) id specifier;
@end

@implementation HKCellularNetworkRow {
    int _notification;
}

- (instancetype)init
{
    if ((self = [super init])) {
        _notification = -1;
        __weak HKCellularNetworkRow *weakSelf = self;
        int token;
        if (notify_register_dispatch(stateNotification, &token, dispatch_get_main_queue(), ^(int unused) {
            HKCellularNetworkRow *row = weakSelf;
            id<HKCellularList> list = row.list;
            id spec = row.specifier;
            if (spec && [list specifierForID:rowID] == spec) [list reloadSpecifier:spec animated:NO];
        }) == NOTIFY_STATUS_OK) _notification = token;
    }
    return self;
}
- (void)dealloc
{
    if (_notification >= 0) notify_cancel(_notification);
}

/* Prefer the same localized name as the stock registration cache. Validate it
 * against the current modem value because the appearance callback can run while
 * the CoreTelephony delegate is still invalidating a previous subscription. */
- (NSString *)networkValue:(id)specifier
{
    (void)specifier;
    int sim = number("kern.hackios.cellular_sim_state", 0);
    int reg = number("kern.hackios.cellular_registered", 0);
    NSString *unknown = localized(@"UNKNOWN_NUMBER", @"Unknown");
    if (sim != 3 || reg != 1 || number("kern.hackios.cellular_up", 0) != 1) {
        log_row("value", sim, reg, NO);
        return sim == 1 ? localized(@"NO_SIM", @"No SIM") : unknown;
    }
    char utf8[64] = {0};
    size_t n = sizeof(utf8);
    if (sysctlbyname("kern.hackios.cellular_operator", utf8, &n, NULL, 0) ||
        n < 2 || n > sizeof(utf8) || utf8[n-1] != 0 || !utf8[0]) {
        log_row("value", sim, reg, NO);
        return unknown;
    }
    NSString *name = [[NSString alloc] initWithBytes:utf8 length:n-1 encoding:NSUTF8StringEncoding];
    Class cacheClass = NSClassFromString(@"PSUICoreTelephonyRegistrationCache");
    Class contextClass = NSClassFromString(@"CTXPCServiceSubscriptionContext");
    SEL shared = NSSelectorFromString(@"sharedInstance"), slot = NSSelectorFromString(@"contextWithSlot:");
    SEL localizedName = NSSelectorFromString(@"localizedOperatorName:");
    if ([cacheClass respondsToSelector:shared] && [contextClass respondsToSelector:slot]) {
        id cache = ((id (*)(id, SEL))objc_msgSend)(cacheClass, shared);
        id context = ((id (*)(id, SEL, long long))objc_msgSend)(contextClass, slot, 1);
        if (context && [cache respondsToSelector:localizedName]) {
            id ct = ((id (*)(id, SEL, id))objc_msgSend)(cache, localizedName, context);
            if ([ct isKindOfClass:NSString.class] && [ct isEqualToString:name]) name = ct;
        }
    }
    sim = number("kern.hackios.cellular_sim_state", 0);
    reg = number("kern.hackios.cellular_registered", 0);
    BOOL valid = sim == 3 && reg == 1 && name.length != 0;
    log_row("value", sim, reg, valid);
    if (sim == 1) return localized(@"NO_SIM", @"No SIM");
    return valid ? name : unknown;
}
@end

static void (*originalFinishList)(id, SEL, NSMutableArray *);
static char ownerKey;

static void finishList(id<HKCellularList> list, SEL cmd, NSMutableArray<id<HKSpecifier>> *specifiers)
{
    Class<HKSpecifier> cls = (Class<HKSpecifier>)NSClassFromString(@"PSSpecifier");
    if ([specifiers isKindOfClass:NSMutableArray.class] && cls &&
        [list respondsToSelector:@selector(specifierForID:)] &&
        [list respondsToSelector:@selector(reloadSpecifier:animated:)]) {
        BOOL present = NO;
        for (id<HKSpecifier> s in specifiers)
            if ([[s identifier] isEqualToString:rowID]) { present = YES; break; }
        if (!present) {
            HKCellularNetworkRow *row = objc_getAssociatedObject(list, &ownerKey);
            if (!row) {
                row = [HKCellularNetworkRow new];
                row.list = list;
                objc_setAssociatedObject(list, &ownerKey, row, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            }
            /* PSTitleValueCell (4), with only a getter. */
            id<HKSpecifier> spec = [cls preferenceSpecifierNamed:localized(@"NETWORK", @"Network") target:row
                                       set:NULL get:@selector(networkValue:) detail:Nil cell:4 edit:Nil];
            if (spec) {
                [spec setProperty:rowID forKey:@"id"];
                NSUInteger index = 0;
                if (!specifiers.count) [specifiers addObject:[cls emptyGroupSpecifier]];
                if ([specifiers.firstObject cellType] == 0) index = 1;
                [specifiers insertObject:spec atIndex:index];
                row.specifier = spec;
                log_row("inserted", number("kern.hackios.cellular_sim_state", 0),
                        number("kern.hackios.cellular_registered", 0), NO);
            }
        }
    }
    originalFinishList(list, cmd, specifiers);
}

static void install_network_row(void)
{
    Class cls = objc_lookUpClass("PSUICellularController");
    SEL sel = sel_registerName("disableSpecifiersIfNeeded:");
    Method method = cls ? class_getInstanceMethod(cls, sel) : NULL;
    if (!method || method_getNumberOfArguments(method) != 3 || !objc_lookUpClass("PSSpecifier")) return;
    char type[8], arg[8];
    method_getReturnType(method, type, sizeof(type));
    method_getArgumentType(method, 2, arg, sizeof(arg));
    if (type[0] != 'v' || arg[0] != '@') return;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        originalFinishList = (void (*)(id,SEL,NSMutableArray *))method_getImplementation(method);
        method_setImplementation(method, (IMP)finishList);
        log_row("installed", number("kern.hackios.cellular_sim_state", 0),
                number("kern.hackios.cellular_registered", 0), NO);
    });
}

__attribute__((constructor))
static void start_network_row(void)
{
    if (strcmp(getprogname(), "Preferences") || number("kern.hackios.cellular_sim_state", -1) < 0) return;
    @autoreleasepool {
        [NSNotificationCenter.defaultCenter addObserverForName:NSBundleDidLoadNotification object:nil queue:nil
            usingBlock:^(NSNotification *note) { install_network_row(); }];
        install_network_row();
    }
}
