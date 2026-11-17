#import <CoreFoundation/CoreFoundation.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <sys/sysctl.h>
#include <dlfcn.h>
#include <grp.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

static bool ram_scope(void) {
    char args[1024]={0};
    const char *const keys[]={"board","als","hk_data","var_data","var_store","ufs_write"};
    const char *const values[]={"instantnoodle","1","2","hfs","0","0"};
    unsigned seen=0;size_t length=sizeof(args);
    if(sysctlbyname("kern.bootargs",args,&length,NULL,0)||length>sizeof(args)||!memchr(args,0,length))return false;
    char *save=NULL;
    for(char *t=strtok_r(args," ",&save);t;t=strtok_r(NULL," ",&save)) {
        char *eq=strchr(t,'=');if(!eq)continue;*eq=0;
        for(unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);i++) {
            if(strcmp(t,keys[i]))continue;
            if(seen&(1u<<i) || strcmp(eq+1,values[i]))return false;
            seen|=1u<<i;
        }
    }
    return seen==63;
}

typedef unsigned char (*StockRead)(void);
typedef void (*StockSet)(bool);

static void spin(double seconds) {
 [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
}

int main(int argc,char **argv) { @autoreleasepool {
 if(argc!=2 || strcmp(argv[1],"--enable-once") || !ram_scope())return 2;
 gid_t gid=501;
 if(geteuid()==0 && (setgroups(1,&gid)||setgid(gid)||setuid(501)))return 2;
 if(getuid()!=501 || geteuid()!=501 || getgid()!=501 || getegid()!=501)return 2;
 void *ax=dlopen("/usr/lib/libAccessibility.dylib",RTLD_NOW|RTLD_LOCAL);
 void *bk=dlopen("/System/Library/PrivateFrameworks/BackBoardServices.framework/BackBoardServices",RTLD_NOW|RTLD_LOCAL);
 StockRead readSetting=ax?(StockRead)dlsym(ax,"_AXSAutoBrightnessEnabled"):NULL;
 StockSet set=bk?(StockSet)dlsym(bk,"BKSDisplayBrightnessSetAutoBrightnessEnabled"):NULL;
 const CFStringRef *notification=ax?(const CFStringRef*)dlsym(ax,"kAXSAutoBrightnessChangedNotification"):NULL;
 if(!readSetting||!set||!notification||!*notification)return 3;
 int before=readSetting();
 if(before!=0 && before!=1)return 4;
 printf("als-enable-stock: uid=501 Accessibility Settings read before=%d\n",before);
 if(before==1){printf("als-enable-stock: writes=0 Settings already on\n");return 0;}
 /* Wait for ordinary backboardd/launch services. Scheduling readiness is
  * queried via the independently shipped BrightnessSystemClient. */
 void *cb=dlopen("/System/Library/PrivateFrameworks/CoreBrightness.framework/CoreBrightness",RTLD_NOW|RTLD_LOCAL);
 Class cls=cb?NSClassFromString(@"BrightnessSystemClient"):Nil;
 SEL getter=NSSelectorFromString(@"copyPropertyForKey:");
 BOOL ready=NO;
 for(unsigned i=0;cls && i<60 && !ready;i++) {
  @autoreleasepool {
   id client=[[cls alloc] init];NSMethodSignature *sig=[client methodSignatureForSelector:getter];
   if(sig && sig.numberOfArguments==3 && sig.methodReturnType[0]=='@'){
    NSInvocation *call=[NSInvocation invocationWithMethodSignature:sig];
    [call setTarget:client];[call setSelector:getter];
    __unsafe_unretained id key=@"DisplayBrightnessAuto",answer=nil;
    [call setArgument:&key atIndex:2];[call invoke];[call getReturnValue:&answer];
    ready=[answer isKindOfClass:NSNumber.class];
   }
  }
  if(!ready)spin(0.5);
 }
 if(!ready){printf("als-enable-stock: service unavailable writes=0\n");return 4;}
 before=readSetting();
 if(before==1){printf("als-enable-stock: writes=0 Settings already on\n");return 0;}
 if(before!=0)return 4;
 set(true);
 CFNotificationCenterPostNotification(CFNotificationCenterGetDarwinNotifyCenter(),*notification,NULL,NULL,true);
 for(unsigned i=0;i<48;i++){
  spin(0.25);
  if(readSetting()==1){
   printf("als-enable-stock: writes=1 stock_Settings_readback=on\n");return 0;
  }
 }
 printf("als-enable-stock: writes=1 stock_Settings_readback_on=0\n");return 5;
}}
