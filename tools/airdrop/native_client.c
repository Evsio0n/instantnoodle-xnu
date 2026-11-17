/* One native, scoped echo to the operator's preconfigured local Mac. The
 * only wireless SETs are the standard 20F66 CREATE/DELETE payloads. The
 * outer diagnostic guard prevents removal of another creator's lease. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "apple80211_provider.h"
extern char **environ;

extern int hk_native_echo6(const char *,const struct in6_addr *,const struct in6_addr *,unsigned);
#ifdef HK_AWDL_UDP
#include "udp_wire.h"
extern int hk_native_udp6(const char *,const struct in6_addr *,const struct in6_addr *,const uint8_t[16],unsigned);

static int read_udp_token(const char *path,uint8_t token[16]) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return errno;
    struct stat st;
    int error=0;
    if (fstat(fd,&st)) error=errno;
    else if (!S_ISREG(st.st_mode) || st.st_size!=16 || (st.st_mode&077)) error=EINVAL;
    size_t used=0;
    while (!error && used<16) {
        ssize_t n=read(fd,token+used,16-used);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) { error=n<0?errno:EIO; break; }
        used+=(size_t)n;
    }
    close(fd);
    return error;
}
#endif
struct req { char name[IFNAMSIZ]; int32_t type,value; uint32_t len; void *payload; };
struct create { uint32_t version; uint8_t zero[8]; uint32_t role; char name[IFNAMSIZ]; };
struct remove_if { uint32_t version; char name[IFNAMSIZ]; };
struct llstart { char name[IFNAMSIZ]; struct sockaddr_in6 addr,destination,mask; int32_t flags;
                 struct {uint64_t expires,preferred; uint32_t valid_seconds,preferred_seconds;} lifetime; };
/* 20F66 nd6.h, in6_nbrinfo_64. nd6_ioctl scopes this exact address lookup
 * to the named ifnet and calls nd6_lookup with create=0. */
struct neighbor_info {
    char name[IFNAMSIZ];
    struct in6_addr addr;
    int64_t asked;
    int32_t isrouter,state,expire;
};

#define AIR_GET _IOWR('i',201,struct req)
#define AIR_SET _IOW('i',200,struct req)
#define START_LL _IOWR('i',130,struct llstart)
#define GET_NEIGHBOR _IOWR('i',78,struct neighbor_info)
_Static_assert(AIR_GET==0xc02869c9UL && AIR_SET==0x802869c8UL &&
               offsetof(struct req,payload)==32,"20F66 Apple80211 ABI");
_Static_assert(sizeof(struct create)==32 && offsetof(struct create,name)==16 &&
               sizeof(struct remove_if)==20,"20F66 virtual interface ABI");
_Static_assert(sizeof(struct llstart)==128 && START_LL==0xc0806982UL,"20F66 SIOCLL_START_64 ABI");
_Static_assert(sizeof(struct neighbor_info)==56 && offsetof(struct neighbor_info,state)==44 &&
               GET_NEIGHBOR==0xc038694eUL,"20F66 SIOCGNBRINFO_IN6_64 ABI");

static volatile sig_atomic_t stopping;
static void stop(int unused) { (void)unused; stopping=1; }
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000+(uint64_t)ts.tv_nsec/1000000;
}

static int read_peer(const char *path,struct in6_addr *peer) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return errno;
    struct stat st;
    char text[INET6_ADDRSTRLEN+10];
    int error=0;
    if (fstat(fd,&st)) error=errno;
    else if (!S_ISREG(st.st_mode) || (st.st_mode&077) || st.st_size<2 || st.st_size>=(off_t)sizeof(text)) error=EINVAL;
    if (!error) {
        size_t used=0;
        while (used<(size_t)st.st_size) {
            ssize_t count=read(fd,text+used,(size_t)st.st_size-used);
            if (count<0 && errno==EINTR) continue;
            if (count<=0) { error=count<0?errno:EIO; break; }
            used+=(size_t)count;
        }
        if (!error && memchr(text,0,used)) error=EINVAL;
        if (!error) {
            while (used && (text[used-1]=='\n'||text[used-1]=='\r')) --used;
            text[used]=0;
            char *scope=strchr(text,'%');
            if (scope && strcmp(scope,"%awdl0")) error=EINVAL;
            if (scope) *scope=0;
            if (inet_pton(AF_INET6,text,peer)!=1 || !IN6_IS_ADDR_LINKLOCAL(peer)) error=EINVAL;
        }
    }
    close(fd); return error;
}

