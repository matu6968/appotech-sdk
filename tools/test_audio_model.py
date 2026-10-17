#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
sdk=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='jt5295-audio-') as directory:
 for mhz in (96,144):
  binary=Path(directory)/'audio-model'
  subprocess.run(['cc','-std=c11',f'-DAPX_CPU_MHZ={mhz}','-D_GNU_SOURCE','-O2','-no-pie','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
   '-Wl,--section-start=.large_buffer=0x02220000','-I'+str(sdk/'tests/mock'),'-I'+str(sdk/'include'),
   '-I'+str(sdk/'boards/action-vision'),'-I'+str(sdk/'third_party/fatfs'),str(sdk/'src/audio.c'),str(sdk/'src/audio_files.c'),
   str(sdk/'src/microphone.c'),str(sdk/'src/microphone_files.c'),str(sdk/'tests/audio_model.c'),'-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
