#!/usr/bin/env bash
# run_smoke.sh — QEMU reactive smoke harness.
#
# Boots the smoke binary under espressif/qemu (via `idf.py qemu`),
# captures UART to a log file, then asserts each line in
# expected_markers.txt appears in order via `grep -nF`.
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
# --no-monitor: don't attach the interactive monitor; we tail the log.
# -no-reboot:   exit QEMU on guest halt instead of looping.
# -serial file: redirect UART to the capture file.
idf.py qemu --no-monitor \
    --qemu-extra-args "-serial file:${UART_LOG} -no-reboot" \
    >/dev/null 2>&1 &
QEMU_PID=$!

# Wait up to QEMU_TIMEOUT_S for DONE to appear in the log.
deadline=$(( $(date +%s) + QEMU_TIMEOUT_S ))
while (( $(date +%s) < deadline )); do
    if grep -qF "DONE" "${UART_LOG}" 2>/dev/null; then
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

if ! grep -qF "DONE" "${UART_LOG}" 2>/dev/null; then
    echo "[run_smoke] TIMEOUT after ${QEMU_TIMEOUT_S}s — DONE marker not seen" >&2
    echo "--- UART log dump ---" >&2
    cat "${UART_LOG}" >&2 || true
    echo "--- end UART log ---" >&2
    exit 2
fi

# Order check: each marker's first-match line number must be strictly
# greater than the previous one.  grep -nF gives "LINE:CONTENT"; we
# slice the leading number with cut.
prev_line=0
fail=0
while IFS= read -r marker || [[ -n "${marker}" ]]; do
    # Skip blank lines / comments in markers file.
    [[ -z "${marker}" || "${marker}" =~ ^# ]] && continue

    match="$(grep -nF -- "${marker}" "${UART_LOG}" | head -n 1 || true)"
    if [[ -z "${match}" ]]; then
        echo "[run_smoke] MISSING marker: ${marker}" >&2
        fail=1
        continue
    fi
    line="${match%%:*}"
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
