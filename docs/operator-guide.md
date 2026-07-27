# Standalone Operator Guide

This guide covers the current experimental standalone transport. It does not
install or start a user-space control system and does not authorize machine
motion or nonzero EtherCAT output.

## Preconditions

- Use the recorded target kernel/EtherLab build or explicitly supply matching
  paths as described in
  `docs/building/etherlab-dkms-environment.md`.
- Stop every other EtherLab application. Master 0 has one application owner.
- Confirm hardware motion inhibition using the site's commissioning procedure.
- Run privileged hardware commands as root.
- Confirm the working tree and selected fixture are the intended versions.

## Build and verify the contract

Optional cyclic scheduler controls are immutable module parameters. The tested
target used the following example settings:

```sh
sudo insmod kernel/elc_ethercat.ko cycle_cpu=1 cycle_fifo_priority=70
```

Omit both parameters to preserve normal scheduler policy and affinity.
Activation rejects an offline/out-of-range CPU or invalid FIFO priority.

```sh
uname -r
make check-build-env
make -j6
```

`check-build-env` must resolve `ecrt.h`, generated target headers, and
`Module.symvers` from the same kernel/EtherLab build. Do not continue through
an ambiguous or mismatched result.

## Discovery-only check

```sh
sudo insmod kernel/elc_ethercat.ko
sudo tools/elc_bus
sudo rmmod elc_ethercat
ethercat master
```

After release, `ethercat master` must report `Phase: Idle` and `Active: no`.
For the repeatable ABI and identity comparison:

```sh
sudo tools/elc_test_bus.sh
```

## Validate configuration without hardware activation

Syntax-only validation does not claim the master:

```sh
tools/elc_config check \
  tools/configs/ed3l_velocity_dc_pos29.conf
```

The EtherLab build-path contract is also testable without claiming the master:

```sh
make test-build-contract
```

`prepare` claims master 0, constructs the declarative configuration and
domain set, prints stable entry IDs and global offsets, but does not activate
or send cyclic process data:

```sh
sudo insmod kernel/elc_ethercat.ko
sudo tools/elc_config prepare \
  tools/configs/ed3l_velocity_dc_pos29.conf
sudo rmmod elc_ethercat
ethercat master
```

The ED3L fixture is target-specific commissioning input; it is not kernel
policy.

API 0.12 accepts explicit availability/validity domains. Each slave must
be assigned exactly once when any domain is declared:

```text
domain 1
domain 2
domain_slave 1 1 1
domain_slave 2 2 2
```

The fields are `domain DOMAIN_ID` and
`domain_slave ASSIGNMENT_ID SLAVE_ID DOMAIN_ID`. Declaration order defines
the contiguous domain-segment order in the copied global process image. Files
without these records retain the implicit single-domain compatibility mode.

## Log process-data value changes

`cycle-log` runs a **disarmed** cyclic session and writes only **changed**
values for named entries. Each log line is simply:

```text
name value
```

Create a name map file (see `tools/configs/ed3l_pos29_io_names.txt`):

```text
# ENTRY_ID NAME
0x604100 Statusword
0x606c00 VelocityActual
# or object INDEX SUBINDEX NAME
object 0x6041 0 Statusword
```

Then:

```sh
sudo insmod kernel/elc_ethercat.ko
# log to stdout for 10 s at 1 ms
sudo tools/elc_config cycle-log \
  tools/configs/ed3l_velocity_pos29.conf 1000000 10 \
  tools/configs/ed3l_pos29_io_names.txt
# or write a file
sudo tools/elc_config cycle-log \
  tools/configs/ed3l_velocity_pos29.conf 1000000 10 \
  tools/configs/ed3l_pos29_io_names.txt /tmp/io-changes.log
sudo rmmod elc_ethercat
```

The first sample for each name is always logged; later lines appear only when
the value changes. Outputs remain disarmed.

## Zero-output cyclic check

