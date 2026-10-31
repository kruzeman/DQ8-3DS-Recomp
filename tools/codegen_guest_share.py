#!/usr/bin/env python3
"""t_3de68379: guest-interpretation share from a codegen_budget.sh run.

Reads evidence/t_3de68379/<label>/{samples.tsv,proc.tsv} and prints, for the
MAIN host thread (which carries all guest execution, see
reports/FRAMEBUDGET_t_432a85ab.md) and for all threads:

  guest-strict  the bucket the t_432a85ab frame-budget report called
                "guest code interpretation": dq8_part_*, guest_*, dq8_mem*,
                recomp_*, svc_*  - the generated dispatch + its helpers.
  guest-dispatch+  the above PLUS the dispatch prologue the strict bucket
                leaves out (translated_from, dq8_pc_needs_boundary,
                branch_indirect, dq8_override_*, savestate_service,
                schedule_guest_thread, host_display_*).

usage: tools/codegen_guest_share.py <evidence-dir> [<binary-basename>]
"""
import collections
import os
import sys

base = sys.argv[1]
binbase = sys.argv[2] if len(sys.argv) > 2 else None

STRICT = ('dq8_part_', 'guest_', 'dq8_mem', 'recomp_', 'svc_')
PROLOGUE = ('translated_from', 'dq8_pc_needs_boundary', 'branch_indirect',
            'dq8_override_', 'savestate_service', 'schedule_guest_thread',
            'host_display_pump_events', 'host_display_present',
            'guest_periodic_dump', 'bench_report')


def classify(sym, mod):
    if binbase and mod != binbase:
        return None
    if sym.startswith(STRICT):
        return 'guest-strict'
    if sym.startswith(PROLOGUE):
        return 'guest-prologue'
    return None


samples = []
with open(os.path.join(base, 'samples.tsv')) as f:
    next(f)
    for line in f:
        p = line.rstrip('\n').split('\t')
        samples.append((int(p[1]), p[4], p[5]))

proc = []
with open(os.path.join(base, 'proc.tsv')) as f:
    next(f)
    for line in f:
        p = line.rstrip('\n').split('\t')
        if len(p) >= 7:
            proc.append((float(p[0]), int(p[1]), p[2]))

# main thread = the dq8 binary thread that accumulated the most wall in-window
cpu = collections.defaultdict(list)
for r in proc:
    cpu[r[1]].append(r[0])
main_tid, main_span = None, -1.0
for tid, ts in cpu.items():
    span = max(ts) - min(ts)
    name = next((r[2] for r in proc if r[1] == tid), '')
    if binbase and name != binbase:
        continue
    if span > main_span:
        main_tid, main_span = tid, span

def report(title, rows):
    cnt = collections.Counter()
    other = collections.Counter()
    for tid, mod, sym in rows:
        k = classify(sym, mod)
        if k:
            cnt[k] += 1
        else:
            other[sym] += 1
    n = len(rows)
    guest = cnt['guest-strict'] + cnt['guest-prologue']
    print('%s: %d samples' % (title, n))
    print('  guest-strict       %5d  %6.2f%%' % (cnt['guest-strict'],
                                                  100.0 * cnt['guest-strict'] / max(n, 1)))
    print('  guest-prologue     %5d  %6.2f%%' % (cnt['guest-prologue'],
                                                  100.0 * cnt['guest-prologue'] / max(n, 1)))
    print('  GUEST TOTAL        %5d  %6.2f%%' % (guest, 100.0 * guest / max(n, 1)))
    print('  top non-guest leaves:')
    for s, v in other.most_common(12):
        print('    %-46s %5d  %5.1f%%' % (s, v, 100.0 * v / max(n, 1)))
    print()

print('=== %s ===' % base)
report('MAIN thread (tid=%s)' % main_tid,
       [s for s in samples if s[0] == main_tid])
report('ALL threads', samples)

print('=== top guest leaves on MAIN (where the interpretation time goes) ===')
leaf = collections.Counter(sym for tid, mod, sym in samples
                           if tid == main_tid and classify(sym, mod))
for s, v in leaf.most_common(20):
    print('  %-46s %5d' % (s, v))
