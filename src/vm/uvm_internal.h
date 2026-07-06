/* SPDX-License-Identifier: BSD-3-Clause */
/* uvm_internal.h — private inter-TU API for the vm subsystem.
 * Consumed only by src/vm/ translation units. */

#ifndef UVM_INTERNAL_H
#define UVM_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vm/uvm.h"
#include "chunk/uchunk.h"   /* UProto */
#include "runtime/uclosure.h" /* UClosure, UUpvalCell */
#include "sched/ustrand.h"    /* UStrand */

/* --- UDiagWriter (defined here; uvm.c dispatch loop uses it directly) --- */

/* Fixed-buffer diagnostic writer.  Truncates with "..." when the buffer
 * fills.  Freestanding: no snprintf, no <stdio.h>. */
typedef struct UDiagWriter {
    char   *buf;
    size_t  cap;
    size_t  used;
    bool    truncated;
} UDiagWriter;

/* --- From uvm_diag.c --- */

void urbi_vm_diag_init(UDiagWriter *w, char *buf, size_t cap);
void urbi_vm_diag_write_cstr(UDiagWriter *w, const char *s);
void urbi_vm_diag_write_u32(UDiagWriter *w, uint32_t n);
void urbi_vm_diag_write_prefix(UDiagWriter *w, const UProto *module, size_t pc);

const char *kind_name(uint8_t kind);
const char *urbi_vm_op_name(uint8_t op);
uint32_t    vm_line_for_pc(const UProto *module, size_t pc);

void urbi_vm_format_type_error_binary(UVM *vm, const UProto *module, size_t pc,
                                 uint8_t op, uint8_t b_kind, uint8_t c_kind);
void urbi_vm_format_type_error_unary(UVM *vm, const UProto *module, size_t pc,
                                uint8_t op, uint8_t b_kind);
void urbi_vm_format_type_error_msg(UVM *vm, const char *msg);
void urbi_vm_format_oom(UVM *vm, size_t nbytes);

/* completeness check: returns 1 if urbi_vm_op_name() and op_user_name() return
 * non-fallback strings for every valid opcode. */
int urbi_vm_diag_opnames_complete(void);

/* --- From uvm_closure.c --- */

UClosure   *urbi_vm_alloc_closure(UVM *vm, UProto *proto);
UUpvalCell *urbi_vm_open_upvalue(UVM *vm, UStrand *s, UValue *slot);

#endif /* UVM_INTERNAL_H */
