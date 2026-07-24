# EtherLab Cyclic Kernel Module

Experimental standalone Linux kernel transport for deterministic EtherCAT
cyclic I/O using the EtherLab master, with runtime user-space configuration of
slaves, PDOs, and SDOs.

This project is under development. It is not production-ready. Faulty kernel
code can crash the host, and EtherCAT outputs may control industrial machinery.
Develop with motion safely inhibited; this software is not a substitute for
hardware safety systems.

## Current scope

Phase 2 bus discovery, the provisional Phase 3 commissioning SDO interface,
and non-mutating validation of a pending declarative PDO hierarchy are
implemented. Discovery is proven on the current 34-slave target. The project
does not yet apply persistent slave/PDO configuration or provide cyclic
process data.

```text
cw_ec_bus
    |
versioned ioctl UAPI
    |
/dev/cw_ethercat0
    |
    v
EtherLab master
    |
    v
EtherCAT network
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

Examples:

```sh
sudo tools/cw_ec_sdo read 29 0x6060 0 1
tools/cw_ec_sdo validate 29 u32 0x1600 1 0x60400010
```

Do not run write/recipe commands while IOD owns the master or while machine
motion is not safely inhibited.

## License

GPL-2.0-only. See `LICENSE`.
