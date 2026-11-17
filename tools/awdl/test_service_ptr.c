#include <string.h>
#include <assert.h>
#include <stdio.h>
#include "service_ptr.h"
#include "service_details.h"
int main(int argc,char **argv){
    assert(argc==2);
    uint8_t dns[2048],tlv[80];struct awdl_local_ptr p={0};
    FILE *f=fopen(argv[1],"rb");assert(f);
    size_t n=fread(dns,1,sizeof(dns),f);fclose(f);
    assert(awdl_extract_ptr(dns,n,&p));
    assert(p.length==12 && p.ttl==4500);
    assert(awdl_append_ptr(tlv,26,&p)==0);
    assert(awdl_append_ptr(tlv,sizeof(tlv),&p)==27);
    assert(tlv[0]==2 && tlv[1]==24 && tlv[7]==12 && tlv[8]==15 && !memcmp(tlv+13,p.label,12));
    assert(tlv[25]==0xc0 && tlv[26]==7);
    struct awdl_service_details details;
    assert(awdl_details(dns,n,&p,&details));
    assert(details.srv_len && details.txt_len && details.ttl==120);
    assert(ptr_u16(details.srv+4)==8770);
    assert(details.srv[details.srv_len-2]==0xc0 && details.srv[details.srv_len-1]==12);
    uint8_t full[512]={0};
    unsigned z=awdl_detail_tlv(full,sizeof(full),details.instance,33,details.srv,details.srv_len);
    assert(z==13+12+details.srv_len && (unsigned)full[1]+3==z && full[20]==33);
    assert(!memcmp(full+25,details.srv,details.srv_len));
    assert(!awdl_detail_tlv(full,z-1,details.instance,33,details.srv,details.srv_len));
    for(size_t k=0;k<n;k++)(void)awdl_details(dns,k,&p,&details);
    p.ttl=0;assert(!awdl_append_ptr(tlv,sizeof(tlv),&p));
    for(size_t k=0;k<n;k++)(void)awdl_extract_ptr(dns,k,&p);
    dns[12]=0xc0;dns[13]=12;assert(!awdl_extract_ptr(dns,n,&p));
    return 0;
}
