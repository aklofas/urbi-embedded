/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/urealm.c — see rt/urealm.h. */

#include "rt/urealm.h"

/* --- the session registry -------------------------------------------
 *
 * `Lobby.lobbies` is a List of every live realm's globals object.  It is
 * maintained HERE rather than in the stdlib file that creates it because
 * a realm comes and goes below the stdlib layer: urealm_new is the only
 * place that knows a new script world exists, and urealm_free the only
 * place that knows one is gone.
 *
 * The slot is a plain local on the Lobby prototype (stdlib/lobby_native.c
 * installs it at boot), so a realm created before the boot finished — or
 * in a VM booted without the standard library at all — finds no list and
 * simply is not registered. */
static UList *urealm_lobbies(UVM *vm)
{
    UObject *lobby = vm->protos[UP_LOBBY];
    if (!lobby) return NULL;
    const USym *name = usym_cstr(vm, "lobbies");
    if (!name) return NULL;
    int idx = uobj_find_local(lobby, name);
    if (idx < 0) return NULL;
    UValue v = lobby->values[idx];
    return uv_is_list(v) ? (UList *)v.v.p : NULL;
}

static void urealm_lobbies_remove(UVM *vm, URealm *r)
{
    UList *l = urealm_lobbies(vm);
    if (!l || !r->globals) return;
    for (uint32_t i = 0; i < l->len; i++) {
        if (l->items[i].kind == UV_OBJ && l->items[i].v.p == (void *)r->globals) {
            (void)ulist_remove_at(l, i);
            return;
        }
    }
}

void urealm_trace(UVM *vm, URealm *r)
{
    if (r->globals) ugc_mark(vm, &r->globals->cell);
    if (r->root_tag) ugc_mark(vm, &r->root_tag->cell);
    for (UStrand *s = r->strands; s; s = s->next_in_realm) ugc_mark(vm, &s->cell);
}

URealm *urealm_new(UVM *vm)
{
    URealm *r = (URealm *)ugc_alloc(vm, UCELL_REALM, sizeof(URealm));
    if (!r) return NULL;
    r->vm = vm;
    /* Link before allocating the globals object: uobj_new may collect, and
     * the realm is only reachable through vm->realms. */
    r->next = vm->realms;
    vm->realms = r;
    r->globals = uobj_new(vm, vm->root_globals);
    if (!r->globals) { vm->realms = r->next; return NULL; }

    /* `Realm` is the per-realm self-reference the corpus resolves
     * `Realm.x` through.  It cannot live on root_globals: that object is
     * shared by every realm, and each realm has to see ITS OWN globals.
     * CONSTANT so a script cannot repoint it. */
    USym *self_name = usym_cstr(vm, "Realm");
    if (!self_name || uobj_set_local(vm, r->globals, self_name, uv_obj(r->globals),
                                     USLOT_CONSTANT) < 0) {
        vm->realms = r->next;
        return NULL;
    }

    /* The realm's connection tag.  Every strand the realm spawns — the
     * chunk a host runs, a REPL line, a forked arm — inherits it as its
     * ambient tag, so `Lobby.connectionTag` names it and stopping it
     * cancels the whole realm.  Created last: it is the only step that
     * can be skipped without leaving a half-built realm behind. */
    r->root_tag = utag_new(vm, uv_nil());
    if (!r->root_tag) { vm->realms = r->next; return NULL; }

    /* Publish the realm as a lobby.  Last, and its failure is not the
     * realm's: an unregistered realm still runs code and still writes on
     * its own writer; only a `wall` from elsewhere would miss it. */
    {
        UList *l = urealm_lobbies(vm);
        if (l) (void)ulist_push(vm, l, uv_obj(r->globals));
    }

    if (!vm->main_realm) vm->main_realm = r;
    return r;
}

void urealm_free(UVM *vm, URealm *r)
{
    if (!vm || !r || r == vm->main_realm) return;
    /* Unpublish first, while r->globals still names the entry to drop. */
    urealm_lobbies_remove(vm, r);
    /* Stop the connection tag BEFORE unlinking: utag_stop finds members
     * by walking vm->realms, so a realm already off that list would leave
     * its strands marked by nobody. */
    if (r->root_tag) utag_stop(vm, r->root_tag);
    /* And every timer the realm owns.  Stopping the connection tag drops
     * only connection-tag periodics; one armed under a user tag in this
     * realm would go on spawning bodies into a realm with no globals,
     * and would keep urbi_has_live_work true for the life of the VM. */
    usched_timers_drop_realm(vm, r);
    /* And every watcher: a condition whose realm has no globals left
     * would raise on every drain for the life of the VM. */
    uwatch_realm_dropped(vm, r);
    /* Every strand the realm owns, named directly: the connection tag only
     * covers strands under it, and a child detached under a user tag is
     * not.  Each one is stopped so it dies on its next slice, and its open
     * upvalues are closed NOW -- a closure the host still holds may point
     * at this strand's register stack, which the collector frees with the
     * strand. */
    for (UStrand *s = r->strands; s; s = s->next_in_realm) {
        if (s->state != USTRAND_DEAD && s->unwind == (uint8_t)UUNWIND_NONE) {
            s->unwind = (uint8_t)UUNWIND_STOP;
            s->transfer = r->root_tag ? uv_ptr(UV_CELL, r->root_tag) : uv_nil();
            if (s != vm->sched.current) {
                s->gates = 0;
                if (s->state == USTRAND_PARKED) usched_wake(s, uv_nil());
            }
        }
        ustrand_close_upvals(s, 0);
    }
    for (URealm **pp = &vm->realms; *pp; pp = &(*pp)->next) {
        if (*pp == r) { *pp = r->next; r->next = NULL; break; }
    }
    /* Dropping the lists here makes the realm and everything below it
     * unreachable once the stopped strands have died, and the next
     * collection takes the lot.  A strand still on the run queue is
     * reached through the queue until it dies. */
    r->strands = NULL;
    r->globals = NULL;
    r->root_tag = NULL;
}

void urealm_set_writer(UVM *vm, URealm *r,
                       void (*fn)(void *ud, const char *chan, size_t cl,
                                  const char *msg, size_t ml), void *ud)
{
    (void)vm;
    if (!r) return;
    r->writer = fn;
    r->writer_ud = ud;
}

void urealm_set_budget(UVM *vm, URealm *r, const UCompileBudget *b)
{
    (void)vm;
    if (!r) return;
    if (b) r->budget = *b;
    else { r->budget.max_parser_depth = 0; r->budget.max_ast_nodes = 0; r->budget.max_source_bytes = 0; }
}

void urealm_write(UVM *vm, URealm *r, const char *chan, size_t cl,
                  const char *msg, size_t ml)
{
    if (r && r->writer) { r->writer(r->writer_ud, chan, cl, msg, ml); return; }
    if (vm && vm->writer) vm->writer(vm->writer_ud, chan, cl, msg, ml);
}
