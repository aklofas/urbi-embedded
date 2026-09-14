/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/uapi.c — the public C API, implemented on the runtime core.
 * See include/urbi/urbi.h for the contract of every function here. */

#include "rt/uexec.h"
#include "urbi/urbi.h"
#include "chunk/uchunk.h"
#include "emit/ufront.h"

#include <stdio.h>    /* the value formatter's float/int rendering */

/* ===================================================================
 * Lifecycle
 * =================================================================== */

UVM *urbi_open(UVMAllocFn alloc, void *ud, const UVMConfig *config)
{
    (void)config;   /* step_budget/boot_stdlib become live with the scheduler and boot tasks */
    UVM *vm = uvm_open((UAllocFn)alloc, ud);
    if (!vm) return NULL;
    if (urealm_new(vm) == NULL) { uvm_close(vm); return NULL; }
    return vm;
}

void urbi_close(UVM *vm) { uvm_close(vm); }

int urbi_step(UVM *vm, uint32_t budget, uint64_t *next_wake_us)
{
    (void)vm; (void)budget; (void)next_wake_us;
    return URBI_ERR_INVALID_STATE;
}

bool urbi_has_live_work(UVM *vm)
{
    return vm != NULL && vm->sched.run_head != NULL;
}

void urbi_set_clock(UVM *vm, uint64_t (*fn)(void *ud), void *ud)
{ if (vm) { vm->clock_us = fn; vm->clock_ud = ud; } }

void urbi_set_diag(UVM *vm, void (*fn)(UVM *, void *, int, const char *, size_t), void *ud)
{ if (vm) { vm->diag = fn; vm->diag_ud = ud; } }

void urbi_set_writer(UVM *vm, void (*fn)(void *, const char *, size_t, const char *, size_t), void *ud)
{ if (vm) { vm->writer = fn; vm->writer_ud = ud; } }

void urbi_set_wake(UVM *vm, void (*fn)(void *ud), void *ud)
{ if (vm) { vm->wake = fn; vm->wake_ud = ud; } }

/* ===================================================================
 * Realms
 * =================================================================== */

URealm *urbi_realm_new(UVM *vm) { return vm ? urealm_new(vm) : NULL; }

URealm *urbi_realm_main(UVM *vm) { return vm ? vm->main_realm : NULL; }

void urbi_realm_free(UVM *vm, URealm *realm)
{
    if (!vm || !realm || realm == vm->main_realm) return;
    for (URealm **pp = &vm->realms; *pp; pp = &(*pp)->next) {
        if (*pp == realm) { *pp = realm->next; realm->next = NULL; break; }
    }
    /* The realm cell and everything below it are reclaimed by the next
     * collection once nothing else refers to them. */
}

/* ===================================================================
 * Code
 * =================================================================== */

int urbi_compile(UVM *vm, const char *src, size_t n, const char *name,
                 uint8_t **out_bytes, size_t *out_len, char *err, size_t errcap)
{
    if (!vm || !src || !out_bytes || !out_len) return URBI_ERR_INVALID_ARG;
    *out_bytes = NULL;
    *out_len = 0;

    UProto *root = NULL;
    int rc = ufront_compile(vm, src, n, name, &root, err, errcap);
    if (rc != URBI_OK) return rc;

    ptrdiff_t need = ufront_serialize(root, NULL, 0);
    if (need < 0) {
        if (err && errcap) snprintf(err, errcap, "serialize size-query failed");
        uchunk_destroy(root, NULL);
        return URBI_ERR_COMPILE;
    }
    uint8_t *buf = (uint8_t *)vm->gc.alloc(NULL, (size_t)need, vm->gc.alloc_ud);
    if (!buf) { uchunk_destroy(root, NULL); return URBI_ERR_OOM; }
    ptrdiff_t wrote = ufront_serialize(root, buf, (size_t)need);
    uchunk_destroy(root, NULL);
    if (wrote != need) {
        vm->gc.alloc(buf, 0, vm->gc.alloc_ud);
        if (err && errcap) snprintf(err, errcap, "serialize produced an unexpected length");
        return URBI_ERR_COMPILE;
    }
    *out_bytes = buf;
    *out_len = (size_t)need;
    return URBI_OK;
}

