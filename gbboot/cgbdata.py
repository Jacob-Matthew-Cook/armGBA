# cgbdata.py out.inc: the data for armGBA's Game Boy Color boot ROM (cgb.asm).
# The art: "ARMGBC" at full size in a picture of TITLE_COLUMNS tile columns by 3 tile rows, made of
# two layers. Most letters are drawn at half size and doubled by the boot ROM, stored as the
# cartridge's logo is: a block of 4x4 of their pixels, a tile on screen, is its 4 rows a nibble each,
# 2 bytes. The tiles go down each column, the columns in order from the fourth, so that the first
# three, the A's, come last and aren't stored. The A, and the rows of the M that doubling can't draw,
# are patched in at full size: runs of rows, a count and the row's 3 bytes, over 3 tile columns.
# The compatibility palettes for original Game Boy games come from cgbpalettes.json, measured from
# what the reference boot ROM shows, never from the reference itself. cgbwaits.json, measured the
# same way, holds how long it takes to hand over with each of those titles, and with a few whose
# fourth letter isn't one of those but still changes it: the cycles of the hand-over's extra wait,
# and how many of them more than a plain cartridge's come after the screen comes on. The Advance's are
# the same.
import json, math, os, sys

OUT = sys.argv[1]
TITLE = 'ARMGBC'
# The title's letters at full size, where they're patched in.
FULL = {
    'A': """
......#####......
......#####......
......#####......
.....#######.....
.....#######.....
.....#######.....
....####.####....
....####.####....
....####.####....
...####...####...
...####...####...
...####...####...
..####.....####..
..####.....####..
..#############..
.###############.
.###############.
.###############.
####.........####
####.........####
####.........####
####.........####
""",
    'M': """
####............####
####............####
#####..........#####
#####..........#####
######........######
######........######
#######......#######
#######......#######
########....########
########....########
####.####..####.####
####.####..####.####
####..########..####
####..########..####
####...######...####
####...######...####
####....####....####
####....####....####
####............####
####............####
####............####
####............####
""",
}
# The others at half size, which the boot ROM doubles.
LETTERS = {
    'R': """
#######.
########
##....##
##....##
########
#######.
##...##.
##....##
##....##
##....##
##....##""",
    'M': """
##......##
###....###
###....###
####..####
####..####
##.####.##
##.####.##
##..##..##
##..##..##
##......##
##......##""",
    'G': """
.#######.
#########
##.....##
##.......
##...####
##...####
##.....##
##.....##
##.....##
#########
.#######.""",
    'B': """
#######..
########.
##....##.
##....##.
########.
#########
##.....##
##.....##
##.....##
#########
########.""",
    'C': """
.#######.
#########
##.....##
##.....##
##.......
##.......
##.......
##.....##
##.....##
#########
.#######.""",
}
# Where each letter starts in the picture, in pixels.
PLACES = dict(A=6, R=24, M=42, G=64, B=84, C=104)
# The patches: a letter's rows, at full size, over the tile columns from one.
PATCHES = [('A', 0, range(22)), ('M', 5, range(10, 12))]
TITLE_COLUMNS, BLOCK_ROWS = 16, 3
# Tile columns at the left the doubled layer leaves out: the A's.
SKIP = 3
# The title's top line, on the reference's title's.
TITLE_TOP = 48
# The extra wait after the screen comes on: Delay's steps of 7, then the slide's 7 nops.
LATE_STEPS, LATE_ENTRY = 33, 0
# The title's columns come in yellow, red, magenta and green, then settle blue, the colours the
# reference's logo is seen to take.
WAVE_COLOURS = [(31, 31, 0), (31, 0, 0), (31, 0, 31), (0, 31, 0)]
TITLE_BLUE = (0, 0, 31)
WAVE = len(WAVE_COLOURS)


