#ifndef HK_USER_LIGHT_PREF_H
#define HK_USER_LIGHT_PREF_H
#include <stdint.h>
#include <stdbool.h>
enum { LP_PREF_UNKNOWN=0,LP_PREF_OFF=1,LP_PREF_ON=2 };
struct lp_pref {unsigned last_known;uint64_t seen;bool live,manual_hold;};
enum lp_change { LP_KEEP,LP_DISABLE,LP_ENABLE };
static inline enum lp_change lp_preference(struct lp_pref *s,unsigned pos,uint64_t now){
 if(pos>LP_PREF_ON || !now || (s->seen&&now<s->seen))return LP_KEEP;
 bool old=s->live;
 s->seen=now;
 if(pos==LP_PREF_OFF){s->last_known=LP_PREF_OFF;s->live=false;}
 else if(pos==LP_PREF_UNKNOWN)s->live=false;
 else{
  /* A missing service/timeout is not an explicit Settings off->on edge. */
  if(s->last_known==LP_PREF_OFF)s->manual_hold=false;
  s->last_known=LP_PREF_ON;s->live=!s->manual_hold;
 }
 return old==s->live?LP_KEEP:s->live?LP_ENABLE:LP_DISABLE;
}
static inline bool lp_pref_fresh(const struct lp_pref *s,uint64_t now){
 return s->live && !s->manual_hold && now>=s->seen && now-s->seen<=3500000;
}
static inline void lp_pref_manual(struct lp_pref *s){s->manual_hold=true;s->live=false;}
#endif
