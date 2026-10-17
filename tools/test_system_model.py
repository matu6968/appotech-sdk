#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
sdk=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='jt5295-system-') as directory:
    binary=Path(directory)/'system-model'
    subprocess.run(['cc','-std=c11','-O2','-no-pie','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-I'+str(sdk/'tests/mock'),
                    '-I'+str(sdk/'include'),'-I'+str(sdk/'boards/action-vision'),
                    str(sdk/'src/watchdog.c'),str(sdk/'src/power.c'),str(sdk/'src/debug.c'),
                    str(sdk/'tests/system_model.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
