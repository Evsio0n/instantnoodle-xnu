"""Verify the retained NF linker inputs and apply the two AWDL replacements."""
import hashlib
import json
from pathlib import Path

FILELIST_SHA = '649977f418338c7f0f0ac32767804ed5511b80c4ed4888251e183bd8668d37b9'
ORIGINAL = {
    'bsd/DEVELOPMENT/hk_apple80211.o': 'da1fa8bc9a75d25c67e29b773620092436474ebf41c5e7e0e21325e30b70c3bd',
    'iokit/DEVELOPMENT/HKWLAN.cpo': '3c299211a813705ea2bfe9e092688afea42deb994964aae81b33d871675e73e7',
}
APPEND = 'hk_awdl_ifnet.o'
VFS_SHA = 'cbaa44b8c8523c45a1cb6f3afb3fc1c44445437925df4112d1394eaf59c6ac8a'

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def apply(paths, obj, root):
    folder=root/'stage/awdl_link'
    spec=json.loads((folder/'plan.json').read_text())
    filelist=obj/'link.filelist'
    if sha(filelist)!=FILELIST_SHA:
        raise ValueError('NF link.filelist differs from the c533 control')
    if sha(root/'ath11k-cell.o')!=spec['radio_sha256'] or sha(root/'vfs_attrlist.o')!=VFS_SHA:
        raise ValueError('radio or VFS changed before link')
    replaced={}
    for relative,digest in ORIGINAL.items():
        old=obj/relative
        same_name=[i for i,p in enumerate(paths) if Path(p).name==old.name]
        if len(same_name)!=1 or paths[same_name[0]]!=str(old) or sha(old)!=digest:
            raise ValueError('expected exactly one unmodified '+relative)
        if spec['integrate']:
            replacement=folder/old.name
            if sha(replacement)!=spec['objects'][old.name]:
                raise ValueError('staged object changed: '+old.name)
            paths[same_name[0]]=str(replacement)
            replaced[relative]=spec['objects'][old.name]
    if any(Path(path).name==APPEND for path in paths):
        raise ValueError('AWDL ifnet unit already present')
    if spec['integrate']:
        addition=folder/APPEND
        if sha(addition)!=spec['objects'][APPEND]:
            raise ValueError('staged ifnet unit changed')
        paths.append(str(addition))
    receipt=dict(link_filelist_sha256=FILELIST_SHA,old_object_sha256=ORIGINAL,replaced=replaced,
                 appended=[APPEND] if spec['integrate'] else [],radio_sha256=spec['radio_sha256'],
                 vfs_sha256=VFS_SHA,final_filelist_count=len(paths))
    (root/'awdl-link-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
