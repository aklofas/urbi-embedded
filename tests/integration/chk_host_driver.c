/* SPDX-License-Identifier: BSD-3-Clause */
/* chk-host-driver — bounded C host-driver for `.chk` fixtures that need
 * embedding-API operations the single-pass `urbi -i` REPL path cannot express
 * (multi-realm isolation, urbi_step quiescence observation).
 *
 * This is a TEST binary (tests/ is exempt from the freestanding rules; hosted
 * headers and private source headers are OK).  It uses ONLY the existing
 * public embedding API — no new public symbol is introduced.
 *
 * Invocation: chk-host-driver <chk-fixture>
 *   Reads the fixture, executes its `## host:` directives in order, and prints
 *   framed output lines (matching the `urbi -i` `[NNNNNNNN] result` convention
 *   so tests/integration/run_chk.sh's existing `[...]`-prefix normalization
 *   diffs identically).  The runner owns the diff; this binary exits 0 once it
 *   has run all directives, non-zero only on a setup/IO error.
 *
 * Directive grammar (MVP — only what the activation targets need):
 *   ## host: realm <name>     create-or-select a named realm; subsequent
 *                             run/step target it.  The name "default" is a
 *                             reserved alias for the VM global realm (NULL).
 *   ## host: run <source>     compile + run one urbiscript source line under
 *                             the current realm via urbi_repl_eval; print the
 *                             framed result.
 *   ## host: step <budget>    call urbi_step(vm, budget) and print a framed
 *                             observable line "step: <STATE>".
 *   ## host: advance-clock <ms>
 *                             advance the driver's virtual monotonic clock by
 *                             <ms> milliseconds.  The next `## host: step`
 *                             wakes any sleep-queue strand whose wake_us has
 *                             now passed.  A virtual clock (vs. the default
 *                             wall clock) makes sleep/timer wakeups
 *                             deterministic and observable from a fixture.
 *   ## host: expect-host-call <n>
 *                             emit a framed line "host-calls: <count>" giving
 *                             the number of times the pre-registered native
 *                             probe global `__hostprobe()` has been invoked.
 *                             The fixture pins the expected count.
 *
 * Lines that are not `## host:` directives are ignored (plain `#` comments and
 * `[...]` expected lines are the runner's concern, not the driver's).
 */

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "urbi/urbi.h"
#include "value/uvalue.h"
#include "vm/uvm.h"

#define CHK_MAX_REALMS 8
#define CHK_LINE_CAP   4096

/* Named-realm table.  realm == NULL denotes the VM global realm (the
 * "default" reserved name); created lazily by urbi_repl_eval(realm=NULL). */
typedef struct {
    char           name[64];
    struct URealm *realm;   /* NULL => global realm */
} NamedRealm;

static NamedRealm    g_realms[CHK_MAX_REALMS];
static int           g_realm_count;
static struct URealm *g_current;   /* NULL => global realm */

/* Virtual monotonic clock backing `## host: advance-clock`.  Installed over
 * vm->host_time_us after init so sleep-queue wakeups are deterministic. */
static uint64_t g_now_us;
static uint64_t chk_now(void *ud) { (void)ud; return g_now_us; }

/* Counting native probe backing `## host: expect-host-call`.  Bound as the
 * realm global `__hostprobe`; each script-side call bumps g_hostcalls. */
static int g_hostcalls;
static int chk_probe(UVM *vm, UValue self, UValue *args, uint8_t nargs,
                     UValue *out) {
    (void)vm; (void)self; (void)args; (void)nargs;
    g_hostcalls++;
    if (out != NULL) *out = urbi_make_nil();
    return 0;   /* UEXEC_OK */
}

static const char *step_name(UStepResult r) {
    switch (r) {
    case URBI_STEP_RUNNING:   return "RUNNING";
    case URBI_STEP_QUIESCENT: return "QUIESCENT";
    case URBI_STEP_FATAL:     return "FATAL";
    case URBI_STEP_WAKE_AT:   return "WAKE_AT";
    }
    return "UNKNOWN";
}

/* Frame a result line exactly like the REPL: "[NNNNNNNN] <text>".  The
 * timestamp field is fixed at 0 so it is deterministic; run_chk.sh strips the
 * whole "[...] " prefix before diffing, so the value is immaterial — only the
 * trailing text is compared. */
static void emit_framed(const char *text) {
    printf("[00000000] %s\n", text);
}

/* Resolve a named realm, creating it on first reference.  "default" maps to
 * the VM global realm (NULL sentinel).  Returns 0 on success, -1 on table
 * overflow or OOM. */
static int select_realm(UVM *vm, const char *name) {
    if (strcmp(name, "default") == 0) {
        g_current = NULL;
        return 0;
    }
    for (int i = 0; i < g_realm_count; i++) {
        if (strcmp(g_realms[i].name, name) == 0) {
            g_current = g_realms[i].realm;
            return 0;
        }
    }
    if (g_realm_count >= CHK_MAX_REALMS) {
        fprintf(stderr, "chk-host-driver: too many realms (max %d)\n",
                CHK_MAX_REALMS);
        return -1;
    }
    struct URealm *r = urbi_realm_create(vm);
    if (r == NULL) {
        fprintf(stderr, "chk-host-driver: urbi_realm_create failed for '%s'\n",
                name);
        return -1;
    }
    NamedRealm *slot = &g_realms[g_realm_count++];
    snprintf(slot->name, sizeof slot->name, "%s", name);
    slot->realm = r;
    g_current   = r;
    return 0;
}

