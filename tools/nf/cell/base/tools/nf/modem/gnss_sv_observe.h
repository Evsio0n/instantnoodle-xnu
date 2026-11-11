/* libqmi LOC GNSS Sv Info (0x25), 28-byte packed records. Counts only. */
#ifndef HK_GNSS_SV_OBSERVE_H
#define HK_GNSS_SV_OBSERVE_H
struct mdm_sv_wire_counts { uint32_t indications, absent, empty, malformed, no_cn0; };
static int mdm_sv_has_cn0(const uint8_t *entry) { return (entry[0] & 0x80) != 0; }
static void mdm_sv_observe(struct mdm_sv_wire_counts *c,const uint8_t *q,size_t n)
{
    if(n<5 || q[0]!=4 || q[3]!=0x25 || q[4]!=0)return;
    c->indications++;
    if(n<7 || (size_t)(q[5]|q[6]<<8)!=n-7){c->malformed++;return;}
    int count=-1; unsigned invalid=0;
    for(size_t at=7;at<n;){
        if(n-at<3){c->malformed++;return;}
        unsigned t=q[at],len=q[at+1]|q[at+2]<<8;at+=3;
        if(len>n-at){c->malformed++;return;}
        if(t==0x10){
            if(count>=0 || !len || len!=1u+28u*q[at]){c->malformed++;return;}
            count=q[at];
            for(int i=0;i<count;i++)invalid+=!mdm_sv_has_cn0(q+at+1+28*i);
        }
        at+=len;
    }
    if(count<0)c->absent++;
    else if(!count)c->empty++;
    c->no_cn0+=invalid;
}
#endif
