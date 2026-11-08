#!/usr/bin/env python3
"""SPI NOR driver against the controller/flash model, for the Action (5e4015, 2 MiB) board."""
from pathlib import Path
import subprocess,tempfile
sdk=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='jt5295-spi-') as directory:
    binary=Path(directory)/'model'
    subprocess.run(['cc','-std=c11','-O2','-no-pie','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-I'+str(sdk/'tests/mock'),'-I'+str(sdk/'include'),'-I'+str(sdk/'boards/action-vision'),
                    str(sdk/'src/spi.c'),str(sdk/'tests/spi_model.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
