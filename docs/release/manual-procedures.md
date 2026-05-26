# Manual release procedures

> These steps are not automated in CI. They comprise the "shiptest" tier
> defined in [test-tiers.md](test-tiers.md). Required only for stable release
> tags (v1.0.0 and beyond) and the first v1.0-rc. Not required for v0.10.x
> interstitial tags.
>
> Each step is owner-executable (no external infrastructure required).

## Pre-tag checklist

Run these after `make releasetest` passes and before `git tag`.

### H1 — ESP32-S3 hardware bring-up

**Rationale:** CI cross-compiles but does not flash; QEMU smoke covers
reactive semantics but not silicon-level UART/GPIO/heap behavior.

**Steps:**

1. Flash `examples/esp32s3-eye/` to an ESP32-S3-EYE development board.
2. Connect USB serial monitor at 115200 baud.
3. At startup, verify: `[urbi] realm ready`, no panic output.
4. Send `1+1` via the REPL transport. Expect `{..., "result": "2"}`.
5. Run the blob-tracking demo for 60 s. Verify no crash, no heap exhaustion
   message.

**Pass criterion:** All 5 steps complete without error.

**Tracking:** `hardware-validation.md` §ESP32-S3 — update the "Last verified"
date when this step passes.

### H2 — Raspberry Pi Pico bring-up

**Rationale:** Same rationale as H1. RP2040 is the only non-FPU target.

**Steps:**

1. Flash `examples/pico/repl_demo/` to a Raspberry Pi Pico (original, not
   Pico 2).
2. Connect USB CDC serial at 115200 baud.
3. At startup, verify: `[urbi] realm ready`, no panic output.
4. Send `1+1` via USB CDC. Expect `{..., "result": "2"}`.

**Pass criterion:** All 4 steps complete without error.

**Tracking:** `hardware-validation.md` §Pico.

### H3 — STM32F4 bring-up

**Rationale:** STM32F429I-DISC1 is the primary bare-metal (no-RTOS) target.

**Steps:**

1. Flash `examples/stm32f4-disc/mandelbrot/` to an STM32F429I-DISC1 board.
2. Connect SWD debugger and open a terminal on the UART.
3. At startup, verify the Mandelbrot demo output begins printing.
4. Verify no HardFault output on the SWD console.

**Pass criterion:** Mandelbrot output renders without fault.

**Tracking:** `hardware-validation.md` §STM32F4.

### D1 — README + CHANGELOG accuracy review

**Rationale:** Automated link-checking catches dead links but not stale
version numbers, outdated feature lists, or claims that no longer apply.

**Steps:**

1. Read `README.md` "What works today" section. Verify every listed item
   is actually present in the release tag.
2. Read `CHANGELOG.md` for the new tag section. Verify the ABI version,
   wire format version, and gate counts match the actual `make releasetest`
   output.
3. Check that "Known deferrals" in the release notes matches
   `docs/release/release-readiness.md` rows still marked non-passing.

**Pass criterion:** No stale claims found; if found, fix before tagging.

### D2 — Tag artifact dry-run

**Rationale:** Verifies the tag annotation is well-formed and the signed tag
round-trips cleanly.

**Steps:**

1. Run: `git tag -a vX.Y.Z-slug -m "urbi-embedded vX.Y.Z-slug" --no-sign`
   on a scratch branch (do not push).
2. Run: `git show vX.Y.Z-slug` — verify the annotation is readable.
3. Delete the scratch tag: `git tag -d vX.Y.Z-slug`.
4. Create the real tag with the correct message per `WORKFLOW.md §8`.

**Pass criterion:** Dry-run produces a well-formed annotated tag.

## Post-tag checklist

### P1 — Release notes publication

1. Copy the tag's CHANGELOG section into the GitHub Release description
   using the [release-notes-template.md](release-notes-template.md) structure.
2. Verify "Supported targets", "ABI version", "Wire format version", and
   "Known deferrals" are all present and accurate.

### P2 — Post-tag smoke (release branch)

1. Check out the release tag: `git checkout vX.Y.Z-slug`
2. Run: `make clean && make test`
3. Expect: all tests pass (same as releasetest output).
