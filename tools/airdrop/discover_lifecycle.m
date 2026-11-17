/* Observe the unmodified 20F66 receiver at the HTTP serialization boundary.
 * Private RAM /var records are bounded and preserve the original invocations.
 * Constructor and individual hook receipts distinguish missing instrumentation
 * from a native call that did not happen. No credentials or record bytes logged. */
#import <Foundation/Foundation.h>
#import <CFNetwork/CFNetwork.h>
#import <objc/runtime.h>
#import <mach-o/dyld.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#ifdef HK_DISCOVER_CAPABILITY
#import "DiscoverCapability.h"
#endif

static int trace_fd=-1;
static atomic_uint rows;
static __thread void *discover_connection;
static void record(const char *format,...) {
    if(trace_fd<0 || atomic_fetch_add(&rows,1)>=2048)return;
    struct timespec rt,mt;clock_gettime(CLOCK_REALTIME,&rt);clock_gettime(CLOCK_MONOTONIC,&mt);
    mach_timebase_info_data_t tb;mach_timebase_info(&tb);
    uint64_t absolute_us=(uint64_t)((__uint128_t)mach_absolute_time()*tb.numer/tb.denom/1000);
    char text[3072];int n=snprintf(text,sizeof(text),"wall=%lld.%06ld mono=%lld.%06ld absolute_us=%llu ",
        (long long)rt.tv_sec,rt.tv_nsec/1000,(long long)mt.tv_sec,mt.tv_nsec/1000,(unsigned long long)absolute_us);
    va_list ap;va_start(ap,format);vsnprintf(text+n,sizeof(text)-(size_t)n,format,ap);va_end(ap);
    size_t len=strlen(text);text[len++]='\n';write(trace_fd,text,len);
}
#include "receiver_browse.inc"
typedef void *Request;
extern CFTypeRef _CFHTTPServerRequestCopyProperty(Request,CFStringRef);
extern const CFStringRef _kCFHTTPServerRequestMethod;
extern const CFStringRef _kCFHTTPServerRequestURL;
static void request_record(const char *event,void *connection,Request req) {
    CFTypeRef method=req?_CFHTTPServerRequestCopyProperty(req,_kCFHTTPServerRequestMethod):NULL;
    CFTypeRef url=req?_CFHTTPServerRequestCopyProperty(req,_kCFHTTPServerRequestURL):NULL;
    char verb[32]="unknown",path[128]="unknown";
    if(method && CFGetTypeID(method)==CFStringGetTypeID())CFStringGetCString(method,verb,sizeof(verb),kCFStringEncodingUTF8);
    if(url && CFGetTypeID(url)==CFURLGetTypeID()) {
        CFStringRef component=CFURLCopyPath(url);
        if(component){CFStringGetCString(component,path,sizeof(path),kCFStringEncodingUTF8);CFRelease(component);}
    }
    record("%s conn=%p request=%p method=%s path=%s",event,connection,req,verb,path);
    if(method)CFRelease(method);if(url)CFRelease(url);
}
#include "upload_stream_observe.inc"
extern void *_CFHTTPServerResponseCreateWithData(Request,CFHTTPMessageRef,CFDataRef);
static void *create_response(Request req,CFHTTPMessageRef message,CFDataRef body) {
    @autoreleasepool {
        NSData *selected=body?(__bridge NSData *)body:nil;
        request_record("http_response_request",discover_connection,req);
        record("http_response_status request=%p status=%ld bytes=%ld",req,
            message?(long)CFHTTPMessageGetResponseStatusCode(message):0L,body?(long)CFDataGetLength(body):0L);
        if(discover_connection) {
            CFIndex size=body?CFDataGetLength(body):0;
            record("discover_body conn=%p request=%p status=%ld bytes=%ld",discover_connection,req,
                message?(long)CFHTTPMessageGetResponseStatusCode(message):0L,(long)size);
            if(size>0 && size<=65536) {
                NSPropertyListFormat format=NSPropertyListBinaryFormat_v1_0;
                id plist=[NSPropertyListSerialization propertyListWithData:(__bridge NSData *)body options:0 format:&format error:NULL];
                if([plist isKindOfClass:[NSDictionary class]]) {
                    for(id key in [(NSDictionary *)plist allKeys]) {
                        id value=plist[key];NSString *description;
                        if([value isKindOfClass:[NSData class]])description=[NSString stringWithFormat:@"data:%lu",(unsigned long)[value length]];
                        else if([value isKindOfClass:[NSString class]] || [value isKindOfClass:[NSNumber class]])description=[value description];
                        else description=NSStringFromClass([value class]);
                        record("discover_field conn=%p key=%s value=%.512s",discover_connection,[[key description] UTF8String],[description UTF8String]);
                    }
#ifdef HK_DISCOVER_CAPABILITY
                    NSDictionary *updated=HKDiscoverCapability(plist,message?(NSInteger)CFHTTPMessageGetResponseStatusCode(message):0);
                    if(updated){
                        NSData *encoded=[NSPropertyListSerialization dataWithPropertyList:updated format:format options:0 error:NULL];
                        if(encoded){selected=encoded;record("discover_capability conn=%p added=1 original_bytes=%ld bytes=%lu",discover_connection,(long)size,(unsigned long)encoded.length);}
                    }
#endif
                } else record("discover_body conn=%p plist_invalid=1",discover_connection);
            }
        }
        return _CFHTTPServerResponseCreateWithData(req,message,selected?(__bridge CFDataRef)selected:body);
    }
}
__attribute__((used,section("__DATA,__interpose"))) static const struct {const void *replacement,*original;} interpose[]={
    {(const void *)create_response,(const void *)_CFHTTPServerResponseCreateWithData}
};
static void (*forward_discover)(id,SEL,NSInteger,id);
static void discover(id self,SEL sel,NSInteger code,id alias) {
    void *previous=discover_connection;discover_connection=(__bridge void *)self;
    record("discover_enter conn=%p code=%ld known=%d",discover_connection,(long)code,alias!=nil);
    @try {forward_discover(self,sel,code,alias);}
    @finally {record("discover_return conn=%p",discover_connection);discover_connection=previous;}
}
static void (*forward_sent)(id,SEL,void *,Request);
static void sent(id self,SEL sel,void *response,Request req) {
    record("response_sent conn=%p request=%p response=%p",(__bridge void *)self,req,response);
    forward_sent(self,sel,response,req);
}
static void (*forward_zipper)(id,SEL,id,NSInteger,void *);
static void zipper(id self,SEL sel,id z,NSInteger event,void *property) {
    if(event!=7)record("zipper conn=%p event=%ld",(__bridge void *)self,(long)event);
    if(event!=7)upload_checkpoint((__bridge void*)self,"zipper");
    forward_zipper(self,sel,z,event,property);
}
static void (*forward_error)(id,SEL,CFErrorRef);
static void error(id self,SEL sel,CFErrorRef err) {
    record("connection_error conn=%p code=%ld",(__bridge void *)self,err?(long)CFErrorGetCode(err):0L);
    upload_checkpoint((__bridge void*)self,"connection_error");
    forward_error(self,sel,err);
}
static void (*forward_request)(id,SEL,Request);
static void received_request(id self,SEL sel,Request req){
    request_record("http_request_enter",(__bridge void *)self,req);
    upload_request_begin(req,(__bridge void*)self);
    forward_request(self,sel,req);
}
static void (*forward_ask)(id,SEL,NSInteger);
static void ask(id self,SEL sel,NSInteger code){
    record("ask_response conn=%p status=%ld",(__bridge void *)self,(long)code);
    forward_ask(self,sel,code);
}
static IMP install_for_class(const char *class_name,const char *name,const char *encoding,uintptr_t address,IMP hook) {
    Method m=class_getInstanceMethod(objc_getClass(class_name),sel_registerName(name));
    IMP current=m?method_getImplementation(m):NULL;Dl_info image={0};
    BOOL matched=NO;uintptr_t preferred=0;
    if(current && dladdr((void *)current,&image)) {
        const struct mach_header_64 *h=image.dli_fbase;
        static const unsigned char uuid[16]={46,79,175,64,30,196,56,230,172,131,151,10,167,26,27,144};
        if(h && h->magic==MH_MAGIC_64) {
            const struct load_command *c=(const void *)(h+1);
            for(unsigned i=0;i<h->ncmds;i++) {
                if(c->cmd==LC_UUID && c->cmdsize>=sizeof(struct uuid_command))matched=!memcmp(((const struct uuid_command *)c)->uuid,uuid,16);
                if(c->cmd==LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)c)->segname,"__TEXT"))preferred=((const struct segment_command_64 *)c)->vmaddr;
                c=(const void *)((const char *)c+c->cmdsize);
            }
        }
    }
    if(!m || strcmp(method_getTypeEncoding(m),encoding) || !matched ||
       (uintptr_t)current-(uintptr_t)image.dli_fbase+preferred!=address) {
        record("hook_rejected selector=%s types=%s imp=%p base=%p preferred=%llx uuid=%d",name,m?method_getTypeEncoding(m):"missing",current,image.dli_fbase,(unsigned long long)preferred,matched);return NULL;
    }
    if(!hook){record("method_verified class=%s selector=%s",class_name,name);return current;}
    IMP original=method_setImplementation(m,hook);record("hook_installed selector=%s",name);return original;
}
static IMP install(const char *name,const char *encoding,uintptr_t address,IMP hook){
 return install_for_class("SDAirDropConnection",name,encoding,address,hook);
}
#include "receiver_server.inc"
__attribute__((constructor)) static void start(void) {
    trace_fd=open("/private/var/tmp/airdrop-discover.log",O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
    record("observer_loaded pid=%d",getpid());
    receiver_configure();
    dispatch_async(dispatch_get_main_queue(),^{
        forward_request=(void *)install("didReceiveRequest:","v24@0:8^{_CFHTTPServerRequest=}16",0x100197924,(IMP)received_request);
        forward_ask=(void *)install("sendAskResponse:","v24@0:8q16",0x100194dcc,(IMP)ask);
        forward_discover=(void *)install("sendDiscoverResponse:forKnownAlias:","v32@0:8q16@24",0x100194994,(IMP)discover);
        forward_sent=(void *)install("didSendResponse:forRequest:","v32@0:8^{_CFHTTPServerResponse=}16^{_CFHTTPServerRequest=}24",0x1001985e4,(IMP)sent);
        forward_zipper=(void *)install("fileZipper:event:withProperty:","v40@0:8@16q24^v32",0x100198c40,(IMP)zipper);
        forward_error=(void *)install("didReceiveError:","v24@0:8^{__CFError=}16",0x100193be0,(IMP)error);
        bool receiver_original=receiver_server_install();
        receiver_authorize(forward_request && forward_ask && forward_discover && forward_sent && receiver_original);
    });
}
