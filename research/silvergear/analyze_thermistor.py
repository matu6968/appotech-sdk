#!/usr/bin/env python3
"""Reproduce the stock NTC-table fit and an unpowered contact-check guide.

Photo contact labels are hypotheses, not a confirmed mainboard pinout.
Never use this guide to apply power to an unidentified mechanism.
"""
import argparse
import base64
import hashlib
import io
import json
import math
from pathlib import Path
import struct

STOCK_SHA = '4874af74051372363e8c36ff99e369657648eb353950b41fe2a5a00d35acdb9e'
PHOTO_SHA = '4ba7b6ddc7b4a91f553037a4967f09c4b458e9c514d9ce86c2e44dc6ddbad572'
REFERENCE = 'https://download.mikroe.com/documents/datasheets/LTP02-245-13_datasheet.pdf'


def fit(data):
    if hashlib.sha256(data).hexdigest() != STOCK_SHA:
        raise ValueError('Expected the verified Silvergear stock backup')
    actual = list(struct.unpack_from('<81H', data, 0x2200 + 0xa347c))
    # R(T)/Rpull = (R25/Rpull)*exp(B*(1/(T+273)-1/298)).
    predicted = [round(1023 / (1 + math.exp(-3950 * (1/(t+273)-1/298)) / 3))
                 for t in range(81)]
    errors = [a-b for a, b in zip(actual, predicted)]
    return dict(stock_sha256=STOCK_SHA, table_spi_offset=hex(0x2200 + 0xa347c),
                table_ram_address='0x020a347c', entries=81,
                beta_kelvin=3950, resistance_ratio_at_25c=3,
                kelvin_offset=273, adc_full_scale=1023,
                max_error_counts=max(map(abs, errors)), exact=actual == predicted,
                scale_example=dict(thermistor_r25_ohm=30000, pullup_ohm=10000),
                limitation='The ADC table determines a resistance ratio, not absolute resistor values or the fitted part identity.',
                reference=REFERENCE, stock=actual, predicted=predicted)


def guide(photo, output):
    from PIL import Image, ImageOps
    if hashlib.sha256(photo.read_bytes()).hexdigest() != PHOTO_SHA:
        raise ValueError('Expected the original printer-assembly-back.jpg')
    image = ImageOps.exif_transpose(Image.open(photo))
    # Same inspection crop as the recorded visual analysis. No invented traces.
    image = image.crop((1000, 1700, 3400, 3000))
    image.thumbnail((2200, 1200))
    image = image.crop((400, 260, 1100, 900))
    stream = io.BytesIO(); image.save(stream, format='PNG')
    data = base64.b64encode(stream.getvalue()).decode()
    svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="1800" height="1260" viewBox="0 0 1800 1260">',
           '<rect width="1800" height="1260" fill="#f5f7fa"/>',
           '<g font-family="DejaVu Sans, sans-serif" fill="#172331">',
           '<text x="60" y="60" font-size="32" font-weight="bold">Silvergear printer: candidate temperature contacts</text>',
           '<text x="60" y="105" font-size="23">Local labels C1-C24 count from the motor-side edge of the ribbon.</text>',
           '<text x="60" y="140" font-size="23" fill="#b14800">Mapping paused: reliable probe contact has not been established.</text>',
           f'<image x="610" y="185" width="910" height="832" href="data:image/png;base64,{data}"/>',
           '<text x="60" y="245" font-size="25" font-weight="bold">Orientation</text>',
           '<text x="60" y="285" font-size="22">C1-C4 branch toward the motor.</text>',
           '<text x="60" y="322" font-size="22">C22-C24 branch toward the sensor.</text>',
           '<text x="60" y="370" font-size="20">Gold areas are conductors;</text>',
           '<text x="60" y="402" font-size="20">dark slots separate them.</text>']
    # Contact centres are between the dark slots, not on the dark slots.
    y0, pitch = 330.0, (863.0 - 330.0) / 23
    for n in range(1, 25):
        y = 185 + (y0 + (n-1)*pitch - 260)*1.3
        colour = '#b85c00' if n == 11 else '#006c8c' if n in (9, 15) else '#273946'
        svg.append(f'<text x="940" y="{y+7:.1f}" text-anchor="end" font-size="19" fill="{colour}">C{n:02}</text>')
        svg.append(f'<line x1="952" y1="{y:.1f}" x2="1135" y2="{y:.1f}" stroke="{colour}" stroke-width="1.5" opacity="0.7"/>')
        if n in (9, 11, 15):
            svg.append(f'<circle cx="1149" cy="{y:.1f}" r="9" fill="none" stroke="{colour}" stroke-width="4"/>')
    for n, label in ((9, 'C9: candidate ground'), (11, 'C11: candidate TH'), (15, 'C15: candidate ground')):
        y = 185 + (y0 + (n-1)*pitch - 260)*1.3
        colour = '#b85c00' if n == 11 else '#006c8c'
        svg += [f'<line x1="1160" y1="{y:.1f}" x2="1535" y2="{y:.1f}" stroke="{colour}" stroke-width="3"/>',
                f'<text x="1545" y="{y-12:.1f}" font-size="19" fill="{colour}">{label}</text>']
    svg += ['<text x="60" y="510" font-size="25" font-weight="bold">Current results are inconclusive</text>',
            '<text x="60" y="550" font-size="22">C9 to C15: over-range on 200 Ω.</text>',
            '<text x="60" y="585" font-size="20">C8-to-C9 probe contact is also uncertain.</text>',
            '<text x="60" y="640" font-size="22">C9 to C15: ground not yet confirmed.</text>',
            '<text x="60" y="680" font-size="20">C11-to-C9 readings remain inconclusive.</text>',
            '<text x="60" y="715" font-size="20">Do not keep probing with oversized tips.</text>',
            '<text x="60" y="765" font-size="20">Use the matching exposed contacts at</text>',
            '<text x="60" y="797" font-size="20">the free ribbon end. Circles mark traces,</text>',
            '<text x="60" y="829" font-size="20">not test pads; do not pierce the coating.</text>',
            '<text x="60" y="882" font-size="20">Ribbon disconnected from mainboard.</text>',
            '<text x="60" y="914" font-size="20">USB, battery and UART disconnected.</text>',
            '<text x="60" y="946" font-size="20">Do not remove protective head covers.</text>',
            '<rect x="60" y="1060" width="1680" height="145" rx="12" fill="#e5ecf1"/>',
            '<text x="85" y="1098" font-size="21">The trace grouping resembles the Seiko LTP02 24-contact arrangement; electrical compatibility is unconfirmed.</text>',
            '<text x="85" y="1132" font-size="21">The stock ADC table fits B=3950 K and R25/Rpull-up=3 exactly. Absolute resistances still need measurement.</text>',
            '<text x="85" y="1166" font-size="19">Reference hypothesis: 30 kΩ thermistor / 10 kΩ pull-up. This is not evidence that the assembly is a Seiko part.</text>',
            f'<text x="60" y="1240" font-size="15">Original photograph SHA256: {PHOTO_SHA}</text>', '</g></svg>']
    output.write_text('\n'.join(svg) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('backup', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--photo', type=Path)
    parser.add_argument('--guide', type=Path)
    args = parser.parse_args()
    result = fit(args.backup.read_bytes())
    text = json.dumps(result, indent=2) + '\n'
    if args.output: args.output.write_text(text)
    else: print(text, end='')
    if args.photo or args.guide:
        if not args.photo or not args.guide: parser.error('--photo and --guide are required together')
        guide(args.photo, args.guide)
    if not result['exact']: raise SystemExit('Stock table did not match the inferred model')


if __name__ == '__main__': main()
