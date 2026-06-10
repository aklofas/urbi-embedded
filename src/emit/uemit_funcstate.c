/* SPDX-License-Identifier: BSD-3-Clause */
/* UFuncState lifecycle + upvalue cascade + block stack + IC index assign
 * + prologue_prepend_instr.
 *
 * Extracted from uemit.c (v0.5.4-decompose T13).
 * EMIT-034: prologue_prepend_instr split into proto_grow_for_prologue /
 * module_grow_for_prologue to name the two container paths. */

#include "emit/uemit_internal.h"

#include <stddef.h>
#include <stdint.h>
#include "emit/uemit.h"
#include "chunk/uchunk.h"
#include "runtime/umacros.h"
#include "value/uarena.h"
#include "value/uintern.h"    /* ustr_intern — for known-lazy pre-seed */

/* --- Upvalue cascade helpers --- */

/* Find `name` (interned) as a local in fs->actvars. Returns slot or -1. */
static int local_lookup_for_upvalue(UFuncState *fs, const char *name) {
    for (int i = fs->nactvar - 1; i >= 0; i--) {
        if (fs->actvars[i].name == name) return fs->actvars[i].slot;
    }
    return -1;
}

/* Find `name` already installed in fs->upvalue table. Returns idx or -1. */
static int upvalue_lookup(const UFuncState *fs, const char *name) {
    for (int i = 0; i < fs->nupvalues; i++) {
        if (fs->upvalues[i].name == name) return i;
    }
    return -1;
}

/* Install a new upvalue descriptor on fs. Returns the new index, or -1
 * (sets EMIT_UPVAL_EXHAUSTED) on overflow. */
static int upvalue_install(UEmitter *e, UFuncState *fs,
                           const char *name, int name_len,
                           uint8_t parent_idx, bool in_stack) {
    if (fs->nupvalues >= UFS_MAX_UPVALUES) {
        e->error = EMIT_UPVAL_EXHAUSTED;
        return -1;
    }
    int idx = fs->nupvalues++;
    fs->upvalues[idx].name     = name;
    fs->upvalues[idx].name_len = name_len;
    fs->upvalues[idx].idx      = parent_idx;
    fs->upvalues[idx].in_stack = in_stack;
    return idx;
}

/* Recursive upvalue cascade. Returns the upvalue index in fs's table, or -1
 * if name is not found in any enclosing scope. Marks the parent's actvar and
 * enclosing block as captured so OP_CLOSE fires on block exit. */
int find_or_install_upvalue(UEmitter *e, UFuncState *fs,
                            const char *name, int name_len) {
    if (fs->parent == NULL) return -1;       /* no enclosing scope to capture from */

    /* Short-circuit: already installed in this function's upvalue table. */
    int existing = upvalue_lookup(fs, name);
    if (existing >= 0) return existing;

    /* Check immediate parent's locals. */
    int parent_local = local_lookup_for_upvalue(fs->parent, name);
    if (parent_local >= 0) {
        /* Mark the parent actvar as captured and flag the enclosing block.
         * Scan INNERMOST-first to match local_lookup_for_upvalue — with
         * shadowing, the innermost declaration is the one captured
         * (refactor-3 FE-04). */
        for (int i = fs->parent->nactvar - 1; i >= 0; i--) {
            if (fs->parent->actvars[i].name == name) {
                fs->parent->actvars[i].is_captured = true;
                /* Find the innermost block in parent that contains this local. */
                for (int b = fs->parent->nblocks - 1; b >= 0; b--) {
                    if (fs->parent->blocks[b].first_local_idx <= i) {
                        fs->parent->blocks[b].has_captured = true;
                        break;
                    }
                }
                break;
            }
        }
        return upvalue_install(e, fs, name, name_len,
                               (uint8_t)parent_local, true);
    }

    /* Recurse into grandparent — intermediate frame captures via upvalue
     * (in_stack=false). */
    int grand_idx = find_or_install_upvalue(e, fs->parent, name, name_len);
    if (grand_idx < 0) return -1;
    return upvalue_install(e, fs, name, name_len,
                           (uint8_t)grand_idx, false);
}

/* --- UFuncState lifecycle --- */

