#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# tests/integration/ros/run-integration.sh — ros:jazzy container integration harness.
# Runs inside the derived urbi-ros-jazzy image; repo is copied in at /src.
set -euo pipefail

# setup.bash uses unbound variables internally; relax nounset around the source.
set +u
# shellcheck source=/dev/null
source /opt/ros/jazzy/setup.bash
set -u
export RMW_IMPLEMENTATION=rmw_fastrtps_cpp

cd /src

# Build the grounding spike using the validated include/link flags.
IFLAGS="-I/opt/ros/jazzy/include"
for d in /opt/ros/jazzy/include/*/; do
    IFLAGS="$IFLAGS -I$d"
done

LFLAGS="-L/opt/ros/jazzy/lib -Wl,-rpath,/opt/ros/jazzy/lib"
LFLAGS="$LFLAGS -lrcl -lrclc -lrcutils -lrmw -lrmw_implementation"
LFLAGS="$LFLAGS -lrosidl_runtime_c -lrosidl_typesupport_c"
# Per-message-package typesupport + generator libs (matches Makefile ROS2_MSG_PKGS).
for p in std_msgs geometry_msgs sensor_msgs builtin_interfaces example_interfaces; do
    LFLAGS="$LFLAGS -l${p}__rosidl_typesupport_c -l${p}__rosidl_generator_c"
done

# shellcheck disable=SC2086
gcc -std=c99 -Wall -Wextra -Wno-unused-result -o /tmp/spike_pubsub \
    tests/integration/ros/spike_pubsub.c \
    $IFLAGS $LFLAGS

output=$(/tmp/spike_pubsub)
echo "$output"
if ! echo "$output" | grep -q "PUBSUB got=42"; then
    echo "ros-integration: grounding gate FAIL"
    exit 1
fi
echo "ros-integration: grounding gate PASS"

# === [B2] liburbi with the rcl backend: ros.init brings up a real node ===
# The repo was copied in (may include stale host objects); rebuild from clean
# so the archive contains rcl-backend objects.
echo "=== [B2] rcl backend node init ==="
# TARGET=host-ros2 keeps the bake tool's build/host objects ros-free
# (design-risk v0.12.0-H); the rcl flags apply globally via URBI_ROS_BACKEND.
make -s clean
# Pre-generate the rcl-target marshaling (needs rosidl headers, container-only).
python3 tools/urbi-rosgen.py --target rcl src/ros/msgs/manifest.json \
    src/ros/generated/ros_msgs_rcl.gen.c src/ros/generated/ros_msgs_rcl.gen.h
make -s TARGET=host-ros2 URBI_ENABLE_ROS2=1 URBI_ROS_BACKEND=rcl \
    CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g" core
LIBA=build/host-ros2/liburbi.a
# shellcheck disable=SC2086
gcc -std=c99 -Wall -Wextra -Wno-unused-result -Iinclude -Isrc \
    -o /tmp/driver_init tests/integration/ros/driver_init.c \
    "$LIBA" $IFLAGS $LFLAGS -lm
b2out=$(/tmp/driver_init)
echo "$b2out"
if ! echo "$b2out" | grep -q "ROSINIT ok"; then
    echo "ros-integration: B2 rcl node init FAIL"
    exit 1
fi
echo "ros-integration: B2 rcl node init PASS"

# === [B3] rosidl-targeting codegen round-trip (marshal_rcl / unmarshal_rcl) ===
echo "=== [B3] rcl marshal round-trip ==="
# liburbi.a (built above) already contains the rcl-target generated marshaling.
# The driver includes gated headers (ulist_build.h, ros_msgs_rcl.gen.h) so it
# needs the same URBI_ENABLE_ROS2 + URBI_ROS_BACKEND_RCL defines as the library.
# shellcheck disable=SC2086
gcc -std=c99 -Wall -Wextra -Wno-unused-result \
    -DURBI_ENABLE_ROS2=1 -DURBI_ROS_BACKEND_RCL=1 -Iinclude -Isrc \
    -o /tmp/driver_marshal tests/integration/ros/driver_marshal.c \
    "$LIBA" $IFLAGS $LFLAGS -lm
b3out=$(/tmp/driver_marshal)
echo "$b3out"
if ! echo "$b3out" | grep -q "RCLMARSHAL ok"; then
    echo "ros-integration: B3 rcl marshal FAIL"
    exit 1
fi
echo "ros-integration: B3 rcl marshal PASS"

# === [B4+B5] rcl publisher + subscriber loopback through live DDS ===
echo "=== [B4+B5] rcl pub/sub loopback ==="
# shellcheck disable=SC2086
gcc -std=c99 -Wall -Wextra -Wno-unused-result \
    -DURBI_ENABLE_ROS2=1 -DURBI_ROS_BACKEND_RCL=1 -Iinclude -Isrc \
    -o /tmp/driver_pubsub tests/integration/ros/driver_pubsub.c \
    "$LIBA" $IFLAGS $LFLAGS -lm
b45out=$(/tmp/driver_pubsub)
echo "$b45out"
if ! echo "$b45out" | grep -q "PUBSUB42 ok"; then
    echo "ros-integration: B4+B5 pub/sub FAIL"
    exit 1
fi
echo "ros-integration: B4+B5 pub/sub PASS"
