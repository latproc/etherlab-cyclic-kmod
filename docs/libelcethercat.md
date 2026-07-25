# libelcethercat — Generic User-Space Library API

**Status:** Phase 7 library landed for API 0.16. Sources live under `lib/`
and `include/elc_ethercat.h`. This document remains the public contract;
keep it aligned when the API changes.

This project is a **generic** EtherCAT cyclic transport. It is not a
Clockwork-only stack. Any controller may use the kernel module and this
library: standalone tools, test harnesses, motion services, or higher-level
runtimes. Clockwork/IOD is one optional consumer; its migration notes are
an **appendix**, not part of the library design.

**Related documents:**

- Normative ioctl ABI: [`uapi.md`](uapi.md)
- Controller lifecycle (any client): [`developer-guide.md`](developer-guide.md)
- Process-image rules: [`process-image-exchange.md`](process-image-exchange.md)
- Roadmap: [`../Implementation_Plan.md`](../Implementation_Plan.md)
- Architecture: [`architecture.md`](architecture.md)

---

## 1. Purpose

`libelcethercat` is a thin, **policy-free** C library that wraps
`/dev/elc_ethercat0` and `include/elc_ethercat_uapi.h`. It exists so that:

1. every userspace client shares one ABI client instead of open-coding
   ioctl layouts;
2. in-tree tools (`elc_bus`, `elc_config`, … — **elc** = EtherLab Cyclic,
   not Clockwork) and external controllers exercise the same path; and
3. machine policy stays **out** of both the kernel module and this library.

**User space owns** device identity, ESI/XML (or other) parsing, PDO and
setup recipes, process-data meaning, and arm/recovery policy.

**The library owns** only open/close, structure packing, ioctl dispatch,
and small conveniences that do not encode product or machine semantics.

```text
  any controller / tool              libelcethercat           kernel module
  ---------------------              -------------           -------------
  topology + recipes          --->   C API              ---> ioctl UAPI
  stable entry IDs            --->   config / offsets   ---> domains / cycle
  app logic                   <---   snapshots / status <--- process images
```

---

## 2. Placement, packaging, and install

### 2.1 Source location

The library lives in **this repository**, next to the UAPI and tools:

```text
etherlab-cyclic-kmod/
├── include/
│   ├── elc_ethercat_uapi.h      # wire ABI (kernel + userspace)
│   └── elc_ethercat.h           # public library API
├── lib/
│   ├── elc_ethercat.c           # implementation
│   └── elcethercat.pc.in
├── kernel/
├── tools/                      # migrate onto the library over time
└── docs/
```

Do **not** embed a second copy of the UAPI inside an application tree.
Do **not** depend on any control-system framework (for example Boost,
application object models, or messaging middleware) inside the library.

### 2.2 Install surface

Install headers and the library under a single prefix (default
`/usr/local`, site override as needed):

```text
$(PREFIX)/include/elc_ethercat_uapi.h
$(PREFIX)/include/elc_ethercat.h
$(PREFIX)/lib/libelcethercat.so
$(PREFIX)/lib/libelcethercat.a          # optional static archive
$(PREFIX)/lib/pkgconfig/elcethercat.pc  # recommended
```

Suggested Make targets (to implement with the library):

```text
make lib
make install-lib PREFIX=/usr/local
```

Downstream builds should locate the package without hard-coding paths:

```cmake
find_path(ELC_INCLUDE_DIR elc_ethercat.h
  HINTS $ENV{CW_ETHERCAT_PREFIX}/include)
find_library(ELC_LIBRARY elcethercat
  HINTS $ENV{CW_ETHERCAT_PREFIX}/lib)
```

Or via pkg-config:

```text
pkg-config --cflags --libs elcethercat
```

### 2.3 Versioning

| Layer | Versioning rule |
|-------|-----------------|
| Kernel UAPI | `ELC_API_VERSION_MAJOR` / `MINOR` in `elc_ethercat_uapi.h` |
| Library SONAME | Track UAPI major (e.g. `libelcethercat.so.0`) |
| Library package | Require matching major; refuse open if module major mismatches |

Minor UAPI bumps are additive. Callers must still negotiate capabilities
before using optional features.

### 2.4 Licensing

The kernel module is GPL-2.0-only. The initial `libelcethercat` sources and
public headers use **GPL-2.0-only** to match the repository default. A
different userspace license (for example LGPL) requires an explicit
documented decision before broader proprietary linking is claimed.

---

## 3. Design principles

1. **C API only** for the shared object. Language bindings or C++ facades
   belong in the consumer, not in this library’s ABI.
