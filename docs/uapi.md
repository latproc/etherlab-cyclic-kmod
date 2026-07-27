# User-Space API

## Status

The current experimental API is version 0.18. It supports discovery, a
provisional bounded commissioning SDO batch, transactional
slave/Sync/PDO/entry/DC configuration, domain registration, configurable
cyclic pumping, copied process images, distributed clocks, health/fault
status, explicit output arm/disarm, per-configured-slave validity, mandatory
PDO padding, explicit multiple validity domains, coherent per-cycle timing,
capability discovery, interruptible cycle notification, and optional
authority-scoped output leases (publish/arm renew and optional timeout_ms),
disarmed cycle-boundary period updates, and per-domain output authority
(independent arm/health and scoped publish).

## Ownership and lifecycle

The module registers `/dev/elc_ethercat0` without claiming EtherLab master 0.
Opening the device read-write claims master 0 exclusively. A second open, or
an open while IOD/direct libethercat already owns the master, fails with
`EBUSY`.

Closing the only control file releases the master and all EtherLab application
state associated with this owner. The file operations hold a module reference,
so normal module unload cannot race an open file.

This initial API intentionally supports one controller and no diagnostic-only
opens. A future status interface must not bypass EtherLab's exclusive
application ownership rules.

## Versioning

The shared header is `include/elc_ethercat_uapi.h`. Structures use fixed-width
Linux UAPI types, contain no pointers, and have fixed layouts suitable for the
compat ioctl path.

The tool first calls `ELC_IOC_GET_API_VERSION`. Major versions must match.
Minor versions add:

- 0.2: provisional ad-hoc setup-SDO batch;
- 0.3: declarative configuration and stable entry offsets;
- 0.4: bounded activation, cycle status, and synchronous deactivation;
- 0.5: distributed-clock configuration, synchronization, and status;
- 0.6: generation-bound health, fault, and re-arm status;
- 0.7: coherent copied input snapshots;
- 0.8: masked copied output publication while hard-disarmed;
- 0.9: generation/sequence-bound arm and synchronous disarm;
- 0.10: stable-ID per-configured-slave state and conservative data validity;
- 0.11: explicit mandatory PDO padding with `entry_id=0`;
- 0.12: explicit ordered domains, slave-domain assignments, and per-domain
  status;
- 0.13: capability discovery, coherent cycle timing/generation records, and
  interruptible wait-for-cycle;
- 0.14: optional armed-cycle output lease configuration, renewal, status,
  deterministic expiry, and a distinct stale-controller fault;
- 0.15: acknowledged cycle-boundary period changes while outputs are disarmed
  (including rewriting matching DC SYNC0 records when DC is configured);
- 0.16: optional preactivation bounded input-image history and ordered batched
  reads with explicit record-gap and capture-contention reporting, plus
  coherent DC motion-clock records (`ELC_IOC_CYCLE_GET_DC_INFO`); and
- 0.17: per-domain output authority (independent arm/health per domain;
  publish/arm/disarm may target domain_config_id; capability bit
  `ELC_CAP_DOMAIN_OUTPUT_AUTHORITY`);
- 0.18: hang-failsafe lease improvements — remaining seeded on configure,
  successful publish/arm refill the budget (`ELC_CAP_OUTPUT_LEASE_PUBLISH_RENEW`),
  optional `timeout_ms` wall-time budget, configure while cycling, and
  per-domain lease via `flags` = domain_config_id.

Input/output structures that accept caller fields include `struct_size` and
`api_major`. The kernel rejects an unexpected size with `EINVAL` and an
incompatible major version with `EPROTONOSUPPORT`. Callers must zero every
declared reserved field. The kernel rejects nonzero reserved input with
`EINVAL`, including fields in bidirectional request/result structures.

## Operations

### `ELC_IOC_GET_API_VERSION`

Returns `struct elc_api_version`.

### `ELC_IOC_GET_CAPABILITIES`

