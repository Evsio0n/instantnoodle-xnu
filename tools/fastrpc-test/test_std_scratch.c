#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../xnu/files/bsd/dev/slpi_apps_std.h"
#include "../../xnu/files/bsd/dev/slpi_std_scratch.h"
static struct ssc_scratch fs;
static struct ssc_output out;
static struct ssc_buf strb(const char *s){return (struct ssc_buf){(const uint8_t*)s,(unsigned)strlen(s)+1};}
static unsigned call(uint32_t sc,uint32_t *words,unsigned nwords,struct ssc_buf x,
                     struct ssc_buf y,unsigned o0,unsigned o1,bool owned)
{
    uint8_t raw[640]={0};
    struct ssc_buf args[]={{(uint8_t*)words,nwords*4},x,y};
    unsigned off=0,ni=(sc>>16)&255,no=(sc>>8)&255;
    assert(ni<=3&&no<=2);
    for(unsigned i=0;i<ni;i++){
        unsigned n=args[i].n;ssc_put(raw+off,n);off+=4;
        if(n){off=(off+7)&~7u;assert(n<=sizeof(raw)-off);
            memcpy(raw+off,args[i].p,n);off+=n;}
    }
    if(no){ssc_put(raw+off,o0);off+=4;}
    if(no==2){ssc_put(raw+off,o1);off+=4;}
    struct ssc_inv a={0};
    assert(ssc_unpack(sc,raw,off,&a));out.used=0;
    unsigned result=0xdead;
    bool hit=ssc_scratch_try(&fs,sc,&a,&out,&result);
    assert(hit==owned);return result;
}
#define EMPTY ((struct ssc_buf){NULL,0})
static unsigned statp(const char *path,bool owned){struct ssc_buf p=strb(path);uint32_t w[]={31,p.n};return call(0x1f020100,w,2,p,EMPTY,96,0,owned);}
static unsigned openp(const char *path,const char *mode,uint32_t *fd){
    struct ssc_buf p=strb(path),m=strb(mode);uint32_t w[]={p.n,m.n};
    unsigned e=call(0x30100,w,2,p,m,4,0,true);if(!e)*fd=ssc_word(out.bytes+8);return e;
}
static unsigned fdop(uint32_t sc,uint32_t fd){return call(sc,&fd,1,EMPTY,EMPTY,0,0,true);}
static unsigned writefd(uint32_t fd,const char *s,unsigned n){uint32_t w[]={fd,n};return call(0x05020100,w,2,(struct ssc_buf){(const uint8_t*)s,n},EMPTY,8,0,true);}
static unsigned readfd(uint32_t fd,unsigned n){uint32_t w[]={fd,n};return call(0x04010200,w,2,EMPTY,EMPTY,8,n,true);}
static unsigned seekfd(uint32_t fd,int pos,int whence){uint32_t w[]={fd,(uint32_t)pos,(uint32_t)whence};return call(0x09010000,w,3,EMPTY,EMPTY,0,0,true);}
static unsigned truncfd(uint32_t fd,unsigned n){uint32_t w[]={32,fd,n};return call(0x1f010000,w,3,EMPTY,EMPTY,0,0,true);}
static unsigned renamep(const char *a,const char *b,bool owned){struct ssc_buf x=strb(a),y=strb(b);uint32_t w[]={33,x.n,y.n};return call(0x1f030000,w,3,x,y,0,0,owned);}
static unsigned remove_at(const char *path,bool dir){struct ssc_buf p=strb(path);uint32_t w[]={p.n};return call(dir?0x1e020000:0x18020000,w,1,p,EMPTY,0,0,true);}
static unsigned mkdir_at(const char *path){struct ssc_buf p=strb(path);uint32_t w[]={p.n,0700};return call(0x1d020000,w,2,p,EMPTY,0,0,true);}
static uint64_t opendir_at(const char *path){struct ssc_buf p=strb(path);uint32_t w[]={p.n};assert(!call(SSC_OPENDIR,w,1,p,EMPTY,8,0,true));uint64_t t;memcpy(&t,out.bytes+8,8);return t;}
static unsigned dirop(uint32_t sc,uint64_t token){uint32_t w[2];memcpy(w,&token,8);return call(sc,w,2,EMPTY,EMPTY,sc==SSC_READDIR?264:0,0,true);}
int main(void){
    const char *root=SSC_SCR_ROOT "/registry", *tmp=SSC_SCR_ROOT "/registry/../temp.json";
    const char *version=SSC_SCR_ROOT "/sns_reg_version", *group=SSC_SCR_ROOT "/registry/als.group";
    assert(!statp(root,true)&&ssc_word(out.bytes+8+24)==0040700&&ssc_word(out.bytes+8+40)==0);
    assert(statp(SSC_SCR_ROOT "/../keystore",true)==SSC_BADARG);
    assert(statp(SSC_SCR_ROOT "x/a",false)==0xdead);
    assert(statp(tmp,true)==SCR_NOENT);
    uint32_t f,g,old;assert(!openp(tmp,"wb+",&f)&&f);
    assert(!writefd(f,"version=10",10)&&ssc_word(out.bytes+8)==10);
    assert(!statp(SSC_SCR_ROOT "/temp.json",true)&&ssc_word(out.bytes+8+40)==10);
    assert(!renamep(tmp,version,true)&&statp(tmp,true)==SCR_NOENT);
    assert(!seekfd(f,0,0)&&!readfd(f,16)&&ssc_word(out.bytes+8)==10&&ssc_word(out.bytes+12)==1&&!memcmp(out.bytes+24,"version=10",10));
    assert(!truncfd(f,3)&&!seekfd(f,7,0)&&!writefd(f,"!",1));
    assert(!seekfd(f,0,0)&&!readfd(f,8)&&!memcmp(out.bytes+24,"ver\0\0\0\0!",8));
    assert(!openp(version,"rb",&old));
    assert(!openp(tmp,"wb+",&g)&&!writefd(g,"new",3));
    assert(!renamep(tmp,version,true));
    assert(!readfd(old,8)&&!memcmp(out.bytes+24,"ver\0\0\0\0!",8));
    assert(!fdop(0x03010000,f)&&!fdop(0x03010000,old));
    assert(!remove_at(version,false)&&statp(version,true)==SCR_NOENT);
    assert(!seekfd(g,0,0)&&!readfd(g,3)&&!memcmp(out.bytes+24,"new",3));
    assert(!fdop(0x03010000,g)&&fdop(0x03010000,g)==SSC_BADHANDLE&&fs.free_count==SCR_BLOCKS);
    assert(!openp(group,"w+b",&f));
    assert(!writefd(f,"x",1));
    assert(!truncfd(f,SCR_BLOCKS*SCR_UNIT));
    assert(!openp(tmp,"wb",&g)&&writefd(g,"y",1)==SCR_NOSPACE);
    assert(!statp(tmp,true)&&ssc_word(out.bytes+8+40)==0);
    assert(!truncfd(f,0)&&!writefd(g,"y",1));
    assert(!fdop(0x03010000,g)&&!remove_at(tmp,false)&&!fdop(0x03010000,f));
    assert(renamep(group,"/outside/output",true)==SSC_BADARG);
    assert(renamep("/outside/source",group,true)==SSC_BADARG);
    assert(renamep("/vendor/x","/vendor/y",false)==0xdead);
    assert(mkdir_at(root)==SCR_EXIST && mkdir_at(SSC_SCR_ROOT)==SCR_EXIST);
    assert(!openp(group,"ab+",&g)&&!writefd(g,"ab",2)&&!seekfd(g,0,0)&&!writefd(g,"c",1));
    assert(truncfd(g,UINT32_MAX)==SSC_BADARG && !seekfd(g,0,0) && !readfd(g,3));
    assert(!memcmp(out.bytes+24,"abc",3) && !fdop(0x03010000,g));
    const char *sub=SSC_SCR_ROOT "/registry/sub";assert(!mkdir_at(sub));
    uint64_t dir=opendir_at(root);bool found_group=false,found_sub=false;
    for(unsigned i=0;i<6;i++){
        assert(!dirop(SSC_READDIR,dir));
        if(ssc_word(out.bytes+268))break;
        const char *name=(char*)out.bytes+12;
        found_group|=!strcmp(name,"als.group");found_sub|=!strcmp(name,"sub");
    }
    assert(found_group&&found_sub&&!dirop(SSC_CLOSEDIR,dir)&&dirop(SSC_READDIR,dir)==SSC_BADHANDLE);
    assert(renamep(root,SSC_SCR_ROOT "/registry/sub/cycle",true)==SSC_BADARG);
    assert(remove_at(root,true)==SCR_NOTEMPTY);
    assert(!remove_at(sub,true)&&!remove_at(group,false)&&!remove_at(root,true)&&fs.free_count==SCR_BLOCKS);
    assert(statp(root,true)==SCR_NOENT);
    assert(!mkdir_at(root));
    memset(&fs,0,sizeof(fs));assert(statp(tmp,true)==SCR_NOENT&&statp(version,true)==SCR_NOENT);
    (void)&ssc_dispatch; puts("PASS registry scratch wire/stat/open/read/write/seek/trunc/rename/unlink/directories/capacity/isolation/reset.");
}
