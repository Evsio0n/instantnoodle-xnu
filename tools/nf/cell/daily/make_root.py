"""Daily root from owner-accepted installer/ALS/settings root; no AirDrop experiment."""
import hashlib,json,shutil,subprocess,sys,tempfile
from pathlib import Path
K=Path(__file__).resolve().parents[4];OUT=K/'out/daily-20261115';sys.path.insert(0,str(K/'tools/nf/cell/als'));from settings_root import contents,sha
base_receipt=json.loads((K/'out/trollstore-lite/ram-root.json').read_text());base=Path(base_receipt['root']);assert sha(base)==base_receipt['root_sha256']
with tempfile.TemporaryDirectory(dir=OUT,prefix='package-') as t:
 stage=Path(t)/'root';stage.mkdir();before=contents(base,stage)
 receipt=(stage/'fw/als-calibration-249.sha256').read_bytes();assert len(receipt)==32 and receipt==(K/'out/als-calibration-249.sha256').read_bytes()
 shutil.copyfile(OUT/'read_cali_coe',stage/'bin/read_cali_coe');(stage/'bin/read_cali_coe').chmod(0o755)
 removed=set()
 for name in ['LaunchDaemons/com.hackios.front-light-observe.plist','bin/front_light_observe','share/trollstore/iSH.ipa']:
  p=stage/name
  if p.exists():p.unlink();removed.add(name)
 # Keep the accepted installer bootstrap; omit the diagnostic IPA seed
 # to fit the full persistent payload. No application is opened automatically.
 jobs=(stage/'etc/rc.jobs').read_text();jobs='\n'.join(line for line in jobs.splitlines() if '/hackios/share/trollstore/iSH.ipa' not in line)+'\n';(stage/'etc/rc.jobs').write_text(jobs);assert '/hackios/bin/read_cali_coe' in jobs and '/hackios/bin/als_prefbridge' in jobs
 assert 'airdrop' not in (stage/'etc/inject.conf').read_text().lower()
 assert sha(stage/'sbin/init')=='5589c6f5e3480fdd0f4a8b8d658d52a91009235ee83bdbd9dea52a71aac00bf8'
 stage.chmod(0o755);image=Path(t)/'daily.apfs'
 subprocess.run([sys.executable,'tools/apfs/mkapfs.py','-z','--plain','fw','-o',str(image),'hackios='+str(stage)],cwd=K,check=True,stdout=subprocess.DEVNULL)
 after=contents(image);changed={'bin/read_cali_coe','etc/rc.jobs'}|removed
 assert set(after)==set(before)-removed
 assert all(after[n]==v for n,v in before.items() if n not in changed)
 assert after['bin/read_cali_coe'][-1]==sha(OUT/'read_cali_coe')
 target=OUT/'root.apfs';image.chmod(0o600);image.replace(target)
 kernel=Path((OUT/'kernel.txt').read_text().strip());payload=target.stat().st_size+kernel.stat().st_size+(K/'userland/flashboot.apfs').stat().st_size+65536
 assert payload<=63*1024*1024
 r={'base':str(base),'base_sha256':sha(base),'root':str(target),'root_sha256':sha(target),'kernel':str(kernel),'kernel_sha256':sha(kernel),'changed':['bin/read_cali_coe','etc/rc.jobs'],'removed':sorted(removed),'original_entries_verified':len(before)-len(changed),'airdrop_experiment':False,'payload_estimate_bytes':payload,'payload_margin_bytes':63*1024*1024-payload,'stock_services_and_installer_bootstrap_preserved':True}
 (OUT/'root.json').write_text(json.dumps(r,indent=2));print(json.dumps(r,indent=2))
