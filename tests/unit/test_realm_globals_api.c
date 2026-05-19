/* SPDX-License-Identifier: BSD-3-Clause */
/* T75: public C API — urbi_realm_set_global / set_global_const / get_global.
 *
 * Verifies:
 *   1. urbi_realm_set_global installs a slot that a script can read.
 *   2. urbi_realm_set_global_const installs/updates a const slot that
 *      a script cannot overwrite (var write fails at runtime).
 *      N.B. CONSTANT enforcement in the IC is limited to slot indices 0-7 at
 *      M5 baseline (packed shape flags; M6 side-table tier lifts the cap).
 *      The test therefore exercises set_global_const on an existing built-in
 *      at slot index 0 ("Object"), which IS in the protected range.
 *   3. urbi_realm_get_global returns URBI_ERR_SLOT_NOT_FOUND when the name
 *      is absent.
 *   4. urbi_realm_set_global on an existing non-const slot overwrites it. */

#include "utest.h"

#include <stddef.h>
#include <string.h>

#include "value/uarena.h"
#include "parse/uast.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "chunk/uchunk.h"
#include "parse/uparse.h"
#include "vm/uvm.h"
#include "urbi/urbi.h"
#include "realm/urealm.h"   /* URealm.global_object */
#include "object/uobject.h" /* T68: deep prototype-graph build for PROTO_DEPTH test */

#define UTEST(name) static void name(void)

/* Helper: compile + run source under the VM's global Realm (same realm that
 * urbi_run_chunk always uses internally); returns URBI_OK or error code. */
static int compile_and_run(UVM *vm, const char *src, UValue *out_result)
{
    URealm *realm = urbi_realm_global(vm);
    if (realm == NULL) return URBI_ERR_OOM;

    ULexer   lex;
    UArena   arena;
    UModule  module = {0};
    UEmitter e;
    UParser  p;
    UAstNode *node;

    ulex_init(&lex, src, strlen(src));
    uarena_init(&arena, 4096);
    uemit_init(&e, &module, &arena, vm, NULL);
    uparse_init(&p, &lex, &arena);

    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) {
            uarena_destroy(&arena);
            uchunk_destroy(&module, NULL);
            return URBI_ERR_COMPILE;
        }
        if (uemit_statement(&e, node) != EMIT_OK) {
            uarena_destroy(&arena);
            uchunk_destroy(&module, NULL);
            return URBI_ERR_COMPILE;
        }
        uarena_reset(&arena);
    }
    if (uemit_finish(&e) != EMIT_OK) {
        uarena_destroy(&arena);
        uchunk_destroy(&module, NULL);
        return URBI_ERR_COMPILE;
    }

    UValue result = {0};
    int rc = urbi_run_chunk(vm, realm, &module, &result);
    if (out_result != NULL) {
        *out_result = result;
    }
    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
    return rc;
}

/* Helpers to build UValue literals without <string.h>. */
static UValue make_int(int64_t n)
{
    UValue v;
    int i;
    v.kind = UVAL_INT;
    for (i = 0; i < 7; i++) v._pad[i] = 0;
    v.v.i = n;
    return v;
}

/* === Tests === */

UTEST(set_global_then_script_reads) {
    /* Install "myAnswer" = integer 42 on the VM's global realm via the C API,
     * then verify a script can read it back as an integer value.
     *
     * Note: urbi_run_chunk always executes scripts in the VM's global realm
     * (realm argument is reserved for future multi-realm scheduling). */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Auto-create the global realm so we can install on it. */
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    int rc = urbi_realm_set_global(&vm, realm, "myAnswer", 8, make_int(42));
    UASSERT_EQ(URBI_OK, rc);

    UValue result = {0};
    int run_rc = compile_and_run(&vm, "myAnswer", &result);
    UASSERT_EQ(URBI_OK, run_rc);
    UASSERT_EQ((uint8_t)UVAL_INT, result.kind);
    UASSERT_EQ((int64_t)42, result.v.i);

    urbi_vm_destroy(&vm);
}

UTEST(set_global_const_blocks_script_write) {
    /* T67 (REALM-003): urbi_realm_set_global_const on an existing built-in
     * constant slot ("Object", slot index 0) MUST reject the overwrite with
     * URBI_ERR_CONST_SLOT_WRITE — the prior bypass (silent overwrite of the
     * value while preserving the CONSTANT flag) is gone.  A subsequent script
     * "var Object = 42" still fails because "Object" remained CONSTANT.
     *
     * "Object" is at slot index 0 in the global object (populated by
     * urbi_populate_realm_globals).  The IC checks packed shape flags for
     * indices 0-7, so CONSTANT enforcement is guaranteed here. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Re-install "Object" via set_global_const must now reject. */
    UValue sentinel = make_int(99);
    int rc = urbi_realm_set_global_const(&vm, realm, "Object", 6, sentinel);
    UASSERT_EQ(URBI_ERR_CONST_SLOT_WRITE, rc);

    /* Script write must still fail: "Object" is still const (slot index 0). */
    int run_rc = compile_and_run(&vm, "var Object = 42", NULL);
    UASSERT(run_rc != URBI_OK);
    /* Error message must mention the slot name. */
    UASSERT(strstr(vm.last_errmsg, "Object") != NULL);

    urbi_vm_destroy(&vm);
}

