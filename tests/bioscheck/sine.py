# sine.py reference.ram: prints the BIOS's sine table, for bios/start.s's sine:, as the reference
# BIOS's outputs show it. The test ROM's probe calls ObjAffineSet at each of the 256 angles with both
# scales at 1.0 in 1.14 fixed point, so the matrix's third entry is the angle's sine.
import struct, sys

PROBE = 0x10000 + 100 * (80 + 64) + 100 * (32 + 128)
ram = open(sys.argv[1], 'rb').read()
sines = [struct.unpack_from('<4h', ram, PROBE + angle * 8)[2] for angle in range(256)]
for i in range(0, 256, 16):
    print('\t.hword ' + ', '.join('%d' % v for v in sines[i:i + 16]))
