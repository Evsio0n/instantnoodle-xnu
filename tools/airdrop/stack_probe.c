/* Read-only upper-stack observations. Optional BPF observes ONLY awdl0
 * inbound traffic; only two mDNS packet prefixes are printed. No socket
 * configuration, sysctl writes or retained packet files. Native DNS-SD
 * browsing is an optional ordinary query, never a service registration. */
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <net/if.h>
#include <net/bpf.h>
#include <dns_sd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stddef.h>
#include "wire_checksum.h"
static double monotime(void)
{
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec+t.tv_nsec/1e9;
}
static void statistics(void)
{
    const char *names[]={"net.inet6.ip6.stats","net.inet.udp.stats"};
    for(unsigned i=0;i<2;i++) {
        size_t n=0;int e=0;
        if(sysctlbyname(names[i],NULL,&n,NULL,0))e=errno;
        if(!e && (!n || n>65536))e=EOVERFLOW;
        unsigned char *b=e?NULL:malloc(n);if(!e && !b)e=ENOMEM;
        if(!e && sysctlbyname(names[i],b,&n,NULL,0))e=errno;
        printf("{\"name\":\"%s\",\"uid\":%u,\"errno\":%d,\"bytes\":%lu,\"prefix_hex\":\"",names[i],(unsigned)getuid(),e,(unsigned long)n);
        if(!e)for(size_t j=0;j<n && j<2304;j++)printf("%02x",b[j]);
        puts("\"}");free(b);
    }
}
static int capture(unsigned port, const char *peer)
{
    int fd=-1,e=0;char dev[32];unsigned blen=0,one=1,see_sent=port!=0;unsigned tcp_events=0;
    const char *stage="open";
    unsigned frames=0,ipv6=0,udp=0,mdns=0,shown=0;
    for(unsigned i=0;i<32;i++) {
        snprintf(dev,sizeof(dev),"/dev/bpf%u",i);fd=open(dev,O_RDONLY|O_NONBLOCK);
        if(fd>=0 || errno!=EBUSY)break;
    }
    if(fd<0)e=errno;
    struct ifreq ifr={0};strcpy(ifr.ifr_name,"awdl0");
    if(!e){stage="bind";if(ioctl(fd,BIOCSETIF,&ifr))e=errno;}
    if(!e){stage="immediate";if(ioctl(fd,BIOCIMMEDIATE,&one))e=errno;}
    if(!e){stage="inbound_only";if(ioctl(fd,BIOCSSEESENT,&see_sent))e=errno;}
    if(!e){stage="buffer_size";if(ioctl(fd,BIOCGBLEN,&blen))e=errno;}
    if(!e && (!blen || blen>1024*1024))e=EOVERFLOW;
    unsigned char *buf=e?NULL:malloc(blen);if(!e && !buf)e=ENOMEM;
    if(!e && fd>=FD_SETSIZE)e=EOVERFLOW;
    int client=-1;
    if (!e && peer) {
        struct sockaddr_in6 dst={0},local={0};socklen_t count=sizeof(local);
        dst.sin6_len=sizeof(dst);dst.sin6_family=AF_INET6;dst.sin6_port=htons(port);
        dst.sin6_scope_id=if_nametoindex("awdl0");stage="connect_setup";
        if(inet_pton(AF_INET6,peer,&dst.sin6_addr)!=1 || !IN6_IS_ADDR_LINKLOCAL(&dst.sin6_addr))e=EINVAL;
        if(!e && (client=socket(AF_INET6,SOCK_STREAM,0))<0)e=errno;
        if(!e && setsockopt(client,SOL_SOCKET,0x1113,&one,sizeof(one)))e=errno;
        if(!e && setsockopt(client,IPPROTO_IPV6,IPV6_BOUND_IF,&dst.sin6_scope_id,sizeof(unsigned)))e=errno;
        if(!e && fcntl(client,F_SETFL,O_NONBLOCK)<0)e=errno;
        if(!e) {
            int result=connect(client,(struct sockaddr *)&dst,sizeof(dst));
            int ce=result<0?errno:0;
            if(getsockname(client,(struct sockaddr *)&local,&count)==0)
                printf("{\"connect_errno\":%d,\"local_port\":%u}\n",ce,ntohs(local.sin6_port));
            if(ce && ce!=EINPROGRESS)e=ce;
        }
    }
    if(!e)stage="capture";
    double end=monotime()+8;
    while(!e && monotime()<end) {
        fd_set fds;FD_ZERO(&fds);FD_SET(fd,&fds);struct timeval tv={1,0};
        int ready=select(fd+1,&fds,NULL,NULL,&tv);
        if(ready<0){if(errno==EINTR)continue;e=errno;break;}
        if(!ready)continue;
        ssize_t got=read(fd,buf,blen);
        if(got<0){if(errno==EAGAIN || errno==EINTR)continue;e=errno;break;}
        /* Darwin's wire header may be18 bytes despite sizeof(bpf_hdr)==20. */
        size_t minhdr=offsetof(struct bpf_hdr,bh_hdrlen)+sizeof(((struct bpf_hdr *)0)->bh_hdrlen);
        for(size_t off=0;off+minhdr<=(size_t)got;) {
            struct bpf_hdr h={0};memcpy(&h,buf+off,minhdr);
            if(h.bh_hdrlen<minhdr || h.bh_hdrlen>(size_t)got-off ||
               h.bh_caplen>(size_t)got-off-h.bh_hdrlen){e=EPROTO;break;}
            const unsigned char *p=buf+off+h.bh_hdrlen;size_t n=h.bh_caplen;
            frames++;
            if(n>=54 && p[12]==0x86 && p[13]==0xdd && (p[14]>>4)==6) {
                ipv6++;
                if(port && p[20]==6 && n>=74) {
                    unsigned src=(unsigned)p[54]*256+p[55],dst=(unsigned)p[56]*256+p[57];
                    if((src==port || dst==port) && tcp_events++<48)
                        printf("{\"src_port\":%u,\"dst_port\":%u,\"tcp_flags\":%u,\"bytes\":%lu}\n",src,dst,p[67],(unsigned long)n);
                }
                if(p[20]==17 && n>=62) {
                    udp++;unsigned a=(unsigned)p[54]*256+p[55],b=(unsigned)p[56]*256+p[57];
                    if(a==5353 || b==5353) {
                        mdns++;size_t iplen=40u+((unsigned)p[18]<<8)+p[19];
                        if(!port && shown++<2) {
                            printf("{\"mdns_inbound\":1,\"bytes\":%lu,\"checksum\":%d,\"prefix_hex\":\"",(unsigned long)n,n>=14+iplen?awdl_wire_checksum(p+14,iplen):-1);
                            for(size_t j=0;j<n && j<512;j++)printf("%02x",p[j]);
                            puts("\"}");
                        }
                    }
                }
            }
            size_t step=BPF_WORDALIGN((size_t)h.bh_hdrlen+h.bh_caplen);
            if(!step)break;off+=step;
        }
    }
    if(client>=0) {
        struct sockaddr_in6 connected;socklen_t len=sizeof(connected);
        int connected_ok=getpeername(client,(struct sockaddr *)&connected,&len)==0;
        printf("{\"connected\":%d}\n",connected_ok);close(client);
    }
    if(fd>=0)close(fd);free(buf);
    printf("{\"bpf_interface\":\"awdl0\",\"stage\":\"%s\",\"inbound_only\":%u,\"errno\":%d,\"frames\":%u,\"ipv6\":%u,\"udp\":%u,\"mdns\":%u}\n",stage,!see_sent,e,frames,ipv6,udp,mdns);
    return e?1:0;
}
static void browse_event(DNSServiceRef ref,DNSServiceFlags flags,uint32_t index,
    DNSServiceErrorType error,const char *name,const char *type,const char *domain,void *context)
{
    (void)ref;(void)type;(void)domain;unsigned *events=context;
    if((*events)++>=8)return;
    printf("{\"bonjour_event\":1,\"flags\":%u,\"interface\":%u,\"error\":%d,\"name_hex\":\"",flags,index,error);
    if(name)for(unsigned i=0;name[i] && i<63;i++)printf("%02x",(unsigned char)name[i]);
    puts("\"}");
}
static void address_event(DNSServiceRef ref,DNSServiceFlags flags,uint32_t index,
    DNSServiceErrorType error,const char *name,uint16_t type,uint16_t cls,
    uint16_t size,const void *bytes,uint32_t ttl,void *context)
{
    (void)ref;(void)name;(void)ttl;
    unsigned *events=context;(*events)++;
    printf("{\"aaaa_callback\":%u,\"error\":%d,\"added\":%u,\"address_hex\":\"",index,error,!!(flags&kDNSServiceFlagsAdd));
    if(!error && type==28 && cls==1 && size==16)
        for(unsigned i=0;i<16;i++)printf("%02x",((const unsigned char *)bytes)[i]);
    puts("\"}");
}
static int browse(const char *host)
{
    unsigned index=if_nametoindex("awdl0"),events=0;
    if(!index){puts("{\"bonjour_start\":0,\"errno\":6}");return 1;}
    DNSServiceRef ref=NULL;
    DNSServiceErrorType error=host?DNSServiceQueryRecord(&ref,kDNSServiceFlagsIncludeAWDL,index,host,28,1,address_event,&events):DNSServiceBrowse(&ref,kDNSServiceFlagsIncludeAWDL,index,
        "_airdrop._tcp","local.",browse_event,&events);
    printf("{\"bonjour_start\":1,\"interface\":%u,\"error\":%d}\n",index,error);
    if(error)return 1;
    int fd=DNSServiceRefSockFD(ref);double end=monotime()+8;
    if(fd<0 || fd>=FD_SETSIZE){DNSServiceRefDeallocate(ref);return 1;}
    while(monotime()<end) {
        fd_set fds;FD_ZERO(&fds);FD_SET(fd,&fds);struct timeval tv={1,0};
        int ready=select(fd+1,&fds,NULL,NULL,&tv);
        if(ready<0){if(errno==EINTR)continue;error=-1;break;}
        if(ready && (error=DNSServiceProcessResult(ref)))break;
    }
    DNSServiceRefDeallocate(ref);
    printf("{\"bonjour_events\":%u,\"error\":%d}\n",events,error);return error?1:0;
}
int main(int argc,char **argv)
{
    if(argc==3 && !strcmp(argv[1],"--aaaa"))return browse(argv[2]);
    if((argc==3 && !strcmp(argv[1],"--tcp")) || (argc==4 && !strcmp(argv[1],"--connect"))) {
        char *end=NULL;long port=strtol(argv[2],&end,10);
        if(!*argv[2] || *end || port<1 || port>65535)return 2;
        return capture((unsigned)port,argc==4?argv[3]:NULL);
    }
    if(argc>2 || (argc==2 && strcmp(argv[1],"--bpf") && strcmp(argv[1],"--browse")))return 2;
    statistics();int result=argc==2?(!strcmp(argv[1],"--bpf")?capture(0,NULL):browse(NULL)):0;
    if(argc==2)statistics();return result;
}