2. **No hidden global state** beyond one open control fd per handle.
   One `elc_handle` maps to one exclusive `/dev/elc_ethercat*` owner.
3. **UAPI types may appear in public headers** for structure-sized
   results; prefer library helpers for lifecycle.
4. **Errors:** return `0` on success and `-errno` on failure (or an
   equivalent documented scheme). Never crash on recoverable transport
   errors.
5. **Buffers:** the caller owns all data buffers. The library does not
   retain user pointers across successful returns.
6. **No application logging framework** inside the library; return codes
   and optional short error strings only.
7. **Thread safety:** one handle is not thread-safe unless documented.
   Controllers that use multiple threads must serialise handle use or
   wait for an explicit multi-thread contract.
8. **No product identities** (drive families, machine names, axis labels)
   in headers, symbols, or comments that constrain the ABI.

---

## 4. Public library API

This is the intended public surface. Names are stable for documentation;
implementation may add internal helpers. All functions take
`elc_handle *` unless noted.

### 4.1 Types

```c
/* include/elc_ethercat.h (design) */

#include "elc_ethercat_uapi.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct elc_handle elc_handle;

typedef struct elc_slave_summary {
	uint16_t position;
	uint16_t alias;
	uint32_t vendor_id;
	uint32_t product_code;
	uint32_t revision_number;
	uint32_t serial_number;
	uint8_t al_state;
	uint8_t error_flag;
	char name[ELC_SLAVE_NAME_LEN];
} elc_slave_summary;

/* Stable for one applied configuration generation. */
typedef uint32_t elc_entry_id;

#ifdef __cplusplus
}
#endif
```

### 4.2 Open, negotiate, close

| Function | Behaviour |
|----------|-----------|
| `elc_open(const char *device_path, elc_handle **out)` | Open `O_RDWR\|O_CLOEXEC`. Default path `/dev/elc_ethercat0`. Claims EtherLab master 0. `EBUSY` if another owner holds the master. |
| `elc_close(elc_handle *h)` | Close fd; kernel gates outputs, stops cyclic work, releases master. Safe if `h` is NULL. |
| `elc_get_api_version(h, struct elc_api_version *v)` | `ELC_IOC_GET_API_VERSION`. |
| `elc_get_capabilities(h, struct elc_capabilities *c)` | `ELC_IOC_GET_CAPABILITIES`. |
| `elc_require_api(h, uint16_t major, uint16_t min_minor)` | Fail if major mismatch or minor too old. |
| `elc_fd(const elc_handle *h)` | Underlying fd for poll/tests only. |

**Exclusivity:** opening this device is mutually exclusive with any other
EtherLab application that has requested master 0 (including direct
`ecrt_request_master(0)` clients). Only one application owner at a time.

### 4.3 Discovery

| Function | Behaviour |
|----------|-----------|
| `elc_get_master_info(h, struct elc_master_info *info)` | Link, scan busy, slave count. |
| `elc_get_slave_info(h, uint16_t position, struct elc_slave_info *info)` | One position. |
| `elc_list_slaves(h, elc_slave_summary *buf, size_t cap, size_t *count)` | Convenience: fill summaries for `0..slave_count-1`. Caller chooses whether to wait while `scan_busy`. |

Discovery returns raw bus identity. Matching policy (required topology,
revision rules, alias use) is entirely the controller’s responsibility.

### 4.4 Ordered setup SDOs (pre-activation)

| Function | Behaviour |
|----------|-----------|
| `elc_setup_begin(h)` | Start setup transaction. |
| `elc_setup_add_sdo(h, const struct elc_setup_sdo *sdo)` | Ordered typed write. |
| `elc_setup_apply(h, struct elc_setup_apply *result)` | Execute batch; report first failure. |
| `elc_setup_reset(h)` | Drop pending setup. |
| `elc_sdo_upload(h, struct elc_sdo_upload *req)` | Bounded diagnostic upload. |

Use for commissioning and pre-activation parameter/PDO setup owned by the
controller. Runtime mailbox SDO policy during OP is a separate concern.

### 4.5 Transactional configuration