UTEST(set_global_const_rejects_existing_const_overwrite) {
    /* T67 (REALM-003): set_global_const on an already-CONSTANT slot must
     * reject with URBI_ERR_CONST_SLOT_WRITE.  Use "Object" (slot index 0,
     * within the v1.0 packed-flag enforcement range 0..7).  The original
     * value must be preserved across rejected overwrites — no silent
     * mutation through the public C API.
     *
     * Note: this test uses a builtin pre-installed by populate.  Testing
     * fresh-install-then-reject would require a slot in the 0..7 range
     * but those are all consumed by populate at v1.0.  M6 will land the
     * spill side-table that lifts the cap; the parallel test for slot
     * indices >= 8 lands then. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Read the populate-installed value of "Object" so we can check it
     * is unchanged after the rejected overwrite. */
    UValue before = {0};
    int rc = urbi_realm_get_global(&vm, realm, "Object", 6, &before);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((uint8_t)UVAL_OBJECT, before.kind);

    /* Attempt to overwrite — must reject with URBI_ERR_CONST_SLOT_WRITE. */
    rc = urbi_realm_set_global_const(&vm, realm, "Object", 6, make_int(999));
    UASSERT_EQ(URBI_ERR_CONST_SLOT_WRITE, rc);

    /* Read back via C API — value must still be the populate-installed
     * Object proto (kind UVAL_OBJECT, same pointer payload). */
    UValue after = {0};
    rc = urbi_realm_get_global(&vm, realm, "Object", 6, &after);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((uint8_t)UVAL_OBJECT, after.kind);
    UASSERT(before.v.p == after.v.p);

    /* Re-attempt rejection — the slot is still CONSTANT after the
     * rejected first overwrite, so the second call also rejects. */
    rc = urbi_realm_set_global_const(&vm, realm, "Object", 6, make_int(42));
    UASSERT_EQ(URBI_ERR_CONST_SLOT_WRITE, rc);

    urbi_vm_destroy(&vm);
}

UTEST(get_global_distinguishes_overflow_from_oom) {
    /* T68 (REALM-010): urbi_realm_get_global on a name absent from a
     * prototype graph that exceeds the 64-deep resolve stack must return
     * URBI_ERR_PROTO_DEPTH — NOT URBI_ERR_OOM (the pre-T68 mapping).
     *
     * To trigger overflow, the DFS at uobject_slot.c needs sp >= 64 at a
     * push.  Plain linear depth doesn't suffice (DFS pops before pushing
     * children).  The minimal trigger is 64 siblings at one level plus 2+
     * children at a sibling that's popped while sp is still 63 — the
     * second push then fails the cap check.
     *
     * Build: global_object adds 64 protos.  Then add 2 extra protos to
     * proto_at(0) (the first-popped sibling).  After popping global_object
     * (sp=0), 64 are pushed (sp=64).  Pop proto_at(0) (sp=63), push its
     * first child (sp=64 OK), push the second — `if (sp >= 64) return -1`. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Make 64 fresh objects, register each as a proto on global_object.
     * urbi_object_add_proto PREPENDS — so after the loop the proto_list
     * order is reversed: siblings[63] sits at proto_at(0), siblings[0]
     * sits at proto_at(63).  The DFS pushes in proto_at(n-1)..proto_at(0)
     * order, so proto_at(0) = siblings[63] is on the top of the stack and
     * pops FIRST.  To trigger overflow we need the first-popped sibling
     * (sp=63 at pop time) to have 2+ children — pushing the second one
     * fails the cap check at sp >= 64. */
    UObject *siblings[64];
    for (int i = 0; i < 64; i++) {
        siblings[i] = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
        UASSERT(siblings[i] != NULL);
        int rc = urbi_object_add_proto(&vm, realm->global_object, siblings[i]);
        UASSERT_EQ(URBI_OK, rc);
    }

    /* Add 2 protos to siblings[63] — last-prepended → proto_at(0) on
     * global_object → popped first when sp = 63.  Pushing the second
     * grandchild then trips `if (sp >= URBI_RESOLVE_STACK_CAP) return -1`. */
    UObject *grand_a = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *grand_b = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(grand_a != NULL);
    UASSERT(grand_b != NULL);
    UASSERT_EQ(URBI_OK, urbi_object_add_proto(&vm, siblings[63], grand_a));
    UASSERT_EQ(URBI_OK, urbi_object_add_proto(&vm, siblings[63], grand_b));

    /* get_global on a name absent from the entire graph must overflow. */
    UValue out = {0};
    int rc = urbi_realm_get_global(&vm, realm,
                                   "absent_in_deep_graph", 20, &out);
    UASSERT_EQ(URBI_ERR_PROTO_DEPTH, rc);
    /* The error code MUST NOT be OOM — pre-T68 returned OOM here. */
    UASSERT(rc != URBI_ERR_OOM);

    urbi_vm_destroy(&vm);
}

