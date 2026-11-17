#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <dispatch/dispatch.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#import "EmptyExtensionPolicy.h"
static void (*originalConsume)(id,SEL);
static Ivar tokensIvar;
static void consume(id self,SEL command){
 NSDictionary *tokens=object_getIvar(self,tokensIvar);NSUInteger removed=0;
 NSDictionary *filtered=HKWithoutEmptyExtensionData(tokens,&removed);
 if(!filtered){originalConsume(self,command);return;}
 fprintf(stderr,"airdrop-empty-extension: excluded %lu zero-length local placeholders; native valid tokens unchanged\n",(unsigned long)removed);
 object_setIvarWithStrongDefault(self,tokensIvar,filtered);
 @try{originalConsume(self,command);}
 @finally{object_setIvarWithStrongDefault(self,tokensIvar,tokens);}
}
static void install(void){
 const char *enabled=getenv("HK_AIRDROP_EMPTY_EXTENSION_FIX");if(!enabled || strcmp(enabled,"1"))return;
 Class cls=objc_getClass("SDAirDropSession");SEL sel=sel_registerName("consumeSandboxExtensions");Method method=class_getInstanceMethod(cls,sel);
 if(!method || strcmp(method_getTypeEncoding(method),"v16@0:8"))return;
 IMP imp=method_getImplementation(method);Dl_info image={0};if(!dladdr((void *)imp,&image))return;
 const struct mach_header_64 *h=image.dli_fbase;if(!h || h->magic!=MH_MAGIC_64)return;
 static const unsigned char uuid[16]={46,79,175,64,30,196,56,230,172,131,151,10,167,26,27,144};
 BOOL matched=NO;uint64_t preferred=0;const struct load_command *c=(const void *)(h+1);
 for(unsigned i=0;i<h->ncmds;i++){
  if(c->cmd==LC_UUID && c->cmdsize>=sizeof(struct uuid_command))matched=!memcmp(((const struct uuid_command *)c)->uuid,uuid,16);
  if(c->cmd==LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)c)->segname,"__TEXT"))preferred=((const struct segment_command_64 *)c)->vmaddr;
  c=(const void *)((const char *)c+c->cmdsize);
 }
 if(!matched || (uintptr_t)imp-(uintptr_t)h+preferred!=0x100016df4ull)return;
 tokensIvar=class_getInstanceVariable(cls,"_sandboxExtensions");if(!tokensIvar || ivar_getTypeEncoding(tokensIvar)[0]!='@')return;
 originalConsume=(void(*)(id,SEL))imp;method_setImplementation(method,(IMP)consume);
 fprintf(stderr,"airdrop-empty-extension: installed exact20F66 zero-length input normalization\n");
}
__attribute__((constructor)) static void start(void){dispatch_async(dispatch_get_main_queue(),^{install();});}
