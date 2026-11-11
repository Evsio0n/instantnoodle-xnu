#include "slpi_glink_core.h"
#ifndef SLPI_LINK_READ_BARRIER
#define SLPI_LINK_READ_BARRIER() __asm__ volatile("dmb oshld":::"memory")
#endif
static uint32_t link_probe_enabled;
static struct sg_core sensor_link;
static uint32_t link_descriptor,link_tx,link_rx,link_tx_size,link_rx_size;
static bool link_irq_enabled;
static uint8_t link_frame[SG_MAX+16];
static int link_peer_item(const struct slpi_report*r,unsigned slot,uint32_t*off,uint32_t*bytes){
 const struct slpi_item*i=&r->items[slot];
 if(!i->bytes||!((i->host0==0&&i->host1==3)||(i->host0==3&&i->host1==0)))return 0;
 *off=i->offset;*bytes=i->bytes;return 1;
}
static int link_inventory(struct slpi_report*r){const char*e=slpi_observe(ro_reader,NULL,r);return !e&&r->remote_partition==0x17f000&&r->remote_bytes==0x30000;}
static int link_make(unsigned id,uint32_t bytes){
 int ok=0;uint32_t owner=0;struct slpi_report r;struct smem_partition h;
 lck_mtx_lock(&hk_smem_alloc_lock);need(&smem_lock);
 for(unsigned ms=0;ms<1000;ms++){wr(smem_lock.base+LOCK_OWNER,LOCK_US);owner=rd(smem_lock.base+LOCK_OWNER);if(owner==LOCK_US)break;sleep_us(1000);}
 if(owner!=LOCK_US){lck_mtx_unlock(&hk_smem_alloc_lock);return 0;}
 if(os_atomic_load(&boot_cancelled,acquire)||!link_inventory(&r))goto out;
 unsigned slot=id==478?3:4;uint32_t existing,size;
 if(link_peer_item(&r,slot,&existing,&size)){ok=size==bytes;goto out;}
 if(!smem_peek(r.remote_partition,&h,sizeof(h))||h.free_uncached<sizeof(h)||h.free_uncached>h.free_cached||h.free_cached>h.size||bytes>h.free_cached-h.free_uncached||16>h.free_cached-h.free_uncached-bytes)goto out;
 uint32_t at=r.remote_partition+h.free_uncached;
 /* Zero only a newly allocated owned item before publishing its allocation. */
 for(uint32_t p=0;p<bytes;p+=4)smem_poke(at+16+p,0);
 smem_poke(at,SMEM_CANARY|(id<<16));smem_poke(at+4,bytes);smem_poke(at+8,0);smem_poke(at+12,0);settle();
 smem_poke(r.remote_partition+__offsetof(struct smem_partition,free_uncached),h.free_uncached+16+bytes);settle();ok=1;
 tell("slpi-link: created AP-owned item%u offset=%x bytes=%u\n",id,at+16,bytes);
 out:wr(smem_lock.base+LOCK_OWNER,0);lck_mtx_unlock(&hk_smem_alloc_lock);return ok;
}
static int link_indices(uint32_t*txhead,uint32_t*txtail,uint32_t*rxhead,uint32_t*rxtail){
 *txtail=smem_word(link_descriptor);*txhead=smem_word(link_descriptor+4);*rxtail=smem_word(link_descriptor+8);*rxhead=smem_word(link_descriptor+12);
 return sg_ring_bounds(link_tx_size,*txhead,*txtail)&&sg_ring_bounds(link_rx_size,*rxhead,*rxtail);
}
static int link_emit(void*unused,const uint8_t*b,unsigned n){
 uint32_t h=smem_word(link_descriptor+4),t=smem_word(link_descriptor);
 if(!sg_ring_bounds(link_tx_size,h,t)||n<8||(n&7))return 0;
 if(link_rx_size&&!sg_ring_bounds(link_rx_size,smem_word(link_descriptor+12),smem_word(link_descriptor+8)))return 0;
 uint32_t room=sg_ring_room(link_tx_size,h,t);if(n>room)return 0;
 volatile uint8_t*q=(volatile uint8_t *)(uintptr_t)smem+link_tx;
 for(unsigned i=0;i<n;i++)q[(h+i)%link_tx_size]=b[i];settle();
 smem_poke(link_descriptor+4,(h+n)%link_tx_size);settle();wr(ipcc.base+IPCC_SEND,IPCC_SLPI<<16|IPCC_GLINK);return 1;
}
static void link_note(void*unused,unsigned event,uint32_t a,uint32_t b,uint32_t c,uint32_t d){
 if(event==13){wr(ipcc.base+IPCC_SEND,IPCC_SLPI<<16|IPCC_GLINK);return;}
 if(event==28)tell("slpi-wise: sent txn=%u peer=%u:%u mode=%u rate_bits=%x\n",b,c,d,sensor_link.wise.config.mode,sensor_link.wise.config.rate_binary32);
 else if(event==27 && a<VA_COUNT)tell("slpi-vattr: request=%s txn=%u peer=%u:%u\n",va_names[a],b,c,d);
 else if(event==26 && a<SD_COUNT)tell("slpi-deps: sent name=%s txn=%u peer=%u:%u\n",sd_names[a],b,c,d);
 else if(event==25 && a<SR_COUNT)tell("slpi-reg: sent group=%s txn=%u node=%u port=%u\n",sr_groups[a].name,b,c,d);
 else if(event==23){const char*name=(sensor_link.lookup==3&&d<SG_QUERY_COUNT)?sg_queries[d].name:(sensor_link.lookup==2?"ambient_light(all)":"ambient_light");tell("slpi-see: %s SUID lookup sent node=%u port=%u QMI-bytes=%u\n",name,a,b,c);}
 else if(event==24)tell("slpi-see: received packet%u bytes=%u from=%u:%u\n",a,b,c,d);
 else if(event==4||event==5)tell("slpi-link: QRTR %s service=%u version=%u instance=%u node=%u port=%u\n",event==4?"NEW":"DEL",a,b&255,b>>8,c,d);
 else if(event==10)tell("slpi-link: peer LOOKUP service=%u instance=%u from=%u:%u (not fabricated)\n",a,b,c,d);
 else if(event==20)tell("slpi-link: IPCRTR open ours=%u theirs=%u features=%x\n",a,b,c);
 else if(event==21)tell("slpi-link: unrelated remote channel%u name=%s left unanswered\n",a,sensor_link.others[b]);
 else if(event==2&&sensor_link.hello_received<=2)tell("slpi-link: QRTR HELLO node=%u\n",a);
}
static void link_peek(uint32_t tail,void*out,unsigned n){
 uint8_t*d=out;SLPI_LINK_READ_BARRIER();
 for(unsigned i=0;i<n;i++)d[i]=smem[link_rx+(tail+i)%link_rx_size];
}
static void slpi_link_quiesce(void){
 sensor_link.broken=1;
 if(link_irq_enabled){wr(ipcc.base+IPCC_DISABLE,IPCC_SLPI<<16|IPCC_GLINK);link_irq_enabled=false;}
 /* Static packet/intents storage is deliberately not freed on panic. */
}
static int slpi_glink_probe(unsigned lookup){
 struct slpi_report r;uint32_t size=0;uint64_t began=now_us();int success=0;
 if(!boot_once.owned_running||!boot_once.ready||os_atomic_load(&boot_cancelled,acquire)||!link_inventory(&r))return 0;
 for(unsigned slot=3;slot<6;slot++){uint32_t off=0,n=0;int found=link_peer_item(&r,slot,&off,&n);tell("slpi-link: after READY item%u present=%d offset=%x bytes=%u\n",475+slot,found,off,n);}
 if(!link_make(478,32)||!link_make(479,16384))goto done;
 /* Publish VERSION using only validated TX. Peer RX480 may be allocated lazily. */
 if(!link_inventory(&r)||!link_peer_item(&r,3,&link_descriptor,&size)||size!=32||!link_peer_item(&r,4,&link_tx,&link_tx_size)||link_tx_size!=16384)goto done;
 uint32_t th=smem_word(link_descriptor+4),tt=smem_word(link_descriptor),rh=0,rt=smem_word(link_descriptor+8);
 if(!sg_ring_bounds(link_tx_size,th,tt)||th!=0||tt!=0||rt!=0){tell("slpi-link: unexpected preexisting AP-owned indices; no reset\n");goto done;}
 sensor_link.rpc_probe=lookup>=4?lookup-3:0;
 sensor_link.wise_stream=oem130_wise==1 && sensor_link.rpc_probe==8;
 sensor_link.vendor_attrs=oem130_attrs!=0 && sensor_link.rpc_probe==8;
 sensor_link.lookup=sensor_link.rpc_probe==8?2:sensor_link.rpc_probe?0:lookup;
 if(!sg_init(&sensor_link,NULL,link_emit,link_note))goto done;
 while(now_us()-began<3000000&&!os_atomic_load(&boot_cancelled,acquire)){
  if(!link_inventory(&r))goto done;
  if(link_peer_item(&r,3,&link_descriptor,&size)&&size==32&&link_peer_item(&r,4,&link_tx,&link_tx_size)&&link_peer_item(&r,5,&link_rx,&link_rx_size))break;
  boot_wait_ms(NULL,2);
 }
 if(os_atomic_load(&boot_cancelled,acquire)||!link_rx||link_tx_size!=16384||link_rx_size<1024||link_rx_size>65536||(link_rx_size&7))goto done;
 if(!link_indices(&th,&tt,&rh,&rt)||rt!=0){tell("slpi-link: invalid indices after RX acquisition; no reset\n");goto done;}
 tell("slpi-link: descriptor=%x TX=%x/%u RX=%x/%u indices=%u,%u,%u,%u (no reset)\n",link_descriptor,link_tx,link_tx_size,link_rx,link_rx_size,tt,th,rt,rh);
 wr(ipcc.base+IPCC_ENABLE,IPCC_SLPI<<16|IPCC_GLINK);link_irq_enabled=true;
 while(now_us()-began<(sensor_link.rpc_probe==8?60000000:lookup?30000000:10000000)&&!sensor_link.broken&&!os_atomic_load(&boot_cancelled,acquire)){
  struct smp2p words;if(dsp_words(&words)){dsp_listen(&words);if(dsp_state(&words)&DSP_FATAL){sensor_link.broken=1;tell("slpi-link: DSP FATAL during inventory\n");break;}}
  doorbells();unsigned pumped=0;
  for(;pumped<32;pumped++){
   if(!link_indices(&th,&tt,&rh,&rt)){sensor_link.broken=1;break;}
   unsigned available=sg_ring_used(link_rx_size,rh,rt);if(available<8)break;
   link_peek(rt,link_frame,available<16?available:16);int n=sg_frame_length(link_frame,available,link_rx_size);
   if(n<0||(unsigned)n>sizeof(link_frame)){sensor_link.broken=1;break;}if(!n)break;
   link_peek(rt,link_frame,(unsigned)n);
   /* Advance AP-consumed tail once a bounded complete frame is captured. */
   smem_poke(link_descriptor+8,(rt+(uint32_t)n)%link_rx_size);settle();
   /* Bounded transport metadata only: locate the observed frame-6 rejection.
    * Do not dump QRTR/sensor payloads or relax validation without wire evidence. */
   if(sensor_link.received<16)
    tell("slpi-link: RX frame%u cmd=%u channel=%u arg=%u bytes=%u\n",sensor_link.received+1,
     slpi_u16(link_frame),slpi_u16(link_frame+2),slpi_u32(link_frame+4),(unsigned)n);
   if(sensor_link.received<16&&slpi_u16(link_frame)==5){
    unsigned count=slpi_u32(link_frame+4);
    for(unsigned j=0;j<count&&j<4&&16+8*j<=(unsigned)n;j++)
     tell("slpi-link: RX intent%u size=%u id=%u\n",j,slpi_u32(link_frame+8+8*j),slpi_u32(link_frame+12+8*j));
   }
   if(!sg_receive(&sensor_link,link_frame,(unsigned)n)){
    tell("slpi-link: rejected RX cmd=%u channel=%u arg=%u bytes=%u received=%u\n",
     slpi_u16(link_frame),slpi_u16(link_frame+2),slpi_u32(link_frame+4),(unsigned)n,sensor_link.received);
    break;
   }
  }
  /* Subscribe from boot and rescan once after a quiet interval following
   * registry writes. Keep initial and late transaction IDs distinct. */
  if(sensor_link.rpc_probe==8 && sensor_link.lookup==2 && sensor_link.lookup_sent &&
     sensor_link.wrote_registry && now_us()-sensor_link.std_activity_us>=4000000){
   sensor_link.query_base=SG_QUERY_COUNT;
   sensor_link.lookup=3;sensor_link.lookup_sent=0;
   tell("slpi-see: late inventory after registry activity; callbacks=%u\n",sensor_link.callbacks);
  }
  if(!sg_tick(&sensor_link))break;
  if(pumped<32 && sg_registry_reads_done(&sensor_link)){
   struct sd_state *deps=&sensor_link.deps;
   uint64_t now=now_us();
   if(!deps->started_us){
    deps->started_us=now;
    if(sensor_link.vendor_attrs)
     for(unsigned i=0;i<sensor_link.reply_count;i++)va_report(&sensor_link.vendor,
        &sensor_link.registry,sensor_link.replies[i],sensor_link.reply_size[i]);
    tell("slpi-reg: all four reads terminal after late inventory; observing dependency SUIDs\n");
   }else if(sd_window_done(deps,now) &&
             (!sensor_link.vendor_attrs || va_terminal(&sensor_link.vendor) || now-deps->started_us>=8000000) &&
             (!sensor_link.wise_stream || sensor_link.wise.invalid || sensor_link.wise.rejected ||
              (sensor_link.wise.accepted && sensor_link.wise.count>=3) || now-deps->started_us>=8000000)){
    tell("slpi-deps: observation ended elapsed_ms=%llu terminal=%u\n",
         (unsigned long long)((now-deps->started_us)/1000),sd_terminal(deps));
    break;
   }
  }
  if(sensor_link.rpc_probe>=3 && sensor_link.rpc.completed && sensor_link.rpc.tx_done &&
     (sensor_link.rpc_probe==3 || (sensor_link.rpc_probe==4 && sensor_link.listener_started) || (sensor_link.next_started && (sensor_link.rpc_probe!=8 || sensor_link.callbacks>=SG_REGISTRY_CALLBACK_LIMIT)) || sensor_link.rpc.retval))break;
  boot_wait_ms(NULL,2);
 }
 success=!os_atomic_load(&boot_cancelled,acquire)&&sensor_link.agreed&&sensor_link.open&&sensor_link.hello_received&&!sensor_link.broken;
 if(sensor_link.rpc_probe){
  tell("slpi-rpc: peer=%u local=2 requested=%u acknowledged=%u intents=%u broken=%u\n",
   sensor_link.rpc_remote,sensor_link.rpc_requested,sensor_link.rpc_ack,sensor_link.rpc_intents,sensor_link.broken);
  if(sensor_link.rpc_probe>=3){
   struct sg_rpc_call *call=&sensor_link.rpc;
   if(sensor_link.next_started)
    tell("slpi-next: attach_ok=1 init2_ok=1 sent=%u complete=%u retval=%d retired=%u\n",
     call->sent,call->completed,call->retval,call->tx_done);
   else if(sensor_link.listener_started)
    tell("slpi-listener: attach_ok=1 init2_sent=%u complete=%u retval=%d flags=%u tx_done=%u\n",
     call->sent,call->completed,call->retval,call->flags,call->tx_done);
   else tell("slpi-attach: sent=%u tx_done=%u complete=%u retval=%d flags=%u early=%u stale=%u\n",
    call->sent,call->tx_done,call->completed,call->retval,call->flags,call->early,call->stale);
   if(!call->completed||call->retval)success=0;
  }
  if(sensor_link.rpc_probe>=2){
   tell("slpi-rpc: intent_request=%u reply=%u max_capacity=%u\n",sensor_link.rpc_intent_requested,
    sensor_link.rpc_intent_reply,sensor_link.rpc_max_intent);
   if(sensor_link.rpc_intent_reply!=2 || sensor_link.rpc_max_intent<40)success=0;
  }
  if(sensor_link.rpc_probe>=6){
   const struct sg_rpc_call *fg=&sensor_link.foreground;
   tell("slpi-module: open_sent=%u start_phase=%u complete=%u retval=%d tx_done=%u\n",
       sensor_link.foreground_armed && (fg->sent || sensor_link.module_start),
       sensor_link.module_start,fg->completed,fg->retval,fg->tx_done);
  }
  if(!sensor_link.rpc_ack)success=0;
  if(sensor_link.rpc_remote&&!sensor_link.broken&&!sg_emit(&sensor_link,3,2,0,NULL,0))success=0;
 }
 if(sensor_link.open&&!sensor_link.broken&&!sg_emit(&sensor_link,3,1,0,NULL,0))success=0;
 done:;
 unsigned count=0;for(unsigned i=0;i<64;i++)count+=sensor_link.services[i].present!=0;
 tell("slpi-link: inventory result=%d negotiated=%u open=%u hello=%u services=%u frames=%u QRTR=%u broken=%u cancelled=%u; no SEE/lux claim\n",success,sensor_link.agreed,sensor_link.open,sensor_link.hello_received,count,sensor_link.received,sensor_link.qrtr_in,sensor_link.broken,os_atomic_load(&boot_cancelled,acquire));
 for(unsigned i=0;i<64;i++)if(sensor_link.services[i].present){struct sg_service*s=&sensor_link.services[i];tell("slpi-link: final service=%u version=%u instance=%u node=%u port=%u\n",s->service,s->instance&255,s->instance>>8,s->node,s->port);}
 if(lookup){
  tell("slpi-see: lookup_sent=%u replies=%u; identification only, no lux claim\n",sensor_link.lookup_sent,sensor_link.reply_count);
  static const char digits[]="0123456789abcdef";
  for(unsigned i=0;i<sensor_link.reply_count;i++)for(unsigned off=0;off<sensor_link.reply_size[i];off+=32){
   char hex[65];unsigned n=sensor_link.reply_size[i]-off;if(n>32)n=32;
   for(unsigned j=0;j<n;j++){unsigned v=sensor_link.replies[i][off+j];hex[2*j]=digits[v>>4];hex[2*j+1]=digits[v&15];}hex[2*n]=0;
   tell("slpi-see: packet%u offset%u hex=%s\n",i,off,hex);
  }
 }
 if(sensor_link.rpc_probe==8){
  if(sensor_link.wise_stream){
   const struct vw_state *w=&sensor_link.wise;
   tell("slpi-wise: sent=%u accepted=%u rejected=%u invalid=%u samples=%u malformed=%u uid=%llx,%llx\n",
    w->sent,w->accepted,w->rejected,w->invalid,w->count,w->bad_events,(unsigned long long)w->config.source.lo,
    (unsigned long long)w->config.source.hi);
   for(unsigned i=0;i<w->count;i++){
    const struct vw_sample *m=&w->samples[i];
    tell("slpi-wise: sample=%u ticks=%llu status=%u n=%u lux_bits=%x c_bits=%x period_ms_bits=%x\n",
     i,(unsigned long long)m->ticks,m->status,m->fields,m->lux_bits,m->c_bits,m->period_bits);
   }
  }
  if(sensor_link.vendor_attrs){
   const struct va_state *v=&sensor_link.vendor;
   tell("slpi-vattr: seen=%x sent=%x ack=%x failed=%x attrs=%x invalid=%x\n",
        v->seen,v->sent,v->ack,v->failed,v->received,v->invalid);
   for(unsigned i=0;i<VA_COUNT;i++)if(v->received&(1u<<i)){
    const struct sl_attrs *a=&v->attrs[i];
    tell("slpi-vattr: lookup=%s uid=%llx,%llx name=%s vendor=%s type=%s fields=%x physical=%u available=%u stream=%llu rate_count=%u api_count=%u api_extra=%u\n",
         va_names[i],(unsigned long long)a->source.lo,(unsigned long long)a->source.hi,a->name,a->vendor,a->type,
         a->present,a->physical,a->available,(unsigned long long)a->stream,a->rate_count,a->api_count,a->extra_api);
    for(unsigned j=0;j<a->api_count;j++)tell("slpi-vattr: lookup=%s api%u=%s\n",va_names[i],j,a->api[j]);
    for(unsigned j=0;j<a->rate_count;j++)tell("slpi-vattr: lookup=%s rate%u_bits=%x\n",va_names[i],j,a->rates[j]);
   }
  }
  const struct sr_client *reg=&sensor_link.registry;
  const struct sd_state *deps=&sensor_link.deps;
  tell("slpi-deps: sent=%x ack_ok=%x ack_fail=%x events=%x available=%x invalidated=%u capture_dropped=%u\n",
       deps->sent,deps->ack_ok,deps->ack_fail,deps->observed,deps->available,reg->invalidated,sensor_link.capture_dropped);
  for(unsigned i=0;i<SD_COUNT;i++)
   tell("slpi-deps: name=%s reports=%u count=%u ticks=%llu\n",sd_names[i],deps->reports[i],
        deps->count[i],(unsigned long long)deps->ticks[i]);
  tell("slpi-reg: sent=%x ack_ok=%x ack_fail=%x events=%x invalidated=%u\n",
       reg->sent,reg->ack_ok,reg->ack_fail,reg->received,reg->invalidated);
  for(unsigned i=0;i<SR_COUNT;i++)if(reg->sent&(1u<<i)){
   const struct sr_result *r=&reg->result[i];
   tell("slpi-reg: group=%s read_events=%u items=%u seen=%x mismatches=%x versions=%x version_mismatches=%x extra=%u nested=%u values_match=%u\n",
       sr_groups[i].name,r->events,r->items,r->seen,r->mismatch,r->has_version,r->version_mismatch,
       r->extra,r->nested,sr_values_match(reg,i));
  }
 }
 slpi_link_quiesce();return success;
}
