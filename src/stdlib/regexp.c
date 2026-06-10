/* SPDX-License-Identifier: BSD-3-Clause */
/* regexp.c — RegExp atom-backed type + compact backtracking matcher.
 *
 * New type following the Mutex / Date precedent (src/stdlib/primitives.c):
 * a URBI_ATOM_OBJECT proto in vm->regexp_proto, GC-reachable via
 * object_roots_walker (uobject.c), native methods installed by a per-type
 * method table, hidden per-instance state on a `_pattern` slot.
 *
 * The matcher is a self-contained recursive backtracking engine.  It is
 * freestanding-clean: no <regex.h>, no <stdlib.h>, no <string.h>.
 * Supported syntax (v1.0):
 *
 *   literal char   matches itself
 *   .              matches any single char
 *   [abc] [^abc]   positive / negated char class
 *   [a-z]          range inside a class
 *   ^ $            start- / end-of-string anchors
 *   * + ?          greedy quantifier on the preceding atom
 *
 * No capture groups at v1.0 — RegExp.match is an alias of RegExp.test.
 * Captures are tracked as a v1.x follow-up. */

#include "stdlib/regexp.h"
#include "stdlib/object_root.h"        /* urbi_native_closure_create + raise helpers */

#include "chunk/uchunk.h"              /* UValue / UVAL_* */
#include "object/uobject.h"            /* urbi_object_alloc / clone / set_local_slot */
#include "object/ushape.h"             /* urbi_shape_find_slot */
#include "realm/urealm.h"              /* URealm */
#include "runtime/uclosure.h"          /* urbi_native_method_fn */
#include "runtime/umacros.h"           /* urbi_strlen */
#include "sched/ustrand.h"             /* UEXEC_OK */
#include "urbi/object.h"               /* URBI_ATOM_OBJECT */
#include "urbi/types.h"                /* urbi_make_nil */
#include "urbi/urbi.h"                 /* URBI_OK / URBI_ERR_* / urbi_realm_set_global */
#include "value/uintern.h"             /* ustr_intern + USymbol */
#include "vm/uvm.h"                    /* UVM */

#include <stdint.h>
#include <stddef.h>

/* === UValue construction helpers ========================================= */

static UValue
val_bool(int b)
{
    UValue v = urbi_make_nil();
    v.kind = (uint8_t)UVAL_BOOL;
    v.v.i  = b ? 1 : 0;
    return v;
}

static UValue
val_obj(UObject *o)
{
    UValue v = urbi_make_nil();
    v.kind = (uint8_t)UVAL_OBJECT;
    v.v.p  = o;
    return v;
}

/* === Compact backtracking matcher ========================================
 *
 * An "atom" is one match unit in the pattern: a char class `[..]`, a `.`,
 * or a literal char.  re_atom_len returns the atom's pattern length;
 * re_atom_matches tests one input char against it. */

/* Length (in pattern bytes) of the atom starting at re[0]. */
static size_t
re_atom_len(const char *re, size_t relen)
{
    if (relen == 0U) return 0U;
    if (re[0] == '[') {
        size_t i = 1U;
        if (i < relen && re[i] == '^') i++;
        /* A `]` immediately after `[` or `[^` is a literal member. */
        if (i < relen && re[i] == ']') i++;
        while (i < relen && re[i] != ']') i++;
        if (i < relen && re[i] == ']') i++;   /* consume closing `]` */
        return i;
    }
    return 1U;
}

/* Does the atom starting at re[0] (length alen) match input char ch? */
static int
re_atom_matches(const char *re, size_t alen, char ch)
{
    if (alen == 0U) return 0;
    if (re[0] == '[') {
        size_t i = 1U;
        int neg = 0, hit = 0;
        size_t end = (re[alen - 1U] == ']') ? alen - 1U : alen;
        if (i < end && re[i] == '^') { neg = 1; i++; }
        while (i < end) {
            if (i + 2U < end && re[i + 1U] == '-') {
                unsigned char lo = (unsigned char)re[i];
                unsigned char hi = (unsigned char)re[i + 2U];
                if ((unsigned char)ch >= lo && (unsigned char)ch <= hi) hit = 1;
                i += 3U;
            } else {
                if (re[i] == ch) hit = 1;
                i++;
            }
        }
        return neg ? !hit : hit;
    }
    if (re[0] == '.') return 1;
    return re[0] == ch;
}

