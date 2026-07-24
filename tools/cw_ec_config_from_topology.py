#!/usr/bin/env python3
"""Emit a cw_ec_config fixture from a captured generic topology JSON file."""

import argparse
import json
import sys


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("topology")
    parser.add_argument(
        "--split-position",
        type=int,
        help="put positions below this value in domain 1 and the rest in domain 2",
    )
    return parser.parse_args()


def number(value):
    if isinstance(value, int):
        return value
    return int(str(value).split()[0], 0)


def stable_entry_id(position, index, ordinal):
    if index == 0:
        return 0
    value = ((position + 1) << 24) | ordinal
    if value > 0xFFFFFFFF:
        raise ValueError("stable entry ID exceeds 32 bits")
    return value


def main():
    args = parse_args()
    with open(args.topology, encoding="utf-8") as stream:
        devices = json.load(stream)
    devices.sort(key=lambda device: device["position"])

    if not devices:
        raise ValueError("topology contains no devices")
    positions = [device["position"] for device in devices]
    if positions != list(range(len(devices))):
        raise ValueError("topology positions must be contiguous from zero")

    print("# Generated from captured topology; revision 0 is the required wildcard.")
    print("# Review physical identity, domains, PDO ownership, and output safety before use.")
    print("# This registers every captured output; a pulse update mask must select only")
    print("# the approved bit and holds all other registered output bits at zero.")
    print()
    if args.split_position is not None:
        print("domain 1")
        print("domain 2")
        for device in devices:
            config_id = device["position"] + 1
            domain_id = 1 if device["position"] < args.split_position else 2
            print(f"domain_slave {1000 + config_id} {config_id} {domain_id}")
        print()

    for device in devices:
        position = device["position"]
        config_id = position + 1
        name = str(device.get("name", "")).replace("\n", " ")
        print(f"# position {position}: {name}")
        print(
            f"slave {config_id} {device.get('alias', 0)} {position} "
            f"0x{device['vendor_id']:08x} 0x{device['product_code']:08x} 0"
        )
    print()

    next_sync = 10000
    next_pdo = 20000
    next_entry = 30000
    seen_entry_ids = set()
    for device_number, device in enumerate(devices):
        slave_id = device["position"] + 1
        entry_ordinal = 0
        for sync in device.get("sync_managers", []):
            sync_id = next_sync
            next_sync += 1
            direction = str(sync["direction"]).lower()
            watchdog = "enable" if direction == "output" else "disable"
            print(
                f"sync {sync_id} {slave_id} {number(sync['index'])} "
                f"{direction} {watchdog}"
            )
            for pdo in sync.get("pdos", []):
                pdo_id = next_pdo
                next_pdo += 1
                pdo_index = number(pdo["index"])
                print(f"pdo {pdo_id} {sync_id} 0x{pdo_index:04x}")
                for entry in pdo.get("entries", []):
                    index = number(entry["index"])
                    subindex = number(entry["subindex"])
                    if index:
                        entry_ordinal += 1
                    entry_id = stable_entry_id(
                        device["position"], index, entry_ordinal
                    )
                    if entry_id and entry_id in seen_entry_ids:
                        raise ValueError(
                            f"duplicate stable entry ID 0x{entry_id:08x}"
                        )
                    seen_entry_ids.add(entry_id)
                    print(
                        f"entry {next_entry} {pdo_id} 0x{entry_id:08x} "
                        f"0x{index:04x} {subindex} "
                        f"{number(entry['bit_length'])}"
                    )
                    next_entry += 1
        if device_number + 1 < len(devices):
            print()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
