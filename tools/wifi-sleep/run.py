#!/usr/bin/env python3
"""Source-backed host contracts only. No hardware, kernel build, or external writes."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def function(path, name):
    """Extract definition verbatim; fail on ambiguous/missing signatures or braces.

    This small lexer counts braces outside comments/strings, not a C parser.
    Compilation and contract cases provide further fail-closed validation.
    """
    text = (ROOT / path).read_text()
    matches = list(re.finditer(r"(?m)^" + re.escape(name) + r"\([^;]*?\)\s*\{", text))
    if len(matches) != 1:
        raise RuntimeError(f"{path}: expected exactly one definition of {name}")
    start = matches[0].start()
    opening = text.index("{", start)
    tokens = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
    depth = 0
    for token in tokens.finditer(text, opening):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if depth == 0:
                return text[start:token.end()]
    raise RuntimeError(f"unterminated definition: {name}")


def run(args, build):
    print('+', ' '.join(map(str, args)), flush=True)
    env = dict(os.environ, TMPDIR=str(build))
    subprocess.run(list(map(str, args)), cwd=build, env=env, check=True)


def main():
    sources = ['wifi/lx/src/lx_bus.c', 'wifi/lx/src/lx_pci.c',
               'wifi/lx/src/lx_mhi.c', 'wifi/lx/src/mhi_host.c',
               'tools/mhi-test/test_mhi.c', 'xnu/files/bsd/dev/hk_wifi.c',
               'wifi/lx/src/lx_base.c']
    before = {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in sources}
    # Deliberately preserved per-run directories, no rm or writes outside ownership.
    build = Path(tempfile.mkdtemp(prefix='run-', dir=HERE))
    cc = os.environ.get('CC', 'cc')
    flags = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-parameter', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all']
    template = (HERE / 'contracts.c.in').read_text()
    bodies = {
        '@RESUME@': 'int\n' + function(sources[0], 'lx_resume'),
        '@SUSPEND@': 'int\n' + function(sources[0], 'lx_suspend'),
        '@STOP@': 'void\n' + function(sources[0], 'lx_stop'),
        '@RESTORE@': 'static void\n' + function(sources[1], 'lx_pci_restore'),
        '@RESUMED@': 'void\n' + function(sources[1], 'lx_pci_resumed'),
        '@MHI_SYNC@': 'int\n' + function(sources[2], 'mhi_sync_power_up'),
        '@MHI_ASYNC@': 'int\n' + function(sources[2], 'mhi_async_power_up'),
    }
    for marker, body in bodies.items():
        template = template.replace(marker, body)
    (build / 'contracts.c').write_text(template)
    run([cc, *flags, build / 'contracts.c', '-o', build / 'contracts'], build)
    run([build / 'contracts'], build)
    fixture = ROOT / 'tools/mhi-test/test_mhi.c'
    mhi = (HERE / 'reset.c.in').read_text().replace('@FIXTURE@', str(fixture))
    (build / 'reset.c').write_text(mhi)
    run([cc, *flags, '-pthread', '-I' + str(ROOT / 'wifi/lx/src'),
         build / 'reset.c', ROOT / 'wifi/lx/src/mhi_host.c', '-o', build / 'reset'], build)
    run([build / 'reset'], build)
    after = {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in sources}
    if before != after:
        raise RuntimeError('Sources changed concurrently: discard mixed-generation result and rerun')
    result = {'scope': 'host-only extracted real function bodies + real MHI host/software device',
              'source_sha256': before, 'result': 'PASS',
              'hardware_validated': False, 'risk_gate': 'HOLD: no sleep/hardware approval inferred'}
    (build / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS host contracts only; hardware gate HOLD. Evidence:', build / 'result.json')


if __name__ == '__main__':
    main()
