# Standalone Architecture Review — 2026-07-24

## Decision

The standalone transport is ready for continued standalone hardening, but it
is not ready for IOD integration or production use.

API 0.9 proves the central transport shape: transactional generic
configuration, EtherLab-owned recovery, distributed-clock cycling, copied
process images, topology-derived output ownership, and an explicit
generation/sequence arm gate. The remaining gaps are primarily observability,
failure-injection/stress evidence, lifecycle behavior, and documentation—not a
reason to move application policy into the kernel.

No nonzero output has been authorized or tested.

## Architecture retained

- User space owns device matching policy, ESI parsing, recipes, application
  meaning, and process-data interpretation.
- The kernel owns the EtherLab master/configuration/domain lifecycle, the
  deterministic cyclic loop, copied image exchange, low-level health, and the
  output safety gate.
- Configuration is per-open, transactional before apply, and immutable while
  active.
- One controller owns master 0. Close is the final synchronous unwind.
- Process image exchange is copied and bounded to 64 KiB; no mmap lifetime is
  exposed.
- Output publication uses IOD-compatible domain-sized data and per-bit masks,
  intersected with a kernel-derived output mask.
- Arm requires exact generation and latest publication sequence. Fault or
  manual disarm requires a newer publication. Disarm/deactivation waits for a
  zero-gated send acknowledgement.

## Hardware evidence

- Exact target: Debian `6.1.0-49-rt-amd64`, EtherLab DKMS 1.6.9.
- Position 29 declarative velocity PDO layout: 28 bytes, WC 3/complete, OP.
- Distributed clocks: valid reference reads and monitor results across
  repeated motion-inhibited runs.
- Copied snapshots: live TxPDO bytes with all RxPDO/output bytes zero.
- Disarmed all-ones publication: sequence accepted while transmitted output
  bytes remained zero.
- Servo-supply power loss/restoration: transport continued, recovered position
  29 to OP/complete WC, and retained `rearm_required` without restart.
- Zero-only arm gate: arm, synchronous disarm, stale-sequence rejection,
  fresh-sequence re-arm, and final disarm passed.

## Kernel-safety acceptance gate

| Plan requirement | Status | Evidence or blocker |
|---|---|---|
| Repeated load/unload | Partial | Earlier probe/device repetitions passed; repeat API 0.9 active lifecycle stress. |
| Repeated master acquire/release | Pass for earlier stages | 120 open/scan/close iterations and continued CLI usability. Repeat with current cyclic API. |
| Repeated scan memory stability | Partial | Functional repetitions passed; no allocator/leak instrumentation. |
| Invalid ABI calls | Partial | Broad ABI suite passes; add active stale generation/size/sequence cases and fuzzed counts. |
| Allocation failure paths | Not passed | Add deterministic fault injection and exercise every activation buffer/allocation unwind. |
| Configuration create/destroy stress | Not passed | Add high-count repeated pending/apply/domain/close test and leak observation. |
| Cyclic start/stop stress | Partial | Earlier short repetitions passed; API 0.9 gate/buffer/thread lifecycle needs repetition. |
| SDO failure teardown | Partial | Abort/error handling tested; allocation/close interruption stress remains. |
| Unload with resources | Pass by design, needs current test | `file_operations.owner` blocks normal unload; explicitly test current API with an open/active fd. |
| kmemleak/equivalent | Not passed | Target procedure and results required. |
| KASAN/KFENCE | Not passed | Requires a suitable test kernel/environment. |
| lockdep | Not passed | Requires a lockdep-enabled kernel and lifecycle/load tests. |
| EtherLab usable after release | Pass | CLI repeatedly reports idle master and full topology after teardown. |

The kernel-safety gate is not closed.

## Functional and safety gaps

1. **Per-slave validity is missing.** Aggregate configured-slave counts and a
   global fault mask cannot tell user space which entry data is stale. Add a
   generation-bound per-configured-slave status query before IOD integration.
2. **Controller-death behavior needs explicit proof.** Kill a standalone
   controller while active/zero-armed, verify close synchronously sends zero,
   joins the thread, releases master 0, and leaves no task or warning beyond
   the known EtherLab/ED3L stop boundary.
3. **Current cyclic lifecycle stress is incomplete.** Repeat API 0.9
   configure/activate/zero-arm/disarm/deactivate/close, checking tasks, CLI,
   counters, and new kernel diagnostics.
4. **EtherLab public deactivation remains imperfect.** ED3L can report AL
   `0x001b` before EtherLab's asynchronous PREOP transition. The application
   now sends zero before stopping, but the watchdog/idle-FSM boundary remains.
5. **Fault detail currently records the first fault of an epoch.** The power
   test latched domain-incomplete before later offline/not-operational states.
   Decide whether the UAPI needs first cause, accumulated causes, or both.
6. **Nonzero output is untested.** A bounded commissioning output requires a
   separate reviewed procedure, selected harmless output/object, physical
   observation, rollback, and explicit authorization.
7. **No comparable timing acceptance exists.** Functional runs are short and
   mostly unloaded. Establish latency/WC/DC criteria and test under declared
   CPU and system load.
8. **Manual EtherLab build compatibility is untested.** Only the exact DKMS
   target has evidence.

## Documentation gate

The detailed UAPI, process-image, DC, testing, and DKMS-environment documents
contain useful evidence. At review start, however, `README.md`,
`docs/architecture.md`, and `docs/safety-and-failure-behaviour.md` still
described early phases and therefore failed the plan's documentation gate.
They must be updated before integration.

The repository also still needs an end-to-end standalone operator sequence
covering build, load, configuration, zero-output cycling, status inspection,
safe teardown, and uninstall. Memory/debug-kernel procedures must distinguish
commands that were actually run from procedures that remain untested.

## Next work order

1. Add generation-bound per-configured-slave status and document entry
   validity semantics.
2. Add current API 0.9 lifecycle/controller-death stress tooling.
3. Add deterministic allocation-failure tests and configuration stress.
4. Run available kmemleak/KFENCE/lockdep procedures, recording unsupported
   facilities explicitly.
5. Finish end-to-end standalone documentation and repeat the gate review.
6. Only then decide on a bounded nonzero commissioning test.
7. Do not begin IOD integration until both acceptance gates pass.
