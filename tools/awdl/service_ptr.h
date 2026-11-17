#ifndef HK_AWDL_SERVICE_PTR_H
#define HK_AWDL_SERVICE_PTR_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct awdl_local_ptr { uint8_t label[63],length; uint32_t ttl; };
static unsigned ptr_u16(const uint8_t *p){return ((unsigned)p[0]<<8)|p[1];}

/* Expand DNS labels while following strictly backward compression pointers. */
static bool ptr_name(const uint8_t *dns,size_t n,size_t *cursor,uint8_t *out,size_t *len)
{
    size_t at=*cursor,used=0,next=0;
    for(unsigned hops=0;at<n && hops<128;hops++){
        unsigned k=dns[at++];
        if(k==0){*cursor=next?next:at;*len=used;return true;}
        if((k&0xc0)==0xc0){
            if(at>=n)return false;
            unsigned target=((k&63)<<8)|dns[at++];
            if(target>=at-2)return false;
            if(!next)next=at;
            at=target;continue;
        }
        if(k>63 || k>n-at || used+1+k>255)return false;
        out[used++]=(uint8_t)k;
        memcpy(out+used,dns+at,k);used+=k;at+=k;
    }
    return false;
}

/* Parse only an actual locally generated AirDrop service PTR. A goodbye
 * carries ttl=0 and withdraws the announcement. No record is invented. */
static bool awdl_extract_ptr(const uint8_t *d,size_t n,struct awdl_local_ptr *out)
{
    static const uint8_t service[]={8,'_','a','i','r','d','r','o','p',4,'_','t','c','p',5,'l','o','c','a','l'};
    if(n<12 || (d[2]&0xf8)!=0x80 || (d[3]&15))return false;
    unsigned questions=ptr_u16(d+4),rr=ptr_u16(d+6)+ptr_u16(d+8)+ptr_u16(d+10);
    uint8_t labels[255];size_t at=12,len=0;
    for(unsigned i=0;i<questions;i++){
        if(!ptr_name(d,n,&at,labels,&len)||n-at<4)return false;
        at+=4;
    }
    for(unsigned i=0;i<rr;i++){
        if(!ptr_name(d,n,&at,labels,&len)||n-at<10)return false;
        unsigned type=ptr_u16(d+at),cls=ptr_u16(d+at+2)&0x7fff,size=ptr_u16(d+at+8);
        uint32_t ttl=((uint32_t)ptr_u16(d+at+4)<<16)|ptr_u16(d+at+6);
        at+=10;if(size>n-at)return false;
        if(type==12 && cls==1 && len==sizeof(service) && !memcmp(labels,service,len)){
            size_t end=at;
            if(!ptr_name(d,n,&end,labels,&len)||end!=at+size || !len)return false;
            unsigned k=labels[0];
            if(!k || k>63 || len!=1+k+sizeof(service) || memcmp(labels+1+k,service,sizeof(service)))return false;
            memcpy(out->label,labels+1,k);out->length=k;out->ttl=ttl;return true;
        }
        at+=size;
    }
    return false;
}

static unsigned awdl_append_ptr(uint8_t *dst,size_t cap,const struct awdl_local_ptr *p)
{
    unsigned k=p->length,body=12+k;
    if(!p->ttl || !k || k>63 || cap<3+body)return 0;
    /* AWDL short suffix 0xc007 = _airdrop._tcp.local. */
    dst[0]=2;dst[1]=(uint8_t)body;dst[2]=0;
    dst[3]=3;dst[4]=0;dst[5]=0xc0;dst[6]=7;dst[7]=12;
    dst[8]=(uint8_t)(3+k);dst[9]=dst[10]=dst[11]=0;
    dst[12]=(uint8_t)k;memcpy(dst+13,p->label,k);
    dst[13+k]=0xc0;dst[14+k]=7;
    return 3+body;
}
#endif