Returns `struct elc_capabilities`. API 0.18 reports only implemented,
documented features: coherent copied process images, cycle timing,
wait-for-cycle, DC diagnostics, output leases (including publish/arm renew
and `timeout_ms`), cycle-period updates, bounded input history, and
per-domain output authority (`ELC_CAP_DOMAIN_OUTPUT_AUTHORITY`,
`ELC_CAP_OUTPUT_LEASE_PUBLISH_RENEW`).
Scheduled output and delegated domain connections are not currently reported.

### Input history

`ELC_IOC_CONFIGURE_INPUT_HISTORY` selects a generation-bound ring depth
before activation. Depth zero disables history. Depth is limited to 4,096
records and the complete ring image storage is limited to 16 MiB. Allocation
occurs during activation, before the cyclic task starts.

`ELC_IOC_GET_INPUT_HISTORY_BATCH` copies up to 256 ordered records newer than
`after_cycle_index` plus one complete global input image per record. Each
record identifies its configuration generation, cycle, input sequence,
scheduled and actual wake time, lateness, and cycle result. The caller supplies
record and image buffers; `data_capacity` must cover `max_records * image_size`.

`dropped_records` reports missing cycle records across the returned interval,
including overwritten records. `capture_drop_count` is cumulative and reports
cycles the kernel declined to capture because a reader had reserved the target
slot. Cycle IDs remain authoritative. The cyclic task never waits for a reader
and performs no history allocation. Deactivation joins the task before freeing
the ring.

### `ELC_IOC_GET_MASTER_INFO`

Returns `struct elc_master_info` containing:

- scanned slave count;
- main link state;
- scan-in-progress state;
- EtherLab application time.

### `ELC_IOC_GET_SLAVE_INFO`

The caller initializes:

- `struct_size`;
- `api_major`;
- physical `position`.

The kernel calls the target EtherLab `ecrt_master_get_slave()` API and returns
the physical position, alias, vendor ID, product code, revision, serial number,
E-bus current, AL state, error flag, Sync Manager count, SDO count, and a
diagnostic name. Device matching must use identity fields, not the name.

An unknown position returns `ENOENT`.

## Compatibility and memory safety

All current ioctl structures contain only fixed-width values and inline
arrays. No kernel address or user pointer is retained. Each ioctl copies a
complete bounded structure with `copy_from_user()`/`copy_to_user()`.

Unknown ioctl types or commands return `ENOTTY`. The non-activating hostile
ABI suite covers exclusive open, unknown command, structure size/version,
reserved fields, flags, identifiers, hierarchy/reference validation, ordering,
generations, and inactive-state rejection.

## Known limitations

- The public EtherLab `ecrt_request_master()` kernel API returns only `NULL` on
  failure, so open currently maps all claim failures to `EBUSY`.
- Master index is fixed at 0 for this prototype.
- Opening the device changes the EtherLab master from idle to application
  operation phase until close, although it does not activate a PDO domain.
- There is no wait-for-scan operation. `scan_busy` is reported to user space.
- Slave state is a scan snapshot. No topology generation is exposed yet.

## Provisional ad-hoc setup-SDO batch

The following operations exist only to prove ordered blocking SDO downloads
and reproduce commissioning recipes:

```text
ELC_IOC_SETUP_BEGIN
ELC_IOC_SETUP_ADD_SDO
ELC_IOC_SETUP_APPLY
ELC_IOC_SETUP_RESET
ELC_IOC_SDO_UPLOAD
```

They are deliberately separate from the persistent declarative configuration
transaction. The batch uses `ecrt_master_sdo_download()` and therefore applies
only to online slaves; EtherLab does not retain or replay it after power loss.

Limits are:

- 256 operations;
- 256 bytes per payload;
- 16 KiB total payload.

Sequences must be nonzero and strictly increasing. Scalar types require their
exact width; `bytes` accepts a nonempty bounded payload. Payload bytes are
already encoded in EtherCAT little-endian wire order by user space.

`SETUP_APPLY` executes in insertion/sequence order and stops at the first
failure. Its result identifies the failed sequence, slave, object, errno, and
CoE abort code. A batch becomes non-retryable as soon as apply starts because
earlier writes may have succeeded before a later failure. User space must
explicitly begin and resubmit a new batch.