| Function | Behaviour |
|----------|-----------|
| `elc_config_begin(h)` | Start pending configuration. |
| `elc_config_add_slave(h, const struct elc_config_slave *)` | |
| `elc_config_add_sync(h, const struct elc_config_sync *)` | |
| `elc_config_add_pdo(h, const struct elc_config_pdo *)` | |
| `elc_config_add_entry(h, const struct elc_config_entry *)` | Application entries need nonzero `entry_id`. Padding uses `0x0000:00` and `entry_id=0`. |
| `elc_config_add_dc(h, const struct elc_config_dc *)` | Optional. |
| `elc_config_add_dc_policy(h, const struct elc_config_dc_policy *)` | Optional. |
| `elc_config_add_domain(h, const struct elc_config_domain *)` | Explicit domains. |
| `elc_config_add_domain_assignment(h, const struct elc_config_domain_assignment *)` | Exactly one assignment per configured slave when domains are explicit. |
| `elc_config_validate(h, struct elc_config_validate *result)` | |
| `elc_config_apply(h, struct elc_config_apply *result)` | Config immutable until deactivate/close. |

If no domain records are submitted, the kernel creates one **implicit**
compatibility domain (single concatenated image).

### 4.6 Domains and entry offsets

| Function | Behaviour |
|----------|-----------|
| `elc_domain_create(h, struct elc_domain_create *req)` | Materialise domain(s) after apply. |
| `elc_get_entry_offset(h, struct elc_entry_offset *io)` | Resolve `entry_id` → `global_offset`, bit position, bit length for the active generation. |

Controllers should choose **stable entry IDs** when submitting entries,
then resolve offsets after domain creation. Offsets are valid only for
the active configuration generation.

### 4.7 Cyclic engine

| Function | Behaviour |
|----------|-----------|
| `elc_cycle_activate(h, uint32_t period_ns, uint32_t flags)` | Start cyclic receive/process/queue/send. Period in `[ELC_CYCLE_PERIOD_MIN_NS, ELC_CYCLE_PERIOD_MAX_NS]` (100 µs … 1 s). |
| `elc_cycle_deactivate(h)` | Synchronous gate + join cyclic task. |
| `elc_cycle_status(h, struct elc_cycle_status *st)` | |
| `elc_cycle_wait(h, struct elc_cycle_wait *wait)` | Interruptible wait for a newer cycle record. |
| `elc_cycle_info(h, struct elc_cycle_info *info)` | Coherent timing/generation record without waiting. |
| `elc_cycle_set_period(h, struct elc_cycle_period_update *upd)` | Disarmed, non-DC acknowledged period change (API 0.15). |

The kernel owns the bus timeline. Application loop rate may be lower than
the EtherCAT cycle rate; clients must use cycle identity and may skip
intermediate notifications.

### 4.8 Process images and output gate

| Function | Behaviour |
|----------|-----------|
| `elc_get_input_snapshot(h, struct elc_input_snapshot *snap, void *buf, size_t len)` | Coherent global input image. |
| `elc_publish_output(h, const void *image, const void *mask, size_t len, struct elc_output_publish *pub)` | Publish full image + update mask. Does **not** arm. |
| `elc_arm_output(h, struct elc_output_arm *arm)` | Arm exact generation + latest publication sequence. |
| `elc_disarm_output(h, struct elc_output_disarm *disarm)` | Synchronous disarm / zero gate. |
| `elc_get_io_status(h, struct elc_io_status *st)` | Health, arm, re-arm required, faults. |

Optional lease (API 0.14):

| Function | Behaviour |
|----------|-----------|
| `elc_configure_output_lease(h, …)` | Armed-cycle budget when supported. |
| `elc_renew_output_lease(h, …)` | Does not re-arm by itself. |
| `elc_get_output_lease_status(h, …)` | |

### 4.9 Status helpers

| Function | Behaviour |
|----------|-----------|
| `elc_get_config_slave_status(h, …)` | Per configured slave ID. |
| `elc_get_domain_status(h, …)` | Per domain WC/validity. |
| `elc_get_dc_status(h, …)` | DC diagnostics when configured. |

### 4.10 Convenience builders (optional)

Policy-free helpers only:

```c
/* Example pure ID scheme — callers may use any stable scheme. */
elc_entry_id elc_make_entry_id(uint32_t slave_config_id,
                                   uint16_t local_index);

int elc_fill_config_slave(struct elc_config_slave *out,
                            uint32_t config_id,
                            uint16_t position,
                            uint16_t alias,
                            uint32_t vendor_id,
                            uint32_t product_code,
                            uint32_t revision_number,
                            uint32_t flags);
```

Do **not** put ESI XML parsing, vendor recipes, or motion modes in these
helpers.

### 4.11 Mapping library calls to UAPI ioctls

