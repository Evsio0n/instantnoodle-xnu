/* One-shot test responder for one private per-session token; ordinary user,
 * specific current awdl0 address only. No discovery or network configuration. */
#define __APPLE_USE_RFC_3542 1
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "udp_wire.h"

static volatile sig_atomic_t stopping;
static void shutdown_requested(int unused) { (void)unused; stopping=1; }
static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}

static int read_private(const char *path,void *buffer,size_t capacity,size_t *length) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return errno;
    struct stat s;
    int error=0;
    if (fstat(fd,&s)) error=errno;
    else if (!S_ISREG(s.st_mode) || (s.st_mode&077) || s.st_size<=0 || s.st_size>(off_t)capacity) error=EINVAL;
    size_t used=0;
    while (!error && used<(size_t)s.st_size) {
        ssize_t n=read(fd,(char *)buffer+used,(size_t)s.st_size-used);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) { error=n<0?errno:EIO;break; }
        used+=(size_t)n;
    }
    close(fd);
    if (!error) *length=used;
    return error;
}

static int normalize(struct in6_addr *a,unsigned index) {
    if (!IN6_IS_ADDR_LINKLOCAL(a)) return 0;
    unsigned embedded=((unsigned)a->s6_addr[2]<<8)|a->s6_addr[3];
    if (embedded && embedded!=index) return 0;
    a->s6_addr[2]=a->s6_addr[3]=0;
    return 1;
}

static int is_assigned(unsigned index,const struct in6_addr *expected) {
    struct ifaddrs *all=NULL;
    if (getifaddrs(&all)) return errno;
    int result=EADDRNOTAVAIL;
    for (const struct ifaddrs *i=all;i;i=i->ifa_next) {
        if (!i->ifa_name || strcmp(i->ifa_name,"awdl0") || !i->ifa_addr ||
            i->ifa_addr->sa_family!=AF_INET6 || !(i->ifa_flags&IFF_UP)) continue;
        const struct sockaddr_in6 *s=(const struct sockaddr_in6 *)i->ifa_addr;
        struct in6_addr a=s->sin6_addr;
        if ((!s->sin6_scope_id || s->sin6_scope_id==index) && normalize(&a,index) &&
            !memcmp(&a,expected,sizeof(a))) {result=0;break;}
    }
    freeifaddrs(all); return result;
}

static int on_bound_interface(const struct msghdr *m,unsigned index,const struct in6_addr *expected) {
    for (struct cmsghdr *c=CMSG_FIRSTHDR(m);c;c=CMSG_NXTHDR((struct msghdr *)m,c)) {
        if (c->cmsg_level!=IPPROTO_IPV6 || c->cmsg_type!=IPV6_PKTINFO ||
            c->cmsg_len<CMSG_LEN(sizeof(struct in6_pktinfo))) continue;
        struct in6_pktinfo info;
        memcpy(&info,CMSG_DATA(c),sizeof(info));
        return info.ipi6_ifindex==index && normalize(&info.ipi6_addr,index) &&
            !memcmp(&info.ipi6_addr,expected,sizeof(*expected));
    }
    return 0;
}

