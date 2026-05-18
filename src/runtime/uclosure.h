/* SPDX-License-Identifier: BSD-3-Clause */
/* uclosure.h — UClosure runtime function value (proto + captured upvalues).
 *
 * UClosure embeds UCell as its first member at offset 0 (M4 — closes the
 * M3 baseline TODO at gc/ugc_incremental.h).  This makes the
 * UClosure* → UCell* cast performed by urbi_gc_upvalue_write well-defined.
 *
 * Lives outside umodule.h because the struct definition needs both UValue
 * (from umodule.h) and UCell (from gc/ugc.h), and gc/ugc.h itself includes
 * umodule.h for UValue — a direct UCell embed inside umodule.h would create
 * a circular include.  Files that only need `struct UClosure *` keep the
 * forward typedef in umodule.h; files that touch UClosure fields include
 * this header. */

#ifndef UCLOSURE_H
#define UCLOSURE_H

#include <stdint.h>

#include "module/umodule.h"                       /* UProto + forward typedef `UClosure`; pulls in uframe.h which forward-declares UUpvalCell */
#include "gc/ugc.h"                        /* UCell (2 B) */
#include "object/umodule_instance.h"        /* UProtoInstance — M4: IC table per nested proto */

/* --- UUpvalCell: runtime heap cell for captured locals (full definition).
 * Forward typedef is in uframe.h; full layout lives here because the struct
 * embeds UCell (from gc/ugc.h) as its FIRST member, and uframe.h cannot
 * include ugc.h without forming a circular include (uframe.h → ugc.h →
 * umodule.h → uframe.h).  This header has both UValue (via umodule.h) and
 * UCell (via gc/ugc.h) in scope, so the full definition is safe here.
 *
 * When a closure captures a local still live on a call frame stack, the cell
 * points into the register window (on_heap=false).  When the scope exits
 * (OP_CLOSE), the value is copied into the cell itself and on_heap is set to
 * true.
 *
 * Layout (v0.8.4): UCell prefix at offset 0 for GC; type_tag is initialised
 * to UTYPE_UPVAL_CELL at allocation.  sizeof stays 32 B on 64-bit hosts —
 * the compiler-inserted padding that previously preceded the 8-aligned union
 * now holds the UCell header (bytes 0-1); on_heap moves to byte 2. */
struct UUpvalCell {
    UCell   cell;               /* 2 B — type_tag = UTYPE_UPVAL_CELL at offset 0/1 */
    bool    on_heap;            /* byte 2 */
    /* 5 B compiler padding on 64-bit (aligns the 16-B UValue union to 8 B) */
    union {
        UValue  *stack_ptr;     /* on_heap=false: pointer into register window */
        UValue   value;         /* on_heap=true:  owned copy                   */
    } u;
    struct UUpvalCell *next;    /* intrusive singly-linked list in strand */
};

/* Layout pin (v0.8.4): UUpvalCell embeds UCell at offset 0 for GC.
 * sizeof stays 32 B on 64-bit (compiler already padded on_heap to the
 * 8-aligned union); the UCell prefix occupies bytes 0-1 where
 * compiler-inserted padding lived before.
 *
 * On 32-bit hosts the union/pointer alignment is 4-byte, so the layout
 * differs — pin host-pointer-size cases independently. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
_Static_assert(sizeof(UUpvalCell) == 32,
               "UUpvalCell size pin on 64-bit hosts (v0.8.4)");
#endif

/* --- UClosure: runtime function value (proto + captured upvalues).
 * Heap-allocated by OP_CLOSURE; lives until end-of-run via the strand's
 * pre-GC closure_list (threaded by next_alloc).  The cell header is
 * initialised (type_tag + gc_byte) so the upvalue write barrier can
 * safely cast UClosure* → UCell*, but the closure is NOT yet enrolled on
 * vm->all_cells_head — the strand-local closure_list still owns lifetime
 * pre-GC (full GC enrollment deferred to v1.x).
 *
 * proto_inst is bound end-to-end by the M4 follow-up landed on `main`
 * 2026-05-03: OP_CLOSURE writes
 * `s->module_instance->proto_instances->entries[bx + 1]` here so
 * OP_GETSLOT/OP_SETSLOT can read the per-(vm,proto) IC table off this
 * field.  NULL only when no module_instance was wired (defensive — does
 * not occur in production paths from urbi_vm_run / urbi_run_chunk).
 *
 * The upvals[] array is a trailing flexible member — allocate
 * sizeof(UClosure) + (nupvals - 1) * sizeof(UUpvalCell*).
 * `next_alloc` threads all closures allocated in one run into a free list
 * so they can be reclaimed at halt (pre-GC bookkeeping). */
