"""Apply the passive history exporter to a private copy of the #282 sources."""
from pathlib import Path
H=Path(__file__).resolve().parent
def once(src,old,new):
    if src.count(old)!=1:raise ValueError('history anchor changed: '+old[:85])
    return src.replace(old,new)
def apply(rel,text):
    if rel.endswith('/mdm_rfs_uim.inc'):
        line='static struct hk_cellular_observation obs={.version=HK_CELLULAR_OBSERVATION_VERSION,.size=sizeof(obs),.phase=HK_OBS_OFF};'
        header=(H/'hk_cellular_history.h').read_text().replace('#include "hk_cellular_observation.h"', '/* v1 ABI included above */')
        text=once(text,line,line+'\n'+header+'\n'+(H/'producer.inc').read_text())
        text=once(text,'lck_mtx_lock(&obs_lock);obs.phase=phase;', 'lck_mtx_lock(&obs_lock);hist_phase_locked(phase);obs.phase=phase;')
        a='obs.card_ms=op_nanoseconds(NULL)/NSEC_PER_MSEC;obs.valid_mask|=HK_OBS_CARD|HK_OBS_GW;obs.sequence++;'
        text=once(text,a,a+'\n\thist_field_locked(HK_OBS_CARD|HK_OBS_GW,obs.card_ms);')
        a='if(bit)obs.sequence++;'
        text=once(text,a,a+'\n\tif(good && bit)hist_field_locked(bit,stamp);')
        a='if(value!=obs.activation){obs.activation=value;obs.decision_ms=now/NSEC_PER_MSEC;obs.sequence++;}'
        text=once(text,a,a+'\n\thist_uim_locked();')
        a='say("modem: gw-online submit msg=46'
        text=once(text,a,'hist_online_submit(d->generation,cold.cmd.txn.id,cold.cmd.attempted,cold.cmd.submitted,now);\n\t'+a)
        a='say("modem: gw-online ACK tid='
        text=once(text,a,'hist_online_ack(ru.c[RUC_DMS].generation,cold.cmd.txn.id,cold.cmd.ack_seen,bytes,cold.cmd.result,cold.cmd.error,now);\n\t\t\t'+a)
    elif rel.endswith('/modem_boot.inc'):
        a='say("modem: MDM2AP ERRFATAL is high: the modem has crashed\\n");'
        if text.count(a)!=2:raise ValueError('expected both genuine ERRFATAL detection sites')
        text=text.replace(a,'hist_errfatal();\n\t\t\t'+a)
    return text