/* Run one `## host:` directive (verb + remainder).  Returns 0 on success,
 * -1 on a setup error that should abort the fixture. */
static int run_directive(UVM *vm, const char *verb, const char *rest) {
    if (strcmp(verb, "realm") == 0) {
        if (rest[0] == '\0') {
            fprintf(stderr, "chk-host-driver: `realm` needs a name\n");
            return -1;
        }
        return select_realm(vm, rest);
    }

    if (strcmp(verb, "run") == 0) {
        char out[512] = {0};
        int rc = urbi_repl_eval(vm, g_current, rest, strlen(rest),
                                out, sizeof out);
        if (rc == URBI_OK) {
            emit_framed(out);
        } else {
            /* Mirror the REPL error frame: "!!! <message>". */
            char line[576];
            const char *msg = out[0] ? out
                            : (vm->last_errmsg[0] ? vm->last_errmsg
                                                  : "(vm error)");
            snprintf(line, sizeof line, "!!! %s", msg);
            emit_framed(line);
        }
        return 0;
    }

    if (strcmp(verb, "advance-clock") == 0) {
        char *end = NULL;
        unsigned long long ms = strtoull(rest, &end, 10);
        if (end == rest) {
            fprintf(stderr, "chk-host-driver: `advance-clock` needs ms\n");
            return -1;
        }
        g_now_us += (uint64_t)ms * 1000ULL;
        return 0;
    }

    if (strcmp(verb, "expect-host-call") == 0) {
        char line[64];
        snprintf(line, sizeof line, "host-calls: %d", g_hostcalls);
        emit_framed(line);
        return 0;
    }

    if (strcmp(verb, "step") == 0) {
        char *end = NULL;
        unsigned long long budget = strtoull(rest, &end, 10);
        if (end == rest) {
            fprintf(stderr, "chk-host-driver: `step` needs a numeric budget\n");
            return -1;
        }
        UStepResult sr = urbi_step(vm, (uint64_t)budget, NULL);
        char line[64];
        snprintf(line, sizeof line, "step: %s", step_name(sr));
        emit_framed(line);
        return 0;
    }

    fprintf(stderr, "chk-host-driver: unknown directive `%s`\n", verb);
    return -1;
}

/* Strip a trailing newline / carriage-return in place. */
static void chomp(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
}

/* Skip leading spaces / tabs. */
static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Parse a `## host:` line into verb + rest.  Returns 1 if the line was a host
 * directive (verb/rest filled), 0 otherwise. */
static int parse_host_line(const char *line, char *verb, size_t verb_cap,
                           const char **rest_out) {
    const char *p = skip_ws(line);
    if (p[0] != '#' || p[1] != '#') return 0;
    p = skip_ws(p + 2);
    if (strncmp(p, "host:", 5) != 0) return 0;
    p = skip_ws(p + 5);

    /* Copy the verb token (up to first space/tab). */
    size_t i = 0;
    while (p[i] && p[i] != ' ' && p[i] != '\t' && i + 1 < verb_cap) {
        verb[i] = p[i];
        i++;
    }
    verb[i] = '\0';
    *rest_out = skip_ws(p + i);
    return 1;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <chk-fixture>\n", argv[0]);
        return 2;
    }

    FILE *fp = fopen(argv[1], "r");
    if (fp == NULL) {
        fprintf(stderr, "chk-host-driver: cannot open %s\n", argv[1]);
        return 2;
    }

    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    g_realm_count = 0;
    g_current     = NULL;   /* start on the global realm */

    /* Install the deterministic virtual clock and the counting native probe.
     * The clock starts at 0; `advance-clock` bumps it so sleep wakeups fire
     * exactly when a fixture wants.  `__hostprobe` is a const realm global the
     * `expect-host-call` directive observes. */
    vm.host_time_us = chk_now;
    vm.host_time_ud = NULL;
    g_now_us        = 0;
    g_hostcalls     = 0;
    if (urbi_register(&vm, NULL, "__hostprobe", chk_probe) != URBI_OK) {
        fprintf(stderr, "chk-host-driver: failed to register __hostprobe\n");
    }

    int rc = 0;
    char line[CHK_LINE_CAP];
    while (fgets(line, sizeof line, fp) != NULL) {
        chomp(line);
        char verb[32];
        const char *rest;
        if (!parse_host_line(line, verb, sizeof verb, &rest)) continue;
        if (run_directive(&vm, verb, rest) != 0) { rc = 1; break; }
    }
    fclose(fp);

    /* Tear down named realms before the VM.  The global realm (NULL slots)
     * is owned by the VM and freed in urbi_vm_destroy. */
    for (int i = 0; i < g_realm_count; i++) {
        if (g_realms[i].realm != NULL) {
            urbi_realm_destroy(&vm, g_realms[i].realm);
        }
    }
    urbi_vm_destroy(&vm);
    return rc;
}
