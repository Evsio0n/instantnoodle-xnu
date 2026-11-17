/* Read-only SEE light client primitives. No allocation, kernel floating point
 * or device I/O. Buffers belong to the caller and expire at the next RX. */
#ifndef HK_SEE_LIGHT_H
#define HK_SEE_LIGHT_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <string.h>
#endif

enum { SL_MAX_UID=8, SL_MAX_RATES=24, SL_MAX_QMI=1032 };
struct sl_uid { uint64_t lo,hi; };
struct sl_span { const uint8_t *p; size_t n; };
struct sl_field { unsigned number,wire; uint64_t word; struct sl_span bytes; };
static inline uint16_t sl_u16(const uint8_t *p){return (uint16_t)p[0]|(uint16_t)p[1]<<8;}
static inline uint32_t sl_u32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static inline uint64_t sl_u64(const uint8_t *p){return (uint64_t)sl_u32(p)|((uint64_t)sl_u32(p+4)<<32);}
static inline bool sl_equal(struct sl_uid a, struct sl_uid b){return a.lo==b.lo&&a.hi==b.hi;}
static inline bool sl_nonzero(struct sl_uid a){return a.lo||a.hi;}
static inline bool sl_var(struct sl_span *in,uint64_t *out){
    uint64_t v=0;
    for(unsigned i=0;i<10 && in->n;i++){
        uint8_t c=*in->p++;in->n--;
        if(i==9 && c>1)return false;
        v|=(uint64_t)(c&127)<<(7*i);
        if(!(c&128)){*out=v;return true;}
    }
    return false;
}
static inline bool sl_field(struct sl_span *in,struct sl_field *f){
    uint64_t tag,len;
    if(!sl_var(in,&tag)||tag<8||tag>0xffffffffu)return false;
    f->number=(unsigned)(tag>>3);f->wire=(unsigned)(tag&7);f->word=0;
    f->bytes=(struct sl_span){0};
    switch(f->wire){
        case 0:return sl_var(in,&f->word);
        case 1:len=8;break;
        case 2:if(!sl_var(in,&len))return false;break;
        case 5:len=4;break;
        default:return false;
    }
    if(len>in->n)return false;
    f->bytes=(struct sl_span){in->p,(size_t)len};
    if(f->wire==1)f->word=sl_u64(in->p);
    if(f->wire==5)f->word=sl_u32(in->p);
    in->p+=len;in->n-=len;
    return true;
}
static inline bool sl_uid(struct sl_span in, struct sl_uid *out){
    struct sl_uid uid={0};unsigned fields=0;
    while(in.n){struct sl_field f;if(!sl_field(&in,&f))return false;
        if(f.number==1||f.number==2){
            unsigned mask=1u<<(f.number-1);
            if((fields&mask)||f.wire!=1)return false;
            fields|=mask;
            if(f.number==1)uid.lo=f.word;else uid.hi=f.word;
        }
    }
    if(fields!=3||!sl_nonzero(uid))return false;
    *out=uid;return true;
}
static inline bool sl_str_eq(struct sl_span b,const char *want){
    size_t n=strlen(want);
    if(b.n==n+1 && !b.p[n])b.n--;
    return b.n==n&&!memcmp(b.p,want,n);
}
static inline bool sl_string(struct sl_span b,char *dst,size_t cap){
    if(b.n && !b.p[b.n-1])b.n--;
    if(!b.n||b.n>=cap)return false;
    for(size_t i=0;i<b.n;i++)if(b.p[i]<32||b.p[i]>126)return false;
    memcpy(dst,b.p,b.n);dst[b.n]=0;return true;
}

/* The QRTR endpoint and current QMI client are checked by the caller. */
struct sl_event { uint32_t id;uint64_t ticks;struct sl_span payload; };
struct sl_events { uint64_t client;struct sl_uid source;unsigned count;struct sl_event event[16]; };
static inline bool sl_event(struct sl_span in,struct sl_event *out){
    unsigned seen=0;struct sl_event ev={0};
    while(in.n){struct sl_field f;if(!sl_field(&in,&f))return false;
        if(f.number>3)continue;
        unsigned mask=1u<<(f.number-1);if(seen&mask)return false;seen|=mask;
        if(f.number==1){if(f.wire!=5)return false;ev.id=(uint32_t)f.word;}
        if(f.number==2){if(f.wire!=1)return false;ev.ticks=f.word;}
        if(f.number==3){if(f.wire!=2)return false;ev.payload=f.bytes;}
    }
    if(seen!=7)return false;
    *out=ev;return true;
}
/* Only consume report fields after this returns true. Normal indications are
 * capped at 1000 protobuf bytes, matching the explicitly non-jumbo request. */
