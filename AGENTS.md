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

- Current phase: standalone Phase 3 hardening after the first architecture
  review. IOD integration remains blocked.
- Current UAPI is 0.17. It covers discovery, ordered setup SDOs, declarative
  PDO/DC configuration, domain registration, cyclic pumping, copied input and
  masked output images, explicit arm/disarm, health, timing/DC statistics, and
  per-configured-slave validity, coherent cycle timing, capability discovery,
  interruptible cycle notification, and acknowledged disarmed period updates.
- Target is Debian RT kernel `6.1.0-49-rt-amd64` with EtherLab DKMS 1.6.9.
  Exact build artifacts are documented.
- Master contention, discovery, lifecycle, declarative PDO mapping, DC,
  zero-output cycling, live servo power loss/restoration, stale-output
  re-arming, and controller-death teardown have hardware evidence.
- Nonzero output testing is limited to one-bit core-console EL2034 status-LED
  and buzzer commands while servo three-phase and E-stop output power were
  absent. Physical observation is not yet recorded. No actuator, drive-enable,
  or motion output has been requested or tested.
- Killing the controller during a bounded console-LED pulse gates output via
  file release, stops cyclic work, returns master 0 idle, and permits normal
  module unload without a topology or kernel-log regression.
- A captured-topology converter generated the full 34-slave configuration:
  71 Sync Managers, 288 PDOs, and 508 entries in separate Beckhoff/drive
  domains. All 34 slaves are hardware-proven online, OP, and valid
  simultaneously with both domains complete. A following 34/34-healthy
  session pulsed only the console LED update bit and synchronously disarmed.
- The converter must use slave-reported `sync_managers`, not IOD's requested
  `configured_sync_managers`. EL5152 position 3 is rev `0x00120000` and
  position 4 is `0x00140000`; diagnostic object identities differ at the same
  bit slots. Match live CoE maps per position. IOD `modules.lpc` must declare
  matching `RevisionNo` values. Clean full-topology activate is free of fixed-
  map PDO warnings after rescan with the dual-revision fixture.
- Five corrected full-topology zero-arm lifecycles and a full-topology
  zero-armed controller-death test pass with no new kernel warning/error,
  topology change, cyclic task leak, or retained master ownership.
- Six no-hardware converter regression tests cover reported/requested source
  selection, duplicate occurrences, padding, domain assignment, position
  gaps, and malformed numeric fields.
- PDO assignment/mapping belongs to `ecrt_slave_config_pdos()`. Persistent
  configuration SDOs are for ordinary startup parameters; ad-hoc master SDOs
  are commissioning fallback only.
- Aliases are optional. Baseline device matching uses configured physical
  topology/absolute position and never substitutes an identical device for an
  absent logical axis.
- Deterministic pending and cyclic process-image allocation unwind and
  maximum-count create/reset stress pass.
- Normal module unload is hardware-proven blocked while a zero-armed control
  fd, cyclic task, and master ownership are live. Controller death then
  releases all resources and permits unload.
- Twenty consecutive current-API zero-arm lifecycles pass after making the
  standalone tool wait up to five seconds for bus health before arming. Every
  iteration returned master 0 idle with no cyclic task; final topology was
  unchanged and no new kernel warning/error appeared.
- The target kernel lacks fault-injection, kmemleak, KFENCE, and lockdep
  validation facilities; debug-kernel evidence remains outstanding.
- Servo-off startup and live power-loss/restoration are mandatory. Outputs
  remain stale-gated until explicit user-space re-arm.
- Distributed clocks are mandatory when configured. User space owns policy;
  the kernel owns cyclic application time, synchronization calls, steering,
  and low-overhead status.
- Detailed milestones and evidence live in `docs/project-history.md` and
  `docs/testing.md`. Do not duplicate them here.
- `docs/operator-guide.md` is the end-to-end build, test, zero-output
  operation, teardown, and cleanup sequence.
- `README.md` is the concise project entry point: generic-interface benefits,
  requirements, installation, first safe tests, and documentation routes.
  `docs/developer-guide.md` gives the user-space controller lifecycle and
  points to the normative UAPI and reference tools.
