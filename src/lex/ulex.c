/* SPDX-License-Identifier: BSD-3-Clause */
/* ULexer. */

#include "lex/ulex.h"
#include "lex/ulex_internal.h"
#include "runtime/umacros.h"
#include "urbi/types.h"   /* URBI_STATIC_ASSERT */

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>   /* memcmp, memcpy */

/* strtod (for float literal parsing): from stdlib.h on hosted; declared
 * explicitly for freestanding builds (newlib / picolibc supply it at link
 * time for embedded targets; no header pull-in needed for the declaration). */
#if __STDC_HOSTED__
#  include <stdlib.h>
#else
/* Forward declaration for newlib / picolibc strtod on embedded targets.
 * On a pure freestanding build without a C library this will produce a
 * linker error for any float literal in the input — acceptable because
 * the URBI_BYTECODE_ONLY=1 build strips src/lex/, src/parse/, src/emit/
 * from the source list entirely for bare-metal deploys (Makefile
 * COMPILER_FRONTEND_DIRS_EXCLUDED, T15 in v0.7.0-c-api).  The
 * cross-compile gate only verifies that the code compiles; actual
 * float-literal parse is host-only. */
extern double strtod(const char *, char **);
#endif

/* Length of a radix prefix ("0x", "0b", "0o").  Used by scan_radix to size
   the EMPTY_RADIX error span; LEX_RADIX_PREFIX_LEN + 1 sizes the MALFORMED
   span (prefix + the bad digit-ish byte we consumed for recovery). */
#define LEX_RADIX_PREFIX_LEN 2

static const char * const TOKEN_NAMES[] = {
    "TOK_EOF", "TOK_INT", "TOK_FLOAT", "TOK_STRING", "TOK_IDENT",
    "TOK_PLUS", "TOK_MINUS", "TOK_STAR", "TOK_SLASH",
    "TOK_LPAREN", "TOK_RPAREN",
    "TOK_PIPE", "TOK_SEMI", "TOK_COMMA", "TOK_AMP",
    "TOK_LBRACE", "TOK_RBRACE",
    "TOK_EQ", "TOK_EQEQ", "TOK_NEQ", "TOK_LT", "TOK_LE", "TOK_GT", "TOK_GE",
    "TOK_KW_VAR", "TOK_KW_FUNCTION", "TOK_KW_RETURN", "TOK_KW_IF", "TOK_KW_ELSE",
    "TOK_KW_WHILE", "TOK_KW_LAZY", "TOK_KW_CLOSURE", "TOK_KW_TRUE", "TOK_KW_FALSE",
    "TOK_KW_NIL",
    "TOK_COLON",
    "TOK_DOT", "TOK_ARROW",
    "TOK_KW_TRY", "TOK_KW_CATCH", "TOK_KW_FINALLY", "TOK_KW_THROW",
    "TOK_KW_AT", "TOK_KW_WHENEVER", "TOK_KW_WAITUNTIL", "TOK_KW_EVERY",
    "TOK_KW_ONLEAVE", "TOK_KW_SYNC", "TOK_KW_ASYNC",
    "TOK_QUESTION", "TOK_BANG",
    "TOK_KW_CLASS", "TOK_KW_PUBLIC",
    "TOK_KW_THIS",
    "TOK_ERROR"
};
/* LEX-014: positional alignment with UTokenType — guard against silent
   drift when a new token is added to one but not the other. */
URBI_STATIC_ASSERT(sizeof(TOKEN_NAMES) / sizeof(TOKEN_NAMES[0]) == TOK__LAST,
               "TOKEN_NAMES[] must have one entry per UTokenType");

static const char * const ERR_MSG[] = {
    "ok",
    "unknown character",
    "unterminated block comment",
    "ambiguous leading-zero numeric; use 0x / 0b / 0o or drop the leading zero",
    "empty radix literal",
    "malformed hex literal",
    "malformed binary literal",
    "malformed octal literal",
    "leading underscore in numeric literal",
    "trailing underscore in numeric literal",
    "adjacent underscores in numeric literal",
    "integer literal exceeds INT64_MAX",
    "unterminated string literal",
    "invalid escape sequence in string literal",
    "unicode escape requires 4 hex digits",
    "unicode code point exceeds U+10FFFF",
    "unicode escape resolves to a lone surrogate",
    "float literal has no fraction digits after the decimal point",
    "float literal exponent marker has no digits",
    "float literal exceeds representable range",
    /* === W2/v0.10.5: quoted identifiers === */
    "unterminated quoted identifier (missing closing single-quote)",
    "empty quoted identifier ('' is not a valid name)"
    /* === end W2/v0.10.5: quoted identifiers === */
};
/* LEX-015: same drift guard for ERR_MSG[] vs ULexError. */
URBI_STATIC_ASSERT(sizeof(ERR_MSG) / sizeof(ERR_MSG[0]) == LEX__LAST,
               "ERR_MSG[] must have one entry per ULexError");

static UToken make_tok_base(const UTokenType type, const int line, const int col) {
    UToken t = {0};
    t.type = type;
    t.line = line;
    t.col  = col;
    return t;
}

static UToken make_error(const ULexError code, const int line, const int col, const int len) {
    UToken t = make_tok_base(TOK_ERROR, line, col);
    t.len = len;
    t.u.err.code = code;
    /* Defensive bounds check (LEX-015); URBI_STATIC_ASSERT above pins the table
       size to LEX__LAST, but a future caller could still pass an
       out-of-range int.  Return a static fallback string rather than read
       OOB. */
    t.u.err.message = ((unsigned)code < (unsigned)LEX__LAST)
                          ? ERR_MSG[code]
                          : "unknown lex error";
    return t;
}

static UToken make_tok(const ULexer *l, const UTokenType type,
                     const char *start, const int len) {
    UToken t = make_tok_base(type, l->line, (int)(start - l->line_start) + 1);
    t.len = len;
    return t;
}

static int digit_value(const char c, const int base) {
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else return -1;
    return v < base ? v : -1;
}

/* Accumulate one digit into *acc, returning 0 on overflow. */
static int acc_digit(int64_t *acc, const int digit, const int base) {
    if (*acc > (INT64_MAX - digit) / base) return 0;
    *acc = *acc * base + digit;
    return 1;
}

/* Return value for accumulate_digits: ok==1 means *value is valid;
   ok==0 means err carries the error token. */
typedef struct { int ok; int64_t value; UToken err; } UDigitAccResult;

/* Consume underscore-separated digits in the given base starting at lex->cur.
   Returns success with the accumulated value, or an error token (adjacent
   underscores, trailing underscore, or overflow).  lex->cur is advanced past
   all consumed digits and underscores regardless of outcome. */
