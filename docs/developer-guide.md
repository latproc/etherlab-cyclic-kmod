# User-Space Controller Developer Guide

This guide describes how to build a controller against the current
experimental API 0.16. The normative structure and ioctl semantics are in
[`uapi.md`](uapi.md); use the shared
[`cw_ethercat_uapi.h`](../include/cw_ethercat_uapi.h) definitions rather than
duplicating numeric commands or layouts.

## Responsibility boundary

The kernel transport owns EtherLab lifecycle, validated configuration
execution, domain registration, cyclic exchange, copied images, low-level
health, timing, and output gates.

Your controller owns:

- slave identity and topology matching;
- ESI/XML parsing and device recipes;
- Sync Manager, PDO, entry, setup-SDO, and DC policy;
- stable application entry IDs;
- interpretation and scaling of process data;
- machine state, motion planning, and recovery policy; and
- the decision to publish, arm, disarm, or stop.

Do not infer device meaning from names returned by discovery. Match the
required physical topology and identity fields explicitly.

## Build against the UAPI

Include the repository header:

```c
#include "cw_ethercat_uapi.h"
```

Build with the repository include directory, for example:

```sh
cc -O2 -Wall -Wextra -Werror -std=c11 \
  -I/path/to/etherlab-cyclic-kmod/include controller.c -o controller
```

All structures use fixed-width Linux UAPI types. Zero-initialize every request,
set `struct_size` and `api_major`, and leave every reserved field zero.
Treat every ioctl failure as meaningful; do not retry configuration errors
blindly.

Prefer `libcwethercat` for tools and external controllers instead of
open-coding ioctls. Build with `make lib` and install headers/library with
`make install-lib PREFIX=...`. The generic library API and install layout are
specified in [`libcwethercat.md`](libcwethercat.md). That document also has an
optional appendix for one existing runtime (Clockwork/IOD); the library itself
is not Clockwork-specific.

## Lifecycle

The compatibility API uses one exclusive control file:

```c
int fd = open("/dev/cw_ethercat0", O_RDWR | O_CLOEXEC);
```

Opening claims EtherLab master 0. `EBUSY` means another application owns the
master or another control file is open. Closing the fd is the final ownership
boundary: the kernel gates outputs, stops cyclic work, releases EtherLab
resources, and returns master 0 to the CLI.

The normal controller sequence is:

```text
open
  -> negotiate API/capabilities
  -> discover and match topology
  -> optional ordered setup SDOs
  -> begin/add/validate/apply configuration
  -> resolve stable entry IDs to offsets
  -> activate
  -> wait/read/publish while disarmed
  -> optionally arm exact output generation
  -> synchronously disarm
  -> deactivate
  -> close
```

Configuration is transactional and immutable while active. Use a new control
session to change topology or mapping.

## Negotiate before using features

Call `CW_EC_IOC_GET_API_VERSION`, require the supported major version, and
then call `CW_EC_IOC_GET_CAPABILITIES`. Minor versions are additive, but a
controller must not call an optional operation merely because it was compiled
from a newer header.

API 0.16 reports coherent copied process images, cycle timing,
wait-for-cycle, DC diagnostics, and optional output leases. It does not report
scheduled outputs or delegated domain connections.

See `get_api_version()` and `get_capabilities()` in
[`elc_bus.c`](../tools/elc_bus.c).

## Discover and match the network

Use `CW_EC_IOC_GET_MASTER_INFO`, then query physical positions with
`CW_EC_IOC_GET_SLAVE_INFO`. Wait or fail clearly while EtherLab reports
`scan_busy`.

Match at least the intended physical position, vendor ID, product code, and
required revision policy. Aliases may strengthen matching where supported but
must not be the only identity mechanism. Never substitute an identical
physical device at another position for an absent logical axis.

Discovery does not parse ESI XML. Build the desired generic configuration in
user space from reviewed XML/device data.

## Submit configuration