There is no physical rollback for SDO writes. `SETUP_RESET`, close, or an
allocation failure only frees kernel metadata; it cannot undo writes already
accepted by a slave. A `copy_to_user()` failure after apply may prevent the
caller from receiving results even though physical writes occurred.

`SDO_UPLOAD` is a bounded blocking diagnostic read. The caller supplies slave
position, object index/subindex, and a maximum result length from 1 to 256
bytes. The result includes actual length, data, errno, and CoE abort code. It
does not retain an asynchronous request.

The eventual ordinary-startup parameter path will store ordered
`ecrt_slave_config_sdo()` data for non-PDO startup parameters with each
persistent slave configuration so EtherLab can replay it during PREOP
reconfiguration. PDO mapping and assignment must use
`ecrt_slave_config_pdos()`, as required by the target EtherLab API.

## Pending declarative configuration validation

The following operations build and validate kernel-owned pending metadata.
`CONFIG_APPLY` subsequently constructs EtherLab configuration objects but does
not activate the master:

```text
ELC_IOC_CONFIG_BEGIN
ELC_IOC_CONFIG_ADD_SLAVE
ELC_IOC_CONFIG_ADD_SYNC
ELC_IOC_CONFIG_ADD_PDO
ELC_IOC_CONFIG_ADD_ENTRY
ELC_IOC_CONFIG_VALIDATE
ELC_IOC_CONFIG_APPLY
```

Each object has a nonzero configuration ID. Child objects reference their
parent by ID, and process-data entries also carry a nonzero stable
user-supplied `entry_id`. Validation rejects missing parents, duplicate IDs,
duplicate slave addresses, duplicate Sync Manager indices within a slave,
duplicate PDO indices within a Sync Manager, and duplicate entry IDs or object
references within a PDO.

The current conservative limits are 256 slaves, 1024 Sync Managers, 4096 PDOs,
and 16384 entries. A successful validation freezes the pending transaction
against further additions.

API 0.5 adds bounded per-slave distributed-clock records and one optional
master reference policy:

```text
ELC_IOC_CONFIG_ADD_DC
ELC_IOC_CONFIG_SET_DC_POLICY
```

Each DC record references exactly one configured slave and supplies
`assign_activate`, SYNC0/SYNC1 cycles, and signed shifts. The current
validation requires nonzero `assign_activate` and SYNC0 cycle values, rejects
duplicate DC records for one slave, and limits records to the configured slave
limit. Reference policy is disabled by default and can instead request
automatic EtherLab selection or an explicit slave configuration ID. An
explicit reference must itself have a DC record.

Apply calls `ecrt_slave_config_dc()` for each record and selects the requested
reference before activation. These are idle, blocking configuration calls. At
activation, every configured SYNC0 period must equal the requested application
cycle period.

`CONFIG_APPLY` constructs persistent EtherLab slave configurations and copies
the submitted hierarchy through the granular Sync Manager, PDO-assignment, and
PDO-mapping calls. It preserves submission order and reports the object kind
and configuration ID at which construction failed. It does not create a
domain, register entries, activate the master, or communicate the configuration
to a slave.

EtherLab has no public operation for removing one partially constructed slave
configuration. Consequently, an apply failure poisons the current control
session and requires close/reopen; releasing the master provides the
transaction rollback. A successful apply also prevents another
`CONFIG_BEGIN` or apply in the same session.

The target EtherLab matching API accepts alias, position, vendor ID, and product
code, but not revision. API 0.3 therefore rejects a nonzero revision constraint
instead of silently failing to enforce it. A future explicit revision policy
must preserve absent-at-startup configuration.

Revision still matters in **user space**: ESI/IOD recipes often key PDO and
CoE layouts by revision. The kernel does not compare the submitted map to the
live bus. A wrong recipe (for example two EL5152 instances with the same
product code but different revisions, programmed with one shared map) can pass
validate/apply and still fail at slave programming with EtherLab fixed-map
warnings and incomplete OP/WC. Controllers must submit the map that matches
each physical instance; see the developer guide.

