#!/usr/bin/env python3
"""FatFs R0.16 acceptance on disposable RAM images, with sanitizers."""
from pathlib import Path
import subprocess,tempfile
sdk=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='jt5295-fatfs-') as directory:
    binary=Path(directory)/'fatfs-model'
    subprocess.run(['cc','-std=c11','-O2','-no-pie','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-I'+str(sdk/'tests/mock'),'-I'+str(sdk/'third_party/fatfs'),'-I'+str(sdk/'include'),
                    str(sdk/'third_party/fatfs/ff.c'),str(sdk/'third_party/fatfs/ffunicode.c'),
                    str(sdk/'src/storage_stdio.c'),str(sdk/'src/storage.c'),str(sdk/'tests/fatfs_model.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
