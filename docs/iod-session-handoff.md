# IOD / Latproc session handoff

Use this when starting a new Grok (or other agent) session in
`/opt/latproc` or `/opt/latproc/iod`. This file lives in the **transport**
repo so it stays versioned with the API.

No Markdown tables below. Copy the fenced blocks as-is.

## How to point at the transport

You do not need the transport as the working directory.

- Working directory: `/opt/latproc/iod` or `/opt/latproc`
- Transport source: `/opt/etherlab-cyclic-kmod` (absolute path)

Either paste the starter prompt below, or put a short `TRANSPORT.md` in
the IOD tree that only points at that path and say: read TRANSPORT.md.

## Starter prompt (copy everything inside the fence)

```text
Work only in this Latproc/IOD tree. Do not put Clockwork policy into the transport repo.

Transport source (read-only dependency):
  /opt/etherlab-cyclic-kmod

Read first:
  /opt/etherlab-cyclic-kmod/docs/libelcethercat.md
  /opt/etherlab-cyclic-kmod/docs/uapi.md
  /opt/etherlab-cyclic-kmod/docs/developer-guide.md
  /opt/etherlab-cyclic-kmod/docs/iod-session-handoff.md

Transport names:
  module=elc_ethercat.ko
  device=/dev/elc_ethercat0
  library=libelcethercat
  headers=elc_ethercat.h and elc_ethercat_uapi.h
  api=0.16

Install library for IOD linking:
  cd /opt/etherlab-cyclic-kmod && make lib && sudo make install-lib PREFIX=/opt/elc

First IOD task: Phase 8 only
  - CMake option default OFF for kernel transport
  - Link libelcethercat
  - Discovery-only adapter (elc_open, list slaves, elc_close)
  - Keep legacy ecrt cyclic path as default
  - Never open /dev/elc_ethercat0 and ecrt_request_master at the same time

IOD EtherCAT files:
  src/ECInterface.cpp
  src/ECInterface.h
  src/iod.cpp (setupEtherCatThread)
  src/ecat_thread.cpp
  src/process_data.cpp
  src/process_data.h
  src/EtherCATSetup.cpp
  src/EtherCATEntrySelector.cpp
  src/EtherCATEntrySelector.h
```

## Optional TRANSPORT.md for the IOD tree (copy into IOD)

Create `/opt/latproc/iod/TRANSPORT.md` with:

```text
EtherCAT transport source: /opt/etherlab-cyclic-kmod
Installed prefix (headers + libelcethercat): /opt/elc

Docs:
  /opt/etherlab-cyclic-kmod/docs/libelcethercat.md
  /opt/etherlab-cyclic-kmod/docs/uapi.md
  /opt/etherlab-cyclic-kmod/docs/developer-guide.md
  /opt/etherlab-cyclic-kmod/docs/iod-session-handoff.md

module=elc_ethercat
device=/dev/elc_ethercat0
library=libelcethercat
```

Then start Grok in IOD with:

```text
Read TRANSPORT.md for the external EtherCAT transport path.
Work only in this IOD tree. Phase 8 discovery only; keep legacy ecrt cyclic default.
```

## Install and quick transport smoke (optional)

```sh
cd /opt/etherlab-cyclic-kmod
make lib
sudo make install-lib PREFIX=/opt/elc
sudo tools/elc_test_bus.sh
```

## Phase order (do not skip)

```text
Phase 7  done in transport: libelcethercat
Phase 8  IOD discovery only, opt-in, non-default
Phase 9  XML/ECModule config via kernel; fill offsets from entry IDs
Phase 10 setup SDOs; keep sdo.sh fallback
Phase 11 cyclic path: wait, snapshot, publish, arm
```

## Hard rules

```text
- One master owner: elc device open OR ecrt_request_master(0), never both
- Transport stays policy-free (no CiA 402, MACHINE, or ESI parsing in lib/kernel)
- Publish does not arm; arm is explicit; close fd gates outputs
- Prefer object selectors; no silent fallback from object to flat pos
- Motion inhibited for first cyclic tests
- Kernel path stays opt-in until an explicit production decision
```
