# compare.py reference.ram ours.ram: compares the test ROM's records from two runs, the first on the
# reference BIOS, per call: the registers returned, the memory written, and the cycles taken.
import struct, sys

NAMES = {0x01: 'RegisterRamReset', 0x02: 'Halt', 0x04: 'IntrWait', 0x05: 'VBlankIntrWait', 0x06: 'Div', 0x07: 'DivArm', 0x08: 'Sqrt', 0x09: 'ArcTan', 0x0A: 'ArcTan2', 0x0B: 'CpuSet',
         0x0C: 'CpuFastSet', 0x0D: 'GetBiosChecksum', 0x0E: 'BgAffineSet', 0x0F: 'ObjAffineSet',
         0x10: 'BitUnPack', 0x11: 'LZ77UnCompWram', 0x12: 'LZ77UnCompVram', 0x13: 'HuffUnComp',
         0x14: 'RLUnCompWram', 0x15: 'RLUnCompVram', 0x16: 'Diff8bitUnFilterWram',
         0x17: 'Diff8bitUnFilterVram', 0x18: 'Diff16bitUnFilter', 0x19: 'SoundBias', 0xF0: 'interrupt path'}


def records(path):
    ram = open(path, 'rb').read()
    count, done = struct.unpack_from('<II', ram, 0x1FFF8)
    if done != 0x454E4F44:
        sys.exit('%s: the test ROM didn\'t finish (%d records)' % (path, count))
    return [struct.unpack_from('<11I', ram, 0x20000 + 44 * i) for i in range(count)]


# Calls whose timing differences are shown with examples (every call's are counted).
TIMED = {0x02, 0x04, 0x05, 0x19}

ref, ours = records(sys.argv[1]), records(sys.argv[2])
if len(ref) != len(ours):
    sys.exit('record counts differ: %d and %d' % (len(ref), len(ours)))
stats = {}
for a, b in zip(ref, ours):
    s = stats.setdefault(a[0], {'n': 0, 'regs': [0, 0, 0, 0], 'mem': 0, 'examples': [], 'cyc': [0, 0]})
    s['n'] += 1
    bad = False
    for r in range(4):
        if a[5 + r] != b[5 + r]:
            s['regs'][r] += 1
            bad = True
    if a[10] != b[10]:
        s['mem'] += 1
        bad = True
    s['cyc'][0] += a[9]
    s['cyc'][1] += b[9]
    s.setdefault('cycdiff', 0)
    if a[9] != b[9]:
        s['cycdiff'] += 1
        if a[0] in TIMED and not bad and len(s['examples']) < 3:
            s['examples'].append((a, b))
    if bad and len(s['examples']) < 3:
        s['examples'].append((a, b))
print('%-22s %5s %20s %6s %9s' % ('call', 'tests', 'r0/r1/r2/r3 differ', 'memory', 'cycles'))
total = 0
for i, s in sorted(stats.items()):
    wrong = max(s['regs'] + [s['mem']])
    total += any(s['regs']) or s['mem']
    cycles = '%.2fx' % (s['cyc'][1] / s['cyc'][0]) if s['cyc'][0] else '-'
    timed = ' (%d timings differ)' % s['cycdiff'] if s['cyc'][0] else ''
    print('%-22s %5d %20s %6d %9s%s' % (NAMES.get(i, hex(i)), s['n'], '/'.join(map(str, s['regs'])), s['mem'], cycles, timed))
    for a, b in s['examples']:
        print('    in %s' % ' '.join('%08X' % v for v in a[1:5]))
        print('    reference %s  sum %08X' % (' '.join('%08X' % v for v in a[5:9]), a[10]))
        print('    ours      %s  sum %08X' % (' '.join('%08X' % v for v in b[5:9]), b[10]))
        print('    cycles reference %d ours %d' % (a[9], b[9]))
print('%d of %d calls differ in results' % (total, len(stats)))


def irqLog(path):
    ram = open(path, 'rb').read()
    size = struct.unpack_from('<I', ram, 0x1FFE8)[0]
    return [struct.unpack_from('<3I', ram, 0x1C000 + i) for i in range(0, size, 12)]


# The cycle counter's start depends on how long the calls before took, so each interrupt is timed
# from the one before. The last word holds the scanline, and the System mode stack pointer's low
# half above it.
def relative(log):
    return [(f, c - log[i - 1][1] if i else 0, line & 0xFFFF, line >> 16) for i, (f, c, line) in enumerate(log)]


a, b = relative(irqLog(sys.argv[1])), relative(irqLog(sys.argv[2]))
same = sum(x == y for x, y in zip(a, b))
print('interrupt log: %d and %d interrupts, %d the same (flags, cycles since the last, scanline, stack)' % (len(a), len(b), same))
for x, y in list(zip(a, b))[:40]:
    if x != y:
        print('    reference flags %04X cycles %9d line %3d stack %04X   ours flags %04X cycles %9d line %3d stack %04X' % (x + y))
