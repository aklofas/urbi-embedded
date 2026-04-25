# urbi-embedded documentation

These docs cover the urbi-embedded runtime: the urbiscript language, the C embedding API, and the runtime internals. Implementation source lives in `../src/`; contribution workflow is in `../CONTRIBUTING.md`; release history is in `../CHANGELOG.md`.

---

## Start here

Choose the section for your role. Each entry is either a link to an existing doc or plain text naming a planned doc and its target release. Planned docs are not stubs — they do not exist yet in the tree.

### Writing urbiscript

For anyone learning the language or porting scripts from urbi 2.x.

- [Getting started](language/getting-started.md) — prerequisites, build, first script
- Language tour — planned for `v1.0.0`
- Language reference — planned for `v0.2.0-expressions`
- Standard library reference — planned for `v0.2.0-expressions` (initial), comprehensive at `v0.5.0-stdlib`
- Cookbook — planned for `v1.0.0`
- Using the REPL — planned for `v0.7.0-repl`
- Migration from urbi 2.x — planned, grows incrementally each release

### Embedding the runtime

For application developers linking urbi-embedded into firmware or a host process.

- Embedding guide — planned for `v0.6.0-embedded`
- C API reference — planned for `v0.6.0-embedded`
- REPL protocol — planned for `v0.7.0-repl`
- ROS2 bridge — planned for `v0.8.0-ros2`
- Sandbox API — planned for `v0.6.0-embedded`
- Footprint guide — planned for `v0.6.0-embedded`

### Working on the runtime

For contributors building or modifying the C99 implementation.

- [Architecture](internals/architecture.md) — module map and data-flow overview
- [Opcode reference](internals/opcodes.md) — every opcode: encoding, operands, semantics
- [Bytecode format](internals/bytecode-format.md) — `.urb` on-disk layout
- [Design decisions](internals/design-decisions.md) — rationale log for implementation choices
- [Test harness](internals/test-harness.md) — `utest.h` API and how to add unit tests
- [Code style](STYLE.md) — C naming, memory model, freestanding discipline, const-correctness
- [Language conventions](LANG-CONVENTIONS.md) — numeric types, time literals, enums-as-singletons, C API contract

---

## Supported platforms

| Architecture | Target | Toolchain | Status |
| ------------ | ------ | --------- | ------ |
| x86_64 | Linux (glibc) | system GCC / Clang | gating CI |
| ARM Cortex-M7 | STM32H7 (bare-metal) | `arm-none-eabi-gcc` | gating CI (cross-compile sanity) |
| RISC-V rv32imc | ESP32-C3 (bare-metal) | `riscv64-unknown-elf-gcc` | gating CI (cross-compile sanity) |
| Xtensa LX7 | ESP32-S3 | — | post-v1.0 |

Hardware-in-loop validation on physical STM32H7 and ESP32-C3 is a gate for the `v1.0.0` release. Cross-compile CI jobs verify that the code compiles clean for ARM and RISC-V on every commit; they do not run the test suite on hardware.

CI runs eight jobs on every push: host release, host debug, host ASan, host UBSan, cross-arm, cross-riscv, lint (clang-tidy + cppcheck + GCC `-fanalyzer`), and docs-check. All eight must be green before merge. See `../.github/workflows/ci.yml` for the full matrix.

---

## Build and test quick reference

The commands below apply to the host (Linux x86_64) build. See [Getting started](language/getting-started.md) for prerequisites.

```sh
make              # build liburbi.a (release)
make test         # run unit tests (host, release)
make test-asan    # run unit tests with AddressSanitizer
make test-ubsan   # run unit tests with UBSan
make test-debug   # run unit tests (debug build, no optimisation)
make cross-arm    # sanity cross-compile for ARM Cortex-M7
make cross-riscv  # sanity cross-compile for RISC-V rv32imc
make lint         # run clang-tidy + cppcheck + GCC -fanalyzer
make docs-check   # lint all Markdown docs (markdownlint + link check)
```

---

## Full index

The "Since" column is the first release where the doc ships. Rows without a link are planned docs that do not yet exist in the tree.

### Root

| Doc | Description | Since |
| --- | ----------- | ----- |
| [Roadmap](ROADMAP.md) | Release sequence to v1.0, exit criteria for each release, non-goals | `v0.1.0-skeleton` |
| [Code style](STYLE.md) | C code conventions: naming, memory model, freestanding discipline, const-correctness | `v0.1.0-skeleton` |
| [Language conventions](LANG-CONVENTIONS.md) | urbiscript numeric types, time literals, enums-as-singletons, bytecode flavor descriptor, C API core/aux contract | `v0.1.0-skeleton` |

### language/

