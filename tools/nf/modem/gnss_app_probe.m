/* Read-only LaunchServices diagnostics, restricted to this bundle and Apple's Settings control. */
#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonDigest.h>
#import <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <notify.h>
@interface NSObject (ProbeProxy)
+ (id)applicationProxyForIdentifier:(NSString *)identifier;
+ (id)applicationProxyForBundleURL:(NSURL *)url;
- (NSString *)bundleIdentifier;
- (NSURL *)bundleURL;
- (NSURL *)dataContainerURL;
- (BOOL)isInstalled;
- (NSString *)applicationType;
@end
static NSString *const AppID=@"com.hackios.gnss-monitor";
static int hexfile(NSString *path,char text[65])
{
    FILE *f=fopen(path.fileSystemRepresentation,"rb");
    if(!f)return 0;
    uint8_t buf[8192],digest[CC_SHA256_DIGEST_LENGTH];size_t n=0,total=0;
    CC_SHA256_CTX c;CC_SHA256_Init(&c);
    while((n=fread(buf,1,sizeof(buf),f))>0){
        total+=n;if(total>2*1024*1024){fclose(f);return 0;}CC_SHA256_Update(&c,buf,(CC_LONG)n);
    }
    int ok=!ferror(f)&&total>0;fclose(f);if(!ok)return 0;
    CC_SHA256_Final(digest,&c);
    for(unsigned i=0;i<CC_SHA256_DIGEST_LENGTH;i++)snprintf(text+2*i,3,"%02x",digest[i]);
    return 1;
}
static BOOL trustedPath(NSString *path)
{
    if ([path isEqualToString:@"/Applications/GNSSMonitor.app"]) return YES;
    NSArray *parts=[path pathComponents];
    BOOL container=([path hasPrefix:@"/private/var/containers/Bundle/Application/"]||[path hasPrefix:@"/var/containers/Bundle/Application/"]);
    if (!container || parts.count<3 || ![parts.lastObject isEqualToString:@"GNSSMonitor.app"]) return NO;
    NSUUID *uuid=[[[NSUUID alloc] initWithUUIDString:[parts objectAtIndex:parts.count-2]] autorelease];
    NSString *parent=[path stringByDeletingLastPathComponent].stringByDeletingLastPathComponent;
    return uuid && ([parent isEqualToString:@"/private/var/containers/Bundle/Application"]||[parent isEqualToString:@"/var/containers/Bundle/Application"]);
}
static BOOL identified(id proxy,NSString *name)
{
    return proxy && [proxy respondsToSelector:@selector(bundleIdentifier)]
           && [[proxy bundleIdentifier] isEqualToString:name];
}
static int installed(id p)
{
    return p && [p respondsToSelector:@selector(isInstalled)] ? ([p isInstalled]?1:0) : -1;
}
static void settingsAction(int console,id proxy,unsigned seq)
{
    if(!identified(proxy,AppID)||installed(proxy)!=1||![proxy respondsToSelector:@selector(dataContainerURL)])return;
    NSString *base=[[proxy dataContainerURL] path].stringByStandardizingPath;
    NSString *parent=base.stringByDeletingLastPathComponent;
    if(![parent isEqualToString:@"/var/mobile/Containers/Data/Application"]&&
       ![parent isEqualToString:@"/private/var/mobile/Containers/Data/Application"])return;
    if(![[[NSUUID alloc] initWithUUIDString:base.lastPathComponent] autorelease])return;
    int dir=open(base.fileSystemRepresentation,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(dir<0)return;
    int lib=openat(dir,"Library",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);close(dir);if(lib<0)return;
    int cache=openat(lib,"Caches",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);close(lib);if(cache<0)return;
    int file=openat(cache,"com.hackios.gnss-monitor.settings.plist",O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    close(cache);if(file<0)return;
    struct stat st;char b[1024];ssize_t n=-1;
    if(fstat(file,&st)==0&&S_ISREG(st.st_mode)&&st.st_uid==501&&st.st_size>0&&st.st_size<(off_t)sizeof(b))
        n=read(file,b,sizeof(b));
    close(file);if(n<=0||n!=st.st_size)return;
    id data=[NSPropertyListSerialization propertyListWithData:[NSData dataWithBytes:b length:(NSUInteger)n] options:0 format:NULL error:NULL];
    if(![data isKindOfClass:[NSDictionary class]])return;
    for(NSString *key in @[@"v",@"pid",@"attempt",@"tap",@"ended",@"accepted"])
        if(![[data objectForKey:key] isKindOfClass:[NSNumber class]])return;
    unsigned pid=[data[@"pid"] unsignedIntValue],attempt=[data[@"attempt"] unsignedIntValue];
    uint64_t tap=[data[@"tap"] unsignedLongLongValue],end=[data[@"ended"] unsignedLongLongValue];
    uint64_t now=(uint64_t)(NSProcessInfo.processInfo.systemUptime*1000);
    int accepted=[data[@"accepted"] intValue];
    if([data[@"v"] intValue]!=1||!pid||!attempt||!tap||tap>now||now-tap>150000||accepted < -1||accepted>1||
       (accepted==-1?end!=0:(end<tap||end>now)))return;
    dprintf(console,"gnss-settings-action seq=%u uid=501 pid=%u attempt=%u tap_ms=%llu end_ms=%llu accepted=%d end\n",
            seq,pid,attempt,tap,end,accepted);
}
static void snapshot(int fd, Class cls,unsigned seq)
{
    NSString *controlID=@"com.apple.Preferences";
    id control=[cls applicationProxyForIdentifier:controlID], target=[cls applicationProxyForIdentifier:AppID];
    id byURL=[cls respondsToSelector:@selector(applicationProxyForBundleURL:)]?
        [cls applicationProxyForBundleURL:[NSURL fileURLWithPath:@"/Applications/GNSSMonitor.app" isDirectory:YES]]:nil;
    NSString *settingsPath=@"/Applications/Preferences.app";
    NSDictionary *info=[NSDictionary dictionaryWithContentsOfFile:[settingsPath stringByAppendingPathComponent:@"Info.plist"]];
    NSString *exeName=info[@"CFBundleExecutable"];
    BOOL stockID=[info[@"CFBundleIdentifier"] isEqualToString:controlID];
    BOOL stockExe=[exeName isEqualToString:@"Preferences"] &&
        [[NSFileManager defaultManager] isExecutableFileAtPath:[settingsPath stringByAppendingPathComponent:@"Preferences"]];
    NSString *reported=identified(control,controlID) && [control respondsToSelector:@selector(bundleURL)] ? [[control bundleURL] path] : nil;
    id fromSettingsURL=[cls respondsToSelector:@selector(applicationProxyForBundleURL:)] ?
        [cls applicationProxyForBundleURL:[NSURL fileURLWithPath:settingsPath isDirectory:YES]] : nil;
    const char *registered=[reported isEqualToString:settingsPath] ? "stock" : (reported ? "other" : "none");
    dprintf(fd,"gnss-settings-ls seq=%u uid=%u stock_plist=%d stock_id=%d stock_exec=%d proxy_id=%d proxy_installed=%d proxy_path=%s by_url=%d end\n",
        seq,geteuid(),info!=nil,(int)stockID,(int)stockExe,(int)identified(control,controlID),installed(control),
        registered,(int)identified(fromSettingsURL,controlID));
    NSString *path=identified(target,AppID)&&[target respondsToSelector:@selector(bundleURL)]?[[target bundleURL] path]:nil;
    char executable[65]="missing",plist[65]="missing";
    if (identified(target,AppID) && [target respondsToSelector:@selector(applicationType)]) {
        NSString *type=[target applicationType];
        const char *value=[type isEqualToString:@"System"]?"System":([type isEqualToString:@"User"]?"User":"other");
        BOOL container=[target respondsToSelector:@selector(dataContainerURL)] && [target dataContainerURL]!=nil;
        dprintf(fd,"gnss-permission-ls seq=%u uid=%u type=%s has_container=%d end\n",seq,geteuid(),value,container);
    }
    if(trustedPath(path)){
        hexfile([path stringByAppendingPathComponent:@"GNSSMonitor"],executable);
        hexfile([path stringByAppendingPathComponent:@"Info.plist"],plist);
    }
    const char *display=trustedPath(path)?path.UTF8String:(path?"other":"none");
    if(trustedPath(path)&&strcmp(executable,"missing"))settingsAction(fd,target,seq);
    dprintf(fd,"gnss-ls seq=%u uid=%u settings=%d id=%d installed=%d byurl=%d path=%s exe=%s plist=%s end\n",
            seq,geteuid(),identified(control,controlID)?installed(control):-1,identified(target,AppID)?1:0,
            installed(target),identified(byURL,AppID)?1:0,display,executable,plist);
}
static void permissionCounters(int fd,int token,unsigned sequence)
{
    static int stages; static dispatch_once_t once;
    dispatch_once(&once, ^{if(notify_register_check("com.hackios.gnss-monitor.lifecycle",&stages)!=NOTIFY_STATUS_OK)stages=-1;});
    uint64_t bits=0;
    if(stages>=0 && notify_get_state(stages,&bits)==NOTIFY_STATUS_OK && (bits>>32))
        dprintf(fd,"gnss-permission-stages seq=%u uid=%u pid=%u bits=%u end\n",sequence,geteuid(),(unsigned)(bits>>32),(unsigned)bits);
    uint64_t value=0;
    if(token<0 || notify_get_state(token,&value)!=NOTIFY_STATUS_OK || !(value>>32)) return;
    dprintf(fd,"gnss-permission-app seq=%u uid=%u pid=%u taps=%u calls=%u callbacks=%u global=%d auth=%u end\n",
            sequence,geteuid(),(unsigned)(value>>32),(unsigned)((value>>24)&255),
            (unsigned)((value>>16)&255),(unsigned)((value>>8)&255),(int)((value>>4)&15)-1,(unsigned)(value&15));
}
int main(void)
{
    int fd=open("/dev/console",O_WRONLY|O_NOCTTY);if(fd<0)return 1;
    if(getuid()==0){
        gid_t group=501;
        if(setgroups(1,&group)||setgid(501)||setuid(501)){dprintf(fd,"gnss-ls-unavailable drop-user end\n");return 1;}
    }
    if(geteuid()!=501){dprintf(fd,"gnss-ls-unavailable unexpected-user end\n");return 1;}
    setenv("HOME","/var/mobile",1);setenv("CFFIXED_USER_HOME","/var/mobile",1);
    @autoreleasepool {
        void *sbs=dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices",RTLD_LAZY|RTLD_LOCAL);
        CFStringRef (*explain)(uint32_t)=sbs?dlsym(sbs,"SBSApplicationLaunchingErrorString"):NULL;
        char value[170]="unavailable";
        if(explain){CFStringRef s=explain(9);if(s)CFStringGetCString(s,value,sizeof(value),kCFStringEncodingUTF8);}
        dprintf(fd,"gnss-sbs original_code=9 reason=%s end\n",value);
        dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices",RTLD_LAZY|RTLD_LOCAL);
        Class cls=NSClassFromString(@"LSApplicationProxy");
        if(!cls||![cls respondsToSelector:@selector(applicationProxyForIdentifier:)]){
            dprintf(fd,"gnss-ls-unavailable lookup end\n");return 1;
        }
        int token=-1;
        if(notify_register_check("com.hackios.gnss-monitor.permission-observation",&token)!=NOTIFY_STATUS_OK)token=-1;
        for(unsigned seq=0;seq<7;seq++){
            @autoreleasepool {snapshot(fd,cls,seq);permissionCounters(fd,token,seq);}
            if(seq<6)sleep(15);
        }
        for(unsigned seq=7;seq<42 && token>=0;seq++){permissionCounters(fd,token,seq);sleep(5);}
        if(token>=0)notify_cancel(token);
    }
    close(fd);return 0;
}
