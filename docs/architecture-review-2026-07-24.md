# Standalone Architecture Review — 2026-07-24

## Decision

The standalone transport is ready for continued standalone hardening, but it
is not ready for IOD integration or production use.

API 0.10 proves the central transport shape: transactional generic
configuration, EtherLab-owned recovery, distributed-clock cycling, copied
process images, topology-derived output ownership, and an explicit
generation/sequence arm gate. The remaining gaps are primarily observability,
debug-kernel evidence, timing acceptance, and lifecycle edge cases—not a
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
- API 0.10 per-configured-slave status reported position 29 online,
  operational, and data-valid with complete WC.
- Controller death while zero-armed, all module-owned allocation/construction
  failures, and ten maximum pending create/reset iterations unwind cleanly.

## Kernel-safety acceptance gate

| Plan requirement | Status | Evidence or blocker |
|---|---|---|
| Repeated load/unload | Partial | Earlier probe/device repetitions plus 20 consecutive API 0.10 active lifecycles passed; instrumentation remains unavailable. |
| Repeated master acquire/release | Pass | 120 open/scan/close iterations, 20 current cyclic lifecycles, controller-death teardown, and continued CLI usability. |
| Repeated scan memory stability | Partial | Functional repetitions passed; no allocator/leak instrumentation. |
| Invalid ABI calls | Partial | Broad ABI suite passes; add active stale generation/size/sequence cases and fuzzed counts. |
| Allocation failure paths | Partial | Every module-owned pending/image allocation and cyclic-task constructor passed deterministic unwind; external allocations require a fault-injection kernel. |
| Configuration create/destroy stress | Partial | Ten maximum pending create/reset iterations passed; applied/domain stress still needs instrumentation. |
| Cyclic start/stop stress | Partial | Twenty API 0.10 zero-arm lifecycles passed with no task leak or new warning; instrumented stress remains. |
| SDO failure teardown | Partial | Abort/error handling tested; allocation/close interruption stress remains. |
| Unload with resources | Pass | Normal unload failed while a zero-armed control fd/task/master were live; controller continued, then file release and unload succeeded. |
| kmemleak/equivalent | Unsupported on target | `CONFIG_DEBUG_KMEMLEAK` is disabled. |
| KASAN/KFENCE | Unsupported on target | `CONFIG_KFENCE` and fault injection are disabled; use a debug kernel. |
| lockdep | Unsupported on target | `CONFIG_PROVE_LOCKING` is disabled; use a lockdep-enabled kernel. |
| EtherLab usable after release | Pass | CLI repeatedly reports idle master and full topology after teardown. |

The kernel-safety gate is not closed.

## Functional and safety findings

1. **Per-slave validity transition evidence passes.** A current API 0.10
   disarmed hardware run captured position 29 becoming invalid, then offline,
   then online/OP/valid again without restarting the transport. The explicit
   re-arm requirement remained latched after recovery.
2. **Controller-death stress needs expansion.** One standalone controller was
   killed while explicitly zero-armed; file release left no cyclic task,
   returned master 0 idle, preserved topology, and added no warning/error.
   Repeat this under leak/debug-kernel instrumentation.
3. **Current cyclic lifecycle functional stress passes 20 iterations.**
   Configure/activate/zero-arm/disarm/deactivate/close returned the master idle
   with no task leak after every iteration and no final topology/log change.
   Memory/debug-kernel instrumentation remains unavailable.
4. **EtherLab public deactivation remains imperfect.** ED3L can report AL
   `0x001b` before EtherLab's asynchronous PREOP transition. The application
   now sends zero before stopping, but the watchdog/idle-FSM boundary remains.
5. **Fault detail uses accumulated causes per re-arm epoch.** This preserves
   the domain-incomplete onset plus later offline/not-operational causes while
   keeping `fault_count` as the healthy-to-unhealthy transition count. A
   repeated disarmed hardware power cycle proved final mask `0x38` with one
   transition and recovery to OP/valid.
6. **Nonzero output is untested.** A bounded commissioning output requires a
   separate reviewed procedure, selected harmless output/object, physical
   observation, rollback, and explicit authorization.
7. **No comparable timing acceptance exists.** Functional runs are short and
   mostly unloaded. Establish latency/WC/DC criteria and test under declared
   CPU and system load.
8. **Manual EtherLab build compatibility is untested.** Only the exact DKMS
   target has evidence.

## Documentation acceptance gate

The plan's documentation gate now passes for standalone API 0.10:

| Requirement | Location |
|---|---|
| Purpose and architecture | `README.md`, `docs/architecture.md` |
| Supported EtherLab build | `docs/building/etherlab-dkms-environment.md` |
| Install, load, unload, uninstall | `docs/operator-guide.md`, Make targets |
| Discovery and standalone tests | `docs/operator-guide.md`, `docs/testing.md` |
| Lifecycle and state machine | `docs/architecture.md`, safety document |
| User-space interface | `docs/uapi.md`, process-image document |
| Safety/memory procedures | `docs/testing.md`, operator guide |
| GPL-2.0-only requirements | `README.md`, `LICENSE`, SPDX identifiers |

The install/uninstall layout was tested under a temporary `DESTDIR`: both
modules were installed mode 0644 under the target release's
`extra/cw_ethercat` directory and removed cleanly. The live module tree was not
modified. Debug-kernel procedures distinguish unsupported target facilities
from completed tests.

## Next work order

1. Capture API 0.10 per-slave invalid/recovery transitions.
2. Expand current API 0.10 lifecycle/controller-death stress under
   instrumentation.
3. **Module-controlled construction failure coverage passes.** Pending
   configuration, all six copied process-image allocations, and post-activation
   cyclic-task construction unwind cleanly. Ten full-limit create/reset
   iterations also pass. External EtherLab/internal kernel allocations still
   require a fault-injection-capable debug kernel.
4. Run available kmemleak/KFENCE/lockdep procedures, recording unsupported
   facilities explicitly.
5. Keep the passing documentation gate current as implementation changes.
6. Only then decide on a bounded nonzero commissioning test.
7. Do not begin IOD integration until the kernel-safety gate also passes.