Use the `CONFIG_BEGIN`, `CONFIG_ADD_*`, `CONFIG_VALIDATE`, and `CONFIG_APPLY`
operations described in [`uapi.md`](uapi.md). Every record has a stable
user-supplied configuration ID, and references use those IDs rather than array
positions.

If explicit domains are declared:

- every configured slave must have exactly one domain assignment;
- domain declaration order defines contiguous global image segments; and
- per-slave validity follows the assigned domain's WC.

With no domain records, the kernel creates one implicit compatibility domain.
There is no two-domain limit.

PDO padding is explicit with object `0x0000:00` and `entry_id=0`. Registered
application entries require a nonzero stable entry ID. After domain creation,
resolve each application entry with `CW_EC_IOC_GET_ENTRY_OFFSET`; retain the
returned `global_offset`, bit position, and bit length only for that
configuration generation. The deprecated `domain_offset` union member denotes
the same global offset and exists only for source compatibility.

[`elc_config.c`](../tools/elc_config.c) is the complete reference for
submission order, result checking, domains, padding, and offset resolution.

## Activate and follow the cycle

Activate with a validated cycle period. If DC is configured, its cycle
settings must agree with the activation period. User space selects
`cw_ec_cycle_activate.cycle_period_ns` for each activation; the value is
immutable until deactivation.

On the current target, EtherLab 1.6.9 declares
`ecrt_master_set_send_interval()` in `ecrt.h`, but `ec_master.ko` does not
implement or export it. The equivalent character-device ioctl exists for
ordinary user-space EtherLab applications. Consequently, this external module
uses the requested period for its own cyclic timer but cannot claim to update
EtherLab's separate operation-FSM interval. Do not patch around missing kernel
symbols; detect and document the installed API.

`CW_EC_IOC_CYCLE_WAIT` provides an interruptible, bounded sleep for a record
newer than a supplied cycle index. The returned coherent record includes:

- configuration generation and global cycle index;
- scheduled and actual monotonic wake times;
- signed wake lateness and missed-deadline count;
- input sequence;
- output sequence selected in that cycle;
- stale output reuse;
- aggregate WC, health, arm state, and cycle result.

The wait is a notification, not a user-space callback in the cyclic task. A
slow client may skip intermediate records and must use the returned cycle
identity rather than assuming one wake per cycle.

### Distributed-clock control loops

The kernel owns EtherLab application time, reference sampling, DC steering,
slave synchronization, and the common cycle index. A user-space controller
must follow that published timeline rather than independently recreating or
steering the DC clock.

API 0.14 still exposes coherent monotonic cycle timing and a separate DC
diagnostic snapshot. It does not yet expose the exact application time and DC
sample coherently in the same cycle record, so it is not a complete
motion-control clock contract.

The planned compatible record will associate the global cycle index with the
exact 64-bit application time passed to EtherLab, reference validity and the
low-32-bit reference sample available from EtherLab 1.6.9, normalized phase
difference, and applied adjustment. A controller woken after cycle N can only
compute for a future cycle; it cannot change the datagram already sent for N.
Tightly scheduled motion therefore also requires the planned bounded
cycle-addressed output queue and an explicit lead/underrun policy.

## Read coherent inputs

Call `CW_EC_IOC_GET_INPUT_SNAPSHOT` with the exact active generation and a
buffer at least as large as the global process image. The result identifies
both the copied input sequence and the exact EtherCAT cycle that produced that
buffer.

Interpret only configured entry bits at their returned offsets. Check
aggregate, domain, or configured-slave validity appropriate to the
application. Do not treat bytes from an incomplete domain as current merely
because another domain is complete.

## Publish and arm outputs

`CW_EC_IOC_PUBLISH_OUTPUT` copies a complete global shadow plus an update mask.
The kernel intersects that mask with the topology-derived output mask, so
input and unregistered bits cannot be written. Publication returns a new
output sequence but does not arm it.

To arm, provide:

