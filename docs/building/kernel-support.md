# Kernel support policy

## Goal

If the **EtherLab master** builds and runs on a given Linux kernel, this
transport should eventually build and run there too. Kernel differences are
handled in one place so new interfaces and features do not scatter
`LINUX_VERSION_CODE` checks through the cyclic path.

## Floors and targets

| Level | Kernel | Meaning |
|-------|--------|---------|
| **Compile floor** | **Linux ≥ 4.19** | Enforced in `kernel/elc_kcompat.h`. Common field RT / EtherLab baseline. |
| **Primary acceptance** | **Debian RT 6.1** (recorded host) | Full lifecycle, domains, timing screens, docs evidence. |
| **Field older RT** | e.g. **4.19 RT** + source EtherLab | First-class *build* support via kcompat; hardware evidence grows as sites validate. |
| **Newer than 6.1** | whatever EtherLab supports | Prefer kcompat helpers when APIs move; re-smoke build. |

Lowering the floor below 4.19 requires an explicit kcompat change, a build
smoke on that kernel, and a doc update. Do not claim production timing or
safety acceptance on a kernel without evidence on that kernel.

## Where compatibility lives

```text
kernel/elc_kcompat.h     — version shims, includes, small helpers/macros
kernel/elc_ethercat_main.c — no LINUX_VERSION_CODE for kernel API drift
kernel/elc_ethercat_probe.c — keep minimal; share kcompat if it grows
```

Today kcompat covers:

- required headers that are not transitive on older trees (`slab`, `mm`, …);
- `elc_set_fifo_priority()` (`sched_setattr_nocheck` vs `sched_setscheduler_nocheck`);
- `ELC_DEFINE_COMPAT_IOCTL` / `ELC_FOP_COMPAT_IOCTL` (pre-5.0 `compat_ptr_ioctl`).

## Adding a new interface or feature

1. Implement against the **newest** kernel you actively accept (currently 6.1).
2. If an older supported kernel lacks the API, add a helper or macro in
   **`elc_kcompat.h`** (with a one-line version comment).
3. Call only the helper from main code — **no** new `#if LINUX_VERSION_CODE`
   in the cyclic or ioctl bodies unless the difference is feature-optional
   and documented as a capability (prefer UAPI capability bits for
   userspace-visible gaps).
4. Smoke-build at least:
   - primary 6.1 RT + EtherLab DKMS (or recorded paths);
   - one older RT with **source** EtherLab (`ETHERLAB_INCLUDE` /
     `ETHERLAB_SYMVERS`).
5. Userspace-visible behaviour that cannot be emulated on old kernels must
   either fail closed with a clear errno or be gated by capability discovery —
   never silently change safety semantics.

## Build recipes

**DKMS EtherLab (auto-detect):**

```sh
make check-build-env
make -j"$(nproc)"
```

**Source EtherLab (any kernel with headers + matching Module.symvers):**

```sh
make \
  ETHERLAB_INCLUDE=/path/to/etherlab/include \
  ETHERLAB_SYMVERS=/path/to/etherlab/Module.symvers \
  save-build-env
make -j"$(nproc)"
```

`ETHERLAB_INCLUDE` is the directory that **contains** `ecrt.h`. After
`save-build-env`, plain `make` reads gitignored `local.mk` (see
`local.mk.example`).

See also [`etherlab-dkms-environment.md`](etherlab-dkms-environment.md) and
[`elc-dkms.md`](elc-dkms.md).

## Validation expectations

| Kind | Expectation |
|------|-------------|
| Compile | Must pass on every kernel in the stated support set. |
| Module load / master acquire | Same lifecycle rules as on 6.1. |
| Full topology / timing / DC | Claimed only where hardware evidence exists. |
| New UAPI features | Additive minors; capability bits where optional. |

## Related files

- `kernel/elc_kcompat.h` — implementation of this policy
- `docs/building/etherlab-dkms-environment.md` — EtherLab path contract
- `README.md` — requirements and first build steps
