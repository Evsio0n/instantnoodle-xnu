"""Independently measure Settings' on-disk app, LS state and an optional user-selected public handoff."""
import re
import gnss_discovery

STOCK = re.compile(rb'gnss-settings-ls seq=(\d+) uid=(\d+) stock_plist=([01]) stock_id=([01]) stock_exec=([01]) proxy_id=([01]) proxy_installed=(-?\d+) proxy_path=(stock|other|none) by_url=([01]) end')
ACTION = re.compile(rb'gnss-settings-action seq=(\d+) uid=(\d+) pid=(\d+) attempt=(\d+) tap_ms=(\d+) end_ms=(\d+) accepted=(-?\d+) end')
SCENE = re.compile(rb'new scene host settings:[^\r\n]{0,140}<sceneID:(com\.hackios\.gnss-monitor|com\.apple\.Preferences)(?:-[^>]{0,100})?> [^\r\n]{0,30}?-> foreground')
def store(rows,index,value):
    if index in rows and rows[index]!=value:raise ValueError('conflicting Settings evidence')
    rows[index]=value
def collect(files,manifest):
    facts=gnss_discovery.collect(files,manifest)
    stocks={};actions={};saw_settings=False;handoff_scene=False
    for path in files:
        raw=path.read_bytes()
        for match in STOCK.finditer(raw):
            seq,uid,plist,bid,exe,proxy,installed=map(int,match.groups()[:7])
            if not 0<=seq<7 or uid!=501 or installed not in (-1,0,1):raise ValueError('invalid Settings observation')
            store(stocks,seq,dict(plist=bool(plist),bundle_id_matches=bool(bid),executable=bool(exe),proxy_id_matches=bool(proxy),
                                  installed=installed,registered_path=match[8].decode(),by_url=match[9]==b'1'))
        for match in ACTION.finditer(raw):
            seq,uid,pid,attempt,tap,ended,accepted=map(int,match.groups())
            if (not 0<=seq<7 or uid!=501 or not pid or not attempt or not tap or accepted not in (-1,0,1)
                or (accepted==-1 and ended!=0) or (accepted!=-1 and ended<tap)):
                raise ValueError('invalid public Settings action')
            store(actions,seq,dict(pid=pid,attempt=attempt,tap_ms=tap,end_ms=ended,accepted=accepted))
        in_own_scene=False
        for m in SCENE.finditer(raw):
            if m[1]==b'com.hackios.gnss-monitor':
                in_own_scene=True
            else:
                saw_settings=True
                handoff_scene|=in_own_scene
                in_own_scene=False
    owners={int(i):v for i,v in facts['ls_samples'].items()}
    qualified={i for i,s in owners.items() if s['identifier'] and s['installed']==1 and s['signed_bundle_matches']
               and gnss_discovery.OWN.fullmatch(s['path'])}
    measured=[a for i,a in sorted(actions.items()) if i in qualified]
    if len(measured)!=len(actions):raise ValueError('action from unattributed app process/container')
    latest=stocks[max(stocks)] if stocks else {}
    disk=latest.get('plist') is True and latest.get('bundle_id_matches') is True and latest.get('executable') is True
    reg=latest.get('proxy_id_matches') is True and latest.get('installed')==1 and latest.get('registered_path')=='stock'
    completed=[a for a in measured if a['accepted']!=-1]
    answer=('accepted' if completed[-1]['accepted']==1 else 'refused') if completed else ('pending' if measured else 'untested')
    facts.update(settings_samples={str(i):s for i,s in sorted(stocks.items())},settings_bundle_on_disk=disk,
                 settings_proxy_registered=reg,settings_by_url=latest.get('by_url') is True,settings_scene_seen=saw_settings,
                 signed_app_registered=bool(qualified),signed_app_foreground=bool(qualified) and facts['scene_foreground'],
                 settings_user_action=bool(measured),settings_public_open=answer,
                 settings_public_handoff_observed=bool(qualified and facts['scene_foreground'] and disk and reg and
                                                        answer=='accepted' and handoff_scene))
    return facts
