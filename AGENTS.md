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
- Current UAPI is 0.12. It covers discovery, ordered setup SDOs, declarative
  PDO/DC configuration, domain registration, cyclic pumping, copied input and
  masked output images, explicit arm/disarm, health, timing/DC statistics, and
  per-configured-slave validity.
- Target is Debian RT kernel `6.1.0-49-rt-amd64` with EtherLab DKMS 1.6.9.
  Exact build artifacts are documented.
- Master contention, discovery, lifecycle, declarative PDO mapping, DC,
  zero-output cycling, live servo power loss/restoration, stale-output
  re-arming, and controller-death teardown have hardware evidence.
- No nonzero output has been requested or tested. Motion remains inhibited for
  hardware tests.
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
- The API 0.12 documentation acceptance gate passes after the multi-domain
  audit. The kernel-safety gate remains open, so IOD integration is still
  blocked.
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

- Current API: 0.12.
- Deactivation synchronously gates outputs and joins the cyclic thread, then
  waits for configured slaves to leave SAFEOP/OP before invalidating
  EtherLab-owned pointers. The public EtherLab lifecycle can still expose an
  ED3L watchdog event during its asynchronous PREOP transition.
- DC policy and parameters remain user-space owned. The installed IOD
  reference-led algorithm is preserved; timing acceptance is not yet claimed.
- Process images are bounded copied buffers. Output publication is masked to
  configured output entries; arm requires the active generation, latest
  publication sequence, and a healthy bus. Disarm/fault requires a newer
  publication before re-arm.
- Live servo power loss/restoration recovers without transport restart and
  leaves `rearm_required` latched. Nonzero output remains untested.
- Copied process-image concurrency and recovery rules are documented in
  `docs/process-image-exchange.md`.
- The 2026-07-24 architecture review is in
  `docs/architecture-review-2026-07-24.md`. The safety gate remains open.
  Major gaps include debug-kernel validation, manual EtherLab build
  compatibility, nonzero-output commissioning, and timing acceptance.
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
  offsets, per-domain validity, and the still-global output gate. The
  documentation acceptance gate passes.
- Next step: capture the powered-off ED3L case showing EL5152 valid and the
  ED3L domain invalid, then re-audit API 0.12 documentation and safety status.
- Do not begin IOD integration before the standalone architecture and
  acceptance review required by `Implementation_Plan.md`.
