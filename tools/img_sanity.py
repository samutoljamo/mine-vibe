#!/usr/bin/env python3
"""
img_sanity.py - render smoke-test image analyzer for the minecraft Vulkan game.

Given a PNG produced by `./build/minecraft --screenshot <path> [frames]`, this
computes a handful of cheap statistics and asserts the frame looks like a
plausible rendered 3D scene. It exists to catch *visual corruption* in CI that
unit tests can't see, e.g. the past greedy-mesher bug that turned terrain into
random static, or a "nothing rendered" regression that yields a blank frame.

Usage:
  python3 tools/img_sanity.py <png>     # exit 0 = looks good, 1 = corrupt
  python3 tools/img_sanity.py --self-test
                                        # synthesize good/blank/noise images and
                                        # assert the checker classifies each
                                        # correctly (no GPU/game needed)

Dependencies: Python stdlib + Pillow if available. Pillow is already used by
tools/gen_assets.py so it is present in the build/CI env. If Pillow is missing,
a tiny built-in PNG decoder (zlib + stdlib only) handles the
8-bit RGB/RGBA non-interlaced PNGs that stbi_write_png emits.

------------------------------------------------------------------------------
HEURISTICS & THRESHOLDS  (all tuned to PASS a normal frame, FAIL blank/noise)
------------------------------------------------------------------------------
The image is decoded to RGB and downsampled to at most ANALYZE_DIM on the long
edge (keeps everything O(small) and smooths away sensor-level dither). All
metrics run on that small luma/RGB buffer.

1. NOT BLANK / UNIFORM
   - distinct_colors: number of unique quantized (>>3, i.e. 32 levels/chan)
     colors. A solid fill or all-black frame has ~1. Real scenes have many.
     FAIL if distinct_colors < MIN_DISTINCT_COLORS (8).
   - luma_stddev: standard deviation of per-pixel luma (0..255). A uniform
     frame is ~0. FAIL if luma_stddev < MIN_LUMA_STDDEV (4.0).

2. NOT STATIC-NOISE GARBAGE
   - neighbor_corr: a smoothness metric = mean absolute luma difference between
     horizontally-adjacent pixels, normalized to 0..1 by /255 and reported as
     "roughness". Pure random static has large adjacent differences
     (roughness ~0.25-0.33); a real scene (sky gradient + flat-shaded terrain
     faces) is far smoother (roughness < ~0.05). FAIL if
     roughness > MAX_ROUGHNESS (0.18).
   - Equivalently we compute neighbor correlation; we report roughness because
     it is robust to brightness and trivially bounded.

3. REASONABLE COLOR DISTRIBUTION
   - magenta_frac: fraction of pixels close to pure magenta (255,0,255), the
     "missing texture" debug color. FAIL if magenta_frac > MAX_MAGENTA_FRAC
     (0.50) -- a frame that is mostly missing-texture magenta is broken.
   - sky_top_brightness: mean luma of the top 15% of rows. A normal daytime
     frame has a brightish sky band up top. This is a SOFT signal only (a cave
     / night frame legitimately has a dark top), so it is reported but NOT a
     hard failure on its own -- it only contributes if EVERYTHING is dark, which
     the blank check already covers.

Thresholds are deliberately lenient: the bar is "distinguish a real frame from
blank/uniform/static", not "grade image quality".
"""

import sys
import struct
import zlib
import math

# ---- tunables (documented above) ----
ANALYZE_DIM        = 256     # long-edge px the analysis runs at
COLOR_QUANT_SHIFT  = 3       # >>3 -> 32 levels per channel for distinct-color count
MIN_DISTINCT_COLORS = 8
MIN_LUMA_STDDEV    = 4.0
MAX_ROUGHNESS      = 0.18
MAX_MAGENTA_FRAC   = 0.50


# --------------------------------------------------------------------------
# Image loading: prefer Pillow, fall back to a minimal stdlib PNG decoder.
# Returns (width, height, list-of-(r,g,b) rows? ) -> we return a flat RGB
# bytes-like + dims to keep it simple.
# --------------------------------------------------------------------------
def _load_rgb(path):
    """Return (width, height, bytes) where bytes is row-major RGB (3*w*h)."""
    try:
        from PIL import Image
        im = Image.open(path).convert("RGB")
        return im.width, im.height, im.tobytes()
    except ImportError:
        return _decode_png_min(path)


