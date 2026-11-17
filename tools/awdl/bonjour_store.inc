#include "bonjour_offload.h"
static LCK_MTX_DECLARE(awif_service_lock,&awif_group);
static struct hk_service_record service_records[8];
static uint64_t service_lease;
static int awif_service_request(const uint8_t *p,unsigned n,uint64_t lease)
{
    if(n<20 || p[0]!=1 || p[1] || p[2] || p[3] || hk_le16(p+16)!=n-20)return EINVAL;
    unsigned op=p[10];
    bool clear=op==1 && n==20;
    struct hk_service_record incoming={0};
    if(!clear){
        if(op>1)return EOPNOTSUPP;
        int error=hk_service_decode(p+20,n-20,&incoming);
        if(error)return error;
    }
    lck_mtx_lock(&awif_service_lock);
    if(clear || service_lease!=lease)memset(service_records,0,sizeof(service_records));
    service_lease=lease;
    int result=0;
    if(!clear){
        int vacant=-1,found=-1;
        for(unsigned i=0;i<8;i++){
            struct hk_service_record *r=&service_records[i];
            if(!r->length && vacant<0)vacant=(int)i;
            if(r->length==incoming.length && !memcmp(r->bytes,incoming.bytes,r->length)){found=(int)i;break;}
        }
        if(op==1){if(found>=0)service_records[found].length=0;}
        else if(found<0){if(vacant<0)result=ENOSPC;else service_records[vacant]=incoming;}
    }
    lck_mtx_unlock(&awif_service_lock);
    printf("awdl-offload: op=%u clear=%u tlv_bytes=%u rc=%d\n",op,clear,incoming.length,result);
    return result;
}
unsigned lx_os_awdl_service_bytes(uint64_t lease,uint8_t *out,unsigned cap)
{
    unsigned used=0;
    lck_mtx_lock(&awif_service_lock);
    if(lease && service_lease==lease){
        for(unsigned i=0;i<8;i++){
            unsigned n=service_records[i].length;
            if(n && service_records[i].bytes[0]==2 && n<=cap-used){memcpy(out+used,service_records[i].bytes,n);used+=n;}
        }
    }
    lck_mtx_unlock(&awif_service_lock);
    return used;
}
/* Packed question list: uint16 LE length followed by a complete ordinary DNS
 * question. The radio's serialized owner handles the requested multicast;
 * this accessor neither sends nor synthesizes a response. */
unsigned lx_os_awdl_query_bytes(uint64_t lease,uint8_t *out,unsigned cap)
{
    unsigned used=0;
    lck_mtx_lock(&awif_service_lock);
    if(lease && service_lease==lease)for(unsigned i=0;i<8;i++){
        const struct hk_service_record *r=&service_records[i];
        unsigned n=r->length;
        if(n>3 && r->bytes[0]==1 && n-1<=cap-used){memcpy(out+used,r->bytes+1,n-1);used+=n-1;}
    }
    lck_mtx_unlock(&awif_service_lock);return used;
}
