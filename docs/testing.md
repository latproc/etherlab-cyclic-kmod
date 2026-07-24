# Testing

## Build checks

```sh
make check-build-env
make
git diff --check
```

The build uses the target kernel tree, EtherLab `ecrt.h`, and exact matching
DKMS `Module.symvers`.

## Minimal acquisition probe

With IOD stopped:

```sh
sudo CW_EC_TEST_REPEAT=10 tools/cw_ec_test_master.sh
```

On 2026-07-24 this passed ten load/acquire/release/unload iterations. Master 0
remained available with 29 slaves and link up.

With IOD running, one probe load was rejected with `EBUSY`; IOD retained an
active Operation-phase master with 29 slaves and link up.

## Phase 2 discovery

With IOD stopped:

```sh
sudo tools/cw_ec_test_bus.sh
```

The script:

1. snapshots `ethercat slaves -v`;
2. loads `cw_ethercat.ko`;
3. runs malformed ABI and exclusive-open checks;
4. retrieves topology through `cw_ec_bus`;
5. closes and unloads the module;
6. confirms the CLI topology is unchanged;
7. compares position/vendor/product/revision for every slave.

Observed result on 2026-07-24:

```text
PASS: second control open returned Device or resource busy
PASS: unknown ioctl returned Inappropriate ioctl for device
PASS: short slave structure returned Invalid argument
PASS: wrong API major returned Protocol not supported
PASS: invalid slave position returned No such file or directory
PASS: setup add before begin returned Invalid argument
PASS: setup scalar length mismatch returned Invalid argument
PASS: duplicate setup sequence returned File exists
PASS: apply empty setup batch returned Invalid argument
PASS: zero-length SDO upload returned Invalid argument
PASS: zero-index SDO upload returned Invalid argument
PASS: 34 slave identities match; CLI topology unchanged
```

An additional 100 open/scan/close iterations completed, followed by 20
instrumented iterations that added no kernel warning/error. Master 0 was idle
and available afterward. This is lifecycle smoke testing, not kmemleak,
KASAN/KFENCE, or fault-injection evidence.

## Phase 3 commissioning SDO

Read-only upload:

```text
EtherLab CLI: 0x03
kernel UAPI:  03
```

for ED3L position 29 object `0x6060:00`.

The legacy recipe was applied to all five ED3Ls and every final mapping object
was verified. The legacy script ignored aborts from unnecessary zero-entry
writes. A cleaned strict 21-write recipe was then applied through the kernel
batch to position 29; all writes succeeded and final readback matched the
velocity PDO layout.

See `docs/ed3l-pdo-configuration-test.md` for exact values and remaining
persistent/declarative decision-gate tests.

## API 0.3 pending-configuration validation

On 2026-07-24, the API 0.3 module built against the target kernel/EtherLab
artifacts, loaded with IOD stopped, and passed the extended
`cw_ec_abi_test`. The new cases covered:

```text
PASS: config add before begin returned Invalid argument
PASS: duplicate config slave ID returned File exists
PASS: validate orphan config sync returned No such file or directory
PASS: valid config hierarchy accepted
PASS: config mutation after validation returned Invalid argument
```

The test used fictitious pending metadata only. It did not apply an EtherLab
slave configuration, activate the master, or write any slave. The module
unloaded cleanly and the subsequent five-minute kernel warning/error window was
empty.

The following no-activation construction test then passed:

```text
PASS: unsupported revision constraint returned Invalid argument
PASS: validated config hierarchy applied to EtherLab
PASS: duplicate config apply returned Invalid argument
PASS: config begin after apply returned Device or resource busy
```

The hierarchy addressed a deliberately absent fictitious slave. EtherLab
configuration objects were constructed and released on close, but the master
was not activated and no mailbox or process-data traffic was sent. Module
unload again left the recent kernel warning/error window empty.

The offline domain-registration extension then passed:

```text
PASS: domain created with one registered entry
PASS: stable entry ID resolved to domain offset
PASS: unknown entry ID returned No such file or directory
PASS: duplicate domain create returned Invalid argument
```

