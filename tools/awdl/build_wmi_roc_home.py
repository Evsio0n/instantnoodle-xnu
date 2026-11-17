"""Require real ROC grants for data, including the infrastructure channel.

The ordinary WMI fanout control submitted same-channel frames without a
firmware ROC transaction; completions were delayed until carrier teardown.
This candidate removes that optimization and gates the asynchronous ROC
worker polling on an actual foreign-channel grant. Packet bytes, raw WMI ABI,
peer selection, PHY limits and receive handling remain the pinned control.
Deferred WMI submissions also recheck the real grant before assigning DMA
ownership, so a late worker cannot fall back to the home-beacon shortcut.
A hardware experiment must establish whether same-channel ROC grants work.
"""
import json
import subprocess
from build_roc import BUS, OUT, read, write, once, sha
BASE = '/tmp/awdl-wmi-fanout-fi8ce5'
BASE_SHA = '8b8dcbd478aafe3e21b98eeb3ae43f4ef19765431b20217ff9fa68fe8bbda188'

def build():
    obj = BASE + '/xnu-deps/build/ath11k/out/ath11k.o'
    actual = subprocess.check_output(['docker', 'exec', BUS, 'sha256sum', obj], text=True).split()[0]
    if actual != BASE_SHA:
        raise ValueError('ordinary fanout control changed')
    stage = subprocess.check_output(['docker', 'exec', BUS, 'mktemp', '-d', '/tmp/awdl-wmi-roc-home-XXXXXX'], text=True).strip()
    subprocess.run(['docker', 'exec', BUS, 'cp', '-a', BASE + '/.', stage + '/'], check=True)
    source = stage + '/wifi/lx/src/auto_owner.inc'
    before = read(source)
    anchor = '''\t\tif(r->channel && r->channel->def.chan && r->channel->def.chan->center_freq==ch->center_freq) {
\t\t\tlive_tick(r,v,ch,lx_awdl_home_basis(&r->hw));auto_mark_ready();lx_pause_ms(2);continue;
\t\t}'''
    after = once(before, anchor, '''\t\t/* WMI raw-data needs a firmware transaction even when its target
         * matches the infrastructure channel. A fresh home beacon does not
         * grant the secondary VDEV's off-channel data command. Use the same
         * bounded ROC worker and actual event checks as other channels. */''')
    write(source, after)
    data_path = stage + '/wifi/lx/src/data.inc'
    data = read(data_path)
    data = once(data,
                'if (radio->persistent_awdl) live_tick(radio,vif,ch,basis);',
                '''/* The asynchronous ROC worker may not have started yet.
         * Do not let its first idle iteration reuse the home shortcut. */
        if (radio->persistent_awdl) live_tick(radio,vif,ch,basis==1?1:0);''')
    write(data_path, data)
    live_path = stage + '/wifi/lx/src/live_transport.inc'
    live = read(live_path)
    live = once(live,
                'if(!EG(live.armed) || EG(live.stop) || echo.hw!=hw)return 0;',
                'if(!EG(live.armed) || EG(live.stop) || echo.hw!=hw || !lx_awdl_data_foreign(hw))return 0;')
    live = once(live,
                'if(!f || !EG(live.armed) || EG(live.stop) || !EG(live.accept) ||',
                'if(!f || !EG(live.armed) || EG(live.stop) || !EG(live.accept) || !lx_awdl_data_foreign(hw) ||')
    write(live_path, live)
    roc_path = stage + '/wifi/lx/src/roc.inc'
    roc = read(roc_path)
    roc = once(roc,
               '|| home==target) {',
               '|| (home==target && !(radio->persistent_awdl && EG(bw.armed)))) {')
    write(roc_path, roc)
    work = stage + '/xnu-deps/build/ath11k/'
    subprocess.run(['docker', 'exec', BUS, 'rm', work + 'obj/lx_lx_80211.o'], check=True)
    with (OUT / 'wmi-roc-home-build.log').open('w') as log:
        subprocess.run(['docker', 'exec', BUS, 'bash', stage + '/tools/mk-ath11k.sh'], check=True, stdout=log, stderr=subprocess.STDOUT)
    dest = OUT / 'ath11k-wmi-roc-home.o'
    subprocess.run(['docker', 'cp', BUS + ':' + work + 'out/ath11k.o', str(dest)], check=True)
    dest.chmod(0o600)
    receipt = dict(object=str(dest), sha256=sha(dest), stage=stage,
                   source_radio_sha256=BASE_SHA, real_roc_on_home=True,
                   additional_peer=False, fixed_endpoint=False, lifetime_seconds=900)
    (OUT / 'wmi-roc-home-build.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt))

if __name__ == '__main__':
    build()
