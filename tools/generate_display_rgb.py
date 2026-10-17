#!/usr/bin/env python3
"""Reproduce the small RGB565 to full-range BT.601 contribution tables."""
from pathlib import Path
parts=['/* Generated integer BT.601 contributions for expanded RGB565 channels. */',
       'struct rgb_component { uint16_t y; int16_t u,v; };']
for name,bits,coefficients in [('red',5,(77,-43,128)),('green',6,(150,-85,-107)),('blue',5,(29,128,-21))]:
    parts.append('static const struct rgb_component rgb_'+name+'[] = {')
    for index in range(1<<bits):
        value=(index<<(8-bits))|(index>>(2*bits-8))
        parts.append('    {%d,%d,%d},'%tuple(value*c for c in coefficients))
    parts.append('};')
(Path(__file__).resolve().parents[1]/'src/display_rgb_table.inc').write_text('\n'.join(parts)+'\n')
