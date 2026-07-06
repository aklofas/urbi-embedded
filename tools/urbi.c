/* SPDX-License-Identifier: BSD-3-Clause */
/* urbi — the host REPL binary.  Host-only. */

/* Enable POSIX interfaces: clock_gettime, struct timespec, fileno, isatty. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "value/uarena.h"
#include "parse/uast.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "chunk/uchunk.h"
#include "parse/uparse.h"
#include "urbi/urbi.h"
#include "urbi/trace.h"
#include "value/uvalue.h"
#include "vm/uvm.h"

#include "linenoise.h"

#if defined(URBI_ENABLE_REPL)
#  include "urbi/repl.h"
#  include "repl/urepl_transport_tcp.h"
#endif

/* --- helpers --- */

static void print_usage(FILE *out) {
    fputs(
        "Usage: urbi [options] [file]\n"
        "\n"
        "Modes:\n"
        "  (no args)            interactive REPL if stdin is a terminal,\n"
        "                       otherwise read stdin as a source file\n"
        "  -i                   force interactive mode\n"
        "  -e <expr>            evaluate <expr> and print the result\n"
        "  -f <file>            run <file> as a source script\n"
        "  <file>               run <file> as a source script (positional)\n"
        "  --dump-bytecode      print disassembly instead of running\n"
        "                       (combine with -e <expr> or a file)\n"
        "  --dump-wire-format   print on-disk serialized wire-format bytes (raw binary)\n"
        "                       to stdout instead of running (combine with -e or file)\n"
        "\n"
        "Options:\n"
        "  --version, -V        print version and exit\n"
        "  --help, -h           print this help and exit\n"
        "  --trace=SPEC         enable trace channels (chan:level,...; e.g.\n"
        "                       sched:debug,gc:info or all:info) — needs a\n"
        "                       URBI_TRACE=1 build (make urbi-trace)\n"
        "  --trace-out=FILE     write the binary trace dump to FILE (default\n"
        "                       urbi-trace.bin); decode with tools/urbi-trace-decode.py\n"
        "  --dump-on-fatal      on a fatal run, print a host-side dump (trace\n"
        "                       tail + Debug.coros/gc) to stderr\n"
#if defined(URBI_ENABLE_REPL)
        "  --listen [ADDR:]PORT also serve NDJSON REPL on the given TCP\n"
        "                       socket (interactive mode only)\n"
        "  --token TOK          bearer token for --listen\n"
        "                       (env URBI_REPL_TOKEN also consulted)\n"
#endif
        "\n"
        "Exit status:\n"
        "  0   success\n"
        "  1   runtime or compile error\n"
        "  2   usage error (bad flag, missing file)\n",
        out);
}

static int eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* --- trace capture (v0.11.2) ---
 * --trace=SPEC enables channels and --trace-out=FILE receives a URBT binary
 * dump at exit.  Globals exist in both build modes so the arg parser is
 * uniform; the ring-touching code is behind URBI_TRACE so a trace-off build
 * has no dead references and --trace fails with a friendly message. */
static const char *trace_spec     = NULL;            /* "chan:level[,...]" or NULL */
static const char *trace_out_path = "urbi-trace.bin";
static int          want_dump_on_fatal = 0;          /* --dump-on-fatal */

#if URBI_TRACE
static int8_t parse_level(const char *s, size_t n) {
    if (n == 5 && strncmp(s, "debug", 5) == 0) return (int8_t)URBI_LOG_DEBUG;
    if (n == 4 && strncmp(s, "info",  4) == 0) return (int8_t)URBI_LOG_INFO;
    if (n == 4 && strncmp(s, "warn",  4) == 0) return (int8_t)URBI_LOG_WARN;
    if (n == 5 && strncmp(s, "error", 5) == 0) return (int8_t)URBI_LOG_ERROR;
    return (int8_t)-2;  /* invalid (URBI_TRACE_OFF is -1) */
}

/* Resolve a channel name to its index via urbi_trace_channel_name(). */
static int parse_channel(const char *s, size_t n) {
    uint8_t ch;
    for (ch = 0; ch < URBI_TRACE_CHANNEL_MAX; ch++) {
        const char *nm = urbi_trace_channel_name(ch);
        if (strlen(nm) == n && strncmp(nm, s, n) == 0) return (int)ch;
    }
    return -1;
}