static UDigitAccResult accumulate_digits(ULexer *lex, const char *start,
                                         const int start_line,
                                         const int start_col, const int base) {
    UDigitAccResult r = {1, 0, {0}};
    char prev = 0;
    while (lex->cur < lex->end) {
        const char c = *lex->cur;
        if (c == '_') {
            if (prev == '_') {
                const int len = (int)(lex->cur - start) + 1;
                r.ok  = 0;
                r.err = make_error(LEX_ADJACENT_UNDERSCORES, start_line, start_col, len);
                return r;
            }
            prev = '_';
            lex->cur++;
            continue;
        }
        const int d = digit_value(c, base);
        if (d < 0) break;
        if (!acc_digit(&r.value, d, base)) {
            /* LEX-013: "first error wins" — overflow is reported and the
             * recovery loop consumes both digits AND underscores until the
             * next non-digit-non-underscore boundary so the caller resumes
             * at a clean lexeme boundary.  This deliberately MASKS any
             * trailing or adjacent underscore violation that would
             * otherwise be reported on the same literal: the user already
             * has a more impactful error (overflow) to fix first, and the
             * underscore violation reappears once they bring the literal
             * within range.  Behaviour is locked in at v0.5.8 — see the
             * scan_radix_overflow_consumes_trailing_underscores regression
             * in test_lexer.c. */
            while (lex->cur < lex->end &&
                   (digit_value(*lex->cur, base) >= 0 || *lex->cur == '_')) {
                lex->cur++;
            }
            const int len = (int)(lex->cur - start);
            r.ok  = 0;
            r.err = make_error(LEX_INT_OVERFLOW, start_line, start_col, len);
            return r;
        }
        prev = c;
        lex->cur++;
    }
    if (prev == '_') {
        const int len = (int)(lex->cur - start);
        r.ok  = 0;
        r.err = make_error(LEX_TRAILING_UNDERSCORE, start_line, start_col, len);
    }
    return r;
}

/* Scan a radix-prefixed integer. lex->cur points at the first char after
   the prefix; start points at the '0' of the prefix; prefix length is
   LEX_RADIX_PREFIX_LEN (2).  base is 16/2/8; malformed code is the
   base-appropriate LEX_MALFORMED_*. */
static UToken scan_radix(ULexer *lex, const char *start, const int base,
                        const ULexError malformed_code) {
    const int start_col = (int)(start - lex->line_start) + 1;
    const int start_line = lex->line;

    /* Must have at least one digit or underscore. */
    if (lex->cur >= lex->end) {
        return make_error(LEX_EMPTY_RADIX, start_line, start_col,
                          LEX_RADIX_PREFIX_LEN);
    }
    const char c0 = *lex->cur;
    if (c0 == '_') {
        /* leading underscore after prefix */
        while (lex->cur < lex->end &&
               (digit_value(*lex->cur, base) >= 0 || *lex->cur == '_')) {
            lex->cur++;
        }
        const int len = (int)(lex->cur - start);
        return make_error(LEX_LEADING_UNDERSCORE,
                          start_line, start_col, len);
    }
    if (digit_value(c0, base) < 0) {
        /* Either EMPTY_RADIX (followed by non-alpha/digit-ish non-continuation)
           or MALFORMED — distinguish by whether the char looks like it was
           trying to be a digit. Any [0-9a-zA-Z] that isn't valid for this
           base = MALFORMED; anything else = EMPTY_RADIX. */
        const int looks_digitish =
            (c0 >= '0' && c0 <= '9') ||
            (c0 >= 'a' && c0 <= 'z') ||
            (c0 >= 'A' && c0 <= 'Z');
        if (looks_digitish) {
            /* Consume the bad digit-ish char so we advance. */
            lex->cur++;
            return make_error(malformed_code,
                              start_line, start_col,
                              LEX_RADIX_PREFIX_LEN + 1);
        }
        return make_error(LEX_EMPTY_RADIX,
                          start_line, start_col,
                          LEX_RADIX_PREFIX_LEN);
    }

    const UDigitAccResult r = accumulate_digits(lex, start, start_line, start_col, base);
    if (!r.ok) return r.err;

    UToken t = make_tok_base(TOK_INT, start_line, start_col);
    t.len = (int)(lex->cur - start);
    t.u.i = r.value;
    return t;
}

/* Identifier character classification.  Defined ahead of scan_number /
 * scan_radix so the suffix-parsing fall-throughs can call them without a
 * forward declaration (LEX-022, Wave 1 v0.5.3-layout). */
static int is_ident_start(const char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_';
}

