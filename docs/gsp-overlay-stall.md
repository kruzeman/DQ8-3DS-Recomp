# GSP startup stall: diagnostic logo overlay

The startup worker stopped at guest PC `0x0016396C`. The instruction there is
`svc #0x32` (SendSyncRequest), not an ARM spin loop. The host was still inside
GSP TriggerCmdReqQueue after its first display transfer.

The unconditional diagnostic overlay called `pica_sample_bflim` for all 8192
pixels before sending PPF completion. Before the layout resources are loaded,
the sampler has no cached texture. Each failed lookup scans the entire
0x08000000..0x18000000 RAM range again. This makes a cosmetic overlay block
queue completion and all subsequent guest progress.

The overlay is now opt-in with `DQ8_DEBUG_LOGO_OVERLAY=1`, and it only runs
when `bflim_hdr_cache` is already populated. It must never discover resources
inside the per-pixel loop. Actual guest draws and the BFLIM sampler are
otherwise unchanged. The overlay is diagnostic and does not count as a
frame produced by translated guest code.

## Verification on 2026-10-18

Built the Rust tool and regenerated/compiled the native C runner separately
under `/tmp`, leaving `target/dq8-startup` untouched. The control and patched
generated C differed only in the overlay guard.

| Bounded run | Transfers | Draw triggers |
| --- | ---: | ---: |
| Control, 12 seconds | 1 | 0 |
| Patched, 12 seconds | 214 | 308 |
| Patched with debug overlay enabled, 6 seconds | 88 | 56 |

The control repeatedly reported PC `0x0016396C`; the patched runs proceeded
through command submission. Runs used `DQ8_DISPLAY=0`,
`DQ8_RUN_GUEST_THREADS=1`, and `DQ8_TRACE_GPU=1`. Their exit status 124 was
intentional termination by `timeout`, not a guest crash.

`cargo test --locked --offline --bin dq8tool --target-dir /tmp/dq8-overlay-build`
passed all 31 tests. `git diff --check` passed. These results establish that
the startup stall is removed; they do not establish correct UI geometry or
a real guest-rendered title screen.

Local diagnostic logs: `/tmp/dq8-overlay-before.err`,
`/tmp/dq8-overlay-after.err`, and `/tmp/dq8-overlay-optin.err`.
