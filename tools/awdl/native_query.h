#ifndef HK_AWDL_NATIVE_QUERY_H
#define HK_AWDL_NATIVE_QUERY_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
/* Native SET120 type1 is a subscription key, not a DNS answer. Execute its
 * actual IN question through ordinary IPv6 mDNS in software. Do not claim a
 * firmware Service Request IE or manufacture any remote service record. */
static inline bool hk_query_expand(const uint8_t *p,unsigned n,uint8_t *out,unsigned *length)
{
    static const uint8_t tcp[]={4,'_','t','c','p',5,'l','o','c','a','l',0};
    static const uint8_t udp[]={4,'_','u','d','p',5,'l','o','c','a','l',0};
    static const uint8_t local[]={5,'l','o','c','a','l',0};
    unsigned used=0;
    while(n){
        unsigned k=*p++;--n;
        if((k&0xc0)==0xc0){
            if(n!=1)return false;
            unsigned offset=((k&63)<<8)|*p,size=0;const uint8_t *suffix=NULL;
            if(offset==12){suffix=tcp;size=sizeof(tcp);}
            else if(offset==17){suffix=local;size=sizeof(local);}
            else if(offset==28){suffix=udp;size=sizeof(udp);}
            else return false;
            if(used+size>255)return false;
            __builtin_memcpy(out+used,suffix,size);*length=used+size;return true;
        }
        if(!k){if(n || used+1>255)return false;out[used++]=0;*length=used;return used>1;}
        if(k>63 || k>n || used+k+1>254)return false;
        out[used++]=(uint8_t)k;__builtin_memcpy(out+used,p,k);used+=k;p+=k;n-=k;
    }
    return false;
}
static inline bool hk_query_question(const uint8_t *p,unsigned n,uint8_t *out,unsigned *size)
{
    if(n<11 || p[0]!=1 || (unsigned)(p[1]|p[2]<<8)!=n-3 || p[3] || p[4] || p[5] || p[6] || p[n-1]!=1)return false;
    unsigned name=0;
    if(!hk_query_expand(p+7,n-10,out,&name))return false;
    unsigned type=(unsigned)p[n-3]<<8|p[n-2];
    if(type!=12 && type!=16 && type!=33)return false;
    out[name++]=(uint8_t)(type>>8);out[name++]=(uint8_t)type;out[name++]=0;out[name++]=1;
    *size=name;return true;
}
static inline unsigned hk_query_frame(uint8_t *out,unsigned cap,const uint8_t mac[6],const uint8_t source[16],const uint8_t *question,unsigned bytes)
{
    unsigned total=14+40+8+12+bytes,udp=8+12+bytes;
    if(!out || !mac || !source || !question || bytes<5 || bytes>259 || total>cap || (mac[0]&1) || source[0]!=0xfe || (source[1]&0xc0)!=0x80)return 0;
    __builtin_memset(out,0,total);out[0]=out[1]=0x33;out[5]=0xfb;__builtin_memcpy(out+6,mac,6);out[12]=0x86;out[13]=0xdd;
    uint8_t *ip=out+14,*u=ip+40,*dns=u+8;
    ip[0]=0x60;ip[4]=(uint8_t)(udp>>8);ip[5]=(uint8_t)udp;ip[6]=17;ip[7]=255;
    __builtin_memcpy(ip+8,source,16);ip[24]=0xff;ip[25]=2;ip[39]=0xfb;
    u[0]=u[2]=0x14;u[1]=u[3]=0xe9;u[4]=(uint8_t)(udp>>8);u[5]=(uint8_t)udp;
    dns[5]=1;__builtin_memcpy(dns+12,question,bytes);
    uint32_t sum=udp+17;
    for(unsigned i=8;i<40;i+=2)sum+=(unsigned)ip[i]<<8|ip[i+1];
    for(unsigned i=0;i<udp;i+=2)sum+=(unsigned)u[i]<<8|(i+1<udp?u[i+1]:0);
    while(sum>>16)sum=(sum&65535)+(sum>>16);
    unsigned checksum=(~sum)&65535;if(!checksum)checksum=65535;
    u[6]=(uint8_t)(checksum>>8);u[7]=(uint8_t)checksum;
    return total;
}
#endif
