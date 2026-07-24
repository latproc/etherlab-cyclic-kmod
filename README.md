# EtherLab Cyclic Kernel Module

Experimental standalone Linux kernel transport for deterministic EtherCAT
cyclic I/O using the EtherLab master, with runtime user-space configuration of
slaves, PDOs, and SDOs.

This project is under development. It is not production-ready. Faulty kernel
code can crash the host, and EtherCAT outputs may control industrial machinery.
Develop with motion safely inhibited; this software is not a substitute for
hardware safety systems.

## Current scope

The experimental API 0.12 implements bus discovery, bounded commissioning SDOs,
transactional declarative slave/Sync/PDO/entry/DC configuration, domain
registration with explicit user-defined validity groups, a configurable cyclic
thread, distributed clocks, copied process-image exchange, aggregate,
per-domain, and per-configured-slave health/validity status, and an explicit
global output arm gate.

Discovery is proven on the current 34-slave target. A single ED3L
configuration has reached OP with complete working counter, recovered from
servo-supply loss without restarting the controller, and passed zero-only
output arm/disarm tests. No nonzero output has been authorized or tested. The
API 0.12 documentation is being re-audited; the standalone kernel-safety gate
remains open. See
[the current architecture review](docs/architecture-review-2026-07-24.md).

```text
standalone user-space tools/controller
    |
versioned ioctl UAPI
    |
/dev/cw_ethercat0
    |
    v
transactional configuration + cyclic transport
    |
    v
EtherLab master and EtherCAT network
```

The long-term module remains device-agnostic. User space owns identity
matching, PDO and setup-SDO policy, and application semantics; the kernel owns
EtherLab lifecycle, validated configuration execution, domain mapping, and
cyclic transport.

## Build the probe

On the currently tested EtherLab DKMS environment:

```sh
make check-build-env
make
```

Optional module-tree installation and removal:

```sh
sudo make install
sudo make uninstall
```

See [the recorded DKMS environment](docs/building/etherlab-dkms-environment.md)
for exact detected paths and explicit overrides.
The complete build-to-teardown sequence is in the
[standalone operator guide](docs/operator-guide.md).

The build produces:

```text
kernel/cw_ethercat.ko
kernel/cw_ethercat_probe.ko
tools/cw_ec_bus
tools/cw_ec_abi_test
tools/cw_ec_sdo
tools/cw_ec_config
tools/cw_ec_config_stress
```

## Test the probe

The lifecycle test loads the module, which briefly acquires and immediately
releases master 0, then unloads the module:

```sh
sudo tools/cw_ec_test_master.sh
```

Repeat the lifecycle:

```sh
sudo CW_EC_TEST_REPEAT=100 tools/cw_ec_test_master.sh
```

Review kernel logs after testing. Do not force-unload modules.

## Discover the bus

IOD or any other EtherLab application must be stopped because master ownership
is exclusive:

```sh
sudo insmod kernel/cw_ethercat.ko
sudo tools/cw_ec_bus
sudo rmmod cw_ethercat
```

Run the repeatable ABI and identity-comparison test:

```sh
sudo tools/cw_ec_test_bus.sh
```

The module claims master 0 only while a process has `/dev/cw_ethercat0` open.
See [the UAPI documentation](docs/uapi.md).

## Commissioning SDO tool

`cw_ec_sdo` provides bounded diagnostic uploads, single writes, and ordered
recipe execution. Writes require exclusive master ownership and alter physical
slave state. They are not the future persistent recovery mechanism.

## Declarative configuration tool

`cw_ec_config` reads a dependency-free line format containing generic domain,
slave assignment, slave, Sync Manager, PDO, and PDO-entry records. Domain
grouping is optional; a file without domain records uses one implicit
compatibility domain:

```sh
./tools/cw_ec_config check tools/configs/ed3l_velocity_pos29.conf
./tools/cw_ec_config prepare tools/configs/ed3l_velocity_pos29.conf
```

`check` validates syntax and resource counts without opening the device.
`prepare` submits the hierarchy, constructs EtherLab configuration, creates
the domain set, and prints global offsets keyed by stable entry IDs. It does
not activate the master or send process data. The explicit-domain example is:

```sh
./tools/cw_ec_config check \
  tools/configs/el5152_pos3_with_absent_ed3l_pos29.conf
```

With motion safely inhibited, `cycle` activates a configuration while outputs
remain disarmed. It publishes an all-ones shadow only to prove that publication
does not bypass the gate:

```sh
sudo ./tools/cw_ec_config cycle \
  tools/configs/ed3l_velocity_dc_pos29.conf 1000000 5
```

`cycle-zero-arm` is a separate zero-only gate test. It arms an all-zero shadow,
synchronously disarms it, verifies stale-sequence rejection, publishes a fresh
zero shadow, arms it, and disarms again:

```sh
sudo ./tools/cw_ec_config cycle-zero-arm \
  tools/configs/ed3l_velocity_dc_pos29.conf 1000000 5
```

Neither command requests a nonzero transmitted output. These fixtures are
commissioning examples for the recorded target, not device policy embedded in
the module.

Repeat the complete zero-only lifecycle only after confirming the commissioning
state:

```sh
sudo env CW_EC_MOTION_INHIBITED=YES CW_EC_TEST_REPEAT=5 \
  tools/cw_ec_test_cycle_lifecycle.sh
```

The script checks cyclic task cleanup, idle master release, unchanged topology,
and newly added kernel warning/error lines.

The controller-death test explicitly arms an all-zero shadow, kills the
standalone controller, and verifies synchronous release:

```sh
sudo env CW_EC_MOTION_INHIBITED=YES \
  tools/cw_ec_test_controller_death.sh
```

No nonzero output is requested.

Deterministic module-owned allocation unwind can be tested without applying
SDOs or activating cyclic I/O:

```sh
sudo tools/cw_ec_test_allocation_failures.sh
```

Maximum pending configuration limits and repeated synchronous cleanup can be
tested without applying configuration or activating the master:

```sh
sudo tools/cw_ec_test_config_stress.sh
```

Copied process-image allocation unwind requires the motion-inhibited hardware
fixture for its successful activation boundary:

```sh
sudo env CW_EC_MOTION_INHIBITED=YES \
  tools/cw_ec_test_process_image_allocations.sh
```

Examples:

```sh
sudo tools/cw_ec_sdo read 29 0x6060 0 1
tools/cw_ec_sdo validate 29 u32 0x1600 1 0x60400010
```

Do not run write/recipe commands while IOD owns the master or while machine
motion is not safely inhibited.

## License

GPL-2.0-only. See `LICENSE`.
