#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
sdk=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='jt5295-exception-') as directory:
    binary=Path(directory)/'model'
    subprocess.run(['cc','-std=gnu11','-O1','-no-pie','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-DAPX_EXC_NO_ASM','-DAPX_EXC_HANDLER_ADDRESS=0x02180abcu',
                    '-DAPX_EXC_VECTORS=((volatile uint32_t *)exc_vectors)','-DAPX_EXC_RECORD=((volatile uint32_t *)exc_record)',
                    '-DAPX_EXC_PREVIOUS=((volatile uint32_t *)exc_previous)','-DAPX_EXC_FLAG=((volatile uint32_t *)exc_flag)','-include','stdint.h',
                    '-include',str(sdk/'tests/exception_model.h'),
                    '-I'+str(sdk/'tests/mock'),'-I'+str(sdk/'include'),
                    str(sdk/'src/exception.c'),str(sdk/'tests/exception_model.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
