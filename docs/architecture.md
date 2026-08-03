# Architecture

## Current API 0.19 boundary

```text
standalone controller
    |
    | fixed-width versioned ioctls
    v
/dev/elc_ethercat0
    |
    | one exclusive control open
    v
elc_ethercat.ko
    |
    | validated configuration, domains, cyclic/DC thread,
    | copied images, per-domain health and output authorities,
    | optional setup-hold (PREOP/SAFEOP inhibit while cycling)
    v
EtherLab master 0
```

Loading `elc_ethercat.ko` registers the misc character device but does not claim
the EtherLab master. Opening the device claims master 0. Closing the controller
file releases it. This preserves EtherLab CLI and direct-IOD access whenever no
kernel-transport controller is attached.

One process may control the device. The module's file-operation owner prevents
normal unload while an fd remains open. The per-open context owns all pending
metadata, EtherLab configuration/domain objects, copied image buffers, masks,
the cyclic thread, and its wait queues.

## Ownership

The per-open `elc_file` object owns the acquired `ec_master_t` reference.
Construction begins:

```text
reserve exclusive-open token
allocate zeroed file context
request EtherLab master
attach context to file
```

Every failure frees the context, releases the token, and returns an error.
Close first closes every domain output authority, waits for a zero-gated send,
joins the cyclic thread, deactivates EtherLab, invalidates EtherLab-owned
pointers, frees all pending metadata and copied buffers, releases the master,
and releases the exclusive token.

The module-global misc device is registered during module initialization and
deregistered during module exit. An open fd holds the module reference through
`file_operations.owner`.

## Locking

An atomic compare/exchange protects the single-controller admission decision.
A mutex serializes process-context configuration, image publication, arm,
disarm, and teardown. Separate short spinlocks protect input/output buffer
selection, reader reservation, and the coherent API 0.13 cycle record. The
cyclic thread never takes the process mutex, allocates, performs mailbox
operations, or waits for user space.

Input publication skips a buffer swap if a process reader still reserves the
inactive buffer. Output publication selects only a buffer not reserved by the
cyclic reader. Disarm uses a monotonically increasing gate request and a
waitqueue acknowledgement made after a successful EtherLab send.
Cycle notification uses a separate waitqueue and published-cycle sequence.
The sequence changes only after the complete cycle record is visible.

EtherLab provides its own blocking `master_sem` protection for
`ecrt_master_get_slave()`. None of these calls occur in real-time context.

## State model

The implemented high-level state is:

```text
MODULE_UNLOADED
    |
    v
DEVICE_AVAILABLE -- open/claim succeeds --> CONTROL_OPEN
                                             |
                                             v
                                  CONFIG_PENDING
                                             |
                                      validate/apply
                                             v
                                  DOMAIN_REGISTERED
                                             |
                                         activate
                                             v
                              RUNNING_DISARMED <---- fault/disarm
                                             |
                         publish exact generation/sequence
                                             |
                                            arm
                                             v
                                RUNNING_ARMED
```

An open while IOD/direct libethercat owns master 0 returns `EBUSY`. A second
control open also returns `EBUSY`. Configuration is immutable while active.
Master/link loss closes every domain output authority and latches a
fresh-publication requirement on each. A domain WC or assigned-slave fault
disarms only that domain's authority; other healthy domains may remain armed.
Deactivation returns to close/release because EtherLab destroys domains and
slave configurations; reconfiguration currently requires a new control session.

## Process image, domains, and DC

API 0.12 supports explicit user-defined domains. The API 0.11 single-domain
behavior remains available for existing configurations containing no domain
records. The kernel must
never infer grouping from vendor, product, topology, or device type.

A domain is an availability and data-validity boundary, not automatically a
slave boundary. The recommended machine layout is one domain for
always-powered Beckhoff I/O and another for switchable drives. Split drives
further only when independent axes need independent validity or output fault
containment. Every configured slave belongs to exactly one declared domain.

Domain declaration order defines stable, contiguous segments in the copied
user-space process image. Entry offsets remain global: an entry's returned
offset is its EtherLab domain-local offset plus its segment base. This retains
one bounded snapshot and one stable entry-ID namespace while allowing each
domain to report its own WC and validity.

Per-configured-slave status is keyed by stable configuration ID. In explicit
mode, bus `data_valid` follows the assigned domain's complete WC and a
published input snapshot. `online` / `operational` remain separate. One
incomplete switchable-drive domain must not interrupt an independently
complete always-powered I/O domain (domain bus firewall).

The master still has one cyclic task and one datagram cadence. The cyclic order
is receive; process every domain; assemble the input snapshot; reference/DC
processing; evaluate per-domain health and output gates; apply outputs;
application-time/slave synchronization; queue every domain; and send once.
Application policy and object interpretation never enter the kernel.

API 0.12 independently reports domain WC, validity, current faults, and image
segments. API 0.17 places an independent `elc_output_authority` on each
configured domain: publication buffers and masks, sequence, arm/re-arm,
latched fault epoch, lease, and per-domain health for output selection.
`domain_config_id` / arm-disarm `flags` of zero retain full-image and
all-domain compatibility; non-zero selects one domain. Aggregate IO status
reports any-armed / any-rearm across authorities. Domain status reports that
domain's own arm and re-arm fields. Master/link still gates every authority.
Domain WC isolates domain health: power loss, cable damage, or module failure
on one domain must not clear validity or gate outputs for domains that still
have complete WC.

Detailed concurrency and recovery rules are in
`process-image-exchange.md`; DC behavior is in `distributed-clocks.md`.

