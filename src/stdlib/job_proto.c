/* SPDX-License-Identifier: BSD-3-Clause */
/* v0.10.10 / D7-A: Job proto — script-side strand introspection.
 *
 * See job_proto.h banner for design rationale.
 *
 * Four C-native methods installed on vm->job_proto:
 *   current  — returns a fresh Job wrapping vm->cur_strand
 *   tags     — returns a List<Tag> of the strand's ambient tag-scope chain
 *   uid      — returns the strand's pointer as a UVAL_INT
 *   status   — returns the strand's state name as a String
 *
 * P1 design choice: __strand slot stores the UStrand pointer cast to
 * uint64_t.  Resolving a pointer that was already freed (DEAD strand
 * reaped by sched_post_dispatch) returns NULL because the strand is no
 * longer in any realm's strands_head; accessors degrade safely to
 * nil / empty-list / "dead".  No UAF because the resolver never
 * dereferences a pointer not found in strands_head. */

#include "stdlib/job_proto.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "object/uobject.h"       /* urbi_object_alloc, urbi_object_clone,
                                   *   urbi_object_set_local_slot,
                                   *   urbi_object_resolve_slot,
                                   *   urbi_object_add_proto,
                                   *   urbi_object_root, URBI_ATOM_OBJECT */
#include "realm/urealm.h"         /* URealm, strands_head, next_in_vm,
                                   *   urbi_realm_set_global */
#include "runtime/uclosure.h"     /* UClosure, urbi_native_method_fn */
#include "runtime/umacros.h"      /* URBI_INTERNAL_ASSERT, urbi_strlen, urbi_zero */
#include "runtime/ucleanup.h"     /* UCleanupEntry, UCLEANUP_TAG_SCOPE */
#include "sched/ustrand.h"        /* UStrand, USTRAND_STATE_MASK, USTRAND_* */
#include "stdlib/containers.h"    /* urbi_stdlib_list_new_empty,
                                   *   urbi_stdlib_list_append_value */
#include "stdlib/object_root.h"   /* urbi_native_closure_create,
                                   *   urbi_raise_arity, urbi_raise_type,
                                   *   urbi_raise_oom */
#include "urbi/urbi.h"            /* URBI_OK, URBI_ERR_OOM, urbi_make_nil,
                                   *   UEXEC_OK */
#include "value/uintern.h"        /* ustr_intern */
#include "vm/uvm.h"               /* UVM, job_proto */
#include "urbi/object.h"          /* URBI_ATOM_OBJECT */
#include "urbi/types.h"           /* UValue, UVAL_OBJECT, UVAL_INT, UVAL_STR,
                                   *   UVAL_NIL, UVAL_TAG */

/* Forward declaration for the resolve helper used by multiple getters. */
static UStrand *job_resolve_strand(UVM *vm, int64_t ptr_as_int);

/* === Internal strand resolver ==========================================
 *
 * Walk every realm's strands_head list for a strand whose pointer
 * matches ptr_as_int.  Returns NULL if not found (already freed,
 * or ptr was never valid).  O(n) over total live strand count. */
static UStrand *
job_resolve_strand(UVM *vm, int64_t ptr_as_int)
{
    if (vm == NULL || ptr_as_int == 0) return NULL;
    const UStrand *needle = (const UStrand *)(uintptr_t)(uint64_t)ptr_as_int;  /* NOLINT(performance-no-int-to-ptr) — Job __strand stores UStrand* as int */
    for (URealm *r = vm->realms_head; r != NULL; r = r->next_in_vm) {
        for (UStrand *s = r->strands_head; s != NULL; s = s->next_in_realm) {
            if (s == needle) return s;
        }
    }
    return NULL;
}

/* === Internal helper: read __strand slot from a Job instance ===========
 *
 * On success writes *out_ptr and returns URBI_OK.
 * On type error or missing slot sets *out and returns UEXEC_THROW. */
