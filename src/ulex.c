/* SPDX-License-Identifier: BSD-3-Clause */
/* ULexer. */

#include "ulex.h"

#include <limits.h>

static const char * const TOKEN_NAMES[] = {
    "TOK_EOF", "TOK_INT", "TOK_IDENT",
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
    "TOK_KW_AT", "TOK_KW_WHENEVER", "TOK_KW_WAITUNTIL",
    "TOK_KW_ONLEAVE", "TOK_KW_SYNC", "TOK_KW_ASYNC",
    "TOK_ERROR"
};

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
    "integer literal exceeds INT64_MAX"
};

static UToken make_error(const ULexError code, const int line, const int col, const int len) {
    UToken t = {0};
    t.type = TOK_ERROR;
    t.line = line;
    t.col = col;
    t.len = len;
    t.u.err.code = code;
    t.u.err.message = ERR_MSG[code];
    return t;
}

static UToken make_tok(const ULexer *l, const UTokenType type,
                     const char *start, const int len) {
    UToken t = {0};
    t.type = type;
    t.line = l->line;
    t.col = (int)(start - l->line_start) + 1;
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

/* Scan a radix-prefixed integer. lex->cur points at the first char after
   the prefix; start points at the '0' of the prefix; prefix_len is 2.
   base is 16/2/8; malformed code is the base-appropriate LEX_MALFORMED_*. */
static UToken scan_radix(ULexer *lex, const char *start, const int base,
                        const ULexError malformed_code) {
    const int start_col = (int)(start - lex->line_start) + 1;
    const int start_line = lex->line;

    /* Must have at least one digit or underscore. */
    if (lex->cur >= lex->end) {
        return make_error(LEX_EMPTY_RADIX, start_line, start_col, 2);
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
                              start_line, start_col, 3);
        }
        return make_error(LEX_EMPTY_RADIX,
                          start_line, start_col, 2);
    }

    int64_t value = 0;
    char prev = 0;
    while (lex->cur < lex->end) {
        const char c = *lex->cur;
        if (c == '_') {
            if (prev == '_') {
                const int len = (int)(lex->cur - start) + 1;
                return make_error(LEX_ADJACENT_UNDERSCORES,
                                  start_line, start_col, len);
            }
            prev = '_';
            lex->cur++;
            continue;
        }
        const int d = digit_value(c, base);
        if (d < 0) break;
        if (!acc_digit(&value, d, base)) {
            while (lex->cur < lex->end &&
                   (digit_value(*lex->cur, base) >= 0 || *lex->cur == '_')) {
                lex->cur++;
            }
            const int len = (int)(lex->cur - start);
            return make_error(LEX_INT_OVERFLOW,
                              start_line, start_col, len);
        }
        prev = c;
        lex->cur++;
    }

    if (prev == '_') {
        const int len = (int)(lex->cur - start);
        return make_error(LEX_TRAILING_UNDERSCORE,
                          start_line, start_col, len);
    }

    UToken t = {0};
    t.type = TOK_INT;
    t.line = start_line;
    t.col = start_col;
    t.len = (int)(lex->cur - start);
    t.u.i = value;
    return t;
}

/* Forward declaration for suffix parsing in scan_decimal. */
static int is_ident_cont(const char c);

/* Scan a decimal integer starting at lex->cur.
   Caller has confirmed *lex->cur is a decimal digit. */
