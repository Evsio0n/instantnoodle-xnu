/* Encode native SRV/TXT alongside the native PTR. The AWDL dictionary
 * suffixes below follow the service response format in packet-awdl.c. */
struct awdl_service_details {uint8_t instance[64],txt[256],srv[72];unsigned txt_len,srv_len;uint32_t ttl;};

static bool awdl_details(const uint8_t *d,size_t n,const struct awdl_local_ptr *ptr,
                         struct awdl_service_details *out)
{
    static const uint8_t suffix[]={8,'_','a','i','r','d','r','o','p',4,'_','t','c','p',5,'l','o','c','a','l'};
    if(n<12 || !ptr->length || !ptr->ttl)return false;
    unsigned q=ptr_u16(d+4),r=ptr_u16(d+6)+ptr_u16(d+8)+ptr_u16(d+10);
    size_t at=12,len;uint8_t name[255];
    struct awdl_service_details s={0};
    s.ttl=ptr->ttl;s.instance[0]=ptr->length;memcpy(s.instance+1,ptr->label,ptr->length);
    for(unsigned i=0;i<q;i++){
        if(!ptr_name(d,n,&at,name,&len)||n-at<4)return false;
        at+=4;
    }
    for(unsigned i=0;i<r;i++){
        if(!ptr_name(d,n,&at,name,&len)||n-at<10)return false;
        unsigned kind=ptr_u16(d+at),cls=ptr_u16(d+at+2)&0x7fff,sz=ptr_u16(d+at+8);
        uint32_t ttl=((uint32_t)ptr_u16(d+at+4)<<16)|ptr_u16(d+at+6);
        at+=10;if(sz>n-at)return false;
        if(cls==1 && len==1u+ptr->length+sizeof(suffix) && !memcmp(name,s.instance,1+ptr->length) &&
           !memcmp(name+1+ptr->length,suffix,sizeof(suffix)) && (kind==16 || kind==33)){
            if(!ttl)return false;
            if(s.ttl>ttl)s.ttl=ttl;
            if(kind==16 && sz && sz<=sizeof(s.txt)){
                for(unsigned off=0;off<sz;){unsigned k=d[at+off++];if(k>sz-off)return false;off+=k;}
                memcpy(s.txt,d+at,sz);s.txt_len=sz;
            }else if(kind==33 && sz>=8){
                size_t end=at+6,host_len;
                if(!ptr_name(d,n,&end,name,&host_len)||end!=at+sz || host_len<8)return false;
                unsigned k=name[0];
                if(!k || k>63 || host_len!=1+k+6 || memcmp(name+1+k,"\005local",6))return false;
                memcpy(s.srv,d+at,6);memcpy(s.srv+6,name,k+1);
                s.srv[7+k]=0xc0;s.srv[8+k]=0x0c;s.srv_len=9+k;
            }
        }
        at+=sz;
    }
    if(!s.srv_len || !s.txt_len)return false;
    *out=s;return true;
}
static unsigned awdl_detail_tlv(uint8_t *dst,size_t cap,const uint8_t *instance,
                                 unsigned type,const uint8_t *data,unsigned size)
{
    unsigned k=instance[0],body=k+10+size;
    if(k>63 || !size || cap<body+3)return 0;
    dst[0]=2;dst[1]=(uint8_t)body;dst[2]=(uint8_t)(body>>8);
    dst[3]=(uint8_t)(k+4);dst[4]=0;memcpy(dst+5,instance,k+1);
    dst[6+k]=0xc0;dst[7+k]=7;dst[8+k]=(uint8_t)type;
    dst[9+k]=(uint8_t)size;dst[10+k]=(uint8_t)(size>>8);dst[11+k]=dst[12+k]=0;
    memcpy(dst+13+k,data,size);return body+3;
}