UTEST(get_global_returns_slot_not_found_when_absent) {
    /* urbi_realm_get_global on a name that doesn't exist must return
     * URBI_ERR_SLOT_NOT_FOUND (not a crash, not URBI_OK). */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UValue out = {0};
    int rc = urbi_realm_get_global(&vm, realm,
                                   "totally_absent_xyz", 18, &out);
    UASSERT_EQ(URBI_ERR_SLOT_NOT_FOUND, rc);

    urbi_vm_destroy(&vm);
}

UTEST(set_global_distinguishes_const_reject_from_oom) {
    /* T68 (REALM-004): set_global on an existing CONSTANT slot must return
     * URBI_ERR_CONST_SLOT_WRITE — distinct from URBI_ERR_OOM (which the
     * pre-T68 code emitted indiscriminately for any non-OK path).
     *
     * "Object" is at slot index 0 and CONSTANT after populate; set_global
     * (non-const variant) must NOT bypass the CONSTANT bit. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    int rc = urbi_realm_set_global(&vm, realm, "Object", 6, make_int(99));
    UASSERT_EQ(URBI_ERR_CONST_SLOT_WRITE, rc);
    /* The error code MUST NOT be OOM — pre-T68 returned OOM here. */
    UASSERT(rc != URBI_ERR_OOM);

    /* set_global_const path: same disambiguation — already-const rejection
     * comes back as URBI_ERR_CONST_SLOT_WRITE, not URBI_ERR_OOM. */
    rc = urbi_realm_set_global_const(&vm, realm, "Object", 6, make_int(99));
    UASSERT_EQ(URBI_ERR_CONST_SLOT_WRITE, rc);
    UASSERT(rc != URBI_ERR_OOM);

    urbi_vm_destroy(&vm);
}

UTEST(populate_and_set_global_const_share_reject_logic) {
    /* T70 (REALM-023): both code paths that install CONSTANT slots on
     * realm->global_object route through the shared realm_install_const
     * helper.  Verify the share: every registry entry installed by
     * populate within the v1.0 packed-flag enforcement range (slots 0..7)
     * is rejected by set_global_const.  This is the regression-guard
     * against the helper's reject-already-CONSTANT predicate diverging
     * from populate's install path.
     *
     * Slots >= 8 are not covered by this test — populate's
     * install_property path short-circuits at urbi_shape_transition_property
     * because the packed-nibble shift is out of range past 31 bits, so
     * UProps[idx] stays NULL and the helper's UProps fallback can't see
     * a CONSTANT mark.  M6 spill-side-table will lift the cap; until
     * then this is a documented v1.0 limitation (REVIVAL.md §14
     * S-globals-cap-8).  The 7 names below cover the entire enforced
     * range — "Object" (slot 0) through "List" (slot 7) — so any future
     * helper divergence from populate's transition-property behaviour
     * surfaces here. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Registry order: Object, Integer, Float, String, Boolean, Nil, Void,
     * List → slots 0..7.  Each must be CONSTANT-rejected.  M6 Phase 4
     * renamed the M5 placeholder "Bool" → "Boolean" when promoting the
     * row from resolve_nil_placeholder to resolve_atom_boolean. */
    static const struct { const char *name; size_t len; } slot_0_to_7[] = {
        { "Object",  6 },
        { "Integer", 7 },
        { "Float",   5 },
        { "String",  6 },
        { "Boolean", 7 },
        { "Nil",     3 },
        { "Void",    4 },
        { "List",    4 },
    };
    for (size_t i = 0;
         i < sizeof(slot_0_to_7) / sizeof(slot_0_to_7[0]); i++) {
        int rc = urbi_realm_set_global_const(&vm, realm,
                                             slot_0_to_7[i].name,
                                             slot_0_to_7[i].len,
                                             make_int(123));
        UASSERT_EQ(URBI_ERR_CONST_SLOT_WRITE, rc);
    }

    urbi_vm_destroy(&vm);
}

