/* Exact-path single-file acquisition. The caller supplies one partition-relative
 * 4KiB block per request. Cache bounds include all geometry, inode, directory,
 * extent-index and file reads. No block write API or external path argument. */
#ifndef CALI_EXT4_H
#define CALI_EXT4_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

enum { CE_BS=4096, CE_MAX_READS=384, CE_MAX_DIR=128, CE_MAX_FILE=65536 };
typedef int (*ce_read_block)(void *,uint32_t,uint8_t *);
struct ce_slot { uint32_t number; uint8_t data[CE_BS]; };
struct ce_fs {
    ce_read_block read; void *source;
    struct ce_slot *cache;
    unsigned count, blocks, inodes, ipg, isize, dsize, inc, ro, seed;
    uint8_t uuid[16]; const char *why;
};
struct ce_inode {
    const uint8_t *node;
    unsigned bytes, flags, seed, ino, nblocks;
    uint32_t phys[CE_MAX_DIR], indexes[CE_MAX_READS], nindex;
};
static uint16_t ce16(const uint8_t *p) { return p[0]|(uint16_t)p[1]<<8; }
static uint32_t ce32(const uint8_t *p) { return ce16(p)|(uint32_t)ce16(p+2)<<16; }
static void ce_put(uint8_t *p,uint32_t n) { for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i)); }
static uint32_t ce_crc(uint32_t v,const uint8_t *p,unsigned len,uint32_t poly) {
    while(len--) {v^=*p++;for(unsigned i=0;i<8;i++)v=(v>>1)^((0u-(v&1))&poly);}
    return v;
}
static uint32_t ce_crc32(uint32_t v,const uint8_t *p,unsigned n) {return ce_crc(v,p,n,0x82f63b78u);}
#define CE_REQUIRE(fs, expr, msg) do {if (!(expr)){(fs)->why=msg;return false;}} while(0)
static const uint8_t *ce_block(struct ce_fs *fs,uint32_t n) {
    if(n>=fs->blocks){fs->why="block outside partition/volume";return NULL;}
    for(unsigned i=0;i<fs->count;i++)if(fs->cache[i].number==n)return fs->cache[i].data;
    if(fs->count==CE_MAX_READS){fs->why="read budget";return NULL;}
    struct ce_slot *s=&fs->cache[fs->count];
    if(!fs->read(fs->source,n,s->data)){fs->why="incomplete device block read";return NULL;}
    s->number=n;fs->count++;return s->data;
}
static bool ce_super(struct ce_fs *fs){
    const uint8_t *b=ce_block(fs,0);if(!b)return false;b+=1024;
    CE_REQUIRE(fs,ce16(b+56)==0xef53&&ce32(b+24)==2&&ce32(b+20)==0&&ce16(b+58)==1&&ce32(b+76)==1,"not clean 4KiB EXT4");
    fs->inc=ce32(b+96);fs->ro=ce32(b+100);
    CE_REQUIRE(fs,!(fs->inc & ~(2u|64u|128u|512u|0x2000u))&&(fs->inc&66)==66,"unsupported EXT4 incompatible features");
    CE_REQUIRE(fs,!(fs->ro & ~(1u|2u|4u|8u|16u|32u|64u|128u|256u|1024u|4096u|8192u)),"unsupported EXT4 layout");
    memcpy(fs->uuid,b+104,16);fs->seed=fs->inc&0x2000?ce32(b+0x270):ce_crc32(~0u,fs->uuid,16);
    CE_REQUIRE(fs,!(fs->ro&1024)||(b[0x175]==1&&ce_crc32(~0u,b,1020)==ce32(b+1020)),"superblock checksum");
    unsigned count=ce32(b+4),bpg=ce32(b+32);
    CE_REQUIRE(fs,count>=16&&count<=fs->blocks&&(!(fs->inc&128)||!ce32(b+336)),"filesystem block count");
    fs->blocks=count;fs->ipg=ce32(b+40);fs->inodes=ce32(b);fs->isize=ce16(b+88);fs->dsize=fs->inc&128?ce16(b+254):32;
    CE_REQUIRE(fs,bpg>0&&bpg<=32768&&fs->ipg>0&&fs->ipg<=32768&&fs->inodes>=2&&
        fs->inodes<=((uint64_t)count+bpg-1)/bpg*fs->ipg&&(fs->isize==128||fs->isize==256||fs->isize==512)&&
        (fs->dsize==32||fs->dsize==64),"group/inode geometry");
    return true;
}
static bool ce_get_inode(struct ce_fs *fs,unsigned ino,unsigned typ,struct ce_inode *out){
    CE_REQUIRE(fs,ino&&ino<=fs->inodes,"inode outside volume");
    memset(out,0,sizeof(*out));
    unsigned group=(ino-1)/fs->ipg,ix=(ino-1)%fs->ipg;
    uint64_t byte=(uint64_t)group*fs->dsize;
    const uint8_t *gd=ce_block(fs,(unsigned)(1+byte/CE_BS));if(!gd)return false;gd+=byte%CE_BS;
    uint8_t gr[4],desc[64];ce_put(gr,group);memcpy(desc,gd,fs->dsize);
    unsigned sum=ce16(desc+30);desc[30]=desc[31]=0;
    if(fs->ro&1024){
        CE_REQUIRE(fs,(ce_crc32(ce_crc32(fs->seed,gr,4),desc,fs->dsize)&65535)==sum,"group checksum");
    }else if(fs->ro&16){
        unsigned v=ce_crc(0xffff,fs->uuid,16,0xa001);v=ce_crc(v,gr,4,0xa001);
        v=ce_crc(v,desc,30,0xa001);v=ce_crc(v,desc+32,fs->dsize-32,0xa001);
        CE_REQUIRE(fs,v==sum,"legacy group checksum");
    }
    uint64_t base=ce32(gd+8),offset=(uint64_t)ix*fs->isize;
    CE_REQUIRE(fs,base&&(!(fs->dsize==64)||!ce32(gd+40))&&base+offset/CE_BS<fs->blocks,"inode table bounds");
    const uint8_t *b=ce_block(fs,(unsigned)(base+offset/CE_BS));if(!b)return false;b+=offset%CE_BS;
    CE_REQUIRE(fs,(ce16(b)>>12)==typ && ce16(b+26)>0&&!ce32(b+20),"unexpected inode type/state");
    out->ino=ino;out->bytes=ce32(b+4);out->flags=ce32(b+32);out->node=b+40;
    CE_REQUIRE(fs,(out->flags&0x80000)&&!(out->flags&(4u|0x800u|0x10000000u|0x100000u))&&!ce32(b+108),"unsupported inode flags/size");
    CE_REQUIRE(fs,typ==4?(out->bytes>0&&out->bytes<=CE_MAX_DIR*CE_BS&&out->bytes%CE_BS==0):
        (out->bytes>0&&out->bytes<=CE_MAX_FILE&&ce16(b+26)==1),"file/directory cap or hardlink");
    uint8_t in[4];ce_put(in,ino);
    out->seed=ce_crc32(ce_crc32(fs->seed,in,4),b+100,4);
    if(fs->ro&1024){
        uint8_t copy[512];memcpy(copy,b,fs->isize);
        unsigned expected=ce16(copy+124),mask=0xffff;copy[124]=copy[125]=0;
        if(fs->isize>=132&&ce16(b+128)>=4){expected|=(uint32_t)ce16(copy+130)<<16;copy[130]=copy[131]=0;mask=~0u;}
        CE_REQUIRE(fs,(ce_crc32(out->seed,copy,fs->isize)&mask)==expected,"inode checksum");
    }
    out->nblocks=(out->bytes+CE_BS-1)/CE_BS;
    return true;
}
static bool ce_extents(struct ce_fs *fs,struct ce_inode *in,const uint8_t *h,unsigned bytes,
                       unsigned depth,uint64_t lo,uint64_t hi){
    unsigned count=ce16(h+2),cap=ce16(h+4);
    CE_REQUIRE(fs,ce16(h)==0xf30a&&ce16(h+6)==depth&&depth<=4&&count>0&&count<=cap&&cap<=(bytes-12)/12,"extent header");
    if((fs->ro&1024)&&bytes==CE_BS){
        unsigned end=12+12*cap;
        CE_REQUIRE(fs,end+4<=bytes&&ce_crc32(in->seed,h,end)==ce32(h+end),"extent block checksum");
    }
    for(unsigned i=0;i<count;i++){
        const uint8_t *e=h+12+12*i;
        uint64_t start=ce32(e),end=i+1<count?ce32(e+12):hi;
        CE_REQUIRE(fs,start>=lo&&start<end&&end<=hi,"extent order");
        if(depth){
            uint32_t child=ce32(e+4);
            CE_REQUIRE(fs,child&&!ce16(e+8)&&in->nindex<CE_MAX_READS,"extent node bounds");
            for(unsigned n=0;n<in->nindex;n++)CE_REQUIRE(fs,in->indexes[n]!=child,"extent cycle");
            in->indexes[in->nindex++]=child;
            const uint8_t *sub=ce_block(fs,child);
            if(!sub||!ce_extents(fs,in,sub,CE_BS,depth-1,start,end))return false;
        }else{
            unsigned length=ce16(e+4),p=ce32(e+8);
            CE_REQUIRE(fs,length&&length<=32768&&!ce16(e+6)&&start+length<=end&&p&&(uint64_t)p+length<=fs->blocks,"extent range");
            for(unsigned j=0;j<length&&start+j<in->nblocks;j++)in->phys[start+j]=p+j;
        }
    }
    return true;
}
static bool ce_map(struct ce_fs *fs,struct ce_inode *in){
    if(!ce_extents(fs,in,in->node,60,ce16(in->node+6),0,UINT64_C(1)<<32))return false;
    for(unsigned i=0;i<in->nblocks;i++){
        CE_REQUIRE(fs,in->phys[i],"sparse file");
        for(unsigned j=0;j<i;j++)CE_REQUIRE(fs,in->phys[j]!=in->phys[i],"alias extent");
        for(unsigned j=0;j<in->nindex;j++)CE_REQUIRE(fs,in->indexes[j]!=in->phys[i],"metadata/data overlap");
    }
    return true;
}
static bool ce_lookup(struct ce_fs *fs,unsigned parent,const char *name,unsigned type,unsigned *result){
    struct ce_inode in;
    if(!ce_get_inode(fs,parent,4,&in)||!ce_map(fs,&in))return false;
    *result=0;
    for(unsigned i=0;i<in.nblocks;i++){
        const uint8_t *b=ce_block(fs,in.phys[i]);if(!b)return false;unsigned end=CE_BS;
        if(fs->ro&1024){
            if(!ce32(b+end-12)&&ce16(b+end-8)==12&&ce16(b+end-6)==0xde00){
                end-=12;CE_REQUIRE(fs,ce_crc32(in.seed,b,end)==ce32(b+CE_BS-4),"directory checksum");
            }else{
                CE_REQUIRE(fs,in.flags&0x1000,"directory checksum tail missing");
                unsigned off=i==0?32:8;
                CE_REQUIRE(fs,i==0?(ce16(b+4)==12&&ce16(b+16)==CE_BS-12&&!ce32(b+24)&&b[29]==8):
                    (!ce32(b)&&ce16(b+4)==CE_BS&&!ce16(b+6)),"Htree node");
                unsigned limit=ce16(b+off),count=ce16(b+off+2),tail=off+limit*8;
                CE_REQUIRE(fs,count&&count<=limit&&tail+8<=CE_BS,"Htree bounds");
                uint32_t v=ce_crc32(in.seed,b,off+count*8);v=ce_crc32(v,b+tail,4);
                uint8_t zero[4]={0};CE_REQUIRE(fs,ce_crc32(v,zero,4)==ce32(b+tail+4),"Htree checksum");
                continue;
            }
        }
        for(unsigned at=0;at<end;){
            CE_REQUIRE(fs,end-at>=8,"short directory record");
            unsigned len=ce16(b+at+4),nm=b[at+6],kind=b[at+7],child=ce32(b+at);
            CE_REQUIRE(fs,len>=8&&len%4==0&&len<=end-at&&nm<=len-8,"directory record range");
            if(child&&nm==strlen(name)&&!memcmp(b+at+8,name,nm)){
                CE_REQUIRE(fs,(!kind||kind==type)&&!*result,"wrong type or duplicate");
                *result=child;
            }
            at+=len;
        }
    }
    CE_REQUIRE(fs,*result!=0,"exact target entry absent");
    return true;
}
static bool ce_acquire(struct ce_fs *fs,uint8_t *data,unsigned *size){
    unsigned parent,ino;struct ce_inode in;
    if(!ce_super(fs)||!ce_lookup(fs,2,"engineermode",2,&parent)||!ce_lookup(fs,parent,"cali_coe",1,&ino)||
       !ce_get_inode(fs,ino,8,&in)||!ce_map(fs,&in))return false;
    for(unsigned i=0;i<in.nblocks;i++){
        const uint8_t *b=ce_block(fs,in.phys[i]);if(!b)return false;
        unsigned bytes=in.bytes-i*CE_BS;if(bytes>CE_BS)bytes=CE_BS;
        memcpy(data+(size_t)i*CE_BS,b,bytes);
    }
    *size=in.bytes;return true;
}
#endif
