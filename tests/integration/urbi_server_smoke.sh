#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# urbi-server smoke test (v0.9.1).
#
# Spins up the daemon on a high port, sends `1+2` via NDJSON, expects the
# `"value":"3"` envelope back, then SIGTERMs the daemon.  Uses python3 as
# the TCP client because nc/ncat is not assumed to be installed.  Skips
# cleanly if python3 is missing.

set -u

BUILD=${BUILD:-build/host}
SERVER=$BUILD/urbi-server
SEND=$BUILD/urbi-send

if [ ! -x "$SERVER" ]; then
    echo "urbi_server_smoke: $SERVER not built; skipping"
    exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "urbi_server_smoke: python3 not found; skipping"
    exit 0
fi

# Pick a high-ish ephemeral port from PID so parallel test runs don't collide.
PORT=$(( ($$ % 40000) + 20000 ))

"$SERVER" --port "$PORT" --quiet &
SERVER_PID=$!
# trap removes the server on any exit (success, failure, signal).
trap 'kill "$SERVER_PID" 2>/dev/null; wait 2>/dev/null' EXIT INT TERM

# Give the listener pthread a moment to come up.
sleep 0.5

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "urbi_server_smoke: server failed to start on port $PORT"
    exit 1
fi

# Eval round-trip via python3.
RESULT=$(python3 - "$PORT" <<'PY'
import socket, sys, time
port = int(sys.argv[1])
s = socket.socket()
s.settimeout(3.0)
s.connect(('127.0.0.1', port))
# Read hello line.
hello = b''
while b'\n' not in hello:
    chunk = s.recv(4096)
    if not chunk:
        break
    hello += chunk
sys.stderr.write('hello: ' + hello.decode(errors='replace').strip() + '\n')
# Send eval op.
s.send(b'{"id":1,"op":"eval","code":"1+2"}\n')
got = b''
deadline = time.time() + 3.0
while time.time() < deadline:
    try:
        chunk = s.recv(4096)
    except socket.timeout:
        break
    if not chunk:
        break
    got += chunk
    if b'"kind":"done"' in got:
        break
s.close()
sys.stdout.write(got.decode(errors='replace'))
PY
)
RC=$?

if [ "$RC" -ne 0 ]; then
    echo "urbi_server_smoke: python client failed (rc=$RC)"
    echo "$RESULT"
    exit 1
fi

# Expect "value":"3" in the response (eval results are stringified).
echo "$RESULT" | grep -q '"value":"3"' || {
    echo "urbi_server_smoke: response missing value:3"
    echo "$RESULT"
    exit 1
}
echo "urbi_server_smoke: OK ($RESULT)" | tr '\n' ' '
echo
exit 0
