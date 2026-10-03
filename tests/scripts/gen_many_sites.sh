#!/bin/sh
# gen_many_sites.sh N — print urbiscript that touches N distinct slots of
# one object inside one function, then sums two of them.
n=${1:-300}
printf 'var f = function() { var o = Object.new(); '
i=0; while [ "$i" -lt "$n" ]; do printf 'var o.s%d = %d; ' "$i" "$i"; i=$((i + 1)); done
printf 'o.s0 + o.s%d }; f()\n' $((n - 1))