UFuncState *uemit_open_function(UEmitter *e, UFuncState *parent) {
    if (e->error != EMIT_OK) return NULL;

    UFuncState *fs = uarena_alloc(e->arena, sizeof(UFuncState));
    if (fs == NULL) {
        e->error = EMIT_OOM;
        return NULL;
    }
    /* zero-init via byte-loop — UFuncState is POD */
    urbi_zero(fs, sizeof(UFuncState));
    fs->parent = parent;
    fs->target_proto = NULL;            /* T14 wires nested-proto bufs */

    /* T73: For the chunk-top funcstate (parent == NULL), pre-reserve
     * r_global_slot = 0 unconditionally, mirroring what emit_function_literal
     * does for nested functions.  This ensures r_global_slot is never
     * overwritten by a condition register (rd) in an if/while expression —
     * both would land at index 0 without the pre-reservation.  The prologue
     * OP_LOAD_REALM_GLOBAL is still only emitted iff references_global is
     * true; the reserved register is simply unused for pure-local chunks. */
    if (parent == NULL && fs->freereg < (uint8_t)(UFS_MAX_REGS - 1)) {
        fs->r_global_slot      = fs->freereg;
        fs->global_slot_reserved = true;
        fs->freereg++;
        if (fs->freereg > fs->max_reg_seen)
            fs->max_reg_seen = fs->freereg;
    }

    /* v0.10.10 / R5: pre-seed global_var_sigs for stdlib-defined functions
     * with lazy params.  The baked stdlib is compiled as a separate unit, so
     * their signatures are not visible when user code is compiled.  Pre-seeding
     * here ensures that call sites like `detach(expr)` get correct lazy-arg
     * wrapping (emit_lazy_thunk) without requiring the user to write
     * `detach(function() { expr })` explicitly.
     *
     * Only seeds the root FuncState (parent == NULL) when a VM is present
     * (vm->intern_table is required for ustr_intern). */
    if (parent == NULL && e->vm != NULL) {
        struct {
            const char *name;
            int         nparams;
            bool        lazy[16];
        } kKnownLazyGlobals[] = {
            { "detach", 1, { true } },
            { "disown", 1, { true } }
        };
        int ki;
        int ntable = (int)(sizeof kKnownLazyGlobals / sizeof kKnownLazyGlobals[0]);
        for (ki = 0; ki < ntable && fs->n_global_vars < UFS_MAX_LOCALS; ki++) {
            const char *interned = ustr_intern(e->vm,
                                               kKnownLazyGlobals[ki].name,
                                               urbi_strlen(kKnownLazyGlobals[ki].name));
            if (interned == NULL) continue;  /* OOM — skip gracefully */
            UFuncSig *gsig = &fs->global_var_sigs[fs->n_global_vars];
            int pi;
            gsig->resolved = true;
            gsig->nparams  = kKnownLazyGlobals[ki].nparams;
            for (pi = 0; pi < kKnownLazyGlobals[ki].nparams && pi < 16; pi++) {
                gsig->param_is_lazy[pi] = kKnownLazyGlobals[ki].lazy[pi];
            }
            fs->global_var_names[fs->n_global_vars] = interned;
            fs->n_global_vars++;
        }
    }

    e->current_fs = fs;
    return fs;
}

/* M4 T15: assign the next IC index for the current function.  Allocates
 * fs->ic_names lazily in 16/32/64/128/256-slot chunks via the module
 * allocator (matches the rest of the emitter — funcstate itself is
 * arena-allocated, but variable-sized side tables go through the module
 * allocator so they survive into the proto and are freed via
 * uproto_destroy_buffers). */
int uemit_assign_ic_index(UEmitter *e, USymbol *name) {
    if (e == NULL || e->current_fs == NULL) return -1;
    UFuncState *fs = e->current_fs;
    if (fs->ic_next >= 256U) {
        e->error = EMIT_TOO_MANY_IC_SITES;
        return -1;
    }
    if (fs->ic_next >= fs->ic_names_cap) {
        uint16_t new_cap = (fs->ic_names_cap == 0U) ? 16U
            : (fs->ic_names_cap < 128U ? (uint16_t)(fs->ic_names_cap * 2U) : 256U);
        UProto *ic_rp = e->module;
        if (ic_rp == NULL) { e->error = EMIT_OOM; return -1; }
        UChunkAllocFn alloc = emit_alloc_for(ic_rp);
        if (alloc == NULL) { e->error = EMIT_OOM; return -1; }
        /* TIDY-005: explicit (void *) cast on the inout pointer prevents
         * bugprone-multi-level-implicit-pointer-conversion from firing on
         * USymbol ** → void * decay through alloc's first argument. */
        USymbol **fresh = (USymbol **)alloc((void *)fs->ic_names,
                                            (size_t)new_cap * sizeof(USymbol *),
                                            ic_rp->alloc_ud);
        if (fresh == NULL) { e->error = EMIT_OOM; return -1; }
        fs->ic_names = fresh;
        fs->ic_names_cap = new_cap;
    }
    int idx = (int)fs->ic_next++;
    fs->ic_names[idx] = name;
    return idx;
}

