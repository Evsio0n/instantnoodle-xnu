"""Join bounded native HTTP, WMI and Mac service observations.

The receiver observer samples realtime, CLOCK_MONOTONIC (BPF probe clock),
and scaled mach_absolute_time (radio clock) together. Keep the raw samples
and use the nearest one; this avoids assuming those three clocks share an
epoch. A firmware completion is not proof of remote RF receipt.
"""
import argparse
import bisect
import json
import re
from pathlib import Path


def timeline(receiver, radio, ptr, packets=(), instance=None):
    anchors = []
    rows = []
    for line in receiver.read_text().splitlines():
        m = re.match(r"wall=([\d.]+) mono=([\d.]+) absolute_us=(\d+) (.*)", line)
        if not m:
            continue
        wall, mono, absolute, event = m.groups()
        a = dict(wall=float(wall), mono=float(mono), absolute_us=int(absolute))
        anchors.append(a)
        rows.append(dict(wall=a["wall"], source="native_receiver", event=event, anchor=a))
    if not anchors:
        raise ValueError("no verified triple-clock anchor; cannot correlate clocks")
    def convert(value, clock, scale):
        anchor = min(anchors, key=lambda a: abs(a[clock] - value))
        return anchor["wall"] + (value-anchor[clock])/scale
    for line in radio.read_text(errors="replace").splitlines():
        m = re.search(r"(awdl-(?:wmi-raw|grant|slot|mif-tx|mif-done|visit):) (.*)", line)
        if not m:
            continue
        tag, event = m.groups()
        f = {k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)", event)}
        clock = next((k for k in ("us", "issue", "submit", "time") if k in f), None)
        if clock:
            rows.append(dict(wall=convert(f[clock], "absolute_us", 1e6), source="radio", event=tag+event, fields=f))
    for line in ptr.read_text().splitlines():
        item = json.loads(line)
        if instance and item.get("instance") != instance:
            continue
        rows.append(dict(wall=item["time"], source="mac_ptr", event=item))
    for path in packets:
        for line in path.read_text().splitlines():
            try:
                item = json.loads(line)
            except json.JSONDecodeError:
                continue  # A live tail can end in an incomplete JSON line.
            if "time" in item and "source_ip" in item:
                rows.append(dict(wall=convert(item["time"], "mono", 1), source="phone_bpf", event=item))
    return dict(instance=instance, anchors=anchors,
                limitations=["host/phone wall-clock offset is not independently calibrated",
                             "nearest sampled clock conversion; preserve possible clock steps",
                             "BPF timestamps are userspace observation times, not RF timestamps",
                             "raw event and packet capture coverage remains bounded"],
                events=sorted(rows, key=lambda r: r["wall"]))


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("receiver", type=Path)
    p.add_argument("radio", type=Path)
    p.add_argument("ptr", type=Path)
    p.add_argument("output", type=Path)
    p.add_argument("--packets", nargs="*", type=Path, default=[])
    p.add_argument("--instance")
    args = p.parse_args()
    report = timeline(args.receiver, args.radio, args.ptr, args.packets, args.instance)
    args.output.write_text(json.dumps(report, indent=2)+"\n")
    print(json.dumps(dict(events=len(report["events"]), anchors=len(report["anchors"]))))
