/* Raw Apple80211 arguments from the 20F66 peer manager, before IOCTL
 * routing. Retain command metadata and timing only. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <mach/mach_time.h>
struct request { char name[16];int32_t type,value; uint32_t size,pad;void *bytes; };
extern int Apple80211RawSet(void *,struct request *);
extern int Apple80211RawGet(void *,struct request *);
static int fd=-1;
__attribute__((constructor)) static void open_log(void){
 fd=open("/private/var/tmp/p2p-requests.log",O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
}
static int record(void *h,struct request *r,int set){
 if(fd>=0 && set && r && (r->type==120 || r->type==164) && r->bytes && r->size<=512){
  const uint8_t *b=r->bytes;
  char hex[1025];
  static const char digits[]="0123456789abcdef";
  for(uint32_t i=0;i<r->size;i++){hex[i*2]=digits[b[i]>>4];hex[i*2+1]=digits[b[i]&15];}
  hex[2*r->size]=0;
  struct timespec wall;clock_gettime(CLOCK_REALTIME,&wall);
  mach_timebase_info_data_t tb;mach_timebase_info(&tb);
  uint64_t us=(uint64_t)((__uint128_t)mach_absolute_time()*tb.numer/tb.denom/1000);
  dprintf(fd,"request-clock type=%d wall=%lld.%06ld absolute_us=%llu\n",r->type,(long long)wall.tv_sec,wall.tv_nsec/1000,(unsigned long long)us);
  dprintf(fd,"%s-request size=%u data=%s\n",r->type==120?"service":"traffic",r->size,hex);
 }
 int rc=set?Apple80211RawSet(h,r):Apple80211RawGet(h,r);
 int e=errno;
 if(fd>=0 && r)dprintf(fd,"%s %.16s type=%d size=%u val=%d rc=%d errno=%d\n",set?"set":"get",r->name,r->type,r->size,r->value,rc,rc?e:0);
 errno=e;return rc;
}
static int get(void*h,struct request*r){return record(h,r,0);}
static int set(void*h,struct request*r){return record(h,r,1);}
__attribute__((used,section("__DATA,__interpose"))) static const struct {const void *new_fn,*old_fn;} entries[]={
 {(const void *)get,(const void *)Apple80211RawGet},{(const void *)set,(const void *)Apple80211RawSet}
};
