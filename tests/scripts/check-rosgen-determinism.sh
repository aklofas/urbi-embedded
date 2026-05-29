#!/bin/sh
set -e
A=$(mktemp -d); B=$(mktemp -d)
python3 tools/urbi-rosgen.py src/ros/msgs/manifest.json "$A/c" "$A/h"
python3 tools/urbi-rosgen.py src/ros/msgs/manifest.json "$B/c" "$B/h"
diff "$A/c" "$B/c" >/dev/null && diff "$A/h" "$B/h" >/dev/null && echo "check-rosgen-determinism: PASS" || { echo "FAIL: non-deterministic"; exit 1; }
rm -rf "$A" "$B"
