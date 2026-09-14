/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/ustdlib_glue.c — see rt/ustdlib_glue.h. */

#include "rt/ustdlib_glue.h"

/* --- the current strand ------------------------------------------------ */

UStrand *uvm_current_strand(UVM *vm) { return vm ? vm->sched.current : NULL; }

URealm *uvm_current_realm(UVM *vm)
{
    if (!vm) return NULL;
    UStrand *s = vm->sched.current;
    if (s && s->realm) return s->realm;
    return vm->main_realm;
}

/* --- raising ----------------------------------------------------------- */

/* Every raise funnels here.  Before the boot table has run, protos[which]
 * is NULL and uexec_throw builds a protoless exception object that still
 * carries name and message — enough for vm->last_error to be truthful.
 *
 * `positioned` prefixes the source location of the call site (the top
 * frame is the bytecode frame that called the native, and its pc still
 * points just past the OP_CALL).  Only division/modulo by zero uses it,
 * so a native `%` raise reads the same as the dispatch loop's own `/`. */
static int glue_raise(UVM *vm, int which, UValue *out, const char *msg, int positioned)
{
    if (out) *out = uv_nil();
    UStrand *s = uvm_current_strand(vm);
    if (!s) {
        /* Outside dispatch there is nowhere to deposit the exception.
         * Record the text so urbi_last_error is still informative. */
        if (vm) vm->last_error_code = URBI_ERR_UNCAUGHT_THROW;
        return UEXEC_THROW;
    }
    int rc = positioned ? uexec_throw_here(vm, s, which, msg)
                        : uexec_throw(vm, s, which, msg);
    if (out) *out = s->transfer;
    return rc;
}

int urbi_raise_typed(UVM *vm, int which, UValue *out, const char *msg)
{ return glue_raise(vm, which, out, msg, 0); }

int urbi_raise_type(UVM *vm, const char *msg, UValue *out)
{ return urbi_raise_typed(vm, UP_TYPEERROR, out, msg ? msg : "type error"); }

int urbi_raise_oom(UVM *vm, UValue *out)
{ return urbi_raise_typed(vm, UP_OOMERROR, out, "out of memory"); }

int urbi_raise_index(UVM *vm, const char *msg, UValue *out)
{ return urbi_raise_typed(vm, UP_INDEXERROR, out, msg ? msg : "index out of range"); }

int urbi_raise_range(UVM *vm, const char *msg, UValue *out)
{ return urbi_raise_typed(vm, UP_RANGEERROR, out, msg ? msg : "value out of range"); }

int urbi_raise_divzero(UVM *vm, const char *msg, UValue *out)
{ return glue_raise(vm, UP_DIVBYZERO, out, msg ? msg : "division by 0", 1); }

int urbi_raise_lookup(UVM *vm, const USym *name, UValue *out)
{
    char buf[160];
    size_t at = 0;
    const char *p = "slot '";
    while (*p && at + 1 < sizeof buf) buf[at++] = *p++;
    if (name) { p = name->bytes; while (*p && at + 1 < sizeof buf) buf[at++] = *p++; }
    p = "' not found";
    while (*p && at + 1 < sizeof buf) buf[at++] = *p++;
    buf[at] = '\0';
    return urbi_raise_typed(vm, UP_LOOKUPERROR, out, buf);
}

/* Decimal, no <stdio.h>. */
static size_t glue_put_u32(char *buf, size_t cap, size_t at, uint32_t n)
{
    char tmp[12];
    size_t k = 0;
    do { tmp[k++] = (char)('0' + (n % 10u)); n /= 10u; } while (n);
    while (k && at + 1 < cap) buf[at++] = tmp[--k];
    buf[at] = '\0';
    return at;
}

int urbi_raise_arity(UVM *vm, const char *fn, uint8_t want, uint8_t got, UValue *out)
{
    char buf[160];
    size_t at = 0;
    const char *p = fn ? fn : "<native>";
    while (*p && at + 1 < sizeof buf) buf[at++] = *p++;
    p = " expected ";
    while (*p && at + 1 < sizeof buf) buf[at++] = *p++;
    at = glue_put_u32(buf, sizeof buf, at, want);
    p = " args, got ";
    while (*p && at + 1 < sizeof buf) buf[at++] = *p++;
    at = glue_put_u32(buf, sizeof buf, at, got);
    buf[at] = '\0';
    return urbi_raise_typed(vm, UP_ARITYERROR, out, buf);
}

/* --- strings ----------------------------------------------------------- */

UValue urbi_make_str(UVM *vm, const char *s, size_t n)
{
    UStr *r = ustr_new(vm, s ? s : "", s ? n : 0);
    return r ? uv_str(r) : uv_nil();
}

UValue urbi_make_str_interned(UVM *vm, const char *s, size_t n)
{
    USym *sym = usym_intern(vm, s ? s : "", s ? n : 0);
    return sym ? uv_sym(sym) : uv_nil();
}

USym *urbi_str_to_sym(UVM *vm, UValue v)
{
    if (!urbi_is_str(v)) return NULL;
    return uv_to_sym(vm, v);
}

/* --- closures ---------------------------------------------------------- */

int urbi_call_closure(UVM *vm, UValue closure, UValue recv,
                      const UValue *argv, uint8_t argc, UValue *out)
{
    UStrand *s = uvm_current_strand(vm);
    if (!urbi_is_closure(closure)) return urbi_raise_type(vm, "call: not a closure", out);
    if (!s) return urbi_raise_type(vm, "call: no strand in dispatch", out);
    return uexec_call(vm, s, (UClosure *)closure.v.p, recv, argv, argc, out);
}