/* Native-method extension typedef — promoted to the public API at v0.7.1
 * (<urbi/urbi.h>).  The definition is identical in both locations; the
 * guard URBI_NATIVE_METHOD_FN_DEFINED prevents a duplicate-typedef error
 * in translation units that include both headers. */
struct UVM;
#ifndef URBI_NATIVE_METHOD_FN_DEFINED
#define URBI_NATIVE_METHOD_FN_DEFINED
typedef int (*urbi_native_method_fn)(struct UVM *vm,
                                     UValue self,
                                     UValue *args,
                                     uint8_t nargs,
                                     UValue *out);
#endif /* URBI_NATIVE_METHOD_FN_DEFINED */

struct UClosure {
    UCell             cell;        /* 2 B — type_tag + gc_byte at offset 0/1 */
    /* Compiler-inserted padding before the next pointer field.
     * 64-bit targets: 6 B (cell at 0..1, pad 2..7, proto at 8).
     * 32-bit targets: 2 B (cell at 0..1, pad 2..3, proto at 4).
     * Either way the field order below matches natural pointer alignment;
     * no manual pad bytes are inserted. */
    UProto           *proto;
    /* origin_nested [runtime-only]: pointer to the nested[] proto array of
     * the UModule this closure was compiled in.  Set at OP_CLOSURE creation
     * from s->module->nested.  Used by OP_CLOSURE inside the callee to look
     * up sibling nested protos via the current frame's closure rather than
     * s->module->nested (which points at the TOP-LEVEL session module, not
     * the callee's originating module).
     *
     * Lifetime: v0.8.1 Variant B rescues the whole root_proto (which carries
     * the nested[] array) to vm->rescued_protos when umodule_destroy is called
     * while root_proto->refcount > 0 (surviving closures).  The array remains
     * valid until vm_destroy frees all rescued root_protos.  The old
     * urbi_steal_repl_protos (per-nested steal) is deleted at v0.8.1.
     *
     * NULL for native-fn closures and closures whose module has no nested
     * protos (nupvals == 0 closures that don't create inner functions).
     * OP_CLOSURE falls back to s->module->nested when NULL. */
    struct UProto   **origin_nested;
    uint16_t          origin_nested_count;
    UProtoInstance   *proto_inst;  /* M4 follow-up: per-(vm,proto) IC tier
                                      pointer.  See banner above for
                                      binding/lifecycle. */
    /* origin_module_instance [runtime-only]: the UModuleInstance that was
     * active at OP_CLOSURE creation time.  Set from s->module_instance.
     * Used by OP_CLOSURE inside the callee to resolve proto_inst entries
     * for inner closures via origin_module_instance->proto_instances->
     * entries[bx+1], even when s->module_instance belongs to a different
     * session.  NULL for native-fn closures (those never execute bytecode).
     * Lifetime: UModuleInstance objects are GC-managed and remain valid
     * for the VM's lifetime once created. */
    struct UModuleInstance *origin_module_instance;
    struct UClosure  *next_alloc; /* legacy free-list link (lifetime owner pre-GC) */
    /* M6 Phase 3: C-native method dispatch. NULL for ordinary urbiscript
     * closures.  When non-NULL, OP_CALL calls this function instead of
     * pushing a bytecode frame.  proto / proto_inst / upvals are NULL on
     * native closures (GC trace already guards each via NULL checks).
     * The receiver (`self`) is sourced from R[A+1] when OP_CALL's C
     * carries the method-flag bit (set by a preceding OP_SELF, v1.6 S42);
     * plain calls pass nil as self. */
    urbi_native_method_fn native_fn;
    uint8_t           nupvals;
    UUpvalCell       *upvals[1];  /* flexible trailing array of pointers */
};

#endif /* UCLOSURE_H */
