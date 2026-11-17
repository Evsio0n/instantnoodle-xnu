/* 20F66 SET 120. Local DNS-SD keys use mDNSResponder's compression
 * base (_tcp.local at 12, local at 17, _udp.local at 28, key at 39).
 * Action frames use Apple's separate AWDL short-name dictionary. */
#define HK_SERVICE_MAX 480
#include "native_query.h"
struct hk_service_record {
    uint16_t length;
    uint8_t bytes[HK_SERVICE_MAX];
};
static unsigned hk_le16(const uint8_t *p){return p[0]|(unsigned)p[1]<<8;}

static bool hk_service_name(const uint8_t *p,unsigned n,const uint8_t *key,unsigned keylen,
                            uint8_t *out,unsigned *outlen,bool allow_key)
{
    unsigned used=0;
    while(n){
        unsigned k=*p++;n--;
        if((k&0xc0)==0xc0){
            if(n!=1)return false;
            unsigned offset=((k&63)<<8)|*p;
            if(offset==39 && allow_key){
                unsigned extra=0;
                uint8_t expanded[256];
                if(!hk_service_name(key,keylen,NULL,0,expanded,&extra,false)||used+extra>255)return false;
                memcpy(out+used,expanded,extra);*outlen=used+extra;return true;
            }
            unsigned code=offset==12?10:offset==17?12:offset==28?11:0;
            if(!code || used+2>255)return false;
            out[used++]=0xc0;out[used++]=(uint8_t)code;*outlen=used;return true;
        }
        if(!k){if(n)return false;*outlen=used;return used!=0;}
        if(k>63 || k>n || used+1+k>255)return false;
        out[used++]=(uint8_t)k;memcpy(out+used,p,k);used+=k;p+=k;n-=k;
    }
    return false;
}

static int hk_service_decode(const uint8_t *p,unsigned n,struct hk_service_record *record)
{
    if(n && p[0]==1){
        unsigned bytes=0;
        if(!hk_query_question(p,n,record->bytes+3,&bytes))return EINVAL;
        record->bytes[0]=1;record->bytes[1]=(uint8_t)bytes;record->bytes[2]=(uint8_t)(bytes>>8);
        record->length=(uint16_t)(3+bytes);return 0;
    }
    if(n<12 || p[0]!=2 || hk_le16(p+1)!=n-3)return EINVAL;
    unsigned keysize=hk_le16(p+3);
    if(keysize<3 || keysize>n-9)return EINVAL;
    const uint8_t *key=p+5,*value=key+keysize+4;
    unsigned keyname=keysize-3,type=((unsigned)key[keyname]<<8)|key[keyname+1],size=hk_le16(key+keysize);
    if(key[keyname+2]!=1 || size!=n-9-keysize)return EINVAL;
    uint8_t owner[256],target[256];
    unsigned ownerlen=0,targetlen=0;
    if(!hk_service_name(key,keyname,NULL,0,owner,&ownerlen,false))return EINVAL;
    const uint8_t *payload=value;
    unsigned prefix=0;
    if(type==12 || type==33){
        prefix=type==33?6:0;
        if(size<=prefix || !hk_service_name(value+prefix,size-prefix,key,keyname,target,&targetlen,true))return EINVAL;
        size=prefix+targetlen;
    } else if(type!=16)return EOPNOTSUPP;
    unsigned total=3+2+ownerlen+1+4+size;
    if(total>HK_SERVICE_MAX)return EMSGSIZE;
    uint8_t *q=record->bytes;
    q[0]=2;q[1]=(uint8_t)(total-3);q[2]=(uint8_t)((total-3)>>8);
    q[3]=(uint8_t)(ownerlen+1);q[4]=(uint8_t)((ownerlen+1)>>8);
    memcpy(q+5,owner,ownerlen);q+=5+ownerlen;
    *q++=(uint8_t)type;q[0]=(uint8_t)size;q[1]=(uint8_t)(size>>8);q[2]=q[3]=0;q+=4;
    if(type==16)memcpy(q,payload,size);
    else {memcpy(q,payload,prefix);memcpy(q+prefix,target,targetlen);}
    record->length=(uint16_t)total;return 0;
}