/* Forward decl: match the pattern [re, re+relen) anchored at the start of
 * the input [s, s_end). */
static int re_match_here(const char *re, size_t relen,
                         const char *s, const char *s_end);

/* Greedy quantifier `*` / `+` over an atom: match `atom` (length alen)
 * zero/one-or-more times at s, then the rest of the pattern after the
 * quantifier.  `min` is 0 for `*`, 1 for `+`. */
static int
re_match_repeat(const char *atom, size_t alen,
                const char *rest, size_t restlen,
                const char *s, const char *s_end, int min)
{
    /* Consume as many matching chars as possible, recording positions, then
     * backtrack from longest to shortest (greedy). */
    const char *p = s;
    while (p < s_end && re_atom_matches(atom, alen, *p)) p++;
    /* p now points one past the last matching char. */
    while (p - s >= min) {
        if (re_match_here(rest, restlen, p, s_end)) return 1;
        if (p == s) break;
        p--;
    }
    return 0;
}

/* Match the pattern [re, re+relen) anchored at the start of [s, s_end). */
static int
re_match_here(const char *re, size_t relen, const char *s, const char *s_end)
{
    if (relen == 0U) return 1;                 /* empty pattern matches */

    if (re[0] == '$' && relen == 1U) {         /* end-of-string anchor */
        return s == s_end;
    }

    size_t alen = re_atom_len(re, relen);
    char quant = (relen > alen) ? re[alen] : '\0';

    if (quant == '*' || quant == '+') {
        const char *rest = re + alen + 1U;
        size_t restlen = relen - alen - 1U;
        return re_match_repeat(re, alen, rest, restlen, s, s_end,
                               (quant == '+') ? 1 : 0);
    }
    if (quant == '?') {
        const char *rest = re + alen + 1U;
        size_t restlen = relen - alen - 1U;
        /* Try matching the atom once, then skip it. */
        if (s < s_end && re_atom_matches(re, alen, *s) &&
            re_match_here(rest, restlen, s + 1U, s_end)) {
            return 1;
        }
        return re_match_here(rest, restlen, s, s_end);
    }

    /* Plain atom: must match one char, then the rest. */
    if (s < s_end && re_atom_matches(re, alen, *s)) {
        return re_match_here(re + alen, relen - alen, s + 1U, s_end);
    }
    return 0;
}

/* Public matcher entry: does [re, re+relen) match anywhere in [s, s_end)? */
int
urbi_regexp_search(const char *re, size_t relen, const char *s, const char *s_end)
{
    if (re == NULL || s == NULL || s_end == NULL) return 0;

    if (relen > 0U && re[0] == '^') {          /* start anchor: only at s */
        return re_match_here(re + 1U, relen - 1U, s, s_end);
    }
    /* Unanchored: try each starting position, including s_end (so an empty
     * or `$`-only pattern can match at the end). */
    const char *p = s;
    for (;;) {
        if (re_match_here(re, relen, p, s_end)) return 1;
        if (p == s_end) break;
        p++;
    }
    return 0;
}

/* === Hidden-slot helpers (mirror primitives.c) =========================== */

static int
write_local_slot(UVM *vm, UObject *o, const char *name, UValue value)
{
    USymbol *sym = (USymbol *)ustr_intern(vm, name, urbi_strlen(name));
    if (sym == NULL) return -1;
    if (urbi_object_set_local_slot(vm, o, sym, value) != 0) return -1;
    return 0;
}

static int
read_local_slot(UVM *vm, UObject *o, const char *name, UValue *out)
{
    const USymbol *sym = (const USymbol *)ustr_intern(vm, name, urbi_strlen(name));
    if (sym == NULL) return -1;
    int32_t idx = urbi_shape_find_slot(o->shape, sym);
    if (idx < 0 || o->slots == NULL) {
        *out = urbi_make_nil();
        return 0;
    }
    *out = o->slots[idx];
    return 0;
}

/* === Native methods ====================================================== */

static int
regexp_new(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "RegExp.new", 1, nargs, out);
    if (self.kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "RegExp.new: receiver must be an Object", out);
    if (args[0].kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "RegExp.new: pattern must be String", out);

    UObject *r = urbi_object_clone(vm, (UObject *)self.v.p);
    if (r == NULL) return urbi_raise_oom(vm, out);

    if (write_local_slot(vm, r, "_pattern", args[0]) != 0)
        return urbi_raise_oom(vm, out);

    *out = val_obj(r);
    return UEXEC_OK;
}

