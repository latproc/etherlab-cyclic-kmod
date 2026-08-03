# Client Slave Recovery and Setup CoE (Generic Controllers)

**Audience:** any user-space controller or tool using `libelcethercat` /
`/dev/elc_ethercat0` — not only Clockwork/IOD.

**Status:** field guidance from multi-domain plants (IO + servo daisy-chains)
with ordered setup SDO batches after control-power loss. Kernel and library
remain **policy-free**; this document is what **clients** must implement.

**Related:** [`libelcethercat.md`](libelcethercat.md),
[`uapi.md`](uapi.md) (setup batch + ownership),
[`recommended-master-lifecycle.md`](recommended-master-lifecycle.md),
[`safety-and-failure-behaviour.md`](safety-and-failure-behaviour.md),
[`operator-guide.md`](operator-guide.md),
[`developer-guide.md`](developer-guide.md).

---

## 1. Responsibility split (do not put this in the kernel)

| Layer | Owns | Does **not** own |
|-------|------|------------------|
| **Kernel (`elc_ethercat`)** | Master claim, scan/rescan, config/cycle, domain WC, arm/disarm/lease, setup **batch execution**, per-slave online/AL snapshots | Plant recipes, brand/model names, “retry until drive boots”, domain ready policy for motion |
| **`libelcethercat`** | Packing, ioctl dispatch, thin helpers | Retry/backoff, debounce, topology matching |
| **Controller / tool (client)** | Topology match, PDO/setup **recipes**, when to run setup, **retry after power return**, arm only when safe, multi-domain readiness | Re-implementing EtherLab cyclic exchange |

Ordered setup SDO batches (`elc_setup_*` / `ELC_IOC_SETUP_*`) use blocking
mailbox downloads. The kernel **does not** remember or replay that batch after
power loss. Persistent declarative config (Sync/PDO layout on the configuration
object) is a separate path; many drives still need an **explicit CoE recipe**
(PDO map disable → entries → assign → mode) that the **client** re-runs.

**Do not** move recipe retry loops, brand-specific SDO tables, or “wait 750 ms
then re-apply” into the kernel module. That is application policy and would
block unrelated clients and RT paths.

---

## 2. What power-loss looks like on the bus

Typical servo control-power cut on a daisy-chain (example: positions 29–33
drives after Beckhoff IO 0–28):

1. Slave count drops (e.g. 34 → ~29). IO may stay OP if only drives lose power.
2. On restore, slaves reappear **unevenly**. The **last** device on the MII
   chain often lags (slow boot, cable, weak supply).
3. A slave can show **online** while still **INIT**, with:
   - identity `vendor_id=0`, `product_code=0` (SII not readable yet);
   - AL status **0x0016 Invalid mailbox configuration**;
   - `Failed to fetch SII contents` / timed-out datagrams in `dmesg`.
4. Early setup/SDO apply then fails (`EINVAL`, I/O error, abort 0, or
   “SDO does not exist”). A **single-shot** apply on the first online edge
   leaves that axis unrecovered for a long time even after the drive is fine.
5. Mid-chain blips or full-string link events can take **IO** offline too;
   do not assume “only the drive domain moved.”

SM watchdog / SAFEOP on stop when the master releases or stops cycling is
normal; it is not the same as a successful cold re-commission.

---

## 3. Recommended client recovery state machine

For each **expected** slave that needs client-owned CoE setup after return
(especially **PDO map / SM assign** objects `0x1600`–`0x17ff`,
`0x1a00`–`0x1bff`, `0x1c10`–`0x1c2f`):

```text
offline (or AL INIT / identity 0:0)
    |
    v
seen online  ──────────────────────────────► queue "needs_setup"
    |                                              |
    | wait until setup-state ready:                |
    |   online && identity != 0:0                  |
    |   && AL in {PREOP, SAFEOP}   ◄── NOT OP      |
    | short hold ~0.3–0.5 s (SII/mailbox settle)   |
    | (window is short while cyclic is active)     |
    v                                              |
apply ordered setup batch (new begin/add/apply) ◄──┘
    |
    +-- success --> clear needs_setup; optional non-PDO defaults
    |
    +-- still OP / not PREOP yet --> status waiting_preop
    |                                 keep needs_setup
    |                                 do NOT burn fail attempts
    |
    +-- apply failure --> backoff, keep needs_setup
                          reset hold if AL leaves PREOP/SAFEOP
                          give up only after bounded attempts / max age
```

