#!/usr/bin/env python3

import argparse
import pathlib
import re
import struct


FLASH_SIZE = 16 * 1024 * 1024
SECTOR_SIZE = 0x1000
TABLE_OFFSET = 0x8000
TABLE_SIZE = 0x1000
NVS_OFFSET = 0x9000
NVS_SIZE = 0x6000
CANONICAL_FACTORY_OFFSET = 0x10000
CANONICAL_FACTORY_SIZE = 0x400000
CANONICAL_OTADATA_OFFSET = 0xF10000
CANONICAL_OTADATA_SIZE = 0x2000
LEGACY_OTA_OFFSETS = (0x320000, 0x620000)
LEGACY_APP_SIZE = 0x300000


def embedded_table(path):
    text = pathlib.Path(path).read_text()
    return bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", text))


def table_entries(table):
    entries = []
    for offset in range(0, 0xC00, 32):
        magic, part_type, subtype, address, size = struct.unpack_from(
            "<HBBII", table, offset
        )
        if magic == 0xFFFF:
            break
        if magic == 0xEBEB:
            continue
        if magic != 0x50AA:
            raise ValueError(f"bad partition magic 0x{magic:04x} at {offset}")
        label = table[offset + 12 : offset + 28].split(b"\0", 1)[0].decode()
        entries.append((label, part_type, subtype, address, size))
    return entries


def ranges_overlap(first_start, first_size, second_start, second_size):
    return (
        first_start < second_start + second_size
        and second_start < first_start + first_size
    )


def simulate(app, legacy_table, canonical_table, source_offset):
    assert len(app) <= LEGACY_APP_SIZE
    assert len(app) <= CANONICAL_FACTORY_SIZE
    assert not ranges_overlap(
        source_offset, len(app), CANONICAL_FACTORY_OFFSET, len(app)
    )

    flash = bytearray(b"\xff") * FLASH_SIZE
    nvs_sentinel = bytes((index * 37 + 11) & 0xFF for index in range(NVS_SIZE))
    flash[NVS_OFFSET : NVS_OFFSET + NVS_SIZE] = nvs_sentinel
    flash[TABLE_OFFSET : TABLE_OFFSET + len(legacy_table)] = legacy_table
    flash[source_offset : source_offset + len(app)] = app

    erase_size = (len(app) + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1)
    flash[
        CANONICAL_FACTORY_OFFSET : CANONICAL_FACTORY_OFFSET + erase_size
    ] = b"\xff" * erase_size
    flash[
        CANONICAL_FACTORY_OFFSET : CANONICAL_FACTORY_OFFSET + len(app)
    ] = app
    flash[
        CANONICAL_OTADATA_OFFSET : CANONICAL_OTADATA_OFFSET
        + CANONICAL_OTADATA_SIZE
    ] = b"\xff" * CANONICAL_OTADATA_SIZE

    assert flash[TABLE_OFFSET : TABLE_OFFSET + len(legacy_table)] == legacy_table
    assert flash[source_offset : source_offset + len(app)] == app
    assert flash[NVS_OFFSET : NVS_OFFSET + NVS_SIZE] == nvs_sentinel

    table_sector = canonical_table + b"\xff" * (TABLE_SIZE - len(canonical_table))
    flash[TABLE_OFFSET : TABLE_OFFSET + TABLE_SIZE] = table_sector

    assert flash[TABLE_OFFSET : TABLE_OFFSET + TABLE_SIZE] == table_sector
    assert (
        flash[
            CANONICAL_FACTORY_OFFSET : CANONICAL_FACTORY_OFFSET + len(app)
        ]
        == app
    )
    assert flash[NVS_OFFSET : NVS_OFFSET + NVS_SIZE] == nvs_sentinel


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", required=True)
    parser.add_argument("--legacy-table", required=True)
    parser.add_argument("--canonical-header", required=True)
    args = parser.parse_args()

    app = pathlib.Path(args.app).read_bytes()
    legacy_table = pathlib.Path(args.legacy_table).read_bytes()
    canonical_table = embedded_table(args.canonical_header)
    expected_legacy = {
        ("nvs", 0x01, 0x02, 0x9000, 0x6000),
        ("otadata", 0x01, 0x00, 0xF000, 0x2000),
        ("phy_init", 0x01, 0x01, 0x11000, 0x1000),
        ("factory", 0x00, 0x00, 0x20000, 0x300000),
        ("ota_0", 0x00, 0x10, 0x320000, 0x300000),
        ("ota_1", 0x00, 0x11, 0x620000, 0x300000),
    }
    actual_legacy = set(table_entries(legacy_table))
    if actual_legacy != expected_legacy:
        raise SystemExit(f"legacy partition mismatch: {sorted(actual_legacy)}")

    expected = {
        ("nvs", 0x01, 0x02, 0x9000, 0x6000),
        ("phy_init", 0x01, 0x01, 0xF000, 0x1000),
        ("factory", 0x00, 0x00, 0x10000, 0x400000),
        ("www", 0x01, 0x82, 0x410000, 0x300000),
        ("ota_0", 0x00, 0x10, 0x710000, 0x400000),
        ("ota_1", 0x00, 0x11, 0xB10000, 0x400000),
        ("otadata", 0x01, 0x00, 0xF10000, 0x2000),
        ("coredump", 0x01, 0x03, 0xF12000, 0x10000),
    }
    actual = set(table_entries(canonical_table + b"\xff" * TABLE_SIZE))
    if actual != expected:
        raise SystemExit(f"canonical partition mismatch: {sorted(actual)}")

    for source_offset in LEGACY_OTA_OFFSETS:
        simulate(app, legacy_table, canonical_table, source_offset)
    print(
        f"migration simulation passed: app={len(app)} bytes, "
        "sources=ota_0,ota_1, NVS preserved"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
