# cgbdata.py font.ttf out.inc: the data for armGBA's Game Boy Color boot ROM (cgb.asm).
# The art: "ARMGBC" in the font's own hinted 1-bit rendering at half size, centred in a picture of
# TITLE_COLUMNS 4x4 blocks by 3, stored as the cartridge's logo is: a block's 4 rows a nibble each, 2
# bytes, the blocks left to right then top to bottom. The boot ROM doubles each pixel, so a block is a
# tile.
# The compatibility palettes for original Game Boy games come from cgbpalettes.json, measured from
# what the reference boot ROM shows, never from the reference itself. cgbwaits.json, measured the
# same way, holds how long it takes to hand over with each of those titles, and with a few whose
# fourth letter isn't one of those but still changes it: the cycles of the hand-over's extra wait,
# and how many of them more than a plain cartridge's come after the screen comes on.
import json, math, os, sys
from PIL import Image, ImageDraw, ImageFont

FONT, OUT = sys.argv[1], sys.argv[2]
TITLE = 'ARMGBC'
SIZE, TITLE_GAP = 15, 1
TITLE_COLUMNS, BLOCK_ROWS = 16, 3
# The title's top line, on the reference's title's.
TITLE_TOP = 48
# The extra wait after the screen comes on: Delay's steps of 7, then the slide's 7 nops.
LATE_STEPS, LATE_ENTRY = 33, 0
# The title's columns come in yellow, red, magenta and green, then settle blue, the colours the
# reference's logo is seen to take.
WAVE_COLOURS = [(31, 31, 0), (31, 0, 0), (31, 0, 31), (0, 31, 0)]
TITLE_BLUE = (0, 0, 31)
WAVE = len(WAVE_COLOURS)


def render(text, size, gap):
    font = ImageFont.truetype(FONT, size)
    marks = []
    for ch in text:
        if ch == ' ':
            marks.append(None)
            continue
        m = Image.new('1', (size * 3, size * 3), 0)
        d = ImageDraw.Draw(m)
        d.fontmode = '1'
        d.text((size, size), ch, font=font, fill=1)
        marks.append(m)
    boxes = [m.getbbox() for m in marks if m]
    top, bottom = min(b[1] for b in boxes), max(b[3] for b in boxes)
    places, x = [], 0
    for m in marks:
        if m is None:
            x += size // 4
            continue
        b = m.getbbox()
        places.append((m.crop((b[0], top, b[2], bottom)), x))
        x += b[2] - b[0] + gap
    width = x - gap
    image = Image.new('1', (width, bottom - top), 0)
    for m, x in places:
        image.paste(m, (x, 0))
    return image, [(x, m.width) for m, x in places]


