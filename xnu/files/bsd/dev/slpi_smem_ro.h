/* Pure bounded SMEM metadata reader. No write callback or device API. */
#ifndef SLPI_SMEM_RO_H
#define SLPI_SMEM_RO_H
#define SLPI_SMEM_BYTES 0x200000u
#define SLPI_SMEM_TOC (SLPI_SMEM_BYTES-0x1000u)
struct slpi_item { uint32_t id,offset,bytes; uint16_t host0,host1; };
struct slpi_report { uint32_t remote_partition,remote_bytes,found;struct slpi_item items[6];
 uint32_t global_partitions,global130_count;struct slpi_item global130; };
typedef int (*slpi_read_fn)(void *,uint32_t,void *,uint32_t);
static uint16_t slpi_u16(const uint8_t *p){return (uint16_t)p[0]|(uint16_t)p[1]<<8;}
static uint32_t slpi_u32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int slpi_read(slpi_read_fn rd,void *ctx,uint32_t off,void *p,uint32_t n){return off<=SLPI_SMEM_BYTES && n<=SLPI_SMEM_BYTES-off && rd(ctx,off,p,n);}
static int slpi_magic(const uint8_t *p,const char *m){return p[0]==m[0]&&p[1]==m[1]&&p[2]==m[2]&&p[3]==m[3];}
static void slpi_keep(struct slpi_report *r,uint16_t a,uint16_t b,uint32_t id,uint32_t off,uint32_t n){
 /* SMEM 130 is AP board data in the generic partition. It belongs outside
  * found, which the owned boot preflight uses for remote transport items. */
 if(id==130 && a==0xfffe && b==0xfffe){
  r->global130_count++; r->global130=(struct slpi_item){id,off,n,a,b};return;
 }
 static const uint16_t wanted[]={430,481,424,478,479,480};
 for(unsigned k=0;k<6;k++)if(id==wanted[k]){
  /* Prefer remote0/3 partition. Global fallback only fills missing items. */
  if(r->items[k].bytes==0){r->items[k]=(struct slpi_item){id,off,n,a,b};r->found++;}return;
 }
}
static const char *slpi_partition_scan(slpi_read_fn rd,void *ctx,const uint8_t *te,struct slpi_report *r){
 uint8_t h[32],again[32],e[16];uint32_t off=slpi_u32(te),size=slpi_u32(te+4),cl=slpi_u32(te+16),at,steps=0;
 uint16_t a=slpi_u16(te+12),b=slpi_u16(te+14);
 if(!off || size<32 || off>SLPI_SMEM_TOC || size>SLPI_SMEM_TOC-off || !slpi_read(rd,ctx,off,h,32))return "partition range/read";
 uint32_t lo=slpi_u32(h+12),hi=slpi_u32(h+16);
 if(!slpi_magic(h,"$PRT")||slpi_u32(h+8)!=size||slpi_u16(h+4)!=a||slpi_u16(h+6)!=b||lo<32||lo>hi||hi>size)return "partition geometry";
 if(a!=0xfffe){r->remote_partition=off;r->remote_bytes=size;}else r->global_partitions++;
 for(at=32;at<lo;){
  if(++steps>512 || lo-at<16 || !slpi_read(rd,ctx,off+at,e,16))return "uncached entry bounds";
  uint32_t n=slpi_u32(e+4),pd=slpi_u16(e+8),ph=slpi_u16(e+10);
  if(slpi_u16(e)!=0xa5a5||pd>n||ph>lo-at-16||n>lo-at-16-ph)return "uncached entry invalid";
  slpi_keep(r,a,b,slpi_u16(e+2),off+at+16+ph,n-pd);at+=16+ph+n;
 }
 if(hi<size){
  if(!cl || cl>4096 || (cl&(cl-1)))return "cached alignment";
  uint32_t back=(16+cl-1)&~(cl-1);
  if(size<back)return "cached range";
  for(at=size-back;at>hi;){
   if(++steps>512 || !slpi_read(rd,ctx,off+at,e,16))return "cached entry bounds";
   uint32_t n=slpi_u32(e+4),pd=slpi_u16(e+8);
   if(slpi_u16(e)!=0xa5a5||pd>n||n>at||at-n<hi)return "cached entry invalid";
   slpi_keep(r,a,b,slpi_u16(e+2),off+at-n,n-pd);
   if(n+back>at)return "cached underflow";at-=n+back;
  }
 }
 if(!slpi_read(rd,ctx,off,again,32))return "partition reread";
 for(unsigned i=0;i<32;i++)if(h[i]!=again[i])return "partition changed during observation";
 return 0;
}
static const char *slpi_observe(slpi_read_fn rd,void *ctx,struct slpi_report *r){
 uint8_t t[32],again[32],entries[64][48];uint32_t n;*r=(struct slpi_report){0};
 if(!slpi_read(rd,ctx,SLPI_SMEM_TOC,t,32)||!slpi_magic(t,"$TOC")||slpi_u32(t+4)!=1)return "no supported SMEM TOC";
 n=slpi_u32(t+8);if(n>64)return "TOC count";
 if(!slpi_read(rd,ctx,SLPI_SMEM_TOC+32,entries,n*48))return "TOC entries";
 for(unsigned pass=0;pass<2;pass++)for(unsigned i=0;i<n;i++){
  uint16_t a=slpi_u16(entries[i]+12),b=slpi_u16(entries[i]+14);
  if((pass==0 && ((a==0&&b==3)||(a==3&&b==0))) || (pass==1&&a==0xfffe&&b==0xfffe)){
   const char *err=slpi_partition_scan(rd,ctx,entries[i],r);if(err)return err;
  }
 }
 if(!slpi_read(rd,ctx,SLPI_SMEM_TOC,again,32))return "TOC reread";
 for(unsigned i=0;i<32;i++)if(t[i]!=again[i])return "TOC changed during observation";
 return 0;
}
#endif
