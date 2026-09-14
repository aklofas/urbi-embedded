/* SPDX-License-Identifier: BSD-3-Clause */
/* chk-host-driver — runs the `.chk` fixtures that need embedding-API
 * operations the single-pass `urbi -i` REPL path cannot express: more
 * than one realm, a clock the fixture controls, and urbi_step called
 * where the fixture says rather than after every line.
 *
 * A TEST binary (tests/ is exempt from the freestanding rules), but it
 * uses ONLY the public embedding API -- no runtime-internal header --
 * because half of what it is pinning is that the API is enough.
 *
 * Invocation: chk-host-driver <chk-fixture>
 *   Executes the fixture's `## host:` directives in order and prints one
 *   framed line per directive that produces output, in the same
 *   "[NNNNNNNN] text" shape `urbi -i` uses so run_chk.sh's existing
 *   prefix-stripping diffs them identically.  Exits 0 once every
 *   directive has run; non-zero only on a setup or IO failure.
 *
 * Directives:
 *   ## host: realm <name>       create-or-select a named realm; `default`
 *                               names the VM's main realm.  Later `run`
 *                               and `set-global` target it.
 *   ## host: run <source>       run one chunk on the current realm and
 *                               print its value, or "!!! <message>".
 *   ## host: step <budget>      one urbi_step, printing "step: <RESULT>"
 *                               (RUNNING / WAKE_AT / QUIESCENT).
 *   ## host: advance-clock <ms> move the fixture's virtual clock forward.
 *                               Nothing is printed; the next `step` is
 *                               where the moved deadlines come due.
 *   ## host: set-global <name> <int>
 *                               write an Integer global from C, between
 *                               steps -- a host-side slot write, which is
 *                               not a thing script can do to itself.
 *                               Prints "set-global: ok".
 *   ## host: live-work         print "live-work: true|false", what
 *                               urbi_has_live_work says right now.  It
 *                               and `step` answer different questions --
 *                               an armed watcher is live work while every
 *                               step reports QUIESCENT -- which is
 *                               exactly what wants pinning.
 *   ## host: expect-host-call <n>
 *                               print "host-calls: <count>", the number
 *                               of times the pre-registered native global
 *                               __hostprobe() has been called.
 *
 * Anything that is not a `## host:` line is the runner's business, not
 * this binary's: plain `#` comments and `[...]` expected lines are
 * skipped.
 *
 * THE CLOCK IS VIRTUAL and starts at zero.  That is the whole reason this
 * driver exists for the temporal fixtures: a sleeper's wake-up is an
 * assertion here, not a race. */

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "urbi/urbi.h"

#define CHK_MAX_REALMS 8
#define CHK_LINE_CAP   4096

typedef struct { char name[64]; URealm *realm; } NamedRealm;

static NamedRealm g_realms[CHK_MAX_REALMS];
static int        g_realm_count;
static URealm    *g_current;

static uint64_t g_now_us;
static uint64_t chk_clock(void *ud) { (void)ud; return g_now_us; }

static int g_hostcalls;
static int chk_probe(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    g_hostcalls++;
    if (out) *out = urbi_make_nil();
    return 0;   /* UEXEC_OK */
}

/* The REPL's frame, with the timestamp pinned at zero so the output is
 * reproducible; run_chk.sh strips the whole prefix before diffing. */
static void emit_framed(const char *text) { printf("[00000000] %s\n", text); }

static const char *step_name(int r)
{
    switch (r) {
    case URBI_STEP_RAN:        return "RUNNING";
    case URBI_STEP_IDLE_UNTIL: return "WAKE_AT";
    case URBI_STEP_QUIESCENT:  return "QUIESCENT";
    default:                   return "ERROR";
    }
}

static int select_realm(UVM *vm, const char *name)
{
    if (strcmp(name, "default") == 0) { g_current = urbi_realm_main(vm); return 0; }
    for (int i = 0; i < g_realm_count; i++) {
        if (strcmp(g_realms[i].name, name) == 0) { g_current = g_realms[i].realm; return 0; }
    }
    if (g_realm_count >= CHK_MAX_REALMS) {
        fprintf(stderr, "chk-host-driver: too many realms (max %d)\n", CHK_MAX_REALMS);
        return -1;
    }
    URealm *r = urbi_realm_new(vm);
    if (!r) {
        fprintf(stderr, "chk-host-driver: urbi_realm_new failed for '%s'\n", name);
        return -1;
    }
    NamedRealm *slot = &g_realms[g_realm_count++];
    snprintf(slot->name, sizeof slot->name, "%s", name);
    slot->realm = r;
    g_current = r;
    return 0;
}

static int do_run(UVM *vm, const char *src)
{
    UValue out = urbi_make_nil();
    char err[512] = { 0 };
    int rc = urbi_run(vm, g_current, src, strlen(src), NULL, &out, err, sizeof err);
    if (rc == URBI_OK) {
        char text[512];
        urbi_value_to_string(vm, out, text, sizeof text);
        emit_framed(text);
    } else {
        /* The REPL's error frame, from the same two sources it uses: the
         * compile diagnostic if there is one, else the error channel. */
        UErrorInfo info;
        urbi_last_error(vm, &info);
        const char *msg = err[0] ? err
                        : (info.message && info.message[0] ? info.message : "(vm error)");
        char line[576];
        snprintf(line, sizeof line, "!!! %s", msg);
        emit_framed(line);
    }
    return 0;
}

