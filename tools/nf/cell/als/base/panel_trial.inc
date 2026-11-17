#include "hk_light_policy.h"
#include "hk_user_preference.h"
static struct lp_pref lpUser;
static struct lp_state lp;
static uint64_t lpOwnerGeneration,lpOwnerDone,lpOwnerChangeUs,lpAttemptUs;
static unsigned lpOwnerLevel,lpSourceReadAttempts;
static bool lpReadUnknown;
static unsigned lpEntries,lpNoLock,lpTerminal,lpOff,lpHbm,lpNotPushing,lpNotReady,lpWrongStatus,lpIdle,lpBaseline,lpMatched,lpSent,lpRestored;
static void lpMarks(void){
 displayLog("light-panel: gates entry=%u no_lock=%u terminal=%u off=%u hbm=%u inactive=%u no_dsi=%u bad_sample=%u idle_scanout=%u baseline=%u selections=%u applied=%u restored=%u\n",
    lpEntries,lpNoLock,lpTerminal,lpOff,lpHbm,lpNotPushing,lpNotReady,lpWrongStatus,lpIdle,lpBaseline,lpMatched,lpSent,lpRestored);
}
static void lpOwnerLocked(void){
 lpOwnerGeneration++;lpOwnerChangeUs=micros(mach_absolute_time());
 if(lp.armed&&!lp.terminal){
  lp.terminal=true;lp.armed=false;lp_pref_manual(&lpUser);
  displayLog("light-panel: manual-priority generation=%llu applied_before=%u writes=%u\n",lpOwnerGeneration,lp.current,lp.writes);
 }
}
static void lpOwnerResultLocked(bool done){
 if(done && lpFreshDone){lpOwnerDone=gBacklightSent;lpOwnerLevel=gBacklight;}
 else{lpOwnerDone=0;lpOwnerLevel=0;}
 displayLog("light-panel: owner-command generation=%llu dbv=%u dma_done=%u fresh_done=%u count=%llu\n",
       lpOwnerGeneration,gBacklight,done,done&&lpFreshDone,gBacklightSent);
}
static bool lpReadLocked(unsigned *value){
 /* The two short packets are submitted together: MRPS 2, then a read of
  * GET_DISPLAY_BRIGHTNESS (0x52) with BTA+last flags. No panel register is set. */
 if(lpIoUncertain||!instantnoodle()||gPanelOff||gHbmOn||!gPushing||!dsiReady())return false;
 if(gPushed)pushWait();
 uint32_t saved=displayReg(gDSI,DSI_CMD_DMA_CTRL);
 *(volatile uint32_t*)(gDSI+DSI_CMD_DMA_CTRL)=saved&~DSI_CMD_DMA_LOW_POWER;
 volatile uint8_t *q=(volatile uint8_t*)gDsiDma;
 const uint8_t packets[8]={2,0,0x37,0,0x52,0,0x06,0xa0};
 for(unsigned i=0;i<8;i++)q[i]=packets[i];
 *(volatile uint32_t*)(gDSI+0x1d4)=1;__asm__ volatile("dsb sy":::"memory");
 *(volatile uint32_t*)(gDSI+0x1d4)=0;__asm__ volatile("dsb sy":::"memory");
 unsigned n=0;uint32_t data=0;
 bool transmitted=!(displayReg(gDSI,0x1d4)>>16) && lpSubmitDma(gDSI,8,true);
 if(transmitted){
  for(unsigned i=0;i<100;i++){n=displayReg(gDSI,0x1d4)>>16;if(n)break;IODelay(10);}
  if(n==4)data=displayReg(gDSI,0x6c);
  if(!n)lpIoUncertain=true;
 }
 *(volatile uint32_t*)(gDSI+DSI_CMD_DMA_CTRL)=saved;
 bool ok=transmitted&&lp_read_dbv(n,data,value);
 displayLog("light-panel: read52 dma_done=%u count=%u type=%x dbv=%u valid=%u\n",transmitted,n,data>>24,ok?*value:0,ok);
 return ok;
}
static bool lpWriteLocked(unsigned target,const char *reason){
 uint64_t before=gBacklightSent;unsigned previous=gBacklight;
 gBacklight=target;lpStrictTransfer=true;lpFreshDone=false;
 bool sent=backlightSendLocked();lpStrictTransfer=false;
 bool ok=sent&&lpFreshDone&&gBacklightSent==before+1;
 displayLog("light-panel: %s target=%u before=%llu after=%llu dma_done=%u fresh_done=%u\n",
    reason,target,before,gBacklightSent,sent,lpFreshDone);
 if(!ok)gBacklight=previous;
 return ok;
}
static void lpEndLocked(const char *reason){
 if(!lp.armed||lp.terminal)return;
 lp.armed=false;lp.terminal=true;lpUser.live=false;bool restored=!lp.touched;
 if(lp.touched){
  if(!lpIoUncertain && !gPanelOff && !gHbmOn && dsiReady()){
   restored=lpWriteLocked(lp.baseline,"restore");
   if(restored && lp.source==2){unsigned actual=0;restored=lpReadLocked(&actual)&&actual==lp.baseline;}
  }
  /* Normal wake/exit will re-use the original proven owner DBV even when a
   * failed or interrupted transfer prevented an immediate confirmed restore. */
  gBacklight=lp.baseline;
  if(restored){
   lpRestored++;
   /* This confirmed restore reinstated the same verified manual DBV. Carry
    * forward its new hardware completion counter for the next OS On edge. */
   if(lp.source==1 && lpOwnerLevel==lp.baseline)lpOwnerDone=gBacklightSent;
  }
 }
 displayLog("light-panel: end reason=%s baseline=%u final_request=%u restored=%u writes=%u source=%u\n",
     reason,lp.baseline,gBacklight,restored,lp.writes,lp.source);
}
extern "C" void hk_light_user_preference(unsigned value,uint64_t now){
 if(!gPanelLock)return;
 IOLockLock(gPanelLock);enum lp_change action=lp_preference(&lpUser,value,now);
 if(action==LP_DISABLE){lpEndLocked("user-pref-or-service-off");}
 if(action==LP_ENABLE && !lpIoUncertain){lp=(struct lp_state){};lpSourceReadAttempts=0;lpReadUnknown=false;}
 if(action!=LP_KEEP)displayLog("light-panel: user preference value=%u action=%u manual_hold=%u\n",value,action,lpUser.manual_hold);
 IOLockUnlock(gPanelLock);
}
extern "C" void hk_light_panel_tick(uint64_t now){
 if(!gPanelLock)return;
 IOLockLock(gPanelLock);
 if(lp.armed && !lp.terminal){
  if(!lp_pref_fresh(&lpUser,now)){lpUser.live=false;lpEndLocked("preference-expired");}
  else if(now-lp.last_sample>=1500000)lpEndLocked("stale");
 }
 IOLockUnlock(gPanelLock);
}
extern "C" void hk_light_panel_end(void){
 if(!gPanelLock){lpMarks();return;}IOLockLock(gPanelLock);lpEndLocked("source-end");lpMarks();IOLockUnlock(gPanelLock);
}
extern "C" void hk_light_panel_sample(uint64_t now,uint64_t tick,uint64_t lo,uint64_t hi,unsigned seq,unsigned status,uint32_t lux){
 lpEntries++;
 if(!gPanelLock){lpNoLock++;return;}
 if(!tick||!(lo|hi)){lpWrongStatus++;return;}
 unsigned completions=0,swaps=0;
 IOLockLock(gPanelLock);
 if(!lp_pref_fresh(&lpUser,now))goto out;
 if(lp.terminal){lpTerminal++;goto out;}
 if(lpIoUncertain){lpEndLocked("controller-uncertain");goto out;}
 completions=__atomic_load_n(&lpCompletedFrames,__ATOMIC_RELAXED);
 swaps=__atomic_load_n(&lpPresentedSwaps,__ATOMIC_RELAXED);
 if(!completions && gPushing && !gPanelOff && gPushed)pushWait();
 completions=__atomic_load_n(&lpCompletedFrames,__ATOMIC_RELAXED);
 if(lpEntries==1 || lpEntries==16)
  displayLog("light-panel: backend lock=1 pushing=%u lit=%u hbm=%u dsi=%u pending=%u swap_id=%u completed=%u distinct_swaps=%u\n",
   gPushing,!gPanelOff,gHbmOn,dsiReady(),gPushed!=0,gPushId,completions,swaps);
 if(!gPushed)lpIdle++;
 if(gPanelOff)lpOff++;
 if(gHbmOn)lpHbm++;
 if(!gPushing || !completions)lpNotPushing++;
 if(!dsiReady())lpNotReady++;
 if(status!=3||lux>=0x4f800000u)lpWrongStatus++;
 if(gPanelOff||gHbmOn||!gPushing||!completions||!dsiReady()||status!=3||lux>=0x4f800000u){
  if(lpEntries==1)displayLog("light-panel: denied off=%u hbm=%u flushing=%u dsi=%u pending=%u status=%u\n",
                             gPanelOff,gHbmOn,gPushing,dsiReady(),gPushed!=0,status);
  lpEndLocked("source-or-display-invalid");goto out;
 }
 if(!lp.armed){
  unsigned base=0,source=0;
  if(lpOwnerDone && lpOwnerDone==gBacklightSent && lpOwnerLevel && lpOwnerLevel==gBacklight){base=gBacklight;source=1;}
  else if(!lpReadUnknown && !lpSourceReadAttempts){
   unsigned a=0,b=0;lpSourceReadAttempts++;
   if(lpReadLocked(&a)&&lpReadLocked(&b)&&a==b){base=a;source=2;}else lpReadUnknown=true;
  }
  if(source==1 && now>=lpOwnerChangeUs && now-lpOwnerChangeUs<2500000){
   if(!lpAttemptUs||now-lpAttemptUs>1000000){lpAttemptUs=now;displayLog("light-panel: owner settling generation=%llu dbv=%u elapsed_us=%llu\n",lpOwnerGeneration,base,now-lpOwnerChangeUs);}
   goto out;
  }
  /* A confirmed, already visible user's baseline below 256 is allowed
   * only as its own lower bound. Never darken it below its original value. */
  if(!source||base<64||base>LP_MAX||(base<LP_FLOOR && source!=1)){
   if(seq==1||seq==16)displayLog("light-panel: awaiting observable visible baseline owner_done=%llu candidate=%u attempts=%u\n",lpOwnerDone,base,lpSourceReadAttempts);
   goto out;
  }
  lpBaseline++;lp.baseline=lp.current=base;lp.source=source;lp.lo=lo;lp.hi=hi;lp.last_sample=lp.start=lp.last_apply=now;lp.armed=true;
  displayLog("light-panel: arm baseline=%u source=%u owner_gen=%llu panel_sent=%llu uid=%llx,%llx\n",base,source,lpOwnerGeneration,gBacklightSent,lo,hi);
 }
 if(lo!=lp.lo||hi!=lp.hi){lpEndLocked("source-changed");goto out;}
 {
  unsigned target=lp_target(&lp,now,tick,lux);
  if(target){
   lpMatched++;
   displayLog("light-curve: seq=%u tick=%llu mlux=%u nits_x10000=%u reference_dbv=%u bounded_goal=%u stable_us=%llu\n",
      seq,tick,lp_millilux(lux),lp.nits_x10000,lp.reference,lp.goal,now-lp.stable_since);
   displayLog("light-panel: select seq=%u tick=%llu us=%llu lux_bits=%x band=%u from=%u target=%u\n",
     seq,tick,now,lux,lp.band,lp.current,target);
   lp.touched=true; /* after a timeout the old hardware level is uncertain */
   if(lpWriteLocked(target,"apply")){
    lpSent++;lp.current=target;lp.last_apply=now;lp.writes++;
    if(lp.source==2){unsigned read=0;if(!lpReadLocked(&read)||read!=target)lpEndLocked("readback-mismatch");}
   }else lpEndLocked("transfer-failed");
  }
 }
out:IOLockUnlock(gPanelLock);
}

extern "C" bool hk_light_panel_is_armed(void){
 if(!gPanelLock)return false;IOLockLock(gPanelLock);bool ready=lp.armed&&!lp.terminal;IOLockUnlock(gPanelLock);return ready;
}
