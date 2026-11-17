/* Bounded diagnostics for the owner's current link-local AWDL peer.
 * DNS queries only; no service registration or synthetic discovery record.
 * The sole write option changes the existing Bluetooth debug verbosity. */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/sysctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <spawn.h>
#include <time.h>
extern char **environ;
static int dns_query(const char *peer,const char *name,uint16_t type,unsigned seconds){
 unsigned index=if_nametoindex("awdl0"),one=1;int hops=255,fd=-1,e=0;const char *stage="setup";
 struct sockaddr_in6 dst={0};dst.sin6_len=sizeof(dst);dst.sin6_family=AF_INET6;dst.sin6_port=htons(5353);dst.sin6_scope_id=index;
 if(!index || inet_pton(AF_INET6,peer,&dst.sin6_addr)!=1 || !IN6_IS_ADDR_LINKLOCAL(&dst.sin6_addr))return 2;
 unsigned char q[512]={0x41,0x23,0,0,0,1};size_t n=12;const char *p=name;
 while(*p){const char *dot=strchr(p,'.');size_t k=dot?(size_t)(dot-p):strlen(p);if(!k||k>63||n+k+6>sizeof(q))return 2;q[n++]=(unsigned char)k;memcpy(q+n,p,k);n+=k;p=dot?dot+1:p+k;}
 q[n++]=0;q[n++]=(unsigned char)(type>>8);q[n++]=(unsigned char)type;q[n++]=0;q[n++]=1;
 fd=socket(AF_INET6,SOCK_DGRAM,0);if(fd<0)e=errno;
 if(!e&&setsockopt(fd,SOL_SOCKET,0x1113,&one,sizeof(one)))e=errno;
 if(!e&&setsockopt(fd,IPPROTO_IPV6,IPV6_BOUND_IF,&index,sizeof(index)))e=errno;
 if(!e&&setsockopt(fd,IPPROTO_IPV6,IPV6_UNICAST_HOPS,&hops,sizeof(hops)))e=errno;
 if(!e&&connect(fd,(struct sockaddr *)&dst,sizeof(dst)))e=errno;
 if(!e){stage="send";if(send(fd,q,n,0)!=(ssize_t)n)e=errno?errno:EIO;}
 ssize_t got=0;unsigned char reply[8192];
 if(!e){stage="receive";fd_set f;FD_ZERO(&f);FD_SET(fd,&f);struct timeval tv={(time_t)seconds,0};int r=select(fd+1,&f,0,0,&tv);if(r<0)e=errno;else if(r){got=recv(fd,reply,sizeof(reply),0);if(got<0){e=errno;got=0;}}}
 printf("{\"udp_dns\":1,\"interface\":%u,\"question_type\":%u,\"stage\":\"%s\",\"errno\":%d,\"reply_bytes\":%ld,\"reply_hex\":\"",index,type,stage,e,(long)got);
 for(ssize_t i=0;i<got;i++)printf("%02x",reply[i]);puts("\"}");if(fd>=0)close(fd);return e?1:0;
}

/* Run a single bounded probe without the agent's 15-second RPC lifetime
 * truncating the transport observation. No listener, service registration,
 * shell, credentials or persistent job. Output is a new private temp file. */
