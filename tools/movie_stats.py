#!/usr/bin/env python3
import csv
import sys

for n in sys.argv[1:]:
    rows = list(csv.DictReader(open(f"tools/joint/{n}/metrics.csv")))
    win = [r for r in rows if 135 <= int(r["t"]) < 205]
    if not win:
        print(n, "no rows")
        continue
    y2r = sum(int(r["y2r_d"]) for r in win)
    mv = sum(int(r["movie_d"]) for r in win)
    print(n, "y2r/s=", round(y2r / len(win), 2),
          "movie_entries/s=", round(mv / len(win), 2))
