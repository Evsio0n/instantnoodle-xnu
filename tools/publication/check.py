#!/usr/bin/env python3
"""Check the staged publication without printing any matching secret values.

Run before committing. Gitleaks is a separate required review step: this gate
adds path, file type, local-path and high-confidence credential checks; it is
not a replacement for a secret scanner or a human diff review.
"""
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
DENIED_PARTS = {'.git', '.userland.git', 'out', 'logs', 'archives', 'backup', 'backups',
                'node_modules', '__pycache__', 'obj', 'oneplus8-audit', 'xnu-src',
                'ref-pro', 'fw-wifi', 'fw-camera', 'fw-audio'}
DENIED_SUFFIXES = {'.pem', '.key', '.p12', '.pfx', '.mobileprovision', '.ipa', '.ipsw',
                   '.apfs', '.img', '.dmg', '.pcap', '.pcapng', '.bin', '.mbn', '.elf',
                   '.o', '.a', '.cpo', '.pyc', '.tar', '.gz', '.zst', '.zip', '.heic'}
PATTERNS = {
    'private-key': re.compile(rb'-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----'),
    'github-token': re.compile(rb'\b(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{30,})\b'),
    'gitlab-token': re.compile(rb'\bglpat-[A-Za-z0-9_-]{20,}\b'),
    'aws-access-key': re.compile(rb'\b(?:AKIA|ASIA)[A-Z0-9]{16}\b'),
    'local-home-path': re.compile(rb'/(?:Users|home)/[A-Za-z0-9_.-]+/'),
    'credential-url': re.compile(rb'https?://[^\s/:]+:[^\s/@]+@'),
}


def findings(path, data, mode):
    p = PurePosixPath(path)
    result = []
    if any(x in DENIED_PARTS or x.startswith('._') for x in p.parts):
        result.append('private-or-generated-directory')
    if p.name.startswith('.env') or p.name in {'owner_key.h', '.DS_Store'}:
        result.append('private-filename')
    if p.suffix.lower() in DENIED_SUFFIXES:
        result.append('binary-or-private-artifact-suffix')
    if mode != '100644' and mode != '100755':
        result.append('not-an-ordinary-file')
    if b'\0' in data:
        result.append('binary-content')
    for rule, pattern in PATTERNS.items():
        if pattern.search(data):
            result.append(rule)
    return result


def main():
    raw = subprocess.check_output(['git', 'diff', '--cached', '--name-only',
                                   '--diff-filter=ACMR', '-z'], cwd=ROOT)
    paths = [x.decode() for x in raw.split(b'\0') if x]
    results = []
    for path in paths:
        data = subprocess.check_output(['git', 'show', ':' + path], cwd=ROOT)
        entry = subprocess.check_output(['git', 'ls-files', '--stage', '--', path], cwd=ROOT)
        mode = entry.decode().split()[0]
        for rule in findings(path, data, mode):
            results.append({'path': path, 'rule': rule})
    print(json.dumps({'staged_files': len(paths), 'findings': results}, indent=2))
    return bool(results)


if __name__ == '__main__':
    sys.exit(main())
