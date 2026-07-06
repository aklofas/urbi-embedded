/* SPDX-License-Identifier: BSD-3-Clause */
/* urbi-server — headless network REPL server (v0.9.1).
 *
 * Builds only when URBI_ENABLE_REPL=1.  Spins up a UVM, optionally runs
 * an urbiscript boot script under the global realm, then enters a step
 * loop driving the REPL service until SIGINT/SIGTERM.
 *
 * Default-secure: a non-loopback --bind without --token (or the
 * URBI_REPL_TOKEN env var) is refused by urbi_repl_serve with
 * URBI_ERR_INSECURE_CONFIG; we print a hint and exit 1.
 *
 * The TCP listener pthread is started indirectly when we register the
 * UREPL_TCP_TRANSPORT vtable on the server.  No public urbi_step()
 * caller is required for accept/read — those run on their own threads —
 * but urbi_step() IS required for dispatch (NDJSON jobs run on the VM
 * thread via the dispatcher drain hook).
 */

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "urbi/urbi.h"
#include "urbi/repl.h"

/* Internal headers: TCP transport vtable + UTcpListener factory + the
 * compile-then-run pattern for the optional boot script.  Same approach
 * tools/urbi.c uses for `urbi script.u`. */
#include "repl/urepl_transport_tcp.h"
#include "value/uarena.h"
#include "parse/uast.h"
#include "parse/uparse.h"
#include "lex/ulex.h"
#include "emit/uemit.h"
#include "chunk/uchunk.h"
#include "vm/uvm.h"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void usage(FILE *out) {
    fputs(
        "Usage: urbi-server [options] [script.u]\n"
        "\n"
        "Network REPL service (v0.9.1).  Listens for NDJSON clients over TCP.\n"
        "\n"
        "Options:\n"
        "  --bind ADDR         bind address (default: 127.0.0.1)\n"
        "  --port N            TCP port (default: 54000; 0 = kernel-assigned)\n"
        "  --token TOK         require bearer-token auth\n"
        "                      (env URBI_REPL_TOKEN also consulted)\n"
        "  --max-clients N     per-server connection cap (default: 16)\n"
        "  --script FILE.u     run urbiscript boot script before serving\n"
        "                      (positional non-flag arg also accepted)\n"
        "  --quiet             suppress banner on stderr\n"
        "  --help, -h          show this help and exit\n"
        "\n"
        "Default-secure: a non-loopback --bind without --token is refused.\n",
        out);
}

/* --- Boot-script compile-then-run (mirrors tools/urbi.c) --------------- */

#define URBI_SERVER_MAX_SCRIPT (1024U * 1024U)

static char *slurp(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "urbi-server: cannot open %s\n", path); return NULL; }
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { fprintf(stderr, "urbi-server: out of memory\n"); fclose(fp); return NULL; }
    for (;;) {
        if (len + 4096 > cap) {
            if (cap >= URBI_SERVER_MAX_SCRIPT) {
                fprintf(stderr, "urbi-server: %s exceeds %u byte cap\n",
                        path, URBI_SERVER_MAX_SCRIPT);
                free(buf); fclose(fp); return NULL;
            }
            size_t ncap = cap * 2;
            if (ncap > URBI_SERVER_MAX_SCRIPT) ncap = URBI_SERVER_MAX_SCRIPT;
            char *n = realloc(buf, ncap);
            if (!n) { fprintf(stderr, "urbi-server: out of memory\n");
                      free(buf); fclose(fp); return NULL; }
            buf = n; cap = ncap;
        }
        size_t r = fread(buf + len, 1, cap - len, fp);
        len += r;
        if (r == 0) break;
    }
    fclose(fp);
    *out_len = len;
    return buf;
}

static int run_boot_script(UVM *vm, const char *path) {
    size_t len = 0;
    char *src = slurp(path, &len);
    if (!src) return 1;

    ULexer lex;
    ulex_init(&lex, src, len);

    UArena arena;
    uarena_init(&arena, 4096);

    /* CHSTR-027 pattern: the root proto must be
     * heap-allocated — closures created by the boot script keep proto
     * pointers alive past this frame; uchunk_destroy defers the actual
     * free to the refcount-rescue machinery (vm->rescued_protos) when
     * references remain, and frees immediately when none do.  Ownership
     * fields go in BEFORE uemit_init per its documented contract, so the
     * single uchunk_destroy below also covers the compile-failure path
     * (refcount 0 + heap_allocated → buffers and struct freed in place). */
    UProto *module = (UProto *)vm->alloc_fn(NULL, sizeof(UProto), vm->alloc_ud);
    if (module == NULL) {
        fprintf(stderr, "urbi-server: out of memory\n");
        uarena_destroy(&arena);
        free(src);
        return 1;
    }
    memset(module, 0, sizeof *module);
    module->alloc_fn       = vm->alloc_fn;
    module->alloc_ud       = vm->alloc_ud;
    module->heap_allocated = true;
    UEmitter e;
    uemit_init(&e, module, &arena, vm, path);

    UParser p;
    uparse_init(&p, &lex, &arena);

    UAstNode *node;
    int rc = 0;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) {
            const char *msg = node->u.err.message ? node->u.err.message : "parse error";
            fprintf(stderr, "urbi-server: %s:%d:%d: %s\n", path, node->line, node->col, msg);
            rc = 1;
            break;
        }
        (void)uemit_statement(&e, node);
        uarena_reset(&arena);
    }
    if (rc == 0 && uemit_finish(&e) != EMIT_OK) {
        fprintf(stderr, "urbi-server: %s: emit error: %s\n",
                path, uemit_error_name(e.error));
        rc = 1;
    }
    if (rc != 0) {
        /* Parse-error path skipped uemit_finish; release emitter-owned
         * funcstate storage (no-op when finish already ran — FE-07). */
        urbi_emit_abandon(&e);
    }
    if (rc == 0) {
        UValue out;
        UVMError vrc = urbi_vm_run(vm, NULL, module, &out);
        if (vrc != UVM_OK) {
            fprintf(stderr, "urbi-server: %s: %s\n",
                    path, vm->last_errmsg[0] ? vm->last_errmsg : "(vm error)");
            rc = 1;
        }
    }
    uchunk_destroy(module, vm);
    uarena_destroy(&arena);
    free(src);
    return rc;
}

