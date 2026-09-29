# Old-core benchmark timings (refound/core baseline)

Recorded 2026-09-13 against `refound/core` (old runtime, `make clean && make
test` green: 1250 unit cases / 10098 checks / 0 failed; 291 chk passed / 9
skipped / 85 placeholders / 0 vacuous / 0 failed). These numbers are the
lookup-gate's "don't regress below this" floor for the new `src/rt/` core.

CPU: `AMD RYZEN AI MAX+ 395 w/ Radeon 8060S` (from `/proc/cpuinfo`, `model
name`), 32 logical CPUs. Single-threaded workloads below; core count doesn't
affect these numbers.

**Timing tool note:** `/usr/bin/time` (the `time` package) is not installed
in this sandbox and there is no interactive password available to `apt-get
install` it (`sudo` requires a TTY here). Wall-clock elapsed seconds were
measured instead with `date +%s.%N` immediately before and after the
`urbi` invocation, subtracted with `awk` — the same quantity
`/usr/bin/time -f %e` reports, at millisecond rather than 10ms precision.

**CLI note:** `urbi -f <file>` compiles and runs the file but never prints
the chunk's return value (no writer is wired into the CLI outside the
REPL/`-e` path — see `tools/urbi.c`'s `run_file` vs. `run_expression`).
Both probes below are therefore invoked as `urbi -e "$(cat <file>)"`, which
prints the trailing expression (the checksum) after running everything
before it.

## (a) Mandelbrot inner loop

`examples/stm32f4/mandelbrot/mandelbrot.u` cannot run through `build/host/urbi
-f` as-is: it reads/writes board-specific globals only `main.c` registers
(`lcd_fill_rect`, `gyro_x`/`gyro_y`/`gyro_z`, the `gyro_tick?`/
`button_press?` events) and its outer loop is `while (true) { ...
waituntil (...) }`, which never terminates on host. `tests/probes/
mandelbrot_host.u` keeps `mandel()` and `color_for()` byte-for-byte
identical to the original and replaces the LCD/gyro/button machinery with
a single full-width pass over `Realm.W` x `Realm.H` pixels that sums
`color_for(mandel(x, y))` into a checksum instead of drawing.

`Realm.H` is 125, not the original 240. A single un-yielded pass at the
original 320x240 (76,800 `mandel()`+`color_for()` calls) hits
`URBI_ERR_LOADER_BUDGET` -- `src/chunk/uchunk_strand.c`'s loader-strand
forward-progress cap (`URBI_LOADER_OUTER_CAP` (10000) *
`URBI_LOADER_INNER_BUDGET` (1000) = 10,000,000 instructions with no yield;
there is no `at`/`sleep`/`waituntil`/fork in a straight-line pixel pass to
reset it). Empirically: 320x130 (41,600 calls) succeeds, 320x140 (44,800
calls) hits the cap; 125 rows leaves headroom below that edge. **Worth
flagging to the team:** the real board script's `render_level(1)` finest
pass does the identical 76,800-call straight-line sweep with no
intervening yield and would hit the same cap on real hardware -- this
looks like a latent issue in the shipped demo, not something introduced
by this probe.

Command:

```sh
urbi -e "$(cat tests/probes/mandelbrot_host.u)"
```

Output (checksum, identical every run -- pure function of fixed inputs):
`13890050`.

5 runs (elapsed seconds):

| run | seconds |
|-----|---------|
| 1   | 0.220   |
| 2   | 0.209   |
| 3   | 0.205   |
| 4   | 0.199   |
| 5   | 0.213   |

**Median: 0.209 s**

## (b) Slot-lookup micro-benchmark

`tests/probes/lookup_bench.u`: a 3-deep prototype chain (`P1` -> `P2` ->
`o`, via `.clone()`) carrying 6 total slots (`a`, `b` on `P1`; `c`, `d` on
`P2`; `e`, `f` on `o`), then 200,000 reads of `o.f` followed by 100,000
`o.f = o.f + 1` read-modify-write statements. Final value: `100000`
(printed by the trailing `o.f` expression).

Command:

```sh
urbi -e "$(cat tests/probes/lookup_bench.u)"
```

Output: `100000` on every run.

5 runs (elapsed seconds):

| run | seconds |
|-----|---------|
| 1   | 0.024   |
| 2   | 0.021   |
| 3   | 0.021   |
| 4   | 0.022   |
| 5   | 0.021   |

**Median: 0.021 s**

## (c) ESP32-S3-EYE blob tracker

`examples/esp32/eye_demo/main/eye_demo.u` does not run on host without
stubs, so it is skipped per the task's own fallback. It fails on the very
first top-level statement that isn't a pure definition:

```sh
$ ./build/host/urbi -f examples/esp32/eye_demo/main/eye_demo.u
urbi: TypeError: slot access: slot 'log' not found
```

`log(...)` (line 372, `log("eye_demo (class-based ...) loaded")`) is a
bare-name host function registered by `examples/esp32/eye_demo/main/
eye_demo_main.c` via `urbi_register(&vm, r, "log", c_log)` -- it is not a
stdlib builtin (`Float.log` in `src/stdlib/atoms.c` is unrelated: a
method, not a bare name). The rest of the script depends on ~20 more
ESP32-only host functions (`c_cam_fps`, `c_get_pixel_r/g/b`,
`c_scan_begin`/`c_scan_end`, `set_target_chroma`, `draw_crosshair`,
`c_watchers_in_use`, camera/heap/ring telemetry, ...) and four
host-injected events (`blob_seen?`, `button_pressed?`, `stats_tick?`,
`scan_tick?`). Writing a host-stub harness for all of that is out of
scope here; no timing recorded for this probe.

## New core after the performance work

Recorded 2026-09-29 against commit `e434e293` (`src/rt/`, after the slot
cache (`src/rt/uslotcache.h`), threaded dispatch, and the yield fast path).
Same machine as above. `make clean && make && make test-bench`, then
`make test-bench` twice more, each run alone, nothing else building.

| run | lookup_bench | mandelbrot_host |
|-----|--------------|------------------|
| 1   | 0.60x        | 0.83x            |
| 2   | 0.60x        | 0.84x            |
| 3   | 0.59x        | 0.83x            |

All six ratios at or under the 1.20 spec ratio; the probe's ratchet
ceilings are now pinned to that same 1.20.

**Many-receivers case:** 64 clones of one prototype, read through a
one-argument function call, 2,000 rounds (128,000 reads total). Written
to a scratch file outside the repo and run five times each on
`build/host/urbi` from this tree and on a scratch build of baseline
commit `8253b960`, via `urbi -e "$(cat <file>)"`, each run timed with
`date +%s.%N` before and after and subtracted with `awk`:

```
var C = Object.clone();
var C.k = 1;
var objs = [];
var i = 0;
while (i < 64) { objs << C.clone(); i = i + 1 };
var rd = function(x) { x.k };
var sum = 0;
var r = 0;
while (r < 2000) {
    var j = 0;
    while (j < 64) { sum = sum + rd(objs[j]); j = j + 1 };
    r = r + 1
};
sum
```

- new tree median: 0.0125 s
- baseline median: 0.0209 s (about 40% faster, not slower)
