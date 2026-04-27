/* SPDX-License-Identifier: BSD-3-Clause */
/* UStrand state byte encoding + lifecycle declarations.
   Full lifecycle operations land across T20 (create/start/spawn) and T29 (tag fields). */

#ifndef USTRAND_H
#define USTRAND_H

#include <stdint.h>
#include "uvalue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* === State byte encoding (row 9 §3.1) ===
   Upper nibble: logical state class.
   Lower nibble: reason sub-code (meaningful only in WAITING). */

#define USTRAND_STATE_MASK     0xF0u
#define USTRAND_REASON_MASK    0x0Fu

/* Logical state classes (pre-shifted into upper nibble). */
#define USTRAND_DORMANT        0x00u  /* allocated, not yet enqueued */
#define USTRAND_READY          0x10u  /* on run-queue */
#define USTRAND_RUNNING        0x20u  /* currently dispatching */
#define USTRAND_WAITING        0x30u  /* blocked, see reason sub-code */
#define USTRAND_DEAD           0x40u  /* terminated, awaiting GC */
#define USTRAND_SUSPENDED      0x50u  /* RESERVED — Tag.freeze (M5/M6) */

/* WAITING reason sub-codes (lower nibble). */
#define USTRAND_REASON_NONE    0x00u
#define USTRAND_REASON_SLEEP   0x01u
#define USTRAND_REASON_EVENT   0x02u
#define USTRAND_REASON_JOIN    0x03u
#define USTRAND_REASON_HOST    0x04u  /* RESERVED v1.x/v2 */

/* Composite values stored in strand->state. */
#define USTRAND_STATE_DORMANT         (USTRAND_DORMANT)
#define USTRAND_STATE_READY           (USTRAND_READY)
#define USTRAND_STATE_RUNNING         (USTRAND_RUNNING)
#define USTRAND_STATE_DEAD            (USTRAND_DEAD)
#define USTRAND_STATE_SUSPENDED       (USTRAND_SUSPENDED)
#define USTRAND_STATE_WAITING_SLEEP   (USTRAND_WAITING | USTRAND_REASON_SLEEP)
#define USTRAND_STATE_WAITING_EVENT   (USTRAND_WAITING | USTRAND_REASON_EVENT)
#define USTRAND_STATE_WAITING_JOIN    (USTRAND_WAITING | USTRAND_REASON_JOIN)
#define USTRAND_STATE_WAITING_HOST    (USTRAND_WAITING | USTRAND_REASON_HOST)

/* Helper macros — take a pointer to UStrand. */
#define USTRAND_IS_WAITING(s)  (((s)->state & USTRAND_STATE_MASK) == USTRAND_WAITING)
#define USTRAND_GET_STATE(s)   ((s)->state & USTRAND_STATE_MASK)
#define USTRAND_GET_REASON(s)  ((s)->state & USTRAND_REASON_MASK)

/* === UExecStatus (row 7 §2; full T8 lands the dispatch transition) === */

typedef enum {
    UEXEC_OK = 0,
    UEXEC_RETURN,
    UEXEC_THROW,
    UEXEC_TAG_STOP,
    UEXEC_CANCEL
} UExecStatus;

/* === Forward declarations for types that land in later tasks. ===
   T3 creates ucleanup.h — include it then; use opaque pointer for now.
   T29 creates utag.h.  Reactive types land later. */

struct UCleanupEntry; /* T3 */
struct UTag;          /* T29 */
struct UEvent;        /* reactive runtime */
struct UVM;           /* uvm.h — forward-decl to avoid circular include */

/* === UStrand struct (M3 baseline) ===
   T20 and T29 add lifecycle operations; T9 wires the unwind walker;
   T3 initialises the cleanup-stack array. */

typedef struct UStrand UStrand;
struct UStrand {
    /* M2 fields for frame stack, registers, lex env, etc. are added by T20
       when the strand becomes a full execution context. */

    /* --- Row 7 unwind/cleanup fields (T9 wires walker; T3 lands cleanup-stack) --- */
    UExecStatus             pending_unwind;
    uint8_t                 unwind_pad[3];
    UValue                  unwind_value;
    struct UTag            *unwind_target;
    void                   *suppressed_head;   /* RESERVED v1.x */
    struct UCleanupEntry   *cleanup_top;
    uint16_t                cleanup_depth;
    uint16_t                cleanup_cap;
    struct UCleanupEntry   *cleanup_base;
    UExecStatus             fatal_status;
    uint8_t                 fatal_pad[3];
    UValue                  fatal_value;

    /* --- Row 9 state machine + budget --- */
    uint8_t                 state;
    uint8_t                 state_pad[3];
    uint16_t                instruction_budget_remaining;
    uint16_t                budget_pad;

    /* --- Cooperative scheduler intrusive list (T5 wires) ---
       Included unconditionally at T2; T5 will revisit whether to gate
       these fields behind URBI_SCHED == URBI_SCHED_COOPERATIVE if the
       scheduler dispatch becomes plural or multi-strategy.
       TODO(T5): conditionally compile under URBI_SCHED == URBI_SCHED_COOPERATIVE
       if scheduler dispatch becomes plural. */
    UStrand                *ready_next;
    UStrand                *ready_prev;

    /* --- WAITING-related queue fields --- */
    UStrand                *wait_next;
    union {
        uint64_t            wake_us;
        struct UEvent      *event;
        UStrand            *join_parent;
    } wait_payload;
};

/* === Lifecycle functions (stubs; full impl across T20 + T29) === */

void ustrand_init(UStrand *s, struct UVM *vm);
void ustrand_destroy(UStrand *s);

#ifdef __cplusplus
}
#endif

#endif /* USTRAND_H */
