#!/usr/bin/env python3

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from unittest import mock

import cw_ec_config_from_topology as converter


def device(position, sync_managers=None, configured_sync_managers=None):
    return {
        "position": position,
        "alias": 0,
        "vendor_id": 2,
        "product_code": 0x12340000 + position,
        "revision_number": 1,
        "name": f"device-{position}",
        "sync_managers": sync_managers or [],
        "configured_sync_managers": configured_sync_managers or [],
    }


def sync(entries, direction="Input", pdo_index="0x1a00 (6656)"):
    return {
        "index": "0x0003 (3)",
        "direction": direction,
        "pdos": [
            {
                "index": pdo_index,
                "entries": entries,
            }
        ],
    }


def entry(index, subindex, bit_length):
    return {
        "index": index,
        "subindex": subindex,
        "bit_length": bit_length,
    }


class ConverterTest(unittest.TestCase):
    def run_converter(self, topology, split_position=None):
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", delete=False
        ) as stream:
            json.dump(topology, stream)
            path = stream.name
        argv = ["cw_ec_config_from_topology.py"]
        if split_position is not None:
            argv.extend(["--split-position", str(split_position)])
        argv.append(path)
        output = io.StringIO()
        try:
            with mock.patch.object(sys, "argv", argv):
                with contextlib.redirect_stdout(output):
                    self.assertEqual(converter.main(), 0)
        finally:
            os.unlink(path)
        return output.getvalue()

    def test_uses_slave_reported_mapping_and_ignores_requested_view(self):
        actual = sync([entry("0x6000 (24576)", 14, 1)])
        requested = sync([entry(0x1C32, 32, 1)])
        output = self.run_converter(
            [device(0, [actual], [requested])]
        )
        self.assertIn("0x6000 14 1", output)
        self.assertNotIn("0x1c32 32 1", output)

    def test_duplicate_objects_receive_distinct_occurrence_ids(self):
        duplicate = entry("0x6000 (24576)", 1, 1)
        output = self.run_converter(
            [device(0, [sync([duplicate, duplicate])])]
        )
        self.assertIn("0x01000001 0x6000 1 1", output)
        self.assertIn("0x01000002 0x6000 1 1", output)

    def test_padding_remains_unregistered(self):
        padding = entry("0x0000 (0)", 0, 7)
        output = self.run_converter([device(0, [sync([padding])])])
        self.assertIn("0x00000000 0x0000 0 7", output)

    def test_explicit_domain_split_assigns_every_slave(self):
        output = self.run_converter(
            [device(0), device(1)], split_position=1
        )
        self.assertIn("domain_slave 1001 1 1", output)
        self.assertIn("domain_slave 1002 2 2", output)

    def test_noncontiguous_positions_fail(self):
        with self.assertRaisesRegex(ValueError, "contiguous"):
            self.run_converter([device(1)])

    def test_malformed_numeric_field_fails(self):
        malformed = sync([entry("not-a-number", 1, 1)])
        with self.assertRaises(ValueError):
            self.run_converter([device(0, [malformed])])


if __name__ == "__main__":
    unittest.main()