def main():
    width, height = TITLE_COLUMNS * 8, BLOCK_ROWS * 8
    full = [[False] * width for _ in range(height)]
    half = [[False] * (width // 2) for _ in range(height // 2)]
    for ch, x0 in PLACES.items():
        rows = FULL.get(ch, '').strip('\n').split('\n') if ch in FULL else None
        if rows:
            for y, row in enumerate(rows):
                for x, c in enumerate(row):
                    full[y][x0 + x] = c == '#'
        if ch in LETTERS:
            assert x0 % 2 == 0
            for y, row in enumerate(LETTERS[ch].strip('\n').split('\n')):
                for x, c in enumerate(row):
                    half[y][x0 // 2 + x] = c == '#'
    # The doubled layer, the A's columns left out.
    order = list(range(SKIP, TITLE_COLUMNS)) + list(range(SKIP))
    art = bytearray()
    for bx in order[:TITLE_COLUMNS - SKIP]:
        for by in range(BLOCK_ROWS):
            for pair in range(2):
                b = 0
                for r in range(2):
                    for c in range(4):
                        if half[by * 4 + pair * 2 + r][bx * 4 + c]:
                            b |= 0x80 >> (r * 4 + c)
                art.append(b)
    # The patches: the address of a column's top row in the tiles from 1, then runs of rows.
    def column_address(column):
        return 0x8010 + order.index(column) * 48
    patches = bytearray()
    for ch, column, rows in PATCHES:
        address = column_address(column) + rows[0] * 2
        patches += bytes([address >> 8, address & 0xFF])
        lines = [bytes(sum(full[y][column * 8 + i * 8 + k] << (7 - k) for k in range(8)) for i in range(3)) for y in rows]
        i = 0
        while i < len(lines):
            n = 1
            while i + n < len(lines) and lines[i + n] == lines[i]:
                n += 1
            patches += bytes([n]) + lines[i]
            i += n
        patches.append(0)
    patches.append(0)
    tw, th = width, 22
    tx, ty = (160 - width) // 2, TITLE_TOP
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
    # The combination and the waits before and after the screen comes on, in 3 bytes: Delay's steps
    # of 7 before; where to enter the 7-nop slide before in bits 0-2 and the combination's bits 0-4 in
    # 3-7; (the cycles after - 14) / 2 in bits 0-6 and the combination's bit 5 in 7, which cgb.asm
    # turns into steps and an entry. The slide is never entered at its first, so that the newer
    # licensee code's 8 more cycles before are a step and a nop.
    def wait(combo, key):
        total, after = waits[key]
        before, after = total - after, LATE_STEPS * 7 + 7 - LATE_ENTRY + after
        assert 7 <= before and before + 8 < 256 * 7 and 14 <= after <= 14 + 2 * 127 and after % 2 == 0 and combo < 64
        return before // 7, 7 - before % 7 | (combo & 31) << 3, (after - 14) // 2 | (combo >> 5) << 7
    plain = [(s,) + wait(combos.index(c), '%d-0' % s) for s, letter, c in facts['entries'] if letter is None]
    lettered = [(s, letter) + wait(combos.index(c), '%d-%d' % (s, letter)) for s, letter, c in facts['entries'] if letter is not None]
    # Titles timed as their own whose colours are the default's.
    known = {'%d-%d' % (s, letter or 0) for s, letter, c in facts['entries']}
    for key in waits:
        if key not in known:
            s, letter = (int(x) for x in key.split('-'))
            if letter:
                lettered.append((s, letter) + wait(0, key))
            else:
                plain.append((s,) + wait(0, key))
    with open(OUT, 'w') as f:
        f.write('; Written by cgbdata.py.\n')
        f.write('DEF TITLE_ROWS EQU %d\nDEF ART_TILES EQU %d\nDEF SKIPPED EQU %d\n' % (height, len(art) // 2, SKIP))
        f.write('DEF TITLE_X EQU %d\nDEF TITLE_Y EQU %d\n' % (tx, ty))
        f.write('DEF TITLE_COLUMNS EQU %d\nDEF WAVE EQU %d\n' % (TITLE_COLUMNS, WAVE))
        f.write('DEF LATE_STEPS EQU %d\nDEF LATE_ENTRY EQU %d\n' % (LATE_STEPS, LATE_ENTRY))
        f.write('DEF PLAIN EQU %d\nDEF LETTERED EQU %d\nDEF STANDARD_SETS EQU %d\n' % (len(plain), len(lettered), len(standard)))
    with open(OUT.replace('.inc', '-data.inc'), 'w') as f:
        f.write('; Written by cgbdata.py.\nArt:\n')
        for i in range(0, len(art), 16):
            f.write('\tdb %s\n' % ', '.join('$%02X' % b for b in art[i:i + 16]))
        f.write('Patches:\n')
        for i in range(0, len(patches), 16):
            f.write('\tdb %s\n' % ', '.join('$%02X' % b for b in patches[i:i + 16]))
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
        f.write('; Title checksum and fourth letter, then the combination and waits; then by the checksum alone.\nLettered:\n')
        for e in lettered:
            f.write('\tdb $%02X, $%02X, $%02X, $%02X, $%02X\n' % e)
        plain.sort()
        bits = [0] * 32
        for e in plain:
            bits[e[0] >> 3] |= 0x80 >> (e[0] & 7)
        f.write('; A bit per checksum, the first the highest: whether it has its own entry, which follow in order.\nPlainSums:\n')
        for i in range(0, 32, 16):
            f.write('\tdb %s\n' % ', '.join('$%02X' % b for b in bits[i:i + 16]))
        f.write('PlainPacks:\n')
        for e in plain:
            f.write('\tdb $%02X, $%02X, $%02X\n' % e[1:])
        f.write('; Per direction (right, left, up, down as the joypad bits): none, A, B, and A and B as A.\nButtonCombos:\n')
        for d in ('right', 'left', 'up', 'down'):
            f.write('\tdb %s\n' % ', '.join(str(combos.index(facts['buttons'][d + s])) for s in ('', ' a', ' b', ' a')))
    print('art %d bytes, patches %d bytes, title at %d,%d (%d columns)' % (len(art), len(patches), tx, ty, TITLE_COLUMNS))

main()
