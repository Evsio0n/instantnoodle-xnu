/* Read-only SSC registry diagnostic. Caller supplies the discovered endpoint
 * and retains DMA/GLINK ownership. This code does not submit any hardware I/O. */
#ifndef HK_REGISTRY_READ_H
#define HK_REGISTRY_READ_H
#include "hk_see_light.h"

enum { SR_ALSPS_PLATFORM, SR_ALSPS_CONFIG, SR_CCT_PLATFORM, SR_CCT_CONFIG, SR_COUNT };
enum { SR_I64=13, SR_STRING=11 };
struct sr_key { const char *name; unsigned type; int64_t integer; const char *str; uint32_t version; };
struct sr_group { const char *name; const struct sr_key *keys; unsigned count; };
#include "registry_keys.h"
struct sr_result {
    uint32_t seen, mismatch, has_version, version_mismatch;
    unsigned items, extra, nested, events;
    uint64_t ticks;
};
struct sr_client {
    uint32_t node, port;
    uint64_t client_id;
    struct sl_uid registry;
    unsigned sent, ack_ok, ack_fail, received;
    uint16_t txn[SR_COUNT];
    bool invalidated;
    struct sr_result result[SR_COUNT];
};
static inline bool sr_bind(struct sr_client *c,unsigned service,unsigned version,
                           unsigned instance,uint32_t node,uint32_t port)
{
    if(!c || service!=400 || version!=1 || instance!=0 || !node || !port ||
       node==UINT32_MAX || port>=UINT32_C(0xfffffffe))return false;
    *c=(struct sr_client){.node=node,.port=port};
    return true;
}
/* Called on new lookup indications on this client port. Exactly one
 * registry SUID is required. A later removal/change invalidates the probe. */
static inline bool sr_observe_lookup(struct sr_client *c,uint32_t node,uint32_t port,
                                     const uint8_t *packet,size_t size)
{
    const struct sl_uid lookup={UINT64_C(0xabababababababab),UINT64_C(0xabababababababab)};
    struct sl_events events;
    if(!c || c->invalidated || c->node!=node || c->port!=port ||
       !sl_indication(packet,size,&events)||!sl_equal(events.source,lookup))return false;
    bool recognized=false;
    for(unsigned i=0;i<events.count;i++){
        const struct sl_event *e=&events.event[i];if(e->id!=768)continue;
        struct sl_span b=e->payload,name={0};
        unsigned count=0;struct sl_uid id={0};
        while(b.n){
            struct sl_field f;if(!sl_field(&b,&f))return false;
            if(f.number==1){
                if(name.p||f.wire!=2)return false;
                name=f.bytes;
            }
            if(f.number==2){
                if(f.wire!=2||!sl_uid(f.bytes,&id))return false;
                count++;
            }
        }
        if(!name.p||!sl_str_eq(name,"registry"))continue;
        if(count!=1 || (sl_nonzero(c->registry) &&
           (!sl_equal(c->registry,id)||c->client_id!=events.client))){
            if(sl_nonzero(c->registry))c->invalidated=true;
            return false;
        }
        c->registry=id;c->client_id=events.client;recognized=true;
    }
    return recognized;
}
/* Both SUID lookup and registry READ use SEE message 512, with distinct
 * target services and payloads. This firmware includes the trailing NUL. */
