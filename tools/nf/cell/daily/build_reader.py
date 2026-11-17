"""Read-only per-handset calibration acquisition for RAM and kept HFS data."""
import hashlib,json,shutil,subprocess,tempfile
from pathlib import Path
K=Path(__file__).resolve().parents[4];HERE=Path(__file__).resolve().parent
SRC=HERE.parent/'als/reader-base';OUT=K/'out/daily-20261115';OUT.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(dir=OUT) as tmp:
 p=Path(tmp);shutil.copyfile(SRC/'cali_ext4.h',p/'cali_ext4.h');shutil.copyfile(HERE/'calibration_session.h',p/'calibration_session.h');shutil.copyfile(HERE.parent/'als/cal_receipt.h',p/'cal_receipt.h')
 h=(p/'cali_ext4.h').read_text();old='!(fs->inc & ~(2u|64u|128u|512u|0x2000u))';assert h.count(old)==1
 (p/'cali_ext4.h').write_text(h.replace(old,'('+old+'||(fs->inc==0x46 && fs->ro==0x7b && ce32(b+92)==0x3c))'))
 s=(SRC/'read_cali_coe.c').read_text();s=s.replace('#include <unistd.h>','#include <unistd.h>\n#include <sys/time.h>\n#include "calibration_session.h"')
 old='''        exact(args,"board","instantnoodle") && exact(args,"hk_data","2") &&
        exact(args,"ufs_write","0") && exact(args,"var_store","0") && exact(args,"var_data","hfs");'''
 assert s.count(old)==1
 s=s.replace(old,'''        exact(args,"board","instantnoodle") && exact(args,"cell","1") && exact(args,"als","1") && exact(args,"ufs","2") && exact(args,"var_data","hfs") &&
        ((exact(args,"hk_data","2") && exact(args,"ufs_write","0") && exact(args,"var_store","0")) ||
         (exact(args,"hk_data","1") && exact(args,"ufs_write","1") && (exact(args,"var_store","0")||exact(args,"var_store","1"))));''')
 s=s.replace('static int acquire_file(', '#include "cal_receipt.h"\nstatic int acquire_file(')
 old='        if(write_private(data,size)){' ;assert s.count(old)==1
 s=s.replace(old,'        if(als_current_capture(&fs,data,size) && write_private(data,size)){')
 a=s.index('    int fd=open("/private/var/root/.slpi-cali-checked"');b=s.index('    return result;',a)
 s=s[:a]+'''    struct timeval boot={0};size_t bytes=sizeof(boot);
    if(sysctlbyname("kern.boottime",&boot,&bytes,NULL,0) || bytes!=sizeof(boot))return 4;
    struct hk_als_completion record={.result=result==0?'1':'0',.version=1,.magic=0x534c4143u,.boot_sec=boot.tv_sec,.boot_usec=boot.tv_usec};
    int dir=open("/private/var/root",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(!private_dir(dir)){if(dir>=0)close(dir);return 4;}
    char name[64];snprintf(name,sizeof(name),".slpi-cali-%ld.tmp",(long)getpid());
    int fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    bool done=fd>=0 && write(fd,&record,sizeof(record))==sizeof(record) && fsync(fd)==0;
    if(fd>=0 && close(fd))done=false;
    if(done)done=renameat(dir,name,dir,".slpi-cali-checked")==0;
    if(!done)unlinkat(dir,name,0);close(dir);
    printf("sensor-cali: current_boot_completion=%u persistent_output_supported=1\\n",done);
    if(!done)return 4;
''' +s[b:]
 (p/'read_cali_coe.c').write_text(s)
 remote=subprocess.check_output(['docker','exec','oneplus8-build','mktemp','-d','/tmp/daily-reader-XXXXXX'],text=True).strip()
 try:
  subprocess.run(['docker','cp',str(p)+'/.','oneplus8-build:'+remote],check=True)
  cmd='source /build/xnu-env.sh; xnu-clang -arch arm64 -isysroot /build/local-theos/sdks/iPhoneOS16.5.sdk -Wall -Wextra -Werror -Wno-unused-parameter -O2 read_cali_coe.c -o read_cali_coe; /build/local-theos/toolchain/linux/iphone/bin/ldid -S read_cali_coe'
  subprocess.run(['docker','exec','-w',remote,'oneplus8-build','bash','-ec',cmd],check=True)
  target=OUT/'read_cali_coe';subprocess.run(['docker','cp','oneplus8-build:'+remote+'/read_cali_coe',str(target)],check=True);target.chmod(0o755)
  (OUT/'reader-source.c').write_text(s);(OUT/'reader.json').write_text(json.dumps({'binary':str(target),'sha256':hashlib.sha256(target.read_bytes()).hexdigest(),'source_sha256':hashlib.sha256(s.encode()).hexdigest(),'source_partition_read_only':True,'output':'fixed root-owned file and boot-scoped completion only'},indent=2))
 finally:subprocess.run(['docker','exec','oneplus8-build','rm','-rf',remote],check=True)