static int capability(int fd,bool *ready,bool *guarded) {
    uint8_t bits[13]={0};
    struct req r={.name="en0",.type=12,.value=HK_AWDL_DIAG_GUARD,.len=sizeof(bits),.payload=bits};
    if (ioctl(fd,AIR_GET,&r)) return errno;
    if (r.len!=sizeof(bits)) return EPROTO;
    *ready=!!(bits[4]&1);
    *guarded=r.value==HK_AWDL_DIAG_ACK;
    return 0;
}

static int acquire(int fd,char name[IFNAMSIZ],unsigned *index,bool *owned) {
    bool ready=false, guarded=false;
    uint64_t limit=now_ms()+45000;
    int rc=0;
    do {
        rc=capability(fd,&ready,&guarded);
        if (rc && rc!=ENXIO && rc!=ENETDOWN) break;
        if (ready || stopping) break;
        usleep(250000);
    } while (now_ms()<limit);
    printf("awdl-native: capability=%s guard=%u errno=%d\n",rc?"unknown":ready?"ready":"unavailable",guarded,rc);
    if (rc || !ready || stopping) return rc?rc:stopping?EINTR:ENETDOWN;
    strlcpy(name,"awdl0",IFNAMSIZ);
    *index=if_nametoindex(name);
    if (*index) { puts("awdl-native: create=existing delete_owned=0"); return 0; }
    if (!guarded) { puts("awdl-native: create=skipped ownership_guard_unavailable"); return ENOTSUP; }
    struct create c={.version=1,.role=6};
    struct req r={.name="en0",.type=94,.value=HK_AWDL_DIAG_GUARD,.len=sizeof(c),.payload=&c};
    if (ioctl(fd,AIR_SET,&r)) {
        rc=errno;
        if (rc==EEXIST && (*index=if_nametoindex(name))) {
            puts("awdl-native: create=concurrent_existing delete_owned=0"); return 0;
        }
        printf("awdl-native: create=refused errno=%d\n",rc); return rc;
    }
    if (!memchr(c.name,0,sizeof(c.name)) || strcmp(c.name,name)) return EPROTO;
    *owned=true; *index=if_nametoindex(name);
    printf("awdl-native: create=accepted index=%u role=6\n",*index);
    return *index?0:ENXIO;
}

static bool snapshot(const char *name,unsigned index,struct in6_addr *local,bool *link) {
    struct ifaddrs *all=NULL;
    *link=false;
    bool ll=false;
    if (!index || if_nametoindex(name)!=index || getifaddrs(&all)) return false;
    for (struct ifaddrs *it=all;it;it=it->ifa_next) {
        if (!it->ifa_addr || !it->ifa_name || strcmp(it->ifa_name,name) || !(it->ifa_flags&IFF_UP)) continue;
        if (it->ifa_addr->sa_family==AF_LINK) {
            const struct sockaddr_dl *s=(const struct sockaddr_dl *)it->ifa_addr;
            if (s->sdl_type!=IFT_ETHER || s->sdl_alen!=6 || s->sdl_index!=index ||
                s->sdl_len<offsetof(struct sockaddr_dl,sdl_data)+s->sdl_nlen+6) continue;
            const uint8_t *mac=(const uint8_t *)LLADDR(s);
            if (!(mac[0]&1) && memcmp(mac,"\0\0\0\0\0\0",6)) *link=true;
        } else if (it->ifa_addr->sa_family==AF_INET6) {
            const struct sockaddr_in6 *s=(const struct sockaddr_in6 *)it->ifa_addr;
            if (!IN6_IS_ADDR_LINKLOCAL(&s->sin6_addr) || (s->sin6_scope_id && s->sin6_scope_id!=index)) continue;
            struct in6_addr candidate=s->sin6_addr;
            /* ifaddrs can supply the in-kernel KAME scope. The socket call
             * uses a separate scope_id and must receive an unembedded LLA. */
            if (candidate.s6_addr[2] || candidate.s6_addr[3]) {
                unsigned embedded=(unsigned)candidate.s6_addr[2]*256+candidate.s6_addr[3];
                if (embedded!=index) continue;
                candidate.s6_addr[2]=candidate.s6_addr[3]=0;
            }
            *local=candidate;
            ll=true;
        }
    }
    freeifaddrs(all);
    return ll;
}

