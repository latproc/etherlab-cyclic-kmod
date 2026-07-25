# EtherLab DKMS Environment

## Observed target

Recorded on 2026-07-24 on host `1G2C-122`.

| Item | Observed value |
|---|---|
| Kernel | `6.1.0-49-rt-amd64` |
| Kernel build | Debian `6.1.174-1` |
| Real-time mode | `PREEMPT_RT`; `/sys/kernel/realtime` is `1` |
| Architecture | `x86_64` |
| Compiler used for kernel | GCC 12 |
| EtherLab package | `ethercat-dkms` |
| EtherLab DKMS version | `1.6.9` |
| Debian EtherLab package version | `1.6.9.gb709e58-1+28.2` |
| EtherLab runtime version | `1.6.9 1.6.9.gb709e58-1+28.2` |
| EtherLab API header | `/usr/src/ethercat-dkms-1.6.9/include/ecrt.h` |
| Kernel build directory | `/lib/modules/6.1.0-49-rt-amd64/build` |
| Matching EtherLab symbol file | `/var/lib/dkms/ethercat-dkms/1.6.9/6.1.0-49-rt-amd64/x86_64/module/Module.symvers` |
| Installed master module | `/lib/modules/6.1.0-49-rt-amd64/updates/dkms/ec_master.ko` |
| Active EtherLab modules | `ec_master`, `ec_igc` |

The loaded `ec_master` reports vermagic:

```text
6.1.0-49-rt-amd64 SMP preempt_rt mod_unload modversions
```

The matching DKMS `Module.symvers` is retained by this package's
`POST_BUILD="save_module_symvers $dkms_tree"` hook. It is not in the EtherLab
source directory or the kernel's own `Module.symvers`.

## Confirmed external API

The installed EtherLab source and matching symbol file export the APIs needed
for the planned stages, including:

- `ecrt_request_master`, `ecrt_release_master`, and `ecrt_version_magic`;
- master/domain creation, activation, receive/process/queue/send, and state;
- scanned-slave lookup with `ecrt_master_get_slave`;
- slave configuration, declarative PDO setup, and PDO-entry registration;
- synchronous master SDO upload/download and configuration SDO helpers;
- asynchronous runtime SDO request functions;
- distributed-clock application time, synchronization, and monitoring.

The first probe deliberately uses only version checking plus master acquisition
and release. Later APIs remain unproven until exercised by their own stages.

`ecrt_request_master()` is documented as blocking and returns `NULL` on
failure. The target implementation reserves the master exclusively, enters its
operation phase, and returns `NULL` for conditions including invalid master,
busy master, missing device, and operation-phase failure. Because this public
API discards the internal errno, the first probe can report failure but cannot
distinguish those causes programmatically.

`ecrt_release_master()` is blocking, leaves the operation phase, frees the
application configuration, releases device-module references, and clears the
master reservation. It must not run in real-time context.

## Build contract

The top-level build resolves and validates:

```text
KERNEL_RELEASE
KERNEL_BUILD
ETHERLAB_INCLUDE
ETHERLAB_SYMVERS
```

For this target:

```sh
make
```

is equivalent to:

```sh
make \
  KERNEL_RELEASE=6.1.0-49-rt-amd64 \
  KERNEL_BUILD=/lib/modules/6.1.0-49-rt-amd64/build \
  ETHERLAB_INCLUDE=/usr/src/ethercat-dkms-1.6.9/include \
  ETHERLAB_SYMVERS=/var/lib/dkms/ethercat-dkms/1.6.9/6.1.0-49-rt-amd64/x86_64/module/Module.symvers
```

The build fails if `ecrt.h`, the selected kernel build tree, the symbol file, or
the `ecrt_request_master` symbol is absent. Explicit overrides are the intended
path for manually built EtherLab trees.

Automatic DKMS resolution also fails when more than one
`/usr/src/ethercat-dkms-*` source tree exists. Select `ETHERLAB_VERSION`, or
provide both `ETHERLAB_INCLUDE` and `ETHERLAB_SYMVERS`; the build never chooses
the first ambiguous installation silently.

Never select a `Module.symvers` belonging to another kernel release or EtherLab
build. Symbol CRCs are required because this kernel enables module versioning.

## Explicit manual-layout contract test

On 2026-07-24, the installed 1.6.9 `ecrt.h` and its exact matching DKMS
`Module.symvers` were copied into arbitrary temporary paths shaped like a
separate source/build tree:

