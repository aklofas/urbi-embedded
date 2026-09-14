/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/uboot.c — the boot table and the VM it builds.  See rt/uboot.h.
 *
 * Nil and Void have no methods at all; their rows exist so that
 * `nil.isA(Object)` resolves. */

#include "rt/uboot.h"
#include "chunk/uchunk.h"

#include "stdlib/object_root.h"
#include "stdlib/isa_method.h"
#include "stdlib/atoms.h"
#include "stdlib/namespaces.h"
#include "stdlib/primitives.h"
#include "stdlib/runtime_types.h"
#include "stdlib/regexp.h"
#include "stdlib/containers.h"
#include "stdlib/lobby_native.h"
#include "stdlib/debug_namespace.h"

/* ====================================================================
 * The table
 * ==================================================================== */

/* Every table is exported by a src/stdlib file with its own count (the
 * array itself is an incomplete type here). */
#define NONE   NULL, 0

/* Every built-in, once.  Order is irrelevant: uboot_init allocates all
 * the protos before it links any parent or installs any method, so a row
 * may name a parent that appears later.
 *
 * READONLY keeps script-side `String.foo = 1` from mutating a prototype
 * every realm shares.  Object is deliberately NOT readonly — extending
 * Object is a documented urbiscript idiom.
 *
 * Every UP_* slot has a row. */
