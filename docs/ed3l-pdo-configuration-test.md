# ED3L PDO Configuration Decision Gate

## Status

In progress. No PDO mapping writes have been issued through the new module.

## Target environment

- EtherLab: `1.6.9` (`1.6.9.gb709e58-1+28.2`)
- Kernel: `6.1.0-49-rt-amd64`, PREEMPT_RT
- ED3L count: 5
- Positions with control power on: 29–33
- Vendor: `0x0000060a`
- Product: `0xed310001`
- Revision: `0x00000001`
- Serial: `0`
- Alias: `0`
- Firmware version: not yet identified

## 2026-07-24 post-power-up observation

The drives were powered while IOD was already running. They appeared and
reached OP, increasing the topology from 29 to 34 slaves. IOD was later stopped
before the following read-only capture.

All five drives had identical object values:

```text
0x1600:00 = 2
0x1600:01 = 0x60400010  Control word, 16 bit
0x1600:02 = 0x607A0020  Target position, 32 bit
0x1600:03..06 = abort 0x06090011 (subindex does not exist)

0x1A00:00 = 2
0x1A00:01 = 0x60410010  Status word, 16 bit
0x1A00:02 = 0x60640020  Actual position, 32 bit
0x1A00:03..04 = abort 0x06090011 (subindex does not exist)

0x1C12:00 = 1
0x1C12:01 = 0x1600
0x1C13:00 = 1
0x1C13:01 = 0x1A00
0x6060:00 = 3
```

`ethercat pdos -p 29` agreed:

```text
SM2 / RxPDO 0x1600:
  0x6040:00 / 16
  0x607A:00 / 32

SM3 / TxPDO 0x1A00:
  0x6041:00 / 16
  0x6064:00 / 32
```

This is not the velocity mapping described as the known-good target in
`Implementation_Plan.md`. The planned target is:

```text
Rx: 6040/16, 60FF/32, 6083/32, 6084/32, 60E0/16, 60E1/16
Tx: 6041/16, 606C/32, 6077/16, 603F/16
```

The installed startup path confirms the cause:

```text
/opt/latproc/code/machine/Grab/scripts/services/iod.sh
    scans currently visible "Summa ED3L ServoDrive" positions
    runs scripts/tools/sdo.sh only for those positions
    starts iod_sdo
```

`sdo.sh` contains the exact ordered velocity mapping recipe from
`Implementation_Plan.md`. Drives powered after the startup scan do not receive
that recipe. They can appear and reach OP with the two-entry position mapping
recorded above. This directly demonstrates why restoration must use persistent
EtherLab configuration rather than a one-time pre-IOD script.

## 2026-08 plant note (iod-elc + ordered setup reapply)

Plant path now uses iod `ECSETUPRECIPE` / `ElcSetupRecipe` (same ordered CoE
as the velocity PDO recipe) at configure time and on offline→online. Field
result:

- Map rewrite in **OP** fails or is rejected; status flapped
  `pending` / `retrying` if the client treated OP as “mailbox ready.”
- Correct client policy: apply map batches only in **PREOP or SAFEOP**;
  short hold; while OP keep `waiting_preop` without burning attempts.
- **API 0.19 setup-hold** can keep a configured slave in PREOP/SAFEOP while
  cyclic is active until the client releases (or timeout / fd close); see
  [`client-slave-recovery.md` §9](client-slave-recovery.md#9-kernel--elc-requirement-hold-preopsafeop-until-setup-complete)
  for the module requirement.

The known-good velocity baseline still needs to be captured after deliberately
running the legacy recipe with motion safely inhibited.

## Kernel upload validation

The new bounded `ELC_IOC_SDO_UPLOAD` path read position 29 object
`0x6060:00` as byte `03`. The standard EtherLab CLI returned `0x03` for the
same object. The module then released master 0 cleanly.

## Test A — legacy explicit SDO baseline

The installed legacy `sdo.sh -f` recipe was applied to all five drives with IOD
stopped and machine motion inhibited. The script produced abort `0x06040041`
when attempting to write zero into mapping entries after setting each mapping
count to zero. It intentionally continued, and every final object on every
drive matched the planned six-entry velocity RxPDO, four-entry velocity TxPDO,
SM assignments, and mode 3.

Those failed zero writes are unnecessary and unsuitable for a strict
fail-fast transaction.

## Strict kernel ad-hoc recipe

A cleaned 21-operation recipe was applied through the new kernel batch to
position 29:

1. disable both SM assignment counts;
2. set both mapping counts to zero;
3. write only the desired valid Rx/Tx entries;
4. restore mapping counts;
5. restore SM assignments and counts;
6. set operating mode 3.

All 21 blocking writes succeeded in sequence with no ignored abort. Readback
confirmed the velocity mapping, assignments, and mode. Master 0 was released
cleanly and the 34-slave link remained up.

This proves the generic ad-hoc batch and a corrected explicit recipe. It does
not establish the preferred persistent production mechanism or power-cycle
replay.

## Required remaining tests

1. Run the known-good legacy `sdo.sh`/IOD startup path and capture every object
   again.
2. Save that velocity mapping as Test A.
3. Test declarative `ecrt_slave_config_pdos()` for mapping and assignment.
4. Attach only non-PDO startup values, such as `0x6060:00`, through persistent
   configuration SDOs. EtherLab 1.6.9 explicitly says not to use
   `ecrt_slave_config_sdo()` for PDO assignment or mapping objects.
5. Compare actual readback, OP transition, working counter, repeated lifecycle,
   startup absent, power loss, and restoration without restart.
6. Verify a non-energizing recovery output image before any motion-capable
   operational test.