| Doc | Description | Since |
| --- | ----------- | ----- |
| [Getting started](language/getting-started.md) | Install prerequisites, build the library, run the unit tests, write and run a first script | `v0.1.0-skeleton` |
| Language tour | Narrative walkthrough: concurrency separators, reactive constructs (`at`, `whenever`, `every`), tags, prototype objects | `v1.0.0` |
| Language reference | Complete formal reference: syntax grammar, expression semantics, statement forms, scoping rules | `v0.2.0-expressions` |
| Standard library reference | Reference for `List`, `Dict`, `Float`, `String`, `Date`, `Duration`, `Tag`, `Event`, and bounded-container variants (`FixedList`, `RingBuffer`) | `v0.2.0-expressions` (initial), comprehensive at `v0.5.0-stdlib` |
| Cookbook | Task-oriented recipes: sensor polling loops, inter-coroutine communication, building sandboxed plugins | `v1.0.0` |
| Using the REPL | NDJSON protocol, per-session lobbies, introspection commands (`:coros`, `:tags`, `:watchers`), hot-reload workflow | `v0.7.0-repl` |
| Migration from urbi 2.x | Semantic divergences from urbi 2.x documented with rationale; grows incrementally with each release | incremental |

### embedding/

| Doc | Description | Since |
| --- | ----------- | ----- |
| Embedding guide | Step-by-step: link the library, plug in allocator and time source, push urbiscript, read results | `v0.6.0-embedded` |
| C API reference | Every public function in `urbi.h` and `urbi_aux.h`: signature, preconditions, error codes, examples | `v0.6.0-embedded` |
| REPL protocol | NDJSON wire format, connection lifecycle, lobby multiplexing, `urbi-send` CLI reference | `v0.7.0-repl` |
| ROS2 bridge | micro-ROS integration: `ros.subscribe()`, `ros.publisher()`, `ros.client()`, reactive topic bindings | `v0.8.0-ros2` |
| Sandbox API | Instruction and allocation budgets, host-call allow-lists, isolation contract | `v0.6.0-embedded` |
| Footprint guide | Flash and RAM breakdown by subsystem; trim strategies for deeply constrained targets | `v0.6.0-embedded` (initial), refresh at `v1.0.0` |

### internals/

| Doc | Description | Since |
| --- | ----------- | ----- |
| [Architecture](internals/architecture.md) | Module map, data-flow between lexer / parser / emitter / VM / GC / scheduler, threading model | `v0.1.0-skeleton` |
| [Bytecode format](internals/bytecode-format.md) | `.urb` on-disk layout: 24-byte header, varint sections, constant table, instruction stream, delta synclines | `v0.1.0-skeleton` |
| [Opcode reference](internals/opcodes.md) | Every opcode in the current set: encoding, operand fields, semantics, pseudocode | `v0.1.0-skeleton` |
| [Test harness](internals/test-harness.md) | `utest.h` header-only harness API, naming conventions, sanitizer and cross-compile targets | `v0.1.0-skeleton` |
| [Design decisions](internals/design-decisions.md) | Rationale log: choices made during implementation with the alternatives that were rejected | `v0.1.0-skeleton` |
| GC internals | Incremental tri-color mark-sweep: write barrier protocol, safe-point discipline, bounded step size, pause budget | `v0.3.0-concurrency` |
| Scheduler internals | Priority-aware cooperative scheduler: coroutine lifecycle, statement-boundary preemption points, tag integration | `v0.3.0-concurrency` |

### reference/

| Doc | Description | Since |
| --- | ----------- | ----- |
| [Peer languages](reference/peer-languages.md) | Comparison with other embeddable scripting languages: feature matrix, detailed peer descriptions, when-to-pick-which decision guide | `v0.1.0-skeleton` |
| Glossary | Definitions for terms used across the docs: watcher, coroutine, tag, arena, module, lobby, syncline | `v1.0.0` |
| FAQ | Frequently asked questions on language semantics, embedding patterns, and platform support | `v1.0.0` |
| Bibliography | Academic papers, language standards, and prior work that informed the design | `v1.0.0` |

---

## Conventions

**Writing styles.** Three registers are used across the tree: *narrative* (tutorials, guides — full sentences, worked examples, deliberate pacing), *terse reference* (language reference, API reference, opcode tables — minimal prose, complete enumeration), *expository* (architecture docs, design decisions — explains the "why" behind choices). Files do not advertise their register; readers pick it up from the shape of the content.

**Cross-links are repo-relative.** All links in this tree use paths relative to the file containing the link, resolved within the repository. They render correctly on GitHub, in local Markdown previewers, and in concatenated PDF output. Do not use absolute paths or external URLs when linking between docs in this tree.

**Single-page PDF.** Starting at `v1.0.0`, a release-time Pandoc export will concatenate the tree into a single PDF for the release page. The docs use Pandoc-friendly Markdown only: no admonition-extension syntax, no static-site-generator frontmatter blocks. All code blocks carry explicit language tags.

**Unwritten docs.** Entries in the index tables that have no Markdown link do not exist yet in the tree. They are listed to show the intended shape of the full documentation set and the release where each will first ship. Do not create stub or placeholder files for planned docs — an empty file is more confusing than no file.

**Migration information.** urbi 2.x divergences are documented in `language/migration-from-urbi-2.md` (planned, grows incrementally). The main docs describe current urbi-embedded behavior without cross-referencing urbi 2.x inline; the migration doc is the single place for divergence details. Readers wanting to know whether a specific 2.x construct still works should consult that doc rather than inferring from the main reference.

**Versioning.** Each doc row in the index carries a "Since" release tag. Docs are added to the tree at the release named — not earlier, not as placeholders. The `v0.1.0-skeleton` tag corresponds to the first end-to-end pipeline: lexer, parser, emitter, VM, and a working REPL.
