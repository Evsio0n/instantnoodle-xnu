#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <sys/sysctl.h>
#include <dlfcn.h>
#include <grp.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

@interface NSObject (ReadBrightness)
- (id)copyPropertyForKey:(NSString *)key;
@end

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

static int pref(id client) {
    if(!client || ![client respondsToSelector:@selector(copyPropertyForKey:)])return -1;
    id value=[client copyPropertyForKey:@"DisplayBrightnessAuto"];
    if(![value isKindOfClass:NSNumber.class])return -1;
    double v=[(NSNumber *)value doubleValue];
    return v==1.0?1:v==0.0?0:-1;
}

static bool bool_encoding(const char *p) {return !strcmp(p,"B")||!strcmp(p,"c");}

int main(int argc,char **argv) { @autoreleasepool {
    if(argc!=2||strcmp(argv[1],"--enable-once")||!ram_scope())return 2;
    gid_t mobile=501;
    if(geteuid()==0 && (setgroups(1,&mobile)||setgid(mobile)||setuid(501)))return 2;
    if(getuid()!=501||geteuid()!=501||getgid()!=501||getegid()!=501)return 2;
    void *framework=dlopen("/System/Library/PrivateFrameworks/CoreBrightness.framework/CoreBrightness",RTLD_NOW|RTLD_LOCAL);
    Class cls=framework?NSClassFromString(@"BrightnessSystemClient"):Nil;
    SEL setter=NSSelectorFromString(@"setProperty:forKey:");
    if(!cls || !class_getInstanceMethod(cls,setter))return 3;
    @try {
        id client=[[cls alloc] init];
        int before=-1;
        for(unsigned i=0;i<60;i++) {
            before=pref(client);
            if(before>=0)break;
            usleep(500000);
            client=[[cls alloc] init];
        }
        if(before<0){printf("als-enable-once: ordinary preference unavailable after 30s uid=501\n");return 4;}
        NSMethodSignature *sig=[client methodSignatureForSelector:setter];
        bool signature=sig && sig.numberOfArguments==4 && sig.methodReturnLength<=1 &&
            [sig getArgumentTypeAtIndex:2][0]=='@' && [sig getArgumentTypeAtIndex:3][0]=='@' &&
            (bool_encoding(sig.methodReturnType)||!strcmp(sig.methodReturnType,"v"));
        if(!signature){printf("als-enable-once: normal setter signature unavailable uid=501\n");return 3;}
        printf("als-enable-once: uid=501 normal setter return=%s object_args=2 before=%d\n",sig.methodReturnType,before);
        if(before==1){printf("als-enable-once: existing=on writes=0\n");return 0;}
        NSInvocation *call=[NSInvocation invocationWithMethodSignature:sig];
        [call setTarget:client];[call setSelector:setter];
        __unsafe_unretained id value=@YES,key=@"DisplayBrightnessAuto";
        [call setArgument:&value atIndex:2];[call setArgument:&key atIndex:3];
        [call invoke];
        int status=-1;
        if(bool_encoding(sig.methodReturnType)) {
            signed char answer=0;[call getReturnValue:&answer];status=answer!=0;
        }
        // Only an independent fresh client can establish what the ordinary service reports.
        for(unsigned i=0;i<12;i++) {
            @autoreleasepool {
                id other=[[cls alloc] init];int readback=pref(other);
                if(readback==1){
                    printf("als-enable-once: writes=1 setter_reply=%d fresh_corebrightness_readback=on\n",status);
                    return status==0?5:0;
                }
            }
            usleep(250000);
        }
        printf("als-enable-once: writes=1 setter_reply=%d fresh_readback_on=0\n",status);return 5;
    } @catch(NSException *ex) {
        (void)ex;printf("als-enable-once: ordinary CoreBrightness operation refused uid=501\n");return 6;
    }
}}