- The API 0.12 documentation acceptance gate passed after the multi-domain
  audit. API 0.15 timing/lease/rate documentation must be re-audited after live
  validation. The kernel-safety gate remains open, so IOD integration is
  still blocked.
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
3. Add a versioned character-device status/discovery API and `elc_bus`.
4. Add generic ordered typed pre-activation SDO operations and `elc_sdo`.
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
- Default repository licensing is GPL-2.0-only for the kernel, tools, and
  documentation unless a file’s SPDX says otherwise.
- **`libelcethercat` is LGPL-2.1-or-later** (`lib/elc_ethercat.c`,
  `include/elc_ethercat.h`) so non-public applications may link the library.
  The shared UAPI header is dual-licensed
  `GPL-2.0-only OR LGPL-2.1-or-later`. Full texts: `LICENSE`,
  `LICENSE.LGPL-2.1`. Document this split in README and
  `docs/libelcethercat.md` when it changes.
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

- Current API: 0.17.
- Deactivation synchronously gates outputs and joins the cyclic thread, then
  waits for configured slaves to leave SAFEOP/OP before invalidating
  EtherLab-owned pointers. The public EtherLab lifecycle can still expose
  Sync Manager watchdog events during its asynchronous PREOP transition;
  repeated full-topology timing stops logged these for ED3L positions 29--33
  and, on the final stop, EL5152 positions 3/4 before idle recovery.
- DC policy and parameters remain user-space owned. The installed IOD
  reference-led algorithm is preserved; timing acceptance is not yet claimed.
- Process images are bounded copied buffers. Output publication is masked to
  configured output entries; arm requires the active generation, latest
  publication sequence, and a healthy bus. Disarm/fault requires a newer
  publication before re-arm.
- Live servo power loss/restoration recovers without transport restart and
  leaves `rearm_required` latched. Nonzero actuator/drive output remains
  untested.
- Copied process-image concurrency and recovery rules are documented in
  `docs/process-image-exchange.md`.
- The 2026-07-24 architecture review is in
  `docs/architecture-review-2026-07-24.md`. The safety gate remains open.
  Major gaps include debug-kernel validation, manual EtherLab build
  compatibility, actuator/drive-output commissioning, and timing acceptance.
- API 0.10 per-slave status is hardware-proven across live position-29 power
  loss and restoration: validity cleared offline, returned in OP, and
  `rearm_required` remained latched throughout recovery.
- Fault detail accumulates all causes within a re-arm epoch. A repeated live
  power cycle ended with latched mask `0x38`, one fault transition, restored
  OP/valid input, and outputs still disarmed.
- Active API 0.10 hostile-input checks pass for undersized snapshots, bad
  flags, stale generations, wrong output size, unknown output sequence, stale
  disarm, and duplicate activation. The checks leave outputs disarmed.
- The cyclic task supports immutable module-load scheduler controls:
  `cycle_cpu=-1` and `cycle_fifo_priority=0` preserve prior defaults. CPU 1 /
  FIFO 70 is live-proven; configuration occurs before the task's first cycle.
- API 0.11 represents mandatory `0x0000:00` PDO padding with `entry_id=0` and
  does not register padding as user process data. An XML-derived EL5152 mapping
  is hardware-proven with the drives absent.
- Servo-off mixed cycling proves present EL5152 data continues updating, but a
  shared domain has incomplete WC and cannot certify it independently.
  EtherLab has only domain-level WC; independently recoverable groups require
  separate domains rather than weakened validity.
- API 0.11 represents mandatory PDO padding explicitly. A mixed
  present-EL5152/absent-ED3L test proved that a shared domain's incomplete WC
  invalidates both devices even though the encoder remains in OP with live
  data.
- Confirmed domain policy: user space explicitly declares availability/
  validity domains and assigns every slave. Do not infer domains. Preserve an
  implicit single-domain compatibility mode when no domains are declared.
  Domain declaration order defines concatenated global process-image segments;
  per-slave validity follows its assigned domain WC.
- Recommended machine grouping is always-powered Beckhoff I/O separately from
  switchable drives, splitting drives further only for independently required
  validity or fault containment.
- API 0.12 implements explicit domain records, one assignment per configured
  slave, ordered global image segments, per-domain WC/status, and per-slave
  validity from the assigned domain. No-domain configurations retain one
  implicit compatibility domain.
