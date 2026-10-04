#!/bin/sh
# A cross build whose compiler is not on PATH must fail with the
# compiler's name, never skip.
set -u
trap 'rm -f presets/absent.mk' EXIT INT TERM
cat > presets/absent.mk <<'EOF'
CROSS_CC       := no-such-compiler-gcc
CROSS_AR       := no-such-compiler-ar
CROSS_NM       := no-such-compiler-nm
CROSS_SIZE     := no-such-compiler-size
CROSS_CPUFLAGS :=
EOF
out=$(make --no-print-directory cross-absent 2>&1)
rc=$?
if [ "$rc" -eq 0 ]; then echo "FAIL: cross-absent succeeded"; exit 1; fi
case "$out" in
    *"no-such-compiler-gcc is not on PATH"*) echo "PASS: missing toolchain is a loud error"; exit 0 ;;
    *) echo "FAIL: wrong message:"; echo "$out"; exit 1 ;;
esac
