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
 * Backtracking budget:
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

#include "rt/ustdlib_glue.h"
#include "stdlib/regexp.h"

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
    USym *sym = usym_cstr(vm, name);
    if (sym == NULL) return -1;
    return uobj_set_local(vm, o, sym, value, 0) < 0 ? -1 : 0;
}

static int
read_local_slot(UVM *vm, UObject *o, const char *name, UValue *out)
{
    const USym *sym = usym_cstr(vm, name);
    UObjSlotRef ref;
    if (sym == NULL) return -1;
    /* Through the chain, so an un-cloned RegExp still sees the
     * prototype's default pattern. */
    if (!uobj_resolve(vm, o, sym, &ref)) { *out = uv_nil(); return 0; }
    *out = uobj_slot_value(&ref);
    return 0;
}

/* === Native methods ====================================================== */

static int
regexp_new(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    if (self.kind != UV_OBJ)
        return urbi_raise_type(vm, "RegExp.new: receiver must be an Object", out);
    if (!urbi_is_str(args[0]))
        return urbi_raise_type(vm, "RegExp.new: pattern must be String", out);

    UObject *r = uobj_new(vm, (UObject *)self.v.p);
    if (r == NULL) return urbi_raise_oom(vm, out);

    if (write_local_slot(vm, r, "_pattern", args[0]) != 0)
        return urbi_raise_oom(vm, out);

    *out = uv_obj(r);
    return UEXEC_OK;
}

/* Shared body for test / match (no captures at v1.0). */
static int
regexp_do_test(UVM *vm, UValue self, UValue *args, uint8_t nargs,
               UValue *out, const char *fn_name)
{
    (void)nargs; (void)fn_name;
    if (self.kind != UV_OBJ)
        return urbi_raise_type(vm, "RegExp.test: receiver must be a RegExp", out);
    if (!urbi_is_str(args[0]))
        return urbi_raise_type(vm, "RegExp.test: argument must be String", out);

    UValue pat;
    if (read_local_slot(vm, (UObject *)self.v.p, "_pattern", &pat) != 0)
        return urbi_raise_oom(vm, out);
    if (!urbi_is_str(pat))
        return urbi_raise_type(vm, "RegExp.test: no pattern", out);

    const char *re  = urbi_str_cstr(pat);
    size_t      relen = urbi_str_size(pat);
    const char *s   = urbi_str_cstr(args[0]);
    size_t      slen = urbi_str_size(args[0]);

    int result = urbi_regexp_search(re, relen, s, s + slen);
    if (result < 0) {
        /* Budget exhausted: raise a catchable RangeError from the top-level
         * native entry point so the C stack has fully unwound from the
         * recursive matcher before the throw is delivered. */
        return urbi_raise_range(vm, "regexp budget exceeded", out);
    }
    *out = uv_bool(result);
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

/* === the table =========================================================== */

const UMethodDef ustdlib_regexp_methods[USTDLIB_REGEXP_NMETHODS] = {
    { "new",   regexp_new,   1, 1 },
    { "test",  regexp_test,  1, 1 },
    { "match", regexp_match, 1, 1 }
};

/* The bare prototype carries the empty pattern, which matches anything,
 * so `RegExp.test("x")` on the un-cloned proto answers rather than
 * failing to find a pattern. */
int urbi_regexp_init(UVM *vm, UObject *proto)
{
    UValue empty = urbi_make_str_interned(vm, "", 0);
    if (empty.kind == UV_NIL) return URBI_ERR_OOM;
    return write_local_slot(vm, proto, "_pattern", empty) == 0 ? URBI_OK : URBI_ERR_OOM;
}
