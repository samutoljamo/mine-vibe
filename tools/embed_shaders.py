#!/usr/bin/env python3
"""
Embed compiled SPIR-V shaders as C arrays.
Run after: cmake --build build --target shaders
Writes: src/shaders_generated.c
"""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SHADERS = [
    ('build/shaders/block.vert.spv',   'g_block_vert_spv'),
    ('build/shaders/block.frag.spv',   'g_block_frag_spv'),
    ('build/shaders/player.vert.spv',  'g_player_vert_spv'),
    ('build/shaders/player.frag.spv',  'g_player_frag_spv'),
]

def spv_to_c(path, name):
    data = open(path, 'rb').read()
    lines = [f'const uint8_t {name}[] = {{']
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        lines.append('    ' + ', '.join(f'0x{b:02x}' for b in chunk) + ',')
    lines.append('};')
    lines.append(f'const size_t {name}_size = sizeof({name});')
    return '\n'.join(lines)

with open(ROOT / 'src' / 'shaders_generated.c', 'w') as f:
    f.write('#include <stdint.h>\n#include <stddef.h>\n#include "assets.h"\n\n')
    for rel_path, name in SHADERS:
        path = ROOT / rel_path
        if not path.exists():
            raise FileNotFoundError(f'{path} not found — run cmake --build build --target shaders first')
        f.write(spv_to_c(str(path), name))
        f.write('\n\n')

print('wrote src/shaders_generated.c')