static inline bool sl_indication(const uint8_t *p,size_t n,struct sl_events *out){
    if(!p||!out||n<7||n>SL_MAX_QMI||p[0]!=4||sl_u16(p+3)!=0x21||sl_u16(p+5)!=n-7)return false;
    struct sl_span pb={0};bool id=false,body=false;out->count=0;
    for(size_t i=7;i<n;){
        if(n-i<3)return false;
        unsigned type=p[i];size_t len=sl_u16(p+i+1);i+=3;
        if(len>n-i)return false;
        if(type==1){if(id||len!=8)return false;id=true;out->client=sl_u64(p+i);}
        if(type==2){
            if(body||len<2||len-2>1000||sl_u16(p+i)!=len-2)return false;
            body=true;pb=(struct sl_span){p+i+2,len-2};
        }
        i+=len;
    }
    if(!id||!body)return false;
    bool source=false;
    while(pb.n){struct sl_field f;if(!sl_field(&pb,&f))return false;
        if(f.number==1){if(source||f.wire!=2||!sl_uid(f.bytes,&out->source))return false;source=true;}
        if(f.number==2){
            if(f.wire!=2||out->count==16||!sl_event(f.bytes,&out->event[out->count]))return false;
            out->count++;
        }
    }
    return source&&out->count;
}

struct sl_lookup { unsigned count;struct sl_uid uids[SL_MAX_UID]; };
static inline bool sl_lookup_named_event(struct sl_uid source,const struct sl_event *e,
                                         const char *datatype,struct sl_lookup *out){
    const struct sl_uid lookup={UINT64_C(0xabababababababab),UINT64_C(0xabababababababab)};
    if(!sl_equal(source,lookup)||e->id!=768)return false;
    struct sl_span pb=e->payload;struct sl_lookup found={0};bool name=false;
    while(pb.n){struct sl_field f;if(!sl_field(&pb,&f))return false;
        if(f.number==1){if(name||f.wire!=2||!sl_str_eq(f.bytes,datatype))return false;name=true;}
        if(f.number==2){
            struct sl_uid u;if(f.wire!=2||found.count==SL_MAX_UID||!sl_uid(f.bytes,&u))return false;
            for(unsigned i=0;i<found.count;i++)if(sl_equal(u,found.uids[i]))return false;
            found.uids[found.count++]=u;
        }
    }
    if(!name)return false;*out=found;return true;
}

static inline bool sl_lookup_event(struct sl_uid source,const struct sl_event *e,struct sl_lookup *out){
    return sl_lookup_named_event(source,e,"ambient_light",out);
}

enum { SL_NAME=0,SL_VENDOR=1,SL_TYPE=2,SL_AVAILABLE=3,SL_API=5,SL_RATES=6,
       SL_STREAM=16,SL_HW=18,SL_RIGID=19,SL_PHYSICAL=21 };