/* Shared body for test / match (no captures at v1.0). */
static int
regexp_do_test(UVM *vm, UValue self, UValue *args, uint8_t nargs,
               UValue *out, const char *fn_name)
{
    if (nargs != 1) return urbi_raise_arity(vm, fn_name, 1, nargs, out);
    if (self.kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "RegExp.test: receiver must be a RegExp", out);
    if (args[0].kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "RegExp.test: argument must be String", out);

    UValue pat;
    if (read_local_slot(vm, (UObject *)self.v.p, "_pattern", &pat) != 0)
        return urbi_raise_oom(vm, out);
    if (pat.kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "RegExp.test: no pattern", out);

    const char *re = (const char *)pat.v.p;
    size_t relen = urbi_strlen(re);
    const char *s = (const char *)args[0].v.p;
    size_t slen = urbi_strlen(s);

    *out = val_bool(urbi_regexp_search(re, relen, s, s + slen));
    return UEXEC_OK;
}

static int
regexp_test(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    return regexp_do_test(vm, self, args, nargs, out, "RegExp.test");
}

/* RegExp.match — alias of test at v1.0 (no capture groups). */
static int
regexp_match(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    return regexp_do_test(vm, self, args, nargs, out, "RegExp.match");
}

/* === Method-table install helper ========================================= */

typedef struct {
    const char           *name;
    urbi_native_method_fn fn;
} RMethodEntry;

static const RMethodEntry REGEXP_METHODS[] = {
    { "new",   regexp_new   },
    { "test",  regexp_test  },
    { "match", regexp_match }
};

#define REGEXP_METHODS_COUNT (sizeof(REGEXP_METHODS) / sizeof(REGEXP_METHODS[0]))

static int
install_methods(UVM *vm, UObject *proto, const RMethodEntry *table, size_t count)
{
    if (proto == NULL) return URBI_ERR_OOM;
    size_t i;
    for (i = 0U; i < count; i++) {
        UClosure *cl = urbi_native_closure_create(vm, table[i].fn);
        if (cl == NULL) return URBI_ERR_OOM;

        USymbol *sym = (USymbol *)ustr_intern(vm, table[i].name,
                                              urbi_strlen(table[i].name));
        if (sym == NULL) return URBI_ERR_OOM;

        UValue v = urbi_make_nil();
        v.kind = (uint8_t)UVAL_CLOSURE;
        v.v.p  = cl;
        if (urbi_object_set_local_slot(vm, proto, sym, v) != 0)
            return URBI_ERR_OOM;
    }
    return URBI_OK;
}

/* === Registration ======================================================== */

int
urbi_stdlib_register_regexp(UVM *vm)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;

    if (vm->regexp_proto == NULL) {
        UObject *p = urbi_object_alloc(vm, URBI_ATOM_OBJECT);
        if (p == NULL) return URBI_ERR_OOM;
        vm->regexp_proto = p;
    }
    int rc = install_methods(vm, vm->regexp_proto,
                             REGEXP_METHODS, REGEXP_METHODS_COUNT);
    if (rc != URBI_OK) return rc;

    /* Default the proto's `_pattern` slot to the empty string so an
     * un-cloned RegExp proto reads as the always-matching empty pattern. */
    USymbol *empty = (USymbol *)ustr_intern(vm, "", 0U);
    if (empty == NULL) return URBI_ERR_OOM;
    UValue ev = urbi_make_nil();
    ev.kind = (uint8_t)UVAL_STR;
    ev.v.p  = empty;
    if (write_local_slot(vm, vm->regexp_proto, "_pattern", ev) != 0)
        return URBI_ERR_OOM;

    return URBI_OK;
}

int
urbi_stdlib_register_regexp_globals(UVM *vm, URealm *realm)
{
    if (vm == NULL || realm == NULL) return URBI_ERR_INVALID_ARG;
    if (vm->regexp_proto != NULL) {
        int rc = urbi_realm_set_global(vm, realm, "RegExp", 6,
                                       val_obj(vm->regexp_proto));
        if (rc != URBI_OK) return rc;
    }
    return URBI_OK;
}
