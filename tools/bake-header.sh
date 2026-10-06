#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Turns an urbiscript file into a C header holding its serialized chunk.
#
#   tools/bake-header.sh <urbi binary> <file.u> <out.h> <symbol>
#
# The host `urbi --dump-wire-format` writes the chunk the library loads
# with urbi_load; the format does not depend on the target.  od is POSIX,
# so no hexdump tool is needed on the bench or in CI.
set -eu
URBI=$1; IN=$2; OUT=$3; SYM=$4
RAW="$OUT.uc"
trap 'rm -f "$RAW"' EXIT
"$URBI" --dump-wire-format "$IN" > "$RAW"
len=$(wc -c < "$RAW" | tr -d ' ')
if [ "$len" -eq 0 ]; then echo "bake-header: $IN produced no bytes" >&2; rm -f "$RAW"; exit 1; fi
{
    printf '/* Generated from %s by tools/bake-header.sh; do not edit. */\n' "$(basename "$IN")"
    printf '#include <stddef.h>\n'
    printf 'static const unsigned char %s[] = {\n' "$SYM"
    od -An -v -tx1 "$RAW" | sed 's/ *\([0-9a-f][0-9a-f]\)/0x\1,/g'
    printf '};\nstatic const size_t %s_len = %su;\n' "$SYM" "$len"
} > "$OUT"
rm -f "$RAW"
