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
        /* Mark the parent actvar as captured and flag the enclosing block. */
        for (int i = 0; i < fs->parent->nactvar; i++) {
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

    e->current_fs = fs;
    return fs;
}

/* M4 T15: assign the next IC index for the current function.  Allocates
 * fs->ic_names lazily in 16/32/64/128/256-slot chunks via the module
 * allocator (matches the rest of the emitter — funcstate itself is
 * arena-allocated, but variable-sized side tables go through the module
 * allocator so they survive into the proto and are freed via
 * umodule_destroy_proto_buffers). */
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
        UModuleAllocFn alloc = emit_alloc_for(e->module);
        if (alloc == NULL) { e->error = EMIT_OOM; return -1; }
        USymbol **fresh = (USymbol **)alloc(fs->ic_names,
                                            (size_t)new_cap * sizeof(USymbol *),
                                            e->module->alloc_ud);
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
        UModuleAllocFn alloc = p->alloc_fn;
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

/* Grow and prepend one instruction into the root UModule.
 * Same shift/patch/line logic as proto_grow_for_prologue but targets
 * module->instructions / line_deltas / abs_lines. */
static bool module_grow_for_prologue(UEmitter *e, uint32_t instr) {
    UModule *m = e->module;

    /* Instructions. */
    if (!emit_grow(m, (void **)&m->instructions, &m->instr_cap,
                   m->instr_count + 1U, sizeof(uint32_t))) {
        e->error = EMIT_OOM; return false;
    }
    if (m->instr_count > 0U) {
        emit_memmove_right(m->instructions + 1, m->instructions,
                           m->instr_count * sizeof(uint32_t));
    }
    m->instructions[0] = instr;
    m->instr_count++;

    /* Patch absolute-PC instructions shifted right by 1. */
    for (size_t pi = 1U; pi < m->instr_count; pi++) {
        UOpcode op = uinstr_op(m->instructions[pi]);
        if (op == OP_TRY_BEGIN || op == OP_PUSH_TAG) {
            uint8_t  a  = uinstr_a(m->instructions[pi]);
            uint16_t bx = uinstr_bx(m->instructions[pi]);
            m->instructions[pi] = uinstr_enc_abx(op, a, (uint16_t)(bx + 1U));
        }
    }

    /* line_deltas: reallocate to new instr_count bytes, shift, insert. */
    {
        UModuleAllocFn alloc = emit_alloc_for(m);
        if (alloc == NULL) { e->error = EMIT_OOM; return false; }
        int8_t *fresh = (int8_t *)alloc(m->line_deltas,
                                        m->instr_count * sizeof(int8_t),
                                        m->alloc_ud);
        if (fresh == NULL) { e->error = EMIT_OOM; return false; }
        m->line_deltas = fresh;
    }
    if (m->instr_count > 1U) {
        emit_memmove_right(m->line_deltas + 1, m->line_deltas,
                           (m->instr_count - 1U) * sizeof(int8_t));
    }
    m->line_deltas[0] = (int8_t)-128;

    /* Bump existing abs_lines pc values. */
    for (size_t ai = 0; ai < m->abs_line_count; ai++) {
        m->abs_lines[ai].pc++;
    }
    uint32_t line0 = 0U;
    if (m->abs_line_count > 0U && m->abs_lines[0].pc == 1U) {
        line0 = m->abs_lines[0].line;
    }
    if (!emit_grow(m, (void **)&m->abs_lines, &m->abs_line_cap,
                   m->abs_line_count + 1U, sizeof(UAbsLine))) {
        e->error = EMIT_OOM; return false;
    }
    if (m->abs_line_count > 0U) {
        emit_memmove_right(m->abs_lines + 1, m->abs_lines,
                           m->abs_line_count * sizeof(UAbsLine));
    }
    m->abs_lines[0].pc   = 0U;
    m->abs_lines[0].line = line0;
    m->abs_line_count++;

    return true;
}

