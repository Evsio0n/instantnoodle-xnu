"""A/B candidate: grant each data ROC to the secondary data VDEV.

The control alternates three primary-STA ROCs with one secondary ROC, while
all OFFCHAN_DATA commands name the secondary VDEV. Keep the existing scan
ownership, dwell, firmware event checks and discovery cadence; change only
the data-window owner. Social-channel discovery still uses its primary STA.
"""
import json
import subprocess
from build_roc import BUS, OUT, read, write, once, sha

BASE = '/tmp/awdl-discovery-fair-7uNop1'
BASE_SHA = 'adab74784b0ce2d033a2eb2026f20fdbc956de6e7bc8eda3228db9dd58d5a207'


def build():
    work = BASE + '/xnu-deps/build/ath11k/'
    actual = subprocess.check_output(['docker', 'exec', BUS, 'sha256sum', work+'out/ath11k.o'], text=True).split()[0]
    if actual != BASE_SHA:
        raise ValueError('discovery-fair control changed')
    stage = subprocess.check_output(['docker', 'exec', BUS, 'mktemp', '-d', '/tmp/awdl-vdev-window-XXXXXX'], text=True).strip()
    subprocess.run(['docker', 'exec', BUS, 'cp', '-a', BASE+'/.', stage+'/'], check=True)
    src = stage+'/wifi/lx/src/'
    owner = read(src+'auto_owner.inc')
    owner = once(owner, 'live_infra_window=(++cycles%4)!=0;', '''/* Data and its ROC use the same secondary VDEV. The primary STA
         * retains its infrastructure context and social discovery windows. */
        ++cycles;live_infra_window=0;''')
    write(src+'auto_owner.inc', owner)
    roc = read(src+'roc.inc')
    roc = once(roc, '\tunsigned duration = 500;', '\tuint64_t receipt_begin=lx_os_nanoseconds()/1000ull;\n\tunsigned duration = 500;')
    roc = once(roc, '\tROC_SET(r->active,0);\n\tunsigned retained=', '''	ROC_SET(r->active,0);
    lx_tell("awdl-roc-time: begin_us=%llu end_us=%llu foreign_us=%llu vdev=%u target=%u done=%u home_beacon=%u home_data=%u\\n",
        receipt_begin,lx_os_nanoseconds()/1000ull,ROC_GET(r->foreign_us),r->vdev,target,
        ROC_GET(r->done),ROC_GET(r->home_beacon),ROC_GET(r->home_data));
	unsigned retained=''')
    write(src+'roc.inc', roc)
    work = stage+'/xnu-deps/build/ath11k/'
    subprocess.run(['docker', 'exec', BUS, 'rm', work+'obj/lx_lx_80211.o'], check=True)
    with (OUT/'wmi-vdev-window-build.log').open('w') as log:
        subprocess.run(['docker', 'exec', BUS, 'bash', stage+'/tools/mk-ath11k.sh'], check=True, stdout=log, stderr=subprocess.STDOUT)
    dest = OUT/'ath11k-wmi-vdev-window.o'
    subprocess.run(['docker', 'cp', BUS+':'+work+'out/ath11k.o', str(dest)], check=True)
    dest.chmod(0o600)
    result = dict(object=str(dest), sha256=sha(dest), stage=stage, source_radio_sha256=BASE_SHA,
                  secondary_data_roc=True, social_primary_roc=True, dwell_ms=500,
                  hardware_verified=False)
    (OUT/'wmi-vdev-window-build.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result), flush=True)


if __name__ == '__main__':
    build()