| Library group | Primary ioctls |
|---------------|----------------|
| Negotiate | `GET_API_VERSION`, `GET_CAPABILITIES` |
| Discovery | `GET_MASTER_INFO`, `GET_SLAVE_INFO` |
| Setup SDO | `SETUP_BEGIN`, `SETUP_ADD_SDO`, `SETUP_APPLY`, `SETUP_RESET`, `SDO_UPLOAD` |
| Config | `CONFIG_BEGIN`, `CONFIG_ADD_*`, `CONFIG_VALIDATE`, `CONFIG_APPLY` |
| Domains | `DOMAIN_CREATE`, `GET_ENTRY_OFFSET` |
| Cycle | `CYCLE_ACTIVATE`, `CYCLE_DEACTIVATE`, `CYCLE_STATUS`, `CYCLE_WAIT`, `CYCLE_INFO`, `CYCLE_SET_PERIOD` |
| Images | `GET_INPUT_SNAPSHOT`, `PUBLISH_OUTPUT`, `ARM`, `DISARM` |
| Status | `GET_IO_STATUS`, `GET_CONFIG_SLAVE_STATUS`, `GET_DOMAIN_STATUS`, `GET_DC_STATUS` |
| Lease / history | as in `elc_ethercat_uapi.h` for API 0.14–0.16 |

Normative field semantics remain in [`uapi.md`](uapi.md). The library
must zero structures and set `struct_size` / `api_major` correctly.

---

## 5. Entry IDs and process-image layout

Recommended pattern for **any** controller:

```text
1. Assign a stable nonzero entry_id to each application PDO entry.
2. Represent mandatory padding as 0x0000:00 with entry_id = 0.
3. Submit config; validate; apply; create domain(s).
4. Resolve each application entry_id to global_offset / bit / length.
5. Interpret inputs and build outputs only at those locations for this
   configuration generation.
```

| Domain mode | When to use |
|-------------|-------------|
| Implicit single domain | Simple machines; one WC for the whole image. |
| Explicit multi-domain | Independent validity (e.g. always-on I/O vs switchable equipment). |

With multiple domains the kernel still exposes one **concatenated global**
image; domain declaration order defines segment order. Per-slave validity
follows the assigned domain’s working counter.

When object identity `(index, subindex)` is duplicated, the controller must
supply an additional discriminator (for example PDO index or occurrence).
Never infer selector mode from numeric magnitude and never silently fall
back from a failed object selector to a flat position index.

---

## 6. Controller lifecycle (generic)

```text
elc_open
  → negotiate API / capabilities
  → discover and match required topology
  → optional ordered setup SDOs
  → config begin / add / validate / apply
  → domain create / resolve entry offsets
  → cycle_activate(period_ns)
  → loop: wait or poll → input snapshot → publish (+ mask)
  → optional arm under site safety policy
  → disarm → cycle_deactivate → elc_close
```

Close of the control fd is the hard ownership boundary: outputs gated,
cyclic task stopped, master returned for diagnostic CLI use.

Full behavioural detail: [`developer-guide.md`](developer-guide.md).

---

## 7. In-tree tools as first clients

Migrate tools onto `libelcethercat` so the library is proven before external
integrations:

| Tool | Library? | Coverage |
|------|----------|----------|
| `elc_bus` | yes | open, negotiate, discovery |
| `elc_sdo` | yes | setup / upload / recipes |
| `elc_config` | yes | full config, cycle, images, timing; hostile active checks still use raw ioctl via `elc_fd` so wrong `struct_size` is not papered over |
| `elc_config_stress` | yes | maximum pending create/reset limits |
| `elc_abi_test` | **no** (intentional) | raw ioctl hostile ABI suite against the kernel UAPI |

External controllers should use the same installed library, not a fork of
ioctl glue.

---

## 8. Implementation checklist (this repository)

- [x] Add `include/elc_ethercat.h` matching §4
- [x] Implement `lib/` sources
- [x] `make lib` / `make install-lib` / pkg-config
- [x] Migrate `tools/elc_bus` to the library
- [x] Migrate `tools/elc_sdo`, `tools/elc_config`, `tools/elc_config_stress`
- [x] Keep `tools/elc_abi_test` on raw ioctls for hostile UAPI checks
- [x] Document package version and library license decision (GPL-2.0-only v1)
- [x] Keep this document updated when the public API ships

---

## 9. Non-goals

- Parsing Beckhoff ESI (or any vendor XML) inside the library
- Embedding product, machine, or motion-planner policy
- Requiring any particular userspace runtime
- mmap process-image export (copied images unless redesigned later)
- CSP setpoint queues or delegated domain controllers (later UAPI work)

---

## 10. Summary