`ELC_IOC_DOMAIN_CREATE` creates the configured EtherLab domain set (or one
implicit compatibility domain when none were declared). API 0.11 permits
mandatory PDO padding only as `entry_id=0, index=0, subindex=0`; padding keeps
its submitted bit length and position in the EtherLab mapping but is not
registered as user process data. Real entries require nonzero object indexes
and stable IDs. Position-based registration preserves the exact Sync
Manager/PDO/entry hierarchy even when object indices are repeated.
Registration failure poisons the session and requires close/reopen for the
same rollback reason as configuration apply.

After successful registration, `ELC_IOC_GET_ENTRY_OFFSET` resolves a stable
user-supplied `entry_id` to its global process-image byte offset, bit position,
and bit length. `global_offset` is the preferred member name.
`domain_offset` remains an ABI-neutral deprecated alias in the same union for
source compatibility. Unknown IDs return `ENOENT`. Domain creation does not
activate the master, obtain the process-data pointer, or send traffic.

## API 0.12 multi-domain configuration

API 0.12 adds `ELC_IOC_CONFIG_ADD_DOMAIN`,
`ELC_IOC_CONFIG_ASSIGN_DOMAIN`, and `ELC_IOC_GET_DOMAIN_STATUS`.
Domain IDs are stable, nonzero
configuration IDs. In explicit mode every configured slave must resolve to
exactly one declared domain; duplicate IDs, duplicate assignments, missing
assignments, and references to unknown domains or slaves are errors.

Domain declaration order defines the order of contiguous segments in the
single copied process image. `ELC_IOC_GET_ENTRY_OFFSET` returns a global
byte/bit offset, calculated from the assigned domain's segment base plus
the EtherLab-local entry offset. The kernel does not infer domains from vendor,
product, position, PDO layout, or online state.

For compatibility, a configuration containing no domain declarations uses one
implicit domain containing all configured slaves. Mixing implicit and explicit
assignment is invalid. This preserves existing API 0.11 configuration files
and tools without making inferred grouping part of the new policy.

Each domain status exposes its global segment base and size, WC and WC state,
current health faults, validity, cycle/input sequence, and that domain's own
output arm/re-arm state (API 0.17). Per-slave bus `data_valid` follows the
assigned domain WC (plus a published input sequence); `online` /
`operational` remain separate. Aggregate `GET_IO_STATUS` remains available
and reports any-armed / any-rearm across domain authorities; master/link and
full-bus counts stay global.

API 0.17 places an independent output authority on each configured domain.
Publication may target the full global image (`domain_config_id = 0`) or one
domain segment (non-zero id, size must match the segment). Arm and disarm use
`flags = 0` for all domains or a non-zero `domain_config_id` for one domain.
Master/link faults still disarm every authority. Domain health uses a WC
firewall: complete WC keeps a domain valid with no interruption when another
domain fails (power loss, cable damage, module failure); incomplete WC fails
only that domain. Capability bit `ELC_CAP_DOMAIN_OUTPUT_AUTHORITY` is set when
this model is present.

## Initial cyclic lifecycle

API 0.4 adds:

```text
ELC_IOC_CYCLE_ACTIVATE
ELC_IOC_CYCLE_GET_STATUS
ELC_IOC_CYCLE_DEACTIVATE
```

The caller selects `cycle_period_ns` on every activation within the published
hard bounds. It is immutable while active; changing rate requires deactivation
and another activation. Configured DC SYNC0 periods must exactly match it.
This controls the transport's cyclic timer. It does not claim to configure a
separate EtherLab operation-FSM interval when the installed EtherLab kernel
module lacks its declared `ecrt_master_set_send_interval()` symbol.

API 0.5 additionally provides `ELC_IOC_CYCLE_GET_DC_STATUS` without changing
the API 0.4 cycle-status structure. The DC snapshot reports enable/reference/
monitor state, the last reference result and phase difference, bounded cycle
adjustment, last/maximum synchrony deviation, reference read errors and
resumptions, and monitor results/timeouts.

