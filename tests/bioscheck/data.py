# data.py: writes data.h, the compressed inputs the test ROM gives the BIOS's decompressors, made
# from seeded random data with repeats in it. The encoders follow GBATEK's format descriptions.
import random

random.seed(20261020)


def sample(size, alphabet=256, runs=True):
    out = []
    while len(out) < size:
        if runs and random.random() < 0.3:
            out += [random.randrange(alphabet)] * random.randint(3, 40)
        elif runs and len(out) > 8 and random.random() < 0.3:
            start = random.randrange(len(out))
            out += out[start:start + random.randint(3, 18)]
        else:
            out.append(random.randrange(alphabet))
    return bytes(out[:size])


def lz77(data):
    out = bytearray([0x10]) + len(data).to_bytes(3, 'little')
    i = 0
    while i < len(data):
        flags_at = len(out)
        out.append(0)
        flags = 0
        for bit in range(8):
            if i >= len(data):
                break
            best, best_len = 0, 0
            # Distances of 1 aren't used: VRAM decompression can't copy from the halfword being made.
            for disp in range(2, min(i, 4096) + 1):
                length = 0
                while length < 18 and i + length < len(data) and data[i + length] == data[i - disp + length]:
                    length += 1
                if length > best_len:
                    best, best_len = disp, length
            if best_len >= 3:
                flags |= 0x80 >> bit
                value = ((best_len - 3) << 12) | (best - 1)
                out += bytes([value >> 8, value & 0xFF])
                i += best_len
            else:
                out.append(data[i])
                i += 1
        out[flags_at] = flags
    return pad(out)


def rl(data):
    out = bytearray([0x30]) + len(data).to_bytes(3, 'little')
    i = 0
    while i < len(data):
        run = 1
        while run < 130 and i + run < len(data) and data[i + run] == data[i]:
            run += 1
        if run >= 3:
            out += bytes([0x80 | (run - 3), data[i]])
            i += run
            continue
        start = i
        while i < len(data) and i - start < 128:
            if i + 2 < len(data) and data[i] == data[i + 1] == data[i + 2]:
                break
            i += 1
        out.append(i - start - 1)
        out += data[start:i]
    return pad(out)


def diff(data, width):
    out = bytearray([0x80 | width]) + len(data).to_bytes(3, 'little')
    previous = 0
    for i in range(0, len(data), width):
        value = int.from_bytes(data[i:i + width], 'little')
        out += ((value - previous) & ((1 << (8 * width)) - 1)).to_bytes(width, 'little')
        previous = value
    return pad(out)


