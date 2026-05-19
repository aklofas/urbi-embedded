/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_repl_uproto_readonly.c — v0.9.1 UPROTO_FLAG_READONLY
 *
 * Verifies OP_SETSLOT denies mutation when the receiver UObject carries
 * URBI_OBJ_FLAG_READONLY (= UPROTO_FLAG_READONLY) per spec §4.2.
 *
 * Task 3 lands the flag + enforcement.  Task 4 sets the flag on the 15
 * builtin atom protos at boot — those tests live in test_atom_protos_readonly.
 */

#include "utest.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "urbi/object.h"
#include "object/uobject.h"
#include "realm/urealm.h"
#include "vm/uvm.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ---- T1: SETSLOT on a readonly UObject raises TypeError --------------- */
UTEST(setslot_on_readonly_uobject_raises)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    /* Read the realm-global "Object" (the atom-Object UObject). */
    UValue obj_v;
    int rc = urbi_realm_get_global(&vm, r, "Object", 6, &obj_v);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ(obj_v.kind, (uint8_t)UVAL_OBJECT);

    UObject *atom_obj = (UObject *)obj_v.v.p;
    UASSERT(atom_obj != NULL);

    /* Mark it readonly directly (Task 4 wires this at boot for all
     * 15 atom protos; here we drive the OP_SETSLOT path in isolation). */
    atom_obj->flags |= UPROTO_FLAG_READONLY;

    /* Attempt mutation via bytecode: `Object.foo = 5` must raise. */
    char buf[256];
    buf[0] = '\0';
    rc = urbi_repl_eval(&vm, NULL, "Object.foo = 5", 14, buf, sizeof(buf));

    /* URBI_OK would mean the write succeeded — bug.  Any non-OK rc plus a
     * "frozen prototype" diagnostic in buf is the correct outcome.  The
     * VM's existing error-translation collapses SETSLOT TypeError to
     * URBI_ERR_STRAND_FATAL for repl_eval; the diagnostic is what we pin. */
    UASSERT(rc != URBI_OK);
    UASSERT(strstr(buf, "frozen") != NULL ||
            strstr(buf, "UPROTO_READONLY") != NULL ||
            strstr(buf, "TypeError") != NULL);

    /* Clear the readonly bit so urbi_vm_destroy's stdlib teardown doesn't
     * trip over it (host-side C destroys go through the same chains and
     * the readonly flag was added with bytecode-side enforcement only,
     * but be defensive — leave the world as we found it). */
    atom_obj->flags &= ~(uint32_t)UPROTO_FLAG_READONLY;

    urbi_vm_destroy(&vm);
}

/* ---- T2: SETSLOT on a mutable (non-readonly) clone succeeds ----------- */
UTEST(setslot_on_mutable_clone_succeeds)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    /* A cloned Object is NOT marked readonly by clone; mutation works. */
    char buf[256];
    buf[0] = '\0';
    const char *src =
        "var x = Object.clone(); x.foo = 5; x.foo";
    int rc = urbi_repl_eval(&vm, NULL, src, (size_t)strlen(src),
                            buf, sizeof(buf));

    UASSERT_EQ(rc, URBI_OK);
    UASSERT(strcmp(buf, "5") == 0);

    urbi_vm_destroy(&vm);
}

/* ---- T3: clearing the readonly bit restores mutability ---------------- */
UTEST(setslot_readonly_can_be_cleared)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    URealm *r = urbi_realm_global(&vm);
    UValue obj_v;
    UASSERT_EQ(urbi_realm_get_global(&vm, r, "Object", 6, &obj_v), URBI_OK);

    UObject *atom_obj = (UObject *)obj_v.v.p;
    /* Verify the flag macros agree on the bit value. */
    UASSERT_EQ((unsigned)UPROTO_FLAG_READONLY,
               (unsigned)URBI_OBJ_FLAG_READONLY);

    atom_obj->flags |= UPROTO_FLAG_READONLY;
    UASSERT((atom_obj->flags & URBI_OBJ_FLAG_READONLY) != 0U);

    atom_obj->flags &= ~(uint32_t)UPROTO_FLAG_READONLY;
    UASSERT((atom_obj->flags & URBI_OBJ_FLAG_READONLY) == 0U);

    urbi_vm_destroy(&vm);
}

/* ---- T4: all builtin atom protos are marked readonly at boot ---------- */
UTEST(all_builtin_atom_protos_are_readonly)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    /* Names that must resolve to readonly UObjects at boot.  Tracks the
     * urbi_atom_protos_mark_readonly cohort: atom singletons + runtime-
     * type singletons; excludes Global (mutable per spec §4.1).
     * v0.9.1 Phase 5 adds "Lobby" — the lobby-stdlib proto. */
    static const char *READONLY_NAMES[] = {
        "Object", "Integer", "Float", "String",
        "Boolean", "Nil", "Void",
        "List", "Dict", "Symbol",
        "Tag", "Event", "Mutex", "Date", "Duration",
        "Lobby",
        NULL
    };

    size_t i;
    for (i = 0; READONLY_NAMES[i] != NULL; i++) {
        const char *name = READONLY_NAMES[i];
        UValue v;
        int rc = urbi_realm_get_global(&vm, r, name,
                                       (size_t)strlen(name), &v);
        UASSERT_EQ(rc, URBI_OK);
        UASSERT_EQ(v.kind, (uint8_t)UVAL_OBJECT);
        UObject *p = (UObject *)v.v.p;
        UASSERT(p != NULL);
        UASSERT((p->flags & UPROTO_FLAG_READONLY) != 0U);
    }

    urbi_vm_destroy(&vm);
}

/* ---- T5: Global is NOT readonly (mutable shared atom per spec §4.1) --- */
UTEST(global_atom_is_not_readonly)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    URealm *r = urbi_realm_global(&vm);
    UValue v;
    int rc = urbi_realm_get_global(&vm, r, "Global", 6, &v);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ(v.kind, (uint8_t)UVAL_OBJECT);

    UObject *p = (UObject *)v.v.p;
    UASSERT(p != NULL);
    UASSERT((p->flags & UPROTO_FLAG_READONLY) == 0U);

    urbi_vm_destroy(&vm);
}

/* ---- T6: Object.foo = 5 at fresh boot raises (without our test poking) - */
UTEST(boot_object_setslot_raises_typeerror)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    char buf[256];
    buf[0] = '\0';
    int rc = urbi_repl_eval(&vm, NULL, "Object.foo = 5", 14, buf, sizeof(buf));
    UASSERT(rc != URBI_OK);
    UASSERT(strstr(buf, "frozen") != NULL ||
            strstr(buf, "UPROTO_READONLY") != NULL ||
            strstr(buf, "TypeError") != NULL);

    urbi_vm_destroy(&vm);
}

/* ---- suite entry ------------------------------------------------------ */
void
test_repl_uproto_readonly_suite(void)
{
    printf("test_repl_uproto_readonly\n");
    utest_run("setslot_on_readonly_uobject_raises", setslot_on_readonly_uobject_raises);
    utest_run("setslot_on_mutable_clone_succeeds",  setslot_on_mutable_clone_succeeds);
    utest_run("setslot_readonly_can_be_cleared",    setslot_readonly_can_be_cleared);
    utest_run("all_builtin_atom_protos_are_readonly", all_builtin_atom_protos_are_readonly);
    utest_run("global_atom_is_not_readonly",        global_atom_is_not_readonly);
    utest_run("boot_object_setslot_raises_typeerror", boot_object_setslot_raises_typeerror);
}
