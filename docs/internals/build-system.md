# Build system

## Overview

The urbi-embedded build is a single top-level `Makefile` that produces
`build/<target>/liburbi.a` from `src/` plus optional auxiliaries
(`build/<target>/urbi` REPL binary, fuzzer harnesses, stress drivers,
test runners). Cross-compilation is selected with `make cross-<preset>`,
which invokes the same Makefile with a per-target `TARGET=<dir>` and the
preset's toolchain. Every target gets its own `build/$(TARGET)/` tree, so
concurrent builds never race.

## Cross presets

A cross preset is a file under `presets/` (a tracked directory, because
`make clean` wipes `build/`) naming the compiler, archiver, `nm`, `size`
and CPU flags for one part. `presets/preset.mk` turns it into the
ordinary build: it sets `CC`, `AR` and `CFLAGS` to
`-std=c99 -Wall -Wextra -Wpedantic -Os <cpu flags> -ffreestanding`. A
preset whose compiler is not on PATH is a hard error naming the
compiler, never a skip. The host-side `make test` needs no cross
toolchain; [cross-toolchain-setup.md](../cross-toolchain-setup.md)
gives the install.

| Preset | CPU flags | Toolchain |
|---|---|---|
| `arm-cortex-m0plus` | `-mcpu=cortex-m0plus -mthumb -mfloat-abi=soft` | `arm-none-eabi-gcc` |
| `arm-cortex-m4f` | `-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard` | `arm-none-eabi-gcc` |
| `arm-cortex-m7` | `-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard` | `arm-none-eabi-gcc` |
| `riscv32` | `-march=rv32imc -mabi=ilp32` | `riscv-none-elf-gcc` |

Each preset builds in three shapes. Every shape builds `core`, the
archive only; host tools are never cross-built.

| Target | Archive | What it is |
|---|---|---|
| `make cross-<preset>` | `build/<preset>/liburbi.a` | the full library under `-ffreestanding` |
| `make cross-<preset>-bytecode-only` | `build/<preset>-bytecode-only/liburbi.a` | `URBI_BYTECODE_ONLY=1`: no compiler, no `src/host`, no `src/repl`; the archive gate runs on it |
| `make cross-<preset>-hosted` | `build/<preset>-hosted/liburbi.a` | `URBI_HOSTED=1` drops `-ffreestanding`, for parts that link a libc |

The cross build compiles the tracked `src/stdlib/urbi_stdlib_bytecode.gen.c`
like any other source. The bake tool that produces it is host-only and
never runs in a cross build.

### Freestanding contract

- `src/rt`, `src/chunk`, `src/util` include no `<stdio.h>` or `<stdlib.h>`; their libc use is `memcpy`, `memmove`, `memset`, `memcmp`, `strlen`, `strcmp`, which the embedder provides (newlib, or stubs as the STM32 example does).
- `src/stdlib` may call hosted libc (`snprintf`, `strtod`, `strtoll`, `<math.h>`) only under `#if __STDC_HOSTED__`, with a fallback that raises a `TypeError` naming the method. `atoms.c` already does this; `debug_namespace.c` gets the same guard. A freestanding build therefore has no `Float.asString`; a part that wants it links a libc and builds hosted.
- `src/host` (the value formatter) and `src/repl` (the eval service: `malloc`, `realloc`, `memchr`) are hosted-only and are excluded from `-bytecode-only` builds. A hosted cross build (the Pico, with newlib) includes them.
- A cross preset builds with `-ffreestanding` by default; `URBI_HOSTED=1` on the recursive make drops the flag for parts with a libc.
- The value formatter is wired in only on a hosted build, so on a freestanding build `Object.asString` and `echo` of a non-String raise a `TypeError` naming the method, even in the full `cross-<preset>` archive that contains `src/host`.
- `urbi_compile`, `urbi_run` and `urbi_watch` take source, so under `URBI_BYTECODE_ONLY` they are neither declared in `<urbi/urbi.h>` nor defined in the archive.

### Gates

- `make test-freestanding-host` compiles every TU a bytecode-only
  archive keeps under `-ffreestanding -DURBI_BYTECODE_ONLY=1` with the
  host compiler and matches its undefined symbols against the forbidden
  libc set. It needs no cross toolchain.
- `make test-freestanding` builds every `cross-<preset>-bytecode-only`
  archive and runs `tests/scripts/test-freestanding.sh` on it with the
  preset's `nm`.

