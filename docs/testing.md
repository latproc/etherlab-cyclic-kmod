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
PASS: 29 slave identities match; CLI topology unchanged
```

An additional 100 open/scan/close iterations completed, followed by 20
instrumented iterations that added no kernel warning/error. Master 0 was idle
and available afterward. This is lifecycle smoke testing, not kmemleak,
KASAN/KFENCE, or fault-injection evidence.

## Remaining Phase 2 test

Load `cw_ethercat.ko` while IOD owns master 0, then run `cw_ec_bus`. Module load
must succeed because it does not claim the master; tool open must fail with
`EBUSY`; IOD must remain active and unchanged. This exact character-device
contention path remains to be recorded.