def main():
    title, _ = render(TITLE, SIZE, TITLE_GAP)
    width, height = TITLE_COLUMNS * 4, BLOCK_ROWS * 4
    assert title.width <= width and title.height <= height
    px, py = (width - title.width) // 2, (height - title.height) // 2
    def pixel(x, y):
        x, y = x - px, y - py
        return 0 <= x < title.width and 0 <= y < title.height and title.getpixel((x, y))
    art = bytearray()
    for by in range(BLOCK_ROWS):
        for bx in range(TITLE_COLUMNS):
            for pair in range(2):
                b = 0
                for r in range(2):
                    for c in range(4):
                        if pixel(bx * 4 + c, by * 4 + pair * 2 + r):
                            b |= 0x80 >> (r * 4 + c)
                art.append(b)
    tx, ty = (160 - width * 2) // 2, TITLE_TOP
    assert ty % 8 == 0
    colours = [r | g << 5 | b << 10 for r, g, b in WAVE_COLOURS + [TITLE_BLUE]]
    # Compatibility palettes.
    facts = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'cgbpalettes.json')))
    waits = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'cgbwaits.json')))
    def sets(combo):
        v = [int(c, 16) for c in combo.split()]
        return [tuple(v[0:4]), tuple(v[4:8]), tuple(v[8:12])]
    combos = [facts['default']]
    for _, _, c in facts['entries']:
        if c not in combos:
            combos.append(c)
    order = ['right a', 'up', 'up a', 'up b', 'left', 'left a', 'left b', 'down', 'down a', 'down b', 'right', 'right b']
    for name in order:
        if facts['buttons'][name] not in combos:
            combos.append(facts['buttons'][name])
    # Sprite colour 0 shows too, where a game's OBP gives a pixel shade 0, so sets are kept whole.
    unique = []
    def place(s):
        if s not in unique:
            unique.append(s)
        return unique.index(s)
    for c in combos:
        place(sets(c)[0])
    index = [[place(s) for s in sets(c)] for c in combos]
    # Sets of white, two colours and black first, stored as just the two.
    # Combinations pack in 15 bits, so a BG-only set goes first and a sprite-only set last: BG sets
    # 0-31, sprite sets 1-32 (stored less 1).
    standard = [i for i, u in enumerate(unique) if u[0] == 0x7FFF and u[3] == 0]
    bg_used = {i[0] for i in index}
    obj_used = {v for i in index for v in i[1:]}
    standard.sort(key=lambda i: i in obj_used)
    others = [i for i in range(len(unique)) if i not in standard]
    others.sort(key=lambda i: i in bg_used, reverse=True)
    order = standard + others
    assert order[0] not in obj_used and order[-1] not in bg_used
    index = [[order.index(v) for v in i] for i in index]
    unique = [unique[i] for i in order]
    # The waits before and after the screen comes on, each as Delay's steps of 7 and where to enter
    # the 7-nop slide, never at its first, so that the newer licensee code's 8 more cycles before are
    # a step and a nop. Steps before, steps after, then both entries.
    def wait(key):
        total, after = waits[key]
        before, after = total - after, LATE_STEPS * 7 + 7 - LATE_ENTRY + after
        assert 7 <= before and 7 <= after < 256 * 7 and before + 8 < 256 * 7
        return before // 7, after // 7, (7 - before % 7) << 4 | (7 - after % 7)
    plain = [(s, combos.index(c)) + wait('%d-0' % s) for s, letter, c in facts['entries'] if letter is None]
    lettered = [(s, letter, combos.index(c)) + wait('%d-%d' % (s, letter)) for s, letter, c in facts['entries'] if letter is not None]
    # Titles timed as their own whose colours are the default's.
    known = {'%d-%d' % (s, letter or 0) for s, letter, c in facts['entries']}
    for key in waits:
        if key not in known:
            s, letter = (int(x) for x in key.split('-'))
            if letter:
                lettered.append((s, letter, 0) + wait(key))
            else:
                plain.append((s, 0) + wait(key))
    with open(OUT, 'w') as f:
        f.write('; Written by cgbdata.py.\n')
        f.write('DEF TITLE_ROWS EQU %d\nDEF ART_TILES EQU %d\n' % (height * 2, len(art) // 2))
        f.write('DEF TITLE_X EQU %d\nDEF TITLE_Y EQU %d\n' % (tx, ty))
        f.write('DEF TITLE_COLUMNS EQU %d\nDEF WAVE EQU %d\n' % (TITLE_COLUMNS, WAVE))
        f.write('DEF LATE_STEPS EQU %d\nDEF LATE_ENTRY EQU %d\n' % (LATE_STEPS, LATE_ENTRY))
        f.write('DEF PLAIN EQU %d\nDEF LETTERED EQU %d\nDEF STANDARD_SETS EQU %d\n' % (len(plain), len(lettered), len(standard)))
    with open(OUT.replace('.inc', '-data.inc'), 'w') as f:
        f.write('; Written by cgbdata.py.\nArt:\n')
        for i in range(0, len(art), 16):
            f.write('\tdb %s\n' % ', '.join('$%02X' % b for b in art[i:i + 16]))
        f.write('; BG palettes 1 to %d, colour 3: the colours the columns come in through, then the title\'s blue.\nTitleColours:\n\tdw %s\n'
                % (len(colours), ', '.join('$%04X' % c for c in colours)))
        f.write('; Colour sets: white, two colours and black as just the two; then the others whole.\nStandardSets:\n')
        for s in unique[:len(standard)]:
            f.write('\tdw $%04X, $%04X\n' % s[1:3])
        f.write('OtherSets:\n')
        for s in unique[len(standard):]:
            f.write('\tdw %s\n' % ', '.join('$%04X' % c for c in s))
        f.write('; Per combination: the BG set in bits 0-4, the sprite sets less 1 in 5-9 and 10-14.\nCombos:\n')
        for i in index:
            assert i[0] < 32 and 1 <= i[1] <= 32 and 1 <= i[2] <= 32
            f.write('\tdw $%04X\n' % (i[0] | (i[1] - 1) << 5 | (i[2] - 1) << 10))
        f.write('; Title checksum, combination, waits; then those that also need the fourth letter.\nPlain:\n')
        for e in plain:
            f.write('\tdb $%02X, %d, %d, %d, $%02X\n' % e)
        f.write('Lettered:\n')
        for e in lettered:
            f.write('\tdb $%02X, $%02X, %d, %d, %d, $%02X\n' % e)
        f.write('; Per direction (right, left, up, down as the joypad bits): none, A, B, and A and B as A.\nButtonCombos:\n')
        for d in ('right', 'left', 'up', 'down'):
            f.write('\tdb %s\n' % ', '.join(str(combos.index(facts['buttons'][d + s])) for s in ('', ' a', ' b', ' a')))
    print('art %d bytes, title %dx%d doubled at %d,%d (%d columns)' % (len(art), title.width, title.height, tx, ty, TITLE_COLUMNS))

main()