struct sl_attrs {
    struct sl_uid source;
    uint32_t present;
    char name[64],vendor[64],type[48],api[4][64];
    unsigned api_count;bool extra_api;
    uint64_t hw,rigid[8],stream;
    unsigned rigid_count,rate_count;
    uint32_t rates[SL_MAX_RATES];
    bool available,physical,als_api;
};
static inline uint32_t sl_bit(unsigned n){return UINT32_C(1)<<n;}
static inline bool sl_attr_data(struct sl_attrs *a,unsigned id,struct sl_span pb,unsigned index){
    struct sl_field v={0};bool found=false;
    while(pb.n){struct sl_field f;if(!sl_field(&pb,&f))return false;
        if(f.number>5)continue;
        if(found)return false;found=true;v=f;
    }
    if(!found)return false;
    switch(id){
    case SL_NAME: case SL_VENDOR: case SL_TYPE:
        if(index||v.number!=2||v.wire!=2)return false;
        if(id==SL_NAME)return sl_string(v.bytes,a->name,sizeof(a->name));
        if(id==SL_VENDOR)return sl_string(v.bytes,a->vendor,sizeof(a->vendor));
        return sl_string(v.bytes,a->type,sizeof(a->type));
    case SL_API:
        if(v.number!=2||v.wire!=2)return false;
        a->als_api|=sl_str_eq(v.bytes,"sns_ambient_light.proto");
        if(a->api_count<4 && sl_string(v.bytes,a->api[a->api_count],sizeof(a->api[0])))a->api_count++;
        else a->extra_api=true;
        return true;
    case SL_AVAILABLE: case SL_PHYSICAL:
        if(index||v.number!=5||v.wire!=0||v.word>1)return false;
        if(id==SL_AVAILABLE)a->available=v.word!=0;else a->physical=v.word!=0;
        return true;
    case SL_STREAM: case SL_HW: case SL_RIGID:
        if(v.number!=4||v.wire!=1)return false; /* sfixed64 */
        if(id==SL_RIGID){if(index>=8)return false;a->rigid[a->rigid_count++]=v.word;return true;}
        if(index)return false;
        if(id==SL_STREAM)a->stream=v.word;else a->hw=v.word;
        return true;
    case SL_RATES:
        if(v.number!=3||v.wire!=5||index>=SL_MAX_RATES||!v.word||v.word>=UINT32_C(0x7f800000))return false;
        a->rates[a->rate_count++]=(uint32_t)v.word;return true;
    default:return false;
    }
}
static inline bool sl_attr(struct sl_attrs *a,struct sl_span pb){
    uint64_t id=0;struct sl_span vals={0};unsigned seen=0;
    while(pb.n){struct sl_field f;if(!sl_field(&pb,&f))return false;
        if(f.number==1){if((seen&1)||f.wire!=0)return false;seen|=1;id=f.word;}
        if(f.number==2){if((seen&2)||f.wire!=2)return false;seen|=2;vals=f.bytes;}
    }
    if(seen!=3)return false;
    switch(id){
    case SL_NAME:case SL_VENDOR:case SL_TYPE:case SL_API:case SL_AVAILABLE:case SL_PHYSICAL:
    case SL_HW:case SL_STREAM:case SL_RIGID:case SL_RATES:break;
    default:return true;
    }
    uint32_t bit=sl_bit((unsigned)id);
    if(a->present&bit)return false;
    a->present|=bit;
    unsigned n=0;
    while(vals.n){struct sl_field f;if(!sl_field(&vals,&f))return false;
        if(f.number==1){if(f.wire!=2||!sl_attr_data(a,(unsigned)id,f.bytes,n++))return false;}
    }
    return n!=0;
}
static inline bool sl_attributes(struct sl_uid expected,struct sl_uid source,const struct sl_event *e,struct sl_attrs *out){
    if(e->id!=128||!sl_nonzero(expected)||!sl_equal(expected,source))return false;
    struct sl_attrs a={.source=source};struct sl_span pb=e->payload;
    while(pb.n){struct sl_field f;if(!sl_field(&pb,&f))return false;
        if(f.number==1&&(f.wire!=2||!sl_attr(&a,f.bytes)))return false;
    }
    if(!a.present)return false;
    *out=a;return true;
}

enum sl_mode {SL_UNKNOWN,SL_ON_CHANGE,SL_STREAMING};
struct sl_config { struct sl_uid source;enum sl_mode mode;uint32_t rate_binary32; };
static inline bool sl_config_for(const struct sl_attrs *a, struct sl_config *out){
    const uint32_t need=sl_bit(SL_TYPE)|sl_bit(SL_AVAILABLE)|sl_bit(SL_STREAM)|sl_bit(SL_PHYSICAL);
    if(!sl_nonzero(a->source)||(a->present&need)!=need||strcmp(a->type,"ambient_light")||
       !a->physical||!a->available||((a->present&sl_bit(SL_API))&&!a->als_api))return false;
    struct sl_config c={.source=a->source};
    if(a->stream==1)c.mode=SL_ON_CHANGE;
    else if(a->stream==0){
        /* Positive IEEE bit patterns sort in numeric order. Prefer the smallest
         * advertised rate >=1Hz and <=5Hz. Otherwise the slowest <=20Hz. */
        uint32_t preferred=0, fallback=0;
        for(unsigned i=0;i<a->rate_count;i++){
            uint32_t v=a->rates[i];
            if(v<=UINT32_C(0x41a00000)&&(!fallback||v<fallback))fallback=v;
            if(v>=UINT32_C(0x3f800000)&&v<=UINT32_C(0x40a00000)&&(!preferred||v<preferred))preferred=v;
        }
        c.rate_binary32=preferred?preferred:fallback;
        if(!c.rate_binary32)return false;c.mode=SL_STREAMING;
    }else return false;
    *out=c;return true;
}

