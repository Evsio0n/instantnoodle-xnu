/* Standard IOService lookup/notifications from the ordinary mobile account.
 * Requests no privileges, sends no external methods and maps no memory. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <unistd.h>

struct watch {
    const char *class_name;
    const char *protocol;
    const char *event;
    io_iterator_t iter;
};
static CFMutableDictionaryRef matching(const char *cls,const char *protocol) {
    CFMutableDictionaryRef d=IOServiceMatching(cls);
    if (!d || !protocol) return d;
    CFStringRef p=CFStringCreateWithCString(NULL,protocol,kCFStringEncodingASCII);
    if (!p) { CFRelease(d); return NULL; }
    CFDictionarySetValue(d,CFSTR("ABPInterfaceProtocol"),p);
    CFRelease(p);
    return d;
}
static uint64_t number(io_service_t service,CFStringRef key) {
    CFTypeRef v=IORegistryEntryCreateCFProperty(service,key,NULL,0);
    int64_t n=0;
    if (v) {
        if (CFGetTypeID(v)==CFNumberGetTypeID())
            (void)CFNumberGetValue((CFNumberRef)v,kCFNumberSInt64Type,&n);
        CFRelease(v);
    }
    return (uint64_t)n;
}
static void notify(void *arg,io_iterator_t iterator) {
    struct watch *w=arg;
    io_service_t s;
    while ((s=IOIteratorNext(iterator))) {
        uint64_t entry=0;
        kern_return_t id_status=IORegistryEntryGetRegistryEntryID(s,&entry);
        CFTypeRef prop=IORegistryEntryCreateCFProperty(s,CFSTR("ABPInterfaceProtocol"),NULL,0);
        CFStringRef want=CFStringCreateWithCString(NULL,w->protocol,kCFStringEncodingASCII);
        bool exact=prop && want && CFGetTypeID(prop)==CFStringGetTypeID() && CFEqual(prop,want);
        if (prop) CFRelease(prop);
        if (want) CFRelease(want);
        printf("qmux-registry: class=%s protocol=%s event=%s entry=%llu id_status=%d exact=%d generation=%llu node=%llu peer=%llu local=%llu cid=%llu end\n",
            w->class_name,w->protocol,w->event,(unsigned long long)entry,id_status,exact,
            (unsigned long long)number(s,CFSTR("HKTransportGeneration")),(unsigned long long)number(s,CFSTR("HKQRTRNode")),
            (unsigned long long)number(s,CFSTR("HKQRTRPeerPort")),(unsigned long long)number(s,CFSTR("HKQRTRLocalPort")),
            (unsigned long long)number(s,CFSTR("HKQMUXClient")));
        if (!strcmp(w->class_name,"HKCellularQMUXChannel") && !strcmp(w->event,"present") && exact) {
            io_connect_t connection=IO_OBJECT_NULL;
            IOReturn result=IOServiceOpen(s,mach_task_self(),0,&connection);
            printf("qmux-registry: ordinary_open uid=%u protocol=%s entry=%llu type=0 kr=%#x handle=%d end\n",
                geteuid(),w->protocol,(unsigned long long)entry,(unsigned)result,connection!=IO_OBJECT_NULL);
            if (connection) IOServiceClose(connection);
        }
        IOObjectRelease(s);
    }
}
static int count(const char *cls,const char *protocol) {
    CFMutableDictionaryRef match=matching(cls,protocol);
    io_iterator_t iter=0;
    if(!match || IOServiceGetMatchingServices(MACH_PORT_NULL,match,&iter)!=KERN_SUCCESS) return -1;
    if(!iter) return 0;
    int n=0;io_service_t s;
    while((s=IOIteratorNext(iter))){n++;IOObjectRelease(s);}
    if(!IOIteratorIsValid(iter))n=-1;
    IOObjectRelease(iter);return n;
}
int main(void) {
    if(getuid()!=501 || geteuid()!=501)return 2;
    setvbuf(stdout,NULL,_IOLBF,0);
    IONotificationPortRef port=IONotificationPortCreate(MACH_PORT_NULL);
    if(!port)return 2;
    CFRunLoopSourceRef source=IONotificationPortGetRunLoopSource(port);
    if(!source){IONotificationPortDestroy(port);return 2;}
    CFRunLoopAddSource(CFRunLoopGetCurrent(),source,kCFRunLoopDefaultMode);
    const char *cls="HKCellularQMUXChannel";
    struct watch watches[]={{cls,"qmux1","present",0},{cls,"qmux1","gone",0},{cls,"qmux2","present",0},
        {cls,"qmux2","gone",0},{cls,"not-a-channel","present",0},
        {"AppleBasebandPCIInterface","qmux1","present",0},{"AppleBasebandPCIInterface","qmux2","present",0}};
    bool installed=true;
    for(unsigned i=0;i<sizeof(watches)/sizeof(watches[0]);i++){
        struct watch *w=&watches[i];CFMutableDictionaryRef query=matching(w->class_name,w->protocol);
        const char *which=!strcmp(w->event,"gone")?kIOTerminatedNotification:kIOFirstMatchNotification;
        if(!query || IOServiceAddMatchingNotification(port,which,query,notify,w,&w->iter)!=KERN_SUCCESS){
            installed=false;break;
        }
        notify(w,w->iter);
    }
    printf("qmux-registry: subscriptions=%d positive=%d negative=%d apple_control=%d end\n",installed,
        count("IOResources",NULL),count(cls,"not-a-channel"),count("AppleBasebandPCIControl",NULL));
    if(installed){
        for(int i=0;i<72;i++){
            CFRunLoopRunInMode(kCFRunLoopDefaultMode,3,false);
            int first=count(cls,"qmux1"),second=count(cls,"qmux2");
            printf("qmux-registry: lookup tick=%d first=%d second=%d negative=%d stock1=%d stock2=%d end\n",i,
                   first,second,count(cls,"not-a-channel"),count("AppleBasebandPCIInterface","qmux1"),
                   count("AppleBasebandPCIInterface","qmux2"));
        }
    }
    for(unsigned i=0;i<sizeof(watches)/sizeof(watches[0]);i++)if(watches[i].iter)IOObjectRelease(watches[i].iter);
    CFRunLoopRemoveSource(CFRunLoopGetCurrent(),source,kCFRunLoopDefaultMode);
    IONotificationPortDestroy(port);
    return installed?0:2;
}
