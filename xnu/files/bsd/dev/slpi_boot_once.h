/* Offline review candidate: one-shot SLPI start/lifecycle core.
 * Hardware callbacks must meet contracts in start-adapter-review.md.
 * This file contains NO default hardware implementation and cannot self-start.
 */
#ifndef SLPI_BOOT_ONCE_H
#define SLPI_BOOT_ONCE_H
#define SLPI_PAS_ID 12u
#define SLPI_LOAD_BASE 0x90600000u
#define SLPI_LOAD_BYTES 0x1500000u
#define SLPI_READY_LIMIT_MS 10000u
#define SLPI_STOP_LIMIT_MS 2000u
struct slpi_boot_evidence {
 unsigned cold_boot_exclusive, map_verified, native_hashes_verified;
 unsigned proxy_adapter_reviewed, cleanup_hook_registered;
};
struct slpi_boot_state {
 unsigned attempted, proxy_held, metadata_held, load_state_on;
 unsigned auth_attempted, owned_running, ready, poisoned, shutdown_attempted;
 unsigned preserve_metadata, scm_uncertain;
 const char *last;
};
struct slpi_boot_ops {
 void *ctx;
 /* Must re-run bounded PID0/3 preflight. Reject ALL selected existing items.
  * Success is only an additional check, not an independent DSP-off proof. */
 int (*empty_smem_preflight)(void *);
 /* Parse/check actual ELF and hashes, reserve full range, pin contiguous metadata.
  * No physical DSP-memory writes here. */
 int (*prepare_native_image)(void *,unsigned base,unsigned bytes);
 /* Shared per-client proxy ownership: lcx/lmx384 and xo.lvl3, bounded.
  * Return1 acquired;0 definite all-or-nothing refusal;negative ambiguous/partial. */
 int (*proxy_acquire)(void *);
 int (*aop_load_state)(void *,int on);
 int (*pas_init_header)(void *,unsigned pas);
 int (*pas_mem_setup)(void *,unsigned pas,unsigned base,unsigned bytes);
 /* Exact PT_LOAD file bytes, zero EVERY p_memsz tail (including phdr22),
  * bounded mapping, readback, D-cache clean/invalidate as mapping requires +DSB. */
 int (*copy_verify_publish_image)(void *);
 /* Shared SMEM allocation lock+hardware lock3; SMP2P430 host0/3, client4/signal2.
  * Never overwrite preexisting SLPI items. Queue initialisation only owned ones. */
 int (*publish_owned_smp2p)(void *);
 /* Return1 only when both SMC transport and secure result indicate success.
  * Any other result treated ambiguous: no retry/no unowned shutdown. */
 int (*pas_auth_reset)(void *,unsigned pas);
 /* Bounded poll10s; handle SMP2P restart/READY/FATAL and only client4 IPCC events.
  * Must not extend deadline indefinitely after READY. */
 int (*wait_ready)(void *,unsigned milliseconds);
 int (*request_owned_stop)(void *,unsigned milliseconds);
 int (*pas_shutdown)(void *,unsigned pas);
 /* Restore only this client's aggregate vote, never blindly write rail/XO0.
  * Diagnostic retained-floor implementation may return0, requiring cold reboot. */
 int (*proxy_release)(void *);
 void (*release_metadata)(void *);
};
static int slpi_boot_ops_valid(const struct slpi_boot_ops *o){return o&&o->empty_smem_preflight&&o->prepare_native_image&&o->proxy_acquire&&o->aop_load_state&&o->pas_init_header&&o->pas_mem_setup&&o->copy_verify_publish_image&&o->publish_owned_smp2p&&o->pas_auth_reset&&o->wait_ready&&o->request_owned_stop&&o->pas_shutdown&&o->proxy_release&&o->release_metadata;}
static int slpi_boot_release_pre_auth(struct slpi_boot_state *s,const struct slpi_boot_ops *o){
 if(s->scm_uncertain){s->poisoned=1;s->preserve_metadata=1;return 0;}
 int ok=1;
 if(s->load_state_on){if(o->aop_load_state(o->ctx,0)==1)s->load_state_on=0;else ok=0;}
 if(s->proxy_held){if(o->proxy_release(o->ctx)==1)s->proxy_held=0;else ok=0;}
 if(s->metadata_held&&!s->preserve_metadata){o->release_metadata(o->ctx);s->metadata_held=0;}
 if(!ok)s->poisoned=1;return ok;
}
static int slpi_boot_stop_once(struct slpi_boot_state *s,const struct slpi_boot_ops *o){
 if(s->scm_uncertain){s->poisoned=1;s->preserve_metadata=1;return 0;}
 if(!s->owned_running)return !s->poisoned;
 if(s->shutdown_attempted)return 0;
 s->shutdown_attempted=1;s->last="owned stop";
 /* Stop acknowledgement failure does not transfer ownership away: primary
  * remoteproc still PAS-shuts down its own started DSP after bounded timeout. */
 (void)o->request_owned_stop(o->ctx,SLPI_STOP_LIMIT_MS);
 int result=o->pas_shutdown(o->ctx,SLPI_PAS_ID);
 if(result!=1){if(result<0){s->scm_uncertain=1;s->preserve_metadata=1;}s->poisoned=1;s->last="shutdown uncertain: retain power, no repeat";return 0;}
 s->owned_running=0;s->ready=0;s->last="shutdown confirmed";
 return slpi_boot_release_pre_auth(s,o);
}
static int slpi_boot_start_once(struct slpi_boot_state *s,const struct slpi_boot_ops *o,const struct slpi_boot_evidence *e){
 int v;
 if(!s||!e||!slpi_boot_ops_valid(o))return 0;
 if(s->attempted){s->last="repeat refused";return 0;}s->attempted=1;
 if(!e->cold_boot_exclusive||!e->map_verified||!e->native_hashes_verified||!e->proxy_adapter_reviewed||!e->cleanup_hook_registered){s->last="missing reviewed precondition";return 0;}
 s->last="empty SMEM preflight";if(o->empty_smem_preflight(o->ctx)!=1)return 0;
 s->last="native image prepare";if(o->prepare_native_image(o->ctx,SLPI_LOAD_BASE,SLPI_LOAD_BYTES)!=1)return 0;s->metadata_held=1;
 s->last="proxy acquire";v=o->proxy_acquire(o->ctx);
 if(v!=1){if(v<0){s->proxy_held=1;s->poisoned=1;s->last="proxy partial: retain, cold reboot";return 0;}goto safe_fail;}s->proxy_held=1;
 s->last="PAS12 header";v=o->pas_init_header(o->ctx,SLPI_PAS_ID);if(v<0)goto uncertain_smc;if(v!=1)goto safe_fail;
 s->last="PAS12 memory";v=o->pas_mem_setup(o->ctx,SLPI_PAS_ID,SLPI_LOAD_BASE,SLPI_LOAD_BYTES);if(v<0)goto uncertain_smc;if(v!=1)goto safe_fail;
 s->last="copy and zero BSS";if(o->copy_verify_publish_image(o->ctx)!=1)goto safe_fail;
 s->last="owned SMP2P publish";if(o->publish_owned_smp2p(o->ctx)!=1)goto safe_fail;
 s->last="AOP slpi on";if(o->aop_load_state(o->ctx,1)!=1)goto safe_fail;s->load_state_on=1;
 s->last="PAS12 auth/reset";s->auth_attempted=1;
 v=o->pas_auth_reset(o->ctx,SLPI_PAS_ID);if(v<0)goto uncertain_smc;
 if(v!=1){s->poisoned=1;s->preserve_metadata=1;s->last="auth terminal failure: conservatively retain; cold reboot";return 0;}
 s->owned_running=1;s->last="bounded READY wait";
 if(o->wait_ready(o->ctx,SLPI_READY_LIMIT_MS)!=1){(void)slpi_boot_stop_once(s,o);return 0;}
 s->ready=1;s->last="READY; ownership retained until stop";return 1;
uncertain_smc:
 s->scm_uncertain=1;s->poisoned=1;s->preserve_metadata=1;s->last="SCM unfinished: retain all, no cleanup/PAS retry; cold reboot";return 0;
safe_fail:
 (void)slpi_boot_release_pre_auth(s,o);return 0;
}
#endif
