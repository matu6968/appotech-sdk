#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
sdk=Path(__file__).resolve().parents[1]
# Action Vision may use a four-bit bus; the Silvergear is limited to one line (its PA3/PE10/PE11 belong to the printer and the cameras).
for board in ('action-vision','silvergear-3222882'):
    with tempfile.TemporaryDirectory(prefix='jt5295-sd-') as directory:
        binary=Path(directory)/'sd-model'
        subprocess.run(['cc','-std=c11','-O2','-no-pie','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-I'+str(sdk/'tests/mock'),
                        '-I'+str(sdk/'include'),'-I'+str(sdk/'boards'/board),'-I'+str(sdk/'third_party/fatfs'),
                        str(sdk/'src/sd.c'),str(sdk/'src/fatfs_diskio.c'),str(sdk/'tests/sd_model.c'),'-o',str(binary)],check=True)
        print(board+':',flush=True)
        subprocess.run([str(binary)],check=True)
