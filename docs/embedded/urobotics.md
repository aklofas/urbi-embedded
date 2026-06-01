# Robotics facet library

The Robotics facet library (`URBI_ENABLE_UROBOTICS`, v0.12.2) is a pure-urbiscript
overlay that defines a `Robotics` namespace with standard hardware-abstraction
facets: motors, sensors, LEDs, a mobile base, and a tracker. It is **off by
default**: with the gate off, no `Robotics` symbols exist and the core build
is byte-identical to a standard build. Enabling it bakes an additional bytecode
blob into the library with no changes to the core VM.

## Configuration

| Flag | Default | Effect |
| --- | --- | --- |
| `URBI_ENABLE_UROBOTICS` | undefined (off) | Master gate. Off: no `Robotics` symbols, no `urobotics.c` linked, no overhead in the core VM. On: compiles `src/urobotics/` and bakes the facet overlay into the library. |

Build with the urobotics preset to get a consistent define set:

```sh
make test-urobotics                     # build + unit tests + chk fixtures
make TARGET=host-urobotics              # build the urobotics-capable library + binaries
```

Do **not** add `-DURBI_ENABLE_UROBOTICS=1` to the default `TARGET=host` build
path: the default and urobotics targets share object directories, and mixing
objects from different flag sets causes stale-object collisions.

## The `Robotics` namespace

All facets live on the `Robotics` object. Each facet is a plain urbiscript
prototype with standard slots; cloning a facet gives an independent instance
whose slots can be read and written freely.

```text
Robotics.RotationalMotor.clone().angle    // -> 0 (default)
```

The facet library is available under `URBI_BUILD_PRESET=urobotics`:

```text
Robotics.Identity.isA(Object)             // -> true
Robotics.RGBLed.isA(Robotics.Led)         // -> true
```

## Facet table

| Facet | Parent | Standard slots |
| --- | --- | --- |
| `Robotics.Identity` | `Object` | `type`, `name`, `model`, `serial` |
| `Robotics.Network` | `Object` | `type`, `IP` |
| `Robotics.Motor` | `Object` | `type`, `val`, `PGain`, `IGain`, `DGain` |
| `Robotics.LinearMotor` | `Robotics.Motor` | `position`, `force` |
| `Robotics.RotationalMotor` | `Robotics.Motor` | `angle`, `turn`, `torque` |
| `Robotics.Sensor` | `Object` | `type`, `val` |
| `Robotics.DistanceSensor` | `Robotics.Sensor` | `distance` |
| `Robotics.TouchSensor` | `Robotics.Sensor` | `pressure` |
| `Robotics.AccelerationSensor` | `Robotics.Sensor` | `acceleration` |
| `Robotics.GyroSensor` | `Robotics.Sensor` | `speed` |
| `Robotics.TemperatureSensor` | `Robotics.Sensor` | `temperature` |
| `Robotics.Mobile` | `Object` | `type`, `go(meters)`, `turn(radians)` |
| `Robotics.Tracker` | `Object` | `type`, `yaw`, `pitch` |
| `Robotics.Led` | `Object` | `type`, `val` |
| `Robotics.RGBLed` | `Robotics.Led` | `r`, `g`, `b` |

All slots default to `0` (or `""` for string fields such as `name` and `IP`).
`Mobile.go` and `Mobile.turn` default to identity functions that return their
argument; override them on a clone to wire real movement.

## Structure-tree assembly

Build a robot description as a plain object tree, adding facet clones where
needed:

```text
var robot = Object.clone();
robot.body = Object.clone();
robot.body.head = Object.clone();

robot.body.head.pan = Robotics.RotationalMotor.clone();
robot.body.head.pan.angle = 0.5;

robot.identity = Robotics.Identity.clone();
robot.identity.name  = "my_bot";
robot.identity.model = "urbi-demo-v1";
```

The tree is just a nested object graph — any urbiscript slot manipulation
(`isA`, `clone`, property inspection) works on the result.

## Dict-backed localization

When a body part groups several symmetrically placed components (eyes, wheels,
arm joints), address them by qualifier string using a `Dict`:

```text
robot.body.head.eyes = Robotics.group(["left" => Robotics.RGBLed.clone(), "right" => Robotics.RGBLed.clone()]);
robot.body.head.eyes["left"].r = 1;    // light left eye red
robot.body.head.eyes["right"].r = 0;
```