The first run exposed and then fixed an error-path bug where an unknown entry
ID retained the successful structure-header result. The full suite passed
after the fix. The deliberately absent slave allowed domain layout and cleanup
to be tested without activation or bus traffic; recent kernel warnings/errors
remained empty after unload.

## ED3L declarative no-activation preparation

The standalone `cw_ec_config` tool and
`tools/configs/ed3l_velocity_pos29.conf` fixture were tested on 2026-07-24.
Syntax checking reported one slave, two Sync Managers, two PDOs, and ten
entries. With IOD stopped, `prepare` constructed the configuration and domain
and returned:

```text
0x6040:00 offset=0  length=16
0x60ff:00 offset=2  length=32
0x6083:00 offset=6  length=32
0x6084:00 offset=10 length=32
0x60e0:00 offset=14 length=16
0x60e1:00 offset=16 length=16
0x6041:00 offset=18 length=16
0x606c:00 offset=20 length=32
0x6077:00 offset=24 length=16
0x603f:00 offset=26 length=16
```

The stable IDs in this fixture encode `(index << 8) | subindex`; that is a
user-space convention, not a kernel requirement. The master was not activated,
no configuration was sent to the drive, and recent kernel warnings/errors
remained empty after release and unload.

## API 0.4 cyclic preflight

Build and syntax checks:

```sh
make -j2
./tools/cw_ec_config check tools/configs/ed3l_velocity_pos29.conf
```

The no-activation ABI suite also verifies inactive status, inactive
deactivation rejection, cycle periods below/above the hard limits, and
unsupported activation flags.

The bounded hardware command is:

```sh
./tools/cw_ec_config cycle \
  tools/configs/ed3l_velocity_pos29.conf 1000000 10
```

Do not run it while IOD owns master 0. Motion must be safely inhibited. This
activation applies the declarative PDO mapping to position 29, zeroes the
domain before its first send, pumps for ten seconds, prints timing/send
counters, stops and joins the thread, and deactivates EtherLab. Verify mapping
readback, AL/working-counter state, EtherLab CLI recovery, and kernel warnings
afterward.

Hardware evidence on 2026-07-24:

- position 29 was directly observed in OP during a 15-second run;
- the 28-byte domain reported working counter 3 and `EC_WC_COMPLETE`;
- 14,999 cycles completed with zero EtherLab API errors and zero full-period
  overruns; maximum observed wake latency was 334,976 ns;
- typed SDO readback exactly matched the six-entry `0x1600` and four-entry
  `0x1a00` fixture, assigned through `0x1c12` and `0x1c13`;
- the master returned idle with link up and all 34 slaves visible.

Five short runs each maintained complete working counter and zero cycle API
errors. Four had no overrun; one non-RT-scheduled run had one 1,889,557 ns
wakeup. This is functional cycle evidence, not deterministic scheduling
acceptance.

Rapid stop/close/reopen is not yet a clean lifecycle acceptance result.
EtherLab deactivation restarts its idle state machine and requests PREOP
asynchronously after application traffic stops. Immediate reacquisition can
collide with pending AL-state datagrams, and an output Sync Manager watchdog
enabled in the fixture may expire during that gap. The target public API has
no application operation to request OP→PREOP while cyclic traffic continues.
Repetition testing must wait for idle/PREOP settlement, and the production
lifecycle must define how that settlement is reported rather than relying on
arbitrary sleeps.

The kernel-side five-second settled-state poll was then tested with five
immediate two-second sessions and no user-space delay. Every run completed
2,000 cycles with WC 3/complete, zero cycle API errors, and zero overruns. No
new unmatched datagram or failed/skipped AL-state datagram was logged, proving
that immediate reacquisition no longer races the unfinished transition.

Four of those five intentional stops still produced ED3L AL status `0x001b`
(`Sync manager watchdog`). The watchdog is enabled in the realistic output
fixture, application datagrams necessarily stop before the public
`ecrt_master_deactivate()` implementation asynchronously requests PREOP, and
the public API exposes no earlier state request. Settled reuse is therefore
proven, but warning-free watchdog-enabled deactivation is not.

