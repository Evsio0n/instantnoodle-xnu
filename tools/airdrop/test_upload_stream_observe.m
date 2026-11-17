/* In-memory conformance test for the observer using the real host CF stream.
 * Synthetic request metadata is test-only; no service or network is created. */
#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
struct test_request {CFURLRef url;CFDictionaryRef headers;CFReadStreamRef stream;};
typedef struct test_request *Request;
const CFStringRef _kCFHTTPServerRequestURL=CFSTR("url");
const CFStringRef _kCFHTTPServerRequestHeaders=CFSTR("headers");
const CFStringRef _kCFHTTPServerRequestHeaderValuesKey=CFSTR("values");
static char evidence[32768];static size_t evidence_used;
static void record(const char *fmt,...){
 va_list ap;va_start(ap,fmt);int n=vsnprintf(evidence+evidence_used,sizeof(evidence)-evidence_used,fmt,ap);va_end(ap);
 assert(n>=0 && (size_t)n<sizeof(evidence)-evidence_used);evidence_used+=(size_t)n;evidence[evidence_used++]='\n';evidence[evidence_used]=0;
}
CFTypeRef _CFHTTPServerRequestCopyProperty(Request r,CFStringRef key){
 CFTypeRef value=CFEqual(key,_kCFHTTPServerRequestURL)?(CFTypeRef)r->url:CFEqual(key,_kCFHTTPServerRequestHeaders)?(CFTypeRef)r->headers:NULL;
 return value?CFRetain(value):NULL;
}
CFReadStreamRef _CFHTTPServerRequestCopyBodyStream(Request r){return r->stream?(CFReadStreamRef)CFRetain(r->stream):NULL;}
#include "upload_stream_observe.inc"
int main(void){@autoreleasepool {
 unsigned char source[15000],output[15001];for(unsigned i=0;i<sizeof(source);i++)source[i]=(unsigned char)(i*17);
 CFURLRef url=CFURLCreateWithString(NULL,CFSTR("https://local.invalid/Upload"),NULL);
 NSDictionary *headers=@{@"values":@{@"content-length":@[@"15000"],@"content-encoding":@"gzip",@"Authorization":@"not-a-credential",@"Unrelated":@"private-metadata"}};
 struct test_request req={url,(__bridge CFDictionaryRef)headers,CFReadStreamCreateWithBytesNoCopy(NULL,source,sizeof(source),kCFAllocatorNull)};
 assert(CFReadStreamOpen(req.stream));
 upload_request_begin(&req,(void*)1);CFReadStreamRef stream=upload_copy_body(&req);
 assert(stream==req.stream);
 size_t used=0;CFIndex n;
 while((n=upload_read(stream,output+used,(CFIndex)(sizeof(output)-used?sizeof(output)-used:1)))>0){used+=(size_t)n;assert(used<=sizeof(output));}
 assert(n==0 && used==sizeof(source) && !memcmp(source,output,sizeof(source)));
 assert(upload_streams[0].bytes==sizeof(source));assert(upload_streams[0].last_status==kCFStreamStatusAtEnd);
 upload_checkpoint((void*)1,"test-end");upload_close(stream);CFRelease(stream);CFRelease(req.stream);
 for(unsigned i=0;i<16;i++)assert(!upload_streams[i].request && !upload_streams[i].stream);
 assert(strstr(evidence,"key=Content-Length value=15000"));assert(strstr(evidence,"key=Content-Encoding value=gzip"));
 assert(!strstr(evidence,"not-a-credential") && !strstr(evidence,"private-metadata") && !strstr(evidence,"Authorization"));
 /* Non-upload request is never tracked, and its reads remain unchanged. */
 CFRelease(url);url=CFURLCreateWithString(NULL,CFSTR("https://local.invalid/Discover"),NULL);
 req.url=url;req.stream=CFReadStreamCreateWithBytesNoCopy(NULL,source,7,kCFAllocatorNull);assert(CFReadStreamOpen(req.stream));
 upload_request_begin(&req,(void*)2);stream=upload_copy_body(&req);assert(upload_read(stream,output,7)==7 && !memcmp(source,output,7));
 for(unsigned i=0;i<16;i++)assert(!upload_streams[i].request);
 upload_close(stream);CFRelease(stream);CFRelease(req.stream);CFRelease(url);
 puts("PASS real CF memory stream forwarding/EOF/bytes, nested whitelist headers, close cleanup, non-upload isolation");
 return 0;
}}
