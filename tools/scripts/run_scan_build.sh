#!/usr/bin/env bash
set -euo pipefail
SCAN_BUILD="${SCAN_BUILD:-scan-build-18}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

OUT="${1:-build/scan-build-out.txt}"
HTML_DIR="${2:-build/scan-build-html}"
mkdir -p "$(dirname "$OUT")" "$HTML_DIR"

# Build into a dedicated tree so concurrent releasetest gates that
# share build/host/ (test, lint, ...) do not race with scan-build's
# instrumented compile.
"$SCAN_BUILD" --status-bugs -o "$HTML_DIR" make TARGET=host-scan-build all 2>&1 | tee "$OUT"
RC=${PIPESTATUS[0]}

if [[ "$RC" -ne 0 ]]; then
    echo "FAIL: scan-build found bugs (exit $RC); see $HTML_DIR" >&2
    exit "$RC"
fi
echo "OK: 0 scan-build bugs"
