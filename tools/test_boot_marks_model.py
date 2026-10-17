#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
sdk=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='jt5295-marks-') as directory:
    binary=Path(directory)/'model'
    subprocess.run(['cc','-std=gnu11','-O1','-no-pie','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-DAPX_MARK_PTR=((volatile uint32_t *)marks_ram)','-I'+str(sdk/'tests/mock'),'-I'+str(sdk/'include'),
                    '-include','stdint.h','-include',str(sdk/'tests/boot_marks_model.h'),
                    str(sdk/'src/boot_marks.c'),str(sdk/'tests/boot_marks_model.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
