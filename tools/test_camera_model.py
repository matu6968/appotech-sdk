#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
sdk=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='jt5295-camera-') as directory:
    binary=Path(directory)/'camera-model'
    subprocess.run(['cc','-std=c11','-O2','-no-pie','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-I'+str(sdk/'tests/mock'),'-I'+str(sdk/'include'),
        str(sdk/'src/camera.c'),str(sdk/'src/watchdog.c'),str(sdk/'tests/camera_model.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
    service=Path(directory)/'camera-files-model'
    subprocess.run(['cc','-std=c11','-O2','-no-pie','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-Wl,--section-start=.large_buffer=0x02220000',
        '-Wl,--section-start=.jpeg_buffer=0x022e0000','-I'+str(sdk/'tests/mock'),
        '-I'+str(sdk/'include'),'-I'+str(sdk/'third_party/fatfs'),
        str(sdk/'src/camera_files.c'),str(sdk/'tests/camera_files_model.c'),'-o',str(service)],check=True)
    subprocess.run([str(service)],check=True)
