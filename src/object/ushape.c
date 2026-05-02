/* SPDX-License-Identifier: BSD-3-Clause */
/* ushape.c — UShape root singleton + transition primitive stubs.
 *
 * Spec references:
 *   docs/superpowers/specs/2026-04-24-urbi-pre-m2-object-model-design.md §3, §7.1, §7.2
 *   docs/superpowers/specs/2026-04-29-urbi-pre-m4-uslot-uprops-collapse-design.md §4.1, §4.2
 *
 * The root-shape singleton is owned by UVM (vm->root_shape) and lazily
 * allocated on the first urbi_shape_root call.  All root-shape fields are
 * zero except cell (filled by urbi_gc_alloc).
 *
 * Transition primitives are stubs at this task; later tasks land the
 * transition-cache lookup + sibling-shape materialisation. */

#include "object/ushape.h"
#include "uvm.h"
#include "urbi/gc.h"          /* urbi_gc_alloc */

UShape *urbi_shape_root(struct UVM *vm)
{
    if (vm->root_shape != NULL) {
        return vm->root_shape;
    }

    UCell *c = urbi_gc_alloc(vm, sizeof(UShape), UTYPE_SHAPE);
    if (c == NULL) {
        return NULL;
    }

    UShape *s = (UShape *)c;
    s->name        = NULL;
    s->index       = 0u;
    s->count       = 0u;
    s->flags       = 0u;
    s->_pad        = 0u;
    s->parent      = NULL;
    s->transitions = NULL;
    s->props_table = NULL;

    vm->root_shape = s;
    return s;
}

UShape *urbi_shape_transition_add_slot(struct UVM *vm, UShape *parent,
                                       USymbol *name)
{
    /* Transition-cache lookup + fresh-shape allocation lands at a later
     * M4 task (per pre-M2 §7.1). */
    (void)vm; (void)parent; (void)name;
    return NULL;
}

UShape *urbi_shape_transition_property(struct UVM *vm, UShape *parent,
                                       uint32_t slot_index,
                                       uint8_t flag_bit, int install)
{
    /* Sibling-shape materialisation for property install/remove lands at a
     * later M4 task (per pre-M2 §7.2 + pre-M4 USlot/UProps spec §5.1, §5.2). */
    (void)vm; (void)parent; (void)slot_index; (void)flag_bit; (void)install;
    return NULL;
}

/* T12 stub: always returns -1 (slot not found locally).
 * T13 lands the real lineage walk over s->parent collecting matched USymbol
 * pointers; until then, urbi_object_lookup falls straight through to the
 * proto-walk path on every lookup, which is exactly what T12's cycle-safety
 * + rollover tests need to exercise. */
int32_t urbi_shape_find_slot(const UShape *s, const USymbol *name)
{
    (void)s; (void)name;
    return -1;
}
