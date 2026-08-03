# DKMS packaging for elc_ethercat

This repository can install the kernel modules through [DKMS](https://github.com/dell/dkms)
so they rebuild when the host kernel is updated.

Userspace tools and `libelcethercat` are **not** part of the DKMS package;
build those with `make lib tools` / `make install-lib` as before.

## Prerequisites

1. **DKMS** installed (`dkms` package).
2. **Kernel headers/build tree** for the target kernel
   (`/lib/modules/$(uname -r)/build`).
3. **EtherLab master** already built for that same kernel, with a usable
   `ecrt.h` and matching `Module.symvers` that exports `ecrt_request_master`.

On the recorded target that is EtherLab DKMS `ethercat-dkms` 1.6.9, which
keeps symbols via its `POST_BUILD=save_module_symvers` hook:

```text
/usr/src/ethercat-dkms-*/include/ecrt.h
/var/lib/dkms/ethercat-dkms/<ver>/<kernel>/<arch>/module/Module.symvers
```

`make modules` (and therefore DKMS) fails closed if those paths are missing,
lack `ecrt_request_master`, or multiple EtherLab DKMS source trees make
auto-detection ambiguous.

**Remember paths on the machine** (preferred for source EtherLab):

```sh
make \
  ETHERLAB_INCLUDE=/path/to/include \
  ETHERLAB_SYMVERS=/path/to/Module.symvers \
  save-build-env
sudo make dkms-install
```

That writes gitignored `local.mk` (see `local.mk.example`). Later
`make dkms-install` needs no path arguments. One-shot without saving:

```sh
export ETHERLAB_INCLUDE=/path/to/include
export ETHERLAB_SYMVERS=/path/to/Module.symvers
# optional: ETHERLAB_VERSION=1.6.9
sudo -E make dkms-install
```

Install **EtherLab for the target kernel first** (DKMS or source). This package
does not embed EtherLab; it only consumes its headers and symbols.

## Install from a source checkout

```sh
make check-build-env    # optional: verify EtherLab/kernel paths
sudo make dkms-install
dkms status
modinfo elc_ethercat
```

### One-shot reinstall + reload

After module source changes, rebuild, force-install, drop older DKMS package
versions, and `modprobe` the new `.ko`:

```sh
sudo ./scripts/elc-dkms-reinstall.sh
```

Options: `--no-reload` (install only), `--keep-old` (leave prior package
versions registered), `--check-only` (env + status). Same EtherLab path
overrides as `make` (`ETHERLAB_*`, `local.mk`). Close control fds before
reload if `/dev/elc_ethercat0` is open.

`make dkms-install`:

1. writes `dkms.conf` from `dkms.conf.in` (version = library/UAPI `0.x.0`);
2. stages a minimal source tree under `/usr/src/elc-ethercat-<version>/`
   (`Makefile`, `dkms.conf`, `kernel/` including `elc_kcompat.h` and
   `etherlab_layout_stub/`, `include/`);
3. runs `dkms add` (if needed) and `dkms install -k $(uname -r)`.

Modules are registered as package **`elc-ethercat`**. Built objects:

- `elc_ethercat.ko` — full transport
- `elc_ethercat_probe.ko` — minimal master acquire/release probe

## Remove

```sh
sudo make dkms-uninstall
# or: sudo dkms remove elc-ethercat/<version> --all
```

Unload modules first if they are in use (`rmmod elc_ethercat` after closing
control fds). Do not force-unload a live controller session.

## Manual DKMS commands

```sh
make dkms.conf
sudo make dkms-stage          # refresh /usr/src/elc-ethercat-<version>
sudo dkms add -m elc-ethercat -v <version>
sudo dkms build -m elc-ethercat -v <version> -k $(uname -r)
sudo dkms install -m elc-ethercat -v <version> -k $(uname -r)
sudo dkms status
```

## Relation to `make install`

| Path | What it does |
|------|----------------|
| `sudo make install` | Copies `.ko` to `extra/elc_ethercat` for the **current** build only; also installs the userspace library. |
| `sudo make dkms-install` | Registers sources with DKMS so modules **rebuild on kernel upgrades**; does not install `libelcethercat` or tools. |

Prefer **one** install path for the kernel modules. Mixing a hand-copied
`extra/elc_ethercat` tree with DKMS for the same modules can confuse `modprobe`
if two copies exist; remove one before using the other.

## Versioning

DKMS package version tracks `LIB_VERSION` in the top-level Makefile
(currently `0.19.0`, aligned with UAPI major.minor). Bumping
`LIB_VERSION_MINOR` and regenerating `dkms.conf` is required for a new DKMS
package version; remove the old DKMS package before installing a different
version if both would claim the same module names.

## Open limits

- Proven pattern targets Debian RT + `ethercat-dkms` on the same machine that
  builds the module; other distros need the same header/symbol contract.
- No Debian `.deb` wrapper is provided yet—only DKMS source registration.
- Autoinstall rebuilds when a new kernel is installed **if** EtherLab symbols
  for that kernel already exist; otherwise the elc DKMS build fails until
  EtherLab is built for the new kernel first.
