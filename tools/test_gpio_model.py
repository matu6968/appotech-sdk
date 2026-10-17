#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
sdk=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='jt5295-gpio-') as directory:
    binary=Path(directory)/'gpio-model'
    subprocess.run(['cc','-std=c11','-O2','-no-pie','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-I'+str(sdk/'tests/mock'),
                    '-I'+str(sdk/'include'),'-I'+str(sdk/'boards/action-vision'),
                    str(sdk/'src/adc.c'),str(sdk/'src/gpio.c'),str(sdk/'src/buttons.c'),str(sdk/'tests/gpio_model.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
