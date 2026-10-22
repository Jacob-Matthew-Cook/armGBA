# Copyright (c) 2026 Jacob Cook
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#
# splash.py font.ttf: writes splash-data.inc, the power-on splash's picture and colours, from Chakra
# Petch Bold (SIL Open Font License, Cadson Demak). Needs Pillow.
#
# "ARMGBA" over "Open BIOS", spaced by ink: each pair of letters sits so that the narrowest gap
# between them, row by row and against the rows either side, is exactly 3 pixels (2 in "Open BIOS").
# Coverage is kept at 4 levels, 2 bits a pixel, each row XORed with the one above, in the GBA's LZ77
# format. Each title letter rises from the bottom of the screen at three times its size, easing in
# and out and shrinking as it goes, magenta, then turning through the colour wheel at the rate the
# reference's logo does: red, orange, yellow, green, cyan, and blue as it goes a little past its place
# and settles back; "Open BIOS" goes from white to black in 4-pixel bands.
# The letters are 64x64 sprites, double size, which leaves room for 3x since none is over 42 wide.
# As the chime's last note comes, a gold spark, a 32x32 sprite drawn by splash.s, flashes on the top
# right of the last letter of "Open BIOS".
import sys
from PIL import Image, ImageDraw, ImageFont

TITLE, SUBTITLE = 'ARMGBA', 'Open BIOS'
TITLE_GAP, SUBTITLE_GAP, LINE_GAP, BAND = 3, 2, 10, 4
RISE, BOUNCE = 30, 10  # frames rising and settling
START, START_SIZE, OVERSHOOT = 96, 3, 4  # pixels below its place it starts, at what size; how far past
CELL = 64
# The title's top line, on the reference's title's.
TITLE_TOP = 43
SPARK_FRAMES, SPARK_SIZE, GOLD = 20, 2.0, (1.0, 0.74, 0.08)


def glyph(font, ch):
    m = Image.new('L', (200, 200), 0)
    ImageDraw.Draw(m).text((50, 50), ch, font=font, fill=255)
    box = m.getbbox()
    return m.crop((box[0], 0, box[2], 200))


def level(v):
    return round(v / 255 * 3)


def edges(g):
    left, right = {}, {}
    for y in range(g.height):
        xs = [x for x in range(g.width) if level(g.getpixel((x, y)))]
        if xs:
            left[y], right[y] = xs[0], xs[-1]
    return left, right


def layout(font, text, gap, space):
    placed, x, prev = [], 0, None
    for ch in text:
        if ch == ' ':
            x += space
            prev = None
            continue
        g = glyph(font, ch)
        left, right = edges(g)
        pos = x
        if prev:
            pg, px, pright = prev
            need = [px + max(pright.get(z, -10**6) for z in (y - 1, y, y + 1)) - left[y] + 1 + gap for y in left]
            need = [n for n in need if n > -10**5]
            pos = max(need) if need else px + pg.width + gap
        placed.append((ch, g, pos))
        prev = (g, pos, right)
        x = pos + g.width
    return placed, x


def raster(placed, width):
    rows = [y for _, g, _ in placed for y in range(g.height) if any(level(g.getpixel((x, y))) for x in range(g.width))]
    top, bottom = min(rows), max(rows)
    out = []
    for y in range(top, bottom + 1):
        row = [0] * width
        for _, g, pos in placed:
            for x in range(g.width):
                row[pos + x] = max(row[pos + x], level(g.getpixel((x, y))))
        out.append(row)
    return out


