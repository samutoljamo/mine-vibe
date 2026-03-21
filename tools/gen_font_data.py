#!/usr/bin/env python3
"""Embed a binary file as a C byte array header."""
import sys

if len(sys.argv) != 3:
    print(f"Usage: {sys.argv[0]} input.ttf output.h", file=sys.stderr)
    sys.exit(1)

with open(sys.argv[1], 'rb') as f:
    data = f.read()

with open(sys.argv[2], 'w') as out:
    out.write('#pragma once\n')
    out.write(f'/* Auto-generated from {sys.argv[1]} — do not edit */\n')
    out.write(f'static const unsigned int  ui_font_data_len = {len(data)};\n')
    out.write( 'static const unsigned char ui_font_data[] = {\n')
    for i, b in enumerate(data):
        if i % 16 == 0:
            out.write('    ')
        out.write(f'0x{b:02x},')
        if i % 16 == 15:
            out.write('\n')
    out.write('\n};\n')
