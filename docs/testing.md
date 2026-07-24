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

## Phase 2 contention

On 2026-07-24, with IOD owning master 0, `cw_ethercat.ko` registered its device
without claiming the master. `cw_ec_bus` then failed to open the device with
`EBUSY` and reported that master 0 was already owned. After module unload, IOD
still held master 0 in active Operation phase with 29 slaves and link up.

This closes the Phase 2 contention check: registering the transport module does
not disturb the direct backend, and attempted control ownership fails cleanly.