void urbi_chunk_free(UVM *vm, uint8_t *bytes, size_t n)
{
    (void)n;
    if (vm && bytes) vm->gc.alloc(bytes, 0, vm->gc.alloc_ud);
}

/* The chunk allocator the loader uses, routed at the VM's allocator so a
 * loaded chunk's buffers come from the same place as a compiled one's. */
static void *uapi_chunk_alloc(void *ptr, size_t nbytes, void *ud)
{
    UVM *vm = (UVM *)ud;
    return vm->gc.alloc(ptr, nbytes, vm->gc.alloc_ud);
}

int urbi_load(UVM *vm, URealm *realm, const uint8_t *bytes, size_t n, UValue *out)
{
    if (out) *out = urbi_make_nil();
    if (!vm || !bytes) return URBI_ERR_INVALID_ARG;
    if (!realm) realm = vm->main_realm;
    if (!realm) return URBI_ERR_INVALID_ARG;

    UProto *root = NULL;
    UChunkLoadError lrc = uchunk_deserialize(&root, bytes, n, uapi_chunk_alloc, vm,
                                             vm->last_error, sizeof vm->last_error);
    if (lrc != UCHUNK_LOAD_OK) {
        vm->last_error_code = (lrc == UCHUNK_LOAD_UNSUPPORTED_VERSION
                            || lrc == UCHUNK_LOAD_FLAVOR_MISMATCH
                            || lrc == UCHUNK_LOAD_BAD_MAGIC)
                            ? URBI_ERR_BYTECODE_VERSION_MISMATCH
                            : (lrc == UCHUNK_LOAD_OOM ? URBI_ERR_OOM : URBI_ERR_COMPILE);
        return vm->last_error_code;
    }

    UStrand *s = uvm_spare_acquire(vm, realm);
    if (!s) { uchunk_destroy(root, NULL); return URBI_ERR_OOM; }
    UProtoCell *pc = uproto_bind(vm, root);
    if (!pc) { uvm_spare_release(vm, s); return URBI_ERR_OOM; }
    pc->cell.flags |= UCELL_F_PINNED;
    UClosure *cl = uclosure_new(vm, root, 0);
    pc->cell.flags &= (uint16_t)~UCELL_F_PINNED;
    if (!cl) { uvm_spare_release(vm, s); return URBI_ERR_OOM; }

    UValue res = urbi_make_nil();
    int crc = uexec_call(vm, s, cl, uv_obj(realm->globals), NULL, 0, &res);
    uvm_spare_release(vm, s);
    if (crc != UEXEC_OK) return URBI_ERR_UNCAUGHT_THROW;
    if (out) *out = res;
    return URBI_OK;
}

int urbi_run(UVM *vm, URealm *realm, const char *src, size_t n, const char *name,
             UValue *out, char *err, size_t errcap)
{
    if (!vm) return URBI_ERR_INVALID_ARG;
    if (!realm) realm = vm->main_realm;
    return uexec_run_source(vm, realm, src, n, name, out, err, errcap);
}

int urbi_call(UVM *vm, URealm *realm, UValue callee, UValue recv,
              const UValue *argv, uint8_t argc, UValue *out)
{
    if (out) *out = urbi_make_nil();
    if (!vm) return URBI_ERR_INVALID_ARG;
    if (!realm) realm = vm->main_realm;
    if (callee.kind != UV_CELL || ((UCell *)callee.v.p)->type != UCELL_CLOSURE)
        return URBI_ERR_INVALID_ARG;
    UStrand *s = uvm_spare_acquire(vm, realm);
    if (!s) return URBI_ERR_OOM;
    UValue res = urbi_make_nil();
    int rc = uexec_call(vm, s, (UClosure *)callee.v.p, recv, argv, argc, &res);
    uvm_spare_release(vm, s);
    if (rc != UEXEC_OK) return URBI_ERR_UNCAUGHT_THROW;
    if (out) *out = res;
    return URBI_OK;
}

/* ===================================================================
 * Values
 * =================================================================== */

UValue urbi_make_string(UVM *vm, const char *bytes, size_t n)
{
    if (!vm || !bytes) return urbi_make_nil();
    UStr *s = ustr_new(vm, bytes, n);
    return s ? uv_str(s) : urbi_make_nil();
}

