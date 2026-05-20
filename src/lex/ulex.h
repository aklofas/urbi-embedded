/* SPDX-License-Identifier: BSD-3-Clause */
/* Public lexer API. */

#ifndef ULEX_H
#define ULEX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* v0.9.0-repl: depth cap for //#push / //#pop syncline directives.
 * 4 is sufficient for any plausible REPL framing (top + include + macro-expand
 * + safety).  Overflow degrades silently (drop further pushes); underflow
 * pops on an empty stack also degrade silently. */
#ifndef URBI_SYNCLINE_STACK_MAX
#  define URBI_SYNCLINE_STACK_MAX 4
#endif

/* Maximum length of a //#line or //#push filename (including NUL).
 * Filenames longer than this are silently truncated. */
#ifndef URBI_SYNCLINE_NAME_MAX
#  define URBI_SYNCLINE_NAME_MAX 256
#endif

/* Full token-type space of the lexer.  Every call to ulex_next returns
   exactly one of these values in UToken.type. */
typedef enum {
    TOK_EOF = 0,      /* end of input — sentinel */
    TOK_INT,          /* integer literal */
    TOK_FLOAT,        /* floating-point literal — 1.5, .5, 1.5e3, 1e3 */
    TOK_STRING,       /* string literal — "foo", with \n/\t/\\/\" escapes */
    TOK_IDENT,        /* identifier [a-zA-Z_][a-zA-Z0-9_]* */

    /* arithmetic — M1 */
    TOK_PLUS,         /* + */
    TOK_MINUS,        /* - */
    TOK_STAR,         /* * */
    TOK_SLASH,        /* / */
    TOK_LPAREN,       /* ( */
    TOK_RPAREN,       /* ) */

    /* separators — M2 */
    TOK_PIPE,         /* | */
    TOK_SEMI,         /* ; */
    TOK_COMMA,        /* , */
    TOK_AMP,          /* & */

    /* blocks — M2 */
    TOK_LBRACE,       /* { */
    TOK_RBRACE,       /* } */

    /* assignment + comparison — M2 */
    TOK_EQ,           /* =  */
    TOK_EQEQ,         /* == */
    TOK_NEQ,          /* != */
    TOK_LT,           /* <  */
    TOK_LE,           /* <= */
    TOK_GT,           /* >  */
    TOK_GE,           /* >= */

    /* keywords — M2 */
    TOK_KW_VAR,
    TOK_KW_FUNCTION,
    TOK_KW_RETURN,
    TOK_KW_IF,
    TOK_KW_ELSE,
    TOK_KW_WHILE,
    TOK_KW_LAZY,
    TOK_KW_CLOSURE,    /* recognized for migration-error path; no semantics */
    TOK_KW_TRUE,
    TOK_KW_FALSE,
    TOK_KW_NIL,

    /* M3 tag-scope separator */
    TOK_COLON,        /* : */

    /* member access — M4 */
    TOK_DOT,          /* .  — slot access (obj.x) */
    TOK_ARROW,        /* -> — slot-property access (obj.x->prop) */

    /* keywords — M3 control-transfer */
    TOK_KW_TRY,
    TOK_KW_CATCH,
    TOK_KW_FINALLY,
    TOK_KW_THROW,

    /* keywords — M5 reactive */
    TOK_KW_AT,
    TOK_KW_WHENEVER,
    TOK_KW_WAITUNTIL,
    TOK_KW_EVERY,
    TOK_KW_ONLEAVE,
    TOK_KW_SYNC,
    TOK_KW_ASYNC,

    /* M5 punctuation — event postfix sugar */
    TOK_QUESTION,      /* ? — event-subscribe postfix inside at(...) */
    TOK_BANG,          /* ! — event-emit postfix (e.g. `e!`) */

    /* M6 wave 1 — class declarations */
    TOK_KW_CLASS,
    TOK_KW_PUBLIC,

    /* M6 wave 3 — this keyword (Gap #3) */
    TOK_KW_THIS,

    TOK_ERROR,        /* malformed input */

    TOK__LAST          /* sentinel; not a real token type — used to size
                          TOKEN_NAMES[] and detect drift via URBI_STATIC_ASSERT */
} UTokenType;

