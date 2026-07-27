# EtherLab Cyclic Kernel Transport

A generic Linux kernel transport for deterministic EtherCAT cyclic I/O using
the [EtherLab](https://etherlab.org/) master.

User space describes the network at runtime: slaves, Sync Managers, PDOs,
PDO entries, setup SDOs, Distributed Clocks, and validity domains. The module
validates that description, configures EtherLab, runs one cyclic exchange, and
provides bounded copied process images through a versioned ioctl API.

The transport contains no machine, servo, CiA 402, XML, or control-system
policy. Those decisions remain in user space, so the same module can support
different devices and control systems without recompiling kernel code.

> **Development status:** experimental API 0.18. The standalone documentation
> gate passes, but the kernel-safety and production timing gates remain open.
> Kernel faults can crash the host and EtherCAT outputs can move machinery.
> Always use the site's hardware safety and commissioning procedures.

## Why use this interface?

EtherLab already provides the EtherCAT master. Getting a clean, reusable
application boundary around it—master lifecycle, configuration, cyclic
exchange, and teardown—is a recurring pain for people writing their own
EtherLab clients. This module aims to make that part straightforward without
putting application policy in the kernel:

- **Runtime configuration:** one kernel binary accepts different topologies,
  PDO layouts, setup recipes, cycle periods, and DC policies.
- **Pre-activation setup SDOs:** ordered typed SDO transactions run before
  activation so devices that need CoE startup parameters (or other mailbox
  setup) can be configured without embedding those recipes in the kernel.
- **User-owned PDO maps:** ESI/XML and revision-specific recipes stay in user
  space. The module applies the map you submit; it does not parse XML or
  auto-heal a recipe that disagrees with the live CoE layout. Wrong
  revision-selected maps (for example two EL5152s, same product, different
  revisions) fail at slave programming, not at a kernel “revision match”
  check—see the [developer guide](docs/developer-guide.md).
- **Deterministic ownership:** one kernel task performs
  receive/process/DC/queue/send for every configured domain on one cycle
  timeline.
- **Stable process entries:** user-supplied entry IDs map EtherCAT objects to
  returned byte/bit offsets; application code does not depend on flattened
  enumeration order.
- **Explicit validity domains:** always-powered I/O and switchable equipment
  can have independent working-counter and data-validity results.
- **Per-domain output authority (API 0.17+):** each domain has its own arm,
  re-arm, publication buffers, and health gate. A drive-domain fault need not
  disarm a healthy I/O domain; master/link loss still gates every domain.
- **Domain bus firewall:** complete domain WC keeps that domain valid with no
  interruption when another domain fails (power loss, cable damage, module
  failure). Ring/redundant Ethernet for mid-bus cable splits is a planned
  companion to multi-client domain interfaces.
- **Hang-failsafe lease (API 0.18):** optional wall-time or cycle budget;
  successful publish/arm refill the budget so a hung controller cannot keep
  last motion outputs without high-rate renew ioctls.
- **Safe copied exchange:** bounded double-buffered input and masked output
  images avoid retaining user pointers or exposing kernel memory.
- **Fail-safe output control:** publishing data never arms it. Arming is
  generation-bound; domain health loss, disarm, controller exit, and
  deactivation select zero outputs for the affected authority and require
  fresh publication before re-arm.
- **Controller liveness lease:** an optional armed-cycle budget prevents a
  stalled controller from retaining output authority indefinitely while still
  allowing monitoring to run without a heartbeat when outputs are disarmed.
- **Power-loss recovery:** an unavailable configured slave can recover while
  the transport continues; stale outputs remain gated until user space
  explicitly re-arms.
- **Distributed Clocks:** user space selects DC policy while the kernel owns
  application time, synchronization calls, steering, and low-overhead status.
- **Observable timing:** coherent cycle identity, scheduled/actual wake times,
  input and consumed-output generations, stale reuse, missed deadlines, WC,
  and interruptible cycle notification are available without per-cycle logs.
- **Standalone validation:** discovery, configuration, ABI, stress, lifecycle,
  timing, and zero-output tools are included.

The design is also being prepared for optional delegated domain controllers:
one process may own ordinary machine I/O while a dedicated motion service owns
a drive domain. There will still be one EtherLab master and one cyclic kernel
timeline. This delegation is planned, not part of API 0.18.

```text
              user-space configuration and controllers
                              |
                    versioned ioctl UAPI
                              |
                     /dev/elc_ethercat0
                              |
       validation / domains / output gates / cyclic task
                              |
                       EtherLab master 0
                              |
                       EtherCAT network
```

## Main link without opening the elc device

`/dev/elc_ethercat0` is an **exclusive** application owner. Do not open it
(or run `tools/elc_bus`) only to wait for cable/link before a controller
starts — that double-claims the master and clutters kernel logs.

**Non-owner link check** (EtherLab userspace; works idle or while another app
owns master 0):

```sh
ethercat master
# under Main: … → Link: UP
```

Details: [operator guide](docs/operator-guide.md#main-link-without-control-ownership),
[UAPI ownership](docs/uapi.md#ownership-and-lifecycle).

## What is implemented?

API 0.18 currently includes:

- exclusive EtherLab master lifecycle and raw bus discovery;
- ordered typed setup SDOs;
- transactional slave, Sync Manager, PDO, entry, DC, and domain setup;
- implicit single-domain compatibility and explicit multi-domain layouts;
- configurable cyclic pumping, copied process-image exchange, and optional
  bounded per-cycle input history with batched reads;
- aggregate, per-domain, and per-configured-slave health/validity;
- per-domain output authority with masked publication, explicit arm, and
  synchronous disarm (global or domain-scoped selectors);
- optional authority-scoped output leases with publish/arm budget refill,
  optional `timeout_ms`, configure-while-active, and zero-gating on expiry;
- coherent timing/generation records and blocking wait-for-cycle; and
- bounded resource limits, hostile-input validation, and partial-failure
  unwind.

For commissioning and diagnosis, `tools/elc_io` is an interactive frontend
over the same declarative configuration parser. It can list and read entries,
watch values, and stage masked output values. Outputs start disarmed; staging
and publication do not transmit a commanded value. `arm` is a separate
explicit command and is refused unless the process was started with
`ELC_NONZERO_OUTPUT_AUTHORIZED=YES` after site safety approval.

Hardware evidence includes the current 34-slave target, multiple explicit
domains, Distributed Clocks, servo supply loss/restoration, controller-death
teardown, and zero-output lifecycle repetition. Nonzero commissioning has been
limited to a separately authorized single console LED/buzzer output. See the
[architecture review](docs/architecture-review-2026-07-24.md) for the open
acceptance gaps.

## Requirements

You need:

- a Linux kernel build tree matching the running target kernel (**≥ 4.19**;
  see [kernel support](docs/building/kernel-support.md));
- an EtherLab master built for that same kernel;
- EtherLab `ecrt.h`;
- the exact `Module.symvers` from that EtherLab build; and
- a C compiler and normal kernel external-module build tools.

Do not combine a header or symbol file from another EtherLab or kernel build.
The build deliberately fails when paths are missing or automatic DKMS
detection is ambiguous.

**Kernel policy:** if EtherLab builds on a kernel, this transport aims to
support it. Version differences go in `kernel/elc_kcompat.h`. The recorded
**hardware acceptance** target is still Debian RT `6.1.0-49-rt-amd64` with
EtherLab DKMS 1.6.9; older RT (for example 4.19 + source EtherLab) is
supported for build and is validated progressively. Exact paths and overrides:
[EtherLab build environment](docs/building/etherlab-dkms-environment.md).

## Build and install

Check the resolved build contract, compile the module and tools, then install
the modules into the target kernel tree:

```sh
make check-build-env
make -j6
sudo make install
```

`make install` installs `elc_ethercat.ko` and the minimal acquisition probe
under `/lib/modules/$(uname -r)/extra/elc_ethercat/` and runs `depmod`.
User-space tools remain in `tools/`.

For a manually built EtherLab tree, provide the matching artifacts once, then
**remember** them in a gitignored `local.mk`:

```sh
make \
  ETHERLAB_INCLUDE=/path/to/etherlab/include \
  ETHERLAB_SYMVERS=/path/to/etherlab/Module.symvers \
  save-build-env

make -j6
sudo make install
```

`save-build-env` writes `local.mk` (see `local.mk.example`). After that, plain
`make` / `make install` / `make dkms-install` reuse those paths. Command-line
`VAR=...` still overrides. Delete `local.mk` to return to EtherLab DKMS
auto-detection.

One-shot without saving:

```sh
make \
  KERNEL_BUILD=/path/to/kernel/build \
  ETHERLAB_INCLUDE=/path/to/etherlab/include \
  ETHERLAB_SYMVERS=/path/to/etherlab/Module.symvers
```

Verify that the external-path build contract fails closed as expected:

```sh
make test-build-contract
```

Uninstall the installed kernel modules with:

```sh
sudo make uninstall
```

### DKMS (kernel modules)

To rebuild the modules automatically when the kernel is updated, use DKMS
instead of (or after removing) a plain `make install` of the `.ko` files.
EtherLab must already be built for that kernel with a matching
`Module.symvers` (typically `ethercat-dkms`):

```sh
make check-build-env
sudo make dkms-install
dkms status
```

Remove with `sudo make dkms-uninstall`. Details:
[docs/building/elc-dkms.md](docs/building/elc-dkms.md). DKMS does not install
`libelcethercat` or the userspace tools.

For the complete build, load, scheduler-option, teardown, and recovery
sequence, follow the [standalone operator guide](docs/operator-guide.md).

## First safe test

Stop every other EtherLab application first; master 0 has one application
owner. Confirm the machine's physical motion inhibition before running any
hardware test.

Start with the minimal acquire/release probe:

```sh
sudo tools/elc_test_master.sh
```

Then run discovery and hostile ABI checks:

```sh
sudo tools/elc_test_bus.sh
```

After each test, `ethercat master` should report an idle, inactive master.
Review new kernel warnings and errors. Never force-unload the module.

Configuration syntax can be checked without claiming the master:

```sh
tools/elc_config check \
  tools/configs/ed3l_velocity_dc_pos29.conf
```

That file is only a syntax and commissioning example. A new application should
generate or write its own generic configuration from the intended network's
ESI data and reviewed device requirements. Do not copy a target-specific
fixture to another network: identity, PDO layout, DC parameters, output masks,
and safe commissioning state must be established for that machine.

The [testing guide](docs/testing.md) records test purpose, commands, expected
results, hardware evidence, and remaining gaps. The
[operator guide](docs/operator-guide.md) provides the shorter end-to-end safe
procedure.

## Write a user-space controller

Start with the [developer guide](docs/developer-guide.md). It explains:

1. API and capability negotiation;
2. discovery and topology matching;
3. transactional configuration and stable entry IDs;
4. activation and cycle notification;
5. coherent input snapshots;
6. masked output publication and explicit arming;
7. fault recovery, disarm, deactivation, and close.

The normative ABI description is [docs/uapi.md](docs/uapi.md), and the shared
fixed-width definitions are in
[`include/elc_ethercat_uapi.h`](include/elc_ethercat_uapi.h). The most useful
reference implementations are the generic **`elc_*` tools** (EtherLab Cyclic;
not Clockwork-specific):

- [`tools/elc_bus.c`](tools/elc_bus.c) for discovery and capabilities;
- [`tools/elc_config.c`](tools/elc_config.c) for complete configuration,
  cycling, snapshots, output gating, and teardown;
- [`tools/elc_sdo.c`](tools/elc_sdo.c) for bounded SDO operations; and
- [`tools/elc_abi_test.c`](tools/elc_abi_test.c) for error behavior and
  hostile-input expectations.

Harness environment variables use the same `ELC_` prefix (for example
`ELC_MOTION_INHIBITED`). The kernel module is `elc_ethercat`, the control
device is `/dev/elc_ethercat0`, and the userspace library is `libelcethercat`.

Applications should parse ESI XML and implement device/machine policy outside
the kernel. They must use returned entry offsets and explicit generations,
never assume that physical position, PDO enumeration order, or process-image
layout is stable across a changed configuration.

## Documentation map

- [Kernel support policy](docs/building/kernel-support.md) — multi-kernel
  floor, kcompat rules, how to add features.
- [EtherLab build environment](docs/building/etherlab-dkms-environment.md) —
  target paths, source EtherLab, and `local.mk` / `save-build-env`.
- [elc DKMS packaging](docs/building/elc-dkms.md) — install modules via DKMS.
- [`local.mk.example`](local.mk.example) — site path template (copy or
  `make save-build-env`).
- [Architecture](docs/architecture.md) — ownership, lifecycle, domains, and
  cycle timeline.
- [UAPI](docs/uapi.md) — ioctl structures, validation, and exact semantics.
- [Developer guide](docs/developer-guide.md) — implementing a new controller.
- [libelcethercat](docs/libelcethercat.md) — generic userspace library API
  (`make lib` / `make install-lib`) and optional consumer integration notes.
- [IOD session handoff](docs/iod-session-handoff.md) — copy-pasteable prompt
  for starting an agent session in Latproc/IOD against this transport.
- [Operator guide](docs/operator-guide.md) — safe build and test sequence.
- [Testing](docs/testing.md) — test matrix and recorded evidence.
- [Process-image exchange](docs/process-image-exchange.md) — buffering,
  generations, output gating, and recovery.
- [Distributed Clocks](docs/distributed-clocks.md) — DC configuration and
  kernel/user-space responsibilities.
- [Safety and failure behavior](docs/safety-and-failure-behaviour.md) —
  failure responses and acceptance limits.
- [Current architecture review](docs/architecture-review-2026-07-24.md) —
  completed evidence and open gates.
- [Implementation plan](Implementation_Plan.md) — authoritative roadmap and
  acceptance gates.

## License

Licensing is split so the kernel stays GPL while applications can link the
userspace library without opening their own code:

| Component | License |
|-----------|---------|
| Kernel modules (`kernel/`) | **GPL-2.0-only** — [LICENSE](LICENSE) |
| Tools and most of the tree | **GPL-2.0-only** — [LICENSE](LICENSE) |
| `libelcethercat` (`lib/elc_ethercat.c`, `include/elc_ethercat.h`) | **LGPL-2.1-or-later** — [LICENSE.LGPL-2.1](LICENSE.LGPL-2.1) |
| Shared UAPI header (`include/elc_ethercat_uapi.h`) | **GPL-2.0-only OR LGPL-2.1-or-later** (dual) |

Proprietary or other non-public software may **dynamically link**
`libelcethercat` and include the public headers under the LGPL terms. Static
linking is also permitted under LGPL-2.1 if you meet its relink requirements
(typically ship object files or an equivalent way to relink against a modified
library). The kernel module and GPL tools remain under GPL-2.0-only.
