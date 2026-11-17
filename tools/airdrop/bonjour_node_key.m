#import <Foundation/Foundation.h>
#import <CoreFoundation/CoreFoundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <fcntl.h>
#include <dispatch/dispatch.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#import "BonjourLocalKey.h"
#import "UnknownReceiverPolicy.h"
typedef CFTypeRef (*NodeCopy)(CFTypeRef);
typedef void (*NodeSet)(CFTypeRef,CFStringRef);
static NodeCopy copyKinds,copyService,copyComputer,copyUser,copyContact;
static NodeSet setUser;
static CFStringRef bonjourKind;
static id (*original)(id,SEL,id);
static void trace(const char *text);
static id (*originalPredicate)(id,SEL,id);
static const void *nativePeoplePredicate;
static id makePredicate(id self,SEL command,id block){
 if((__bridge const void *)block!=nativePeoplePredicate)return originalPredicate(self,command,block);
 BOOL (^native)(id,NSDictionary *)=block;
 BOOL (^include)(id,NSDictionary *)=^BOOL(id node,NSDictionary *bindings){
  BOOL accepted=native(node,bindings);if(accepted)return YES;
  if(![node isKindOfClass:objc_getClass("SFAirDropNode")])return NO;
  SEL un=sel_registerName("isUnknown"),ident=sel_registerName("nodeIdentifier"),name=sel_registerName("displayName"),transport=sel_registerName("transportBundleID");
  if(![node respondsToSelector:un] || ![node respondsToSelector:ident] || ![node respondsToSelector:name] || ![node respondsToSelector:transport])return NO;
  BOOL unknown=((BOOL(*)(id,SEL))objc_msgSend)(node,un);
  id i=((id(*)(id,SEL))objc_msgSend)(node,ident),n=((id(*)(id,SEL))objc_msgSend)(node,name),t=((id(*)(id,SEL))objc_msgSend)(node,transport);
  BOOL listed=HKListUnknownReceiver(accepted,unknown,i,n,t);
  if(listed){static unsigned count;if(count++<16)trace("UI included real native unknown AirDrop receiver; identity unchanged");}
  return listed;
 };
 return originalPredicate(self,command,include);
}
static BOOL observeOnly;
static unsigned applied,checked,returned;
static id (*originalNodes)(id,SEL);
static void trace(const char *text){
 int fd=open("/private/var/tmp/airdrop-node-key-status",O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0600);
 if(fd>=0){write(fd,text,strlen(text));write(fd,"\n",1);close(fd);}
}
static NSString *stringValue(CFTypeRef x){return x && CFGetTypeID(x)==CFStringGetTypeID()?(__bridge NSString *)x:nil;}
static id groupNodes(id self,SEL command,id nodes){
 @autoreleasepool {
  if([nodes isKindOfClass:NSArray.class] && [nodes count]<=256)for(id node in nodes){
   CFTypeRef n=(__bridge CFTypeRef)node,k=copyKinds(n),s=copyService(n),c=copyComputer(n),u=copyUser(n),contact=copyContact(n);
   id kinds=(__bridge id)k;
   BOOL bonjour=[kinds respondsToSelector:@selector(containsObject:)] && [kinds containsObject:(__bridge id)bonjourKind];
   if(checked++<24){char line[192];snprintf(line,sizeof(line),"candidate bonjour=%d network_id=%lu computer=%lu user=%lu contact=%lu",bonjour,(unsigned long)stringValue(s).length,(unsigned long)stringValue(c).length,(unsigned long)stringValue(u).length,(unsigned long)stringValue(contact).length);trace(line);}
   NSString *key=(!observeOnly && !u && !contact)?HKBonjourLocalKey(bonjour,stringValue(s),stringValue(c),nil,nil):nil;
   if(key){trace("applied real network identifier local key");setUser(n,(__bridge CFStringRef)key);if(++applied<=16)fprintf(stderr,"airdrop-node-key: anonymous native Bonjour grouping key supplied; trust flags unchanged\n");}
   if(k)CFRelease(k);if(s)CFRelease(s);if(c)CFRelease(c);if(u)CFRelease(u);if(contact)CFRelease(contact);
  }
 }
 id result=original(self,command,nodes);
 if(returned++<48){
  char line[256];snprintf(line,sizeof(line),"coalescer returned array=%d count=%lu",[result isKindOfClass:NSArray.class],(unsigned long)([result isKindOfClass:NSArray.class]?[result count]:0));trace(line);
  if([result isKindOfClass:NSArray.class] && [result count]<=256)for(id node in result){
   CFTypeRef n=(__bridge CFTypeRef)node,k=copyKinds(n),u=copyUser(n);
   NSString *description=[(__bridge id)k description];trace([[@"native result kinds=" stringByAppendingString:description?:@"nil"] UTF8String]);
   Class model=objc_getClass("SFAirDropNode");SEL make=sel_registerName("nodeWithSFNode:");
   if([model respondsToSelector:make]){
    id snapshot=((id(*)(id,SEL,CFTypeRef))objc_msgSend)(model,make,n);
    SEL unknown=sel_registerName("isUnknown");
    if([snapshot respondsToSelector:unknown]){
     BOOL isUnknown=((BOOL(*)(id,SEL))objc_msgSend)(snapshot,unknown);
     snprintf(line,sizeof(line),"native model snapshot unknown=%d user_length=%lu",isUnknown,(unsigned long)stringValue(u).length);trace(line);
    }
   }
   if(k)CFRelease(k);if(u)CFRelease(u);
  }
 }
 return result;
}
static id observeNodes(id self,SEL command){
 id result=originalNodes(self,command);
 static unsigned calls;if(calls++<64){char line[128];snprintf(line,sizeof(line),"native browser nodes array=%d count=%lu",[result isKindOfClass:NSArray.class],(unsigned long)([result isKindOfClass:NSArray.class]?[result count]:0));trace(line);}
 return result;
}
static void install_checked(void){
 trace("main queue installation entered");
 observeOnly=getenv("HK_AIRDROP_OBSERVE_ONLY") && !strcmp(getenv("HK_AIRDROP_OBSERVE_ONLY"),"1");
 if(observeOnly)trace("observer-only: native grouping and identity untouched");
 const char *enabled=getenv("HK_AIRDROP_BONJOUR_LOCAL_KEY");if(!enabled || strcmp(enabled,"1")){trace("disabled by environment");return;}
 static const unsigned char expected_uuid[16]={46,79,175,64,30,196,56,230,172,131,151,10,167,26,27,144};
 Class cls=objc_getClass("SDNodeCoalescer");SEL sel=sel_registerName("coalescedNodesForNewAvailableNodes:");Method method=cls?class_getInstanceMethod(cls,sel):NULL;
 if(!method || strcmp(method_getTypeEncoding(method),"@24@0:8@16")){trace("native class or method not ready");return;}
 IMP imp=method_getImplementation(method);Dl_info image={0};
 if(!dladdr((const void *)imp,&image) || !image.dli_fbase){trace("cannot identify original method image");return;}
 const struct mach_header_64 *h=image.dli_fbase;BOOL matched=NO;uint64_t preferred=0;
 if(h->magic!=MH_MAGIC_64){trace("original method image is not Mach-O64");return;}
 const struct load_command *lc=(const void *)(h+1);
 for(uint32_t i=0;i<h->ncmds;i++){
  if(lc->cmd==LC_UUID && lc->cmdsize>=sizeof(struct uuid_command))matched=!memcmp(((const struct uuid_command *)lc)->uuid,expected_uuid,16);
  if(lc->cmd==LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)lc)->segname,"__TEXT"))preferred=((const struct segment_command_64 *)lc)->vmaddr;
  lc=(const void *)((const char *)lc+lc->cmdsize);
 }
 if(!matched){trace("original method image UUID mismatch");return;}
 if((uintptr_t)imp-(uintptr_t)h+preferred!=(uintptr_t)0x10010c654){trace("original method offset mismatch");return;}
 trace("original sharingd method image and UUID matched");
 const char *listUnknown=getenv("HK_AIRDROP_LIST_UNKNOWN");
 if(listUnknown && !strcmp(listUnknown,"1")){
  const void *global=(const char *)h+(0x1003b91b8ull-preferred);
  const uintptr_t *layout=global;
  uintptr_t invoke=layout[2];uint32_t blockFlags=((const uint32_t *)global)[2];
  Method pm=class_getClassMethod(NSPredicate.class,sel_registerName("predicateWithBlock:"));
  if(invoke-(uintptr_t)h+preferred==0x100063ae8ull && blockFlags==0x50000000u && pm && !strcmp(method_getTypeEncoding(pm),"@24@0:8@?16")){
   nativePeoplePredicate=global;originalPredicate=(id(*)(id,SEL,id))method_getImplementation(pm);
   method_setImplementation(pm,(IMP)makePredicate);trace("installed exact native people predicate UI inclusion bridge");
  }else trace("UI predicate block address, flags or public factory type mismatch");
 }

 copyKinds=(NodeCopy)dlsym(RTLD_DEFAULT,"SFNodeCopyKinds");copyService=(NodeCopy)dlsym(RTLD_DEFAULT,"SFNodeCopyRealName");
 copyComputer=(NodeCopy)dlsym(RTLD_DEFAULT,"SFNodeCopyComputerName");copyUser=(NodeCopy)dlsym(RTLD_DEFAULT,"SFNodeCopyUserName");
 copyContact=(NodeCopy)dlsym(RTLD_DEFAULT,"SFNodeCopyContactIdentifier");setUser=(NodeSet)dlsym(RTLD_DEFAULT,"SFNodeSetUserName");
 CFStringRef *kind=dlsym(RTLD_DEFAULT,"kSFNodeKindBonjour");bonjourKind=kind?*kind:NULL;
 if(!copyKinds || !copyService || !copyComputer || !copyUser || !copyContact || !setUser || !bonjourKind || !method || strcmp(method_getTypeEncoding(method),"@24@0:8@16")){
  trace("symbols or method contract unavailable");
  fprintf(stderr,"airdrop-node-key: native symbols or method contract unavailable\n");return;
 }
 original=(id(*)(id,SEL,id))imp;method_setImplementation(method,(IMP)groupNodes);trace("installed native grouping hook");
 fprintf(stderr,"airdrop-node-key: installed local Bonjour grouping compatibility\n");
 Method nm=class_getInstanceMethod(objc_getClass("SDNodeBrowser"),sel_registerName("nodes"));
 if(nm && !strcmp(method_getTypeEncoding(nm),"@16@0:8")){
  IMP ni=method_getImplementation(nm);Dl_info nd={0};
  if(dladdr((const void *)ni,&nd) && nd.dli_fbase==(const void *)h && (uintptr_t)ni-(uintptr_t)h+preferred==(uintptr_t)0x10016fae0){
   originalNodes=(id(*)(id,SEL))ni;method_setImplementation(nm,(IMP)observeNodes);trace("installed read-only native browser observer");
  }else trace("native browser observer original image or offset mismatch");
 }
}

__attribute__((constructor)) static void schedule_install(void){
 trace("constructor scheduled main queue installation");
 dispatch_async(dispatch_get_main_queue(),^{install_checked();});
}
