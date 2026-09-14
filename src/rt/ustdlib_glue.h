/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/ustdlib_glue.h — the ONE header every src/stdlib file includes.
 *
 * The standard-library bodies are ordinary C functions with one shape:
 *
 *   int method(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
 *
 * They need a handful of runtime services — raise an exception, make a
 * string, build a list, read a slot, call back into script — and nothing
 * else.  This header is that surface.  Keeping it to one include means a
 * stdlib file cannot accidentally reach into the object model, the
 * collector or the scheduler, and the layering gate
 * (tests/scripts/check_rt_layering.sh) enforces exactly that.
 *
 * ARITY.  Bodies do NOT check their own argument count: uexec's native
 * call path rejects the call from the boot table's min_args/max_args
 * before the body runs.  A body that still wants to complain about a
 * *kind* uses urbi_raise_type.
 *
 * STRINGS.  The core has two string representations — USym (interned,
 * immortal, what a literal becomes) and UStr (a GC cell, what
 * concatenation produces).  A body must accept both, so it tests
 * urbi_is_str(v) rather than a kind constant and reads bytes through
 * urbi_str_cstr / urbi_str_size.  urbi_make_str_interned is for names
 * that become slot keys; urbi_make_str is for ordinary values. */

#ifndef URT_STDLIB_GLUE_H
#define URT_STDLIB_GLUE_H

#include "rt/uexec.h"
#include "rt/urealm.h"
#include "rt/uboot.h"
#include "urbi/urbi.h"

/* === freestanding byte helpers ======================================== */

/* The old bodies called these out of src/runtime/umacros.h, which is an
 * old-runtime header the layering gate keeps out of src/rt.  They are
 * three lines each; re-declaring them here costs less than an exception
 * to the rule. */
static inline void urbi_zero(void *dst, size_t n) { memset(dst, 0, n); }
static inline size_t urbi_strlen(const char *s) { return s ? strlen(s) : 0; }
static inline void urbi_memcpy(void *dst, const void *src, size_t n)
{ if (n) memcpy(dst, src, n); }

/* === the current strand =============================================== */

/* The strand in dispatch, i.e. the one whose C-root stack a native must
 * push onto and whose transfer slot a throw lands in.  NULL only outside
 * any call, which no method body can observe. */
UStrand *uvm_current_strand(UVM *vm);
/* The realm that strand belongs to, falling back to the main realm. */
URealm  *uvm_current_realm(UVM *vm);

/* Root a UValue local across an allocation.  Pairs; the name must be a
 * plain identifier because it is pasted into a hidden variable name. */
#define URBI_ROOT(vm, var)   UStrand *_rs_##var = uvm_current_strand(vm); \
                             struct UCRoot _rr_##var = { &(var), _rs_##var ? _rs_##var->croots : NULL }; \
                             if (_rs_##var) _rs_##var->croots = &_rr_##var
#define URBI_UNROOT(vm, var) do { if (_rs_##var) _rs_##var->croots = _rr_##var.prev; } while (0)

/* === raising ========================================================== */

/* Each builds an exception object of the matching built-in prototype,
 * deposits it on the current strand, and returns UEXEC_THROW, so a body
 * can `return urbi_raise_type(...)` directly. */
int urbi_raise_type(UVM *vm, const char *msg, UValue *out);
int urbi_raise_arity(UVM *vm, const char *fn, uint8_t want, uint8_t got, UValue *out);
int urbi_raise_oom(UVM *vm, UValue *out);
int urbi_raise_index(UVM *vm, const char *msg, UValue *out);
int urbi_raise_range(UVM *vm, const char *msg, UValue *out);
int urbi_raise_divzero(UVM *vm, const char *msg, UValue *out);
int urbi_raise_lookup(UVM *vm, const USym *name, UValue *out);
/* `which` is a UP_* index; anything out of range raises a plain Exception. */
int urbi_raise_typed(UVM *vm, int which, UValue *out, const char *msg);

/* === strings ========================================================== */

static inline bool urbi_is_str(UValue v) { return v.kind == UV_STR || v.kind == UV_SYM; }
/* Bytes of a SYM or STR value, always NUL-terminated.  "" for anything
 * else, so a body that forgot a kind check misbehaves visibly rather
 * than dereferencing an integer. */
static inline const char *urbi_str_cstr(UValue v)
{ return urbi_is_str(v) ? (v.kind == UV_SYM ? ((const USym *)v.v.p)->bytes
                                            : ((const UStr *)v.v.p)->bytes) : ""; }
static inline size_t urbi_str_size(UValue v)
{ return urbi_is_str(v) ? (v.kind == UV_SYM ? ((const USym *)v.v.p)->len
                                            : ((const UStr *)v.v.p)->len) : 0; }

/* A GC-cell string (the ordinary result kind). nil on OOM. */
UValue urbi_make_str(UVM *vm, const char *s, size_t n);
/* An interned symbol — for values that become slot keys.  nil on OOM. */
UValue urbi_make_str_interned(UVM *vm, const char *s, size_t n);
/* Legacy spelling kept because ~15 bodies call it; `oom` is set to 1 on
 * failure when non-NULL. */
static inline UValue urbi_val_str_intern(UVM *vm, const char *s, size_t n, int *oom)
{
    UValue v = urbi_make_str_interned(vm, s, n);
    if (v.kind == UV_NIL && oom) *oom = 1;
    return v;
}
/* The interned symbol behind a string value, for slot lookup. NULL on a
 * non-string or on OOM. */
USym *urbi_str_to_sym(UVM *vm, UValue v);

/* === closures ========================================================= */

static inline bool urbi_is_closure(UValue v)
{ return v.kind == UV_CELL && ((const UCell *)v.v.p)->type == UCELL_CLOSURE; }
static inline UValue urbi_make_closure_value(UClosure *cl) { return uv_ptr(UV_CELL, cl); }

/* Call a closure on the current strand.  URBI/UEXEC_OK or UEXEC_THROW. */
int urbi_call_closure(UVM *vm, UValue closure, UValue recv,
                      const UValue *argv, uint8_t argc, UValue *out);

/* === objects ========================================================== */

/* All four keep their old names and old return conventions (0 ok, -1
 * OOM / not found) so the ported bodies read unchanged. */
UObject *urbi_object_new(UVM *vm, UObject *proto);
UObject *urbi_object_clone(UVM *vm, UObject *src);      /* fresh object whose proto is `src` */
int      urbi_object_set_local_slot(UVM *vm, UObject *o, USym *name, UValue v);
int      urbi_object_remove_slot(UVM *vm, UObject *o, const USym *name);
/* 1 found, 0 missing.  `holder`/`index` name the owning object and slot. */
int      urbi_object_resolve_slot(UVM *vm, UObject *o, const USym *name,
                                  UObject **holder, uint32_t *index);
/* -1 when absent; the index into the object's own slots otherwise. */
int      urbi_object_find_local(const UObject *o, const USym *name);
uint16_t urbi_object_proto_count(const UObject *o);
UObject *urbi_object_proto_at(const UObject *o, uint16_t i);
int      urbi_object_add_proto(UVM *vm, UObject *o, UObject *p);
int      urbi_object_remove_proto(UVM *vm, UObject *o, const UObject *p);
int      urbi_object_set_protos(UVM *vm, UObject *o, UObject **ps, uint16_t n);
/* Installs a getter/setter pair on one slot. 0 ok, -1 OOM. */
int      urbi_object_install_property(UVM *vm, UObject *o, USym *name,
                                      UValue getter, UValue setter, UValue value);

/* The Object root, and the prototype any value dispatches slots on. */
static inline UObject *urbi_object_root(UVM *vm) { return vm->protos[UP_OBJECT]; }
static inline UObject *urbi_atom_proto_for_value(UVM *vm, UValue v)
{ return uv_dispatch_proto(vm, v); }
static inline UObject *urbi_builtin_proto(UVM *vm, int which)
{ return (which >= 0 && which < UP_COUNT) ? vm->protos[which] : NULL; }

/* === lists and dictionaries =========================================== */

UValue   urbi_list_new(UVM *vm);
int      urbi_list_append(UVM *vm, UValue list, UValue v);   /* 0 ok, -1 OOM */
uint32_t urbi_list_len(UValue list);
UValue   urbi_list_get(UValue list, uint32_t i);
UValue   urbi_dict_new(UVM *vm);

/* === realm globals ==================================================== */

/* Bind a name in the shared root globals (what every realm inherits).
 * The old per-realm spelling is kept: at boot there is exactly one place
 * a built-in can go, and it is the root. */
int urbi_realm_set_global(UVM *vm, URealm *realm, const char *name, UValue v);
int urbi_realm_get_global(UVM *vm, URealm *realm, const char *name, UValue *out);

/* === output =========================================================== */

/* One message on one channel, through the current realm's writer. */
void urbi_stdlib_write(UVM *vm, const char *chan, size_t cl, const char *msg, size_t ml);

#endif
