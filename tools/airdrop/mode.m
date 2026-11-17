/* The read-only Settings controller operation from iOS 16.5. Do not
 * infer Receiving Off from its initial, zeroed cache before a reply. */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

@interface NSObject (HKAirDropSettings)
+ (id)sharedConnection;
- (BOOL)isAirDropAllowed;
- (void)setDelegate:(id)delegate;
- (NSInteger)discoverableMode;
- (BOOL)isVisible;
- (BOOL)isTimeLimitedEveryoneMode;
- (void)invalidate;
@end

@interface HKModeObserver : NSObject
@property(atomic) BOOL haveMode;
@property(atomic) NSUInteger visibilityEvents;
@end
@implementation HKModeObserver
- (void)discoveryControllerSettingsDidChange:(id)controller { self.haveMode = YES; }
- (void)discoveryControllerVisibilityDidChange:(id)controller { self.visibilityEvents++; }
@end

static void expire(int sig) {
    const char text[]="airdrop-mode: operation=timeout mode=unknown\n";
    (void)sig;
    (void)write(STDOUT_FILENO,text,sizeof(text)-1);
    _exit(2);
}

static CFStringRef exportedString(void *library, const char *symbol) {
    if (!library) return NULL;
    CFStringRef const *value=dlsym(library,symbol);
    if (!value || !*value || CFGetTypeID(*value)!=CFStringGetTypeID()) return NULL;
    return *value;
}

static int savedMode(void) {
    void *library=dlopen("/System/Library/PrivateFrameworks/Sharing.framework/Sharing",RTLD_LAZY|RTLD_LOCAL);
    CFStringRef key=exportedString(library,"kSFOperationDiscoverableModeKey");
    if (!key) {
        puts("airdrop-mode: user_domain=unavailable effective_mode=unqueried");
        return 1;
    }
    /* This is the mobile user's saved preference, not sharingd's resolved
     * value after restrictions, a default or Everyone-mode expiration. */
    CFTypeRef value=CFPreferencesCopyValue(key,CFSTR("com.apple.sharingd"),kCFPreferencesCurrentUser,kCFPreferencesAnyHost);
    if (!value) {
        puts("airdrop-mode: user_domain=no_value_visible saved=unknown effective_mode=unqueried");
        return 1;
    }
    const struct {const char *symbol, *label;} modes[]={
        {"kSFOperationDiscoverableModeOff","off"},
        {"kSFOperationDiscoverableModeContactsOnly","contacts"},
        {"kSFOperationDiscoverableModeEveryone","everyone"},
        {"kSFOperationDiscoverableModeDisabled","disabled"}
    };
    const char *label="unknown";
    for (size_t i=0;i<sizeof(modes)/sizeof(modes[0]);++i) {
        CFStringRef expected=exportedString(library,modes[i].symbol);
        if (expected && CFEqual(value,expected)) { label=modes[i].label; break; }
    }
    CFRelease(value);
    printf("airdrop-mode: user_domain=value_visible saved=%s effective_mode=unqueried\n",label);
    return !strcmp(label,"unknown");
}

int main(int argc,char **argv) {
    @autoreleasepool {
        if (argc>2 || (argc==2 && strcmp(argv[1],"--saved"))) return 2;
        setvbuf(stdout,NULL,_IOLBF,0);
        gid_t mobile=501;
        if (geteuid()==0 && (setgroups(1,&mobile) || setgid(mobile) || setuid(501))) return 2;
        if (getuid()!=501 || geteuid()!=501) return 2;
        struct sigaction sa={0}; sa.sa_handler=expire; sigemptyset(&sa.sa_mask);
        if (sigaction(SIGALRM,&sa,NULL)) return 2;
        alarm(12);
        if (argc==2) return savedMode();
        void *mc=dlopen("/System/Library/PrivateFrameworks/ManagedConfiguration.framework/ManagedConfiguration",RTLD_LAZY|RTLD_LOCAL);
        Class mcClass=mc?NSClassFromString(@"MCProfileConnection"):Nil;
        id connection=[mcClass respondsToSelector:@selector(sharedConnection)]?[mcClass sharedConnection]:nil;
        const char *allowed="unknown";
        if ([connection respondsToSelector:@selector(isAirDropAllowed)])
            allowed=[connection isAirDropAllowed]?"yes":"no";
        printf("airdrop-mode: uid=%u policy_allows=%s\n",(unsigned)geteuid(),allowed);

        void *ui=dlopen("/System/Library/PrivateFrameworks/SharingUI.framework/SharingUI",RTLD_LAZY|RTLD_LOCAL);
        Class cls=ui?NSClassFromString(@"SFAirDropDiscoveryController"):Nil;
        id controller=[cls new];
        if (!controller || ![controller respondsToSelector:@selector(setDelegate:)] ||
            ![controller respondsToSelector:@selector(discoverableMode)] ||
            ![controller respondsToSelector:@selector(isVisible)] ||
            ![controller respondsToSelector:@selector(invalidate)]) {
            puts("airdrop-mode: operation=unavailable mode=unknown");
            return 1;
        }
        HKModeObserver *observer=[HKModeObserver new];
        [controller setDelegate:observer];
        const NSTimeInterval deadline=NSProcessInfo.processInfo.systemUptime+7;
        while (!observer.haveMode && NSProcessInfo.processInfo.systemUptime<deadline)
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.15]];
        BOOL received=observer.haveMode;
        if (received) {
            NSInteger value=[controller discoverableMode];
            BOOL visible=[controller isVisible];
            const char *mode="unknown";
            if (!visible) mode="disabled";
            else if (value==0) mode="off";
            else if (value==1) mode="contacts";
            else if (value==2) mode="everyone";
            const char *timed="unknown";
            if ([controller respondsToSelector:@selector(isTimeLimitedEveryoneMode)])
                timed=[controller isTimeLimitedEveryoneMode]?"yes":"no";
            printf("airdrop-mode: operation=reply visible=%u raw=%ld mode=%s timed_everyone=%s visibility_callbacks=%lu\n",
                   (unsigned)visible,(long)value,mode,timed,(unsigned long)observer.visibilityEvents);
        } else {
            printf("airdrop-mode: operation=no_mode_callback mode=unknown visibility_callbacks=%lu\n",
                   (unsigned long)observer.visibilityEvents);
        }
        [controller setDelegate:nil];
        [controller invalidate];
        alarm(0);
        return received?0:1;
    }
}