### 3.0 Why PREOP/SAFEOP only (not OP) — field requirement

EtherCAT device model and ED3L practice:

| AL | PDO map rewrite (`0x1600` / `0x1C12` / …) | Typical non-map SDO |
|----|------------------------------------------|---------------------|
| **INIT** | No (mailbox often 0x0016) | No |
| **PREOP** | **Yes — required window** | Yes |
| **SAFEOP** | Usually yes (mailbox; process data limited) | Yes |
| **OP** | **No / fails / rejected** | Sometimes yes |

Cold start works because the client runs the full recipe **before**
`CYCLE_ACTIVATE` (slaves still PREOP). After power return with the cycle
**already active**, the master promotes reappearing slaves toward OP
quickly. If the client waits too long (e.g. multi-second “ready hold” that
also accepts OP) or applies while AL=OP, mapping batches fail and status
flaps `pending` / `retrying`.

**Client policy (required for mapping recipes):**

1. Apply mapping CoE **only** when AL is PREOP or SAFEOP.
2. While AL is OP and setup is still needed: keep the job queued
   (`waiting_preop`); do **not** count OP as a failed apply.
3. Re-arm / re-queue on **entry to PREOP or SAFEOP** (AL edge) so the
   short window is not missed.
4. Use a **short** PREOP hold (hundreds of ms for identity/mailbox), not
   multi-second holds that outlast the PREOP window.

**Kernel gap (see §9):** today elc **cannot** force a configured slave to
stay in PREOP until the client finishes setup while cyclic is active.
Clients only **catch** the PREOP/SAFEOP window. A true “hold until setup
complete” needs a module/UAPI feature (§9).

### 3.1 Triggers (queue work; do not apply on the hard-RT path)

- **Offline → online** after the slave was previously known online (return),
  not on first cold scan (cold start uses pre-activate setup once).
- **AL → PREOP or SAFEOP** after prior life: re-queue / reset ready-hold so
  mapping can run in that window (do **not** apply on the PREOP→OP edge
  itself once already OP — that is too late for map CoE).
- Optional: operator “recommission” command.

### 3.2 Setup-state gate (before `SETUP_APPLY` for PDO maps)

Do **not** apply **mapping** setup CoE when any of these hold:

| Signal | Why |
|--------|-----|
| not online | No slave |
| AL is INIT (1) or 0 | Mailbox often refused (0x0016) |
| AL is **OP (8)** | PDO map objects not rewritable; treat as **wait**, not fail |
| `vendor_id` and `product_code` both 0 | SII/scan incomplete (“ghost” position) |
| scan busy / topology still changing | Batch may hit wrong or missing positions |

**Allowed for mapping batches:** AL **PREOP (2)** or **SAFEOP (4)**, online,
identity valid, short stable hold.

Non-map-only recipes (e.g. profile accel `0x6083` after map is already
correct) may still work in OP on some drives; prefer one policy for all
ordered “setup” recipes if the batch includes map objects: **PREOP/SAFEOP
only**.

### 3.3 Setup batch rules (UAPI)

- One **new** `setup_begin` → `add_sdo`… → `setup_apply` per attempt.
- After a failed apply, the batch is **not** retryable as-is: earlier writes
  may have landed; always **reset** and rebuild the full recipe (or a known
  safe repair sequence).
- Sequences strictly increasing; stop at first failure; log
  `failed_sequence`, position, index, abort code.
- Cap concurrent / rate-limit applies (e.g. ≥500 ms between batches) so a
  flapping chain does not storm the master FSM.
- Setup is **blocking mailbox** work: run off the hard real-time cyclic path
  (worker thread or low-priority loop). Never call setup from the kernel cycle
  thread or a SCHED_FIFO hard-RT loop.

#### While cyclic is active

**Current module behaviour (field-driven):** ordered setup SDO ioctls and
`SDO_UPLOAD` / slave discovery are **allowed while the cyclic task is
active**. They still block in the caller for mailbox completion; the kernel
does **not** schedule them on the cyclic RT task.