/* --- EMIT-034: prologue_prepend_instr split into per-container helpers --- */

/* Grow and prepend one instruction into a nested UProto.
 * Shifts instructions, line_deltas, and abs_lines rightward by one.
 * Patches OP_TRY_BEGIN / OP_PUSH_TAG Bx fields (absolute PCs).
 * Returns true on success; false + sets e->error on OOM. */
static bool proto_grow_for_prologue(UEmitter *e, UProto *p, uint32_t instr) {
    /* Instructions: grow by 1, shift right, insert at [0]. */
    if (!proto_grow(e->module, p,
                    (void **)&p->instructions, &p->instr_cap,
                    p->instr_count + 1U, sizeof(uint32_t))) {
        e->error = EMIT_OOM; return false;
    }
    if (p->instr_count > 0U) {
        emit_memmove_right(p->instructions + 1, p->instructions,
                           p->instr_count * sizeof(uint32_t));
    }
    p->instructions[0] = instr;
    p->instr_count++;

    /* Patch instructions that store absolute PCs in their Bx field.
     * OP_TRY_BEGIN Bx = handler_pc; OP_PUSH_TAG Bx = onleave_pc.
     * All targets shifted right by 1 — increment by 1. */
    for (size_t pi = 1U; pi < p->instr_count; pi++) {
        UOpcode op = uinstr_op(p->instructions[pi]);
        if (op == OP_TRY_BEGIN || op == OP_PUSH_TAG) {
            uint8_t  a  = uinstr_a(p->instructions[pi]);
            uint16_t bx = uinstr_bx(p->instructions[pi]);
            p->instructions[pi] = uinstr_enc_abx(op, a, (uint16_t)(bx + 1U));
        }
    }

    /* line_deltas: resize to new instr_count, shift right, insert at [0].
     * line_deltas uses no separate cap — allocate exactly instr_count bytes. */
    {
        UChunkAllocFn alloc = p->alloc_fn;
#if __STDC_HOSTED__
        if (alloc == NULL) alloc = emit_stdlib_alloc;
#else
        if (alloc == NULL) { e->error = EMIT_OOM; return false; }
#endif
        int8_t *fresh = (int8_t *)alloc(p->line_deltas,
                                        p->instr_count * sizeof(int8_t),
                                        p->alloc_ud);
        if (fresh == NULL) { e->error = EMIT_OOM; return false; }
        p->line_deltas = fresh;
    }
    if (p->instr_count > 1U) {
        emit_memmove_right(p->line_deltas + 1, p->line_deltas,
                           (p->instr_count - 1U) * sizeof(int8_t));
    }
    p->line_deltas[0] = (int8_t)-128;   /* abs-checkpoint sentinel */

    /* Bump all existing abs_lines pc values. */
    for (size_t ai = 0; ai < p->abs_line_count; ai++) {
        p->abs_lines[ai].pc++;
    }
    /* Insert an abs_line entry at pc=0 using the first real instruction's
     * line (which is now at abs_lines[0].pc == 1 after the bump). */
    uint32_t line0 = 0U;
    if (p->abs_line_count > 0U && p->abs_lines[0].pc == 1U) {
        line0 = p->abs_lines[0].line;
    }
    if (!proto_grow(e->module, p,
                    (void **)&p->abs_lines, &p->abs_line_cap,
                    p->abs_line_count + 1U, sizeof(UAbsLine))) {
        e->error = EMIT_OOM; return false;
    }
    if (p->abs_line_count > 0U) {
        emit_memmove_right(p->abs_lines + 1, p->abs_lines,
                           p->abs_line_count * sizeof(UAbsLine));
    }
    p->abs_lines[0].pc   = 0U;
    p->abs_lines[0].line = line0;
    p->abs_line_count++;

    return true;
}