/* Apply "chan:level[,chan:level...]" (and "all:level") to vm. 0 on success. */
static int apply_trace_spec(UVM *vm, const char *spec) {
    const char *p = spec;
    while (*p) {
        const char *colon = strchr(p, ':');
        const char *comma;
        size_t cn, ln;
        int8_t lvl;
        if (!colon) { fprintf(stderr, "urbi: --trace: expected chan:level near '%s'\n", p); return 2; }
        cn = (size_t)(colon - p);
        comma = strchr(colon + 1, ',');
        ln = comma ? (size_t)(comma - colon - 1) : strlen(colon + 1);
        lvl = parse_level(colon + 1, ln);
        if (lvl == (int8_t)-2) { fprintf(stderr, "urbi: --trace: bad level\n"); return 2; }
        if (cn == 3 && strncmp(p, "all", 3) == 0) {
            urbi_trace_set_level_all(vm, lvl);
        } else {
            int ch = parse_channel(p, cn);
            if (ch < 0) { fprintf(stderr, "urbi: --trace: unknown channel\n"); return 2; }
            urbi_trace_set_level(vm, (uint8_t)ch, lvl);
        }
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

/* Drain the whole ring to a URBT dump file. 0 on success. */
static int write_urbt_dump(UVM *vm, const char *path) {
    FILE *f;
    UTraceRecord buf[64];
    uint32_t dropped = 0, total_dropped = 0;
    size_t n, total = 0;
    UTraceStats st;
    f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "urbi: --trace-out: cannot open %s\n", path); return 1; }
    {
        unsigned char hdr[20] = {0};   /* placeholder; rewritten once count is known */
        if (fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr) goto werr;
    }
    while ((n = urbi_trace_snapshot(vm, buf, 64, &dropped)) > 0) {
        total_dropped += dropped;
        if (fwrite(buf, sizeof(UTraceRecord), n, f) != n) goto werr;
        total += n;
        if (n < 64) break;
    }
    {
        unsigned char hdr[20];
        uint32_t count = (uint32_t)total;
        uint32_t flags = 1u;            /* bit0: little-endian payload */
        memcpy(hdr + 0, "URBT", 4);
        hdr[4] = 1;  hdr[5] = 0;        /* format_version = 1 (u16 LE) */
        hdr[6] = (unsigned char)(sizeof(UTraceRecord) & 0xFF);          /* record_bytes (u16 LE) */
        hdr[7] = (unsigned char)((sizeof(UTraceRecord) >> 8) & 0xFF);
        memcpy(hdr + 8,  &count, 4);
        memcpy(hdr + 12, &total_dropped, 4);
        memcpy(hdr + 16, &flags, 4);
        if (fseek(f, 0, SEEK_SET) != 0) goto werr;
        if (fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr) goto werr;
    }
    fclose(f);
    urbi_trace_stats(vm, &st);
    fprintf(stderr, "urbi: wrote %zu trace records to %s (emitted %u, dropped %u, high-water %u)\n",
            total, path, st.emitted, total_dropped, st.high_water);
    return 0;
werr:
    fprintf(stderr, "urbi: --trace-out: write error\n");
    fclose(f);
    return 1;
}
#endif /* URBI_TRACE */

/* Apply --trace before a run. Returns 0 to proceed, or a nonzero exit code. */
static int trace_begin(UVM *vm) {
    if (!trace_spec) return 0;
#if URBI_TRACE
    return apply_trace_spec(vm, trace_spec);
#else
    (void)vm;
    fprintf(stderr, "urbi: --trace requires a build with URBI_TRACE=1 (try: make urbi-trace)\n");
    return 2;
#endif
}

/* Drain the ring to the dump file after a run (no-op without --trace). */
static void trace_end(UVM *vm) {
#if URBI_TRACE
    if (trace_spec) (void)write_urbt_dump(vm, trace_out_path);
#else
    (void)vm;
#endif
}

/* Best-effort host-side dump after a fatal run.  The script-independent dump
 * is GDB's `urbi-dump` (tools/gdb/urbi.py) on a halted target / core; this is
 * the hosted convenience path, composed from existing public surface only —
 * no new C symbol.  When --trace is also active the full ring goes to the
 * dump file, so here we just point at it rather than draining destructively. */
static void emergency_dump(UVM *vm) {
    fprintf(stderr, "=== urbi emergency dump ===\n");
#if URBI_TRACE
    {
        UTraceStats st;
        urbi_trace_stats(vm, &st);
        fprintf(stderr, "trace: emitted %u dropped %u high-water %u depth %u\n",
                st.emitted, st.dropped, st.high_water, st.ring_depth);
        if (trace_spec) {
            fprintf(stderr, "trace tail: (full dump written to %s)\n", trace_out_path);
        } else {
            UTraceRecord buf[16];
            uint32_t dropped = 0;
            size_t n, i;
            char line[160];
            fprintf(stderr, "trace tail:\n");
            while ((n = urbi_trace_snapshot(vm, buf, 16, &dropped)) > 0) {
                for (i = 0; i < n; i++) {
                    (void)utrace_format(line, sizeof line, &buf[i]);
                    fprintf(stderr, "  %s\n", line);
                }
                if (n < 16) break;
            }
        }
    }
#endif
#if !defined(URBI_BYTECODE_ONLY)
    {
        char out[2048];
        struct URealm *r = urbi_realm_global(vm);
        if (r) {
            if (urbi_repl_eval(vm, r, "Debug.coros()", 13, out, sizeof out) == 0)
                fprintf(stderr, "coros: %s\n", out);
            if (urbi_repl_eval(vm, r, "Debug.gc()", 10, out, sizeof out) == 0)
                fprintf(stderr, "gc:    %s\n", out);
        }
    }
#endif
    fprintf(stderr, "=== end dump ===\n");
}

/* Compile src (length len) into *out_module.
   Returns true on success; caller owns the module and must call
   uchunk_destroy + uarena_destroy when done.
   On failure, writes the formatted error message (no trailing newline) into
   err_buf (up to err_cap bytes, NUL-terminated), destroys internal state,
   and returns false; caller must NOT call uchunk_destroy / uarena_destroy. */
