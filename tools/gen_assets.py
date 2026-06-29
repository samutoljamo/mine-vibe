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
    # Uniform blue + fully opaque (alpha 255). Any directional pixel pattern
    # in the tile reads as banding once perspective compresses tiles to a
    # few screen pixels — and the per-cell UV rotation in the mesher can't
    # hide directional content because it just rotates the same pattern.
    # Alpha 255 leaves headroom against anisotropic-filter bleeding into
    # adjacent empty atlas cells (alpha=0); the previous alpha=200 left the
    # averaged sample on the wrong side of the discard threshold at oblique
    # angles, producing whole-fragment kills that read as "see through" the
    # water surface.
    img = Image.new('RGBA', (size, size), (28, 82, 185, 255))
    return img

def _draw_ore(speckle_rgb, seed, blobs=7, size=16):
    """Stone base with coloured ore speckles — small clustered blobs so the
    ore reads as veins rather than random pixels."""
    img = draw_stone(size)
    rng = random.Random(seed)
    sr, sg, sb = speckle_rgb
    for _ in range(blobs):
        cx, cy = rng.randint(1, size - 2), rng.randint(1, size - 2)
        # 2x2-ish blob with slight shading variation
        for dx in (0, 1):
            for dy in (0, 1):
                if rng.random() < 0.25:
                    continue
                px, py = cx + dx, cy + dy
                if 0 <= px < size and 0 <= py < size:
                    d = rng.randint(-18, 18)
                    img.putpixel((px, py),
                                 (max(0, min(255, sr + d)),
                                  max(0, min(255, sg + d)),
                                  max(0, min(255, sb + d)), 255))
    return img

def draw_coal_ore(size=16):
    return _draw_ore((38, 38, 38), seed=101, blobs=8, size=size)

def draw_iron_ore(size=16):
    return _draw_ore((188, 152, 116), seed=102, blobs=7, size=size)

def draw_gold_ore(size=16):
    return _draw_ore((230, 196, 70), seed=103, blobs=6, size=size)

def draw_diamond_ore(size=16):
    return _draw_ore((96, 220, 220), seed=104, blobs=6, size=size)

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