- Explicit EL5152/ED3L domains completed 5,702 disarmed cycles with both
  devices powered and OP: segment sizes were 32 and 28 bytes, each WC was
  complete, and global offsets were stable. The unchanged implicit-domain
  ED3L fixture completed 2,000 disarmed cycles, proving legacy configuration
  compatibility. The servo-off independence case still needs capture.
- API 0.12 output publication/gating and latched re-arm state remain global.
  Do not claim independent domain output control.
- API 0.12 hostile domain-record/status checks pass. Ten maximum-count resets
  include 256 domains and 256 assignments. All 107 deterministic allocation
  failures across setup, legacy implicit-domain, and explicit-domain
  preparation unwind cleanly with unchanged topology and no new kernel
  warning/error.
- Five explicit multi-domain zero-arm lifecycles and an explicit multi-domain
  zero-armed controller-death test pass. Each returned master 0 idle, leaked no
  cyclic task, preserved topology, and added no kernel warning/error. Active
  resources correctly blocked module unload before controller death.
- The API 0.12 documentation audit updated README, architecture, UAPI,
  process-image, safety, and operator guidance for explicit domains, global
  offsets, per-domain validity, and the then-global output gate (superseded by
  API 0.17). The documentation acceptance gate passes.
- API 0.12 deterministic copied-image failures are allocations 19-24 because
  the implicit domain owns allocation 18. All six failures and cyclic-task
  construction failure unwind cleanly; allocation 25 completed 8,000 disarmed
  cycles with zero errors/overruns and no topology/kernel-log regression.
- The copied-image allocation harness now accepts a fixture-specific pending
  allocation count. For the explicit two-domain fixture, failures 68-73 and
  cyclic-task construction failure unwind cleanly; allocation 74 completed
  8,000 disarmed cycles with both domains complete and valid.
- Active domain-status hostile checks reject stale generation, unknown domain,
  and nonzero reserved fields while leaving outputs disarmed.
- Hostile domain-record checks now also cover header/version/reserved fields,
  zero and duplicate identifiers, missing and unknown assignments, and
  duplicate slave assignments.
- Declared reserved input is now consistently rejected across setup, SDO,
  configuration, domain creation/lookup, cyclic status, output publication,
  and per-slave/domain status ioctls. The expanded non-activating ABI suite
  passes and returns the 34-slave master idle.
- A reproducible disarmed timing harness compares baseline, same-CPU, and
  all-CPU load. The default three-by-30-second full-topology gate completed
  270,000 cycles with no errors/overruns; worst wake latency was 66,147 ns
  against the declared 250,000 ns Phase 3 gate, both domains were complete and
  valid, and all 34 slaves were operational. This is not DC or production
  timing acceptance.
- API 0.13 adds an additive coherent per-cycle record and interruptible
  wait-for-cycle ioctl. It reports monotonic scheduled/actual wake times,
  exact input-image cycle identity, selected output generation, armed reuse,
  missed deadlines, WC, health, and cycle result. Capability discovery does
  not advertise deferred scheduled output. Non-activating ABI checks pass. A
  2,001-cycle full-topology disarmed run proved coherent wait/timing and exact
  input cycle identity; a zero-only arm proved consumed generation 1 and nine
  stale reuse cycles. Both domains were complete and all 34 slaves OP/valid.
  Teardown again logged the known asynchronous watchdog boundary, plus one
  AL-state datagram initialization failure and one skipped master-FSM
  datagram, so clean-log/lifecycle acceptance remains open.
- The build contract is proven with explicit EtherLab header and matching
  symbol paths staged outside the DKMS layout. Missing headers, a kernel-only
  symbol file, and ambiguous DKMS auto-detection fail clearly. No independently
  compiled manual EtherLab revision exists locally, so binary/source-revision
  compatibility remains unclaimed. Reproduce the layout/fail-closed checks with
  `make test-build-contract`.
- Next powered test: capture the powered-off ED3L case showing EL5152 valid
  and the ED3L domain invalid. The bounded console LED/buzzer software commands
  have passed; record physical observation when an operator is present. Keep
  every actuator and drive output masked off.
- Before that power test is available, extend broader configuration
  fuzzing/SDO interruption coverage and plan longer timing/DC soak evidence.
