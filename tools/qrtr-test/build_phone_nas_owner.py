"""Reproduce #267 and #269, then build a poller-owned, bounded read-only NAS trial."""
import hashlib,importlib.util,json,shutil,subprocess,tempfile
from pathlib import Path
H=Path(__file__).resolve().parent;K=H.parents[1];A=K.parent/'oneplus8-audit'
D=A/'stock-cellular-ui/qmux-owner';D.mkdir(exist_ok=True)
mod=importlib.util.spec_from_file_location('uim_source',A/'cellular-data-next/uim-endpoints/prepare.py')
uim=importlib.util.module_from_spec(mod);mod.loader.exec_module(uim)
def change(src,old,new):
    assert src.count(old)==1,old
    return src.replace(old,new)
def legacy(src):
    src=change(src,'static int\nmdm_radio_init(void)',(H/'phone_nas.inc').read_text()+'\nstatic int\nmdm_radio_init(void)')
    src=change(src,'\t\tmdm_radio_query(tenths);','\t\tmdm_radio_query(tenths);\n\t\tqn_tick(tenths);')
    return change(src,'\tuep_close();','\tqn_close("modem_teardown");\n\tuep_close();')
def owner(src):
    src=change(src,'static void\nop_receive(', 'static bool qno_admit_rx(const void *,size_t);\nstatic void\nop_receive(')
    src=change(src,'\t\tint adapter_error = hk_qrtr_mhi_input', '\t\tif(qno_admit_rx(data,length))return;\n\t\tint adapter_error = hk_qrtr_mhi_input')
    src=change(src,'static volatile bool mdm_polling;',(H/'phone_nas_owner.inc').read_text()+'\nstatic volatile bool mdm_polling;')
    src=change(src,'\twhile (mdm_polling) {','''    if(hk_qo_phase(&qno_gate)!=HK_QO_OFF){
        qno_run_poller();thread_terminate(current_thread());return;
    }
\twhile (mdm_polling) {''')
    src=change(src,'\tif (mdm_radio_enabled > 6 || mdm_mission != 1) mdm_radio_enabled = 0;', '''    uint32_t owner_flag=0,wifi_arg=1;
    (void)PE_parse_boot_argn("ui_qmux",&owner_flag,sizeof(owner_flag));
    (void)PE_parse_boot_argn("wifi",&wifi_arg,sizeof(wifi_arg));
    bool owner_trial=(owner_flag==2);
    if(owner_flag && (!owner_trial || mdm_mission!=1 || wifi_arg || mdm_radio_enabled || mdm_loc_enabled ||
       mdm_gnss_enabled || mdm_wds_settings_enabled)){
        say("modem: qmux-owner: conflicting profile; no trial\\n");return;
    }
\tif (mdm_radio_enabled > 6 || mdm_mission != 1) mdm_radio_enabled = 0;''')
    src=change(src,'\tmdm_polling = true;', '\tif(owner_trial && !hk_qo_arm(&qno_gate))return;\n\tmdm_polling = true;')
    a='\t\tthread_deallocate(thread);\n\t}\n\terror = mdm_mhi_to_sbl(&mdm_mhi);'
    src=change(src,a,'''\t\tthread_deallocate(thread);
    } else if(owner_trial){
        (void)hk_qo_no_worker(&qno_gate);
        mdm_polling=false;say("modem: qmux-owner: poller creation failed, never active\\n");goto end;
\t}\n\terror = mdm_mhi_to_sbl(&mdm_mhi);''')
    src=change(src,'\thk_qrtr_mhi_set_up(&mdm_qrtr_mhi, true);','\tif(!owner_trial)hk_qrtr_mhi_set_up(&mdm_qrtr_mhi,true);')
    src=change(src,'\tmdm_gnss_stage = MDM_GNSS_MISSION;', '\tif(owner_trial && !hk_qo_ready(&qno_gate))goto end;\n\tmdm_gnss_stage = MDM_GNSS_MISSION;')
    src=change(src,'\nend:\n\tif (mdm_radio_enabled)', '\nend:\n\tif(owner_trial)qno_drain();\n\tif (mdm_radio_enabled)')
    src=change(src,'\thk_qrtr_mhi_set_up(&mdm_qrtr_mhi, false);','\tif(!owner_trial)hk_qrtr_mhi_set_up(&mdm_qrtr_mhi,false);')
    src=change(src,'\tsay("modem: qrtr adapter: %u packets in,', '\tif(!owner_trial)say("modem: qrtr adapter: %u packets in,')
    return src
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
files,_=uim.variants()
with tempfile.TemporaryDirectory()as folder:
    t=Path(folder)
    for label in ('reference','legacy','owner'):
        for rel,(_,s) in files.items():
            if rel.endswith('modem_boot.inc') and label!='reference':s=(legacy if label=='legacy' else owner)(s)
            p=t/label/rel;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(s)
        extra=[] if label=='reference' else ['bsd/dev/hk_qrtr_mhi.c','bsd/dev/hk_qmux_nas.inc','bsd/sys/hk_qmux_nas.h']
        if label=='owner':extra+=['bsd/sys/hk_abp_nas.h','bsd/dev/hk_abp_nas.c','bsd/sys/hk_qmux_owner.h','bsd/sys/hk_qmux_ingress.h']
        for rel in extra:
            p=t/label/'xnu/files'/rel;p.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(K/'xnu/files'/rel,p)
        if label=='owner':
            # All ABP code is in the one existing QRTR object for this private
            # relink. Compiler include flags/header ABI match the actual device.
            p=t/label/'xnu/files/bsd/dev/hk_qrtr_mhi.c';p.write_text(p.read_text()+'\n#include "hk_abp_nas.c"\n')
    s=(A/'cellular-data-next/wds-observer/link.py').read_text()
    s=change(s,"['ref233','control263','maintained','wds263']","['reference','legacy','owner']")
    a="for unit in [inc/'instantnoodle_modem.c', fs/'dev/hk_qmi_radio.c']:"
    s=change(s,a,"for unit in [inc/'instantnoodle_modem.c',fs/'dev/hk_qmi_radio.c']+([fs/'dev/hk_qrtr_mhi.c'] if label!='reference' else []):")
    a="        cc=flags(bsd/'.CFLAGS')"
    s=change(s,a,'''        if label=='owner':
            p=inc/'instantnoodle_modem.c';board_text=p.read_text();guard='hk_modem_quiesce(void)\\n{'
            assert board_text.count(guard)==1
            p.write_text(board_text.replace(guard,guard+"\\n    unsigned phase=hk_qo_phase(&qno_gate);\\n    if(phase!=HK_QO_OFF && phase!=HK_QO_DRAINED)qno_held();"))
'''+a)
    (t/'link.py').write_text(s);c='oneplus8-build';remote=subprocess.check_output(['docker','exec',c,'mktemp','-d','/tmp/qmux-owner-XXXXXXXX'],text=True).strip();assert remote.startswith('/tmp/qmux-owner-')
    try:
        subprocess.run(['docker','cp',str(t)+'/.',c+':'+remote],check=True)
        with (D/'build.log').open('w')as log:
            subprocess.run(['docker','exec','-e','THEOS=/build/local-theos','-e','W='+remote,c,'bash','-ec','. /build/xnu-env.sh; python3 "$W/link.py" "$W"'],stdout=log,stderr=subprocess.STDOUT,check=True)
        for label in ('reference','legacy','owner'):
            subprocess.run(['docker','cp',c+':'+remote+'/'+label+'.mach',str(t/(label+'.mach'))],check=True)
        ref=sha(t/'reference.mach');legacy_sha=sha(t/'legacy.mach');digest=sha(t/'owner.mach')
        assert ref=='4764013dd575065f92541439739bffa2a3036452d94f59ca387e073bc27025dc'
        assert legacy_sha=='9f997271929650e5bbc854303aba491c4299f8ef08f0480f55989cfe4a40f73e'
        dst=K/'out/kernels'/f'nf-qmux-owner-{digest}.mach'
        if not dst.exists():shutil.copyfile(t/'owner.mach',dst);dst.chmod(0o444)
        assert sha(dst)==digest
        json_data=dict(kernel=str(dst),kernel_sha256=digest,reference_control_267=ref,legacy_control_269=legacy_sha,
                       two_byte_identical_controls=True,required=['ui_qmux=2','wifi=0','modem=3','modem_mission=1','modem_radio=0','modem_loc=0','modem_gnss=0'],
                       max_firmware_reads=2)
        (D/'kernel.json').write_text(json.dumps(json_data,indent=2)+'\n');print(json.dumps(json_data))
    finally:subprocess.run(['docker','exec',c,'rm','-rf','--',remote],check=True)
