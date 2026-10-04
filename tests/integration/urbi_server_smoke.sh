#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# urbi-server smoke test.
#
# Starts the server on loopback port 0 (and on a Unix socket), reads the
# bound port from its banner, and drives it with python3 clients:
#
#   tcp          hello, then `1+2` gives "value":"3" and "kind":"done"
#   unix         the same exchange over a Unix socket
#   token        without auth an eval gets an error and a closed socket;
#                with auth the client gets auth_ok, then 3
#   leave        a client that disconnects mid-output does not take the
#                server down: the next client still gets 3
#   max-clients  with --max-clients 2, a third concurrent connection is
#                closed without a hello
#
# Skips cleanly if the server is not built or python3 is missing.

set -u

BUILD=${BUILD:-build/host}
SERVER=$BUILD/urbi-server

if [ ! -x "$SERVER" ]; then
    echo "urbi_server_smoke: $SERVER not built; skipping"
    exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "urbi_server_smoke: python3 not found; skipping"
    exit 0
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/urbi-server-smoke.XXXXXX") || exit 1
SERVER_PID=
cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null && wait "$SERVER_PID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

FAIL=0
ok()   { echo "  ok:   $1"; }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# start_server ARGS... — runs the server in the background with its
# banner going to $WORK/banner, waits for the banner, and sets PORT from
# a "listening on HOST:PORT" line when there is one.
start_server() {
    : > "$WORK/banner"
    "$SERVER" "$@" 2> "$WORK/banner" &
    SERVER_PID=$!
    PORT=
    i=0
    while [ $i -lt 50 ]; do
        if grep -q 'listening on' "$WORK/banner" 2>/dev/null; then
            PORT=$(sed -n 's/^urbi-server listening on [0-9.]*:\([0-9][0-9]*\).*/\1/p' "$WORK/banner" | head -n 1)
            return 0
        fi
        kill -0 "$SERVER_PID" 2>/dev/null || break
        sleep 0.1
        i=$((i + 1))
    done
    echo "urbi_server_smoke: server did not start ($*)"
    cat "$WORK/banner"
    return 1
}

# stop_server NAME — SIGTERM is an orderly exit, so anything but status 0
# (a crash, a sanitizer report) fails the case.
stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID"
        st=$?
        SERVER_PID=
        if [ "$st" -ne 0 ]; then
            fail "$1: server exited with status $st"
            cat "$WORK/banner"
        fi
    fi
}

# client MODE ADDR — one python3 client; prints "ok" on success, or what
# it saw on failure.  ADDR is a port number or a socket path.
client() {
    python3 - "$1" "$2" <<'PY'
import socket, sys, time

mode, addr = sys.argv[1], sys.argv[2]

def connect():
    if addr.isdigit():
        s = socket.create_connection(('127.0.0.1', int(addr)), timeout=5.0)
    else:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5.0)
        s.connect(addr)
    return s

def read_until(s, needle, buf=b''):
    """Reads until needle is in buf, the peer closes, or 5 s pass.
    Returns (buf, closed)."""
    deadline = time.time() + 5.0
    while needle not in buf and time.time() < deadline:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        except ConnectionResetError:
            return buf, True
        if not chunk:
            return buf, True
        buf += chunk
    return buf, False

def hello(s):
    buf, _ = read_until(s, b'\n')
    if b'"kind":"hello"' not in buf:
        raise SystemExit('no hello: %r' % buf)
    return buf[buf.index(b'\n') + 1:]

def eval_three(s, rest=b''):
    s.sendall(b'{"id":1,"op":"eval","code":"1+2"}\n')
    got, _ = read_until(s, b'"kind":"done"', rest)
    if b'"value":"3"' not in got or b'"kind":"done"' not in got:
        raise SystemExit('no 3: %r' % got)

if mode == 'eval':
    s = connect()
    eval_three(s, hello(s))
    s.close()
elif mode == 'noauth':
    s = connect()
    rest = hello(s)
    s.sendall(b'{"id":1,"op":"eval","code":"1+2"}\n')
    got, closed = read_until(s, b'\x00', rest)   # read to end of stream
    if b'"kind":"error"' not in got or not closed:
        raise SystemExit('expected error and close: closed=%s %r' % (closed, got))
    s.close()
elif mode == 'auth':
    s = connect()
    rest = hello(s)
    s.sendall(b'{"id":1,"op":"auth","token":"s3cret"}\n')
    got, _ = read_until(s, b'\n', rest)
    if b'"kind":"auth_ok"' not in got:
        raise SystemExit('no auth_ok: %r' % got)
    eval_three(s, got[got.index(b'\n') + 1:])
    s.close()
elif mode == 'leave':
    s = connect()
    hello(s)
    s.sendall(b'{"id":1,"op":"eval","code":"var i = 0; while (i < 2000) { echo(i); i = i + 1 }"}\n')
    got, _ = read_until(s, b'"kind":"output"')
    if b'"kind":"output"' not in got:
        raise SystemExit('no output before leaving: %r' % got)
    s.close()
elif mode == 'maxclients':
    a = connect(); hello(a)
    b = connect(); hello(b)
    c = connect()
    got, closed = read_until(c, b'\n')
    if got or not closed:
        raise SystemExit('third client not refused: closed=%s %r' % (closed, got))
    c.close()
    # The two admitted clients still work.
    eval_three(a)
    eval_three(b)
    a.close(); b.close()
print('ok')
PY
}

# check NAME MODE ADDR — runs one client and records the verdict.
check() {
    out=$(client "$2" "$3" 2>&1)
    if [ "$out" = "ok" ]; then ok "$1"; else fail "$1: $out"; fi
}

# 1. TCP.
if start_server --tcp 127.0.0.1:0; then
    check "tcp: 1+2 gives 3" eval "$PORT"
else
    fail "tcp: server start"
fi
stop_server tcp

# 2. Unix socket.
SOCK=$WORK/urbi.sock
if start_server --unix "$SOCK"; then
    check "unix: 1+2 gives 3" eval "$SOCK"
else
    fail "unix: server start"
fi
stop_server unix

# 3. Token required.
if start_server --token s3cret --tcp 127.0.0.1:0; then
    check "token: unauthenticated eval is refused and closed" noauth "$PORT"
    check "token: authenticated eval gives 3" auth "$PORT"
else
    fail "token: server start"
fi
stop_server token

# 4. A client that leaves mid-output.
if start_server --tcp 127.0.0.1:0; then
    check "leave: client sends a long eval and disconnects" leave "$PORT"
    sleep 0.3
    if kill -0 "$SERVER_PID" 2>/dev/null; then
        check "leave: the next client still gets 3" eval "$PORT"
    else
        fail "leave: server exited after the client left"
        cat "$WORK/banner"
        SERVER_PID=
    fi
else
    fail "leave: server start"
fi
stop_server leave

# 5. --max-clients 2.
if start_server --max-clients 2 --tcp 127.0.0.1:0; then
    check "max-clients: a third connection is closed without a hello" maxclients "$PORT"
else
    fail "max-clients: server start"
fi
stop_server max-clients

if [ "$FAIL" -ne 0 ]; then
    echo "urbi_server_smoke: $FAIL case(s) failed"
    exit 1
fi
echo "urbi_server_smoke: all cases passed"
exit 0
