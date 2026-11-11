/* Matching IN2010 DSP reads OPlus board description from generic SMEM item
 * 130. The 13.1 OP8 OEM ABI uses 3 sources of 184 bytes per physical type.
 * Only IDs and four board-descriptor bytes per source are observable here. */
#ifndef SLPI_OEM130_RO_H
#define SLPI_OEM130_RO_H
#include "slpi_smem_ro.h"
enum { SO130_BYTES=6792, SO130_TYPES=3, SO130_SOURCES=3,
       SO130_VECTOR_BYTES=556, SO130_SOURCE_BYTES=184 };
static const unsigned so130_types[SO130_TYPES]={0,3,4};
struct so130_type {uint32_t id;uint8_t slot[SO130_SOURCES][4];};
struct so130_observation {unsigned checked;struct so130_type type[SO130_TYPES];};

static int so130_read_headers(slpi_read_fn read,void *ctx,const struct slpi_item *item,
                             struct so130_observation *o)
{
 if(item->bytes!=SO130_BYTES||item->offset>SLPI_SMEM_TOC-SO130_BYTES)return 0;
 for(unsigned i=0;i<SO130_TYPES;i++){
  uint32_t base=item->offset+4+so130_types[i]*SO130_VECTOR_BYTES;
  uint8_t id[4];
  if(!slpi_read(read,ctx,base,id,sizeof(id)))return 0;
  o->type[i].id=slpi_u32(id);
  for(unsigned j=0;j<SO130_SOURCES;j++){
   if(!slpi_read(read,ctx,base+4+j*SO130_SOURCE_BYTES,o->type[i].slot[j],4))return 0;
  }
 }
 return 1;
}
static const char *so130_observe(slpi_read_fn read,void *ctx,const struct slpi_report *r,
                                  struct so130_observation *out)
{
 *out=(struct so130_observation){0};
 if(r->global_partitions!=1)return "global-partition-unresolved";
 if(r->global130_count>1)return "duplicate";
 struct slpi_report final;
 struct so130_observation first={0},second={0};
 if(r->global130_count==1 && r->global130.bytes==SO130_BYTES){
  if(!so130_read_headers(read,ctx,&r->global130,&first)||
     !so130_read_headers(read,ctx,&r->global130,&second))return "read-error";
  for(unsigned i=0;i<SO130_TYPES;i++){
   if(first.type[i].id!=second.type[i].id)return "changed";
   for(unsigned j=0;j<SO130_SOURCES;j++)for(unsigned k=0;k<4;k++)
    if(first.type[i].slot[j][k]!=second.type[i].slot[j][k])return "changed";
  }
 }
 if(slpi_observe(read,ctx,&final)||final.global_partitions!=1||
    final.global130_count!=r->global130_count || final.global130.offset!=r->global130.offset ||
    final.global130.bytes!=r->global130.bytes)return "metadata-changed";
 if(!r->global130_count)return "absent";
 if(r->global130.bytes!=SO130_BYTES)return "unknown-size";
 first.checked=1;*out=first;return "observed";
}
#endif
