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
