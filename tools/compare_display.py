#!/usr/bin/env python3
"""Develop the same 1:1 crop in LibRaw for both files. Left: NX Studio; right: ours."""
import argparse
import binascii
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument("dcraw_emu",type=Path); ap.add_argument("reference",type=Path)
ap.add_argument("exported",type=Path); ap.add_argument("png",type=Path)
ap.add_argument("--crop",nargs=4,type=int,metavar=("X","Y","W","H"),default=(2000,1500,512,512))
args=ap.parse_args()
x,y,w,h=args.crop
if min(x,y)<0 or min(w,h)<1: ap.error("invalid crop")
def develop(path,ppm):
    subprocess.run([args.dcraw_emu,"-w","-W","-B",*map(str,args.crop),"-Z",ppm,path],check=True)
    with ppm.open("rb") as f:
        if f.readline().strip()!=b"P6" or f.readline().split()!=[str(w).encode(),str(h).encode()] or f.readline().strip()!=b"255":
            raise ValueError("expected a full-size 8-bit P6 crop from LibRaw")
        pixels=f.read()
    if len(pixels)!=w*h*3: raise ValueError("truncated P6")
    return pixels

def chunk(tag,data):
    return struct.pack(">I",len(data))+tag+data+struct.pack(">I",binascii.crc32(tag+data))
with tempfile.TemporaryDirectory() as d:
    a=develop(args.reference,Path(d)/"reference.ppm"); b=develop(args.exported,Path(d)/"export.ppm")
diff=[abs(i-j) for i,j in zip(a,b)]
print(f"{w}x{h} RGB channels: exact={100*diff.count(0)/len(diff):.5f}%, MAE={sum(diff)/len(diff):.6f}/255, max={max(diff)}")
gap=4
rows=b"".join(b"\x00"+a[j*w*3:(j+1)*w*3]+b"\xf4"*(gap*3)+b[j*w*3:(j+1)*w*3] for j in range(h))
with args.png.open("xb") as out:
    out.write(b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR",struct.pack(">IIBBBBB",w*2+gap,h,8,2,0,0,0))+chunk(b"tEXt",b"Description\0Left: NX Studio; right: Nikon Pixel Merge. Same LibRaw development at 1:1.")+chunk(b"IDAT",zlib.compress(rows))+chunk(b"IEND",b""))