def _decode_png_min(path):
    """Minimal PNG decoder: 8-bit RGB/RGBA, non-interlaced. stdlib only."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG file")
    pos = 8
    width = height = bit_depth = color_type = None
    idat = bytearray()
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length  # 4 len + 4 type + data + 4 crc
        if ctype == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(">IIBB", chunk[:10])
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break
    if bit_depth != 8 or color_type not in (2, 6):
        raise ValueError("unsupported PNG (need 8-bit RGB/RGBA non-interlaced)")
    channels = 3 if color_type == 2 else 4
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(width * height * 3)
    prev = bytearray(stride)
    p = 0
    for y in range(height):
        filt = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        # undo PNG row filter
        if filt == 1:    # Sub
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filt == 2:  # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif filt == 3:  # Average
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif filt == 4:  # Paeth
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        # pack to RGB
        for x in range(width):
            si = x * channels
            di = (y * width + x) * 3
            out[di] = line[si]; out[di + 1] = line[si + 1]; out[di + 2] = line[si + 2]
        prev = line
    return width, height, bytes(out)


def _downsample(w, h, rgb, target):
    """Nearest-ish downsample to <= target on long edge. Returns (w2,h2,rgb2)."""
    scale = max(w, h) / float(target)
    if scale <= 1.0:
        return w, h, rgb
    w2 = max(1, int(w / scale))
    h2 = max(1, int(h / scale))
    out = bytearray(w2 * h2 * 3)
    for y in range(h2):
        sy = int(y * scale)
        for x in range(w2):
            sx = int(x * scale)
            si = (sy * w + sx) * 3
            di = (y * w2 + x) * 3
            out[di] = rgb[si]; out[di + 1] = rgb[si + 1]; out[di + 2] = rgb[si + 2]
    return w2, h2, bytes(out)


# --------------------------------------------------------------------------
# Metrics
# --------------------------------------------------------------------------
def _luma(r, g, b):
    return 0.299 * r + 0.587 * g + 0.114 * b


def compute_stats(w, h, rgb):
    n = w * h
    distinct = set()
    sum_l = 0.0
    sum_l2 = 0.0
    magenta = 0
    # per-row for neighbor roughness
    rough_sum = 0.0
    rough_cnt = 0
    top_rows = max(1, int(h * 0.15))
    top_l_sum = 0.0
    top_l_cnt = 0

    lumas = [0.0] * n
    for y in range(h):
        prev_l = None
        for x in range(w):
            i = (y * w + x) * 3
            r, g, b = rgb[i], rgb[i + 1], rgb[i + 2]
            l = _luma(r, g, b)
            lumas[y * w + x] = l
            sum_l += l
            sum_l2 += l * l
            distinct.add(((r >> COLOR_QUANT_SHIFT), (g >> COLOR_QUANT_SHIFT), (b >> COLOR_QUANT_SHIFT)))
            if r > 200 and b > 200 and g < 60:
                magenta += 1
            if prev_l is not None:
                rough_sum += abs(l - prev_l)
                rough_cnt += 1
            prev_l = l
            if y < top_rows:
                top_l_sum += l
                top_l_cnt += 1

    mean_l = sum_l / n
    var_l = max(0.0, sum_l2 / n - mean_l * mean_l)
    return {
        "width": w, "height": h, "pixels": n,
        "distinct_colors": len(distinct),
        "luma_mean": mean_l,
        "luma_stddev": math.sqrt(var_l),
        "roughness": (rough_sum / rough_cnt / 255.0) if rough_cnt else 0.0,
        "magenta_frac": magenta / n,
        "sky_top_brightness": (top_l_sum / top_l_cnt) if top_l_cnt else 0.0,
    }


def evaluate(stats):
    """Return (ok: bool, reasons: list[str]) given a stats dict."""
    reasons = []
    if stats["distinct_colors"] < MIN_DISTINCT_COLORS:
        reasons.append(
            f"blank/uniform: only {stats['distinct_colors']} distinct colors "
            f"(< {MIN_DISTINCT_COLORS})")
    if stats["luma_stddev"] < MIN_LUMA_STDDEV:
        reasons.append(
            f"blank/uniform: luma stddev {stats['luma_stddev']:.2f} "
            f"(< {MIN_LUMA_STDDEV})")
    if stats["roughness"] > MAX_ROUGHNESS:
        reasons.append(
            f"static-noise: roughness {stats['roughness']:.3f} "
            f"(> {MAX_ROUGHNESS}) — neighbor pixels uncorrelated like random garbage")
    if stats["magenta_frac"] > MAX_MAGENTA_FRAC:
        reasons.append(
            f"missing-texture: {stats['magenta_frac'] * 100:.0f}% magenta "
            f"(> {MAX_MAGENTA_FRAC * 100:.0f}%)")
    return (len(reasons) == 0), reasons


def check_file(path, verbose=True):
    w, h, rgb = _load_rgb(path)
    w, h, rgb = _downsample(w, h, rgb, ANALYZE_DIM)
    stats = compute_stats(w, h, rgb)
    ok, reasons = evaluate(stats)
    if verbose:
        print(f"img_sanity: {path}")
        print(f"  distinct_colors    = {stats['distinct_colors']}")
        print(f"  luma_mean          = {stats['luma_mean']:.2f}")
        print(f"  luma_stddev        = {stats['luma_stddev']:.2f}")
        print(f"  roughness          = {stats['roughness']:.3f}")
        print(f"  magenta_frac       = {stats['magenta_frac']:.3f}")
        print(f"  sky_top_brightness = {stats['sky_top_brightness']:.2f}")
        if ok:
            print("  RESULT: PASS (plausible rendered scene)")
        else:
            print("  RESULT: FAIL")
            for r in reasons:
                print(f"    - {r}")
    return ok, stats, reasons


# --------------------------------------------------------------------------
# Self-test: synthesize known-good / known-bad images and verify classification.
# Needs no GPU and no real screenshot, so it can be a HARD gate in CI.
# --------------------------------------------------------------------------
def _synth_gradient(w=160, h=120):
    """A plausible 'scene': sky gradient up top, a few flat terrain bands below."""
    out = bytearray(w * h * 3)
    horizon = int(h * 0.55)
    for y in range(h):
        for x in range(w):
            i = (y * w + x) * 3
            if y < horizon:
                # blue->light sky gradient
                t = y / horizon
                out[i] = int(120 + 80 * t)
                out[i + 1] = int(160 + 70 * t)
                out[i + 2] = int(220 + 30 * t)
            else:
                # blocky terrain: flat green/brown bands -> smooth, many colors
                band = ((y // 8) + (x // 24)) % 3
                if band == 0:
                    out[i], out[i + 1], out[i + 2] = 70, 130, 60
                elif band == 1:
                    out[i], out[i + 1], out[i + 2] = 110, 80, 50
                else:
                    out[i], out[i + 1], out[i + 2] = 90, 90, 95
    return w, h, bytes(out)


def _synth_uniform(w=160, h=120, color=(0, 0, 0)):
    out = bytearray(w * h * 3)
    for i in range(0, len(out), 3):
        out[i], out[i + 1], out[i + 2] = color
    return w, h, bytes(out)


def _synth_noise(w=160, h=120, seed=1):
    """Pure pseudo-random static (the corruption signature)."""
    out = bytearray(w * h * 3)
    s = seed & 0xFFFFFFFF
    for i in range(len(out)):
        # xorshift32
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        out[i] = s & 0xFF
    return w, h, bytes(out)


def _synth_magenta(w=160, h=120):
    return _synth_uniform(w, h, (255, 0, 255))


def self_test():
    print("img_sanity --self-test")
    cases = []

    def run(name, synth, expect_ok):
        w, h, rgb = synth()
        w, h, rgb = _downsample(w, h, rgb, ANALYZE_DIM)
        stats = compute_stats(w, h, rgb)
        ok, reasons = evaluate(stats)
        passed = (ok == expect_ok)
        verdict = "PASS" if ok else "FAIL"
        marker = "ok" if passed else "WRONG"
        detail = (f"distinct={stats['distinct_colors']} std={stats['luma_stddev']:.1f} "
                  f"rough={stats['roughness']:.3f} magenta={stats['magenta_frac']:.2f}")
        print(f"  [{marker}] {name:18s} classified {verdict:4s} "
              f"(expected {'PASS' if expect_ok else 'FAIL'})  {detail}")
        if not passed:
            for r in reasons:
                print(f"        reason: {r}")
        cases.append(passed)

    run("good-gradient", _synth_gradient, expect_ok=True)
    run("bad-uniform-black", lambda: _synth_uniform(color=(0, 0, 0)), expect_ok=False)
    run("bad-uniform-gray", lambda: _synth_uniform(color=(128, 128, 128)), expect_ok=False)
    run("bad-random-noise", _synth_noise, expect_ok=False)
    run("bad-magenta", _synth_magenta, expect_ok=False)

    all_ok = all(cases)
    print(f"  SELF-TEST: {'PASS' if all_ok else 'FAIL'} "
          f"({sum(cases)}/{len(cases)} cases correct)")
    return all_ok


def main(argv):
    args = argv[1:]
    if not args:
        print("usage: img_sanity.py <png> | --self-test", file=sys.stderr)
        return 2
    if args[0] == "--self-test":
        return 0 if self_test() else 1
    path = args[0]
    try:
        ok, _, _ = check_file(path)
    except Exception as e:
        print(f"img_sanity: ERROR reading {path}: {e}", file=sys.stderr)
        return 1
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