/* Error codes carried in UToken.u.err.code when UToken.type == TOK_ERROR. */
typedef enum {
    LEX_OK = 0,
    LEX_UNKNOWN_CHAR,
    LEX_UNTERMINATED_BLOCK_COMMENT,
    LEX_AMBIGUOUS_LEADING_ZERO,
    LEX_EMPTY_RADIX,
    LEX_MALFORMED_HEX,
    LEX_MALFORMED_BIN,
    LEX_MALFORMED_OCT,
    LEX_LEADING_UNDERSCORE,
    LEX_TRAILING_UNDERSCORE,
    LEX_ADJACENT_UNDERSCORES,
    LEX_INT_OVERFLOW,
    LEX_UNTERMINATED_STRING,    /* string opened but not closed before EOF */
    LEX_INVALID_ESCAPE,         /* unrecognized escape sequence \x */
    LEX_UNICODE_ESCAPE_TOO_SHORT,    /* \uXXXX form has fewer than 4 hex digits */
    LEX_UNICODE_ESCAPE_OUT_OF_RANGE, /* \u{HHHHHH} code point exceeds U+10FFFF */
    LEX_LONE_SURROGATE,              /* \u escape resolves to U+D800..U+DFFF */
    LEX_FLOAT_TRAILING_DOT,          /* 1. — no fraction digits after the decimal point */
    LEX_FLOAT_EXPONENT_NO_DIGITS,    /* 1.5e+ or 1e — exponent marker with no digits */
    LEX_FLOAT_OVERFLOW,              /* float literal exceeds representable range (±inf) */
    LEX__LAST          /* sentinel; not a real error code — used to size
                          ERR_MSG[] and detect drift via URBI_STATIC_ASSERT */
} ULexError;

/*
 * UToken — returned by value from ulex_next; no heap allocation.
 *
 * Lifetime (LEX-029): u.str.start is a non-owning pointer into the caller's
 * source buffer (the same buffer passed to ulex_init).  The source buffer
 * MUST outlive any UToken that references it.  u.err.message is a static-
 * storage string literal (lives for the program lifetime; no caller action
 * required).  This holds for every UToken consumer — parser, REPL,
 * diagnostic emitters — every site that reads u.str.start must keep the
 * source buffer alive at least as long as the UToken.
 *
 * Union invariants (active member per type):
 *   u.i    — TOK_INT: the parsed integer value
 *   u.f    — TOK_FLOAT: the parsed floating-point value (double)
 *   u.str  — TOK_IDENT (and keyword tokens TOK_KW_*): start/len point into
 *            the source buffer (caller-owned lifetime, see above)
 *   u.err  — TOK_ERROR: code (ULexError) + static-storage message string
 *   (other types leave u zero-valued)
 *
 * Position fields: line and col are 1-based; len is the span in source bytes.
 */
typedef struct {
    UTokenType type;
    int line;         /* 1-based */
    int col;          /* 1-based column of first char */
    int len;          /* span length in source chars */
    union {
        int64_t i;                          /* TOK_INT */
        double  f;                          /* TOK_FLOAT */
        struct {                            /* TOK_IDENT */
            const char *start;
            int len;
        } str;
        struct {                            /* TOK_ERROR */
            int code;                       /* ULexError */
            const char *message;            /* static string */
        } err;
    } u;
} UToken;

/*
 * ULexer — stack-allocated by the caller; zero-initialized by ulex_init.
 * No heap allocation at any point during lexing.
 *
 * The source buffer (src) must outlive the ULexer and any UTokens derived
 * from it, and must not be modified while lexing is in progress.
 */
