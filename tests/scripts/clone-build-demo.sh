#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# clone-build-demo.sh — verify each shipped-port example builds from a pristine
# tree (no stale build/ artifacts).  Host-side: the Linux REPL + each cross
# firmware.  Hardware flashing is manual (see docs/release/clone-build-demo.md).
# Exits non-zero on any build failure.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

# refactor-3 BLD-07: build from a REAL `git clone`, not a cleaned tree.
if [ -n "$(git -C "$ROOT" status --porcelain)" ]; then
    echo "WARNING: working tree has uncommitted changes — the clone builds the committed state only"
fi
CLONE_TMP=$(mktemp -d)
trap 'rm -rf "$CLONE_TMP"' EXIT
BRANCH=$(git -C "$ROOT" rev-parse --abbrev-ref HEAD)
echo "== fresh clone =="
if [ "$BRANCH" = "HEAD" ]; then
    git -C "$ROOT" clone --quiet . "$CLONE_TMP/clone"
    git -C "$CLONE_TMP/clone" checkout --quiet "$(git -C "$ROOT" rev-parse HEAD)"
else
    git -C "$ROOT" clone --quiet --branch "$BRANCH" . "$CLONE_TMP/clone"
fi
cd "$CLONE_TMP/clone"
echo "  ok: pristine tree at $(git rev-parse --short HEAD) ($BRANCH)"

echo "== linux host REPL =="
make all >/dev/null
test -x build/host/urbi || { echo "FAIL: urbi REPL not built"; exit 1; }
echo "1 + 2" | build/host/urbi -i | grep -q '\] 3' || { echo "FAIL: REPL smoke"; exit 1; }
echo "  ok: linux-repl"

echo "== pico firmware =="
# `cross-pico-repl` builds the cross liburbi.a (always — catches compile errors).
# The flashable .uf2 is built by the pico-sdk CMake flow (test-cross-pico-repl-elf),
# gated behind PICO_SDK_PATH (or ../tools/pico-sdk) like the ESP32 IDF gate.
make cross-pico-repl >/dev/null
PSP="${PICO_SDK_PATH:-$ROOT/../tools/pico-sdk}"
export PICO_SDK_PATH="$PSP"
if [ -d "$PSP" ]; then
  rm -f examples/pico/repl_demo/build/repl_demo.uf2   # avoid a stale-artifact false pass
  make test-cross-pico-repl-elf >/dev/null 2>&1
  test -f examples/pico/repl_demo/build/repl_demo.uf2 || { echo "FAIL: pico uf2"; exit 1; }
  echo "  ok: pico (.uf2 built)"
else
  echo "  SKIP: pico .uf2 (no PICO_SDK_PATH / ../tools/pico-sdk — cross liburbi.a built)"
fi

echo "== stm32f4 firmware =="
# refactor-3 BLD-07 followup: the HAL tree lives OUTSIDE the repo (workspace
# tools/ next to the original checkout); the clone's ../tools is an empty
# tmpdir, so resolve it against $ROOT like the pico SDK above, and SKIP
# loudly when absent (same convention as the pico/esp32 sections).
HAL="$ROOT/../tools/stm32cube-f4"
if [ -d "$HAL" ]; then
  ( cd examples/stm32f4/mandelbrot && rm -rf build && \
    make HAL_ROOT="$HAL" >/dev/null )
  test -f examples/stm32f4/mandelbrot/build/mandelbrot.bin || { echo "FAIL: stm32f4 bin"; exit 1; }
  echo "  ok: stm32f4"
else
  echo "  SKIP: stm32f4 (no ../tools/stm32cube-f4 next to the checkout)"
fi

echo "== esp32-s3 firmware =="
# ESP-IDF build is environment-heavy; gate behind IDF_PATH so CI without IDF skips cleanly.
if [ -n "${IDF_PATH:-}" ]; then
  ( cd examples/esp32/eye_demo && idf.py build >/dev/null )
  test -f examples/esp32/eye_demo/build/eye_demo.bin || { echo "FAIL: esp32 bin"; exit 1; }
  echo "  ok: esp32-s3"
else
  echo "  SKIP: esp32-s3 (IDF_PATH unset — documented manual build)"
fi

echo "ALL CLONE-BUILD-DEMO CHECKS PASSED"
