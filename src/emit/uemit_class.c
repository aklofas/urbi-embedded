/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_class.c — class declaration emit (T38 desugar).
 *
 * Phase 6 of M6 stdlib.  Per spec §8 T38, class Foo : public A, B { body }
 * desugars to:
 *
 *   var Foo = Object.clone()
 *   Foo.protos.insertFront(B)        # protos in REVERSE order
 *   Foo.protos.insertFront(A)        # so chain ends in declaration order
 *   <body emitted with Foo as receiver>
 *
 * S-emit-freereg-discipline (carry-forward from v0.5.7-fixes): each
 * emitted call (clone, insertFront, body slot-set) is a sibling site
 * with its own register-allocation drift hazard.  Sync freereg to
 * next_reg between sibling sites — same fix shape as emit_at_event_arm
 * in uemit_react.c.
 *
 * Wave 1 body-statement support: AST_VAR_DECL (`var x = expr`) and
 * AST_FUNCTION (`var f = function() { ... }`) emit as Foo.setSlot.
 * Other statement kinds raise EMIT_UNSUPPORTED_AST. */

#include "emit/uemit_internal.h"
#include "value/uintern.h"
#include "parse/uast.h"
#include "module/umodule.h"
#include "runtime/umacros.h"
#include <stddef.h>
#include <stdint.h>

/* === emit_class_body_stmt — emit one body statement as Foo.<name> = value.
 *
 * Each iteration is a sibling call site relative to the prior emit; the
 * caller MUST sync freereg to next_reg before invoking us so OP_CLOSURE
 * destinations (when init is AST_FUNCTION) do not clobber a still-live
 * value held above the freereg cursor. === */
static void
emit_class_body_stmt(UEmitter *e, UAstNode *stmt, uint8_t foo_reg)
{
    if (stmt == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return;
    }

    /* Wave 1: only var-decl is supported in the class body.  The init
     * may be any expression including an AST_FUNCTION literal — that
     * case naturally produces a closure value that gets installed as a
     * slot-method. */
    if (stmt->kind != AST_VAR_DECL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return;
    }

    const char *canonical = ustr_intern(e->vm,
                                        stmt->u.var_decl.name_start,
                                        (size_t)stmt->u.var_decl.name_len);
    if (canonical == NULL) { e->error = EMIT_OOM; return; }

    int ic = uemit_assign_ic_index(e, (USymbol *)canonical);
    if (ic < 0) return;

    /* Emit init expression into a fresh temp.  Sync freereg up to
     * next_reg first so emit_function_literal (when init is AST_FUNCTION)
     * uses freereg as its OP_CLOSURE destination consistently with
     * next_reg.  Same fix shape as emit_at_event_arm. */
    if (e->current_fs->freereg < e->next_reg) {
        e->current_fs->freereg = e->next_reg;
    }
    uint8_t value_reg = emit_expr(e, stmt->u.var_decl.init);
    if (e->error != EMIT_OK) return;

    /* OP_SETSLOT A=src B=recv C=ic_idx — Foo.<name> = value. */
    emit_instr(e, uinstr_enc_abc(OP_SETSLOT, value_reg, foo_reg,
                                 (uint8_t)ic),
               (uint32_t)stmt->line);

    /* Release the value temp.  Use the freereg-synced variant when the
     * init was an AST_FUNCTION literal (emit_function_literal raises both
     * cursors); otherwise plain free_reg. */
    if (stmt->u.var_decl.init != NULL &&
        stmt->u.var_decl.init->kind == AST_FUNCTION) {
        free_reg_freereg_synced(e);
    } else {
        free_reg(e);
    }
}

/* === emit_class_decl_arm — top-level class declaration desugar.
 *
 * Returns the dest register holding the new class object (for use when
 * the class declaration appears as an expression in a separator chain).
 * Returns 0 with e->error set on failure. === */