| Question | Answer |
|----------|--------|
| What is this project? | Generic kernel EtherCAT cyclic transport + UAPI + tools + library |
| Who is the library for? | Any userspace controller or tool |
| Where does it live? | `lib/` in this repository; installable headers + `libelcethercat` |
| API language? | C wrapping UAPI 0.16 |
| What stays out of the library? | Device recipes, ESI parsing, machine semantics, arm policy meaning |
| Hard exclusivity rule? | One master-0 application owner: this control fd **or** another EtherLab client |

---

# Appendix A — Integrating a specific consumer (Clockwork/IOD)

For a table-free, copy-pasteable starter prompt when opening a new agent
session in Latproc/IOD, see
[`iod-session-handoff.md`](iod-session-handoff.md).

This appendix is **not** part of the generic library contract. It records
how one existing runtime (`/opt/latproc/iod`) can adopt the transport
without putting Clockwork types into this repository.

Other consumers should follow §4–§7 and
[`developer-guide.md`](developer-guide.md); they need not follow this
appendix.

### A.1 Responsibility split

| Clockwork keeps | Uses from this project |
|-----------------|------------------------|
| MODULE / XML / matching | Discovery via library |
| `ECModule`, `IOComponent`, machines | Entry offsets after config |
| Recipes and SDO policy | Setup SDO batch API |
| Application cycle / ZMQ | Cycle wait + snapshots |
| When to arm outputs | Publish / arm / disarm |

Keep the existing direct-EtherLab path as a **non-default fallback** until
proven. Do not make the kernel backend default without an explicit
production decision.

### A.2 Entry ID and offset bridge

Today IOD fills `ECModule::offsets[]` and `bit_positions[]` via
`ecrt_slave_config_reg_pdo_entry_pos` in `registerModules()`.

With the kernel backend:

```text
for each module entry i:
  entry_id = make_entry_id(slave_config_id, i)   # nonzero for app entries
  submit config entry (index/subindex/bit_length)
after domain create:
  resolve entry_id → global_offset, bit_pos
  module->offsets[i] = global_offset
  module->bit_positions[i] = bit_pos
```

`generateIOComponentModules()` can stay largely unchanged if those arrays
are filled the same way. Use `EtherCATEntrySelector` (index/subindex,
optional PDO) when building IDs; never silently fall back to flat `pos`.

### A.3 Phased IOD changes (in the Latproc tree only)

**Phase 8 — Discovery only**

- Files: thin `KernelEthercatBus` adapter; optional call from
  `listSlaves()` / `setupEtherCatThread()` behind a flag.
- Accept: topology matches `ethercat slaves` / `elc_bus`; master
  exclusivity documented.

**Phase 9 — Configuration**

- Convert `ECModule` / XML result to `elc_config_*`; skip ecrt
  configure/register when backend is kernel; fill offsets from
  `elc_get_entry_offset`.
- Accept: same XML matching; disarmed OP on a fixed fixture.

**Phase 10 — Setup SDOs**

- Map prep recipes to `elc_setup_*`; keep external scripts as recovery.
- Accept: mapping objects match known-good commissioning path.

**Phase 11 — Cyclic path**

- `EtherCATThread` / `ProcessData` / activate: wait, snapshot, publish,
  optional arm; no userspace `ecrt` domain send for that backend.
- Accept: zero-output lifecycle, controller-death gate, re-arm rules;
  legacy backend still selectable.

### A.4 Behaviour differences IOD must absorb

| Topic | Legacy IOD | This transport |
|-------|------------|----------------|
| Bus timing | Userspace timer + ecrt send | Kernel cyclic task |
| Domain memory | `ecrt_domain_data` pointer | Copied snapshots |
| Enabling outputs | Writing domain process data | Publish then **explicit arm** |
| Controller death | Process exit | Control fd close gates outputs |
| Missed app wakes | Often skews the bus cycle | Bus continues; use latest cycle id |
| Multi-domain validity | Single WC | Per-domain WC |

### A.5 Suggested Latproc layout

```text
/opt/latproc/iod/src/
  KernelEthercatBus.*          # discovery
  KernelEthercatTransport.*    # config + cycle + images
  ECModuleToKernelConfig.*     # ECModule → generic config records
  ECInterface.cpp / ecat_thread.cpp  # dispatch legacy vs kernel
```

Default backend remains legacy until an explicit production decision.

### A.6 Testing the consumer

| Stage | What |
|-------|------|
| Library + tools | Proven in this repository first |
| Discovery flag | Topology match only |
| Config path | Disarmed activate, offset consistency |
| Setup SDO | Compare to known-good commissioning |
| Cyclic | Zero-output lifecycle; death; re-arm |
| Nonzero outputs | Site-authorised commissioning only |

Standalone tool evidence does not by itself claim any particular runtime
is production-ready.
