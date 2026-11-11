/* Stock iOS cellular affordances, queried by the ordinary mobile account.
 * Four independent subprocesses preserve the distinction between a blocked
 * framework, a missing provider, an empty answer and a failed inspection. */
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../modem/mdm_radio_status.h"

extern char **environ;
extern int proc_pidpath(int pid,void *path,uint32_t size);
extern kern_return_t bootstrap_look_up(mach_port_t,const char *,mach_port_t *);
@interface NSObject (StockCTReads)
- (id)initWithQueue:(dispatch_queue_t)queue;
- (id)getSubscriptionInfoWithError:(NSError **)error;
- (NSArray *)subscriptions;
- (NSDictionary *)serviceCurrentRadioAccessTechnology;
@end

static void row(void) {
    NSString *path=@"/System/Library/PrivateFrameworks/PreferencesUI.framework/Settings.plist";
    NSDictionary *settings=[NSDictionary dictionaryWithContentsOfFile:path];
    unsigned matches=0; BOOL capability=NO,getter=NO,bundle=NO;
    for(NSDictionary *item in settings[@"items"]){
        if (![item[@"id"] isEqual:@"MOBILE_DATA_SETTINGS_ID"])continue;
        ++matches;
        capability=[item[@"requiredCapabilities"] containsObject:@"cellular-data"];
        getter=[item[@"get"] isEqual:@"cellularDataStatusForSpecifier:"];
        bundle=[item[@"bundle"] isEqual:@"SettingsCellular"];
    }
    path=@"/System/Library/PreferenceBundles/SettingsCellular.bundle/Info.plist";
    NSDictionary *info=[NSDictionary dictionaryWithContentsOfFile:path];
    printf("stock-cell: resource matches=%u capability=%d getter=%d bundle=%d principal=%d end\n",
           matches,capability,getter,bundle,[info[@"NSPrincipalClass"] isEqual:@"PSUICellularController"]);
}

static const char *boolean_answer(CFTypeRef (*readAnswer)(CFStringRef),CFStringRef key) {
    CFTypeRef answer=readAnswer(key);
    if(!answer)return "unknown";
    const char *text="nonboolean";
    if(CFGetTypeID(answer)==CFBooleanGetTypeID())text=CFBooleanGetValue(answer)?"yes":"no";
    CFRelease(answer);return text;
}
static void gestalt(void) {
    void *lib=dlopen("/usr/lib/libMobileGestalt.dylib",RTLD_LAZY|RTLD_LOCAL);
    CFTypeRef (*answer)(CFStringRef)=lib?dlsym(lib,"MGCopyAnswer"):NULL;
    if(!answer){puts("stock-cell: gestalt unavailable end");return;}
    const char *data=boolean_answer(answer,CFSTR("cellular-data"));
    const char *telephony=boolean_answer(answer,CFSTR("telephony"));
    const char *baseband=boolean_answer(answer,CFSTR("HasBaseband"));
    printf("stock-cell: gestalt cellular_data=%s telephony=%s has_baseband=%s end\n",data,telephony,baseband);
}

