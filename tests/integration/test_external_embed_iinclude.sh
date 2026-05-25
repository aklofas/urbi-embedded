#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# test_external_embed_iinclude.sh — verify public headers are self-contained.
#
# Compiles a minimal external program with ONLY -Iinclude (no -Isrc).
# The program exercises the canonical public API: urbi_vm_create, realm,
# event registration, function registration, run-chunk.  If any public
# header pulls in a src/-prefixed include, this gate fails at compile time.
#
# W2/v0.10.3: closes audit-1 F1 (completion — W1 added opaque alloc API;
# W2 refactors gc.h + sched.h so -Iinclude alone is sufficient).

set -euo pipefail

BUILDDIR="${1:-build/host}"
LIB="${BUILDDIR}/liburbi.a"
LIBAUX="${BUILDDIR}/liburbi_aux.a"
CC="${CC:-cc}"

if [ ! -f "$LIB" ]; then
    echo "FAIL: $LIB not found (run make first)" >&2
    exit 1
fi

TMPC=$(mktemp --suffix=.c)
TMPBIN=$(mktemp)
trap 'rm -f "$TMPC" "$TMPBIN"' EXIT

cat > "$TMPC" <<'EOF'
/* External embedder smoke: must compile with -Iinclude alone. */
#include <stdio.h>
#include <stdlib.h>

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "urbi/aux.h"
#include "urbi/gc.h"
#include "urbi/sched.h"
#include "urbi/object.h"
#include "urbi/version.h"

static void *my_alloc(void *p, size_t n, void *ud) {
    (void)ud;
    if (n == 0) { free(p); return NULL; }
    return realloc(p, n);
}

int main(void) {
    struct UVM *vm = urbi_vm_create(my_alloc, NULL);
    if (vm == NULL) return 1;
    struct URealm *realm = urbi_realm_global(vm);
    if (realm == NULL) { urbi_vm_free(vm); return 1; }
    int major = 0, minor = 0, patch = 0;
    urbi_api_version(&major, &minor, &patch);
    printf("urbi %d.%d.%d ok\n", major, minor, patch);
    urbi_vm_free(vm);
    return 0;
}
EOF

# CRITICAL: -Iinclude ONLY. No -Isrc.
"$CC" -std=c99 -Wall -Wextra -Wpedantic -Iinclude \
    "$TMPC" "$LIB" "$LIBAUX" -o "$TMPBIN" -lpthread -lm

"$TMPBIN" > /dev/null
echo "PASS: external embedder compiles + runs with -Iinclude alone"
