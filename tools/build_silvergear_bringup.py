#!/usr/bin/env python3
"""Build the fixed-address, SPI-free SRAM power/DDR diagnostic."""
import hashlib
import json
from pathlib import Path
import subprocess

sdk = Path(__file__).resolve().parents[1]
out = sdk / 'build/flasher'
out.mkdir(parents=True, exist_ok=True)
prefix = str(sdk / 'toolchain/install/bin/or1k-appotech-elf-')
inputs = ['boot/flasher.S', 'boot/silvergear_bringup.c', 'boot/silvergear_bringup.ld',
          'tools/build_silvergear_bringup.py']
digest = hashlib.sha256()
for name in inputs:
    digest.update(name.encode() + b'\0' + (sdk / name).read_bytes())
build_id = int(digest.hexdigest()[:8], 16)
flags = ['-Os', '-g', '-mappotech', '-fno-delayed-branch', '-ffreestanding', '-fno-builtin',
         '-fno-pic', '-fno-jump-tables', '-ffunction-sections', '-fdata-sections', '-fstack-usage',
         '-Wall', '-Wextra', '-Werror', f'-DBRINGUP_BUILD_ID=0x{build_id:08x}u']
objects = []
for source, stem in [('boot/flasher.S', 'bringup-entry'), ('boot/silvergear_bringup.c', 'silvergear-bringup')]:
    obj = out / (stem + '.o')
    subprocess.run([prefix + 'gcc', *flags, '-c', str(sdk / source), '-o', str(obj)], check=True)
    objects.append(str(obj))
elf = out / 'silvergear-bringup.elf'
binary = elf.with_suffix('.bin')
subprocess.run([prefix + 'gcc', *flags, '-nostdlib', '-T' + str(sdk / inputs[2]),
                '-Wl,--gc-sections,--build-id=none,-Map,' + str(elf.with_suffix('.map')),
                *objects, '-o', str(elf)], check=True)
subprocess.run([prefix + 'objcopy', '-O', 'binary', str(elf), str(binary)], check=True)
with elf.with_suffix('.disasm').open('w') as f:
    subprocess.run([prefix + 'objdump', '-d', str(elf)], stdout=f, check=True)
code = binary.read_bytes()
manifest = dict(board='silvergear-3222882', abi=1, entry=0x2000, control=0x3900,
                reservation_end=0x3c00, request_bytes=208, build_id=build_id,
                source_sha256=digest.hexdigest(), bytes=len(code),
                sha256=hashlib.sha256(code).hexdigest(),
                operations='PM power hold / stock rail setup and bounded DDR pattern blocks; no SPI or peripheral drivers')
binary.with_suffix('.json').write_text(json.dumps(manifest, indent=2) + '\n')
subprocess.run([prefix + 'size', str(elf)], check=True)
print(json.dumps(manifest, indent=2))