/* Grow and prepend one instruction into the root chunk (root proto).
 * v0.9.2: e->module IS the root UProto; rp == e->module. */
static bool module_grow_for_prologue(UEmitter *e, uint32_t instr) {
    UProto  *rp = e->module;
    if (rp == NULL) { e->error = EMIT_OOM; return false; }

    /* Instructions. */
    if (!emit_grow(rp, (void **)&rp->instructions, &rp->instr_cap,
                   rp->instr_count + 1U, sizeof(uint32_t))) {
        e->error = EMIT_OOM; return false;
    }
    if (rp->instr_count > 0U) {
        emit_memmove_right(rp->instructions + 1, rp->instructions,
                           rp->instr_count * sizeof(uint32_t));
    }
    rp->instructions[0] = instr;
    rp->instr_count++;

    /* Patch absolute-PC instructions shifted right by 1. */
    for (size_t pi = 1U; pi < rp->instr_count; pi++) {
        UOpcode op = uinstr_op(rp->instructions[pi]);
        if (op == OP_TRY_BEGIN || op == OP_PUSH_TAG) {
            uint8_t  a  = uinstr_a(rp->instructions[pi]);
            uint16_t bx = uinstr_bx(rp->instructions[pi]);
            rp->instructions[pi] = uinstr_enc_abx(op, a, (uint16_t)(bx + 1U));
        }
    }

    /* line_deltas: reallocate to new instr_count bytes, shift, insert. */
    {
        UChunkAllocFn alloc = emit_alloc_for(rp);
        if (alloc == NULL) { e->error = EMIT_OOM; return false; }
        int8_t *fresh = (int8_t *)alloc(rp->line_deltas,
                                        rp->instr_count * sizeof(int8_t),
                                        rp->alloc_ud);
        if (fresh == NULL) { e->error = EMIT_OOM; return false; }
        rp->line_deltas = fresh;
    }
    if (rp->instr_count > 1U) {
        emit_memmove_right(rp->line_deltas + 1, rp->line_deltas,
                           (rp->instr_count - 1U) * sizeof(int8_t));
    }
    rp->line_deltas[0] = (int8_t)-128;

    /* Bump existing abs_lines pc values. */
    for (size_t ai = 0; ai < rp->abs_line_count; ai++) {
        rp->abs_lines[ai].pc++;
    }
    uint32_t line0 = 0U;
    if (rp->abs_line_count > 0U && rp->abs_lines[0].pc == 1U) {
        line0 = rp->abs_lines[0].line;
    }
    if (!emit_grow(rp, (void **)&rp->abs_lines, &rp->abs_line_cap,
                   rp->abs_line_count + 1U, sizeof(UAbsLine))) {
        e->error = EMIT_OOM; return false;
    }
    if (rp->abs_line_count > 0U) {
        emit_memmove_right(rp->abs_lines + 1, rp->abs_lines,
                           rp->abs_line_count * sizeof(UAbsLine));
    }
    rp->abs_lines[0].pc   = 0U;
    rp->abs_lines[0].line = line0;
    rp->abs_line_count++;

    return true;
}

/* T73: Prepend one instruction to the current function's instruction buffer.
 *
 * Routes to UProto (nested function) via proto_grow_for_prologue or to
 * root UProto (chunk root) via module_grow_for_prologue.
 *
 * All abs_lines entries' pc values are bumped by 1 because every existing
 * instruction is now at pc+1.  The prepended instruction itself gets a
 * line_delta entry of INT8_MIN (abs-checkpoint sentinel) with an abs_line
 * entry at pc=0 pointing at the first pre-existing instruction's line so
 * that a debugger can attribute the prologue to the function's start.
 *
 * Returns true on success, false on OOM (sets e->error). */
static bool prologue_prepend_instr(UEmitter *e, uint32_t instr) {
    UProto *p = (e->current_fs && e->current_fs->target_proto)
                ? (UProto *)e->current_fs->target_proto
                : NULL;

    if (p != NULL) {
        return proto_grow_for_prologue(e, p, instr);
    } else {
        return module_grow_for_prologue(e, instr);
    }
}