## API 0.5 DC configuration preflight

With IOD stopped, API 0.5 was built against the exact target kernel and
EtherLab artifacts. Both the unchanged non-DC fixture and the separate ED3L DC
fixture passed standalone syntax validation. The non-activating ABI suite
passed, including:

- DC addition before a configuration transaction;
- zero AssignActivate and zero SYNC0-cycle rejection;
- explicit reference policy without a reference slave ID;
- an orphan DC slave reference rejected at transaction validation;
- all earlier malformed, hierarchy, domain, stable-entry, and inactive-cycle
  checks.

The suite applied only the existing fictitious absent-slave hierarchy. It did
not activate EtherLab, configure a physical slave, issue an SDO, or enable
distributed clocks. The module was unloaded afterward. Existing historical
EtherLab warnings remained in the retained kernel log; this run did not
capture a before/after log cursor, so it is not claimed as a strict
no-new-warning comparison.

## API 0.5 DC hardware smoke test

With motion safely inhibited, the separate ED3L DC fixture configured position
29 with `AssignActivate 0x0300`, 1,000,000 ns SYNC0, zero shift, and automatic
reference selection. A five-second run completed:

- 5,000 cycles, WC 3/complete;
- zero cycle API errors and zero full-period overruns;
- 83,781 ns maximum scheduling lateness;
- valid reference with zero reference-read errors/resumptions;
- five successful synchrony-monitor results and zero timeouts;
- 4,095 ns maximum observed slave deviation.

Three immediate three-second repetitions each completed 3,000 cycles with WC
complete, zero cycle errors/overruns, valid reference, three monitor results,
and zero read errors/timeouts. Maximum scheduling lateness was 65,220 ns,
55,516 ns, and 154,930 ns. Short-run maximum synchrony deviation was
1,507,327 ns, 1,404,927 ns, and 73,471 ns respectively, showing that initial
convergence can dominate a short sample. These figures are functional smoke
evidence, not deterministic timing acceptance.

After every close the master returned Idle with 34 slaves and link up. The
first stop reported that position 29 had reached SAFEOP+ERROR before the idle
transition observed it; each repetition then logged ED3L AL `0x001b` Sync
Manager watchdog. This is the already documented public-deactivation
limitation and remains unresolved.

## API 0.6 health-status smoke test

The non-activating ABI suite passed generation-bound inactive IO status checks.
A subsequent three-second position-29 DC run reported:

```text
generation=2 healthy=1 armed=0 rearm_required=0
faults=0x00000000 latched=0x00000000 fault_count=0
link=1 responding=34 configured=1 online=1 operational=1
```

The same run completed 3,000 cycles with WC 3/complete, zero cycle errors, and
zero overruns. Outputs remained permanently zero/disarmed. A deliberate
power-loss/restoration test is still required to validate the unhealthy
transition and sticky re-arm latch.

## API 0.7 copied-snapshot smoke test

The non-activating ABI suite rejected unsupported snapshot flags and snapshot
requests while inactive. The first three-second hardware attempt published
3,000 coherent snapshots but position 29 remained non-operational with WC zero;
the returned image was therefore all zero. This was not counted as a pass.

A five-second motion-inhibited retry reached OP and reported:

```text
cycles=4991 errors=0 overruns=1 maximum_lateness=8136627 ns
wc=3 wc_state=2
generation=3 healthy=1 armed=0 faults=0x00000000
input sequence=4992 cycle=4992 size=28
data=00000000000000000000000000000000000008160000000000004700
```

The first 18 bytes are the configured RxPDO/output region and remained zero.
The final 10 bytes contain the live TxPDO/input region. The run validates
coherent copied exchange and generation/sequence reporting, but its single
8.1 ms scheduling overrun means it is not timing-acceptance evidence.

## API 0.8 disarmed-output publication smoke test

The non-activating ABI suite rejected unsupported output-publication flags and
publication while inactive. A five-second motion-inhibited position-29 run
then published all-ones data with an all-ones update mask against generation 2.
The kernel reported output sequence 1 while `outputs_armed` remained false.

