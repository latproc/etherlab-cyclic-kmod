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

## Phase 2 contention

On 2026-07-24, with IOD owning master 0, `cw_ethercat.ko` registered its device
without claiming the master. `cw_ec_bus` then failed to open the device with
`EBUSY` and reported that master 0 was already owned. After module unload, IOD
still held master 0 in active Operation phase with 29 slaves and link up.

This closes the Phase 2 contention check: registering the transport module does
not disturb the direct backend, and attempted control ownership fails cleanly.
