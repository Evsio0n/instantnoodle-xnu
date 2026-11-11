#ifndef HK_CELLULAR_OBSERVATION_H
#define HK_CELLULAR_OBSERVATION_H
#include <stdint.h>
#define HK_CELLULAR_OBSERVATION_NAME "kern.hackios.cellular_observation"
#define HK_CELLULAR_OBSERVATION_VERSION 1u
enum hk_obs_phase { HK_OBS_OFF,HK_OBS_LIVE,HK_OBS_STOPPED };
enum hk_obs_valid { HK_OBS_PHYSICAL=1u, HK_OBS_CARD=2u, HK_OBS_ATR=4u, HK_OBS_DMS=8u,
    HK_OBS_NAS=16u, HK_OBS_WDS=32u, HK_OBS_GW=64u };
enum hk_obs_act { HK_OBS_NOT_SUBMITTED,HK_OBS_ATTEMPT_UNCERTAIN,HK_OBS_SUBMITTED,
    HK_OBS_MODEM_ACK,HK_OBS_MODEM_ERROR,HK_OBS_ALREADY_BOUND,HK_OBS_READBACK_BOUND };
struct hk_cellular_observation {
    uint32_t version,size,sequence,phase,valid_mask,round,uim_generation,card_validity;
    /* Bit masks use the raw QMI GW application state (1=DETECTED -> mask 2;
     * 2=PIN_REQUIRED -> mask 4; 7=READY -> mask 128). No selectors or IDs. */
    uint32_t physical_slot,logical_slot,card_present,slot2_empty;
    uint32_t card_state,apps,sim_states,usim_states,ready_gw,atr_present,atr_length;
    /* gw_status: 0 unassigned, 1 mapped, 2 invalid, 3 outside projection.
     * gw_slot/gw_app are 1-based only for MAPPED. */
    uint32_t gw_status,gw_slot,gw_app,gw_type,gw_state;
    uint32_t dms_mode,nas_registration,cs_attach,ps_attach,wds_state;
    /* Ack status is meaningful only with ack_seen. "submitted" is the local
     * TX return, not a completed firmware transition. verified is an independent
     * post-decision Get Card Status matching slot 1/the actual USIM index/type. */
    uint32_t activation,attempted,submitted,ack_seen,ack_result,ack_error,verified,post_reads;
    /* Age calculation: now_ms and field times are measured by the SAME kernel
     * clock; never compare to wall time. Valid bits clear on error, source loss
     * or stop. A large age is an old observation, not a new read. */
    uint64_t now_ms,physical_ms,card_ms,atr_ms,dms_ms,nas_ms,wds_ms,decision_ms,verified_ms;
};
#endif
