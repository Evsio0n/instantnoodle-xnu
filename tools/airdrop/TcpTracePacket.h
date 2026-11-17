#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
struct hk_tcp_trace {unsigned src,dst,flags,header,payload;uint32_t seq,ack;size_t data_offset;unsigned window;};
static uint32_t hk_trace_be32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static bool hk_trace_packet(const uint8_t *p,size_t cap,const uint8_t peer[16],struct hk_tcp_trace *out){
 if(!p || cap<74 || p[12]!=0x86 || p[13]!=0xdd || p[14]>>4!=6 || p[20]!=6)return false;
 if(peer){
  bool source=p[22]==peer[0] && p[23]==peer[1],dest=p[38]==peer[0] && p[39]==peer[1];
  for(unsigned i=4;i<16;i++){source=source&&p[22+i]==peer[i];dest=dest&&p[38+i]==peer[i];}
  if(!source && !dest)return false;
 }
 unsigned a=(unsigned)p[54]*256+p[55],b=(unsigned)p[56]*256+p[57],h=(p[66]>>4)*4,iplen=(unsigned)p[18]*256+p[19];
 if((a!=8770 && b!=8770) || h<20 || h>60 || iplen<h || cap<54+h)return false;
 *out=(struct hk_tcp_trace){a,b,p[67],h,iplen-h,hk_trace_be32(p+58),hk_trace_be32(p+62),54+h,(unsigned)p[68]*256+p[69]};return true;
}
