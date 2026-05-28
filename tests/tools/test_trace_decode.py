#!/usr/bin/env python3
"""Constructive test for tools/urbi-trace-decode.py: build URBT blobs in memory,
run the decoder as a subprocess, assert the Chrome Trace JSON. No committed
binary golden (avoids endianness/layout fragility)."""
import json
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
DECODER = os.path.join(ROOT, "tools", "urbi-trace-decode.py")

HDR = struct.Struct("<4sHHIII")
PREFIX = struct.Struct("<QIHBBHH")


def rec(ts, seq, strand, ch, lvl, schema, a=0, b=0, s=None):
    out = bytearray(PREFIX.pack(ts, seq, strand, ch, lvl, schema, 0))
    if s is not None:
        payload = s.encode("ascii")[:8].ljust(8, b"\x00")
    else:
        payload = struct.pack("<II", a, b)
    out += payload + b"\x00\x00\x00\x00"  # 4 trailing pad bytes -> 32-byte record
    assert len(out) == 32, len(out)
    return bytes(out)


def dump(records, dropped=0):
    body = b"".join(records)
    return HDR.pack(b"URBT", 1, 32, len(records), dropped, 1) + body


def decode(blob):
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(blob)
        path = f.name
    try:
        out = subprocess.check_output([sys.executable, DECODER, path])
    finally:
        os.unlink(path)
    return json.loads(out)


def test_strand_span_pairs():
    # sched_start(2) then sched_exit(6) for the same strand -> one b/e async pair
    blob = dump([
        rec(100, 1, 7, ch=1, lvl=0, schema=2),   # sched_start, strand 7
        rec(200, 2, 7, ch=1, lvl=0, schema=6, a=7, b=0),  # sched_exit
    ])
    doc = decode(blob)
    evs = doc["traceEvents"]
    begins = [e for e in evs if e["ph"] == "b"]
    ends = [e for e in evs if e["ph"] == "e"]
    assert len(begins) == 1 and len(ends) == 1, evs
    assert begins[0]["ts"] == 100 and ends[0]["ts"] == 200


def test_user_marker_label():
    blob = dump([rec(50, 1, 0, ch=7, lvl=1, schema=18, s="hello")])
    doc = decode(blob)
    insts = [e for e in doc["traceEvents"] if e["ph"] == "i"]
    assert any("hello" in e["name"] for e in insts), insts


def test_counter_and_summary():
    blob = dump([rec(10, 1, 0, ch=4, lvl=0, schema=13, a=3)], dropped=5)  # event_emit, 3 waiters
    doc = decode(blob)
    counters = [e for e in doc["traceEvents"] if e["ph"] == "C"]
    assert counters and counters[0]["args"]["waiters"] == 3
    assert doc["metadata"]["summary"]["dropped"] == 5


def test_seq_gap_detected():
    blob = dump([
        rec(10, 1, 0, ch=2, lvl=0, schema=7, a=1),
        rec(20, 5, 0, ch=2, lvl=0, schema=7, a=0),  # seq jumps 1 -> 5
    ])
    doc = decode(blob)
    assert doc["metadata"]["summary"]["seq_gaps"] == 1


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("PASS", name)
    print("all decoder tests passed")


if __name__ == "__main__":
    main()