- Section 13B now distinguishes process death from a hung controller retaining
  its fd. The initial lease design is resolved: explicit compatibility opt-in,
  1--1,000,000 armed-cycle budget, paused while disarmed, invalid on each
  activation, and renewed explicitly. Expiry zero-gates before output
  selection but keeps cyclic input exchange alive, latches a distinct
  controller-stale fault, and requires renewal, fresh output and explicit
  re-arm.
- Section 13C requires optional delegated per-domain user-space connections.
  Keep one coordinator, EtherLab master, cyclic task and global cycle timeline.
  The preferred interface is coordinator-created domain fds that may be passed
  with `SCM_RIGHTS`; each fd is confined to its immutable domain set and at
  most one writer owns a domain. A motion planner remains user-space policy and
  supplies a domain controller through ordinary user-space IPC.
- Implement Section 13B lease state internally as domain-set-scoped output
  authority even if the first compatible UAPI exposes only the existing
  all-domain owner. Do not deepen the current global-output-state assumption.
- The first internal authority refactor is implemented without a UAPI change:
  each configured domain points to the compatibility output authority, which
  now owns publication buffers/masks, sequence, arm/re-arm, latched fault
  epoch, gate acknowledgement, and stale-use state. The hostile ABI/topology
  harness and five zero-output full lifecycle iterations pass against the final
  code. Final teardown logged only the known position-29 ED3L watchdog
  boundary; master 0 returned idle with all 34 slaves visible. An earlier
  intermediate run also logged one AL-state datagram initialization failure
  and one skipped master-FSM datagram.
- API 0.16 `ELC_IOC_CYCLE_GET_DC_INFO` is the coherent DC motion-clock record
  (application time, reference validity/sample, phase, applied adjustment)
  snapshotted with the base cycle under `cycle_info_lock`. Scheduled
  cycle-addressed outputs remain future work: a wake after cycle N still
  cannot alter cycle N.
- API 0.14 adds an optional 1--1,000,000 armed-cycle lease to the compatibility
  output authority. It starts invalid on activation, pauses while disarmed,
  and expires into a distinct controller-stale fault and zero gate. Renewal
  alone cannot re-arm; recovery requires a newer publication and explicit arm.
  Hostile/inactive ABI coverage passes. A live 100-cycle all-zero lease expired
  while inputs continued, rejected stale recovery, accepted renewal plus a
  fresh zero publication, and returned master 0 idle. A subsequent
  lease-disabled zero-arm lifecycle also passed after one transient
  post-teardown health-wait failure.
- API 0.15 cycle-period update works while disarmed for non-DC and DC sessions.
  At the boundary the host period changes, DC SYNC0 config records that matched
  the previous period are rewritten, and the DC filter resets. ESC SYNC0
  hardware may wait for the next slave reconfiguration; deactivate/reactivate
  remains the hard guarantee. `cycle-rate` starts conservatively, waits for
  strict health, applies the target period, then measures.
- Staged disarmed full-topology screens activated at 1 ms and then ran at
  333,333, 250,000, 200,000, and 100,000 ns across baseline, same-CPU, and
  system load. All four ended with zero errors/overruns, 34/34 OP, and both
  domains valid; maximum lateness was 37,806, 39,388, 42,391, and 36,847 ns.
  This is short non-DC characterization, not production timing acceptance.
- A 600-second 100 us staged soak completed 6,002,674 kernel cycles with zero
  errors/overruns and 33,793 ns maximum lateness. A separate 60-second
  end-to-end user-space exchange completed 599,620 image reads/zero
  publications, skipped 381 intermediate cycle notifications, and measured
  kernel/user median latency of 2.5/9.5 us and p99.9 of 5.5/24.5 us. The
  274,992 ns user outlier proves 10 kHz cycling does not guarantee one
  user-space wake per cycle.
- A follow-up 100,000-interval 10 kHz exchange measured an exact 10 s
  scheduled span (`grid_error=0`) and 99,999.975 ns mean actual wake interval.
  The kernel deadline grid did not drift in that sample.
- `tools/elc_io` provides interactive list/read/watch/status and staged
  masked set/publish/arm/disarm commands over the shared configuration parser.
  Hardware tests proved 34/34 read-only operation and that a nonzero staged
  publication remains disarmed and unauthorized `arm` is refused.