Older module builds returned **`EBUSY` (`-16`)** for `SETUP_*` while
`cycle_activate` was in force. That forced clients to tear down cycling (or
restart the owner) only to re-run a recipe — too heavy for servo power-return.
If you still see `setup_begin` → `EBUSY` with no other owner, upgrade the
module.

**Still EBUSY while active:** declarative `CONFIG_*` change and
`CYCLE_ACTIVATE` (already active). Rebuild topology → deactivate first.

**Client must still:** debounce, mailbox-ready gate, rebuild batch, retry.
The kernel only **executes** the batch you submit; it does not invent recipes
or retries.

### 3.4 What the kernel already does for you

- Rescan when slave count changes; config objects can reattach by address.
- Domain WC / incomplete and per-slave online/operational in cycle status.
- Output arm/disarm and optional **output lease** hang failsafe.
- **Does not** re-run your ad-hoc setup recipe after power loss.

### 3.5 What you must still own

- Which positions need which recipe (domain map, position list, vendor/product
  filter — **no brand hardcoding required in the transport**).
- Debounce, backoff, max attempts, operator-visible status
  (`idle` / `pending` / `waiting_preop` / `retrying` / `applied` / `failed`).
- Multi-domain readiness (e.g. IO domain OP while drive domain still recovering).
- Motion/enable gates until setup **and** OP/WC policy are satisfied.
- Not opening a second exclusive control owner (`elc_bus` / second `elc_open`)
  during recovery; that thrashing delays everyone.

---

## 4. Pre-start and exclusivity (related field lessons)

| Goal | Do | Avoid |
|------|----|--------|
| Wait for cable before becoming owner | `ethercat master` → Main **Link: UP** (non-owner) | `elc_open` / `elc_bus` only to poll link |
| One production controller | Single open of `/dev/elc_ethercat0` for the session | Shell recipe + `elc_bus` + controller all opening |
| Netdev carrier as EC link | Usually wrong: main_devices MAC is not a normal `enp*` UP port | `ip link` on management NICs as EC proof |

