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

The cyclic thread evaluates:

- master link state;
- online and operational state of every configured slave;
- complete working-counter state for every configured domain.

Startup is unhealthy until all conditions first become true. After that first
healthy state, any loss latches outputs disarmed and records a fault epoch.
Restored link/slaves do not restore the old output image. Re-arm requires an
output update newer than the fault epoch and an explicit arm operation while
the bus is healthy.

This behavior is deterministic software containment, not a replacement for
hardware safety.

The health/latch half of this rule is hardware-proven. With the cyclic
transport active and outputs disarmed, removing and restoring the servo supply
returned the configured drive to OP and complete WC without restarting the
transport. `rearm_required` remained set and the fault epoch count remained
one after recovery.

API 0.13 exposes each configured slave's online, operational, and AL state by
stable `config_id`. Its `data_valid` flag additionally requires a published
snapshot and complete WC for that slave's assigned domain. Snapshots remain
inspectable when another domain is incomplete. Aggregate health and the output
gate remain conservative across all configured domains.

## Copy concurrency

Read-only snapshots use two preallocated buffers. A process-context reader reserves
the active buffer briefly under a spinlock; the cyclic thread skips publishing
a new snapshot rather than overwriting a reserved buffer.

Outputs use two preallocated buffers and a short pointer/state spinlock. A
writer selects the inactive buffer, copies global-image-sized data and per-bit update
mask arrays from user space without holding the spinlock, intersects that mask
with the topology-derived output mask, and merges the selected bits over the
previous shadow. The cyclic thread reads only the published active buffer at a
cycle boundary, so it never waits for or copies from a buffer being modified.
API 0.13 retains one global publication and arm gate across all domains.

Each published input buffer also stores the exact cycle index that produced
its bytes. `GET_INPUT_SNAPSHOT` returns that per-buffer index rather than a
separately sampled global counter. `CYCLE_GET_INFO` associates the same
cycle's input sequence with the output generation selected before queue/send.
The selected generation is zero when the output gate chooses the zero image.

## IOD format compatibility

The current IOD path represents process data as a full zero-based EtherLab
domain byte array. The future adapter will use the API 0.12 concatenated global
image in the same way. Output updates carry a second equally sized byte array whose
set bits select individual bits to change. `IOComponent` builds this mask from
`io_offset`, `io_bitpos`, and `bitlen`, including values crossing byte
boundaries; `ECInterface::updateDomain()` applies those masked bits to domain
memory.

API 0.8 deliberately uses the same data-plus-bit-mask shape. An eventual IOD
adapter can therefore forward its accumulated output data and update mask
without expanding them into entry records. The kernel independently intersects
the IOD mask with the output mask derived from configured output Sync Managers,
so an incorrect application mask cannot select input or padding bits.

All buffers, masks, and metadata are allocated before activation and freed only
after the cyclic thread is synchronously joined.

## Initial staged implementation

1. Add generation-bound health/fault/re-arm status while output data remains
   permanently zero. Completed in API 0.6.
2. Add copied read-only domain snapshots and a standalone reader. Completed in
   API 0.7.
3. Add copied output publication without arming. Completed in API 0.8.
4. Add explicit re-arm and prove its state machine with an all-zero
   motion-inhibited output. Completed in API 0.9. A bounded nonzero
   commissioning output remains a separate decision.
5. Measure copy and masking cost before considering mmap.
