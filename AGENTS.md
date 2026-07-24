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

- Current phase: Phase 2 complete; Phase 3 setup-SDO investigation/design.
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
- All 29 physical slave identity records matched `ethercat slaves -v`; topology
  was unchanged after release. An additional 120 open/scan/close lifecycle
  iterations completed, with the final 20 adding no kernel warning.
- No persistent configuration, PDO/domain, process-image, or cyclic code
  exists.
- A provisional bounded ad-hoc SDO batch now exists in uncommitted Phase 3
  work for commissioning/decision-gate tests. It is not the persistent
  production setup mechanism and has not been loaded or executed on hardware.
- Bounded SDO upload is hardware-proven against ED3L `0x6060:00`; no write has
  been issued. Post-power-up readback showed all five drives in the default
  two-entry position PDO layout, not the planned velocity layout. Confirm the
  legacy restart/recipe baseline before writing.
- Installed `iod.sh` confirms the recovery gap: it configures only ED3Ls visible
  during startup. The installed `sdo.sh` exactly matches the planned velocity
  recipe. Running it is the next decision-gate step and requires motion safely
  inhibited plus explicit approval because it mutates all five drives.
- Test A legacy mapping is captured on all five drives. The legacy script
  reaches the right final state but ignores aborts from unnecessary zero-entry
  writes. A cleaned strict 21-write kernel batch succeeded on position 29 with
  full final-state verification.
- Installed `iod` and `iod_sdo` both compile with `USE_DC`. Preserve the
  documented reference selection, application-time steering, cycle ordering,
  monitoring, and adjustment algorithm in the eventual kernel cyclic thread.
- Phase 2 acceptance is complete. Current work is Phase 3 investigation and
  bounded ordered setup-SDO design.
- Servo-off startup and live power-loss/restoration without restarting IOD are
  now mandatory requirements. Recovery must gate stale outputs until user
  space explicitly permits re-arm.
- Distributed clocks are mandatory when configured. Preserve the installed
  IOD algorithm first: user space owns DC policy/parameters; the kernel cyclic
  backend owns reference selection, application time, cyclic sync calls, and
  low-overhead DC status/statistics.
- Research confirms the recommended production path is persistent EtherLab
  slave configuration: declarative PDOs plus ordered configuration SDOs applied
  by the EtherLab PREOP state machine and replayed on reconfiguration. Ad-hoc
  master SDO downloads are for commissioning and the decision gate, not the
  normal recovery mechanism.
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

## Living Notes

Update this section during work. Use dated entries for facts that may change.

- 2026-07-24: Repository contains `Implementation_Plan.md`, `.gitignore`, and
  `LICENSE`; implementation has not started.
- 2026-07-24: Full implementation plan reviewed. Phase 0 and the minimal
  master-acquisition experiment were selected as the first work.
- 2026-07-24: Target confirmed as Debian kernel `6.1.0-49-rt-amd64`,
  PREEMPT_RT enabled, with EtherLab DKMS 1.6.9. Exact `ecrt.h` and matching
  per-kernel `Module.symvers` paths are documented.
- 2026-07-24: Minimal probe built successfully with matching modversions and an
  `ec_master` dependency. Ten live load/acquire/release/unload iterations
  passed with Clockwork stopped; `ethercat master` remained usable and reported
  29 slaves/link up. This is not evidence of contention safety.
- 2026-07-24: Probe contention test passed with IOD running: load was rejected
  with `EBUSY`, while IOD retained active Operation state, 29 slaves, and link
  up.
- 2026-07-24: Phase 2 character device and `cw_ec_bus` reported all 29 slaves
  in physical order with identities matching EtherLab CLI. Malformed ABI,
  exclusive-open, release, and repeated lifecycle smoke tests passed.
- 2026-07-24: Exact Phase 2 contention test completed: module registration
  succeeded while IOD ran, `cw_ec_bus` received `EBUSY`, and IOD retained
  active Operation state with 29 slaves/link up.