const UBuiltinDef uboot_table[] = {
    { "Object",  UP_OBJECT,  -1,         ustdlib_object_methods, USTDLIB_OBJECT_NMETHODS,    0 },

    /* Atoms.  Each inherits Object, so `1.clone()` and `"s".hasSlot(...)`
     * work without boxing. */
    { "Integer", UP_INTEGER, UP_OBJECT,  ustdlib_int_methods, USTDLIB_INT_NMETHODS,       UBOOT_F_READONLY },
    { "Float",   UP_FLOAT,   UP_OBJECT,  ustdlib_float_methods, USTDLIB_FLOAT_NMETHODS,     UBOOT_F_READONLY },
    { "String",  UP_STRING,  UP_OBJECT,  ustdlib_string_methods, USTDLIB_STRING_NMETHODS,    UBOOT_F_READONLY },
    { "Boolean", UP_BOOLEAN, UP_OBJECT,  ustdlib_bool_methods, USTDLIB_BOOL_NMETHODS,      UBOOT_F_READONLY },
    { "Nil",     UP_NIL,     UP_OBJECT,  NONE,                    UBOOT_F_READONLY },
    { "Void",    UP_VOID,    UP_OBJECT,  NONE,                    UBOOT_F_READONLY },
    { "List",    UP_LIST,    UP_OBJECT,  ustdlib_list_methods, USTDLIB_LIST_NMETHODS, UBOOT_F_READONLY },
    { "Dict",    UP_DICT,    UP_OBJECT,  ustdlib_dict_methods, USTDLIB_DICT_NMETHODS, UBOOT_F_READONLY },
    { "Symbol",  UP_SYMBOL,  UP_OBJECT,  NONE,                    UBOOT_F_READONLY },

    /* Runtime types.  Tag, Event and Job take their methods from
     * rt/usched_natives.c rather than a table column here: they are
     * scheduler surface, installed alongside the globals that go with
     * them. */
    { "Closure", UP_CLOSURE, UP_OBJECT,  NONE,                    UBOOT_F_READONLY },
    { "Tag",     UP_TAG,     UP_OBJECT,  NONE,                    UBOOT_F_READONLY },
    { "Event",   UP_EVENT,   UP_OBJECT,  NONE,                    UBOOT_F_READONLY },
    { "Job",     UP_STRAND,  UP_OBJECT,  NONE,                    UBOOT_F_READONLY },

    /* The exception family.  It is C data rather than an overlay because
     * the runtime has to be able to throw a TypeError before any script
     * has run. */
    { "Exception",        UP_EXCEPTION,  UP_OBJECT,    ustdlib_exception_methods, USTDLIB_EXCEPTION_NMETHODS, 0 },
    { "TypeError",        UP_TYPEERROR,  UP_EXCEPTION, NONE, 0 },
    { "ArityError",       UP_ARITYERROR, UP_EXCEPTION, NONE, 0 },
    { "LookupError",      UP_LOOKUPERROR, UP_EXCEPTION, NONE, 0 },
    { "OutOfMemoryError", UP_OOMERROR,   UP_EXCEPTION, NONE, 0 },
    /* Two-level: an out-of-range index IS a failed lookup, so
     * `catch (var e if e.isA(LookupError))` catches it.  Same shape the
     * pre-refoundation exception_subclasses overlay declared, and what
     * the KeyError row in stdlib.u still assumes. */
    { "IndexError",       UP_INDEXERROR, UP_LOOKUPERROR, NONE, 0 },
    { "RangeError",       UP_RANGEERROR, UP_EXCEPTION, NONE, 0 },
    { "DivByZero",        UP_DIVBYZERO,  UP_EXCEPTION, NONE, 0 },

    /* Output.  Lobby is in every realm's chain, which is what makes a
     * bare `echo("hi")` work everywhere.  READONLY because it carries
     * the `lobbies` session registry, which the runtime maintains and a
     * script may read but not reshape. */
    { "Lobby",   UP_LOBBY,   UP_OBJECT,  ustdlib_lobby_methods, USTDLIB_LOBBY_NMETHODS, UBOOT_F_READONLY },
    { "Channel", UP_CHANNEL, UP_OBJECT,  NONE,                    0 },

    /* Namespaces and primitives. */
    { "Math",     UP_MATH,     UP_OBJECT, NONE,                   UBOOT_F_READONLY },
    { "System",   UP_SYSTEM,   UP_OBJECT, ustdlib_system_methods, USTDLIB_SYSTEM_NMETHODS,   UBOOT_F_READONLY },
    { "Date",     UP_DATE,     UP_OBJECT, ustdlib_date_methods, USTDLIB_DATE_NMETHODS,     0 },
    { "Duration", UP_DURATION, UP_OBJECT, ustdlib_duration_methods, USTDLIB_DURATION_NMETHODS, 0 },
    { "RegExp",   UP_REGEXP,   UP_OBJECT, ustdlib_regexp_methods, USTDLIB_REGEXP_NMETHODS,   0 },
    { "Mutex",    UP_MUTEX,    UP_OBJECT, ustdlib_mutex_methods, USTDLIB_MUTEX_NMETHODS,    0 },

    /* Containers beyond List/Dict, and the Global reflection namespace. */
    { "Pair",    UP_PAIR,    UP_OBJECT,  ustdlib_pair_methods, USTDLIB_PAIR_NMETHODS, 0 },
    { "Triplet", UP_TRIPLET, UP_OBJECT,  ustdlib_triplet_methods, USTDLIB_TRIPLET_NMETHODS, 0 },
    { "Tuple",   UP_TUPLE,   UP_OBJECT,  ustdlib_tuple_methods, USTDLIB_TUPLE_NMETHODS, 0 },
    { "Global",  UP_GLOBAL,  UP_OBJECT,  ustdlib_global_methods, USTDLIB_GLOBAL_NMETHODS, 0 },

    /* REPL introspection, reachable from script as `Debug.coros()`. */
    { "Debug",   UP_DEBUG,   UP_OBJECT,  ustdlib_debug_methods, USTDLIB_DEBUG_NMETHODS, UBOOT_F_READONLY },

    /* Vestigial.  The legacy fallback() reflection mechanism is not
     * coming back, but the marker slot is what scripts test for, so the
     * prototype stays with its `kind` constant and nothing else. */
    { "CallMessage", UP_CALLMESSAGE, UP_OBJECT, NONE, 0 }
};

const uint16_t uboot_table_len = (uint16_t)(sizeof uboot_table / sizeof uboot_table[0]);

#undef NONE

/* ====================================================================
 * Booting
 * ==================================================================== */

int uboot_install_methods(UVM *vm, UObject *proto, const UMethodDef *m, uint16_t n)
{
    if (proto == NULL) return URBI_ERR_INVALID_ARG;
    for (uint16_t i = 0; i < n; i++) {
        /* A zeroed trailing row means someone bumped K_*_NMETHODS without
         * adding the method (the other direction is an "excess elements"
         * compile error).  Fail the boot rather than install a NULL. */
        if (m[i].name == NULL || m[i].fn == NULL) return URBI_ERR_INVALID_STATE;
        USym *name = usym_cstr(vm, m[i].name);
        if (!name) return URBI_ERR_OOM;
        /* `proto` is already in vm->protos[] (or otherwise rooted by the
         * caller), so the allocation below cannot collect it. */
        UClosure *cl = uclosure_native(vm, m[i].fn, m[i].min_args, m[i].max_args);
        if (!cl) return URBI_ERR_OOM;
        cl->name = m[i].name;   /* a string literal in the table: immortal */
        if (uobj_set_local(vm, proto, name, uv_ptr(UV_CELL, cl), 0) < 0) return URBI_ERR_OOM;
    }
    return URBI_OK;
}

