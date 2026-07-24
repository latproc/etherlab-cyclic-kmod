# Repository Instructions

## Mission

Build a standalone, generic Linux kernel transport for deterministic EtherCAT
cyclic I/O using the EtherLab master. Runtime user space supplies slave, Sync
Manager, PDO, PDO-entry, and SDO configuration. Clockwork/IOD is a later
consumer; this repository must not contain Clockwork machine policy.

Read `Implementation_Plan.md` before architectural or implementation work. It
is the authoritative project plan. This file is the concise operating guide
and living project status; update it whenever completed work, discoveries,
decisions, risks, commands, or next steps change.

## Current Status

- Current phase: standalone Phase 3 hardening after the first formal
  architecture review. IOD integration remains blocked.
- The implementation plan has been read in full.
- A minimal kernel probe, DKMS-aware build, environment documentation, and
  lifecycle test script exist.
- With Clockwork stopped and the master idle, the probe builds against the
  exact target and passed ten live load/acquire/release/unload iterations
  without retaining the master.
- Contention while IOD owns master 0 is proven for both the minimal probe and
  Phase 2 device: ownership attempts return `EBUSY` without disturbing IOD.
- Versioned UAPI 0.1, `/dev/cw_ethercat0`, `cw_ec_bus`, ABI tests, and a
  repeatable topology comparison exist.
- All then-present 29 physical slave identity records matched
  `ethercat slaves -v`; topology was unchanged after release. An additional
  120 open/scan/close lifecycle iterations completed, with the final 20 adding
  no kernel warning.
- API 0.3 accepts and validates a bounded pending slave/Sync/PDO/PDO-entry
  hierarchy, constructs persistent EtherLab configuration objects, registers
  a domain, and resolves stable entry IDs to byte/bit offsets.
- API 0.4 builds a configurable-period cyclic pump around an applied domain,
  zeroes the image before its first send, reports basic timing/send counters,
  and synchronously joins before deactivation. It intentionally has no
  process-image writer or DC configuration yet. Position 29 has reached OP
  with complete working counter under motion inhibit.
- API 0.5 adds transactional DC records, reference policy, the IOD-compatible
  reference-led controller, cyclic slave synchronization, monitoring, and a
  bounded status ioctl. Four motion-inhibited position-29 runs reached complete
  WC with valid reference reads, monitor results, and zero cycle errors.
- API 0.6 adds generation-bound health/fault/re-arm status while outputs remain
  hard-disarmed.
- API 0.7 adds a coherent copied, read-only domain snapshot. It uses
  preallocated double buffers and never waits for user space in the cyclic
  path. A motion-inhibited position-29 run returned live TxPDO bytes with all
  RxPDO/output bytes zero.
- API 0.8 accepts generation-bound copied output shadows and masks them to
  configured output entries, but provides no arm operation. Its domain-sized
  data plus per-bit update mask matches IOD's existing update representation.
  The cyclic thread clears all configured output bits before every send.
  Publishing all-ones data/mask on position 29 left all 18 output bytes zero.
- API 0.9 adds explicit generation/latest-sequence-bound arm and synchronous
  disarm. Faults disarm in the cyclic thread; re-arm after fault or manual
  disarm requires a newer publication. A zero-only position-29 test proved
  arm, acknowledged disarm, stale-sequence rejection, and fresh-sequence
  recovery without transmitting a nonzero output.
- API 0.10 adds generation-bound per-configured-slave state keyed by stable
  `config_id`. It reports online/operational/AL state and a conservative
  `data_valid` bit requiring that slave OP plus complete domain WC. Position 29
  reported valid at cycle/input sequence 5,000; an offline transition query is
  not yet hardware-captured.
- The current zero-only lifecycle test ran five complete API 0.10
  configure/activate/arm/disarm/deactivate/close iterations. The EtherLab
  master returned idle after every iteration, no cyclic task leaked, topology
  was unchanged, and no new kernel warning/error line appeared.
- A provisional bounded ad-hoc SDO batch exists for commissioning and
  decision-gate tests. It is not the persistent production setup mechanism.
- Bounded SDO upload is hardware-proven against ED3L `0x6060:00`; no write has
  been issued. Post-power-up readback showed all five drives in the default
  two-entry position PDO layout, not the planned velocity layout. Confirm the
  legacy restart/recipe baseline before writing.
