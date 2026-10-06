#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# A different compiler reached through the same name must rebuild.
#
# The flag stamp carries the compiler's resolved path and version, not
# only the name make was given.  Without that, an archive compiled by one
# toolchain stays in build/ when another toolchain with the same name
# comes first on PATH, and the firmware linked against it mixes two
# compilers' objects (seen on a bench machine whose PATH put a vendor
# IDE's arm-none-eabi-gcc ahead of the one the archive was built with).
#
# One object is enough to prove the mechanism: build it, build it again
# (nothing to do), then build it with a wrapper of the same name first on
# PATH (a recompile).
set -eu

T=host-stampcheck
D=build/$T
OBJ=$D/src/util/uvarint.o
CC_NAME=${CC:-gcc}
CC_REAL=$(command -v "$CC_NAME")

rm -rf "$D"
mkdir -p "$D/tc"
printf '#!/bin/sh\nexec "%s" "$@"\n' "$CC_REAL" > "$D/tc/$CC_NAME"
chmod +x "$D/tc/$CC_NAME"

make --no-print-directory TARGET=$T CC="$CC_NAME" "$OBJ" > "$D/run1.log" 2>&1
if ! grep -q -- '-c -o' "$D/run1.log"; then
    echo "FAIL: first build compiled nothing"; cat "$D/run1.log"; exit 1
fi
make --no-print-directory TARGET=$T CC="$CC_NAME" "$OBJ" > "$D/run2.log" 2>&1
if grep -q -- '-c -o' "$D/run2.log"; then
    echo "FAIL: an unchanged toolchain rebuilt the object"; cat "$D/run2.log"; exit 1
fi
PATH="$PWD/$D/tc:$PATH" make --no-print-directory TARGET=$T CC="$CC_NAME" "$OBJ" > "$D/run3.log" 2>&1
if ! grep -q -- '-c -o' "$D/run3.log"; then
    echo "FAIL: a different compiler behind the same name did not rebuild"; cat "$D/run3.log"; exit 1
fi
echo "PASS: a changed toolchain behind the same compiler name rebuilds"