static int is_ident_cont(const char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

typedef struct {
    const char *suffix;
    int         sufflen;
    int64_t     mul;  /* positive: value *= mul; negative: value /= -mul */
} UDurationSuffix;

/* Longer suffixes first so "ms" / "us" / "ns" match before bare "m".
   The single boundary check inside apply_duration_suffix is uniform across
   one-char and two-char entries — closes LEX-008. */
static const UDurationSuffix kDurationSuffixes[] = {
    { "ms", 2,          1000LL },
    { "us", 2,             1LL },
    { "ns", 2,        -1000LL },   /* division: value /= 1000 */
    { "s",  1,       1000000LL },
    { "m",  1,      60000000LL },
    { "h",  1,    3600000000LL },
    { "d",  1,   86400000000LL },
    { NULL, 0,             0LL },
};

/* === W4/v0.10.5: angle literals === */

/* Angle-suffix table (legacy §20.1.6.1).  Each entry carries:
 *   suffix  — the literal suffix text (NUL-terminated for clarity).
 *   sufflen — byte length of the suffix.
 *   from    — divisor for the unit (deg: 180; grad: 200; rad: 1).
 *   to      — multiplier for the unit (deg/grad: π; rad: 1).
 *
 * Conversion formula matches legacy utoken.l RETURN_UNIT macro exactly:
 *   radians = value / from * to
 * This is intentionally "divide then multiply" (not "multiply by
 * precomputed ratio") so that integer multiples of a full circle are
 * exactly representable.  E.g. 200grad: 200/200 * π = 1.0 * π = exactly π.
 *
 * All three entries produce TOK_FLOAT regardless of whether the numeric
 * part was scanned as integer or float.  Longer suffixes first: "grad" (4)
 * before "deg" and "rad" (3) — same longest-first discipline as duration
 * table (LEX-008). */
typedef struct {
    const char *suffix;
    int         sufflen;
    double      from;
    double      to;
} UAngleSuffix;

/* M_PI equivalent without requiring <math.h> on freestanding targets.
 * IEEE-754 double has enough precision (53-bit mantissa) to hold this
 * constant to the last representable ULP. */
#define URBI_PI_D 3.14159265358979323846

static const UAngleSuffix kAngleSuffixes[] = {
    { "grad", 4, 200.0, URBI_PI_D },  /* grade/gradian: n / 200 * π */
    { "deg",  3, 180.0, URBI_PI_D },  /* degree:        n / 180 * π */
    { "rad",  3,   1.0, 1.0       },  /* radian:        n / 1   * 1 */
    { NULL,   0,   0.0, 0.0       },
};

/* apply_angle_suffix — if lex->cur sits at an angle suffix ("deg", "rad",
   "grad"), consume it and store the radian-converted double in *out_rad.
   Returns 1 if a suffix was matched and consumed (caller must emit
   TOK_FLOAT); 0 if no angle suffix present (caller continues as before).
   Ident-cont boundary check: next char after suffix must not be ident-cont
   (same discipline as apply_duration_suffix — closes LEX-008 for angles). */
static int apply_angle_suffix(ULexer *lex, double in_value, double *out_rad) {
    for (const UAngleSuffix *e = kAngleSuffixes; e->suffix != NULL; e++) {
        if (lex->cur + e->sufflen > lex->end) continue;
        if (!urbi_memeq(lex->cur, e->suffix, e->sufflen)) continue;
        /* Boundary: next char must not be ident-cont. */
        if (lex->cur + e->sufflen < lex->end &&
            is_ident_cont(lex->cur[e->sufflen])) continue;
        lex->cur += e->sufflen;
        *out_rad = in_value / e->from * e->to;
        return 1;
    }
    return 0;
}

/* === end W4/v0.10.5: angle literals === */

/* Result of dispatch_radix_prefix: either we routed to scan_radix / produced
   an AMBIGUOUS_LEADING_ZERO error (handled=1, tok carries the value), or
   the caller should fall through to decimal accumulation (handled=0). */
typedef struct { int handled; UToken tok; } URadixDispatch;

/* On a leading '0', detect a radix prefix (0x/0b/0o) or an ambiguous
   leading-zero sequence ("01", "0_") and produce a complete token.
   Otherwise return handled=0 so the caller can fall through to decimal
   digit accumulation. */
static URadixDispatch dispatch_radix_prefix(ULexer *lex, const char *start,
                                            const int start_line,
                                            const int start_col) {
    URadixDispatch r = {0, {0}};
    if (*start != '0' || lex->cur + 1 >= lex->end) return r;

    const char c2 = lex->cur[1];
    if (c2 == 'x' || c2 == 'X') {
        lex->cur += LEX_RADIX_PREFIX_LEN;
        r.handled = 1;
        r.tok = scan_radix(lex, start, 16, LEX_MALFORMED_HEX);
        return r;
    }
    if (c2 == 'b' || c2 == 'B') {
        lex->cur += LEX_RADIX_PREFIX_LEN;
        r.handled = 1;
        r.tok = scan_radix(lex, start, 2, LEX_MALFORMED_BIN);
        return r;
    }
    if (c2 == 'o' || c2 == 'O') {
        lex->cur += LEX_RADIX_PREFIX_LEN;
        r.handled = 1;
        r.tok = scan_radix(lex, start, 8, LEX_MALFORMED_OCT);
        return r;
    }
    if ((c2 >= '0' && c2 <= '9') || c2 == '_') {
        /* LEX-012: precondition for the leading-zero ambiguous path —
         * lex->cur must be at start so that `cur - start` after consumption
         * measures the full ambiguous span.  All entry paths into
         * dispatch_radix_prefix come from scan_number which sets
         * `start = lex->cur` immediately before the call; this assert pins
         * that invariant against future refactors. */
        URBI_INTERNAL_ASSERT(lex->cur == start);
        /* Consume the leading-zero sequence so caller advances. */
        lex->cur++;
        while (lex->cur < lex->end &&
               ((*lex->cur >= '0' && *lex->cur <= '9') || *lex->cur == '_')) {
            lex->cur++;
        }
        const int len = (int)(lex->cur - start);
        r.handled = 1;
        r.tok = make_error(LEX_AMBIGUOUS_LEADING_ZERO,
                           start_line, start_col, len);
        return r;
    }
    return r;
}

/* Accumulate base-10 digits at lex->cur into a TOK_INT.  On overflow or an
   underscore violation, returns an error token via UDigitAccResult.err. */
static UDigitAccResult scan_decimal_digits(ULexer *lex, const char *start,
                                           const int start_line,
                                           const int start_col) {
    return accumulate_digits(lex, start, start_line, start_col, 10);
}

/* If lex->cur sits at a duration suffix ("ms", "us", "ns", "s", "m", "h",
   "d"), consume it and scale *value to microseconds.  No-op otherwise.
   Suffix table is ordered longest-first so "ms" beats bare "m"; the
   ident-cont boundary check is uniform across all entries (LEX-008
   structurally closed by the table rewrite — both two-char and one-char
   paths now share one predicate).

   Returns 1 on overflow (the multiply by mul would exceed INT64_MAX); the
   caller is responsible for emitting a LEX_INT_OVERFLOW token.  The division
   path (ns) cannot overflow.  Returns 0 on success or no-suffix (LEX-006). */
static int apply_duration_suffix(ULexer *lex, int64_t *value) {
    for (const UDurationSuffix *e = kDurationSuffixes; e->suffix != NULL; e++) {
        if (lex->cur + e->sufflen > lex->end) continue;
        if (!urbi_memeq(lex->cur, e->suffix, e->sufflen)) continue;
        /* Boundary: next char must not be ident-cont. */
        if (lex->cur + e->sufflen < lex->end &&
            is_ident_cont(lex->cur[e->sufflen])) continue;
        lex->cur += e->sufflen;
        if (e->mul >= 0) {
            /* Pre-check: the digit accumulator only guards INT64_MAX during
               digit-by-digit accumulation; a value that fits as a bare int
               can still wrap when scaled by 1000…86_400_000_000. */
            if (e->mul > 0 && *value > INT64_MAX / e->mul) return 1;
            *value *= e->mul;
        } else {
            *value /= -e->mul;
        }
        return 0;
    }
    return 0;
}

/* scan_float_body — called when a float literal is confirmed.  lex->cur
   points at the first character AFTER the part already scanned (either the
   decimal point that we just confirmed is followed by a digit, or the 'e'/'E'
   of an exponent).  start points at the beginning of the entire numeric
   literal in the source buffer.
   Consumes: optional remaining fraction digits + optional exponent form
             ([eE][+-]?[0-9]+).
   Returns a TOK_FLOAT token on success or a TOK_ERROR on bad exponent form
   or out-of-range value.
   Precondition: lex->cur points at the '.' that starts the fraction (for the
   fraction path) or at 'e'/'E' (for the integer-plus-exponent path). */
static UToken scan_float_body(ULexer *lex, const char *start,
                              const int start_line, const int start_col) {
    /* Consume fraction digits if we are sitting on a '.'. */
    if (lex->cur < lex->end && *lex->cur == '.') {
        /* Peek: next char must be a digit (caller should have verified this
           for the INT+'.<digit>' promotion path, but be defensive). */
        if (lex->cur + 1 >= lex->end ||
            !(lex->cur[1] >= '0' && lex->cur[1] <= '9')) {
            /* Trailing dot — no fraction digits. */
            lex->cur++;   /* consume the dot for recovery */
            const int len = (int)(lex->cur - start);
            return make_error(LEX_FLOAT_TRAILING_DOT, start_line, start_col, len);
        }
        lex->cur++;   /* consume '.' */
        while (lex->cur < lex->end && *lex->cur >= '0' && *lex->cur <= '9') {
            lex->cur++;
        }
    }

    /* Consume optional exponent [eE][+-]?[0-9]+. */
    if (lex->cur < lex->end && (*lex->cur == 'e' || *lex->cur == 'E')) {
        lex->cur++;   /* consume 'e'/'E' */
        /* Optional sign. */
        if (lex->cur < lex->end && (*lex->cur == '+' || *lex->cur == '-')) {
            lex->cur++;
        }
        /* Must have at least one digit. */
        if (lex->cur >= lex->end || !(*lex->cur >= '0' && *lex->cur <= '9')) {
            const int len = (int)(lex->cur - start);
            return make_error(LEX_FLOAT_EXPONENT_NO_DIGITS,
                              start_line, start_col, len);
        }
        while (lex->cur < lex->end && *lex->cur >= '0' && *lex->cur <= '9') {
            lex->cur++;
        }
    }

    /* Use strtod to convert the full source span to double. */
    const int span = (int)(lex->cur - start);
    /* strtod needs a NUL-terminated string; the source buffer may not have
       one at lex->cur.  We copy the span into a stack buffer — float literal
       lengths are bounded (no underscores, no radix prefixes) so 64 bytes
       is ample.  If somehow the span is larger, strtod will stop at the
       first non-float character, which is correct. */
    char buf[64];
    int copy_len = span < 63 ? span : 63;
    int ci;
    for (ci = 0; ci < copy_len; ci++) buf[ci] = start[ci];
    buf[copy_len] = '\0';

    char *endp = NULL;
    const double val = strtod(buf, &endp);
    /* Detect overflow: strtod returns ±infinity when the value exceeds the
     * representable double range.  We check without <math.h> by using the
     * IEEE-754 identity that (val - val) is NaN (not 0.0) for infinite
     * operands; this avoids a <math.h> dependency in the lexer (freestanding
     * targets may not have isinf() without the header). */
    if (val != 0.0 && val - val != 0.0) {
        return make_error(LEX_FLOAT_OVERFLOW, start_line, start_col, span);
    }

    /* === W4/v0.10.5: angle literals — float path ===
     * After the strtod conversion, check for an angle suffix.  If present,
     * apply the radian conversion and extend the token span to include the
     * suffix.  This handles "1.5deg", "0.5rad", etc. */
    {
        double rad_val;
        if (apply_angle_suffix(lex, val, &rad_val)) {
            UToken ta = make_tok_base(TOK_FLOAT, start_line, start_col);
            ta.len = (int)(lex->cur - start);
            ta.u.f = rad_val;
            return ta;
        }
    }
    /* === end W4/v0.10.5: angle literals — float path === */

    UToken t = make_tok_base(TOK_FLOAT, start_line, start_col);
    t.len = span;
    t.u.f = val;
    return t;
}

/* scan_float_leading_dot — called from ulex_next when '.' is followed by a
   decimal digit.  lex->cur points at the '.'. */
static UToken scan_float_leading_dot(ULexer *lex) {
    const char *start = lex->cur;
    const int start_col = (int)(start - lex->line_start) + 1;
    const int start_line = lex->line;
    /* scan_float_body will consume the '.' and all subsequent float parts. */
    return scan_float_body(lex, start, start_line, start_col);
}

/* Scan a numeric literal starting at lex->cur.  Caller has confirmed
   *lex->cur is a decimal digit; this function dispatches to the radix
   path on a leading '0' and otherwise scans a decimal integer with an
   optional duration suffix or float promotion.  Renamed from scan_decimal
   (LEX-018).
   Float promotion rules (Gap #5):
     - After accumulating decimal digits, if the next char is '.' followed by
       a digit, promote to TOK_FLOAT (calls scan_float_body at the '.').
     - If the next char after digits is 'e' or 'E', promote to TOK_FLOAT
       (calls scan_float_body at the 'e'/'E').
     - Disambiguation: '0.foo' keeps INT(0) DOT IDENT(foo) because '.' is not
       followed by a digit. */
static UToken scan_number(ULexer *lex) {
    const char *start = lex->cur;
    const int start_col = (int)(start - lex->line_start) + 1;
    const int start_line = lex->line;

    const URadixDispatch rd = dispatch_radix_prefix(lex, start, start_line, start_col);
    if (rd.handled) return rd.tok;

    const UDigitAccResult dr = scan_decimal_digits(lex, start, start_line, start_col);
    if (!dr.ok) return dr.err;
    int64_t value = dr.value;

    /* Float promotion: '.<digit>', '.<EOF>', or 'e'/'E' after the integer
       part.
       Disambiguation:
         '1.5'  → TOK_FLOAT (dot followed by digit)
         '1.'   → TOK_ERROR / LEX_FLOAT_TRAILING_DOT (dot at EOF or before
                  non-ident-non-digit; scan_float_body reports the error)
         '1.foo'→ TOK_INT + TOK_DOT + TOK_IDENT (dot followed by ident char;
                  keep as-is for member-access)
       Rule: enter scan_float_body on '.' only when the char after '.' is
       a decimal digit OR there is no char after '.' (EOF).  If '.' is
       followed by an ident-start character, leave the integer as-is so
       '0.foo', 'obj.method' patterns work. */
    if (lex->cur < lex->end) {
        const char c = *lex->cur;
        if (c == '.') {
            /* Peek at what follows the dot. */
            if (lex->cur + 1 >= lex->end) {
                /* Dot at end of input → trailing-dot error. */
                return scan_float_body(lex, start, start_line, start_col);
            }
            const char after_dot = lex->cur[1];
            if (after_dot >= '0' && after_dot <= '9') {
                /* Dot followed by digit → valid fraction. */
                return scan_float_body(lex, start, start_line, start_col);
            }
            /* Dot followed by ident char or other punctuation: leave as INT;
               '.' will become TOK_DOT on the next ulex_next call. */
        }
        /* Check for bare exponent: 'e'/'E' without a fraction. */
        if (c == 'e' || c == 'E') {
            return scan_float_body(lex, start, start_line, start_col);
        }
    }

    if (apply_duration_suffix(lex, &value)) {
        return make_error(LEX_INT_OVERFLOW, start_line, start_col,
                          (int)(lex->cur - start));
    }

    /* === W4/v0.10.5: angle literals — integer path ===
     * Check for angle suffix AFTER duration (duration suffixes have already
     * consumed their tokens above).  apply_angle_suffix handles "deg", "grad",
     * "rad" with the same ident-cont boundary check used by duration. */
    {
        double rad_val;
        if (apply_angle_suffix(lex, (double)value, &rad_val)) {
            UToken ta = make_tok_base(TOK_FLOAT, start_line, start_col);
            ta.len = (int)(lex->cur - start);
            ta.u.f = rad_val;
            return ta;
        }
    }
    /* === end W4/v0.10.5: angle literals — integer path === */

    UToken t = make_tok_base(TOK_INT, start_line, start_col);
    t.len = (int)(lex->cur - start);
    t.u.i = value;
    return t;
}

typedef struct {
    const char *name;
    int         len;
    UTokenType  type;
} UKeyword;

/* KW_ENTRY drops the redundant hand-counted length from KEYWORDS[] entries
   (LEX-016).  `sizeof(name) - 1` excludes the trailing NUL of a string
   literal — fine since every name above is a literal. */
#define KW_ENTRY(name, tok) { name, (int)(sizeof(name) - 1), tok }

/* Sorted by name for human readability; lookup is linear — kept in sync
 * with the keyword set, faster than a hash for this size (LEX-017: count
 * elided to avoid drift between comment and table). */
static const UKeyword KEYWORDS[] = {
    KW_ENTRY("async",     TOK_KW_ASYNC),
    KW_ENTRY("at",        TOK_KW_AT),
    KW_ENTRY("catch",     TOK_KW_CATCH),
    KW_ENTRY("class",     TOK_KW_CLASS),
    KW_ENTRY("closure",   TOK_KW_CLOSURE),
    KW_ENTRY("else",      TOK_KW_ELSE),
    KW_ENTRY("every",     TOK_KW_EVERY),
    KW_ENTRY("false",     TOK_KW_FALSE),
    KW_ENTRY("finally",   TOK_KW_FINALLY),
    KW_ENTRY("function",  TOK_KW_FUNCTION),
    KW_ENTRY("if",        TOK_KW_IF),
    KW_ENTRY("lazy",      TOK_KW_LAZY),
    KW_ENTRY("nil",       TOK_KW_NIL),
    KW_ENTRY("onleave",   TOK_KW_ONLEAVE),
    KW_ENTRY("public",    TOK_KW_PUBLIC),
    KW_ENTRY("return",    TOK_KW_RETURN),
    KW_ENTRY("sync",      TOK_KW_SYNC),
    KW_ENTRY("this",      TOK_KW_THIS),
    KW_ENTRY("throw",     TOK_KW_THROW),
    KW_ENTRY("true",      TOK_KW_TRUE),
    KW_ENTRY("try",       TOK_KW_TRY),
    KW_ENTRY("var",       TOK_KW_VAR),
    KW_ENTRY("waituntil", TOK_KW_WAITUNTIL),
    KW_ENTRY("whenever",  TOK_KW_WHENEVER),
    KW_ENTRY("while",     TOK_KW_WHILE),
};
#define KEYWORD_COUNT (sizeof KEYWORDS / sizeof KEYWORDS[0])

static UTokenType keyword_lookup(const char *bytes, int len) {
    for (size_t i = 0; i < KEYWORD_COUNT; i++) {
        if (KEYWORDS[i].len == len) {
            int eq = 1;
            for (int j = 0; j < len; j++) {
                if (KEYWORDS[i].name[j] != bytes[j]) { eq = 0; break; }
            }
            if (eq) return KEYWORDS[i].type;
        }
    }
    return TOK_IDENT;
}

static UToken scan_ident(ULexer *lex) {
    const char *start = lex->cur;
    const int start_col = (int)(start - lex->line_start) + 1;
    const int start_line = lex->line;
    while (lex->cur < lex->end && is_ident_cont(*lex->cur)) {
        lex->cur++;
    }
    const int len = (int)(lex->cur - start);
    const UTokenType kw_type = keyword_lookup(start, len);

    UToken t = make_tok_base(kw_type, start_line, start_col);
    t.len = len;
    /* Populate u.str for both keywords and identifiers; useful for
     * downstream diagnostics that need to quote the lexeme. */
    t.u.str.start = start;
    t.u.str.len = len;
    return t;
}

/* === W2/v0.10.5: quoted identifiers === */

/* scan_quoted_ident — scan a 'X' quoted-identifier form (legacy §20.1.4).
 *
 * On entry lex->cur points at the opening single-quote character.
 * On success: lex->cur is advanced past the closing single-quote; returns
 *   TOK_IDENT with u.str.{start,len} pointing at the unquoted body (i.e.
 *   the characters between the quotes).  The token's len field covers the
 *   full quoted form including delimiters (for diagnostic span purposes).
 * On error: cursor is advanced for recovery; returns TOK_ERROR.
 *
 * Constraints (legacy manual §20.1.4):
 *   - The body may contain any character except newline ('\n').
 *   - An empty body ('') is rejected (empty identifier has no meaning).
 *   - The quote must be closed before EOF or newline (unterminated → error).
 *
 * No escape sequences inside quoted identifiers.  The legacy manual says
 * they "may contain any character" with no mention of escaping. */
static UToken scan_quoted_ident(ULexer *lex) {
    const char *outer_start = lex->cur;          /* points at opening '\'' */
    const int start_line = lex->line;
    const int start_col = (int)(outer_start - lex->line_start) + 1;
    lex->cur++;                                   /* skip opening '\'' */

    const char *body_start = lex->cur;

    while (lex->cur < lex->end) {
        const char ch = *lex->cur;
        if (ch == '\'') {
            /* Found the closing quote. */
            const char *body_end = lex->cur;
            lex->cur++;                           /* skip closing '\'' */
            const int body_len = (int)(body_end - body_start);
            if (body_len == 0) {
                /* '' — empty quoted identifier is invalid. */
                const int span = (int)(lex->cur - outer_start);
                return make_error(LEX_EMPTY_QUOTED_IDENT, start_line, start_col, span);
            }
            /* Emit TOK_IDENT; u.str points at unquoted body. */
            UToken t = make_tok_base(TOK_IDENT, start_line, start_col);
            t.len = (int)(lex->cur - outer_start); /* full quoted span */
            t.u.str.start = body_start;
            t.u.str.len   = body_len;
            return t;
        }
        if (ch == '\n') {
            /* Newline before close quote — unterminated. */
            break;
        }
        lex->cur++;
    }

    /* Reached EOF or newline without a closing quote. */
    const int span = (int)(lex->cur - outer_start);
    return make_error(LEX_UNTERMINATED_QUOTED_IDENT, start_line, start_col, span);
}

/* === end W2/v0.10.5: quoted identifiers === */

/* urbi_encode_utf8 — emit 1-4 UTF-8 bytes for a code point.  See the
 * docstring in src/lex/ulex_internal.h for the full contract; this
 * helper is non-validating and assumes the caller has already rejected
 * out-of-range and lone-surrogate inputs. */
int urbi_encode_utf8(uint32_t cp, unsigned char buf[4]) {
    if (cp <= 0x7F) {
        buf[0] = (unsigned char)cp;
        return 1;
    }
    if (cp <= 0x7FF) {
        buf[0] = (unsigned char)(0xC0 | (cp >> 6));
        buf[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        buf[0] = (unsigned char)(0xE0 | (cp >> 12));
        buf[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    /* cp <= 0x10FFFF (caller-validated). */
    buf[0] = (unsigned char)(0xF0 | (cp >> 18));
    buf[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Lex-time validation of a \u escape body.  On entry lex->cur points at
 * the 'u' of the escape; on success lex->cur is advanced past the entire
 * \u-form (4 hex digits for \uXXXX, '{' + 1-6 hex + '}' for \u{HHHHHH}).
 *
 * The caller (lex_string) owns the surrounding error-token construction;
 * this helper returns LEX_OK on success or one of the v0.6.1 unicode
 * lex error codes.  The cursor is advanced past the offending span on
 * error per the LEX-028 recovery contract.
 *
 * Lone surrogates (U+D800..U+DFFF) are rejected for both forms — they
 * are reserved by RFC 3629 / Unicode for UTF-16 pair encoding and have
 * no UTF-8 byte sequence.  Code points exceeding U+10FFFF are rejected
 * as out-of-range. */
static ULexError validate_unicode_escape(ULexer *lex) {
    /* Skip past the 'u'. */
    lex->cur++;

    if (lex->cur < lex->end && *lex->cur == '{') {
        /* \u{HHHHHH} form — 1 to 6 hex digits, then '}'. */
        lex->cur++;
        uint32_t cp = 0;
        int hex_count = 0;
        while (lex->cur < lex->end && hex_count < 6) {
            const int d = digit_value(*lex->cur, 16);
            if (d < 0) break;
            cp = (cp << 4) | (uint32_t)d;
            lex->cur++;
            hex_count++;
        }
        /* Empty (\u{}) is rejected as too-short; >6 hex digits would
         * exceed U+FFFFFF anyway and is rejected via the missing-'}' or
         * out-of-range path below. */
        if (hex_count == 0) {
            return LEX_UNICODE_ESCAPE_TOO_SHORT;
        }
        if (lex->cur >= lex->end || *lex->cur != '}') {
            /* Missing closing '}': treat as too-short.  Cursor is already
             * past the hex digits; do not advance again, the outer loop
             * will resume on the current byte. */
            return LEX_UNICODE_ESCAPE_TOO_SHORT;
        }
        /* Consume the '}'. */
        lex->cur++;
        if (cp > 0x10FFFF) {
            return LEX_UNICODE_ESCAPE_OUT_OF_RANGE;
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return LEX_LONE_SURROGATE;
        }
        return LEX_OK;
    }

    /* \uXXXX form — exactly 4 hex digits. */
    uint32_t cp = 0;
    int hex_count = 0;
    while (lex->cur < lex->end && hex_count < 4) {
        const int d = digit_value(*lex->cur, 16);
        if (d < 0) break;
        cp = (cp << 4) | (uint32_t)d;
        lex->cur++;
        hex_count++;
    }
    if (hex_count < 4) {
        return LEX_UNICODE_ESCAPE_TOO_SHORT;
    }
    /* The 4-hex form cannot exceed U+FFFF; only the surrogate range needs
     * a guard. */
    if (cp >= 0xD800 && cp <= 0xDFFF) {
        return LEX_LONE_SURROGATE;
    }
    return LEX_OK;
}

/* lex_string — consume a "..." string literal (LEX-035 / v0.6.1 Phase 1).
 *
 * Pre: lex->cur points at the opening '"'; start_line / start_col record
 * the position of that opening quote (1-based).
 *
 * Post: on success, lex->cur points one past the closing '"' and the
 * returned UToken has type=TOK_STRING with u.str.start/len pointing at the
 * INTERIOR of the literal (no quote chars).  The byte span is the raw
 * source view — escape sequences are NOT resolved here; the parser owns
 * escape resolution so the lexer stays zero-allocation (LEX-027).
 *
 * Wave 1 (v0.6.0) escape set: \n (newline), \t (tab), \\ (backslash),
 * \" (quote).
 *
 * Wave 2 (v0.6.1) additions: \uXXXX (4-hex BMP code point) and
 * \u{HHHHHH} (1-6 hex full-plane up to U+10FFFF).  Both forms are
 * validated for syntax + range here; the parser uses urbi_encode_utf8
 * to materialize the UTF-8 byte sequence into the AST string-literal
 * buffer (escape resolution is monotonically non-expansive — every \X
 * is at least 2 source bytes, every UTF-8 emission is at most 4 bytes,
 * so the parser's worst-case-source-len capacity holds).
 *
 * Errors (cursor advances past the offending span for clean recovery,
 * LEX-028 contract):
 *   - LEX_UNTERMINATED_STRING: EOF reached before closing quote.  Cursor
 *     ends at lex->end.
 *   - LEX_INVALID_ESCAPE: an unrecognized escape body was encountered.
 *     Cursor advances past the bad escape body so the next ulex_next can
 *     resume cleanly.
 *   - LEX_UNICODE_ESCAPE_TOO_SHORT: \uXXXX with fewer than 4 hex digits,
 *     or \u{} with no hex digits, or \u{...} missing the closing '}'.
 *   - LEX_UNICODE_ESCAPE_OUT_OF_RANGE: \u{HHHHHH} exceeds U+10FFFF.
 *   - LEX_LONE_SURROGATE: \u escape resolves to U+D800..U+DFFF. */
static UToken lex_string(ULexer *lex, const int start_line, const int start_col) {
    /* Skip opening quote. */
    lex->cur++;
    const char *body_start = lex->cur;

    while (lex->cur < lex->end && *lex->cur != '"') {
        if (*lex->cur == '\\') {
            /* Validate the escape body, then advance past it.  The parser
             * resolves the byte; the lexer just guards the recognized set. */
            lex->cur++;
            if (lex->cur < lex->end) {
                const char c = *lex->cur;
                if (c == 'n' || c == 't' || c == '\\' || c == '"') {
                    lex->cur++;
                } else if (c == 'u') {
                    const ULexError uerr = validate_unicode_escape(lex);
                    if (uerr != LEX_OK) {
                        return make_error(uerr, start_line, start_col,
                                          (int)(lex->cur - body_start) + 1);
                    }
                } else {
                    /* Recovery: consume the bad escape body so the next
                     * ulex_next call resumes at a clean boundary. */
                    lex->cur++;
                    return make_error(LEX_INVALID_ESCAPE,
                                      start_line, start_col,
                                      (int)(lex->cur - body_start) + 1);
                }
            }
            /* If we hit EOF mid-escape, the outer loop will exit and the
             * unterminated-string branch reports the error. */
        } else {
            if (*lex->cur == '\n') {
                lex->line++;
                lex->line_start = lex->cur + 1;
            }
            lex->cur++;
        }
    }

    if (lex->cur >= lex->end) {
        return make_error(LEX_UNTERMINATED_STRING, start_line, start_col,
                          (int)(lex->cur - body_start) + 1);
    }

    /* lex->cur points at the closing '"'. */
    UToken t = make_tok_base(TOK_STRING, start_line, start_col);
    t.len = (int)(lex->cur - body_start) + 2;   /* includes both quote chars */
    t.u.str.start = body_start;
    t.u.str.len = (int)(lex->cur - body_start);
    lex->cur++;                                  /* skip closing quote */
    return t;
}

void ulex_init(ULexer *lex, const char *src, const size_t len) {
    /* Preconditions (LEX-001 + LEX-027): lex must be non-NULL; src must be
     * non-NULL whenever len > 0.  The (NULL, 0) case is permitted — it
     * represents empty input (e.g. an idle REPL) and ulex_next will return
     * TOK_EOF without dereferencing src.  Asserts fire in URBI_DEBUG builds;
     * release builds inherit the original behaviour (UB on NULL+N). */
    URBI_INTERNAL_ASSERT(lex != NULL);
    URBI_INTERNAL_ASSERT(src != NULL || len == 0);

    lex->src = src;
    lex->end = src + len;
    lex->cur = src;
    lex->line = 1;
    lex->line_start = src;
    lex->source_name = "<stdin>";
    lex->syncline_depth = 0;
    /* syncline_stack contents irrelevant when depth == 0 */
    lex->syncline_pool_idx = 0;
    /* syncline_name_pool contents irrelevant until first //#line or //#push */

    /* LEX-002: post-init invariant.  `line_start == src` even on empty input;
     * for len == 0, (cur - line_start) is 0 and the column computed by
     * make_eof / make_tok stays 1.  The pointer arithmetic is well-defined
     * for src == NULL only when len == 0 (asserted above). */
    URBI_INTERNAL_ASSERT(lex->line_start == lex->src);
    URBI_INTERNAL_ASSERT(lex->cur == lex->src);
    URBI_INTERNAL_ASSERT(lex->line == 1);
}

static UToken make_eof(const ULexer *l) {
    return make_tok_base(TOK_EOF, l->line, (int)(l->cur - l->line_start) + 1);
}

typedef struct {
    ULexError code;
    int line;
    int col;
    int len;       /* error span length; defaults to 2 (the slash-star prefix) */
} UTriviaResult;

/* try_parse_syncline — mini-parser for //#line / //#push / //#pop directives.
 *
 * Called with l->cur pointing at the '#' character (the two '//' have already
 * been consumed).  On success: advances l->cur to the end-of-line position
 * (just before '\n' or at EOF); updates lexer state; returns true.  The
 * caller's plain-comment loop then skips the terminating newline.
 *
 * On failure (malformed or unrecognized directive): resets l->cur to 'start'
 * (the '#' position); returns false.  The caller falls back to plain-comment
 * skip-to-newline, treating the whole sequence as a comment.
 *
 * Overflow (//#push when syncline_depth == URBI_SYNCLINE_STACK_MAX) and
 * underflow (//#pop on empty stack) degrade silently: the directive is
 * consumed (returns true) but the lexer state is left unchanged.
 *
 * Filename storage: per-lex round-robin name pool of (URBI_SYNCLINE_STACK_MAX+1)
 * slots of URBI_SYNCLINE_NAME_MAX bytes each.  Filenames longer than
 * URBI_SYNCLINE_NAME_MAX-1 are silently truncated.  Pool slot count exceeds
 * the stack depth by one, guaranteeing that the live source_name and all
 * stacked entries remain valid simultaneously.
 *
 * v0.9.0-repl. */
static bool
try_parse_syncline(ULexer *l)
{
    const char *start = l->cur;       /* points at '#' */
    if (l->cur >= l->end || *l->cur != '#') goto fail;
    l->cur++;                         /* past '#' */

    /* Directive name: scan lowercase ASCII letters. */
    const char *name_start = l->cur;
    while (l->cur < l->end && *l->cur >= 'a' && *l->cur <= 'z') l->cur++;
    size_t name_len = (size_t)(l->cur - name_start);

    /* Skip whitespace after directive name. */
    while (l->cur < l->end && (*l->cur == ' ' || *l->cur == '\t')) l->cur++;

    /* --- //#pop --- */
    if (name_len == 3 && memcmp(name_start, "pop", 3) == 0) {
        /* No arguments.  Remaining characters to '\n' must be whitespace. */
        const char *p = l->cur;
        while (p < l->end && *p != '\n') {
            if (*p != ' ' && *p != '\t' && *p != '\r') goto fail;
            p++;
        }
        l->cur = p;   /* at '\n' or EOF */
        if (l->syncline_depth > 0) {
            l->syncline_depth--;
            l->source_name = l->syncline_stack[l->syncline_depth].file;
            l->line        = (int)l->syncline_stack[l->syncline_depth].line;
        }
        /* else: underflow — silent no-op */
        return true;
    }

    /* --- //#line and //#push both take N "FILE" --- */
    bool is_push = (name_len == 4 && memcmp(name_start, "push", 4) == 0);
    bool is_line = (name_len == 4 && memcmp(name_start, "line", 4) == 0);
    if (!is_push && !is_line) goto fail;

    /* Parse decimal line number (required, at least one digit). */
    if (l->cur >= l->end || *l->cur < '0' || *l->cur > '9') goto fail;
    uint32_t n = 0;
    while (l->cur < l->end && *l->cur >= '0' && *l->cur <= '9') {
        if (n > 0xFFFFFFFFU / 10U) goto fail;   /* overflow guard */
        n = n * 10U + (uint32_t)(*l->cur - '0');
        l->cur++;
    }
    while (l->cur < l->end && (*l->cur == ' ' || *l->cur == '\t')) l->cur++;

    /* Parse "FILE" — opening quote, no escape sequences, closing quote. */
    if (l->cur >= l->end || *l->cur != '"') goto fail;
    l->cur++;                         /* past opening '"' */
    const char *file_start = l->cur;
    while (l->cur < l->end && *l->cur != '"' && *l->cur != '\n') l->cur++;
    if (l->cur >= l->end || *l->cur != '"') goto fail;
    size_t file_len = (size_t)(l->cur - file_start);
    l->cur++;                         /* past closing '"' */

    /* Remaining characters to '\n' must be whitespace. */
    const char *p = l->cur;
    while (p < l->end && *p != '\n') {
        if (*p != ' ' && *p != '\t' && *p != '\r') goto fail;
        p++;
    }
    l->cur = p;   /* at '\n' or EOF */

    /* Allocate filename in the round-robin name pool.
     * Truncate to URBI_SYNCLINE_NAME_MAX - 1 bytes if necessary. */
    uint8_t slot = l->syncline_pool_idx;
    l->syncline_pool_idx =
        (uint8_t)((slot + 1U) % (uint8_t)(URBI_SYNCLINE_STACK_MAX + 1));
    size_t copy_len = file_len < (URBI_SYNCLINE_NAME_MAX - 1U)
                      ? file_len : (URBI_SYNCLINE_NAME_MAX - 1U);
    memcpy(l->syncline_name_pool[slot], file_start, copy_len);
    l->syncline_name_pool[slot][copy_len] = '\0';
    const char *stored_file = l->syncline_name_pool[slot];

    if (is_push) {
        if (l->syncline_depth < URBI_SYNCLINE_STACK_MAX) {
            /* Save current state before applying N + FILE. */
            l->syncline_stack[l->syncline_depth].file = l->source_name;
            l->syncline_stack[l->syncline_depth].line = (uint32_t)l->line;
            l->syncline_stack[l->syncline_depth].col  = 0;
            l->syncline_depth++;
        }
        /* else: overflow — still apply N + FILE (matches //#line semantics),
         * but don't push anything onto the (full) stack. */
    }

    /* Apply: set line so the next physical newline bumps it to N. */
    l->line        = (int)n - 1;
    l->source_name = stored_file;
    return true;

fail:
    l->cur = start;
    return false;
}

static UTriviaResult skip_trivia(ULexer *l) {
    /* LEX-003: initialize line/col to 1 (not 0) so that even if a future
     * caller reads them on the LEX_OK path the values are valid 1-based
     * positions, not sentinels.  Error paths overwrite these with the
     * actual error position (e.g. start_line/start_col for an unterminated
     * block comment).  LEX-004: len defaults to 2 (the "/" + "*" prefix);
     * error paths overwrite with the actual span. */
    UTriviaResult r = {LEX_OK, 1, 1, 2};
    while (l->cur < l->end) {
        const char c = *l->cur;
        if (c == ' ' || c == '\t') {
            l->cur++;
        } else if (c == '\n') {
            l->cur++;
            l->line++;
            l->line_start = l->cur;
        } else if (c == '\r') {
            if (l->cur + 1 < l->end && l->cur[1] == '\n') {
                l->cur += 2;
                l->line++;
                l->line_start = l->cur;
            } else {
                /* Lone CR — not a line terminator; let dispatch emit
                   LEX_UNKNOWN_CHAR on the next ulex_next call. */
                break;
            }
        } else if (c == '/' && l->cur + 1 < l->end && l->cur[1] == '/') {
            /* Line comment.  v0.9.0-repl: check for syncline directive
             * (//#line / //#push / //#pop) before treating as plain comment. */
            l->cur += 2;   /* past '//' */
            if (l->cur < l->end && *l->cur == '#') {
                /* Mini-parser handles directive; on failure falls back to
                 * plain-comment skip-to-newline below. */
                (void)try_parse_syncline(l);
            }
            /* Skip remainder of line (handles both failure fall-through and
             * any trailing whitespace past a successfully-parsed directive). */
            while (l->cur < l->end && *l->cur != '\n') {
                l->cur++;
            }
        } else if (c == '/' && l->cur + 1 < l->end && l->cur[1] == '*') {
            /* Block comment — NON-NESTING (LEX-034).  The first occurrence
             * of "*"+"/" closes the comment regardless of intervening
             * "/"+"*" sequences.  Matches C semantics.
             *
             * This diverges from legacy urbiscript (aldebaran 2.x), which
             * supported nested block comments.  The choice is locked by the
             * tests/chk/lex/block_comment_no_nest.chk pin fixture and
             * documented at:
             *   docs/LANG-CONVENTIONS.md §7 "Block comments — divergence
             *   from legacy"
             *   docs/language-compatibility-matrix.md row "Block comments"
             *   (status: dropped / locked non-nesting; legacy F7 / Wave 6 W6)
             *
             * Record start for error reporting. */
            const int start_line = l->line;
            const int start_col = (int)(l->cur - l->line_start) + 1;
            const char *const start = l->cur;
            l->cur += 2;
            int closed = 0;
            while (l->cur + 1 < l->end) {
                if (l->cur[0] == '*' && l->cur[1] == '/') {
                    l->cur += 2;
                    closed = 1;
                    break;
                }
                if (*l->cur == '\n') {
                    l->line++;
                    l->line_start = l->cur + 1;
                }
                l->cur++;
            }
            if (!closed) {
                /* Advance cur to end so caller sees EOF on retry. */
                l->cur = l->end;
                r.code = LEX_UNTERMINATED_BLOCK_COMMENT;
                r.line = start_line;
                r.col = start_col;
                /* LEX-004: report the full unterminated extent, not just the
                 * "/" + "*" prefix.  Span runs from the opening "/" to the
                 * end of the source. */
                r.len = (int)(l->cur - start);
                return r;
            }
        } else {
            break;
        }
    }
    return r;
}

/* Purely single-char punctuation tokens — 0 (TOK_EOF) means "not here". */
static const UTokenType kPunctTable[256] = {
    ['+'] = TOK_PLUS,   ['*'] = TOK_STAR,    ['/'] = TOK_SLASH,
    ['('] = TOK_LPAREN, [')'] = TOK_RPAREN,
    ['|'] = TOK_PIPE,   [';'] = TOK_SEMI,    [','] = TOK_COMMA,
    ['&'] = TOK_AMP,    ['{'] = TOK_LBRACE,  ['}'] = TOK_RBRACE,
    [':'] = TOK_COLON,  ['.'] = TOK_DOT,     ['?'] = TOK_QUESTION,
};

UToken ulex_next(ULexer *lex) {
    UTriviaResult tr = skip_trivia(lex);
    if (tr.code != LEX_OK) {
        /* LEX-004: tr.len carries the actual error span (full unterminated
         * extent for block comments; 2 — "/" + "*" — for any future
         * trivia-level error that doesn't override it). */
        return make_error(tr.code, tr.line, tr.col, tr.len);
    }
    if (lex->cur >= lex->end) {
        return make_eof(lex);
    }

    const char *start = lex->cur;
    const char c = *lex->cur;

    /* String literal — needs its own helper (escape recognition + unterminated
     * tracking).  Branched ahead of the punct fast-path because the helper
     * advances lex->cur past the closing quote and handles its own errors. */
    if (c == '"') {
        const int start_line = lex->line;
        const int start_col = (int)(start - lex->line_start) + 1;
        return lex_string(lex, start_line, start_col);
    }

    /* === W2/v0.10.5: quoted identifiers ===
     * 'X' — single-quote-delimited identifier (legacy §20.1.4).
     * Emits TOK_IDENT with u.str pointing at the unquoted body.
     * Branched ahead of the punct fast-path (single-quote is not in
     * kPunctTable so would fall through to LEX_UNKNOWN_CHAR anyway). */
    if (c == '\'') {
        return scan_quoted_ident(lex);
    }
    /* === end W2/v0.10.5: quoted identifiers === */

    /* Leading-dot float: '.5', '.123', etc.  Must be checked before the
     * punct table fast-path (which would otherwise emit TOK_DOT).
     * Disambiguation: '.' followed by a non-digit remains TOK_DOT. */
    if (c == '.' && lex->cur + 1 < lex->end &&
        lex->cur[1] >= '0' && lex->cur[1] <= '9') {
        return scan_float_leading_dot(lex);
    }

    /* Fast path: purely single-char punctuation. */
    {
        const UTokenType pt = kPunctTable[(unsigned char)c];
        if (pt != 0) { lex->cur++; return make_tok(lex, pt, start, 1); }
    }

    /* Multi-char tokens and the default fall-through. */
    switch (c) {
    case '-':
        if (lex->cur + 1 < lex->end && lex->cur[1] == '>') {
            lex->cur += 2;
            return make_tok(lex, TOK_ARROW, start, 2);
        }
        lex->cur++;
        return make_tok(lex, TOK_MINUS, start, 1);
    case '=':
        if (lex->cur + 1 < lex->end && lex->cur[1] == '=') {
            lex->cur += 2;
            return make_tok(lex, TOK_EQEQ, start, 2);
        }
        lex->cur++;
        return make_tok(lex, TOK_EQ, start, 1);
    case '!':
        if (lex->cur + 1 < lex->end && lex->cur[1] == '=') {
            lex->cur += 2;
            return make_tok(lex, TOK_NEQ, start, 2);
        }
        lex->cur++;
        return make_tok(lex, TOK_BANG, start, 1);
    case '<':
        if (lex->cur + 1 < lex->end && lex->cur[1] == '=') {
            lex->cur += 2;
            return make_tok(lex, TOK_LE, start, 2);
        }
        lex->cur++;
        return make_tok(lex, TOK_LT, start, 1);
    case '>':
        if (lex->cur + 1 < lex->end && lex->cur[1] == '=') {
            lex->cur += 2;
            return make_tok(lex, TOK_GE, start, 2);
        }
        lex->cur++;
        return make_tok(lex, TOK_GT, start, 1);
    default:
        if (c >= '0' && c <= '9') {
            return scan_number(lex);
        }
        if (is_ident_start(c)) {
            return scan_ident(lex);
        }
        {
            const int col = (int)(lex->cur - lex->line_start) + 1;
            const int line = lex->line;
            lex->cur++;  /* recovery: advance past the bad byte */
            return make_error(LEX_UNKNOWN_CHAR, line, col, 1);
        }
    }
}

const char *ulex_token_name(const UTokenType t) {
    if ((unsigned)t >= sizeof(TOKEN_NAMES) / sizeof(TOKEN_NAMES[0])) {
        return "TOK_UNKNOWN";
    }
    return TOKEN_NAMES[t];
}