static int wait_for_neighbor(const char *name,unsigned index,const struct in6_addr *peer) {
    int fd=socket(AF_INET6,SOCK_DGRAM,0);
    if (fd<0) return errno;
    uint64_t start=now_ms(), deadline=start+8000;
    int result=ETIMEDOUT, last_error=0, state=-99;
    unsigned checks=0;
    bool scope=false, usable=false;
    do {
        if (stopping) { result=EINTR; break; }
        scope=index && if_nametoindex(name)==index;
        if (!scope) { result=ENXIO; break; }
        struct neighbor_info entry={.addr=*peer};
        strlcpy(entry.name,name,sizeof(entry.name));
        ++checks;
        last_error=ioctl(fd,GET_NEIGHBOR,&entry)<0?errno:0;
        scope=if_nametoindex(name)==index;
        if (!scope) { result=ENXIO; break; }
        if (!last_error) {
            state=entry.state;
            /* REACHABLE, STALE, DELAY and PROBE all carry an existing link
             * mapping. Do not require an over-the-air confirmation. */
            usable=state>=1 && state<=4;
            if (usable) { result=0; break; }
        } else {
            state=-99;
            /* 20F66 returns EINVAL when no scoped neighbor route exists. */
            if (last_error!=EINVAL && last_error!=ENOENT && last_error!=ESRCH) {
                result=last_error; break;
            }
        }
        usleep(100000);
    } while (now_ms()<deadline);
    printf("awdl-native: neighbor_lookup checks=%u ifindex=%u scope_matches=%u route_scoped=%u "
           "state=%d usable=%u lookup_errno=%d result_errno=%d waited_ms=%llu uptime_ms=%llu\n",
           checks,index,scope,scope && checks && !last_error,state,usable,last_error,result,
           (unsigned long long)(now_ms()-start),(unsigned long long)now_ms());
    close(fd); return result;
}

static int configure_ll(const char *name,unsigned index,struct in6_addr *local) {
    bool link=false;
    bool ll=snapshot(name,index,local,&link);
    printf("awdl-native: initial index=%u actual_mac=%u scoped_linklocal=%u\n",index,link,ll);
    if (!link) return ENXIO;
    if (!ll) {
        int fd=socket(AF_INET6,SOCK_DGRAM,0);
        if (fd<0) return errno;
        struct llstart req={0};
        strlcpy(req.name,name,sizeof(req.name));
        int error=ioctl(fd,START_LL,&req)<0?errno:0;
        close(fd);
        printf("awdl-native: native_llstart_errno=%d\n",error);
        if (error && error!=EEXIST) return error;
    }
    uint64_t end=now_ms()+6000;
    do {
        if (snapshot(name,index,local,&link) && link) {
            puts("awdl-native: actual_mac=1 scoped_linklocal=1"); return 0;
        }
        if (stopping) return EINTR;
        usleep(100000);
    } while(now_ms()<end);
    return EADDRNOTAVAIL;
}

static int cleanup(int fd,const char *name,unsigned index) {
    struct remove_if wire={.version=1};strlcpy(wire.name,name,sizeof(wire.name));
    struct req req={.name="en0",.type=95,.value=HK_AWDL_DIAG_GUARD,.len=sizeof(wire),.payload=&wire};
    int rc=0;
    uint64_t end=now_ms()+7000;
    do {
        unsigned current=if_nametoindex(name);
        if(!current) {puts("awdl-native: delete=already_absent");return 0;}
        if (current && index && current!=index) {
            puts("awdl-native: delete=replaced_interface_preserved");return EBUSY;
        }
        rc=ioctl(fd,AIR_SET,&req)<0?errno:0;
        if (!rc || (rc==ENOENT && !current)) break;
        if (rc!=EINPROGRESS && rc!=EAGAIN && rc!=EBUSY) break;
        usleep(200000);
    } while(now_ms()<end);
    unsigned current=if_nametoindex(name);
    printf("awdl-native: delete errno=%d old_index_present=%u\n",rc,!!(current && current==index));
    if (rc==ENOENT && !current) return 0;
    return rc?rc:current==index && current?EBUSY:0;
}

