#!/usr/bin/env python3
"""Compare the SDK renderer with native stock instructions under ASan/UBSan."""
import argparse
from pathlib import Path
import random
import subprocess
import tempfile
from stock_printer_reference import StockDot
from extract_silvergear import printer_render_tables

def main():
 sdk=Path(__file__).resolve().parents[1]
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--stock-app',type=Path,default=sdk.parent/'stock-backups/silvergear-3222882-20261102/analysis/stock-app-02000000.bin')
 p.add_argument('--stock',type=Path,default=sdk.parent/'stock-backups/silvergear-3222882-20261102/stock.bin')
 a=p.parse_args();stock=StockDot(a.stock_app,sdk/'research/silvergear/stock-app.disasm')
 assert (sdk/'boards/silvergear-3222882/printer-render.inc').read_text()==printer_render_tables(a.stock.read_bytes())
 rng=random.Random(5295);fixtures=[]
 for w,h,kind in [(1,17,'random'),(2,7,'random'),(3,5,'random'),(13,9,'random'),(17,19,'ramp'),
                  (37,11,'random'),(9,5,'white'),(9,5,'black'),(384,3,'random')]:
  source=bytes(255 if kind=='white' else 0 if kind=='black' else (i*255//(w*h-1)) if kind=='ramp' else rng.randrange(256) for i in range(w*h))
  fixtures.append((w,h,source,stock.render(source,w,h)))
 text=['struct fixture {unsigned width,height;const unsigned char *source,*expected;};']
 for n,(w,h,source,expected) in enumerate(fixtures):
  for name,data in [('source',source),('expected',expected)]:text.append('static const unsigned char %s%d[]={%s};'%(name,n,','.join(map(str,data))))
 text.append('static const struct fixture fixtures[]={'+','.join('{%d,%d,source%d,expected%d}'%(w,h,n,n) for n,(w,h,_,_) in enumerate(fixtures))+'};')
 text.append('static const unsigned char expected_thresholds[]={'+','.join(map(str,stock.image[0xa3cfc:0xa3d1c]))+'};')
 with tempfile.TemporaryDirectory(prefix='printer-modes-') as directory:
  root=Path(directory);(root/'printer_render_fixtures.h').write_text('\n'.join(text)+'\n');binary=root/'model'
  subprocess.run(['cc','-std=c11','-O2','-no-pie','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
   '-I'+str(root),'-I'+str(sdk/'include'),'-I'+str(sdk/'boards/silvergear-3222882'),
   str(sdk/'src/printer_render.c'),str(sdk/'tests/printer_render_model.c'),'-o',str(binary)],check=True)
  subprocess.run([binary],check=True)
if __name__=='__main__':main()
