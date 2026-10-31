# Guest region overrides

`DQ8_OVERRIDES=/path/to/registry.json tools/build_split.sh ...` applies a named
registry during the normal generated-C split/build pipeline. The default
`registry.json` is intentionally empty. Without entries, split guest code has no
override-dispatch calls.

Registry schema (version 1):

```json
{
  "version": 1,
  "overrides": [
    {
      "name": "example_kernel",
      "start": "0x00100000",
      "end": "0x00100004",
      "body": "if (r[1] & 1u) return DQ8_OVERRIDE_CALL_ORIGINAL; r[0] = r[1]; *pc_io = guest_pc + 4u; return DQ8_OVERRIDE_HANDLED;"
    }
  ]
}
```

Ranges are half-open `[start, end)`, must not overlap, and the entry PC must be a
translated guest case. For each dispatched PC in the range, the handler receives
`guest_pc`, mutable registers, PC, NZCV/GE, `running`, `steps`, and split batch
budget pointers. `DQ8_OVERRIDE_HANDLED` means the handler performed this guest
step and advanced/terminated the state; `DQ8_OVERRIDE_CALL_ORIGINAL` falls
through to the exact generated instruction case at that PC. The latter is the
safe per-instruction call-original fallback. Handlers that implement more than
one guest instruction must update retired `steps`/batch budget exactly and
preserve all scheduling/observer boundaries; do not coalesce across an
unverified boundary.

Set `DQ8_DISABLE_OVERRIDES=1` when launching a generated binary to bypass all
handlers and run the reference dispatch cases. Handlers may call existing
checked guest memory helpers; direct RAM access must not bypass tracing, aliases,
faults, or LDREX/STREX invalidation.
