#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
sdk=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='jt5295-usb-') as directory:
    binary=Path(directory)/'usb-model'
    subprocess.run(['cc','-std=gnu11','-O1','-no-pie','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-DAPX_USB_VID=0x1908','-DAPX_USB_PID=0x3319','-DAPX_USB_FORCE_FULL_SPEED=0',
                    '-DAPX_USB_SETUP_FIFO(i)=apx_test_fifo(i)','-include',str(sdk/'tests/usb_model_fifo.h'),
                    '-I'+str(sdk/'tests/mock'),'-I'+str(sdk/'include'),
                    str(sdk/'src/usb.c'),str(sdk/'tests/usb_model.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