static int
job_read_strand_slot(UVM *vm, UValue self, const char *fn_name,
                     int64_t *out_ptr, UValue *out)
{
    if (self.kind != (uint8_t)UVAL_OBJECT) {
        return urbi_raise_type(vm, fn_name, out);
    }
    UObject *jo = (UObject *)self.v.p;
    const USymbol *sym = (const USymbol *)ustr_intern(vm, "__strand", 8);
    if (sym == NULL) return urbi_raise_oom(vm, out);
    UObject *holder = NULL;
    uint32_t idx = 0U;
    int rc = urbi_object_resolve_slot(vm, jo, sym, &holder, &idx);
    if (rc != 1 || holder == NULL) {
        return urbi_raise_type(vm, fn_name, out);
    }
    UValue uid_v = holder->slots[idx];
    if (uid_v.kind != (uint8_t)UVAL_INT) {
        return urbi_raise_type(vm, fn_name, out);
    }
    *out_ptr = uid_v.v.i;
    return URBI_OK;
}

/* === Job.current (no args; called on Job proto or instance) =============
 *
 * Returns a fresh Job instance wrapping vm->cur_strand.  This replaces
 * the legacy System.currentRunner (legacy/share/urbi/system.u:74-75).
 * Used by mutex.u / monitoring.u / uobject.u for re-entrancy checks. */