API 0.13 adds `ELC_IOC_CYCLE_GET_INFO` and `ELC_IOC_CYCLE_WAIT`.
`GET_INFO` returns one coherent record for the most recently completed cycle.
`WAIT` takes the active configuration generation, the last cycle index seen,
and a timeout from 1 through 60,000 ms. It sleeps interruptibly until a
different completed cycle is published. Timeout returns `ETIMEDOUT`, a signal
returns the normal restart/interruption errno, stale generation returns
`ESTALE`, and deactivation wakes the waiter with `ESHUTDOWN`.

Cycle indices start at one and increase once per completed cyclic iteration.
They reset on activation; the associated nonzero configuration generation
disambiguates different activations. `scheduled_time_ns` is the absolute
deadline passed to the high-resolution scheduler before that iteration.
`actual_wake_time_ns` is sampled from `ktime_get_ns()` immediately after the
thread wakes. Both therefore use the kernel monotonic clock.
`wake_lateness_ns` is exactly actual minus scheduled and may be negative.

`input_sequence` is the coherent image generation published by that cycle.
The copied input snapshot now stores the exact cycle index alongside each
buffer, so its `cycle_count` identifies the cycle that produced those bytes
rather than whichever cycle happened to be current during the ioctl.

`output_sequence_consumed` means the user-space output generation selected by
the kernel before queue/send in that cycle. It is zero when the health/arm gate
selected the zero image. It does not claim network acknowledgement.
`stale_output_cycles` increments when an armed, healthy cycle selects the same
nonzero generation as the preceding cycle. Disarmed zero-image cycling does
not count as stale output. `missed_deadlines` increments when wake lateness is
at least one configured period. The record also associates WC state, health,
arm state, and aggregate cycle result with that cycle.

The cycle record is published under a dedicated short spinlock after
`ecrt_master_send()` returns. Waiters observe a separate published sequence
only after that coherent record is complete. The cyclic task neither waits for
nor calls user space.

API 0.16 adds `ELC_IOC_CYCLE_GET_DC_INFO` with structure
`elc_cycle_dc_info`. It extends the coherent cycle record with the
Distributed Clocks motion-clock contract: the exact application time sent
for that cycle, reference-clock validity and low-32-bit sample, normalized
phase difference, and the total adjustment applied. All DC fields are
published under the same `cycle_info_lock` as the base timing record,
ensuring one atomic snapshot per cycle. DC fields are zero when the
configuration contains no DC records. `dc_enabled` distinguishes the
non-DC case from a zero-valued DC field. The new ioctl preserves the
existing `ELC_IOC_CYCLE_GET_INFO` size and semantics unchanged.

API 0.6 adds `ELC_IOC_GET_IO_STATUS`. Each successfully validated
configuration receives a nonzero monotonically increasing generation for the
current module lifetime. While cycling, the status reports master link and
responding-slave count, online/operational configured-slave counts, domain
health, current and last-latched fault masks, and a fault transition count.
`last_latched_faults` is the union of every cause observed in the current
re-arm epoch. A successful arm ends that epoch; the next fault transition
replaces the old mask. `fault_count` separately counts physical-health fault
transitions and controller-lease expiries.

The aggregate bus becomes healthy only when the link is up, every configured
slave is online and operational, and every domain working counter is complete.
After health
has first been reached, a transition to unhealthy latches `rearm_required`.
API 0.7 adds `ELC_IOC_GET_INPUT_SNAPSHOT`. The name describes the
user-space direction: it is a read-only snapshot of the complete EtherLab
domain layout, including both input and output regions. The request contains a
user pointer and capacity. On success it returns the exact data size,
configuration generation, published input sequence, and associated cycle
count. A capacity smaller than the domain returns `ENOSPC` after reporting the
required size. Unsupported flags return `EINVAL`, and snapshots are unavailable
while inactive.

