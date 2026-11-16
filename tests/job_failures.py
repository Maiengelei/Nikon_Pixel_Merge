#!/usr/bin/env python3
"""Exercise cancellation, destination protection and incomplete-sequence handling on real inputs."""
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[1]
sources=sorted(Path(sys.argv[1]).glob("*.NEF"))
assert len(sources)==4, "pass the four-frame sample folder"
cli=root/"build"/"nikon-merge"
with tempfile.TemporaryDirectory() as tmp:
    d=Path(tmp)
    out=d/"another.nefx"
    out.write_bytes(b"existing image")
    def merge(*frames):
        return subprocess.run([cli,"merge",*frames,"--output",out],capture_output=True)
    assert merge(*sources).returncode and out.read_bytes()==b"existing image", "must not overwrite"
    out.unlink()
    missing=merge(*sources[1:])
    assert missing.returncode and "缺帧：1" in missing.stderr.decode() and not out.exists()
    # File chosen during a merge must also survive the atomic publish step.
    race=subprocess.Popen([cli,"merge",*sources,"--ca","off","--output",out],stderr=subprocess.PIPE)
    for msg in race.stderr:
        if b"0%" in msg:
            out.write_bytes(b"appeared after start")
            break
    _,_ = race.communicate(timeout=120)
    assert race.returncode and out.read_bytes()==b"appeared after start"
    out.unlink()
    proc=subprocess.Popen([cli,"merge",*sources,"--ca","off","--output",out],stderr=subprocess.PIPE)
    while True:
        msg=proc.stderr.readline()
        assert msg, "job stopped before reaching merge"
        if "正在读取并合并".encode() in msg:
            proc.send_signal(signal.SIGINT)
            break
    proc.communicate(timeout=60)
    assert proc.returncode==130 and not list(d.iterdir()), "cancelled job must remove all partial files"
print("Output protection and cancellation passed")
