"""Compile the actual NF board configuration into an entirely in-memory MHI test."""
from pathlib import Path
import re
import subprocess
import tempfile

d=Path(__file__).resolve().parent
src=(d/'modem_boot.inc').read_text()
config=src[src.index('static const struct mhi_host_config baseline = {'):]
config=config[:config.index('\n\t};')+4].replace('struct mhi_host_config baseline', 'struct mhi_host_config board_profile',1)
assert 'struct mhi_host_config config = baseline;' in src
macros=['MDM_ADDRESSES','MDM_ADDRESSES_SIZE','MDM_POOL_SIZE','SAHARA_OUT','SAHARA_IN',
        'IPCR_OUT','IPCR_IN','MDM_EFS_OUT','MDM_EFS_IN']
with tempfile.TemporaryDirectory() as name:
    tmp=Path(name)
    lines=[re.search('^#define '+key+r'\s+.+$',src,re.M)[0] for key in macros]
    (tmp/'board-profile.inc').write_text('#include "modem_sahara.h"\n'+'\n'.join(lines)+'\n'+config+'\n')
    subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers',
                    '-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I',str(d),
                    '-I',str(tmp), str(d/'test_ip_hw0.c'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True)