static inline size_t sr_named_request(struct sl_uid target,const char *name,uint16_t txn,
                                      bool lookup,uint8_t *out,size_t cap)
{
    if(!sl_nonzero(target)||!name||!txn||!out)return 0;
    size_t n=strlen(name)+1,extra=lookup?4:0,proto=37+n+extra,total=16+proto;
    if(n>64 || total>cap)return 0;
    out[0]=0;sl_le16(out+1,txn);sl_le16(out+3,0x20);sl_le16(out+5,(unsigned)total-7);
    out[7]=1;sl_le16(out+8,(unsigned)proto+2);sl_le16(out+10,(unsigned)proto);
    uint8_t *p=out+12;
    *p++=0x0a;*p++=18;*p++=9;sl_le64(p,target.lo);p+=8;
    *p++=17;sl_le64(p,target.hi);p+=8;
    *p++=0x15;sl_le32(p,512);p+=4; /* READ_REQ or SUID_REQ */
    const uint8_t suspend[]={0x1a,4,8,1,16,0};
    memcpy(p,suspend,sizeof(suspend));p+=sizeof(suspend);
    *p++=0x22;*p++=(uint8_t)(4+n+extra);*p++=0x12;*p++=(uint8_t)(2+n+extra);
    *p++=0x0a;*p++=(uint8_t)n;memcpy(p,name,n);p+=n;
    if(lookup){*p++=0x10;*p++=1;*p++=0x18;*p++=0;} /* all SUIDs, with updates */
    *p++=0x10;*p++=1;*p++=0;*p++=0;
    return (size_t)(p-out)==total?total:0;
}
static inline size_t sr_request(const struct sr_client *c,unsigned group,uint16_t txn,
                                uint8_t *out,size_t cap)
{
    if(!c || group>=SR_COUNT || !c->port || c->invalidated || (c->sent&(1u<<group)))return 0;
    for(unsigned i=0;i<SR_COUNT;i++)if(c->txn[i]==txn)return 0;
    return sr_named_request(c->registry,sr_groups[group].name,txn,false,out,cap);
}
/* Call exactly once after successful transport publication. */
static inline bool sr_sent(struct sr_client *c,unsigned group,uint16_t txn)
{
    if(!c || c->invalidated || !sl_nonzero(c->registry) || group>=SR_COUNT ||
       !txn || (c->sent&(1u<<group)))return false;
    for(unsigned i=0;i<SR_COUNT;i++)if(c->txn[i]==txn)return false;
    c->txn[group]=txn;c->sent|=1u<<group;return true;
}
/* Returns +1 accepted, -1 explicitly rejected, zero malformed or unrelated. */
static inline int sr_ack_result(const uint8_t *p,size_t n,uint64_t client_id)
{
    if(!p||n<7||n>SL_MAX_QMI||p[0]!=2||sl_u16(p+3)!=0x20||sl_u16(p+5)!=n-7)return 0;
    bool response=false,id=false,status=false,success=true;
    for(size_t at=7;at<n;){
        if(n-at<3)return 0;
        unsigned t=p[at];size_t len=sl_u16(p+at+1);at+=3;
        if(len>n-at)return 0;
        if(t==2){if(response||len!=4)return 0;response=true;
            unsigned result=sl_u16(p+at),error=sl_u16(p+at+2);
            if(result>1 || ((result==0)!=(error==0)))return 0;
            if(result)success=false;}
        else if(t==0x10){if(id||len!=8||sl_u64(p+at)!=client_id)return 0;id=true;}
        else if(t==0x11){if(status||len!=4)return 0;status=true;
            if(sl_u32(p+at)!=0)success=false;}
        at+=len;
    }
    return response ? (success ? 1 : -1) : 0;
}
static inline bool sr_ack(struct sr_client *c,uint32_t node,uint32_t port,const uint8_t *p,size_t n)
{
    if(!c||!p||node!=c->node||port!=c->port||c->invalidated||n<7||n>SL_MAX_QMI||
       p[0]!=2||sl_u16(p+3)!=0x20||sl_u16(p+5)!=n-7)return false;
    unsigned group=0;
    for(;group<SR_COUNT;group++)if((c->sent&(1u<<group))&&c->txn[group]==sl_u16(p+1))break;
    if(group==SR_COUNT||((c->ack_fail|c->ack_ok)&(1u<<group)))return false;
    int result=sr_ack_result(p,n,c->client_id);
    if(!result)return false;
    if(result>0)c->ack_ok|=1u<<group;else c->ack_fail|=1u<<group;
    return true;
}
static inline bool sr_data(const struct sr_group *spec,struct sl_span data,
                           unsigned depth,struct sr_result *out)
{
    if(depth>3)return false;
    while(data.n){
        struct sl_field item;if(!sl_field(&data,&item))return false;
        if(item.number!=3)continue;
        if(item.wire!=2||out->items>=64)return false;
        out->items++;
        struct sl_span b=item.bytes,name={0};struct sl_field value={0};
        uint32_t version=0;bool has_version=false;
        while(b.n){
            struct sl_field f;if(!sl_field(&b,&f))return false;
            if(f.number==1){if(name.p||f.wire!=2)return false;name=f.bytes;}
            else if(f.number==2){if(has_version||f.wire!=5)return false;has_version=true;version=(uint32_t)f.word;}
            else if(f.number>=10&&f.number<=13){
                unsigned wire=f.number==13?1:f.number==12?5:2;
                if(value.number||f.wire!=wire)return false;
                value=f;
            }
        }
        if(!name.p||!name.n||!value.number)return false;
        unsigned key=0;
        if(depth==0)for(;key<spec->count;key++)if(sl_str_eq(name,spec->keys[key].name))break;
        if(depth==0&&key<spec->count){
            const struct sr_key *k=&spec->keys[key];uint32_t bit=1u<<key;
            if(out->seen&bit)return false;
            out->seen|=bit;
            if(has_version){out->has_version|=bit;if(version!=k->version)out->version_mismatch|=bit;}
            if(value.number!=k->type ||
               (k->type==SR_I64 && value.word!=(uint64_t)k->integer) ||
               (k->type==SR_STRING && !sl_str_eq(value.bytes,k->str)))out->mismatch|=bit;
        }else out->extra++;
        if(value.number==10){out->nested++;if(!sr_data(spec,value.bytes,depth+1,out))return false;}
    }
    return true;
}
/* Parse the *complete* indication before committing. Source, bound client,
 * requested group name and message type all match before any value is used. */