/* The loader's allocator, routed at the VM's, so a chunk's buffers come
 * from the same place as everything else the VM owns. */
static void *uboot_chunk_alloc(void *ptr, size_t nbytes, void *ud)
{
    UVM *vm = (UVM *)ud;
    return vm->gc.alloc(ptr, nbytes, vm->gc.alloc_ud);
}

/* Runs the baked stdlib.u blob's root with recv = root_globals, so its
 * top-level `var`s become slots on the shared root object — exactly the
 * old "populate every realm" result, produced once. */
static int uboot_run_stdlib(UVM *vm)
{
    if (urbi_stdlib_bytecode_len == 0) return URBI_OK;

    UProto *root = NULL;
    UChunkLoadError lrc = uchunk_deserialize(&root, urbi_stdlib_bytecode,
                                             urbi_stdlib_bytecode_len,
                                             uboot_chunk_alloc, vm,
                                             vm->last_error, sizeof vm->last_error);
    if (lrc != UCHUNK_LOAD_OK)
        return lrc == UCHUNK_LOAD_OOM ? URBI_ERR_OOM : URBI_ERR_BYTECODE_VERSION_MISMATCH;

    /* The blob runs before any realm exists, on a spare strand with no
     * realm: it only ever touches root_globals through its receiver.
     * Acquired FIRST, because acquiring one can allocate -- and therefore
     * collect -- and the closure built below is reachable from nothing
     * until uexec_call roots it. */
    UStrand *s = uvm_spare_acquire(vm, NULL);
    if (!s) { uchunk_destroy(root, NULL); return URBI_ERR_OOM; }

    UProtoCell *pc = uproto_bind(vm, root);   /* owns `root` either way */
    if (!pc) { uvm_spare_release(vm, s); return URBI_ERR_OOM; }

    /* Pin across the closure allocation: the chunk is reachable from
     * nothing until a closure points at it. */
    pc->cell.flags |= UCELL_F_PINNED;
    UClosure *cl = uclosure_new(vm, root, 0);
    pc->cell.flags &= (uint16_t)~UCELL_F_PINNED;
    if (!cl) { uvm_spare_release(vm, s); return URBI_ERR_OOM; }
    cl->proto_obj = vm->protos[UP_CLOSURE];

    UValue ignored = uv_nil();
    int rc = uexec_call(vm, s, cl, uv_obj(vm->root_globals), NULL, 0, &ignored);
    uvm_spare_release(vm, s);
    return rc == UEXEC_OK ? URBI_OK : URBI_ERR_UNCAUGHT_THROW;
}

