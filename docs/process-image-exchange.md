# Process Image Exchange

## Safety contract

The first user-space exchange API will copy shadows. It will not mmap or expose
EtherLab domain memory.

The ordered concatenation of all configured domain segments is the stable
global offset namespace. Kernel-owned
metadata derives an output bit mask from entries under output Sync Managers.
Each processed domain segment is copied into a coherent, read-only user-space
snapshot. The API name uses "input" for the user-space transfer direction; the
bytes retain the full EtherLab domain layout, including zero/disarmed output
regions.
Only masked output bits are copied from an explicitly published output shadow
into the EtherLab domain.

Initial outputs are zero and disarmed. Publishing an output image does not arm
it. User space must explicitly arm the exact configuration generation and a
fresh output sequence.

## Fault and recovery rule

The cyclic thread evaluates master link state, then per-domain:

- online and operational state of slaves assigned to that domain;
- complete working-counter state for that domain.

API 0.17 owns one `elc_output_authority` per configured domain. Master/link
loss disarms every authority. A domain WC or assigned-slave fault disarms only
that domain's authority and records a fault epoch on it; other healthy domains
may remain armed. Restored link/slaves do not restore an old armed shadow.
Re-arm of an affected authority requires an output update newer than its fault
epoch and an explicit arm while that authority is healthy.

This behavior is deterministic software containment, not a replacement for
hardware safety.

Earlier global-gate power-loss evidence (shared domain) still holds for the
latched re-arm rule. Separate-domain independence under live drive power loss
is the intended Monday commissioning check: I/O domain validity and arm must
not depend on drive-domain WC.

API 0.14 exposes each configured slave's online, operational, and AL state by
stable `config_id`. Its `data_valid` flag additionally requires a published
snapshot and complete WC for that slave's assigned domain. Snapshots remain
inspectable when another domain is incomplete. Aggregate IO status reports
global link/counts and any-armed / any-rearm; domain status reports each
authority's arm and re-arm fields.

When the optional output lease is enabled, its armed-cycle budget is part of
the domain's output authority. Expiry synchronously selects zeros for that
authority, disarms it, latches `ELC_IO_FAULT_CONTROLLER_STALE`, and requires a
renewal plus a publication newer than the fault epoch before explicit re-arm.
Renewal never arms outputs. A zero cycle budget preserves the compatibility
behavior without lease expiry.

## Copy concurrency

Read-only snapshots use two preallocated buffers. A process-context reader reserves
the active buffer briefly under a spinlock; the cyclic thread skips publishing
a new snapshot rather than overwriting a reserved buffer.

Each domain authority uses two preallocated segment-sized buffers and a short
pointer/state spinlock. A global publish (`domain_config_id = 0`) fans
segments into every authority; a domain-scoped publish writes only that
authority. The writer selects the inactive buffer, copies data and per-bit
update masks without holding the spinlock, intersects the mask with the
topology-derived output mask for that segment, and merges over the previous
shadow. The cyclic thread reads only each authority's published active buffer
at a cycle boundary. API 0.17 keeps arm, re-arm, sequence, lease, and health
on each authority independently.

Each published input buffer also stores the exact cycle index that produced
its bytes. `GET_INPUT_SNAPSHOT` returns that per-buffer index rather than a
separately sampled global counter. `CYCLE_GET_INFO` associates the same
cycle's input sequence with the output generation selected before queue/send.
The selected generation is zero when the output gate chooses the zero image.

## Generic masked-output format

Process data is represented as a full zero-based global image formed by
concatenating declared domain segments in declaration order. Output updates
carry a second equally sized byte array whose set bits select individual bits
to change. This handles bit fields and values crossing byte boundaries without
embedding application-specific entry types in the transport.

The kernel independently intersects the user mask with the output mask derived
from configured output Sync Managers, so an incorrect application mask cannot
select input or padding bits. Existing applications that already maintain a
flat process image and per-bit update mask can adapt that representation
without expanding updates into entry records.

All buffers, masks, and metadata are allocated before activation and freed only
after the cyclic thread is synchronously joined.

## Optional input history

API 0.16 optionally preallocates a bounded ring of complete coherent global
input images. The cyclic task writes a slot only when it is free; a batch
reader reserves selected slots briefly and the cyclic task drops capture
rather than waiting. Ordered cycle metadata, `dropped_records`, and the
cumulative capture-contention count make loss explicit. A zero configured
depth leaves the latest-snapshot path and cost unchanged.

## Initial staged implementation

1. Add generation-bound health/fault/re-arm status while output data remains
   permanently zero. Completed in API 0.6.
2. Add copied read-only domain snapshots and a standalone reader. Completed in
   API 0.7.
3. Add copied output publication without arming. Completed in API 0.8.
4. Add explicit re-arm and prove its state machine with an all-zero
   motion-inhibited output. Completed in API 0.9. A bounded nonzero
   commissioning output remains a separate decision.
5. Split output authority per domain so a drive fault need not gate healthy
   I/O. Completed in API 0.17 (global selectors remain for compatibility).
6. Measure copy and masking cost before considering mmap.