- Installed `iod.sh` confirms the recovery gap: it configures only ED3Ls
  visible during startup. The installed `sdo.sh` matches the velocity recipe;
  its behavior has been captured and a cleaned strict position-29 batch was
  verified.
- Test A legacy mapping is captured on all five drives. The legacy script
  reaches the right final state but ignores aborts from unnecessary zero-entry
  writes. A cleaned strict 21-write kernel batch succeeded on position 29 with
  full final-state verification.
- Installed `iod` and `iod_sdo` both compile with `USE_DC`. Preserve the
  documented reference selection, application-time steering, cycle ordering,
  monitoring, and adjustment algorithm in the eventual kernel cyclic thread.
- Phase 2 acceptance is complete. Current work is the standalone cyclic/DC
  portion of Phase 3.
- Servo-off startup and live power-loss/restoration without restarting IOD are
  now mandatory requirements. Recovery must gate stale outputs until user
  space explicitly permits re-arm.
- Distributed clocks are mandatory when configured. Preserve the installed
  IOD algorithm first: user space owns DC policy/parameters; the kernel cyclic
  backend owns reference selection, application time, cyclic sync calls, and
  low-overhead DC status/statistics.
- EtherLab 1.6.9 explicitly reserves PDO assignment/mapping for declarative
  `ecrt_slave_config_pdos()` configuration. Persistent
  `ecrt_slave_config_sdo()` entries are replayed on reconfiguration, but its
  API says not to use them for PDO assignment or mapping objects. Use them for
  ordinary startup parameters such as operating mode. Ad-hoc master SDO
  downloads remain a commissioning fallback, not normal recovery.
- EtherCAT station aliases are optional. Baseline matching must support alias
  zero using configured physical topology and absolute position. Never
  remap an absent logical axis to another identical device merely because its
  vendor/product matches.
- Do not modify Clockwork/IOD behavior in this repository.

## Non-Negotiable Architecture

User space owns:

- device identity and matching policy;
- Beckhoff/ESI XML parsing;
- PDO-layout and setup-recipe selection;
- machine and application semantics;
- interpretation of process data;
- runtime SDO policy and scheduling.

Clockwork integration must support two explicit process-entry selector modes:

- legacy flattened `pos`, preserved for unconverted systems;
- durable EtherCAT object identity, normally `(index, subindex)`.

If `(index, subindex)` is duplicated, require an additional PDO index or
occurrence discriminator. Never infer selector mode from numeric magnitude and
never silently fall back from a failed object selector to `pos`.

Provide a separate conversion/audit tool for Clockwork configurations. It must
use a captured matching topology/PDO description, default to dry-run, report
ambiguous or missing entries, preserve backups, and only rewrite selectors
that resolve uniquely.

The kernel module owns:

- EtherLab application/master lifecycle;
- raw bus discovery;
- execution of validated generic configuration;
- ordered typed setup-SDO execution;
- slave configuration, domain registration, and entry mappings;
- deterministic cyclic receive/process/queue/send;
- process-image exchange and low-level runtime mailbox execution;
- low-overhead status and cycle statistics.

Never put ED3L/Summa/Estun identities, recipes, XML parsing, machine layouts, or
Clockwork-specific policy in the kernel.

## Required Work Order and Gates

Work incrementally and keep every stage independently buildable and testable.
Do not begin a later phase before its prerequisite evidence exists.

1. Inspect and document the target kernel and EtherLab installation/build.
2. Build a minimal external module that requests master 0, reports the result,
   releases it, and unloads cleanly.
3. Add a versioned character-device status/discovery API and `cw_ec_bus`.
4. Add generic ordered typed pre-activation SDO operations and `cw_ec_sdo`.
5. Run the ED3L PDO mechanism decision gate: compare explicit ordered SDO
   mapping with `ecrt_slave_config_pdos()` using readback, operation,
   power-cycle recovery, and lifecycle repetition.
6. Only then finalize generic slave/sync/PDO/entry configuration and domain
   registration.
7. Add the cyclic loop, process-image exchange, statistics, and standalone IO
   tools.
8. Stop for an architecture review before any IOD integration.

Before IOD integration, the standalone kernel-safety acceptance gate and
documentation acceptance gate in `Implementation_Plan.md` must pass.

