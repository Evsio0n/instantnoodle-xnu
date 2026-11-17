#include <sys/ioctl.h>
#include "BpfAbi.h"
#include <stddef.h>
#include "TcpTracePacket.h"
#include "../awdl/wire_checksum.h"
static int tcp_capture(const char *peer){
 uint8_t address[16];bool local=!strcmp(peer,"local-airdrop");if(!local && (inet_pton(AF_INET6,peer,address)!=1 || address[0]!=0xfe || (address[1]&0xc0)!=0x80))return 2;
 int fd=-1,e=0;char dev[32];unsigned one=1,blen=262144,events=0;const char *stage="open";
 for(unsigned i=0;i<32;i++){snprintf(dev,sizeof(dev),"/dev/bpf%u",i);fd=open(dev,O_RDONLY|O_NONBLOCK);if(fd>=0 || errno!=EBUSY)break;}
 if(fd<0)e=errno;
 struct ifreq ifr={0};strcpy(ifr.ifr_name,"awdl0");
 if(!e){stage="buffer";if(ioctl(fd,HK_BIOCSBLEN,&blen))e=errno;}
 if(!e){stage="bind";if(ioctl(fd,HK_BIOCSETIF,&ifr))e=errno;}
 if(!e){stage="immediate";if(ioctl(fd,HK_BIOCIMMEDIATE,&one))e=errno;}
 if(!e){stage="both_directions";if(ioctl(fd,HK_BIOCSSEESENT,&one))e=errno;}
 struct hk_bpf_insn insn={6,0,0,2048};struct hk_bpf_program filter={1,&insn};
 if(!e){stage="snaplen";if(ioctl(fd,HK_BIOCSETF,&filter))e=errno;}
 if(!e && ioctl(fd,HK_BIOCGBLEN,&blen))e=errno;
 if(!e && (!blen || blen>1024*1024 || fd>=FD_SETSIZE))e=EOVERFLOW;
 uint8_t *buf=e?NULL:malloc(blen);if(!e && !buf)e=ENOMEM;
 printf("{\"capture_start\":1,\"errno\":%d,\"stage\":\"%s\",\"seconds\":180,\"see_sent\":1}\n",e,stage);fflush(stdout);
 unsigned bulk_samples=0,ack_samples=0;
 double end=monotonic_seconds()+180;
 while(!e && monotonic_seconds()<end && events<2048){
  fd_set f;FD_ZERO(&f);FD_SET(fd,&f);struct timeval tv={1,0};int ready=select(fd+1,&f,NULL,NULL,&tv);
  if(ready<0){if(errno==EINTR)continue;e=errno;break;}if(!ready)continue;
  ssize_t n=read(fd,buf,blen);if(n<0){if(errno==EAGAIN || errno==EINTR)continue;e=errno;break;}
  size_t min=offsetof(struct hk_bpf_hdr,bh_hdrlen)+sizeof(((struct hk_bpf_hdr *)0)->bh_hdrlen);
  for(size_t at=0;at+min<=(size_t)n;){
   struct hk_bpf_hdr h={0};memcpy(&h,buf+at,min);
   if(h.bh_hdrlen<min || h.bh_hdrlen>(size_t)n-at || h.bh_caplen>(size_t)n-at-h.bh_hdrlen){e=EPROTO;break;}
   const uint8_t *p=buf+at+h.bh_hdrlen;struct hk_tcp_trace t;
   /* Retain complete native mDNS responses for the host PTR/TTL decoder.
    * TCP's independent 2048-event budget still covers the upload. */
   if(local && h.bh_caplen>=74 && p[12]==0x86 && p[13]==0xdd && p[20]==17 &&
      p[54]==0x14 && p[55]==0xe9 && (p[64]&0x80)){
    unsigned udp=(unsigned)p[58]*256+p[59];
    if(udp>=20 && udp<=h.bh_caplen-54){
     char source[INET6_ADDRSTRLEN];inet_ntop(AF_INET6,p+22,source,sizeof(source));
     printf("{\"mdns_source\":\"%s\",\"time\":%.6f,\"dns_hex\":\"",source,monotonic_seconds());
     for(unsigned k=62;k<54+udp;k++)printf("%02x",p[k]);
     puts("\"}");fflush(stdout);
    }
   }
   if(hk_trace_packet(p,h.bh_caplen,local?NULL:address,&t) &&
      ((t.flags&7) || (t.payload>0 && t.payload<1200) ||
       (t.payload>=1200 && (++bulk_samples%32)==1) ||
       (!t.payload && (++ack_samples%16)==1))){
    char source[INET6_ADDRSTRLEN],destination[INET6_ADDRSTRLEN];inet_ntop(AF_INET6,p+22,source,sizeof(source));inet_ntop(AF_INET6,p+38,destination,sizeof(destination));
    printf("{\"source_ip\":\"%s\",\"destination_ip\":\"%s\",",source,destination);
    printf("\"time\":%.6f,\"src\":%u,\"dst\":%u,\"flags\":%u,\"seq\":%u,\"ack\":%u,\"payload\":%u,\"window\":%u,\"checksum\":%d,\"wire\":%u,\"prefix\":\"",monotonic_seconds(),t.src,t.dst,t.flags,t.seq,t.ack,t.payload,t.window,h.bh_caplen>=54+t.header+t.payload?awdl_wire_checksum(p+14,40+t.header+t.payload):-2,h.bh_datalen);
    for(size_t i=t.data_offset;i<h.bh_caplen && i<t.data_offset+8;i++)printf("%02x",p[i]);puts("\"}");fflush(stdout);events++;
   }
   size_t step=HK_BPF_WORDALIGN((size_t)h.bh_hdrlen+h.bh_caplen);if(!step){e=EPROTO;break;}at+=step;
  }
 }
 struct hk_bpf_stat stats={0};int se=fd>=0 && ioctl(fd,HK_BIOCGSTATS,&stats)?errno:0;
 printf("{\"capture_end\":1,\"errno\":%d,\"events\":%u,\"bpf_received\":%u,\"bpf_dropped\":%u,\"stats_errno\":%d}\n",e,events,stats.bs_recv,stats.bs_drop,se);
 if(fd>=0)close(fd);free(buf);return e?1:0;
}