Activation rejects domains larger than `ELC_PROCESS_IMAGE_MAX` (64 KiB).
Two zeroed buffers are allocated before EtherLab activation. The cyclic thread
copies processed domain data into the inactive buffer and publishes it under a
spinlock. A process-context reader reserves the current buffer during
`copy_to_user`; the cyclic thread skips a publication rather than waiting for
or overwriting that reader. Deactivation joins the cyclic thread before freeing
the buffers.

API 0.8 adds `ELC_IOC_PUBLISH_OUTPUT`. The caller supplies data and per-bit
update-mask arrays plus the exact active configuration generation. API 0.17
extends the request with `domain_config_id`: zero means the full global image
(size must match the concatenated image); non-zero targets that domain
segment (size must match the segment). A stale generation returns `ESTALE`; a
size mismatch returns `EMSGSIZE`; unsupported flags return `EINVAL`; an
unknown domain id returns `ENOENT`.

The kernel copies into each targeted authority's inactive preallocated output
buffer, intersects the caller mask with the topology-derived output mask, and
merges only those bits with the previous published shadow. It then atomically
publishes the buffer and increments that authority's `output_sequence`. The
returned sequence (max when fanning out) is visible through
`ELC_IOC_GET_IO_STATUS`.

Publication is intentionally not activation. Without a successful arm,
`outputs_armed` remains false for each authority, and the cyclic thread
clears topology-derived output bits for disarmed authorities before every
queue/send.

API 0.9 adds `ELC_IOC_ARM_OUTPUTS` and `ELC_IOC_DISARM_OUTPUTS`. API 0.17
uses `flags = 0` for all domains or non-zero `flags` as `domain_config_id`.
Arm requires, for each targeted authority:

- an active cycle, healthy master/link, and healthy domain authority;
- the exact active configuration generation;
- the exact latest nonzero output-publication sequence for that authority;
- after a fault or manual disarm on that authority, a sequence newer than the
  sequence recorded at that epoch.

Generation or sequence mismatch returns `ESTALE`; an unhealthy target domain
or not-new-enough recovery publication returns `EAGAIN`; an unknown domain id
returns `ENOENT`. A successful arm clears that authority's `rearm_required`.
Each cycle applies a domain's retained output shadow only when that authority
is healthy and armed; otherwise it clears that domain's topology-derived
output bits.

Disarm immediately closes the selected authority gates, latches
`rearm_required`, records the current publication sequence, and waits for a
bounded acknowledgement made only after the cyclic thread has queued and sent
the zero-gated image for those authorities. Deactivation and close perform the
same handshake for every authority before stopping the cyclic thread. Timeout
returns `ETIMEDOUT` but leaves the gates disarmed.

API 0.14 adds `ELC_IOC_CONFIGURE_OUTPUT_LEASE`,
`ELC_IOC_RENEW_OUTPUT_LEASE`, and
`ELC_IOC_GET_OUTPUT_LEASE_STATUS`. API 0.18 extends the hang-failsafe model
for multi-ms userspace controllers (capability
`ELC_CAP_OUTPUT_LEASE_PUBLISH_RENEW`).

Lease configuration is permitted after domain creation (including while the
cycle is active in 0.18) and is bound to the exact configuration generation.
`flags` is 0 for all domains or a non-zero `domain_config_id` for one domain.

Budget selection:

- `timeout_ms` (0.18): wall-time hang; converted to bus cycles as
  `max(1, timeout_ms * 1e6 / cycle_period_ns)`, clamped to
  `ELC_OUTPUT_LEASE_CYCLES_MAX`. Pre-activate use requires an explicit
  `cycle_budget` when the period is not yet known.
- `cycle_budget`: explicit cycle count, or a max when combined with
  `timeout_ms`. Both zero disables the lease on the target domain(s).
- Recommended plant values: `timeout_ms` 500–2000 (longer than worst
  legitimate mailbox stall, short enough for hang failsafe).

