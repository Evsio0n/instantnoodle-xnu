"""Join decoded pre-filter scan events to owned ROC requests using device time."""
import argparse
import bisect
import collections
import json
import re
from pathlib import Path

def analyze(path):
    raw = {}
    grants = {}
    for line in path.read_text(errors="replace").splitlines():
        for tag, store, key in (("awdl-wmi-raw:", raw, "n"), ("awdl-grant:", grants, "token")):
            if tag not in line:
                continue
            fields = {k:int(v) for k,v in re.findall(r"(\w+)=(-?\d+)", line.split(tag,1)[1])}
            required = ("n","us","type","req","scan","vdev","freq","reason") if store is raw else ("token","submit","end","foreign","ch")
            if all(k in fields for k in required):
                store[fields[key]] = fields
    events = sorted(raw.values(),key=lambda row:row["us"])
    timestamps = [e["us"] for e in events]
    result = []
    for g in sorted(grants.values(),key=lambda row:row["submit"]):
        freq = 2407+5*g["ch"] if g["ch"]<14 else 5000+5*g["ch"]
        a=bisect.bisect_left(timestamps,g["submit"])
        b=bisect.bisect_right(timestamps,g["end"])
        window=events[a:b]
        owned=[e for e in window if (e["req"],e["scan"],e["vdev"])==(1,40960,1)]
        contiguous=all(y["n"]==x["n"]+1 for x,y in zip(window,window[1:]))
        complete=bool(owned and owned[0]["type"]==1 and owned[-1]["type"]==2 and contiguous)
        correct=any(e["type"]==8 and e["freq"]==freq for e in owned)
        if not complete: state="incomplete"
        elif correct: state="accepted" if g["foreign"] else "matching_event_lost"
        elif g["foreign"]: state="inconsistent"
        elif any(e["type"]==8 for e in owned): state="other_frequency"
        else: state="no_foreign_event"
        result.append(dict(token=g["token"],channel=g["ch"],state=state,events=window))
    return dict(counts=collections.Counter(r["state"] for r in result),requests=result)

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace",type=Path)
    parser.add_argument("output",type=Path)
    args=parser.parse_args()
    report=analyze(args.trace)
    args.output.write_text(json.dumps(report,indent=2))
    print(report["counts"])
