#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../xnu/files/bsd/dev/slpi_apps_std.h"
static unsigned blob(uint8_t *b,struct ssc_buf *inputs,unsigned ni,unsigned *sizes,unsigned no)
{
    unsigned at=0;
    for(unsigned i=0;i<ni;i++){
        ssc_put(b+at,inputs[i].n);at+=4;
        if(inputs[i].n){at=(at+7)&~7u;memcpy(b+at,inputs[i].p,inputs[i].n);at+=inputs[i].n;}
    }
    for(unsigned i=0;i<no;i++){ssc_put(b+at,sizes[i]);at+=4;}
    return at;
}
int main(void)
{
    static uint8_t pack[SSC_PACK_SIZE];
    struct ssc_session s={.pack=pack,.size=sizeof(pack)};
    struct ssc_output out;
    uint8_t b[640]={0};uint32_t words[]={9,0};unsigned lens[]={8,0};
    struct ssc_buf args[]={{(uint8_t*)words,8},{(const uint8_t*)"apps_std",9},{(const uint8_t*)"rb",3}};
    unsigned n=blob(b,args,2,lens,2);
    assert(n==41&&!ssc_dispatch(&s,0,0x20200,b,n,&out)&&out.used==20);
    assert(ssc_word(out.bytes+8)==SSC_MODULE);
    const struct ssc_entry *e=&ssc_entries[0];
    memcpy(pack+e->offset,"test",4);
    args[1].p=(const uint8_t*)e->path;args[1].n=words[0]=strlen(e->path)+1;words[1]=3;lens[0]=4;
    n=blob(b,args,3,lens,1);
    assert(!ssc_dispatch(&s,SSC_MODULE,0x30100,b,n,&out));
    uint32_t token=ssc_word(out.bytes+8);assert(token);
    assert(ssc_dispatch(&s,SSC_MODULE,0x30100,b,n-1,&out)==SSC_BADARG);
    words[0]=token;words[1]=4;lens[0]=8;lens[1]=4;
    n=blob(b,args,1,lens,2);assert(!ssc_dispatch(&s,SSC_MODULE,0x04010200,b,n,&out));
    assert(ssc_word(out.bytes+8)==4&&!memcmp(out.bytes+24,"test",4));
    args[0].n=4;n=blob(b,args,1,NULL,0);
    assert(!ssc_dispatch(&s,SSC_MODULE,0x03010000,b,n,&out));
    assert(ssc_dispatch(&s,SSC_MODULE,0x03010000,b,n,&out)==SSC_BADHANDLE);
    const char *config="/vendor/etc/sensors/sns_reg_config";
    uint32_t ext[]={31,(uint32_t)strlen(config)+1};args[0]=(struct ssc_buf){(uint8_t*)ext,8};
    args[1]=(struct ssc_buf){(const uint8_t*)config,ext[1]};lens[0]=96;
    n=blob(b,args,2,lens,1);assert(!ssc_dispatch(&s,SSC_MODULE,0x1f020100,b,n,&out));
    assert(ssc_word(out.bytes+8+24)==0100444 && ssc_word(out.bytes+8+40)>0);
    uint32_t lengths[]={18,2,ext[1],3};
    struct ssc_buf env[]={{(uint8_t*)lengths,16},{(uint8_t*)"ADSP_LIBRARY_PATH",18},
        {(uint8_t*)";",2},args[1],{(uint8_t*)"rb",3}};
    lens[0]=4;n=blob(b,env,5,lens,1);
    assert(!ssc_dispatch(&s,SSC_MODULE,0x13050100,b,n,&out));
    puts("PASS apps_std open/read/close, native reply packing, stale handle and truncated request");
}
