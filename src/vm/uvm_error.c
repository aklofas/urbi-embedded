/* SPDX-License-Identifier: BSD-3-Clause */
/* uvm_error.c — Per-VM error ring buffer: urbi_last_error, urbi_clear_error,
 * urbi_set_error_internal (Gap P, v0.7.1).
 *
 * Design:
 *   The error ring holds up to URBI_ERROR_RING_DEPTH entries.  Each entry
 *   owns three fixed-size string buffers (message, source_name, context).
 *   The urbi_error_info_t struct returned by urbi_last_error has const char*
 *   fields pointing into those UVM-owned buffers — valid until the next
 *   call that mutates the error ring.
 *
 *   Size note: UErrorRing is ~4 KB inline inside UVM.  Tests that put
 *   `UVM vm;` on the stack add that to the stack frame; acceptable on
 *   hosted (8 MB default stack) but embedders running on FreeRTOS with
 *   tight task stacks should allocate UVM on the heap or a static region.
 *
 * Freestanding discipline: no <string.h>.  String copies use
 * urbi_strncpy_truncating from runtime/umacros.h. */

#include "vm/uvm.h"
#include "runtime/umacros.h"  /* urbi_strncpy_truncating, urbi_zero */
#include "urbi/urbi.h"        /* urbi_last_error, urbi_clear_error */
#include "urbi/types.h"       /* UErrCode, URBI_OK */

#include <stddef.h>
#include <stdint.h>

/* urbi_set_error_internal — internal (not declared in public headers).
 * Declared in vm/uvm_error.h which is included by subsystems that need it.
 * See include/vm/uvm_error.h for the declaration. */

/* Helper: copy src into dst[0..URBI_ERROR_STRING_BUF) with NUL termination.
 * If src == NULL, writes the empty string. */
static void
copy_error_str(char *dst, const char *src)
{
    if (src == NULL) {
        dst[0] = '\0';
    } else {
        urbi_strncpy_truncating(dst, (size_t)URBI_ERROR_STRING_BUF, src);
    }
}

/* urbi_set_error_internal: push an error entry onto the ring.
 *
 * If the ring is full (count == URBI_ERROR_RING_DEPTH), the oldest entry
 * (at index `head - count` mod depth) is overwritten (FIFO discard).
 *
 * NULL string args → empty string in the entry buf. */
void
urbi_set_error_internal(struct UVM *vm,
                         int         code,
                         const char *message,
                         const char *source_name,
                         int         line,
                         const char *context)
{
    UErrorRingEntry *e;
    size_t slot;

    if (vm == NULL) return;

    /* Determine which slot to write.  `head` is the next write position
     * (mod URBI_ERROR_RING_DEPTH).  If the ring is already full, advancing
     * head also discards the oldest entry (count stays at max). */
    slot = vm->error_ring.head % (size_t)URBI_ERROR_RING_DEPTH;
    e    = &vm->error_ring.entries[slot];

    /* Copy strings into the entry's owned buffers. */
    copy_error_str(e->message_buf,     message);
    copy_error_str(e->source_name_buf, source_name);
    copy_error_str(e->context_buf,     context);

    /* Wire the info struct's const char* pointers to the entry buffers. */
    e->info.code        = code;
    e->info.message     = e->message_buf;
    e->info.source_name = e->source_name_buf;
    e->info.source_line = line;
    e->info.context     = e->context_buf;

    /* Advance ring state. */
    vm->error_ring.head = (vm->error_ring.head + 1U) % (size_t)URBI_ERROR_RING_DEPTH;
    if (vm->error_ring.count < (size_t)URBI_ERROR_RING_DEPTH) {
        vm->error_ring.count++;
    }
}

/* urbi_last_error: read the most-recent error entry.
 *
 * If the ring is empty (count == 0), zeroes *out_info and returns URBI_OK.
 * Otherwise copies the most-recent entry's info into *out_info (const char*
 * fields point into UVM-owned buffers) and returns entry.code.
 *
 * out_info == NULL is a no-op (returns URBI_OK or the code, skips copy). */
int
urbi_last_error(struct UVM *vm, urbi_error_info_t *out_info)
{
    size_t last_slot;
    const UErrorRingEntry *e;

    if (vm == NULL) {
        if (out_info != NULL) {
            urbi_zero(out_info, sizeof(*out_info));
        }
        return URBI_OK;
    }

    if (vm->error_ring.count == 0U) {
        if (out_info != NULL) {
            urbi_zero(out_info, sizeof(*out_info));
        }
        return URBI_OK;
    }

    /* Most-recent slot: head was advanced AFTER the write, so the last
     * write landed at (head - 1 + DEPTH) % DEPTH. */
    last_slot = (vm->error_ring.head + (size_t)URBI_ERROR_RING_DEPTH - 1U)
                % (size_t)URBI_ERROR_RING_DEPTH;
    e = &vm->error_ring.entries[last_slot];

    if (out_info != NULL) {
        *out_info = e->info;
    }
    return e->info.code;
}

/* urbi_clear_error: empty the error ring.
 * Does not zero the string buffers — just marks the ring empty. */
void
urbi_clear_error(struct UVM *vm)
{
    if (vm == NULL) return;
    vm->error_ring.count = 0U;
    vm->error_ring.head  = 0U;
}