static int protocol_index(CFTypeRef prop) {
    const char *const known[]={"pbl","sahara","qmux1","qmux2"};char name[32];
    if(!prop)return -1;
    if(CFGetTypeID(prop)==CFStringGetTypeID()){
        if(!CFStringGetCString((CFStringRef)prop,name,sizeof(name),kCFStringEncodingASCII))return -1;
        for(int i=0;i<4;i++)if(!strcmp(known[i],name))return i;
    }else if(CFGetTypeID(prop)==CFDataGetTypeID()){
        const UInt8 *raw=CFDataGetBytePtr((CFDataRef)prop);
        size_t n=(size_t)CFDataGetLength((CFDataRef)prop);
        if(n && !raw[n-1])--n;
        for(int i=0;i<4;i++)if(n==strlen(known[i]) && !memcmp(raw,known[i],n))return i;
    }
    return -1;
}
static int count_services(const char *key,bool by_name,unsigned *channels) {
    CFMutableDictionaryRef dict=by_name?IOServiceNameMatching(key):IOServiceMatching(key);
    io_iterator_t iter=IO_OBJECT_NULL;int count=0;
    if(!dict || IOServiceGetMatchingServices(MACH_PORT_NULL,dict,&iter)!=KERN_SUCCESS)return -1;
    if(!iter)return 0;
    io_object_t entry;
    while((entry=IOIteratorNext(iter))!=IO_OBJECT_NULL){
        count++;
        if(channels){
            CFTypeRef v=IORegistryEntryCreateCFProperty(entry,CFSTR("ABPInterfaceProtocol"),NULL,0);
            int index=protocol_index(v);
            if(index>=0)channels[index]++;
            if(v)CFRelease(v);
        }
        IOObjectRelease(entry);
    }
    if(!IOIteratorIsValid(iter))count=-1;
    IOObjectRelease(iter);return count;
}
static void registry(void) {
    unsigned ch[4]={0};
    int positive=count_services("IOResources",false,NULL);
    int modem=count_services("AppleBaseband",false,NULL),control=count_services("AppleBasebandPCIControl",false,NULL);
    int interfaces=count_services("AppleBasebandPCIInterface",false,ch);
    int manager=count_services("AppleBasebandPCIPDPManagerBase",false,NULL),appender=count_services("AppleIPAppender",false,NULL);
    printf("stock-cell: iokit control_view=%d baseband=%d pci_control=%d interfaces=%d pbl=%u sahara=%u qmux1=%u qmux2=%u pdp_manager=%d ip_appender=%d end\n",positive,modem,control,interfaces,ch[0],ch[1],ch[2],ch[3],manager,appender);
    struct hk_radio_snapshot s={0};size_t length=sizeof(s);
    if(!sysctlbyname("kern.hackios.modem_radio_status",&s,&length,NULL,0) && length==sizeof(s) && s.size==length && s.version==HK_RADIO_VERSION){
        printf("stock-cell: producer phase=%u dms_state=%u nas_state=%u wds_state=%u end\n",
               s.phase,s.fields[HK_RADIO_MODE].state,s.fields[HK_RADIO_SIGNAL].state,s.fields[HK_RADIO_PACKET].state);
    }else puts("stock-cell: producer unavailable end");
}
static int stock_pid(void) {
    int mib[4]={CTL_KERN,KERN_PROC,KERN_PROC_ALL,0};size_t n=0;
    if(sysctl(mib,3,NULL,&n,NULL,0))return -1;
    n+=64*sizeof(struct kinfo_proc);
    if(n>16*1024*1024)return -1;
    struct kinfo_proc *p=malloc(n);
    if(!p)return -1;
    int found=0;
    if(sysctl(mib,3,p,&n,NULL,0))found=-1;
    else for(size_t i=0;i<n/sizeof(*p);i++){
        if(strcmp(p[i].kp_proc.p_comm,"CommCenter"))continue;
        char path[1024]={0};
        if(proc_pidpath(p[i].kp_proc.p_pid,path,sizeof(path))>0 &&
           !strcmp(path,"/System/Library/Frameworks/CoreTelephony.framework/Support/CommCenter")){
            found=p[i].kp_proc.p_pid;break;
        }
        found=-2;
    }
    free(p);return found;
}
static BOOL endpoint(void){
    int pid=stock_pid();
    if(pid<=0){printf("stock-cell: ct provider=%s endpoint=unqueried end\n",
            pid==0?"absent":pid==-2?"identity_unverified":"inspection_failed");return NO;}
    mach_port_t p=MACH_PORT_NULL;
    kern_return_t kr=bootstrap_look_up(bootstrap_port,"com.apple.commcenter.coretelephony.xpc",&p);
    if(p!=MACH_PORT_NULL)mach_port_deallocate(mach_task_self(),p);
    printf("stock-cell: ct provider=stock pid=%d endpoint=%s kr=%d end\n",pid,
        kr==KERN_SUCCESS && p!=MACH_PORT_NULL?"resolved":"unavailable",kr);
    return kr==KERN_SUCCESS && p!=MACH_PORT_NULL;
}
static void ct_read(bool radio) {
    if(!endpoint())return;
    void *lib=dlopen("/System/Library/Frameworks/CoreTelephony.framework/CoreTelephony",RTLD_LAZY|RTLD_LOCAL);
    if(!lib){puts("stock-cell: ct framework=unavailable end");return;}
    if(radio){
        id client=[[NSClassFromString(@"CTTelephonyNetworkInfo") alloc] init];
        if(![client respondsToSelector:@selector(serviceCurrentRadioAccessTechnology)]){puts("stock-cell: rat api=unavailable end");return;}
        NSDictionary *rat=[client serviceCurrentRadioAccessTechnology];
        if(![rat isKindOfClass:NSDictionary.class]){puts("stock-cell: rat api=returned type=nil_or_other end");return;}
        unsigned lte=0,nr=0,other=0;
        for(id tech in rat.allValues){
            if([tech isEqual:@"CTRadioAccessTechnologyLTE"])lte++;
            else if([tech isEqual:@"CTRadioAccessTechnologyNR"] || [tech isEqual:@"CTRadioAccessTechnologyNRNSA"])nr++;
            else other++;
        }
        printf("stock-cell: rat api=returned entries=%lu lte=%u nr=%u other=%u end\n",(unsigned long)rat.count,lte,nr,other);
    }else{
        Class cls=NSClassFromString(@"CoreTelephonyClient");
        if(![cls instancesRespondToSelector:@selector(initWithQueue:)]){puts("stock-cell: subscription api=unavailable end");return;}
        id client=[[cls alloc] initWithQueue:dispatch_queue_create("local.cellular.stock",DISPATCH_QUEUE_SERIAL)];
        if(![client respondsToSelector:@selector(getSubscriptionInfoWithError:)]){puts("stock-cell: subscription api=unavailable end");return;}
        NSError *err=nil;
        id info=[client getSubscriptionInfoWithError:&err];
        if(err){printf("stock-cell: subscription api=error code=%ld end\n",(long)err.code);return;}
        if(![info respondsToSelector:@selector(subscriptions)]){puts("stock-cell: subscription api=returned result=unavailable end");return;}
        id list=[info subscriptions];
        if(![list isKindOfClass:NSArray.class]){puts("stock-cell: subscription api=returned result=unknown end");return;}
        printf("stock-cell: subscription api=returned entries=%lu end\n",(unsigned long)[list count]);
    }
}
int main(int argc,char **argv) {
    if(getuid()!=501 || geteuid()!=501)return 2;
    setvbuf(stdout,NULL,_IOLBF,0);
    @autoreleasepool {
        if(argc==2){
            if(!strcmp(argv[1],"row"))row();
            else if(!strcmp(argv[1],"gestalt"))gestalt();
            else if(!strcmp(argv[1],"registry"))registry();
            else if(!strcmp(argv[1],"subscription"))ct_read(false);
            else if(!strcmp(argv[1],"rat"))ct_read(true);
            else return 2;
            return 0;
        }
        if(argc!=1)return 2;
        sleep(45);
        char *groups[]={"row","gestalt","registry","abp","subscription","rat"};
        for(unsigned i=0;i<sizeof(groups)/sizeof(groups[0]);i++){
            bool abp=!strcmp(groups[i],"abp");
            char *program=abp?"/hackios/bin/abp_match":argv[0];
            char *args[]={program,abp?NULL:groups[i],NULL};pid_t child;int status=0;
            if(posix_spawn(&child,program,NULL,NULL,args,environ)){printf("stock-cell: stage=%s spawn_failed end\n",groups[i]);continue;}
            unsigned tick=0;
            for(;tick<70;tick++){
                pid_t done=waitpid(child,&status,WNOHANG);
                if(done==child)break;
                if(done<0 && errno!=EINTR){printf("stock-cell: stage=%s wait_failed end\n",groups[i]);return 1;}
                usleep(100000);
            }
            if(tick==70){kill(child,SIGKILL);while(waitpid(child,&status,0)<0 && errno==EINTR){}
                printf("stock-cell: stage=%s timeout end\n",groups[i]);
            }else printf("stock-cell: stage=%s exit=%d end\n",groups[i],WIFEXITED(status)?WEXITSTATUS(status):-1);
        }
    }
    return 0;
}
