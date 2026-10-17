#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
sdk=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='jt5295-trace-') as directory:
    binary=Path(directory)/'model'
    subprocess.run(['cc','-std=gnu11','-O1','-no-pie','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-DAPX_SPI_TRACE=1','-DAPX_TRACE_RING_PTR=((volatile uint32_t *)trace_test_ring)','-DAPX_BUILD_ID="model-build-id!!"',
                    '-I'+str(sdk/'tests/mock'),'-I'+str(sdk/'include'),
                    str(sdk/'src/trace.c'),str(sdk/'tests/trace_model.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
