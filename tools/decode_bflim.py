import sys, struct
from pathlib import Path

def decode(p):
    data = Path(p).read_bytes()
    print(f'--- {p} ({len(data)} bytes) ---')
    # find FLIM
    i = data.find(b'FLIM')
    if i < 0:
        print('  no FLIM tag')
        return
    hdr = i
    # BFLIM v7: sig(4)=FLIM, version(2), endian(2), file_size(4)
    version = struct.unpack_from('<H', data, hdr+4)[0]
    endian = struct.unpack_from('<H', data, hdr+6)[0]
    size = struct.unpack_from('<I', data, hdr+8)[0]
    print(f'  sig at {i} version={version} endian=0x{endian:04x} filesize={size}')
    if version == 0x0010:  # v7.2.0
        w = struct.unpack_from('<H', data, hdr+28)[0]
        h = struct.unpack_from('<H', data, hdr+30)[0]
        fmt = data[hdr+34] if hdr+34 < len(data) else -1
        print(f'  w={w} h={h} fmt={fmt}')
    elif version == 0x0000:  # older
        # layout: sig4 + size4 + version? try generic
        pass
    # dump first 64 bytes hex
    print('  hdr bytes:', data[hdr:hdr+48].hex())

for p in sys.argv[1:]:
    decode(p)
