#!/bin/sh
set -e
OUT=$(mktemp -d)
python3 tools/urbi-rosgen.py src/ros/msgs/manifest.json "$OUT/ros_msgs.gen.c" "$OUT/ros_msgs.gen.h"
grep -q "struct urbi_ros__geometry_msgs__Vector3" "$OUT/ros_msgs.gen.h" || { echo "FAIL: Vector3 struct"; exit 1; }
grep -q "urbi_ros__geometry_msgs__Vector3 linear;" "$OUT/ros_msgs.gen.h" || { echo "FAIL: Twist nests Vector3"; exit 1; }
grep -q "int32_t data;" "$OUT/ros_msgs.gen.h" || { echo "FAIL: Int32 field"; exit 1; }
echo "check-rosgen: struct emit PASS"
rm -rf "$OUT"