static bool compile_source(const char *src, size_t len, const char *src_name,
                           UVM *vm, UProto *out_module, UArena *arena,
                           char *err_buf, size_t err_cap) {
    ULexer lex;
    ulex_init(&lex, src, len);

    uarena_init(arena, 4096);

    *out_module = (UProto){0};
    UEmitter e;
    uemit_init(&e, out_module, arena, vm, src_name);

    UParser p;
    uparse_init(&p, &lex, arena);

    UAstNode *node;
    bool had_error = false;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) {
            const char *msg = node->u.err.message ? node->u.err.message : "parse error";
            snprintf(err_buf, err_cap, "%s:%d:%d: %s", src_name, node->line, node->col, msg);
            had_error = true;
            break;
        }
        (void)uemit_statement(&e, node);
        uarena_reset(arena);
    }

    if (had_error) {
        urbi_emit_diag_free_all(&e);
        urbi_emit_abandon(&e);   /* parse error — finish never runs (FE-07) */
        uchunk_destroy(out_module, vm);
        uarena_destroy(arena);
        return false;
    }

    if (uemit_finish(&e) != EMIT_OK) {
        char diag_msg[256];
        if (!urbi_emit_diag_format_first_error(&e, diag_msg, sizeof diag_msg)) {
            snprintf(diag_msg, sizeof diag_msg, "%s: emit error: %s",
                     src_name, uemit_error_name(e.error));
        }
        snprintf(err_buf, err_cap, "%s", diag_msg);
        urbi_emit_diag_free_all(&e);
        uchunk_destroy(out_module, vm);
        uarena_destroy(arena);
        return false;
    }

    /* Display any accumulated warnings. */
    {
        int di;
        for (di = 0; di < e.diag_count; di++) {
            if (e.diag_buf[di].level == UEMIT_DIAG_WARN)
                fprintf(stderr, "%s:%d:%d: warning: %s\n", src_name,
                        e.diag_buf[di].line, e.diag_buf[di].col,
                        e.diag_buf[di].message);
        }
    }
    urbi_emit_diag_free_all(&e);
    return true;
}

/* Compile src and print disassembly to stdout.  Does not execute.
   Returns 0 on success, 1 on compile error. */
static int run_dump(UVM *vm, const char *src, size_t len, const char *src_name) {
    UArena arena;
    UProto module;  /* stack is safe: compile-only, never executed — no closure
                       can capture a proto pointer past this frame (VM-11). */
    char err[256] = {0};
    if (!compile_source(src, len, src_name, vm, &module, &arena, err, sizeof err)) {
        fprintf(stderr, "urbi: %s\n", err);
        return 1;
    }
    /* 4 KiB is adequate for initial use — the 8-opcode VM produces tiny
       disassemblies. Revisit (promote to a heap allocation) when
       programs exceed this. */
    char buf[4096];
    size_t n = uemit_disassemble(&module, buf, sizeof buf);
    fwrite(buf, 1, n, stdout);
    if (n > 0 && buf[n - 1] != '\n') fputc('\n', stdout);
    uchunk_destroy(&module, vm);
    uarena_destroy(&arena);
    return 0;
}

/* Compile src and write the on-disk wire-format bytes to stdout (raw binary).
   Used by tests/scripts/capture_wire_format_hashes.sh to hash the genuine
   wire-format shape, complementing capture_bytecode_hashes.sh which hashes
   the disassembled mnemonic text.  Returns 0 on success, 1 on compile or
   serialize error. */
static int run_dump_wire_format(UVM *vm, const char *src, size_t len,
                                const char *src_name) {
    UArena arena;
    UProto module;  /* stack is safe: compile-only, never executed (VM-11). */
    char err[256] = {0};
    if (!compile_source(src, len, src_name, vm, &module, &arena, err, sizeof err)) {
        fprintf(stderr, "urbi: %s\n", err);
        return 1;
    }
    /* First-pass: query required size.  uchunk_serialize returns a negative
       value on failure (-(ptrdiff_t)UChunkLoadError code). */
    ptrdiff_t need = uchunk_serialize(&module, NULL, 0);
    if (need < 0) {
        fprintf(stderr, "urbi: serialize size-query failed: %ld\n", (long)-need);
        uchunk_destroy(&module, vm);
        uarena_destroy(&arena);
        return 1;
    }
    uint8_t *buf = malloc((size_t)need);
    if (!buf) {
        fprintf(stderr, "urbi: out of memory\n");
        uchunk_destroy(&module, vm);
        uarena_destroy(&arena);
        return 1;
    }
    ptrdiff_t wrote = uchunk_serialize(&module, buf, (size_t)need);
    if (wrote != need) {
        fprintf(stderr, "urbi: serialize wrote %ld, expected %ld\n",
                (long)wrote, (long)need);
        free(buf);
        uchunk_destroy(&module, vm);
        uarena_destroy(&arena);
        return 1;
    }
    fwrite(buf, 1, (size_t)need, stdout);
    free(buf);
    uchunk_destroy(&module, vm);
    uarena_destroy(&arena);
    return 0;
}

#define URBI_REPL_MAX_FILE (1024U * 1024U)

/* Slurp a file (or stdin, with path=="-") into a freshly-malloc'd buffer.
   Returns pointer on success, NULL on error (message printed to stderr).
   *out_len receives the byte count. */
static char *slurp(const char *path, size_t *out_len) {
    FILE *fp = NULL;
    if (strcmp(path, "-") == 0) {
        fp = stdin;
    } else {
        fp = fopen(path, "rb");
        if (!fp) {
            fprintf(stderr, "urbi: cannot open %s\n", path);
            return NULL;
        }
    }

    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { fprintf(stderr, "urbi: out of memory\n"); goto fail; }

    for (;;) {
        if (len + 4096 > cap) {
            if (cap >= URBI_REPL_MAX_FILE) {
                fprintf(stderr, "urbi: %s exceeds %u byte cap\n",
                        path, URBI_REPL_MAX_FILE);
                goto fail;
            }
            size_t ncap = cap * 2;
            if (ncap > URBI_REPL_MAX_FILE) ncap = URBI_REPL_MAX_FILE;
            char *n = realloc(buf, ncap);
            if (!n) { fprintf(stderr, "urbi: out of memory\n"); goto fail; }
            buf = n;
            cap = ncap;
        }
        size_t r = fread(buf + len, 1, cap - len, fp);
        len += r;
        if (r == 0) break;
    }

    if (fp != stdin) fclose(fp);
    *out_len = len;
    return buf;

fail:
    free(buf);
    if (fp && fp != stdin) fclose(fp);
    return NULL;
}