## API 0.13 transport timeline

The cyclic kernel task is the authoritative EtherCAT timing source. Each
activation starts cycle index one under a new configuration generation.
Cycle timestamps use monotonic `ktime_get_ns()`: scheduled time is the
absolute high-resolution wake deadline, and actual wake time is sampled
immediately after scheduling returns.

After receive/process, the task publishes the input image tagged with the
current cycle index, evaluates health, and selects either the current armed
output generation or the zero image. After DC/application time, every domain
is queued and the master send returns; only then is one coherent cycle record
published and waiters woken. The record identifies the input sequence and
selected output sequence but does not claim slave acknowledgement.

Repeated selection of the same nonzero output generation while armed counts as
stale reuse. Disarmed zero-image cycles do not. This latest-output path remains
separate from a future cycle-addressed scheduled-output queue. Capability
discovery reports only the timing and wait features that are implemented.

## API 0.14 / 0.17 output-authority lease

Output state is owned by a per-domain `elc_output_authority` (API 0.17). API
0.14 first exposed lease controls on the then-compatibility authority; the
same lease fields now live on each domain authority. Future delegated domain
fds are expected to reuse this boundary.

The lease is optional and measured in armed output selections rather than wall
time. User space configures the budget before activation and renews it while
active. Disarmed cycles do not consume the budget. Immediately before output
selection, the cyclic task either consumes one unit or, if none remains,
closes that authority's gate and selects zeros for its domain.

Expiry does not stop the common cyclic task or mark the physical bus
unhealthy. It is a distinct controller-stale output-authority fault. Inputs,
WC, DC, and unrelated future authorities remain observable. Renewal never
resurrects output; a fresh publication and explicit arm are still required.

## Future delegated domain connections

Explicit domains may later become user-space ownership boundaries as well as
WC and process-image boundaries. This does not change the single-master
architecture. One coordinator continues to own topology, activation, DC and
the common cyclic task; a delegated controller can receive a restricted file
descriptor for an immutable domain set.

```text
                         coordinator
                 configure / activate / teardown
                              |
                       one EtherLab master
                              |
                   one common cyclic timeline
                     /                    \
          Beckhoff domain fd          drive domain fd
          machine controller          motion controller
                                             ^
                                             |
                                   user-space planner IPC
```

The preferred interface is a kernel-created fd returned to the coordinator,
which can pass it to a controller with `SCM_RIGHTS`. The delegated fd exposes
only its authorized input segment, output mask, status, publication, lease and
arm state. It cannot configure EtherLab or address another segment. There is
at most one output writer per domain.

All connections observe the same global cycle identity and notification
timeline. Domain output generations can advance independently, but this does
not provide atomic cross-domain commits. A future group-commit facility would
need explicit cycle-addressed semantics.

### Cable-level bus splits and ring Ethernet (development requirement)

Software domain isolation assumes the physical segment for a healthy domain
still has a path to the master. On a **line** topology, a break or power loss
at a mid-chain device can black out every slave after the fault even if those
slaves are in a different software domain. Domain grouping should place
always-on I/O **before** switchable equipment in the physical order when
possible, but that is not enough for true cable-split survival.

When the project adds **extra domain ownership on extra user-space interfaces**
(Section 13C delegated domain fds), it shall **also** plan support for
**redundant Ethernet / loop (ring) master configuration**: traffic out one
Ethernet port and in on another so a single cable break or mid-bus failure
can be handled without losing the entire run. That work includes:

- documenting the target EtherLab multi-device / redundant-link model for the
  installed master version;
- UAPI or build-time configuration for primary and secondary NICs and ring
  policy;
- validation that domains on the surviving path keep complete WC and remain
  independently armable across a deliberate split;
- interaction with delegated domain fds (which controller sees which segment
  after a split).

Do not implement ring support ad hoc outside that design; it is a planned
companion to multi-client domain interfaces, not a silent kernel default.

Closing or expiring a delegated controller gates its domains while the common
cycle and unrelated authorities continue. Closing the coordinator gates all
outputs and performs the existing complete teardown. Until that ABI is
implemented, the exclusive control fd remains the compatibility owner of all
domains and output safety state remains global.

## Clockwork process-entry selectors

The flattened entry `pos` in `/tmp/ecat.log` is an enumeration artifact. For
example, the first EL2838 output currently appears as:

```text
pos = 0
index = 0x7000
subindex = 1
```

New Clockwork configuration should identify this entry by explicit object
identity `(0x7000, 1)`. The user-space backend resolves that identity within
the selected module's PDO hierarchy, assigns a stable kernel `entry_id`, and
uses the returned domain byte/bit offset.

Index alone is not guaranteed unique. A duplicated `(index, subindex)` must be
disambiguated with PDO index or occurrence; ambiguity is a configuration
error.

Legacy positional configuration remains supported for systems that have not
been converted. Selector mode must be explicit, not inferred from whether a
number looks like an ordinal or object index, and an invalid object selector
must never fall back to the same numeric value as `pos`.

Migration requires a standalone conversion tool that combines a Clockwork
configuration with a matching captured PDO description:

```text
Clockwork config + /tmp/ecat.log
              |
              v
       audit positional selectors
              |
       uniquely resolve each pos
              |
              v
  proposed index/subindex selectors
```

The converter defaults to dry-run, reports missing and ambiguous mappings,
does not rewrite partially resolved files, and preserves a backup when an
explicit write is requested. The original `pos` mode remains available as a
deliberate compatibility path.