void urbi_ref(UVM *vm, UValue v)
{
    (void)vm;
    if (v.kind == UV_STR || v.kind == UV_OBJ || v.kind == UV_CELL)
        ((UCell *)v.v.p)->flags |= UCELL_F_PINNED;
}

void urbi_unref(UVM *vm, UValue v)
{
    (void)vm;
    if (v.kind == UV_STR || v.kind == UV_OBJ || v.kind == UV_CELL)
        ((UCell *)v.v.p)->flags &= (uint16_t)~UCELL_F_PINNED;
}

/* Renders a value the way the REPL prints it.  The float rule is Lua's:
 * print with %.14g, then append ".0" when the result reads as an
 * integer, so 4/2 shows as 2.0 rather than 2.  The .chk corpus pins
 * this spelling. */
size_t urbi_value_to_string(UVM *vm, UValue v, char *buf, size_t cap)
{
    (void)vm;
    if (cap == 0) return 0;
    int n = 0;
    switch (v.kind) {
    case UV_NIL:  n = snprintf(buf, cap, "nil"); break;
    case UV_BOOL: n = snprintf(buf, cap, "%s", v.v.i ? "true" : "false"); break;
    case UV_INT:  n = snprintf(buf, cap, "%lld", (long long)v.v.i); break;
    case UV_FLOAT: {
        n = snprintf(buf, cap, "%.14g", v.v.f);
        if (n < 0 || (size_t)n >= cap) break;
        int needs_dot_zero = 1;
        for (int k = 0; k < n; k++) {
            char c = buf[k];
            if (c == '.' || c == 'e' || c == 'E' || c == 'n' || c == 'i') { needs_dot_zero = 0; break; }
        }
        if (needs_dot_zero && (size_t)n + 2U < cap) {
            buf[n++] = '.'; buf[n++] = '0'; buf[n] = '\0';
        }
        break;
    }
    case UV_SYM: case UV_STR: {
        uint32_t len;
        const char *s = uv_str_bytes(v, &len);
        size_t w = 0;
        if (w + 1 >= cap) { buf[0] = '\0'; return 0; }
        buf[w++] = '"';
        for (uint32_t k = 0; k < len; k++) {
            unsigned char c = (unsigned char)s[k];
            const char *esc = NULL;
            switch (c) {
            case '\\': esc = "\\\\"; break;
            case '"':  esc = "\\\""; break;
            case '\n': esc = "\\n"; break;
            case '\t': esc = "\\t"; break;
            case '\r': esc = "\\r"; break;
            default: break;
            }
            if (esc) {
                if (w + 3 >= cap) break;
                buf[w++] = esc[0]; buf[w++] = esc[1];
            } else if (c >= 0x20 && c < 0x7f) {
                if (w + 2 >= cap) break;
                buf[w++] = (char)c;
            } else {
                static const char hex[] = "0123456789abcdef";
                if (w + 5 >= cap) break;
                buf[w++] = '\\'; buf[w++] = 'x';
                buf[w++] = hex[(c >> 4) & 0xf]; buf[w++] = hex[c & 0xf];
            }
        }
        if (w + 1 >= cap) { buf[w] = '\0'; return w; }
        buf[w++] = '"';
        buf[w] = '\0';
        return w;
    }
    case UV_OBJ: n = snprintf(buf, cap, "<object %p>", v.v.p); break;
    default:
        /* Closures and void render as "<?>" — the spelling the corpus
         * has pinned since the first REPL fixtures. */
        n = snprintf(buf, cap, "<?>");
        break;
    }
    if (n < 0) { buf[0] = '\0'; return 0; }
    if ((size_t)n >= cap) return cap - 1;
    return (size_t)n;
}

/* ===================================================================
 * Globals and slots
 * =================================================================== */