/* cli_sleep_until_us — sleep until the monotonic deadline wake_us (µs).
 * Uses the same CLOCK_MONOTONIC source as the VM's host_time_us default.
 * Capped at 2 s so the loop re-polls at 2 s granularity. */
static void cli_sleep_until_us(uint64_t wake_us) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_us = (uint64_t)ts.tv_sec * 1000000ULL
                    + (uint64_t)ts.tv_nsec / 1000ULL;
    if (wake_us <= now_us) return;
    uint64_t delta = wake_us - now_us;
    if (delta > 2000000ULL) delta = 2000000ULL;  /* 2 s cap */
    struct timespec req = {
        .tv_sec  = (time_t)(delta / 1000000ULL),
        .tv_nsec = (long)((delta % 1000000ULL) * 1000ULL)
    };
    nanosleep(&req, NULL);
}

/* Drive forked strands and timers to quiescence after the root chunk
 * ran.  Honors WAKE_AT deadlines with real sleeps; a fatal in any
 * strand maps to a strand-fatal return for the caller's error print. */
static int cli_drive_to_quiescence(UVM *vm, int vrc) {
    if (vrc != URBI_OK) return vrc;
    for (;;) {
        uint64_t wake_us = 0;
        int sr = urbi_step(vm, 65536ULL, &wake_us);
        if (sr == URBI_STEP_RUNNING) continue;
        if (sr == URBI_STEP_WAKE_AT) { cli_sleep_until_us(wake_us); continue; }
        if (sr == URBI_STEP_FATAL) vrc = URBI_ERR_STRAND_FATAL;
        break;
    }
    return vrc;
}

/* Run a source file: compile, execute, discard result (scripts produce
   output via side effects). */
static int run_file(UVM *vm, const char *path) {
    size_t len = 0;
    char *src = slurp(path, &len);
    if (!src) return 2;

    UArena arena;
    int rc = 1;
    char err[256] = {0};
    /* CHSTR-027 pattern: heap-allocate the root
     * proto — same rationale as run_expression above. */
    UProto *module = (UProto *)vm->alloc_fn(NULL, sizeof(UProto), vm->alloc_ud);
    if (module == NULL) {
        fprintf(stderr, "urbi: out of memory\n");
        free(src);
        return 1;
    }
    if (compile_source(src, len, path, vm, module, &arena, err, sizeof err)) {
        /* Ownership fields AFTER compile_source — it zero-fills *module. */
        module->alloc_fn       = vm->alloc_fn;
        module->alloc_ud       = vm->alloc_ud;
        module->heap_allocated = true;
        UValue out;
        /* LANG-S06: route through the persistent loader strand so chunk-top
         * & and , forks are legal; urbi_vm_run used a transient strand that
         * rejected OP_FORK_DETACH/OP_FORK_JOIN.  Drive urbi_step until
         * quiescent, honoring WAKE_AT for sleep/timer workloads. */
        int vrc = urbi_run_chunk(vm, NULL, module, &out);
        vrc = cli_drive_to_quiescence(vm, vrc);
        if (vrc == URBI_OK) {
            rc = 0;
        } else {
            const char *msg = vm->last_errmsg[0] ? vm->last_errmsg
                            : (vrc == URBI_ERR_UNCAUGHT_THROW ? "uncaught throw" : "(vm error)");
            fprintf(stderr, "urbi: %s\n", msg);
            rc = 1;
        }
        uchunk_destroy(module, vm);
        uarena_destroy(&arena);
    } else {
        fprintf(stderr, "urbi: %s\n", err);
        /* Compile failed: struct already zeroed by compile_source's own
         * uchunk_destroy; free the raw storage. */
        vm->alloc_fn(module, 0, vm->alloc_ud);
    }
    free(src);
    return rc;
}

