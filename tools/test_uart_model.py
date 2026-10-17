#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
sdk=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='jt5295-uart-') as d:
 for profile in [96,144]:
  p=Path(d)/str(profile)
  subprocess.run(['cc','-std=c11','-O2','-no-pie','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',f'-DAPX_CPU_MHZ={profile}',
 '-I'+str(sdk/'tests/mock'),'-I'+str(sdk/'include'),'-I'+str(sdk/'boards/action-vision'),
 str(sdk/'src/uart.c'),str(sdk/'tests/uart_model.c'),'-o',str(p)],check=True)
  subprocess.run([str(p)],check=True)
