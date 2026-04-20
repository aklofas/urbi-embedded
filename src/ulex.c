/* SPDX-License-Identifier: BSD-3-Clause */
/* Lexer. */

#include "ulex.h"

#include <limits.h>
#include <string.h>

static const char *TOKEN_NAMES[] = {
    "TOK_EOF", "TOK_INT", "TOK_IDENT",
    "TOK_PLUS", "TOK_MINUS", "TOK_STAR", "TOK_SLASH",
    "TOK_LPAREN", "TOK_RPAREN", "TOK_PIPE",
    "TOK_ERROR"
};

static const char *ERR_MSG[] = {
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

static Token make_error(const Lexer *l, LexErrorCode code,
                       int line, int col, int len) {
    Token t;
    memset(&t, 0, sizeof(t));
    t.type = TOK_ERROR;
    t.line = line;
    t.col = col;
    t.len = len;
    t.u.err.code = code;
    t.u.err.message = ERR_MSG[code];
    (void)l;
    return t;
}

static Token make_tok(const Lexer *l, TokenType type,
                     const char *start, int len) {
    Token t;
    memset(&t, 0, sizeof(t));
    t.type = type;
    t.line = l->line;
    t.col = (int)(start - l->line_start) + 1;
    t.len = len;
    return t;
}

static int digit_value(char c, int base) {
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else return -1;
    return v < base ? v : -1;
}

/* Accumulate one digit into *acc, returning 0 on overflow. */
static int acc_digit(int64_t *acc, int digit, int base) {
    if (*acc > (INT64_MAX - digit) / base) return 0;
    *acc = *acc * base + digit;
    return 1;
}

/* Scan a radix-prefixed integer. lex->cur points at the first char after
   the prefix; start points at the '0' of the prefix; prefix_len is 2.
   base is 16/2/8; malformed code is the base-appropriate LEX_MALFORMED_*. */
static Token scan_radix(Lexer *lex, const char *start, int base,
                        LexErrorCode malformed_code) {
    int start_col = (int)(start - lex->line_start) + 1;
    int start_line = lex->line;

    /* Must have at least one digit or underscore. */
    if (lex->cur >= lex->end) {
        return make_error(lex, LEX_EMPTY_RADIX, start_line, start_col, 2);
    }
    char c0 = *lex->cur;
    if (c0 == '_') {
        /* leading underscore after prefix */
        while (lex->cur < lex->end &&
               (digit_value(*lex->cur, base) >= 0 || *lex->cur == '_')) {
            lex->cur++;
        }
        int len = (int)(lex->cur - start);
        return make_error(lex, LEX_LEADING_UNDERSCORE,
                          start_line, start_col, len);
    }
    if (digit_value(c0, base) < 0) {
        /* Either EMPTY_RADIX (followed by non-alpha/digit-ish non-continuation)
           or MALFORMED — distinguish by whether the char looks like it was
           trying to be a digit. Any [0-9a-zA-Z] that isn't valid for this
           base = MALFORMED; anything else = EMPTY_RADIX. */
        int looks_digitish =
            (c0 >= '0' && c0 <= '9') ||
            (c0 >= 'a' && c0 <= 'z') ||
            (c0 >= 'A' && c0 <= 'Z');
        if (looks_digitish) {
            /* Consume the bad digit-ish char so we advance. */
            lex->cur++;
            return make_error(lex, malformed_code,
                              start_line, start_col, 3);
        }
        return make_error(lex, LEX_EMPTY_RADIX,
                          start_line, start_col, 2);
    }

    int64_t value = 0;
    char prev = 0;
    while (lex->cur < lex->end) {
        char c = *lex->cur;
        if (c == '_') {
            if (prev == '_') {
                int len = (int)(lex->cur - start) + 1;
                return make_error(lex, LEX_ADJACENT_UNDERSCORES,
                                  start_line, start_col, len);
            }
            prev = '_';
            lex->cur++;
            continue;
        }
        int d = digit_value(c, base);
        if (d < 0) break;
        if (!acc_digit(&value, d, base)) {
            while (lex->cur < lex->end &&
                   (digit_value(*lex->cur, base) >= 0 || *lex->cur == '_')) {
                lex->cur++;
            }
            int len = (int)(lex->cur - start);
            return make_error(lex, LEX_INT_OVERFLOW,
                              start_line, start_col, len);
        }
        prev = c;
        lex->cur++;
    }

    if (prev == '_') {
        int len = (int)(lex->cur - start);
        return make_error(lex, LEX_TRAILING_UNDERSCORE,
                          start_line, start_col, len);
    }

    Token t;
    memset(&t, 0, sizeof(t));
    t.type = TOK_INT;
    t.line = start_line;
    t.col = start_col;
    t.len = (int)(lex->cur - start);
    t.u.i = value;
    return t;
}

/* Scan a decimal integer starting at lex->cur.
   Caller has confirmed *lex->cur is a decimal digit. */
static Token scan_decimal(Lexer *lex) {
    const char *start = lex->cur;
    int start_col = (int)(start - lex->line_start) + 1;
    int start_line = lex->line;

    /* Radix-prefix dispatch on a leading '0'. */
    if (*start == '0' && lex->cur + 1 < lex->end) {
        char c2 = lex->cur[1];
        if (c2 == 'x' || c2 == 'X') {
            lex->cur += 2;
            return scan_radix(lex, start, 16, LEX_MALFORMED_HEX);
        }
        /* 0b and 0o arrive in later commits. */
        if ((c2 >= '0' && c2 <= '9') || c2 == '_') {
            /* Consume the leading-zero sequence so caller advances. */
            lex->cur++;
            while (lex->cur < lex->end &&
                   ((*lex->cur >= '0' && *lex->cur <= '9') || *lex->cur == '_')) {
                lex->cur++;
            }
            int len = (int)(lex->cur - start);
            return make_error(lex, LEX_AMBIGUOUS_LEADING_ZERO,
                              start_line, start_col, len);
        }
    }

    int64_t value = 0;
    char prev = 0;
    int digit_seen = 0;
    while (lex->cur < lex->end) {
        char c = *lex->cur;
        if (c == '_') {
            if (!digit_seen) {
                /* Underscore before any digit — should never reach here
                   for a leading '_' since '_' doesn't dispatch to scan_decimal. */
                return make_error(lex, LEX_LEADING_UNDERSCORE,
                                  start_line, start_col, 1);
            }
            if (prev == '_') {
                int len = (int)(lex->cur - start) + 1;
                return make_error(lex, LEX_ADJACENT_UNDERSCORES,
                                  start_line, start_col, len);
            }
            prev = '_';
            lex->cur++;
            continue;
        }
        int d = digit_value(c, 10);
        if (d < 0) break;
        if (!acc_digit(&value, d, 10)) {
            while (lex->cur < lex->end &&
                   (digit_value(*lex->cur, 10) >= 0 || *lex->cur == '_')) {
                lex->cur++;
            }
            int len = (int)(lex->cur - start);
            return make_error(lex, LEX_INT_OVERFLOW,
                              start_line, start_col, len);
        }
        prev = c;
        digit_seen = 1;
        lex->cur++;
    }

    if (prev == '_') {
        int len = (int)(lex->cur - start);
        return make_error(lex, LEX_TRAILING_UNDERSCORE,
                          start_line, start_col, len);
    }

    Token t;
    memset(&t, 0, sizeof(t));
    t.type = TOK_INT;
    t.line = start_line;
    t.col = start_col;
    t.len = (int)(lex->cur - start);
    t.u.i = value;
    return t;
}

void ulex_init(Lexer *lex, const char *src, size_t len) {
    lex->src = src;
    lex->end = src + len;
    lex->cur = src;
    lex->line = 1;
    lex->line_start = src;
}

static Token make_eof(const Lexer *l) {
    Token t;
    memset(&t, 0, sizeof(t));
    t.type = TOK_EOF;
    t.line = l->line;
    t.col = (int)(l->cur - l->line_start) + 1;
    return t;
}

typedef struct {
    LexErrorCode code;
    int line;
    int col;
} TriviaResult;

static TriviaResult skip_trivia(Lexer *l) {
    TriviaResult r = {LEX_OK, 0, 0};
    while (l->cur < l->end) {
        char c = *l->cur;
        if (c == ' ' || c == '\t') {
            l->cur++;
        } else if (c == '\n') {
            l->cur++;
            l->line++;
            l->line_start = l->cur;
        } else if (c == '\r') {
            l->cur++;
            if (l->cur < l->end && *l->cur == '\n') {
                l->cur++;
            }
            l->line++;
            l->line_start = l->cur;
        } else if (c == '/' && l->cur + 1 < l->end && l->cur[1] == '/') {
            /* Line comment — skip to LF or EOF. */
            l->cur += 2;
            while (l->cur < l->end && *l->cur != '\n') {
                l->cur++;
            }
        } else if (c == '/' && l->cur + 1 < l->end && l->cur[1] == '*') {
            /* Block comment — record start for error reporting. */
            int start_line = l->line;
            int start_col = (int)(l->cur - l->line_start) + 1;
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

Token ulex_next(Lexer *lex) {
    TriviaResult tr = skip_trivia(lex);
    if (tr.code != LEX_OK) {
        return make_error(lex, tr.code, tr.line, tr.col, 2);
    }
    if (lex->cur >= lex->end) {
        return make_eof(lex);
    }

    const char *start = lex->cur;
    char c = *lex->cur;
    switch (c) {
    case '+': lex->cur++; return make_tok(lex, TOK_PLUS,   start, 1);
    case '-': lex->cur++; return make_tok(lex, TOK_MINUS,  start, 1);
    case '*': lex->cur++; return make_tok(lex, TOK_STAR,   start, 1);
    case '/': lex->cur++; return make_tok(lex, TOK_SLASH,  start, 1);
    case '(': lex->cur++; return make_tok(lex, TOK_LPAREN, start, 1);
    case ')': lex->cur++; return make_tok(lex, TOK_RPAREN, start, 1);
    case '|': lex->cur++; return make_tok(lex, TOK_PIPE,   start, 1);
    default:
        if (c >= '0' && c <= '9') {
            return scan_decimal(lex);
        }
        lex->cur++;
        return make_eof(lex);
    }
}

const char *ulex_token_name(TokenType t) {
    if ((unsigned)t >= sizeof(TOKEN_NAMES) / sizeof(TOKEN_NAMES[0])) {
        return "TOK_UNKNOWN";
    }
    return TOKEN_NAMES[t];
}
