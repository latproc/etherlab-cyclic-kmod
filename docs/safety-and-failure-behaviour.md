# Safety and Failure Behaviour

## Status

API 0.10 implements global configured-bus health, sticky fault/re-arm status,
topology-derived output ownership, copied output shadows, and an explicit
generation/sequence arm gate. A position-29 servo power-loss/restoration test
recovered without restarting the controller while outputs remained disarmed.

Per-configured-slave online/operational/AL state and conservative data validity
are now available by stable configuration ID. This is experimental evidence,
not production safety certification; acceptance and debug-kernel gates remain
open.

## Servo power observation

Observed on 2026-07-24:

- with servo control power off, EtherLab/IOD reported 29 slaves;
- with control power on, it reported 34 slaves;
- five `Summa ED3L ServoDrive` devices appeared at positions 29 through 33;
- all five reached OP under the existing direct IOD backend;
- all five reported vendor `0x0000060a`, product `0xed310001`, revision
  `0x00000001`, serial `0`, and alias `0`.

The current operational workaround is to detect drive power loss/restoration
and restart IOD. The kernel backend must remove that restart dependency.

## Required startup behavior

Clockwork and the kernel transport must be able to start and operate available
bus devices while the ED3L drives are unpowered.

The desired configuration is not identical to the currently scanned topology.
Clockwork must be able to submit expected slave configurations from its machine
model even when those slaves are absent. The kernel must retain those expected
configurations and report per-slave presence/configuration state.

Missing expected drives must not make the transport fail globally unless
Clockwork explicitly marks them as required for startup. Required/optional
startup policy belongs in user space.

## Required power-loss behavior

When a configured drive disappears, the current implementation:

1. keeps the cyclic thread running;
2. reports aggregate configured online/operational counts and fault bits;
3. closes the global output arm gate;
4. latches `rearm_required` and the fault epoch;
5. continues publishing snapshots and counters;
6. permits EtherLab to reconfigure the returning slave without process restart.

API 0.10 reports each configured slave separately. Its `data_valid` requires
that slave online and OP, at least one input snapshot, and complete global
domain WC. This is intentionally conservative; offline-transition hardware
evidence through the new ioctl is still required before IOD integration.

If one powered device also disconnects downstream devices, every affected
configuration must report offline. The kernel must not silently renumber
expected devices and attach configurations to a different physical device.

## Required restoration behavior

When a drive returns:

1. identity is checked against the expected configuration;
2. PDO/setup configuration is reapplied in a defined order;
3. it reaches the required EtherCAT and working-counter state;
4. only then is it reported operational;
5. normal outputs remain gated until application recovery policy permits them.

The five observed ED3Ls have identical vendor/product/revision and no usable
serial or alias. Their current distinction is physical chain position. A
topology change cannot be resolved by pretending these identities uniquely
identify a drive. Mismatch/reordering must be reported rather than guessed.

Station aliases are optional. Zero-alias devices remain supported through the
configured physical topology and absolute position. When aliases are available
they may strengthen matching, but neither Clockwork startup nor recovery may
depend on them.

## Output re-arm requirement

The cyclic loop may retain the most recent output image while a drive is
offline. Blindly applying that image when the drive returns could replay a
CiA-402 enable/control word or nonzero target velocity.

A returning slave does not automatically consume stale ordinary outputs. API
0.9 implements a conservative global gate. Arm requires the exact active
configuration generation and latest output-publication sequence while the bus
is healthy. After a fault or manual disarm, the sequence must be newer than the
fault/disarm epoch. Disarm and orderly deactivation wait until the cyclic
thread has sent a zero-gated image.

This safely sacrifices availability: one configured-slave fault disarms all
outputs. Per-slave gating may be considered later, but is not required to
weaken this baseline. The kernel does not embed ED3L or CiA-402 meaning.

Clockwork owns the decision that machine state is safe to re-arm. Hardware
safety remains responsible for personnel protection.

## Consequences for setup SDO design

One-shot synchronous `ecrt_master_sdo_download()` can prove raw ordered writes
while a slave is online, but does not itself solve reconfiguration after power
restoration.

EtherLab slave-configuration SDOs are stored in insertion order on
`ec_slave_config_t` and replayed by EtherLab's slave configuration state
machine. Declarative `ecrt_slave_config_pdos()` is also intended to be
reapplied during slave configuration.

The ED3L decision gate must therefore compare:

- explicit synchronous SDO mapping;
- ordered SDOs attached to persistent slave configuration;
- declarative PDO configuration;
- any necessary hybrid.

For each candidate, test startup with drives absent, power loss while running,
power restoration without application restart, actual object readback, OP/WC
recovery, and output gating. Exactly one component must own each PDO mapping
operation during a configuration transition.

## State model requirement

At minimum, status must distinguish:

```text
EXPECTED_OFFLINE
ONLINE_UNVERIFIED
IDENTITY_MISMATCH
CONFIGURING
OPERATIONAL_DISARMED
OPERATIONAL_ARMED
CONFIGURATION_ERROR
```

These are conceptual states. Final UAPI values require review alongside the
configuration and recovery implementation.

Global transport state and per-slave state must remain separate. One absent
optional drive must not force all available EtherCAT I/O offline.

## API 0.4 zero-output cyclic increment

The first activation increment exposes no user-space process-image writer.
After EtherLab allocates the domain, the kernel zeroes the complete domain
before starting its cycle thread. This gives the decision-gate test a
non-commanding cyclic pump, not a production output protocol.

The cycle thread is synchronously stopped before EtherLab deactivation or
master release. The implementation waits one bounded cycle and drains the
final response before deactivation. A missed activation result caused by
`copy_to_user()` failure also triggers synchronous deactivation. Activation
failure poisons the control session because EtherLab may have partially
finished domain construction; the safe recovery is close and reopen.

EtherLab 1.6.9 requests PREOP asynchronously during deactivation, after the
application cyclic sender has stopped. With an enabled output Sync Manager
watchdog, the drive can report a watchdog fault before that transition
completes. Rapid reacquisition can also collide with pending idle-state-machine
datagrams. This unresolved lifecycle boundary must not be hidden by disabling
the production watchdog.

The transport now bounds the reuse side of that boundary: deactivation waits
up to five seconds until every configured, present physical position has left
SAFEOP/OP. Failure poisons the current session and requires close/reopen. This
does not solve or suppress a watchdog event during the transition itself.

Generations, copied process images, explicit arm/re-arm, fault-triggered
disarm, zero-gated deactivation, and per-configured-slave validity now exist.
An explicit live kill while an all-zero shadow was armed proved synchronous
file-release teardown with no cyclic-task leak or new kernel warning.

## Tests required before production

- start Clockwork/kernel transport with all five drives off;
- power all drives on and recover without restarting either component;
- power drives off while active and continue available terminal I/O;
- restore power and verify ordered configuration/object values;
- verify no stale control word or target causes unexpected enable/motion;
- repeat power cycles and configuration recovery many times;
- test partial visibility where physically possible;
- verify identity/topology mismatch faults instead of silent reassignment;
- compare declarative, persistent configuration-SDO, and explicit recipe
  recovery behavior.