/* 7-byte SSC QMI request + TLV1 with uint16 protobuf size + TLV10(jumbo=0).
 * The request wrapper matches the captured SDSP API: APSS=1, WAKEUP=0,
 * fixed32 message ID. Native attribute requests explicitly encode bool field 2 as false. */
static inline void sl_le16(uint8_t *p,unsigned v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static inline void sl_le32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
static inline void sl_le64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(i*8));}
static inline size_t sl_request(uint8_t *out,size_t cap,uint16_t txn,struct sl_uid uid,
                                uint32_t mid,uint32_t rate){
    if(!out||!txn||!sl_nonzero(uid))return 0;
    if(mid!=1&&mid!=513&&mid!=514)return 0;
    if(mid==513&&(!rate||rate>UINT32_C(0x41a00000)))return 0;
    if(mid!=513&&rate)return 0;
    /* Every nested length here is <=127. Never truncate a general varint. */
    uint8_t b[64];size_t i=0;
    b[i++]=0x0a;b[i++]=18;b[i++]=9;sl_le64(b+i,uid.lo);i+=8;
    b[i++]=17;sl_le64(b+i,uid.hi);i+=8;
    b[i++]=0x15;sl_le32(b+i,mid);i+=4;
    const uint8_t susp[]={0x1a,4,8,1,16,0};
    memcpy(b+i,susp,sizeof(susp));i+=sizeof(susp);
    b[i++]=0x22;
    if(mid==513){b[i++]=7;b[i++]=0x12;b[i++]=5;b[i++]=0x0d;sl_le32(b+i,rate);i+=4;}
    else if(mid==1){b[i++]=4;b[i++]=0x12;b[i++]=2;b[i++]=0x10;b[i++]=0;}
    else b[i++]=0;
    size_t total=7+5+i+4;
    if(cap<total)return 0;
    out[0]=0;sl_le16(out+1,txn);sl_le16(out+3,0x20);sl_le16(out+5,(unsigned)total-7);
    out[7]=1;sl_le16(out+8,(unsigned)i+2);sl_le16(out+10,(unsigned)i);memcpy(out+12,b,i);
    out[12+i]=0x10;out[13+i]=1;out[14+i]=0;out[15+i]=0;
    return total;
}
static inline size_t sl_attribute_request(uint8_t *out,size_t cap,uint16_t txn,struct sl_uid uid){
    return sl_request(out,cap,txn,uid,1,0);
}
static inline size_t sl_config_request(uint8_t *out,size_t cap,uint16_t txn,const struct sl_config *c){
    if(!c||(c->mode!=SL_ON_CHANGE&&c->mode!=SL_STREAMING))return 0;
    return sl_request(out,cap,txn,c->source,c->mode==SL_ON_CHANGE?514:513,c->rate_binary32);
}
struct sl_measurement { struct sl_uid source;uint64_t see_ticks;uint32_t status,lux_binary32,adc_binary32; };
static inline bool sl_sample(struct sl_uid expected,struct sl_uid source,
                             const struct sl_event *e,struct sl_measurement *out){
    if(e->id!=1025||!e->ticks||!sl_nonzero(expected)||!sl_equal(expected,source))return false;
    struct sl_span b=e->payload;uint32_t values[2];unsigned n=0;bool status_seen=false;
    struct sl_measurement result={.source=source,.see_ticks=e->ticks};
    while(b.n){struct sl_field f;if(!sl_field(&b,&f))return false;
        if(f.number==1){
            if((f.wire!=2&&f.wire!=5)||!f.bytes.n||f.bytes.n%4||f.bytes.n/4>2u-n)return false;
            for(size_t i=0;i<f.bytes.n;i+=4)values[n++]=sl_u32(f.bytes.p+i);
        }else if(f.number==2){
            if(status_seen||f.wire!=0||f.word>3)return false;
            result.status=(unsigned)f.word;status_seen=true;
        }
    }
    /* Native ALS contract: data[0] lux, data[1] ADC. Finite binary32 only. */
    if(!status_seen||n!=2)return false;
    if(values[0]==UINT32_C(0x80000000))values[0]=0;
    if(values[0]>=UINT32_C(0x4f800000)||(values[1]&UINT32_C(0x7fffffff))>=UINT32_C(0x7f800000))return false;
    result.lux_binary32=values[0];result.adc_binary32=values[1];*out=result;return true;
}

