/* The local responder owns the registration. Apply each SRV/TXT update or
 * goodbye even when that datagram contains no PTR or the other detail. */
static void service_update(const uint8_t *dns,size_t size,struct awdl_local_ptr *ptr,
                           struct awdl_service_details *details,uint64_t now,uint64_t *until)
{
    if(size<12 || (dns[2]&0xf8)!=0x80 || (dns[3]&15))return;
    struct awdl_local_ptr update={0};
    if(awdl_extract_ptr(dns,size,&update)){
        if(!update.ttl || ptr->length!=update.length || memcmp(ptr->label,update.label,update.length))
            memset(details,0,sizeof(*details));
        *ptr=update;
        *until=now+1000000ull*update.ttl;
    }
    if(!ptr->ttl || now>=*until)return;
    static const uint8_t suffix[]={8,'_','a','i','r','d','r','o','p',4,'_','t','c','p',5,'l','o','c','a','l'};
    unsigned questions=ptr_u16(dns+4),records=ptr_u16(dns+6)+ptr_u16(dns+8)+ptr_u16(dns+10);
    size_t at=12,len;
    uint8_t name[255];
    for(unsigned i=0;i<questions;i++){
        if(!ptr_name(dns,size,&at,name,&len)||size-at<4)return;
        at+=4;
    }
    for(unsigned i=0;i<records;i++){
        if(!ptr_name(dns,size,&at,name,&len)||size-at<10)return;
        unsigned type=ptr_u16(dns+at),cls=ptr_u16(dns+at+2)&0x7fff,n=ptr_u16(dns+at+8);
        bool goodbye=!(ptr_u16(dns+at+4)|ptr_u16(dns+at+6));
        at+=10;
        if(size-at<n)return;
        if(cls==1 && len==1u+ptr->length+sizeof(suffix) && name[0]==ptr->length &&
           !memcmp(name+1,ptr->label,ptr->length) && !memcmp(name+1+ptr->length,suffix,sizeof(suffix))){
            details->instance[0]=ptr->length;memcpy(details->instance+1,ptr->label,ptr->length);
            if(type==16){
                details->txt_len=0;
                if(!goodbye && n && n<=sizeof(details->txt)){
                    for(unsigned j=0;j<n;){unsigned k=dns[at+j++];if(k>n-j)return;j+=k;}
                    memcpy(details->txt,dns+at,n);details->txt_len=n;
                }
            } else if(type==33){
                details->srv_len=0;
                size_t end=at+6,host;
                if(!goodbye && n>=8 && ptr_name(dns,size,&end,name,&host) && end==at+n &&
                   host>=8 && host==7u+name[0] && !memcmp(name+1+name[0],"\005local",6)){
                    unsigned k=name[0];memcpy(details->srv,dns+at,6);
                    memcpy(details->srv+6,name,k+1);details->srv[7+k]=0xc0;
                    details->srv[8+k]=12;details->srv_len=9+k;
                }
            }
        }
        at+=n;
    }
}
