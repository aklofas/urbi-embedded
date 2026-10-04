/* SPDX-License-Identifier: BSD-3-Clause */
/* object_root.c — the Object root's native methods.
 *
 * Everything in urbiscript ends up here: every prototype chain
 * terminates at Object, so these are the methods that answer for any
 * value the language can produce, atoms included.  They are the v1.0
 * surface for slot manipulation, cloning, prototype-graph mutation and
 * reflection.
 *
 * CLONING.  `clone` returns a fresh, EMPTY object whose prototype is the
 * original.  The new core has no copy-on-write slot vector, so the
 * parent/child independence the corpus pins falls out of slot shadowing:
 * a write on the clone adds a local slot that hides the prototype's,
 * and the prototype is untouched.  An atom receiver short-circuits to
 * itself with no allocation at all, which is why `1.clone()` is 1.
 *
 * ERRORS.  Every failure path returns one of the urbi_raise_* helpers,
 * which deposit a typed exception on the current strand and return
 * UEXEC_THROW.  Argument COUNTS are not checked here — the boot table
 * declares min/max args and uexec rejects a bad call before the body
 * runs.  Argument KINDS still are. */

#include "rt/ustdlib_glue.h"
#include "stdlib/object_root.h"

/* Cap for the stack array insertFront builds.  Sized one past the proto
 * cap so the worst case (a full chain plus one prepend) reaches
 * uobj_set_protos and is rejected there rather than overflowing here. */
#define OBJ_PROTOS_CAP 65

/* --- shared helpers ---------------------------------------------------- */

/* The receiver as an object, or NULL when it is an atom.  Methods that
 * MUTATE need this; methods that only read route through
 * urbi_atom_proto_for_value instead so `1.hasSlot("clone")` works. */
static UObject *self_object(UValue self)
{ return self.kind == UV_OBJ ? (UObject *)self.v.p : NULL; }

/* The interned name an argument denotes, or NULL when it is not a
 * string.  Both string representations are accepted: a literal arrives
 * as a USym, a concatenation as a UStr. */
static USym *arg_name(UVM *vm, UValue v) { return urbi_str_to_sym(vm, v); }

/* --- Object.setSlot(name, value) --------------------------------------- */

static int obj_setSlot(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    UObject *recv = self_object(self);
    if (!recv) return urbi_raise_type(vm, "setSlot: self must be an Object", out);
    USym *name = arg_name(vm, args[0]);
    if (!name) return urbi_raise_type(vm, "setSlot: name must be a String", out);
    /* The same notification seam a bytecode slot write and a host
     * urbi_slot_set go through: watchers on this object re-evaluate, and
     * `x.changed?` fires on an update (or is armed by a declaration). */
    bool existed = uobj_find_local(recv, name) >= 0;
    if (uobj_set_local(vm, recv, name, args[1], 0) < 0) return urbi_raise_oom(vm, out);
    uexec_note_write(vm, recv, name, args[1], existed);
    *out = args[1];
    return UEXEC_OK;
}

/* --- Object.getSlot(name) ---------------------------------------------- */

static int obj_getSlot(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    const USym *name = arg_name(vm, args[0]);
    if (!name) return urbi_raise_type(vm, "getSlot: name must be a String", out);
    UObject *recv = urbi_atom_proto_for_value(vm, self);
    UObjSlotRef ref;
    if (!recv || !uobj_resolve(vm, recv, name, &ref)) return urbi_raise_lookup(vm, name, out);
    *out = uobj_slot_value(&ref);
    return UEXEC_OK;
}

/* Legacy alias.  The 2014 inheritance.chk fixture spells it
 * getSlotValue; the v1.0 split between "the slot" and "its value" does
 * not exist because a slot IS a UValue. */
