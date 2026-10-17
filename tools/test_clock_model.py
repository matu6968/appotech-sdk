#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
sdk=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='jt5295-clock-') as directory:
 for mhz in (96,144):
  binary=Path(directory)/f'clock-{mhz}'
  subprocess.run(['cc','-std=c11','-O2','-no-pie','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
   f'-DAPX_CPU_MHZ={mhz}','-I'+str(sdk/'tests/mock'),'-I'+str(sdk/'include'),'-I'+str(sdk/'boards/action-vision'),
   str(sdk/'src/clock.c'),str(sdk/'tests/clock_model.c'),'-o',str(binary)],check=True)
  for args in [[],['spi'],['spi-96'],['bad-pair'],['bad-timer']]+([['bad-write']] if mhz==144 else []):
   subprocess.run([str(binary),*args],check=True)
