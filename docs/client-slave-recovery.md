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

For each **expected** slave that needs client-owned CoE setup after return:

```text
offline (or AL INIT / identity 0:0)
    |
    v
seen online  ──────────────────────────────► queue "needs_setup"
    |                                              |
    | wait until mailbox-ready:                    |
    |   online && AL in {PREOP, SAFEOP, OP}        |
    |   && identity != 0:0                         |
    | hold stable ~0.5–1 s (debounce flaps)        |
    v                                              |
apply ordered setup batch (new begin/add/apply) ◄──┘
    |
    +-- success --> clear needs_setup; optional non-PDO defaults
    |
    +-- failure --> backoff (1s, 2s, 4s … cap ~8s), keep needs_setup
                    reset debounce if AL drops to INIT / offline
                    give up only after bounded attempts / max age
```

### 3.1 Triggers (queue work; do not apply on the hot path)

- **Offline → online** after the slave was previously known online (return),
  not necessarily on first cold scan (cold start often uses pre-activate setup).
- **AL enters PREOP+** from INIT/0 after a return (online alone is not enough).
- Optional: operator “recommission” command.

### 3.2 Mailbox-ready gate (before `SETUP_APPLY`)

Do **not** apply setup CoE when any of these hold:

| Signal | Why |
|--------|-----|
| not online | No slave |
| AL is INIT (1) or 0 | Mailbox often refused (0x0016) |
| `vendor_id` and `product_code` both 0 | SII/scan incomplete (“ghost” position) |
| scan busy / topology still changing | Batch may hit wrong or missing positions |

Prefer PREOP (2) before large mapping batches when the device docs require
parameterization in PREOP; SAFEOP/OP may still accept some SDOs but mapping
changes often need PREOP.

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
  (worker thread or low-priority loop).

### 3.4 What the kernel already does for you

- Rescan when slave count changes; config objects can reattach by address.
- Domain WC / incomplete and per-slave online/operational in cycle status.
- Output arm/disarm and optional **output lease** hang failsafe.
- **Does not** re-run your ad-hoc setup recipe after power loss.

### 3.5 What you must still own

- Which positions need which recipe (domain map, position list, vendor/product
  filter — **no brand hardcoding required in the transport**).
- Debounce, backoff, max attempts, operator-visible status
  (`pending` / `retrying` / `applied` / `failed`).
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
slave[pos].setup_state          # idle | pending | ready_hold | applying | applied | retrying | failed
slave[pos].setup_attempts
domain[id].slaves_op / expected
domain[id].wc_ok
bus_healthy                     # client policy over the above
```

A single end-of-chain drive in INIT should not be reported as “recipe engine
broken” if others applied; it should read as **that position still recovering**.

---

## 6. Drive EEPROM / default map note

Many servo drives **retain CoE PDO mapping across master restart** but **lose
it on control-power loss**. Warm controller restart is therefore a weak proof
of recovery logic; cold power on the drive is required to validate client
re-apply. Last-on-chain devices need the longest wait and the best cabling.

---

## 7. Checklist for a new client

- [ ] Cold-start setup (before or during configure) for all expected positions.
- [ ] Detect return (online edge and/or AL PREOP+ after prior life).
- [ ] Gate on mailbox-ready + short stable hold.
- [ ] Full recipe rebuild per attempt; exponential backoff; bounded give-up.
- [ ] No second exclusive open for “status.”
- [ ] Domain/arm policy independent of brand strings.
- [ ] Log first failure detail and final success per position.

---

## 8. Example timing (illustrative, not ABI)

| Constant | Typical starting point |
|----------|------------------------|
| Ready hold after PREOP+ | 500–1000 ms |
| Min gap between setup applies | ≥ 500 ms |
| Backoff | 1 s → 2 s → 4 s → … cap 8 s |
| Max attempts / max age | tens of attempts / a few minutes |

Tune per device family; keep values in the **client**, not the kernel.
