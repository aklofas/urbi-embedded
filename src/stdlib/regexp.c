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
 * Captures are tracked as a v1.x follow-up.
 *
 * Backtracking budget (STD-04):
 *   RE_BUDGET_STEPS — maximum re_match_here_b calls per urbi_regexp_search
 *     invocation.  Bounds worst-case run-time; 1 000 000 steps at an
 *     embedded Cortex-M7 rate (~10 ns/step) ≈ 10 ms.  Real patterns
 *     consume at most a few thousand steps; catastrophic patterns
 *     (a*a*a*a*b against a long all-a string) exhaust it quickly.
 *   RE_BUDGET_DEPTH — maximum simultaneous re_match_here_b frames on the
 *     C call stack.  Follows the loader depth cap precedent:
 *     UCHUNK_MAX_PROTO_DEPTH = 64 was sized for 64 KB MCU stacks; regexp
 *     frames are similarly sized (~64 B on 32-bit), so 128 levels = 8 KB
 *     stack headroom — well within the smallest 64 KB embedded target.
 *     Real patterns rarely exceed 30 levels.
 *   On either exhaustion urbi_regexp_search returns RE_MATCH_BUDGET (-1).
 *   The top-level native call (regexp_do_test) converts that into a
 *   catchable RangeError; the matcher itself never allocates or raises. */

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

/* === Backtracking budget constants ======================================= */

/* Maximum re_match_here_b calls per urbi_regexp_search invocation.
 * Sized to abort catastrophic backtracking (a*a*a*a*b against 25+ a's
 * exhausts ~1.3 M paths — over the cap) while completing every legitimate
 * in-tree pattern in well under 10 000 steps. */
#define RE_BUDGET_STEPS  1000000U

/* Maximum simultaneous re_match_here_b frames on the C call stack.
 * Follows UCHUNK_MAX_PROTO_DEPTH = 64 (loader, sized for 64 KB MCU stacks).
 * Regexp frames are similarly sized (~64 B on 32-bit); 128 levels = 8 KB,
 * safely within the smallest 64 KB embedded stack budget.
 * Real patterns (e.g. a?a?a?... with N quantifiers) produce N levels;
 * the worst legitimate in-tree pattern uses fewer than 20 levels. */
#define RE_BUDGET_DEPTH  128U

/* Distinguishable return code meaning "budget exhausted".
 * Must not collide with 0 (no match) or 1 (match). */
#define RE_MATCH_BUDGET  (-1)

/* Per-call budget struct threaded through the recursive matcher. */
struct re_budget {
    uint32_t steps;   /* remaining step allowance; exhausted when 0 */
    uint16_t depth;   /* remaining depth; exhausted when 0 */
};

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

/* Forward decl: budgeted matcher anchored at the start of [s, s_end).
 * Returns RE_MATCH_BUDGET (-1) if the budget is exhausted, 1 on match,
 * 0 on no-match.  Never raises — the budget check is a plain integer
 * comparison; the caller (urbi_regexp_search → regexp_do_test) raises. */
static int re_match_here_b(const char *re, size_t relen,
                            const char *s, const char *s_end,
                            struct re_budget *budget);

/* Greedy quantifier `*` / `+` over an atom: match `atom` (length alen)
 * zero/one-or-more times at s, then the rest of the pattern after the
 * quantifier.  `min` is 0 for `*`, 1 for `+`.  Propagates RE_MATCH_BUDGET. */
static int
re_match_repeat_b(const char *atom, size_t alen,
                  const char *rest, size_t restlen,
                  const char *s, const char *s_end, int min,
                  struct re_budget *budget)
{
    /* Consume as many matching chars as possible, then backtrack from
     * longest to shortest (greedy).  Step cost is charged by the recursive
     * re_match_here_b calls below, not by the forward scan. */
    const char *p = s;
    while (p < s_end && re_atom_matches(atom, alen, *p)) p++;
    while ((size_t)(p - s) >= (size_t)min) {
        int r = re_match_here_b(rest, restlen, p, s_end, budget);
        if (r != 0) return r;   /* 1 (match) or RE_MATCH_BUDGET — propagate */
        if (p == s) break;
        p--;
    }
    return 0;
}

