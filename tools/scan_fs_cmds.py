#!/usr/bin/env python3
"""Enumerate FS-service command headers embedded as literals in DQ8.code.

SDK IPC stubs materialise the command header as a literal word, so scanning for
words shaped like an FS header ((word>>16) in 0x0800..0x08FF and low6 == 2,
the header model low16 = param_words<<6 | 2) lists every FS command the game
can issue. Checked against native_fs.c coverage.
usage: tools/scan_fs_cmds.py
"""
import struct

CODE = '/home/hermes/DQ8/DQ8.code'
BASE = 0x00100000
TEXT_END = 0x0053BCB4

code = open(CODE, 'rb').read()

NAMES = {
    0x0801: 'Initialize', 0x0802: 'OpenFile', 0x0803: 'OpenFileDirectly',
    0x0804: 'DeleteFile', 0x0805: 'RenameFile', 0x0806: 'DeleteDirectory',
    0x0807: 'DeleteDirectoryRecursively', 0x0808: 'CreateFile',
    0x0809: 'CreateDirectory', 0x080A: 'RenameDirectory',
    0x080B: 'OpenDirectory', 0x080C: 'OpenArchive', 0x080D: 'ControlArchive',
    0x080E: 'CloseArchive', 0x080F: 'FormatThisUserSaveData',
    0x0810: 'CreateLegacySystemSaveData', 0x0811: 'DeleteSystemSaveData',
    0x0812: 'GetFreeBytes', 0x0813: 'GetCardType',
    0x0814: 'GetSdmcArchiveResource', 0x0815: 'GetNandArchiveResource',
    0x0817: 'IsSdmcDetected', 0x0818: 'IsSdmcWritable',
    0x0821: 'CardSlotIsInserted', 0x084C: 'FormatSaveData',
}

found = {}
for off in range(0, TEXT_END - BASE, 4):
    w = struct.unpack_from('<I', code, off)[0]
    hi = w >> 16
    if not (0x0801 <= hi <= 0x08FF):
        continue
    if (w & 0x3F) != 2:
        continue
    # reject obvious non-headers: param words must be sane (< 64)
    nparams = (w & 0xFFF) >> 6
    if nparams > 32:
        continue
    found.setdefault(hi, []).append(BASE + off)

# file-session commands (handle = file) use a different id space too
print('FS command headers embedded in .text:')
for hi in sorted(found):
    addrs = found[hi]
    print('  0x%04X %-28s sites=%d  e.g. %s' %
          (hi, NAMES.get(hi, '???'), len(addrs),
           ' '.join('0x%08X' % a for a in addrs[:3])))
