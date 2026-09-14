/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/urealm.c — see rt/urealm.h. */

#include "rt/urealm.h"

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

    if (!vm->main_realm) vm->main_realm = r;
    return r;
}

void urealm_free(UVM *vm, URealm *r)
{
    if (!vm || !r || r == vm->main_realm) return;
    /* Stop the connection tag BEFORE unlinking: utag_stop finds members
     * by walking vm->realms, so a realm already off that list would leave
     * its strands marked by nobody. */
    if (r->root_tag) utag_stop(vm, r->root_tag);
    /* And every timer the realm owns.  Stopping the connection tag drops
     * only connection-tag periodics; one armed under a user tag in this
     * realm would go on spawning bodies into a realm with no globals,
     * and would keep urbi_has_live_work true for the life of the VM. */
    usched_timers_drop_realm(vm, r);
    for (URealm **pp = &vm->realms; *pp; pp = &(*pp)->next) {
        if (*pp == r) { *pp = r->next; r->next = NULL; break; }
    }
    /* The marked strands never get to run that cleanup -- nothing
     * schedules them again -- but dropping the lists here makes the realm
     * and everything below it unreachable, and the next collection takes
     * the lot.  A strand still on the run queue is reached through the
     * queue until it dies. */
    r->strands = NULL;
    r->globals = NULL;
    r->root_tag = NULL;
}

void urealm_write(UVM *vm, URealm *r, const char *chan, size_t cl,
                  const char *msg, size_t ml)
{
    if (r && r->writer) { r->writer(r->writer_ud, chan, cl, msg, ml); return; }
    if (vm && vm->writer) vm->writer(vm->writer_ud, chan, cl, msg, ml);
}