```text
cycles=5000 errors=0 overruns=0 maximum_lateness=55608 ns
wc=3 wc_state=2
generation=3 healthy=1 armed=0 faults=0x00000000
published output sequence=1
snapshot sequence=5010 cycle=5010 size=28
data=00000000000000000000000000000000000008160000000000004700
```

The first activation after loading the corrected ABI remained non-operational
with WC zero; the established retry reached OP and produced the evidence above.
All 18 configured output bytes remained zero after publication; the final 10
input bytes contained live drive data. This proves publication sequencing and
the independent hard-zero cyclic gate. The internal ownership-mask merge is
not observable on the bus until an arm test and is therefore not yet claimed
as hardware-proven.

## Power-loss recovery and re-arm latch

With API 0.8 cycling position 29 at 1 ms and outputs permanently disarmed, the
servo supply was deliberately removed for about five seconds and restored.
The same process continued running for 90 seconds and finished with:

```text
cycles=90000 overruns=0 wc=3 wc_state=2
cycle errors=29063
DC read errors=29065 reference resumptions=2
generation=1 healthy=1 armed=0 rearm_required=1
current faults=0x00000000 latched faults=0x00000020 fault_count=1
link=1 responding=34 configured=1 online=1 operational=1
```

`0x20` is `CW_EC_IO_FAULT_DOMAIN_INCOMPLETE`, the first unhealthy condition
observed. The drive recovered to OP and complete WC without restarting the
transport, but the re-arm requirement remained sticky. The cycle/DC error
counters captured the unavailable/reconfiguration interval. The final snapshot
contained live input data and all configured output bytes remained zero.

## API 0.9 zero-output arm/disarm gate

The inactive ABI suite rejected unsupported arm/disarm flags and both
operations while inactive. A five-second position-29 run then published an
all-zero shadow and exercised the live gate:

```text
cycles=4999 errors=0 overruns=0 maximum_lateness=112015 ns
wc=3 wc_state=2
generation=2 healthy=1 armed=0 rearm_required=0
published zero output sequence=1
zero sequence 1 armed
synchronous disarm acknowledged
stale sequence 1 rejected with EAGAIN
fresh zero sequence 2 published and armed
final synchronous disarm acknowledged
```

The final 28-byte snapshot retained zero in all 18 output bytes and live data
in the 10 input bytes. This validates exact generation/latest-sequence arm,
bounded cyclic disarm acknowledgement, and fresh-publication recovery without
requesting or transmitting a nonzero output. A nonzero commissioning output
has not been authorized or tested.

## API 0.10 configured-slave validity

The non-activating ABI suite rejected a stale configuration generation and an
unknown slave `config_id`, then reported the known configured slave inactive
with `state_result=-ENODATA` and `data_valid=0`.

A five-second motion-inhibited position-29 run reported:

```text
cycles=5000 errors=0 overruns=0 wc=3 wc_state=2
slave id=1 active=1 online=1 operational=1
data_valid=1 al_state=0x08 state_result=0
status cycle=5000 input_sequence=5000
```

The query is keyed by the stable user-supplied ID and exact configuration
generation. `data_valid` requires the individual slave online/operational plus
complete domain WC and a published snapshot.

A later 90-second `cycle-monitor` run power-cycled position 29 in the same
active, disarmed session. The observed transitions were:

```text
healthy=1 rearm_required=0 faults=0x00000000 online=1 operational=1 valid=1 al=0x08
healthy=0 rearm_required=1 faults=0x00000020 online=1 operational=1 valid=0
healthy=0 rearm_required=1 faults=0x00000038 online=0 operational=0 valid=0 al=0x00
healthy=1 rearm_required=1 faults=0x00000000 online=1 operational=1 valid=1 al=0x08
```

The transport was not restarted, outputs remained disarmed, the fault count
was one, and the final status retained latched fault `0x20` while current
faults cleared. This validates conservative per-slave invalidation and
recovery without silently clearing the explicit re-arm requirement.

