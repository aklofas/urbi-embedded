/* SPDX-License-Identifier: BSD-3-Clause */
/* uvm_op_overload.h — operator-method fallback dispatch (Gap #4, M6 Wave 3).
 *
 * Shared between uvm.c (call sites in dispatch arms) and uvm_op_overload.c
 * (implementation).  Not part of the public <urbi/> API. */

#ifndef UVM_OP_OVERLOAD_H
#define UVM_OP_OVERLOAD_H

#include <stdbool.h>
#include <stdint.h>

#include "object/uobject.h"   /* UValue, UObject, USymbol */

struct UVM;

/* Return codes for the fallback helpers. */
#define VM_OP_OVERLOAD_OK    0   /* slot found and called; result in *dst */
#define VM_OP_OVERLOAD_MISS  1   /* no usable slot found; emit original error */
#define VM_OP_OVERLOAD_OOM   2   /* allocation failure during lookup */
#define VM_OP_OVERLOAD_THREW 3   /* body raised: thrown value in *dst (or
                                    *out_thrown for the cmp variant) — caller
                                    must re-deposit it as a strand THROW,
                                    mirroring OP_THROW (refactor-3 VM-07) */

/* Binary operator fallback (arith_add / sub / mul / div type-errors).
 * op_name: interned operator symbol ("+", "-", "*", "/").
 * pc_off:  (size_t)(s->pc - s->pc_base) at the opcode — used as IC key.
 * On OK: *dst holds the return value of the overloaded method.
 * On MISS: caller should emit the original type error and HALT.
 * On THREW: *dst holds the user exception thrown by the overload body;
 * caller re-deposits it (s->unwind_value / pending_unwind = UEXEC_THROW). */
int vm_arith_method_fallback(struct UVM *vm,
                             UValue     *dst,
                             const UValue *lhs,
                             const UValue *rhs,
                             USymbol    *op_name,
                             uint32_t    pc_off);

/* Unary operator fallback (arith_neg type-error).
 * op_name: typically "-" (same slot name as binary minus).
 * THREW contract identical to the binary variant. */
int vm_arith_method_fallback_unary(struct UVM *vm,
                                   UValue     *dst,
                                   const UValue *operand,
                                   USymbol    *op_name,
                                   uint32_t    pc_off);

/* Comparison operator fallback for OP_EQ / OP_NEQ / OP_LT / OP_LE.
 * Unlike arith, uvalue_equal never errors; gate is lhs-is-object.
 * On OK: *out_bool is the coerced result of the overloaded "==" / "!=" slot.
 * On MISS: caller uses uvalue_equal / the original type error as before.
 * On THREW: *out_thrown holds the user exception thrown by the overload
 * body (there is no dst register for comparisons); caller re-deposits it.
 * out_thrown must be non-NULL; untouched unless THREW is returned. */
int vm_cmp_method_fallback(struct UVM *vm,
                           bool       *out_bool,
                           UValue     *out_thrown,
                           const UValue *lhs,
                           const UValue *rhs,
                           USymbol    *op_name,
                           uint32_t    pc_off);

#endif /* UVM_OP_OVERLOAD_H */