def lz77(data):
    # The fewest bytes: from the end back, the cheapest way on from each place, a literal (9 bits
    # with its flag) or a match of 3 to 18 bytes up to 4096 back (17 bits).
    n = len(data)
    cost = [0] * (n + 1)
    step = [(0, 0)] * n
    for i in range(n - 1, -1, -1):
        cost[i], step[i] = 9 + cost[i + 1], (0, 0)
        for d in range(1, min(i, 4096) + 1):
            k = 0
            while k < 18 and i + k < n and data[i + k - d] == data[i + k]:
                k += 1
                if k >= 3 and 17 + cost[i + k] < cost[i]:
                    cost[i], step[i] = 17 + cost[i + k], (k, d)
    out = bytearray([0x10, n & 0xFF, n >> 8 & 0xFF, n >> 16])
    i = 0
    while i < n:
        flagpos = len(out)
        out.append(0)
        for bit in range(8):
            if i >= n:
                break
            k, d = step[i]
            if k:
                out[flagpos] |= 0x80 >> bit
                out += bytes([(k - 3) << 4 | (d - 1) >> 8, (d - 1) & 0xFF])
                i += k
            else:
                out.append(data[i])
                i += 1
    while len(out) % 4:
        out.append(0)
    return out


def rgb555(r, g, b):
    return round(r * 31) | round(g * 31) << 5 | round(b * 31) << 10


def blend(color, amount):
    return [1 + (c - 1) * amount for c in color]


