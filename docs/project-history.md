# Project History

This file holds dated implementation and validation evidence moved out of
`AGENTS.md`. The repository history and focused documents retain the exact
commands, outputs, and design details.

## 2026-07-24

- Confirmed Debian `6.1.0-49-rt-amd64`, PREEMPT_RT, and EtherLab DKMS 1.6.9.
  The exact headers, generated files, build tree, and `Module.symvers` are
  recorded in `docs/building/etherlab-dkms-environment.md`.
- Built the minimal external master probe. Ten idle acquire/release/unload
  iterations passed, and contention returned `EBUSY` without disturbing IOD.
- Added the versioned character device, discovery API, and standalone bus
  tool. All then-present 29 slaves matched EtherLab CLI identity/order; 120
  lifecycle iterations completed cleanly.
- Added bounded typed commissioning SDO support. Upload was proven on ED3L
  `0x6060:00`; a cleaned strict 21-write velocity mapping succeeded and
  readback matched.
- Confirmed from EtherLab source that PDO assignment/mapping belongs to the
  declarative PDO API, while ordinary configuration SDOs are retained and
  replayed after reconfiguration.
- Added transactional slave/Sync Manager/PDO/entry metadata, EtherLab
  configuration application, domain registration, stable entry IDs, and
  standalone configuration tooling. The position-29 ED3L fixture produced a
  contiguous 28-byte domain layout.
- Implemented and published Clockwork object selectors on LatProc
  `prod-experimental-mqtt-fix` at
  `51af5222213ea49353982dafc31c456394baf27d`. Legacy flattened `pos` and
  explicit object-selector A/B runs produced identical mappings and sampled
  values. Machine configuration was committed separately as SVN revision
  20001.
- Added API 0.4 bounded zero-output cyclic activation. ED3L position 29 reached
  OP with WC 3/complete, exact typed readback, zero cycle API errors, and a
  28-byte image. Five rapid lifecycle repetitions added no unmatched or
  failed/skipped AL-state datagrams.
- Exact EtherLab lifecycle inspection showed deactivation destroys all domain
  and slave-configuration objects. The module invalidates those pointers and
  waits for configured slaves to leave SAFEOP/OP before reuse.
- Four of five intentional ED3L stops still reported AL `0x001b` Sync Manager
  watchdog because the public deactivation API stops application traffic
  before its asynchronous PREOP request. This remains an explicit lifecycle
  limitation; the watchdog is not disabled to conceal it.
- Captured the installed IOD distributed-clock algorithm and the ED3L ESI
  values: `AssignActivate 0x0300`, SYNC0 equal to the application period, and
  zero shift.
- Added API 0.5 transactional per-slave DC metadata and automatic/explicit
  reference policy, followed by the IOD-compatible reference-led cyclic
  controller and bounded DC status snapshot. Four motion-inhibited position-29
  runs reached complete WC with valid reference reads, successful monitoring,
  and zero cycle errors/overruns. The known stop-path watchdog remains.
- Added API 0.6 generation-bound bus/configured-slave/domain health status.
  A zero-output position-29 run reported healthy with 34 responding slaves and
  the configured slave online/operational. Fault-latch validation remains.
- Added API 0.7 coherent copied, read-only domain snapshots with a 64 KiB
  activation bound and preallocated double buffering. A motion-inhibited
  position-29 run returned live TxPDO data while all RxPDO/output bytes
  remained zero. One scheduling overrun was observed, so timing acceptance
  remains open.
- Added API 0.8 generation-bound copied output publication with output-entry
  masking and IOD-compatible per-bit update masks, but no arm operation. An
  all-ones shadow was published during a
  motion-inhibited position-29 run; all configured output bytes remained zero,
  WC was complete, and 5,000 cycles completed without errors or overruns.
- Deliberately power-cycled the servo supply during a 90-second zero-output
  run. Position 29 recovered to OP/complete WC without restarting the
  transport, while `rearm_required` stayed latched with one fault epoch and
  outputs remained disarmed.
- Added API 0.9 exact-generation/latest-sequence arm and synchronous disarm.
  A zero-only position-29 test armed sequence 1, acknowledged disarm, rejected
  stale sequence 1, accepted fresh sequence 2, and disarmed again with OP,
  complete WC, and all output bytes zero.
- Completed the first formal standalone architecture review. The core
  configuration/cyclic/DC/copy/gating architecture was retained, but IOD
  integration remains blocked by per-slave validity, current lifecycle and
  controller-death stress, allocation/leak/debug-kernel testing, manual-build
  compatibility, and stale top-level documentation identified during review.
- Added API 0.10 generation-bound per-configured-slave state and conservative
  data validity. Position 29 reported online, operational, AL OP, and valid at
  cycle/input sequence 5,000 with complete WC; stale, unknown, and inactive
  queries were rejected/reported correctly.
- Added a current-API zero-only lifecycle harness. Five API 0.10 iterations
  returned the master idle after every close, leaked no cyclic task, preserved
  topology, and added no kernel warning/error line.
- Added an armed-zero controller-death harness. Killing the controller forced
  the kernel file-release path; no cyclic task leaked, master 0 returned idle,
  topology was unchanged, and no new warning/error line appeared.
- Added disabled-by-default deterministic module-allocation injection and a
  non-applying SDO staging mode. All 39 allocations reached by SDO staging and
  declarative preparation failed individually and unwound cleanly; both
  success boundaries passed without topology or kernel-log changes.
- Added a maximum pending-configuration stress tool. Ten iterations filled all
  setup/configuration hard limits, required `E2BIG` at each boundary, and
  synchronously reset more than 220,000 records without changing master idle
  state, topology, or the kernel warning/error log.
