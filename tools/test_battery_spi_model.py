#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
sdk=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='jt5295-battery-spi-') as directory:
    binary=Path(directory)/'model'
    subprocess.run(['cc','-std=c11','-O2','-no-pie','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-I'+str(sdk/'tests/mock'),
                    '-I'+str(sdk/'include'),'-I'+str(sdk/'boards/action-vision'),
                    *map(str,[sdk/'src/adc.c',sdk/'src/battery.c',sdk/'src/spi.c',sdk/'src/gpio.c',sdk/'src/buttons.c',sdk/'tests/battery_spi_model.c']),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
