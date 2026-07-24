# User-Space API

## Status

The current experimental API is version 0.1 and supports read-only master and
slave discovery. It is not stable and provides no configuration or cyclic I/O.

## Ownership and lifecycle

The module registers `/dev/cw_ethercat0` without claiming EtherLab master 0.
Opening the device read-write claims master 0 exclusively. A second open, or
an open while IOD/direct libethercat already owns the master, fails with
`EBUSY`.

Closing the only control file releases the master and all EtherLab application
state associated with this owner. The file operations hold a module reference,
so normal module unload cannot race an open file.

This initial API intentionally supports one controller and no diagnostic-only
opens. A future status interface must not bypass EtherLab's exclusive
application ownership rules.

## Versioning

The shared header is `include/cw_ethercat_uapi.h`. Structures use fixed-width
Linux UAPI types, contain no pointers, and have fixed layouts suitable for the
compat ioctl path.

The tool first calls `CW_EC_IOC_GET_API_VERSION`. Major versions must match.
Minor version 0.1 currently identifies the discovery prototype.

Input/output structures that accept caller fields include `struct_size` and
`api_major`. The kernel rejects an unexpected size with `EINVAL` and an
incompatible major version with `EPROTONOSUPPORT`.

## Operations

### `CW_EC_IOC_GET_API_VERSION`

Returns `struct cw_ec_api_version`.

### `CW_EC_IOC_GET_MASTER_INFO`

Returns `struct cw_ec_master_info` containing:

- scanned slave count;
- main link state;
- scan-in-progress state;
- EtherLab application time.

### `CW_EC_IOC_GET_SLAVE_INFO`

The caller initializes:

- `struct_size`;
- `api_major`;
- physical `position`.

The kernel calls the target EtherLab `ecrt_master_get_slave()` API and returns
the physical position, alias, vendor ID, product code, revision, serial number,
E-bus current, AL state, error flag, Sync Manager count, SDO count, and a
diagnostic name. Device matching must use identity fields, not the name.

An unknown position returns `ENOENT`.

## Compatibility and memory safety

All current ioctl structures contain only fixed-width values and inline
arrays. No kernel address or user pointer is retained. Each ioctl copies a
complete bounded structure with `copy_from_user()`/`copy_to_user()`.

Unknown ioctl types or commands return `ENOTTY`. The initial malformed-call
test covers a second control open, unknown command, short structure,
incompatible API major, and invalid slave position.

## Known limitations

- The public EtherLab `ecrt_request_master()` kernel API returns only `NULL` on
  failure, so open currently maps all claim failures to `EBUSY`.
- Master index is fixed at 0 for this prototype.
- Opening the device changes the EtherLab master from idle to application
  operation phase until close, although it does not activate a PDO domain.
- There is no wait-for-scan operation. `scan_busy` is reported to user space.
- Slave state is a scan snapshot. No topology generation is exposed yet.