/* Budgeted core matcher.  Returns 1 (match), 0 (no match), or
 * RE_MATCH_BUDGET (budget exhausted).  Single-exit so every path
 * restores budget->depth before returning. */
static int
re_match_here_b(const char *re, size_t relen,
                const char *s, const char *s_end,
                struct re_budget *budget)
{
    /* --- budget check at entry ------------------------------------------ */
    if (budget->steps == 0U || budget->depth == 0U) return RE_MATCH_BUDGET;
    budget->steps--;
    budget->depth--;

    int result;

    if (relen == 0U) {
        result = 1;                              /* empty pattern matches */
    } else if (re[0] == '$' && relen == 1U) {
        result = (s == s_end);                   /* end-of-string anchor */
    } else {
        size_t alen = re_atom_len(re, relen);
        char quant  = (char)((relen > alen) ? re[alen] : '\0');

        if (quant == '*' || quant == '+') {
            const char *rest    = re + alen + 1U;
            size_t      restlen = relen - alen - 1U;
            result = re_match_repeat_b(re, alen, rest, restlen, s, s_end,
                                       (quant == '+') ? 1 : 0, budget);
        } else if (quant == '?') {
            const char *rest    = re + alen + 1U;
            size_t      restlen = relen - alen - 1U;
            /* Try matching the atom once; if that fails (or no char), skip. */
            result = 0;
            if (s < s_end && re_atom_matches(re, alen, *s)) {
                result = re_match_here_b(rest, restlen, s + 1U, s_end, budget);
            }
            if (result == 0) {   /* also handles RE_MATCH_BUDGET: don't retry */
                result = re_match_here_b(rest, restlen, s, s_end, budget);
            }
        } else {
            /* Plain atom: must match one char, then the rest. */
            if (s < s_end && re_atom_matches(re, alen, *s)) {
                result = re_match_here_b(re + alen, relen - alen,
                                         s + 1U, s_end, budget);
            } else {
                result = 0;
            }
        }
    }

    budget->depth++;
    return result;
}

/* Public matcher entry: does [re, re+relen) match anywhere in [s, s_end)?
 *
 * Returns:
 *   1              — pattern matches (at some position)
 *   0              — pattern does not match
 *   RE_MATCH_BUDGET (-1) — step or depth budget exhausted
 *
 * The budget is fresh per call; it is NOT shared across calls and does not
 * accumulate across a long-running REPL session. */
int
urbi_regexp_search(const char *re, size_t relen, const char *s, const char *s_end)
{
    if (re == NULL || s == NULL || s_end == NULL) return 0;

    struct re_budget bud;
    bud.steps = RE_BUDGET_STEPS;
    bud.depth = (uint16_t)RE_BUDGET_DEPTH;

    if (relen > 0U && re[0] == '^') {           /* start anchor: only at s */
        return re_match_here_b(re + 1U, relen - 1U, s, s_end, &bud);
    }
    /* Unanchored: try each starting position, including s_end (so an empty
     * or `$`-only pattern can match at the end).  Budget is shared across
     * all positions — exhaustion from any position stops the search. */
    const char *p = s;
    for (;;) {
        int r = re_match_here_b(re, relen, p, s_end, &bud);
        if (r != 0) return r;   /* 1 (match) or RE_MATCH_BUDGET — propagate */
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

    const char *re  = (const char *)pat.v.p;
    size_t      relen = urbi_strlen(re);
    const char *s   = (const char *)args[0].v.p;
    size_t      slen = urbi_strlen(s);

    int result = urbi_regexp_search(re, relen, s, s + slen);
    if (result < 0) {
        /* Budget exhausted: raise a catchable RangeError from the top-level
         * native entry point so the C stack has fully unwound from the
         * recursive matcher before the throw is delivered. */
        return urbi_raise_range(vm, "regexp budget exceeded", out);
    }
    *out = val_bool(result);
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