static UToken scan_decimal(ULexer *lex) {
    const char *start = lex->cur;
    const int start_col = (int)(start - lex->line_start) + 1;
    const int start_line = lex->line;

    /* Radix-prefix dispatch on a leading '0'. */
    if (*start == '0' && lex->cur + 1 < lex->end) {
        const char c2 = lex->cur[1];
        if (c2 == 'x' || c2 == 'X') {
            lex->cur += 2;
            return scan_radix(lex, start, 16, LEX_MALFORMED_HEX);
        }
        if (c2 == 'b' || c2 == 'B') {
            lex->cur += 2;
            return scan_radix(lex, start, 2, LEX_MALFORMED_BIN);
        }
        if (c2 == 'o' || c2 == 'O') {
            lex->cur += 2;
            return scan_radix(lex, start, 8, LEX_MALFORMED_OCT);
        }
        if ((c2 >= '0' && c2 <= '9') || c2 == '_') {
            /* Consume the leading-zero sequence so caller advances. */
            lex->cur++;
            while (lex->cur < lex->end &&
                   ((*lex->cur >= '0' && *lex->cur <= '9') || *lex->cur == '_')) {
                lex->cur++;
            }
            const int len = (int)(lex->cur - start);
            return make_error(LEX_AMBIGUOUS_LEADING_ZERO,
                              start_line, start_col, len);
        }
    }

    int64_t value = 0;
    char prev = 0;
    while (lex->cur < lex->end) {
        const char c = *lex->cur;
        if (c == '_') {
            if (prev == '_') {
                const int len = (int)(lex->cur - start) + 1;
                return make_error(LEX_ADJACENT_UNDERSCORES,
                                  start_line, start_col, len);
            }
            prev = '_';
            lex->cur++;
            continue;
        }
        const int d = digit_value(c, 10);
        if (d < 0) break;
        if (!acc_digit(&value, d, 10)) {
            while (lex->cur < lex->end &&
                   (digit_value(*lex->cur, 10) >= 0 || *lex->cur == '_')) {
                lex->cur++;
            }
            const int len = (int)(lex->cur - start);
            return make_error(LEX_INT_OVERFLOW,
                              start_line, start_col, len);
        }
        prev = c;
        lex->cur++;
    }

    if (prev == '_') {
        const int len = (int)(lex->cur - start);
        return make_error(LEX_TRAILING_UNDERSCORE,
                          start_line, start_col, len);
    }

    UToken t = {0};
    t.type = TOK_INT;
    t.line = start_line;
    t.col = start_col;

    /* Check for duration suffix and convert to microseconds. */
    if (lex->cur + 1 < lex->end && lex->cur[0] == 'm' && lex->cur[1] == 's' &&
        (lex->cur + 2 >= lex->end || !is_ident_cont(lex->cur[2]))) {
        /* "ms" → multiply by 1000 to get microseconds */
        lex->cur += 2;
        value *= 1000;
    }
    else if (lex->cur + 1 < lex->end && lex->cur[0] == 'u' && lex->cur[1] == 's' &&
             (lex->cur + 2 >= lex->end || !is_ident_cont(lex->cur[2]))) {
        /* "us" → already in microseconds */
        lex->cur += 2;
    }
    else if (lex->cur + 1 < lex->end && lex->cur[0] == 'n' && lex->cur[1] == 's' &&
             (lex->cur + 2 >= lex->end || !is_ident_cont(lex->cur[2]))) {
        /* "ns" → divide by 1000 (with truncation) to get microseconds */
        lex->cur += 2;
        value /= 1000;
    }
    else if (lex->cur < lex->end && lex->cur[0] == 's' &&
             (lex->cur + 1 >= lex->end || !is_ident_cont(lex->cur[1]))) {
        /* "s" → multiply by 1,000,000 to get microseconds */
        lex->cur += 1;
        value *= 1000000;
    }
    else if (lex->cur < lex->end && lex->cur[0] == 'm' &&
             (lex->cur + 1 >= lex->end || !is_ident_cont(lex->cur[1]))) {
        /* "m" → multiply by 60,000,000 to get microseconds */
        lex->cur += 1;
        value *= 60LL * 1000000;
    }
    else if (lex->cur < lex->end && lex->cur[0] == 'h' &&
             (lex->cur + 1 >= lex->end || !is_ident_cont(lex->cur[1]))) {
        /* "h" → multiply by 3,600,000,000 to get microseconds */
        lex->cur += 1;
        value *= 3600LL * 1000000;
    }
    else if (lex->cur < lex->end && lex->cur[0] == 'd' &&
             (lex->cur + 1 >= lex->end || !is_ident_cont(lex->cur[1]))) {
        /* "d" → multiply by 86,400,000,000 to get microseconds */
        lex->cur += 1;
        value *= 86400LL * 1000000;
    }

    /* Update token length if a suffix was consumed. */
    t.len = (int)(lex->cur - start);
    t.u.i = value;

    return t;
}

static int is_ident_start(const char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_';
}

