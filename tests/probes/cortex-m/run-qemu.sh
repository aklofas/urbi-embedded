#!/bin/sh
# Runs one probe .elf under qemu's Cortex-M4 MPS2 model and judges it by
# the PASS line probe_verdict prints: qemu's own exit status does not
# carry the guest's.
set -u
ELF=$1
OUT=$(timeout 120 qemu-system-arm -M mps2-an386 -cpu cortex-m4 \
        -semihosting-config enable=on,target=native \
        -nographic -monitor none -serial none -kernel "$ELF" 2>&1)
echo "$OUT"
case "$OUT" in
    *": PASS"*) exit 0 ;;
    *) echo "probe did not pass: $ELF"; exit 1 ;;
esac