Configure **seeds** `remaining` to the resolved budget (arm no longer requires
a prior renew). While armed, the cyclic task decrements remaining once per
armed selection; at zero it expires (disarm, zero domain PDO image,
`ELC_IO_FAULT_CONTROLLER_STALE`, `rearm_required`). Remaining does not
decrease while disarmed.

**Publish renew (0.18):** a successful `ELC_IOC_PUBLISH_OUTPUT` that updates a
leased authority refills remaining (and clears the current stale bit).
Successful **arm** also refills. Explicit `RENEW` remains supported; a no-op
renew when already full does not bump `renewal_count`. Userspace that only
publishes at its ecat rate needs no separate high-rate renew ioctl.

Recovery after expiry: a publication newer than the fault sequence and an
explicit arm (publish itself refills the budget). Orderly disarm,
deactivation, and close are not lease expiries.

API 0.10 adds `ELC_IOC_GET_CONFIG_SLAVE_STATUS`, keyed by the stable
user-supplied slave `config_id` and exact configuration generation. A stale
generation returns `ESTALE`; an unknown ID returns `ENOENT`.

The result reports active, online, operational, AL state, the EtherLab
slave-state call result, cycle count, input sequence, and `data_valid`.
Bus `data_valid` requires an active transport, a published input snapshot, and
a complete assigned-domain working counter. `online` / `operational` are
reported separately so application policy can still see PS rails and AL state
without clearing domain-bus validity during EtherLab re-scan while WC remains
complete. EtherLab does not expose per-entry WC; API 0.12 independent domains
plus the API 0.17 WC firewall isolate domains under power loss and similar
segment failures.

Activation requires an applied configuration and registered domain set. The caller
supplies a cycle period from 100 microseconds through one second; flags must be
zero. The kernel activates EtherLab, obtains the internal domain memory,
zeroes every domain image before the first application send, and starts one
kernel thread. The thread receives once, processes every domain, assembles the
global copied snapshot, queues every domain, and sends once.

Status returns active state, configured period, domain size, working counter
and interpretation, completed cycles, cycle API errors, full-period overruns,
maximum observed lateness, and the last aggregate cycle result. Any positive
scheduler lateness contributes to the maximum, but only lateness of at least
one full period is an overrun. There is no normal-cycle logging, allocation,
blocking mailbox operation, or user-space callback.

### `ELC_IOC_CYCLE_SET_PERIOD`

API 0.15 permits a controller to activate conservatively, wait for its required
slaves and domains to reach OP/valid, and then change the cyclic period without
rebuilding the full EtherLab configuration. The caller supplies the active
configuration generation and a period within the activation limits. Outputs
must be disarmed. An armed session returns `EBUSY`; a stale generation returns
`ESTALE`.

DC sessions are allowed while disarmed. At the completed-cycle boundary the
kernel updates the host cycle period, rewrites each configured SYNC0 that
matched the previous period to the new period via `ecrt_slave_config_dc()`,
and resets the DC filter/adjustment so phase control re-locks. Host
application-time stepping uses the new period immediately. EtherLab programs
ESC SYNC0 registers during slave configuration; the stored `slave_config` is
updated at the boundary, but a full hardware SYNC0 rewrite may wait until the
next slave reconfiguration pass. Controllers that need a guaranteed ESC SYNC0
change should deactivate and reactivate with the new period.

The cyclic thread uses one immutable period for each complete
receive/process/application-time/queue/send cycle. It publishes that cycle's
coherent timing record, then installs a pending period for the next deadline.
The ioctl waits for this boundary and returns `applied_period_ns` and
`effective_after_cycle`; the new period governs the cycle following that
number. This is a scheduling-rate change, not a promise that slaves will
remain operational at the requested rate. User space must continue monitoring
per-slave/domain status and timing.

Application time is initialized to monotonic time rounded to the configured
period before activation and advanced once per cycle before queue/send. This
prevents EtherLab from activating without application time and establishes the
required DC initialization ordering.