That first capture also exposed that the latched mask retained only the first
cyclic sample. After changing it to accumulate causes for the entire re-arm
epoch, the same disarmed power-cycle procedure observed:

```text
current 0x20: domain incomplete, slave still online/OP, data invalid
current 0x38: domain incomplete, slave offline/not operational
current 0x30: domain incomplete, slave online/not operational in PREOP
current 0x10: slave online/not operational in SAFEOP
current 0x00: slave online/OP/valid again
final latched=0x00000038 fault_count=1 rearm_required=1 armed=0
```

The run completed 90,974 cycles with zero overruns. Cycle errors and DC read
errors increased while the drive was deliberately absent, as expected. After
close/unload, master 0 was idle/inactive with all 34 slaves visible. The kernel
log contained ED3L emergency requests, the known stop-path Sync Manager
watchdog, and EtherLab scan errors during the deliberate disruption; this test
does not claim a warning-free power cycle.

## API 0.10 lifecycle repetition

`tools/cw_ec_test_cycle_lifecycle.sh` requires the explicit
`CW_EC_MOTION_INHIBITED=YES` acknowledgement and repeats the complete
configure/activate/zero-publish/arm/disarm/deactivate/close lifecycle. It
checks the cyclic task count after every iteration, requires the EtherLab
master to be idle/inactive after close, compares topology before/after module
unload, and reports new kernel warning/error lines.

Five two-second iterations passed on the target:

```text
PASS: 5 API lifecycle iteration(s); no cyclic task leak; topology unchanged
New kernel warning/error lines: none
```

A later 20-iteration run initially exposed a tool race: iteration 6 attempted
zero-arm before the rapidly reconfigured drive was healthy and correctly
received `EAGAIN`. `cycle-zero-arm` now uses the same bounded five-second
health prerequisite as `cycle-zero-hold`. The corrected run passed:

```text
PASS: 20 API lifecycle iteration(s); no cyclic task leak; topology unchanged
New kernel warning/error lines: none
```

Master 0 was idle/inactive and the cyclic task count matched baseline after
every iteration.

## API 0.10 active hostile-input checks

`cw_ec_config cycle-abi` activates a configured domain but never arms outputs.
It requires a healthy bus, then verifies undersized snapshot capacity, bad
flags, stale publish/arm/disarm generations, wrong output size, unknown output
sequence, and duplicate activation return their documented errors.

The position-29 test initially found the drive in `SAFEOP+ERROR` following the
earlier deliberate power cycle. Declarative activation recovered it to OP and
the retry passed every check:

```text
cycles=6405 errors=0 overruns=0 wc=3 wc_state=2
healthy=1 armed=0 rearm_required=0
slave id=1 online=1 operational=1 valid=1 al=0x08
```

Close/unload returned master 0 idle/inactive with 34 slaves.

## API 0.11 PDO padding and servo-off startup

The installed `Wooltech_EL5152.xml` and live position-3 PDO scan contain
multiple mandatory `0x0000:00` gap entries. API 0.11 assigns these
`entry_id=0`: they retain order/bit length in EtherLab configuration but are
not registered or returned as user process entries. The non-activating ABI
suite proved an 8-bit gap shifts the following real entry to byte offset 1.

The XML-derived mixed fixture at
`tools/configs/el5152_pos3_with_absent_ed3l_pos29.conf` prepared successfully
with only 29 slaves present. Its real EL5152 entries occupied bytes 0–31 and
the expected absent ED3L reserved bytes 32–59.

A five-second disarmed cycle reported:

```text
cycles=5000 errors=0 overruns=0 maximum_lateness=47445 ns
wc=3 wc_state=1 healthy=0 armed=0 faults=0x38
EL5152: online=1 operational=1 al=0x08
ED3L:   online=0 operational=0 al=0x00
```

The 60-byte snapshot contained nonzero EL5152 counter/frequency data while the
ED3L region remained zero. Both `data_valid` values remained false because
EtherLab reports WC completeness for the combined domain. This proves
servo-off startup and continued present-device exchange, but also proves a
single domain cannot independently certify unaffected data. The safe next
architecture is separate domains for independently recoverable groups, not
removing the complete-WC validity condition.

