"""Daily kernel: proven cellular/ALS/Settings plus boot-scoped calibration completion."""
import importlib.util,sys,shutil
from pathlib import Path
K=Path(__file__).resolve().parents[4];HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('als_daily',K/'tools/nf/cell/als/build.py');als=importlib.util.module_from_spec(spec);spec.loader.exec_module(als)
def extend(cell,text):
 source=cell/'overlay/xnu/files/bsd/dev/hk_slpi_boot.c';s=source.read_text()
 anchor='static void slpi_load_optional_coefficient(void)'
 assert s.count(anchor)==1
 s=s.replace(anchor,'#include <sys/time.h>\n#include "calibration_session.h"\n'+anchor)
 anchor='''            int left=1;
            if(vnode_vtype(v)==VREG)
                (void)vn_rdwr(UIO_READ,v,(caddr_t)&flag,1,0,UIO_SYSSPACE,0,vfs_context_ucred(ctx),&left,vfs_context_proc(ctx));
            vnode_put(v);
            if(!left && (flag=='0'||flag=='1'))break;'''
 assert s.count(anchor)==1
 replacement='''            int data_mode=0;(void)PE_parse_boot_argn("hk_data",&data_mode,sizeof(data_mode));
            bool daily=data_mode==1;
            struct hk_als_completion completion={0};
            int wanted=daily?(int)sizeof(completion):1,left=wanted;
            if(vnode_vtype(v)==VREG)
                (void)vn_rdwr(UIO_READ,v,daily?(caddr_t)&completion:(caddr_t)&flag,wanted,0,UIO_SYSSPACE,0,vfs_context_ucred(ctx),&left,vfs_context_proc(ctx));
            vnode_put(v);
            if(daily && !left){struct timeval boot;boottime_timeval(&boot);
                if(hk_als_completion_matches(&completion,boot.tv_sec,boot.tv_usec))flag=completion.result;
                else left=1;
            }
            if(!left && (flag=='0'||flag=='1')){tell("slpi-cal: completion current_boot=1 persistent=%u result=%c\\n",daily,flag);break;}'''
 source.write_text(s.replace(anchor,replacement));shutil.copyfile(HERE/'calibration_session.h',source.parent/'calibration_session.h');return text
als.main(['--settings','--sim-state-patch',str(K/'tools/nf/cell/sim/sim-state.patch'),'--output',str(K/'out/daily-20261115/kernel.txt')],extend_private_link=extend)