def main():
    path = sys.argv[1]
    size = 70
    while True:
        font = ImageFont.truetype(path, size)
        title, width = layout(font, TITLE, TITLE_GAP, 0)
        if width <= 196:
            break
        size -= 1
    small = ImageFont.truetype(path, round(size * 0.42))
    subtitle, swidth = layout(small, SUBTITLE, SUBTITLE_GAP, round(size * 0.16))
    w = max(width, swidth)
    w += w & 1
    t, s = raster(title, width), raster(subtitle, swidth)
    tx, sx = (w - width) // 2 & ~1, (w - swidth) // 2 & ~1
    sy = len(t) + LINE_GAP
    h = sy + len(s)
    cover = [[0] * w for _ in range(h)]
    for y, row in enumerate(t):
        cover[y][tx:tx + width] = row
    for y, row in enumerate(s):
        cover[sy + y][sx:sx + swidth] = row
    while w % 4:
        w += 1
        for row in cover:
            row.append(0)
    rows = [cover[0]] + [[a ^ b for a, b in zip(cover[y], cover[y - 1])] for y in range(1, h)]
    flat = [v for row in rows for v in row]
    packed = bytes(flat[i] | flat[i + 1] << 2 | flat[i + 2] << 4 | flat[i + 3] << 6 for i in range(0, len(flat), 4))
    logo = lz77(packed)
    bands = (swidth + BAND - 1) // BAND
    # Each letter's place in the picture, its width, and its sprite's x: the letter centred in its
    # 64x64 cell, whose double-size box is 32 more each side.
    x = (240 - w) // 2 & ~1
    letters = []
    for ch, g, pos in title:
        lw = g.width
        letters.append((tx + pos, lw, (x + tx + pos - (CELL - lw) // 2 - CELL // 2) & 0x1FF))
    row0 = (CELL - len(t)) // 2
    home_y = TITLE_TOP - row0 - CELL // 2
    # Frame by frame from when a letter comes in: how far below its place it is (above, negative),
    # and while it rises its sprite's scale as the affine parameter (256 / size). The rise eases in
    # and out.
    import math
    offset, scale = [], []
    for f in range(RISE + BOUNCE + 1):
        if f < RISE:
            p = f / RISE
            rest = 1 - (3 * p * p - 2 * p * p * p)
            offset.append(round(START * rest))
            scale.append(round(256 / (1 + (START_SIZE - 1) * rest)))
        else:
            offset.append(-round(OVERSHOOT * math.sin(math.pi * (f - RISE) / BOUNCE)))

    # The spark, frame by frame from the last note: its size as the affine parameter, popping up then
    # shrinking, and its colour, gold fading out to the white in its second half.
    spark_scale, spark_colour = [], []
    for f in range(SPARK_FRAMES):
        size = SPARK_SIZE * (f + 1) / 4 if f < 4 else SPARK_SIZE - (f - 3) * (SPARK_SIZE - 0.2) / (SPARK_FRAMES - 4)
        spark_scale.append(round(256 / size))
        fade = 1 if f < SPARK_FRAMES // 2 else (SPARK_FRAMES - 1 - f) / (SPARK_FRAMES // 2 - 1)
        spark_colour.append(rgb555(*blend(GOLD, fade)))
    # Its double-size box centred on the top right of the last letter of "Open BIOS".
    last = subtitle[-1]
    cx = x + sx + last[2] + last[1].width - 2
    cy = TITLE_TOP + sy + 2
    spark_x, spark_y = (cx - 32) & 0x1FF, (cy - 32) & 0xFF

    # Each title letter's colour, frame by frame from when it comes in: magenta, then one channel at a
    # time moving 4 a frame, as the reference's logo is seen to, round to red, yellow, green, cyan and
    # blue. splash.s works out the lighter coverage levels from it.
    down, up = [27, 23, 19, 15, 11, 7, 3, 0], [3, 7, 11, 15, 19, 23, 27, 31]
    path = [(31, 0, 31)] + [(31, 0, b) for b in down] + [(31, g, 0) for g in up] + [(r, 31, 0) for r in down] \
        + [(0, 31, b) for b in up] + [(0, g, 31) for g in down]
    assert len(path) == RISE + BOUNCE + 1
    sweep = [r | g << 5 | b << 10 for r, g, b in path]
    # A band of "Open BIOS" over 4 frames, white to black.
    fade = [rgb555(*blend((0, 0, 0), step / 3)) for step in range(4)]

    with open('splash-data.inc', 'w') as f:
        f.write('@ Written by splash.py from Chakra Petch Bold (SIL Open Font License, Cadson Demak).\n')
        f.write('.equ SPLASH_WIDTH, %d\n.equ SPLASH_HEIGHT, %d\n' % (w, h))
        f.write('.equ SPLASH_X, %d\n.equ SPLASH_Y, %d\n' % ((240 - w) // 2 & ~1, TITLE_TOP))
        f.write('.equ SPLASH_SUBTITLE_Y, %d\n.equ SPLASH_SUBTITLE_X, %d\n' % (sy, sx))
        f.write('.equ SPLASH_LETTERS, %d\n.equ SPLASH_BANDS, %d\n' % (len(letters), bands))
        f.write('.equ SPLASH_SWEEP, %d\n.equ SPLASH_PACKED, %d\n' % (len(path), len(packed)))
        f.write('.equ SPLASH_RISE, %d\n.equ SPLASH_TITLE_HEIGHT, %d\n' % (RISE, len(t)))
        f.write('.equ SPLASH_CELL_ROW, %d\n.equ SPLASH_HOME_Y, %d\n' % (row0, home_y))
        f.write('@ Each letter: its first column in the picture, its width, its sprite\'s x.\nsplashLetters:\n')
        for x0, lw, ax in letters:
            f.write('\t.byte %d, %d\n\t.hword %d\n' % (x0, lw, ax))
        f.write('splashOffset:\n\t.byte %s\n\t.balign 2\n' % ', '.join(str(v & 0xFF) for v in offset))
        f.write('splashScale:\n\t.hword %s\n' % ', '.join(str(v) for v in scale))
        f.write('splashSweep:\n')
        for i in range(0, len(sweep), 12):
            f.write('\t.hword %s\n' % ', '.join('0x%04X' % c for c in sweep[i:i + 12]))
        f.write('splashFade:\n\t.hword %s\n' % ', '.join('0x%04X' % c for c in fade))
        f.write('.equ SPARK_FRAMES, %d\n.equ SPARK_X, %d\n.equ SPARK_Y, %d\n' % (SPARK_FRAMES, spark_x, spark_y))
        f.write('sparkScale:\n\t.hword %s\n' % ', '.join(str(v) for v in spark_scale))
        f.write('sparkColour:\n')
        for i in range(0, len(spark_colour), 12):
            f.write('\t.hword %s\n' % ', '.join('0x%04X' % c for c in spark_colour[i:i + 12]))
        f.write('\t.balign 4\nsplashLogo:\n')
        for i in range(0, len(logo), 16):
            f.write('\t.byte %s\n' % ', '.join('0x%02X' % b for b in logo[i:i + 16]))
    print('splash-data.inc: %dx%d, logo %d bytes' % (w, h, len(logo)))


main()