## Cyclic scheduler controls

The module-load parameters `cycle_cpu` and `cycle_fifo_priority` are immutable
after load. Defaults `-1` and `0` preserve scheduler affinity and normal
scheduling. The task is created stopped; requested affinity and policy are
applied before its first cycle.

A disarmed position-29 run loaded with CPU 1 and FIFO priority 70. Live process
inspection reported `cw_ec_cycle` as class `FF`, RT priority 70, on CPU 1:

```text
cycles=31109 errors=0 overruns=0 maximum_lateness=51293 ns
wc=3 wc_state=2 healthy=1 armed=0
```

An invalid CPU 99 was rejected with `EINVAL` before cyclic activation. Close
and unload returned master 0 idle/inactive with 34 slaves. This establishes
the scheduler foundation but is not baseline-versus-load timing acceptance.

## Zero-armed controller death

`tools/cw_ec_test_controller_death.sh` starts `cycle-zero-hold`, waits until an
all-zero output shadow is explicitly armed, then sends `SIGKILL` to the
controller. It verifies kernel file-release teardown rather than allowing the
tool to issue its normal disarm/deactivate calls.

Before the kill, it attempts normal `rmmod cw_ethercat` and requires failure
while the control fd, cyclic task, and master ownership are live. The
controller must remain running and the module must remain loaded. The target
test passed:

```text
Active control file correctly blocked module unload
PASS: killed zero-armed controller; no cyclic task leak;
      master released; topology unchanged
New kernel warning/error lines: none
```

The same fixture also completed its normal two-second hold path: the bus was
healthy with the configured slave online and operational, the zero shadow
remained armed through 3251 cycles, synchronous disarm succeeded, the input
snapshot advanced, and close/deactivation returned master 0 to idle/inactive.

After process death, master 0 reported idle/inactive, the cyclic task count
matched its baseline, module unload succeeded, and the EtherLab topology was
unchanged. This proves the basic controller-death path with an armed zero
shadow; repeated/instrumented death stress remains part of the broader safety
gate.

## Deterministic allocation failure

The target kernel reports `CONFIG_FAULT_INJECTION`, `CONFIG_DEBUG_KMEMLEAK`,
`CONFIG_KFENCE`, and `CONFIG_PROVE_LOCKING` disabled. The module therefore has
a test-only, read-only `test_fail_allocation=N` parameter. Its default is zero,
which adds no failure. A positive value fails exactly the Nth module-owned
allocation.

`tools/cw_ec_test_allocation_failures.sh` reloads the module for each failure
point. It covers the file context and all allocations reached by:

- staging the 21-write ED3L recipe without applying it; and
- preparing the one-slave/two-Sync/two-PDO/ten-entry/DC declarative fixture
  with its implicit domain, without activation; and
- preparing the explicit two-domain EL5152/ED3L fixture without activation.

The target test passed all 107 injected failures plus the first success
boundary of each path:

```text
New kernel warning/error lines:
  none
PASS: 107 injected allocation failures unwound; success boundaries passed;
      topology unchanged
```

API 0.12 extends this series through the implicit compatibility-domain
allocation and every explicit-domain fixture record: two domains, two
assignments, two slaves, four Sync Managers, eight PDOs, and 48 entries. The
first run exposed the new implicit-domain allocation at the former success
boundary; after correcting that boundary, all 107 injected failures and all
three success boundaries passed with no new kernel warning/error.

Every failed operation closed its control file, master 0 returned
idle/inactive, module unload succeeded, and the final topology matched the
initial capture.

The six copied process-image allocations were then tested separately using the
motion-inhibited ED3L position-29 fixture. In API 0.12, allocations 19 through
24 (two input
images, two output images, the output ownership mask, and the update mask)
each returned `ENOMEM` before master activation and unwound to an idle master.
The separate cyclic-task construction hook then returned `ENOMEM` after
EtherLab activation; that path deactivated the master, freed all copied
buffers, invalidated EtherLab-owned configuration/domain pointers, and closed
cleanly. Allocation 25, immediately beyond the owned allocation paths,
completed an eight-second zero-output cycle:

