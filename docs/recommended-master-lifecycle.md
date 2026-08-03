# Recommended EtherCAT Master Lifecycle

## Purpose

This note records the recommended generic design before the repository freezes
its configuration or setup-SDO UAPI. It combines the target EtherLab 1.6.9
source with published EtherLab and EtherCAT device-state documentation.

## Recommended production path

The production transport should use EtherLab's persistent application
configuration model:

```text
request master
create expected slave configurations
attach declarative Sync/PDO/PDO-entry configuration
attach ordered startup SDO configuration
attach distributed-clock configuration where requested
create/register domain
prepare a non-energizing initial output image
activate master
run receive/process/queue/send continuously
monitor master/domain/per-slave configuration state
```

When distributed clocks are enabled, the kernel cyclic owner must also perform
the application-time and clock-synchronization calls at the same defined point
in every cycle. User space selects the reference and supplies per-slave DC
parameters; the kernel owns timing execution and statistics. Preserve the
existing IOD algorithm before attempting controller improvements.

Expected slave configurations must be created from the user-space machine
model, not only from the slaves present during the initial scan. The target
`ecrt_master_slave_config()` implementation creates a configuration object even
when its addressed slave is absent. It attaches later when a matching
alias/position and vendor/product appears.

This is consistent with EtherLab's application model: configuration is built
before `ecrt_master_activate()`, after which configuration must remain
unchanged while cyclic processing and asynchronous requests run. See the
[EtherLab API usage notes](https://docs.etherlab.org/ethercat/1.6/doxygen/apiusage.html)
and [EtherLab application interface](https://docs.etherlab.org/ethercat/1.6/doxygen/group__ApplicationInterface.html).

## Persistent configuration SDO scope

`ecrt_slave_config_sdo()` and its typed helpers copy each startup value into
the persistent `ec_slave_config_t` in insertion order. The target EtherLab
slave configuration state machine executes that list in PREOP before moving
toward SAFEOP.

EtherLab's published design explicitly places startup SDO configuration in the
slave configuration state machine so it is applied every time the slave is
reconfigured. This is the desired behavior when a powered-down drive returns.
See the [EtherLab 1.6.9 manual](https://docs.etherlab.org/ethercat/1.6/pdf/ethercat_doc.pdf).

The target 1.6.9 API places a strict boundary around this mechanism:
`ecrt_slave_config_sdo()` says not to use configuration SDOs for PDO assignment
objects `0x1c10`--`0x1c2f` or mapping objects `0x1600`--`0x17ff` and
`0x1a00`--`0x1bff`. Those are owned by `ecrt_slave_config_pdos()` and related
declarative calls. Persistent configuration SDOs remain appropriate for
non-PDO startup parameters such as mode of operation at `0x6060:00`.

By contrast, `ecrt_master_sdo_download()` is a blocking ad-hoc transaction.
It is useful for:

- standalone commissioning and diagnostics;
- reading/writing a known online slave before activation;
- reproducing the legacy ED3L recipe for the decision gate;
- comparing actual object dictionaries.

It should not be the normal production recovery mechanism because the
application would have to detect every reappearance and coordinate a custom
replay with EtherLab's own configuration state machine.

## PDO configuration

PDO assignment and mapping should normally be submitted declaratively through
`ecrt_slave_config_pdos()`. EtherLab then owns the PREOP configuration
lifecycle and can reapply the desired layout after slave reconfiguration.

The EtherCAT state model expects mailbox parameterization and configurable PDO
mapping during PREOP. SAFEOP permits input process data while outputs remain in
a safe state under the device/watchdog policy. Before transition to OP, the
master must already provide valid output data. See Beckhoff's
[EtherCAT State Machine](https://infosys.beckhoff.com/content/1033/el2535/1036980875.html)
and the ETG
[EtherCAT Device Protocol Poster](https://www.ethercat.org/download/documents/EtherCAT_Device_Protocol_Poster.pdf).

The ED3L decision gate remains necessary because the drive may require a
specific mapping sequence. The candidates are:

1. declarative `ecrt_slave_config_pdos()` only;
2. declarative PDO configuration plus persistent non-PDO startup SDOs;
3. only if declarative configuration fails, a documented explicit fallback
   with one owner per mapping object.

One-shot master downloads are the known-good comparison baseline, not the
preferred final lifecycle.

## Power loss and automatic rescan

Client-owned ad-hoc setup after return (debounce, **PREOP/SAFEOP-only** gate
for PDO map CoE, short hold, `waiting_preop` while OP, retry) is documented
for **all** controllers in
[`client-slave-recovery.md`](client-slave-recovery.md). Kernel rescan alone does
not re-run those batches.

**Field rule:** PDO mapping / SM assignment CoE must complete in PREOP or
SAFEOP. Applying map recipes in OP fails. Clients must not treat OP as
“setup-ready.” True **hold-in-PREOP until setup complete** while cyclic is
active is a **module gap** — see
[`client-slave-recovery.md` §9](client-slave-recovery.md#9-kernel--elc-requirement-hold-preopsafeop-until-setup-complete).

In target EtherLab 1.6.9:

- activation enables scanning after topology changes;
- a responding-slave-count change requests a rescan;
- rescanning clears transient scanned slave objects, then recreates them;
- persistent application slave configurations remain owned by the master;
- configurations are reattached by alias/position and vendor/product;
- attached slaves are driven through their configuration state machines;
- ordered configuration SDOs and PDO setup are executed again.

The kernel transport therefore should not restart or rebuild its entire
configuration merely because an expected slave disappears. It should keep the
master active, keep cycling, and expose per-slave online/operational/error
state through `ecrt_slave_config_state()`.

`ecrt_master_reset()` exists to retry configuration of slaves after a
configuration failure. It is not a substitute for ordinary topology-change
rescan. Retry policy needs bounded backoff and diagnostics; it must not create
a tight reconfiguration loop for a persistently invalid device.

## Addressing and optional aliases

EtherLab attaches configurations using:

```text
alias + relative position + vendor ID + product code
```

Station aliases are optional. The design must work with alias zero and must not
require ED3L alias support.

The five observed ED3Ls have alias zero, serial zero, and identical
vendor/product/revision. Their logical axis assignment therefore comes from
the configured physical topology and absolute ring position. A nonzero alias
may strengthen matching on devices that support it, but it is only an optional
user-supplied addressing mode.

The kernel must never search for "another matching ED3L" and silently move an
axis configuration when its expected position is absent. It retains the
logical configuration ID and expected position, reports that configuration
offline, and reattaches only when the expected position and surrounding
topology are consistent.

A missing device in the middle of a reachable chain can alter positional
meaning. The initial implementation must fail closed on topology/identity
mismatch and require explicit reconfiguration or operator acknowledgement
after physical rewiring. Commissioned aliases are a useful optional
improvement where supported, not a baseline dependency.

## Output behavior on return

EtherCAT requires valid outputs before SAFEOP-to-OP. "Valid" at the protocol
level does not mean "safe for this machine."

The kernel should provide a generic per-slave recovery gate:

```text
offline
  -> online/configuring
  -> configured SAFEOP or OP-but-gated
  -> user-space recovery acknowledgement
  -> ordinary output image enabled
```

The precise achievable gate point must be tested with EtherLab. If EtherLab
automatically requests OP immediately after successful configuration, the
kernel still must ensure that the domain contains a user-supplied
non-energizing recovery image before the returning slave can consume outputs.
Clockwork subsequently decides when to publish ordinary commands.

Do not encode CiA-402 semantics in the kernel. User space supplies recovery
images and re-arm policy.

## UAPI consequence

Keep two mechanisms distinct:

- **persistent setup configuration**: ordered data stored as part of a pending
  slave configuration and committed before activation;
- **ad-hoc SDO transfer**: explicit diagnostic/commissioning operation against
  an online slave, unavailable from the real-time cyclic path.

Do not name an ad-hoc transaction `CONFIG_APPLY` or imply that it is the
production configuration transaction. The configuration UAPI should be
finalized together with slave/PDO/domain objects so ownership and replay
semantics are unambiguous.

## Next experiments

1. Capture ED3L object baselines with the current direct backend stopped.
2. Prove the legacy ordered recipe through ad-hoc downloads.
3. Configure the same mapping through persistent configuration SDOs.
4. Configure it through declarative PDO data.
5. For each approach, test startup absent, power loss, restoration, OP/WC,
   object readback, and safe output behavior without restarting the owner.
6. Select the simplest reliable persistent mechanism.
