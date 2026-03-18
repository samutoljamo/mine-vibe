#!/usr/bin/env python3
"""
Asset generator for the minecraft Vulkan project.
Usage:
  python tools/gen_assets.py            # generate missing PNGs + C arrays
  python tools/gen_assets.py --regenerate  # force-regenerate all PNGs

Tile indices must match block.c:
  stone=0 dirt=1 grass_top=2 grass_side=3 sand=4
  wood_top=5 wood_side=6 leaves=7 water=16 bedrock=17
"""
import argparse, os, random, struct
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent  # tools/ → project root

ATLAS_SIZE   = 256
TILE_SIZE    = 16
TILES_PER_ROW = ATLAS_SIZE // TILE_SIZE
SKIN_W, SKIN_H = 64, 32

# ── block colors ──────────────────────────────────────────────────────────────

def _noise(r, g, b, a, px, py, strength=12):
    rng = random.Random(px * 31 + py * 97)
    d = rng.randint(-strength, strength)
    return (max(0,min(255,r+d)), max(0,min(255,g+d)), max(0,min(255,b+d)), a)

def draw_stone(size=16):
    img = Image.new('RGBA', (size, size))
    for y in range(size):
        for x in range(size):
            img.putpixel((x, y), _noise(122, 122, 122, 255, x, y, 15))
    # darker speckles
    rng = random.Random(1)
    for _ in range(10):
        px, py = rng.randint(0, size-1), rng.randint(0, size-1)
        img.putpixel((px, py), (85, 85, 85, 255))
    return img

def draw_dirt(size=16):
    img = Image.new('RGBA', (size, size))
    for y in range(size):
        for x in range(size):
            img.putpixel((x, y), _noise(134, 86, 40, 255, x, y, 12))
    return img

def draw_grass_top(size=16):
    img = Image.new('RGBA', (size, size))
    for y in range(size):
        for x in range(size):
            img.putpixel((x, y), _noise(80, 158, 10, 255, x, y, 18))
    return img

def draw_grass_side(size=16):
    img = Image.new('RGBA', (size, size))
    for y in range(size):
        for x in range(size):
            if y < 4:
                img.putpixel((x, y), _noise(80, 158, 10, 255, x, y, 14))
            else:
                img.putpixel((x, y), _noise(134, 86, 40, 255, x, y, 10))
    return img

def draw_sand(size=16):
    img = Image.new('RGBA', (size, size))
    for y in range(size):
        for x in range(size):
            img.putpixel((x, y), _noise(215, 193, 138, 255, x, y, 10))
    return img

def draw_wood_top(size=16):
    img = Image.new('RGBA', (size, size))
    cx, cy = size // 2, size // 2
    for y in range(size):
        for x in range(size):
            d = ((x-cx)**2 + (y-cy)**2) ** 0.5
            ring = int(d) % 3
            base = 145 if ring == 0 else (120 if ring == 1 else 100)
            img.putpixel((x, y), _noise(base, int(base*0.7), int(base*0.4), 255, x, y, 6))
    return img

def draw_wood_side(size=16):
    img = Image.new('RGBA', (size, size))
    for y in range(size):
        for x in range(size):
            stripe = x % 4
            base = 130 if stripe < 2 else 100
            img.putpixel((x, y), _noise(base, int(base*0.68), int(base*0.38), 255, x, y, 6))
    return img

def draw_leaves(size=16):
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    rng = random.Random(7)
    for y in range(size):
        for x in range(size):
            if rng.random() > 0.15:
                img.putpixel((x, y), _noise(42, 120, 18, 200, x, y, 20))
    return img

def draw_water(size=16):
    img = Image.new('RGBA', (size, size))
    for y in range(size):
        for x in range(size):
            ripple = 8 if (x + y * 2) % 5 == 0 else 0
            img.putpixel((x, y), (28 + ripple, 82 + ripple, 185 + ripple, 200))
    return img

def draw_bedrock(size=16):
    img = Image.new('RGBA', (size, size))
    for y in range(size):
        for x in range(size):
            img.putpixel((x, y), _noise(52, 52, 52, 255, x, y, 8))
    rng = random.Random(13)
    for _ in range(14):
        px, py = rng.randint(0, size-1), rng.randint(0, size-1)
        img.putpixel((px, py), (22, 22, 22, 255))
    return img

TILE_GENERATORS = {
    0:  draw_stone,
    1:  draw_dirt,
    2:  draw_grass_top,
    3:  draw_grass_side,
    4:  draw_sand,
    5:  draw_wood_top,
    6:  draw_wood_side,
    7:  draw_leaves,
    16: draw_water,
    17: draw_bedrock,
}

TILE_NAMES = {
    0: "stone", 1: "dirt", 2: "grass_top", 3: "grass_side", 4: "sand",
    5: "wood_top", 6: "wood_side", 7: "leaves", 16: "water", 17: "bedrock",
}

# ── player skin ───────────────────────────────────────────────────────────────

