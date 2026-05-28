#!/usr/bin/env python3
"""urbi-trace-decode.py — decode a URBT binary trace dump (v0.11.0 ring) into
Chrome Trace Event Format JSON (loadable in Perfetto / chrome://tracing).

  python3 tools/urbi-trace-decode.py DUMP.bin [--out trace.json]
  cat DUMP.bin | python3 tools/urbi-trace-decode.py -  > trace.json

The CHANNELS / SCHEMAS tables mirror src/runtime/utrace_format.c
(k_schema_name[]) and urbi_trace_channel_name(); keep them in sync when a tag
adds a channel or schema. Unknown ids degrade to chan_<n> / schema_<n>.
"""
import argparse
import json
import struct
import sys

URBT_MAGIC = b"URBT"
# header: magic(4) fmt_ver(H) record_bytes(H) count(I) dropped(I) flags(I) = 20 bytes
URBT_HEADER = struct.Struct("<4sHHIII")
RECORD_BYTES = 32  # sizeof(UTraceRecord): 8-byte aligned (NOT 24 — see plan finding)
# fixed prefix (offsets 0..20): ts_us(Q) seq(I) strand_id(H) channel(B) level(B) schema_id(H) _pad(H)
REC_PREFIX = struct.Struct("<QIHBBHH")
PAYLOAD_WORDS = struct.Struct("<II")  # a,b at offset 20; trailing 4 bytes are struct padding

CHANNELS = ["vm", "sched", "gc", "watcher", "event", "tag", "repl", "user"]
SCHEMAS = [
    "milestone", "sched_create", "sched_start", "sched_block", "sched_yield",
    "sched_resume", "sched_exit", "gc_phase", "gc_slice", "gc_alloc_denied",
    "watcher_install", "watcher_fire", "watcher_complete", "event_emit",
    "event_drain", "tag_op", "repl_session", "repl_eval", "user_marker",
]
LEVELS = ["DEBUG", "INFO", "WARN", "ERROR"]
TAG_OPS = ["stop", "block", "unblock", "freeze", "unfreeze"]
STR_SCHEMAS = {0, 18}  # milestone, user_marker carry an 8-byte string payload

# schema id constants used in the event mapping
SCHED_START, SCHED_EXIT = 2, 6
GC_PHASE, GC_SLICE = 7, 8
EVENT_EMIT = 13
TAG_OP = 15
REPL_EVAL = 17


def chan_name(c):
    return CHANNELS[c] if c < len(CHANNELS) else "chan_%d" % c


def schema_name(s):
    return SCHEMAS[s] if s < len(SCHEMAS) else "schema_%d" % s


def level_name(l):
    return LEVELS[l] if l < len(LEVELS) else "?"


class Record:
    __slots__ = ("ts_us", "seq", "strand_id", "channel", "level", "schema_id", "a", "b", "s")


def parse_dump(data):
    if len(data) < URBT_HEADER.size:
        raise ValueError("short dump: %d bytes" % len(data))
    magic, ver, rec_bytes, count, dropped, flags = URBT_HEADER.unpack_from(data, 0)
    if magic != URBT_MAGIC:
        raise ValueError("bad magic %r (not a URBT dump)" % magic)
    if rec_bytes != RECORD_BYTES:
        raise ValueError("record size %d != expected %d" % (rec_bytes, RECORD_BYTES))
    if not (flags & 1):
        raise ValueError("dump not little-endian (flags=0x%x)" % flags)
    recs = []
    off = URBT_HEADER.size
    for _ in range(count):
        if off + rec_bytes > len(data):
            break
        ts, seq, strand, ch, lvl, schema, _pad = REC_PREFIX.unpack_from(data, off)
        r = Record()
        r.ts_us, r.seq, r.strand_id, r.channel, r.level, r.schema_id = ts, seq, strand, ch, lvl, schema
        if schema in STR_SCHEMAS:
            raw = data[off + 20:off + 28]
            r.s = raw.split(b"\x00", 1)[0].decode("ascii", "replace")
            r.a = r.b = 0
        else:
            r.a, r.b = PAYLOAD_WORDS.unpack_from(data, off + 20)
            r.s = None
        recs.append(r)
        off += rec_bytes
    header = {"format_version": ver, "record_bytes": rec_bytes, "count": count,
              "dropped": dropped, "flags": flags, "parsed": len(recs)}
    return header, recs