/* Run one expression string, print result to stdout, return 0 on success. */
static int run_expression(UVM *vm, const char *expr) {
    /* Append " |" to terminate the statement if the expression doesn't already
       end with a pipe separator. */
    size_t len = strlen(expr);
    char *buf = malloc(len + 3);  /* expr + " |" + NUL */
    if (!buf) {
        fprintf(stderr, "urbi: out of memory\n");
        return 1;
    }
    memcpy(buf, expr, len);

    /* Trim trailing whitespace for the has-pipe check. */
    size_t t = len;
    while (t > 0 && (buf[t - 1] == ' ' || buf[t - 1] == '\t' ||
                     buf[t - 1] == '\n' || buf[t - 1] == '\r')) {
        t--;
    }

    size_t final_len;
    if (t > 0 && buf[t - 1] == '|') {
        buf[len] = '\0';
        final_len = len;
    } else {
        buf[len]     = ' ';
        buf[len + 1] = '|';
        buf[len + 2] = '\0';
        final_len = len + 2;
    }

    UArena arena;
    int rc = 1;
    char err[256] = {0};
    /* CHSTR-027 pattern: the root proto must be
     * heap-allocated — closures created during the run keep proto pointers
     * alive past this frame; uchunk_destroy defers the actual free to the
     * refcount-rescue machinery (vm->rescued_protos) when references remain,
     * and frees immediately when none do. */
    UProto *module = (UProto *)vm->alloc_fn(NULL, sizeof(UProto), vm->alloc_ud);
    if (module == NULL) {
        fprintf(stderr, "urbi: out of memory\n");
        free(buf);
        return 1;
    }
    if (compile_source(buf, final_len, "<expr>", vm, module, &arena, err, sizeof err)) {
        /* Ownership fields AFTER compile_source — it zero-fills *module. */
        module->alloc_fn       = vm->alloc_fn;
        module->alloc_ud       = vm->alloc_ud;
        module->heap_allocated = true;
        UValue out;
        /* LANG-S06: route through the persistent loader strand; same
         * rationale as run_file above (urbi_vm_run transient → fork errors). */
        int vrc = urbi_run_chunk(vm, NULL, module, &out);
        vrc = cli_drive_to_quiescence(vm, vrc);
        if (vrc == URBI_OK) {
            /* 64 bytes fits Int (max 21 chars) and Float (~24 chars).
               early string literals may truncate silently — promote to
               UVALUE_FORMAT_MAX or heap when strings land. */
            char fmt[64];
            uvalue_format(&out, fmt, sizeof fmt);
            puts(fmt);
            rc = 0;
        } else {
            const char *msg = vm->last_errmsg[0] ? vm->last_errmsg
                            : (vrc == URBI_ERR_UNCAUGHT_THROW ? "uncaught throw" : "(vm error)");
            fprintf(stderr, "urbi: %s\n", msg);
            rc = 1;
        }
        uchunk_destroy(module, vm);
        uarena_destroy(&arena);
    } else {
        fprintf(stderr, "urbi: %s\n", err);
        /* Compile failed: compile_source already ran uchunk_destroy on the
         * zero-refcount, heap_allocated=false struct (buffers freed, struct
         * zeroed in place) — only the raw storage remains to free. */
        vm->alloc_fn(module, 0, vm->alloc_ud);
    }
    free(buf);
    return rc;
}

/* --- interactive mode --- */

static struct timespec g_start_time;
static volatile sig_atomic_t g_interrupted = 0;

static uint32_t ms_since_start(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long sec  = (long)(now.tv_sec  - g_start_time.tv_sec);
    long nsec = now.tv_nsec - g_start_time.tv_nsec;
    if (nsec < 0) { sec -= 1; nsec += 1000000000L; }
    uint64_t ms = (uint64_t)sec * 1000ULL + (uint64_t)nsec / 1000000ULL;
    return (uint32_t)ms;
}

static void sigint_handler(int sig) { (void)sig; g_interrupted = 1; }

static char *history_path(void) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) return NULL;
    size_t hlen = strlen(home);
    const char *suffix = "/.urbi_history";
    size_t slen = strlen(suffix);
    char *path = malloc(hlen + slen + 1);
    if (!path) return NULL;
    memcpy(path, home, hlen);
    memcpy(path + hlen, suffix, slen);
    path[hlen + slen] = '\0';
    return path;
}

/* listen_addr_port: "[ADDR:]PORT" or "PORT".  NULL = no network listener.
 * listen_token:    NULL = no auth (loopback only).
 *
 * The local linenoise REPL realm and the network listener's per-session
 * lobby realms are independent — this is the simple v0.9.1 split. */
