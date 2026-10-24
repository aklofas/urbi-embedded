#!/usr/bin/env bash
# run_smoke.sh — QEMU smoke harness.
#
# Boots the smoke binary under espressif/qemu (via `idf.py qemu`),
# captures UART to a log file, then asserts each line in
# expected_markers.txt starts a line, in order.
#
# The path under test: the component's internal-SRAM arena opens a VM,
# a workload baked at build time loads, three payload events go through
# the port's queue and fire a watcher under urbi_step, and the eval
# service answers one request on an in-memory transport.  The log also
# carries the boot heap, the heap after the session and the task stack's
# high-water mark, the figures the board must match.
#
# Exit codes:
#   0 — all markers found in order ("SMOKE PASS")
#   1 — a marker is missing or appears out of order
#   2 — timeout waiting for DONE marker
#
# Env overrides:
#   UART_LOG          path to UART capture file (default /tmp/urbi_qemu_uart.log)
#   QEMU_TIMEOUT_S    seconds to wait for DONE marker (default 30)
#
# Expects idf.py on PATH (sourced via `. $IDF_PATH/export.sh` by the
# caller, e.g. the GitHub Actions step or a local devshell).

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UART_LOG="${UART_LOG:-/tmp/urbi_qemu_uart.log}"
QEMU_TIMEOUT_S="${QEMU_TIMEOUT_S:-30}"
MARKERS_FILE="${HERE}/expected_markers.txt"

if [[ ! -f "${MARKERS_FILE}" ]]; then
    echo "ERR: missing expected_markers.txt at ${MARKERS_FILE}" >&2
    exit 1
fi

# Fresh log per run so stale output from a prior failed run can't
# satisfy markers from this one.
rm -f "${UART_LOG}"
: > "${UART_LOG}"

echo "[run_smoke] launching idf.py qemu (UART -> ${UART_LOG})"
# -no-reboot: exit QEMU on guest halt instead of looping.
#
# idf.py qemu wires UART0 to `mon:stdio` itself (see idf.py source) and the
# `-serial` chardev in --qemu-extra-args is ignored / overridden, so a
# `-serial file:${UART_LOG}` would silently produce a 0-byte file.  The
# reliable capture path is to redirect idf.py's stdout to the log file --
# UART0 output appears there (interleaved with bootloader/idf.py status
# lines, which the marker check at the bottom of this script skips
# ).  </dev/null on stdin keeps the monitor from blocking on TTY.
idf.py qemu --qemu-extra-args "-no-reboot" \
    </dev/null >"${UART_LOG}" 2>&1 &
QEMU_PID=$!

# Wait up to QEMU_TIMEOUT_S for DONE to appear in the log.
deadline=$(( $(date +%s) + QEMU_TIMEOUT_S ))
while (( $(date +%s) < deadline )); do
    if grep -q "^DONE" "${UART_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

# Stop QEMU regardless of outcome.
if kill -0 "${QEMU_PID}" 2>/dev/null; then
    kill -TERM "${QEMU_PID}" 2>/dev/null
    sleep 1
    kill -KILL "${QEMU_PID}" 2>/dev/null
fi
wait "${QEMU_PID}" 2>/dev/null

if ! grep -q "^DONE" "${UART_LOG}" 2>/dev/null; then
    echo "[run_smoke] TIMEOUT after ${QEMU_TIMEOUT_S}s — DONE marker not seen" >&2
    echo "--- UART log dump ---" >&2
    cat "${UART_LOG}" >&2 || true
    echo "--- end UART log ---" >&2
    exit 2
fi

# Order check: each marker's first-match line number must be strictly
# greater than the previous one.  A marker matches only at the start of a
# line, the way the smoke prints it, so bootloader text cannot match.
prev_line=0
fail=0
while IFS= read -r marker || [[ -n "${marker}" ]]; do
    # Skip blank lines / comments in markers file.
    [[ -z "${marker}" || "${marker}" =~ ^# ]] && continue

    line="$(awk -v m="${marker}" 'index($0, m) == 1 { print NR; exit }' "${UART_LOG}")"
    if [[ -z "${line}" ]]; then
        echo "[run_smoke] MISSING marker: ${marker}" >&2
        fail=1
        continue
    fi
    if (( line <= prev_line )); then
        echo "[run_smoke] OUT-OF-ORDER marker at line ${line} (prev was ${prev_line}): ${marker}" >&2
        fail=1
    fi
    prev_line="${line}"
done < "${MARKERS_FILE}"

if (( fail != 0 )); then
    echo "--- UART log dump ---" >&2
    cat "${UART_LOG}" >&2 || true
    echo "--- end UART log ---" >&2
    exit 1
fi

echo "[run_smoke] SMOKE PASS"
exit 0
