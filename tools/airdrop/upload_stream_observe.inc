/* Observe only original /Upload body streams. No body bytes are retained,
 * no client callbacks or read results are changed. Counters distinguish a
 * complete HTTP body from file-progress reaching100% before stream end. */
#include <pthread.h>
extern CFReadStreamRef _CFHTTPServerRequestCopyBodyStream(Request);
extern const CFStringRef _kCFHTTPServerRequestHeaders;
extern const CFStringRef _kCFHTTPServerRequestHeaderValuesKey;
static pthread_mutex_t upload_lock=PTHREAD_MUTEX_INITIALIZER;
static struct upload_stream_record {Request request;void *connection;CFReadStreamRef stream;uint64_t bytes,reads,next_report;CFStreamStatus last_status;unsigned epoch;} upload_streams[16];
static unsigned upload_epoch;
static bool upload_request(Request req){
 CFTypeRef value=_CFHTTPServerRequestCopyProperty(req,_kCFHTTPServerRequestURL);bool result=false;
 if(value && CFGetTypeID(value)==CFURLGetTypeID()){
  CFStringRef path=CFURLCopyPath(value);result=path && CFEqual(path,CFSTR("/Upload"));if(path)CFRelease(path);
 }
 if(value)CFRelease(value);return result;
}
static void upload_request_begin(Request req,void *connection){
 if(!req || !upload_request(req))return;
 pthread_mutex_lock(&upload_lock);int slot=-1;
 for(unsigned i=0;i<16;i++){if(upload_streams[i].request==req){slot=(int)i;break;}if(slot<0 && !upload_streams[i].request)slot=(int)i;}
 if(slot>=0){memset(&upload_streams[slot],0,sizeof(upload_streams[slot]));upload_streams[slot].request=req;upload_streams[slot].connection=connection;upload_streams[slot].epoch=++upload_epoch;}
 pthread_mutex_unlock(&upload_lock);
 CFTypeRef headers=_CFHTTPServerRequestCopyProperty(req,_kCFHTTPServerRequestHeaders);
 record("upload_body_begin conn=%p request=%p tracked=%u headers_dictionary=%u",connection,req,slot>=0,headers && CFGetTypeID(headers)==CFDictionaryGetTypeID());
 if(headers && CFGetTypeID(headers)==CFDictionaryGetTypeID()){
  CFDictionaryRef values=headers;CFTypeRef nested=CFDictionaryGetValue(headers,_kCFHTTPServerRequestHeaderValuesKey);
  if(nested && CFGetTypeID(nested)==CFDictionaryGetTypeID())values=nested;
  const char *keys[]={"Content-Length","Transfer-Encoding","Content-Encoding","Content-Type","Expect","Connection"};
  for(unsigned i=0;i<6;i++){
   CFStringRef key=CFStringCreateWithCString(NULL,keys[i],kCFStringEncodingUTF8);
   CFTypeRef value=CFDictionaryGetValue(values,key);
   if(!value){CFMutableStringRef lower=CFStringCreateMutableCopy(NULL,0,key);CFStringLowercase(lower,NULL);value=CFDictionaryGetValue(values,lower);CFRelease(lower);}
   if(value && CFGetTypeID(value)==CFArrayGetTypeID())value=CFArrayGetCount(value)?CFArrayGetValueAtIndex(value,0):NULL;
   char text[192]="unavailable";
   if(value && CFGetTypeID(value)==CFStringGetTypeID())CFStringGetCString(value,text,sizeof(text),kCFStringEncodingUTF8);
   else if(value && CFGetTypeID(value)==CFNumberGetTypeID()){long long number=0;CFNumberGetValue(value,kCFNumberLongLongType,&number);snprintf(text,sizeof(text),"%lld",number);}
   if(value)record("upload_header request=%p key=%s value=%s",req,keys[i],text);
   CFRelease(key);
  }
 }
 if(headers)CFRelease(headers);
}
static CFReadStreamRef upload_copy_body(Request req){
 CFReadStreamRef stream=_CFHTTPServerRequestCopyBodyStream(req);
 CFStreamStatus status=stream?CFReadStreamGetStatus(stream):kCFStreamStatusError;
 pthread_mutex_lock(&upload_lock);unsigned epoch=0;
 for(unsigned i=0;i<16;i++)if(upload_streams[i].request==req){upload_streams[i].stream=stream;upload_streams[i].last_status=status;epoch=upload_streams[i].epoch;break;}
 pthread_mutex_unlock(&upload_lock);
 if(epoch)record("upload_body_stream request=%p stream=%p epoch=%u",req,stream,epoch);
 return stream;
}
static CFIndex upload_read(CFReadStreamRef stream,UInt8 *bytes,CFIndex cap){
 unsigned epoch=0;pthread_mutex_lock(&upload_lock);
 for(unsigned i=0;i<16;i++)if(upload_streams[i].stream==stream){epoch=upload_streams[i].epoch;break;}
 pthread_mutex_unlock(&upload_lock);
 CFIndex n=CFReadStreamRead(stream,bytes,cap);
 if(!epoch)return n;
 CFStreamStatus status=CFReadStreamGetStatus(stream);
 struct upload_stream_record snapshot={0};bool report=false;
 pthread_mutex_lock(&upload_lock);
 for(unsigned i=0;i<16;i++){struct upload_stream_record *r=&upload_streams[i];if(r->stream==stream && r->epoch==epoch){
  r->last_status=status;r->reads++;if(n>0)r->bytes+=(uint64_t)n;
  report=n<=0 || r->bytes>=r->next_report;
  if(report){r->next_report=r->bytes+1048576;snapshot=*r;}break;
 }}
 pthread_mutex_unlock(&upload_lock);
 if(report)record("upload_body_read conn=%p stream=%p epoch=%u n=%ld bytes=%llu reads=%llu status=%ld",snapshot.connection,stream,epoch,(long)n,(unsigned long long)snapshot.bytes,(unsigned long long)snapshot.reads,(long)snapshot.last_status);
 return n;
}
static void upload_close(CFReadStreamRef stream){
 struct upload_stream_record snapshot={0};
 pthread_mutex_lock(&upload_lock);
 for(unsigned i=0;i<16;i++)if(upload_streams[i].stream==stream){snapshot=upload_streams[i];memset(&upload_streams[i],0,sizeof(upload_streams[i]));break;}
 pthread_mutex_unlock(&upload_lock);
 if(snapshot.epoch)record("upload_body_close conn=%p stream=%p epoch=%u bytes=%llu reads=%llu status=%ld",snapshot.connection,stream,snapshot.epoch,(unsigned long long)snapshot.bytes,(unsigned long long)snapshot.reads,(long)CFReadStreamGetStatus(stream));
 CFReadStreamClose(stream);
}
static void upload_checkpoint(void *connection,const char *event){
 struct upload_stream_record snapshots[16];unsigned count=0;
 pthread_mutex_lock(&upload_lock);
 for(unsigned i=0;i<16;i++)if(upload_streams[i].request && upload_streams[i].connection==connection){snapshots[count++]=upload_streams[i];}
 pthread_mutex_unlock(&upload_lock);
 for(unsigned i=0;i<count;i++){
  struct upload_stream_record *r=&snapshots[i];record("upload_body_checkpoint event=%s conn=%p stream=%p epoch=%u bytes=%llu reads=%llu last_status=%ld",event,connection,r->stream,r->epoch,(unsigned long long)r->bytes,(unsigned long long)r->reads,(long)r->last_status);
 }
}
__attribute__((used,section("__DATA,__interpose"))) static const struct {const void *replacement,*original;} upload_interpose[]={
 {(const void*)upload_copy_body,(const void*)_CFHTTPServerRequestCopyBodyStream},
 {(const void*)upload_read,(const void*)CFReadStreamRead},
 {(const void*)upload_close,(const void*)CFReadStreamClose}
};