static int obj_getSlotValue(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{ return obj_getSlot(vm, self, args, nargs, out); }

/* --- Object.hasSlot(name) ---------------------------------------------- */

static int obj_hasSlot(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    const USym *name = arg_name(vm, args[0]);
    if (!name) return urbi_raise_type(vm, "hasSlot: name must be a String", out);
    UObject *recv = urbi_atom_proto_for_value(vm, self);
    UObjSlotRef ref;
    *out = urbi_make_bool(recv != NULL && uobj_resolve(vm, recv, name, &ref));
    return UEXEC_OK;
}

/* --- Object.removeSlot(name) ------------------------------------------- */

static int obj_removeSlot(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    UObject *recv = self_object(self);
    if (!recv) return urbi_raise_type(vm, "removeSlot: self must be an Object", out);
    const USym *name = arg_name(vm, args[0]);
    if (!name) return urbi_raise_type(vm, "removeSlot: name must be a String", out);
    /* Idempotent, as the legacy semantics are: removing an absent slot is
     * a no-op, not an error.  A removal that did happen on a watched
     * object re-evaluates its watchers; there is no `changed?` to fire
     * for a slot that no longer exists. */
    if (uobj_remove_local(vm, recv, name) && (recv->cell.flags & UOBJ_F_WATCHED))
        uwatch_mark_dirty(vm, recv);
    *out = self;
    return UEXEC_OK;
}

/* Legacy alias: uobj_remove_local only ever touches the receiver's own
 * slots, so "remove" and "remove local" are the same operation. */
static int obj_removeLocalSlot(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{ return obj_removeSlot(vm, self, args, nargs, out); }

/* --- Object.clone() / Object.new() -------------------------------------- */

static int obj_clone(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args; (void)nargs;
    if (self.kind != UV_OBJ) { *out = self; return UEXEC_OK; }   /* atoms clone to themselves */
    UObject *c = uobj_new(vm, (UObject *)self.v.p);
    if (!c) return urbi_raise_oom(vm, out);
    *out = uv_obj(c);
    return UEXEC_OK;
}

/* `Foo.new()` is the class idiom.  It is clone today; when the language
 * grows init hooks this is where the post-clone dispatch goes.  Extra
 * arguments are accepted and ignored so a class that declares its own
 * `new` can override without an arity mismatch at the call site. */
static int obj_new(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{ return obj_clone(vm, self, args, nargs, out); }

/* --- the prototype graph ------------------------------------------------ */

static int obj_addProto(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    UObject *recv = self_object(self);
    if (!recv) return urbi_raise_type(vm, "addProto: self must be an Object", out);
    if (args[0].kind != UV_OBJ)
        return urbi_raise_type(vm, "addProto: argument must be an Object", out);
    if (uobj_add_proto(vm, recv, (UObject *)args[0].v.p) != 0) return urbi_raise_oom(vm, out);
    *out = self;
    return UEXEC_OK;
}

static int obj_removeProto(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    UObject *recv = self_object(self);
    if (!recv) return urbi_raise_type(vm, "removeProto: self must be an Object", out);
    if (args[0].kind != UV_OBJ)
        return urbi_raise_type(vm, "removeProto: argument must be an Object", out);
    (void)uobj_remove_proto(vm, recv, (UObject *)args[0].v.p);   /* absent = no-op */
    *out = self;
    return UEXEC_OK;
}

static int obj_setProtos(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    UObject *recv = self_object(self);
    if (!recv) return urbi_raise_type(vm, "setProtos: self must be an Object", out);
    if (args[0].kind == UV_OBJ) {
        UObject *single = (UObject *)args[0].v.p;
        if (uobj_set_protos(vm, recv, &single, 1) != 0) return urbi_raise_oom(vm, out);
        *out = self;
        return UEXEC_OK;
    }
    if (uv_is_list(args[0])) {
        const UList *l = (const UList *)args[0].v.p;
        if (l->len > OBJ_PROTOS_CAP)
            return urbi_raise_range(vm, "setProtos: too many prototypes", out);
        UObject *ps[OBJ_PROTOS_CAP];
        for (uint32_t i = 0; i < l->len; i++) {
            if (l->items[i].kind != UV_OBJ)
                return urbi_raise_type(vm, "setProtos: every element must be an Object", out);
            ps[i] = (UObject *)l->items[i].v.p;
        }
        if (uobj_set_protos(vm, recv, ps, (uint16_t)l->len) != 0) return urbi_raise_oom(vm, out);
        *out = self;
        return UEXEC_OK;
    }
    return urbi_raise_type(vm, "setProtos: argument must be an Object or a List", out);
}

/* `C.protos().insertFront(A)` — the legacy idiom shared-protos.chk
 * pins.  `self` is the synthetic list obj_protos handed back; the
 * receiver whose chain is really being edited hangs off its `_owner`
 * slot. */
static int obj_protos_insertFront(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    UObject *list = self_object(self);
    if (!list) return urbi_raise_type(vm, "insertFront: self must be the protos list", out);
    if (args[0].kind != UV_OBJ)
        return urbi_raise_type(vm, "insertFront: argument must be an Object", out);

    const USym *sym_owner = usym_cstr(vm, "_owner");
    UObjSlotRef ref;
    if (!sym_owner || !uobj_resolve(vm, list, sym_owner, &ref))
        return urbi_raise_type(vm, "insertFront: protos list is missing _owner", out);
    UValue owner_v = uobj_slot_value(&ref);
    if (owner_v.kind != UV_OBJ)
        return urbi_raise_type(vm, "insertFront: protos _owner is not an Object", out);

    UObject *owner = (UObject *)owner_v.v.p;
    uint16_t old_n = owner->nprotos;
    if ((uint32_t)old_n + 1u > OBJ_PROTOS_CAP)
        return urbi_raise_range(vm, "insertFront: prototype list cap exceeded", out);

    UObject *combined[OBJ_PROTOS_CAP];
    combined[0] = (UObject *)args[0].v.p;
    for (uint16_t i = 0; i < old_n; i++) combined[i + 1] = urbi_object_proto_at(owner, i);
    if (uobj_set_protos(vm, owner, combined, (uint16_t)(old_n + 1)) != 0)
        return urbi_raise_oom(vm, out);

    /* Keep the synthetic list's `size` honest for a caller that chains
     * off the same list. */
    USym *sym_size = usym_cstr(vm, "size");
    if (sym_size) (void)uobj_set_local(vm, list, sym_size, uv_int(owner->nprotos), 0);
    *out = self;
    return UEXEC_OK;
}

/* A synthetic object standing in for the prototype list: it reports
 * `size`, remembers its `_owner`, and carries the one method the corpus
 * calls on it.  A real List would have to carry the owner some other
 * way to keep insertFront able to mutate the original chain. */
static UObject *proto_list_create(UVM *vm, UObject *recv)
{
    USym *sym_size  = usym_cstr(vm, "size");
    USym *sym_owner = usym_cstr(vm, "_owner");
    USym *sym_if    = usym_cstr(vm, "insertFront");
    if (!sym_size || !sym_owner || !sym_if) return NULL;

    UObject *list = uobj_new(vm, vm->protos[UP_OBJECT]);
    if (!list) return NULL;
    UValue lv = uv_obj(list);
    URBI_ROOT(vm, lv);

    UClosure *cl = uclosure_native(vm, obj_protos_insertFront, 1, 1);
    int ok = cl != NULL
          && uobj_set_local(vm, list, sym_size, uv_int(recv->nprotos), 0) >= 0
          && uobj_set_local(vm, list, sym_owner, uv_obj(recv), 0) >= 0
          && uobj_set_local(vm, list, sym_if, urbi_make_closure_value(cl), 0) >= 0;
    URBI_UNROOT(vm, lv);
    return ok ? list : NULL;
}

static int obj_protos(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args; (void)nargs;
    UObject *recv = self_object(self);
    if (!recv) return urbi_raise_type(vm, "protos: self must be an Object", out);
    UObject *list = proto_list_create(vm, recv);
    if (!list) return urbi_raise_oom(vm, out);
    *out = uv_obj(list);
    return UEXEC_OK;
}

/* --- slot properties (the get/set parse sugar) -------------------------- */

/* Maps a property name to the slot-attribute bit it installs.  0 for an
 * unknown name, which the caller turns into a TypeError. */
static uint8_t property_bit(UVM *vm, UValue v)
{
    const char *p = urbi_str_cstr(v);
    if (strcmp(p, "oget") == 0) return USLOT_GETTER;
    if (strcmp(p, "oset") == 0) return USLOT_SETTER;
    if (strcmp(p, "constant") == 0) return USLOT_CONSTANT;
    (void)vm;
    return 0;
}

static int obj_setProperty(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    UObject *recv = self_object(self);
    if (!recv) return urbi_raise_type(vm, "setProperty: self must be an Object", out);
    USym *name = arg_name(vm, args[0]);
    if (!name || !urbi_is_str(args[1]))
        return urbi_raise_type(vm, "setProperty: name and prop name must be Strings", out);
    uint8_t bit = property_bit(vm, args[1]);
    if (bit == 0)
        return urbi_raise_type(vm,
            "setProperty: prop name must be one of \"oget\", \"oset\", \"constant\"", out);

    if (bit & (USLOT_GETTER | USLOT_SETTER)) {
        /* Validate arity at INSTALL time.  A getter is dispatched with no
         * arguments and a setter with exactly one; a body expecting a
         * different count would read registers the dispatcher never
         * filled, so this is the last point at which the mistake is
         * cheap to report. */
        if (!urbi_is_closure(args[2]))
            return urbi_raise_type(vm, "setProperty: oget/oset requires a function value", out);
        const UClosure *cl = (const UClosure *)args[2].v.p;
        uint8_t expected = (bit == USLOT_GETTER) ? 0u : 1u;
        uint8_t got = cl->proto ? cl->proto->nparams : cl->min_args;
        if (got != expected)
            return urbi_raise_arity(vm, bit == USLOT_GETTER ? "get" : "set", expected, got, out);
    }

    /* Accumulate onto whatever the slot already carries: `get x` followed
     * by `set x` must leave both installed, and the slot's own value must
     * survive both. */
    int idx = uobj_find_local(recv, name);
    uint8_t attrs = idx >= 0 ? recv->attrs[idx] : 0;
    UValue held = uv_nil(), getter = uv_nil(), setter = uv_nil();
    if (idx >= 0) {
        if (attrs & (USLOT_GETTER | USLOT_SETTER)) {
            const UProps *pr = (const UProps *)recv->values[idx].v.p;
            held = pr->value; getter = pr->getter; setter = pr->setter;
        } else {
            held = recv->values[idx];
        }
    }
    if (bit == USLOT_GETTER) getter = args[2];
    else if (bit == USLOT_SETTER) setter = args[2];
    else held = args[2];

    attrs |= bit;
    int slot = uobj_set_local(vm, recv, name, held, attrs);
    if (slot < 0) return urbi_raise_oom(vm, out);
    if (attrs & (USLOT_GETTER | USLOT_SETTER)) {
        UProps *pr = (UProps *)recv->values[slot].v.p;
        pr->getter = getter;
        pr->setter = setter;
        pr->value = held;
    }
    *out = self;
    return UEXEC_OK;
}

static int obj_getProperty(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    const UObject *recv = self_object(self);
    if (!recv) return urbi_raise_type(vm, "getProperty: self must be an Object", out);
    const USym *name = arg_name(vm, args[0]);
    if (!name || !urbi_is_str(args[1]))
        return urbi_raise_type(vm, "getProperty: name and prop must be Strings", out);
    uint8_t bit = property_bit(vm, args[1]);
    int idx = uobj_find_local(recv, name);
    uint8_t attrs = idx >= 0 ? recv->attrs[idx] : 0;

    if (bit == USLOT_CONSTANT) { *out = urbi_make_bool((attrs & USLOT_CONSTANT) != 0); return UEXEC_OK; }
    if (bit && (attrs & bit)) {
        const UProps *pr = (const UProps *)recv->values[idx].v.p;
        *out = (bit == USLOT_GETTER) ? pr->getter : pr->setter;
        return UEXEC_OK;
    }
    *out = uv_nil();
    return UEXEC_OK;
}

static int obj_properties(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    UObject *recv = self_object(self);
    if (!recv) return urbi_raise_type(vm, "properties: self must be an Object", out);
    USym *name = arg_name(vm, args[0]);
    if (!name) return urbi_raise_type(vm, "properties: name must be a String", out);

    UValue lst = urbi_list_new(vm);
    if (lst.kind == UV_NIL) return urbi_raise_oom(vm, out);
    int idx = uobj_find_local(recv, name);
    if (idx < 0) { *out = lst; return UEXEC_OK; }

    URBI_ROOT(vm, lst);
    static const struct { uint8_t bit; const char *nm; } kProps[] = {
        { USLOT_GETTER, "oget" }, { USLOT_SETTER, "oset" }, { USLOT_CONSTANT, "constant" }
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof kProps / sizeof kProps[0]; i++) {
        if ((recv->attrs[idx] & kProps[i].bit) == 0) continue;
        UValue nm = urbi_make_str_interned(vm, kProps[i].nm, strlen(kProps[i].nm));
        if (nm.kind == UV_NIL || urbi_list_append(vm, lst, nm) != 0) { ok = 0; break; }
    }
    URBI_UNROOT(vm, lst);
    if (!ok) return urbi_raise_oom(vm, out);
    *out = lst;
    return UEXEC_OK;
}

/* --- reflection --------------------------------------------------------- */

/* Appends every one of `o`'s own slot names to `lst`.  Most recently
 * added first, which is the order the corpus pins (the slot vector is
 * append-ordered, so it is walked backwards). */
static int collect_local_slot_names(UVM *vm, const UObject *o, UValue lst)
{
    for (uint16_t i = o->count; i > 0; i--) {
        /* A leading \x01 marks a slot the runtime keeps on the object for
         * its own use -- today, the event behind `x.changed?`.  No
         * identifier can contain that byte, so the prefix is a namespace
         * script cannot reach and must not be shown. */
        if (o->names[i - 1]->len > 0 && o->names[i - 1]->bytes[0] == '\x01') continue;
        UValue nm = uv_sym(o->names[i - 1]);
        if (urbi_list_append(vm, lst, nm) != 0) return -1;
    }
    return 0;
}

static int obj_localSlotNames(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args; (void)nargs;
    UObject *recv = self_object(self);
    if (!recv) return urbi_raise_type(vm, "localSlotNames: self must be an Object", out);
    UValue lst = urbi_list_new(vm);
    if (lst.kind == UV_NIL) return urbi_raise_oom(vm, out);
    URBI_ROOT(vm, lst);
    int rc = collect_local_slot_names(vm, recv, lst);
    URBI_UNROOT(vm, lst);
    if (rc != 0) return urbi_raise_oom(vm, out);
    *out = lst;
    return UEXEC_OK;
}

/* Own names first, then each prototype's own names.  Names that shadow
 * across the chain appear once per owner; de-duplication is a v1.x
 * refinement, not something any fixture asks for. */
static int obj_slotNames(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args; (void)nargs;
    UObject *recv = self_object(self);
    if (!recv) return urbi_raise_type(vm, "slotNames: self must be an Object", out);
    UValue lst = urbi_list_new(vm);
    if (lst.kind == UV_NIL) return urbi_raise_oom(vm, out);
    URBI_ROOT(vm, lst);
    int rc = collect_local_slot_names(vm, recv, lst);
    for (uint16_t i = 0; rc == 0 && i < recv->nprotos; i++) {
        const UObject *p = urbi_object_proto_at(recv, i);
        if (p) rc = collect_local_slot_names(vm, p, lst);
    }
    URBI_UNROOT(vm, lst);
    if (rc != 0) return urbi_raise_oom(vm, out);
    *out = lst;
    return UEXEC_OK;
}

static int obj_hasLocalSlot(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    const USym *name = arg_name(vm, args[0]);
    if (!name) return urbi_raise_type(vm, "hasLocalSlot: name must be a String", out);
    const UObject *recv = self_object(self);
    *out = urbi_make_bool(recv != NULL && uobj_find_local(recv, name) >= 0);
    return UEXEC_OK;
}

/* --- Object.asString ----------------------------------------------------
 *
 * The universal fallback.  Integer, Float and String shadow it with their
 * own conversions, so anything that reaches here is an object (or an
 * atom kind with no conversion of its own) and renders as its identity.
 * Rendering goes through urbi_value_to_string, which is the same
 * formatter the REPL prints with, so the two never disagree. */
static int obj_asString(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args; (void)nargs;
    char buf[96];
    size_t n = urbi_value_to_string(vm, self, buf, sizeof buf);
    UValue v = urbi_make_str(vm, buf, n);
    if (v.kind == UV_NIL) return urbi_raise_oom(vm, out);
    *out = v;
    return UEXEC_OK;
}

/* --- the table ---------------------------------------------------------- */

const UMethodDef ustdlib_object_methods[USTDLIB_OBJECT_NMETHODS] = {
    { "setSlot",         obj_setSlot,         2, 2 },
    { "getSlot",         obj_getSlot,         1, 1 },
    { "getSlotValue",    obj_getSlotValue,    1, 1 },   /* legacy alias */
    { "hasSlot",         obj_hasSlot,         1, 1 },
    { "removeSlot",      obj_removeSlot,      1, 1 },
    { "removeLocalSlot", obj_removeLocalSlot, 1, 1 },   /* legacy alias */
    { "clone",           obj_clone,           0, 0 },
    { "new",             obj_new,             0, UMETHOD_VARARGS },
    { "addProto",        obj_addProto,        1, 1 },
    { "removeProto",     obj_removeProto,     1, 1 },
    { "protos",          obj_protos,          0, 0 },
    { "setProtos",       obj_setProtos,       1, 1 },
    { "setProperty",     obj_setProperty,     3, 3 },
    { "getProperty",     obj_getProperty,     2, 2 },
    { "properties",      obj_properties,      1, 1 },
    { "slotNames",       obj_slotNames,       0, 0 },
    { "localSlotNames",  obj_localSlotNames,  0, 0 },
    { "hasLocalSlot",    obj_hasLocalSlot,    1, 1 },
    { "asString",        obj_asString,        0, 0 }
};
