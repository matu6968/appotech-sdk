#!/usr/bin/env python3
"""Capture the camera's UART (PA7 TX, 115200 8N1) to a file for a fixed time, with host timestamps.

Usage: uart_capture.py OUTPUT SECONDS [DEVICE]. Read-only; the adapter's RX/TX are never written.
"""
import sys,time
import serial
out,seconds=sys.argv[1],float(sys.argv[2]);dev=sys.argv[3] if len(sys.argv)>3 else '/dev/ttyACM0'
port=serial.Serial(dev,115200,timeout=0.05)
end=time.monotonic()+seconds;start=time.monotonic();buf=b''
with open(out,'x') as f:
    while time.monotonic()<end:
        chunk=port.read(256)
        if not chunk:continue
        buf+=chunk
        while b'\n' in buf:
            line,buf=buf.split(b'\n',1)
            f.write(f'{time.monotonic()-start:7.3f}  {line.decode("ascii","replace").rstrip()}\n');f.flush()
