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
LFLAGS="$LFLAGS -lstd_msgs__rosidl_typesupport_c -lstd_msgs__rosidl_generator_c"

# shellcheck disable=SC2086
gcc -std=c99 -Wall -Wextra -Wno-unused-result -o /tmp/spike_pubsub \
    tests/integration/ros/spike_pubsub.c \
    $IFLAGS $LFLAGS

output=$(/tmp/spike_pubsub)
echo "$output"
if echo "$output" | grep -q "PUBSUB got=42"; then
    echo "ros-integration: grounding gate PASS"
    exit 0
else
    echo "ros-integration: grounding gate FAIL"
    exit 1
fi
