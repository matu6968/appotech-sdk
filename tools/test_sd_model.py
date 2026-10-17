#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
sdk=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='jt5295-sd-') as directory:
    binary=Path(directory)/'sd-model'
    subprocess.run(['cc','-std=c11','-O2','-no-pie','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-I'+str(sdk/'tests/mock'),
                    '-I'+str(sdk/'include'),'-I'+str(sdk/'boards/action-vision'),'-I'+str(sdk/'third_party/fatfs'),
                    str(sdk/'src/sd.c'),str(sdk/'src/fatfs_diskio.c'),str(sdk/'tests/sd_model.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
