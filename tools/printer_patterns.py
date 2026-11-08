#!/usr/bin/env python3
"""Generate the bounded blank/sparse acceptance pair; never accesses a device."""
import argparse
import hashlib
import json
from pathlib import Path

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    blank=bytes(48*32);sparse=bytearray(blank)
    # Three 8-pixel-wide bars, heights 24/16/8, all ending at row 27.
    # At most 24 black pixels per row, 12 per alternating half.
    for x0,y0 in ((48,4),(188,12),(320,20)):
        for y in range(y0,28):
            for x in range(x0,x0+8):sparse[y*48+x//8]|=0x80>>(x&7)
    records=[]
    for name,data in (('blank',blank),('sparse',bytes(sparse))):
        path=args.output/(name+'-384x32.bin')
        if path.exists() and path.read_bytes()!=data:raise ValueError('refusing to replace a different raster: '+str(path))
        path.write_bytes(data)
        records.append(dict(file=path.name,width=384,height=32,stride=48,
            black_pixels=sum(v.bit_count() for v in data),sha256=hashlib.sha256(data).hexdigest()))
    report=dict(format='packed MSB first, 1=black',energy_percent=100,
        expected='blank control; three bars of increasing start row (24,16,8 rows tall)',
        maximum_black_pixels_per_row=24,rasters=records)
    (args.output/'patterns.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))

if __name__=='__main__':main()
