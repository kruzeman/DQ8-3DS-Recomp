import struct
data = open('DQ8.code', 'rb').read()
base = 0x00100000
def w(a):
    o = a - base
    return struct.unpack('<I', data[o:o+4])[0]
# heap_init sub_00106A54: E59F402C at 0x00106A58 => r4 = *(pc + 0x2C) => addr = pc+8+imm
a = 0x00106A58 + 8 + 0x2C
print('heap_init (0x00106A54) descriptor literal @0x%08X = 0x%08X' % (a, w(a)))
b = 0x001069D4 + 8 + 0x74
print('_heap_expand reg (0x001069BC) desc literal @0x%08X = 0x%08X' % (b, w(b)))
c = 0x00106B0C + 8 + 0xBC
print('_heap_expand lin (0x00106AF4) desc literal @0x%08X = 0x%08X' % (c, w(c)))
# dump the two descriptors' static contents (bss => likely zero, but check .data)
for name, d in [('reg', 0x005E94E0), ('lin', 0x005E94F0)]:
    words = [w(d + i*4) for i in range(4)]
    print('%s desc @0x%08X: %s' % (name, d, ' '.join('%08X' % x for x in words)))