/* REALM-002 closure (defer:M6 → closed at v0.6.1):
 *
 * Audit finding [unsafe]: 'urbi_populate_realm_globals install_property_idx_guard
 * — is_const silently ignored for slot indices >= 8'.
 *
 * Today's documented contract (urealm_globals.c:287-300): the v1.0 packed-
 * nibble form of UShape.flags is 4 bits/slot across a single uint32_t
 * (8 slots worth).  urbi_shape_transition_property's bit-shift arithmetic
 * (shift = slot_index * 4) is undefined behaviour at slot_index >= 8 — UB
 * that the explicit `idx < 8` gate suppresses.  The slot is still locally
 * installed via set_local_slot for any slot index — its value remains
 * reachable by name; only the IC-enforced CONSTANT bit is dropped at
 * slot >= 8.
 *
 * This regression test pins both halves of that contract:
 *   (a) installing slots past index 7 succeeds (URBI_OK), and the value
 *       is reachable via urbi_realm_get_global.
 *   (b) the CONSTANT enforcement at slot >= 8 is best-effort: a re-install
 *       via set_global on the same name with a new value does NOT raise
 *       CONST_SLOT_WRITE (the IC-side gate is not active).
 *
 * The cap is a v1.0 architectural limit; closing-via-doc + this regression
 * test pins the surface so a future M6+spill-table implementation can lift
 * it without silently regressing the contract.  See REVIVAL.md §14 row
 * S-globals-cap-8 + design-risks 'M6: realm-globals UProps spill side-table'.
 */
UTEST(set_global_const_past_slot_7_installs_without_const_enforcement) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    /* The new realm starts with 15 builtin globals installed by populate
     * (Object, Boolean, Nil, Void, Realm, Math, System, Date, ... per the
     * urbi_builtin_registry).  Adding our own const slot lands well past
     * the 8-slot packed-flag cap. */
    const char *name = "MyConstPast8";
    UASSERT_EQ(URBI_OK,
               urbi_realm_set_global_const(&vm, realm, name,
                                           strlen(name), make_int(100)));

    /* (a) Slot is installed and readable via the public API. */
    UValue out = {0};
    UASSERT_EQ(URBI_OK,
               urbi_realm_get_global(&vm, realm, name,
                                     strlen(name), &out));
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ((int64_t)100, out.v.i);

    /* (b) Const enforcement at slot >= 8 is best-effort.  set_global on
     * the same name does NOT reject — the slot's value updates.  This
     * pins the documented behaviour; future M6+spill-table work that
     * promotes CONSTANT to slots >= 8 should flip this assertion. */
    int rc = urbi_realm_set_global(&vm, realm, name,
                                   strlen(name), make_int(200));
    UASSERT_EQ(URBI_OK, rc);

    UValue out2 = {0};
    UASSERT_EQ(URBI_OK,
               urbi_realm_get_global(&vm, realm, name,
                                     strlen(name), &out2));
    UASSERT_EQ((int)UVAL_INT, (int)out2.kind);
    UASSERT_EQ((int64_t)200, out2.v.i);

    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

UTEST(set_global_overwrites_non_const) {
    /* urbi_realm_set_global on a name that was already installed (non-const)
     * must update the value, and urbi_realm_get_global must return the
     * new value. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Install "counter" = 1 */
    int rc = urbi_realm_set_global(&vm, realm, "counter", 7, make_int(1));
    UASSERT_EQ(URBI_OK, rc);

    /* Overwrite "counter" = 2 */
    rc = urbi_realm_set_global(&vm, realm, "counter", 7, make_int(2));
    UASSERT_EQ(URBI_OK, rc);

    /* Read back via C API */
    UValue out = {0};
    rc = urbi_realm_get_global(&vm, realm, "counter", 7, &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((uint8_t)UVAL_INT, out.kind);
    UASSERT_EQ((int64_t)2, out.v.i);

    urbi_vm_destroy(&vm);
}

void
test_realm_globals_api_suite(void)
{
    utest_run("set_global: C-installed slot readable from script",
              set_global_then_script_reads);
    utest_run("set_global_const: const-flagged slot rejects script write",
              set_global_const_blocks_script_write);
    utest_run("set_global_const: rejects existing CONSTANT overwrite (T67)",
              set_global_const_rejects_existing_const_overwrite);
    utest_run("set_global: distinguishes const-reject from OOM (T68)",
              set_global_distinguishes_const_reject_from_oom);
    utest_run("get_global: distinguishes proto-depth overflow from OOM (T68)",
              get_global_distinguishes_overflow_from_oom);
    utest_run("populate + set_global_const share reject logic (T70)",
              populate_and_set_global_const_share_reject_logic);
    utest_run("set_global_const past slot 7 installs without const enforcement (REALM-002)",
              set_global_const_past_slot_7_installs_without_const_enforcement);
    utest_run("get_global: absent slot returns URBI_ERR_SLOT_NOT_FOUND",
              get_global_returns_slot_not_found_when_absent);
    utest_run("set_global: overwrites existing non-const slot",
              set_global_overwrites_non_const);
}