- the exact active configuration generation;
- the exact latest nonzero output sequence;
- a currently healthy bus; and
- after any fault or disarm, a publication newer than the recorded fault
  epoch.

Arming retained output is a safety-relevant application decision. Publish a
known safe shadow before the first arm.

`CW_EC_IOC_DISARM_OUTPUTS` closes the gate and waits for the cyclic task to
queue/send the zero-gated image. A timeout is an error, but the atomic gate
remains closed. Always disarm during orderly shutdown even though close also
performs fail-safe teardown.

## Recover from faults

Health loss disarms outputs and latches `rearm_required`. Cyclic receive and
input publication continue where EtherLab permits, allowing user space to
observe restoration. Recovery never silently reuses the old armed shadow.

After health returns:

1. verify the required domain/slave states and WC;
2. decide in application policy that restart is safe;
3. publish a fresh safe output generation;
4. explicitly arm that exact generation.

Power restoration, lease renewal, or a new planner command must never arm
outputs by itself.

## Use an output lease

API 0.14 can optionally require controller liveness while outputs are armed.
Before activation, configure an armed-cycle budget from 1 through 1,000,000
with `CW_EC_IOC_CONFIGURE_OUTPUT_LEASE`. A budget of zero preserves the
lease-disabled compatibility behavior.

Each activation starts with the enabled lease invalid. After activation,
`CW_EC_IOC_RENEW_OUTPUT_LEASE` loads the configured budget. The cyclic task
decrements it exactly once for every armed output selection. The budget pauses
while disarmed, so a monitoring-only controller need not send heartbeats.

When the budget is exhausted, the next cycle:

- selects zero outputs;
- clears the authority's arm state;
- latches `CW_EC_IO_FAULT_CONTROLLER_STALE` and `rearm_required`;
- records the current publication sequence; and
- continues cyclic input exchange.

Renewal clears the current stale-controller condition but never clears the
latched recovery epoch or arms output. Recovery requires renewal, a newer safe
output publication, and explicit arm. Query configured/remaining cycles and
renewal/expiry counters with `CW_EC_IOC_GET_OUTPUT_LEASE_STATUS`.

The lease detects a controller that remains alive but stops renewing. It does
not interpret why upstream control was lost, and it does not replace a
hardware watchdog or safety system.

`tools/elc_io` is the interactive reference controller for inspecting this
lifecycle. It resolves stable entry IDs to global offsets, decodes scalar
entries up to 64 bits, stages only configured output entries, publishes a
masked shadow while disarmed, and keeps arm as a distinct authorized action.
It deliberately shares `elc_config`'s parser and activation implementation
through the `elc_config io` frontend so commissioning behavior cannot drift
to a second configuration format.

## Error and teardown rules

Important errors include:

- `EBUSY`: exclusive owner already exists;
- `EINVAL`: malformed fields, invalid state, or invalid ordering;
- `EPROTONOSUPPORT`: incompatible API major;
- `ESTALE`: wrong configuration or output generation;
- `EAGAIN`: health/recovery precondition is not yet satisfied;
- `EMSGSIZE`: process-image buffer size mismatch;
- `ETIMEDOUT`: bounded wait or synchronous gate acknowledgement expired; and
- `ESHUTDOWN`: a cycle waiter was woken by deactivation.

On normal shutdown, disarm, deactivate, and close. On an unrecoverable
controller error, close the fd rather than leaving ownership alive. Never
force-unload the kernel module.

## Validate a new client

Before hardware activation:

1. run syntax and configuration checks;
2. exercise wrong sizes, generations, flags, reserved fields, IDs, and call
   order;
3. prove every partial construction path unwinds;
4. verify close returns EtherLab master 0 idle.

Hardware validation begins disarmed with physical motion inhibited. Follow the
[`operator guide`](operator-guide.md) and the recorded
[`testing guide`](testing.md). Do not add nonzero output tests until the exact
physical output and commissioning procedure are separately authorized.
