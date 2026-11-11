#!/usr/bin/env python3
"""Check one explicitly selected sleep-control log, never the newest log.
Exit 0: control markers pass, 1: explicit failure, 2: missing evidence.
Does not operate the phone or inspect credentials/photos.
"""
import argparse
from pathlib import Path
import re


def check(text):
    bad = re.search(r'wifi-sleep-control: FAIL|t-sleep(?:s)?: FAIL|t-display: (?:off|on) NOT reached|UNCLEAN PCIe DMA OWNERSHIP|panic\(cpu', text)
    if bad:
        return 1, 'FAIL: ' + bad.group(0)
    patterns = [
        r'wifi-sleep-control: BEGIN wifi-OFF control, one 5-second sleep',
        r't-display: off reached',
        r't-sleep: PASS n=1 want=5 ',
        r't-sleeps: PASS 1 of 1 came back as asked',
        r't-display: on reached',
        r'wifi-sleep-control: PASS timer and display checks; not Wi-Fi resume proof',
        r't-mark: done',
    ]
    cursor = 0
    for pattern in patterns:
        match = re.search(pattern, text[cursor:])
        if not match:
            return 2, 'UNKNOWN: missing or out-of-order evidence: ' + pattern
        cursor += match.end()
    return 0, 'PASS: Wi-Fi-OFF timer/display control only; panel appearance and touch require owner observation'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', nargs='?', type=Path)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        good = '\n'.join([
            'wifi-sleep-control: BEGIN wifi-OFF control, one 5-second sleep',
            't-display: off reached after 0.00 s',
            't-sleep: PASS n=1 want=5 cont=5.01 mach=0.1 by=pwrkey cores_off=6/8',
            't-sleeps: PASS 1 of 1 came back as asked',
            't-display: on reached after 0.00 s',
            'wifi-sleep-control: PASS timer and display checks; not Wi-Fi resume proof',
            't-mark: done'])
        assert check(good)[0] == 0
        assert check(good + '\nwifi-sleep-control: FAIL step=4')[0] == 1
        assert check('t-mark: done')[0] == 2
        assert check(good.replace('t-display: on reached', 't-display: off reached'))[0] == 2
        assert check(good.replace('t-sleep: PASS', 't-sleep: FAIL'))[0] == 1
        print('check_control: 5 self-tests PASS (synthetic logs only)')
        return 0
    if args.log is None:
        parser.error('select the exact run log, or --self-test')
    result, message = check(args.log.read_text(errors='replace'))
    print(message)
    return result

if __name__ == '__main__':
    raise SystemExit(main())
