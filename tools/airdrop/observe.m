/* Read the stock pane resources and local runtime state. Address data is
 * reduced to a link-local presence flag; no address or peer identity is logged. */
#import <Foundation/Foundation.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <ifaddrs.h>
#include <mach/mach.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern int proc_pidpath(int pid, void *path, uint32_t size);
extern kern_return_t bootstrap_look_up(mach_port_t port, const char *name, mach_port_t *service);

static NSString *const General = @"/System/Library/PrivateFrameworks/Settings/GeneralSettingsUI.framework";
static NSString *const Sharing = @"/System/Library/PrivateFrameworks/Sharing.framework";

static NSDictionary *plist(NSString *path) {
    return [NSDictionary dictionaryWithContentsOfFile:path];
}

static BOOL hasItem(NSDictionary *resource, NSString *identifier, NSString *controller) {
    for (NSDictionary *row in resource[@"items"]) {
        if (![row[@"id"] isEqual:identifier]) continue;
        if (!controller || [row[@"detail"] isEqual:controller]) return YES;
    }
    return NO;
}

static void resources(void) {
    NSDictionary *general = plist([General stringByAppendingPathComponent:@"General.plist"]);
    NSDictionary *drop = plist([General stringByAppendingPathComponent:@"AirDrop.plist"]);
    NSString *sender = [Sharing stringByAppendingPathComponent:@"PlugIns/AirDrop.appex/Info.plist"];
    NSString *receiver = [Sharing stringByAppendingPathComponent:@"PlugIns/AirDropAlertUI.appex/Info.plist"];
    NSDictionary *send = plist(sender), *receive = plist(receiver);
    NSDictionary *app = plist(@"/Applications/AirDropUI.app/Info.plist");
    printf("airdrop-ui: resources general=%d pane=%d link=%d off=%d contacts=%d everyone=%d send_extension=%d receive_extension=%d system_ui=%d\n",
        general != nil, drop != nil, hasItem(general, @"AIRDROP_LINK", @"PSGAirDropController"),
        hasItem(drop, @"OFF_ID", nil), hasItem(drop, @"CONTACTS_ONLY_ID", nil), hasItem(drop, @"EVERYONE_ID", nil),
        [send[@"CFBundleIdentifier"] isEqual:@"com.apple.Sharing.AirDrop"],
        [receive[@"CFBundleIdentifier"] isEqual:@"com.apple.Sharing.AirDropAlertUI"],
        [app[@"CFBundleIdentifier"] isEqual:@"com.apple.Sharing.AirDropUI"]);
    void *framework = dlopen([Sharing stringByAppendingPathComponent:@"Sharing"].fileSystemRepresentation, RTLD_LAZY|RTLD_LOCAL);
    int (*supported)(void) = framework ? dlsym(framework, "SFDeviceSupportsAirDrop") : NULL;
    printf("airdrop-ui: stock_support=%s\n", supported ? (supported()?"yes":"no") : "unknown");
}

static const char *paths[] = {
    "/usr/libexec/sharingd", "/usr/libexec/rapportd", "/usr/sbin/bluetoothd",
    "/System/Library/PrivateFrameworks/IDS.framework/identityservicesd.app/identityservicesd",
    "/usr/sbin/wifid", "/usr/sbin/mDNSResponder"
};

static void processes(void) {
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL};
    size_t bytes = 0;
    if (sysctl(mib,3,NULL,&bytes,NULL,0) || bytes > 8*1024*1024) {
        printf("airdrop-ui: process_table=unavailable errno=%d\n", errno);
        return;
    }
    bytes += 64*sizeof(struct kinfo_proc);
    struct kinfo_proc *rows = calloc(1,bytes);
    if (!rows || sysctl(mib,3,rows,&bytes,NULL,0)) {
        printf("airdrop-ui: process_table=unavailable errno=%d\n", errno);
        free(rows);
        return;
    }
    for (size_t j=0; j<sizeof(paths)/sizeof(paths[0]); ++j) {
        const char *name = strrchr(paths[j],'/')+1;
        unsigned matched = 0, verified = 0;
        for (size_t i=0; i<bytes/sizeof(*rows); ++i) {
            if (strncmp(rows[i].kp_proc.p_comm,name,sizeof(rows[i].kp_proc.p_comm)-1)) continue;
            ++matched;
            char path[1024] = {0};
            if (proc_pidpath(rows[i].kp_proc.p_pid,path,sizeof(path))>0 && !strcmp(path,paths[j]))
                ++verified;
        }
        printf("airdrop-ui: process=%s name_matches=%u verified_path=%u\n", name,matched,verified);
    }
    free(rows);
}

static void interfaces(void) {
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list)) {
        printf("airdrop-ui: interfaces=unavailable errno=%d\n", errno);
        return;
    }
    const char *names[] = {"en0","awdl0","llw0","p2p0"};
    for (size_t i=0;i<sizeof(names)/sizeof(names[0]);++i) {
        unsigned flags = 0;
        bool present = false, linkLocal = false;
        for (struct ifaddrs *it=list;it;it=it->ifa_next) {
            if (!it->ifa_name || strcmp(it->ifa_name,names[i])) continue;
            present = true;
            flags |= it->ifa_flags;
            if (it->ifa_addr && it->ifa_addr->sa_family == AF_INET6) {
                struct sockaddr_in6 *addr = (struct sockaddr_in6 *)it->ifa_addr;
                if (IN6_IS_ADDR_LINKLOCAL(&addr->sin6_addr)) linkLocal = true;
            }
        }
        printf("airdrop-ui: interface=%s present=%d up=%d running=%d ipv6_link_local=%d\n",names[i],present,
               !!(flags&IFF_UP),!!(flags&IFF_RUNNING),linkLocal);
    }
    freeifaddrs(list);
}

static void endpoints(void) {
    /* This tests name resolution from the current bootstrap namespace. No
     * message is sent to the ports and no discovery client is created. */
    const char *names[] = {"com.apple.sharingd","com.apple.sharing.sharesheet","com.apple.sharingd.nsxpc",
        "com.apple.rapport","com.apple.bluetooth.xpc","com.apple.wifi-xpc"};
    for (size_t i=0;i<sizeof(names)/sizeof(names[0]);++i) {
        mach_port_t service = MACH_PORT_NULL;
        kern_return_t kr = bootstrap_look_up(bootstrap_port,names[i],&service);
        printf("airdrop-ui: mach=%s resolved=%d kr=%d\n",names[i],kr==KERN_SUCCESS && MACH_PORT_VALID(service),kr);
        if (MACH_PORT_VALID(service)) mach_port_deallocate(mach_task_self(),service);
    }
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc>2 || (argc==2 && strcmp(argv[1],"--lookup"))) return 2;
        setvbuf(stdout,NULL,_IOLBF,0);
        printf("airdrop-ui: uid=%u\n",(unsigned)geteuid());
        processes();
        interfaces();
        resources();
        if (argc==2) endpoints();
        return 0;
    }
}
