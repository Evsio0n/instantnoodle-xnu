#ifndef SLPI_SMP2P_CHECKS_H
#define SLPI_SMP2P_CHECKS_H
#include "slpi_smem_ro.h"
static int slpi_smp2p_state(const uint8_t *p,unsigned n,unsigned local,unsigned remote,const char *name,uint32_t *state){
 if(n<20||slpi_u32(p)!=0x504d5324||(slpi_u32(p+4)&255)!=1||slpi_u16(p+8)!=local||slpi_u16(p+10)!=remote)return 0;
 unsigned total=slpi_u16(p+12),valid=slpi_u16(p+14),namelen=0,hits=0;
 if(!total||total>16||valid>total||valid>(n-20)/20)return 0;
 while(namelen<16&&name[namelen])namelen++;if(namelen==16)return 0;
 for(unsigned i=0;i<valid;i++){
  const uint8_t *e=p+20+20*i;unsigned k=0;
  for(;k<=namelen&&e[k]==(uint8_t)name[k];k++);
  if(k==namelen+1){*state=slpi_u32(e+16);hits++;}
 }
 return hits==1;
}
struct slpi_stop_transition {unsigned saw_low;};
static int slpi_stop_fresh(struct slpi_stop_transition *t,uint32_t state){
 if(!(state&8)){t->saw_low=1;return 0;}return t->saw_low!=0;
}
#endif