static inline bool sr_report(struct sr_client *c,uint32_t node,uint32_t port,
                             const uint8_t *p,size_t n)
{
    struct sl_events events;
    if(!c||c->invalidated||node!=c->node||port!=c->port||!sl_nonzero(c->registry)||
       !sl_indication(p,n,&events)||!sl_equal(events.source,c->registry)||
       events.client!=c->client_id)return false;
    struct sr_result pending[SR_COUNT];unsigned changed=0;
    for(unsigned i=0;i<events.count;i++){
        const struct sl_event *e=&events.event[i];if(e->id!=514)continue;
        struct sl_span b=e->payload,name={0},data={0};
        while(b.n){
            struct sl_field f;if(!sl_field(&b,&f))return false;
            if(f.number==1){if(name.p||f.wire!=2)return false;name=f.bytes;}
            if(f.number==2){if(data.p||f.wire!=2)return false;data=f.bytes;}
        }
        if(!name.p||!data.p)return false;
        unsigned g=0;for(;g<SR_COUNT;g++)if(sl_str_eq(name,sr_groups[g].name))break;
        if(g==SR_COUNT||!(c->sent&(1u<<g)))continue;
        if(changed&(1u<<g))return false;
        struct sr_result r={.ticks=e->ticks,.events=c->result[g].events+1};
        if(!sr_data(&sr_groups[g],data,0,&r))return false;
        pending[g]=r;changed|=1u<<g;
    }
    for(unsigned i=0;i<SR_COUNT;i++)if(changed&(1u<<i))c->result[i]=pending[i];
    c->received|=changed;return changed!=0;
}
static inline bool sr_values_match(const struct sr_client *c,unsigned group)
{
    if(!c||group>=SR_COUNT||c->invalidated)return false;
    unsigned bit=1u<<group;
    return (c->received&c->ack_ok&bit) && !c->result[group].mismatch &&
        c->result[group].seen==((1u<<sr_groups[group].count)-1u);
}
#endif