`Robotics.group(d)` is an identity helper that returns its argument unchanged;
it is a documentation convention — callers can equally store the dict directly.

### Standard qualifiers and axes

Two constant lists document the conventional string keys:

```text
Robotics.qualifiers    // ["left", "right", "front", "back", "up", "down",
                       //  "frontleft", "frontright", "backleft", "backright"]
Robotics.axes          // ["x", "y", "z"]
```

These are conventions, not an enforced enum. The runtime performs no
validation on qualifier keys.

## Standard frame of reference

`Robotics.Frame` documents the standard body-frame convention:

| Slot | Value | Meaning |
| --- | --- | --- |
| `forward` | `"X"` | forward axis |
| `up` | `"Z"` | up axis |
| `right` | `"Y"` | right axis |
| `yawAxis` | `"Z"` | rotation about Z |
| `pitchAxis` | `"Y"` | rotation about Y |
| `rollAxis` | `"X"` | rotation about X |

This is the standard right-handed frame used by the facet slot naming
conventions (`yaw`, `pitch` on `Tracker`; `angle` on `RotationalMotor`).

```text
Robotics.Frame.forward     // -> "X"
Robotics.Frame.up          // -> "Z"
```

## Full assembled-robot example

`examples/urobotics/robot_demo.u` demonstrates structure-tree assembly,
dict-backed localization, and frame-constant inspection with no hardware binding:

```text
var robot = Object.clone();
robot.body = Object.clone();
robot.body.head = Object.clone();
robot.body.torso = Object.clone();

// Localized group: two eyes by qualifier string.
robot.body.head.eyes = Robotics.group(["left" => Robotics.RGBLed.clone(), "right" => Robotics.RGBLed.clone()]);
robot.body.head.eyes["left"].r = 1;

// Pan motor on the head.
robot.body.head.pan = Robotics.RotationalMotor.clone();
robot.body.head.pan.angle = 0.5;

// Distance sensor on the torso.
robot.body.torso.sonar = Robotics.DistanceSensor.clone();

cout << "robot assembled";

robot.body.head.eyes["left"].r;    // echoes 1
robot.body.head.pan.angle;         // echoes 0.5
```

Run it with:

```sh
URBI_BUILD_PRESET=urobotics build/host-urobotics/urbi -i < examples/urobotics/robot_demo.u
```

## Binding values later

Facets are **inert slots**. Setting `pan.angle = 0.5` writes a plain numeric
slot; it does not send a command to a servo. To make a facet control real
hardware, an embedder or transport layer reads/writes the slots:

- **ROS2 bridge** (v0.12.x): the `ros` namespace publishes and subscribes
  to topics. A future integration tag will wire facet slots to `ros`
  publisher and subscriber handles.
- **GPIO / PWM**: an embedder calls `urbi_vm_get_slot` / `urbi_vm_set_slot` on
  the facet proto from a C ISR or control loop.
- **Simulator**: a sim adapter drives slot values from physics callbacks.

Until that wiring exists, the facet library is useful for describing robot
structure and testing script logic without hardware.

## How to enable and test

```sh
make test-urobotics          # build host-urobotics preset + run all urobotics fixtures
make test-chk-urobotics      # run only the chk fixtures under URBI_BUILD_PRESET=urobotics
make check-urobotics-determinism   # verify baked bytecode is deterministic
```

The three chk fixtures (`tests/chk/urobotics/`): `identity.chk`,
`facets.chk`, `localization.chk`. Every fixture is gated `# tunables: urobotics`
and is skipped (pass) by the default `make test` sweep.

`make clean && make test` (default build, no `URBI_ENABLE_UROBOTICS`) must still
pass — the overlay is fully absent from the default archive.

## Deferred items

The following are **not** part of this overlay:

- **Media facets**: camera, video-in, audio capture/playback, text-to-speech,
  blob detector, and speech recognizer. These need an image or sound stack
  (a future major version).
- **Trajectory generators**: sinusoidal value drivers and similar time-varying
  slot-value sources. Motor slots accept static values only in this release.
- **Variable blend modes**: mix, add, and discard blend semantics for
  multi-source effector commands are not implemented.
- **Sensor/effector read-write duality**: the protocol for distinguishing a
  commanded value (write intent) from a measured value (sensor readback) on
  the same slot is not part of this overlay.

None of these affect the facet-tree assembly or localization patterns
described above.