UFuncState *uemit_close_function(UEmitter *e) {
    UFuncState *fs = e->current_fs;
    if (fs == NULL) return NULL;

    /* T73: Frame prologue — prepend OP_LOAD_REALM_GLOBAL as the first
     * instruction iff this function body referenced at least one realm
     * global.  This guarantees r_global_slot holds realm->global_object
     * before any OP_GETSLOT / OP_SETSLOT that targets the global object,
     * even when the first global reference is inside a branch arm that
     * may not be taken at runtime.  Pure-local functions are left
     * prologue-free (no wasted instruction).
     *
     * T20 (EMIT-003): capture prologue_prepend_instr's return value.  On
     * OOM (instruction-buffer / line-table grow failure) it sets
     * e->error = EMIT_OOM internally.  The IC-array branches below gate
     * on e->error == EMIT_OK so a prologue failure cleanly short-circuits
     * the rest of close — no wasted IC-array allocation against a proto
     * whose instructions buffer is in an indeterminate partial-shift
     * state.  The cleanup-only tail (free fs->ic_names, reset prev_line,
     * pop e->current_fs) still runs unconditionally. */
    if (fs->references_global && e->error == EMIT_OK) {
        uint32_t prologue = uinstr_enc_abc(OP_LOAD_REALM_GLOBAL,
                                           fs->r_global_slot, 0U, 0U);
        if (!prologue_prepend_instr(e, prologue)) {
            /* prologue_prepend_instr already set e->error = EMIT_OOM. */
        }
    }

    /* Roll max_reg_seen into target_proto when closing a nested function.
     * Skipped on prior error to avoid touching a proto whose instructions
     * buffer may be in a half-prepended state. */
    if (fs->target_proto != NULL && e->error == EMIT_OK) {
        UProto *p = (UProto *)fs->target_proto;
        p->max_reg  = fs->max_reg_seen;
        p->nupvals  = (uint8_t)fs->nupvalues;

        /* M4 T15: copy IC names side table into the UProto.  Use the
         * proto's own allocator (inherited from the module at
         * uproto_alloc_nested time); the resulting array is freed
         * by uproto_destroy_buffers.
         *
         * T22 (EMIT-005): mirror the module-sibling pattern below — only
         * write p->ic_count / p->ic_names after the IC-array allocation
         * succeeds.  Pre-fix, the proto path assigned p->ic_count first,
         * then reset it to 0 on OOM (silent zeroing).  The new shape
         * leaves p->ic_count at its zero-init value when allocation
         * fails and propagates the failure via e->error alone, matching
         * the module path. */
        if (fs->ic_next > 0U) {
            UChunkAllocFn palloc = p->alloc_fn;
#if __STDC_HOSTED__
            if (palloc == NULL && e->module != NULL) palloc = emit_alloc_for(e->module);
#endif
            if (palloc == NULL) {
                e->error = EMIT_OOM;
            } else {
                USymbol **dst = (USymbol **)palloc(NULL,
                    (size_t)fs->ic_next * sizeof(USymbol *), p->alloc_ud);
                if (dst == NULL) {
                    e->error = EMIT_OOM;
                } else {
                    for (uint16_t i = 0; i < fs->ic_next; i++) {
                        dst[i] = fs->ic_names[i];
                    }
                    p->ic_count = fs->ic_next;
                    p->ic_names = dst;
                }
                /* T11: parallel char** companion array.  USymbol* is a const-char*
                 * canonical interned pointer (from ustr_intern); each entry is a
                 * NUL-terminated allocator-owned strdup so the deserializer can
                 * populate ic_name_strs without an originating VM intern table. */
                if (p->ic_names != NULL) {
                    char **dst_strs = (char **)palloc(NULL,
                        (size_t)p->ic_count * sizeof(char *), p->alloc_ud);
                    if (dst_strs == NULL) {
                        e->error = EMIT_OOM;
                    } else {
                        for (uint16_t i = 0; i < p->ic_count; i++) {
                            dst_strs[i] = NULL;
                        }
                        for (uint16_t i = 0; i < p->ic_count; i++) {
                            const char *name = (const char *)fs->ic_names[i];
                            size_t nlen = (name != NULL) ? urbi_strlen(name) : 0U;
                            char *dup = (char *)palloc(NULL, nlen + 1U, p->alloc_ud);
                            if (dup == NULL) {
                                e->error = EMIT_OOM;
                                break;
                            }
                            if (nlen > 0U) emit_memcpy(dup, name, nlen);
                            dup[nlen] = '\0';
                            dst_strs[i] = dup;
                        }
                        p->ic_name_strs = dst_strs;
                    }
                }
            }
        } else {
            p->ic_names = NULL;
            p->ic_name_strs = NULL;
        }
    }
    /* v0.9.2: top-level funcstate (no target_proto) — copy IC names
     * into root->ic_count / ic_names.  e->module IS the root UProto.
     * Mirrors the UProto path above. */
    if (fs->target_proto == NULL && fs->parent == NULL && fs->ic_next > 0U) {
        UProto *rp = e->module;
        if (rp == NULL) {
            e->error = EMIT_OOM;
        } else {
            UChunkAllocFn malloc_fn = emit_alloc_for(rp);
            if (malloc_fn == NULL) {
                e->error = EMIT_OOM;
            } else {
                USymbol **dst = (USymbol **)malloc_fn(NULL,
                    (size_t)fs->ic_next * sizeof(USymbol *), rp->alloc_ud);
                if (dst == NULL) {
                    e->error = EMIT_OOM;
                } else {
                    for (uint16_t i = 0; i < fs->ic_next; i++) {
                        dst[i] = fs->ic_names[i];
                    }
                    rp->ic_count = fs->ic_next;
                    rp->ic_names = dst;
                }
            }
        }
        /* T11: parallel char** companion array for the root chunk. */
        if (rp != NULL && rp->ic_names != NULL) {
            UChunkAllocFn malloc_fn = emit_alloc_for(rp);
            char **dst_strs = (char **)malloc_fn(NULL,
                (size_t)rp->ic_count * sizeof(char *), rp->alloc_ud);
            if (dst_strs == NULL) {
                e->error = EMIT_OOM;
            } else {
                for (uint16_t i = 0; i < rp->ic_count; i++) {
                    dst_strs[i] = NULL;
                }
                for (uint16_t i = 0; i < rp->ic_count; i++) {
                    const char *name = (const char *)fs->ic_names[i];
                    size_t nlen = (name != NULL) ? urbi_strlen(name) : 0U;
                    char *dup = (char *)malloc_fn(NULL, nlen + 1U,
                                                   rp->alloc_ud);
                    if (dup == NULL) {
                        e->error = EMIT_OOM;
                        break;
                    }
                    if (nlen > 0U) emit_memcpy(dup, name, nlen);
                    dup[nlen] = '\0';
                    dst_strs[i] = dup;
                }
                rp->ic_name_strs = dst_strs;
            }
        }
    }
    /* Always free the funcstate-side IC array (allocated via the root
     * allocator).  For nested funcstates the names were copied into UProto
     * above; for top-level funcstates they were copied into root UProto by the
     * block above.  Either way the funcstate-side buffer is now redundant. */
    if (fs->ic_names != NULL) {
        UProto *free_rp = e->module;
        if (free_rp != NULL) {
            UChunkAllocFn alloc = emit_alloc_for(free_rp);
            if (alloc != NULL) {
                /* TIDY-005: explicit (void *) cast on free path. */
                alloc((void *)fs->ic_names, 0, free_rp->alloc_ud);
            }
        }
        fs->ic_names = NULL;
        fs->ic_names_cap = 0;
    }

    /* Restore prev_line to a sentinel so the parent proto's next instruction
     * bootstraps a fresh abs checkpoint after re-entering. This is a subtle
     * correctness point: without this reset, the first instruction of the
     * parent emitted after the CLOSURE prelude would compute a delta against
     * the last line of the child body, producing a wrong delta. */
    if (fs->parent != NULL && fs->target_proto != NULL) {
        e->prev_line = 0U;
    }
    e->current_fs = fs->parent;
    return fs;
}

