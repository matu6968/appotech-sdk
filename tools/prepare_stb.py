#!/usr/bin/env python3
"""Generate the SDK's paged-JPEG hooks without modifying the bundled stb header."""
import argparse,hashlib,json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('output',type=Path);a=p.parse_args()
sdk=Path(__file__).resolve().parents[1];source=sdk/'third_party/stb/stb_image.h';s=source.read_text();original=s
# The SDK is a singleton decoder. Move large automatic metadata/scratch into
# its explicitly reserved NOLOAD region, without changing upstream defaults.
for old,new in [
 ('stbi__zbuf a;', 'STBI_APX_SCRATCH stbi__zbuf a;'),
 ('stbi__zhuffman z_codelength;', 'STBI_APX_SCRATCH stbi__zhuffman z_codelength;'),
 ('stbi__gif g;', 'STBI_APX_SCRATCH stbi__gif g;'),
 ('stbi_uc temp[2048];', 'STBI_APX_SCRATCH stbi_uc temp[2048];'),
 ('stbi_uc palette[1024], pal_img_n=0;', 'STBI_APX_SCRATCH stbi_uc palette[1024]; stbi_uc pal_img_n=0;'),
 ('   stbi_uc pal[256][4];', '   STBI_APX_SCRATCH stbi_uc pal[256][4];')]:
 assert old in s,old;s=s.replace(old,new)
# GIF prefix chains can contain thousands of entries. Expand them iteratively
# into reserved scratch rather than recursively consuming the small CPU stack.
begin=s.index('static void stbi__out_gif_code(')
end=s.index('\nstatic stbi_uc *stbi__process_gif_raster',begin)
old=s[begin:end]
render=old[old.index('   if (g->cur_y >= g->max_y) return;'):old.rfind('}')]
signature=old[:old.index('{')]
s=s[:begin]+signature+'''{
   STBI_APX_SCRATCH stbi__uint16 chain[4096];
   unsigned count=0;
   int prefix=code;
   do {
      if (prefix>=4096 || count==4096) { STBI_APX_GIF_ERROR(); return; }
      chain[count++]=(stbi__uint16)prefix;
      prefix=g->codes[prefix].prefix;
   } while (prefix>=0);
   while (count) {
      stbi_uc *p,*c;
      int idx;
      code=chain[--count];
'''+render+'''   }
}
'''+s[end:]
s='#ifndef STBI_APX_SCRATCH\n#define STBI_APX_SCRATCH\n#endif\n#ifndef STBI_APX_GIF_ERROR\n#define STBI_APX_GIF_ERROR() ((void)0)\n#endif\n'+s
# The macros default to normal upstream behavior when paging is disabled.
needle='static int stbi__parse_entropy_coded_data(stbi__jpeg *z)'
s=s.replace(needle,'''#ifndef STBI_APX_COEFF
#define STBI_APX_COEFF(z,n,x,y) ((z)->img_comp[n].coeff + 64*((x)+(y)*(z)->img_comp[n].coeff_w))
#endif
#ifndef STBI_APX_PROGRESS
#define STBI_APX_PROGRESS() ((void)0)
#endif
#ifndef STBI_APX_BLOCK
#define STBI_APX_BLOCK(z,n,x,y,data) (z)->idct_block_kernel((z)->img_comp[n].data+(z)->img_comp[n].w2*(y)*8+(x)*8,(z)->img_comp[n].w2,data)
#endif
''' + needle,1)
for old,new in [
 ('z->img_comp[n].coeff + 64 * (i + j * z->img_comp[n].coeff_w)','STBI_APX_COEFF(z,n,i,j)'),
 ('z->img_comp[n].coeff + 64 * (x2 + y2 * z->img_comp[n].coeff_w)','STBI_APX_COEFF(z,n,x2,y2)'),
 ('z->idct_block_kernel(z->img_comp[n].data+z->img_comp[n].w2*j*8+i*8, z->img_comp[n].w2, data);','STBI_APX_BLOCK(z,n,i,j,data);'),
 ('z->idct_block_kernel(z->img_comp[n].data+z->img_comp[n].w2*y2+x2, z->img_comp[n].w2, data);','STBI_APX_BLOCK(z,n,x2/8,y2/8,data);')]:
 assert old in s,old;s=s.replace(old,new)
needle='      z->img_comp[i].raw_data = stbi__malloc_mad2'
assert needle in s
s=s.replace(needle,'''#ifdef STBI_APX_COMPONENT
      if (STBI_APX_COMPONENT(z,i)) {
         z->img_comp[i].coeff_w = z->img_comp[i].w2 / 8;
         z->img_comp[i].coeff_h = z->img_comp[i].h2 / 8;
         continue;
      }
#endif
''' + needle,1)
# Yield once per MCU/block row (all four entropy traversal cases).
start=s.index('static int stbi__parse_entropy_coded_data(');end=s.index('static void stbi__jpeg_dequantize',start)
part=s[start:end];part=part.replace('for (j=0; j < h; ++j) {','for (j=0; j < h; ++j) {\n            STBI_APX_PROGRESS();').replace('for (j=0; j < z->img_mcu_y; ++j) {','for (j=0; j < z->img_mcu_y; ++j) {\n            STBI_APX_PROGRESS();')
s=s[:start]+part+s[end:]
a.output.parent.mkdir(parents=True,exist_ok=True)
if not a.output.exists() or a.output.read_text()!=s:a.output.write_text(s)
a.output.with_suffix('.provenance.json').write_text(json.dumps({'upstream_version':'2.30','upstream_sha256':hashlib.sha256(original.encode()).hexdigest(),'generated_sha256':hashlib.sha256(s.encode()).hexdigest(),'patch_generator_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'changes':'Optional reserved scratch, bounded iterative GIF prefixes, coefficient paging/component allocation/block output hooks and MCU-row progress calls; source file unchanged'},indent=2)+'\n')
