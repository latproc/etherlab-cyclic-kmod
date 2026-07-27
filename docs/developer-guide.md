# User-Space Controller Developer Guide

This guide describes how to build a controller against the current
experimental API 0.18. The normative structure and ioctl semantics are in
[`uapi.md`](uapi.md); use the shared
[`elc_ethercat_uapi.h`](../include/elc_ethercat_uapi.h) definitions rather than
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
#include "elc_ethercat_uapi.h"
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

Prefer `libelcethercat` for tools and external controllers instead of
open-coding ioctls. Build with `make lib` and install headers/library with
`make install-lib PREFIX=...`. The generic library API and install layout are
specified in [`libelcethercat.md`](libelcethercat.md). That document also has an
optional appendix for one existing runtime (Clockwork/IOD); the library itself
is not Clockwork-specific.

## Lifecycle

The compatibility API uses one exclusive control file:

```c
int fd = open("/dev/elc_ethercat0", O_RDWR | O_CLOEXEC);
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

Call `ELC_IOC_GET_API_VERSION`, require the supported major version, and
then call `ELC_IOC_GET_CAPABILITIES`. Minor versions are additive, but a
controller must not call an optional operation merely because it was compiled
from a newer header.

API 0.18 reports coherent copied process images, cycle timing,
wait-for-cycle, DC diagnostics, optional output leases (including publish/arm
renew and `timeout_ms`), input history, cycle-period updates, and per-domain
output authority (`ELC_CAP_DOMAIN_OUTPUT_AUTHORITY`,
`ELC_CAP_OUTPUT_LEASE_PUBLISH_RENEW`). It does not report scheduled outputs or
delegated domain connections.

See `get_api_version()` and `get_capabilities()` in
[`elc_bus.c`](../tools/elc_bus.c).

## Discover and match the network

Use `ELC_IOC_GET_MASTER_INFO`, then query physical positions with
`ELC_IOC_GET_SLAVE_INFO`. Wait or fail clearly while EtherLab reports
`scan_busy`.

Match at least the intended physical position, vendor ID, product code, and
required revision policy. Aliases may strengthen matching where supported but
must not be the only identity mechanism. Never substitute an identical
physical device at another position for an absent logical axis.

Discovery does not parse ESI XML. Build the desired generic configuration in
user space from reviewed XML/device data.

### Revision numbers, PDO maps, and “bus not up”

These are easy to confuse:

1. **Kernel / EtherLab attach** uses alias, position, vendor ID, and product
   code only. The UAPI currently **rejects** a nonzero slave
   `revision_number` so controllers cannot pretend the transport enforces
   revision. Fixtures therefore use revision zero.
2. **User-space recipes** (ESI XML, IOD `modules.lpc`, product tables) often
   use revision to select **which PDO/CoE map** to emit for that instance.
3. **`CONFIG_VALIDATE` / `CONFIG_APPLY`** check that the submitted hierarchy is
   self-consistent. They do **not** diff it against a live bus capture or
   rewrite IOD’s map to match discovery.
4. If the submitted map is wrong for the hardware, EtherLab still tries to
   program it. Fixed-map devices then log errors such as “does not support
   changing the PDO mapping,” and slaves may stay out of OP with incomplete
   WC—the bus looks “not up” even though identity attach succeeded.

Hardware lesson on this target: two EL5152s share product code `0x14203052`
but report revisions `0x00120000` (position 3) and `0x00140000` (position 4).
Diagnostic objects differ at the same bit slots. A single product-level XML
recipe, or IOD’s requested `configured_sync_managers` view, is not enough.
Commissioning must use the **live-reported map per position** (or XML
selected with the correct `RevisionNo` per instance). See
[`testing.md`](testing.md) (full captured topology) for the evidence trail.

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
resolve each application entry with `ELC_IOC_GET_ENTRY_OFFSET`; retain the
returned `global_offset`, bit position, and bit length only for that
configuration generation. The deprecated `domain_offset` union member denotes
the same global offset and exists only for source compatibility.

[`elc_config.c`](../tools/elc_config.c) is the complete reference for
submission order, result checking, domains, padding, and offset resolution.

## Activate and follow the cycle

Activate with a validated cycle period. If DC is configured, its cycle
settings must agree with the activation period. User space selects
`elc_cycle_activate.cycle_period_ns` for each activation; the value is
immutable until deactivation.

On the current target, EtherLab 1.6.9 declares
`ecrt_master_set_send_interval()` in `ecrt.h`, but `ec_master.ko` does not
implement or export it. The equivalent character-device ioctl exists for
ordinary user-space EtherLab applications. Consequently, this external module
uses the requested period for its own cyclic timer but cannot claim to update
EtherLab's separate operation-FSM interval. Do not patch around missing kernel
symbols; detect and document the installed API.

`ELC_IOC_CYCLE_WAIT` provides an interruptible, bounded sleep for a record
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

API 0.13 provides coherent monotonic cycle timing via
`ELC_IOC_CYCLE_GET_INFO` / `ELC_IOC_CYCLE_WAIT`. API 0.16 adds
`ELC_IOC_CYCLE_GET_DC_INFO`, which associates the same cycle index with the
exact 64-bit application time passed to EtherLab, reference validity and the
low-32-bit reference sample, normalized phase difference, and applied
adjustment. Those DC fields are published under the same lock as the base
cycle record. Aggregate diagnostics remain available through
`ELC_IOC_CYCLE_GET_DC_STATUS`.

A controller woken after cycle N can only compute for a future cycle; it
cannot change the datagram already sent for N. Tightly scheduled motion
therefore still needs a planned bounded cycle-addressed output queue and an
explicit lead/underrun policy. Capability discovery does not advertise
deferred scheduled outputs.

## Read coherent inputs

Call `ELC_IOC_GET_INPUT_SNAPSHOT` with the exact active generation and a
buffer at least as large as the global process image. The result identifies
both the copied input sequence and the exact EtherCAT cycle that produced that
buffer.

Interpret only configured entry bits at their returned offsets. Check
aggregate, domain, or configured-slave validity appropriate to the
application. Do not treat bytes from an incomplete domain as current merely
because another domain is complete.

## Publish and arm outputs

`ELC_IOC_PUBLISH_OUTPUT` copies a masked output shadow. With
`domain_config_id = 0` (default), supply the full global image size; the
kernel fans the segments into each domain authority. With a non-zero
`domain_config_id`, supply only that domain's segment size. The kernel
intersects the caller mask with the topology-derived output mask, so input
and unregistered bits cannot be written. Publication returns a new output
sequence but does not arm it.

To arm, provide:

- the exact active configuration generation;
- the exact latest nonzero output sequence for the targeted authority;
- a healthy master/link and a healthy target domain authority; and
- after any fault or disarm on that authority, a publication newer than the
  recorded fault epoch.

`arm.flags = 0` arms every healthy domain that matches the sequence;
non-zero `flags` is the `domain_config_id` to arm alone. Domain status
reports that domain's `outputs_armed` and `rearm_required`; aggregate IO
status reports any-armed / any-rearm.

Arming retained output is a safety-relevant application decision. Publish a
known safe shadow before the first arm.

`ELC_IOC_DISARM_OUTPUTS` closes the selected authority gates (`flags = 0`
for all domains) and waits for the cyclic task to queue/send zero-gated
images for those authorities. A timeout is an error, but the gates remain
closed. Always disarm during orderly shutdown even though close also
performs fail-safe teardown.

## Recover from faults

Master/link loss disarms every domain authority and latches `rearm_required`
on each. A domain WC or assigned-slave fault disarms only that domain;
another healthy domain may stay armed. Cyclic receive and input publication
continue where EtherLab permits. Recovery never silently reuses a stale
armed shadow for an affected authority.

After the affected domain health returns:

1. verify that domain's slave states and WC;
2. decide in application policy that restart is safe for that domain;
3. publish a fresh safe output generation (global or domain-scoped);
4. explicitly arm that exact generation for the target domain(s).

Power restoration, lease renewal, or a new planner command must never arm
outputs by itself.

## Use an output lease

API 0.14+ can require controller liveness while outputs are armed. API 0.18
makes the hang-failsafe usable for multi-ms userspace loops (e.g. iod-elc):

- Prefer `timeout_ms` (500–2000 recommended) so you need not convert bus
  periods yourself; optional `cycle_budget` remains.
- `flags` = 0 for all domains, or a `domain_config_id` for one domain (e.g.
  servos only).
- Configure after domain create; **0.18 allows configure while cycling** so
  you can reach OP first, then enable the lease.
- Remaining is **seeded** on configure. Successful **publish** and **arm**
  refill the budget in the kernel when
  `ELC_CAP_OUTPUT_LEASE_PUBLISH_RENEW` is set — no high-rate renew ioctl.
- Explicit `ELC_IOC_RENEW_OUTPUT_LEASE` still works; a no-op full refill does
  not bump the renewal counter.

While armed, the cyclic task decrements remaining once per armed selection.
When remaining hits zero it disarms, zeros that domain's outputs, latches
`ELC_IO_FAULT_CONTROLLER_STALE` and `rearm_required`, and keeps cycling
inputs. Recovery needs a newer publication and explicit arm (publish itself
refills the budget). Orderly disarm/deactivate/close are not lease expiries.

The lease detects a controller that is still open but stops publishing. It
does not replace STO, E-stop, or other hardware safety.

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