int uemit_declare_local(UEmitter *e, const char *name, int name_len) {
    UFuncState *fs = e->current_fs;
    if (fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return -1;
    }
    /* Search current block (or whole actvars table at no-block scope) for
     * duplicate. T7 will use blocks[].first_local_idx; for T6 we just
     * scan from 0. */
    int search_from = (fs->nblocks > 0)
                    ? fs->blocks[fs->nblocks - 1].first_local_idx
                    : 0;
    for (int i = search_from; i < fs->nactvar; i++) {
        if (fs->actvars[i].name == name) {
            e->error = EMIT_LOCAL_REDECLARE;
            return -1;
        }
    }
    if (fs->nactvar >= UFS_MAX_LOCALS) {
        e->error = EMIT_REG_EXHAUSTED;
        return -1;
    }
    ULocalVar *lv = &fs->actvars[fs->nactvar];
    lv->name = name;
    lv->name_len = name_len;
    lv->slot = fs->freereg;
    lv->is_captured = false;
    lv->is_lazy = false;
    fs->nactvar++;
    fs->freereg++;
    if (fs->freereg > fs->max_reg_seen) fs->max_reg_seen = fs->freereg;

    /* Keep the emitter's scratch cursor in sync with the local zone's new
     * top.  Without this, callers that don't manually sync afterward (the
     * catch-handler emit was the first one found, 2026-05-16, S45) will
     * have the next `alloc_reg` collide with the local's slot — the local
     * gets clobbered by the first temp the surrounding expression
     * allocates.  Pre-fix: catch variable `e` in `try { throw "x" } catch
     * (e) { Realm.caught = e }` was overwritten with the Realm object
     * (kind=8 / UVAL_OBJECT on host, kind=5 / UVAL_CLOSURE on ESP32)
     * before the body could read it.
     *
     * The function-param emit path (uemit_stmt.c after the params loop)
     * does this same sync explicitly; making it part of the
     * uemit_declare_local contract removes the footgun for all future
     * callers — they can no longer forget. */
    if (e->next_reg < fs->freereg) {
        e->next_reg = fs->freereg;
        if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    }
    return lv->slot;
}

