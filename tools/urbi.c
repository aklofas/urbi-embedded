/* SPDX-License-Identifier: BSD-3-Clause */
/* urbi — the M1 REPL binary.  Host-only. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "urbi.h"

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
        "\n"
        "Options:\n"
        "  --version, -V        print version and exit\n"
        "  --help, -h           print this help and exit\n"
        "\n"
        "Exit status:\n"
        "  0   success\n"
        "  1   runtime or compile error\n"
        "  2   usage error (bad flag, missing file)\n",
        out);
}

static int eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

int main(int argc, char *argv[]) {
    /* First pass: handle --version / --help short-circuits. */
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

    /* Reject any other flags at M1 — will be expanded in later tasks. */
    if (argc > 1 && argv[1][0] == '-') {
        fprintf(stderr, "urbi: unknown option: %s\n", argv[1]);
        print_usage(stderr);
        return 2;
    }

    /* No other modes implemented yet — return 0 for now to keep existing
       stub behavior.  Subsequent tasks wire in -i / -e / -f / --dump-bytecode. */
    return EXIT_SUCCESS;
}