static int do_set_global(UVM *vm, const char *rest)
{
    char name[64];
    size_t ni = 0;
    const char *p = rest;
    while (*p && *p != ' ' && *p != '\t' && ni + 1 < sizeof name) name[ni++] = *p++;
    name[ni] = '\0';
    while (*p == ' ' || *p == '\t') p++;
    char *end = NULL;
    long long v = strtoll(p, &end, 10);
    if (end == p) {
        fprintf(stderr, "chk-host-driver: `set-global` needs a numeric value\n");
        return -1;
    }
    if (urbi_global_set(vm, g_current, name, urbi_make_int((int64_t)v)) != URBI_OK) {
        fprintf(stderr, "chk-host-driver: set-global '%s' failed\n", name);
        return -1;
    }
    emit_framed("set-global: ok");
    return 0;
}

static int run_directive(UVM *vm, const char *verb, const char *rest)
{
    if (strcmp(verb, "realm") == 0) {
        if (rest[0] == '\0') { fprintf(stderr, "chk-host-driver: `realm` needs a name\n"); return -1; }
        return select_realm(vm, rest);
    }
    if (strcmp(verb, "run") == 0) return do_run(vm, rest);
    if (strcmp(verb, "set-global") == 0) return do_set_global(vm, rest);

    if (strcmp(verb, "advance-clock") == 0) {
        char *end = NULL;
        unsigned long long ms = strtoull(rest, &end, 10);
        if (end == rest) { fprintf(stderr, "chk-host-driver: `advance-clock` needs ms\n"); return -1; }
        g_now_us += (uint64_t)ms * 1000ULL;
        return 0;
    }
    if (strcmp(verb, "expect-host-call") == 0) {
        char line[64];
        snprintf(line, sizeof line, "host-calls: %d", g_hostcalls);
        emit_framed(line);
        return 0;
    }
    if (strcmp(verb, "live-work") == 0) {
        char line[64];
        snprintf(line, sizeof line, "live-work: %s",
                 urbi_has_live_work(vm) ? "true" : "false");
        emit_framed(line);
        return 0;
    }
    if (strcmp(verb, "step") == 0) {
        char *end = NULL;
        unsigned long long budget = strtoull(rest, &end, 10);
        if (end == rest) { fprintf(stderr, "chk-host-driver: `step` needs a numeric budget\n"); return -1; }
        char line[64];
        snprintf(line, sizeof line, "step: %s",
                 step_name(urbi_step(vm, (uint32_t)budget, NULL)));
        emit_framed(line);
        return 0;
    }
    fprintf(stderr, "chk-host-driver: unknown directive `%s`\n", verb);
    return -1;
}

static void chomp(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
}

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static int parse_host_line(const char *line, char *verb, size_t verb_cap, const char **rest_out)
{
    const char *p = skip_ws(line);
    if (p[0] != '#' || p[1] != '#') return 0;
    p = skip_ws(p + 2);
    if (strncmp(p, "host:", 5) != 0) return 0;
    p = skip_ws(p + 5);
    size_t i = 0;
    while (p[i] && p[i] != ' ' && p[i] != '\t' && i + 1 < verb_cap) { verb[i] = p[i]; i++; }
    verb[i] = '\0';
    *rest_out = skip_ws(p + i);
    return 1;
}

static void *chk_alloc(void *p, size_t n, void *ud)
{
    (void)ud;
    if (n == 0) { free(p); return NULL; }
    return realloc(p, n);
}

/* A strand's uncaught throw, framed like a result line.  A detached arm
 * has no caller to return a code to, so this is the only way a fixture
 * can pin one; the "!!!" spelling matches the `run` directive's own error
 * frame because it means the same thing. */
static void chk_diag(UVM *vm, void *ud, int level, const char *msg, size_t len)
{
    (void)vm; (void)ud; (void)level;
    printf("[00000000] !!! %.*s\n", (int)len, msg);
}

/* Script output goes to stdout unframed, the way the REPL's writer does
 * it, so an `echo` in a fixture lands in the same stream as the framed
 * result lines. */
static void chk_writer(void *ud, const char *chan, size_t cl, const char *msg, size_t ml)
{
    (void)ud; (void)chan; (void)cl;
    fwrite(msg, 1, ml, stdout);
}

int main(int argc, char *argv[])
{
    if (argc != 2) { fprintf(stderr, "usage: %s <chk-fixture>\n", argv[0]); return 2; }
    FILE *fp = fopen(argv[1], "r");
    if (!fp) { fprintf(stderr, "chk-host-driver: cannot open %s\n", argv[1]); return 2; }

    UVM *vm = urbi_open(chk_alloc, NULL, NULL);
    if (!vm) { fclose(fp); fprintf(stderr, "chk-host-driver: urbi_open failed\n"); return 2; }
    urbi_set_writer(vm, chk_writer, NULL);
    urbi_set_diag(vm, chk_diag, NULL);
    urbi_set_clock(vm, chk_clock, NULL);
    g_now_us = 0;
    g_hostcalls = 0;
    g_realm_count = 0;
    g_current = urbi_realm_main(vm);
    if (urbi_register(vm, "__hostprobe", chk_probe, 0, 0) != URBI_OK)
        fprintf(stderr, "chk-host-driver: failed to register __hostprobe\n");

    int rc = 0;
    char line[CHK_LINE_CAP];
    while (fgets(line, sizeof line, fp)) {
        chomp(line);
        char verb[32];
        const char *rest;
        if (!parse_host_line(line, verb, sizeof verb, &rest)) continue;
        if (run_directive(vm, verb, rest) != 0) { rc = 1; break; }
    }
    fclose(fp);
    fflush(stdout);
    urbi_close(vm);
    return rc;
}
