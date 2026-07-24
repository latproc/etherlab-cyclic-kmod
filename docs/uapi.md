# User-Space API

## Status

The current experimental API is version 0.10. It supports discovery, a
provisional bounded commissioning SDO batch, transactional
slave/Sync/PDO/entry/DC configuration, domain registration, configurable
cyclic pumping, copied process images, distributed clocks, health/fault
status, explicit output arm/disarm, and per-configured-slave validity.

## Ownership and lifecycle

The module registers `/dev/cw_ethercat0` without claiming EtherLab master 0.
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

The shared header is `include/cw_ethercat_uapi.h`. Structures use fixed-width
Linux UAPI types, contain no pointers, and have fixed layouts suitable for the
compat ioctl path.

The tool first calls `CW_EC_IOC_GET_API_VERSION`. Major versions must match.
Minor versions add:

- 0.2: provisional ad-hoc setup-SDO batch;
- 0.3: declarative configuration and stable entry offsets;
- 0.4: bounded activation, cycle status, and synchronous deactivation;
- 0.5: distributed-clock configuration, synchronization, and status;
- 0.6: generation-bound health, fault, and re-arm status;
- 0.7: coherent copied input snapshots;
- 0.8: masked copied output publication while hard-disarmed;
- 0.9: generation/sequence-bound arm and synchronous disarm; and
- 0.10: stable-ID per-configured-slave state and conservative data validity.

Input/output structures that accept caller fields include `struct_size` and
`api_major`. The kernel rejects an unexpected size with `EINVAL` and an
incompatible major version with `EPROTONOSUPPORT`.

## Operations

### `CW_EC_IOC_GET_API_VERSION`

Returns `struct cw_ec_api_version`.

### `CW_EC_IOC_GET_MASTER_INFO`

Returns `struct cw_ec_master_info` containing:

- scanned slave count;
- main link state;
- scan-in-progress state;
- EtherLab application time.

### `CW_EC_IOC_GET_SLAVE_INFO`

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

Unknown ioctl types or commands return `ENOTTY`. The initial malformed-call
test covers a second control open, unknown command, short structure,
incompatible API major, and invalid slave position.

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
CW_EC_IOC_SETUP_BEGIN
CW_EC_IOC_SETUP_ADD_SDO
CW_EC_IOC_SETUP_APPLY
CW_EC_IOC_SETUP_RESET
CW_EC_IOC_SDO_UPLOAD
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
CW_EC_IOC_CONFIG_BEGIN
CW_EC_IOC_CONFIG_ADD_SLAVE
CW_EC_IOC_CONFIG_ADD_SYNC
CW_EC_IOC_CONFIG_ADD_PDO
CW_EC_IOC_CONFIG_ADD_ENTRY
CW_EC_IOC_CONFIG_VALIDATE
CW_EC_IOC_CONFIG_APPLY
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
CW_EC_IOC_CONFIG_ADD_DC
CW_EC_IOC_CONFIG_SET_DC_POLICY
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

`CW_EC_IOC_DOMAIN_CREATE` creates one EtherLab domain and registers every
submitted entry with `ecrt_slave_config_reg_pdo_entry_pos()`. Position-based
registration preserves the exact Sync Manager/PDO/entry hierarchy even when
object indices are repeated. Registration failure poisons the session and
requires close/reopen for the same rollback reason as configuration apply.

After successful registration, `CW_EC_IOC_GET_ENTRY_OFFSET` resolves a stable
user-supplied `entry_id` to its domain byte offset, bit position, and bit
length. Unknown IDs return `ENOENT`. Domain creation does not activate the
master, obtain the process-data pointer, or send traffic.

## Initial cyclic lifecycle

API 0.4 adds:

```text
CW_EC_IOC_CYCLE_ACTIVATE
CW_EC_IOC_CYCLE_GET_STATUS
CW_EC_IOC_CYCLE_DEACTIVATE
```

API 0.5 additionally provides `CW_EC_IOC_CYCLE_GET_DC_STATUS` without changing
the API 0.4 cycle-status structure. The DC snapshot reports enable/reference/
monitor state, the last reference result and phase difference, bounded cycle
adjustment, last/maximum synchrony deviation, reference read errors and
resumptions, and monitor results/timeouts.

API 0.6 adds `CW_EC_IOC_GET_IO_STATUS`. Each successfully validated
configuration receives a nonzero monotonically increasing generation for the
current module lifetime. While cycling, the status reports master link and
responding-slave count, online/operational configured-slave counts, domain
health, current and last-latched fault masks, and a fault transition count.

