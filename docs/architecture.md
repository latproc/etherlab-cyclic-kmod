# Architecture

## Current API 0.11 boundary

```text
standalone controller
    |
    | fixed-width versioned ioctls
    v
/dev/cw_ethercat0
    |
    | one exclusive control open
    v
cw_ethercat.ko
    |
    | validated configuration, domain, cyclic/DC thread,
    | copied images, health and output gate
    v
EtherLab master 0
```

Loading `cw_ethercat.ko` registers the misc character device but does not claim
the EtherLab master. Opening the device claims master 0. Closing the controller
file releases it. This preserves EtherLab CLI and direct-IOD access whenever no
kernel-transport controller is attached.

One process may control the device. The module's file-operation owner prevents
normal unload while an fd remains open. The per-open context owns all pending
metadata, EtherLab configuration/domain objects, copied image buffers, masks,
the cyclic thread, and its wait queues.

## Ownership

The per-open `cw_ec_file` object owns the acquired `ec_master_t` reference.
Construction begins:

```text
reserve exclusive-open token
allocate zeroed file context
request EtherLab master
attach context to file
```

Every failure frees the context, releases the token, and returns an error.
Close first closes the output gate, waits for a zero-gated send, joins the
cyclic thread, deactivates EtherLab, invalidates EtherLab-owned pointers, frees
all pending metadata and copied buffers, releases the master, and releases the
exclusive token.

The module-global misc device is registered during module initialization and
deregistered during module exit. An open fd holds the module reference through
`file_operations.owner`.

## Locking

An atomic compare/exchange protects the single-controller admission decision.
A mutex serializes process-context configuration, image publication, arm,
disarm, and teardown. Separate short spinlocks protect input/output buffer
selection and reader reservation. The cyclic thread never takes the process
mutex, allocates, performs mailbox operations, or waits for user space.

Input publication skips a buffer swap if a process reader still reserves the
inactive buffer. Output publication selects only a buffer not reserved by the
cyclic reader. Disarm uses a monotonically increasing gate request and a
waitqueue acknowledgement made after a successful EtherLab send.

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
Any health loss closes the output gate and latches a fresh-publication
requirement. Deactivation returns to close/release because EtherLab destroys
domains and slave configurations; reconfiguration currently requires a new
control session.

## Process image, domains, and DC

API 0.11 has one EtherLab domain and therefore one global WC/validity boundary.
That compatibility behavior remains available for existing configurations.
The next UAPI increment adds explicit user-defined domains; the kernel must
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
mode its conservative `data_valid` requires that slave online and operational,
a published input snapshot, and complete WC for that slave's assigned domain.
One incomplete switchable-drive domain must not invalidate an independently
complete always-powered-I/O domain.

The master still has one cyclic task and one datagram cadence. The cyclic order
is receive; process every domain; assemble the input snapshot; reference/DC
processing; evaluate per-domain health and output gates; apply outputs;
application-time/slave synchronization; queue every domain; and send once.
Application policy and object interpretation never enter the kernel.

Output safety state is ultimately per domain: health, current and latched
faults, arm state, re-arm requirement, and fresh-publication epoch. The first
multi-domain increment may retain the existing conservative all-domain output
control ioctls while the domain-specific control ABI is added, but it must not
claim independent output availability until those controls exist.

Detailed concurrency and recovery rules are in
`process-image-exchange.md`; DC behavior is in `distributed-clocks.md`.

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
