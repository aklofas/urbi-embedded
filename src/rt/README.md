# src/rt/ — the new runtime core

Headers here `#include "rt/u*.h"` each other in one direction only, in this
order (lowest first; a header may only include earlier ones):

    uvalue -> ugc -> ustr -> uobj -> ulist -> ustrand -> usched -> uexec -> uwatch -> urealm -> uboot

`rt/ustdlib_glue.h` sits above `uboot` and is not in that chain: nothing
in `src/rt/` may include it.

`src/rt/*.c` and `src/rt/*.h` may not include any old-runtime header
(`vm/`, `sched/`, `object/`, `gc/`, `runtime/`, `watcher/`, `event/`, `tag/`,
`realm/`, `changed/`, `value/`). A `src/stdlib/*.c` file reaches the
runtime through exactly one header, `rt/ustdlib_glue.h`, which re-exports
what a native method needs; it may include its own `stdlib/<file>.h`
besides. `tests/scripts/check_rt_layering.sh` enforces both rules.
