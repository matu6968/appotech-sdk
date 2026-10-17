#!/usr/bin/env python3
"""Reject large image-decoder frames before a 4 KiB-stack SDK can be linked."""
import argparse
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('usage',type=Path)
a=p.parse_args()
entries=[]
for line in a.usage.read_text().splitlines():
    location,size,kind=line.split('\t')
    entries.append((int(size),location,kind))
assert entries,'empty compiler stack-usage report'
for size,location,kind in entries:
    assert kind=='static' and size<=1280,(location,size,kind)
print('Image stack: maximum static frame',max(e[0] for e in entries),'bytes; native nested-call guard still required')