/* T73: Prepend one instruction to the current function's instruction buffer.
 *
 * Routes to UProto (nested function) via proto_grow_for_prologue or to
 * UModule (chunk root) via module_grow_for_prologue.
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
     * prologue-free (no wasted instruction). */
    if (fs->references_global && e->error == EMIT_OK) {
        uint32_t prologue = uinstr_enc_abc(OP_LOAD_REALM_GLOBAL,
                                           fs->r_global_slot, 0U, 0U);
        prologue_prepend_instr(e, prologue);
    }

    /* Roll max_reg_seen into target_proto when closing a nested function. */
    if (fs->target_proto != NULL) {
        UProto *p = (UProto *)fs->target_proto;
        p->max_reg  = fs->max_reg_seen;
        p->nupvals  = (uint8_t)fs->nupvalues;

        /* M4 T15: copy IC names side table into the UProto.  Use the
         * proto's own allocator (inherited from the module at
         * umodule_alloc_nested_proto time); the resulting array is freed
         * by umodule_destroy_proto_buffers. */
        p->ic_count = fs->ic_next;
        if (p->ic_count > 0U) {
            UModuleAllocFn palloc = p->alloc_fn;
#if __STDC_HOSTED__
            if (palloc == NULL) palloc = emit_alloc_for(e->module);
#endif
            if (palloc == NULL) {
                e->error = EMIT_OOM;
            } else {
                USymbol **dst = (USymbol **)palloc(NULL,
                    (size_t)p->ic_count * sizeof(USymbol *), p->alloc_ud);
                if (dst == NULL) {
                    e->error = EMIT_OOM;
                    p->ic_count = 0;
                    p->ic_names = NULL;
                } else {
                    for (uint16_t i = 0; i < p->ic_count; i++) {
                        dst[i] = fs->ic_names[i];
                    }
                    p->ic_names = dst;
                }
            }
        } else {
            p->ic_names = NULL;
        }
    }
    /* M4 follow-up: top-level funcstate (no target_proto) — copy IC names
     * into UModule.ic_count / ic_names so urbi_module_instance_create can
     * populate proto_instances->entries[0].  Mirrors the UProto path above. */
    if (fs->target_proto == NULL && fs->parent == NULL && fs->ic_next > 0U) {
        UModule *mod = e->module;
        UModuleAllocFn malloc_fn = emit_alloc_for(e->module);
        if (malloc_fn == NULL) {
            e->error = EMIT_OOM;
        } else {
            USymbol **dst = (USymbol **)malloc_fn(NULL,
                (size_t)fs->ic_next * sizeof(USymbol *), e->module->alloc_ud);
            if (dst == NULL) {
                e->error = EMIT_OOM;
            } else {
                for (uint16_t i = 0; i < fs->ic_next; i++) {
                    dst[i] = fs->ic_names[i];
                }
                mod->ic_count = fs->ic_next;
                mod->ic_names = dst;
            }
        }
    }
    /* Always free the funcstate-side IC array (allocated via the module
     * allocator).  For nested funcstates the names were copied into UProto
     * above; for top-level funcstates they were copied into UModule by the
     * block above.  Either way the funcstate-side buffer is now redundant. */
    if (fs->ic_names != NULL) {
        UModuleAllocFn alloc = emit_alloc_for(e->module);
        if (alloc != NULL) {
            alloc(fs->ic_names, 0, e->module->alloc_ud);
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
    blk->break_chain = -1;
    blk->continue_chain = -1;
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
     * captured. base = first slot to close = first_local_idx. */
    if (blk->has_captured) {
        uint32_t i = uinstr_enc_abc(OP_CLOSE, (uint8_t)blk->first_local_idx, 0U, 0U);
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
    if (blk->is_loop && blk->has_captured) {
        uint32_t i = uinstr_enc_abc(OP_CLOSE, (uint8_t)blk->first_local_idx, 0U, 0U);
        emit_instr(e, i, e->prev_line);
    }
}
