#!/usr/bin/env python3
"""PERF-05 regression test: SDL input must reach HID on the SAME host pump.

The runtime's host pump (`host_display_pump_events`) is the only place where
host SDL events are turned into the guest button mask and where that mask is
published into the guest HID shared-memory ring (`native_hid_pump`).  Before
PERF-05 the publication ran first and SDL_PollEvent second, so a key press that
was already in the SDL queue when a due pump started was published only by the
NEXT pump - one extra host-to-HID delivery interval per fresh event.

The test drives the emitted runtime directly: the generated C is built from a
3-instruction guest stub (LDR r1,[r0]; STR r1,[r2]; BX lr) so the real
translated-guest load path can read the HID ring, and the harness appends its
own main() that
  * initialises the real HID service objects (HID:GetIPCHandles + MapMemoryBlock
    of handle 0x30010 at a guest address, exactly like the guest does),
  * brings up SDL with the dummy video driver,
  * pushes real SDL_KEYDOWN/SDL_KEYUP events and calls the real
    host_display_pump_events() once per due pump,
  * inspects the emitted HID shared memory through mem32 (the same reads the
    guest performs) plus the TLS handle descriptor and the two HID events.

Scenarios: fresh keydown on one due pump, guest LDR of the published mask,
keydown/keyup queue drained inside one window, two distinct press edges, the
16.7 ms throttle deadline, ring wrap at 8 entries (+ timestamp rollover), and
the no-display/headless publication path.  A direct GDB button-mask write is
never used: every button transition below comes from SDL_PushEvent().

usage:
  tools/test_hid_publish_order.py                 # expects the fixed runtime
  tools/test_hid_publish_order.py --expect-defect # pre-PERF-05 runtime: the
                                                  # same-pump checks must fail

env: DQ8_TOOL overrides the recompiler binary (default target/release/dq8tool).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# LDR r1,[r0] ; STR r1,[r2] ; BX lr  -> the translated guest read path.
STUB_ARM = "001090e5" "001082e5" "1eff2fe1"

HARNESS = r'''
/* ---- PERF-05 host->HID publication-order harness (appended) ---- */
static int h_fail;
static void hc(const char *name, unsigned long long got, unsigned long long want) {
    int ok = got == want;
    printf("%s %s got=%llu want=%llu\n", ok ? "PASS" : "FAIL", name, got, want);
    if (!ok) h_fail++;
}
static int push_key(SDL_Keycode k, int down) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = e.key.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.repeat = 0;
    e.key.keysym.sym = k;
    return SDL_PushEvent(&e);
}
/* Queued events, without removing them. */
static int queue_depth(void) {
    SDL_Event e[8];
    return SDL_PeepEvents(e, 8, SDL_PEEKEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT);
}
/* Same guest-visible setup the game performs: HID session -> 5 events, then
   MapMemoryBlock(0x30010) at a guest address, which also resets ring index and
   throttle deadline. */
static void hid_reset(uint32_t at) {
    put32(active_tls_base + 0x80u, 0x000A0000u);
    svc_send_sync(11u);
    svc_map_memory_block(0x30010u, at);
    native_hid_buttons = 0;
    native_hid_previous = 0;
    memset(event_signaled, 0, sizeof event_signaled);
}
static void due_pump(void) {
    native_hid_next_update = 0;   /* the pump is due now */
    host_display_pump_events();   /* exactly one host pump */
}
int main(void) {
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    unsetenv("DQ8_INPUT_FILE");
    unsetenv("DQ8_TRACE_INPUT");
    ram = calloc(1, GUEST_RAM_SIZE);
    if (!ram) { puts("FAIL harness/alloc-ram got=0 want=1"); return 2; }
    hid_reset(0x10000000u);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("FAIL harness/sdl-init %s\n", SDL_GetError());
        return 2;
    }
    host_display_ready = 1;
    host_display_quit = 0;
    hc("harness/hid-base-mapped", native_hid_base, 0x10000000u);
    hc("harness/hid-events-allocated", native_hid_events[0] != 0u && native_hid_events[1] != 0u, 1);

    /* ---- C1: one due pump must publish a fresh SDL_KEYDOWN ---- */
    hc("c1/pushed", push_key(SDLK_z, 1), 1);
    hc("c1/queued", queue_depth(), 1);
    unsigned pumps = 0;
    while (pumps < 4u && mem32(native_hid_base + 28u) != 1u) { due_pump(); ++pumps; }
    printf("PUMPS_TO_VISIBLE=%u\n", pumps);
    hc("c1/pumps-until-mask-visible", pumps, 1);
    hc("c1/mask", mem32(native_hid_base + 28u), 1);
    hc("c1/index", mem32(native_hid_base + 16u), 0);
    hc("c1/entry-buttons", mem32(native_hid_base + 40u), 1);
    hc("c1/entry-pressed", mem32(native_hid_base + 44u), 1);
    hc("c1/entry-released", mem32(native_hid_base + 48u), 0);
    hc("c1/entry-reserved", mem32(native_hid_base + 52u), 0);
    hc("c1/timestamp-set", (mem32(native_hid_base) | mem32(native_hid_base + 4u)) != 0u, 1);
    hc("c1/hid-event0-signaled", event_signaled[native_hid_events[0]], 1);
    hc("c1/hid-event1-signaled", event_signaled[native_hid_events[1]], 1);
    hc("c1/hid-event-auto-reset", event_manual_reset[native_hid_events[0]], 0);
    hc("c1/tls-handle-descriptor", mem32(active_tls_base + 0x90u), native_hid_events[0]);

    /* Guest read path: translated LDR of the HID buttons word, STR to scratch. */
    {
        uint32_t r[16] = {0};
        r[0] = native_hid_base + 28u;
        r[2] = 0x00500000u;
        r[14] = 0xFFFFFFFFu;
        translated_from(r, 0x00100000u, NULL);
        hc("c1/guest-ldr-sees-fresh-mask", mem32(0x00500000u), 1);
    }
#ifndef HID_ORDER_C1_ONLY
    /* ---- C2: the 16.7 ms throttle deadline still gates publication ---- */
    {
        uint64_t now = host_monotonic_ns();
        hc("c2/deadline-is-one-frame",
           native_hid_next_update > now && native_hid_next_update <= now + 20000000ull, 1);
        uint32_t index_before = native_hid_index, idx = native_hid_index & 7u;
        hc("c2/throttled-push", push_key(SDLK_z, 0), 1);
        host_display_pump_events();          /* not due yet */
        hc("c2/host-mask-collected", native_hid_buttons, 0);
        hc("c2/throttled-index-unchanged", native_hid_index, index_before);
        hc("c2/throttled-mask-unchanged", mem32(native_hid_base + 28u), 1);
        due_pump();
        hc("c2/due-index-advanced", native_hid_index, index_before + 1u);
        hc("c2/due-mask", mem32(native_hid_base + 28u), 0);
        hc("c2/due-entry-buttons", mem32(native_hid_base + 40u + idx * 16u), 0);
        hc("c2/due-entry-pressed", mem32(native_hid_base + 44u + idx * 16u), 0);
        hc("c2/due-entry-released", mem32(native_hid_base + 48u + idx * 16u), 1);
    }

    /* ---- C3: keydown+keyup inside one window publish no edge ---- */
    {
        uint32_t index_before = native_hid_index, idx = native_hid_index & 7u;
        hc("c3/pushed-down", push_key(SDLK_z, 1), 1);
        hc("c3/pushed-up", push_key(SDLK_z, 0), 1);
        hc("c3/queued", queue_depth(), 2);
        due_pump();
        hc("c3/one-publish-per-pump", native_hid_index, index_before + 1u);
        hc("c3/index", mem32(native_hid_base + 16u), idx);
        hc("c3/mask", mem32(native_hid_base + 28u), 0);
        hc("c3/entry-buttons", mem32(native_hid_base + 40u + idx * 16u), 0);
        hc("c3/entry-pressed", mem32(native_hid_base + 44u + idx * 16u), 0);
        hc("c3/entry-released", mem32(native_hid_base + 48u + idx * 16u), 0);
    }

    /* ---- C4: two distinct A presses across four due pumps ---- */
    {
        int ok = 1;
        for (int i = 0; i < 4; ++i) {
            uint32_t idx = native_hid_index & 7u;
            int down = (i % 2) == 0;
            ok &= push_key(SDLK_z, down) == 1;
            due_pump();
            ok &= mem32(native_hid_base + 16u) == idx;
            ok &= mem32(native_hid_base + 28u) == (uint32_t)(down ? 1 : 0);
            ok &= mem32(native_hid_base + 44u + idx * 16u) == (uint32_t)(down ? 1 : 0);
            ok &= mem32(native_hid_base + 48u + idx * 16u) == (uint32_t)(down ? 0 : 1);
        }
        hc("c4/two-presses-two-edges", ok, 1);
    }

    /* ---- C5: ring wrap at 8 entries + timestamp rollover ---- */
    {
        int ok = 1;
        hid_reset(0x10000000u);
        for (unsigned i = 0; i < 9; ++i) {
            uint32_t idx = i & 7u, on = (i & 1u) ? 0u : 2048u, off = 2048u - on;
            ok &= push_key(SDLK_a, (i & 1u) == 0u) == 1;
            due_pump();
            ok &= mem32(native_hid_base + 16u) == idx;
            ok &= mem32(native_hid_base + 28u) == on;
            ok &= mem32(native_hid_base + 40u + idx * 16u) == on;
            ok &= mem32(native_hid_base + 44u + idx * 16u) == on;
            ok &= mem32(native_hid_base + 48u + idx * 16u) == off;
        }
        hc("c5/wrap-nine-publishes", ok, 1);
        hc("c5/final-index", mem32(native_hid_base + 16u), 0);
        hc("c5/final-mask", mem32(native_hid_base + 28u), 2048);
        hc("c5/timestamp-rollover", (mem32(native_hid_base + 8u) | mem32(native_hid_base + 12u)) != 0u, 1);
        /* The ninth publish lands in ring slot 0 again: the guest reads the
           wrapped entry, not a stale one. */
        hc("c5/guest-read-after-wrap", mem32(0x10000000u + 40u), 2048);
    }

    /* ---- C6: no display -> publication still happens, SDL is not polled ---- */
    {
        host_display_ready = 0;
        hid_reset(0x10000000u);
        native_hid_buttons = 1;
        hc("c6/pushed", push_key(SDLK_z, 1), 1);
        due_pump();
        hc("c6/headless-publish-mask", mem32(native_hid_base + 28u), 1);
        hc("c6/headless-index", mem32(native_hid_base + 16u), 0);
        hc("c6/headless-entry-pressed", mem32(native_hid_base + 44u), 1);
        hc("c6/sdl-queue-untouched", queue_depth(), 1);
        {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {}
        }
        hc("c6/queue-drained", queue_depth(), 0);
        host_display_ready = 1;
        hc("c6/pushed-up", push_key(SDLK_z, 0), 1);
        {
            uint32_t idx = native_hid_index & 7u;
            due_pump();
            hc("c6/display-back-mask", mem32(native_hid_base + 28u), 0);
            hc("c6/display-back-index", mem32(native_hid_base + 16u), idx);
            hc("c6/display-back-released", mem32(native_hid_base + 48u + idx * 16u), 1);
        }
    }
#endif /* HID_ORDER_C1_ONLY */

    SDL_Quit();
    free(ram);
    printf("RESULT %s failures=%d\n", h_fail ? "FAIL" : "PASS", h_fail);
    return h_fail ? 1 : 0;
}
'''


def build_and_run(workdir: Path, expect_defect: bool) -> tuple[int, str]:
    tool = Path(os.environ.get("DQ8_TOOL", ROOT / "target/release/dq8tool"))
    if not tool.is_file():
        sys.exit("missing recompiler: %s (cargo build --offline --release)" % tool)
    code = workdir / "stub.code"
    code.write_bytes(bytes.fromhex(STUB_ARM))
    source = workdir / "stub.c"
    subprocess.run([str(tool), "recompile-arm", str(code), "0x00100000", str(source)],
                   check=True, capture_output=True)
    text = source.read_text().replace("int main(int argc, char **argv)", "int guest_main(int argc, char **argv)")
    assert "static void host_display_pump_events(void) {" in text, "SDL pump missing: build without DQ8_ENABLE_SDL?"
    source.write_text(text + HARNESS)
    exe = workdir / "hid_order_test"
    cflags = subprocess.run(["sdl2-config", "--cflags"], check=True, capture_output=True, text=True).stdout.split()
    libs = subprocess.run(["sdl2-config", "--libs"], check=True, capture_output=True, text=True).stdout.split()
    extra = ["-DHID_ORDER_C1_ONLY"] if expect_defect else []
    subprocess.run(["clang", "-std=c11", "-O0", "-w", "-DDQ8_ENABLE_SDL", *extra, *cflags,
                    str(source), "-o", str(exe), *libs, "-lm"], check=True)
    env = dict(os.environ)
    env.pop("DQ8_INPUT_FILE", None)
    env.pop("DQ8_TRACE_INPUT", None)
    env["SDL_VIDEODRIVER"] = "dummy"
    env["SDL_AUDIODRIVER"] = "dummy"
    proc = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120, env=env)
    return proc.returncode, proc.stdout + proc.stderr


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--expect-defect", action="store_true",
                    help="run a pre-PERF-05 runtime: the same-pump checks must fail")
    args = ap.parse_args()
    workdir = Path(tempfile.mkdtemp(prefix="dq8-hid-order-"))
    try:
        rc, out = build_and_run(workdir, args.expect_defect)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    print(out.rstrip())
    pumps = [line.split("=", 1)[1] for line in out.splitlines() if line.startswith("PUMPS_TO_VISIBLE=")]
    fails = [line.split()[1] for line in out.splitlines() if line.startswith("FAIL ")]
    if not pumps:
        print("test_hid_publish_order: FAIL (harness produced no PUMPS_TO_VISIBLE)")
        return 1
    if args.expect_defect:
        # The old order needs a second pump for the fresh press; every other
        # HID semantic (ring layout, edges, throttle, wrap, headless path) must
        # behave identically, so only c1/* may fail.
        bad = [name for name in fails if not name.startswith("c1/")]
        ok = pumps[0] == "2" and rc == 1 and not bad
        print("test_hid_publish_order: %s (pre-fix order, pumps_to_visible=%s, "
              "non-c1 failures=%s)" % ("PASS" if ok else "FAIL", pumps[0], bad))
        return 0 if ok else 1
    ok = rc == 0 and pumps[0] == "1" and not fails
    print("test_hid_publish_order: %s (pumps_to_visible=%s, failures=%s)"
          % ("PASS" if ok else "FAIL", pumps[0], fails))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
