# frame-budget profiling kit (t_432a85ab)

Measurement-only tooling used to build `reports/FRAMEBUDGET_t_432a85ab.md`
on `target/dq8-texcache3` (savestate scene, headless, DQ8_GL=1).

Why not perf/gdb-attach:
* `perf record` fails — `kernel.perf_event_paranoid=4`, no sudo.
* `gdb -p <pid>` fails — `yama/ptrace_scope=1` (only ancestors may trace).

What works:
* `pysample.py <label> <dur_s> <warmup_s> <hz>` — spawns the target as its OWN
  child (yama allows ancestor tracing), PTRACE_SEIZEs every thread, samples
  RIP at `hz` plus per-thread utime/stime/wchan. Writes `samples.tsv`,
  `proc.tsv`, `run.log` under `<ws>/evidence/<label>/`.
* `gdb_stack.sh <label> <n_samples> <warmup_s>` — runs the target UNDER gdb
  (gdb is the parent), arms `alarm(1)` at main and re-arms at every SIGALRM
  stop, `thread apply all bt 14`.  DQ8_PROGRESS_SECONDS must stay unset.
* `profile_run.sh <label> <dur_s> [ENV=…]` — plain run + tsample.sh counters.
* `baseline_run.sh <label> <dur_s>` — same with PPM capture enabled.
* analyzers: `extract_stats.py <run.log>`, `analyze_threads.py <threads.log>`,
  `analyze_samples.py <label>`, `parse_stacks.py <gdb.log>`, `wait_mod.py <gdb.log>`.

Evidence for the report lives in the task workspace
`/home/hermes/.hermes/kanban/workspaces/t_432a85ab/evidence/`:
prof1 (clean baseline), prof2 (15 900 RIP samples), stacks1 (60 backtraces),
skip_nodst / skip_nogeo (DQ8_GL_SKIP ablations).
