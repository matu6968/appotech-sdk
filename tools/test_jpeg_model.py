#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
sdk=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='jt5295-jpeg-') as directory:
 binary=Path(directory)/'jpeg-model'
 subprocess.run(['cc','-std=c11','-D_GNU_SOURCE','-O2','-no-pie','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
  '-I'+str(sdk/'tests/mock'),'-I'+str(sdk/'include'),str(sdk/'src/jpeg.c'),str(sdk/'tests/jpeg_model.c'),'-o',str(binary)],check=True)
 subprocess.run([str(binary),str(sdk/'tests/fixtures/jpeg-baseline-220x176.jpg')],check=True)
