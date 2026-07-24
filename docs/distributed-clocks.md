# Distributed Clocks

## Production status

Distributed clocks are mandatory for the kernel backend when enabled by user
configuration.

API 0.5 accepts, validates, and applies generic per-slave DC parameters plus
disabled/automatic/explicit reference policy. The kernel now implements the
reference-led IOD controller, cyclic slave synchronization, synchrony
monitoring, and a bounded status snapshot. It builds, passes non-activating ABI
checks, and has completed four motion-inhibited position-29 hardware runs with
complete working counter, valid reference reads, and successful synchrony
monitoring. See `testing.md` for exact results and limitations.

The installed Clockwork build enables `USE_DC` for both `iod` and `iod_sdo`:

```text
/opt/latproc/iod/CMakeLists.txt
```

The migration must preserve this algorithm before attempting improvements.

## Official examples for a kernel module

EtherLab does not provide one modern example combining an ordinary Linux
out-of-tree module, PREEMPT_RT `kthread`/high-resolution scheduling, and DC.
Use these installed EtherLab 1.6.9 examples together:

- `examples/mini/mini.c` for external kernel-module master ownership,
  callbacks/locking, configuration, cyclic calls, and synchronous teardown;
- `examples/rtai_rtdm_dc/main.c` for a DC-reference-led correction loop;
- `examples/dc_user/main.c` for current DC API ordering and an
  application-clock-led alternative.

The upstream source is the
[official EtherLab repository](https://gitlab.com/etherlab.org/ethercat/).

Do not copy `mini.c`'s jiffies timer for this project; it is unsuitable for a
500 microsecond cycle. Do not copy RTAI/RTDM scheduling primitives into a
normal Linux module. The intended implementation is a dedicated kernel thread
with a high-resolution absolute-period wakeup design verified on PREEMPT_RT.

Borrow the EtherLab calls and controller math, not an obsolete scheduling
framework.

## Two valid clock-master modes

The official examples demonstrate two distinct policies:

### Application/host clock leads

`dc_user` supplies its absolute target wake time through
`ecrt_master_application_time()`, calls
`ecrt_master_sync_reference_clock_to()` to steer the EtherCAT reference toward
the host/application clock, then calls `ecrt_master_sync_slave_clocks()`.

### EtherCAT reference clock leads

The RTAI/RTDM DC example reads `ecrt_master_reference_clock_time()` and adjusts
the application schedule toward the reference. Current IOD follows this
general policy with its own bounded filter and phase correction.

These policies must not be mixed accidentally. The configuration model should
eventually name the mode explicitly. For migration, preserve the current IOD
"application follows reference" mode and its controller constants. A
host-led mode can be added and benchmarked separately.

## Current reference selection and initialization

During activation, IOD:

1. calls `ecrt_master_select_reference_clock(master, nullptr)`, allowing
   EtherLab to select the first DC-capable slave;
2. obtains `CLOCK_MONOTONIC` time in nanoseconds;
3. rounds it down to the 2 kHz cycle boundary;
4. resets filter, monitor, difference, and adjustment state;
5. calls `ecrt_master_application_time()` before activation to seed DC offset
   initialization;
6. activates the master.

The kernel API must allow automatic reference selection and later an explicit
user-selected reference configuration. Reference choice is policy supplied by
user space; the cyclic implementation belongs in the kernel.

## Current cyclic ordering

Receive half:

```text
ecrt_master_receive
ecrt_domain_process
ecrt_master_reference_clock_time
process queued synchrony-monitor result
domain/master/slave state checks
```

Send half:

```text
advance corrected application time
ecrt_master_application_time
ecrt_master_sync_slave_clocks
optionally ecrt_master_sync_monitor_queue
ecrt_domain_queue
ecrt_master_send
```

This ordering must be preserved initially.

## Current correction algorithm

IOD compares the low 32 bits of application time with the reference clock.
The subtraction intentionally uses signed 32-bit rollover behavior.

The difference is normalized to the nearest cycle phase in:

```text
[-cycle_period / 2, +cycle_period / 2]
```

For every 1024 valid reference samples, it adjusts the per-cycle correction by:

```text
average change in phase difference
+ one nanosecond in the sign direction of accumulated phase error
```

The accumulated cycle adjustment is clamped to `[-1000, +1000]` ns.

Every cycle, application time advances by:

```text
nominal_cycle_ns - cycle_adjustment_ns - sign(last_difference_ns)
```

IOD does not call `ecrt_master_sync_reference_clock()` in this algorithm. It
steers its application-time progression toward the EtherLab-selected reference
and calls `ecrt_master_sync_slave_clocks()`.

## Synchrony monitoring

Approximately once per second, IOD queues
`ecrt_master_sync_monitor_queue()`. Subsequent receive cycles call
`ecrt_master_sync_monitor_process()`.

It records/reports:

- application/reference difference;
- maximum slave deviation;
- cycle adjustment;
- reference read failure/resumption;
- monitor timeout after ten cycles without a result.

The kernel path must expose these as counters/snapshots without formatted
per-cycle logging.

## Kernel ownership and locking

The kernel cyclic thread owns all mutable DC controller state:

- application time;
- last difference;
- filter accumulators/count;
- cycle adjustment;
- reference validity/result;
- monitor pending/countdown/timeout;
- maximum slave deviation.

Configuration fields are immutable while running. Status readers receive a
bounded snapshot through atomics, sequence counting, or a short non-sleeping
snapshot lock. No user-space call, allocation, blocking mailbox operation, or
formatted log belongs in the DC cycle path.

## Configuration model

The configuration transaction now provides:

- DC disabled/enabled;
- automatic versus explicit reference selection;
- explicit reference slave configuration ID when selected;
- cycle period in nanoseconds;
- per-slave `assign_activate`, Sync0/Sync1 cycle, and shift values;
- monitoring interval and timeout remain to be added with conservative
  defaults;
- adjustment clamp/filter values only if operational evidence requires them
  configurable.

Do not expose raw kernel pointers or EtherLab configuration addresses.

The installed ED3L ESI file
`/opt/latproc/code/config/xml/ESTUN_SUMMA_SERVO_V1.00B9.xml` provides the
first fixture values for its DC-enabled operation mode:

```text
AssignActivate = 0x0300
CycleTimeSync0 = application cycle * 1 + 0
ShiftTimeSync0 = 0 ns
```

At the current 1 ms standalone test period this resolves to a 1,000,000 ns
SYNC0 cycle. These values belong in the user-space fixture/configuration, not
as kernel device policy.

## Required tests

- compare direct-IOD and kernel ordering and correction results;
- reference clock absent at startup;
- DC reference appearing after power restoration;
- reference loss while cyclic operation continues;
- application-time low-32-bit rollover;
- monitor timeout and recovery;
- maximum slave deviation under CPU/I/O/network load;
- repeated activate/deactivate and topology rescan;
- all five ED3Ls power loss/restoration;
- verify no divide-by-zero or overflow for validated cycle periods.