static int is_ident_cont(const char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

typedef struct {
    const char *name;
    int         len;
    UTokenType  type;
} UKeyword;

/* Sorted by name for human readability; lookup is linear (15 entries —
 * faster than a hash for this size). */
static const UKeyword KEYWORDS[] = {
    { "async",     5, TOK_KW_ASYNC    },
    { "at",        2, TOK_KW_AT       },
    { "catch",     5, TOK_KW_CATCH    },
    { "closure",   7, TOK_KW_CLOSURE  },
    { "else",      4, TOK_KW_ELSE     },
    { "false",     5, TOK_KW_FALSE    },
    { "finally",   7, TOK_KW_FINALLY  },
    { "function",  8, TOK_KW_FUNCTION },
    { "if",        2, TOK_KW_IF       },
    { "lazy",      4, TOK_KW_LAZY     },
    { "nil",       3, TOK_KW_NIL      },
    { "onleave",   7, TOK_KW_ONLEAVE  },
    { "return",    6, TOK_KW_RETURN   },
    { "sync",      4, TOK_KW_SYNC     },
    { "throw",     5, TOK_KW_THROW    },
    { "true",      4, TOK_KW_TRUE     },
    { "try",       3, TOK_KW_TRY      },
    { "var",       3, TOK_KW_VAR      },
    { "waituntil", 9, TOK_KW_WAITUNTIL},
    { "whenever",  8, TOK_KW_WHENEVER },
    { "while",     5, TOK_KW_WHILE    }
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

    UToken t = {0};
    t.type = kw_type;
    t.line = start_line;
    t.col = start_col;
    t.len = len;
    /* Populate u.str for both keywords and identifiers; useful for
     * downstream diagnostics that need to quote the lexeme. */
    t.u.str.start = start;
    t.u.str.len = len;
    return t;
}

void ulex_init(ULexer *lex, const char *src, const size_t len) {
    lex->src = src;
    lex->end = src + len;
    lex->cur = src;
    lex->line = 1;
    lex->line_start = src;
}

static UToken make_eof(const ULexer *l) {
    UToken t = {0};
    t.type = TOK_EOF;
    t.line = l->line;
    t.col = (int)(l->cur - l->line_start) + 1;
    return t;
}

typedef struct {
    ULexError code;
    int line;
    int col;
} UTriviaResult;

static UTriviaResult skip_trivia(ULexer *l) {
    UTriviaResult r = {LEX_OK, 0, 0};
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
            /* Line comment — skip to LF or EOF. */
            l->cur += 2;
            while (l->cur < l->end && *l->cur != '\n') {
                l->cur++;
            }
        } else if (c == '/' && l->cur + 1 < l->end && l->cur[1] == '*') {
            /* Block comment — record start for error reporting. */
            const int start_line = l->line;
            const int start_col = (int)(l->cur - l->line_start) + 1;
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
                return r;
            }
        } else {
            break;
        }
    }
    return r;
}

UToken ulex_next(ULexer *lex) {
    UTriviaResult tr = skip_trivia(lex);
    if (tr.code != LEX_OK) {
        return make_error(tr.code, tr.line, tr.col, 2);
    }
    if (lex->cur >= lex->end) {
        return make_eof(lex);
    }

    const char *start = lex->cur;
    const char c = *lex->cur;
    switch (c) {
    case '+': lex->cur++; return make_tok(lex, TOK_PLUS,   start, 1);
    case '-':
        if (lex->cur + 1 < lex->end && lex->cur[1] == '>') {
            lex->cur += 2;
            return make_tok(lex, TOK_ARROW, start, 2);
        }
        lex->cur++;
        return make_tok(lex, TOK_MINUS,  start, 1);
    case '*': lex->cur++; return make_tok(lex, TOK_STAR,   start, 1);
    case '/': lex->cur++; return make_tok(lex, TOK_SLASH,  start, 1);
    case '(': lex->cur++; return make_tok(lex, TOK_LPAREN, start, 1);
    case ')': lex->cur++; return make_tok(lex, TOK_RPAREN, start, 1);
    case '|': lex->cur++; return make_tok(lex, TOK_PIPE,   start, 1);
    case ';': lex->cur++; return make_tok(lex, TOK_SEMI,   start, 1);
    case ',': lex->cur++; return make_tok(lex, TOK_COMMA,  start, 1);
    case '&': lex->cur++; return make_tok(lex, TOK_AMP,    start, 1);
    case '{': lex->cur++; return make_tok(lex, TOK_LBRACE, start, 1);
    case '}': lex->cur++; return make_tok(lex, TOK_RBRACE, start, 1);
    case ':': lex->cur++; return make_tok(lex, TOK_COLON,  start, 1);
    case '.': lex->cur++; return make_tok(lex, TOK_DOT,    start, 1);
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
        {
            const int col = (int)(lex->cur - lex->line_start) + 1;
            const int line = lex->line;
            lex->cur++;
            return make_error(LEX_UNKNOWN_CHAR, line, col, 1);
        }
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
            return scan_decimal(lex);
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
