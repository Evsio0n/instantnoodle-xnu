"""Build only a private, USB-only, RAM-log developer image inside the audited IN2010 container."""
from pathlib import Path
import hashlib
import os
import shutil
import struct
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
OUTPUT = ROOT / 'out/portable-gnss'
BASE_SHA = '5fea3ea2683e6555b90602cc0a19272705d24fb2af9d12f009ed8edca20853b4'


def need(ok, why):
    if not ok:
        raise ValueError(why)


def v2_id(image):
    """Android boot v2 id covers each component and its little-endian length."""
    size = lambda off: struct.unpack_from('<I', image, off)[0]
    page = size(36)
    need(image[:8] == b'ANDROID!' and page == 4096 and size(40) == 2, 'unknown container')
    pos = page
    result = hashlib.sha1()
    for offset in (8, 16, 24, 1632, 1648):
        length = size(offset)
        need(pos + length <= len(image), 'bad boot image geometry')
        result.update(image[pos:pos + length])
        result.update(struct.pack('<I', length))
        pos += (length + page - 1) // page * page
    return result.digest()


def repack(original, previous_efi, efi):
    need(hashlib.sha256(original).hexdigest() == BASE_SHA, 'developer container changed')
    need(original[576:596] == v2_id(original), 'original v2 id mismatch')
    image = bytearray(original)
    offset = image.index(previous_efi)
    need(image.count(previous_efi) == 1 and offset + len(previous_efi) <= 4096 + struct.unpack_from('<I', image, 8)[0]
         and image[offset-32:offset-28] == b'XNUP' and image[offset-24:offset-14] == b'loader.efi'
         and struct.unpack_from('<I', image, offset-8)[0] == len(previous_efi), 'unknown embedded payload')
    need(efi.startswith(b'MZ') and len(efi) <= len(previous_efi), 'EFI exceeds fixed room')
    # Trailing padding in a PE file does not change the image loaded by UEFI.
    image[offset:offset+len(previous_efi)] = efi.ljust(len(previous_efi), b'\0')
    image[576:596] = v2_id(image)
    return image


def main():
    os.umask(0o077)
    base = ROOT / 'xnuloader'
    OUTPUT.mkdir(mode=0o700, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=OUTPUT) as temp:
        work = Path(temp)
        shutil.copyfile(base/'loader.c', work/'loader.c')
        subprocess.run(['git', 'apply', '--directory=.', str(HERE/'ram-loader.patch')], cwd=work, check=True,
                       env={**os.environ, 'GIT_CEILING_DIRECTORIES': str(OUTPUT)})
        patched = (work/'loader.c').read_text()
        need('efi_block_io *bio = find_log_partition();\n\n\tcompose_logs' not in patched
             and 'block_write_requested |=' not in patched and 'hc_requested |=' not in patched
             and 'fb_standalone();\n\treboot();' not in patched
             and 'ram-log-v1' in patched, 'private patch was not fully applied')
        obj, efi = work/'loader.obj', work/'portable.efi'
        subprocess.run(['clang', '--target=aarch64-unknown-windows', '-ffreestanding', '-fno-builtin',
                        '-fno-stack-protector', '-fshort-wchar', '-mgeneral-regs-only', '-O1',
                        '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-unused-function',
                        '-Wno-unused-variable', '-I', base, '-c', 'loader.c', '-o', obj], cwd=work, check=True)
        link = shutil.which('lld-link')
        if not link:
            candidates = list((Path.home()/'.rustup/toolchains').glob('stable-*/lib/rustlib/*/bin/gcc-ld/lld-link'))
            need(len(candidates) == 1, 'put lld-link on PATH')
            link = candidates[0]
        subprocess.run([link, '/subsystem:efi_application', '/entry:efi_main', '/nodefaultlib',
                        '/timestamp:0', '/base:0xA1000000', '/out:'+str(efi), obj], check=True)
        image = repack((ROOT/'out/instantnoodle-fwreader.img').read_bytes(),
                       (base/'loader-fwreader.efi').read_bytes(), efi.read_bytes())
        digest = hashlib.sha256(image).hexdigest()
        dest = OUTPUT / ('loader-'+digest+'.img')
        tmp = work/'portable.img'
        tmp.write_bytes(image)
        tmp.replace(dest)
        print(dest)


if __name__ == '__main__':
    main()