def huffman(data, bits):
    symbols = []
    for byte in data:
        if bits == 8:
            symbols.append(byte)
        else:
            symbols += [byte & 0xF, byte >> 4]
    counts = {}
    for s in symbols:
        counts[s] = counts.get(s, 0) + 1
    # Build the tree.
    nodes = [(c, n, s) for n, (s, c) in enumerate(sorted(counts.items()))]
    if len(nodes) == 1:
        nodes.append((0, len(nodes), (nodes[0][2] + 1) % (1 << bits)))
    serial = len(nodes)
    while len(nodes) > 1:
        nodes.sort(key=lambda n: (n[0], n[1]))
        a, b = nodes.pop(0), nodes.pop(0)
        nodes.append((a[0] + b[0], serial, (a[2], b[2])))
        serial += 1
    root = nodes[0][2]
    codes = {}

    def walk(node, code):
        if isinstance(node, tuple):
            walk(node[0], code + '0')
            walk(node[1], code + '1')
        else:
            codes[node] = code
    walk(root, '')
    # Lay the tree out breadth first, by address from the header's start: the root is at 5, and
    # each node's two children are a pair at an even address after it.
    table = {5: None}
    queue = [(root, 5)]
    end = 6
    while queue:
        node, at = queue.pop(0)
        if end % 2:
            end += 1
        pair = end
        end += 2
        offset = (pair - (at & ~1) - 2) // 2
        assert offset < 64, 'tree too wide for 6-bit offsets'
        value = offset
        for child, slot, leaf_bit in ((node[0], pair, 0x80), (node[1], pair + 1, 0x40)):
            if isinstance(child, tuple):
                queue.append((child, slot))
            else:
                table[slot] = child
                value |= leaf_bit
        table[at] = value
    # The bitstream starts on a word: the table's length is 2 * size byte + 1, from address 5.
    while (end - 5) % 4 != 3:
        end += 1
    body = bytes(table.get(a, 0) for a in range(5, end))
    out = bytearray([0x20 | bits]) + len(data).to_bytes(3, 'little')
    out.append((len(body) - 1) // 2)
    out += body
    assert len(out) % 4 == 0
    stream = ''.join(codes[s] for s in symbols)
    stream += '0' * (-len(stream) % 32)
    for i in range(0, len(stream), 32):
        out += int(stream[i:i + 32], 2).to_bytes(4, 'little')
    return pad(out)


def pad(out):
    while len(out) % 4:
        out.append(0)
    return bytes(out)


def array(name, data):
    rows = ['\t' + ', '.join('0x%02x' % b for b in data[i:i + 16]) + ',' for i in range(0, len(data), 16)]
    return 'static const uint8_t %s[%d] __attribute__((aligned(4))) = {\n%s\n};\n' % (name, len(data), '\n'.join(rows))


blobs = []
for n, size in enumerate([16, 100, 777, 4096, 20000, 33, 501, 1023]):
    blobs.append(('lz%d' % n, lz77(sample(size))))
    blobs.append(('rl%d' % n, rl(sample(size))))
    blobs.append(('d8_%d' % n, diff(sample(size & ~1), 1)))
    blobs.append(('d16_%d' % n, diff(sample(size & ~3), 2)))
    blobs.append(('h4_%d' % n, huffman(sample(size & ~3, 16), 4)))
    blobs.append(('h8_%d' % n, huffman(sample(size & ~3, 24), 8)))
blobs.append(('packed', sample(512, runs=False)))
# The timing set: 24 of each, small, from all literals to mostly repeats, for fitting each
# decompressor's time to what it does. timing.py decodes the same blobs from timing.json.
TIMING = 24
mixes = [(False, 256), (True, 256), (True, 16), (True, 4)]
timing = {kind: [] for kind in ('lz', 'rl', 'd8', 'd16', 'h4', 'h8')}
for n in range(TIMING):
    runs, alphabet = mixes[n % 4]
    size = 16 + n * 16
    timing['lz'].append(lz77(sample(size, alphabet, runs)))
    timing['rl'].append(rl(sample(size, alphabet, runs)))
    timing['d8'].append(diff(sample(size & ~1, alphabet, runs), 1))
    timing['d16'].append(diff(sample(size & ~3, alphabet, runs), 2))
    timing['h4'].append(huffman(sample(size & ~3, min(alphabet, 16), runs), 4))
    timing['h8'].append(huffman(sample(size & ~3, min(alphabet, 24), runs), 8))
for kind, datas in timing.items():
    for n, data in enumerate(datas):
        blobs.append(('t%s_%d' % (kind, n), data))
with open('data.h', 'w') as f:
    f.write('// Made by data.py.\n#include <stdint.h>\n\n')
    for name, data in blobs:
        f.write(array(name, data))
    for kind, prefix in (('lz', 'lz'), ('rl', 'rl'), ('d8', 'd8_'), ('d16', 'd16_'), ('h4', 'h4_'), ('h8', 'h8_')):
        f.write('static const uint8_t* const %sAll[] = { %s };\n' % (kind, ', '.join('%s%d' % (prefix, n) for n in range(8))))
    for kind in timing:
        f.write('static const uint8_t* const %sTiming[] = { %s };\n' % (kind, ', '.join('t%s_%d' % (kind, n) for n in range(TIMING))))
import json
json.dump({kind: [d.hex() for d in datas] for kind, datas in timing.items()}, open('timing.json', 'w'))