/* --- main ------------------------------------------------------------- */

int main(int argc, char **argv) {
    UReplConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.bind_addr          = "127.0.0.1";
    cfg.tcp_port           = 54000;
    cfg.max_clients        = 16;
    cfg.output_ringbuf_cap = 64 * 1024;
    cfg.auth_token         = getenv("URBI_REPL_TOKEN");

    const char *script = NULL;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout); return 0;
        } else if (!strcmp(a, "--bind") && i + 1 < argc) {
            cfg.bind_addr = argv[++i];
        } else if (!strcmp(a, "--port") && i + 1 < argc) {
            cfg.tcp_port = atoi(argv[++i]);
        } else if (!strcmp(a, "--token") && i + 1 < argc) {
            cfg.auth_token = argv[++i];
        } else if (!strcmp(a, "--max-clients") && i + 1 < argc) {
            cfg.max_clients = atoi(argv[++i]);
        } else if (!strcmp(a, "--script") && i + 1 < argc) {
            script = argv[++i];
        } else if (!strcmp(a, "--quiet")) {
            quiet = true;
        } else if (a[0] != '-' && script == NULL) {
            script = a;
        } else {
            fprintf(stderr, "urbi-server: unknown arg: %s\n", a);
            usage(stderr); return 2;
        }
    }

    UVM vm;
    if (urbi_vm_init(&vm, NULL, NULL) != URBI_OK) {
        fprintf(stderr, "urbi-server: urbi_vm_init failed\n");
        return 1;
    }

    if (script) {
        int rc = run_boot_script(&vm, script);
        if (rc != 0) {
            urbi_vm_destroy(&vm);
            return 1;
        }
    }

    int err = 0;
    UReplServer *server = urbi_repl_serve(&vm, &cfg, &err);
    if (!server) {
        if (err == URBI_ERR_INSECURE_CONFIG) {
            fprintf(stderr,
                    "urbi-server: refused: --bind %s is non-loopback "
                    "but no --token / URBI_REPL_TOKEN set\n", cfg.bind_addr);
        } else {
            fprintf(stderr, "urbi-server: urbi_repl_serve failed: %d\n", err);
        }
        urbi_vm_destroy(&vm);
        return 1;
    }

    UTcpListener *listener = urepl_tcp_listener_create(cfg.bind_addr, cfg.tcp_port);
    if (!listener) {
        fprintf(stderr, "urbi-server: failed to bind %s:%d\n",
                cfg.bind_addr, cfg.tcp_port);
        urbi_repl_stop(server);
        urbi_vm_destroy(&vm);
        return 1;
    }
    int rc = urbi_repl_register_transport(server, &UREPL_TCP_TRANSPORT, listener);
    if (rc != URBI_OK) {
        fprintf(stderr, "urbi-server: register_transport failed: %d\n", rc);
        urepl_tcp_listener_destroy(listener);
        urbi_repl_stop(server);
        urbi_vm_destroy(&vm);
        return 1;
    }

    if (!quiet) {
        fprintf(stderr, "urbi-server listening on %s:%u%s\n",
                cfg.bind_addr, (unsigned)listener->port,
                (cfg.auth_token && cfg.auth_token[0]) ? " (auth required)" : " (no auth)");
        fflush(stderr);
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);  /* client disconnects mid-write must not kill us */

    /* Drive loop.  The listener + per-client reader threads run their
     * own pthreads; dispatch jobs queued by readers run on THIS thread
     * via urbi_step()'s registered drain hook.  1 ms sleep keeps idle
     * CPU low; v1.x optimization: use urbi_step's out_next_wake_us. */
    while (!g_stop) {
        (void)urbi_step(&vm, 1024, NULL);
        struct timespec ts = { 0, 1 * 1000 * 1000 };  /* 1 ms */
        nanosleep(&ts, NULL);
    }

    urbi_repl_stop(server);
    urepl_tcp_listener_destroy(listener);
    urbi_vm_destroy(&vm);
    return 0;
}
