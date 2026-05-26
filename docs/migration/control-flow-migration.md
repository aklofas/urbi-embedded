# Control-flow migration guide

Applies to: urbi-embedded v0.10.5+

## C-style `for` loop — deferred to v1.x

Legacy urbiscript accepted the three-part C-style form:

```urbi
// legacy — NOT supported
for (var i = 0; i < 10; i = i + 1) { ... }
```

The equivalent in urbi-embedded is a `while` loop:

```urbi
// urbi-embedded
var i = 0; while (i < 10) { ...; i = i + 1 }
```

## `loop` — deferred to v1.x

`loop` is infinite-loop sugar. Use `while (true)` instead:

```urbi
// legacy — NOT supported
loop { ... }

// urbi-embedded
while (true) { ... }
```

## `do (receiver) { ... }` — deferred to v1.x

The receiver-bound block form is uncommon in practice. Use explicit qualified
calls instead:

```urbi
// legacy — NOT supported
do (myObj) { slot1 = 1; slot2 = 2 }

// urbi-embedded
myObj.slot1 = 1; myObj.slot2 = 2
```

## Supported forms (v0.10.5+)

The following control-flow constructs are fully implemented:

- `for (var x : iter) { ... }` — for-each over any object with `.length()` and `.get(i)`
- `for (var x in iter) { ... }` — alias for the above (`in` and `:` are synonyms)
- `break` — exits the innermost enclosing loop or `switch`
- `continue` — skips the rest of the current loop iteration
- `switch (expr) { case v: { ... }; ... }` — equality-dispatch; no fall-through
- `while (cond) { ... }` — standard while loop
- `if / else`, `return`, `try / catch / finally`, `throw` — unchanged from legacy