static int serve(int fd,unsigned index,const struct in6_addr *me,const uint8_t token[16],unsigned seconds) {
    struct sockaddr_in6 bind_address={.sin6_len=sizeof(bind_address),.sin6_family=AF_INET6,
        .sin6_addr=*me,.sin6_scope_id=index,.sin6_port=htons(HK_UDP_PORT)};
    int one=1;
    if (setsockopt(fd,IPPROTO_IPV6,IPV6_BOUND_IF,&index,sizeof(index)) ||
        setsockopt(fd,IPPROTO_IPV6,IPV6_V6ONLY,&one,sizeof(one)) ||
        setsockopt(fd,IPPROTO_IPV6,IPV6_RECVPKTINFO,&one,sizeof(one)) ||
        bind(fd,(struct sockaddr *)&bind_address,sizeof(bind_address))) return errno;
    unsigned bound=0;
    socklen_t size=sizeof(bound);
    if (getsockopt(fd,IPPROTO_IPV6,IPV6_BOUND_IF,&bound,&size)) return errno;
    struct sockaddr_in6 actual={0};
    size=sizeof(actual);
    if (getsockname(fd,(struct sockaddr *)&actual,&size)) return errno;
    if (bound!=index || actual.sin6_family!=AF_INET6 || actual.sin6_port!=bind_address.sin6_port ||
        (actual.sin6_scope_id && actual.sin6_scope_id!=index) || !normalize(&actual.sin6_addr,index) ||
        memcmp(&actual.sin6_addr,me,sizeof(*me))) return EADDRNOTAVAIL;
    printf("awdl-udp-mac: listening=1 uid=%u ifindex=%u bound_if_match=1 local_match=1 port=%u wait_seconds=%u\n",
           (unsigned)geteuid(),index,HK_UDP_PORT,seconds);

    unsigned rejected=0;
    uint64_t started=now_ms(),end=started+seconds*1000,recheck=started+1000;
    while (!stopping) {
        uint64_t now=now_ms();
        if (now>=end) break;
        if (now>=recheck) {
            int status=if_nametoindex("awdl0")==index?is_assigned(index,me):ENXIO;
            if (status) {
                printf("awdl-udp-mac: interface_or_address_lost=1 check_errno=%d\n",status);
                return status;
            }
            recheck=now+1000;
        }
        struct pollfd p={.fd=fd,.events=POLLIN};
        int rc=poll(&p,1,(int)((end<recheck?end:recheck)-now));
        if (rc<0 && errno==EINTR) continue;
        if (rc<0) return errno;
        if (!rc) continue;
        if (!(p.revents&POLLIN)) return EIO;
        struct hk_udp_message request;
        struct sockaddr_in6 peer={0};
        struct iovec iov={.iov_base=&request,.iov_len=sizeof(request)};
        union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(struct in6_pktinfo))]; } control;
        struct msghdr msg={.msg_name=&peer,.msg_namelen=sizeof(peer),.msg_iov=&iov,.msg_iovlen=1,
                           .msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
        ssize_t n=recvmsg(fd,&msg,0);
        if (n<0 && (errno==EINTR || errno==EAGAIN)) continue;
        if (n<0) return errno;
        if (n!=(ssize_t)sizeof(request) || (msg.msg_flags&(MSG_TRUNC|MSG_CTRUNC)) ||
            msg.msg_namelen<sizeof(peer) || peer.sin6_family!=AF_INET6 || !peer.sin6_port ||
            (peer.sin6_scope_id && peer.sin6_scope_id!=index) || !normalize(&peer.sin6_addr,index) ||
            !memcmp(&peer.sin6_addr,me,sizeof(*me)) || !on_bound_interface(&msg,index,me) ||
            !hk_udp_matches(&request,HK_UDP_REQUEST,token)) { ++rejected;continue; }
        peer.sin6_scope_id=index;
        char digest[65];hk_udp_digest(&request,digest);
        printf("awdl-udp-mac: request_valid=1 received=%ld scope_match=1 request_sha256=%s elapsed_ms=%llu\n",
               (long)n,digest,(unsigned long long)(now_ms()-started));
        struct hk_udp_message reply;hk_udp_make(&reply,HK_UDP_REPLY,token);
        hk_udp_digest(&reply,digest);
        ssize_t written=sendto(fd,&reply,sizeof(reply),0,(struct sockaddr *)&peer,sizeof(peer));
        int error=written<0?errno:written!=(ssize_t)sizeof(reply)?EIO:0;
        printf("awdl-udp-mac: reply_attempts=1 reply_bytes=%ld reply_errno=%d reply_sha256=%s rejected=%u\n",
               (long)written,error,digest,rejected);
        return error;
    }
    printf("awdl-udp-mac: request_valid=0 rejected=%u wait_errno=%d\n",rejected,stopping?EINTR:ETIMEDOUT);
    return stopping?EINTR:ETIMEDOUT;
}

int main(int argc,char **argv) {
    if (argc!=4) { fputs("usage: mac_udp_receiver PRIVATE_MAC_PEER PRIVATE_TOKEN WAIT_SECONDS(1..300)\n",stderr); return 2; }
    char *tail=NULL;errno=0;
    long wait=strtol(argv[3],&tail,10);
    if (errno || !tail || *tail || wait<1 || wait>300) return 2;
    setvbuf(stdout,NULL,_IOLBF,0);
    uint8_t token[16];char ip[INET6_ADDRSTRLEN+2];size_t used=0;
    int error=read_private(argv[2],token,sizeof(token),&used);
    if (!error && used!=sizeof(token)) error=EINVAL;
    if (!error) error=read_private(argv[1],ip,sizeof(ip)-1,&used);
    struct in6_addr me;
    if (!error) {
        if (memchr(ip,0,used)) error=EINVAL;
        while (used && (ip[used-1]=='\r'||ip[used-1]=='\n')) --used;
        ip[used]=0;
        if (inet_pton(AF_INET6,ip,&me)!=1 || !IN6_IS_ADDR_LINKLOCAL(&me)) error=EINVAL;
    }
    unsigned index=if_nametoindex("awdl0");
    if (!error && !index) error=ENXIO;
    if (!error) error=is_assigned(index,&me);
    int fd=-1;
    if (!error) {
        fd=socket(AF_INET6,SOCK_DGRAM,0); if (fd<0) error=errno;
    }
    if (!error) {
        struct sigaction sa={0};sa.sa_handler=shutdown_requested;sigemptyset(&sa.sa_mask);
        if (sigaction(SIGTERM,&sa,NULL) || sigaction(SIGINT,&sa,NULL)) error=errno;
        else error=serve(fd,index,&me,token,(unsigned)wait);
    }
    if (fd>=0) close(fd);
    memset_s(token,sizeof(token),0,sizeof(token));
    printf("awdl-udp-mac: closed=1 errno=%d\n",error);
    return error?1:0;
}
