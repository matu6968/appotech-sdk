#!/usr/bin/env python3
"""Run actual SRAM power/DDR code with ASan/UBSan; build its target image."""
from pathlib import Path
import subprocess
import tempfile
sdk=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='silvergear-bringup-') as tmp:
    binary=Path(tmp)/'model'
    subprocess.run(['cc','-std=c11','-O2','-no-pie','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined',str(sdk/'tests/silvergear_bringup_model.c'),
                    '-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
subprocess.run(['make','-C',str(sdk/'boot'),'silvergear-bringup'],check=True)
assert len((sdk/'build/flasher/silvergear-bringup.bin').read_bytes())<=0xf00
