# Architecture

## Current Phase 2 boundary

```text
cw_ec_bus
    |
    | fixed-width, versioned ioctl structures
    v
/dev/cw_ethercat0
    |
    | one exclusive control open
    v
cw_ethercat.ko
    |
    | ecrt_request_master / ecrt_master /
    | ecrt_master_get_slave / ecrt_release_master
    v
EtherLab master 0
```

Loading `cw_ethercat.ko` registers the misc character device but does not claim
the EtherLab master. Opening the device claims master 0. Closing the controller
file releases it. This preserves EtherLab CLI and direct-IOD access whenever no
kernel-transport controller is attached.

One process may control the device. The module's file-operation owner prevents
normal unload while an fd remains open. No persistent configuration or
asynchronous object exists in this phase.

## Ownership

The per-open `cw_ec_file` object owns the acquired `ec_master_t` reference.
Construction is:

```text
reserve exclusive-open token
allocate zeroed file context
request EtherLab master
attach context to file
```

Every failure frees the context, releases the token, and returns an error.
Close releases the EtherLab master, clears the pointer, frees the context, and
releases the token.

The module-global misc device is registered during module initialization and
deregistered during module exit. An open fd holds the module reference through
`file_operations.owner`.

## Locking

An atomic compare/exchange protects the single-controller admission decision.
After a successful open, the file context and its master pointer are private to
that file. Ioctls are currently expected from one controlling process; no
configuration or cyclic thread exists.

EtherLab provides its own blocking `master_sem` protection for
`ecrt_master_get_slave()`. None of these calls occur in real-time context.

## State model

The implemented state is deliberately small:

```text
MODULE_UNLOADED
    |
    v
DEVICE_AVAILABLE -- open/claim succeeds --> CONTROL_OPEN
    ^                                      |
    |-------------- close/release ---------|
```

An open while IOD/direct libethercat owns master 0 returns `EBUSY`. A second
control open also returns `EBUSY`.

Future configuration and running states will extend this model only after their
ownership and teardown rules are documented.

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
