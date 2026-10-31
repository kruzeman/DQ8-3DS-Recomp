#!/usr/bin/env python3
"""Convert the final dq8-screen.ppm of evidence runs to PNG (diagnosis aid)."""
import sys
from PIL import Image

for run in sys.argv[1:]:
    src = "target/audio-workers-evidence/%s/frames/dq8-screen.ppm" % run
    dst = "target/audio-workers-evidence/%s/final-top.png" % run
    try:
        im = Image.open(src)
        im.save(dst)
        print(run, "ok", im.size)
    except Exception as exc:
        print(run, "ERR", exc)