- API 0.16 adds an optional preactivation full-input-image history ring with
  ordered batched reads and explicit overwrite/contention loss. A disarmed
  34-slave 1 kHz run drained 5,003 290-byte images in 10 ms batches with zero
  gaps, capture drops, cycle errors, or overruns; all slaves and domains
  remained valid.
- `elc_entry_offset.global_offset` is now the preferred name for the global
  concatenated-image byte offset. The old `domain_offset` member remains an
  ABI-neutral deprecated union alias.
- Timing trials now use a strict-health mode: every configured slave/domain
  must become OP/valid within five seconds before the timed interval, and load
  begins only after that gate. A short 1 kHz three-mode screen passed. A 2 kHz
  baseline reached all 34 OP and completed 6,604 cycles with zero overruns and
  10,741 ns maximum lateness, but the immediate second 2 kHz activation
  correctly aborted because EL5152 position 4 remained SAFEOP. Do not claim a
  2 kHz full-topology pass yet; test continuous load phases in one activation
  and investigate the repeated-activation boundary.
- Continuous single-activation rate screens pass 2 and 2.5 kHz across
  baseline, same-CPU, and all-CPU phases with zero errors/overruns; maximum
  lateness was 13,160 ns and 12,518 ns. Approximately 3 and 4 kHz abort before
  timing because EL5152 position 4 refuses OP with Sync Manager watchdog
  `0x001b`. Both EL5152s have the same approximately 100 ms ESC watchdog and
  advertise much shorter minimum cycles, so this is an activation/FSM
  limitation rather than a hardware-rate finding.
- Target EtherLab 1.6.9 declares `ecrt_master_set_send_interval()` but its
  kernel module does not implement/export it; only the cdev ioctl reaches the
  internal setter. Do not carry an EtherLab fork for this. Keep the selected
  user-space activation period, document the external limitation, and avoid
  claiming that EtherLab's operation-FSM interval was changed.
- Do not begin IOD integration before the standalone architecture and
  acceptance review required by `Implementation_Plan.md`.
- API 0.17: per-domain output authority. Each domain has independent
  arm/rearm/healthy state and process-image output buffers. Global publish
  (domain_config_id=0) fans out to all domains; arm/disarm flags=0 apply to
  all domains, non-zero flags select domain_config_id. Domain bus firewall:
  complete domain WC keeps that domain healthy/valid with no interruption when
  another domain fails (power loss, cable damage, module failure on that
  segment); incomplete WC fails only that domain. Transient slave_config
  offline during re-scan must not invalidate a complete-WC domain. Live
  dual-domain drive control power loss/restore: domain 1 stayed valid through
  re-scan; domain 2 failed and recovered; no transport restart. Capability
  ELC_CAP_DOMAIN_OUTPUT_AUTHORITY is advertised.
- Future (with Section 13C multi-client domain fds): also plan ring/redundant
  Ethernet (out one NIC, in another) so mid-bus cable splits can be handled
  physically; software domains alone cannot invent a path past a line break.
- IOD Stage 4 CW domain re-prove: dual INVALID + domain_size=0 during power
  loss is **iod lifecycle / sampling**, not an elc WC-firewall bug. Sustained
  d1-valid/d2-incomplete is proven at elc; presentation and re-activate policy
  fix in iod-elc/CW only. Do not weaken elc isolation for IOD poll quirks.
- Phase 7 `libelcethercat` is implemented under `lib/` with public header
  `include/elc_ethercat.h` (LGPL-2.1-or-later; SONAME major tracks UAPI major).
  Build/install: `make lib`, `make install-lib PREFIX=...`, pkg-config
  `elcethercat`. Feature tools use the generic `elc_*` prefix (EtherLab
  Cyclic): `elc_bus`, `elc_sdo`, `elc_config`, `elc_config_stress`; harness
  env vars use `ELC_*`. `elc_abi_test` keeps raw ioctls for hostile kernel
  UAPI checks. Contract: `docs/libelcethercat.md`. This project is a
  generic transport; the library and UAPI must not embed Clockwork or any
  other control-system policy. External runtimes are optional consumers via
  installable headers and `libelcethercat`.