static int run_interactive(UVM *vm,
                           const char *listen_addr_port,
                           const char *listen_token) {
    clock_gettime(CLOCK_MONOTONIC, &g_start_time);
    signal(SIGINT, sigint_handler);

    struct URealm *repl_realm = urbi_realm_create_repl(vm);
    if (repl_realm == NULL) {
        fprintf(stderr, "OOM creating REPL realm\n");
        return 1;
    }

#if defined(URBI_ENABLE_REPL)
    UReplServer  *listen_server   = NULL;
    UTcpListener *listen_listener = NULL;
    if (listen_addr_port != NULL) {
        UReplConfig cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.bind_addr          = "127.0.0.1";
        cfg.tcp_port           = 54000;
        cfg.max_clients        = 16;
        cfg.output_ringbuf_cap = 64 * 1024;
        cfg.auth_token         = listen_token;

        /* Accept ":PORT", "ADDR:PORT", or bare "PORT". */
        const char *colon = strrchr(listen_addr_port, ':');
        static char host_buf[128];
        if (colon != NULL) {
            size_t hn = (size_t)(colon - listen_addr_port);
            if (hn > 0) {
                if (hn >= sizeof host_buf) hn = sizeof host_buf - 1;
                memcpy(host_buf, listen_addr_port, hn);
                host_buf[hn] = '\0';
                cfg.bind_addr = host_buf;
            }
            cfg.tcp_port = atoi(colon + 1);
        } else {
            cfg.tcp_port = atoi(listen_addr_port);
        }
        if (cfg.tcp_port <= 0) {
            fprintf(stderr, "urbi: --listen: bad port: %s\n", listen_addr_port);
            urbi_realm_destroy(vm, repl_realm);
            return 2;
        }

        int err = 0;
        listen_server = urbi_repl_serve(vm, &cfg, &err);
        if (listen_server == NULL) {
            if (err == URBI_ERR_INSECURE_CONFIG) {
                fprintf(stderr,
                        "urbi: --listen %s refused (non-loopback bind "
                        "without --token / URBI_REPL_TOKEN)\n", cfg.bind_addr);
            } else {
                fprintf(stderr, "urbi: --listen: urbi_repl_serve failed: %d\n", err);
            }
            urbi_realm_destroy(vm, repl_realm);
            return 1;
        }
        listen_listener = urepl_tcp_listener_create(cfg.bind_addr, cfg.tcp_port);
        if (listen_listener == NULL) {
            fprintf(stderr, "urbi: --listen: failed to bind %s:%d\n",
                    cfg.bind_addr, cfg.tcp_port);
            urbi_repl_stop(listen_server);
            urbi_realm_destroy(vm, repl_realm);
            return 1;
        }
        int rrc = urbi_repl_register_transport(listen_server,
                                               &UREPL_TCP_TRANSPORT,
                                               listen_listener);
        if (rrc != URBI_OK) {
            fprintf(stderr, "urbi: --listen: register_transport failed: %d\n", rrc);
            urepl_tcp_listener_destroy(listen_listener);
            urbi_repl_stop(listen_server);
            urbi_realm_destroy(vm, repl_realm);
            return 1;
        }
        fprintf(stderr, "urbi: --listen %s:%u%s\n",
                cfg.bind_addr, (unsigned)listen_listener->port,
                (cfg.auth_token && cfg.auth_token[0]) ? " (auth required)" : " (no auth)");
        fflush(stderr);
    }
#else
    (void)listen_addr_port; (void)listen_token;
#endif

    char *histpath = history_path();
    linenoiseHistorySetMaxLen(1000);
    if (histpath) linenoiseHistoryLoad(histpath);

#if defined(URBI_ENABLE_REPL)
    /* Listen-mode loop: use the multiplexed linenoise API so we can
     * interleave urbi_step() calls.  The dispatch drain hook is wired
     * into urbi_step, so periodic stepping is what lets a remote
     * urbi-send client get a response even while the local user has
     * not yet hit enter. */
    if (listen_server != NULL) {
        struct linenoiseState ls;
        char editbuf[2048];
        if (linenoiseEditStart(&ls, -1, -1, editbuf, sizeof editbuf, "") < 0) {
            fprintf(stderr, "urbi: linenoiseEditStart failed\n");
            urbi_repl_stop(listen_server);
            urepl_tcp_listener_destroy(listen_listener);
            urbi_realm_destroy(vm, repl_realm);
            free(histpath);
            return 1;
        }
        for (;;) {
            if (g_interrupted) { g_interrupted = 0; continue; }

            struct pollfd pfd = { .fd = ls.ifd, .events = POLLIN };
            int pr = poll(&pfd, 1, 25 /* ms */);
            /* Drive the VM on every poll wake so queued NDJSON jobs from
             * network clients dispatch even while the local user types. */
            (void)urbi_step(vm, 1024, NULL);
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (pr == 0) continue;  /* idle tick — keep polling */

            char *line = linenoiseEditFeed(&ls);
            if (line == linenoiseEditMore) continue;
            linenoiseEditStop(&ls);
            if (line == NULL) break;   /* Ctrl-D / Ctrl-C / I/O error */

            if (line[0] != '\0') {
                linenoiseHistoryAdd(line);
                if (histpath) linenoiseHistorySave(histpath);

                size_t ll = strlen(line);
                size_t t  = ll;
                while (t > 0 && (line[t - 1] == ' ' || line[t - 1] == '\t' ||
                                 line[t - 1] == '\r' || line[t - 1] == '\n')) {
                    t--;
                }
                size_t bufcap = ll + 3;
                char *buf = malloc(bufcap);
                if (buf != NULL) {
                    memcpy(buf, line, ll);
                    size_t final_len;
                    if (t > 0 && line[t - 1] == '|') {
                        buf[ll] = '\0';
                        final_len = ll;
                    } else {
                        buf[ll]     = ' ';
                        buf[ll + 1] = '|';
                        buf[ll + 2] = '\0';
                        final_len   = ll + 2;
                    }
                    char result_buf[512] = {0};
                    int eval_rc = urbi_repl_eval(vm, repl_realm, buf, final_len,
                                                 result_buf, sizeof result_buf);
                    if (eval_rc == URBI_OK) {
                        printf("[%08u] %s\n", ms_since_start(), result_buf);
                    } else {
                        const char *msg = result_buf[0] ? result_buf
                                        : (vm->last_errmsg[0] ? vm->last_errmsg : "(vm error)");
                        printf("[%08u] !!! %s\n", ms_since_start(), msg);
                    }
                    fflush(stdout);
                    free(buf);
                }
            }
            linenoiseFree(line);

            /* Re-arm linenoise for the next line. */
            if (linenoiseEditStart(&ls, -1, -1, editbuf, sizeof editbuf, "") < 0) {
                break;
            }
        }
    } else
#endif
    for (;;) {
        if (g_interrupted) { g_interrupted = 0; continue; }

        char *line = linenoise("");
        if (line == NULL) break;   /* Ctrl-D or error */
        if (line[0] == '\0') { free(line); continue; }

        linenoiseHistoryAdd(line);
        if (histpath) linenoiseHistorySave(histpath);

        /* Append " |" if missing. */
        size_t ll = strlen(line);
        size_t t  = ll;
        while (t > 0 && (line[t - 1] == ' ' || line[t - 1] == '\t' ||
                         line[t - 1] == '\r' || line[t - 1] == '\n')) {
            t--;
        }
        size_t bufcap = ll + 3;
        char *buf = malloc(bufcap);
        if (!buf) { free(line); continue; }
        memcpy(buf, line, ll);
        size_t final_len;
        if (t > 0 && line[t - 1] == '|') {
            buf[ll] = '\0';
            final_len = ll;
        } else {
            buf[ll]     = ' ';
            buf[ll + 1] = '|';
            buf[ll + 2] = '\0';
            final_len   = ll + 2;
        }

        /* Use urbi_repl_eval: compiles, runs, drains spawned strands, and
         * formats the result.  The drain workaround previously inlined here
         * is now inside urbi_repl_eval (API-009).
         * 512 bytes: large enough for all parse-error messages (longest is
         * ~200 chars for the 'closure' retirement message + line/col prefix). */
        char result_buf[512] = {0};
        int eval_rc = urbi_repl_eval(vm, repl_realm, buf, final_len,
                                     result_buf, sizeof result_buf);
        if (eval_rc == URBI_OK) {
            printf("[%08u] %s\n", ms_since_start(), result_buf);
        } else {
            const char *msg = result_buf[0] ? result_buf
                            : (vm->last_errmsg[0] ? vm->last_errmsg : "(vm error)");
            printf("[%08u] !!! %s\n", ms_since_start(), msg);
        }
        fflush(stdout);

        free(buf);
        free(line);
    }

    if (histpath) linenoiseHistorySave(histpath);
    free(histpath);
#if defined(URBI_ENABLE_REPL)
    if (listen_server != NULL) {
        urbi_repl_stop(listen_server);
    }
    if (listen_listener != NULL) {
        urepl_tcp_listener_destroy(listen_listener);
    }
#endif
    urbi_realm_destroy(vm, repl_realm);
    return 0;
}

