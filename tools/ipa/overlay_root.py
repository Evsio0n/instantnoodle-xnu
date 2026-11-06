#!/usr/bin/env python3
"""tools/ipa/overlay_root.py IMAGE DIR: an existing root image's tree into DIR, modes and links as they are, so that a few files can be laid over exactly the root it is (the owner's row) and an
image made again with tools/apfs/mkapfs.py. DIR can hold what IMAGE holds, the network's file included: under out/, and to be deleted after."""
import os
import stat
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "apfs"))
import apfs  # noqa: E402


def main():
    image, out = sys.argv[1], sys.argv[2]
    container = apfs.Container(image)
    volume = apfs.pick_volume(container, "hackios")
    root = volume.lookup("/")
    os.makedirs(out, exist_ok=True)
    files = 0
    for path, _name, fid, dt in apfs.walk(volume, root[0], ""):
        target = out + path
        inode = volume.inode(fid)
        mode = stat.S_IMODE(inode.mode)
        if dt == 4:
            os.makedirs(target, exist_ok=True)
            os.chmod(target, mode | 0o700)
        elif dt == 10:
            os.symlink(inode.contents().rstrip(b"\0").decode(), target)
        else:
            with open(target, "wb") as f:
                f.write(inode.contents())
            os.chmod(target, mode)
            files += 1
    container.close()
    print(files, "files")


if __name__ == "__main__":
    main()
