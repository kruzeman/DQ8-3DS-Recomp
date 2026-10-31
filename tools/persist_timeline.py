"""Correlate key events in a DQ8 runner log with wall-clock time.

The runner prints a 'guest snapshot' line every 5 s (DQ8_PROGRESS_SECONDS), so
snapshot ordinals act as time markers for every other log line.

usage: python3 tools/persist_timeline.py LOG [LOG ...]
"""
import sys

EVENTS = {
    'first honeySave open FAIL': lambda l: 'honeySave.bin' in l and 'result=C8804470' in l,
    'first honeySave open OK': lambda l: 'honeySave.bin' in l and 'result=00000000' in l,
    'FormatSaveData': lambda l: 'FS FormatSaveData' in l,
    'ControlArchive': lambda l: 'FS ControlArchive' in l,
    'full-size save read (0x33E50)': lambda l: 'size=00033E50' in l,
    'first HID A press': lambda l: 'HID buttons=00000001' in l,
    'first HID A release': lambda l: 'released=00000001' in l,
    'y2r first command': lambda l: 'handle=00000010 ' in l,
    'y2r Y-plane 0x0010 (96000 B)': lambda l: 'handle=00000010 ' in l and 'command=00100102' in l,
    'y2r RGB dst 0x0018 (288000 B)': lambda l: 'handle=00000010 ' in l and 'command=00180102' in l,
    'y2r get transfer end 0x0026': lambda l: 'handle=00000010 ' in l and 'command=00260000' in l,
}


def main(paths):
    for path in paths:
        lines = open(path, errors='replace').read().splitlines()
        snaps = [i for i, l in enumerate(lines, 1) if l.startswith('guest snapshot')]

        def t_of(line_no):
            return 5 * sum(1 for s in snaps if s <= line_no)

        print(f'== {path}  ({len(lines)} lines, {len(snaps)} snapshots)')
        for name, pred in EVENTS.items():
            hits = [i for i, l in enumerate(lines, 1) if pred(l)]
            if hits:
                print(f'   {name:32s} line {hits[0]:6d}  ~t+{t_of(hits[0]):4d}s  count={len(hits)}')
            else:
                print(f'   {name:32s} never')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
