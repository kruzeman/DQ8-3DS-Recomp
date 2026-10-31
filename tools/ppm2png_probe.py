#!/usr/bin/env python3
"""Convert the probe PPMs to PNG for visual inspection (t_bc6b4eb7)."""
import sys
from PIL import Image

pairs = [
    ("target/blackworld-evidence/geom-before-f120/frames/f0120.ppm",
     "target/blackworld-evidence/geom-before-f120/f0120.png"),
    ("target/blackworld-evidence/geom-after-f120/frames/f0120.ppm",
     "target/blackworld-evidence/geom-after-f120/f0120.png"),
    ("target/blackworld-evidence/geom-before-f240/frames/f0240.ppm",
     "target/blackworld-evidence/geom-before-f240/f0240.png"),
]
for src, dst in pairs:
    try:
        Image.open(src).save(dst)
        print(dst)
    except Exception as e:
        print(f"FAIL {src}: {e}", file=sys.stderr)
