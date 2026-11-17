#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define LCK_MTX_DECLARE(n,g) int n
static void lck_mtx_lock(int *p){(void)p;}
static void lck_mtx_unlock(int *p){(void)p;}
#include "bonjour_store.inc"
#include "wire_checksum.h"
int main(void)
{
    /* Actual 20F66 PTR query bytes; no device identity or fabricated answer. */
    const uint8_t query[]={1,18,0,0,0,0,0,8,'_','a','i','r','d','r','o','p',0xc0,12,0,12,1};
    const uint8_t question[]={8,'_','a','i','r','d','r','o','p',4,'_','t','c','p',5,'l','o','c','a','l',0,0,12,0,1};
    uint8_t request[41]={1},packed[512],wire[512],frame[512],source[16]={0xfe,0x80},mac[6]={2,0,0,0,0,1};
    request[16]=sizeof(query);memcpy(request+20,query,sizeof(query));source[15]=1;
    assert(awif_service_request(request,sizeof(request),7)==0);
    unsigned n=lx_os_awdl_query_bytes(7,packed,sizeof(packed));
    assert(n==2+sizeof(question) && hk_le16(packed)==sizeof(question) && !memcmp(packed+2,question,sizeof(question)));
    assert(lx_os_awdl_service_bytes(7,wire,sizeof(wire))==0); /* queries aren't answer IEs */
    /* Local TXT encoding fixture only: existing publication must coexist
     * without query state leaking into the advertised response list. */
    const uint8_t txt[]={2,18,0,10,0,4,'t','e','s','t',0xc0,12,0,16,1,2,0,0,0,1,'x'};
    uint8_t publication[20+sizeof(txt)]={1};publication[16]=sizeof(txt);memcpy(publication+20,txt,sizeof(txt));
    assert(awif_service_request(publication,sizeof(publication),7)==0);
    assert(lx_os_awdl_service_bytes(7,wire,sizeof(wire))==19 && wire[0]==2);
    assert(lx_os_awdl_query_bytes(7,packed,sizeof(packed))==n);
    assert(!lx_os_awdl_query_bytes(8,packed,sizeof(packed)));
    assert(awif_service_request(request,sizeof(request),7)==0);
    assert(lx_os_awdl_query_bytes(7,packed,sizeof(packed))==n); /* duplicate subscription */
    memset(wire,0x5a,sizeof(wire));assert(!lx_os_awdl_query_bytes(7,wire,1));assert(wire[0]==0x5a);
    for(unsigned length=0;length<sizeof(request);length++)assert(awif_service_request(request,length,7)==EINVAL);
    request[23]=1;assert(awif_service_request(request,sizeof(request),7)==EINVAL);request[23]=0;
    request[40]=2;assert(awif_service_request(request,sizeof(request),7)==EINVAL);request[40]=1;
    unsigned bytes=hk_query_frame(frame,sizeof(frame),mac,source,packed+2,hk_le16(packed));
    assert(bytes==14+40+8+12+sizeof(question));assert(frame[14+7]==255);
    assert(frame[54]==0x14 && frame[55]==0xe9 && frame[56]==0x14 && frame[57]==0xe9);
    assert(frame[67]==1 && !memcmp(frame+74,question,sizeof(question)));
    assert(awdl_wire_checksum(frame+14,bytes-14)==1);
    frame[bytes-1]^=1;assert(awdl_wire_checksum(frame+14,bytes-14)==0);
    assert(!hk_query_frame(frame,bytes-1,mac,source,packed+2,hk_le16(packed)));
    request[10]=1;assert(awif_service_request(request,sizeof(request),7)==0);
    assert(!lx_os_awdl_query_bytes(7,packed,sizeof(packed)));
    assert(lx_os_awdl_service_bytes(7,wire,sizeof(wire))==19);
    request[10]=0;assert(awif_service_request(request,sizeof(request),8)==0);
    assert(!lx_os_awdl_query_bytes(7,packed,sizeof(packed)));
    uint8_t clear[20]={1};clear[10]=1;assert(awif_service_request(clear,sizeof(clear),8)==0);
    assert(!lx_os_awdl_query_bytes(8,packed,sizeof(packed)));
    puts("PASS actual native query: decoding, DNS expansion, lease/add/delete/clear, duplicate handling, frame/checksum and bounds");
    return 0;
}
