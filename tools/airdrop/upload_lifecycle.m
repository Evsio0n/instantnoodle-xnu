/* Trace the three native completion boundaries for the 20F66 receiver.
 * Every invocation is forwarded with its original arguments and return. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <mach-o/dyld.h>
#import <dispatch/dispatch.h>
#include <stdio.h>
#include <string.h>

typedef void (*ZipperEvent)(id,SEL,id,NSInteger,void *);
typedef void (*UploadResponse)(id,SEL,NSInteger);
typedef void (*ConnectionError)(id,SEL,CFErrorRef);
static ZipperEvent forward_event;
static UploadResponse forward_response;
static ConnectionError forward_error;
static void zipper_event(id self,SEL sel,id zipper,NSInteger event,void *property){
    if(event!=7)fprintf(stderr,"airdrop-finish: conn=%p zipper_event=%ld\n",(__bridge void *)self,(long)event);
    forward_event(self,sel,zipper,event,property);
}
static void upload_response(id self,SEL sel,NSInteger code){
    fprintf(stderr,"airdrop-finish: conn=%p enqueue_response=%ld\n",(__bridge void *)self,(long)code);
    forward_response(self,sel,code);
}
static void connection_error(id self,SEL sel,CFErrorRef error){
    fprintf(stderr,"airdrop-finish: conn=%p error=%ld\n",(__bridge void *)self,error?(long)CFErrorGetCode(error):0L);
    forward_error(self,sel,error);
}
static Method checked(Class cls,const char *name,const char *type,uintptr_t preferred){
    Method m=class_getInstanceMethod(cls,sel_registerName(name));
    if(!m || strcmp(method_getTypeEncoding(m),type))return NULL;
    return (uintptr_t)method_getImplementation(m)==preferred+_dyld_get_image_vmaddr_slide(0)?m:NULL;
}
__attribute__((constructor)) static void start(void){
    dispatch_async(dispatch_get_main_queue(),^{
        Class cls=objc_getClass("SDAirDropConnection");
        Method event=checked(cls,"fileZipper:event:withProperty:","v40@0:8@16q24^v32",0x100198c40);
        Method response=checked(cls,"sendUploadResponse:","v24@0:8q16",0x1001951fc);
        Method error=checked(cls,"didReceiveError:","v24@0:8^{__CFError=}16",0x100193be0);
        if(!event || !response || !error)return;
        forward_event=(ZipperEvent)method_getImplementation(event);
        forward_response=(UploadResponse)method_getImplementation(response);
        forward_error=(ConnectionError)method_getImplementation(error);
        method_setImplementation(event,(IMP)zipper_event);
        method_setImplementation(response,(IMP)upload_response);
        method_setImplementation(error,(IMP)connection_error);
        fprintf(stderr,"airdrop-finish: observing native 20F66 completion boundaries\n");
    });
}