bool uemit_open_block(UEmitter *e, bool is_loop) {
    UFuncState *fs = e->current_fs;
    if (fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return false;
    }
    if (fs->nblocks >= UFS_MAX_BLOCKS) {
        e->error = EMIT_NESTING_TOO_DEEP;
        return false;
    }
    UBlockCtx *blk = &fs->blocks[fs->nblocks++];
    blk->nactvar_on_enter  = fs->nactvar;
    blk->freereg_on_enter  = fs->freereg;
    blk->first_local_idx   = fs->nactvar;
    blk->is_loop = is_loop;
    blk->has_captured = false;
    return true;
}

bool uemit_close_block(UEmitter *e) {
    UFuncState *fs = e->current_fs;
    if (fs == NULL || fs->nblocks == 0) {
        e->error = EMIT_UNSUPPORTED_AST;
        return false;
    }

    const UBlockCtx *blk = &fs->blocks[fs->nblocks - 1];

    /* Per allocator spec: emit OP_CLOSE if any local in this block was
     * captured.  A = the first REGISTER SLOT of the block's locals — NOT
     * first_local_idx, which is an actvar INDEX; slots are offset from
     * indices by the reserved r_global_slot register, so the bare index
     * lands one register too low and prematurely heapifies the enclosing
     * function's last-declared local (refactor-3 FE-04 follow-on).
     * Guard first_local_idx < nactvar: a block whose only local was a
     * catch variable has it popped — and its cell closed — by
     * emit_catch_handler_section before block close, leaving has_captured
     * set with no live locals and nothing left to close. */
    if (blk->has_captured && blk->first_local_idx < fs->nactvar) {
        uint32_t i = uinstr_enc_abc(OP_CLOSE,
                                    fs->actvars[blk->first_local_idx].slot,
                                    0U, 0U);
        emit_instr(e, i, e->prev_line);
    }

    /* Pop actvars back to entry snapshot; restore freereg.
     * Restore from the snapshot saved at open time: this correctly handles
     * the case where r_global_slot was pre-reserved (freereg != nactvar). */
    fs->nactvar = blk->nactvar_on_enter;
    fs->freereg = blk->freereg_on_enter;
    fs->nblocks--;
    return true;
}

void uemit_emit_loop_back_close(UEmitter *e) {
    UFuncState *fs = e->current_fs;
    if (fs == NULL || fs->nblocks == 0) return;
    const UBlockCtx *blk = &fs->blocks[fs->nblocks - 1];
    /* Same operand + guard rationale as uemit_close_block above: OP_CLOSE A
     * is a register slot, not an actvar index (refactor-3 FE-04 follow-on). */
    if (blk->is_loop && blk->has_captured && blk->first_local_idx < fs->nactvar) {
        uint32_t i = uinstr_enc_abc(OP_CLOSE,
                                    fs->actvars[blk->first_local_idx].slot,
                                    0U, 0U);
        emit_instr(e, i, e->prev_line);
    }
}
