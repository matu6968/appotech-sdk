#!/usr/bin/env python3
"""Exercise real Silvergear drivers against independent peripheral models."""
from pathlib import Path
import subprocess
import tempfile
sdk=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='silvergear-models-') as tmp:
    for name,sources in [('st7789',['src/display_st7789.c','tests/st7789_model.c']),('printer',['src/printer.c','src/printer_render.c','src/gpio.c','tests/printer_model.c']),('buttons',['src/adc.c','src/gpio.c','src/buttons.c','tests/gpio_model.c']),('camera',['src/camera.c','src/watchdog.c','tests/camera_model.c']),('spi',['src/spi.c','tests/spi_model.c'])]:
        output=Path(tmp)/name
        subprocess.run(['cc','-std=c11','-O2','-no-pie','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-I'+str(sdk/'tests/mock'),'-I'+str(sdk/'include'),
            '-I'+str(sdk/'boards/silvergear-3222882'),*[str(sdk/p) for p in sources],'-o',str(output)],check=True)
        subprocess.run([output],check=True)
    # Fixed row period enabled, and the old synchronous temperature sample (board.h guards them with #ifndef).
    for variant,flags in (('printer_period',['-DAPX_PRINTER_ROW_PERIOD_GRAY_US=14500u','-DAPX_PRINTER_ROW_PERIOD_DOT_US=16500u']),
                          ('printer_sync_temperature',['-DAPX_PRINTER_SPREAD_TEMPERATURE=0'])):
        output=Path(tmp)/variant
        subprocess.run(['cc','-std=c11','-O2','-no-pie','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',*flags,
            '-I'+str(sdk/'tests/mock'),'-I'+str(sdk/'include'),'-I'+str(sdk/'boards/silvergear-3222882'),
            *[str(sdk/p) for p in ('src/printer.c','src/printer_render.c','src/gpio.c','tests/printer_model.c')],'-o',str(output)],check=True)
        subprocess.run([output],check=True)
    # Non-default per-mode energy defaults (board.h guards them with #ifndef).
    output=Path(tmp)/'printer_energy'
    subprocess.run(['cc','-std=c11','-O2','-no-pie','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-DAPX_PRINTER_ENERGY_GRAY_PERCENT=130u','-DAPX_PRINTER_ENERGY_DOT_PERCENT=170u',
        '-I'+str(sdk/'tests/mock'),'-I'+str(sdk/'include'),'-I'+str(sdk/'boards/silvergear-3222882'),
        *[str(sdk/p) for p in ('src/printer.c','src/printer_render.c','src/gpio.c','tests/printer_model.c')],'-o',str(output)],check=True)
    subprocess.run([output],check=True)
