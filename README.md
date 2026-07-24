# EtherLab Cyclic Kernel Module

Experimental standalone Linux kernel transport for deterministic EtherCAT
cyclic I/O using the EtherLab master, with runtime user-space configuration of
slaves, PDOs, and SDOs.

This project is under development. It is not production-ready. Faulty kernel
code can crash the host, and EtherCAT outputs may control industrial machinery.
Develop with motion safely inhibited; this software is not a substitute for
hardware safety systems.

## Current scope

The experimental API 0.10 implements bus discovery, bounded commissioning SDOs,
transactional declarative slave/Sync/PDO/entry/DC configuration, domain
registration, a configurable cyclic thread, distributed clocks, copied
process-image exchange, aggregate and per-configured-slave health/validity
status, and an explicit output arm gate.

Discovery is proven on the current 34-slave target. A single ED3L
configuration has reached OP with complete working counter, recovered from
servo-supply loss without restarting the controller, and passed zero-only
output arm/disarm tests. No nonzero output has been authorized or tested. The
standalone kernel-safety and documentation acceptance gates remain open; see
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

See [the recorded DKMS environment](docs/building/etherlab-dkms-environment.md)
for exact detected paths and explicit overrides.

The build produces:

```text
kernel/cw_ethercat.ko
kernel/cw_ethercat_probe.ko
tools/cw_ec_bus
tools/cw_ec_abi_test
tools/cw_ec_sdo
tools/cw_ec_config
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

`cw_ec_config` reads a dependency-free line format containing generic slave,
Sync Manager, PDO, and PDO-entry records:

```sh
./tools/cw_ec_config check tools/configs/ed3l_velocity_pos29.conf
./tools/cw_ec_config prepare tools/configs/ed3l_velocity_pos29.conf
```

`check` validates syntax and resource counts without opening the device.
`prepare` submits the hierarchy, constructs EtherLab configuration, creates a
domain, and prints offsets keyed by stable entry IDs. It does not activate the
master or send process data.

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

Examples:

```sh
sudo tools/cw_ec_sdo read 29 0x6060 0 1
tools/cw_ec_sdo validate 29 u32 0x1600 1 0x60400010
```

Do not run write/recipe commands while IOD owns the master or while machine
motion is not safely inhibited.

## License

GPL-2.0-only. See `LICENSE`.