def _tid(r):
    # strand-bearing records get a per-strand swimlane; others a per-channel lane.
    if r.strand_id:
        return r.strand_id
    return 100000 + r.channel


def to_chrome_trace(header, recs):
    events = []
    open_async = {}  # id -> (name, cat) for unmatched begins

    def begin(name, cat, ts, aid, args):
        events.append({"name": name, "cat": cat, "ph": "b", "ts": ts, "pid": 1,
                       "tid": 0, "id": aid, "args": args})
        open_async[aid] = (name, cat)

    def end(ts, aid):
        if aid in open_async:
            name, cat = open_async.pop(aid)
            events.append({"name": name, "cat": cat, "ph": "e", "ts": ts, "pid": 1,
                           "tid": 0, "id": aid})

    last_ts = 0
    prev_seq = None
    gaps = 0
    per_chan = {}
    for r in recs:
        last_ts = max(last_ts, r.ts_us)
        cat = chan_name(r.channel)
        per_chan[cat] = per_chan.get(cat, 0) + 1
        if prev_seq is not None and r.seq > prev_seq + 1:
            gaps += 1
            events.append({"name": "seq gap (%d records)" % (r.seq - prev_seq - 1),
                           "cat": "meta", "ph": "i", "ts": r.ts_us, "pid": 1, "tid": 0, "s": "g"})
        prev_seq = r.seq
        name = schema_name(r.schema_id)
        args = {"seq": r.seq, "a": r.a, "b": r.b, "level": level_name(r.level)}
        if r.s is not None:
            args = {"seq": r.seq, "label": r.s, "level": level_name(r.level)}
            name = "%s: %s" % (name, r.s)
        if r.schema_id == SCHED_START:
            begin("strand", "sched", r.ts_us, ("strand", r.strand_id), {"seq": r.seq})
        elif r.schema_id == SCHED_EXIT:
            end(r.ts_us, ("strand", r.strand_id))
        elif r.schema_id == REPL_EVAL:
            aid = ("repl", r.strand_id)
            if r.a == 1:
                begin("repl_eval", "repl", r.ts_us, aid, {"seq": r.seq})
            else:
                end(r.ts_us, aid)
        elif r.schema_id == GC_PHASE:
            aid = ("gc", 0)
            if r.a != 0 and aid not in open_async:
                begin("gc_cycle", "gc", r.ts_us, aid, {"seq": r.seq})
            elif r.a == 0:
                end(r.ts_us, aid)
        elif r.schema_id in (EVENT_EMIT, GC_SLICE):
            events.append({"name": name, "cat": cat, "ph": "C", "ts": r.ts_us, "pid": 1,
                           "tid": 0, "args": {("waiters" if r.schema_id == EVENT_EMIT else "bytes"): r.a}})
        else:
            if r.schema_id == TAG_OP:
                args["op"] = TAG_OPS[r.a] if r.a < len(TAG_OPS) else r.a
            events.append({"name": name, "cat": cat, "ph": "i", "ts": r.ts_us, "pid": 1,
                           "tid": _tid(r), "s": "t", "args": args})
    # close any still-open async slices at the last timestamp
    for aid in list(open_async):
        end(last_ts, aid)
    summary = {"parsed": header["parsed"], "dropped": header["dropped"],
               "seq_gaps": gaps, "per_channel": per_chan}
    return {"traceEvents": events, "displayTimeUnit": "ms",
            "metadata": {"urbt": header, "summary": summary}}, summary


def main(argv=None):
    ap = argparse.ArgumentParser(description="Decode a URBT trace dump to Chrome Trace JSON.")
    ap.add_argument("dump", help="URBT dump file, or '-' for stdin")
    ap.add_argument("--out", default="-", help="output JSON path (default stdout)")
    args = ap.parse_args(argv)
    data = sys.stdin.buffer.read() if args.dump == "-" else open(args.dump, "rb").read()
    header, recs = parse_dump(data)
    doc, summary = to_chrome_trace(header, recs)
    out = sys.stdout if args.out == "-" else open(args.out, "w")
    json.dump(doc, out)
    if args.out != "-":
        out.close()
    sys.stderr.write("decoded %d records | dropped %d | seq-gaps %d | channels %s\n"
                     % (summary["parsed"], summary["dropped"], summary["seq_gaps"],
                        ", ".join("%s=%d" % kv for kv in sorted(summary["per_channel"].items()))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
