// airdump SRC.metal OUT.metallib: what this Mac's Metal compiler makes of a source, through the private -serializeToURL:error: of _MTLLibrary
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
int main(int argc,char**argv){setbuf(stdout,NULL);@autoreleasepool{
  NSString*src=[NSString stringWithContentsOfFile:[NSString stringWithUTF8String:argv[1]] encoding:NSUTF8StringEncoding error:nil];
  id<MTLDevice> d=MTLCreateSystemDefaultDevice(); MTLCompileOptions*o=[MTLCompileOptions new]; o.languageVersion=MTLLanguageVersion2_4;
  NSError*e=nil; id<MTLLibrary> l=[d newLibraryWithSource:src options:o error:&e];
  if(!l){printf("compile: %s\n",e.description.UTF8String);return 1;}
  printf("functions:"); for(NSString*n in l.functionNames)printf(" %s",n.UTF8String); printf("\n");
  if(argc>3 && !strcmp(argv[3],"bitcode")){ id data=[(id)l performSelector:NSSelectorFromString(@"bitcodeData")]; printf("class %s\n",data?object_getClassName(data):"nil"); if(data){[(NSData*)data writeToFile:[NSString stringWithUTF8String:argv[2]] atomically:NO];} return 0; }
  NSURL*u=[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[2]]];
  NSMethodSignature*sig=[(id)l methodSignatureForSelector:NSSelectorFromString(@"serializeToURL:error:")];
  NSInvocation*inv=[NSInvocation invocationWithMethodSignature:sig]; inv.selector=NSSelectorFromString(@"serializeToURL:error:"); inv.target=l; [inv setArgument:&u atIndex:2]; NSError*__autoreleasing e2=nil; NSError*__autoreleasing*pe=&e2; [inv setArgument:&pe atIndex:3]; [inv invoke]; BOOL ok=NO; [inv getReturnValue:&ok]; printf("serialize %d %s\n",ok,e2?e2.description.UTF8String:"");
}return 0;}
