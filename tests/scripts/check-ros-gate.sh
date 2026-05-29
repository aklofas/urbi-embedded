#!/bin/sh
# check-ros-gate.sh — URBI_ENABLE_ROS2 builds clean; OFF build is byte-identical.
set -e

echo "check-ros-gate: building base (gate OFF) ..."
make -s TARGET=host-rosgate-off CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g" core >/dev/null

echo "check-ros-gate: building with URBI_ENABLE_ROS2=1 ..."
make -s TARGET=host-rosgate-on URBI_ENABLE_ROS2=1 CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g" core >/dev/null

if ! ar t build/host-rosgate-on/liburbi.a | grep -q '^uros\.o$'; then
    echo "check-ros-gate: FAIL — uros.o missing from URBI_ENABLE_ROS2=1 archive"; exit 1
fi
if ar t build/host-rosgate-off/liburbi.a | grep -q '^uros\.o$'; then
    echo "check-ros-gate: FAIL — uros.o leaked into the OFF archive"; exit 1
fi
echo "check-ros-gate: PASS"