```text
cycles=8000 errors=0 overruns=0 maximum_lateness=75494 ns
wc=3 wc_state=2
healthy=1 armed=0 faults=0x00000000
configured=1 online=1 operational=1
New kernel warning/error lines:
  none
PASS: all six process-image and cyclic-task construction failures unwound;
      success boundary passed; topology unchanged
```

This is allocation/unwind evidence, not a timing-acceptance result. No nonzero
output was requested.

The same harness was then parameterized for the explicit two-domain
EL5152/ED3L fixture. Its 67 fixture-specific allocations precede the copied
images. Injected failures 68 through 73 covered both input images, both output
images, the output ownership mask, and the update mask; each unwound before
activation. Cyclic-task construction failure also unwound after activation.
Allocation 74 completed the eight-second disarmed success run:

```text
cycles=8000 errors=0 overruns=0 maximum_lateness=102428 ns
domain 1: base=0  size=32 wc=3 complete valid=1
domain 2: base=32 size=28 wc=3 complete valid=1
healthy=1 armed=0 faults=0x00000000
New kernel warning/error lines:
  none
PASS: all six process-image and cyclic-task construction failures unwound;
      success boundary passed; topology unchanged
```

Use `CW_EC_PRE_IMAGE_ALLOCATIONS` when exercising another fixture; it is the
number of module-owned allocations through the last pending configuration
record, before activation allocates copied process images.

## Maximum pending configuration stress

`tools/cw_ec_test_config_stress.sh` defaults to ten iterations. Each iteration
constructs and resets:

- 256 pending ordered setup SDOs;
- 256 slaves;
- 1024 Sync Managers;
- 4096 PDOs;
- 16384 PDO entries;
- 256 distributed-clock records;
- 256 domain declarations; and
- 256 slave-to-domain assignments.

For each collection, the tool also submits one extra valid record and requires
`E2BIG`. It does not validate, apply, or activate the synthetic hierarchy.

The ten-iteration target run passed:

```text
PASS: 10 maximum pending configuration iteration(s)
New kernel warning/error lines:
  none
PASS: 10 maximum pending configuration iteration(s);
      master idle; topology unchanged
```

The API 0.12 ten-iteration rerun exercised 227,840 pending record allocations
plus synchronous
list teardown. It is limit and lifetime evidence, not evidence that the
synthetic hierarchy represents valid EtherCAT hardware.

Lifecycle scripts compare a stable topology projection: physical master/slave
order plus vendor ID, product code, revision, and serial. They deliberately do
not compare volatile AL state or DC receive timestamps. Operational recovery is
checked separately through runtime status where the test requires it.

## Staged install/uninstall

`make install` and `make uninstall` were exercised with a temporary `DESTDIR`.
The install created:

```text
lib/modules/6.1.0-49-rt-amd64/extra/cw_ethercat/cw_ethercat.ko
lib/modules/6.1.0-49-rt-amd64/extra/cw_ethercat/cw_ethercat_probe.ko
```

Both files had mode 0644. The matching staged uninstall removed both files.
This validates target paths and scope without modifying the live module tree;
live `depmod` behavior was not exercised.

## Phase 2 contention

On 2026-07-24, with IOD owning master 0, `cw_ethercat.ko` registered its device
without claiming the master. `cw_ec_bus` then failed to open the device with
`EBUSY` and reported that master 0 was already owned. After module unload, IOD
still held master 0 in active Operation phase with 29 slaves and link up.

This closes the Phase 2 contention check: registering the transport module does
not disturb the direct backend, and attempted control ownership fails cleanly.

## API 0.12 explicit-domain smoke test

With 34 slaves present and motion inhibited, the mixed EL5152 position-3 /
ED3L position-29 fixture explicitly assigned each slave to a separate domain.
A five-second disarmed run reported:

```text
domain 1: base=0  size=32 wc=3 complete valid=1
domain 2: base=32 size=28 wc=3 complete valid=1
cycles=5702 errors=0 overruns=0 maximum_lateness=54376 ns
aggregate wc=6 complete
```

Returned entry offsets for the ED3L began at byte 32, proving that the public
offset namespace is the ordered concatenation of the two EtherLab domains.
The final 60-byte snapshot contained live data in both segments. No output was
armed and the output shadow publication occurred only after measurement.

The unchanged `ed3l_velocity_pos29.conf`, which contains no domain records,
then exercised the implicit compatibility domain for two seconds:

```text
cycles=2000 errors=0 overruns=0 maximum_lateness=77841 ns
wc=3 complete; slave online=1 operational=1 valid=1
```

The non-activating hostile ABI suite also rejected an explicit domain without
an assignment and an assignment referencing an unknown domain. It retained
the existing legacy-domain offset and padding checks.

An active, disarmed `cycle-abi` run against the same explicit fixture rejected
stale-generation, unknown-domain, and nonzero-reserved-field domain-status
queries. A valid query reported both domains active and valid. Existing active
snapshot, publication, arm/disarm, and duplicate-activation hostile checks
also passed, outputs remained disarmed, and the 2.7-second run completed with
2,701 cycles, zero errors/overruns, and 150,479 ns maximum lateness.

The actual topology was 34 slaves, not the intended servo-off 29-slave state,
so independent validity across an absent drive is still outstanding. The
known EtherLab asynchronous deactivation limitation produced Sync Manager
watchdog messages for ED3L position 29 and EL5152 position 3; master 0
nevertheless returned idle and inactive after both runs.

## API 0.12 multi-domain lifecycle and controller death

The explicit EL5152/ED3L two-domain fixture was exercised through five
consecutive `cycle-zero-arm` sessions at 1 ms for two seconds each. Every
session published and armed only an all-zero output image, synchronously
disarmed, stopped its cyclic task, and returned master 0 idle/inactive.
After all five iterations:

```text
New kernel warning/error lines:
  none
PASS: 5 API lifecycle iteration(s); no cyclic task leak; topology unchanged
```

The same explicit fixture then ran `cycle-zero-hold`. While its all-zero image
was armed, normal module unload was correctly rejected. Killing the controller
exercised file-release teardown; the cyclic task terminated, master 0 returned
idle/inactive, module unload succeeded, topology was unchanged, and no new
kernel warning/error appeared:

```text
PASS: killed zero-armed controller; no cyclic task leak; master released;
      topology unchanged
```

These tests prove multi-domain lifetime/unwind behavior with two configured
domains; the ABI limit is 256 domains and is separately covered by the
maximum-count reset test.

## Bounded nonzero console-output commissioning

With servo three-phase power absent, the E-stop circuit pressed, and other
machine outputs without E-stop power, the core-console EL2034 at position 15
was configured alone. Its zero-only test reached OP with complete WC and valid
data, then passed zero arm/disarm and stale-sequence checks.

`pulse-entry` requires `CW_EC_NONZERO_OUTPUT_AUTHORIZED=YES`. It resolves the
stable entry ID, proves the entry is uniquely linked through a PDO to an
output Sync Manager, requires a one-bit entry, and publishes an update mask
containing only that bit. A diagnostic input was rejected before opening the
device.

```text
status indicator 0x7010:01: entry 0x701001, offset 0, bit 1, 1000 ms
buzzer           0x7000:01: entry 0x700001, offset 0, bit 0,  300 ms
PULSE: synchronously disarmed; output returned to zero
```

Both software commands returned success. Physical LED/audible observation was
not reported, so it remains unconfirmed. This evidence does not validate an
actuator, drive-enable, or motion output.

A separate controller-death test started a five-second status-indicator pulse
and terminated the controller after 1.5 seconds. Kernel file release gated the
output and stopped the cyclic task; master 0 returned idle/inactive, normal
module unload succeeded, all 34 slaves remained visible, and no new kernel
warning/error appeared.