/* Calibration attributes and physical configuration are evidence to record.
 * Never reapply these coefficients to data[0]; the ALS event is already lux.
 * A generic calibration packet does not attest per-unit factory provenance. */
struct sl_calibration {
    uint32_t bias[3],scale[3],matrix[9],status,cal_id;unsigned nb,ns,nm;
    bool has_id;
};
static inline bool sl_float_array(struct sl_field f,uint32_t *dst,unsigned capacity,unsigned *used){
    if((f.wire!=2&&f.wire!=5)||f.bytes.n%4 || *used>capacity || f.bytes.n/4>capacity-*used)return false;
    for(size_t off=0;off<f.bytes.n;off+=4){
        uint32_t v=sl_u32(f.bytes.p+off);
        if((v&0x7f800000u)==0x7f800000u)return false;
        dst[(*used)++]=v;
    }
    return true;
}
static inline bool sl_cal_event(struct sl_uid expected,struct sl_uid source,
                                const struct sl_event *e,struct sl_calibration *out){
    if(e->id!=1022||!sl_nonzero(expected)||!sl_equal(expected,source))return false;
    struct sl_calibration c={0};struct sl_span b=e->payload;bool status=false;
    while(b.n){struct sl_field f;if(!sl_field(&b,&f))return false;
        if(f.number==1&&!sl_float_array(f,c.bias,3,&c.nb))return false;
        if(f.number==2&&!sl_float_array(f,c.scale,3,&c.ns))return false;
        if(f.number==3&&!sl_float_array(f,c.matrix,9,&c.nm))return false;
        if(f.number==4){if(status||f.wire!=0||f.word>3)return false;status=true;c.status=(unsigned)f.word;}
        if(f.number==5){if(c.has_id||f.wire!=5)return false;c.has_id=true;c.cal_id=(uint32_t)f.word;}
    }
    if(!status)return false;*out=c;return true;
}
/* Recover the nominal configured sampling rate (when supplied), not an
 * inferred rate from an on-change event gap. Zero means rate absent. */
static inline bool sl_physical_config(struct sl_uid expected,struct sl_uid source,
                                      const struct sl_event *e,uint32_t *rate){
    if(e->id!=768||!sl_nonzero(expected)||!sl_equal(expected,source))return false;
    struct sl_span b=e->payload;uint32_t r=0;
    while(b.n){struct sl_field f;if(!sl_field(&b,&f))return false;
        if(f.number==1){
            if(r||f.wire!=5||!f.word||f.word>=0x7f800000u)return false;r=(uint32_t)f.word;
        }
    }
    *rate=r;return true;
}

/* These booleans come from independent board/registry and per-unit calibration
 * evidence. No standard name, HW_ID, list position or HIGH sample proves front. */
struct sl_evidence {struct sl_uid uid;uint64_t generation;bool front,calibration;};
static inline bool sl_publishable(const struct sl_measurement *m,const struct sl_evidence *v,
                                  uint64_t live_generation,uint64_t last_see_ticks){
    return m&&v&&v->generation&&v->generation==live_generation&&v->front&&v->calibration&&
        sl_nonzero(v->uid)&&sl_equal(m->source,v->uid)&&m->status==3&&m->see_ticks>last_see_ticks;
}
#endif