## First Phase Deliverables

Create and maintain:

- `docs/building/etherlab-dkms-environment.md` for the actual target build
  environment, including exact header, build, generated-header, and
  `Module.symvers` paths;
- a minimal kernel experiment/module and matching Kbuild instructions;
- a standalone master acquire/release test script;
- evidence of repeated clean load/acquire/release/unload and restored EtherLab
  CLI usability.

The plan mentions an old Clockwork architecture document. This standalone
repository does not contain Clockwork sources, so do not fabricate it. Inspect
Clockwork only when its repository/branch is explicitly available.

## Kernel Safety Rules

- Treat all user input as hostile. Version and size ABI structures; use fixed
  width Linux UAPI types; validate counts, enums, references, duplicates,
  lengths, state, and arithmetic before allocation or EtherLab calls.
- Define conservative hard limits for every user-controlled resource.
- Never expose kernel pointers or retain raw user pointers.
- Check every return value and return useful negative errno values.
- Every construction path needs a complete partial-failure unwind.
- Prefer zero-initialized allocations and one top-level owner per
  configuration. Cleanup helpers should tolerate partial initialization and be
  idempotent where practical.
- Configuration is transactional (`pending` versus `active`) and immutable
  while running. Initial reconfiguration is stop/configure/start only.
- Stop and synchronously join threads, timers, workers, and asynchronous SDO
  activity before freeing anything they can reference.
- Define ownership, lifetime, locking, lock order, and state transitions before
  adding concurrency. Do not sleep under spinlocks and avoid sleeping locks in
  the cyclic path.
- No allocation, blocking mailbox operation, normal-cycle logging, or
  user-space call in the cyclic path.
- No `BUG()`, `BUG_ON()`, panic-style handling, or kernel warnings for
  recoverable input/runtime errors.
- Protect IDs with configuration generations; use `kref`/`refcount_t` only for
  real shared ownership.
- Start with one exclusive control owner. Add multi-client control only after a
  documented need and lifetime design.
- Prefer copying process images before mmap/zero-copy complexity. If mmap is
  added, document page allocation, VMA references, close/reset/unload behavior,
  and stale-generation handling first.
- Module unload must have an explicit, race-free teardown order and must not
  outlive open files, mappings, threads, timers, work, or SDO requests.

## EtherLab and Build Rules

- Inspect the exact installed/target EtherLab version, `ecrt.h`, source when
  needed, exported symbols, and matching kernel artifacts. Do not assume APIs
  from another release.
- Support both EtherLab DKMS installations and explicit manual source/build
  paths through one resolved build contract.
- Never silently use headers, generated files, or `Module.symvers` from a
  different kernel/EtherLab build. Ambiguous auto-detection must fail clearly.
- Confirm lifetimes and behavior of every used `ecrt_*` operation against the
  target implementation.
- Keep diagnostic EtherLab CLI access available whenever this module has
  released the master.

## UAPI and Runtime Design

- Use API major/minor versions plus structure size/version fields.
- Prefer stable user-supplied entry IDs; return offset and bit position keyed by
  those IDs.
- Setup SDOs are ordered typed transactions, distinct from asynchronous runtime
  SDO handles. Preserve abort/error context and identify the failed sequence.
- Do not let explicit SDO mapping and EtherLab declarative PDO configuration
  race or unknowingly configure the same objects. Document one owner per
  operation.
- Cycle period is configurable in nanoseconds; do not hard-code 2 kHz.
- Preserve the target's receive/process/DC/queue/send order after inspection.
- Measure timing under load; do not claim deterministic improvement without
  comparable data.
- Explicitly define initial outputs, stale outputs, controller death, link/slave
  loss, topology mismatch, deactivation, and restart behavior. Software is not
  a substitute for hardware safety.

## Testing Expectations

Each feature needs no-hardware validation where possible and hardware evidence
where required. Add explicit tests for:

- malformed, oversized, version-mismatched, duplicate, stale, and out-of-order
  ABI calls;
- every allocation/construction failure stage;
- repeated configure/activate/deactivate/reset/close cycles;
- repeated safe module load/unload and master acquire/release;
- thread, timer, worker, mapping, and SDO teardown;
- entry-ID and generation behavior;
- link/slave loss and controller exit;
- kmemleak, KASAN/KFENCE, and lockdep procedures where supported.