def draw_player_skin():
    """64×32 skin. Explorer in a navy suit with tan face."""
    img = Image.new('RGBA', (SKIN_W, SKIN_H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    SKIN   = (210, 175, 130, 255)
    NAVY   = (30,  58,  100, 255)
    DARK   = (18,  35,  65,  255)
    PANTS  = (48,  48,  62,  255)
    HAIR   = (55,  38,  18,  255)
    EYE    = (30,  30,  50,  255)
    WHITE  = (230, 230, 230, 255)

    def fill(x0, y0, x1, y1, color):
        d.rectangle([x0, y0, x1-1, y1-1], fill=color)

    # ── Head ──────────────────────────────────────────────────────────────────
    # top (8,0)-(16,8)
    fill(8, 0, 16, 8, HAIR)
    # bottom (16,0)-(24,8)
    fill(16, 0, 24, 8, SKIN)
    # right (0,8)-(8,16)
    fill(0, 8, 8, 16, HAIR)
    # front (8,8)-(16,16) — face
    fill(8, 8, 16, 16, SKIN)
    img.putpixel((10, 11), EYE)   # left eye
    img.putpixel((13, 11), EYE)   # right eye
    img.putpixel((10, 12), EYE)
    img.putpixel((13, 12), EYE)
    img.putpixel((11, 14), DARK)  # mouth left
    img.putpixel((13, 14), DARK)  # mouth right
    # left (16,8)-(24,16)
    fill(16, 8, 24, 16, HAIR)
    # back (24,8)-(32,16)
    fill(24, 8, 32, 16, HAIR)

    # ── Body ──────────────────────────────────────────────────────────────────
    # top (20,16)-(28,20)
    fill(20, 16, 28, 20, NAVY)
    # bottom (28,16)-(36,20)
    fill(28, 16, 36, 20, NAVY)
    # right (16,20)-(20,32)
    fill(16, 20, 20, 32, DARK)
    # front (20,20)-(28,32)
    fill(20, 20, 28, 32, NAVY)
    # left (28,20)-(32,32)
    fill(28, 20, 32, 32, DARK)
    # back (32,20)-(40,32)
    fill(32, 20, 40, 32, NAVY)

    # ── Right arm ─────────────────────────────────────────────────────────────
    # top (44,16)-(48,20)
    fill(44, 16, 48, 20, NAVY)
    # bottom (48,16)-(52,20)
    fill(48, 16, 52, 20, NAVY)
    # right/outer (40,20)-(44,32)
    fill(40, 20, 44, 32, DARK)
    # front (44,20)-(48,32)
    fill(44, 20, 48, 32, NAVY)
    # left/inner (48,20)-(52,32)
    fill(48, 20, 52, 32, DARK)
    # back (52,20)-(56,32)
    fill(52, 20, 56, 32, NAVY)

    # ── Right leg ─────────────────────────────────────────────────────────────
    # top (4,16)-(8,20)
    fill(4, 16, 8, 20, PANTS)
    # bottom (8,16)-(12,20)
    fill(8, 16, 12, 20, PANTS)
    # right/outer (0,20)-(4,32)
    fill(0, 20, 4, 32, (35, 35, 45, 255))
    # front (4,20)-(8,32)
    fill(4, 20, 8, 32, PANTS)
    # left/inner (8,20)-(12,32)
    fill(8, 20, 12, 32, (35, 35, 45, 255))
    # back (12,20)-(16,32)
    fill(12, 20, 16, 32, PANTS)

    return img

# ── Atlas assembly ────────────────────────────────────────────────────────────

def assemble_atlas(tile_images):
    atlas = Image.new('RGBA', (ATLAS_SIZE, ATLAS_SIZE), (0, 0, 0, 0))
    for idx, img in tile_images.items():
        tx = (idx % TILES_PER_ROW) * TILE_SIZE
        ty = (idx // TILES_PER_ROW) * TILE_SIZE
        atlas.paste(img, (tx, ty))
    return atlas

# ── C array writer ────────────────────────────────────────────────────────────

def image_to_c_array(img, name):
    data = list(img.tobytes())
    lines = [f'const uint8_t {name}[] = {{']
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        lines.append('    ' + ', '.join(f'0x{b:02x}' for b in chunk) + ',')
    lines.append('};')
    return '\n'.join(lines)

def write_assets_c(atlas, skin, out_path):
    with open(out_path, 'w') as f:
        f.write('#include <stdint.h>\n\n')
        f.write(image_to_c_array(atlas, 'g_atlas_pixels'))
        f.write('\n\n')
        f.write(image_to_c_array(skin, 'g_player_skin_pixels'))
        f.write('\n')

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--regenerate', action='store_true',
                    help='Force-regenerate all PNGs from code')
    args = ap.parse_args()

    os.makedirs(ROOT / 'assets' / 'blocks', exist_ok=True)

    tile_images = {}
    for idx, gen in TILE_GENERATORS.items():
        name = TILE_NAMES[idx]
        path = str(ROOT / 'assets' / 'blocks' / f'{name}.png')
        if args.regenerate or not os.path.exists(path):
            img = gen()
            img.save(path)
            print(f'  generated {path}')
        else:
            img = Image.open(path).convert('RGBA')
            if img.size != (TILE_SIZE, TILE_SIZE):
                raise ValueError(f'{path}: expected {TILE_SIZE}x{TILE_SIZE}, got {img.size}')
            print(f'  read      {path}')
        tile_images[idx] = img

    skin_path = str(ROOT / 'assets' / 'player_skin.png')
    if args.regenerate or not os.path.exists(skin_path):
        skin = draw_player_skin()
        skin.save(skin_path)
        print(f'  generated {skin_path}')
    else:
        skin = Image.open(skin_path).convert('RGBA')
        if skin.size != (SKIN_W, SKIN_H):
            raise ValueError(f'{skin_path}: expected {SKIN_W}x{SKIN_H}, got {skin.size}')
        print(f'  read      {skin_path}')

    atlas = assemble_atlas(tile_images)
    atlas.save(str(ROOT / 'assets' / 'atlas_preview.png'))

    write_assets_c(atlas, skin, str(ROOT / 'src' / 'assets_generated.c'))
    print('  wrote     src/assets_generated.c')

if __name__ == '__main__':
    main()
