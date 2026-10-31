#!/usr/bin/env python3
"""Aggregate FRAME/TIME phase numbers from a DQ8_TRACE_TIME run log."""
import re, sys, statistics
path = sys.argv[1]
txt = open(path, errors='replace').read()

win = re.findall(r'TIME window=([\d.]+)s frames=(\d+) rt=(\d+)x(\d+)', txt)
draw = re.findall(r'DRAWPHASE n=(\d+) prep_ms=([\d.]+) vert_ms=([\d.]+) flush_ms=([\d.]+) loadv_ms=([\d.]+) verts=(\d+)', txt)
glph = re.findall(r'GLPHASE draws=(\d+) setup_ms=([\d.]+) copy_ms=([\d.]+) draw_ms=([\d.]+) mean_us=([\d.]+) skip=\S+ blend_gl=(\d+) blend_sw=(\d+)', txt)
stage = re.findall(r'STAGEPHASE calls=(\d+) hits=(\d+) miss_dirty=(\d+) miss_new=(\d+) miss_evict=(\d+) decode_ms=([\d.]+) cmp_ms=([\d.]+) texels=(\d+)', txt)
fs = re.findall(r'cum: frames=(\d+) identical=(\d+)', txt)

fw = sum(float(a) for a, b, c, d in win)
ff = sum(int(b) for a, b, c, d in win)
print('TIME windows: %d, wall=%.1f s, frames=%d, fps=%.2f, frame=%.0f ms, rt=%sx%s'
      % (len(win), fw, ff, ff / fw if fw else 0, 1000.0 * fw / ff if ff else 0,
         win[0][2] if win else '?', win[0][3] if win else '?'))

def avg(rows, idx, n=None):
    vals = [float(r[idx]) for r in rows]
    return sum(vals) / len(vals) if vals else 0.0

print('\nper 2.0-s report block (mean over %d blocks):' % len(draw))
n = len(draw)
prep, vert, flush, loadv = (avg(draw, i) for i in (1, 2, 3, 4))
setup, copy, draw_ms = avg(glph, 1), avg(glph, 2), avg(glph, 3)
meanwin = (fw / len(win)) * 1000.0 if win else 2000.0
print('  window wall            %7.1f ms  (mean over %d TIME lines)' % (meanwin, len(win)))
print('  prep                    %7.1f ms  %5.1f%%' % (prep, 100 * prep / meanwin))
print('  vertex load+clip        %7.1f ms  %5.1f%%   (of which load_vertex %.1f ms)'
      % (vert, 100 * vert / meanwin, loadv))
print('    flush (GL phase)      %7.1f ms  %5.1f%%' % (flush, 100 * flush / meanwin))
print('      GL setup/state      %7.1f ms  %5.1f%%' % (setup, 100 * setup / meanwin))
print('      GL VBO pack+upload  %7.1f ms  %5.1f%%' % (copy, 100 * copy / meanwin))
print('      glDrawArrays submit %7.1f ms  %5.1f%%' % (draw_ms, 100 * draw_ms / meanwin))
tot = prep + vert + flush
print('  draw path TOTAL         %7.1f ms  %5.1f%%' % (tot, 100 * tot / meanwin))
print('  rest (guest/present/sync outside draws) %5.1f%%' % (100 * (1 - tot / meanwin)))
print('\n  draws/block=%.0f  verts/block=%.0f  us/draw=%.0f  us/vert=%.2f'
      % (avg(glph, 0), avg(draw, 5),
         (avg(glph, 0) and (setup + copy + draw_ms) * 1000 / avg(glph, 0)),
         loadv * 1e3 / avg(draw, 5) if avg(draw, 5) else 0))
# STAGEPHASE is cumulative: take diffs of the last two for the per-window decode cost
if len(stage) >= 2:
    a, b = stage[-2], stage[-1]
    print('  texture decode per block: %.1f ms (cum diff), cmp %.1f ms, texels %d'
          % (float(b[5]) - float(a[5]), float(b[6]) - float(a[6]), int(b[7]) - int(a[7])))
if fs:
    print('\nFRAMESTAT: frames=%s identical=%s ident_pct=%.1f%%'
          % (fs[-1][0], fs[-1][1], 100.0 * int(fs[-1][1]) / max(int(fs[-1][0]), 1)))