int urbi_global_get(UVM *vm, URealm *realm, const char *name, UValue *out)
{
    if (out) *out = urbi_make_nil();
    if (!vm || !name) return URBI_ERR_INVALID_ARG;
    if (!realm) realm = vm->main_realm;
    if (!realm || !realm->globals) return URBI_ERR_INVALID_ARG;
    USym *sym = usym_cstr(vm, name);
    if (!sym) return URBI_ERR_OOM;
    UObjSlotRef ref;
    if (!uobj_resolve(vm, realm->globals, sym, &ref)) return URBI_ERR_INVALID_ARG;
    if (out) *out = uobj_slot_value(&ref);
    return URBI_OK;
}

int urbi_global_set(UVM *vm, URealm *realm, const char *name, UValue v)
{
    if (!vm || !name) return URBI_ERR_INVALID_ARG;
    if (!realm) realm = vm->main_realm;
    if (!realm || !realm->globals) return URBI_ERR_INVALID_ARG;
    USym *sym = usym_cstr(vm, name);
    if (!sym) return URBI_ERR_OOM;
    return uobj_set_local(vm, realm->globals, sym, v, 0) < 0 ? URBI_ERR_OOM : URBI_OK;
}

int urbi_slot_get(UVM *vm, UValue obj, const char *name, UValue *out)
{
    if (out) *out = urbi_make_nil();
    if (!vm || !name) return URBI_ERR_INVALID_ARG;
    UObject *o = uv_dispatch_proto(vm, obj);
    if (!o) return URBI_ERR_INVALID_ARG;
    USym *sym = usym_cstr(vm, name);
    if (!sym) return URBI_ERR_OOM;
    UObjSlotRef ref;
    if (!uobj_resolve(vm, o, sym, &ref)) return URBI_ERR_INVALID_ARG;
    if (out) *out = uobj_slot_value(&ref);
    return URBI_OK;
}

int urbi_slot_set(UVM *vm, UValue obj, const char *name, UValue v)
{
    if (!vm || !name) return URBI_ERR_INVALID_ARG;
    if (obj.kind != UV_OBJ) return URBI_ERR_INVALID_ARG;
    UObject *o = (UObject *)obj.v.p;
    if (o->cell.flags & UOBJ_F_READONLY) return URBI_ERR_INVALID_ARG;
    USym *sym = usym_cstr(vm, name);
    if (!sym) return URBI_ERR_OOM;
    return uobj_set_local(vm, o, sym, v, 0) < 0 ? URBI_ERR_OOM : URBI_OK;
}

/* ===================================================================
 * Host functions
 * =================================================================== */

int urbi_register(UVM *vm, const char *path, urbi_native_fn fn,
                  uint8_t min_args, uint8_t max_args)
{
    if (!vm || !path || !fn) return URBI_ERR_INVALID_ARG;
    URealm *realm = vm->main_realm;
    if (!realm || !realm->globals) return URBI_ERR_INVALID_STATE;

    UObject *owner = realm->globals;
    const char *seg = path;
    for (;;) {
        const char *dot = seg;
        while (*dot && *dot != '.') dot++;
        if (*dot == '\0') break;                    /* `seg` is the final slot name */
        USym *sym = usym_intern(vm, seg, (size_t)(dot - seg));
        if (!sym) return URBI_ERR_OOM;
        UObjSlotRef ref;
        if (!uobj_resolve(vm, owner, sym, &ref)) return URBI_ERR_INVALID_STATE;
        UValue v = uobj_slot_value(&ref);
        if (v.kind != UV_OBJ) return URBI_ERR_INVALID_STATE;
        owner = (UObject *)v.v.p;
        seg = dot + 1;
    }
    if (*seg == '\0') return URBI_ERR_INVALID_ARG;

    USym *name = usym_cstr(vm, seg);
    if (!name) return URBI_ERR_OOM;
    UValue ownerv = uv_obj(owner);
    urbi_ref(vm, ownerv);   /* uclosure_native may collect */
    UClosure *cl = uclosure_native(vm, fn, min_args, max_args);
    urbi_unref(vm, ownerv);
    if (!cl) return URBI_ERR_OOM;
    return uobj_set_local(vm, owner, name, uv_ptr(UV_CELL, cl), 0) < 0 ? URBI_ERR_OOM : URBI_OK;
}

