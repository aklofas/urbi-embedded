#!/bin/sh
# check-urobotics-determinism — the baked Robotics overlay blob must be
# byte-reproducible, and the tracked src/urobotics/urobotics_bytecode.gen.c
# must match a fresh bake (no stale checkin).  Builds only the flag-free bake
# tool (no URBI_ENABLE_UROBOTICS — the tool just compiles .u -> bytecode; the
# gate is irrelevant to baking and would trip the v0.12.0-H link trap).
set -e
# refactor-3 BLD-02b: no nested make — the bake tool is a Makefile
# prerequisite of the check-urobotics-determinism target.  A nested make
# raced the top-level graph's own bake-tool builds under releasetest -j.
[ -x ./tools/urbi-compile-stdlib ] || {
    echo "check-urobotics-determinism: tools/urbi-compile-stdlib missing — invoke via 'make check-urobotics-determinism'"
    exit 1
}
A=$(mktemp); B=$(mktemp)
./tools/urbi-compile-stdlib src/urobotics/UROBOTICS_ORDER.txt src/urobotics "$A" urbi_urobotics_bytecode >/dev/null
./tools/urbi-compile-stdlib src/urobotics/UROBOTICS_ORDER.txt src/urobotics "$B" urbi_urobotics_bytecode >/dev/null
if ! diff "$A" "$B" >/dev/null; then
    echo "check-urobotics-determinism: FAIL (overlay blob not reproducible)"; rm -f "$A" "$B"; exit 1
fi
if ! diff "$A" src/urobotics/urobotics_bytecode.gen.c >/dev/null; then
    echo "check-urobotics-determinism: FAIL (tracked blob is stale — rebake src/urobotics/urobotics_bytecode.gen.c)"; rm -f "$A" "$B"; exit 1
fi
rm -f "$A" "$B"
echo "check-urobotics-determinism: PASS"