static int background_probe(const char *self,const char *mode,const char *peer,unsigned seconds){
 struct in6_addr address;
 if(self[0]!='/' || (strcmp(mode,"--capture-all-worker") && (inet_pton(AF_INET6,peer,&address)!=1 || !IN6_IS_ADDR_LINKLOCAL(&address))))return 2;
 char path[]="/private/var/tmp/awdl-probe-XXXXXX";int fd=mkstemp(path);if(fd<0)return 1;
 posix_spawn_file_actions_t files;posix_spawnattr_t attr;
 int e=posix_spawn_file_actions_init(&files);if(e){close(fd);return 1;}
 e=posix_spawnattr_init(&attr);if(e){posix_spawn_file_actions_destroy(&files);close(fd);return 1;}
 if(!e)e=posix_spawn_file_actions_addopen(&files,0,"/dev/null",O_RDONLY,0);
 if(!e)e=posix_spawn_file_actions_adddup2(&files,fd,1);
 if(!e)e=posix_spawn_file_actions_adddup2(&files,fd,2);
 if(!e)e=posix_spawn_file_actions_addclose(&files,fd);
 if(!e)e=posix_spawnattr_setflags(&attr,POSIX_SPAWN_CLOEXEC_DEFAULT|POSIX_SPAWN_SETPGROUP);
 if(!e)e=posix_spawnattr_setpgroup(&attr,0);
 char *args[]={(char *)self,(char *)mode,(char *)peer,NULL};pid_t pid=0;
 if(!e)e=posix_spawn(&pid,self,&files,&attr,args,environ);
 posix_spawnattr_destroy(&attr);posix_spawn_file_actions_destroy(&files);close(fd);
 printf("{\"background_probe\":1,\"pid\":%d,\"errno\":%d,\"seconds\":%u,\"output\":\"%s\"}\n",pid,e,seconds,path);
 return e?1:0;
}
static double monotonic_seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static int tcp_watch(const char *peer){
 unsigned index=if_nametoindex("awdl0"),one=1;struct sockaddr_in6 target={0},local={0};
 target.sin6_len=sizeof(target);target.sin6_family=AF_INET6;target.sin6_port=htons(8770);target.sin6_scope_id=index;
 if(!index || inet_pton(AF_INET6,peer,&target.sin6_addr)!=1 || !IN6_IS_ADDR_LINKLOCAL(&target.sin6_addr))return 2;
 int e=0,fd=socket(AF_INET6,SOCK_STREAM,0);double start=monotonic_seconds();
 if(fd<0)e=errno;
 if(!e && setsockopt(fd,SOL_SOCKET,0x1113,&one,sizeof(one)))e=errno;
 if(!e && setsockopt(fd,IPPROTO_IPV6,IPV6_BOUND_IF,&index,sizeof(index)))e=errno;
 if(!e && fcntl(fd,F_SETFL,O_NONBLOCK)<0)e=errno;
 int ce=0;if(!e && connect(fd,(struct sockaddr *)&target,sizeof(target))<0){ce=errno;if(ce!=EINPROGRESS)e=ce;}
 socklen_t n=sizeof(local);if(fd>=0)getsockname(fd,(struct sockaddr *)&local,&n);
 printf("{\"tcp_start\":1,\"interface\":%u,\"local_port\":%u,\"connect_errno\":%d,\"setup_errno\":%d}\n",index,ntohs(local.sin6_port),ce,e);fflush(stdout);
 bool ready=false;
 if(fd>=FD_SETSIZE)e=EOVERFLOW;
 while(!e && monotonic_seconds()-start<60){
  fd_set f;FD_ZERO(&f);FD_SET(fd,&f);struct timeval tv={1,0};int r=select(fd+1,NULL,&f,NULL,&tv);
  if(r<0){if(errno==EINTR)continue;e=errno;break;}
  if(r){int status=0;socklen_t size=sizeof(status);if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&status,&size))e=errno;else e=status;ready=true;break;}
 }
 struct sockaddr_in6 actual;socklen_t size=sizeof(actual);int connected=fd>=0 && getpeername(fd,(struct sockaddr *)&actual,&size)==0;
 if(!e && !connected && !ready)e=ETIMEDOUT;
 printf("{\"tcp_complete\":1,\"connected\":%d,\"errno\":%d,\"elapsed_ms\":%.0f}\n",connected,e,(monotonic_seconds()-start)*1000);fflush(stdout);
 if(fd>=0)close(fd);return connected?0:1;
}
#include "tcp_trace.inc"
/* Read the native protocol counters from a real userspace sysctl caller.
 * The kernel agent intentionally does not proxy arbitrary net.* sysctls. */
static int protocol_stats(void){
 const char *names[]={"net.inet.tcp.stats","net.inet6.ip6.stats"};
 for(unsigned i=0;i<2;i++){
  unsigned char bytes[16384];size_t n=sizeof(bytes);int rc=sysctlbyname(names[i],bytes,&n,NULL,0),saved=rc?errno:0;
  printf("{\"sysctl\":\"%s\",\"errno\":%d,\"bytes\":%lu,\"hex\":\"",names[i],saved,(unsigned long)(rc?0:n));
  if(!rc)for(size_t j=0;j<n;j++)printf("%02x",bytes[j]);
  puts("\"}");
 }
 return 0;
}
int main(int argc,char **argv){
 if(argc==2&&!strcmp(argv[1],"--protocol-stats"))return protocol_stats();
 if(argc==2&&!strcmp(argv[1],"--capture-all-start"))return background_probe(argv[0],"--capture-all-worker","local-airdrop",180);
 if(argc==3&&!strcmp(argv[1],"--capture-all-worker"))return tcp_capture("local-airdrop");
 if(argc==3&&!strcmp(argv[1],"--capture-start"))return background_probe(argv[0],"--capture-worker",argv[2],180);
 if(argc==3&&!strcmp(argv[1],"--capture-worker"))return tcp_capture(argv[2]);
 if(argc==3&&!strcmp(argv[1],"--tcp-start"))return background_probe(argv[0],"--tcp-worker",argv[2],60);
 if(argc==3&&!strcmp(argv[1],"--udp-start"))return background_probe(argv[0],"--udp-ptr-long",argv[2],60);
 if(argc==3&&!strcmp(argv[1],"--tcp-worker"))return tcp_watch(argv[2]);
 if(argc==3&&!strcmp(argv[1],"--udp-ptr-long"))return dns_query(argv[2],"_airdrop._tcp.local.",12,60);

 if(argc==3&&!strcmp(argv[1],"--udp-ptr"))return dns_query(argv[2],"_airdrop._tcp.local.",12,8);
 if(argc==4&&!strcmp(argv[1],"--udp-aaaa"))return dns_query(argv[2],argv[3],28,8);
 if(argc==3&&!strcmp(argv[1],"--bt-trace")&&(!strcmp(argv[2],"0")||!strcmp(argv[2],"1"))){
  unsigned old=0,value=(unsigned)(argv[2][0]-'0');size_t n=sizeof(old);int e=0;
  if(sysctlbyname("kern.hackios.bt_trace",&old,&n,&value,sizeof(value)))e=errno;
  printf("{\"bt_trace_old\":%u,\"bt_trace_new\":%u,\"errno\":%d}\n",old,value,e);return e?1:0;
 }
 return 2;
}
