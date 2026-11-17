#ifndef HK_LIGHT_POLICY_H
#define HK_LIGHT_POLICY_H
#include <stdint.h>
#include <stdbool.h>
enum { LP_FLOOR=256,LP_MAX=2047,LP_STEP=24,LP_DEADBAND=8,LP_REQUIRED=3,LP_MAX_WRITES=240 };
struct lp_state {
 unsigned baseline,current,writes,band,stable,source;
 uint64_t start,last_apply,last_sample,last_tick,lo,hi;
 bool armed,terminal,touched;
 unsigned reference,goal,stable_goal,nits_x10000;uint64_t stable_since;
};
struct lp_point {uint32_t x,y;};
#include "lp_tables.h"
static inline uint32_t lp_interp(uint32_t x,const struct lp_point *p,unsigned n){
 if(x<=p[0].x)return p[0].y;
 for(unsigned i=1;i<n;i++)if(x<=p[i].x){
  uint32_t span=p[i].x-p[i-1].x;
  uint64_t delta=(uint64_t)(x-p[i-1].x)*(p[i].y-p[i-1].y);
  return p[i-1].y+(uint32_t)((delta+span/2)/span);
 }
 return p[n-1].y;
}
static inline uint32_t lp_millilux(uint32_t bits){
 if(bits>=0x469c4000u)return 20000000;
 unsigned exp=(bits>>23)&255;
 if(exp<105)return 0;
 unsigned shift=150-exp;
 uint64_t scaled=(uint64_t)((bits&0x7fffff)|0x800000)*1000;
 return (uint32_t)((scaled+(((uint64_t)1)<<(shift-1)))>>shift);
}
static inline unsigned lp_target(struct lp_state *s,uint64_t now_us,uint64_t tick,uint32_t lux){
 if(!s->armed||s->terminal||!tick||tick<=s->last_tick||lux>=0x4f800000u||now_us<s->last_sample)return 0;
 s->last_tick=tick;s->last_sample=now_us;
 s->nits_x10000=lp_interp(lp_millilux(lux),lux_nits,sizeof(lux_nits)/sizeof(lux_nits[0]));
 uint32_t numerator=lp_interp(s->nits_x10000,nits_backlight,sizeof(nits_backlight)/sizeof(nits_backlight[0]));
 s->reference=(numerator+127)/255;
 unsigned low=s->baseline-s->baseline/4;
 unsigned visible=s->baseline<LP_FLOOR?s->baseline:LP_FLOOR;
 if(low<visible)low=visible;
 if(s->baseline>64 && low<s->baseline-64)low=s->baseline-64;
 unsigned high=s->baseline+48;if(high>LP_MAX)high=LP_MAX;
 s->goal=s->reference<low?low:s->reference>high?high:s->reference;
 s->band=s->reference<low?0:s->reference>high?2:1;
 int delta=(int)s->goal-(int)s->current;
 if(delta>=-LP_DEADBAND&&delta<=LP_DEADBAND){s->stable=0;return 0;}
 int drift=(int)s->goal-(int)s->stable_goal;
 if(!s->stable||drift>LP_DEADBAND||drift< -LP_DEADBAND){
  s->stable=1;s->stable_goal=s->goal;s->stable_since=now_us;return 0;
 }
 if(s->stable<LP_REQUIRED)s->stable++;
 if(s->stable<LP_REQUIRED||s->writes>=LP_MAX_WRITES||now_us-s->stable_since<1500000||now_us-s->last_apply<1500000)return 0;
 if(delta>LP_STEP)delta=LP_STEP;if(delta< -LP_STEP)delta=-LP_STEP;
 return (unsigned)((int)s->current+delta);
}
static inline bool lp_read_dbv(uint32_t bytes,uint32_t fifo,unsigned *value)
{
 /* OEM readback returns ntohl(RDBK_DATA0). For a two-byte short DCS reply
  * the high byte is the response type, then the panel's high/low DBV. */
 if(bytes!=4 || fifo>>24!=0x22)return false;
 unsigned dbv=(fifo>>8)&0xffff;if(!dbv||dbv>LP_MAX)return false;*value=dbv;return true;
}
#endif