/* --- main --- */

int main(int argc, char *argv[]) {
    /* Handle --version / --help short-circuits. */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (eq(a, "--version") || eq(a, "-V")) {
            printf("urbi %s\n", urbi_version());
            return EXIT_SUCCESS;
        }
        if (eq(a, "--help") || eq(a, "-h")) {
            print_usage(stdout);
            return EXIT_SUCCESS;
        }
    }

    /* Scan for --dump-bytecode and --dump-wire-format; both incompatible with -i. */
    bool dump = false;
    bool dump_wire = false;
    bool want_interactive = false;
    for (int i = 1; i < argc; i++) {
        if (eq(argv[i], "--dump-bytecode")) dump = true;
        if (eq(argv[i], "--dump-wire-format")) dump_wire = true;
        if (eq(argv[i], "-i")) want_interactive = true;
    }

    if ((dump || dump_wire) && want_interactive) {
        fprintf(stderr, "urbi: --dump-bytecode/--dump-wire-format requires -e <expr> or a file\n");
        return 2;
    }
    if (dump && dump_wire) {
        fprintf(stderr, "urbi: --dump-bytecode and --dump-wire-format are mutually exclusive\n");
        return 2;
    }

    /* Scan for --listen [addr:]port and --token TOK (v0.9.1).  Both are
     * interactive-mode-only — they're ignored by -e/-f/dump modes (which
     * exit before reaching the local REPL loop).  Token precedence:
     *   --token flag > URBI_REPL_TOKEN env > NULL (no auth, loopback only).
     *
     * Outside URBI_ENABLE_REPL=1 builds the flags are accepted and rejected
     * with a clear error rather than treated as unknown-option. */
    const char *listen_addr_port = NULL;
    const char *listen_token     = getenv("URBI_REPL_TOKEN");
    for (int i = 1; i < argc; i++) {
        if (eq(argv[i], "--listen") && i + 1 < argc) {
            listen_addr_port = argv[++i];
        } else if (eq(argv[i], "--token") && i + 1 < argc) {
            listen_token = argv[++i];
        }
    }
#if !defined(URBI_ENABLE_REPL)
    if (listen_addr_port != NULL) {
        fprintf(stderr, "urbi: --listen requires URBI_ENABLE_REPL=1 at build time\n");
        return 2;
    }
