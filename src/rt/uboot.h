/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/uboot.h — the boot table: one static description of every
 * built-in, and the one function that turns it into a live VM.
 *
 * The old core booted through eighteen hand-ordered `*_register`
 * functions, each allocating its own proto, interning its own names and
 * installing its own methods, with the ordering constraints between them
 * living only in stdlib_boot.c's comments.  Here there is one table: a
 * row per built-in naming its global, its slot in vm->protos[], its
 * parent, and its method list.  uboot_init walks it three times —
 * allocate every proto, link the parents, install the methods — so no
 * row can depend on another row's position, and a new built-in is one
 * more row rather than one more registration function.
 *
 * Arity lives in the table too.  `min_args`/`max_args` are checked by
 * uexec's native call path before the body runs, which is why the bodies
 * in src/stdlib/ have no arity prologues. */

#ifndef URT_BOOT_H
#define URT_BOOT_H
#include "rt/urealm.h"

/* One native method.  The signature is the one every src/stdlib/ body
 * already has: `self` is the receiver as the caller passed it (an atom
 * stays an atom — no boxing), `args`/`nargs` is the argument window, and
 * the result goes through `out`.  Returns UEXEC_OK, or UEXEC_THROW with
 * the exception already deposited on the current strand. */
typedef struct UMethodDef {
    const char *name;
    int (*fn)(UVM *, UValue, UValue *, uint8_t, UValue *);
    uint8_t min_args, max_args;
} UMethodDef;

/* max_args sentinel: any argument count at or above min_args.  The same
 * value the closure layer calls UCLOSURE_ANY_ARGS. */
#define UMETHOD_VARARGS UCLOSURE_ANY_ARGS

/* Flags column. */
#define UBOOT_F_READONLY  0x0001   /* script-side slot writes throw TypeError */

/* One built-in.
 *   global       — the name it binds in root_globals, or NULL for none.
 *   proto_index  — its slot in vm->protos[]; one row per slot, no repeats.
 *   parent_index — the UP_* it inherits from, or -1 for the root itself.
 *   methods/n    — its native methods.
 *   flags        — UBOOT_F_*. */
typedef struct UBuiltinDef {
    const char        *global;
    int                proto_index;
    int                parent_index;
    const UMethodDef  *methods;
    uint16_t           nmethods;
    uint16_t           flags;
} UBuiltinDef;

extern const UBuiltinDef uboot_table[];
extern const uint16_t    uboot_table_len;

/* Builds vm->protos[] and vm->root_globals from the table, installs every
 * method, then loads the single baked stdlib.u blob and runs its root
 * with recv = root_globals, so the overlay's top-level `var`s land as
 * slots on the shared root object.  Idempotent: a second call returns
 * URBI_OK without doing anything.  Sets vm->stdlib_booted on success. */
int uboot_init(UVM *vm);

/* Installs the scheduler's script surface: the Tag, Event and Job
 * method tables, the `sleep` / `every` / `scopeTag` / detach globals, and
 * the Lobby's per-realm connectionTag getter.  Defined in
 * rt/usched_natives.c, beside the scheduler state they all touch. */
int usched_natives_init(UVM *vm);

/* Installs `n` methods on `proto` as native closures.  Exposed because
 * the stdlib overlay and the ROS component install method tables of their
 * own; `proto` must already be reachable (the installer allocates). */
int uboot_install_methods(UVM *vm, UObject *proto, const UMethodDef *m, uint16_t n);

/* The baked stdlib.u blob (src/stdlib/urbi_stdlib_bytecode.gen.c).  A
 * build with no stdlib links the stub in tools/stub_stdlib_bytecode.c,
 * which declares the same two symbols with a zero length. */
extern const unsigned char urbi_stdlib_bytecode[];
extern const size_t        urbi_stdlib_bytecode_len;

#endif
