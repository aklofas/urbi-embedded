# Closures and UFuncState

## Overview

urbi-embedded functions capture their enclosing lexical scope via
**upvalues** — a mechanism adapted from Lua 5.x. The emitter tracks
all live locals and upvalues in a `UFuncState` struct per function being
compiled. Nested functions resolve identifiers by walking the enclosing
`UFuncState` chain and installing upvalue references as needed.

---

## Example walkthrough

```text
function outer() {
  var x = 7;
  function inner() { x }
}
```

### Compile `outer`

1. `uemit_open_block` pushes a new scope on `outer`'s `UFuncState`.
2. `var x = 7` allocates a local: `outer.actvars[0] = {name="x", slot=0}`.
3. `function inner` triggers a nested `UFuncState` for `inner`.

### Compile `inner` — identifier resolution

4. Inside `inner`, the expression `x` triggers `resolve_identifier("x")`.
5. `inner`'s local table has no entry for `x`. The resolver calls
   `find_or_install_upvalue(inner_state, "x")`.
6. `find_or_install_upvalue` walks to the parent (`outer`'s `UFuncState`)
   and calls `local_lookup("x")` there — finds `outer.actvars[0]`.
7. It marks `outer.actvars[0].is_captured = true` and sets
   `outer`'s enclosing block's `has_captured = true`.
8. A new upvalue entry is appended: `inner.upvalues[0] = {name="x", idx=0,
   in_stack=true}`. (`in_stack=true` means the captured variable is still
   live on the enclosing function's register stack — not yet heapified.)
9. The emitter writes `OP_GETUPVAL R0, U0` into `inner`'s instruction
   stream, loading upvalue slot 0 into register 0.

### Block exit — heapification

```text
outer.actvars[0]  →  OP_CLOSE R0   (emitted on outer's block exit)
                                    │
                              open upvalue cell
                              is moved to heap:
                              inner.upvalues[0].in_stack = false
```

10. When `outer`'s block closes (`uemit_close_block`), the emitter sees
    `has_captured = true` and emits `OP_CLOSE R0`.
11. At runtime `OP_CLOSE` heapifies all open upvalue cells from `R0`
    upward: the value previously on the register stack is copied into a
    heap-allocated `UUpvalCell`, and all closures that reference the cell
    are updated to point at the heap copy. After `OP_CLOSE`, reading
    `inner.upvalues[0]` reads the heap cell, not the (now-gone) stack slot.

---

## Cascade — upvalue of upvalue

If `inner` itself contains a further-nested function `innermost` that
captures `x`, `find_or_install_upvalue` propagates the upvalue chain:

- `innermost` cannot find `x` locally.
- It calls `find_or_install_upvalue(inner_state, "x")`.
- `inner` already has `upvalues[0]` for `x` with `in_stack=true`
  (while `outer`'s block is still open) or `in_stack=false` (after
  `outer`'s block has closed and the cell is on the heap).
- A new entry is added: `innermost.upvalues[0] = {name="x", idx=0,
  in_stack=false}` (pointing at `inner`'s upvalue slot, not at
  `outer`'s register directly).
- `OP_GETUPVAL` in `innermost` still reads upvalue slot 0; the runtime
  follows the indirection chain transparently.

---

## Source references

| Component | Location |
|---|---|
| `UFuncState` struct definition | `src/emit/uemit.h` |
| `find_or_install_upvalue` | `src/emit/uemit_funcstate.c` |
| Block open / close | `uemit_open_block`, `uemit_close_block` in `src/emit/uemit_stmt.c` |
| Back-edge close (while loops) | `uemit_emit_loop_back_close` in `src/emit/uemit_funcstate.c` |
| `OP_CLOSE` runtime | `src/vm/uvm_closure.c` — closes all open upvalue cells ≥ R[A] |
| `OP_GETUPVAL` / `OP_SETUPVAL` | `src/vm/uvm_closure.c` — upvalue read / write dispatch |

See [opcodes.md](opcodes.md) for the full `OP_CLOSE` and `OP_GETUPVAL`
encoding.

---

## UClosure prototype-instance binding

Every `UClosure` carries a `proto_inst` pointer to the `UProtoInstance`
it runs inside — the per-VM realm-bound view of the closure's `UProto`.
The binding is end-to-end:

- **Top-level closures** route through
  `s->module_instance->proto_instances->entries[0]` at `urbi_run_chunk` /
  `urbi_vm_run` time.
- **Nested closures** inherit `cur_closure->proto_inst` from the
  enclosing call frame at `OP_CLOSURE` execution.

The `proto_inst` pointer is what bridges the call site's compiled-once
`UProto` (constants, instructions, IC name table) to the VM-and-realm
specific `UProtoInstance` (interned `USymbol*` IC table, per-realm-global
binding cache). Without it, two VMs running the same module would share
mutable IC state.

---

## Watcher closure ownership

Reactive watchers (`at`, `whenever`, `waituntil`, `every`) install
closures whose lifetime is decoupled from the installing call frame. The
`URBI_WATCHER_OWNS_*` flags on the watcher describe which of the
installed closures the watcher took ownership of — the cond closure, the
body closure, the onleave closure, or some subset — so that GC and
explicit teardown release the right set of references.

See [`reactive-runtime.md`](reactive-runtime.md) for the full watcher
lifecycle and the sync-execution sites that share
`urbi_run_closure_on_scratch`.