When DC is configured, the receive half reads and normalizes the reference
phase after domain processing, updates IOD's 1024-sample bounded controller,
and processes a pending synchrony monitor. The send half advances corrected
application time, calls `ecrt_master_sync_slave_clocks()`, queues a monitor
approximately once per second, then queues the domain and sends. Reference read
failure is tracked separately and does not stop cyclic exchange.

Deactivation first stops and joins the cycle thread, then calls
`ecrt_master_deactivate()`. Before deactivation it waits one bounded cycle
period and consumes/processes the response to the final send. Exact EtherLab
1.6.9 source inspection confirms that deactivation destroys all domains and
slave configurations. The kernel therefore invalidates applied EtherLab
pointers and entry offsets while retaining validated user metadata; the caller
must apply and create the domain again before another activation. Close
performs the same synchronous stop before releasing master 0.

After EtherLab deactivation, the call polls the configured physical slave
positions for up to five seconds and returns only after none remains in
SAFEOP/OP. An absent configured slave is already settled for this purpose.
Timeout or discovery error poisons the session against re-apply; close/reopen
is required. This prevents immediate reuse from racing EtherLab's asynchronous
idle-state-machine transition, but cannot keep output datagrams flowing during
that transition.

The API 0.4 increment intentionally had no process-image writer and could not
command motion. Distributed clocks and copied exchange were added by later
minor versions as documented above.

The target DKMS `Module.symvers` does not export
`ecrt_master_set_send_interval()`, although EtherLab declares it. This external
module cannot provide that optional hint and uses the validated period only
for its cyclic schedule.

## Standalone declarative file format

`tools/elc_config` provides a temporary dependency-free text format for
testing this UAPI independently of Clockwork:

```text
slave CONFIG_ID ALIAS POSITION VENDOR_ID PRODUCT_CODE REVISION
sync CONFIG_ID SLAVE_CONFIG_ID SYNC_INDEX DIRECTION WATCHDOG
pdo CONFIG_ID SYNC_CONFIG_ID PDO_INDEX
entry CONFIG_ID PDO_CONFIG_ID ENTRY_ID INDEX SUBINDEX BIT_LENGTH
```

Numbers accept C base notation, including hexadecimal. Directions are `input`
or `output`; watchdog values are `default`, `enable`, or `disable`. The tool
first parses and bounds the complete file, then submits it in file order.
`prepare` stops after domain registration and closes the device.

`cycle CONFIG PERIOD_NS DURATION_SECONDS [DEVICE]` performs the same
preparation, activates the cyclic pump for a bounded duration, prints status,
publishes a shadow while leaving outputs disarmed, and synchronously
deactivates. `cycle-zero-arm` instead publishes an all-zero shadow and tests
exact-sequence arm, synchronous disarm, stale-sequence rejection, and fresh
zero-sequence recovery. `cycle-zero-hold` waits for a healthy bus, arms an
all-zero shadow, emits a flushed readiness marker, and holds it for
controller-death testing. `cycle-monitor` observes disarmed per-slave recovery
transitions. `cycle-abi` keeps outputs disarmed while checking active-state
size, flag, generation, sequence, and lifecycle rejection paths. Closing the
descriptor is also a kernel-enforced cleanup path if status or explicit
deactivation fails. All cycle commands
change EtherCAT slave PDO configuration during activation and are hardware
commissioning operations; neither requests a nonzero transmitted output.
`cycle-rate CONFIG START_PERIOD_NS TARGET_PERIOD_NS DURATION_SECONDS [DEVICE]`
uses the strict health gate at the start period, requests the acknowledged
boundary change, and only then emits the timed-interval readiness marker.
`cycle-exchange-rate` adds a continuous user-space loop: it waits for each
published cycle record, copies the latest complete input image, and publishes
an all-zero output image while remaining disarmed. It reports skipped cycle
notifications and fixed-histogram kernel-wake/user-observation latency
statistics. This distinguishes kernel bus-cycle capacity from the rate at
which a particular user process actually observes every cycle.

`elc_sdo stage FILE` parses and submits an ordered SDO batch to the pending
kernel transaction, then closes without applying it. It is intended for
validation and allocation/unwind tests and performs no SDO download.
