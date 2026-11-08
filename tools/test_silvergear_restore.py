#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile
sdk=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='silvergear-restore-') as directory:
    binary=Path(directory)/'restore-model'
    subprocess.run(['cc','-std=c11','-O1','-no-pie','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-DFLASHER_HOST','-DSILVERGEAR_BOOT_RESTORE',
                    str(sdk/'boot/flasher.c'),str(sdk/'tests/flasher_model.c'),
                    str(sdk/'tests/silvergear_restore_model.c'),'-o',str(binary)],check=True)
    subprocess.run([binary],check=True)