Normal error results must produce no oops, warning, leak, dangling thread, or
unrecoverable EtherLab master.

## Documentation and Licensing

- Documentation changes are part of implementation changes, not deferred
  cleanup.
- Keep `README.md`, build, architecture, UAPI, testing, and safety documentation
  accurate as their subjects appear.
- Record tested versions and observed results; never claim untested
  compatibility.
- Repository licensing is GPL-2.0-only unless an explicit documented decision
  changes user-space component licensing.
- Kernel C files use `// SPDX-License-Identifier: GPL-2.0-only` and the module
  declares `MODULE_LICENSE("GPL")`.
- Preserve third-party notices and document any adapted EtherLab-derived code.

## Working Practices

- Preserve unrelated user changes and inspect the worktree before edits.
- Prefer small commits that leave a runnable/testable state; do not combine
  unrelated phases.
- Do not remove a known-good fallback until its replacement is proven.
- Do not perform machine motion automatically. Hardware tests begin with motion
  safely inhibited and follow the site's commissioning procedure.
- Record exact commands and results for environment discoveries and acceptance
  tests in the relevant documentation.

## Living Status

Keep this section concise. Historical milestones and validation evidence are in
`docs/project-history.md`; focused details belong in the relevant design,
testing, safety, and build documents.

- Current API: 0.10.
- API 0.4 zero-output cyclic activation is hardware-proven on ED3L position 29
  with complete working counter and exact 28-byte PDO layout.
- Deactivation waits for configured slaves to leave SAFEOP/OP and invalidates
  EtherLab-owned configuration/domain pointers. The public EtherLab lifecycle
  still permits an ED3L Sync Manager watchdog event when traffic stops before
  the asynchronous PREOP transition.
- API 0.5 accepts and applies bounded per-slave DC parameters and
  disabled/automatic/explicit reference policy. Its reference-led controller
  and status snapshot are hardware-proven on position 29. Short initial
  synchrony convergence remains visible and timing acceptance is not claimed.
- The ED3L DC fixture values are `AssignActivate 0x0300`, SYNC0 equal to the
  application period, and zero shift. These remain user-space policy.
- API 0.6 adds generation-bound bus health and stale-output re-arm status.
  Position 29 reported healthy with all configured counts correct while
  outputs remained hard-disarmed. A deliberate servo-supply power cycle then
  recovered position 29 to OP/complete WC without restarting the transport,
  while `rearm_required` remained set with one latched fault epoch.
- API 0.7 adds a generation-bound copied domain snapshot with a 64 KiB limit.
  A five-second position-29 retry reached OP and returned a coherent 28-byte
  image with live input data and zero outputs. It recorded one 8.1 ms scheduling
  overrun, so this is functional evidence, not timing acceptance.
- API 0.8 adds copied, generation-bound output publication without arming.
  The published image is masked to entries owned by output Sync Managers, and
  the cyclic path still forces those bits zero. A five-second all-ones shadow
  test completed 5,000 cycles with complete WC, no errors/overruns, and zero
  in every configured output byte. Publication and the hard-zero gate are
  hardware-proven; the retained ownership-masked shadow is not bus-observable
  until the future arm test.
- API 0.9 adds the explicit arm/disarm gate. Arm requires an active healthy
  bus, exact configuration generation, and exact latest nonzero publication
  sequence. Disarm waits for cyclic acknowledgement and requires a newer
  publication before re-arm. The complete state sequence is hardware-proven
  with an all-zero shadow; nonzero output remains untested.
- Copied process-image concurrency and recovery rules are documented in
  `docs/process-image-exchange.md`.
- The 2026-07-24 architecture review is in
  `docs/architecture-review-2026-07-24.md`. The kernel-safety and documentation
  gates remain open. API 0.10 now supplies conservative per-configured-slave
  validity; remaining major blockers include its offline-transition proof,
  current lifecycle/controller-death stress, allocation-failure/leak testing,
  debug-kernel testing, and manual EtherLab build compatibility.
- Next step: add and run the zero-armed controller-death teardown test.
- Do not begin IOD integration before the standalone architecture and
  acceptance review required by `Implementation_Plan.md`.
