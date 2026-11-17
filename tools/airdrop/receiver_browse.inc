/* Native receiver liveness fallback, opt-in and tied to the actual original HTTP receiver lifetime.
 * A real DNS-SD browse produced the same native SET120 subscription as the
 * share UI and restored native Ask/Upload in the paired hardware experiment.
 * Never publish an answer/identity, accept a transfer, or alter TLS policy.
 * Native receiver methods are forwarded unchanged; no service is published here. */
#include <dns_sd.h>
#include <net/if.h>
#include <pthread.h>
#include "receiver_browse_owners.h"
static pthread_mutex_t receiver_lock=PTHREAD_MUTEX_INITIALIZER;
static struct hk_receiver_publishers receiver_publishers;
static bool receiver_enabled,receiver_cfg;
static dispatch_queue_t receiver_queue;
/* These two fields belong exclusively to receiver_queue. */
static DNSServiceRef receiver_browser;
static unsigned receiver_interface;
static void receiver_reconcile(void);
static void receiver_schedule(void){
 if(receiver_queue)dispatch_async(receiver_queue,^{receiver_reconcile();});
}
static void receiver_result(DNSServiceRef ref,DNSServiceFlags flags,uint32_t index,
 DNSServiceErrorType error,const char *name,const char *type,const char *domain,void *context){
 (void)flags;(void)name;(void)type;(void)domain;(void)context;
 if(!error){record("receiver_browse_result interface=%u error=0",index);return;}
 record("receiver_browse_result interface=%u error=%d",index,error);
 if(receiver_browser==ref){receiver_browser=NULL;receiver_interface=0;DNSServiceRefDeallocate(ref);}
 /* A daemon error must not create a retry spin or keep a dead ref alive. */
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),receiver_queue,^{receiver_reconcile();});
}
static void receiver_reconcile(void){
 pthread_mutex_lock(&receiver_lock);
 unsigned owners=hk_receiver_count(&receiver_publishers);bool enabled=receiver_enabled;
 pthread_mutex_unlock(&receiver_lock);
 unsigned index=if_nametoindex("awdl0");
 bool wanted=enabled && owners && index;
 if(receiver_browser && (!wanted || receiver_interface!=index)){
  DNSServiceRef old=receiver_browser;receiver_browser=NULL;receiver_interface=0;
  DNSServiceRefDeallocate(old);record("receiver_browse_stop owners=%u",owners);
 }
 if(!wanted || receiver_browser)return;
 DNSServiceRef ref=NULL;
 DNSServiceErrorType error=DNSServiceBrowse(&ref,kDNSServiceFlagsIncludeAWDL,index,
  "_airdrop._tcp","local.",receiver_result,NULL);
 if(!error)error=DNSServiceSetDispatchQueue(ref,receiver_queue);
 if(error){if(ref)DNSServiceRefDeallocate(ref);record("receiver_browse_start owners=%u interface=%u error=%d",owners,index,error);return;}
 receiver_browser=ref;receiver_interface=index;
 record("receiver_browse_start owners=%u interface=%u error=0",owners,index);
}
static void receiver_owner_update(const void *owner,bool live){
 if(!receiver_cfg)return;
 pthread_mutex_lock(&receiver_lock);
 unsigned before=hk_receiver_count(&receiver_publishers);
 bool ok=live?hk_receiver_add(&receiver_publishers,owner):hk_receiver_remove(&receiver_publishers,owner);
 unsigned after=hk_receiver_count(&receiver_publishers);
 pthread_mutex_unlock(&receiver_lock);
 if(before!=after){record("receiver_owner live=%u owners=%u",live,after);receiver_schedule();}
 else if(live && !ok)record("receiver_owner_registry_full");
}
static void receiver_configure(void){
 receiver_cfg=getenv("HK_AIRDROP_RECEIVER_BROWSE") && !strcmp(getenv("HK_AIRDROP_RECEIVER_BROWSE"),"1");
 if(receiver_cfg)receiver_queue=dispatch_queue_create("hk.airdrop.receiver-browse",DISPATCH_QUEUE_SERIAL);
}
static void receiver_authorize(bool original_20f66_verified){
 pthread_mutex_lock(&receiver_lock);receiver_enabled=receiver_cfg && original_20f66_verified;pthread_mutex_unlock(&receiver_lock);
 record("receiver_browse_guard enabled=%u original_verified=%u",receiver_enabled,original_20f66_verified);
 receiver_schedule();
}