def draw_planks(size=16):
    """Warm oak planks: horizontal plank bands with darker seams and a few
    vertical board joints, plus light grain noise."""
    img = Image.new('RGBA', (size, size))
    plank_h = 4
    for y in range(size):
        band = (y // plank_h)
        base = 168 if band % 2 == 0 else 150
        seam = (y % plank_h == 0)
        for x in range(size):
            r = base
            g = int(base * 0.72)
            b = int(base * 0.42)
            if seam:
                r, g, b = int(r*0.7), int(g*0.7), int(b*0.7)
            img.putpixel((x, y), _noise(r, g, b, 255, x, y, 8))
    # vertical board joints, offset per band
    for band in range(size // plank_h):
        jx = (band * 7 + 3) % size
        for y in range(band*plank_h, (band+1)*plank_h):
            if 0 <= y < size:
                img.putpixel((jx, y), (96, 68, 38, 255))
    return img

def draw_cobble(size=16):
    """Grey cobblestone: stone base with a darker mortar grid and lumpy stones."""
    img = draw_stone(size)
    # mortar grid (darker lines)
    for i in range(size):
        if i % 5 == 0:
            for j in range(size):
                img.putpixel((i, j), (70, 70, 70, 255))
                img.putpixel((j, i), (70, 70, 70, 255))
    # a few lighter lumps
    rng = random.Random(55)
    for _ in range(14):
        px, py = rng.randint(0, size-1), rng.randint(0, size-1)
        v = rng.randint(140, 165)
        img.putpixel((px, py), (v, v, v, 255))
    return img

def draw_glass(size=16):
    """Near-transparent pale window pane with a brighter border + corner glints."""
    img = Image.new('RGBA', (size, size), (210, 232, 238, 40))
    for x in range(size):
        img.putpixel((x, 0), (230, 245, 250, 200))
        img.putpixel((x, size-1), (230, 245, 250, 200))
        img.putpixel((0, x), (230, 245, 250, 200))
        img.putpixel((size-1, x), (230, 245, 250, 200))
    # diagonal glint
    for i in range(2, 6):
        img.putpixel((i, i+1), (255, 255, 255, 180))
    return img

def draw_torch(size=16):
    """Wooden torch on a transparent tile: a brown handle rising from the
    bottom with a bright yellow/orange flame at the top."""
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    cx = size // 2
    # Handle: a 2-wide brown stick in the lower two-thirds.
    for y in range(size // 3, size):
        for x in (cx - 1, cx):
            img.putpixel((x, y), _noise(110, 74, 38, 255, x, y, 8))
    # Flame head: a small glowing blob near the top.
    top = size // 3
    for y in range(0, top + 1):
        for x in range(cx - 2, cx + 2):
            if 0 <= x < size and 0 <= y < size:
                # core bright, edges oranger
                edge = abs(x - cx) + (top - y)
                if edge <= 1:
                    img.putpixel((x, y), (255, 240, 160, 255))
                elif edge <= 3:
                    img.putpixel((x, y), (255, 180, 60, 255))
    return img

def draw_path(size=16):
    """Desaturated gravel path: dirt base with scattered grey pebbles."""
    img = Image.new('RGBA', (size, size))
    for y in range(size):
        for x in range(size):
            img.putpixel((x, y), _noise(120, 104, 82, 255, x, y, 10))
    rng = random.Random(77)
    for _ in range(28):
        px, py = rng.randint(0, size-1), rng.randint(0, size-1)
        v = rng.randint(95, 140)
        img.putpixel((px, py), (v, v, int(v*0.95), 255))
    return img

# ── tools ──────────────────────────────────────────────────────────────────────
#
# Tool icons share a wooden handle running corner-to-corner; the head colour
# encodes the material tier (wood/stone/iron). Each is drawn on a transparent
# 16×16 tile so it reads on the hotbar over any background.

HANDLE = (120, 82, 44, 255)   # wooden stick

TIER_HEAD = {
    'wood':    (156, 110,  58, 255),
    'stone':   (130, 130, 130, 255),
    'iron':    (216, 216, 222, 255),
    'diamond': ( 92, 214, 213, 255),
}

def _draw_handle(img, size):
    """Diagonal wooden stick from lower-left toward upper-right."""
    for i in range(2, size - 2):
        x = i
        y = size - 1 - i
        for (dx, dy) in ((0, 0), (1, 0), (0, 1)):
            px, py = x + dx, y + dy
            if 0 <= px < size and 0 <= py < size:
                img.putpixel((px, py), HANDLE)

def draw_pickaxe(tier, size=16):
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    _draw_handle(img, size)
    head = TIER_HEAD[tier]
    # Curved head across the top: a wide arc of head pixels.
    for x in range(2, size - 2):
        y = 2 + (abs(x - size // 2) // 3)
        for dy in (0, 1):
            if 0 <= y + dy < size:
                img.putpixel((x, y + dy), head)
    return img

def draw_axe(tier, size=16):
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    _draw_handle(img, size)
    head = TIER_HEAD[tier]
    # Blade: a solid wedge in the upper-right quadrant.
    for y in range(2, 8):
        for x in range(size - 7, size - 1):
            if (x - (size - 7)) <= (y - 1) * 2:
                img.putpixel((x, y), head)
    return img

def draw_shovel(tier, size=16):
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    _draw_handle(img, size)
    head = TIER_HEAD[tier]
    # Scoop: a small rounded blade at the upper-right tip of the handle.
    for y in range(2, 6):
        for x in range(size - 6, size - 1):
            edge = (x == size - 6 or x == size - 2 or y == 2 or y == 5)
            if not (edge and (x + y) % 2 == 0):
                img.putpixel((x, y), head)
    return img

def draw_sword(tier, size=16):
    """A sword: a short wooden grip at the lower-left and a long tier-coloured
    blade running diagonally to the upper-right, with a small crossguard."""
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    blade = TIER_HEAD[tier]
    guard = (110, 82, 44, 255)
    # Grip: a couple of handle pixels in the lower-left corner.
    for i in range(2, 5):
        x = i
        y = size - 1 - i
        for (dx, dy) in ((0, 0), (1, 0), (0, 1)):
            px, py = x + dx, y + dy
            if 0 <= px < size and 0 <= py < size:
                img.putpixel((px, py), HANDLE)
    # Crossguard near the grip, perpendicular to the blade.
    gx, gy = 5, size - 6
    for (dx, dy) in ((-1, -1), (1, 1), (0, 0)):
        px, py = gx + dx, gy + dy
        if 0 <= px < size and 0 <= py < size:
            img.putpixel((px, py), guard)
    # Blade: thick diagonal toward the upper-right tip.
    for i in range(5, size - 1):
        x = i
        y = size - 1 - i
        for (dx, dy) in ((0, 0), (1, 0), (0, 1)):
            px, py = x + dx, y + dy
            if 0 <= px < size and 0 <= py < size:
                img.putpixel((px, py), blade)
    return img

def draw_stick(size=16):
    """A single diagonal wooden stick — the crafting material icon."""
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    _draw_handle(img, size)
    return img

def draw_furnace(size=16):
    """Stone-faced furnace. Cobble-like body with a dark firebox mouth on the
    front face (tile 50) — the side/top (tile 51) is plain stone via draw_stone."""
    img = draw_stone(size)
    # darker stone frame
    for i in range(size):
        if i == 0 or i == size - 1:
            for j in range(size):
                img.putpixel((i, j), (80, 80, 80, 255))
                img.putpixel((j, i), (80, 80, 80, 255))
    # firebox opening: a dark recessed rectangle with a faint ember glow.
    _fill_rect(img, 4, 7, size - 4, size - 3, (40, 40, 40, 255))
    _fill_rect(img, 5, size - 5, size - 5, size - 4, (150, 70, 20, 255))  # embers
    # a small vent slot above the firebox
    _fill_rect(img, 5, 3, size - 5, 5, (70, 70, 70, 255))
    return img

def draw_furnace_side(size=16):
    """Plain stone side/top for the furnace (tile 51) with a darker frame."""
    img = draw_stone(size)
    for i in range(size):
        if i == 0 or i == size - 1:
            for j in range(size):
                img.putpixel((i, j), (80, 80, 80, 255))
                img.putpixel((j, i), (80, 80, 80, 255))
    return img

def draw_chest_top(size=16):
    """Chest lid (tile 52): planks with a dark rim and a small central latch."""
    img = draw_planks(size)
    for i in range(size):
        if i == 0 or i == size - 1:
            for j in range(size):
                img.putpixel((i, j), (90, 62, 32, 255))
                img.putpixel((j, i), (90, 62, 32, 255))
    # latch in the middle
    _fill_rect(img, size // 2 - 1, size // 2 - 1, size // 2 + 1, size // 2 + 1,
               (70, 70, 74, 255))
    return img

def draw_chest_side(size=16):
    """Chest front/side (tile 53): planks with a dark rim and an iron latch +
    keyhole on the lower-middle, reading as the front of a wooden chest."""
    img = draw_planks(size)
    for i in range(size):
        if i == 0 or i == size - 1:
            for j in range(size):
                img.putpixel((i, j), (90, 62, 32, 255))
                img.putpixel((j, i), (90, 62, 32, 255))
    # horizontal lid seam
    _fill_rect(img, 1, size // 2 - 1, size - 1, size // 2, (90, 62, 32, 255))
    # iron latch + keyhole centred below the seam
    cx = size // 2
    _fill_rect(img, cx - 1, size // 2 - 1, cx + 1, size // 2 + 3, (80, 80, 84, 255))
    img.putpixel((cx, size // 2 + 1), (30, 30, 30, 255))  # keyhole
    return img

# ── armour materials + pieces ───────────────────────────────────────────────

ARMOR_TINT = {
    'leather': (150, 102,  60, 255),
    'iron':    (200, 200, 208, 255),
}

def _fill_rect(img, x0, y0, x1, y1, col):
    for y in range(y0, y1):
        for x in range(x0, x1):
            if 0 <= x < img.size[0] and 0 <= y < img.size[1]:
                img.putpixel((x, y), col)

def draw_leather(size=16):
    """A folded tan hide — the leather crafting material."""
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    col = (150, 102, 60, 255)
    _fill_rect(img, 3, 4, size - 3, size - 4, col)
    # a couple of darker stitch lines
    dark = (110, 72, 40, 255)
    for x in range(3, size - 3):
        img.putpixel((x, 7), dark)
        img.putpixel((x, size - 7), dark)
    return img

def draw_iron_ingot(size=16):
    """A grey iron ingot bar."""
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    col = (216, 216, 220, 255)
    edge = (150, 150, 156, 255)
    _fill_rect(img, 2, 6, size - 2, size - 4, col)
    for x in range(2, size - 2):
        img.putpixel((x, 6), edge)
        img.putpixel((x, size - 5), edge)
    return img

def draw_helmet(tier, size=16):
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    col = ARMOR_TINT[tier]
    _fill_rect(img, 3, 3, size - 3, 9, col)     # dome
    _fill_rect(img, 3, 9, 6, 12, col)           # left cheek
    _fill_rect(img, size - 6, 9, size - 3, 12, col)  # right cheek
    return img

def draw_chestplate(tier, size=16):
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    col = ARMOR_TINT[tier]
    _fill_rect(img, 2, 3, size - 2, 5, col)     # shoulders
    _fill_rect(img, 4, 5, size - 4, size - 3, col)  # torso
    return img

def draw_leggings(tier, size=16):
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    col = ARMOR_TINT[tier]
    _fill_rect(img, 3, 2, size - 3, 6, col)     # waist
    _fill_rect(img, 3, 6, 7, size - 2, col)     # left leg
    _fill_rect(img, size - 7, 6, size - 3, size - 2, col)  # right leg
    return img

def draw_boots(tier, size=16):
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    col = ARMOR_TINT[tier]
    _fill_rect(img, 3, 7, 7, size - 2, col)     # left boot
    _fill_rect(img, size - 7, 7, size - 3, size - 2, col)  # right boot
    _fill_rect(img, 3, size - 4, size - 3, size - 2, col)  # soles
    return img

# ── mob-loot materials + foods ──────────────────────────────────────────────
#
# Drawn on transparent 16×16 tiles so they read on the hotbar over any
# background. Materials (feather/bone/arrow/gunpowder) and foods (raw/cooked
# meats + rotten flesh).

def draw_feather(size=16):
    """A pale plume: a diagonal quill with a soft vane."""
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    quill = (210, 210, 215, 255)
    vane  = (240, 240, 245, 255)
    for i in range(2, size - 2):
        x = i
        y = size - 1 - i
        if 0 <= x < size and 0 <= y < size:
            img.putpixel((x, y), quill)
        # vane to one side of the shaft, thinning toward the tip
        w = max(1, (size - 2 - i) // 3)
        for d in range(1, w + 1):
            px, py = x - d, y - d
            if 0 <= px < size and 0 <= py < size:
                img.putpixel((px, py), vane)
    return img

def draw_bone(size=16):
    """A bone: a pale shaft with knobbed ends, drawn diagonally."""
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    col = (230, 226, 205, 255)
    # shaft
    for i in range(4, size - 4):
        x = i
        y = size - 1 - i
        for (dx, dy) in ((0, 0), (1, 0)):
            px, py = x + dx, y + dy
            if 0 <= px < size and 0 <= py < size:
                img.putpixel((px, py), col)
    # knobs at both ends
    for (cx, cy) in ((4, size - 5), (size - 5, 4)):
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                px, py = cx + dx, cy + dy
                if 0 <= px < size and 0 <= py < size:
                    img.putpixel((px, py), col)
    return img

def draw_arrow(size=16):
    """An arrow: brown shaft, grey flint tip, pale fletching."""
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    shaft = (140, 100, 60, 255)
    tip   = (170, 170, 176, 255)
    fletch = (225, 225, 230, 255)
    for i in range(2, size - 2):
        x = i
        y = size - 1 - i
        if 0 <= x < size and 0 <= y < size:
            img.putpixel((x, y), shaft)
    # tip near upper-right
    for (cx, cy) in ((size - 3, 2), (size - 4, 3), (size - 3, 3), (size - 4, 2)):
        if 0 <= cx < size and 0 <= cy < size:
            img.putpixel((cx, cy), tip)
    # fletching near lower-left
    for (cx, cy) in ((3, size - 4), (4, size - 3), (2, size - 3)):
        if 0 <= cx < size and 0 <= cy < size:
            img.putpixel((cx, cy), fletch)
    return img

def draw_gunpowder(size=16):
    """A pile of dark grey dust with lighter speckles."""
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    rng = random.Random(909)
    cx, cy = size // 2, size // 2 + 2
    for y in range(size):
        for x in range(size):
            d = ((x - cx) ** 2 + ((y - cy) * 1.4) ** 2) ** 0.5
            if d < 6:
                v = rng.randint(45, 75)
                img.putpixel((x, y), (v, v, v + 4, 255))
    for _ in range(8):
        px, py = rng.randint(cx - 5, cx + 5), rng.randint(cy - 4, cy + 4)
        if 0 <= px < size and 0 <= py < size:
            img.putpixel((px, py), (110, 110, 116, 255))
    return img

def _draw_meat(base, cooked, size=16):
    """A rounded chunk of meat. `base`/`cooked` are flesh colours; cooked tints
    the surface darker/browner."""
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    cx, cy = size // 2, size // 2
    for y in range(size):
        for x in range(size):
            d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
            if d <= 5.5:
                col = cooked if d > 3.5 else base
                img.putpixel((x, y), _noise(col[0], col[1], col[2], 255, x, y, 10))
    return img

def draw_raw_pork(size=16):
    return _draw_meat((235, 150, 150), (220, 130, 130), size)

def draw_cooked_pork(size=16):
    return _draw_meat((190, 120, 70), (150, 92, 52), size)

def draw_raw_beef(size=16):
    return _draw_meat((200, 70, 70), (170, 55, 55), size)

def draw_cooked_beef(size=16):
    return _draw_meat((140, 88, 56), (110, 66, 42), size)

def draw_raw_chicken(size=16):
    return _draw_meat((240, 200, 180), (225, 180, 160), size)

def draw_cooked_chicken(size=16):
    return _draw_meat((210, 165, 100), (180, 135, 78), size)

def draw_rotten_flesh(size=16):
    """A sickly greenish-brown lump of flesh."""
    img = _draw_meat((120, 100, 78), (95, 80, 60), size)
    # a few green rot speckles
    rng = random.Random(666)
    cx, cy = size // 2, size // 2
    for _ in range(6):
        px, py = rng.randint(cx - 4, cx + 4), rng.randint(cy - 4, cy + 4)
        if 0 <= px < size and 0 <= py < size and img.getpixel((px, py))[3] > 0:
            img.putpixel((px, py), (95, 120, 70, 255))
    return img

def draw_sandstone(size=16):
    """Pale sandy stone: sand-toned base with faint horizontal strata lines and
    a slightly darker top/bottom band, reading as compacted layered rock."""
    img = Image.new('RGBA', (size, size))
    for y in range(size):
        for x in range(size):
            img.putpixel((x, y), _noise(214, 200, 150, 255, x, y, 8))
    # faint horizontal strata seams
    for y in (4, 8, 12):
        for x in range(size):
            img.putpixel((x, y), _noise(190, 174, 124, 255, x, y, 6))
    # darker capping bands top and bottom
    for x in range(size):
        img.putpixel((x, 0), (188, 172, 122, 255))
        img.putpixel((x, size - 1), (188, 172, 122, 255))
    return img

def draw_snow(size=16):
    """Bright near-white snow with a faint cool tint and sparse pale speckles."""
    img = Image.new('RGBA', (size, size))
    for y in range(size):
        for x in range(size):
            img.putpixel((x, y), _noise(238, 242, 248, 255, x, y, 6))
    rng = random.Random(31)
    for _ in range(10):
        px, py = rng.randint(0, size-1), rng.randint(0, size-1)
        img.putpixel((px, py), (252, 254, 255, 255))
    return img

def draw_ice(size=16):
    """Translucent pale-blue ice with brighter cracks and a glossy corner glint.
    Semi-transparent so the block reads as see-through like glass."""
    img = Image.new('RGBA', (size, size))
    for y in range(size):
        for x in range(size):
            img.putpixel((x, y), _noise(150, 192, 232, 150, x, y, 8))
    # a few brighter cracks
    rng = random.Random(63)
    for _ in range(5):
        x0 = rng.randint(1, size - 2)
        y0 = rng.randint(1, size - 2)
        for k in range(rng.randint(2, 5)):
            px, py = min(size - 1, x0 + k), min(size - 1, y0 + k)
            img.putpixel((px, py), (210, 232, 248, 200))
    # diagonal glint
    for i in range(2, 6):
        img.putpixel((i, i + 1), (235, 248, 255, 220))
    return img

def draw_mossy_cobblestone(size=16):
    """Cobblestone overgrown with moss: cobble base with green moss patches in
    the mortar seams and a few scattered green tufts."""
    img = draw_cobble(size)
    rng = random.Random(88)
    # moss in the mortar grid cells
    for y in range(size):
        for x in range(size):
            if rng.random() < 0.22:
                base = img.getpixel((x, y))
                # blend toward mossy green, keeping some of the stone luminance
                lum = (base[0] + base[1] + base[2]) // 3
                g = min(255, 90 + lum // 3)
                img.putpixel((x, y), (max(0, 40 + lum // 4), g, max(0, 40 + lum // 6), 255))
    # a few brighter moss tufts
    for _ in range(8):
        px, py = rng.randint(0, size-1), rng.randint(0, size-1)
        img.putpixel((px, py), (70, 140, 60, 255))
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
    8:  draw_coal_ore,
    9:  draw_iron_ore,
    10: draw_gold_ore,
    11: draw_diamond_ore,
    12: draw_planks,
    13: draw_cobble,
    14: draw_glass,
    15: draw_path,
    16: draw_water,
    17: draw_bedrock,
    18: draw_torch,
    # Tool icons (free atlas indices after torch=18).
    19: lambda: draw_pickaxe('wood'),
    20: lambda: draw_axe('wood'),
    21: lambda: draw_shovel('wood'),
    22: lambda: draw_pickaxe('stone'),
    23: lambda: draw_axe('stone'),
    24: lambda: draw_shovel('stone'),
    25: lambda: draw_pickaxe('iron'),
    26: lambda: draw_axe('iron'),
    27: lambda: draw_shovel('iron'),
    # Crafting materials (free atlas indices after the tools).
    28: draw_stick,
    29: draw_leather,
    30: draw_iron_ingot,
    # Armour pieces (leather then iron, helmet/chest/legs/boots).
    31: lambda: draw_helmet('leather'),
    32: lambda: draw_chestplate('leather'),
    33: lambda: draw_leggings('leather'),
    34: lambda: draw_boots('leather'),
    35: lambda: draw_helmet('iron'),
    36: lambda: draw_chestplate('iron'),
    37: lambda: draw_leggings('iron'),
    38: lambda: draw_boots('iron'),
    # Mob-loot crafting materials (free atlas indices after armour).
    39: draw_feather,
    40: draw_bone,
    41: draw_arrow,
    42: draw_gunpowder,
    # Mob-loot foods (raw/cooked meats + rotten flesh).
    43: draw_raw_pork,
    44: draw_cooked_pork,
    45: draw_raw_beef,
    46: draw_cooked_beef,
    47: draw_raw_chicken,
    48: draw_cooked_chicken,
    49: draw_rotten_flesh,
    # Crafting/storage blocks (furnace front/side, chest top/side).
    50: draw_furnace,
    51: draw_furnace_side,
    52: draw_chest_top,
    53: draw_chest_side,
    # Diamond mining tools.
    54: lambda: draw_pickaxe('diamond'),
    55: lambda: draw_axe('diamond'),
    56: lambda: draw_shovel('diamond'),
    # Swords (wood/stone/iron/diamond).
    57: lambda: draw_sword('wood'),
    58: lambda: draw_sword('stone'),
    59: lambda: draw_sword('iron'),
    60: lambda: draw_sword('diamond'),
    # Natural/decorative terrain blocks.
    61: draw_sandstone,
    62: draw_snow,
    63: draw_ice,
    64: draw_mossy_cobblestone,
}

TILE_NAMES = {
    0: "stone", 1: "dirt", 2: "grass_top", 3: "grass_side", 4: "sand",
    5: "wood_top", 6: "wood_side", 7: "leaves",
    8: "coal_ore", 9: "iron_ore", 10: "gold_ore", 11: "diamond_ore",
    12: "planks", 13: "cobble", 14: "glass", 15: "path",
    16: "water", 17: "bedrock", 18: "torch",
    19: "wood_pickaxe",  20: "wood_axe",  21: "wood_shovel",
    22: "stone_pickaxe", 23: "stone_axe", 24: "stone_shovel",
    25: "iron_pickaxe",  26: "iron_axe",  27: "iron_shovel",
    28: "stick", 29: "leather", 30: "iron_ingot",
    31: "leather_helmet", 32: "leather_chestplate",
    33: "leather_leggings", 34: "leather_boots",
    35: "iron_helmet", 36: "iron_chestplate",
    37: "iron_leggings", 38: "iron_boots",
    39: "feather", 40: "bone", 41: "arrow", 42: "gunpowder",
    43: "raw_pork", 44: "cooked_pork", 45: "raw_beef", 46: "cooked_beef",
    47: "raw_chicken", 48: "cooked_chicken", 49: "rotten_flesh",
    50: "furnace_front", 51: "furnace_side", 52: "chest_top", 53: "chest_side",
    54: "diamond_pickaxe", 55: "diamond_axe", 56: "diamond_shovel",
    57: "wood_sword", 58: "stone_sword", 59: "iron_sword", 60: "diamond_sword",
    61: "sandstone", 62: "snow", 63: "ice", 64: "mossy_cobblestone",
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
