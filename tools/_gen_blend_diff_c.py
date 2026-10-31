#!/usr/bin/env python3
"""Regenerate the blend-diff C source into /tmp for syntax iteration."""
import importlib.util
spec = importlib.util.spec_from_file_location("t", "/home/hermes/DQ8/tools/test_pica_blend_diff.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
open('/tmp/blend_diff_check.c', 'w').write(m.build_source())
print("written /tmp/blend_diff_check.c")