/* --- objects ----------------------------------------------------------- */

UObject *urbi_object_new(UVM *vm, UObject *proto) { return uobj_new(vm, proto); }

/* A clone is a fresh, empty object whose prototype is the original: the
 * new core has no copy-on-write slot vector, so "clone" means "inherit"
 * and the corpus's parent/child independence falls out of slot shadowing
 * rather than out of copying. */
UObject *urbi_object_clone(UVM *vm, UObject *src) { return uobj_new(vm, src); }

int urbi_object_set_local_slot(UVM *vm, UObject *o, USym *name, UValue v)
{ return (o && name && uobj_set_local(vm, o, name, v, 0) >= 0) ? 0 : -1; }

int urbi_object_remove_slot(UVM *vm, UObject *o, const USym *name)
{ return (o && name && uobj_remove_local(vm, o, name)) ? 0 : -1; }

int urbi_object_resolve_slot(UVM *vm, UObject *o, const USym *name,
                             UObject **holder, uint32_t *index)
{
    UObjSlotRef ref;
    if (!o || !name || !uobj_resolve(vm, o, name, &ref)) return 0;
    if (holder) *holder = ref.owner;
    if (index) *index = (uint32_t)ref.index;
    return 1;
}

int urbi_object_find_local(const UObject *o, const USym *name)
{ return (o && name) ? uobj_find_local(o, name) : -1; }

uint16_t urbi_object_proto_count(const UObject *o) { return o ? o->nprotos : 0; }

UObject *urbi_object_proto_at(const UObject *o, uint16_t i)
{
    if (!o || i >= o->nprotos) return NULL;
    return o->protos ? o->protos[i] : o->proto0;
}

int urbi_object_add_proto(UVM *vm, UObject *o, UObject *p)
{ return (o && p) ? uobj_add_proto(vm, o, p) : -1; }

int urbi_object_remove_proto(UVM *vm, UObject *o, const UObject *p)
{ return (o && p) ? uobj_remove_proto(vm, o, p) : -1; }

int urbi_object_set_protos(UVM *vm, UObject *o, UObject **ps, uint16_t n)
{ return o ? uobj_set_protos(vm, o, ps, n) : -1; }

int urbi_object_install_property(UVM *vm, UObject *o, USym *name,
                                 UValue getter, UValue setter, UValue value)
{
    if (!o || !name) return -1;
    uint8_t attrs = 0;
    if (urbi_is_closure(getter)) attrs |= USLOT_GETTER;
    if (urbi_is_closure(setter)) attrs |= USLOT_SETTER;
    if (attrs == 0) return uobj_set_local(vm, o, name, value, 0) >= 0 ? 0 : -1;
    int idx = uobj_set_local(vm, o, name, value, attrs);
    if (idx < 0) return -1;
    UProps *pr = (UProps *)o->values[idx].v.p;
    pr->getter = getter;
    pr->setter = setter;
    return 0;
}

/* --- lists and dictionaries -------------------------------------------- */

UValue urbi_list_new(UVM *vm)
{
    UList *l = ulist_new(vm, vm->protos[UP_LIST], 0);
    return l ? uv_list(l) : uv_nil();
}

int urbi_list_append(UVM *vm, UValue list, UValue v)
{
    if (!uv_is_list(list)) return -1;
    return ulist_push(vm, (UList *)list.v.p, v);
}

uint32_t urbi_list_len(UValue list)
{ return uv_is_list(list) ? ((const UList *)list.v.p)->len : 0; }

UValue urbi_list_get(UValue list, uint32_t i)
{
    if (!uv_is_list(list)) return uv_nil();
    const UList *l = (const UList *)list.v.p;
    return i < l->len ? l->items[i] : uv_nil();
}

UValue urbi_dict_new(UVM *vm)
{
    UDict *d = udict_new(vm, vm->protos[UP_DICT]);
    return d ? uv_dict(d) : uv_nil();
}

/* --- realm globals ------------------------------------------------------ */

/* At boot the only sensible destination is the shared root object, which
 * every realm's globals inherits from; a realm argument is accepted (and
 * honoured when it names a real realm) so a host can still bind something
 * realm-local. */
int urbi_realm_set_global(UVM *vm, URealm *realm, const char *name, UValue v)
{
    UObject *dst = realm && realm->globals ? realm->globals : vm->root_globals;
    USym *sym = usym_cstr(vm, name);
    if (!dst || !sym) return URBI_ERR_OOM;
    return uobj_set_local(vm, dst, sym, v, USLOT_CONSTANT) < 0 ? URBI_ERR_OOM : URBI_OK;
}

int urbi_realm_get_global(UVM *vm, URealm *realm, const char *name, UValue *out)
{
    if (out) *out = uv_nil();
    UObject *src = realm && realm->globals ? realm->globals : vm->root_globals;
    const USym *sym = usym_cstr(vm, name);
    UObjSlotRef ref;
    if (!src || !sym || !uobj_resolve(vm, src, sym, &ref)) return URBI_ERR_INVALID_ARG;
    if (out) *out = uobj_slot_value(&ref);
    return URBI_OK;
}

/* --- output -------------------------------------------------------------- */

void urbi_stdlib_write(UVM *vm, const char *chan, size_t cl, const char *msg, size_t ml)
{ urealm_write(vm, uvm_current_realm(vm), chan, cl, msg, ml); }
