#!/bin/sh
# tests/scripts/check-dependency-pins.sh
#
# Verifies that pinned dependency versions in .github/workflows/ci.yml
# are consistent with the versions recorded in
# docs/reference/embedded-port-sources.md.
#
# Checks only dependencies that are explicitly version-pinned in CI.
# Distribution-packaged toolchains (e.g., gcc-arm-none-eabi via apt)
# are NOT pinned in CI by design — the divergence between the local
# development image (xpack arm 14.2.1) and GHA apt arm is documented
# in docs/reference/embedded-port-sources.md and accepted.
#
# Called by: make test-dependency-pins (RELEASETEST_PHASE1).

set -eu

CI=".github/workflows/ci.yml"
PORTS="docs/reference/embedded-port-sources.md"

fail=0

check_ci() {
    local label="$1"
    local pattern="$2"
    if ! grep -q "$pattern" "$CI"; then
        echo "dependency-pins FAIL: CI missing '$label' (pattern: $pattern)"
        fail=1
    fi
}

check_doc() {
    local label="$1"
    local pattern="$2"
    if ! grep -q "$pattern" "$PORTS"; then
        echo "dependency-pins FAIL: docs/reference/embedded-port-sources.md missing '$label' (pattern: $pattern)"
        fail=1
    fi
}

# ESP-IDF v6.0.1 — pinned as a Docker container tag
check_ci  "ESP-IDF v6.0.1 container"      "espressif/idf:v6.0.1"
check_doc "ESP-IDF v6.0.1 tag"            "v6.0.1"

# pico-sdk 2.2.0 — pinned via git clone --branch in cross-pico-repl job
check_ci  "pico-sdk 2.2.0 clone"          "branch 2.2.0"
check_doc "pico-sdk 2.2.0 tag"            "2.2.0"

# xpack riscv-none-elf-gcc 15.2.0 — pinned via XPACK_VER in cross-riscv job
check_ci  "xpack riscv 15.2.0"            "XPACK_VER=15.2.0"
# Note: xpack riscv is not in embedded-port-sources.md (it is a CI-only
# toolchain pin, not an embedded SDK).  No doc check needed here.

if [ "$fail" -ne 0 ]; then
    exit 1
fi

echo "dependency pins in sync: ESP-IDF v6.0.1, pico-sdk 2.2.0, xpack riscv 15.2.0"
echo "note: gcc-arm-none-eabi is distribution-packaged (not pinned by design)"
echo "      local image uses xpack arm 14.2.1; GHA uses apt arm — divergence is documented"
