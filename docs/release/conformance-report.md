# urbi-embedded v1.0 Conformance Report

How the v1.0 runtime maps onto the legacy urbiscript 2.x language, measured two
ways: **coverage** (how much of the legacy surface is implemented) and
**pass-rate** (of what we claim to implement, how much actually passes its
conformance fixtures). Detailed per-construct status lives in
[`docs/language-compatibility-matrix.md`](../language-compatibility-matrix.md);
the 2026-06-01 legacy-completeness audit (internal design analysis) is
summarized here.

## Headline

- **Language core (control / reactive / tags / concurrency / OOP / exceptions):**
  ≈ **75–79%** of the in-scope legacy surface implemented. The differentiating
  parts of urbiscript — separators-encode-concurrency, `at`/`whenever`/`waituntil`,
  first-class tags with stop/block/freeze, prototype OOP, `try`/`catch`/`finally` —
  are done.
- **Stdlib data layer (String / List / Dict / Object-reflection / Integer / Float):**
  ≈ **80% usage-weighted** after the v0.12.4 stdlib-completeness arc (was ≈ 50% at
  the 2026-06-01 compat-2 audit). The audit's biggest gaps — `List.each`/`sort`,
  `Dict.keys`/`values`/`each`, `String.split`/`join`/`format`, `Object.slotNames`/
  reflection, `Integer.times`, `%` modulo, `&&`/`||` — all shipped in v0.12.4.
- **Pass-rate on the implemented surface: 100%.** All active `.chk` conformance
  fixtures pass (331 total fixtures, 63 are intentionally-blocked placeholders
  for deferred/v1.x features, ≈ 268 active — all green), plus 2077 unit cases /
  14925 checks. Blocked-placeholder taxonomy:
  [`chk-deferred-taxonomy.md`](chk-deferred-taxonomy.md).

The two numbers answer different questions: *"can I run an arbitrary legacy
program unmodified?"* → no, ≈ 75–80% of the surface plus the legacy idioms
(`closure`, `callMessage`) are intentionally retired (see divergences). *"does
the language I shipped behave correctly?"* → yes, 100% on the conformance corpus.

## Per-family status

Pulled from the compatibility matrix (see it for per-construct rows). FULL =
legacy-equivalent; PARTIAL = core present, edges deferred; OUT = intentional
scope decision.

| Family | Status | Notes |
|--------|--------|-------|
| Control flow (`if`/`while`/`for`/`for-each`/`switch`/`break`/`continue`) | FULL | C-style `for` / `loop` / `do(recv)` deferred-v1.x |
| Separators / concurrency (`;` `\|` `,` `&`) | FULL | the defining feature; persistent loader strand |
| Reactive (`at` / `whenever` / `waituntil` / events / slot-change) | FULL | `~duration`, `watch(expr)`, `$wheneverOn/Off` deferred-v1.x |
| Tags (scope, `stop`/`block`/`freeze`, enter/leave) | ≈ FULL (~95%) | tag-stop absorption fixed at v1.0; `Tag.begin`/`.end` overlay deferred-v1.x |
| Exceptions (`try`/`catch`/`finally`, guards, `else`, subclasses) | FULL | `finally`-on-normal-path fixed at v1.0; `catch` never catches TAG_STOP/CANCEL (S5a) |
| Prototype OOP (multi-proto MRO, COW, getters/setters, `isA`) | FULL | hidden-class + IC; `UObject` C++ binding intentionally OUT |
| Operators (`+ - * / %`, comparison, `&&` / `\|\|`, Kotlin bitwise methods) | FULL | symbolic bitwise → methods by design (S14); Comparable/Orderable operator-derivation deferred-v1.x |
| Literals (numbers, strings, durations, angles, `pi`) | FULL | adjacent-string-concat + synclines verified at v1.0 |
| Stdlib data layer (String/List/Dict/Object/Integer/Float) | PARTIAL (~80% usage-weighted) | v0.12.4 closed the high-traffic gaps; long-tail methods (`String.replace`/`trim`, `List.head`/`tail`) deferred-v1.x |

## Intentional divergences from legacy 2.x

These are deliberate, documented design decisions (REVIVAL §14), not gaps:

- **`closure` keyword + bare-`function` retired** — eager-by-default with a
  per-parameter `lazy` keyword; parse-error migration traps (§14 S1).
- **`callMessage` / call-reflection retired** — `lazy` covers introspection
  needs; narrowed call-reflection is v1.x (§14.5 / W7).
- **Symbolic bitwise operators → methods** (`and`/`or`/`xor`/`inv`/`shl`/`shr`/
  `ushr`) to avoid colliding with the `&`/`|` separators (§14 S14). `&&`/`||`
  (logical) and `%` (modulo) are present.
- **Block comments are non-nesting** (C-style), locked (§14 S6 / LANG-CONVENTIONS).
- **Distinct `Integer` type** (Lua-5.3 evolution) — legacy is Float-only;
  `.isA(Float)` on integer literals needs minor porting (§14 S14).
- **`import` is not a runtime keyword** — subsystem namespaces (`ros`,
  `Robotics`) are bare realm globals; `import "file.u"` is a compile-time host
  directive (§14 T33/T37).

## Out of v1.0 scope (by design)

- **`UObject` C++ component model** — gone; replaced by the C embedding API
  (§14.4).
- **Host networking (`Socket` / `Server` / TCP / UDP)** — served by the C API +
  the ROS2 transport; a urbiscript `Socket` facet is a possible v1.x/v2.0 overlay
  (§14 T36 / design-risks `compat2-B`).
- **On-silicon micro-ROS / ESP32-C3 / STM32H7** — v1.0 ROS2 is the host
  rcl/Fast-DDS backend (shipped, tested) plus a documented micro-ROS-on-MCU path;
  on-silicon bring-up is v1.x.
- **Vision / audio / media facets** (`UImage`, camera, ASR) — v2.0.
- **urbiscript-level finalizers** (`Object.onCollect`) — native `UType.destroy` +
  `tag.onleave`/`finally` cover deterministic cleanup; v1.x (§14 T28).

## Known issues at v1.0.0

Accepted, non-blocking, targeted for v1.0.1+ (B4 closure triage):

- `v0.10.9-C` — valued-block script-side resume-value (`var x = t.block(default)`);
  the `urbi_tag_block` C API works.
- `v0.10.10-A` — OGET auto-getter dispatch for native closures (worked around with
  call-style methods).
- `v0.10.10-D` — `props_table` propagation on shape transition (ordering workaround).
- `v0.11.3-C` — root-provider imbalance leak detection (diagnostics-only).
- `v1.0-loader-count-quiescence` — host-driver QUIESCENT-edge observability
  (chk-observation only; counter semantics unit-tested).
