#include <string.h>
#include <assert.h>
#include <stdio.h>
#include "service_ptr.h"
#include "service_details.h"
#include "service_registration.h"
int main(int argc,char **argv){
    assert(argc==2);
    uint8_t d[2048];FILE *f=fopen(argv[1],"rb");assert(f);size_t n=fread(d,1,sizeof(d),f);fclose(f);
    struct awdl_local_ptr p={0};struct awdl_service_details details={0};uint64_t end=0;
    service_update(d,n,&p,&details,1,&end);
    assert(end==4500000001ull && details.srv_len && details.txt_len);
    uint8_t old[256];memcpy(old,details.txt,details.txt_len);
    uint8_t one[128]={0,0,0x84,0,0,0,0,1};size_t at=12;
    one[at++]=p.length;memcpy(one+at,p.label,p.length);at+=p.length;
    const uint8_t suffix[]="\010_airdrop\004_tcp\005local";
    memcpy(one+at,suffix,sizeof(suffix));at+=sizeof(suffix);
    const uint8_t txt[]={0,16,0x80,1,0,0,0,120,0,2,1,'x'};
    memcpy(one+at,txt,sizeof(txt));
    service_update(one,at+sizeof(txt),&p,&details,200000001,&end);
    assert(details.txt_len==2 && details.txt[1]=='x' && details.srv_len);
    one[at+7]=0;service_update(one,at+sizeof(txt),&p,&details,200000002,&end);
    assert(details.txt_len==0 && details.srv_len && end==4500000001ull);
    return 0;
}
