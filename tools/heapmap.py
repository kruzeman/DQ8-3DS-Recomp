import re
# Scan the runtime's ControlMemory log lines for the heap reservation pattern.
# The guest reserves regular heap @0x08000000 (op=0x03) and linear heap via
# op=0x10003 (MEMOP_COMMIT|MEMOP_LINEAR) bumping control_memory_next from 0x14000000.
# We extract every (op, req, size) and reconstruct the non-overlap check.
lines = open('/tmp/heapctl.log').read().splitlines()
ctl = []
for ln in lines:
    m = re.search(r'ControlMemory pc=(\w+) lr=(\w+) op=(\w+) req=(\w+) addr1=(\w+) size=(\w+) perm=(\w+)', ln)
    if m:
        ctl.append(dict(pc=m.group(1), lr=m.group(2), op=int(m.group(3),16),
                        req=int(m.group(4),16), a1=int(m.group(5),16),
                        size=int(m.group(6),16), perm=int(m.group(7),16)))
print('total ControlMemory calls:', len(ctl))
from collections import Counter
print('by op:', Counter(hex(c['op']) for c in ctl))
# reconstruct regions
GRS = 0x40000000
linear_next = 0x14000000
regions = []
for c in ctl:
    op = c['op']; req = c['req']; size = c['size']
    if op == 0x10003 and req == 0:
        result = (linear_next + 0xFFF) & ~0xFFF
        amount = (size + 0xFFF) & ~0xFFF
        regions.append(('linear', result, result+amount))
        linear_next = result + amount
    elif (op & 0xFF) == 3:
        # MEMOP_COMMIT at requested address (regular heap base 0x08000000)
        regions.append(('heap', req, req + size))
print('\nregions reserved:')
for kind, lo, hi in regions:
    print('  %-7s [0x%08X, 0x%08X)  size=0x%X (%d MB)' % (kind, lo, hi, hi-lo, (hi-lo)>>20))
# overlap check among same-kind
def check(kind):
    rs = [(lo,hi) for k,lo,hi in regions if k==kind]
    rs.sort()
    ov = []
    for i in range(len(rs)-1):
        if rs[i][1] > rs[i+1][0]:
            ov.append((rs[i], rs[i+1]))
    print('\n%s overlaps: %d' % (kind, len(ov)))
    for a,b in ov: print('   OVERLAP', hex(a[0]),hex(a[1]),'vs',hex(b[0]),hex(b[1]))
check('heap'); check('linear')