uint8_t
emit_class_decl_arm(UEmitter *e, UAstNode *n)
{
    if (e->current_fs == NULL || e->vm == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    /* === Step 1: Foo = Object.clone() ===
     *
     * Build a synthetic AST_CALL of Object.clone() and dispatch through
     * the existing call/member-get arms.  Reusing the existing emit
     * machinery means we inherit lazy-arg context handling, IC alloc,
     * register discipline, and the realm-global resolution path for
     * "Object" without re-implementing any of it. */
    UAstNode obj_ident = (UAstNode){0};
    obj_ident.kind          = AST_IDENT;
    obj_ident.line          = n->line;
    obj_ident.col           = n->col;
    obj_ident.u.ident.start = "Object";
    obj_ident.u.ident.len   = 6;

    UAstNode clone_member = (UAstNode){0};
    clone_member.kind                 = AST_MEMBER_GET;
    clone_member.line                 = n->line;
    clone_member.col                  = n->col;
    clone_member.u.member.recv        = &obj_ident;
    clone_member.u.member.name_start  = "clone";
    clone_member.u.member.name_len    = 5;
    clone_member.u.member.value       = NULL;

    UAstNode clone_call = (UAstNode){0};
    clone_call.kind             = AST_CALL;
    clone_call.line             = n->line;
    clone_call.col              = n->col;
    clone_call.u.call.callee    = &clone_member;
    clone_call.u.call.args      = NULL;
    clone_call.u.call.arg_count = 0;

    uint8_t foo_reg = emit_expr(e, &clone_call);
    if (e->error != EMIT_OK) return 0U;

    /* Sync freereg to next_reg before the proto-insertion calls
     * (S-emit-freereg-discipline; matches the v0.5.7 fix at
     * AST_AT_EVENT in uemit_react.c).  emit_call_arm leaves freereg at
     * the local-zone boundary while next_reg points just past foo_reg;
     * subsequent OP_CLOSURE / OP_GETSLOT temps allocated through
     * alloc_reg use next_reg, so the cursors must agree. */
    if (e->current_fs->freereg < e->next_reg) {
        e->current_fs->freereg = e->next_reg;
    }

    /* === Step 2: For each proto in REVERSE order, emit
     *   Foo.protos().insertFront(proto)
     *
     * Reversed iteration so declaration-order ends up as the chain head:
     * `class F : public A, B` parses [A, B], we emit insertFront(B) then
     * insertFront(A); final chain is [A, B, Object] (S-mro-declaration-
     * order).
     *
     * `Foo.protos` is a method (obj_protos C-native) that returns the
     * synthetic proto-list UObject; the synthetic list has insertFront
     * installed on it (urbi_proto_list_create wires the method).  So
     * the call chain is: load Foo.protos → call to get list → look up
     * insertFront on list → call with proto arg.
     *
     * Each insertFront site is its own sibling call — sync freereg
     * between iterations. === */
    {
        int i;
        USymbol *sym_protos = (USymbol *)ustr_intern(e->vm, "protos", 6);
        if (sym_protos == NULL) { e->error = EMIT_OOM; return 0U; }
        USymbol *sym_insert = (USymbol *)ustr_intern(e->vm,
                                                    "insertFront", 11);
        if (sym_insert == NULL) { e->error = EMIT_OOM; return 0U; }

        for (i = n->u.class_decl.proto_count - 1; i >= 0; i--) {
            UAstNode *proto_node = n->u.class_decl.protos[i];

            uint8_t callee_reg = alloc_reg(e);
            if (e->error != EMIT_OK) return 0U;
            if (e->current_fs->freereg < e->next_reg) {
                e->current_fs->freereg = e->next_reg;
            }

            /* Each site needs its own IC slot — the receiver/value class
             * may differ across iterations and the IC is per-site. */
            int ic_protos = uemit_assign_ic_index(e, sym_protos);
            if (ic_protos < 0) return 0U;

            /* callee_reg = Foo.protos (the method closure). */
            emit_instr(e, uinstr_enc_abc(OP_GETSLOT, callee_reg, foo_reg,
                                         (uint8_t)ic_protos),
                       (uint32_t)n->line);

            /* OP_CALL Foo.protos() — 0 args + callee slot = B=1, C=2.
             * Result lands at callee_reg, replacing the method closure
             * with the synthetic protos-list UObject. */
            emit_instr(e, uinstr_enc_abc(OP_CALL, callee_reg, 1U, 2U),
                       (uint32_t)n->line);

            /* callee_reg now holds the protos list; look up insertFront. */
            int ic_insert = uemit_assign_ic_index(e, sym_insert);
            if (ic_insert < 0) return 0U;
            emit_instr(e, uinstr_enc_abc(OP_GETSLOT, callee_reg, callee_reg,
                                         (uint8_t)ic_insert),
                       (uint32_t)n->line);

            /* Sync freereg to next_reg before emitting the proto arg —
             * the arg expression may allocate its own temps. */
            if (e->current_fs->freereg < e->next_reg) {
                e->current_fs->freereg = e->next_reg;
            }

            /* Emit the proto argument; it should land at callee_reg+1. */
            uint8_t arg_reg = emit_expr(e, proto_node);
            if (e->error != EMIT_OK) return 0U;
            uint8_t expected = (uint8_t)(callee_reg + 1U);
            if (arg_reg != expected) {
                emit_instr(e, uinstr_enc_abc(OP_MOVE, expected, arg_reg, 0U),
                           (uint32_t)n->line);
            }

            /* OP_CALL list.insertFront(proto) — B=2 (1 arg + callee),
             * C=2 (1 ret, discarded). */
            emit_instr(e, uinstr_enc_abc(OP_CALL, callee_reg, 2U, 2U),
                       (uint32_t)n->line);

            /* Result is in callee_reg; we discard it.  Release the
             * scratch zone above foo_reg. */
            e->next_reg = (uint8_t)(foo_reg + 1U);
            if (e->current_fs->freereg > e->next_reg) {
                e->current_fs->freereg = e->next_reg;
            }

            /* Re-sync between sibling proto-insertion calls
             * (S-emit-freereg-discipline). */
            if (e->current_fs->freereg < e->next_reg) {
                e->current_fs->freereg = e->next_reg;
            }
        }
    }

    /* === Step 3: body[Foo] — walk body statements and install each
     * var-decl/function-decl as a slot on Foo.
     *
     * Wave 1 simplification: only AST_VAR_DECL is supported (catches
     * `var x = 1` and `var f = function() { ... }` both via the same
     * arm).  Other body statement kinds raise EMIT_UNSUPPORTED_AST. === */
    if (n->u.class_decl.body != NULL) {
        UAstNode *body = n->u.class_decl.body;
        if (body->kind == AST_BLOCK) {
            int i;
            for (i = 0; i < body->u.block.count; i++) {
                /* Sync freereg between sibling body-stmt sites
                 * (S-emit-freereg-discipline). */
                if (e->current_fs->freereg < e->next_reg) {
                    e->current_fs->freereg = e->next_reg;
                }
                emit_class_body_stmt(e, body->u.block.stmts[i], foo_reg);
                if (e->error != EMIT_OK) return 0U;
                /* Reset scratch above foo_reg — body stmt is a side-effect
                 * site whose value isn't used. */
                e->next_reg = (uint8_t)(foo_reg + 1U);
                if (e->current_fs->freereg > e->next_reg) {
                    e->current_fs->freereg = e->next_reg;
                }
            }
        } else {
            /* Unexpected body shape (parser always emits AST_BLOCK). */
            e->error = EMIT_UNSUPPORTED_AST;
            return 0U;
        }
    }

    /* === Step 4: bind the class name as a var in the enclosing scope.
     *
     * Synthesize an AST_ASSIGN equivalent: at chunk-top this writes the
     * realm global; inside a function body this routes through the
     * existing local/upvalue resolver via emit_assign_arm.  But we don't
     * have a pre-bound local/global yet — the emit_var_decl_arm path
     * declares a fresh binding.  We simulate that here: build a
     * synthetic AST_VAR_DECL whose init is an AST_LOCAL_REF pointing at
     * foo_reg, but emit_expr doesn't dispatch AST_LOCAL_REF for read.
     *
     * Simpler approach: emit a synthetic AST_VAR_DECL with an AST_NIL
     * init, then OP_MOVE foo_reg into the local's slot.  But since
     * foo_reg already holds the value and is positioned at the top of
     * the live stack, a straight var-decl won't work either.
     *
     * Cleanest approach for chunk-top (where M6 stdlib classes live):
     * use emit_assign_arm-style realm-global write.  We synthesize an
     * AST_ASSIGN with foo_reg sourced via a no-op move and let the
     * assign arm route to either local-rewrite or realm-global SETSLOT.
     *
     * For Wave 1: support chunk-top (fs->parent == NULL) only — write
     * the class name into the realm global slot.  Inside-a-function
     * class declarations are EMIT_UNSUPPORTED_AST and deferred. === */
    UFuncState *fs = e->current_fs;
    USymbol *sym_name = (USymbol *)ustr_intern(e->vm,
                                               n->u.class_decl.name_start,
                                               (size_t)n->u.class_decl.name_len);
    if (sym_name == NULL) { e->error = EMIT_OOM; return 0U; }

    if (fs->parent != NULL) {
        /* Inside a function: defer to v1.x. */
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    /* Chunk-top: write Foo into the realm-global slot.  Reserve
     * r_global_slot if needed (same protocol as emit_var_decl_arm
     * chunk-top path). */
    if (!fs->references_global) {
        if (!fs->global_slot_reserved) {
            if (fs->freereg >= (uint8_t)(UFS_MAX_REGS - 1)) {
                e->error = EMIT_REG_EXHAUSTED;
                return 0U;
            }
            fs->r_global_slot = fs->freereg;
            fs->global_slot_reserved = true;
            fs->freereg++;
            if (fs->freereg > fs->max_reg_seen)
                fs->max_reg_seen = fs->freereg;
            e->next_reg = fs->freereg;
            if (e->next_reg > e->max_reg_seen)
                e->max_reg_seen = e->next_reg;
        }
        fs->references_global = true;
    }

    int ic_idx = uemit_assign_ic_index(e, sym_name);
    if (ic_idx < 0) return 0U;

    /* OP_SETSLOT foo_reg → realm.global_object.<name> */
    emit_instr(e, uinstr_enc_abc(OP_SETSLOT, foo_reg, fs->r_global_slot,
                                 (uint8_t)ic_idx),
               (uint32_t)n->line);

    /* Track the name in the chunk's global-vars list so subsequent
     * references via AST_IDENT / AST_ASSIGN resolve correctly. */
    if (fs->n_global_vars < UFS_MAX_LOCALS) {
        int gidx = fs->n_global_vars++;
        fs->global_var_names[gidx] = (const char *)sym_name;
        UFuncSig *gsig = &fs->global_var_sigs[gidx];
        urbi_zero(gsig, sizeof(*gsig));
    }

    /* Return foo_reg as the expression value (the class object). */
    return foo_reg;
}
