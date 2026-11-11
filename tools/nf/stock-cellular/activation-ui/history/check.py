"""Exercise the unmodified history producer with retained #282 values, without a device."""
import json
from pathlib import Path
import subprocess
import tempfile
import patch

H=Path(__file__).resolve().parent
K=H.parents[4]
AUDIT=K.parent/'oneplus8-audit/cellular-data-next'
base=AUDIT/'pro-donor-integrated';logs=json.loads((base/'hardware.json').read_text())
assert logs['ticket']==282 and logs['run']=='181704' and logs['fault_observed']
frozen=base/'29c9871d-sources/tools/nf/modem'
for name in ('mdm_rfs_uim.inc','modem_boot.inc'):
    text=(frozen/name).read_text()
    assert patch.apply('tools/nf/modem/'+name,text)!=text
dms=logs['radio_query_dms'][-1]['mode'];nas=logs['radio_query_nas'][-1];wds=logs['radio_query_wds'][-1]['state']
uim=logs['independent_primary_reads'][-1]
online=logs['matched_online_acks'][-1]
# Exact QMI values are from the retained run; the test clock is synthetic.
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "hk_cellular_history.h"
static struct hk_cellular_observation obs;
static unsigned locked;
static uint64_t ms;
#define NSEC_PER_MSEC 1000000ull
#define SYSCTL_HANDLER_ARGS (struct hk_cellular_history *req)
#define SYSCTL_OUT(req,p,n) (memcpy(req,p,n),0)
#define SYSCTL_PROC(...)
#define lck_mtx_lock(p) do{assert(!locked);locked=1;}while(0)
#define lck_mtx_unlock(p) do{assert(locked);locked=0;}while(0)
static uint64_t op_nanoseconds(void *unused){(void)unused;return ms*NSEC_PER_MSEC;}
#include "producer.inc"
int main(void){
    hist_phase_locked(HK_OBS_LIVE);obs.phase=HK_OBS_LIVE;ms=100;
    obs.valid_mask=HK_OBS_NAS|HK_OBS_DMS|HK_OBS_WDS;obs.nas_ms=obs.dms_ms=obs.wds_ms=ms;
    obs.dms_mode=DMSMODE;obs.nas_registration=REG;obs.cs_attach=CS;obs.ps_attach=PS;obs.wds_state=WDS;
    hist_field_locked(obs.valid_mask,ms);
    obs.valid_mask=HK_OBS_CARD|HK_OBS_GW;obs.uim_generation=1;obs.card_ms=++ms;obs.usim_states=1u<<7;
    obs.gw_status=1;obs.gw_slot=SLOT;obs.gw_app=APP;obs.gw_type=2;obs.gw_state=7;hist_field_locked(obs.valid_mask,ms);
    obs.activation=HK_OBS_READBACK_BOUND;obs.verified=1;obs.post_reads=2;obs.verified_ms=ms;hist_uim_locked();
    hist_online_submit(1,TID,true,true,++ms);hist_online_ack(2,TID,true,14,1,52,++ms);
    assert(!hist.online_ack_seen);
    hist_online_ack(1,TID,true,14,RESULT,ERR,++ms);hist_errfatal();uint64_t fault_time=ms;
    obs.valid_mask=0;hist_field_locked(HK_OBS_DMS,++ms);hist_phase_locked(HK_OBS_STOPPED);obs.phase=HK_OBS_STOPPED;
    struct hk_cellular_history r={0};assert(!history_sysctl(&r));
    assert(!obs.valid_mask&&r.phase==HK_OBS_STOPPED&&r.retained&&r.last.dms_mode==DMSMODE);
    assert(r.last.gw_slot==SLOT&&r.last.gw_app==APP&&r.last.usim_states==(1u<<7)&&r.last.verified);
    assert((r.last.valid_mask&(HK_OBS_CARD|HK_OBS_GW|HK_OBS_NAS|HK_OBS_DMS|HK_OBS_WDS))==r.last.valid_mask);
    assert(r.last.nas_registration==REG&&r.last.cs_attach==CS&&r.last.ps_attach==PS&&r.last.wds_state==WDS);
    assert(r.online_attempts==1&&r.online_ack_seen&&r.online_result==RESULT&&r.online_error==ERR);
    assert(r.errfatal_ms==fault_time&&r.stopped_ms==ms&&r.latest_field_ms<r.stopped_ms);
    ++ms;hist_errfatal();assert(hist.errfatal_ms==fault_time);
    hist_phase_locked(HK_OBS_LIVE);assert(!hist.retained&&!hist.online_ack_seen&&!hist.errfatal_ms);
    puts("PASS retained #282 radio/GW/refusal values, wrong-generation ACK rejected, STOP does not relabel live, next mission reset");
}
'''
defines={'DMSMODE':dms,'REG':nas['reg'],'CS':nas['cs'],'PS':nas['ps'],'WDS':wds,'SLOT':uim['primary_slot'],'APP':uim['primary_app'],'TID':online['tid'],'RESULT':online['result'],'ERR':online['error']}
with tempfile.TemporaryDirectory()as work:
    p=Path(work);f=p/'test.c';f.write_text(code)
    subprocess.run(['clang','-std=gnu11','-fsanitize=address,undefined','-fno-sanitize-recover=all','-Wall','-Wextra','-Werror','-I'+str(H),'-I'+str(frozen),*[f'-D{k}={v}' for k,v in defines.items()],str(f),'-o',str(p/'check')],check=True)
    subprocess.run([str(p/'check')],check=True)
