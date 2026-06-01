# Facet↔ROS2 binding

The facet↔ROS2 binding layer connects the Robotics facet library
(`URBI_ENABLE_UROBOTICS`) to the ROS2 bridge (`URBI_ENABLE_ROS2`), letting
a single urbiscript line wire a facet slot to a ROS2 topic — both for input
(sensor data arriving from ROS2 updates a facet slot) and output (a facet
slot value is published as a ROS2 message on each cycle or on demand).

Both gates must be enabled. The binding surface is pure urbiscript; no C code
is required.

It is **EXPERIMENTAL** — the API may change before v1.0.

## Configuration

| Flag | Default | Effect |
| --- | --- | --- |
| `URBI_ENABLE_ROS2` | undefined (off) | ROS2 bridge; see `docs/embedded/ros-bridge.md`. |
| `URBI_ENABLE_UROBOTICS` | undefined (off) | Robotics facet overlay; see `docs/embedded/urobotics.md`. |

Both flags must be set together. Build with the combined preset:

```sh
make test-ros-urobotics       # build ros-urobotics preset + unit tests + chk fixture
make TARGET=host-ros-urobotics URBI_ENABLE_ROS2=1 URBI_ENABLE_UROBOTICS=1 urbi-bin
```

Do **not** mix the `ros-urobotics` objects into either the bare `host` or the
single-flag `host-ros2` / `host-urobotics` targets. Each preset writes to its
own `build/` subdirectory to prevent stale-object collisions.

## Binding API

### `Robotics.bindInput(facet, slot, topic, msgType, field)`

Subscribes to `topic` and writes the value of `field` from each incoming
message of type `msgType` into `facet.<slot>`. Returns the underlying
subscription `Event`; you can attach additional `at` watchers to the same
event if needed.

```text
var insub = Robotics.bindInput(sensor, "distance", "/range",
                               "sensor_msgs/Range", "range");
```

### `Robotics.bindOutput(facet, slot, topic, msgType, field, rate)`

Creates a publisher on `topic` and arranges for `facet.<slot>` to be
published as the `field` of a `msgType` message.

- If `rate` is a positive duration (e.g. `100ms`), an `every(rate)` periodic
  publisher is installed automatically. The handle returned exposes
  `.publishNow()` for additional on-demand publishes.
- If `rate` is `0ms`, no periodic publisher is installed. Call
  `.publishNow()` explicitly to trigger a publish.

```text
// Periodic output: /cmd published every 100 ms.
var out = Robotics.bindOutput(motor, "val", "/cmd",
                              "std_msgs/Float64", "data", 100ms);

// On-demand only.
var out2 = Robotics.bindOutput(motor, "val", "/cmd",
                               "std_msgs/Float64", "data", 0ms);
out2.publishNow();
```

### `handle.publishNow()`

Reads `facet.<slot>` immediately and publishes one message. Available on any
handle returned by `bindOutput`, regardless of whether a periodic rate was
set.

## Slot↔message-field mapping

The table below lists the standard mapping between facet slots and ROS2
message fields. Any `msgType` supported by the ROS2 bridge can be used — the
table reflects the recommended pairings for each facet.

| Facet slot | ROS2 type | Field | Notes |
| --- | --- | --- | --- |
| `DistanceSensor.distance` | `sensor_msgs/Range` | `range` | float32; see note on float literals below |
| `Motor.val` | `std_msgs/Float64` | `data` | float64 |
| `Led.val` | `std_msgs/Float64` | `data` | float64 |
| `TouchSensor.pressure` | `std_msgs/Float64` | `data` | float64 |
| `Mobile` | `geometry_msgs/Twist` | — | Twist has nested `linear`/`angular` vectors; field-path mapping deferred |

### Float64-backed slots: use float literals

Slots bound to `std_msgs/Float64.data` must hold float values. Assigning an
integer literal to a Float64-backed slot will produce an incorrect wire value.
Always assign float literals for these slots:

```text
motor.val = 1.0;   // correct — marshals as Float64
motor.val = 0.0;   // correct
motor.val = 1;     // incorrect — marshals as integer, produces garbage on the wire
```

This applies to `Motor.val`, `Led.val`, and `TouchSensor.pressure`.
`DistanceSensor.distance` is backed by `sensor_msgs/Range.range` (float32),
which has the same requirement.

## Reactive safety behavior

The facet binding works naturally with urbiscript reactive watchers. A bound
slot is an ordinary urbiscript slot; `at` and `whenever` watchers fire on
slot-change events just as they do for any other slot:

```text
// Stop the motor when the sensor reads closer than 0.5 m.
at (sensor.distance < 0.5) motor.val = 0.0;
```

The watcher body runs on the VM strand. The updated `motor.val` is picked up
by the next `publishNow()` call (either periodic or explicit).

## Full assembled-robot example

`examples/urobotics/ros_robot_demo.u` assembles a minimal robot with one
distance sensor and one motor, wires both to ROS2 topics, and installs a
reactive safety stop:

```text
ros.init("demo_bot");

var sensor = Robotics.DistanceSensor.clone();
sensor.distance = 1.0;
var motor = Robotics.Motor.clone();
motor.val = 1.0;

var insub = Robotics.bindInput(sensor, "distance", "/range",
                               "sensor_msgs/Range", "range");
var out   = Robotics.bindOutput(motor, "val", "/cmd",
                                "std_msgs/Float64", "data", 100ms);
at (sensor.distance < 0.5) motor.val = 0.0;
```

Run against the mock transport (no ROS installation required):

```sh
URBI_BUILD_PRESET=ros-urobotics \
    build/host-ros-urobotics/urbi -i < examples/urobotics/ros_robot_demo.u
```

Run the full test suite (mock-backed, host-only):

```sh
make test-ros-urobotics
```

The one chk fixture (`tests/chk/ros-urobotics/binding_loopback.chk`) exercises
the complete round-trip: `bindInput` drives the sensor slot from an injected
message, the `at`-watcher fires, and `publishNow()` verifies the updated motor
value on the wire.

## Running against real DDS

```sh
make ros-integration    # links against rclc + Fast-DDS; requires a ROS2 Jazzy install
```

This replaces the mock transport with a real rclc/rmw backend. The urbiscript
surface is identical to the mock-backed case.

## Deferred items

The following are **not** part of this release:

- **On-hardware embedded demo**: an ESP32 ROS2 demo combining this binding
  with a real sensor is a future step.
- **Nested-field input binding**: `geometry_msgs/Twist` with its nested
  `linear`/`angular` vector fields requires a field-path mapping API not yet
  present. The `Mobile` facet is not bindable in this release.
- **Reactive slot-change output**: publishing on every slot-change event
  (rather than on a fixed rate or on demand) is not supported in this release.
- **Trajectory generators and blend modes**: sinusoidal value drivers and
  multi-source effector blending are not part of this overlay.
- **`UOwned` slot semantics**: ownership-tracked binding for multi-source
  effector arbitration is deferred.
