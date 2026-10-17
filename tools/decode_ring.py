#!/usr/bin/env python3
"""Decode a trace SRAM ring read from 0x2800 (see src/trace.c): decode_ring.py ring.bin"""
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "apxtool", "src"))
from apxtool.trace import CRUMB_NAMES

USB = {0x91: "bus event INTRUSB<<16|POWER<<8|INTRTX", 0x92: "setup dir|request|wValue", 0x93: "SET_ADDRESS accepted",
       0x94: "address applied to FADDR", 0x95: "configured", 0x96: "EP0 RxPktRdy csr<<8|count", 0x97: "EP0 data packet count<<8|last", 0x98: "EP0 SENTSTALL/SETUPEND csr", 0x99: "enumeration timeout", 0x9B: "phy wait ticks"}
words = struct.unpack("<%dI" % (len(open(sys.argv[1], "rb").read()) // 4), open(sys.argv[1], "rb").read())
if words[0] != 0x474E5252:
    sys.exit("no ring (magic %#x)" % words[0])
for i in range(min(words[1], 200)):
    v, t = words[2 + 2 * i], words[3 + 2 * i]
    cid = v >> 24
    if cid == 0x9A:
        print("%3d  SPR %04x = %08x" % (i, v & 0xFFFF, t))
        continue
    print("%3d  t=%5d  %02x %-30s %06x" % (i, t, cid, USB.get(cid) or CRUMB_NAMES.get(cid, "?"), v & 0xFFFFFF))
print("heartbeat", words[2 + 400], "timer", words[3 + 192] if len(words) > 403 else "-")