typedef struct {
    const char *src;
    const char *end;
    const char *cur;
    int line;
    const char *line_start;
    /* v0.9.0-repl: syncline state.
     * source_name defaults to "<stdin>" (or whatever was passed to ulex_init).
     * Rewritten by //#line / //#push / //#pop.  Pointer into the symbol
     * table (lifetime = VM lifetime). */
    const char *source_name;
    struct {
        const char *file;
        uint32_t    line;
        uint32_t    col;
    } syncline_stack[URBI_SYNCLINE_STACK_MAX];
    uint8_t syncline_depth;
    /* Name pool for //#line and //#push filenames.  Round-robin allocation
     * over (URBI_SYNCLINE_STACK_MAX + 1) slots keeps the current source_name
     * plus all stacked names alive for the lifetime of the ULexer.
     * Filenames longer than URBI_SYNCLINE_NAME_MAX - 1 are truncated.
     * v0.9.0-repl. */
    char    syncline_name_pool[URBI_SYNCLINE_STACK_MAX + 1][URBI_SYNCLINE_NAME_MAX];
    uint8_t syncline_pool_idx;
} ULexer;

/* Initialize the ULexer over a source buffer.  No allocation.
 *
 * Preconditions (LEX-001 + LEX-027):
 *   - lex must be non-NULL.
 *   - src must point to at least len valid bytes for any len > 0.
 *   - The (NULL, 0) case is permitted: it represents empty input (e.g.
 *     a freshly-opened REPL with no line yet) and ulex_next will return
 *     TOK_EOF without dereferencing src.
 *
 * The source buffer must remain valid and unmodified for the lifetime of
 * the ULexer AND for the lifetime of every UToken the lexer produces
 * (UToken.u.str.start aliases into it — see UToken docs above).
 *
 * Preconditions are enforced by URBI_INTERNAL_ASSERT in URBI_DEBUG builds;
 * release builds inherit the original UB-on-violation semantics. */
void ulex_init(ULexer *lex, const char *src, size_t len);

/* v0.9.0-repl: current claimed source name (syncline-aware).  Defaults to
 * "<stdin>" if no syncline directive has been seen. */
static inline const char *
ulex_current_source(const ULexer *lex)
{
    return lex && lex->source_name ? lex->source_name : "<stdin>";
}

/* Read and return the next UToken.  Idempotent at EOF — subsequent calls
 * keep returning TOK_EOF.
 *
 * Post-error advance contract (LEX-028): after a TOK_ERROR the cursor has
 * advanced past the offending lexeme so a follow-up ulex_next resumes at a
 * clean boundary.  Per-error specifics:
 *
 *   - LEX_UNKNOWN_CHAR: cursor advances exactly 1 byte (the bad byte).
 *   - LEX_UNTERMINATED_BLOCK_COMMENT: cursor jumps to end-of-source; the
 *     reported len covers the full unterminated extent.
 *   - LEX_AMBIGUOUS_LEADING_ZERO, LEX_INT_OVERFLOW, LEX_LEADING_UNDERSCORE,
 *     LEX_TRAILING_UNDERSCORE, LEX_ADJACENT_UNDERSCORES: cursor advances
 *     past the entire malformed numeric run so the next token starts on
 *     the byte after the literal.
 *   - LEX_EMPTY_RADIX, LEX_MALFORMED_HEX/BIN/OCT: cursor advances past the
 *     "0x" / "0b" / "0o" prefix plus the offending byte (if any).
 *
 * In every case the caller may continue lexing for error recovery without
 * risk of an infinite loop. */
UToken ulex_next(ULexer *lex);

/* Return a static string such as "TOK_PLUS" for the given type.
   Debug helper; bounds-guarded (out-of-range returns "TOK_UNKNOWN").
   Never allocates. */
const char *ulex_token_name(UTokenType t);

#ifdef __cplusplus
}
#endif

#endif
