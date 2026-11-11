/* Optional RAM experiment using the two stock board records for the captured
 * IN2010. A caller must hold the global SMEM lock and own the cold boot.
 * No allocator; no firmware, register, global magic or calibration writes. */
#ifndef SLPI_OEM130_TRIAL_H
#define SLPI_OEM130_TRIAL_H
#include "slpi_oem130_ro.h"
struct so130_word {unsigned offset;uint32_t value;};
#include "slpi_oem130_words.h"
enum { SO130_NWORDS=sizeof(so130_oem_words)/sizeof(so130_oem_words[0]) };
struct so130_trial {uint32_t offset,owned;};
typedef void (*so130_write_fn)(void *,uint32_t,uint32_t);

static int so130_entry(const struct slpi_report *r)
{
 return r->global_partitions==1 && r->global130_count==1 &&
   r->global130.id==130 && r->global130.host0==0xfffe && r->global130.host1==0xfffe &&
   r->global130.bytes==SO130_BYTES && !(r->global130.offset&3) &&
   r->global130.offset>=48 && r->global130.offset<=SLPI_SMEM_TOC-SO130_BYTES;
}
static int so130_read_word(slpi_read_fn rd,void *ctx,uint32_t offset,uint32_t *word)
{
 uint8_t b[4];if(!slpi_read(rd,ctx,offset,b,4))return 0;*word=slpi_u32(b);return 1;
}
static int so130_vector_id(unsigned off){return off==4+3*SO130_VECTOR_BYTES || off==4+4*SO130_VECTOR_BYTES;}
/* Called only before PAS AUTH/RESET or after positive PAS shutdown. Keep an
 * ownership bit on conflict; never overwrite another actor's replacement. */
static int so130_restore_locked(struct so130_trial *t,const struct slpi_report *r,
                                 slpi_read_fn rd,so130_write_fn wr,void *ctx)
{
 if(!t->owned)return 1;
 if(!so130_entry(r)||r->global130.offset!=t->offset)return 0;
 for(unsigned i=0;i<SO130_NWORDS;i++)if(t->owned&(1u<<i)){
  uint32_t v;const struct so130_word *w=&so130_oem_words[i];
  if(!so130_read_word(rd,ctx,t->offset+w->offset,&v)||(v && v!=w->value))return 0;
 }
 /* Withdraw the two vector IDs before retiring their payloads. */
 for(unsigned pass=0;pass<2;pass++)for(unsigned i=0;i<SO130_NWORDS;i++){
  const struct so130_word *w=&so130_oem_words[i];uint32_t bit=1u<<i,v;
  if(!(t->owned&bit)||so130_vector_id(w->offset)!=(pass==0))continue;
  if(!so130_read_word(rd,ctx,t->offset+w->offset,&v))return 0;
  if(v){
   if(v!=w->value)return 0;
   wr(ctx,t->offset+w->offset,0);
   if(!so130_read_word(rd,ctx,t->offset+w->offset,&v)||v)return 0;
  }
  t->owned&=~bit;
 }
 return !t->owned;
}
static int so130_publish_locked(struct so130_trial *t,const struct slpi_report *r,
                                slpi_read_fn rd,so130_write_fn wr,void *ctx)
{
 if(t->owned||!so130_entry(r))return 0;
 /* Changing a vector ID also makes slots 1/2 discoverable. Require both
  * complete physical vectors to be blank, including unmodified neighbors. */
 uint32_t base=r->global130.offset;
 for(unsigned p=0;p<2;p++)for(unsigned off=4+3*SO130_VECTOR_BYTES;off<4+5*SO130_VECTOR_BYTES;off+=4){
  uint32_t v;if(!so130_read_word(rd,ctx,base+off,&v)||v)return 0;
 }
 t->offset=base;
 for(unsigned pass=0;pass<2;pass++)for(unsigned i=0;i<SO130_NWORDS;i++){
  const struct so130_word *w=&so130_oem_words[i];uint32_t v;
  if(so130_vector_id(w->offset)!=(pass==1))continue;
  if(!so130_read_word(rd,ctx,base+w->offset,&v)||v)goto rollback;
  t->owned|=1u<<i;
  wr(ctx,base+w->offset,w->value);
  if(!so130_read_word(rd,ctx,base+w->offset,&v)||v!=w->value)goto rollback;
 }
 return 1;
rollback:
 (void)so130_restore_locked(t,r,rd,wr,ctx);
 return 0;
}
#endif