The bus becomes healthy only when the link is up, every configured slave is
online and operational, and domain working counter is complete. After health
has first been reached, a transition to unhealthy latches `rearm_required`.
API 0.7 adds `CW_EC_IOC_GET_INPUT_SNAPSHOT`. The name describes the
user-space direction: it is a read-only snapshot of the complete EtherLab
domain layout, including both input and output regions. The request contains a
user pointer and capacity. On success it returns the exact data size,
configuration generation, published input sequence, and associated cycle
count. A capacity smaller than the domain returns `ENOSPC` after reporting the
required size. Unsupported flags return `EINVAL`, and snapshots are unavailable
while inactive.

Activation rejects domains larger than `CW_EC_PROCESS_IMAGE_MAX` (64 KiB).
Two zeroed buffers are allocated before EtherLab activation. The cyclic thread
copies processed domain data into the inactive buffer and publishes it under a
spinlock. A process-context reader reserves the current buffer during
`copy_to_user`; the cyclic thread skips a publication rather than waiting for
or overwriting that reader. Deactivation joins the cyclic thread before freeing
the buffers.

API 0.8 adds `CW_EC_IOC_PUBLISH_OUTPUT`. The caller supplies complete
domain-sized data and per-bit update-mask arrays plus the exact active
configuration generation. A stale generation returns `ESTALE`; a size other
than the active domain returns `EMSGSIZE`; unsupported flags/reserved fields
return `EINVAL`.

The kernel copies into the inactive preallocated output buffer, intersects the
caller mask with the topology-derived output mask, and merges only those bits
with the previous published shadow. It then atomically publishes the buffer
and increments `output_sequence`. The returned sequence is also visible through
`CW_EC_IOC_GET_IO_STATUS`.

Publication is intentionally not activation. API 0.8 has no arm operation,
`outputs_armed` remains false, and the cyclic thread clears every configured
output bit in domain memory before every queue/send. This makes publication and
masking independently testable without transmitting the requested values.

API 0.9 adds `CW_EC_IOC_ARM_OUTPUTS` and `CW_EC_IOC_DISARM_OUTPUTS`. Arm
requires:

- an active cycle and healthy bus;
- the exact active configuration generation;
- the exact latest nonzero output-publication sequence;
- after a fault or manual disarm, a sequence newer than the sequence recorded
  at that epoch.

Generation or sequence mismatch returns `ESTALE`; an unhealthy bus or
not-new-enough recovery publication returns `EAGAIN`. A successful arm clears
`rearm_required`. Each cycle applies the retained output shadow only when both
health and arm are true; otherwise it clears all topology-derived output bits.

Disarm immediately closes the atomic output gate, latches
`rearm_required`, records the current publication sequence, and waits for a
bounded acknowledgement made only after the cyclic thread has queued and sent
the zero-gated image. Deactivation performs the same handshake before stopping
the cyclic thread. This prevents a successful disarm ioctl or orderly
deactivation from leaving a previously selected shadow as the last application
datagram. Timeout returns `ETIMEDOUT` but leaves the gate disarmed.

API 0.10 adds `CW_EC_IOC_GET_CONFIG_SLAVE_STATUS`, keyed by the stable
user-supplied slave `config_id` and exact configuration generation. A stale
generation returns `ESTALE`; an unknown ID returns `ENOENT`.

The result reports active, online, operational, AL state, the EtherLab
slave-state call result, cycle count, input sequence, and `data_valid`.
Validity is deliberately conservative: the transport must be active, the
slave-state call must succeed, that configured slave must be online and
operational, at least one input snapshot must have been published, and the
whole domain working counter must be complete. EtherLab does not expose
per-entry working-counter validity through this configuration-state API, so
API 0.10 does not claim that an unaffected slave's bytes are fresh while the
shared domain is incomplete.

Activation requires an applied configuration and registered domain. The caller
supplies a cycle period from 100 microseconds through one second; flags must be
zero. The kernel activates EtherLab, obtains the internal domain memory,
zeroes the complete image before the first application send, and starts one
kernel thread. The thread calls receive, domain process, domain queue, and send
in that order.

Status returns active state, configured period, domain size, working counter
and interpretation, completed cycles, cycle API errors, full-period overruns,
maximum observed lateness, and the last aggregate cycle result. Any positive
scheduler lateness contributes to the maximum, but only lateness of at least
one full period is an overrun. There is no normal-cycle logging, allocation,
blocking mailbox operation, or user-space callback.

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

`tools/cw_ec_config` provides a temporary dependency-free text format for
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
controller-death testing. Closing the descriptor is also a kernel-enforced
cleanup path if status or explicit deactivation fails. All cycle commands
change EtherCAT slave PDO configuration during activation and are hardware
commissioning operations; neither requests a nonzero transmitted output.

`cw_ec_sdo stage FILE` parses and submits an ordered SDO batch to the pending
kernel transaction, then closes without applying it. It is intended for
validation and allocation/unwind tests and performs no SDO download.
