/* SPDX-License-Identifier: BSD-3-Clause */
/* runtime_types.c — the Exception prototype's own methods.
 *
 *   var e = Exception.new("boom")   a fresh clone carrying `message`
 *   e.message                       the bound message
 *   e.raise                         throw `e` itself, so an enclosing
 *                                   catch binds this very object
 *
 * The Exception FAMILY — Exception and the seven subclasses the VM
 * raises — is built by uboot_table, not here: the runtime has to be able
 * to throw a TypeError before any script has run, so the protos and
 * their parent links are C data rather than something an overlay
 * installs. */

#include "rt/ustdlib_glue.h"
#include "stdlib/runtime_types.h"

static int exc_new(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    if (self.kind != UV_OBJ)
        return urbi_raise_type(vm, "Exception.new: receiver must be an Object", out);

    UObject *e = uobj_new(vm, (UObject *)self.v.p);
    if (e == NULL) return urbi_raise_oom(vm, out);
    USym *sym_message = usym_cstr(vm, "message");
    if (sym_message == NULL || uobj_set_local(vm, e, sym_message, args[0], 0) < 0)
        return urbi_raise_oom(vm, out);
    *out = uv_obj(e);
    return UEXEC_OK;
}

/* Throws `self` rather than building a new exception, so the object the
 * script has been holding is exactly the one its catch clause binds. */
static int exc_raise(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args; (void)nargs;
    UStrand *s = uvm_current_strand(vm);
    if (s == NULL) return urbi_raise_type(vm, "Exception.raise: no active strand", out);
    *out = uv_nil();
    return uexec_throw_value(vm, s, self);
}

const UMethodDef k_exception_methods[K_EXCEPTION_NMETHODS] = {
    { "new",   exc_new,   1, 1 },
    { "raise", exc_raise, 0, 0 }
};

/* `Exception.message` without a clone reads as nil instead of failing to
 * resolve, which is what the corpus expects of the bare prototype. */
int urbi_exception_init(UVM *vm, UObject *proto)
{
    USym *sym = usym_cstr(vm, "message");
    if (!sym || uobj_set_local(vm, proto, sym, uv_nil(), 0) < 0) return URBI_ERR_OOM;
    return URBI_OK;
}