See [operator guide — main link without control ownership](operator-guide.md#main-link-without-control-ownership).

---

## 5. Suggested readiness reporting (generic)

Expose something equivalent for HMI/logs (names free):

```text
slave[pos].online
slave[pos].al_state
slave[pos].identity_ok          # vendor/product nonzero and match expected
slave[pos].setup_state          # idle | pending | waiting_preop | ready_hold | applying | applied | retrying | failed
slave[pos].setup_attempts
slave[pos].al_state             # especially PREOP vs OP while pending
domain[id].slaves_op / expected
domain[id].wc_ok
bus_healthy                     # client policy over the above
```

A single end-of-chain drive in INIT or OP-with-pending-map should not be
reported as “recipe engine broken” if others applied; it should read as
**that position still recovering** (`waiting_preop` if AL=OP before map done).

---

## 6. Drive EEPROM / default map note

Many servo drives **retain CoE PDO mapping across master restart** but **lose
it on control-power loss**. Warm controller restart is therefore a weak proof
of recovery logic; cold power on the drive is required to validate client
re-apply. Last-on-chain devices need the longest wait and the best cabling.

---

## 7. Checklist for a new client

- [ ] Cold-start setup (before or during configure) for all expected positions.
- [ ] Detect return (online edge and/or AL → PREOP/SAFEOP after prior life).
- [ ] Gate mapping CoE on **PREOP/SAFEOP only** (not OP); short PREOP hold.
- [ ] `waiting_preop` while OP with work still queued (no false fail spam).
- [ ] Full recipe rebuild per attempt; exponential backoff; bounded give-up.
- [ ] No second exclusive open for “status.”
- [ ] Domain/arm policy independent of brand strings.
- [ ] Log first failure detail and final success per position.
- [ ] Motion blocked until setup applied **and** domain OP/WC policy satisfied.

---

## 8. Example timing (illustrative, not ABI)

| Constant | Typical starting point |
|----------|------------------------|
| Ready hold in **PREOP/SAFEOP** only | **300–500 ms** (catch window; do not wait multi-second into OP) |
| Min gap between setup applies | ≥ 500 ms |
| Backoff (real apply failures only) | 1 s → 2 s → 4 s → … cap 8 s |
| Max attempts / max age | tens of attempts / a few minutes |
| Status throttle for `waiting_preop` | ~1–2 s so HMI is not flooded |

Tune per device family; keep values in the **client**, not the kernel.

---

## 9. Kernel / elc requirement: hold PREOP–SAFEOP until setup complete

### 9.1 Problem

While `CYCLE_ACTIVATE` is in force, EtherLab/elc drives attached configs
toward **OP**. User space can run `SETUP_APPLY` (blocking mailbox) **while
cyclic is active**, but it **cannot** today:

- request a configured slave to remain in PREOP (or SAFEOP) until setup finishes;
- inhibit OP promotion for a subset of positions (e.g. domain 2 servos only);
- learn from the module that “setup hold is active for config_id X”.

Without that, clients only **race** the PREOP/SAFEOP window after power return.
Miss the window → map wrong or apply fails → operator power-cycle or long
recovery.

### 9.2 Desired behaviour (requirement for elc)

**Goal:** After a slave reattaches (or on client request), the module (via
EtherLab slave config state machine) must **not** request OP for that
configuration until user space has completed ordered setup CoE for it—or
explicitly released the hold.

Suggested semantics (API names illustrative; final ABI TBD in `uapi.md`):

```text
Client (return online or recommission):
  1. ELC_IOC_SETUP_HOLD_BEGIN  { positions[] | domain_config_id | all }
     → kernel/master: target AL PREOP (or keep SAFEOP), do not promote to OP
  2. Client: debounce identity, SETUP_BEGIN / ADD_SDO / APPLY (as today)
  3. ELC_IOC_SETUP_HOLD_RELEASE { same scope }
     → master may proceed SAFEOP → OP under normal cyclic policy
```

Minimum viable capabilities:

| Capability | Why |
|------------|-----|
| **Per-position or per-domain setup hold** | Servos (domain 2) without blocking primary IO OP |
| **Hold target AL PREOP or SAFEOP** (not OP) | Mapping CoE requires PREOP/SAFEOP |
| **Status bit** “setup_hold active / released” | Client + HMI / domain ready gates |
| **Timeout / force-release** | Avoid permanent hold if client dies |
| **Works while cycle active** | Same reason setup is allowed while active |

Optional:

- Auto-hold on reattach for configs that have non-empty client-owned setup
  flag (if ever encoded in config) — still prefer **client-initiated** hold
  so the kernel stays recipe-agnostic.
- Combine with declarative `ecrt_slave_config_pdos()` path when the drive
  allows EtherLab-owned PDO replay (§ `recommended-master-lifecycle.md`).

### 9.3 What must **not** go into the kernel

- Brand-specific SDO tables / ED3L recipe content.
- Fixed “wait 750 ms” plant timers.
- Motion enable policy.

Recipes, debounce, and backoff stay in the **client** (`client-slave-recovery`
policy). The module only supplies **state hold + batch execution**.

### 9.4 Status (implementation)

| Layer | Status |
|-------|--------|
| Client PREOP/SAFEOP gate + short hold + `waiting_preop` | **Required now** (e.g. iod `ElcSetupRecipe`) |
| elc **setup hold** UAPI / master behaviour | **API 0.19** — `ELC_IOC_SETUP_HOLD_{BEGIN,RELEASE,STATUS}`, capability `ELC_CAP_SETUP_HOLD`, per-slave `setup_hold_active`, wall-time timeout and control-fd release |
| Full declarative PDO-only recovery (no ad-hoc map CoE) | Preferred long-term if drive allows |

Hardware exercise: `tools/elc_test_setup_hold.sh` (hold/release, timeout,
client death) with `ELC_MOTION_INHIBITED=YES`.

**Layout regression:** setup-hold uses private EtherLab `requested_state`
offsets (`kernel/elc_etherlab_layout.h`). Run `make test-etherlab-layout` on
every EtherLab upgrade or module rebuild against DKMS sources so offsetof
drift fails the build instead of silently mis-writing AL state.