- Added stable identity-only topology capture for lifecycle tests and covered
  all six copied process-image allocation failures. Each failed before master
  activation and unwound cleanly; the next allocation boundary reached
  OP/complete WC in an eight-second zero-output cycle with no new kernel
  warning/error.
- Added deterministic cyclic-task construction failure after master
  activation. Its deeper unwind deactivated the master, freed copied images,
  invalidated EtherLab-owned pointers, and restored idle state without a new
  kernel warning/error.
- Consolidated `AGENTS.md`, corrected stale API 0.4/0.9 documentation, and
  added an end-to-end standalone operator guide covering build, discovery,
  non-activating preparation, zero-output operation, teardown, stress, and
  local cleanup.
- Added `DESTDIR`-testable install/uninstall targets and completed the API 0.10
  documentation acceptance audit. Staged installation/removal passed without
  modifying the live module tree; the kernel-safety gate remains open.
- Proved module reference protection with current active resources: normal
  unload was rejected while a zero-armed controller remained healthy, then
  controller death synchronously released the task/master and allowed unload.
- A 20-cycle zero-arm lifecycle run exposed premature arm at iteration 6.
  Requiring bounded bus health before zero-arm fixed the tool race; the
  corrected 20 iterations returned the master idle every time with no task,
  topology, or kernel-log regression.
- Added a disarmed `cycle-monitor` commissioning mode and captured API 0.10
  per-slave state through a live position-29 power cycle. Validity cleared
  before the slave went offline, returned only after online/OP recovery in the
  same active session, and `rearm_required` stayed latched with one fault
  epoch.
- Changed `last_latched_faults` from first-sample detail to the union of causes
  observed in a re-arm epoch. A repeated disarmed power cycle accumulated
  `0x38` across domain-incomplete/offline/not-operational phases, retained one
  fault transition, recovered to OP/valid without restart, and remained
  disarmed pending explicit re-arm.
- Added active, disarmed hostile-ABI checks for snapshot capacity/flags,
  publish size/generation, arm sequence/generation, disarm generation, and
  duplicate activation. Position 29 recovered from post-power-cycle
  `SAFEOP+ERROR`, passed every rejection with OP/complete WC, and returned the
  master idle after close.
- Added immutable module-load CPU-affinity and FIFO-priority controls for the
  cyclic task. The task is configured before its first wake. CPU 1 / FIFO 70
  was visible live and completed a 30-second disarmed run with zero
  errors/overruns and 51,293 ns maximum lateness; invalid CPU 99 was rejected
  before activation.
- API 0.11 added explicit unregistered `0x0000:00` PDO padding so the installed
  EL5152 XML/PDO layout can be represented exactly. The non-activating ABI
  suite and live offset preparation passed with padding omitted from stable
  entry lookup.
- A mixed present-EL5152/absent-ED3L run completed 5,000 disarmed cycles with
  zero errors/overruns. The encoder reached OP and returned live data while
  the drive remained offline. The combined domain WC stayed incomplete, so
  conservative validity correctly remained false for both. EtherLab exposes
  WC per domain, not per slave; independent validity requires separate
  domains.
- Selected the general multi-domain architecture after that result. User space
  explicitly declares availability/validity domains and slave assignments;
  the kernel does not infer grouping. Existing configurations retain one
  implicit compatibility domain. Declared domains form ordered segments in one
  copied global process image, and per-slave validity will follow the assigned
  domain's WC. The recommended machine policy is always-powered Beckhoff I/O
  separate from switchable drives, with further splits only where independent
  validity or fault containment is required.
- API 0.12 implemented explicit domain declarations and slave assignments,
  ordered domain segments in one copied image, multi-domain process/queue
  cycling, per-domain WC/status, and per-slave validity from its assigned
  domain. Configurations without domain records retain the legacy implicit
  single domain.
- A live explicit EL5152/ED3L run with all 34 slaves powered completed 5,702
  disarmed cycles with zero errors/overruns. Domain 1 occupied bytes 0-31 and
  domain 2 bytes 32-59; each reported WC 3/complete and valid data. The
  unchanged implicit ED3L fixture then completed 2,000 disarmed cycles with
  complete WC, proving compatibility. No nonzero output was armed. This did
  not prove the intended servo-off split because the observed topology
  contained all five drives.
- Hardened the API 0.12 domain ABI against unsupported flags, missing and
  unknown domains, duplicate slave assignments, stale/unknown status queries,
  and inactive implicit-domain status. Extended maximum pending stress to 256
  domain declarations plus 256 assignments and completed ten reset iterations.
  Deterministic allocation injection covered all 107 module-owned allocation
  points in setup, legacy implicit-domain, and explicit-domain preparation;
  every failure unwound to an idle master with unchanged topology and no new
  kernel warning/error.
- Repeated the lifecycle and controller-death gates with the explicit
  EL5152/ED3L domain fixture. Five zero-arm sessions each returned master 0
  idle with no cyclic task leak. A live zero-armed control file blocked module
  unload; killing it released every resource and then allowed unload. Both
  tests preserved topology and added no kernel warning/error.
- Re-ran the documentation acceptance audit for API 0.12. README,
  architecture, UAPI, process-image, safety/failure, and operator documentation
  now describe explicit domain declarations and assignment syntax, ordered
  global image offsets, per-domain validity, implicit compatibility behavior,
  and the still-global output gate. The standalone documentation gate passes;
  the kernel-safety gate remains open.
- Corrected deterministic process-image allocation indices for the API 0.12
  implicit-domain allocation. All six copied-image failures and cyclic-task
  construction failure unwound cleanly; allocation 25 completed 8,000
  disarmed cycles with zero errors/overruns, master idle after teardown,
  unchanged topology, and no new kernel warning/error.
