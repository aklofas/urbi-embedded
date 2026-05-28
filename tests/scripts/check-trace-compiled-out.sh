#!/bin/sh
# check-trace-compiled-out.sh — verify a URBI_TRACE-OFF archive contains no
# trace ring/emit internals. Run against the default (URBI_TRACE undefined)
# liburbi.a. The trace control-API stubs (urbi_trace_set_level / _get_level /
# _set_level_all / _snapshot / _stats / _channel_name) are INTENTIONALLY linked
# in both build modes so embedder code compiles either way — they are NOT in
# the forbidden list below.
set -e

AR="${1:-build/host/liburbi.a}"
NM="${NM:-nm}"

if [ ! -f "$AR" ]; then
    echo "check-trace-compiled-out: archive not found: $AR" >&2
    exit 1
fi

# Internals that must be ABSENT when URBI_TRACE is off (preprocessor-stripped).
forbidden='urbi_trace_emit urbi_trace_emit_str urbi_trace_channel_level urbi_trace_init utrace_format urbi_trace_flush_to_writer'

leaked=''
for sym in $forbidden; do
    if "$NM" "$AR" 2>/dev/null | grep -qE " [Tt] ${sym}\$"; then
        leaked="$leaked $sym"
    fi
done

if [ -n "$leaked" ]; then
    echo "FAIL: trace internals leaked into URBI_TRACE-off archive:$leaked" >&2
    echo "      (the URBI_TP macros should be preprocessor-stripped when URBI_TRACE=0)" >&2
    exit 1
fi

echo "PASS: trace internals absent from URBI_TRACE-off archive ($AR)"
