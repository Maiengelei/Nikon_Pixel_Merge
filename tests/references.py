#!/usr/bin/env python3
"""End-to-end comparison against eight NX Studio files. No photo leaves this machine."""
import argparse
import csv
import os
import sys
from pathlib import Path
import subprocess
import tempfile
import time

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("samples", type=Path, help="root containing {4,8,16,32}Image")
p.add_argument("--output", type=Path, required=True, help="new directory for the CSV, logs and optional example")
p.add_argument("--keep", action="store_true", help="keep the 4-frame CA-on file as a usable example")
p.add_argument("--baseline", type=Path, help="check per-channel errors against the recorded run for these same photos")
p.add_argument("--exact", action="store_true", help="fail if even one raw sample differs")
args = p.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
program = root / "build" / "nikon-merge"
compare = root / "build" / "compare"
mismatches = 0
with (args.output / "metrics.csv").open("w", newline="") as f, tempfile.TemporaryDirectory(dir=args.output) as tmp:
    cols = ["frames","ca","seconds","peak_rss_mib","width","height","channel","mae","rmse","p99","p999","max","exact_pct","within_1_pct","different","more_than_1"]
    rows = csv.DictWriter(f, cols); rows.writeheader()
    for frames in (4, 8, 16, 32):
        folder = args.samples / f"{frames}Image"
        for ca in ("off", "on"):
            tag = f"{frames}f-{ca}"
            name = args.output / "4f-ca.nefx" if args.keep and tag == "4f-on" else Path(tmp)/"out.nefx"
            # Pass only source NEFs to the application; the independent comparator alone sees the reference.
            sources = sorted(folder.glob("*.NEF"))
            if len(sources) != frames:
                raise SystemExit(f"{folder}: expected {frames} source frames")
            start = time.monotonic()
            with (args.output / (tag+".log")).open("w") as log:
                cmd = [program,"merge",*sources,"--ca",ca,"--output",name]
                with subprocess.Popen(cmd,stderr=log,stdout=log) as process:
                    _,status,usage = os.wait4(process.pid,0)
                    process.returncode = os.waitstatus_to_exitcode(status)
                    if process.returncode:
                        raise subprocess.CalledProcessError(process.returncode,cmd)
                rss = round(usage.ru_maxrss / (1024**2 if sys.platform=="darwin" else 1024),1)
            seconds = round(time.monotonic()-start,2)
            ref = folder / f"NEFX_ColorCorrection_{ca.upper()}.NEFX"
            metrics = subprocess.check_output([compare,name,ref],text=True)
            for r in csv.DictReader(metrics.splitlines()):
                rows.writerow(dict(r,frames=frames,ca=ca,seconds=seconds,peak_rss_mib=rss))
                mismatches += int(int(r["max"]) != 0)
            f.flush()
            print(tag, seconds, "s",flush=True)
            if name.parent == Path(tmp): name.unlink()
print("comparison complete:", args.output / "metrics.csv")
if args.baseline:
    def read_metrics(path):
        with path.open(newline="") as src:
            return {(r["frames"],r["ca"],r["channel"]):r for r in csv.DictReader(src)}
    base=read_metrics(args.baseline); actual=read_metrics(args.output/"metrics.csv")
    if base.keys()!=actual.keys(): raise SystemExit("reference set differs from baseline")
    for key,row in actual.items():
        expected=base[key]
        if (row["width"],row["height"]) != (expected["width"],expected["height"]):
            raise SystemExit(f"resolution regression: {key}")
        for metric in ("mae","rmse","max","p99","p999"):
            if float(row[metric])>float(expected[metric]):
                raise SystemExit(f"{key}: {metric} regressed from {expected[metric]} to {row[metric]}")
if args.exact and mismatches:
    raise SystemExit("At least one channel differs from NX Studio")