```text
TEMP/source/include/ecrt.h
TEMP/build/Module.symvers
```

Both `make check-build-env` and `make modules` passed when those paths were
supplied explicitly as `ETHERLAB_INCLUDE` and `ETHERLAB_SYMVERS`. The rebuilt
module reported:

```text
license:  GPL
vermagic: 6.1.0-49-rt-amd64 SMP preempt_rt mod_unload modversions
```

A Linux kernel-header `Module.symvers` was rejected because it lacked
`ecrt_request_master`; a missing manual header path was also rejected.
Simulated multiple auto-detected DKMS source trees failed with a clear
ambiguity error, while explicit paths continued to pass.

This proves that the build contract does not require the DKMS directory layout
and fails closed for the tested missing/wrong inputs. It does **not** prove
compatibility with a separately compiled EtherLab source revision, because no
independent manual EtherLab build is present on this host. Such a build must
still supply its own header and matching `Module.symvers`, build successfully,
load under the target kernel, and pass the standalone lifecycle tests before
compatibility is claimed.

The repeatable no-hardware check is:

```sh
make test-build-contract
```

It stages the selected header/symbol pair under temporary arbitrary paths,
rebuilds the modules, checks GPL license and target vermagic, verifies the
default DKMS path, and requires rejection of a missing header, kernel-only
symbol file, and ambiguous automatic DKMS selection. `ETHERLAB_INCLUDE`,
`ETHERLAB_SYMVERS`, `KERNEL_RELEASE`, and `KERNEL_SYMVERS` may be overridden
to exercise another prepared build.

## Runtime baseline and probe result

At inspection time, `ec_master` and the EtherCAT-enabled `ec_igc` device module
were loaded. A host-level `ethercat master` reported:

```text
Master0
  Phase: Idle
  Active: no
  Slaves: 29
  Main link: UP
```

The restricted development sandbox initially hid `/dev/EtherCAT0`; that was
not a host EtherLab configuration fault. Host-level lifecycle checks must be
used when device-node access or kernel module operations are required.

On 2026-07-24, the probe passed ten consecutive iterations of:

```text
insmod elc_ethercat_probe.ko
  -> API magic matched: 0x106
  -> acquired master 0
  -> released master 0
rmmod elc_ethercat_probe
```

The kernel log contained the expected acquire/release/unload messages for each
iteration. Afterward, the probe was absent from the module list and
`ethercat master` still exited successfully with master 0 idle, link up, and
29 slaves.

Clockwork was shut down before this test. The result therefore proves external
symbol linkage and basic acquire/release semantics only while the master is
unreserved. It does **not** prove contention behavior or coexistence with
Clockwork.

The inspected EtherLab implementation protects master reservation with
`master_sem` and rejects an already reserved master internally with `-EBUSY`.
The public kernel function `ecrt_request_master()` converts that error to
`NULL`, so the current probe reports `-EBUSY` from module initialization without
being able to distinguish internal failure causes. This expected behavior must
still be verified while the existing Clockwork backend owns master 0. The
acceptance condition is:

```text
Clockwork remains operational and retains ownership
probe module load fails cleanly
no EtherLab state or configuration is changed
no kernel warning or oops occurs
probe can acquire/release normally after Clockwork exits
```

The completed test also does not yet prove configuration, discovery through
`ecrt_master_get_slave`, cyclic operation, or long-duration memory safety.

## Reproduction commands

Useful read-only checks:

```sh
uname -a
cat /sys/kernel/realtime
dkms status
modinfo ec_master
modinfo ec_igc
ethercat version
ethercat master
```

Build environment validation without compiling:

```sh
make check-build-env
```

Build and inspect the probe:

```sh
make
modinfo kernel/elc_ethercat_probe.ko
```

The privileged lifecycle test is:

```sh
sudo ELC_TEST_REPEAT=10 tools/elc_test_master.sh
```

The script refuses to disturb a probe module that was already loaded.

## Open items

- Both the minimal probe and Phase 2 character-device open contention tests
  passed while IOD owned master 0.
- Extend lifecycle repetition and run kmemleak/fault-injection testing when the
  module begins allocating persistent objects.
- Obtain and test an independently compiled manual EtherLab build before
  claiming binary/source-revision compatibility. The explicit arbitrary-path
  contract itself is validated.