int uboot_init(UVM *vm)
{
    if (!vm) return URBI_ERR_INVALID_ARG;
    if (vm->stdlib_booted) return URBI_OK;

    /* Pass 1 — allocate every prototype before linking anything, so a row
     * may name a parent that appears later in the table.  Each lands in
     * vm->protos[] immediately, which is a GC root, so the next
     * allocation cannot reclaim it. */
    for (uint16_t i = 0; i < uboot_table_len; i++) {
        int idx = uboot_table[i].proto_index;
        if (idx < 0 || idx >= UP_COUNT) return URBI_ERR_INVALID_STATE;
        if (vm->protos[idx]) continue;
        vm->protos[idx] = uobj_new(vm, NULL);
        if (!vm->protos[idx]) return URBI_ERR_OOM;
        vm->protos[idx]->cell.flags |= UOBJ_F_IS_PROTO;
    }

    /* Pass 2 — parents. */
    for (uint16_t i = 0; i < uboot_table_len; i++) {
        int parent = uboot_table[i].parent_index;
        if (parent < 0) continue;
        UObject *o = vm->protos[uboot_table[i].proto_index];
        if (o->nprotos == 0 && uobj_add_proto(vm, o, vm->protos[parent]) != 0)
            return URBI_ERR_OOM;
    }

    /* Pass 3 — methods.  isA is installed on the Object root here rather
     * than being a row of its own: it is universal, so it belongs
     * wherever every chain ends. */
    for (uint16_t i = 0; i < uboot_table_len; i++) {
        if (uboot_table[i].nmethods == 0) continue;
        int rc = uboot_install_methods(vm, vm->protos[uboot_table[i].proto_index],
                                       uboot_table[i].methods, uboot_table[i].nmethods);
        if (rc != URBI_OK) return rc;
    }
    {
        int rc = uboot_install_methods(vm, vm->protos[UP_OBJECT], ustdlib_isa_methods, USTDLIB_ISA_NMETHODS);
        if (rc != URBI_OK) return rc;
    }

    /* Pass 4 — the shared root globals object every realm inherits.
     *
     * Its prototype is the LOBBY, not Object: spec section 6 puts the
     * Lobby in every realm's chain, and Lobby's own prototype is Object,
     * so the chain reads globals -> root_globals -> Lobby -> Object and
     * an unqualified `echo("hi")` resolves by inheritance rather than by
     * a second binding.  Every other Lobby slot -- `lobbies`, `wall`,
     * the LobbyMethods overlay -- arrives on the same path. */
    vm->root_globals = uobj_new(vm, vm->protos[UP_LOBBY]);
    if (!vm->root_globals) return URBI_ERR_OOM;
    for (uint16_t i = 0; i < uboot_table_len; i++) {
        const UBuiltinDef *d = &uboot_table[i];
        if (!d->global) continue;
        USym *name = usym_cstr(vm, d->global);
        if (!name) return URBI_ERR_OOM;
        if (uobj_set_local(vm, vm->root_globals, name,
                           uv_obj(vm->protos[d->proto_index]), USLOT_CONSTANT) < 0)
            return URBI_ERR_OOM;
    }
    /* The two value singletons, spelled in lower case, alongside the
     * `Nil` / `Void` prototypes the loop just bound. */
    {
        USym *n = usym_cstr(vm, "nil");
        USym *v = usym_cstr(vm, "void");
        if (!n || !v) return URBI_ERR_OOM;
        if (uobj_set_local(vm, vm->root_globals, n, uv_nil(), USLOT_CONSTANT) < 0) return URBI_ERR_OOM;
        if (uobj_set_local(vm, vm->root_globals, v, uv_void(), USLOT_CONSTANT) < 0) return URBI_ERR_OOM;
    }
    /* Pass 5 — the constant and default SLOTS the table has no column
     * for.  Each is a one-function hook beside the methods it belongs
     * with. */
    {
        USym *k = usym_cstr(vm, "kind");
        UValue v = urbi_make_str_interned(vm, "callmessage", 11);
        if (!k || v.kind == UV_NIL) return URBI_ERR_OOM;
        if (uobj_set_local(vm, vm->protos[UP_CALLMESSAGE], k, v, USLOT_CONSTANT) < 0)
            return URBI_ERR_OOM;
    }
    {
        int rc = urbi_lobby_init(vm);
        if (rc == URBI_OK) rc = urbi_namespaces_init(vm);
        if (rc == URBI_OK) rc = usched_natives_init(vm);
        if (rc == URBI_OK) rc = urbi_primitives_init(vm);
        if (rc == URBI_OK) rc = urbi_exception_init(vm, vm->protos[UP_EXCEPTION]);
        if (rc == URBI_OK) rc = urbi_regexp_init(vm, vm->protos[UP_REGEXP]);
        if (rc != URBI_OK) return rc;
    }

    /* Pass 6 — the script overlay, then the read-only seal.  Sealing LAST
     * matters: the overlay installs methods on the very prototypes the
     * flag protects, and OP_SETSLOT honours the flag. */
    {
        int rc = uboot_run_stdlib(vm);
        if (rc != URBI_OK) return rc;
    }
    for (uint16_t i = 0; i < uboot_table_len; i++) {
        if (uboot_table[i].flags & UBOOT_F_READONLY)
            vm->protos[uboot_table[i].proto_index]->cell.flags |= UOBJ_F_READONLY;
    }

    vm->stdlib_booted = 1;
    return URBI_OK;
}
