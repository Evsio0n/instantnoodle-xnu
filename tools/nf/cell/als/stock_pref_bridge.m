#import <Foundation/Foundation.h>
#import <CoreFoundation/CoreFoundation.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <grp.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
static int logfd;
typedef unsigned char (*StockAutoBrightnessRead)(void);

static void mobile(int fd) {
 gid_t gid=501;
 if(setgroups(1,&gid)||setgid(gid)||setuid(501)||getuid()!=501||geteuid()!=501)_exit(2);
 void *accessibility=dlopen("/usr/lib/libAccessibility.dylib",RTLD_NOW|RTLD_LOCAL);
 StockAutoBrightnessRead readSetting=accessibility?(StockAutoBrightnessRead)dlsym(accessibility,"_AXSAutoBrightnessEnabled"):NULL;
 for(;;) {
  @autoreleasepool {
   uint8_t value=0;
   if(readSetting) {
    unsigned char pos=readSetting();
    if(pos<=1)value=pos?2:1;
   }
   if(write(fd,&value,1)!=1)break;
   [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1]];
  }
 }
 close(fd);_exit(0);
}
static bool als_enabled(void){
 char args[1024]={0};size_t n=sizeof(args);if(sysctlbyname("kern.bootargs",args,&n,NULL,0)||n>sizeof(args)||!memchr(args,0,n))return false;
 for(char *save=NULL,*t=strtok_r(args," ",&save);t;t=strtok_r(NULL," ",&save))if(!strcmp(t,"als=1"))return true;
 return false;
}
int main(void){@autoreleasepool{
 if(geteuid()!=0||!als_enabled())return 2;
 logfd=open("/dev/console",O_WRONLY|O_CLOEXEC);if(logfd<0)logfd=STDERR_FILENO;
 pid_t daemon=fork();if(daemon<0)return 2;if(daemon>0)return 0;
 int channel[2];if(pipe(channel))_exit(2);
 pid_t child=fork();if(child<0)_exit(2);
 if(!child){close(channel[0]);mobile(channel[1]);}
 close(channel[1]);int previous=-1;
 for(;;){
  struct pollfd p={.fd=channel[0],.events=POLLIN};uint8_t input=0;int wait=poll(&p,1,1800);
  if(wait<0 && errno==EINTR)continue;
  if(wait<0||(wait>0&&!(p.revents&POLLIN)))break;
  if(wait>0&&read(channel[0],&input,1)!=1)break;
  if(input>2)break;
  uint32_t value=input;int rc=sysctlbyname("kern.hackios.als_user_preference",NULL,NULL,&value,sizeof(value));
  if(previous!=input)dprintf(logfd,"light-pref: ordinary mobile Settings _AXSAutoBrightnessEnabled state=%u kernel_result=%d\n",input,rc);
  previous=input;
 }
 uint32_t unknown=0;(void)sysctlbyname("kern.hackios.als_user_preference",NULL,NULL,&unknown,sizeof(unknown));
 kill(child,SIGTERM);waitpid(child,NULL,0);close(channel[0]);_exit(0);
}}
