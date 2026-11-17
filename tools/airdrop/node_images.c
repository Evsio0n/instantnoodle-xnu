/* Read-only image inventory for the explicitly selected sharingd test pid.
 * No remote execution, memory writes, attach, credentials or key material. */
#include <mach/mach.h>

#include <mach/task_info.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <errno.h>
#include <unistd.h>
_Static_assert(sizeof(vm_address_t)==8,"64-bit remote address ABI");
static int read_task(task_t task,uint64_t address,void *out,size_t bytes){vm_size_t got=0;return vm_read_overwrite(task,(vm_address_t)address,(vm_size_t)bytes,(vm_address_t)out,&got)==KERN_SUCCESS && got==bytes;}
int main(int argc,char **argv){
 if(argc!=2)return 2;char *end;long pid=strtol(argv[1],&end,10);if(*end || pid<=1 || pid>1000000)return 2;
 int mib[]={CTL_KERN,KERN_PROCARGS2,(int)pid};size_t n=0;int env_error=0,env_library=0,env_enabled=0;
 if(sysctl(mib,3,NULL,&n,NULL,0))env_error=errno;
 if(!env_error && n && n<=1024*1024){char *b=calloc(1,n+1);if(sysctl(mib,3,b,&n,NULL,0))env_error=errno;else for(size_t i=0;i<n;i++){
  if(n-i>=21 && !memcmp(b+i,"DYLD_INSERT_LIBRARIES=",21))env_library=strstr(b+i,"libbonjour_node_key")!=NULL;
  if(n-i>=sizeof("HK_AIRDROP_BONJOUR_LOCAL_KEY=1")-1 && !strcmp(b+i,"HK_AIRDROP_BONJOUR_LOCAL_KEY=1"))env_enabled=1;
 }free(b);}
 task_t task=MACH_PORT_NULL;kern_return_t kr=task_for_pid(mach_task_self(),(int)pid,&task);
 printf("{\"pid\":%ld,\"env_errno\":%d,\"env_library\":%d,\"env_enabled\":%d,\"task_for_pid\":%d",pid,env_error,env_library,env_enabled,kr);
 if(kr!=KERN_SUCCESS){puts("}");return 1;}
 struct task_dyld_info info={0};mach_msg_type_number_t count=TASK_DYLD_INFO_COUNT;kr=task_info(task,TASK_DYLD_INFO,(task_info_t)&info,&count);
 struct {uint32_t version,count;uint64_t array;} images={0};
 int ok=kr==KERN_SUCCESS && read_task(task,info.all_image_info_addr,&images,sizeof(images));
 unsigned found=0,read=0;
 if(ok && images.count<=4096)for(unsigned i=0;i<images.count;i++){
  struct {uint64_t header,path,date;} entry={0};if(!read_task(task,images.array+i*sizeof(entry),&entry,sizeof(entry)))continue;
  char path[1024]={0};size_t page=(size_t)getpagesize(),bytes=page-(entry.path%page);if(bytes>sizeof(path)-1)bytes=sizeof(path)-1;
  if(!entry.path || !read_task(task,entry.path,path,bytes))continue;read++;
  if(strstr(path,"libbonjour_node_key.dylib"))found++;
 }
 printf(",\"task_info\":%d,\"image_header_read\":%d,\"images\":%u,\"image_paths_read\":%u,\"node_library_loaded\":%u}\n",kr,ok,images.count,read,found);
 mach_port_deallocate(mach_task_self(),task);return ok?0:1;
}