int urbi_throw(UVM *vm, const char *proto, const char *msg)
{
    if (!vm) return URBI_ERR_INVALID_ARG;
    UStrand *s = vm->sched.current;
    if (!s) return URBI_ERR_INVALID_STATE;
    int which = UP_EXCEPTION;
    if (proto) {
        static const struct { const char *name; int idx; } known[] = {
            { "TypeError", UP_TYPEERROR }, { "ArityError", UP_ARITYERROR },
            { "LookupError", UP_LOOKUPERROR }, { "OutOfMemoryError", UP_OOMERROR },
            { "IndexError", UP_INDEXERROR }, { "RangeError", UP_RANGEERROR },
            { "DivisionByZero", UP_DIVBYZERO }, { "Exception", UP_EXCEPTION }
        };
        for (size_t k = 0; k < sizeof known / sizeof known[0]; k++) {
            if (strcmp(known[k].name, proto) == 0) { which = known[k].idx; break; }
        }
    }
    return uexec_throw(vm, s, which, msg);
}

/* ===================================================================
 * Events, watchers, tags — pending their own tasks
 * =================================================================== */

int urbi_event_new(UVM *vm, URealm *realm, const char *name, UValue *out)
{ (void)vm; (void)realm; (void)name; if (out) *out = urbi_make_nil(); return URBI_ERR_INVALID_STATE; }

int urbi_event_emit(UVM *vm, UValue event, UValue payload)
{ (void)vm; (void)event; (void)payload; return URBI_ERR_INVALID_STATE; }

int urbi_inject_event(UVM *vm, urbi_event_id_t id, const urbi_event_payload_t *payload, size_t n)
{ (void)vm; (void)id; (void)payload; (void)n; return URBI_ERR_INVALID_STATE; }

int urbi_watch(UVM *vm, URealm *realm, const char *expr,
               int (*cb)(UVM *, void *, UValue), void *ud)
{ (void)vm; (void)realm; (void)expr; (void)cb; (void)ud; return URBI_ERR_INVALID_STATE; }

int urbi_tag_new(UVM *vm, URealm *realm, const char *name, UValue *out)
{ (void)vm; (void)realm; (void)name; if (out) *out = urbi_make_nil(); return URBI_ERR_INVALID_STATE; }

int urbi_tag_stop(UVM *vm, UValue tag)     { (void)vm; (void)tag; return URBI_ERR_INVALID_STATE; }
int urbi_tag_block(UVM *vm, UValue tag)    { (void)vm; (void)tag; return URBI_ERR_INVALID_STATE; }
int urbi_tag_unblock(UVM *vm, UValue tag)  { (void)vm; (void)tag; return URBI_ERR_INVALID_STATE; }
int urbi_tag_freeze(UVM *vm, UValue tag)   { (void)vm; (void)tag; return URBI_ERR_INVALID_STATE; }
int urbi_tag_unfreeze(UVM *vm, UValue tag) { (void)vm; (void)tag; return URBI_ERR_INVALID_STATE; }

/* ===================================================================
 * Errors, GC, version
 * =================================================================== */

int urbi_last_error(UVM *vm, UErrorInfo *info)
{
    if (!vm) return URBI_ERR_INVALID_ARG;
    if (info) { info->code = vm->last_error_code; info->message = vm->last_error; }
    return vm->last_error_code;
}

void urbi_clear_error(UVM *vm)
{
    if (!vm) return;
    vm->last_error[0] = '\0';
    vm->last_error_code = URBI_OK;
}

void urbi_gc_collect(UVM *vm) { if (vm) ugc_collect(vm); }

int urbi_gc_stats(UVM *vm, UGcStats *out)
{
    if (!vm || !out) return URBI_ERR_INVALID_ARG;
    out->bytes_live  = vm->gc.bytes_live;
    out->bytes_since = vm->gc.bytes_since;
    out->cells_live  = vm->gc.cells_live;
    out->cycles      = vm->gc.cycles;
    return URBI_OK;
}

const char *urbi_version(void) { return URBI_API_VERSION_STRING; }

void urbi_api_version(int *out_major, int *out_minor, int *out_patch)
{
    if (out_major) *out_major = URBI_API_VERSION_MAJOR;
    if (out_minor) *out_minor = URBI_API_VERSION_MINOR;
    if (out_patch) *out_patch = URBI_API_VERSION_PATCH;
}
