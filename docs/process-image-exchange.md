# Process Image Exchange

## Safety contract

The first user-space exchange API will copy shadows. It will not mmap or expose
EtherLab domain memory.

The complete domain layout remains the stable offset namespace. Kernel-owned
metadata derives an output bit mask from entries under output Sync Managers.
Inputs are copied from the processed domain into a coherent input snapshot.
Only masked output bits are copied from an explicitly published output shadow
into the EtherLab domain.

Initial outputs are zero and disarmed. Publishing an output image does not arm
it. User space must explicitly arm the exact configuration generation and a
fresh output sequence.

## Fault and recovery rule

The cyclic thread evaluates:

- master link state;
- online and operational state of every configured slave;
- complete domain working-counter state.

Startup is unhealthy until all conditions first become true. After that first
healthy state, any loss latches outputs disarmed and records a fault epoch.
Restored link/slaves do not restore the old output image. Re-arm requires an
output update newer than the fault epoch and an explicit arm operation while
the bus is healthy.

This behavior is deterministic software containment, not a replacement for
hardware safety.

## Copy concurrency

Input snapshots use two preallocated buffers. A process-context reader reserves
the active buffer briefly under a spinlock; the cyclic thread skips publishing
a new snapshot rather than overwriting a reserved buffer.

Outputs use two preallocated buffers and a short pointer/state spinlock. A
writer reserves the free buffer, copies from user space without holding the
spinlock, then publishes it. The cyclic thread swaps a complete pending image
at a cycle boundary. It never waits for a user copy and never copies from a
buffer being modified.

All buffers, masks, and metadata are allocated before activation and freed only
after the cyclic thread is synchronously joined.

## Initial staged implementation

1. Add generation-bound health/fault/re-arm status while output data remains
   permanently zero.
2. Add copied input snapshots and a standalone reader.
3. Add copied output publication without arming.
4. Add explicit re-arm and one motion-inhibited test output.
5. Measure copy and masking cost before considering mmap.
