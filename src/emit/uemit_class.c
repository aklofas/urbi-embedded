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

/* === emit_class_body_property_decl — T41 in class body.
 *
 * Class-body `get name() { body }` and `set name(v) { body }`.  The
 * receiver is the class object held in foo_reg, but foo_reg is a raw
 * register slot (no AST node) — we can't reuse emit_property_decl_arm
 * (which builds an AST_MEMBER_GET on n->u.property_decl.recv).
 *
 * Inline the desugar:
 *   1. Load `setProperty` method from foo_reg via OP_GETSLOT.
 *   2. Load name and "oget"/"oset" string args via AST_STR through emit_expr.
 *   3. Compile the function literal to a closure.
 *   4. OP_CALL with [self=foo_reg, name, prop, closure] -> discard.
 *
 * Sibling-site freereg discipline matches emit_class_body_stmt's
 * existing pattern (S-emit-freereg-discipline).  Result is discarded
 * since property installation is a side-effecting statement. === */
static void
emit_class_body_property_decl(UEmitter *e, UAstNode *stmt, uint8_t foo_reg)
{
    const UAstMethodKind kind = stmt->u.property_decl.kind;
    const char *prop_name = (kind == UAST_METHOD_GETTER) ? "oget" : "oset";

    if (e->current_fs->freereg < e->next_reg) {
        e->current_fs->freereg = e->next_reg;
    }

    /* Load setProperty method from foo_reg into a fresh callee register. */
    USymbol *sym_setProp = (USymbol *)ustr_intern(e->vm, "setProperty", 11);
    if (sym_setProp == NULL) { e->error = EMIT_OOM; return; }
    int ic_setProp = uemit_assign_ic_index(e, sym_setProp);
    if (ic_setProp < 0) return;

    uint8_t callee_reg = alloc_reg(e);
    if (e->error != EMIT_OK) return;
    if (e->current_fs->freereg < e->next_reg) {
        e->current_fs->freereg = e->next_reg;
    }

    emit_instr(e, uinstr_enc_abc(OP_GETSLOT, callee_reg, foo_reg,
                                 (uint8_t)ic_setProp),
               (uint32_t)stmt->line);

    /* Move foo_reg into callee_reg+1 as the implicit `self` argument.
     * OP_CALL convention: A is callee, B is (nargs+1) where the +1 is
     * the implicit self-or-callee shim — but the class-decl-arm pattern
     * passes args at A+1..A+B-1 with B = nargs+1, so for setProperty
     * with 3 args the call shape is callee_reg=A, args at A+1..A+3,
     * B = 4.  The native dispatch reads vm->last_recv set by the
     * preceding OP_GETSLOT (which used foo_reg as B), so `self` arrives
     * correctly without an explicit move. */

    /* Arg 1: name string. */
    UAstNode name_str = (UAstNode){0};
    name_str.kind            = AST_STR;
    name_str.line            = stmt->line;
    name_str.col             = stmt->col;
    name_str.u.str_lit.bytes = stmt->u.property_decl.name_start;
    name_str.u.str_lit.len   = stmt->u.property_decl.name_len;
    if (e->current_fs->freereg < e->next_reg) {
        e->current_fs->freereg = e->next_reg;
    }
    uint8_t name_reg = emit_expr(e, &name_str);
    if (e->error != EMIT_OK) return;
    uint8_t expected_name = (uint8_t)(callee_reg + 1U);
    if (name_reg != expected_name) {
        emit_instr(e, uinstr_enc_abc(OP_MOVE, expected_name, name_reg, 0U),
                   (uint32_t)stmt->line);
    }

    /* Arg 2: "oget" or "oset" string. */
    UAstNode prop_str = (UAstNode){0};
    prop_str.kind            = AST_STR;
    prop_str.line            = stmt->line;
    prop_str.col             = stmt->col;
    prop_str.u.str_lit.bytes = prop_name;
    prop_str.u.str_lit.len   = 4;
    if (e->current_fs->freereg < e->next_reg) {
        e->current_fs->freereg = e->next_reg;
    }
    uint8_t prop_reg = emit_expr(e, &prop_str);
    if (e->error != EMIT_OK) return;
    uint8_t expected_prop = (uint8_t)(callee_reg + 2U);
    if (prop_reg != expected_prop) {
        emit_instr(e, uinstr_enc_abc(OP_MOVE, expected_prop, prop_reg, 0U),
                   (uint32_t)stmt->line);
    }

    /* Arg 3: the function literal (closure). */
    if (e->current_fs->freereg < e->next_reg) {
        e->current_fs->freereg = e->next_reg;
    }
    uint8_t func_reg = emit_expr(e, stmt->u.property_decl.func);
    if (e->error != EMIT_OK) return;
    uint8_t expected_func = (uint8_t)(callee_reg + 3U);
    if (func_reg != expected_func) {
        emit_instr(e, uinstr_enc_abc(OP_MOVE, expected_func, func_reg, 0U),
                   (uint32_t)stmt->line);
    }

    /* OP_CALL: B = (nargs + 1) = 4; C = 2 (1 ret, discarded). */
    emit_instr(e, uinstr_enc_abc(OP_CALL, callee_reg, 4U, 2U),
               (uint32_t)stmt->line);

    /* Discard the result and any trailing scratch above foo_reg. */
    e->next_reg = (uint8_t)(foo_reg + 1U);
    if (e->current_fs->freereg > e->next_reg) {
        e->current_fs->freereg = e->next_reg;
    }
}

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

    /* Multi-slot class body: parser folds `;`-separated statements into
     * AST_BIN_SEP / AST_NARY.  Recurse on lhs+rhs so each leaf is one
     * sibling Foo.<name> = … site, with freereg discipline between
     * iterations (matches existing class-body iterator at line ~368). */
    if (stmt->kind == AST_BIN_SEP) {
        if (e->current_fs->freereg < e->next_reg) {
            e->current_fs->freereg = e->next_reg;
        }
        emit_class_body_stmt(e, stmt->u.bin_sep.lhs, foo_reg);
        if (e->error != EMIT_OK) return;
        e->next_reg = (uint8_t)(foo_reg + 1U);
        if (e->current_fs->freereg > e->next_reg) {
            e->current_fs->freereg = e->next_reg;
        }
        emit_class_body_stmt(e, stmt->u.bin_sep.rhs, foo_reg);
        return;
    }
    if (stmt->kind == AST_NARY) {
        /* N-ary separator — iterate children with same freereg discipline */
        for (int i = 0; i < stmt->u.nary.count; i++) {
            if (e->current_fs->freereg < e->next_reg) {
                e->current_fs->freereg = e->next_reg;
            }
            emit_class_body_stmt(e, stmt->u.nary.children[i], foo_reg);
            if (e->error != EMIT_OK) return;
            e->next_reg = (uint8_t)(foo_reg + 1U);
            if (e->current_fs->freereg > e->next_reg) {
                e->current_fs->freereg = e->next_reg;
            }
        }
        return;
    }

    /* T41 (Wave 2): class-body `get name() {...}` / `set name(v) {...}`
     * — implicit receiver is the class object (foo_reg). */
    if (stmt->kind == AST_PROPERTY_DECL) {
        emit_class_body_property_decl(e, stmt, foo_reg);
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

/* === emit_property_decl_arm — T41 get/set parse sugar.
 *
 * AST_PROPERTY_DECL desugars to:
 *
 *   recv.setProperty(name, "oget"|"oset", function() body)
 *
 * where the function literal carries the params + body parsed by
 * parse_property_decl.  No new opcodes; runtime `oget`/`oset` slot-
 * property dispatch (M4 baseline) handles the trigger on slot read /
 * write.
 *
 * `n->u.property_decl.recv` must be non-NULL on entry.  Class-body
 * forms (`recv == NULL`) are routed via emit_class_property_decl_arm
 * which synthesizes the implicit class-object receiver. === */
uint8_t
emit_property_decl_arm(UEmitter *e, UAstNode *n)
{
    if (e->current_fs == NULL || e->vm == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    if (n->u.property_decl.recv == NULL) {
        /* Implicit-receiver form (class body or top-level `get x()`).
         * v0.6.1 supports the explicit-receiver form only at the AST arm;
         * class-body wiring lives in emit_class_body_stmt. */
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    const UAstMethodKind kind = n->u.property_decl.kind;
    const char *prop_name = (kind == UAST_METHOD_GETTER) ? "oget" : "oset";

    /* Build synthetic AST nodes for the desugar.  All on stack — they
     * borrow into the emitter for the duration of one emit_expr call,
     * which copies the bytes it needs (interned strings, function-
     * literal proto allocations, etc.) before returning. */
    UAstNode name_str = (UAstNode){0};
    name_str.kind            = AST_STR;
    name_str.line            = n->line;
    name_str.col             = n->col;
    name_str.u.str_lit.bytes = n->u.property_decl.name_start;
    name_str.u.str_lit.len   = n->u.property_decl.name_len;

    UAstNode prop_str = (UAstNode){0};
    prop_str.kind            = AST_STR;
    prop_str.line            = n->line;
    prop_str.col             = n->col;
    prop_str.u.str_lit.bytes = prop_name;
    prop_str.u.str_lit.len   = 4;   /* "oget" and "oset" both 4 bytes */

    UAstNode setprop_member = (UAstNode){0};
    setprop_member.kind                = AST_MEMBER_GET;
    setprop_member.line                = n->line;
    setprop_member.col                 = n->col;
    setprop_member.u.member.recv       = n->u.property_decl.recv;
    setprop_member.u.member.name_start = "setProperty";
    setprop_member.u.member.name_len   = 11;
    setprop_member.u.member.value      = NULL;

    UAstNode *args[3];
    args[0] = &name_str;
    args[1] = &prop_str;
    args[2] = n->u.property_decl.func;       /* AST_FUNCTION */

    UAstNode setprop_call = (UAstNode){0};
    setprop_call.kind             = AST_CALL;
    setprop_call.line             = n->line;
    setprop_call.col              = n->col;
    setprop_call.u.call.callee    = &setprop_member;
    setprop_call.u.call.args      = args;
    setprop_call.u.call.arg_count = 3;

    return emit_expr(e, &setprop_call);
}
