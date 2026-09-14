# src/rt/ — the new runtime core

Headers here `#include "rt/u*.h"` each other in one direction only, in this
order (lowest first; a header may only include earlier ones):

    uvalue -> ugc -> ustr -> uobj -> ulist -> ustrand -> usched -> uexec -> uwatch -> urealm -> uboot

`src/rt/*.c` and `src/rt/*.h` may not include any old-runtime header
(`vm/`, `sched/`, `object/`, `gc/`, `runtime/`, `watcher/`, `event/`, `tag/`,
`realm/`, `changed/`, `value/`). `src/stdlib/*.c` may include only
`rt/uvalue.h`, `rt/ugc.h`, `rt/ustr.h`, `rt/uobj.h`, `rt/ulist.h`, and
`rt/uexec.h`. `tests/scripts/check_rt_layering.sh` enforces both rules.
