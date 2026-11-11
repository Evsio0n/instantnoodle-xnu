"""Promote measured sensor milestones into the local 8765 feature matrix."""
import fcntl
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT / 'tools/metal/loop'))
import release_catalog as catalog

def milestone(text):
    marker = '== KERNEL LOG (the kernel is still running) =='
    if marker in text:
        text = text.rsplit(marker,1)[1]
    # These are bring-up milestones on a private RAM kernel, not proof that
    # automatic brightness is present in the selected daily integration build.
    if 'slpi-attach: peer_stopped=1' not in text or not re.search(r'slpi-route: complete [^\n]*released=1',text):
        return None
    rows = re.findall(r'slpi-std: call=\d+ handle=7310 sc=([0-9a-f]+) bytes=\d+ result=(\d+) reply=\d+ path=([^\s]*)',text)
    successes={(sc,path) for sc,result,path in rows if result=='0'}
    if 'slpi-see: late inventory after registry activity' in text:
        # Complete data events carry the datatype and SUID. ACKs alone are insufficient.
        import importlib.util
        decoder=ROOT.parent/'oneplus8-audit/als-native/glink/read_inventory_events.py'
        spec=importlib.util.spec_from_file_location("inventory_evidence",decoder)
        mod=importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        try:
            events=mod.decode(text)["events"]
        except (ValueError,AssertionError):
            events=[]
        physical=[e for e in events if e["datatype"]=="ambient_light" and e["suids"]]
        if physical:
            return 10,"已取得環境光 SUID；仍待確認前置來源、有效 lux 與面板反應。"
        if ("slpi-oem130: restore ok=1 scoped_words=17 owned=0" in text
                and re.search(r'slpi-deps: name=wise_light reports=[1-9]\d* count=[1-9]\d* ticks=\d+', text)
                and any(e["datatype"]=="proximity" and e["suids"] for e in events)):
            return 9,"補入原廠板級資料後，DSP 已註冊 wise_light 與近接。前置光感的實際 lux 及自動調光待驗證。"
        if any(e["datatype"]=="registry" and e["suids"] for e in events):
            if "slpi-reg: sent=f ack_ok=f ack_fail=0 events=f invalidated=0" in text:
                return 7,"DSP 已讀回四組光感設定；實體環境光識別碼與 lux 仍待驗證。"
            return 6,"registry 服務已註冊；實機查詢的環境光清單仍為空。"
    vendor_read = any(sc=='13050100' and path.startswith('/vendor/etc/sensors/config/') for sc,path in successes)
    odm_read = any(sc=='13050100' and path.startswith('/odm/etc/sensor/config/') for sc,path in successes)
    generated = any(sc=='1f030000' and '/registry/' in path for sc,path in successes)
    if vendor_read and odm_read and generated:
        return 5,'DSP 已讀取 vendor 與 ODM 設定，並產生 RAM registry 群組。實際 lux 仍待驗證。'
    if odm_read and generated:
        return 4,'DSP 已讀取 ODM 設定，並產生 RAM registry 群組。光感測器註冊待驗證。'
    if ('13050100','/mnt/vendor/persist/sensors/registry/registry/../temp.json') in successes and any(
        path.endswith('.json') and path.startswith('/odm/etc/sensor/config/') for sc,path in successes if sc=='13050100'):
        return 3,'DSP 已開啟原機感測器 JSON，並在 RAM registry 建立暫存檔。光照讀值待驗證。'
    if ('13050100','/sys/devices/soc0/revision') in successes:
        return 2,'DSP 已讀取原機設定及硬體識別值，正在驗證 registry 初始化。'
    if ('13050100','/vendor/etc/sensors/sns_reg_config') in successes:
        return 1,'DSP 已透過 apps_std 開啟原機設定檔，正在驗證硬體識別值。'
    return None

def record(ticket,log):
    result=milestone(log.read_text(errors='replace'))
    if result is None:return
    rank,detail=result
    with (catalog.EYE/'release.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        current=catalog.read()
        feature=next(f for f in current['features'] if f['id']=='als')
        if feature['state'] in ('verified','partial') or rank<feature.get('bringup_rank',0):return
        if rank==feature.get('bringup_rank',0) and ticket<=feature.get('bringup_ticket',0):return
        feature.update(state='lab',detail=detail,evidence=f'RAM #{ticket} · {log.name[:6]}',
                       bringup_ticket=ticket,bringup_rank=rank)
        catalog.write_feature(current, feature)

if __name__=='__main__':record(int(sys.argv[1]),Path(sys.argv[2]))