`make cross-all` builds every preset, full and bytecode-only, and prints
`size --totals` for each archive. The footprint is recorded in the
CHANGELOG, not capped. `cross-all` and `test-cross-missing-toolchain`
are in `releasetest`; CI runs each preset's two shapes in its `cross`
matrix.

## Stdlib bake (M6 Wave 2)

The standard library ships as a hybrid:

- C-native methods compiled into `liburbi.a` directly (`src/stdlib/*.c`,
  e.g. `object_root.c`, `atom_protos.c`).
- Pre-compiled urbiscript modules baked at build time as a single
  `.rodata` byte blob (`urbi_stdlib_bytecode[]`) and loaded at
  `urbi_open` via the boot table.

Per master spec §5.1.

### Tool

`tools/urbi-compile-stdlib` is a build-time C program that links
against the host `liburbi.a` and walks `src/stdlib/STDLIB_ORDER.txt`.
For each newline-separated `.u` filename, it compiles the source via
the public Urbi compile API and concatenates the resulting v1.5
wire-format buffers into a single blob.

Phase-3 baseline: walks the order file but does not actually compile —
empty `STDLIB_ORDER.txt` produces a 0-length blob. Phase 10 fills in
the `urbi_compile_source` loop once the public compile API and the
`.u` files are in place.

The blob is emitted as `src/stdlib/urbi_stdlib_bytecode.gen.c`:

```c
const unsigned char urbi_stdlib_bytecode[N] = { 0xXX, 0xYY, ... };
const size_t urbi_stdlib_bytecode_len = N;
```

The `.gen.c` is a TRACKED source file (committed to the repo, not
generated under `build/`) so the first build of `liburbi.a` does not
require the bake tool — closing the chicken-and-egg between the tool
and the library it links against.

### Two-pass build

1. `liburbi.a` builds with the placeholder `.gen.c` (the committed
   0-length blob).
2. `tools/urbi-compile-stdlib` links against that intermediate
   `liburbi.a`.
3. Subsequent builds regenerate `.gen.c` whenever
   `src/stdlib/STDLIB_ORDER.txt` or any `src/stdlib/*.u` changes,
   causing `liburbi.a` to re-link with the populated blob.

`Makefile` rules:

```makefile
tools/urbi-compile-stdlib: tools/urbi-compile-stdlib.c \
                           | build/host/liburbi.a
        $(CC) -std=c99 -Wall -Wextra -Wpedantic -Os \
            -Iinclude -o $@ $< build/host/liburbi.a

src/stdlib/urbi_stdlib_bytecode.gen.c: tools/urbi-compile-stdlib \
                                        src/stdlib/STDLIB_ORDER.txt \
                                        $(wildcard src/stdlib/*.u)
        ./tools/urbi-compile-stdlib \
            src/stdlib/STDLIB_ORDER.txt \
            src/stdlib \
            $@
```

A cycle exists in the dep graph:

```text
liburbi.a → .gen.o → .gen.c → bake-tool → liburbi.a
```

GNU make detects this and silently drops one edge with a one-line
`Circular ... dependency dropped` warning. This is intentional and
correctness-safe: `.gen.c` is a tracked source so the first build of
`liburbi.a` does not need the bake tool, and subsequent rebakes only
happen when `STDLIB_ORDER.txt` or a `.u` changes (then `liburbi.a`
re-links from the regenerated `.gen.o`). The order-only edge
(`| build/host/liburbi.a`) on the bake-tool rule communicates intent —
the tool needs `liburbi.a` to LINK against, but does not need to
relink whenever `liburbi.a`'s contents change.

### Determinism

The bake tool MUST be deterministic — same input `.u` files produce
byte-identical `.gen.c`. Asserted by `tests/scripts/bake_smoke.sh`
(3-run byte-identity check) wired into `make releasetest` as
`test-bake-smoke`. Any non-determinism here would cause spurious
wire-format-hash churn at every build, which would in turn invalidate
the wire-format-hash CI gate (`tests/golden/*-wire-format-hashes.txt`).

### Boot

Phase 4 wires `urbi_module_load(stdlib_blob, len)` into the
`urbi_open` boot path after the C-native protos are installed. Single
ordered module load; no parser/emit involvement at boot.

### Force-regenerate

`make bake-clean` re-runs the bake tool against the current
`STDLIB_ORDER.txt` and `.u` files, overwriting `.gen.c`. Used for
debugging when the committed `.gen.c` drifts from what the current
sources would produce (e.g. a `.u` was edited but `make` did not
notice because the timestamp regressed). Routine builds do not need
this — the dep graph picks up `.u` changes automatically.
