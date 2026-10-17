`jpeg-baseline-220x176.jpg` is a synthetic baseline 4:2:0 fixture for parser
and register/DMA model tests, independent of the user's SD images. It was
generated with Pillow using:

```python
from PIL import Image
image = Image.new('RGB', (220, 176))
for y in range(176):
    for x in range(220):
        image.putpixel((x, y), ((x*7+y*3) % 256, (y*11+x) % 256,
                               ((x//8+y//8) & 1)*240))
image.save('jpeg-baseline-220x176.jpg', quality=90, subsampling=2,
           progressive=False)
```

Tests consume the stored fixture; Pillow is only required to regenerate it.
