#!/usr/bin/env python3
"""Run the SDK display driver against an independent ILI9225G GRAM model."""
from pathlib import Path
import subprocess
import tempfile
sdk=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='jt5295-display-') as directory:
    binary=Path(directory)/'display-model'
    subprocess.run(['cc','-std=c11','-O2','-no-pie','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-I'+str(sdk/'tests/mock'),
                    '-I'+str(sdk/'include'),'-I'+str(sdk/'boards/action-vision'),
                    str(sdk/'src/display.c'),str(sdk/'src/gpio.c'),str(sdk/'tests/display_model.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
