#!/usr/bin/env python3
# Find a free guest-RAM gap for relocated thread stacks.
# App heap used by the guest: 0x08000000.. (observed stacks 0x0828E0B8..0x08831318)
# LINEAR: 0x14000000..0x1C000000. GUEST_RAM_SIZE = 0x40000000.
# TLS: 0x1FF82000..0x1FF82000+64*0x200 = 0x1FF8A000.
GUEST_RAM_SIZE = 0x40000000
# Candidate: just below LINEAR start, above observed app-heap top (~0x08900000).
# 0x10000000 (256MB) is a clean boundary. Check it does not collide with
# LINEAR (0x14000000) or app heap or TLS.
STACK_BASE = 0x10000000
SLOT_SIZE  = 0x10000   # 64KB per thread stack slot (3DS default stack ~4-64KB)
SLOTS      = 64
STACK_TOP  = STACK_BASE + SLOT_SIZE * SLOTS
print("relocated stack region: [0x%08X, 0x%08X)  (%d slots x 64KB)" % (
    STACK_BASE, STACK_TOP, SLOTS))
assert STACK_TOP <= 0x14000000, "collides with LINEAR"
assert STACK_BASE >= 0x08900000, "collides with observed app-heap top"
assert STACK_TOP <= GUEST_RAM_SIZE
# check it's clear of TLS 0x1FF82000
assert not (STACK_BASE < 0x1FF8A000 and STACK_TOP > 0x1FF82000)
print("OK: clear of app-heap, LINEAR, TLS, within GUEST_RAM_SIZE")
# For each observed colliding stack top, what would its relocated slot be?
colliding = [0x0868C710, 0x0868C6D8, 0x086C3DB8, 0x086C3D88]
for i, top in enumerate(colliding):
    new_top = STACK_BASE + SLOT_SIZE * (i + 1)   # slot i top = base + (i+1)*SLOT
    print("  slot %2d: 0x%08X -> relocated top 0x%08X (extent 0x%08X..0x%08X)" % (
        i, top, new_top, new_top - SLOT_SIZE, new_top))