int main(int argc,char **argv) {
    /* The port has no airportd-created AWDL interface. This path uses the
     * real CREATE and stock IPv6 ioctls only, with no peer or test traffic.
     * Keep its lease alive while the original sharingd owns discovery/TLS. */
    bool interface_service=argc==2 && !strcmp(argv[1],"--interface-service");
    if (argc==2 && (interface_service || !strcmp(argv[1],"--interface-only"))) {
        setvbuf(stdout,NULL,_IOLBF,0);
        signal(SIGTERM,stop);signal(SIGINT,stop);
        int fd=socket(AF_INET,SOCK_DGRAM,0);
        if(fd<0) return 2;
        char name[IFNAMSIZ]={0};unsigned index=0;bool owned=false;
        struct in6_addr local;
        int error=acquire(fd,name,&index,&owned);
        if(!error) error=configure_ll(name,index,&local);
        printf("awdl-native: interface_only errno=%d index=%u peer_file=0\n",error,index);
        printf("awdl-native: lifecycle=%s teardown_on_signal_or_interface_change=1\n",
            interface_service?"radio-service":"bounded-probe");
        pid_t launcher=0;
        const char *launcher_path="/hackios/bin/airdrop_frontboard_launch";
        if(!error && !interface_service && access(launcher_path,X_OK)==0) {
            char *launch_args[]={(char *)launcher_path,"com.hackios.airdrop-test",NULL};
            int launch=posix_spawn(&launcher,launcher_path,NULL,NULL,launch_args,environ);
            printf("awdl-native: foreground_request_spawn_errno=%d\n",launch);
            if(launch)launcher=0;
        }
        uint64_t deadline=now_ms()+540000;
        /* The system bootstrap must outlive a sender's UI and a receiver's
         * idle timeout. The old diagnostic command retains its nine-minute
         * bound. Both modes still exit on signal or interface replacement;
         * the radio provider owns final generation teardown. */
        while(!error && !stopping && (interface_service || now_ms()<deadline) && if_nametoindex(name)==index) {
            if(launcher && waitpid(launcher,NULL,WNOHANG)==launcher)launcher=0;
            usleep(250000);
        }
        int teardown=owned?cleanup(fd,name,index):0;
        printf("awdl-native: interface_only_end errno=%d teardown_errno=%d\n",error,teardown);
        close(fd);return error||teardown?1:0;
    }
#ifdef HK_AWDL_UDP
    bool udp=argc==4 && !strcmp(argv[2],"--udp");
    if (argc!=2 && !udp) {
        fputs("usage: airdrop_native_client PRIVATE_PEER_FILE [--udp PRIVATE_TOKEN]\n",stderr);return 2;
    }
    uint8_t token[16]={0};
    if (udp) {
        int error=read_udp_token(argv[3],token);
        if (error) { printf("awdl-native: token_file_errno=%d\n",error);return 2; }
    }
#else
    if (argc!=2) { fputs("usage: airdrop_native_client PRIVATE_PEER_FILE\n",stderr);return 2; }
#endif
    setvbuf(stdout,NULL,_IOLBF,0);
    struct in6_addr peer,local;
    int error=read_peer(argv[1],&peer);
    if (error) { printf("awdl-native: peer_file_errno=%d\n",error);return 2; }
    signal(SIGTERM,stop);signal(SIGINT,stop);
    int fd=socket(AF_INET,SOCK_DGRAM,0);
    if (fd<0) { printf("awdl-native: control_socket_errno=%d\n",errno);return 2; }
    char name[IFNAMSIZ]={0};unsigned index=0;bool owned=false;
    error=acquire(fd,name,&index,&owned);
    if (!error) error=configure_ll(name,index,&local);
    if (!error && !memcmp(&local,&peer,sizeof(local))) error=EINVAL;
    if (!error) error=wait_for_neighbor(name,index,&peer);
    if (!error && !stopping) {
#ifdef HK_AWDL_UDP
        if (udp) error=hk_native_udp6(name,&local,&peer,token,5000);
        else
#endif
            error=hk_native_echo6(name,&local,&peer,5000);
    }
#ifdef HK_AWDL_UDP
    memset_s(token,sizeof(token),0,sizeof(token));
#endif
    if (stopping && !error) error=EINTR;
    int teardown=owned?cleanup(fd,name,index):0;
    printf("awdl-native: result_errno=%d teardown_errno=%d owned=%u\n",error,teardown,owned);
    close(fd);return error||teardown?1:0;
}
