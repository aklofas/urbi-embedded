/* SPDX-License-Identifier: BSD-3-Clause */
/* The merged AST_WATCHER node: at / at sync / whenever / waituntil all
 * build one node kind, discriminated by u.watcher.mode and u.watcher.source. */
#include "utest.h"
#include "util/uarena.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "parse/uast.h"
#include <string.h>

#define UTEST(name) static void name(void)

typedef struct { ULexer lex; UArena arena; UParser p; } PFix;

static UAstNode *parse_one(PFix *f, const char *src) {
    ulex_init(&f->lex, src, strlen(src));
    uarena_init(&f->arena, 4096);
    uparse_init(&f->p, &f->lex, &f->arena);
    return uparse_next_statement(&f->p);
}
static void pfix_close(PFix *f) { uarena_destroy(&f->arena); }

static int ident_is(const UAstNode *n, const char *s) {
    return n && n->kind == AST_IDENT && (int)strlen(s) == n->u.ident.len
        && memcmp(n->u.ident.start, s, (size_t)n->u.ident.len) == 0;
}

UTEST(at_cond_form) {
    PFix f; UAstNode *n = parse_one(&f, "at (x > 1) echo(1)");
    UASSERT(n && n->kind == AST_WATCHER && n->u.watcher.mode == UWATCHER_AT && n->u.watcher.source == UWSRC_COND);
    UASSERT(n->u.watcher.body && n->u.watcher.else_body == NULL && n->u.watcher.onleave == NULL);
    pfix_close(&f);
}
UTEST(at_event_form_with_payload) {
    PFix f; UAstNode *n = parse_one(&f, "at (e?(var v)) echo(v)");
    UASSERT(n && n->u.watcher.source == UWSRC_EVENT && ident_is(n->u.watcher.cond, "e"));
    UASSERT(n->u.watcher.payload_var_len == 1 && n->u.watcher.payload_var[0] == 'v');
    pfix_close(&f);
}
UTEST(at_slot_change_form) {
    PFix f; UAstNode *n = parse_one(&f, "at (o.x.changed?) echo(1)");
    UASSERT(n && n->u.watcher.source == UWSRC_SLOT_CHANGE && ident_is(n->u.watcher.cond, "o"));
    UASSERT(n->u.watcher.slot_name_len == 1 && n->u.watcher.slot_name[0] == 'x');
    pfix_close(&f);
}
UTEST(at_sync_sets_the_mode) {
    PFix f; UAstNode *n = parse_one(&f, "at sync (x > 1) echo(1)");
    UASSERT(n && n->u.watcher.mode == UWATCHER_AT_SYNC);
    pfix_close(&f);
}
UTEST(whenever_with_else_and_onleave) {
    PFix f; UAstNode *n = parse_one(&f, "whenever (x > 1) echo(1) onleave echo(2) else echo(3)");
    UASSERT(n && n->u.watcher.mode == UWATCHER_WHENEVER && n->u.watcher.else_body && n->u.watcher.onleave);
    pfix_close(&f);
}
UTEST(waituntil_is_a_watcher_without_a_body) {
    PFix f; UAstNode *n = parse_one(&f, "waituntil (x > 1)");
    UASSERT(n && n->kind == AST_WATCHER && n->u.watcher.mode == UWATCHER_WAITUNTIL && n->u.watcher.body == NULL);
    pfix_close(&f);
}
UTEST(waituntil_event_form_keeps_the_payload_name) {
    PFix f; UAstNode *n = parse_one(&f, "waituntil (e?(var p))");
    UASSERT(n && n->u.watcher.source == UWSRC_EVENT && n->u.watcher.payload_var_len == 1);
    pfix_close(&f);
}
UTEST(slot_change_payload_binding_is_kept) {
    /* The baseline dropped the name on this path; the merged node keeps it. */
    PFix f; UAstNode *n = parse_one(&f, "at (o.x.changed?(var nv)) echo(nv)");
    UASSERT(n && n->u.watcher.source == UWSRC_SLOT_CHANGE && n->u.watcher.payload_var_len == 2);
    pfix_close(&f);
}
UTEST(at_sync_event_form_accepts_onleave) {
    /* onleave is rejected only for an AT_SYNC watcher over a COND source
     * (parse_at_cond_form); AT_SYNC + EVENT does accept it. */
    PFix f; UAstNode *n = parse_one(&f, "at sync (e?) echo(1) onleave echo(2)");
    UASSERT(n && n->kind == AST_WATCHER && n->u.watcher.mode == UWATCHER_AT_SYNC
          && n->u.watcher.source == UWSRC_EVENT && n->u.watcher.onleave != NULL);
    pfix_close(&f);
}
UTEST(at_sync_cond_form_rejects_onleave) {
    PFix f; UAstNode *n = parse_one(&f, "at sync (x > 1) echo(1) onleave echo(2)");
    UASSERT(n && n->kind == AST_ERROR
          && (UParseError)n->u.err.code == PARSE_AT_SYNC_DOES_NOT_SUPPORT_ONLEAVE);
    pfix_close(&f);
}

void test_parse_watcher_suite(void) {
    utest_run("at_cond_form", at_cond_form);
    utest_run("at_event_form_with_payload", at_event_form_with_payload);
    utest_run("at_slot_change_form", at_slot_change_form);
    utest_run("at_sync_sets_the_mode", at_sync_sets_the_mode);
    utest_run("whenever_with_else_and_onleave", whenever_with_else_and_onleave);
    utest_run("waituntil_is_a_watcher_without_a_body", waituntil_is_a_watcher_without_a_body);
    utest_run("waituntil_event_form_keeps_the_payload_name", waituntil_event_form_keeps_the_payload_name);
    utest_run("slot_change_payload_binding_is_kept", slot_change_payload_binding_is_kept);
    utest_run("at_sync_event_form_accepts_onleave", at_sync_event_form_accepts_onleave);
    utest_run("at_sync_cond_form_rejects_onleave", at_sync_cond_form_rejects_onleave);
}