Only after motion inhibition is confirmed:

```sh
sudo insmod kernel/elc_ethercat.ko
sudo tools/elc_config cycle \
  tools/configs/ed3l_velocity_dc_pos29.conf 1000000 8
sudo rmmod elc_ethercat
ethercat master
```

The command prints cycle/WC/DC status, global I/O status, per-domain status
(WC, validity, and that domain's arm/re-arm under API 0.17),
per-configured-slave status, publication sequence, and a copied input
snapshot. `cycle` leaves outputs disarmed. The all-ones shadow it publishes is
not allowed through the kernel output gate until an explicit arm.

API 0.17 may publish/arm a single domain (`domain_config_id` / arm `flags`) or
all domains (`0`). Current standalone tools use the global selectors by
default.

Expected functional evidence for the current position-29 fixture includes:

- `wc_state=2` (complete);
- `healthy=1`, `armed=0`, and no current faults;
- configured slave online, operational, and data-valid; and
- a final idle/inactive master after close.

Do not interpret a short functional run as timing acceptance. Record errors,
overruns, maximum lateness, DC convergence, and the declared system load.

For a bounded disarmed baseline/load comparison of the captured full topology:

```sh
sudo env ELC_MOTION_INHIBITED=YES tools/elc_test_timing.sh
```

The harness declares its period, duration, repetition count, cyclic CPU/FIFO
priority, load modes, and maximum-lateness criterion. It runs baseline,
same-CPU, and all-online-CPU load trials; requires complete/valid domains and
all configured slaves operational; and verifies idle teardown, unchanged
topology, and fatal kernel diagnostics. Override its `ELC_TEST_*`
environment variables only when the changed test conditions are recorded.
Each trial first requires every configured slave to reach OP and every domain
to become valid; it aborts before the timed interval otherwise. Generated load
starts only after that strict-health gate. The command remains disarmed and
does not provide DC evidence when its configuration contains no DC records. It
prints new kernel warning/error lines; the target EtherLab lifecycle can log
Sync Manager watchdog transitions after deactivation even when the master
subsequently returns idle.

For a short rate screen that keeps all load phases inside one activation, set
`ELC_TEST_CONTINUOUS_PHASES=YES`. This avoids conflating the known
re-activation transition boundary with cyclic timing, but reports one
aggregate maximum-lateness value for all phases. It is characterization, not
the default multi-trial acceptance gate.

To activate conservatively and begin the measured phases only after a live,
acknowledged period change, also set `ELC_TEST_START_PERIOD_NS`. For example,
`ELC_TEST_START_PERIOD_NS=1000000` with
`ELC_TEST_PERIOD_NS=333333` first reaches the strict-health gate at 1 kHz,
then changes the disarmed non-DC session at a cycle boundary. The harness
aborts if either OP/valid health or the period acknowledgement fails.

## Zero-only arm and teardown checks

These tests never request nonzero output but still require motion inhibition:

```sh
sudo env ELC_MOTION_INHIBITED=YES \
  tools/elc_test_cycle_lifecycle.sh

sudo env ELC_MOTION_INHIBITED=YES \
  tools/elc_test_controller_death.sh

sudo env ELC_MOTION_INHIBITED=YES \
  tools/elc_test_process_image_allocations.sh
```

Each harness verifies stable physical identity/topology, master release, task
cleanup, and newly added kernel warning/error lines.

To exercise the optional controller lease with a zero-only image:

```sh
sudo env ELC_MOTION_INHIBITED=YES \
  tools/elc_config cycle-zero-lease \
  tools/configs/ed3l_velocity_dc_pos29.conf 1000000 1
```

This renews a 100-cycle lease, arms only an all-zero publication, waits for
expiry, verifies that inputs continue while outputs are gated, then proves
that re-arming requires both renewal and a fresh zero publication.

## Non-activating stress checks

These tests do not apply SDO writes or activate cyclic I/O:

```sh
sudo tools/elc_test_allocation_failures.sh
sudo tools/elc_test_config_stress.sh
```

The allocation harness uses disabled-by-default test module parameters. Never
load a production-intended instance with either failure parameter enabled.

## SDO diagnostics and commissioning

Read-only upload example:

```sh
sudo tools/elc_sdo read 29 0x6060 0 1
```

Parse and stage an ordered recipe without applying it:

```sh
sudo tools/elc_sdo stage \
  tools/recipes/ed3l_velocity_pdo_pos29.txt
```

`write` and `recipe` mutate physical slave state. They require a separate
commissioning decision, motion inhibition, exact target verification, and a
rollback/readback plan.

## Safe stop and recovery

Normal tool exit closes the device. The kernel then disarms outputs, stops and
joins the cyclic task, deactivates EtherLab, invalidates owned pointers, and
releases master 0. If a controller is killed, file release is the final
synchronous unwind.

After any abnormal result:

1. do not force-unload the module;
2. confirm the controller process has exited;
3. wait for `/dev/EtherCAT0` to return;
4. run `ethercat master` and confirm idle/inactive;
5. inspect `dmesg --level=err,warn`; and
6. preserve logs and the exact fixture before retrying.

If an open file still owns the module, normal `rmmod elc_ethercat` must fail.
Resolve the owner; never bypass module reference protection.

## Bounded one-bit output commissioning

Use this only after reviewing the exact output and physical power state. The
command rejects an entry unless the configuration proves it is one unique,
single-bit output. Its update mask contains only that bit, the pulse is
limited to five seconds, and cleanup closes the control fd so the kernel
returns configured outputs to zero.

The current machine fixture is limited to the core-console EL2034 at position
15. Stable entry ID `0x701001` is the panel status indicator and `0x700001` is
the buzzer:

```sh
sudo env ELC_NONZERO_OUTPUT_AUTHORIZED=YES \
  ./tools/elc_config pulse-entry \
  tools/configs/el2034_core_console_pos15.conf \
  1000000 0x701001 1000
```

This acknowledgement does not authorize other entries. Confirm terminal
identity, power isolation, and the intended observable output at the machine.
Never use it for drive enable or motion.

## Interactive I/O commissioning

`elc_io` uses the same validated configuration path as `elc_config`:

```sh
sudo tools/elc_io tools/configs/all34_captured_topology.conf 1000000
```

The session activates with outputs disarmed. `list`, `read`, `watch`, and
`status` are available without nonzero-output authorization. `set` only stages
a value, and `publish` only copies the masked shadow into the kernel while the
gate remains disarmed. Actual output requires a later explicit `arm` command.

The CLI refuses `arm` unless it was started with
`ELC_NONZERO_OUTPUT_AUTHORIZED=YES`. Set that variable only under the site's
commissioning procedure after identifying the exact stable entry ID and
physically confirming that the output is safe. `disarm` synchronously
zero-gates the output. `quit` disarms if necessary, deactivates, and releases
the master; process death invokes the kernel's descriptor-release teardown.
The CLI also prohibits publishing a replacement image while armed.

## Unload and local cleanup

In-tree development teardown is:

```sh
sudo rmmod elc_ethercat
ethercat master
make clean
```

For a persistent module-tree installation:

```sh
sudo make install
sudo modprobe elc_ethercat
sudo rmmod elc_ethercat
sudo make uninstall
```

`install` places only `elc_ethercat.ko` and `elc_ethercat_probe.ko` under
`/lib/modules/$(uname -r)/extra/elc_ethercat/` and refreshes module
dependencies. `uninstall` removes only those two files and refreshes
dependencies. It does not remove EtherLab/DKMS artifacts.

Packaging can stage the same layout without touching the live module tree:

```sh
make DESTDIR=/path/to/package-root install
make DESTDIR=/path/to/package-root uninstall
```
