# ROS2 bridge

The ROS2 bridge (`<urbi/ros.h>`, v0.12.0) is an optional component that exposes
a `ros` urbiscript namespace for publishing and subscribing to ROS2 topics,
calling services, and registering service handlers. It is **off by default**:
the default `liburbi.a` contains no ROS symbols. Build with
`URBI_ENABLE_ROS2=1` to include the bridge.

It is **EXPERIMENTAL** — the API may change before v1.0.

## Configuration

| Flag | Default | Effect |
| --- | --- | --- |
| `URBI_ENABLE_ROS2` | undefined (off) | Master gate. Off: no `ros` symbols, no `URosTransport` type, no overhead in the core VM. On: compiles `src/ros/` and links the bridge into the archive. |

Build with the ROS preset to get a consistent include and define set:

```sh
make test-ros2                       # unit + chk under URBI_BUILD_PRESET=ros
make TARGET=host-ros2                # build the ros-capable library + binaries
```

Do **not** add `-DURBI_ENABLE_ROS2=1` to the default `TARGET=host` build
path: both targets share `build/host/`, and mixing baked objects from different
flag sets causes stale-object collisions.

## Urbiscript surface

### Initialization

```text
ros.init("my_node_name");
```

Connects to the ROS2 middleware (the mock transport on v0.12.0). Must be
called before creating any publisher, subscriber, client, or service.

### Messages

Messages are urbiscript objects with named slots corresponding to the ROS
field layout.

```text
var t = ros.msg("geometry_msgs/Twist");
t.linear.x = 1.0;
t.angular.z = 0.5;
```

Nested message types, string fields, and sequence (array) fields are all
supported (e.g. `sensor_msgs/LaserScan` with a `float[]` `ranges` field and a
nested `std_msgs/Header`).

### Publishers

```text
var pub = ros.publisher("/cmd_vel", "geometry_msgs/Twist");
pub.publish(t);
```

`ros.publisher(topic, type)` allocates a publisher handle. `.publish(msg)`
serializes the message via the transport vtable.

### Subscribers

```text
var sub = ros.subscribe("/odom", "nav_msgs/Odometry");
at (sub?(var m)) {
    Io.println("x = " + m.pose.position.x);
};
```

`ros.subscribe(topic, type)` returns an `Event`. Incoming messages are queued
by the transport and delivered during `urbi_ros_pump`, which fires the event
on the VM strand. Use `at (sub?(var m))` to bind the message object.

### Clients (request/response)

```text
var cli = ros.client("/add_two_ints", "example_interfaces/AddTwoInts");
var req = ros.msg("example_interfaces/AddTwoInts_Request");
req.a = 3; req.b = 4;
var resp = cli.call(req, "example_interfaces/AddTwoInts_Response");
```

`ros.client(svc, req_type)` allocates a client handle. `.call(req, resp_type)`
is synchronous over the mock transport in v0.12.0.

### Services

```text
ros.service("/my_svc", "example_interfaces/AddTwoInts", function (req) {
    var resp = ros.msg("example_interfaces/AddTwoInts_Response");
    resp.sum = req.a + req.b;
    resp
});
```

`ros.service(name, req_type, handler)` registers a handler closure.
Service handler invocation (dispatching incoming requests to the handler on
the VM strand) is deferred to v0.12.1.

## Supported message types

The v0.12.0 codegen manifest (`tools/urbi-rosgen.py`) covers:

- `std_msgs/Bool`, `std_msgs/Int32`, `std_msgs/Int64`,
  `std_msgs/Float32`, `std_msgs/Float64`
- `geometry_msgs/Vector3`, `geometry_msgs/Twist`
- `example_interfaces/AddTwoInts_Request`,
  `example_interfaces/AddTwoInts_Response`

**Not yet supported:** string fields, sequence (array) fields. Scalar and
nested-scalar fields only.

## Transport seam

The bridge calls into ROS middleware through a `URosTransport` vtable:

```c
typedef struct URosTransport {
    int  (*init)(const char *node_name, void *ud);
    int  (*publish)(const char *topic, const void *buf, size_t len, void *ud);
    int  (*subscribe)(const char *topic, const char *type, void *ud);
    int  (*poll)(struct URosMsgEnvelope *out, void *ud);
    void (*destroy)(void *ud);
    void *ud;
} URosTransport;
```

v0.12.0 ships only the **mock transport** (`src/ros/ros_transport_mock.c`),
which records published messages in a ring buffer and allows tests to inject
incoming messages via `urbi_ros_mock_inject`. Real rclc/DDS integration
lands in v0.12.1.

## Public C API

Three symbols are declared in `include/urbi/ros.h`, all gated by
`URBI_ENABLE_ROS2`. They appear in Tier 3 (EXPERIMENTAL) of
`docs/api-surface-tiers.md`.

```c
/* Allocate + install the ros native namespace proto on the VM.
 * Idempotent. Called from stdlib boot. */
int  urbi_ros_register(struct UVM *vm);

/* Bind ros as a realm global pointing at the cached proto.
 * Called from urbi_populate_realm_globals (post-bake hook). */
int  urbi_ros_register_globals(struct UVM *vm, struct URealm *realm);

/* Drain the transport incoming queue once and emit events.
 * Called once per urbi_step. No-op if ros.init() was never called. */
void urbi_ros_pump(struct UVM *vm);
```

## Test strategy

Tests are mock-backed and host-only. No real ROS installation is required.

```sh
make test-ros2          # build ros preset + unit suite + 4 chk fixtures
make check-rosgen       # verify urbi-rosgen.py output is deterministic
make check-rosgen-determinism   # codegen determinism gate
```

The 4 chk fixtures (`tests/chk/ros/`): `import_ros.chk`, `init.chk`,
`reactive_loopback.chk`, `service.chk`.

`make clean && make test` (default build, no `URBI_ENABLE_ROS2`) must still
pass — the bridge is fully absent from the default archive.

## Known limitations in v0.12.0

- `import ros` is not a language keyword. The `ros` identifier is a realm
  global bound by `urbi_ros_register_globals`. Use `ros.init(...)` directly.
- Service handler invocation deferred. `ros.service()` records the handler
  closure but does not yet dispatch incoming service requests to it.
- `__injectInt32` is a mock-only test hook exposed on the `ros` proto. It is
  not part of the public API and will be removed when the real transport lands.
- Single process-global bridge. Each `UVM` has at most one `URosTransport`.
  Multi-realm ROS routing is deferred to v1.x.
- Build via `make test-ros2` / `TARGET=host-ros2` only. Enabling
  `URBI_ENABLE_ROS2=1` in the default `TARGET=host` build collides with
  pre-baked objects in `build/host/` and will produce link errors or silent
  runtime failures.