#endif
    /* --listen implies interactive — the listener thread needs the main
     * thread to drive urbi_step (via urbi_repl_eval inside the linenoise
     * loop). */
    if (listen_addr_port != NULL) {
        want_interactive = true;
    }

    /* Scan for -e. */
    const char *expr = NULL;
    for (int i = 1; i < argc; i++) {
        if (eq(argv[i], "-e")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "urbi: -e requires an argument\n");
                return 2;
            }
            expr = argv[i + 1];
            break;
        }
    }

    /* Scan for -f. */
    const char *file_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (eq(argv[i], "-f")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "urbi: -f requires a path argument\n");
                return 2;
            }
            file_arg = argv[i + 1];
            break;
        }
    }

    /* Scan for --trace=SPEC / --trace-out=FILE (v0.11.2).  Both are the "="
       form (single token), so the positional-file detector below skips them
       (they start with '-').  They apply to the -e/-f/positional/stdin run
       paths; ignored by dump and interactive modes. */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--trace=", 8) == 0) {
            trace_spec = argv[i] + 8;
        } else if (strncmp(argv[i], "--trace-out=", 12) == 0) {
            trace_out_path = argv[i] + 12;
        } else if (eq(argv[i], "--dump-on-fatal")) {
            want_dump_on_fatal = 1;
        }
    }

    /* Positional file: first non-flag argument that isn't an -e/-f value
       nor a --listen/--token value (v0.9.1).  Skips known multi-arg flag
       values so e.g. `urbi --listen :14242` doesn't treat ":14242" as a
       script path. */
    if (!file_arg) {
        for (int i = 1; i < argc; i++) {
            if (eq(argv[i], "--listen") || eq(argv[i], "--token") ||
                eq(argv[i], "-e") || eq(argv[i], "-f")) {
                i++;  /* skip the flag's value */
                continue;
            }
            if (argv[i][0] != '-') {
                file_arg = argv[i];
                break;
            }
        }
    }

    /* --dump-bytecode dispatch: compile and disassemble, no execution. */
    if (dump) {
        if (expr) {
            /* Append " |" to terminate the statement, matching run_expression. */
            size_t len = strlen(expr);
            char *buf = malloc(len + 3);
            if (!buf) { fprintf(stderr, "urbi: out of memory\n"); return 1; }
            memcpy(buf, expr, len);

            size_t t = len;
            while (t > 0 && (buf[t - 1] == ' ' || buf[t - 1] == '\t' ||
                             buf[t - 1] == '\n' || buf[t - 1] == '\r')) t--;

            size_t final_len;
            if (t > 0 && buf[t - 1] == '|') {
                buf[len] = '\0';
                final_len = len;
            } else {
                buf[len]     = ' ';
                buf[len + 1] = '|';
                buf[len + 2] = '\0';
                final_len = len + 2;
            }
            UVM vm;
            urbi_vm_init(&vm, NULL, NULL);
            int rc = run_dump(&vm, buf, final_len, "<expr>");
            urbi_vm_destroy(&vm);
            free(buf);
            return rc;
        }
        if (file_arg) {
            size_t flen = 0;
            char *src = slurp(file_arg, &flen);
            if (!src) return 2;
            UVM vm;
            urbi_vm_init(&vm, NULL, NULL);
            int rc = run_dump(&vm, src, flen, file_arg);
            urbi_vm_destroy(&vm);
            free(src);
            return rc;
        }
        fprintf(stderr, "urbi: --dump-bytecode requires -e <expr> or a file\n");
        return 2;
    }

    /* --dump-wire-format dispatch: compile and serialize raw bytes, no execution. */
    if (dump_wire) {
        if (expr) {
            /* Append " |" to terminate the statement, matching run_expression. */
            size_t len = strlen(expr);
            char *buf = malloc(len + 3);
            if (!buf) { fprintf(stderr, "urbi: out of memory\n"); return 1; }
            memcpy(buf, expr, len);

            size_t t = len;
            while (t > 0 && (buf[t - 1] == ' ' || buf[t - 1] == '\t' ||
                             buf[t - 1] == '\n' || buf[t - 1] == '\r')) t--;

            size_t final_len;
            if (t > 0 && buf[t - 1] == '|') {
                buf[len] = '\0';
                final_len = len;
            } else {
                buf[len]     = ' ';
                buf[len + 1] = '|';
                buf[len + 2] = '\0';
                final_len = len + 2;
            }
            UVM vm;
            urbi_vm_init(&vm, NULL, NULL);
            int rc = run_dump_wire_format(&vm, buf, final_len, "<expr>");
            urbi_vm_destroy(&vm);
            free(buf);
            return rc;
        }
        if (file_arg) {
            size_t flen = 0;
            char *src = slurp(file_arg, &flen);
            if (!src) return 2;
            UVM vm;
            urbi_vm_init(&vm, NULL, NULL);
            int rc = run_dump_wire_format(&vm, src, flen, file_arg);
            urbi_vm_destroy(&vm);
            free(src);
            return rc;
        }
        fprintf(stderr, "urbi: --dump-wire-format requires -e <expr> or a file\n");
        return 2;
    }

    if (expr) {
        UVM vm;
        int tc, rc;
        urbi_vm_init(&vm, NULL, NULL);
        tc = trace_begin(&vm);
        if (tc) { urbi_vm_destroy(&vm); return tc; }
        rc = run_expression(&vm, expr);
        if (want_dump_on_fatal && rc != 0) emergency_dump(&vm);
        trace_end(&vm);
        urbi_vm_destroy(&vm);
        return rc;
    }

    if (file_arg) {
        UVM vm;
        int tc, rc;
        urbi_vm_init(&vm, NULL, NULL);
        tc = trace_begin(&vm);
        if (tc) { urbi_vm_destroy(&vm); return tc; }
        rc = run_file(&vm, file_arg);
        if (want_dump_on_fatal && rc != 0) emergency_dump(&vm);
        trace_end(&vm);
        urbi_vm_destroy(&vm);
        return rc;
    }

    /* No -e, no file: check stdin. */
    if (argc == 1 && !isatty(fileno(stdin))) {
        UVM vm;
        int tc, rc;
        urbi_vm_init(&vm, NULL, NULL);
        tc = trace_begin(&vm);
        if (tc) { urbi_vm_destroy(&vm); return tc; }
        rc = run_file(&vm, "-");
        if (want_dump_on_fatal && rc != 0) emergency_dump(&vm);
        trace_end(&vm);
        urbi_vm_destroy(&vm);
        return rc;
    }

    /* -i explicit or (no args, stdin is a tty). */
    if (want_interactive ||
        (argc == 1 && isatty(fileno(stdin)))) {
        UVM vm;
        urbi_vm_init(&vm, NULL, NULL);
        int rc = run_interactive(&vm, listen_addr_port, listen_token);
        urbi_vm_destroy(&vm);
        return rc;
    }

    /* Reject unknown flags (no mode matched). */
    if (argc > 1 && argv[1][0] == '-') {
        fprintf(stderr, "urbi: unknown option: %s\n", argv[1]);
        print_usage(stderr);
        return 2;
    }

    return EXIT_SUCCESS;
}