static int
job_current_native(UVM *vm, UValue self, UValue *args, uint8_t nargs,
                   UValue *out)
{
    (void)self; (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Job.current", 0, nargs, out);

    UStrand *cur = vm->cur_strand;
    if (cur == NULL) {
        *out = urbi_make_nil();
        return UEXEC_OK;
    }
    *out = urbi_job_make(vm, cur);
    if (out->kind == (uint8_t)UVAL_NIL) return urbi_raise_oom(vm, out);
    return UEXEC_OK;
}

/* === Job.tags (no args; self must be a Job instance) ====================
 *
 * Returns a List<Tag> of every UCLEANUP_TAG_SCOPE entry on the
 * strand's cleanup stack, in bottom-to-top order (connection tag first,
 * innermost user-applied tag last).  Returns an empty list if the
 * strand is already dead/reaped. */
static int
job_tags_native(UVM *vm, UValue self, UValue *args, uint8_t nargs,
                UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Job.tags", 0, nargs, out);

    int64_t ptr_as_int = 0;
    int rc = job_read_strand_slot(vm, self, "Job.tags: self must be a Job", &ptr_as_int, out);
    if (rc != URBI_OK) return rc;

    UObject *list_obj = urbi_stdlib_list_new_empty(vm);
    if (list_obj == NULL) return urbi_raise_oom(vm, out);

    UStrand *s = job_resolve_strand(vm, ptr_as_int);
    if (s != NULL && s->cleanup_base != NULL) {
        size_t i;
        for (i = 0; i < (size_t)s->cleanup_depth; i++) {
            UCleanupEntry *e = &s->cleanup_base[i];
            if (e->kind == UCLEANUP_TAG_SCOPE && e->owning_tag != NULL) {
                UValue tv;
                urbi_zero(&tv, sizeof(tv));
                tv.kind = (uint8_t)UVAL_TAG;
                tv.v.p  = (void *)e->owning_tag;
                int arc = urbi_stdlib_list_append_value(vm, list_obj, tv);
                if (arc != URBI_OK) return urbi_raise_oom(vm, out);
            }
        }
    }

    urbi_zero(out, sizeof(*out));
    out->kind = (uint8_t)UVAL_OBJECT;
    out->v.p  = (void *)list_obj;
    return UEXEC_OK;
}

/* === Job.uid (no args; self must be a Job instance) =====================
 *
 * Returns the stored pointer value as a UVAL_INT.  Stable for the
 * lifetime of the Job object (the pointer is copied at job creation). */
static int
job_uid_native(UVM *vm, UValue self, UValue *args, uint8_t nargs,
               UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Job.uid", 0, nargs, out);

    int64_t ptr_as_int = 0;
    int rc = job_read_strand_slot(vm, self, "Job.uid: self must be a Job", &ptr_as_int, out);
    if (rc != URBI_OK) return rc;

    urbi_zero(out, sizeof(*out));
    out->kind = (uint8_t)UVAL_INT;
    out->v.i  = ptr_as_int;
    return UEXEC_OK;
}

/* === Job.status (no args; self must be a Job instance) ==================
 *
 * Returns the strand's state as a String:
 *   "running"   — USTRAND_RUNNING
 *   "ready"     — USTRAND_READY
 *   "waiting"   — USTRAND_WAITING
 *   "suspended" — USTRAND_SUSPENDED
 *   "dead"      — USTRAND_DEAD (or strand already reaped)
 *   "dormant"   — USTRAND_DORMANT (rare; pre-activation)
 *
 * Mirrors the state-name table in src/repl/urepl_introspect.c:64-90
 * but accessible from script-land per Cat. E re-audit D7 decision. */
static int
job_status_native(UVM *vm, UValue self, UValue *args, uint8_t nargs,
                  UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Job.status", 0, nargs, out);

    int64_t ptr_as_int = 0;
    int rc = job_read_strand_slot(vm, self, "Job.status: self must be a Job", &ptr_as_int, out);
    if (rc != URBI_OK) return rc;

    const UStrand *s = job_resolve_strand(vm, ptr_as_int);
    const char *name;
    if (s == NULL) {
        name = "dead";
    } else {
        uint8_t st = s->state & (uint8_t)USTRAND_STATE_MASK;
        switch (st) {
            case USTRAND_READY:     name = "ready";     break;
            case USTRAND_RUNNING:   name = "running";   break;
            case USTRAND_WAITING:   name = "waiting";   break;
            case USTRAND_SUSPENDED: name = "suspended"; break;
            case USTRAND_DEAD:      name = "dead";      break;
            case USTRAND_DORMANT:   name = "dormant";   break;
            default:                name = "unknown";   break;
        }
    }
    size_t nlen = urbi_strlen(name);
    USymbol *si = (USymbol *)ustr_intern(vm, name, nlen);
    if (si == NULL) return urbi_raise_oom(vm, out);
    urbi_zero(out, sizeof(*out));
    out->kind = (uint8_t)UVAL_STR;
    out->v.p  = (void *)si;
    return UEXEC_OK;
}

/* === Job.jobs (no args; self is the Job proto or any Job instance) ======
 *
 * Walks vm->realms_head × realm->strands_head building a List<Job> of
 * every live (non-DEAD) strand across all realms.  DEAD strands are
 * excluded at source — they'd resolve to "dead" status anyway and their
 * memory may be recycled before the caller inspects the list.
 *
 * GC safety: the list UObject is allocated first and stored in `out` as
 * a UVAL_OBJECT before each urbi_job_make call; both the list and any
 * already-appended Job items are reachable via the list during the walk.
 * urbi_job_make itself stores only a UVAL_INT (pointer-as-integer) in the
 * fresh Job object, which cannot trigger GC movement in v0.10.10's
 * non-moving allocator.  The discipline costs nothing and is future-safe. */
static int
job_jobs_native(UVM *vm, UValue self, UValue *args, uint8_t nargs,
                UValue *out)
{
    (void)self; (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Job.jobs", 0, nargs, out);

    UObject *list_obj = urbi_stdlib_list_new_empty(vm);
    if (list_obj == NULL) return urbi_raise_oom(vm, out);

    /* Root the list in *out before any urbi_job_make allocations so the
     * list is reachable if GC runs. */
    urbi_zero(out, sizeof(*out));
    out->kind = (uint8_t)UVAL_OBJECT;
    out->v.p  = (void *)list_obj;

    for (URealm *r = vm->realms_head; r != NULL; r = r->next_in_vm) {
        for (UStrand *s = r->strands_head; s != NULL; s = s->next_in_realm) {
            if ((s->state & (uint8_t)USTRAND_STATE_MASK) == (uint8_t)USTRAND_DEAD)
                continue;
            UValue jv = urbi_job_make(vm, s);
            if (jv.kind == (uint8_t)UVAL_NIL) return urbi_raise_oom(vm, out);
            int rc = urbi_stdlib_list_append_value(vm, list_obj, jv);
            if (rc != URBI_OK) return urbi_raise_oom(vm, out);
        }
    }

    return UEXEC_OK;
}

/* Method table for Job proto (UNativeMethodDef from stdlib/object_root.h). */
static const UNativeMethodDef JOB_METHODS[] = {
    { "current", job_current_native },
    { "tags",    job_tags_native    },
    { "uid",     job_uid_native     },
    { "status",  job_status_native  },
    { "jobs",    job_jobs_native    }
};

/* === urbi_job_make: public ctor =========================================
 *
 * Allocates a fresh UObject cloned from vm->job_proto, writes the
 * strand pointer as UVAL_INT into the __strand slot, and returns the
 * wrapped UValue.  Returns UVAL_NIL on OOM. */
UValue
urbi_job_make(UVM *vm, UStrand *strand)
{
    URBI_INTERNAL_ASSERT(vm != NULL);
    URBI_INTERNAL_ASSERT(strand != NULL);
    URBI_INTERNAL_ASSERT(vm->job_proto != NULL);

    /* GC soundness (v0.13.2): intern BEFORE the clone — the first runtime
     * intern of "__strand" allocates, and a collection there would sweep
     * the fresh clone held only in this C local (careful-ordering
     * pattern; see urbi_proto_list_create). */
    USymbol *sym = (USymbol *)ustr_intern(vm, "__strand", 8);
    if (sym == NULL) return urbi_make_nil();

    UObject *j = urbi_object_clone(vm, vm->job_proto);
    if (j == NULL) return urbi_make_nil();

    UValue uid_v;
    urbi_zero(&uid_v, sizeof(uid_v));
    uid_v.kind = (uint8_t)UVAL_INT;
    /* Store the pointer as a uint64_t integer.  Safe round-trip on all
     * v1.0 targets (POSIX / Cortex-M / RISC-V / Xtensa — all flat
     * Harvard, sizeof(void*) <= sizeof(uint64_t)). */
    uid_v.v.i = (int64_t)(uintptr_t)strand;  /* NOLINT(performance-no-int-to-ptr) — Job stores UStrand* as int */

    if (urbi_object_set_local_slot(vm, j, sym, uid_v) != 0)
        return urbi_make_nil();

    UValue jv;
    urbi_zero(&jv, sizeof(jv));
    jv.kind = (uint8_t)UVAL_OBJECT;
    jv.v.p  = (void *)j;
    return jv;
}

/* === urbi_job_proto_register: stdlib_boot hook ==========================
 *
 * Allocates vm->job_proto if not yet present, chains it onto Object root,
 * and installs five C-native methods: current, tags, uid, status, jobs (W2).
 * Idempotent: returns URBI_OK immediately if already done. */
int
urbi_job_proto_register(UVM *vm)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;
    if (vm->job_proto != NULL) return URBI_OK;   /* idempotent */

    UObject *p = urbi_object_alloc(vm, URBI_ATOM_OBJECT);
    if (p == NULL) return URBI_ERR_OOM;
    vm->job_proto = p;

    /* Chain Job onto root Object so Job instances inherit Object methods. */
    int rc = urbi_object_add_proto(vm, p, urbi_object_root(vm));
    if (rc != URBI_OK) return rc;

    rc = URBI_REGISTER_METHODS(vm, p, JOB_METHODS);
    if (rc != URBI_OK) return rc;

    /* Mark the proto readonly per the v0.9.1 atom-proto convention.
     * Scripts can clone() Job instances but not setSlot on the proto. */
    p->flags |= (uint32_t)UPROTO_FLAG_READONLY;

    return URBI_OK;
}

/* === urbi_job_proto_register_globals: post-loop realm-global binding ====
 *
 * Called by urbi_populate_realm_globals in the post-loop hook section
 * (after urbi_stdlib_boot has run urbi_job_proto_register).  Binds
 * "Job" as a realm-global slot pointing at vm->job_proto. */
int
urbi_job_proto_register_globals(UVM *vm, struct URealm *realm)
{
    if (vm == NULL || realm == NULL) return URBI_ERR_INVALID_ARG;
    if (vm->job_proto == NULL) return URBI_OK;   /* not yet registered */

    UValue v;
    urbi_zero(&v, sizeof(v));
    v.kind = (uint8_t)UVAL_OBJECT;
    v.v.p  = (void *)vm->job_proto;
    return urbi_realm_set_global(vm, realm, "Job", 3, v);
}
